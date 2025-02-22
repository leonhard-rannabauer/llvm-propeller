//===-- CodeGen/AsmPrinter/WasmException.cpp - Wasm Exception Impl --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains support for writing WebAssembly exception info into asm
// files.
//
//===----------------------------------------------------------------------===//

#include "WasmException.h"
#include "llvm/IR/Mangler.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCStreamer.h"
using namespace llvm;

void WasmException::endModule() {
  // This is the symbol used in 'throw' and 'br_on_exn' instruction to denote
  // this is a C++ exception. This symbol has to be emitted somewhere once in
  // the module.  Check if the symbol has already been created, i.e., we have at
  // least one 'throw' or 'br_on_exn' instruction in the module, and emit the
  // symbol only if so.
  SmallString<60> NameStr;
  Mangler::getNameWithPrefix(NameStr, "__cpp_exception", Asm->getDataLayout());
  if (Asm->OutContext.lookupSymbol(NameStr)) {
    MCSymbol *ExceptionSym = Asm->GetExternalSymbolSymbol("__cpp_exception");
    Asm->OutStreamer->emitLabel(ExceptionSym);
  }
}

void WasmException::markFunctionEnd() {
  // Get rid of any dead landing pads.
  if (!Asm->MF->getLandingPads().empty()) {
    auto *NonConstMF = const_cast<MachineFunction *>(Asm->MF);
    // Wasm does not set BeginLabel and EndLabel information for landing pads,
    // so we should set the second argument false.
    NonConstMF->tidyLandingPads(nullptr, /* TidyIfNoBeginLabels */ false);
  }
}

void WasmException::endFunction(const MachineFunction *MF) {
  bool ShouldEmitExceptionTable = false;
  for (const LandingPadInfo &Info : MF->getLandingPads()) {
    if (MF->hasWasmLandingPadIndex(Info.LandingPadBlock)) {
      ShouldEmitExceptionTable = true;
      break;
    }
  }
  if (!ShouldEmitExceptionTable)
    return;
  MCSymbol *LSDALabel = emitExceptionTable();
  assert(LSDALabel && ".GCC_exception_table has not been emitted!");

  // Wasm requires every data section symbol to have a .size set. So we emit an
  // end marker and set the size as the difference between the start end the end
  // marker.
  MCSymbol *LSDAEndLabel = Asm->createTempSymbol("GCC_except_table_end");
  Asm->OutStreamer->emitLabel(LSDAEndLabel);
  MCContext &OutContext = Asm->OutStreamer->getContext();
  const MCExpr *SizeExp = MCBinaryExpr::createSub(
      MCSymbolRefExpr::create(LSDAEndLabel, OutContext),
      MCSymbolRefExpr::create(LSDALabel, OutContext), OutContext);
  Asm->OutStreamer->emitELFSize(LSDALabel, SizeExp);
}

// Compute the call-site table for wasm EH. Even though we use the same function
// name to share the common routines, a call site entry in the table corresponds
// to not a call site for possibly-throwing functions but a landing pad. In wasm
// EH the VM is responsible for stack unwinding. After an exception occurs and
// the stack is unwound, the control flow is transferred to wasm 'catch'
// instruction by the VM, after which the personality function is called from
// the compiler-generated code. Refer to WasmEHPrepare pass for more
// information.
void WasmException::computeCallSiteTable(
    SmallVectorImpl<CallSiteEntry> &CallSites,
    SmallVectorImpl<CallSiteRange> &CallSiteRanges,
    const SmallVectorImpl<const LandingPadInfo *> &LandingPads,
    const SmallVectorImpl<unsigned> &FirstActions) {
  MachineFunction &MF = *Asm->MF;
  for (unsigned I = 0, N = LandingPads.size(); I < N; ++I) {
    const LandingPadInfo *Info = LandingPads[I];
    MachineBasicBlock *LPad = Info->LandingPadBlock;
    // We don't emit LSDA for single catch (...).
    if (!MF.hasWasmLandingPadIndex(LPad))
      continue;
    // Wasm EH must maintain the EH pads in the order assigned to them by the
    // WasmEHPrepare pass.
    unsigned LPadIndex = MF.getWasmLandingPadIndex(LPad);
    CallSiteEntry Site = {nullptr, nullptr, Info, FirstActions[I]};
    if (CallSites.size() < LPadIndex + 1)
      CallSites.resize(LPadIndex + 1);
    CallSites[LPadIndex] = Site;
  }

  // Add a single range containing all the call-sites.
  CallSiteRanges.push_back({Asm->getFunctionBegin(), Asm->getFunctionEnd(),
                            Asm->getCurExceptionSym(), 0, CallSites.size(),
                            true});
}
