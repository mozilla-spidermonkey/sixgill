
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

// Translation of function bodies and global initializers. This is modeled on
// gcc/tree.c: see the comments there for the conventions.
//
// Expressions are translated in the value form used by gcc/tree.c: an lvalue
// expression such as a variable x translates to Drf(Var x), and its address
// (XIL_ExpAddress) is Var x.

#include "xgill-clang.h"

#include "clang/AST/Attr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/StmtCXX.h"

#include <climits>

using namespace clang;

namespace xgill {

/////////////////////////////////////////////////////////////////////
// Constants
/////////////////////////////////////////////////////////////////////

std::string IntegerString(const llvm::APSInt &value)
{
  return llvm::toString(value, 10);
}

// Print the exact decimal value of a floating point constant, in the format
// gcc's real_to_decimal uses: d.ddde+N
std::string FloatString(const llvm::APFloat &value)
{
  if (value.isNaN())
    return "NaN";
  if (value.isInfinity())
    return value.isNegative() ? "-Inf" : "+Inf";

  std::string sign = value.isNegative() ? "-" : "";
  if (value.isZero())
    return sign + "0.0e+0";

  // Get an integer mantissa and binary exponent with value = mantissa * 2^exp.
  llvm::APFloat abs = llvm::abs(value);
  unsigned precision = llvm::APFloat::semanticsPrecision(abs.getSemantics());
  int shift = (int) precision - 1 - llvm::ilogb(abs);
  llvm::APFloat scaled =
    llvm::scalbn(abs, shift, llvm::APFloat::rmNearestTiesToEven);

  llvm::APSInt mantissa(precision + 1, /* isUnsigned */ true);
  bool exact = false;
  scaled.convertToInteger(mantissa, llvm::APFloat::rmTowardZero, &exact);
  int exp = -shift;

  // Compute the exact decimal digits, as digits * 10^-decimals.
  llvm::APInt digits;
  int decimals = 0;
  if (exp >= 0) {
    digits = mantissa.zext(precision + 1 + exp);
    digits <<= exp;
  }
  else {
    // mantissa * 2^exp == mantissa * 5^-exp / 10^-exp
    decimals = -exp;
    unsigned bits = precision + 1 + 3 * decimals;
    digits = mantissa.zext(bits);
    llvm::APInt five(bits, 5);
    for (int i = 0; i < decimals; i++)
      digits *= five;
  }

  std::string str = llvm::toString(digits, 10, false);

  // Strip trailing zeros.
  while (str.size() > 1 && str.back() == '0') {
    str.pop_back();
    decimals--;
  }

  int exponent = (int) str.size() - 1 - decimals;
  std::string rest = str.substr(1);
  if (rest.empty())
    rest = "0";

  char buf[32];
  snprintf(buf, sizeof(buf), "e%+d", exponent);
  return sign + str.substr(0, 1) + "." + rest + buf;
}

// The value of __func__ and friends. In C, gcc gives __PRETTY_FUNCTION__ the
// plain function name, while clang gives the full signature.
std::string BlockTranslator::FuncNameString(const PredefinedExpr *pe)
{
  if (!Ctx.getLangOpts().CPlusPlus && Func)
    return Func->getName().str();
  if (const StringLiteral *name = pe->getFunctionName())
    return name->getString().str();
  return "";
}

/////////////////////////////////////////////////////////////////////
// Locations, points, errors
/////////////////////////////////////////////////////////////////////

BlockTranslator::BlockTranslator(Translator &tr, const clang::Decl *decl)
  : T(tr), Ctx(tr.Ctx), Func(dyn_cast<FunctionDecl>(decl))
{}

XIL_Location BlockTranslator::CurrentLocation()
{
  return XIL_CFGGetPointLocation(LastPoint);
}

XIL_Location BlockTranslator::UpdateLocation(XIL_PPoint point, SourceLocation sloc)
{
  XIL_Location loc = T.MakeLocation(sloc);
  if (!loc)
    return CurrentLocation();

  if (point) {
    XIL_CFGSetPointLocation(point, loc);
    LastPoint = point;
  }
  return loc;
}

XIL_Location BlockTranslator::UpdateLocation(XIL_PPoint point, const Stmt *s)
{
  if (const Expr *e = dyn_cast<Expr>(s))
    return UpdateLocation(point, e->getExprLoc());
  return UpdateLocation(point, s->getBeginLoc());
}

XIL_PPoint BlockTranslator::AddPoint(XIL_Location loc)
{
  return XIL_CFGAddPoint(loc ? loc : CurrentLocation());
}

void BlockTranslator::Unexpected(const char *what, const Stmt *s)
{
  if (Dropped)
    return;
  Dropped = true;

  std::string where = s ? s->getBeginLoc().printToString(T.SM) : "<unknown>";
  fprintf(LogFile(), "\nXIL: Unexpected %s (%s) at %s in %s\n\n",
          what, s ? s->getStmtClassName() : "", where.c_str(),
          DeclName.c_str());
  fflush(LogFile());
}

void BlockTranslator::BogusResult(Env &env)
{
  XIL_Var errorVar = XIL_VarGlob("error", "error");
  ProcessResult(env, XIL_ExpDrf(XIL_ExpVar(errorVar)));
}

void BlockTranslator::UnexpectedResult(Env &env, const char *what, const Stmt *s)
{
  Unexpected(what, s);
  BogusResult(env);
}

/////////////////////////////////////////////////////////////////////
// Variables
/////////////////////////////////////////////////////////////////////

XIL_Var BlockTranslator::TranslateParam(const ParmVarDecl *pd)
{
  auto iter = Locals.find(pd);
  if (iter != Locals.end())
    return iter->second;

  unsigned index = pd->getFunctionScopeIndex();
  std::string name = pd->getIdentifier()
    ? pd->getName().str()
    : "__arg" + std::to_string(index);

  XIL_Var var = XIL_VarArg((int) index, name.c_str());
  XIL_CFGAddVar(var, T.TranslateType(pd->getType()), 0);
  Locals[pd] = var;
  return var;
}

XIL_Var BlockTranslator::TranslateLocal(const VarDecl *vd)
{
  auto iter = Locals.find(vd);
  if (iter != Locals.end())
    return iter->second;

  // Unique the names of locals that shadow other locals with ':N'.
  std::string source = vd->getName().str();
  int &count = LocalNameCounts[source];
  std::string name = count ? source + ":" + std::to_string(count) : source;
  count++;

  T.NameAnonymousType(vd->getType(), DeclName + ":" + source);

  XIL_Var var = XIL_VarLocal(name.c_str(), source.c_str());
  XIL_CFGAddVar(var, T.TranslateType(vd->getType()), 0);
  Locals[vd] = var;
  return var;
}

XIL_Var BlockTranslator::TranslateValueDecl(const ValueDecl *d)
{
  if (const ParmVarDecl *pd = dyn_cast<ParmVarDecl>(d))
    return TranslateParam(pd);
  if (const VarDecl *vd = dyn_cast<VarDecl>(d)) {
    if (vd->hasGlobalStorage())
      return T.GlobalVar(vd);
    return TranslateLocal(vd);
  }
  if (const FunctionDecl *fd = dyn_cast<FunctionDecl>(d))
    return T.FunctionVar(fd);
  return nullptr;
}

XIL_Var BlockTranslator::GetReturnVar()
{
  if (!ReturnVar) {
    ReturnVar = XIL_VarReturn();
    XIL_CFGAddVar(ReturnVar, T.TranslateType(Func->getReturnType()), 0);
  }
  return ReturnVar;
}

// The value of 'this', a pointer to the object. In a lambda, this is the
// captured 'this' of the enclosing method.
XIL_Exp BlockTranslator::ThisValue()
{
  XIL_Exp value = XIL_ExpDrf(XIL_ExpVar(XIL_VarThis()));
  if (ThisCapture) {
    XIL_Exp field = XIL_ExpFld(value, T.TranslateField(ThisCapture));
    // [*this] captures a copy of the object.
    value = ThisCapture->getType()->isPointerType() ? XIL_ExpDrf(field) : field;
  }
  return value;
}

XIL_Var BlockTranslator::NewTemporary(XIL_Type type)
{
  TempCount++;
  std::string name = "__temp_" + std::to_string(TempCount);
  XIL_Var var = XIL_VarTemp(name.c_str());
  XIL_CFGAddVar(var, type, 0);
  return var;
}

XIL_Var BlockTranslator::NewTemporary(QualType type)
{
  T.NameAnonymousType(type, DeclName + ":__temp_" + std::to_string(TempCount + 1));
  return NewTemporary(T.TranslateType(type));
}

/////////////////////////////////////////////////////////////////////
// Expressions
/////////////////////////////////////////////////////////////////////

void BlockTranslator::ProcessResult(Env &env, XIL_Exp result)
{
  if (!env.used())
    return;

  env.processed = true;

  if (env.point && *env.point)
    LastPoint = *env.point;

  if (env.branch) {
    // Fold branches on constants, eg for do { ... } while (0) loops.
    long value;
    if (XIL_GetExpInt(result, &value)) {
      XIL_CFGEdgeSkip(*env.point, value ? env.truePoint : env.falsePoint);
    }
    else {
      XIL_CFGEdgeAssume(*env.point, env.truePoint, result, 1);
      XIL_CFGEdgeAssume(*env.point, env.falsePoint, result, 0);
    }
    *env.point = 0;
    return;
  }

  if (env.assign) {
    XIL_PPoint locPoint = *env.point ? *env.point : LastPoint;
    XIL_PPoint after = XIL_CFGAddPoint(XIL_CFGGetPointLocation(locPoint));
    XIL_CFGEdgeAssign(*env.point, after, env.assignType, env.assign, result);
    *env.point = after;
    return;
  }

  if (env.lval) {
    XIL_Exp address = XIL_ExpAddress(result);
    if (!address) {
      // Not an lvalue. This shouldn't happen for well formed code.
      Unexpected("non-lvalue", nullptr);
      address = result;
    }
    *env.lval = address;
    return;
  }

  if (env.rval) {
    *env.rval = result;
    return;
  }
}

XIL_Exp BlockTranslator::RValue(const Expr *e, XIL_PPoint *point, PostEdges *post)
{
  XIL_Exp result = nullptr;
  Env env(point, post);
  env.rval = &result;
  TranslateExpr(env, e);
  return result;
}

XIL_Exp BlockTranslator::LValue(const Expr *e, XIL_PPoint *point, PostEdges *post)
{
  XIL_Exp result = nullptr;
  Env env(point, post);
  env.lval = &result;
  TranslateExpr(env, e);
  return result;
}

// Make a test of whether a value is nonzero (or zero), as gcc's C front end
// does for the conditions of branches.
XIL_Exp BlockTranslator::MakeTruthValue(XIL_Exp value, QualType type, bool nonzero)
{
  QualType canon = type.getCanonicalType();
  if (canon->isPointerType() || canon->isNullPtrType()) {
    XIL_Type stride = canon->isPointerType()
      ? T.TranslateType(canon->getPointeeType())
      : XIL_TypeVoid();
    return XIL_ExpBinop(nonzero ? XIL_B_NotEqualP : XIL_B_EqualP,
                        value, XIL_ExpInt(0), stride, 0, true);
  }

  XIL_Exp zero = canon->isRealFloatingType()
    ? XIL_ExpFloat("0.0e+0")
    : XIL_ExpInt(0);
  return XIL_ExpBinop(nonzero ? XIL_B_NotEqual : XIL_B_Equal,
                      value, zero, nullptr, 0, true);
}

// Whether an expression used as a condition has a truth value already, rather
// than needing a comparison against zero.
static bool IsTruthValue(const Expr *e)
{
  e = e->IgnoreParens();
  if (e->getType()->isBooleanType())
    return true;
  if (const BinaryOperator *bo = dyn_cast<BinaryOperator>(e))
    return bo->isComparisonOp() || bo->isLogicalOp();
  if (const UnaryOperator *uo = dyn_cast<UnaryOperator>(e))
    return uo->getOpcode() == UO_LNot;
  return false;
}

void BlockTranslator::Branch(const Expr *e, XIL_PPoint *point,
                             XIL_PPoint truePoint, XIL_PPoint falsePoint)
{
  Env env(point);
  env.branch = true;
  env.truePoint = truePoint;
  env.falsePoint = falsePoint;

  const Expr *inner = e->IgnoreParens();

  // Branch on the value of the last expression in a comma expression.
  if (const BinaryOperator *bo = dyn_cast<BinaryOperator>(inner)) {
    if (bo->getOpcode() == BO_Comma) {
      UpdateLocation(*point, bo);
      Env first(point);
      TranslateExpr(first, bo->getLHS());
      Branch(bo->getRHS(), point, truePoint, falsePoint);
      return;
    }
  }

  // !x branches on x with the targets swapped, except that in C a non-boolean
  // x is tested with x == 0, as gcc does.
  if (const UnaryOperator *uo = dyn_cast<UnaryOperator>(inner)) {
    if (uo->getOpcode() == UO_LNot && IsTruthValue(uo->getSubExpr())) {
      Branch(uo->getSubExpr(), point, falsePoint, truePoint);
      return;
    }
  }

  // Short circuit and conditional operators branch on their operands.
  if (isa<AbstractConditionalOperator>(inner) || IsTruthValue(inner) ||
      e->isIntegerConstantExpr(Ctx)) {
    TranslateExpr(env, e);
    return;
  }

  XIL_Exp value = RValue(e, point);
  ProcessResult(env, MakeTruthValue(value, e->getType(), true));
}

bool BlockTranslator::TryFoldConstant(Env &env, const Expr *e)
{
  if (e->isValueDependent() || e->isTypeDependent() || !e->isPRValue())
    return false;

  // gcc folds constant subexpressions before we see them, so do the same.
  QualType type = e->getType();
  if (type->isIntegralOrEnumerationType()) {
    if (isa<IntegerLiteral>(e) || !e->isIntegerConstantExpr(Ctx))
      return false;
    Expr::EvalResult result;
    if (!e->EvaluateAsInt(result, Ctx, Expr::SE_NoSideEffects) || !result.Val.isInt())
      return false;
    std::string str = IntegerString(result.Val.getInt());
    ProcessResult(env, XIL_ExpIntStr(str.c_str()));
    return true;
  }

  if (type->isRealFloatingType()) {
    if (e->HasSideEffects(Ctx) || !e->isEvaluatable(Ctx))
      return false;
    llvm::APFloat value(0.0);
    if (!e->EvaluateAsFloat(value, Ctx))
      return false;
    std::string str = FloatString(value);
    ProcessResult(env, XIL_ExpFloat(str.c_str()));
    return true;
  }

  return false;
}

void BlockTranslator::TranslateExpr(Env &env, const Expr *e)
{
  TranslateExprInner(env, e);

  // If the caller wanted the result, make sure it got it.
  if (env.used()) {
    if (!env.processed || (env.rval && !*env.rval) || (env.lval && !*env.lval))
      UnexpectedResult(env, "unprocessed expression", e);
  }
}

void BlockTranslator::TranslateExprInner(Env &env, const Expr *e)
{
  if (TryFoldConstant(env, e))
    return;

  switch (e->getStmtClass()) {

  case Stmt::ParenExprClass:
    TranslateExpr(env, cast<ParenExpr>(e)->getSubExpr());
    return;

  case Stmt::ConstantExprClass:
    TranslateExpr(env, cast<FullExpr>(e)->getSubExpr());
    return;

  case Stmt::ExprWithCleanupsClass:
    TranslateFullExpr(env, cast<ExprWithCleanups>(e));
    return;

  case Stmt::ImplicitCastExprClass:
  case Stmt::CStyleCastExprClass:
  case Stmt::CXXStaticCastExprClass:
  case Stmt::CXXFunctionalCastExprClass:
  case Stmt::CXXReinterpretCastExprClass:
  case Stmt::CXXConstCastExprClass:
  case Stmt::CXXDynamicCastExprClass:
  case Stmt::BuiltinBitCastExprClass:
    TranslateCast(env, cast<CastExpr>(e));
    return;

  case Stmt::DeclRefExprClass: {
    const ValueDecl *d = cast<DeclRefExpr>(e)->getDecl();

    if (const EnumConstantDecl *ecd = dyn_cast<EnumConstantDecl>(d)) {
      std::string str = IntegerString(ecd->getInitVal());
      ProcessResult(env, XIL_ExpIntStr(str.c_str()));
      return;
    }

    // Structured bindings refer to part of the decomposed object.
    if (const BindingDecl *bd = dyn_cast<BindingDecl>(d)) {
      if (const Expr *binding = bd->getBinding()) {
        TranslateExpr(env, binding);
        return;
      }
    }

    XIL_Var var = TranslateValueDecl(d);
    if (!var) {
      UnexpectedResult(env, "declaration reference", e);
      return;
    }

    XIL_Exp result = XIL_ExpDrf(XIL_ExpVar(var));

    // Variable length arrays are pointers, and references are pointers to
    // the referenced object, so these need an extra dereference.
    if (d->getType()->isVariableArrayType() || d->getType()->isReferenceType())
      result = XIL_ExpDrf(result);
    else if (const VarDecl *vd = dyn_cast<VarDecl>(d); vd && ReferenceCaptures.count(vd))
      result = XIL_ExpDrf(result);

    ProcessResult(env, result);
    return;
  }

  case Stmt::CXXThisExprClass:
    ProcessResult(env, ThisValue());
    return;

  case Stmt::CXXBoolLiteralExprClass:
    ProcessResult(env, XIL_ExpInt(cast<CXXBoolLiteralExpr>(e)->getValue() ? 1 : 0));
    return;

  case Stmt::CXXNullPtrLiteralExprClass:
  case Stmt::GNUNullExprClass:
    ProcessResult(env, XIL_ExpInt(0));
    return;

  case Stmt::CXXScalarValueInitExprClass:
    if (e->getType()->isRealFloatingType())
      ProcessResult(env, XIL_ExpFloat("0.0e+0"));
    else if (!e->getType()->isVoidType())
      ProcessResult(env, XIL_ExpInt(0));
    else if (env.used())
      ProcessResult(env, XIL_ExpEmpty());
    return;

  case Stmt::CXXDefaultArgExprClass:
    TranslateExpr(env, cast<CXXDefaultArgExpr>(e)->getExpr());
    return;

  case Stmt::CXXDefaultInitExprClass:
    TranslateExpr(env, cast<CXXDefaultInitExpr>(e)->getExpr());
    return;

  case Stmt::SubstNonTypeTemplateParmExprClass:
    TranslateExpr(env, cast<SubstNonTypeTemplateParmExpr>(e)->getReplacement());
    return;

  case Stmt::CXXRewrittenBinaryOperatorClass:
    TranslateExpr(env, cast<CXXRewrittenBinaryOperator>(e)->getSemanticForm());
    return;

  case Stmt::CXXMemberCallExprClass:
  case Stmt::CXXOperatorCallExprClass:
  case Stmt::UserDefinedLiteralClass:
    TranslateCall(env, cast<CallExpr>(e));
    return;

  case Stmt::CXXConstructExprClass:
  case Stmt::CXXTemporaryObjectExprClass:
    TranslateConstruct(env, cast<CXXConstructExpr>(e));
    return;

  case Stmt::MaterializeTemporaryExprClass:
    TranslateMaterialize(env, cast<MaterializeTemporaryExpr>(e));
    return;

  case Stmt::CXXBindTemporaryExprClass:
    TranslateBindTemporary(env, cast<CXXBindTemporaryExpr>(e));
    return;

  case Stmt::CXXNewExprClass:
    TranslateNew(env, cast<CXXNewExpr>(e));
    return;

  case Stmt::CXXDeleteExprClass:
    TranslateDelete(env, cast<CXXDeleteExpr>(e));
    return;

  case Stmt::ArrayInitLoopExprClass:
    // Copies of arrays in implicit copy constructors and lambda captures.
    // Treat these as a copy of the whole array.
    TranslateExpr(env, cast<ArrayInitLoopExpr>(e)->getCommonExpr());
    return;

  case Stmt::LambdaExprClass:
    TranslateLambda(env, cast<LambdaExpr>(e));
    return;

  case Stmt::CXXInheritedCtorInitExprClass: {
    // The base class initializer of an inheriting constructor, which passes
    // on the constructor's arguments.
    const CXXInheritedCtorInitExpr *ie = cast<CXXInheritedCtorInitExpr>(e);
    XIL_Location loc = UpdateLocation(*env.point, e);
    const CXXConstructorDecl *ctor = ie->getConstructor();
    XIL_Exp object = env.assign ? env.assign : XIL_ExpVar(NewTemporary(e->getType()));
    std::vector<XIL_Exp> args;
    if (Func) {
      for (const ParmVarDecl *pd : Func->parameters())
        args.push_back(XIL_ExpDrf(XIL_ExpVar(TranslateParam(pd))));
    }
    CXXCtorType kind = env.baseSubobject ? Ctor_Base : Ctor_Complete;
    XIL_Exp function = XIL_ExpVar(T.FunctionVar(GlobalDecl(ctor, kind)));
    AddCall(env.point, T.TranslateMethodType(ctor), nullptr, object, function, args, loc);
    if (env.assign)
      env.processed = true;
    else
      ProcessResult(env, XIL_ExpDrf(object));
    return;
  }

  case Stmt::AtomicExprClass: {
    // Atomic builtins are calls to a function named after the builtin, as
    // with gcc.
    const AtomicExpr *ae = cast<AtomicExpr>(e);
    XIL_Location loc = UpdateLocation(*env.point, e);
    std::vector<const Expr*> operands = { ae->getPtr() };
    unsigned count = ae->getNumSubExprs();
    if (count > 2)
      operands.push_back(ae->getVal1());
    if (count > 4)
      operands.push_back(ae->getVal2());
    if (count > 5)
      operands.push_back(ae->getWeak());
    operands.push_back(ae->getOrder());
    if (count > 3)
      operands.push_back(ae->getOrderFail());

    std::vector<XIL_Exp> args;
    std::vector<XIL_Type> argTypes;
    for (const Expr *operand : operands) {
      args.push_back(RValue(operand, env.point));
      argTypes.push_back(T.TranslateType(operand->getType()));
    }

    XIL_Type type = XIL_TypeFunction(T.TranslateType(e->getType()), nullptr, 0,
                                     argTypes.data(), nullptr, (int) argTypes.size(),
                                     XIL_MakeAnnotationList());
    std::string name = ae->getOpAsString().str();
    XIL_Exp function = XIL_ExpVar(XIL_VarFunc(name.c_str(), name.c_str()));

    XIL_Exp result = nullptr;
    if (env.used() && !e->getType()->isVoidType())
      result = XIL_ExpVar(NewTemporary(e->getType()));
    AddCall(env.point, type, result, nullptr, function, args, loc);
    if (result)
      ProcessResult(env, XIL_ExpDrf(result));
    else if (env.used())
      ProcessResult(env, XIL_ExpEmpty());
    return;
  }

  case Stmt::ShuffleVectorExprClass:
  case Stmt::ConvertVectorExprClass:
  case Stmt::ExtVectorElementExprClass:
    // Vector types are not handled, as with gcc.
    BogusResult(env);
    return;

  case Stmt::CXXStdInitializerListExprClass: {
    // An initializer list points to a temporary array, with either its length
    // or its end, as with gcc.
    const CXXStdInitializerListExpr *ile = cast<CXXStdInitializerListExpr>(e);
    UpdateLocation(*env.point, e);
    const Expr *sub = ile->getSubExpr();
    XIL_Exp array = LValue(sub, env.point);
    uint64_t length = 0;
    QualType elementType = Ctx.CharTy;
    if (const ConstantArrayType *cat = Ctx.getAsConstantArrayType(sub->getType())) {
      length = cat->getZExtSize();
      elementType = cat->getElementType();
    }

    XIL_Exp object = env.assign ? env.assign : XIL_ExpVar(NewTemporary(e->getType()));
    const RecordDecl *rd = e->getType()->getAsRecordDecl();
    auto field = rd ? rd->getDefinition()->field_begin() : RecordDecl::field_iterator();
    if (rd && field != rd->getDefinition()->field_end()) {
      const FieldDecl *first = *field++;
      XIL_PPoint after = AddPoint(CurrentLocation());
      XIL_CFGEdgeAssign(*env.point, after, T.TranslateType(first->getType()),
                        XIL_ExpFld(object, T.TranslateField(first)), array);
      *env.point = after;
      if (field != rd->getDefinition()->field_end()) {
        const FieldDecl *second = *field;
        XIL_Exp value = XIL_ExpInt((long) length);
        if (second->getType()->isPointerType())
          value = XIL_ExpBinop(XIL_B_PlusPI, array, value, T.TranslateType(elementType), 0, true);
        after = AddPoint(CurrentLocation());
        XIL_CFGEdgeAssign(*env.point, after, T.TranslateType(second->getType()),
                          XIL_ExpFld(object, T.TranslateField(second)), value);
        *env.point = after;
      }
    }

    if (env.assign)
      env.processed = true;
    else
      ProcessResult(env, XIL_ExpDrf(object));
    return;
  }

  case Stmt::CXXParenListInitExprClass:
  case Stmt::CXXTypeidExprClass:
  case Stmt::CXXPseudoDestructorExprClass:
    BogusResult(env);
    return;

  case Stmt::IntegerLiteralClass: {
    const IntegerLiteral *il = cast<IntegerLiteral>(e);
    llvm::APSInt value(il->getValue(), il->getType()->isUnsignedIntegerType());
    std::string str = IntegerString(value);
    ProcessResult(env, XIL_ExpIntStr(str.c_str()));
    return;
  }

  case Stmt::FloatingLiteralClass: {
    std::string str = FloatString(cast<FloatingLiteral>(e)->getValue());
    ProcessResult(env, XIL_ExpFloat(str.c_str()));
    return;
  }

  case Stmt::StringLiteralClass: {
    const StringLiteral *sl = cast<StringLiteral>(e);
    std::string data = sl->getBytes().str();
    data.append(sl->getCharByteWidth(), '\0');
    XIL_Type type = T.TranslateType(sl->getType());
    XIL_Exp str = XIL_ExpString(type, (void*) data.data(), (int) data.size());
    ProcessResult(env, XIL_ExpDrf(str));
    return;
  }

  case Stmt::PredefinedExprClass: {
    const PredefinedExpr *pe = cast<PredefinedExpr>(e);
    if (!Func) {
      UnexpectedResult(env, "predefined expression outside function", e);
      return;
    }
    XIL_Var var = T.FuncNameVar(pe, Func);
    ProcessResult(env, XIL_ExpDrf(XIL_ExpVar(var)));
    return;
  }

  case Stmt::UnaryOperatorClass:
    TranslateUnary(env, cast<UnaryOperator>(e));
    return;

  case Stmt::BinaryOperatorClass:
  case Stmt::CompoundAssignOperatorClass:
    TranslateBinary(env, cast<BinaryOperator>(e));
    return;

  case Stmt::ConditionalOperatorClass:
  case Stmt::BinaryConditionalOperatorClass:
    TranslateConditional(env, cast<AbstractConditionalOperator>(e));
    return;

  case Stmt::CallExprClass:
    TranslateCall(env, cast<CallExpr>(e));
    return;

  case Stmt::MemberExprClass:
    TranslateMember(env, cast<MemberExpr>(e));
    return;

  case Stmt::ArraySubscriptExprClass: {
    const ArraySubscriptExpr *ae = cast<ArraySubscriptExpr>(e);
    const Expr *base = ae->getBase()->IgnoreParens();
    XIL_Type elementType = T.TranslateType(ae->getType());

    // Indexing into an actual array (rather than a pointer) is an Index on
    // the array's address, as with gcc's ARRAY_REF.
    const ImplicitCastExpr *decay = dyn_cast<ImplicitCastExpr>(base);
    if (decay && decay->getCastKind() == CK_ArrayToPointerDecay &&
        !decay->getSubExpr()->getType()->isVariableArrayType()) {
      XIL_Exp array = LValue(decay->getSubExpr(), env.point, env.post);
      XIL_Exp index = RValue(ae->getIdx(), env.point, env.post);
      ProcessResult(env, XIL_ExpDrf(XIL_ExpIndex(array, elementType, index)));
      return;
    }

    XIL_Exp pointer = RValue(ae->getBase(), env.point, env.post);
    XIL_Exp index = RValue(ae->getIdx(), env.point, env.post);

    // gcc folds p[0] to *p.
    long value;
    if (XIL_GetExpInt(index, &value) && value == 0) {
      ProcessResult(env, XIL_ExpDrf(pointer));
      return;
    }

    XIL_Exp sum = XIL_ExpBinop(XIL_B_PlusPI, pointer, index, elementType, 0, true);
    ProcessResult(env, XIL_ExpDrf(sum));
    return;
  }

  case Stmt::CompoundLiteralExprClass: {
    // Compound literals are unnamed locals, initialized where they appear.
    const CompoundLiteralExpr *cle = cast<CompoundLiteralExpr>(e);
    UpdateLocation(*env.point, e);
    XIL_Var temp = NewTemporary(cle->getType());
    XIL_Exp tempExp = XIL_ExpVar(temp);

    Env init(env.point);
    init.assign = tempExp;
    init.assignType = T.TranslateType(cle->getType());
    TranslateExpr(init, cle->getInitializer());

    ProcessResult(env, XIL_ExpDrf(tempExp));
    return;
  }

  case Stmt::InitListExprClass:
    TranslateInitList(env, cast<InitListExpr>(e));
    return;

  case Stmt::ImplicitValueInitExprClass:
    ProcessResult(env, XIL_ExpInt(0));
    return;

  case Stmt::StmtExprClass:
    TranslateStmtExpr(env, cast<StmtExpr>(e));
    return;

  case Stmt::VAArgExprClass:
    TranslateVAArg(env, cast<VAArgExpr>(e));
    return;

  case Stmt::UnaryExprOrTypeTraitExprClass:
  case Stmt::OffsetOfExprClass: {
    // These are constants except for sizeof on variable length arrays.
    Expr::EvalResult result;
    if (e->EvaluateAsInt(result, Ctx)) {
      std::string str = IntegerString(result.Val.getInt());
      ProcessResult(env, XIL_ExpIntStr(str.c_str()));
      return;
    }
    BogusResult(env);
    return;
  }

  case Stmt::AddrLabelExprClass:
    // The address of a label is treated as a null pointer, as with gcc.
    ProcessResult(env, XIL_ExpInt(0));
    return;

  case Stmt::ChooseExprClass:
    TranslateExpr(env, cast<ChooseExpr>(e)->getChosenSubExpr());
    return;

  case Stmt::GenericSelectionExprClass:
    TranslateExpr(env, cast<GenericSelectionExpr>(e)->getResultExpr());
    return;

  case Stmt::OpaqueValueExprClass: {
    const OpaqueValueExpr *ove = cast<OpaqueValueExpr>(e);
    auto iter = SavedValues.find(ove);
    if (iter != SavedValues.end()) {
      ProcessResult(env, iter->second);
      return;
    }
    if (const Expr *source = ove->getSourceExpr()) {
      TranslateExpr(env, source);
      return;
    }
    UnexpectedResult(env, "opaque value", e);
    return;
  }

  default:
    UnexpectedResult(env, "expression", e);
    return;
  }
}

void BlockTranslator::TranslateCast(Env &env, const CastExpr *e)
{
  const Expr *sub = e->getSubExpr();

  switch (e->getCastKind()) {
  case CK_ArrayToPointerDecay:
  case CK_FunctionToPointerDecay:
  case CK_BuiltinFnToFnPtr: {
    // The value of the pointer is the address of the array or function. This
    // is an ADDR_EXPR for gcc, which updates the location.
    UpdateLocation(*env.point, e);
    if (!env.used()) {
      Env subEnv(env.point, env.post);
      TranslateExpr(subEnv, sub);
      return;
    }
    XIL_Exp address = LValue(sub, env.point, env.post);
    ProcessResult(env, address);
    return;
  }

  case CK_IntegralToBoolean:
  case CK_PointerToBoolean:
  case CK_FloatingToBoolean: {
    // Conversions to bool compare against zero, as with gcc.
    if (!env.used()) {
      Env subEnv(env.point, env.post);
      TranslateExpr(subEnv, sub);
      return;
    }
    XIL_Exp value = RValue(sub, env.point, env.post);
    ProcessResult(env, MakeTruthValue(value, sub->getType(), true));
    return;
  }

  case CK_ToVoid: {
    Env subEnv(env.point, env.post);
    TranslateExpr(subEnv, sub);
    ProcessResult(env, XIL_ExpEmpty());
    return;
  }

  case CK_NullToPointer:
  case CK_NullToMemberPointer: {
    Env subEnv(env.point, env.post);
    TranslateExpr(subEnv, sub);
    ProcessResult(env, XIL_ExpInt(0));
    return;
  }

  case CK_DerivedToBase:
  case CK_UncheckedDerivedToBase:
  case CK_BaseToDerived: {
    // Conversions between classes and their bases access the base class
    // subobject fields, with each step in the path to the base.
    bool toBase = (e->getCastKind() != CK_BaseToDerived);
    QualType derivedType = toBase ? sub->getType() : e->getType();
    if (const clang::PointerType *pt = derivedType->getAs<clang::PointerType>())
      derivedType = pt->getPointeeType();
    const CXXRecordDecl *rd = derivedType->getAsCXXRecordDecl();
    if (!rd) {
      TranslateExpr(env, sub);
      return;
    }

    std::vector<XIL_Field> fields;
    for (const CXXBaseSpecifier *spec : e->path()) {
      const CXXRecordDecl *base = spec->getType()->getAsCXXRecordDecl();
      if (!base)
        break;
      fields.push_back(T.BaseField(rd, base));
      rd = base;
    }

    // The value of a pointer is the address of the object it points to, so
    // these are the same for pointers and lvalues.
    bool isPointer = sub->isPRValue();
    if (!isPointer && !env.used()) {
      Env subEnv(env.point, env.post);
      TranslateExpr(subEnv, sub);
      return;
    }
    XIL_Exp object = isPointer
      ? RValue(sub, env.point, env.post)
      : LValue(sub, env.point, env.post);
    if (toBase) {
      for (XIL_Field field : fields)
        object = XIL_ExpFld(object, field);
    }
    else {
      for (auto it = fields.rbegin(); it != fields.rend(); ++it)
        object = XIL_ExpRfld(object, *it);
    }
    ProcessResult(env, isPointer ? object : XIL_ExpDrf(object));
    return;
  }

  case CK_IntegralCast: {
    // gcc translates narrowing conversions by computing the operand's value
    // and then converting it, so a call result goes through a temporary
    // before it is assigned somewhere.
    if (env.used() && !env.rval &&
        Ctx.getTypeSize(e->getType()) < Ctx.getTypeSize(sub->getType())) {
      XIL_Exp value = RValue(sub, env.point, env.post);
      ProcessResult(env, value);
      return;
    }
    TranslateExpr(env, sub);
    return;
  }

  default:
    // Other conversions do not change the value as far as we are concerned,
    // as with NOP_EXPR in gcc.
    TranslateExpr(env, sub);
    return;
  }
}

void BlockTranslator::TranslateUnary(Env &env, const UnaryOperator *e)
{
  const Expr *sub = e->getSubExpr();

  switch (e->getOpcode()) {
  case UO_AddrOf: {
    UpdateLocation(*env.point, e);
    XIL_Exp address = LValue(sub, env.point, env.post);
    ProcessResult(env, address);
    return;
  }

  case UO_Deref: {
    XIL_Exp target = RValue(sub, env.point, env.post);
    ProcessResult(env, XIL_ExpDrf(target));
    return;
  }

  case UO_Plus:
  case UO_Extension:
    TranslateExpr(env, sub);
    return;

  case UO_Minus:
  case UO_Not: {
    if (!env.used()) {
      Env subEnv(env.point, env.post);
      TranslateExpr(subEnv, sub);
      return;
    }
    XIL_Exp value = RValue(sub, env.point, env.post);
    XIL_UnopKind unop = (e->getOpcode() == UO_Minus) ? XIL_U_Neg : XIL_U_BitwiseNot;
    ProcessResult(env, XIL_ExpUnop(unop, value, 0, true));
    return;
  }

  case UO_LNot: {
    // In C, gcc turns !x into the comparison x == 0, which doesn't update the
    // location.
    if (Ctx.getLangOpts().CPlusPlus)
      UpdateLocation(*env.point, e);
    if (env.branch && IsTruthValue(sub)) {
      Branch(sub, env.point, env.falsePoint, env.truePoint);
      env.processed = true;
      return;
    }
    if (!env.used()) {
      Env subEnv(env.point, env.post);
      TranslateExpr(subEnv, sub);
      return;
    }

    // In C, !x is x == 0. In C++ the operand is a bool.
    XIL_Exp value = RValue(sub, env.point, env.post);
    if (sub->getType()->isBooleanType() && Ctx.getLangOpts().CPlusPlus)
      ProcessResult(env, XIL_ExpUnop(XIL_U_LogicalNot, value, 0, true));
    else
      ProcessResult(env, MakeTruthValue(value, sub->getType(), false));
    return;
  }

  case UO_PreInc:
  case UO_PreDec:
  case UO_PostInc:
  case UO_PostDec:
    TranslateIncDec(env, e);
    return;

  default:
    // __real__, __imag__, co_await
    BogusResult(env);
    return;
  }
}

void BlockTranslator::TranslateIncDec(Env &env, const UnaryOperator *e)
{
  XIL_Location loc = UpdateLocation(*env.point, e);

  bool isIncrement = e->isIncrementOp();

  // Postincrements whose result is not used are the same as preincrements.
  bool isPre = e->isPrefix() || !env.used();

  QualType type = e->getSubExpr()->getType();
  XIL_Type xilType = T.TranslateType(type);

  XIL_Type stride = nullptr;
  if (type->isPointerType())
    stride = T.TranslateType(type->getPointeeType());

  XIL_Exp target = LValue(e->getSubExpr(), env.point, env.post);
  XIL_Exp drfTarget = XIL_ExpDrf(target);

  // The result is the updated lvalue, except for postincrements with no
  // place to hang the side effect, in which case we use a temporary.
  XIL_Exp result = drfTarget;
  if (!isPre && !env.post) {
    XIL_Var temp = NewTemporary(xilType);
    XIL_Exp tempExp = XIL_ExpVar(temp);
    result = XIL_ExpDrf(tempExp);

    XIL_PPoint tempPoint = AddPoint(loc);
    XIL_CFGEdgeAssign(*env.point, tempPoint, xilType, tempExp, drfTarget);
    *env.point = tempPoint;
  }

  XIL_BinopKind binop;
  if (isIncrement)
    binop = stride ? XIL_B_PlusPI : XIL_B_Plus;
  else
    binop = stride ? XIL_B_MinusPI : XIL_B_Minus;
  XIL_Exp value = XIL_ExpBinop(binop, drfTarget, XIL_ExpInt(1), stride, 0, true);

  XIL_PPoint after = AddPoint(loc);
  if (isPre || !env.post) {
    XIL_CFGEdgeAssign(*env.point, after, xilType, target, value);
    *env.point = after;
  }
  else {
    if (!env.post->end) {
      XIL_PPoint start = AddPoint(loc);
      env.post->start = env.post->end = start;
    }
    XIL_CFGEdgeAssign(env.post->end, after, xilType, target, value);
    env.post->end = after;
  }

  ProcessResult(env, result);
}

XIL_Exp BlockTranslator::MakeArithmetic(BinaryOperatorKind op, QualType resultType,
                                        QualType leftType, XIL_Exp left,
                                        QualType rightType, XIL_Exp right,
                                        const Stmt *s)
{
  int bits = (int) Ctx.getTypeSize(resultType);

  // Stride type for pointer arithmetic. Arithmetic on void* and function
  // pointers uses a void stride, as with gcc.
  auto stride = [&](QualType pointerType) {
    return T.TranslateType(pointerType->getPointeeType());
  };

  switch (op) {
  case BO_Add:
    if (leftType->isPointerType())
      return XIL_ExpBinop(XIL_B_PlusPI, left, right, stride(leftType), 0, true);
    if (rightType->isPointerType())
      return XIL_ExpBinop(XIL_B_PlusPI, right, left, stride(rightType), 0, true);
    // Treat constants with their high bit set as negative values, as with
    // gcc.
    left = XIL_ExpSign(left, bits, true);
    right = XIL_ExpSign(right, bits, true);
    return XIL_ExpBinop(XIL_B_Plus, left, right, nullptr, 0, true);

  case BO_Sub:
    if (leftType->isPointerType() && rightType->isPointerType())
      return XIL_ExpBinop(XIL_B_MinusPP, left, right, stride(leftType), 0, true);
    if (leftType->isPointerType())
      return XIL_ExpBinop(XIL_B_MinusPI, left, right, stride(leftType), 0, true);
    return XIL_ExpBinop(XIL_B_Minus, left, right, nullptr, 0, true);

  case BO_Mul:
    return XIL_ExpBinop(XIL_B_Mult, left, right, nullptr, 0, true);
  case BO_Div:
    return XIL_ExpBinop(XIL_B_Div, left, right, nullptr, 0, true);
  case BO_Rem:
    return XIL_ExpBinop(XIL_B_Mod, left, right, nullptr, 0, true);
  case BO_Shl:
    return XIL_ExpBinop(XIL_B_ShiftLeft, left, right, nullptr, 0, true);
  case BO_Shr:
    return XIL_ExpBinop(XIL_B_ShiftRight, left, right, nullptr, 0, true);

  case BO_And:
  case BO_Or:
  case BO_Xor: {
    left = XIL_ExpSign(left, bits, false);
    right = XIL_ExpSign(right, bits, false);
    XIL_BinopKind binop = (op == BO_And) ? XIL_B_BitwiseAnd
                        : (op == BO_Or) ? XIL_B_BitwiseOr
                        : XIL_B_BitwiseXOr;
    return XIL_ExpBinop(binop, left, right, nullptr, 0, true);
  }

  default:
    Unexpected("arithmetic operator", s);
    return XIL_ExpDrf(XIL_ExpVar(XIL_VarGlob("error", "error")));
  }
}

// gcc canonicalizes integer comparisons against constants: the constant goes
// on the right, and the constant is moved one closer to zero where possible,
// eg x < 5 becomes x <= 4 and x > -5 becomes x >= -4. Comparisons of unsigned
// values against zero become equality tests. Do the same so that the CFGs from
// the two compilers match.
static void CanonicalizeComparison(XIL_BinopKind *binop, XIL_Exp *left,
                                   XIL_Exp *right, bool isUnsigned)
{
  long value;
  if (XIL_GetExpInt(*left, &value) && !XIL_GetExpInt(*right, &value)) {
    std::swap(*left, *right);
    switch (*binop) {
    case XIL_B_LessThan: *binop = XIL_B_GreaterThan; break;
    case XIL_B_GreaterThan: *binop = XIL_B_LessThan; break;
    case XIL_B_LessEqual: *binop = XIL_B_GreaterEqual; break;
    case XIL_B_GreaterEqual: *binop = XIL_B_LessEqual; break;
    default: break;
    }
  }

  if (!XIL_GetExpInt(*right, &value))
    return;

  auto setConstant = [&](XIL_BinopKind newBinop, long newValue) {
    *binop = newBinop;
    std::string str = std::to_string(newValue);
    *right = XIL_ExpIntStr(str.c_str());
    value = newValue;
  };

  if (value > 0 && *binop == XIL_B_LessThan)
    setConstant(XIL_B_LessEqual, value - 1);
  else if (value > 0 && *binop == XIL_B_GreaterEqual)
    setConstant(XIL_B_GreaterThan, value - 1);
  else if (value < 0 && value != LONG_MIN && *binop == XIL_B_GreaterThan)
    setConstant(XIL_B_GreaterEqual, value + 1);
  else if (value < 0 && value != LONG_MIN && *binop == XIL_B_LessEqual)
    setConstant(XIL_B_LessThan, value + 1);

  if (isUnsigned && value == 0) {
    if (*binop == XIL_B_LessEqual)
      *binop = XIL_B_Equal;
    else if (*binop == XIL_B_GreaterThan)
      *binop = XIL_B_NotEqual;
  }
}

// gcc's fold puts a plain variable operand of a commutative operator second
// (tree_swap_operands_p). Return whether to swap the operands to match.
static bool SwapCommutativeOperands(ASTContext &ctx, BinaryOperatorKind op,
                                    const Expr *lhs, const Expr *rhs)
{
  switch (op) {
  case BO_Add:
  case BO_Mul:
  case BO_And:
  case BO_Or:
  case BO_Xor:
  case BO_EQ:
  case BO_NE:
    break;
  default:
    return false;
  }

  if (lhs->getType()->isPointerType() || rhs->getType()->isPointerType())
    return false;

  // Look through conversions that don't change the width, as gcc's
  // STRIP_SIGN_NOPS does.
  auto isVariable = [&](const Expr *e) {
    e = e->IgnoreParens();
    while (const ImplicitCastExpr *ice = dyn_cast<ImplicitCastExpr>(e)) {
      CastKind kind = ice->getCastKind();
      bool sameWidth = kind == CK_LValueToRValue || kind == CK_NoOp ||
        (kind == CK_IntegralCast &&
         ctx.getTypeSize(ice->getType()) ==
         ctx.getTypeSize(ice->getSubExpr()->getType()));
      if (!sameWidth)
        break;
      e = ice->getSubExpr()->IgnoreParens();
    }
    const DeclRefExpr *dre = dyn_cast<DeclRefExpr>(e);
    return dre && isa<VarDecl>(dre->getDecl());
  };
  if (rhs->isEvaluatable(ctx))
    return false;
  return isVariable(lhs) && !isVariable(rhs);
}

void BlockTranslator::TranslateBinary(Env &env, const BinaryOperator *e)
{
  BinaryOperatorKind op = e->getOpcode();

  if (e->isAssignmentOp()) {
    TranslateAssign(env, e);
    return;
  }

  if (op == BO_Comma) {
    UpdateLocation(*env.point, e);
    Env first(env.point, env.post);
    TranslateExpr(first, e->getLHS());
    TranslateExpr(env, e->getRHS());
    return;
  }

  if (e->isLogicalOp()) {
    TranslateLogical(env, e);
    return;
  }

  const Expr *lhs = e->getLHS();
  const Expr *rhs = e->getRHS();

  if (e->isComparisonOp()) {
    XIL_Type stride = nullptr;
    QualType leftType = lhs->getType().getCanonicalType();
    if (leftType->isPointerType())
      stride = T.TranslateType(leftType->getPointeeType());
    else if (leftType->isNullPtrType())
      stride = XIL_TypeVoid();

    XIL_BinopKind binop;
    switch (op) {
    case BO_LT: binop = stride ? XIL_B_LessThanP : XIL_B_LessThan; break;
    case BO_GT: binop = stride ? XIL_B_GreaterThanP : XIL_B_GreaterThan; break;
    case BO_LE: binop = stride ? XIL_B_LessEqualP : XIL_B_LessEqual; break;
    case BO_GE: binop = stride ? XIL_B_GreaterEqualP : XIL_B_GreaterEqual; break;
    case BO_EQ: binop = stride ? XIL_B_EqualP : XIL_B_Equal; break;
    case BO_NE: binop = stride ? XIL_B_NotEqualP : XIL_B_NotEqual; break;
    default:
      // <=>
      BogusResult(env);
      return;
    }

    XIL_Exp left = RValue(lhs, env.point, env.post);
    XIL_Exp right = RValue(rhs, env.point, env.post);
    if (SwapCommutativeOperands(Ctx, op, lhs, rhs))
      std::swap(left, right);
    if (!stride)
      CanonicalizeComparison(&binop, &left, &right,
                             leftType->isUnsignedIntegerOrEnumerationType());
    ProcessResult(env, XIL_ExpBinop(binop, left, right, stride, 0, true));
    return;
  }

  if (op == BO_PtrMemD || op == BO_PtrMemI) {
    BogusResult(env);
    return;
  }

  XIL_Exp left = RValue(lhs, env.point, env.post);
  XIL_Exp right = RValue(rhs, env.point, env.post);
  QualType leftType = lhs->getType().getCanonicalType();
  QualType rightType = rhs->getType().getCanonicalType();
  if (SwapCommutativeOperands(Ctx, op, lhs, rhs)) {
    std::swap(left, right);
    std::swap(leftType, rightType);
  }
  XIL_Exp result = MakeArithmetic(op, e->getType(), leftType, left,
                                  rightType, right, e);
  ProcessResult(env, result);
}

void BlockTranslator::TranslateAssign(Env &env, const BinaryOperator *e)
{
  XIL_Location loc = UpdateLocation(*env.point, e);

  const Expr *lhs = e->getLHS();
  const Expr *rhs = e->getRHS();
  XIL_Type type = T.TranslateType(e->getType());

  // Hang any post side effects after this assignment, unless the result of
  // the assignment will itself be used.
  PostEdges postLocal;
  PostEdges *post = env.used() ? env.post : &postLocal;

  XIL_Exp left = LValue(lhs, env.point, post);

  if (e->getOpcode() == BO_Assign) {
    Env rightEnv(env.point, post);
    rightEnv.assign = left;
    rightEnv.assignType = type;
    TranslateExpr(rightEnv, rhs);
  }
  else {
    // x op= y is x = x op y. gcc folds the operation into the type of x.
    const CompoundAssignOperator *ce = cast<CompoundAssignOperator>(e);
    BinaryOperatorKind op = BinaryOperator::getOpForCompoundAssignment(e->getOpcode());
    XIL_Exp right = RValue(rhs, env.point, post);
    XIL_Exp leftValue = XIL_ExpDrf(left);
    QualType leftType = ce->getComputationLHSType().getCanonicalType();
    QualType rightType = rhs->getType().getCanonicalType();
    if (SwapCommutativeOperands(Ctx, op, lhs, rhs)) {
      std::swap(leftValue, right);
      std::swap(leftType, rightType);
    }
    QualType resultType = e->getType()->isIntegerType()
      ? e->getType() : ce->getComputationResultType();
    XIL_Exp value = MakeArithmetic(op, resultType, leftType, leftValue,
                                   rightType, right, e);
    XIL_PPoint after = AddPoint(loc);
    XIL_CFGEdgeAssign(*env.point, after, type, left, value);
    *env.point = after;
  }

  ConnectPostPoint(env.point, postLocal);
  ProcessResult(env, XIL_ExpDrf(left));
}

void BlockTranslator::TranslateLogical(Env &env, const BinaryOperator *e)
{
  XIL_Location loc = UpdateLocation(*env.point, e);
  bool isAnd = (e->getOpcode() == BO_LAnd);

  XIL_PPoint truePoint = env.truePoint;
  XIL_PPoint falsePoint = env.falsePoint;
  XIL_PPoint joinPoint = 0;

  XIL_PPoint midPoint = AddPoint(loc);

  if (!truePoint && !falsePoint) {
    truePoint = AddPoint(loc);
    falsePoint = AddPoint(loc);
    joinPoint = AddPoint(loc);
  }

  if (env.lval) {
    UnexpectedResult(env, "lvalue of logical operator", e);
    return;
  }

  if (env.assign || env.rval) {
    // Assign one or zero to the result on the true/false branches.
    XIL_Exp lval = env.assign;
    XIL_Type type = env.assignType;
    if (env.rval) {
      type = T.TranslateType(e->getType());
      lval = XIL_ExpVar(NewTemporary(type));
      *env.rval = XIL_ExpDrf(lval);
    }
    XIL_CFGEdgeAssign(truePoint, joinPoint, type, lval, XIL_ExpInt(1));
    XIL_CFGEdgeAssign(falsePoint, joinPoint, type, lval, XIL_ExpInt(0));
  }
  else if (joinPoint) {
    // The result is not used.
    XIL_CFGEdgeSkip(truePoint, joinPoint);
    XIL_CFGEdgeSkip(falsePoint, joinPoint);
  }

  Branch(e->getLHS(), env.point,
         isAnd ? midPoint : truePoint, isAnd ? falsePoint : midPoint);
  ConditionalDepth++;
  Branch(e->getRHS(), &midPoint, truePoint, falsePoint);
  ConditionalDepth--;

  *env.point = joinPoint;
  env.processed = true;
}

void BlockTranslator::TranslateConditional(Env &env, const AbstractConditionalOperator *e)
{
  XIL_Location loc = UpdateLocation(*env.point, e);

  // For the GNU x ?: y operator, evaluate x once and use it for both the
  // condition and the true value.
  if (const BinaryConditionalOperator *bco = dyn_cast<BinaryConditionalOperator>(e)) {
    XIL_Exp common = e->getType()->isRecordType() || e->isGLValue()
      ? LValue(bco->getCommon(), env.point)
      : RValue(bco->getCommon(), env.point);
    if (e->getType()->isRecordType() || e->isGLValue())
      common = XIL_ExpDrf(common);
    SavedValues[bco->getOpaqueValue()] = common;
  }

  XIL_PPoint truePoint = AddPoint(loc);
  XIL_PPoint falsePoint = AddPoint(loc);
  Branch(e->getCond(), env.point, truePoint, falsePoint);

  ConditionalDepth++;
  if (env.branch) {
    Branch(e->getTrueExpr(), &truePoint, env.truePoint, env.falsePoint);
    Branch(e->getFalseExpr(), &falsePoint, env.truePoint, env.falsePoint);
  }
  else if (env.lval) {
    // Store the address of whichever operand is chosen in a temporary.
    QualType pointerType = Ctx.getPointerType(e->getType());
    XIL_Type type = T.TranslateType(pointerType);
    XIL_Exp temp = XIL_ExpVar(NewTemporary(type));
    *env.lval = XIL_ExpDrf(temp);
    env.processed = true;

    for (auto [point, operand] : { std::make_pair(&truePoint, e->getTrueExpr()),
                                   std::make_pair(&falsePoint, e->getFalseExpr()) }) {
      XIL_Exp address = LValue(operand, point);
      if (*point) {
        XIL_PPoint after = AddPoint(loc);
        XIL_CFGEdgeAssign(*point, after, type, temp, address);
        *point = after;
      }
    }
  }
  else {
    Env trueEnv(&truePoint);
    Env falseEnv(&falsePoint);

    if (env.assign) {
      trueEnv.assign = falseEnv.assign = env.assign;
      trueEnv.assignType = falseEnv.assignType = env.assignType;
      trueEnv.destroyAssign = falseEnv.destroyAssign = env.destroyAssign;
    }
    else if (env.rval) {
      XIL_Type type = T.TranslateType(e->getType());
      XIL_Exp temp = XIL_ExpVar(NewTemporary(type));
      *env.rval = XIL_ExpDrf(temp);
      trueEnv.assign = falseEnv.assign = temp;
      trueEnv.assignType = falseEnv.assignType = type;

      // A C++ object constructed in the temporary needs to be destroyed.
      if (e->getType()->isRecordType())
        trueEnv.destroyAssign = falseEnv.destroyAssign = Env::DestroyFullExpr;
    }

    TranslateExpr(trueEnv, e->getTrueExpr());
    TranslateExpr(falseEnv, e->getFalseExpr());
  }
  ConditionalDepth--;

  if (truePoint || falsePoint) {
    XIL_PPoint joinPoint = AddPoint(loc);
    XIL_CFGEdgeSkip(truePoint, joinPoint);
    XIL_CFGEdgeSkip(falsePoint, joinPoint);
    *env.point = joinPoint;
  }
  else {
    *env.point = 0;
  }

  env.processed = true;
}

// Get where to put the result of a call: directly into the assigned lvalue if
// there is one, otherwise a temporary. A call returning a reference stores
// the address of the result.
BlockTranslator::CallResult BlockTranslator::PrepareCallResult(Env &env, const CallExpr *e)
{
  CallResult result;
  if (!env.used())
    return result;
  if (env.assign && e->isPRValue()) {
    result.lval = env.assign;
    result.done = true;
    return result;
  }
  result.lval = XIL_ExpVar(NewTemporary(e->getCallReturnType(Ctx)));
  return result;
}

void BlockTranslator::FinishCallResult(Env &env, const CallExpr *e, const CallResult &result)
{
  if (result.done) {
    env.processed = true;
    return;
  }
  if (!result.lval)
    return;
  XIL_Exp value = XIL_ExpDrf(result.lval);
  if (e->isGLValue())
    value = XIL_ExpDrf(value);
  ProcessResult(env, value);
}

// Arguments bound to reference parameters are passed by address.
XIL_Exp BlockTranslator::CallArgument(const Expr *arg, XIL_PPoint *point, bool reference)
{
  if (reference && arg->isGLValue())
    return LValue(arg, point);
  return RValue(arg, point);
}

std::vector<XIL_Exp> BlockTranslator::CallArguments(llvm::ArrayRef<const Expr*> args,
                                                    XIL_PPoint *point,
                                                    const FunctionProtoType *proto)
{
  std::vector<XIL_Exp> result;
  for (size_t i = 0; i < args.size(); i++) {
    bool reference = proto && i < proto->getNumParams() &&
      proto->getParamType(i)->isReferenceType();
    result.push_back(CallArgument(args[i], point, reference));
  }
  return result;
}

void BlockTranslator::AddCall(XIL_PPoint *point, XIL_Type type, XIL_Exp result,
                              XIL_Exp instance, XIL_Exp function,
                              std::vector<XIL_Exp> &args, XIL_Location loc)
{
  if (!*point)
    return;
  XIL_PPoint after = AddPoint(loc ? loc : XIL_CFGGetPointLocation(*point));
  XIL_CFGEdgeCall(*point, after, type, result, instance, function,
                  args.data(), (int) args.size());
  *point = after;
}

void BlockTranslator::TranslateCall(Env &env, const CallExpr *e)
{
  const Expr *callee = e->getCallee()->IgnoreParens();

  if (const CXXMemberCallExpr *mce = dyn_cast<CXXMemberCallExpr>(e)) {
    if (const MemberExpr *me = dyn_cast<MemberExpr>(callee)) {
      const CXXMethodDecl *md = cast<CXXMethodDecl>(me->getMemberDecl());
      llvm::ArrayRef<const Expr*> args(mce->getArgs(), mce->getNumArgs());
      TranslateMemberCall(env, e, md, me->getBase(), me->isArrow(),
                          me->hasQualifier(), args);
      return;
    }
  }

  if (const CXXOperatorCallExpr *oce = dyn_cast<CXXOperatorCallExpr>(e)) {
    const CXXMethodDecl *md = dyn_cast_or_null<CXXMethodDecl>(oce->getCalleeDecl());
    if (md && md->isInstance() && oce->getNumArgs() >= 1) {
      llvm::ArrayRef<const Expr*> args(oce->getArgs() + 1, oce->getNumArgs() - 1);
      TranslateMemberCall(env, e, md, oce->getArg(0), false, false, args);
      return;
    }
  }

  // Calling a pseudo destructor (p->~int()) does nothing.
  if (isa<CXXPseudoDestructorExpr>(callee)) {
    if (env.used())
      ProcessResult(env, XIL_ExpEmpty());
    return;
  }

  XIL_Location loc = UpdateLocation(*env.point, e);

  QualType calleeType = callee->getType().getCanonicalType();
  if (calleeType->isPointerType())
    calleeType = calleeType->getPointeeType();
  if (const MemberPointerType *mpt = calleeType->getAs<MemberPointerType>())
    calleeType = mpt->getPointeeType().getCanonicalType();
  if (!calleeType->isFunctionType()) {
    // Calls through pointers to members have callees with a special bound
    // member function type.
    const BinaryOperator *bo = dyn_cast<BinaryOperator>(callee);
    if (bo && bo->isPtrMemOp()) {
      calleeType = bo->getRHS()->getType()->getAs<MemberPointerType>()->getPointeeType();
    }
    else {
      UnexpectedResult(env, "call", e);
      return;
    }
  }
  XIL_Type funcType = T.TranslateFunctionType(calleeType);

  CallResult result = PrepareCallResult(env, e);

  std::vector<XIL_Exp> args =
    CallArguments(llvm::ArrayRef<const Expr*>(e->getArgs(), e->getNumArgs()), env.point,
                  calleeType->getAs<FunctionProtoType>());

  XIL_Exp function;
  XIL_Exp instance = nullptr;
  if (const BinaryOperator *bo = dyn_cast<BinaryOperator>(callee); bo && bo->isPtrMemOp()) {
    // Calls through pointers to member functions are not handled, as with
    // gcc. Use a bogus function.
    instance = bo->getOpcode() == BO_PtrMemI
      ? RValue(bo->getLHS(), env.point)
      : LValue(bo->getLHS(), env.point);
    RValue(bo->getRHS(), env.point);
    function = XIL_ExpDrf(XIL_ExpVar(XIL_VarGlob("error", "error")));
  }
  else if (const MemberExpr *me = dyn_cast<MemberExpr>(callee);
           me && isa<FunctionDecl>(me->getMemberDecl())) {
    // Static method called with member syntax (x.f()).
    Env baseEnv(env.point);
    TranslateExpr(baseEnv, me->getBase());
    function = XIL_ExpVar(T.FunctionVar(cast<FunctionDecl>(me->getMemberDecl())));
  }
  else {
    function = RValue(e->getCallee(), env.point);
  }

  AddCall(env.point, funcType, result.lval, instance, function, args, loc);
  FinishCallResult(env, e, result);
}

// Find the field of a class for a virtual method, as gcc does from the vtable
// index of a virtual call.
XIL_Field BlockTranslator::VirtualField(const CXXRecordDecl *rd, const CXXMethodDecl *md,
                                        CXXDtorType dtorKind)
{
  if (rd && rd->hasDefinition()) {
    bool isDtor = isa<CXXDestructorDecl>(md);
    for (const Translator::VirtualFunction &function : T.GetVirtualFunctions(rd)) {
      bool matches = isDtor
        ? isa<CXXDestructorDecl>(function.decl) && function.dtorKind == dtorKind
        : function.decl->getCanonicalDecl() == md->getCanonicalDecl();
      if (matches)
        return T.MethodField(function.fieldMethod, function.dtorKind);
    }
  }
  return T.MethodField(md, dtorKind);
}

void BlockTranslator::TranslateMemberCall(Env &env, const CallExpr *e,
                                          const CXXMethodDecl *md, const Expr *object,
                                          bool isArrow, bool qualified,
                                          llvm::ArrayRef<const Expr*> args)
{
  XIL_Location loc = UpdateLocation(*env.point, e);

  CallResult result = PrepareCallResult(env, e);

  // The object to call the method on, which is an lvalue (the address of the
  // object, as in x.f()) or a pointer value (as in p->f()).
  XIL_Exp instance = isArrow
    ? RValue(object, env.point)
    : LValue(object, env.point);

  std::vector<XIL_Exp> xargs =
    CallArguments(args, env.point, md->getType()->getAs<FunctionProtoType>());

  // Virtual calls go through the method's field in the class of the object,
  // unless clang's code generator can resolve the call.
  const CXXMethodDecl *target = md;
  bool isVirtual = md->isVirtual() && !qualified;
  if (isVirtual) {
    if (const CXXMethodDecl *devirtualized = md->getDevirtualizedMethod(object, false)) {
      target = devirtualized;
      isVirtual = false;
    }
  }

  const CXXDestructorDecl *dtor = dyn_cast<CXXDestructorDecl>(target);
  XIL_Exp function;
  if (isVirtual) {
    QualType objectType = object->getType();
    if (isArrow)
      objectType = objectType->getPointeeType();
    XIL_Field field = VirtualField(objectType->getAsCXXRecordDecl(), md, Dtor_Complete);
    function = XIL_ExpDrf(XIL_ExpFld(XIL_ExpEmpty(), field));
  }
  else if (dtor) {
    function = XIL_ExpVar(T.FunctionVar(GlobalDecl(dtor, Dtor_Complete)));
  }
  else {
    function = XIL_ExpVar(T.FunctionVar(target));
  }

  AddCall(env.point, T.TranslateMethodType(md), result.lval, instance, function, xargs, loc);
  FinishCallResult(env, e, result);
}

/////////////////////////////////////////////////////////////////////
// C++ objects
/////////////////////////////////////////////////////////////////////

void BlockTranslator::InitializeObject(XIL_PPoint *point, PostEdges *post, XIL_Exp target,
                                       QualType type, const Expr *init,
                                       Env::Destroy destroy)
{
  if (type->isReferenceType()) {
    XIL_Exp address = LValue(init, point, post);
    if (*point) {
      XIL_PPoint after = AddPoint(XIL_CFGGetPointLocation(*point));
      XIL_CFGEdgeAssign(*point, after, T.TranslateType(type), target, address);
      *point = after;
    }
    return;
  }

  Env env(point, post);
  env.assign = target;
  env.assignType = T.TranslateType(type);
  env.destroyAssign = destroy;
  TranslateExpr(env, init);
}

void BlockTranslator::TranslateMember(Env &env, const MemberExpr *me)
{
  const ValueDecl *member = me->getMemberDecl();

  // Static data members and enumerators are not part of the object.
  if (!isa<FieldDecl>(member) && !isa<IndirectFieldDecl>(member)) {
    Env baseEnv(env.point, env.post);
    TranslateExpr(baseEnv, me->getBase());
    if (const EnumConstantDecl *ecd = dyn_cast<EnumConstantDecl>(member)) {
      std::string str = IntegerString(ecd->getInitVal());
      ProcessResult(env, XIL_ExpIntStr(str.c_str()));
      return;
    }
    if (const FunctionDecl *fd = dyn_cast<FunctionDecl>(member)) {
      // Static methods named with member syntax.
      ProcessResult(env, XIL_ExpDrf(XIL_ExpVar(T.FunctionVar(fd))));
      return;
    }
    if (const VarDecl *vd = dyn_cast<VarDecl>(member)) {
      XIL_Exp result = XIL_ExpDrf(XIL_ExpVar(T.GlobalVar(vd)));
      if (vd->getType()->isReferenceType())
        result = XIL_ExpDrf(result);
      ProcessResult(env, result);
      return;
    }
    UnexpectedResult(env, "member", me);
    return;
  }

  XIL_Exp base = me->isArrow()
    ? RValue(me->getBase(), env.point, env.post)
    : LValue(me->getBase(), env.point, env.post);

  XIL_Exp result = base;
  if (const IndirectFieldDecl *ifd = dyn_cast<IndirectFieldDecl>(member)) {
    for (const NamedDecl *nd : ifd->chain())
      result = XIL_ExpFld(result, T.TranslateField(cast<FieldDecl>(nd)));
  }
  else {
    result = XIL_ExpFld(result, T.TranslateField(cast<FieldDecl>(member)));
  }

  result = XIL_ExpDrf(result);
  if (member->getType()->isReferenceType())
    result = XIL_ExpDrf(result);
  ProcessResult(env, result);
}

// A lambda constructs its closure object by initializing a field for each
// captured variable, as gcc does.
void BlockTranslator::TranslateLambda(Env &env, const LambdaExpr *e)
{
  UpdateLocation(*env.point, e);

  XIL_Exp object = env.assign;
  if (!object)
    object = XIL_ExpVar(NewTemporary(e->getType()));

  const CXXRecordDecl *rd = e->getLambdaClass();
  auto field = rd->field_begin();
  for (const Expr *init : e->capture_inits()) {
    if (field == rd->field_end())
      break;
    if (init) {
      XIL_Exp target = XIL_ExpFld(object, T.TranslateField(*field));
      InitializeObject(env.point, nullptr, target, field->getType(), init);
    }
    ++field;
  }

  if (env.assign)
    env.processed = true;
  else
    ProcessResult(env, XIL_ExpDrf(object));
}

// At the start of a lambda's body, initialize locals for the captured
// variables from the closure's fields.
void BlockTranslator::InitializeCaptures(XIL_PPoint *point, const CXXMethodDecl *md)
{
  const CXXRecordDecl *rd = md->getParent();
  llvm::DenseMap<const ValueDecl*, FieldDecl*> captures;
  FieldDecl *thisField = nullptr;
  rd->getCaptureFields(captures, thisField);

  XIL_Exp closure = XIL_ExpDrf(XIL_ExpVar(XIL_VarThis()));

  // Initialize in field order, so the output doesn't depend on hashing.
  for (const FieldDecl *fd : rd->fields()) {
    const VarDecl *vd = nullptr;
    for (const auto &entry : captures) {
      if (entry.second == fd)
        vd = dyn_cast<VarDecl>(entry.first);
    }
    if (!vd)
      continue;

    bool byReference = fd->getType()->isReferenceType();
    if (byReference && !vd->getType()->isReferenceType())
      ReferenceCaptures.insert(vd);

    // The local has the type of the field: a reference for by-reference
    // captures.
    std::string source = vd->getName().str();
    int &count = LocalNameCounts[source];
    std::string name = count ? source + ":" + std::to_string(count) : source;
    count++;
    XIL_Var var = XIL_VarLocal(name.c_str(), source.c_str());
    XIL_Type type = T.TranslateType(fd->getType());
    XIL_CFGAddVar(var, type, 0);
    Locals[vd] = var;

    XIL_Exp value = XIL_ExpDrf(XIL_ExpFld(closure, T.TranslateField(fd)));
    XIL_PPoint after = AddPoint(XIL_CFGGetPointLocation(*point));
    XIL_CFGEdgeAssign(*point, after, type, XIL_ExpVar(var), value);
    *point = after;
  }

  ThisCapture = thisField;
}

void BlockTranslator::TranslateConstruct(Env &env, const CXXConstructExpr *e)
{
  XIL_Location loc = UpdateLocation(*env.point, e);
  const CXXConstructorDecl *ctor = e->getConstructor();
  QualType type = e->getType();

  // Elided copies construct the copied temporary directly.
  if (e->isElidable() && e->getNumArgs() >= 1) {
    const Expr *arg = e->getArg(0)->IgnoreParenNoopCasts(Ctx);
    if (const MaterializeTemporaryExpr *mte = dyn_cast<MaterializeTemporaryExpr>(arg)) {
      TranslateExpr(env, mte->getSubExpr());
      return;
    }
  }

  if (ctor->isTrivial() && ctor->isCopyOrMoveConstructor() && e->getNumArgs() >= 1) {
    // Trivial copies are assignments.
    TranslateExpr(env, e->getArg(0));
    return;
  }

  // Construct the object in place if it is being assigned somewhere, or in a
  // new temporary otherwise.
  XIL_Exp object = env.assign;
  if (!object && (env.used() || !ctor->isTrivial()))
    object = XIL_ExpVar(NewTemporary(type));

  // Value initialization zeroes the object before any constructor runs.
  if (object && e->requiresZeroInitialization())
    ZeroInitialize(env.point, object, type);

  // Trivial default constructors do nothing.
  if (ctor->isTrivial()) {
    if (env.assign)
      env.processed = true;
    else if (env.used())
      ProcessResult(env, XIL_ExpDrf(object));
    return;
  }

  // Constructing an array calls the constructor on each element. Treat this
  // as a single call on the first element.
  QualType elementType = type;
  XIL_Exp instance = FirstElement(object, &elementType);

  std::vector<XIL_Exp> args =
    CallArguments(llvm::ArrayRef<const Expr*>(e->getArgs(), e->getNumArgs()), env.point,
                  ctor->getType()->getAs<FunctionProtoType>());

  CXXCtorType kind = env.baseSubobject ? Ctor_Base : Ctor_Complete;
  XIL_Exp function = XIL_ExpVar(T.FunctionVar(GlobalDecl(ctor, kind)));

  AddCall(env.point, T.TranslateMethodType(ctor), nullptr, instance, function, args, loc);

  if (env.assign)
    env.processed = true;
  else
    ProcessResult(env, XIL_ExpDrf(object));
}

// Assign zero to every scalar in an object, as gcc does for value
// initialization. As with gcc, at most 100 elements of an array are assigned.
void BlockTranslator::ZeroInitialize(XIL_PPoint *point, XIL_Exp target, QualType type)
{
  if (!*point)
    return;
  type = type.getCanonicalType();

  if (type->isScalarType()) {
    XIL_Exp zero = type->isRealFloatingType() ? XIL_ExpFloat("0.0e+0") : XIL_ExpInt(0);
    XIL_PPoint after = AddPoint(XIL_CFGGetPointLocation(*point));
    XIL_CFGEdgeAssign(*point, after, T.TranslateType(type), target, zero);
    *point = after;
    return;
  }

  if (const ConstantArrayType *cat = Ctx.getAsConstantArrayType(type)) {
    QualType elementType = cat->getElementType();
    XIL_Type xilElementType = T.TranslateType(elementType);
    uint64_t size = std::min<uint64_t>(cat->getZExtSize(), 100);
    for (uint64_t i = 0; i < size; i++) {
      XIL_Exp element = XIL_ExpIndex(target, xilElementType, XIL_ExpInt((long) i));
      ZeroInitialize(point, element, elementType);
    }
    return;
  }

  const RecordDecl *rd = type->getAsRecordDecl();
  if (!rd || !rd->getDefinition())
    return;
  rd = rd->getDefinition();

  if (const CXXRecordDecl *crd = dyn_cast<CXXRecordDecl>(rd)) {
    for (const CXXBaseSpecifier &spec : crd->bases()) {
      const CXXRecordDecl *base = spec.getType()->getAsCXXRecordDecl();
      if (!base || spec.isVirtual())
        continue;
      ZeroInitialize(point, XIL_ExpFld(target, T.BaseField(crd, base)), spec.getType());
    }
  }

  for (const FieldDecl *fd : rd->fields()) {
    if (fd->isUnnamedBitField() || fd->getType()->isReferenceType())
      continue;
    ZeroInitialize(point, XIL_ExpFld(target, T.TranslateField(fd)), fd->getType());
    // Only the first member of a union is zeroed.
    if (rd->isUnion())
      break;
  }
}

void BlockTranslator::TranslateMaterialize(Env &env, const MaterializeTemporaryExpr *e)
{
  UpdateLocation(*env.point, e);

  // Temporaries bound to references that are local variables last until the
  // end of the variable's scope, otherwise until the end of the full
  // expression. Temporaries bound to globals are not destroyed here.
  Env::Destroy destroy = Env::DestroyFullExpr;
  if (e->getExtendingDecl()) {
    destroy = (e->getStorageDuration() == SD_Automatic)
      ? Env::DestroyScope
      : Env::DestroyNone;
  }

  QualType type = e->getType().getNonReferenceType();
  XIL_Exp temp = XIL_ExpVar(NewTemporary(type));
  InitializeObject(env.point, env.post, temp, type, e->getSubExpr(), destroy);
  ProcessResult(env, XIL_ExpDrf(temp));
}

void BlockTranslator::TranslateBindTemporary(Env &env, const CXXBindTemporaryExpr *e)
{
  // Objects constructed directly into a variable, field or other object are
  // destroyed with that object. Otherwise, the object is a temporary that
  // needs to be destroyed.
  if (env.assign) {
    XIL_Exp target = env.assign;
    Env::Destroy destroy = env.destroyAssign;
    TranslateExpr(env, e->getSubExpr());
    if (destroy != Env::DestroyNone)
      AddCleanup(env.point, target, e->getType(), destroy);
    return;
  }

  XIL_Exp value = RValue(e->getSubExpr(), env.point, env.post);
  if (XIL_Exp address = value ? XIL_ExpAddress(value) : nullptr)
    AddCleanup(env.point, address, e->getType(), Env::DestroyFullExpr);
  ProcessResult(env, value);
}

void BlockTranslator::TranslateFullExpr(Env &env, const ExprWithCleanups *e)
{
  // The scope for the temporaries of this full expression, which are
  // destroyed at its end.
  Scope &scope = PushScope();
  scope.cleanupPoint = true;
  scope.conditionalDepth = ConditionalDepth;

  // Leave space at the start to clear the flags of conditionally
  // constructed temporaries.
  XIL_PPoint flagStart = *env.point;
  if (flagStart)
    *env.point = AddPoint(XIL_CFGGetPointLocation(flagStart));
  XIL_PPoint flagEnd = *env.point;

  if (env.branch) {
    // Destroy the temporaries on both sides of the branch.
    XIL_PPoint truePoint = AddPoint(CurrentLocation());
    XIL_PPoint falsePoint = AddPoint(CurrentLocation());
    Branch(e->getSubExpr(), env.point, truePoint, falsePoint);
    env.processed = true;

    EmitCleanups(&truePoint, scope, 0, scope.cleanups.size());
    EmitCleanups(&falsePoint, scope, 0, scope.cleanups.size());
    XIL_CFGEdgeSkip(truePoint, env.truePoint);
    XIL_CFGEdgeSkip(falsePoint, env.falsePoint);
  }
  else {
    TranslateExpr(env, e->getSubExpr());
    EmitCleanups(env.point, scope, 0, scope.cleanups.size());
  }

  PopScope();

  if (flagStart) {
    XIL_PPoint point = flagStart;
    for (XIL_Var flag : scope.flags) {
      XIL_PPoint after = AddPoint(XIL_CFGGetPointLocation(flagStart));
      XIL_CFGEdgeAssign(point, after, T.TranslateType(Ctx.BoolTy), XIL_ExpVar(flag),
                        XIL_ExpInt(0));
      point = after;
    }
    XIL_CFGEdgeSkip(point, flagEnd);
  }
}

void BlockTranslator::TranslateNew(Env &env, const CXXNewExpr *e)
{
  XIL_Location loc = UpdateLocation(*env.point, e);

  const FunctionDecl *opNew = e->getOperatorNew();
  QualType allocType = e->getAllocatedType();

  // Allocate the memory, with a size for the allocated type.
  std::vector<XIL_Exp> args;
  XIL_Exp size = XIL_ExpInt(T.TypeBytes(allocType));
  if (e->isArray()) {
    if (std::optional<const Expr*> count = e->getArraySize()) {
      XIL_Exp xcount = RValue(*count, env.point);
      size = XIL_ExpBinop(XIL_B_Mult, xcount, size, nullptr, 0, true);
    }
  }
  args.push_back(size);
  const FunctionProtoType *newProto =
    opNew ? opNew->getType()->getAs<FunctionProtoType>() : nullptr;
  for (const Expr *arg : e->placement_arguments()) {
    bool reference = newProto && args.size() < newProto->getNumParams() &&
      newProto->getParamType(args.size())->isReferenceType();
    args.push_back(CallArgument(arg, env.point, reference));
  }

  QualType resultType = opNew ? opNew->getReturnType() : Ctx.VoidPtrTy;
  XIL_Exp temp = XIL_ExpVar(NewTemporary(resultType));
  if (opNew) {
    XIL_Exp function = XIL_ExpVar(T.FunctionVar(opNew));
    AddCall(env.point, T.TranslateFunctionType(opNew->getType()), temp, nullptr,
            function, args, loc);
  }

  // Construct the object in the allocated memory.
  XIL_Exp object = XIL_ExpDrf(temp);
  if (const Expr *init = e->getInitializer())
    InitializeObject(env.point, nullptr, object, allocType, init);

  ProcessResult(env, XIL_ExpDrf(temp));
}

void BlockTranslator::AddOperatorDelete(XIL_PPoint *point, XIL_Exp pointer, QualType type,
                                        const FunctionDecl *opDelete)
{
  if (!opDelete)
    return;

  // Pass the size of the object to sized deallocation functions.
  std::vector<XIL_Exp> args;
  args.push_back(pointer);
  if (opDelete->getNumParams() >= 2 &&
      opDelete->getParamDecl(1)->getType()->isIntegerType() &&
      !type->isIncompleteType())
    args.push_back(XIL_ExpInt(T.TypeBytes(type)));

  XIL_Exp function = XIL_ExpVar(T.FunctionVar(opDelete));
  AddCall(point, T.TranslateFunctionType(opDelete->getType()), nullptr, nullptr,
          function, args);
}

void BlockTranslator::TranslateDelete(Env &env, const CXXDeleteExpr *e)
{
  XIL_Location loc = UpdateLocation(*env.point, e);

  const Expr *arg = e->getArgument();
  XIL_Exp pointer = RValue(arg, env.point);

  // Deleting a null pointer does nothing.
  XIL_PPoint deletePoint = AddPoint(loc);
  XIL_PPoint joinPoint = AddPoint(loc);
  XIL_Exp test = MakeTruthValue(pointer, arg->getType(), true);
  XIL_CFGEdgeAssume(*env.point, deletePoint, test, 1);
  XIL_CFGEdgeAssume(*env.point, joinPoint, test, 0);

  QualType type = e->getDestroyedType();
  const CXXRecordDecl *rd = type->getAsCXXRecordDecl();
  const CXXDestructorDecl *dtor =
    (rd && rd->hasDefinition() && !rd->hasTrivialDestructor()) ? rd->getDestructor() : nullptr;

  bool done = false;
  if (dtor && !e->isArrayForm()) {
    if (dtor->isVirtual() && !dtor->getDevirtualizedMethod(arg, false)) {
      // Call the deleting destructor through the object's vtable. This also
      // deallocates the object.
      XIL_Field field = VirtualField(rd, dtor, Dtor_Deleting);
      XIL_Exp function = XIL_ExpDrf(XIL_ExpFld(XIL_ExpEmpty(), field));
      std::vector<XIL_Exp> args;
      AddCall(&deletePoint, T.TranslateMethodType(dtor), nullptr, pointer, function, args);
      done = true;
    }
    else {
      AddDestructorCall(&deletePoint, pointer, dtor, Dtor_Complete);
    }
  }
  if (!done)
    AddOperatorDelete(&deletePoint, pointer, type, e->getOperatorDelete());

  XIL_CFGEdgeSkip(deletePoint, joinPoint);
  *env.point = joinPoint;

  if (env.used())
    ProcessResult(env, XIL_ExpEmpty());
}

void BlockTranslator::AddDestructorCall(XIL_PPoint *point, XIL_Exp instance,
                                        const CXXDestructorDecl *dtor, CXXDtorType kind)
{
  XIL_Exp function = XIL_ExpVar(T.FunctionVar(GlobalDecl(dtor, kind)));
  std::vector<XIL_Exp> args;
  AddCall(point, T.TranslateMethodType(dtor), nullptr, instance, function, args);
}

// Constructing or destroying an array of objects calls the constructor or
// destructor on each element. These are treated as a single call on the
// first element.
XIL_Exp BlockTranslator::FirstElement(XIL_Exp object, QualType *type)
{
  while (const ArrayType *at = Ctx.getAsArrayType(*type)) {
    *type = at->getElementType();
    object = XIL_ExpIndex(object, T.TranslateType(*type), XIL_ExpInt(0));
  }
  return object;
}

// Remember to destroy an object when leaving the current scope (for local
// variables and lifetime extended temporaries) or at the end of the current
// full expression (for other temporaries).
void BlockTranslator::AddCleanup(XIL_PPoint *point, XIL_Exp instance, QualType type,
                                 Env::Destroy destroy)
{
  instance = FirstElement(instance, &type);
  const CXXRecordDecl *rd = type->getAsCXXRecordDecl();
  if (!rd || !rd->hasDefinition() || rd->hasTrivialDestructor())
    return;
  const CXXDestructorDecl *dtor = rd->getDestructor();
  if (!dtor)
    return;

  Scope *scope = &LanguageScope();
  if (destroy == Env::DestroyFullExpr) {
    for (Scope *s = ActiveScope; s; s = s->parent) {
      if (s->cleanupPoint) {
        scope = s;
        break;
      }
    }
  }

  Cleanup cleanup{instance, dtor};

  // Objects constructed in a conditional branch of the full expression are
  // destroyed only if they were constructed.
  if (scope->cleanupPoint && ConditionalDepth > scope->conditionalDepth && *point) {
    cleanup.flag = NewTemporary(Ctx.BoolTy);
    XIL_PPoint after = AddPoint(XIL_CFGGetPointLocation(*point));
    XIL_CFGEdgeAssign(*point, after, T.TranslateType(Ctx.BoolTy),
                      XIL_ExpVar(cleanup.flag), XIL_ExpInt(1));
    *point = after;
    scope->flags.push_back(cleanup.flag);
  }

  scope->cleanups.push_back(cleanup);
}

void BlockTranslator::EmitCleanup(XIL_PPoint *point, const Cleanup &cleanup)
{
  if (!*point)
    return;

  if (!cleanup.flag) {
    AddDestructorCall(point, cleanup.instance, cleanup.dtor, Dtor_Complete);
    return;
  }

  XIL_Location loc = XIL_CFGGetPointLocation(*point);
  XIL_Exp test = XIL_ExpDrf(XIL_ExpVar(cleanup.flag));
  XIL_PPoint destroyPoint = AddPoint(loc);
  XIL_PPoint joinPoint = AddPoint(loc);
  XIL_CFGEdgeAssume(*point, destroyPoint, test, 1);
  XIL_CFGEdgeAssume(*point, joinPoint, test, 0);
  AddDestructorCall(&destroyPoint, cleanup.instance, cleanup.dtor, Dtor_Complete);
  XIL_CFGEdgeSkip(destroyPoint, joinPoint);
  *point = joinPoint;
}

// Destroy objects in reverse order of construction.
void BlockTranslator::EmitCleanups(XIL_PPoint *point, Scope &scope, size_t begin, size_t end)
{
  for (size_t i = end; i > begin; i--)
    EmitCleanup(point, scope.cleanups[i - 1]);
}

// Call the variant of a constructor or destructor with the same arguments.
void BlockTranslator::ForwardToVariant(XIL_PPoint *point, GlobalDecl target)
{
  const CXXMethodDecl *md = cast<CXXMethodDecl>(target.getDecl());
  std::vector<XIL_Exp> args;
  for (const ParmVarDecl *pd : md->parameters())
    args.push_back(XIL_ExpDrf(XIL_ExpVar(TranslateParam(pd))));
  XIL_Exp function = XIL_ExpVar(T.FunctionVar(target));
  AddCall(point, T.TranslateMethodType(md), nullptr, ThisValue(), function, args);
}

void BlockTranslator::TranslateCtorInitializers(XIL_PPoint *point,
                                                const CXXConstructorDecl *ctor,
                                                CXXCtorType kind)
{
  const CXXRecordDecl *rd = ctor->getParent();
  XIL_Exp object = ThisValue();

  for (const CXXCtorInitializer *init : ctor->inits()) {
    // Virtual bases are constructed only by the complete object constructor.
    if (init->isBaseInitializer() && init->isBaseVirtual() && kind == Ctor_Base)
      continue;

    if (init->isWritten())
      UpdateLocation(*point, init->getSourceLocation());

    // Each initializer is a separate statement.
    PostEdges post;
    if (init->isBaseInitializer()) {
      const CXXRecordDecl *base = init->getBaseClass()->getAsCXXRecordDecl();
      XIL_Exp target = XIL_ExpFld(object, T.BaseField(rd, base));
      Env env(point, &post);
      env.assign = target;
      env.assignType = T.TranslateType(init->getBaseClass()->getCanonicalTypeInternal());
      env.baseSubobject = true;
      TranslateExpr(env, init->getInit());
    }
    else if (init->isDelegatingInitializer()) {
      Env env(point, &post);
      env.assign = object;
      env.assignType = T.TranslateType(Ctx.getCanonicalTagType(rd));
      env.baseSubobject = (kind == Ctor_Base);
      TranslateExpr(env, init->getInit());
    }
    else if (const FieldDecl *fd = init->getAnyMember()) {
      XIL_Exp target = object;
      if (const IndirectFieldDecl *ifd = init->getIndirectMember()) {
        for (const NamedDecl *nd : ifd->chain())
          target = XIL_ExpFld(target, T.TranslateField(cast<FieldDecl>(nd)));
      }
      else {
        target = XIL_ExpFld(target, T.TranslateField(fd));
      }
      InitializeObject(point, &post, target, fd->getType(), init->getInit());
    }
    ConnectPostPoint(point, post);
  }
}

// Destroy the members and bases of an object at the end of a base object
// destructor, in reverse order of construction.
void BlockTranslator::TranslateDestructorEpilogue(XIL_PPoint *point,
                                                  const CXXDestructorDecl *dtor)
{
  const CXXRecordDecl *rd = dtor->getParent();
  XIL_Exp object = ThisValue();

  if (!rd->isUnion()) {
    std::vector<const FieldDecl*> fields(rd->field_begin(), rd->field_end());
    for (auto it = fields.rbegin(); it != fields.rend(); ++it) {
      const FieldDecl *fd = *it;
      QualType type = fd->getType();
      XIL_Exp target = FirstElement(XIL_ExpFld(object, T.TranslateField(fd)), &type);
      const CXXRecordDecl *frd = type->getAsCXXRecordDecl();
      if (!frd || !frd->hasDefinition() || frd->hasTrivialDestructor() || frd->isUnion())
        continue;
      if (const CXXDestructorDecl *fdtor = frd->getDestructor())
        AddDestructorCall(point, target, fdtor, Dtor_Complete);
    }
  }

  std::vector<const CXXBaseSpecifier*> bases;
  for (const CXXBaseSpecifier &spec : rd->bases())
    bases.push_back(&spec);
  for (auto it = bases.rbegin(); it != bases.rend(); ++it) {
    if ((*it)->isVirtual())
      continue;
    const CXXRecordDecl *base = (*it)->getType()->getAsCXXRecordDecl();
    if (!base || !base->hasDefinition() || base->hasTrivialDestructor())
      continue;
    if (const CXXDestructorDecl *bdtor = base->getDestructor()) {
      XIL_Exp target = XIL_ExpFld(object, T.BaseField(rd, base));
      AddDestructorCall(point, target, bdtor, Dtor_Base);
    }
  }
}

void BlockTranslator::TranslateVAArg(Env &env, const VAArgExpr *e)
{
  // Model va_arg as a call, as with gcc.
  XIL_Location loc = UpdateLocation(*env.point, e);

  XIL_Type type = T.TranslateType(e->getType());
  XIL_Type argType = XIL_TypePointer(XIL_TypeVoid(), T.PointerWidth, PTR_POINTER);
  XIL_Type funcType = XIL_TypeFunction(type, nullptr, 0, &argType, nullptr, 1,
                                       XIL_MakeAnnotationList());

  XIL_Exp list = RValue(e->getSubExpr(), env.point);
  XIL_Exp function = XIL_ExpVar(XIL_VarFunc("va_arg", "va_arg"));
  XIL_Exp result = XIL_ExpVar(NewTemporary(type));

  XIL_PPoint after = AddPoint(loc);
  XIL_CFGEdgeCall(*env.point, after, funcType, result, nullptr, function, &list, 1);
  *env.point = after;

  ProcessResult(env, XIL_ExpDrf(result));
}

void BlockTranslator::TranslateInitList(Env &env, const InitListExpr *e)
{
  QualType type = e->getType();

  // Braces around a scalar, or an initializer for a struct from another
  // value of the struct type.
  if (e->isTransparent() || type->isScalarType()) {
    if (e->getNumInits())
      TranslateExpr(env, e->getInit(0));
    else
      ProcessResult(env, XIL_ExpInt(0));
    return;
  }

  // Assign to a temporary unless the list is being assigned somewhere.
  XIL_Exp lval;
  XIL_Exp result = nullptr;
  if (env.assign) {
    lval = env.assign;
  }
  else {
    lval = XIL_ExpVar(NewTemporary(type));
    result = XIL_ExpDrf(lval);
  }

  auto assignElement = [&](XIL_Exp elementLval, QualType elementType,
                           const Expr *init) {
    // Elements that are not explicitly initialized are not assigned, as
    // with gcc.
    if (isa<ImplicitValueInitExpr>(init))
      return;
    InitializeObject(env.point, nullptr, elementLval, elementType, init);
  };

  if (const ArrayType *at = Ctx.getAsArrayType(type)) {
    QualType elementType = at->getElementType();
    XIL_Type xilElementType = T.TranslateType(elementType);

    // Limit the number of identical elements from a range designator, as gcc
    // does: [0 ... 999999] = x would otherwise generate a huge CFG.
    const Expr *last = nullptr;
    int repeat = 0;
    for (unsigned i = 0; i < e->getNumInits(); i++) {
      const Expr *init = e->getInit(i);
      repeat = (init == last) ? repeat + 1 : 0;
      last = init;
      if (repeat > 100)
        continue;
      XIL_Exp index = XIL_ExpInt((long) i);
      assignElement(XIL_ExpIndex(lval, xilElementType, index), elementType, init);
    }
  }
  else if (const RecordDecl *rd = type->getAsRecordDecl()) {
    rd = rd->getDefinition();
    if (rd->isUnion()) {
      const FieldDecl *fd = e->getInitializedFieldInUnion();
      if (fd && e->getNumInits())
        assignElement(XIL_ExpFld(lval, T.TranslateField(fd)), fd->getType(),
                      e->getInit(0));
    }
    else {
      // In C++, base classes are initialized before the fields.
      unsigned index = 0;
      if (const CXXRecordDecl *crd = dyn_cast<CXXRecordDecl>(rd)) {
        for (const CXXBaseSpecifier &spec : crd->bases()) {
          const CXXRecordDecl *base = spec.getType()->getAsCXXRecordDecl();
          if (!base || index >= e->getNumInits())
            break;
          const Expr *init = e->getInit(index++);
          XIL_Exp target = XIL_ExpFld(lval, T.BaseField(crd, base));
          if (!isa<ImplicitValueInitExpr>(init)) {
            Env baseEnv(env.point);
            baseEnv.assign = target;
            baseEnv.assignType = T.TranslateType(spec.getType());
            baseEnv.baseSubobject = true;
            TranslateExpr(baseEnv, init);
          }
        }
      }
      for (const FieldDecl *fd : rd->fields()) {
        if (fd->isUnnamedBitField())
          continue;
        if (index >= e->getNumInits())
          break;
        const Expr *init = e->getInit(index++);
        assignElement(XIL_ExpFld(lval, T.TranslateField(fd)), fd->getType(), init);
      }
    }
  }
  else {
    // Vector and complex types are not handled, as with gcc.
    BogusResult(env);
    return;
  }

  if (result)
    ProcessResult(env, result);
  else
    env.processed = true;
}

void BlockTranslator::TranslateStmtExpr(Env &env, const StmtExpr *e)
{
  UpdateLocation(*env.point, e);
  const CompoundStmt *body = e->getSubStmt();

  Scope &scope = PushScope();

  // The value of the statement expression is the value of the last statement.
  unsigned count = body->size();
  unsigned index = 0;
  for (const Stmt *s : body->body()) {
    index++;
    const Expr *value = nullptr;
    if (index == count && env.used()) {
      if (const ValueStmt *vs = dyn_cast<ValueStmt>(s))
        value = vs->getExprStmt();
    }

    if (value) {
      // Labels on the last statement still need to be handled.
      while (const LabelStmt *ls = dyn_cast<LabelStmt>(s)) {
        TranslateLabel(env.point, ls);
        s = ls->getSubStmt();
      }
      TranslateExpr(env, value);
    }
    else {
      PostEdges post;
      Env stmtEnv(env.point, &post);
      if (const Expr *expr = dyn_cast<Expr>(s))
        TranslateExpr(stmtEnv, expr);
      else
        TranslateStmt(env.point, s);
      ConnectPostPoint(env.point, post);
    }
  }

  FinishScope(env.point, scope);

  if (env.used() && !env.processed)
    ProcessResult(env, XIL_ExpEmpty());
}

/////////////////////////////////////////////////////////////////////
// Statements
/////////////////////////////////////////////////////////////////////

Scope &BlockTranslator::PushScope()
{
  AllScopes.emplace_back();
  Scope &scope = AllScopes.back();
  scope.parent = ActiveScope;
  ActiveScope = &scope;
  return scope;
}

void BlockTranslator::PopScope()
{
  ActiveScope = ActiveScope->parent;
}

// The innermost scope that local variables are declared in.
Scope &BlockTranslator::LanguageScope()
{
  for (Scope *scope = ActiveScope; scope; scope = scope->parent) {
    if (!scope->cleanupPoint)
      return *scope;
  }
  return PushScope();
}

ScopeChain BlockTranslator::CurrentChain()
{
  ScopeChain chain;
  for (Scope *scope = ActiveScope; scope; scope = scope->parent)
    chain.push_back({scope, scope->cleanups.size()});
  return chain;
}

void BlockTranslator::ConnectPostPoint(XIL_PPoint *point, const PostEdges &post)
{
  if (!post.start)
    return;
  XIL_CFGEdgeSkip(*point, post.start);
  *point = post.end;
}

// Jump from one point to another, destroying the objects in scopes that are
// left. For each scope at the source, objects that were not yet constructed
// at the target are destroyed, as gcc/xgill.c's XIL_ResolveGoto does.
void BlockTranslator::ResolveGoto(XIL_PPoint source, XIL_PPoint target,
                                  const ScopeChain &sourceChain,
                                  const ScopeChain &targetChain)
{
  if (!source)
    return;

  XIL_PPoint point = source;
  for (const auto &[scope, count] : sourceChain) {
    size_t targetCount = 0;
    bool common = false;
    for (const auto &[targetScope, n] : targetChain) {
      if (targetScope == scope) {
        targetCount = n;
        common = true;
        break;
      }
    }
    if (count > targetCount)
      EmitCleanups(&point, *scope, targetCount, count);
    if (common)
      break;
  }

  XIL_CFGEdgeSkip(point, target);
}

void BlockTranslator::TranslateStmt(XIL_PPoint *point, const Stmt *s)
{
  if (const Expr *e = dyn_cast<Expr>(s)) {
    Env env(point);
    TranslateExpr(env, e);
    return;
  }

  switch (s->getStmtClass()) {
  case Stmt::CompoundStmtClass:
    TranslateCompound(point, cast<CompoundStmt>(s));
    return;

  case Stmt::DeclStmtClass:
    TranslateDeclStmt(point, nullptr, s);
    return;

  case Stmt::CXXForRangeStmtClass:
    TranslateForRange(point, cast<CXXForRangeStmt>(s));
    return;

  case Stmt::CXXTryStmtClass:
    // Exceptions are not modeled, as with gcc.
    TranslateStmt(point, cast<CXXTryStmt>(s)->getTryBlock());
    return;

  case Stmt::ReturnStmtClass:
    TranslateReturn(point, cast<ReturnStmt>(s));
    return;

  case Stmt::IfStmtClass:
    TranslateIf(point, cast<IfStmt>(s));
    return;

  case Stmt::WhileStmtClass:
    TranslateWhile(point, cast<WhileStmt>(s));
    return;

  case Stmt::DoStmtClass:
    TranslateDo(point, cast<DoStmt>(s));
    return;

  case Stmt::ForStmtClass:
    TranslateFor(point, cast<ForStmt>(s));
    return;

  case Stmt::SwitchStmtClass:
    TranslateSwitch(point, cast<SwitchStmt>(s));
    return;

  case Stmt::CaseStmtClass:
  case Stmt::DefaultStmtClass:
    TranslateCase(point, cast<SwitchCase>(s));
    return;

  case Stmt::LabelStmtClass:
    TranslateLabel(point, cast<LabelStmt>(s));
    TranslateStmt(point, cast<LabelStmt>(s)->getSubStmt());
    return;

  case Stmt::GotoStmtClass:
    TranslateGoto(point, cast<GotoStmt>(s));
    return;

  case Stmt::BreakStmtClass:
  case Stmt::ContinueStmtClass: {
    UpdateLocation(*point, s);
    bool isBreak = isa<BreakStmt>(s);
    Scope *scope = ActiveScope;
    while (scope && !(isBreak ? scope->breakPoint : scope->continuePoint))
      scope = scope->parent;
    if (!scope) {
      Unexpected("break/continue", s);
      return;
    }
    ResolveGoto(*point, isBreak ? scope->breakPoint : scope->continuePoint,
                CurrentChain(), isBreak ? scope->breakChain : scope->continueChain);
    *point = 0;
    return;
  }

  case Stmt::NullStmtClass:
    return;

  case Stmt::AttributedStmtClass:
    TranslateStmt(point, cast<AttributedStmt>(s)->getSubStmt());
    return;

  case Stmt::GCCAsmStmtClass:
  case Stmt::MSAsmStmtClass: {
    XIL_Location loc = UpdateLocation(*point, s);
    XIL_PPoint after = AddPoint(loc);
    XIL_CFGEdgeAssembly(*point, after);
    *point = after;
    return;
  }

  default:
    // Computed gotos are not handled, as with gcc.
    Unexpected("statement", s);
    return;
  }
}

void BlockTranslator::TranslateCompound(XIL_PPoint *point, const CompoundStmt *s)
{
  UpdateLocation(*point, s->getLBracLoc());

  Scope &scope = PushScope();

  for (const Stmt *child : s->body()) {
    PostEdges post;
    if (const Expr *e = dyn_cast<Expr>(child)) {
      Env env(point, &post);
      TranslateExpr(env, e);
    }
    else if (isa<DeclStmt>(child)) {
      TranslateDeclStmt(point, &post, child);
    }
    else {
      TranslateStmt(point, child);
    }
    ConnectPostPoint(point, post);
  }

  // Destroy the scope's objects when falling through the end of the block.
  if (*point && !scope.cleanups.empty()) {
    UpdateLocation(*point, s->getRBracLoc());
    EmitCleanups(point, scope, 0, scope.cleanups.size());
  }

  PopScope();
}

// Initialize a static local if its guard variable says it hasn't been
// initialized yet, and register its destructor to run at exit. The calls that
// lock the guard variable are left out.
void BlockTranslator::TranslateStaticLocal(XIL_PPoint *point, const VarDecl *vd)
{
  const Expr *init = vd->getInit();
  bool dynamic = init && !vd->hasConstantInitialization();
  bool destroy = vd->needsDestruction(Ctx) == QualType::DK_cxx_destructor;
  if ((!dynamic && !destroy) || !*point)
    return;

  XIL_Location loc = CurrentLocation();
  std::string guardName;
  {
    llvm::raw_string_ostream out(guardName);
    T.Mangler->mangleStaticGuardVariable(vd, out);
  }
  XIL_Var guard = XIL_VarGlob(guardName.c_str(), guardName.c_str());
  XIL_Exp test = XIL_ExpBinop(XIL_B_Equal, XIL_ExpDrf(XIL_ExpVar(guard)), XIL_ExpInt(0),
                              nullptr, 0, true);

  XIL_PPoint initPoint = AddPoint(loc);
  XIL_PPoint afterPoint = AddPoint(loc);
  XIL_CFGEdgeAssume(*point, initPoint, test, 1);
  XIL_CFGEdgeAssume(*point, afterPoint, test, 0);

  XIL_Exp var = XIL_ExpVar(T.GlobalVar(vd));
  QualType type = vd->getType();
  if (dynamic)
    InitializeObject(&initPoint, nullptr, var, type, init);

  if (destroy) {
    XIL_Exp object = FirstElement(var, &type);
    const CXXRecordDecl *rd = type->getAsCXXRecordDecl();
    if (const CXXDestructorDecl *dtor = rd ? rd->getDestructor() : nullptr) {
      XIL_Type voidPtr = XIL_TypePointer(XIL_TypeVoid(), T.PointerWidth, PTR_POINTER);
      XIL_Type argTypes[3] = { voidPtr, voidPtr, voidPtr };
      XIL_Type atexitType = XIL_TypeFunction(T.TranslateType(Ctx.IntTy), nullptr, 0,
                                             argTypes, nullptr, 3, XIL_MakeAnnotationList());
      std::vector<XIL_Exp> args = {
        XIL_ExpVar(T.FunctionVar(GlobalDecl(dtor, Dtor_Complete))),
        object,
        XIL_ExpVar(XIL_VarGlob("__dso_handle", "__dso_handle")),
      };
      XIL_Exp function = XIL_ExpVar(XIL_VarFunc("__cxa_atexit", "__cxa_atexit"));
      AddCall(&initPoint, atexitType, nullptr, nullptr, function, args);
    }
  }

  XIL_CFGEdgeSkip(initPoint, afterPoint);
  *point = afterPoint;
}

void BlockTranslator::TranslateDeclStmt(XIL_PPoint *point, PostEdges *post, const Stmt *s)
{
  for (const clang::Decl *d : cast<DeclStmt>(s)->decls())
    TranslateDecl(point, post, d);
}

void BlockTranslator::TranslateDecl(XIL_PPoint *point, PostEdges *post,
                                    const clang::Decl *d, bool destroy)
{
  const VarDecl *vd = dyn_cast<VarDecl>(d);
  if (!vd)
    return;

  XIL_Location loc = UpdateLocation(*point, vd->getLocation());

  // Static locals with dynamic initialization or destructors are initialized
  // the first time their declaration is reached.
  if (vd->isStaticLocal() && Ctx.getLangOpts().CPlusPlus)
    TranslateStaticLocal(point, vd);

  // Static locals and extern declarations are globals, with any constant
  // initializers handled separately.
  if (vd->hasGlobalStorage() || vd->hasExternalStorage())
    return;

  XIL_Var var = TranslateLocal(vd);
  XIL_Exp varExp = XIL_ExpVar(var);
  QualType type = vd->getType();

  // Allocate variable length arrays with a call to __xil_alloca, as with gcc.
  if (const VariableArrayType *vat = Ctx.getAsVariableArrayType(type)) {
    XIL_Exp count = RValue(vat->getSizeExpr(), point);
    int elementBytes = T.TypeBytes(vat->getElementType());
    XIL_Exp size = XIL_ExpBinop(XIL_B_Mult, count, XIL_ExpInt(elementBytes),
                                nullptr, 0, true);

    XIL_Type voidPtr = XIL_TypePointer(XIL_TypeVoid(), T.PointerWidth, PTR_POINTER);
    XIL_Type sizeType = XIL_TypeInt(T.PointerWidth, 0, 0);
    XIL_Type allocaType = XIL_TypeFunction(voidPtr, nullptr, 0, &sizeType,
                                           nullptr, 1, XIL_MakeAnnotationList());
    XIL_Exp alloca = XIL_ExpVar(XIL_VarFunc("__xil_alloca", "__xil_alloca"));

    XIL_PPoint after = AddPoint(loc);
    XIL_CFGEdgeCall(*point, after, allocaType, varExp, nullptr, alloca, &size, 1);
    *point = after;
  }

  if (const Expr *init = vd->getInit())
    InitializeObject(point, post, varExp, type, init);

  // The holding variables of structured bindings to tuple-like types.
  if (const DecompositionDecl *dd = dyn_cast<DecompositionDecl>(vd)) {
    for (const BindingDecl *bd : dd->bindings()) {
      if (const VarDecl *holding = bd->getHoldingVar())
        TranslateDecl(point, post, holding, destroy);
    }
  }

  // Destroy the variable at the end of its scope. A variable for the named
  // return value optimization is the returned object, which isn't destroyed.
  if (destroy && !vd->isNRVOVariable() &&
      vd->needsDestruction(Ctx) == QualType::DK_cxx_destructor)
    AddCleanup(point, varExp, type, Env::DestroyScope);
}

void BlockTranslator::TranslateReturn(XIL_PPoint *point, const ReturnStmt *s)
{
  UpdateLocation(*point, s);

  if (const Expr *value = s->getRetValue()) {
    // This is an assignment to the return value, with any post side effects
    // after the assignment.
    PostEdges post;
    if (const VarDecl *nrvo = s->getNRVOCandidate(); nrvo && Func) {
      // The variable is the returned object.
      XIL_Var var = TranslateLocal(nrvo);
      XIL_PPoint after = AddPoint(CurrentLocation());
      XIL_CFGEdgeAssign(*point, after, T.TranslateType(nrvo->getType()),
                        XIL_ExpVar(GetReturnVar()), XIL_ExpDrf(XIL_ExpVar(var)));
      *point = after;
    }
    else if (!value->getType()->isVoidType() && Func) {
      InitializeObject(point, &post, XIL_ExpVar(GetReturnVar()),
                       Func->getReturnType(), value);
    }
    else {
      Env env(point, &post);
      TranslateExpr(env, value);
    }
    ConnectPostPoint(point, post);
  }

  ResolveGoto(*point, ReturnPoint, CurrentChain(), ScopeChain());
  *point = 0;
}

// A statement that is evaluated before a loop or if statement.
void BlockTranslator::TranslateInitStmt(XIL_PPoint *point, const Stmt *init)
{
  PostEdges post;
  if (isa<DeclStmt>(init)) {
    TranslateDeclStmt(point, &post, init);
  }
  else if (const Expr *e = dyn_cast<Expr>(init)) {
    Env env(point, &post);
    TranslateExpr(env, e);
  }
  else {
    TranslateStmt(point, init);
  }
  ConnectPostPoint(point, post);
}

// Leave a scope at the end of a statement, destroying its objects.
void BlockTranslator::FinishScope(XIL_PPoint *point, Scope &scope)
{
  EmitCleanups(point, scope, 0, scope.cleanups.size());
  PopScope();
}

void BlockTranslator::TranslateIf(XIL_PPoint *point, const IfStmt *s)
{
  XIL_Location loc = UpdateLocation(*point, s);

  // Only one branch of if consteval runs at runtime.
  if (s->isConsteval()) {
    const Stmt *taken = s->isNegatedConsteval() ? s->getThen() : s->getElse();
    if (taken)
      TranslateStmt(point, taken);
    return;
  }

  // Variables declared in the init statement or condition are in a scope
  // around the whole statement.
  Scope &scope = PushScope();

  if (const Stmt *init = s->getInit()) {
    TranslateInitStmt(point, init);
    loc = UpdateLocation(*point, s);
  }

  // The discarded branch of if constexpr is not part of the program.
  if (s->isConstexpr()) {
    if (std::optional<const Stmt*> taken = s->getNondiscardedCase(Ctx)) {
      if (*taken)
        TranslateStmt(point, *taken);
      FinishScope(point, scope);
      return;
    }
  }

  if (const DeclStmt *ds = s->getConditionVariableDeclStmt())
    TranslateDeclStmt(point, nullptr, ds);

  XIL_PPoint truePoint = AddPoint(loc);
  XIL_PPoint falsePoint = AddPoint(loc);
  Branch(s->getCond(), point, truePoint, falsePoint);

  TranslateStmt(&truePoint, s->getThen());
  if (s->getElse())
    TranslateStmt(&falsePoint, s->getElse());

  if (truePoint || falsePoint) {
    XIL_PPoint joinPoint = AddPoint(loc);
    XIL_CFGEdgeSkip(truePoint, joinPoint);
    XIL_CFGEdgeSkip(falsePoint, joinPoint);
    *point = joinPoint;
  }
  else {
    *point = 0;
  }

  FinishScope(point, scope);
}

void BlockTranslator::TranslateWhile(XIL_PPoint *point, const WhileStmt *s)
{
  XIL_Location loc = UpdateLocation(*point, s);

  // As with labels, always consider the entry points of loops to be
  // reachable, in case there is a jump into the middle of the loop.
  if (!*point)
    *point = AddPoint(loc);

  XIL_PPoint startPoint = *point;
  XIL_PPoint afterPoint = AddPoint(loc);
  XIL_PPoint bodyPoint = AddPoint(loc);
  XIL_CFGAddLoopHead(startPoint, nullptr);

  ScopeChain outerChain = CurrentChain();
  Scope &scope = PushScope();
  scope.continuePoint = startPoint;
  scope.breakPoint = afterPoint;
  scope.breakChain = outerChain;
  scope.continueChain = CurrentChain();

  // A condition variable is declared on each iteration. Its destruction is
  // not modeled.
  if (const VarDecl *vd = s->getConditionVariable())
    TranslateDecl(point, nullptr, vd, false);

  Branch(s->getCond(), point, bodyPoint, afterPoint);
  TranslateStmt(&bodyPoint, s->getBody());
  XIL_CFGEdgeSkip(bodyPoint, startPoint);

  PopScope();
  *point = afterPoint;
}

void BlockTranslator::TranslateDo(XIL_PPoint *point, const DoStmt *s)
{
  XIL_Location loc = UpdateLocation(*point, s);

  if (!*point)
    *point = AddPoint(loc);

  XIL_PPoint startPoint = *point;
  XIL_PPoint afterPoint = AddPoint(loc);
  XIL_CFGAddLoopHead(startPoint, nullptr);

  ScopeChain outerChain = CurrentChain();
  Scope &scope = PushScope();
  scope.continuePoint = startPoint;
  scope.breakPoint = afterPoint;
  scope.breakChain = outerChain;
  scope.continueChain = CurrentChain();

  TranslateStmt(point, s->getBody());
  Branch(s->getCond(), point, startPoint, afterPoint);

  PopScope();
  *point = afterPoint;
}

void BlockTranslator::TranslateFor(XIL_PPoint *point, const ForStmt *s)
{
  XIL_Location loc = UpdateLocation(*point, s);

  // Variables declared in the initializer are in a scope around the loop.
  Scope &outerScope = PushScope();

  // The initializer is a separate statement before the loop, as with gcc.
  if (const Stmt *init = s->getInit()) {
    TranslateInitStmt(point, init);
    loc = UpdateLocation(*point, s);
  }

  if (!*point)
    *point = AddPoint(loc);

  XIL_PPoint startPoint = *point;
  XIL_PPoint afterPoint = AddPoint(loc);
  XIL_PPoint bodyPoint = AddPoint(loc);
  XIL_PPoint incPoint = AddPoint(loc);
  XIL_CFGAddLoopHead(startPoint, nullptr);

  ScopeChain outerChain = CurrentChain();
  Scope &scope = PushScope();
  scope.continuePoint = incPoint;
  scope.breakPoint = afterPoint;
  scope.breakChain = outerChain;
  scope.continueChain = CurrentChain();

  if (const VarDecl *vd = s->getConditionVariable())
    TranslateDecl(point, nullptr, vd, false);

  if (s->getCond())
    Branch(s->getCond(), point, bodyPoint, afterPoint);
  else
    XIL_CFGEdgeSkip(*point, bodyPoint);

  TranslateStmt(&bodyPoint, s->getBody());
  XIL_CFGEdgeSkip(bodyPoint, incPoint);

  if (const Expr *inc = s->getInc()) {
    PostEdges post;
    Env env(&incPoint, &post);
    TranslateExpr(env, inc);
    ConnectPostPoint(&incPoint, post);
  }

  XIL_CFGEdgeSkip(incPoint, startPoint);
  PopScope();
  *point = afterPoint;
  FinishScope(point, outerScope);
}

void BlockTranslator::TranslateForRange(XIL_PPoint *point, const CXXForRangeStmt *s)
{
  XIL_Location loc = UpdateLocation(*point, s);

  // The range, begin and end variables are in a scope around the loop. A
  // temporary range is lifetime extended by the range variable.
  Scope &outerScope = PushScope();

  if (const Stmt *init = s->getInit())
    TranslateInitStmt(point, init);
  TranslateInitStmt(point, s->getRangeStmt());
  if (const Stmt *begin = s->getBeginStmt())
    TranslateInitStmt(point, begin);
  if (const Stmt *end = s->getEndStmt())
    TranslateInitStmt(point, end);
  loc = UpdateLocation(*point, s);

  if (!*point)
    *point = AddPoint(loc);

  XIL_PPoint startPoint = *point;
  XIL_PPoint afterPoint = AddPoint(loc);
  XIL_PPoint bodyPoint = AddPoint(loc);
  XIL_PPoint incPoint = AddPoint(loc);
  XIL_CFGAddLoopHead(startPoint, nullptr);

  ScopeChain outerChain = CurrentChain();
  Scope &scope = PushScope();
  scope.continuePoint = incPoint;
  scope.breakPoint = afterPoint;
  scope.breakChain = outerChain;
  scope.continueChain = CurrentChain();

  Branch(s->getCond(), point, bodyPoint, afterPoint);

  // The loop variable is declared in each iteration.
  {
    Scope &bodyScope = PushScope();
    TranslateDeclStmt(&bodyPoint, nullptr, s->getLoopVarStmt());
    TranslateStmt(&bodyPoint, s->getBody());
    FinishScope(&bodyPoint, bodyScope);
  }
  XIL_CFGEdgeSkip(bodyPoint, incPoint);

  if (const Expr *inc = s->getInc()) {
    PostEdges post;
    Env env(&incPoint, &post);
    TranslateExpr(env, inc);
    ConnectPostPoint(&incPoint, post);
  }

  XIL_CFGEdgeSkip(incPoint, startPoint);
  PopScope();
  *point = afterPoint;
  FinishScope(point, outerScope);
}

void BlockTranslator::TranslateSwitch(XIL_PPoint *point, const SwitchStmt *s)
{
  XIL_Location loc = UpdateLocation(*point, s);

  // Variables declared in the init statement or condition are in a scope
  // around the whole statement.
  Scope &outerScope = PushScope();

  if (const Stmt *init = s->getInit()) {
    TranslateInitStmt(point, init);
    loc = UpdateLocation(*point, s);
  }
  if (const DeclStmt *ds = s->getConditionVariableDeclStmt())
    TranslateDeclStmt(point, nullptr, ds);

  XIL_Exp test = RValue(s->getCond(), point);
  XIL_PPoint afterPoint = AddPoint(loc);

  ScopeChain outerChain = CurrentChain();
  Scope &scope = PushScope();
  scope.switchTest = test;
  scope.switchPoint = *point;
  scope.breakPoint = afterPoint;
  scope.breakChain = outerChain;

  XIL_PPoint bodyPoint = 0;
  TranslateStmt(&bodyPoint, s->getBody());

  // If we don't jump to any of the case labels, jump to the default label if
  // there is one, else the end of the switch.
  if (scope.defaultPoint) {
    XIL_CFGEdgeSkip(scope.switchPoint, scope.defaultPoint);
  }
  else {
    if (!bodyPoint)
      bodyPoint = AddPoint(loc);
    XIL_CFGEdgeSkip(scope.switchPoint, bodyPoint);
  }

  XIL_CFGEdgeSkip(bodyPoint, afterPoint);
  *point = afterPoint;
  PopScope();
  FinishScope(point, outerScope);
}

void BlockTranslator::TranslateCase(XIL_PPoint *point, const SwitchCase *s)
{
  XIL_Location loc = UpdateLocation(*point, s);

  Scope *scope = ActiveScope;
  while (scope && !scope->switchTest)
    scope = scope->parent;
  if (!scope) {
    Unexpected("case label", s);
    return;
  }

  if (!*point)
    *point = AddPoint(loc);

  if (const CaseStmt *cs = dyn_cast<CaseStmt>(s)) {
    std::string low = IntegerString(cs->getLHS()->EvaluateKnownConstInt(Ctx));
    XIL_Exp lowExp = XIL_ExpIntStr(low.c_str());

    XIL_Exp test;
    if (const Expr *rhs = cs->getRHS()) {
      // GNU case range.
      std::string high = IntegerString(rhs->EvaluateKnownConstInt(Ctx));
      XIL_Exp highExp = XIL_ExpIntStr(high.c_str());
      XIL_Exp ge = XIL_ExpBinop(XIL_B_GreaterEqual, scope->switchTest, lowExp,
                                nullptr, 0, true);
      XIL_Exp le = XIL_ExpBinop(XIL_B_LessEqual, scope->switchTest, highExp,
                                nullptr, 0, true);
      test = XIL_ExpBinop(XIL_B_LogicalAnd, ge, le, nullptr, 0, true);
    }
    else {
      test = XIL_ExpBinop(XIL_B_Equal, scope->switchTest, lowExp, nullptr, 0, true);
    }

    XIL_CFGEdgeAssume(scope->switchPoint, *point, test, 1);
    XIL_PPoint nextPoint = AddPoint(loc);
    XIL_CFGEdgeAssume(scope->switchPoint, nextPoint, test, 0);
    scope->switchPoint = nextPoint;
  }
  else {
    if (scope->defaultPoint)
      Unexpected("duplicate default label", s);
    scope->defaultPoint = *point;
  }

  TranslateStmt(point, s->getSubStmt());
}

void BlockTranslator::TranslateLabel(XIL_PPoint *point, const LabelStmt *s)
{
  XIL_Location loc = UpdateLocation(*point, s);

  LabelData &data = Labels[s->getDecl()];
  if (!*point)
    *point = AddPoint(loc);
  data.point = *point;
  data.chain = CurrentChain();

  // Resolve any gotos to this label we saw before the label.
  for (const GotoData &g : data.gotos) {
    ResolveGoto(g.point, data.point, g.chain, data.chain);
    XIL_CFGAddLoopHead(data.point, nullptr);
  }
  data.gotos.clear();
}

void BlockTranslator::TranslateGoto(XIL_PPoint *point, const GotoStmt *s)
{
  UpdateLocation(*point, s);

  LabelData &data = Labels[s->getLabel()];
  if (data.point) {
    ResolveGoto(*point, data.point, CurrentChain(), data.chain);
    XIL_CFGAddLoopHead(data.point, nullptr);
  }
  else {
    data.gotos.push_back({*point, CurrentChain()});
  }

  *point = 0;
}

/////////////////////////////////////////////////////////////////////
// Blocks
/////////////////////////////////////////////////////////////////////

void BlockTranslator::GenerateFunction(GlobalDecl gd)
{
  const FunctionDecl *fd = cast<FunctionDecl>(gd.getDecl());
  Func = fd;
  DeclName = T.FunctionName(gd);

  // The function's own type carries its annotations.
  XIL_Type type = T.TranslateFunctionType(fd->getType(), fd);
  XIL_Var var = T.FunctionVar(gd);

  XIL_Location beginLoc = T.MakeLocation(fd->getLocation());
  XIL_Location endLoc = T.MakeLocation(fd->getBody()->getEndLoc());
  if (!beginLoc || !endLoc)
    return;

  XIL_SetActiveBlock(var);
  XIL_CFGSetCommand(T.Options.command.c_str());
  XIL_CFGSetBeginLocation(beginLoc);
  XIL_CFGSetEndLocation(endLoc);

  EntryPoint = LastPoint = XIL_CFGAddPoint(beginLoc);
  ExitPoint = ReturnPoint = XIL_CFGAddPoint(endLoc);
  XIL_CFGSetEntryPoint(EntryPoint);
  XIL_CFGSetExitPoint(ExitPoint);

  XIL_CFGAddVar(var, type, 1);

  const CXXMethodDecl *md = dyn_cast<CXXMethodDecl>(fd);
  if (md && md->isInstance())
    XIL_CFGAddVar(XIL_VarThis(), T.TranslateType(md->getThisType()), 0);

  for (const ParmVarDecl *pd : fd->parameters())
    TranslateParam(pd);

  XIL_PPoint point = EntryPoint;

  if (md && md->isInstance() && md->getParent()->isLambda())
    InitializeCaptures(&point, md);

  // The variants of constructors and destructors call each other. The base
  // object variants have the real bodies, as described in
  // local/clang-port-plan.md.
  const CXXConstructorDecl *ctor = dyn_cast<CXXConstructorDecl>(fd);
  const CXXDestructorDecl *dtor = dyn_cast<CXXDestructorDecl>(fd);
  if (ctor && gd.getCtorType() == Ctor_Complete && !ctor->getParent()->getNumVBases() &&
      !ctor->isVariadic()) {
    ForwardToVariant(&point, GlobalDecl(ctor, Ctor_Base));
  }
  else if (dtor && gd.getDtorType() == Dtor_Deleting) {
    ForwardToVariant(&point, GlobalDecl(dtor, Dtor_Complete));
    QualType type = Ctx.getCanonicalTagType(dtor->getParent());
    AddOperatorDelete(&point, ThisValue(), type, dtor->getOperatorDelete());
  }
  else if (dtor && gd.getDtorType() == Dtor_Complete) {
    // Call the base object destructor, then destroy the virtual bases.
    ForwardToVariant(&point, GlobalDecl(dtor, Dtor_Base));
    const CXXRecordDecl *rd = dtor->getParent();
    std::vector<const CXXBaseSpecifier*> vbases;
    for (const CXXBaseSpecifier &spec : rd->vbases())
      vbases.push_back(&spec);
    for (auto it = vbases.rbegin(); it != vbases.rend(); ++it) {
      const CXXRecordDecl *base = (*it)->getType()->getAsCXXRecordDecl();
      if (!base || !base->hasDefinition() || base->hasTrivialDestructor())
        continue;
      if (const CXXDestructorDecl *bdtor = base->getDestructor()) {
        XIL_Exp target = XIL_ExpFld(ThisValue(), T.BaseField(rd, base));
        AddDestructorCall(&point, target, bdtor, Dtor_Base);
      }
    }
  }
  else {
    if (ctor)
      TranslateCtorInitializers(&point, ctor, gd.getCtorType());

    // Returns in a destructor body go to the member and base destructors.
    if (dtor)
      ReturnPoint = XIL_CFGAddPoint(endLoc);

    TranslateStmt(&point, fd->getBody());

    if (dtor) {
      XIL_CFGEdgeSkip(point, ReturnPoint);
      point = ReturnPoint;
      UpdateLocation(point, fd->getBody()->getEndLoc());
      TranslateDestructorEpilogue(&point, dtor);
    }
  }

  // Connect any fall through to the exit point.
  XIL_CFGEdgeSkip(point, ExitPoint);

  XIL_ClearActiveBlock(Dropped);
  XIL_ClearAssociate(XIL_AscBlock);
}

void BlockTranslator::GenerateGlobal(const VarDecl *vd, XIL_Var var)
{
  DeclName = T.GlobalVarName(vd);
  XIL_Type type = T.TranslateType(vd->getType());

  XIL_Location beginLoc = T.MakeLocation(vd->getLocation());
  XIL_Location endLoc = T.MakeLocation(vd->getEndLoc());
  if (!beginLoc || !endLoc)
    return;

  XIL_SetActiveBlock(var);
  XIL_CFGSetCommand(T.Options.command.c_str());
  XIL_CFGSetBeginLocation(beginLoc);
  XIL_CFGSetEndLocation(endLoc);

  EntryPoint = LastPoint = XIL_CFGAddPoint(beginLoc);
  ExitPoint = ReturnPoint = XIL_CFGAddPoint(endLoc);
  XIL_CFGSetEntryPoint(EntryPoint);
  XIL_CFGSetExitPoint(ExitPoint);

  XIL_CFGAddVar(var, type, 1);

  XIL_PPoint point = EntryPoint;

  // Static locals with dynamic initialization are initialized in their
  // function.
  const Expr *init = vd->getInit();
  if (vd->isStaticLocal() && Ctx.getLangOpts().CPlusPlus && !vd->hasConstantInitialization())
    init = nullptr;

  if (init) {
    Env env(&point);
    env.assign = XIL_ExpVar(var);
    env.assignType = type;
    TranslateExpr(env, init);
  }

  XIL_CFGEdgeSkip(point, ExitPoint);

  XIL_ClearActiveBlock(Dropped);
  XIL_ClearAssociate(XIL_AscBlock);
}

void BlockTranslator::GenerateFuncName(const PredefinedExpr *pe, XIL_Var var)
{
  DeclName = XIL_GetVarName(var);
  std::string value = FuncNameString(pe);
  XIL_Type type = XIL_TypeArray(XIL_TypeInt(1, 1, 0), (int) value.size() + 1);

  XIL_Location loc = T.MakeLocation(pe->getLocation());
  if (!loc)
    return;

  XIL_SetActiveBlock(var);
  XIL_CFGSetCommand(T.Options.command.c_str());
  XIL_CFGSetBeginLocation(loc);
  XIL_CFGSetEndLocation(loc);

  EntryPoint = LastPoint = XIL_CFGAddPoint(loc);
  ExitPoint = ReturnPoint = XIL_CFGAddPoint(loc);
  XIL_CFGSetEntryPoint(EntryPoint);
  XIL_CFGSetExitPoint(ExitPoint);

  XIL_CFGAddVar(var, type, 1);

  XIL_Exp str = XIL_ExpString(type, (void*) value.c_str(), (int) value.size() + 1);
  XIL_CFGEdgeAssign(EntryPoint, ExitPoint, type, XIL_ExpVar(var), XIL_ExpDrf(str));

  XIL_ClearActiveBlock(Dropped);
  XIL_ClearAssociate(XIL_AscBlock);
}

} // namespace xgill
