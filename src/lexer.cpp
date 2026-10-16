#include "lexer.h"
#include <cctype>
#include <optional>

Lexer::Lexer(const std::string &source, const std::string &filename)
    : src(source), fname(filename), pos(0), line(1), col(1) {}
bool Lexer::is_binary_digit(char c) const { return c == '0' || c == '1'; }

bool Lexer::is_hex_digit(char c) const {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}
bool Lexer::is_at_end() const { return pos >= src.size(); }
char Lexer::peek() const { return is_at_end() ? '\0' : src[pos]; }
char Lexer::peek_next() const {
  return (pos + 1 >= src.size()) ? '\0' : src[pos + 1];
}
char Lexer::advance() {
  char c = peek();
  pos++;
  if (c == '\n') {
    line++;
    col = 1;
  } else
    col++;
  return c;
}

bool Lexer::is_alpha(char c) const {
  return std::isalpha((unsigned char)c) || c == '_';
}
bool Lexer::is_digit(char c) const { return std::isdigit((unsigned char)c); }
bool Lexer::match(char expected) {
  if (is_at_end())
    return false;
  if (src[pos] != expected)
    return false;
  pos++;
  col++;
  return true;
}

void Lexer::skip_whitespace_and_comments(std::vector<Token> &out) {
  for (;;) {
    if (is_at_end())
      return;
    char c = peek();
    if (c == ' ' || c == '\r' || c == '\t' || c == '\n') {
      advance();
      continue;
    }

    if (c == '/' && peek_next() == '/') {
      int start_line = line, start_col = col;
      std::string lex = "//";
      advance();
      advance();
      while (!is_at_end() && peek() != '\n') {
        lex.push_back(advance());
      }
      out.emplace_back(TokenType::Comment, lex, start_line, start_col);
      continue;
    }

    if (c == '/' && peek_next() == '*') {
      int start_line = line, start_col = col;
      std::string lex = "/*";
      advance();
      advance();
      while (!is_at_end()) {
        if (peek() == '*' && peek_next() == '/') {
          lex.push_back(advance());
          lex.push_back(advance());
          break;
        } else {
          lex.push_back(advance());
        }
      }
      out.emplace_back(TokenType::Comment, lex, start_line, start_col);
      continue;
    }
    break;
  }
}

Token Lexer::make_token(TokenType t, const std::string &lexeme, int ln, int c) {
  return Token(t, lexeme, ln, c);
}

Token Lexer::lex_identifier_or_keyword() {
  int start_line = line, start_col = col;
  std::string s;
  while (!is_at_end() && (is_alpha(peek()) || is_digit(peek()))) {
    s.push_back(advance());
  }
  if (s == "HEADER_FILE") {
    return make_token(TokenType::HeaderFileMarker, s, start_line, start_col);
  }
  if (s == "true" || s == "false") {
    return make_token(TokenType::Boolean, s, start_line, start_col);
  }
  if (s == "INF") {
    return make_token(TokenType::Number, s, start_line, start_col);
  }
  if (s == "NAN") {
    return make_token(TokenType::Number, s, start_line, start_col);
  }
  return make_token(TokenType::Identifier, s, start_line, start_col);
}

Token Lexer::lex_number() {
  int start_line = line, start_col = col;
  std::string s;

  auto check_invalid_trailing_ident = [&](const std::string &numeric_part,
                                          bool also_reject_digits =
                                              false) -> std::optional<Token> {
    if (is_at_end())
      return std::nullopt;
    bool bad_start =
        is_alpha(peek()) || (also_reject_digits && is_digit(peek()));
    if (!bad_start)
      return std::nullopt;
    std::string full = numeric_part;
    while (!is_at_end() && (is_alpha(peek()) || is_digit(peek())))
      full.push_back(advance());
    return make_token(TokenType::Error, full, start_line, start_col);
  };

  if (peek() == '0' && !is_at_end()) {
    char next = peek_next();

    if (next == 'b' || next == 'B') {
      std::string raw = "0";
      advance();
      raw.push_back(advance());

      unsigned long long value = 0;
      bool has_digits = false;
      while (!is_at_end() && is_binary_digit(peek())) {
        value = value * 2 + (peek() - '0');
        raw.push_back(peek());
        advance();
        has_digits = true;
      }

      if (auto err = check_invalid_trailing_ident(raw, true))
        return *err;

      if (!has_digits) {
        s = "0";
      } else {
        s = std::to_string(value);
      }
      return make_token(TokenType::Number, s, start_line, start_col);
    }

    if (next == 'x' || next == 'X') {
      std::string raw = "0";
      advance();
      raw.push_back(advance());

      unsigned long long value = 0;
      bool has_digits = false;
      while (!is_at_end() && is_hex_digit(peek())) {
        char c = peek();
        value *= 16;
        if (c >= '0' && c <= '9') {
          value += c - '0';
        } else if (c >= 'a' && c <= 'f') {
          value += c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
          value += c - 'A' + 10;
        }
        raw.push_back(c);
        advance();
        has_digits = true;
      }

      if (auto err = check_invalid_trailing_ident(raw))
        return *err;

      if (!has_digits) {
        s = "0";
      } else {
        s = std::to_string(value);
      }
      return make_token(TokenType::Number, s, start_line, start_col);
    }
  }

  if (peek() == '.') {
    s.push_back(advance());
    while (!is_at_end() && is_digit(peek()))
      s.push_back(advance());
    if (auto err = check_invalid_trailing_ident(s))
      return *err;
    return make_token(TokenType::Number, s, start_line, start_col);
  }

  while (!is_at_end() && is_digit(peek())) {
    s.push_back(advance());
  }

  if (!is_at_end() && peek() == '.' && is_digit(peek_next())) {
    s.push_back(advance());
    while (!is_at_end() && is_digit(peek()))
      s.push_back(advance());
  }

  if (auto err = check_invalid_trailing_ident(s))
    return *err;

  return make_token(TokenType::Number, s, start_line, start_col);
}

Token Lexer::lex_string() {
  int start_line = line, start_col = col;
  std::string s;
  char quote = advance();
  s.push_back(quote);
  while (!is_at_end()) {
    char c = advance();
    s.push_back(c);
    if (c == '\\') {
      if (!is_at_end())
        s.push_back(advance());
      continue;
    }
    if (c == '"')
      break;
  }
  return make_token(TokenType::String, s, start_line, start_col);
}

Token Lexer::lex_char() {
  int start_line = line, start_col = col;
  std::string s;
  char quote = advance();
  s.push_back(quote);
  while (!is_at_end()) {
    char c = advance();
    s.push_back(c);
    if (c == '\\') {
      if (!is_at_end())
        s.push_back(advance());
      continue;
    }
    if (c == '\'')
      break;
  }
  return make_token(TokenType::Char, s, start_line, start_col);
}

Token Lexer::lex_token() {

  {
    std::vector<Token> _skip_dummy;
    skip_whitespace_and_comments(_skip_dummy);
  }

  if (is_at_end())
    return make_token(TokenType::EndOfFile, "", line, col);
  char c = peek();
  int start_line = line, start_col = col;

  if (is_alpha(c))
    return lex_identifier_or_keyword();

  if (is_digit(c))
    return lex_number();

  if (c == '.' && is_digit(peek_next()))
    return lex_number();

  if (c == '"')
    return lex_string();

  if (c == '\'')
    return lex_char();

  std::string two;
  two.push_back(c);
  two.push_back(peek_next());
  const char *two_ops[] = {"==", "!=", "<=", ">=", "&&", "||",
                           "<<", ">>", "+=", "-=", "*=", "/=",
                           "%=", "&=", "|=", "^=", "->", nullptr};
  for (const char **p = two_ops; *p; ++p) {
    if (two == *p) {
      advance();
      advance();
      return make_token(TokenType::Symbol, two, start_line, start_col);
    }
  }

  std::string s;
  s.push_back(advance());
  return make_token(TokenType::Symbol, s, start_line, start_col);
}

std::vector<Token> Lexer::tokenize() {
  std::vector<Token> tokens;
  while (!is_at_end()) {
    skip_whitespace_and_comments(tokens);
    if (is_at_end())
      break;
    Token t = lex_token();

    if (t.type != TokenType::Comment)
      tokens.push_back(t);
    if (t.type == TokenType::EndOfFile)
      break;
  }
  tokens.emplace_back(TokenType::EndOfFile, "", line, col);
  return tokens;
}
