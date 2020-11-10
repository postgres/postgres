/*-------------------------------------------------------------------------
 *
 * llvmjit.c
 *	  Core part of the LLVM JIT provider.
 *
 * Copyright (c) 2016-2020, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/jit/llvm/llvmjit.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <llvm-c/Analysis.h>
#include <llvm-c/BitReader.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#if LLVM_VERSION_MAJOR > 11
#include <llvm-c/Orc.h>
#include <llvm-c/OrcEE.h>
#include <llvm-c/LLJIT.h>
#else
#include <llvm-c/OrcBindings.h>
#endif
#include <llvm-c/Support.h>
#include <llvm-c/Target.h>
#include <llvm-c/Transforms/IPO.h>
#include <llvm-c/Transforms/PassManagerBuilder.h>
#include <llvm-c/Transforms/Scalar.h>
#if LLVM_VERSION_MAJOR > 6
#include <llvm-c/Transforms/Utils.h>
#endif

#include "jit/llvmjit.h"
#include "jit/llvmjit_emit.h"
#include "miscadmin.h"
#include "portability/instr_time.h"
#include "storage/ipc.h"
#include "utils/memutils.h"
#include "utils/resowner_private.h"

/* Handle of a module emitted via ORC JIT */
typedef struct LLVMJitHandle
{
#if LLVM_VERSION_MAJOR > 11
	LLVMOrcLLJITRef lljit;
	LLVMOrcResourceTrackerRef resource_tracker;
#else
	LLVMOrcJITStackRef stack;
	LLVMOrcModuleHandle orc_handle;
#endif
} LLVMJitHandle;


/* types & functions commonly needed for JITing */
LLVMTypeRef TypeSizeT;
LLVMTypeRef TypeParamBool;
LLVMTypeRef TypeStorageBool;
LLVMTypeRef TypePGFunction;
LLVMTypeRef StructNullableDatum;
LLVMTypeRef StructHeapTupleFieldsField3;
LLVMTypeRef StructHeapTupleFields;
LLVMTypeRef StructHeapTupleHeaderData;
LLVMTypeRef StructHeapTupleDataChoice;
LLVMTypeRef StructHeapTupleData;
LLVMTypeRef StructMinimalTupleData;
LLVMTypeRef StructItemPointerData;
LLVMTypeRef StructBlockId;
LLVMTypeRef StructFormPgAttribute;
LLVMTypeRef StructTupleConstr;
LLVMTypeRef StructTupleDescData;
LLVMTypeRef StructTupleTableSlot;
LLVMTypeRef StructHeapTupleTableSlot;
LLVMTypeRef StructMinimalTupleTableSlot;
LLVMTypeRef StructMemoryContextData;
LLVMTypeRef StructPGFinfoRecord;
LLVMTypeRef StructFmgrInfo;
LLVMTypeRef StructFunctionCallInfoData;
LLVMTypeRef StructExprContext;
LLVMTypeRef StructExprEvalStep;
LLVMTypeRef StructExprState;
LLVMTypeRef StructAggState;
LLVMTypeRef StructAggStatePerGroupData;
LLVMTypeRef StructAggStatePerTransData;

LLVMValueRef AttributeTemplate;

LLVMModuleRef llvm_types_module = NULL;

static bool llvm_session_initialized = false;
static size_t llvm_generation = 0;
static const char *llvm_triple = NULL;
static const char *llvm_layout = NULL;


static LLVMTargetRef llvm_targetref;
#if LLVM_VERSION_MAJOR > 11
static LLVMOrcThreadSafeContextRef llvm_ts_context;
static LLVMOrcLLJITRef llvm_opt0_orc;
static LLVMOrcLLJITRef llvm_opt3_orc;
#else							/* LLVM_VERSION_MAJOR > 11 */
static LLVMOrcJITStackRef llvm_opt0_orc;
static LLVMOrcJITStackRef llvm_opt3_orc;
#endif							/* LLVM_VERSION_MAJOR > 11 */


static void llvm_release_context(JitContext *context);
static void llvm_session_initialize(void);
static void llvm_shutdown(int code, Datum arg);
static void llvm_compile_module(LLVMJitContext *context);
static void llvm_optimize_module(LLVMJitContext *context, LLVMModuleRef module);

static void llvm_create_types(void);
static uint64_t llvm_resolve_symbol(const char *name, void *ctx);

#if LLVM_VERSION_MAJOR > 11
static LLVMOrcLLJITRef llvm_create_jit_instance(LLVMTargetMachineRef tm);
static char *llvm_error_message(LLVMErrorRef error);
#endif							/* LLVM_VERSION_MAJOR > 11 */

PG_MODULE_MAGIC;


/*
 * Initialize LLVM JIT provider.
 */
void
_PG_jit_provider_init(JitProviderCallbacks *cb)
{
	cb->reset_after_error = llvm_reset_after_error;
	cb->release_context = llvm_release_context;
	cb->compile_expr = llvm_compile_expr;
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
	LLVMJitContext *llvm_context = (LLVMJitContext *) context;

	llvm_enter_fatal_on_oom();

	/*
	 * When this backend is exiting, don't clean up LLVM. As an error might
	 * have occurred from within LLVM, we do not want to risk reentering. All
	 * resource cleanup is going to happen through process exit.
	 */
	if (proc_exit_inprogress)
		return;

	if (llvm_context->module)
	{
		LLVMDisposeModule(llvm_context->module);
		llvm_context->module = NULL;
	}

	while (llvm_context->handles != NIL)
	{
		LLVMJitHandle *jit_handle;

		jit_handle = (LLVMJitHandle *) linitial(llvm_context->handles);
		llvm_context->handles = list_delete_first(llvm_context->handles);

#if LLVM_VERSION_MAJOR > 11
		{
			LLVMOrcExecutionSessionRef ee;
			LLVMOrcSymbolStringPoolRef sp;

			LLVMOrcResourceTrackerRemove(jit_handle->resource_tracker);
			LLVMOrcReleaseResourceTracker(jit_handle->resource_tracker);

			/*
			 * Without triggering cleanup of the string pool, we'd leak
			 * memory. It'd be sufficient to do this far less often, but in
			 * experiments the required time was small enough to just always
			 * do it.
			 */
			ee = LLVMOrcLLJITGetExecutionSession(jit_handle->lljit);
			sp = LLVMOrcExecutionSessionGetSymbolStringPool(ee);
			LLVMOrcSymbolStringPoolClearDeadEntries(sp);
		}
#else							/* LLVM_VERSION_MAJOR > 11 */
		{
			LLVMOrcRemoveModule(jit_handle->stack, jit_handle->orc_handle);
		}
#endif							/* LLVM_VERSION_MAJOR > 11 */

		pfree(jit_handle);
	}
}

/*
 * Return module which may be modified, e.g. by creating new functions.
 */
LLVMModuleRef
llvm_mutable_module(LLVMJitContext *context)
{
	llvm_assert_in_fatal_section();

	/*
	 * If there's no in-progress module, create a new one.
	 */
	if (!context->module)
	{
		context->compiled = false;
		context->module_generation = llvm_generation++;
		context->module = LLVMModuleCreateWithName("pg");
		LLVMSetTarget(context->module, llvm_triple);
		LLVMSetDataLayout(context->module, llvm_layout);
	}

	return context->module;
}

/*
 * Expand function name to be non-conflicting. This should be used by code
 * generating code, when adding new externally visible function definitions to
 * a Module.
 */
char *
llvm_expand_funcname(struct LLVMJitContext *context, const char *basename)
{
	Assert(context->module != NULL);

	context->base.instr.created_functions++;

	/*
	 * Previously we used dots to separate, but turns out some tools, e.g.
	 * GDB, don't like that and truncate name.
	 */
	return psprintf("%s_%zu_%d",
					basename,
					context->module_generation,
					context->counter++);
}

/*
 * Return pointer to function funcname, which has to exist. If there's pending
 * code to be optimized and emitted, do so first.
 */
void *
llvm_get_function(LLVMJitContext *context, const char *funcname)
{
#if LLVM_VERSION_MAJOR > 11 || \
	defined(HAVE_DECL_LLVMORCGETSYMBOLADDRESSIN) && HAVE_DECL_LLVMORCGETSYMBOLADDRESSIN
	ListCell   *lc;
#endif

	llvm_assert_in_fatal_section();

	/*
	 * If there is a pending / not emitted module, compile and emit now.
	 * Otherwise we might not find the [correct] function.
	 */
	if (!context->compiled)
	{
		llvm_compile_module(context);
	}

	/*
	 * ORC's symbol table is of *unmangled* symbols. Therefore we don't need
	 * to mangle here.
	 */

#if LLVM_VERSION_MAJOR > 11
	foreach(lc, context->handles)
	{
		LLVMJitHandle *handle = (LLVMJitHandle *) lfirst(lc);
		instr_time	starttime;
		instr_time	endtime;
		LLVMErrorRef error;
		LLVMOrcJITTargetAddress addr;

		INSTR_TIME_SET_CURRENT(starttime);

		addr = 0;
		error = LLVMOrcLLJITLookup(handle->lljit, &addr, funcname);
		if (error)
			elog(ERROR, "failed to look up symbol \"%s\": %s",
				 funcname, llvm_error_message(error));

		/*
		 * LLJIT only actually emits code the first time a symbol is
		 * referenced. Thus add lookup time to emission time. That's counting
		 * a bit more than with older LLVM versions, but unlikely to ever
		 * matter.
		 */
		INSTR_TIME_SET_CURRENT(endtime);
		INSTR_TIME_ACCUM_DIFF(context->base.instr.emission_counter,
							  endtime, starttime);

		if (addr)
			return (void *) (uintptr_t) addr;
	}
#elif defined(HAVE_DECL_LLVMORCGETSYMBOLADDRESSIN) && HAVE_DECL_LLVMORCGETSYMBOLADDRESSIN
	foreach(lc, context->handles)
	{
		LLVMOrcTargetAddress addr;
		LLVMJitHandle *handle = (LLVMJitHandle *) lfirst(lc);

		addr = 0;
		if (LLVMOrcGetSymbolAddressIn(handle->stack, &addr, handle->orc_handle, funcname))
			elog(ERROR, "failed to look up symbol \"%s\"", funcname);
		if (addr)
			return (void *) (uintptr_t) addr;
	}
#elif LLVM_VERSION_MAJOR < 5
	{
		LLVMOrcTargetAddress addr;

		if ((addr = LLVMOrcGetSymbolAddress(llvm_opt0_orc, funcname)))
			return (void *) (uintptr_t) addr;
		if ((addr = LLVMOrcGetSymbolAddress(llvm_opt3_orc, funcname)))
			return (void *) (uintptr_t) addr;
	}
#else
	{
		LLVMOrcTargetAddress addr;

		if (LLVMOrcGetSymbolAddress(llvm_opt0_orc, &addr, funcname))
			elog(ERROR, "failed to look up symbol \"%s\"", funcname);
		if (addr)
			return (void *) (uintptr_t) addr;
		if (LLVMOrcGetSymbolAddress(llvm_opt3_orc, &addr, funcname))
			elog(ERROR, "failed to look up symbol \"%s\"", funcname);
		if (addr)
			return (void *) (uintptr_t) addr;
	}
#endif

	elog(ERROR, "failed to JIT: %s", funcname);

	return NULL;
}

/*
 * Return declaration for a function referenced in llvmjit_types.c, adding it
 * to the module if necessary.
 *
 * This is used to make functions discovered via llvm_create_types() known to
 * the module that's currently being worked on.
 */
LLVMValueRef
llvm_pg_func(LLVMModuleRef mod, const char *funcname)
{
	LLVMValueRef v_srcfn;
	LLVMValueRef v_fn;

	/* don't repeatedly add function */
	v_fn = LLVMGetNamedFunction(mod, funcname);
	if (v_fn)
		return v_fn;

	v_srcfn = LLVMGetNamedFunction(llvm_types_module, funcname);

	if (!v_srcfn)
		elog(ERROR, "function %s not in llvmjit_types.c", funcname);

	v_fn = LLVMAddFunction(mod,
						   funcname,
						   LLVMGetElementType(LLVMTypeOf(v_srcfn)));
	llvm_copy_attributes(v_srcfn, v_fn);

	return v_fn;
}

/*
 * Copy attributes from one function to another, for a specific index (an
 * index can reference return value, function and parameter attributes).
 */
static void
llvm_copy_attributes_at_index(LLVMValueRef v_from, LLVMValueRef v_to, uint32 index)
{
	int			num_attributes;
	LLVMAttributeRef *attrs;

	num_attributes = LLVMGetAttributeCountAtIndexPG(v_from, index);

	/*
	 * Not just for efficiency: LLVM <= 3.9 crashes when
	 * LLVMGetAttributesAtIndex() is called for an index with 0 attributes.
	 */
	if (num_attributes == 0)
		return;

	attrs = palloc(sizeof(LLVMAttributeRef) * num_attributes);
	LLVMGetAttributesAtIndex(v_from, index, attrs);

	for (int attno = 0; attno < num_attributes; attno++)
		LLVMAddAttributeAtIndex(v_to, index, attrs[attno]);

	pfree(attrs);
}

/*
 * Copy all attributes from one function to another. I.e. function, return and
 * parameters will be copied.
 */
void
llvm_copy_attributes(LLVMValueRef v_from, LLVMValueRef v_to)
{
	uint32		param_count;

	/* copy function attributes */
	llvm_copy_attributes_at_index(v_from, v_to, LLVMAttributeFunctionIndex);

	/* and the return value attributes */
	llvm_copy_attributes_at_index(v_from, v_to, LLVMAttributeReturnIndex);

	/* and each function parameter's attribute */
	param_count = LLVMCountParams(v_from);

	for (int paramidx = 1; paramidx <= param_count; paramidx++)
		llvm_copy_attributes_at_index(v_from, v_to, paramidx);
}

/*
 * Return a callable LLVMValueRef for fcinfo.
 */
LLVMValueRef
llvm_function_reference(LLVMJitContext *context,
						LLVMBuilderRef builder,
						LLVMModuleRef mod,
						FunctionCallInfo fcinfo)
{
	char	   *modname;
	char	   *basename;
	char	   *funcname;

	LLVMValueRef v_fn;

	fmgr_symbol(fcinfo->flinfo->fn_oid, &modname, &basename);

	if (modname != NULL && basename != NULL)
	{
		/* external function in loadable library */
		funcname = psprintf("pgextern.%s.%s", modname, basename);
	}
	else if (basename != NULL)
	{
		/* internal function */
		funcname = psprintf("%s", basename);
	}
	else
	{
		/*
		 * Function we don't know to handle, return pointer. We do so by
		 * creating a global constant containing a pointer to the function.
		 * Makes IR more readable.
		 */
		LLVMValueRef v_fn_addr;

		funcname = psprintf("pgoidextern.%u",
							fcinfo->flinfo->fn_oid);
		v_fn = LLVMGetNamedGlobal(mod, funcname);
		if (v_fn != 0)
			return LLVMBuildLoad(builder, v_fn, "");

		v_fn_addr = l_ptr_const(fcinfo->flinfo->fn_addr, TypePGFunction);

		v_fn = LLVMAddGlobal(mod, TypePGFunction, funcname);
		LLVMSetInitializer(v_fn, v_fn_addr);
		LLVMSetGlobalConstant(v_fn, true);
		LLVMSetLinkage(v_fn, LLVMPrivateLinkage);
		LLVMSetUnnamedAddr(v_fn, true);

		return LLVMBuildLoad(builder, v_fn, "");
	}

	/* check if function already has been added */
	v_fn = LLVMGetNamedFunction(mod, funcname);
	if (v_fn != 0)
		return v_fn;

	v_fn = LLVMAddFunction(mod, funcname, LLVMGetElementType(TypePGFunction));

	return v_fn;
}

/*
 * Optimize code in module using the flags set in context.
 */
static void
llvm_optimize_module(LLVMJitContext *context, LLVMModuleRef module)
{
	LLVMPassManagerBuilderRef llvm_pmb;
	LLVMPassManagerRef llvm_mpm;
	LLVMPassManagerRef llvm_fpm;
	LLVMValueRef func;
	int			compile_optlevel;

	if (context->base.flags & PGJIT_OPT3)
		compile_optlevel = 3;
	else
		compile_optlevel = 0;

	/*
	 * Have to create a new pass manager builder every pass through, as the
	 * inliner has some per-builder state. Otherwise one ends up only inlining
	 * a function the first time though.
	 */
	llvm_pmb = LLVMPassManagerBuilderCreate();
	LLVMPassManagerBuilderSetOptLevel(llvm_pmb, compile_optlevel);
	llvm_fpm = LLVMCreateFunctionPassManagerForModule(module);

	if (context->base.flags & PGJIT_OPT3)
	{
		/* TODO: Unscientifically determined threshold */
		LLVMPassManagerBuilderUseInlinerWithThreshold(llvm_pmb, 512);
	}
	else
	{
		/* we rely on mem2reg heavily, so emit even in the O0 case */
		LLVMAddPromoteMemoryToRegisterPass(llvm_fpm);
	}

	LLVMPassManagerBuilderPopulateFunctionPassManager(llvm_pmb, llvm_fpm);

	/*
	 * Do function level optimization. This could be moved to the point where
	 * functions are emitted, to reduce memory usage a bit.
	 */
	LLVMInitializeFunctionPassManager(llvm_fpm);
	for (func = LLVMGetFirstFunction(context->module);
		 func != NULL;
		 func = LLVMGetNextFunction(func))
		LLVMRunFunctionPassManager(llvm_fpm, func);
	LLVMFinalizeFunctionPassManager(llvm_fpm);
	LLVMDisposePassManager(llvm_fpm);

	/*
	 * Perform module level optimization. We do so even in the non-optimized
	 * case, so always-inline functions etc get inlined. It's cheap enough.
	 */
	llvm_mpm = LLVMCreatePassManager();
	LLVMPassManagerBuilderPopulateModulePassManager(llvm_pmb,
													llvm_mpm);
	/* always use always-inliner pass */
	if (!(context->base.flags & PGJIT_OPT3))
		LLVMAddAlwaysInlinerPass(llvm_mpm);
	/* if doing inlining, but no expensive optimization, add inlining pass */
	if (context->base.flags & PGJIT_INLINE
		&& !(context->base.flags & PGJIT_OPT3))
		LLVMAddFunctionInliningPass(llvm_mpm);
	LLVMRunPassManager(llvm_mpm, context->module);
	LLVMDisposePassManager(llvm_mpm);

	LLVMPassManagerBuilderDispose(llvm_pmb);
}

/*
 * Emit code for the currently pending module.
 */
static void
llvm_compile_module(LLVMJitContext *context)
{
	LLVMJitHandle *handle;
	MemoryContext oldcontext;
	instr_time	starttime;
	instr_time	endtime;
#if LLVM_VERSION_MAJOR > 11
	LLVMOrcLLJITRef compile_orc;
#else
	LLVMOrcJITStackRef compile_orc;
#endif

	if (context->base.flags & PGJIT_OPT3)
		compile_orc = llvm_opt3_orc;
	else
		compile_orc = llvm_opt0_orc;

	/* perform inlining */
	if (context->base.flags & PGJIT_INLINE)
	{
		INSTR_TIME_SET_CURRENT(starttime);
		llvm_inline(context->module);
		INSTR_TIME_SET_CURRENT(endtime);
		INSTR_TIME_ACCUM_DIFF(context->base.instr.inlining_counter,
							  endtime, starttime);
	}

	if (jit_dump_bitcode)
	{
		char	   *filename;

		filename = psprintf("%u.%zu.bc",
							MyProcPid,
							context->module_generation);
		LLVMWriteBitcodeToFile(context->module, filename);
		pfree(filename);
	}


	/* optimize according to the chosen optimization settings */
	INSTR_TIME_SET_CURRENT(starttime);
	llvm_optimize_module(context, context->module);
	INSTR_TIME_SET_CURRENT(endtime);
	INSTR_TIME_ACCUM_DIFF(context->base.instr.optimization_counter,
						  endtime, starttime);

	if (jit_dump_bitcode)
	{
		char	   *filename;

		filename = psprintf("%u.%zu.optimized.bc",
							MyProcPid,
							context->module_generation);
		LLVMWriteBitcodeToFile(context->module, filename);
		pfree(filename);
	}

	handle = (LLVMJitHandle *)
		MemoryContextAlloc(TopMemoryContext, sizeof(LLVMJitHandle));

	/*
	 * Emit the code. Note that this can, depending on the optimization
	 * settings, take noticeable resources as code emission executes low-level
	 * instruction combining/selection passes etc. Without optimization a
	 * faster instruction selection mechanism is used.
	 */
	INSTR_TIME_SET_CURRENT(starttime);
#if LLVM_VERSION_MAJOR > 11
	{
		LLVMOrcThreadSafeModuleRef ts_module;
		LLVMErrorRef error;
		LLVMOrcJITDylibRef jd = LLVMOrcLLJITGetMainJITDylib(compile_orc);

		ts_module = LLVMOrcCreateNewThreadSafeModule(context->module, llvm_ts_context);

		handle->lljit = compile_orc;
		handle->resource_tracker = LLVMOrcJITDylibCreateResourceTracker(jd);

		/*
		 * NB: This doesn't actually emit code. That happens lazily the first
		 * time a symbol defined in the module is requested. Due to that
		 * llvm_get_function() also accounts for emission time.
		 */

		context->module = NULL; /* will be owned by LLJIT */
		error = LLVMOrcLLJITAddLLVMIRModuleWithRT(compile_orc,
												  handle->resource_tracker,
												  ts_module);

		if (error)
			elog(ERROR, "failed to JIT module: %s",
				 llvm_error_message(error));

		handle->lljit = compile_orc;

		/* LLVMOrcLLJITAddLLVMIRModuleWithRT takes ownership of the module */
	}
#elif LLVM_VERSION_MAJOR > 6
	{
		handle->stack = compile_orc;
		if (LLVMOrcAddEagerlyCompiledIR(compile_orc, &handle->orc_handle, context->module,
										llvm_resolve_symbol, NULL))
			elog(ERROR, "failed to JIT module");

		/* LLVMOrcAddEagerlyCompiledIR takes ownership of the module */
	}
#elif LLVM_VERSION_MAJOR > 4
	{
		LLVMSharedModuleRef smod;

		smod = LLVMOrcMakeSharedModule(context->module);
		handle->stack = compile_orc;
		if (LLVMOrcAddEagerlyCompiledIR(compile_orc, &handle->orc_handle, smod,
										llvm_resolve_symbol, NULL))
			elog(ERROR, "failed to JIT module");

		LLVMOrcDisposeSharedModuleRef(smod);
	}
#else							/* LLVM 4.0 and 3.9 */
	{
		handle->stack = compile_orc;
		handle->orc_handle = LLVMOrcAddEagerlyCompiledIR(compile_orc, context->module,
														 llvm_resolve_symbol, NULL);

		LLVMDisposeModule(context->module);
	}
#endif

	INSTR_TIME_SET_CURRENT(endtime);
	INSTR_TIME_ACCUM_DIFF(context->base.instr.emission_counter,
						  endtime, starttime);

	context->module = NULL;
	context->compiled = true;

	/* remember emitted code for cleanup and lookups */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	context->handles = lappend(context->handles, handle);
	MemoryContextSwitchTo(oldcontext);

	ereport(DEBUG1,
			(errmsg("time to inline: %.3fs, opt: %.3fs, emit: %.3fs",
					INSTR_TIME_GET_DOUBLE(context->base.instr.inlining_counter),
					INSTR_TIME_GET_DOUBLE(context->base.instr.optimization_counter),
					INSTR_TIME_GET_DOUBLE(context->base.instr.emission_counter)),
			 errhidestmt(true),
			 errhidecontext(true)));
}

/*
 * Per session initialization.
 */
static void
llvm_session_initialize(void)
{
	MemoryContext oldcontext;
	char	   *error = NULL;
	char	   *cpu = NULL;
	char	   *features = NULL;
	LLVMTargetMachineRef opt0_tm;
	LLVMTargetMachineRef opt3_tm;

	if (llvm_session_initialized)
		return;

	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	LLVMInitializeNativeTarget();
	LLVMInitializeNativeAsmPrinter();
	LLVMInitializeNativeAsmParser();

	/*
	 * Synchronize types early, as that also includes inferring the target
	 * triple.
	 */
	llvm_create_types();

	if (LLVMGetTargetFromTriple(llvm_triple, &llvm_targetref, &error) != 0)
	{
		elog(FATAL, "failed to query triple %s\n", error);
	}

	/*
	 * We want the generated code to use all available features. Therefore
	 * grab the host CPU string and detect features of the current CPU. The
	 * latter is needed because some CPU architectures default to enabling
	 * features not all CPUs have (weird, huh).
	 */
	cpu = LLVMGetHostCPUName();
	features = LLVMGetHostCPUFeatures();
	elog(DEBUG2, "LLVMJIT detected CPU \"%s\", with features \"%s\"",
		 cpu, features);

	opt0_tm =
		LLVMCreateTargetMachine(llvm_targetref, llvm_triple, cpu, features,
								LLVMCodeGenLevelNone,
								LLVMRelocDefault,
								LLVMCodeModelJITDefault);
	opt3_tm =
		LLVMCreateTargetMachine(llvm_targetref, llvm_triple, cpu, features,
								LLVMCodeGenLevelAggressive,
								LLVMRelocDefault,
								LLVMCodeModelJITDefault);

	LLVMDisposeMessage(cpu);
	cpu = NULL;
	LLVMDisposeMessage(features);
	features = NULL;

	/* force symbols in main binary to be loaded */
	LLVMLoadLibraryPermanently(NULL);

#if LLVM_VERSION_MAJOR > 11
	{
		llvm_ts_context = LLVMOrcCreateNewThreadSafeContext();

		llvm_opt0_orc = llvm_create_jit_instance(opt0_tm);
		opt0_tm = 0;

		llvm_opt3_orc = llvm_create_jit_instance(opt3_tm);
		opt3_tm = 0;
	}
#else							/* LLVM_VERSION_MAJOR > 11 */
	{
		llvm_opt0_orc = LLVMOrcCreateInstance(opt0_tm);
		llvm_opt3_orc = LLVMOrcCreateInstance(opt3_tm);

#if defined(HAVE_DECL_LLVMCREATEGDBREGISTRATIONLISTENER) && HAVE_DECL_LLVMCREATEGDBREGISTRATIONLISTENER
		if (jit_debugging_support)
		{
			LLVMJITEventListenerRef l = LLVMCreateGDBRegistrationListener();

			LLVMOrcRegisterJITEventListener(llvm_opt0_orc, l);
			LLVMOrcRegisterJITEventListener(llvm_opt3_orc, l);
		}
#endif
#if defined(HAVE_DECL_LLVMCREATEPERFJITEVENTLISTENER) && HAVE_DECL_LLVMCREATEPERFJITEVENTLISTENER
		if (jit_profiling_support)
		{
			LLVMJITEventListenerRef l = LLVMCreatePerfJITEventListener();

			LLVMOrcRegisterJITEventListener(llvm_opt0_orc, l);
			LLVMOrcRegisterJITEventListener(llvm_opt3_orc, l);
		}
#endif
	}
#endif							/* LLVM_VERSION_MAJOR > 11 */

	on_proc_exit(llvm_shutdown, 0);

	llvm_session_initialized = true;

	MemoryContextSwitchTo(oldcontext);
}

static void
llvm_shutdown(int code, Datum arg)
{
#if LLVM_VERSION_MAJOR > 11
	{
		if (llvm_opt3_orc)
		{
			LLVMOrcDisposeLLJIT(llvm_opt3_orc);
			llvm_opt3_orc = NULL;
		}
		if (llvm_opt0_orc)
		{
			LLVMOrcDisposeLLJIT(llvm_opt0_orc);
			llvm_opt0_orc = NULL;
		}
		if (llvm_ts_context)
		{
			LLVMOrcDisposeThreadSafeContext(llvm_ts_context);
			llvm_ts_context = NULL;
		}
	}
#else							/* LLVM_VERSION_MAJOR > 11 */
	{
		/* unregister profiling support, needs to be flushed to be useful */

		if (llvm_opt3_orc)
		{
#if defined(HAVE_DECL_LLVMORCREGISTERPERF) && HAVE_DECL_LLVMORCREGISTERPERF
			if (jit_profiling_support)
				LLVMOrcUnregisterPerf(llvm_opt3_orc);
#endif
			LLVMOrcDisposeInstance(llvm_opt3_orc);
			llvm_opt3_orc = NULL;
		}

		if (llvm_opt0_orc)
		{
#if defined(HAVE_DECL_LLVMORCREGISTERPERF) && HAVE_DECL_LLVMORCREGISTERPERF
			if (jit_profiling_support)
				LLVMOrcUnregisterPerf(llvm_opt0_orc);
#endif
			LLVMOrcDisposeInstance(llvm_opt0_orc);
			llvm_opt0_orc = NULL;
		}
	}
#endif							/* LLVM_VERSION_MAJOR > 11 */
}

/* helper for llvm_create_types, returning a global var's type */
static LLVMTypeRef
load_type(LLVMModuleRef mod, const char *name)
{
	LLVMValueRef value;
	LLVMTypeRef typ;

	/* this'll return a *pointer* to the global */
	value = LLVMGetNamedGlobal(mod, name);
	if (!value)
		elog(ERROR, "type %s is unknown", name);

	/* therefore look at the contained type and return that */
	typ = LLVMTypeOf(value);
	Assert(typ != NULL);
	typ = LLVMGetElementType(typ);
	Assert(typ != NULL);
	return typ;
}

/* helper for llvm_create_types, returning a function's return type */
static LLVMTypeRef
load_return_type(LLVMModuleRef mod, const char *name)
{
	LLVMValueRef value;
	LLVMTypeRef typ;

	/* this'll return a *pointer* to the function */
	value = LLVMGetNamedFunction(mod, name);
	if (!value)
		elog(ERROR, "function %s is unknown", name);

	/* get type of function pointer */
	typ = LLVMTypeOf(value);
	Assert(typ != NULL);
	/* dereference pointer */
	typ = LLVMGetElementType(typ);
	Assert(typ != NULL);
	/* and look at return type */
	typ = LLVMGetReturnType(typ);
	Assert(typ != NULL);

	return typ;
}

/*
 * Load required information, types, function signatures from llvmjit_types.c
 * and make them available in global variables.
 *
 * Those global variables are then used while emitting code.
 */
static void
llvm_create_types(void)
{
	char		path[MAXPGPATH];
	LLVMMemoryBufferRef buf;
	char	   *msg;

	snprintf(path, MAXPGPATH, "%s/%s", pkglib_path, "llvmjit_types.bc");

	/* open file */
	if (LLVMCreateMemoryBufferWithContentsOfFile(path, &buf, &msg))
	{
		elog(ERROR, "LLVMCreateMemoryBufferWithContentsOfFile(%s) failed: %s",
			 path, msg);
	}

	/* eagerly load contents, going to need it all */
	if (LLVMParseBitcode2(buf, &llvm_types_module))
	{
		elog(ERROR, "LLVMParseBitcode2 of %s failed", path);
	}
	LLVMDisposeMemoryBuffer(buf);

	/*
	 * Load triple & layout from clang emitted file so we're guaranteed to be
	 * compatible.
	 */
	llvm_triple = pstrdup(LLVMGetTarget(llvm_types_module));
	llvm_layout = pstrdup(LLVMGetDataLayoutStr(llvm_types_module));

	TypeSizeT = load_type(llvm_types_module, "TypeSizeT");
	TypeParamBool = load_return_type(llvm_types_module, "FunctionReturningBool");
	TypeStorageBool = load_type(llvm_types_module, "TypeStorageBool");
	TypePGFunction = load_type(llvm_types_module, "TypePGFunction");
	StructNullableDatum = load_type(llvm_types_module, "StructNullableDatum");
	StructExprContext = load_type(llvm_types_module, "StructExprContext");
	StructExprEvalStep = load_type(llvm_types_module, "StructExprEvalStep");
	StructExprState = load_type(llvm_types_module, "StructExprState");
	StructFunctionCallInfoData = load_type(llvm_types_module, "StructFunctionCallInfoData");
	StructMemoryContextData = load_type(llvm_types_module, "StructMemoryContextData");
	StructTupleTableSlot = load_type(llvm_types_module, "StructTupleTableSlot");
	StructHeapTupleTableSlot = load_type(llvm_types_module, "StructHeapTupleTableSlot");
	StructMinimalTupleTableSlot = load_type(llvm_types_module, "StructMinimalTupleTableSlot");
	StructHeapTupleData = load_type(llvm_types_module, "StructHeapTupleData");
	StructTupleDescData = load_type(llvm_types_module, "StructTupleDescData");
	StructAggState = load_type(llvm_types_module, "StructAggState");
	StructAggStatePerGroupData = load_type(llvm_types_module, "StructAggStatePerGroupData");
	StructAggStatePerTransData = load_type(llvm_types_module, "StructAggStatePerTransData");

	AttributeTemplate = LLVMGetNamedFunction(llvm_types_module, "AttributeTemplate");
}

/*
 * Split a symbol into module / function parts.  If the function is in the
 * main binary (or an external library) *modname will be NULL.
 */
void
llvm_split_symbol_name(const char *name, char **modname, char **funcname)
{
	*modname = NULL;
	*funcname = NULL;

	/*
	 * Module function names are pgextern.$module.$funcname
	 */
	if (strncmp(name, "pgextern.", strlen("pgextern.")) == 0)
	{
		/*
		 * Symbol names cannot contain a ., therefore we can split based on
		 * first and last occurrence of one.
		 */
		*funcname = rindex(name, '.');
		(*funcname)++;			/* jump over . */

		*modname = pnstrdup(name + strlen("pgextern."),
							*funcname - name - strlen("pgextern.") - 1);
		Assert(funcname);

		*funcname = pstrdup(*funcname);
	}
	else
	{
		*modname = NULL;
		*funcname = pstrdup(name);
	}
}

/*
 * Attempt to resolve symbol, so LLVM can emit a reference to it.
 */
static uint64_t
llvm_resolve_symbol(const char *symname, void *ctx)
{
	uintptr_t	addr;
	char	   *funcname;
	char	   *modname;

	/*
	 * macOS prefixes all object level symbols with an underscore. But neither
	 * dlsym() nor PG's inliner expect that. So undo.
	 */
#if defined(__darwin__)
	if (symname[0] != '_')
		elog(ERROR, "expected prefixed symbol name, but got \"%s\"", symname);
	symname++;
#endif

	llvm_split_symbol_name(symname, &modname, &funcname);

	/* functions that aren't resolved to names shouldn't ever get here */
	Assert(funcname);

	if (modname)
		addr = (uintptr_t) load_external_function(modname, funcname,
												  true, NULL);
	else
		addr = (uintptr_t) LLVMSearchForAddressOfSymbol(symname);

	pfree(funcname);
	if (modname)
		pfree(modname);

	/* let LLVM will error out - should never happen */
	if (!addr)
		elog(WARNING, "failed to resolve name %s", symname);

	return (uint64_t) addr;
}

#if LLVM_VERSION_MAJOR > 11

static LLVMErrorRef
llvm_resolve_symbols(LLVMOrcDefinitionGeneratorRef GeneratorObj, void *Ctx,
					 LLVMOrcLookupStateRef *LookupState, LLVMOrcLookupKind Kind,
					 LLVMOrcJITDylibRef JD, LLVMOrcJITDylibLookupFlags JDLookupFlags,
					 LLVMOrcCLookupSet LookupSet, size_t LookupSetSize)
{
	LLVMOrcCSymbolMapPairs symbols = palloc0(sizeof(LLVMJITCSymbolMapPair) * LookupSetSize);
	LLVMErrorRef error;
	LLVMOrcMaterializationUnitRef mu;

	for (int i = 0; i < LookupSetSize; i++)
	{
		const char *name = LLVMOrcSymbolStringPoolEntryStr(LookupSet[i].Name);

		symbols[i].Name = LookupSet[i].Name;
		symbols[i].Sym.Address = llvm_resolve_symbol(name, NULL);
		symbols[i].Sym.Flags.GenericFlags = LLVMJITSymbolGenericFlagsExported;
	}

	mu = LLVMOrcAbsoluteSymbols(symbols, LookupSetSize);
	error = LLVMOrcJITDylibDefine(JD, mu);
	if (error != LLVMErrorSuccess)
		LLVMOrcDisposeMaterializationUnit(mu);

	pfree(symbols);

	return error;
}

/*
 * We cannot throw errors through LLVM (without causing a FATAL at least), so
 * just use WARNING here. That's OK anyway, as the error is also reported at
 * the top level action (with less detail) and there might be multiple
 * invocations of errors with details.
 *
 * This doesn't really happen during normal operation, but in cases like
 * symbol resolution breakage. So just using elog(WARNING) is fine.
 */
static void
llvm_log_jit_error(void *ctx, LLVMErrorRef error)
{
	elog(WARNING, "error during JITing: %s",
		 llvm_error_message(error));
}

/*
 * Create our own object layer, so we can add event listeners.
 */
static LLVMOrcObjectLayerRef
llvm_create_object_layer(void *Ctx, LLVMOrcExecutionSessionRef ES, const char *Triple)
{
	LLVMOrcObjectLayerRef objlayer =
	LLVMOrcCreateRTDyldObjectLinkingLayerWithSectionMemoryManager(ES);

#if defined(HAVE_DECL_LLVMCREATEGDBREGISTRATIONLISTENER) && HAVE_DECL_LLVMCREATEGDBREGISTRATIONLISTENER
	if (jit_debugging_support)
	{
		LLVMJITEventListenerRef l = LLVMCreateGDBRegistrationListener();

		LLVMOrcRTDyldObjectLinkingLayerRegisterJITEventListener(objlayer, l);
	}
#endif

#if defined(HAVE_DECL_LLVMCREATEPERFJITEVENTLISTENER) && HAVE_DECL_LLVMCREATEPERFJITEVENTLISTENER
	if (jit_profiling_support)
	{
		LLVMJITEventListenerRef l = LLVMCreatePerfJITEventListener();

		LLVMOrcRTDyldObjectLinkingLayerRegisterJITEventListener(objlayer, l);
	}
#endif

	return objlayer;
}

/*
 * Create LLJIT instance, using the passed in target machine. Note that the
 * target machine afterwards is owned by the LLJIT instance.
 */
static LLVMOrcLLJITRef
llvm_create_jit_instance(LLVMTargetMachineRef tm)
{
	LLVMOrcLLJITRef lljit;
	LLVMOrcJITTargetMachineBuilderRef tm_builder;
	LLVMOrcLLJITBuilderRef lljit_builder;
	LLVMErrorRef error;
	LLVMOrcDefinitionGeneratorRef main_gen;
	LLVMOrcDefinitionGeneratorRef ref_gen;

	lljit_builder = LLVMOrcCreateLLJITBuilder();
	tm_builder = LLVMOrcJITTargetMachineBuilderCreateFromTargetMachine(tm);
	LLVMOrcLLJITBuilderSetJITTargetMachineBuilder(lljit_builder, tm_builder);

	LLVMOrcLLJITBuilderSetObjectLinkingLayerCreator(lljit_builder,
													llvm_create_object_layer,
													NULL);

	error = LLVMOrcCreateLLJIT(&lljit, lljit_builder);
	if (error)
		elog(ERROR, "failed to create lljit instance: %s",
			 llvm_error_message(error));

	LLVMOrcExecutionSessionSetErrorReporter(LLVMOrcLLJITGetExecutionSession(lljit),
											llvm_log_jit_error, NULL);

	/*
	 * Symbol resolution support for symbols in the postgres binary /
	 * libraries already loaded.
	 */
	error = LLVMOrcCreateDynamicLibrarySearchGeneratorForProcess(&main_gen,
																 LLVMOrcLLJITGetGlobalPrefix(lljit),
																 0, NULL);
	if (error)
		elog(ERROR, "failed to create generator: %s",
			 llvm_error_message(error));
	LLVMOrcJITDylibAddGenerator(LLVMOrcLLJITGetMainJITDylib(lljit), main_gen);

	/*
	 * Symbol resolution support for "special" functions, e.g. a call into an
	 * SQL callable function.
	 */
	ref_gen = LLVMOrcCreateCustomCAPIDefinitionGenerator(llvm_resolve_symbols, NULL);
	LLVMOrcJITDylibAddGenerator(LLVMOrcLLJITGetMainJITDylib(lljit), ref_gen);

	return lljit;
}

static char *
llvm_error_message(LLVMErrorRef error)
{
	char	   *orig = LLVMGetErrorMessage(error);
	char	   *msg = pstrdup(orig);

	LLVMDisposeErrorMessage(orig);

	return msg;
}

#endif							/* LLVM_VERSION_MAJOR > 11 */
