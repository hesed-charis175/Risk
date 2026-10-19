#include <cstdint>

#include "analyzer.h"
#include "ast.h"
#include "json.hpp"
#include "lexer.h"
#include "parser.h"
#include "typechecker.h"

#include <algorithm>
#include <functional>
#include <sstream>

using json = nlohmann::json;

namespace {

json type_json(const TypeNode *t) {
  if (!t)
    return nullptr;
  json j;
  j["n"] = t->size_expr ? std::string("<expr>") : t->name;
  if (!t->params.empty()) {
    json a = json::array();
    for (auto &p : t->params)
      a.push_back(type_json(p.get()));
    j["a"] = a;
  }
  if (t->return_type)
    j["r"] = type_json(t->return_type.get());
  if (t->is_this_param)
    j["this"] = true;
  return j;
}

std::string type_str(const TypeNode *t) {
  if (!t)
    return "";
  if (t->size_expr)
    return "<expr>";
  if (t->is_this_param)
    return "this";
  const std::string &n = t->name;
  if (n.rfind("__size__:", 0) == 0)
    return n.substr(9);
  if (n.rfind("__optval__:", 0) == 0)
    return n.substr(11);
  if (n == "const" && t->params.size() == 1)
    return "const " + type_str(t->params[0].get());
  std::string out = n;
  if (n == "func") {
    out += "(";
    for (size_t i = 0; i < t->params.size(); ++i) {
      if (i)
        out += ", ";
      out += type_str(t->params[i].get());
    }
    out += ")";
    if (t->return_type)
      out += " -> " + type_str(t->return_type.get());
    return out;
  }
  if (!t->params.empty()) {
    out += "<";
    for (size_t i = 0; i < t->params.size(); ++i) {
      if (i)
        out += ", ";
      out += type_str(t->params[i].get());
    }
    out += ">";
  }
  return out;
}

struct Indexer {
  const std::string &src;
  std::vector<Token> toks;
  std::vector<std::string> lines;
  json syms = json::array();

  Indexer(const std::string &s, std::vector<Token> t)
      : src(s), toks(std::move(t)) {
    std::istringstream in(src);
    std::string ln;
    while (std::getline(in, ln)) {
      if (!ln.empty() && ln.back() == '\r')
        ln.pop_back();
      lines.push_back(ln);
    }
  }

  int tok_at(int line, int col) const {
    auto it =
        std::lower_bound(toks.begin(), toks.end(), std::make_pair(line, col),
                         [](const Token &t, const std::pair<int, int> &p) {
                           return std::make_pair(t.line, t.col) < p;
                         });
    if (it == toks.end() || it->line != line || it->col != col)
      return -1;
    return (int)(it - toks.begin());
  }
  bool is_sym(int i, const char *s) const {
    return i >= 0 && i < (int)toks.size() &&
           toks[i].type == TokenType::Symbol && toks[i].lexeme == s;
  }
  bool is_ident(int i) const {
    return i >= 0 && i < (int)toks.size() &&
           toks[i].type == TokenType::Identifier;
  }
  bool is_ident(int i, const char *s) const {
    return is_ident(i) && toks[i].lexeme == s;
  }
  int last_tok() const { return (int)toks.size() - 1; }

  int match_open(int i, const char *open, const char *close) const {
    int depth = 0;
    for (int k = i; k < (int)toks.size(); ++k) {
      if (is_sym(k, open))
        depth++;
      else if (is_sym(k, close)) {
        if (--depth == 0)
          return k;
      }
    }
    return last_tok();
  }
  int match_brace(int i) const { return match_open(i, "{", "}"); }

  int enclosing_brace_end(int i) const {
    int depth = 0;
    for (int k = i - 1; k >= 0; --k) {
      if (is_sym(k, "}"))
        depth++;
      else if (is_sym(k, "{")) {
        if (depth == 0)
          return match_brace(k);
        depth--;
      }
    }
    return last_tok();
  }

  std::string doc_above(int line) const {
    std::vector<std::string> out;
    for (int l = line - 1; l >= 1 && l <= (int)lines.size(); --l) {
      const std::string &s = lines[l - 1];
      size_t p = s.find_first_not_of(" \t");
      if (p == std::string::npos || s.compare(p, 2, "//") != 0)
        break;
      std::string body = s.substr(p + 2);
      if (!body.empty() && body[0] == ' ')
        body.erase(0, 1);
      out.push_back(body);
    }
    std::reverse(out.begin(), out.end());
    std::string r;
    for (auto &l : out)
      r += (r.empty() ? "" : "\n") + l;
    return r;
  }

  json &add(const char *kind, const std::string &name, int line, int col,
            const std::string &detail) {
    json s;
    s["k"] = kind;
    s["n"] = name;
    s["l"] = line;
    s["c"] = col;
    s["d"] = detail;
    syms.push_back(std::move(s));
    return syms.back();
  }
  void set_scope(json &s, int endTok) {
    const Token &e = toks[std::max(0, std::min(endTok, last_tok()))];
    s["el"] = e.line;
    s["ec"] = e.col;
  }
  void set_scope_from(json &s, int startTok, int endTok) {
    const Token &b = toks[std::max(0, std::min(startTok, last_tok()))];
    s["sl"] = b.line;
    s["sc"] = b.col;
    set_scope(s, endTok);
  }
  void attach_doc(json &s, int line) {
    std::string d = doc_above(line);
    if (!d.empty())
      s["doc"] = d;
  }

  std::string generic_list_str(const std::vector<GenericParam> &gps) {
    if (gps.empty())
      return "";
    std::string out = "<";
    for (size_t i = 0; i < gps.size(); ++i) {
      if (i)
        out += ", ";
      if (gps[i].is_value)
        out += "const ";
      out += gps[i].name + ": " + gps[i].constraint_set;
    }
    return out + ">";
  }

  void index_generic_params(int nameTok, const std::vector<GenericParam> &gps,
                            int scopeEndTok, const std::string &owner) {
    if (gps.empty() || !is_sym(nameTok + 1, "<"))
      return;
    int close = nameTok + 1, depth = 0;
    for (int k = nameTok + 1; k < (int)toks.size(); ++k) {
      if (is_sym(k, "<"))
        depth++;
      else if (is_sym(k, ">")) {
        if (--depth == 0) {
          close = k;
          break;
        }
      } else if (is_sym(k, ">>")) {
        depth -= 2;
        if (depth <= 0) {
          close = k;
          break;
        }
      }
    }
    size_t gi = 0;
    for (int k = nameTok + 2; k < close && gi < gps.size(); ++k) {
      if (is_ident(k) && toks[k].lexeme == gps[gi].name && is_sym(k + 1, ":") &&
          (is_sym(k - 1, "<") || is_sym(k - 1, ",") ||
           is_ident(k - 1, "const"))) {
        auto &s = add("gparam", gps[gi].name, toks[k].line, toks[k].col,
                      (gps[gi].is_value ? "const " : "") + gps[gi].name + ": " +
                          gps[gi].constraint_set);
        s["p"] = owner;
        s["t"] = gps[gi].constraint_set;
        json ty;
        ty["n"] = gps[gi].constraint_set;
        s["ty"] = ty;
        set_scope_from(s, nameTok, scopeEndTok);
        ++gi;
      }
    }
  }

  std::string func_sig(const FuncDecl *f) {
    std::string out =
        "func " + f->name + generic_list_str(f->generic_params) + "(";
    for (size_t i = 0; i < f->params.size(); ++i) {
      if (i)
        out += ", ";
      out += (f->params[i].noalias ? "noalias " : "");
      out += f->params[i].name + ": " + type_str(f->params[i].type.get());
      if (f->params[i].default_val)
        out += " = …";
    }
    out += ")";
    if (f->return_type)
      out += " -> " + type_str(f->return_type.get());
    return out;
  }

  void index_func(FuncDecl *f) {
    int nameTok = tok_at(f->line, f->col);
    auto &s = add("function", f->name, f->line, f->col,
                  (f->is_extern ? "extern " : "") + func_sig(f));
    if (f->return_type) {
      s["t"] = type_str(f->return_type.get());
      s["ty"] = type_json(f->return_type.get());
    } else {
      s["t"] = "void";
      s["ty"] = json{{"n", "void"}};
    }
    json ps = json::array();
    for (auto &p : f->params) {
      std::string label = p.name + ": " + type_str(p.type.get());
      if (p.default_val)
        label += " = …";
      ps.push_back(json{{"n", p.name}, {"label", label}});
    }
    s["ps"] = ps;
    if (f->is_extern)
      s["ext"] = f->extern_header;
    attach_doc(s, f->line);
    if (nameTok < 0)
      return;

    int open = -1;
    for (int k = nameTok + 1; k < (int)toks.size(); ++k)
      if (is_sym(k, "(")) {
        open = k;
        break;
      }
    if (open < 0)
      return;
    int close = match_open(open, "(", ")");
    int bodyEnd = close;
    for (int k = close + 1; k < (int)toks.size(); ++k) {
      if (is_sym(k, ";"))
        break;
      if (is_sym(k, "{")) {
        bodyEnd = match_brace(k);
        break;
      }
      if (is_ident(k, "func") || is_ident(k, "block") || is_ident(k, "opts"))
        break;
    }
    index_generic_params(nameTok, f->generic_params, bodyEnd, f->name);

    size_t pi = 0;
    int depth = 0;
    for (int k = open; k <= close && pi < f->params.size(); ++k) {
      if (is_sym(k, "(") || is_sym(k, "["))
        depth++;
      else if (is_sym(k, ")") || is_sym(k, "]"))
        depth--;
      else if (depth == 1 && is_ident(k) &&
               toks[k].lexeme == f->params[pi].name && is_sym(k + 1, ":") &&
               (is_sym(k - 1, "(") || is_sym(k - 1, ","))) {
        auto &ps2 =
            add("param", f->params[pi].name, toks[k].line, toks[k].col,
                f->params[pi].name + ": " + type_str(f->params[pi].type.get()));
        ps2["t"] = type_str(f->params[pi].type.get());
        ps2["ty"] = type_json(f->params[pi].type.get());
        ps2["p"] = f->name;
        set_scope_from(ps2, k, bodyEnd);
        ++pi;
      }
    }

    for (auto &st : f->body)
      walk_stmt(st.get());
  }

  void index_block(BlockDecl *b) {
    std::string det = "block " + b->name + generic_list_str(b->generic_params);
    auto &s = add("block", b->name, b->line, b->col, det);
    if (b->is_extern)
      s["ext"] = b->extern_header;
    attach_doc(s, b->line);
    int nameTok = tok_at(b->line, b->col);
    if (nameTok < 0)
      return;
    int open = -1;
    for (int k = nameTok + 1; k < (int)toks.size(); ++k) {
      if (is_sym(k, "{")) {
        open = k;
        break;
      }
      if (is_sym(k, ";"))
        break;
    }
    int end = open >= 0 ? match_brace(open) : nameTok;
    index_generic_params(nameTok, b->generic_params, end, b->name);
    if (open < 0)
      return;

    int cursor = open + 1;
    for (auto &f : b->fields) {
      int found = -1;
      for (int k = cursor; k < end; ++k) {
        if (is_ident(k) && toks[k].lexeme == f.name &&
            (is_sym(k + 1, ":") || is_sym(k + 1, ","))) {
          found = k;
          break;
        }
      }
      if (found < 0)
        continue;
      cursor = found + 1;
      bool method = f.type && f.type->name == "func" &&
                    !f.type->params.empty() && f.type->params[0]->is_this_param;
      std::string d = f.name + ": " + type_str(f.type.get());
      auto &fs = add(method ? "method" : "field", f.name, toks[found].line,
                     toks[found].col, d);
      fs["p"] = b->name;
      fs["t"] = type_str(f.type.get());
      fs["ty"] = type_json(f.type.get());
      if (f.default_val)
        fs["d"] = d + " = …";
      attach_doc(fs, toks[found].line);
    }
  }

  void index_opts(OptDecl *o) {
    auto &s = add("opts", o->name, o->line, o->col, "opts " + o->name);
    if (o->is_extern)
      s["ext"] = o->extern_header;
    attach_doc(s, o->line);
    int nameTok = tok_at(o->line, o->col);
    if (nameTok < 0)
      return;
    int open = -1;
    for (int k = nameTok + 1; k < (int)toks.size(); ++k)
      if (is_sym(k, "{")) {
        open = k;
        break;
      }
    if (open < 0)
      return;
    int end = match_brace(open);
    int cursor = open + 1;
    for (auto &f : o->fields) {
      for (int k = cursor; k < end; ++k) {
        if (is_ident(k) && toks[k].lexeme == f.name &&
            (is_sym(k - 1, "{") || is_sym(k - 1, ","))) {
          std::string d = o->name + "." + f.name;
          if (f.value)
            d += " = " + std::to_string(*f.value);
          auto &fs = add("optmember", f.name, toks[k].line, toks[k].col, d);
          fs["p"] = o->name;
          fs["t"] = o->name;
          fs["ty"] = json{{"n", o->name}};
          cursor = k + 1;
          break;
        }
      }
    }
  }

  void add_local(const char *kind, const std::string &name, TypeNode *type,
                 int line, int col) {
    int t = tok_at(line, col);
    std::string ts = type_str(type);
    auto &s = add(kind, name, line, col, name + ": " + ts);
    s["t"] = ts;
    s["ty"] = type_json(type);
    if (t >= 0) {
      s["sl"] = line;
      s["sc"] = col;
      set_scope(s, enclosing_brace_end(t));
    }
  }

  void walk_stmt(Stmt *st) {
    if (!st)
      return;
    if (auto v = dynamic_cast<VarDeclStmt *>(st)) {
      add_local("local", v->name, v->type.get(), v->line, v->col);
      walk_expr(v->init.get());
    } else if (auto r = dynamic_cast<RegisterDecl *>(st)) {
      add_local("local", r->name, r->element_type.get(), r->line, r->col);
    } else if (auto e = dynamic_cast<ExprStmt *>(st)) {
      walk_expr(e->expr.get());
    } else if (auto r = dynamic_cast<ReturnStmt *>(st)) {
      walk_expr(r->expr.get());
    } else if (auto b = dynamic_cast<BlockStmt *>(st)) {
      for (auto &x : b->stmts)
        walk_stmt(x.get());
    } else if (auto i = dynamic_cast<IfStmt *>(st)) {
      walk_expr(i->cond.get());
      walk_stmt(i->then_branch.get());
      if (i->else_branch)
        walk_stmt(i->else_branch->get());
    } else if (auto w = dynamic_cast<WhileStmt *>(st)) {
      walk_expr(w->cond.get());
      walk_stmt(w->body.get());
    } else if (auto f = dynamic_cast<ForStmt *>(st)) {
      if (f->init)
        walk_stmt(f->init->get());
      if (f->cond)
        walk_expr(f->cond->get());
      if (f->post)
        walk_stmt(f->post->get());
      walk_stmt(f->body.get());
    } else if (auto d = dynamic_cast<DeferStmt *>(st)) {
      walk_stmt(d->body.get());
    } else if (auto sw = dynamic_cast<SwitchStmt *>(st)) {
      walk_expr(sw->expr.get());
      for (auto &c : sw->cases) {
        if (c.value)
          walk_expr(c.value->get());
        for (auto &x : c.stmts)
          walk_stmt(x.get());
      }
    }
  }

  void walk_expr(Expr *e) {
    if (!e)
      return;
    if (auto u = dynamic_cast<UnaryExpr *>(e))
      walk_expr(u->rhs.get());
    else if (auto b = dynamic_cast<BinaryExpr *>(e)) {
      walk_expr(b->lhs.get());
      walk_expr(b->rhs.get());
    } else if (auto c = dynamic_cast<CallExpr *>(e)) {
      walk_expr(c->callee.get());
      for (auto &a : c->args)
        walk_expr(a.get());
    } else if (auto c = dynamic_cast<CastExpr *>(e))
      walk_expr(c->expr.get());
    else if (auto m = dynamic_cast<MemberExpr *>(e))
      walk_expr(m->object.get());
    else if (auto bl = dynamic_cast<BlockLiteralExpr *>(e)) {
      for (auto &p : bl->positional)
        walk_expr(p.get());
      for (auto &n : bl->named)
        walk_expr(n.second.get());
    } else if (auto a = dynamic_cast<ArrayLiteralExpr *>(e)) {
      for (auto &x : a->elements)
        walk_expr(x.get());
    } else if (auto i = dynamic_cast<IndexExpr *>(e)) {
      walk_expr(i->object.get());
      walk_expr(i->index.get());
    } else if (auto s = dynamic_cast<SizeofExpr *>(e))
      walk_expr(s->expr_arg.get());
    else if (auto l = dynamic_cast<LambdaExpr *>(e))
      index_lambda(l);
  }

  void index_lambda(LambdaExpr *l) {
    int open = tok_at(l->line, l->col);
    if (open >= 0 && is_sym(open, "(")) {
      int close = match_open(open, "(", ")");
      if (is_sym(close + 1, "[")) {
        int rb = match_open(close + 1, "[", "]");
        int body = rb;
        for (int k = rb + 1; k < (int)toks.size(); ++k)
          if (is_sym(k, "{")) {
            body = k;
            break;
          }
        int bodyEnd = is_sym(body, "{") ? match_brace(body) : rb;
        size_t pi = 0;
        for (int k = close + 2; k < rb && pi < l->params.size(); ++k) {
          if (is_ident(k) && toks[k].lexeme == l->params[pi].first &&
              is_sym(k + 1, ":") &&
              (is_sym(k - 1, "[") || is_sym(k - 1, ","))) {
            TypeNode *ty = l->params[pi].second.get();
            auto &s =
                add("param", l->params[pi].first, toks[k].line, toks[k].col,
                    l->params[pi].first + ": " + type_str(ty));
            s["t"] = type_str(ty);
            s["ty"] = type_json(ty);
            set_scope_from(s, k, bodyEnd);
            ++pi;
          }
        }
      }
    }
    for (auto &st : l->body)
      walk_stmt(st.get());
  }

  void index_toplevel(TranslationUnit *tu) {
    for (auto &d : tu->decls) {
      Decl *p = d.get();
      if (auto f = dynamic_cast<FuncDecl *>(p))
        index_func(f);
      else if (auto b = dynamic_cast<BlockDecl *>(p))
        index_block(b);
      else if (auto o = dynamic_cast<OptDecl *>(p))
        index_opts(o);
      else if (auto ta = dynamic_cast<TypeAliasDecl *>(p)) {
        int kw = tok_at(ta->line, ta->col);
        if (kw >= 0 && is_ident(kw + 1)) {
          auto &s = add("alias", ta->name, toks[kw + 1].line, toks[kw + 1].col,
                        "typealias " + ta->name + " = " +
                            type_str(ta->target_type.get()));
          s["t"] = type_str(ta->target_type.get());
          s["ty"] = type_json(ta->target_type.get());
          attach_doc(s, ta->line);
        }
      } else if (auto g = dynamic_cast<GenericSetDecl *>(p)) {
        int kw = tok_at(g->line, g->col);
        if (kw >= 0 && is_ident(kw + 1)) {
          std::string det = "generic " + g->name + " = [";
          for (size_t i = 0; i < g->members.size(); ++i)
            det += (i ? ", " : "") + type_str(g->members[i].get());
          det += "]";
          auto &s =
              add("generic", g->name, toks[kw + 1].line, toks[kw + 1].col, det);
          attach_doc(s, g->line);
        }
      } else if (auto gv = dynamic_cast<GlobalVarDecl *>(p)) {
        if (!gv->var)
          continue;
        auto &v = *gv->var;
        std::string ts = type_str(v.type.get());
        auto &s = add("global", v.name, v.line, v.col,
                      (gv->is_extern ? "extern " : "") + v.name + ": " +
                          (v.is_const ? "const " : "") + ts);
        s["t"] = ts;
        s["ty"] = type_json(v.type.get());
        if (gv->is_extern)
          s["ext"] = gv->extern_header;
        attach_doc(s, v.line);
        walk_expr(v.init.get());
      }
    }
  }
};

} // namespace

std::string analyze_source(const std::string &path, const std::string &src,
                           bool typecheck) {
  json out;
  out["diagnostics"] = json::array();
  out["symbols"] = json::array();
  out["imports"] = json::array();

  Lexer lexer(src, path);
  std::vector<Token> tokens = lexer.tokenize();

  for (size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i].type == TokenType::Comment)
      continue;
    if (tokens[i].type == TokenType::HeaderFileMarker)
      tokens.erase(tokens.begin() + i);
    break;
  }

  Parser parser(tokens);
  auto tu = parser.parse_translation_unit();

  bool parse_error = false;
  auto emit = [&](const Diagnostic &d) {
    json j;
    j["s"] = d.severity == Diagnostic::Severity::Error ? 1 : 2;
    j["m"] = d.message;
    j["l"] = d.line;
    j["c"] = d.col;
    j["e"] = d.end_col;
    out["diagnostics"].push_back(std::move(j));
  };
  for (auto &d : parser.diags) {
    if (d.severity == Diagnostic::Severity::Error)
      parse_error = true;
    emit(d);
  }
  if (!tu)
    return out.dump();
  tu->filename = path;

  Indexer ix(src, tokens);

  {
    std::vector<int> import_toks;
    int depth = 0;
    for (int i = 0; i < (int)ix.toks.size(); ++i) {
      if (ix.is_sym(i, "{"))
        depth++;
      else if (ix.is_sym(i, "}"))
        depth--;
      else if (depth == 0 && ix.is_ident(i, "import"))
        import_toks.push_back(i);
    }
    std::vector<ImportDecl *> imps;
    for (auto &d : tu->decls)
      if (auto im = dynamic_cast<ImportDecl *>(d.get()))
        imps.push_back(im);
    bool aligned = imps.size() == import_toks.size();
    for (size_t i = 0; i < imps.size(); ++i) {
      std::string pth = imps[i]->path;
      if (!pth.empty() && pth.front() == '"')
        pth.erase(0, 1);
      if (!pth.empty() && pth.back() == '"')
        pth.pop_back();
      json j;
      j["path"] = pth;
      j["alias"] = imps[i]->alias;
      if (aligned) {
        const Token &t = ix.toks[import_toks[i]];
        imps[i]->line = t.line;
        imps[i]->col = t.col;
        j["l"] = t.line;
        j["c"] = t.col;
      } else {
        j["l"] = 0;
        j["c"] = 0;
      }
      out["imports"].push_back(std::move(j));
    }
  }

  ix.index_toplevel(tu.get());
  out["symbols"] = std::move(ix.syms);

  if (typecheck && !parse_error) {
    TypeChecker tc;
    auto diags = tc.check(tu.get());
    for (auto &d : diags)
      emit(d);
  }
  return out.dump();
}
