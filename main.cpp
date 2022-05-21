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
#include <iomanip>
#include <iostream>
#include <llvm-11/llvm/ADT/IntrusiveRefCntPtr.h>
#include <llvm-11/llvm/ADT/Triple.h>
#include <llvm-11/llvm/Support/MemoryBuffer.h>
#include <llvm-11/llvm/Support/VirtualFileSystem.h>
#include <llvm-11/llvm/Support/raw_ostream.h>
#include <llvm-11/llvm/Target/TargetOptions.h>
#include <map>
#include <pthread.h>
#include <span>
#include <sstream>
#include <vector>

void annotate_location(std::map<unsigned, std::string> &annotations,
                       const clang::SourceManager &sm, clang::Token token,
                       const std::string &start_annotation,
                       const std::string &end_annotation) {

  unsigned begin_offset{0}, end_offset{0};
  begin_offset = sm.getFileOffset(token.getLocation());
  end_offset = begin_offset + token.getLength();
  assert(begin_offset != 0);
  assert(end_offset != 0);

  annotations[begin_offset] = "<" + start_annotation + ">";
  annotations[end_offset] = "</" + end_annotation + ">";

  // TODO: Determine whether I need to free up the clang variables here.
  return;
}

void replace_location(std::map<unsigned, std::string> &replacements,
                      const clang::SourceManager &sm, clang::Token token,
                      const std::string &replacement) {
  unsigned begin_offset{0};
  begin_offset = sm.getFileOffset(token.getLocation());
  assert(begin_offset != 0);
  replacements[begin_offset] = replacement;
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

clang::tooling::Range build_entire_range(const clang::FileID &fid,
                                         const clang::SourceManager &sm) {
  auto Start = sm.getLocForStartOfFile(fid);
  auto End = sm.getLocForEndOfFile(fid);

  return clang::tooling::Range(sm.getFileOffset(Start),
                               sm.getFileOffset(End) - sm.getFileOffset(Start));
}

std::string clang_format(std::unique_ptr<llvm::MemoryBuffer> Code,
                         const clang::FileID &fid,
                         llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fs,
                         const clang::FileManager &fm,
                         clang::SourceManager &sm) {

  llvm::Expected<clang::format::FormatStyle> FormatStyle =
      clang::format::getStyle("LLVM", "unformatted.cpp", "LLVM",
                              Code->getBuffer(), nullptr);
  assert(!!FormatStyle);

  auto entire_range = build_entire_range(fid, sm);
  std::vector<clang::tooling::Range> Ranges{entire_range};
  unsigned CursorPosition{0};

  clang::tooling::Replacements cumulative_replacements =
      clang::format::sortIncludes(*FormatStyle, Code->getBuffer(), Ranges,
                                  "unformatted.cpp", &CursorPosition);

  auto ChangedCode = clang::tooling::applyAllReplacements(
      Code->getBuffer(), cumulative_replacements);
  assert(!!ChangedCode);

  Ranges = clang::tooling::calculateRangesAfterReplacements(
      cumulative_replacements, Ranges);
  clang::format::FormattingAttemptStatus Status;
  clang::tooling::Replacements format_replacements = clang::format::reformat(
      *FormatStyle, *ChangedCode, Ranges, "unformatted.cpp", &Status);
  cumulative_replacements = cumulative_replacements.merge(format_replacements);

  clang::Rewriter rewriter(sm, clang::LangOptions());
  clang::tooling::applyAllReplacements(cumulative_replacements, rewriter);

  std::string rewritten_string{};
  llvm::raw_string_ostream rewritten_string_stream{rewritten_string};
  rewriter.getEditBuffer(fid).write(rewritten_string_stream);

  return rewritten_string;
}

void initialize_compilerinstance(
    clang::CompilerInstance &ci,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fs) {
  auto DiagOpts = new clang::DiagnosticOptions();
  clang::TextDiagnosticPrinter *tpd =
      new clang::TextDiagnosticPrinter(llvm::errs(), DiagOpts, false);

  std::shared_ptr<clang::TargetOptions> target_options =
      std::make_shared<clang::TargetOptions>();
  target_options->Triple = llvm::sys::getDefaultTargetTriple();
  ci.createDiagnostics(tpd);
  auto ti =
      clang::TargetInfo::CreateTargetInfo(ci.getDiagnostics(), target_options);
  ci.setTarget(ti);
  ci.createFileManager(fs);
  ci.createSourceManager(ci.getFileManager());
  ci.createPreprocessor(clang::TU_Complete);
}

/*
 * This function will calculate the number of digits in
 * _num_.
 */
unsigned int calculate_padding(unsigned int num) {
  unsigned int digits{0};
  for (; num != 0; num /= 10, digits++);
  return digits;
}

unsigned int count_lines(const std::unique_ptr<llvm::MemoryBuffer> &Code) {
  unsigned int lines{0};
  for (unsigned offset = 0; offset < Code->getBufferSize(); offset++) {
    if (Code->getBuffer()[offset] == '\n') {
      lines++;
    }
  }
  return lines;
}

void print_annotated_file(
    const std::unique_ptr<llvm::MemoryBuffer> &FormattedCode,
    std::map<unsigned, std::string> annotations,
    std::map<unsigned, std::string> replacements) {

  unsigned int line_number{1};
  unsigned int line_number_width{calculate_padding(count_lines(FormattedCode))};
  bool saw_newline{true};
  for (unsigned offset = 0; offset < FormattedCode->getBufferSize(); offset++) {

    if (saw_newline) {
      std::cout << std::setw(line_number_width) << std::right << line_number << std::setw(-1) << std::left << " ";
      line_number++;
      saw_newline = false;
    }

    if (annotations.contains(offset)) {
      std::cout << annotations[offset];
    }
    if (replacements.contains(offset)) {
      std::cout << replacements[offset];
    } else {
      std::cout << FormattedCode->getBuffer()[offset];
    }

    if (FormattedCode->getBuffer()[offset] == '\n') {
      saw_newline = true;
    }

  }
  std::cout << "\n";
}

enum class Annotation {
  Literal,
  Keyword,
};

std::map<Annotation, std::pair<std::string, std::string>> styles{
    {Annotation::Literal, {"font color=red", "font"}},
    {Annotation::Keyword, {"font color=green", "font"}},
};

std::map<clang::tok::TokenKind, std::string> replacement_values{
    {clang::tok::greater, "&gt;"},
    {clang::tok::less, "&lt;"},
};

int main(int argc, char **argv) {

  if (argc < 2) {
    return 1;
  }

  llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> inmemory_fs(
      new llvm::vfs::InMemoryFileSystem);
  clang::CompilerInstance ci;

  initialize_compilerinstance(ci, inmemory_fs);

  // First, let's do our clang formatting!
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> CodeOrError =
      llvm::MemoryBuffer::getFileAsStream(argv[1]);
  assert(!CodeOrError.getError());
  std::unique_ptr<llvm::MemoryBuffer> UnformattedCode =
      std::move(CodeOrError.get());
  clang::FileID unformatted_fid = create_in_memory_file(
      "unformatted.cpp", UnformattedCode, ci.getSourceManager(),
      ci.getFileManager(), inmemory_fs);

  auto formatted_code_string =
      clang_format(std::move(UnformattedCode), unformatted_fid, inmemory_fs,
                   ci.getFileManager(), ci.getSourceManager());

  std::cout << formatted_code_string << "\n";

  // Now, let's take that formatted code and treat it is an in-memory file.
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> FormattedCodeOrError =
      llvm::MemoryBuffer::getMemBufferCopy(formatted_code_string);
  assert(!FormattedCodeOrError.getError());
  std::unique_ptr<llvm::MemoryBuffer> FormattedCode =
      std::move(FormattedCodeOrError.get());
  clang::FileID formatted_fid = create_in_memory_file(
      "formatted.cpp", FormattedCode, ci.getSourceManager(),
      ci.getFileManager(), inmemory_fs);

  clang::Lexer lexer{formatted_fid, FormattedCode.get(), ci.getPreprocessor()};

  std::map<unsigned, std::string> annotations{};
  std::map<unsigned, std::string> replacements{};

  clang::SourceLocation location{
      ci.getSourceManager().getLocForStartOfFile(formatted_fid)};

  while (true) {
    llvm::Optional<clang::Token> token =
        lexer.findNextToken(location, ci.getSourceManager(), ci.getLangOpts());

    if (!!token && token->is(clang::tok::eof)) {
      break;
    }

    if (replacement_values.contains(token->getKind())) {
      replace_location(replacements, ci.getSourceManager(), *token,
                       replacement_values[token->getKind()]);
    } else if (token->isLiteral()) {
      annotate_location(annotations, ci.getSourceManager(), *token,
                        std::get<0>(styles[Annotation::Literal]),
                        std::get<1>(styles[Annotation::Literal]));
    } else if (token->is(clang::tok::raw_identifier)) {
      auto idinfo =
          ci.getPreprocessor().getIdentifierInfo(token->getRawIdentifier());
      if (idinfo) {
        assert(idinfo);
        if (idinfo->isKeyword(ci.getLangOpts())) {
          annotate_location(annotations, ci.getSourceManager(), *token,
                            std::get<0>(styles[Annotation::Keyword]),
                            std::get<1>(styles[Annotation::Keyword]));
        }
      }
    }
    location = token->getEndLoc();
  }

  std::cout << "<html><head></head><body><pre>\n";
  print_annotated_file(FormattedCode, annotations, replacements);
  std::cout << "</pre></body></html>\n";

  return 0;
}