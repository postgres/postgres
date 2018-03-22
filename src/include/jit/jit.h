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

#include "utils/resowner.h"


typedef struct JitContext
{
	int			flags;

	ResourceOwner resowner;
} JitContext;

typedef struct JitProviderCallbacks JitProviderCallbacks;

extern void _PG_jit_provider_init(JitProviderCallbacks *cb);
typedef void (*JitProviderInit) (JitProviderCallbacks *cb);
typedef void (*JitProviderResetAfterErrorCB) (void);
typedef void (*JitProviderReleaseContextCB) (JitContext *context);

struct JitProviderCallbacks
{
	JitProviderResetAfterErrorCB reset_after_error;
	JitProviderReleaseContextCB release_context;
};


/* GUCs */
extern bool jit_enabled;
extern char *jit_provider;


extern void jit_reset_after_error(void);
extern void jit_release_context(JitContext *context);

#endif							/* JIT_H */
