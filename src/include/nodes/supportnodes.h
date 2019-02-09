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
struct SpecialJoinInfo;


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

/*
 * The Selectivity request allows the support function to provide a
 * selectivity estimate for a function appearing at top level of a WHERE
 * clause (so it applies only to functions returning boolean).
 *
 * The input arguments are the same as are supplied to operator restriction
 * and join estimators, except that we unify those two APIs into just one
 * request type.  See clause_selectivity() for the details.
 *
 * If an estimate can be made, store it into the "selectivity" field and
 * return the address of the SupportRequestSelectivity node; the estimate
 * must be between 0 and 1 inclusive.  Return NULL if no estimate can be
 * made (in which case the planner will fall back to a default estimate,
 * traditionally 1/3).
 *
 * If the target function is being used as the implementation of an operator,
 * the support function will not be used for this purpose; the operator's
 * restriction or join estimator is consulted instead.
 */
typedef struct SupportRequestSelectivity
{
	NodeTag		type;

	/* Input fields: */
	struct PlannerInfo *root;	/* Planner's infrastructure */
	Oid			funcid;			/* function we are inquiring about */
	List	   *args;			/* pre-simplified arguments to function */
	Oid			inputcollid;	/* function's input collation */
	bool		is_join;		/* is this a join or restriction case? */
	int			varRelid;		/* if restriction, RTI of target relation */
	JoinType	jointype;		/* if join, outer join type */
	struct SpecialJoinInfo *sjinfo; /* if outer join, info about join */

	/* Output fields: */
	Selectivity selectivity;	/* returned selectivity estimate */
} SupportRequestSelectivity;

/*
 * The Cost request allows the support function to provide an execution
 * cost estimate for its target function.  The cost estimate can include
 * both a one-time (query startup) component and a per-execution component.
 * The estimate should *not* include the costs of evaluating the target
 * function's arguments, only the target function itself.
 *
 * The "node" argument is normally the parse node that is invoking the
 * target function.  This is a FuncExpr in the simplest case, but it could
 * also be an OpExpr, DistinctExpr, NullIfExpr, or WindowFunc, or possibly
 * other cases in future.  NULL is passed if the function cannot presume
 * its arguments to be equivalent to what the calling node presents as
 * arguments; that happens for, e.g., aggregate support functions and
 * per-column comparison operators used by RowExprs.
 *
 * If an estimate can be made, store it into the cost fields and return the
 * address of the SupportRequestCost node.  Return NULL if no estimate can be
 * made, in which case the planner will rely on the target function's procost
 * field.  (Note: while procost is automatically scaled by cpu_operator_cost,
 * this is not the case for the outputs of the Cost request; the support
 * function must scale its results appropriately on its own.)
 */
typedef struct SupportRequestCost
{
	NodeTag		type;

	/* Input fields: */
	struct PlannerInfo *root;	/* Planner's infrastructure (could be NULL) */
	Oid			funcid;			/* function we are inquiring about */
	Node	   *node;			/* parse node invoking function, or NULL */

	/* Output fields: */
	Cost		startup;		/* one-time cost */
	Cost		per_tuple;		/* per-evaluation cost */
} SupportRequestCost;

/*
 * The Rows request allows the support function to provide an output rowcount
 * estimate for its target function (so it applies only to set-returning
 * functions).
 *
 * The "node" argument is the parse node that is invoking the target function;
 * currently this will always be a FuncExpr or OpExpr.
 *
 * If an estimate can be made, store it into the rows field and return the
 * address of the SupportRequestRows node.  Return NULL if no estimate can be
 * made, in which case the planner will rely on the target function's prorows
 * field.
 */
typedef struct SupportRequestRows
{
	NodeTag		type;

	/* Input fields: */
	struct PlannerInfo *root;	/* Planner's infrastructure (could be NULL) */
	Oid			funcid;			/* function we are inquiring about */
	Node	   *node;			/* parse node invoking function */

	/* Output fields: */
	double		rows;			/* number of rows expected to be returned */
} SupportRequestRows;

#endif							/* SUPPORTNODES_H */
