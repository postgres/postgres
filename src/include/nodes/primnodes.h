/*-------------------------------------------------------------------------
 *
 * primnodes.h
 *	  Definitions for "primitive" node types, those that are used in more
 *	  than one of the parse/plan/execute stages of the query pipeline.
 *	  Currently, these are mostly nodes for executable expressions
 *	  and join trees.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: primnodes.h,v 1.92 2003/08/17 23:43:26 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PRIMNODES_H
#define PRIMNODES_H

#include "access/attnum.h"
#include "nodes/pg_list.h"


/* ----------------------------------------------------------------
 *						node definitions
 * ----------------------------------------------------------------
 */

/*--------------------
 * Resdom (Result Domain)
 *
 * Notes:
 *
 * In a SELECT's targetlist, resno should always be equal to the item's
 * ordinal position (counting from 1).  However, in an INSERT or UPDATE
 * targetlist, resno represents the attribute number of the destination
 * column for the item; so there may be missing or out-of-order resnos.
 * In an UPDATE, it is even legal to have duplicated resnos; consider
 *		UPDATE table SET arraycol[1] = ..., arraycol[2] = ..., ...
 * The two meanings come together in the executor, because the planner
 * transforms INSERT/UPDATE tlists into a normalized form with exactly
 * one entry for each column of the destination table.  Before that's
 * happened, however, it is risky to assume that resno == position.
 * Generally get_tle_by_resno() should be used rather than nth() to fetch
 * tlist entries by resno.
 *
 * resname is required to represent the correct column name in non-resjunk
 * entries of top-level SELECT targetlists, since it will be used as the
 * column title sent to the frontend.  In most other contexts it is only
 * a debugging aid, and may be wrong or even NULL.  (In particular, it may
 * be wrong in a tlist from a stored rule, if the referenced column has been
 * renamed by ALTER TABLE since the rule was made.  Also, the planner tends
 * to store NULL rather than look up a valid name for tlist entries in
 * non-toplevel plan nodes.)  In resjunk entries, resname should be either
 * a specific system-generated name (such as "ctid") or NULL; anything else
 * risks confusing ExecGetJunkAttribute!
 *
 * ressortgroupref is used in the representation of ORDER BY and
 * GROUP BY items.	Targetlist entries with ressortgroupref=0 are not
 * sort/group items.  If ressortgroupref>0, then this item is an ORDER BY or
 * GROUP BY value.	No two entries in a targetlist may have the same nonzero
 * ressortgroupref --- but there is no particular meaning to the nonzero
 * values, except as tags.	(For example, one must not assume that lower
 * ressortgroupref means a more significant sort key.)	The order of the
 * associated SortClause or GroupClause lists determine the semantics.
 *
 * resorigtbl/resorigcol identify the source of the column, if it is a
 * simple reference to a column of a base table (or view).	If it is not
 * a simple reference, these fields are zeroes.
 *
 * If resjunk is true then the column is a working column (such as a sort key)
 * that should be removed from the final output of the query.  Resjunk columns
 * must have resnos that cannot duplicate any regular column's resno.  Also
 * note that there are places that assume resjunk columns come after non-junk
 * columns.
 *--------------------
 */
typedef struct Resdom
{
	NodeTag		type;
	AttrNumber	resno;			/* attribute number (see notes above) */
	Oid			restype;		/* type of the value */
	int32		restypmod;		/* type-specific modifier of the value */
	char	   *resname;		/* name of the column (could be NULL) */
	Index		ressortgroupref;/* nonzero if referenced by a sort/group
								 * clause */
	Oid			resorigtbl;		/* OID of column's source table */
	AttrNumber	resorigcol;		/* column's number in source table */
	bool		resjunk;		/* set to true to eliminate the attribute
								 * from final target list */
} Resdom;


/*
 * Alias -
 *	  specifies an alias for a range variable; the alias might also
 *	  specify renaming of columns within the table.
 */
typedef struct Alias
{
	NodeTag		type;
	char	   *aliasname;		/* aliased rel name (never qualified) */
	List	   *colnames;		/* optional list of column aliases */
	/* Note: colnames is a list of Value nodes (always strings) */
} Alias;

typedef enum InhOption
{
	INH_NO,						/* Do NOT scan child tables */
	INH_YES,					/* DO scan child tables */
	INH_DEFAULT					/* Use current SQL_inheritance option */
} InhOption;

/*
 * RangeVar - range variable, used in FROM clauses
 *
 * Also used to represent table names in utility statements; there, the alias
 * field is not used, and inhOpt shows whether to apply the operation
 * recursively to child tables.  In some contexts it is also useful to carry
 * a TEMP table indication here.
 */
typedef struct RangeVar
{
	NodeTag		type;
	char	   *catalogname;	/* the catalog (database) name, or NULL */
	char	   *schemaname;		/* the schema name, or NULL */
	char	   *relname;		/* the relation/sequence name */
	InhOption	inhOpt;			/* expand rel by inheritance? recursively
								 * act on children? */
	bool		istemp;			/* is this a temp relation/sequence? */
	Alias	   *alias;			/* table alias & optional column aliases */
} RangeVar;


/* ----------------------------------------------------------------
 *					node types for executable expressions
 * ----------------------------------------------------------------
 */

/*
 * Expr - generic superclass for executable-expression nodes
 *
 * All node types that are used in executable expression trees should derive
 * from Expr (that is, have Expr as their first field).  Since Expr only
 * contains NodeTag, this is a formality, but it is an easy form of
 * documentation.  See also the ExprState node types in execnodes.h.
 */
typedef struct Expr
{
	NodeTag		type;
} Expr;

/*
 * Var - expression node representing a variable (ie, a table column)
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
	Expr		xpr;
	Index		varno;			/* index of this var's relation in the
								 * range table (could also be INNER or
								 * OUTER) */
	AttrNumber	varattno;		/* attribute number of this var, or zero
								 * for all */
	Oid			vartype;		/* pg_type tuple OID for the type of this
								 * var */
	int32		vartypmod;		/* pg_attribute typmod value */
	Index		varlevelsup;

	/*
	 * for subquery variables referencing outer relations; 0 in a normal
	 * var, >0 means N levels up
	 */
	Index		varnoold;		/* original value of varno, for debugging */
	AttrNumber	varoattno;		/* original value of varattno */
} Var;

/*
 * Const
 */
typedef struct Const
{
	Expr		xpr;
	Oid			consttype;		/* PG_TYPE OID of the constant's datatype */
	int			constlen;		/* typlen of the constant's datatype */
	Datum		constvalue;		/* the constant's value */
	bool		constisnull;	/* whether the constant is null (if true,
								 * constvalue is undefined) */
	bool		constbyval;		/* whether this datatype is passed by
								 * value. If true, then all the
								 * information is stored in the Datum. If
								 * false, then the Datum contains a
								 * pointer to the information. */
} Const;

/* ----------------
 * Param
 *		paramkind - specifies the kind of parameter. The possible values
 *		for this field are specified in "params.h", and they are:
 *
 *		PARAM_NAMED: The parameter has a name, i.e. something
 *				like `$.salary' or `$.foobar'.
 *				In this case field `paramname' must be a valid name.
 *
 *		PARAM_NUM:	 The parameter has only a numeric identifier,
 *				i.e. something like `$1', `$2' etc.
 *				The number is contained in the `paramid' field.
 *
 *		PARAM_EXEC:  The parameter is an internal executor parameter.
 *				It has a number contained in the `paramid' field.
 * ----------------
 */
typedef struct Param
{
	Expr		xpr;
	int			paramkind;		/* kind of parameter. See above */
	AttrNumber	paramid;		/* numeric ID for parameter ("$1") */
	char	   *paramname;		/* name for parameter ("$.foo") */
	Oid			paramtype;		/* PG_TYPE OID of parameter's datatype */
} Param;

/*
 * Aggref
 */
typedef struct Aggref
{
	Expr		xpr;
	Oid			aggfnoid;		/* pg_proc Oid of the aggregate */
	Oid			aggtype;		/* type Oid of result of the aggregate */
	Expr	   *target;			/* expression we are aggregating on */
	Index		agglevelsup;	/* > 0 if agg belongs to outer query */
	bool		aggstar;		/* TRUE if argument was really '*' */
	bool		aggdistinct;	/* TRUE if it's agg(DISTINCT ...) */
} Aggref;

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
 * Note: refrestype is NOT the element type, but the array type,
 * when doing subarray fetch or either type of store.
 * ----------------
 */
typedef struct ArrayRef
{
	Expr		xpr;
	Oid			refrestype;		/* type of the result of the ArrayRef
								 * operation */
	Oid			refarraytype;	/* type of the array proper */
	Oid			refelemtype;	/* type of the array elements */
	List	   *refupperindexpr;/* expressions that evaluate to upper
								 * array indexes */
	List	   *reflowerindexpr;/* expressions that evaluate to lower
								 * array indexes */
	Expr	   *refexpr;		/* the expression that evaluates to an
								 * array value */
	Expr	   *refassgnexpr;	/* expression for the source value, or
								 * NULL if fetch */
} ArrayRef;

/*
 * CoercionContext - distinguishes the allowed set of type casts
 *
 * NB: ordering of the alternatives is significant; later (larger) values
 * allow more casts than earlier ones.
 */
typedef enum CoercionContext
{
	COERCION_IMPLICIT,			/* coercion in context of expression */
	COERCION_ASSIGNMENT,		/* coercion in context of assignment */
	COERCION_EXPLICIT			/* explicit cast operation */
} CoercionContext;

/*
 * CoercionForm - information showing how to display a function-call node
 */
typedef enum CoercionForm
{
	COERCE_EXPLICIT_CALL,		/* display as a function call */
	COERCE_EXPLICIT_CAST,		/* display as an explicit cast */
	COERCE_IMPLICIT_CAST,		/* implicit cast, so hide it */
	COERCE_DONTCARE				/* special case for pathkeys */
} CoercionForm;

/*
 * FuncExpr - expression node for a function call
 */
typedef struct FuncExpr
{
	Expr		xpr;
	Oid			funcid;			/* PG_PROC OID of the function */
	Oid			funcresulttype; /* PG_TYPE OID of result value */
	bool		funcretset;		/* true if function returns set */
	CoercionForm funcformat;	/* how to display this function call */
	List	   *args;			/* arguments to the function */
} FuncExpr;

/*
 * OpExpr - expression node for an operator invocation
 *
 * Semantically, this is essentially the same as a function call.
 *
 * Note that opfuncid is not necessarily filled in immediately on creation
 * of the node.  The planner makes sure it is valid before passing the node
 * tree to the executor, but during parsing/planning opfuncid is typically 0.
 */
typedef struct OpExpr
{
	Expr		xpr;
	Oid			opno;			/* PG_OPERATOR OID of the operator */
	Oid			opfuncid;		/* PG_PROC OID of underlying function */
	Oid			opresulttype;	/* PG_TYPE OID of result value */
	bool		opretset;		/* true if operator returns set */
	List	   *args;			/* arguments to the operator (1 or 2) */
} OpExpr;

/*
 * DistinctExpr - expression node for "x IS DISTINCT FROM y"
 *
 * Except for the nodetag, this is represented identically to an OpExpr
 * referencing the "=" operator for x and y.
 * We use "=", not the more obvious "<>", because more datatypes have "="
 * than "<>".  This means the executor must invert the operator result.
 * Note that the operator function won't be called at all if either input
 * is NULL, since then the result can be determined directly.
 */
typedef OpExpr DistinctExpr;

/*
 * ScalarArrayOpExpr - expression node for "scalar op ANY/ALL (array)"
 *
 * The operator must yield boolean.  It is applied to the left operand
 * and each element of the righthand array, and the results are combined
 * with OR or AND (for ANY or ALL respectively).  The node representation
 * is almost the same as for the underlying operator, but we need a useOr
 * flag to remember whether it's ANY or ALL, and we don't have to store
 * the result type because it must be boolean.
 */
typedef struct ScalarArrayOpExpr
{
	Expr		xpr;
	Oid			opno;			/* PG_OPERATOR OID of the operator */
	Oid			opfuncid;		/* PG_PROC OID of underlying function */
	bool		useOr;			/* true for ANY, false for ALL */
	List	   *args;			/* the scalar and array operands */
} ScalarArrayOpExpr;

/*
 * BoolExpr - expression node for the basic Boolean operators AND, OR, NOT
 *
 * Notice the arguments are given as a List.  For NOT, of course the list
 * must always have exactly one element.  For AND and OR, the executor can
 * handle any number of arguments.	The parser treats AND and OR as binary
 * and so it only produces two-element lists, but the optimizer will flatten
 * trees of AND and OR nodes to produce longer lists when possible.
 */
typedef enum BoolExprType
{
	AND_EXPR, OR_EXPR, NOT_EXPR
} BoolExprType;

typedef struct BoolExpr
{
	Expr		xpr;
	BoolExprType boolop;
	List	   *args;			/* arguments to this expression */
} BoolExpr;

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
 *	ARRAY_SUBLINK		ARRAY(SELECT with single targetlist item ...)
 * For ALL, ANY, and MULTIEXPR, the lefthand is a list of expressions of the
 * same length as the subselect's targetlist.  MULTIEXPR will *always* have
 * a list with more than one entry; if the subselect has just one target
 * then the parser will create an EXPR_SUBLINK instead (and any operator
 * above the subselect will be represented separately).  Note that both
 * MULTIEXPR and EXPR require the subselect to deliver only one row.
 * ARRAY requires just one target column, and creates an array of the target
 * column's type using one or more rows resulting from the subselect.
 * ALL, ANY, and MULTIEXPR require the combining operators to deliver boolean
 * results.  These are reduced to one result per row using OR or AND semantics
 * depending on the "useOr" flag.  ALL and ANY combine the per-row results
 * using AND and OR semantics respectively.
 *
 * SubLink is classed as an Expr node, but it is not actually executable;
 * it must be replaced in the expression tree by a SubPlan node during
 * planning.
 *
 * NOTE: in the raw output of gram.y, lefthand contains a list of raw
 * expressions; useOr and operOids are not filled in yet.  Also, subselect
 * is a raw parsetree.	During parse analysis, the parser transforms the
 * lefthand expression list using normal expression transformation rules.
 * It fills operOids with the OIDs representing the specific operator(s)
 * to apply to each pair of lefthand and targetlist expressions.
 * And subselect is transformed to a Query.  This is the representation
 * seen in saved rules and in the rewriter.
 *
 * In EXISTS, EXPR, and ARRAY SubLinks, lefthand, operName, and operOids are
 * unused and are always NIL.  useOr is not significant either for these
 * sublink types.
 * ----------------
 */
typedef enum SubLinkType
{
	EXISTS_SUBLINK,
	ALL_SUBLINK,
	ANY_SUBLINK,
	MULTIEXPR_SUBLINK,
	EXPR_SUBLINK,
	ARRAY_SUBLINK
} SubLinkType;


typedef struct SubLink
{
	Expr		xpr;
	SubLinkType subLinkType;	/* EXISTS, ALL, ANY, MULTIEXPR, EXPR */
	bool		useOr;			/* TRUE to combine column results with
								 * "OR" not "AND" */
	List	   *lefthand;		/* list of outer-query expressions on the
								 * left */
	List	   *operName;		/* originally specified operator name */
	List	   *operOids;		/* OIDs of actual combining operators */
	Node	   *subselect;		/* subselect as Query* or parsetree */
} SubLink;

/*
 * SubPlan - executable expression node for a subplan (sub-SELECT)
 *
 * The planner replaces SubLink nodes in expression trees with SubPlan
 * nodes after it has finished planning the subquery.  SubPlan contains
 * a sub-plantree and rtable instead of a sub-Query.
 *
 * In an ordinary subplan, "exprs" points to a list of executable expressions
 * (OpExpr trees) for the combining operators; their left-hand arguments are
 * the original lefthand expressions, and their right-hand arguments are
 * PARAM_EXEC Param nodes representing the outputs of the sub-select.
 * (NOTE: runtime coercion functions may be inserted as well.)	But if the
 * sub-select becomes an initplan rather than a subplan, these executable
 * expressions are part of the outer plan's expression tree (and the SubPlan
 * node itself is not).  In this case "exprs" is NIL to avoid duplication.
 *
 * The planner also derives lists of the values that need to be passed into
 * and out of the subplan.	Input values are represented as a list "args" of
 * expressions to be evaluated in the outer-query context (currently these
 * args are always just Vars, but in principle they could be any expression).
 * The values are assigned to the global PARAM_EXEC params indexed by parParam
 * (the parParam and args lists must have the same ordering).  setParam is a
 * list of the PARAM_EXEC params that are computed by the sub-select, if it
 * is an initplan; they are listed in order by sub-select output column
 * position.  (parParam and setParam are integer Lists, not Bitmapsets,
 * because their ordering is significant.)
 */
typedef struct SubPlan
{
	Expr		xpr;
	/* Fields copied from original SubLink: */
	SubLinkType subLinkType;	/* EXISTS, ALL, ANY, MULTIEXPR, EXPR */
	bool		useOr;			/* TRUE to combine column results with
								 * "OR" not "AND" */
	/* The combining operators, transformed to executable expressions: */
	List	   *exprs;			/* list of OpExpr expression trees */
	List	   *paramIds;		/* IDs of Params embedded in the above */
	/* Note: paramIds has a one-to-one correspondence to the exprs list */
	/* The subselect, transformed to a Plan: */
	struct Plan *plan;			/* subselect plan itself */
	int			plan_id;		/* dummy thing because of we haven't equal
								 * funcs for plan nodes... actually, we
								 * could put *plan itself somewhere else
								 * (TopPlan node ?)... */
	List	   *rtable;			/* range table for subselect */
	/* Information about execution strategy: */
	bool		useHashTable;	/* TRUE to store subselect output in a
								 * hash table (implies we are doing "IN") */
	bool		unknownEqFalse; /* TRUE if it's okay to return FALSE when
								 * the spec result is UNKNOWN; this allows
								 * much simpler handling of null values */
	/* Information for passing params into and out of the subselect: */
	/* setParam and parParam are lists of integers (param IDs) */
	List	   *setParam;		/* initplan subqueries have to set these
								 * Params for parent plan */
	List	   *parParam;		/* indices of input Params from parent
								 * plan */
	List	   *args;			/* exprs to pass as parParam values */
} SubPlan;

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
	Expr		xpr;
	Expr	   *arg;			/* input expression */
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
	Expr		xpr;
	Expr	   *arg;			/* input expression */
	Oid			resulttype;		/* output type of coercion expression */
	int32		resulttypmod;	/* output typmod (usually -1) */
	CoercionForm relabelformat; /* how to display this node */
} RelabelType;

/*
 * CaseExpr - a CASE expression
 */
typedef struct CaseExpr
{
	Expr		xpr;
	Oid			casetype;		/* type of expression result */
	Expr	   *arg;			/* implicit equality comparison argument */
	List	   *args;			/* the arguments (list of WHEN clauses) */
	Expr	   *defresult;		/* the default result (ELSE clause) */
} CaseExpr;

/*
 * CaseWhen - an argument to a CASE expression
 */
typedef struct CaseWhen
{
	Expr		xpr;
	Expr	   *expr;			/* condition expression */
	Expr	   *result;			/* substitution result */
} CaseWhen;

/*
 * ArrayExpr - an ARRAY[] expression
 *
 * Note: if multidims is false, the constituent expressions all yield the
 * scalar type identified by element_typeid.  If multidims is true, the
 * constituent expressions all yield arrays of element_typeid (ie, the same
 * type as array_typeid); at runtime we must check for compatible subscripts.
 */
typedef struct ArrayExpr
{
	Expr		xpr;
	Oid			array_typeid;	/* type of expression result */
	Oid			element_typeid; /* common type of array elements */
	List	   *elements;		/* the array elements or sub-arrays */
	bool		multidims;		/* true if elements are sub-arrays */
} ArrayExpr;

/*
 * CoalesceExpr - a COALESCE expression
 */
typedef struct CoalesceExpr
{
	Expr		xpr;
	Oid			coalescetype;	/* type of expression result */
	List	   *args;			/* the arguments */
} CoalesceExpr;

/*
 * NullIfExpr - a NULLIF expression
 *
 * Like DistinctExpr, this is represented the same as an OpExpr referencing
 * the "=" operator for x and y.
 */
typedef OpExpr NullIfExpr;

/* ----------------
 * NullTest
 *
 * NullTest represents the operation of testing a value for NULLness.
 * Currently, we only support scalar input values, but eventually a
 * row-constructor input should be supported.
 * The appropriate test is performed and returned as a boolean Datum.
 * ----------------
 */

typedef enum NullTestType
{
	IS_NULL, IS_NOT_NULL
} NullTestType;

typedef struct NullTest
{
	Expr		xpr;
	Expr	   *arg;			/* input expression */
	NullTestType nulltesttype;	/* IS NULL, IS NOT NULL */
} NullTest;

/*
 * BooleanTest
 *
 * BooleanTest represents the operation of determining whether a boolean
 * is TRUE, FALSE, or UNKNOWN (ie, NULL).  All six meaningful combinations
 * are supported.  Note that a NULL input does *not* cause a NULL result.
 * The appropriate test is performed and returned as a boolean Datum.
 */

typedef enum BoolTestType
{
	IS_TRUE, IS_NOT_TRUE, IS_FALSE, IS_NOT_FALSE, IS_UNKNOWN, IS_NOT_UNKNOWN
} BoolTestType;

typedef struct BooleanTest
{
	Expr		xpr;
	Expr	   *arg;			/* input expression */
	BoolTestType booltesttype;	/* test type */
} BooleanTest;

/*
 * CoerceToDomain
 *
 * CoerceToDomain represents the operation of coercing a value to a domain
 * type.  At runtime (and not before) the precise set of constraints to be
 * checked will be determined.	If the value passes, it is returned as the
 * result; if not, an error is raised.	Note that this is equivalent to
 * RelabelType in the scenario where no constraints are applied.
 */
typedef struct CoerceToDomain
{
	Expr		xpr;
	Expr	   *arg;			/* input expression */
	Oid			resulttype;		/* domain type ID (result type) */
	int32		resulttypmod;	/* output typmod (currently always -1) */
	CoercionForm coercionformat;	/* how to display this node */
} CoerceToDomain;

/*
 * Placeholder node for the value to be processed by a domain's check
 * constraint.	This is effectively like a Param, but can be implemented more
 * simply since we need only one replacement value at a time.
 *
 * Note: the typeId/typeMod will be set from the domain's base type, not
 * the domain itself.  This is because we shouldn't consider the value to
 * be a member of the domain if we haven't yet checked its constraints.
 */
typedef struct CoerceToDomainValue
{
	Expr		xpr;
	Oid			typeId;			/* type for substituted value */
	int32		typeMod;		/* typemod for substituted value */
} CoerceToDomainValue;

/*
 * Placeholder node for a DEFAULT marker in an INSERT or UPDATE command.
 *
 * This is not an executable expression: it must be replaced by the actual
 * column default expression during rewriting.	But it is convenient to
 * treat it as an expression node during parsing and rewriting.
 */
typedef struct SetToDefault
{
	Expr		xpr;
	Oid			typeId;			/* type for substituted value */
	int32		typeMod;		/* typemod for substituted value */
} SetToDefault;

/*
 * TargetEntry -
 *	   a target entry (used in query target lists)
 *
 * Strictly speaking, a TargetEntry isn't an expression node (since it can't
 * be evaluated by ExecEvalExpr).  But we treat it as one anyway, since in
 * very many places it's convenient to process a whole query targetlist as a
 * single expression tree.
 *
 * The separation between TargetEntry and Resdom is historical.  One of these
 * days, Resdom should probably get folded into TargetEntry.
 */
typedef struct TargetEntry
{
	Expr		xpr;
	Resdom	   *resdom;			/* descriptor for targetlist item */
	Expr	   *expr;			/* expression to evaluate */
} TargetEntry;


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
 * qual of the top-level FromExpr.	The reason for associating quals with
 * specific nodes in the jointree is that the position of a qual is critical
 * when outer joins are present.  (If we enforce a qual too soon or too late,
 * that may cause the outer join to produce the wrong set of NULL-extended
 * rows.)  If all joins are inner joins then all the qual positions are
 * semantically interchangeable.
 *
 * NOTE: in the raw output of gram.y, a join tree contains RangeVar,
 * RangeSubselect, and RangeFunction nodes, which are all replaced by
 * RangeTblRef nodes during the parse analysis phase.  Also, the top-level
 * FromExpr is added during parse analysis; the grammar regards FROM and
 * WHERE as separate.
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
 * isNatural, using, and quals are interdependent.	The user can write only
 * one of NATURAL, USING(), or ON() (this is enforced by the grammar).
 * If he writes NATURAL then parse analysis generates the equivalent USING()
 * list, and from that fills in "quals" with the right equality comparisons.
 * If he writes USING() then "quals" is filled with equality comparisons.
 * If he writes ON() then only "quals" is set.	Note that NATURAL/USING
 * are not equivalent to ON() since they also affect the output column list.
 *
 * alias is an Alias node representing the AS alias-clause attached to the
 * join expression, or NULL if no clause.  NB: presence or absence of the
 * alias has a critical impact on semantics, because a join with an alias
 * restricts visibility of the tables/columns inside it.
 *
 * During parse analysis, an RTE is created for the Join, and its index
 * is filled into rtindex.	This RTE is present mainly so that Vars can
 * be created that refer to the outputs of the join.
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
	Alias	   *alias;			/* user-written alias clause, if any */
	int			rtindex;		/* RT index assigned for join */
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

#endif   /* PRIMNODES_H */
