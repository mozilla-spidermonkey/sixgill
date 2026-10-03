
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

#include "clang/AST/Attr.h"
#include "clang/AST/RecordLayout.h"

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
    // gcc represents pointers to member functions as a structure.
    if (canon->isMemberFunctionPointerType())
      return XIL_TypeCSU("__ptr_member", nullptr);
    return XIL_TypeError();

  default:
    // Vectors, complex numbers, etc. are not handled, as with gcc.
    return XIL_TypeError();
  }
}

XIL_Type Translator::TranslateFunctionType(QualType type, const FunctionDecl *annotated)
{
  const FunctionType *ft = type->getAs<FunctionType>();
  if (!ft)
    return XIL_TypeError();

  XIL_Type ret = TranslateType(ft->getReturnType());

  std::vector<XIL_Type> args;
  std::vector<XIL_AnnotationList> argAnnotations;
  bool anyArgAnnotations = false;

  if (const FunctionProtoType *fpt = dyn_cast<FunctionProtoType>(ft)) {
    for (unsigned i = 0; i < fpt->getNumParams(); i++) {
      args.push_back(TranslateType(fpt->getParamType(i)));

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
    XIL_TypeFunction(ret, nullptr, 0, args.data(),
                     anyArgAnnotations ? argAnnotations.data() : nullptr,
                     (int) args.size(), annotations);

  for (XIL_AnnotationList list : argAnnotations)
    XIL_ReleaseAnnotationList(list);
  XIL_ReleaseAnnotationList(annotations);
  return result;
}

/////////////////////////////////////////////////////////////////////
// CSUs
/////////////////////////////////////////////////////////////////////

// Anonymous types are structs/unions without a name of their own, possibly
// behind a pointer or array, as with XIL_IsAnonymous in gcc/type.c.
static const RecordDecl *AnonymousRecord(QualType type)
{
  QualType canon = type.getCanonicalType();
  if (canon->isPointerType())
    canon = canon->getPointeeType().getCanonicalType();
  while (const ArrayType *at = canon->getAsArrayTypeUnsafe())
    canon = at->getElementType().getCanonicalType();

  const RecordDecl *rd = canon->getAsRecordDecl();
  if (!rd)
    return nullptr;
  if (rd->getIdentifier() || rd->getTypedefNameForAnonDecl())
    return nullptr;
  return rd;
}

bool Translator::IsAnonymousType(QualType type)
{
  return AnonymousRecord(type) != nullptr;
}

void Translator::NameAnonymousType(QualType type, const std::string &name)
{
  // The first use of an anonymous type determines its name.
  if (const RecordDecl *rd = AnonymousRecord(type))
    RecordNames.try_emplace(cast<RecordDecl>(rd->getCanonicalDecl()), name);
}

std::string Translator::RecordName(const RecordDecl *rd)
{
  rd = cast<RecordDecl>(rd->getCanonicalDecl());

  auto iter = RecordNames.find(rd);
  if (iter != RecordNames.end())
    return iter->second;

  std::string name;
  if (const IdentifierInfo *id = rd->getIdentifier())
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

XIL_Type Translator::TranslateRecordType(const RecordDecl *rd)
{
  std::string name = RecordName(rd);

  // Generate the CSU the first time we see it, if it is complete.
  const RecordDecl *def = rd->getDefinition();
  int generate = 0;
  XIL_Type csuType =
    XIL_TypeCSU(name.c_str(), (def && !def->isInvalidDecl()) ? &generate : nullptr);
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

  for (const FieldDecl *fd : def->fields()) {
    if (fd->isUnnamedBitField())
      continue;
    XIL_Field field = TranslateField(fd);
    int offset = (int) (layout.getFieldOffset(fd->getFieldIndex()) / 8);
    XIL_CSUAddDataField(field, offset);
  }

  for (const AnnotateAttr *attr : def->specific_attrs<AnnotateAttr>()) {
    std::string value = attr->getAnnotation().str();
    XIL_CSUAddAnnotation("annotate", value.c_str());
  }

  XIL_PopActiveCSU(0);
  return csuType;
}

XIL_Field Translator::TranslateField(const FieldDecl *fd)
{
  const RecordDecl *parent = fd->getParent();
  std::string csuName = RecordName(parent);

  // Unnamed fields (anonymous struct/union members) are named by their index,
  // as with gcc.
  std::string name;
  if (fd->getIdentifier())
    name = fd->getName().str();
  else
    name = "field:" + std::to_string(fd->getFieldIndex());

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

} // namespace xgill
