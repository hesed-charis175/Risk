#pragma once

#include "ast.h"
#include "typechecker.h"
#include <map>
#include <ostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

class CodeGen {
public:
  CodeGen(
      TranslationUnit *tu_, const std::string &out_c_path,
      const std::string &module_name,
      const std::unordered_map<Expr *, std::string> *compile_time_map = nullptr,
      const std::unordered_map<Expr *, TypeInfo> *expr_type_map = nullptr,
      const std::unordered_map<std::string, std::string> *type_aliases_ =
          nullptr,
      bool emit_debug_lines = false);

  bool generate();

private:
  TranslationUnit *tu;

  std::string out_path;

  std::string module;

  bool debug_lines_enabled = false;

  int last_emitted_line = -1;

  std::string build_type_name_from_node(TypeNode *tn);

  unsigned long unique_id = 1;

  std::string c_source;

  std::unordered_map<std::string, bool> top_level_funcs;

  std::unordered_set<std::string> extern_func_names;

  std::unordered_map<LambdaExpr *, std::string> lambda_ids;

  std::unordered_map<LambdaExpr *, std::string> immediate_wrapper_ids;

  std::string helper_code;

  std::string helper_prototypes;

  std::unordered_map<std::string, std::string> capture_map;

  std::unordered_map<std::string, TypeNode *> current_param_types;

  std::unordered_map<std::string, TypeNode *> current_local_var_types;

  std::unordered_map<std::string, TypeNode *> global_var_types;

  const std::unordered_map<Expr *, std::string> *compile_time_map = nullptr;

  const std::unordered_map<Expr *, TypeInfo> *expr_type_map = nullptr;

  const std::unordered_map<std::string, std::string> *type_aliases = nullptr;

  std::unordered_map<std::string, std::unique_ptr<TypeNode>>
      resolved_type_nodes;

  std::unordered_map<std::string, std::string> array_return_wrappers;

  std::string array_return_wrapper_typedefs;

  bool current_return_is_array = false;

  std::string current_return_wrapper;

  std::string current_return_array_base;

  std::string current_return_array_suffix;

  std::string func_return_ctype(TypeNode *ret_tn, bool &is_array,
                                std::string &array_base,
                                std::string &array_suffix);

  std::string wrap_array_return_expr(Expr *e, const std::string &wrapper,
                                     const std::string &array_base,
                                     const std::string &array_suffix);

  std::string func_return_ctype_from_string(const std::string &ret);

  std::string current_defer_cleanup_label;

  std::string current_defer_ret_var;

  bool current_defer_has_ret_value = false;

  std::vector<DeferStmt *> current_defer_list;

  std::unordered_map<DeferStmt *, std::string> defer_flag_names;

  void collect_defers(Stmt *s, std::vector<DeferStmt *> &out);

  void emit_body_with_defers(std::ostream &os, const StmtList &body,
                             const std::string &ret_ctype, int indent);

  BlockField *find_block_repr_field(const std::string &elem_type_name);

  BlockField *find_block_init_field(const std::string &block_type_name);

  void emit_auto_init_call(std::ostream &os, const std::string &ind,
                           const std::string &var_expr,
                           const std::string &block_type_name);

  std::unordered_set<std::string> this_ref_funcs;

  std::string current_this_ref_param_name;

  std::string current_this_ref_block_type;

  void collect_this_ref_funcs();

  bool is_this_ref_object(Expr *obj) const;

  std::string mangle(const std::string &name);

  std::string block_struct_name(BlockDecl *b);

  std::string fresh_id(const std::string &prefix);

  void emit_top_level_func_names();

  void emit_register_decl(RegisterDecl *rd);

  std::string map_risk_type_to_c(const std::string &risk_type);

  void collect_lambdas();

  std::string prepare_lambda(LambdaExpr *lam);

  std::string prepare_immediate_wrapper(LambdaExpr *lam);

  std::string make_lambda_env_name(const std::string &id);

  std::string make_lambda_wrapper_name(const std::string &id);

  std::string make_lambda_factory_name(const std::string &id);

  bool generate_file(const std::string &path);

  void
  ensure_closure_structs_and_helpers_for_lambda_with_id(LambdaExpr *lam,
                                                        const std::string &id);

  void ensure_immediate_wrapper_for_lambda_with_id(LambdaExpr *lam,
                                                   const std::string &id);

  void emit_func(FuncDecl *f, std::ostream &os);

  void emit_stmt(std::ostream &os, Stmt *s, int indent = 0);

  void emit_expr(std::ostream &os, Expr *e);

  std::string emit_expr_to_string(Expr *e);

  void emit_lvalue(std::ostream &os, Expr *e);

  std::string ctype_from_tn(TypeNode *tn);

  std::string ctype_from_type_name(const std::string &type_name);

  std::pair<std::string, std::string> flatten_static_array(TypeNode *tn);

  bool flatten_ptr_to_array(TypeNode *tn, std::string &base,
                            std::string &suffix);

  std::pair<std::string, std::string>
  split_c_array_suffix(const std::string &ctype);
};
