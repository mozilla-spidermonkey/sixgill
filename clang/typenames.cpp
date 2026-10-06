
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

// Names of C++ types, spelled the way gcc's type_as_string spells them. These
// are used for the names of CSUs (struct/class types), which are keys in the
// comp table and are also matched textually by the analysis. See XIL_CSUName
// and XIL_QualifiedName in gcc/type.c for the gcc side.

#include "xgill-clang.h"

#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/TemplateBase.h"
#include "clang/Sema/Sema.h"
#include "clang/Sema/Template.h"

using namespace clang;

namespace xgill {

// gcc's names for builtin types.
static const char *BuiltinName(const BuiltinType *bt, const PrintingPolicy &policy)
{
  switch (bt->getKind()) {
  case BuiltinType::Void: return "void";
  case BuiltinType::Bool: return "bool";
  case BuiltinType::Char_S:
  case BuiltinType::Char_U: return "char";
  case BuiltinType::SChar: return "signed char";
  case BuiltinType::UChar: return "unsigned char";
  case BuiltinType::WChar_S:
  case BuiltinType::WChar_U: return "wchar_t";
  case BuiltinType::Char8: return "char8_t";
  case BuiltinType::Char16: return "char16_t";
  case BuiltinType::Char32: return "char32_t";
  case BuiltinType::Short: return "short int";
  case BuiltinType::UShort: return "short unsigned int";
  case BuiltinType::Int: return "int";
  case BuiltinType::UInt: return "unsigned int";
  case BuiltinType::Long: return "long int";
  case BuiltinType::ULong: return "long unsigned int";
  case BuiltinType::LongLong: return "long long int";
  case BuiltinType::ULongLong: return "long long unsigned int";
  case BuiltinType::Int128: return "__int128";
  case BuiltinType::UInt128: return "__int128 unsigned";
  case BuiltinType::Half: return "__fp16";
  case BuiltinType::Float16: return "_Float16";
  case BuiltinType::BFloat16: return "__bf16";
  case BuiltinType::Float: return "float";
  case BuiltinType::Double: return "double";
  case BuiltinType::LongDouble: return "long double";
  case BuiltinType::Float128: return "__float128";
  case BuiltinType::NullPtr: return "std::nullptr_t";
  default: break;
  }
  return bt->getName(policy).data();
}

// A declarator being built up around a base type, eg "*" or "(*)(int)".
// startsWithParams is set when the declarator is a parameter list with no
// name, which gcc attaches to the base type without a space: "int(int)".
struct Translator::Declarator {
  std::string text;
  bool startsWithParams = false;
};

static std::string QualifierString(Qualifiers quals)
{
  std::string str;
  if (quals.hasConst())
    str += "const";
  if (quals.hasVolatile())
    str += str.empty() ? "volatile" : " volatile";
  return str;
}

std::string Translator::GccTypeString(QualType type)
{
  return GccTypeString(type, Declarator());
}

std::string Translator::GccTypeString(QualType type, const Declarator &decl)
{
  QualType canon = type.getCanonicalType();
  std::string quals = QualifierString(canon.getLocalQualifiers());

  switch (canon->getTypeClass()) {
  case Type::Pointer:
  case Type::LValueReference:
  case Type::RValueReference:
  case Type::MemberPointer: {
    std::string op;
    if (isa<clang::PointerType>(canon))
      op = "*";
    else if (isa<LValueReferenceType>(canon))
      op = "&";
    else if (isa<RValueReferenceType>(canon))
      op = "&&";
    else {
      const MemberPointerType *mpt = cast<MemberPointerType>(canon);
      op = GccRecordTypeName(mpt->getMostRecentCXXRecordDecl()) + "::*";
    }

    Declarator inner;
    inner.text = op + (quals.empty() ? "" : " " + quals);
    // gcc separates a pointer or reference from an enclosing declarator in
    // parentheses, eg the return type in "char* (*)(int)".
    if (!decl.text.empty() && decl.text[0] == '(')
      inner.text += " ";
    inner.text += decl.text;

    // Pointers to functions and arrays need parentheses.
    QualType pointee = canon->getPointeeType();
    if (pointee->isFunctionType() || pointee->isArrayType())
      inner.text = "(" + inner.text + ")";
    else if (isa<MemberPointerType>(canon))
      inner.text = " " + inner.text;
    return GccTypeString(pointee, inner);
  }

  case Type::ConstantArray:
  case Type::IncompleteArray:
  case Type::VariableArray: {
    const ArrayType *at = Ctx.getAsArrayType(canon);
    Declarator inner;
    inner.text = decl.text + "[";
    if (const ConstantArrayType *cat = dyn_cast<ConstantArrayType>(at))
      inner.text += std::to_string(cat->getSize().getZExtValue());
    inner.text += "]";
    if (decl.text.empty() || decl.text[0] != '(')
      inner.text = " " + inner.text;
    return GccTypeString(at->getElementType(), inner);
  }

  case Type::FunctionProto:
  case Type::FunctionNoProto: {
    const FunctionType *ft = canon->castAs<FunctionType>();
    std::string params = "(";
    bool first = true;
    if (const FunctionProtoType *fpt = dyn_cast<FunctionProtoType>(ft)) {
      for (QualType param : fpt->getParamTypes()) {
        if (!first)
          params += ", ";
        params += GccTypeString(param);
        first = false;
      }
      if (fpt->isVariadic())
        params += first ? "..." : ", ...";
    }
    params += ")";
    if (const FunctionProtoType *fpt = dyn_cast<FunctionProtoType>(ft)) {
      std::string methodQuals = QualifierString(fpt->getMethodQuals());
      if (!methodQuals.empty())
        params += " " + methodQuals;
    }

    Declarator inner;
    inner.text = decl.text + params;
    inner.startsWithParams = decl.text.empty();
    return GccTypeString(ft->getReturnType(), inner);
  }

  default:
    break;
  }

  std::string base = quals.empty() ? "" : quals + " ";
  base += GccBaseTypeName(canon);

  if (decl.text.empty())
    return base;
  char c = decl.text[0];
  if (c == '*' || c == '&' || c == ' ' || decl.startsWithParams)
    return base + decl.text;
  return base + " " + decl.text;
}

std::string Translator::GccBaseTypeName(QualType canon)
{
  if (const BuiltinType *bt = dyn_cast<BuiltinType>(canon))
    return BuiltinName(bt, Ctx.getPrintingPolicy());

  if (const RecordDecl *rd = canon->getAsRecordDecl())
    return GccRecordTypeName(rd);

  if (const EnumType *et = canon->getAs<EnumType>()) {
    const EnumDecl *ed = et->getDecl();
    if (!ed->getIdentifier())
      return GccScope(ed->getDeclContext()) + "<unnamed enum>";
    return GccScope(ed->getDeclContext()) + ed->getName().str();
  }

  // The parameters of generic lambdas. gcc prints these as "auto:N", which is
  // changed to "auto" (see XIL_StripAutoCounts in gcc/type.c).
  if (isa<TemplateTypeParmType>(canon))
    return "auto";

  // Shouldn't happen for the types of instantiated code.
  return canon.getAsString(Ctx.getPrintingPolicy());
}

// The scope prefix (eg "js::gc::") for a declaration in the given context, as
// gcc prints it in types.
std::string Translator::GccScope(const DeclContext *dc)
{
  std::string scope;
  for (; dc && !dc->isTranslationUnit(); dc = dc->getParent()) {
    std::string part;
    if (const NamespaceDecl *ns = dyn_cast<NamespaceDecl>(dc)) {
      part = ns->isAnonymousNamespace() ? "{anonymous}" : ns->getName().str();
    }
    else if (const RecordDecl *rd = dyn_cast<RecordDecl>(dc)) {
      // The enclosing class includes its own scope.
      return GccRecordTypeName(rd) + "::" + scope;
    }
    else if (const FunctionDecl *fd = dyn_cast<FunctionDecl>(dc)) {
      return GccFunctionString(fd) + "::" + scope;
    }
    else {
      // extern "C" blocks, etc.
      continue;
    }
    scope = part + "::" + scope;
  }
  return scope;
}

// A function as gcc prints it as the scope of a local class or lambda:
// qualified name, template arguments, parameter types, and qualifiers.
std::string Translator::GccFunctionString(const FunctionDecl *fd)
{
  std::string str = GccScope(fd->getDeclContext()) + fd->getNameAsString();

  if (const TemplateArgumentList *args = fd->getTemplateSpecializationArgs()) {
    std::string list = "<";
    for (const TemplateArgument &arg : args->asArray()) {
      if (arg.getKind() == TemplateArgument::Pack && arg.pack_size() == 0)
        continue;
      if (list.size() > 1)
        list += ", ";
      list += GccTemplateArgument(arg);
    }
    if (list.back() == '>')
      list += " ";
    str += list + ">";
  }

  str += "(";
  bool first = true;
  for (const ParmVarDecl *pd : fd->parameters()) {
    if (!first)
      str += ", ";
    str += GccTypeString(pd->getType());
    first = false;
  }
  if (fd->isVariadic())
    str += first ? "..." : ", ...";
  str += ")";

  if (const CXXMethodDecl *md = dyn_cast<CXXMethodDecl>(fd)) {
    std::string quals = QualifierString(md->getMethodQualifiers());
    if (!quals.empty())
      str += " " + quals;
  }
  return str;
}

// Print a template argument the way gcc does.
std::string Translator::GccTemplateArgument(const TemplateArgument &arg)
{
  switch (arg.getKind()) {
  case TemplateArgument::Type:
    return GccTypeString(arg.getAsType());

  case TemplateArgument::Integral: {
    QualType type = arg.getIntegralType();
    llvm::APSInt value = arg.getAsIntegral();

    if (type->isBooleanType())
      return value.getBoolValue() ? "true" : "false";

    if (const EnumType *et = type->getAs<EnumType>()) {
      const EnumDecl *ed = et->getDecl();
      for (const EnumConstantDecl *ecd : ed->enumerators()) {
        if (llvm::APSInt::isSameValue(ecd->getInitVal(), value)) {
          // Unscoped enumerators are in the scope enclosing the enum.
          std::string scope = ed->isScoped()
            ? GccScope(ed->getDeclContext()) + ed->getName().str() + "::"
            : GccScope(ed->getDeclContext());
          return scope + ecd->getName().str();
        }
      }
      return "(" + GccTypeString(type) + ")" + IntegerString(value);
    }

    if (type->isAnyCharacterType()) {
      int64_t c = value.getExtValue();
      if (c >= 32 && c < 127 && c != '\'' && c != '\\')
        return std::string("'") + (char) c + "'";
    }

    return IntegerString(value);
  }

  case TemplateArgument::Declaration: {
    const ValueDecl *vd = arg.getAsDecl();
    std::string name = GccScope(vd->getDeclContext()) + vd->getName().str();

    // gcc prints functions by name, with any template arguments.
    if (const FunctionDecl *fd = dyn_cast<FunctionDecl>(vd)) {
      if (const TemplateArgumentList *fargs = fd->getTemplateSpecializationArgs()) {
        name += "<";
        for (unsigned i = 0; i < fargs->size(); i++) {
          if (i)
            name += ", ";
          name += GccTemplateArgument(fargs->get(i));
        }
        if (name.back() == '>')
          name += " ";
        name += ">";
      }
      return name;
    }

    if (arg.getParamTypeForDecl()->isReferenceType())
      return name;
    return "(& " + name + ")";
  }

  case TemplateArgument::NullPtr:
    return "nullptr";

  case TemplateArgument::Template:
  case TemplateArgument::TemplateExpansion: {
    TemplateDecl *td = arg.getAsTemplateOrTemplatePattern().getAsTemplateDecl();
    if (td)
      return GccScope(td->getDeclContext()) + td->getName().str();
    return "<template>";
  }

  case TemplateArgument::Pack: {
    // gcc prints argument packs with TFF_NO_OMIT_DEFAULT_TEMPLATE_ARGUMENTS,
    // which applies to every type within them too.
    bool inPack = InArgumentPack;
    InArgumentPack = true;
    std::string str;
    for (const TemplateArgument &elt : arg.pack_elements()) {
      if (!str.empty())
        str += ", ";
      str += GccTemplateArgument(elt);
    }
    InArgumentPack = inPack;
    return str;
  }

  default: {
    std::string str;
    llvm::raw_string_ostream out(str);
    arg.print(Ctx.getPrintingPolicy(), out, /* IncludeType */ false);
    return str;
  }
  }
}

// Whether a template argument has the default value for its parameter.
namespace {

// Finds class template specializations named by a default template argument
// that have not been instantiated. See DefaultIsInstantiated.
class IncompleteScopeFinder : public RecursiveASTVisitor<IncompleteScopeFinder> {
public:
  IncompleteScopeFinder(Sema &s, const MultiLevelTemplateArgumentList &args)
    : S(s), Args(args) {}

  bool Found = false;

  // Visit a typename's scope before the typename itself.
  bool shouldTraversePostOrder() const { return true; }

  bool VisitDependentNameType(DependentNameType *t) {
    return CheckComplete(t);
  }

  bool VisitTemplateSpecializationType(TemplateSpecializationType *t) {
    return CheckComplete(t);
  }

private:
  // Whether substituting a dependent type gives a class that has already been
  // instantiated. Returns false (stopping the traversal) if not.
  bool CheckComplete(const Type *t) {
    if (!t->isDependentType())
      return true;
    QualType type = S.SubstType(QualType(t, 0), Args, SourceLocation(),
                                DeclarationName());
    if (!type.isNull()) {
      const CXXRecordDecl *rd = type->getAsCXXRecordDecl();
      if (!rd || rd->hasDefinition())
        return true;
    }
    Found = true;
    return false;
  }


  Sema &S;
  const MultiLevelTemplateArgumentList &Args;
};

} // anonymous namespace

bool Translator::IsDefaultTemplateArgument(const ClassTemplateSpecializationDecl *spec,
                                           ArrayRef<TemplateArgument> args,
                                           TemplateParameterList *params,
                                           unsigned index)
{
  const NamedDecl *param = params->getParam(index);
  if (isSubstitutedDefaultArgument(Ctx, args[index], param,
                                   args.take_front(index), params->getDepth()))
    return true;

  // isSubstitutedDefaultArgument can't evaluate defaults that depend on other
  // parameters through anything but direct substitution, eg N = sizeof(T) or
  // U = std::make_unsigned_t<T>. Substitute and evaluate those.
  if (!S)
    return false;
  const NonTypeTemplateParmDecl *nttp = dyn_cast<NonTypeTemplateParmDecl>(param);
  const TemplateTypeParmDecl *ttp = dyn_cast<TemplateTypeParmDecl>(param);
  Expr *def = nullptr;
  if (nttp && nttp->hasDefaultArgument() &&
      args[index].getKind() == TemplateArgument::Integral)
    def = nttp->getDefaultArgument().getArgument().getAsExpr();
  else if (!(ttp && ttp->hasDefaultArgument() &&
             args[index].getKind() == TemplateArgument::Type))
    return false;
  if (nttp && !def)
    return false;

  // The default may be ill-formed for these arguments, eg when it names a
  // trait that isn't specialized for them, which is why the argument was given
  // explicitly. Instantiation errors are hard errors even in a SFINAE trap, so
  // suppress diagnostics entirely to keep them from failing the compile.
  DiagnosticsEngine &diags = Ctx.getDiagnostics();
  bool suppressed = diags.getSuppressAllDiagnostics();
  diags.setSuppressAllDiagnostics(true);
  DiagnosticErrorTrap errors(diags);
  Sema::SFINAETrap trap(*S);
  MultiLevelTemplateArgumentList mlist = S->getTemplateInstantiationArgs(spec);

  if (ttp) {
    // As with the gcc plugin, types are substituted directly.
    QualType type = S->SubstType(ttp->getDefaultArgument().getArgument().getAsType(),
                                 mlist, SourceLocation(), DeclarationName());
    bool failed = errors.hasErrorOccurred() || trap.hasErrorOccurred();
    diags.setSuppressAllDiagnostics(suppressed);
    return !failed && !type.isNull() &&
      Ctx.hasSameType(type, args[index].getAsType());
  }

  // Only use defaults whose class template specializations have already been
  // instantiated, as the gcc plugin must (XIL_DefaultIsInstantiated). When
  // the argument was left out, they were instantiated to compute it.
  IncompleteScopeFinder finder(*S, mlist);
  finder.TraverseStmt(def);
  if (finder.Found) {
    diags.setSuppressAllDiagnostics(suppressed);
    return false;
  }

  ExprResult result = S->SubstExpr(def, mlist);
  bool failed = errors.hasErrorOccurred() || trap.hasErrorOccurred();
  diags.setSuppressAllDiagnostics(suppressed);
  if (result.isInvalid() || failed || !result.get())
    return false;

  Expr::EvalResult value;
  if (result.get()->isValueDependent() || !result.get()->EvaluateAsInt(value, Ctx))
    return false;
  return llvm::APSInt::isSameValue(value.Val.getInt(), args[index].getAsIntegral());
}

std::string Translator::GccTemplateArgs(const ClassTemplateSpecializationDecl *spec)
{
  ArrayRef<TemplateArgument> args = spec->getTemplateArgs().asArray();
  TemplateParameterList *params =
    spec->getSpecializedTemplate()->getTemplateParameters();

  // Leave out trailing arguments that have their default values.
  size_t count = args.size();
  while (!InArgumentPack && count > 0 && count <= params->size() &&
         IsDefaultTemplateArgument(spec, args, params, count - 1))
    count--;

  std::string str = "<";
  bool first = true;
  for (size_t i = 0; i < count; i++) {
    std::string arg = GccTemplateArgument(args[i]);
    // Empty argument packs are left out entirely.
    if (args[i].getKind() == TemplateArgument::Pack && args[i].pack_size() == 0)
      continue;
    if (!first)
      str += ", ";
    str += arg;
    first = false;
  }
  if (str.back() == '>')
    str += " ";
  return str + ">";
}

// The name of a class type as gcc's type printer would print it.
std::string Translator::GccRecordTypeName(const RecordDecl *rd)
{
  if (const CXXRecordDecl *crd = dyn_cast<CXXRecordDecl>(rd)) {
    if (crd->isLambda()) {
      std::string params;
      if (const CXXMethodDecl *call = crd->getLambdaCallOperator()) {
        for (const ParmVarDecl *pd : call->parameters()) {
          if (!params.empty())
            params += ", ";
          params += GccTypeString(pd->getType());
        }
      }
      return GccScope(crd->getDeclContext()) + "<lambda(" + params + ")>";
    }
  }

  std::string name;
  if (rd->getIdentifier())
    name = rd->getName().str();
  else if (const TypedefNameDecl *td = rd->getTypedefNameForAnonDecl())
    name = td->getName().str();
  else
    name = rd->isUnion() ? "<unnamed union>" : "<unnamed struct>";

  if (const ClassTemplateSpecializationDecl *spec =
        dyn_cast<ClassTemplateSpecializationDecl>(rd))
    name += GccTemplateArgs(spec);

  return GccScope(rd->getDeclContext()) + name;
}

// Whether a class is named with gcc's type printer: template
// specializations, and classes that are members of them.
bool IsTemplateInstantiation(const RecordDecl *rd)
{
  // Local classes are named by their function, even in function templates.
  if (rd->getParentFunctionOrMethod())
    return false;
  for (const DeclContext *dc = rd; dc; dc = dc->getParent()) {
    if (isa<FunctionDecl>(dc))
      return false;
    if (isa<ClassTemplateSpecializationDecl>(dc))
      return true;
    if (const CXXRecordDecl *crd = dyn_cast<CXXRecordDecl>(dc)) {
      if (crd->getTemplateSpecializationKind() != TSK_Undeclared)
        return true;
    }
  }
  return false;
}

// The name of a C++ class for its CSU, following XIL_CSUName and
// XIL_QualifiedName in gcc/type.c.
std::string Translator::CxxRecordName(const RecordDecl *rd)
{
  if (IsTemplateInstantiation(rd))
    return GccRecordTypeName(rd);

  std::string name;
  if (rd->getIdentifier())
    name = rd->getName().str();
  else if (const TypedefNameDecl *td = rd->getTypedefNameForAnonDecl())
    name = td->getName().str();
  else
    return "";

  const DeclContext *dc = rd->getDeclContext();
  while (isa<LinkageSpecDecl>(dc))
    dc = dc->getParent();

  if (dc->isTranslationUnit())
    return name;

  if (const RecordDecl *outer = dyn_cast<RecordDecl>(dc))
    return RecordName(outer) + "::" + name;

  if (const FunctionDecl *fd = dyn_cast<FunctionDecl>(dc)) {
    // Types in lambdas are named after the closure type, as gcc and clang
    // mangle lambda functions differently.
    if (const CXXMethodDecl *md = dyn_cast<CXXMethodDecl>(fd))
      if (md->getParent()->isLambda())
        return RecordName(md->getParent()) + "::" + name;
    return FunctionName(fd) + "::" + name;
  }

  if (const NamespaceDecl *ns = dyn_cast<NamespaceDecl>(dc)) {
    if (ns->isAnonymousNamespace())
      return FileBasename(rd->getLocation()) + ":" + name;
    return NamespaceName(ns) + "::" + name;
  }

  return name;
}

// The qualified name of a namespace, following XIL_QualifiedName.
std::string Translator::NamespaceName(const NamespaceDecl *ns)
{
  std::string name = ns->getName().str();
  const DeclContext *dc = ns->getParent();
  while (isa<LinkageSpecDecl>(dc))
    dc = dc->getParent();
  if (const NamespaceDecl *outer = dyn_cast<NamespaceDecl>(dc)) {
    if (outer->isAnonymousNamespace())
      return FileBasename(ns->getLocation()) + ":" + name;
    return NamespaceName(outer) + "::" + name;
  }
  return name;
}

} // namespace xgill
