/*-------------------------------------------------------------------------
 *
 * parsenodes.h
 *	  definitions for parse tree nodes
 *
 * Many of the node types used in parsetrees include a "location" field.
 * This is a byte (not character) offset in the original source text, to be
 * used for positioning an error cursor when there is an error related to
 * the node.  Access to the original source text is needed to make use of
 * the location.  At the topmost (statement) level, we also provide a
 * statement length, likewise measured in bytes, for convenience in
 * identifying statement boundaries in multi-statement source strings.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/parsenodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSENODES_H
#define PARSENODES_H

#include "common/relpath.h"
#include "nodes/bitmapset.h"
#include "nodes/lockoptions.h"
#include "nodes/primnodes.h"
#include "nodes/value.h"
#include "partitioning/partdefs.h"


/* Possible sources of a Query */
typedef enum QuerySource
{
	QSRC_ORIGINAL,				/* original parsetree (explicit query) */
	QSRC_PARSER,				/* added by parse analysis (now unused) */
	QSRC_INSTEAD_RULE,			/* added by unconditional INSTEAD rule */
	QSRC_QUAL_INSTEAD_RULE,		/* added by conditional INSTEAD rule */
	QSRC_NON_INSTEAD_RULE,		/* added by non-INSTEAD rule */
} QuerySource;

/* Sort ordering options for ORDER BY and CREATE INDEX */
typedef enum SortByDir
{
	SORTBY_DEFAULT,
	SORTBY_ASC,
	SORTBY_DESC,
	SORTBY_USING,				/* not allowed in CREATE INDEX ... */
} SortByDir;

typedef enum SortByNulls
{
	SORTBY_NULLS_DEFAULT,
	SORTBY_NULLS_FIRST,
	SORTBY_NULLS_LAST,
} SortByNulls;

/* Options for [ ALL | DISTINCT ] */
typedef enum SetQuantifier
{
	SET_QUANTIFIER_DEFAULT,
	SET_QUANTIFIER_ALL,
	SET_QUANTIFIER_DISTINCT,
} SetQuantifier;

/*
 * Grantable rights are encoded so that we can OR them together in a bitmask.
 * The present representation of AclItem limits us to 32 distinct rights,
 * even though AclMode is defined as uint64.  See utils/acl.h.
 *
 * Caution: changing these codes breaks stored ACLs, hence forces initdb.
 */
typedef uint64 AclMode;			/* a bitmask of privilege bits */

#define ACL_INSERT		(1<<0)	/* for relations */
#define ACL_SELECT		(1<<1)
#define ACL_UPDATE		(1<<2)
#define ACL_DELETE		(1<<3)
#define ACL_TRUNCATE	(1<<4)
#define ACL_REFERENCES	(1<<5)
#define ACL_TRIGGER		(1<<6)
#define ACL_EXECUTE		(1<<7)	/* for functions */
#define ACL_USAGE		(1<<8)	/* for various object types */
#define ACL_CREATE		(1<<9)	/* for namespaces and databases */
#define ACL_CREATE_TEMP (1<<10) /* for databases */
#define ACL_CONNECT		(1<<11) /* for databases */
#define ACL_SET			(1<<12) /* for configuration parameters */
#define ACL_ALTER_SYSTEM (1<<13)	/* for configuration parameters */
#define ACL_MAINTAIN	(1<<14) /* for relations */
#define N_ACL_RIGHTS	15		/* 1 plus the last 1<<x */
#define ACL_NO_RIGHTS	0
/* Currently, SELECT ... FOR [KEY] UPDATE/SHARE requires UPDATE privileges */
#define ACL_SELECT_FOR_UPDATE	ACL_UPDATE


/*****************************************************************************
 *	Query Tree
 *****************************************************************************/

/*
 * Query -
 *	  Parse analysis turns all statements into a Query tree
 *	  for further processing by the rewriter and planner.
 *
 *	  Utility statements (i.e. non-optimizable statements) have the
 *	  utilityStmt field set, and the rest of the Query is mostly dummy.
 *
 *	  Planning converts a Query tree into a Plan tree headed by a PlannedStmt
 *	  node --- the Query structure is not used by the executor.
 *
 *	  All the fields ignored for the query jumbling are not semantically
 *	  significant (such as alias names), as is ignored anything that can
 *	  be deduced from child nodes (else we'd just be double-hashing that
 *	  piece of information).
 */
typedef struct Query
{
	NodeTag		type;

	CmdType		commandType;	/* select|insert|update|delete|merge|utility */

	/* where did I come from? */
	QuerySource querySource pg_node_attr(query_jumble_ignore);

	/*
	 * query identifier (can be set by plugins); ignored for equal, as it
	 * might not be set; also not stored.  This is the result of the query
	 * jumble, hence ignored.
	 *
	 * We store this as a signed value as this is the form it's displayed to
	 * users in places such as EXPLAIN and pg_stat_statements.  Primarily this
	 * is done due to lack of an SQL type to represent the full range of
	 * uint64.
	 */
	int64		queryId pg_node_attr(equal_ignore, query_jumble_ignore, read_write_ignore, read_as(0));

	/* do I set the command result tag? */
	bool		canSetTag pg_node_attr(query_jumble_ignore);

	Node	   *utilityStmt;	/* non-null if commandType == CMD_UTILITY */

	/*
	 * rtable index of target relation for INSERT/UPDATE/DELETE/MERGE; 0 for
	 * SELECT.  This is ignored in the query jumble as unrelated to the
	 * compilation of the query ID.
	 */
	int			resultRelation pg_node_attr(query_jumble_ignore);

	/* has aggregates in tlist or havingQual */
	bool		hasAggs pg_node_attr(query_jumble_ignore);
	/* has window functions in tlist */
	bool		hasWindowFuncs pg_node_attr(query_jumble_ignore);
	/* has set-returning functions in tlist */
	bool		hasTargetSRFs pg_node_attr(query_jumble_ignore);
	/* has subquery SubLink */
	bool		hasSubLinks pg_node_attr(query_jumble_ignore);
	/* distinctClause is from DISTINCT ON */
	bool		hasDistinctOn pg_node_attr(query_jumble_ignore);
	/* WITH RECURSIVE was specified */
	bool		hasRecursive pg_node_attr(query_jumble_ignore);
	/* has INSERT/UPDATE/DELETE/MERGE in WITH */
	bool		hasModifyingCTE pg_node_attr(query_jumble_ignore);
	/* FOR [KEY] UPDATE/SHARE was specified */
	bool		hasForUpdate pg_node_attr(query_jumble_ignore);
	/* rewriter has applied some RLS policy */
	bool		hasRowSecurity pg_node_attr(query_jumble_ignore);
	/* parser has added an RTE_GROUP RTE */
	bool		hasGroupRTE pg_node_attr(query_jumble_ignore);
	/* is a RETURN statement */
	bool		isReturn pg_node_attr(query_jumble_ignore);

	List	   *cteList;		/* WITH list (of CommonTableExpr's) */

	List	   *rtable;			/* list of range table entries */

	/*
	 * list of RTEPermissionInfo nodes for the rtable entries having
	 * perminfoindex > 0
	 */
	List	   *rteperminfos pg_node_attr(query_jumble_ignore);
	FromExpr   *jointree;		/* table join tree (FROM and WHERE clauses);
								 * also USING clause for MERGE */

	List	   *mergeActionList;	/* list of actions for MERGE (only) */

	/*
	 * rtable index of target relation for MERGE to pull data. Initially, this
	 * is the same as resultRelation, but after query rewriting, if the target
	 * relation is a trigger-updatable view, this is the index of the expanded
	 * view subquery, whereas resultRelation is the index of the target view.
	 */
	int			mergeTargetRelation pg_node_attr(query_jumble_ignore);

	/* join condition between source and target for MERGE */
	Node	   *mergeJoinCondition;

	List	   *targetList;		/* target list (of TargetEntry) */

	/* OVERRIDING clause */
	OverridingKind override pg_node_attr(query_jumble_ignore);

	OnConflictExpr *onConflict; /* ON CONFLICT DO [NOTHING | UPDATE] */

	/*
	 * The following three fields describe the contents of the RETURNING list
	 * for INSERT/UPDATE/DELETE/MERGE. returningOldAlias and returningNewAlias
	 * are the alias names for OLD and NEW, which may be user-supplied values,
	 * the defaults "old" and "new", or NULL (if the default "old"/"new" is
	 * already in use as the alias for some other relation).
	 */
	char	   *returningOldAlias pg_node_attr(query_jumble_ignore);
	char	   *returningNewAlias pg_node_attr(query_jumble_ignore);
	List	   *returningList;	/* return-values list (of TargetEntry) */

	List	   *groupClause;	/* a list of SortGroupClause's */
	bool		groupDistinct;	/* is the group by clause distinct? */

	List	   *groupingSets;	/* a list of GroupingSet's if present */

	Node	   *havingQual;		/* qualifications applied to groups */

	List	   *windowClause;	/* a list of WindowClause's */

	List	   *distinctClause; /* a list of SortGroupClause's */

	List	   *sortClause;		/* a list of SortGroupClause's */

	Node	   *limitOffset;	/* # of result tuples to skip (int8 expr) */
	Node	   *limitCount;		/* # of result tuples to return (int8 expr) */
	LimitOption limitOption;	/* limit type */

	List	   *rowMarks;		/* a list of RowMarkClause's */

	Node	   *setOperations;	/* set-operation tree if this is top level of
								 * a UNION/INTERSECT/EXCEPT query */

	/*
	 * A list of pg_constraint OIDs that the query depends on to be
	 * semantically valid
	 */
	List	   *constraintDeps pg_node_attr(query_jumble_ignore);

	/* a list of WithCheckOption's (added during rewrite) */
	List	   *withCheckOptions pg_node_attr(query_jumble_ignore);

	/*
	 * The following two fields identify the portion of the source text string
	 * containing this query.  They are typically only populated in top-level
	 * Queries, not in sub-queries.  When not set, they might both be zero, or
	 * both be -1 meaning "unknown".
	 */
	/* start location, or -1 if unknown */
	ParseLoc	stmt_location;
	/* length in bytes; 0 means "rest of string" */
	ParseLoc	stmt_len pg_node_attr(query_jumble_ignore);
} Query;


/****************************************************************************
 *	Supporting data structures for Parse Trees
 *
 *	Most of these node types appear in raw parsetrees output by the grammar,
 *	and get transformed to something else by the analyzer.  A few of them
 *	are used as-is in transformed querytrees.
 ****************************************************************************/

/*
 * TypeName - specifies a type in definitions
 *
 * For TypeName structures generated internally, it is often easier to
 * specify the type by OID than by name.  If "names" is NIL then the
 * actual type OID is given by typeOid, otherwise typeOid is unused.
 * Similarly, if "typmods" is NIL then the actual typmod is expected to
 * be prespecified in typemod, otherwise typemod is unused.
 *
 * If pct_type is true, then names is actually a field name and we look up
 * the type of that field.  Otherwise (the normal case), names is a type
 * name possibly qualified with schema and database name.
 */
typedef struct TypeName
{
	NodeTag		type;
	List	   *names;			/* qualified name (list of String nodes) */
	Oid			typeOid;		/* type identified by OID */
	bool		setof;			/* is a set? */
	bool		pct_type;		/* %TYPE specified? */
	List	   *typmods;		/* type modifier expression(s) */
	int32		typemod;		/* prespecified type modifier */
	List	   *arrayBounds;	/* array bounds */
	ParseLoc	location;		/* token location, or -1 if unknown */
} TypeName;

/*
 * ColumnRef - specifies a reference to a column, or possibly a whole tuple
 *
 * The "fields" list must be nonempty.  It can contain String nodes
 * (representing names) and A_Star nodes (representing occurrence of a '*').
 * Currently, A_Star must appear only as the last list element --- the grammar
 * is responsible for enforcing this!
 *
 * Note: any container subscripting or selection of fields from composite columns
 * is represented by an A_Indirection node above the ColumnRef.  However,
 * for simplicity in the normal case, initial field selection from a table
 * name is represented within ColumnRef and not by adding A_Indirection.
 */
typedef struct ColumnRef
{
	NodeTag		type;
	List	   *fields;			/* field names (String nodes) or A_Star */
	ParseLoc	location;		/* token location, or -1 if unknown */
} ColumnRef;

/*
 * ParamRef - specifies a $n parameter reference
 */
typedef struct ParamRef
{
	NodeTag		type;
	int			number;			/* the number of the parameter */
	ParseLoc	location;		/* token location, or -1 if unknown */
} ParamRef;

/*
 * A_Expr - infix, prefix, and postfix expressions
 */
typedef enum A_Expr_Kind
{
	AEXPR_OP,					/* normal operator */
	AEXPR_OP_ANY,				/* scalar op ANY (array) */
	AEXPR_OP_ALL,				/* scalar op ALL (array) */
	AEXPR_DISTINCT,				/* IS DISTINCT FROM - name must be "=" */
	AEXPR_NOT_DISTINCT,			/* IS NOT DISTINCT FROM - name must be "=" */
	AEXPR_NULLIF,				/* NULLIF - name must be "=" */
	AEXPR_IN,					/* [NOT] IN - name must be "=" or "<>" */
	AEXPR_LIKE,					/* [NOT] LIKE - name must be "~~" or "!~~" */
	AEXPR_ILIKE,				/* [NOT] ILIKE - name must be "~~*" or "!~~*" */
	AEXPR_SIMILAR,				/* [NOT] SIMILAR - name must be "~" or "!~" */
	AEXPR_BETWEEN,				/* name must be "BETWEEN" */
	AEXPR_NOT_BETWEEN,			/* name must be "NOT BETWEEN" */
	AEXPR_BETWEEN_SYM,			/* name must be "BETWEEN SYMMETRIC" */
	AEXPR_NOT_BETWEEN_SYM,		/* name must be "NOT BETWEEN SYMMETRIC" */
} A_Expr_Kind;

typedef struct A_Expr
{
	pg_node_attr(custom_read_write)

	NodeTag		type;
	A_Expr_Kind kind;			/* see above */
	List	   *name;			/* possibly-qualified name of operator */
	Node	   *lexpr;			/* left argument, or NULL if none */
	Node	   *rexpr;			/* right argument, or NULL if none */

	/*
	 * If rexpr is a list of some kind, we separately track its starting and
	 * ending location; it's not the same as the starting and ending location
	 * of the token itself.
	 */
	ParseLoc	rexpr_list_start;
	ParseLoc	rexpr_list_end;
	ParseLoc	location;		/* token location, or -1 if unknown */
} A_Expr;

/*
 * A_Const - a literal constant
 *
 * Value nodes are inline for performance.  You can treat 'val' as a node,
 * as in IsA(&val, Integer).  'val' is not valid if isnull is true.
 */
union ValUnion
{
	Node		node;
	Integer		ival;
	Float		fval;
	Boolean		boolval;
	String		sval;
	BitString	bsval;
};

typedef struct A_Const
{
	pg_node_attr(custom_copy_equal, custom_read_write, custom_query_jumble)

	NodeTag		type;
	union ValUnion val;
	bool		isnull;			/* SQL NULL constant */
	ParseLoc	location;		/* token location, or -1 if unknown */
} A_Const;

/*
 * TypeCast - a CAST expression
 */
typedef struct TypeCast
{
	NodeTag		type;
	Node	   *arg;			/* the expression being casted */
	TypeName   *typeName;		/* the target type */
	ParseLoc	location;		/* token location, or -1 if unknown */
} TypeCast;

/*
 * CollateClause - a COLLATE expression
 */
typedef struct CollateClause
{
	NodeTag		type;
	Node	   *arg;			/* input expression */
	List	   *collname;		/* possibly-qualified collation name */
	ParseLoc	location;		/* token location, or -1 if unknown */
} CollateClause;

/*
 * RoleSpec - a role name or one of a few special values.
 */
typedef enum RoleSpecType
{
	ROLESPEC_CSTRING,			/* role name is stored as a C string */
	ROLESPEC_CURRENT_ROLE,		/* role spec is CURRENT_ROLE */
	ROLESPEC_CURRENT_USER,		/* role spec is CURRENT_USER */
	ROLESPEC_SESSION_USER,		/* role spec is SESSION_USER */
	ROLESPEC_PUBLIC,			/* role name is "public" */
} RoleSpecType;

typedef struct RoleSpec
{
	NodeTag		type;
	RoleSpecType roletype;		/* Type of this rolespec */
	char	   *rolename;		/* filled only for ROLESPEC_CSTRING */
	ParseLoc	location;		/* token location, or -1 if unknown */
} RoleSpec;

/*
 * FuncCall - a function or aggregate invocation
 *
 * agg_order (if not NIL) indicates we saw 'foo(... ORDER BY ...)', or if
 * agg_within_group is true, it was 'foo(...) WITHIN GROUP (ORDER BY ...)'.
 * agg_star indicates we saw a 'foo(*)' construct, while agg_distinct
 * indicates we saw 'foo(DISTINCT ...)'.  In any of these cases, the
 * construct *must* be an aggregate call.  Otherwise, it might be either an
 * aggregate or some other kind of function.  However, if FILTER or OVER is
 * present it had better be an aggregate or window function.
 *
 * Normally, you'd initialize this via makeFuncCall() and then only change the
 * parts of the struct its defaults don't match afterwards, as needed.
 */
typedef struct FuncCall
{
	NodeTag		type;
	List	   *funcname;		/* qualified name of function */
	List	   *args;			/* the arguments (list of exprs) */
	List	   *agg_order;		/* ORDER BY (list of SortBy) */
	Node	   *agg_filter;		/* FILTER clause, if any */
	struct WindowDef *over;		/* OVER clause, if any */
	bool		agg_within_group;	/* ORDER BY appeared in WITHIN GROUP */
	bool		agg_star;		/* argument was really '*' */
	bool		agg_distinct;	/* arguments were labeled DISTINCT */
	bool		func_variadic;	/* last argument was labeled VARIADIC */
	CoercionForm funcformat;	/* how to display this node */
	ParseLoc	location;		/* token location, or -1 if unknown */
} FuncCall;

/*
 * A_Star - '*' representing all columns of a table or compound field
 *
 * This can appear within ColumnRef.fields, A_Indirection.indirection, and
 * ResTarget.indirection lists.
 */
typedef struct A_Star
{
	NodeTag		type;
} A_Star;

/*
 * A_Indices - array subscript or slice bounds ([idx] or [lidx:uidx])
 *
 * In slice case, either or both of lidx and uidx can be NULL (omitted).
 * In non-slice case, uidx holds the single subscript and lidx is always NULL.
 */
typedef struct A_Indices
{
	NodeTag		type;
	bool		is_slice;		/* true if slice (i.e., colon present) */
	Node	   *lidx;			/* slice lower bound, if any */
	Node	   *uidx;			/* subscript, or slice upper bound if any */
} A_Indices;

/*
 * A_Indirection - select a field and/or array element from an expression
 *
 * The indirection list can contain A_Indices nodes (representing
 * subscripting), String nodes (representing field selection --- the
 * string value is the name of the field to select), and A_Star nodes
 * (representing selection of all fields of a composite type).
 * For example, a complex selection operation like
 *				(foo).field1[42][7].field2
 * would be represented with a single A_Indirection node having a 4-element
 * indirection list.
 *
 * Currently, A_Star must appear only as the last list element --- the grammar
 * is responsible for enforcing this!
 */
typedef struct A_Indirection
{
	NodeTag		type;
	Node	   *arg;			/* the thing being selected from */
	List	   *indirection;	/* subscripts and/or field names and/or * */
} A_Indirection;

/*
 * A_ArrayExpr - an ARRAY[] construct
 */
typedef struct A_ArrayExpr
{
	NodeTag		type;
	List	   *elements;		/* array element expressions */
	ParseLoc	list_start;		/* start of the element list */
	ParseLoc	list_end;		/* end of the elements list */
	ParseLoc	location;		/* token location, or -1 if unknown */
} A_ArrayExpr;

/*
 * ResTarget -
 *	  result target (used in target list of pre-transformed parse trees)
 *
 * In a SELECT target list, 'name' is the column label from an
 * 'AS ColumnLabel' clause, or NULL if there was none, and 'val' is the
 * value expression itself.  The 'indirection' field is not used.
 *
 * INSERT uses ResTarget in its target-column-names list.  Here, 'name' is
 * the name of the destination column, 'indirection' stores any subscripts
 * attached to the destination, and 'val' is not used.
 *
 * In an UPDATE target list, 'name' is the name of the destination column,
 * 'indirection' stores any subscripts attached to the destination, and
 * 'val' is the expression to assign.
 *
 * See A_Indirection for more info about what can appear in 'indirection'.
 */
typedef struct ResTarget
{
	NodeTag		type;
	char	   *name;			/* column name or NULL */
	List	   *indirection;	/* subscripts, field names, and '*', or NIL */
	Node	   *val;			/* the value expression to compute or assign */
	ParseLoc	location;		/* token location, or -1 if unknown */
} ResTarget;

/*
 * MultiAssignRef - element of a row source expression for UPDATE
 *
 * In an UPDATE target list, when we have SET (a,b,c) = row-valued-expression,
 * we generate separate ResTarget items for each of a,b,c.  Their "val" trees
 * are MultiAssignRef nodes numbered 1..n, linking to a common copy of the
 * row-valued-expression (which parse analysis will process only once, when
 * handling the MultiAssignRef with colno=1).
 */
typedef struct MultiAssignRef
{
	NodeTag		type;
	Node	   *source;			/* the row-valued expression */
	int			colno;			/* column number for this target (1..n) */
	int			ncolumns;		/* number of targets in the construct */
} MultiAssignRef;

/*
 * SortBy - for ORDER BY clause
 */
typedef struct SortBy
{
	NodeTag		type;
	Node	   *node;			/* expression to sort on */
	SortByDir	sortby_dir;		/* ASC/DESC/USING/default */
	SortByNulls sortby_nulls;	/* NULLS FIRST/LAST */
	List	   *useOp;			/* name of op to use, if SORTBY_USING */
	ParseLoc	location;		/* operator location, or -1 if none/unknown */
} SortBy;

/*
 * WindowDef - raw representation of WINDOW and OVER clauses
 *
 * For entries in a WINDOW list, "name" is the window name being defined.
 * For OVER clauses, we use "name" for the "OVER window" syntax, or "refname"
 * for the "OVER (window)" syntax, which is subtly different --- the latter
 * implies overriding the window frame clause.
 */
typedef struct WindowDef
{
	NodeTag		type;
	char	   *name;			/* window's own name */
	char	   *refname;		/* referenced window name, if any */
	List	   *partitionClause;	/* PARTITION BY expression list */
	List	   *orderClause;	/* ORDER BY (list of SortBy) */
	int			frameOptions;	/* frame_clause options, see below */
	Node	   *startOffset;	/* expression for starting bound, if any */
	Node	   *endOffset;		/* expression for ending bound, if any */
	ParseLoc	location;		/* parse location, or -1 if none/unknown */
} WindowDef;

/*
 * frameOptions is an OR of these bits.  The NONDEFAULT and BETWEEN bits are
 * used so that ruleutils.c can tell which properties were specified and
 * which were defaulted; the correct behavioral bits must be set either way.
 * The START_foo and END_foo options must come in pairs of adjacent bits for
 * the convenience of gram.y, even though some of them are useless/invalid.
 */
#define FRAMEOPTION_NONDEFAULT					0x00001 /* any specified? */
#define FRAMEOPTION_RANGE						0x00002 /* RANGE behavior */
#define FRAMEOPTION_ROWS						0x00004 /* ROWS behavior */
#define FRAMEOPTION_GROUPS						0x00008 /* GROUPS behavior */
#define FRAMEOPTION_BETWEEN						0x00010 /* BETWEEN given? */
#define FRAMEOPTION_START_UNBOUNDED_PRECEDING	0x00020 /* start is U. P. */
#define FRAMEOPTION_END_UNBOUNDED_PRECEDING		0x00040 /* (disallowed) */
#define FRAMEOPTION_START_UNBOUNDED_FOLLOWING	0x00080 /* (disallowed) */
#define FRAMEOPTION_END_UNBOUNDED_FOLLOWING		0x00100 /* end is U. F. */
#define FRAMEOPTION_START_CURRENT_ROW			0x00200 /* start is C. R. */
#define FRAMEOPTION_END_CURRENT_ROW				0x00400 /* end is C. R. */
#define FRAMEOPTION_START_OFFSET_PRECEDING		0x00800 /* start is O. P. */
#define FRAMEOPTION_END_OFFSET_PRECEDING		0x01000 /* end is O. P. */
#define FRAMEOPTION_START_OFFSET_FOLLOWING		0x02000 /* start is O. F. */
#define FRAMEOPTION_END_OFFSET_FOLLOWING		0x04000 /* end is O. F. */
#define FRAMEOPTION_EXCLUDE_CURRENT_ROW			0x08000 /* omit C.R. */
#define FRAMEOPTION_EXCLUDE_GROUP				0x10000 /* omit C.R. & peers */
#define FRAMEOPTION_EXCLUDE_TIES				0x20000 /* omit C.R.'s peers */

#define FRAMEOPTION_START_OFFSET \
	(FRAMEOPTION_START_OFFSET_PRECEDING | FRAMEOPTION_START_OFFSET_FOLLOWING)
#define FRAMEOPTION_END_OFFSET \
	(FRAMEOPTION_END_OFFSET_PRECEDING | FRAMEOPTION_END_OFFSET_FOLLOWING)
#define FRAMEOPTION_EXCLUSION \
	(FRAMEOPTION_EXCLUDE_CURRENT_ROW | FRAMEOPTION_EXCLUDE_GROUP | \
	 FRAMEOPTION_EXCLUDE_TIES)

#define FRAMEOPTION_DEFAULTS \
	(FRAMEOPTION_RANGE | FRAMEOPTION_START_UNBOUNDED_PRECEDING | \
	 FRAMEOPTION_END_CURRENT_ROW)

/*
 * RangeSubselect - subquery appearing in a FROM clause
 */
typedef struct RangeSubselect
{
	NodeTag		type;
	bool		lateral;		/* does it have LATERAL prefix? */
	Node	   *subquery;		/* the untransformed sub-select clause */
	Alias	   *alias;			/* table alias & optional column aliases */
} RangeSubselect;

/*
 * RangeFunction - function call appearing in a FROM clause
 *
 * functions is a List because we use this to represent the construct
 * ROWS FROM(func1(...), func2(...), ...).  Each element of this list is a
 * two-element sublist, the first element being the untransformed function
 * call tree, and the second element being a possibly-empty list of ColumnDef
 * nodes representing any columndef list attached to that function within the
 * ROWS FROM() syntax.
 *
 * alias and coldeflist represent any alias and/or columndef list attached
 * at the top level.  (We disallow coldeflist appearing both here and
 * per-function, but that's checked in parse analysis, not by the grammar.)
 */
typedef struct RangeFunction
{
	NodeTag		type;
	bool		lateral;		/* does it have LATERAL prefix? */
	bool		ordinality;		/* does it have WITH ORDINALITY suffix? */
	bool		is_rowsfrom;	/* is result of ROWS FROM() syntax? */
	List	   *functions;		/* per-function information, see above */
	Alias	   *alias;			/* table alias & optional column aliases */
	List	   *coldeflist;		/* list of ColumnDef nodes to describe result
								 * of function returning RECORD */
} RangeFunction;

/*
 * RangeTableFunc - raw form of "table functions" such as XMLTABLE
 *
 * Note: JSON_TABLE is also a "table function", but it uses JsonTable node,
 * not RangeTableFunc.
 */
typedef struct RangeTableFunc
{
	NodeTag		type;
	bool		lateral;		/* does it have LATERAL prefix? */
	Node	   *docexpr;		/* document expression */
	Node	   *rowexpr;		/* row generator expression */
	List	   *namespaces;		/* list of namespaces as ResTarget */
	List	   *columns;		/* list of RangeTableFuncCol */
	Alias	   *alias;			/* table alias & optional column aliases */
	ParseLoc	location;		/* token location, or -1 if unknown */
} RangeTableFunc;

/*
 * RangeTableFuncCol - one column in a RangeTableFunc->columns
 *
 * If for_ordinality is true (FOR ORDINALITY), then the column is an int4
 * column and the rest of the fields are ignored.
 */
typedef struct RangeTableFuncCol
{
	NodeTag		type;
	char	   *colname;		/* name of generated column */
	TypeName   *typeName;		/* type of generated column */
	bool		for_ordinality; /* does it have FOR ORDINALITY? */
	bool		is_not_null;	/* does it have NOT NULL? */
	Node	   *colexpr;		/* column filter expression */
	Node	   *coldefexpr;		/* column default value expression */
	ParseLoc	location;		/* token location, or -1 if unknown */
} RangeTableFuncCol;

/*
 * RangeTableSample - TABLESAMPLE appearing in a raw FROM clause
 *
 * This node, appearing only in raw parse trees, represents
 *		<relation> TABLESAMPLE <method> (<params>) REPEATABLE (<num>)
 * Currently, the <relation> can only be a RangeVar, but we might in future
 * allow RangeSubselect and other options.  Note that the RangeTableSample
 * is wrapped around the node representing the <relation>, rather than being
 * a subfield of it.
 */
typedef struct RangeTableSample
{
	NodeTag		type;
	Node	   *relation;		/* relation to be sampled */
	List	   *method;			/* sampling method name (possibly qualified) */
	List	   *args;			/* argument(s) for sampling method */
	Node	   *repeatable;		/* REPEATABLE expression, or NULL if none */
	ParseLoc	location;		/* method name location, or -1 if unknown */
} RangeTableSample;

/*
 * ColumnDef - column definition (used in various creates)
 *
 * If the column has a default value, we may have the value expression
 * in either "raw" form (an untransformed parse tree) or "cooked" form
 * (a post-parse-analysis, executable expression tree), depending on
 * how this ColumnDef node was created (by parsing, or by inheritance
 * from an existing relation).  We should never have both in the same node!
 *
 * Similarly, we may have a COLLATE specification in either raw form
 * (represented as a CollateClause with arg==NULL) or cooked form
 * (the collation's OID).
 *
 * The constraints list may contain a CONSTR_DEFAULT item in a raw
 * parsetree produced by gram.y, but transformCreateStmt will remove
 * the item and set raw_default instead.  CONSTR_DEFAULT items
 * should not appear in any subsequent processing.
 */
typedef struct ColumnDef
{
	NodeTag		type;
	char	   *colname;		/* name of column */
	TypeName   *typeName;		/* type of column */
	char	   *compression;	/* compression method for column */
	int16		inhcount;		/* number of times column is inherited */
	bool		is_local;		/* column has local (non-inherited) def'n */
	bool		is_not_null;	/* NOT NULL constraint specified? */
	bool		is_from_type;	/* column definition came from table type */
	char		storage;		/* attstorage setting, or 0 for default */
	char	   *storage_name;	/* attstorage setting name or NULL for default */
	Node	   *raw_default;	/* default value (untransformed parse tree) */
	Node	   *cooked_default; /* default value (transformed expr tree) */
	char		identity;		/* attidentity setting */
	RangeVar   *identitySequence;	/* to store identity sequence name for
									 * ALTER TABLE ... ADD COLUMN */
	char		generated;		/* attgenerated setting */
	CollateClause *collClause;	/* untransformed COLLATE spec, if any */
	Oid			collOid;		/* collation OID (InvalidOid if not set) */
	List	   *constraints;	/* other constraints on column */
	List	   *fdwoptions;		/* per-column FDW options */
	ParseLoc	location;		/* parse location, or -1 if none/unknown */
} ColumnDef;

/*
 * TableLikeClause - CREATE TABLE ( ... LIKE ... ) clause
 */
typedef struct TableLikeClause
{
	NodeTag		type;
	RangeVar   *relation;
	bits32		options;		/* OR of TableLikeOption flags */
	Oid			relationOid;	/* If table has been looked up, its OID */
} TableLikeClause;

typedef enum TableLikeOption
{
	CREATE_TABLE_LIKE_COMMENTS = 1 << 0,
	CREATE_TABLE_LIKE_COMPRESSION = 1 << 1,
	CREATE_TABLE_LIKE_CONSTRAINTS = 1 << 2,
	CREATE_TABLE_LIKE_DEFAULTS = 1 << 3,
	CREATE_TABLE_LIKE_GENERATED = 1 << 4,
	CREATE_TABLE_LIKE_IDENTITY = 1 << 5,
	CREATE_TABLE_LIKE_INDEXES = 1 << 6,
	CREATE_TABLE_LIKE_STATISTICS = 1 << 7,
	CREATE_TABLE_LIKE_STORAGE = 1 << 8,
	CREATE_TABLE_LIKE_ALL = PG_INT32_MAX
} TableLikeOption;

/*
 * IndexElem - index parameters (used in CREATE INDEX, and in ON CONFLICT)
 *
 * For a plain index attribute, 'name' is the name of the table column to
 * index, and 'expr' is NULL.  For an index expression, 'name' is NULL and
 * 'expr' is the expression tree.
 */
typedef struct IndexElem
{
	NodeTag		type;
	char	   *name;			/* name of attribute to index, or NULL */
	Node	   *expr;			/* expression to index, or NULL */
	char	   *indexcolname;	/* name for index column; NULL = default */
	List	   *collation;		/* name of collation; NIL = default */
	List	   *opclass;		/* name of desired opclass; NIL = default */
	List	   *opclassopts;	/* opclass-specific options, or NIL */
	SortByDir	ordering;		/* ASC/DESC/default */
	SortByNulls nulls_ordering; /* FIRST/LAST/default */
} IndexElem;

/*
 * DefElem - a generic "name = value" option definition
 *
 * In some contexts the name can be qualified.  Also, certain SQL commands
 * allow a SET/ADD/DROP action to be attached to option settings, so it's
 * convenient to carry a field for that too.  (Note: currently, it is our
 * practice that the grammar allows namespace and action only in statements
 * where they are relevant; C code can just ignore those fields in other
 * statements.)
 */
typedef enum DefElemAction
{
	DEFELEM_UNSPEC,				/* no action given */
	DEFELEM_SET,
	DEFELEM_ADD,
	DEFELEM_DROP,
} DefElemAction;

typedef struct DefElem
{
	NodeTag		type;
	char	   *defnamespace;	/* NULL if unqualified name */
	char	   *defname;
	Node	   *arg;			/* typically Integer, Float, String, or
								 * TypeName */
	DefElemAction defaction;	/* unspecified action, or SET/ADD/DROP */
	ParseLoc	location;		/* token location, or -1 if unknown */
} DefElem;

/*
 * LockingClause - raw representation of FOR [NO KEY] UPDATE/[KEY] SHARE
 *		options
 *
 * Note: lockedRels == NIL means "all relations in query".  Otherwise it
 * is a list of RangeVar nodes.  (We use RangeVar mainly because it carries
 * a location field --- currently, parse analysis insists on unqualified
 * names in LockingClause.)
 */
typedef struct LockingClause
{
	NodeTag		type;
	List	   *lockedRels;		/* FOR [KEY] UPDATE/SHARE relations */
	LockClauseStrength strength;
	LockWaitPolicy waitPolicy;	/* NOWAIT and SKIP LOCKED */
} LockingClause;

/*
 * XMLSERIALIZE (in raw parse tree only)
 */
typedef struct XmlSerialize
{
	NodeTag		type;
	XmlOptionType xmloption;	/* DOCUMENT or CONTENT */
	Node	   *expr;
	TypeName   *typeName;
	bool		indent;			/* [NO] INDENT */
	ParseLoc	location;		/* token location, or -1 if unknown */
} XmlSerialize;

/* Partitioning related definitions */

/*
 * PartitionElem - parse-time representation of a single partition key
 *
 * expr can be either a raw expression tree or a parse-analyzed expression.
 * We don't store these on-disk, though.
 */
typedef struct PartitionElem
{
	NodeTag		type;
	char	   *name;			/* name of column to partition on, or NULL */
	Node	   *expr;			/* expression to partition on, or NULL */
	List	   *collation;		/* name of collation; NIL = default */
	List	   *opclass;		/* name of desired opclass; NIL = default */
	ParseLoc	location;		/* token location, or -1 if unknown */
} PartitionElem;

typedef enum PartitionStrategy
{
	PARTITION_STRATEGY_LIST = 'l',
	PARTITION_STRATEGY_RANGE = 'r',
	PARTITION_STRATEGY_HASH = 'h',
} PartitionStrategy;

/*
 * PartitionSpec - parse-time representation of a partition key specification
 *
 * This represents the key space we will be partitioning on.
 */
typedef struct PartitionSpec
{
	NodeTag		type;
	PartitionStrategy strategy;
	List	   *partParams;		/* List of PartitionElems */
	ParseLoc	location;		/* token location, or -1 if unknown */
} PartitionSpec;

/*
 * PartitionBoundSpec - a partition bound specification
 *
 * This represents the portion of the partition key space assigned to a
 * particular partition.  These are stored on disk in pg_class.relpartbound.
 */
struct PartitionBoundSpec
{
	NodeTag		type;

	char		strategy;		/* see PARTITION_STRATEGY codes above */
	bool		is_default;		/* is it a default partition bound? */

	/* Partitioning info for HASH strategy: */
	int			modulus;
	int			remainder;

	/* Partitioning info for LIST strategy: */
	List	   *listdatums;		/* List of Consts (or A_Consts in raw tree) */

	/* Partitioning info for RANGE strategy: */
	List	   *lowerdatums;	/* List of PartitionRangeDatums */
	List	   *upperdatums;	/* List of PartitionRangeDatums */

	ParseLoc	location;		/* token location, or -1 if unknown */
};

/*
 * PartitionRangeDatum - one of the values in a range partition bound
 *
 * This can be MINVALUE, MAXVALUE or a specific bounded value.
 */
typedef enum PartitionRangeDatumKind
{
	PARTITION_RANGE_DATUM_MINVALUE = -1,	/* less than any other value */
	PARTITION_RANGE_DATUM_VALUE = 0,	/* a specific (bounded) value */
	PARTITION_RANGE_DATUM_MAXVALUE = 1, /* greater than any other value */
} PartitionRangeDatumKind;

typedef struct PartitionRangeDatum
{
	NodeTag		type;

	PartitionRangeDatumKind kind;
	Node	   *value;			/* Const (or A_Const in raw tree), if kind is
								 * PARTITION_RANGE_DATUM_VALUE, else NULL */

	ParseLoc	location;		/* token location, or -1 if unknown */
} PartitionRangeDatum;

/*
 * PartitionCmd - info for ALTER TABLE/INDEX ATTACH/DETACH PARTITION commands
 */
typedef struct PartitionCmd
{
	NodeTag		type;
	RangeVar   *name;			/* name of partition to attach/detach */
	PartitionBoundSpec *bound;	/* FOR VALUES, if attaching */
	bool		concurrent;
} PartitionCmd;

/****************************************************************************
 *	Nodes for a Query tree
 ****************************************************************************/

/*--------------------
 * RangeTblEntry -
 *	  A range table is a List of RangeTblEntry nodes.
 *
 *	  A range table entry may represent a plain relation, a sub-select in
 *	  FROM, or the result of a JOIN clause.  (Only explicit JOIN syntax
 *	  produces an RTE, not the implicit join resulting from multiple FROM
 *	  items.  This is because we only need the RTE to deal with SQL features
 *	  like outer joins and join-output-column aliasing.)  Other special
 *	  RTE types also exist, as indicated by RTEKind.
 *
 *	  Note that we consider RTE_RELATION to cover anything that has a pg_class
 *	  entry.  relkind distinguishes the sub-cases.
 *
 *	  alias is an Alias node representing the AS alias-clause attached to the
 *	  FROM expression, or NULL if no clause.
 *
 *	  eref is the table reference name and column reference names (either
 *	  real or aliases).  Note that system columns (OID etc) are not included
 *	  in the column list.
 *	  eref->aliasname is required to be present, and should generally be used
 *	  to identify the RTE for error messages etc.
 *
 *	  In RELATION RTEs, the colnames in both alias and eref are indexed by
 *	  physical attribute number; this means there must be colname entries for
 *	  dropped columns.  When building an RTE we insert empty strings ("") for
 *	  dropped columns.  Note however that a stored rule may have nonempty
 *	  colnames for columns dropped since the rule was created (and for that
 *	  matter the colnames might be out of date due to column renamings).
 *	  The same comments apply to FUNCTION RTEs when a function's return type
 *	  is a named composite type.
 *
 *	  In JOIN RTEs, the colnames in both alias and eref are one-to-one with
 *	  joinaliasvars entries.  A JOIN RTE will omit columns of its inputs when
 *	  those columns are known to be dropped at parse time.  Again, however,
 *	  a stored rule might contain entries for columns dropped since the rule
 *	  was created.  (This is only possible for columns not actually referenced
 *	  in the rule.)  When loading a stored rule, we replace the joinaliasvars
 *	  items for any such columns with null pointers.  (We can't simply delete
 *	  them from the joinaliasvars list, because that would affect the attnums
 *	  of Vars referencing the rest of the list.)
 *
 *	  inFromCl marks those range variables that are listed in the FROM clause.
 *	  It's false for RTEs that are added to a query behind the scenes, such
 *	  as the NEW and OLD variables for a rule, or the subqueries of a UNION.
 *	  This flag is not used during parsing (except in transformLockingClause,
 *	  q.v.); the parser now uses a separate "namespace" data structure to
 *	  control visibility.  But it is needed by ruleutils.c to determine
 *	  whether RTEs should be shown in decompiled queries.
 *
 *	  securityQuals is a list of security barrier quals (boolean expressions),
 *	  to be tested in the listed order before returning a row from the
 *	  relation.  It is always NIL in parser output.  Entries are added by the
 *	  rewriter to implement security-barrier views and/or row-level security.
 *	  Note that the planner turns each boolean expression into an implicitly
 *	  AND'ed sublist, as is its usual habit with qualification expressions.
 *--------------------
 */
typedef enum RTEKind
{
	RTE_RELATION,				/* ordinary relation reference */
	RTE_SUBQUERY,				/* subquery in FROM */
	RTE_JOIN,					/* join */
	RTE_FUNCTION,				/* function in FROM */
	RTE_TABLEFUNC,				/* TableFunc(.., column list) */
	RTE_VALUES,					/* VALUES (<exprlist>), (<exprlist>), ... */
	RTE_CTE,					/* common table expr (WITH list element) */
	RTE_NAMEDTUPLESTORE,		/* tuplestore, e.g. for AFTER triggers */
	RTE_RESULT,					/* RTE represents an empty FROM clause; such
								 * RTEs are added by the planner, they're not
								 * present during parsing or rewriting */
	RTE_GROUP,					/* the grouping step */
} RTEKind;

typedef struct RangeTblEntry
{
	pg_node_attr(custom_read_write)

	NodeTag		type;

	/*
	 * Fields valid in all RTEs:
	 *
	 * put alias + eref first to make dump more legible
	 */
	/* user-written alias clause, if any */
	Alias	   *alias pg_node_attr(query_jumble_ignore);

	/*
	 * Expanded reference names.  This uses a custom query jumble function so
	 * that the table name is included in the computation, but not its list of
	 * columns.
	 */
	Alias	   *eref pg_node_attr(custom_query_jumble);

	RTEKind		rtekind;		/* see above */

	/*
	 * Fields valid for a plain relation RTE (else zero):
	 *
	 * inh is true for relation references that should be expanded to include
	 * inheritance children, if the rel has any.  In the parser, this will
	 * only be true for RTE_RELATION entries.  The planner also uses this
	 * field to mark RTE_SUBQUERY entries that contain UNION ALL queries that
	 * it has flattened into pulled-up subqueries (creating a structure much
	 * like the effects of inheritance).
	 *
	 * rellockmode is really LOCKMODE, but it's declared int to avoid having
	 * to include lock-related headers here.  It must be RowExclusiveLock if
	 * the RTE is an INSERT/UPDATE/DELETE/MERGE target, else RowShareLock if
	 * the RTE is a SELECT FOR UPDATE/FOR SHARE target, else AccessShareLock.
	 *
	 * Note: in some cases, rule expansion may result in RTEs that are marked
	 * with RowExclusiveLock even though they are not the target of the
	 * current query; this happens if a DO ALSO rule simply scans the original
	 * target table.  We leave such RTEs with their original lockmode so as to
	 * avoid getting an additional, lesser lock.
	 *
	 * perminfoindex is 1-based index of the RTEPermissionInfo belonging to
	 * this RTE in the containing struct's list of same; 0 if permissions need
	 * not be checked for this RTE.
	 *
	 * As a special case, relid, relkind, rellockmode, and perminfoindex can
	 * also be set (nonzero) in an RTE_SUBQUERY RTE.  This occurs when we
	 * convert an RTE_RELATION RTE naming a view into an RTE_SUBQUERY
	 * containing the view's query.  We still need to perform run-time locking
	 * and permission checks on the view, even though it's not directly used
	 * in the query anymore, and the most expedient way to do that is to
	 * retain these fields from the old state of the RTE.
	 *
	 * As a special case, RTE_NAMEDTUPLESTORE can also set relid to indicate
	 * that the tuple format of the tuplestore is the same as the referenced
	 * relation.  This allows plans referencing AFTER trigger transition
	 * tables to be invalidated if the underlying table is altered.
	 */
	/* OID of the relation */
	Oid			relid pg_node_attr(query_jumble_ignore);
	/* inheritance requested? */
	bool		inh;
	/* relation kind (see pg_class.relkind) */
	char		relkind pg_node_attr(query_jumble_ignore);
	/* lock level that query requires on the rel */
	int			rellockmode pg_node_attr(query_jumble_ignore);
	/* index of RTEPermissionInfo entry, or 0 */
	Index		perminfoindex pg_node_attr(query_jumble_ignore);
	/* sampling info, or NULL */
	struct TableSampleClause *tablesample;

	/*
	 * Fields valid for a subquery RTE (else NULL):
	 */
	/* the sub-query */
	Query	   *subquery;
	/* is from security_barrier view? */
	bool		security_barrier pg_node_attr(query_jumble_ignore);

	/*
	 * Fields valid for a join RTE (else NULL/zero):
	 *
	 * joinaliasvars is a list of (usually) Vars corresponding to the columns
	 * of the join result.  An alias Var referencing column K of the join
	 * result can be replaced by the K'th element of joinaliasvars --- but to
	 * simplify the task of reverse-listing aliases correctly, we do not do
	 * that until planning time.  In detail: an element of joinaliasvars can
	 * be a Var of one of the join's input relations, or such a Var with an
	 * implicit coercion to the join's output column type, or a COALESCE
	 * expression containing the two input column Vars (possibly coerced).
	 * Elements beyond the first joinmergedcols entries are always just Vars,
	 * and are never referenced from elsewhere in the query (that is, join
	 * alias Vars are generated only for merged columns).  We keep these
	 * entries only because they're needed in expandRTE() and similar code.
	 *
	 * Vars appearing within joinaliasvars are marked with varnullingrels sets
	 * that describe the nulling effects of this join and lower ones.  This is
	 * essential for FULL JOIN cases, because the COALESCE expression only
	 * describes the semantics correctly if its inputs have been nulled by the
	 * join.  For other cases, it allows expandRTE() to generate a valid
	 * representation of the join's output without consulting additional
	 * parser state.
	 *
	 * Within a Query loaded from a stored rule, it is possible for non-merged
	 * joinaliasvars items to be null pointers, which are placeholders for
	 * (necessarily unreferenced) columns dropped since the rule was made.
	 * Also, once planning begins, joinaliasvars items can be almost anything,
	 * as a result of subquery-flattening substitutions.
	 *
	 * joinleftcols is an integer list of physical column numbers of the left
	 * join input rel that are included in the join; likewise joinrighttcols
	 * for the right join input rel.  (Which rels those are can be determined
	 * from the associated JoinExpr.)  If the join is USING/NATURAL, then the
	 * first joinmergedcols entries in each list identify the merged columns.
	 * The merged columns come first in the join output, then remaining
	 * columns of the left input, then remaining columns of the right.
	 *
	 * Note that input columns could have been dropped after creation of a
	 * stored rule, if they are not referenced in the query (in particular,
	 * merged columns could not be dropped); this is not accounted for in
	 * joinleftcols/joinrighttcols.
	 */
	JoinType	jointype;
	/* number of merged (JOIN USING) columns */
	int			joinmergedcols pg_node_attr(query_jumble_ignore);
	/* list of alias-var expansions */
	List	   *joinaliasvars pg_node_attr(query_jumble_ignore);
	/* left-side input column numbers */
	List	   *joinleftcols pg_node_attr(query_jumble_ignore);
	/* right-side input column numbers */
	List	   *joinrightcols pg_node_attr(query_jumble_ignore);

	/*
	 * join_using_alias is an alias clause attached directly to JOIN/USING. It
	 * is different from the alias field (below) in that it does not hide the
	 * range variables of the tables being joined.
	 */
	Alias	   *join_using_alias pg_node_attr(query_jumble_ignore);

	/*
	 * Fields valid for a function RTE (else NIL/zero):
	 *
	 * When funcordinality is true, the eref->colnames list includes an alias
	 * for the ordinality column.  The ordinality column is otherwise
	 * implicit, and must be accounted for "by hand" in places such as
	 * expandRTE().
	 */
	/* list of RangeTblFunction nodes */
	List	   *functions;
	/* is this called WITH ORDINALITY? */
	bool		funcordinality;

	/*
	 * Fields valid for a TableFunc RTE (else NULL):
	 */
	TableFunc  *tablefunc;

	/*
	 * Fields valid for a values RTE (else NIL):
	 */
	/* list of expression lists */
	List	   *values_lists;

	/*
	 * Fields valid for a CTE RTE (else NULL/zero):
	 */
	/* name of the WITH list item */
	char	   *ctename;
	/* number of query levels up */
	Index		ctelevelsup;
	/* is this a recursive self-reference? */
	bool		self_reference pg_node_attr(query_jumble_ignore);

	/*
	 * Fields valid for CTE, VALUES, ENR, and TableFunc RTEs (else NIL):
	 *
	 * We need these for CTE RTEs so that the types of self-referential
	 * columns are well-defined.  For VALUES RTEs, storing these explicitly
	 * saves having to re-determine the info by scanning the values_lists. For
	 * ENRs, we store the types explicitly here (we could get the information
	 * from the catalogs if 'relid' was supplied, but we'd still need these
	 * for TupleDesc-based ENRs, so we might as well always store the type
	 * info here).  For TableFuncs, these fields are redundant with data in
	 * the TableFunc node, but keeping them here allows some code sharing with
	 * the other cases.
	 *
	 * For ENRs only, we have to consider the possibility of dropped columns.
	 * A dropped column is included in these lists, but it will have zeroes in
	 * all three lists (as well as an empty-string entry in eref).  Testing
	 * for zero coltype is the standard way to detect a dropped column.
	 */
	/* OID list of column type OIDs */
	List	   *coltypes pg_node_attr(query_jumble_ignore);
	/* integer list of column typmods */
	List	   *coltypmods pg_node_attr(query_jumble_ignore);
	/* OID list of column collation OIDs */
	List	   *colcollations pg_node_attr(query_jumble_ignore);

	/*
	 * Fields valid for ENR RTEs (else NULL/zero):
	 */
	/* name of ephemeral named relation */
	char	   *enrname;
	/* estimated or actual from caller */
	Cardinality enrtuples pg_node_attr(query_jumble_ignore);

	/*
	 * Fields valid for a GROUP RTE (else NIL):
	 */
	/* list of grouping expressions */
	List	   *groupexprs pg_node_attr(query_jumble_ignore);

	/*
	 * Fields valid in all RTEs:
	 */
	/* was LATERAL specified? */
	bool		lateral pg_node_attr(query_jumble_ignore);
	/* present in FROM clause? */
	bool		inFromCl pg_node_attr(query_jumble_ignore);
	/* security barrier quals to apply, if any */
	List	   *securityQuals pg_node_attr(query_jumble_ignore);
} RangeTblEntry;

/*
 * RTEPermissionInfo
 * 		Per-relation information for permission checking. Added to the Query
 * 		node by the parser when adding the corresponding RTE to the query
 * 		range table and subsequently editorialized on by the rewriter if
 * 		needed after rule expansion.
 *
 * Only the relations directly mentioned in the query are checked for
 * access permissions by the core executor, so only their RTEPermissionInfos
 * are present in the Query.  However, extensions may want to check inheritance
 * children too, depending on the value of rte->inh, so it's copied in 'inh'
 * for their perusal.
 *
 * requiredPerms and checkAsUser specify run-time access permissions checks
 * to be performed at query startup.  The user must have *all* of the
 * permissions that are OR'd together in requiredPerms (never 0!).  If
 * checkAsUser is not zero, then do the permissions checks using the access
 * rights of that user, not the current effective user ID.  (This allows rules
 * to act as setuid gateways.)
 *
 * For SELECT/INSERT/UPDATE permissions, if the user doesn't have table-wide
 * permissions then it is sufficient to have the permissions on all columns
 * identified in selectedCols (for SELECT) and/or insertedCols and/or
 * updatedCols (INSERT with ON CONFLICT DO UPDATE may have all 3).
 * selectedCols, insertedCols and updatedCols are bitmapsets, which cannot have
 * negative integer members, so we subtract FirstLowInvalidHeapAttributeNumber
 * from column numbers before storing them in these fields.  A whole-row Var
 * reference is represented by setting the bit for InvalidAttrNumber.
 *
 * updatedCols is also used in some other places, for example, to determine
 * which triggers to fire and in FDWs to know which changed columns they need
 * to ship off.
 */
typedef struct RTEPermissionInfo
{
	NodeTag		type;

	Oid			relid;			/* relation OID */
	bool		inh;			/* separately check inheritance children? */
	AclMode		requiredPerms;	/* bitmask of required access permissions */
	Oid			checkAsUser;	/* if valid, check access as this role */
	Bitmapset  *selectedCols;	/* columns needing SELECT permission */
	Bitmapset  *insertedCols;	/* columns needing INSERT permission */
	Bitmapset  *updatedCols;	/* columns needing UPDATE permission */
} RTEPermissionInfo;

/*
 * RangeTblFunction -
 *	  RangeTblEntry subsidiary data for one function in a FUNCTION RTE.
 *
 * If the function had a column definition list (required for an
 * otherwise-unspecified RECORD result), funccolnames lists the names given
 * in the definition list, funccoltypes lists their declared column types,
 * funccoltypmods lists their typmods, funccolcollations their collations.
 * Otherwise, those fields are NIL.
 *
 * Notice we don't attempt to store info about the results of functions
 * returning named composite types, because those can change from time to
 * time.  We do however remember how many columns we thought the type had
 * (including dropped columns!), so that we can successfully ignore any
 * columns added after the query was parsed.
 *
 * The query jumbling only needs to track the function expression.
 */
typedef struct RangeTblFunction
{
	NodeTag		type;

	Node	   *funcexpr;		/* expression tree for func call */
	/* number of columns it contributes to RTE */
	int			funccolcount pg_node_attr(query_jumble_ignore);
	/* These fields record the contents of a column definition list, if any: */
	/* column names (list of String) */
	List	   *funccolnames pg_node_attr(query_jumble_ignore);
	/* OID list of column type OIDs */
	List	   *funccoltypes pg_node_attr(query_jumble_ignore);
	/* integer list of column typmods */
	List	   *funccoltypmods pg_node_attr(query_jumble_ignore);
	/* OID list of column collation OIDs */
	List	   *funccolcollations pg_node_attr(query_jumble_ignore);

	/* This is set during planning for use by the executor: */
	/* PARAM_EXEC Param IDs affecting this func */
	Bitmapset  *funcparams pg_node_attr(query_jumble_ignore);
} RangeTblFunction;

/*
 * TableSampleClause - TABLESAMPLE appearing in a transformed FROM clause
 *
 * Unlike RangeTableSample, this is a subnode of the relevant RangeTblEntry.
 */
typedef struct TableSampleClause
{
	NodeTag		type;
	Oid			tsmhandler;		/* OID of the tablesample handler function */
	List	   *args;			/* tablesample argument expression(s) */
	Expr	   *repeatable;		/* REPEATABLE expression, or NULL if none */
} TableSampleClause;

/*
 * WithCheckOption -
 *		representation of WITH CHECK OPTION checks to be applied to new tuples
 *		when inserting/updating an auto-updatable view, or RLS WITH CHECK
 *		policies to be applied when inserting/updating a relation with RLS.
 */
typedef enum WCOKind
{
	WCO_VIEW_CHECK,				/* WCO on an auto-updatable view */
	WCO_RLS_INSERT_CHECK,		/* RLS INSERT WITH CHECK policy */
	WCO_RLS_UPDATE_CHECK,		/* RLS UPDATE WITH CHECK policy */
	WCO_RLS_CONFLICT_CHECK,		/* RLS ON CONFLICT DO UPDATE USING policy */
	WCO_RLS_MERGE_UPDATE_CHECK, /* RLS MERGE UPDATE USING policy */
	WCO_RLS_MERGE_DELETE_CHECK, /* RLS MERGE DELETE USING policy */
} WCOKind;

typedef struct WithCheckOption
{
	NodeTag		type;
	WCOKind		kind;			/* kind of WCO */
	char	   *relname;		/* name of relation that specified the WCO */
	char	   *polname;		/* name of RLS policy being checked */
	Node	   *qual;			/* constraint qual to check */
	bool		cascaded;		/* true for a cascaded WCO on a view */
} WithCheckOption;

/*
 * SortGroupClause -
 *		representation of ORDER BY, GROUP BY, PARTITION BY,
 *		DISTINCT, DISTINCT ON items
 *
 * You might think that ORDER BY is only interested in defining ordering,
 * and GROUP/DISTINCT are only interested in defining equality.  However,
 * one way to implement grouping is to sort and then apply a "uniq"-like
 * filter.  So it's also interesting to keep track of possible sort operators
 * for GROUP/DISTINCT, and in particular to try to sort for the grouping
 * in a way that will also yield a requested ORDER BY ordering.  So we need
 * to be able to compare ORDER BY and GROUP/DISTINCT lists, which motivates
 * the decision to give them the same representation.
 *
 * tleSortGroupRef must match ressortgroupref of exactly one entry of the
 *		query's targetlist; that is the expression to be sorted or grouped by.
 * eqop is the OID of the equality operator.
 * sortop is the OID of the ordering operator (a "<" or ">" operator),
 *		or InvalidOid if not available.
 * nulls_first means about what you'd expect.  If sortop is InvalidOid
 *		then nulls_first is meaningless and should be set to false.
 * hashable is true if eqop is hashable (note this condition also depends
 *		on the datatype of the input expression).
 *
 * In an ORDER BY item, all fields must be valid.  (The eqop isn't essential
 * here, but it's cheap to get it along with the sortop, and requiring it
 * to be valid eases comparisons to grouping items.)  Note that this isn't
 * actually enough information to determine an ordering: if the sortop is
 * collation-sensitive, a collation OID is needed too.  We don't store the
 * collation in SortGroupClause because it's not available at the time the
 * parser builds the SortGroupClause; instead, consult the exposed collation
 * of the referenced targetlist expression to find out what it is.
 *
 * In a grouping item, eqop must be valid.  If the eqop is a btree equality
 * operator, then sortop should be set to a compatible ordering operator.
 * We prefer to set eqop/sortop/nulls_first to match any ORDER BY item that
 * the query presents for the same tlist item.  If there is none, we just
 * use the default ordering op for the datatype.
 *
 * If the tlist item's type has a hash opclass but no btree opclass, then
 * we will set eqop to the hash equality operator, sortop to InvalidOid,
 * and nulls_first to false.  A grouping item of this kind can only be
 * implemented by hashing, and of course it'll never match an ORDER BY item.
 *
 * The hashable flag is provided since we generally have the requisite
 * information readily available when the SortGroupClause is constructed,
 * and it's relatively expensive to get it again later.  Note there is no
 * need for a "sortable" flag since OidIsValid(sortop) serves the purpose.
 *
 * A query might have both ORDER BY and DISTINCT (or DISTINCT ON) clauses.
 * In SELECT DISTINCT, the distinctClause list is as long or longer than the
 * sortClause list, while in SELECT DISTINCT ON it's typically shorter.
 * The two lists must match up to the end of the shorter one --- the parser
 * rearranges the distinctClause if necessary to make this true.  (This
 * restriction ensures that only one sort step is needed to both satisfy the
 * ORDER BY and set up for the Unique step.  This is semantically necessary
 * for DISTINCT ON, and presents no real drawback for DISTINCT.)
 */
typedef struct SortGroupClause
{
	NodeTag		type;
	Index		tleSortGroupRef;	/* reference into targetlist */
	Oid			eqop;			/* the equality operator ('=' op) */
	Oid			sortop;			/* the ordering operator ('<' op), or 0 */
	bool		reverse_sort;	/* is sortop a "greater than" operator? */
	bool		nulls_first;	/* do NULLs come before normal values? */
	/* can eqop be implemented by hashing? */
	bool		hashable pg_node_attr(query_jumble_ignore);
} SortGroupClause;

/*
 * GroupingSet -
 *		representation of CUBE, ROLLUP and GROUPING SETS clauses
 *
 * In a Query with grouping sets, the groupClause contains a flat list of
 * SortGroupClause nodes for each distinct expression used.  The actual
 * structure of the GROUP BY clause is given by the groupingSets tree.
 *
 * In the raw parser output, GroupingSet nodes (of all types except SIMPLE
 * which is not used) are potentially mixed in with the expressions in the
 * groupClause of the SelectStmt.  (An expression can't contain a GroupingSet,
 * but a list may mix GroupingSet and expression nodes.)  At this stage, the
 * content of each node is a list of expressions, some of which may be RowExprs
 * which represent sublists rather than actual row constructors, and nested
 * GroupingSet nodes where legal in the grammar.  The structure directly
 * reflects the query syntax.
 *
 * In parse analysis, the transformed expressions are used to build the tlist
 * and groupClause list (of SortGroupClause nodes), and the groupingSets tree
 * is eventually reduced to a fixed format:
 *
 * EMPTY nodes represent (), and obviously have no content
 *
 * SIMPLE nodes represent a list of one or more expressions to be treated as an
 * atom by the enclosing structure; the content is an integer list of
 * ressortgroupref values (see SortGroupClause)
 *
 * CUBE and ROLLUP nodes contain a list of one or more SIMPLE nodes.
 *
 * SETS nodes contain a list of EMPTY, SIMPLE, CUBE or ROLLUP nodes, but after
 * parse analysis they cannot contain more SETS nodes; enough of the syntactic
 * transforms of the spec have been applied that we no longer have arbitrarily
 * deep nesting (though we still preserve the use of cube/rollup).
 *
 * Note that if the groupingSets tree contains no SIMPLE nodes (only EMPTY
 * nodes at the leaves), then the groupClause will be empty, but this is still
 * an aggregation query (similar to using aggs or HAVING without GROUP BY).
 *
 * As an example, the following clause:
 *
 * GROUP BY GROUPING SETS ((a,b), CUBE(c,(d,e)))
 *
 * looks like this after raw parsing:
 *
 * SETS( RowExpr(a,b) , CUBE( c, RowExpr(d,e) ) )
 *
 * and parse analysis converts it to:
 *
 * SETS( SIMPLE(1,2), CUBE( SIMPLE(3), SIMPLE(4,5) ) )
 */
typedef enum GroupingSetKind
{
	GROUPING_SET_EMPTY,
	GROUPING_SET_SIMPLE,
	GROUPING_SET_ROLLUP,
	GROUPING_SET_CUBE,
	GROUPING_SET_SETS,
} GroupingSetKind;

typedef struct GroupingSet
{
	NodeTag		type;
	GroupingSetKind kind pg_node_attr(query_jumble_ignore);
	List	   *content;
	ParseLoc	location;
} GroupingSet;

/*
 * WindowClause -
 *		transformed representation of WINDOW and OVER clauses
 *
 * A parsed Query's windowClause list contains these structs.  "name" is set
 * if the clause originally came from WINDOW, and is NULL if it originally
 * was an OVER clause (but note that we collapse out duplicate OVERs).
 * partitionClause and orderClause are lists of SortGroupClause structs.
 * partitionClause is sanitized by the query planner to remove any columns or
 * expressions belonging to redundant PathKeys.
 * If we have RANGE with offset PRECEDING/FOLLOWING, the semantics of that are
 * specified by startInRangeFunc/inRangeColl/inRangeAsc/inRangeNullsFirst
 * for the start offset, or endInRangeFunc/inRange* for the end offset.
 * winref is an ID number referenced by WindowFunc nodes; it must be unique
 * among the members of a Query's windowClause list.
 * When refname isn't null, the partitionClause is always copied from there;
 * the orderClause might or might not be copied (see copiedOrder); the framing
 * options are never copied, per spec.
 *
 * The information relevant for the query jumbling is the partition clause
 * type and its bounds.
 */
typedef struct WindowClause
{
	NodeTag		type;
	/* window name (NULL in an OVER clause) */
	char	   *name pg_node_attr(query_jumble_ignore);
	/* referenced window name, if any */
	char	   *refname pg_node_attr(query_jumble_ignore);
	List	   *partitionClause;	/* PARTITION BY list */
	/* ORDER BY list */
	List	   *orderClause;
	int			frameOptions;	/* frame_clause options, see WindowDef */
	Node	   *startOffset;	/* expression for starting bound, if any */
	Node	   *endOffset;		/* expression for ending bound, if any */
	/* in_range function for startOffset */
	Oid			startInRangeFunc pg_node_attr(query_jumble_ignore);
	/* in_range function for endOffset */
	Oid			endInRangeFunc pg_node_attr(query_jumble_ignore);
	/* collation for in_range tests */
	Oid			inRangeColl pg_node_attr(query_jumble_ignore);
	/* use ASC sort order for in_range tests? */
	bool		inRangeAsc pg_node_attr(query_jumble_ignore);
	/* nulls sort first for in_range tests? */
	bool		inRangeNullsFirst pg_node_attr(query_jumble_ignore);
	Index		winref;			/* ID referenced by window functions */
	/* did we copy orderClause from refname? */
	bool		copiedOrder pg_node_attr(query_jumble_ignore);
} WindowClause;

/*
 * RowMarkClause -
 *	   parser output representation of FOR [KEY] UPDATE/SHARE clauses
 *
 * Query.rowMarks contains a separate RowMarkClause node for each relation
 * identified as a FOR [KEY] UPDATE/SHARE target.  If one of these clauses
 * is applied to a subquery, we generate RowMarkClauses for all normal and
 * subquery rels in the subquery, but they are marked pushedDown = true to
 * distinguish them from clauses that were explicitly written at this query
 * level.  Also, Query.hasForUpdate tells whether there were explicit FOR
 * UPDATE/SHARE/KEY SHARE clauses in the current query level.
 */
typedef struct RowMarkClause
{
	NodeTag		type;
	Index		rti;			/* range table index of target relation */
	LockClauseStrength strength;
	LockWaitPolicy waitPolicy;	/* NOWAIT and SKIP LOCKED */
	bool		pushedDown;		/* pushed down from higher query level? */
} RowMarkClause;

/*
 * WithClause -
 *	   representation of WITH clause
 *
 * Note: WithClause does not propagate into the Query representation;
 * but CommonTableExpr does.
 */
typedef struct WithClause
{
	NodeTag		type;
	List	   *ctes;			/* list of CommonTableExprs */
	bool		recursive;		/* true = WITH RECURSIVE */
	ParseLoc	location;		/* token location, or -1 if unknown */
} WithClause;

/*
 * InferClause -
 *		ON CONFLICT unique index inference clause
 *
 * Note: InferClause does not propagate into the Query representation.
 */
typedef struct InferClause
{
	NodeTag		type;
	List	   *indexElems;		/* IndexElems to infer unique index */
	Node	   *whereClause;	/* qualification (partial-index predicate) */
	char	   *conname;		/* Constraint name, or NULL if unnamed */
	ParseLoc	location;		/* token location, or -1 if unknown */
} InferClause;

/*
 * OnConflictClause -
 *		representation of ON CONFLICT clause
 *
 * Note: OnConflictClause does not propagate into the Query representation.
 */
typedef struct OnConflictClause
{
	NodeTag		type;
	OnConflictAction action;	/* DO NOTHING or UPDATE? */
	InferClause *infer;			/* Optional index inference clause */
	List	   *targetList;		/* the target list (of ResTarget) */
	Node	   *whereClause;	/* qualifications */
	ParseLoc	location;		/* token location, or -1 if unknown */
} OnConflictClause;

/*
 * CommonTableExpr -
 *	   representation of WITH list element
 */

typedef enum CTEMaterialize
{
	CTEMaterializeDefault,		/* no option specified */
	CTEMaterializeAlways,		/* MATERIALIZED */
	CTEMaterializeNever,		/* NOT MATERIALIZED */
} CTEMaterialize;

typedef struct CTESearchClause
{
	NodeTag		type;
	List	   *search_col_list;
	bool		search_breadth_first;
	char	   *search_seq_column;
	ParseLoc	location;
} CTESearchClause;

typedef struct CTECycleClause
{
	NodeTag		type;
	List	   *cycle_col_list;
	char	   *cycle_mark_column;
	Node	   *cycle_mark_value;
	Node	   *cycle_mark_default;
	char	   *cycle_path_column;
	ParseLoc	location;
	/* These fields are set during parse analysis: */
	Oid			cycle_mark_type;	/* common type of _value and _default */
	int			cycle_mark_typmod;
	Oid			cycle_mark_collation;
	Oid			cycle_mark_neop;	/* <> operator for type */
} CTECycleClause;

typedef struct CommonTableExpr
{
	NodeTag		type;

	/*
	 * Query name (never qualified).  The string name is included in the query
	 * jumbling because RTE_CTE RTEs need it.
	 */
	char	   *ctename;
	/* optional list of column names */
	List	   *aliascolnames pg_node_attr(query_jumble_ignore);
	CTEMaterialize ctematerialized; /* is this an optimization fence? */
	/* SelectStmt/InsertStmt/etc before parse analysis, Query afterwards: */
	Node	   *ctequery;		/* the CTE's subquery */
	CTESearchClause *search_clause pg_node_attr(query_jumble_ignore);
	CTECycleClause *cycle_clause pg_node_attr(query_jumble_ignore);
	ParseLoc	location;		/* token location, or -1 if unknown */
	/* These fields are set during parse analysis: */
	/* is this CTE actually recursive? */
	bool		cterecursive pg_node_attr(query_jumble_ignore);

	/*
	 * Number of RTEs referencing this CTE (excluding internal
	 * self-references), irrelevant for query jumbling.
	 */
	int			cterefcount pg_node_attr(query_jumble_ignore);
	/* list of output column names */
	List	   *ctecolnames pg_node_attr(query_jumble_ignore);
	/* OID list of output column type OIDs */
	List	   *ctecoltypes pg_node_attr(query_jumble_ignore);
	/* integer list of output column typmods */
	List	   *ctecoltypmods pg_node_attr(query_jumble_ignore);
	/* OID list of column collation OIDs */
	List	   *ctecolcollations pg_node_attr(query_jumble_ignore);
} CommonTableExpr;

/* Convenience macro to get the output tlist of a CTE's query */
#define GetCTETargetList(cte) \
	(AssertMacro(IsA((cte)->ctequery, Query)), \
	 ((Query *) (cte)->ctequery)->commandType == CMD_SELECT ? \
	 ((Query *) (cte)->ctequery)->targetList : \
	 ((Query *) (cte)->ctequery)->returningList)

/*
 * MergeWhenClause -
 *		raw parser representation of a WHEN clause in a MERGE statement
 *
 * This is transformed into MergeAction by parse analysis
 */
typedef struct MergeWhenClause
{
	NodeTag		type;
	MergeMatchKind matchKind;	/* MATCHED/NOT MATCHED BY SOURCE/TARGET */
	CmdType		commandType;	/* INSERT/UPDATE/DELETE/DO NOTHING */
	OverridingKind override;	/* OVERRIDING clause */
	Node	   *condition;		/* WHEN conditions (raw parser) */
	List	   *targetList;		/* INSERT/UPDATE targetlist */
	/* the following members are only used in INSERT actions */
	List	   *values;			/* VALUES to INSERT, or NULL */
} MergeWhenClause;

/*
 * ReturningOptionKind -
 *		Possible kinds of option in RETURNING WITH(...) list
 *
 * Currently, this is used only for specifying OLD/NEW aliases.
 */
typedef enum ReturningOptionKind
{
	RETURNING_OPTION_OLD,		/* specify alias for OLD in RETURNING */
	RETURNING_OPTION_NEW,		/* specify alias for NEW in RETURNING */
} ReturningOptionKind;

/*
 * ReturningOption -
 *		An individual option in the RETURNING WITH(...) list
 */
typedef struct ReturningOption
{
	NodeTag		type;
	ReturningOptionKind option; /* specified option */
	char	   *value;			/* option's value */
	ParseLoc	location;		/* token location, or -1 if unknown */
} ReturningOption;

/*
 * ReturningClause -
 *		List of RETURNING expressions, together with any WITH(...) options
 */
typedef struct ReturningClause
{
	NodeTag		type;
	List	   *options;		/* list of ReturningOption elements */
	List	   *exprs;			/* list of expressions to return */
} ReturningClause;

/*
 * TriggerTransition -
 *	   representation of transition row or table naming clause
 *
 * Only transition tables are initially supported in the syntax, and only for
 * AFTER triggers, but other permutations are accepted by the parser so we can
 * give a meaningful message from C code.
 */
typedef struct TriggerTransition
{
	NodeTag		type;
	char	   *name;
	bool		isNew;
	bool		isTable;
} TriggerTransition;

/* Nodes for SQL/JSON support */

/*
 * JsonOutput -
 *		representation of JSON output clause (RETURNING type [FORMAT format])
 */
typedef struct JsonOutput
{
	NodeTag		type;
	TypeName   *typeName;		/* RETURNING type name, if specified */
	JsonReturning *returning;	/* RETURNING FORMAT clause and type Oids */
} JsonOutput;

/*
 * JsonArgument -
 *		representation of argument from JSON PASSING clause
 */
typedef struct JsonArgument
{
	NodeTag		type;
	JsonValueExpr *val;			/* argument value expression */
	char	   *name;			/* argument name */
} JsonArgument;

/*
 * JsonQuotes -
 *		representation of [KEEP|OMIT] QUOTES clause for JSON_QUERY()
 */
typedef enum JsonQuotes
{
	JS_QUOTES_UNSPEC,			/* unspecified */
	JS_QUOTES_KEEP,				/* KEEP QUOTES */
	JS_QUOTES_OMIT,				/* OMIT QUOTES */
} JsonQuotes;

/*
 * JsonFuncExpr -
 *		untransformed representation of function expressions for
 *		SQL/JSON query functions
 */
typedef struct JsonFuncExpr
{
	NodeTag		type;
	JsonExprOp	op;				/* expression type */
	char	   *column_name;	/* JSON_TABLE() column name or NULL if this is
								 * not for a JSON_TABLE() */
	JsonValueExpr *context_item;	/* context item expression */
	Node	   *pathspec;		/* JSON path specification expression */
	List	   *passing;		/* list of PASSING clause arguments, if any */
	JsonOutput *output;			/* output clause, if specified */
	JsonBehavior *on_empty;		/* ON EMPTY behavior */
	JsonBehavior *on_error;		/* ON ERROR behavior */
	JsonWrapper wrapper;		/* array wrapper behavior (JSON_QUERY only) */
	JsonQuotes	quotes;			/* omit or keep quotes? (JSON_QUERY only) */
	ParseLoc	location;		/* token location, or -1 if unknown */
} JsonFuncExpr;

/*
 * JsonTablePathSpec
 *		untransformed specification of JSON path expression with an optional
 *		name
 */
typedef struct JsonTablePathSpec
{
	NodeTag		type;

	Node	   *string;
	char	   *name;
	ParseLoc	name_location;
	ParseLoc	location;		/* location of 'string' */
} JsonTablePathSpec;

/*
 * JsonTable -
 *		untransformed representation of JSON_TABLE
 */
typedef struct JsonTable
{
	NodeTag		type;
	JsonValueExpr *context_item;	/* context item expression */
	JsonTablePathSpec *pathspec;	/* JSON path specification */
	List	   *passing;		/* list of PASSING clause arguments, if any */
	List	   *columns;		/* list of JsonTableColumn */
	JsonBehavior *on_error;		/* ON ERROR behavior */
	Alias	   *alias;			/* table alias in FROM clause */
	bool		lateral;		/* does it have LATERAL prefix? */
	ParseLoc	location;		/* token location, or -1 if unknown */
} JsonTable;

/*
 * JsonTableColumnType -
 *		enumeration of JSON_TABLE column types
 */
typedef enum JsonTableColumnType
{
	JTC_FOR_ORDINALITY,
	JTC_REGULAR,
	JTC_EXISTS,
	JTC_FORMATTED,
	JTC_NESTED,
} JsonTableColumnType;

/*
 * JsonTableColumn -
 *		untransformed representation of JSON_TABLE column
 */
typedef struct JsonTableColumn
{
	NodeTag		type;
	JsonTableColumnType coltype;	/* column type */
	char	   *name;			/* column name */
	TypeName   *typeName;		/* column type name */
	JsonTablePathSpec *pathspec;	/* JSON path specification */
	JsonFormat *format;			/* JSON format clause, if specified */
	JsonWrapper wrapper;		/* WRAPPER behavior for formatted columns */
	JsonQuotes	quotes;			/* omit or keep quotes on scalar strings? */
	List	   *columns;		/* nested columns */
	JsonBehavior *on_empty;		/* ON EMPTY behavior */
	JsonBehavior *on_error;		/* ON ERROR behavior */
	ParseLoc	location;		/* token location, or -1 if unknown */
} JsonTableColumn;

/*
 * JsonKeyValue -
 *		untransformed representation of JSON object key-value pair for
 *		JSON_OBJECT() and JSON_OBJECTAGG()
 */
typedef struct JsonKeyValue
{
	NodeTag		type;
	Expr	   *key;			/* key expression */
	JsonValueExpr *value;		/* JSON value expression */
} JsonKeyValue;

/*
 * JsonParseExpr -
 *		untransformed representation of JSON()
 */
typedef struct JsonParseExpr
{
	NodeTag		type;
	JsonValueExpr *expr;		/* string expression */
	JsonOutput *output;			/* RETURNING clause, if specified */
	bool		unique_keys;	/* WITH UNIQUE KEYS? */
	ParseLoc	location;		/* token location, or -1 if unknown */
} JsonParseExpr;

/*
 * JsonScalarExpr -
 *		untransformed representation of JSON_SCALAR()
 */
typedef struct JsonScalarExpr
{
	NodeTag		type;
	Expr	   *expr;			/* scalar expression */
	JsonOutput *output;			/* RETURNING clause, if specified */
	ParseLoc	location;		/* token location, or -1 if unknown */
} JsonScalarExpr;

/*
 * JsonSerializeExpr -
 *		untransformed representation of JSON_SERIALIZE() function
 */
typedef struct JsonSerializeExpr
{
	NodeTag		type;
	JsonValueExpr *expr;		/* json value expression */
	JsonOutput *output;			/* RETURNING clause, if specified  */
	ParseLoc	location;		/* token location, or -1 if unknown */
} JsonSerializeExpr;

/*
 * JsonObjectConstructor -
 *		untransformed representation of JSON_OBJECT() constructor
 */
typedef struct JsonObjectConstructor
{
	NodeTag		type;
	List	   *exprs;			/* list of JsonKeyValue pairs */
	JsonOutput *output;			/* RETURNING clause, if specified  */
	bool		absent_on_null; /* skip NULL values? */
	bool		unique;			/* check key uniqueness? */
	ParseLoc	location;		/* token location, or -1 if unknown */
} JsonObjectConstructor;

/*
 * JsonArrayConstructor -
 *		untransformed representation of JSON_ARRAY(element,...) constructor
 */
typedef struct JsonArrayConstructor
{
	NodeTag		type;
	List	   *exprs;			/* list of JsonValueExpr elements */
	JsonOutput *output;			/* RETURNING clause, if specified  */
	bool		absent_on_null; /* skip NULL elements? */
	ParseLoc	location;		/* token location, or -1 if unknown */
} JsonArrayConstructor;

/*
 * JsonArrayQueryConstructor -
 *		untransformed representation of JSON_ARRAY(subquery) constructor
 */
typedef struct JsonArrayQueryConstructor
{
	NodeTag		type;
	Node	   *query;			/* subquery */
	JsonOutput *output;			/* RETURNING clause, if specified  */
	JsonFormat *format;			/* FORMAT clause for subquery, if specified */
	bool		absent_on_null; /* skip NULL elements? */
	ParseLoc	location;		/* token location, or -1 if unknown */
} JsonArrayQueryConstructor;

/*
 * JsonAggConstructor -
 *		common fields of untransformed representation of
 *		JSON_ARRAYAGG() and JSON_OBJECTAGG()
 */
typedef struct JsonAggConstructor
{
	NodeTag		type;
	JsonOutput *output;			/* RETURNING clause, if any */
	Node	   *agg_filter;		/* FILTER clause, if any */
	List	   *agg_order;		/* ORDER BY clause, if any */
	struct WindowDef *over;		/* OVER clause, if any */
	ParseLoc	location;		/* token location, or -1 if unknown */
} JsonAggConstructor;

/*
 * JsonObjectAgg -
 *		untransformed representation of JSON_OBJECTAGG()
 */
typedef struct JsonObjectAgg
{
	NodeTag		type;
	JsonAggConstructor *constructor;	/* common fields */
	JsonKeyValue *arg;			/* object key-value pair */
	bool		absent_on_null; /* skip NULL values? */
	bool		unique;			/* check key uniqueness? */
} JsonObjectAgg;

/*
 * JsonArrayAgg -
 *		untransformed representation of JSON_ARRAYAGG()
 */
typedef struct JsonArrayAgg
{
	NodeTag		type;
	JsonAggConstructor *constructor;	/* common fields */
	JsonValueExpr *arg;			/* array element expression */
	bool		absent_on_null; /* skip NULL elements? */
} JsonArrayAgg;


/*****************************************************************************
 *		Raw Grammar Output Statements
 *****************************************************************************/

/*
 *		RawStmt --- container for any one statement's raw parse tree
 *
 * Parse analysis converts a raw parse tree headed by a RawStmt node into
 * an analyzed statement headed by a Query node.  For optimizable statements,
 * the conversion is complex.  For utility statements, the parser usually just
 * transfers the raw parse tree (sans RawStmt) into the utilityStmt field of
 * the Query node, and all the useful work happens at execution time.
 *
 * stmt_location/stmt_len identify the portion of the source text string
 * containing this raw statement (useful for multi-statement strings).
 *
 * This is irrelevant for query jumbling, as this is not used in parsed
 * queries.
 */
typedef struct RawStmt
{
	pg_node_attr(no_query_jumble)

	NodeTag		type;
	Node	   *stmt;			/* raw parse tree */
	ParseLoc	stmt_location;	/* start location, or -1 if unknown */
	ParseLoc	stmt_len;		/* length in bytes; 0 means "rest of string" */
} RawStmt;

/*****************************************************************************
 *		Optimizable Statements
 *****************************************************************************/

/* ----------------------
 *		Insert Statement
 *
 * The source expression is represented by SelectStmt for both the
 * SELECT and VALUES cases.  If selectStmt is NULL, then the query
 * is INSERT ... DEFAULT VALUES.
 * ----------------------
 */
typedef struct InsertStmt
{
	NodeTag		type;
	RangeVar   *relation;		/* relation to insert into */
	List	   *cols;			/* optional: names of the target columns */
	Node	   *selectStmt;		/* the source SELECT/VALUES, or NULL */
	OnConflictClause *onConflictClause; /* ON CONFLICT clause */
	ReturningClause *returningClause;	/* RETURNING clause */
	WithClause *withClause;		/* WITH clause */
	OverridingKind override;	/* OVERRIDING clause */
} InsertStmt;

/* ----------------------
 *		Delete Statement
 * ----------------------
 */
typedef struct DeleteStmt
{
	NodeTag		type;
	RangeVar   *relation;		/* relation to delete from */
	List	   *usingClause;	/* optional using clause for more tables */
	Node	   *whereClause;	/* qualifications */
	ReturningClause *returningClause;	/* RETURNING clause */
	WithClause *withClause;		/* WITH clause */
} DeleteStmt;

/* ----------------------
 *		Update Statement
 * ----------------------
 */
typedef struct UpdateStmt
{
	NodeTag		type;
	RangeVar   *relation;		/* relation to update */
	List	   *targetList;		/* the target list (of ResTarget) */
	Node	   *whereClause;	/* qualifications */
	List	   *fromClause;		/* optional from clause for more tables */
	ReturningClause *returningClause;	/* RETURNING clause */
	WithClause *withClause;		/* WITH clause */
} UpdateStmt;

/* ----------------------
 *		Merge Statement
 * ----------------------
 */
typedef struct MergeStmt
{
	NodeTag		type;
	RangeVar   *relation;		/* target relation to merge into */
	Node	   *sourceRelation; /* source relation */
	Node	   *joinCondition;	/* join condition between source and target */
	List	   *mergeWhenClauses;	/* list of MergeWhenClause(es) */
	ReturningClause *returningClause;	/* RETURNING clause */
	WithClause *withClause;		/* WITH clause */
} MergeStmt;

/* ----------------------
 *		Select Statement
 *
 * A "simple" SELECT is represented in the output of gram.y by a single
 * SelectStmt node; so is a VALUES construct.  A query containing set
 * operators (UNION, INTERSECT, EXCEPT) is represented by a tree of SelectStmt
 * nodes, in which the leaf nodes are component SELECTs and the internal nodes
 * represent UNION, INTERSECT, or EXCEPT operators.  Using the same node
 * type for both leaf and internal nodes allows gram.y to stick ORDER BY,
 * LIMIT, etc, clause values into a SELECT statement without worrying
 * whether it is a simple or compound SELECT.
 * ----------------------
 */
typedef enum SetOperation
{
	SETOP_NONE = 0,
	SETOP_UNION,
	SETOP_INTERSECT,
	SETOP_EXCEPT,
} SetOperation;

typedef struct SelectStmt
{
	NodeTag		type;

	/*
	 * These fields are used only in "leaf" SelectStmts.
	 */
	List	   *distinctClause; /* NULL, list of DISTINCT ON exprs, or
								 * lcons(NIL,NIL) for all (SELECT DISTINCT) */
	IntoClause *intoClause;		/* target for SELECT INTO */
	List	   *targetList;		/* the target list (of ResTarget) */
	List	   *fromClause;		/* the FROM clause */
	Node	   *whereClause;	/* WHERE qualification */
	List	   *groupClause;	/* GROUP BY clauses */
	bool		groupDistinct;	/* Is this GROUP BY DISTINCT? */
	Node	   *havingClause;	/* HAVING conditional-expression */
	List	   *windowClause;	/* WINDOW window_name AS (...), ... */

	/*
	 * In a "leaf" node representing a VALUES list, the above fields are all
	 * null, and instead this field is set.  Note that the elements of the
	 * sublists are just expressions, without ResTarget decoration. Also note
	 * that a list element can be DEFAULT (represented as a SetToDefault
	 * node), regardless of the context of the VALUES list. It's up to parse
	 * analysis to reject that where not valid.
	 */
	List	   *valuesLists;	/* untransformed list of expression lists */

	/*
	 * These fields are used in both "leaf" SelectStmts and upper-level
	 * SelectStmts.
	 */
	List	   *sortClause;		/* sort clause (a list of SortBy's) */
	Node	   *limitOffset;	/* # of result tuples to skip */
	Node	   *limitCount;		/* # of result tuples to return */
	LimitOption limitOption;	/* limit type */
	List	   *lockingClause;	/* FOR UPDATE (list of LockingClause's) */
	WithClause *withClause;		/* WITH clause */

	/*
	 * These fields are used only in upper-level SelectStmts.
	 */
	SetOperation op;			/* type of set op */
	bool		all;			/* ALL specified? */
	struct SelectStmt *larg;	/* left child */
	struct SelectStmt *rarg;	/* right child */
	/* Eventually add fields for CORRESPONDING spec here */
} SelectStmt;


/* ----------------------
 *		Set Operation node for post-analysis query trees
 *
 * After parse analysis, a SELECT with set operations is represented by a
 * top-level Query node containing the leaf SELECTs as subqueries in its
 * range table.  Its setOperations field shows the tree of set operations,
 * with leaf SelectStmt nodes replaced by RangeTblRef nodes, and internal
 * nodes replaced by SetOperationStmt nodes.  Information about the output
 * column types is added, too.  (Note that the child nodes do not necessarily
 * produce these types directly, but we've checked that their output types
 * can be coerced to the output column type.)  Also, if it's not UNION ALL,
 * information about the types' sort/group semantics is provided in the form
 * of a SortGroupClause list (same representation as, eg, DISTINCT).
 * The resolved common column collations are provided too; but note that if
 * it's not UNION ALL, it's okay for a column to not have a common collation,
 * so a member of the colCollations list could be InvalidOid even though the
 * column has a collatable type.
 * ----------------------
 */
typedef struct SetOperationStmt
{
	NodeTag		type;
	SetOperation op;			/* type of set op */
	bool		all;			/* ALL specified? */
	Node	   *larg;			/* left child */
	Node	   *rarg;			/* right child */
	/* Eventually add fields for CORRESPONDING spec here */

	/* Fields derived during parse analysis (irrelevant for query jumbling): */
	/* OID list of output column type OIDs */
	List	   *colTypes pg_node_attr(query_jumble_ignore);
	/* integer list of output column typmods */
	List	   *colTypmods pg_node_attr(query_jumble_ignore);
	/* OID list of output column collation OIDs */
	List	   *colCollations pg_node_attr(query_jumble_ignore);
	/* a list of SortGroupClause's */
	List	   *groupClauses pg_node_attr(query_jumble_ignore);
	/* groupClauses is NIL if UNION ALL, but must be set otherwise */
} SetOperationStmt;


/*
 * RETURN statement (inside SQL function body)
 */
typedef struct ReturnStmt
{
	NodeTag		type;
	Node	   *returnval;
} ReturnStmt;


/* ----------------------
 *		PL/pgSQL Assignment Statement
 *
 * Like SelectStmt, this is transformed into a SELECT Query.
 * However, the targetlist of the result looks more like an UPDATE.
 * ----------------------
 */
typedef struct PLAssignStmt
{
	NodeTag		type;

	char	   *name;			/* initial column name */
	List	   *indirection;	/* subscripts and field names, if any */
	int			nnames;			/* number of names to use in ColumnRef */
	SelectStmt *val;			/* the PL/pgSQL expression to assign */
	ParseLoc	location;		/* name's token location, or -1 if unknown */
} PLAssignStmt;


/*****************************************************************************
 *		Other Statements (no optimizations required)
 *
 *		These are not touched by parser/analyze.c except to put them into
 *		the utilityStmt field of a Query.  This is eventually passed to
 *		ProcessUtility (by-passing rewriting and planning).  Some of the
 *		statements do need attention from parse analysis, and this is
 *		done by routines in parser/parse_utilcmd.c after ProcessUtility
 *		receives the command for execution.
 *		DECLARE CURSOR, EXPLAIN, and CREATE TABLE AS are special cases:
 *		they contain optimizable statements, which get processed normally
 *		by parser/analyze.c.
 *****************************************************************************/

/*
 * When a command can act on several kinds of objects with only one
 * parse structure required, use these constants to designate the
 * object type.  Note that commands typically don't support all the types.
 */

typedef enum ObjectType
{
	OBJECT_ACCESS_METHOD,
	OBJECT_AGGREGATE,
	OBJECT_AMOP,
	OBJECT_AMPROC,
	OBJECT_ATTRIBUTE,			/* type's attribute, when distinct from column */
	OBJECT_CAST,
	OBJECT_COLUMN,
	OBJECT_COLLATION,
	OBJECT_CONVERSION,
	OBJECT_DATABASE,
	OBJECT_DEFAULT,
	OBJECT_DEFACL,
	OBJECT_DOMAIN,
	OBJECT_DOMCONSTRAINT,
	OBJECT_EVENT_TRIGGER,
	OBJECT_EXTENSION,
	OBJECT_FDW,
	OBJECT_FOREIGN_SERVER,
	OBJECT_FOREIGN_TABLE,
	OBJECT_FUNCTION,
	OBJECT_INDEX,
	OBJECT_LANGUAGE,
	OBJECT_LARGEOBJECT,
	OBJECT_MATVIEW,
	OBJECT_OPCLASS,
	OBJECT_OPERATOR,
	OBJECT_OPFAMILY,
	OBJECT_PARAMETER_ACL,
	OBJECT_POLICY,
	OBJECT_PROCEDURE,
	OBJECT_PUBLICATION,
	OBJECT_PUBLICATION_NAMESPACE,
	OBJECT_PUBLICATION_REL,
	OBJECT_ROLE,
	OBJECT_ROUTINE,
	OBJECT_RULE,
	OBJECT_SCHEMA,
	OBJECT_SEQUENCE,
	OBJECT_SUBSCRIPTION,
	OBJECT_STATISTIC_EXT,
	OBJECT_TABCONSTRAINT,
	OBJECT_TABLE,
	OBJECT_TABLESPACE,
	OBJECT_TRANSFORM,
	OBJECT_TRIGGER,
	OBJECT_TSCONFIGURATION,
	OBJECT_TSDICTIONARY,
	OBJECT_TSPARSER,
	OBJECT_TSTEMPLATE,
	OBJECT_TYPE,
	OBJECT_USER_MAPPING,
	OBJECT_VIEW,
} ObjectType;

/* ----------------------
 *		Create Schema Statement
 *
 * NOTE: the schemaElts list contains raw parsetrees for component statements
 * of the schema, such as CREATE TABLE, GRANT, etc.  These are analyzed and
 * executed after the schema itself is created.
 * ----------------------
 */
typedef struct CreateSchemaStmt
{
	NodeTag		type;
	char	   *schemaname;		/* the name of the schema to create */
	RoleSpec   *authrole;		/* the owner of the created schema */
	List	   *schemaElts;		/* schema components (list of parsenodes) */
	bool		if_not_exists;	/* just do nothing if schema already exists? */
} CreateSchemaStmt;

typedef enum DropBehavior
{
	DROP_RESTRICT,				/* drop fails if any dependent objects */
	DROP_CASCADE,				/* remove dependent objects too */
} DropBehavior;

/* ----------------------
 *	Alter Table
 * ----------------------
 */
typedef struct AlterTableStmt
{
	NodeTag		type;
	RangeVar   *relation;		/* table to work on */
	List	   *cmds;			/* list of subcommands */
	ObjectType	objtype;		/* type of object */
	bool		missing_ok;		/* skip error if table missing */
} AlterTableStmt;

typedef enum AlterTableType
{
	AT_AddColumn,				/* add column */
	AT_AddColumnToView,			/* implicitly via CREATE OR REPLACE VIEW */
	AT_ColumnDefault,			/* alter column default */
	AT_CookedColumnDefault,		/* add a pre-cooked column default */
	AT_DropNotNull,				/* alter column drop not null */
	AT_SetNotNull,				/* alter column set not null */
	AT_SetExpression,			/* alter column set expression */
	AT_DropExpression,			/* alter column drop expression */
	AT_SetStatistics,			/* alter column set statistics */
	AT_SetOptions,				/* alter column set ( options ) */
	AT_ResetOptions,			/* alter column reset ( options ) */
	AT_SetStorage,				/* alter column set storage */
	AT_SetCompression,			/* alter column set compression */
	AT_DropColumn,				/* drop column */
	AT_AddIndex,				/* add index */
	AT_ReAddIndex,				/* internal to commands/tablecmds.c */
	AT_AddConstraint,			/* add constraint */
	AT_ReAddConstraint,			/* internal to commands/tablecmds.c */
	AT_ReAddDomainConstraint,	/* internal to commands/tablecmds.c */
	AT_AlterConstraint,			/* alter constraint */
	AT_ValidateConstraint,		/* validate constraint */
	AT_AddIndexConstraint,		/* add constraint using existing index */
	AT_DropConstraint,			/* drop constraint */
	AT_ReAddComment,			/* internal to commands/tablecmds.c */
	AT_AlterColumnType,			/* alter column type */
	AT_AlterColumnGenericOptions,	/* alter column OPTIONS (...) */
	AT_ChangeOwner,				/* change owner */
	AT_ClusterOn,				/* CLUSTER ON */
	AT_DropCluster,				/* SET WITHOUT CLUSTER */
	AT_SetLogged,				/* SET LOGGED */
	AT_SetUnLogged,				/* SET UNLOGGED */
	AT_DropOids,				/* SET WITHOUT OIDS */
	AT_SetAccessMethod,			/* SET ACCESS METHOD */
	AT_SetTableSpace,			/* SET TABLESPACE */
	AT_SetRelOptions,			/* SET (...) -- AM specific parameters */
	AT_ResetRelOptions,			/* RESET (...) -- AM specific parameters */
	AT_ReplaceRelOptions,		/* replace reloption list in its entirety */
	AT_EnableTrig,				/* ENABLE TRIGGER name */
	AT_EnableAlwaysTrig,		/* ENABLE ALWAYS TRIGGER name */
	AT_EnableReplicaTrig,		/* ENABLE REPLICA TRIGGER name */
	AT_DisableTrig,				/* DISABLE TRIGGER name */
	AT_EnableTrigAll,			/* ENABLE TRIGGER ALL */
	AT_DisableTrigAll,			/* DISABLE TRIGGER ALL */
	AT_EnableTrigUser,			/* ENABLE TRIGGER USER */
	AT_DisableTrigUser,			/* DISABLE TRIGGER USER */
	AT_EnableRule,				/* ENABLE RULE name */
	AT_EnableAlwaysRule,		/* ENABLE ALWAYS RULE name */
	AT_EnableReplicaRule,		/* ENABLE REPLICA RULE name */
	AT_DisableRule,				/* DISABLE RULE name */
	AT_AddInherit,				/* INHERIT parent */
	AT_DropInherit,				/* NO INHERIT parent */
	AT_AddOf,					/* OF <type_name> */
	AT_DropOf,					/* NOT OF */
	AT_ReplicaIdentity,			/* REPLICA IDENTITY */
	AT_EnableRowSecurity,		/* ENABLE ROW SECURITY */
	AT_DisableRowSecurity,		/* DISABLE ROW SECURITY */
	AT_ForceRowSecurity,		/* FORCE ROW SECURITY */
	AT_NoForceRowSecurity,		/* NO FORCE ROW SECURITY */
	AT_GenericOptions,			/* OPTIONS (...) */
	AT_AttachPartition,			/* ATTACH PARTITION */
	AT_DetachPartition,			/* DETACH PARTITION */
	AT_DetachPartitionFinalize, /* DETACH PARTITION FINALIZE */
	AT_AddIdentity,				/* ADD IDENTITY */
	AT_SetIdentity,				/* SET identity column options */
	AT_DropIdentity,			/* DROP IDENTITY */
	AT_ReAddStatistics,			/* internal to commands/tablecmds.c */
} AlterTableType;

typedef struct AlterTableCmd	/* one subcommand of an ALTER TABLE */
{
	NodeTag		type;
	AlterTableType subtype;		/* Type of table alteration to apply */
	char	   *name;			/* column, constraint, or trigger to act on,
								 * or tablespace, access method */
	int16		num;			/* attribute number for columns referenced by
								 * number */
	RoleSpec   *newowner;
	Node	   *def;			/* definition of new column, index,
								 * constraint, or parent table */
	DropBehavior behavior;		/* RESTRICT or CASCADE for DROP cases */
	bool		missing_ok;		/* skip error if missing? */
	bool		recurse;		/* exec-time recursion */
} AlterTableCmd;

/* Ad-hoc node for AT_AlterConstraint */
typedef struct ATAlterConstraint
{
	NodeTag		type;
	char	   *conname;		/* Constraint name */
	bool		alterEnforceability;	/* changing enforceability properties? */
	bool		is_enforced;	/* ENFORCED? */
	bool		alterDeferrability; /* changing deferrability properties? */
	bool		deferrable;		/* DEFERRABLE? */
	bool		initdeferred;	/* INITIALLY DEFERRED? */
	bool		alterInheritability;	/* changing inheritability properties */
	bool		noinherit;
} ATAlterConstraint;

/* Ad-hoc node for AT_ReplicaIdentity */
typedef struct ReplicaIdentityStmt
{
	NodeTag		type;
	char		identity_type;
	char	   *name;
} ReplicaIdentityStmt;


/* ----------------------
 * Alter Collation
 * ----------------------
 */
typedef struct AlterCollationStmt
{
	NodeTag		type;
	List	   *collname;
} AlterCollationStmt;


/* ----------------------
 *	Alter Domain
 *
 * The fields are used in different ways by the different variants of
 * this command.
 * ----------------------
 */
typedef enum AlterDomainType
{
	AD_AlterDefault = 'T',		/* SET|DROP DEFAULT */
	AD_DropNotNull = 'N',		/* DROP NOT NULL */
	AD_SetNotNull = 'O',		/* SET NOT NULL */
	AD_AddConstraint = 'C',		/* ADD CONSTRAINT */
	AD_DropConstraint = 'X',	/* DROP CONSTRAINT */
	AD_ValidateConstraint = 'V',	/* VALIDATE CONSTRAINT */
} AlterDomainType;

typedef struct AlterDomainStmt
{
	NodeTag		type;
	AlterDomainType subtype;	/* subtype of command */
	List	   *typeName;		/* domain to work on */
	char	   *name;			/* column or constraint name to act on */
	Node	   *def;			/* definition of default or constraint */
	DropBehavior behavior;		/* RESTRICT or CASCADE for DROP cases */
	bool		missing_ok;		/* skip error if missing? */
} AlterDomainStmt;


/* ----------------------
 *		Grant|Revoke Statement
 * ----------------------
 */
typedef enum GrantTargetType
{
	ACL_TARGET_OBJECT,			/* grant on specific named object(s) */
	ACL_TARGET_ALL_IN_SCHEMA,	/* grant on all objects in given schema(s) */
	ACL_TARGET_DEFAULTS,		/* ALTER DEFAULT PRIVILEGES */
} GrantTargetType;

typedef struct GrantStmt
{
	NodeTag		type;
	bool		is_grant;		/* true = GRANT, false = REVOKE */
	GrantTargetType targtype;	/* type of the grant target */
	ObjectType	objtype;		/* kind of object being operated on */
	List	   *objects;		/* list of RangeVar nodes, ObjectWithArgs
								 * nodes, or plain names (as String values) */
	List	   *privileges;		/* list of AccessPriv nodes */
	/* privileges == NIL denotes ALL PRIVILEGES */
	List	   *grantees;		/* list of RoleSpec nodes */
	bool		grant_option;	/* grant or revoke grant option */
	RoleSpec   *grantor;
	DropBehavior behavior;		/* drop behavior (for REVOKE) */
} GrantStmt;

/*
 * ObjectWithArgs represents a function/procedure/operator name plus parameter
 * identification.
 *
 * objargs includes only the types of the input parameters of the object.
 * In some contexts, that will be all we have, and it's enough to look up
 * objects according to the traditional Postgres rules (i.e., when only input
 * arguments matter).
 *
 * objfuncargs, if not NIL, carries the full specification of the parameter
 * list, including parameter mode annotations.
 *
 * Some grammar productions can set args_unspecified = true instead of
 * providing parameter info.  In this case, lookup will succeed only if
 * the object name is unique.  Note that otherwise, NIL parameter lists
 * mean zero arguments.
 */
typedef struct ObjectWithArgs
{
	NodeTag		type;
	List	   *objname;		/* qualified name of function/operator */
	List	   *objargs;		/* list of Typename nodes (input args only) */
	List	   *objfuncargs;	/* list of FunctionParameter nodes */
	bool		args_unspecified;	/* argument list was omitted? */
} ObjectWithArgs;

/*
 * An access privilege, with optional list of column names
 * priv_name == NULL denotes ALL PRIVILEGES (only used with a column list)
 * cols == NIL denotes "all columns"
 * Note that simple "ALL PRIVILEGES" is represented as a NIL list, not
 * an AccessPriv with both fields null.
 */
typedef struct AccessPriv
{
	NodeTag		type;
	char	   *priv_name;		/* string name of privilege */
	List	   *cols;			/* list of String */
} AccessPriv;

/* ----------------------
 *		Grant/Revoke Role Statement
 *
 * Note: because of the parsing ambiguity with the GRANT <privileges>
 * statement, granted_roles is a list of AccessPriv; the execution code
 * should complain if any column lists appear.  grantee_roles is a list
 * of role names, as String values.
 * ----------------------
 */
typedef struct GrantRoleStmt
{
	NodeTag		type;
	List	   *granted_roles;	/* list of roles to be granted/revoked */
	List	   *grantee_roles;	/* list of member roles to add/delete */
	bool		is_grant;		/* true = GRANT, false = REVOKE */
	List	   *opt;			/* options e.g. WITH GRANT OPTION */
	RoleSpec   *grantor;		/* set grantor to other than current role */
	DropBehavior behavior;		/* drop behavior (for REVOKE) */
} GrantRoleStmt;

/* ----------------------
 *	Alter Default Privileges Statement
 * ----------------------
 */
typedef struct AlterDefaultPrivilegesStmt
{
	NodeTag		type;
	List	   *options;		/* list of DefElem */
	GrantStmt  *action;			/* GRANT/REVOKE action (with objects=NIL) */
} AlterDefaultPrivilegesStmt;

/* ----------------------
 *		Copy Statement
 *
 * We support "COPY relation FROM file", "COPY relation TO file", and
 * "COPY (query) TO file".  In any given CopyStmt, exactly one of "relation"
 * and "query" must be non-NULL.
 * ----------------------
 */
typedef struct CopyStmt
{
	NodeTag		type;
	RangeVar   *relation;		/* the relation to copy */
	Node	   *query;			/* the query (SELECT or DML statement with
								 * RETURNING) to copy, as a raw parse tree */
	List	   *attlist;		/* List of column names (as Strings), or NIL
								 * for all columns */
	bool		is_from;		/* TO or FROM */
	bool		is_program;		/* is 'filename' a program to popen? */
	char	   *filename;		/* filename, or NULL for STDIN/STDOUT */
	List	   *options;		/* List of DefElem nodes */
	Node	   *whereClause;	/* WHERE condition (or NULL) */
} CopyStmt;

/* ----------------------
 * SET Statement (includes RESET)
 *
 * "SET var TO DEFAULT" and "RESET var" are semantically equivalent, but we
 * preserve the distinction in VariableSetKind for CreateCommandTag().
 * ----------------------
 */
typedef enum VariableSetKind
{
	VAR_SET_VALUE,				/* SET var = value */
	VAR_SET_DEFAULT,			/* SET var TO DEFAULT */
	VAR_SET_CURRENT,			/* SET var FROM CURRENT */
	VAR_SET_MULTI,				/* special case for SET TRANSACTION ... */
	VAR_RESET,					/* RESET var */
	VAR_RESET_ALL,				/* RESET ALL */
} VariableSetKind;

typedef struct VariableSetStmt
{
	pg_node_attr(custom_query_jumble)

	NodeTag		type;
	VariableSetKind kind;
	/* variable to be set */
	char	   *name;
	/* List of A_Const nodes */
	List	   *args;

	/*
	 * True if arguments should be accounted for in query jumbling.  We use a
	 * separate flag rather than query_jumble_ignore on "args" as several
	 * grammar flavors of SET rely on a list of values that are parsed
	 * directly from the grammar's keywords.
	 */
	bool		jumble_args;
	/* SET LOCAL? */
	bool		is_local;
	/* token location, or -1 if unknown */
	ParseLoc	location pg_node_attr(query_jumble_location);
} VariableSetStmt;

/* ----------------------
 * Show Statement
 * ----------------------
 */
typedef struct VariableShowStmt
{
	NodeTag		type;
	char	   *name;
} VariableShowStmt;

/* ----------------------
 *		Create Table Statement
 *
 * NOTE: in the raw gram.y output, ColumnDef and Constraint nodes are
 * intermixed in tableElts, and constraints and nnconstraints are NIL.  After
 * parse analysis, tableElts contains just ColumnDefs, nnconstraints contains
 * Constraint nodes of CONSTR_NOTNULL type from various sources, and
 * constraints contains just CONSTR_CHECK Constraint nodes.
 * ----------------------
 */

typedef struct CreateStmt
{
	NodeTag		type;
	RangeVar   *relation;		/* relation to create */
	List	   *tableElts;		/* column definitions (list of ColumnDef) */
	List	   *inhRelations;	/* relations to inherit from (list of
								 * RangeVar) */
	PartitionBoundSpec *partbound;	/* FOR VALUES clause */
	PartitionSpec *partspec;	/* PARTITION BY clause */
	TypeName   *ofTypename;		/* OF typename */
	List	   *constraints;	/* constraints (list of Constraint nodes) */
	List	   *nnconstraints;	/* NOT NULL constraints (ditto) */
	List	   *options;		/* options from WITH clause */
	OnCommitAction oncommit;	/* what do we do at COMMIT? */
	char	   *tablespacename; /* table space to use, or NULL */
	char	   *accessMethod;	/* table access method */
	bool		if_not_exists;	/* just do nothing if it already exists? */
} CreateStmt;

/* ----------
 * Definitions for constraints in CreateStmt
 *
 * Note that column defaults are treated as a type of constraint,
 * even though that's a bit odd semantically.
 *
 * For constraints that use expressions (CONSTR_CHECK, CONSTR_DEFAULT)
 * we may have the expression in either "raw" form (an untransformed
 * parse tree) or "cooked" form (the nodeToString representation of
 * an executable expression tree), depending on how this Constraint
 * node was created (by parsing, or by inheritance from an existing
 * relation).  We should never have both in the same node!
 *
 * FKCONSTR_ACTION_xxx values are stored into pg_constraint.confupdtype
 * and pg_constraint.confdeltype columns; FKCONSTR_MATCH_xxx values are
 * stored into pg_constraint.confmatchtype.  Changing the code values may
 * require an initdb!
 *
 * If skip_validation is true then we skip checking that the existing rows
 * in the table satisfy the constraint, and just install the catalog entries
 * for the constraint.  A new FK constraint is marked as valid iff
 * initially_valid is true.  (Usually skip_validation and initially_valid
 * are inverses, but we can set both true if the table is known empty.)
 *
 * Constraint attributes (DEFERRABLE etc) are initially represented as
 * separate Constraint nodes for simplicity of parsing.  parse_utilcmd.c makes
 * a pass through the constraints list to insert the info into the appropriate
 * Constraint node.
 * ----------
 */

typedef enum ConstrType			/* types of constraints */
{
	CONSTR_NULL,				/* not standard SQL, but a lot of people
								 * expect it */
	CONSTR_NOTNULL,
	CONSTR_DEFAULT,
	CONSTR_IDENTITY,
	CONSTR_GENERATED,
	CONSTR_CHECK,
	CONSTR_PRIMARY,
	CONSTR_UNIQUE,
	CONSTR_EXCLUSION,
	CONSTR_FOREIGN,
	CONSTR_ATTR_DEFERRABLE,		/* attributes for previous constraint node */
	CONSTR_ATTR_NOT_DEFERRABLE,
	CONSTR_ATTR_DEFERRED,
	CONSTR_ATTR_IMMEDIATE,
	CONSTR_ATTR_ENFORCED,
	CONSTR_ATTR_NOT_ENFORCED,
} ConstrType;

/* Foreign key action codes */
#define FKCONSTR_ACTION_NOACTION	'a'
#define FKCONSTR_ACTION_RESTRICT	'r'
#define FKCONSTR_ACTION_CASCADE		'c'
#define FKCONSTR_ACTION_SETNULL		'n'
#define FKCONSTR_ACTION_SETDEFAULT	'd'

/* Foreign key matchtype codes */
#define FKCONSTR_MATCH_FULL			'f'
#define FKCONSTR_MATCH_PARTIAL		'p'
#define FKCONSTR_MATCH_SIMPLE		's'

typedef struct Constraint
{
	NodeTag		type;
	ConstrType	contype;		/* see above */
	char	   *conname;		/* Constraint name, or NULL if unnamed */
	bool		deferrable;		/* DEFERRABLE? */
	bool		initdeferred;	/* INITIALLY DEFERRED? */
	bool		is_enforced;	/* enforced constraint? */
	bool		skip_validation;	/* skip validation of existing rows? */
	bool		initially_valid;	/* mark the new constraint as valid? */
	bool		is_no_inherit;	/* is constraint non-inheritable? */
	Node	   *raw_expr;		/* CHECK or DEFAULT expression, as
								 * untransformed parse tree */
	char	   *cooked_expr;	/* CHECK or DEFAULT expression, as
								 * nodeToString representation */
	char		generated_when; /* ALWAYS or BY DEFAULT */
	char		generated_kind; /* STORED or VIRTUAL */
	bool		nulls_not_distinct; /* null treatment for UNIQUE constraints */
	List	   *keys;			/* String nodes naming referenced key
								 * column(s); for UNIQUE/PK/NOT NULL */
	bool		without_overlaps;	/* WITHOUT OVERLAPS specified */
	List	   *including;		/* String nodes naming referenced nonkey
								 * column(s); for UNIQUE/PK */
	List	   *exclusions;		/* list of (IndexElem, operator name) pairs;
								 * for exclusion constraints */
	List	   *options;		/* options from WITH clause */
	char	   *indexname;		/* existing index to use; otherwise NULL */
	char	   *indexspace;		/* index tablespace; NULL for default */
	bool		reset_default_tblspc;	/* reset default_tablespace prior to
										 * creating the index */
	char	   *access_method;	/* index access method; NULL for default */
	Node	   *where_clause;	/* partial index predicate */

	/* Fields used for FOREIGN KEY constraints: */
	RangeVar   *pktable;		/* Primary key table */
	List	   *fk_attrs;		/* Attributes of foreign key */
	List	   *pk_attrs;		/* Corresponding attrs in PK table */
	bool		fk_with_period; /* Last attribute of FK uses PERIOD */
	bool		pk_with_period; /* Last attribute of PK uses PERIOD */
	char		fk_matchtype;	/* FULL, PARTIAL, SIMPLE */
	char		fk_upd_action;	/* ON UPDATE action */
	char		fk_del_action;	/* ON DELETE action */
	List	   *fk_del_set_cols;	/* ON DELETE SET NULL/DEFAULT (col1, col2) */
	List	   *old_conpfeqop;	/* pg_constraint.conpfeqop of my former self */
	Oid			old_pktable_oid;	/* pg_constraint.confrelid of my former
									 * self */

	ParseLoc	location;		/* token location, or -1 if unknown */
} Constraint;

/* ----------------------
 *		Create/Drop Table Space Statements
 * ----------------------
 */

typedef struct CreateTableSpaceStmt
{
	NodeTag		type;
	char	   *tablespacename;
	RoleSpec   *owner;
	char	   *location;
	List	   *options;
} CreateTableSpaceStmt;

typedef struct DropTableSpaceStmt
{
	NodeTag		type;
	char	   *tablespacename;
	bool		missing_ok;		/* skip error if missing? */
} DropTableSpaceStmt;

typedef struct AlterTableSpaceOptionsStmt
{
	NodeTag		type;
	char	   *tablespacename;
	List	   *options;
	bool		isReset;
} AlterTableSpaceOptionsStmt;

typedef struct AlterTableMoveAllStmt
{
	NodeTag		type;
	char	   *orig_tablespacename;
	ObjectType	objtype;		/* Object type to move */
	List	   *roles;			/* List of roles to move objects of */
	char	   *new_tablespacename;
	bool		nowait;
} AlterTableMoveAllStmt;

/* ----------------------
 *		Create/Alter Extension Statements
 * ----------------------
 */

typedef struct CreateExtensionStmt
{
	NodeTag		type;
	char	   *extname;
	bool		if_not_exists;	/* just do nothing if it already exists? */
	List	   *options;		/* List of DefElem nodes */
} CreateExtensionStmt;

/* Only used for ALTER EXTENSION UPDATE; later might need an action field */
typedef struct AlterExtensionStmt
{
	NodeTag		type;
	char	   *extname;
	List	   *options;		/* List of DefElem nodes */
} AlterExtensionStmt;

typedef struct AlterExtensionContentsStmt
{
	NodeTag		type;
	char	   *extname;		/* Extension's name */
	int			action;			/* +1 = add object, -1 = drop object */
	ObjectType	objtype;		/* Object's type */
	Node	   *object;			/* Qualified name of the object */
} AlterExtensionContentsStmt;

/* ----------------------
 *		Create/Alter FOREIGN DATA WRAPPER Statements
 * ----------------------
 */

typedef struct CreateFdwStmt
{
	NodeTag		type;
	char	   *fdwname;		/* foreign-data wrapper name */
	List	   *func_options;	/* HANDLER/VALIDATOR options */
	List	   *options;		/* generic options to FDW */
} CreateFdwStmt;

typedef struct AlterFdwStmt
{
	NodeTag		type;
	char	   *fdwname;		/* foreign-data wrapper name */
	List	   *func_options;	/* HANDLER/VALIDATOR options */
	List	   *options;		/* generic options to FDW */
} AlterFdwStmt;

/* ----------------------
 *		Create/Alter FOREIGN SERVER Statements
 * ----------------------
 */

typedef struct CreateForeignServerStmt
{
	NodeTag		type;
	char	   *servername;		/* server name */
	char	   *servertype;		/* optional server type */
	char	   *version;		/* optional server version */
	char	   *fdwname;		/* FDW name */
	bool		if_not_exists;	/* just do nothing if it already exists? */
	List	   *options;		/* generic options to server */
} CreateForeignServerStmt;

typedef struct AlterForeignServerStmt
{
	NodeTag		type;
	char	   *servername;		/* server name */
	char	   *version;		/* optional server version */
	List	   *options;		/* generic options to server */
	bool		has_version;	/* version specified */
} AlterForeignServerStmt;

/* ----------------------
 *		Create FOREIGN TABLE Statement
 * ----------------------
 */

typedef struct CreateForeignTableStmt
{
	CreateStmt	base;
	char	   *servername;
	List	   *options;
} CreateForeignTableStmt;

/* ----------------------
 *		Create/Drop USER MAPPING Statements
 * ----------------------
 */

typedef struct CreateUserMappingStmt
{
	NodeTag		type;
	RoleSpec   *user;			/* user role */
	char	   *servername;		/* server name */
	bool		if_not_exists;	/* just do nothing if it already exists? */
	List	   *options;		/* generic options to server */
} CreateUserMappingStmt;

typedef struct AlterUserMappingStmt
{
	NodeTag		type;
	RoleSpec   *user;			/* user role */
	char	   *servername;		/* server name */
	List	   *options;		/* generic options to server */
} AlterUserMappingStmt;

typedef struct DropUserMappingStmt
{
	NodeTag		type;
	RoleSpec   *user;			/* user role */
	char	   *servername;		/* server name */
	bool		missing_ok;		/* ignore missing mappings */
} DropUserMappingStmt;

/* ----------------------
 *		Import Foreign Schema Statement
 * ----------------------
 */

typedef enum ImportForeignSchemaType
{
	FDW_IMPORT_SCHEMA_ALL,		/* all relations wanted */
	FDW_IMPORT_SCHEMA_LIMIT_TO, /* include only listed tables in import */
	FDW_IMPORT_SCHEMA_EXCEPT,	/* exclude listed tables from import */
} ImportForeignSchemaType;

typedef struct ImportForeignSchemaStmt
{
	NodeTag		type;
	char	   *server_name;	/* FDW server name */
	char	   *remote_schema;	/* remote schema name to query */
	char	   *local_schema;	/* local schema to create objects in */
	ImportForeignSchemaType list_type;	/* type of table list */
	List	   *table_list;		/* List of RangeVar */
	List	   *options;		/* list of options to pass to FDW */
} ImportForeignSchemaStmt;

/*----------------------
 *		Create POLICY Statement
 *----------------------
 */
typedef struct CreatePolicyStmt
{
	NodeTag		type;
	char	   *policy_name;	/* Policy's name */
	RangeVar   *table;			/* the table name the policy applies to */
	char	   *cmd_name;		/* the command name the policy applies to */
	bool		permissive;		/* restrictive or permissive policy */
	List	   *roles;			/* the roles associated with the policy */
	Node	   *qual;			/* the policy's condition */
	Node	   *with_check;		/* the policy's WITH CHECK condition. */
} CreatePolicyStmt;

/*----------------------
 *		Alter POLICY Statement
 *----------------------
 */
typedef struct AlterPolicyStmt
{
	NodeTag		type;
	char	   *policy_name;	/* Policy's name */
	RangeVar   *table;			/* the table name the policy applies to */
	List	   *roles;			/* the roles associated with the policy */
	Node	   *qual;			/* the policy's condition */
	Node	   *with_check;		/* the policy's WITH CHECK condition. */
} AlterPolicyStmt;

/*----------------------
 *		Create ACCESS METHOD Statement
 *----------------------
 */
typedef struct CreateAmStmt
{
	NodeTag		type;
	char	   *amname;			/* access method name */
	List	   *handler_name;	/* handler function name */
	char		amtype;			/* type of access method */
} CreateAmStmt;

/* ----------------------
 *		Create TRIGGER Statement
 * ----------------------
 */
typedef struct CreateTrigStmt
{
	NodeTag		type;
	bool		replace;		/* replace trigger if already exists */
	bool		isconstraint;	/* This is a constraint trigger */
	char	   *trigname;		/* TRIGGER's name */
	RangeVar   *relation;		/* relation trigger is on */
	List	   *funcname;		/* qual. name of function to call */
	List	   *args;			/* list of String or NIL */
	bool		row;			/* ROW/STATEMENT */
	/* timing uses the TRIGGER_TYPE bits defined in catalog/pg_trigger.h */
	int16		timing;			/* BEFORE, AFTER, or INSTEAD */
	/* events uses the TRIGGER_TYPE bits defined in catalog/pg_trigger.h */
	int16		events;			/* "OR" of INSERT/UPDATE/DELETE/TRUNCATE */
	List	   *columns;		/* column names, or NIL for all columns */
	Node	   *whenClause;		/* qual expression, or NULL if none */
	/* explicitly named transition data */
	List	   *transitionRels; /* TriggerTransition nodes, or NIL if none */
	/* The remaining fields are only used for constraint triggers */
	bool		deferrable;		/* [NOT] DEFERRABLE */
	bool		initdeferred;	/* INITIALLY {DEFERRED|IMMEDIATE} */
	RangeVar   *constrrel;		/* opposite relation, if RI trigger */
} CreateTrigStmt;

/* ----------------------
 *		Create EVENT TRIGGER Statement
 * ----------------------
 */
typedef struct CreateEventTrigStmt
{
	NodeTag		type;
	char	   *trigname;		/* TRIGGER's name */
	char	   *eventname;		/* event's identifier */
	List	   *whenclause;		/* list of DefElems indicating filtering */
	List	   *funcname;		/* qual. name of function to call */
} CreateEventTrigStmt;

/* ----------------------
 *		Alter EVENT TRIGGER Statement
 * ----------------------
 */
typedef struct AlterEventTrigStmt
{
	NodeTag		type;
	char	   *trigname;		/* TRIGGER's name */
	char		tgenabled;		/* trigger's firing configuration WRT
								 * session_replication_role */
} AlterEventTrigStmt;

/* ----------------------
 *		Create LANGUAGE Statements
 * ----------------------
 */
typedef struct CreatePLangStmt
{
	NodeTag		type;
	bool		replace;		/* T => replace if already exists */
	char	   *plname;			/* PL name */
	List	   *plhandler;		/* PL call handler function (qual. name) */
	List	   *plinline;		/* optional inline function (qual. name) */
	List	   *plvalidator;	/* optional validator function (qual. name) */
	bool		pltrusted;		/* PL is trusted */
} CreatePLangStmt;

/* ----------------------
 *	Create/Alter/Drop Role Statements
 *
 * Note: these node types are also used for the backwards-compatible
 * Create/Alter/Drop User/Group statements.  In the ALTER and DROP cases
 * there's really no need to distinguish what the original spelling was,
 * but for CREATE we mark the type because the defaults vary.
 * ----------------------
 */
typedef enum RoleStmtType
{
	ROLESTMT_ROLE,
	ROLESTMT_USER,
	ROLESTMT_GROUP,
} RoleStmtType;

typedef struct CreateRoleStmt
{
	NodeTag		type;
	RoleStmtType stmt_type;		/* ROLE/USER/GROUP */
	char	   *role;			/* role name */
	List	   *options;		/* List of DefElem nodes */
} CreateRoleStmt;

typedef struct AlterRoleStmt
{
	NodeTag		type;
	RoleSpec   *role;			/* role */
	List	   *options;		/* List of DefElem nodes */
	int			action;			/* +1 = add members, -1 = drop members */
} AlterRoleStmt;

typedef struct AlterRoleSetStmt
{
	NodeTag		type;
	RoleSpec   *role;			/* role */
	char	   *database;		/* database name, or NULL */
	VariableSetStmt *setstmt;	/* SET or RESET subcommand */
} AlterRoleSetStmt;

typedef struct DropRoleStmt
{
	NodeTag		type;
	List	   *roles;			/* List of roles to remove */
	bool		missing_ok;		/* skip error if a role is missing? */
} DropRoleStmt;

/* ----------------------
 *		{Create|Alter} SEQUENCE Statement
 * ----------------------
 */

typedef struct CreateSeqStmt
{
	NodeTag		type;
	RangeVar   *sequence;		/* the sequence to create */
	List	   *options;
	Oid			ownerId;		/* ID of owner, or InvalidOid for default */
	bool		for_identity;
	bool		if_not_exists;	/* just do nothing if it already exists? */
} CreateSeqStmt;

typedef struct AlterSeqStmt
{
	NodeTag		type;
	RangeVar   *sequence;		/* the sequence to alter */
	List	   *options;
	bool		for_identity;
	bool		missing_ok;		/* skip error if a role is missing? */
} AlterSeqStmt;

/* ----------------------
 *		Create {Aggregate|Operator|Type} Statement
 * ----------------------
 */
typedef struct DefineStmt
{
	NodeTag		type;
	ObjectType	kind;			/* aggregate, operator, type */
	bool		oldstyle;		/* hack to signal old CREATE AGG syntax */
	List	   *defnames;		/* qualified name (list of String) */
	List	   *args;			/* a list of TypeName (if needed) */
	List	   *definition;		/* a list of DefElem */
	bool		if_not_exists;	/* just do nothing if it already exists? */
	bool		replace;		/* replace if already exists? */
} DefineStmt;

/* ----------------------
 *		Create Domain Statement
 * ----------------------
 */
typedef struct CreateDomainStmt
{
	NodeTag		type;
	List	   *domainname;		/* qualified name (list of String) */
	TypeName   *typeName;		/* the base type */
	CollateClause *collClause;	/* untransformed COLLATE spec, if any */
	List	   *constraints;	/* constraints (list of Constraint nodes) */
} CreateDomainStmt;

/* ----------------------
 *		Create Operator Class Statement
 * ----------------------
 */
typedef struct CreateOpClassStmt
{
	NodeTag		type;
	List	   *opclassname;	/* qualified name (list of String) */
	List	   *opfamilyname;	/* qualified name (ditto); NIL if omitted */
	char	   *amname;			/* name of index AM opclass is for */
	TypeName   *datatype;		/* datatype of indexed column */
	List	   *items;			/* List of CreateOpClassItem nodes */
	bool		isDefault;		/* Should be marked as default for type? */
} CreateOpClassStmt;

#define OPCLASS_ITEM_OPERATOR		1
#define OPCLASS_ITEM_FUNCTION		2
#define OPCLASS_ITEM_STORAGETYPE	3

typedef struct CreateOpClassItem
{
	NodeTag		type;
	int			itemtype;		/* see codes above */
	ObjectWithArgs *name;		/* operator or function name and args */
	int			number;			/* strategy num or support proc num */
	List	   *order_family;	/* only used for ordering operators */
	List	   *class_args;		/* amproclefttype/amprocrighttype or
								 * amoplefttype/amoprighttype */
	/* fields used for a storagetype item: */
	TypeName   *storedtype;		/* datatype stored in index */
} CreateOpClassItem;

/* ----------------------
 *		Create Operator Family Statement
 * ----------------------
 */
typedef struct CreateOpFamilyStmt
{
	NodeTag		type;
	List	   *opfamilyname;	/* qualified name (list of String) */
	char	   *amname;			/* name of index AM opfamily is for */
} CreateOpFamilyStmt;

/* ----------------------
 *		Alter Operator Family Statement
 * ----------------------
 */
typedef struct AlterOpFamilyStmt
{
	NodeTag		type;
	List	   *opfamilyname;	/* qualified name (list of String) */
	char	   *amname;			/* name of index AM opfamily is for */
	bool		isDrop;			/* ADD or DROP the items? */
	List	   *items;			/* List of CreateOpClassItem nodes */
} AlterOpFamilyStmt;

/* ----------------------
 *		Drop Table|Sequence|View|Index|Type|Domain|Conversion|Schema Statement
 * ----------------------
 */

typedef struct DropStmt
{
	NodeTag		type;
	List	   *objects;		/* list of names */
	ObjectType	removeType;		/* object type */
	DropBehavior behavior;		/* RESTRICT or CASCADE behavior */
	bool		missing_ok;		/* skip error if object is missing? */
	bool		concurrent;		/* drop index concurrently? */
} DropStmt;

/* ----------------------
 *				Truncate Table Statement
 * ----------------------
 */
typedef struct TruncateStmt
{
	NodeTag		type;
	List	   *relations;		/* relations (RangeVars) to be truncated */
	bool		restart_seqs;	/* restart owned sequences? */
	DropBehavior behavior;		/* RESTRICT or CASCADE behavior */
} TruncateStmt;

/* ----------------------
 *				Comment On Statement
 * ----------------------
 */
typedef struct CommentStmt
{
	NodeTag		type;
	ObjectType	objtype;		/* Object's type */
	Node	   *object;			/* Qualified name of the object */
	char	   *comment;		/* Comment to insert, or NULL to remove */
} CommentStmt;

/* ----------------------
 *				SECURITY LABEL Statement
 * ----------------------
 */
typedef struct SecLabelStmt
{
	NodeTag		type;
	ObjectType	objtype;		/* Object's type */
	Node	   *object;			/* Qualified name of the object */
	char	   *provider;		/* Label provider (or NULL) */
	char	   *label;			/* New security label to be assigned */
} SecLabelStmt;

/* ----------------------
 *		Declare Cursor Statement
 *
 * The "query" field is initially a raw parse tree, and is converted to a
 * Query node during parse analysis.  Note that rewriting and planning
 * of the query are always postponed until execution.
 * ----------------------
 */
#define CURSOR_OPT_BINARY		0x0001	/* BINARY */
#define CURSOR_OPT_SCROLL		0x0002	/* SCROLL explicitly given */
#define CURSOR_OPT_NO_SCROLL	0x0004	/* NO SCROLL explicitly given */
#define CURSOR_OPT_INSENSITIVE	0x0008	/* INSENSITIVE */
#define CURSOR_OPT_ASENSITIVE	0x0010	/* ASENSITIVE */
#define CURSOR_OPT_HOLD			0x0020	/* WITH HOLD */
/* these planner-control flags do not correspond to any SQL grammar: */
#define CURSOR_OPT_FAST_PLAN	0x0100	/* prefer fast-start plan */
#define CURSOR_OPT_GENERIC_PLAN 0x0200	/* force use of generic plan */
#define CURSOR_OPT_CUSTOM_PLAN	0x0400	/* force use of custom plan */
#define CURSOR_OPT_PARALLEL_OK	0x0800	/* parallel mode OK */

typedef struct DeclareCursorStmt
{
	NodeTag		type;
	char	   *portalname;		/* name of the portal (cursor) */
	int			options;		/* bitmask of options (see above) */
	Node	   *query;			/* the query (see comments above) */
} DeclareCursorStmt;

/* ----------------------
 *		Close Portal Statement
 * ----------------------
 */
typedef struct ClosePortalStmt
{
	NodeTag		type;
	char	   *portalname;		/* name of the portal (cursor) */
	/* NULL means CLOSE ALL */
} ClosePortalStmt;

/* ----------------------
 *		Fetch Statement (also Move)
 * ----------------------
 */
typedef enum FetchDirection
{
	/* for these, howMany is how many rows to fetch; FETCH_ALL means ALL */
	FETCH_FORWARD,
	FETCH_BACKWARD,
	/* for these, howMany indicates a position; only one row is fetched */
	FETCH_ABSOLUTE,
	FETCH_RELATIVE,
} FetchDirection;

typedef enum FetchDirectionKeywords
{
	FETCH_KEYWORD_NONE = 0,
	FETCH_KEYWORD_NEXT,
	FETCH_KEYWORD_PRIOR,
	FETCH_KEYWORD_FIRST,
	FETCH_KEYWORD_LAST,
	FETCH_KEYWORD_ABSOLUTE,
	FETCH_KEYWORD_RELATIVE,
	FETCH_KEYWORD_ALL,
	FETCH_KEYWORD_FORWARD,
	FETCH_KEYWORD_FORWARD_ALL,
	FETCH_KEYWORD_BACKWARD,
	FETCH_KEYWORD_BACKWARD_ALL,
} FetchDirectionKeywords;

#define FETCH_ALL	LONG_MAX

typedef struct FetchStmt
{
	NodeTag		type;
	FetchDirection direction;	/* see above */
	/* number of rows, or position argument */
	long		howMany pg_node_attr(query_jumble_ignore);
	/* name of portal (cursor) */
	char	   *portalname;
	/* true if MOVE */
	bool		ismove;

	/*
	 * Set when a direction_keyword (e.g., FETCH FORWARD) is used, to
	 * distinguish it from a numeric variant (e.g., FETCH 1) for the purpose
	 * of query jumbling.
	 */
	FetchDirectionKeywords direction_keyword;

	/* token location, or -1 if unknown */
	ParseLoc	location pg_node_attr(query_jumble_location);
} FetchStmt;

/* ----------------------
 *		Create Index Statement
 *
 * This represents creation of an index and/or an associated constraint.
 * If isconstraint is true, we should create a pg_constraint entry along
 * with the index.  But if indexOid isn't InvalidOid, we are not creating an
 * index, just a UNIQUE/PKEY constraint using an existing index.  isconstraint
 * must always be true in this case, and the fields describing the index
 * properties are empty.
 * ----------------------
 */
typedef struct IndexStmt
{
	NodeTag		type;
	char	   *idxname;		/* name of new index, or NULL for default */
	RangeVar   *relation;		/* relation to build index on */
	char	   *accessMethod;	/* name of access method (eg. btree) */
	char	   *tableSpace;		/* tablespace, or NULL for default */
	List	   *indexParams;	/* columns to index: a list of IndexElem */
	List	   *indexIncludingParams;	/* additional columns to index: a list
										 * of IndexElem */
	List	   *options;		/* WITH clause options: a list of DefElem */
	Node	   *whereClause;	/* qualification (partial-index predicate) */
	List	   *excludeOpNames; /* exclusion operator names, or NIL if none */
	char	   *idxcomment;		/* comment to apply to index, or NULL */
	Oid			indexOid;		/* OID of an existing index, if any */
	RelFileNumber oldNumber;	/* relfilenumber of existing storage, if any */
	SubTransactionId oldCreateSubid;	/* rd_createSubid of oldNumber */
	SubTransactionId oldFirstRelfilelocatorSubid;	/* rd_firstRelfilelocatorSubid
													 * of oldNumber */
	bool		unique;			/* is index unique? */
	bool		nulls_not_distinct; /* null treatment for UNIQUE constraints */
	bool		primary;		/* is index a primary key? */
	bool		isconstraint;	/* is it for a pkey/unique constraint? */
	bool		iswithoutoverlaps;	/* is the constraint WITHOUT OVERLAPS? */
	bool		deferrable;		/* is the constraint DEFERRABLE? */
	bool		initdeferred;	/* is the constraint INITIALLY DEFERRED? */
	bool		transformed;	/* true when transformIndexStmt is finished */
	bool		concurrent;		/* should this be a concurrent index build? */
	bool		if_not_exists;	/* just do nothing if index already exists? */
	bool		reset_default_tblspc;	/* reset default_tablespace prior to
										 * executing */
} IndexStmt;

/* ----------------------
 *		Create Statistics Statement
 * ----------------------
 */
typedef struct CreateStatsStmt
{
	NodeTag		type;
	List	   *defnames;		/* qualified name (list of String) */
	List	   *stat_types;		/* stat types (list of String) */
	List	   *exprs;			/* expressions to build statistics on */
	List	   *relations;		/* rels to build stats on (list of RangeVar) */
	char	   *stxcomment;		/* comment to apply to stats, or NULL */
	bool		transformed;	/* true when transformStatsStmt is finished */
	bool		if_not_exists;	/* do nothing if stats name already exists */
} CreateStatsStmt;

/*
 * StatsElem - statistics parameters (used in CREATE STATISTICS)
 *
 * For a plain attribute, 'name' is the name of the referenced table column
 * and 'expr' is NULL.  For an expression, 'name' is NULL and 'expr' is the
 * expression tree.
 */
typedef struct StatsElem
{
	NodeTag		type;
	char	   *name;			/* name of attribute to index, or NULL */
	Node	   *expr;			/* expression to index, or NULL */
} StatsElem;


/* ----------------------
 *		Alter Statistics Statement
 * ----------------------
 */
typedef struct AlterStatsStmt
{
	NodeTag		type;
	List	   *defnames;		/* qualified name (list of String) */
	Node	   *stxstattarget;	/* statistics target */
	bool		missing_ok;		/* skip error if statistics object is missing */
} AlterStatsStmt;

/* ----------------------
 *		Create Function Statement
 * ----------------------
 */
typedef struct CreateFunctionStmt
{
	NodeTag		type;
	bool		is_procedure;	/* it's really CREATE PROCEDURE */
	bool		replace;		/* T => replace if already exists */
	List	   *funcname;		/* qualified name of function to create */
	List	   *parameters;		/* a list of FunctionParameter */
	TypeName   *returnType;		/* the return type */
	List	   *options;		/* a list of DefElem */
	Node	   *sql_body;
} CreateFunctionStmt;

typedef enum FunctionParameterMode
{
	/* the assigned enum values appear in pg_proc, don't change 'em! */
	FUNC_PARAM_IN = 'i',		/* input only */
	FUNC_PARAM_OUT = 'o',		/* output only */
	FUNC_PARAM_INOUT = 'b',		/* both */
	FUNC_PARAM_VARIADIC = 'v',	/* variadic (always input) */
	FUNC_PARAM_TABLE = 't',		/* table function output column */
	/* this is not used in pg_proc: */
	FUNC_PARAM_DEFAULT = 'd',	/* default; effectively same as IN */
} FunctionParameterMode;

typedef struct FunctionParameter
{
	NodeTag		type;
	char	   *name;			/* parameter name, or NULL if not given */
	TypeName   *argType;		/* TypeName for parameter type */
	FunctionParameterMode mode; /* IN/OUT/etc */
	Node	   *defexpr;		/* raw default expr, or NULL if not given */
	ParseLoc	location;		/* token location, or -1 if unknown */
} FunctionParameter;

typedef struct AlterFunctionStmt
{
	NodeTag		type;
	ObjectType	objtype;
	ObjectWithArgs *func;		/* name and args of function */
	List	   *actions;		/* list of DefElem */
} AlterFunctionStmt;

/* ----------------------
 *		DO Statement
 *
 * DoStmt is the raw parser output, InlineCodeBlock is the execution-time API
 * ----------------------
 */
typedef struct DoStmt
{
	NodeTag		type;
	List	   *args;			/* List of DefElem nodes */
} DoStmt;

typedef struct InlineCodeBlock
{
	pg_node_attr(nodetag_only)	/* this is not a member of parse trees */

	NodeTag		type;
	char	   *source_text;	/* source text of anonymous code block */
	Oid			langOid;		/* OID of selected language */
	bool		langIsTrusted;	/* trusted property of the language */
	bool		atomic;			/* atomic execution context */
} InlineCodeBlock;

/* ----------------------
 *		CALL statement
 *
 * OUT-mode arguments are removed from the transformed funcexpr.  The outargs
 * list contains copies of the expressions for all output arguments, in the
 * order of the procedure's declared arguments.  (outargs is never evaluated,
 * but is useful to the caller as a reference for what to assign to.)
 * The transformed call state is not relevant in the query jumbling, only the
 * function call is.
 * ----------------------
 */
typedef struct CallStmt
{
	NodeTag		type;
	/* from the parser */
	FuncCall   *funccall pg_node_attr(query_jumble_ignore);
	/* transformed call, with only input args */
	FuncExpr   *funcexpr;
	/* transformed output-argument expressions */
	List	   *outargs;
} CallStmt;

typedef struct CallContext
{
	pg_node_attr(nodetag_only)	/* this is not a member of parse trees */

	NodeTag		type;
	bool		atomic;
} CallContext;

/* ----------------------
 *		Alter Object Rename Statement
 * ----------------------
 */
typedef struct RenameStmt
{
	NodeTag		type;
	ObjectType	renameType;		/* OBJECT_TABLE, OBJECT_COLUMN, etc */
	ObjectType	relationType;	/* if column name, associated relation type */
	RangeVar   *relation;		/* in case it's a table */
	Node	   *object;			/* in case it's some other object */
	char	   *subname;		/* name of contained object (column, rule,
								 * trigger, etc) */
	char	   *newname;		/* the new name */
	DropBehavior behavior;		/* RESTRICT or CASCADE behavior */
	bool		missing_ok;		/* skip error if missing? */
} RenameStmt;

/* ----------------------
 * ALTER object DEPENDS ON EXTENSION extname
 * ----------------------
 */
typedef struct AlterObjectDependsStmt
{
	NodeTag		type;
	ObjectType	objectType;		/* OBJECT_FUNCTION, OBJECT_TRIGGER, etc */
	RangeVar   *relation;		/* in case a table is involved */
	Node	   *object;			/* name of the object */
	String	   *extname;		/* extension name */
	bool		remove;			/* set true to remove dep rather than add */
} AlterObjectDependsStmt;

/* ----------------------
 *		ALTER object SET SCHEMA Statement
 * ----------------------
 */
typedef struct AlterObjectSchemaStmt
{
	NodeTag		type;
	ObjectType	objectType;		/* OBJECT_TABLE, OBJECT_TYPE, etc */
	RangeVar   *relation;		/* in case it's a table */
	Node	   *object;			/* in case it's some other object */
	char	   *newschema;		/* the new schema */
	bool		missing_ok;		/* skip error if missing? */
} AlterObjectSchemaStmt;

/* ----------------------
 *		Alter Object Owner Statement
 * ----------------------
 */
typedef struct AlterOwnerStmt
{
	NodeTag		type;
	ObjectType	objectType;		/* OBJECT_TABLE, OBJECT_TYPE, etc */
	RangeVar   *relation;		/* in case it's a table */
	Node	   *object;			/* in case it's some other object */
	RoleSpec   *newowner;		/* the new owner */
} AlterOwnerStmt;

/* ----------------------
 *		Alter Operator Set ( this-n-that )
 * ----------------------
 */
typedef struct AlterOperatorStmt
{
	NodeTag		type;
	ObjectWithArgs *opername;	/* operator name and argument types */
	List	   *options;		/* List of DefElem nodes */
} AlterOperatorStmt;

/* ------------------------
 *		Alter Type Set ( this-n-that )
 * ------------------------
 */
typedef struct AlterTypeStmt
{
	NodeTag		type;
	List	   *typeName;		/* type name (possibly qualified) */
	List	   *options;		/* List of DefElem nodes */
} AlterTypeStmt;

/* ----------------------
 *		Create Rule Statement
 * ----------------------
 */
typedef struct RuleStmt
{
	NodeTag		type;
	RangeVar   *relation;		/* relation the rule is for */
	char	   *rulename;		/* name of the rule */
	Node	   *whereClause;	/* qualifications */
	CmdType		event;			/* SELECT, INSERT, etc */
	bool		instead;		/* is a 'do instead'? */
	List	   *actions;		/* the action statements */
	bool		replace;		/* OR REPLACE */
} RuleStmt;

/* ----------------------
 *		Notify Statement
 * ----------------------
 */
typedef struct NotifyStmt
{
	NodeTag		type;
	char	   *conditionname;	/* condition name to notify */
	char	   *payload;		/* the payload string, or NULL if none */
} NotifyStmt;

/* ----------------------
 *		Listen Statement
 * ----------------------
 */
typedef struct ListenStmt
{
	NodeTag		type;
	char	   *conditionname;	/* condition name to listen on */
} ListenStmt;

/* ----------------------
 *		Unlisten Statement
 * ----------------------
 */
typedef struct UnlistenStmt
{
	NodeTag		type;
	char	   *conditionname;	/* name to unlisten on, or NULL for all */
} UnlistenStmt;

/* ----------------------
 *		{Begin|Commit|Rollback} Transaction Statement
 * ----------------------
 */
typedef enum TransactionStmtKind
{
	TRANS_STMT_BEGIN,
	TRANS_STMT_START,			/* semantically identical to BEGIN */
	TRANS_STMT_COMMIT,
	TRANS_STMT_ROLLBACK,
	TRANS_STMT_SAVEPOINT,
	TRANS_STMT_RELEASE,
	TRANS_STMT_ROLLBACK_TO,
	TRANS_STMT_PREPARE,
	TRANS_STMT_COMMIT_PREPARED,
	TRANS_STMT_ROLLBACK_PREPARED,
} TransactionStmtKind;

typedef struct TransactionStmt
{
	NodeTag		type;
	TransactionStmtKind kind;	/* see above */
	List	   *options;		/* for BEGIN/START commands */
	/* for savepoint commands */
	char	   *savepoint_name pg_node_attr(query_jumble_ignore);
	/* for two-phase-commit related commands */
	char	   *gid pg_node_attr(query_jumble_ignore);
	bool		chain;			/* AND CHAIN option */
	/* token location, or -1 if unknown */
	ParseLoc	location pg_node_attr(query_jumble_location);
} TransactionStmt;

/* ----------------------
 *		Create Type Statement, composite types
 * ----------------------
 */
typedef struct CompositeTypeStmt
{
	NodeTag		type;
	RangeVar   *typevar;		/* the composite type to be created */
	List	   *coldeflist;		/* list of ColumnDef nodes */
} CompositeTypeStmt;

/* ----------------------
 *		Create Type Statement, enum types
 * ----------------------
 */
typedef struct CreateEnumStmt
{
	NodeTag		type;
	List	   *typeName;		/* qualified name (list of String) */
	List	   *vals;			/* enum values (list of String) */
} CreateEnumStmt;

/* ----------------------
 *		Create Type Statement, range types
 * ----------------------
 */
typedef struct CreateRangeStmt
{
	NodeTag		type;
	List	   *typeName;		/* qualified name (list of String) */
	List	   *params;			/* range parameters (list of DefElem) */
} CreateRangeStmt;

/* ----------------------
 *		Alter Type Statement, enum types
 * ----------------------
 */
typedef struct AlterEnumStmt
{
	NodeTag		type;
	List	   *typeName;		/* qualified name (list of String) */
	char	   *oldVal;			/* old enum value's name, if renaming */
	char	   *newVal;			/* new enum value's name */
	char	   *newValNeighbor; /* neighboring enum value, if specified */
	bool		newValIsAfter;	/* place new enum value after neighbor? */
	bool		skipIfNewValExists; /* no error if new already exists? */
} AlterEnumStmt;

/* ----------------------
 *		Create View Statement
 * ----------------------
 */
typedef enum ViewCheckOption
{
	NO_CHECK_OPTION,
	LOCAL_CHECK_OPTION,
	CASCADED_CHECK_OPTION,
} ViewCheckOption;

typedef struct ViewStmt
{
	NodeTag		type;
	RangeVar   *view;			/* the view to be created */
	List	   *aliases;		/* target column names */
	Node	   *query;			/* the SELECT query (as a raw parse tree) */
	bool		replace;		/* replace an existing view? */
	List	   *options;		/* options from WITH clause */
	ViewCheckOption withCheckOption;	/* WITH CHECK OPTION */
} ViewStmt;

/* ----------------------
 *		Load Statement
 * ----------------------
 */
typedef struct LoadStmt
{
	NodeTag		type;
	char	   *filename;		/* file to load */
} LoadStmt;

/* ----------------------
 *		Createdb Statement
 * ----------------------
 */
typedef struct CreatedbStmt
{
	NodeTag		type;
	char	   *dbname;			/* name of database to create */
	List	   *options;		/* List of DefElem nodes */
} CreatedbStmt;

/* ----------------------
 *	Alter Database
 * ----------------------
 */
typedef struct AlterDatabaseStmt
{
	NodeTag		type;
	char	   *dbname;			/* name of database to alter */
	List	   *options;		/* List of DefElem nodes */
} AlterDatabaseStmt;

typedef struct AlterDatabaseRefreshCollStmt
{
	NodeTag		type;
	char	   *dbname;
} AlterDatabaseRefreshCollStmt;

typedef struct AlterDatabaseSetStmt
{
	NodeTag		type;
	char	   *dbname;			/* database name */
	VariableSetStmt *setstmt;	/* SET or RESET subcommand */
} AlterDatabaseSetStmt;

/* ----------------------
 *		Dropdb Statement
 * ----------------------
 */
typedef struct DropdbStmt
{
	NodeTag		type;
	char	   *dbname;			/* database to drop */
	bool		missing_ok;		/* skip error if db is missing? */
	List	   *options;		/* currently only FORCE is supported */
} DropdbStmt;

/* ----------------------
 *		Alter System Statement
 * ----------------------
 */
typedef struct AlterSystemStmt
{
	NodeTag		type;
	VariableSetStmt *setstmt;	/* SET subcommand */
} AlterSystemStmt;

/* ----------------------
 *		Cluster Statement (support pbrown's cluster index implementation)
 * ----------------------
 */
typedef struct ClusterStmt
{
	NodeTag		type;
	RangeVar   *relation;		/* relation being indexed, or NULL if all */
	char	   *indexname;		/* original index defined */
	List	   *params;			/* list of DefElem nodes */
} ClusterStmt;

/* ----------------------
 *		Vacuum and Analyze Statements
 *
 * Even though these are nominally two statements, it's convenient to use
 * just one node type for both.
 * ----------------------
 */
typedef struct VacuumStmt
{
	NodeTag		type;
	List	   *options;		/* list of DefElem nodes */
	List	   *rels;			/* list of VacuumRelation, or NIL for all */
	bool		is_vacuumcmd;	/* true for VACUUM, false for ANALYZE */
} VacuumStmt;

/*
 * Info about a single target table of VACUUM/ANALYZE.
 *
 * If the OID field is set, it always identifies the table to process.
 * Then the relation field can be NULL; if it isn't, it's used only to report
 * failure to open/lock the relation.
 */
typedef struct VacuumRelation
{
	NodeTag		type;
	RangeVar   *relation;		/* table name to process, or NULL */
	Oid			oid;			/* table's OID; InvalidOid if not looked up */
	List	   *va_cols;		/* list of column names, or NIL for all */
} VacuumRelation;

/* ----------------------
 *		Explain Statement
 *
 * The "query" field is initially a raw parse tree, and is converted to a
 * Query node during parse analysis.  Note that rewriting and planning
 * of the query are always postponed until execution.
 * ----------------------
 */
typedef struct ExplainStmt
{
	NodeTag		type;
	Node	   *query;			/* the query (see comments above) */
	List	   *options;		/* list of DefElem nodes */
} ExplainStmt;

/* ----------------------
 *		CREATE TABLE AS Statement (a/k/a SELECT INTO)
 *
 * A query written as CREATE TABLE AS will produce this node type natively.
 * A query written as SELECT ... INTO will be transformed to this form during
 * parse analysis.
 * A query written as CREATE MATERIALIZED view will produce this node type,
 * during parse analysis, since it needs all the same data.
 *
 * The "query" field is handled similarly to EXPLAIN, though note that it
 * can be a SELECT or an EXECUTE, but not other DML statements.
 * ----------------------
 */
typedef struct CreateTableAsStmt
{
	NodeTag		type;
	Node	   *query;			/* the query (see comments above) */
	IntoClause *into;			/* destination table */
	ObjectType	objtype;		/* OBJECT_TABLE or OBJECT_MATVIEW */
	bool		is_select_into; /* it was written as SELECT INTO */
	bool		if_not_exists;	/* just do nothing if it already exists? */
} CreateTableAsStmt;

/* ----------------------
 *		REFRESH MATERIALIZED VIEW Statement
 * ----------------------
 */
typedef struct RefreshMatViewStmt
{
	NodeTag		type;
	bool		concurrent;		/* allow concurrent access? */
	bool		skipData;		/* true for WITH NO DATA */
	RangeVar   *relation;		/* relation to insert into */
} RefreshMatViewStmt;

/* ----------------------
 * Checkpoint Statement
 * ----------------------
 */
typedef struct CheckPointStmt
{
	NodeTag		type;
	List	   *options;		/* list of DefElem nodes */
} CheckPointStmt;

/* ----------------------
 * Discard Statement
 * ----------------------
 */

typedef enum DiscardMode
{
	DISCARD_ALL,
	DISCARD_PLANS,
	DISCARD_SEQUENCES,
	DISCARD_TEMP,
} DiscardMode;

typedef struct DiscardStmt
{
	NodeTag		type;
	DiscardMode target;
} DiscardStmt;

/* ----------------------
 *		LOCK Statement
 * ----------------------
 */
typedef struct LockStmt
{
	NodeTag		type;
	List	   *relations;		/* relations to lock */
	int			mode;			/* lock mode */
	bool		nowait;			/* no wait mode */
} LockStmt;

/* ----------------------
 *		SET CONSTRAINTS Statement
 * ----------------------
 */
typedef struct ConstraintsSetStmt
{
	NodeTag		type;
	List	   *constraints;	/* List of names as RangeVars */
	bool		deferred;
} ConstraintsSetStmt;

/* ----------------------
 *		REINDEX Statement
 * ----------------------
 */
typedef enum ReindexObjectType
{
	REINDEX_OBJECT_INDEX,		/* index */
	REINDEX_OBJECT_TABLE,		/* table or materialized view */
	REINDEX_OBJECT_SCHEMA,		/* schema */
	REINDEX_OBJECT_SYSTEM,		/* system catalogs */
	REINDEX_OBJECT_DATABASE,	/* database */
} ReindexObjectType;

typedef struct ReindexStmt
{
	NodeTag		type;
	ReindexObjectType kind;		/* REINDEX_OBJECT_INDEX, REINDEX_OBJECT_TABLE,
								 * etc. */
	RangeVar   *relation;		/* Table or index to reindex */
	const char *name;			/* name of database to reindex */
	List	   *params;			/* list of DefElem nodes */
} ReindexStmt;

/* ----------------------
 *		CREATE CONVERSION Statement
 * ----------------------
 */
typedef struct CreateConversionStmt
{
	NodeTag		type;
	List	   *conversion_name;	/* Name of the conversion */
	char	   *for_encoding_name;	/* source encoding name */
	char	   *to_encoding_name;	/* destination encoding name */
	List	   *func_name;		/* qualified conversion function name */
	bool		def;			/* is this a default conversion? */
} CreateConversionStmt;

/* ----------------------
 *	CREATE CAST Statement
 * ----------------------
 */
typedef struct CreateCastStmt
{
	NodeTag		type;
	TypeName   *sourcetype;
	TypeName   *targettype;
	ObjectWithArgs *func;
	CoercionContext context;
	bool		inout;
} CreateCastStmt;

/* ----------------------
 *	CREATE TRANSFORM Statement
 * ----------------------
 */
typedef struct CreateTransformStmt
{
	NodeTag		type;
	bool		replace;
	TypeName   *type_name;
	char	   *lang;
	ObjectWithArgs *fromsql;
	ObjectWithArgs *tosql;
} CreateTransformStmt;

/* ----------------------
 *		PREPARE Statement
 * ----------------------
 */
typedef struct PrepareStmt
{
	NodeTag		type;
	char	   *name;			/* Name of plan, arbitrary */
	List	   *argtypes;		/* Types of parameters (List of TypeName) */
	Node	   *query;			/* The query itself (as a raw parsetree) */
} PrepareStmt;


/* ----------------------
 *		EXECUTE Statement
 * ----------------------
 */

typedef struct ExecuteStmt
{
	NodeTag		type;
	char	   *name;			/* The name of the plan to execute */
	List	   *params;			/* Values to assign to parameters */
} ExecuteStmt;


/* ----------------------
 *		DEALLOCATE Statement
 * ----------------------
 */
typedef struct DeallocateStmt
{
	NodeTag		type;
	/* The name of the plan to remove, NULL if DEALLOCATE ALL */
	char	   *name pg_node_attr(query_jumble_ignore);

	/*
	 * True if DEALLOCATE ALL.  This is redundant with "name == NULL", but we
	 * make it a separate field so that exactly this condition (and not the
	 * precise name) will be accounted for in query jumbling.
	 */
	bool		isall;
	/* token location, or -1 if unknown */
	ParseLoc	location pg_node_attr(query_jumble_location);
} DeallocateStmt;

/*
 *		DROP OWNED statement
 */
typedef struct DropOwnedStmt
{
	NodeTag		type;
	List	   *roles;
	DropBehavior behavior;
} DropOwnedStmt;

/*
 *		REASSIGN OWNED statement
 */
typedef struct ReassignOwnedStmt
{
	NodeTag		type;
	List	   *roles;
	RoleSpec   *newrole;
} ReassignOwnedStmt;

/*
 * TS Dictionary stmts: DefineStmt, RenameStmt and DropStmt are default
 */
typedef struct AlterTSDictionaryStmt
{
	NodeTag		type;
	List	   *dictname;		/* qualified name (list of String) */
	List	   *options;		/* List of DefElem nodes */
} AlterTSDictionaryStmt;

/*
 * TS Configuration stmts: DefineStmt, RenameStmt and DropStmt are default
 */
typedef enum AlterTSConfigType
{
	ALTER_TSCONFIG_ADD_MAPPING,
	ALTER_TSCONFIG_ALTER_MAPPING_FOR_TOKEN,
	ALTER_TSCONFIG_REPLACE_DICT,
	ALTER_TSCONFIG_REPLACE_DICT_FOR_TOKEN,
	ALTER_TSCONFIG_DROP_MAPPING,
} AlterTSConfigType;

typedef struct AlterTSConfigurationStmt
{
	NodeTag		type;
	AlterTSConfigType kind;		/* ALTER_TSCONFIG_ADD_MAPPING, etc */
	List	   *cfgname;		/* qualified name (list of String) */

	/*
	 * dicts will be non-NIL if ADD/ALTER MAPPING was specified. If dicts is
	 * NIL, but tokentype isn't, DROP MAPPING was specified.
	 */
	List	   *tokentype;		/* list of String */
	List	   *dicts;			/* list of list of String */
	bool		override;		/* if true - remove old variant */
	bool		replace;		/* if true - replace dictionary by another */
	bool		missing_ok;		/* for DROP - skip error if missing? */
} AlterTSConfigurationStmt;

typedef struct PublicationTable
{
	NodeTag		type;
	RangeVar   *relation;		/* relation to be published */
	Node	   *whereClause;	/* qualifications */
	List	   *columns;		/* List of columns in a publication table */
} PublicationTable;

/*
 * Publication object type
 */
typedef enum PublicationObjSpecType
{
	PUBLICATIONOBJ_TABLE,		/* A table */
	PUBLICATIONOBJ_TABLES_IN_SCHEMA,	/* All tables in schema */
	PUBLICATIONOBJ_TABLES_IN_CUR_SCHEMA,	/* All tables in first element of
											 * search_path */
	PUBLICATIONOBJ_CONTINUATION,	/* Continuation of previous type */
} PublicationObjSpecType;

typedef struct PublicationObjSpec
{
	NodeTag		type;
	PublicationObjSpecType pubobjtype;	/* type of this publication object */
	char	   *name;
	PublicationTable *pubtable;
	ParseLoc	location;		/* token location, or -1 if unknown */
} PublicationObjSpec;

typedef struct CreatePublicationStmt
{
	NodeTag		type;
	char	   *pubname;		/* Name of the publication */
	List	   *options;		/* List of DefElem nodes */
	List	   *pubobjects;		/* Optional list of publication objects */
	bool		for_all_tables; /* Special publication for all tables in db */
} CreatePublicationStmt;

typedef enum AlterPublicationAction
{
	AP_AddObjects,				/* add objects to publication */
	AP_DropObjects,				/* remove objects from publication */
	AP_SetObjects,				/* set list of objects */
} AlterPublicationAction;

typedef struct AlterPublicationStmt
{
	NodeTag		type;
	char	   *pubname;		/* Name of the publication */

	/* parameters used for ALTER PUBLICATION ... WITH */
	List	   *options;		/* List of DefElem nodes */

	/*
	 * Parameters used for ALTER PUBLICATION ... ADD/DROP/SET publication
	 * objects.
	 */
	List	   *pubobjects;		/* Optional list of publication objects */
	bool		for_all_tables; /* Special publication for all tables in db */
	AlterPublicationAction action;	/* What action to perform with the given
									 * objects */
} AlterPublicationStmt;

typedef struct CreateSubscriptionStmt
{
	NodeTag		type;
	char	   *subname;		/* Name of the subscription */
	char	   *conninfo;		/* Connection string to publisher */
	List	   *publication;	/* One or more publication to subscribe to */
	List	   *options;		/* List of DefElem nodes */
} CreateSubscriptionStmt;

typedef enum AlterSubscriptionType
{
	ALTER_SUBSCRIPTION_OPTIONS,
	ALTER_SUBSCRIPTION_CONNECTION,
	ALTER_SUBSCRIPTION_SET_PUBLICATION,
	ALTER_SUBSCRIPTION_ADD_PUBLICATION,
	ALTER_SUBSCRIPTION_DROP_PUBLICATION,
	ALTER_SUBSCRIPTION_REFRESH,
	ALTER_SUBSCRIPTION_ENABLED,
	ALTER_SUBSCRIPTION_SKIP,
} AlterSubscriptionType;

typedef struct AlterSubscriptionStmt
{
	NodeTag		type;
	AlterSubscriptionType kind; /* ALTER_SUBSCRIPTION_OPTIONS, etc */
	char	   *subname;		/* Name of the subscription */
	char	   *conninfo;		/* Connection string to publisher */
	List	   *publication;	/* One or more publication to subscribe to */
	List	   *options;		/* List of DefElem nodes */
} AlterSubscriptionStmt;

typedef struct DropSubscriptionStmt
{
	NodeTag		type;
	char	   *subname;		/* Name of the subscription */
	bool		missing_ok;		/* Skip error if missing? */
	DropBehavior behavior;		/* RESTRICT or CASCADE behavior */
} DropSubscriptionStmt;

#endif							/* PARSENODES_H */
