/*-------------------------------------------------------------------------
 * jit.h
 *	  Provider independent JIT infrastructure.
 *
 * Copyright (c) 2016-2018, PostgreSQL Global Development Group
 *
 * src/include/jit/jit.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef JIT_H
#define JIT_H

#include "executor/instrument.h"
#include "utils/resowner.h"


/* Flags deterimining what kind of JIT operations to perform */
#define PGJIT_NONE     0
#define PGJIT_PERFORM  1 << 0
#define PGJIT_OPT3     1 << 1
/* reserved for PGJIT_INLINE */
#define PGJIT_EXPR	   1 << 3


typedef struct JitContext
{
	/* see PGJIT_* above */
	int			flags;

	ResourceOwner resowner;

	/* number of emitted functions */
	size_t		created_functions;

	/* accumulated time to generate code */
	instr_time	generation_counter;

	/* accumulated time for optimization */
	instr_time	optimization_counter;

	/* accumulated time for code emission */
	instr_time	emission_counter;
} JitContext;

typedef struct JitProviderCallbacks JitProviderCallbacks;

extern void _PG_jit_provider_init(JitProviderCallbacks *cb);
typedef void (*JitProviderInit) (JitProviderCallbacks *cb);
typedef void (*JitProviderResetAfterErrorCB) (void);
typedef void (*JitProviderReleaseContextCB) (JitContext *context);
struct ExprState;
typedef bool (*JitProviderCompileExprCB) (struct ExprState *state);

struct JitProviderCallbacks
{
	JitProviderResetAfterErrorCB reset_after_error;
	JitProviderReleaseContextCB release_context;
	JitProviderCompileExprCB compile_expr;
};


/* GUCs */
extern bool jit_enabled;
extern char *jit_provider;
extern bool jit_debugging_support;
extern bool jit_dump_bitcode;
extern bool jit_expressions;
extern bool jit_profiling_support;
extern double jit_above_cost;
extern double jit_optimize_above_cost;


extern void jit_reset_after_error(void);
extern void jit_release_context(JitContext *context);

/*
 * Functions for attempting to JIT code. Callers must accept that these might
 * not be able to perform JIT (i.e. return false).
 */
extern bool jit_compile_expr(struct ExprState *state);


#endif							/* JIT_H */
