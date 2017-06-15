//===--- ExtractRepeatedExpressionIntoVariable.cpp -  ---------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Implements the "Extract repeated expression into variable" refactoring
// operation.
//
//===----------------------------------------------------------------------===//

#include "RefactoringOperations.h"
#include "SourceLocationUtilities.h"
#include "clang/AST/AST.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "llvm/Support/SaveAndRestore.h"

using namespace clang;
using namespace clang::tooling;

namespace {

class ExtractRepeatedExpressionIntoVariableOperation
    : public RefactoringOperation {
public:
  ExtractRepeatedExpressionIntoVariableOperation(
      const Expr *E, ArrayRef<const Expr *> Duplicates, const Decl *ParentDecl)
      : E(E), DuplicateExpressions(Duplicates.begin(), Duplicates.end()),
        ParentDecl(ParentDecl) {}

  const Stmt *getTransformedStmt() const override { return E; }

  llvm::Expected<RefactoringResult> perform(ASTContext &Context, const Preprocessor &ThePreprocessor,
          const RefactoringOptionSet &Options,
          unsigned SelectedCandidateIndex) override;

  const Expr *E;
  SmallVector<const Expr *, 4> DuplicateExpressions;
  const Decl *ParentDecl;
};

using UseOfDeclaration = std::pair<const Decl *, unsigned>;

bool shouldIgnoreParens(const ParenExpr *E) {
  if (!E)
    return false;
  const Expr *Child = E->getSubExpr();
  // Ignore the parens unless they are around an expression that
  // really needs them.
  if (isa<UnaryOperator>(Child) || isa<BinaryOperator>(Child) ||
      isa<AbstractConditionalOperator>(Child) ||
      isa<CXXOperatorCallExpr>(Child))
    return false;
  return true;
}

/// Builds up a list of declarations that are used in an expression.
class DuplicateExprSemanticProfiler
    : public RecursiveASTVisitor<DuplicateExprSemanticProfiler> {
  unsigned Index = 0;
  llvm::SmallVectorImpl<UseOfDeclaration> &DeclRefs;

public:
  DuplicateExprSemanticProfiler(
      llvm::SmallVectorImpl<UseOfDeclaration> &DeclRefs)
      : DeclRefs(DeclRefs) {
    DeclRefs.clear();
  }

  bool VisitStmt(const Stmt *S) {
    if (!shouldIgnoreParens(dyn_cast<ParenExpr>(S)))
      ++Index;
    return true;
  }

  bool VisitDeclRefExpr(const DeclRefExpr *E) {
    if (E->getDecl())
      DeclRefs.emplace_back(E->getDecl(), Index);
    return true;
  }
};

class DuplicateExprFinder : public RecursiveASTVisitor<DuplicateExprFinder>,
                            PrinterHelper {
  const Expr *Target;
  const PrintingPolicy &PP;
  Stmt::StmtClass ExprKind;
  QualType T;
  std::string ExprString, OSString;
  llvm::SmallVector<UseOfDeclaration, 8> ExprDecls, DeclUses;

  void printExpr(std::string &Str, const Expr *E) {
    llvm::raw_string_ostream OS(Str);
    E->printPretty(OS, /*Helper=*/this, PP);
  }

public:
  SmallVector<const Expr *, 4> DuplicateExpressions;

  DuplicateExprFinder(const Expr *E, const PrintingPolicy &PP)
      : Target(E), PP(PP), ExprKind(E->getStmtClass()), T(E->getType()) {
    printExpr(ExprString, E);
    DuplicateExprSemanticProfiler(ExprDecls).TraverseStmt(
        const_cast<Expr *>(E));
  }

  bool handledStmt(Stmt *E, raw_ostream &OS) final override {
    if (const auto *Paren = dyn_cast<ParenExpr>(E)) {
      if (!shouldIgnoreParens(Paren))
        return false;
      Paren->getSubExpr()->printPretty(OS, /*Helper=*/this, PP);
      return true;
    }
    return false;
  }

  bool VisitStmt(const Stmt *S) {
    if (S->getStmtClass() != ExprKind)
      return true;
    const auto *E = cast<Expr>(S);
    if (E == Target) {
      DuplicateExpressions.push_back(E);
      return true;
    }
    // The expression types should match.
    if (E->getType() != T)
      return true;
    // Check if the expression is a duplicate by comparing their lexical
    // representations.
    OSString.clear();
    printExpr(OSString, E);
    if (OSString == ExprString) {
      DuplicateExprSemanticProfiler(DeclUses).TraverseStmt(
          const_cast<Expr *>(E));
      // Check if they're semantically equivalent.
      if (ExprDecls.size() == DeclUses.size() &&
          std::equal(ExprDecls.begin(), ExprDecls.end(), DeclUses.begin()))
        DuplicateExpressions.push_back(E);
    }
    return true;
  }
};

} // end anonymous namespace

static QualType returnTypeOfCall(const Expr *E) {
  if (const auto *Call = dyn_cast<CallExpr>(E)) {
    if (const auto *Fn = Call->getDirectCallee())
      return Fn->getReturnType();
  } else if (const auto *Msg = dyn_cast<ObjCMessageExpr>(E)) {
    if (const auto *M = Msg->getMethodDecl())
      return M->getReturnType();
  } else if (const auto *PRE = dyn_cast<ObjCPropertyRefExpr>(E)) {
    if (PRE->isImplicitProperty()) {
      if (const auto *M = PRE->getImplicitPropertyGetter())
        return M->getReturnType();
    } else if (const auto *Prop = PRE->getExplicitProperty())
      return Prop->getType();
  }
  return QualType();
}

static bool isRepeatableExpression(const Stmt *S) {
  if (const auto *Op = dyn_cast<CXXOperatorCallExpr>(S))
    return Op->getOperator() == OO_Call || Op->getOperator() == OO_Subscript;
  return isa<CallExpr>(S) || isa<ObjCMessageExpr>(S) ||
         isa<ObjCPropertyRefExpr>(S);
}

RefactoringOperationResult
clang::tooling::initiateExtractRepeatedExpressionIntoVariableOperation(
    ASTSlice &Slice, ASTContext &Context, SourceLocation Location,
    SourceRange SelectionRange, bool CreateOperation) {
  const Stmt *S;
  const Decl *ParentDecl;
  if (SelectionRange.isValid()) {
    auto SelectedStmt = Slice.getSelectedStmtSet();
    if (!SelectedStmt)
      return None;
    if (!SelectedStmt->containsSelectionRange)
      return None;
    if (!isRepeatableExpression(SelectedStmt->containsSelectionRange))
      return None;
    S = SelectedStmt->containsSelectionRange;
    ParentDecl =
        Slice.parentDeclForIndex(*SelectedStmt->containsSelectionRangeIndex);
  } else {
    auto SelectedStmt = Slice.nearestSelectedStmt(isRepeatableExpression);
    if (!SelectedStmt)
      return None;
    S = SelectedStmt->getStmt();
    ParentDecl = SelectedStmt->getParentDecl();
  }

  const Expr *E = cast<Expr>(S);
  // Check if the function/method returns a reference/pointer.
  QualType T = returnTypeOfCall(E);
  if (!T.getTypePtrOrNull() ||
      (!T->isAnyPointerType() && !T->isReferenceType()))
    return None;

  DuplicateExprFinder DupFinder(E, Context.getPrintingPolicy());
  DupFinder.TraverseDecl(const_cast<Decl *>(ParentDecl));
  if (DupFinder.DuplicateExpressions.size() < 2)
    return None;

  RefactoringOperationResult Result;
  Result.Initiated = true;
  if (!CreateOperation)
    return Result;
  auto Operation =
      llvm::make_unique<ExtractRepeatedExpressionIntoVariableOperation>(
          E, DupFinder.DuplicateExpressions, ParentDecl);
  Result.RefactoringOp = std::move(Operation);
  return Result;
}

namespace {

/// Checks if a set of expressions is directly contained in some AST region.
class StmtReachabilityChecker
    : public RecursiveASTVisitor<StmtReachabilityChecker> {
  const llvm::SmallPtrSetImpl<const Stmt *> &Expressions;
  unsigned Count = 0;

  StmtReachabilityChecker(
      const llvm::SmallPtrSetImpl<const Stmt *> &Expressions)
      : Expressions(Expressions) {}

  bool areAllExpressionsReached() const { return Count == Expressions.size(); }

public:
  bool VisitStmt(const Stmt *S) {
    if (Expressions.count(S)) {
      ++Count;
      if (areAllExpressionsReached())
        return false;
    }
    return true;
  }

  static bool areAllExpressionsReachableFrom(
      CompoundStmt *S, const llvm::SmallPtrSetImpl<const Stmt *> &Expressions) {
    StmtReachabilityChecker Checker(Expressions);
    Checker.TraverseStmt(S);
    return Checker.areAllExpressionsReached();
  }
};

/// Figures out where the extracted variable should go.
class ExtractedVariableInsertionLocFinder
    : public RecursiveASTVisitor<ExtractedVariableInsertionLocFinder> {
  llvm::SmallPtrSet<const Stmt *, 4> Expressions;
  llvm::SmallVector<std::pair<CompoundStmt *, const Stmt *>, 4>
      InsertionCandidateStack;
  bool IsPrevCompoundStmt = false;

public:
  SourceLocation Loc;

  /// Initializes the insertion location finder using the set of duplicate
  /// \p Expressions from one function.
  ExtractedVariableInsertionLocFinder(ArrayRef<const Expr *> Expressions) {
    for (const Expr *E : Expressions)
      this->Expressions.insert(E);
  }

  bool TraverseStmt(Stmt *S) {
    if (!S)
      return RecursiveASTVisitor::TraverseStmt(S);
    if (IsPrevCompoundStmt && !InsertionCandidateStack.empty())
      InsertionCandidateStack.back().second = S;
    llvm::SaveAndRestore<bool> IsPrevCompoundStmtTracker(IsPrevCompoundStmt,
                                                         false);
    if (auto *CS = dyn_cast<CompoundStmt>(S)) {
      IsPrevCompoundStmt = true;
      InsertionCandidateStack.emplace_back(CS, nullptr);
      return RecursiveASTVisitor::TraverseStmt(S);
    }
    return RecursiveASTVisitor::TraverseStmt(S);
  }

  bool VisitStmt(const Stmt *S) {
    if (Expressions.count(S)) {
      // The insertion location should be in the first compound statement that
      // includes all of the expressions as descendants as we want the new
      // variable to be visible to all uses.
      for (auto I = InsertionCandidateStack.rbegin(),
                E = InsertionCandidateStack.rend();
           I != E; ++I) {
        if (StmtReachabilityChecker::areAllExpressionsReachableFrom(
                I->first, Expressions) &&
            I->second) {
          Loc = I->second->getLocStart();
          break;
        }
      }
      return false;
    }
    return true;
  }
};

} // end anonymous namespace

static StringRef nameForExtractedVariable(const Expr *E) {
  if (const auto *Call = dyn_cast<CallExpr>(E)) {
    if (const auto *Fn = Call->getDirectCallee())
      return Fn->getName();
  } else if (const auto *Msg = dyn_cast<ObjCMessageExpr>(E)) {
    if (const auto *M = Msg->getMethodDecl()) {
      if (M->getSelector().isUnarySelector())
        return M->getSelector().getNameForSlot(0);
    }
  } else if (const auto *PRE = dyn_cast<ObjCPropertyRefExpr>(E)) {
    if (PRE->isImplicitProperty()) {
      if (const auto *M = PRE->getImplicitPropertyGetter())
        return M->getSelector().getNameForSlot(0);
    } else if (const auto *Prop = PRE->getExplicitProperty())
      return Prop->getName();
  }
  return "duplicate";
}

llvm::Expected<RefactoringResult>
ExtractRepeatedExpressionIntoVariableOperation::perform(
    ASTContext &Context, const Preprocessor &ThePreprocessor,
    const RefactoringOptionSet &Options, unsigned SelectedCandidateIndex) {
  std::vector<RefactoringReplacement> Replacements;

  const SourceManager &SM = Context.getSourceManager();
  ExtractedVariableInsertionLocFinder LocFinder(DuplicateExpressions);
  LocFinder.TraverseDecl(const_cast<Decl *>(ParentDecl));
  if (LocFinder.Loc.isInvalid())
    return llvm::make_error<RefactoringOperationError>(
        "no appropriate insertion location found");

  StringRef Name = nameForExtractedVariable(E);

  // Create the variable that will hold the value of the duplicate expression.
  std::string VariableDeclarationString;
  llvm::raw_string_ostream OS(VariableDeclarationString);
  QualType T = returnTypeOfCall(E);
  PrintingPolicy PP = Context.getPrintingPolicy();
  PP.SuppressStrongLifetime = true;
  PP.SuppressLifetimeQualifiers = true;
  PP.SuppressUnwrittenScope = true;
  T.print(OS, PP, /*PlaceHolder*/ Name);
  OS << " = ";
  E->printPretty(OS, /*Helper=*/nullptr, Context.getPrintingPolicy());
  OS << ";\n";
  Replacements.emplace_back(SourceRange(LocFinder.Loc, LocFinder.Loc),
                            OS.str());

  // Replace the duplicates with a reference to the variable.
  for (const Expr *E : DuplicateExpressions)
    Replacements.emplace_back(
        SourceRange(
            E->getLocStart(),
            getPreciseTokenLocEnd(E->getLocEnd(), SM, Context.getLangOpts())),
        Name);

  return std::move(Replacements);
}
