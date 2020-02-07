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
#include <llvm-c/OrcBindings.h>
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
	LLVMOrcJITStackRef stack;
	LLVMOrcModuleHandle orc_handle;
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


static LLVMTargetMachineRef llvm_opt0_targetmachine;
static LLVMTargetMachineRef llvm_opt3_targetmachine;

static LLVMTargetRef llvm_targetref;
static LLVMOrcJITStackRef llvm_opt0_orc;
static LLVMOrcJITStackRef llvm_opt3_orc;


static void llvm_release_context(JitContext *context);
static void llvm_session_initialize(void);
static void llvm_shutdown(int code, Datum arg);
static void llvm_compile_module(LLVMJitContext *context);
static void llvm_optimize_module(LLVMJitContext *context, LLVMModuleRef module);

static void llvm_create_types(void);
static uint64_t llvm_resolve_symbol(const char *name, void *ctx);


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
	if (!proc_exit_inprogress)
	{
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

			LLVMOrcRemoveModule(jit_handle->stack, jit_handle->orc_handle);
			pfree(jit_handle);
		}
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
	LLVMOrcTargetAddress addr = 0;
#if defined(HAVE_DECL_LLVMORCGETSYMBOLADDRESSIN) && HAVE_DECL_LLVMORCGETSYMBOLADDRESSIN
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

#if defined(HAVE_DECL_LLVMORCGETSYMBOLADDRESSIN) && HAVE_DECL_LLVMORCGETSYMBOLADDRESSIN
	foreach(lc, context->handles)
	{
		LLVMJitHandle *handle = (LLVMJitHandle *) lfirst(lc);

		addr = 0;
		if (LLVMOrcGetSymbolAddressIn(handle->stack, &addr, handle->orc_handle, funcname))
			elog(ERROR, "failed to look up symbol \"%s\"", funcname);
		if (addr)
			return (void *) (uintptr_t) addr;
	}

#else

#if LLVM_VERSION_MAJOR < 5
	if ((addr = LLVMOrcGetSymbolAddress(llvm_opt0_orc, funcname)))
		return (void *) (uintptr_t) addr;
	if ((addr = LLVMOrcGetSymbolAddress(llvm_opt3_orc, funcname)))
		return (void *) (uintptr_t) addr;
#else
	if (LLVMOrcGetSymbolAddress(llvm_opt0_orc, &addr, funcname))
		elog(ERROR, "failed to look up symbol \"%s\"", funcname);
	if (addr)
		return (void *) (uintptr_t) addr;
	if (LLVMOrcGetSymbolAddress(llvm_opt3_orc, &addr, funcname))
		elog(ERROR, "failed to look up symbol \"%s\"", funcname);
	if (addr)
		return (void *) (uintptr_t) addr;
#endif							/* LLVM_VERSION_MAJOR */

#endif							/* HAVE_DECL_LLVMORCGETSYMBOLADDRESSIN */

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
 * Copy attributes from one function to another.
 */
void
llvm_copy_attributes(LLVMValueRef v_from, LLVMValueRef v_to)
{
	int			num_attributes;
	int			attno;
	LLVMAttributeRef *attrs;

	num_attributes =
		LLVMGetAttributeCountAtIndex(v_from, LLVMAttributeFunctionIndex);

	attrs = palloc(sizeof(LLVMAttributeRef) * num_attributes);
	LLVMGetAttributesAtIndex(v_from, LLVMAttributeFunctionIndex, attrs);

	for (attno = 0; attno < num_attributes; attno++)
	{
		LLVMAddAttributeAtIndex(v_to, LLVMAttributeFunctionIndex,
								attrs[attno]);
	}
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
	LLVMOrcModuleHandle orc_handle;
	MemoryContext oldcontext;
	static LLVMOrcJITStackRef compile_orc;
	instr_time	starttime;
	instr_time	endtime;

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

	/*
	 * Emit the code. Note that this can, depending on the optimization
	 * settings, take noticeable resources as code emission executes low-level
	 * instruction combining/selection passes etc. Without optimization a
	 * faster instruction selection mechanism is used.
	 */
	INSTR_TIME_SET_CURRENT(starttime);
#if LLVM_VERSION_MAJOR > 6
	{
		if (LLVMOrcAddEagerlyCompiledIR(compile_orc, &orc_handle, context->module,
										llvm_resolve_symbol, NULL))
		{
			elog(ERROR, "failed to JIT module");
		}

		/* LLVMOrcAddEagerlyCompiledIR takes ownership of the module */
	}
#elif LLVM_VERSION_MAJOR > 4
	{
		LLVMSharedModuleRef smod;

		smod = LLVMOrcMakeSharedModule(context->module);
		if (LLVMOrcAddEagerlyCompiledIR(compile_orc, &orc_handle, smod,
										llvm_resolve_symbol, NULL))
		{
			elog(ERROR, "failed to JIT module");
		}
		LLVMOrcDisposeSharedModuleRef(smod);
	}
#else							/* LLVM 4.0 and 3.9 */
	{
		orc_handle = LLVMOrcAddEagerlyCompiledIR(compile_orc, context->module,
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
	{
		LLVMJitHandle *handle;

		handle = (LLVMJitHandle *) palloc(sizeof(LLVMJitHandle));
		handle->stack = compile_orc;
		handle->orc_handle = orc_handle;

		context->handles = lappend(context->handles, handle);
	}
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

	llvm_opt0_targetmachine =
		LLVMCreateTargetMachine(llvm_targetref, llvm_triple, cpu, features,
								LLVMCodeGenLevelNone,
								LLVMRelocDefault,
								LLVMCodeModelJITDefault);
	llvm_opt3_targetmachine =
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

	llvm_opt0_orc = LLVMOrcCreateInstance(llvm_opt0_targetmachine);
	llvm_opt3_orc = LLVMOrcCreateInstance(llvm_opt3_targetmachine);

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

	before_shmem_exit(llvm_shutdown, 0);

	llvm_session_initialized = true;

	MemoryContextSwitchTo(oldcontext);
}

static void
llvm_shutdown(int code, Datum arg)
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
