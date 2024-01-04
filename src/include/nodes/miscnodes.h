/*-------------------------------------------------------------------------
 *
 * miscnodes.h
 *	  Definitions for hard-to-classify node types.
 *
 * Node types declared here are not part of parse trees, plan trees,
 * or execution state trees.  We only assign them NodeTag values because
 * IsA() tests provide a convenient way to disambiguate what kind of
 * structure is being passed through assorted APIs, such as function
 * "context" pointers.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/miscnodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MISCNODES_H
#define MISCNODES_H

#include "nodes/nodes.h"

/*
 * ErrorSaveContext -
 *		function call context node for handling of "soft" errors
 *
 * A caller wishing to trap soft errors must initialize a struct like this
 * with all fields zero/NULL except for the NodeTag.  Optionally, set
 * details_wanted = true if more than the bare knowledge that a soft error
 * occurred is required.  The struct is then passed to a SQL-callable function
 * via the FunctionCallInfo.context field; or below the level of SQL calls,
 * it could be passed to a subroutine directly.
 *
 * After calling code that might report an error this way, check
 * error_occurred to see if an error happened.  If so, and if details_wanted
 * is true, error_data has been filled with error details (stored in the
 * callee's memory context!).  FreeErrorData() can be called to release
 * error_data, although that step is typically not necessary if the called
 * code was run in a short-lived context.
 */
typedef struct ErrorSaveContext
{
	NodeTag		type;
	bool		error_occurred; /* set to true if we detect a soft error */
	bool		details_wanted; /* does caller want more info than that? */
	ErrorData  *error_data;		/* details of error, if so */
} ErrorSaveContext;

/* Often-useful macro for checking if a soft error was reported */
#define SOFT_ERROR_OCCURRED(escontext) \
	((escontext) != NULL && IsA(escontext, ErrorSaveContext) && \
	 ((ErrorSaveContext *) (escontext))->error_occurred)

#endif							/* MISCNODES_H */
