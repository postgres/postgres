/*-------------------------------------------------------------------------
 *
 * primnodes.h
 *	  Definitions for "primitive" node types, those that are used in more
 *	  than one of the parse/plan/execute stages of the query pipeline.
 *	  Currently, these are mostly nodes for executable expressions
 *	  and join trees.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: primnodes.h,v 1.52 2001/02/14 21:35:05 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PRIMNODES_H
#define PRIMNODES_H

#include "access/attnum.h"
#include "nodes/pg_list.h"

/* FunctionCache is declared in utils/fcache.h */
typedef struct FunctionCache *FunctionCachePtr;


/* ----------------------------------------------------------------
 *						node definitions
 * ----------------------------------------------------------------
 */

/*--------------------
 * Resdom (Result Domain)
 *
 * Notes:
 * ressortgroupref is the parse/plan-time representation of ORDER BY and
 * GROUP BY items.	Targetlist entries with ressortgroupref=0 are not
 * sort/group items.  If ressortgroupref>0, then this item is an ORDER BY or
 * GROUP BY value.	No two entries in a targetlist may have the same nonzero
 * ressortgroupref --- but there is no particular meaning to the nonzero
 * values, except as tags.	(For example, one must not assume that lower
 * ressortgroupref means a more significant sort key.)	The order of the
 * associated SortClause or GroupClause lists determine the semantics.
 *
 * reskey and reskeyop are the execution-time representation of sorting.
 * reskey must be zero in any non-sort-key item.  The reskey of sort key
 * targetlist items for a sort plan node is 1,2,...,n for the n sort keys.
 * The reskeyop of each such targetlist item is the sort operator's
 * regproc OID.  reskeyop will be zero in non-sort-key items.
 *
 * Both reskey and reskeyop are typically zero during parse/plan stages.
 * The executor does not pay any attention to ressortgroupref.
 *--------------------
 */
typedef struct Resdom
{
	NodeTag		type;		
	AttrNumber	resno;		/* attribute number */
	Oid			restype;	/* type of the value */
	int32		restypmod;	/* type-specific modifier of the value */
	char	   *resname;	/* name of the resdom (could be NULL) */
	Index		ressortgroupref;
							/* nonzero if referenced by a sort/group clause */
	Index		reskey;		/* order of key in a sort (for those > 0) */
	Oid			reskeyop;	/* sort operator's regproc Oid */
	bool		resjunk;	/* set to true to eliminate the attribute
 *						       from final target list */
} Resdom;

/*
 * Fjoin
 */
typedef struct Fjoin
{
	NodeTag		type;
	bool		fj_initialized;	/* true if the Fjoin has already been
								 * initialized for the current target
								 * list evaluation */
	int			fj_nNodes;		/* The number of Iter nodes returning
								 * sets that the node will flatten */
	List	   *fj_innerNode;	/* exactly one Iter node.  We eval every
								 * node in the outerList once then eval
								 * the inner node to completion pair the
								 * outerList result vector with each inner
								 * result to form the full result.  When
								 * the inner has been exhausted, we get
								 * the next outer result vector and reset
								 * the inner.
								 */
	DatumPtr	fj_results;		/* The complete (flattened) result vector */
	BoolPtr		fj_alwaysDone;	/* a null vector to indicate sets with a
								 * cardinality of 0, we treat them as the
								 * set {NULL}.
								 */
} Fjoin;


/* ----------------------------------------------------------------
 *					node types for executable expressions
 * ----------------------------------------------------------------
 */

/*
 * Expr
 */
typedef enum OpType
{
	OP_EXPR, FUNC_EXPR, OR_EXPR, AND_EXPR, NOT_EXPR, SUBPLAN_EXPR
} OpType;

typedef struct Expr
{
	NodeTag		type;
	Oid			typeOid;	/* oid of the type of this expression */
	OpType		opType;		/* type of this expression */
	Node	   *oper;		/* operator node if needed (Oper, Func, or
							 * SubPlan) */
	List	   *args;		/* arguments to this expression */
} Expr;

/*
 * Var
 *
 * Note: during parsing/planning, varnoold/varoattno are always just copies
 * of varno/varattno.  At the tail end of planning, Var nodes appearing in
 * upper-level plan nodes are reassigned to point to the outputs of their
 * subplans; for example, in a join node varno becomes INNER or OUTER and
 * varattno becomes the index of the proper element of that subplan's target
 * list.  But varnoold/varoattno continue to hold the original values.
 * The code doesn't really need varnoold/varoattno, but they are very useful
 * for debugging and interpreting completed plans, so we keep them around.
 */
#define    INNER		65000
#define    OUTER		65001

#define    PRS2_OLD_VARNO			1
#define    PRS2_NEW_VARNO			2

typedef struct Var
{
	NodeTag		type;
	Index		varno;		/* index of this var's relation in the range
							 * table (could also be INNER or OUTER) */
	AttrNumber	varattno;	/* attribute number of this var, or zero for all */
	Oid			vartype;	/* pg_type tuple OID for the type of this var */
	int32		vartypmod;	/* pg_attribute typmod value */
	Index		varlevelsup;
							/* for subquery variables referencing outer
							 * relations; 0 in a normal var, >0 means N
							 * levels up */
	Index		varnoold;	/* original value of varno, for debugging */
	AttrNumber	varoattno;	/* original value of varattno */
} Var;

/*--------------------
 * Oper
 *
 * NOTE: in the good old days 'opno' used to be both (or either, or
 * neither) the pg_operator oid, and/or the pg_proc oid depending
 * on the postgres module in question (parser->pg_operator,
 * executor->pg_proc, planner->both), the mood of the programmer,
 * and the phase of the moon (rumors that it was also depending on the day
 * of the week are probably false). To make things even more postgres-like
 * (i.e. a mess) some comments were referring to 'opno' using the name
 * 'opid'. Anyway, now we have two separate fields, and of course that
 * immediately removes all bugs from the code...		[ sp :-) ].
 *
 * Note also that opid is not necessarily filled in immediately on creation
 * of the node.  The planner makes sure it is valid before passing the node
 * tree to the executor, but during parsing/planning opid is typically 0.
 *--------------------
 */
typedef struct Oper
{
	NodeTag		type;
	Oid			opno;		/* PG_OPERATOR OID of the operator */
	Oid			opid;		/* PG_PROC OID for the operator's underlying
							 * function */
	Oid			opresulttype;
							/* PG_TYPE OID of the operator's return value */
	FunctionCachePtr op_fcache;
							/* runtime state while running the function */
} Oper;


/*
 * Const
 */
typedef struct Const
{
	NodeTag		type;
	Oid			consttype;		/* PG_TYPE OID of the constant's value */
	int			constlen;		/* length in bytes of the constant's value */
	Datum		constvalue;		/* the constant's value */
	bool		constisnull;	/* whether the constant is null (if true,
								 * the other fields are undefined) */
	bool		constbyval;		/* whether the information in constvalue
								 * if passed by value.  If true, then all
								 * the information is stored in the datum.
								 * If false, then the datum contains a pointer
								 * to the information. */
	bool		constisset;		/* whether the const represents a set.
								 * The const value corresponding will be the
								 * query that defines the set. */
	bool		constiscast;
} Const;

/* ----------------
 * Param
 *		paramkind - specifies the kind of parameter. The possible values
 *		for this field are specified in "params.h", and they are:
 *
 *		PARAM_NAMED: The parameter has a name, i.e. something
 *				like `$.salary' or `$.foobar'.
 *				In this case field `paramname' must be a valid Name.
 *
 *		PARAM_NUM:	 The parameter has only a numeric identifier,
 *				i.e. something like `$1', `$2' etc.
 *				The number is contained in the `paramid' field.
 *
 *		PARAM_NEW:	 Used in PRS2 rule, similar to PARAM_NAMED.
 *					 The `paramname' and `paramid' refer to the "NEW" tuple
 *					 The `pramname' is the attribute name and `paramid'
 *					 is the attribute number.
 *
 *		PARAM_OLD:	 Same as PARAM_NEW, but in this case we refer to
 *				the "OLD" tuple.
 * ----------------
 */
typedef struct Param
{
	NodeTag		type;
	int			paramkind;	/* specifies the kind of parameter.  See above */
	AttrNumber	paramid;	/* numeric identifier for literal-constant
							 * parameters ("$1") */
	char	   *paramname;	/* attribute name for tuple-substitution
							 * parameters ("$.foo") */
	Oid			paramtype;	/* PG_TYPE OID of the parameter's value */
} Param;


/*
 * Func
 */
typedef struct Func
{
	NodeTag		type;
	Oid			funcid;		/* PG_PROC OID of the function */
	Oid			functype;	/* PG_TYPE OID of the function's return value */
	FunctionCachePtr func_fcache;
							/* runtime state while running this function.
							 * Where we are in the execution of the function
							 * if it returns more than one value, etc.
							 * See utils/fcache.h */
} Func;

/* ----------------
 * Iter
 *		can anyone explain what this is for?  Seems to have something to do
 *		with evaluation of functions that return sets...
 * ----------------
 */
typedef struct Iter
{
	NodeTag		type;
	Node	   *iterexpr;
	Oid			itertype;		/* type of the iter expr (use for type
								 * checking) */
} Iter;

/*
 * Aggref
 */
typedef struct Aggref
{
	NodeTag		type;
	char	   *aggname;	/* name of the aggregate */
	Oid			basetype;	/* base type Oid of the aggregate
							 * (ie, input type) */
	Oid			aggtype;	/* type Oid of final result of the aggregate */
	Node	   *target;		/* attribute or expression we are aggregating on */
	bool		aggstar;	/* TRUE if argument was really '*' */
	bool		aggdistinct;/* TRUE if it's agg(DISTINCT ...) */
	int			aggno;		/* workspace for executor (see nodeAgg.c) */
} Aggref;

/* ----------------
 * SubLink
 *
 * A SubLink represents a subselect appearing in an expression, and in some
 * cases also the combining operator(s) just above it.	The subLinkType
 * indicates the form of the expression represented:
 *	EXISTS_SUBLINK		EXISTS(SELECT ...)
 *	ALL_SUBLINK			(lefthand) op ALL (SELECT ...)
 *	ANY_SUBLINK			(lefthand) op ANY (SELECT ...)
 *	MULTIEXPR_SUBLINK	(lefthand) op (SELECT ...)
 *	EXPR_SUBLINK		(SELECT with single targetlist item ...)
 * For ALL, ANY, and MULTIEXPR, the lefthand is a list of expressions of the
 * same length as the subselect's targetlist.  MULTIEXPR will *always* have
 * a list with more than one entry; if the subselect has just one target
 * then the parser will create an EXPR_SUBLINK instead (and any operator
 * above the subselect will be represented separately).  Note that both
 * MULTIEXPR and EXPR require the subselect to deliver only one row.
 * ALL, ANY, and MULTIEXPR require the combining operators to deliver boolean
 * results.  These are reduced to one result per row using OR or AND semantics
 * depending on the "useor" flag.  ALL and ANY combine the per-row results
 * using AND and OR semantics respectively.
 *
 * NOTE: lefthand and oper have varying meanings depending on where you look
 * in the parse/plan pipeline:
 * 1. gram.y delivers a list of the (untransformed) lefthand expressions in
 *	  lefthand, and sets oper to a single A_Expr (not a list!) containing
 *	  the string name of the operator, but no arguments.
 * 2. The parser's expression transformation transforms lefthand normally,
 *	  and replaces oper with a list of Oper nodes, one per lefthand
 *	  expression.  These nodes represent the parser's resolution of exactly
 *	  which operator to apply to each pair of lefthand and targetlist
 *	  expressions.	However, we have not constructed actual Expr trees for
 *	  these operators yet.	This is the representation seen in saved rules
 *	  and in the rewriter.
 * 3. Finally, the planner converts the oper list to a list of normal Expr
 *	  nodes representing the application of the operator(s) to the lefthand
 *	  expressions and values from the inner targetlist.  The inner
 *	  targetlist items are represented by placeholder Param or Const nodes.
 *	  The lefthand field is set to NIL, since its expressions are now in
 *	  the Expr list.  This representation is passed to the executor.
 *
 * Planner routines that might see either representation 2 or 3 can tell
 * the difference by checking whether lefthand is NIL or not.  Also,
 * representation 2 appears in a "bare" SubLink, while representation 3 is
 * found in SubLinks that are children of SubPlan nodes.
 *
 * In EXISTS and EXPR SubLinks, both lefthand and oper are unused and are
 * always NIL.	useor is not significant either for these sublink types.
 * ----------------
 */
typedef enum SubLinkType
{
	EXISTS_SUBLINK, ALL_SUBLINK, ANY_SUBLINK, MULTIEXPR_SUBLINK, EXPR_SUBLINK
} SubLinkType;


typedef struct SubLink
{
	NodeTag		type;
	SubLinkType subLinkType;/* EXISTS, ALL, ANY, MULTIEXPR, EXPR */
	bool		useor;		/* TRUE to combine column results with "OR"
							 * not "AND" */
	List	   *lefthand;	/* list of outer-query expressions on the left */
	List	   *oper;		/* list of Oper nodes for combining operators */
	Node	   *subselect;	/* subselect as Query* or parsetree */
} SubLink;

/* ----------------
 *	ArrayRef: describes an array subscripting operation
 *
 * An ArrayRef can describe fetching a single element from an array,
 * fetching a subarray (array slice), storing a single element into
 * an array, or storing a slice.  The "store" cases work with an
 * initial array value and a source value that is inserted into the
 * appropriate part of the array; the result of the operation is an
 * entire new modified array value.
 *
 * If reflowerindexpr = NIL, then we are fetching or storing a single array
 * element at the subscripts given by refupperindexpr.	Otherwise we are
 * fetching or storing an array slice, that is a rectangular subarray
 * with lower and upper bounds given by the index expressions.
 * reflowerindexpr must be the same length as refupperindexpr when it
 * is not NIL.
 *
 * Note: array types can be fixed-length (refattrlength > 0), but only
 * when the element type is itself fixed-length.  Otherwise they are
 * varlena structures and have refattrlength = -1.	In any case,
 * an array type is never pass-by-value.
 *
 * Note: currently, refelemtype is NOT the element type, but the array type,
 * when doing subarray fetch or either type of store.  It would be cleaner
 * to add more fields so we can distinguish the array element type from the
 * result type of the ArrayRef operator...
 * ----------------
 */
typedef struct ArrayRef
{
	NodeTag		type;
	int			refattrlength;		/* typlen of array type */
	int			refelemlength;		/* typlen of the array element type */
	Oid			refelemtype;		/* type of the result of the ArrayRef
								 	 * operation */
	bool		refelembyval;		/* is the element type pass-by-value? */
	List	   *refupperindexpr;	/* expressions that evaluate to upper
								 	 * array indexes */
	List	   *reflowerindexpr;	/* expressions that evaluate to lower
								 	 * array indexes */
	Node	   *refexpr;			/* the expression that evaluates to an
									 * array value */
	Node	   *refassgnexpr;		/* expression for the source value, or NULL
								 	 * if fetch */
} ArrayRef;

/* ----------------
 * FieldSelect
 *
 * FieldSelect represents the operation of extracting one field from a tuple
 * value.  At runtime, the input expression is expected to yield a Datum
 * that contains a pointer-to-TupleTableSlot.  The specified field number
 * is extracted and returned as a Datum.
 * ----------------
 */

typedef struct FieldSelect
{
	NodeTag		type;
	Node	   *arg;			/* input expression */
	AttrNumber	fieldnum;		/* attribute number of field to extract */
	Oid			resulttype;		/* type of the field (result type of this
								 * node) */
	int32		resulttypmod;	/* output typmod (usually -1) */
} FieldSelect;

/* ----------------
 * RelabelType
 *
 * RelabelType represents a "dummy" type coercion between two binary-
 * compatible datatypes, such as reinterpreting the result of an OID
 * expression as an int4.  It is a no-op at runtime; we only need it
 * to provide a place to store the correct type to be attributed to
 * the expression result during type resolution.  (We can't get away
 * with just overwriting the type field of the input expression node,
 * so we need a separate node to show the coercion's result type.)
 * ----------------
 */

typedef struct RelabelType
{
	NodeTag		type;
	Node	   *arg;			/* input expression */
	Oid			resulttype;		/* output type of coercion expression */
	int32		resulttypmod;	/* output typmod (usually -1) */
} RelabelType;


/* ----------------------------------------------------------------
 *					node types for join trees
 *
 * The leaves of a join tree structure are RangeTblRef nodes.  Above
 * these, JoinExpr nodes can appear to denote a specific kind of join
 * or qualified join.  Also, FromExpr nodes can appear to denote an
 * ordinary cross-product join ("FROM foo, bar, baz WHERE ...").
 * FromExpr is like a JoinExpr of jointype JOIN_INNER, except that it
 * may have any number of child nodes, not just two.  Also, there is an
 * implementation-defined difference: the planner is allowed to join the
 * children of a FromExpr using whatever join order seems good to it.
 * At present, JoinExpr nodes are always joined in exactly the order
 * implied by the jointree structure (except the planner may choose to
 * swap inner and outer members of a join pair).
 *
 * NOTE: the top level of a Query's jointree is always a FromExpr.
 * Even if the jointree contains no rels, there will be a FromExpr.
 *
 * NOTE: the qualification expressions present in JoinExpr nodes are
 * *in addition to* the query's main WHERE clause, which appears as the
 * qual of the top-level FromExpr.  The reason for associating quals with
 * specific nodes in the jointree is that the position of a qual is critical
 * when outer joins are present.  (If we enforce a qual too soon or too late,
 * that may cause the outer join to produce the wrong set of NULL-extended
 * rows.)  If all joins are inner joins then all the qual positions are
 * semantically interchangeable.
 *
 * NOTE: in the raw output of gram.y, a join tree contains RangeVar and
 * RangeSubselect nodes, which are both replaced by RangeTblRef nodes
 * during the parse analysis phase.  Also, the top-level FromExpr is added
 * during parse analysis; the grammar regards FROM and WHERE as separate.
 * ----------------------------------------------------------------
 */

/*
 * RangeTblRef - reference to an entry in the query's rangetable
 *
 * We could use direct pointers to the RT entries and skip having these
 * nodes, but multiple pointers to the same node in a querytree cause
 * lots of headaches, so it seems better to store an index into the RT.
 */
typedef struct RangeTblRef
{
	NodeTag		type;
	int			rtindex;
} RangeTblRef;

/*----------
 * JoinExpr - for SQL JOIN expressions
 *
 * isNatural, using, and quals are interdependent.  The user can write only
 * one of NATURAL, USING(), or ON() (this is enforced by the grammar).
 * If he writes NATURAL then parse analysis generates the equivalent USING()
 * list, and from that fills in "quals" with the right equality comparisons.
 * If he writes USING() then "quals" is filled with equality comparisons.
 * If he writes ON() then only "quals" is set.  Note that NATURAL/USING
 * are not equivalent to ON() since they also affect the output column list.
 *
 * alias is an Attr node representing the AS alias-clause attached to the
 * join expression, or NULL if no clause.  NB: presence or absence of the
 * alias has a critical impact on semantics, because a join with an alias
 * restricts visibility of the tables/columns inside it.
 *
 * During parse analysis, colnames is filled with a list of String nodes
 * giving the column names (real or alias) of the output of the join,
 * and colvars is filled with a list of expressions that can be copied to
 * reference the output columns.
 *----------
 */
typedef struct JoinExpr
{
	NodeTag		type;
	JoinType	jointype;		/* type of join */
	bool		isNatural;		/* Natural join? Will need to shape table */
	Node	   *larg;			/* left subtree */
	Node	   *rarg;			/* right subtree */
	List	   *using;			/* USING clause, if any (list of String) */
	Node	   *quals;			/* qualifiers on join, if any */
	struct Attr *alias;			/* user-written alias clause, if any */
	List	   *colnames;		/* output column names (list of String) */
	List	   *colvars;		/* output column nodes (list of expressions) */
} JoinExpr;

/*----------
 * FromExpr - represents a FROM ... WHERE ... construct
 *
 * This is both more flexible than a JoinExpr (it can have any number of
 * children, including zero) and less so --- we don't need to deal with
 * aliases and so on.  The output column set is implicitly just the union
 * of the outputs of the children.
 *----------
 */
typedef struct FromExpr
{
	NodeTag		type;
	List	   *fromlist;		/* List of join subtrees */
	Node	   *quals;			/* qualifiers on join, if any */
} FromExpr;

#endif	 /* PRIMNODES_H */
