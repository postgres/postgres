/*-------------------------------------------------------------------------
 *
 * llvmjit_error.cpp
 *	  LLVM error related handling that requires interfacing with C++
 *
 * Unfortunately neither (re)setting the C++ new handler, nor the LLVM OOM
 * handler are exposed to C. Therefore this file wraps the necessary code.
 *
 * Copyright (c) 2016-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/jit/llvm/llvmjit_error.cpp
 *
 *-------------------------------------------------------------------------
 */

extern "C"
{
#include "postgres.h"
}

#include <llvm/Support/ErrorHandling.h>

#include "jit/llvmjit.h"

#include <new>

static int fatal_new_handler_depth = 0;
static std::new_handler old_new_handler = NULL;

static void fatal_system_new_handler(void);
static void fatal_llvm_new_handler(void *user_data, const char *reason, bool gen_crash_diag);
static void fatal_llvm_error_handler(void *user_data, const char *reason, bool gen_crash_diag);


/*
 * Enter a section in which C++ and LLVM errors are treated as FATAL errors.
 *
 * This is necessary for LLVM as LLVM's error handling for such cases
 * (exit()ing, throwing std::bad_alloc() if compiled with exceptions, abort())
 * isn't compatible with postgres error handling.  Thus in sections where LLVM
 * code, not LLVM generated functions!, is executing, standard new, LLVM OOM
 * and LLVM fatal errors (some OOM errors masquerade as those) are redirected
 * to our own error handlers.
 *
 * These error handlers use FATAL, because there's no reliable way from within
 * LLVM to throw an error that's guaranteed not to corrupt LLVM's state.
 *
 * To avoid disturbing extensions using C++ and/or LLVM, these handlers are
 * unset when not executing LLVM code. There is no need to call
 * llvm_leave_fatal_on_oom() when ERRORing out, error recovery resets the
 * handlers in that case.
 */
void
llvm_enter_fatal_on_oom(void)
{
	if (fatal_new_handler_depth == 0)
	{
		old_new_handler = std::set_new_handler(fatal_system_new_handler);
		llvm::install_bad_alloc_error_handler(fatal_llvm_new_handler);
		llvm::install_fatal_error_handler(fatal_llvm_error_handler);
	}
	fatal_new_handler_depth++;
}

/*
 * Leave fatal error section started with llvm_enter_fatal_on_oom().
 */
void
llvm_leave_fatal_on_oom(void)
{
	fatal_new_handler_depth--;
	if (fatal_new_handler_depth == 0)
	{
		std::set_new_handler(old_new_handler);
		llvm::remove_bad_alloc_error_handler();
		llvm::remove_fatal_error_handler();
	}
}

/*
 * Are we currently in a fatal-on-oom section? Useful to skip cleanup in case
 * of errors.
 */
bool
llvm_in_fatal_on_oom(void)
{
	return fatal_new_handler_depth > 0;
}

/*
 * Reset fatal error handling. This should only be called in error recovery
 * loops like PostgresMain()'s.
 */
void
llvm_reset_after_error(void)
{
	if (fatal_new_handler_depth != 0)
	{
		std::set_new_handler(old_new_handler);
		llvm::remove_bad_alloc_error_handler();
		llvm::remove_fatal_error_handler();
	}
	fatal_new_handler_depth = 0;
}

void
llvm_assert_in_fatal_section(void)
{
	Assert(fatal_new_handler_depth > 0);
}

static void
fatal_system_new_handler(void)
{
	ereport(FATAL,
			(errcode(ERRCODE_OUT_OF_MEMORY),
			 errmsg("out of memory"),
			 errdetail("while in LLVM")));
}

static void
fatal_llvm_new_handler(void *user_data,
					   const char *reason,
					   bool gen_crash_diag)
{
	ereport(FATAL,
			(errcode(ERRCODE_OUT_OF_MEMORY),
			 errmsg("out of memory"),
			 errdetail("While in LLVM: %s", reason)));
}

static void
fatal_llvm_error_handler(void *user_data,
						 const char *reason,
						 bool gen_crash_diag)
{
	ereport(FATAL,
			(errcode(ERRCODE_OUT_OF_MEMORY),
			 errmsg("fatal llvm error: %s", reason)));
}
