
// Sixgill: Static assertion checker for C/C++ programs.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#pragma once

// clang front end for sixgill. This translates the Clang AST of each function
// and global variable into sixgill's CFG representation through the XIL_*
// interface, the same interface used by the gcc plugin (gcc/*.c). The
// translation is modeled on gcc/tree.c so that the output has the same shape
// as the gcc plugin's: see local/clang-port-plan.md for the overall design.

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/GlobalDecl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/Stmt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"

#include <deque>
#include <string>
#include <vector>

#include <imlang/interface.h>

namespace clang {
class Sema;
}

namespace xgill {

// Settings from the plugin arguments and environment, shared by all
// translation units.
struct PluginOptions {
  std::string remote;
  std::string log;
  std::string basedir;
  std::string command;
};

// Post side effects of an expression (eg the increment in x++) are hung off a
// chain of points that is spliced in after the containing statement. This
// matches XIL_PostEdges in gcc/xgill.h.
struct PostEdges {
  XIL_PPoint start = 0;
  XIL_PPoint end = 0;
};

// How the result of translating an expression is used. At most one of
// branch/assign/lval/rval is set; if none are, the result is discarded. This
// matches XIL_TreeEnv in gcc/xgill.h.
struct Env {
  // Point receiving side effects. Updated as side effects are generated, and
  // set to zero if the code after the expression is unreachable.
  XIL_PPoint *point;

  // Where to hang post side effects, or null to use temporaries instead.
  PostEdges *post;

  // Whether the result fields below have been used.
  bool processed = false;

  // Branch to truePoint/falsePoint according to the value.
  bool branch = false;
  XIL_PPoint truePoint = 0;
  XIL_PPoint falsePoint = 0;

  // Assign the value to this lvalue.
  XIL_Exp assign = nullptr;
  XIL_Type assignType = nullptr;

  // Receive the lvalue (address) or rvalue of the expression.
  XIL_Exp *lval = nullptr;
  XIL_Exp *rval = nullptr;

  // For C++ objects constructed into assign, whether the object is a
  // temporary that needs to be destroyed, and when.
  enum Destroy { DestroyNone, DestroyFullExpr, DestroyScope };
  Destroy destroyAssign = DestroyNone;

  // Whether a constructor called on assign is constructing a base class
  // subobject, which uses the base object constructor variant.
  bool baseSubobject = false;

  explicit Env(XIL_PPoint *point, PostEdges *post = nullptr)
    : point(point), post(post) {}

  bool used() const { return branch || assign || lval || rval; }
};

// A destructor call to make when leaving a scope or at the end of a full
// expression. Temporaries that are only constructed on some paths through a
// full expression have a flag variable that is set when they are constructed.
struct Cleanup {
  XIL_Exp instance;
  const clang::CXXDestructorDecl *dtor;
  XIL_Var flag = nullptr;
};

struct Scope;

// The scopes enclosing a point, innermost first, with how many cleanups each
// scope had at the time. This determines which destructors to call when
// jumping from one point to another.
typedef std::vector<std::pair<Scope*, size_t>> ScopeChain;

// Language scopes (blocks, loops, switches), and the cleanup points for the
// temporaries of full expressions. Scopes stay allocated until the end of the
// block, so that gotos to labels in scopes that have been left can be
// resolved.
struct Scope {
  Scope *parent = nullptr;

  // Control flow targets for break/continue, and the scopes at those targets.
  XIL_PPoint breakPoint = 0;
  XIL_PPoint continuePoint = 0;
  ScopeChain breakChain;
  ScopeChain continueChain;

  // Set for switch statements.
  XIL_Exp switchTest = nullptr;
  XIL_PPoint switchPoint = 0;
  XIL_PPoint defaultPoint = 0;

  // Destructors to call when leaving the scope, in construction order.
  std::vector<Cleanup> cleanups;

  // Set for the scope of a full expression, whose cleanups are for its
  // temporaries. The flags of conditionally constructed temporaries are
  // cleared at the start of the full expression.
  bool cleanupPoint = false;
  int conditionalDepth = 0;
  std::vector<XIL_Var> flags;
};

class Translator;

// Translates the body of a single function or global variable initializer.
class BlockTranslator {
public:
  BlockTranslator(Translator &tr, const clang::Decl *decl);

  // Generate the CFG for a function definition or global variable. For
  // constructors and destructors, this generates one variant.
  void GenerateFunction(clang::GlobalDecl gd);
  void GenerateGlobal(const clang::VarDecl *vd, XIL_Var var);
  void GenerateFuncName(const clang::PredefinedExpr *pe, XIL_Var var);

  std::string FuncNameString(const clang::PredefinedExpr *pe);

private:
  Translator &T;
  clang::ASTContext &Ctx;

  // The function being generated, if this is not a global initializer.
  const clang::FunctionDecl *Func = nullptr;
  std::string DeclName;

  XIL_PPoint EntryPoint = 0;
  XIL_PPoint ExitPoint = 0;

  // Where return statements go. This is the exit point, except in base
  // object destructors, where member and base destructors run after the body.
  XIL_PPoint ReturnPoint = 0;

  // Most recent point we updated the location of, for locations of new
  // points created where there is no source location.
  XIL_PPoint LastPoint = 0;

  int TempCount = 0;

  // Local variables and parameters that have been translated, and how many
  // locals with each name have been seen (for uniquing shadowed names).
  llvm::DenseMap<const clang::ValueDecl*, XIL_Var> Locals;
  llvm::StringMap<int> LocalNameCounts;
  XIL_Var ReturnVar = nullptr;

  // In the body of a lambda, the closure field holding the captured 'this',
  // and variables captured by reference, whose locals are references.
  const clang::FieldDecl *ThisCapture = nullptr;
  llvm::DenseSet<const clang::VarDecl*> ReferenceCaptures;

  Scope *ActiveScope = nullptr;
  std::deque<Scope> AllScopes;

  // How many conditional branches of an expression (arms of ?:, right sides
  // of && and ||) we are inside of. Temporaries constructed in conditional
  // branches of a full expression are destroyed conditionally.
  int ConditionalDepth = 0;

  // Labels, and gotos to labels that have not been seen yet.
  struct GotoData {
    XIL_PPoint point;
    ScopeChain chain;
  };
  struct LabelData {
    XIL_PPoint point = 0;
    ScopeChain chain;
    std::vector<GotoData> gotos;
  };
  llvm::DenseMap<const clang::LabelDecl*, LabelData> Labels;

  // Values of expressions that are evaluated once but referenced more than
  // once (the condition of a GNU ?: operator, OpaqueValueExprs).
  llvm::DenseMap<const clang::Expr*, XIL_Exp> SavedValues;

  // Whether this block hit a construct that could not be translated. The CFG
  // is dropped in that case, as with the gcc plugin.
  bool Dropped = false;

  // Location handling.
  XIL_Location UpdateLocation(XIL_PPoint point, clang::SourceLocation loc);
  XIL_Location UpdateLocation(XIL_PPoint point, const clang::Stmt *s);
  XIL_Location CurrentLocation();
  XIL_PPoint AddPoint(XIL_Location loc);

  // Error handling. Unexpected drops the CFG; the *Result versions also fill
  // in an error value for the expression.
  void Unexpected(const char *what, const clang::Stmt *s);
  void UnexpectedResult(Env &env, const char *what, const clang::Stmt *s);
  void BogusResult(Env &env);

  // Variables.
  XIL_Var TranslateLocal(const clang::VarDecl *vd);
  XIL_Var TranslateParam(const clang::ParmVarDecl *pd);
  XIL_Var TranslateValueDecl(const clang::ValueDecl *d);
  XIL_Var GetReturnVar();
  XIL_Exp ThisValue();
  XIL_Var NewTemporary(XIL_Type type);
  XIL_Var NewTemporary(clang::QualType type);

  // Expressions. TranslateExpr fills in the result per env.
  void ProcessResult(Env &env, XIL_Exp result);
  void TranslateExpr(Env &env, const clang::Expr *e);
  void TranslateExprInner(Env &env, const clang::Expr *e);
  XIL_Exp RValue(const clang::Expr *e, XIL_PPoint *point, PostEdges *post = nullptr);
  XIL_Exp LValue(const clang::Expr *e, XIL_PPoint *point, PostEdges *post = nullptr);
  void Branch(const clang::Expr *e, XIL_PPoint *point,
              XIL_PPoint truePoint, XIL_PPoint falsePoint);
  bool TryFoldConstant(Env &env, const clang::Expr *e);

  void TranslateCast(Env &env, const clang::CastExpr *e);
  void TranslateUnary(Env &env, const clang::UnaryOperator *e);
  void TranslateIncDec(Env &env, const clang::UnaryOperator *e);
  void TranslateBinary(Env &env, const clang::BinaryOperator *e);
  void TranslateAssign(Env &env, const clang::BinaryOperator *e);
  void TranslateLogical(Env &env, const clang::BinaryOperator *e);
  void TranslateConditional(Env &env, const clang::AbstractConditionalOperator *e);
  void TranslateCall(Env &env, const clang::CallExpr *e);
  void TranslateMemberCall(Env &env, const clang::CallExpr *e,
                           const clang::CXXMethodDecl *md, const clang::Expr *object,
                           bool isArrow, bool qualified,
                           llvm::ArrayRef<const clang::Expr*> args);
  void TranslateConstruct(Env &env, const clang::CXXConstructExpr *e);
  void TranslateMaterialize(Env &env, const clang::MaterializeTemporaryExpr *e);
  void ZeroInitialize(XIL_PPoint *point, XIL_Exp target, clang::QualType type);
  void TranslateBindTemporary(Env &env, const clang::CXXBindTemporaryExpr *e);
  void TranslateFullExpr(Env &env, const clang::ExprWithCleanups *e);
  void TranslateNew(Env &env, const clang::CXXNewExpr *e);
  void TranslateDelete(Env &env, const clang::CXXDeleteExpr *e);
  void TranslateMember(Env &env, const clang::MemberExpr *e);
  void TranslateLambda(Env &env, const clang::LambdaExpr *e);
  void InitializeCaptures(XIL_PPoint *point, const clang::CXXMethodDecl *md);
  void TranslateInitList(Env &env, const clang::InitListExpr *e);
  void TranslateStmtExpr(Env &env, const clang::StmtExpr *e);
  void TranslateVAArg(Env &env, const clang::VAArgExpr *e);
  XIL_Exp MakeArithmetic(clang::BinaryOperatorKind op, clang::QualType resultType,
                         clang::QualType leftType, XIL_Exp left,
                         clang::QualType rightType, XIL_Exp right,
                         const clang::Stmt *s);
  XIL_Exp MakeTruthValue(XIL_Exp value, clang::QualType type, bool nonzero);

  // Calls.
  struct CallResult {
    XIL_Exp lval = nullptr;
    bool done = false;
  };
  CallResult PrepareCallResult(Env &env, const clang::CallExpr *e);
  void FinishCallResult(Env &env, const clang::CallExpr *e, const CallResult &result);
  XIL_Exp CallArgument(const clang::Expr *arg, XIL_PPoint *point, bool reference);
  std::vector<XIL_Exp> CallArguments(llvm::ArrayRef<const clang::Expr*> args,
                                     XIL_PPoint *point,
                                     const clang::FunctionProtoType *proto);
  void AddCall(XIL_PPoint *point, XIL_Type type, XIL_Exp result, XIL_Exp instance,
               XIL_Exp function, std::vector<XIL_Exp> &args,
               XIL_Location loc = nullptr);
  XIL_Field VirtualField(const clang::CXXRecordDecl *rd, const clang::CXXMethodDecl *md,
                         clang::CXXDtorType dtorKind);

  // Initialize an object of the given type with an expression: references
  // are bound to the address of the expression, and C++ objects are
  // constructed in place.
  void InitializeObject(XIL_PPoint *point, PostEdges *post, XIL_Exp target,
                        clang::QualType type, const clang::Expr *init,
                        Env::Destroy destroy = Env::DestroyNone);

  // Constructors, destructors and cleanups.
  void AddDestructorCall(XIL_PPoint *point, XIL_Exp instance,
                         const clang::CXXDestructorDecl *dtor, clang::CXXDtorType kind);
  void AddOperatorDelete(XIL_PPoint *point, XIL_Exp pointer, clang::QualType type,
                         const clang::FunctionDecl *opDelete);
  XIL_Exp FirstElement(XIL_Exp object, clang::QualType *type);
  void AddCleanup(XIL_PPoint *point, XIL_Exp instance, clang::QualType type,
                  Env::Destroy destroy);
  void EmitCleanup(XIL_PPoint *point, const Cleanup &cleanup);
  void EmitCleanups(XIL_PPoint *point, Scope &scope, size_t begin, size_t end);
  ScopeChain CurrentChain();
  void TranslateCtorInitializers(XIL_PPoint *point, const clang::CXXConstructorDecl *ctor,
                                 clang::CXXCtorType kind);
  void TranslateDestructorEpilogue(XIL_PPoint *point, const clang::CXXDestructorDecl *dtor);
  void ForwardToVariant(XIL_PPoint *point, clang::GlobalDecl target);

  // Statements.
  void TranslateStmt(XIL_PPoint *point, const clang::Stmt *s);
  void TranslateCompound(XIL_PPoint *point, const clang::CompoundStmt *s);
  void TranslateDecl(XIL_PPoint *point, PostEdges *post, const clang::Decl *d,
                     bool destroy = true);
  void TranslateDeclStmt(XIL_PPoint *point, PostEdges *post, const clang::Stmt *s);
  void TranslateStaticLocal(XIL_PPoint *point, const clang::VarDecl *vd);
  void TranslateForRange(XIL_PPoint *point, const clang::CXXForRangeStmt *s);
  void TranslateInitStmt(XIL_PPoint *point, const clang::Stmt *init);
  void FinishScope(XIL_PPoint *point, Scope &scope);
  void TranslateReturn(XIL_PPoint *point, const clang::ReturnStmt *s);
  void TranslateIf(XIL_PPoint *point, const clang::IfStmt *s);
  void TranslateWhile(XIL_PPoint *point, const clang::WhileStmt *s);
  void TranslateDo(XIL_PPoint *point, const clang::DoStmt *s);
  void TranslateFor(XIL_PPoint *point, const clang::ForStmt *s);
  void TranslateSwitch(XIL_PPoint *point, const clang::SwitchStmt *s);
  void TranslateCase(XIL_PPoint *point, const clang::SwitchCase *s);
  void TranslateLabel(XIL_PPoint *point, const clang::LabelStmt *s);
  void TranslateGoto(XIL_PPoint *point, const clang::GotoStmt *s);
  void ResolveGoto(XIL_PPoint source, XIL_PPoint target,
                   const ScopeChain &sourceChain, const ScopeChain &targetChain);
  void ConnectPostPoint(XIL_PPoint *point, const PostEdges &post);
  Scope &PushScope();
  void PopScope();
  Scope &LanguageScope();
};

// State for a whole translation unit: type and name translation, which is
// shared between all the blocks generated from the translation unit.
class Translator {
public:
  Translator(clang::ASTContext &ctx, const PluginOptions &options);

  clang::ASTContext &Ctx;
  clang::SourceManager &SM;
  const PluginOptions &Options;

  // Width of pointers on the target, in bytes.
  int PointerWidth;

  std::unique_ptr<clang::MangleContext> Mangler;

  // Used to substitute default template arguments, when available.
  clang::Sema *S = nullptr;

  // Generate everything in the translation unit.
  void TranslateUnit();

  // Locations.
  XIL_Location MakeLocation(clang::SourceLocation loc);
  std::string FileBasename(clang::SourceLocation loc);

  // Names (names.cpp).
  std::string FunctionName(const clang::FunctionDecl *fd);
  std::string GlobalVarName(const clang::VarDecl *vd);
  std::string SourceName(const clang::NamedDecl *d);
  XIL_Var FunctionVar(const clang::FunctionDecl *fd);
  XIL_Var FunctionVar(clang::GlobalDecl gd);
  XIL_Var GlobalVar(const clang::VarDecl *vd);
  XIL_Var FuncNameVar(const clang::PredefinedExpr *pe,
                      const clang::FunctionDecl *fd);

  // Types (types.cpp). Annotations on the function declaration are attached
  // to the type when annotated is given; this happens only for the type of
  // the function whose body is being generated, as with the gcc plugin.
  XIL_Type TranslateType(clang::QualType type);
  XIL_Type TranslateFunctionType(clang::QualType type,
                                 const clang::FunctionDecl *annotated = nullptr);
  XIL_Field TranslateField(const clang::FieldDecl *fd);
  std::string LambdaCaptureName(const clang::FieldDecl *fd);
  int TypeBytes(clang::QualType type);

  // The type of a method, including the class for non-static methods.
  XIL_Type TranslateMethodType(const clang::CXXMethodDecl *md,
                               bool annotated = false);

  // The name of a function with a particular constructor/destructor variant.
  std::string FunctionName(clang::GlobalDecl gd);

  // Give an anonymous struct/union type (or pointer/array of one) a name
  // based on how it is first used, as the gcc plugin does.
  void NameAnonymousType(clang::QualType type, const std::string &name);
  bool IsAnonymousType(clang::QualType type);

  // Whether translation of the active block hit an error in a type.
  bool TypeError = false;

  std::string RecordName(const clang::RecordDecl *rd);

private:
  // CSU names that have been assigned to records.
  llvm::DenseMap<const clang::RecordDecl*, std::string> RecordNames;

  // Names assigned to pointer to member function types, which gcc represents
  // as anonymous structures.
  llvm::DenseMap<const clang::Type*, std::string> MemberPointerNames;

  // Set while printing the elements of a template argument pack, where gcc
  // doesn't leave out default template arguments.
  bool InArgumentPack = false;
  XIL_Type TranslateMemberFunctionPointer(clang::QualType type);

  // The fields of a struct/class in gcc's order: the class's own vtable
  // pointer (which has no field), its bases, its data members, and its
  // virtual bases. Unnamed entries are named field:N by their index.
  struct LayoutEntry {
    const clang::CXXRecordDecl *base = nullptr;
    const clang::FieldDecl *field = nullptr;
    int index = 0;
    int offset = 0;
  };
  struct Layout {
    std::vector<LayoutEntry> entries;
  };
  llvm::DenseMap<const clang::RecordDecl*, Layout> Layouts;
  const Layout &GetLayout(const clang::RecordDecl *rd);
  int FieldIndex(const clang::FieldDecl *fd);
  XIL_Field BaseField(const clang::RecordDecl *rd, const LayoutEntry &entry);

public:
  // The field for a base class subobject (direct or virtual) of a class.
  XIL_Field BaseField(const clang::CXXRecordDecl *rd, const clang::CXXRecordDecl *base);

private:

  // Virtual functions of a class, as computed by XIL_GetFunctionFields in
  // gcc/type.c. Destructors are represented by their complete and deleting
  // variants.
  struct VirtualFunction {
    const clang::CXXMethodDecl *fieldMethod;  // method whose field is used
    const clang::CXXMethodDecl *decl;         // final overrider in the class
    clang::CXXDtorType dtorKind = clang::Dtor_Complete;
    XIL_Field base = nullptr;
  };
  llvm::DenseMap<const clang::CXXRecordDecl*, std::vector<VirtualFunction>> VirtualFunctions;
  const std::vector<VirtualFunction> &GetVirtualFunctions(const clang::CXXRecordDecl *rd);
  XIL_Field MethodField(const clang::CXXMethodDecl *md, clang::CXXDtorType dtorKind);
  void AddCxxMembers(const clang::CXXRecordDecl *rd);

  // C++ names spelled as gcc's type printer spells them (typenames.cpp).
  struct Declarator;
  std::string GccTypeString(clang::QualType type);
  std::string GccTypeString(clang::QualType type, const Declarator &decl);
  std::string GccBaseTypeName(clang::QualType canon);
  std::string GccScope(const clang::DeclContext *dc);
  std::string GccFunctionString(const clang::FunctionDecl *fd);
  std::string GccTemplateArgument(const clang::TemplateArgument &arg);
  std::string GccTemplateArgs(const clang::ClassTemplateSpecializationDecl *spec);
  bool IsDefaultTemplateArgument(const clang::ClassTemplateSpecializationDecl *spec,
                                 llvm::ArrayRef<clang::TemplateArgument> args,
                                 clang::TemplateParameterList *params, unsigned index);
  std::string GccRecordTypeName(const clang::RecordDecl *rd);
  std::string CxxRecordName(const clang::RecordDecl *rd);
  std::string NamespaceName(const clang::NamespaceDecl *ns);
  XIL_Type TranslateRecordType(const clang::RecordDecl *rd);
  XIL_Type TranslateIntType(clang::QualType type);

  // Functions and globals that have had blocks generated, to avoid
  // generating duplicates.
  llvm::DenseSet<const clang::Decl*> Generated;

  // __func__ and friends are static variables with their own initializer
  // blocks. These are generated after the block for the function using them.
  struct PendingFuncName {
    const clang::PredefinedExpr *expr;
    XIL_Var var;
  };
  std::vector<PendingFuncName> PendingFuncNames;
  llvm::StringSet<> FuncNamesGenerated;

  friend class BlockTranslator;
};

// Whether a class is a template instantiation or a member of one, in which
// case gcc names it with its type printer.
bool IsTemplateInstantiation(const clang::RecordDecl *rd);

// Collect the annotate attributes on a declaration.
XIL_AnnotationList DeclAnnotations(const clang::Decl *d);

// Print a constant in the format the gcc plugin uses.
std::string IntegerString(const llvm::APSInt &value);
std::string FloatString(const llvm::APFloat &value);

FILE *LogFile();

} // namespace xgill
