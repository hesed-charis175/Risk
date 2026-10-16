#pragma once

#include "ast.h"
#include "diagnostic.h"
#include "token.h"
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct TypeInfo {

  std::string name;

  TypeInfo() = default;

  explicit TypeInfo(std::string n) : name(std::move(n)) {}

  bool operator==(const TypeInfo &o) const { return name == o.name; }

  bool operator!=(const TypeInfo &o) const { return name != o.name; }
};

class TypeChecker {
public:
  TypeChecker() = default;

  std::vector<Diagnostic> check(TranslationUnit *tu);

  std::optional<TypeInfo> lookup_type_at(const TranslationUnit *tu, int line,
                                         int col);

  std::unordered_map<Expr *, std::string> compile_time_string_map;

  std::unordered_map<std::string, std::string> type_aliases;

  std::unordered_map<Expr *, TypeInfo> expr_type_map;

  std::vector<std::string> imported_dirs;

  std::vector<std::string> extern_link_sources;

  std::vector<std::string> extern_link_libs;

private:

  struct VarSymbol {
    std::string name;
    TypeInfo type;
    bool is_const;
    int line, col;
  };

  struct FuncSymbol {
    std::string name;
    std::vector<TypeInfo> params;
    TypeInfo ret;
    int line, col;
    FuncDecl *decl;
  };

  std::vector<std::unordered_map<std::string, VarSymbol>> var_scopes;

  std::unordered_map<std::string, FuncSymbol> functions;

  std::unordered_map<std::string, std::vector<std::pair<std::string, TypeInfo>>>
      blocks;

  std::unordered_map<std::string, BlockDecl *> block_decls;

  std::unordered_map<std::string, std::vector<std::pair<std::string, int>>>
      opts;

  int loop_depth = 0;

  int switch_depth = 0;

  int defer_depth = 0;

  struct FreedEntry { int line; int col; };

  std::unordered_map<std::string, FreedEntry> freed_ptrs;

  bool block_has_owned_ptr_fields(const std::string &block_name);

  std::vector<Diagnostic> diags;

  void push_scope();

  void pop_scope();

  std::string load_imported_header(const std::string &import_path,
                                   const std::string &source_dir);

  bool add_var(const std::string &name, const TypeInfo &t, bool is_const,
               int line, int col);

  bool add_func(const std::string &name, const std::vector<TypeInfo> &params,
                const TypeInfo &ret, int line, int col, FuncDecl *decl);

  std::optional<VarSymbol> lookup_var(const std::string &name);

  std::optional<FuncSymbol> lookup_func(const std::string &name);

  TypeInfo type_of_expr(Expr *e);

  void check_stmt(Stmt *s, const TypeInfo &expected_return);

  void check_func(FuncDecl *f);

  void analyze_lambda_captures(LambdaExpr *lambda);

  void collect_free_vars_in_stmt(Stmt *s,
                                 std::unordered_set<std::string> &used);

  void collect_free_vars_in_expr(Expr *e,
                                 std::unordered_set<std::string> &used);

  void collect_local_var_names_in_stmt(Stmt *s,
                                       std::unordered_set<std::string> &locals);

  TypeInfo type_from_typenode(TypeNode *tn);

  bool register_block(BlockDecl *b);

  std::optional<TypeInfo> lookup_block_field_type(const std::string &blockname,
                                                  const std::string &field);

  bool register_opt(OptDecl *b);

  std::optional<int> lookup_opt_field_type(const std::string &optname,
                                           const std::string &field);

  bool stamp_block_literal(BlockLiteralExpr *bl, const std::string &target,
                           int err_line, int err_col,
                           const std::string &ctx_name);

  void register_type_alias(TypeAliasDecl *ta);

  std::string resolve_type_alias(const std::string &name);

  std::unordered_map<std::string, std::string> imported_aliases;

  std::unordered_set<std::string> imported_files_;

  TranslationUnit *current_tu = nullptr;

  std::unordered_map<std::string, std::vector<TypeNode *>> generic_sets;

  std::unordered_map<std::string, FuncDecl *> generic_templates;

  std::unordered_map<std::string, FuncDecl *> generic_instances;

  std::vector<FuncDecl *> generic_worklist;

  void register_generic_set(GenericSetDecl *g);

  bool type_matches_pattern(const std::string &concrete_name, TypeNode *pattern);

  bool type_matches_generic_set(const std::string &concrete_name,
                                const std::string &set_name);

  bool unify_generic_param(TypeNode *pattern, const std::string &concrete_name,
                           const std::vector<GenericParam> &gparams,
                           std::unordered_map<std::string, std::string> &bindings,
                           std::string &err);

  TypePtr substitute_type(TypeNode *tn,
                          const std::unordered_map<std::string, std::string> &bindings);

  ExprPtr substitute_expr(Expr *e,
                         const std::unordered_map<std::string, std::string> &bindings);

  StmtPtr substitute_stmt(Stmt *s,
                         const std::unordered_map<std::string, std::string> &bindings);

  std::string
  mangle_generic_instance(const std::string &base_name,
                          const std::vector<GenericParam> &gparams,
                          const std::unordered_map<std::string, std::string> &bindings);

  FuncDecl *
  instantiate_generic_func(FuncDecl *tmpl,
                           const std::unordered_map<std::string, std::string> &bindings,
                           const std::string &mangled_name);

  std::unordered_map<std::string, BlockDecl *> generic_block_templates;

  std::unordered_map<std::string, BlockDecl *> generic_block_instances;

  TypeInfo resolve_generic_block_type(TypeNode *tn);

  BlockDecl *instantiate_generic_block(
      BlockDecl *tmpl, const std::unordered_map<std::string, std::string> &bindings,
      const std::string &mangled_name);

  TypeInfo resolve_generic_call(CallExpr *call, IdentifierExpr *id, FuncDecl *tmpl);
};
