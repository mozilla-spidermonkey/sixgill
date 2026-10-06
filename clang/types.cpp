
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

// Translation of types and CSUs (struct/union types). This follows
// gcc/type.c.

#include "xgill-clang.h"

#include "clang/AST/ASTLambda.h"
#include "clang/AST/Attr.h"
#include "clang/AST/RecordLayout.h"
#include "clang/Basic/SourceManager.h"

using namespace clang;

namespace xgill {

// Integer type variants, from imlang/type.h.
static const int VARIANT_NONE = 0;
static const int VARIANT_BOOL = 1;
static const int VARIANT_SIZE_T = 2;

XIL_AnnotationList DeclAnnotations(const Decl *d)
{
  XIL_AnnotationList list = XIL_MakeAnnotationList();
  for (const AnnotateAttr *attr : d->specific_attrs<AnnotateAttr>()) {
    std::string value = attr->getAnnotation().str();
    list = XIL_PrependAnnotation(list, "annotate", value.c_str());
  }
  return list;
}

int Translator::TypeBytes(QualType type)
{
  if (type->isIncompleteType() || type->isDependentType())
    return 0;
  return (int) Ctx.getTypeSizeInChars(type).getQuantity();
}

XIL_Type Translator::TranslateIntType(QualType type)
{
  QualType canon = type.getCanonicalType();
  int bytes = (int) (Ctx.getTypeSize(canon) / 8);
  int sign = canon->isSignedIntegerOrEnumerationType() ? 1 : 0;

  // Watch for the bool and size_t typedefs. As with gcc, the C++ bool type
  // itself does not get the bool variant, only a typedef of it.
  QualType walk = type;
  while (true) {
    if (const TypedefType *tt = walk->getAs<TypedefType>()) {
      // getAs skips non-typedef sugar to find the next typedef.
      llvm::StringRef name = tt->getDecl()->getName();
      if (name == "bool")
        return XIL_TypeInt(bytes, sign, VARIANT_BOOL);
      if (name == "size_t")
        return XIL_TypeInt(bytes, sign, VARIANT_SIZE_T);
      walk = tt->desugar();
      continue;
    }
    break;
  }

  return XIL_TypeInt(bytes, sign, VARIANT_NONE);
}

XIL_Type Translator::TranslateType(QualType type)
{
  if (type.isNull())
    return XIL_TypeError();

  QualType canon = type.getCanonicalType();

  switch (canon->getTypeClass()) {
  case Type::Builtin: {
    const BuiltinType *bt = cast<BuiltinType>(canon);
    if (bt->getKind() == BuiltinType::Void)
      return XIL_TypeVoid();
    if (bt->getKind() == BuiltinType::NullPtr)
      return XIL_TypePointer(XIL_TypeVoid(), PointerWidth, PTR_POINTER);
    if (bt->isInteger())
      return TranslateIntType(type);
    if (bt->isFloatingPoint()) {
      // As with gcc, use the precision of the type rather than its size,
      // eg 10 bytes for x87 long double.
      const llvm::fltSemantics &sem = Ctx.getFloatTypeSemantics(canon);
      return XIL_TypeFloat((int) (llvm::APFloat::getSizeInBits(sem) / 8));
    }
    return XIL_TypeError();
  }

  case Type::Enum:
    return TranslateIntType(type);

  // Use the sugared pointee and element types below, so that typedefs like
  // size_t are still visible to TranslateIntType.

  case Type::Pointer:
    return XIL_TypePointer(TranslateType(type->getPointeeType()),
                           PointerWidth, PTR_POINTER);

  case Type::LValueReference:
    return XIL_TypePointer(TranslateType(type->getPointeeType()),
                           PointerWidth, PTR_REFERENCE);

  case Type::RValueReference:
    return XIL_TypePointer(TranslateType(type->getPointeeType()),
                           PointerWidth, PTR_RVALUE_REF);

  case Type::ConstantArray: {
    const ConstantArrayType *at = Ctx.getAsConstantArrayType(type);
    return XIL_TypeArray(TranslateType(at->getElementType()),
                         (int) at->getSize().getZExtValue());
  }

  case Type::IncompleteArray: {
    const IncompleteArrayType *at = Ctx.getAsIncompleteArrayType(type);
    return XIL_TypeArray(TranslateType(at->getElementType()), 0);
  }

  case Type::VariableArray: {
    // Variable sized arrays are treated as pointers, as with gcc.
    const VariableArrayType *at = Ctx.getAsVariableArrayType(type);
    return XIL_TypePointer(TranslateType(at->getElementType()),
                           PointerWidth, PTR_POINTER);
  }

  case Type::Record:
    return TranslateRecordType(canon->getAsRecordDecl());

  case Type::FunctionProto:
  case Type::FunctionNoProto:
    return TranslateFunctionType(type);

  case Type::Atomic:
    return TranslateType(type->castAs<AtomicType>()->getValueType());

  case Type::MemberPointer:
    // gcc represents pointers to member functions as a structure, and
    // doesn't handle pointers to data members.
    if (canon->isMemberFunctionPointerType())
      return TranslateMemberFunctionPointer(canon);
    return XIL_TypeError();

  default:
    // Vectors, complex numbers, etc. are not handled, as with gcc.
    return XIL_TypeError();
  }
}

// Translate a function type. thisCSU is the class of a non-static method.
static XIL_Type MakeFunctionType(Translator &T, QualType type,
                                 const FunctionDecl *annotated,
                                 const char *thisCSU)
{
  const FunctionType *ft = type->getAs<FunctionType>();
  if (!ft)
    return XIL_TypeError();

  XIL_Type ret = T.TranslateType(ft->getReturnType());

  std::vector<XIL_Type> args;
  std::vector<XIL_AnnotationList> argAnnotations;
  bool anyArgAnnotations = false;

  if (const FunctionProtoType *fpt = dyn_cast<FunctionProtoType>(ft)) {
    for (unsigned i = 0; i < fpt->getNumParams(); i++) {
      args.push_back(T.TranslateType(fpt->getParamType(i)));

      XIL_AnnotationList annots = XIL_MakeAnnotationList();
      if (annotated && i < annotated->getNumParams())
        annots = DeclAnnotations(annotated->getParamDecl(i));
      if (annots)
        anyArgAnnotations = true;
      argAnnotations.push_back(annots);
    }
  }

  XIL_AnnotationList annotations =
    annotated ? DeclAnnotations(annotated) : XIL_MakeAnnotationList();

  // gcc never marks function types as varargs, so neither do we.
  XIL_Type result =
    XIL_TypeFunction(ret, thisCSU, 0, args.data(),
                     anyArgAnnotations ? argAnnotations.data() : nullptr,
                     (int) args.size(), annotations);

  for (XIL_AnnotationList list : argAnnotations)
    XIL_ReleaseAnnotationList(list);
  XIL_ReleaseAnnotationList(annotations);
  return result;
}

XIL_Type Translator::TranslateFunctionType(QualType type, const FunctionDecl *annotated)
{
  if (const CXXMethodDecl *md = dyn_cast_or_null<CXXMethodDecl>(annotated))
    return TranslateMethodType(md, true);
  return MakeFunctionType(*this, type, annotated, nullptr);
}

XIL_Type Translator::TranslateMethodType(const CXXMethodDecl *md, bool annotated)
{
  // Translating the class (rather than just naming it) generates its CSU, as
  // gcc's translation of method types does.
  std::string csu;
  if (md->isInstance())
    csu = XIL_GetTypeCSUName(TranslateRecordType(md->getParent()));
  return MakeFunctionType(*this, md->getType(), annotated ? md : nullptr,
                          csu.empty() ? nullptr : csu.c_str());
}

/////////////////////////////////////////////////////////////////////
// CSUs
/////////////////////////////////////////////////////////////////////

// Anonymous types are structs/unions without a name of their own, possibly
// behind a pointer or array, as with XIL_IsAnonymous in gcc/type.c. gcc also
// treats pointers to member functions as anonymous structs.
static QualType StripPointerAndArrays(QualType type)
{
  QualType canon = type.getCanonicalType();
  if (canon->isPointerType())
    canon = canon->getPointeeType().getCanonicalType();
  while (const ArrayType *at = canon->getAsArrayTypeUnsafe())
    canon = at->getElementType().getCanonicalType();
  return canon;
}

static const RecordDecl *AnonymousRecord(QualType type)
{
  const RecordDecl *rd = StripPointerAndArrays(type)->getAsRecordDecl();
  if (!rd)
    return nullptr;
  if (rd->getIdentifier() || rd->getTypedefNameForAnonDecl())
    return nullptr;
  // Lambda closure types are named by their location, not by their first
  // use, which depends on the order things are translated in.
  if (const CXXRecordDecl *crd = dyn_cast<CXXRecordDecl>(rd))
    if (crd->isLambda())
      return nullptr;
  // Anonymous types inside template instantiations are named by gcc's type
  // printer, eg Foo<int>::<unnamed union>.
  if (IsTemplateInstantiation(rd))
    return nullptr;
  return rd;
}

bool Translator::IsAnonymousType(QualType type)
{
  return AnonymousRecord(type) != nullptr ||
    StripPointerAndArrays(type)->isMemberFunctionPointerType();
}

void Translator::NameAnonymousType(QualType type, const std::string &name)
{
  // The first use of an anonymous type determines its name.
  if (const RecordDecl *rd = AnonymousRecord(type))
    RecordNames.try_emplace(cast<RecordDecl>(rd->getCanonicalDecl()), name);

  QualType stripped = StripPointerAndArrays(type);
  if (stripped->isMemberFunctionPointerType())
    MemberPointerNames.try_emplace(stripped.getTypePtr(), name);
}

std::string Translator::RecordName(const RecordDecl *rd)
{
  rd = cast<RecordDecl>(rd->getCanonicalDecl());

  auto iter = RecordNames.find(rd);
  if (iter != RecordNames.end())
    return iter->second;

  std::string name;
  if (Ctx.getLangOpts().CPlusPlus) {
    name = CxxRecordName(rd);
    if (name.empty()) {
      // An anonymous type that we haven't seen a use of yet, or a lambda.
      // These are named by their location, as with the gcc plugin. Lambdas in
      // functions are prefixed with the function's name instead of the file's,
      // as each instantiation of a template has its own closure types.
      const CXXRecordDecl *crd = dyn_cast<CXXRecordDecl>(rd);
      bool lambda = crd && crd->isLambda();
      PresumedLoc ploc = SM.getPresumedLoc(SM.getExpansionLoc(rd->getLocation()));
      const FunctionDecl *function = lambda ?
        dyn_cast_or_null<FunctionDecl>(rd->getParentFunctionOrMethod()) : nullptr;
      std::string prefix;
      if (!function)
        prefix = FileBasename(rd->getLocation());
      else if (isLambdaCallOperator(function))
        // Lambdas within lambdas are named after the enclosing closure type,
        // as gcc and clang mangle lambda functions differently.
        prefix = RecordName(cast<CXXMethodDecl>(function)->getParent());
      else
        prefix = FunctionName(function);
      name = prefix +
        (lambda ? ":<lambda:" : ":<unnamed:") +
        std::to_string(ploc.isValid() ? ploc.getLine() : 0) + ">";
    }
  }
  else if (const IdentifierInfo *id = rd->getIdentifier())
    name = id->getName().str();
  else if (const TypedefNameDecl *td = rd->getTypedefNameForAnonDecl())
    name = td->getName().str();
  else if (rd->field_empty())
    name = "__empty__";
  else
    name = "<unknown-type>";

  RecordNames[rd] = name;
  return name;
}

const Translator::Layout &Translator::GetLayout(const RecordDecl *rd)
{
  const RecordDecl *def = rd->getDefinition();
  if (!def)
    def = rd;

  auto iter = Layouts.find(def);
  if (iter != Layouts.end())
    return iter->second;

  Layout layout;
  int index = 0;

  const CXXRecordDecl *crd = dyn_cast<CXXRecordDecl>(def);
  const ASTRecordLayout *al =
    def->isCompleteDefinition() && !def->isInvalidDecl() ? &Ctx.getASTRecordLayout(def) : nullptr;

  if (crd && al) {
    // The class's own vtable pointer is the first field, but isn't emitted.
    if (al->hasOwnVFPtr())
      index++;

    // The primary base comes first, followed by the other bases.
    const CXXRecordDecl *primary = al->isPrimaryBaseVirtual() ? nullptr : al->getPrimaryBase();
    if (primary) {
      LayoutEntry entry;
      entry.base = primary;
      entry.index = index++;
      entry.offset = 0;
      layout.entries.push_back(entry);
    }
    for (const CXXBaseSpecifier &spec : crd->bases()) {
      if (spec.isVirtual())
        continue;
      const CXXRecordDecl *base = spec.getType()->getAsCXXRecordDecl();
      if (!base || base == primary)
        continue;
      LayoutEntry entry;
      entry.base = base;
      entry.index = index++;
      entry.offset = (int) al->getBaseClassOffset(base).getQuantity();
      layout.entries.push_back(entry);
    }
  }

  for (const FieldDecl *fd : def->fields()) {
    LayoutEntry entry;
    entry.field = fd;
    entry.index = index++;
    if (al)
      entry.offset = (int) (al->getFieldOffset(fd->getFieldIndex()) / 8);
    layout.entries.push_back(entry);
  }

  // Virtual bases (direct or indirect) come last.
  if (crd && al) {
    for (const CXXBaseSpecifier &spec : crd->vbases()) {
      const CXXRecordDecl *base = spec.getType()->getAsCXXRecordDecl();
      if (!base)
        continue;
      LayoutEntry entry;
      entry.base = base;
      entry.index = index++;
      entry.offset = (int) al->getVBaseClassOffset(base).getQuantity();
      layout.entries.push_back(entry);
    }
  }

  return Layouts[def] = std::move(layout);
}

int Translator::FieldIndex(const FieldDecl *fd)
{
  for (const LayoutEntry &entry : GetLayout(fd->getParent()).entries) {
    if (entry.field == fd)
      return entry.index;
  }
  return (int) fd->getFieldIndex();
}

XIL_Field Translator::BaseField(const RecordDecl *rd, const LayoutEntry &entry)
{
  std::string name = "field:" + std::to_string(entry.index);
  std::string csu = RecordName(rd);
  XIL_Type type = TranslateRecordType(entry.base);
  return XIL_MakeField(name.c_str(), name.c_str(), csu.c_str(), type, 0,
                       XIL_MakeAnnotationList());
}

XIL_Field Translator::BaseField(const CXXRecordDecl *rd, const CXXRecordDecl *base)
{
  base = base->getCanonicalDecl();
  for (const LayoutEntry &entry : GetLayout(rd).entries) {
    if (entry.base && entry.base->getCanonicalDecl() == base)
      return BaseField(rd, entry);
  }

  // Empty bases with no layout entry. This shouldn't happen.
  std::string csu = RecordName(rd);
  return XIL_MakeField("field:?", "field:?", csu.c_str(), TranslateRecordType(base), 0,
                       XIL_MakeAnnotationList());
}

XIL_Type Translator::TranslateRecordType(const RecordDecl *rd)
{
  std::string name = RecordName(rd);

  // Generate the CSU the first time we see it, if it is complete.
  const RecordDecl *def = rd->getDefinition();
  int generate = 0;
  bool complete = def && !def->isInvalidDecl() && !def->isDependentType();
  XIL_Type csuType = XIL_TypeCSU(name.c_str(), complete ? &generate : nullptr);
  if (!generate)
    return csuType;

  XIL_PushActiveCSU(name.c_str());
  XIL_CSUSetKind(def->isUnion() ? XIL_CSU_Union : XIL_CSU_Struct);
  XIL_CSUSetCommand(Options.command.c_str());

  // We don't have location information for CSUs, as with gcc.
  XIL_Location empty = XIL_MakeLocation("<empty>", 0);
  XIL_CSUSetBeginLocation(empty);
  XIL_CSUSetEndLocation(empty);

  const ASTRecordLayout &layout = Ctx.getASTRecordLayout(def);
  XIL_CSUSetWidth((int) layout.getSize().getQuantity());

  for (const LayoutEntry &entry : GetLayout(def).entries) {
    if (entry.base) {
      XIL_CSUAddDataField(BaseField(def, entry), entry.offset);
    }
    else if (!entry.field->isUnnamedBitField()) {
      XIL_CSUAddDataField(TranslateField(entry.field), entry.offset);
    }
  }

  if (const CXXRecordDecl *crd = dyn_cast<CXXRecordDecl>(def)) {
    for (const CXXBaseSpecifier &spec : crd->bases()) {
      if (const CXXRecordDecl *base = spec.getType()->getAsCXXRecordDecl()) {
        std::string baseName = RecordName(base);
        XIL_CSUAddBase(baseName.c_str());
      }
    }
    AddCxxMembers(crd);
  }

  for (const AnnotateAttr *attr : def->specific_attrs<AnnotateAttr>()) {
    std::string value = attr->getAnnotation().str();
    XIL_CSUAddAnnotation("annotate", value.c_str());
  }

  XIL_PopActiveCSU(0);
  return csuType;
}

// The name gcc gives the field of a lambda closure: __var for a captured
// variable, or __this.
std::string Translator::LambdaCaptureName(const FieldDecl *fd)
{
  const CXXRecordDecl *rd = dyn_cast<CXXRecordDecl>(fd->getParent());
  if (!rd || !rd->isLambda())
    return "";

  llvm::DenseMap<const ValueDecl*, FieldDecl*> captures;
  FieldDecl *thisField = nullptr;
  rd->getCaptureFields(captures, thisField);
  if (fd == thisField)
    return "__this";
  for (const auto &entry : captures) {
    if (entry.second == fd)
      return "__" + entry.first->getName().str();
  }
  return "";
}

XIL_Field Translator::TranslateField(const FieldDecl *fd)
{
  const RecordDecl *parent = fd->getParent();
  std::string csuName = RecordName(parent);

  // Unnamed fields (anonymous struct/union members) are named by their index,
  // as with gcc. The fields of lambda closures are named after the captured
  // variables, as gcc does.
  std::string name;
  if (fd->getIdentifier())
    name = fd->getName().str();
  else if (std::string capture = LambdaCaptureName(fd); !capture.empty())
    name = capture;
  else
    name = "field:" + std::to_string(FieldIndex(fd));

  // Anonymous field types are named after the field.
  NameAnonymousType(fd->getType(), csuName + ":" + name);

  XIL_Type type;
  if (fd->isBitField()) {
    // gcc gives bit-fields an integer type of the smallest size that holds
    // the bits.
    unsigned bits = fd->getBitWidthValue();
    int bytes = bits <= 8 ? 1 : bits <= 16 ? 2 : bits <= 32 ? 4 : 8;
    bool sign = fd->getType()->isSignedIntegerOrEnumerationType();
    type = XIL_TypeInt(bytes, sign, VARIANT_NONE);
  }
  else {
    type = TranslateType(fd->getType());
  }

  XIL_AnnotationList annotations = DeclAnnotations(fd);
  XIL_Field field = XIL_MakeField(name.c_str(), name.c_str(), csuName.c_str(),
                                  type, 0, annotations);
  XIL_ReleaseAnnotationList(annotations);
  return field;
}

/////////////////////////////////////////////////////////////////////
// Virtual functions
/////////////////////////////////////////////////////////////////////

const std::vector<Translator::VirtualFunction> &
Translator::GetVirtualFunctions(const CXXRecordDecl *rd)
{
  rd = rd->getDefinition();
  auto iter = VirtualFunctions.find(rd);
  if (iter != VirtualFunctions.end())
    return iter->second;

  std::vector<VirtualFunction> list;

  // Functions inherited from each base, updated with any overrides in this
  // class. These are the functions from all base subobjects that are fields
  // of this class (direct bases and virtual bases).
  for (const LayoutEntry &entry : GetLayout(rd).entries) {
    if (!entry.base || !entry.base->hasDefinition())
      continue;
    XIL_Field baseField = BaseField(rd, entry);
    std::vector<VirtualFunction> baseList = GetVirtualFunctions(entry.base);
    for (const VirtualFunction &baseFunction : baseList) {
      VirtualFunction function = baseFunction;
      function.base = baseField;

      const CXXMethodDecl *override = nullptr;
      if (isa<CXXDestructorDecl>(baseFunction.decl))
        override = rd->getDestructor();
      else
        override = baseFunction.decl->getCorrespondingMethodDeclaredInClass(rd);
      if (override && override->getCanonicalDecl() != baseFunction.decl->getCanonicalDecl()) {
        override = override->getCanonicalDecl();
        function.decl = override;
        function.fieldMethod = override;
      }
      list.push_back(function);
    }
  }

  // Virtual functions declared in this class that don't override anything.
  for (const CXXMethodDecl *md : rd->methods()) {
    if (!md->isVirtual())
      continue;
    bool overrides = false;
    for (const VirtualFunction &function : list)
      overrides |= (function.decl->getCanonicalDecl() == md->getCanonicalDecl());
    if (overrides)
      continue;

    if (isa<CXXDestructorDecl>(md)) {
      list.push_back({md, md, Dtor_Complete, nullptr});
      list.push_back({md, md, Dtor_Deleting, nullptr});
    }
    else {
      list.push_back({md, md, Dtor_Complete, nullptr});
    }
  }

  return VirtualFunctions[rd] = std::move(list);
}

XIL_Field Translator::MethodField(const CXXMethodDecl *md, CXXDtorType dtorKind)
{
  std::string csu = RecordName(md->getParent());
  std::string name;
  XIL_Type type;

  if (isa<CXXDestructorDecl>(md)) {
    // gcc's names for the virtual destructor variants.
    name = (dtorKind == Dtor_Deleting) ? "__dt_del " : "__dt_comp ";
    type = XIL_TypeFunction(XIL_TypeVoid(), csu.c_str(), 0, nullptr, nullptr, 0,
                            XIL_MakeAnnotationList());
  }
  else {
    name = md->getNameAsString();
    type = TranslateMethodType(md);
  }

  XIL_AnnotationList annotations = DeclAnnotations(md);
  XIL_Field field = XIL_MakeField(name.c_str(), name.c_str(), csu.c_str(),
                                  type, 1, annotations);
  XIL_ReleaseAnnotationList(annotations);
  return field;
}

void Translator::AddCxxMembers(const CXXRecordDecl *rd)
{
  for (const VirtualFunction &function : GetVirtualFunctions(rd)) {
    XIL_Field field = MethodField(function.fieldMethod, function.dtorKind);

    XIL_Var var = nullptr;
    if (!function.decl->isPureVirtual()) {
      std::string name;
      if (const CXXDestructorDecl *dtor = dyn_cast<CXXDestructorDecl>(function.decl))
        name = FunctionName(GlobalDecl(dtor, function.dtorKind));
      else
        name = FunctionName(function.decl);
      std::string source = SourceName(function.decl);
      var = XIL_VarFunc(name.c_str(), source.c_str());
    }

    XIL_CSUAddFunctionField(field, function.base, var);
  }
}

XIL_Type Translator::TranslateMemberFunctionPointer(QualType type)
{
  QualType canon = type.getCanonicalType();

  std::string name = "__ptr_member";
  auto iter = MemberPointerNames.find(canon.getTypePtr());
  if (iter != MemberPointerNames.end())
    name = iter->second;

  int generate = 0;
  XIL_Type csuType = XIL_TypeCSU(name.c_str(), &generate);
  if (!generate)
    return csuType;

  // The structure gcc uses for pointers to member functions: a pointer to the
  // function, and an adjustment for the this pointer.
  const MemberPointerType *mpt = canon->castAs<MemberPointerType>();
  std::string classCSU =
    XIL_GetTypeCSUName(TranslateRecordType(mpt->getMostRecentCXXRecordDecl()));
  XIL_Type funcType = MakeFunctionType(*this, mpt->getPointeeType(), nullptr,
                                       classCSU.c_str());
  XIL_Type pfnType = XIL_TypePointer(funcType, PointerWidth, PTR_POINTER);
  XIL_Type deltaType = XIL_TypeInt(PointerWidth, 1, VARIANT_NONE);

  XIL_PushActiveCSU(name.c_str());
  XIL_CSUSetKind(XIL_CSU_Struct);
  XIL_CSUSetCommand(Options.command.c_str());
  XIL_Location empty = XIL_MakeLocation("<empty>", 0);
  XIL_CSUSetBeginLocation(empty);
  XIL_CSUSetEndLocation(empty);
  XIL_CSUSetWidth(2 * PointerWidth);

  XIL_AnnotationList none = XIL_MakeAnnotationList();
  XIL_CSUAddDataField(XIL_MakeField("__pfn", "__pfn", name.c_str(), pfnType, 0, none), 0);
  XIL_CSUAddDataField(XIL_MakeField("__delta", "__delta", name.c_str(), deltaType, 0, none),
                      PointerWidth);

  XIL_PopActiveCSU(0);
  return csuType;
}

} // namespace xgill
