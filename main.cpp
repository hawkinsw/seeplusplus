#include "clang-c/CXString.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/FileSystemOptions.h"
#include "clang/Basic/IdentifierTable.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/LangStandard.h"
#include "clang/Basic/Module.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/TargetOptions.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Basic/Version.h"
#include "clang/Format/Format.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendOptions.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/ModuleLoader.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/Token.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/Core/Replacement.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/Process.h"
#include <cassert>
#include <clang-c/Index.h>
#include <clang/Basic/DiagnosticOptions.h>
#include <clang/Frontend/TextDiagnosticPrinter.h>
#include <fstream>
#include <iostream>
#include <llvm-11/llvm/ADT/IntrusiveRefCntPtr.h>
#include <llvm-11/llvm/ADT/Triple.h>
#include <llvm-11/llvm/Support/MemoryBuffer.h>
#include <llvm-11/llvm/Support/raw_ostream.h>
#include <llvm-11/llvm/Target/TargetOptions.h>
#include <map>
#include <pthread.h>
#include <span>
#include <sstream>
#include <vector>

class ClangFormatDiagConsumer : public clang::DiagnosticConsumer {
  virtual void anchor() {}

  void HandleDiagnostic(clang::DiagnosticsEngine::Level DiagLevel,
                        const clang::Diagnostic &Info) override {

    llvm::SmallVector<char, 16> vec;
    Info.FormatDiagnostic(vec);
    llvm::errs() << "clang-format error:" << vec << "\n";
  }
};

void annotate_location(std::map<unsigned, std::string> &annotations,
                       const clang::SourceManager &sm, clang::Token token,
                       const std::string &annotation) {

  unsigned begin_offset{0}, end_offset{0};
  begin_offset = sm.getFileOffset(token.getLocation());
  end_offset = begin_offset + token.getLength();
  assert(begin_offset != 0);
  assert(end_offset != 0);

  annotations[begin_offset] = "<" + annotation + ">";
  annotations[end_offset] = "</" + annotation + ">";

  // TODO: Determine whether I need to free up the clang variables here.
  return;
}

static clang::FileID create_in_memory_file(
    llvm::StringRef FileName, const std::unique_ptr<llvm::MemoryBuffer> &Source,
    clang::SourceManager &Sources, clang::FileManager &Files,
    llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> MemFS) {
  MemFS->addFileNoOwn(FileName, 0, Source.get());
  auto File = Files.getOptionalFileRef(FileName);
  assert(File && "File not added to MemFS?");
  return Sources.createFileID(*File, clang::SourceLocation(),
                              clang::SrcMgr::C_User);
}

clang::tooling::Range
build_entire_range(const std::unique_ptr<llvm::MemoryBuffer> &Code) {
  llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> InMemoryFileSystem(
      new llvm::vfs::InMemoryFileSystem);
  clang::FileManager Files(clang::FileSystemOptions(), InMemoryFileSystem);
  clang::DiagnosticsEngine Diagnostics(
      llvm::IntrusiveRefCntPtr<clang::DiagnosticIDs>(new clang::DiagnosticIDs),
      new clang::DiagnosticOptions);
  clang::SourceManager Sources(Diagnostics, Files);
  clang::FileID ID = create_in_memory_file("<irrelevant>", Code, Sources, Files,
                                           InMemoryFileSystem);

  auto Start = Sources.getLocForStartOfFile(ID);
  auto End = Sources.getLocForEndOfFile(ID);

  return clang::tooling::Range(Sources.getFileOffset(Start),
                               Sources.getFileOffset(End) -
                                   Sources.getFileOffset(Start));
}

std::string clang_format(std::unique_ptr<llvm::MemoryBuffer> Code) {
  llvm::Expected<clang::format::FormatStyle> FormatStyle =
      clang::format::getStyle("LLVM", "input.cpp",
                              clang::format::DefaultFallbackStyle,
                              Code->getBuffer(), nullptr);
  assert(!!FormatStyle);

  auto entire_range = build_entire_range(Code);
  std::vector<clang::tooling::Range> Ranges{entire_range};
  unsigned CursorPosition{0};

  clang::tooling::Replacements cumulative_replacements =
      clang::format::sortIncludes(*FormatStyle, Code->getBuffer(), Ranges,
                                  "input.cpp", &CursorPosition);

  auto ChangedCode = clang::tooling::applyAllReplacements(
      Code->getBuffer(), cumulative_replacements);
  assert(!!ChangedCode);

  Ranges = clang::tooling::calculateRangesAfterReplacements(
      cumulative_replacements, Ranges);
  clang::format::FormattingAttemptStatus Status;
  clang::tooling::Replacements format_replacements = clang::format::reformat(
      *FormatStyle, *ChangedCode, Ranges, "input.cpp", &Status);
  cumulative_replacements = cumulative_replacements.merge(format_replacements);

  llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> InMemoryFileSystem(
      new llvm::vfs::InMemoryFileSystem);
  clang::FileManager Files(clang::FileSystemOptions(), InMemoryFileSystem);

  llvm::IntrusiveRefCntPtr<clang::DiagnosticOptions> DiagOpts(
      new clang::DiagnosticOptions());
  ClangFormatDiagConsumer IgnoreDiagnostics;
  clang::DiagnosticsEngine Diagnostics(
      llvm::IntrusiveRefCntPtr<clang::DiagnosticIDs>(new clang::DiagnosticIDs),
      &*DiagOpts, &IgnoreDiagnostics, false);
  clang::SourceManager Sources(Diagnostics, Files);
  clang::FileID ID = create_in_memory_file("input.cpp", Code, Sources, Files,
                                           InMemoryFileSystem.get());
  clang::Rewriter rewritere(Sources, clang::LangOptions());
  clang::tooling::applyAllReplacements(cumulative_replacements, rewritere);

  std::string rewritten_string{};
  llvm::raw_string_ostream rewritten_string_stream{rewritten_string};
  rewritere.getEditBuffer(ID).write(rewritten_string_stream);

  return rewritten_string;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    return 1;
  }

  // First, let's do our clang formatting!
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> CodeOrError =
      llvm::MemoryBuffer::getFileAsStream(argv[1]);
  assert(!CodeOrError.getError());
  std::unique_ptr<llvm::MemoryBuffer> Code = std::move(CodeOrError.get());
  llvm::StringRef CodeStringRef = Code->getBuffer();
  auto formatted_code_string = clang_format(std::move(Code));

  // Now, let's take that formatted code and treat it is an in-memory file.
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> FormattedCodeOrError =
      llvm::MemoryBuffer::getMemBufferCopy(formatted_code_string);
  assert(!FormattedCodeOrError.getError());
  std::unique_ptr<llvm::MemoryBuffer> FormattedCode =
      std::move(FormattedCodeOrError.get());

  llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> inmemory_fs(
      new llvm::vfs::InMemoryFileSystem);

  auto DiagOpts = new clang::DiagnosticOptions();
  clang::TextDiagnosticPrinter *tpd =
      new clang::TextDiagnosticPrinter(llvm::errs(), DiagOpts, false);
  ClangFormatDiagConsumer ignore_diagnostics;
  clang::CompilerInstance ci{};

  std::shared_ptr<clang::TargetOptions> target_options =
      std::make_shared<clang::TargetOptions>();
  target_options->Triple = llvm::sys::getDefaultTargetTriple();
  ci.createDiagnostics(tpd);
  auto ti =
      clang::TargetInfo::CreateTargetInfo(ci.getDiagnostics(), target_options);
  ci.setTarget(ti);
  ci.createFileManager(inmemory_fs);
  ci.createSourceManager(ci.getFileManager());
  ci.createPreprocessor(clang::TU_Complete);
  clang::FileID ID =
      create_in_memory_file("input.cpp", FormattedCode, ci.getSourceManager(),
                            ci.getFileManager(), inmemory_fs);

  clang::Lexer lexer{ID, FormattedCode.get(), ci.getPreprocessor()};
  std::map<unsigned, std::string> annotations{};

  clang::SourceLocation location{
      ci.getSourceManager().getLocForStartOfFile(ID)};
  while (true) {
    llvm::Optional<clang::Token> token =
        lexer.findNextToken(location, ci.getSourceManager(), ci.getLangOpts());

    if (token->is(clang::tok::eof)) {
      break;
    }

    std::cout << token->getName() << "\n";

    if (token->isLiteral()) {
      annotate_location(annotations, ci.getSourceManager(), *token, "literal");
    } else if (token->is(clang::tok::raw_identifier)) {
      auto idinfo =
          ci.getPreprocessor().getIdentifierInfo(token->getRawIdentifier());
      if (idinfo) {
        assert(idinfo);
        if (idinfo->isKeyword(ci.getLangOpts())) {
          annotate_location(annotations, ci.getSourceManager(), *token,
                            "keyword");
        }
      }
    }
    location = token->getEndLoc();
  }

  for (unsigned offset = 0; offset < FormattedCode->getBufferSize(); offset++) {
    if (annotations.contains(offset)) {
      std::cout << annotations[offset];
    }
    std::cout << FormattedCode->getBuffer()[offset];
  }
  std::cout << "\n";

  return 0;
}