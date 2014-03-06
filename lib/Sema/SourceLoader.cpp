//===--- SourceLoader.cpp - Import .swift files as modules ------*- c++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief A simple module loader that loads .swift source files.
///
//===----------------------------------------------------------------------===//

#include "swift/Sema/SourceLoader.h"
#include "swift/Subsystems.h"
#include "swift/AST/AST.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/Parse/DelayedParsingCallbacks.h"
#include "swift/Parse/PersistentParserState.h"
#include "swift/Basic/SourceManager.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/system_error.h"

using namespace swift;

// FIXME: Basically the same as SerializedModuleLoader.
static llvm::error_code findModule(ASTContext &ctx, StringRef moduleID,
                                   SourceLoc importLoc,
                                   std::unique_ptr<llvm::MemoryBuffer> &buffer){
  llvm::SmallString<128> inputFilename;

  for (auto Path : ctx.SearchPathOpts.ImportSearchPaths) {
    inputFilename = Path;
    llvm::sys::path::append(inputFilename, moduleID);
    inputFilename.append(".swift");
    auto err = llvm::MemoryBuffer::getFile(inputFilename.str(), buffer);
    if (!err || err.value() != llvm::errc::no_such_file_or_directory)
      return err;
  }

  return make_error_code(llvm::errc::no_such_file_or_directory);
}

namespace {

/// Don't parse any function bodies except those that are transparent.
class SkipNonTransparentFunctions : public DelayedParsingCallbacks {
  bool shouldDelayFunctionBodyParsing(Parser &TheParser,
                                      AbstractFunctionDecl *AFD,
                                      const DeclAttributes &Attrs,
                                      SourceRange BodyRange) override {
    return Attrs.isTransparent();
  }
};

} // unnamed namespace

Module *SourceLoader::loadModule(SourceLoc importLoc,
                             ArrayRef<std::pair<Identifier, SourceLoc>> path) {
  // FIXME: Swift submodules?
  if (path.size() > 1)
    return nullptr;

  auto moduleID = path[0];

  std::unique_ptr<llvm::MemoryBuffer> inputFile;
  if (llvm::error_code err = findModule(Ctx, moduleID.first.str(),
                                        moduleID.second, inputFile)) {
    if (err.value() != llvm::errc::no_such_file_or_directory) {
      Ctx.Diags.diagnose(moduleID.second, diag::sema_opening_import,
                         moduleID.first.str(), err.message());
    }

    return nullptr;
  }

  // Turn off debugging while parsing other modules.
  llvm::SaveAndRestore<bool> turnOffDebug(Ctx.LangOpts.DebugConstraintSolver,
                                          false);

  unsigned bufferID;
  if (auto BufID =
       Ctx.SourceMgr.getIDForBufferIdentifier(inputFile->getBufferIdentifier()))
    bufferID = BufID.getValue();
  else
    bufferID = Ctx.SourceMgr.addNewSourceBuffer(std::move(inputFile));

  auto *importMod = Module::create(moduleID.first, Ctx);
  Ctx.LoadedModules[moduleID.first.str()] = importMod;

  auto *importFile = new (Ctx) SourceFile(*importMod, SourceFileKind::Library,
                                          bufferID);
  importMod->addFile(*importFile);

  bool done;
  PersistentParserState persistentState;
  SkipNonTransparentFunctions delayCallbacks;
  parseIntoSourceFile(*importFile, bufferID, &done, nullptr, &persistentState,
                      SkipBodies ? &delayCallbacks : nullptr);
  assert(done && "Parser returned early?");
  (void)done;
  
  if (SkipBodies)
    performDelayedParsing(importMod, persistentState, nullptr);

  // FIXME: Support recursive definitions in immediate modes by making type
  // checking even lazier.
  if (SkipBodies)
    performNameBinding(*importFile);
  else
    performTypeChecking(*importFile, persistentState.getTopLevelContext());

  return importMod;
}

void SourceLoader::loadExtensions(NominalTypeDecl *nominal,
                                  unsigned previousGeneration) {
  // Type-checking the source automatically loads all extensions; there's
  // nothing to do here.
}
