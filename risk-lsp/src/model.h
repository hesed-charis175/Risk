#pragma once
#include "json.hpp"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct Sym {
  std::string k;   // function block opts optmember alias generic global
                   // field method local param gparam
  std::string n;   // name
  std::string d;   // display detail like `func add(a: i32) -> i32`
  std::string t;   // type (variables: its type; functions: return type)
  std::string p;   // container( owning block/opts or function)
  std::string doc; // preceding `//` comment lines
  std::string ext; // extern header if declared in an `extern` block
  int l = 0, c = 0;
  bool scoped = false; // local/param/gparam: visible only in [s, e]
  int sl = 0, sc = 0, el = 0, ec = 0;
  nlohmann::json ty;           // structured type, see analyzer.cpp
  std::vector<std::string> ps; // function parameter labels
  std::vector<std::string> psNames;

  bool is_member() const {
    return k == "field" || k == "method" || k == "optmember";
  }
  bool is_variable() const {
    return k == "local" || k == "param" || k == "global" || k == "field";
  }
};

struct DiagInfo {
  int sev = 1;
  int l = 0, c = 0, e = 0;
  std::string msg;
};

struct ImportInfo {
  std::string path, alias;
  int l = 0, c = 0;
};

struct Analysis {
  std::vector<Sym> syms;
  std::vector<DiagInfo> diags;
  std::vector<ImportInfo> imports;
  std::unordered_map<std::string, std::vector<size_t>> by_name;

  void index();
};

std::shared_ptr<Analysis> run_analysis(const std::string &path,
                                       const std::string &src, bool typecheck,
                                       std::string *err);
