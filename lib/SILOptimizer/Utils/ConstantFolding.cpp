//===--- ConstantFolding.cpp - Utils for SIL constant folding -------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/SILOptimizer/Utils/ConstantFolding.h"

#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/Expr.h"
#include "swift/SIL/PatternMatch.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SILOptimizer/Utils/CastOptimizer.h"
#include "swift/SILOptimizer/Utils/Local.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "constant-folding"

using namespace swift;

APInt swift::constantFoldBitOperation(APInt lhs, APInt rhs, BuiltinValueKind ID) {
  switch (ID) {
    default: llvm_unreachable("Not all cases are covered!");
    case BuiltinValueKind::And:
      return lhs & rhs;
    case BuiltinValueKind::AShr:
      return lhs.ashr(rhs);
    case BuiltinValueKind::LShr:
      return lhs.lshr(rhs);
    case BuiltinValueKind::Or:
      return lhs | rhs;
    case BuiltinValueKind::Shl:
      return lhs.shl(rhs);
    case BuiltinValueKind::Xor:
      return lhs ^ rhs;
  }
}

APInt swift::constantFoldComparison(APInt lhs, APInt rhs, BuiltinValueKind ID) {
  bool result;
  switch (ID) {
    default: llvm_unreachable("Invalid integer compare kind");
    case BuiltinValueKind::ICMP_EQ:  result = lhs == rhs; break;
    case BuiltinValueKind::ICMP_NE:  result = lhs != rhs; break;
    case BuiltinValueKind::ICMP_SLT: result = lhs.slt(rhs); break;
    case BuiltinValueKind::ICMP_SGT: result = lhs.sgt(rhs); break;
    case BuiltinValueKind::ICMP_SLE: result = lhs.sle(rhs); break;
    case BuiltinValueKind::ICMP_SGE: result = lhs.sge(rhs); break;
    case BuiltinValueKind::ICMP_ULT: result = lhs.ult(rhs); break;
    case BuiltinValueKind::ICMP_UGT: result = lhs.ugt(rhs); break;
    case BuiltinValueKind::ICMP_ULE: result = lhs.ule(rhs); break;
    case BuiltinValueKind::ICMP_UGE: result = lhs.uge(rhs); break;
  }
  return APInt(1, result);
}

APInt swift::constantFoldBinaryWithOverflow(APInt lhs, APInt rhs,
                                            bool &Overflow,
                                            llvm::Intrinsic::ID ID) {
  switch (ID) {
    default: llvm_unreachable("Invalid case");
    case llvm::Intrinsic::sadd_with_overflow:
      return lhs.sadd_ov(rhs, Overflow);
    case llvm::Intrinsic::uadd_with_overflow:
      return lhs.uadd_ov(rhs, Overflow);
    case llvm::Intrinsic::ssub_with_overflow:
      return lhs.ssub_ov(rhs, Overflow);
    case llvm::Intrinsic::usub_with_overflow:
      return lhs.usub_ov(rhs, Overflow);
    case llvm::Intrinsic::smul_with_overflow:
      return lhs.smul_ov(rhs, Overflow);
    case llvm::Intrinsic::umul_with_overflow:
      return lhs.umul_ov(rhs, Overflow);
  }
}

APInt swift::constantFoldDiv(APInt lhs, APInt rhs, bool &Overflow,
                             BuiltinValueKind ID) {
  assert(rhs != 0 && "division by zero");
  switch (ID) {
    default : llvm_unreachable("Invalid case");
    case BuiltinValueKind::SDiv:
      return lhs.sdiv_ov(rhs, Overflow);
    case BuiltinValueKind::SRem: {
      // Check for overflow
      APInt Div = lhs.sdiv_ov(rhs, Overflow);
      (void)Div;
      return lhs.srem(rhs);
    }
    case BuiltinValueKind::UDiv:
      Overflow = false;
      return lhs.udiv(rhs);
    case BuiltinValueKind::URem:
      Overflow = false;
      return lhs.urem(rhs);
  }
}

APInt swift::constantFoldCast(APInt val, const BuiltinInfo &BI) {
  // Get the cast result.
  Type SrcTy = BI.Types[0];
  Type DestTy = BI.Types.size() == 2 ? BI.Types[1] : Type();
  uint32_t SrcBitWidth =
  SrcTy->castTo<BuiltinIntegerType>()->getGreatestWidth();
  uint32_t DestBitWidth =
  DestTy->castTo<BuiltinIntegerType>()->getGreatestWidth();
  
  APInt CastResV;
  if (SrcBitWidth == DestBitWidth) {
    return val;
  } else switch (BI.ID) {
    default : llvm_unreachable("Invalid case.");
    case BuiltinValueKind::Trunc:
    case BuiltinValueKind::TruncOrBitCast:
      return val.trunc(DestBitWidth);
    case BuiltinValueKind::ZExt:
    case BuiltinValueKind::ZExtOrBitCast:
      return val.zext(DestBitWidth);
      break;
    case BuiltinValueKind::SExt:
    case BuiltinValueKind::SExtOrBitCast:
      return val.sext(DestBitWidth);
  }
}

//===----------------------------------------------------------------------===//
//                           ConstantFolder
//===----------------------------------------------------------------------===//

STATISTIC(NumInstFolded, "Number of constant folded instructions");

template<typename...T, typename...U>
static InFlightDiagnostic
diagnose(ASTContext &Context, SourceLoc loc, Diag<T...> diag, U &&...args) {
  return Context.Diags.diagnose(loc, diag, std::forward<U>(args)...);
}

/// \brief Construct (int, overflow) result tuple.
static SILValue constructResultWithOverflowTuple(BuiltinInst *BI,
                                                 APInt Res, bool Overflow) {
  // Get the SIL subtypes of the returned tuple type.
  SILType FuncResType = BI->getType();
  assert(FuncResType.castTo<TupleType>()->getNumElements() == 2);
  SILType ResTy1 = FuncResType.getTupleElementType(0);
  SILType ResTy2 = FuncResType.getTupleElementType(1);

  // Construct the folded instruction - a tuple of two literals, the
  // result and overflow.
  SILBuilderWithScope B(BI);
  SILLocation Loc = BI->getLoc();
  SILValue Result[] = {
    B.createIntegerLiteral(Loc, ResTy1, Res),
    B.createIntegerLiteral(Loc, ResTy2, Overflow)
  };
  return B.createTuple(Loc, FuncResType, Result);
}

/// \brief Fold arithmetic intrinsics with overflow.
static SILValue
constantFoldBinaryWithOverflow(BuiltinInst *BI, llvm::Intrinsic::ID ID,
                               bool ReportOverflow,
                               Optional<bool> &ResultsInError) {
  OperandValueArrayRef Args = BI->getArguments();
  assert(Args.size() >= 2);

  auto *Op1 = dyn_cast<IntegerLiteralInst>(Args[0]);
  auto *Op2 = dyn_cast<IntegerLiteralInst>(Args[1]);

  // If either Op1 or Op2 is not a literal, we cannot do anything.
  if (!Op1 || !Op2)
    return nullptr;

  // Calculate the result.
  APInt LHSInt = Op1->getValue();
  APInt RHSInt = Op2->getValue();
  bool Overflow;
  APInt Res = constantFoldBinaryWithOverflow(LHSInt, RHSInt, Overflow, ID);

  // If we can statically determine that the operation overflows,
  // warn about it if warnings are not disabled by ResultsInError being null.
  if (ResultsInError.hasValue() && Overflow && ReportOverflow) {
    if (BI->getFunction()->isSpecialization()) {
      // Do not report any constant propagation issues in specializations,
      // because they are eventually not present in the original function.
      return nullptr;
    }
    // Try to infer the type of the constant expression that the user operates
    // on. If the intrinsic was lowered from a call to a function that takes
    // two arguments of the same type, use the type of the LHS argument.
    // This would detect '+'/'+=' and such.
    Type OpType;
    SILLocation Loc = BI->getLoc();
    const ApplyExpr *CE = Loc.getAsASTNode<ApplyExpr>();
    SourceRange LHSRange, RHSRange;
    if (CE) {
      const auto *Args = dyn_cast_or_null<TupleExpr>(CE->getArg());
      if (Args && Args->getNumElements() == 2) {
        // Look through inout types in order to handle += well.
        CanType LHSTy = Args->getElement(0)->getType()->getInOutObjectType()->
                         getCanonicalType();
        CanType RHSTy = Args->getElement(1)->getType()->getCanonicalType();
        if (LHSTy == RHSTy)
          OpType = Args->getElement(1)->getType();

        LHSRange = Args->getElement(0)->getSourceRange();
        RHSRange = Args->getElement(1)->getSourceRange();
      }
    }

    bool Signed = false;
    StringRef Operator = "+";

    switch (ID) {
      default: llvm_unreachable("Invalid case");
      case llvm::Intrinsic::sadd_with_overflow:
        Signed = true;
        break;
      case llvm::Intrinsic::uadd_with_overflow:
        break;
      case llvm::Intrinsic::ssub_with_overflow:
        Operator = "-";
        Signed = true;
        break;
      case llvm::Intrinsic::usub_with_overflow:
        Operator = "-";
        break;
      case llvm::Intrinsic::smul_with_overflow:
        Operator = "*";
        Signed = true;
        break;
      case llvm::Intrinsic::umul_with_overflow:
        Operator = "*";
        break;
    }

    if (!OpType.isNull()) {
      diagnose(BI->getModule().getASTContext(),
               Loc.getSourceLoc(),
               diag::arithmetic_operation_overflow,
               LHSInt.toString(/*Radix*/ 10, Signed),
               Operator,
               RHSInt.toString(/*Radix*/ 10, Signed),
               OpType).highlight(LHSRange).highlight(RHSRange);
    } else {
      // If we cannot get the type info in an expected way, describe the type.
      diagnose(BI->getModule().getASTContext(),
               Loc.getSourceLoc(),
               diag::arithmetic_operation_overflow_generic_type,
               LHSInt.toString(/*Radix*/ 10, Signed),
               Operator,
               RHSInt.toString(/*Radix*/ 10, Signed),
               Signed,
               LHSInt.getBitWidth()).highlight(LHSRange).highlight(RHSRange);
    }
    ResultsInError = Optional<bool>(true);
  }

  return constructResultWithOverflowTuple(BI, Res, Overflow);
}

static SILValue
constantFoldBinaryWithOverflow(BuiltinInst *BI, BuiltinValueKind ID,
                               Optional<bool> &ResultsInError) {
  OperandValueArrayRef Args = BI->getArguments();
  auto *ShouldReportFlag = dyn_cast<IntegerLiteralInst>(Args[2]);
  return constantFoldBinaryWithOverflow(BI,
           getLLVMIntrinsicIDForBuiltinWithOverflow(ID),
           ShouldReportFlag && (ShouldReportFlag->getValue() == 1),
           ResultsInError);
}

static SILValue constantFoldIntrinsic(BuiltinInst *BI, llvm::Intrinsic::ID ID,
                                      Optional<bool> &ResultsInError) {
  switch (ID) {
  default: break;
  case llvm::Intrinsic::expect: {
    // An expect of an integral constant is the constant itself.
    assert(BI->getArguments().size() == 2 && "Expect should have 2 args.");
    auto *Op1 = dyn_cast<IntegerLiteralInst>(BI->getArguments()[0]);
    if (!Op1)
      return nullptr;
    return Op1;
  }

  case llvm::Intrinsic::ctlz: {
    assert(BI->getArguments().size() == 2 && "Ctlz should have 2 args.");
    OperandValueArrayRef Args = BI->getArguments();

    // Fold for integer constant arguments.
    auto *LHS = dyn_cast<IntegerLiteralInst>(Args[0]);
    if (!LHS) {
      return nullptr;
    }
    APInt LHSI = LHS->getValue();
    unsigned LZ = 0;
    // Check corner-case of source == zero
    if (LHSI == 0) {
      auto *RHS = dyn_cast<IntegerLiteralInst>(Args[1]);
      if (!RHS || RHS->getValue() != 0) {
        // Undefined
        return nullptr;
      }
      LZ = LHSI.getBitWidth();
    } else {
      LZ = LHSI.countLeadingZeros();
    }
    APInt LZAsAPInt = APInt(LHSI.getBitWidth(), LZ);
    SILBuilderWithScope B(BI);
    return B.createIntegerLiteral(BI->getLoc(), LHS->getType(), LZAsAPInt);
  }

  case llvm::Intrinsic::sadd_with_overflow:
  case llvm::Intrinsic::uadd_with_overflow:
  case llvm::Intrinsic::ssub_with_overflow:
  case llvm::Intrinsic::usub_with_overflow:
  case llvm::Intrinsic::smul_with_overflow:
  case llvm::Intrinsic::umul_with_overflow:
    return constantFoldBinaryWithOverflow(BI, ID,
                                          /* ReportOverflow */ false,
                                          ResultsInError);
  }
  return nullptr;
}

static SILValue constantFoldCompare(BuiltinInst *BI, BuiltinValueKind ID) {
  OperandValueArrayRef Args = BI->getArguments();

  // Fold for integer constant arguments.
  auto *LHS = dyn_cast<IntegerLiteralInst>(Args[0]);
  auto *RHS = dyn_cast<IntegerLiteralInst>(Args[1]);
  if (LHS && RHS) {
    APInt Res = constantFoldComparison(LHS->getValue(), RHS->getValue(), ID);
    SILBuilderWithScope B(BI);
    return B.createIntegerLiteral(BI->getLoc(), BI->getType(), Res);
  }

  using namespace swift::PatternMatch;

  // Comparisons of an unsigned value with 0.
  SILValue Other;
  auto MatchNonNegative =
      m_BuiltinInst(BuiltinValueKind::AssumeNonNegative, m_ValueBase());
  if (match(BI, m_CombineOr(m_BuiltinInst(BuiltinValueKind::ICMP_ULT,
                                          m_SILValue(Other), m_Zero()),
                            m_BuiltinInst(BuiltinValueKind::ICMP_UGT, m_Zero(),
                                          m_SILValue(Other)))) ||
      match(BI, m_CombineOr(m_BuiltinInst(BuiltinValueKind::ICMP_SLT,
                                          MatchNonNegative, m_Zero()),
                            m_BuiltinInst(BuiltinValueKind::ICMP_SGT, m_Zero(),
                                          MatchNonNegative)))) {
    SILBuilderWithScope B(BI);
    return B.createIntegerLiteral(BI->getLoc(), BI->getType(), APInt());
  }

  if (match(BI, m_CombineOr(m_BuiltinInst(BuiltinValueKind::ICMP_UGE,
                                          m_SILValue(Other), m_Zero()),
                            m_BuiltinInst(BuiltinValueKind::ICMP_ULE, m_Zero(),
                                          m_SILValue(Other)))) ||
      match(BI, m_CombineOr(m_BuiltinInst(BuiltinValueKind::ICMP_SGE,
                                          MatchNonNegative, m_Zero()),
                            m_BuiltinInst(BuiltinValueKind::ICMP_SLE, m_Zero(),
                                          MatchNonNegative)))) {
    SILBuilderWithScope B(BI);
    return B.createIntegerLiteral(BI->getLoc(), BI->getType(), APInt(1, 1));
  }

  // Comparisons with Int.Max.
  IntegerLiteralInst *IntMax;

  // Check signed comparisons.
  if (match(BI,
            m_CombineOr(
                // Int.max < x
                m_BuiltinInst(BuiltinValueKind::ICMP_SLT,
                              m_IntegerLiteralInst(IntMax), m_SILValue(Other)),
                // x > Int.max
                m_BuiltinInst(BuiltinValueKind::ICMP_SGT, m_SILValue(Other),
                              m_IntegerLiteralInst(IntMax)))) &&
      IntMax->getValue().isMaxSignedValue()) {
    // Any signed number should be <= then IntMax.
    SILBuilderWithScope B(BI);
    return B.createIntegerLiteral(BI->getLoc(), BI->getType(), APInt());
  }

  if (match(BI,
            m_CombineOr(
                m_BuiltinInst(BuiltinValueKind::ICMP_SGE,
                              m_IntegerLiteralInst(IntMax), m_SILValue(Other)),
                m_BuiltinInst(BuiltinValueKind::ICMP_SLE, m_SILValue(Other),
                              m_IntegerLiteralInst(IntMax)))) &&
      IntMax->getValue().isMaxSignedValue()) {
    // Any signed number should be <= then IntMax.
    SILBuilderWithScope B(BI);
    return B.createIntegerLiteral(BI->getLoc(), BI->getType(), APInt(1, 1));
  }

  // For any x of the same size as Int.max and n>=1 , (x>>n) is always <= Int.max,
  // that is (x>>n) <= Int.max and Int.max >= (x>>n) are true.
  if (match(BI,
            m_CombineOr(
                // Int.max >= x
                m_BuiltinInst(BuiltinValueKind::ICMP_UGE,
                              m_IntegerLiteralInst(IntMax), m_SILValue(Other)),
                // x <= Int.max
                m_BuiltinInst(BuiltinValueKind::ICMP_ULE, m_SILValue(Other),
                              m_IntegerLiteralInst(IntMax)),
                // Int.max >= x
                m_BuiltinInst(BuiltinValueKind::ICMP_SGE,
                              m_IntegerLiteralInst(IntMax), m_SILValue(Other)),
                // x <= Int.max
                m_BuiltinInst(BuiltinValueKind::ICMP_SLE, m_SILValue(Other),
                              m_IntegerLiteralInst(IntMax)))) &&
      IntMax->getValue().isMaxSignedValue()) {
    // Check if other is a result of a logical shift right by a strictly
    // positive number of bits.
    IntegerLiteralInst *ShiftCount;
    if (match(Other, m_BuiltinInst(BuiltinValueKind::LShr, m_ValueBase(),
                                   m_IntegerLiteralInst(ShiftCount))) &&
        ShiftCount->getValue().isStrictlyPositive()) {
      SILBuilderWithScope B(BI);
      return B.createIntegerLiteral(BI->getLoc(), BI->getType(), APInt(1, 1));
    }
  }

  // At the same time (x>>n) > Int.max and Int.max < (x>>n) is false.
  if (match(BI,
            m_CombineOr(
                // Int.max < x
                m_BuiltinInst(BuiltinValueKind::ICMP_ULT,
                              m_IntegerLiteralInst(IntMax), m_SILValue(Other)),
                // x > Int.max
                m_BuiltinInst(BuiltinValueKind::ICMP_UGT, m_SILValue(Other),
                              m_IntegerLiteralInst(IntMax)),
                // Int.max < x
                m_BuiltinInst(BuiltinValueKind::ICMP_SLT,
                              m_IntegerLiteralInst(IntMax), m_SILValue(Other)),
                // x > Int.max
                m_BuiltinInst(BuiltinValueKind::ICMP_SGT, m_SILValue(Other),
                              m_IntegerLiteralInst(IntMax)))) &&
      IntMax->getValue().isMaxSignedValue()) {
    // Check if other is a result of a logical shift right by a strictly
    // positive number of bits.
    IntegerLiteralInst *ShiftCount;
    if (match(Other, m_BuiltinInst(BuiltinValueKind::LShr, m_ValueBase(),
                                   m_IntegerLiteralInst(ShiftCount))) &&
        ShiftCount->getValue().isStrictlyPositive()) {
      SILBuilderWithScope B(BI);
      return B.createIntegerLiteral(BI->getLoc(), BI->getType(), APInt());
    }
  }

  // Fold x < 0 into false, if x is known to be a result of an unsigned
  // operation with overflow checks enabled.
  BuiltinInst *BIOp;
  if (match(BI, m_BuiltinInst(BuiltinValueKind::ICMP_SLT,
                              m_TupleExtractInst(m_BuiltinInst(BIOp), 0),
                              m_Zero()))) {
    // Check if Other is a result of an unsigned operation with overflow.
    switch (BIOp->getBuiltinInfo().ID) {
    default:
      break;
    case BuiltinValueKind::UAddOver:
    case BuiltinValueKind::USubOver:
    case BuiltinValueKind::UMulOver:
      // Was it an operation with an overflow check?
      if (match(BIOp->getOperand(2), m_One())) {
        SILBuilderWithScope B(BI);
        return B.createIntegerLiteral(BI->getLoc(), BI->getType(), APInt());
      }
    }
  }

  // Fold x >= 0 into true, if x is known to be a result of an unsigned
  // operation with overflow checks enabled.
  if (match(BI, m_BuiltinInst(BuiltinValueKind::ICMP_SGE,
                              m_TupleExtractInst(m_BuiltinInst(BIOp), 0),
                              m_Zero()))) {
    // Check if Other is a result of an unsigned operation with overflow.
    switch (BIOp->getBuiltinInfo().ID) {
    default:
      break;
    case BuiltinValueKind::UAddOver:
    case BuiltinValueKind::USubOver:
    case BuiltinValueKind::UMulOver:
      // Was it an operation with an overflow check?
      if (match(BIOp->getOperand(2), m_One())) {
        SILBuilderWithScope B(BI);
        return B.createIntegerLiteral(BI->getLoc(), BI->getType(), APInt(1, 1));
      }
    }
  }

  return nullptr;
}

static SILValue
constantFoldAndCheckDivision(BuiltinInst *BI, BuiltinValueKind ID,
                             Optional<bool> &ResultsInError) {
  assert(ID == BuiltinValueKind::SDiv ||
         ID == BuiltinValueKind::SRem ||
         ID == BuiltinValueKind::UDiv ||
         ID == BuiltinValueKind::URem);

  OperandValueArrayRef Args = BI->getArguments();
  SILModule &M = BI->getModule();

  // Get the denominator.
  auto *Denom = dyn_cast<IntegerLiteralInst>(Args[1]);
  if (!Denom)
    return nullptr;
  APInt DenomVal = Denom->getValue();

  // If the denominator is zero...
  if (DenomVal == 0) {
    // And if we are not asked to report errors, just return nullptr.
    if (!ResultsInError.hasValue())
      return nullptr;

    // Otherwise emit a diagnosis error and set ResultsInError to true.
    diagnose(M.getASTContext(), BI->getLoc().getSourceLoc(),
             diag::division_by_zero);
    ResultsInError = Optional<bool>(true);
    return nullptr;
  }

  // Get the numerator.
  auto *Num = dyn_cast<IntegerLiteralInst>(Args[0]);
  if (!Num)
    return nullptr;
  APInt NumVal = Num->getValue();

  bool Overflowed;
  APInt ResVal = constantFoldDiv(NumVal, DenomVal, Overflowed, ID);

  // If we overflowed...
  if (Overflowed) {
    // And we are not asked to produce diagnostics, just return nullptr...
    if (!ResultsInError.hasValue())
      return nullptr;

    bool IsRem = ID == BuiltinValueKind::SRem || ID == BuiltinValueKind::URem;

    // Otherwise emit the diagnostic, set ResultsInError to be true, and return
    // nullptr.
    diagnose(M.getASTContext(),
             BI->getLoc().getSourceLoc(),
             diag::division_overflow,
             NumVal.toString(/*Radix*/ 10, /*Signed*/true),
             IsRem ? "%" : "/",
             DenomVal.toString(/*Radix*/ 10, /*Signed*/true));
    ResultsInError = Optional<bool>(true);
    return nullptr;
  }

  // Add the literal instruction to represent the result of the division.
  SILBuilderWithScope B(BI);
  return B.createIntegerLiteral(BI->getLoc(), BI->getType(), ResVal);
}

/// \brief Fold binary operations.
///
/// The list of operations we constant fold might not be complete. Start with
/// folding the operations used by the standard library.
static SILValue constantFoldBinary(BuiltinInst *BI,
                                   BuiltinValueKind ID,
                                   Optional<bool> &ResultsInError) {
  switch (ID) {
  default:
    llvm_unreachable("Not all BUILTIN_BINARY_OPERATIONs are covered!");

  // Not supported yet (not easily computable for APInt).
  case BuiltinValueKind::ExactSDiv:
  case BuiltinValueKind::ExactUDiv:
    return nullptr;

  // Not supported now.
  case BuiltinValueKind::FRem:
    return nullptr;

  // Fold constant division operations and report div by zero.
  case BuiltinValueKind::SDiv:
  case BuiltinValueKind::SRem:
  case BuiltinValueKind::UDiv:
  case BuiltinValueKind::URem: {
    return constantFoldAndCheckDivision(BI, ID, ResultsInError);
  }

  // Are there valid uses for these in stdlib?
  case BuiltinValueKind::Add:
  case BuiltinValueKind::Mul:
  case BuiltinValueKind::Sub:
    return nullptr;

  case BuiltinValueKind::And:
  case BuiltinValueKind::AShr:
  case BuiltinValueKind::LShr:
  case BuiltinValueKind::Or:
  case BuiltinValueKind::Shl:
  case BuiltinValueKind::Xor: {
    OperandValueArrayRef Args = BI->getArguments();
    auto *LHS = dyn_cast<IntegerLiteralInst>(Args[0]);
    auto *RHS = dyn_cast<IntegerLiteralInst>(Args[1]);
    if (!RHS || !LHS)
      return nullptr;
    APInt LHSI = LHS->getValue();
    APInt RHSI = RHS->getValue();

    bool IsShift = ID == BuiltinValueKind::AShr ||
                   ID == BuiltinValueKind::LShr ||
                   ID == BuiltinValueKind::Shl;

    // Reject shifting all significant bits
    if (IsShift && RHSI.getZExtValue() >= LHSI.getBitWidth()) {
      diagnose(BI->getModule().getASTContext(),
               RHS->getLoc().getSourceLoc(),
               diag::shifting_all_significant_bits);

      ResultsInError = Optional<bool>(true);
      return nullptr;
    }

    APInt ResI = constantFoldBitOperation(LHSI, RHSI, ID);
    // Add the literal instruction to represent the result.
    SILBuilderWithScope B(BI);
    return B.createIntegerLiteral(BI->getLoc(), BI->getType(), ResI);
  }
  case BuiltinValueKind::FAdd:
  case BuiltinValueKind::FDiv:
  case BuiltinValueKind::FMul:
  case BuiltinValueKind::FSub: {
    OperandValueArrayRef Args = BI->getArguments();
    auto *LHS = dyn_cast<FloatLiteralInst>(Args[0]);
    auto *RHS = dyn_cast<FloatLiteralInst>(Args[1]);
    if (!RHS || !LHS)
      return nullptr;
    APFloat LHSF = LHS->getValue();
    APFloat RHSF = RHS->getValue();
    switch (ID) {
    default: llvm_unreachable("Not all cases are covered!");
    case BuiltinValueKind::FAdd:
      LHSF.add(RHSF, APFloat::rmNearestTiesToEven);
      break;
    case BuiltinValueKind::FDiv:
      LHSF.divide(RHSF, APFloat::rmNearestTiesToEven);
      break;
    case BuiltinValueKind::FMul:
      LHSF.multiply(RHSF, APFloat::rmNearestTiesToEven);
      break;
    case BuiltinValueKind::FSub:
      LHSF.subtract(RHSF, APFloat::rmNearestTiesToEven);
      break;
    }

    // Add the literal instruction to represent the result.
    SILBuilderWithScope B(BI);
    return B.createFloatLiteral(BI->getLoc(), BI->getType(), LHSF);
  }
  }
}

static std::pair<bool, bool> getTypeSignedness(const BuiltinInfo &Builtin) {
  bool SrcTySigned =
  (Builtin.ID == BuiltinValueKind::SToSCheckedTrunc ||
   Builtin.ID == BuiltinValueKind::SToUCheckedTrunc ||
   Builtin.ID == BuiltinValueKind::SUCheckedConversion);

  bool DstTySigned =
  (Builtin.ID == BuiltinValueKind::SToSCheckedTrunc ||
   Builtin.ID == BuiltinValueKind::UToSCheckedTrunc ||
   Builtin.ID == BuiltinValueKind::USCheckedConversion);

  return std::pair<bool, bool>(SrcTySigned, DstTySigned);
}

static SILValue
constantFoldAndCheckIntegerConversions(BuiltinInst *BI,
                                       const BuiltinInfo &Builtin,
                                       Optional<bool> &ResultsInError) {
  assert(Builtin.ID == BuiltinValueKind::SToSCheckedTrunc ||
         Builtin.ID == BuiltinValueKind::UToUCheckedTrunc ||
         Builtin.ID == BuiltinValueKind::SToUCheckedTrunc ||
         Builtin.ID == BuiltinValueKind::UToSCheckedTrunc ||
         Builtin.ID == BuiltinValueKind::SUCheckedConversion ||
         Builtin.ID == BuiltinValueKind::USCheckedConversion);

  // Check if we are converting a constant integer.
  OperandValueArrayRef Args = BI->getArguments();
  auto *V = dyn_cast<IntegerLiteralInst>(Args[0]);
  if (!V)
    return nullptr;
  APInt SrcVal = V->getValue();

  // Get source type and bit width.
  Type SrcTy = Builtin.Types[0];
  uint32_t SrcBitWidth =
    Builtin.Types[0]->castTo<BuiltinIntegerType>()->getGreatestWidth();

  // Compute the destination (for SrcBitWidth < DestBitWidth) and enough info
  // to check for overflow.
  APInt Result;
  bool OverflowError;
  Type DstTy;

  // Process conversions signed <-> unsigned for same size integers.
  if (Builtin.ID == BuiltinValueKind::SUCheckedConversion ||
      Builtin.ID == BuiltinValueKind::USCheckedConversion) {
    DstTy = SrcTy;
    Result = SrcVal;
    // Report an error if the sign bit is set.
    OverflowError = SrcVal.isNegative();

  // Process truncation from unsigned to signed.
  } else if (Builtin.ID != BuiltinValueKind::UToSCheckedTrunc) {
    assert(Builtin.Types.size() == 2);
    DstTy = Builtin.Types[1];
    uint32_t DstBitWidth =
      DstTy->castTo<BuiltinIntegerType>()->getGreatestWidth();
    //     Result = trunc_IntFrom_IntTo(Val)
    //   For signed destination:
    //     sext_IntFrom(Result) == Val ? Result : overflow_error
    //   For signed destination:
    //     zext_IntFrom(Result) == Val ? Result : overflow_error
    Result = SrcVal.trunc(DstBitWidth);
    // Get the signedness of the destination.
    bool Signed = (Builtin.ID == BuiltinValueKind::SToSCheckedTrunc);
    APInt Ext = Signed ? Result.sext(SrcBitWidth) : Result.zext(SrcBitWidth);
    OverflowError = (SrcVal != Ext);

  // Process the rest of truncations.
  } else {
    assert(Builtin.Types.size() == 2);
    DstTy = Builtin.Types[1];
    uint32_t DstBitWidth =
      Builtin.Types[1]->castTo<BuiltinIntegerType>()->getGreatestWidth();
    // Compute the destination (for SrcBitWidth < DestBitWidth):
    //   Result = trunc_IntTo(Val)
    //   Trunc  = trunc_'IntTo-1bit'(Val)
    //   zext_IntFrom(Trunc) == Val ? Result : overflow_error
    Result = SrcVal.trunc(DstBitWidth);
    APInt TruncVal = SrcVal.trunc(DstBitWidth - 1);
    OverflowError = (SrcVal != TruncVal.zext(SrcBitWidth));
  }

  // Check for overflow.
  if (OverflowError) {
    // If we are not asked to emit overflow diagnostics, just return nullptr on
    // overflow.
    if (!ResultsInError.hasValue())
      return nullptr;

    SILLocation Loc = BI->getLoc();
    SILModule &M = BI->getModule();
    const ApplyExpr *CE = Loc.getAsASTNode<ApplyExpr>();
    Type UserSrcTy;
    Type UserDstTy;
    // Primitive heuristics to get the user-written type.
    // Eventually we might be able to use SILLocation (when it contains info
    // about inlined call chains).
    if (CE) {
      if (const TupleType *RTy = CE->getArg()->getType()->getAs<TupleType>()) {
        if (RTy->getNumElements() == 1) {
          UserSrcTy = RTy->getElementType(0);
          UserDstTy = CE->getType();
        }
      } else {
        UserSrcTy = CE->getArg()->getType();
        UserDstTy = CE->getType();
      }
    }


    // Assume that we are converting from a literal if the Source size is
    // 2048. Is there a better way to identify conversions from literals?
    bool Literal = (SrcBitWidth == 2048);

    // FIXME: This will prevent hard error in cases the error is coming
    // from ObjC interoperability code. Currently, we treat NSUInteger as
    // Int.
    if (Loc.getSourceLoc().isInvalid()) {
      // Otherwise emit the appropriate diagnostic and set ResultsInError.
      if (Literal)
        diagnose(M.getASTContext(), Loc.getSourceLoc(),
                 diag::integer_literal_overflow_warn,
                 UserDstTy.isNull() ? DstTy : UserDstTy);
      else
        diagnose(M.getASTContext(), Loc.getSourceLoc(),
                 diag::integer_conversion_overflow_warn,
                 UserSrcTy.isNull() ? SrcTy : UserSrcTy,
                 UserDstTy.isNull() ? DstTy : UserDstTy);

      ResultsInError = Optional<bool>(true);
      return nullptr;
    }

    // Otherwise report the overflow error.
    if (Literal) {
      bool SrcTySigned, DstTySigned;
      std::tie(SrcTySigned, DstTySigned) = getTypeSignedness(Builtin);
      SmallString<10> SrcAsString;
      SrcVal.toString(SrcAsString, /*radix*/10, SrcTySigned);

      // Try to print user-visible types if they are available.
      if (!UserDstTy.isNull()) {
        auto diagID = diag::integer_literal_overflow;

        // If this is a negative literal in an unsigned type, use a specific
        // diagnostic.
        if (SrcTySigned && !DstTySigned && SrcVal.isNegative())
          diagID = diag::negative_integer_literal_overflow_unsigned;

        diagnose(M.getASTContext(), Loc.getSourceLoc(),
                 diagID, UserDstTy, SrcAsString);
      // Otherwise, print the Builtin Types.
      } else {
        bool SrcTySigned, DstTySigned;
        std::tie(SrcTySigned, DstTySigned) = getTypeSignedness(Builtin);
        diagnose(M.getASTContext(), Loc.getSourceLoc(),
                 diag::integer_literal_overflow_builtin_types,
                 DstTySigned, DstTy, SrcAsString);
      }
    } else {
      if (Builtin.ID == BuiltinValueKind::SUCheckedConversion) {
        diagnose(M.getASTContext(), Loc.getSourceLoc(),
                 diag::integer_conversion_sign_error,
                 UserDstTy.isNull() ? DstTy : UserDstTy);
      } else {
        // Try to print user-visible types if they are available.
        if (!UserSrcTy.isNull()) {
          diagnose(M.getASTContext(), Loc.getSourceLoc(),
                   diag::integer_conversion_overflow,
                   UserSrcTy, UserDstTy);

        // Otherwise, print the Builtin Types.
        } else {
          // Since builtin types are sign-agnostic, print the signedness
          // separately.
          bool SrcTySigned, DstTySigned;
          std::tie(SrcTySigned, DstTySigned) = getTypeSignedness(Builtin);
          diagnose(M.getASTContext(), Loc.getSourceLoc(),
                   diag::integer_conversion_overflow_builtin_types,
                   SrcTySigned, SrcTy, DstTySigned, DstTy);
        }
      }
    }

    ResultsInError = Optional<bool>(true);
    return nullptr;
  }

  // The call to the builtin should be replaced with the constant value.
  return constructResultWithOverflowTuple(BI, Result, false);

}

/// A utility function that extracts the literal text corresponding
/// to a given FloatLiteralInst the way it appears in the AST.
/// This function can be used on FloatLiteralInsts generated by the
/// constant folding phase.
/// If the extraction is successful, the function returns true and
/// 'fpStr' contains the literal the way it appears in the AST.
/// If the extraction is unsuccessful, e.g. because there is no AST
/// for the FloatLiteralInst, the function returns false.
template<unsigned N>
static bool tryExtractLiteralText(FloatLiteralInst *flitInst,
                                  SmallString<N> &fpStr) {

  Expr *expr = flitInst->getLoc().getAsASTNode<Expr>();
  if (!expr)
    return false;

  // 'expr' may not be a FloatLiteralExpr since 'flitInst' could have been
  // created by the ConstantFolder by folding floating-point constructor calls.
  // So we iterate through the sequence of folded constructors if any, and
  // try to extract the FloatLiteralExpr.
  while (auto *callExpr = dyn_cast<CallExpr>(expr)) {
    if (callExpr->getNumArguments() != 1 ||
         !dyn_cast<ConstructorRefCallExpr>(callExpr->getFn()))
      break;

    auto *tupleExpr = dyn_cast<TupleExpr>(callExpr->getArg());
    if (!tupleExpr)
      break;

    expr = tupleExpr->getElement(0);
  }

  auto *flitExpr = dyn_cast<FloatLiteralExpr>(expr);
  if (!flitExpr)
    return false;

  if (flitExpr->isNegative())
    fpStr += '-';
  fpStr += flitExpr->getDigitsText();
  return true;
}

static SILValue foldFPToIntConversion(BuiltinInst *BI,
      const BuiltinInfo &Builtin, Optional<bool> &ResultsInError) {

  assert(Builtin.ID == BuiltinValueKind::FPToSI ||
         Builtin.ID == BuiltinValueKind::FPToUI);

  OperandValueArrayRef Args = BI->getArguments();
  bool conversionToUnsigned = (Builtin.ID == BuiltinValueKind::FPToUI);

  auto *flitInst = dyn_cast<FloatLiteralInst>(Args[0]);
  if (!flitInst)
    return nullptr;
  APFloat fpVal = flitInst->getValue();
  auto *destTy = Builtin.Types[1]->castTo<BuiltinIntegerType>();

  // Check non-negativeness of 'fpVal' for conversion to unsigned int.
  if (conversionToUnsigned && fpVal.isNegative() && !fpVal.isZero()) {
    // Stop folding and emit diagnostics if enabled.
    if (ResultsInError.hasValue()) {
      SILModule &M = BI->getModule();
      const ApplyExpr *CE = BI->getLoc().getAsASTNode<ApplyExpr>();

      SmallString<10> fpStr;
      if (!tryExtractLiteralText(flitInst, fpStr))
        flitInst->getValue().toString(fpStr);

      diagnose(M.getASTContext(), BI->getLoc().getSourceLoc(),
               diag::negative_fp_literal_overflow_unsigned, fpStr,
               CE ? CE->getType() : destTy,
               CE ? false : conversionToUnsigned);
      ResultsInError = Optional<bool>(true);
    }
    return nullptr;
  }

  llvm::APSInt resInt(destTy->getFixedWidth(), conversionToUnsigned);
  bool isExact = false;
  APFloat::opStatus status =
    fpVal.convertToInteger(resInt, APFloat::rmTowardZero, &isExact);

  if (status & APFloat::opStatus::opInvalidOp) {
    // Stop folding and emit diagnostics if enabled.
    if (ResultsInError.hasValue()) {
      SILModule &M = BI->getModule();
      const ApplyExpr *CE = BI->getLoc().getAsASTNode<ApplyExpr>();

      SmallString<10> fpStr;
      if (!tryExtractLiteralText(flitInst, fpStr))
        flitInst->getValue().toString(fpStr);

      diagnose(M.getASTContext(), BI->getLoc().getSourceLoc(),
               diag::float_to_int_overflow, fpStr,
               CE ? CE->getType() : destTy,
               CE ? CE->isImplicit() : false);
      ResultsInError = Optional<bool>(true);
    }
    return nullptr;
  }

  if (status != APFloat::opStatus::opOK &&
      status != APFloat::opStatus::opInexact) {
    return nullptr;
  }
  // The call to the builtin should be replaced with the constant value.
  SILBuilderWithScope B(BI);
  return B.createIntegerLiteral(BI->getLoc(), BI->getType(), resInt);
}

/// Captures the layout of IEEE754 floating point values.
struct IEEESemantics {
  uint8_t bitWidth;
  uint8_t exponentBitWidth;
  uint8_t significandBitWidth; // Ignores the integer part.
  bool explicitIntegerPart;
  int minExponent;

public:
  IEEESemantics(uint8_t bits, uint8_t expBits, uint8_t sigBits,
                bool explicitIntPart) {
    bitWidth = bits;
    exponentBitWidth = expBits;
    significandBitWidth = sigBits;
    explicitIntegerPart = explicitIntPart;
    minExponent = -(1 << (exponentBitWidth - 1)) + 2;
  }
};

IEEESemantics getFPSemantics(BuiltinFloatType *fpType) {
  switch (fpType->getFPKind()) {
  case BuiltinFloatType::IEEE32:
    return IEEESemantics(32, 8, 23, false);
  case BuiltinFloatType::IEEE64:
    return IEEESemantics(64, 11, 52, false);
  case BuiltinFloatType::IEEE80:
    return IEEESemantics(80, 15, 63, true);
  default:
    llvm_unreachable("Unexpected semantics");
  }
}

/// This function, given the exponent and significand of a binary fraction
/// equalling 1.srcSignificand x 2^srcExponent,
/// determines whether converting the value to a given destination semantics
/// results in an underflow and whether the significand precision is reduced
/// because of the underflow.
bool isLossyUnderflow(int srcExponent, uint64_t srcSignificand,
                      IEEESemantics srcSem, IEEESemantics destSem) {
  if (srcExponent >= destSem.minExponent)
    return false;

  // Is the value smaller than the smallest non-zero value of destSem?
  if (srcExponent < destSem.minExponent - destSem.significandBitWidth)
    return true;

  // Truncate the significand to the significand width of destSem.
  uint8_t bitWidthDecrease =
      srcSem.significandBitWidth - destSem.significandBitWidth;
  uint64_t truncSignificand = bitWidthDecrease > 0
                                  ? (srcSignificand >> bitWidthDecrease)
                                  : srcSignificand;

  // Compute the significand bits lost due to subnormal form. Note that the
  // integer part: 1 will use up a significand bit in denormal form.
  unsigned additionalLoss = destSem.minExponent - srcExponent + 1;

  // Check whether a set LSB is lost due to subnormal representation.
  unsigned lostLSBBitMask = (1 << additionalLoss) - 1;
  return (truncSignificand & lostLSBBitMask);
}

/// This function, given an IEEE floating-point value (srcVal), determines
/// whether the conversion to a given destination semantics results
/// in an underflow and whether the significand precision is reduced
/// because of the underflow.
bool isLossyUnderflow(APFloat srcVal, BuiltinFloatType *srcType,
                      BuiltinFloatType *destType) {
  if (srcVal.isNaN() || srcVal.isZero() || srcVal.isInfinity())
    return false;

  IEEESemantics srcSem = getFPSemantics(srcType);
  IEEESemantics destSem = getFPSemantics(destType);

  if (srcSem.bitWidth <= destSem.bitWidth)
    return false;

  if (srcVal.isDenormal()) {
    // A denormal value of a larger IEEE FP type will definitely
    // reduce to zero when truncated to smaller IEEE FP type.
    return true;
  }

  APInt bitPattern = srcVal.bitcastToAPInt();
  uint64_t significand =
      bitPattern.getLoBits(srcSem.significandBitWidth).getZExtValue();
  return isLossyUnderflow(ilogb(srcVal), significand, srcSem, destSem);
}

/// This function determines whether the float literal in the given
/// SIL instruction is specified using hex-float notation in the Swift source.
bool isHexLiteralInSource(FloatLiteralInst *flitInst) {
  Expr *expr = flitInst->getLoc().getAsASTNode<Expr>();
  if (!expr)
    return false;

  // Iterate through a sequence of folded implicit constructors if any, and
  // try to extract the FloatLiteralExpr.
  while (auto *callExpr = dyn_cast<CallExpr>(expr)) {
    if (!callExpr->isImplicit() || callExpr->getNumArguments() != 1 ||
        !dyn_cast<ConstructorRefCallExpr>(callExpr->getFn()))
      break;

    auto *tupleExpr = dyn_cast<TupleExpr>(callExpr->getArg());
    if (!tupleExpr)
      break;

    expr = tupleExpr->getElement(0);
  }
  auto *flitExpr = dyn_cast<FloatLiteralExpr>(expr);
  if (!flitExpr)
    return false;
  return flitExpr->getDigitsText().startswith("0x");
}

bool maybeExplicitFPCons(BuiltinInst *BI, const BuiltinInfo &Builtin) {
  assert(Builtin.ID == BuiltinValueKind::FPTrunc ||
         Builtin.ID == BuiltinValueKind::IntToFPWithOverflow);

  auto *callExpr = BI->getLoc().getAsASTNode<CallExpr>();
  if (!callExpr || !dyn_cast<ConstructorRefCallExpr>(callExpr->getFn()))
    return true; // not enough information here, so err on the safer side.

  if (!callExpr->isImplicit())
    return true;

  // Here, the 'callExpr' is an implicit FP construction. However, if it is
  // constructing a Double it could be a part of an explicit construction of
  // another FP type, which uses an implicit conversion to Double as an
  // intermediate step. So we conservatively assume that an implicit
  // construction of Double could be a part of an explicit conversion
  // and suppress the warning.
  auto &astCtx = BI->getModule().getASTContext();
  auto *typeDecl = callExpr->getType()->getCanonicalType().getAnyNominal();
  return (typeDecl && typeDecl == astCtx.getDoubleDecl());
}

static SILValue foldFPTrunc(BuiltinInst *BI, const BuiltinInfo &Builtin,
                            Optional<bool> &ResultsInError) {

  assert(Builtin.ID == BuiltinValueKind::FPTrunc);

  auto *flitInst = dyn_cast<FloatLiteralInst>(BI->getArguments()[0]);
  if (!flitInst)
    return nullptr; // We can fold only compile-time constant arguments.

  SILLocation Loc = BI->getLoc();
  auto *srcType = Builtin.Types[0]->castTo<BuiltinFloatType>();
  auto *destType = Builtin.Types[1]->castTo<BuiltinFloatType>();
  bool losesInfo;
  APFloat truncVal = flitInst->getValue();
  APFloat::opStatus opStatus =
      truncVal.convert(destType->getAPFloatSemantics(),
                       APFloat::rmNearestTiesToEven, &losesInfo);

  // Emit a warning if one of the following conditions hold: (a) the source
  // value overflows the destination type, or (b) the source value is tiny and
  // the tininess results in additional loss of precision when converted to the
  // destination type beyond what would result in the normal scenario, or
  // (c) the source value is a hex-float literal that cannot be precisely
  // represented in the destination type.
  // Suppress all warnings if the conversion is made through an explicit
  // constructor invocation.
  if (ResultsInError.hasValue() && !maybeExplicitFPCons(BI, Builtin)) {
    bool overflow = opStatus & APFloat::opStatus::opOverflow;
    bool tinynInexact =
        isLossyUnderflow(flitInst->getValue(), srcType, destType);
    bool hexnInexact =
        (opStatus != APFloat::opStatus::opOK) && isHexLiteralInSource(flitInst);

    if (overflow || tinynInexact || hexnInexact) {
      SILModule &M = BI->getModule();
      const ApplyExpr *CE = Loc.getAsASTNode<ApplyExpr>();

      SmallString<10> fplitStr;
      tryExtractLiteralText(flitInst, fplitStr);

      auto userType = CE ? CE->getType() : destType;
      auto diagId = overflow
                        ? diag::warning_float_trunc_overflow
                        : (hexnInexact ? diag::warning_float_trunc_hex_inexact
                                       : diag::warning_float_trunc_underflow);
      diagnose(M.getASTContext(), Loc.getSourceLoc(), diagId, fplitStr,
               userType, truncVal.isNegative());

      ResultsInError = Optional<bool>(true);
    }
  }
  // Abort folding if we have subnormality, NaN or opInvalid status.
  if ((opStatus & APFloat::opStatus::opInvalidOp) ||
      (opStatus & APFloat::opStatus::opDivByZero) ||
      (opStatus & APFloat::opStatus::opUnderflow) || truncVal.isDenormal()) {
    return nullptr;
  }
  // Allow folding if there is no loss, overflow or normal imprecision
  // (i.e., opOverflow, opOk, or opInexact).
  SILBuilderWithScope B(BI);
  return B.createFloatLiteral(Loc, BI->getType(), truncVal);
}

static SILValue constantFoldBuiltin(BuiltinInst *BI,
                                    Optional<bool> &ResultsInError) {
  const IntrinsicInfo &Intrinsic = BI->getIntrinsicInfo();
  SILModule &M = BI->getModule();

  // If it's an llvm intrinsic, fold the intrinsic.
  if (Intrinsic.ID != llvm::Intrinsic::not_intrinsic)
    return constantFoldIntrinsic(BI, Intrinsic.ID, ResultsInError);

  // Otherwise, it should be one of the builtin functions.
  OperandValueArrayRef Args = BI->getArguments();
  const BuiltinInfo &Builtin = BI->getBuiltinInfo();

  switch (Builtin.ID) {
  default: break;

// Check and fold binary arithmetic with overflow.
#define BUILTIN(id, name, Attrs)
#define BUILTIN_BINARY_OPERATION_WITH_OVERFLOW(id, name, _, attrs, overload) \
  case BuiltinValueKind::id:
#include "swift/AST/Builtins.def"
    return constantFoldBinaryWithOverflow(BI, Builtin.ID, ResultsInError);

#define BUILTIN(id, name, Attrs)
#define BUILTIN_BINARY_OPERATION(id, name, attrs, overload) \
case BuiltinValueKind::id:
#include "swift/AST/Builtins.def"
      return constantFoldBinary(BI, Builtin.ID, ResultsInError);

// Fold comparison predicates.
#define BUILTIN(id, name, Attrs)
#define BUILTIN_BINARY_PREDICATE(id, name, attrs, overload) \
case BuiltinValueKind::id:
#include "swift/AST/Builtins.def"
      return constantFoldCompare(BI, Builtin.ID);

  case BuiltinValueKind::Trunc:
  case BuiltinValueKind::ZExt:
  case BuiltinValueKind::SExt:
  case BuiltinValueKind::TruncOrBitCast:
  case BuiltinValueKind::ZExtOrBitCast:
  case BuiltinValueKind::SExtOrBitCast: {

    // We can fold if the value being cast is a constant.
    auto *V = dyn_cast<IntegerLiteralInst>(Args[0]);
    if (!V)
      return nullptr;

    APInt CastResV = constantFoldCast(V->getValue(), Builtin);

    // Add the literal instruction to represent the result of the cast.
    SILBuilderWithScope B(BI);
    return B.createIntegerLiteral(BI->getLoc(), BI->getType(), CastResV);
  }

  // Process special builtins that are designed to check for overflows in
  // integer conversions.
  case BuiltinValueKind::SToSCheckedTrunc:
  case BuiltinValueKind::UToUCheckedTrunc:
  case BuiltinValueKind::SToUCheckedTrunc:
  case BuiltinValueKind::UToSCheckedTrunc:
  case BuiltinValueKind::SUCheckedConversion:
  case BuiltinValueKind::USCheckedConversion: {
    return constantFoldAndCheckIntegerConversions(BI, Builtin, ResultsInError);
  }

  case BuiltinValueKind::IntToFPWithOverflow: {
    // Get the value. It should be a constant in most cases.
    // Note, this will not always be a constant, for example, when analyzing
    // _convertFromBuiltinIntegerLiteral function itself.
    auto *V = dyn_cast<IntegerLiteralInst>(Args[0]);
    if (!V)
      return nullptr;
    APInt SrcVal = V->getValue();
    auto *DestTy = Builtin.Types[1]->castTo<BuiltinFloatType>();

    APFloat TruncVal(DestTy->getAPFloatSemantics());
    APFloat::opStatus ConversionStatus = TruncVal.convertFromAPInt(
        SrcVal, /*IsSigned=*/true, APFloat::rmNearestTiesToEven);

    SILLocation Loc = BI->getLoc();
    const ApplyExpr *CE = Loc.getAsASTNode<ApplyExpr>();

    bool overflow = ConversionStatus & APFloat::opOverflow;
    bool inexact = ConversionStatus & APFloat::opInexact;

    if (overflow || inexact) {
      // Check if diagnostics is enabled. If so, make sure to suppress
      // warnings for conversions through explicit initializers,
      // but do not suppress errors.
      if (ResultsInError.hasValue() &&
          (overflow || !maybeExplicitFPCons(BI, Builtin))) {
        SmallString<10> SrcAsString;
        SrcVal.toString(SrcAsString, /*radix*/ 10, true /*isSigned*/);

        if (overflow) {
          diagnose(M.getASTContext(), Loc.getSourceLoc(),
                   diag::integer_literal_overflow, CE ? CE->getType() : DestTy,
                   SrcAsString);
        } else {
          SmallString<10> destStr;
          unsigned srcBitWidth = SrcVal.getBitWidth();
          // Display the 'TruncVal' like an integer in order to make the
          // imprecision due to floating-point representation obvious.
          TruncVal.toString(destStr, srcBitWidth, srcBitWidth);
          diagnose(M.getASTContext(), Loc.getSourceLoc(),
                   diag::warning_int_to_fp_inexact, CE ? CE->getType() : DestTy,
                   SrcAsString, destStr);
        }
        ResultsInError = Optional<bool>(true);
      }
      // If there is an overflow, just return nullptr as this is undefined
      // behavior. Otherwise, continue folding as in the normal workflow.
      if (overflow)
        return nullptr;
    }

    // The call to the builtin should be replaced with the constant value.
    SILBuilderWithScope B(BI);
    return B.createFloatLiteral(Loc, BI->getType(), TruncVal);
  }

  case BuiltinValueKind::FPTrunc: {
    return foldFPTrunc(BI, Builtin, ResultsInError);
  }

  // Conversions from floating point to integer,
  case BuiltinValueKind::FPToSI:
  case BuiltinValueKind::FPToUI: {
    return foldFPToIntConversion(BI, Builtin, ResultsInError);
  }

  case BuiltinValueKind::AssumeNonNegative: {
    auto *V = dyn_cast<IntegerLiteralInst>(Args[0]);
    if (!V)
      return nullptr;

    APInt VInt = V->getValue();
    if (VInt.isNegative() && ResultsInError.hasValue()) {
      diagnose(M.getASTContext(), BI->getLoc().getSourceLoc(),
               diag::wrong_non_negative_assumption,
               VInt.toString(/*Radix*/ 10, /*Signed*/ true));
      ResultsInError = Optional<bool>(true);
    }
    return V;
  }
  }
  return nullptr;
}

static SILValue constantFoldInstruction(SILInstruction &I,
                                        Optional<bool> &ResultsInError) {
  // Constant fold function calls.
  if (auto *BI = dyn_cast<BuiltinInst>(&I)) {
    return constantFoldBuiltin(BI, ResultsInError);
  }

  // Constant fold extraction of a constant element.
  if (auto *TEI = dyn_cast<TupleExtractInst>(&I)) {
    if (auto *TheTuple = dyn_cast<TupleInst>(TEI->getOperand()))
      return TheTuple->getElement(TEI->getFieldNo());
  }

  // Constant fold extraction of a constant struct element.
  if (auto *SEI = dyn_cast<StructExtractInst>(&I)) {
    if (auto *Struct = dyn_cast<StructInst>(SEI->getOperand()))
      return Struct->getOperandForField(SEI->getField())->get();
  }

  // Constant fold indexing insts of a 0 integer literal.
  if (auto *II = dyn_cast<IndexingInst>(&I))
    if (auto *IntLiteral = dyn_cast<IntegerLiteralInst>(II->getIndex()))
      if (!IntLiteral->getValue())
        return II->getBase();

  return SILValue();
}

static bool isApplyOfBuiltin(SILInstruction &I, BuiltinValueKind kind) {
  if (auto *BI = dyn_cast<BuiltinInst>(&I))
    if (BI->getBuiltinInfo().ID == kind)
      return true;
  return false;
}

static bool isApplyOfStringConcat(SILInstruction &I) {
  if (auto *AI = dyn_cast<ApplyInst>(&I))
    if (auto *Fn = AI->getReferencedFunction())
      if (Fn->hasSemanticsAttr("string.concat"))
        return true;
  return false;
}

static bool isFoldable(SILInstruction *I) {
  return isa<IntegerLiteralInst>(I) || isa<FloatLiteralInst>(I);
}

bool ConstantFolder::constantFoldStringConcatenation(ApplyInst *AI) {
  SILBuilder B(AI);
  // Try to apply the string literal concatenation optimization.
  auto *Concatenated = tryToConcatenateStrings(AI, B);
  // Bail if string literal concatenation could not be performed.
  if (!Concatenated)
    return false;

  // Replace all uses of the old instruction by a new instruction.
  AI->replaceAllUsesWith(Concatenated);

  auto RemoveCallback = [&](SILInstruction *DeadI) { WorkList.remove(DeadI); };
  // Remove operands that are not used anymore.
  // Even if they are apply_inst, it is safe to
  // do so, because they can only be applies
  // of functions annotated as string.utf16
  // or string.utf16.
  for (auto &Op : AI->getAllOperands()) {
    SILValue Val = Op.get();
    Op.drop();
    if (Val->use_empty()) {
      auto *DeadI = Val->getDefiningInstruction();
      assert(DeadI);
      recursivelyDeleteTriviallyDeadInstructions(DeadI, /*force*/ true,
                                                 RemoveCallback);
      WorkList.remove(DeadI);
    }
  }
  // Schedule users of the new instruction for constant folding.
  // We only need to schedule the string.concat invocations.
  for (auto AIUse : Concatenated->getUses()) {
    if (isApplyOfStringConcat(*AIUse->getUser())) {
      WorkList.insert(AIUse->getUser());
    }
  }
  // Delete the old apply instruction.
  recursivelyDeleteTriviallyDeadInstructions(AI, /*force*/ true,
                                             RemoveCallback);
  return true;
}

/// Initialize the worklist to all of the constant instructions.
void ConstantFolder::initializeWorklist(SILFunction &F) {
  for (auto &BB : F) {
    for (auto &I : BB) {
      // If `I` is a floating-point literal instruction where the literal is
      // inf, it means the input has a literal that overflows even
      // MaxBuiltinFloatType. Diagnose this error, but allow this instruction
      // to be folded, if needed.
      if (auto floatLit = dyn_cast<FloatLiteralInst>(&I)) {
        APFloat fpVal = floatLit->getValue();
        if (EnableDiagnostics && fpVal.isInfinity()) {
          SmallString<10> litStr;
          tryExtractLiteralText(floatLit, litStr);
          diagnose(I.getModule().getASTContext(), I.getLoc().getSourceLoc(),
                   diag::warning_float_overflows_maxbuiltin, litStr,
                   fpVal.isNegative());
        }
      }

      if (isFoldable(&I) && I.hasUsesOfAnyResult()) {
        WorkList.insert(&I);
        continue;
      }

      // Should we replace calls to assert_configuration by the assert
      // configuration.
      if (AssertConfiguration != SILOptions::DisableReplacement &&
          (isApplyOfBuiltin(I, BuiltinValueKind::AssertConf) ||
           isApplyOfBuiltin(I, BuiltinValueKind::CondUnreachable))) {
        WorkList.insert(&I);
        continue;
      }

      if (isa<CheckedCastBranchInst>(&I) ||
          isa<CheckedCastAddrBranchInst>(&I) ||
          isa<UnconditionalCheckedCastInst>(&I) ||
          isa<UnconditionalCheckedCastAddrInst>(&I)) {
        WorkList.insert(&I);
        continue;
      }

      if (!isApplyOfStringConcat(I)) {
        continue;
      }
      WorkList.insert(&I);
    }
  }
}

SILAnalysis::InvalidationKind
ConstantFolder::processWorkList() {
  LLVM_DEBUG(llvm::dbgs() << "*** ConstPropagation processing: \n");

  // This is the list of traits that this transformation might preserve.
  bool InvalidateBranches = false;
  bool InvalidateCalls = false;
  bool InvalidateInstructions = false;

  // The list of instructions whose evaluation resulted in error or warning.
  // This is used to avoid duplicate error reporting in case we reach the same
  // instruction from different entry points in the WorkList.
  llvm::DenseSet<SILInstruction *> ErrorSet;

  llvm::SetVector<SILInstruction *> FoldedUsers;
  CastOptimizer CastOpt(
      [&](SingleValueInstruction *I, ValueBase *V) { /* ReplaceInstUsesAction */

        InvalidateInstructions = true;
        I->replaceAllUsesWith(V);
      },
      [&](SILInstruction *I) { /* EraseAction */
        auto *TI = dyn_cast<TermInst>(I);

        if (TI) {
          // Invalidate analysis information related to branches. Replacing
          // unconditional_check_branch type instructions by a trap will also
          // invalidate branches/the CFG.
          InvalidateBranches = true;
        }

        InvalidateInstructions = true;

        WorkList.remove(I);
        I->eraseFromParent();
      });

  while (!WorkList.empty()) {
    SILInstruction *I = WorkList.pop_back_val();
    assert(I->getParent() && "SILInstruction must have parent.");

    LLVM_DEBUG(llvm::dbgs() << "Visiting: " << *I);

    Callback(I);

    // Replace assert_configuration instructions by their constant value. We
    // want them to be replace even if we can't fully propagate the constant.
    if (AssertConfiguration != SILOptions::DisableReplacement)
      if (auto *BI = dyn_cast<BuiltinInst>(I)) {
        if (isApplyOfBuiltin(*BI, BuiltinValueKind::AssertConf)) {
          // Instantiate the constant.
          SILBuilderWithScope B(BI);
          auto AssertConfInt = B.createIntegerLiteral(
            BI->getLoc(), BI->getType(), AssertConfiguration);
          BI->replaceAllUsesWith(AssertConfInt);
          // Schedule users for constant folding.
          WorkList.insert(AssertConfInt);
          // Delete the call.
          recursivelyDeleteTriviallyDeadInstructions(BI);

          InvalidateInstructions = true;
          continue;
        }

        // Kill calls to conditionallyUnreachable if we've folded assert
        // configuration calls.
        if (isApplyOfBuiltin(*BI, BuiltinValueKind::CondUnreachable)) {
          assert(BI->use_empty() && "use of conditionallyUnreachable?!");
          recursivelyDeleteTriviallyDeadInstructions(BI, /*force*/ true);
          InvalidateInstructions = true;
          continue;
        }
      }

    if (auto *AI = dyn_cast<ApplyInst>(I)) {
      // Apply may only come from a string.concat invocation.
      if (constantFoldStringConcatenation(AI)) {
        // Invalidate all analysis that's related to the call graph.
        InvalidateInstructions = true;
      }

      continue;
    }

    if (isa<CheckedCastBranchInst>(I) || isa<CheckedCastAddrBranchInst>(I) ||
        isa<UnconditionalCheckedCastInst>(I) ||
        isa<UnconditionalCheckedCastAddrInst>(I)) {
      // Try to perform cast optimizations. Invalidation is handled by a
      // callback inside the cast optimizer.
      SILInstruction *Result = nullptr;
      switch(I->getKind()) {
      default:
        llvm_unreachable("Unexpected instruction for cast optimizations");
      case SILInstructionKind::CheckedCastBranchInst:
        Result = CastOpt.simplifyCheckedCastBranchInst(cast<CheckedCastBranchInst>(I));
        break;
      case SILInstructionKind::CheckedCastAddrBranchInst:
        Result = CastOpt.simplifyCheckedCastAddrBranchInst(cast<CheckedCastAddrBranchInst>(I));
        break;
      case SILInstructionKind::UnconditionalCheckedCastInst: {
        auto Value =
          CastOpt.optimizeUnconditionalCheckedCastInst(cast<UnconditionalCheckedCastInst>(I));
        if (Value) Result = Value->getDefiningInstruction();
        break;
      }
      case SILInstructionKind::UnconditionalCheckedCastAddrInst:
        Result = CastOpt.optimizeUnconditionalCheckedCastAddrInst(cast<UnconditionalCheckedCastAddrInst>(I));
        break;
      }

      if (Result) {
        if (isa<CheckedCastBranchInst>(Result) ||
            isa<CheckedCastAddrBranchInst>(Result) ||
            isa<UnconditionalCheckedCastInst>(Result) ||
            isa<UnconditionalCheckedCastAddrInst>(Result))
          WorkList.insert(Result);
      }
      continue;
    }


    // Go through all users of the constant and try to fold them.
    // TODO: MultiValueInstruction
    FoldedUsers.clear();
    for (auto Use : cast<SingleValueInstruction>(I)->getUses()) {
      SILInstruction *User = Use->getUser();
      LLVM_DEBUG(llvm::dbgs() << "    User: " << *User);

      // It is possible that we had processed this user already. Do not try
      // to fold it again if we had previously produced an error while folding
      // it.  It is not always possible to fold an instruction in case of error.
      if (ErrorSet.count(User))
        continue;

      // Some constant users may indirectly cause folding of their users.
      if (isa<StructInst>(User) || isa<TupleInst>(User)) {
        WorkList.insert(User);
        continue;
      }

      // Always consider cond_fail instructions as potential for DCE.  If the
      // expression feeding them is false, they are dead.  We can't handle this
      // as part of the constant folding logic, because there is no value
      // they can produce (other than empty tuple, which is wasteful).
      if (isa<CondFailInst>(User))
        FoldedUsers.insert(User);

      // Initialize ResultsInError as a None optional.
      //
      // We are essentially using this optional to represent 3 states: true,
      // false, and n/a.
      Optional<bool> ResultsInError;

      // If we are asked to emit diagnostics, override ResultsInError with a
      // Some optional initialized to false.
      if (EnableDiagnostics)
        ResultsInError = false;

      // Try to fold the user. If ResultsInError is None, we do not emit any
      // diagnostics. If ResultsInError is some, we use it as our return value.
      SILValue C = constantFoldInstruction(*User, ResultsInError);

      // If we did not pass in a None and the optional is set to true, add the
      // user to our error set.
      if (ResultsInError.hasValue() && ResultsInError.getValue())
        ErrorSet.insert(User);

      // We failed to constant propagate... continue...
      if (!C)
        continue;

      // We can currently only do this constant-folding of single-value
      // instructions.
      auto UserV = cast<SingleValueInstruction>(User);

      // Ok, we have succeeded. Add user to the FoldedUsers list and perform the
      // necessary cleanups, RAUWs, etc.
      FoldedUsers.insert(User);
      ++NumInstFolded;

      InvalidateInstructions = true;

      // If the constant produced a tuple, be smarter than RAUW: explicitly nuke
      // any tuple_extract instructions using the apply.  This is a common case
      // for functions returning multiple values.
      if (auto *TI = dyn_cast<TupleInst>(C)) {
        for (auto UI = UserV->use_begin(), E = UserV->use_end(); UI != E;) {
          Operand *O = *UI++;

          // If the user is a tuple_extract, just substitute the right value in.
          if (auto *TEI = dyn_cast<TupleExtractInst>(O->getUser())) {
            SILValue NewVal = TI->getOperand(TEI->getFieldNo());
            TEI->replaceAllUsesWith(NewVal);
            TEI->dropAllReferences();
            FoldedUsers.insert(TEI);
            if (auto *Inst = NewVal->getDefiningInstruction())
              WorkList.insert(Inst);
          }
        }

        if (UserV->use_empty())
          FoldedUsers.insert(TI);
      }


      // We were able to fold, so all users should use the new folded value.
      UserV->replaceAllUsesWith(C);

      // The new constant could be further folded now, add it to the worklist.
      if (auto *Inst = C->getDefiningInstruction())
        if (isa<SingleValueInstruction>(Inst))
          WorkList.insert(Inst);
    }

    // Eagerly DCE. We do this after visiting all users to ensure we don't
    // invalidate the uses iterator.
    ArrayRef<SILInstruction *> UserArray = FoldedUsers.getArrayRef();
    if (!UserArray.empty()) {
      InvalidateInstructions = true;
    }

    recursivelyDeleteTriviallyDeadInstructions(UserArray, false,
                                               [&](SILInstruction *DeadI) {
                                                 WorkList.remove(DeadI);
                                               });
  }

  // TODO: refactor this code outside of the method. Passes should not merge
  // invalidation kinds themselves.
  using InvalidationKind = SILAnalysis::InvalidationKind;

  unsigned Inv = InvalidationKind::Nothing;
  if (InvalidateInstructions) Inv |= (unsigned) InvalidationKind::Instructions;
  if (InvalidateCalls)        Inv |= (unsigned) InvalidationKind::Calls;
  if (InvalidateBranches)     Inv |= (unsigned) InvalidationKind::Branches;
  return InvalidationKind(Inv);
}


