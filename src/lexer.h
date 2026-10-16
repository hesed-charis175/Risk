#ifndef RISK_LEXER_H
#define RISK_LEXER_H

#include "token.h"
#include <string>
#include <vector>

class Lexer {
public:
  Lexer(const std::string &source, const std::string &filename = "");

  std::vector<Token> tokenize();

private:
  std::string src;

  bool is_binary_digit(char c) const;

  bool is_hex_digit(char c) const;

  std::string fname;

  size_t pos;

  int line;

  int col;

  bool is_at_end() const;

  char peek() const;

  char peek_next() const;

  char advance();

  bool is_alpha(char c) const;

  bool is_digit(char c) const;

  bool match(char expected);

  void skip_whitespace_and_comments(std::vector<Token> &out);

  Token make_token(TokenType t, const std::string &lexeme, int ln, int c);

  Token lex_identifier_or_keyword();

  Token lex_number();

  Token lex_string();

  Token lex_char();

  Token lex_token();
};

#endif
