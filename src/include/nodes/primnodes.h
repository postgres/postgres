/*-------------------------------------------------------------------------
 *
 * primnodes.h
 *	  Definitions for "primitive" node types, those that are used in more
 *	  than one of the parse/plan/execute stages of the query pipeline.
 *	  Currently, these are mostly nodes for executable expressions
 *	  and join trees.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/primnodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PRIMNODES_H
#define PRIMNODES_H

#include "access/attnum.h"
#include "nodes/bitmapset.h"
#include "nodes/pg_list.h"


typedef enum OverridingKind
{
	OVERRIDING_NOT_SET = 0,
	OVERRIDING_USER_VALUE,
	OVERRIDING_SYSTEM_VALUE,
} OverridingKind;


/* ----------------------------------------------------------------
 *						node definitions
 * ----------------------------------------------------------------
 */

/*
 * Alias -
 *	  specifies an alias for a range variable; the alias might also
 *	  specify renaming of columns within the table.
 *
 * Note: colnames is a list of String nodes.  In Alias structs
 * associated with RTEs, there may be entries corresponding to dropped
 * columns; these are normally empty strings ("").  See parsenodes.h for info.
 */
typedef struct Alias
{
	NodeTag		type;
	char	   *aliasname;		/* aliased rel name (never qualified) */
	List	   *colnames;		/* optional list of column aliases */
} Alias;

/* What to do at commit time for temporary relations */
typedef enum OnCommitAction
{
	ONCOMMIT_NOOP,				/* No ON COMMIT clause (do nothing) */
	ONCOMMIT_PRESERVE_ROWS,		/* ON COMMIT PRESERVE ROWS (do nothing) */
	ONCOMMIT_DELETE_ROWS,		/* ON COMMIT DELETE ROWS */
	ONCOMMIT_DROP,				/* ON COMMIT DROP */
} OnCommitAction;

/*
 * RangeVar - range variable, used in FROM clauses
 *
 * Also used to represent table names in utility statements; there, the alias
 * field is not used, and inh tells whether to apply the operation
 * recursively to child tables.  In some contexts it is also useful to carry
 * a TEMP table indication here.
 */
typedef struct RangeVar
{
	NodeTag		type;

	/* the catalog (database) name, or NULL */
	char	   *catalogname;

	/* the schema name, or NULL */
	char	   *schemaname;

	/* the relation/sequence name */
	char	   *relname;

	/* expand rel by inheritance? recursively act on children? */
	bool		inh;

	/* see RELPERSISTENCE_* in pg_class.h */
	char		relpersistence;

	/* table alias & optional column aliases */
	Alias	   *alias;

	/* token location, or -1 if unknown */
	ParseLoc	location;
} RangeVar;

typedef enum TableFuncType
{
	TFT_XMLTABLE,
	TFT_JSON_TABLE,
} TableFuncType;

/*
 * TableFunc - node for a table function, such as XMLTABLE and JSON_TABLE.
 *
 * Entries in the ns_names list are either String nodes containing
 * literal namespace names, or NULL pointers to represent DEFAULT.
 */
typedef struct TableFunc
{
	NodeTag		type;
	/* XMLTABLE or JSON_TABLE */
	TableFuncType functype;
	/* list of namespace URI expressions */
	List	   *ns_uris pg_node_attr(query_jumble_ignore);
	/* list of namespace names or NULL */
	List	   *ns_names pg_node_attr(query_jumble_ignore);
	/* input document expression */
	Node	   *docexpr;
	/* row filter expression */
	Node	   *rowexpr;
	/* column names (list of String) */
	List	   *colnames pg_node_attr(query_jumble_ignore);
	/* OID list of column type OIDs */
	List	   *coltypes pg_node_attr(query_jumble_ignore);
	/* integer list of column typmods */
	List	   *coltypmods pg_node_attr(query_jumble_ignore);
	/* OID list of column collation OIDs */
	List	   *colcollations pg_node_attr(query_jumble_ignore);
	/* list of column filter expressions */
	List	   *colexprs;
	/* list of column default expressions */
	List	   *coldefexprs pg_node_attr(query_jumble_ignore);
	/* JSON_TABLE: list of column value expressions */
	List	   *colvalexprs pg_node_attr(query_jumble_ignore);
	/* JSON_TABLE: list of PASSING argument expressions */
	List	   *passingvalexprs pg_node_attr(query_jumble_ignore);
	/* nullability flag for each output column */
	Bitmapset  *notnulls pg_node_attr(query_jumble_ignore);
	/* JSON_TABLE plan */
	Node	   *plan pg_node_attr(query_jumble_ignore);
	/* counts from 0; -1 if none specified */
	int			ordinalitycol pg_node_attr(query_jumble_ignore);
	/* token location, or -1 if unknown */
	ParseLoc	location;
} TableFunc;

/*
 * IntoClause - target information for SELECT INTO, CREATE TABLE AS, and
 * CREATE MATERIALIZED VIEW
 *
 * For CREATE MATERIALIZED VIEW, viewQuery is the parsed-but-not-rewritten
 * SELECT Query for the view; otherwise it's NULL.  This is irrelevant in
 * the query jumbling as CreateTableAsStmt already includes a reference to
 * its own Query, so ignore it.  (We declare it as struct Query* to avoid a
 * forward reference.)
 */
typedef struct IntoClause
{
	NodeTag		type;

	RangeVar   *rel;			/* target relation name */
	List	   *colNames;		/* column names to assign, or NIL */
	char	   *accessMethod;	/* table access method */
	List	   *options;		/* options from WITH clause */
	OnCommitAction onCommit;	/* what do we do at COMMIT? */
	char	   *tableSpaceName; /* table space to use, or NULL */
	/* materialized view's SELECT query */
	struct Query *viewQuery pg_node_attr(query_jumble_ignore);
	bool		skipData;		/* true for WITH NO DATA */
} IntoClause;


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
	pg_node_attr(abstract)

	NodeTag		type;
} Expr;

/*
 * Var - expression node representing a variable (ie, a table column)
 *
 * In the parser and planner, varno and varattno identify the semantic
 * referent, which is a base-relation column unless the reference is to a join
 * USING column that isn't semantically equivalent to either join input column
 * (because it is a FULL join or the input column requires a type coercion).
 * In those cases varno and varattno refer to the JOIN RTE.  (Early in the
 * planner, we replace such join references by the implied expression; but up
 * till then we want join reference Vars to keep their original identity for
 * query-printing purposes.)
 *
 * At the end of planning, Var nodes appearing in upper-level plan nodes are
 * reassigned to point to the outputs of their subplans; for example, in a
 * join node varno becomes INNER_VAR or OUTER_VAR and varattno becomes the
 * index of the proper element of that subplan's target list.  Similarly,
 * INDEX_VAR is used to identify Vars that reference an index column rather
 * than a heap column.  (In ForeignScan and CustomScan plan nodes, INDEX_VAR
 * is abused to signify references to columns of a custom scan tuple type.)
 *
 * ROWID_VAR is used in the planner to identify nonce variables that carry
 * row identity information during UPDATE/DELETE/MERGE.  This value should
 * never be seen outside the planner.
 *
 * varnullingrels is the set of RT indexes of outer joins that can force
 * the Var's value to null (at the point where it appears in the query).
 * See optimizer/README for discussion of that.
 *
 * varlevelsup is greater than zero in Vars that represent outer references.
 * Note that it affects the meaning of all of varno, varnullingrels, and
 * varnosyn, all of which refer to the range table of that query level.
 *
 * In the parser, varnosyn and varattnosyn are either identical to
 * varno/varattno, or they specify the column's position in an aliased JOIN
 * RTE that hides the semantic referent RTE's refname.  This is a syntactic
 * identifier as opposed to the semantic identifier; it tells ruleutils.c
 * how to print the Var properly.  varnosyn/varattnosyn retain their values
 * throughout planning and execution, so they are particularly helpful to
 * identify Vars when debugging.  Note, however, that a Var that is generated
 * in the planner and doesn't correspond to any simple relation column may
 * have varnosyn = varattnosyn = 0.
 */
#define    INNER_VAR		(-1)	/* reference to inner subplan */
#define    OUTER_VAR		(-2)	/* reference to outer subplan */
#define    INDEX_VAR		(-3)	/* reference to index column */
#define    ROWID_VAR		(-4)	/* row identity column during planning */

#define IS_SPECIAL_VARNO(varno)		((int) (varno) < 0)

/* Symbols for the indexes of the special RTE entries in rules */
#define    PRS2_OLD_VARNO			1
#define    PRS2_NEW_VARNO			2

typedef struct Var
{
	Expr		xpr;

	/*
	 * index of this var's relation in the range table, or
	 * INNER_VAR/OUTER_VAR/etc
	 */
	int			varno;

	/*
	 * attribute number of this var, or zero for all attrs ("whole-row Var")
	 */
	AttrNumber	varattno;

	/* pg_type OID for the type of this var */
	Oid			vartype pg_node_attr(query_jumble_ignore);
	/* pg_attribute typmod value */
	int32		vartypmod pg_node_attr(query_jumble_ignore);
	/* OID of collation, or InvalidOid if none */
	Oid			varcollid pg_node_attr(query_jumble_ignore);

	/*
	 * RT indexes of outer joins that can replace the Var's value with null.
	 * We can omit varnullingrels in the query jumble, because it's fully
	 * determined by varno/varlevelsup plus the Var's query location.
	 */
	Bitmapset  *varnullingrels pg_node_attr(query_jumble_ignore);

	/*
	 * for subquery variables referencing outer relations; 0 in a normal var,
	 * >0 means N levels up
	 */
	Index		varlevelsup;

	/*
	 * varnosyn/varattnosyn are ignored for equality, because Vars with
	 * different syntactic identifiers are semantically the same as long as
	 * their varno/varattno match.
	 */
	/* syntactic relation index (0 if unknown) */
	Index		varnosyn pg_node_attr(equal_ignore, query_jumble_ignore);
	/* syntactic attribute number */
	AttrNumber	varattnosyn pg_node_attr(equal_ignore, query_jumble_ignore);

	/* token location, or -1 if unknown */
	ParseLoc	location;
} Var;

/*
 * Const
 *
 * Note: for varlena data types, we make a rule that a Const node's value
 * must be in non-extended form (4-byte header, no compression or external
 * references).  This ensures that the Const node is self-contained and makes
 * it more likely that equal() will see logically identical values as equal.
 *
 * Only the constant type OID is relevant for the query jumbling.
 */
typedef struct Const
{
	pg_node_attr(custom_copy_equal, custom_read_write)

	Expr		xpr;
	/* pg_type OID of the constant's datatype */
	Oid			consttype;
	/* typmod value, if any */
	int32		consttypmod pg_node_attr(query_jumble_ignore);
	/* OID of collation, or InvalidOid if none */
	Oid			constcollid pg_node_attr(query_jumble_ignore);
	/* typlen of the constant's datatype */
	int			constlen pg_node_attr(query_jumble_ignore);
	/* the constant's value */
	Datum		constvalue pg_node_attr(query_jumble_ignore);
	/* whether the constant is null (if true, constvalue is undefined) */
	bool		constisnull pg_node_attr(query_jumble_ignore);

	/*
	 * Whether this datatype is passed by value.  If true, then all the
	 * information is stored in the Datum.  If false, then the Datum contains
	 * a pointer to the information.
	 */
	bool		constbyval pg_node_attr(query_jumble_ignore);

	/*
	 * token location, or -1 if unknown.  All constants are tracked as
	 * locations in query jumbling, to be marked as parameters.
	 */
	ParseLoc	location pg_node_attr(query_jumble_location);
} Const;

/*
 * Param
 *
 *		paramkind specifies the kind of parameter. The possible values
 *		for this field are:
 *
 *		PARAM_EXTERN:  The parameter value is supplied from outside the plan.
 *				Such parameters are numbered from 1 to n.
 *
 *		PARAM_EXEC:  The parameter is an internal executor parameter, used
 *				for passing values into and out of sub-queries or from
 *				nestloop joins to their inner scans.
 *				For historical reasons, such parameters are numbered from 0.
 *				These numbers are independent of PARAM_EXTERN numbers.
 *
 *		PARAM_SUBLINK:	The parameter represents an output column of a SubLink
 *				node's sub-select.  The column number is contained in the
 *				`paramid' field.  (This type of Param is converted to
 *				PARAM_EXEC during planning.)
 *
 *		PARAM_MULTIEXPR:  Like PARAM_SUBLINK, the parameter represents an
 *				output column of a SubLink node's sub-select, but here, the
 *				SubLink is always a MULTIEXPR SubLink.  The high-order 16 bits
 *				of the `paramid' field contain the SubLink's subLinkId, and
 *				the low-order 16 bits contain the column number.  (This type
 *				of Param is also converted to PARAM_EXEC during planning.)
 */
typedef enum ParamKind
{
	PARAM_EXTERN,
	PARAM_EXEC,
	PARAM_SUBLINK,
	PARAM_MULTIEXPR,
} ParamKind;

typedef struct Param
{
	Expr		xpr;
	ParamKind	paramkind;		/* kind of parameter. See above */
	int			paramid;		/* numeric ID for parameter */
	Oid			paramtype;		/* pg_type OID of parameter's datatype */
	/* typmod value, if known */
	int32		paramtypmod pg_node_attr(query_jumble_ignore);
	/* OID of collation, or InvalidOid if none */
	Oid			paramcollid pg_node_attr(query_jumble_ignore);
	/* token location, or -1 if unknown */
	ParseLoc	location;
} Param;

/*
 * Aggref
 *
 * The aggregate's args list is a targetlist, ie, a list of TargetEntry nodes.
 *
 * For a normal (non-ordered-set) aggregate, the non-resjunk TargetEntries
 * represent the aggregate's regular arguments (if any) and resjunk TLEs can
 * be added at the end to represent ORDER BY expressions that are not also
 * arguments.  As in a top-level Query, the TLEs can be marked with
 * ressortgroupref indexes to let them be referenced by SortGroupClause
 * entries in the aggorder and/or aggdistinct lists.  This represents ORDER BY
 * and DISTINCT operations to be applied to the aggregate input rows before
 * they are passed to the transition function.  The grammar only allows a
 * simple "DISTINCT" specifier for the arguments, but we use the full
 * query-level representation to allow more code sharing.
 *
 * For an ordered-set aggregate, the args list represents the WITHIN GROUP
 * (aggregated) arguments, all of which will be listed in the aggorder list.
 * DISTINCT is not supported in this case, so aggdistinct will be NIL.
 * The direct arguments appear in aggdirectargs (as a list of plain
 * expressions, not TargetEntry nodes).
 *
 * aggtranstype is the data type of the state transition values for this
 * aggregate (resolved to an actual type, if agg's transtype is polymorphic).
 * This is determined during planning and is InvalidOid before that.
 *
 * aggargtypes is an OID list of the data types of the direct and regular
 * arguments.  Normally it's redundant with the aggdirectargs and args lists,
 * but in a combining aggregate, it's not because the args list has been
 * replaced with a single argument representing the partial-aggregate
 * transition values.
 *
 * aggpresorted is set by the query planner for ORDER BY and DISTINCT
 * aggregates where the chosen plan provides presorted input for this
 * aggregate during execution.
 *
 * aggsplit indicates the expected partial-aggregation mode for the Aggref's
 * parent plan node.  It's always set to AGGSPLIT_SIMPLE in the parser, but
 * the planner might change it to something else.  We use this mainly as
 * a crosscheck that the Aggrefs match the plan; but note that when aggsplit
 * indicates a non-final mode, aggtype reflects the transition data type
 * not the SQL-level output type of the aggregate.
 *
 * aggno and aggtransno are -1 in the parse stage, and are set in planning.
 * Aggregates with the same 'aggno' represent the same aggregate expression,
 * and can share the result.  Aggregates with same 'transno' but different
 * 'aggno' can share the same transition state, only the final function needs
 * to be called separately.
 *
 * Information related to collations, transition types and internal states
 * are irrelevant for the query jumbling.
 */
typedef struct Aggref
{
	Expr		xpr;

	/* pg_proc Oid of the aggregate */
	Oid			aggfnoid;

	/* type Oid of result of the aggregate */
	Oid			aggtype pg_node_attr(query_jumble_ignore);

	/* OID of collation of result */
	Oid			aggcollid pg_node_attr(query_jumble_ignore);

	/* OID of collation that function should use */
	Oid			inputcollid pg_node_attr(query_jumble_ignore);

	/*
	 * type Oid of aggregate's transition value; ignored for equal since it
	 * might not be set yet
	 */
	Oid			aggtranstype pg_node_attr(equal_ignore, query_jumble_ignore);

	/* type Oids of direct and aggregated args */
	List	   *aggargtypes pg_node_attr(query_jumble_ignore);

	/* direct arguments, if an ordered-set agg */
	List	   *aggdirectargs;

	/* aggregated arguments and sort expressions */
	List	   *args;

	/* ORDER BY (list of SortGroupClause) */
	List	   *aggorder;

	/* DISTINCT (list of SortGroupClause) */
	List	   *aggdistinct;

	/* FILTER expression, if any */
	Expr	   *aggfilter;

	/* true if argument list was really '*' */
	bool		aggstar pg_node_attr(query_jumble_ignore);

	/*
	 * true if variadic arguments have been combined into an array last
	 * argument
	 */
	bool		aggvariadic pg_node_attr(query_jumble_ignore);

	/* aggregate kind (see pg_aggregate.h) */
	char		aggkind pg_node_attr(query_jumble_ignore);

	/* aggregate input already sorted */
	bool		aggpresorted pg_node_attr(equal_ignore, query_jumble_ignore);

	/* > 0 if agg belongs to outer query */
	Index		agglevelsup pg_node_attr(query_jumble_ignore);

	/* expected agg-splitting mode of parent Agg */
	AggSplit	aggsplit pg_node_attr(query_jumble_ignore);

	/* unique ID within the Agg node */
	int			aggno pg_node_attr(query_jumble_ignore);

	/* unique ID of transition state in the Agg */
	int			aggtransno pg_node_attr(query_jumble_ignore);

	/* token location, or -1 if unknown */
	ParseLoc	location;
} Aggref;

/*
 * GroupingFunc
 *
 * A GroupingFunc is a GROUPING(...) expression, which behaves in many ways
 * like an aggregate function (e.g. it "belongs" to a specific query level,
 * which might not be the one immediately containing it), but also differs in
 * an important respect: it never evaluates its arguments, they merely
 * designate expressions from the GROUP BY clause of the query level to which
 * it belongs.
 *
 * The spec defines the evaluation of GROUPING() purely by syntactic
 * replacement, but we make it a real expression for optimization purposes so
 * that one Agg node can handle multiple grouping sets at once.  Evaluating the
 * result only needs the column positions to check against the grouping set
 * being projected.  However, for EXPLAIN to produce meaningful output, we have
 * to keep the original expressions around, since expression deparse does not
 * give us any feasible way to get at the GROUP BY clause.
 *
 * Also, we treat two GroupingFunc nodes as equal if they have equal arguments
 * lists and agglevelsup, without comparing the refs and cols annotations.
 *
 * In raw parse output we have only the args list; parse analysis fills in the
 * refs list, and the planner fills in the cols list.
 *
 * All the fields used as information for an internal state are irrelevant
 * for the query jumbling.
 */
typedef struct GroupingFunc
{
	Expr		xpr;

	/* arguments, not evaluated but kept for benefit of EXPLAIN etc. */
	List	   *args pg_node_attr(query_jumble_ignore);

	/* ressortgrouprefs of arguments */
	List	   *refs pg_node_attr(equal_ignore);

	/* actual column positions set by planner */
	List	   *cols pg_node_attr(equal_ignore, query_jumble_ignore);

	/* same as Aggref.agglevelsup */
	Index		agglevelsup;

	/* token location */
	ParseLoc	location;
} GroupingFunc;

/*
 * WindowFunc
 *
 * Collation information is irrelevant for the query jumbling, as is the
 * internal state information of the node like "winstar" and "winagg".
 */
typedef struct WindowFunc
{
	Expr		xpr;
	/* pg_proc Oid of the function */
	Oid			winfnoid;
	/* type Oid of result of the window function */
	Oid			wintype pg_node_attr(query_jumble_ignore);
	/* OID of collation of result */
	Oid			wincollid pg_node_attr(query_jumble_ignore);
	/* OID of collation that function should use */
	Oid			inputcollid pg_node_attr(query_jumble_ignore);
	/* arguments to the window function */
	List	   *args;
	/* FILTER expression, if any */
	Expr	   *aggfilter;
	/* List of WindowFuncRunConditions to help short-circuit execution */
	List	   *runCondition pg_node_attr(query_jumble_ignore);
	/* index of associated WindowClause */
	Index		winref;
	/* true if argument list was really '*' */
	bool		winstar pg_node_attr(query_jumble_ignore);
	/* is function a simple aggregate? */
	bool		winagg pg_node_attr(query_jumble_ignore);
	/* token location, or -1 if unknown */
	ParseLoc	location;
} WindowFunc;

/*
 * WindowFuncRunCondition
 *
 * Represents intermediate OpExprs which will be used by WindowAgg to
 * short-circuit execution.
 */
typedef struct WindowFuncRunCondition
{
	Expr		xpr;

	/* PG_OPERATOR OID of the operator */
	Oid			opno;
	/* OID of collation that operator should use */
	Oid			inputcollid pg_node_attr(query_jumble_ignore);

	/*
	 * true of WindowFunc belongs on the left of the resulting OpExpr or false
	 * if the WindowFunc is on the right.
	 */
	bool		wfunc_left;

	/*
	 * The Expr being compared to the WindowFunc to use in the OpExpr in the
	 * WindowAgg's runCondition
	 */
	Expr	   *arg;
} WindowFuncRunCondition;

/*
 * MergeSupportFunc
 *
 * A MergeSupportFunc is a merge support function expression that can only
 * appear in the RETURNING list of a MERGE command.  It returns information
 * about the currently executing merge action.
 *
 * Currently, the only supported function is MERGE_ACTION(), which returns the
 * command executed ("INSERT", "UPDATE", or "DELETE").
 */
typedef struct MergeSupportFunc
{
	Expr		xpr;
	/* type Oid of result */
	Oid			msftype;
	/* OID of collation, or InvalidOid if none */
	Oid			msfcollid;
	/* token location, or -1 if unknown */
	ParseLoc	location;
} MergeSupportFunc;

/*
 * SubscriptingRef: describes a subscripting operation over a container
 * (array, etc).
 *
 * A SubscriptingRef can describe fetching a single element from a container,
 * fetching a part of a container (e.g. an array slice), storing a single
 * element into a container, or storing a slice.  The "store" cases work with
 * an initial container value and a source value that is inserted into the
 * appropriate part of the container; the result of the operation is an
 * entire new modified container value.
 *
 * If reflowerindexpr = NIL, then we are fetching or storing a single container
 * element at the subscripts given by refupperindexpr. Otherwise we are
 * fetching or storing a container slice, that is a rectangular subcontainer
 * with lower and upper bounds given by the index expressions.
 * reflowerindexpr must be the same length as refupperindexpr when it
 * is not NIL.
 *
 * In the slice case, individual expressions in the subscript lists can be
 * NULL, meaning "substitute the array's current lower or upper bound".
 * (Non-array containers may or may not support this.)
 *
 * refcontainertype is the actual container type that determines the
 * subscripting semantics.  (This will generally be either the exposed type of
 * refexpr, or the base type if that is a domain.)  refelemtype is the type of
 * the container's elements; this is saved for the use of the subscripting
 * functions, but is not used by the core code.  refrestype, reftypmod, and
 * refcollid describe the type of the SubscriptingRef's result.  In a store
 * expression, refrestype will always match refcontainertype; in a fetch,
 * it could be refelemtype for an element fetch, or refcontainertype for a
 * slice fetch, or possibly something else as determined by type-specific
 * subscripting logic.  Likewise, reftypmod and refcollid will match the
 * container's properties in a store, but could be different in a fetch.
 *
 * Any internal state data is ignored for the query jumbling.
 *
 * Note: for the cases where a container is returned, if refexpr yields a R/W
 * expanded container, then the implementation is allowed to modify that
 * object in-place and return the same object.
 */
typedef struct SubscriptingRef
{
	Expr		xpr;
	/* type of the container proper */
	Oid			refcontainertype pg_node_attr(query_jumble_ignore);
	/* the container type's pg_type.typelem */
	Oid			refelemtype pg_node_attr(query_jumble_ignore);
	/* type of the SubscriptingRef's result */
	Oid			refrestype pg_node_attr(query_jumble_ignore);
	/* typmod of the result */
	int32		reftypmod pg_node_attr(query_jumble_ignore);
	/* collation of result, or InvalidOid if none */
	Oid			refcollid pg_node_attr(query_jumble_ignore);
	/* expressions that evaluate to upper container indexes */
	List	   *refupperindexpr;

	/*
	 * expressions that evaluate to lower container indexes, or NIL for single
	 * container element.
	 */
	List	   *reflowerindexpr;
	/* the expression that evaluates to a container value */
	Expr	   *refexpr;
	/* expression for the source value, or NULL if fetch */
	Expr	   *refassgnexpr;
} SubscriptingRef;

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
	COERCION_PLPGSQL,			/* if no assignment cast, use CoerceViaIO */
	COERCION_EXPLICIT,			/* explicit cast operation */
} CoercionContext;

/*
 * CoercionForm - how to display a FuncExpr or related node
 *
 * "Coercion" is a bit of a misnomer, since this value records other
 * special syntaxes besides casts, but for now we'll keep this naming.
 *
 * NB: equal() ignores CoercionForm fields, therefore this *must* not carry
 * any semantically significant information.  We need that behavior so that
 * the planner will consider equivalent implicit and explicit casts to be
 * equivalent.  In cases where those actually behave differently, the coercion
 * function's arguments will be different.
 */
typedef enum CoercionForm
{
	COERCE_EXPLICIT_CALL,		/* display as a function call */
	COERCE_EXPLICIT_CAST,		/* display as an explicit cast */
	COERCE_IMPLICIT_CAST,		/* implicit cast, so hide it */
	COERCE_SQL_SYNTAX,			/* display with SQL-mandated special syntax */
} CoercionForm;

/*
 * FuncExpr - expression node for a function call
 *
 * Collation information is irrelevant for the query jumbling, only the
 * arguments and the function OID matter.
 */
typedef struct FuncExpr
{
	Expr		xpr;
	/* PG_PROC OID of the function */
	Oid			funcid;
	/* PG_TYPE OID of result value */
	Oid			funcresulttype pg_node_attr(query_jumble_ignore);
	/* true if function returns set */
	bool		funcretset pg_node_attr(query_jumble_ignore);

	/*
	 * true if variadic arguments have been combined into an array last
	 * argument
	 */
	bool		funcvariadic pg_node_attr(query_jumble_ignore);
	/* how to display this function call */
	CoercionForm funcformat pg_node_attr(query_jumble_ignore);
	/* OID of collation of result */
	Oid			funccollid pg_node_attr(query_jumble_ignore);
	/* OID of collation that function should use */
	Oid			inputcollid pg_node_attr(query_jumble_ignore);
	/* arguments to the function */
	List	   *args;
	/* token location, or -1 if unknown */
	ParseLoc	location;
} FuncExpr;

/*
 * NamedArgExpr - a named argument of a function
 *
 * This node type can only appear in the args list of a FuncCall or FuncExpr
 * node.  We support pure positional call notation (no named arguments),
 * named notation (all arguments are named), and mixed notation (unnamed
 * arguments followed by named ones).
 *
 * Parse analysis sets argnumber to the positional index of the argument,
 * but doesn't rearrange the argument list.
 *
 * The planner will convert argument lists to pure positional notation
 * during expression preprocessing, so execution never sees a NamedArgExpr.
 */
typedef struct NamedArgExpr
{
	Expr		xpr;
	/* the argument expression */
	Expr	   *arg;
	/* the name */
	char	   *name pg_node_attr(query_jumble_ignore);
	/* argument's number in positional notation */
	int			argnumber;
	/* argument name location, or -1 if unknown */
	ParseLoc	location;
} NamedArgExpr;

/*
 * OpExpr - expression node for an operator invocation
 *
 * Semantically, this is essentially the same as a function call.
 *
 * Note that opfuncid is not necessarily filled in immediately on creation
 * of the node.  The planner makes sure it is valid before passing the node
 * tree to the executor, but during parsing/planning opfuncid can be 0.
 * Therefore, equal() will accept a zero value as being equal to other values.
 *
 * Internal state information and collation data is irrelevant for the query
 * jumbling.
 */
typedef struct OpExpr
{
	Expr		xpr;

	/* PG_OPERATOR OID of the operator */
	Oid			opno;

	/* PG_PROC OID of underlying function */
	Oid			opfuncid pg_node_attr(equal_ignore_if_zero, query_jumble_ignore);

	/* PG_TYPE OID of result value */
	Oid			opresulttype pg_node_attr(query_jumble_ignore);

	/* true if operator returns set */
	bool		opretset pg_node_attr(query_jumble_ignore);

	/* OID of collation of result */
	Oid			opcollid pg_node_attr(query_jumble_ignore);

	/* OID of collation that operator should use */
	Oid			inputcollid pg_node_attr(query_jumble_ignore);

	/* arguments to the operator (1 or 2) */
	List	   *args;

	/* token location, or -1 if unknown */
	ParseLoc	location;
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
 * NullIfExpr - a NULLIF expression
 *
 * Like DistinctExpr, this is represented the same as an OpExpr referencing
 * the "=" operator for x and y.
 */
typedef OpExpr NullIfExpr;

/*
 * ScalarArrayOpExpr - expression node for "scalar op ANY/ALL (array)"
 *
 * The operator must yield boolean.  It is applied to the left operand
 * and each element of the righthand array, and the results are combined
 * with OR or AND (for ANY or ALL respectively).  The node representation
 * is almost the same as for the underlying operator, but we need a useOr
 * flag to remember whether it's ANY or ALL, and we don't have to store
 * the result type (or the collation) because it must be boolean.
 *
 * A ScalarArrayOpExpr with a valid hashfuncid is evaluated during execution
 * by building a hash table containing the Const values from the RHS arg.
 * This table is probed during expression evaluation.  The planner will set
 * hashfuncid to the hash function which must be used to build and probe the
 * hash table.  The executor determines if it should use hash-based checks or
 * the more traditional means based on if the hashfuncid is set or not.
 *
 * When performing hashed NOT IN, the negfuncid will also be set to the
 * equality function which the hash table must use to build and probe the hash
 * table.  opno and opfuncid will remain set to the <> operator and its
 * corresponding function and won't be used during execution.  For
 * non-hashtable based NOT INs, negfuncid will be set to InvalidOid.  See
 * convert_saop_to_hashed_saop().
 *
 * Similar to OpExpr, opfuncid, hashfuncid, and negfuncid are not necessarily
 * filled in right away, so will be ignored for equality if they are not set
 * yet.
 *
 * OID entries of the internal function types are irrelevant for the query
 * jumbling, but the operator OID and the arguments are.
 */
typedef struct ScalarArrayOpExpr
{
	Expr		xpr;

	/* PG_OPERATOR OID of the operator */
	Oid			opno;

	/* PG_PROC OID of comparison function */
	Oid			opfuncid pg_node_attr(equal_ignore_if_zero, query_jumble_ignore);

	/* PG_PROC OID of hash func or InvalidOid */
	Oid			hashfuncid pg_node_attr(equal_ignore_if_zero, query_jumble_ignore);

	/* PG_PROC OID of negator of opfuncid function or InvalidOid.  See above */
	Oid			negfuncid pg_node_attr(equal_ignore_if_zero, query_jumble_ignore);

	/* true for ANY, false for ALL */
	bool		useOr;

	/* OID of collation that operator should use */
	Oid			inputcollid pg_node_attr(query_jumble_ignore);

	/* the scalar and array operands */
	List	   *args;

	/* token location, or -1 if unknown */
	ParseLoc	location;
} ScalarArrayOpExpr;

/*
 * BoolExpr - expression node for the basic Boolean operators AND, OR, NOT
 *
 * Notice the arguments are given as a List.  For NOT, of course the list
 * must always have exactly one element.  For AND and OR, there can be two
 * or more arguments.
 */
typedef enum BoolExprType
{
	AND_EXPR, OR_EXPR, NOT_EXPR
} BoolExprType;

typedef struct BoolExpr
{
	pg_node_attr(custom_read_write)

	Expr		xpr;
	BoolExprType boolop;
	List	   *args;			/* arguments to this expression */
	ParseLoc	location;		/* token location, or -1 if unknown */
} BoolExpr;

/*
 * SubLink
 *
 * A SubLink represents a subselect appearing in an expression, and in some
 * cases also the combining operator(s) just above it.  The subLinkType
 * indicates the form of the expression represented:
 *	EXISTS_SUBLINK		EXISTS(SELECT ...)
 *	ALL_SUBLINK			(lefthand) op ALL (SELECT ...)
 *	ANY_SUBLINK			(lefthand) op ANY (SELECT ...)
 *	ROWCOMPARE_SUBLINK	(lefthand) op (SELECT ...)
 *	EXPR_SUBLINK		(SELECT with single targetlist item ...)
 *	MULTIEXPR_SUBLINK	(SELECT with multiple targetlist items ...)
 *	ARRAY_SUBLINK		ARRAY(SELECT with single targetlist item ...)
 *	CTE_SUBLINK			WITH query (never actually part of an expression)
 * For ALL, ANY, and ROWCOMPARE, the lefthand is a list of expressions of the
 * same length as the subselect's targetlist.  ROWCOMPARE will *always* have
 * a list with more than one entry; if the subselect has just one target
 * then the parser will create an EXPR_SUBLINK instead (and any operator
 * above the subselect will be represented separately).
 * ROWCOMPARE, EXPR, and MULTIEXPR require the subselect to deliver at most
 * one row (if it returns no rows, the result is NULL).
 * ALL, ANY, and ROWCOMPARE require the combining operators to deliver boolean
 * results.  ALL and ANY combine the per-row results using AND and OR
 * semantics respectively.
 * ARRAY requires just one target column, and creates an array of the target
 * column's type using any number of rows resulting from the subselect.
 *
 * SubLink is classed as an Expr node, but it is not actually executable;
 * it must be replaced in the expression tree by a SubPlan node during
 * planning.
 *
 * NOTE: in the raw output of gram.y, testexpr contains just the raw form
 * of the lefthand expression (if any), and operName is the String name of
 * the combining operator.  Also, subselect is a raw parsetree.  During parse
 * analysis, the parser transforms testexpr into a complete boolean expression
 * that compares the lefthand value(s) to PARAM_SUBLINK nodes representing the
 * output columns of the subselect.  And subselect is transformed to a Query.
 * This is the representation seen in saved rules and in the rewriter.
 *
 * In EXISTS, EXPR, MULTIEXPR, and ARRAY SubLinks, testexpr and operName
 * are unused and are always null.
 *
 * subLinkId is currently used only for MULTIEXPR SubLinks, and is zero in
 * other SubLinks.  This number identifies different multiple-assignment
 * subqueries within an UPDATE statement's SET list.  It is unique only
 * within a particular targetlist.  The output column(s) of the MULTIEXPR
 * are referenced by PARAM_MULTIEXPR Params appearing elsewhere in the tlist.
 *
 * The CTE_SUBLINK case never occurs in actual SubLink nodes, but it is used
 * in SubPlans generated for WITH subqueries.
 */
typedef enum SubLinkType
{
	EXISTS_SUBLINK,
	ALL_SUBLINK,
	ANY_SUBLINK,
	ROWCOMPARE_SUBLINK,
	EXPR_SUBLINK,
	MULTIEXPR_SUBLINK,
	ARRAY_SUBLINK,
	CTE_SUBLINK,				/* for SubPlans only */
} SubLinkType;


typedef struct SubLink
{
	Expr		xpr;
	SubLinkType subLinkType;	/* see above */
	int			subLinkId;		/* ID (1..n); 0 if not MULTIEXPR */
	Node	   *testexpr;		/* outer-query test for ALL/ANY/ROWCOMPARE */
	/* originally specified operator name */
	List	   *operName pg_node_attr(query_jumble_ignore);
	/* subselect as Query* or raw parsetree */
	Node	   *subselect;
	ParseLoc	location;		/* token location, or -1 if unknown */
} SubLink;

/*
 * SubPlan - executable expression node for a subplan (sub-SELECT)
 *
 * The planner replaces SubLink nodes in expression trees with SubPlan
 * nodes after it has finished planning the subquery.  SubPlan references
 * a sub-plantree stored in the subplans list of the toplevel PlannedStmt.
 * (We avoid a direct link to make it easier to copy expression trees
 * without causing multiple processing of the subplan.)
 *
 * In an ordinary subplan, testexpr points to an executable expression
 * (OpExpr, an AND/OR tree of OpExprs, or RowCompareExpr) for the combining
 * operator(s); the left-hand arguments are the original lefthand expressions,
 * and the right-hand arguments are PARAM_EXEC Param nodes representing the
 * outputs of the sub-select.  (NOTE: runtime coercion functions may be
 * inserted as well.)  This is just the same expression tree as testexpr in
 * the original SubLink node, but the PARAM_SUBLINK nodes are replaced by
 * suitably numbered PARAM_EXEC nodes.
 *
 * If the sub-select becomes an initplan rather than a subplan, the executable
 * expression is part of the outer plan's expression tree (and the SubPlan
 * node itself is not, but rather is found in the outer plan's initPlan
 * list).  In this case testexpr is NULL to avoid duplication.
 *
 * The planner also derives lists of the values that need to be passed into
 * and out of the subplan.  Input values are represented as a list "args" of
 * expressions to be evaluated in the outer-query context (currently these
 * args are always just Vars, but in principle they could be any expression).
 * The values are assigned to the global PARAM_EXEC params indexed by parParam
 * (the parParam and args lists must have the same ordering).  setParam is a
 * list of the PARAM_EXEC params that are computed by the sub-select, if it
 * is an initplan or MULTIEXPR plan; they are listed in order by sub-select
 * output column position.  (parParam and setParam are integer Lists, not
 * Bitmapsets, because their ordering is significant.)
 *
 * Also, the planner computes startup and per-call costs for use of the
 * SubPlan.  Note that these include the cost of the subquery proper,
 * evaluation of the testexpr if any, and any hashtable management overhead.
 */
typedef struct SubPlan
{
	pg_node_attr(no_query_jumble)

	Expr		xpr;
	/* Fields copied from original SubLink: */
	SubLinkType subLinkType;	/* see above */
	/* The combining operators, transformed to an executable expression: */
	Node	   *testexpr;		/* OpExpr or RowCompareExpr expression tree */
	List	   *paramIds;		/* IDs of Params embedded in the above */
	/* Identification of the Plan tree to use: */
	int			plan_id;		/* Index (from 1) in PlannedStmt.subplans */
	/* Identification of the SubPlan for EXPLAIN and debugging purposes: */
	char	   *plan_name;		/* A name assigned during planning */
	/* Extra data useful for determining subplan's output type: */
	Oid			firstColType;	/* Type of first column of subplan result */
	int32		firstColTypmod; /* Typmod of first column of subplan result */
	Oid			firstColCollation;	/* Collation of first column of subplan
									 * result */
	/* Information about execution strategy: */
	bool		useHashTable;	/* true to store subselect output in a hash
								 * table (implies we are doing "IN") */
	bool		unknownEqFalse; /* true if it's okay to return FALSE when the
								 * spec result is UNKNOWN; this allows much
								 * simpler handling of null values */
	bool		parallel_safe;	/* is the subplan parallel-safe? */
	/* Note: parallel_safe does not consider contents of testexpr or args */
	/* Information for passing params into and out of the subselect: */
	/* setParam and parParam are lists of integers (param IDs) */
	List	   *setParam;		/* initplan and MULTIEXPR subqueries have to
								 * set these Params for parent plan */
	List	   *parParam;		/* indices of input Params from parent plan */
	List	   *args;			/* exprs to pass as parParam values */
	/* Estimated execution costs: */
	Cost		startup_cost;	/* one-time setup cost */
	Cost		per_call_cost;	/* cost for each subplan evaluation */
} SubPlan;

/*
 * AlternativeSubPlan - expression node for a choice among SubPlans
 *
 * This is used only transiently during planning: by the time the plan
 * reaches the executor, all AlternativeSubPlan nodes have been removed.
 *
 * The subplans are given as a List so that the node definition need not
 * change if there's ever more than two alternatives.  For the moment,
 * though, there are always exactly two; and the first one is the fast-start
 * plan.
 */
typedef struct AlternativeSubPlan
{
	pg_node_attr(no_query_jumble)

	Expr		xpr;
	List	   *subplans;		/* SubPlan(s) with equivalent results */
} AlternativeSubPlan;

/* ----------------
 * FieldSelect
 *
 * FieldSelect represents the operation of extracting one field from a tuple
 * value.  At runtime, the input expression is expected to yield a rowtype
 * Datum.  The specified field number is extracted and returned as a Datum.
 * ----------------
 */

typedef struct FieldSelect
{
	Expr		xpr;
	Expr	   *arg;			/* input expression */
	AttrNumber	fieldnum;		/* attribute number of field to extract */
	/* type of the field (result type of this node) */
	Oid			resulttype pg_node_attr(query_jumble_ignore);
	/* output typmod (usually -1) */
	int32		resulttypmod pg_node_attr(query_jumble_ignore);
	/* OID of collation of the field */
	Oid			resultcollid pg_node_attr(query_jumble_ignore);
} FieldSelect;

/* ----------------
 * FieldStore
 *
 * FieldStore represents the operation of modifying one field in a tuple
 * value, yielding a new tuple value (the input is not touched!).  Like
 * the assign case of SubscriptingRef, this is used to implement UPDATE of a
 * portion of a column.
 *
 * resulttype is always a named composite type (not a domain).  To update
 * a composite domain value, apply CoerceToDomain to the FieldStore.
 *
 * A single FieldStore can actually represent updates of several different
 * fields.  The parser only generates FieldStores with single-element lists,
 * but the planner will collapse multiple updates of the same base column
 * into one FieldStore.
 * ----------------
 */

typedef struct FieldStore
{
	Expr		xpr;
	Expr	   *arg;			/* input tuple value */
	List	   *newvals;		/* new value(s) for field(s) */
	/* integer list of field attnums */
	List	   *fieldnums pg_node_attr(query_jumble_ignore);
	/* type of result (same as type of arg) */
	Oid			resulttype pg_node_attr(query_jumble_ignore);
	/* Like RowExpr, we deliberately omit a typmod and collation here */
} FieldStore;

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
	/* output typmod (usually -1) */
	int32		resulttypmod pg_node_attr(query_jumble_ignore);
	/* OID of collation, or InvalidOid if none */
	Oid			resultcollid pg_node_attr(query_jumble_ignore);
	/* how to display this node */
	CoercionForm relabelformat pg_node_attr(query_jumble_ignore);
	ParseLoc	location;		/* token location, or -1 if unknown */
} RelabelType;

/* ----------------
 * CoerceViaIO
 *
 * CoerceViaIO represents a type coercion between two types whose textual
 * representations are compatible, implemented by invoking the source type's
 * typoutput function then the destination type's typinput function.
 * ----------------
 */

typedef struct CoerceViaIO
{
	Expr		xpr;
	Expr	   *arg;			/* input expression */
	Oid			resulttype;		/* output type of coercion */
	/* output typmod is not stored, but is presumed -1 */
	/* OID of collation, or InvalidOid if none */
	Oid			resultcollid pg_node_attr(query_jumble_ignore);
	/* how to display this node */
	CoercionForm coerceformat pg_node_attr(query_jumble_ignore);
	ParseLoc	location;		/* token location, or -1 if unknown */
} CoerceViaIO;

/* ----------------
 * ArrayCoerceExpr
 *
 * ArrayCoerceExpr represents a type coercion from one array type to another,
 * which is implemented by applying the per-element coercion expression
 * "elemexpr" to each element of the source array.  Within elemexpr, the
 * source element is represented by a CaseTestExpr node.  Note that even if
 * elemexpr is a no-op (that is, just CaseTestExpr + RelabelType), the
 * coercion still requires some effort: we have to fix the element type OID
 * stored in the array header.
 * ----------------
 */

typedef struct ArrayCoerceExpr
{
	Expr		xpr;
	Expr	   *arg;			/* input expression (yields an array) */
	Expr	   *elemexpr;		/* expression representing per-element work */
	Oid			resulttype;		/* output type of coercion (an array type) */
	/* output typmod (also element typmod) */
	int32		resulttypmod pg_node_attr(query_jumble_ignore);
	/* OID of collation, or InvalidOid if none */
	Oid			resultcollid pg_node_attr(query_jumble_ignore);
	/* how to display this node */
	CoercionForm coerceformat pg_node_attr(query_jumble_ignore);
	ParseLoc	location;		/* token location, or -1 if unknown */
} ArrayCoerceExpr;

/* ----------------
 * ConvertRowtypeExpr
 *
 * ConvertRowtypeExpr represents a type coercion from one composite type
 * to another, where the source type is guaranteed to contain all the columns
 * needed for the destination type plus possibly others; the columns need not
 * be in the same positions, but are matched up by name.  This is primarily
 * used to convert a whole-row value of an inheritance child table into a
 * valid whole-row value of its parent table's rowtype.  Both resulttype
 * and the exposed type of "arg" must be named composite types (not domains).
 * ----------------
 */

typedef struct ConvertRowtypeExpr
{
	Expr		xpr;
	Expr	   *arg;			/* input expression */
	Oid			resulttype;		/* output type (always a composite type) */
	/* Like RowExpr, we deliberately omit a typmod and collation here */
	/* how to display this node */
	CoercionForm convertformat pg_node_attr(query_jumble_ignore);
	ParseLoc	location;		/* token location, or -1 if unknown */
} ConvertRowtypeExpr;

/*----------
 * CollateExpr - COLLATE
 *
 * The planner replaces CollateExpr with RelabelType during expression
 * preprocessing, so execution never sees a CollateExpr.
 *----------
 */
typedef struct CollateExpr
{
	Expr		xpr;
	Expr	   *arg;			/* input expression */
	Oid			collOid;		/* collation's OID */
	ParseLoc	location;		/* token location, or -1 if unknown */
} CollateExpr;

/*----------
 * CaseExpr - a CASE expression
 *
 * We support two distinct forms of CASE expression:
 *		CASE WHEN boolexpr THEN expr [ WHEN boolexpr THEN expr ... ]
 *		CASE testexpr WHEN compexpr THEN expr [ WHEN compexpr THEN expr ... ]
 * These are distinguishable by the "arg" field being NULL in the first case
 * and the testexpr in the second case.
 *
 * In the raw grammar output for the second form, the condition expressions
 * of the WHEN clauses are just the comparison values.  Parse analysis
 * converts these to valid boolean expressions of the form
 *		CaseTestExpr '=' compexpr
 * where the CaseTestExpr node is a placeholder that emits the correct
 * value at runtime.  This structure is used so that the testexpr need be
 * evaluated only once.  Note that after parse analysis, the condition
 * expressions always yield boolean.
 *
 * Note: we can test whether a CaseExpr has been through parse analysis
 * yet by checking whether casetype is InvalidOid or not.
 *----------
 */
typedef struct CaseExpr
{
	Expr		xpr;
	/* type of expression result */
	Oid			casetype pg_node_attr(query_jumble_ignore);
	/* OID of collation, or InvalidOid if none */
	Oid			casecollid pg_node_attr(query_jumble_ignore);
	Expr	   *arg;			/* implicit equality comparison argument */
	List	   *args;			/* the arguments (list of WHEN clauses) */
	Expr	   *defresult;		/* the default result (ELSE clause) */
	ParseLoc	location;		/* token location, or -1 if unknown */
} CaseExpr;

/*
 * CaseWhen - one arm of a CASE expression
 */
typedef struct CaseWhen
{
	Expr		xpr;
	Expr	   *expr;			/* condition expression */
	Expr	   *result;			/* substitution result */
	ParseLoc	location;		/* token location, or -1 if unknown */
} CaseWhen;

/*
 * Placeholder node for the test value to be processed by a CASE expression.
 * This is effectively like a Param, but can be implemented more simply
 * since we need only one replacement value at a time.
 *
 * We also abuse this node type for some other purposes, including:
 *	* Placeholder for the current array element value in ArrayCoerceExpr;
 *	  see build_coercion_expression().
 *	* Nested FieldStore/SubscriptingRef assignment expressions in INSERT/UPDATE;
 *	  see transformAssignmentIndirection().
 *	* Placeholder for intermediate results in some SQL/JSON expression nodes,
 *	  such as JsonConstructorExpr.
 *
 * The uses in CaseExpr and ArrayCoerceExpr are safe only to the extent that
 * there is not any other CaseExpr or ArrayCoerceExpr between the value source
 * node and its child CaseTestExpr(s).  This is true in the parse analysis
 * output, but the planner's function-inlining logic has to be careful not to
 * break it.
 *
 * The nested-assignment-expression case is safe because the only node types
 * that can be above such CaseTestExprs are FieldStore and SubscriptingRef.
 */
typedef struct CaseTestExpr
{
	Expr		xpr;
	Oid			typeId;			/* type for substituted value */
	/* typemod for substituted value */
	int32		typeMod pg_node_attr(query_jumble_ignore);
	/* collation for the substituted value */
	Oid			collation pg_node_attr(query_jumble_ignore);
} CaseTestExpr;

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
	/* type of expression result */
	Oid			array_typeid pg_node_attr(query_jumble_ignore);
	/* OID of collation, or InvalidOid if none */
	Oid			array_collid pg_node_attr(query_jumble_ignore);
	/* common type of array elements */
	Oid			element_typeid pg_node_attr(query_jumble_ignore);
	/* the array elements or sub-arrays */
	List	   *elements;
	/* true if elements are sub-arrays */
	bool		multidims pg_node_attr(query_jumble_ignore);
	/* token location, or -1 if unknown */
	ParseLoc	location;
} ArrayExpr;

/*
 * RowExpr - a ROW() expression
 *
 * Note: the list of fields must have a one-for-one correspondence with
 * physical fields of the associated rowtype, although it is okay for it
 * to be shorter than the rowtype.  That is, the N'th list element must
 * match up with the N'th physical field.  When the N'th physical field
 * is a dropped column (attisdropped) then the N'th list element can just
 * be a NULL constant.  (This case can only occur for named composite types,
 * not RECORD types, since those are built from the RowExpr itself rather
 * than vice versa.)  It is important not to assume that length(args) is
 * the same as the number of columns logically present in the rowtype.
 *
 * colnames provides field names if the ROW() result is of type RECORD.
 * Names *must* be provided if row_typeid is RECORDOID; but if it is a
 * named composite type, colnames will be ignored in favor of using the
 * type's cataloged field names, so colnames should be NIL.  Like the
 * args list, colnames is defined to be one-for-one with physical fields
 * of the rowtype (although dropped columns shouldn't appear in the
 * RECORD case, so this fine point is currently moot).
 */
typedef struct RowExpr
{
	Expr		xpr;
	List	   *args;			/* the fields */

	/* RECORDOID or a composite type's ID */
	Oid			row_typeid pg_node_attr(query_jumble_ignore);

	/*
	 * row_typeid cannot be a domain over composite, only plain composite.  To
	 * create a composite domain value, apply CoerceToDomain to the RowExpr.
	 *
	 * Note: we deliberately do NOT store a typmod.  Although a typmod will be
	 * associated with specific RECORD types at runtime, it will differ for
	 * different backends, and so cannot safely be stored in stored
	 * parsetrees.  We must assume typmod -1 for a RowExpr node.
	 *
	 * We don't need to store a collation either.  The result type is
	 * necessarily composite, and composite types never have a collation.
	 */

	/* how to display this node */
	CoercionForm row_format pg_node_attr(query_jumble_ignore);

	/* list of String, or NIL */
	List	   *colnames pg_node_attr(query_jumble_ignore);

	ParseLoc	location;		/* token location, or -1 if unknown */
} RowExpr;

/*
 * RowCompareExpr - row-wise comparison, such as (a, b) <= (1, 2)
 *
 * We support row comparison for any operator that can be determined to
 * act like =, <>, <, <=, >, or >= (we determine this by looking for the
 * operator in btree opfamilies).  Note that the same operator name might
 * map to a different operator for each pair of row elements, since the
 * element datatypes can vary.
 *
 * A RowCompareExpr node is only generated for the < <= > >= cases;
 * the = and <> cases are translated to simple AND or OR combinations
 * of the pairwise comparisons.  However, we include = and <> in the
 * RowCompareType enum for the convenience of parser logic.
 */
typedef enum RowCompareType
{
	/* Values of this enum are chosen to match btree strategy numbers */
	ROWCOMPARE_LT = 1,			/* BTLessStrategyNumber */
	ROWCOMPARE_LE = 2,			/* BTLessEqualStrategyNumber */
	ROWCOMPARE_EQ = 3,			/* BTEqualStrategyNumber */
	ROWCOMPARE_GE = 4,			/* BTGreaterEqualStrategyNumber */
	ROWCOMPARE_GT = 5,			/* BTGreaterStrategyNumber */
	ROWCOMPARE_NE = 6,			/* no such btree strategy */
} RowCompareType;

typedef struct RowCompareExpr
{
	Expr		xpr;

	/* LT LE GE or GT, never EQ or NE */
	RowCompareType rctype;
	/* OID list of pairwise comparison ops */
	List	   *opnos pg_node_attr(query_jumble_ignore);
	/* OID list of containing operator families */
	List	   *opfamilies pg_node_attr(query_jumble_ignore);
	/* OID list of collations for comparisons */
	List	   *inputcollids pg_node_attr(query_jumble_ignore);
	/* the left-hand input arguments */
	List	   *largs;
	/* the right-hand input arguments */
	List	   *rargs;
} RowCompareExpr;

/*
 * CoalesceExpr - a COALESCE expression
 */
typedef struct CoalesceExpr
{
	Expr		xpr;
	/* type of expression result */
	Oid			coalescetype pg_node_attr(query_jumble_ignore);
	/* OID of collation, or InvalidOid if none */
	Oid			coalescecollid pg_node_attr(query_jumble_ignore);
	/* the arguments */
	List	   *args;
	/* token location, or -1 if unknown */
	ParseLoc	location;
} CoalesceExpr;

/*
 * MinMaxExpr - a GREATEST or LEAST function
 */
typedef enum MinMaxOp
{
	IS_GREATEST,
	IS_LEAST
} MinMaxOp;

typedef struct MinMaxExpr
{
	Expr		xpr;
	/* common type of arguments and result */
	Oid			minmaxtype pg_node_attr(query_jumble_ignore);
	/* OID of collation of result */
	Oid			minmaxcollid pg_node_attr(query_jumble_ignore);
	/* OID of collation that function should use */
	Oid			inputcollid pg_node_attr(query_jumble_ignore);
	/* function to execute */
	MinMaxOp	op;
	/* the arguments */
	List	   *args;
	/* token location, or -1 if unknown */
	ParseLoc	location;
} MinMaxExpr;

/*
 * SQLValueFunction - parameterless functions with special grammar productions
 *
 * The SQL standard categorizes some of these as <datetime value function>
 * and others as <general value specification>.  We call 'em SQLValueFunctions
 * for lack of a better term.  We store type and typmod of the result so that
 * some code doesn't need to know each function individually, and because
 * we would need to store typmod anyway for some of the datetime functions.
 * Note that currently, all variants return non-collating datatypes, so we do
 * not need a collation field; also, all these functions are stable.
 */
typedef enum SQLValueFunctionOp
{
	SVFOP_CURRENT_DATE,
	SVFOP_CURRENT_TIME,
	SVFOP_CURRENT_TIME_N,
	SVFOP_CURRENT_TIMESTAMP,
	SVFOP_CURRENT_TIMESTAMP_N,
	SVFOP_LOCALTIME,
	SVFOP_LOCALTIME_N,
	SVFOP_LOCALTIMESTAMP,
	SVFOP_LOCALTIMESTAMP_N,
	SVFOP_CURRENT_ROLE,
	SVFOP_CURRENT_USER,
	SVFOP_USER,
	SVFOP_SESSION_USER,
	SVFOP_CURRENT_CATALOG,
	SVFOP_CURRENT_SCHEMA,
} SQLValueFunctionOp;

typedef struct SQLValueFunction
{
	Expr		xpr;
	SQLValueFunctionOp op;		/* which function this is */

	/*
	 * Result type/typmod.  Type is fully determined by "op", so no need to
	 * include this Oid in the query jumbling.
	 */
	Oid			type pg_node_attr(query_jumble_ignore);
	int32		typmod;
	ParseLoc	location;		/* token location, or -1 if unknown */
} SQLValueFunction;

/*
 * XmlExpr - various SQL/XML functions requiring special grammar productions
 *
 * 'name' carries the "NAME foo" argument (already XML-escaped).
 * 'named_args' and 'arg_names' represent an xml_attribute list.
 * 'args' carries all other arguments.
 *
 * Note: result type/typmod/collation are not stored, but can be deduced
 * from the XmlExprOp.  The type/typmod fields are just used for display
 * purposes, and are NOT necessarily the true result type of the node.
 */
typedef enum XmlExprOp
{
	IS_XMLCONCAT,				/* XMLCONCAT(args) */
	IS_XMLELEMENT,				/* XMLELEMENT(name, xml_attributes, args) */
	IS_XMLFOREST,				/* XMLFOREST(xml_attributes) */
	IS_XMLPARSE,				/* XMLPARSE(text, is_doc, preserve_ws) */
	IS_XMLPI,					/* XMLPI(name [, args]) */
	IS_XMLROOT,					/* XMLROOT(xml, version, standalone) */
	IS_XMLSERIALIZE,			/* XMLSERIALIZE(is_document, xmlval, indent) */
	IS_DOCUMENT,				/* xmlval IS DOCUMENT */
} XmlExprOp;

typedef enum XmlOptionType
{
	XMLOPTION_DOCUMENT,
	XMLOPTION_CONTENT,
} XmlOptionType;

typedef struct XmlExpr
{
	Expr		xpr;
	/* xml function ID */
	XmlExprOp	op;
	/* name in xml(NAME foo ...) syntaxes */
	char	   *name pg_node_attr(query_jumble_ignore);
	/* non-XML expressions for xml_attributes */
	List	   *named_args;
	/* parallel list of String values */
	List	   *arg_names pg_node_attr(query_jumble_ignore);
	/* list of expressions */
	List	   *args;
	/* DOCUMENT or CONTENT */
	XmlOptionType xmloption pg_node_attr(query_jumble_ignore);
	/* INDENT option for XMLSERIALIZE */
	bool		indent;
	/* target type/typmod for XMLSERIALIZE */
	Oid			type pg_node_attr(query_jumble_ignore);
	int32		typmod pg_node_attr(query_jumble_ignore);
	/* token location, or -1 if unknown */
	ParseLoc	location;
} XmlExpr;

/*
 * JsonEncoding -
 *		representation of JSON ENCODING clause
 */
typedef enum JsonEncoding
{
	JS_ENC_DEFAULT,				/* unspecified */
	JS_ENC_UTF8,
	JS_ENC_UTF16,
	JS_ENC_UTF32,
} JsonEncoding;

/*
 * JsonFormatType -
 *		enumeration of JSON formats used in JSON FORMAT clause
 */
typedef enum JsonFormatType
{
	JS_FORMAT_DEFAULT,			/* unspecified */
	JS_FORMAT_JSON,				/* FORMAT JSON [ENCODING ...] */
	JS_FORMAT_JSONB,			/* implicit internal format for RETURNING
								 * jsonb */
} JsonFormatType;

/*
 * JsonFormat -
 *		representation of JSON FORMAT clause
 */
typedef struct JsonFormat
{
	NodeTag		type;
	JsonFormatType format_type; /* format type */
	JsonEncoding encoding;		/* JSON encoding */
	ParseLoc	location;		/* token location, or -1 if unknown */
} JsonFormat;

/*
 * JsonReturning -
 *		transformed representation of JSON RETURNING clause
 */
typedef struct JsonReturning
{
	NodeTag		type;
	JsonFormat *format;			/* output JSON format */
	Oid			typid;			/* target type Oid */
	int32		typmod;			/* target type modifier */
} JsonReturning;

/*
 * JsonValueExpr -
 *		representation of JSON value expression (expr [FORMAT JsonFormat])
 *
 * raw_expr is the user-specified value, while formatted_expr is the value
 * obtained by coercing raw_expr to the type required by either the FORMAT
 * clause or an enclosing node's RETURNING clause.
 *
 * When deparsing a JsonValueExpr, get_rule_expr() prints raw_expr. However,
 * during the evaluation of a JsonValueExpr, the value of formatted_expr
 * takes precedence over that of raw_expr.
 */
typedef struct JsonValueExpr
{
	NodeTag		type;
	Expr	   *raw_expr;		/* user-specified expression */
	Expr	   *formatted_expr; /* coerced formatted expression */
	JsonFormat *format;			/* FORMAT clause, if specified */
} JsonValueExpr;

typedef enum JsonConstructorType
{
	JSCTOR_JSON_OBJECT = 1,
	JSCTOR_JSON_ARRAY = 2,
	JSCTOR_JSON_OBJECTAGG = 3,
	JSCTOR_JSON_ARRAYAGG = 4,
	JSCTOR_JSON_PARSE = 5,
	JSCTOR_JSON_SCALAR = 6,
	JSCTOR_JSON_SERIALIZE = 7,
} JsonConstructorType;

/*
 * JsonConstructorExpr -
 *		wrapper over FuncExpr/Aggref/WindowFunc for SQL/JSON constructors
 */
typedef struct JsonConstructorExpr
{
	Expr		xpr;
	JsonConstructorType type;	/* constructor type */
	List	   *args;
	Expr	   *func;			/* underlying json[b]_xxx() function call */
	Expr	   *coercion;		/* coercion to RETURNING type */
	JsonReturning *returning;	/* RETURNING clause */
	bool		absent_on_null; /* ABSENT ON NULL? */
	bool		unique;			/* WITH UNIQUE KEYS? (JSON_OBJECT[AGG] only) */
	ParseLoc	location;
} JsonConstructorExpr;

/*
 * JsonValueType -
 *		representation of JSON item type in IS JSON predicate
 */
typedef enum JsonValueType
{
	JS_TYPE_ANY,				/* IS JSON [VALUE] */
	JS_TYPE_OBJECT,				/* IS JSON OBJECT */
	JS_TYPE_ARRAY,				/* IS JSON ARRAY */
	JS_TYPE_SCALAR,				/* IS JSON SCALAR */
} JsonValueType;

/*
 * JsonIsPredicate -
 *		representation of IS JSON predicate
 */
typedef struct JsonIsPredicate
{
	NodeTag		type;
	Node	   *expr;			/* subject expression */
	JsonFormat *format;			/* FORMAT clause, if specified */
	JsonValueType item_type;	/* JSON item type */
	bool		unique_keys;	/* check key uniqueness? */
	ParseLoc	location;		/* token location, or -1 if unknown */
} JsonIsPredicate;

/* Nodes used in SQL/JSON query functions */

/*
 * JsonWrapper -
 *		representation of WRAPPER clause for JSON_QUERY()
 */
typedef enum JsonWrapper
{
	JSW_UNSPEC,
	JSW_NONE,
	JSW_CONDITIONAL,
	JSW_UNCONDITIONAL,
} JsonWrapper;

/*
 * JsonBehaviorType -
 *		enumeration of behavior types used in SQL/JSON ON ERROR/EMPTY clauses
 *
 * 		If enum members are reordered, get_json_behavior() from ruleutils.c
 * 		must be updated accordingly.
 */
typedef enum JsonBehaviorType
{
	JSON_BEHAVIOR_NULL = 0,
	JSON_BEHAVIOR_ERROR,
	JSON_BEHAVIOR_EMPTY,
	JSON_BEHAVIOR_TRUE,
	JSON_BEHAVIOR_FALSE,
	JSON_BEHAVIOR_UNKNOWN,
	JSON_BEHAVIOR_EMPTY_ARRAY,
	JSON_BEHAVIOR_EMPTY_OBJECT,
	JSON_BEHAVIOR_DEFAULT,
} JsonBehaviorType;

/*
 * JsonBehavior
 *		Specifications for ON ERROR / ON EMPTY behaviors of SQL/JSON
 *		query functions specified by a JsonExpr
 *
 * 'expr' is the expression to emit when a given behavior (EMPTY or ERROR)
 * occurs on evaluating the SQL/JSON query function.  'coerce' is set to true
 * if 'expr' isn't already of the expected target type given by
 * JsonExpr.returning.
 */
typedef struct JsonBehavior
{
	NodeTag		type;

	JsonBehaviorType btype;
	Node	   *expr;
	bool		coerce;
	ParseLoc	location;		/* token location, or -1 if unknown */
} JsonBehavior;

/*
 * JsonExprOp -
 *		enumeration of SQL/JSON query function types
 */
typedef enum JsonExprOp
{
	JSON_EXISTS_OP,				/* JSON_EXISTS() */
	JSON_QUERY_OP,				/* JSON_QUERY() */
	JSON_VALUE_OP,				/* JSON_VALUE() */
	JSON_TABLE_OP,				/* JSON_TABLE() */
} JsonExprOp;

/*
 * JsonExpr -
 *		Transformed representation of JSON_VALUE(), JSON_QUERY(), and
 *		JSON_EXISTS()
 */
typedef struct JsonExpr
{
	Expr		xpr;

	JsonExprOp	op;

	char	   *column_name;	/* JSON_TABLE() column name or NULL if this is
								 * not for a JSON_TABLE() */

	/* jsonb-valued expression to query */
	Node	   *formatted_expr;

	/* Format of the above expression needed by ruleutils.c */
	JsonFormat *format;

	/* jsonpath-valued expression containing the query pattern */
	Node	   *path_spec;

	/* Expected type/format of the output. */
	JsonReturning *returning;

	/* Information about the PASSING argument expressions */
	List	   *passing_names;
	List	   *passing_values;

	/* User-specified or default ON EMPTY and ON ERROR behaviors */
	JsonBehavior *on_empty;
	JsonBehavior *on_error;

	/*
	 * Information about converting the result of jsonpath functions
	 * JsonPathQuery() and JsonPathValue() to the RETURNING type.
	 */
	bool		use_io_coercion;
	bool		use_json_coercion;

	/* WRAPPER specification for JSON_QUERY */
	JsonWrapper wrapper;

	/* KEEP or OMIT QUOTES for singleton scalars returned by JSON_QUERY() */
	bool		omit_quotes;

	/* JsonExpr's collation. */
	Oid			collation;

	/* Original JsonFuncExpr's location */
	ParseLoc	location;
} JsonExpr;

/*
 * JsonTablePath
 *		A JSON path expression to be computed as part of evaluating
 *		a JSON_TABLE plan node
 */
typedef struct JsonTablePath
{
	NodeTag		type;

	Const	   *value;
	char	   *name;
} JsonTablePath;

/*
 * JsonTablePlan -
 *		Abstract class to represent different types of JSON_TABLE "plans".
 *		A plan is used to generate a "row pattern" value by evaluating a JSON
 *		path expression against an input JSON document, which is then used for
 *		populating JSON_TABLE() columns
 */
typedef struct JsonTablePlan
{
	pg_node_attr(abstract)

	NodeTag		type;
} JsonTablePlan;

/*
 * JSON_TABLE plan to evaluate a JSON path expression and NESTED paths, if
 * any.
 */
typedef struct JsonTablePathScan
{
	JsonTablePlan plan;

	/* JSON path to evaluate */
	JsonTablePath *path;

	/*
	 * ERROR/EMPTY ON ERROR behavior; only significant in the plan for the
	 * top-level path.
	 */
	bool		errorOnError;

	/* Plan(s) for nested columns, if any. */
	JsonTablePlan *child;

	/*
	 * 0-based index in TableFunc.colvalexprs of the 1st and the last column
	 * covered by this plan.  Both are -1 if all columns are nested and thus
	 * computed by the child plan(s).
	 */
	int			colMin;
	int			colMax;
} JsonTablePathScan;

/*
 * JsonTableSiblingJoin -
 *		Plan to join rows of sibling NESTED COLUMNS clauses in the same parent
 *		COLUMNS clause
 */
typedef struct JsonTableSiblingJoin
{
	JsonTablePlan plan;

	JsonTablePlan *lplan;
	JsonTablePlan *rplan;
} JsonTableSiblingJoin;

/* ----------------
 * NullTest
 *
 * NullTest represents the operation of testing a value for NULLness.
 * The appropriate test is performed and returned as a boolean Datum.
 *
 * When argisrow is false, this simply represents a test for the null value.
 *
 * When argisrow is true, the input expression must yield a rowtype, and
 * the node implements "row IS [NOT] NULL" per the SQL standard.  This
 * includes checking individual fields for NULLness when the row datum
 * itself isn't NULL.
 *
 * NOTE: the combination of a rowtype input and argisrow==false does NOT
 * correspond to the SQL notation "row IS [NOT] NULL"; instead, this case
 * represents the SQL notation "row IS [NOT] DISTINCT FROM NULL".
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
	/* T to perform field-by-field null checks */
	bool		argisrow pg_node_attr(query_jumble_ignore);
	ParseLoc	location;		/* token location, or -1 if unknown */
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
	ParseLoc	location;		/* token location, or -1 if unknown */
} BooleanTest;


/*
 * MergeAction
 *
 * Transformed representation of a WHEN clause in a MERGE statement
 */

typedef enum MergeMatchKind
{
	MERGE_WHEN_MATCHED,
	MERGE_WHEN_NOT_MATCHED_BY_SOURCE,
	MERGE_WHEN_NOT_MATCHED_BY_TARGET
} MergeMatchKind;

#define NUM_MERGE_MATCH_KINDS (MERGE_WHEN_NOT_MATCHED_BY_TARGET + 1)

typedef struct MergeAction
{
	NodeTag		type;
	MergeMatchKind matchKind;	/* MATCHED/NOT MATCHED BY SOURCE/TARGET */
	CmdType		commandType;	/* INSERT/UPDATE/DELETE/DO NOTHING */
	/* OVERRIDING clause */
	OverridingKind override pg_node_attr(query_jumble_ignore);
	Node	   *qual;			/* transformed WHEN conditions */
	List	   *targetList;		/* the target list (of TargetEntry) */
	/* target attribute numbers of an UPDATE */
	List	   *updateColnos pg_node_attr(query_jumble_ignore);
} MergeAction;

/*
 * CoerceToDomain
 *
 * CoerceToDomain represents the operation of coercing a value to a domain
 * type.  At runtime (and not before) the precise set of constraints to be
 * checked will be determined.  If the value passes, it is returned as the
 * result; if not, an error is raised.  Note that this is equivalent to
 * RelabelType in the scenario where no constraints are applied.
 */
typedef struct CoerceToDomain
{
	Expr		xpr;
	Expr	   *arg;			/* input expression */
	Oid			resulttype;		/* domain type ID (result type) */
	/* output typmod (currently always -1) */
	int32		resulttypmod pg_node_attr(query_jumble_ignore);
	/* OID of collation, or InvalidOid if none */
	Oid			resultcollid pg_node_attr(query_jumble_ignore);
	/* how to display this node */
	CoercionForm coercionformat pg_node_attr(query_jumble_ignore);
	ParseLoc	location;		/* token location, or -1 if unknown */
} CoerceToDomain;

/*
 * Placeholder node for the value to be processed by a domain's check
 * constraint.  This is effectively like a Param, but can be implemented more
 * simply since we need only one replacement value at a time.
 *
 * Note: the typeId/typeMod/collation will be set from the domain's base type,
 * not the domain itself.  This is because we shouldn't consider the value
 * to be a member of the domain if we haven't yet checked its constraints.
 */
typedef struct CoerceToDomainValue
{
	Expr		xpr;
	/* type for substituted value */
	Oid			typeId;
	/* typemod for substituted value */
	int32		typeMod pg_node_attr(query_jumble_ignore);
	/* collation for the substituted value */
	Oid			collation pg_node_attr(query_jumble_ignore);
	/* token location, or -1 if unknown */
	ParseLoc	location;
} CoerceToDomainValue;

/*
 * Placeholder node for a DEFAULT marker in an INSERT or UPDATE command.
 *
 * This is not an executable expression: it must be replaced by the actual
 * column default expression during rewriting.  But it is convenient to
 * treat it as an expression node during parsing and rewriting.
 */
typedef struct SetToDefault
{
	Expr		xpr;
	/* type for substituted value */
	Oid			typeId;
	/* typemod for substituted value */
	int32		typeMod pg_node_attr(query_jumble_ignore);
	/* collation for the substituted value */
	Oid			collation pg_node_attr(query_jumble_ignore);
	/* token location, or -1 if unknown */
	ParseLoc	location;
} SetToDefault;

/*
 * Node representing [WHERE] CURRENT OF cursor_name
 *
 * CURRENT OF is a bit like a Var, in that it carries the rangetable index
 * of the target relation being constrained; this aids placing the expression
 * correctly during planning.  We can assume however that its "levelsup" is
 * always zero, due to the syntactic constraints on where it can appear.
 * Also, cvarno will always be a true RT index, never INNER_VAR etc.
 *
 * The referenced cursor can be represented either as a hardwired string
 * or as a reference to a run-time parameter of type REFCURSOR.  The latter
 * case is for the convenience of plpgsql.
 */
typedef struct CurrentOfExpr
{
	Expr		xpr;
	Index		cvarno;			/* RT index of target relation */
	char	   *cursor_name;	/* name of referenced cursor, or NULL */
	int			cursor_param;	/* refcursor parameter number, or 0 */
} CurrentOfExpr;

/*
 * NextValueExpr - get next value from sequence
 *
 * This has the same effect as calling the nextval() function, but it does not
 * check permissions on the sequence.  This is used for identity columns,
 * where the sequence is an implicit dependency without its own permissions.
 */
typedef struct NextValueExpr
{
	Expr		xpr;
	Oid			seqid;
	Oid			typeId;
} NextValueExpr;

/*
 * InferenceElem - an element of a unique index inference specification
 *
 * This mostly matches the structure of IndexElems, but having a dedicated
 * primnode allows for a clean separation between the use of index parameters
 * by utility commands, and this node.
 */
typedef struct InferenceElem
{
	Expr		xpr;
	Node	   *expr;			/* expression to infer from, or NULL */
	Oid			infercollid;	/* OID of collation, or InvalidOid */
	Oid			inferopclass;	/* OID of att opclass, or InvalidOid */
} InferenceElem;

/*--------------------
 * TargetEntry -
 *	   a target entry (used in query target lists)
 *
 * Strictly speaking, a TargetEntry isn't an expression node (since it can't
 * be evaluated by ExecEvalExpr).  But we treat it as one anyway, since in
 * very many places it's convenient to process a whole query targetlist as a
 * single expression tree.
 *
 * In a SELECT's targetlist, resno should always be equal to the item's
 * ordinal position (counting from 1).  However, in an INSERT or UPDATE
 * targetlist, resno represents the attribute number of the destination
 * column for the item; so there may be missing or out-of-order resnos.
 * It is even legal to have duplicated resnos; consider
 *		UPDATE table SET arraycol[1] = ..., arraycol[2] = ..., ...
 * In an INSERT, the rewriter and planner will normalize the tlist by
 * reordering it into physical column order and filling in default values
 * for any columns not assigned values by the original query.  In an UPDATE,
 * after the rewriter merges multiple assignments for the same column, the
 * planner extracts the target-column numbers into a separate "update_colnos"
 * list, and then renumbers the tlist elements serially.  Thus, tlist resnos
 * match ordinal position in all tlists seen by the executor; but it is wrong
 * to assume that before planning has happened.
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
 * ressortgroupref is used in the representation of ORDER BY, GROUP BY, and
 * DISTINCT items.  Targetlist entries with ressortgroupref=0 are not
 * sort/group items.  If ressortgroupref>0, then this item is an ORDER BY,
 * GROUP BY, and/or DISTINCT target value.  No two entries in a targetlist
 * may have the same nonzero ressortgroupref --- but there is no particular
 * meaning to the nonzero values, except as tags.  (For example, one must
 * not assume that lower ressortgroupref means a more significant sort key.)
 * The order of the associated SortGroupClause lists determine the semantics.
 *
 * resorigtbl/resorigcol identify the source of the column, if it is a
 * simple reference to a column of a base table (or view).  If it is not
 * a simple reference, these fields are zeroes.
 *
 * If resjunk is true then the column is a working column (such as a sort key)
 * that should be removed from the final output of the query.  Resjunk columns
 * must have resnos that cannot duplicate any regular column's resno.  Also
 * note that there are places that assume resjunk columns come after non-junk
 * columns.
 *--------------------
 */
typedef struct TargetEntry
{
	Expr		xpr;
	/* expression to evaluate */
	Expr	   *expr;
	/* attribute number (see notes above) */
	AttrNumber	resno;
	/* name of the column (could be NULL) */
	char	   *resname pg_node_attr(query_jumble_ignore);
	/* nonzero if referenced by a sort/group clause */
	Index		ressortgroupref;
	/* OID of column's source table */
	Oid			resorigtbl pg_node_attr(query_jumble_ignore);
	/* column's number in source table */
	AttrNumber	resorigcol pg_node_attr(query_jumble_ignore);
	/* set to true to eliminate the attribute from final target list */
	bool		resjunk pg_node_attr(query_jumble_ignore);
} TargetEntry;


/* ----------------------------------------------------------------
 *					node types for join trees
 *
 * The leaves of a join tree structure are RangeTblRef nodes.  Above
 * these, JoinExpr nodes can appear to denote a specific kind of join
 * or qualified join.  Also, FromExpr nodes can appear to denote an
 * ordinary cross-product join ("FROM foo, bar, baz WHERE ...").
 * FromExpr is like a JoinExpr of jointype JOIN_INNER, except that it
 * may have any number of child nodes, not just two.
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
 * isNatural, usingClause, and quals are interdependent.  The user can write
 * only one of NATURAL, USING(), or ON() (this is enforced by the grammar).
 * If he writes NATURAL then parse analysis generates the equivalent USING()
 * list, and from that fills in "quals" with the right equality comparisons.
 * If he writes USING() then "quals" is filled with equality comparisons.
 * If he writes ON() then only "quals" is set.  Note that NATURAL/USING
 * are not equivalent to ON() since they also affect the output column list.
 *
 * alias is an Alias node representing the AS alias-clause attached to the
 * join expression, or NULL if no clause.  NB: presence or absence of the
 * alias has a critical impact on semantics, because a join with an alias
 * restricts visibility of the tables/columns inside it.
 *
 * join_using_alias is an Alias node representing the join correlation
 * name that SQL:2016 and later allow to be attached to JOIN/USING.
 * Its column alias list includes only the common column names from USING,
 * and it does not restrict visibility of the join's input tables.
 *
 * During parse analysis, an RTE is created for the Join, and its index
 * is filled into rtindex.  This RTE is present mainly so that Vars can
 * be created that refer to the outputs of the join.  The planner sometimes
 * generates JoinExprs internally; these can have rtindex = 0 if there are
 * no join alias variables referencing such joins.
 *----------
 */
typedef struct JoinExpr
{
	NodeTag		type;
	JoinType	jointype;		/* type of join */
	bool		isNatural;		/* Natural join? Will need to shape table */
	Node	   *larg;			/* left subtree */
	Node	   *rarg;			/* right subtree */
	/* USING clause, if any (list of String) */
	List	   *usingClause pg_node_attr(query_jumble_ignore);
	/* alias attached to USING clause, if any */
	Alias	   *join_using_alias pg_node_attr(query_jumble_ignore);
	/* qualifiers on join, if any */
	Node	   *quals;
	/* user-written alias clause, if any */
	Alias	   *alias pg_node_attr(query_jumble_ignore);
	/* RT index assigned for join, or 0 */
	int			rtindex;
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

/*----------
 * OnConflictExpr - represents an ON CONFLICT DO ... expression
 *
 * The optimizer requires a list of inference elements, and optionally a WHERE
 * clause to infer a unique index.  The unique index (or, occasionally,
 * indexes) inferred are used to arbitrate whether or not the alternative ON
 * CONFLICT path is taken.
 *----------
 */
typedef struct OnConflictExpr
{
	NodeTag		type;
	OnConflictAction action;	/* DO NOTHING or UPDATE? */

	/* Arbiter */
	List	   *arbiterElems;	/* unique index arbiter list (of
								 * InferenceElem's) */
	Node	   *arbiterWhere;	/* unique index arbiter WHERE clause */
	Oid			constraint;		/* pg_constraint OID for arbiter */

	/* ON CONFLICT UPDATE */
	List	   *onConflictSet;	/* List of ON CONFLICT SET TargetEntrys */
	Node	   *onConflictWhere;	/* qualifiers to restrict UPDATE to */
	int			exclRelIndex;	/* RT index of 'excluded' relation */
	List	   *exclRelTlist;	/* tlist of the EXCLUDED pseudo relation */
} OnConflictExpr;

#endif							/* PRIMNODES_H */
