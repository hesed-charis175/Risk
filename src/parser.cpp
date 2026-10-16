#include "parser.h"
#include <cassert>
#include <functional>
#include <iostream>
#include <unordered_map>

static std::unordered_map<std::string, int> const_int_map;

static std::optional<int> evaluate_const_expr(Expr *e) {
  if (!e)
    return std::nullopt;

  if (auto num = dynamic_cast<NumberExpr *>(e)) {
    try {
      return std::stoi(num->literal);
    } catch (...) {
      return std::nullopt;
    }
  }

  if (auto unary = dynamic_cast<UnaryExpr *>(e)) {
    auto rhs_val = evaluate_const_expr(unary->rhs.get());
    if (!rhs_val)
      return std::nullopt;

    if (unary->op == "-")
      return -*rhs_val;
    if (unary->op == "+")
      return *rhs_val;
    if (unary->op == "~")
      return ~(*rhs_val);
    if (unary->op == "!")
      return !(*rhs_val);
    return std::nullopt;
  }

  if (auto binop = dynamic_cast<BinaryExpr *>(e)) {
    auto lhs_val = evaluate_const_expr(binop->lhs.get());
    auto rhs_val = evaluate_const_expr(binop->rhs.get());
    if (!lhs_val || !rhs_val)
      return std::nullopt;

    if (binop->op == "+")
      return *lhs_val + *rhs_val;
    if (binop->op == "-")
      return *lhs_val - *rhs_val;
    if (binop->op == "*")
      return *lhs_val * *rhs_val;
    if (binop->op == "/") {
      if (*rhs_val == 0)
        return std::nullopt;
      return *lhs_val / *rhs_val;
    }
    if (binop->op == "%") {
      if (*rhs_val == 0)
        return std::nullopt;
      return *lhs_val % *rhs_val;
    }
    if (binop->op == "<<")
      return *lhs_val << *rhs_val;
    if (binop->op == ">>")
      return *lhs_val >> *rhs_val;
    if (binop->op == "&")
      return *lhs_val & *rhs_val;
    if (binop->op == "|")
      return *lhs_val | *rhs_val;
    if (binop->op == "^")
      return *lhs_val ^ *rhs_val;
    return std::nullopt;
  }

  if (auto id = dynamic_cast<IdentifierExpr *>(e)) {
    auto it = const_int_map.find(id->name);
    if (it != const_int_map.end())
      return it->second;
    return std::nullopt;
  }

  return std::nullopt;
}

bool Parser::expr_only_references_value_generics(Expr *e) const {
  if (!e)
    return false;
  if (dynamic_cast<NumberExpr *>(e))
    return true;
  if (auto id = dynamic_cast<IdentifierExpr *>(e))
    return value_generic_scope.count(id->name) > 0 ||
           const_int_map.count(id->name) > 0;
  if (auto un = dynamic_cast<UnaryExpr *>(e)) {
    if (un->op != "-" && un->op != "+" && un->op != "~" && un->op != "!")
      return false;
    return expr_only_references_value_generics(un->rhs.get());
  }
  if (auto bin = dynamic_cast<BinaryExpr *>(e)) {
    static const std::unordered_set<std::string> ops = {
        "+", "-", "*", "/", "%", "<<", ">>", "&", "|", "^"};
    if (!ops.count(bin->op))
      return false;
    return expr_only_references_value_generics(bin->lhs.get()) &&
           expr_only_references_value_generics(bin->rhs.get());
  }
  return false;
}

static int get_binop_prec(const std::string &op) {
  if (op == "++" || op == "--")
    return -1;

  if (op == "*" || op == "/" || op == "%")
    return 40;
  if (op == "+" || op == "-")
    return 30;
  if (op == "<<" || op == ">>")
    return 25;
  if (op == "<" || op == "<=" || op == ">" || op == ">=")
    return 20;
  if (op == "==" || op == "!=")
    return 15;
  if (op == "&")
    return 12;
  if (op == "^")
    return 11;
  if (op == "|")
    return 10;
  if (op == "&&")
    return 5;
  if (op == "||")
    return 4;
  return -1;
}

static std::vector<Token>
preprocess_generic_tokens(const std::vector<Token> &tokens) {
  std::vector<Token> result;
  int angle_depth = 0;

  for (size_t i = 0; i < tokens.size(); ++i) {
    const Token &tok = tokens[i];

    if (tok.type == TokenType::Symbol && tok.lexeme == "<") {
      angle_depth++;
      result.push_back(tok);
    } else if (tok.type == TokenType::Symbol && tok.lexeme == ">") {
      angle_depth = std::max(0, angle_depth - 1);
      result.push_back(tok);
    } else if (tok.type == TokenType::Symbol && tok.lexeme == ">>" &&
               angle_depth >= 2) {

      Token gt1 = tok;
      gt1.lexeme = ">";
      Token gt2 = tok;
      gt2.lexeme = ">";
      gt2.col = tok.col + 1;
      result.push_back(gt1);
      result.push_back(gt2);
      angle_depth = std::max(0, angle_depth - 2);
    } else {
      result.push_back(tok);
    }
  }

  return result;
}
Parser::Parser(const std::vector<Token> &tokens)
    : toks(preprocess_generic_tokens(tokens)), idx(0) {}

const Token &Parser::peek() const {
  assert(!toks.empty());
  if (idx >= toks.size())
    return toks.back();
  return toks[idx];
}
const Token &Parser::peek_n(size_t n) const {
  size_t pos = idx + n;
  if (pos >= toks.size())
    return toks.back();
  return toks[pos];
}
const Token &Parser::advance() {
  if (!is_at_end())
    ++idx;
  return toks[std::min(idx, toks.size() - 1)];
}
bool Parser::is_at_end() const { return peek().type == TokenType::EndOfFile; }
bool Parser::match_symbol(const std::string &s) {
  if (is_at_end())
    return false;
  if (peek().type == TokenType::Symbol && peek().lexeme == s) {
    advance();
    return true;
  }
  return false;
}

bool Parser::peek_is_case_keyword() const {
  return (peek().type == TokenType::Identifier &&
          (peek().lexeme == "case" || peek().lexeme == "default"));
}
bool Parser::match_keyword_header_file() const {
  return peek().type == TokenType::HeaderFileMarker;
}
void Parser::consume_if_symbol(const std::string &s) {
  if (match_symbol(s))
    return;
}
void Parser::expect_symbol(const std::string &s, const std::string &err) {
  if (!match_symbol(s)) {
    Parser::diags.push_back({Diagnostic::Severity::Error,
                             "expected symbol '" + s + "'", peek().line,
                             peek().col});
  }
}

std::string Parser::cur_lexeme() const { return peek().lexeme; }
bool Parser::is_symbol(const std::string &s) const {
  return peek().type == TokenType::Symbol && peek().lexeme == s;
}

static int token_end_col(const Token &t) {
  if (t.lexeme.empty())
    return t.col;
  return t.col + (int)t.lexeme.size() - 1;
}

std::unique_ptr<TranslationUnit> Parser::parse_translation_unit() {
  auto tu = std::make_unique<TranslationUnit>();
  while (!is_at_end()) {
    if (peek().type == TokenType::Comment) {
      advance();
      continue;
    }
    if (peek().type == TokenType::EndOfFile)
      break;

    while (!pending_global_decls.empty()) {
      tu->decls.push_back(std::move(pending_global_decls.front()));
      pending_global_decls.erase(pending_global_decls.begin());
    }
    DeclPtr d = parse_decl();
    if (d)
      tu->decls.push_back(std::move(d));
    else {
      advance();
    }
  }

  while (!pending_global_decls.empty()) {
    tu->decls.push_back(std::move(pending_global_decls.front()));
    pending_global_decls.erase(pending_global_decls.begin());
  }
  return tu;
}
DeclPtr Parser::parse_typealias() {
  Token alias_tok = peek();
  advance();
  if (peek().type != TokenType::Identifier) {
    Parser::diags.push_back({Diagnostic::Severity::Error,
                             "expected alias name after 'typealias",
                             peek().line, peek().col});

    return nullptr;
  }

  std::string alias_name = peek().lexeme;
  advance();

  expect_symbol("=", "type alias assignment");

  TypePtr target = parse_type();

  if (is_symbol(";"))
    advance();

  auto ta = std::make_unique<TypeAliasDecl>(alias_name, std::move(target));
  ta->line = alias_tok.line;
  ta->col = alias_tok.col;
  return ta;
}

DeclPtr Parser::parse_generic_set_decl() {
  Token generic_tok = peek();
  advance();

  if (peek().type != TokenType::Identifier) {
    Parser::diags.push_back({Diagnostic::Severity::Error,
                             "expected name after 'generic'", peek().line,
                             peek().col});
    return nullptr;
  }
  std::string set_name = peek().lexeme;
  advance();

  expect_symbol("=", "generic set assignment");
  expect_symbol("[", "generic set list start");

  auto gd = std::make_unique<GenericSetDecl>(set_name);
  while (!is_symbol("]") && !is_at_end()) {
    TypePtr member = parse_type();
    gd->members.push_back(std::move(member));
    if (is_symbol(","))
      advance();
    else
      break;
  }
  expect_symbol("]", "generic set list end");
  if (is_symbol(";"))
    advance();

  gd->line = generic_tok.line;
  gd->col = generic_tok.col;
  return gd;
}

DeclPtr Parser::parse_decl() {

  if (peek().type == TokenType::Identifier && peek().lexeme == "typealias") {
    return parse_typealias();
  }
  if (peek().type == TokenType::Identifier && peek().lexeme == "generic") {
    return parse_generic_set_decl();
  }
  if (peek().type == TokenType::Identifier && peek().lexeme == "import") {
    return parse_import();
  }
  if (peek().type == TokenType::Identifier && peek().lexeme == "extern") {
    return parse_extern_block();
  }
  if (peek().type == TokenType::Identifier && peek().lexeme == "block") {
    return parse_block_decl();
  }
  if (peek().type == TokenType::Identifier && peek().lexeme == "opts") {
    return parse_opt_decl();
  }
  if (peek().type == TokenType::Identifier && peek().lexeme == "const") {

    Parser::diags.push_back(
        {Diagnostic::Severity::Error,
         "'const' must appear in the type position. "
         "Use 'name: const T = value;' for a const-value global.",
         peek().line, peek().col});
    advance();
    if (peek().type == TokenType::Identifier && peek_n(1).lexeme == ":") {
      auto var_stmt = parse_var_decl_statement(false);
      if (var_stmt)
        return std::make_unique<GlobalVarDecl>(std::unique_ptr<VarDeclStmt>(
            static_cast<VarDeclStmt *>(var_stmt.release())));
    }
    return nullptr;
  }
  if (peek().type == TokenType::Identifier && peek().lexeme == "func") {
    return parse_func_decl_or_def();
  }

  if (peek().type == TokenType::Identifier &&
      peek_n(1).type == TokenType::Symbol && peek_n(1).lexeme == ",") {
    size_t lookahead = 1;
    bool is_multi_decl = false;
    while (true) {
      if (!(peek_n(lookahead).type == TokenType::Symbol &&
            peek_n(lookahead).lexeme == ","))
        break;
      lookahead++;
      if (peek_n(lookahead).type != TokenType::Identifier)
        break;
      lookahead++;
      if (peek_n(lookahead).type == TokenType::Symbol &&
          peek_n(lookahead).lexeme == ":") {
        is_multi_decl = true;
        break;
      }
    }
    if (is_multi_decl) {
      auto blk_stmt = parse_multi_var_decl_statement();
      if (auto blk = dynamic_cast<BlockStmt *>(blk_stmt.get())) {

        DeclPtr first;
        for (auto &s : blk->stmts) {
          auto gv =
              std::make_unique<GlobalVarDecl>(std::unique_ptr<VarDeclStmt>(
                  static_cast<VarDeclStmt *>(s.release())));
          if (!first)
            first = std::move(gv);
          else
            pending_global_decls.push_back(std::move(gv));
        }
        return first;
      }
      return nullptr;
    }
  }
  if (peek().type == TokenType::Identifier && peek_n(1).lexeme == ":") {
    auto var_stmt = parse_var_decl_statement(false);
    return std::make_unique<GlobalVarDecl>(std::unique_ptr<VarDeclStmt>(
        static_cast<VarDeclStmt *>(var_stmt.release())));
  }
  StmtPtr s = parse_statement();
  if (s) {
    Parser::diags.push_back({Diagnostic::Severity::Error,
                             "Ignored Unsupported Statements", peek().line,
                             peek().col});
  }
  return nullptr;
}

DeclPtr Parser::parse_import() {
  advance();

  std::string path;
  std::string alias;

  if (is_symbol("@")) {
    advance();
    if (peek().type != TokenType::Identifier) {
      Parser::diags.push_back({Diagnostic::Severity::Error,
                               "expected module name after @", peek().line,
                               peek().col});
      return nullptr;
    }

    std::string pkg_path = peek().lexeme;
    std::string last_segment = peek().lexeme;
    advance();
    while (is_symbol("/") && peek_n(1).type == TokenType::Identifier) {
      advance();
      last_segment = peek().lexeme;
      pkg_path += "/" + last_segment;
      advance();
    }

    if (is_symbol(".") && peek_n(1).type == TokenType::Identifier &&
        peek_n(1).lexeme == "risk") {
      advance();
      advance();
    }
    path = "\"@" + pkg_path + ".risk\"";
    alias = last_segment;
  } else if (peek().type == TokenType::String) {

    path = peek().lexeme;
    advance();
  } else {

    Parser::diags.push_back({Diagnostic::Severity::Error,
                             "expected string literal or @module after import",
                             peek().line, peek().col});
    return nullptr;
  }

  if (peek().type == TokenType::Identifier && peek().lexeme == "as") {
    advance();
    if (peek().type != TokenType::Identifier) {

      Parser::diags.push_back({Diagnostic::Severity::Error,
                               "expected identifier after 'as'", peek().line,
                               peek().col});
      return nullptr;
    }
    alias = peek().lexeme;
    advance();
  }

  if (is_symbol(";"))
    advance();

  auto decl = std::make_unique<ImportDecl>(path);
  decl->alias = alias;
  return decl;
}

DeclPtr Parser::parse_extern_block() {
  Token extern_tok = peek();
  advance();

  if (peek().type != TokenType::String) {
    Parser::diags.push_back({Diagnostic::Severity::Error,
                             "expected a string literal header path after "
                             "'extern'",
                             peek().line, peek().col});
    return nullptr;
  }
  std::string header_path = peek().lexeme;
  advance();

  std::vector<std::string> link_libs;
  if (peek().type == TokenType::Identifier && peek().lexeme == "lib") {
    advance();
    for (;;) {
      if (peek().type != TokenType::String) {
        Parser::diags.push_back(
            {Diagnostic::Severity::Error,
             "expected a string literal library name after 'lib'", peek().line,
             peek().col});
        break;
      }
      std::string lib_name = peek().lexeme;
      if (!lib_name.empty() && lib_name.front() == '"')
        lib_name.erase(0, 1);
      if (!lib_name.empty() && lib_name.back() == '"')
        lib_name.pop_back();
      link_libs.push_back(lib_name);
      advance();
      if (is_symbol(",")) {
        advance();
        continue;
      }
      break;
    }
  }

  expect_symbol("{", "extern block body");

  std::vector<DeclPtr> parsed;
  while (!is_symbol("}") && !is_at_end()) {
    if (peek().type == TokenType::Comment) {
      advance();
      continue;
    }
    if (peek().type == TokenType::Identifier && peek().lexeme == "block") {
      Token block_tok = peek();
      advance();
      if (peek().type != TokenType::Identifier) {
        Parser::diags.push_back({Diagnostic::Severity::Error,
                                 "expected block name after 'block'",
                                 peek().line, peek().col});
        continue;
      }
      std::string bname = peek().lexeme;
      Token bname_tok = peek();
      advance();
      auto bdecl = std::make_unique<BlockDecl>(bname);
      bdecl->is_extern = true;
      bdecl->extern_header = header_path;
      bdecl->line = bname_tok.line;
      bdecl->col = bname_tok.col;

      if (peek().type == TokenType::String) {

        std::string raw = peek().lexeme;
        if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"')
          raw = raw.substr(1, raw.size() - 2);
        bdecl->extern_c_spelling = raw;
        advance();
      }

      if (is_symbol(";")) {

        advance();
      } else {
        expect_symbol("{",
                      "block body start (or ';' for an opaque extern block)");
        parse_block_body(bdecl.get());
        if (is_symbol(";"))
          advance();
      }
      (void)block_tok;
      parsed.push_back(std::move(bdecl));
      continue;
    }

    if (peek().type == TokenType::Identifier && peek().lexeme == "opts") {

      DeclPtr d = parse_opt_decl();
      auto od = dynamic_cast<OptDecl *>(d.get());
      if (od) {
        od->is_extern = true;
        od->extern_header = header_path;
        parsed.push_back(std::move(d));
      }
      continue;
    }

    if (peek().type == TokenType::Identifier &&
        peek_n(1).type == TokenType::Symbol && peek_n(1).lexeme == ":") {
      std::string vname = peek().lexeme;
      Token vname_tok = peek();
      advance();
      advance();
      TypePtr vtype = parse_type();
      if (is_symbol("=")) {
        Parser::diags.push_back(
            {Diagnostic::Severity::Error,
             "an extern variable can't have an initializer -- it already "
             "has one wherever the real C global is defined",
             peek().line, peek().col});
        advance();
        parse_expression();
      }
      expect_symbol(";", "expected ';' after extern variable declaration");
      auto vdecl =
          std::make_unique<GlobalVarDecl>(std::make_unique<VarDeclStmt>(
              vname, std::move(vtype), nullptr, false));
      vdecl->is_extern = true;
      vdecl->extern_header = header_path;
      vdecl->line = vname_tok.line;
      vdecl->col = vname_tok.col;
      vdecl->var->line = vname_tok.line;
      vdecl->var->col = vname_tok.col;
      parsed.push_back(std::move(vdecl));
      continue;
    }

    if (!(peek().type == TokenType::Identifier && peek().lexeme == "func")) {
      Parser::diags.push_back(
          {Diagnostic::Severity::Error,
           "only 'func' signatures, 'block'/'opts' declarations, and bare "
           "'name: Type;' variable bindings are allowed inside an "
           "'extern' block",
           peek().line, peek().col});
      advance();
      continue;
    }
    DeclPtr d = parse_func_decl_or_def();
    auto fd = dynamic_cast<FuncDecl *>(d.get());
    if (fd) {
      if (fd->is_definition) {
        Parser::diags.push_back(
            {Diagnostic::Severity::Error,
             "function '" + fd->name +
                 "' inside an 'extern' block cannot have a body -- it must "
                 "be a signature only, terminated with ';'",
             fd->line, fd->col});
      }
      fd->is_extern = true;
      fd->is_definition = false;
      fd->extern_header = header_path;
      fd->extern_link_libs = link_libs;
      parsed.push_back(std::move(d));
    }
  }
  expect_symbol("}", "closing '}' for extern block");

  if (parsed.empty()) {
    Parser::diags.push_back({Diagnostic::Severity::Error,
                             "'extern' block declares nothing", extern_tok.line,
                             extern_tok.col});
    return nullptr;
  }

  DeclPtr first = std::move(parsed.front());
  for (size_t i = 1; i < parsed.size(); ++i)
    pending_global_decls.push_back(std::move(parsed[i]));
  return first;
}

DeclPtr Parser::parse_block_decl() {
  advance();
  if (peek().type != TokenType::Identifier) {

    Parser::diags.push_back({Diagnostic::Severity::Error,
                             "expected block name after 'block'", peek().line,
                             peek().col});
    return nullptr;
  }
  std::string bname = peek().lexeme;
  Token bname_tok = peek();
  advance();
  auto bdecl = std::make_unique<BlockDecl>(bname);

  parse_generic_param_list(bdecl->generic_params);

  std::unordered_set<std::string> saved_value_generic_scope =
      value_generic_scope;
  struct ScopeGuard {
    Parser *p;
    std::unordered_set<std::string> saved;
    ~ScopeGuard() { p->value_generic_scope = std::move(saved); }
  } scope_guard{this, std::move(saved_value_generic_scope)};
  for (auto &gp : bdecl->generic_params)
    if (gp.is_value)
      value_generic_scope.insert(gp.name);

  expect_symbol("{", "block body start");
  parse_block_body(bdecl.get());
  bdecl->line = bname_tok.line;
  bdecl->col = bname_tok.col;
  return bdecl;
}

void Parser::parse_block_body(BlockDecl *bdecl) {
  while (!is_symbol("}") && !is_at_end()) {
    if (peek().type == TokenType::Comment) {
      advance();
      continue;
    }
    if (peek().type != TokenType::Identifier) {

      Parser::diags.push_back({Diagnostic::Severity::Error,
                               "expected field name in block", peek().line,
                               peek().col});
      advance();
      continue;
    }

    std::vector<Token> fnames;
    fnames.push_back(peek());
    advance();
    while (is_symbol(",")) {
      advance();
      if (peek().type != TokenType::Identifier) {
        Parser::diags.push_back({Diagnostic::Severity::Error,
                                 "expected field name after ','", peek().line,
                                 peek().col});
        break;
      }
      fnames.push_back(peek());
      advance();
    }
    expect_symbol(":", "field type separator");
    TypePtr ftype = parse_type();

    if (ftype && ftype->name == "func" && !ftype->params.empty() &&
        ftype->params[0]->name == "this") {
      ftype->params[0] = std::make_unique<TypeNode>(bdecl->name);
      ftype->params[0]->is_this_param = true;
    }

    ExprPtr default_val;
    if (is_symbol("=")) {
      if (fnames.size() > 1) {
        Parser::diags.push_back(
            {Diagnostic::Severity::Warning,
             "default value on multi-name field applies to all names",
             peek().line, peek().col});
      }
      advance();
      default_val = parse_expression();
    }
    if (is_symbol(";"))
      advance();

    for (size_t fi = 0; fi < fnames.size(); fi++) {

      TypePtr field_type;
      if (fi == 0) {
        field_type = std::move(ftype);
      } else {

        std::function<TypePtr(const TypeNode *)> clone_type;
        clone_type = [&](const TypeNode *src) -> TypePtr {
          auto c = std::make_unique<TypeNode>(src->name);
          c->is_this_param = src->is_this_param;
          for (const auto &p : src->params)
            c->params.push_back(clone_type(p.get()));
          if (src->return_type)
            c->return_type = clone_type(src->return_type.get());
          return c;
        };
        field_type = clone_type(bdecl->fields.back().type.get());
      }
      BlockField bf(fnames[fi].lexeme, std::move(field_type));
      if (default_val && fi == 0) {
        bf.default_val = std::move(default_val);
      }
      bdecl->fields.push_back(std::move(bf));
    }
  }
  expect_symbol("}", "block body end");
}

DeclPtr Parser::parse_opt_decl() {
  advance();
  if (peek().type != TokenType::Identifier) {
    Parser::diags.push_back({Diagnostic::Severity::Error,
                             "expected option name after 'opts'", peek().line,
                             peek().col});
    return nullptr;
  }
  std::string optname = peek().lexeme;
  Token optname_tok = peek();
  advance();

  auto optdecl = std::make_unique<OptDecl>(optname);
  expect_symbol("{", "opts body start");

  int next_auto_value = 0;

  while (!is_symbol("}") && !is_at_end()) {
    if (peek().type == TokenType::Comment) {
      advance();
      continue;
    }
    if (peek().type != TokenType::Identifier) {
      Parser::diags.push_back({Diagnostic::Severity::Error,
                               "expected option name in opts", peek().line,
                               peek().col});
      advance();
      continue;
    }

    std::string fname = peek().lexeme;
    Token fname_tok = peek();
    advance();

    std::optional<int> explicit_value;

    if (is_symbol("=")) {
      advance();

      ExprPtr value_expr = parse_expression();

      if (!value_expr) {
        Parser::diags.push_back({Diagnostic::Severity::Error,
                                 "expected value expression after '='",
                                 peek().line, peek().col});
      } else {

        auto result = evaluate_const_expr(value_expr.get());
        if (result) {
          explicit_value = *result;
          next_auto_value = *explicit_value + 1;
        } else {
          Parser::diags.push_back({Diagnostic::Severity::Error,
                                   "opts values must be constant expressions",
                                   peek().line, peek().col});
        }
      }
    } else {

      explicit_value = next_auto_value++;
    }

    optdecl->fields.emplace_back(fname, explicit_value);

    if (is_symbol(","))
      advance();
  }
  expect_symbol("}", "opts body end");
  optdecl->line = optname_tok.line;
  optdecl->col = optname_tok.col;
  return optdecl;
}

void Parser::parse_generic_param_list(std::vector<GenericParam> &out) {
  if (!is_symbol("<"))
    return;
  advance();
  while (!is_symbol(">") && !is_at_end()) {
    bool is_value = false;
    if (peek().type == TokenType::Identifier && peek().lexeme == "const") {
      is_value = true;
      advance();
    }
    if (peek().type != TokenType::Identifier) {
      Parser::diags.push_back({Diagnostic::Severity::Error,
                               "expected generic parameter name", peek().line,
                               peek().col});
      break;
    }
    std::string pname = peek().lexeme;
    advance();
    expect_symbol(":", "generic parameter constraint");
    if (peek().type != TokenType::Identifier) {
      Parser::diags.push_back({Diagnostic::Severity::Error,
                               "expected generic set name after ':'",
                               peek().line, peek().col});
      break;
    }
    std::string set_name = peek().lexeme;
    Token set_name_tok = peek();
    advance();
    static const std::unordered_set<std::string> integer_types = {
        "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64"};

    if (!is_value && integer_types.count(set_name))
      is_value = true;
    if (is_value) {
      if (!integer_types.count(set_name)) {
        Parser::diags.push_back(
            {Diagnostic::Severity::Error,
             "value generic parameter '" + pname +
                 "' must have an integer type (i8/i16/i32/i64/u8/u16/u32/"
                 "u64), got '" +
                 set_name + "'",
             set_name_tok.line, set_name_tok.col});
      }
    }
    GenericParam gp;
    gp.name = pname;
    gp.constraint_set = set_name;
    gp.is_value = is_value;

    gp.kind = is_value ? GenericParamKind::ValueInt : GenericParamKind::Type;
    out.push_back(std::move(gp));
    if (is_symbol(","))
      advance();
    else
      break;
  }
  expect_symbol(">", "end generic parameter list");
}

DeclPtr Parser::parse_func_decl_or_def() {
  advance();
  if (peek().type != TokenType::Identifier) {

    Parser::diags.push_back({Diagnostic::Severity::Error,
                             "expected function name after 'func'", peek().line,
                             peek().col});

    return nullptr;
  }
  std::string fname = peek().lexeme;
  Token fname_tok = peek();
  advance();
  auto fdecl = std::make_unique<FuncDecl>(fname);
  fdecl->line = fname_tok.line;
  fdecl->col = fname_tok.col;
  parse_generic_param_list(fdecl->generic_params);

  std::unordered_set<std::string> saved_value_generic_scope =
      value_generic_scope;
  struct ScopeGuard {
    Parser *p;
    std::unordered_set<std::string> saved;
    ~ScopeGuard() { p->value_generic_scope = std::move(saved); }
  } scope_guard{this, std::move(saved_value_generic_scope)};
  for (auto &gp : fdecl->generic_params)
    if (gp.is_value)
      value_generic_scope.insert(gp.name);

  expect_symbol("(", "function parameters");
  while (!is_symbol(")")) {
    if (peek().type == TokenType::Identifier && peek().lexeme == "_") {
      advance();
      if (is_symbol(":"))
        advance();

      bool is_noalias = false;
      if (peek().type == TokenType::Identifier && peek().lexeme == "noalias") {
        is_noalias = true;
        advance();
      }
      TypePtr t = parse_type();
      FuncParam fp_underscore;
      fp_underscore.name = "_";
      fp_underscore.type = std::move(t);
      fp_underscore.noalias = is_noalias;
      fdecl->params.push_back(std::move(fp_underscore));
    } else if (peek().type == TokenType::Identifier) {
      std::string pname = peek().lexeme;
      advance();
      if (!is_symbol(":")) {

        Parser::diags.push_back({Diagnostic::Severity::Error,
                                 "expected ':' after parameter name",
                                 peek().line, peek().col});
      } else
        advance();

      bool is_noalias = false;
      if (peek().type == TokenType::Identifier && peek().lexeme == "noalias") {
        is_noalias = true;
        advance();
      }
      TypePtr ptype = parse_type();
      FuncParam fp;
      fp.name = std::move(pname);
      fp.type = std::move(ptype);
      fp.noalias = is_noalias;
      if (is_symbol("=")) {
        advance();
        fp.default_val = parse_expression();
      }
      fdecl->params.push_back(std::move(fp));
    } else {
      break;
    }
    if (is_symbol(","))
      advance();
  }
  expect_symbol(")", "closing parameter list");

  if (peek().type == TokenType::Symbol && peek().lexeme == "->") {
    advance();
    TypePtr rt = parse_type();
    fdecl->return_type = std::move(rt);
  }

  if (is_symbol(";")) {
    advance();
    fdecl->is_definition = false;
    return fdecl;
  }
  if (is_symbol("{")) {
    fdecl->is_definition = true;
    auto body = parse_block();
    if (auto blk = dynamic_cast<BlockStmt *>(body.get())) {
      for (auto &s : blk->stmts)
        fdecl->body.push_back(std::move(s));
    }
    return fdecl;
  }
  Parser::diags.push_back({Diagnostic::Severity::Error,
                           "unexpected token after function signature",
                           peek().line, peek().col});

  return fdecl;
}

StmtPtr Parser::parse_statement() {
  if (peek().type == TokenType::Comment) {
    advance();
    return nullptr;
  }

  if (is_symbol("{"))
    return parse_block();
  if (peek().type == TokenType::Identifier && peek().lexeme == "if")
    return parse_if();
  if (peek().type == TokenType::Identifier && peek().lexeme == "while")
    return parse_while();
  if (peek().type == TokenType::Identifier && peek().lexeme == "for")
    return parse_for();
  if (peek().type == TokenType::Identifier && peek().lexeme == "return")
    return parse_return();
  if (peek().type == TokenType::Identifier && peek().lexeme == "switch")
    return parse_switch();
  if (peek().type == TokenType::Identifier && peek().lexeme == "defer")
    return parse_defer();
  if (peek().type == TokenType::Identifier && peek().lexeme == "const") {
    Parser::diags.push_back(
        {Diagnostic::Severity::Error,
         "'const' must appear in the type position, not before the variable "
         "name. Use: name: const T = value;",
         peek().line, peek().col});

    advance();
    if (peek().type == TokenType::Identifier &&
        peek_n(1).type == TokenType::Symbol && peek_n(1).lexeme == ":") {
      return parse_var_decl_statement(false);
    }
    return nullptr;
  }

  if (peek().type == TokenType::Identifier &&
      peek_n(1).type == TokenType::Symbol && peek_n(1).lexeme == ",") {

    size_t lookahead = 1;
    bool is_multi_decl = false;
    while (true) {

      if (!(peek_n(lookahead).type == TokenType::Symbol &&
            peek_n(lookahead).lexeme == ","))
        break;
      lookahead++;
      if (peek_n(lookahead).type != TokenType::Identifier)
        break;
      lookahead++;
      if (peek_n(lookahead).type == TokenType::Symbol &&
          peek_n(lookahead).lexeme == ":") {
        is_multi_decl = true;
        break;
      }
    }
    if (is_multi_decl)
      return parse_multi_var_decl_statement();
  }
  if (peek().type == TokenType::Identifier &&
      peek_n(1).type == TokenType::Symbol && peek_n(1).lexeme == ":") {
    return parse_var_decl_statement(false);
  }

  if (peek().type == TokenType::Identifier && peek().lexeme == "break") {
    Token tok = peek();
    advance();
    if (is_symbol(";"))
      advance();
    auto bs = std::make_unique<BreakStmt>();
    bs->line = tok.line;
    bs->col = tok.col;
    return bs;
  }

  if (peek().type == TokenType::Identifier && peek().lexeme == "continue") {
    Token tok = peek();
    advance();
    if (is_symbol(";"))
      advance();

    auto cs = std::make_unique<ContinueStmt>();
    cs->line = tok.line;
    cs->col = tok.col;
    return cs;
  }
  Token start_tok = peek();
  ExprPtr e = parse_expression();
  if (is_symbol(";"))
    advance();
  auto es = std::make_unique<ExprStmt>(std::move(e));
  es->line = start_tok.line;
  es->col = start_tok.col;
  return es;
}

StmtPtr Parser::parse_block() {
  expect_symbol("{", "block start");
  auto block = std::make_unique<BlockStmt>();
  block->line = peek().line;
  while (!is_symbol("}") && !is_at_end()) {
    if (peek().type == TokenType::Comment) {
      advance();
      continue;
    }
    StmtPtr s = parse_statement();
    if (s)
      block->stmts.push_back(std::move(s));
    else
      advance();
  }
  expect_symbol("}", "block end");
  return block;
}

StmtPtr Parser::parse_if() {
  Token if_tok = peek();
  advance();
  expect_symbol("(", "if condition");
  ExprPtr cond = parse_expression();
  expect_symbol(")", "if condition end");
  StmtPtr then_branch = parse_statement();
  std::optional<StmtPtr> else_branch;
  if (peek().type == TokenType::Identifier && peek().lexeme == "else") {
    advance();
    else_branch = parse_statement();
  }
  auto ifs = std::make_unique<IfStmt>(std::move(cond), std::move(then_branch));
  ifs->line = if_tok.line;
  ifs->col = if_tok.col;
  if (else_branch)
    ifs->else_branch = std::move(else_branch);
  return ifs;
}
StmtPtr Parser::parse_switch() {
  Token switch_tok = peek();
  advance();

  expect_symbol("(", "switch condition");
  ExprPtr expr = parse_expression();
  expect_symbol(")", "switch condition end");

  expect_symbol("{", "switch body");

  auto sw = std::make_unique<SwitchStmt>();
  sw->expr = std::move(expr);
  sw->line = switch_tok.line;
  sw->col = switch_tok.col;

  while (!is_symbol("}") && !is_at_end()) {
    if (peek().type == TokenType::Comment) {
      advance();
      continue;
    }

    if (peek().type == TokenType::Identifier && peek().lexeme == "case") {
      advance();
      ExprPtr val = parse_expression();

      if (is_symbol(":")) {
        advance();
      }

      SwitchCase c;
      c.value = std::move(val);

      while (!peek_is_case_keyword() && !is_symbol("}") && !is_at_end()) {
        if (peek().type == TokenType::Comment) {
          advance();
          continue;
        }
        StmtPtr s = parse_statement();
        if (s) {
          c.stmts.push_back(std::move(s));
        }
      }
      sw->cases.push_back(std::move(c));
    } else if (peek().type == TokenType::Identifier &&
               peek().lexeme == "default") {
      advance();

      if (is_symbol(":")) {
        advance();
      }

      SwitchCase c;
      c.value = std::nullopt;

      while (!peek_is_case_keyword() && !is_symbol("}") && !is_at_end()) {
        if (peek().type == TokenType::Comment) {
          advance();
          continue;
        }
        StmtPtr s = parse_statement();
        if (s) {
          c.stmts.push_back(std::move(s));
        }
      }
      sw->cases.push_back(std::move(c));
    } else {
      Parser::diags.push_back({Diagnostic::Severity::Error,
                               "unexpected token in switch", peek().line,
                               peek().col});

      advance();
    }
  }

  expect_symbol("}", "switch body end");
  return sw;
}
StmtPtr Parser::parse_while() {
  Token while_tok = peek();
  advance();
  expect_symbol("(", "while cond");
  ExprPtr cond = parse_expression();
  expect_symbol(")", "while cond end");
  StmtPtr body = parse_statement();
  auto ws = std::make_unique<WhileStmt>(std::move(cond), std::move(body));
  ws->line = while_tok.line;
  ws->col = while_tok.col;
  return ws;
}

StmtPtr Parser::parse_for() {
  Token for_tok = peek();
  advance();
  expect_symbol("(", "for header");
  std::optional<StmtPtr> init;
  if (!is_symbol(";")) {
    if (peek().type == TokenType::Identifier &&
        peek_n(1).type == TokenType::Symbol && peek_n(1).lexeme == ":") {
      init = parse_var_decl_statement(false);
    } else {
      ExprPtr e = parse_expression();
      if (is_symbol(";"))
        advance();
      init = std::make_unique<ExprStmt>(std::move(e));
    }
  } else {
    advance();
  }
  std::optional<ExprPtr> cond;
  if (!is_symbol(";")) {
    ExprPtr c = parse_expression();
    cond = std::move(c);
  }
  expect_symbol(";", "for separator");
  std::optional<StmtPtr> post;
  if (!is_symbol(")")) {
    ExprPtr p = parse_expression();
    post = std::make_unique<ExprStmt>(std::move(p));
  }
  expect_symbol(")", "for end");
  StmtPtr body = parse_statement();
  auto f = std::make_unique<ForStmt>();
  f->line = for_tok.line;
  f->col = for_tok.col;
  f->init = std::move(init);
  f->cond = std::move(cond);
  f->post = std::move(post);
  f->body = std::move(body);
  return f;
}

StmtPtr Parser::parse_return() {
  Token ret_tok = peek();
  advance();
  ExprPtr e;
  if (!is_symbol(";") && peek().type != TokenType::EndOfFile) {
    e = parse_expression();
  }
  if (is_symbol(";"))
    advance();
  auto rs = std::make_unique<ReturnStmt>(std::move(e));
  rs->line = ret_tok.line;
  rs->col = ret_tok.col;
  return rs;
}

StmtPtr Parser::parse_defer() {

  Token defer_tok = peek();
  advance();
  StmtPtr body = parse_statement();
  auto ds = std::make_unique<DeferStmt>(std::move(body));
  ds->line = defer_tok.line;
  ds->col = defer_tok.col;
  return ds;
}

StmtPtr Parser::parse_multi_var_decl_statement() {

  Token first_tok = peek();

  std::vector<Token> names;
  if (peek().type != TokenType::Identifier) {
    Parser::diags.push_back(
        {Diagnostic::Severity::Error,
         "expected identifier in multi-variable declaration", peek().line,
         peek().col});
    return nullptr;
  }
  names.push_back(peek());
  advance();

  while (is_symbol(",")) {
    advance();
    if (peek().type != TokenType::Identifier) {
      Parser::diags.push_back({Diagnostic::Severity::Error,
                               "expected identifier after ',' in declaration",
                               peek().line, peek().col});
      break;
    }
    names.push_back(peek());
    advance();
  }

  expect_symbol(":", "type annotation in multi-variable declaration");

  bool is_const = false;
  if (peek().type == TokenType::Identifier && peek().lexeme == "const") {
    bool next_is_angle =
        (peek_n(1).type == TokenType::Symbol && peek_n(1).lexeme == "<");
    if (!next_is_angle) {
      advance();
      is_const = true;
    }
  }

  TypePtr shared_type = parse_type();

  std::vector<ExprPtr> inits;
  if (is_symbol("=")) {
    advance();

    inits.push_back(parse_expression());

    while (is_symbol(",")) {
      advance();
      inits.push_back(parse_expression());
    }
  }

  if (is_symbol(";"))
    advance();

  size_t n = names.size();
  if (!inits.empty() && inits.size() != 1 && inits.size() != n) {
    Parser::diags.push_back(
        {Diagnostic::Severity::Error,
         "multi-variable declaration has " + std::to_string(n) +
             " variables but " + std::to_string(inits.size()) +
             " initializers (expected 0, 1, or " + std::to_string(n) + ")",
         first_tok.line, first_tok.col});

    inits.clear();
  }

  auto block = std::make_unique<BlockStmt>();
  block->line = first_tok.line;
  block->col = first_tok.col;
  block->is_multi_var_expansion = true;

  for (size_t i = 0; i < n; ++i) {

    std::function<TypePtr(TypeNode *)> clone_type =
        [&](TypeNode *tn) -> TypePtr {
      if (!tn)
        return nullptr;
      auto copy = std::make_unique<TypeNode>(tn->name);
      for (auto &p : tn->params)
        copy->params.push_back(clone_type(p.get()));
      if (tn->return_type)
        copy->return_type = clone_type(tn->return_type.get());
      return copy;
    };

    TypePtr t = clone_type(shared_type.get());

    ExprPtr init;
    if (inits.size() == 1) {

      std::function<ExprPtr(Expr *)> clone_expr = [&](Expr *e) -> ExprPtr {
        if (!e)
          return nullptr;
        if (auto num = dynamic_cast<NumberExpr *>(e)) {
          auto c = std::make_unique<NumberExpr>(num->literal);
          c->line = num->line;
          c->col = num->col;
          c->end_col = num->end_col;
          return c;
        }
        if (auto str = dynamic_cast<StringExpr *>(e)) {
          auto c = std::make_unique<StringExpr>(str->literal);
          c->line = str->line;
          c->col = str->col;
          c->end_col = str->end_col;
          return c;
        }
        if (auto b = dynamic_cast<BoolExpr *>(e)) {
          auto c = std::make_unique<BoolExpr>(b->value);
          c->line = b->line;
          c->col = b->col;
          c->end_col = b->end_col;
          return c;
        }
        if (auto id = dynamic_cast<IdentifierExpr *>(e)) {
          auto c = std::make_unique<IdentifierExpr>(id->name);
          c->line = id->line;
          c->col = id->col;
          c->end_col = id->end_col;
          return c;
        }
        if (auto un = dynamic_cast<UnaryExpr *>(e)) {
          auto c =
              std::make_unique<UnaryExpr>(un->op, clone_expr(un->rhs.get()));
          c->is_postfix = un->is_postfix;
          c->line = un->line;
          c->col = un->col;
          c->end_col = un->end_col;
          return c;
        }
        if (auto bin = dynamic_cast<BinaryExpr *>(e)) {
          auto c = std::make_unique<BinaryExpr>(
              clone_expr(bin->lhs.get()), bin->op, clone_expr(bin->rhs.get()));
          c->line = bin->line;
          c->col = bin->col;
          c->end_col = bin->end_col;
          return c;
        }
        if (auto cast = dynamic_cast<CastExpr *>(e)) {
          std::function<TypePtr(TypeNode *)> clone_type =
              [&](TypeNode *tn) -> TypePtr {
            if (!tn)
              return nullptr;
            auto copy = std::make_unique<TypeNode>(tn->name);
            for (auto &p : tn->params)
              copy->params.push_back(clone_type(p.get()));
            if (tn->return_type)
              copy->return_type = clone_type(tn->return_type.get());
            return copy;
          };
          auto c = std::make_unique<CastExpr>(clone_expr(cast->expr.get()),
                                              clone_type(cast->target.get()));
          c->is_unsafe = cast->is_unsafe;
          c->line = cast->line;
          c->col = cast->col;
          c->end_col = cast->end_col;
          return c;
        }

        auto ref = std::make_unique<IdentifierExpr>("__multi_var_init__");
        ref->line = e->line;
        ref->col = e->col;
        return ref;
      };

      if (i < n - 1) {
        init = clone_expr(inits[0].get());
      } else {
        init = std::move(inits[0]);
      }
    } else if (inits.size() == n) {
      init = std::move(inits[i]);
    }

    if (is_const && init) {
      auto val = evaluate_const_expr(init.get());
      if (val)
        const_int_map[names[i].lexeme] = *val;
    }
    auto vds = std::make_unique<VarDeclStmt>(names[i].lexeme, std::move(t),
                                             std::move(init), is_const);
    vds->line = names[i].line;
    vds->col = names[i].col;
    block->stmts.push_back(std::move(vds));
  }

  return block;
}

StmtPtr Parser::parse_var_decl_statement(bool leading_const) {

  if (leading_const)
    advance();

  if (peek().type != TokenType::Identifier) {
    Parser::diags.push_back({Diagnostic::Severity::Error,
                             "expected identifier in variable declaration",
                             peek().line, peek().col});
    return nullptr;
  }
  Token name_tok = peek();
  std::string name = peek().lexeme;
  advance();
  expect_symbol(":", "type annotation");

  if (peek().type == TokenType::Identifier && peek().lexeme == "register") {
    advance();
    expect_symbol("<", "register type parameter");
    TypePtr elem_type = parse_type();
    expect_symbol(">", "register type parameter");
    expect_symbol("(", "register address");

    if (peek().type != TokenType::Number) {
      Parser::diags.push_back({Diagnostic::Severity::Error,
                               "expected address number", peek().line,
                               peek().col});
      return nullptr;
    }
    Token addr_tok = peek();
    advance();

    uint32_t address;
    try {
      address = std::stoul(addr_tok.lexeme, nullptr, 0);
    } catch (...) {
      Parser::diags.push_back({Diagnostic::Severity::Error,
                               "invalid address: " + addr_tok.lexeme,
                               addr_tok.line, addr_tok.col});
      return nullptr;
    }

    expect_symbol(")", "register address");

    ExprPtr init;
    if (is_symbol("=")) {
      advance();
      init = parse_expression();
    }
    if (is_symbol(";"))
      advance();

    auto rd =
        std::make_unique<RegisterDecl>(name, std::move(elem_type), address);
    rd->line = name_tok.line;
    rd->col = name_tok.col;
    return rd;
  }

  bool is_const = leading_const;

  if (!leading_const && peek().type == TokenType::Identifier &&
      peek().lexeme == "const") {

    bool next_is_angle =
        (peek_n(1).type == TokenType::Symbol && peek_n(1).lexeme == "<");
    if (!next_is_angle) {

      advance();
      is_const = true;
    }
  }

  TypePtr t = parse_type();
  ExprPtr init;
  if (is_symbol("=")) {
    advance();
    init = parse_expression();
  }
  if (is_symbol(";"))
    advance();

  if (is_const && init) {
    auto val = evaluate_const_expr(init.get());
    if (val)
      const_int_map[name] = *val;
  }

  auto vds = std::make_unique<VarDeclStmt>(name, std::move(t), std::move(init),
                                           is_const);
  vds->line = name_tok.line;
  vds->col = name_tok.col;
  return vds;
}

ExprPtr Parser::parse_expression() { return parse_assignment(); }

ExprPtr Parser::parse_assignment() {
  ExprPtr lhs = parse_unary();
  if (peek().type == TokenType::Symbol) {
    std::string op = peek().lexeme;
    static const std::vector<std::string> assigns = {
        "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^="};
    for (auto &a : assigns) {
      if (op == a) {
        Token op_tok = peek();
        advance();
        ExprPtr rhs = parse_assignment();
        auto be =
            std::make_unique<BinaryExpr>(std::move(lhs), op, std::move(rhs));
        be->line = lhs ? lhs->line : op_tok.line;
        be->col = lhs ? lhs->col : op_tok.col;
        be->end_col = be->rhs
                          ? (be->rhs->end_col ? be->rhs->end_col : be->rhs->col)
                          : token_end_col(op_tok);
        return be;
      }
    }
  }
  return parse_binary_rhs(0, std::move(lhs));
}

ExprPtr Parser::parse_binary_rhs(int expr_prec, ExprPtr lhs) {
  while (true) {
    if (peek().type == TokenType::Symbol) {
      if (peek().lexeme == "++" || peek().lexeme == "--") {
        break;
      }
      if ((peek().lexeme == "+" || peek().lexeme == "-") &&
          peek_n(1).type == TokenType::Symbol &&
          peek_n(1).lexeme == peek().lexeme) {
        break;
      }
    }
    bool is_op_token = (peek().type == TokenType::Symbol);
    if (!is_op_token)
      break;
    std::string op = peek().lexeme;
    int prec = get_binop_prec(op);
    if (prec < expr_prec)
      break;

    Token op_tok = peek();

    if (peek().type == TokenType::Symbol) {
      advance();
    } else {
      break;
    }

    ExprPtr rhs = parse_unary();
    if (peek().type == TokenType::Symbol) {
      std::string next_op = peek().lexeme;
      if (next_op != "++" && next_op != "--") {
        int next_prec = get_binop_prec(next_op);
        if (prec < next_prec) {
          rhs = parse_binary_rhs(prec + 1, std::move(rhs));
        }
      }
    }
    auto be = std::make_unique<BinaryExpr>(std::move(lhs), op, std::move(rhs));
    be->line = be->lhs ? be->lhs->line : op_tok.line;
    be->col = be->lhs ? be->lhs->col : op_tok.col;
    be->end_col = be->rhs ? (be->rhs->end_col ? be->rhs->end_col : be->rhs->col)
                          : token_end_col(op_tok);
    lhs = std::move(be);
  }
  return lhs;
}

ExprPtr Parser::parse_unary() {
  if (peek().type == TokenType::Symbol) {
    std::string op = peek().lexeme;
    if (op == "!" || op == "~" || op == "-" || op == "+" || op == "++" ||
        op == "--" || op == "&" || op == "*") {
      Token op_tok = peek();
      advance();
      ExprPtr rhs = parse_unary();
      auto u = std::make_unique<UnaryExpr>(op, std::move(rhs));
      u->is_postfix = false;
      u->line = op_tok.line;
      u->col = op_tok.col;
      u->end_col = u->rhs ? (u->rhs->end_col ? u->rhs->end_col : u->rhs->col)
                          : token_end_col(op_tok);
      return u;
    }

    if (op == "+" && peek_n(1).type == TokenType::Symbol &&
        peek_n(1).lexeme == "+") {
      Token op_tok = peek();
      advance();
      advance();
      ExprPtr rhs = parse_unary();
      auto u = std::make_unique<UnaryExpr>("++", std::move(rhs));
      u->is_postfix = false;
      u->line = op_tok.line;
      u->col = op_tok.col;
      u->end_col = u->rhs ? (u->rhs->end_col ? u->rhs->end_col : u->rhs->col)
                          : (op_tok.col + 1);
      return u;
    }

    if (op == "-" && peek_n(1).type == TokenType::Symbol &&
        peek_n(1).lexeme == "-") {
      Token op_tok = peek();
      advance();
      advance();
      ExprPtr rhs = parse_unary();
      auto u = std::make_unique<UnaryExpr>("--", std::move(rhs));
      u->is_postfix = false;
      u->line = op_tok.line;
      u->col = op_tok.col;
      u->end_col = u->rhs ? (u->rhs->end_col ? u->rhs->end_col : u->rhs->col)
                          : (op_tok.col + 1);
      return u;
    }
  }
  return parse_primary();
}

ExprPtr Parser::parse_block_literal() {
  expect_symbol("{", "block literal start");
  auto lit = std::make_unique<BlockLiteralExpr>();
  while (!is_symbol("}") && !is_at_end()) {

    bool is_named = peek().type == TokenType::Identifier &&
                    peek_n(1).type == TokenType::Symbol &&
                    peek_n(1).lexeme == "=";
    if (is_named) {
      std::string fname = peek().lexeme;
      advance();
      advance();
      ExprPtr val = parse_expression();
      lit->named.emplace_back(fname, std::move(val));
    } else {
      ExprPtr e = parse_expression();
      if (e)
        lit->positional.push_back(std::move(e));
    }
    if (peek().type == TokenType::EndOfFile)
      break;
    if (is_symbol(","))
      advance();
    else
      break;
  }
  expect_symbol("}", "block literal end");
  return lit;
}

ExprPtr Parser::parse_array_literal() {
  expect_symbol("[", "array literal start");
  auto lit = std::make_unique<ArrayLiteralExpr>();
  while (!is_symbol("]") && !is_at_end()) {

    while (peek().type == TokenType::Comment)
      advance();
    if (is_symbol("]") || is_at_end())
      break;
    ExprPtr e = parse_expression();
    if (e)
      lit->elements.push_back(std::move(e));
    if (peek().type == TokenType::EndOfFile)
      break;

    while (peek().type == TokenType::Comment)
      advance();
    if (is_symbol(","))
      advance();
    else
      break;
  }

  while (peek().type == TokenType::Comment)
    advance();
  expect_symbol("]", "array literal end");
  return lit;
}

ExprPtr Parser::parse_primary() {

  if (peek().type == TokenType::Error) {

    Token bad = peek();
    Parser::diags.push_back(
        {Diagnostic::Severity::Error,
         "invalid numeric literal '" + bad.lexeme +
             "': it runs directly into a letter or an out-of-range digit "
             "with no operator or space between them -- did you mean to "
             "write an operator here, or misspell/forget a separator?",
         bad.line, bad.col, token_end_col(bad)});
    advance();
    auto err = std::make_unique<IdentifierExpr>("__err__");
    err->line = bad.line;
    err->col = bad.col;
    err->end_col = token_end_col(bad);
    return err;
  }

  if (peek().type == TokenType::Identifier && peek().lexeme == "sizeof" &&
      peek_n(1).type == TokenType::Symbol && peek_n(1).lexeme == "(") {
    Token sizeof_tok = peek();
    advance();
    expect_symbol("(", "sizeof expression");

    size_t saved_pos = idx;
    TypePtr type_arg = nullptr;
    ExprPtr expr_arg = nullptr;

    try {

      type_arg = parse_type();

      if (is_symbol(")")) {
        expect_symbol(")", "sizeof type");
        auto s = std::make_unique<SizeofExpr>(std::move(type_arg));
        s->line = sizeof_tok.line;
        s->col = sizeof_tok.col;
        s->end_col = token_end_col(peek());
        return s;
      } else {

        idx = saved_pos;
        type_arg = nullptr;
      }
    } catch (...) {

      idx = saved_pos;
      type_arg = nullptr;
    }

    expr_arg = parse_expression();
    expect_symbol(")", "sizeof expression");
    auto s = std::make_unique<SizeofExpr>(std::move(expr_arg));
    s->line = sizeof_tok.line;
    s->col = sizeof_tok.col;
    s->end_col = token_end_col(peek());
    return s;
  }

  if (peek().type == TokenType::Identifier && peek().lexeme == "cast" &&
      peek_n(1).type == TokenType::Symbol && peek_n(1).lexeme == "<") {
    Token cast_tok = peek();
    advance();
    expect_symbol("<", "cast type start");
    TypePtr t = parse_type();
    expect_symbol(">", "cast type end");
    expect_symbol("(", "cast call");
    ExprPtr inner = parse_expression();
    expect_symbol(")", "cast call end");
    auto c = std::make_unique<CastExpr>(std::move(inner), std::move(t));
    c->is_unsafe = false;
    c->line = cast_tok.line;
    c->col = cast_tok.col;
    return c;
  }
  if (peek().type == TokenType::Identifier && peek().lexeme == "unsafe_cast" &&
      peek_n(1).type == TokenType::Symbol && peek_n(1).lexeme == "<") {
    Token cast_tok = peek();
    advance();
    expect_symbol("<", "unsafe_cast type start");
    TypePtr t = parse_type();
    expect_symbol(">", "unsafe_cast type end");
    expect_symbol("(", "unsafe_cast call");
    ExprPtr inner = parse_expression();
    expect_symbol(")", "unsafe_cast call end");
    auto c = std::make_unique<CastExpr>(std::move(inner), std::move(t));
    c->is_unsafe = true;
    c->line = cast_tok.line;
    c->col = cast_tok.col;
    return c;
  }
  if (peek().type == TokenType::Identifier) {
    Token id_tok = peek();
    std::string id = peek().lexeme;
    if (id == "null") {
      advance();
      auto n = std::make_unique<NullExpr>();
      n->line = id_tok.line;
      n->col = id_tok.col;
      n->end_col = token_end_col(id_tok);
      return n;
    }
    advance();
    ExprPtr e = std::make_unique<IdentifierExpr>(id);
    e->line = id_tok.line;
    e->col = id_tok.col;
    e->end_col = token_end_col(id_tok);
    return parse_call_or_postfix(std::move(e));
  }
  if (peek().type == TokenType::Number) {
    Token num_tok = peek();
    std::string lit = peek().lexeme;
    advance();
    auto e = std::make_unique<NumberExpr>(lit);
    e->line = num_tok.line;
    e->col = num_tok.col;
    e->end_col = token_end_col(num_tok);
    return e;
  }
  if (peek().type == TokenType::String) {
    Token str_tok = peek();
    std::string lit = peek().lexeme;
    advance();
    auto e = std::make_unique<StringExpr>(lit);
    e->line = str_tok.line;
    e->col = str_tok.col;
    e->end_col = token_end_col(str_tok);
    return e;
  }
  if (peek().type == TokenType::Char) {
    Token char_tok = peek();
    std::string lit = peek().lexeme;
    advance();
    auto e = std::make_unique<CharExpr>(lit);
    e->line = char_tok.line;
    e->col = char_tok.col;
    e->end_col = token_end_col(char_tok);
    return e;
  }
  if (peek().type == TokenType::Boolean) {
    Token b_tok = peek();
    bool v = (peek().lexeme == "true");
    advance();
    auto e = std::make_unique<BoolExpr>(v);
    e->line = b_tok.line;
    e->col = b_tok.col;
    e->end_col = token_end_col(b_tok);
    return e;
  }
  if (is_symbol("(")) {

    if (peek_n(1).type == TokenType::Symbol && peek_n(1).lexeme == "." &&
        peek_n(2).type == TokenType::Symbol && peek_n(2).lexeme == "." &&
        peek_n(3).type == TokenType::Symbol && peek_n(3).lexeme == ".") {
      return parse_lambda();
    }

    {
      size_t n = 1;
      bool looks_like_captures = true;
      while (
          !(peek_n(n).type == TokenType::Symbol && peek_n(n).lexeme == ")")) {
        if (peek_n(n).type == TokenType::EndOfFile ||
            peek_n(n).type != TokenType::Identifier) {
          looks_like_captures = false;
          break;
        }
        n++;
        if (peek_n(n).type == TokenType::Symbol && peek_n(n).lexeme == ",") {
          n++;
          continue;
        }
      }
      if (looks_like_captures && peek_n(n).type == TokenType::Symbol &&
          peek_n(n).lexeme == ")" && peek_n(n + 1).type == TokenType::Symbol &&
          peek_n(n + 1).lexeme == "[") {
        bool params_shape = (peek_n(n + 2).type == TokenType::Symbol &&
                             peek_n(n + 2).lexeme == "]") ||
                            (peek_n(n + 2).type == TokenType::Identifier &&
                             peek_n(n + 3).type == TokenType::Symbol &&
                             peek_n(n + 3).lexeme == ":");
        if (params_shape)
          return parse_lambda();
      }
    }

    Token open = peek();
    advance();
    ExprPtr e = parse_expression();
    expect_symbol(")", "grouping");
    if (e) {
      if (e->line <= 0)
        e->line = open.line;
      if (e->col <= 0)
        e->col = open.col;
      if (e->end_col <= 0)
        e->end_col = token_end_col(open);
      return parse_call_or_postfix(std::move(e));
    } else {
      auto err = std::make_unique<IdentifierExpr>("__err__");
      err->line = open.line;
      err->col = open.col;
      err->end_col = token_end_col(open);
      return err;
    }
  }
  if (peek().type == TokenType::Symbol && peek().lexeme == "(" &&
      peek_n(1).type == TokenType::Symbol && peek_n(1).lexeme == ".") {
    return parse_lambda();
  }

  if (is_symbol("{")) {
    return parse_block_literal();
  }

  if (is_symbol("[")) {
    return parse_array_literal();
  }
  Parser::diags.push_back({Diagnostic::Severity::Error,
                           "unexpected token in primary: " + peek().lexeme,
                           peek().line, peek().col});

  advance();
  auto err = std::make_unique<IdentifierExpr>("__err__");
  err->line = peek().line;
  err->col = peek().col;
  err->end_col = peek().col;
  return err;
}

ExprPtr Parser::parse_call_or_postfix(ExprPtr left) {
  while (true) {
    if (!left) {
      return left;
    }

    if (is_symbol("(")) {
      Token open = peek();
      advance();
      auto call = std::make_unique<CallExpr>(std::move(left));
      call->line = call->callee ? call->callee->line : open.line;
      call->col = call->callee ? call->callee->col : open.col;
      while (!is_symbol(")")) {
        if (peek().type == TokenType::EndOfFile)
          break;
        ExprPtr a = parse_expression();
        if (a)
          call->args.push_back(std::move(a));
        if (is_symbol(","))
          advance();
        else
          break;
      }
      if (is_symbol(")")) {
        Token close_tok = peek();
        advance();
        if (!call->args.empty()) {
          Expr *last = call->args.back().get();
          call->end_col = last->end_col ? last->end_col : last->col;
        } else if (call->callee) {
          call->end_col =
              call->callee->end_col ? call->callee->end_col : call->callee->col;
        } else {
          call->end_col = token_end_col(close_tok);
        }
      } else {
        Parser::diags.push_back({Diagnostic::Severity::Error,
                                 "expected closing parantheses ')", peek().line,
                                 peek().col});
      }
      left = std::move(call);
      continue;
    }

    if (is_symbol(".")) {
      advance();
      if (!left) {

        Parser::diags.push_back({Diagnostic::Severity::Error,
                                 "postfix '.' on null expression", peek().line,
                                 peek().col});

        return nullptr;
      }
      if (peek().type != TokenType::Identifier) {
        Parser::diags.push_back({Diagnostic::Severity::Error,
                                 "expected identifier after '.'", peek().line,
                                 peek().col});

        return left;
      }
      std::string fname = peek().lexeme;
      Token fname_tok = peek();
      advance();
      auto mem = std::make_unique<MemberExpr>(std::move(left), fname);
      mem->line = mem->object ? mem->object->line : fname_tok.line;
      mem->col = mem->object ? mem->object->col : fname_tok.col;
      mem->end_col = token_end_col(fname_tok);
      left = std::move(mem);
      continue;
    }

    if (is_symbol("[")) {
      Token open = peek();
      advance();
      ExprPtr idx = parse_expression();
      expect_symbol("]", "index end");
      auto idxexpr =
          std::make_unique<IndexExpr>(std::move(left), std::move(idx));
      idxexpr->line = open.line;
      idxexpr->col = open.col;
      if (idxexpr->index)
        idxexpr->end_col = idxexpr->index->end_col ? idxexpr->index->end_col
                                                   : idxexpr->index->col;
      left = std::move(idxexpr);
      continue;
    }

    if (is_symbol("++") || is_symbol("--")) {
      Token op_tok = peek();
      std::string op = peek().lexeme;
      advance();
      auto un = std::make_unique<UnaryExpr>(op, std::move(left));
      un->is_postfix = true;
      un->line = un->rhs ? un->rhs->line : op_tok.line;
      un->col = un->rhs ? un->rhs->col : op_tok.col;
      un->end_col = token_end_col(op_tok);
      left = std::move(un);
      continue;
    }
    if (is_symbol("+") && peek_n(1).type == TokenType::Symbol &&
        peek_n(1).lexeme == "+") {
      Token op_tok = peek();
      advance();
      advance();
      auto un = std::make_unique<UnaryExpr>("++", std::move(left));
      un->is_postfix = true;
      un->line = un->rhs ? un->rhs->line : op_tok.line;
      un->col = un->rhs ? un->rhs->col : op_tok.col;
      un->end_col = op_tok.col + 1;
      left = std::move(un);
      continue;
    }

    if (is_symbol("-") && peek_n(1).type == TokenType::Symbol &&
        peek_n(1).lexeme == "-") {
      Token op_tok = peek();
      advance();
      advance();
      auto un = std::make_unique<UnaryExpr>("--", std::move(left));
      un->is_postfix = true;
      un->line = un->rhs ? un->rhs->line : op_tok.line;
      un->col = un->rhs ? un->rhs->col : op_tok.col;
      un->end_col = op_tok.col + 1;
      left = std::move(un);
      continue;
    }
    break;
  }
  return left;
}
ExprPtr Parser::parse_lambda() {
  Token open = peek();
  expect_symbol("(", "lambda start");

  std::vector<std::string> explicit_captures;
  if (is_symbol(".") && peek_n(1).type == TokenType::Symbol &&
      peek_n(1).lexeme == "." && peek_n(2).type == TokenType::Symbol &&
      peek_n(2).lexeme == ".") {
    advance();
    advance();
    advance();
  } else {
    while (!is_symbol(")")) {
      if (peek().type != TokenType::Identifier) {
        Parser::diags.push_back(
            {Diagnostic::Severity::Error,
             "expected a capture name or '...' in lambda marker", peek().line,
             peek().col});
        break;
      }
      explicit_captures.push_back(peek().lexeme);
      advance();
      if (is_symbol(","))
        advance();
      else
        break;
    }
  }
  expect_symbol(")", "lambda marker end");
  std::vector<std::pair<std::string, TypePtr>> params;
  if (is_symbol("[")) {
    advance();
    while (!is_symbol("]")) {
      if (peek().type != TokenType::Identifier) {

        Parser::diags.push_back({Diagnostic::Severity::Error,
                                 "expected parameter name in lambda",
                                 peek().line, peek().col});

        break;
      }
      std::string pname = peek().lexeme;
      advance();
      if (is_symbol(":"))
        advance();
      TypePtr ptype = parse_type();
      params.emplace_back(std::move(pname), std::move(ptype));
      if (is_symbol(","))
        advance();
      else
        break;
    }
    expect_symbol("]", "lambda params end");
  }

  TypePtr lambda_return_type;
  if (peek().type == TokenType::Symbol && peek().lexeme == "->") {
    advance();
    lambda_return_type = parse_type();
  }
  StmtPtr body = parse_block();
  auto lambda = std::make_unique<LambdaExpr>();
  lambda->params = std::move(params);
  lambda->captures = std::move(explicit_captures);
  lambda->return_type = std::move(lambda_return_type);
  if (auto blk = dynamic_cast<BlockStmt *>(body.get())) {
    lambda->body = std::move(blk->stmts);
  }
  lambda->line = open.line;
  lambda->col = open.col;
  if (!lambda->body.empty()) {
    Stmt *last = lambda->body.back().get();
    lambda->end_col = last ? last->col : open.col;
  } else {
    lambda->end_col = open.col;
  }
  return lambda;
}

TypePtr Parser::parse_generic_type_arg() {

  if (peek().type == TokenType::Identifier &&
      peek_n(1).type == TokenType::Symbol && peek_n(1).lexeme == "." &&
      peek_n(2).type == TokenType::Identifier) {
    std::string opt_name = peek().lexeme;
    advance();
    advance();
    std::string field_name = peek().lexeme;
    advance();
    return std::make_unique<TypeNode>("__optval__:" + opt_name + "." +
                                      field_name);
  }

  if (peek().type == TokenType::Identifier &&
      value_generic_scope.count(peek().lexeme) &&
      !(peek_n(1).type == TokenType::Symbol && peek_n(1).lexeme != "," &&
        peek_n(1).lexeme != ">" && peek_n(1).lexeme != ">>")) {
    std::string vname = peek().lexeme;
    advance();
    return std::make_unique<TypeNode>(vname);
  }

  if (peek().type == TokenType::Number ||
      (peek().type == TokenType::Symbol && peek().lexeme == "-" &&
       peek_n(1).type == TokenType::Number)) {
    Token val_tok = peek();
    ExprPtr size_lhs = parse_unary();
    ExprPtr size_expr = parse_binary_rhs(21, std::move(size_lhs));
    auto val = evaluate_const_expr(size_expr.get());
    if (val) {
      return std::make_unique<TypeNode>("__size__:" + std::to_string(*val));
    } else if (expr_only_references_value_generics(size_expr.get())) {
      auto size_node = std::make_unique<TypeNode>("__gsize_expr__");
      size_node->size_expr = std::move(size_expr);
      return size_node;
    }
    Parser::diags.push_back(
        {Diagnostic::Severity::Error,
         "generic argument must be a compile-time constant expression",
         val_tok.line, val_tok.col});
    return std::make_unique<TypeNode>("__size__:1");
  }

  return parse_type();
}

TypePtr Parser::parse_type() {

  if (peek().type == TokenType::Identifier && peek().lexeme == "const") {
    if (peek_n(1).type == TokenType::Symbol && peek_n(1).lexeme == "<") {

    } else {

      advance();
      TypePtr inner = parse_type();
      auto t = std::make_unique<TypeNode>("const");
      t->params.push_back(std::move(inner));
      return t;
    }
  }

  if (peek().type == TokenType::Identifier && peek().lexeme == "refc") {
    if (peek_n(1).type == TokenType::Symbol && peek_n(1).lexeme == "<") {

    } else {

      advance();
      TypePtr inner = parse_type();
      auto t = std::make_unique<TypeNode>("refc");
      t->params.push_back(std::move(inner));
      return t;
    }
  }

  if (peek().type == TokenType::Identifier) {
    std::string name = peek().lexeme;
    Token name_tok = peek();
    advance();
    if (name == "func") {
      auto t = std::make_unique<TypeNode>("func");
      expect_symbol("(", "function type parameters");
      int param_index = 0;
      while (!is_symbol(")")) {
        TypePtr p;

        if (peek().type == TokenType::Identifier && peek().lexeme == "this") {
          Token this_tok = peek();
          advance();
          if (param_index != 0) {
            Parser::diags.push_back(
                {Diagnostic::Severity::Error,
                 "'this' can only be used as the first parameter of a "
                 "function type",
                 this_tok.line, this_tok.col});
          }
          p = std::make_unique<TypeNode>("this");
        } else {
          p = parse_type();
        }
        t->params.push_back(std::move(p));
        param_index++;
        if (is_symbol(","))
          advance();
        else
          break;
      }
      expect_symbol(")", "end function type param list");
      if (peek().type == TokenType::Symbol && peek().lexeme == "->") {
        advance();
        TypePtr rt = parse_type();
        t->return_type = std::move(rt);
      }
      return t;
    }

    auto t = std::make_unique<TypeNode>(name);
    if (is_symbol("<")) {
      advance();
      while (true) {

        if (is_symbol(">")) {
          break;
        }

        if (peek().type == TokenType::Symbol && peek().lexeme == ">>") {
          break;
        }

        if (name == "Array" && t->params.size() == 1) {

          if (peek().type == TokenType::Identifier &&
              value_generic_scope.count(peek().lexeme) &&
              !(peek_n(1).type == TokenType::Symbol &&
                peek_n(1).lexeme != "," && peek_n(1).lexeme != ">" &&
                peek_n(1).lexeme != ">>")) {
            std::string vname = peek().lexeme;
            advance();
            t->params.push_back(std::make_unique<TypeNode>(vname));
            if (is_symbol(",")) {
              advance();
            } else if (!is_symbol(">") && !(peek().type == TokenType::Symbol &&
                                            peek().lexeme == ">>")) {
              break;
            }
            continue;
          }

          Token size_tok = peek();

          ExprPtr size_lhs = parse_unary();
          ExprPtr size_expr = parse_binary_rhs(21, std::move(size_lhs));
          auto size_val = evaluate_const_expr(size_expr.get());
          if (size_val) {
            std::string size_str = std::to_string(*size_val);
            auto size_node = std::make_unique<TypeNode>("__size__:" + size_str);
            t->params.push_back(std::move(size_node));
          } else if (expr_only_references_value_generics(size_expr.get())) {

            auto size_node = std::make_unique<TypeNode>("__gsize_expr__");
            size_node->size_expr = std::move(size_expr);
            t->params.push_back(std::move(size_node));
          } else {
            Parser::diags.push_back(
                {Diagnostic::Severity::Error,
                 "Array size must be a compile-time constant expression",
                 size_tok.line, size_tok.col});

            auto size_node = std::make_unique<TypeNode>("__size__:1");
            t->params.push_back(std::move(size_node));
          }
        } else {
          TypePtr sub = parse_generic_type_arg();
          t->params.push_back(std::move(sub));
        }

        if (is_symbol(",")) {
          advance();
        } else if (!is_symbol(">") && !(peek().type == TokenType::Symbol &&
                                        peek().lexeme == ">>")) {
          break;
        }
      }

      expect_symbol(">", "end generic type");
    }
    return t;
  }
  Parser::diags.push_back(
      {Diagnostic::Severity::Error, "expected type", peek().line, peek().col});

  advance();
  return std::make_unique<TypeNode>("__err_type__");
}
