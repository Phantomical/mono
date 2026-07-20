//
// engine_spike.cpp: De-risk spike for porting Mono's LLVM JIT backend to
// UNMODIFIED system LLVM 18, using the ORCv2 LLJIT API.
//
// Goal: build an IR module at runtime, JIT it with ORCv2, resolve a host
// (in-process) symbol, call the compiled code, and verify the results.
//
// This is a standalone spike. It does NOT touch the mono runtime build.
//
// Build: ./build.sh   (invokes llvm-config-18)
//
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include <llvm/ADT/StringRef.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>   // DynamicLibrarySearchGenerator
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace llvm::orc;

// ---------------------------------------------------------------------------
// Host symbol. The JITed code below will call this by name, proving that
// ORCv2 can resolve symbols against the running process. In the real backend
// this is how JITed IL would reach mono runtime helpers (icalls, etc.).
//
// extern "C" so the symbol name is unmangled ("host_triple"), and it must be
// exported into the dynamic symbol table -> we link the driver with -rdynamic
// (see build.sh) so DynamicLibrarySearchGenerator::GetForCurrentProcess finds
// it via dlsym.
// ---------------------------------------------------------------------------
extern "C" int32_t host_triple(int32_t x) {
    return x * 3;
}

// Fatal helper for Expected<T>/Error.
static void fail(Error Err) {
    logAllUnhandledErrors(std::move(Err), errs(), "[engine_spike] FATAL: ");
    std::exit(1);
}

// Build a module containing:
//   i32 add(i32 a, i32 b)        -> a + b
//   i32 triple_plus(i32 a, i32 b)-> host_triple(a) + b   (calls host symbol)
static ThreadSafeModule buildDemoModule() {
    auto Ctx = std::make_unique<LLVMContext>();
    auto M = std::make_unique<Module>("spike_module", *Ctx);

    IRBuilder<> B(*Ctx);
    Type *I32 = Type::getInt32Ty(*Ctx);
    FunctionType *BinOpTy = FunctionType::get(I32, {I32, I32}, /*vararg*/ false);

    // --- i32 add(i32, i32) ---
    {
        Function *F = Function::Create(BinOpTy, Function::ExternalLinkage, "add", M.get());
        auto *BB = BasicBlock::Create(*Ctx, "entry", F);
        B.SetInsertPoint(BB);
        auto args = F->arg_begin();
        Value *a = &*args++;
        Value *b = &*args++;
        B.CreateRet(B.CreateAdd(a, b, "sum"));
    }

    // --- declaration of the external host function: i32 host_triple(i32) ---
    FunctionType *HostTy = FunctionType::get(I32, {I32}, false);
    Function *HostF =
        Function::Create(HostTy, Function::ExternalLinkage, "host_triple", M.get());

    // --- i32 triple_plus(i32, i32) -> host_triple(a) + b ---
    {
        Function *F = Function::Create(BinOpTy, Function::ExternalLinkage, "triple_plus", M.get());
        auto *BB = BasicBlock::Create(*Ctx, "entry", F);
        B.SetInsertPoint(BB);
        auto args = F->arg_begin();
        Value *a = &*args++;
        Value *b = &*args++;
        Value *tripled = B.CreateCall(HostF, {a}, "tripled");
        B.CreateRet(B.CreateAdd(tripled, b, "res"));
    }

    // Verify IR before handing it to the JIT.
    if (verifyModule(*M, &errs())) {
        errs() << "[engine_spike] module verification failed\n";
        std::exit(1);
    }

    return ThreadSafeModule(std::move(M), std::move(Ctx));
}

int main() {
    // ORCv2 needs the native target + asm printer initialized for codegen.
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();

    // ---------------------------------------------------------------------
    // Build the JIT.
    //
    // LLJITBuilder gives us a ready-made tiered-friendly stack:
    //   IRCompileLayer -> ObjectLinkingLayer (RTDyld by default on ELF/amd64).
    //
    // FORWARD-COMPAT HOOKS (not implemented in this spike, noted per task):
    //   * Custom memory manager: LLJITBuilder::setObjectLinkingLayerCreator
    //     lets us install an RTDyldObjectLinkingLayer wired to a custom
    //     RTDyldMemoryManager/SectionMemoryManager for code+data allocation.
    //     (LLJIT defaults to RTDyld on ELF/amd64, so RTDyld + a custom
    //     MemoryManager is our real path. The JITLink alternative pairs the
    //     ObjectLinkingLayer with a JITLinkMemoryManager instead -- a
    //     different layer, don't conflate the two.)
    //   * .eh_frame registration for exceptions: with RTDyld, the memory
    //     manager's registerEHFrames() hook fires here; the JITLink path uses
    //     an ObjectLinkingLayer EHFrameRegistrationPlugin. Either way this is
    //     where unwinding through JITed frames gets wired up.
    //   * Tiering / lazy compilation: CompileOnDemandLayer / lazy reexports
    //     layer on top of this same LLJIT stack.
    // ---------------------------------------------------------------------
    auto JITOrErr = LLJITBuilder().create();
    if (!JITOrErr)
        fail(JITOrErr.takeError());
    std::unique_ptr<LLJIT> JIT = std::move(*JITOrErr);

    // Resolve symbols against the current process (finds host_triple).
    // Requires the driver to be linked with -rdynamic (see build.sh).
    auto DLSG = DynamicLibrarySearchGenerator::GetForCurrentProcess(
        JIT->getDataLayout().getGlobalPrefix());
    if (!DLSG)
        fail(DLSG.takeError());
    JIT->getMainJITDylib().addGenerator(std::move(*DLSG));

    // Add our runtime-built module.
    if (auto Err = JIT->addIRModule(buildDemoModule()))
        fail(std::move(Err));

    // ---- look up + call: add ----
    auto AddSym = JIT->lookup("add");
    if (!AddSym)
        fail(AddSym.takeError());
    auto add_fn = AddSym->toPtr<int32_t (*)(int32_t, int32_t)>();
    int32_t add_res = add_fn(40, 2);
    printf("add(40, 2)         = %d  (expected 42)\n", add_res);

    // ---- look up + call: triple_plus (exercises host symbol resolution) ----
    auto TpSym = JIT->lookup("triple_plus");
    if (!TpSym)
        fail(TpSym.takeError());
    auto tp_fn = TpSym->toPtr<int32_t (*)(int32_t, int32_t)>();
    int32_t tp_res = tp_fn(10, 5);   // host_triple(10) + 5 = 35
    printf("triple_plus(10, 5) = %d  (expected 35)\n", tp_res);

    // ---- verify ----
    bool ok = (add_res == 42) && (tp_res == 35);
    printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
