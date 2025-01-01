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
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/supportnodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SUPPORTNODES_H
#define SUPPORTNODES_H

#include "nodes/plannodes.h"

struct PlannerInfo;				/* avoid including pathnodes.h here */
struct IndexOptInfo;
struct SpecialJoinInfo;
struct WindowClause;

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

/*
 * The IndexCondition request allows the support function to generate
 * a directly-indexable condition based on a target function call that is
 * not itself indexable.  The target function call must appear at the top
 * level of WHERE or JOIN/ON, so this applies only to functions returning
 * boolean.
 *
 * The "node" argument is the parse node that is invoking the target function;
 * currently this will always be a FuncExpr or OpExpr.  The call is made
 * only if at least one function argument matches an index column's variable
 * or expression.  "indexarg" identifies the matching argument (it's the
 * argument's zero-based index in the node's args list).
 *
 * If the transformation is possible, return a List of directly-indexable
 * condition expressions, else return NULL.  (A List is used because it's
 * sometimes useful to generate more than one indexable condition, such as
 * when a LIKE with constant prefix gives rise to both >= and < conditions.)
 *
 * "Directly indexable" means that the condition must be directly executable
 * by the index machinery.  Typically this means that it is a binary OpExpr
 * with the index column value on the left, a pseudo-constant on the right,
 * and an operator that is in the index column's operator family.  Other
 * possibilities include RowCompareExpr, ScalarArrayOpExpr, and NullTest,
 * depending on the index type; but those seem less likely to be useful for
 * derived index conditions.  "Pseudo-constant" means that the right-hand
 * expression must not contain any volatile functions, nor any Vars of the
 * table the index is for; use is_pseudo_constant_for_index() to check this.
 * (Note: if the passed "node" is an OpExpr, the core planner already verified
 * that the non-indexkey operand is pseudo-constant; but when the "node"
 * is a FuncExpr, it does not check, since it doesn't know which of the
 * function's arguments you might need to use in an index comparison value.)
 *
 * In many cases, an index condition can be generated but it is weaker than
 * the function condition itself; for example, a LIKE with a constant prefix
 * can produce an index range check based on the prefix, but we still need
 * to execute the LIKE operator to verify the rest of the pattern.  We say
 * that such an index condition is "lossy".  When returning an index condition,
 * you should set the "lossy" request field to true if the condition is lossy,
 * or false if it is an exact equivalent of the function's result.  The core
 * code will initialize that field to true, which is the common case.
 *
 * It is important to verify that the index operator family is the correct
 * one for the condition you want to generate.  Core support functions tend
 * to use the known OID of a built-in opfamily for this, but extensions need
 * to work harder, since their OIDs aren't fixed.  A possibly workable
 * answer for an index on an extension datatype is to verify the index AM's
 * OID instead, and then assume that there's only one relevant opclass for
 * your datatype so the opfamily must be the right one.  Generating OpExpr
 * nodes may also require knowing extension datatype OIDs (often you can
 * find these out by applying exprType() to a function argument) and
 * operator OIDs (which you can look up using get_opfamily_member).
 */
typedef struct SupportRequestIndexCondition
{
	NodeTag		type;

	/* Input fields: */
	struct PlannerInfo *root;	/* Planner's infrastructure */
	Oid			funcid;			/* function we are inquiring about */
	Node	   *node;			/* parse node invoking function */
	int			indexarg;		/* index of function arg matching indexcol */
	struct IndexOptInfo *index; /* planner's info about target index */
	int			indexcol;		/* index of target index column (0-based) */
	Oid			opfamily;		/* index column's operator family */
	Oid			indexcollation; /* index column's collation */

	/* Output fields: */
	bool		lossy;			/* set to false if index condition is an exact
								 * equivalent of the function call */
} SupportRequestIndexCondition;

/* ----------
 * To support more efficient query execution of any monotonically increasing
 * and/or monotonically decreasing window functions, we support calling the
 * window function's prosupport function passing along this struct whenever
 * the planner sees an OpExpr qual directly reference a window function in a
 * subquery.  When the planner encounters this, we populate this struct and
 * pass it along to the window function's prosupport function so that it can
 * evaluate if the given WindowFunc is;
 *
 * a) monotonically increasing, or
 * b) monotonically decreasing, or
 * c) both monotonically increasing and decreasing, or
 * d) none of the above.
 *
 * A function that is monotonically increasing can never return a value that
 * is lower than a value returned in a "previous call".  A monotonically
 * decreasing function can never return a value higher than a value returned
 * in a previous call.  A function that is both must return the same value
 * each time.
 *
 * We define "previous call" to mean a previous call to the same WindowFunc
 * struct in the same window partition.
 *
 * row_number() is an example of a monotonically increasing function.  The
 * return value will be reset back to 1 in each new partition.  An example of
 * a monotonically increasing and decreasing function is COUNT(*) OVER ().
 * Since there is no ORDER BY clause in this example, all rows in the
 * partition are peers and all rows within the partition will be within the
 * frame bound.  Likewise for COUNT(*) OVER(ORDER BY a ROWS BETWEEN UNBOUNDED
 * PRECEDING AND UNBOUNDED FOLLOWING).
 *
 * COUNT(*) OVER (ORDER BY a ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
 * is an example of a monotonically decreasing function.
 *
 * Implementations must only concern themselves with the given WindowFunc
 * being monotonic in a single partition.
 *
 * Inputs:
 *	'window_func' is the pointer to the window function being called.
 *
 *	'window_clause' pointer to the WindowClause data.  Support functions can
 *	use this to check frame bounds, etc.
 *
 * Outputs:
 *	'monotonic' the resulting MonotonicFunction value for the given input
 *	window function and window clause.
 * ----------
 */
typedef struct SupportRequestWFuncMonotonic
{
	NodeTag		type;

	/* Input fields: */
	WindowFunc *window_func;	/* Pointer to the window function data */
	struct WindowClause *window_clause; /* Pointer to the window clause data */

	/* Output fields: */
	MonotonicFunction monotonic;
} SupportRequestWFuncMonotonic;

/*
 * Some WindowFunc behavior might not be affected by certain variations in
 * the WindowClause's frameOptions.  For example, row_number() is coded in
 * such a way that the frame options don't change the returned row number.
 * nodeWindowAgg.c will have less work to do if the ROWS option is used
 * instead of the RANGE option as no check needs to be done for peer rows.
 * Since RANGE is included in the default frame options, window functions
 * such as row_number() might want to change that to ROW.
 *
 * Here we allow a WindowFunc's support function to determine which, if
 * anything, can be changed about the WindowClause which the WindowFunc
 * belongs to.  Currently only the frameOptions can be modified.  However,
 * we may want to allow more optimizations in the future.
 *
 * The support function is responsible for ensuring the optimized version of
 * the frameOptions doesn't affect the result of the window function.  The
 * planner is responsible for only changing the frame options when all
 * WindowFuncs using this particular WindowClause agree on what the optimized
 * version of the frameOptions are.  If a particular WindowFunc being used
 * does not have a support function then the planner will not make any changes
 * to the WindowClause's frameOptions.
 *
 * 'window_func' and 'window_clause' are set by the planner before calling the
 * support function so that the support function has these fields available.
 * These may be required in order to determine which optimizations are
 * possible.
 *
 * 'frameOptions' is set by the planner to WindowClause.frameOptions.  The
 * support function must only adjust this if optimizations are possible for
 * the given WindowFunc.
 */
typedef struct SupportRequestOptimizeWindowClause
{
	NodeTag		type;

	/* Input fields: */
	WindowFunc *window_func;	/* Pointer to the window function data */
	struct WindowClause *window_clause; /* Pointer to the window clause data */

	/* Input/Output fields: */
	int			frameOptions;	/* New frameOptions, or left untouched if no
								 * optimizations are possible. */
} SupportRequestOptimizeWindowClause;

#endif							/* SUPPORTNODES_H */
