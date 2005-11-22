/*-------------------------------------------------------------------------
 *
 * parsenodes.h
 *	  definitions for parse tree nodes
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/nodes/parsenodes.h,v 1.292.2.1 2005/11/22 18:23:28 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSENODES_H
#define PARSENODES_H

#include "nodes/primnodes.h"


/* Possible sources of a Query */
typedef enum QuerySource
{
	QSRC_ORIGINAL,				/* original parsetree (explicit query) */
	QSRC_PARSER,				/* added by parse analysis */
	QSRC_INSTEAD_RULE,			/* added by unconditional INSTEAD rule */
	QSRC_QUAL_INSTEAD_RULE,		/* added by conditional INSTEAD rule */
	QSRC_NON_INSTEAD_RULE		/* added by non-INSTEAD rule */
} QuerySource;

/*
 * Grantable rights are encoded so that we can OR them together in a bitmask.
 * The present representation of AclItem limits us to 16 distinct rights,
 * even though AclMode is defined as uint32.  See utils/acl.h.
 *
 * Caution: changing these codes breaks stored ACLs, hence forces initdb.
 */
typedef uint32 AclMode;			/* a bitmask of privilege bits */

#define ACL_INSERT		(1<<0)	/* for relations */
#define ACL_SELECT		(1<<1)
#define ACL_UPDATE		(1<<2)
#define ACL_DELETE		(1<<3)
#define ACL_RULE		(1<<4)
#define ACL_REFERENCES	(1<<5)
#define ACL_TRIGGER		(1<<6)
#define ACL_EXECUTE		(1<<7)	/* for functions */
#define ACL_USAGE		(1<<8)	/* for languages and namespaces */
#define ACL_CREATE		(1<<9)	/* for namespaces and databases */
#define ACL_CREATE_TEMP (1<<10) /* for databases */
#define N_ACL_RIGHTS	11		/* 1 plus the last 1<<x */
#define ACL_NO_RIGHTS	0
/* Currently, SELECT ... FOR UPDATE/FOR SHARE requires UPDATE privileges */
#define ACL_SELECT_FOR_UPDATE	ACL_UPDATE


/*****************************************************************************
 *	Query Tree
 *****************************************************************************/

/*
 * Query -
 *	  all statements are turned into a Query tree (via transformStmt)
 *	  for further processing by the optimizer
 *
 *	  utility statements (i.e. non-optimizable statements) have the
 *	  utilityStmt field set, and the Query itself is mostly dummy.
 */
typedef struct Query
{
	NodeTag		type;

	CmdType		commandType;	/* select|insert|update|delete|utility */

	QuerySource querySource;	/* where did I come from? */

	bool		canSetTag;		/* do I set the command result tag? */

	Node	   *utilityStmt;	/* non-null if this is a non-optimizable
								 * statement */

	int			resultRelation; /* target relation (index into rtable) */

	RangeVar   *into;			/* target relation for SELECT INTO */
	bool		intoHasOids;	/* should target relation contain OIDs? */

	bool		hasAggs;		/* has aggregates in tlist or havingQual */
	bool		hasSubLinks;	/* has subquery SubLink */

	List	   *rtable;			/* list of range table entries */
	FromExpr   *jointree;		/* table join tree (FROM and WHERE clauses) */

	List	   *rowMarks;		/* integer list of RT indexes of relations
								 * that are selected FOR UPDATE/SHARE */

	bool		forUpdate;		/* true if rowMarks are FOR UPDATE, false if
								 * they are FOR SHARE */
	bool		rowNoWait;		/* FOR UPDATE/SHARE NOWAIT option */

	List	   *targetList;		/* target list (of TargetEntry) */

	List	   *groupClause;	/* a list of GroupClause's */

	Node	   *havingQual;		/* qualifications applied to groups */

	List	   *distinctClause; /* a list of SortClause's */

	List	   *sortClause;		/* a list of SortClause's */

	Node	   *limitOffset;	/* # of result tuples to skip */
	Node	   *limitCount;		/* # of result tuples to return */

	Node	   *setOperations;	/* set-operation tree if this is top level of
								 * a UNION/INTERSECT/EXCEPT query */

	/*
	 * If the resultRelation turns out to be the parent of an inheritance
	 * tree, the planner will add all the child tables to the rtable and store
	 * a list of the rtindexes of all the result relations here. This is done
	 * at plan time, not parse time, since we don't want to commit to the
	 * exact set of child tables at parse time.  This field ought to go in
	 * some sort of TopPlan plan node, not in the Query.
	 */
	List	   *resultRelations;	/* integer list of RT indexes, or NIL */
} Query;


/****************************************************************************
 *	Supporting data structures for Parse Trees
 *
 *	Most of these node types appear in raw parsetrees output by the grammar,
 *	and get transformed to something else by the analyzer.	A few of them
 *	are used as-is in transformed querytrees.
 ****************************************************************************/

/*
 * TypeName - specifies a type in definitions
 *
 * For TypeName structures generated internally, it is often easier to
 * specify the type by OID than by name.  If "names" is NIL then the
 * actual type OID is given by typeid, otherwise typeid is unused.
 *
 * If pct_type is TRUE, then names is actually a field name and we look up
 * the type of that field.	Otherwise (the normal case), names is a type
 * name possibly qualified with schema and database name.
 */
typedef struct TypeName
{
	NodeTag		type;
	List	   *names;			/* qualified name (list of Value strings) */
	Oid			typeid;			/* type identified by OID */
	bool		timezone;		/* timezone specified? */
	bool		setof;			/* is a set? */
	bool		pct_type;		/* %TYPE specified? */
	int32		typmod;			/* type modifier */
	List	   *arrayBounds;	/* array bounds */
} TypeName;

/*
 * ColumnRef - specifies a reference to a column, or possibly a whole tuple
 *
 *		The "fields" list must be nonempty; its last component may be "*"
 *		instead of a regular field name.
 *
 * Note: any array subscripting or selection of fields from composite columns
 * is represented by an A_Indirection node above the ColumnRef.  However,
 * for simplicity in the normal case, initial field selection from a table
 * name is represented within ColumnRef and not by adding A_Indirection.
 */
typedef struct ColumnRef
{
	NodeTag		type;
	List	   *fields;			/* field names (list of Value strings) */
} ColumnRef;

/*
 * ParamRef - specifies a $n parameter reference
 */
typedef struct ParamRef
{
	NodeTag		type;
	int			number;			/* the number of the parameter */
} ParamRef;

/*
 * A_Expr - infix, prefix, and postfix expressions
 */
typedef enum A_Expr_Kind
{
	AEXPR_OP,					/* normal operator */
	AEXPR_AND,					/* booleans - name field is unused */
	AEXPR_OR,
	AEXPR_NOT,
	AEXPR_OP_ANY,				/* scalar op ANY (array) */
	AEXPR_OP_ALL,				/* scalar op ALL (array) */
	AEXPR_DISTINCT,				/* IS DISTINCT FROM - name must be "=" */
	AEXPR_NULLIF,				/* NULLIF - name must be "=" */
	AEXPR_OF					/* IS (not) OF - name must be "=" or "!=" */
} A_Expr_Kind;

typedef struct A_Expr
{
	NodeTag		type;
	A_Expr_Kind kind;			/* see above */
	List	   *name;			/* possibly-qualified name of operator */
	Node	   *lexpr;			/* left argument, or NULL if none */
	Node	   *rexpr;			/* right argument, or NULL if none */
} A_Expr;

/*
 * A_Const - a constant expression
 */
typedef struct A_Const
{
	NodeTag		type;
	Value		val;			/* the value (with the tag) */
	TypeName   *typename;		/* typecast */
} A_Const;

/*
 * TypeCast - a CAST expression
 *
 * NOTE: for mostly historical reasons, A_Const parsenodes contain
 * room for a TypeName; we only generate a separate TypeCast node if the
 * argument to be casted is not a constant.  In theory either representation
 * would work, but it is convenient to have the target type immediately
 * available while resolving a constant's datatype.
 */
typedef struct TypeCast
{
	NodeTag		type;
	Node	   *arg;			/* the expression being casted */
	TypeName   *typename;		/* the target type */
} TypeCast;

/*
 * FuncCall - a function or aggregate invocation
 *
 * agg_star indicates we saw a 'foo(*)' construct, while agg_distinct
 * indicates we saw 'foo(DISTINCT ...)'.  In either case, the construct
 * *must* be an aggregate call.  Otherwise, it might be either an
 * aggregate or some other kind of function.
 */
typedef struct FuncCall
{
	NodeTag		type;
	List	   *funcname;		/* qualified name of function */
	List	   *args;			/* the arguments (list of exprs) */
	bool		agg_star;		/* argument was really '*' */
	bool		agg_distinct;	/* arguments were labeled DISTINCT */
} FuncCall;

/*
 * A_Indices - array reference or bounds ([lidx:uidx] or [uidx])
 */
typedef struct A_Indices
{
	NodeTag		type;
	Node	   *lidx;			/* could be NULL */
	Node	   *uidx;
} A_Indices;

/*
 * A_Indirection - select a field and/or array element from an expression
 *
 * The indirection list can contain both A_Indices nodes (representing
 * subscripting) and string Value nodes (representing field selection
 * --- the string value is the name of the field to select).  For example,
 * a complex selection operation like
 *				(foo).field1[42][7].field2
 * would be represented with a single A_Indirection node having a 4-element
 * indirection list.
 *
 * Note: as of Postgres 8.0, we don't support arrays of composite values,
 * so cases in which a field select follows a subscript aren't actually
 * semantically legal.	However the parser is prepared to handle such.
 */
typedef struct A_Indirection
{
	NodeTag		type;
	Node	   *arg;			/* the thing being selected from */
	List	   *indirection;	/* subscripts and/or field names */
} A_Indirection;

/*
 * ResTarget -
 *	  result target (used in target list of pre-transformed parse trees)
 *
 * In a SELECT or INSERT target list, 'name' is the column label from an
 * 'AS ColumnLabel' clause, or NULL if there was none, and 'val' is the
 * value expression itself.  The 'indirection' field is not used.
 *
 * INSERT has a second ResTarget list which is the target-column-names list.
 * Here, 'val' is not used, 'name' is the name of the destination column,
 * and 'indirection' stores any subscripts attached to the destination.
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
	List	   *indirection;	/* subscripts and field names, or NIL */
	Node	   *val;			/* the value expression to compute or assign */
} ResTarget;

/*
 * SortBy - for ORDER BY clause
 */
#define SORTBY_ASC		1
#define SORTBY_DESC		2
#define SORTBY_USING	3

typedef struct SortBy
{
	NodeTag		type;
	int			sortby_kind;	/* see codes above */
	List	   *useOp;			/* name of op to use, if SORTBY_USING */
	Node	   *node;			/* expression to sort on */
} SortBy;

/*
 * RangeSubselect - subquery appearing in a FROM clause
 */
typedef struct RangeSubselect
{
	NodeTag		type;
	Node	   *subquery;		/* the untransformed sub-select clause */
	Alias	   *alias;			/* table alias & optional column aliases */
} RangeSubselect;

/*
 * RangeFunction - function call appearing in a FROM clause
 */
typedef struct RangeFunction
{
	NodeTag		type;
	Node	   *funccallnode;	/* untransformed function call tree */
	Alias	   *alias;			/* table alias & optional column aliases */
	List	   *coldeflist;		/* list of ColumnDef nodes for runtime
								 * assignment of RECORD TupleDesc */
} RangeFunction;

/*
 * ColumnDef - column definition (used in various creates)
 *
 * If the column has a default value, we may have the value expression
 * in either "raw" form (an untransformed parse tree) or "cooked" form
 * (the nodeToString representation of an executable expression tree),
 * depending on how this ColumnDef node was created (by parsing, or by
 * inheritance from an existing relation).	We should never have both
 * in the same node!
 *
 * The constraints list may contain a CONSTR_DEFAULT item in a raw
 * parsetree produced by gram.y, but transformCreateStmt will remove
 * the item and set raw_default instead.  CONSTR_DEFAULT items
 * should not appear in any subsequent processing.
 *
 * The "support" field, if not null, denotes a supporting relation that
 * should be linked by an internal dependency to the column.  Currently
 * this is only used to link a SERIAL column's sequence to the column.
 */
typedef struct ColumnDef
{
	NodeTag		type;
	char	   *colname;		/* name of column */
	TypeName   *typename;		/* type of column */
	int			inhcount;		/* number of times column is inherited */
	bool		is_local;		/* column has local (non-inherited) def'n */
	bool		is_not_null;	/* NOT NULL constraint specified? */
	Node	   *raw_default;	/* default value (untransformed parse tree) */
	char	   *cooked_default; /* nodeToString representation */
	List	   *constraints;	/* other constraints on column */
	RangeVar   *support;		/* supporting relation, if any */
} ColumnDef;

/*
 * inhRelation - Relations a CREATE TABLE is to inherit attributes of
 */
typedef struct InhRelation
{
	NodeTag		type;
	RangeVar   *relation;
	bool		including_defaults;
} InhRelation;

/*
 * IndexElem - index parameters (used in CREATE INDEX)
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
	List	   *opclass;		/* name of desired opclass; NIL = default */
} IndexElem;

/*
 * DefElem -
 *	  a definition (used in definition lists in the form of defname = arg)
 */
typedef struct DefElem
{
	NodeTag		type;
	char	   *defname;
	Node	   *arg;			/* a (Value *) or a (TypeName *) */
} DefElem;

/*
 * LockingClause - raw representation of FOR UPDATE/SHARE options
 *
 * Note: lockedRels == NIL means "all relations in query".	Otherwise it
 * is a list of String nodes giving relation eref names.
 */
typedef struct LockingClause
{
	NodeTag		type;
	List	   *lockedRels;		/* FOR UPDATE or FOR SHARE relations */
	bool		forUpdate;		/* true = FOR UPDATE, false = FOR SHARE */
	bool		nowait;			/* NOWAIT option */
} LockingClause;


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
 *	  dropped columns.	When building an RTE we insert empty strings ("") for
 *	  dropped columns.	Note however that a stored rule may have nonempty
 *	  colnames for columns dropped since the rule was created (and for that
 *	  matter the colnames might be out of date due to column renamings).
 *	  The same comments apply to FUNCTION RTEs when the function's return type
 *	  is a named composite type.
 *
 *	  In JOIN RTEs, the colnames in both alias and eref are one-to-one with
 *	  joinaliasvars entries.  A JOIN RTE will omit columns of its inputs when
 *	  those columns are known to be dropped at parse time.	Again, however,
 *	  a stored rule might contain entries for columns dropped since the rule
 *	  was created.	(This is only possible for columns not actually referenced
 *	  in the rule.)  When loading a stored rule, we replace the joinaliasvars
 *	  items for any such columns with NULL Consts.	(We can't simply delete
 *	  them from the joinaliasvars list, because that would affect the attnums
 *	  of Vars referencing the rest of the list.)
 *
 *	  inh is TRUE for relation references that should be expanded to include
 *	  inheritance children, if the rel has any.  This *must* be FALSE for
 *	  RTEs other than RTE_RELATION entries.
 *
 *	  inFromCl marks those range variables that are listed in the FROM clause.
 *	  It's false for RTEs that are added to a query behind the scenes, such
 *	  as the NEW and OLD variables for a rule, or the subqueries of a UNION.
 *	  This flag is not used anymore during parsing, since the parser now uses
 *	  a separate "namespace" data structure to control visibility, but it is
 *	  needed by ruleutils.c to determine whether RTEs should be shown in
 *	  decompiled queries.
 *
 *	  requiredPerms and checkAsUser specify run-time access permissions
 *	  checks to be performed at query startup.	The user must have *all*
 *	  of the permissions that are OR'd together in requiredPerms (zero
 *	  indicates no permissions checking).  If checkAsUser is not zero,
 *	  then do the permissions checks using the access rights of that user,
 *	  not the current effective user ID.  (This allows rules to act as
 *	  setuid gateways.)
 *--------------------
 */
typedef enum RTEKind
{
	RTE_RELATION,				/* ordinary relation reference */
	RTE_SUBQUERY,				/* subquery in FROM */
	RTE_JOIN,					/* join */
	RTE_SPECIAL,				/* special rule relation (NEW or OLD) */
	RTE_FUNCTION				/* function in FROM */
} RTEKind;

typedef struct RangeTblEntry
{
	NodeTag		type;

	RTEKind		rtekind;		/* see above */

	/*
	 * XXX the fields applicable to only some rte kinds should be merged into
	 * a union.  I didn't do this yet because the diffs would impact a lot of
	 * code that is being actively worked on.  FIXME later.
	 */

	/*
	 * Fields valid for a plain relation RTE (else zero):
	 */
	Oid			relid;			/* OID of the relation */

	/*
	 * Fields valid for a subquery RTE (else NULL):
	 */
	Query	   *subquery;		/* the sub-query */

	/*
	 * Fields valid for a function RTE (else NULL):
	 */
	Node	   *funcexpr;		/* expression tree for func call */
	List	   *coldeflist;		/* list of ColumnDef nodes for runtime
								 * assignment of RECORD TupleDesc */

	/*
	 * Fields valid for a join RTE (else NULL/zero):
	 *
	 * joinaliasvars is a list of Vars or COALESCE expressions corresponding
	 * to the columns of the join result.  An alias Var referencing column K
	 * of the join result can be replaced by the K'th element of joinaliasvars
	 * --- but to simplify the task of reverse-listing aliases correctly, we
	 * do not do that until planning time.	In a Query loaded from a stored
	 * rule, it is also possible for joinaliasvars items to be NULL Consts,
	 * denoting columns dropped since the rule was made.
	 */
	JoinType	jointype;		/* type of join */
	List	   *joinaliasvars;	/* list of alias-var expansions */

	/*
	 * Fields valid in all RTEs:
	 */
	Alias	   *alias;			/* user-written alias clause, if any */
	Alias	   *eref;			/* expanded reference names */
	bool		inh;			/* inheritance requested? */
	bool		inFromCl;		/* present in FROM clause? */
	AclMode		requiredPerms;	/* bitmask of required access permissions */
	Oid			checkAsUser;	/* if valid, check access as this role */
} RangeTblEntry;

/*
 * SortClause -
 *	   representation of ORDER BY clauses
 *
 * tleSortGroupRef must match ressortgroupref of exactly one entry of the
 * associated targetlist; that is the expression to be sorted (or grouped) by.
 * sortop is the OID of the ordering operator.
 *
 * SortClauses are also used to identify targets that we will do a "Unique"
 * filter step on (for SELECT DISTINCT and SELECT DISTINCT ON).  The
 * distinctClause list is simply a copy of the relevant members of the
 * sortClause list.  Note that distinctClause can be a subset of sortClause,
 * but cannot have members not present in sortClause; and the members that
 * do appear must be in the same order as in sortClause.
 */
typedef struct SortClause
{
	NodeTag		type;
	Index		tleSortGroupRef;	/* reference into targetlist */
	Oid			sortop;			/* the sort operator to use */
} SortClause;

/*
 * GroupClause -
 *	   representation of GROUP BY clauses
 *
 * GroupClause is exactly like SortClause except for the nodetag value
 * (it's probably not even really necessary to have two different
 * nodetags...).  We have routines that operate interchangeably on both.
 */
typedef SortClause GroupClause;


/*****************************************************************************
 *		Optimizable Statements
 *****************************************************************************/

/* ----------------------
 *		Insert Statement
 * ----------------------
 */
typedef struct InsertStmt
{
	NodeTag		type;
	RangeVar   *relation;		/* relation to insert into */
	List	   *cols;			/* optional: names of the target columns */

	/*
	 * An INSERT statement has *either* VALUES or SELECT, never both. If
	 * VALUES, a targetList is supplied (empty for DEFAULT VALUES). If SELECT,
	 * a complete SelectStmt (or set-operation tree) is supplied.
	 */
	List	   *targetList;		/* the target list (of ResTarget) */
	Node	   *selectStmt;		/* the source SELECT */
} InsertStmt;

/* ----------------------
 *		Delete Statement
 * ----------------------
 */
typedef struct DeleteStmt
{
	NodeTag		type;
	RangeVar   *relation;		/* relation to delete from */
	Node	   *whereClause;	/* qualifications */
	List	   *usingClause;	/* optional using clause for more tables */
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
} UpdateStmt;

/* ----------------------
 *		Select Statement
 *
 * A "simple" SELECT is represented in the output of gram.y by a single
 * SelectStmt node.  A SELECT construct containing set operators (UNION,
 * INTERSECT, EXCEPT) is represented by a tree of SelectStmt nodes, in
 * which the leaf nodes are component SELECTs and the internal nodes
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
	SETOP_EXCEPT
} SetOperation;

typedef enum ContainsOids
{
	MUST_HAVE_OIDS,				/* WITH OIDS explicitely specified */
	MUST_NOT_HAVE_OIDS,			/* WITHOUT OIDS explicitely specified */
	DEFAULT_OIDS				/* neither specified; use the default, which
								 * is the value of the default_with_oids GUC
								 * var */
} ContainsOids;

typedef struct SelectStmt
{
	NodeTag		type;

	/*
	 * These fields are used only in "leaf" SelectStmts.
	 *
	 * into, intoColNames and intoHasOids are a kluge; they belong somewhere
	 * else...
	 */
	List	   *distinctClause; /* NULL, list of DISTINCT ON exprs, or
								 * lcons(NIL,NIL) for all (SELECT DISTINCT) */
	RangeVar   *into;			/* target table (for select into table) */
	List	   *intoColNames;	/* column names for into table */
	ContainsOids intoHasOids;	/* should target table have OIDs? */
	List	   *targetList;		/* the target list (of ResTarget) */
	List	   *fromClause;		/* the FROM clause */
	Node	   *whereClause;	/* WHERE qualification */
	List	   *groupClause;	/* GROUP BY clauses */
	Node	   *havingClause;	/* HAVING conditional-expression */

	/*
	 * These fields are used in both "leaf" SelectStmts and upper-level
	 * SelectStmts.
	 */
	List	   *sortClause;		/* sort clause (a list of SortBy's) */
	Node	   *limitOffset;	/* # of result tuples to skip */
	Node	   *limitCount;		/* # of result tuples to return */
	LockingClause *lockingClause;		/* FOR UPDATE/FOR SHARE */

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
 * nodes replaced by SetOperationStmt nodes.
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

	/* Fields derived during parse analysis: */
	List	   *colTypes;		/* list of OIDs of output column types */
} SetOperationStmt;


/*****************************************************************************
 *		Other Statements (no optimizations required)
 *
 *		Some of them require a little bit of transformation (which is also
 *		done by transformStmt). The whole structure is then passed on to
 *		ProcessUtility (by-passing the optimization step) as the utilityStmt
 *		field in Query.
 *****************************************************************************/

/*
 * When a command can act on several kinds of objects with only one
 * parse structure required, use these constants to designate the
 * object type.
 */

typedef enum ObjectType
{
	OBJECT_AGGREGATE,
	OBJECT_CAST,
	OBJECT_COLUMN,
	OBJECT_CONSTRAINT,
	OBJECT_CONVERSION,
	OBJECT_DATABASE,
	OBJECT_DOMAIN,
	OBJECT_FUNCTION,
	OBJECT_INDEX,
	OBJECT_LANGUAGE,
	OBJECT_LARGEOBJECT,
	OBJECT_OPCLASS,
	OBJECT_OPERATOR,
	OBJECT_ROLE,
	OBJECT_RULE,
	OBJECT_SCHEMA,
	OBJECT_SEQUENCE,
	OBJECT_TABLE,
	OBJECT_TABLESPACE,
	OBJECT_TRIGGER,
	OBJECT_TYPE,
	OBJECT_VIEW
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
	char	   *authid;			/* the owner of the created schema */
	List	   *schemaElts;		/* schema components (list of parsenodes) */
} CreateSchemaStmt;

typedef enum DropBehavior
{
	DROP_RESTRICT,				/* drop fails if any dependent objects */
	DROP_CASCADE				/* remove dependent objects too */
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
	ObjectType	relkind;		/* type of object */
} AlterTableStmt;

typedef enum AlterTableType
{
	AT_AddColumn,				/* add column */
	AT_ColumnDefault,			/* alter column default */
	AT_DropNotNull,				/* alter column drop not null */
	AT_SetNotNull,				/* alter column set not null */
	AT_SetStatistics,			/* alter column statistics */
	AT_SetStorage,				/* alter column storage */
	AT_DropColumn,				/* drop column */
	AT_DropColumnRecurse,		/* internal to commands/tablecmds.c */
	AT_AddIndex,				/* add index */
	AT_ReAddIndex,				/* internal to commands/tablecmds.c */
	AT_AddConstraint,			/* add constraint */
	AT_ProcessedConstraint,		/* pre-processed add constraint (local in
								 * parser/analyze.c) */
	AT_DropConstraint,			/* drop constraint */
	AT_DropConstraintQuietly,	/* drop constraint, no error/warning (local in
								 * commands/tablecmds.c) */
	AT_AlterColumnType,			/* alter column type */
	AT_ToastTable,				/* create toast table */
	AT_ChangeOwner,				/* change owner */
	AT_ClusterOn,				/* CLUSTER ON */
	AT_DropCluster,				/* SET WITHOUT CLUSTER */
	AT_DropOids,				/* SET WITHOUT OIDS */
	AT_SetTableSpace,			/* SET TABLESPACE */
	AT_EnableTrig,				/* ENABLE TRIGGER name */
	AT_DisableTrig,				/* DISABLE TRIGGER name */
	AT_EnableTrigAll,			/* ENABLE TRIGGER ALL */
	AT_DisableTrigAll,			/* DISABLE TRIGGER ALL */
	AT_EnableTrigUser,			/* ENABLE TRIGGER USER */
	AT_DisableTrigUser			/* DISABLE TRIGGER USER */
} AlterTableType;

typedef struct AlterTableCmd	/* one subcommand of an ALTER TABLE */
{
	NodeTag		type;
	AlterTableType subtype;		/* Type of table alteration to apply */
	char	   *name;			/* column, constraint, or trigger to act on,
								 * or new owner or tablespace */
	Node	   *def;			/* definition of new column, column type,
								 * index, or constraint */
	Node	   *transform;		/* transformation expr for ALTER TYPE */
	DropBehavior behavior;		/* RESTRICT or CASCADE for DROP cases */
} AlterTableCmd;


/* ----------------------
 *	Alter Domain
 *
 * The fields are used in different ways by the different variants of
 * this command.
 * ----------------------
 */
typedef struct AlterDomainStmt
{
	NodeTag		type;
	char		subtype;		/*------------
								 *	T = alter column default
								 *	N = alter column drop not null
								 *	O = alter column set not null
								 *	C = add constraint
								 *	X = drop constraint
								 *------------
								 */
	List	   *typename;		/* domain to work on */
	char	   *name;			/* column or constraint name to act on */
	Node	   *def;			/* definition of default or constraint */
	DropBehavior behavior;		/* RESTRICT or CASCADE for DROP cases */
} AlterDomainStmt;


/* ----------------------
 *		Grant|Revoke Statement
 * ----------------------
 */
typedef enum GrantObjectType
{
	ACL_OBJECT_RELATION,		/* table, view, sequence */
	ACL_OBJECT_DATABASE,		/* database */
	ACL_OBJECT_FUNCTION,		/* function */
	ACL_OBJECT_LANGUAGE,		/* procedural language */
	ACL_OBJECT_NAMESPACE,		/* namespace */
	ACL_OBJECT_TABLESPACE		/* tablespace */
} GrantObjectType;

typedef struct GrantStmt
{
	NodeTag		type;
	bool		is_grant;		/* true = GRANT, false = REVOKE */
	GrantObjectType objtype;	/* kind of object being operated on */
	List	   *objects;		/* list of RangeVar nodes, FuncWithArgs nodes,
								 * or plain names (as Value strings) */
	List	   *privileges;		/* list of privilege names (as Strings) */
	/* privileges == NIL denotes "all privileges" */
	List	   *grantees;		/* list of PrivGrantee nodes */
	bool		grant_option;	/* grant or revoke grant option */
	DropBehavior behavior;		/* drop behavior (for REVOKE) */
} GrantStmt;

typedef struct PrivGrantee
{
	NodeTag		type;
	char	   *rolname;		/* if NULL then PUBLIC */
} PrivGrantee;

/*
 * Note: FuncWithArgs carries only the types of the input parameters of the
 * function.  So it is sufficient to identify an existing function, but it
 * is not enough info to define a function nor to call it.
 */
typedef struct FuncWithArgs
{
	NodeTag		type;
	List	   *funcname;		/* qualified name of function */
	List	   *funcargs;		/* list of Typename nodes */
} FuncWithArgs;

/* This is only used internally in gram.y. */
typedef struct PrivTarget
{
	NodeTag		type;
	GrantObjectType objtype;
	List	   *objs;
} PrivTarget;

/* ----------------------
 *		Grant/Revoke Role Statement
 *
 * Note: the lists of roles are lists of names, as Value strings
 * ----------------------
 */
typedef struct GrantRoleStmt
{
	NodeTag		type;
	List	   *granted_roles;	/* list of roles to be granted/revoked */
	List	   *grantee_roles;	/* list of member roles to add/delete */
	bool		is_grant;		/* true = GRANT, false = REVOKE */
	bool		admin_opt;		/* with admin option */
	char	   *grantor;		/* set grantor to other than current role */
	DropBehavior behavior;		/* drop behavior (for REVOKE) */
} GrantRoleStmt;

/* ----------------------
 *		Copy Statement
 * ----------------------
 */
typedef struct CopyStmt
{
	NodeTag		type;
	RangeVar   *relation;		/* the relation to copy */
	List	   *attlist;		/* List of column names (as Strings), or NIL
								 * for all columns */
	bool		is_from;		/* TO or FROM */
	char	   *filename;		/* if NULL, use stdin/stdout */
	List	   *options;		/* List of DefElem nodes */
} CopyStmt;

/* ----------------------
 *		Create Table Statement
 *
 * NOTE: in the raw gram.y output, ColumnDef, Constraint, and FkConstraint
 * nodes are intermixed in tableElts, and constraints is NIL.  After parse
 * analysis, tableElts contains just ColumnDefs, and constraints contains
 * just Constraint nodes (in fact, only CONSTR_CHECK nodes, in the present
 * implementation).
 * ----------------------
 */

/* What to do at commit time for temporary relations */
typedef enum OnCommitAction
{
	ONCOMMIT_NOOP,				/* No ON COMMIT clause (do nothing) */
	ONCOMMIT_PRESERVE_ROWS,		/* ON COMMIT PRESERVE ROWS (do nothing) */
	ONCOMMIT_DELETE_ROWS,		/* ON COMMIT DELETE ROWS */
	ONCOMMIT_DROP				/* ON COMMIT DROP */
} OnCommitAction;

typedef struct CreateStmt
{
	NodeTag		type;
	RangeVar   *relation;		/* relation to create */
	List	   *tableElts;		/* column definitions (list of ColumnDef) */
	List	   *inhRelations;	/* relations to inherit from (list of
								 * inhRelation) */
	List	   *constraints;	/* constraints (list of Constraint nodes) */
	ContainsOids hasoids;		/* should it have OIDs? */
	OnCommitAction oncommit;	/* what do we do at COMMIT? */
	char	   *tablespacename; /* table space to use, or NULL */
} CreateStmt;

/* ----------
 * Definitions for plain (non-FOREIGN KEY) constraints in CreateStmt
 *
 * XXX probably these ought to be unified with FkConstraints at some point?
 * To this end we include CONSTR_FOREIGN in the ConstrType enum, even though
 * the parser does not generate it.
 *
 * For constraints that use expressions (CONSTR_DEFAULT, CONSTR_CHECK)
 * we may have the expression in either "raw" form (an untransformed
 * parse tree) or "cooked" form (the nodeToString representation of
 * an executable expression tree), depending on how this Constraint
 * node was created (by parsing, or by inheritance from an existing
 * relation).  We should never have both in the same node!
 *
 * Constraint attributes (DEFERRABLE etc) are initially represented as
 * separate Constraint nodes for simplicity of parsing.  analyze.c makes
 * a pass through the constraints list to attach the info to the appropriate
 * FkConstraint node (and, perhaps, someday to other kinds of constraints).
 * ----------
 */

typedef enum ConstrType			/* types of constraints */
{
	CONSTR_NULL,				/* not SQL92, but a lot of people expect it */
	CONSTR_NOTNULL,
	CONSTR_DEFAULT,
	CONSTR_CHECK,
	CONSTR_FOREIGN,
	CONSTR_PRIMARY,
	CONSTR_UNIQUE,
	CONSTR_ATTR_DEFERRABLE,		/* attributes for previous constraint node */
	CONSTR_ATTR_NOT_DEFERRABLE,
	CONSTR_ATTR_DEFERRED,
	CONSTR_ATTR_IMMEDIATE
} ConstrType;

typedef struct Constraint
{
	NodeTag		type;
	ConstrType	contype;
	char	   *name;			/* name, or NULL if unnamed */
	Node	   *raw_expr;		/* expr, as untransformed parse tree */
	char	   *cooked_expr;	/* expr, as nodeToString representation */
	List	   *keys;			/* String nodes naming referenced column(s) */
	char	   *indexspace;		/* index tablespace for PKEY/UNIQUE
								 * constraints; NULL for default */
} Constraint;

/* ----------
 * Definitions for FOREIGN KEY constraints in CreateStmt
 *
 * Note: FKCONSTR_ACTION_xxx values are stored into pg_constraint.confupdtype
 * and pg_constraint.confdeltype columns; FKCONSTR_MATCH_xxx values are
 * stored into pg_constraint.confmatchtype.  Changing the code values may
 * require an initdb!
 *
 * If skip_validation is true then we skip checking that the existing rows
 * in the table satisfy the constraint, and just install the catalog entries
 * for the constraint.	This is currently used only during CREATE TABLE
 * (when we know the table must be empty).
 * ----------
 */
#define FKCONSTR_ACTION_NOACTION	'a'
#define FKCONSTR_ACTION_RESTRICT	'r'
#define FKCONSTR_ACTION_CASCADE		'c'
#define FKCONSTR_ACTION_SETNULL		'n'
#define FKCONSTR_ACTION_SETDEFAULT	'd'

#define FKCONSTR_MATCH_FULL			'f'
#define FKCONSTR_MATCH_PARTIAL		'p'
#define FKCONSTR_MATCH_UNSPECIFIED	'u'

typedef struct FkConstraint
{
	NodeTag		type;
	char	   *constr_name;	/* Constraint name, or NULL if unnamed */
	RangeVar   *pktable;		/* Primary key table */
	List	   *fk_attrs;		/* Attributes of foreign key */
	List	   *pk_attrs;		/* Corresponding attrs in PK table */
	char		fk_matchtype;	/* FULL, PARTIAL, UNSPECIFIED */
	char		fk_upd_action;	/* ON UPDATE action */
	char		fk_del_action;	/* ON DELETE action */
	bool		deferrable;		/* DEFERRABLE */
	bool		initdeferred;	/* INITIALLY DEFERRED */
	bool		skip_validation;	/* skip validation of existing rows? */
} FkConstraint;


/* ----------------------
 *		Create/Drop Table Space Statements
 * ----------------------
 */

typedef struct CreateTableSpaceStmt
{
	NodeTag		type;
	char	   *tablespacename;
	char	   *owner;
	char	   *location;
} CreateTableSpaceStmt;

typedef struct DropTableSpaceStmt
{
	NodeTag		type;
	char	   *tablespacename;
} DropTableSpaceStmt;

/* ----------------------
 *		Create/Drop TRIGGER Statements
 * ----------------------
 */

typedef struct CreateTrigStmt
{
	NodeTag		type;
	char	   *trigname;		/* TRIGGER's name */
	RangeVar   *relation;		/* relation trigger is on */
	List	   *funcname;		/* qual. name of function to call */
	List	   *args;			/* list of (T_String) Values or NIL */
	bool		before;			/* BEFORE/AFTER */
	bool		row;			/* ROW/STATEMENT */
	char		actions[4];		/* 1 to 3 of 'i', 'u', 'd', + trailing \0 */

	/* The following are used for referential */
	/* integrity constraint triggers */
	bool		isconstraint;	/* This is an RI trigger */
	bool		deferrable;		/* [NOT] DEFERRABLE */
	bool		initdeferred;	/* INITIALLY {DEFERRED|IMMEDIATE} */
	RangeVar   *constrrel;		/* opposite relation */
} CreateTrigStmt;

/* ----------------------
 *		Create/Drop PROCEDURAL LANGUAGE Statement
 * ----------------------
 */
typedef struct CreatePLangStmt
{
	NodeTag		type;
	char	   *plname;			/* PL name */
	List	   *plhandler;		/* PL call handler function (qual. name) */
	List	   *plvalidator;	/* optional validator function (qual. name) */
	bool		pltrusted;		/* PL is trusted */
} CreatePLangStmt;

typedef struct DropPLangStmt
{
	NodeTag		type;
	char	   *plname;			/* PL name */
	DropBehavior behavior;		/* RESTRICT or CASCADE behavior */
} DropPLangStmt;

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
	ROLESTMT_GROUP
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
	char	   *role;			/* role name */
	List	   *options;		/* List of DefElem nodes */
	int			action;			/* +1 = add members, -1 = drop members */
} AlterRoleStmt;

typedef struct AlterRoleSetStmt
{
	NodeTag		type;
	char	   *role;			/* role name */
	char	   *variable;		/* GUC variable name */
	List	   *value;			/* value for variable, or NIL for Reset */
} AlterRoleSetStmt;

typedef struct DropRoleStmt
{
	NodeTag		type;
	List	   *roles;			/* List of roles to remove */
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
} CreateSeqStmt;

typedef struct AlterSeqStmt
{
	NodeTag		type;
	RangeVar   *sequence;		/* the sequence to alter */
	List	   *options;
} AlterSeqStmt;

/* ----------------------
 *		Create {Aggregate|Operator|Type} Statement
 * ----------------------
 */
typedef struct DefineStmt
{
	NodeTag		type;
	ObjectType	kind;			/* aggregate, operator, type */
	List	   *defnames;		/* qualified name (list of Value strings) */
	List	   *definition;		/* a list of DefElem */
} DefineStmt;

/* ----------------------
 *		Create Domain Statement
 * ----------------------
 */
typedef struct CreateDomainStmt
{
	NodeTag		type;
	List	   *domainname;		/* qualified name (list of Value strings) */
	TypeName   *typename;		/* the base type */
	List	   *constraints;	/* constraints (list of Constraint nodes) */
} CreateDomainStmt;

/* ----------------------
 *		Create Operator Class Statement
 * ----------------------
 */
typedef struct CreateOpClassStmt
{
	NodeTag		type;
	List	   *opclassname;	/* qualified name (list of Value strings) */
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
	/* fields used for an operator or function item: */
	List	   *name;			/* operator or function name */
	List	   *args;			/* argument types */
	int			number;			/* strategy num or support proc num */
	bool		recheck;		/* only used for operators */
	/* fields used for a storagetype item: */
	TypeName   *storedtype;		/* datatype stored in index */
} CreateOpClassItem;

/* ----------------------
 *		Drop Table|Sequence|View|Index|Type|Domain|Conversion|Schema Statement
 * ----------------------
 */

typedef struct DropStmt
{
	NodeTag		type;
	List	   *objects;		/* list of sublists of names (as Values) */
	ObjectType	removeType;		/* object type */
	DropBehavior behavior;		/* RESTRICT or CASCADE behavior */
} DropStmt;

/* ----------------------
 *		Drop Rule|Trigger Statement
 *
 * In general this may be used for dropping any property of a relation;
 * for example, someday soon we may have DROP ATTRIBUTE.
 * ----------------------
 */

typedef struct DropPropertyStmt
{
	NodeTag		type;
	RangeVar   *relation;		/* owning relation */
	char	   *property;		/* name of rule, trigger, etc */
	ObjectType	removeType;		/* OBJECT_RULE or OBJECT_TRIGGER */
	DropBehavior behavior;		/* RESTRICT or CASCADE behavior */
} DropPropertyStmt;

/* ----------------------
 *				Truncate Table Statement
 * ----------------------
 */
typedef struct TruncateStmt
{
	NodeTag		type;
	List	   *relations;		/* relations (RangeVars) to be truncated */
} TruncateStmt;

/* ----------------------
 *				Comment On Statement
 * ----------------------
 */
typedef struct CommentStmt
{
	NodeTag		type;
	ObjectType	objtype;		/* Object's type */
	List	   *objname;		/* Qualified name of the object */
	List	   *objargs;		/* Arguments if needed (eg, for functions) */
	char	   *comment;		/* Comment to insert, or NULL to remove */
} CommentStmt;

/* ----------------------
 *		Declare Cursor Statement
 * ----------------------
 */
#define CURSOR_OPT_BINARY		0x0001
#define CURSOR_OPT_SCROLL		0x0002
#define CURSOR_OPT_NO_SCROLL	0x0004
#define CURSOR_OPT_INSENSITIVE	0x0008
#define CURSOR_OPT_HOLD			0x0010

typedef struct DeclareCursorStmt
{
	NodeTag		type;
	char	   *portalname;		/* name of the portal (cursor) */
	int			options;		/* bitmask of options (see above) */
	Node	   *query;			/* the SELECT query */
} DeclareCursorStmt;

/* ----------------------
 *		Close Portal Statement
 * ----------------------
 */
typedef struct ClosePortalStmt
{
	NodeTag		type;
	char	   *portalname;		/* name of the portal (cursor) */
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
	FETCH_RELATIVE
} FetchDirection;

#define FETCH_ALL	LONG_MAX

typedef struct FetchStmt
{
	NodeTag		type;
	FetchDirection direction;	/* see above */
	long		howMany;		/* number of rows, or position argument */
	char	   *portalname;		/* name of portal (cursor) */
	bool		ismove;			/* TRUE if MOVE */
} FetchStmt;

/* ----------------------
 *		Create Index Statement
 * ----------------------
 */
typedef struct IndexStmt
{
	NodeTag		type;
	char	   *idxname;		/* name of new index, or NULL for default */
	RangeVar   *relation;		/* relation to build index on */
	char	   *accessMethod;	/* name of access method (eg. btree) */
	char	   *tableSpace;		/* tablespace, or NULL to use parent's */
	List	   *indexParams;	/* a list of IndexElem */
	Node	   *whereClause;	/* qualification (partial-index predicate) */
	List	   *rangetable;		/* range table for qual and/or expressions,
								 * filled in by transformStmt() */
	bool		unique;			/* is index unique? */
	bool		primary;		/* is index on primary key? */
	bool		isconstraint;	/* is it from a CONSTRAINT clause? */
} IndexStmt;

/* ----------------------
 *		Create Function Statement
 * ----------------------
 */
typedef struct CreateFunctionStmt
{
	NodeTag		type;
	bool		replace;		/* T => replace if already exists */
	List	   *funcname;		/* qualified name of function to create */
	List	   *parameters;		/* a list of FunctionParameter */
	TypeName   *returnType;		/* the return type */
	List	   *options;		/* a list of DefElem */
	List	   *withClause;		/* a list of DefElem */
} CreateFunctionStmt;

typedef enum FunctionParameterMode
{
	/* the assigned enum values appear in pg_proc, don't change 'em! */
	FUNC_PARAM_IN = 'i',		/* input only */
	FUNC_PARAM_OUT = 'o',		/* output only */
	FUNC_PARAM_INOUT = 'b'		/* both */
} FunctionParameterMode;

typedef struct FunctionParameter
{
	NodeTag		type;
	char	   *name;			/* parameter name, or NULL if not given */
	TypeName   *argType;		/* TypeName for parameter type */
	FunctionParameterMode mode; /* IN/OUT/INOUT */
} FunctionParameter;

typedef struct AlterFunctionStmt
{
	NodeTag		type;
	FuncWithArgs *func;			/* name and args of function */
	List	   *actions;		/* list of DefElem */
} AlterFunctionStmt;

/* ----------------------
 *		Drop Aggregate Statement
 * ----------------------
 */
typedef struct RemoveAggrStmt
{
	NodeTag		type;
	List	   *aggname;		/* aggregate to drop */
	TypeName   *aggtype;		/* TypeName for input datatype, or NULL */
	DropBehavior behavior;		/* RESTRICT or CASCADE behavior */
} RemoveAggrStmt;

/* ----------------------
 *		Drop Function Statement
 * ----------------------
 */
typedef struct RemoveFuncStmt
{
	NodeTag		type;
	List	   *funcname;		/* function to drop */
	List	   *args;			/* types of the arguments */
	DropBehavior behavior;		/* RESTRICT or CASCADE behavior */
} RemoveFuncStmt;

/* ----------------------
 *		Drop Operator Statement
 * ----------------------
 */
typedef struct RemoveOperStmt
{
	NodeTag		type;
	List	   *opname;			/* operator to drop */
	List	   *args;			/* types of the arguments */
	DropBehavior behavior;		/* RESTRICT or CASCADE behavior */
} RemoveOperStmt;

/* ----------------------
 *		Drop Operator Class Statement
 * ----------------------
 */
typedef struct RemoveOpClassStmt
{
	NodeTag		type;
	List	   *opclassname;	/* qualified name (list of Value strings) */
	char	   *amname;			/* name of index AM opclass is for */
	DropBehavior behavior;		/* RESTRICT or CASCADE behavior */
} RemoveOpClassStmt;

/* ----------------------
 *		Alter Object Rename Statement
 * ----------------------
 */
typedef struct RenameStmt
{
	NodeTag		type;
	ObjectType	renameType;		/* OBJECT_TABLE, OBJECT_COLUMN, etc */
	RangeVar   *relation;		/* in case it's a table */
	List	   *object;			/* in case it's some other object */
	List	   *objarg;			/* argument types, if applicable */
	char	   *subname;		/* name of contained object (column, rule,
								 * trigger, etc) */
	char	   *newname;		/* the new name */
} RenameStmt;

/* ----------------------
 *		ALTER object SET SCHEMA Statement
 * ----------------------
 */
typedef struct AlterObjectSchemaStmt
{
	NodeTag		type;
	ObjectType objectType;		/* OBJECT_TABLE, OBJECT_TYPE, etc */
	RangeVar   *relation;		/* in case it's a table */
	List	   *object;			/* in case it's some other object */
	List	   *objarg;			/* argument types, if applicable */
	char	   *addname;		/* additional name if needed */
	char	   *newschema;		/* the new schema */
} AlterObjectSchemaStmt;

/* ----------------------
 *		Alter Object Owner Statement
 * ----------------------
 */
typedef struct AlterOwnerStmt
{
	NodeTag		type;
	ObjectType objectType;		/* OBJECT_TABLE, OBJECT_TYPE, etc */
	RangeVar   *relation;		/* in case it's a table */
	List	   *object;			/* in case it's some other object */
	List	   *objarg;			/* argument types, if applicable */
	char	   *addname;		/* additional name if needed */
	char	   *newowner;		/* the new owner */
} AlterOwnerStmt;


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
	RangeVar   *relation;		/* qualified name to notify */
} NotifyStmt;

/* ----------------------
 *		Listen Statement
 * ----------------------
 */
typedef struct ListenStmt
{
	NodeTag		type;
	RangeVar   *relation;		/* qualified name to listen on */
} ListenStmt;

/* ----------------------
 *		Unlisten Statement
 * ----------------------
 */
typedef struct UnlistenStmt
{
	NodeTag		type;
	RangeVar   *relation;		/* qualified name to unlisten on, or '*' */
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
	TRANS_STMT_ROLLBACK_PREPARED
} TransactionStmtKind;

typedef struct TransactionStmt
{
	NodeTag		type;
	TransactionStmtKind kind;	/* see above */
	List	   *options;		/* for BEGIN/START and savepoint commands */
	char	   *gid;			/* for two-phase-commit related commands */
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
 *		Create View Statement
 * ----------------------
 */
typedef struct ViewStmt
{
	NodeTag		type;
	RangeVar   *view;			/* the view to be created */
	List	   *aliases;		/* target column names */
	Query	   *query;			/* the SQL statement */
	bool		replace;		/* replace an existing view? */
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

typedef struct AlterDatabaseSetStmt
{
	NodeTag		type;
	char	   *dbname;
	char	   *variable;
	List	   *value;
} AlterDatabaseSetStmt;

/* ----------------------
 *		Dropdb Statement
 * ----------------------
 */
typedef struct DropdbStmt
{
	NodeTag		type;
	char	   *dbname;			/* database to drop */
} DropdbStmt;

/* ----------------------
 *		Cluster Statement (support pbrown's cluster index implementation)
 * ----------------------
 */
typedef struct ClusterStmt
{
	NodeTag		type;
	RangeVar   *relation;		/* relation being indexed, or NULL if all */
	char	   *indexname;		/* original index defined */
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
	bool		vacuum;			/* do VACUUM step */
	bool		full;			/* do FULL (non-concurrent) vacuum */
	bool		analyze;		/* do ANALYZE step */
	bool		freeze;			/* early-freeze option */
	bool		verbose;		/* print progress info */
	RangeVar   *relation;		/* single table to process, or NULL */
	List	   *va_cols;		/* list of column names, or NIL for all */
} VacuumStmt;

/* ----------------------
 *		Explain Statement
 * ----------------------
 */
typedef struct ExplainStmt
{
	NodeTag		type;
	Query	   *query;			/* the query */
	bool		verbose;		/* print plan info */
	bool		analyze;		/* get statistics by executing plan */
} ExplainStmt;

/* ----------------------
 * Checkpoint Statement
 * ----------------------
 */
typedef struct CheckPointStmt
{
	NodeTag		type;
} CheckPointStmt;

/* ----------------------
 * Set Statement
 * ----------------------
 */

typedef struct VariableSetStmt
{
	NodeTag		type;
	char	   *name;
	List	   *args;
	bool		is_local;		/* SET LOCAL */
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
 * Reset Statement
 * ----------------------
 */

typedef struct VariableResetStmt
{
	NodeTag		type;
	char	   *name;
} VariableResetStmt;

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
	List	   *constraints;	/* List of names as Value strings */
	bool		deferred;
} ConstraintsSetStmt;

/* ----------------------
 *		REINDEX Statement
 * ----------------------
 */
typedef struct ReindexStmt
{
	NodeTag		type;
	ObjectType	kind;			/* OBJECT_INDEX, OBJECT_TABLE, OBJECT_DATABASE */
	RangeVar   *relation;		/* Table or index to reindex */
	const char *name;			/* name of database to reindex */
	bool		do_system;		/* include system tables in database case */
	bool		do_user;		/* include user tables in database case */
} ReindexStmt;

/* ----------------------
 *		CREATE CONVERSION Statement
 * ----------------------
 */
typedef struct CreateConversionStmt
{
	NodeTag		type;
	List	   *conversion_name;	/* Name of the conversion */
	char	   *for_encoding_name;		/* source encoding name */
	char	   *to_encoding_name;		/* destination encoding name */
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
	FuncWithArgs *func;
	CoercionContext context;
} CreateCastStmt;

/* ----------------------
 *	DROP CAST Statement
 * ----------------------
 */
typedef struct DropCastStmt
{
	NodeTag		type;
	TypeName   *sourcetype;
	TypeName   *targettype;
	DropBehavior behavior;
} DropCastStmt;


/* ----------------------
 *		PREPARE Statement
 * ----------------------
 */
typedef struct PrepareStmt
{
	NodeTag		type;
	char	   *name;			/* Name of plan, arbitrary */
	List	   *argtypes;		/* Types of parameters (TypeNames) */
	List	   *argtype_oids;	/* Types of parameters (OIDs) */
	Query	   *query;			/* The query itself */
} PrepareStmt;


/* ----------------------
 *		EXECUTE Statement
 * ----------------------
 */

typedef struct ExecuteStmt
{
	NodeTag		type;
	char	   *name;			/* The name of the plan to execute */
	RangeVar   *into;			/* Optional table to store results in */
	List	   *params;			/* Values to assign to parameters */
} ExecuteStmt;


/* ----------------------
 *		DEALLOCATE Statement
 * ----------------------
 */
typedef struct DeallocateStmt
{
	NodeTag		type;
	char	   *name;			/* The name of the plan to remove */
} DeallocateStmt;

#endif   /* PARSENODES_H */
