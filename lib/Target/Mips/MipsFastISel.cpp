//===-- MipsastISel.cpp - Mips FastISel implementation
//---------------------===//

#include "MipsCCState.h"
#include "MipsInstrInfo.h"
#include "MipsISelLowering.h"
#include "MipsMachineFunction.h"
#include "MipsRegisterInfo.h"
#include "MipsSubtarget.h"
#include "MipsTargetMachine.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/CodeGen/FastISel.h"
#include "llvm/CodeGen/FunctionLoweringInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Target/TargetInstrInfo.h"

using namespace llvm;

namespace {

class MipsFastISel final : public FastISel {

  // All possible address modes.
  class Address {
  public:
    typedef enum { RegBase, FrameIndexBase } BaseKind;

  private:
    BaseKind Kind;
    union {
      unsigned Reg;
      int FI;
    } Base;

    int64_t Offset;

    const GlobalValue *GV;

  public:
    // Innocuous defaults for our address.
    Address() : Kind(RegBase), Offset(0), GV(0) { Base.Reg = 0; }
    void setKind(BaseKind K) { Kind = K; }
    BaseKind getKind() const { return Kind; }
    bool isRegBase() const { return Kind == RegBase; }
    bool isFIBase() const { return Kind == FrameIndexBase; }
    void setReg(unsigned Reg) {
      assert(isRegBase() && "Invalid base register access!");
      Base.Reg = Reg;
    }
    unsigned getReg() const {
      assert(isRegBase() && "Invalid base register access!");
      return Base.Reg;
    }
    void setFI(unsigned FI) {
      assert(isFIBase() && "Invalid base frame index access!");
      Base.FI = FI;
    }
    unsigned getFI() const {
      assert(isFIBase() && "Invalid base frame index access!");
      return Base.FI;
    }

    void setOffset(int64_t Offset_) { Offset = Offset_; }
    int64_t getOffset() const { return Offset; }
    void setGlobalValue(const GlobalValue *G) { GV = G; }
    const GlobalValue *getGlobalValue() { return GV; }
  };

  /// Subtarget - Keep a pointer to the MipsSubtarget around so that we can
  /// make the right decision when generating code for different targets.
  const TargetMachine &TM;
  const MipsSubtarget *Subtarget;
  const TargetInstrInfo &TII;
  const TargetLowering &TLI;
  MipsFunctionInfo *MFI;

  // Convenience variables to avoid some queries.
  LLVMContext *Context;

  bool fastLowerCall(CallLoweringInfo &CLI) override;
  bool fastLowerIntrinsicCall(const IntrinsicInst *II) override;

  bool TargetSupported;
  bool UnsupportedFPMode; // To allow fast-isel to proceed and just not handle
  // floating point but not reject doing fast-isel in other
  // situations

private:
  // Selection routines.
  bool selectLogicalOp(const Instruction *I);
  bool selectLoad(const Instruction *I);
  bool selectStore(const Instruction *I);
  bool selectBranch(const Instruction *I);
  bool selectSelect(const Instruction *I);
  bool selectCmp(const Instruction *I);
  bool selectFPExt(const Instruction *I);
  bool selectFPTrunc(const Instruction *I);
  bool selectFPToInt(const Instruction *I, bool IsSigned);
  bool selectRet(const Instruction *I);
  bool selectTrunc(const Instruction *I);
  bool selectIntExt(const Instruction *I);
  bool selectShift(const Instruction *I);
  bool selectDivRem(const Instruction *I, unsigned ISDOpcode);

  // Utility helper routines.
  bool isTypeLegal(Type *Ty, MVT &VT);
  bool isTypeSupported(Type *Ty, MVT &VT);
  bool isLoadTypeLegal(Type *Ty, MVT &VT);
  bool computeAddress(const Value *Obj, Address &Addr);
  bool computeCallAddress(const Value *V, Address &Addr);
  void simplifyAddress(Address &Addr);

  // Emit helper routines.
  bool emitCmp(unsigned DestReg, const CmpInst *CI);
  bool emitLoad(MVT VT, unsigned &ResultReg, Address &Addr,
                unsigned Alignment = 0);
  bool emitStore(MVT VT, unsigned SrcReg, Address Addr,
                 MachineMemOperand *MMO = nullptr);
  bool emitStore(MVT VT, unsigned SrcReg, Address &Addr,
                 unsigned Alignment = 0);
  unsigned emitIntExt(MVT SrcVT, unsigned SrcReg, MVT DestVT, bool isZExt);
  bool emitIntExt(MVT SrcVT, unsigned SrcReg, MVT DestVT, unsigned DestReg,

                  bool IsZExt);
  bool emitIntZExt(MVT SrcVT, unsigned SrcReg, MVT DestVT, unsigned DestReg);

  bool emitIntSExt(MVT SrcVT, unsigned SrcReg, MVT DestVT, unsigned DestReg);
  bool emitIntSExt32r1(MVT SrcVT, unsigned SrcReg, MVT DestVT,
                       unsigned DestReg);
  bool emitIntSExt32r2(MVT SrcVT, unsigned SrcReg, MVT DestVT,
                       unsigned DestReg);

  unsigned getRegEnsuringSimpleIntegerWidening(const Value *, bool IsUnsigned);

  unsigned emitLogicalOp(unsigned ISDOpc, MVT RetVT, const Value *LHS,
                         const Value *RHS);

  unsigned materializeFP(const ConstantFP *CFP, MVT VT);
  unsigned materializeGV(const GlobalValue *GV, MVT VT);
  unsigned materializeInt(const Constant *C, MVT VT);
  unsigned materialize32BitInt(int64_t Imm, const TargetRegisterClass *RC);
  unsigned materializeExternalCallSym(MCSymbol *Syn);

  MachineInstrBuilder emitInst(unsigned Opc) {
    return BuildMI(*FuncInfo.MBB, FuncInfo.InsertPt, DbgLoc, TII.get(Opc));
  }
  MachineInstrBuilder emitInst(unsigned Opc, unsigned DstReg) {
    return BuildMI(*FuncInfo.MBB, FuncInfo.InsertPt, DbgLoc, TII.get(Opc),
                   DstReg);
  }
  MachineInstrBuilder emitInstStore(unsigned Opc, unsigned SrcReg,
                                    unsigned MemReg, int64_t MemOffset) {
    return emitInst(Opc).addReg(SrcReg).addReg(MemReg).addImm(MemOffset);
  }
  MachineInstrBuilder emitInstLoad(unsigned Opc, unsigned DstReg,
                                   unsigned MemReg, int64_t MemOffset) {
    return emitInst(Opc, DstReg).addReg(MemReg).addImm(MemOffset);
  }

  unsigned fastEmitInst_rr(unsigned MachineInstOpcode,
                           const TargetRegisterClass *RC,
                           unsigned Op0, bool Op0IsKill,
                           unsigned Op1, bool Op1IsKill);

  // for some reason, this default is not generated by tablegen
  // so we explicitly generate it here.
  //
  unsigned fastEmitInst_riir(uint64_t inst, const TargetRegisterClass *RC,
                             unsigned Op0, bool Op0IsKill, uint64_t imm1,
                             uint64_t imm2, unsigned Op3, bool Op3IsKill) {
    return 0;
  }

  // Call handling routines.
private:
  CCAssignFn *CCAssignFnForCall(CallingConv::ID CC) const;
  bool processCallArgs(CallLoweringInfo &CLI, SmallVectorImpl<MVT> &ArgVTs,
                       unsigned &NumBytes);
  bool finishCall(CallLoweringInfo &CLI, MVT RetVT, unsigned NumBytes);

public:
  // Backend specific FastISel code.
  explicit MipsFastISel(FunctionLoweringInfo &funcInfo,
                        const TargetLibraryInfo *libInfo)
      : FastISel(funcInfo, libInfo), TM(funcInfo.MF->getTarget()),
        Subtarget(&funcInfo.MF->getSubtarget<MipsSubtarget>()),
        TII(*Subtarget->getInstrInfo()), TLI(*Subtarget->getTargetLowering()) {
    MFI = funcInfo.MF->getInfo<MipsFunctionInfo>();
    Context = &funcInfo.Fn->getContext();
    TargetSupported =
        ((TM.getRelocationModel() == Reloc::PIC_) &&
         ((Subtarget->hasMips32r2() || Subtarget->hasMips32()) &&
          (static_cast<const MipsTargetMachine &>(TM).getABI().IsO32())));
    UnsupportedFPMode = Subtarget->isFP64bit();
  }

  unsigned fastMaterializeAlloca(const AllocaInst *AI) override;
  unsigned fastMaterializeConstant(const Constant *C) override;
  bool fastSelectInstruction(const Instruction *I) override;

#include "MipsGenFastISel.inc"
};
} // end anonymous namespace.

static bool CC_Mips(unsigned ValNo, MVT ValVT, MVT LocVT,
                    CCValAssign::LocInfo LocInfo, ISD::ArgFlagsTy ArgFlags,
                    CCState &State) LLVM_ATTRIBUTE_UNUSED;

static bool CC_MipsO32_FP32(unsigned ValNo, MVT ValVT, MVT LocVT,
                            CCValAssign::LocInfo LocInfo,
                            ISD::ArgFlagsTy ArgFlags, CCState &State) {
  llvm_unreachable("should not be called");
}

static bool CC_MipsO32_FP64(unsigned ValNo, MVT ValVT, MVT LocVT,
                            CCValAssign::LocInfo LocInfo,
                            ISD::ArgFlagsTy ArgFlags, CCState &State) {
  llvm_unreachable("should not be called");
}

#include "MipsGenCallingConv.inc"

CCAssignFn *MipsFastISel::CCAssignFnForCall(CallingConv::ID CC) const {
  return CC_MipsO32;
}

unsigned MipsFastISel::emitLogicalOp(unsigned ISDOpc, MVT RetVT,
                                     const Value *LHS, const Value *RHS) {
  // Canonicalize immediates to the RHS first.
  if (isa<ConstantInt>(LHS) && !isa<ConstantInt>(RHS))
    std::swap(LHS, RHS);

  unsigned Opc;
  switch (ISDOpc) {
    case ISD::AND:
      Opc = Mips::AND;
      break;
    case ISD::OR:
      Opc = Mips::OR;
      break;
    case ISD::XOR:
      Opc = Mips::XOR;
      break;
    default:
    llvm_unreachable("unexpected opcode");
  }

  unsigned LHSReg = getRegForValue(LHS);
  if (!LHSReg)
    return 0;

  unsigned RHSReg;
  if (const auto *C = dyn_cast<ConstantInt>(RHS))
    RHSReg = materializeInt(C, MVT::i32);
  else
    RHSReg = getRegForValue(RHS);
  if (!RHSReg)
    return 0;

  unsigned ResultReg = createResultReg(&Mips::GPR32RegClass);
  if (!ResultReg)
    return 0;

  emitInst(Opc, ResultReg).addReg(LHSReg).addReg(RHSReg);
  return ResultReg;
}

unsigned MipsFastISel::fastMaterializeAlloca(const AllocaInst *AI) {
  if (!TargetSupported)
    return 0;

  assert(TLI.getValueType(DL, AI->getType(), true) == MVT::i32 &&
         "Alloca should always return a pointer.");

  DenseMap<const AllocaInst *, int>::iterator SI =
      FuncInfo.StaticAllocaMap.find(AI);

  if (SI != FuncInfo.StaticAllocaMap.end()) {
    unsigned ResultReg = createResultReg(&Mips::GPR32RegClass);
    BuildMI(*FuncInfo.MBB, FuncInfo.InsertPt, DbgLoc, TII.get(Mips::LEA_ADDiu),
            ResultReg)
        .addFrameIndex(SI->second)
        .addImm(0);
    return ResultReg;
  }

  return 0;
}

unsigned MipsFastISel::materializeInt(const Constant *C, MVT VT) {
  if (VT != MVT::i32 && VT != MVT::i16 && VT != MVT::i8 && VT != MVT::i1)
    return 0;
  const TargetRegisterClass *RC = &Mips::GPR32RegClass;
  const ConstantInt *CI = cast<ConstantInt>(C);
  return materialize32BitInt(CI->getZExtValue(), RC);
}

unsigned MipsFastISel::materialize32BitInt(int64_t Imm,
                                           const TargetRegisterClass *RC) {
  unsigned ResultReg = createResultReg(RC);

  if (isInt<16>(Imm)) {
    unsigned Opc = Mips::ADDiu;
    emitInst(Opc, ResultReg).addReg(Mips::ZERO).addImm(Imm);
    return ResultReg;
  } else if (isUInt<16>(Imm)) {
    emitInst(Mips::ORi, ResultReg).addReg(Mips::ZERO).addImm(Imm);
    return ResultReg;
  }
  unsigned Lo = Imm & 0xFFFF;
  unsigned Hi = (Imm >> 16) & 0xFFFF;
  if (Lo) {
    // Both Lo and Hi have nonzero bits.
    unsigned TmpReg = createResultReg(RC);
    emitInst(Mips::LUi, TmpReg).addImm(Hi);
    emitInst(Mips::ORi, ResultReg).addReg(TmpReg).addImm(Lo);
  } else {
    emitInst(Mips::LUi, ResultReg).addImm(Hi);
  }
  return ResultReg;
}

unsigned MipsFastISel::materializeFP(const ConstantFP *CFP, MVT VT) {
  if (UnsupportedFPMode)
    return 0;
  int64_t Imm = CFP->getValueAPF().bitcastToAPInt().getZExtValue();
  if (VT == MVT::f32) {
    const TargetRegisterClass *RC = &Mips::FGR32RegClass;
    unsigned DestReg = createResultReg(RC);
    unsigned TempReg = materialize32BitInt(Imm, &Mips::GPR32RegClass);
    emitInst(Mips::MTC1, DestReg).addReg(TempReg);
    return DestReg;
  } else if (VT == MVT::f64) {
    const TargetRegisterClass *RC = &Mips::AFGR64RegClass;
    unsigned DestReg = createResultReg(RC);
    unsigned TempReg1 = materialize32BitInt(Imm >> 32, &Mips::GPR32RegClass);
    unsigned TempReg2 =
        materialize32BitInt(Imm & 0xFFFFFFFF, &Mips::GPR32RegClass);
    emitInst(Mips::BuildPairF64, DestReg).addReg(TempReg2).addReg(TempReg1);
    return DestReg;
  }
  return 0;
}

unsigned MipsFastISel::materializeGV(const GlobalValue *GV, MVT VT) {
  // For now 32-bit only.
  if (VT != MVT::i32)
    return 0;
  const TargetRegisterClass *RC = &Mips::GPR32RegClass;
  unsigned DestReg = createResultReg(RC);
  const GlobalVariable *GVar = dyn_cast<GlobalVariable>(GV);
  bool IsThreadLocal = GVar && GVar->isThreadLocal();
  // TLS not supported at this time.
  if (IsThreadLocal)
    return 0;
  emitInst(Mips::LW, DestReg)
      .addReg(MFI->getGlobalBaseReg())
      .addGlobalAddress(GV, 0, MipsII::MO_GOT);
  if ((GV->hasInternalLinkage() ||
       (GV->hasLocalLinkage() && !isa<Function>(GV)))) {
    unsigned TempReg = createResultReg(RC);
    emitInst(Mips::ADDiu, TempReg)
        .addReg(DestReg)
        .addGlobalAddress(GV, 0, MipsII::MO_ABS_LO);
    DestReg = TempReg;
  }
  return DestReg;
}

unsigned MipsFastISel::materializeExternalCallSym(MCSymbol *Sym) {
  const TargetRegisterClass *RC = &Mips::GPR32RegClass;
  unsigned DestReg = createResultReg(RC);
  emitInst(Mips::LW, DestReg)
      .addReg(MFI->getGlobalBaseReg())
      .addSym(Sym, MipsII::MO_GOT);
  return DestReg;
}

// Materialize a constant into a register, and return the register
// number (or zero if we failed to handle it).
unsigned MipsFastISel::fastMaterializeConstant(const Constant *C) {
  if (!TargetSupported)
    return 0;

  EVT CEVT = TLI.getValueType(DL, C->getType(), true);

  // Only handle simple types.
  if (!CEVT.isSimple())
    return 0;
  MVT VT = CEVT.getSimpleVT();

  if (const ConstantFP *CFP = dyn_cast<ConstantFP>(C))
    return (UnsupportedFPMode) ? 0 : materializeFP(CFP, VT);
  else if (const GlobalValue *GV = dyn_cast<GlobalValue>(C))
    return materializeGV(GV, VT);
  else if (isa<ConstantInt>(C))
    return materializeInt(C, VT);

  return 0;
}

bool MipsFastISel::computeAddress(const Value *Obj, Address &Addr) {

  const User *U = nullptr;
  unsigned Opcode = Instruction::UserOp1;
  if (const Instruction *I = dyn_cast<Instruction>(Obj)) {
    // Don't walk into other basic blocks unless the object is an alloca from
    // another block, otherwise it may not have a virtual register assigned.
    if (FuncInfo.StaticAllocaMap.count(static_cast<const AllocaInst *>(Obj)) ||
        FuncInfo.MBBMap[I->getParent()] == FuncInfo.MBB) {
      Opcode = I->getOpcode();
      U = I;
    }
  } else if (const ConstantExpr *C = dyn_cast<ConstantExpr>(Obj)) {
    Opcode = C->getOpcode();
    U = C;
  }
  switch (Opcode) {
  default:
    break;
  case Instruction::BitCast: {
    // Look through bitcasts.
    return computeAddress(U->getOperand(0), Addr);
  }
  case Instruction::GetElementPtr: {
    Address SavedAddr = Addr;
    uint64_t TmpOffset = Addr.getOffset();
    // Iterate through the GEP folding the constants into offsets where
    // we can.
    gep_type_iterator GTI = gep_type_begin(U);
    for (User::const_op_iterator i = U->op_begin() + 1, e = U->op_end(); i != e;
         ++i, ++GTI) {
      const Value *Op = *i;
      if (StructType *STy = dyn_cast<StructType>(*GTI)) {
        const StructLayout *SL = DL.getStructLayout(STy);
        unsigned Idx = cast<ConstantInt>(Op)->getZExtValue();
        TmpOffset += SL->getElementOffset(Idx);
      } else {
        uint64_t S = DL.getTypeAllocSize(GTI.getIndexedType());
        for (;;) {
          if (const ConstantInt *CI = dyn_cast<ConstantInt>(Op)) {
            // Constant-offset addressing.
            TmpOffset += CI->getSExtValue() * S;
            break;
          }
          if (canFoldAddIntoGEP(U, Op)) {
            // A compatible add with a constant operand. Fold the constant.
            ConstantInt *CI =
                cast<ConstantInt>(cast<AddOperator>(Op)->getOperand(1));
            TmpOffset += CI->getSExtValue() * S;
            // Iterate on the other operand.
            Op = cast<AddOperator>(Op)->getOperand(0);
            continue;
          }
          // Unsupported
          goto unsupported_gep;
        }
      }
    }
    // Try to grab the base operand now.
    Addr.setOffset(TmpOffset);
    if (computeAddress(U->getOperand(0), Addr))
      return true;
    // We failed, restore everything and try the other options.
    Addr = SavedAddr;
  unsupported_gep:
    break;
  }
  case Instruction::Alloca: {
    const AllocaInst *AI = cast<AllocaInst>(Obj);
    DenseMap<const AllocaInst *, int>::iterator SI =
        FuncInfo.StaticAllocaMap.find(AI);
    if (SI != FuncInfo.StaticAllocaMap.end()) {
      Addr.setKind(Address::FrameIndexBase);
      Addr.setFI(SI->second);
      return true;
    }
    break;
  }
  }
  Addr.setReg(getRegForValue(Obj));
  return Addr.getReg() != 0;
}

bool MipsFastISel::computeCallAddress(const Value *V, Address &Addr) {
  const User *U = nullptr;
  unsigned Opcode = Instruction::UserOp1;

  if (const auto *I = dyn_cast<Instruction>(V)) {
    // Check if the value is defined in the same basic block. This information
    // is crucial to know whether or not folding an operand is valid.
    if (I->getParent() == FuncInfo.MBB->getBasicBlock()) {
      Opcode = I->getOpcode();
      U = I;
    }
  } else if (const auto *C = dyn_cast<ConstantExpr>(V)) {
    Opcode = C->getOpcode();
    U = C;
  }

  switch (Opcode) {
  default:
    break;
  case Instruction::BitCast:
    // Look past bitcasts if its operand is in the same BB.
      return computeCallAddress(U->getOperand(0), Addr);
    break;
  case Instruction::IntToPtr:
    // Look past no-op inttoptrs if its operand is in the same BB.
    if (TLI.getValueType(DL, U->getOperand(0)->getType()) ==
        TLI.getPointerTy(DL))
      return computeCallAddress(U->getOperand(0), Addr);
    break;
  case Instruction::PtrToInt:
    // Look past no-op ptrtoints if its operand is in the same BB.
    if (TLI.getValueType(DL, U->getType()) == TLI.getPointerTy(DL))
      return computeCallAddress(U->getOperand(0), Addr);
    break;
  }

  if (const GlobalValue *GV = dyn_cast<GlobalValue>(V)) {
    Addr.setGlobalValue(GV);
    return true;
  }

  // If all else fails, try to materialize the value in a register.
  if (!Addr.getGlobalValue()) {
    Addr.setReg(getRegForValue(V));
    return Addr.getReg() != 0;
  }

  return false;
}

bool MipsFastISel::isTypeLegal(Type *Ty, MVT &VT) {
  EVT evt = TLI.getValueType(DL, Ty, true);
  // Only handle simple types.
  if (evt == MVT::Other || !evt.isSimple())
    return false;
  VT = evt.getSimpleVT();

  // Handle all legal types, i.e. a register that will directly hold this
  // value.
  return TLI.isTypeLegal(VT);
}

bool MipsFastISel::isTypeSupported(Type *Ty, MVT &VT) {
  if (Ty->isVectorTy())
    return false;

  if (isTypeLegal(Ty, VT))
    return true;

  // If this is a type than can be sign or zero-extended to a basic operation
  // go ahead and accept it now.
  if (VT == MVT::i1 || VT == MVT::i8 || VT == MVT::i16)
    return true;

  return false;
}

bool MipsFastISel::isLoadTypeLegal(Type *Ty, MVT &VT) {
  if (isTypeLegal(Ty, VT))
    return true;
  // We will extend this in a later patch:
  //   If this is a type than can be sign or zero-extended to a basic operation
  //   go ahead and accept it now.
  if (VT == MVT::i8 || VT == MVT::i16)
    return true;
  return false;
}
// Because of how EmitCmp is called with fast-isel, you can
// end up with redundant "andi" instructions after the sequences emitted below.
// We should try and solve this issue in the future.
//
bool MipsFastISel::emitCmp(unsigned ResultReg, const CmpInst *CI) {
  const Value *Left = CI->getOperand(0), *Right = CI->getOperand(1);
  bool IsUnsigned = CI->isUnsigned();
  unsigned LeftReg = getRegEnsuringSimpleIntegerWidening(Left, IsUnsigned);
  if (LeftReg == 0)
    return false;
  unsigned RightReg = getRegEnsuringSimpleIntegerWidening(Right, IsUnsigned);
  if (RightReg == 0)
    return false;
  CmpInst::Predicate P = CI->getPredicate();

  switch (P) {
  default:
    return false;
  case CmpInst::ICMP_EQ: {
    unsigned TempReg = createResultReg(&Mips::GPR32RegClass);
    emitInst(Mips::XOR, TempReg).addReg(LeftReg).addReg(RightReg);
    emitInst(Mips::SLTiu, ResultReg).addReg(TempReg).addImm(1);
    break;
  }
  case CmpInst::ICMP_NE: {
    unsigned TempReg = createResultReg(&Mips::GPR32RegClass);
    emitInst(Mips::XOR, TempReg).addReg(LeftReg).addReg(RightReg);
    emitInst(Mips::SLTu, ResultReg).addReg(Mips::ZERO).addReg(TempReg);
    break;
  }
  case CmpInst::ICMP_UGT: {
    emitInst(Mips::SLTu, ResultReg).addReg(RightReg).addReg(LeftReg);
    break;
  }
  case CmpInst::ICMP_ULT: {
    emitInst(Mips::SLTu, ResultReg).addReg(LeftReg).addReg(RightReg);
    break;
  }
  case CmpInst::ICMP_UGE: {
    unsigned TempReg = createResultReg(&Mips::GPR32RegClass);
    emitInst(Mips::SLTu, TempReg).addReg(LeftReg).addReg(RightReg);
    emitInst(Mips::XORi, ResultReg).addReg(TempReg).addImm(1);
    break;
  }
  case CmpInst::ICMP_ULE: {
    unsigned TempReg = createResultReg(&Mips::GPR32RegClass);
    emitInst(Mips::SLTu, TempReg).addReg(RightReg).addReg(LeftReg);
    emitInst(Mips::XORi, ResultReg).addReg(TempReg).addImm(1);
    break;
  }
  case CmpInst::ICMP_SGT: {
    emitInst(Mips::SLT, ResultReg).addReg(RightReg).addReg(LeftReg);
    break;
  }
  case CmpInst::ICMP_SLT: {
    emitInst(Mips::SLT, ResultReg).addReg(LeftReg).addReg(RightReg);
    break;
  }
  case CmpInst::ICMP_SGE: {
    unsigned TempReg = createResultReg(&Mips::GPR32RegClass);
    emitInst(Mips::SLT, TempReg).addReg(LeftReg).addReg(RightReg);
    emitInst(Mips::XORi, ResultReg).addReg(TempReg).addImm(1);
    break;
  }
  case CmpInst::ICMP_SLE: {
    unsigned TempReg = createResultReg(&Mips::GPR32RegClass);
    emitInst(Mips::SLT, TempReg).addReg(RightReg).addReg(LeftReg);
    emitInst(Mips::XORi, ResultReg).addReg(TempReg).addImm(1);
    break;
  }
  case CmpInst::FCMP_OEQ:
  case CmpInst::FCMP_UNE:
  case CmpInst::FCMP_OLT:
  case CmpInst::FCMP_OLE:
  case CmpInst::FCMP_OGT:
  case CmpInst::FCMP_OGE: {
    if (UnsupportedFPMode)
      return false;
    bool IsFloat = Left->getType()->isFloatTy();
    bool IsDouble = Left->getType()->isDoubleTy();
    if (!IsFloat && !IsDouble)
      return false;
    unsigned Opc, CondMovOpc;
    switch (P) {
    case CmpInst::FCMP_OEQ:
      Opc = IsFloat ? Mips::C_EQ_S : Mips::C_EQ_D32;
      CondMovOpc = Mips::MOVT_I;
      break;
    case CmpInst::FCMP_UNE:
      Opc = IsFloat ? Mips::C_EQ_S : Mips::C_EQ_D32;
      CondMovOpc = Mips::MOVF_I;
      break;
    case CmpInst::FCMP_OLT:
      Opc = IsFloat ? Mips::C_OLT_S : Mips::C_OLT_D32;
      CondMovOpc = Mips::MOVT_I;
      break;
    case CmpInst::FCMP_OLE:
      Opc = IsFloat ? Mips::C_OLE_S : Mips::C_OLE_D32;
      CondMovOpc = Mips::MOVT_I;
      break;
    case CmpInst::FCMP_OGT:
      Opc = IsFloat ? Mips::C_ULE_S : Mips::C_ULE_D32;
      CondMovOpc = Mips::MOVF_I;
      break;
    case CmpInst::FCMP_OGE:
      Opc = IsFloat ? Mips::C_ULT_S : Mips::C_ULT_D32;
      CondMovOpc = Mips::MOVF_I;
      break;
    default:
      llvm_unreachable("Only switching of a subset of CCs.");
    }
    unsigned RegWithZero = createResultReg(&Mips::GPR32RegClass);
    unsigned RegWithOne = createResultReg(&Mips::GPR32RegClass);
    emitInst(Mips::ADDiu, RegWithZero).addReg(Mips::ZERO).addImm(0);
    emitInst(Mips::ADDiu, RegWithOne).addReg(Mips::ZERO).addImm(1);
    emitInst(Opc).addReg(LeftReg).addReg(RightReg).addReg(
        Mips::FCC0, RegState::ImplicitDefine);
    MachineInstrBuilder MI = emitInst(CondMovOpc, ResultReg)
                                 .addReg(RegWithOne)
                                 .addReg(Mips::FCC0)
                                 .addReg(RegWithZero, RegState::Implicit);
    MI->tieOperands(0, 3);
    break;
  }
  }
  return true;
}
bool MipsFastISel::emitLoad(MVT VT, unsigned &ResultReg, Address &Addr,
                            unsigned Alignment) {
  //
  // more cases will be handled here in following patches.
  //
  unsigned Opc;
  switch (VT.SimpleTy) {
  case MVT::i32: {
    ResultReg = createResultReg(&Mips::GPR32RegClass);
    Opc = Mips::LW;
    break;
  }
  case MVT::i16: {
    ResultReg = createResultReg(&Mips::GPR32RegClass);
    Opc = Mips::LHu;
    break;
  }
  case MVT::i8: {
    ResultReg = createResultReg(&Mips::GPR32RegClass);
    Opc = Mips::LBu;
    break;
  }
  case MVT::f32: {
    if (UnsupportedFPMode)
      return false;
    ResultReg = createResultReg(&Mips::FGR32RegClass);
    Opc = Mips::LWC1;
    break;
  }
  case MVT::f64: {
    if (UnsupportedFPMode)
      return false;
    ResultReg = createResultReg(&Mips::AFGR64RegClass);
    Opc = Mips::LDC1;
    break;
  }
  default:
    return false;
  }
  if (Addr.isRegBase()) {
    simplifyAddress(Addr);
    emitInstLoad(Opc, ResultReg, Addr.getReg(), Addr.getOffset());
    return true;
  }
  if (Addr.isFIBase()) {
    unsigned FI = Addr.getFI();
    unsigned Align = 4;
    unsigned Offset = Addr.getOffset();
    MachineFrameInfo &MFI = *MF->getFrameInfo();
    MachineMemOperand *MMO = MF->getMachineMemOperand(
        MachinePointerInfo::getFixedStack(*MF, FI), MachineMemOperand::MOLoad,
        MFI.getObjectSize(FI), Align);
    BuildMI(*FuncInfo.MBB, FuncInfo.InsertPt, DbgLoc, TII.get(Opc), ResultReg)
        .addFrameIndex(FI)
        .addImm(Offset)
        .addMemOperand(MMO);
    return true;
  }
  return false;
}

bool MipsFastISel::emitStore(MVT VT, unsigned SrcReg, Address &Addr,
                             unsigned Alignment) {
  //
  // more cases will be handled here in following patches.
  //
  unsigned Opc;
  switch (VT.SimpleTy) {
  case MVT::i8:
    Opc = Mips::SB;
    break;
  case MVT::i16:
    Opc = Mips::SH;
    break;
  case MVT::i32:
    Opc = Mips::SW;
    break;
  case MVT::f32:
    if (UnsupportedFPMode)
      return false;
    Opc = Mips::SWC1;
    break;
  case MVT::f64:
    if (UnsupportedFPMode)
      return false;
    Opc = Mips::SDC1;
    break;
  default:
    return false;
  }
  if (Addr.isRegBase()) {
    simplifyAddress(Addr);
    emitInstStore(Opc, SrcReg, Addr.getReg(), Addr.getOffset());
    return true;
  }
  if (Addr.isFIBase()) {
    unsigned FI = Addr.getFI();
    unsigned Align = 4;
    unsigned Offset = Addr.getOffset();
    MachineFrameInfo &MFI = *MF->getFrameInfo();
    MachineMemOperand *MMO = MF->getMachineMemOperand(
        MachinePointerInfo::getFixedStack(*MF, FI), MachineMemOperand::MOLoad,
        MFI.getObjectSize(FI), Align);
    BuildMI(*FuncInfo.MBB, FuncInfo.InsertPt, DbgLoc, TII.get(Opc))
        .addReg(SrcReg)
        .addFrameIndex(FI)
        .addImm(Offset)
        .addMemOperand(MMO);
    return true;
  }
  return false;
}

bool MipsFastISel::selectLogicalOp(const Instruction *I) {
  MVT VT;
  if (!isTypeSupported(I->getType(), VT))
    return false;

  unsigned ResultReg;
  switch (I->getOpcode()) {
  default:
    llvm_unreachable("Unexpected instruction.");
  case Instruction::And:
    ResultReg = emitLogicalOp(ISD::AND, VT, I->getOperand(0), I->getOperand(1));
    break;
  case Instruction::Or:
    ResultReg = emitLogicalOp(ISD::OR, VT, I->getOperand(0), I->getOperand(1));
    break;
  case Instruction::Xor:
    ResultReg = emitLogicalOp(ISD::XOR, VT, I->getOperand(0), I->getOperand(1));
    break;
  }

  if (!ResultReg)
    return false;

  updateValueMap(I, ResultReg);
  return true;
}

bool MipsFastISel::selectLoad(const Instruction *I) {
  // Atomic loads need special handling.
  if (cast<LoadInst>(I)->isAtomic())
    return false;

  // Verify we have a legal type before going any further.
  MVT VT;
  if (!isLoadTypeLegal(I->getType(), VT))
    return false;

  // See if we can handle this address.
  Address Addr;
  if (!computeAddress(I->getOperand(0), Addr))
    return false;

  unsigned ResultReg;
  if (!emitLoad(VT, ResultReg, Addr, cast<LoadInst>(I)->getAlignment()))
    return false;
  updateValueMap(I, ResultReg);
  return true;
}

bool MipsFastISel::selectStore(const Instruction *I) {
  Value *Op0 = I->getOperand(0);
  unsigned SrcReg = 0;

  // Atomic stores need special handling.
  if (cast<StoreInst>(I)->isAtomic())
    return false;

  // Verify we have a legal type before going any further.
  MVT VT;
  if (!isLoadTypeLegal(I->getOperand(0)->getType(), VT))
    return false;

  // Get the value to be stored into a register.
  SrcReg = getRegForValue(Op0);
  if (SrcReg == 0)
    return false;

  // See if we can handle this address.
  Address Addr;
  if (!computeAddress(I->getOperand(1), Addr))
    return false;

  if (!emitStore(VT, SrcReg, Addr, cast<StoreInst>(I)->getAlignment()))
    return false;
  return true;
}

//
// This can cause a redundant sltiu to be generated.
// FIXME: try and eliminate this in a future patch.
//
bool MipsFastISel::selectBranch(const Instruction *I) {
  const BranchInst *BI = cast<BranchInst>(I);
  MachineBasicBlock *BrBB = FuncInfo.MBB;
  //
  // TBB is the basic block for the case where the comparison is true.
  // FBB is the basic block for the case where the comparison is false.
  // if (cond) goto TBB
  // goto FBB
  // TBB:
  //
  MachineBasicBlock *TBB = FuncInfo.MBBMap[BI->getSuccessor(0)];
  MachineBasicBlock *FBB = FuncInfo.MBBMap[BI->getSuccessor(1)];
  BI->getCondition();
  // For now, just try the simplest case where it's fed by a compare.
  if (const CmpInst *CI = dyn_cast<CmpInst>(BI->getCondition())) {
    unsigned CondReg = createResultReg(&Mips::GPR32RegClass);
    if (!emitCmp(CondReg, CI))
      return false;
    BuildMI(*BrBB, FuncInfo.InsertPt, DbgLoc, TII.get(Mips::BGTZ))
        .addReg(CondReg)
        .addMBB(TBB);
    finishCondBranch(BI->getParent(), TBB, FBB);
    return true;
  }
  return false;
}

bool MipsFastISel::selectCmp(const Instruction *I) {
  const CmpInst *CI = cast<CmpInst>(I);
  unsigned ResultReg = createResultReg(&Mips::GPR32RegClass);
  if (!emitCmp(ResultReg, CI))
    return false;
  updateValueMap(I, ResultReg);
  return true;
}

// Attempt to fast-select a floating-point extend instruction.
bool MipsFastISel::selectFPExt(const Instruction *I) {
  if (UnsupportedFPMode)
    return false;
  Value *Src = I->getOperand(0);
  EVT SrcVT = TLI.getValueType(DL, Src->getType(), true);
  EVT DestVT = TLI.getValueType(DL, I->getType(), true);

  if (SrcVT != MVT::f32 || DestVT != MVT::f64)
    return false;

  unsigned SrcReg =
      getRegForValue(Src); // his must be a 32 bit floating point register class
                           // maybe we should handle this differently
  if (!SrcReg)
    return false;

  unsigned DestReg = createResultReg(&Mips::AFGR64RegClass);
  emitInst(Mips::CVT_D32_S, DestReg).addReg(SrcReg);
  updateValueMap(I, DestReg);
  return true;
}

bool MipsFastISel::selectSelect(const Instruction *I) {
  assert(isa<SelectInst>(I) && "Expected a select instruction.");

  MVT VT;
  if (!isTypeSupported(I->getType(), VT))
    return false;

  unsigned CondMovOpc;
  const TargetRegisterClass *RC;

  if (VT.isInteger() && !VT.isVector() && VT.getSizeInBits() <= 32) {
    CondMovOpc = Mips::MOVN_I_I;
    RC = &Mips::GPR32RegClass;
  } else if (VT == MVT::f32) {
    CondMovOpc = Mips::MOVN_I_S;
    RC = &Mips::FGR32RegClass;
  } else if (VT == MVT::f64) {
    CondMovOpc = Mips::MOVN_I_D32;
    RC = &Mips::AFGR64RegClass;
  } else
    return false;

  const SelectInst *SI = cast<SelectInst>(I);
  const Value *Cond = SI->getCondition();
  unsigned Src1Reg = getRegForValue(SI->getTrueValue());
  unsigned Src2Reg = getRegForValue(SI->getFalseValue());
  unsigned CondReg = getRegForValue(Cond);

  if (!Src1Reg || !Src2Reg || !CondReg)
    return false;

  unsigned ZExtCondReg = createResultReg(&Mips::GPR32RegClass);
  if (!ZExtCondReg)
    return false;

  if (!emitIntExt(MVT::i1, CondReg, MVT::i32, ZExtCondReg, true))
    return false;

  unsigned ResultReg = createResultReg(RC);
  unsigned TempReg = createResultReg(RC);

  if (!ResultReg || !TempReg)
    return false;

  emitInst(TargetOpcode::COPY, TempReg).addReg(Src2Reg);
  emitInst(CondMovOpc, ResultReg)
    .addReg(Src1Reg).addReg(ZExtCondReg).addReg(TempReg);
  updateValueMap(I, ResultReg);
  return true;
}

// Attempt to fast-select a floating-point truncate instruction.
bool MipsFastISel::selectFPTrunc(const Instruction *I) {
  if (UnsupportedFPMode)
    return false;
  Value *Src = I->getOperand(0);
  EVT SrcVT = TLI.getValueType(DL, Src->getType(), true);
  EVT DestVT = TLI.getValueType(DL, I->getType(), true);

  if (SrcVT != MVT::f64 || DestVT != MVT::f32)
    return false;

  unsigned SrcReg = getRegForValue(Src);
  if (!SrcReg)
    return false;

  unsigned DestReg = createResultReg(&Mips::FGR32RegClass);
  if (!DestReg)
    return false;

  emitInst(Mips::CVT_S_D32, DestReg).addReg(SrcReg);
  updateValueMap(I, DestReg);
  return true;
}

// Attempt to fast-select a floating-point-to-integer conversion.
bool MipsFastISel::selectFPToInt(const Instruction *I, bool IsSigned) {
  if (UnsupportedFPMode)
    return false;
  MVT DstVT, SrcVT;
  if (!IsSigned)
    return false; // We don't handle this case yet. There is no native
                  // instruction for this but it can be synthesized.
  Type *DstTy = I->getType();
  if (!isTypeLegal(DstTy, DstVT))
    return false;

  if (DstVT != MVT::i32)
    return false;

  Value *Src = I->getOperand(0);
  Type *SrcTy = Src->getType();
  if (!isTypeLegal(SrcTy, SrcVT))
    return false;

  if (SrcVT != MVT::f32 && SrcVT != MVT::f64)
    return false;

  unsigned SrcReg = getRegForValue(Src);
  if (SrcReg == 0)
    return false;

  // Determine the opcode for the conversion, which takes place
  // entirely within FPRs.
  unsigned DestReg = createResultReg(&Mips::GPR32RegClass);
  unsigned TempReg = createResultReg(&Mips::FGR32RegClass);
  unsigned Opc = (SrcVT == MVT::f32) ? Mips::TRUNC_W_S : Mips::TRUNC_W_D32;

  // Generate the convert.
  emitInst(Opc, TempReg).addReg(SrcReg);
  emitInst(Mips::MFC1, DestReg).addReg(TempReg);

  updateValueMap(I, DestReg);
  return true;
}

bool MipsFastISel::processCallArgs(CallLoweringInfo &CLI,
                                   SmallVectorImpl<MVT> &OutVTs,
                                   unsigned &NumBytes) {
  CallingConv::ID CC = CLI.CallConv;
  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CC, false, *FuncInfo.MF, ArgLocs, *Context);
  CCInfo.AnalyzeCallOperands(OutVTs, CLI.OutFlags, CCAssignFnForCall(CC));
  // Get a count of how many bytes are to be pushed on the stack.
  NumBytes = CCInfo.getNextStackOffset();
  // This is the minimum argument area used for A0-A3.
  if (NumBytes < 16)
    NumBytes = 16;

  emitInst(Mips::ADJCALLSTACKDOWN).addImm(16);
  // Process the args.
  MVT firstMVT;
  for (unsigned i = 0, e = ArgLocs.size(); i != e; ++i) {
    CCValAssign &VA = ArgLocs[i];
    const Value *ArgVal = CLI.OutVals[VA.getValNo()];
    MVT ArgVT = OutVTs[VA.getValNo()];

    if (i == 0) {
      firstMVT = ArgVT;
      if (ArgVT == MVT::f32) {
        VA.convertToReg(Mips::F12);
      } else if (ArgVT == MVT::f64) {
        VA.convertToReg(Mips::D6);
      }
    } else if (i == 1) {
      if ((firstMVT == MVT::f32) || (firstMVT == MVT::f64)) {
        if (ArgVT == MVT::f32) {
          VA.convertToReg(Mips::F14);
        } else if (ArgVT == MVT::f64) {
          VA.convertToReg(Mips::D7);
        }
      }
    }
    if (((ArgVT == MVT::i32) || (ArgVT == MVT::f32) || (ArgVT == MVT::i16) ||
         (ArgVT == MVT::i8)) &&
        VA.isMemLoc()) {
      switch (VA.getLocMemOffset()) {
      case 0:
        VA.convertToReg(Mips::A0);
        break;
      case 4:
        VA.convertToReg(Mips::A1);
        break;
      case 8:
        VA.convertToReg(Mips::A2);
        break;
      case 12:
        VA.convertToReg(Mips::A3);
        break;
      default:
        break;
      }
    }
    unsigned ArgReg = getRegForValue(ArgVal);
    if (!ArgReg)
      return false;

    // Handle arg promotion: SExt, ZExt, AExt.
    switch (VA.getLocInfo()) {
    case CCValAssign::Full:
      break;
    case CCValAssign::AExt:
    case CCValAssign::SExt: {
      MVT DestVT = VA.getLocVT();
      MVT SrcVT = ArgVT;
      ArgReg = emitIntExt(SrcVT, ArgReg, DestVT, /*isZExt=*/false);
      if (!ArgReg)
        return false;
      break;
    }
    case CCValAssign::ZExt: {
      MVT DestVT = VA.getLocVT();
      MVT SrcVT = ArgVT;
      ArgReg = emitIntExt(SrcVT, ArgReg, DestVT, /*isZExt=*/true);
      if (!ArgReg)
        return false;
      break;
    }
    default:
      llvm_unreachable("Unknown arg promotion!");
    }

    // Now copy/store arg to correct locations.
    if (VA.isRegLoc() && !VA.needsCustom()) {
      BuildMI(*FuncInfo.MBB, FuncInfo.InsertPt, DbgLoc,
              TII.get(TargetOpcode::COPY), VA.getLocReg()).addReg(ArgReg);
      CLI.OutRegs.push_back(VA.getLocReg());
    } else if (VA.needsCustom()) {
      llvm_unreachable("Mips does not use custom args.");
      return false;
    } else {
      //
      // FIXME: This path will currently return false. It was copied
      // from the AArch64 port and should be essentially fine for Mips too.
      // The work to finish up this path will be done in a follow-on patch.
      //
      assert(VA.isMemLoc() && "Assuming store on stack.");
      // Don't emit stores for undef values.
      if (isa<UndefValue>(ArgVal))
        continue;

      // Need to store on the stack.
      // FIXME: This alignment is incorrect but this path is disabled
      // for now (will return false). We need to determine the right alignment
      // based on the normal alignment for the underlying machine type.
      //
      unsigned ArgSize = RoundUpToAlignment(ArgVT.getSizeInBits(), 4);

      unsigned BEAlign = 0;
      if (ArgSize < 8 && !Subtarget->isLittle())
        BEAlign = 8 - ArgSize;

      Address Addr;
      Addr.setKind(Address::RegBase);
      Addr.setReg(Mips::SP);
      Addr.setOffset(VA.getLocMemOffset() + BEAlign);

      unsigned Alignment = DL.getABITypeAlignment(ArgVal->getType());
      MachineMemOperand *MMO = FuncInfo.MF->getMachineMemOperand(
          MachinePointerInfo::getStack(*FuncInfo.MF, Addr.getOffset()),
          MachineMemOperand::MOStore, ArgVT.getStoreSize(), Alignment);
      (void)(MMO);
      // if (!emitStore(ArgVT, ArgReg, Addr, MMO))
      return false; // can't store on the stack yet.
    }
  }

  return true;
}

bool MipsFastISel::finishCall(CallLoweringInfo &CLI, MVT RetVT,
                              unsigned NumBytes) {
  CallingConv::ID CC = CLI.CallConv;
  emitInst(Mips::ADJCALLSTACKUP).addImm(16);
  if (RetVT != MVT::isVoid) {
    SmallVector<CCValAssign, 16> RVLocs;
    CCState CCInfo(CC, false, *FuncInfo.MF, RVLocs, *Context);
    CCInfo.AnalyzeCallResult(RetVT, RetCC_Mips);

    // Only handle a single return value.
    if (RVLocs.size() != 1)
      return false;
    // Copy all of the result registers out of their specified physreg.
    MVT CopyVT = RVLocs[0].getValVT();
    // Special handling for extended integers.
    if (RetVT == MVT::i1 || RetVT == MVT::i8 || RetVT == MVT::i16)
      CopyVT = MVT::i32;

    unsigned ResultReg = createResultReg(TLI.getRegClassFor(CopyVT));
    if (!ResultReg)
      return false;
    BuildMI(*FuncInfo.MBB, FuncInfo.InsertPt, DbgLoc,
            TII.get(TargetOpcode::COPY),
            ResultReg).addReg(RVLocs[0].getLocReg());
    CLI.InRegs.push_back(RVLocs[0].getLocReg());

    CLI.ResultReg = ResultReg;
    CLI.NumResultRegs = 1;
  }
  return true;
}

bool MipsFastISel::fastLowerCall(CallLoweringInfo &CLI) {
  if (!TargetSupported)
    return false;

  CallingConv::ID CC = CLI.CallConv;
  bool IsTailCall = CLI.IsTailCall;
  bool IsVarArg = CLI.IsVarArg;
  const Value *Callee = CLI.Callee;
  MCSymbol *Symbol = CLI.Symbol;

  // Do not handle FastCC.
  if (CC == CallingConv::Fast)
    return false;

  // Allow SelectionDAG isel to handle tail calls.
  if (IsTailCall)
    return false;

  // Let SDISel handle vararg functions.
  if (IsVarArg)
    return false;

  // FIXME: Only handle *simple* calls for now.
  MVT RetVT;
  if (CLI.RetTy->isVoidTy())
    RetVT = MVT::isVoid;
  else if (!isTypeSupported(CLI.RetTy, RetVT))
    return false;

  for (auto Flag : CLI.OutFlags)
    if (Flag.isInReg() || Flag.isSRet() || Flag.isNest() || Flag.isByVal())
      return false;

  // Set up the argument vectors.
  SmallVector<MVT, 16> OutVTs;
  OutVTs.reserve(CLI.OutVals.size());

  for (auto *Val : CLI.OutVals) {
    MVT VT;
    if (!isTypeLegal(Val->getType(), VT) &&
        !(VT == MVT::i1 || VT == MVT::i8 || VT == MVT::i16))
      return false;

    // We don't handle vector parameters yet.
    if (VT.isVector() || VT.getSizeInBits() > 64)
      return false;

    OutVTs.push_back(VT);
  }

  Address Addr;
  if (!computeCallAddress(Callee, Addr))
    return false;

  // Handle the arguments now that we've gotten them.
  unsigned NumBytes;
  if (!processCallArgs(CLI, OutVTs, NumBytes))
    return false;

  if (!Addr.getGlobalValue())
    return false;

  // Issue the call.
  unsigned DestAddress;
  if (Symbol)
    DestAddress = materializeExternalCallSym(Symbol);
  else
    DestAddress = materializeGV(Addr.getGlobalValue(), MVT::i32);
  emitInst(TargetOpcode::COPY, Mips::T9).addReg(DestAddress);
  MachineInstrBuilder MIB =
      BuildMI(*FuncInfo.MBB, FuncInfo.InsertPt, DbgLoc, TII.get(Mips::JALR),
              Mips::RA).addReg(Mips::T9);

  // Add implicit physical register uses to the call.
  for (auto Reg : CLI.OutRegs)
    MIB.addReg(Reg, RegState::Implicit);

  // Add a register mask with the call-preserved registers.
  // Proper defs for return values will be added by setPhysRegsDeadExcept().
  MIB.addRegMask(TRI.getCallPreservedMask(*FuncInfo.MF, CC));

  CLI.Call = MIB;

  // Finish off the call including any return values.
  return finishCall(CLI, RetVT, NumBytes);
}

bool MipsFastISel::fastLowerIntrinsicCall(const IntrinsicInst *II) {
  if (!TargetSupported)
    return false;

  switch (II->getIntrinsicID()) {
  default:
    return false;
  case Intrinsic::bswap: {
    Type *RetTy = II->getCalledFunction()->getReturnType();

    MVT VT;
    if (!isTypeSupported(RetTy, VT))
      return false;

    unsigned SrcReg = getRegForValue(II->getOperand(0));
    if (SrcReg == 0)
      return false;
    unsigned DestReg = createResultReg(&Mips::GPR32RegClass);
    if (DestReg == 0)
      return false;
    if (VT == MVT::i16) {
      if (Subtarget->hasMips32r2()) {
        emitInst(Mips::WSBH, DestReg).addReg(SrcReg);
        updateValueMap(II, DestReg);
        return true;
      } else {
        unsigned TempReg[3];
        for (int i = 0; i < 3; i++) {
          TempReg[i] = createResultReg(&Mips::GPR32RegClass);
          if (TempReg[i] == 0)
            return false;
        }
        emitInst(Mips::SLL, TempReg[0]).addReg(SrcReg).addImm(8);
        emitInst(Mips::SRL, TempReg[1]).addReg(SrcReg).addImm(8);
        emitInst(Mips::OR, TempReg[2]).addReg(TempReg[0]).addReg(TempReg[1]);
        emitInst(Mips::ANDi, DestReg).addReg(TempReg[2]).addImm(0xFFFF);
        updateValueMap(II, DestReg);
        return true;
      }
    } else if (VT == MVT::i32) {
      if (Subtarget->hasMips32r2()) {
        unsigned TempReg = createResultReg(&Mips::GPR32RegClass);
        emitInst(Mips::WSBH, TempReg).addReg(SrcReg);
        emitInst(Mips::ROTR, DestReg).addReg(TempReg).addImm(16);
        updateValueMap(II, DestReg);
        return true;
      } else {
        unsigned TempReg[8];
        for (int i = 0; i < 8; i++) {
          TempReg[i] = createResultReg(&Mips::GPR32RegClass);
          if (TempReg[i] == 0)
            return false;
        }

        emitInst(Mips::SRL, TempReg[0]).addReg(SrcReg).addImm(8);
        emitInst(Mips::SRL, TempReg[1]).addReg(SrcReg).addImm(24);
        emitInst(Mips::ANDi, TempReg[2]).addReg(TempReg[0]).addImm(0xFF00);
        emitInst(Mips::OR, TempReg[3]).addReg(TempReg[1]).addReg(TempReg[2]);

        emitInst(Mips::ANDi, TempReg[4]).addReg(SrcReg).addImm(0xFF00);
        emitInst(Mips::SLL, TempReg[5]).addReg(TempReg[4]).addImm(8);

        emitInst(Mips::SLL, TempReg[6]).addReg(SrcReg).addImm(24);
        emitInst(Mips::OR, TempReg[7]).addReg(TempReg[3]).addReg(TempReg[5]);
        emitInst(Mips::OR, DestReg).addReg(TempReg[6]).addReg(TempReg[7]);
        updateValueMap(II, DestReg);
        return true;
      }
    }
    return false;
  }
  case Intrinsic::memcpy:
  case Intrinsic::memmove: {
    const auto *MTI = cast<MemTransferInst>(II);
    // Don't handle volatile.
    if (MTI->isVolatile())
      return false;
    if (!MTI->getLength()->getType()->isIntegerTy(32))
      return false;
    const char *IntrMemName = isa<MemCpyInst>(II) ? "memcpy" : "memmove";
    return lowerCallTo(II, IntrMemName, II->getNumArgOperands() - 2);
  }
  case Intrinsic::memset: {
    const MemSetInst *MSI = cast<MemSetInst>(II);
    // Don't handle volatile.
    if (MSI->isVolatile())
      return false;
    if (!MSI->getLength()->getType()->isIntegerTy(32))
      return false;
    return lowerCallTo(II, "memset", II->getNumArgOperands() - 2);
  }
  }
  return false;
}

bool MipsFastISel::selectRet(const Instruction *I) {
  const Function &F = *I->getParent()->getParent();
  const ReturnInst *Ret = cast<ReturnInst>(I);

  if (!FuncInfo.CanLowerReturn)
    return false;

  // Build a list of return value registers.
  SmallVector<unsigned, 4> RetRegs;

  if (Ret->getNumOperands() > 0) {
    CallingConv::ID CC = F.getCallingConv();

    // Do not handle FastCC.
    if (CC == CallingConv::Fast)
      return false;

    SmallVector<ISD::OutputArg, 4> Outs;
    GetReturnInfo(F.getReturnType(), F.getAttributes(), Outs, TLI, DL);

    // Analyze operands of the call, assigning locations to each operand.
    SmallVector<CCValAssign, 16> ValLocs;
    MipsCCState CCInfo(CC, F.isVarArg(), *FuncInfo.MF, ValLocs,
                       I->getContext());
    CCAssignFn *RetCC = RetCC_Mips;
    CCInfo.AnalyzeReturn(Outs, RetCC);

    // Only handle a single return value for now.
    if (ValLocs.size() != 1)
      return false;

    CCValAssign &VA = ValLocs[0];
    const Value *RV = Ret->getOperand(0);

    // Don't bother handling odd stuff for now.
    if ((VA.getLocInfo() != CCValAssign::Full) &&
        (VA.getLocInfo() != CCValAssign::BCvt))
      return false;

    // Only handle register returns for now.
    if (!VA.isRegLoc())
      return false;

    unsigned Reg = getRegForValue(RV);
    if (Reg == 0)
      return false;

    unsigned SrcReg = Reg + VA.getValNo();
    unsigned DestReg = VA.getLocReg();
    // Avoid a cross-class copy. This is very unlikely.
    if (!MRI.getRegClass(SrcReg)->contains(DestReg))
      return false;

    EVT RVEVT = TLI.getValueType(DL, RV->getType());
    if (!RVEVT.isSimple())
      return false;

    if (RVEVT.isVector())
      return false;

    MVT RVVT = RVEVT.getSimpleVT();
    if (RVVT == MVT::f128)
      return false;

    MVT DestVT = VA.getValVT();
    // Special handling for extended integers.
    if (RVVT != DestVT) {
      if (RVVT != MVT::i1 && RVVT != MVT::i8 && RVVT != MVT::i16)
        return false;

      if (Outs[0].Flags.isZExt() || Outs[0].Flags.isSExt()) {
        bool IsZExt = Outs[0].Flags.isZExt();
        SrcReg = emitIntExt(RVVT, SrcReg, DestVT, IsZExt);
        if (SrcReg == 0)
          return false;
      }
    }

    // Make the copy.
    BuildMI(*FuncInfo.MBB, FuncInfo.InsertPt, DbgLoc,
            TII.get(TargetOpcode::COPY), DestReg).addReg(SrcReg);

    // Add register to return instruction.
    RetRegs.push_back(VA.getLocReg());
  }
  MachineInstrBuilder MIB = emitInst(Mips::RetRA);
  for (unsigned i = 0, e = RetRegs.size(); i != e; ++i)
    MIB.addReg(RetRegs[i], RegState::Implicit);
  return true;
}

bool MipsFastISel::selectTrunc(const Instruction *I) {
  // The high bits for a type smaller than the register size are assumed to be
  // undefined.
  Value *Op = I->getOperand(0);

  EVT SrcVT, DestVT;
  SrcVT = TLI.getValueType(DL, Op->getType(), true);
  DestVT = TLI.getValueType(DL, I->getType(), true);

  if (SrcVT != MVT::i32 && SrcVT != MVT::i16 && SrcVT != MVT::i8)
    return false;
  if (DestVT != MVT::i16 && DestVT != MVT::i8 && DestVT != MVT::i1)
    return false;

  unsigned SrcReg = getRegForValue(Op);
  if (!SrcReg)
    return false;

  // Because the high bits are undefined, a truncate doesn't generate
  // any code.
  updateValueMap(I, SrcReg);
  return true;
}
bool MipsFastISel::selectIntExt(const Instruction *I) {
  Type *DestTy = I->getType();
  Value *Src = I->getOperand(0);
  Type *SrcTy = Src->getType();

  bool isZExt = isa<ZExtInst>(I);
  unsigned SrcReg = getRegForValue(Src);
  if (!SrcReg)
    return false;

  EVT SrcEVT, DestEVT;
  SrcEVT = TLI.getValueType(DL, SrcTy, true);
  DestEVT = TLI.getValueType(DL, DestTy, true);
  if (!SrcEVT.isSimple())
    return false;
  if (!DestEVT.isSimple())
    return false;

  MVT SrcVT = SrcEVT.getSimpleVT();
  MVT DestVT = DestEVT.getSimpleVT();
  unsigned ResultReg = createResultReg(&Mips::GPR32RegClass);

  if (!emitIntExt(SrcVT, SrcReg, DestVT, ResultReg, isZExt))
    return false;
  updateValueMap(I, ResultReg);
  return true;
}
bool MipsFastISel::emitIntSExt32r1(MVT SrcVT, unsigned SrcReg, MVT DestVT,
                                   unsigned DestReg) {
  unsigned ShiftAmt;
  switch (SrcVT.SimpleTy) {
  default:
    return false;
  case MVT::i8:
    ShiftAmt = 24;
    break;
  case MVT::i16:
    ShiftAmt = 16;
    break;
  }
  unsigned TempReg = createResultReg(&Mips::GPR32RegClass);
  emitInst(Mips::SLL, TempReg).addReg(SrcReg).addImm(ShiftAmt);
  emitInst(Mips::SRA, DestReg).addReg(TempReg).addImm(ShiftAmt);
  return true;
}

bool MipsFastISel::emitIntSExt32r2(MVT SrcVT, unsigned SrcReg, MVT DestVT,
                                   unsigned DestReg) {
  switch (SrcVT.SimpleTy) {
  default:
    return false;
  case MVT::i8:
    emitInst(Mips::SEB, DestReg).addReg(SrcReg);
    break;
  case MVT::i16:
    emitInst(Mips::SEH, DestReg).addReg(SrcReg);
    break;
  }
  return true;
}

bool MipsFastISel::emitIntSExt(MVT SrcVT, unsigned SrcReg, MVT DestVT,
                               unsigned DestReg) {
  if ((DestVT != MVT::i32) && (DestVT != MVT::i16))
    return false;
  if (Subtarget->hasMips32r2())
    return emitIntSExt32r2(SrcVT, SrcReg, DestVT, DestReg);
  return emitIntSExt32r1(SrcVT, SrcReg, DestVT, DestReg);
}

bool MipsFastISel::emitIntZExt(MVT SrcVT, unsigned SrcReg, MVT DestVT,
                               unsigned DestReg) {
  int64_t Imm;

  switch (SrcVT.SimpleTy) {
  default:
    return false;
  case MVT::i1:
    Imm = 1;
    break;
  case MVT::i8:
    Imm = 0xff;
    break;
  case MVT::i16:
    Imm = 0xffff;
    break;
  }

  emitInst(Mips::ANDi, DestReg).addReg(SrcReg).addImm(Imm);
  return true;
}

bool MipsFastISel::emitIntExt(MVT SrcVT, unsigned SrcReg, MVT DestVT,
                              unsigned DestReg, bool IsZExt) {
  // FastISel does not have plumbing to deal with extensions where the SrcVT or
  // DestVT are odd things, so test to make sure that they are both types we can
  // handle (i1/i8/i16/i32 for SrcVT and i8/i16/i32/i64 for DestVT), otherwise
  // bail out to SelectionDAG.
  if (((DestVT != MVT::i8) && (DestVT != MVT::i16) && (DestVT != MVT::i32)) ||
      ((SrcVT != MVT::i1) && (SrcVT != MVT::i8) && (SrcVT != MVT::i16)))
    return false;
  if (IsZExt)
    return emitIntZExt(SrcVT, SrcReg, DestVT, DestReg);
  return emitIntSExt(SrcVT, SrcReg, DestVT, DestReg);
}

unsigned MipsFastISel::emitIntExt(MVT SrcVT, unsigned SrcReg, MVT DestVT,
                                  bool isZExt) {
  unsigned DestReg = createResultReg(&Mips::GPR32RegClass);
  bool Success = emitIntExt(SrcVT, SrcReg, DestVT, DestReg, isZExt);
  return Success ? DestReg : 0;
}

bool MipsFastISel::selectDivRem(const Instruction *I, unsigned ISDOpcode) {
  EVT DestEVT = TLI.getValueType(DL, I->getType(), true);
  if (!DestEVT.isSimple())
    return false;

  MVT DestVT = DestEVT.getSimpleVT();
  if (DestVT != MVT::i32)
    return false;

  unsigned DivOpc;
  switch (ISDOpcode) {
  default:
    return false;
  case ISD::SDIV:
  case ISD::SREM:
    DivOpc = Mips::SDIV;
    break;
  case ISD::UDIV:
  case ISD::UREM:
    DivOpc = Mips::UDIV;
    break;
  }

  unsigned Src0Reg = getRegForValue(I->getOperand(0));
  unsigned Src1Reg = getRegForValue(I->getOperand(1));
  if (!Src0Reg || !Src1Reg)
    return false;

  emitInst(DivOpc).addReg(Src0Reg).addReg(Src1Reg);
  emitInst(Mips::TEQ).addReg(Src1Reg).addReg(Mips::ZERO).addImm(7);

  unsigned ResultReg = createResultReg(&Mips::GPR32RegClass);
  if (!ResultReg)
    return false;

  unsigned MFOpc = (ISDOpcode == ISD::SREM || ISDOpcode == ISD::UREM)
                       ? Mips::MFHI
                       : Mips::MFLO;
  emitInst(MFOpc, ResultReg);

  updateValueMap(I, ResultReg);
  return true;
}

bool MipsFastISel::selectShift(const Instruction *I) {
  MVT RetVT;

  if (!isTypeSupported(I->getType(), RetVT))
    return false;

  unsigned ResultReg = createResultReg(&Mips::GPR32RegClass);
  if (!ResultReg)
    return false;

  unsigned Opcode = I->getOpcode();
  const Value *Op0 = I->getOperand(0);
  unsigned Op0Reg = getRegForValue(Op0);
  if (!Op0Reg)
    return false;

  // If AShr or LShr, then we need to make sure the operand0 is sign extended.
  if (Opcode == Instruction::AShr || Opcode == Instruction::LShr) {
    unsigned TempReg = createResultReg(&Mips::GPR32RegClass);
    if (!TempReg)
      return false;

    MVT Op0MVT = TLI.getValueType(DL, Op0->getType(), true).getSimpleVT();
    bool IsZExt = Opcode == Instruction::LShr;
    if (!emitIntExt(Op0MVT, Op0Reg, MVT::i32, TempReg, IsZExt))
      return false;

    Op0Reg = TempReg;
  }

  if (const auto *C = dyn_cast<ConstantInt>(I->getOperand(1))) {
    uint64_t ShiftVal = C->getZExtValue();

    switch (Opcode) {
    default:
      llvm_unreachable("Unexpected instruction.");
    case Instruction::Shl:
      Opcode = Mips::SLL;
      break;
    case Instruction::AShr:
      Opcode = Mips::SRA;
      break;
    case Instruction::LShr:
      Opcode = Mips::SRL;
      break;
    }

    emitInst(Opcode, ResultReg).addReg(Op0Reg).addImm(ShiftVal);
    updateValueMap(I, ResultReg);
    return true;
  }

  unsigned Op1Reg = getRegForValue(I->getOperand(1));
  if (!Op1Reg)
    return false;

  switch (Opcode) {
  default:
    llvm_unreachable("Unexpected instruction.");
  case Instruction::Shl:
    Opcode = Mips::SLLV;
    break;
  case Instruction::AShr:
    Opcode = Mips::SRAV;
    break;
  case Instruction::LShr:
    Opcode = Mips::SRLV;
    break;
  }

  emitInst(Opcode, ResultReg).addReg(Op0Reg).addReg(Op1Reg);
  updateValueMap(I, ResultReg);
  return true;
}

bool MipsFastISel::fastSelectInstruction(const Instruction *I) {
  if (!TargetSupported)
    return false;
  switch (I->getOpcode()) {
  default:
    break;
  case Instruction::Load:
    return selectLoad(I);
  case Instruction::Store:
    return selectStore(I);
  case Instruction::SDiv:
    if (!selectBinaryOp(I, ISD::SDIV))
      return selectDivRem(I, ISD::SDIV);
    return true;
  case Instruction::UDiv:
    if (!selectBinaryOp(I, ISD::UDIV))
      return selectDivRem(I, ISD::UDIV);
    return true;
  case Instruction::SRem:
    if (!selectBinaryOp(I, ISD::SREM))
      return selectDivRem(I, ISD::SREM);
    return true;
  case Instruction::URem:
    if (!selectBinaryOp(I, ISD::UREM))
      return selectDivRem(I, ISD::UREM);
    return true;
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr:
    return selectShift(I);
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
    return selectLogicalOp(I);
  case Instruction::Br:
    return selectBranch(I);
  case Instruction::Ret:
    return selectRet(I);
  case Instruction::Trunc:
    return selectTrunc(I);
  case Instruction::ZExt:
  case Instruction::SExt:
    return selectIntExt(I);
  case Instruction::FPTrunc:
    return selectFPTrunc(I);
  case Instruction::FPExt:
    return selectFPExt(I);
  case Instruction::FPToSI:
    return selectFPToInt(I, /*isSigned*/ true);
  case Instruction::FPToUI:
    return selectFPToInt(I, /*isSigned*/ false);
  case Instruction::ICmp:
  case Instruction::FCmp:
    return selectCmp(I);
  case Instruction::Select:
    return selectSelect(I);
  }
  return false;
}

unsigned MipsFastISel::getRegEnsuringSimpleIntegerWidening(const Value *V,
                                                           bool IsUnsigned) {
  unsigned VReg = getRegForValue(V);
  if (VReg == 0)
    return 0;
  MVT VMVT = TLI.getValueType(DL, V->getType(), true).getSimpleVT();
  if ((VMVT == MVT::i8) || (VMVT == MVT::i16)) {
    unsigned TempReg = createResultReg(&Mips::GPR32RegClass);
    if (!emitIntExt(VMVT, VReg, MVT::i32, TempReg, IsUnsigned))
      return 0;
    VReg = TempReg;
  }
  return VReg;
}

void MipsFastISel::simplifyAddress(Address &Addr) {
  if (!isInt<16>(Addr.getOffset())) {
    unsigned TempReg =
        materialize32BitInt(Addr.getOffset(), &Mips::GPR32RegClass);
    unsigned DestReg = createResultReg(&Mips::GPR32RegClass);
    emitInst(Mips::ADDu, DestReg).addReg(TempReg).addReg(Addr.getReg());
    Addr.setReg(DestReg);
    Addr.setOffset(0);
  }
}

unsigned MipsFastISel::fastEmitInst_rr(unsigned MachineInstOpcode,
                                       const TargetRegisterClass *RC,
                                       unsigned Op0, bool Op0IsKill,
                                       unsigned Op1, bool Op1IsKill) {
  // We treat the MUL instruction in a special way because it clobbers
  // the HI0 & LO0 registers. The TableGen definition of this instruction can
  // mark these registers only as implicitly defined. As a result, the
  // register allocator runs out of registers when this instruction is
  // followed by another instruction that defines the same registers too.
  // We can fix this by explicitly marking those registers as dead.
  if (MachineInstOpcode == Mips::MUL) {
    unsigned ResultReg = createResultReg(RC);
    const MCInstrDesc &II = TII.get(MachineInstOpcode);
    Op0 = constrainOperandRegClass(II, Op0, II.getNumDefs());
    Op1 = constrainOperandRegClass(II, Op1, II.getNumDefs() + 1);
    BuildMI(*FuncInfo.MBB, FuncInfo.InsertPt, DbgLoc, II, ResultReg)
      .addReg(Op0, getKillRegState(Op0IsKill))
      .addReg(Op1, getKillRegState(Op1IsKill))
      .addReg(Mips::HI0, RegState::ImplicitDefine | RegState::Dead)
      .addReg(Mips::LO0, RegState::ImplicitDefine | RegState::Dead);
    return ResultReg;
  }

  return FastISel::fastEmitInst_rr(MachineInstOpcode, RC, Op0, Op0IsKill, Op1,
                                   Op1IsKill);
}

namespace llvm {
FastISel *Mips::createFastISel(FunctionLoweringInfo &funcInfo,
                               const TargetLibraryInfo *libInfo) {
  return new MipsFastISel(funcInfo, libInfo);
}
}