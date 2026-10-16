#pragma once

#include "ast.h"
#include "diagnostic.h"
#include "token.h"
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

class Parser {
public:
  Parser(const std::vector<Token> &tokens);

  std::unique_ptr<TranslationUnit> parse_translation_unit();

  std::vector<Diagnostic> diags;

private:
  const std::vector<Token> toks;

  size_t idx;

  std::vector<DeclPtr> pending_global_decls;

  const Token &peek() const;

  const Token &peek_n(size_t n) const;

  const Token &advance();

  bool is_at_end() const;

  bool match_symbol(const std::string &s);

  bool match_keyword_header_file() const;

  void consume_if_symbol(const std::string &s);

  void expect_symbol(const std::string &s, const std::string &err = "");

  bool peek_is_case_keyword() const;

  DeclPtr parse_decl();

  DeclPtr parse_typealias();

  DeclPtr parse_generic_set_decl();

  DeclPtr parse_import();

  DeclPtr parse_extern_block();

  DeclPtr parse_func_decl_or_def();

  void parse_generic_param_list(std::vector<GenericParam> &out);

  std::unordered_set<std::string> value_generic_scope;

  bool expr_only_references_value_generics(Expr *e) const;

  DeclPtr parse_block_decl();

  void parse_block_body(BlockDecl *bdecl);

  DeclPtr parse_opt_decl();

  FuncDecl *parse_func_signature(std::unique_ptr<FuncDecl> f);

  StmtPtr parse_statement();

  StmtPtr parse_block();

  StmtPtr parse_if();

  StmtPtr parse_while();

  StmtPtr parse_switch();

  StmtPtr parse_for();

  StmtPtr parse_return();

  StmtPtr parse_defer();

  StmtPtr parse_multi_var_decl_statement();

  StmtPtr parse_var_decl_statement(bool leading_const);

  StmtPtr parse_register_decl();

  ExprPtr parse_expression();

  ExprPtr parse_assignment();

  ExprPtr parse_binary_rhs(int expr_prec, ExprPtr lhs);

  ExprPtr parse_unary();

  ExprPtr parse_primary();

  ExprPtr parse_call_or_postfix(ExprPtr left);

  ExprPtr parse_lambda();

  ExprPtr parse_block_literal();

  ExprPtr parse_array_literal();

  TypePtr parse_type();

  TypePtr parse_generic_type_arg();

  std::string cur_lexeme() const;

  bool is_symbol(const std::string &s) const;
};
