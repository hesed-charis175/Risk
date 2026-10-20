#include "codegen.h"
#include "runtime.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <functional>
#include <iostream>
#include <sstream>
#include <string.h>
#include <string>
#include <tuple>

CodeGen::CodeGen(
    TranslationUnit *tu_, const std::string &out_c_path,
    const std::string &module_name,
    const std::unordered_map<Expr *, std::string> *compile_time_map_,
    const std::unordered_map<Expr *, TypeInfo> *expr_type_map_,
    const std::unordered_map<std::string, std::string> *type_aliases_,
    bool emit_debug_lines)
    : tu(tu_), out_path(out_c_path), module(module_name),
      debug_lines_enabled(emit_debug_lines),
      compile_time_map(compile_time_map_), expr_type_map(expr_type_map_),
      type_aliases(type_aliases_) {}

std::string CodeGen::mangle(const std::string &name) {
  return module + "__" + name;
}

std::string CodeGen::block_struct_name(BlockDecl *b) {
  if (!b)
    return "";
  if (!b->is_extern)
    return mangle(b->name);
  if (!b->extern_c_spelling.empty())
    return b->extern_c_spelling;

  return "struct " + b->name;
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

static std::string get_module_name_from_alias(TranslationUnit *tu,
                                              const std::string &alias) {

  for (auto &d : tu->decls) {
    if (auto im = dynamic_cast<ImportDecl *>(d.get())) {
      if (im->alias == alias) {
        return module_name_from_import_path(im->path);
      }
    }
  }

  return alias;
}

static std::string extract_inner_type_name(const std::string &n) {
  size_t p = n.find('<');
  size_t q = n.rfind('>');
  if (p != std::string::npos && q != std::string::npos && q > p + 1) {
    std::string contents = n.substr(p + 1, q - p - 1);

    if (n.rfind("Array<", 0) == 0) {
      size_t comma = contents.find(',');
      if (comma != std::string::npos) {
        std::string result = contents.substr(0, comma);

        while (!result.empty() && result.back() == ' ')
          result.pop_back();
        while (!result.empty() && result[0] == ' ')
          result = result.substr(1);
        return result;
      }
    }

    while (!contents.empty() && contents.back() == ' ')
      contents.pop_back();
    while (!contents.empty() && contents[0] == ' ')
      contents = contents.substr(1);
    return contents;
  }
  return "";
}

std::string CodeGen::fresh_id(const std::string &prefix) {
  return prefix + "_" + std::to_string(unique_id++);
}
std::string CodeGen::build_type_name_from_node(TypeNode *tn) {
  if (!tn)
    return "void";

  if (tn->name == "func") {
    std::string s = "func(";
    for (size_t i = 0; i < tn->params.size(); ++i) {
      if (i)
        s += ",";
      auto param = tn->params[i].get();
      if (!param) {
        s += "void";
      } else {
        s += build_type_name_from_node(param);
      }
    }
    s += ")->";
    if (tn->return_type) {
      auto ret = tn->return_type.get();
      if (ret) {
        s += build_type_name_from_node(ret);
      } else {
        s += "void";
      }
    } else {
      s += "void";
    }
    return s;
  }

  std::string s = tn->name;
  if (!tn->params.empty()) {
    s += "<";
    for (size_t i = 0; i < tn->params.size(); ++i) {
      if (i)
        s += ",";
      auto param = tn->params[i].get();
      if (!param) {
        s += "void";
      } else {
        s += build_type_name_from_node(param);
      }
    }
    s += ">";
  }
  return s;
}
void CodeGen::emit_top_level_func_names() {
  top_level_funcs.clear();
  extern_func_names.clear();
  for (auto &dptr : tu->decls) {
    if (auto f = dynamic_cast<FuncDecl *>(dptr.get())) {

      if (!f->generic_params.empty()) {
        if (f->is_extern) {
          top_level_funcs[f->name] = true;
          extern_func_names.insert(f->name);
        }
        continue;
      }
      top_level_funcs[f->name] = true;
      if (f->is_extern)
        extern_func_names.insert(f->name);
    }
  }
}

void CodeGen::collect_lambdas() {
  lambda_ids.clear();
  immediate_wrapper_ids.clear();
  std::function<void(Stmt *)> walk_stmt;
  std::function<void(Expr *)> walk_expr;
  walk_expr = [&](Expr *e) {
    if (!e)
      return;
    if (auto call = dynamic_cast<CallExpr *>(e)) {

      if (auto lam = dynamic_cast<LambdaExpr *>(call->callee.get())) {
        prepare_immediate_wrapper(lam);
      } else {
        walk_expr(call->callee.get());
      }
      for (auto &a : call->args)
        walk_expr(a.get());
      return;
    }
    if (auto bin = dynamic_cast<BinaryExpr *>(e)) {
      walk_expr(bin->lhs.get());
      walk_expr(bin->rhs.get());
      return;
    }
    if (auto un = dynamic_cast<UnaryExpr *>(e)) {
      walk_expr(un->rhs.get());
      return;
    }
    if (auto lam = dynamic_cast<LambdaExpr *>(e)) {
      prepare_lambda(lam);
      return;
    }
    if (auto cast = dynamic_cast<CastExpr *>(e)) {
      walk_expr(cast->expr.get());
      return;
    }
    if (auto mem = dynamic_cast<MemberExpr *>(e)) {
      walk_expr(mem->object.get());
      return;
    }
    if (auto bl = dynamic_cast<BlockLiteralExpr *>(e)) {
      for (auto &p : bl->positional)
        walk_expr(p.get());
      for (auto &n : bl->named)
        walk_expr(n.second.get());
      return;
    }
    if (auto al = dynamic_cast<ArrayLiteralExpr *>(e)) {
      for (auto &el : al->elements)
        walk_expr(el.get());
      return;
    }
    if (auto idx = dynamic_cast<IndexExpr *>(e)) {
      walk_expr(idx->object.get());
      walk_expr(idx->index.get());
      return;
    }
  };
  walk_stmt = [&](Stmt *s) {
    if (!s)
      return;
    if (auto vs = dynamic_cast<VarDeclStmt *>(s)) {
      if (vs->init)
        walk_expr(vs->init.get());
      return;
    }
    if (auto es = dynamic_cast<ExprStmt *>(s)) {
      walk_expr(es->expr.get());
      return;
    }
    if (auto ret = dynamic_cast<ReturnStmt *>(s)) {
      if (ret->expr)
        walk_expr(ret->expr.get());
      return;
    }
    if (auto blk = dynamic_cast<BlockStmt *>(s)) {
      for (auto &st : blk->stmts)
        walk_stmt(st.get());
      return;
    }
    if (auto ifs = dynamic_cast<IfStmt *>(s)) {
      walk_expr(ifs->cond.get());
      walk_stmt(ifs->then_branch.get());
      if (ifs->else_branch)
        walk_stmt((*ifs->else_branch).get());
      return;
    }
    if (auto wh = dynamic_cast<WhileStmt *>(s)) {
      walk_expr(wh->cond.get());
      walk_stmt(wh->body.get());
      return;
    }
    if (auto fr = dynamic_cast<ForStmt *>(s)) {
      if (fr->init)
        walk_stmt(fr->init->get());
      if (fr->cond)
        walk_expr(fr->cond->get());
      if (fr->post)
        walk_stmt(fr->post->get());
      walk_stmt(fr->body.get());
      return;
    }
    if (auto sw = dynamic_cast<SwitchStmt *>(s)) {
      walk_expr(sw->expr.get());
      for (auto &c : sw->cases) {
        if (c.value)
          walk_expr(c.value->get());
        for (auto &st : c.stmts)
          walk_stmt(st.get());
      }
      return;
    }
    if (auto df = dynamic_cast<DeferStmt *>(s)) {
      walk_stmt(df->body.get());
      return;
    }
  };

  for (auto &dptr : tu->decls) {
    if (auto f = dynamic_cast<FuncDecl *>(dptr.get())) {

      if (!f->generic_params.empty())
        continue;
      for (auto &p : f->params)
        if (p.default_val)
          walk_expr(p.default_val.get());
      for (auto &s : f->body)
        walk_stmt(s.get());
      continue;
    }

    if (auto gv = dynamic_cast<GlobalVarDecl *>(dptr.get())) {
      if (gv->var && gv->var->init)
        walk_expr(gv->var->init.get());
      continue;
    }

    if (auto bd = dynamic_cast<BlockDecl *>(dptr.get())) {
      if (!bd->generic_params.empty())
        continue;
      for (auto &f : bd->fields)
        if (f.default_val)
          walk_expr(f.default_val.get());
      continue;
    }
  }
}

std::string CodeGen::prepare_lambda(LambdaExpr *lam) {
  auto it = lambda_ids.find(lam);
  if (it != lambda_ids.end())
    return it->second;
  std::string id = std::to_string(unique_id++);
  lambda_ids[lam] = id;
  ensure_closure_structs_and_helpers_for_lambda_with_id(lam, id);
  return id;
}

std::string CodeGen::prepare_immediate_wrapper(LambdaExpr *lam) {
  auto it = immediate_wrapper_ids.find(lam);
  if (it != immediate_wrapper_ids.end())
    return it->second;
  std::string id = "imm_" + std::to_string(unique_id++);
  immediate_wrapper_ids[lam] = id;
  ensure_immediate_wrapper_for_lambda_with_id(lam, id);
  return id;
}

std::string CodeGen::make_lambda_env_name(const std::string &id) {
  return "lambda_env_" + id;
}
std::string CodeGen::make_lambda_wrapper_name(const std::string &id) {
  return "lambda_wrapper_" + id;
}
std::string CodeGen::make_lambda_factory_name(const std::string &id) {
  return "make_lambda_" + id;
}

static BlockDecl *find_block_decl(TranslationUnit *tu,
                                  const std::string &name) {
  if (!tu)
    return nullptr;
  for (auto &d : tu->decls) {
    if (auto b = dynamic_cast<BlockDecl *>(d.get())) {
      if (b->name == name)
        return b;
    }
  }
  return nullptr;
}

static OptDecl *find_opts_decl(TranslationUnit *tu, const std::string &name) {
  if (!tu)
    return nullptr;
  for (auto &d : tu->decls) {
    if (auto b = dynamic_cast<OptDecl *>(d.get())) {
      if (b->name == name)
        return b;
    }
  }

  return nullptr;
}

static void opts_value_range(OptDecl *od, long long &min_val,
                             long long &max_val) {
  min_val = 0;
  max_val = 0;
  bool first = true;
  for (auto &f : od->fields) {
    long long v = f.value ? *f.value : 0;
    if (first) {
      min_val = max_val = v;
      first = false;
    } else {
      min_val = std::min(min_val, v);
      max_val = std::max(max_val, v);
    }
  }
}

static void parse_func_signature_string(const std::string &s,
                                        std::vector<std::string> &out_params,
                                        std::string &out_ret);

BlockField *CodeGen::find_block_repr_field(const std::string &elem_type_name) {
  BlockDecl *b = find_block_decl(tu, elem_type_name);
  if (!b)
    return nullptr;
  for (auto &field : b->fields) {
    if (field.name != "repr" || !field.type || field.type->name != "func")
      continue;
    std::string fn_type_name = build_type_name_from_node(field.type.get());
    std::vector<std::string> params;
    std::string ret;
    parse_func_signature_string(fn_type_name, params, ret);
    if (params.size() == 1 && params[0] == elem_type_name && ret == "string")
      return &field;
  }
  return nullptr;
}

BlockField *CodeGen::find_block_init_field(const std::string &block_type_name) {
  BlockDecl *b = find_block_decl(tu, block_type_name);
  if (!b)
    return nullptr;
  for (auto &field : b->fields) {
    if (field.name != "init" || !field.type || field.type->name != "func")
      continue;
    std::string fn_type_name = build_type_name_from_node(field.type.get());
    std::vector<std::string> params;
    std::string ret;
    parse_func_signature_string(fn_type_name, params, ret);
    if (params.size() == 1 && params[0] == block_type_name && ret == "void")
      return &field;
  }
  return nullptr;
}

void CodeGen::collect_this_ref_funcs() {
  this_ref_funcs.clear();
  for (auto &dptr : tu->decls) {
    auto b = dynamic_cast<BlockDecl *>(dptr.get());
    if (!b)
      continue;
    for (auto &field : b->fields) {
      if (!field.type || field.type->name != "func" ||
          field.type->params.empty())
        continue;
      if (!field.type->params[0]->is_this_param)
        continue;
      if (auto id = dynamic_cast<IdentifierExpr *>(field.default_val.get()))
        this_ref_funcs.insert(id->name);
    }
  }
}

bool CodeGen::is_this_ref_object(Expr *obj) const {
  if (current_this_ref_param_name.empty())
    return false;
  auto id = dynamic_cast<IdentifierExpr *>(obj);
  return id && id->name == current_this_ref_param_name;
}

void CodeGen::emit_auto_init_call(std::ostream &os, const std::string &ind,
                                  const std::string &var_expr,
                                  const std::string &block_type_name) {
  if (!find_block_init_field(block_type_name))
    return;

  std::string this_ctype = ctype_from_type_name(block_type_name) + "*";
  std::string mem_str = var_expr + ".init";
  os << ind << "((void(*)(void*, " << this_ctype << "))"
     << "(((risk_closure_t*)(" << mem_str << "))->fn))"
     << "(((risk_closure_t*)(" << mem_str << "))->env, &" << var_expr << ");\n";
}

static void parse_func_signature_string(const std::string &s,
                                        std::vector<std::string> &out_params,
                                        std::string &out_ret) {
  out_params.clear();
  out_ret = "void";
  if (s.rfind("func(", 0) != 0)
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
    if (a != std::string::npos && b != std::string::npos && b >= a) {
      out_params.push_back(tok.substr(a, b - a + 1));
    } else if (!tok.empty()) {
      out_params.push_back(tok);
    }
    i = j + 1;
  }
}

bool CodeGen::generate() {
  helper_code.clear();
  helper_prototypes.clear();
  emit_top_level_func_names();
  collect_lambdas();
  collect_this_ref_funcs();

  for (auto &dptr : tu->decls) {
    if (auto gv = dynamic_cast<GlobalVarDecl *>(dptr.get())) {
      if (gv->is_extern)
        continue;
      if (gv->var && gv->var->type)
        global_var_types[gv->var->name] = gv->var->type.get();
    }
  }

  std::ostringstream s;
  s << RISK_RUNTIME_SOURCE;

  {
    std::vector<std::string> extern_headers;
    std::unordered_set<std::string> seen_headers;
    for (auto &dptr : tu->decls) {
      if (auto f = dynamic_cast<FuncDecl *>(dptr.get())) {
        if (!f->is_extern || f->extern_header.empty())
          continue;
        if (seen_headers.insert(f->extern_header).second)
          extern_headers.push_back(f->extern_header);
      }
      if (auto b = dynamic_cast<BlockDecl *>(dptr.get())) {
        if (!b->is_extern || b->extern_header.empty())
          continue;
        if (seen_headers.insert(b->extern_header).second)
          extern_headers.push_back(b->extern_header);
      }
      if (auto o = dynamic_cast<OptDecl *>(dptr.get())) {
        if (!o->is_extern || o->extern_header.empty())
          continue;
        if (seen_headers.insert(o->extern_header).second)
          extern_headers.push_back(o->extern_header);
      }
      if (auto gv = dynamic_cast<GlobalVarDecl *>(dptr.get())) {
        if (!gv->is_extern || gv->extern_header.empty())
          continue;
        if (seen_headers.insert(gv->extern_header).second)
          extern_headers.push_back(gv->extern_header);
      }
    }
    for (auto &h : extern_headers)
      s << "#include " << h << "\n";
    if (!extern_headers.empty())
      s << "\n";
  }

  s << "typedef struct { void* env; void* fn; } risk_closure_t;\n\n";
  s << "typedef struct risk_array_t risk_array_t;\n";
  s << "typedef struct risk_hashmap_t risk_hashmap_t;\n\n";

  auto is_runtime_init = [&](Expr *init, TypeNode *type_node) -> bool {
    if (!init)
      return false;

    if (dynamic_cast<ArrayLiteralExpr *>(init)) {
      if (type_node) {
        if (type_node->name == "Array" && type_node->params.size() == 1)
          return true;
        if (type_node->name == "Hashmap")
          return true;
      }
      return false;
    }

    if (auto bl = dynamic_cast<BlockLiteralExpr *>(init)) {
      if (expr_type_map) {
        auto it = expr_type_map->find(bl);
        if (it != expr_type_map->end() &&
            it->second.name.rfind("Hashmap<", 0) == 0)
          return true;
      }
      return true;
    }

    if (type_node) {
      if (type_node->name == "Hashmap")
        return true;
      if (type_node->name == "Array" && type_node->params.size() == 1)
        return true;
    }
    return false;
  };

  auto emit_global_decl_zero = [&](std::ostream &os, VarDeclStmt *vs) {
    if (!vs)
      return;
    std::string t = ctype_from_tn(vs->type.get());
    bool is_struct = vs->type && find_block_decl(tu, vs->type->name) != nullptr;
    os << t << " " << vs->name;
    os << (is_struct ? " = {0}" : " = NULL");
    os << ";\n";
  };

  auto emit_global_assign = [&](std::ostream &os, VarDeclStmt *vs) {
    if (!vs || !vs->init)
      return;
    bool is_hashmap = vs->type && vs->type->name == "Hashmap";
    os << "    " << vs->name << " = ";
    if (is_hashmap && dynamic_cast<ArrayLiteralExpr *>(vs->init.get())) {
      auto al = dynamic_cast<ArrayLiteralExpr *>(vs->init.get());
      if (al->elements.empty() && vs->type && vs->type->params.size() == 2) {
        std::string key_type = ctype_from_tn(vs->type->params[0].get());
        std::string val_type = ctype_from_tn(vs->type->params[1].get());
        int is_string_key =
            (vs->type->params[0] && vs->type->params[0]->name == "string") ? 1
                                                                           : 0;
        os << "risk_hashmap_create(sizeof(" << key_type << "), sizeof("
           << val_type << "), " << is_string_key << ")";
      }
    } else {
      emit_expr(os, vs->init.get());
    }
    os << ";\n";
  };

  std::ostringstream deferred_global_inits;
  bool has_deferred_globals = false;

  for (auto &dptr : tu->decls) {
    if (auto b = dynamic_cast<OptDecl *>(dptr.get())) {
      if (b->is_extern)
        continue;
      std::string enum_name = mangle(b->name);
      s << "typedef enum " << enum_name << " {\n";
      for (size_t i = 0; i < b->fields.size(); ++i) {
        s << "    " << mangle(b->name) << "_" << b->fields[i].name;
        if (b->fields[i].value) {
          s << " = " << *b->fields[i].value;
        }
        if (i + 1 < b->fields.size()) {
          s << ",";
        }
        s << "\n";
      }
      s << "} " << enum_name << ";\n\n";
    }
  }

  for (auto &dptr : tu->decls) {
    if (auto b = dynamic_cast<BlockDecl *>(dptr.get())) {
      if (!b->generic_params.empty())
        continue;
      if (b->is_extern)
        continue;
      std::string struct_name = mangle(b->name);
      s << "typedef struct " << struct_name << " " << struct_name << ";\n";
    }
  }
  s << "\n";

  for (auto &dptr : tu->decls) {
    if (auto b = dynamic_cast<BlockDecl *>(dptr.get())) {
      if (!b->generic_params.empty())
        continue;
      if (b->is_extern)
        continue;
      std::string struct_name = mangle(b->name);
      s << "struct " << struct_name << " {\n";
      for (auto &f : b->fields) {
        std::string ctype = ctype_from_tn(f.type.get());
        std::string field_name = f.name;
        std::string array_suffix = "";

        if (f.type && f.type->name == "Array" && f.type->params.size() == 2) {

          auto [base, suffix] = flatten_static_array(f.type.get());
          ctype = base;
          array_suffix = suffix;
        } else {
          auto [base, suffix] = split_c_array_suffix(ctype);
          ctype = base;
          array_suffix = suffix;
        }

        s << "    " << ctype << " " << field_name << array_suffix << ";\n";
      }
      s << "};\n\n";
    }
  }

  for (auto &dptr : tu->decls) {
    if (auto f = dynamic_cast<FuncDecl *>(dptr.get())) {
      if (!f->generic_params.empty())
        continue;
      bool is_arr = false;
      std::string base, suffix;
      func_return_ctype(f->return_type.get(), is_arr, base, suffix);
    }
  }
  if (!array_return_wrapper_typedefs.empty())
    s << "/* Wrapper structs for functions returning fixed-size arrays */\n"
      << array_return_wrapper_typedefs << "\n";

  if (!helper_prototypes.empty())
    s << "/* Lambda factory prototypes */\n" << helper_prototypes << "\n";

  for (auto &dptr : tu->decls) {
    if (auto f = dynamic_cast<FuncDecl *>(dptr.get())) {

      if (!f->generic_params.empty())
        continue;

      if (f->is_extern)
        continue;
      bool ret_is_array = false;
      std::string ret_base, ret_suffix;
      std::string ret = func_return_ctype(f->return_type.get(), ret_is_array,
                                          ret_base, ret_suffix);
      bool fwd_is_this_ref =
          this_ref_funcs.find(f->name) != this_ref_funcs.end() &&
          !f->params.empty();
      s << ret << " " << mangle(f->name) << "(";
      for (size_t i = 0; i < f->params.size(); ++i) {
        if (i)
          s << ", ";

        std::string param_type = ctype_from_tn(f->params[i].type.get());
        std::string param_name = f->params[i].name;
        std::string array_suffix = "";

        if (i == 0 && fwd_is_this_ref) {
          param_type = ctype_from_type_name(f->params[0].type->name) + "*";
        } else if (f->params[i].type && f->params[i].type->name == "Array" &&
                   f->params[i].type->params.size() == 2) {
          auto [base, suffix] = flatten_static_array(f->params[i].type.get());
          param_type = base;
          array_suffix = suffix;
        } else if (std::string pab, pas;
                   flatten_ptr_to_array(f->params[i].type.get(), pab, pas)) {
          param_type = pab + " (*";
          array_suffix = ")" + pas;
        } else {
          auto [base, suffix] = split_c_array_suffix(param_type);
          param_type = base;
          array_suffix = suffix;
        }

        if (f->params[i].noalias && !param_type.empty() &&
            param_type.back() == '*') {
          param_type += " restrict";
        }

        s << param_type << " " << param_name << array_suffix;
      }
      s << ");\n";
    }
  }
  s << "\n";

  for (auto &dptr : tu->decls) {
    if (auto f = dynamic_cast<FuncDecl *>(dptr.get())) {
      if (!f->generic_params.empty())
        continue;
      if (!f->is_definition)
        continue;
      bool ret_is_array = false;
      std::string ret_base, ret_suffix;
      std::string ret = func_return_ctype(f->return_type.get(), ret_is_array,
                                          ret_base, ret_suffix);
      std::string tramp = mangle(f->name) + "_trampoline";
      bool tramp_is_this_ref =
          this_ref_funcs.find(f->name) != this_ref_funcs.end() &&
          !f->params.empty();
      s << "static " << ret << " " << tramp << "(void* _env";
      for (size_t i = 0; i < f->params.size(); ++i) {
        std::string param_type = ctype_from_tn(f->params[i].type.get());
        std::string array_suffix = "";
        if (i == 0 && tramp_is_this_ref) {
          param_type = ctype_from_type_name(f->params[0].type->name) + "*";
        } else if (f->params[i].type && f->params[i].type->name == "Array" &&
                   f->params[i].type->params.size() == 2) {
          auto [base, suffix] = flatten_static_array(f->params[i].type.get());
          param_type = base;
          array_suffix = suffix;
        } else if (std::string pab, pas;
                   flatten_ptr_to_array(f->params[i].type.get(), pab, pas)) {
          param_type = pab + " (*";
          array_suffix = ")" + pas;
        } else {
          auto [base, suffix] = split_c_array_suffix(param_type);
          param_type = base;
          array_suffix = suffix;
        }
        s << ", " << param_type << " " << f->params[i].name << array_suffix;
      }
      s << ") { (void)_env; ";
      if (ret != "void")
        s << "return ";
      s << mangle(f->name) << "(";
      for (size_t i = 0; i < f->params.size(); ++i) {
        if (i)
          s << ", ";
        s << f->params[i].name;
      }
      s << "); }\n";
      std::string clname = mangle(f->name) + "_closure";
      s << "static risk_closure_t " << clname << " = { NULL, (void*)&" << tramp
        << " };\n";
    }
  }
  s << "\n";

  for (auto &d : tu->decls) {
    if (auto gv = dynamic_cast<GlobalVarDecl *>(d.get())) {
      if (gv->is_extern)
        continue;
      VarDeclStmt *vs = gv->var.get();
      if (vs && vs->init &&
          is_runtime_init(vs->init.get(),
                          vs->type ? vs->type.get() : nullptr)) {
        emit_global_decl_zero(s, vs);
        emit_global_assign(deferred_global_inits, vs);
        has_deferred_globals = true;
      } else {
        emit_stmt(s, vs, 0);
      }
    }
  }

  if (has_deferred_globals) {
    s << "__attribute__((constructor)) static void __risk_global_init__(void) "
         "{\n";
    s << deferred_global_inits.str();
    s << "}\n\n";
  }

  for (auto &dptr : tu->decls) {
    if (auto f = dynamic_cast<FuncDecl *>(dptr.get())) {
      if (!f->generic_params.empty())
        continue;
      if (f->is_extern)
        continue;
      if (!f->is_definition) {
        bool ret_is_array = false;
        std::string ret_base, ret_suffix;
        std::string ret = func_return_ctype(f->return_type.get(), ret_is_array,
                                            ret_base, ret_suffix);
        s << ret << " " << mangle(f->name) << "(";
        for (size_t i = 0; i < f->params.size(); ++i) {
          if (i)
            s << ", ";

          std::string param_type = ctype_from_tn(f->params[i].type.get());
          std::string param_name = f->params[i].name;
          std::string array_suffix = "";

          if (f->params[i].type && f->params[i].type->name == "Array" &&
              f->params[i].type->params.size() == 2) {
            auto [base, suffix] = flatten_static_array(f->params[i].type.get());
            param_type = base;
            array_suffix = suffix;
          } else if (std::string pab, pas;
                     flatten_ptr_to_array(f->params[i].type.get(), pab, pas)) {
            param_type = pab + " (*";
            array_suffix = ")" + pas;
          } else {
            auto [base, suffix] = split_c_array_suffix(param_type);
            param_type = base;
            array_suffix = suffix;
          }

          s << param_type << " " << param_name << array_suffix;
        }
        s << ");\n\n";
      }
    }
  }

  bool has_main = false;
  bool main_takes_args = false;
  for (auto &dptr : tu->decls) {
    if (auto f = dynamic_cast<FuncDecl *>(dptr.get())) {
      if (!f->generic_params.empty())
        continue;
      if (f->is_definition) {
        if (f->name == "main") {
          has_main = true;
          main_takes_args = !f->params.empty();
        }
        emit_func(f, s);
      }
    }
  }

  if (!helper_code.empty()) {
    s << "\n/* Helpers for lambdas and immediate wrappers */\n";
    s << helper_code << "\n";
  }

  if (has_main) {

    if (debug_lines_enabled) {
      s << "\n#line 1 \"<risk-runtime-trampoline>\"\n";
    }
    s << "int main(int argc, char** argv) {\n";
    if (main_takes_args) {

      s << "    risk_array_t* __risk_args = risk_array_create(sizeof(const "
           "char*));\n";
      s << "    for (int __risk_i = 1; __risk_i < argc; __risk_i++) {\n";
      s << "        const char* __risk_argp = argv[__risk_i];\n";
      s << "        risk_array_push(__risk_args, &__risk_argp);\n";
      s << "    }\n";
      s << "    int __risk_rc = (int)" << mangle("main") << "(__risk_args);\n";
      s << "    risk_array_free(__risk_args);\n";
      s << "    return __risk_rc;\n";
    } else {
      s << "    return " << mangle("main") << "();\n";
    }
    s << "}\n";
  }

  c_source = s.str();

  FILE *out = fopen(out_path.c_str(), "wb");
  if (!out) {
    std::cerr << "Failed to open output C file: " << out_path << "\n";
    return false;
  }
  fwrite(c_source.data(), 1, c_source.size(), out);
  fclose(out);
  return true;
}

static std::string escape_c_literal(const std::string &s) {
  std::string out;
  for (char c : s) {
    if (c == '\n')
      out += "\\n";
    else if (c == '\t')
      out += "\\t";
    else if (c == '\"')
      out += "\\\"";
    else if (c == '\\')
      out += "\\\\";
    else
      out.push_back(c);
  }
  return out;
}

std::string CodeGen::ctype_from_tn(TypeNode *tn) {
  if (!tn)
    return std::string("void");

  if (type_aliases) {
    auto alias_it = type_aliases->find(tn->name);
    if (alias_it != type_aliases->end()) {

      std::string resolved_type = alias_it->second;

      std::string c_type = ctype_from_type_name(resolved_type);

      if (c_type != "void*") {
        return c_type;
      }

      if (!resolved_type.empty()) {
        return resolved_type;
      }
    }
  }

  if (tn->name == "i8")
    return "int8_t";
  if (tn->name == "i16")
    return "int16_t";
  if (tn->name == "i32")
    return "int32_t";
  if (tn->name == "i64")
    return "int64_t";
  if (tn->name == "u8")
    return "uint8_t";
  if (tn->name == "u16")
    return "uint16_t";
  if (tn->name == "u32")
    return "uint32_t";
  if (tn->name == "u64")
    return "uint64_t";
  if (tn->name == "f32")
    return "float";
  if (tn->name == "f64")
    return "double";
  if (tn->name == "bool")
    return "bool";
  if (tn->name == "char")
    return "char";
  if (tn->name == "string")
    return "const char*";
  if (tn->name == "void")
    return "void";
  if (tn->name == "func")
    return "risk_closure_t*";

  if ((tn->name == "ptr" || tn->name == "ref") && !tn->params.empty()) {

    std::string base, suffix;
    if (flatten_ptr_to_array(tn, base, suffix))
      return base + " (*)" + suffix;
    return ctype_from_tn(tn->params[0].get()) + "*";
  }
  if (tn->name == "opt" && !tn->params.empty())
    return ctype_from_tn(tn->params[0].get()) + "*";
  if (tn->name == "Array" && !tn->params.empty()) {

    if (tn->params.size() == 2) {

      std::string elem_type = ctype_from_tn(tn->params[0].get());

      std::string size_str = tn->params[1]->name;
      if (size_str.rfind("__size__:", 0) == 0) {
        size_str = size_str.substr(9);
      }
      return elem_type + "[" + size_str + "]";
    } else {

      return "risk_array_t*";
    }
  }
  if (tn->name == "Hashmap" && !tn->params.empty())
    return "risk_hashmap_t*";

  if (auto *bd = find_block_decl(tu, tn->name)) {
    return block_struct_name(bd);
  }

  if (find_opts_decl(tu, tn->name)) {
    return mangle(tn->name);
  }

  if (tn->name == "const" && !tn->params.empty()) {
    TypeNode *inner_tn = tn->params[0].get();
    if (inner_tn &&
        (inner_tn->name == "ptr" || inner_tn->name == "ref" ||
         inner_tn->name == "opt") &&
        !inner_tn->params.empty()) {

      std::string pointee = ctype_from_tn(inner_tn->params[0].get());
      return pointee + "* const";
    }

    std::string inner = ctype_from_tn(inner_tn);
    return "const " + inner;
  }

  return "void*";
}

std::string CodeGen::ctype_from_type_name(const std::string &type_name) {
  if (type_name == "i32")
    return "int32_t";
  if (type_name == "i64")
    return "int64_t";
  if (type_name == "i16")
    return "int16_t";
  if (type_name == "i8")
    return "int8_t";
  if (type_name == "u32")
    return "uint32_t";
  if (type_name == "u64")
    return "uint64_t";
  if (type_name == "u16")
    return "uint16_t";
  if (type_name == "u8")
    return "uint8_t";
  if (type_name == "f64")
    return "double";
  if (type_name == "f32")
    return "float";
  if (type_name == "string")
    return "const char*";
  if (type_name == "char")
    return "char";
  if (type_name.find("func(") == 0)
    return "risk_closure_t*";
  if (type_name == "bool")
    return "bool";

  if (type_name.rfind("Hashmap<", 0) == 0) {

    return "risk_hashmap_t*";
  }
  if (type_name.rfind("Array<", 0) == 0) {
    size_t p = type_name.find('<');
    size_t q = type_name.rfind('>');
    if (p != std::string::npos && q != std::string::npos && q > p + 1) {
      std::string contents = type_name.substr(p + 1, q - p - 1);
      size_t comma = contents.find(',');
      if (comma != std::string::npos) {

        std::string elem_type = contents.substr(0, comma);
        std::string size = contents.substr(comma + 1);

        size.erase(0, size.find_first_not_of(" \t"));
        size.erase(size.find_last_not_of(" \t") + 1);

        if (size.rfind("__size__:", 0) == 0) {
          size = size.substr(9);
        }
        return ctype_from_type_name(elem_type) + "[" + size + "]";
      } else {

        return "risk_array_t*";
      }
    }
  }
  if (type_name.rfind("ptr<", 0) == 0 || type_name.rfind("ref<", 0) == 0) {
    size_t p = type_name.find('<');
    size_t q = type_name.rfind('>');
    if (p != std::string::npos && q != std::string::npos && q > p + 1) {
      std::string inner = type_name.substr(p + 1, q - p - 1);
      std::string inner_ctype = ctype_from_type_name(inner);
      auto [base, suffix] = split_c_array_suffix(inner_ctype);
      if (!suffix.empty()) {

        return base + " (*)" + suffix;
      }
      return inner_ctype + "*";
    }
  }

  if (auto *bd = find_block_decl(tu, type_name)) {
    return block_struct_name(bd);
  }

  if (find_opts_decl(tu, type_name)) {
    return mangle(type_name);
  }

  if (type_name.rfind("const<", 0) == 0) {
    size_t p = type_name.find('<');
    size_t q = type_name.rfind('>');
    if (p != std::string::npos && q != std::string::npos && q > p + 1) {
      std::string inner = type_name.substr(p + 1, q - p - 1);

      if (inner.rfind("ptr<", 0) == 0 || inner.rfind("ref<", 0) == 0) {
        size_t ip = inner.find('<'), iq = inner.rfind('>');
        if (ip != std::string::npos && iq != std::string::npos && iq > ip + 1) {
          std::string pointee = inner.substr(ip + 1, iq - ip - 1);
          return ctype_from_type_name(pointee) + "* const";
        }
      }
      return "const " + ctype_from_type_name(inner);
    }
  }
  if (type_name.rfind("const ", 0) == 0) {
    std::string inner = type_name.substr(6);

    if (inner.rfind("ptr<", 0) == 0 || inner.rfind("ref<", 0) == 0) {
      size_t ip = inner.find('<'), iq = inner.rfind('>');
      if (ip != std::string::npos && iq != std::string::npos && iq > ip + 1) {
        std::string pointee = inner.substr(ip + 1, iq - ip - 1);
        return ctype_from_type_name(pointee) + "* const";
      }
    }
    return "const " + ctype_from_type_name(inner);
  }
  return "void*";
}

std::pair<std::string, std::string>
CodeGen::flatten_static_array(TypeNode *tn) {
  if (tn && tn->name == "Array" && tn->params.size() == 2) {
    std::string size_str = tn->params[1]->name;
    if (size_str.rfind("__size__:", 0) == 0)
      size_str = size_str.substr(9);
    auto [base, inner_suffix] = flatten_static_array(tn->params[0].get());
    return {base, "[" + size_str + "]" + inner_suffix};
  }
  return {ctype_from_tn(tn), ""};
}

bool CodeGen::flatten_ptr_to_array(TypeNode *tn, std::string &base,
                                   std::string &suffix) {
  if (!tn || (tn->name != "ptr" && tn->name != "ref") || tn->params.empty())
    return false;
  TypeNode *inner = tn->params[0].get();
  if (!inner || inner->name != "Array" || inner->params.size() != 2)
    return false;
  std::tie(base, suffix) = flatten_static_array(inner);
  return true;
}

std::pair<std::string, std::string>
CodeGen::split_c_array_suffix(const std::string &ctype) {
  size_t bracket_pos = ctype.find('[');
  if (bracket_pos == std::string::npos)
    return {ctype, ""};
  return {ctype.substr(0, bracket_pos), ctype.substr(bracket_pos)};
}

std::string CodeGen::func_return_ctype(TypeNode *ret_tn, bool &is_array,
                                       std::string &array_base,
                                       std::string &array_suffix) {
  is_array = false;
  array_base.clear();
  array_suffix.clear();
  if (!ret_tn)
    return "void";

  std::string base, suffix;
  if (ret_tn->name == "Array" && ret_tn->params.size() == 2) {
    std::tie(base, suffix) = flatten_static_array(ret_tn);
  } else {

    std::tie(base, suffix) = split_c_array_suffix(ctype_from_tn(ret_tn));
  }

  if (suffix.empty())
    return base;

  is_array = true;
  array_base = base;
  array_suffix = suffix;

  std::string key = base + suffix;
  auto it = array_return_wrappers.find(key);
  if (it != array_return_wrappers.end())
    return it->second;

  std::string wrapper_name = "risk_arrret_" + fresh_id("t");
  array_return_wrappers[key] = wrapper_name;
  array_return_wrapper_typedefs += "typedef struct { " + base + " arr" +
                                   suffix + "; } " + wrapper_name + ";\n";
  return wrapper_name;
}

std::string CodeGen::func_return_ctype_from_string(const std::string &ret) {
  std::string ctype = ctype_from_type_name(ret);
  auto [base, suffix] = split_c_array_suffix(ctype);
  if (suffix.empty())
    return ctype;

  std::string key = base + suffix;
  auto it = array_return_wrappers.find(key);
  if (it != array_return_wrappers.end())
    return it->second;

  std::string wrapper_name = "risk_arrret_" + fresh_id("t");
  array_return_wrappers[key] = wrapper_name;
  array_return_wrapper_typedefs += "typedef struct { " + base + " arr" +
                                   suffix + "; } " + wrapper_name + ";\n";
  return wrapper_name;
}

std::string CodeGen::wrap_array_return_expr(Expr *e, const std::string &wrapper,
                                            const std::string &array_base,
                                            const std::string &array_suffix) {
  std::ostringstream exprstr;
  emit_expr(exprstr, e);
  std::string src = exprstr.str();

  if (dynamic_cast<ArrayLiteralExpr *>(e))
    src = "(" + array_base + array_suffix + ")" + src;

  std::ostringstream os;
  os << "({ " << wrapper << " __risk_ret; memcpy(__risk_ret.arr, (" << src
     << "), sizeof(__risk_ret.arr)); __risk_ret; })";
  return os.str();
}

void CodeGen::ensure_closure_structs_and_helpers_for_lambda_with_id(
    LambdaExpr *lam, const std::string &id) {
  std::string envname = make_lambda_env_name(id);
  std::ostringstream h;
  h << "typedef struct " << envname << " {\n";
  for (size_t i = 0; i < lam->captures.size(); ++i) {
    std::string cap = lam->captures[i];
    std::string tname = "void*";
    if (i < lam->capture_type_names.size())
      tname = ctype_from_type_name(lam->capture_type_names[i]);
    h << "    " << tname << " " << cap << ";\n";
  }
  h << "} " << envname << ";\n\n";

  std::string wrapper = make_lambda_wrapper_name(id);
  std::string factory = make_lambda_factory_name(id);

  bool ret_is_array = false;
  std::string ret_base, ret_suffix;
  std::string ret = func_return_ctype(lam->return_type.get(), ret_is_array,
                                      ret_base, ret_suffix);

  std::ostringstream wrap;
  wrap << ret << " " << wrapper << "(void* _env";
  for (size_t i = 0; i < lam->params.size(); ++i)
    wrap << ", " << ctype_from_tn(lam->params[i].second.get()) << " "
         << lam->params[i].first;
  wrap << ") {\n";
  wrap << "    " << envname << "* env = (" << envname << "*)_env;\n";
  auto old_map = capture_map;
  capture_map.clear();
  for (auto &cap : lam->captures)
    capture_map[cap] = std::string("env->") + cap;

  bool saved_is_array = current_return_is_array;
  std::string saved_wrapper = current_return_wrapper;
  std::string saved_base = current_return_array_base;
  std::string saved_suffix = current_return_array_suffix;
  current_return_is_array = ret_is_array;
  current_return_wrapper = ret_is_array ? ret : "";
  current_return_array_base = ret_base;
  current_return_array_suffix = ret_suffix;

  std::ostringstream body_os;
  emit_body_with_defers(body_os, lam->body, ret, 4);

  current_return_is_array = saved_is_array;
  current_return_wrapper = saved_wrapper;
  current_return_array_base = saved_base;
  current_return_array_suffix = saved_suffix;

  capture_map = old_map;
  wrap << body_os.str();
  wrap << "}\n\n";

  std::ostringstream proto;
  proto << "static risk_closure_t* " << factory << "(";
  for (size_t i = 0; i < lam->captures.size(); ++i) {
    if (i)
      proto << ", ";
    std::string tname = "void*";
    if (i < lam->capture_type_names.size())
      tname = ctype_from_type_name(lam->capture_type_names[i]);
    proto << tname << " _" << lam->captures[i];
  }
  proto << ");\n";

  std::ostringstream fac;
  fac << "static risk_closure_t* " << factory << "(";
  for (size_t i = 0; i < lam->captures.size(); ++i) {
    if (i)
      fac << ", ";
    std::string tname = "void*";
    if (i < lam->capture_type_names.size())
      tname = ctype_from_type_name(lam->capture_type_names[i]);
    fac << tname << " _" << lam->captures[i];
  }
  fac << ") {\n";
  fac << "    " << envname << "* env = (" << envname << "*)malloc(sizeof("
      << envname << "));\n";
  for (size_t i = 0; i < lam->captures.size(); ++i)
    fac << "    env->" << lam->captures[i] << " = _" << lam->captures[i]
        << ";\n";
  fac << "    risk_closure_t* cl = "
         "(risk_closure_t*)malloc(sizeof(risk_closure_t));\n";
  fac << "    cl->env = (void*)env;\n";
  fac << "    cl->fn = (void*)&" << wrapper << ";\n";
  fac << "    return cl;\n";
  fac << "}\n\n";

  helper_prototypes += proto.str();
  helper_code += h.str();
  helper_code += wrap.str();
  helper_code += fac.str();
}

void CodeGen::ensure_immediate_wrapper_for_lambda_with_id(
    LambdaExpr *lam, const std::string &id) {
  std::string wrapper = "imm_wrapper_" + id;

  bool ret_is_array = false;
  std::string ret_base, ret_suffix;
  std::string ret = func_return_ctype(lam->return_type.get(), ret_is_array,
                                      ret_base, ret_suffix);

  std::ostringstream proto;
  proto << ret << " " << wrapper << "(";
  for (size_t i = 0; i < lam->captures.size(); ++i) {
    if (i)
      proto << ", ";
    std::string tname = "void*";
    if (i < lam->capture_type_names.size())
      tname = ctype_from_type_name(lam->capture_type_names[i]);
    proto << tname << " _cap_" << lam->captures[i];
  }
  if (!lam->captures.empty() && !lam->params.empty())
    proto << ", ";
  for (size_t i = 0; i < lam->params.size(); ++i) {
    if (i)
      proto << ", ";
    proto << ctype_from_tn(lam->params[i].second.get()) << " "
          << lam->params[i].first;
  }
  proto << ");\n";
  helper_prototypes += proto.str();

  std::ostringstream def;
  def << ret << " " << wrapper << "(";
  for (size_t i = 0; i < lam->captures.size(); ++i) {
    if (i)
      def << ", ";
    std::string tname = "void*";
    if (i < lam->capture_type_names.size())
      tname = ctype_from_type_name(lam->capture_type_names[i]);
    def << tname << " _cap_" << lam->captures[i];
  }
  if (!lam->captures.empty() && !lam->params.empty())
    def << ", ";
  for (size_t i = 0; i < lam->params.size(); ++i) {
    if (i)
      def << ", ";
    def << ctype_from_tn(lam->params[i].second.get()) << " "
        << lam->params[i].first;
  }
  def << ") {\n";

  auto old_map = capture_map;
  capture_map.clear();
  for (auto &cap : lam->captures)
    capture_map[cap] = std::string("_cap_") + cap;

  bool saved_is_array = current_return_is_array;
  std::string saved_wrapper = current_return_wrapper;
  std::string saved_base = current_return_array_base;
  std::string saved_suffix = current_return_array_suffix;
  current_return_is_array = ret_is_array;
  current_return_wrapper = ret_is_array ? ret : "";
  current_return_array_base = ret_base;
  current_return_array_suffix = ret_suffix;

  std::ostringstream body_os;
  emit_body_with_defers(body_os, lam->body, ret, 4);

  current_return_is_array = saved_is_array;
  current_return_wrapper = saved_wrapper;
  current_return_array_base = saved_base;
  current_return_array_suffix = saved_suffix;

  capture_map = old_map;

  def << body_os.str();
  def << "}\n\n";

  helper_code += def.str();
}

static std::string type_node_to_string(TypeNode *tn) {
  if (!tn)
    return std::string();
  if (tn->name == "func") {
    std::string s = "func(";
    for (size_t i = 0; i < tn->params.size(); ++i) {
      if (i)
        s += ",";
      s += type_node_to_string(tn->params[i].get());
    }
    s += ")->";
    if (tn->return_type)
      s += type_node_to_string(tn->return_type.get());
    else
      s += "void";
    return s;
  }
  if (tn->params.empty())
    return tn->name;
  std::string s = tn->name + "<";
  for (size_t i = 0; i < tn->params.size(); ++i) {
    if (i)
      s += ",";
    s += type_node_to_string(tn->params[i].get());
  }
  s += ">";
  return s;
}

static std::string infer_type_name_for_expr(
    Expr *e, const std::unordered_map<Expr *, TypeInfo> *expr_type_map,
    const std::unordered_map<std::string, TypeNode *> &params,
    const std::unordered_map<std::string, TypeNode *> &locals) {
  if (!e)
    return "";
  if (expr_type_map) {
    auto it = expr_type_map->find(e);
    if (it != expr_type_map->end())
      return it->second.name;
  }
  if (auto id = dynamic_cast<IdentifierExpr *>(e)) {
    auto pit = params.find(id->name);
    if (pit != params.end())
      return type_node_to_string(pit->second);
    auto lit = locals.find(id->name);
    if (lit != locals.end())
      return type_node_to_string(lit->second);
    return "";
  }
  if (auto num = dynamic_cast<NumberExpr *>(e)) {
    return (num->literal.find('.') != std::string::npos) ? "f64" : "i32";
  }
  if (auto s = dynamic_cast<StringExpr *>(e))
    return "string";
  if (auto c = dynamic_cast<CharExpr *>(e))
    return "char";
  if (auto un = dynamic_cast<UnaryExpr *>(e)) {
    if (un->op == "*") {
      std::string rt = infer_type_name_for_expr(un->rhs.get(), expr_type_map,
                                                params, locals);
      if (rt.rfind("ptr<", 0) == 0 || rt.rfind("ref<", 0) == 0) {
        size_t p1 = rt.find('<'), p2 = rt.rfind('>');
        if (p1 != std::string::npos && p2 != std::string::npos && p2 > p1 + 1)
          return rt.substr(p1 + 1, p2 - p1 - 1);
      }
      if (auto rhs_id = dynamic_cast<IdentifierExpr *>(un->rhs.get())) {
        auto lit = locals.find(rhs_id->name);
        if (lit != locals.end()) {
          TypeNode *tn = lit->second;
          if (tn && (tn->name == "ptr" || tn->name == "ref") &&
              !tn->params.empty())
            return type_node_to_string(tn->params[0].get());
        }
        auto pit = params.find(rhs_id->name);
        if (pit != params.end()) {
          TypeNode *tn = pit->second;
          if (tn && (tn->name == "ptr" || tn->name == "ref") &&
              !tn->params.empty())
            return type_node_to_string(tn->params[0].get());
        }
      }
      return "";
    }
    return infer_type_name_for_expr(un->rhs.get(), expr_type_map, params,
                                    locals);
  }
  if (auto cast = dynamic_cast<CastExpr *>(e)) {
    if (cast->target)
      return cast->target->name;
  }
  if (auto mem = dynamic_cast<MemberExpr *>(e)) {

    if (expr_type_map) {
      auto it = expr_type_map->find(e);
      if (it != expr_type_map->end())
        return it->second.name;
    }
  }
  if (auto call = dynamic_cast<CallExpr *>(e)) {

    if (expr_type_map) {
      auto it = expr_type_map->find(e);
      if (it != expr_type_map->end())
        return it->second.name;
    }
  }
  return "";
}

static bool expression_is_pointer_address(
    Expr *arg, const std::unordered_map<Expr *, TypeInfo> *expr_type_map,
    const std::unordered_map<std::string, TypeNode *> &params,
    const std::unordered_map<std::string, TypeNode *> &locals) {
  if (!arg)
    return false;

  if (auto un = dynamic_cast<UnaryExpr *>(arg)) {
    if (un->op == "*")
      return false;
  }

  if (expr_type_map) {
    auto it = expr_type_map->find(arg);
    if (it != expr_type_map->end()) {
      const std::string &tn = it->second.name;
      if (tn.rfind("ptr<", 0) == 0 || tn.rfind("ref<", 0) == 0)
        return true;
    }
  }

  if (auto id = dynamic_cast<IdentifierExpr *>(arg)) {
    auto pit = params.find(id->name);
    if (pit != params.end()) {
      TypeNode *tn = pit->second;
      if (tn && (tn->name == "ptr" || tn->name == "ref"))
        return true;
    }
    auto lit = locals.find(id->name);
    if (lit != locals.end()) {
      TypeNode *tn = lit->second;
      if (tn && (tn->name == "ptr" || tn->name == "ref"))
        return true;
    }
  }

  {
    std::string inf =
        infer_type_name_for_expr(arg, expr_type_map, params, locals);
    if (!inf.empty() &&
        (inf.rfind("ptr<", 0) == 0 || inf.rfind("ref<", 0) == 0))
      return true;
  }
  return false;
}

static void emit_expr_maybe_deref_when_read(
    std::ostream &os, Expr *arg,
    const std::unordered_map<std::string, TypeNode *> &params,
    const std::unordered_map<std::string, TypeNode *> &locals,
    std::function<void(std::ostream &, Expr *)> emit_expr_fn) {
  if (!arg) {
    return;
  }
  if (auto aid = dynamic_cast<IdentifierExpr *>(arg)) {
    TypeNode *decltn = nullptr;
    auto pit = params.find(aid->name);
    if (pit != params.end())
      decltn = pit->second;
    else {
      auto lit = locals.find(aid->name);
      if (lit != locals.end())
        decltn = lit->second;
    }
    if (decltn && decltn->name == "ref") {
      os << "*(" << aid->name << ")";
      return;
    }
  }

  if (emit_expr_fn && os && arg) {
    emit_expr_fn(os, arg);
  }
}

void CodeGen::emit_lvalue(std::ostream &os, Expr *e) {
  if (!e)
    return;

  if (auto id = dynamic_cast<IdentifierExpr *>(e)) {
    TypeNode *tn = nullptr;
    auto pit = current_param_types.find(id->name);
    if (pit != current_param_types.end())
      tn = pit->second;
    else {
      auto lit = current_local_var_types.find(id->name);
      if (lit != current_local_var_types.end())
        tn = lit->second;
    }
    if (tn && tn->name == "ref") {
      os << "*(" << id->name << ")";
      return;
    }
    os << id->name;
    return;
  }

  if (auto mem = dynamic_cast<MemberExpr *>(e)) {

    if (auto obj_id = dynamic_cast<IdentifierExpr *>(mem->object.get())) {
      if (auto *od = find_opts_decl(tu, obj_id->name)) {
        if (od->is_extern) {

          for (auto &f : od->fields) {
            if (f.name == mem->field) {
              os << (f.value ? *f.value : 0);
              return;
            }
          }
        }

        os << mangle(obj_id->name) << "_" << mem->field;
        return;
      }
    }

    std::ostringstream obj_os;
    emit_expr(obj_os, mem->object.get());
    std::string obj_str = obj_os.str();

    std::string obj_type;
    if (expr_type_map) {
      auto it = expr_type_map->find(mem->object.get());
      if (it != expr_type_map->end())
        obj_type = it->second.name;
    }
    if (obj_type.empty()) {
      obj_type = infer_type_name_for_expr(mem->object.get(), expr_type_map,
                                          current_param_types,
                                          current_local_var_types);
    }

    if (is_this_ref_object(mem->object.get()))
      obj_type = "ref<" + current_this_ref_block_type + ">";
    bool is_ptr = false;
    std::string inner;
    if (!obj_type.empty()) {
      std::string cleaned = obj_type;

      if (cleaned.rfind("const<", 0) == 0) {
        size_t p = cleaned.find('<');
        size_t q = cleaned.rfind('>');
        if (p != std::string::npos && q != std::string::npos && q > p + 1)
          cleaned = cleaned.substr(p + 1, q - p - 1);
      }
      if (cleaned.rfind("ref<", 0) == 0 || cleaned.rfind("ptr<", 0) == 0) {
        is_ptr = true;
        size_t p = cleaned.find('<'), q = cleaned.rfind('>');
        if (p != std::string::npos && q != std::string::npos && q > p + 1)
          inner = cleaned.substr(p + 1, q - p - 1);
      } else {
        inner = cleaned;
      }
    }
    std::string struct_name;
    if (!inner.empty()) {
      if (auto *bd = find_block_decl(tu, inner))
        struct_name = block_struct_name(bd);
      else
        struct_name = mangle(inner);
    }

    bool is_deref = false;
    if (auto un = dynamic_cast<UnaryExpr *>(mem->object.get())) {
      if (un->op == "*") {
        is_deref = true;
      }
    }

    if (is_deref) {
      os << "(" << obj_str << ")." << mem->field;
    } else if (is_ptr) {
      if (!struct_name.empty())
        os << "((" << struct_name << "*)(" << obj_str << "))->" << mem->field;
      else
        os << "(" << obj_str << ")->" << mem->field;
    } else {
      os << obj_str << "." << mem->field;
    }
    return;
  }

  if (auto un = dynamic_cast<UnaryExpr *>(e)) {
    if (un->op == "*") {
      os << "*(";
      emit_expr(os, un->rhs.get());
      os << ")";
      return;
    }
  }

  emit_expr(os, e);
}

void CodeGen::collect_defers(Stmt *s, std::vector<DeferStmt *> &out) {
  if (!s)
    return;
  if (auto d = dynamic_cast<DeferStmt *>(s)) {
    out.push_back(d);
    return;
  }
  if (auto blk = dynamic_cast<BlockStmt *>(s)) {
    for (auto &st : blk->stmts)
      collect_defers(st.get(), out);
    return;
  }
  if (auto ifs = dynamic_cast<IfStmt *>(s)) {
    collect_defers(ifs->then_branch.get(), out);
    if (ifs->else_branch)
      collect_defers((*ifs->else_branch).get(), out);
    return;
  }
  if (auto wh = dynamic_cast<WhileStmt *>(s)) {
    collect_defers(wh->body.get(), out);
    return;
  }
  if (auto fr = dynamic_cast<ForStmt *>(s)) {
    if (fr->body)
      collect_defers(fr->body.get(), out);
    return;
  }
  if (auto sw = dynamic_cast<SwitchStmt *>(s)) {
    for (auto &c : sw->cases)
      for (auto &st : c.stmts)
        collect_defers(st.get(), out);
    return;
  }
}

void CodeGen::emit_body_with_defers(std::ostream &os, const StmtList &body,
                                    const std::string &ret_ctype, int indent) {
  std::vector<DeferStmt *> defers;
  for (auto &s : body)
    collect_defers(s.get(), defers);

  std::string saved_label = current_defer_cleanup_label;
  std::string saved_ret_var = current_defer_ret_var;
  bool saved_has_ret = current_defer_has_ret_value;
  auto saved_list = current_defer_list;
  auto saved_flag_names = defer_flag_names;

  auto ind = std::string(indent, ' ');
  bool has_ret_val = ret_ctype != "void";

  if (!defers.empty()) {
    current_defer_list = defers;
    current_defer_cleanup_label = fresh_id("defer_cleanup");
    current_defer_has_ret_value = has_ret_val;
    if (has_ret_val) {
      current_defer_ret_var = fresh_id("defer_retval");
      os << ind << ret_ctype << " " << current_defer_ret_var << ";\n";
    } else {
      current_defer_ret_var.clear();
    }
    for (auto *d : defers) {
      std::string flag = fresh_id("defer_flag");
      defer_flag_names[d] = flag;
      os << ind << "int " << flag << " = 0;\n";
    }
  } else {
    current_defer_cleanup_label.clear();
    current_defer_ret_var.clear();
    current_defer_list.clear();
  }

  for (auto &s : body)
    emit_stmt(os, s.get(), indent);

  if (!defers.empty()) {
    os << ind << current_defer_cleanup_label << ":;\n";

    for (auto it = defers.rbegin(); it != defers.rend(); ++it) {
      DeferStmt *d = *it;
      os << ind << "if (" << defer_flag_names[d] << ") {\n";
      if (d->body)
        emit_stmt(os, d->body.get(), indent + 2);
      os << ind << "}\n";
    }
    if (has_ret_val)
      os << ind << "return " << current_defer_ret_var << ";\n";
  }

  current_defer_cleanup_label = saved_label;
  current_defer_ret_var = saved_ret_var;
  current_defer_has_ret_value = saved_has_ret;
  current_defer_list = saved_list;
  defer_flag_names = saved_flag_names;
}

void CodeGen::emit_func(FuncDecl *f, std::ostream &os) {
  current_param_types.clear();
  current_local_var_types.clear();
  capture_map.clear();
  current_this_ref_param_name.clear();
  current_this_ref_block_type.clear();

  for (auto &p : f->params)
    current_param_types[p.name] = p.type.get();

  bool is_this_ref_func =
      this_ref_funcs.find(f->name) != this_ref_funcs.end() &&
      !f->params.empty();
  if (is_this_ref_func) {
    current_this_ref_param_name = f->params[0].name;
    current_this_ref_block_type =
        f->params[0].type ? f->params[0].type->name : "";
  }

  bool ret_is_array = false;
  std::string ret_base, ret_suffix;
  std::string ret = func_return_ctype(f->return_type.get(), ret_is_array,
                                      ret_base, ret_suffix);
  current_return_is_array = ret_is_array;
  current_return_wrapper = ret_is_array ? ret : "";
  current_return_array_base = ret_base;
  current_return_array_suffix = ret_suffix;

  last_emitted_line = -1;
  if (debug_lines_enabled && f->line > 0) {
    os << "\n#line " << f->line << " \"" << tu->filename << "\"\n";
  }

  os << ret << " " << mangle(f->name) << "(";
  for (size_t i = 0; i < f->params.size(); ++i) {
    if (i)
      os << ", ";

    std::string param_type = ctype_from_tn(f->params[i].type.get());
    std::string param_name = f->params[i].name;
    std::string array_suffix = "";

    if (i == 0 && is_this_ref_func) {

      param_type = ctype_from_type_name(current_this_ref_block_type) + "*";
    } else if (f->params[i].type && f->params[i].type->name == "Array" &&
               f->params[i].type->params.size() == 2) {
      auto [base, suffix] = flatten_static_array(f->params[i].type.get());
      param_type = base;
      array_suffix = suffix;
    } else if (std::string pab, pas;
               flatten_ptr_to_array(f->params[i].type.get(), pab, pas)) {

      param_type = pab + " (*";
      array_suffix = ")" + pas;
    } else {
      auto [base, suffix] = split_c_array_suffix(param_type);
      param_type = base;
      array_suffix = suffix;
    }

    if (f->params[i].noalias && !param_type.empty() &&
        param_type.back() == '*') {
      param_type += " restrict";
    }

    os << param_type << " " << param_name << array_suffix;
  }
  os << ") {\n";
  emit_body_with_defers(os, f->body, ret, 4);
  os << "}\n\n";
}
std::string CodeGen::map_risk_type_to_c(const std::string &risk_type) {
  if (risk_type == "u8")
    return "uint8_t";
  if (risk_type == "u16")
    return "uint16_t";
  if (risk_type == "u32")
    return "uint32_t";
  if (risk_type == "u64")
    return "uint64_t";
  if (risk_type == "i8")
    return "int8_t";
  if (risk_type == "i16")
    return "int16_t";
  if (risk_type == "i32")
    return "int32_t";
  if (risk_type == "i64")
    return "int64_t";
  if (risk_type == "f32")
    return "float";
  if (risk_type == "f64")
    return "double";
  if (risk_type == "bool")
    return "bool";
  return "uint8_t";
}

void CodeGen::emit_stmt(std::ostream &os, Stmt *s, int indent) {
  auto ind = std::string(indent, ' ');
  if (!s)
    return;

  if (debug_lines_enabled && s->line > 0 && s->line != last_emitted_line) {

    os << "\n#line " << s->line << " \"" << tu->filename << "\"\n" << ind;
    last_emitted_line = s->line;
  }

  if (auto reg = dynamic_cast<RegisterDecl *>(s)) {
    std::string c_type = map_risk_type_to_c(reg->element_type->name);

    os << "#define " << reg->name << " (*(volatile " << c_type << "*)0x"
       << std::hex << reg->address << std::dec << ")\n";
    return;
  }
  if (auto vs = dynamic_cast<VarDeclStmt *>(s)) {
    std::string t = ctype_from_tn(vs->type.get());

    std::string var_name = vs->name;
    std::string array_suffix = "";
    bool is_static_array = false;
    bool is_hashmap = false;

    if (vs->type && vs->type->name == "Array" && vs->type->params.size() == 2) {

      auto [base, suffix] = flatten_static_array(vs->type.get());
      t = base;
      array_suffix = suffix;
      is_static_array = true;
    } else if (vs->type && vs->type->name == "Hashmap") {
      is_hashmap = true;
    } else if (std::string pab, pas;
               flatten_ptr_to_array(vs->type.get(), pab, pas)) {

      t = pab + " (*";
      array_suffix = ")" + pas;
    } else {

      auto [base, suffix] = split_c_array_suffix(t);
      if (!suffix.empty()) {
        t = base;
        array_suffix = suffix;
        is_static_array = true;
      }
    }

    if (is_static_array && vs->init &&
        dynamic_cast<CallExpr *>(vs->init.get())) {

      os << ind << t << " " << var_name << array_suffix << ";\n";
      os << ind << "memcpy(" << var_name << ", (";
      emit_expr(os, vs->init.get());
      os << ").arr, sizeof(" << var_name << "));\n";
      current_local_var_types[vs->name] = vs->type.get();
      return;
    }

    os << ind << (vs->is_const ? "const " : "") << t << " " << var_name
       << array_suffix;
    if (vs->init) {
      os << " = ";
      if (is_hashmap && dynamic_cast<ArrayLiteralExpr *>(vs->init.get())) {

        auto al = dynamic_cast<ArrayLiteralExpr *>(vs->init.get());
        if (al->elements.empty() && vs->type && vs->type->params.size() == 2) {
          std::string key_type = ctype_from_tn(vs->type->params[0].get());
          std::string val_type = ctype_from_tn(vs->type->params[1].get());
          int is_string_key =
              (vs->type->params[0] && vs->type->params[0]->name == "string")
                  ? 1
                  : 0;
          os << "risk_hashmap_create(sizeof(" << key_type << "), sizeof("
             << val_type << "), " << is_string_key << ")";
        }
      } else if (auto lam = dynamic_cast<LambdaExpr *>(vs->init.get())) {
        std::string id = prepare_lambda(lam);
        std::string fname = make_lambda_factory_name(id);
        os << fname << "(";
        for (size_t i = 0; i < lam->captures.size(); ++i) {
          if (i)
            os << ", ";
          os << lam->captures[i];
        }
        os << ")";
      } else if (is_static_array &&
                 dynamic_cast<ArrayLiteralExpr *>(vs->init.get())) {

        auto al = dynamic_cast<ArrayLiteralExpr *>(vs->init.get());
        os << "{ ";
        for (size_t i = 0; i < al->elements.size(); ++i) {
          if (i)
            os << ", ";
          emit_expr(os, al->elements[i].get());
        }
        os << " }";
      } else {
        bool handled = false;

        bool is_func_type = false;
        if (vs->type && vs->type->name == "func") {
          is_func_type = true;
        } else if (vs->type && type_aliases) {

          auto alias_it = type_aliases->find(vs->type->name);
          if (alias_it != type_aliases->end()) {
            std::string resolved = alias_it->second;
            if (resolved.rfind("func(", 0) == 0) {
              is_func_type = true;
            }
          }
        }

        if (is_func_type) {
          if (auto init_id = dynamic_cast<IdentifierExpr *>(vs->init.get())) {
            if (top_level_funcs.find(init_id->name) != top_level_funcs.end()) {
              os << "&" << mangle(init_id->name) << "_closure";
              handled = true;
            }
          }
        }
        if (!handled) {
          if (vs->type && vs->type->name == "ref") {
            if (auto init_id = dynamic_cast<IdentifierExpr *>(vs->init.get()))
              os << "&" << init_id->name;
            else {
              std::ostringstream expr_os;
              emit_expr(expr_os, vs->init.get());
              os << expr_os.str();
            }
          } else {
            std::ostringstream expr_os;
            emit_expr(expr_os, vs->init.get());
            os << expr_os.str();
          }
        }
      }
    }
    os << ";\n";

    if (!vs->init && vs->type) {
      BlockDecl *b = find_block_decl(tu, vs->type->name);
      if (b) {
        for (auto &field : b->fields) {
          if (field.default_val) {
            os << ind << vs->name << "." << field.name << " = ";

            bool default_is_func_field = false;
            if (field.type) {
              std::string field_type_name =
                  build_type_name_from_node(field.type.get());
              default_is_func_field = (field.type->name == "func" ||
                                       field_type_name.rfind("func(", 0) == 0);
            }

            bool emitted = false;
            if (default_is_func_field) {
              if (auto did =
                      dynamic_cast<IdentifierExpr *>(field.default_val.get())) {
                if (top_level_funcs.find(did->name) != top_level_funcs.end()) {
                  os << "&" << mangle(did->name) << "_closure";
                  emitted = true;
                }
              }
            }
            if (!emitted)
              emit_expr(os, field.default_val.get());
            os << ";\n";
          }
        }
      }
    }

    if (vs->type &&
        ((!vs->init) || dynamic_cast<BlockLiteralExpr *>(vs->init.get())))
      emit_auto_init_call(os, ind, var_name, vs->type->name);

    current_local_var_types[vs->name] = vs->type.get();
    return;
  }

  if (auto es = dynamic_cast<ExprStmt *>(s)) {
    if (auto call = dynamic_cast<CallExpr *>(es->expr.get())) {
      if (auto id = dynamic_cast<IdentifierExpr *>(call->callee.get())) {
        if (id->name == "ioPrint" || id->name == "ioPrintln") {
          if (call->args.empty()) {
            if (id->name == "ioPrint")
              os << ind << "core_print(\"\");\n";
            else
              os << ind << "core_println(\"\");\n";
            return;
          }
          std::string bufname = "__print_buf_" + std::to_string(unique_id++);
          os << ind << "char " << bufname << "[1024];\n";
          os << ind << bufname << "[0] = '\\0';\n";

          for (size_t ai = 0; ai < call->args.size(); ++ai) {
            Expr *arg = call->args[ai].get();

            if (compile_time_map) {
              auto it = compile_time_map->find(arg);
              if (it != compile_time_map->end()) {
                std::string lit = it->second;
                os << ind << "strncat(" << bufname << ", \""
                   << escape_c_literal(lit) << "\", sizeof(" << bufname
                   << ") - strlen(" << bufname << ") - 1);\n";
                continue;
              }
            }

            if (dynamic_cast<StringExpr *>(arg)) {
              std::ostringstream a_os;
              emit_expr(a_os, arg);
              os << ind << "strncat(" << bufname << ", " << a_os.str()
                 << ", sizeof(" << bufname << ") - strlen(" << bufname
                 << ") - 1);\n";
              continue;
            }

            bool is_deref_expr = false;
            if (auto un = dynamic_cast<UnaryExpr *>(arg)) {
              if (un->op == "*")
                is_deref_expr = true;
            }

            bool print_ptr = expression_is_pointer_address(
                arg, expr_type_map, current_param_types,
                current_local_var_types);

            std::string atype;
            if (is_deref_expr) {

              auto un = dynamic_cast<UnaryExpr *>(arg);
              atype = infer_type_name_for_expr(un->rhs.get(), expr_type_map,
                                               current_param_types,
                                               current_local_var_types);
              if (atype.rfind("ptr<", 0) == 0 || atype.rfind("ref<", 0) == 0) {
                size_t p = atype.find('<'), q = atype.rfind('>');
                if (p != std::string::npos && q != std::string::npos &&
                    q > p + 1)
                  atype = atype.substr(p + 1, q - p - 1);
              }
            } else {
              atype = infer_type_name_for_expr(arg, expr_type_map,
                                               current_param_types,
                                               current_local_var_types);

              if (auto aid = dynamic_cast<IdentifierExpr *>(arg)) {
                TypeNode *decltn = nullptr;
                auto pit = current_param_types.find(aid->name);
                if (pit != current_param_types.end())
                  decltn = pit->second;
                else {
                  auto lit = current_local_var_types.find(aid->name);
                  if (lit != current_local_var_types.end())
                    decltn = lit->second;
                }
                if (decltn && decltn->name == "ref" && !decltn->params.empty())
                  atype = type_node_to_string(decltn->params[0].get());
              }

              if (auto call = dynamic_cast<CallExpr *>(arg)) {
                if (auto callee_id =
                        dynamic_cast<IdentifierExpr *>(call->callee.get())) {
                  if (callee_id->name == "length") {
                    atype = "i32";
                  }
                }
              }
            }

            std::string tmp = "__print_arg_" + std::to_string(unique_id++);

            if (print_ptr) {

              os << ind << "char " << tmp << "[64]; snprintf(" << tmp
                 << ", sizeof(" << tmp << "), \"%p\", (void*)(";
              emit_expr(os, arg);
              os << "));\n";
              os << ind << "strncat(" << bufname << ", " << tmp << ", sizeof("
                 << bufname << ") - strlen(" << bufname << ") - 1);\n";
              continue;
            }

            if (atype == "string") {
              std::ostringstream a_os;
              emit_expr(a_os, arg);
              os << ind << "strncat(" << bufname << ", " << a_os.str()
                 << ", sizeof(" << bufname << ") - strlen(" << bufname
                 << ") - 1);\n";
            } else if (atype == "char") {

              os << ind << "char " << tmp << "[2]; snprintf(" << tmp
                 << ", sizeof(" << tmp << "), \"%c\", (unsigned char)(";
              emit_expr_maybe_deref_when_read(
                  os, arg, current_param_types, current_local_var_types,
                  [&](std::ostream &o, Expr *ex) { emit_expr(o, ex); });
              os << "));\n";
              os << ind << "strncat(" << bufname << ", " << tmp << ", sizeof("
                 << bufname << ") - strlen(" << bufname << ") - 1);\n";
            } else if (atype == "bool") {
              os << ind << "char " << tmp << "[8]; snprintf(" << tmp
                 << ", sizeof(" << tmp << "), \"%s\", (";
              emit_expr_maybe_deref_when_read(
                  os, arg, current_param_types, current_local_var_types,
                  [&](std::ostream &o, Expr *ex) { emit_expr(o, ex); });
              os << ") ? \"true\" : \"false\");\n";
              os << ind << "strncat(" << bufname << ", " << tmp << ", sizeof("
                 << bufname << ") - strlen(" << bufname << ") - 1);\n";
            } else if (atype == "i32" || atype == "i16" || atype == "i8" ||
                       atype == "u8" || atype == "u16" || atype == "u32") {
              os << ind << "char " << tmp << "[64]; snprintf(" << tmp
                 << ", sizeof(" << tmp << "), \"%d\", (int)(";
              emit_expr_maybe_deref_when_read(
                  os, arg, current_param_types, current_local_var_types,
                  [&](std::ostream &o, Expr *ex) { emit_expr(o, ex); });
              os << "));\n";
              os << ind << "strncat(" << bufname << ", " << tmp << ", sizeof("
                 << bufname << ") - strlen(" << bufname << ") - 1);\n";
            } else if (atype == "i64" || atype == "u64") {
              os << ind << "char " << tmp << "[64]; snprintf(" << tmp
                 << ", sizeof(" << tmp << "), \"%lld\", (long long)(";
              emit_expr_maybe_deref_when_read(
                  os, arg, current_param_types, current_local_var_types,
                  [&](std::ostream &o, Expr *ex) { emit_expr(o, ex); });
              os << "));\n";
              os << ind << "strncat(" << bufname << ", " << tmp << ", sizeof("
                 << bufname << ") - strlen(" << bufname << ") - 1);\n";
            } else if (atype == "f64" || atype == "f32") {
              os << ind << "char " << tmp << "[128]; snprintf(" << tmp
                 << ", sizeof(" << tmp << "), risk_float_fmt, (double)(";
              emit_expr_maybe_deref_when_read(
                  os, arg, current_param_types, current_local_var_types,
                  [&](std::ostream &o, Expr *ex) { emit_expr(o, ex); });
              os << "));\n";
              os << ind << "strncat(" << bufname << ", " << tmp << ", sizeof("
                 << bufname << ") - strlen(" << bufname << ") - 1);\n";
            } else {

              if (is_deref_expr) {
                std::ostringstream val_os;
                emit_expr(val_os, arg);
                os << ind << "char " << tmp << "[64]; snprintf(" << tmp
                   << ", sizeof(" << tmp << "), \"%p\", (void*)("
                   << val_os.str() << "));\n";
                os << ind << "strncat(" << bufname << ", " << tmp << ", sizeof("
                   << bufname << ") - strlen(" << bufname << ") - 1);\n";
              } else {
                os << ind << "char " << tmp << "[64]; snprintf(" << tmp
                   << ", sizeof(" << tmp << "), \"%p\", (void*)(";
                emit_expr(os, arg);
                os << "));\n";
                os << ind << "strncat(" << bufname << ", " << tmp << ", sizeof("
                   << bufname << ") - strlen(" << bufname << ") - 1);\n";
              }
            }
          }

          if (id->name == "ioPrint")
            os << ind << "core_print(" << bufname << ");\n";
          else
            os << ind << "core_println(" << bufname << ");\n";
          return;
        }
      }
    }
    std::ostringstream expr;
    emit_expr(expr, es->expr.get());
    os << ind << expr.str() << ";\n";
    return;
  }

  if (auto ret = dynamic_cast<ReturnStmt *>(s)) {

    auto emit_return_value = [&](std::ostream &target) {
      if (!ret->expr)
        return;
      if (current_return_is_array &&
          !dynamic_cast<CallExpr *>(ret->expr.get())) {
        target << wrap_array_return_expr(
            ret->expr.get(), current_return_wrapper, current_return_array_base,
            current_return_array_suffix);
      } else {
        emit_expr(target, ret->expr.get());
      }
    };

    if (!current_defer_cleanup_label.empty()) {

      if (ret->expr && current_defer_has_ret_value) {
        std::ostringstream expr;
        emit_return_value(expr);
        os << ind << current_defer_ret_var << " = " << expr.str() << ";\n";
      }
      os << ind << "goto " << current_defer_cleanup_label << ";\n";
      return;
    }
    os << ind << "return";
    if (ret->expr) {
      os << " ";
      std::ostringstream expr;
      emit_return_value(expr);
      os << expr.str();
    }
    os << ";\n";
    return;
  }
  if (auto df = dynamic_cast<DeferStmt *>(s)) {

    auto it = defer_flag_names.find(df);
    if (it != defer_flag_names.end()) {
      os << ind << it->second << " = 1;\n";
    }
    return;
  }
  if (auto br = dynamic_cast<BreakStmt *>(s)) {
    auto ind = std::string(indent, ' ');
    os << ind << "break;\n";
    return;
  }

  if (auto cont = dynamic_cast<ContinueStmt *>(s)) {
    auto ind = std::string(indent, ' ');
    os << ind << "continue;\n";
    return;
  }
  if (auto blk = dynamic_cast<BlockStmt *>(s)) {
    if (blk->is_multi_var_expansion) {

      for (auto &st : blk->stmts)
        emit_stmt(os, st.get(), indent);
    } else {
      os << ind << "{\n";
      for (auto &st : blk->stmts)
        emit_stmt(os, st.get(), indent + 2);
      os << ind << "}\n";
    }
    return;
  }

  if (auto ifs = dynamic_cast<IfStmt *>(s)) {
    os << ind << "if (";
    std::ostringstream cond;
    emit_expr(cond, ifs->cond.get());
    os << cond.str() << ") ";
    if (dynamic_cast<BlockStmt *>(ifs->then_branch.get())) {
      emit_stmt(os, ifs->then_branch.get(), 0);
    } else {
      os << "\n";
      emit_stmt(os, ifs->then_branch.get(), indent + 2);
    }
    if (ifs->else_branch) {
      os << ind << "else ";
      if (dynamic_cast<BlockStmt *>((*ifs->else_branch).get())) {
        emit_stmt(os, (*ifs->else_branch).get(), 0);
      } else {
        os << "\n";
        emit_stmt(os, (*ifs->else_branch).get(), indent + 2);
      }
    }
    return;
  }
  if (auto sw = dynamic_cast<SwitchStmt *>(s)) {
    auto ind = std::string(indent, ' ');
    os << ind << "switch (";
    std::ostringstream switch_expr;
    emit_expr(switch_expr, sw->expr.get());
    os << switch_expr.str() << ") {\n";

    for (const auto &c : sw->cases) {
      if (c.value) {

        os << ind << "  case ";
        std::ostringstream case_val;
        emit_expr(case_val, c.value.value().get());
        os << case_val.str() << ":\n";
      } else {

        os << ind << "  default:\n";
      }

      for (const auto &st : c.stmts) {
        if (st) {
          emit_stmt(os, st.get(), indent + 4);
        }
      }
    }

    os << ind << "}\n";
    return;
  }
  if (auto wh = dynamic_cast<WhileStmt *>(s)) {
    os << ind << "while (";
    std::ostringstream cond;
    emit_expr(cond, wh->cond.get());
    os << cond.str() << ") ";
    if (dynamic_cast<BlockStmt *>(wh->body.get())) {
      emit_stmt(os, wh->body.get(), 0);
    } else {
      os << "\n";
      emit_stmt(os, wh->body.get(), indent + 2);
    }
    return;
  }

  if (auto fr = dynamic_cast<ForStmt *>(s)) {
    os << ind << "for (";
    if (fr->init) {
      if (auto vd = dynamic_cast<VarDeclStmt *>(fr->init->get())) {
        std::string tt = ctype_from_tn(vd->type.get());
        std::string for_suffix = "";
        if (vd->type && vd->type->name == "Array" &&
            vd->type->params.size() == 2) {
          auto [base, suffix] = flatten_static_array(vd->type.get());
          tt = base;
          for_suffix = suffix;
        } else {
          auto [base, suffix] = split_c_array_suffix(tt);
          tt = base;
          for_suffix = suffix;
        }
        os << tt << " " << vd->name << for_suffix;
        if (vd->init) {
          os << " = ";
          std::ostringstream tmp;
          emit_expr(tmp, vd->init.get());
          os << tmp.str();
        }
      } else if (auto es = dynamic_cast<ExprStmt *>(fr->init->get())) {
        std::ostringstream tmp;
        emit_expr(tmp, es->expr.get());
        os << tmp.str();
      } else {
        std::ostringstream tmp;
        emit_stmt(tmp, fr->init->get(), 0);
        std::string init = tmp.str();
        while (!init.empty() && (init.back() == '\n' || init.back() == ';' ||
                                 init.back() == ' '))
          init.pop_back();
        os << init;
      }
    }
    os << "; ";
    if (fr->cond) {
      std::ostringstream tmp;
      emit_expr(tmp, fr->cond->get());
      os << tmp.str();
    }
    os << "; ";
    if (fr->post) {
      if (auto es = dynamic_cast<ExprStmt *>(fr->post->get())) {
        std::ostringstream tmp;
        emit_expr(tmp, es->expr.get());
        os << tmp.str();
      } else {
        std::ostringstream tmp;
        emit_stmt(tmp, fr->post->get(), 0);
        std::string p = tmp.str();
        if (!p.empty() && p.back() == '\n')
          p.pop_back();
        os << p;
      }
    }
    os << ") ";
    if (dynamic_cast<BlockStmt *>(fr->body.get())) {
      emit_stmt(os, fr->body.get(), 0);
    } else {
      os << "\n";
      emit_stmt(os, fr->body.get(), indent + 2);
    }
    return;
  }

  os << ind << "/* stmt-not-implemented */\n";
}

void CodeGen::emit_expr(std::ostream &os, Expr *e) {
  if (!e)
    return;

  if (auto n = dynamic_cast<NullExpr *>(e)) {
    os << "NULL";
    return;
  }

  if (auto id = dynamic_cast<IdentifierExpr *>(e)) {
    auto it = capture_map.find(id->name);
    if (it != capture_map.end()) {
      os << it->second;
      return;
    }
    auto pit = current_param_types.find(id->name);
    TypeNode *tn = nullptr;
    if (pit != current_param_types.end())
      tn = pit->second;
    else {
      auto lit = current_local_var_types.find(id->name);
      if (lit != current_local_var_types.end())
        tn = lit->second;
    }

    if (tn == nullptr &&
        top_level_funcs.find(id->name) != top_level_funcs.end()) {
      os << "&" << mangle(id->name) << "_closure";
      return;
    }

    if (tn == nullptr) {
      for (auto &dptr : tu->decls) {
        if (auto opt = dynamic_cast<OptDecl *>(dptr.get())) {
          for (auto &field : opt->fields) {
            if (field.name == id->name) {

              os << mangle(opt->name) << "_" << field.name;
              return;
            }
          }
        }
      }
    }

    os << id->name;
    return;
  }
  if (auto num = dynamic_cast<NumberExpr *>(e)) {
    if (num->literal == "INF") {
      os << "INFINITY";
    } else if (num->literal == "NAN") {
      os << "NAN";
    } else {
      os << num->literal;
    }
    return;
  }
  if (auto s = dynamic_cast<StringExpr *>(e)) {
    os << s->literal;
    return;
  }
  if (auto c = dynamic_cast<CharExpr *>(e)) {
    os << c->literal;
    return;
  }
  if (auto b = dynamic_cast<BoolExpr *>(e)) {
    os << (b->value ? "true" : "false");
    return;
  }

  if (auto mem = dynamic_cast<MemberExpr *>(e)) {

    if (auto obj_id = dynamic_cast<IdentifierExpr *>(mem->object.get())) {

      if (auto *od = find_opts_decl(tu, obj_id->name)) {
        if (od->is_extern) {
          for (auto &f : od->fields) {
            if (f.name == mem->field) {
              os << (f.value ? *f.value : 0);
              return;
            }
          }
        }
        os << mangle(obj_id->name) << "_" << mem->field;
        return;
      }
    }

    std::ostringstream obj_os;
    emit_expr(obj_os, mem->object.get());
    std::string obj_str = obj_os.str();

    std::string obj_type;
    if (expr_type_map) {
      auto it = expr_type_map->find(mem->object.get());
      if (it != expr_type_map->end())
        obj_type = it->second.name;
    }

    if (obj_type.empty()) {
      obj_type = infer_type_name_for_expr(mem->object.get(), expr_type_map,
                                          current_param_types,
                                          current_local_var_types);
    }

    if (is_this_ref_object(mem->object.get()))
      obj_type = "ref<" + current_this_ref_block_type + ">";

    bool is_deref = false;
    if (auto un = dynamic_cast<UnaryExpr *>(mem->object.get())) {
      if (un->op == "*") {
        is_deref = true;
      }
    }

    bool is_ptr = false;
    std::string inner;
    if (!obj_type.empty()) {
      std::string cleaned = obj_type;
      if (cleaned.rfind("const<", 0) == 0) {
        size_t p = cleaned.find('<'), q = cleaned.rfind('>');
        if (p != std::string::npos && q != std::string::npos && q > p + 1)
          cleaned = cleaned.substr(p + 1, q - p - 1);
      }

      if (cleaned.rfind("Array<", 0) == 0) {
        is_ptr = true;

        inner = "risk_array_t";
      } else if (cleaned.rfind("ref<", 0) == 0 ||
                 cleaned.rfind("ptr<", 0) == 0) {
        is_ptr = true;
        size_t p = cleaned.find('<'), q = cleaned.rfind('>');
        if (p != std::string::npos && q != std::string::npos && q > p + 1)
          inner = cleaned.substr(p + 1, q - p - 1);
      } else {
        inner = cleaned;
      }
    }

    std::string struct_name;
    if (!inner.empty()) {
      if (auto *bd = find_block_decl(tu, inner))
        struct_name = block_struct_name(bd);
      else
        struct_name = mangle(inner);
    }
    if (is_deref) {

      os << "(" << obj_str << ")." << mem->field;
    } else if (is_ptr) {
      if (!struct_name.empty() && struct_name != "risk_array_t")
        os << "((" << struct_name << "*)(" << obj_str << "))->" << mem->field;
      else
        os << "(" << obj_str << ")->" << mem->field;
    } else {
      os << obj_str << "." << mem->field;
    }
    return;
  }

  if (auto bl = dynamic_cast<BlockLiteralExpr *>(e)) {
    std::string block_typename;
    if (expr_type_map) {
      auto it = expr_type_map->find(bl);
      if (it != expr_type_map->end())
        block_typename = it->second.name;
    }

    if (block_typename.empty()) {
      os << "/* expr-not-implemented */";
      return;
    }

    if (block_typename.rfind("Array<", 0) == 0) {
      os << "{ ";
      for (size_t i = 0; i < bl->positional.size(); ++i) {
        if (i)
          os << ", ";
        emit_expr(os, bl->positional[i].get());
      }
      os << " }";
      return;
    }

    if (block_typename.rfind("Hashmap<", 0) == 0) {

      size_t open = block_typename.find('<');
      size_t close = block_typename.rfind('>');
      if (open != std::string::npos && close != std::string::npos) {
        std::string contents =
            block_typename.substr(open + 1, close - open - 1);
        size_t comma = contents.find(',');
        if (comma != std::string::npos) {
          std::string key_type = contents.substr(0, comma);
          std::string value_type = contents.substr(comma + 1);

          key_type.erase(0, key_type.find_first_not_of(" \t"));
          key_type.erase(key_type.find_last_not_of(" \t") + 1);
          value_type.erase(0, value_type.find_first_not_of(" \t"));
          value_type.erase(value_type.find_last_not_of(" \t") + 1);

          std::string c_key_type = ctype_from_type_name(key_type);
          std::string c_value_type = ctype_from_type_name(value_type);
          int is_string_key = (key_type == "string") ? 1 : 0;

          os << "risk_hashmap_create(sizeof(" << c_key_type << "), sizeof("
             << c_value_type << "), " << is_string_key << ")";
          return;
        }
      }
      os << "/* invalid-hashmap-type */";
      return;
    }

    BlockDecl *b = find_block_decl(tu, block_typename);
    if (!b) {
      os << "/* expr-not-implemented */";
      return;
    }
    std::string struct_name = block_struct_name(b);
    os << "((" << struct_name << "){ ";

    if (!bl->positional.empty()) {
      for (size_t i = 0; i < bl->positional.size() && i < b->fields.size();
           ++i) {
        if (i)
          os << ", ";

        bool is_func_field = false;
        std::string field_type_name;
        if (i < b->fields.size() && b->fields[i].type) {
          field_type_name = build_type_name_from_node(b->fields[i].type.get());
          is_func_field = (b->fields[i].type->name == "func" ||
                           field_type_name.rfind("func(", 0) == 0);
        }

        if (is_func_field) {
          if (auto id =
                  dynamic_cast<IdentifierExpr *>(bl->positional[i].get())) {
            bool is_top_level =
                (top_level_funcs.find(id->name) != top_level_funcs.end());
            if (is_top_level) {

              os << "&" << mangle(id->name) << "_closure";
            } else {

              std::ostringstream v;
              emit_expr(v, bl->positional[i].get());
              os << v.str();
            }
          } else {

            std::ostringstream v;
            emit_expr(v, bl->positional[i].get());
            os << v.str();
          }
        } else {

          std::ostringstream v;
          emit_expr(v, bl->positional[i].get());
          os << v.str();
        }
      }

      if (!bl->named.empty())
        os << ", ";
    }

    for (size_t i = 0; i < bl->named.size(); ++i) {
      if (i)
        os << ", ";

      bool is_func_field = false;
      for (const auto &field : b->fields) {
        if (field.name == bl->named[i].first) {
          std::string field_type_name =
              build_type_name_from_node(field.type.get());
          is_func_field = (field.type->name == "func" ||
                           field_type_name.rfind("func(", 0) == 0);
          break;
        }
      }

      os << "." << bl->named[i].first << " = ";

      if (is_func_field) {
        if (auto id =
                dynamic_cast<IdentifierExpr *>(bl->named[i].second.get())) {
          if (top_level_funcs.find(id->name) != top_level_funcs.end()) {

            os << "&" << mangle(id->name) << "_closure";
          } else {

            std::ostringstream v;
            emit_expr(v, bl->named[i].second.get());
            os << v.str();
          }
        } else {

          std::ostringstream v;
          emit_expr(v, bl->named[i].second.get());
          os << v.str();
        }
      } else {

        std::ostringstream v;
        emit_expr(v, bl->named[i].second.get());
        os << v.str();
      }
    }

    if (!bl->named.empty()) {
      for (const auto &field : b->fields) {
        bool provided = false;
        for (const auto &n : bl->named) {
          if (n.first == field.name) {
            provided = true;
            break;
          }
        }
        if (!provided && field.default_val) {
          os << ", ." << field.name << " = ";

          bool default_is_func_field = false;
          if (field.type) {
            std::string field_type_name =
                build_type_name_from_node(field.type.get());
            default_is_func_field = (field.type->name == "func" ||
                                     field_type_name.rfind("func(", 0) == 0);
          }

          std::ostringstream v;
          if (default_is_func_field) {
            if (auto did =
                    dynamic_cast<IdentifierExpr *>(field.default_val.get())) {
              if (top_level_funcs.find(did->name) != top_level_funcs.end()) {
                v << "&" << mangle(did->name) << "_closure";
              } else {
                emit_expr(v, field.default_val.get());
              }
            } else {
              emit_expr(v, field.default_val.get());
            }
          } else {
            emit_expr(v, field.default_val.get());
          }
          os << v.str();
        }
      }
    }
    os << " })";
    return;
  }

  if (auto cast = dynamic_cast<CastExpr *>(e)) {
    std::string ctype = "void*";
    if (cast->target)
      ctype = ctype_from_tn(cast->target.get());

    std::string src_risk =
        infer_type_name_for_expr(cast->expr.get(), expr_type_map,
                                 current_param_types, current_local_var_types);

    if (cast->target) {
      static const std::unordered_set<std::string> numeric_risk_types = {
          "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64"};
      if (numeric_risk_types.count(src_risk)) {
        if (OptDecl *od = find_opts_decl(tu, cast->target->name)) {
          long long min_val, max_val;
          opts_value_range(od, min_val, max_val);
          std::string tmp = fresh_id("__opt_clamp");
          os << "({ long long " << tmp << " = (long long)(";
          emit_expr(os, cast->expr.get());
          os << "); (" << ctype << ")(" << tmp << " < " << min_val << "LL ? "
             << min_val << "LL : (" << tmp << " > " << max_val << "LL ? "
             << max_val << "LL : " << tmp << ")); })";
          return;
        }
      }
    }

    if (ctype == "char" && src_risk == "string") {
      os << "((";
      emit_expr(os, cast->expr.get());
      os << ")[0])";
      return;
    }

    if ((ctype == "const char*" || ctype == "char*") && src_risk == "char") {
      os << "char_to_str(";
      emit_expr(os, cast->expr.get());
      os << ")";
      return;
    }

    if (cast->is_unsafe) {

      os << "((" << ctype << ")(";
      emit_expr(os, cast->expr.get());
      os << "))";
    } else {

      os << "((" << ctype << ")(";
      emit_expr(os, cast->expr.get());
      os << "))";
    }
    return;
  }
  if (auto un = dynamic_cast<UnaryExpr *>(e)) {

    if (un->op == "++" || un->op == "--") {
      std::ostringstream lv;
      emit_lvalue(lv, un->rhs.get());
      if (un->is_postfix) {
        os << "(";
        emit_lvalue(os, un->rhs.get());
        os << un->op;
        os << ")";
      } else {

        os << un->op << "(" << lv.str() << ")";
      }
      return;
    }

    if (un->op == "&") {
      if (auto id = dynamic_cast<IdentifierExpr *>(un->rhs.get())) {
        os << "&" << id->name;
        return;
      } else {
        os << "&(";
        emit_expr(os, un->rhs.get());
        os << ")";
        return;
      }
    }
    if (un->op == "*") {
      os << "*(";
      emit_expr(os, un->rhs.get());
      os << ")";
      return;
    }
    os << un->op;
    emit_expr(os, un->rhs.get());
    return;
  }
  if (auto bin = dynamic_cast<BinaryExpr *>(e)) {
    if (bin->op == "=") {

      if (auto mem = dynamic_cast<MemberExpr *>(bin->lhs.get())) {

        std::ostringstream obj_os;
        emit_expr(obj_os, mem->object.get());
        std::string obj_str = obj_os.str();

        std::string obj_type;
        if (expr_type_map) {
          auto it = expr_type_map->find(mem->object.get());
          if (it != expr_type_map->end())
            obj_type = it->second.name;
        }
        if (obj_type.empty()) {
          obj_type = infer_type_name_for_expr(mem->object.get(), expr_type_map,
                                              current_param_types,
                                              current_local_var_types);
        }

        if (is_this_ref_object(mem->object.get()))
          obj_type = "ref<" + current_this_ref_block_type + ">";
        bool is_ptr = false;
        std::string inner;
        if (!obj_type.empty()) {
          std::string cleaned = obj_type;
          if (cleaned.rfind("const<", 0) == 0) {
            size_t p = cleaned.find('<'), q = cleaned.rfind('>');
            if (p != std::string::npos && q != std::string::npos && q > p + 1)
              cleaned = cleaned.substr(p + 1, q - p - 1);
          }
          if (cleaned.rfind("ref<", 0) == 0 || cleaned.rfind("ptr<", 0) == 0) {
            is_ptr = true;
            size_t p = cleaned.find('<'), q = cleaned.rfind('>');
            if (p != std::string::npos && q != std::string::npos && q > p + 1)
              inner = cleaned.substr(p + 1, q - p - 1);
          } else {
            inner = cleaned;
          }
        }
        std::string struct_name;
        if (!inner.empty()) {
          if (auto *bd = find_block_decl(tu, inner))
            struct_name = block_struct_name(bd);
          else
            struct_name = mangle(inner);
        }

        std::ostringstream rhs_os;
        emit_expr(rhs_os, bin->rhs.get());

        bool is_deref = false;
        if (auto un = dynamic_cast<UnaryExpr *>(mem->object.get())) {
          if (un->op == "*") {
            is_deref = true;
          }
        }

        if (is_deref) {

          os << "(" << obj_str << ")." << mem->field << " = " << rhs_os.str();
        } else if (is_ptr) {
          if (!struct_name.empty())
            os << "((" << struct_name << "*)(" << obj_str << "))->"
               << mem->field << " = " << rhs_os.str();
          else
            os << "(" << obj_str << ")->" << mem->field << " = "
               << rhs_os.str();
        } else {
          os << obj_str << "." << mem->field << " = " << rhs_os.str();
        }
        return;
      }

      if (auto idx = dynamic_cast<IndexExpr *>(bin->lhs.get())) {
        std::string obj_type;
        if (expr_type_map) {
          auto it = expr_type_map->find(idx->object.get());
          if (it != expr_type_map->end())
            obj_type = it->second.name;
        }
        if (obj_type.empty()) {
          obj_type = infer_type_name_for_expr(idx->object.get(), expr_type_map,
                                              current_param_types,
                                              current_local_var_types);
        }

        if (obj_type.rfind("Hashmap<", 0) == 0) {

          size_t open = obj_type.find('<');
          size_t close = obj_type.rfind('>');
          std::string key_type, value_type;
          if (open != std::string::npos && close != std::string::npos) {
            std::string contents = obj_type.substr(open + 1, close - open - 1);
            size_t comma = contents.find(',');
            if (comma != std::string::npos) {
              key_type = contents.substr(0, comma);
              value_type = contents.substr(comma + 1);

              while (!key_type.empty() && key_type.back() == ' ')
                key_type.pop_back();
              while (!value_type.empty() && value_type[0] == ' ')
                value_type = value_type.substr(1);
              while (!value_type.empty() && value_type.back() == ' ')
                value_type.pop_back();
            }
          }
          std::string c_key_type = ctype_from_type_name(key_type);
          std::string c_value_type = ctype_from_type_name(value_type);

          std::string key_tmp = fresh_id("__hm_key");
          std::string val_tmp = fresh_id("__hm_val");

          os << "({\n";
          os << "        " << c_key_type << " " << key_tmp << " = ";
          emit_expr(os, idx->index.get());
          os << ";\n";
          os << "        " << c_value_type << " " << val_tmp << " = ";
          emit_expr(os, bin->rhs.get());
          os << ";\n";
          os << "        risk_hashmap_set(";
          emit_expr(os, idx->object.get());
          os << ", &" << key_tmp << ", &" << val_tmp << ");\n";
          os << "      })";
          return;
        }

        if (obj_type.rfind("Array<", 0) == 0) {
          size_t open = obj_type.find('<');
          size_t close = obj_type.rfind('>');
          bool is_static = false;
          std::string elem_type;
          if (open != std::string::npos && close != std::string::npos) {
            std::string contents = obj_type.substr(open + 1, close - open - 1);

            int depth = 0;
            int top_commas = 0;
            size_t last_top_comma = std::string::npos;
            for (size_t ci = 0; ci < contents.size(); ++ci) {
              if (contents[ci] == '<')
                ++depth;
              else if (contents[ci] == '>')
                --depth;
              else if (contents[ci] == ',' && depth == 0) {
                ++top_commas;
                last_top_comma = ci;
              }
            }
            if (top_commas == 0) {

              elem_type = contents;
            } else {

              is_static = true;
              elem_type = contents.substr(0, last_top_comma);
            }
            while (!elem_type.empty() && elem_type.front() == ' ')
              elem_type = elem_type.substr(1);
            while (!elem_type.empty() && elem_type.back() == ' ')
              elem_type.pop_back();
          }
          if (is_static) {

            os << "(";
            emit_expr(os, idx->object.get());
            os << ")[";
            emit_expr(os, idx->index.get());
            os << "] = ";
            emit_expr(os, bin->rhs.get());
            return;
          }
          std::string c_elem_type = ctype_from_type_name(elem_type);
          std::string idx_tmp = fresh_id("__arr_idx");
          std::string val_tmp = fresh_id("__arr_val");
          os << "({\n";
          os << "        int32_t " << idx_tmp << " = ";
          emit_expr(os, idx->index.get());
          os << ";\n";
          os << "        " << c_elem_type << " " << val_tmp << " = ";
          emit_expr(os, bin->rhs.get());
          os << ";\n";
          os << "        risk_array_set(";
          emit_expr(os, idx->object.get());
          os << ", " << idx_tmp << ", &" << val_tmp << ");\n";
          os << "      })";
          return;
        }
      }
      if (auto id = dynamic_cast<IdentifierExpr *>(bin->lhs.get())) {
        auto lit = current_local_var_types.find(id->name);
        TypeNode *tn = nullptr;
        if (lit != current_local_var_types.end())
          tn = lit->second;
        else {
          auto pit = current_param_types.find(id->name);
          if (pit != current_param_types.end())
            tn = pit->second;
        }
        std::ostringstream rhs_os;
        emit_expr(rhs_os, bin->rhs.get());
        std::string rhs_str = rhs_os.str();
        if (tn && tn->name == "ref") {
          os << "*(" << id->name << ") = " << rhs_str;
          return;
        } else {
          os << id->name << " = " << rhs_str;
          return;
        }
      } else if (auto un = dynamic_cast<UnaryExpr *>(bin->lhs.get())) {
        if (un->op == "*") {
          std::ostringstream ptr_os;
          emit_expr(ptr_os, un->rhs.get());
          std::ostringstream rhs_os;
          emit_expr(rhs_os, bin->rhs.get());
          os << "*(" << ptr_os.str() << ") = " << rhs_os.str();
          return;
        }
      }
      os << "(";
      emit_expr(os, bin->lhs.get());
      os << " = ";
      emit_expr(os, bin->rhs.get());
      os << ")";
      return;
    }
    if (bin->op == "==" || bin->op == "!=") {

      auto is_string_operand = [&](Expr *operand) {
        std::string t;
        if (expr_type_map) {
          auto it = expr_type_map->find(operand);
          if (it != expr_type_map->end())
            t = it->second.name;
        }
        if (t.empty())
          t = infer_type_name_for_expr(operand, expr_type_map,
                                       current_param_types,
                                       current_local_var_types);
        return t == "string";
      };
      if (is_string_operand(bin->lhs.get()) ||
          is_string_operand(bin->rhs.get())) {
        if (bin->op == "!=")
          os << "(!";
        os << "risk_streq(";
        emit_expr(os, bin->lhs.get());
        os << ", ";
        emit_expr(os, bin->rhs.get());
        os << ")";
        if (bin->op == "!=")
          os << ")";
        return;
      }
    }
    os << "(";
    emit_expr(os, bin->lhs.get());
    os << " " << bin->op << " ";
    emit_expr(os, bin->rhs.get());
    os << ")";
    return;
  }
  if (auto call = dynamic_cast<CallExpr *>(e)) {

    if (auto id = dynamic_cast<IdentifierExpr *>(call->callee.get())) {
      if (id->name == "toInt" && call->args.size() == 1) {
        os << "(int32_t)risk_to_int(";
        emit_expr(os, call->args[0].get());
        os << ")";
        return;
      }

      if (id->name == "toFloat" && call->args.size() == 1) {
        os << "risk_to_float(";
        emit_expr(os, call->args[0].get());
        os << ")";
        return;
      }

      if (id->name == "toBool" && call->args.size() == 1) {
        os << "(bool)risk_to_bool(";
        emit_expr(os, call->args[0].get());
        os << ")";
        return;
      }

      if (id->name == "length") {
        if (call->args.size() != 1) {
          os << "0 /* length() expects 1 argument */";
          return;
        }

        std::string arg_type = infer_type_name_for_expr(
            call->args[0].get(), expr_type_map, current_param_types,
            current_local_var_types);

        if (arg_type.rfind("Array<", 0) == 0) {
          size_t open = arg_type.find('<');
          size_t close = arg_type.rfind('>');
          if (open != std::string::npos && close != std::string::npos) {
            std::string contents = arg_type.substr(open + 1, close - open - 1);
            size_t comma = contents.find(',');
            if (comma != std::string::npos) {

              std::string size = contents.substr(comma + 1);
              size.erase(0, size.find_first_not_of(" \t"));
              size.erase(size.find_last_not_of(" \t") + 1);

              if (size.rfind("__size__:", 0) == 0) {
                size = size.substr(9);
              }

              os << size;
              return;
            }
          }
        }

        os << "risk_array_length(";
        emit_expr(os, call->args[0].get());
        os << ")";
        return;
      }

      if (id->name == "toString" && call->args.size() == 1) {
        std::string arg_type = infer_type_name_for_expr(
            call->args[0].get(), expr_type_map, current_param_types,
            current_local_var_types);

        if (arg_type == "i32" || arg_type == "i64" || arg_type == "i16" ||
            arg_type == "i8") {
          os << "risk_to_str_int(";
          emit_expr(os, call->args[0].get());
          os << ")";
        } else if (arg_type == "f64" || arg_type == "f32") {
          os << "risk_to_str_float(";
          emit_expr(os, call->args[0].get());
          os << ")";
        } else if (arg_type == "bool") {
          os << "risk_to_str_bool(";
          emit_expr(os, call->args[0].get());
          os << ")";
        } else if (arg_type == "string") {
          emit_expr(os, call->args[0].get());
        } else if (arg_type == "char") {
          os << "char_to_str(";
          emit_expr(os, call->args[0].get());
          os << ")";
        } else if (arg_type == "u8" || arg_type == "u16" || arg_type == "u32" ||
                   arg_type == "u64") {
          os << "risk_to_str_int((int64_t)(";
          emit_expr(os, call->args[0].get());
          os << "))";
        } else {

          os << "risk_to_str_int((int32_t)(";
          emit_expr(os, call->args[0].get());
          os << "))";
        }
        return;
      }

      if (id->name == "intToChar" && call->args.size() == 1) {
        os << "({ int32_t __chr_cp = (int32_t)(";
        emit_expr(os, call->args[0].get());
        os << "); char* __chr_s = (char*)malloc(5); int __chr_len = 0;"
           << " if (__chr_cp < 0x80) { __chr_s[0] = (char)__chr_cp; __chr_len "
              "= 1; }"
           << " else if (__chr_cp < 0x800) { __chr_s[0] = (char)(0xC0 | "
              "(__chr_cp >> 6)); __chr_s[1] = (char)(0x80 | (__chr_cp & "
              "0x3F)); __chr_len = 2; }"
           << " else if (__chr_cp < 0x10000) { __chr_s[0] = (char)(0xE0 | "
              "(__chr_cp >> 12)); __chr_s[1] = (char)(0x80 | ((__chr_cp >> 6) "
              "& 0x3F)); __chr_s[2] = (char)(0x80 | (__chr_cp & 0x3F)); "
              "__chr_len = 3; }"
           << " else { __chr_s[0] = (char)(0xF0 | (__chr_cp >> 18)); "
              "__chr_s[1] = (char)(0x80 | ((__chr_cp >> 12) & 0x3F)); "
              "__chr_s[2] = (char)(0x80 | ((__chr_cp >> 6) & 0x3F)); "
              "__chr_s[3] = (char)(0x80 | (__chr_cp & 0x3F)); __chr_len = 4; }"
           << " __chr_s[__chr_len] = '\\0'; (const char*)__chr_s; })";
        return;
      }

      if (id->name == "sizeof") {
        if (call->args.size() != 1) {
          os << "0 /* sizeof() expects 1 argument */";
          return;
        }

        std::string arg_type = infer_type_name_for_expr(
            call->args[0].get(), expr_type_map, current_param_types,
            current_local_var_types);

        if (!arg_type.empty()) {
          std::string c_type = ctype_from_type_name(arg_type);
          os << "sizeof(" << c_type << ")";
        } else {
          os << "sizeof(";
          emit_expr(os, call->args[0].get());
          os << ")";
        }
        return;
      }

      if (id->name == "type") {
        if (call->args.size() != 1) {
          os << "\"unknown\" /* type() expects 1 argument */";
          return;
        }

        if (compile_time_map) {
          auto it = compile_time_map->find(call);
          if (it != compile_time_map->end()) {
            os << "\"" << escape_c_literal(it->second) << "\"";
            return;
          }
        }

        std::string arg_type;
        if (expr_type_map) {
          auto it = expr_type_map->find(call->args[0].get());
          if (it != expr_type_map->end()) {
            arg_type = it->second.name;
          }
        }

        if (!arg_type.empty()) {
          os << "\"" << escape_c_literal(arg_type) << "\"";
        } else {
          os << "\"unknown\"";
        }
        return;
      }

      if (id->name == "dealloc" && call->args.size() == 1) {
        os << "risk_free(";
        emit_expr(os, call->args[0].get());
        os << ")";
        return;
      }

      if (id->name == "rcNew" && call->args.size() == 1) {

        std::string tmp = fresh_id("__rc_init");
        os << "({";
        os << "__typeof__(";
        emit_expr(os, call->args[0].get());
        os << ") " << tmp << " = ";
        emit_expr(os, call->args[0].get());
        os << "; risk_rc_new(&" << tmp << ", sizeof(" << tmp << "), NULL); })";
        return;
      }
      if (id->name == "rcRetain" && call->args.size() == 1) {
        os << "risk_rc_retain(";
        emit_expr(os, call->args[0].get());
        os << ")";
        return;
      }
      if (id->name == "rcRelease" && call->args.size() == 1) {
        os << "risk_rc_release(";
        emit_expr(os, call->args[0].get());
        os << ")";
        return;
      }
      if (id->name == "rcGet" && call->args.size() == 1) {
        std::string cast_t;
        if (expr_type_map) {
          auto it = expr_type_map->find(call);
          if (it != expr_type_map->end() &&
              it->second.name.rfind("ptr<", 0) == 0) {
            std::string inner = it->second.name.substr(
                4, it->second.name.size() - 5);
            if (!inner.empty() && inner != "void")
              cast_t = ctype_from_type_name(inner) + "*";
          }
        }
        os << "(";
        if (!cast_t.empty())
          os << "(" << cast_t << ")";
        os << "risk_rc_get(";
        emit_expr(os, call->args[0].get());
        os << "))";
        return;
      }
      if (id->name == "rcCount" && call->args.size() == 1) {
        os << "((size_t)risk_rc_count(";
        emit_expr(os, call->args[0].get());
        os << "))";
        return;
      }

      if (id->name == "arenaNew" && call->args.size() == 1) {
        os << "risk_arena_create((size_t)(";
        emit_expr(os, call->args[0].get());
        os << "))";
        return;
      }

      if (id->name == "ioInput" && call->args.size() == 0) {
        os << "risk_io_input()";
        return;
      }

      if (id->name == "strRepeat" && call->args.size() == 2) {
        os << "str_repeat(";
        emit_expr(os, call->args[0].get());
        os << ", ";
        emit_expr(os, call->args[1].get());
        os << ")";
        return;
      }

      if ((id->name == "PI" || id->name == "E") && call->args.size() == 0) {
        os << "mod__" << id->name << "()";
        return;
      }

      if (id->name == "ioRawPrintln") {
        if (call->args.empty()) {
          os << "core_println(\"\")";
          return;
        }

        std::string print_id = "__rprint_" + std::to_string(unique_id++);
        os << "({\n";
        os << "        char " << print_id << "[2048];\n";
        os << "        " << print_id << "[0] = '\\0';\n";

        for (size_t ai = 0; ai < call->args.size(); ++ai) {
          Expr *arg = call->args[ai].get();

          if (compile_time_map) {
            auto it = compile_time_map->find(arg);
            if (it != compile_time_map->end()) {
              os << "        strncat(" << print_id << ", \""
                 << escape_c_literal(it->second) << "\", sizeof(" << print_id
                 << ") - strlen(" << print_id << ") - 1);\n";
              continue;
            }
          }

          if (dynamic_cast<StringExpr *>(arg)) {
            std::ostringstream a_os;
            emit_expr(a_os, arg);
            os << "        strncat(" << print_id << ", " << a_os.str()
               << ", sizeof(" << print_id << ") - strlen(" << print_id
               << ") - 1);\n";
            continue;
          }

          std::string arg_type;
          if (expr_type_map) {
            auto it = expr_type_map->find(arg);
            if (it != expr_type_map->end()) {
              arg_type = it->second.name;
            }
          }

          std::string fmt_id = "__rfmt_" + std::to_string(unique_id++);
          if (arg_type == "i32" || arg_type == "i64" || arg_type == "i8" ||
              arg_type == "i16") {
            os << "        { char " << fmt_id << "[64]; snprintf(" << fmt_id
               << ", sizeof(" << fmt_id << "), \"%d\", (int)(";
            emit_expr(os, arg);
            os << ")); strncat(" << print_id << ", " << fmt_id << ", sizeof("
               << print_id << ") - strlen(" << print_id << ") - 1); }\n";
          } else if (arg_type == "f64" || arg_type == "f32") {
            os << "        { char " << fmt_id << "[64]; snprintf(" << fmt_id
               << ", sizeof(" << fmt_id << "), risk_float_fmt, (double)(";
            emit_expr(os, arg);
            os << ")); strncat(" << print_id << ", " << fmt_id << ", sizeof("
               << print_id << ") - strlen(" << print_id << ") - 1); }\n";
          } else if (arg_type == "char") {
            os << "        { char " << fmt_id << "[2]; " << fmt_id
               << "[0] = (char)(";
            emit_expr(os, arg);
            os << "); " << fmt_id << "[1] = '\\0'; strncat(" << print_id << ", "
               << fmt_id << ", sizeof(" << print_id << ") - strlen(" << print_id
               << ") - 1); }\n";
          } else if (arg_type == "bool") {
            os << "        { const char* " << fmt_id << " = (";
            emit_expr(os, arg);
            os << ") ? \"true\" : \"false\"; strncat(" << print_id << ", "
               << fmt_id << ", sizeof(" << print_id << ") - strlen(" << print_id
               << ") - 1); }\n";
          } else if (arg_type == "string" || arg_type.rfind("ptr<", 0) == 0) {
            os << "        { const char* " << fmt_id << " = (const char*)(";
            emit_expr(os, arg);
            os << "); if (" << fmt_id << ") strncat(" << print_id << ", "
               << fmt_id << ", sizeof(" << print_id << ") - strlen(" << print_id
               << ") - 1); }\n";
          } else {

            os << "        { char " << fmt_id << "[64]; snprintf(" << fmt_id
               << ", sizeof(" << fmt_id << "), \"%d\", (int)(";
            emit_expr(os, arg);
            os << ")); strncat(" << print_id << ", " << fmt_id << ", sizeof("
               << print_id << ") - strlen(" << print_id << ") - 1); }\n";
          }
        }

        os << "        core_println(" << print_id << ");\n";
        os << "      })";
        return;
      }
      if (id->name == "clrScr") {
        os << "#ifdef _WIN32\n";
        os << "system(\"cls\");\n";
        os << "#else\n";
        os << "system(\"clear\");\n";
        os << "#endif\n";
        os << "(void)0";
        return;
      }

      if (id->name == "ioRawPrint") {
        if (call->args.empty()) {
          os << "core_print(\"\")";
          return;
        }

        std::string print_id = "__rprint_" + std::to_string(unique_id++);
        os << "({\n";
        os << "        char " << print_id << "[2048];\n";
        os << "        " << print_id << "[0] = '\\0';\n";

        for (size_t ai = 0; ai < call->args.size(); ++ai) {
          Expr *arg = call->args[ai].get();

          if (compile_time_map) {
            auto it = compile_time_map->find(arg);
            if (it != compile_time_map->end()) {
              os << "        strncat(" << print_id << ", \""
                 << escape_c_literal(it->second) << "\", sizeof(" << print_id
                 << ") - strlen(" << print_id << ") - 1);\n";
              continue;
            }
          }

          if (dynamic_cast<StringExpr *>(arg)) {
            std::ostringstream a_os;
            emit_expr(a_os, arg);
            os << "        strncat(" << print_id << ", " << a_os.str()
               << ", sizeof(" << print_id << ") - strlen(" << print_id
               << ") - 1);\n";
            continue;
          }

          std::string arg_type;
          if (expr_type_map) {
            auto it = expr_type_map->find(arg);
            if (it != expr_type_map->end()) {
              arg_type = it->second.name;
            }
          }

          std::string fmt_id = "__rfmt_" + std::to_string(unique_id++);
          if (arg_type == "i32" || arg_type == "i64" || arg_type == "i8" ||
              arg_type == "i16") {
            os << "        { char " << fmt_id << "[64]; snprintf(" << fmt_id
               << ", sizeof(" << fmt_id << "), \"%d\", (int)(";
            emit_expr(os, arg);
            os << ")); strncat(" << print_id << ", " << fmt_id << ", sizeof("
               << print_id << ") - strlen(" << print_id << ") - 1); }\n";
          } else if (arg_type == "f64" || arg_type == "f32") {
            os << "        { char " << fmt_id << "[64]; snprintf(" << fmt_id
               << ", sizeof(" << fmt_id << "), risk_float_fmt, (double)(";
            emit_expr(os, arg);
            os << ")); strncat(" << print_id << ", " << fmt_id << ", sizeof("
               << print_id << ") - strlen(" << print_id << ") - 1); }\n";
          } else if (arg_type == "char") {
            os << "        { char " << fmt_id << "[2]; " << fmt_id
               << "[0] = (char)(";
            emit_expr(os, arg);
            os << "); " << fmt_id << "[1] = '\\0'; strncat(" << print_id << ", "
               << fmt_id << ", sizeof(" << print_id << ") - strlen(" << print_id
               << ") - 1); }\n";
          } else if (arg_type == "bool") {
            os << "        { const char* " << fmt_id << " = (";
            emit_expr(os, arg);
            os << ") ? \"true\" : \"false\"; strncat(" << print_id << ", "
               << fmt_id << ", sizeof(" << print_id << ") - strlen(" << print_id
               << ") - 1); }\n";
          } else if (arg_type == "string" || arg_type.rfind("ptr<", 0) == 0) {
            os << "        { const char* " << fmt_id << " = (const char*)(";
            emit_expr(os, arg);
            os << "); if (" << fmt_id << ") strncat(" << print_id << ", "
               << fmt_id << ", sizeof(" << print_id << ") - strlen(" << print_id
               << ") - 1); }\n";
          } else {

            os << "        { char " << fmt_id << "[64]; snprintf(" << fmt_id
               << ", sizeof(" << fmt_id << "), \"%d\", (int)(";
            emit_expr(os, arg);
            os << ")); strncat(" << print_id << ", " << fmt_id << ", sizeof("
               << print_id << ") - strlen(" << print_id << ") - 1); }\n";
          }
        }

        os << "        core_print(" << print_id << ");\n";
        os << "      })";
        return;
      }

      if (id->name == "ioPrint" || id->name == "ioPrintln") {
        if (call->args.empty()) {
          os << (id->name == "ioPrint" ? "core_print(\"\")"
                                       : "core_println(\"\")");
          return;
        }
        std::string bufname = "__print_buf_" + std::to_string(unique_id++);
        os << "({ char " << bufname << "[1024]; " << bufname << "[0] = '\\0'; ";
        for (size_t ai = 0; ai < call->args.size(); ++ai) {
          Expr *arg = call->args[ai].get();
          if (compile_time_map) {
            auto it = compile_time_map->find(arg);
            if (it != compile_time_map->end()) {
              os << "strncat(" << bufname << ", \""
                 << escape_c_literal(it->second) << "\", sizeof(" << bufname
                 << ") - strlen(" << bufname << ") - 1); ";
              continue;
            }
          }
          if (dynamic_cast<StringExpr *>(arg)) {
            std::ostringstream a_os;
            emit_expr(a_os, arg);
            os << "strncat(" << bufname << ", " << a_os.str() << ", sizeof("
               << bufname << ") - strlen(" << bufname << ") - 1); ";
            continue;
          }

          bool is_deref_expr = false;
          if (auto un = dynamic_cast<UnaryExpr *>(arg)) {
            if (un->op == "*")
              is_deref_expr = true;
          }

          bool print_ptr = expression_is_pointer_address(
              arg, expr_type_map, current_param_types, current_local_var_types);

          std::string atype;
          if (is_deref_expr) {
            auto un = dynamic_cast<UnaryExpr *>(arg);
            atype = infer_type_name_for_expr(un->rhs.get(), expr_type_map,
                                             current_param_types,
                                             current_local_var_types);
            if (atype.rfind("ptr<", 0) == 0 || atype.rfind("ref<", 0) == 0) {
              size_t p = atype.find('<'), q = atype.rfind('>');
              if (p != std::string::npos && q != std::string::npos && q > p + 1)
                atype = atype.substr(p + 1, q - p - 1);
            }
          } else {
            atype = infer_type_name_for_expr(arg, expr_type_map,
                                             current_param_types,
                                             current_local_var_types);

            if (auto call = dynamic_cast<CallExpr *>(arg)) {
              if (auto callee_id =
                      dynamic_cast<IdentifierExpr *>(call->callee.get())) {
                if (callee_id->name == "length") {
                  atype = "i32";
                }
              }
            }
          }

          std::string tmp = "__print_arg_" + std::to_string(unique_id++);

          if (print_ptr) {
            os << "char " << tmp << "[64]; snprintf(" << tmp << ", sizeof("
               << tmp << "), \"%p\", (void*)(";
            emit_expr(os, arg);
            os << ")); strncat(" << bufname << ", " << tmp << ", sizeof("
               << bufname << ") - strlen(" << bufname << ") - 1); ";
            continue;
          }

          if (atype == "string") {
            std::ostringstream a_os;
            emit_expr(a_os, arg);
            os << "strncat(" << bufname << ", " << a_os.str() << ", sizeof("
               << bufname << ") - strlen(" << bufname << ") - 1); ";
          } else if (atype == "char") {
            os << "char " << tmp << "[2]; snprintf(" << tmp << ", sizeof("
               << tmp << "), \"%c\", (unsigned char)(";
            emit_expr_maybe_deref_when_read(
                os, arg, current_param_types, current_local_var_types,
                [&](std::ostream &o, Expr *ex) { emit_expr(o, ex); });
            os << ")); strncat(" << bufname << ", " << tmp << ", sizeof("
               << bufname << ") - strlen(" << bufname << ") - 1); ";
          } else if (atype == "bool") {
            os << "char " << tmp << "[8]; snprintf(" << tmp << ", sizeof("
               << tmp << "), \"%s\", (";
            emit_expr_maybe_deref_when_read(
                os, arg, current_param_types, current_local_var_types,
                [&](std::ostream &o, Expr *ex) { emit_expr(o, ex); });
            os << ") ? \"true\" : \"false\"); strncat(" << bufname << ", "
               << tmp << ", sizeof(" << bufname << ") - strlen(" << bufname
               << ") - 1); ";
          } else if (atype == "i32" || atype == "i16" || atype == "i8" ||
                     atype == "u8" || atype == "u16" || atype == "u32") {
            os << "char " << tmp << "[64]; snprintf(" << tmp << ", sizeof("
               << tmp << "), \"%d\", (int)(";
            emit_expr_maybe_deref_when_read(
                os, arg, current_param_types, current_local_var_types,
                [&](std::ostream &o, Expr *ex) { emit_expr(o, ex); });
            os << ")); strncat(" << bufname << ", " << tmp << ", sizeof("
               << bufname << ") - strlen(" << bufname << ") - 1); ";
          } else if (atype == "i64" || atype == "u64") {
            os << "char " << tmp << "[64]; snprintf(" << tmp << ", sizeof("
               << tmp << "), \"%lld\", (long long)(";
            emit_expr_maybe_deref_when_read(
                os, arg, current_param_types, current_local_var_types,
                [&](std::ostream &o, Expr *ex) { emit_expr(o, ex); });
            os << ")); strncat(" << bufname << ", " << tmp << ", sizeof("
               << bufname << ") - strlen(" << bufname << ") - 1); ";
          } else if (atype == "f64" || atype == "f32") {
            os << "char " << tmp << "[128]; snprintf(" << tmp << ", sizeof("
               << tmp << "), risk_float_fmt, (double)(";
            emit_expr_maybe_deref_when_read(
                os, arg, current_param_types, current_local_var_types,
                [&](std::ostream &o, Expr *ex) { emit_expr(o, ex); });
            os << ")); strncat(" << bufname << ", " << tmp << ", sizeof("
               << bufname << ") - strlen(" << bufname << ") - 1); ";
          } else {
            if (is_deref_expr) {
              std::ostringstream tmpval;
              emit_expr(tmpval, arg);
              os << "char " << tmp << "[64]; snprintf(" << tmp << ", sizeof("
                 << tmp << "), \"%p\", (void*)(" << tmpval.str()
                 << ")); strncat(" << bufname << ", " << tmp << ", sizeof("
                 << bufname << ") - strlen(" << bufname << ") - 1); ";
            } else {
              os << "char " << tmp << "[64]; snprintf(" << tmp << ", sizeof("
                 << tmp << "), \"%p\", (void*)(";
              emit_expr(os, arg);
              os << ")); strncat(" << bufname << ", " << tmp << ", sizeof("
                 << bufname << ") - strlen(" << bufname << ") - 1); ";
            }
          }
        }
        os << (id->name == "ioPrint" ? "core_print(" : "core_println(")
           << bufname << "); ";
        os << " })";
        return;
      }

      if (top_level_funcs.find(id->name) != top_level_funcs.end()) {
        bool is_ext_call =
            extern_func_names.find(id->name) != extern_func_names.end();
        os << (is_ext_call ? id->name : mangle(id->name)) << "(";

        FuncDecl *fdecl = nullptr;
        for (auto &d : tu->decls) {
          if (auto fd = dynamic_cast<FuncDecl *>(d.get())) {
            if (fd->name == id->name) {
              fdecl = fd;
              break;
            }
          }
        }
        size_t total_params = fdecl ? fdecl->params.size() : call->args.size();
        for (size_t i = 0; i < total_params; ++i) {
          if (i)
            os << ", ";
          if (i < call->args.size()) {
            emit_expr(os, call->args[i].get());
          } else if (fdecl && i < fdecl->params.size() &&
                     fdecl->params[i].default_val) {
            emit_expr(os, fdecl->params[i].default_val.get());
          }
        }
        os << ")";
        return;
      }

      auto pit = current_param_types.find(id->name);
      TypeNode *fn_tn = nullptr;
      std::string fn_type_name;

      if (pit != current_param_types.end()) {
        fn_tn = pit->second;

        fn_type_name = build_type_name_from_node(fn_tn);
      } else {
        auto lit = current_local_var_types.find(id->name);
        if (lit != current_local_var_types.end()) {
          fn_tn = lit->second;
          fn_type_name = build_type_name_from_node(fn_tn);

          if (fn_tn && fn_tn->name != "func" &&
              fn_type_name.rfind("func(", 0) != 0) {
            if (type_aliases &&
                type_aliases->find(fn_type_name) != type_aliases->end()) {
              fn_type_name = type_aliases->at(fn_type_name);
            }
          }
        }
      }

      if ((fn_tn && fn_tn->name == "func") ||
          (fn_type_name.rfind("func(", 0) == 0) ||
          (fn_tn && fn_tn->name != "func" && fn_type_name == fn_tn->name)) {

        if (fn_type_name != "func" && fn_type_name.rfind("func(", 0) != 0) {

          if (type_aliases &&
              type_aliases->find(fn_type_name) != type_aliases->end()) {
            fn_type_name = type_aliases->at(fn_type_name);
          } else {

            os << id->name << "(";
            for (size_t i = 0; i < call->args.size(); ++i) {
              if (i)
                os << ", ";
              emit_expr(os, call->args[i].get());
            }
            os << ")";
            return;
          }
        }

        std::vector<std::string> params;
        std::string ret;

        if (fn_type_name.empty() || fn_type_name.rfind("func(", 0) != 0) {

          os << id->name << "(";
          for (size_t i = 0; i < call->args.size(); ++i) {
            if (i)
              os << ", ";
            emit_expr(os, call->args[i].get());
          }
          os << ")";
          return;
        }

        parse_func_signature_string(fn_type_name, params, ret);

        std::string ret_ctype = func_return_ctype_from_string(ret);
        std::ostringstream cast_type;
        cast_type << ret_ctype << "(*)(void*";
        for (const auto &p : params) {
          cast_type << ", " << ctype_from_type_name(p);
        }
        cast_type << ")";

        os << "((" << cast_type.str() << ")";
        os << "(((risk_closure_t*)" << id->name << ")->fn))";
        os << "(((risk_closure_t*)" << id->name << ")->env";
        for (size_t i = 0; i < call->args.size(); ++i) {
          os << ", ";
          emit_expr(os, call->args[i].get());
        }
        os << ")";
        return;
      }

      std::string func_name = id->name;
      for (auto &d : tu->decls) {
        if (auto im = dynamic_cast<ImportDecl *>(d.get())) {
          func_name = module_name_from_import_path(im->path) + "__" + id->name;
          break;
        }
      }
      os << func_name << "(";
      for (size_t i = 0; i < call->args.size(); ++i) {
        if (i)
          os << ", ";
        emit_expr(os, call->args[i].get());
      }
      os << ")";
      return;
    }

    if (auto lam = dynamic_cast<LambdaExpr *>(call->callee.get())) {
      std::string id = prepare_immediate_wrapper(lam);
      std::string wrapper = "imm_wrapper_" + id;
      os << wrapper << "(";
      for (size_t i = 0; i < lam->captures.size(); ++i) {
        if (i)
          os << ", ";
        os << lam->captures[i];
      }
      if (!lam->captures.empty() && !call->args.empty())
        os << ", ";
      for (size_t i = 0; i < call->args.size(); ++i) {
        if (i)
          os << ", ";
        emit_expr(os, call->args[i].get());
      }
      os << ")";
      return;
    }

    if (auto mem = dynamic_cast<MemberExpr *>(call->callee.get())) {

      if (auto obj_id = dynamic_cast<IdentifierExpr *>(mem->object.get())) {

        bool is_variable = false;

        if (current_param_types.find(obj_id->name) !=
            current_param_types.end()) {
          is_variable = true;
        }

        else if (current_local_var_types.find(obj_id->name) !=
                 current_local_var_types.end()) {
          is_variable = true;
        }

        else if (global_var_types.find(obj_id->name) !=
                 global_var_types.end()) {
          is_variable = true;
        }

        if (!is_variable) {

          std::string module_name =
              get_module_name_from_alias(tu, obj_id->name);
          std::string mangled_name = module_name + "__" + mem->field;
          os << mangled_name << "(";
          for (size_t i = 0; i < call->args.size(); ++i) {
            if (i > 0)
              os << ", ";
            emit_expr(os, call->args[i].get());
          }
          os << ")";
          return;
        }
      }

      std::string obj_type;
      if (expr_type_map) {
        auto it = expr_type_map->find(mem->object.get());
        if (it != expr_type_map->end())
          obj_type = it->second.name;
      }
      if (obj_type.empty()) {
        obj_type = infer_type_name_for_expr(mem->object.get(), expr_type_map,
                                            current_param_types,
                                            current_local_var_types);
      }

      if (obj_type.rfind("Array<", 0) == 0) {
        std::string elem_type = extract_inner_type_name(obj_type);
        std::string c_elem_type = ctype_from_type_name(elem_type);

        bool obj_is_static_array = false;
        std::string static_array_size;
        {
          size_t open = obj_type.find('<');
          size_t close = obj_type.rfind('>');
          if (open != std::string::npos && close != std::string::npos &&
              close > open) {
            std::string contents = obj_type.substr(open + 1, close - open - 1);
            size_t comma = contents.find(',');
            if (comma != std::string::npos) {
              obj_is_static_array = true;
              static_array_size = contents.substr(comma + 1);
              if (static_array_size.rfind("__size__:", 0) == 0)
                static_array_size = static_array_size.substr(9);
            }
          }
        }

        if (obj_is_static_array && mem->field == "repr") {

          std::string tmp_sb = fresh_id("__repr_sb");
          std::string tmp_i = fresh_id("__repr_i");
          std::string tmp_raw = fresh_id("__repr_raw");
          std::string tmp_ep = fresh_id("__repr_ep");
          std::string tmp_buf = fresh_id("__repr_buf");

          bool is_int_type =
              (elem_type == "i8" || elem_type == "i16" || elem_type == "i32" ||
               elem_type == "i64" || elem_type == "u8" || elem_type == "u16" ||
               elem_type == "u32" || elem_type == "u64");
          bool is_float_type = (elem_type == "f32" || elem_type == "f64");
          bool is_bool_type = (elem_type == "bool");
          bool is_char_type = (elem_type == "char");
          bool is_string_type = (elem_type == "string");

          BlockField *user_repr_field =
              (is_int_type || is_float_type || is_bool_type || is_char_type ||
               is_string_type)
                  ? nullptr
                  : find_block_repr_field(elem_type);

          os << "({\n";
          os << "        risk_strbuf_t " << tmp_sb << ";\n";
          os << "        risk_strbuf_init(&" << tmp_sb << ");\n";
          os << "        risk_strbuf_append(&" << tmp_sb << ", \"[\");\n";
          os << "        bool " << tmp_raw << " = ";
          if (!call->args.empty())
            emit_expr(os, call->args[0].get());
          else
            os << "false";
          os << ";\n";
          os << "        (void)" << tmp_raw << ";\n";
          os << "        for (size_t " << tmp_i << " = 0; " << tmp_i << " < "
             << static_array_size << "; " << tmp_i << "++) {\n";
          os << "          if (" << tmp_i << " > 0) risk_strbuf_append(&"
             << tmp_sb << ", \", \");\n";
          os << "          " << c_elem_type << " *" << tmp_ep << " = &((";
          emit_expr(os, mem->object.get());
          os << ")[" << tmp_i << "]);\n";

          if (is_int_type) {
            os << "          char " << tmp_buf << "[32]; snprintf(" << tmp_buf
               << ", sizeof(" << tmp_buf << "), \"%lld\", (long long)(*"
               << tmp_ep << ")); risk_strbuf_append(&" << tmp_sb << ", "
               << tmp_buf << ");\n";
          } else if (is_float_type) {
            os << "          char " << tmp_buf << "[32]; snprintf(" << tmp_buf
               << ", sizeof(" << tmp_buf << "), risk_float_fmt, (double)(*" << tmp_ep
               << ")); risk_strbuf_append(&" << tmp_sb << ", " << tmp_buf
               << ");\n";
          } else if (is_bool_type) {
            os << "          risk_strbuf_append(&" << tmp_sb << ", (*" << tmp_ep
               << ") ? \"true\" : \"false\");\n";
          } else if (is_char_type) {
            os << "          char " << tmp_buf << "[2] = { *" << tmp_ep
               << ", '\\0' }; risk_strbuf_append(&" << tmp_sb << ", " << tmp_buf
               << ");\n";
          } else if (is_string_type) {
            os << "          risk_strbuf_append(&" << tmp_sb << ", *" << tmp_ep
               << " ? *" << tmp_ep << " : \"(null)\");\n";
          } else if (user_repr_field) {
            std::string ret_ctype = ctype_from_type_name("string");
            os << "          if (" << tmp_raw << ") {\n";
            os << "            char " << tmp_buf << "[32]; snprintf(" << tmp_buf
               << ", sizeof(" << tmp_buf << "), \"%p\", (void*)" << tmp_ep
               << "); risk_strbuf_append(&" << tmp_sb << ", " << tmp_buf
               << ");\n";
            os << "          } else {\n";

            os << "            char *" << tmp_buf << "_r = ((" << ret_ctype
               << "(*)(void*, " << c_elem_type << "*))(((risk_closure_t*)(("
               << tmp_ep << ")->repr))->fn))(((risk_closure_t*)((" << tmp_ep
               << ")->repr))->env, " << tmp_ep << "); risk_strbuf_append(&"
               << tmp_sb << ", " << tmp_buf << "_r ? " << tmp_buf
               << "_r : \"(null)\");\n";
            os << "          }\n";
          } else {
            os << "          char " << tmp_buf << "[32]; snprintf(" << tmp_buf
               << ", sizeof(" << tmp_buf << "), \"%p\", (void*)" << tmp_ep
               << "); risk_strbuf_append(&" << tmp_sb << ", " << tmp_buf
               << ");\n";
          }

          os << "        }\n";
          os << "        risk_strbuf_append(&" << tmp_sb << ", \"]\");\n";
          os << "        risk_strbuf_finish(&" << tmp_sb << ");\n";
          os << "      })";
          return;
        }

        if (mem->field == "push") {

          std::string tmp_var = fresh_id("__push_tmp");
          os << "({\n";
          os << "        " << c_elem_type << " " << tmp_var << " = ";
          emit_expr(os, call->args[0].get());
          os << ";\n";
          os << "        risk_array_push(";
          emit_expr(os, mem->object.get());
          os << ", &" << tmp_var << ");\n";
          os << "      })";
          return;
        } else if (mem->field == "pop") {

          std::string tmp_var = fresh_id("__pop_val");
          os << "({\n";
          os << "        " << c_elem_type << " " << tmp_var << ";\n";
          os << "        risk_array_pop(";
          emit_expr(os, mem->object.get());
          os << ", &" << tmp_var << ");\n";
          os << "        " << tmp_var << ";\n";
          os << "      })";
          return;
        } else if (mem->field == "pushFront") {
          std::string tmp_var = fresh_id("__pushf_tmp");
          os << "({\n";
          os << "        " << c_elem_type << " " << tmp_var << " = ";
          emit_expr(os, call->args[0].get());
          os << ";\n";
          os << "        risk_array_push_front(";
          emit_expr(os, mem->object.get());
          os << ", &" << tmp_var << ");\n";
          os << "      })";
          return;
        } else if (mem->field == "popFront") {
          std::string tmp_var = fresh_id("__popf_val");
          os << "({\n";
          os << "        " << c_elem_type << " " << tmp_var << ";\n";
          os << "        risk_array_pop_front(";
          emit_expr(os, mem->object.get());
          os << ", &" << tmp_var << ");\n";
          os << "        " << tmp_var << ";\n";
          os << "      })";
          return;
        } else if (mem->field == "insert") {

          std::string tmp_var = fresh_id("__ins_tmp");
          os << "({\n";
          os << "        " << c_elem_type << " " << tmp_var << " = ";
          emit_expr(os, call->args[1].get());
          os << ";\n";
          os << "        risk_array_insert(";
          emit_expr(os, mem->object.get());
          os << ", (size_t)(";
          emit_expr(os, call->args[0].get());
          os << "), &" << tmp_var << ");\n";
          os << "      })";
          return;
        } else if (mem->field == "removeAt") {
          os << "risk_array_remove_at(";
          emit_expr(os, mem->object.get());
          os << ", (size_t)(";
          emit_expr(os, call->args[0].get());
          os << "))";
          return;
        } else if (mem->field == "clear") {
          os << "risk_array_clear(";
          emit_expr(os, mem->object.get());
          os << ")";
          return;
        } else if (mem->field == "repr") {
          std::string tmp_arr = fresh_id("__repr_arr");
          std::string tmp_sb = fresh_id("__repr_sb");
          std::string tmp_i = fresh_id("__repr_i");
          std::string tmp_n = fresh_id("__repr_n");
          std::string tmp_raw = fresh_id("__repr_raw");
          std::string tmp_ep = fresh_id("__repr_ep");
          std::string tmp_buf = fresh_id("__repr_buf");

          bool is_int_type =
              (elem_type == "i8" || elem_type == "i16" || elem_type == "i32" ||
               elem_type == "i64" || elem_type == "u8" || elem_type == "u16" ||
               elem_type == "u32" || elem_type == "u64");
          bool is_float_type = (elem_type == "f32" || elem_type == "f64");
          bool is_bool_type = (elem_type == "bool");
          bool is_char_type = (elem_type == "char");
          bool is_string_type = (elem_type == "string");

          BlockField *user_repr_field =
              (is_int_type || is_float_type || is_bool_type || is_char_type ||
               is_string_type)
                  ? nullptr
                  : find_block_repr_field(elem_type);

          os << "({\n";
          os << "        risk_array_t *" << tmp_arr << " = ";
          emit_expr(os, mem->object.get());
          os << ";\n";
          os << "        risk_strbuf_t " << tmp_sb << ";\n";
          os << "        risk_strbuf_init(&" << tmp_sb << ");\n";
          os << "        risk_strbuf_append(&" << tmp_sb << ", \"[\");\n";
          os << "        size_t " << tmp_n << " = risk_array_length(" << tmp_arr
             << ");\n";
          os << "        bool " << tmp_raw << " = ";
          if (!call->args.empty())
            emit_expr(os, call->args[0].get());
          else
            os << "false";
          os << ";\n";
          os << "        (void)" << tmp_raw << ";\n";
          os << "        for (size_t " << tmp_i << " = 0; " << tmp_i << " < "
             << tmp_n << "; " << tmp_i << "++) {\n";
          os << "          if (" << tmp_i << " > 0) risk_strbuf_append(&"
             << tmp_sb << ", \", \");\n";
          os << "          " << c_elem_type << " *" << tmp_ep << " = ("
             << c_elem_type << "*)risk_array_get(" << tmp_arr << ", " << tmp_i
             << ");\n";

          if (is_int_type) {
            os << "          char " << tmp_buf << "[32]; snprintf(" << tmp_buf
               << ", sizeof(" << tmp_buf << "), \"%lld\", (long long)(*"
               << tmp_ep << ")); risk_strbuf_append(&" << tmp_sb << ", "
               << tmp_buf << ");\n";
          } else if (is_float_type) {
            os << "          char " << tmp_buf << "[32]; snprintf(" << tmp_buf
               << ", sizeof(" << tmp_buf << "), risk_float_fmt, (double)(*" << tmp_ep
               << ")); risk_strbuf_append(&" << tmp_sb << ", " << tmp_buf
               << ");\n";
          } else if (is_bool_type) {
            os << "          risk_strbuf_append(&" << tmp_sb << ", (*" << tmp_ep
               << ") ? \"true\" : \"false\");\n";
          } else if (is_char_type) {
            os << "          char " << tmp_buf << "[2] = { *" << tmp_ep
               << ", '\\0' }; risk_strbuf_append(&" << tmp_sb << ", " << tmp_buf
               << ");\n";
          } else if (is_string_type) {
            os << "          risk_strbuf_append(&" << tmp_sb << ", *" << tmp_ep
               << " ? *" << tmp_ep << " : \"(null)\");\n";
          } else if (user_repr_field) {
            std::string ret_ctype = ctype_from_type_name("string");
            os << "          if (" << tmp_raw << ") {\n";
            os << "            char " << tmp_buf << "[32]; snprintf(" << tmp_buf
               << ", sizeof(" << tmp_buf << "), \"%p\", (void*)" << tmp_ep
               << "); risk_strbuf_append(&" << tmp_sb << ", " << tmp_buf
               << ");\n";
            os << "          } else {\n";

            os << "            char *" << tmp_buf << "_r = ((" << ret_ctype
               << "(*)(void*, " << c_elem_type << "*))(((risk_closure_t*)(("
               << tmp_ep << ")->repr))->fn))(((risk_closure_t*)((" << tmp_ep
               << ")->repr))->env, " << tmp_ep << "); risk_strbuf_append(&"
               << tmp_sb << ", " << tmp_buf << "_r ? " << tmp_buf
               << "_r : \"(null)\");\n";
            os << "          }\n";
          } else {

            os << "          char " << tmp_buf << "[32]; snprintf(" << tmp_buf
               << ", sizeof(" << tmp_buf << "), \"%p\", (void*)" << tmp_ep
               << "); risk_strbuf_append(&" << tmp_sb << ", " << tmp_buf
               << ");\n";
          }

          os << "        }\n";
          os << "        risk_strbuf_append(&" << tmp_sb << ", \"]\");\n";
          os << "        risk_strbuf_finish(&" << tmp_sb << ");\n";
          os << "      })";
          return;
        }

        else if (mem->field == "freeAll") {

          if (elem_type.rfind("rc<", 0) == 0) {
            os << "risk_array_free_all_rc(";
          } else {
            os << "risk_array_free_all_ptrs(";
          }
          emit_expr(os, mem->object.get());
          os << ")";
          return;
        } else if (mem->field == "destroy") {
          os << "risk_array_destroy(";
          emit_expr(os, mem->object.get());
          os << ")";
          return;
        }
      }

      if (obj_type.rfind("Arena", 0) == 0 || obj_type == "Arena") {
        if (mem->field == "alloc") {
          os << "risk_arena_alloc(";
          emit_expr(os, mem->object.get());
          os << ", (size_t)(";
          emit_expr(os, call->args[0].get());
          os << "))";
          return;
        } else if (mem->field == "reset") {
          os << "risk_arena_reset(";
          emit_expr(os, mem->object.get());
          os << ")";
          return;
        } else if (mem->field == "destroy") {
          os << "risk_arena_destroy(";
          emit_expr(os, mem->object.get());
          os << ")";
          return;
        } else if (mem->field == "used") {

          os << "((uint64_t)risk_arena_total_used(";
          emit_expr(os, mem->object.get());
          os << "))";
          return;
        } else if (mem->field == "KiB") {

          os << "risk_arena_used_kib(";
          emit_expr(os, mem->object.get());
          os << ")";
          return;
        } else if (mem->field == "MiB") {
          os << "risk_arena_used_mib(";
          emit_expr(os, mem->object.get());
          os << ")";
          return;
        } else if (mem->field == "GiB") {
          os << "risk_arena_used_gib(";
          emit_expr(os, mem->object.get());
          os << ")";
          return;
        } else if (mem->field == "capacityBytes") {
          os << "((uint64_t)risk_arena_capacity(";
          emit_expr(os, mem->object.get());
          os << "))";
          return;
        } else if (mem->field == "capacityKiB") {
          os << "risk_arena_capacity_kib(";
          emit_expr(os, mem->object.get());
          os << ")";
          return;
        } else if (mem->field == "capacityMiB") {
          os << "risk_arena_capacity_mib(";
          emit_expr(os, mem->object.get());
          os << ")";
          return;
        } else if (mem->field == "capacityGiB") {
          os << "risk_arena_capacity_gib(";
          emit_expr(os, mem->object.get());
          os << ")";
          return;
        } else if (mem->field == "scratchBegin") {
          os << "risk_arena_scratch_begin(";
          emit_expr(os, mem->object.get());
          os << ")";
          return;
        } else if (mem->field == "scratchEnd") {
          os << "risk_arena_scratch_end(";
          emit_expr(os, mem->object.get());
          os << ", ";
          emit_expr(os, call->args[0].get());
          os << ")";
          return;
        }
      }

      if (obj_type.rfind("Hashmap<", 0) == 0) {
        size_t open = obj_type.find('<');
        size_t close = obj_type.rfind('>');
        std::string key_type, value_type;
        if (open != std::string::npos && close != std::string::npos) {
          std::string contents = obj_type.substr(open + 1, close - open - 1);
          size_t comma = contents.find(',');
          if (comma != std::string::npos) {
            key_type = contents.substr(0, comma);
            value_type = contents.substr(comma + 1);

            while (!key_type.empty() && key_type.back() == ' ')
              key_type.pop_back();
            while (!key_type.empty() && key_type[0] == ' ')
              key_type = key_type.substr(1);
            while (!value_type.empty() && value_type.back() == ' ')
              value_type.pop_back();
            while (!value_type.empty() && value_type[0] == ' ')
              value_type = value_type.substr(1);
          }
        }
        std::string c_key_type = ctype_from_type_name(key_type);
        std::string c_value_type = ctype_from_type_name(value_type);

        if (mem->field == "has" || mem->field == "contains") {

          if (call->args.size() != 1) {
            os << "false /* " << mem->field << "() expects 1 argument */";
            return;
          }
          std::string key_tmp = fresh_id("__hm_key");
          os << "({\n";
          os << "        " << c_key_type << " " << key_tmp << " = ";
          emit_expr(os, call->args[0].get());
          os << ";\n";
          os << "        risk_hashmap_has(";
          emit_expr(os, mem->object.get());
          os << ", &" << key_tmp << ");\n";
          os << "      })";
          return;
        } else if (mem->field == "size") {

          if (call->args.size() != 0) {
            os << "0 /* size() expects 0 arguments */";
            return;
          }
          os << "risk_hashmap_size(";
          emit_expr(os, mem->object.get());
          os << ")";
          return;
        } else if (mem->field == "remove" || mem->field == "delete") {

          if (call->args.size() != 1) {
            os << "/* " << mem->field << "() expects 1 argument */";
            return;
          }
          std::string key_tmp = fresh_id("__hm_key");
          os << "({\n";
          os << "        " << c_key_type << " " << key_tmp << " = ";
          emit_expr(os, call->args[0].get());
          os << ";\n";
          os << "        risk_hashmap_remove(";
          emit_expr(os, mem->object.get());
          os << ", &" << key_tmp << ");\n";
          os << "      })";
          return;
        } else if (mem->field == "clear") {

          if (call->args.size() != 0) {
            os << "/* clear() expects 0 arguments */";
            return;
          }
          os << "risk_hashmap_clear(";
          emit_expr(os, mem->object.get());
          os << ")";
          return;
        }
      }

      std::ostringstream mem_os;
      emit_expr(mem_os, mem);
      std::string mem_str = mem_os.str();

      std::string fn_type_name;
      if (expr_type_map) {
        auto it = expr_type_map->find(mem);
        if (it != expr_type_map->end()) {
          fn_type_name = it->second.name;
        }
      }

      bool first_param_is_this = false;
      {
        std::string this_check_obj_type;
        if (expr_type_map) {
          auto it = expr_type_map->find(mem->object.get());
          if (it != expr_type_map->end())
            this_check_obj_type = it->second.name;
        }
        if (this_check_obj_type.empty()) {
          this_check_obj_type = infer_type_name_for_expr(
              mem->object.get(), expr_type_map, current_param_types,
              current_local_var_types);
        }
        std::string this_check_block_name = this_check_obj_type;
        if (this_check_block_name.rfind("ref<", 0) == 0 ||
            this_check_block_name.rfind("ptr<", 0) == 0) {
          size_t p = this_check_block_name.find('<'),
                 q = this_check_block_name.rfind('>');
          if (p != std::string::npos && q != std::string::npos && q > p + 1)
            this_check_block_name =
                this_check_block_name.substr(p + 1, q - p - 1);
        }
        BlockDecl *bcheck = find_block_decl(tu, this_check_block_name);
        if (bcheck) {
          for (const auto &field : bcheck->fields) {
            if (field.name == mem->field && field.type &&
                field.type->name == "func" && !field.type->params.empty() &&
                field.type->params[0]->is_this_param) {
              first_param_is_this = true;
              break;
            }
          }
        }
      }

      if (fn_type_name.empty()) {

        std::string obj_type;
        if (expr_type_map) {
          auto it = expr_type_map->find(mem->object.get());
          if (it != expr_type_map->end())
            obj_type = it->second.name;
        }
        if (obj_type.empty()) {
          obj_type = infer_type_name_for_expr(mem->object.get(), expr_type_map,
                                              current_param_types,
                                              current_local_var_types);
        }

        std::string block_name = obj_type;
        if (block_name.rfind("ref<", 0) == 0 ||
            block_name.rfind("ptr<", 0) == 0) {
          size_t p = block_name.find('<'), q = block_name.rfind('>');
          if (p != std::string::npos && q != std::string::npos && q > p + 1)
            block_name = block_name.substr(p + 1, q - p - 1);
        }

        BlockDecl *b = find_block_decl(tu, block_name);
        if (b) {
          for (const auto &field : b->fields) {
            if (field.name == mem->field && field.type) {
              fn_type_name = build_type_name_from_node(field.type.get());
              break;
            }
          }
        }
      }

      if (!fn_type_name.empty() && fn_type_name.rfind("func(", 0) == 0) {

        std::vector<std::string> params;
        std::string ret;
        parse_func_signature_string(fn_type_name, params, ret);

        std::string ret_ctype = func_return_ctype_from_string(ret);
        std::ostringstream cast_type;
        cast_type << ret_ctype << "(*)(void*";
        for (size_t pi = 0; pi < params.size(); ++pi) {
          std::string pctype = ctype_from_type_name(params[pi]);
          if (pi == 0 && first_param_is_this)
            pctype += "*";
          cast_type << ", " << pctype;
        }
        cast_type << ")";

        os << "((" << cast_type.str() << ")" << "(((risk_closure_t*)("
           << mem_str << "))->fn))" << "(((risk_closure_t*)(" << mem_str
           << "))->env";
        if (!params.empty()) {
          os << ", ";
          if (first_param_is_this) {
            os << "&(";
            emit_expr(os, mem->object.get());
            os << ")";
          } else {
            emit_expr(os, mem->object.get());
          }
        }
        for (size_t i = 0; i < call->args.size(); ++i) {
          os << ", ";
          emit_expr(os, call->args[i].get());
        }
        os << ")";
        return;
      }
    }

    std::ostringstream callee;
    emit_expr(callee, call->callee.get());
    os << callee.str() << "(";
    for (size_t i = 0; i < call->args.size(); ++i) {
      if (i)
        os << ", ";
      emit_expr(os, call->args[i].get());
    }
    os << ")";
    return;
  }

  if (auto lam = dynamic_cast<LambdaExpr *>(e)) {
    std::string id = prepare_lambda(lam);
    std::string fname = make_lambda_factory_name(id);
    os << fname << "(";
    for (size_t i = 0; i < lam->captures.size(); ++i) {
      if (i)
        os << ", ";
      os << lam->captures[i];
    }
    os << ")";
    return;
  }

  if (auto sof = dynamic_cast<SizeofExpr *>(e)) {
    os << "sizeof(";

    if (sof->type_arg) {

      std::string type_name;
      if (sof->type_arg->name == "func") {

        os << "void*";
      } else {

        type_name = sof->type_arg->name;
        std::string c_type = ctype_from_type_name(type_name);
        os << c_type;
      }
    } else if (sof->expr_arg) {

      emit_expr(os, sof->expr_arg.get());
    } else {

      os << "void";
    }

    os << ")";
    return;
  }

  if (auto idx = dynamic_cast<IndexExpr *>(e)) {

    std::string obj_type;
    if (expr_type_map) {
      auto it = expr_type_map->find(idx->object.get());
      if (it != expr_type_map->end())
        obj_type = it->second.name;
    }
    if (obj_type.empty()) {
      obj_type = infer_type_name_for_expr(idx->object.get(), expr_type_map,
                                          current_param_types,
                                          current_local_var_types);
    }

    if (obj_type.rfind("Hashmap<", 0) == 0) {

      size_t open = obj_type.find('<');
      size_t close = obj_type.rfind('>');
      std::string key_type, value_type;
      if (open != std::string::npos && close != std::string::npos) {
        std::string contents = obj_type.substr(open + 1, close - open - 1);
        size_t comma = contents.find(',');
        if (comma != std::string::npos) {
          key_type = contents.substr(0, comma);
          value_type = contents.substr(comma + 1);

          while (!key_type.empty() && key_type.back() == ' ')
            key_type.pop_back();
          while (!value_type.empty() && value_type[0] == ' ')
            value_type = value_type.substr(1);
          while (!value_type.empty() && value_type.back() == ' ')
            value_type.pop_back();
        }
      }
      std::string c_key_type = ctype_from_type_name(key_type);
      std::string c_value_type = ctype_from_type_name(value_type);

      std::string key_tmp = fresh_id("__hm_key");
      os << "({\n";
      os << "        " << c_key_type << " " << key_tmp << " = ";
      emit_expr(os, idx->index.get());
      os << ";\n";
      os << "        (*(" << c_value_type << "*)risk_hashmap_get(";
      emit_expr(os, idx->object.get());
      os << ", &" << key_tmp << "));\n";
      os << "      })";
      return;
    }

    bool is_dynamic_array = false;
    std::string elem_type;
    if (obj_type.rfind("Array<", 0) == 0) {

      size_t open = obj_type.find('<');
      size_t close = obj_type.rfind('>');
      if (open != std::string::npos && close != std::string::npos) {
        std::string contents = obj_type.substr(open + 1, close - open - 1);

        int depth = 0, top_commas = 0;
        for (char ch : contents) {
          if (ch == '<')
            ++depth;
          else if (ch == '>')
            --depth;
          else if (ch == ',' && depth == 0)
            ++top_commas;
        }
        if (top_commas == 0) {
          is_dynamic_array = true;
          elem_type = extract_inner_type_name(obj_type);
        }
      }
    }

    if (is_dynamic_array) {

      std::string c_elem_type = ctype_from_type_name(elem_type);

      bool is_ptr_elem =
          (elem_type == "string" || elem_type.rfind("ptr<", 0) == 0);

      if (is_ptr_elem) {
        os << "(*(" << c_elem_type << "*)risk_array_get(";
      } else {
        os << "(*(" << c_elem_type << "*)risk_array_get(";
      }
      emit_expr(os, idx->object.get());
      os << ", ";
      emit_expr(os, idx->index.get());
      os << "))";
    } else {

      os << "(";
      emit_expr(os, idx->object.get());
      os << ")[";
      emit_expr(os, idx->index.get());
      os << "]";
    }
    return;
  }

  if (auto al = dynamic_cast<ArrayLiteralExpr *>(e)) {

    bool is_static_array = false;
    std::string array_size;
    std::string elem_type = "void";

    if (expr_type_map) {
      auto it = expr_type_map->find(e);
      if (it != expr_type_map->end()) {
        std::string type_name = it->second.name;

        if (type_name.rfind("Array<", 0) == 0) {
          size_t open = type_name.find('<');
          size_t close = type_name.rfind('>');
          if (open != std::string::npos && close != std::string::npos) {
            std::string contents = type_name.substr(open + 1, close - open - 1);
            size_t comma = contents.find(',');
            if (comma != std::string::npos) {

              is_static_array = true;
              elem_type = contents.substr(0, comma);
              array_size = contents.substr(comma + 1);

              elem_type.erase(0, elem_type.find_first_not_of(" \t"));
              elem_type.erase(elem_type.find_last_not_of(" \t") + 1);
              array_size.erase(0, array_size.find_first_not_of(" \t"));
              array_size.erase(array_size.find_last_not_of(" \t") + 1);

              if (array_size.rfind("__size__:", 0) == 0) {
                array_size = array_size.substr(9);
              }
            } else {

              elem_type = contents;
            }
          }
        }
      }
    }

    if (elem_type == "void" && !al->elements.empty()) {
      if (expr_type_map) {
        auto it = expr_type_map->find(al->elements[0].get());
        if (it != expr_type_map->end()) {
          elem_type = it->second.name;
        }
      }
      if (elem_type == "void") {
        elem_type = infer_type_name_for_expr(al->elements[0].get(),
                                             expr_type_map, current_param_types,
                                             current_local_var_types);
      }
    }

    std::string c_elem_type = ctype_from_type_name(elem_type);

    if (is_static_array) {

      os << "{ ";
      for (size_t i = 0; i < al->elements.size(); ++i) {
        if (i)
          os << ", ";

        if (elem_type == "f32") {
          os << "(float)(";
          emit_expr(os, al->elements[i].get());
          os << ")";
        } else {
          emit_expr(os, al->elements[i].get());
        }
      }
      os << " }";
      return;
    }

    if (al->elements.empty()) {

      os << "risk_array_create(sizeof(" << c_elem_type << "))";
      return;
    }

    os << "({\n";
    os << "      risk_array_t* __arr = risk_array_create(sizeof(" << c_elem_type
       << "));\n";
    for (size_t i = 0; i < al->elements.size(); ++i) {
      os << "      { " << c_elem_type << " __elem = (" << c_elem_type << ")(";
      emit_expr(os, al->elements[i].get());
      os << "); risk_array_push(__arr, &__elem); }\n";
    }
    os << "      __arr;\n";
    os << "    })";
    return;
  }

  os << "/* expr-not-implemented */";
}

std::string CodeGen::emit_expr_to_string(Expr *e) {
  std::ostringstream ss;
  emit_expr(ss, e);
  return ss.str();
}
