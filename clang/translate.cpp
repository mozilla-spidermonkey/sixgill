
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
  case Stmt::ExprWithCleanupsClass:
    TranslateExpr(env, cast<FullExpr>(e)->getSubExpr());
    return;

  case Stmt::ImplicitCastExprClass:
  case Stmt::CStyleCastExprClass:
    TranslateCast(env, cast<CastExpr>(e));
    return;

  case Stmt::DeclRefExprClass: {
    const ValueDecl *d = cast<DeclRefExpr>(e)->getDecl();

    if (const EnumConstantDecl *ecd = dyn_cast<EnumConstantDecl>(d)) {
      std::string str = IntegerString(ecd->getInitVal());
      ProcessResult(env, XIL_ExpIntStr(str.c_str()));
      return;
    }

    XIL_Var var = TranslateValueDecl(d);
    if (!var) {
      UnexpectedResult(env, "declaration reference", e);
      return;
    }

    XIL_Exp result = XIL_ExpDrf(XIL_ExpVar(var));

    // Variable length arrays are pointers, so need an extra dereference.
    if (d->getType()->isVariableArrayType())
      result = XIL_ExpDrf(result);

    ProcessResult(env, result);
    return;
  }

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

  case Stmt::MemberExprClass: {
    const MemberExpr *me = cast<MemberExpr>(e);
    const FieldDecl *fd = dyn_cast<FieldDecl>(me->getMemberDecl());
    if (!fd) {
      UnexpectedResult(env, "member", e);
      return;
    }

    XIL_Exp base = me->isArrow()
      ? RValue(me->getBase(), env.point, env.post)
      : LValue(me->getBase(), env.point, env.post);
    XIL_Field field = T.TranslateField(fd);
    ProcessResult(env, XIL_ExpDrf(XIL_ExpFld(base, field)));
    return;
  }

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
  Branch(e->getRHS(), &midPoint, truePoint, falsePoint);

  *env.point = joinPoint;
  env.processed = true;
}

void BlockTranslator::TranslateConditional(Env &env, const AbstractConditionalOperator *e)
{
  XIL_Location loc = UpdateLocation(*env.point, e);

  if (env.lval) {
    UnexpectedResult(env, "lvalue of conditional operator", e);
    return;
  }

  // For the GNU x ?: y operator, evaluate x once and use it for both the
  // condition and the true value.
  if (const BinaryConditionalOperator *bco = dyn_cast<BinaryConditionalOperator>(e)) {
    XIL_Exp common = RValue(bco->getCommon(), env.point);
    SavedValues[bco->getOpaqueValue()] = common;
  }

  XIL_PPoint truePoint = AddPoint(loc);
  XIL_PPoint falsePoint = AddPoint(loc);
  Branch(e->getCond(), env.point, truePoint, falsePoint);

  if (env.branch) {
    Branch(e->getTrueExpr(), &truePoint, env.truePoint, env.falsePoint);
    Branch(e->getFalseExpr(), &falsePoint, env.truePoint, env.falsePoint);
  }
  else {
    Env trueEnv(&truePoint);
    Env falseEnv(&falsePoint);

    if (env.assign) {
      trueEnv.assign = falseEnv.assign = env.assign;
      trueEnv.assignType = falseEnv.assignType = env.assignType;
    }
    else if (env.rval) {
      XIL_Type type = T.TranslateType(e->getType());
      XIL_Exp temp = XIL_ExpVar(NewTemporary(type));
      *env.rval = XIL_ExpDrf(temp);
      trueEnv.assign = falseEnv.assign = temp;
      trueEnv.assignType = falseEnv.assignType = type;
    }

    TranslateExpr(trueEnv, e->getTrueExpr());
    TranslateExpr(falseEnv, e->getFalseExpr());
  }

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

void BlockTranslator::TranslateCall(Env &env, const CallExpr *e)
{
  XIL_Location loc = UpdateLocation(*env.point, e);

  QualType calleeType = e->getCallee()->getType().getCanonicalType();
  if (calleeType->isPointerType())
    calleeType = calleeType->getPointeeType();
  if (!calleeType->isFunctionType()) {
    UnexpectedResult(env, "call", e);
    return;
  }
  XIL_Type funcType = T.TranslateFunctionType(calleeType);

  // Where to put the result of the call.
  XIL_Exp resultLval = nullptr;
  bool resultDone = false;

  if (env.used()) {
    if (env.assign) {
      resultLval = env.assign;
      resultDone = true;
    }
    else {
      resultLval = XIL_ExpVar(NewTemporary(e->getType()));
    }
  }

  std::vector<XIL_Exp> args;
  for (const Expr *arg : e->arguments())
    args.push_back(RValue(arg, env.point));

  XIL_Exp function = RValue(e->getCallee(), env.point);

  XIL_PPoint after = AddPoint(loc);
  XIL_CFGEdgeCall(*env.point, after, funcType, resultLval, nullptr, function,
                  args.data(), (int) args.size());
  *env.point = after;

  if (resultDone) {
    env.processed = true;
    return;
  }
  if (resultLval)
    ProcessResult(env, XIL_ExpDrf(resultLval));
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
    Env valueEnv(env.point);
    valueEnv.assign = elementLval;
    valueEnv.assignType = T.TranslateType(elementType);
    TranslateExpr(valueEnv, init);
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
    if (rd->isUnion()) {
      const FieldDecl *fd = e->getInitializedFieldInUnion();
      if (fd && e->getNumInits())
        assignElement(XIL_ExpFld(lval, T.TranslateField(fd)), fd->getType(),
                      e->getInit(0));
    }
    else {
      unsigned index = 0;
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
    UnexpectedResult(env, "initializer list", e);
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

  Scope scope;
  PushScope(scope);

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

  PopScope();

  if (env.used() && !env.processed)
    ProcessResult(env, XIL_ExpEmpty());
}

/////////////////////////////////////////////////////////////////////
// Statements
/////////////////////////////////////////////////////////////////////

void BlockTranslator::PushScope(Scope &scope)
{
  scope.parent = ActiveScope;
  ActiveScope = &scope;
}

void BlockTranslator::PopScope()
{
  ActiveScope = ActiveScope->parent;
}

void BlockTranslator::ConnectPostPoint(XIL_PPoint *point, const PostEdges &post)
{
  if (!post.start)
    return;
  XIL_CFGEdgeSkip(*point, post.start);
  *point = post.end;
}

void BlockTranslator::ResolveGoto(XIL_PPoint source, XIL_PPoint target)
{
  // C has no cleanups to run when jumping out of scopes.
  XIL_CFGEdgeSkip(source, target);
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
    for (const clang::Decl *d : cast<DeclStmt>(s)->decls())
      TranslateDecl(point, nullptr, d);
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
    ResolveGoto(*point, isBreak ? scope->breakPoint : scope->continuePoint);
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

  for (const Stmt *child : s->body()) {
    PostEdges post;
    if (const Expr *e = dyn_cast<Expr>(child)) {
      Env env(point, &post);
      TranslateExpr(env, e);
    }
    else if (const DeclStmt *ds = dyn_cast<DeclStmt>(child)) {
      for (const clang::Decl *d : ds->decls())
        TranslateDecl(point, &post, d);
    }
    else {
      TranslateStmt(point, child);
    }
    ConnectPostPoint(point, post);
  }
}

void BlockTranslator::TranslateDecl(XIL_PPoint *point, PostEdges *post,
                                    const clang::Decl *d)
{
  const VarDecl *vd = dyn_cast<VarDecl>(d);
  if (!vd)
    return;

  XIL_Location loc = UpdateLocation(*point, vd->getLocation());

  // Static locals and extern declarations are globals, with their
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

  const Expr *init = vd->getInit();
  if (!init)
    return;

  Env env(point, post);
  env.assign = varExp;
  env.assignType = T.TranslateType(type);
  TranslateExpr(env, init);
}

void BlockTranslator::TranslateReturn(XIL_PPoint *point, const ReturnStmt *s)
{
  UpdateLocation(*point, s);

  if (const Expr *value = s->getRetValue()) {
    // This is an assignment to the return value, with any post side effects
    // after the assignment.
    PostEdges post;
    Env env(point, &post);
    if (!value->getType()->isVoidType()) {
      env.assign = XIL_ExpVar(GetReturnVar());
      env.assignType = T.TranslateType(Func->getReturnType());
    }
    TranslateExpr(env, value);
    ConnectPostPoint(point, post);
  }

  ResolveGoto(*point, ExitPoint);
  *point = 0;
}

void BlockTranslator::TranslateIf(XIL_PPoint *point, const IfStmt *s)
{
  XIL_Location loc = UpdateLocation(*point, s);

  if (s->getInit() || s->getConditionVariable() || s->isConsteval()) {
    Unexpected("if statement", s);
    return;
  }

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
}

void BlockTranslator::TranslateWhile(XIL_PPoint *point, const WhileStmt *s)
{
  XIL_Location loc = UpdateLocation(*point, s);

  if (s->getConditionVariable()) {
    Unexpected("while statement", s);
    return;
  }

  // As with labels, always consider the entry points of loops to be
  // reachable, in case there is a jump into the middle of the loop.
  if (!*point)
    *point = AddPoint(loc);

  XIL_PPoint startPoint = *point;
  XIL_PPoint afterPoint = AddPoint(loc);
  XIL_PPoint bodyPoint = AddPoint(loc);
  XIL_CFGAddLoopHead(startPoint, nullptr);

  Scope scope;
  scope.continuePoint = startPoint;
  scope.breakPoint = afterPoint;
  PushScope(scope);

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

  Scope scope;
  scope.continuePoint = startPoint;
  scope.breakPoint = afterPoint;
  PushScope(scope);

  TranslateStmt(point, s->getBody());
  Branch(s->getCond(), point, startPoint, afterPoint);

  PopScope();
  *point = afterPoint;
}

void BlockTranslator::TranslateFor(XIL_PPoint *point, const ForStmt *s)
{
  XIL_Location loc = UpdateLocation(*point, s);

  if (s->getConditionVariable()) {
    Unexpected("for statement", s);
    return;
  }

  // The initializer is a separate statement before the loop, as with gcc.
  if (const Stmt *init = s->getInit()) {
    PostEdges post;
    if (const DeclStmt *ds = dyn_cast<DeclStmt>(init)) {
      for (const clang::Decl *d : ds->decls())
        TranslateDecl(point, &post, d);
    }
    else if (const Expr *e = dyn_cast<Expr>(init)) {
      Env env(point, &post);
      TranslateExpr(env, e);
    }
    else {
      TranslateStmt(point, init);
    }
    ConnectPostPoint(point, post);
    loc = UpdateLocation(*point, s);
  }

  if (!*point)
    *point = AddPoint(loc);

  XIL_PPoint startPoint = *point;
  XIL_PPoint afterPoint = AddPoint(loc);
  XIL_PPoint bodyPoint = AddPoint(loc);
  XIL_PPoint incPoint = AddPoint(loc);
  XIL_CFGAddLoopHead(startPoint, nullptr);

  Scope scope;
  scope.continuePoint = incPoint;
  scope.breakPoint = afterPoint;
  PushScope(scope);

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
}

void BlockTranslator::TranslateSwitch(XIL_PPoint *point, const SwitchStmt *s)
{
  XIL_Location loc = UpdateLocation(*point, s);

  if (s->getInit() || s->getConditionVariable()) {
    Unexpected("switch statement", s);
    return;
  }

  XIL_Exp test = RValue(s->getCond(), point);
  XIL_PPoint afterPoint = AddPoint(loc);

  Scope scope;
  scope.switchTest = test;
  scope.switchPoint = *point;
  scope.breakPoint = afterPoint;
  PushScope(scope);

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

  // Resolve any gotos to this label we saw before the label.
  for (XIL_PPoint source : data.gotos) {
    ResolveGoto(source, data.point);
    XIL_CFGAddLoopHead(data.point, nullptr);
  }
  data.gotos.clear();
}

void BlockTranslator::TranslateGoto(XIL_PPoint *point, const GotoStmt *s)
{
  UpdateLocation(*point, s);

  LabelData &data = Labels[s->getLabel()];
  if (data.point) {
    ResolveGoto(*point, data.point);
    XIL_CFGAddLoopHead(data.point, nullptr);
  }
  else {
    data.gotos.push_back(*point);
  }

  *point = 0;
}

/////////////////////////////////////////////////////////////////////
// Blocks
/////////////////////////////////////////////////////////////////////

void BlockTranslator::GenerateFunction(const FunctionDecl *fd)
{
  Func = fd;
  DeclName = T.FunctionName(fd);

  // The function's own type carries its annotations.
  XIL_Type type = T.TranslateFunctionType(fd->getType(), fd);
  XIL_Var var = T.FunctionVar(fd);

  XIL_Location beginLoc = T.MakeLocation(fd->getLocation());
  XIL_Location endLoc = T.MakeLocation(fd->getBody()->getEndLoc());
  if (!beginLoc || !endLoc)
    return;

  XIL_SetActiveBlock(var);
  XIL_CFGSetCommand(T.Options.command.c_str());
  XIL_CFGSetBeginLocation(beginLoc);
  XIL_CFGSetEndLocation(endLoc);

  EntryPoint = LastPoint = XIL_CFGAddPoint(beginLoc);
  ExitPoint = XIL_CFGAddPoint(endLoc);
  XIL_CFGSetEntryPoint(EntryPoint);
  XIL_CFGSetExitPoint(ExitPoint);

  XIL_CFGAddVar(var, type, 1);
  for (const ParmVarDecl *pd : fd->parameters())
    TranslateParam(pd);

  XIL_PPoint point = EntryPoint;
  TranslateStmt(&point, fd->getBody());

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
  ExitPoint = XIL_CFGAddPoint(endLoc);
  XIL_CFGSetEntryPoint(EntryPoint);
  XIL_CFGSetExitPoint(ExitPoint);

  XIL_CFGAddVar(var, type, 1);

  XIL_PPoint point = EntryPoint;
  if (const Expr *init = vd->getInit()) {
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
  ExitPoint = XIL_CFGAddPoint(loc);
  XIL_CFGSetEntryPoint(EntryPoint);
  XIL_CFGSetExitPoint(ExitPoint);

  XIL_CFGAddVar(var, type, 1);

  XIL_Exp str = XIL_ExpString(type, (void*) value.c_str(), (int) value.size() + 1);
  XIL_CFGEdgeAssign(EntryPoint, ExitPoint, type, XIL_ExpVar(var), XIL_ExpDrf(str));

  XIL_ClearActiveBlock(Dropped);
  XIL_ClearAssociate(XIL_AscBlock);
}

} // namespace xgill
