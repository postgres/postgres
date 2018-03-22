/*-------------------------------------------------------------------------
 *
 * llvmjit.c
 *	  Core part of the LLVM JIT provider.
 *
 * Copyright (c) 2016-2018, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/jit/llvm/llvmjit.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "jit/llvmjit.h"

#include "miscadmin.h"

#include "utils/memutils.h"
#include "utils/resowner_private.h"
#include "storage/ipc.h"


#include <llvm-c/Target.h>


static bool llvm_session_initialized = false;


static void llvm_release_context(JitContext *context);
static void llvm_session_initialize(void);
static void llvm_shutdown(int code, Datum arg);


PG_MODULE_MAGIC;


/*
 * Initialize LLVM JIT provider.
 */
void
_PG_jit_provider_init(JitProviderCallbacks *cb)
{
	cb->reset_after_error = llvm_reset_after_error;
	cb->release_context = llvm_release_context;
}

/*
 * Create a context for JITing work.
 *
 * The context, including subsidiary resources, will be cleaned up either when
 * the context is explicitly released, or when the lifetime of
 * CurrentResourceOwner ends (usually the end of the current [sub]xact).
 */
LLVMJitContext *
llvm_create_context(int jitFlags)
{
	LLVMJitContext *context;

	llvm_assert_in_fatal_section();

	llvm_session_initialize();

	ResourceOwnerEnlargeJIT(CurrentResourceOwner);

	context = MemoryContextAllocZero(TopMemoryContext,
									 sizeof(LLVMJitContext));
	context->base.flags = jitFlags;

	/* ensure cleanup */
	context->base.resowner = CurrentResourceOwner;
	ResourceOwnerRememberJIT(CurrentResourceOwner, PointerGetDatum(context));

	return context;
}

/*
 * Release resources required by one llvm context.
 */
static void
llvm_release_context(JitContext *context)
{
}

/*
 * Per session initialization.
 */
static void
llvm_session_initialize(void)
{
	MemoryContext oldcontext;

	if (llvm_session_initialized)
		return;

	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	LLVMInitializeNativeTarget();
	LLVMInitializeNativeAsmPrinter();
	LLVMInitializeNativeAsmParser();

	before_shmem_exit(llvm_shutdown, 0);

	llvm_session_initialized = true;

	MemoryContextSwitchTo(oldcontext);
}

static void
llvm_shutdown(int code, Datum arg)
{
}
