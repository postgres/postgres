/*-------------------------------------------------------------------------
 *
 * supportnodes.h
 *	  Definitions for planner support functions.
 *
 * This file defines the API for "planner support functions", which
 * are SQL functions (normally written in C) that can be attached to
 * another "target" function to give the system additional knowledge
 * about the target function.  All the current capabilities have to do
 * with planning queries that use the target function, though it is
 * possible that future extensions will add functionality to be invoked
 * by the parser or executor.
 *
 * A support function must have the SQL signature
 *		supportfn(internal) returns internal
 * The argument is a pointer to one of the Node types defined in this file.
 * The result is usually also a Node pointer, though its type depends on
 * which capability is being invoked.  In all cases, a NULL pointer result
 * (that's PG_RETURN_POINTER(NULL), not PG_RETURN_NULL()) indicates that
 * the support function cannot do anything useful for the given request.
 * Support functions must return a NULL pointer, not fail, if they do not
 * recognize the request node type or cannot handle the given case; this
 * allows for future extensions of the set of request cases.
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/supportnodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SUPPORTNODES_H
#define SUPPORTNODES_H

#include "nodes/primnodes.h"

struct PlannerInfo;				/* avoid including relation.h here */


/*
 * The Simplify request allows the support function to perform plan-time
 * simplification of a call to its target function.  For example, a varchar
 * length coercion that does not decrease the allowed length of its argument
 * could be replaced by a RelabelType node, or "x + 0" could be replaced by
 * "x".  This is invoked during the planner's constant-folding pass, so the
 * function's arguments can be presumed already simplified.
 *
 * The planner's PlannerInfo "root" is typically not needed, but can be
 * consulted if it's necessary to obtain info about Vars present in
 * the given node tree.  Beware that root could be NULL in some usages.
 *
 * "fcall" will be a FuncExpr invoking the support function's target
 * function.  (This is true even if the original parsetree node was an
 * operator call; a FuncExpr is synthesized for this purpose.)
 *
 * The result should be a semantically-equivalent transformed node tree,
 * or NULL if no simplification could be performed.  Do *not* return or
 * modify *fcall, as it isn't really a separately allocated Node.  But
 * it's okay to use fcall->args, or parts of it, in the result tree.
 */
typedef struct SupportRequestSimplify
{
	NodeTag		type;

	struct PlannerInfo *root;	/* Planner's infrastructure */
	FuncExpr   *fcall;			/* Function call to be simplified */
} SupportRequestSimplify;

#endif							/* SUPPORTNODES_H */
