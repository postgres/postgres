/*-------------------------------------------------------------------------
 *
 * parsenodes.h
 *	  definitions for parse tree nodes
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parsenodes.h,v 1.96 2000/01/26 05:58:16 momjian Exp $
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
 *
 */
typedef struct Query
{
	NodeTag		type;

	CmdType		commandType;	/* select|insert|update|delete|utility */

	Node	   *utilityStmt;	/* non-null if this is a non-optimizable
								 * statement */

	int			resultRelation; /* target relation (index to rtable) */
	char	   *into;			/* portal (cursor) name */
	bool		isPortal;		/* is this a retrieve into portal? */
	bool		isBinary;		/* binary portal? */
	bool		isTemp;			/* is 'into' a temp table? */
	bool		unionall;		/* union without unique sort */
	bool		hasAggs;		/* has aggregates in tlist or havingQual */
	bool		hasSubLinks;	/* has subquery SubLink */

	List	   *rtable;			/* list of range table entries */
	List	   *targetList;		/* target list (of TargetEntry) */
	Node	   *qual;			/* qualifications applied to tuples */
	List	   *rowMark;		/* list of RowMark entries */

	char	   *uniqueFlag;		/* NULL, '*', or Unique attribute name */
	List	   *sortClause;		/* a list of SortClause's */

	List	   *groupClause;	/* a list of GroupClause's */

	Node	   *havingQual;		/* qualifications applied to groups */

	List	   *intersectClause;
	List	   *unionClause;	/* unions are linked under the previous
								 * query */

	Node	   *limitOffset;	/* # of result tuples to skip */
	Node	   *limitCount;		/* # of result tuples to return */

	/* internal to planner */
	List	   *base_rel_list;	/* list of base-relation RelOptInfos */
	List	   *join_rel_list;	/* list of join-relation RelOptInfos */
	List	   *query_pathkeys; /* pathkeys for query_planner()'s result */
} Query;


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
 * ----------------------
 */
/* The fields are used in different ways by the different variants of this command */
typedef struct AlterTableStmt
{
	NodeTag		type;
    char        subtype;        /* A = add, T = alter, D = drop, C = add constr, X = drop constr */
	char	   *relname;        /* table to work on */
	bool		inh;			/* recursively on children? */
    char       *name;           /* column or constraint name to act on */
    Node       *def;            /* definition of new column or constraint */
    int         behavior;       /* CASCADE or RESTRICT drop behavior */
} AlterTableStmt;

/* ----------------------
 *		Change ACL Statement
 * ----------------------
 */
typedef struct ChangeACLStmt
{
	NodeTag		type;
	struct AclItem *aclitem;
	unsigned	modechg;
	List	   *relNames;
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
    char       *null_print;     /* how to print NULLs, `\N' by default */
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
	List	   *constraints;	/* list of constraints (Constraint nodes) */
} CreateStmt;

typedef enum ConstrType			/* types of constraints */
{
	CONSTR_NULL, CONSTR_NOTNULL, CONSTR_DEFAULT, CONSTR_CHECK,
	CONSTR_PRIMARY, CONSTR_UNIQUE
} ConstrType;

/*
 * For constraints that use expressions (CONSTR_DEFAULT, CONSTR_CHECK)
 * we may have the expression in either "raw" form (an untransformed
 * parse tree) or "cooked" form (the nodeToString representation of
 * an executable expression tree), depending on how this Constraint
 * node was created (by parsing, or by inheritance from an existing
 * relation).  We should never have both in the same node!
 */

typedef struct Constraint
{
	NodeTag		type;
	ConstrType	contype;
	char	   *name;			/* name */
	Node	   *raw_expr;		/* untransformed parse tree */
	char	   *cooked_expr;	/* nodeToString representation */
	List	   *keys;			/* list of primary keys */
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
	char	   *constr_name;		/* Constraint name */
	char	   *pktable_name;		/* Primary key table name */
	List	   *fk_attrs;			/* Attributes of foreign key */
	List	   *pk_attrs;			/* Corresponding attrs in PK table */
	char	   *match_type;			/* FULL or PARTIAL */
	int32		actions;			/* ON DELETE/UPDATE actions */
	bool		deferrable;			/* DEFERRABLE */
	bool		initdeferred;		/* INITIALLY DEFERRED */
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
	char	   *lang;			/* NULL (which means Clanguage) */
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
    int         sysid;          /* PgSQL system id (-1 if don't care) */
	bool        createdb;		/* Can the user create databases?	  */
	bool 	    createuser;		/* Can this user create users?		  */
	List	   *groupElts;		/* The groups the user is a member of */
	char	   *validUntil;		/* The time the login is valid until  */
} CreateUserStmt;

typedef struct AlterUserStmt
{
	NodeTag		type;
	char	   *user;			/* PostgreSQL user login			  */
	char	   *password;		/* PostgreSQL user password			  */
	int 	    createdb;		/* Can the user create databases?	  */
	int 	    createuser;		/* Can this user create users?		  */
	char	   *validUntil;		/* The time the login is valid until  */
} AlterUserStmt;

typedef struct DropUserStmt
{
	NodeTag		type;
    List       *users;          /* List of users to remove */
} DropUserStmt;


/* ----------------------
 *      Create/Alter/Drop Group Statements
 * ----------------------
 */
typedef struct CreateGroupStmt
{
    NodeTag     type;
    char       *name;           /* name of the new group */
    int         sysid;          /* group id (-1 if pick default) */
    List       *initUsers;      /* list of initial users */
} CreateGroupStmt;

typedef struct AlterGroupStmt
{
    NodeTag     type;
    char       *name;           /* name of group to alter */
    int         action;         /* +1 = add, -1 = drop user */
    int         sysid;          /* sysid change */
    List       *listUsers;      /* list of users to add/drop */
} AlterGroupStmt;

typedef struct DropGroupStmt
{
    NodeTag     type;
    char       *name;
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
 *		Drop Table Statement
 * ----------------------
 */
typedef struct DropStmt
{
	NodeTag		type;
	List	   *relNames;		/* relations to be dropped */
	bool		sequence;
} DropStmt;

/* ----------------------
 *              Truncate Table Statement
 * ----------------------
 */
typedef struct TruncateStmt
{
        NodeTag         type;
        char	   *relName;            /* relation to be truncated */
} TruncateStmt;

/* ----------------------
 *              Comment On Statement
 * ----------------------
 */
typedef struct CommentStmt
{
  NodeTag type;
  int objtype;                         /* Object's type */
  char *objname;                       /* Name of the object */
  char *objproperty;                   /* Property Id (such as column) */
  List *objlist;                       /* Arguments for VAL objects */
  char *comment;                       /* The comment to insert */
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
	char	   *accessMethod;	/* name of acess methood (eg. btree) */
	List	   *indexParams;	/* a list of IndexElem */
	List	   *withClause;		/* a list of DefElem */
	Node	   *whereClause;	/* qualifications */
	List	   *rangetable;		/* range table, filled in by
								 * transformStmt() */
	bool	   *lossy;			/* is index lossy? */
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
	List	   *defArgs;		/* list of definitions a list of strings
								 * (as Value *) */
	Node	   *returnType;		/* the return type (as a string or a
								 * TypeName (ie.setof) */
	List	   *withClause;		/* a list of DefElem */
	List	   *as;				/* the SQL statement or filename */
	char	   *language;		/* C or SQL */
} ProcedureStmt;

/* ----------------------
 *		Drop Aggregate Statement
 * ----------------------
 */
typedef struct RemoveAggrStmt
{
	NodeTag		type;
	char	   *aggname;		/* aggregate to drop */
	char	   *aggtype;		/* for this type */
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
 *		Drop {Type|Index|Rule|View} Statement
 * ----------------------
 */
typedef struct RemoveStmt
{
	NodeTag		type;
	int			removeType;		/* P_TYPE|INDEX|RULE|VIEW */
	char	   *name;			/* name to drop */
} RemoveStmt;

/* ----------------------
 *		Alter Table Statement
 * ----------------------
 */
typedef struct RenameStmt
{
	NodeTag		type;
	char	   *relname;		/* relation to be altered */
	bool		inh;			/* recursively alter children? */
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
	char	   *dbname;			/* database to create */
	char	   *dbpath;			/* location of database */
	int			encoding;		/* default encoding (see regex/pg_wchar.h) */
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
	List		*constraints;
	bool		deferred;
} ConstraintsSetStmt;


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
	char	   *unique;			/* NULL, '*', or unique attribute name */
	List	   *cols;			/* names of the columns */
	List	   *targetList;		/* the target list (of ResTarget) */
	List	   *fromClause;		/* the from clause */
	Node	   *whereClause;	/* qualifications */
	List	   *groupClause;	/* GROUP BY clauses */
	Node	   *havingClause;	/* having conditional-expression */
	List	   *unionClause;	/* union subselect parameters */
	bool		unionall;		/* union without unique sort */
	List	   *intersectClause;
	List	   *forUpdate;		/* FOR UPDATE clause */
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
} UpdateStmt;

/* ----------------------
 *		Select Statement
 * ----------------------
 */
typedef struct SelectStmt
{
	NodeTag		type;
	char	   *unique;			/* NULL, '*', or unique attribute name */
	char	   *into;			/* name of table (for select into table) */
	List	   *targetList;		/* the target list (of ResTarget) */
	List	   *fromClause;		/* the from clause */
	Node	   *whereClause;	/* qualifications */
	List	   *groupClause;	/* GROUP BY clauses */
	Node	   *havingClause;	/* having conditional-expression */
	List	   *intersectClause;
	List	   *exceptClause;

	List	   *unionClause;	/* union subselect parameters */
	List	   *sortClause;		/* sort clause (a list of SortGroupBy's) */
	char	   *portalname;		/* the portal (cursor) to create */
	bool		binary;			/* a binary (internal) portal? */
	bool		istemp;			/* into is a temp table */
	bool		unionall;		/* union without unique sort */
	Node	   *limitOffset;	/* # of result tuples to skip */
	Node	   *limitCount;		/* # of result tuples to return */
	List	   *forUpdate;		/* FOR UPDATE clause */
} SelectStmt;

/****************************************************************************
 *	Supporting data structures for Parse Trees
 *
 *	Most of these node types appear in raw parsetrees output by the grammar,
 *	and get transformed to something else by the analyzer.  A few of them
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
 * inheritance from an existing relation).  We should never have both
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
	Node	   *raw_default;	/* default value (untransformed parse tree) */
	char	   *cooked_default;	/* nodeToString representation */
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
 * RelExpr - relation expressions
 */
typedef struct RelExpr
{
	NodeTag		type;
	char	   *relname;		/* the relation name */
	bool		inh;			/* inheritance query */
} RelExpr;

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
	RelExpr    *relExpr;		/* the relation expression */
	char	   *name;			/* the name to be referenced (optional) */
} RangeVar;

/*
 * IndexElem - index parameters (used in CREATE INDEX)
 */
typedef struct IndexElem
{
	NodeTag		type;
	char	   *name;			/* name of index */
	List	   *args;			/* if not NULL, function index */
	char	   *class;
	TypeName   *typename;		/* type of index's keys (optional) */
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
 * JoinExpr - for JOIN expressions
 */
typedef struct JoinExpr
{
	NodeTag		type;
	int			jointype;
	RangeVar   *larg;
	Node	   *rarg;
	List	   *quals;
} JoinExpr;


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
 *	  Some of the following are only used in one of
 *	  the parsing, optimizing, execution stages.
 *
 *	  inFromCl marks those range variables that are listed in the FROM clause.
 *	  In SQL, the query can only refer to range variables listed in the
 *	  FROM clause, but POSTQUEL allows you to refer to tables not listed,
 *	  in which case a range table entry will be generated.  We still support
 *	  this POSTQUEL feature, although there is some doubt whether it's
 *	  convenient or merely confusing.  The flag is needed since an
 *	  implicitly-added RTE shouldn't change the namespace for unqualified
 *	  column names processed later, and it also shouldn't affect the
 *	  expansion of '*'.
 *
 *	  inJoinSet marks those range variables that the planner should join
 *	  over even if they aren't explicitly referred to in the query.  For
 *	  example, "SELECT COUNT(1) FROM tx" should produce the number of rows
 *	  in tx.  A more subtle example uses a POSTQUEL implicit RTE:
 *			SELECT COUNT(1) FROM tx WHERE TRUE OR (tx.f1 = ty.f2)
 *	  Here we should get the product of the sizes of tx and ty.  However,
 *	  the query optimizer can simplify the WHERE clause to "TRUE", so
 *	  ty will no longer be referred to explicitly; without a flag forcing
 *	  it to be included in the join, we will get the wrong answer.  So,
 *	  a POSTQUEL implicit RTE must be marked inJoinSet but not inFromCl.
 *--------------------
 */
typedef struct RangeTblEntry
{
	NodeTag		type;
	char	   *relname;		/* real name of the relation */
	char	   *refname;		/* the reference name (as specified in the
								 * FROM clause) */
	Oid			relid;			/* OID of the relation */
	bool		inh;			/* inheritance requested? */
	bool		inFromCl;		/* present in FROM clause */
	bool		inJoinSet;		/* planner must include this rel */
	bool		skipAcl;		/* skip ACL check in executor */
} RangeTblEntry;

/*
 * SortClause -
 *	   representation of ORDER BY clauses
 *
 * tleSortGroupRef must match ressortgroupref of exactly one Resdom of the
 * associated targetlist; that is the expression to be sorted (or grouped) by.
 * sortop is the OID of the ordering operator.
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
 * (and it's probably not even really necessary to have two different
 * nodetags...).  We have routines that operate interchangeably on both.
 */
typedef SortClause GroupClause;

#define ROW_MARK_FOR_UPDATE		(1 << 0)
#define ROW_ACL_FOR_UPDATE		(1 << 1)

typedef struct RowMark
{
	NodeTag		type;
	Index		rti;			/* index in Query->rtable */
	bits8		info;			/* as above */
} RowMark;

#endif	 /* PARSENODES_H */
