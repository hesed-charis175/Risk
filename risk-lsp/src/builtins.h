#pragma once
#include <string>
#include <vector>

struct Builtin {
  std::string name;
  std::string sig;
  std::string ret;
  std::vector<std::string> params;
  std::string doc;
};

const std::vector<Builtin> &builtin_functions();
const Builtin *find_builtin_function(const std::string &name);

const std::vector<Builtin> &builtin_methods(const std::string &type_name);
const Builtin *find_builtin_method(const std::string &type_name,
                                   const std::string &name);

const std::vector<std::string> &keywords();
const std::vector<std::string> &primitive_types();
const std::vector<std::string> &container_types();

std::string describe_builtin_word(const std::string &name);
