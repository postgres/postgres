/*-------------------------------------------------------------------------
 *
 * parsenodes.h
 *	  definitions for parse tree nodes
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parsenodes.h,v 1.124 2001/01/24 19:43:25 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSENODES_H
#define PARSENODES_H

#include "nodes/primnodes.h"

/*****************************************************************************
 *	Query Tree
 *****************************************************************************/

/*
 * Query -
 *	  all statments are turned into a Query tree (via transformStmt)
 *	  for further processing by the optimizer
 *	  utility statements (i.e. non-optimizable statements)
 *	  have the *utilityStmt field set.
 *
 * we need the isPortal flag because portal names can be null too; can
 * get rid of it if we support CURSOR as a commandType.
 */
typedef struct Query
{
	NodeTag		type;

	CmdType		commandType;	/* select|insert|update|delete|utility */

	Node	   *utilityStmt;	/* non-null if this is a non-optimizable
								 * statement */

	int			resultRelation; /* target relation (index into rtable) */
	char	   *into;			/* portal (cursor) name */
	bool		isPortal;		/* is this a retrieve into portal? */
	bool		isBinary;		/* binary portal? */
	bool		isTemp;			/* is 'into' a temp table? */

	bool		hasAggs;		/* has aggregates in tlist or havingQual */
	bool		hasSubLinks;	/* has subquery SubLink */

	List	   *rtable;			/* list of range table entries */
	FromExpr   *jointree;		/* table join tree (FROM and WHERE clauses) */

	List	   *rowMarks;		/* integer list of RT indexes of relations
								 * that are selected FOR UPDATE */

	List	   *targetList;		/* target list (of TargetEntry) */

	List	   *groupClause;	/* a list of GroupClause's */

	Node	   *havingQual;		/* qualifications applied to groups */

	List	   *distinctClause; /* a list of SortClause's */

	List	   *sortClause;		/* a list of SortClause's */

	Node	   *limitOffset;	/* # of result tuples to skip */
	Node	   *limitCount;		/* # of result tuples to return */

	Node	   *setOperations;	/* set-operation tree if this is top level
								 * of a UNION/INTERSECT/EXCEPT query */

	/*
	 * If the resultRelation turns out to be the parent of an inheritance
	 * tree, the planner will add all the child tables to the rtable and
	 * store a list of the rtindexes of all the result relations here.
	 * This is done at plan time, not parse time, since we don't want to
	 * commit to the exact set of child tables at parse time.  This field
	 * ought to go in some sort of TopPlan plan node, not in the Query.
	 */
	List	   *resultRelations; /* integer list of RT indexes, or NIL */

	/* internal to planner */
	List	   *base_rel_list;	/* list of base-relation RelOptInfos */
	List	   *join_rel_list;	/* list of join-relation RelOptInfos */
	List	   *equi_key_list;	/* list of lists of equijoined
								 * PathKeyItems */
	List	   *query_pathkeys; /* pathkeys for query_planner()'s result */
} Query;


typedef enum InhOption
{
	INH_NO,						/* Do NOT scan child tables */
	INH_YES,					/* DO scan child tables */
	INH_DEFAULT					/* Use current SQL_inheritance option */
} InhOption;

/*****************************************************************************
 *		Other Statements (no optimizations required)
 *
 *		Some of them require a little bit of transformation (which is also
 *		done by transformStmt). The whole structure is then passed on to
 *		ProcessUtility (by-passing the optimization step) as the utilityStmt
 *		field in Query.
 *****************************************************************************/

/* ----------------------
 *	Alter Table
 *
 * The fields are used in different ways by the different variants of
 * this command.
 * ----------------------
 */
typedef struct AlterTableStmt
{
	NodeTag		type;
	char		subtype;		/* A = add column, T = alter column, D = drop column,
								 * C = add constraint, X = drop constraint,
								 * E = add toast table,
								 * U = change owner */
	char	   *relname;		/* table to work on */
	InhOption	inhOpt;			/* recursively act on children? */
	char	   *name;			/* column or constraint name to act on, or new owner */
	Node	   *def;			/* definition of new column or constraint */
	int			behavior;		/* CASCADE or RESTRICT drop behavior */
} AlterTableStmt;

/* ----------------------
 *		Change ACL Statement
 * ----------------------
 */
typedef struct ChangeACLStmt
{
	NodeTag		type;
	List	   *relNames;
	char	   *aclString;
} ChangeACLStmt;

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
 *		Copy Statement
 * ----------------------
 */
typedef struct CopyStmt
{
	NodeTag		type;
	bool		binary;			/* is a binary copy? */
	char	   *relname;		/* the relation to copy */
	bool		oids;			/* copy oid's? */
	int			direction;		/* TO or FROM */
	char	   *filename;		/* if NULL, use stdin/stdout */
	char	   *delimiter;		/* delimiter character, \t by default */
	char	   *null_print;		/* how to print NULLs, `\N' by default */
} CopyStmt;

/* ----------------------
 *		Create Table Statement
 * ----------------------
 */
typedef struct CreateStmt
{
	NodeTag		type;
	bool		istemp;			/* is this a temp table? */
	char	   *relname;		/* name of relation to create */
	List	   *tableElts;		/* column definitions (list of ColumnDef) */
	List	   *inhRelnames;	/* relations to inherit from (list of
								 * T_String Values) */
	List	   *constraints;	/* constraints (list of Constraint and
								 * FkConstraint nodes) */
} CreateStmt;

/* ----------
 * Definitions for plain (non-FOREIGN KEY) constraints in CreateStmt
 *
 * XXX probably these ought to be unified with FkConstraints at some point?
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
	CONSTR_NULL,				/* not SQL92, but a lot of people expect
								 * it */
	CONSTR_NOTNULL,
	CONSTR_DEFAULT,
	CONSTR_CHECK,
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
	List	   *keys;			/* Ident nodes naming referenced column(s) */
} Constraint;


/* ----------
 * Definitions for FOREIGN KEY constraints in CreateStmt
 * ----------
 */
#define FKCONSTR_ON_KEY_NOACTION		0x0000
#define FKCONSTR_ON_KEY_RESTRICT		0x0001
#define FKCONSTR_ON_KEY_CASCADE			0x0002
#define FKCONSTR_ON_KEY_SETNULL			0x0004
#define FKCONSTR_ON_KEY_SETDEFAULT		0x0008

#define FKCONSTR_ON_DELETE_MASK			0x000F
#define FKCONSTR_ON_DELETE_SHIFT		0

#define FKCONSTR_ON_UPDATE_MASK			0x00F0
#define FKCONSTR_ON_UPDATE_SHIFT		4

typedef struct FkConstraint
{
	NodeTag		type;
	char	   *constr_name;	/* Constraint name */
	char	   *pktable_name;	/* Primary key table name */
	List	   *fk_attrs;		/* Attributes of foreign key */
	List	   *pk_attrs;		/* Corresponding attrs in PK table */
	char	   *match_type;		/* FULL or PARTIAL */
	int32		actions;		/* ON DELETE/UPDATE actions */
	bool		deferrable;		/* DEFERRABLE */
	bool		initdeferred;	/* INITIALLY DEFERRED */
} FkConstraint;


/* ----------------------
 *		Create/Drop TRIGGER Statements
 * ----------------------
 */

typedef struct CreateTrigStmt
{
	NodeTag		type;
	char	   *trigname;		/* TRIGGER' name */
	char	   *relname;		/* triggered relation */
	char	   *funcname;		/* function to call (or NULL) */
	List	   *args;			/* list of (T_String) Values or NULL */
	bool		before;			/* BEFORE/AFTER */
	bool		row;			/* ROW/STATEMENT */
	char		actions[4];		/* Insert, Update, Delete */
	char	   *lang;			/* currently not used, always NULL */
	char	   *text;			/* AS 'text' */
	List	   *attr;			/* UPDATE OF a, b,... (NI) or NULL */
	char	   *when;			/* WHEN 'a > 10 ...' (NI) or NULL */

	/* The following are used for referential */
	/* integrity constraint triggers */
	bool		isconstraint;	/* This is an RI trigger */
	bool		deferrable;		/* [NOT] DEFERRABLE */
	bool		initdeferred;	/* INITIALLY {DEFERRED|IMMEDIATE} */
	char	   *constrrelname;	/* opposite relation */
} CreateTrigStmt;

typedef struct DropTrigStmt
{
	NodeTag		type;
	char	   *trigname;		/* TRIGGER' name */
	char	   *relname;		/* triggered relation */
} DropTrigStmt;


/* ----------------------
 *		Create/Drop PROCEDURAL LANGUAGE Statement
 * ----------------------
 */
typedef struct CreatePLangStmt
{
	NodeTag		type;
	char	   *plname;			/* PL name */
	char	   *plhandler;		/* PL call handler function */
	char	   *plcompiler;		/* lancompiler text */
	bool		pltrusted;		/* PL is trusted */
} CreatePLangStmt;

typedef struct DropPLangStmt
{
	NodeTag		type;
	char	   *plname;			/* PL name */
} DropPLangStmt;


/* ----------------------
 *				Create/Alter/Drop User Statements
 * ----------------------
 */
typedef struct CreateUserStmt
{
	NodeTag		type;
	char	   *user;			/* PostgreSQL user login			  */
	char	   *password;		/* PostgreSQL user password			  */
	int			sysid;			/* PgSQL system id (-1 if don't care) */
	bool		createdb;		/* Can the user create databases?	  */
	bool		createuser;		/* Can this user create users?		  */
	List	   *groupElts;		/* The groups the user is a member of */
	char	   *validUntil;		/* The time the login is valid until  */
} CreateUserStmt;

typedef struct AlterUserStmt
{
	NodeTag		type;
	char	   *user;			/* PostgreSQL user login			  */
	char	   *password;		/* PostgreSQL user password			  */
	int			createdb;		/* Can the user create databases?	  */
	int			createuser;		/* Can this user create users?		  */
	char	   *validUntil;		/* The time the login is valid until  */
} AlterUserStmt;

typedef struct DropUserStmt
{
	NodeTag		type;
	List	   *users;			/* List of users to remove */
} DropUserStmt;


/* ----------------------
 *		Create/Alter/Drop Group Statements
 * ----------------------
 */
typedef struct CreateGroupStmt
{
	NodeTag		type;
	char	   *name;			/* name of the new group */
	int			sysid;			/* group id (-1 if pick default) */
	List	   *initUsers;		/* list of initial users */
} CreateGroupStmt;

typedef struct AlterGroupStmt
{
	NodeTag		type;
	char	   *name;			/* name of group to alter */
	int			action;			/* +1 = add, -1 = drop user */
	int			sysid;			/* sysid change */
	List	   *listUsers;		/* list of users to add/drop */
} AlterGroupStmt;

typedef struct DropGroupStmt
{
	NodeTag		type;
	char	   *name;
} DropGroupStmt;


/* ----------------------
 *		Create SEQUENCE Statement
 * ----------------------
 */

typedef struct CreateSeqStmt
{
	NodeTag		type;
	char	   *seqname;		/* the relation to create */
	List	   *options;
} CreateSeqStmt;

/* ----------------------
 *		Create Version Statement
 * ----------------------
 */
typedef struct VersionStmt
{
	NodeTag		type;
	char	   *relname;		/* the new relation */
	int			direction;		/* FORWARD | BACKWARD */
	char	   *fromRelname;	/* relation to create a version */
	char	   *date;			/* date of the snapshot */
} VersionStmt;

/* ----------------------
 *		Create {Operator|Type|Aggregate} Statement
 * ----------------------
 */
typedef struct DefineStmt
{
	NodeTag		type;
	int			defType;		/* OPERATOR|P_TYPE|AGGREGATE */
	char	   *defname;
	List	   *definition;		/* a list of DefElem */
} DefineStmt;


/* ----------------------
 *		Drop Table|Sequence|View|Index|Rule|Type Statement
 * ----------------------
 */

#define DROP_TABLE    1
#define DROP_SEQUENCE 2
#define DROP_VIEW     3
#define DROP_INDEX    4
#define DROP_RULE     5
#define DROP_TYPE_P   6

typedef struct DropStmt
{
	NodeTag		type;
	List	   *names;
	int			removeType;
} DropStmt;

/* ----------------------
 *				Truncate Table Statement
 * ----------------------
 */
typedef struct TruncateStmt
{
	NodeTag		type;
	char	   *relName;		/* relation to be truncated */
} TruncateStmt;

/* ----------------------
 *				Comment On Statement
 * ----------------------
 */
typedef struct CommentStmt
{
	NodeTag		type;
	int			objtype;		/* Object's type */
	char	   *objname;		/* Name of the object */
	char	   *objproperty;	/* Property Id (such as column) */
	List	   *objlist;		/* Arguments for VAL objects */
	char	   *comment;		/* The comment to insert */
} CommentStmt;

/* ----------------------
 *		Extend Index Statement
 * ----------------------
 */
typedef struct ExtendStmt
{
	NodeTag		type;
	char	   *idxname;		/* name of the index */
	Node	   *whereClause;	/* qualifications */
	List	   *rangetable;		/* range table, filled in by
								 * transformStmt() */
} ExtendStmt;

/* ----------------------
 *		Begin Recipe Statement
 * ----------------------
 */
typedef struct RecipeStmt
{
	NodeTag		type;
	char	   *recipeName;		/* name of the recipe */
} RecipeStmt;

/* ----------------------
 *		Fetch Statement
 * ----------------------
 */
typedef struct FetchStmt
{
	NodeTag		type;
	int			direction;		/* FORWARD or BACKWARD */
	int			howMany;		/* amount to fetch ("ALL" --> 0) */
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
	char	   *idxname;		/* name of the index */
	char	   *relname;		/* name of relation to index on */
	char	   *accessMethod;	/* name of access method (eg. btree) */
	List	   *indexParams;	/* a list of IndexElem */
	List	   *withClause;		/* a list of DefElem */
	Node	   *whereClause;	/* qualification (partial-index predicate) */
	List	   *rangetable;		/* range table for qual, filled in by
								 * transformStmt() */
	bool		unique;			/* is index unique? */
	bool		primary;		/* is index on primary key? */
} IndexStmt;

/* ----------------------
 *		Create Function Statement
 * ----------------------
 */
typedef struct ProcedureStmt
{
	NodeTag		type;
	char	   *funcname;		/* name of function to create */
	List	   *argTypes;		/* list of argument types (TypeName nodes) */
	Node	   *returnType;		/* the return type (a TypeName node) */
	List	   *withClause;		/* a list of DefElem */
	List	   *as;				/* definition of function body */
	char	   *language;		/* C, SQL, etc */
} ProcedureStmt;

/* ----------------------
 *		Drop Aggregate Statement
 * ----------------------
 */
typedef struct RemoveAggrStmt
{
	NodeTag		type;
	char	   *aggname;		/* aggregate to drop */
	Node	   *aggtype;		/* TypeName for input datatype, or NULL */
} RemoveAggrStmt;

/* ----------------------
 *		Drop Function Statement
 * ----------------------
 */
typedef struct RemoveFuncStmt
{
	NodeTag		type;
	char	   *funcname;		/* function to drop */
	List	   *args;			/* types of the arguments */
} RemoveFuncStmt;

/* ----------------------
 *		Drop Operator Statement
 * ----------------------
 */
typedef struct RemoveOperStmt
{
	NodeTag		type;
	char	   *opname;			/* operator to drop */
	List	   *args;			/* types of the arguments */
} RemoveOperStmt;

/* ----------------------
 *		Alter Table Statement
 * ----------------------
 */
typedef struct RenameStmt
{
	NodeTag		type;
	char	   *relname;		/* relation to be altered */
	InhOption	inhOpt;			/* recursively act on children? */
	char	   *column;			/* if NULL, rename the relation name to
								 * the new name. Otherwise, rename this
								 * column name. */
	char	   *newname;		/* the new name */
} RenameStmt;

/* ----------------------
 *		Create Rule Statement
 * ----------------------
 */
typedef struct RuleStmt
{
	NodeTag		type;
	char	   *rulename;		/* name of the rule */
	Node	   *whereClause;	/* qualifications */
	CmdType		event;			/* RETRIEVE */
	struct Attr *object;		/* object affected */
	bool		instead;		/* is a 'do instead'? */
	List	   *actions;		/* the action statements */
} RuleStmt;

/* ----------------------
 *		Notify Statement
 * ----------------------
 */
typedef struct NotifyStmt
{
	NodeTag		type;
	char	   *relname;		/* relation to notify */
} NotifyStmt;

/* ----------------------
 *		Listen Statement
 * ----------------------
 */
typedef struct ListenStmt
{
	NodeTag		type;
	char	   *relname;		/* relation to listen on */
} ListenStmt;

/* ----------------------
 *		Unlisten Statement
 * ----------------------
 */
typedef struct UnlistenStmt
{
	NodeTag		type;
	char	   *relname;		/* relation to unlisten on */
} UnlistenStmt;

/* ----------------------
 *		{Begin|Abort|End} Transaction Statement
 * ----------------------
 */
typedef struct TransactionStmt
{
	NodeTag		type;
	int			command;		/* BEGIN|END|ABORT */
} TransactionStmt;

/* ----------------------
 *		Create View Statement
 * ----------------------
 */
typedef struct ViewStmt
{
	NodeTag		type;
	char	   *viewname;		/* name of the view */
	List	   *aliases;		/* target column names */
	Query	   *query;			/* the SQL statement */
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
	char	   *dbpath;			/* location of database (NULL = default) */
	char	   *dbtemplate;		/* template to use (NULL = default) */
	int			encoding;		/* MULTIBYTE encoding (-1 = use default) */
} CreatedbStmt;

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
	char	   *relname;		/* relation being indexed */
	char	   *indexname;		/* original index defined */
} ClusterStmt;

/* ----------------------
 *		Vacuum Statement
 * ----------------------
 */
typedef struct VacuumStmt
{
	NodeTag		type;
	bool		verbose;		/* print status info */
	bool		analyze;		/* analyze data */
	char	   *vacrel;			/* table to vacuum */
	List	   *va_spec;		/* columns to analyse */
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
	char	   *value;
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
	char	   *relname;		/* relation to lock */
	int			mode;			/* lock mode */
} LockStmt;


/* ----------------------
 *		SET CONSTRAINTS Statement
 * ----------------------
 */
typedef struct ConstraintsSetStmt
{
	NodeTag		type;
	List	   *constraints;
	bool		deferred;
} ConstraintsSetStmt;

/* ----------------------
 *		REINDEX Statement
 * ----------------------
 */
typedef struct ReindexStmt
{
	NodeTag		type;
	int			reindexType;	/* INDEX|TABLE|DATABASE */
	const char *name;			/* name to reindex */
	bool		force;
	bool		all;
} ReindexStmt;


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
	char	   *relname;		/* relation to insert into */
	List	   *cols;			/* optional: names of the target columns */
	/*
	 * An INSERT statement has *either* VALUES or SELECT, never both.
	 * If VALUES, a targetList is supplied (empty for DEFAULT VALUES).
	 * If SELECT, a complete SelectStmt (or set-operation tree) is supplied.
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
	char	   *relname;		/* relation to delete from */
	Node	   *whereClause;	/* qualifications */
	InhOption	inhOpt;			/* recursively act on children? */
} DeleteStmt;

/* ----------------------
 *		Update Statement
 * ----------------------
 */
typedef struct UpdateStmt
{
	NodeTag		type;
	char	   *relname;		/* relation to update */
	List	   *targetList;		/* the target list (of ResTarget) */
	Node	   *whereClause;	/* qualifications */
	List	   *fromClause;		/* the from clause */
	InhOption	inhOpt;			/* recursively act on children? */
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

typedef struct SelectStmt
{
	NodeTag		type;
	/*
	 * These fields are used only in "leaf" SelectStmts.
	 */
	List	   *distinctClause; /* NULL, list of DISTINCT ON exprs, or
								 * lcons(NIL,NIL) for all (SELECT
								 * DISTINCT) */
	char	   *into;			/* name of table (for select into table) */
	bool		istemp;			/* into is a temp table? */
	List	   *targetList;		/* the target list (of ResTarget) */
	List	   *fromClause;		/* the FROM clause */
	Node	   *whereClause;	/* WHERE qualification */
	List	   *groupClause;	/* GROUP BY clauses */
	Node	   *havingClause;	/* HAVING conditional-expression */
	/*
	 * These fields are used in both "leaf" SelectStmts and upper-level
	 * SelectStmts.  portalname/binary may only be set at the top level.
	 */
	List	   *sortClause;		/* sort clause (a list of SortGroupBy's) */
	char	   *portalname;		/* the portal (cursor) to create */
	bool		binary;			/* a binary (internal) portal? */
	Node	   *limitOffset;	/* # of result tuples to skip */
	Node	   *limitCount;		/* # of result tuples to return */
	List	   *forUpdate;		/* FOR UPDATE clause */
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
	List	   *colTypes;		/* integer list of OIDs of output column types */
} SetOperationStmt;

/****************************************************************************
 *	Supporting data structures for Parse Trees
 *
 *	Most of these node types appear in raw parsetrees output by the grammar,
 *	and get transformed to something else by the analyzer.	A few of them
 *	are used as-is in transformed querytrees.
 ****************************************************************************/

/*
 * TypeName - specifies a type in definitions
 */
typedef struct TypeName
{
	NodeTag		type;
	char	   *name;			/* name of the type */
	bool		timezone;		/* timezone specified? */
	bool		setof;			/* is a set? */
	int32		typmod;			/* type modifier */
	List	   *arrayBounds;	/* array bounds */
} TypeName;

/*
 * ParamNo - specifies a parameter reference
 */
typedef struct ParamNo
{
	NodeTag		type;
	int			number;			/* the number of the parameter */
	TypeName   *typename;		/* the typecast */
	List	   *indirection;	/* array references */
} ParamNo;

/*
 * A_Expr - binary expressions
 */
typedef struct A_Expr
{
	NodeTag		type;
	int			oper;			/* type of operation
								 * {OP,OR,AND,NOT,ISNULL,NOTNULL} */
	char	   *opname;			/* name of operator/function */
	Node	   *lexpr;			/* left argument */
	Node	   *rexpr;			/* right argument */
} A_Expr;

/*
 * Attr -
 *	  specifies an Attribute (ie. a Column); could have nested dots or
 *	  array references.
 *
 */
typedef struct Attr
{
	NodeTag		type;
	char	   *relname;		/* name of relation (can be "*") */
	ParamNo    *paramNo;		/* or a parameter */
	List	   *attrs;			/* attributes (possibly nested); list of
								 * Values (strings) */
	List	   *indirection;	/* array refs (list of A_Indices') */
} Attr;

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
 * NOTE: for mostly historical reasons, A_Const and ParamNo parsenodes contain
 * room for a TypeName; we only generate a separate TypeCast node if the
 * argument to be casted is neither of those kinds of nodes.  In theory either
 * representation would work, but it is convenient (especially for A_Const)
 * to have the target type immediately available.
 */
typedef struct TypeCast
{
	NodeTag		type;
	Node	   *arg;			/* the expression being casted */
	TypeName   *typename;		/* the target type */
} TypeCast;

/*
 * CaseExpr - a CASE expression
 */
typedef struct CaseExpr
{
	NodeTag		type;
	Oid			casetype;
	Node	   *arg;			/* implicit equality comparison argument */
	List	   *args;			/* the arguments (list of WHEN clauses) */
	Node	   *defresult;		/* the default result (ELSE clause) */
} CaseExpr;

/*
 * CaseWhen - an argument to a CASE expression
 */
typedef struct CaseWhen
{
	NodeTag		type;
	Node	   *expr;			/* comparison expression */
	Node	   *result;			/* substitution result */
} CaseWhen;

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
 */
typedef struct ColumnDef
{
	NodeTag		type;
	char	   *colname;		/* name of column */
	TypeName   *typename;		/* type of column */
	bool		is_not_null;	/* flag to NOT NULL constraint */
	bool		is_sequence;	/* is a sequence? */
	Node	   *raw_default;	/* default value (untransformed parse
								 * tree) */
	char	   *cooked_default; /* nodeToString representation */
	List	   *constraints;	/* other constraints on column */
} ColumnDef;

/*
 * Ident -
 *	  an identifier (could be an attribute or a relation name). Depending
 *	  on the context at transformStmt time, the identifier is treated as
 *	  either a relation name (in which case, isRel will be set) or an
 *	  attribute (in which case, it will be transformed into an Attr).
 */
typedef struct Ident
{
	NodeTag		type;
	char	   *name;			/* its name */
	List	   *indirection;	/* array references */
	bool		isRel;			/* is a relation - filled in by
								 * transformExpr() */
} Ident;

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
	char	   *funcname;		/* name of function */
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
 * ResTarget -
 *	  result target (used in target list of pre-transformed Parse trees)
 *
 * In a SELECT or INSERT target list, 'name' is either NULL or
 * the column name assigned to the value.  (If there is an 'AS ColumnLabel'
 * clause, the grammar sets 'name' from it; otherwise 'name' is initially NULL
 * and is filled in during the parse analysis phase.)
 * The 'indirection' field is not used at all.
 *
 * In an UPDATE target list, 'name' is the name of the destination column,
 * and 'indirection' stores any subscripts attached to the destination.
 * That is, our representation is UPDATE table SET name [indirection] = val.
 */
typedef struct ResTarget
{
	NodeTag		type;
	char	   *name;			/* column name or NULL */
	List	   *indirection;	/* subscripts for destination column, or
								 * NIL */
	Node	   *val;			/* the value expression to compute or
								 * assign */
} ResTarget;

/*
 * SortGroupBy - for ORDER BY clause
 */
typedef struct SortGroupBy
{
	NodeTag		type;
	char	   *useOp;			/* operator to use */
	Node	   *node;			/* Expression  */
} SortGroupBy;

/*
 * RangeVar - range variable, used in FROM clauses
 */
typedef struct RangeVar
{
	NodeTag		type;
	char	   *relname;		/* the relation name */
	InhOption	inhOpt;			/* expand rel by inheritance? */
	Attr	   *name;			/* optional table alias & column aliases */
} RangeVar;

/*
 * RangeSubselect - subquery appearing in a FROM clause
 */
typedef struct RangeSubselect
{
	NodeTag		type;
	Node	   *subquery;		/* the untransformed sub-select clause */
	Attr	   *name;			/* table alias & optional column aliases */
} RangeSubselect;

/*
 * IndexElem - index parameters (used in CREATE INDEX)
 *
 * For a plain index, each 'name' is an attribute name in the heap relation,
 * and 'args' is NIL.  For a functional index, only one IndexElem is allowed.
 * It has name = name of function and args = list of attribute names that
 * are the function's arguments.
 */
typedef struct IndexElem
{
	NodeTag		type;
	char	   *name;			/* name of attribute to index, or function */
	List	   *args;			/* list of names of function arguments */
	char	   *class;			/* name of desired opclass; NULL = default */
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


/****************************************************************************
 *	Nodes for a Query tree
 ****************************************************************************/

/*
 * TargetEntry -
 *	   a target  entry (used in the transformed target list)
 *
 * one of resdom or fjoin is not NULL. a target list is
 *		((<resdom | fjoin> expr) (<resdom | fjoin> expr) ...)
 */
typedef struct TargetEntry
{
	NodeTag		type;
	Resdom	   *resdom;			/* fjoin overload this to be a list?? */
	Fjoin	   *fjoin;
	Node	   *expr;
} TargetEntry;

/*--------------------
 * RangeTblEntry -
 *	  A range table is a List of RangeTblEntry nodes.
 *
 *	  Currently we use the same node type for both plain relation references
 *	  and sub-selects in the FROM clause.  It might be cleaner to abstract
 *	  the common fields into a "superclass" nodetype.
 *
 *	  alias is an Attr node representing the AS alias-clause attached to the
 *	  FROM expression, or NULL if no clause.
 *
 *	  eref is the table reference name and column reference names (either
 *	  real or aliases).  Note that system columns (OID etc) are not included
 *	  in the column list.
 *	  eref->relname is required to be present, and should generally be used
 *	  to identify the RTE for error messages etc.
 *
 *	  inh is TRUE for relation references that should be expanded to include
 *	  inheritance children, if the rel has any.  This *must* be FALSE for
 *	  subquery RTEs.
 *
 *	  inFromCl marks those range variables that are listed in the FROM clause.
 *	  In SQL, the query can only refer to range variables listed in the
 *	  FROM clause, but POSTQUEL allows you to refer to tables not listed,
 *	  in which case a range table entry will be generated.	We still support
 *	  this POSTQUEL feature, although there is some doubt whether it's
 *	  convenient or merely confusing.  The flag is needed since an
 *	  implicitly-added RTE shouldn't change the namespace for unqualified
 *	  column names processed later, and it also shouldn't affect the
 *	  expansion of '*'.
 *
 *	  checkForRead, checkForWrite, and checkAsUser control run-time access
 *	  permissions checks.  A rel will be checked for read or write access
 *	  (or both, or neither) per checkForRead and checkForWrite.  If
 *	  checkAsUser is not InvalidOid, then do the permissions checks using
 *	  the access rights of that user, not the current effective user ID.
 *	  (This allows rules to act as setuid gateways.)
 *--------------------
 */
typedef struct RangeTblEntry
{
	NodeTag		type;
	/*
	 * Fields valid for a plain relation RTE (else NULL/zero):
	 */
	char	   *relname;		/* real name of the relation */
	Oid			relid;			/* OID of the relation */
	/*
	 * Fields valid for a subquery RTE (else NULL):
	 */
	Query	   *subquery;		/* the sub-query */
	/*
	 * Fields valid in all RTEs:
	 */
	Attr	   *alias;			/* user-written alias clause, if any */
	Attr	   *eref;			/* expanded reference names */
	bool		inh;			/* inheritance requested? */
	bool		inFromCl;		/* present in FROM clause */
	bool		checkForRead;	/* check rel for read access */
	bool		checkForWrite;	/* check rel for write access */
	Oid			checkAsUser;	/* if not zero, check access as this user */
} RangeTblEntry;

/*
 * SortClause -
 *	   representation of ORDER BY clauses
 *
 * tleSortGroupRef must match ressortgroupref of exactly one Resdom of the
 * associated targetlist; that is the expression to be sorted (or grouped) by.
 * sortop is the OID of the ordering operator.
 *
 * SortClauses are also used to identify Resdoms that we will do a "Unique"
 * filter step on (for SELECT DISTINCT and SELECT DISTINCT ON).  The
 * distinctClause list is simply a copy of the relevant members of the
 * sortClause list.  Note that distinctClause can be a subset of sortClause,
 * but cannot have members not present in sortClause; and the members that
 * do appear must be in the same order as in sortClause.
 */
typedef struct SortClause
{
	NodeTag		type;
	Index		tleSortGroupRef;/* reference into targetlist */
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

#endif	 /* PARSENODES_H */
