/*-------------------------------------------------------------------------
 *
 * subscripting.h
 *		API for generic type subscripting
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/subscripting.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SUBSCRIPTING_H
#define SUBSCRIPTING_H

#include "nodes/primnodes.h"

/* Forward declarations, to avoid including other headers */
struct ParseState;
struct SubscriptingRefState;
struct SubscriptExecSteps;

/*
 * The SQL-visible function that defines a subscripting method is declared
 *		subscripting_function(internal) returns internal
 * but it actually is not passed any parameter.  It must return a pointer
 * to a "struct SubscriptRoutines" that provides pointers to the individual
 * subscript parsing and execution methods.  Typically the pointer will point
 * to a "static const" variable, but at need it can point to palloc'd space.
 * The type (after domain-flattening) of the head variable or expression
 * of a subscripting construct determines which subscripting function is
 * called for that construct.
 *
 * In addition to the method pointers, struct SubscriptRoutines includes
 * several bool flags that specify properties of the subscripting actions
 * this data type can perform:
 *
 * fetch_strict indicates that a fetch SubscriptRef is strict, i.e., returns
 * NULL if any input (either the container or any subscript) is NULL.
 *
 * fetch_leakproof indicates that a fetch SubscriptRef is leakproof, i.e.,
 * will not throw any data-value-dependent errors.  Typically this requires
 * silently returning NULL for invalid subscripts.
 *
 * store_leakproof similarly indicates whether an assignment SubscriptRef is
 * leakproof.  (It is common to prefer throwing errors for invalid subscripts
 * in assignments; that's fine, but it makes the operation not leakproof.
 * In current usage there is no advantage in making assignments leakproof.)
 *
 * There is no store_strict flag.  Such behavior would generally be
 * undesirable, since for example a null subscript in an assignment would
 * cause the entire container to become NULL.
 *
 * Regardless of these flags, all SubscriptRefs are expected to be immutable,
 * that is they must always give the same results for the same inputs.
 * They are expected to always be parallel-safe, as well.
 */

/*
 * The transform method is called during parse analysis of a subscripting
 * construct.  The SubscriptingRef node has been constructed, but some of
 * its fields still need to be filled in, and the subscript expression(s)
 * are still in raw form.  The transform method is responsible for doing
 * parse analysis of each subscript expression (using transformExpr),
 * coercing the subscripts to whatever type it needs, and building the
 * refupperindexpr and reflowerindexpr lists from those results.  The
 * reflowerindexpr list must be empty for an element operation, or the
 * same length as refupperindexpr for a slice operation.  Insert NULLs
 * (that is, an empty parse tree, not a null Const node) for any omitted
 * subscripts in a slice operation.  (Of course, if the transform method
 * does not care to support slicing, it can just throw an error if isSlice.)
 * See array_subscript_transform() for sample code.
 *
 * The transform method is also responsible for identifying the result type
 * of the subscripting operation.  At call, refcontainertype and reftypmod
 * describe the container type (this will be a base type not a domain), and
 * refelemtype is set to the container type's pg_type.typelem value.  The
 * transform method must set refrestype and reftypmod to describe the result
 * of subscripting.  For arrays, refrestype is set to refelemtype for an
 * element operation or refcontainertype for a slice, while reftypmod stays
 * the same in either case; but other types might use other rules.  The
 * transform method should ignore refcollid, as that's determined later on
 * during parsing.
 *
 * At call, refassgnexpr has not been filled in, so the SubscriptingRef node
 * always looks like a fetch; refrestype should be set as though for a
 * fetch, too.  (The isAssignment parameter is typically only useful if the
 * transform method wishes to throw an error for not supporting assignment.)
 * To complete processing of an assignment, the core parser will coerce the
 * element/slice source expression to the returned refrestype and reftypmod
 * before putting it into refassgnexpr.  It will then set refrestype and
 * reftypmod to again describe the container type, since that's what an
 * assignment must return.
 */
typedef void (*SubscriptTransform) (SubscriptingRef *sbsref,
									List *indirection,
									struct ParseState *pstate,
									bool isSlice,
									bool isAssignment);

/*
 * The exec_setup method is called during executor-startup compilation of a
 * SubscriptingRef node in an expression.  It must fill *methods with pointers
 * to functions that can be called for execution of the node.  Optionally,
 * exec_setup can initialize sbsrefstate->workspace to point to some palloc'd
 * workspace for execution.  (Typically, such workspace is used to hold
 * looked-up catalog data and/or provide space for the check_subscripts step
 * to pass data forward to the other step functions.)  See executor/execExpr.h
 * for the definitions of these structs and other ones used in expression
 * execution.
 *
 * The methods to be provided are:
 *
 * sbs_check_subscripts: examine the just-computed subscript values available
 * in sbsrefstate's arrays, and possibly convert them into another form
 * (stored in sbsrefstate->workspace).  Return TRUE to continue with
 * evaluation of the subscripting construct, or FALSE to skip it and return an
 * overall NULL result.  If this is a fetch and the data type's fetch_strict
 * flag is true, then sbs_check_subscripts must return FALSE if there are any
 * NULL subscripts.  Otherwise it can choose to throw an error, or return
 * FALSE, or let sbs_fetch or sbs_assign deal with the null subscripts.
 *
 * sbs_fetch: perform a subscripting fetch, using the container value in
 * *op->resvalue and the subscripts from sbs_check_subscripts.  If
 * fetch_strict is true then all these inputs can be assumed non-NULL,
 * otherwise sbs_fetch must check for null inputs.  Place the result in
 * *op->resvalue / *op->resnull.
 *
 * sbs_assign: perform a subscripting assignment, using the original
 * container value in *op->resvalue / *op->resnull, the subscripts from
 * sbs_check_subscripts, and the new element/slice value in
 * sbsrefstate->replacevalue/replacenull.  Any of these inputs might be NULL
 * (unless sbs_check_subscripts rejected null subscripts).  Place the result
 * (an entire new container value) in *op->resvalue / *op->resnull.
 *
 * sbs_fetch_old: this is only used in cases where an element or slice
 * assignment involves an assignment to a sub-field or sub-element
 * (i.e., nested containers are involved).  It must fetch the existing
 * value of the target element or slice.  This is exactly the same as
 * sbs_fetch except that (a) it must cope with a NULL container, and
 * with NULL subscripts if sbs_check_subscripts allows them (typically,
 * returning NULL is good enough); and (b) the result must be placed in
 * sbsrefstate->prevvalue/prevnull, without overwriting *op->resvalue.
 *
 * Subscripting implementations that do not support assignment need not
 * provide sbs_assign or sbs_fetch_old methods.  It might be reasonable
 * to also omit sbs_check_subscripts, in which case the sbs_fetch method must
 * combine the functionality of sbs_check_subscripts and sbs_fetch.  (The
 * main reason to have a separate sbs_check_subscripts method is so that
 * sbs_fetch_old and sbs_assign need not duplicate subscript processing.)
 * Set the relevant pointers to NULL for any omitted methods.
 */
typedef void (*SubscriptExecSetup) (const SubscriptingRef *sbsref,
									struct SubscriptingRefState *sbsrefstate,
									struct SubscriptExecSteps *methods);

/* Struct returned by the SQL-visible subscript handler function */
typedef struct SubscriptRoutines
{
	SubscriptTransform transform;	/* parse analysis function */
	SubscriptExecSetup exec_setup;	/* expression compilation function */
	bool		fetch_strict;	/* is fetch SubscriptRef strict? */
	bool		fetch_leakproof;	/* is fetch SubscriptRef leakproof? */
	bool		store_leakproof;	/* is assignment SubscriptRef leakproof? */
} SubscriptRoutines;

#endif							/* SUBSCRIPTING_H */
