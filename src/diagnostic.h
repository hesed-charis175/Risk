#pragma once

#include <string>

struct Diagnostic {

  enum class Severity { Error, Warning };

  Severity severity;

  std::string message;

  int line;

  int col;

  int end_col;

  std::string file;
};
