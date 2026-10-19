// risk-lsp

// I said it already in the README (well, AI did under my description) but
// people claim i don't comment so lemme make a try or at least get them annoyed
// with this comment. SO, The compiler front-end is never run in this process.
// We run every analysis in a forked, time or memory-capped child and it
// basically comes back as a JSON index of declarations + diagnostics. Editor
// features here are computed from that index plus the lexer's token stream.

#include "builtins.h"
#include "json.hpp"
#include "lexer.h"
#include "model.h"
#include "token.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <poll.h>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

static void log(const std::string &s) {
  fprintf(stderr, "[risk-lsp] %s\n", s.c_str());
}

static std::string path_from_uri(const std::string &uri) {
  if (uri.rfind("file://", 0) != 0)
    return uri;
  std::string out, in = uri.substr(7);
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '%' && i + 2 < in.size() + 0 &&
        isxdigit((unsigned char)in[i + 1]) &&
        isxdigit((unsigned char)in[i + 2])) {
      out.push_back((char)std::stoi(in.substr(i + 1, 2), nullptr, 16));
      i += 2;
    } else
      out.push_back(in[i]);
  }
  return out;
}

static std::string uri_from_path(const std::string &path) {
  if (path.empty() || path[0] != '/')
    return path;
  static const char *hex = "0123456789ABCDEF";
  std::string out = "file://";
  for (unsigned char ch : path) {
    if (isalnum(ch) || ch == '/' || ch == '-' || ch == '_' || ch == '.' ||
        ch == '~')
      out.push_back((char)ch);
    else {
      out.push_back('%');
      out.push_back(hex[ch >> 4]);
      out.push_back(hex[ch & 15]);
    }
  }
  return out;
}

static std::string dir_of(const std::string &path) {
  size_t s = path.find_last_of('/');
  return s == std::string::npos ? "" : path.substr(0, s + 1);
}

static bool file_exists(const std::string &p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
static bool dir_exists(const std::string &p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
static long file_mtime(const std::string &p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0 ? (long)st.st_mtime : -1;
}
static std::string read_file(const std::string &p) {
  FILE *f = fopen(p.c_str(), "rb");
  if (!f)
    return "";
  std::string s;
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0)
    s.append(buf, n);
  fclose(f);
  return s;
}

static std::vector<std::string> split_lines(const std::string &t) {
  std::vector<std::string> out;
  size_t start = 0;
  for (size_t i = 0; i <= t.size(); ++i) {
    if (i == t.size() || t[i] == '\n') {
      std::string ln = t.substr(start, i - start);
      if (!ln.empty() && ln.back() == '\r')
        ln.pop_back();
      out.push_back(std::move(ln));
      start = i + 1;
    }
  }
  return out;
}

static int col_to_lsp(const std::string &line, int col1) {
  int bytes = std::max(0, col1 - 1), u16 = 0;
  size_t i = 0;
  while (i < line.size() && (int)i < bytes) {
    unsigned char c = line[i];
    int len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    u16 += len == 4 ? 2 : 1;
    i += len;
  }
  if ((int)i < bytes)
    u16 += bytes - (int)i;
  return u16;
}
static int lsp_to_col(const std::string &line, int u16) {
  int units = 0;
  size_t i = 0;
  while (i < line.size() && units < u16) {
    unsigned char c = line[i];
    int len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    units += len == 4 ? 2 : 1;
    i += len;
  }
  if (units < u16)
    i += u16 - units;
  return (int)i + 1;
}

static bool pos_le(int l1, int c1, int l2, int c2) {
  return l1 < l2 || (l1 == l2 && c1 <= c2);
}

struct Ty {
  std::string n;
  std::vector<Ty> a;
  std::shared_ptr<Ty> r;
  bool valid() const { return !n.empty(); }
};

static Ty ty_from_json(const json &j) {
  Ty t;
  if (!j.is_object() || !j.contains("n"))
    return t;
  t.n = j["n"].get<std::string>();
  if (j.contains("a"))
    for (auto &x : j["a"])
      t.a.push_back(ty_from_json(x));
  if (j.contains("r"))
    t.r = std::make_shared<Ty>(ty_from_json(j["r"]));
  return t;
}

static std::string ty_str(const Ty &t) {
  if (t.n.rfind("__size__:", 0) == 0)
    return t.n.substr(9);
  std::string s = t.n;
  if (t.n == "func") {
    s += "(";
    for (size_t i = 0; i < t.a.size(); ++i)
      s += (i ? ", " : "") + ty_str(t.a[i]);
    s += ")";
    if (t.r)
      s += " -> " + ty_str(*t.r);
    return s;
  }
  if (!t.a.empty()) {
    s += "<";
    for (size_t i = 0; i < t.a.size(); ++i)
      s += (i ? ", " : "") + ty_str(t.a[i]);
    s += ">";
  }
  return s;
}

struct Doc {
  std::string uri, path, text;
  int version = 0;
  std::vector<std::string> lines;
  std::vector<Token> toks;
  std::shared_ptr<Analysis> an;
  bool dirty = true;
  Clock::time_point changed = Clock::now();

  void set_text(std::string t) {
    text = std::move(t);
    lines = split_lines(text);
    Lexer lx(text, path);
    toks = lx.tokenize();
    // keep EOF token out of the way of lookups
    if (!toks.empty() && toks.back().type == TokenType::EndOfFile)
      toks.pop_back();
    dirty = true;
    changed = Clock::now();
  }
  const std::string &line(int l1) const {
    static const std::string empty;
    return (l1 >= 1 && l1 <= (int)lines.size()) ? lines[l1 - 1] : empty;
  }
};

static int tok_end(const Doc &d, const Token &t) {
  if (t.type == TokenType::Number) {
    const std::string &ln = d.line(t.line);
    size_t i = (size_t)std::max(0, t.col - 1);
    while (i < ln.size() &&
           (isalnum((unsigned char)ln[i]) || ln[i] == '.' || ln[i] == '_'))
      ++i;
    return (int)i + 1;
  }
  return t.col + (int)t.lexeme.size();
}

static int token_at(const Doc &d, int line, int col) {
  auto it =
      std::upper_bound(d.toks.begin(), d.toks.end(), std::make_pair(line, col),
                       [](const std::pair<int, int> &p, const Token &t) {
                         return p < std::make_pair(t.line, t.col);
                       });
  if (it == d.toks.begin())
    return -1;
  int i = (int)(it - d.toks.begin()) - 1;
  const Token &t = d.toks[i];
  return (t.line == line && col >= t.col && col < tok_end(d, t)) ? i : -1;
}

static int token_before(const Doc &d, int line, int col) {
  auto it =
      std::lower_bound(d.toks.begin(), d.toks.end(), std::make_pair(line, col),
                       [](const Token &t, const std::pair<int, int> &p) {
                         return std::make_pair(t.line, t.col) < p;
                       });
  return (int)(it - d.toks.begin()) - 1;
}

static bool is_sym(const Doc &d, int i, const char *s) {
  return i >= 0 && i < (int)d.toks.size() &&
         d.toks[i].type == TokenType::Symbol && d.toks[i].lexeme == s;
}
static bool is_ident(const Doc &d, int i) {
  return i >= 0 && i < (int)d.toks.size() &&
         d.toks[i].type == TokenType::Identifier;
}
static bool is_ident(const Doc &d, int i, const char *s) {
  return is_ident(d, i) && d.toks[i].lexeme == s;
}

static int match_back(const Doc &d, int i, const char *open,
                      const char *close) {
  int depth = 0;
  for (int k = i; k >= 0; --k) {
    if (is_sym(d, k, close))
      depth++;
    else if (is_sym(d, k, open)) {
      if (--depth == 0)
        return k;
    }
  }
  return -1;
}

struct ModRef {
  std::string path;
  std::shared_ptr<Analysis> an;
};

struct CachedModule {
  long mtime = -1;
  std::shared_ptr<Analysis> an;
  std::vector<std::string> lines;
};

struct Server {
  std::map<std::string, Doc> docs;            // uri -> doc
  std::map<std::string, std::string> by_path; // path -> uri
  std::map<std::string, CachedModule> cache;  // closed files
  std::string root, stdlib;
  bool shutdown = false;

  void send(const json &j) {
    std::string body = j.dump();
    std::string msg =
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    const char *p = msg.data();
    size_t left = msg.size();
    while (left > 0) {
      ssize_t n = write(1, p, left);
      if (n < 0) {
        if (errno == EINTR)
          continue;
        return;
      }
      p += n;
      left -= (size_t)n;
    }
  }
  void reply(const json &id, json result) {
    send({{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
  }
  void reply_error(const json &id, int code, const std::string &m) {
    send({{"jsonrpc", "2.0"},
          {"id", id},
          {"error", {{"code", code}, {"message", m}}}});
  }
  void notify(const std::string &method, json params) {
    send({{"jsonrpc", "2.0"},
          {"method", method},
          {"params", std::move(params)}});
  }

  void analyze(Doc &d) {
    std::string err;
    auto a = run_analysis(d.path, d.text, /*typecheck=*/true, &err);
    if (a)
      d.an = a;
    else {
      log("analysis of " + d.path + " failed: " + err);
      if (!d.an)
        d.an = std::make_shared<Analysis>();
    }
    d.dirty = false;
    if (a)
      publish_diagnostics(d);
  }
  void fresh(Doc &d) {
    if (d.dirty || !d.an)
      analyze(d);
  }

  void publish_diagnostics(Doc &d) {
    json arr = json::array();
    int count = 0;
    for (auto &x : d.an->diags) {
      if (++count > 200)
        break;
      int line = std::max(1, x.l), col = std::max(1, x.c);
      const std::string &text = d.line(line);
      int endInclusive = std::max(col, x.e);
      int ti = x.l > 0 ? token_at(d, line, col) : -1;
      if (ti >= 0)
        endInclusive = std::max(endInclusive, tok_end(d, d.toks[ti]) - 1);
      int endCol = std::min(endInclusive + 1, (int)text.size() + 1);
      endCol = std::max(endCol, col);
      if (x.l <= 0) {
        line = 1;
        col = 1;
        endCol = std::max(2, (int)d.line(1).size() + 1);
      }
      json diag;
      diag["range"] = {
          {"start", {{"line", line - 1}, {"character", col_to_lsp(text, col)}}},
          {"end",
           {{"line", line - 1}, {"character", col_to_lsp(text, endCol)}}}};
      diag["severity"] = x.sev == 1 ? 1 : 2;
      diag["source"] = "riskc";
      diag["message"] = x.msg;
      arr.push_back(std::move(diag));
    }
    notify("textDocument/publishDiagnostics",
           {{"uri", d.uri}, {"version", d.version}, {"diagnostics", arr}});
  }

  std::vector<std::string> stdlib_bases(const std::string &from_dir) {
    std::vector<std::string> v;
    if (!stdlib.empty())
      v.push_back(stdlib + "/");
    const char *home = getenv("HOME");
    if (home)
      v.push_back(std::string(home) + "/.risk/lib/");
    if (!root.empty())
      v.push_back(root + "/stdlib/");
    v.push_back("./stdlib/");
    if (!from_dir.empty())
      v.push_back(from_dir + "stdlib/");
    return v;
  }

  std::string resolve_import(std::string p, const std::string &from_dir) {
    bool is_std = !p.empty() && p[0] == '@';
    if (is_std)
      p.erase(0, 1);
    std::vector<std::string> spell = {p};
    if (!(p.size() >= 5 && p.compare(p.size() - 5, 5, ".risk") == 0))
      spell.push_back(p + ".risk");
    if (is_std) {
      for (auto &base : stdlib_bases(from_dir))
        for (auto &sp : spell) {
          std::string stem = sp;
          if (stem.size() >= 5 &&
              stem.compare(stem.size() - 5, 5, ".risk") == 0)
            stem.resize(stem.size() - 5);
          if (file_exists(base + sp))
            return base + sp;
          if (file_exists(base + stem + "/" + sp))
            return base + stem + "/" + sp;
        }
      return "";
    }
    for (auto &sp : spell) {
      if (!from_dir.empty() && file_exists(from_dir + sp))
        return from_dir + sp;
      if (file_exists("./" + sp))
        return "./" + sp;
    }
    return "";
  }

  std::shared_ptr<Analysis> module_analysis(const std::string &path) {
    auto u = by_path.find(path);
    if (u != by_path.end()) {
      Doc &d = docs[u->second];
      if (!d.an)
        analyze(d);
      return d.an;
    }
    long mt = file_mtime(path);
    if (mt < 0)
      return nullptr;
    auto &c = cache[path];
    if (c.an && c.mtime == mt)
      return c.an;
    std::string text = read_file(path), err;
    auto a = run_analysis(path, text, /*typecheck=*/false, &err);
    if (!a) {
      log("module analysis of " + path + " failed: " + err);
      return nullptr;
    }
    c.mtime = mt;
    c.an = a;
    c.lines = split_lines(text);
    return a;
  }

  const std::vector<std::string> *lines_of(const std::string &path) {
    auto u = by_path.find(path);
    if (u != by_path.end())
      return &docs[u->second].lines;
    auto c = cache.find(path);
    if (c == cache.end()) {
      module_analysis(path);
      c = cache.find(path);
    }
    return c == cache.end() ? nullptr : &c->second.lines;
  }

  void collect(const std::string &path, std::set<std::string> &seen,
               std::vector<ModRef> &out, int depth) {
    if (depth > 8 || !seen.insert(path).second)
      return;
    auto an = module_analysis(path);
    if (!an)
      return;
    out.push_back({path, an});
    for (auto &im : an->imports) {
      std::string p = resolve_import(im.path, dir_of(path));
      if (!p.empty())
        collect(p, seen, out, depth + 1);
    }
  }

  std::vector<ModRef> closure(const Doc &d) {
    std::vector<ModRef> out;
    std::set<std::string> seen = {d.path};
    if (!d.an)
      return out;
    for (auto &im : d.an->imports) {
      std::string p = resolve_import(im.path, dir_of(d.path));
      if (!p.empty())
        collect(p, seen, out, 0);
    }
    return out;
  }

  std::vector<ModRef> closure_of_alias(const Doc &d, const std::string &alias) {
    std::vector<ModRef> out;
    std::set<std::string> seen = {d.path};
    if (!d.an)
      return out;
    for (auto &im : d.an->imports)
      if (!im.alias.empty() && im.alias == alias) {
        std::string p = resolve_import(im.path, dir_of(d.path));
        if (!p.empty())
          collect(p, seen, out, 0);
      }
    return out;
  }

  struct Found {
    const Sym *s = nullptr;
    std::string path;
    const Builtin *b = nullptr;
    Ty recv;
    std::string module_label; // for hover
    bool ok() const { return s || b; }
  };

  static bool visible_at(const Sym &s, int l, int c) {
    return s.scoped && pos_le(s.sl, s.sc, l, c) && pos_le(l, c, s.el, s.ec);
  }

  const Sym *find_scoped(const Doc &d, const std::string &name, int l, int c) {
    if (!d.an)
      return nullptr;
    auto it = d.an->by_name.find(name);
    if (it == d.an->by_name.end())
      return nullptr;
    const Sym *best = nullptr;
    for (size_t i : it->second) {
      const Sym &s = d.an->syms[i];
      if (!visible_at(s, l, c))
        continue;
      if (!best || pos_le(best->l, best->c, s.l, s.c))
        best = &s;
    }
    return best;
  }

  static const Sym *find_global_in(const Analysis &an,
                                   const std::string &name) {
    auto it = an.by_name.find(name);
    if (it == an.by_name.end())
      return nullptr;
    const Sym *first = nullptr;
    for (size_t i : it->second) {
      const Sym &s = an.syms[i];
      if (s.scoped || s.is_member())
        continue;
      if (!first)
        first = &s;
    }
    return first;
  }

  Found lookup_global(const Doc &d, const std::string &name) {
    Found f;
    if (d.an)
      if (const Sym *s = find_global_in(*d.an, name)) {
        f.s = s;
        f.path = d.path;
        return f;
      }
    for (auto &m : closure(d))
      if (const Sym *s = find_global_in(*m.an, name)) {
        f.s = s;
        f.path = m.path;
        return f;
      }
    return f;
  }

  Found lookup_visible(const Doc &d, const std::string &name, int l, int c) {
    Found f;
    if (const Sym *s = find_scoped(d, name, l, c)) {
      f.s = s;
      f.path = d.path;
      return f;
    }
    f = lookup_global(d, name);
    if (f.ok())
      return f;
    if (const Builtin *b = find_builtin_function(name))
      f.b = b;
    return f;
  }

  Ty resolve_alias(const Doc &d, Ty t, int depth = 0) {
    if (depth > 6 || !t.valid())
      return t;
    Found f = lookup_global(d, t.n);
    if (f.s && f.s->k == "alias" && f.s->ty.is_object())
      return resolve_alias(d, ty_from_json(f.s->ty), depth + 1);
    return t;
  }

  Ty deref(const Doc &d, Ty t) {
    for (int i = 0; i < 8; ++i) {
      t = resolve_alias(d, t);
      if ((t.n == "ptr" || t.n == "const" || t.n == "refc" || t.n == "opt") &&
          t.a.size() == 1)
        t = t.a[0];
      else
        break;
    }
    return t;
  }

  void collect_members(const Doc &d, const std::string &type,
                       const std::string &only,
                       std::vector<std::pair<const Sym *, std::string>> &out) {
    auto scan = [&](const Analysis &an, const std::string &path) {
      for (auto &s : an.syms)
        if (s.is_member() && s.p == type && (only.empty() || s.n == only))
          out.push_back({&s, path});
    };
    if (d.an)
      scan(*d.an, d.path);
    for (auto &m : closure(d))
      scan(*m.an, m.path);
  }

  Found member_lookup(const Doc &d, const Ty &recv, const std::string &name) {
    Found f;
    Ty t = deref(d, recv);
    if (!t.valid())
      return f;
    std::vector<std::pair<const Sym *, std::string>> v;
    collect_members(d, t.n, name, v);
    if (!v.empty()) {
      f.s = v[0].first;
      f.path = v[0].second;
      return f;
    }
    if (const Builtin *b = find_builtin_method(t.n, name)) {
      f.b = b;
      f.recv = t;
    }
    return f;
  }

  static std::string fill(const std::string &s, const Ty &recv) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
      if (s[i] == '$' && i + 1 < s.size()) {
        char k = s[i + 1];
        size_t idx = (k == 'V') ? 1 : 0;
        if (recv.a.size() > idx)
          out += ty_str(recv.a[idx]);
        else
          out += std::string(1, k);
        ++i;
      } else
        out.push_back(s[i]);
    }
    return out;
  }

  std::optional<Ty> expr_type(Doc &d, int i, int depth = 0) {
    if (i < 0 || i >= (int)d.toks.size() || depth > 12)
      return std::nullopt;
    const Token &t = d.toks[i];
    if (t.type == TokenType::String)
      return Ty{"string", {}, nullptr};
    if (t.type == TokenType::Identifier) {
      if (is_sym(d, i - 1, ".")) {
        auto base = expr_type(d, i - 2, depth + 1);
        if (!base)
          return std::nullopt;
        Found f = member_lookup(d, *base, t.lexeme);
        if (f.s && f.s->ty.is_object())
          return ty_from_json(f.s->ty);
        return std::nullopt;
      }
      Found f = lookup_visible(d, t.lexeme, t.line, t.col);
      if (f.s && f.s->k != "function" && f.s->ty.is_object())
        return ty_from_json(f.s->ty);
      return std::nullopt;
    }
    if (is_sym(d, i, ")")) {
      int open = match_back(d, i, "(", ")");
      if (open < 1)
        return std::nullopt;
      // cast<T>(x) / unsafe_cast<T>(x)
      if (is_sym(d, open - 1, ">")) {
        int lt = match_back(d, open - 1, "<", ">");
        if (lt > 0 &&
            (is_ident(d, lt - 1, "cast") || is_ident(d, lt - 1, "unsafe_cast")))
          return parse_ty_tokens(d, lt + 1);
        return std::nullopt;
      }
      int callee = open - 1;
      if (!is_ident(d, callee))
        return std::nullopt;
      const std::string &name = d.toks[callee].lexeme;
      if (is_sym(d, callee - 1, ".")) {
        auto base = expr_type(d, callee - 2, depth + 1);
        if (!base)
          return std::nullopt;
        Found f = member_lookup(d, *base, name);
        if (f.s && f.s->ty.is_object()) {
          Ty ft = ty_from_json(f.s->ty);
          if (ft.n == "func" && ft.r)
            return *ft.r;
          return std::nullopt;
        }
        if (f.b && !f.b->ret.empty()) {
          Ty r;
          std::string rs = fill(f.b->ret, f.recv);
          r.n = rs;
          return r;
        }
        return std::nullopt;
      }
      Found f =
          lookup_visible(d, name, d.toks[callee].line, d.toks[callee].col);
      if (f.s && f.s->k == "function" && f.s->ty.is_object())
        return ty_from_json(f.s->ty);
      if (f.s && f.s->k == "block")
        return Ty{f.s->n, {}, nullptr};
      if (f.b && !f.b->ret.empty())
        return Ty{f.b->ret, {}, nullptr};
      return std::nullopt;
    }
    if (is_sym(d, i, "]")) {
      int open = match_back(d, i, "[", "]");
      if (open < 1)
        return std::nullopt;
      auto base = expr_type(d, open - 1, depth + 1);
      if (!base)
        return std::nullopt;
      Ty b = deref(d, *base);
      if (b.n == "Array" && !b.a.empty())
        return b.a[0];
      if (b.n == "Hashmap" && b.a.size() > 1)
        return b.a[1];
      if (b.n == "string")
        return Ty{"char", {}, nullptr};
      return std::nullopt;
    }
    return std::nullopt;
  }

  std::optional<Ty> parse_ty_tokens(const Doc &d, int i) {
    int k = i;
    return parse_ty_rec(d, k, 0);
  }
  std::optional<Ty> parse_ty_rec(const Doc &d, int &k, int depth) {
    if (depth > 6 || !is_ident(d, k))
      return std::nullopt;
    Ty t;
    t.n = d.toks[k].lexeme;
    ++k;
    if (is_sym(d, k, "<")) {
      ++k;
      while (k < (int)d.toks.size() && !is_sym(d, k, ">") &&
             !is_sym(d, k, ">>")) {
        auto sub = parse_ty_rec(d, k, depth + 1);
        if (!sub) {
          if (d.toks[k].type == TokenType::Number)
            ++k;
          else
            break;
        } else
          t.a.push_back(*sub);
        if (is_sym(d, k, ","))
          ++k;
      }
      if (is_sym(d, k, ">") || is_sym(d, k, ">>"))
        ++k;
    }
    return t;
  }

  Found resolve_token(Doc &d, int i) {
    Found none;
    if (!is_ident(d, i) || !d.an)
      return none;
    const Token &t = d.toks[i];

    for (auto &s : d.an->syms)
      if (s.l == t.line && s.c == t.col && s.n == t.lexeme) {
        Found f;
        f.s = &s;
        f.path = d.path;
        return f;
      }

    if (is_sym(d, i - 1, ".") && is_ident(d, i - 2)) {
      const Token &q = d.toks[i - 2];
      bool shadowed = find_scoped(d, q.lexeme, q.line, q.col) != nullptr;
      if (!shadowed)
        for (auto &m : closure_of_alias(d, q.lexeme))
          if (const Sym *s = find_global_in(*m.an, t.lexeme)) {
            Found f;
            f.s = s;
            f.path = m.path;
            f.module_label = q.lexeme;
            return f;
          }
      Found base = lookup_visible(d, q.lexeme, q.line, q.col);
      if (base.s && base.s->k == "opts") {
        std::vector<std::pair<const Sym *, std::string>> v;
        collect_members(d, base.s->n, t.lexeme, v);
        if (!v.empty()) {
          Found f;
          f.s = v[0].first;
          f.path = v[0].second;
          return f;
        }
        return none;
      }
    }
    if (is_sym(d, i - 1, ".")) {
      auto base = expr_type(d, i - 2);
      if (!base)
        return none;
      return member_lookup(d, *base, t.lexeme);
    }
    return lookup_visible(d, t.lexeme, t.line, t.col);
  }

  json range_json(const Doc &d, int l1, int c1, int l2, int c2) {
    return {
        {"start",
         {{"line", l1 - 1}, {"character", col_to_lsp(d.line(l1), c1)}}},
        {"end", {{"line", l2 - 1}, {"character", col_to_lsp(d.line(l2), c2)}}}};
  }
  json range_in(const std::string &path, int l, int c, int len) {
    const std::vector<std::string> *ls = lines_of(path);
    static const std::string empty;
    const std::string &ln =
        (ls && l >= 1 && l <= (int)ls->size()) ? (*ls)[l - 1] : empty;
    return {{"start", {{"line", l - 1}, {"character", col_to_lsp(ln, c)}}},
            {"end", {{"line", l - 1}, {"character", col_to_lsp(ln, c + len)}}}};
  }
  std::string uri_for(const std::string &path) {
    auto u = by_path.find(path);
    return u != by_path.end() ? u->second : uri_from_path(path);
  }
  json location(const Found &f) {
    return {{"uri", uri_for(f.path)},
            {"range", range_in(f.path, f.s->l, f.s->c, (int)f.s->n.size())}};
  }

  static std::string kind_label(const Sym &s) {
    if (s.k == "local")
      return "local variable";
    if (s.k == "param")
      return "parameter" + (s.p.empty() ? "" : " of `" + s.p + "`");
    if (s.k == "gparam")
      return "generic parameter";
    if (s.k == "field")
      return "field of `" + s.p + "`";
    if (s.k == "method")
      return "method of `" + s.p + "`";
    if (s.k == "optmember")
      return "member of `" + s.p + "`";
    if (s.k == "global")
      return "global variable";
    if (s.k == "alias")
      return "type alias";
    if (s.k == "generic")
      return "generic set";
    return s.k;
  }

  std::string module_label_for(const Found &f, const Doc &d) {
    if (f.path.empty() || f.path == d.path)
      return "";
    if (!stdlib.empty() && f.path.rfind(stdlib, 0) == 0)
      return "@" + f.path.substr(stdlib.size() + 1);
    size_t s = f.path.find_last_of('/');
    return s == std::string::npos ? f.path : f.path.substr(s + 1);
  }

  json hover_for(const Doc &d, const Found &f) {
    std::string md;
    if (f.s) {
      md = "```risk\n" + f.s->d + "\n```\n";
      std::string kl = kind_label(*f.s);
      std::string where = module_label_for(f, d);
      md += "*" + kl + (where.empty() ? "" : " — from `" + where + "`") + "*";
      if (!f.s->ext.empty())
        md += "  \nC function/type from `" + f.s->ext + "`";
      if (!f.s->doc.empty())
        md += "\n\n" + f.s->doc;
    } else if (f.b) {
      std::string sig = f.recv.valid() ? fill(f.b->sig, f.recv) : f.b->sig;
      md = "```risk\n" + sig + "\n```\n*built-in" +
           (f.recv.valid() ? " method of `" + f.recv.n + "`" : "") + "*\n\n" +
           f.b->doc;
    }
    return {{"contents", {{"kind", "markdown"}, {"value", md}}}};
  }

  Doc *doc_for(const json &params) {
    auto it = docs.find(params["textDocument"]["uri"].get<std::string>());
    return it == docs.end() ? nullptr : &it->second;
  }
  static std::pair<int, int> pos_of(const Doc &d, const json &p) {
    int line = p["line"].get<int>() + 1;
    int col = lsp_to_col(d.line(line), p["character"].get<int>());
    return {line, col};
  }

  int ident_at(const Doc &d, int line, int col) {
    int i = token_at(d, line, col);
    if (i >= 0 && is_ident(d, i))
      return i;
    if (col > 1) {
      i = token_at(d, line, col - 1);
      if (i >= 0 && is_ident(d, i))
        return i;
    }
    return -1;
  }

  json on_hover(Doc &d, const json &params) {
    fresh(d);
    auto [l, c] = pos_of(d, params["position"]);
    int i = ident_at(d, l, c);
    if (i < 0)
      return nullptr;
    Found f = resolve_token(d, i);
    if (!f.ok()) {
      std::string word = describe_builtin_word(d.toks[i].lexeme);
      if (word.empty())
        return nullptr;
      return {{"contents",
               {{"kind", "markdown"},
                {"value", "```risk\n" + d.toks[i].lexeme + "\n```\n" + word}}},
              {"range", range_json(d, d.toks[i].line, d.toks[i].col,
                                   d.toks[i].line, tok_end(d, d.toks[i]))}};
    }
    json h = hover_for(d, f);
    h["range"] = range_json(d, d.toks[i].line, d.toks[i].col, d.toks[i].line,
                            tok_end(d, d.toks[i]));
    return h;
  }

  json on_definition(Doc &d, const json &params) {
    fresh(d);
    auto [l, c] = pos_of(d, params["position"]);
    if (d.an)
      for (auto &im : d.an->imports)
        if (im.l == l) {
          std::string p = resolve_import(im.path, dir_of(d.path));
          if (!p.empty())
            return {{"uri", uri_for(p)},
                    {"range",
                     {{"start", {{"line", 0}, {"character", 0}}},
                      {"end", {{"line", 0}, {"character", 0}}}}}};
        }
    int i = ident_at(d, l, c);
    if (i < 0)
      return nullptr;
    Found f = resolve_token(d, i);
    if (!f.s)
      return nullptr;
    return location(f);
  }

  struct KeyT {
    std::string path, name;
    int l = 0, c = 0;
    bool operator==(const KeyT &o) const {
      return path == o.path && name == o.name && l == o.l && c == o.c;
    }
  };
  std::optional<KeyT> key_of(Doc &d, int i) {
    Found f = resolve_token(d, i);
    if (!f.s)
      return std::nullopt;
    return KeyT{f.path, f.s->n, f.s->l, f.s->c};
  }

  std::vector<int> occurrences(Doc &d, const KeyT &key) {
    std::vector<int> out;
    for (int i = 0; i < (int)d.toks.size(); ++i) {
      if (!is_ident(d, i) || d.toks[i].lexeme != key.name)
        continue;
      auto k = key_of(d, i);
      if (k && *k == key)
        out.push_back(i);
    }
    return out;
  }

  json on_references(Doc &d, const json &params) {
    fresh(d);
    auto [l, c] = pos_of(d, params["position"]);
    int i = ident_at(d, l, c);
    if (i < 0)
      return json::array();
    auto key = key_of(d, i);
    if (!key)
      return json::array();
    bool decl = params.contains("context") &&
                params["context"].value("includeDeclaration", true);
    json out = json::array();
    for (auto &kv : docs) {
      Doc &o = kv.second;
      fresh(o);
      for (int t : occurrences(o, *key)) {
        const Token &tk = o.toks[t];
        if (!decl && o.path == key->path && tk.line == key->l &&
            tk.col == key->c)
          continue;
        out.push_back({{"uri", o.uri},
                       {"range", range_json(o, tk.line, tk.col, tk.line,
                                            tok_end(o, tk))}});
      }
    }
    return out;
  }

  json on_highlight(Doc &d, const json &params) {
    fresh(d);
    auto [l, c] = pos_of(d, params["position"]);
    int i = ident_at(d, l, c);
    json out = json::array();
    if (i < 0)
      return out;
    auto key = key_of(d, i);
    if (!key)
      return out;
    for (int t : occurrences(d, *key)) {
      const Token &tk = d.toks[t];
      out.push_back(
          {{"range", range_json(d, tk.line, tk.col, tk.line, tok_end(d, tk))},
           {"kind", 1}});
    }
    return out;
  }

  static bool renamable(const Sym &s) {
    return s.k == "local" || s.k == "param" || s.k == "function" ||
           s.k == "global" || s.k == "block" || s.k == "opts" ||
           s.k == "alias" || s.k == "generic" || s.k == "gparam" ||
           s.k == "optmember";
  }

  json on_prepare_rename(Doc &d, const json &params, const json &id) {
    fresh(d);
    auto [l, c] = pos_of(d, params["position"]);
    int i = ident_at(d, l, c);
    if (i < 0) {
      reply_error(id, -32602, "Nothing renamable at this position");
      return nullptr;
    }
    Found f = resolve_token(d, i);
    if (!f.s || !renamable(*f.s) || !by_path.count(f.path)) {
      reply_error(id, -32602,
                  !f.s ? "Cannot rename built-in or unresolved symbols"
                       : (!renamable(*f.s)
                              ? "Renaming fields and methods is not supported "
                                "yet (struct literals are not tracked)"
                              : "Cannot rename a symbol defined in a file that "
                                "is not open"));
      return nullptr;
    }
    const Token &tk = d.toks[i];
    return {{"range", range_json(d, tk.line, tk.col, tk.line, tok_end(d, tk))},
            {"placeholder", tk.lexeme}};
  }

  json on_rename(Doc &d, const json &params, const json &id) {
    fresh(d);
    std::string nn = params["newName"].get<std::string>();
    bool ok = !nn.empty() && (isalpha((unsigned char)nn[0]) || nn[0] == '_');
    for (char ch : nn)
      if (!isalnum((unsigned char)ch) && ch != '_')
        ok = false;
    for (auto &kw : keywords())
      if (kw == nn)
        ok = false;
    if (!ok) {
      reply_error(id, -32602, "'" + nn + "' is not a valid identifier name");
      return nullptr;
    }
    auto [l, c] = pos_of(d, params["position"]);
    int i = ident_at(d, l, c);
    if (i < 0)
      return nullptr;
    Found f = resolve_token(d, i);
    if (!f.s || !renamable(*f.s) || !by_path.count(f.path)) {
      reply_error(id, -32602, "This symbol cannot be renamed");
      return nullptr;
    }
    KeyT key{f.path, f.s->n, f.s->l, f.s->c};
    json changes = json::object();
    for (auto &kv : docs) {
      Doc &o = kv.second;
      fresh(o);
      json edits = json::array();
      for (int t : occurrences(o, key)) {
        const Token &tk = o.toks[t];
        edits.push_back(
            {{"range", range_json(o, tk.line, tk.col, tk.line, tok_end(o, tk))},
             {"newText", nn}});
      }
      if (!edits.empty())
        changes[o.uri] = edits;
    }
    return {{"changes", changes}};
  }

  static int symbol_kind(const std::string &k) {
    if (k == "function")
      return 12;
    if (k == "block")
      return 23; // Struct
    if (k == "opts")
      return 10; // Enum
    if (k == "optmember")
      return 22; // EnumMember
    if (k == "field")
      return 8;
    if (k == "method")
      return 6;
    if (k == "alias")
      return 26; // TypeParameter
    if (k == "generic")
      return 26;
    if (k == "global")
      return 14; // Constant
    return 13;   // Variable
  }

  json decl_range(const Doc &d, const Sym &s) {
    int nt = token_at(d, s.l, s.c);
    if (nt < 0)
      return range_json(d, s.l, s.c, s.l, s.c + (int)s.n.size());
    int start = nt;
    if (nt > 0 && is_ident(d, nt - 1) &&
        (d.toks[nt - 1].lexeme == "func" || d.toks[nt - 1].lexeme == "block" ||
         d.toks[nt - 1].lexeme == "opts" ||
         d.toks[nt - 1].lexeme == "generic" ||
         d.toks[nt - 1].lexeme == "typealias"))
      start = nt - 1;
    int end = nt;
    if (s.k == "function" || s.k == "block" || s.k == "opts") {
      int depth = 0;
      for (int k = nt + 1; k < (int)d.toks.size(); ++k) {
        if (is_sym(d, k, "(") || is_sym(d, k, "["))
          depth++;
        else if (is_sym(d, k, ")") || is_sym(d, k, "]"))
          depth--;
        else if (depth <= 0 && is_sym(d, k, ";")) {
          end = k;
          break;
        } else if (depth <= 0 && is_sym(d, k, "{")) {
          int bd = 0;
          end = k;
          for (int m = k; m < (int)d.toks.size(); ++m) {
            if (is_sym(d, m, "{"))
              bd++;
            else if (is_sym(d, m, "}") && --bd == 0) {
              end = m;
              break;
            }
            end = m;
          }
          break;
        } else if (depth <= 0 && is_ident(d, k) &&
                   (d.toks[k].lexeme == "func" || d.toks[k].lexeme == "block" ||
                    d.toks[k].lexeme == "opts")) {
          end = k - 1;
          break;
        }
      }
    }
    const Token &a = d.toks[start];
    const Token &b = d.toks[std::max(end, nt)];
    return range_json(d, a.line, a.col, b.line, tok_end(d, b));
  }

  json on_document_symbol(Doc &d, const json &params) {
    fresh(d);
    json out = json::array();
    if (!d.an)
      return out;
    std::map<std::string, json *> containers;
    std::vector<json> tops;
    tops.reserve(d.an->syms.size());
    for (auto &s : d.an->syms) {
      if (s.scoped || s.is_member() || s.k == "gparam")
        continue;
      json j;
      j["name"] = s.n;
      j["detail"] = s.t.empty() ? "" : s.t;
      j["kind"] = symbol_kind(s.k);
      j["range"] = decl_range(d, s);
      j["selectionRange"] = range_json(d, s.l, s.c, s.l, s.c + (int)s.n.size());
      j["children"] = json::array();
      tops.push_back(std::move(j));
    }
    size_t ti = 0;
    for (auto &s : d.an->syms) {
      if (s.scoped || s.is_member() || s.k == "gparam")
        continue;
      if (s.k == "block" || s.k == "opts")
        containers[s.n] = &tops[ti];
      ++ti;
    }
    for (auto &s : d.an->syms) {
      if (!s.is_member())
        continue;
      auto it = containers.find(s.p);
      if (it == containers.end())
        continue;
      json j;
      j["name"] = s.n;
      j["detail"] = s.t;
      j["kind"] = symbol_kind(s.k);
      j["range"] = range_json(d, s.l, s.c, s.l, s.c + (int)s.n.size());
      j["selectionRange"] = j["range"];
      (*it->second)["children"].push_back(std::move(j));
    }
    for (auto &t : tops)
      out.push_back(std::move(t));
    return out;
  }

  json on_signature_help(Doc &d, const json &params) {
    fresh(d);
    auto [l, c] = pos_of(d, params["position"]);
    int cur = token_before(d, l, c);
    if (cur < 0)
      return nullptr;
    int depth = 0, commas = 0, open = -1;
    for (int k = cur; k >= 0; --k) {
      if (is_sym(d, k, ")") || is_sym(d, k, "]") || is_sym(d, k, "}"))
        depth++;
      else if (is_sym(d, k, "[") || is_sym(d, k, "{")) {
        if (depth == 0)
          return nullptr;
        depth--;
      } else if (is_sym(d, k, "(")) {
        if (depth == 0) {
          open = k;
          break;
        }
        depth--;
      } else if (depth == 0 && is_sym(d, k, ","))
        commas++;
    }
    if (open < 1 || !is_ident(d, open - 1))
      return nullptr;
    int callee = open - 1;
    std::string label;
    std::vector<std::string> plabels;
    std::string doc;
    Found f = resolve_token(d, callee);
    if (f.s && f.s->k == "function") {
      label = f.s->d;
      plabels = f.s->ps;
      doc = f.s->doc;
    } else if (f.s && f.s->k == "method" && f.s->ty.is_object()) {
      Ty ft = ty_from_json(f.s->ty); // func(this, ...) -> R
      label = f.s->p + "." + f.s->n + "(";
      for (size_t i = 1; i < ft.a.size(); ++i) {
        std::string pl = ty_str(ft.a[i]);
        plabels.push_back(pl);
        label += (i > 1 ? ", " : "") + pl;
      }
      label += ")";
      if (ft.r)
        label += " -> " + ty_str(*ft.r);
    } else if (f.b) {
      label = f.recv.valid() ? fill(f.b->sig, f.recv) : f.b->sig;
      for (auto &p : f.b->params)
        plabels.push_back(f.recv.valid() ? fill(p, f.recv) : p);
      doc = f.b->doc;
    } else
      return nullptr;
    json plist = json::array();
    for (auto &p : plabels)
      plist.push_back({{"label", p}});
    json sig = {{"label", label}, {"parameters", plist}};
    if (!doc.empty())
      sig["documentation"] = {{"kind", "markdown"}, {"value", doc}};
    int active = std::min(commas, std::max(0, (int)plabels.size() - 1));
    return {{"signatures", json::array({sig})},
            {"activeSignature", 0},
            {"activeParameter", active}};
  }

  static bool in_comment_or_string(const std::string &line, int col1) {
    bool str = false, chr = false;
    int upto = std::min((int)line.size(), col1 - 1);
    for (int i = 0; i < upto; ++i) {
      char ch = line[i];
      if ((str || chr) && ch == '\\') {
        ++i;
        continue;
      }
      if (!chr && ch == '"')
        str = !str;
      else if (!str && ch == '\'')
        chr = !chr;
      else if (!str && !chr && ch == '/' && i + 1 < (int)line.size() &&
               line[i + 1] == '/')
        return true;
    }
    return str || chr;
  }

  json item(const std::string &label, int kind, const std::string &detail,
            const std::string &doc, const std::string &sort) {
    json j = {{"label", label}, {"kind", kind}, {"sortText", sort + label}};
    if (!detail.empty())
      j["detail"] = detail;
    if (!doc.empty())
      j["documentation"] = {{"kind", "markdown"}, {"value", doc}};
    return j;
  }

  static int completion_kind(const Sym &s) {
    if (s.k == "function")
      return 3;
    if (s.k == "method")
      return 2;
    if (s.k == "field")
      return 5;
    if (s.k == "local" || s.k == "param")
      return 6;
    if (s.k == "global")
      return 21;
    if (s.k == "block")
      return 22;
    if (s.k == "opts")
      return 13;
    if (s.k == "optmember")
      return 20;
    if (s.k == "alias" || s.k == "generic" || s.k == "gparam")
      return 25;
    return 1;
  }

  void add_sym_item(json &items, const Sym &s, const std::string &sort,
                    bool snippet, bool next_is_paren) {
    json j = item(s.n, completion_kind(s), s.d, s.doc, sort);
    if (s.k == "function" && snippet && !next_is_paren) {
      std::string ins = s.n + "(";
      for (size_t i = 0; i < s.psNames.size(); ++i)
        ins += (i ? ", " : "") + std::string("${") + std::to_string(i + 1) +
               ":" + s.psNames[i] + "}";
      ins += ")";
      j["insertText"] = ins;
      j["insertTextFormat"] = 2;
    }
    items.push_back(std::move(j));
  }

  json on_completion(Doc &d, const json &params) {
    if (!d.an)
      fresh(d);
    auto [l, c] = pos_of(d, params["position"]);
    json items = json::array();

    {
      const std::string &ln = d.line(l);
      size_t first = ln.find_first_not_of(" \t");
      if (first != std::string::npos && ln.compare(first, 6, "import") == 0 &&
          (first + 6 >= ln.size() || !isalnum((unsigned char)ln[first + 6]))) {
        std::string upto =
            ln.substr(first + 6, std::max(0, c - 1 - (int)(first + 6)));
        size_t at = upto.find('@');
        auto list = [&](const std::string &dir, bool want_dirs,
                        bool want_files) {
          if (DIR *dp = opendir(dir.c_str())) {
            while (dirent *e = readdir(dp)) {
              std::string n = e->d_name;
              if (n[0] == '.' || n[0] == '_' || n == "manifest")
                continue;
              bool isd = dir_exists(dir + n);
              if (isd && want_dirs)
                items.push_back(item(n, 9, "package", "", "0"));
              else if (!isd && want_files && n.size() > 5 &&
                       n.compare(n.size() - 5, 5, ".risk") == 0 &&
                       dir + n != d.path)
                items.push_back(item(n, 17, "Risk module", "", "0"));
            }
            closedir(dp);
          }
        };
        if (at != std::string::npos) {
          std::string pkg = upto.substr(at + 1);
          size_t slash = pkg.find_last_of('/');
          for (auto &b : stdlib_bases(dir_of(d.path))) {
            if (!dir_exists(b))
              continue;
            if (slash == std::string::npos)
              list(b, true, false);
            else
              list(b + pkg.substr(0, slash + 1), false, true);
            break;
          }
        } else if (upto.find('"') != std::string::npos) {
          list(dir_of(d.path).empty() ? "./" : dir_of(d.path), false, true);
        } else {
          items.push_back(item("@", 14, "standard library import", "", "0"));
        }
        return {{"isIncomplete", false}, {"items", items}};
      }
    }

    if (in_comment_or_string(d.line(l), c))
      return {{"isIncomplete", false}, {"items", items}};

    int partial = -1;
    int t_at = token_at(d, l, c);
    if (t_at >= 0 && is_ident(d, t_at) && d.toks[t_at].col < c)
      partial = t_at;
    else if (c > 1) {
      int t2 = token_at(d, l, c - 1);
      if (t2 >= 0 && is_ident(d, t2))
        partial = t2;
    }
    int prev = partial >= 0 ? partial - 1 : token_before(d, l, c);
    bool next_paren = false;
    {
      const std::string &ln = d.line(l);
      int endc = partial >= 0 ? tok_end(d, d.toks[partial]) : c;
      next_paren = endc - 1 < (int)ln.size() && ln[endc - 1] == '(';
    }

    if (is_sym(d, prev, ".")) {
      int base = prev - 1;
      if (is_ident(d, base)) {
        const Token &q = d.toks[base];
        bool shadowed = find_scoped(d, q.lexeme, q.line, q.col) != nullptr;
        if (!shadowed) {
          auto mods = closure_of_alias(d, q.lexeme);
          if (!mods.empty()) {
            std::set<std::string> seen;
            for (auto &m : mods)
              for (auto &s : m.an->syms)
                if (!s.scoped && !s.is_member() && s.n[0] != '_' &&
                    seen.insert(s.n).second)
                  add_sym_item(items, s, "0", true, next_paren);
            return {{"isIncomplete", false}, {"items", items}};
          }
        }
        Found bf = lookup_visible(d, q.lexeme, q.line, q.col);
        if (bf.s && bf.s->k == "opts" && !is_sym(d, base - 1, ".")) {
          std::vector<std::pair<const Sym *, std::string>> v;
          collect_members(d, bf.s->n, "", v);
          for (auto &m : v)
            add_sym_item(items, *m.first, "0", false, false);
          return {{"isIncomplete", false}, {"items", items}};
        }
      }
      auto ty = expr_type(d, base);
      if (ty) {
        Ty t = deref(d, *ty);
        std::vector<std::pair<const Sym *, std::string>> v;
        collect_members(d, t.n, "", v);
        for (auto &m : v)
          add_sym_item(items, *m.first, "0", false, false);
        for (auto &b : builtin_methods(t.n)) {
          bool fixed_arr = t.n == "Array" && t.a.size() == 2;
          if (fixed_arr && b.name != "repr")
            continue;
          json j = item(b.name, 2, fill(b.sig, t), b.doc, "0");
          std::string ins = b.name + "(";
          for (size_t i = 0; i < b.params.size(); ++i) {
            std::string pn = b.params[i].substr(0, b.params[i].find(':'));
            ins += (i ? ", " : "") + std::string("${") + std::to_string(i + 1) +
                   ":" + pn + "}";
          }
          ins += ")";
          if (!next_paren) {
            j["insertText"] = ins;
            j["insertTextFormat"] = 2;
          }
          items.push_back(std::move(j));
        }
      }
      return {{"isIncomplete", false}, {"items", items}};
    }

    bool type_pos = is_sym(d, prev, ":") || is_sym(d, prev, "->");
    if (is_sym(d, prev, "<") && is_ident(d, prev - 1)) {
      const std::string &w = d.toks[prev - 1].lexeme;
      type_pos = type_pos || w == "Array" || w == "Hashmap" || w == "ptr" ||
                 w == "refc" || w == "opt" || w == "cast" || w == "unsafe_cast";
    }
    if (type_pos) {
      for (auto &t : primitive_types())
        items.push_back(item(t, 25, "primitive type", "", "0"));
      for (auto &t : container_types())
        items.push_back(item(t, 7, "built-in type", "", "1"));
      std::set<std::string> seen;
      auto add_types = [&](const Analysis &an) {
        for (auto &s : an.syms)
          if (!s.scoped &&
              (s.k == "block" || s.k == "opts" || s.k == "alias" ||
               s.k == "generic") &&
              s.n[0] != '_' && seen.insert(s.n).second)
            add_sym_item(items, s, "0", false, false);
      };
      if (d.an) {
        add_types(*d.an);
        for (auto &s : d.an->syms)
          if (s.k == "gparam" && visible_at(s, l, c))
            add_sym_item(items, s, "0", false, false);
      }
      for (auto &m : closure(d))
        add_types(*m.an);
      return {{"isIncomplete", false}, {"items", items}};
    }

    std::set<std::string> seen;
    if (d.an) {
      for (auto &s : d.an->syms)
        if (s.scoped && s.k != "gparam" && visible_at(s, l, c) &&
            seen.insert(s.n).second)
          add_sym_item(items, s, "0", true, next_paren);
      for (auto &s : d.an->syms)
        if (!s.scoped && !s.is_member() && seen.insert(s.n).second)
          add_sym_item(items, s, "1", true, next_paren);
    }
    for (auto &m : closure(d))
      for (auto &s : m.an->syms)
        if (!s.scoped && !s.is_member() && s.n[0] != '_' &&
            seen.insert(s.n).second)
          add_sym_item(items, s, "2", true, next_paren);
    if (d.an)
      for (auto &im : d.an->imports)
        if (!im.alias.empty() && seen.insert(im.alias).second)
          items.push_back(
              item(im.alias, 9, "module alias for " + im.path, "", "1"));
    for (auto &b : builtin_functions()) {
      if (!seen.insert(b.name).second)
        continue;
      json j = item(b.name, 3, b.sig, b.doc, "3");
      if (!next_paren) {
        std::string ins = b.name + "(";
        for (size_t i = 0; i < b.params.size(); ++i)
          ins += (i ? ", " : "") + std::string("${") + std::to_string(i + 1) +
                 ":" + b.params[i].substr(0, b.params[i].find(':')) + "}";
        ins += ")";
        j["insertText"] = ins;
        j["insertTextFormat"] = 2;
      }
      items.push_back(std::move(j));
    }
    for (auto &k : keywords())
      if (seen.insert(k).second)
        items.push_back(item(k, 14, "", "", "4"));
    for (auto &t : primitive_types())
      if (seen.insert(t).second)
        items.push_back(item(t, 25, "primitive type", "", "5"));
    for (auto &t : container_types())
      if (seen.insert(t).second)
        items.push_back(item(t, 7, "built-in type", "", "5"));
    auto snip = [&](const char *label, const char *text, const char *detail) {
      json j = item(label, 15, detail, "", "6");
      j["insertText"] = text;
      j["insertTextFormat"] = 2;
      j["label"] = std::string(label);
      items.push_back(std::move(j));
    };
    snip("func (definition)", "func ${1:name}(${2}) -> ${3:void} {\n\t$0\n}",
         "function definition");
    snip("block (definition)", "block ${1:Name} {\n\t${2:field}: ${3:i32};\n}",
         "block definition");
    snip("opts (definition)",
         "opts ${1:Name} {\n\t${2:First},\n\t${3:Second}\n}",
         "opts definition");
    snip("for (loop)",
         "for (${1:i}: i32 = 0; ${1:i} < ${2:n}; ${1:i} += 1) {\n\t$0\n}",
         "for loop");
    snip("while (loop)", "while (${1:cond}) {\n\t$0\n}", "while loop");
    snip("if (statement)", "if (${1:cond}) {\n\t$0\n}", "if statement");
    snip("switch (statement)",
         "switch (${1:value}) {\n\tcase "
         "${2:1}:\n\t\t$0\n\t\tbreak;\n\tdefault:\n}",
         "switch statement");
    return {{"isIncomplete", false}, {"items", items}};
  }

  json capabilities() {
    return {
        {"textDocumentSync",
         {{"openClose", true},
          {"change", 1},
          {"save", {{"includeText", false}}}}},
        {"completionProvider",
         {{"triggerCharacters", json::array({".", "@", "/"})}}},
        {"hoverProvider", true},
        {"definitionProvider", true},
        {"referencesProvider", true},
        {"documentHighlightProvider", true},
        {"documentSymbolProvider", true},
        {"renameProvider", {{"prepareProvider", true}}},
        {"signatureHelpProvider",
         {{"triggerCharacters", json::array({"(", ","})}}},
    };
  }

  void detect_stdlib(const json &params) {
    std::string configured;
    if (params.contains("initializationOptions") &&
        params["initializationOptions"].is_object())
      configured = params["initializationOptions"].value("stdlibPath", "");
    const char *env = getenv("RISK_STDLIB");
    std::vector<std::string> cands;
    if (!configured.empty())
      cands.push_back(configured);
    if (env && env[0])
      cands.push_back(env);
    if (!root.empty())
      cands.push_back(root + "/stdlib");
    if (const char *home = getenv("HOME"))
      cands.push_back(std::string(home) + "/.risk/lib");
    for (auto &c : cands) {
      std::string s = c;
      while (s.size() > 1 && s.back() == '/')
        s.pop_back();
      if (dir_exists(s)) {
        stdlib = s;
        break;
      }
    }
    if (!stdlib.empty())
      setenv("RISK_STDLIB", stdlib.c_str(), 1);
    log("stdlib: " + (stdlib.empty() ? std::string("<not found>") : stdlib));
  }

  void handle(const json &msg) {
    std::string method = msg.value("method", "");
    bool is_request = msg.contains("id") && msg.contains("method");
    json id = msg.contains("id") ? msg["id"] : json(nullptr);
    json params = msg.contains("params") ? msg["params"] : json::object();

    if (method == "initialize") {
      if (params.contains("rootUri") && params["rootUri"].is_string())
        root = path_from_uri(params["rootUri"].get<std::string>());
      else if (params.contains("workspaceFolders") &&
               params["workspaceFolders"].is_array() &&
               !params["workspaceFolders"].empty())
        root = path_from_uri(
            params["workspaceFolders"][0]["uri"].get<std::string>());
      while (root.size() > 1 && root.back() == '/')
        root.pop_back();
      detect_stdlib(params);
      reply(id, {{"capabilities", capabilities()},
                 {"serverInfo", {{"name", "risk-lsp"}, {"version", "0.1.0"}}}});
      return;
    }
    if (method == "initialized" || method == "$/cancelRequest" ||
        method == "$/setTrace" ||
        method == "workspace/didChangeConfiguration" ||
        method == "workspace/didChangeWatchedFiles")
      return;
    if (method == "shutdown") {
      shutdown = true;
      reply(id, nullptr);
      return;
    }
    if (method == "exit")
      exit(shutdown ? 0 : 1);

    if (method == "textDocument/didOpen") {
      auto &td = params["textDocument"];
      Doc &d = docs[td["uri"].get<std::string>()];
      d.uri = td["uri"].get<std::string>();
      d.path = path_from_uri(d.uri);
      d.version = td.value("version", 0);
      by_path[d.path] = d.uri;
      d.set_text(td["text"].get<std::string>());
      analyze(d);
      return;
    }
    if (method == "textDocument/didChange") {
      auto it = docs.find(params["textDocument"]["uri"].get<std::string>());
      if (it == docs.end() || params["contentChanges"].empty())
        return;
      it->second.version =
          params["textDocument"].value("version", it->second.version);
      it->second.set_text(
          params["contentChanges"].back()["text"].get<std::string>());
      return;
    }
    if (method == "textDocument/didSave") {
      auto it = docs.find(params["textDocument"]["uri"].get<std::string>());
      if (it != docs.end()) {
        for (auto &kv : docs)
          kv.second.dirty = true;
        cache.clear();
      }
      return;
    }
    if (method == "textDocument/didClose") {
      std::string uri = params["textDocument"]["uri"].get<std::string>();
      auto it = docs.find(uri);
      if (it != docs.end()) {
        by_path.erase(it->second.path);
        docs.erase(it);
      }
      notify("textDocument/publishDiagnostics",
             {{"uri", uri}, {"diagnostics", json::array()}});
      return;
    }

    if (!is_request)
      return;

    if (method.rfind("textDocument/", 0) == 0) {
      Doc *d = doc_for(params);
      if (!d) {
        reply(id, nullptr);
        return;
      }
      if (method == "textDocument/hover")
        reply(id, on_hover(*d, params));
      else if (method == "textDocument/definition")
        reply(id, on_definition(*d, params));
      else if (method == "textDocument/references")
        reply(id, on_references(*d, params));
      else if (method == "textDocument/documentHighlight")
        reply(id, on_highlight(*d, params));
      else if (method == "textDocument/documentSymbol")
        reply(id, on_document_symbol(*d, params));
      else if (method == "textDocument/completion")
        reply(id, on_completion(*d, params));
      else if (method == "textDocument/signatureHelp")
        reply(id, on_signature_help(*d, params));
      else if (method == "textDocument/prepareRename") {
        json r = on_prepare_rename(*d, params, id);
        if (!r.is_null())
          reply(id, r);
      } else if (method == "textDocument/rename") {
        json r = on_rename(*d, params, id);
        if (!r.is_null())
          reply(id, r);
      } else
        reply_error(id, -32601, "method not supported: " + method);
      return;
    }
    reply_error(id, -32601, "method not found: " + method);
  }

  std::string inbuf;

  bool next_message(json &out) {
    for (;;) {
      size_t hdr_end = inbuf.find("\r\n\r\n");
      if (hdr_end == std::string::npos)
        return false;
      size_t cl = inbuf.find("Content-Length:");
      if (cl == std::string::npos || cl > hdr_end) {
        inbuf.erase(0, hdr_end + 4);
        continue;
      }
      size_t len = (size_t)atol(inbuf.c_str() + cl + 15);
      if (inbuf.size() < hdr_end + 4 + len)
        return false;
      std::string body = inbuf.substr(hdr_end + 4, len);
      inbuf.erase(0, hdr_end + 4 + len);
      out = json::parse(body, nullptr, false);
      if (out.is_discarded())
        continue;
      return true;
    }
  }

  void run() {
    signal(SIGPIPE, SIG_IGN);
    for (;;) {
      bool any_dirty = false;
      for (auto &kv : docs)
        any_dirty = any_dirty || kv.second.dirty;

      struct pollfd pfd = {0, POLLIN, 0};
      int rc = poll(&pfd, 1, any_dirty ? 30 : -1);
      if (rc > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
        char buf[65536];
        ssize_t n = read(0, buf, sizeof buf);
        if (n <= 0)
          return; // client went away
        inbuf.append(buf, (size_t)n);
      }
      json msg;
      while (next_message(msg)) {
        try {
          handle(msg);
        } catch (const std::exception &e) {
          log(std::string("handler error: ") + e.what());
          if (msg.contains("id") && msg.contains("method"))
            reply_error(msg["id"], -32603, e.what());
        }
      }
      auto now = Clock::now();
      for (auto &kv : docs) {
        Doc &d = kv.second;
        if (d.dirty && now - d.changed >= std::chrono::milliseconds(60))
          analyze(d);
      }
    }
  }
};

int main(int argc, char **argv) {
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--version") {
      printf("risk-lsp 0.1.0\n");
      return 0;
    }
  Server s;
  s.run();
  return 0;
}
