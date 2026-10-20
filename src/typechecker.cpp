#include "typechecker.h"
#include "lexer.h"
#include "parser.h"
#include <cassert>
#include <cctype>
#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#include <sys/stat.h>

static std::string expand_tilde(const std::string &path) {
  if (!path.empty() && path[0] == '~') {
    const char *home = getenv("HOME");
#ifdef _WIN32

    if (!home || !home[0])
      home = getenv("USERPROFILE");
    if (!home || !home[0]) {
      const char *drive = getenv("HOMEDRIVE");
      const char *homepath = getenv("HOMEPATH");
      if (drive && homepath && drive[0] && homepath[0]) {
        std::string h = std::string(drive) + homepath;
        return h + path.substr(1);
      }
    }
#endif
    if (home && home[0])
      return std::string(home) + path.substr(1);
  }
  return path;
}

static bool starts_with(const std::string &s, const std::string &p) {
  return s.rfind(p, 0) == 0;
}

static bool path_is_regular_file(const std::string &path) {
#ifdef _WIN32
  struct _stat st;
  if (_stat(path.c_str(), &st) != 0)
    return false;
  return (st.st_mode & _S_IFREG) != 0;
#else
  struct stat st;
  if (stat(path.c_str(), &st) != 0)
    return false;
  return S_ISREG(st.st_mode);
#endif
}

static std::string canonical_name_of(TypeNode *tn) {
  if (!tn)
    return "void";
  std::string s = tn->name;
  if (!tn->params.empty()) {
    s += "<";
    for (size_t i = 0; i < tn->params.size(); ++i) {
      if (i)
        s += ",";
      s += canonical_name_of(tn->params[i].get());
    }
    s += ">";
  }
  return s;
}

static TypePtr type_node_from_canonical(const std::string &s) {
  size_t lt = s.find('<');
  if (lt == std::string::npos || s.back() != '>')
    return std::make_unique<TypeNode>(s);

  std::string base = s.substr(0, lt);
  std::string inner = s.substr(lt + 1, s.size() - lt - 2);

  auto node = std::make_unique<TypeNode>(base);
  int depth = 0;
  size_t start = 0;
  for (size_t i = 0; i < inner.size(); ++i) {
    char c = inner[i];
    if (c == '<')
      depth++;
    else if (c == '>')
      depth--;
    else if (c == ',' && depth == 0) {
      node->params.push_back(type_node_from_canonical(inner.substr(start, i - start)));
      start = i + 1;
    }
  }
  if (start < inner.size())
    node->params.push_back(type_node_from_canonical(inner.substr(start)));
  return node;
}

static std::optional<long long>
eval_symbolic_size(Expr *e,
                   const std::unordered_map<std::string, std::string> &bindings) {
  if (!e)
    return std::nullopt;
  if (auto n = dynamic_cast<NumberExpr *>(e)) {
    try {
      return std::stoll(n->literal);
    } catch (...) {
      return std::nullopt;
    }
  }
  if (auto id = dynamic_cast<IdentifierExpr *>(e)) {
    auto it = bindings.find(id->name);
    if (it == bindings.end())
      return std::nullopt;
    try {
      return std::stoll(it->second);
    } catch (...) {
      return std::nullopt;
    }
  }
  if (auto un = dynamic_cast<UnaryExpr *>(e)) {
    auto v = eval_symbolic_size(un->rhs.get(), bindings);
    if (!v)
      return std::nullopt;
    if (un->op == "-")
      return -*v;
    if (un->op == "+")
      return *v;
    if (un->op == "~")
      return ~(*v);
    if (un->op == "!")
      return !(*v);
    return std::nullopt;
  }
  if (auto bin = dynamic_cast<BinaryExpr *>(e)) {
    auto l = eval_symbolic_size(bin->lhs.get(), bindings);
    auto r = eval_symbolic_size(bin->rhs.get(), bindings);
    if (!l || !r)
      return std::nullopt;
    if (bin->op == "+")
      return *l + *r;
    if (bin->op == "-")
      return *l - *r;
    if (bin->op == "*")
      return *l * *r;
    if (bin->op == "/")
      return *r == 0 ? std::nullopt : std::make_optional(*l / *r);
    if (bin->op == "%")
      return *r == 0 ? std::nullopt : std::make_optional(*l % *r);
    if (bin->op == "<<")
      return *l << *r;
    if (bin->op == ">>")
      return *l >> *r;
    if (bin->op == "&")
      return *l & *r;
    if (bin->op == "|")
      return *l | *r;
    if (bin->op == "^")
      return *l ^ *r;
    return std::nullopt;
  }
  return std::nullopt;
}

static std::string stringify_symbolic_expr(Expr *e) {
  if (!e)
    return "?";
  if (auto n = dynamic_cast<NumberExpr *>(e))
    return n->literal;
  if (auto id = dynamic_cast<IdentifierExpr *>(e))
    return id->name;
  if (auto un = dynamic_cast<UnaryExpr *>(e))
    return un->op + stringify_symbolic_expr(un->rhs.get());
  if (auto bin = dynamic_cast<BinaryExpr *>(e))
    return stringify_symbolic_expr(bin->lhs.get()) + " " + bin->op + " " +
           stringify_symbolic_expr(bin->rhs.get());
  return "?";
}

static std::string sanitize_for_mangle(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s)
    out.push_back((std::isalnum((unsigned char)c) || c == '_') ? c : '_');
  return out;
}

static bool is_const_compatible_init(const std::string &decl,
                                     const std::string &init);

static bool is_integer_type_name(const std::string &n) {
  return n == "i8" || n == "i16" || n == "i32" || n == "i64" || n == "u8" ||
         n == "u16" || n == "u32" || n == "u64";
}
static bool is_floating_type_name(const std::string &n) {
  return n == "f32" || n == "f64";
}
static bool is_numeric_type_name(const std::string &n) {
  return is_integer_type_name(n) || is_floating_type_name(n);
}

static int numeric_rank(const std::string &n) {
  if (n == "f64")
    return 60;
  if (n == "f32")
    return 50;
  if (n == "i64")
    return 40;
  if (n == "u64")
    return 39;
  if (n == "i32")
    return 30;
  if (n == "u32")
    return 29;
  if (n == "i16")
    return 20;
  if (n == "u16")
    return 19;
  if (n == "i8")
    return 10;
  if (n == "u8")
    return 9;
  return 0;
}

static std::string promote_numeric_name(const std::string &a,
                                        const std::string &b) {
  if (is_floating_type_name(a) || is_floating_type_name(b)) {
    if (a == "f64" || b == "f64")
      return "f64";
    return "f32";
  }
  if (is_integer_type_name(a) && is_integer_type_name(b)) {
    return (numeric_rank(a) >= numeric_rank(b)) ? a : b;
  }
  if (is_numeric_type_name(a))
    return a;
  if (is_numeric_type_name(b))
    return b;
  return a;
}

static bool can_convert_literal_to_unsigned(Expr *e,
                                            const std::string &target_type) {

  auto num = dynamic_cast<NumberExpr *>(e);
  if (!num) {
    return false;
  }

  bool is_float_literal = num->literal.find('.') != std::string::npos;

  if (is_float_literal) {
    return target_type == "f32" || target_type == "f64";
  }

  try {
    long long val = std::stoll(num->literal);

    if (target_type == "u8" && val >= 0 && val <= 255)
      return true;
    if (target_type == "u16" && val >= 0 && val <= 65535)
      return true;
    if (target_type == "u32" && val >= 0 && val <= 4294967295LL)
      return true;
    if (target_type == "u64" && val >= 0)
      return true;
    if (target_type == "i8" && val >= -128 && val <= 127)
      return true;
    if (target_type == "i16" && val >= -32768 && val <= 32767)
      return true;
    if (target_type == "i32" && val >= -2147483648LL && val <= 2147483647LL)
      return true;
    if (target_type == "i64")
      return true;
    if (target_type == "f32" || target_type == "f64")
      return true;

  } catch (...) {
    return false;
  }

  return false;
}

static std::string strip_const_wrapper(const std::string &n) {
  if (n.rfind("const<", 0) == 0) {
    size_t p = n.find('<');
    size_t q = n.rfind('>');
    if (p != std::string::npos && q != std::string::npos && q > p + 1)
      return n.substr(p + 1, q - p - 1);
  }
  if (n.rfind("const ", 0) == 0) {
    return n.substr(6);
  }
  return n;
}

static bool extract_hashmap_kv(const std::string &n, std::string &out_key,
                               std::string &out_value) {
  if (n.rfind("Hashmap<", 0) != 0)
    return false;
  size_t p = n.find('<');
  size_t q = n.rfind('>');
  if (p == std::string::npos || q == std::string::npos || q <= p + 1)
    return false;
  std::string contents = n.substr(p + 1, q - p - 1);
  int depth = 0;
  size_t top_comma = std::string::npos;
  for (size_t i = 0; i < contents.size(); ++i) {
    if (contents[i] == '<')
      ++depth;
    else if (contents[i] == '>')
      --depth;
    else if (contents[i] == ',' && depth == 0) {
      top_comma = i;
      break;
    }
  }
  if (top_comma == std::string::npos)
    return false;
  std::string key = contents.substr(0, top_comma);
  std::string value = contents.substr(top_comma + 1);
  auto trim = [](std::string s) {
    while (!s.empty() && s.back() == ' ')
      s.pop_back();
    while (!s.empty() && s.front() == ' ')
      s = s.substr(1);
    return s;
  };
  out_key = trim(key);
  out_value = trim(value);
  return !out_key.empty() && !out_value.empty();
}

static std::string extract_inner_type_name(const std::string &n) {
  if (n.rfind("const<", 0) == 0) {
    size_t p = n.find('<');
    size_t q = n.rfind('>');
    if (p != std::string::npos && q != std::string::npos && q > p + 1)
      return n.substr(p + 1, q - p - 1);
    return "";
  }
  if (n.rfind("const ", 0) == 0) {
    return n.substr(6);
  }
  size_t p = n.find('<');
  size_t q = n.rfind('>');
  if (p != std::string::npos && q != std::string::npos && q > p + 1) {
    std::string contents = n.substr(p + 1, q - p - 1);

    if (n.rfind("Array<", 0) == 0) {

      int depth = 0;
      size_t top_comma = std::string::npos;
      for (size_t i = 0; i < contents.size(); ++i) {
        if (contents[i] == '<')
          ++depth;
        else if (contents[i] == '>')
          --depth;
        else if (contents[i] == ',' && depth == 0) {
          top_comma = i;
          break;
        }
      }
      if (top_comma != std::string::npos) {
        std::string result = contents.substr(0, top_comma);
        while (!result.empty() && result.back() == ' ')
          result.pop_back();
        while (!result.empty() && result[0] == ' ')
          result = result.substr(1);
        return result;
      }
    }
    return contents;
  }
  return "";
}

static long long extract_static_array_size(const std::string &name) {
  if (name.rfind("Array<", 0) != 0)
    return -1;
  size_t p = name.find('<'), q = name.rfind('>');
  if (p == std::string::npos || q == std::string::npos || q <= p + 1)
    return -1;
  std::string contents = name.substr(p + 1, q - p - 1);

  int depth = 0;
  size_t top_comma = std::string::npos;
  for (size_t i = 0; i < contents.size(); ++i) {
    if (contents[i] == '<')
      ++depth;
    else if (contents[i] == '>')
      --depth;
    else if (contents[i] == ',' && depth == 0) {
      top_comma = i;
      break;
    }
  }
  if (top_comma == std::string::npos)
    return -1;
  std::string size_part = contents.substr(top_comma + 1);

  size_t a = size_part.find_first_not_of(" \t");
  size_t b = size_part.find_last_not_of(" \t");
  if (a == std::string::npos)
    return -1;
  size_part = size_part.substr(a, b - a + 1);

  const std::string prefix = "__size__:";
  if (size_part.rfind(prefix, 0) == 0)
    size_part = size_part.substr(prefix.size());
  try {
    return std::stoll(size_part);
  } catch (...) {
    return -1;
  }
}

static bool is_ptr_type_name(const std::string &n) {
  std::string s = n;
  if (s.rfind("const<", 0) == 0) {
    size_t p = s.find('<');
    size_t q = s.rfind('>');
    if (p != std::string::npos && q != std::string::npos && q > p + 1)
      s = s.substr(p + 1, q - p - 1);
  } else if (s.rfind("const ", 0) == 0) {
    s = s.substr(6);
  }
  return s.rfind("ptr<", 0) == 0;
}
static bool is_ref_type_name(const std::string &n) {
  std::string s = n;
  if (s.rfind("const<", 0) == 0) {
    size_t p = s.find('<');
    size_t q = s.rfind('>');
    if (p != std::string::npos && q != std::string::npos && q > p + 1)
      s = s.substr(p + 1, q - p - 1);
  } else if (s.rfind("const ", 0) == 0) {
    s = s.substr(6);
  }
  return s.rfind("ref<", 0) == 0;
}
static bool is_func_type_name(const std::string &n) {
  std::string s = n;
  if (s.rfind("const<", 0) == 0) {
    size_t p = s.find('<');
    size_t q = s.rfind('>');
    if (p != std::string::npos && q != std::string::npos && q > p + 1)
      s = s.substr(p + 1, q - p - 1);
  } else if (s.rfind("const ", 0) == 0) {
    s = s.substr(6);
  }
  return s.rfind("func(", 0) == 0;
}

static void parse_func_signature_string(const std::string &s,
                                        std::vector<std::string> &out_params,
                                        std::string &out_ret) {
  out_params.clear();
  out_ret = "void";
  if (!starts_with(s, "func("))
    return;
  auto pos = s.find(")->");
  std::string inside;
  if (pos != std::string::npos) {
    inside = s.substr(5, pos - 5);
    out_ret = s.substr(pos + 3);
  } else {
    size_t start = 5;
    size_t end = s.find(')', start);
    if (end != std::string::npos)
      inside = s.substr(start, end - start);
  }
  size_t i = 0;
  while (i < inside.size()) {
    size_t j = inside.find(',', i);
    if (j == std::string::npos)
      j = inside.size();
    std::string tok = inside.substr(i, j - i);
    size_t a = tok.find_first_not_of(" \t");
    size_t b = tok.find_last_not_of(" \t");
    if (a != std::string::npos && b != std::string::npos && b >= a)
      out_params.push_back(tok.substr(a, b - a + 1));
    else if (!tok.empty())
      out_params.push_back(tok);
    i = j + 1;
  }
}

static std::string module_name_from_import_path(const std::string &raw_path);

std::vector<Diagnostic> TypeChecker::check(TranslationUnit *tu) {
  diags.clear();
  functions.clear();
  var_scopes.clear();
  push_scope();
  compile_time_string_map.clear();
  expr_type_map.clear();
  blocks.clear();
  type_aliases.clear();
  opts.clear();
  current_tu = tu;
  generic_sets.clear();
  generic_templates.clear();
  generic_instances.clear();
  generic_worklist.clear();
  generic_block_templates.clear();
  generic_block_instances.clear();

  std::string source_dir = "";
  if (!tu->filename.empty()) {
    size_t last_slash = tu->filename.find_last_of("/\\");
    if (last_slash != std::string::npos) {
      source_dir = tu->filename.substr(0, last_slash + 1);
    }
  }

  for (size_t __i = 0; __i < tu->decls.size(); ++__i) {
    if (auto im = dynamic_cast<ImportDecl *>(tu->decls[__i].get())) {
      std::string error_msg = load_imported_header(im->path, source_dir);
      if (!error_msg.empty()) {

        diags.push_back({Diagnostic::Severity::Error, error_msg, im->line,
                         im->col, im->col});
      }

      std::string module_name = module_name_from_import_path(im->path);

      std::string value = im->path + "|" + module_name;
      if (!im->alias.empty()) {
        imported_aliases[im->alias] = value;
      }
      imported_aliases["__unqualified__" + module_name] = value;
    }
  }

  for (auto &dptr : tu->decls) {
    if (auto ta = dynamic_cast<TypeAliasDecl *>(dptr.get())) {
      register_type_alias(ta);
    }
  }

  for (auto &dptr : tu->decls) {
    if (auto gs = dynamic_cast<GenericSetDecl *>(dptr.get())) {
      register_generic_set(gs);
    }
  }

  for (auto &dptr : tu->decls) {
    if (auto b = dynamic_cast<OptDecl *>(dptr.get())) {
      register_opt(b);
    }
  }

  for (auto &dptr : tu->decls) {
    if (auto b = dynamic_cast<BlockDecl *>(dptr.get())) {
      register_block(b);
    }
  }
  for (auto &dptr : tu->decls) {
    if (auto gv = dynamic_cast<GlobalVarDecl *>(dptr.get())) {
      check_stmt(gv->var.get(), TypeInfo("void"));
    }
  }

  add_func("ioInput", {}, TypeInfo("string"), 0, 0, nullptr);

  add_func("toInt", {TypeInfo("string")}, TypeInfo("i32"), 0, 0, nullptr);
  add_func("toFloat", {TypeInfo("string")}, TypeInfo("f64"), 0, 0, nullptr);
  add_func("toBool", {TypeInfo("string")}, TypeInfo("bool"), 0, 0, nullptr);
  add_func("toString", {TypeInfo("any")}, TypeInfo("string"), 0, 0, nullptr);

  add_func("intToChar", {TypeInfo("i32")}, TypeInfo("string"), 0, 0, nullptr);

  add_func("length", {TypeInfo("any")}, TypeInfo("i32"), 0, 0, nullptr);

  add_func("strRepeat", {TypeInfo("string"), TypeInfo("i32")},
           TypeInfo("string"), 0, 0, nullptr);

  add_func("kbRead", {}, TypeInfo("string"), 0, 0, nullptr);
  add_func("kbWait", {}, TypeInfo("string"), 0, 0, nullptr);
  add_func("clrScr", {}, TypeInfo("void"), 0, 0, nullptr);

  for (auto &dptr : tu->decls) {
    if (auto f = dynamic_cast<FuncDecl *>(dptr.get())) {
      if (!f->generic_params.empty()) {

        generic_templates[f->name] = f;
        continue;
      }
      std::vector<TypeInfo> params;
      for (auto &p : f->params)
        params.emplace_back(type_from_typenode(p.type.get()));
      TypeInfo ret = f->return_type ? type_from_typenode(f->return_type.get())
                                    : TypeInfo("void");

      auto existing = functions.find(f->name);
      if (existing != functions.end() && existing->second.decl == f)
        continue;
      add_func(f->name, params, ret, f->line, f->col, f);
    }
  }

  for (auto &dptr : tu->decls) {
    if (auto b = dynamic_cast<BlockDecl *>(dptr.get())) {
      if (!b->generic_params.empty())
        continue;
      for (auto &f : b->fields) {
        if (f.default_val) {
          type_of_expr(f.default_val.get());
        }
      }
    }
  }

  auto main_it = functions.find("main");
  if (main_it != functions.end() && main_it->second.decl) {
    FuncDecl *main_func = main_it->second.decl;
    TypeInfo main_ret = main_func->return_type
                            ? type_from_typenode(main_func->return_type.get())
                            : TypeInfo("void");

    std::string ret_name = main_ret.name;

    if (!is_integer_type_name(ret_name)) {
      diags.push_back({Diagnostic::Severity::Error,
                       "main function must return an integer type (i8, i16, "
                       "i32, i64, u8, u16, u32, or u64), but returns '" +
                           ret_name + "'",
                       main_func->line, main_func->col});
    }

    if (main_func->params.size() > 1) {
      diags.push_back(
          {Diagnostic::Severity::Error,
           "main function must take no parameters or a single "
           "'args: Array<string>' parameter",
           main_func->line, main_func->col});
    } else if (main_func->params.size() == 1) {
      TypeInfo arg_t = type_from_typenode(main_func->params[0].type.get());
      if (arg_t.name != "Array<string>") {
        diags.push_back(
            {Diagnostic::Severity::Error,
             "main function's parameter must have type 'Array<string>', got '" +
                 arg_t.name + "'",
             main_func->line, main_func->col});
      }
    }
  }

  std::vector<FuncDecl *> to_check;
  for (auto &entry : functions) {
    if (entry.second.decl && entry.second.decl->is_definition)
      to_check.push_back(entry.second.decl);
  }
  for (auto *f : to_check)
    check_func(f);

  while (!generic_worklist.empty()) {
    FuncDecl *f = generic_worklist.back();
    generic_worklist.pop_back();
    if (f->is_definition)
      check_func(f);
  }

  pop_scope();

  {
    std::unordered_set<std::string> checked;
    for (auto &dptr : tu->decls) {
      auto f = dynamic_cast<FuncDecl *>(dptr.get());
      if (!f || !f->is_extern || f->extern_header.empty())
        continue;
      if (!checked.insert(f->extern_header + "|" + f->extern_search_dir)
               .second)
        continue;

      std::string header = f->extern_header;
      if (!header.empty() && header.front() == '"')
        header.erase(0, 1);
      if (!header.empty() && header.back() == '"')
        header.pop_back();

      for (auto &lib : f->extern_link_libs) {
        bool already = false;
        for (auto &existing : extern_link_libs)
          if (existing == lib) {
            already = true;
            break;
          }
        if (!already)
          extern_link_libs.push_back(lib);
      }

      std::vector<std::string> candidates;
      if (!f->extern_search_dir.empty())
        candidates.push_back(f->extern_search_dir + header);
      if (!source_dir.empty())
        candidates.push_back(source_dir + header);
      candidates.push_back(header);

      std::string found_path;
      for (auto &c : candidates) {
        if (!path_is_regular_file(c))
          continue;
        std::ifstream in(c, std::ios::in | std::ios::binary);
        if (in) {
          found_path = c;
          break;
        }
      }
      if (found_path.empty()) {

        if (!f->extern_link_libs.empty())
          continue;
        diags.push_back(
            {Diagnostic::Severity::Error,
             "extern header '" + header + "' not found (looked in " +
                 (f->extern_search_dir.empty() ? source_dir
                                               : f->extern_search_dir) +
                 ")",
             f->line, f->col, f->col});
        continue;
      }

      if (found_path.size() >= 2 &&
          found_path.compare(found_path.size() - 2, 2, ".h") == 0) {
        std::string sibling_c =
            found_path.substr(0, found_path.size() - 2) + ".c";
        std::ifstream cin(sibling_c, std::ios::in | std::ios::binary);
        if (cin) {
          bool already = false;
          for (auto &s : extern_link_sources)
            if (s == sibling_c) {
              already = true;
              break;
            }
          if (!already)
            extern_link_sources.push_back(sibling_c);
        }
      }
    }

    for (auto &dptr : tu->decls) {
      auto b = dynamic_cast<BlockDecl *>(dptr.get());
      if (!b || !b->is_extern || b->extern_header.empty())
        continue;
      if (!checked.insert(b->extern_header + "|" + b->extern_search_dir)
               .second)
        continue;

      std::string header = b->extern_header;
      if (!header.empty() && header.front() == '"')
        header.erase(0, 1);
      if (!header.empty() && header.back() == '"')
        header.pop_back();

      std::vector<std::string> candidates;
      if (!b->extern_search_dir.empty())
        candidates.push_back(b->extern_search_dir + header);
      if (!source_dir.empty())
        candidates.push_back(source_dir + header);
      candidates.push_back(header);

      std::string found_path;
      for (auto &c : candidates) {
        if (!path_is_regular_file(c))
          continue;
        std::ifstream in(c, std::ios::in | std::ios::binary);
        if (in) {
          found_path = c;
          break;
        }
      }
      if (found_path.empty()) {
        diags.push_back(
            {Diagnostic::Severity::Error,
             "extern header '" + header + "' not found (looked in " +
                 (b->extern_search_dir.empty() ? source_dir
                                               : b->extern_search_dir) +
                 ")",
             b->line, b->col, b->col});
      }
    }

    for (auto &dptr : tu->decls) {
      std::string header, search_dir;
      int line = 0, col = 0;
      if (auto o = dynamic_cast<OptDecl *>(dptr.get())) {
        if (!o->is_extern || o->extern_header.empty())
          continue;
        header = o->extern_header;
        search_dir = o->extern_search_dir;
        line = o->line;
        col = o->col;
      } else if (auto gv = dynamic_cast<GlobalVarDecl *>(dptr.get())) {
        if (!gv->is_extern || gv->extern_header.empty())
          continue;
        header = gv->extern_header;
        search_dir = gv->extern_search_dir;
        line = gv->line;
        col = gv->col;
      } else {
        continue;
      }
      if (!checked.insert(header + "|" + search_dir).second)
        continue;

      if (!header.empty() && header.front() == '"')
        header.erase(0, 1);
      if (!header.empty() && header.back() == '"')
        header.pop_back();

      std::vector<std::string> candidates;
      if (!search_dir.empty())
        candidates.push_back(search_dir + header);
      if (!source_dir.empty())
        candidates.push_back(source_dir + header);
      candidates.push_back(header);

      std::string found_path;
      for (auto &c : candidates) {
        if (!path_is_regular_file(c))
          continue;
        std::ifstream in(c, std::ios::in | std::ios::binary);
        if (in) {
          found_path = c;
          break;
        }
      }
      if (found_path.empty()) {
        diags.push_back(
            {Diagnostic::Severity::Error,
             "extern header '" + header + "' not found (looked in " +
                 (search_dir.empty() ? source_dir : search_dir) + ")",
             line, col, col});
      }
    }
  }

  return diags;
}

static std::string module_name_from_import_path(const std::string &raw_path) {
  std::string path = raw_path;
  if (!path.empty() && path.front() == '"')
    path.erase(0, 1);
  if (!path.empty() && path.back() == '"')
    path.pop_back();
  if (!path.empty() && path.front() == '@')
    path.erase(0, 1);
  size_t slash = path.find_last_of("/\\");
  if (slash != std::string::npos)
    path = path.substr(slash + 1);
  if (path.size() >= 5 && path.substr(path.size() - 5) == ".risk")
    path = path.substr(0, path.size() - 5);
  return path;
}

std::string TypeChecker::load_imported_header(const std::string &import_path,
                                              const std::string &source_dir) {

  std::string path = import_path;
  if (!path.empty() && path.front() == '"')
    path.erase(0, 1);
  if (!path.empty() && path.back() == '"')
    path.pop_back();

  std::string module_name = module_name_from_import_path(path);

  bool is_stdlib_import = (!path.empty() && path[0] == '@');
  if (is_stdlib_import) {

    path = path.substr(1);

  }

  if (imported_files_.find(module_name) != imported_files_.end()) {
    return "";
  }
  imported_files_.insert(module_name);

  std::vector<std::string> candidates;
  if (is_stdlib_import) {

    std::vector<std::string> path_spellings = {path};
    if (!(path.size() >= 5 && path.substr(path.size() - 5) == ".risk"))
      path_spellings.push_back(path + ".risk");

    const char *env_stdlib_raw = getenv("RISK_STDLIB");
    if (env_stdlib_raw && env_stdlib_raw[0]) {
      std::string base = expand_tilde(env_stdlib_raw);
      if (!base.empty() && base.back() != '/' && base.back() != '\\')
        base += '/';
      for (auto &sp : path_spellings) {
        candidates.push_back(base + sp);

        std::string stem = sp;
        if (stem.size() >= 5 && stem.substr(stem.size() - 5) == ".risk")
          stem = stem.substr(0, stem.size() - 5);
        candidates.push_back(base + stem + "/" + sp);
      }
    }

    {
      std::string risk_lib = expand_tilde("~/.risk/lib/");
      if (risk_lib != "~/.risk/lib/") {
        for (auto &sp : path_spellings) {
          candidates.push_back(risk_lib + sp);
          std::string stem = sp;
          if (stem.size() >= 5 && stem.substr(stem.size() - 5) == ".risk")
            stem = stem.substr(0, stem.size() - 5);
          candidates.push_back(risk_lib + stem + "/" + sp);
        }
      }
    }

    for (auto &sp : path_spellings) {
      candidates.push_back("./stdlib/" + sp);
      std::string stem = sp;
      if (stem.size() >= 5 && stem.substr(stem.size() - 5) == ".risk")
        stem = stem.substr(0, stem.size() - 5);
      candidates.push_back("./stdlib/" + stem + "/" + sp);
      if (!source_dir.empty()) {
        candidates.push_back(source_dir + "stdlib/" + sp);
        candidates.push_back(source_dir + "stdlib/" + stem + "/" + sp);
      }
    }
  } else {

    if (!source_dir.empty())
      candidates.push_back(source_dir + path);
    candidates.push_back("./" + path);
  }

  std::ifstream in;
  std::string header_path;
  for (auto &candidate : candidates) {
    if (!path_is_regular_file(candidate))
      continue;
    in.open(candidate, std::ios::in | std::ios::binary);
    if (in) {
      header_path = candidate;
      break;
    }
  }
  if (!in) {
    if (is_stdlib_import) {
      return "unknown standard library '" + path + "'";
    }
    return "cannot find '" + path + "'";
  }

  std::ostringstream ss;
  ss << in.rdbuf();
  std::string src = ss.str();

  if (src.empty()) {
    return "";
  }

  Lexer lexer(src, header_path);
  auto tokens = lexer.tokenize();

  bool is_header = false;
  for (size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i].type == TokenType::Comment)
      continue;
    if (tokens[i].type == TokenType::HeaderFileMarker) {
      is_header = true;
      tokens.erase(tokens.begin() + i);
    }
    break;
  }

  Parser parser(tokens);
  auto header_tu = parser.parse_translation_unit();
  if (!header_tu) {
    return "";
  }

  {
    size_t slash = header_path.find_last_of("/\\");
    std::string dir = slash == std::string::npos ? "" : header_path.substr(0, slash + 1);
    if (!dir.empty())
      imported_dirs.push_back(dir);
  }

  for (auto &dptr : header_tu->decls) {

    if (auto im = dynamic_cast<ImportDecl *>(dptr.get())) {
      load_imported_header(im->path, source_dir);
      continue;
    }

    if (!is_header) {

      if (auto f = dynamic_cast<FuncDecl *>(dptr.get())) {
        if (f->is_extern) {
          size_t slash = header_path.find_last_of("/\\");
          f->extern_search_dir = slash == std::string::npos
                                     ? ""
                                     : header_path.substr(0, slash + 1);
        }
      }
      if (auto b = dynamic_cast<BlockDecl *>(dptr.get())) {
        if (b->is_extern) {
          size_t slash = header_path.find_last_of("/\\");
          b->extern_search_dir = slash == std::string::npos
                                     ? ""
                                     : header_path.substr(0, slash + 1);
        }
      }
      if (auto o = dynamic_cast<OptDecl *>(dptr.get())) {
        if (o->is_extern) {
          size_t slash = header_path.find_last_of("/\\");
          o->extern_search_dir = slash == std::string::npos
                                     ? ""
                                     : header_path.substr(0, slash + 1);
        }
      }
      if (auto gv = dynamic_cast<GlobalVarDecl *>(dptr.get())) {
        if (gv->is_extern) {
          size_t slash = header_path.find_last_of("/\\");
          gv->extern_search_dir = slash == std::string::npos
                                      ? ""
                                      : header_path.substr(0, slash + 1);
        }
      }
      if (current_tu)
        current_tu->decls.push_back(std::move(dptr));
      continue;
    }

    if (auto f = dynamic_cast<FuncDecl *>(dptr.get())) {
      std::vector<TypeInfo> params;
      for (auto &p : f->params)
        params.emplace_back(type_from_typenode(p.type.get()));
      TypeInfo ret = f->return_type ? type_from_typenode(f->return_type.get())
                                    : TypeInfo("void");
      if (functions.find(f->name) == functions.end()) {
        add_func(f->name, params, ret, f->line, f->col, nullptr);
      }
    } else if (auto b = dynamic_cast<OptDecl *>(dptr.get())) {
      register_opt(b);
    } else if (auto b = dynamic_cast<BlockDecl *>(dptr.get())) {
      register_block(b);
    }
  }

  return "";
}

void TypeChecker::push_scope() { var_scopes.emplace_back(); }
void TypeChecker::pop_scope() {
  if (var_scopes.empty())
    return;

  for (auto &[name, _sym] : var_scopes.back())
    freed_ptrs.erase(name);
  var_scopes.pop_back();
}

bool TypeChecker::block_has_owned_ptr_fields(const std::string &block_name) {
  auto it = blocks.find(block_name);
  if (it == blocks.end())
    return false;
  for (auto &[_fname, ftype] : it->second) {
    if (ftype.name.rfind("ptr<", 0) == 0 || ftype.name.rfind("refc<", 0) == 0)
      return true;
  }
  return false;
}

bool TypeChecker::add_var(const std::string &name, const TypeInfo &t,
                          bool is_const, int line, int col) {
  if (var_scopes.empty())
    push_scope();
  auto &scope = var_scopes.back();
  if (scope.find(name) != scope.end()) {
    diags.push_back({Diagnostic::Severity::Error,
                     "variable redefinition: " + name, line, col, col});
    return false;
  }
  scope[name] = VarSymbol{name, t, is_const, line, col};
  return true;
}

bool TypeChecker::add_func(const std::string &name,
                           const std::vector<TypeInfo> &params,
                           const TypeInfo &ret, int line, int col,
                           FuncDecl *decl) {
  if (functions.find(name) != functions.end()) {
    diags.push_back({Diagnostic::Severity::Error,
                     "function redefinition/duplicate: " + name, line, col,
                     col});
    return false;
  }
  functions[name] = FuncSymbol{name, params, ret, line, col, decl};
  return true;
}

std::optional<TypeChecker::VarSymbol>
TypeChecker::lookup_var(const std::string &name) {
  for (auto it = var_scopes.rbegin(); it != var_scopes.rend(); ++it) {
    auto &scope = *it;
    auto f = scope.find(name);
    if (f != scope.end())
      return f->second;
  }
  return {};
}

std::optional<TypeChecker::FuncSymbol>
TypeChecker::lookup_func(const std::string &name) {
  auto f = functions.find(name);
  if (f != functions.end())
    return f->second;
  return {};
}
void TypeChecker::register_type_alias(TypeAliasDecl *ta) {
  if (!ta)
    return;

  if (type_aliases.find(ta->name) != type_aliases.end()) {
    diags.push_back({Diagnostic::Severity::Error,
                     "type alias '" + ta->name + "' already defined", ta->line,
                     ta->col, ta->col});
    return;
  }

  TypeInfo target = type_from_typenode(ta->target_type.get());
  if (target.name == "__err_type__") {
    diags.push_back(
        {Diagnostic::Severity::Error,
         "invalid type in alias definition: " + ta->target_type->name, ta->line,
         ta->col, ta->col});
    return;
  }

  type_aliases[ta->name] = target.name;
}

std::string TypeChecker::resolve_type_alias(const std::string &name) {
  auto it = type_aliases.find(name);
  if (it != type_aliases.end()) {
    return it->second;
  }
  return name;
}

bool TypeChecker::register_block(BlockDecl *b) {
  if (!b)
    return false;
  if (blocks.find(b->name) != blocks.end() ||
      generic_block_templates.find(b->name) != generic_block_templates.end()) {
    diags.push_back({Diagnostic::Severity::Error,
                     "duplicate block declaration: " + b->name, b->line, b->col,
                     b->col});
    return false;
  }

  if (!b->generic_params.empty()) {

    for (auto &gp : b->generic_params) {
      if (gp.kind == GenericParamKind::Type && opts.count(gp.constraint_set)) {
        gp.kind = GenericParamKind::ValueOpt;
        gp.is_value = true;
      }
    }

    generic_block_templates[b->name] = b;
    return true;
  }

  blocks[b->name] = {};
  block_decls[b->name] = b;

  std::vector<std::pair<std::string, TypeInfo>> fields;
  for (auto &f : b->fields) {
    TypeInfo ft = type_from_typenode(f.type.get());
    if (ft.name == "__err_type__") {
      diags.push_back({Diagnostic::Severity::Error,
                       "block '" + b->name + "': field '" + f.name +
                           "' has unknown type '" + f.type->name + "'",
                       b->line, b->col, b->col});
    }
    fields.emplace_back(f.name, ft);
  }
  blocks[b->name] = std::move(fields);
  return true;
}

bool TypeChecker::register_opt(OptDecl *b) {
  if (!b)
    return false;
  if (opts.find(b->name) != opts.end()) {
    diags.push_back({Diagnostic::Severity::Error,
                     "duplicate opt declaration: " + b->name, b->line, b->col,
                     b->col});
    return false;
  }

  std::vector<std::pair<std::string, int>> values;
  for (auto &f : b->fields) {
    int value =
        f.value ? *f.value : 0;
    values.emplace_back(f.name, value);
  }
  opts[b->name] = std::move(values);
  return true;
}
std::optional<int>
TypeChecker::lookup_opt_field_type(const std::string &optname,
                                   const std::string &field) {
  auto it = opts.find(optname);
  if (it == opts.end())
    return {};
  for (auto &p : it->second) {
    if (p.first == field)
      return p.second;
  }
  return {};
}

std::optional<TypeInfo>
TypeChecker::lookup_block_field_type(const std::string &blockname,
                                     const std::string &field) {
  auto it = blocks.find(blockname);
  if (it == blocks.end())
    return {};
  for (auto &p : it->second) {
    if (p.first == field)
      return p.second;
  }
  return {};
}

TypeInfo TypeChecker::type_from_typenode(TypeNode *tn) {
  if (!tn)
    return TypeInfo("void");

  auto alias_it = type_aliases.find(tn->name);
  if (alias_it != type_aliases.end()) {

    return TypeInfo(alias_it->second);
  }
  std::string resolved = resolve_type_alias(tn->name);
  if (resolved != tn->name) {

    if (!tn->params.empty()) {

      return TypeInfo("__err_type__");
    }
    return TypeInfo(resolved);
  }
  if (generic_block_templates.find(tn->name) != generic_block_templates.end()) {
    return resolve_generic_block_type(tn);
  }
  if (blocks.find(tn->name) != blocks.end()) {
    std::string s = tn->name;
    if (!tn->params.empty()) {
      s += "<";
      for (size_t i = 0; i < tn->params.size(); ++i) {
        if (i)
          s += ",";
        s += type_from_typenode(tn->params[i].get()).name;
      }
      s += ">";
    }

    return TypeInfo(s);
  }
  if (tn->name == "func") {
    std::string s = "func(";
    for (size_t i = 0; i < tn->params.size(); ++i) {
      if (i)
        s += ",";
      s += type_from_typenode(tn->params[i].get()).name;
    }
    s += ")->";
    if (tn->return_type)
      s += type_from_typenode(tn->return_type.get()).name;
    else
      s += "void";
    return TypeInfo(s);
  }
  std::string s = tn->name;
  if (!tn->params.empty()) {
    s += "<";
    for (size_t i = 0; i < tn->params.size(); ++i) {
      if (i)
        s += ",";
      s += type_from_typenode(tn->params[i].get()).name;
    }
    s += ">";
  }

  static const std::unordered_set<std::string> primitives = {
      "i8",   "i16", "i32", "i64",  "u8",     "u16",     "u32",
      "u64",  "f64", "f32", "bool", "string", "void",    "null",
      "char", "ptr", "ref", "opt",  "Array",  "Hashmap", "const",
      "refc", "Arena"};

  if (tn->name.rfind("__size__:", 0) == 0) {

    return TypeInfo(tn->name);
  }
  if (primitives.find(tn->name) == primitives.end() &&
      blocks.find(tn->name) == blocks.end() &&
      opts.find(tn->name) == opts.end()) {
    return TypeInfo("__err_type__");
  }
  return TypeInfo(s);
}

TypeInfo TypeChecker::type_of_expr(Expr *e) {
  if (!e)
    return TypeInfo("__err__");

  if (auto b = dynamic_cast<BoolExpr *>(e)) {
    TypeInfo ti("bool");
    expr_type_map[e] = ti;
    return ti;
  }

  if (auto n = dynamic_cast<NullExpr *>(e)) {
    TypeInfo ti("null");
    expr_type_map[e] = ti;
    return ti;
  }

  if (auto id = dynamic_cast<IdentifierExpr *>(e)) {
    if (id->name == "null") {
      TypeInfo ti("null");
      expr_type_map[e] = ti;
      return ti;
    }

    {
      auto fi = freed_ptrs.find(id->name);
      if (fi != freed_ptrs.end()) {
        diags.push_back({Diagnostic::Severity::Error,
                         "use of '" + id->name +
                             "' after it was freed/released (dealloc or "
                             "rcRelease was called on line " +
                             std::to_string(fi->second.line) + ")",
                         id->line, id->col,
                         id->end_col ? id->end_col : id->col});
        TypeInfo err("__err__");
        expr_type_map[e] = err;
        return err;
      }
    }
    auto v = lookup_var(id->name);
    if (v) {

      std::string resolved_type = v->type.name;
      auto alias_it = type_aliases.find(v->type.name);
      if (alias_it != type_aliases.end()) {
        resolved_type = alias_it->second;
      }
      TypeInfo resolved(resolved_type);
      expr_type_map[e] = resolved;
      return resolved;
    }
    auto fn = lookup_func(id->name);
    if (fn) {
      if (fn->decl && fn->decl->is_extern) {
        diags.push_back(
            {Diagnostic::Severity::Error,
             "'" + id->name +
                 "' is an extern C function and can only be directly "
                 "called (e.g. '" + id->name +
                 "(...)'), not used as a function value",
             id->line, id->col, id->end_col ? id->end_col : id->col});
        TypeInfo err("__err__");
        expr_type_map[e] = err;
        return err;
      }
      std::string sig = "func(";
      for (size_t i = 0; i < fn->params.size(); ++i) {
        if (i)
          sig += ",";
        sig += fn->params[i].name;
      }
      sig += ")->" + fn->ret.name;
      TypeInfo ti(sig);
      expr_type_map[e] = ti;
      return ti;
    }

    for (auto &opt_entry : opts) {
      const std::string &opt_name = opt_entry.first;
      const std::vector<std::pair<std::string, int>> &opt_values =
          opt_entry.second;
      for (auto &value_pair : opt_values) {
        if (value_pair.first == id->name) {

          TypeInfo ti(opt_name);
          expr_type_map[e] = ti;
          return ti;
        }
      }
    }

    diags.push_back({Diagnostic::Severity::Error,
                     "unknown identifier: " + id->name, id->line, id->col,
                     id->end_col ? id->end_col : id->col});
    TypeInfo err("__err__");
    expr_type_map[e] = err;
    return err;
  }

  if (auto num = dynamic_cast<NumberExpr *>(e)) {
    TypeInfo ti = (num->literal.find('.') != std::string::npos)
                      ? TypeInfo("f64")
                      : TypeInfo("i32");
    expr_type_map[e] = ti;
    return ti;
  }

  if (auto s = dynamic_cast<StringExpr *>(e)) {
    TypeInfo ti("string");
    expr_type_map[e] = ti;
    return ti;
  }

  if (auto c = dynamic_cast<CharExpr *>(e)) {
    TypeInfo ti("char");
    expr_type_map[e] = ti;
    return ti;
  }

  if (auto mem = dynamic_cast<MemberExpr *>(e)) {

    if (auto obj_id = dynamic_cast<IdentifierExpr *>(mem->object.get())) {
      if (imported_aliases.find(obj_id->name) != imported_aliases.end()) {

        TypeInfo ns_fn("__namespace_function__");
        expr_type_map[e] = ns_fn;
        return ns_fn;
      }

      if (opts.find(obj_id->name) != opts.end()) {

        auto field_val = lookup_opt_field_type(obj_id->name, mem->field);
        if (!field_val) {
          diags.push_back(
              {Diagnostic::Severity::Error,
               "unknown field '" + mem->field + "' on opt " + obj_id->name,
               mem->line, mem->col, mem->end_col ? mem->end_col : mem->col});
          TypeInfo err("__err__");
          expr_type_map[e] = err;
          return err;
        }

        TypeInfo ti(obj_id->name);
        expr_type_map[e] = ti;
        return ti;
      }
    }

    TypeInfo ot = type_of_expr(mem->object.get());

    if (ot.name.rfind("Array<", 0) == 0) {
      std::string inner = extract_inner_type_name(ot.name);
      if (mem->field == "push") {
        TypeInfo fn("func(" + inner + ") -> void");
        expr_type_map[e] = fn;
        return fn;
      } else if (mem->field == "pop") {
        TypeInfo fn("func() -> " + inner);
        expr_type_map[e] = fn;
        return fn;
      } else if (mem->field == "pushFront") {
        TypeInfo fn("func(" + inner + ") -> void");
        expr_type_map[e] = fn;
        return fn;
      } else if (mem->field == "popFront") {
        TypeInfo fn("func() -> " + inner);
        expr_type_map[e] = fn;
        return fn;
      } else if (mem->field == "insert") {

        TypeInfo fn("func(i32, " + inner + ") -> void");
        expr_type_map[e] = fn;
        return fn;
      } else if (mem->field == "removeAt") {

        TypeInfo fn("func(i32) -> void");
        expr_type_map[e] = fn;
        return fn;
      } else if (mem->field == "clear") {

        TypeInfo fn("func() -> void");
        expr_type_map[e] = fn;
        return fn;
      } else if (mem->field == "repr") {

        TypeInfo fn("func(bool) -> string");
        expr_type_map[e] = fn;
        return fn;
      } else if (mem->field == "freeAll") {
        TypeInfo fn("func() -> void");
        expr_type_map[e] = fn;
        return fn;
      } else if (mem->field == "destroy") {
        TypeInfo fn("func() -> void");
        expr_type_map[e] = fn;
        return fn;
      } else {
        diags.push_back({Diagnostic::Severity::Error,
                         "unknown field '" + mem->field + "' on Array type",
                         mem->line, mem->col,
                         mem->end_col ? mem->end_col : mem->col});
        TypeInfo err("__err__");
        expr_type_map[e] = err;
        return err;
      }
    }

    if (starts_with(ot.name, "Hashmap<")) {
      std::string key_type, value_type;
      extract_hashmap_kv(ot.name, key_type, value_type);
      if (mem->field == "has" || mem->field == "contains") {
        TypeInfo fn("func(" + key_type + ") -> bool");
        expr_type_map[e] = fn;
        return fn;
      } else if (mem->field == "size") {
        TypeInfo fn("func() -> i32");
        expr_type_map[e] = fn;
        return fn;
      } else if (mem->field == "remove" || mem->field == "delete") {
        TypeInfo fn("func(" + key_type + ") -> void");
        expr_type_map[e] = fn;
        return fn;
      } else if (mem->field == "clear") {
        TypeInfo fn("func() -> void");
        expr_type_map[e] = fn;
        return fn;
      }
    }

    if (ot.name == "Arena") {
      if (mem->field == "alloc") {
        TypeInfo fn("func(u64) -> ptr<void>");
        expr_type_map[e] = fn;
        return fn;
      } else if (mem->field == "reset" || mem->field == "destroy") {
        TypeInfo fn("func() -> void");
        expr_type_map[e] = fn;
        return fn;
      } else if (mem->field == "used") {
        TypeInfo fn("func() -> u64");
        expr_type_map[e] = fn;
        return fn;
      } else if (mem->field == "KiB" || mem->field == "MiB" ||
                 mem->field == "GiB") {
        TypeInfo fn("func() -> f64");
        expr_type_map[e] = fn;
        return fn;
      } else if (mem->field == "capacityBytes") {
        TypeInfo fn("func() -> u64");
        expr_type_map[e] = fn;
        return fn;
      } else if (mem->field == "capacityKiB" || mem->field == "capacityMiB" ||
                 mem->field == "capacityGiB") {
        TypeInfo fn("func() -> f64");
        expr_type_map[e] = fn;
        return fn;
      } else if (mem->field == "scratchBegin") {
        TypeInfo fn("func() -> ptr<void>");
        expr_type_map[e] = fn;
        return fn;
      } else if (mem->field == "scratchEnd") {
        TypeInfo fn("func(ptr<void>) -> void");
        expr_type_map[e] = fn;
        return fn;
      }
    }

    auto extract = [](const std::string &nm) -> std::string {
      size_t p = nm.find('<');
      size_t q = nm.rfind('>');
      if (p != std::string::npos && q != std::string::npos && q > p + 1)
        return nm.substr(p + 1, q - p - 1);
      return "";
    };
    std::string blockname;
    if (starts_with(ot.name, "ref<") || starts_with(ot.name, "ptr<"))
      blockname = extract(ot.name);
    else
      blockname = ot.name;

    for (;;) {
      if (starts_with(blockname, "const<") && !blockname.empty() &&
          blockname.back() == '>') {
        blockname = blockname.substr(6, blockname.size() - 7);
        continue;
      }
      if ((starts_with(blockname, "ref<") || starts_with(blockname, "ptr<")) &&
          !blockname.empty() && blockname.back() == '>') {
        std::string inner = extract(blockname);
        if (inner.empty())
          break;
        blockname = inner;
        continue;
      }
      break;
    }
    if (blockname.empty()) {
      diags.push_back({Diagnostic::Severity::Error,
                       "member access on non-aggregate type: " + ot.name,
                       mem->line, mem->col,
                       mem->end_col ? mem->end_col : mem->col});
      TypeInfo err("__err__");
      expr_type_map[e] = err;
      return err;
    }
    auto ft = lookup_block_field_type(blockname, mem->field);
    if (!ft) {
      diags.push_back(
          {Diagnostic::Severity::Error,
           "unknown field '" + mem->field + "' on block " + blockname,
           mem->line, mem->col, mem->end_col ? mem->end_col : mem->col});
      TypeInfo err("__err__");
      expr_type_map[e] = err;
      return err;
    }
    expr_type_map[e] = *ft;
    return *ft;
  }

  if (auto bl = dynamic_cast<BlockLiteralExpr *>(e)) {

    auto it = expr_type_map.find(e);
    if (it != expr_type_map.end()) {
      return it->second;
    }

    TypeInfo ti("__block_literal__");
    expr_type_map[e] = ti;
    return ti;
  }

  if (auto al = dynamic_cast<ArrayLiteralExpr *>(e)) {

    auto existing = expr_type_map.find(e);
    if (existing != expr_type_map.end() &&
        existing->second.name != "__array_literal__") {
      return existing->second;
    }

    if (!al->elements.empty()) {

      TypeInfo first_type = type_of_expr(al->elements[0].get());

      if (dynamic_cast<ArrayLiteralExpr *>(al->elements[0].get())) {

        TypeInfo ti("Array<" + first_type.name + ">");
        expr_type_map[e] = ti;
        return ti;
      } else if (first_type.name != "__err__" &&
                 first_type.name != "__array_literal__") {

        TypeInfo ti("Array<" + first_type.name + ">");
        expr_type_map[e] = ti;
        return ti;
      }
    }

    TypeInfo ti("__array_literal__");
    expr_type_map[e] = ti;
    return ti;
  }

  if (auto cast = dynamic_cast<CastExpr *>(e)) {
    TypeInfo tgt = type_from_typenode(cast->target.get());
    TypeInfo src = type_of_expr(cast->expr.get());

    auto is_numeric = [](const std::string &n) {
      return n == "i32" || n == "i64" || n == "f32" || n == "f64" ||
             n == "i8" || n == "i16" || n == "u8" || n == "u16" || n == "u32" ||
             n == "u64";
    };

    if (src.name == "__err__") {
      expr_type_map[e] = src;
      return src;
    }

    if (cast->is_unsafe) {

      diags.push_back({Diagnostic::Severity::Warning,
                       "unsafe_cast from " + src.name + " to " + tgt.name +
                           " - this may cause undefined behavior",
                       cast->line, cast->col,
                       cast->end_col ? cast->end_col : cast->col});
      expr_type_map[e] = tgt;
      return tgt;
    }

    if (opts.find(tgt.name) != opts.end() && is_numeric(src.name)) {
      expr_type_map[e] = tgt;
      return tgt;
    }
    if (opts.find(src.name) != opts.end() && is_numeric(tgt.name)) {
      expr_type_map[e] = tgt;
      return tgt;
    }

    if (tgt.name == src.name) {
      expr_type_map[e] = tgt;
      return tgt;
    }

    std::string src_base = src.name;
    if (src_base.rfind("const<", 0) == 0) {
      size_t p = src_base.find('<'), q = src_base.rfind('>');
      if (p != std::string::npos && q != std::string::npos && q > p + 1)
        src_base = src_base.substr(p + 1, q - p - 1);
    }

    std::string tgt_base = tgt.name;
    if (tgt_base.rfind("const<", 0) == 0) {
      size_t p = tgt_base.find('<'), q = tgt_base.rfind('>');
      if (p != std::string::npos && q != std::string::npos && q > p + 1)
        tgt_base = tgt_base.substr(p + 1, q - p - 1);
    }

    if (src_base == tgt_base) {
      expr_type_map[e] = tgt;
      return tgt;
    }

    if (is_numeric(src_base) && is_numeric(tgt_base)) {
      expr_type_map[e] = tgt;
      return tgt;
    }

    if (is_numeric(src.name) && is_numeric(tgt.name)) {
      expr_type_map[e] = tgt;
      return tgt;
    }

    if (src.name == "bool" && is_numeric(tgt.name)) {
      expr_type_map[e] = tgt;
      return tgt;
    }

    if ((src.name == "string" && tgt.name == "char") ||
        (src.name == "char" && tgt.name == "string")) {
      expr_type_map[e] = tgt;
      return tgt;
    }

    if ((src.name == "char" && (tgt.name == "i32" || tgt.name == "i64" ||
                                tgt.name == "u8" || tgt.name == "u32")) ||
        ((src.name == "i32" || src.name == "i64" || src.name == "u8" ||
          src.name == "u32") &&
         tgt.name == "char")) {
      expr_type_map[e] = tgt;
      return tgt;
    }

    if (tgt.name.rfind("ptr<", 0) == 0 || src.name.rfind("ptr<", 0) == 0 ||
        tgt.name.rfind("ref<", 0) == 0 || src.name.rfind("ref<", 0) == 0 ||
        tgt.name.rfind("opt<", 0) == 0 || src.name.rfind("opt<", 0) == 0 ||
        tgt.name.rfind("Array<", 0) == 0 || src.name.rfind("Array<", 0) == 0 ||
        tgt.name.rfind("Hashmap<", 0) == 0 ||
        src.name.rfind("Hashmap<", 0) == 0) {
      expr_type_map[e] = tgt;
      return tgt;
    }

    diags.push_back({Diagnostic::Severity::Error,
                     "unsupported cast from " + src.name + " to " + tgt.name,
                     cast->line, cast->col,
                     cast->end_col ? cast->end_col : cast->col});
    TypeInfo err("__err__");
    expr_type_map[e] = err;
    return err;
  }

  if (auto un = dynamic_cast<UnaryExpr *>(e)) {
    if (un->op == "&") {
      if (auto id = dynamic_cast<IdentifierExpr *>(un->rhs.get())) {
        auto v = lookup_var(id->name);
        if (v) {

          std::string inner = v->type.name;
          if (v->is_const && inner.rfind("const<", 0) != 0 &&
              inner.rfind("const ", 0) != 0) {
            inner = "const<" + inner + ">";
          }
          TypeInfo res("ptr<" + inner + ">");
          expr_type_map[e] = res;
          return res;
        }
      }
      TypeInfo rt = type_of_expr(un->rhs.get());
      if (rt.name == "__err__") {
        expr_type_map[e] = rt;
        return rt;
      }
      TypeInfo res("ptr<" + rt.name + ">");
      expr_type_map[e] = res;
      return res;
    }

    if (un->op == "*") {
      TypeInfo rt = type_of_expr(un->rhs.get());

      if (rt.name == "__err__") {
        expr_type_map[e] = rt;
        return rt;
      }

      if (auto null_expr = dynamic_cast<NullExpr *>(un->rhs.get())) {
        diags.push_back({Diagnostic::Severity::Error,
                         "dereferencing null pointer (will cause crash)",
                         un->line, un->col,
                         un->end_col ? un->end_col : un->col});
        TypeInfo err("__err__");
        expr_type_map[e] = err;
        return err;
      }

      if (auto id = dynamic_cast<IdentifierExpr *>(un->rhs.get())) {
        if (id->name == "null") {
          diags.push_back({Diagnostic::Severity::Error,
                           "dereferencing null pointer (will cause crash)",
                           un->line, un->col,
                           un->end_col ? un->end_col : un->col});
          TypeInfo err("__err__");
          expr_type_map[e] = err;
          return err;
        }
      }

      auto get_inner = [](const std::string &nm) -> std::string {
        size_t p = nm.find('<');
        size_t q = nm.rfind('>');
        if (p != std::string::npos && q != std::string::npos && q > p + 1)
          return nm.substr(p + 1, q - p - 1);
        return "";
      };

      std::string deref_type = rt.name;
      if (deref_type.rfind("const<", 0) == 0) {
        std::string stripped = get_inner(deref_type);
        if (!stripped.empty())
          deref_type = stripped;
      }
      if (starts_with(deref_type, "ptr<") || starts_with(deref_type, "ref<")) {
        std::string inner = get_inner(deref_type);
        if (!inner.empty()) {

          TypeInfo res(inner);
          expr_type_map[e] = res;
          return res;
        }
      }

      if (rt.name != "__err__") {
        diags.push_back(
            {Diagnostic::Severity::Error,
             "cannot dereference non-pointer type '" + rt.name + "'", un->line,
             un->col, un->end_col ? un->end_col : un->col});
      }

      expr_type_map[e] = rt;
      return rt;
    }

    if (un->op == "++" || un->op == "--") {

      if (auto id = dynamic_cast<IdentifierExpr *>(un->rhs.get())) {
        auto v = lookup_var(id->name);
        if (!v) {
          diags.push_back({Diagnostic::Severity::Error,
                           "unknown identifier in " + un->op + ": " + id->name,
                           un->line, un->col,
                           un->end_col ? un->end_col : un->col});
          TypeInfo err("__err__");
          expr_type_map[e] = err;
          return err;
        }

        if (v->is_const) {
          diags.push_back({Diagnostic::Severity::Error,
                           "cannot " + un->op + " '" + id->name +
                               "': variable is declared const",
                           un->line, un->col,
                           un->end_col ? un->end_col : un->col});
          TypeInfo err("__err__");
          expr_type_map[e] = err;
          return err;
        }
        if (v->type.name.rfind("const<", 0) == 0) {
          diags.push_back({Diagnostic::Severity::Error,
                           "cannot " + un->op + " '" + id->name +
                               "': variable has a const type",
                           un->line, un->col,
                           un->end_col ? un->end_col : un->col});
          TypeInfo err("__err__");
          expr_type_map[e] = err;
          return err;
        }

        if (is_ref_type_name(v->type.name)) {
          std::string inner = extract_inner_type_name(v->type.name);
          if (inner.empty()) {
            diags.push_back({Diagnostic::Severity::Error,
                             "ref type has no inner type for " + un->op,
                             un->line, un->col,
                             un->end_col ? un->end_col : un->col});
            TypeInfo err("__err__");
            expr_type_map[e] = err;
            return err;
          }
          if (!is_numeric_type_name(inner) && !is_ptr_type_name(inner)) {
            diags.push_back({Diagnostic::Severity::Error,
                             "cannot " + un->op + " referent of type " + inner,
                             un->line, un->col,
                             un->end_col ? un->end_col : un->col});
            TypeInfo err("__err__");
            expr_type_map[e] = err;
            return err;
          }
          TypeInfo res(inner);
          expr_type_map[e] = res;
          return res;
        } else {
          if (!is_numeric_type_name(v->type.name) &&
              !is_ptr_type_name(v->type.name)) {
            diags.push_back(
                {Diagnostic::Severity::Error,
                 "cannot " + un->op + " variable of type " + v->type.name,
                 un->line, un->col, un->end_col ? un->end_col : un->col});
            TypeInfo err("__err__");
            expr_type_map[e] = err;
            return err;
          }
          expr_type_map[e] = v->type;
          return v->type;
        }
      }

      TypeInfo lt = type_of_expr(un->rhs.get());
      if (lt.name == "__err__") {
        expr_type_map[e] = lt;
        return lt;
      }
      if (is_ref_type_name(lt.name)) {
        std::string inner = extract_inner_type_name(lt.name);
        if (inner.empty()) {
          diags.push_back({Diagnostic::Severity::Error,
                           "ref type has no inner type for " + un->op, un->line,
                           un->col, un->end_col ? un->end_col : un->col});
          TypeInfo err("__err__");
          expr_type_map[e] = err;
          return err;
        }
        if (!is_numeric_type_name(inner) && !is_ptr_type_name(inner)) {
          diags.push_back({Diagnostic::Severity::Error,
                           "cannot " + un->op + " referent of type " + inner,
                           un->line, un->col,
                           un->end_col ? un->end_col : un->col});
          TypeInfo err("__err__");
          expr_type_map[e] = err;
          return err;
        }
        TypeInfo res(inner);
        expr_type_map[e] = res;
        return res;
      }
      if (!is_numeric_type_name(lt.name) && !is_ptr_type_name(lt.name)) {
        diags.push_back({Diagnostic::Severity::Error,
                         "cannot " + un->op + " expression of type " + lt.name,
                         un->line, un->col,
                         un->end_col ? un->end_col : un->col});
        TypeInfo err("__err__");
        expr_type_map[e] = err;
        return err;
      }
      expr_type_map[e] = lt;
      return lt;
    }

    TypeInfo rt = type_of_expr(un->rhs.get());
    expr_type_map[e] = rt;
    return rt;
  }

  if (auto idx = dynamic_cast<IndexExpr *>(e)) {
    TypeInfo ot = type_of_expr(idx->object.get());

    if (starts_with(ot.name, "Array<")) {
      std::string inner = extract_inner_type_name(ot.name);
      if (inner.empty()) {
        diags.push_back({Diagnostic::Severity::Error,
                         "Array has no inner type for indexing", idx->line,
                         idx->col, idx->end_col ? idx->end_col : idx->col});
        TypeInfo err("__err__");
        expr_type_map[e] = err;
        return err;
      }

      type_of_expr(idx->index.get());
      TypeInfo res(inner);
      expr_type_map[e] = res;
      return res;
    }

    if (is_ptr_type_name(ot.name) || is_ref_type_name(ot.name)) {
      std::string inner = extract_inner_type_name(ot.name);
      if (inner.empty()) {
        diags.push_back({Diagnostic::Severity::Error,
                         "pointer/ref has no inner type for indexing",
                         idx->line, idx->col,
                         idx->end_col ? idx->end_col : idx->col});
        TypeInfo err("__err__");
        expr_type_map[e] = err;
        return err;
      }
      type_of_expr(idx->index.get());
      TypeInfo res(inner);
      expr_type_map[e] = res;
      return res;
    }

    if (starts_with(ot.name, "Hashmap<")) {
      size_t open = ot.name.find('<');
      size_t close = ot.name.rfind('>');
      if (open != std::string::npos && close != std::string::npos &&
          close > open) {
        std::string contents = ot.name.substr(open + 1, close - open - 1);
        size_t comma = contents.find(',');
        if (comma != std::string::npos) {
          std::string value_type = contents.substr(comma + 1);

          while (!value_type.empty() && value_type[0] == ' ')
            value_type = value_type.substr(1);
          if (!value_type.empty()) {
            type_of_expr(idx->index.get());
            TypeInfo res(value_type);
            expr_type_map[e] = res;
            return res;
          }
        }
      }
      diags.push_back({Diagnostic::Severity::Error,
                       "Hashmap has invalid type parameters", idx->line,
                       idx->col, idx->end_col ? idx->end_col : idx->col});
      TypeInfo err("__err__");
      expr_type_map[e] = err;
      return err;
    }
    diags.push_back({Diagnostic::Severity::Error,
                     "indexing not supported on type: " + ot.name, idx->line,
                     idx->col, idx->end_col ? idx->end_col : idx->col});
    TypeInfo err("__err__");
    expr_type_map[e] = err;
    return err;
  }

  if (auto bin = dynamic_cast<BinaryExpr *>(e)) {
    TypeInfo lt = type_of_expr(bin->lhs.get());

    if (bin->op == "=" || bin->op == "+=" || bin->op == "-=") {
      if (auto rbl = dynamic_cast<BlockLiteralExpr *>(bin->rhs.get()))
        stamp_block_literal(rbl, lt.name, rbl->line, rbl->col, "assignment");
    }
    TypeInfo rt = type_of_expr(bin->rhs.get());

    if (bin->op == "=") {

      if (rt.name.rfind("refc<", 0) == 0) {
        if (dynamic_cast<IdentifierExpr *>(bin->rhs.get())) {
          diags.push_back({Diagnostic::Severity::Warning,
                           "copying refc T without rcRetain() — both variables "
                           "will share ownership without incrementing the "
                           "refcount; use rcRetain() to get a second "
                           "owning handle, or rcGet() for a raw borrow",
                           bin->line, bin->col,
                           bin->end_col ? bin->end_col : bin->col});
        }
      }

      if (auto lhs_id = dynamic_cast<IdentifierExpr *>(bin->lhs.get())) {
        auto lhs_var = lookup_var(lhs_id->name);
        if (lhs_var && lhs_var->is_const) {
          diags.push_back({Diagnostic::Severity::Error,
                           "cannot assign to '" + lhs_id->name +
                               "': variable is declared const",
                           bin->line, bin->col,
                           bin->end_col ? bin->end_col : bin->col});
          TypeInfo err("__err__");
          expr_type_map[e] = err;
          return err;
        }

        if (lhs_var && (lhs_var->type.name.rfind("const<", 0) == 0)) {
          diags.push_back({Diagnostic::Severity::Error,
                           "cannot assign to '" + lhs_id->name +
                               "': variable has a const type ('" +
                               lhs_var->type.name + "')",
                           bin->line, bin->col,
                           bin->end_col ? bin->end_col : bin->col});
          TypeInfo err("__err__");
          expr_type_map[e] = err;
          return err;
        }
      }

      if (auto lhs_deref = dynamic_cast<UnaryExpr *>(bin->lhs.get())) {
        if (lhs_deref->op == "*") {

          std::string ptr_type_name;
          if (auto ptr_id =
                  dynamic_cast<IdentifierExpr *>(lhs_deref->rhs.get())) {
            auto v = lookup_var(ptr_id->name);
            if (v)
              ptr_type_name = v->type.name;
          } else {
            TypeInfo pt = type_of_expr(lhs_deref->rhs.get());
            ptr_type_name = pt.name;
          }

          std::string effective_ptr = ptr_type_name;
          if (effective_ptr.rfind("const<", 0) == 0) {
            size_t cp = effective_ptr.find('<');
            size_t cq = effective_ptr.rfind('>');
            if (cp != std::string::npos && cq != std::string::npos &&
                cq > cp + 1)
              effective_ptr = effective_ptr.substr(cp + 1, cq - cp - 1);
          }

          if (effective_ptr.rfind("ptr<", 0) == 0) {
            size_t p = effective_ptr.find('<');
            size_t q = effective_ptr.rfind('>');
            if (p != std::string::npos && q != std::string::npos && q > p + 1) {
              std::string inner = effective_ptr.substr(p + 1, q - p - 1);
              if (inner.rfind("const<", 0) == 0 ||
                  inner.rfind("const ", 0) == 0) {
                diags.push_back(
                    {Diagnostic::Severity::Error,
                     "cannot write through pointer to const data (pointer "
                     "type: '" +
                         ptr_type_name + "')",
                     bin->line, bin->col,
                     bin->end_col ? bin->end_col : bin->col});
                TypeInfo err("__err__");
                expr_type_map[e] = err;
                return err;
              }
            }
          }
        }
      }

      if (is_ref_type_name(lt.name)) {
        std::string inner = extract_inner_type_name(lt.name);
        if (inner.empty()) {
          diags.push_back({Diagnostic::Severity::Error,
                           "ref type has no inner type for assignment",
                           bin->line, bin->col, bin->col});
          TypeInfo err("__err__");
          expr_type_map[e] = err;
          return err;
        }

        if (rt.name == inner) {
          TypeInfo res(inner);
          expr_type_map[e] = res;
          return res;
        }

        if (is_numeric_type_name(rt.name) && is_numeric_type_name(inner)) {
          if (auto num = dynamic_cast<NumberExpr *>(bin->rhs.get())) {
            if (num->literal.find('.') == std::string::npos) {
              try {
                long long v = std::stoll(num->literal);
                bool fits = false;
                if (inner == "i8") {
                  fits = (v >= std::numeric_limits<int8_t>::min() &&
                          v <= std::numeric_limits<int8_t>::max());
                } else if (inner == "i16") {
                  fits = (v >= std::numeric_limits<int16_t>::min() &&
                          v <= std::numeric_limits<int16_t>::max());
                } else if (inner == "i32") {
                  fits = (v >= std::numeric_limits<int32_t>::min() &&
                          v <= std::numeric_limits<int32_t>::max());
                } else if (inner == "i64") {
                  fits = (v >= std::numeric_limits<int64_t>::min() &&
                          v <= std::numeric_limits<int64_t>::max());
                } else if (inner == "u8") {
                  fits = (v >= 0 && static_cast<unsigned long long>(v) <=
                                        std::numeric_limits<uint8_t>::max());
                } else if (inner == "u16") {
                  fits = (v >= 0 && static_cast<unsigned long long>(v) <=
                                        std::numeric_limits<uint16_t>::max());
                } else if (inner == "u32") {
                  fits = (v >= 0 && static_cast<unsigned long long>(v) <=
                                        std::numeric_limits<uint32_t>::max());
                } else if (inner == "u64") {
                  fits = (v >= 0 && static_cast<unsigned long long>(v) <=
                                        std::numeric_limits<uint64_t>::max());
                }
                if (fits) {
                  TypeInfo res(inner);
                  expr_type_map[e] = res;
                  return res;
                }
              } catch (...) {
              }
            }
          }
        }
        diags.push_back({Diagnostic::Severity::Error,
                         "assignment to referent: RHS type " + rt.name +
                             " not compatible with referent " + inner,
                         bin->line, bin->col, bin->col});
        TypeInfo err("__err__");
        expr_type_map[e] = err;
        return err;
      }

      if (starts_with(lt.name, "ptr<") && rt.name == "null") {
        expr_type_map[e] = lt;
        return lt;
      }

      if (starts_with(lt.name, "Array<") && rt.name == "__array_literal__") {
        expr_type_map[bin->rhs.get()] = lt;
        expr_type_map[e] = lt;
        return lt;
      }

    }

    {
      static const std::unordered_set<std::string> compound_assigns = {
          "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^="};
      if (compound_assigns.count(bin->op)) {
        if (auto lhs_id = dynamic_cast<IdentifierExpr *>(bin->lhs.get())) {
          auto lhs_var = lookup_var(lhs_id->name);
          if (lhs_var && lhs_var->is_const) {
            diags.push_back({Diagnostic::Severity::Error,
                             "cannot assign to '" + lhs_id->name +
                                 "': variable is declared const",
                             bin->line, bin->col,
                             bin->end_col ? bin->end_col : bin->col});
            TypeInfo err("__err__");
            expr_type_map[e] = err;
            return err;
          }
          if (lhs_var && lhs_var->type.name.rfind("const<", 0) == 0) {
            diags.push_back({Diagnostic::Severity::Error,
                             "cannot assign to '" + lhs_id->name +
                                 "': variable has a const type ('" +
                                 lhs_var->type.name + "')",
                             bin->line, bin->col,
                             bin->end_col ? bin->end_col : bin->col});
            TypeInfo err("__err__");
            expr_type_map[e] = err;
            return err;
          }
        }
      }
    }

    if ((bin->op == "+" || bin->op == "-")) {
      if (starts_with(lt.name, "ptr<") && is_numeric_type_name(rt.name)) {
        expr_type_map[e] = lt;
        return lt;
      }
      if (starts_with(rt.name, "ptr<") && is_numeric_type_name(lt.name) &&
          bin->op == "+") {
        expr_type_map[e] = rt;
        return rt;
      }
    }

    if (starts_with(lt.name, "ptr<") || starts_with(rt.name, "ptr<")) {
      if (bin->op == "*" || bin->op == "/" || bin->op == "%" ||
          bin->op == "&" || bin->op == "|" || bin->op == "^" ||
          bin->op == "<<" || bin->op == ">>") {
        diags.push_back({Diagnostic::Severity::Error,
                         "invalid operation '" + bin->op + "' on pointer type",
                         bin->line, bin->col,
                         bin->end_col ? bin->end_col : bin->col});
        TypeInfo err("__err__");
        expr_type_map[e] = err;
        return err;
      }
    }

    auto unwrap_numeric = [](const std::string &nm) -> std::string {
      if (nm.rfind("ref<", 0) == 0 || nm.rfind("const<", 0) == 0 ||
          nm.rfind("refc<", 0) == 0) {
        size_t p = nm.find('<'), q = nm.rfind('>');
        if (p != std::string::npos && q != std::string::npos && q > p + 1)
          return nm.substr(p + 1, q - p - 1);
      }
      return nm;
    };
    std::string lt_base = unwrap_numeric(lt.name);
    std::string rt_base = unwrap_numeric(rt.name);

    if (is_numeric_type_name(lt_base) && is_numeric_type_name(rt_base)) {

      if (bin->op == "/" || bin->op == "%" || bin->op == "/=" ||
          bin->op == "%=") {
        if (auto rhs_num = dynamic_cast<NumberExpr *>(bin->rhs.get())) {
          bool is_zero = false;
          try {
            is_zero = (std::stod(rhs_num->literal) == 0.0);
          } catch (...) {
          }
          if (is_zero) {
            diags.push_back({Diagnostic::Severity::Warning, "division by zero",
                             bin->line, bin->col,
                             bin->end_col ? bin->end_col : bin->col});
          }
        }
      }
      bool is_cmp = (bin->op == "<" || bin->op == ">" || bin->op == "<=" ||
                     bin->op == ">=" || bin->op == "==" || bin->op == "!=");
      std::string res_name;
      if (is_cmp) {
        res_name = "bool";
      } else {

        auto is_int_literal = [](Expr *ex) {
          auto n = dynamic_cast<NumberExpr *>(ex);
          return n && n->literal.find('.') == std::string::npos;
        };
        bool lhs_is_lit = is_int_literal(bin->lhs.get());
        bool rhs_is_lit = is_int_literal(bin->rhs.get());
        if (rhs_is_lit && !lhs_is_lit && is_integer_type_name(lt_base)) {
          res_name = lt_base;
        } else if (lhs_is_lit && !rhs_is_lit && is_integer_type_name(rt_base)) {
          res_name = rt_base;
        } else {
          res_name = promote_numeric_name(lt_base, rt_base);
        }
      }
      TypeInfo res(res_name);
      expr_type_map[e] = res;
      return res;
    }

    if ((lt.name == "bool" && rt.name == "bool") &&
        (bin->op == "==" || bin->op == "!=" || bin->op == "&&" ||
         bin->op == "||")) {
      TypeInfo res("bool");
      expr_type_map[e] = res;
      return res;
    }
    if (rt.name == "null" && starts_with(lt.name, "ptr<")) {
      expr_type_map[e] = lt;
      return lt;
    }

    auto normalize_for_read = [&](const std::string &nm) -> std::string {
      if (nm.rfind("ref<", 0) == 0 || nm.rfind("const<", 0) == 0 ||
          nm.rfind("refc<", 0) == 0) {
        std::string inner = extract_inner_type_name(nm);
        if (!inner.empty())
          return inner;
      }
      return nm;
    };

    std::string nl = normalize_for_read(lt.name);
    std::string nr = normalize_for_read(rt.name);

    if (bin->op == "=" && rt.name == "ptr<void>" &&
        starts_with(lt.name, "ptr<")) {
      expr_type_map[e] = TypeInfo(lt.name);
      return lt;
    }

    if (nl != nr) {
      int start_line = bin->lhs->line;
      int start_col = bin->lhs->col;
      int end_col = (bin->rhs->end_col ? bin->rhs->end_col : bin->rhs->col);
      diags.push_back({Diagnostic::Severity::Error,
                       "type mismatch in binary op '" + bin->op + "' (" +
                           lt.name + " vs " + rt.name + ")",
                       start_line, start_col, end_col});
      TypeInfo err("__err__");
      expr_type_map[e] = err;
      return err;
    }

    if (bin->op == "==" || bin->op == "!=" || bin->op == "<" ||
        bin->op == ">" || bin->op == "<=" || bin->op == ">=") {
      TypeInfo res("bool");
      expr_type_map[e] = res;
      return res;
    }
    expr_type_map[e] = TypeInfo(nl);
    return expr_type_map[e];
  }

  if (auto call = dynamic_cast<CallExpr *>(e)) {

    if (auto mem = dynamic_cast<MemberExpr *>(call->callee.get())) {

      if (auto obj_id = dynamic_cast<IdentifierExpr *>(mem->object.get())) {
        if (imported_aliases.find(obj_id->name) != imported_aliases.end()) {

          std::string value = imported_aliases[obj_id->name];
          size_t pipe_pos = value.find('|');
          std::string module_name =
              (pipe_pos != std::string::npos)
                  ? value.substr(pipe_pos + 1)
                  : obj_id->name;

          std::string mangled_name = module_name + "__" + mem->field;

          for (auto &arg : call->args) {
            type_of_expr(arg.get());
          }

          auto func_it = functions.find(mangled_name);
          if (func_it != functions.end()) {
            TypeInfo ret = func_it->second.ret;
            expr_type_map[e] = ret;
            return ret;
          } else {

            TypeInfo unknown("f64");
            expr_type_map[e] = unknown;
            return unknown;
          }
        }
      }

      TypeInfo obj_type = type_of_expr(mem->object.get());

      if (obj_type.name.rfind("Array<", 0) == 0) {
        std::string inner = extract_inner_type_name(obj_type.name);

        if (mem->field == "push") {

          if (call->args.size() != 1) {
            diags.push_back({Diagnostic::Severity::Error,
                             "push() expects 1 argument", call->line, call->col,
                             call->end_col ? call->end_col : call->col});
          } else {

            if (auto abl =
                    dynamic_cast<BlockLiteralExpr *>(call->args[0].get()))
              stamp_block_literal(abl, inner, abl->line, abl->col,
                                  "push() argument");
            TypeInfo arg_type = type_of_expr(call->args[0].get());
            if (arg_type.name != inner &&
                !(is_numeric_type_name(arg_type.name) &&
                  is_numeric_type_name(inner))) {
              diags.push_back({Diagnostic::Severity::Error,
                               "push() argument type mismatch (got " +
                                   arg_type.name + ", expected " + inner + ")",
                               call->args[0]->line, call->args[0]->col,
                               call->args[0]->end_col ? call->args[0]->end_col
                                                      : call->args[0]->col});
            }
          }
          TypeInfo ti("void");
          expr_type_map[e] = ti;
          return ti;
        } else if (mem->field == "pop") {

          if (call->args.size() != 0) {
            diags.push_back({Diagnostic::Severity::Error,
                             "pop() expects no arguments", call->line,
                             call->col,
                             call->end_col ? call->end_col : call->col});
          }
          TypeInfo ti(inner);
          expr_type_map[e] = ti;
          return ti;
        } else if (mem->field == "repr") {

          if (call->args.size() > 1) {
            diags.push_back({Diagnostic::Severity::Error,
                             "repr() expects 0 or 1 argument (raw: bool)",
                             call->line, call->col,
                             call->end_col ? call->end_col : call->col});
          } else if (call->args.size() == 1) {
            TypeInfo arg_type = type_of_expr(call->args[0].get());
            if (arg_type.name != "bool") {
              diags.push_back({Diagnostic::Severity::Error,
                               "repr() argument must be of type bool (got " +
                                   arg_type.name + ")",
                               call->args[0]->line, call->args[0]->col,
                               call->args[0]->end_col
                                   ? call->args[0]->end_col
                                   : call->args[0]->col});
            }
          }
          TypeInfo ti("string");
          expr_type_map[e] = ti;
          return ti;
        }
        else if (mem->field == "pushFront") {
          if (call->args.size() != 1) {
            diags.push_back({Diagnostic::Severity::Error,
                             "pushFront() expects 1 argument", call->line,
                             call->col,
                             call->end_col ? call->end_col : call->col});
          } else {
            TypeInfo arg_type = type_of_expr(call->args[0].get());
            if (arg_type.name != inner &&
                !(is_numeric_type_name(arg_type.name) &&
                  is_numeric_type_name(inner))) {
              diags.push_back({Diagnostic::Severity::Error,
                               "pushFront() argument type mismatch (got " +
                                   arg_type.name + ", expected " + inner + ")",
                               call->args[0]->line, call->args[0]->col,
                               call->args[0]->end_col ? call->args[0]->end_col
                                                      : call->args[0]->col});
            }
          }
          TypeInfo ti("void");
          expr_type_map[e] = ti;
          return ti;
        } else if (mem->field == "popFront") {
          if (call->args.size() != 0) {
            diags.push_back({Diagnostic::Severity::Error,
                             "popFront() expects no arguments", call->line,
                             call->col,
                             call->end_col ? call->end_col : call->col});
          }
          TypeInfo ti(inner);
          expr_type_map[e] = ti;
          return ti;
        } else if (mem->field == "insert") {

          if (call->args.size() != 2) {
            diags.push_back({Diagnostic::Severity::Error,
                             "insert() expects 2 arguments (index, value)",
                             call->line, call->col,
                             call->end_col ? call->end_col : call->col});
          }
          TypeInfo ti("void");
          expr_type_map[e] = ti;
          return ti;
        } else if (mem->field == "removeAt") {

          if (call->args.size() != 1) {
            diags.push_back({Diagnostic::Severity::Error,
                             "removeAt() expects 1 argument (index)",
                             call->line, call->col,
                             call->end_col ? call->end_col : call->col});
          }
          TypeInfo ti("void");
          expr_type_map[e] = ti;
          return ti;
        } else if (mem->field == "clear") {

          if (call->args.size() != 0) {
            diags.push_back({Diagnostic::Severity::Error,
                             "clear() expects no arguments",
                             call->line, call->col,
                             call->end_col ? call->end_col : call->col});
          }

          if (block_has_owned_ptr_fields(inner)) {
            diags.push_back({Diagnostic::Severity::Warning,
                             "clear() on Array<" + inner + ">: '" + inner +
                                 "' has ptr<T>/refc T fields that will leak : "
                                 "free inner pointers before calling clear()",
                             call->line, call->col,
                             call->end_col ? call->end_col : call->col});
          }
          TypeInfo ti("void");
          expr_type_map[e] = ti;
          return ti;
        } else if (mem->field == "freeAll") {

          if (call->args.size() != 0) {
            diags.push_back({Diagnostic::Severity::Error,
                             "freeAll() expects no arguments",
                             call->line, call->col,
                             call->end_col ? call->end_col : call->col});
          }
          if (inner.rfind("ptr<", 0) != 0 && inner.rfind("refc<", 0) != 0) {
            diags.push_back({Diagnostic::Severity::Error,
                             "freeAll() can only be called on Array<ptr<T>> "
                             "or Array<refc T>, but element type is " + inner,
                             call->line, call->col,
                             call->end_col ? call->end_col : call->col});
          }
          TypeInfo ti("void");
          expr_type_map[e] = ti;
          return ti;
        } else if (mem->field == "destroy") {

          if (call->args.size() != 0) {
            diags.push_back({Diagnostic::Severity::Error,
                             "destroy() expects no arguments",
                             call->line, call->col,
                             call->end_col ? call->end_col : call->col});
          }

          if (block_has_owned_ptr_fields(inner)) {
            diags.push_back({Diagnostic::Severity::Warning,
                             "destroy() on Array<" + inner + ">: '" + inner +
                                 "' has ptr<T>/refc T fields that will leak : "
                                 "free inner pointers before calling destroy()",
                             call->line, call->col,
                             call->end_col ? call->end_col : call->col});
          }
          TypeInfo ti("void");
          expr_type_map[e] = ti;
          return ti;
        }
      }

      if (obj_type.name.rfind("Hashmap<", 0) == 0) {
        std::string key_type, value_type;
        extract_hashmap_kv(obj_type.name, key_type, value_type);

        if (mem->field == "has" || mem->field == "contains") {
          if (call->args.size() != 1) {
            diags.push_back({Diagnostic::Severity::Error,
                             mem->field + "() expects 1 argument (key)",
                             call->line, call->col,
                             call->end_col ? call->end_col : call->col});
          } else {
            TypeInfo arg_type = type_of_expr(call->args[0].get());
            if (!key_type.empty() && arg_type.name != key_type &&
                !(is_numeric_type_name(arg_type.name) &&
                  is_numeric_type_name(key_type))) {
              diags.push_back(
                  {Diagnostic::Severity::Error,
                   mem->field + "() key type mismatch (got " + arg_type.name +
                       ", expected " + key_type + ")",
                   call->args[0]->line, call->args[0]->col,
                   call->args[0]->end_col ? call->args[0]->end_col
                                          : call->args[0]->col});
            }
          }
          TypeInfo ti("bool");
          expr_type_map[e] = ti;
          return ti;
        } else if (mem->field == "size") {
          if (call->args.size() != 0) {
            diags.push_back({Diagnostic::Severity::Error,
                             "size() expects no arguments", call->line,
                             call->col,
                             call->end_col ? call->end_col : call->col});
          }
          TypeInfo ti("i32");
          expr_type_map[e] = ti;
          return ti;
        } else if (mem->field == "remove" || mem->field == "delete") {
          if (call->args.size() != 1) {
            diags.push_back({Diagnostic::Severity::Error,
                             mem->field + "() expects 1 argument (key)",
                             call->line, call->col,
                             call->end_col ? call->end_col : call->col});
          } else {
            TypeInfo arg_type = type_of_expr(call->args[0].get());
            if (!key_type.empty() && arg_type.name != key_type &&
                !(is_numeric_type_name(arg_type.name) &&
                  is_numeric_type_name(key_type))) {
              diags.push_back(
                  {Diagnostic::Severity::Error,
                   mem->field + "() key type mismatch (got " + arg_type.name +
                       ", expected " + key_type + ")",
                   call->args[0]->line, call->args[0]->col,
                   call->args[0]->end_col ? call->args[0]->end_col
                                          : call->args[0]->col});
            }
          }
          TypeInfo ti("void");
          expr_type_map[e] = ti;
          return ti;
        } else if (mem->field == "clear") {
          if (call->args.size() != 0) {
            diags.push_back({Diagnostic::Severity::Error,
                             "clear() expects no arguments", call->line,
                             call->col,
                             call->end_col ? call->end_col : call->col});
          }

          if (value_type.rfind("ptr<", 0) == 0 ||
              value_type.rfind("refc<", 0) == 0) {
            diags.push_back(
                {Diagnostic::Severity::Warning,
                 "clear() on Hashmap<" + key_type + ", " + value_type +
                     ">: values of type '" + value_type +
                     "' will leak : free each value before calling clear()",
                 call->line, call->col,
                 call->end_col ? call->end_col : call->col});
          } else if (block_has_owned_ptr_fields(value_type)) {
            diags.push_back({Diagnostic::Severity::Warning,
                             "clear() on Hashmap<" + key_type + ", " +
                                 value_type + ">: '" + value_type +
                                 "' has ptr<T>/refc T fields that will leak "
                                 ": free inner pointers before calling "
                                 "clear()",
                             call->line, call->col,
                             call->end_col ? call->end_col : call->col});
          }
          TypeInfo ti("void");
          expr_type_map[e] = ti;
          return ti;
        }
      }

      if (obj_type.name == "Arena") {
        if (mem->field == "alloc") {
          if (call->args.size() != 1) {
            diags.push_back({Diagnostic::Severity::Error,
                             "arena.alloc() expects 1 argument (size)",
                             call->line, call->col,
                             call->end_col ? call->end_col : call->col});
          } else {
            type_of_expr(call->args[0].get());
          }
          TypeInfo ti("ptr<void>");
          expr_type_map[e] = ti;
          return ti;
        } else if (mem->field == "reset") {
          if (call->args.size() != 0) {
            diags.push_back({Diagnostic::Severity::Error,
                             "arena.reset() expects no arguments",
                             call->line, call->col,
                             call->end_col ? call->end_col : call->col});
          }
          TypeInfo ti("void");
          expr_type_map[e] = ti;
          return ti;
        } else if (mem->field == "destroy") {
          if (call->args.size() != 0) {
            diags.push_back({Diagnostic::Severity::Error,
                             "arena.destroy() expects no arguments",
                             call->line, call->col,
                             call->end_col ? call->end_col : call->col});
          }
          TypeInfo ti("void");
          expr_type_map[e] = ti;
          return ti;
        } else if (mem->field == "used") {
          if (call->args.size() != 0) {
            diags.push_back({Diagnostic::Severity::Error,
                             "arena.used() expects no arguments",
                             call->line, call->col,
                             call->end_col ? call->end_col : call->col});
          }
          TypeInfo ti("u64");
          expr_type_map[e] = ti;
          return ti;
        } else if (mem->field == "KiB" || mem->field == "MiB" ||
                   mem->field == "GiB") {

          if (call->args.size() != 0) {
            diags.push_back({Diagnostic::Severity::Error,
                             "arena." + mem->field + "() expects no arguments",
                             call->line, call->col,
                             call->end_col ? call->end_col : call->col});
          }
          TypeInfo ti("f64");
          expr_type_map[e] = ti;
          return ti;
        } else if (mem->field == "capacityBytes") {
          if (call->args.size() != 0) {
            diags.push_back({Diagnostic::Severity::Error,
                             "arena.capacityBytes() expects no arguments",
                             call->line, call->col,
                             call->end_col ? call->end_col : call->col});
          }
          TypeInfo ti("u64");
          expr_type_map[e] = ti;
          return ti;
        } else if (mem->field == "capacityKiB" || mem->field == "capacityMiB" ||
                   mem->field == "capacityGiB") {
          if (call->args.size() != 0) {
            diags.push_back({Diagnostic::Severity::Error,
                             "arena." + mem->field + "() expects no arguments",
                             call->line, call->col,
                             call->end_col ? call->end_col : call->col});
          }
          TypeInfo ti("f64");
          expr_type_map[e] = ti;
          return ti;
        } else if (mem->field == "scratchBegin") {
          if (call->args.size() != 0) {
            diags.push_back({Diagnostic::Severity::Error,
                             "arena.scratchBegin() expects no arguments",
                             call->line, call->col,
                             call->end_col ? call->end_col : call->col});
          }
          TypeInfo ti("ptr<void>");
          expr_type_map[e] = ti;
          return ti;
        } else if (mem->field == "scratchEnd") {
          if (call->args.size() != 1) {
            diags.push_back({Diagnostic::Severity::Error,
                             "arena.scratchEnd() expects 1 argument (checkpoint)",
                             call->line, call->col,
                             call->end_col ? call->end_col : call->col});
          } else {
            type_of_expr(call->args[0].get());
          }
          TypeInfo ti("void");
          expr_type_map[e] = ti;
          return ti;
        }
      }
    }

    if (auto id = dynamic_cast<IdentifierExpr *>(call->callee.get())) {

      if (id->name == "type" && call->args.size() == 1) {
        TypeInfo targ = type_of_expr(call->args[0].get());
        compile_time_string_map[call] = targ.name;
        TypeInfo ti("string");
        expr_type_map[e] = ti;
        return ti;
      }

      if (id->name == "ioPrint" || id->name == "ioPrintln" ||
          id->name == "ioRawPrint" || id->name == "ioRawPrintln") {
        for (auto &a : call->args)
          type_of_expr(a.get());
        TypeInfo ti("void");
        expr_type_map[e] = ti;
        return ti;
      }

      if (id->name == "sizeof" && call->args.size() == 1) {
        type_of_expr(call->args[0].get());
        TypeInfo ti("i32");
        expr_type_map[e] = ti;
        return ti;
      }

      if (id->name == "dealloc" && call->args.size() == 1) {
        TypeInfo arg = type_of_expr(call->args[0].get());
        if (arg.name.rfind("ptr<", 0) != 0 && arg.name != "ptr<void>") {
          diags.push_back({Diagnostic::Severity::Error,
                           "dealloc() requires a ptr<T> argument, got " +
                               arg.name,
                           call->line, call->col,
                           call->end_col ? call->end_col : call->col});
        }

        if (auto arg_id = dynamic_cast<IdentifierExpr *>(call->args[0].get())) {
          auto fi = freed_ptrs.find(arg_id->name);
          if (fi != freed_ptrs.end()) {

            diags.push_back({Diagnostic::Severity::Error,
                             "double-free: '" + arg_id->name +
                                 "' was already freed on line " +
                                 std::to_string(fi->second.line),
                             call->line, call->col,
                             call->end_col ? call->end_col : call->col});
          } else {

            freed_ptrs[arg_id->name] = {call->line, call->col};

            std::string inner;
            if (arg.name.rfind("ptr<", 0) == 0) {
              size_t p = arg.name.find('<'), q = arg.name.rfind('>');
              if (p != std::string::npos && q != std::string::npos && q > p+1)
                inner = arg.name.substr(p+1, q-p-1);
            }
            if (!inner.empty() && block_has_owned_ptr_fields(inner)) {
              diags.push_back({Diagnostic::Severity::Warning,
                               "dealloc('" + arg_id->name + "'): type '" +
                                   inner + "' has ptr<T>/refc T fields that "
                                   "will not be freed — free inner pointers "
                                   "before calling dealloc()",
                               call->line, call->col,
                               call->end_col ? call->end_col : call->col});
            }
          }
        }
        TypeInfo ti("void");
        expr_type_map[e] = ti;
        return ti;
      }

      if (id->name == "rcNew" && call->args.size() == 1) {
        TypeInfo inner = type_of_expr(call->args[0].get());
        TypeInfo ti("refc<" + inner.name + ">");
        expr_type_map[e] = ti;
        return ti;
      }
      if (id->name == "rcRetain" && call->args.size() == 1) {
        TypeInfo arg = type_of_expr(call->args[0].get());
        if (arg.name.rfind("refc<", 0) != 0) {
          diags.push_back({Diagnostic::Severity::Error,
                           "rcRetain() requires an refc T argument, got " +
                               arg.name,
                           call->line, call->col,
                           call->end_col ? call->end_col : call->col});
        }
        expr_type_map[e] = arg;
        return arg;
      }
      if (id->name == "rcRelease" && call->args.size() == 1) {
        TypeInfo arg = type_of_expr(call->args[0].get());
        if (arg.name.rfind("refc<", 0) != 0) {
          diags.push_back({Diagnostic::Severity::Error,
                           "rcRelease() requires an refc T argument, got " +
                               arg.name,
                           call->line, call->col,
                           call->end_col ? call->end_col : call->col});
        }

        if (auto arg_id = dynamic_cast<IdentifierExpr *>(call->args[0].get())) {
          auto fi = freed_ptrs.find(arg_id->name);
          if (fi != freed_ptrs.end()) {
            diags.push_back({Diagnostic::Severity::Error,
                             "double-release: '" + arg_id->name +
                                 "' was already released on line " +
                                 std::to_string(fi->second.line),
                             call->line, call->col,
                             call->end_col ? call->end_col : call->col});
          } else {
            freed_ptrs[arg_id->name] = {call->line, call->col};
          }
        }
        TypeInfo ti("void");
        expr_type_map[e] = ti;
        return ti;
      }
      if (id->name == "rcGet" && call->args.size() == 1) {
        TypeInfo arg = type_of_expr(call->args[0].get());
        std::string inner = "void";
        if (arg.name.rfind("refc<", 0) == 0) {
          size_t p = arg.name.find('<'), q = arg.name.rfind('>');
          if (p != std::string::npos && q != std::string::npos && q > p + 1)
            inner = arg.name.substr(p + 1, q - p - 1);
        } else {
          diags.push_back({Diagnostic::Severity::Error,
                           "rcGet() requires an refc T argument, got " +
                               arg.name,
                           call->line, call->col,
                           call->end_col ? call->end_col : call->col});
        }
        TypeInfo ti("ptr<" + inner + ">");
        expr_type_map[e] = ti;
        return ti;
      }
      if (id->name == "rcCount" && call->args.size() == 1) {
        TypeInfo arg = type_of_expr(call->args[0].get());
        if (arg.name.rfind("refc<", 0) != 0) {
          diags.push_back({Diagnostic::Severity::Error,
                           "rcCount() requires an refc T argument, got " +
                               arg.name,
                           call->line, call->col,
                           call->end_col ? call->end_col : call->col});
        }
        TypeInfo ti("u64");
        expr_type_map[e] = ti;
        return ti;
      }

      if (id->name == "arenaNew" && call->args.size() == 1) {
        type_of_expr(call->args[0].get());
        TypeInfo ti("Arena");
        expr_type_map[e] = ti;
        return ti;
      }

      if ((id->name == "PI" || id->name == "E") && call->args.size() == 0) {
        TypeInfo ti("f64");
        expr_type_map[e] = ti;
        return ti;
      }

      auto v = lookup_var(id->name);
      if (v && v->type.name.rfind("func(", 0) == 0) {
        std::vector<std::string> params;
        std::string ret;
        parse_func_signature_string(v->type.name, params, ret);
        if (call->args.size() != params.size()) {
          diags.push_back({Diagnostic::Severity::Error,
                           "argument count mismatch in call to " + id->name,
                           call->callee->line ? call->callee->line : call->line,
                           call->callee->col ? call->callee->col : call->col,
                           call->callee->end_col ? call->callee->end_col
                                                 : call->callee->col});
        } else {
          for (size_t i = 0; i < call->args.size(); ++i) {

            if (auto abl =
                    dynamic_cast<BlockLiteralExpr *>(call->args[i].get()))
              stamp_block_literal(abl, params[i], abl->line, abl->col,
                                  "argument " + std::to_string(i) +
                                      " of call to " + id->name);
            TypeInfo at = type_of_expr(call->args[i].get());

            if (is_ref_type_name(at.name)) {
              std::string inner = extract_inner_type_name(at.name);
              if (!inner.empty() && inner == params[i])
                continue;
            }
            if (at.name != params[i]) {
              if (at.name == "null" && starts_with(params[i], "ptr<"))
                continue;

              if (params[i] == "ptr<void>" && starts_with(at.name, "ptr<"))
                continue;
              if (at.name == "i32" && can_convert_literal_to_unsigned(
                                          call->args[i].get(), params[i])) {

                continue;
              }

              if (is_const_compatible_init(params[i], at.name))
                continue;
              diags.push_back({Diagnostic::Severity::Error,
                               "argument type mismatch in call to " + id->name +
                                   " for arg " + std::to_string(i) + " (" +
                                   at.name + " vs " + params[i] + ")",
                               call->args[i]->line, call->args[i]->col,
                               call->args[i]->end_col ? call->args[i]->end_col
                                                      : call->args[i]->col});
            }
          }
        }
        TypeInfo ti(ret);
        expr_type_map[e] = ti;
        return ti;
      }

      auto fn = lookup_func(id->name);
      if (fn) {

        size_t min_args = fn->params.size();
        if (fn->decl) {
          for (size_t i = fn->decl->params.size(); i > 0; --i) {
            if (fn->decl->params[i - 1].default_val)
              min_args = i - 1;
            else
              break;
          }
        }
        if (call->args.size() < min_args ||
            call->args.size() > fn->params.size()) {
          diags.push_back({Diagnostic::Severity::Error,
                           "argument count mismatch in call to " + id->name,
                           call->callee->line ? call->callee->line : call->line,
                           call->callee->col ? call->callee->col : call->col,
                           call->callee->end_col ? call->callee->end_col
                                                 : call->callee->col});
        } else {
          for (size_t i = 0; i < call->args.size(); ++i) {
            TypeInfo at = type_of_expr(call->args[i].get());
            const TypeInfo &param_t = fn->params[i];
            if (is_ref_type_name(at.name)) {
              std::string inner = extract_inner_type_name(at.name);
              if (!inner.empty() && inner == param_t.name)
                continue;
            }
            if (at.name != param_t.name) {
              if (!(at.name == "null" && starts_with(param_t.name, "ptr<"))) {

                if (!(param_t.name == "ptr<void>" &&
                      starts_with(at.name, "ptr<"))) {
                  if (at.name == "i32" &&
                      can_convert_literal_to_unsigned(call->args[i].get(),
                                                      param_t.name)) {

                    continue;
                  }
                  if (param_t.name != "any" &&
                      !is_const_compatible_init(param_t.name, at.name)) {
                    diags.push_back(
                        {Diagnostic::Severity::Error,
                         "argument type mismatch in call to " + id->name +
                             " for arg " + std::to_string(i) + " (" + at.name +
                             " vs " + param_t.name + ")",
                         call->args[i]->line, call->args[i]->col,
                         call->args[i]->end_col ? call->args[i]->end_col
                                                : call->args[i]->col});
                  }
                }
              }
            }
          }
        }
        TypeInfo ti = fn->ret;
        expr_type_map[e] = ti;
        return ti;
      }

      {
        auto tmpl_it = generic_templates.find(id->name);
        if (tmpl_it != generic_templates.end()) {
          TypeInfo ti = resolve_generic_call(call, id, tmpl_it->second);
          expr_type_map[e] = ti;
          return ti;
        }
      }

      for (const auto &entry : imported_aliases) {
        if (entry.first.rfind("__unqualified__", 0) == 0) {

          size_t pipe_pos = entry.second.find('|');
          if (pipe_pos != std::string::npos) {
            std::string module_name = entry.second.substr(pipe_pos + 1);
            std::string mangled_name = module_name + "__" + id->name;
            auto fn = lookup_func(mangled_name);
            if (fn) {

              if (call->args.size() != fn->params.size()) {
                diags.push_back(
                    {Diagnostic::Severity::Error,
                     "argument count mismatch in call to " + id->name,
                     call->callee->line ? call->callee->line : call->line,
                     call->callee->col ? call->callee->col : call->col,
                     call->callee->end_col ? call->callee->end_col
                                           : call->callee->col});
              } else {
                for (size_t i = 0; i < call->args.size(); ++i) {
                  TypeInfo at = type_of_expr(call->args[i].get());
                  const TypeInfo &param_t = fn->params[i];
                  if (is_ref_type_name(at.name)) {
                    std::string inner = extract_inner_type_name(at.name);
                    if (!inner.empty() && inner == param_t.name)
                      continue;
                  }
                  if (at.name != param_t.name && param_t.name != "any" &&
                      !is_const_compatible_init(param_t.name, at.name)) {
                    if (!(at.name == "null" &&
                          starts_with(param_t.name, "ptr<"))) {
                      if (!(param_t.name == "ptr<void>" &&
                            starts_with(at.name, "ptr<"))) {
                        diags.push_back(
                            {Diagnostic::Severity::Error,
                             "argument type mismatch in call to " + id->name,
                             call->args[i]->line, call->args[i]->col,
                             call->args[i]->end_col ? call->args[i]->end_col
                                                    : call->args[i]->col});
                      }
                    }
                  }
                }
              }
              TypeInfo ti = fn->ret;
              expr_type_map[e] = ti;
              return ti;
            }
          }
        }
      }

      diags.push_back(
          {Diagnostic::Severity::Error, "call to unknown function: " + id->name,
           id->line ? id->line : call->line, id->col ? id->col : call->col,
           id->end_col ? id->end_col
                       : (call->end_col ? call->end_col : id->col)});
      TypeInfo err("__err__");
      expr_type_map[e] = err;
      return err;
    } else {
      bool is_member_call = dynamic_cast<MemberExpr *>(call->callee.get());
      TypeInfo ct = type_of_expr(call->callee.get());
      if (is_func_type_name(ct.name)) {
        std::vector<std::string> params;
        std::string ret;
        parse_func_signature_string(ct.name, params, ret);

        size_t param_offset = (is_member_call && !params.empty()) ? 1 : 0;

        for (size_t i = 0; i < call->args.size(); ++i) {
          size_t pidx = i + param_offset;
          std::string expected = pidx < params.size() ? params[pidx] : "";
          if (!expected.empty()) {
            if (auto abl =
                    dynamic_cast<BlockLiteralExpr *>(call->args[i].get()))
              stamp_block_literal(abl, expected, abl->line, abl->col,
                                  "function call argument");
          }
          type_of_expr(call->args[i].get());
        }

        TypeInfo ti(ret);
        expr_type_map[e] = ti;
        return ti;
      }
      expr_type_map[e] = ct;
      return ct;
    }
  }

  if (auto lam = dynamic_cast<LambdaExpr *>(e)) {
    std::string sig = "func(";
    for (size_t i = 0; i < lam->params.size(); ++i) {
      if (i)
        sig += ",";
      sig += type_from_typenode(lam->params[i].second.get()).name;
    }
    sig += ")->";
    if (lam->return_type) {
      sig += type_from_typenode(lam->return_type.get()).name;
    } else {

      std::string inferred = "";
      std::function<void(const StmtList &)> scan = [&](const StmtList &stmts) {
        for (auto &st : stmts) {
          if (!st || !inferred.empty()) break;
          if (auto ret = dynamic_cast<ReturnStmt *>(st.get())) {
            if (ret->expr) {

              push_scope();
              for (auto &p : lam->params) {
                TypeInfo pt = type_from_typenode(p.second.get());
                add_var(p.first, pt, false, 0, 0);
              }
              TypeInfo rt = type_of_expr(ret->expr.get());
              pop_scope();
              if (rt.name != "__err__" && rt.name != "__err_type__")
                inferred = rt.name;
            }
          } else if (auto blk = dynamic_cast<BlockStmt *>(st.get())) {
            scan(blk->stmts);
          } else if (auto ifs = dynamic_cast<IfStmt *>(st.get())) {
            if (ifs->then_branch) {
              if (auto b = dynamic_cast<BlockStmt *>(ifs->then_branch.get()))
                scan(b->stmts);
            }
            if (ifs->else_branch && *ifs->else_branch) {
              if (auto b = dynamic_cast<BlockStmt *>((*ifs->else_branch).get()))
                scan(b->stmts);
            }
          } else if (auto wh = dynamic_cast<WhileStmt *>(st.get())) {
            if (wh->body) {
              if (auto b = dynamic_cast<BlockStmt *>(wh->body.get()))
                scan(b->stmts);
            }
          } else if (auto fr = dynamic_cast<ForStmt *>(st.get())) {
            if (fr->body) {
              if (auto b = dynamic_cast<BlockStmt *>(fr->body.get()))
                scan(b->stmts);
            }
          }
        }
      };
      scan(lam->body);
      if (!inferred.empty() && !lam->return_type)
        lam->return_type = type_node_from_canonical(inferred);
      sig += inferred.empty() ? "void" : inferred;
    }
    TypeInfo ti(sig);
    expr_type_map[e] = ti;
    return ti;
  }

  if (auto al = dynamic_cast<ArrayLiteralExpr *>(e)) {
    auto it = expr_type_map.find(e);
    if (it != expr_type_map.end()) {
      return it->second;
    }

    TypeInfo err("__err__");
    expr_type_map[e] = err;
    return err;
  }

  if (auto sof = dynamic_cast<SizeofExpr *>(e)) {

    TypeInfo ti("i32");

    if (sof->expr_arg) {

      type_of_expr(sof->expr_arg.get());
    }

    if (sof->type_arg) {
      TypeInfo arg_type = type_from_typenode(sof->type_arg.get());

    }

    expr_type_map[e] = ti;
    return ti;
  }

  diags.push_back({Diagnostic::Severity::Error,
                   "expression typing not implemented for this node", e->line,
                   e->col, e->end_col ? e->end_col : e->col});
  TypeInfo err("__err__");
  expr_type_map[e] = err;
  return err;
}

void TypeChecker::collect_local_var_names_in_stmt(
    Stmt *s, std::unordered_set<std::string> &locals) {
  if (!s)
    return;
  if (auto vd = dynamic_cast<VarDeclStmt *>(s)) {
    locals.insert(vd->name);
    return;
  }
  if (auto blk = dynamic_cast<BlockStmt *>(s)) {
    for (auto &st : blk->stmts)
      collect_local_var_names_in_stmt(st.get(), locals);
    return;
  }
  if (auto ifs = dynamic_cast<IfStmt *>(s)) {
    collect_local_var_names_in_stmt(ifs->then_branch.get(), locals);
    if (ifs->else_branch)
      collect_local_var_names_in_stmt((*ifs->else_branch).get(), locals);
    return;
  }
  if (auto fr = dynamic_cast<ForStmt *>(s)) {
    if (fr->init)
      collect_local_var_names_in_stmt(fr->init->get(), locals);
    if (fr->body)
      collect_local_var_names_in_stmt(fr->body.get(), locals);
    return;
  }
  if (auto wh = dynamic_cast<WhileStmt *>(s)) {
    collect_local_var_names_in_stmt(wh->body.get(), locals);
    return;
  }
}

void TypeChecker::collect_free_vars_in_stmt(
    Stmt *s, std::unordered_set<std::string> &used) {
  if (!s)
    return;
  if (auto vd = dynamic_cast<VarDeclStmt *>(s)) {
    if (vd->init)
      collect_free_vars_in_expr(vd->init.get(), used);
    return;
  }
  if (auto es = dynamic_cast<ExprStmt *>(s)) {
    collect_free_vars_in_expr(es->expr.get(), used);
    return;
  }
  if (auto ret = dynamic_cast<ReturnStmt *>(s)) {
    if (ret->expr)
      collect_free_vars_in_expr(ret->expr.get(), used);
    return;
  }
  if (auto blk = dynamic_cast<BlockStmt *>(s)) {
    for (auto &st : blk->stmts)
      collect_free_vars_in_stmt(st.get(), used);
    return;
  }
  if (auto ifs = dynamic_cast<IfStmt *>(s)) {
    collect_free_vars_in_expr(ifs->cond.get(), used);
    collect_free_vars_in_stmt(ifs->then_branch.get(), used);
    if (ifs->else_branch)
      collect_free_vars_in_stmt((*ifs->else_branch).get(), used);
    return;
  }
  if (auto wh = dynamic_cast<WhileStmt *>(s)) {
    collect_free_vars_in_expr(wh->cond.get(), used);
    collect_free_vars_in_stmt(wh->body.get(), used);
    return;
  }
  if (auto fr = dynamic_cast<ForStmt *>(s)) {
    if (fr->init)
      collect_free_vars_in_stmt(fr->init->get(), used);
    if (fr->cond)
      collect_free_vars_in_expr(fr->cond->get(), used);
    if (fr->post)
      collect_free_vars_in_stmt(fr->post->get(), used);
    collect_free_vars_in_stmt(fr->body.get(), used);
    return;
  }
}

void TypeChecker::collect_free_vars_in_expr(
    Expr *e, std::unordered_set<std::string> &used) {
  if (!e)
    return;
  if (auto id = dynamic_cast<IdentifierExpr *>(e)) {
    used.insert(id->name);
    return;
  }
  if (auto un = dynamic_cast<UnaryExpr *>(e)) {
    collect_free_vars_in_expr(un->rhs.get(), used);
    return;
  }
  if (auto bin = dynamic_cast<BinaryExpr *>(e)) {
    collect_free_vars_in_expr(bin->lhs.get(), used);
    collect_free_vars_in_expr(bin->rhs.get(), used);
    return;
  }
  if (auto call = dynamic_cast<CallExpr *>(e)) {
    collect_free_vars_in_expr(call->callee.get(), used);
    for (auto &a : call->args)
      collect_free_vars_in_expr(a.get(), used);
    return;
  }
  if (auto lam = dynamic_cast<LambdaExpr *>(e))
    return;
  if (auto cast = dynamic_cast<CastExpr *>(e)) {
    collect_free_vars_in_expr(cast->expr.get(), used);
    return;
  }

  if (auto idx = dynamic_cast<IndexExpr *>(e)) {
    collect_free_vars_in_expr(idx->object.get(), used);
    collect_free_vars_in_expr(idx->index.get(), used);
    return;
  }
  if (auto al = dynamic_cast<ArrayLiteralExpr *>(e)) {
    for (auto &el : al->elements)
      collect_free_vars_in_expr(el.get(), used);
    return;
  }

}

void TypeChecker::analyze_lambda_captures(LambdaExpr *lambda) {
  if (!lambda)
    return;

  std::vector<std::string> requested = std::move(lambda->captures);
  lambda->captures.clear();
  lambda->capture_type_names.clear();

  std::unordered_set<std::string> requested_set;
  for (auto &name : requested) {
    if (!requested_set.insert(name).second) {
      diags.push_back({Diagnostic::Severity::Error,
                       "'" + name + "' is captured more than once",
                       lambda->line, lambda->col, lambda->col});
      continue;
    }
    auto vs = lookup_var(name);
    if (!vs) {
      diags.push_back({Diagnostic::Severity::Error,
                       "cannot capture undefined variable '" + name + "'",
                       lambda->line, lambda->col, lambda->col});
      continue;
    }
    lambda->captures.push_back(name);
    lambda->capture_type_names.push_back(vs->type.name);
  }

  std::unordered_set<std::string> used;
  for (auto &s : lambda->body)
    collect_free_vars_in_stmt(s.get(), used);

  std::unordered_set<std::string> locals;
  for (auto &p : lambda->params)
    locals.insert(p.first);
  for (auto &s : lambda->body)
    collect_local_var_names_in_stmt(s.get(), locals);

  std::unordered_set<std::string> captured_set(lambda->captures.begin(),
                                               lambda->captures.end());
  for (auto &name : used) {
    if (locals.count(name) || captured_set.count(name))
      continue;
    if (lookup_var(name)) {
      diags.push_back(
          {Diagnostic::Severity::Error,
           "'" + name +
               "' is used in this lambda but not captured; add it to the "
               "capture list, e.g. '(" + name + ")['",
           lambda->line, lambda->col, lambda->col});
    }
  }
}

static bool is_const_compatible_init(const std::string &decl,
                                     const std::string &init) {
  if (decl == init)
    return true;

  auto strip_const = [](const std::string &s) -> std::string {
    if (s.rfind("const<", 0) == 0) {
      size_t p = s.find('<'), q = s.rfind('>');
      if (p != std::string::npos && q != std::string::npos && q > p + 1)
        return s.substr(p + 1, q - p - 1);
    }
    return s;
  };

  std::string decl_inner = strip_const(decl);

  if (is_numeric_type_name(decl_inner) && is_numeric_type_name(strip_const(init)))
    return true;
  if (decl_inner != decl) {

    if (is_const_compatible_init(decl_inner, init))
      return true;
  }

  auto get_ptr_inner = [](const std::string &s) -> std::string {
    if (s.rfind("ptr<", 0) == 0) {
      size_t p = s.find('<'), q = s.rfind('>');
      if (p != std::string::npos && q != std::string::npos && q > p + 1)
        return s.substr(p + 1, q - p - 1);
    }
    return "";
  };

  std::string d_ptr = strip_const(decl);
  std::string i_ptr = strip_const(init);
  std::string d_inner = get_ptr_inner(d_ptr);
  std::string i_inner = get_ptr_inner(i_ptr);
  if (!d_inner.empty() && !i_inner.empty()) {

    return is_const_compatible_init(d_inner, i_inner);
  }

  return false;
}

bool TypeChecker::stamp_block_literal(BlockLiteralExpr *bl,
                                      const std::string &target, int err_line,
                                      int err_col,
                                      const std::string &ctx_name) {

  auto already = expr_type_map.find(bl);
  if (already != expr_type_map.end() && already->second.name == target)
    return true;

  auto itb = blocks.find(target);
  if (itb == blocks.end())
    return false;

  const auto &fields = itb->second;

  if (!bl->positional.empty()) {
    if (bl->positional.size() != fields.size()) {
      diags.push_back(
          {Diagnostic::Severity::Error,
           "block literal field count mismatch for '" + target + "' in " +
               ctx_name + " (got " + std::to_string(bl->positional.size()) +
               " values, expected " + std::to_string(fields.size()) + ")",
           err_line, err_col, err_col});
    } else {
      for (size_t i = 0; i < fields.size() && i < bl->positional.size(); ++i) {

        if (auto nested =
                dynamic_cast<BlockLiteralExpr *>(bl->positional[i].get()))
          stamp_block_literal(nested, fields[i].second.name, nested->line,
                              nested->col, ctx_name);
        TypeInfo at = type_of_expr(bl->positional[i].get());
        const TypeInfo &ft = fields[i].second;
        if (!(at.name == "null" && ft.name.rfind("ptr<", 0) == 0) &&
            !(is_numeric_type_name(at.name) && is_numeric_type_name(ft.name)) &&
            at != ft) {
          diags.push_back({Diagnostic::Severity::Error,
                           "field '" + fields[i].first + "' of '" + target +
                               "' in " + ctx_name + ": got " + at.name +
                               ", expected " + ft.name,
                           err_line, err_col, err_col});
        }
      }
    }
  }

  for (auto &n : bl->named) {
    bool found = false;
    for (auto &f : fields) {
      if (f.first == n.first) {
        found = true;
        if (auto nested = dynamic_cast<BlockLiteralExpr *>(n.second.get()))
          stamp_block_literal(nested, f.second.name, nested->line, nested->col,
                              ctx_name);
        TypeInfo at = type_of_expr(n.second.get());
        const TypeInfo &ft = f.second;
        if (!(at.name == "null" && ft.name.rfind("ptr<", 0) == 0) &&
            !(is_numeric_type_name(at.name) && is_numeric_type_name(ft.name)) &&
            at != ft) {
          diags.push_back({Diagnostic::Severity::Error,
                           "field '" + f.first + "' of '" + target + "' in " +
                               ctx_name + ": got " + at.name + ", expected " +
                               ft.name,
                           err_line, err_col, err_col});
        }
        break;
      }
    }
    if (!found) {
      diags.push_back({Diagnostic::Severity::Error,
                       "unknown field '" + n.first + "' in '" + target + "'",
                       err_line, err_col, err_col});
    }
  }

  if (!bl->named.empty()) {
    auto bdecl_it = block_decls.find(target);
    for (auto &f : fields) {
      bool provided = false;
      for (auto &n : bl->named) {
        if (n.first == f.first) {
          provided = true;
          break;
        }
      }
      if (!provided) {
        bool has_default = false;
        if (bdecl_it != block_decls.end()) {
          for (auto &bf : bdecl_it->second->fields) {
            if (bf.name == f.first && bf.default_val) {
              has_default = true;
              break;
            }
          }
        }
        if (!has_default) {
          diags.push_back({Diagnostic::Severity::Error,
                           "missing field '" + f.first + "' in '" + target +
                               "' literal in " + ctx_name,
                           err_line, err_col, err_col});
        }
      }
    }
  }

  expr_type_map[bl] = TypeInfo(target);
  return true;
}

void TypeChecker::check_stmt(Stmt *s, const TypeInfo &expected_return) {
  if (!s)
    return;
  if (auto reg = dynamic_cast<RegisterDecl *>(s)) {
    TypeInfo ti = type_from_typenode(reg->element_type.get());
    add_var(reg->name, ti, false, reg->line, reg->col);
    return;
  }
  if (auto vs = dynamic_cast<VarDeclStmt *>(s)) {
    TypeInfo t = type_from_typenode(vs->type.get());
    bool handled_ref_init = false;
    if (t.name == "__err_type__" || vs->type->name == "__err_type__") {
      diags.push_back({Diagnostic::Severity::Error,
                       "type '" + vs->type->name + "' is not defined", vs->line,
                       vs->col, vs->col + (int)vs->type->name.length()});
      return;
    }
    if (t.name.rfind("ref<", 0) == 0) {
      if (!vs->init) {
        diags.push_back({Diagnostic::Severity::Error,
                         "reference variable must be initialized: " + vs->name,
                         vs->line, vs->col, vs->col});
      } else {

        if (auto init_id = dynamic_cast<IdentifierExpr *>(vs->init.get())) {
          auto v = lookup_var(init_id->name);
          if (!v) {
            diags.push_back({Diagnostic::Severity::Error,
                             "cannot bind reference to unknown identifier: " +
                                 init_id->name,
                             vs->line, vs->col, vs->col});
          } else {
            size_t p = t.name.find('<');
            size_t q = t.name.rfind('>');
            std::string inner =
                (p != std::string::npos && q != std::string::npos && q > p + 1)
                    ? t.name.substr(p + 1, q - p - 1)
                    : "";
            if (inner.empty() || v->type.name != inner) {
              diags.push_back({Diagnostic::Severity::Error,
                               "reference initializer type mismatch for '" +
                                   vs->name + "' (got " + v->type.name +
                                   ", expected " + inner + ")",
                               vs->line, vs->col, vs->col});
            }
          }
        } else {
          diags.push_back(
              {Diagnostic::Severity::Error,
               "reference initializer must be an identifier (lvalue)", vs->line,
               vs->col, vs->col});
        }
      }
      handled_ref_init = true;
    }

    if (vs->init && dynamic_cast<BlockLiteralExpr *>(vs->init.get())) {
      auto bl = dynamic_cast<BlockLiteralExpr *>(vs->init.get());

      if (t.name.rfind("Hashmap<", 0) == 0) {

        if (bl->positional.empty() && bl->named.empty()) {

          expr_type_map[bl] = TypeInfo(t.name);
        } else {
          diags.push_back(
              {Diagnostic::Severity::Error,
               "hashmap initializer must be empty {} for variable '" +
                   vs->name + "'",
               vs->line, vs->col, vs->col});
        }
      } else if (stamp_block_literal(bl, t.name, vs->line, vs->col,
                                     "variable '" + vs->name + "'")) {

      } else {

        auto itb = blocks.find(t.name);
        if (itb == blocks.end()) {
          diags.push_back({Diagnostic::Severity::Error,
                           "initializer type mismatch for variable '" +
                               vs->name + "' (got __err__, expected " + t.name +
                               ")",
                           vs->line, vs->col, vs->col});
        } else {
          const auto &fields = itb->second;
          if (!bl->positional.empty()) {
            if (bl->positional.size() != fields.size()) {
              diags.push_back(
                  {Diagnostic::Severity::Error,
                   "block positional initializer field count mismatch for '" +
                       vs->name + "'",
                   vs->line, vs->col, vs->col});
            } else {
              for (size_t i = 0; i < fields.size() && i < bl->positional.size();
                   ++i) {
                TypeInfo at = type_of_expr(bl->positional[i].get());
                const TypeInfo &ft = fields[i].second;
                if (!(at.name == "null" && starts_with(ft.name, "ptr<"))) {
                  if (!(is_numeric_type_name(at.name) &&
                        is_numeric_type_name(ft.name)) &&
                      at != ft) {
                    diags.push_back(
                        {Diagnostic::Severity::Error,
                         "block positional initializer type mismatch "
                         "for variable '" +
                             vs->name + "' field " + fields[i].first +
                             " (got " + at.name + ", expected " + ft.name + ")",
                         vs->line, vs->col, vs->col});
                  }
                }
              }
            }
          }
          for (auto &n : bl->named) {
            bool found = false;
            for (auto &f : fields) {
              if (f.first == n.first) {
                found = true;
                TypeInfo at = type_of_expr(n.second.get());
                const TypeInfo &ft = f.second;
                if (!(at.name == "null" && starts_with(ft.name, "ptr<"))) {
                  if (!(is_numeric_type_name(at.name) &&
                        is_numeric_type_name(ft.name)) &&
                      at != ft) {
                    diags.push_back({Diagnostic::Severity::Error,
                                     "block named initializer type mismatch "
                                     "for variable '" +
                                         vs->name + "' field " + f.first +
                                         " (got " + at.name + ", expected " +
                                         ft.name + ")",
                                     vs->line, vs->col, vs->col});
                  }
                }
                break;
              }
            }
            if (!found) {
              diags.push_back({Diagnostic::Severity::Error,
                               "unknown field '" + n.first +
                                   "' in block initializer for variable '" +
                                   vs->name + "'",
                               vs->line, vs->col, vs->col});
            }
          }

          if (!bl->named.empty()) {
            auto bdecl_it = block_decls.find(t.name);
            for (auto &f : fields) {
              bool provided = false;
              for (auto &n : bl->named) {
                if (n.first == f.first) {
                  provided = true;
                  break;
                }
              }
              if (!provided) {
                bool has_default = false;
                if (bdecl_it != block_decls.end()) {
                  for (auto &bf : bdecl_it->second->fields) {
                    if (bf.name == f.first && bf.default_val) {
                      has_default = true;
                      break;
                    }
                  }
                }
                if (!has_default) {
                  diags.push_back({Diagnostic::Severity::Error,
                                   "missing field '" + f.first +
                                       "' in block initializer for variable '" +
                                       vs->name + "' (no default value)",
                                   vs->line, vs->col, vs->col});
                }
              }
            }
          }
          expr_type_map[bl] = TypeInfo(t.name);
        }
      }
    }

    if (vs->init && dynamic_cast<ArrayLiteralExpr *>(vs->init.get())) {
      auto al = dynamic_cast<ArrayLiteralExpr *>(vs->init.get());

      if (t.name.rfind("Array<", 0) == 0) {
        std::string inner = extract_inner_type_name(t.name);
        bool ok = true;
        for (auto &el : al->elements) {
          TypeInfo et = type_of_expr(el.get());

          if (auto nested_al = dynamic_cast<ArrayLiteralExpr *>(el.get())) {

            if (et.name != inner && !starts_with(inner, "Array<")) {
              diags.push_back({Diagnostic::Severity::Error,
                               "nested array literal but inner type is not "
                               "Array<...> for '" +
                                   vs->name + "'",
                               vs->line, vs->col, vs->col});
              ok = false;
            }

          } else if (auto nested_bl =
                         dynamic_cast<BlockLiteralExpr *>(el.get())) {

            if (inner.rfind("Array<", 0) == 0) {
              expr_type_map[nested_bl] = TypeInfo(inner);

              std::string inner_elem = extract_inner_type_name(inner);
              (void)inner_elem;
            } else {

              if (!stamp_block_literal(
                      nested_bl, inner, nested_bl->line, nested_bl->col,
                      "array literal for '" + vs->name + "'")) {
                diags.push_back({Diagnostic::Severity::Error,
                                 "array element is a block literal but '" +
                                     inner + "' is not a known block type",
                                 vs->line, vs->col, vs->col});
                ok = false;
              }
            }

          } else if (!(et.name == "null" && starts_with(inner, "ptr<"))) {
            if (!(is_numeric_type_name(et.name) &&
                  is_numeric_type_name(inner)) &&
                et.name != inner) {
              diags.push_back({Diagnostic::Severity::Error,
                               "array literal element type mismatch for '" +
                                   vs->name + "' (got " + et.name +
                                   ", expected " + inner + ")",
                               vs->line, vs->col, vs->col});
              ok = false;
            }
          }
        }

        if (ok && !al->elements.empty()) {
          long long declared = extract_static_array_size(t.name);
          if (declared >= 0) {
            long long actual = static_cast<long long>(al->elements.size());
            if (actual != declared) {
              diags.push_back({Diagnostic::Severity::Error,
                               "array literal for '" + vs->name + "' has " +
                                   std::to_string(actual) +
                                   " element(s) but the declared " +
                                   "size is " + std::to_string(declared),
                               vs->line, vs->col, vs->col});
              ok = false;
            }
          }
        }
        if (ok)
          expr_type_map[al] = TypeInfo(t.name);
        else
          expr_type_map[al] = TypeInfo("__err__");
      } else {
        diags.push_back({Diagnostic::Severity::Error,
                         "array literal used where target is not Array<...>",
                         vs->line, vs->col, vs->col});
        expr_type_map[al] = TypeInfo("__err__");
      }
    }

    if (vs->init) {

      TypeInfo it = type_of_expr(vs->init.get());
      if (t.name.rfind("Array<", 0) == 0 && is_ptr_type_name(it.name)) {
        std::string target_inner = extract_inner_type_name(t.name);
        std::string src_inner = extract_inner_type_name(it.name);
        if (target_inner == src_inner) {

          expr_type_map[vs->init.get()] =
              TypeInfo(it.name);
        } else {
          diags.push_back(
              {Diagnostic::Severity::Error,
               "cannot initialize Array from ptr of different inner "
               "type",
               vs->line, vs->col, vs->col});
        }
      }

      if (!handled_ref_init &&
          !dynamic_cast<BlockLiteralExpr *>(vs->init.get()) &&
          !dynamic_cast<ArrayLiteralExpr *>(vs->init.get())) {
        TypeInfo it2 = type_of_expr(vs->init.get());

        if (t.name.rfind("const<", 0) == 0 || t.name.rfind("const ", 0) == 0) {
          std::string inner = extract_inner_type_name(t.name);
          if (inner.empty()) {
            diags.push_back(
                {Diagnostic::Severity::Error,
                 "const type has no inner type for variable '" + vs->name + "'",
                 vs->line, vs->col, vs->col});
          } else {
            if (!(it2.name == "null" && starts_with(t.name, "ptr<"))) {
              if (!(is_numeric_type_name(it2.name) &&
                    is_numeric_type_name(inner)) &&
                  !(it2.name == inner) &&
                  !is_const_compatible_init(inner, it2.name)) {

                bool literal_ok = false;
                if (auto num = dynamic_cast<NumberExpr *>(vs->init.get())) {
                  if (num->literal.find('.') == std::string::npos) {
                    try {
                      long long v = std::stoll(num->literal);
                      bool fits = false;
                      if (inner == "i8")
                        fits = (v >= std::numeric_limits<int8_t>::min() &&
                                v <= std::numeric_limits<int8_t>::max());
                      else if (inner == "i16")
                        fits = (v >= std::numeric_limits<int16_t>::min() &&
                                v <= std::numeric_limits<int16_t>::max());
                      else if (inner == "i32")
                        fits = (v >= std::numeric_limits<int32_t>::min() &&
                                v <= std::numeric_limits<int32_t>::max());
                      else if (inner == "i64")
                        fits = (v >= std::numeric_limits<int64_t>::min() &&
                                v <= std::numeric_limits<int64_t>::max());
                      else if (inner == "u8")
                        fits =
                            (v >= 0 && static_cast<unsigned long long>(v) <=
                                           std::numeric_limits<uint8_t>::max());
                      else if (inner == "u16")
                        fits = (v >= 0 &&
                                static_cast<unsigned long long>(v) <=
                                    std::numeric_limits<uint16_t>::max());
                      else if (inner == "u32")
                        fits = (v >= 0 &&
                                static_cast<unsigned long long>(v) <=
                                    std::numeric_limits<uint32_t>::max());
                      else if (inner == "u64")
                        fits = (v >= 0 &&
                                static_cast<unsigned long long>(v) <=
                                    std::numeric_limits<uint64_t>::max());
                      literal_ok = fits;
                    } catch (...) {
                      literal_ok = false;
                    }
                  }
                }
                if (!literal_ok) {
                  int end_col =
                      vs->init->end_col ? vs->init->end_col : vs->init->col;
                  diags.push_back(
                      {Diagnostic::Severity::Error,
                       "initializer type mismatch for const variable '" +
                           vs->name + "' (got " + it2.name + ", expected " +
                           t.name + ")",
                       vs->line, vs->col, end_col});
                }
              }
            }
          }
        } else {
          if (!(it2.name == "null" && starts_with(t.name, "ptr<"))) {

            if (!(it2.name == "ptr<void>" && starts_with(t.name, "ptr<"))) {
              if (!(is_numeric_type_name(it2.name) &&
                    is_numeric_type_name(t.name)) &&
                  it2 != t && !is_const_compatible_init(t.name, it2.name)) {

                bool literal_ok = false;

                if (it2.name == "i32" &&
                    can_convert_literal_to_unsigned(vs->init.get(), t.name)) {

                  literal_ok = true;
                }

                if (!literal_ok) {
                  int end_col =
                      vs->init->end_col ? vs->init->end_col : vs->init->col;
                  diags.push_back({Diagnostic::Severity::Error,
                                   "initializer type mismatch for variable '" +
                                       vs->name + "' (got " + it2.name +
                                       ", expected " + t.name + ")",
                                   vs->line, vs->col, end_col});
                }
              }
            }
          }
        }
      }

      if (auto lam = dynamic_cast<LambdaExpr *>(vs->init.get()))
        analyze_lambda_captures(lam);
    }

    if (vs->is_const && !vs->init) {
      diags.push_back({Diagnostic::Severity::Error,
                       "const variable '" + vs->name +
                           "' must be initialized at declaration",
                       vs->line, vs->col, vs->col});
    }

    if (!vs->is_const && t.name.rfind("const<", 0) == 0 && !vs->init) {
      diags.push_back({Diagnostic::Severity::Error,
                       "const-type variable '" + vs->name +
                           "' must be initialized at declaration",
                       vs->line, vs->col, vs->col});
    }

    if (!vs->init && !handled_ref_init && !vs->is_const) {

      bool should_warn = true;

      if (is_numeric_type_name(t.name) || t.name == "bool") {
        should_warn = false;
      }

      if (t.name.rfind("ptr<", 0) == 0) {
        should_warn = false;
      }

      if (should_warn) {
        diags.push_back(
            {Diagnostic::Severity::Warning,
             "variable '" + vs->name + "' is declared but not initialized",
             vs->line, vs->col, vs->col});
      }
    }

    if (vs->init && t.name.rfind("refc<", 0) == 0) {
      if (dynamic_cast<IdentifierExpr *>(vs->init.get())) {
        diags.push_back({Diagnostic::Severity::Warning,
                         "initialising '" + vs->name +
                             "' by copying refc T without rcRetain() — both "
                             "variables share the same refcount; use "
                             "rcRetain() if you want a second owning handle",
                         vs->line, vs->col, vs->col});
      }
    }

    add_var(vs->name, t, vs->is_const, vs->line, vs->col);
    return;
  }
  if (auto sw = dynamic_cast<SwitchStmt *>(s)) {

    TypeInfo switch_type = type_of_expr(sw->expr.get());

    if (switch_type.name == "__err__") {
      diags.push_back({Diagnostic::Severity::Error,
                       "invalid switch expression type", sw->line, sw->col,
                       sw->col});
      return;
    }

    bool is_opt = opts.find(switch_type.name) != opts.end();
    if (!is_integer_type_name(switch_type.name) && !is_opt) {
      diags.push_back({Diagnostic::Severity::Error,
                       "switch expression must be an integer or opt type, got '" +
                           switch_type.name + "'",
                       sw->line, sw->col, sw->col});
      return;
    }

    ++switch_depth;

    for (auto &c : sw->cases) {
      if (c.value) {
        TypeInfo case_type = type_of_expr(c.value.value().get());

        if (case_type.name != switch_type.name) {

          if (!(is_numeric_type_name(case_type.name) &&
                is_numeric_type_name(switch_type.name))) {
            diags.push_back({Diagnostic::Severity::Error,
                             "case value type " + case_type.name +
                                 " does not match switch expression type " +
                                 switch_type.name,
                             c.value.value()->line, c.value.value()->col,
                             c.value.value()->end_col ? c.value.value()->end_col
                                                      : c.value.value()->col});
          }
        }
      }

      for (auto &st : c.stmts) {
        if (st)
          check_stmt(st.get(), expected_return);
      }
    }

    --switch_depth;
    return;
  }
  if (auto br = dynamic_cast<BreakStmt *>(s)) {

    if (loop_depth == 0 && switch_depth == 0) {
      diags.push_back({Diagnostic::Severity::Error,
                       "break statement not within loop or switch", br->line,
                       br->col, br->col});
    }
    return;
  }
  if (auto cont = dynamic_cast<ContinueStmt *>(s)) {

    if (loop_depth == 0) {
      diags.push_back({Diagnostic::Severity::Error,
                       "continue statement not within loop", cont->line,
                       cont->col, cont->col});
    }
    return;
  }

  if (auto es = dynamic_cast<ExprStmt *>(s)) {
    if (auto call = dynamic_cast<CallExpr *>(es->expr.get())) {
      if (auto lam = dynamic_cast<LambdaExpr *>(call->callee.get()))
        analyze_lambda_captures(lam);
    }
    type_of_expr(es->expr.get());
    return;
  }

  if (auto ret = dynamic_cast<ReturnStmt *>(s)) {
    if (defer_depth > 0) {
      diags.push_back({Diagnostic::Severity::Error,
                       "'return' is not allowed inside a defer statement",
                       ret->line, ret->col, ret->col});
      return;
    }
    if (ret->expr) {

      if (auto bl = dynamic_cast<BlockLiteralExpr *>(ret->expr.get()))
        stamp_block_literal(bl, expected_return.name, ret->line, ret->col,
                            "return statement");
      auto rt = type_of_expr(ret->expr.get());

      if (starts_with(expected_return.name, "ptr<") ||
          starts_with(expected_return.name, "ref<")) {

        if (auto un = dynamic_cast<UnaryExpr *>(ret->expr.get())) {
          if (un->op == "&") {

            if (auto id = dynamic_cast<IdentifierExpr *>(un->rhs.get())) {
              auto var = lookup_var(id->name);
              if (var) {

                diags.push_back(
                    {Diagnostic::Severity::Warning,
                     "returning pointer to local variable '" + id->name +
                         "' (undefined behavior)",
                     ret->line, ret->col,
                     ret->expr->end_col ? ret->expr->end_col : ret->expr->col});
              }
            }
          }
        }
      }

      if (rt != expected_return) {
        bool can_convert = false;

        if (is_numeric_type_name(rt.name) &&
            is_numeric_type_name(expected_return.name))
          can_convert = true;

        if (rt.name == "i32" && can_convert_literal_to_unsigned(
                                    ret->expr.get(), expected_return.name)) {

          can_convert = true;
        }

        if (!can_convert) {
          int end_col =
              ret->expr->end_col ? ret->expr->end_col : ret->expr->col;
          diags.push_back({Diagnostic::Severity::Error,
                           "return type mismatch (got " + rt.name +
                               ", expected " + expected_return.name + ")",
                           ret->line, ret->col, end_col});
        }
      }
    } else {
      if (expected_return.name != "void") {
        diags.push_back({Diagnostic::Severity::Error,
                         "return with no value in function expecting " +
                             expected_return.name,
                         ret->line, ret->col, ret->col});
      }
    }
    return;
  }

  if (auto blk = dynamic_cast<BlockStmt *>(s)) {

    if (!blk->is_multi_var_expansion)
      push_scope();
    for (auto &st : blk->stmts)
      check_stmt(st.get(), expected_return);
    if (!blk->is_multi_var_expansion)
      pop_scope();
    return;
  }

  if (auto ifs = dynamic_cast<IfStmt *>(s)) {
    auto ct = type_of_expr(ifs->cond.get());
    check_stmt(ifs->then_branch.get(), expected_return);
    if (ifs->else_branch)
      check_stmt((*ifs->else_branch).get(), expected_return);
    return;
  }

  if (auto wh = dynamic_cast<WhileStmt *>(s)) {
    auto ct = type_of_expr(wh->cond.get());

    ++loop_depth;
    check_stmt(wh->body.get(), expected_return);
    --loop_depth;
    return;
  }

  if (auto fr = dynamic_cast<ForStmt *>(s)) {
    push_scope();
    if (fr->init)
      check_stmt(fr->init->get(), expected_return);

    if (fr->cond)
      type_of_expr(fr->cond->get());

    if (fr->post)
      check_stmt(fr->post->get(), expected_return);

    ++loop_depth;
    if (fr->body)
      check_stmt(fr->body.get(), expected_return);
    --loop_depth;
    pop_scope();
    return;
  }

  if (auto df = dynamic_cast<DeferStmt *>(s)) {
    if (defer_depth > 0) {
      diags.push_back({Diagnostic::Severity::Error,
                       "defer statements cannot be nested inside another "
                       "defer",
                       df->line, df->col, df->col});
      return;
    }

    int saved_loop_depth = loop_depth;
    int saved_switch_depth = switch_depth;
    loop_depth = 0;
    switch_depth = 0;
    ++defer_depth;
    if (df->body)
      check_stmt(df->body.get(), TypeInfo("void"));
    --defer_depth;
    loop_depth = saved_loop_depth;
    switch_depth = saved_switch_depth;
    return;
  }

  diags.push_back({Diagnostic::Severity::Error,
                   "statement checking not implemented for this kind of stmt",
                   s->line, s->col, s->col});
}

void TypeChecker::register_generic_set(GenericSetDecl *g) {
  if (!g)
    return;
  std::vector<TypeNode *> members;
  members.reserve(g->members.size());
  for (auto &m : g->members)
    members.push_back(m.get());
  generic_sets[g->name] = std::move(members);
}

bool TypeChecker::type_matches_pattern(const std::string &concrete_name,
                                      TypeNode *pattern) {
  if (!pattern)
    return false;

  if (generic_sets.count(pattern->name))
    return type_matches_generic_set(concrete_name, pattern->name);

  TypePtr concrete_node = type_node_from_canonical(concrete_name);
  if (concrete_node->name != pattern->name)
    return false;
  if (concrete_node->params.size() != pattern->params.size())
    return false;
  for (size_t i = 0; i < pattern->params.size(); ++i) {
    std::string sub = canonical_name_of(concrete_node->params[i].get());
    if (!type_matches_pattern(sub, pattern->params[i].get()))
      return false;
  }
  return true;
}

bool TypeChecker::type_matches_generic_set(const std::string &concrete_name,
                                           const std::string &set_name) {
  auto it = generic_sets.find(set_name);
  if (it == generic_sets.end())
    return false;
  for (TypeNode *member : it->second) {
    if (type_matches_pattern(concrete_name, member))
      return true;
  }
  return false;
}

bool TypeChecker::unify_generic_param(
    TypeNode *pattern, const std::string &concrete_name,
    const std::vector<GenericParam> &gparams,
    std::unordered_map<std::string, std::string> &bindings, std::string &err) {
  if (!pattern) {
    err = "internal error: missing type in generic signature";
    return false;
  }

  if (pattern->size_expr) {

    auto val = eval_symbolic_size(pattern->size_expr.get(), bindings);
    if (!val) {
      err = "value generic parameter(s) in computed array size '" +
            stringify_symbolic_expr(pattern->size_expr.get()) +
            "' are not yet known here — a value generic can only be "
            "inferred from a plain 'Array<T, N>' argument, not from a "
            "computed size expression; make sure it appears that way in an "
            "earlier parameter first";
      return false;
    }
    std::string concrete_size = concrete_name;
    const std::string prefix = "__size__:";
    if (concrete_size.rfind(prefix, 0) == 0)
      concrete_size = concrete_size.substr(prefix.size());
    long long concrete_val;
    try {
      concrete_val = std::stoll(concrete_size);
    } catch (...) {
      err = "expected a fixed-size array argument here";
      return false;
    }
    if (concrete_val != *val) {
      err = "array size mismatch: '" +
            stringify_symbolic_expr(pattern->size_expr.get()) + "' is " +
            std::to_string(*val) + ", but the argument's size is " +
            std::to_string(concrete_val);
      return false;
    }
    return true;
  }

  for (auto &gp : gparams) {
    if (gp.name != pattern->name)
      continue;
    std::string bind_value = concrete_name;
    if (gp.is_value) {

      const std::string prefix = "__size__:";
      if (bind_value.rfind(prefix, 0) == 0)
        bind_value = bind_value.substr(prefix.size());
      bool looks_integer = !bind_value.empty();
      size_t start = (bind_value[0] == '-') ? 1 : 0;
      if (start >= bind_value.size())
        looks_integer = false;
      for (size_t i = start; i < bind_value.size() && looks_integer; ++i)
        if (!std::isdigit((unsigned char)bind_value[i]))
          looks_integer = false;
      if (!looks_integer) {
        err = "could not infer an integer value for value generic parameter '" +
              gp.name + "'";
        return false;
      }
    }
    auto existing = bindings.find(gp.name);
    if (existing == bindings.end()) {
      bindings[gp.name] = bind_value;
    } else if (existing->second != bind_value &&
               !gp.is_value && is_numeric_type_name(existing->second) &&
               is_numeric_type_name(bind_value)) {
      existing->second = promote_numeric_name(existing->second, bind_value);
    } else if (existing->second != bind_value) {
      err = "conflicting " + std::string(gp.is_value ? "values" : "types") +
            " inferred for generic parameter '" + gp.name + "' (" +
            existing->second + " vs " + bind_value + ")";
      return false;
    }
    return true;
  }

  if (canonical_name_of(pattern) == "ptr<void>" &&
      starts_with(concrete_name, "ptr<")) {
    return true;
  }

  TypePtr concrete_node = type_node_from_canonical(concrete_name);
  if (concrete_node->name != pattern->name) {
    err = "expected '" + pattern->name + "', got '" + concrete_node->name + "'";
    return false;
  }
  if (concrete_node->params.size() != pattern->params.size()) {
    err = "type argument count mismatch for '" + pattern->name + "'";
    return false;
  }
  for (size_t i = 0; i < pattern->params.size(); ++i) {
    std::string sub = canonical_name_of(concrete_node->params[i].get());
    if (!unify_generic_param(pattern->params[i].get(), sub, gparams, bindings, err))
      return false;
  }
  return true;
}

TypePtr TypeChecker::substitute_type(
    TypeNode *tn, const std::unordered_map<std::string, std::string> &bindings) {
  if (!tn)
    return nullptr;
  if (tn->size_expr) {

    auto val = eval_symbolic_size(tn->size_expr.get(), bindings);
    if (val)
      return type_node_from_canonical("__size__:" + std::to_string(*val));

    auto clone = std::make_unique<TypeNode>(tn->name);
    clone->size_expr = substitute_expr(tn->size_expr.get(), bindings);
    return clone;
  }
  auto it = bindings.find(tn->name);
  if (it != bindings.end()) {

    std::string replacement = it->second;
    bool is_integer_literal = !replacement.empty();
    {
      size_t start = (replacement[0] == '-') ? 1 : 0;
      is_integer_literal = is_integer_literal && start < replacement.size();
      for (size_t i = start; i < replacement.size() && is_integer_literal; ++i)
        if (!std::isdigit((unsigned char)replacement[i]))
          is_integer_literal = false;
    }
    if (is_integer_literal)
      replacement = "__size__:" + replacement;
    return type_node_from_canonical(replacement);
  }
  auto clone = std::make_unique<TypeNode>(tn->name);
  clone->params.reserve(tn->params.size());
  for (auto &p : tn->params)
    clone->params.push_back(substitute_type(p.get(), bindings));
  if (tn->return_type)
    clone->return_type = substitute_type(tn->return_type.get(), bindings);
  return clone;
}

ExprPtr TypeChecker::substitute_expr(
    Expr *e, const std::unordered_map<std::string, std::string> &bindings) {
  if (!e)
    return nullptr;

  ExprPtr result;

  if (auto id = dynamic_cast<IdentifierExpr *>(e)) {

    auto opt_it = bindings.find(id->name);
    if (opt_it != bindings.end() && opt_it->second.rfind("opt:", 0) == 0) {
      std::string rest = opt_it->second.substr(4);
      size_t sep = rest.find(':');
      std::string opt_type = sep == std::string::npos ? rest : rest.substr(0, sep);
      std::string field = sep == std::string::npos ? "" : rest.substr(sep + 1);
      result = std::make_unique<MemberExpr>(
          std::make_unique<IdentifierExpr>(opt_type), field);
    } else {

    auto it = bindings.find(id->name);
    bool is_integer_literal = false;
    if (it != bindings.end() && !it->second.empty()) {
      size_t start = (it->second[0] == '-') ? 1 : 0;
      is_integer_literal = start < it->second.size();
      for (size_t i = start; i < it->second.size() && is_integer_literal; ++i)
        if (!std::isdigit((unsigned char)it->second[i]))
          is_integer_literal = false;
    }
    if (is_integer_literal) {
      result = std::make_unique<NumberExpr>(it->second);
    } else {
      result = std::make_unique<IdentifierExpr>(id->name);
    }
    }
  } else if (auto n = dynamic_cast<NumberExpr *>(e)) {
    result = std::make_unique<NumberExpr>(n->literal);
  } else if (auto s = dynamic_cast<StringExpr *>(e)) {
    result = std::make_unique<StringExpr>(s->literal);
  } else if (auto c = dynamic_cast<CharExpr *>(e)) {
    result = std::make_unique<CharExpr>(c->literal);
  } else if (auto b = dynamic_cast<BoolExpr *>(e)) {
    result = std::make_unique<BoolExpr>(b->value);
  } else if (dynamic_cast<NullExpr *>(e)) {
    result = std::make_unique<NullExpr>();
  } else if (auto un = dynamic_cast<UnaryExpr *>(e)) {
    auto c = std::make_unique<UnaryExpr>(un->op, substitute_expr(un->rhs.get(), bindings));
    c->is_postfix = un->is_postfix;
    result = std::move(c);
  } else if (auto bin = dynamic_cast<BinaryExpr *>(e)) {
    result = std::make_unique<BinaryExpr>(substitute_expr(bin->lhs.get(), bindings),
                                          bin->op,
                                          substitute_expr(bin->rhs.get(), bindings));
  } else if (auto call = dynamic_cast<CallExpr *>(e)) {
    auto c = std::make_unique<CallExpr>(substitute_expr(call->callee.get(), bindings));
    for (auto &a : call->args)
      c->args.push_back(substitute_expr(a.get(), bindings));
    result = std::move(c);
  } else if (auto cast = dynamic_cast<CastExpr *>(e)) {
    auto c = std::make_unique<CastExpr>(substitute_expr(cast->expr.get(), bindings),
                                        substitute_type(cast->target.get(), bindings));
    c->is_unsafe = cast->is_unsafe;
    result = std::move(c);
  } else if (auto lam = dynamic_cast<LambdaExpr *>(e)) {
    auto c = std::make_unique<LambdaExpr>();
    for (auto &p : lam->params)
      c->params.emplace_back(p.first, substitute_type(p.second.get(), bindings));
    c->return_type = substitute_type(lam->return_type.get(), bindings);
    for (auto &st : lam->body)
      c->body.push_back(substitute_stmt(st.get(), bindings));
    c->captures = lam->captures;
    c->capture_type_names = lam->capture_type_names;
    result = std::move(c);
  } else if (auto mem = dynamic_cast<MemberExpr *>(e)) {
    result = std::make_unique<MemberExpr>(substitute_expr(mem->object.get(), bindings), mem->field);
  } else if (auto bl = dynamic_cast<BlockLiteralExpr *>(e)) {
    auto c = std::make_unique<BlockLiteralExpr>();
    for (auto &p : bl->positional)
      c->positional.push_back(substitute_expr(p.get(), bindings));
    for (auto &nm : bl->named)
      c->named.emplace_back(nm.first, substitute_expr(nm.second.get(), bindings));
    result = std::move(c);
  } else if (auto al = dynamic_cast<ArrayLiteralExpr *>(e)) {
    auto c = std::make_unique<ArrayLiteralExpr>();
    for (auto &el : al->elements)
      c->elements.push_back(substitute_expr(el.get(), bindings));
    result = std::move(c);
  } else if (auto idx = dynamic_cast<IndexExpr *>(e)) {
    result = std::make_unique<IndexExpr>(substitute_expr(idx->object.get(), bindings),
                                         substitute_expr(idx->index.get(), bindings));
  } else if (auto sz = dynamic_cast<SizeofExpr *>(e)) {
    std::unique_ptr<SizeofExpr> c;
    if (sz->type_arg)
      c = std::make_unique<SizeofExpr>(substitute_type(sz->type_arg.get(), bindings));
    else
      c = std::make_unique<SizeofExpr>(substitute_expr(sz->expr_arg.get(), bindings));
    result = std::move(c);
  } else {

    return nullptr;
  }

  result->line = e->line;
  result->col = e->col;
  result->end_col = e->end_col;
  return result;
}

StmtPtr TypeChecker::substitute_stmt(
    Stmt *s, const std::unordered_map<std::string, std::string> &bindings) {
  if (!s)
    return nullptr;

  StmtPtr result;

  if (auto es = dynamic_cast<ExprStmt *>(s)) {
    result = std::make_unique<ExprStmt>(substitute_expr(es->expr.get(), bindings));
  } else if (auto rs = dynamic_cast<ReturnStmt *>(s)) {
    result = std::make_unique<ReturnStmt>(substitute_expr(rs->expr.get(), bindings));
  } else if (auto vd = dynamic_cast<VarDeclStmt *>(s)) {
    result = std::make_unique<VarDeclStmt>(vd->name, substitute_type(vd->type.get(), bindings),
                                           substitute_expr(vd->init.get(), bindings),
                                           vd->is_const);
  } else if (auto rd = dynamic_cast<RegisterDecl *>(s)) {
    result = std::make_unique<RegisterDecl>(rd->name, substitute_type(rd->element_type.get(), bindings),
                                            rd->address);
  } else if (auto bs = dynamic_cast<BlockStmt *>(s)) {
    auto c = std::make_unique<BlockStmt>();
    for (auto &st : bs->stmts)
      c->stmts.push_back(substitute_stmt(st.get(), bindings));
    c->is_multi_var_expansion = bs->is_multi_var_expansion;
    result = std::move(c);
  } else if (auto ifs = dynamic_cast<IfStmt *>(s)) {
    auto c = std::make_unique<IfStmt>(substitute_expr(ifs->cond.get(), bindings),
                                      substitute_stmt(ifs->then_branch.get(), bindings));
    if (ifs->else_branch)
      c->else_branch = substitute_stmt(ifs->else_branch->get(), bindings);
    result = std::move(c);
  } else if (auto ws = dynamic_cast<WhileStmt *>(s)) {
    result = std::make_unique<WhileStmt>(substitute_expr(ws->cond.get(), bindings),
                                         substitute_stmt(ws->body.get(), bindings));
  } else if (auto fs = dynamic_cast<ForStmt *>(s)) {
    auto c = std::make_unique<ForStmt>();
    if (fs->init)
      c->init = substitute_stmt(fs->init->get(), bindings);
    if (fs->cond)
      c->cond = substitute_expr(fs->cond->get(), bindings);
    if (fs->post)
      c->post = substitute_stmt(fs->post->get(), bindings);
    c->body = substitute_stmt(fs->body.get(), bindings);
    result = std::move(c);
  } else if (dynamic_cast<BreakStmt *>(s)) {
    result = std::make_unique<BreakStmt>();
  } else if (dynamic_cast<ContinueStmt *>(s)) {
    result = std::make_unique<ContinueStmt>();
  } else if (auto ds = dynamic_cast<DeferStmt *>(s)) {
    result = std::make_unique<DeferStmt>(substitute_stmt(ds->body.get(), bindings));
  } else if (auto sw = dynamic_cast<SwitchStmt *>(s)) {
    auto c = std::make_unique<SwitchStmt>();
    c->expr = substitute_expr(sw->expr.get(), bindings);
    for (auto &cs : sw->cases) {
      SwitchCase nc;
      if (cs.value)
        nc.value = substitute_expr(cs.value->get(), bindings);
      for (auto &st : cs.stmts)
        nc.stmts.push_back(substitute_stmt(st.get(), bindings));
      c->cases.push_back(std::move(nc));
    }
    result = std::move(c);
  } else {

    return nullptr;
  }

  result->line = s->line;
  result->col = s->col;
  return result;
}

std::string TypeChecker::mangle_generic_instance(
    const std::string &base_name, const std::vector<GenericParam> &gparams,
    const std::unordered_map<std::string, std::string> &bindings) {
  std::string out = base_name;
  for (auto &gp : gparams) {
    out += "$";
    auto it = bindings.find(gp.name);
    out += sanitize_for_mangle(it != bindings.end() ? it->second : "unknown");
  }
  return out;
}

FuncDecl *TypeChecker::instantiate_generic_func(
    FuncDecl *tmpl, const std::unordered_map<std::string, std::string> &bindings,
    const std::string &mangled_name) {
  auto clone = std::make_unique<FuncDecl>(mangled_name);
  clone->is_definition = tmpl->is_definition;
  clone->source_file = tmpl->source_file;
  clone->line = tmpl->line;
  clone->col = tmpl->col;

  for (auto &p : tmpl->params) {
    FuncParam fp;
    fp.name = p.name;
    fp.type = substitute_type(p.type.get(), bindings);
    fp.default_val = substitute_expr(p.default_val.get(), bindings);
    fp.noalias = p.noalias;
    clone->params.push_back(std::move(fp));
  }
  clone->return_type = substitute_type(tmpl->return_type.get(), bindings);
  for (auto &st : tmpl->body)
    clone->body.push_back(substitute_stmt(st.get(), bindings));

  FuncDecl *raw = clone.get();
  current_tu->decls.push_back(std::move(clone));
  return raw;
}

TypeInfo TypeChecker::resolve_generic_call(CallExpr *call, IdentifierExpr *id,
                                           FuncDecl *tmpl) {

  size_t min_args = tmpl->params.size();
  for (size_t i = tmpl->params.size(); i > 0; --i) {
    if (tmpl->params[i - 1].default_val)
      min_args = i - 1;
    else
      break;
  }
  if (call->args.size() < min_args || call->args.size() > tmpl->params.size()) {
    diags.push_back({Diagnostic::Severity::Error,
                     "argument count mismatch in call to generic function " +
                         id->name,
                     call->line, call->col,
                     call->end_col ? call->end_col : call->col});
    return TypeInfo("__err__");
  }

  std::vector<TypeInfo> arg_types;
  arg_types.reserve(call->args.size());
  for (auto &a : call->args)
    arg_types.push_back(type_of_expr(a.get()));

  std::unordered_map<std::string, std::string> bindings;
  for (size_t i = 0; i < call->args.size(); ++i) {
    std::string err;
    if (!unify_generic_param(tmpl->params[i].type.get(), arg_types[i].name,
                             tmpl->generic_params, bindings, err)) {
      diags.push_back({Diagnostic::Severity::Error,
                       "in call to " + id->name + "(): " + err,
                       call->args[i]->line, call->args[i]->col,
                       call->args[i]->end_col ? call->args[i]->end_col
                                              : call->args[i]->col});
      return TypeInfo("__err__");
    }
  }

  for (auto &gp : tmpl->generic_params) {
    auto b = bindings.find(gp.name);
    if (b == bindings.end()) {
      diags.push_back(
          {Diagnostic::Severity::Error,
           "could not infer generic parameter '" + gp.name + "' in call to " +
               id->name + "() — it does not occur in any argument's type",
           call->line, call->col, call->end_col ? call->end_col : call->col});
      return TypeInfo("__err__");
    }
    if (gp.is_value) {

      try {
        long long v = std::stoll(b->second);
        bool in_range = true;
        if (gp.constraint_set == "u8")
          in_range = v >= 0 && v <= 255;
        else if (gp.constraint_set == "u16")
          in_range = v >= 0 && v <= 65535;
        else if (gp.constraint_set == "u32")
          in_range = v >= 0 && v <= 4294967295LL;
        else if (gp.constraint_set == "u64")
          in_range = v >= 0;
        else if (gp.constraint_set == "i8")
          in_range = v >= -128 && v <= 127;
        else if (gp.constraint_set == "i16")
          in_range = v >= -32768 && v <= 32767;
        else if (gp.constraint_set == "i32")
          in_range = v >= -2147483648LL && v <= 2147483647LL;

        if (!in_range) {
          diags.push_back({Diagnostic::Severity::Error,
                           "value " + b->second +
                               " inferred for value generic parameter '" +
                               gp.name + "' does not fit in '" +
                               gp.constraint_set + "' (in call to " +
                               id->name + "())",
                           call->line, call->col,
                           call->end_col ? call->end_col : call->col});
          return TypeInfo("__err__");
        }
      } catch (...) {
        diags.push_back({Diagnostic::Severity::Error,
                         "internal error: value generic parameter '" +
                             gp.name + "' did not bind to an integer",
                         call->line, call->col,
                         call->end_col ? call->end_col : call->col});
        return TypeInfo("__err__");
      }
      continue;
    }
    if (!type_matches_generic_set(b->second, gp.constraint_set)) {
      diags.push_back({Diagnostic::Severity::Error,
                       "type '" + b->second + "' is not a member of generic "
                       "set '" + gp.constraint_set + "' (parameter '" +
                           gp.name + "' of " + id->name + "())",
                       call->line, call->col,
                       call->end_col ? call->end_col : call->col});
      return TypeInfo("__err__");
    }
  }


  if (tmpl->is_extern) {
    TypeInfo ret("void");
    if (tmpl->return_type) {
      TypePtr rt = substitute_type(tmpl->return_type.get(), bindings);
      ret = type_from_typenode(rt.get());
    }
    return ret;
  }

  std::string mangled = mangle_generic_instance(id->name, tmpl->generic_params, bindings);

  FuncDecl *instance;
  auto existing = generic_instances.find(mangled);
  if (existing != generic_instances.end()) {
    instance = existing->second;
  } else {
    instance = instantiate_generic_func(tmpl, bindings, mangled);
    generic_instances[mangled] = instance;

    std::vector<TypeInfo> param_types;
    for (auto &p : instance->params)
      param_types.push_back(type_from_typenode(p.type.get()));
    TypeInfo ret = instance->return_type ? type_from_typenode(instance->return_type.get())
                                         : TypeInfo("void");
    add_func(mangled, param_types, ret, instance->line, instance->col, instance);
    generic_worklist.push_back(instance);
  }

  id->name = mangled;

  auto fn_it = functions.find(mangled);
  return fn_it != functions.end() ? fn_it->second.ret : TypeInfo("void");
}

BlockDecl *TypeChecker::instantiate_generic_block(
    BlockDecl *tmpl, const std::unordered_map<std::string, std::string> &bindings,
    const std::string &mangled_name) {
  auto clone = std::make_unique<BlockDecl>(mangled_name);
  clone->line = tmpl->line;
  clone->col = tmpl->col;

  for (auto &f : tmpl->fields) {
    BlockField bf(f.name, substitute_type(f.type.get(), bindings));
    if (f.default_val)
      bf.default_val = substitute_expr(f.default_val.get(), bindings);
    clone->fields.push_back(std::move(bf));
  }

  BlockDecl *raw = clone.get();
  current_tu->decls.push_back(std::move(clone));

  blocks[mangled_name] = {};
  block_decls[mangled_name] = raw;
  std::vector<std::pair<std::string, TypeInfo>> fields;
  for (auto &f : raw->fields) {
    TypeInfo ft = type_from_typenode(f.type.get());
    if (ft.name == "__err_type__") {
      diags.push_back({Diagnostic::Severity::Error,
                       "block '" + tmpl->name + "': field '" + f.name +
                           "' has unknown type '" + f.type->name +
                           "' after substitution",
                       tmpl->line, tmpl->col, tmpl->col});
    }
    fields.emplace_back(f.name, ft);
  }
  blocks[mangled_name] = std::move(fields);

  return raw;
}

TypeInfo TypeChecker::resolve_generic_block_type(TypeNode *tn) {
  BlockDecl *tmpl = generic_block_templates[tn->name];

  if (tn->params.size() != tmpl->generic_params.size()) {
    diags.push_back({Diagnostic::Severity::Error,
                     "block '" + tn->name + "' expects " +
                         std::to_string(tmpl->generic_params.size()) +
                         " generic argument(s), got " +
                         std::to_string(tn->params.size()),
                     tmpl->line, tmpl->col, tmpl->col});
    return TypeInfo("__err_type__");
  }

  auto int_fits_constraint = [](const std::string &constraint,
                                long long v) -> bool {
    if (constraint == "u8")
      return v >= 0 && v <= 255;
    if (constraint == "u16")
      return v >= 0 && v <= 65535;
    if (constraint == "u32")
      return v >= 0 && v <= 4294967295LL;
    if (constraint == "u64")
      return v >= 0;
    if (constraint == "i8")
      return v >= -128 && v <= 127;
    if (constraint == "i16")
      return v >= -32768 && v <= 32767;
    if (constraint == "i32")
      return v >= -2147483648LL && v <= 2147483647LL;
    return true;
  };

  std::unordered_map<std::string, std::string> bindings;
  for (size_t i = 0; i < tn->params.size(); ++i) {
    const GenericParam &gp = tmpl->generic_params[i];
    TypeNode *arg = tn->params[i].get();

    if (gp.kind == GenericParamKind::ValueInt) {
      std::optional<long long> val;
      const std::string prefix = "__size__:";
      if (arg->name.rfind(prefix, 0) == 0) {
        try {
          val = std::stoll(arg->name.substr(prefix.size()));
        } catch (...) {
        }
      } else if (arg->size_expr) {
        val = eval_symbolic_size(arg->size_expr.get(), bindings);
      }
      if (!val) {
        diags.push_back({Diagnostic::Severity::Error,
                         "expected a compile-time integer value for generic "
                         "parameter '" +
                             gp.name + "' of block '" + tn->name + "'",
                         tmpl->line, tmpl->col, tmpl->col});
        return TypeInfo("__err_type__");
      }
      if (!int_fits_constraint(gp.constraint_set, *val)) {
        diags.push_back({Diagnostic::Severity::Error,
                         "value " + std::to_string(*val) +
                             " does not fit in '" + gp.constraint_set +
                             "' for generic parameter '" + gp.name +
                             "' of block '" + tn->name + "'",
                         tmpl->line, tmpl->col, tmpl->col});
        return TypeInfo("__err_type__");
      }
      bindings[gp.name] = std::to_string(*val);
    } else if (gp.kind == GenericParamKind::ValueOpt) {
      const std::string prefix = "__optval__:";
      if (arg->name.rfind(prefix, 0) != 0) {
        diags.push_back({Diagnostic::Severity::Error,
                         "expected a value of opts type '" +
                             gp.constraint_set + "' (e.g. '" +
                             gp.constraint_set +
                             ".SomeField') for generic parameter '" +
                             gp.name + "' of block '" + tn->name + "'",
                         tmpl->line, tmpl->col, tmpl->col});
        return TypeInfo("__err_type__");
      }
      std::string rest = arg->name.substr(prefix.size());
      size_t dot = rest.find('.');
      std::string opt_name = dot == std::string::npos ? rest : rest.substr(0, dot);
      std::string field_name = dot == std::string::npos ? "" : rest.substr(dot + 1);
      if (opt_name != gp.constraint_set) {
        diags.push_back({Diagnostic::Severity::Error,
                         "generic parameter '" + gp.name + "' of block '" +
                             tn->name + "' expects a value of opts type '" +
                             gp.constraint_set + "', got '" + opt_name + "'",
                         tmpl->line, tmpl->col, tmpl->col});
        return TypeInfo("__err_type__");
      }
      if (!lookup_opt_field_type(opt_name, field_name)) {
        diags.push_back({Diagnostic::Severity::Error,
                         "unknown field '" + field_name + "' on opts '" +
                             opt_name + "'",
                         tmpl->line, tmpl->col, tmpl->col});
        return TypeInfo("__err_type__");
      }
      bindings[gp.name] = "opt:" + opt_name + ":" + field_name;
    } else {
      TypeInfo concrete = type_from_typenode(arg);
      if (concrete.name == "__err_type__")
        return TypeInfo("__err_type__");
      if (!type_matches_generic_set(concrete.name, gp.constraint_set)) {
        diags.push_back({Diagnostic::Severity::Error,
                         "type '" + concrete.name +
                             "' is not a member of generic set '" +
                             gp.constraint_set + "' (parameter '" + gp.name +
                             "' of block '" + tn->name + "')",
                         tmpl->line, tmpl->col, tmpl->col});
        return TypeInfo("__err_type__");
      }
      bindings[gp.name] = concrete.name;
    }
  }

  std::string mangled =
      mangle_generic_instance(tn->name, tmpl->generic_params, bindings);

  BlockDecl *instance;
  auto existing = generic_block_instances.find(mangled);
  if (existing != generic_block_instances.end()) {
    instance = existing->second;
  } else {
    instance = instantiate_generic_block(tmpl, bindings, mangled);
    generic_block_instances[mangled] = instance;
  }

  tn->name = mangled;
  tn->params.clear();

  return TypeInfo(mangled);
}

void TypeChecker::check_func(FuncDecl *f) {
  push_scope();
  std::vector<TypeInfo> param_types;
  for (auto &p : f->params) {
    TypeInfo t = type_from_typenode(p.type.get());
    add_var(p.name, t, false, f->line, f->col);
    param_types.push_back(t);

    if (p.noalias) {

      TypeNode *tn = p.type.get();
      while (tn && tn->name == "const" && !tn->params.empty())
        tn = tn->params[0].get();
      bool is_pointer_like =
          tn && (tn->name == "ptr" || tn->name == "ref") && !tn->params.empty();
      if (!is_pointer_like) {
        diags.push_back({Diagnostic::Severity::Error,
                         "'noalias' can only be applied to a pointer or ref "
                         "parameter",
                         f->line, f->col});
      }
    }
  }
  TypeInfo ret = f->return_type ? type_from_typenode(f->return_type.get())
                                : TypeInfo("void");

  for (auto &s : f->body)
    check_stmt(s.get(), ret);

  if (ret.name != "void") {

    bool has_return = false;
    std::function<bool(Stmt *)> find_return = [&](Stmt *s) -> bool {
      if (!s)
        return false;
      if (dynamic_cast<ReturnStmt *>(s))
        return true;
      if (auto blk = dynamic_cast<BlockStmt *>(s)) {
        for (auto &st : blk->stmts)
          if (find_return(st.get()))
            return true;
      }
      if (auto ifs = dynamic_cast<IfStmt *>(s)) {
        if (find_return(ifs->then_branch.get()))
          return true;
        if (ifs->else_branch && find_return((*ifs->else_branch).get()))
          return true;
      }
      if (auto wh = dynamic_cast<WhileStmt *>(s)) {
        if (find_return(wh->body.get()))
          return true;
      }
      if (auto fr = dynamic_cast<ForStmt *>(s)) {
        if (find_return(fr->body.get()))
          return true;
      }

      return false;
    };

    for (auto &st : f->body) {
      if (find_return(st.get())) {
        has_return = true;
        break;
      }
    }

    if (!has_return) {
      diags.push_back({Diagnostic::Severity::Warning,
                       "function '" + f->name +
                           "' may not return a value in all code paths",
                       f->line, f->col, f->col});
    }
  }

  pop_scope();
}

std::optional<TypeInfo> TypeChecker::lookup_type_at(const TranslationUnit *tu,
                                                    int line, int col) {
  for (auto &d : tu->decls) {
    if (auto f = dynamic_cast<FuncDecl *>(d.get())) {
      if (f->line == line && f->col == col) {
        TypeInfo ret = f->return_type ? type_from_typenode(f->return_type.get())
                                      : TypeInfo("void");
        return ret;
      }
    }
  }
  return {};
}
