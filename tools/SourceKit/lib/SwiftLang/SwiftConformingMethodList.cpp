//===--- SwiftConformingMethodList.cpp ------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "SwiftASTManager.h"
#include "SwiftEditorDiagConsumer.h"
#include "SwiftLangSupport.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/IDE/ConformingMethodList.h"
#include "swift/IDE/CompletionInstance.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Comment.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Type.h"

using namespace SourceKit;
using namespace swift;
using namespace ide;

static bool swiftConformingMethodListImpl(
    SwiftLangSupport &Lang, llvm::MemoryBuffer *UnresolvedInputFile,
    unsigned Offset, ArrayRef<const char *> Args,
    ArrayRef<const char *> ExpectedTypeNames,
    ide::ConformingMethodListConsumer &Consumer,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FileSystem,
    std::string &Error) {

  // Resolve symlinks for the input file.
  llvm::SmallString<128> bufferIdentifier;
  if (auto err = FileSystem->getRealPath(
          UnresolvedInputFile->getBufferIdentifier(), bufferIdentifier))
    bufferIdentifier = UnresolvedInputFile->getBufferIdentifier();

  auto origOffset = Offset;
  auto newBuffer = ide::makeCodeCompletionMemoryBuffer(
      UnresolvedInputFile, Offset, bufferIdentifier);

  SourceManager SM;
  DiagnosticEngine Diags(SM);
  PrintingDiagnosticConsumer PrintDiags;
  EditorDiagConsumer TraceDiags;
  trace::TracedOperation TracedOp{trace::OperationKind::CodeCompletion};

  Diags.addConsumer(PrintDiags);
  if (TracedOp.enabled()) {
    Diags.addConsumer(TraceDiags);
    trace::SwiftInvocation SwiftArgs;
    trace::initTraceInfo(SwiftArgs, bufferIdentifier, Args);
    TracedOp.setDiagnosticProvider(
        [&](SmallVectorImpl<DiagnosticEntryInfo> &diags) {
          TraceDiags.getAllDiagnostics(diags);
        });
    TracedOp.start(
        SwiftArgs,
        {std::make_pair("OriginalOffset", std::to_string(origOffset)),
         std::make_pair("Offset", std::to_string(Offset))});
  }
  ForwardingDiagnosticConsumer CIDiags(Diags);

  CompilerInvocation Invocation;
  bool Failed = Lang.getASTManager()->initCompilerInvocation(
      Invocation, Args, Diags, newBuffer->getBufferIdentifier(), FileSystem,
      Error);
  if (Failed)
    return false;
  if (!Invocation.getFrontendOptions().InputsAndOutputs.hasInputs()) {
    Error = "no input filenames specified";
    return false;
  }

  // Pin completion instance.
  auto CompletionInst =  Lang.getCompletionInstance();
  CompilerInstance *CI = CompletionInst->getCompilerInstance(
      Invocation, FileSystem, newBuffer.get(), Offset, Error, &CIDiags);
  if (!CI)
    return false;
  SWIFT_DEFER { CI->removeDiagnosticConsumer(&CIDiags); };

  // Perform the parsing and completion.
  if (!CI->hasPersistentParserState())
    CI->performParseAndResolveImportsOnly();

  // Create a factory for code completion callbacks that will feed the
  // Consumer.
  std::unique_ptr<CodeCompletionCallbacksFactory> callbacksFactory(
      ide::makeConformingMethodListCallbacksFactory(ExpectedTypeNames,
                                                    Consumer));

  performCodeCompletionSecondPass(CI->getPersistentParserState(),
                                  *callbacksFactory);

  return true;
}

void SwiftLangSupport::getConformingMethodList(
    llvm::MemoryBuffer *UnresolvedInputFile, unsigned Offset,
    ArrayRef<const char *> Args, ArrayRef<const char *> ExpectedTypeNames,
    SourceKit::ConformingMethodListConsumer &SKConsumer,
    Optional<VFSOptions> vfsOptions) {
  std::string error;

  // FIXME: the use of None as primary file is to match the fact we do not read
  // the document contents using the editor documents infrastructure.
  auto fileSystem = getFileSystem(vfsOptions, /*primaryFile=*/None, error);
  if (!fileSystem)
    return SKConsumer.failed(error);

  class Consumer : public ide::ConformingMethodListConsumer {
    SourceKit::ConformingMethodListConsumer &SKConsumer;

  public:
    Consumer(SourceKit::ConformingMethodListConsumer &SKConsumer)
        : SKConsumer(SKConsumer) {}

    /// Convert an IDE result to a SK result and send it to \c SKConsumer .
    void handleResult(const ide::ConformingMethodListResult &Result) {
      SmallString<512> SS;
      llvm::raw_svector_ostream OS(SS);

      unsigned TypeNameBegin = SS.size();
      Result.ExprType.print(OS);
      unsigned TypeNameLength = SS.size() - TypeNameBegin;

      unsigned TypeUSRBegin = SS.size();
      SwiftLangSupport::printTypeUSR(Result.ExprType, OS);
      unsigned TypeUSRLength = SS.size() - TypeUSRBegin;

      struct MemberInfo {
        size_t DeclNameBegin = 0;
        size_t DeclNameLength = 0;
        size_t TypeNameBegin = 0;
        size_t TypeNameLength = 0;
        size_t TypeUSRBegin = 0;
        size_t TypeUSRLength = 0;
        size_t DescriptionBegin = 0;
        size_t DescriptionLength = 0;
        size_t SourceTextBegin = 0;
        size_t SourceTextLength = 0;
        StringRef BriefComment;

        MemberInfo() {}
      };
      SmallVector<MemberInfo, 8> Members;

      for (auto member : Result.Members) {
        Members.emplace_back();
        auto &memberElem = Members.back();

        auto funcTy = cast<FuncDecl>(member)->getMethodInterfaceType();
        funcTy = Result.ExprType->getTypeOfMember(Result.DC->getParentModule(),
                                                  member, funcTy);
        auto resultTy = funcTy->castTo<FunctionType>()->getResult();

        // Name.
        memberElem.DeclNameBegin = SS.size();
        member->getFullName().print(OS);
        memberElem.DeclNameLength = SS.size() - memberElem.DeclNameBegin;

        // Type name.
        memberElem.TypeNameBegin = SS.size();
        resultTy.print(OS);
        memberElem.TypeNameLength = SS.size() - memberElem.TypeNameBegin;

        // Type USR.
        memberElem.TypeUSRBegin = SS.size();
        SwiftLangSupport::printTypeUSR(resultTy, OS);
        memberElem.TypeUSRLength = SS.size() - memberElem.TypeUSRBegin;

        // Description.
        memberElem.DescriptionBegin = SS.size();
        SwiftLangSupport::printMemberDeclDescription(
            member, Result.ExprType, /*usePlaceholder=*/false, OS);
        memberElem.DescriptionLength =
            SS.size() - memberElem.DescriptionBegin;

        // Sourcetext.
        memberElem.SourceTextBegin = SS.size();
        SwiftLangSupport::printMemberDeclDescription(
            member, Result.ExprType, /*usePlaceholder=*/true, OS);
        memberElem.SourceTextLength =
            SS.size() - memberElem.SourceTextBegin;

        // DocBrief.
        auto MaybeClangNode = member->getClangNode();
        if (MaybeClangNode) {
          if (auto *D = MaybeClangNode.getAsDecl()) {
            const auto &ClangContext = D->getASTContext();
            if (const clang::RawComment *RC =
                    ClangContext.getRawCommentForAnyRedecl(D))
              memberElem.BriefComment = RC->getBriefText(ClangContext);
          }
        } else {
          memberElem.BriefComment = member->getBriefComment();
        }
      }

      SourceKit::ConformingMethodListResult SKResult;
      SmallVector<SourceKit::ConformingMethodListResult::Member, 8>
          SKMembers;

      for (auto info : Members) {
        StringRef Name(SS.begin() + info.DeclNameBegin, info.DeclNameLength);
        StringRef TypeName(SS.begin() + info.TypeNameBegin,
                           info.TypeNameLength);
        StringRef TypeUSR(SS.begin() + info.TypeUSRBegin, info.TypeUSRLength);
        StringRef Description(SS.begin() + info.DescriptionBegin,
                              info.DescriptionLength);
        StringRef SourceText(SS.begin() + info.SourceTextBegin,
                             info.SourceTextLength);
        SKMembers.push_back({Name, TypeName, TypeUSR, Description, SourceText,
                               info.BriefComment});
      }

      SKResult.TypeName = StringRef(SS.begin() + TypeNameBegin, TypeNameLength);
      SKResult.TypeUSR = StringRef(SS.begin() + TypeUSRBegin, TypeUSRLength);
      SKResult.Members = SKMembers;

      SKConsumer.handleResult(SKResult);
    }
  } Consumer(SKConsumer);

  if (!swiftConformingMethodListImpl(*this, UnresolvedInputFile, Offset, Args,
                                     ExpectedTypeNames, Consumer, fileSystem,
                                     error)) {
    SKConsumer.failed(error);
  }
}
