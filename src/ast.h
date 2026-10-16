#pragma once

#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct TypeNode;
using TypePtr = std::unique_ptr<TypeNode>;

struct Expr;

struct Stmt;

struct Decl;

using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;
using DeclPtr = std::unique_ptr<Decl>;

using StmtList = std::vector<StmtPtr>;

using DeclList = std::vector<DeclPtr>;

struct Node {

  virtual ~Node() = default;

  virtual void print(int indent = 0) const = 0;

protected:
  static void print_indent(int n) {
    for (int i = 0; i < n; ++i)
      std::cout << ' ';
  }
};

struct TypeNode : Node {

  std::string name;

  std::vector<TypePtr> params;

  TypePtr return_type;

  ExprPtr size_expr;

  bool is_this_param = false;

  TypeNode() = default;

  explicit TypeNode(std::string n) : name(std::move(n)), return_type(nullptr) {}

  void print(int indent = 0) const override;
};

struct Expr : Node {
  int line = 0;
  int col = 0;
  int end_col = 0;

  virtual ~Expr() = default;
};

struct IdentifierExpr : Expr {
  std::string name;

  IdentifierExpr(std::string n) : name(std::move(n)) {}

  void print(int indent = 0) const override;
};

struct NumberExpr : Expr {
  std::string literal;

  NumberExpr(std::string lit) : literal(std::move(lit)) {}

  void print(int indent = 0) const override;
};

struct StringExpr : Expr {
  std::string literal;

  StringExpr(std::string lit) : literal(std::move(lit)) {}

  void print(int indent = 0) const override;
};

struct CharExpr : Expr {
  std::string literal;

  CharExpr(std::string lit) : literal(std::move(lit)) {}

  void print(int indent = 0) const override;
};

struct BoolExpr : Expr {
  bool value;

  BoolExpr(bool v) : value(v) {}

  void print(int indent = 0) const override;
};

struct NullExpr : Expr {
  NullExpr() = default;

  void print(int indent = 0) const override;
};

struct UnaryExpr : Expr {
  std::string op;
  ExprPtr rhs;
  bool is_postfix = false;

  UnaryExpr(std::string o, ExprPtr r)
      : op(std::move(o)), rhs(std::move(r)), is_postfix(false) {}

  void print(int indent = 0) const override;
};

struct BinaryExpr : Expr {
  ExprPtr lhs;
  std::string op;
  ExprPtr rhs;

  BinaryExpr(ExprPtr l, std::string o, ExprPtr r)
      : lhs(std::move(l)), op(std::move(o)), rhs(std::move(r)) {}

  void print(int indent = 0) const override;
};

struct CallExpr : Expr {
  ExprPtr callee;
  std::vector<ExprPtr> args;

  CallExpr(ExprPtr c) : callee(std::move(c)) {}

  void print(int indent = 0) const override;
};

struct CastExpr : Expr {
  ExprPtr expr;
  TypePtr target;
  bool is_unsafe = false;

  CastExpr(ExprPtr e, TypePtr t)
      : expr(std::move(e)), target(std::move(t)), is_unsafe(false) {}

  void print(int indent = 0) const override;
};

struct LambdaExpr : Expr {
  std::vector<std::pair<std::string, TypePtr>> params;
  TypePtr return_type;
  StmtList body;

  std::vector<std::string> captures;
  std::vector<std::string> capture_type_names;

  LambdaExpr() = default;

  void print(int indent = 0) const override;
};

struct MemberExpr : Expr {
  ExprPtr object;
  std::string field;

  MemberExpr(ExprPtr obj, std::string f)
      : object(std::move(obj)), field(std::move(f)) {}

  void print(int indent = 0) const override;
};

struct BlockLiteralExpr : Expr {
  std::vector<ExprPtr> positional;
  std::vector<std::pair<std::string, ExprPtr>> named;

  BlockLiteralExpr() = default;

  void print(int indent = 0) const override;
};

struct ArrayLiteralExpr : Expr {
  std::vector<ExprPtr> elements;

  ArrayLiteralExpr() = default;

  void print(int indent = 0) const override;
};

struct IndexExpr : Expr {
  ExprPtr object;
  ExprPtr index;

  IndexExpr(ExprPtr o, ExprPtr i) : object(std::move(o)), index(std::move(i)) {}

  void print(int indent = 0) const override;
};

struct SizeofExpr : Expr {
  TypePtr type_arg;
  ExprPtr expr_arg;

  explicit SizeofExpr(TypePtr t) : type_arg(std::move(t)), expr_arg(nullptr) {}

  explicit SizeofExpr(ExprPtr e) : type_arg(nullptr), expr_arg(std::move(e)) {}

  SizeofExpr() = default;

  void print(int indent = 0) const override;
};

struct Stmt : Node {
  int line = 0;
  int col = 0;

  virtual ~Stmt() = default;
};

struct ExprStmt : Stmt {
  ExprPtr expr;

  ExprStmt(ExprPtr e) : expr(std::move(e)) {}

  void print(int indent = 0) const override;
};

struct ReturnStmt : Stmt {
  ExprPtr expr;

  ReturnStmt(ExprPtr e) : expr(std::move(e)) {}

  void print(int indent = 0) const override;
};

struct VarDeclStmt : Stmt {
  std::string name;
  TypePtr type;
  ExprPtr init;
  bool is_const;

  VarDeclStmt(std::string n, TypePtr t, ExprPtr i, bool c)
      : name(std::move(n)), type(std::move(t)), init(std::move(i)),
        is_const(c) {}

  void print(int indent = 0) const override;
};

struct RegisterDecl : Stmt {
  std::string name;
  TypePtr element_type;
  uint32_t address;

  RegisterDecl(std::string n, TypePtr t, uint32_t addr)
      : name(std::move(n)), element_type(std::move(t)), address(addr) {}

  void print(int indent = 0) const override;
};

struct BlockStmt : Stmt {
  StmtList stmts;

  bool is_multi_var_expansion = false;

  void print(int indent = 0) const override;
};

struct IfStmt : Stmt {
  ExprPtr cond;
  StmtPtr then_branch;
  std::optional<StmtPtr> else_branch;

  IfStmt(ExprPtr c, StmtPtr t)
      : cond(std::move(c)), then_branch(std::move(t)) {}

  void print(int indent = 0) const override;
};

struct WhileStmt : Stmt {
  ExprPtr cond;
  StmtPtr body;

  WhileStmt(ExprPtr c, StmtPtr b) : cond(std::move(c)), body(std::move(b)) {}

  void print(int indent = 0) const override;
};

struct ForStmt : Stmt {
  std::optional<StmtPtr> init;
  std::optional<ExprPtr> cond;
  std::optional<StmtPtr> post;
  StmtPtr body;

  void print(int indent = 0) const override;
};

struct BreakStmt : Stmt {
  void print(int indent = 0) const override;
};

struct ContinueStmt : Stmt {
  void print(int indent = 0) const override;
};

struct DeferStmt : Stmt {
  StmtPtr body;

  DeferStmt(StmtPtr b) : body(std::move(b)) {}

  void print(int indent = 0) const override;
};

inline void DeferStmt::print(int indent) const {
  print_indent(indent);
  std::cout << "Defer:\n";
  if (body)
    body->print(indent + 2);
}

inline void BreakStmt::print(int indent) const {
  print_indent(indent);
  std::cout << "Break\n";
}

inline void ContinueStmt::print(int indent) const {
  print_indent(indent);
  std::cout << "Continue\n";
}

struct Decl : Node {
  int line = 0;
  int col = 0;

  virtual ~Decl() = default;
};

struct FuncParam {
  std::string name;
  TypePtr type;
  ExprPtr default_val;
  bool noalias = false;
};

enum class GenericParamKind { Type, ValueInt, ValueOpt };

struct GenericParam {
  std::string name;
  std::string constraint_set;
  bool is_value = false;
  GenericParamKind kind = GenericParamKind::Type;
};

struct FuncDecl : Decl {
  std::string name;
  std::vector<FuncParam> params;
  TypePtr return_type;
  bool is_definition = false;
  StmtList body;
  std::string source_file;

  std::vector<GenericParam> generic_params;

  bool is_extern = false;
  std::string extern_header;

  std::string extern_search_dir;

  std::vector<std::string> extern_link_libs;

  FuncDecl(std::string n) : name(std::move(n)) {}

  void print(int indent = 0) const override;
};

struct TypeAliasDecl : Decl {
  std::string name;
  TypePtr target_type;

  TypeAliasDecl(std::string n, TypePtr t)
      : name(std::move(n)), target_type(std::move(t)) {}

  void print(int indent = 0) const override;
};

struct GenericSetDecl : Decl {
  std::string name;
  std::vector<TypePtr> members;

  GenericSetDecl(std::string n) : name(std::move(n)) {}

  void print(int indent = 0) const override;
};

struct GlobalVarDecl : Decl {
  std::unique_ptr<VarDeclStmt> var;

  GlobalVarDecl(std::unique_ptr<VarDeclStmt> v) : var(std::move(v)) {}

  bool is_extern = false;
  std::string extern_header;
  std::string extern_search_dir;

  void print(int indent = 0) const override {
    print_indent(indent);
    std::cout << "Global";
    if (var)
      var->print(0);
  }
};

struct BlockField {
  std::string name;
  TypePtr type;
  ExprPtr default_val;

  BlockField(std::string n, TypePtr t)
      : name(std::move(n)), type(std::move(t)) {}
};

struct OptField {
  std::string name;
  std::optional<int> value;

  OptField(std::string n, std::optional<int> v = std::nullopt)
      : name(std::move(n)), value(v) {}
};

struct BlockDecl : Decl {
  std::string name;
  std::vector<BlockField> fields;

  std::vector<GenericParam> generic_params;

  BlockDecl(std::string n) : name(std::move(n)) {}

  bool is_extern = false;
  std::string extern_header;
  std::string extern_search_dir;

  std::string extern_c_spelling;

  void print(int indent = 0) const override;
};

struct OptDecl : Decl {
  std::string name;
  std::vector<OptField> fields;

  OptDecl(std::string n) : name(std::move(n)) {}

  bool is_extern = false;
  std::string extern_header;
  std::string extern_search_dir;

  void print(int indent = 0) const override;
};

struct SwitchCase {
  std::optional<ExprPtr> value;
  StmtList stmts;

  SwitchCase() = default;
};

struct SwitchStmt : Stmt {
  ExprPtr expr;
  std::vector<SwitchCase> cases;

  void print(int indent = 0) const override;
};

struct ImportDecl : Decl {
  std::string path;
  std::string alias;

  explicit ImportDecl(std::string p) : path(std::move(p)), alias("") {}

  void print(int indent = 0) const override;
};

struct TranslationUnit : Node {
  DeclList decls;
  std::string filename;

  void print(int indent = 0) const override;
};

inline void TypeNode::print(int indent) const {
  print_indent(indent);
  std::cout << "Type: " << (size_expr ? "<computed size expr>" : name);
  if (!params.empty()) {
    std::cout << "<";
    for (size_t i = 0; i < params.size(); ++i) {
      if (i)
        std::cout << ", ";
      std::cout << (params[i]->size_expr ? "<computed size expr>"
                                         : params[i]->name);
    }
    std::cout << ">";
  }
  if (return_type) {
    std::cout << " -> ";
    return_type->print(0);
  }
  std::cout << "\n";
}

inline void IdentifierExpr::print(int indent) const {
  print_indent(indent);
  std::cout << "Ident: " << name << "\n";
}

inline void NumberExpr::print(int indent) const {
  print_indent(indent);
  std::cout << "Number: " << literal << "\n";
}

inline void StringExpr::print(int indent) const {
  print_indent(indent);
  std::cout << "String: " << literal << "\n";
}

inline void CharExpr::print(int indent) const {
  print_indent(indent);
  std::cout << "Char: " << literal << "\n";
}

inline void BoolExpr::print(int indent) const {
  print_indent(indent);
  std::cout << "Bool: " << (value ? "true" : "false") << "\n";
}

inline void NullExpr::print(int indent) const {
  print_indent(indent);
  std::cout << "Null\n";
}

inline void UnaryExpr::print(int indent) const {
  print_indent(indent);
  std::cout << "Unary: " << op << (is_postfix ? " (postfix)" : " (prefix)")
            << "\n";
  if (rhs)
    rhs->print(indent + 2);
}

inline void BinaryExpr::print(int indent) const {
  print_indent(indent);
  std::cout << "Binary: " << op << "\n";
  if (lhs)
    lhs->print(indent + 2);
  if (rhs)
    rhs->print(indent + 2);
}

inline void CallExpr::print(int indent) const {
  print_indent(indent);
  std::cout << "Call:\n";
  if (callee)
    callee->print(indent + 2);
  for (auto &a : args)
    if (a)
      a->print(indent + 2);
}

inline void CastExpr::print(int indent) const {
  print_indent(indent);
  std::cout << "Cast" << (is_unsafe ? " (unsafe)" : "") << ":\n";
  if (expr) {
    print_indent(indent + 2);
    std::cout << "Expr:\n";
    expr->print(indent + 4);
  }
  if (target) {
    print_indent(indent + 2);
    std::cout << "To: ";
    target->print(0);
  }
}

inline void LambdaExpr::print(int indent) const {
  print_indent(indent);
  std::cout << "Lambda:\n";
  print_indent(indent + 2);
  std::cout << "Params:\n";
  for (auto &p : params) {
    print_indent(indent + 4);
    std::cout << p.first << " : ";
    if (p.second)
      p.second->print(0);
    else
      std::cout << "(none)\n";
  }
  if (!captures.empty()) {
    print_indent(indent + 2);
    std::cout << "Captures: ";
    for (size_t i = 0; i < captures.size(); ++i) {
      if (i)
        std::cout << ", ";
      std::cout << captures[i] << ":"
                << (i < capture_type_names.size() ? capture_type_names[i]
                                                  : "unknown");
    }
    std::cout << "\n";
  }
  print_indent(indent + 2);
  std::cout << "Body:\n";
  for (auto &s : body)
    if (s)
      s->print(indent + 4);
}

inline void MemberExpr::print(int indent) const {
  print_indent(indent);
  std::cout << "Member: " << field << "\n";
  if (object)
    object->print(indent + 2);
}

inline void BlockLiteralExpr::print(int indent) const {
  print_indent(indent);
  std::cout << "BlockLiteral:\n";
  if (!positional.empty()) {
    print_indent(indent + 2);
    std::cout << "Positional:\n";
    for (auto &p : positional)
      if (p)
        p->print(indent + 4);
  }
  if (!named.empty()) {
    print_indent(indent + 2);
    std::cout << "Named:\n";
    for (auto &n : named) {
      print_indent(indent + 4);
      std::cout << n.first << ":\n";
      if (n.second)
        n.second->print(indent + 6);
    }
  }
}

inline void ArrayLiteralExpr::print(int indent) const {
  print_indent(indent);
  std::cout << "ArrayLiteral:\n";
  for (auto &e : elements)
    if (e)
      e->print(indent + 2);
}

inline void IndexExpr::print(int indent) const {
  print_indent(indent);
  std::cout << "IndexExpr:\n";
  if (object) {
    print_indent(indent + 2);
    std::cout << "Object:\n";
    object->print(indent + 4);
  }
  if (index) {
    print_indent(indent + 2);
    std::cout << "Index:\n";
    index->print(indent + 4);
  }
}

inline void SizeofExpr::print(int indent) const {
  print_indent(indent);
  std::cout << "SizeofExpr:\n";
  if (type_arg) {
    print_indent(indent + 2);
    std::cout << "Type: ";
    type_arg->print(0);
  }
  if (expr_arg) {
    print_indent(indent + 2);
    std::cout << "Expr:\n";
    expr_arg->print(indent + 4);
  }
}

inline void ExprStmt::print(int indent) const {
  print_indent(indent);
  std::cout << "ExprStmt:\n";
  if (expr)
    expr->print(indent + 2);
}

inline void ReturnStmt::print(int indent) const {
  print_indent(indent);
  std::cout << "Return:\n";
  if (expr)
    expr->print(indent + 2);
}

inline void VarDeclStmt::print(int indent) const {
  print_indent(indent);
  std::cout << (is_const ? "ConstVarDecl: " : "VarDecl: ") << name << "\n";
  if (type)
    type->print(indent + 2);
  if (init) {
    print_indent(indent + 2);
    std::cout << "Init:\n";
    init->print(indent + 4);
  }
}

inline void BlockStmt::print(int indent) const {
  print_indent(indent);
  std::cout << "Block:\n";
  for (auto &s : stmts)
    if (s)
      s->print(indent + 2);
}

inline void IfStmt::print(int indent) const {
  print_indent(indent);
  std::cout << "If:\n";
  print_indent(indent + 2);
  std::cout << "Cond:\n";
  if (cond)
    cond->print(indent + 4);
  print_indent(indent + 2);
  std::cout << "Then:\n";
  if (then_branch)
    then_branch->print(indent + 4);
  if (else_branch) {
    print_indent(indent + 2);
    std::cout << "Else:\n";
    (*else_branch)->print(indent + 4);
  }
}

inline void WhileStmt::print(int indent) const {
  print_indent(indent);
  std::cout << "While:\n";
  if (cond)
    cond->print(indent + 2);
  if (body)
    body->print(indent + 2);
}

inline void SwitchStmt::print(int indent) const {
  print_indent(indent);
  std::cout << "Switch:\n";
  if (expr) {
    print_indent(indent + 2);
    std::cout << "Expr:\n";
    expr->print(indent + 4);
  }

  print_indent(indent + 2);
  std::cout << "Cases:\n";
  for (const auto &c : cases) {
    if (c.value) {
      print_indent(indent + 4);
      std::cout << "Case:\n";
      c.value.value()->print(indent + 6);
    } else {
      print_indent(indent + 4);
      std::cout << "Default:\n";
    }

    for (const auto &s : c.stmts) {
      if (s)
        s->print(indent + 6);
    }
  }
}

inline void ForStmt::print(int indent) const {
  print_indent(indent);
  std::cout << "For:\n";
  if (init) {
    print_indent(indent + 2);
    std::cout << "Init:\n";
    (*init)->print(indent + 4);
  }
  if (cond) {
    print_indent(indent + 2);
    std::cout << "Cond:\n";
    (*cond)->print(indent + 4);
  }
  if (post) {
    print_indent(indent + 2);
    std::cout << "Post:\n";
    (*post)->print(indent + 4);
  }
  print_indent(indent + 2);
  std::cout << "Body:\n";
  if (body)
    body->print(indent + 4);
}

inline void FuncDecl::print(int indent) const {
  print_indent(indent);
  std::cout << "Func: " << name << (is_definition ? " (def)\n" : " (decl)\n");
  if (!generic_params.empty()) {
    print_indent(indent + 2);
    std::cout << "GenericParams: ";
    for (size_t i = 0; i < generic_params.size(); ++i) {
      if (i)
        std::cout << ", ";
      if (generic_params[i].is_value)
        std::cout << "const ";
      std::cout << generic_params[i].name << ": "
                << generic_params[i].constraint_set;
    }
    std::cout << "\n";
  }
  print_indent(indent + 2);
  std::cout << "Params:\n";
  for (auto &p : params) {
    print_indent(indent + 4);
    std::cout << p.name << " : ";
    if (p.type)
      p.type->print(0);
    else
      std::cout << "(none)\n";
  }
  print_indent(indent + 2);
  std::cout << "Return:\n";
  if (return_type)
    return_type->print(indent + 4);
  if (is_definition) {
    print_indent(indent + 2);
    std::cout << "Body:\n";
    for (auto &s : body)
      if (s)
        s->print(indent + 4);
  }
}

inline void TypeAliasDecl::print(int indent) const {
  print_indent(indent);
  std::cout << "TypeAlias: " << name << " = ";
  if (target_type) {
    target_type->print(0);
  } else {
    std::cout << "(none)\n";
  }
}

inline void GenericSetDecl::print(int indent) const {
  print_indent(indent);
  std::cout << "GenericSet: " << name << " = [";
  for (size_t i = 0; i < members.size(); ++i) {
    if (i)
      std::cout << ", ";
    if (members[i])
      std::cout << members[i]->name;
  }
  std::cout << "]\n";
}

inline void BlockDecl::print(int indent) const {
  print_indent(indent);
  std::cout << "Block: " << name;
  if (!generic_params.empty()) {
    std::cout << "<";
    for (size_t i = 0; i < generic_params.size(); ++i) {
      if (i)
        std::cout << ", ";
      std::cout << generic_params[i].name << ": "
                << generic_params[i].constraint_set;
    }
    std::cout << ">";
  }
  std::cout << "\n";
  for (auto &f : fields) {
    print_indent(indent + 2);
    std::cout << f.name << " : ";
    if (f.type)
      f.type->print(0);
    else
      std::cout << "(none)\n";
  }
}
inline void OptDecl::print(int indent) const {
  print_indent(indent);
  std::cout << "Opt: " << name << "\n";
  for (auto &f : fields) {
    print_indent(indent + 2);
    std::cout << f.name;
    if (f.value) {
      std::cout << " = " << *f.value;
    }
    std::cout << "\n";
  }
}
inline void ImportDecl::print(int indent) const {
  print_indent(indent);
  std::cout << "Import: \"" << path << "\"\n";
}

inline void TranslationUnit::print(int indent) const {
  std::cout << "TranslationUnit (" << filename << "):\n";
  for (auto &d : decls)
    if (d)
      d->print(2);
}
inline void RegisterDecl::print(int indent) const {
  print_indent(indent);
  std::cout << "Register: " << name << " at 0x" << std::hex << address
            << std::dec;
  if (element_type) {
    std::cout << " : ";
    element_type->print(0);
  } else {
    std::cout << "\n";
  }
}
