//===--- ParseAST.cpp - Provide the clang::ParseAST method ----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the clang::ParseAST method.
//
//===----------------------------------------------------------------------===//

#include "clang/Parse/ParseAST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ExternalASTSource.h"
#include "clang/AST/Stmt.h"
#include "clang/Parse/ParseDiagnostic.h"
#include "clang/Parse/Parser.h"
#include "clang/Sema/CodeCompleteConsumer.h"
#include "clang/Sema/ExternalSemaSource.h"
#include "clang/Sema/Sema.h"
#include "clang/Sema/SemaConsumer.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include <cstdio>
#include <memory>

using namespace clang;

namespace {

/// If a crash happens while the parser is active, an entry is printed for it.
class PrettyStackTraceParserEntry : public llvm::PrettyStackTraceEntry {
  const Parser &P;
public:
  PrettyStackTraceParserEntry(const Parser &p) : P(p) {}
  void print(raw_ostream &OS) const override;
};

/// If a crash happens while the parser is active, print out a line indicating
/// what the current token is.
void PrettyStackTraceParserEntry::print(raw_ostream &OS) const {
  const Token &Tok = P.getCurToken();
  if (Tok.is(tok::eof)) {
    OS << "<eof> parser at end of file\n";
    return;
  }

  if (Tok.getLocation().isInvalid()) {
    OS << "<unknown> parser at unknown location\n";
    return;
  }

  const Preprocessor &PP = P.getPreprocessor();
  Tok.getLocation().print(OS, PP.getSourceManager());
  if (Tok.isAnnotation()) {
    OS << ": at annotation token\n";
  } else {
    // Do the equivalent of PP.getSpelling(Tok) except for the parts that would
    // allocate memory.
    bool Invalid = false;
    const SourceManager &SM = P.getPreprocessor().getSourceManager();
    unsigned Length = Tok.getLength();
    const char *Spelling = SM.getCharacterData(Tok.getLocation(), &Invalid);
    if (Invalid) {
      OS << ": unknown current parser token\n";
      return;
    }
    OS << ": current parser token '" << StringRef(Spelling, Length) << "'\n";
  }
}

}  // namespace

//===----------------------------------------------------------------------===//
// TASKIFY Actions
//===----------------------------------------------------------------------===//

class TaskifyPPCallbacks : public clang::PPCallbacks
{
  clang::SourceManager &_sm;
  ASTContext &_context;
  
public:
  TaskifyPPCallbacks (clang::SourceManager &sm, ASTContext &Context) : _sm (sm), _context (Context) { }
  
  // InclusionDirective
  virtual void InclusionDirective (clang::SourceLocation HashLoc,
                                   const clang::Token &,
                                   clang::StringRef,
                                   bool,
                                   clang::CharSourceRange,
                                   const clang::FileEntry *File,
                                   clang::StringRef,
                                   clang::StringRef,
                                   const clang::Module *) {
    // Convert to relative path
    const char *fileWhereIncludeIs = _sm.getBufferName(HashLoc);
    //const char *path = realpath (_sm.getBufferName (HashLoc), NULL);
    if (fileWhereIncludeIs == NULL) {
      llvm::errs() << "null path " << _sm.getBufferName (HashLoc)
      << ": " << strerror (errno)
      << "\n";
    }
    if (File != NULL) {
      const char *include = File->getName();
      //const char *fpath = realpath (File->getName (), NULL);
      if (include == NULL) {
        llvm::errs() << "null fpath " << File->getName ()
        << ": " << strerror (errno)
        << "\n";
      }
      
      if ((include != NULL) && (fileWhereIncludeIs != NULL)) {
        // Store the relationship
		  _context.getIncludedFiles()[fileWhereIncludeIs].push_back(include);
      }
    }
  }
};

//===----------------------------------------------------------------------===//
// Public interface to the file
//===----------------------------------------------------------------------===//

/// ParseAST - Parse the entire file specified, notifying the ASTConsumer as
/// the file is parsed.  This inserts the parsed decls into the translation unit
/// held by Ctx.
///
void clang::ParseAST(Preprocessor &PP, ASTConsumer *Consumer,
                     ASTContext &Ctx, bool PrintStats,
                     TranslationUnitKind TUKind,
                     CodeCompleteConsumer *CompletionConsumer,
                     bool SkipFunctionBodies) {

  std::unique_ptr<Sema> S(
      new Sema(PP, Ctx, *Consumer, TUKind, CompletionConsumer));

  // Recover resources if we crash before exiting this method.
  llvm::CrashRecoveryContextCleanupRegistrar<Sema> CleanupSema(S.get());
  
  //TASKIFY CHANGE
  //ParseAST(*S.get(), PrintStats, SkipFunctionBodies);
  ParseAST(Ctx, *S.get(), PrintStats, SkipFunctionBodies);
}

//TASKIFY CHANGE
//void clang::ParseAST(Sema &S, bool PrintStats, bool SkipFunctionBodies) {
void clang::ParseAST(ASTContext &Ctx, Sema &S, bool PrintStats, bool SkipFunctionBodies) {
  // Collect global stats on Decls/Stmts (until we have a module streamer).
  if (PrintStats) {
    Decl::EnableStatistics();
    Stmt::EnableStatistics();
  }

  // Also turn on collection of stats inside of the Sema object.
  bool OldCollectStats = PrintStats;
  std::swap(OldCollectStats, S.CollectStats);

  ASTConsumer *Consumer = &S.getASTConsumer();

  std::unique_ptr<Parser> ParseOP(
      new Parser(S.getPreprocessor(), S, SkipFunctionBodies));
  Parser &P = *ParseOP.get();

  PrettyStackTraceParserEntry CrashInfo(P);

  // Recover resources if we crash before exiting this method.
  llvm::CrashRecoveryContextCleanupRegistrar<Parser>
    CleanupParser(ParseOP.get());

  // TASKIFY
  TaskifyPPCallbacks *ppc = new TaskifyPPCallbacks (S.getSourceManager(), Ctx);   // add callbacks when we encounter an #include
  S.getPreprocessor().addPPCallbacks (std::unique_ptr<PPCallbacks>(ppc));
  clang::Token token;
  
  // from Clang
  S.getPreprocessor().EnterMainSourceFile();
  P.Initialize();
  
  // save position of the current lex position
  S.getPreprocessor().EnableBacktrackAtThisPos();
  
  // read all tokes for finding #include
  do {
    S.getPreprocessor().Lex(token);
  }
  while (token.isNot (clang::tok::eof));
  
  // restore the lex position and continue
  S.getPreprocessor().Backtrack();
  
  // retrieve all the includes
  std::map<std::string, std::vector<std::string>> includesPerFile = Ctx.getIncludedFiles();

  // C11 6.9p1 says translation units must have at least one top-level
  // declaration. C++ doesn't have this restriction. We also don't want to
  // complain if we have a precompiled header, although technically if the PCH
  // is empty we should still emit the (pedantic) diagnostic.
  Parser::DeclGroupPtrTy ADecl;
  ExternalASTSource *External = S.getASTContext().getExternalSource();
  if (External)
    External->StartTranslationUnit(Consumer);

  if (P.ParseTopLevelDecl(ADecl)) {
    if (!External && !S.getLangOpts().CPlusPlus)
      P.Diag(diag::ext_empty_translation_unit);
  } else {
    do {
      // If we got a null return and something *was* parsed, ignore it.  This
      // is due to a top-level semicolon, an action override, or a parse error
      // skipping something.
      if (ADecl && !Consumer->HandleTopLevelDecl(ADecl.get()))
        return;
    } while (!P.ParseTopLevelDecl(ADecl));
  }

  // Process any TopLevelDecls generated by #pragma weak.
  for (Decl *D : S.WeakTopLevelDecls())
    Consumer->HandleTopLevelDecl(DeclGroupRef(D));
  
  Consumer->HandleTranslationUnit(S.getASTContext());

  std::swap(OldCollectStats, S.CollectStats);
  if (PrintStats) {
    llvm::errs() << "\nSTATISTICS:\n";
    P.getActions().PrintStats();
    S.getASTContext().PrintStats();
    Decl::PrintStats();
    Stmt::PrintStats();
    Consumer->PrintStats();
  }
}
