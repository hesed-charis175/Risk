#include "ast.h"
#include "codegen.h"
#include "lexer.h"
#include "parser.h"

#include "runtime.h"
#include "typechecker.h"
#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <unordered_set>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#include <sys/stat.h>
#include <vector>

static std::string expand_tilde(const std::string &path) {
  if (!path.empty() && path[0] == '~') {
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home || !home[0])
      home = getenv("USERPROFILE");
    if (!home || !home[0]) {
      const char *drive = getenv("HOMEDRIVE");
      const char *homepath = getenv("HOMEPATH");
      if (drive && homepath && drive[0] && homepath[0]) {
        std::string h = std::string(drive) + homepath;
        return h + path.substr(1);
      }
    }
#endif
    if (home && home[0])
      return std::string(home) + path.substr(1);
  }
  return path;
}

static std::string get_stdlib_base() {
  const char *env = getenv("RISK_STDLIB");
  if (env && env[0]) {
    std::string base = expand_tilde(env);
    if (base.back() != '/' && base.back() != '\\')
      base += '/';
    return base;
  }
  return "./stdlib/";
}

static std::string read_file(const std::string &path);

static std::string resolve_stdlib_file(const std::string &base,
                                       const std::string &ext) {
  std::string stdlib_base = get_stdlib_base();
  std::vector<std::string> candidates = {
      stdlib_base + base + ext,
      stdlib_base + base + "/" + base + ext,
      "./stdlib/" + base + ext,
  };
  for (auto &c : candidates) {
    std::ifstream probe(c);
    if (probe.good())
      return c;
  }
  return candidates[0];
}

static std::string get_self_exe_path(const char *argv0) {
#ifndef _WIN32
  char resolved[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", resolved, sizeof(resolved) - 1);
  if (n > 0) {
    resolved[n] = '\0';
    return std::string(resolved);
  }
#endif
  return argv0 ? std::string(argv0) : std::string("riskc");
}

static long file_mtime(const std::string &path) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0)
    return -1;
  return static_cast<long>(st.st_mtime);
}

static bool path_is_regular_file_driver(const std::string &path) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0)
    return false;
  return S_ISREG(st.st_mode);
}

static bool risk_file_is_header_module(const std::string &path) {
  std::string src = read_file(path);
  if (src.empty())
    return false;
  Lexer lexer(src, path);
  auto tokens = lexer.tokenize();
  for (auto &t : tokens) {
    if (t.type == TokenType::Comment)
      continue;
    return t.type == TokenType::HeaderFileMarker;
  }
  return false;
}

static bool ensure_header_module_built(const std::string &risk_path,
                                       const std::string &riskh_path,
                                       const std::string &obj_path,
                                       const std::string &self_exe) {
  long src_time = file_mtime(risk_path);
  if (src_time < 0)
    return true;
  long riskh_time = file_mtime(riskh_path);
  long obj_time = file_mtime(obj_path);
  bool stale = riskh_time < 0 || obj_time < 0 || riskh_time < src_time ||
               obj_time < src_time;
  if (!stale)
    return true;

  std::cout << "Rebuilding out-of-date header module: " << risk_path << "\n";
  std::string cmd = "\"" + self_exe + "\" \"" + risk_path + "\"";
  int rc = std::system(cmd.c_str());
  if (rc != 0) {
    std::cerr << "Failed to rebuild header module: " << risk_path << "\n";
    return false;
  }
  return true;
}

static bool ensure_all_header_deps_built(TranslationUnit *tu,
                                         const std::string &source_dir,
                                         const std::string &self_exe) {
  for (auto &dptr : tu->decls) {
    auto im = dynamic_cast<ImportDecl *>(dptr.get());
    if (!im)
      continue;
    std::string header = im->path;
    if (!header.empty() && header.front() == '"')
      header.erase(0, 1);
    if (!header.empty() && header.back() == '"')
      header.pop_back();

    bool is_stdlib = (!header.empty() && header.front() == '@');
    if (is_stdlib)
      header.erase(0, 1);
    if (!(header.size() >= 5 && header.substr(header.size() - 5) == ".risk"))
      continue;

    std::string base = header.substr(0, header.size() - 5);
    std::string risk_path, riskh_path, obj_path;
    if (is_stdlib) {
      risk_path = resolve_stdlib_file(base, ".risk");
      riskh_path = resolve_stdlib_file(base, ".riskh");
      obj_path = resolve_stdlib_file(base, ".o");
    } else {
      risk_path = source_dir + header;
      riskh_path = source_dir + base + ".riskh";
      obj_path = source_dir + base + ".o";
    }

    if (!path_is_regular_file_driver(risk_path))
      continue;

    if (!risk_file_is_header_module(risk_path))
      continue;

    if (!ensure_header_module_built(risk_path, riskh_path, obj_path, self_exe))
      return false;
  }
  return true;
}

static void
collect_transitive_header_objs(const std::string &risk_path,
                               const std::string &source_dir,
                               std::vector<std::string> &link_files,
                               std::unordered_set<std::string> &visited_objs) {
  std::string src = read_file(risk_path);
  if (src.empty())
    return;
  Lexer lexer(src, risk_path);
  auto tokens = lexer.tokenize();

  for (auto &t : tokens) {
    if (t.type == TokenType::Comment)
      continue;
    if (t.type == TokenType::HeaderFileMarker) {
      auto it = std::find_if(tokens.begin(), tokens.end(),
                             [&](const Token &x) { return &x == &t; });
      if (it != tokens.end())
        tokens.erase(it);
    }
    break;
  }
  Parser parser(tokens);
  auto sub_tu = parser.parse_translation_unit();
  if (!sub_tu)
    return;

  for (auto &dptr : sub_tu->decls) {
    auto im = dynamic_cast<ImportDecl *>(dptr.get());
    if (!im)
      continue;
    std::string header = im->path;
    if (!header.empty() && header.front() == '"')
      header.erase(0, 1);
    if (!header.empty() && header.back() == '"')
      header.pop_back();
    bool is_stdlib = (!header.empty() && header.front() == '@');
    if (is_stdlib)
      header.erase(0, 1);
    if (!(header.size() >= 5 && header.substr(header.size() - 5) == ".risk"))
      continue;

    std::string base = header.substr(0, header.size() - 5);
    std::string sub_risk_path, sub_obj_path;
    if (is_stdlib) {
      sub_risk_path = resolve_stdlib_file(base, ".risk");
      sub_obj_path = resolve_stdlib_file(base, ".o");
    } else {
      sub_risk_path = source_dir + header;
      sub_obj_path = source_dir + base + ".o";
    }

    if (!path_is_regular_file_driver(sub_risk_path))
      continue;
    if (!risk_file_is_header_module(sub_risk_path))
      continue;

    if (visited_objs.find(sub_obj_path) != visited_objs.end())
      continue;

    visited_objs.insert(sub_obj_path);
    link_files.push_back(sub_obj_path);

    collect_transitive_header_objs(sub_risk_path, source_dir, link_files,
                                   visited_objs);
  }
}

#define ANSI_CODE "\033[1;"

#define ANSI_CODE_FG '9'
#define ANSI_CODE_BG "10"

struct ANSI_COLORS {
  static constexpr const char BLACK = '0';
  static constexpr const char RED = '1';
  static constexpr const char GREEN = '2';
  static constexpr const char YELLOW = '3';
  static constexpr const char BLUE = '4';
  static constexpr const char MAGENTA = '5';
  static constexpr const char CYAN = '6';
  static constexpr const char WHITE = '7';
};

std::string ANSI_SEQUENCE_RESET = "\033[0m";
void loadingAnim(const std::string &text) {
  int timer_count = 20, timer_index = 0;
  const char *loading_chars = "/-\\|";
  while (timer_count > 0) {
    std::cout << "\rLoading..." << loading_chars[timer_index];
    std::cout.flush();
    timer_index = (timer_index + 1) % 4;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    timer_count--;
    if (!timer_count)
      std::cout << std::flush << text << std::endl;
  }
}

static void setup_windows_console() {
#ifdef _WIN32

  SetConsoleOutputCP(CP_UTF8);

  HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
  if (hOut != INVALID_HANDLE_VALUE) {
    DWORD dwMode = 0;
    if (GetConsoleMode(hOut, &dwMode)) {
      dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
      SetConsoleMode(hOut, dwMode);
    }
  }
#endif
}

void print_help() {
  setup_windows_console();
  std::cout << R"(
╔═══════════════════════════════════════════════════════════════════════════╗
║                                                                           ║
║   ██████╗ ██╗███████╗██╗  ██╗     ██████╗ ██████╗ ███╗   ███╗██████╗    ║
║   ██╔══██╗██║██╔════╝██║ ██╔╝    ██╔════╝██╔═══██╗████╗ ████║██╔══██╗   ║
║   ██████╔╝██║███████╗█████╔╝     ██║     ██║   ██║██╔████╔██║██████╔╝   ║
║   ██╔══██╗██║╚════██║██╔═██╗     ██║     ██║   ██║██║╚██╔╝██║██╔═══╝    ║
║   ██║  ██║██║███████║██║  ██╗    ╚██████╗╚██████╔╝██║ ╚═╝ ██║██║        ║
║   ╚═╝  ╚═╝╚═╝╚══════╝╚═╝  ╚═╝     ╚═════╝ ╚═════╝ ╚═╝     ╚═╝╚═╝        ║
║                                                                         ║
║                    The Systems Programming Language                     ║
║                          v0.1.0 - Alpha Release                         ║
║                                                                           ║
╚═══════════════════════════════════════════════════════════════════════════╝

)" << "\033[1;96m"
            << "USAGE:" << "\033[0m" << R"(
    riskc <file.risk> [OPTIONS]

)" << "\033[1;96m"
            << "OPTIONS:" << "\033[0m" << R"(
    -o, --out <name>        Specify output executable name (default: a.out)
    -O0, -O1, -O2, -O3      Set optimization level (forwarded to the C
                            backend; default: -O0)
    -Os                     Optimize for size (forwarded to the C backend)
    -Og                     Optimize for debugging (forwarded to the C backend)
    -flto                   Enable link-time optimization (forwarded to the
                            C backend)
    -c                      Compile only; produce an object file, do not link
    -g                      Emit debug info so gdb/lldb show original .risk
                            source (not the generated C) while stepping
    -i                      Keep the generated intermediate .c file instead
                            of deleting it after compilation
    --profile               Enable compilation profiling with detailed metrics
    -h, --help              Display this help message
    --version               Show compiler version information

)" << "\033[1;96m"
            << "EXAMPLES:" << "\033[0m" << R"(
    # Compile a Risk program
    riskc main.risk

    # Compile with custom output name
    riskc main.risk -o myprogram

    # Compile with optimizations and debug info
    riskc main.risk -O2 -g

    # Compile to an object file only, keeping the intermediate C
    riskc main.risk -c -i

    # Compile with profiling enabled
    riskc main.risk --profile

    # Compile header file (generates .h and .risk files)
    riskc mylib.risk    # (if file contains HEADER_FILE marker)
)" << "\033[1;96m"
            << "FILE STRUCTURE:" << "\033[0m" << R"(
    Risk source files use the .risk extension
    Header files are marked with HEADER_FILE at the top

)" << "\033[1;93m"
            << "Copyright © 2026 Risk Language Contributors (Right now, Just "
               "Hesed Charis)"
            << "\033[0m" << "\n";
}

void print_version() {
  setup_windows_console();
  std::cout << R"(
╔═══════════════════════════════════════════════════════════════════════════╗
║                                                                           ║
║   ██████╗ ██╗███████╗██╗  ██╗     ██████╗ ██████╗ ███╗   ███╗██████╗    ║
║   ██╔══██╗██║██╔════╝██║ ██╔╝    ██╔════╝██╔═══██╗████╗ ████║██╔══██╗   ║
║   ██████╔╝██║███████╗█████╔╝     ██║     ██║   ██║██╔████╔██║██████╔╝   ║
║   ██╔══██╗██║╚════██║██╔═██╗     ██║     ██║   ██║██║╚██╔╝██║██╔═══╝    ║
║   ██║  ██║██║███████║██║  ██╗    ╚██████╗╚██████╔╝██║ ╚═╝ ██║██║        ║
║   ╚═╝  ╚═╝╚═╝╚══════╝╚═╝  ╚═╝     ╚═════╝ ╚═════╝ ╚═╝     ╚═╝╚═╝        ║
║                                                                           ║
╚═══════════════════════════════════════════════════════════════════════════╝

)" << "\033[1;96m"
            << "Risk Compiler (riskc)" << "\033[0m" << R"(
Version: 0.1.0-alpha
Build:   )" << __DATE__
            << " " << __TIME__ << R"(
Target:  )"
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
            << "x86_64-windows-msvc"
#elif defined(_WIN32)
            << "x86-windows-msvc"
#elif defined(__APPLE__) && defined(__aarch64__)
            << "arm64-apple-darwin"
#elif defined(__APPLE__)
            << "x86_64-apple-darwin"
#elif defined(__aarch64__)
            << "aarch64-linux-gnu"
#else
            << "x86_64-linux-gnu"
#endif
            << R"(
)" << "\033[1;93m"
            << "Copyright © 2026 Risk Lang Contributors (Right now, Just Hesed "
               "Charis)"
            << "\033[0m" << "\n";
}

static void remove_file(const std::string &path) {
#ifdef _WIN32
  std::system(("del /f /q \"" + path + "\"").c_str());
#else
  std::system(("rm -f \"" + path + "\"").c_str());
#endif
}
static std::string read_file(const std::string &path) {
  std::ifstream in(path, std::ios::in | std::ios::binary);
  if (!in)
    return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

static bool write_file(const std::string &path, const std::string &data) {
  std::ofstream out(path, std::ios::out | std::ios::binary);
  if (!out)
    return false;
  out << data;
  return true;
}

static std::string module_name_from_path(const std::string &path) {
  std::string name = path;
  size_t slash = name.find_last_of("/\\");
  if (slash != std::string::npos)
    name = name.substr(slash + 1);
  if (name.size() >= 5 && name.substr(name.size() - 5) == ".risk")
    name = name.substr(0, name.size() - 5);
  return name;
}

static std::string typenode_to_string(TypeNode *tn) {
  if (!tn)
    return "void";
  if (tn->name == "func") {
    std::string s = "func(";
    for (size_t i = 0; i < tn->params.size(); ++i) {
      if (i)
        s += ", ";
      s += typenode_to_string(tn->params[i].get());
    }
    s += ")";
    if (tn->return_type) {
      s += " -> ";
      s += typenode_to_string(tn->return_type.get());
    }
    return s;
  }
  std::string s = tn->name;
  if (!tn->params.empty()) {
    s += "<";
    for (size_t i = 0; i < tn->params.size(); ++i) {
      if (i)
        s += ", ";
      s += typenode_to_string(tn->params[i].get());
    }
    s += ">";
  }
  return s;
}

static std::string header_from_tu(TranslationUnit *tu) {
  std::ostringstream out;
  out << "\n\n";
  for (auto &dptr : tu->decls) {
    if (auto f = dynamic_cast<FuncDecl *>(dptr.get())) {
      out << "func " << f->name << "(";
      for (size_t i = 0; i < f->params.size(); ++i) {
        if (i)
          out << ", ";
        out << f->params[i].name << " : "
            << typenode_to_string(f->params[i].type.get());
      }
      out << ")";
      if (f->return_type) {
        out << " -> " << typenode_to_string(f->return_type.get());
      }
      out << ";\n";
    }
  }
  return out.str();
}

static std::string c_header_from_tu(TranslationUnit *tu,
                                    const std::string &module_name) {
  std::ostringstream out;
  out << "\n";
  out << "#pragma once\n";
  out << "#include <stdint.h>\n";
  out << "#include <stdbool.h>\n";
  out << "#include <stddef.h>\n\n";
  for (auto &dptr : tu->decls) {
    if (auto f = dynamic_cast<FuncDecl *>(dptr.get())) {
      std::string ret = "void";
      if (f->return_type) {
        std::string rn = f->return_type->name;
        if (rn == "i32")
          ret = "int32_t";
        else if (rn == "i64")
          ret = "int64_t";
        else if (rn == "f64")
          ret = "double";
        else if (rn == "string")
          ret = "const char*";
        else if (rn == "void")
          ret = "void";
        else
          ret = module_name + "__" + rn;
      }
      out << ret << " " << module_name << "__" << f->name << "(";
      for (size_t i = 0; i < f->params.size(); ++i) {
        if (i)
          out << ", ";
        std::string pn = f->params[i].type->name;
        std::string ctype = "void*";
        if (pn == "i32")
          ctype = "int32_t";
        else if (pn == "f64")
          ctype = "double";
        else if (pn == "string")
          ctype = "const char*";
        out << ctype << " " << f->params[i].name;
      }
      out << ");\n";
    }
  }
  return out.str();
}

static std::string meta_file_from_tu(TranslationUnit *tu,
                                     const std::string &module_name) {
  std::ostringstream out;
  out << "MODULE_NAME " << module_name << "\n";

  for (auto &dptr : tu->decls) {
    if (auto f = dynamic_cast<FuncDecl *>(dptr.get())) {
      std::string ret = "void";
      if (f->return_type) {
        std::string rn = f->return_type->name;
        if (rn == "i32")
          ret = "int32_t";
        else if (rn == "i64")
          ret = "int64_t";
        else if (rn == "i16")
          ret = "int16_t";
        else if (rn == "i8")
          ret = "int8_t";
        else if (rn == "u32")
          ret = "uint32_t";
        else if (rn == "u64")
          ret = "uint64_t";
        else if (rn == "u16")
          ret = "uint16_t";
        else if (rn == "u8")
          ret = "uint8_t";
        else if (rn == "f32")
          ret = "float";
        else if (rn == "f64")
          ret = "double";
        else if (rn == "bool")
          ret = "bool";
        else if (rn == "char")
          ret = "char";
        else if (rn == "string")
          ret = "const char*";
        else if (rn == "void")
          ret = "void";
        else if (rn == "Array")
          ret = "risk_array_t*";
        else if (rn == "Hashmap")
          ret = "risk_hashmap_t*";
        else if (rn == "func")
          ret = "risk_closure_t*";
        else
          ret = module_name + "__" + rn;
      }

      std::string func_c_name =
          f->is_extern ? f->name : module_name + "__" + f->name;
      out << "FUNC " << ret << " " << func_c_name << "(";
      for (size_t i = 0; i < f->params.size(); ++i) {
        if (i)
          out << ", ";
        TypeNode *ptype = f->params[i].type.get();
        std::string pn = ptype ? ptype->name : "";
        std::string ctype;
        if (pn == "i32")
          ctype = "int32_t";
        else if (pn == "i64")
          ctype = "int64_t";
        else if (pn == "i16")
          ctype = "int16_t";
        else if (pn == "i8")
          ctype = "int8_t";
        else if (pn == "u32")
          ctype = "uint32_t";
        else if (pn == "u64")
          ctype = "uint64_t";
        else if (pn == "u16")
          ctype = "uint16_t";
        else if (pn == "u8")
          ctype = "uint8_t";
        else if (pn == "f32")
          ctype = "float";
        else if (pn == "f64")
          ctype = "double";
        else if (pn == "bool")
          ctype = "bool";
        else if (pn == "char")
          ctype = "char";
        else if (pn == "string")
          ctype = "const char*";
        else if (pn == "Array")
          ctype = "risk_array_t*";
        else if (pn == "Hashmap")
          ctype = "risk_hashmap_t*";
        else if (pn == "ptr" || pn == "ref")
          ctype = "void*";
        else if (!pn.empty())
          ctype = module_name + "__" + pn;
        else
          ctype = "void*";
        out << ctype << " " << f->params[i].name;
      }
      out << ");\n";
    }

    if (auto b = dynamic_cast<BlockDecl *>(dptr.get())) {
      std::string sname = module_name + "__" + b->name;
      out << "STRUCT typedef struct " << sname << " " << sname << ";\n";
      out << "STRUCTDEF struct " << sname << " {\n";
      for (auto &field : b->fields) {
        std::string ft = "void*";
        if (field.type) {
          std::string fn = field.type->name;
          if (fn == "i32")
            ft = "int32_t";
          else if (fn == "i64")
            ft = "int64_t";
          else if (fn == "i16")
            ft = "int16_t";
          else if (fn == "i8")
            ft = "int8_t";
          else if (fn == "u32")
            ft = "uint32_t";
          else if (fn == "u64")
            ft = "uint64_t";
          else if (fn == "u16")
            ft = "uint16_t";
          else if (fn == "u8")
            ft = "uint8_t";
          else if (fn == "f32")
            ft = "float";
          else if (fn == "f64")
            ft = "double";
          else if (fn == "bool")
            ft = "bool";
          else if (fn == "char")
            ft = "char";
          else if (fn == "string")
            ft = "const char*";
          else if (fn == "Array")
            ft = "risk_array_t*";
          else if (fn == "Hashmap")
            ft = "risk_hashmap_t*";
          else if (fn == "ptr" || fn == "ref")
            ft = "void*";
          else
            ft = module_name + "__" + fn;
        }
        out << "STRUCTFIELD     " << ft << " " << field.name << ";\n";
      }
      out << "STRUCTEND };\n";
    }

    if (auto opt = dynamic_cast<OptDecl *>(dptr.get())) {
      out << "ENUM typedef enum " << module_name << "__" << opt->name << " "
          << module_name << "__" << opt->name << ";\n";
    }
  }

  out << "END\n";
  return out.str();
}

static void print_diagnostic_with_context(const Diagnostic &d,
                                          const std::string &filename,
                                          const std::string &source) {
  std::string kind =
      (d.severity == Diagnostic::Severity::Error) ? "error" : "warning";
  std::string ANSI_SEQUENCE_DIAG =
      (d.severity == Diagnostic::Severity::Error)
          ? ANSI_CODE + std::string{ANSI_CODE_FG} +
                std::string{ANSI_COLORS::WHITE} + ";" +
                std::string{ANSI_CODE_BG} + std::string{ANSI_COLORS::RED} + "m"
          :

          ANSI_CODE + std::string{ANSI_CODE_FG} +
              std::string{ANSI_COLORS::WHITE} + ';' +
              std::string{ANSI_CODE_BG} + std::string{ANSI_COLORS::MAGENTA} +
              "m";
  int line_no = d.line;
  int col_no = d.col;

  std::string ANSI_FILENAME = ANSI_CODE + std::string{ANSI_CODE_FG} +
                              std::string{ANSI_COLORS::WHITE} + ";" + "4" +
                              std::string{ANSI_COLORS::BLUE} + "m";

  if (line_no <= 0 || col_no <= 0) {
    std::cerr << ANSI_FILENAME << filename << ":0:0" << ": "
              << ANSI_SEQUENCE_DIAG << kind << ANSI_SEQUENCE_RESET << ": "
              << d.message << "\n";

    return;
  }
  std::cerr << ANSI_FILENAME << filename << ":" << line_no << ":" << col_no
            << ": " << ANSI_SEQUENCE_DIAG << kind << ANSI_SEQUENCE_RESET << ": "
            << d.message << "\n";

  std::istringstream ss(source);
  std::string line;
  int lineno = 1;
  while (std::getline(ss, line)) {
    if (lineno == line_no) {
      std::cerr << "    " << line << "\n";

      int col = std::max(1, col_no);
      int end_col = (d.end_col > 0 ? d.end_col : col);
      if (end_col < col)
        end_col = col;

      int max_col = 4 + (int)line.size();
      if (end_col > max_col)
        end_col = max_col;

      std::string marker;
      marker.append(4, ' ');
      for (int i = 1; i < col; ++i) {
        char c = (i - 1 < (int)line.size()) ? line[i - 1] : ' ';
        marker.push_back((c == '\t') ? '\t' : ' ');
      }
      marker.push_back('^');
      for (int i = col + 1; i <= end_col; ++i)
        marker.push_back('~');

      std::cerr << "\033[" << ANSI_CODE_FG << ANSI_COLORS::RED << "m" << marker
                << "\n"
                << ANSI_SEQUENCE_RESET;
      break;
    }
    ++lineno;
  }
}

int main(int argc, char **argv) {

  std::string abs_input;
  {
#ifdef _WIN32
    char resolved[MAX_PATH];
    if (argc >= 2 && _fullpath(resolved, argv[1], MAX_PATH)) {
      abs_input = resolved;
    } else if (argc >= 2) {
      abs_input = argv[1];
    }
#else
    char resolved[PATH_MAX];
    if (argc >= 2 && realpath(argv[1], resolved)) {
      abs_input = resolved;
    } else if (argc >= 2) {
      abs_input = argv[1];
    }
#endif
  }

  std::string source_dir = abs_input;
  size_t last_slash = source_dir.find_last_of("/\\");
  if (last_slash != std::string::npos) {
    source_dir = source_dir.substr(0, last_slash + 1);
  } else {
    source_dir = "./";
  }
  const std::string COMPILE_DIR = source_dir;

  if (argc < 2) {
    std::cerr << "Usage: riskc <file.risk> [-o out] [-O0|-O1|-O2|-O3|-Os|-Og] "
                 "[-flto] [-c] [-g] [-i] [--profile] [--help] [--version]\n";
    std::cerr << "Try 'riskc --help' for more information.\n";
    return 1;
  }

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_help();
      return 0;
    } else if (arg == "--version" || arg == "-v") {
      print_version();
      return 0;
    }
  }

  std::string path = abs_input;
#ifdef _WIN32
  std::string out_exec = "a.exe";
#else
  std::string out_exec = "a.out";
#endif

  std::string opt_flags;

  bool compile_only = false;

  bool debug_info = false;

  bool keep_intermediate = false;

  bool user_set_out_exec = false;

  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if ((a == "-o" || a == "--out") && i + 1 < argc) {
      out_exec = argv[++i];
      user_set_out_exec = true;
    } else if (a == "--profile") {

    } else if (a == "-O0" || a == "-O1" || a == "-O2" || a == "-O3" ||
               a == "-Os" || a == "-Og") {
      opt_flags = " " + a;
    } else if (a == "-flto") {
      opt_flags += " -flto";
    } else if (a == "-Ofast") {
      std::cerr
          << "Error: -Ofast is not supported.\n"
          << "It enables -ffast-math, which assumes no value is ever NaN or "
             "Infinity. The C backend is then free to fold is_nan()/is_inf() "
             "and comparisons against INF/NAN literals into constants -- "
             "silently breaking those Risk language features rather than "
             "just changing float rounding behavior. Use -O3 instead.\n";
      return 1;
    } else if (a == "-Oz") {
      std::cerr << "Error: -Oz is not supported. It's a Clang-only flag and "
                   "this compiler's C backend is gcc, which has no "
                   "equivalent. Use -Os instead.\n";
      return 1;
    } else if (a.rfind("-march=", 0) == 0) {

      opt_flags += " " + a;
    } else if (a == "-c") {
      compile_only = true;
    } else if (a == "-g") {
      debug_info = true;
    } else if (a == "-i") {
      keep_intermediate = true;
    }
  }

  std::string src = read_file(path);
  if (src.empty()) {
    std::cerr << "Error: failed to read file: " << path << "\n";
    return 1;
  }

  Lexer lexer(src, path);
  auto tokens = lexer.tokenize();

  bool is_header = false;
  size_t first_non_comment = 0;
  for (size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i].type == TokenType::Comment)
      continue;
    first_non_comment = i;
    if (tokens[i].type == TokenType::HeaderFileMarker)
      is_header = true;
    break;
  }

  if (is_header) {
    tokens.erase(tokens.begin() + first_non_comment);
  }

  Parser parser(tokens);
  auto tu = parser.parse_translation_unit();

  if (!tu) {
    std::cerr << "parse failed\n";
    return 1;
  }

  if (!parser.diags.empty()) {
    bool has_error = false;

    for (auto &d : parser.diags) {
      if (d.severity == Diagnostic::Severity::Error)
        has_error = true;
    }
    for (auto &d : parser.diags) {
      print_diagnostic_with_context(d, path, src);
    }
    if (has_error)
      return 1;
  }
  tu->filename = path;

  {
    std::string self_exe = get_self_exe_path(argv[0]);
    if (!ensure_all_header_deps_built(tu.get(), source_dir, self_exe)) {
      return 1;
    }
  }

  if (is_header) {

    std::string module_name = module_name_from_path(path);

    std::string risk_c_h_path = module_name + ".h";
    std::string risk_c_header =
        c_header_from_tu(tu.get(), module_name_from_path(path));

    if (!write_file(risk_c_h_path, risk_c_header)) {
      std::cerr << "failed to write temporary C header: " << risk_c_h_path
                << "\n";
      return 1;
    }

    std::string c_impl_path = COMPILE_DIR + module_name + "_risk.c";

    TypeChecker tc;
    auto diags = tc.check(tu.get());

    if (!diags.empty()) {
      bool has_error = false;
      for (auto &d : diags)
        if (d.severity == Diagnostic::Severity::Error)
          has_error = true;
      for (auto &d : diags) {
        print_diagnostic_with_context(d, path, src);
      }
      if (has_error) {
        remove_file(risk_c_h_path);
        return 1;
      }
    }

    CodeGen cg(tu.get(), c_impl_path, module_name, &tc.compile_time_string_map,
               &tc.expr_type_map, &tc.type_aliases, debug_info);
    if (!cg.generate()) {
      std::cerr << "failed to generate C implementation\n";
      remove_file(risk_c_h_path);
      remove_file(c_impl_path);
      return 1;
    }

    std::string obj_path = COMPILE_DIR + module_name + ".o";

    std::string compile_cmd = "gcc -c -w -std=c99" + opt_flags +
                              (debug_info ? " -g" : "") + " \"" + c_impl_path +
                              "\" -o \"" + obj_path + "\"";
    std::cout << "-------------------------------------------------------------"
                 "---\n";
    int obj_rc = std::system(compile_cmd.c_str());
    if (obj_rc != 0) {
      std::cerr << "Failed to compile header implementation to object file\n";
      remove_file(risk_c_h_path);
      remove_file(c_impl_path);
      return obj_rc;
    }

    std::string riskh_path = COMPILE_DIR + module_name + ".riskh";
    std::string riskh_content = meta_file_from_tu(tu.get(), module_name);
    if (!write_file(riskh_path, riskh_content)) {
      std::cerr << "Failed to write riskh file: " << riskh_path << "\n";
      return 1;
    }

    std::cout << "Created object file: " << obj_path << "\n";
    std::cout << "Created header file: " << riskh_path << "\n";

    if (!keep_intermediate) {
      remove_file(c_impl_path);
      remove_file(risk_c_h_path);
    } else {
      std::cout << "Kept intermediate files: " << c_impl_path << " & "
                << risk_c_h_path << "\n";
    }

    std::cout << "-------------------------------------------------------------"
                 "---\n";

    std::cout << ANSI_CODE << ANSI_CODE_FG << ANSI_COLORS::WHITE << ";"
              << ANSI_CODE_BG << ANSI_COLORS::GREEN << "m"
              << "Header Compiled:" << obj_path << " & " << riskh_path
              << ANSI_SEQUENCE_RESET << "\n";
    return 0;
  }

  TypeChecker tc;
  auto diags = tc.check(tu.get());

  if (!diags.empty()) {
    bool has_error = false;
    for (auto &d : diags)
      if (d.severity == Diagnostic::Severity::Error)
        has_error = true;
    for (auto &d : diags) {
      print_diagnostic_with_context(d, path, src);
    }
    if (has_error)
      return 1;
  }

  std::string module_c_name = path;
  if (last_slash != std::string::npos) {
    module_c_name = module_c_name.substr(last_slash + 1);
  }
  if (module_c_name.size() >= 5 &&
      module_c_name.substr(module_c_name.size() - 5) == ".risk") {
    module_c_name = module_c_name.substr(0, module_c_name.size() - 5);
  }

  std::string out_c = COMPILE_DIR + module_c_name + ".c";

  for (auto &dptr : tu->decls) {
    if (auto im = dynamic_cast<ImportDecl *>(dptr.get())) {
      std::string header = im->path;
      if (!header.empty() && header.front() == '"')
        header.erase(0, 1);
      if (!header.empty() && header.back() == '"')
        header.pop_back();
      bool pre_is_stdlib = (!header.empty() && header.front() == '@');
      if (pre_is_stdlib)
        header.erase(0, 1);
      if (header.size() >= 5 && header.substr(header.size() - 5) == ".risk") {
        std::string base = header.substr(0, header.size() - 5);
        std::string riskh_file = pre_is_stdlib
                                     ? resolve_stdlib_file(base, ".riskh")
                                     : COMPILE_DIR + base + ".riskh";
        std::ifstream pre_in(riskh_file);
        if (pre_in.good()) {
          std::string imp_module;
          std::string line;
          while (std::getline(pre_in, line)) {
            if (line.rfind("MODULE_NAME ", 0) == 0) {
              imp_module = line.substr(12);
              continue;
            }
            if (line == "END")
              break;

            if (!imp_module.empty() && line.rfind("STRUCT ", 0) == 0) {

              std::string prefix = imp_module + "__";
              std::string mangled = imp_module + "__";

              std::string decl = line.substr(7);

              size_t ts = decl.find(prefix);
              if (ts != std::string::npos) {
                size_t name_start = ts + prefix.size();
                size_t name_end = decl.find(' ', name_start);
                if (name_end == std::string::npos)
                  name_end = decl.find(';', name_start);
                if (name_end == std::string::npos)
                  name_end = decl.size();
                std::string short_name =
                    decl.substr(name_start, name_end - name_start);
                std::string full_name = imp_module + "__" + short_name;

                if (tc.type_aliases.find(short_name) == tc.type_aliases.end()) {
                  tc.type_aliases[short_name] = full_name;
                }
              }
            }
          }
        }
      }
    }
  }

  CodeGen cg(tu.get(), out_c, "mod", &tc.compile_time_string_map,
             &tc.expr_type_map, &tc.type_aliases, debug_info);
  if (!cg.generate()) {
    std::cerr << "codegen failed\n";
    remove_file(out_c);
    return 1;
  }

  std::string c_contents = read_file(out_c);

  size_t insert_pos =
      c_contents.find("typedef struct risk_hashmap_t risk_hashmap_t;");
  if (insert_pos != std::string::npos) {

    insert_pos = c_contents.find('\n', insert_pos);
    if (insert_pos != std::string::npos) {
      insert_pos++;

      while (insert_pos < c_contents.size() && c_contents[insert_pos] == '\n') {
        insert_pos++;
      }
    }
  } else {

    insert_pos = 0;
  }

  std::ostringstream meta_decls;
  std::vector<std::string> link_files;
  std::unordered_set<std::string> visited_objs;

  for (auto &dptr : tu->decls) {
    if (auto im = dynamic_cast<ImportDecl *>(dptr.get())) {
      std::string p = im->path;
      std::string header = p;

      if (!header.empty() && header.front() == '"')
        header.erase(0, 1);
      if (!header.empty() && header.back() == '"')
        header.pop_back();
      bool main_is_stdlib = (!header.empty() && header.front() == '@');
      if (main_is_stdlib)
        header.erase(0, 1);
      if (header.size() >= 5 && header.substr(header.size() - 5) == ".risk") {
        std::string base = header.substr(0, header.size() - 5);
        std::string resolved_base =
            main_is_stdlib ? get_stdlib_base() : COMPILE_DIR;

        std::string riskh_file = main_is_stdlib
                                     ? resolve_stdlib_file(base, ".riskh")
                                     : resolved_base + base + ".riskh";
        std::ifstream riskh_in(riskh_file);

        if (riskh_in.good()) {

          meta_decls << "// Declarations from " << riskh_file << "\n";
          std::string line;
          bool in_meta = false;
          while (std::getline(riskh_in, line)) {
            if (line.rfind("MODULE_NAME ", 0) == 0) {
              in_meta = true;
              continue;
            }
            if (line == "END") {
              break;
            }
            if (in_meta && !line.empty()) {

              if (line.rfind("FUNC ", 0) == 0) {
                meta_decls << line.substr(5) << "\n";
              } else if (line.rfind("STRUCTDEF ", 0) == 0) {
                meta_decls << line.substr(10) << "\n";
              } else if (line.rfind("STRUCTFIELD ", 0) == 0) {
                meta_decls << line.substr(12) << "\n";
              } else if (line.rfind("STRUCTEND ", 0) == 0) {
                meta_decls << line.substr(10) << "\n";
              } else if (line.rfind("STRUCT ", 0) == 0) {
                meta_decls << line.substr(7) << "\n";
              } else if (line.rfind("ENUM ", 0) == 0) {
                meta_decls << line.substr(5) << "\n";
              }
            }
          }
          meta_decls << "\n";
          riskh_in.close();

          std::string obj_file = main_is_stdlib
                                     ? resolve_stdlib_file(base, ".o")
                                     : resolved_base + base + ".o";
          if (visited_objs.find(obj_file) == visited_objs.end()) {
            visited_objs.insert(obj_file);
            link_files.push_back(obj_file);
          }

          std::string risk_path = main_is_stdlib
                                      ? resolve_stdlib_file(base, ".risk")
                                      : resolved_base + header;
          if (path_is_regular_file_driver(risk_path)) {
            collect_transitive_header_objs(risk_path, COMPILE_DIR, link_files,
                                           visited_objs);
          }
        } else {
          std::cerr << "Warning: Could not find riskh file: " << riskh_file
                    << "\n";
        }
      }
    }
  }

  std::string final_c;
  if (insert_pos > 0 && insert_pos < c_contents.size()) {
    final_c = c_contents.substr(0, insert_pos) + meta_decls.str() +
              c_contents.substr(insert_pos);
  } else {
    final_c = meta_decls.str() + c_contents;
  }

  if (!write_file(out_c, final_c)) {
    std::cerr << "Failed to write final C file with includes: " << out_c
              << "\n";
    remove_file(out_c);
    return 1;
  }

  std::string cmd = "gcc -w -std=c99" + opt_flags + (debug_info ? " -g" : "") +
                    " -I\"" + source_dir + "\"";
  {
    std::unordered_set<std::string> seen_dirs{source_dir};
    for (const auto &dir : tc.imported_dirs) {
      if (seen_dirs.insert(dir).second)
        cmd += " -I\"" + dir + "\"";
    }
  }

  if (compile_only) {

    std::string obj_out = user_set_out_exec ? out_exec : (module_c_name + ".o");
    cmd += " -c -o\"" + obj_out + "\" \"" + out_c + "\"";

    std::cout << "\n-----------------------------------------------------------"
                 "-----\n";
    loadingAnim("\rDone Compiling!");
    std::cout << "Wrote Intermediate File: " << out_c
              << "\n-----------------------------------------------------------"
                 "-----\n";

    int rc = std::system(cmd.c_str());
    if (!keep_intermediate)
      remove_file(out_c);
    if (rc != 0) {
      std::cerr << ANSI_CODE << ANSI_CODE_FG << ANSI_COLORS::WHITE << ";"
                << ANSI_CODE_BG << ANSI_COLORS::RED << "m"
                << "Compilation_Error_Code " << rc << ANSI_SEQUENCE_RESET
                << "\n";
      return rc;
    }
    std::cout << ANSI_CODE << ANSI_CODE_FG << ANSI_COLORS::WHITE << ";"
              << ANSI_CODE_BG << ANSI_COLORS::GREEN << "m"
              << "Built object file: " << obj_out << ANSI_SEQUENCE_RESET
              << "\n";
    return 0;
  }

  cmd += " -o\"" + out_exec + "\" \"" + out_c + "\"";
  for (const auto &src : tc.extern_link_sources) {
    cmd += " \"" + src + "\"";
  }
  for (const auto &obj_file : link_files) {
    cmd += " " + obj_file;
  }

  for (const auto &lib : tc.extern_link_libs) {
    cmd += " -l" + lib;
  }
  cmd += " -lm";
  std::cout << "\n-------------------------------------------------------------"
               "---\n";

  loadingAnim("\rDone Compiling!");
  std::cout
      << "Wrote Intermediate File: " << out_c
      << "\n----------------------------------------------------------------\n";

  int rc = std::system(cmd.c_str());

  if (!keep_intermediate)
    remove_file(out_c);
  if (rc != 0) {
    std::cerr << ANSI_CODE << ANSI_CODE_FG << ANSI_COLORS::WHITE << ";"
              << ANSI_CODE_BG << ANSI_COLORS::RED << "m"
              << "Compilation_Error_Code " << rc << ANSI_SEQUENCE_RESET << "\n";
    return rc;
  }
  std::cout << ANSI_CODE << ANSI_CODE_FG << ANSI_COLORS::WHITE << ";"
            << ANSI_CODE_BG << ANSI_COLORS::GREEN << "m"
            << "Built executable: " << out_exec << ANSI_SEQUENCE_RESET << "\n";

  return 0;
}
