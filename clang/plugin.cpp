
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

// Plugin entry point: argument handling, and walking the translation unit
// to find the functions and globals to generate.
//
// Usage:
//   clang -fplugin=xgill-clang.so -fplugin-arg-xgill-remote=ADDR ...
//
// Arguments (each as -fplugin-arg-xgill-NAME=VALUE):
//   remote   address of the xmanager to send results to. Without this, the
//            results are written to xdb.sqlite in the current directory.
//   log      file to append diagnostics to (default stdout).
//   basedir  directory that generated file names are made relative to.

#include "xgill-clang.h"

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendPluginRegistry.h"

using namespace clang;

namespace xgill {

static FILE *g_log = nullptr;

FILE *LogFile()
{
  return g_log ? g_log : stdout;
}

Translator::Translator(ASTContext &ctx, const PluginOptions &options)
  : Ctx(ctx), SM(ctx.getSourceManager()), Options(options)
{
  PointerWidth = ctx.getTargetInfo().getPointerWidth(LangAS::Default) / 8;
  Mangler.reset(ctx.createMangleContext());
}

XIL_Location Translator::MakeLocation(SourceLocation loc)
{
  if (loc.isInvalid())
    return nullptr;

  // Use the location of the macro expansion for code inside macros, and
  // respect #line directives, as gcc does.
  PresumedLoc ploc = SM.getPresumedLoc(SM.getExpansionLoc(loc));
  if (ploc.isInvalid())
    return nullptr;
  return XIL_MakeLocation(ploc.getFilename(), ploc.getLine());
}

std::string Translator::FileBasename(SourceLocation loc)
{
  PresumedLoc ploc = SM.getPresumedLoc(SM.getExpansionLoc(loc));
  if (ploc.isInvalid())
    return "<unknown>";
  llvm::StringRef file = ploc.getFilename();
  size_t slash = file.rfind('/');
  if (slash != llvm::StringRef::npos)
    file = file.substr(slash + 1);
  return file.str();
}

namespace {

// Collects the functions and globals to generate, in source order. The types
// of global variable declarations are also translated, so that the CSUs they
// refer to are generated, as with the gcc plugin.
class DeclCollector : public RecursiveASTVisitor<DeclCollector> {
public:
  struct Item {
    const Decl *decl;
    bool generate;
  };
  std::vector<Item> Decls;

  bool shouldVisitTemplateInstantiations() const { return true; }
  bool shouldVisitImplicitCode() const { return true; }

  // Visit declarations nested in a function (lambda call operators, local
  // classes' methods, static locals) before the function itself. This is the
  // order gcc finishes them in, which matters for how anonymous types are
  // named by their first use.
  bool shouldTraversePostOrder() const { return true; }

  bool VisitFunctionDecl(FunctionDecl *fd) {
    // Trivial special members (eg implicit default constructors that do
    // nothing) are not real functions, and gcc does not generate them.
    if (fd->isTrivial())
      return true;
    if (fd->doesThisDeclarationHaveABody() && !fd->isDependentContext() &&
        !fd->isTemplated() &&
        !fd->isInvalidDecl() && !fd->isDeleted())
      Decls.push_back({fd, true});
    return true;
  }

  bool VisitVarDecl(VarDecl *vd) {
    // Global variables and static locals get an initializer block, whether
    // or not they have an initializer, as with the gcc plugin.
    if (isa<ParmVarDecl>(vd) || !vd->hasGlobalStorage() || vd->isTemplated() ||
        vd->isInvalidDecl() || vd->getDeclContext()->isDependentContext())
      return true;
    bool definition =
      vd->isThisDeclarationADefinition() != VarDecl::DeclarationOnly;
    Decls.push_back({vd, definition});
    return true;
  }
};

} // anonymous namespace

void Translator::TranslateUnit()
{
  DeclCollector collector;
  collector.TraverseDecl(Ctx.getTranslationUnitDecl());

  for (const DeclCollector::Item &item : collector.Decls) {
    const Decl *d = item.decl;

    if (!item.generate) {
      // gcc translates the types of global variable declarations, but not of
      // function declarations.
      if (const VarDecl *vd = dyn_cast<VarDecl>(d)) {
        GlobalVar(vd);
        TranslateType(vd->getType());
      }
      continue;
    }

    if (!Generated.insert(d).second)
      continue;

    if (const FunctionDecl *fd = dyn_cast<FunctionDecl>(d)) {
      {
        BlockTranslator block(*this, fd);
        block.GenerateFunction(fd);
      }

      std::vector<PendingFuncName> pending;
      pending.swap(PendingFuncNames);
      for (const PendingFuncName &p : pending) {
        BlockTranslator block(*this, fd);
        block.GenerateFuncName(p.expr, p.var);
      }
    }
    else if (const VarDecl *vd = dyn_cast<VarDecl>(d)) {
      // Only generate one block for a variable with multiple tentative
      // definitions.
      const VarDecl *def = vd->getDefinition();
      if (!def)
        def = vd->getActingDefinition();
      if (def && def != vd && !Generated.insert(def).second)
        continue;
      XIL_Var var = GlobalVar(vd);
      BlockTranslator block(*this, vd);
      block.GenerateGlobal(def ? def : vd, var);
    }
  }
}

namespace {

class XgillConsumer : public ASTConsumer {
public:
  XgillConsumer(CompilerInstance &ci, const PluginOptions &options)
    : CI(ci), Options(options) {}

  void HandleTranslationUnit(ASTContext &ctx) override {
    // Don't try to generate anything for code with errors.
    if (ctx.getDiagnostics().hasErrorOccurred())
      return;

    if (!Options.log.empty()) {
      g_log = fopen(Options.log.c_str(), "a");
      if (g_log)
        XIL_SetLogFile(g_log);
    }

    XIL_SetupGenerate(Options.remote.empty() ? nullptr : Options.remote.c_str());
    XIL_SetNormalizeDirectory(Options.basedir.empty() ? nullptr : Options.basedir.c_str());

    Translator translator(ctx, Options);
    if (CI.hasSema())
      translator.S = &CI.getSema();
    translator.TranslateUnit();

    XIL_WriteGenerated();

    if (g_log) {
      fclose(g_log);
      g_log = nullptr;
    }
  }

private:
  CompilerInstance &CI;

  // A copy: the action holding the original may be destroyed before the end
  // of the translation unit.
  PluginOptions Options;
};

class XgillAction : public PluginASTAction {
public:
  std::unique_ptr<ASTConsumer>
  CreateASTConsumer(CompilerInstance &ci, llvm::StringRef) override {
    return std::make_unique<XgillConsumer>(ci, Options);
  }

  bool ParseArgs(const CompilerInstance &ci,
                 const std::vector<std::string> &args) override {
    for (const std::string &arg : args) {
      size_t eq = arg.find('=');
      std::string key = arg.substr(0, eq);
      std::string value = (eq == std::string::npos) ? "" : arg.substr(eq + 1);

      if (key == "remote")
        Options.remote = value;
      else if (key == "log")
        Options.log = value;
      else if (key == "basedir")
        Options.basedir = value;
      else
        llvm::errs() << "WARNING: xgill unrecognized argument " << key << "\n";
    }

    // The command used to invoke the compiler comes from the environment, as
    // it may contain characters that can't be passed in plugin arguments.
    const char *command = getenv("XGILL_COMMAND");
    Options.command = command ? command : "UNKNOWN";
    return true;
  }

  // Run alongside the normal compilation, without needing -add-plugin.
  ActionType getActionType() override { return AddAfterMainAction; }

private:
  PluginOptions Options;
};

} // anonymous namespace

} // namespace xgill

static clang::FrontendPluginRegistry::Add<xgill::XgillAction>
  X("xgill", "generate sixgill CFGs");
