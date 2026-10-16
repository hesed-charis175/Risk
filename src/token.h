#ifndef RISK_TOKEN_H
#define RISK_TOKEN_H

#include <string>

enum class TokenType {
  Identifier,
  Number,
  String,
  Char,
  Symbol,
  Comment,
  HeaderFileMarker,
  Boolean,
  Error,
  EndOfFile
};

struct Token {
  TokenType type;
  std::string lexeme;
  int line;
  int col;

  Token() : type(TokenType::EndOfFile), lexeme(""), line(0), col(0) {}

  Token(TokenType t, std::string l, int ln, int c)
      : type(t), lexeme(std::move(l)), line(ln), col(c) {}
};

inline const char *token_type_name(TokenType t) {
  switch (t) {
  case TokenType::Identifier:
    return "Identifier";
  case TokenType::Number:
    return "Number";
  case TokenType::String:
    return "String";
  case TokenType::Char:
    return "Char";
  case TokenType::Symbol:
    return "Symbol";
  case TokenType::Comment:
    return "Comment";
  case TokenType::HeaderFileMarker:
    return "HeaderFileMarker";
  case TokenType::Boolean:
    return "Boolean";
  case TokenType::Error:
    return "Error";
  case TokenType::EndOfFile:
    return "EOF";
  default:
    return "Unknown";
  }
}

#endif
