
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

// Names of functions and global variables. These follow the rules in
// gcc/variable.c (XIL_GlobalName), except that C++ symbols with internal
// linkage get a file: prefix so that they don't collide across translation
// units.

#include "xgill-clang.h"

#include "clang/AST/GlobalDecl.h"
#include "clang/AST/Mangle.h"

using namespace clang;

namespace xgill {

std::string Translator::SourceName(const NamedDecl *d)
{
  if (const IdentifierInfo *id = d->getIdentifier())
    return id->getName().str();
  return d->getNameAsString();
}

// Whether a declaration should be named with its mangled name.
static bool UseMangledName(ASTContext &ctx, MangleContext &mangler,
                           const NamedDecl *d)
{
  if (!ctx.getLangOpts().CPlusPlus)
    return false;
  return mangler.shouldMangleDeclName(d);
}

static std::string MangledName(MangleContext &mangler, GlobalDecl gd)
{
  std::string name;
  llvm::raw_string_ostream out(name);
  mangler.mangleName(gd, out);
  return name;
}

std::string Translator::FunctionName(const FunctionDecl *fd)
{
  fd = fd->getCanonicalDecl();

  if (UseMangledName(Ctx, *Mangler, fd)) {
    GlobalDecl gd;
    if (const CXXConstructorDecl *ctor = dyn_cast<CXXConstructorDecl>(fd))
      gd = GlobalDecl(ctor, Ctor_Complete);
    else if (const CXXDestructorDecl *dtor = dyn_cast<CXXDestructorDecl>(fd))
      gd = GlobalDecl(dtor, Dtor_Complete);
    else
      gd = GlobalDecl(fd);
    std::string name = MangledName(*Mangler, gd);
    if (!fd->isExternallyVisible())
      return FileBasename(fd->getLocation()) + ":" + name;
    return name;
  }

  std::string name = SourceName(fd);

  // Static functions are prefixed with the name of the file they are in. As a
  // special case, main() is always file-prefixed.
  if (!fd->isExternallyVisible() || fd->isMain())
    return FileBasename(fd->getLocation()) + ":" + name;
  return name;
}

std::string Translator::GlobalVarName(const VarDecl *vd)
{
  vd = vd->getCanonicalDecl();

  if (UseMangledName(Ctx, *Mangler, vd)) {
    std::string name = MangledName(*Mangler, GlobalDecl(vd));
    if (!vd->isExternallyVisible() && !vd->isStaticLocal())
      return FileBasename(vd->getLocation()) + ":" + name;
    return name;
  }

  std::string name = SourceName(vd);

  // Static locals are prefixed with the name of their function.
  if (vd->isStaticLocal()) {
    if (const FunctionDecl *fd =
          dyn_cast_or_null<FunctionDecl>(vd->getParentFunctionOrMethod()))
      return FunctionName(fd) + ":" + name;
  }

  if (!vd->isExternallyVisible())
    return FileBasename(vd->getLocation()) + ":" + name;
  return name;
}

XIL_Var Translator::FunctionVar(const FunctionDecl *fd)
{
  std::string name = FunctionName(fd);
  std::string source = SourceName(fd);
  return XIL_VarFunc(name.c_str(), source.c_str());
}

XIL_Var Translator::GlobalVar(const VarDecl *vd)
{
  std::string name = GlobalVarName(vd);
  std::string source = SourceName(vd);
  XIL_Var var = XIL_VarGlob(name.c_str(), source.c_str());

  // Name any anonymous struct type after the variable.
  NameAnonymousType(vd->getType(), name);
  return var;
}

XIL_Var Translator::FuncNameVar(const PredefinedExpr *pe, const FunctionDecl *fd)
{
  // gcc treats __func__ and friends as static locals of the function.
  std::string source = PredefinedExpr::getIdentKindName(pe->getIdentKind()).str();
  std::string name = FunctionName(fd) + ":" + source;
  XIL_Var var = XIL_VarGlob(name.c_str(), source.c_str());

  if (FuncNamesGenerated.insert(name).second)
    PendingFuncNames.push_back({pe, var});
  return var;
}

} // namespace xgill
