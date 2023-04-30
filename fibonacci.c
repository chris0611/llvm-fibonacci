/*===----------------------------------------------------------------------===*\
|*                                                                            *|
|*  Fibonacci example module created with LLVM C bindings and executed with   *|
|*  the JIT.                                                                  *|
|*                                                                            *|
|*  Based on: https://github.com/llvm/llvm-project/blob/main/llvm/examples/   *|
|*      Fibonacci/fibonacci.cpp                                               *|
|*                                                                            *|
|*  With inspiration from: https://github.com/llvm/llvm-project/blob/main/    *|
|*      llvm/examples/OrcV2Examples/OrcV2CBindingsAddObjectFile/              *|
|*      OrcV2CBindingsAddObjectFile.c                                         *|
|*                                                                            *|
\*===----------------------------------------------------------------------===*/

#include <stdio.h>

#include <llvm-c/Core.h>
#include <llvm-c/Error.h>
#include <llvm-c/LLJIT.h>
#include <llvm-c/Support.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

int handle_error(LLVMErrorRef Err) {
    char *ErrMsg = LLVMGetErrorMessage(Err);
    fprintf(stderr, "Error: %s\n", ErrMsg);
    LLVMDisposeErrorMessage(ErrMsg);
    return 1;
}

static LLVMValueRef create_fib_func(LLVMModuleRef mod, LLVMContextRef ctx)
{
    LLVMTypeRef fib_arg_ty[] = { LLVMInt32TypeInContext(ctx) };
    LLVMTypeRef fib_fn_ty = LLVMFunctionType(LLVMInt32TypeInContext(ctx), fib_arg_ty, 1, 0);
    LLVMValueRef fib_fn = LLVMAddFunction(mod, "fib", fib_fn_ty);

    LLVMBasicBlockRef entry_BB = LLVMAppendBasicBlockInContext(ctx, fib_fn, "entry");
    LLVMBasicBlockRef return_BB = LLVMAppendBasicBlockInContext(ctx, fib_fn, "return");
    LLVMBasicBlockRef recurse_BB = LLVMAppendBasicBlockInContext(ctx, fib_fn, "recurse");

    // Number constants
    LLVMValueRef one = LLVMConstInt(LLVMInt32TypeInContext(ctx), 1, 0);
    LLVMValueRef two = LLVMConstInt(LLVMInt32TypeInContext(ctx), 2, 0);

    LLVMValueRef fib_arg = LLVMGetParam(fib_fn, 0);
    LLVMSetValueName(fib_arg, "n");

    // Instruction builder
    LLVMBuilderRef builder = LLVMCreateBuilderInContext(ctx);

    LLVMPositionBuilderAtEnd(builder, entry_BB);
    LLVMValueRef condinst = LLVMBuildICmp(builder, LLVMIntSLE, fib_arg, two, "cond");
    LLVMBuildCondBr(builder, condinst, return_BB, recurse_BB);

    LLVMPositionBuilderAtEnd(builder, return_BB);
    LLVMBuildRet(builder, one);

    LLVMPositionBuilderAtEnd(builder, recurse_BB);
    LLVMValueRef sub = LLVMBuildSub(builder, fib_arg, one, "arg");
    LLVMValueRef fibx1_args[] = { sub };
    LLVMValueRef call_fibx1 = LLVMBuildCall2(builder, fib_fn_ty, fib_fn, fibx1_args, 1, "fibx1");
    LLVMSetTailCall(call_fibx1, 1);
    sub = LLVMBuildSub(builder, fib_arg, two, "arg");
    LLVMValueRef fibx2_args[] = { sub };
    LLVMValueRef call_fibx2 = LLVMBuildCall2(builder, fib_fn_ty, fib_fn, fibx2_args, 1, "fibx2");
    LLVMSetTailCall(call_fibx2, 1);
    LLVMValueRef sum = LLVMBuildAdd(builder, call_fibx1, call_fibx2, "addresult");
    LLVMBuildRet(builder, sum);

    LLVMDisposeBuilder(builder);
    return fib_fn;
}

int main(int argc, const char *argv[])
{
    int main_res = 0;
    LLVMParseCommandLineOptions(argc, argv, "");

    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();

    LLVMContextRef ctx = LLVMContextCreate();
    LLVMModuleRef test_mod = LLVMModuleCreateWithNameInContext("test", ctx);

    create_fib_func(test_mod, ctx);
    LLVMPrintModuleToFile(test_mod, "fib.ll", NULL);

    LLVMOrcLLJITRef lljit;
    {
        LLVMErrorRef err;
        if ((err = LLVMOrcCreateLLJIT(&lljit, 0))) {
            main_res = handle_error(err);
            goto llvm_shutdown;
        }
    }

    LLVMMemoryBufferRef obj_buf;

    const char *triple = LLVMOrcLLJITGetTripleString(lljit);
    LLVMTargetRef target = 0;
    char *err_msg = 0;

    if (LLVMGetTargetFromTriple(triple, &target, &err_msg)) {
        fprintf(stderr, "Error getting target for %s: %s\n", triple, err_msg);
        LLVMDisposeModule(test_mod);
        LLVMContextDispose(ctx);
        goto jit_cleanup;
    }

    LLVMTargetMachineRef tm =
        LLVMCreateTargetMachine(target, triple, "", "", LLVMCodeGenLevelNone,
                                LLVMRelocDefault, LLVMCodeModelDefault);

    // Create assembly output
    if (LLVMTargetMachineEmitToFile(tm, test_mod, "fib.s", LLVMAssemblyFile, &err_msg)) {
        fprintf(stderr, "Error emitting assembly: %s\n", err_msg);
        LLVMDisposeTargetMachine(tm);
        LLVMDisposeModule(test_mod);
        LLVMContextDispose(ctx);
        goto jit_cleanup;
    }

    // Emit object file for JIT
    if (LLVMTargetMachineEmitToMemoryBuffer(tm, test_mod, LLVMObjectFile, &err_msg, &obj_buf)) {
        fprintf(stderr, "Error emitting object: %s\n", err_msg);
        LLVMDisposeTargetMachine(tm);
        LLVMDisposeModule(test_mod);
        LLVMContextDispose(ctx);
        goto jit_cleanup;
    }

    {
        LLVMOrcJITDylibRef mainjd = LLVMOrcLLJITGetMainJITDylib(lljit);
        LLVMErrorRef err;
        if ((err = LLVMOrcLLJITAddObjectFile(lljit, mainjd, obj_buf))) {
            main_res = handle_error(err);
            goto jit_cleanup;
        }
    }

    LLVMOrcJITTargetAddress fib_addr;
    {
        LLVMErrorRef err;
        if ((err = LLVMOrcLLJITLookup(lljit, &fib_addr, "fib"))) {
            main_res = handle_error(err);
            goto jit_cleanup;
        }
    }

    // Execute JIT'ed code
    int32_t (*fib)(int32_t) = (int32_t(*)(int32_t))fib_addr;
    const int32_t fib_arg = 10;
    int32_t res = fib(fib_arg);

    printf("fib(%d) = %d\n", fib_arg, res);

jit_cleanup:
    {
        LLVMErrorRef err;
        if ((err = LLVMOrcDisposeLLJIT(lljit))) {
            int new_failure = handle_error(err);
            if (main_res == 0)
                main_res = new_failure;
        }
    }

llvm_shutdown:
    LLVMShutdown();
    return main_res;
}
