%{

/*#define YYDEBUG 1*/
/*-------------------------------------------------------------------------
 *
 * gram.y
 *	  POSTGRES SQL YACC rules/actions
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/gram.y,v 2.194 2000/10/05 19:11:33 tgl Exp $
 *
 * HISTORY
 *	  AUTHOR			DATE			MAJOR EVENT
 *	  Andrew Yu			Sept, 1994		POSTQUEL to SQL conversion
 *	  Andrew Yu			Oct, 1994		lispy code conversion
 *
 * NOTES
 *	  CAPITALS are used to represent terminal symbols.
 *	  non-capitals are used to represent non-terminals.
 *	  SQL92-specific syntax is separated from plain SQL/Postgres syntax
 *	  to help isolate the non-extensible portions of the parser.
 *
 *	  if you use list, make sure the datum is a node so that the printing
 *	  routines work
 *
 * WARNING
 *	  sometimes we assign constants to makeStrings. Make sure we don't free
 *	  those.
 *
 *-------------------------------------------------------------------------
 */
#include <ctype.h>

#include "postgres.h"

#include "access/htup.h"
#include "access/xact.h"
#include "catalog/catname.h"
#include "catalog/pg_type.h"
#include "nodes/parsenodes.h"
#include "nodes/print.h"
#include "parser/analyze.h"
#include "parser/gramparse.h"
#include "parser/parse_type.h"
#include "storage/bufpage.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/numeric.h"
#include "utils/guc.h"

#ifdef MULTIBYTE
#include "miscadmin.h"
#include "mb/pg_wchar.h"
#endif

extern List *parsetree;			/* final parse result is delivered here */

static char saved_relname[NAMEDATALEN];  /* need this for complex attributes */
static bool QueryIsRule = FALSE;
static Oid	*param_type_info;
static int	pfunc_num_args;


/*
 * If you need access to certain yacc-generated variables and find that
 * they're static by default, uncomment the next line.  (this is not a
 * problem, yet.)
 */
/*#define __YYSCLASS*/

static char *xlateSqlFunc(char *);
static char *xlateSqlType(char *);
static Node *makeA_Expr(int oper, char *opname, Node *lexpr, Node *rexpr);
static Node *makeTypeCast(Node *arg, TypeName *typename);
static Node *makeRowExpr(char *opr, List *largs, List *rargs);
static void mapTargetColumns(List *source, List *target);
static SelectStmt *findLeftmostSelect(Node *node);
static bool exprIsNullConstant(Node *arg);
static Node *doNegate(Node *n);
static void doNegateFloat(Value *v);

%}


%union
{
	int					ival;
	char				chr;
	char				*str;
	bool				boolean;
	JoinType			jtype;
	List				*list;
	Node				*node;
	Value				*value;

	Attr				*attr;
	Ident				*ident;

	TypeName			*typnam;
	DefElem				*defelt;
	SortGroupBy			*sortgroupby;
	JoinExpr			*jexpr;
	IndexElem			*ielem;
	RangeVar			*range;
	A_Indices			*aind;
	ResTarget			*target;
	ParamNo				*paramno;

	VersionStmt			*vstmt;
	DefineStmt			*dstmt;
	RuleStmt			*rstmt;
	InsertStmt			*istmt;
}

%type <node>	stmt,
		AlterGroupStmt, AlterSchemaStmt, AlterTableStmt, AlterUserStmt,
		ClosePortalStmt, ClusterStmt, CommentStmt, ConstraintsSetStmt,
		CopyStmt, CreateAsStmt, CreateGroupStmt, CreatePLangStmt,
		CreateSchemaStmt, CreateSeqStmt, CreateStmt, CreateTrigStmt,
		CreateUserStmt, CreatedbStmt, CursorStmt, DefineStmt, DeleteStmt,
		DropGroupStmt, DropPLangStmt, DropSchemaStmt, DropStmt, DropTrigStmt,
		DropUserStmt, DropdbStmt, ExplainStmt, ExtendStmt, FetchStmt,
		GrantStmt, IndexStmt, InsertStmt, ListenStmt, LoadStmt, LockStmt,
		NotifyStmt, OptimizableStmt, ProcedureStmt, ReindexStmt,
		RemoveAggrStmt, RemoveFuncStmt, RemoveOperStmt, RemoveStmt,
		RenameStmt, RevokeStmt, RuleActionStmt, RuleActionStmtOrEmpty,
		RuleStmt, SelectStmt, SetSessionStmt, TransactionStmt, TruncateStmt,
		UnlistenStmt, UpdateStmt, VacuumStmt, VariableResetStmt,
		VariableSetStmt, VariableShowStmt, ViewStmt

%type <node>	select_clause, select_subclause

%type <list>	SessionList
%type <node>	SessionClause

%type <node>    alter_column_action
%type <ival>    drop_behavior

%type <str>		createdb_opt_location
%type <ival>    createdb_opt_encoding

%type <ival>	opt_lock, lock_type
%type <boolean>	opt_lmode, opt_force

%type <ival>    user_createdb_clause, user_createuser_clause
%type <str>		user_passwd_clause
%type <ival>            sysid_clause
%type <str>		user_valid_clause
%type <list>	user_list, user_group_clause, users_in_new_group_clause

%type <boolean>	TriggerActionTime, TriggerForSpec, PLangTrusted

%type <str>		OptConstrFromTable

%type <str>		TriggerEvents
%type <value>	TriggerFuncArg

%type <str>		relation_name, copy_file_name, copy_delimiter, copy_null, def_name,
		database_name, access_method_clause, access_method, attr_name,
		class, index_name, name, func_name, file_name, aggr_argtype

%type <str>		opt_id,
		all_Op, MathOp, opt_name,
		OptUseOp, opt_class, SpecialRuleRelation

%type <str>		opt_level, opt_encoding
%type <str>		privileges, operation_commalist, grantee
%type <chr>		operation, TriggerOneEvent

%type <list>	stmtblock, stmtmulti,
		result, OptTempTableName, relation_name_list, OptTableElementList,
		OptUnder, OptInherit, definition, opt_distinct,
		opt_with, func_args, func_args_list, func_as,
		oper_argtypes, RuleActionList, RuleActionMulti,
		opt_column_list, columnList, opt_va_list, va_list,
		sort_clause, sortby_list, index_params, index_list, name_list,
		from_clause, from_list, opt_array_bounds,
		expr_list, attrs, target_list, update_target_list,
		def_list, opt_indirection, group_clause, TriggerFuncArgs,
		opt_select_limit

%type <typnam>	func_arg, func_return

%type <boolean>	opt_arg, TriggerForOpt, TriggerForType, OptTemp

%type <list>	for_update_clause, update_list
%type <boolean>	opt_all
%type <boolean>	opt_table
%type <boolean>	opt_chain, opt_trans

%type <node>	join_outer, join_qual
%type <jtype>	join_type

%type <list>	extract_list, position_list
%type <list>	substr_list, substr_from, substr_for, trim_list
%type <list>	opt_interval

%type <boolean> opt_inh_star, opt_binary, opt_using, opt_instead, opt_only
				opt_with_copy, index_opt_unique, opt_verbose, opt_analyze
%type <boolean> opt_cursor

%type <ival>	copy_dirn, def_type, direction, reindex_type, remove_type,
		opt_column, event, comment_type, comment_cl,
		comment_ag, comment_fn, comment_op, comment_tg

%type <ival>	fetch_how_many

%type <node>	select_limit_value, select_offset_value

%type <list>	OptSeqList
%type <defelt>	OptSeqElem

%type <istmt>	insert_rest

%type <node>	OptTableElement, ConstraintElem
%type <node>	columnDef
%type <defelt>	def_elem
%type <node>	def_arg, columnElem, where_clause,
				a_expr, b_expr, c_expr, AexprConst,
				in_expr, having_clause
%type <list>	row_descriptor, row_list, in_expr_nodes
%type <node>	row_expr
%type <node>	case_expr, case_arg, when_clause, case_default
%type <list>	when_clause_list
%type <ival>	sub_type
%type <list>	OptCreateAs, CreateAsList
%type <node>	CreateAsElement
%type <value>	NumericOnly, FloatOnly, IntegerOnly
%type <attr>	event_object, attr, alias_clause
%type <sortgroupby>		sortby
%type <ielem>	index_elem, func_index
%type <node>	table_ref
%type <jexpr>	joined_table
%type <range>	relation_expr
%type <target>	target_el, update_target_el
%type <paramno> ParamNo

%type <typnam>	Typename, SimpleTypename, ConstTypename
				Generic, Numeric, Geometric, Character, ConstDatetime, ConstInterval, Bit
%type <str>		generic, character, datetime, bit
%type <str>		extract_arg
%type <str>		opt_charset, opt_collate
%type <str>		opt_float
%type <ival>	opt_numeric, opt_decimal
%type <boolean>	opt_varying, opt_timezone

%type <ival>	Iconst
%type <str>		Sconst, comment_text
%type <str>		UserId, opt_boolean, var_value, zone_value
%type <str>		ColId, ColLabel, TokenId

%type <node>	TableConstraint
%type <list>	ColQualList
%type <node>	ColConstraint, ColConstraintElem, ConstraintAttr
%type <ival>	key_actions, key_delete, key_update, key_reference
%type <str>		key_match
%type <ival>	ConstraintAttributeSpec, ConstraintDeferrabilitySpec,
				ConstraintTimeSpec

%type <list>	constraints_set_list
%type <list>	constraints_set_namelist
%type <boolean>	constraints_set_mode

/*
 * If you make any token changes, remember to:
 *		- use "yacc -d" and update parse.h
 *		- update the keyword table in parser/keywords.c
 */

/* Reserved word tokens
 * SQL92 syntax has many type-specific constructs.
 * So, go ahead and make these types reserved words,
 *  and call-out the syntax explicitly.
 * This gets annoying when trying to also retain Postgres' nice
 *  type-extensible features, but we don't really have a choice.
 * - thomas 1997-10-11
 * NOTE: Whenever possible, try to add new keywords to the ColId list,
 * or failing that, at least to the ColLabel list.
 */

/* Keywords (in SQL92 reserved words) */
%token	ABSOLUTE, ACTION, ADD, ALL, ALTER, AND, ANY, AS, ASC,
		BEGIN_TRANS, BETWEEN, BOTH, BY,
		CASCADE, CASE, CAST, CHAR, CHARACTER, CHECK, CLOSE,
		COALESCE, COLLATE, COLUMN, COMMIT,
		CONSTRAINT, CONSTRAINTS, CREATE, CROSS, CURRENT_DATE,
		CURRENT_TIME, CURRENT_TIMESTAMP, CURRENT_USER, CURSOR,
		DAY_P, DEC, DECIMAL, DECLARE, DEFAULT, DELETE, DESC,
		DISTINCT, DOUBLE, DROP,
		ELSE, END_TRANS, ESCAPE, EXCEPT, EXECUTE, EXISTS, EXTRACT,
		FALSE_P, FETCH, FLOAT, FOR, FOREIGN, FROM, FULL,
		GLOBAL, GRANT, GROUP, HAVING, HOUR_P,
		IN, INNER_P, INSENSITIVE, INSERT, INTERSECT, INTERVAL, INTO, IS,
		ISOLATION, JOIN, KEY, LANGUAGE, LEADING, LEFT, LEVEL, LIKE, LOCAL,
		MATCH, MINUTE_P, MONTH_P, NAMES,
		NATIONAL, NATURAL, NCHAR, NEXT, NO, NOT, NULLIF, NULL_P, NUMERIC,
		OF, OLD, ON, ONLY, OPTION, OR, ORDER, OUTER_P, OVERLAPS,
		PARTIAL, POSITION, PRECISION, PRIMARY, PRIOR, PRIVILEGES, PROCEDURE, PUBLIC,
		READ, REFERENCES, RELATIVE, REVOKE, RIGHT, ROLLBACK,
		SCHEMA, SCROLL, SECOND_P, SELECT, SESSION, SESSION_USER, SET, SOME, SUBSTRING,
		TABLE, TEMPORARY, THEN, TIME, TIMESTAMP, TIMEZONE_HOUR,
		TIMEZONE_MINUTE, TO, TRAILING, TRANSACTION, TRIM, TRUE_P,
		UNION, UNIQUE, UPDATE, USER, USING,
		VALUES, VARCHAR, VARYING, VIEW,
		WHEN, WHERE, WITH, WORK, YEAR_P, ZONE

/* Keywords (in SQL3 reserved words) */
%token	CHAIN, CHARACTERISTICS,
		DEFERRABLE, DEFERRED,
		IMMEDIATE, INITIALLY, INOUT,
		OFF, OUT,
		PATH_P, PENDANT,
		RESTRICT,
        TRIGGER,
        UNDER,
		WITHOUT

/* Keywords (in SQL92 non-reserved words) */
%token	COMMITTED, SERIALIZABLE, TYPE_P

/* Keywords for Postgres support (not in SQL92 reserved words)
 *
 * The CREATEDB and CREATEUSER tokens should go away
 * when some sort of pg_privileges relation is introduced.
 * - Todd A. Brandys 1998-01-01?
 */
%token	ABORT_TRANS, ACCESS, AFTER, AGGREGATE, ANALYZE,
		BACKWARD, BEFORE, BINARY, BIT,
		CACHE, CLUSTER, COMMENT, COPY, CREATEDB, CREATEUSER, CYCLE,
		DATABASE, DELIMITERS, DO,
		EACH, ENCODING, EXCLUSIVE, EXPLAIN, EXTEND,
		FORCE, FORWARD, FUNCTION, HANDLER,
		ILIKE, INCREMENT, INDEX, INHERITS, INSTEAD, ISNULL,
		LANCOMPILER, LIMIT, LISTEN, LOAD, LOCATION, LOCK_P,
		MAXVALUE, MINVALUE, MODE, MOVE,
		NEW, NOCREATEDB, NOCREATEUSER, NONE, NOTHING, NOTIFY, NOTNULL,
		OFFSET, OIDS, OPERATOR, OWNER, PASSWORD, PROCEDURAL,
		REINDEX, RENAME, RESET, RETURNS, ROW, RULE,
		SEQUENCE, SERIAL, SETOF, SHARE, SHOW, START, STATEMENT, STDIN, STDOUT, SYSID,
		TEMP, TOAST, TRUNCATE, TRUSTED, 
		UNLISTEN, UNTIL, VACUUM, VALID, VERBOSE, VERSION

/* The grammar thinks these are keywords, but they are not in the keywords.c
 * list and so can never be entered directly.  The filter in parser.c
 * creates these tokens when required.
 */
%token			UNIONJOIN

/* Special keywords, not in the query language - see the "lex" file */
%token <str>	IDENT, FCONST, SCONST, Op
%token <ival>	ICONST, PARAM

/* these are not real. they are here so that they get generated as #define's*/
%token			OP

/* precedence: lowest to highest */
%left		UNION EXCEPT
%left		INTERSECT
%left		JOIN UNIONJOIN CROSS LEFT FULL RIGHT INNER_P NATURAL
%left		OR
%left		AND
%right		NOT
%right		'='
%nonassoc	'<' '>'
%nonassoc	LIKE ILIKE
%nonassoc	ESCAPE
%nonassoc	OVERLAPS
%nonassoc	BETWEEN
%nonassoc	IN
%left		Op				/* multi-character ops and user-defined operators */
%nonassoc	NOTNULL
%nonassoc	ISNULL
%nonassoc	NULL_P
%nonassoc	IS
%left		'+' '-'
%left		'*' '/' '%'
%left		'^'
%left		'|'				/* XXX Should this have such a high priority? */
/* Unary Operators */
%right		UMINUS
%left		'.'
%left		'[' ']'
%left		TYPECAST
%%

/*
 *	Handle comment-only lines, and ;; SELECT * FROM pg_class ;;;
 *	psql already handles such cases, but other interfaces don't.
 *	bjm 1999/10/05
 */
stmtblock:  stmtmulti
				{ parsetree = $1; }
		;

/* the thrashing around here is to discard "empty" statements... */
stmtmulti:  stmtmulti ';' stmt
				{ if ($3 != (Node *)NULL)
					$$ = lappend($1, $3);
				  else
					$$ = $1;
				}
		| stmt
				{ if ($1 != (Node *)NULL)
					$$ = makeList1($1);
				  else
					$$ = NIL;
				}
		;

stmt :	AlterSchemaStmt
		| AlterTableStmt
		| AlterGroupStmt
		| AlterUserStmt
		| ClosePortalStmt
		| CopyStmt
		| CreateStmt
		| CreateAsStmt
		| CreateSchemaStmt
		| CreateGroupStmt
		| CreateSeqStmt
		| CreatePLangStmt
		| CreateTrigStmt
		| CreateUserStmt
		| ClusterStmt
		| DefineStmt
		| DropStmt		
		| DropSchemaStmt
		| TruncateStmt
		| CommentStmt
		| DropGroupStmt
		| DropPLangStmt
		| DropTrigStmt
		| DropUserStmt
		| ExtendStmt
		| ExplainStmt
		| FetchStmt
		| GrantStmt
		| IndexStmt
		| ListenStmt
		| UnlistenStmt
		| LockStmt
		| ProcedureStmt
		| ReindexStmt
		| RemoveAggrStmt
		| RemoveOperStmt
		| RemoveFuncStmt
		| RemoveStmt
		| RenameStmt
		| RevokeStmt
		| OptimizableStmt
		| RuleStmt
		| SetSessionStmt
		| TransactionStmt
		| ViewStmt
		| LoadStmt
		| CreatedbStmt
		| DropdbStmt
		| VacuumStmt
		| VariableSetStmt
		| VariableShowStmt
		| VariableResetStmt
		| ConstraintsSetStmt
		| /*EMPTY*/
			{ $$ = (Node *)NULL; }
		;

/*****************************************************************************
 *
 * Create a new Postgres DBMS user
 *
 *
 *****************************************************************************/

CreateUserStmt:  CREATE USER UserId
                 user_createdb_clause user_createuser_clause user_group_clause
                 user_valid_clause
				{
					CreateUserStmt *n = makeNode(CreateUserStmt);
					n->user = $3;
                    n->sysid = -1;
					n->password = NULL;
					n->createdb = $4 == +1 ? TRUE : FALSE;
					n->createuser = $5 == +1 ? TRUE : FALSE;
					n->groupElts = $6;
					n->validUntil = $7;
					$$ = (Node *)n;
				}
                | CREATE USER UserId WITH sysid_clause user_passwd_clause
                user_createdb_clause user_createuser_clause user_group_clause
                user_valid_clause
               {
					CreateUserStmt *n = makeNode(CreateUserStmt);
					n->user = $3;
                    n->sysid = $5;
					n->password = $6;
					n->createdb = $7 == +1 ? TRUE : FALSE;
					n->createuser = $8 == +1 ? TRUE : FALSE;
					n->groupElts = $9;
					n->validUntil = $10;
					$$ = (Node *)n;
               }                   
		;

/*****************************************************************************
 *
 * Alter a postgresql DBMS user
 *
 *
 *****************************************************************************/

AlterUserStmt:  ALTER USER UserId user_createdb_clause
				user_createuser_clause user_valid_clause
				{
					AlterUserStmt *n = makeNode(AlterUserStmt);
					n->user = $3;
					n->password = NULL;
					n->createdb = $4;
					n->createuser = $5;
					n->validUntil = $6;
					$$ = (Node *)n;
				}
			| ALTER USER UserId WITH PASSWORD Sconst
			  user_createdb_clause
			  user_createuser_clause user_valid_clause
				{
					AlterUserStmt *n = makeNode(AlterUserStmt);
					n->user = $3;
					n->password = $6;
					n->createdb = $7;
					n->createuser = $8;
					n->validUntil = $9;
					$$ = (Node *)n;
				}
		;

/*****************************************************************************
 *
 * Drop a postgresql DBMS user
 *
 *
 *****************************************************************************/

DropUserStmt:  DROP USER user_list
				{
					DropUserStmt *n = makeNode(DropUserStmt);
					n->users = $3;
					$$ = (Node *)n;
				}
		;

user_passwd_clause:  PASSWORD Sconst			{ $$ = $2; }
			| /*EMPTY*/							{ $$ = NULL; }
		;

sysid_clause: SYSID Iconst
				{
					if ($2 <= 0)
						elog(ERROR, "sysid must be positive");
					$$ = $2;
				}
			| /*EMPTY*/							{ $$ = -1; }
		;

user_createdb_clause:  CREATEDB					{ $$ = +1; }
			| NOCREATEDB						{ $$ = -1; }
			| /*EMPTY*/							{ $$ = 0; }
		;

user_createuser_clause:  CREATEUSER				{ $$ = +1; }
			| NOCREATEUSER						{ $$ = -1; }
			| /*EMPTY*/							{ $$ = 0; }
		;

user_list:  user_list ',' UserId
				{
					$$ = lappend($1, makeString($3));
				}
			| UserId
				{
					$$ = makeList1(makeString($1));
				}
		;

user_group_clause:  IN GROUP user_list			{ $$ = $3; }
			| /*EMPTY*/							{ $$ = NULL; }
		;

user_valid_clause:  VALID UNTIL SCONST			{ $$ = $3; }
			| /*EMPTY*/							{ $$ = NULL; }
		;


/*****************************************************************************
 *
 * Create a postgresql group
 *
 *
 *****************************************************************************/

CreateGroupStmt:  CREATE GROUP UserId 
				{
					CreateGroupStmt *n = makeNode(CreateGroupStmt);
					n->name = $3;
					n->sysid = -1;
					n->initUsers = NULL;
					$$ = (Node *)n;
				}
			| CREATE GROUP UserId WITH sysid_clause users_in_new_group_clause
				{
					CreateGroupStmt *n = makeNode(CreateGroupStmt);
					n->name = $3;
					n->sysid = $5;
					n->initUsers = $6;
					$$ = (Node *)n;
				}
		;

users_in_new_group_clause:  USER user_list		{ $$ = $2; }
			| /* EMPTY */						{ $$ = NULL; }
		;                         

/*****************************************************************************
 *
 * Alter a postgresql group
 *
 *
 *****************************************************************************/

AlterGroupStmt:  ALTER GROUP UserId ADD USER user_list
				{
					AlterGroupStmt *n = makeNode(AlterGroupStmt);
					n->name = $3;
					n->sysid = -1;
					n->action = +1;
					n->listUsers = $6;
					$$ = (Node *)n;
				}
			| ALTER GROUP UserId DROP USER user_list
				{
					AlterGroupStmt *n = makeNode(AlterGroupStmt);
					n->name = $3;
					n->sysid = -1;
					n->action = -1;
					n->listUsers = $6;
					$$ = (Node *)n;
				}
			;

/*****************************************************************************
 *
 * Drop a postgresql group
 *
 *
 *****************************************************************************/

DropGroupStmt: DROP GROUP UserId
				{
					DropGroupStmt *n = makeNode(DropGroupStmt);
					n->name = $3;
					$$ = (Node *)n;
				}
			;


/*****************************************************************************
 *
 * Manipulate a schema
 *
 *
 *****************************************************************************/

CreateSchemaStmt:  CREATE SCHEMA UserId
				{
					/* for now, just make this the same as CREATE DATABASE */
					CreatedbStmt *n = makeNode(CreatedbStmt);
					n->dbname = $3;
					n->dbpath = NULL;
#ifdef MULTIBYTE
					n->encoding = GetTemplateEncoding();
#else
					n->encoding = 0;
#endif
					$$ = (Node *)n;
				}
		;

AlterSchemaStmt:  ALTER SCHEMA UserId
				{
					elog(ERROR, "ALTER SCHEMA not yet supported");
				}
		;

DropSchemaStmt:  DROP SCHEMA UserId
				{
					DropdbStmt *n = makeNode(DropdbStmt);
					n->dbname = $3;
					$$ = (Node *)n;
				}


/*****************************************************************************
 *
 * Manipulate a postgresql session
 *
 *
 *****************************************************************************/

SetSessionStmt:  SET SESSION CHARACTERISTICS AS SessionList
				{
					SetSessionStmt *n = makeNode(SetSessionStmt);
					n->args = $5;
					$$ = (Node*)n;
				}
		;

SessionList:  SessionList ',' SessionClause
				{
					$$ = lappend($1, $3);
				}
		| SessionClause
				{
					$$ = makeList1($1);
				}
		;

SessionClause:  TRANSACTION COMMIT opt_boolean
				{
					VariableSetStmt *n = makeNode(VariableSetStmt);
					n->name  = "autocommit";
					n->value = $3;
					$$ = (Node *) n;
				}
		| TIME ZONE zone_value
				{
					VariableSetStmt *n = makeNode(VariableSetStmt);
					n->name  = "timezone";
					n->value = $3;
					$$ = (Node *) n;
				}
		| TRANSACTION ISOLATION LEVEL opt_level
				{
					VariableSetStmt *n = makeNode(VariableSetStmt);
					n->name  = "DefaultXactIsoLevel";
					n->value = $4;
					$$ = (Node *) n;
				}
			;


/*****************************************************************************
 *
 * Set PG internal variable
 *	  SET name TO 'var_value'
 * Include SQL92 syntax (thomas 1997-10-22):
 *    SET TIME ZONE 'var_value'
 *
 *****************************************************************************/

VariableSetStmt:  SET ColId TO var_value
				{
					VariableSetStmt *n = makeNode(VariableSetStmt);
					n->name  = $2;
					n->value = $4;
					$$ = (Node *) n;
				}
		| SET ColId '=' var_value
				{
					VariableSetStmt *n = makeNode(VariableSetStmt);
					n->name  = $2;
					n->value = $4;
					$$ = (Node *) n;
				}
		| SET TIME ZONE zone_value
				{
					VariableSetStmt *n = makeNode(VariableSetStmt);
					n->name  = "timezone";
					n->value = $4;
					$$ = (Node *) n;
				}
		| SET TRANSACTION ISOLATION LEVEL opt_level
				{
					VariableSetStmt *n = makeNode(VariableSetStmt);
					n->name  = "XactIsoLevel";
					n->value = $5;
					$$ = (Node *) n;
				}
		| SET NAMES opt_encoding
				{
#ifdef MULTIBYTE
					VariableSetStmt *n = makeNode(VariableSetStmt);
					n->name  = "client_encoding";
					n->value = $3;
					$$ = (Node *) n;
#else
					elog(ERROR, "SET NAMES is not supported");
#endif
				}
		;

opt_level:  READ COMMITTED					{ $$ = "committed"; }
		| SERIALIZABLE						{ $$ = "serializable"; }
		;

var_value:  opt_boolean						{ $$ = $1; }
		| SCONST							{ $$ = $1; }
		| ICONST
			{
				char	buf[64];
				sprintf(buf, "%d", $1);
				$$ = pstrdup(buf);
			}
		| '-' ICONST
			{
			char	buf[64];
			sprintf(buf, "%d", -($2));
			$$ = pstrdup(buf);
		}
		| FCONST							{ $$ = $1; }
		| '-' FCONST
			{
			char * s = palloc(strlen($2)+2);
			s[0] = '-';
			strcpy(s + 1, $2);
			$$ = s;
		}
		| name_list
			{
			List *n;
			int slen = 0;
			char *result;

			/* List of words? Then concatenate together */
			if ($1 == NIL)
				elog(ERROR, "SET must have at least one argument");

			foreach (n, $1)
			{
				Value *p = (Value *) lfirst(n);
				Assert(IsA(p, String));
				/* keep track of room for string and trailing comma */
				slen += (strlen(p->val.str) + 1);
			}
			result = palloc(slen + 1);
			*result = '\0';
			foreach (n, $1)
			{
				Value *p = (Value *) lfirst(n);
				strcat(result, p->val.str);
				strcat(result, ",");
			}
			/* remove the trailing comma from the last element */
			*(result+strlen(result)-1) = '\0';
			$$ = result;
		}
		| DEFAULT							{ $$ = NULL; }
		;

opt_boolean:  TRUE_P						{ $$ = "true"; }
		| FALSE_P							{ $$ = "false"; }
		| ON								{ $$ = "on"; }
		| OFF								{ $$ = "off"; }
		;

zone_value:  Sconst							{ $$ = $1; }
		| DEFAULT							{ $$ = NULL; }
		| LOCAL								{ $$ = NULL; }
		;

opt_encoding:  Sconst						{ $$ = $1; }
        | DEFAULT							{ $$ = NULL; }
        | /*EMPTY*/							{ $$ = NULL; }
        ;

VariableShowStmt:  SHOW ColId
				{
					VariableShowStmt *n = makeNode(VariableShowStmt);
					n->name  = $2;
					$$ = (Node *) n;
				}
		| SHOW TIME ZONE
				{
					VariableShowStmt *n = makeNode(VariableShowStmt);
					n->name  = "timezone";
					$$ = (Node *) n;
				}
		| SHOW TRANSACTION ISOLATION LEVEL
				{
					VariableShowStmt *n = makeNode(VariableShowStmt);
					n->name  = "XactIsoLevel";
					$$ = (Node *) n;
				}
		;

VariableResetStmt:	RESET ColId
				{
					VariableResetStmt *n = makeNode(VariableResetStmt);
					n->name  = $2;
					$$ = (Node *) n;
				}
		| RESET TIME ZONE
				{
					VariableResetStmt *n = makeNode(VariableResetStmt);
					n->name  = "timezone";
					$$ = (Node *) n;
				}
		| RESET TRANSACTION ISOLATION LEVEL
				{
					VariableResetStmt *n = makeNode(VariableResetStmt);
					n->name  = "XactIsoLevel";
					$$ = (Node *) n;
				}
		;


ConstraintsSetStmt:	SET CONSTRAINTS constraints_set_list constraints_set_mode
				{
					ConstraintsSetStmt *n = makeNode(ConstraintsSetStmt);
					n->constraints = $3;
					n->deferred    = $4;
					$$ = (Node *) n;
				}
		;


constraints_set_list:	ALL
				{
					$$ = NIL;
				}
		| constraints_set_namelist
				{
					$$ = $1;
				}
		;


constraints_set_namelist:	IDENT
				{
					$$ = makeList1($1);
				}
		| constraints_set_namelist ',' IDENT
				{
					$$ = lappend($1, $3);
				}
		;


constraints_set_mode:	DEFERRED
				{
					$$ = TRUE;
				}
		| IMMEDIATE
				{
					$$ = FALSE;
				}
		;


/*****************************************************************************
 *
 *	ALTER TABLE variations
 *
 *****************************************************************************/

AlterTableStmt:
/* ALTER TABLE <name> ADD [COLUMN] <coldef> */
		ALTER TABLE relation_name opt_inh_star ADD opt_column columnDef
				{
					AlterTableStmt *n = makeNode(AlterTableStmt);
					n->subtype = 'A';
					n->relname = $3;
					n->inh = $4 || SQL_inheritance;
					n->def = $7;
					$$ = (Node *)n;
				}
/* ALTER TABLE <name> ALTER [COLUMN] <colname> {SET DEFAULT <expr>|DROP DEFAULT} */
		| ALTER TABLE relation_name opt_inh_star ALTER opt_column ColId alter_column_action
				{
					AlterTableStmt *n = makeNode(AlterTableStmt);
					n->subtype = 'T';
					n->relname = $3;
					n->inh = $4 || SQL_inheritance;
					n->name = $7;
					n->def = $8;
					$$ = (Node *)n;
				}
/* ALTER TABLE <name> DROP [COLUMN] <name> {RESTRICT|CASCADE} */
		| ALTER TABLE relation_name opt_inh_star DROP opt_column ColId drop_behavior
				{
					AlterTableStmt *n = makeNode(AlterTableStmt);
					n->subtype = 'D';
					n->relname = $3;
					n->inh = $4 || SQL_inheritance;
					n->name = $7;
					n->behavior = $8;
					$$ = (Node *)n;
				}
/* ALTER TABLE <name> ADD CONSTRAINT ... */
		| ALTER TABLE relation_name opt_inh_star ADD TableConstraint
				{
					AlterTableStmt *n = makeNode(AlterTableStmt);
					n->subtype = 'C';
					n->relname = $3;
					n->inh = $4 || SQL_inheritance;
					n->def = $6;
					$$ = (Node *)n;
				}
/* ALTER TABLE <name> DROP CONSTRAINT <name> {RESTRICT|CASCADE} */
		| ALTER TABLE relation_name opt_inh_star DROP CONSTRAINT name drop_behavior
				{
					AlterTableStmt *n = makeNode(AlterTableStmt);
					n->subtype = 'X';
					n->relname = $3;
					n->inh = $4 || SQL_inheritance;
					n->name = $7;
					n->behavior = $8;
					$$ = (Node *)n;
				}
/* ALTER TABLE <name> CREATE TOAST TABLE */
		| ALTER TABLE relation_name CREATE TOAST TABLE
				{
					AlterTableStmt *n = makeNode(AlterTableStmt);
					n->subtype = 'E';
					n->relname = $3;
					$$ = (Node *)n;
				}
/* ALTER TABLE <name> OWNER TO UserId */
		| ALTER TABLE relation_name OWNER TO UserId
				{
					AlterTableStmt *n = makeNode(AlterTableStmt);
					n->subtype = 'U';
					n->relname = $3;
					n->name = $6;
					$$ = (Node *)n;
				}
		;

alter_column_action:
		SET DEFAULT a_expr
			{
				/* Treat SET DEFAULT NULL the same as DROP DEFAULT */
				if (exprIsNullConstant($3))
					$$ = NULL;
				else
					$$ = $3;
			}
		| DROP DEFAULT					{ $$ = NULL; }
        ;

drop_behavior: CASCADE					{ $$ = CASCADE; }
		| RESTRICT						{ $$ = RESTRICT; }
        ;



/*****************************************************************************
 *
 *		QUERY :
 *				close <optname>
 *
 *****************************************************************************/

ClosePortalStmt:  CLOSE opt_id
				{
					ClosePortalStmt *n = makeNode(ClosePortalStmt);
					n->portalname = $2;
					$$ = (Node *)n;
				}
		;

opt_id:  ColId									{ $$ = $1; }
		| /*EMPTY*/								{ $$ = NULL; }
		;


/*****************************************************************************
 *
 *		QUERY :
 *				COPY [BINARY] <relname> FROM/TO
 *				[USING DELIMITERS <delimiter>]
 *
 *****************************************************************************/

CopyStmt:  COPY opt_binary relation_name opt_with_copy copy_dirn copy_file_name copy_delimiter copy_null
				{
					CopyStmt *n = makeNode(CopyStmt);
					n->binary = $2;
					n->relname = $3;
					n->oids = $4;
					n->direction = $5;
					n->filename = $6;
					n->delimiter = $7;
					n->null_print = $8;
					$$ = (Node *)n;
				}
		;

copy_dirn:	TO
				{ $$ = TO; }
		| FROM
				{ $$ = FROM; }
		;

/*
 * copy_file_name NULL indicates stdio is used. Whether stdin or stdout is
 * used depends on the direction. (It really doesn't make sense to copy from
 * stdout. We silently correct the "typo".		 - AY 9/94
 */
copy_file_name:  Sconst							{ $$ = $1; }
		| STDIN									{ $$ = NULL; }
		| STDOUT								{ $$ = NULL; }
		;

opt_binary:  BINARY								{ $$ = TRUE; }
		| /*EMPTY*/								{ $$ = FALSE; }
		;

opt_with_copy:	WITH OIDS						{ $$ = TRUE; }
		| /*EMPTY*/								{ $$ = FALSE; }
		;

/*
 * the default copy delimiter is tab but the user can configure it
 */
copy_delimiter:  opt_using DELIMITERS Sconst	{ $$ = $3; }
		| /*EMPTY*/								{ $$ = "\t"; }
		;

opt_using:	USING								{ $$ = TRUE; }
		| /*EMPTY*/								{ $$ = TRUE; }
		;

copy_null:      WITH NULL_P AS Sconst			{ $$ = $4; }
                | /*EMPTY*/						{ $$ = "\\N"; }

/*****************************************************************************
 *
 *		QUERY :
 *				CREATE relname
 *
 *****************************************************************************/

CreateStmt:  CREATE OptTemp TABLE relation_name OptUnder '(' OptTableElementList ')' OptInherit
				{
					CreateStmt *n = makeNode(CreateStmt);
					n->istemp = $2;
					n->relname = $4;
					n->tableElts = $7;
					n->inhRelnames = nconc($5, $9);
/* if ($5 != NIL) 
					{
                        n->inhRelnames = $5;
					}
                    else
					{ */
                        /* INHERITS is deprecated */
					/* n->inhRelnames = $9;
					} */
					n->constraints = NIL;
					$$ = (Node *)n;
				}
		;

/*
 * Redundancy here is needed to avoid shift/reduce conflicts,
 * since TEMP is not a reserved word.  See also OptTempTableName.
 */
OptTemp:      TEMPORARY						{ $$ = TRUE; }
			| TEMP							{ $$ = TRUE; }
			| LOCAL TEMPORARY				{ $$ = TRUE; }
			| LOCAL TEMP					{ $$ = TRUE; }
			| GLOBAL TEMPORARY
				{
					elog(ERROR, "GLOBAL TEMPORARY TABLE is not currently supported");
					$$ = TRUE;
				}
			| GLOBAL TEMP
				{
					elog(ERROR, "GLOBAL TEMPORARY TABLE is not currently supported");
					$$ = TRUE;
				}
			| /*EMPTY*/						{ $$ = FALSE; }
		;

OptTableElementList:  OptTableElementList ',' OptTableElement
				{
					if ($3 != NULL)
						$$ = lappend($1, $3);
					else
						$$ = $1;
				}
			| OptTableElement
				{
					if ($1 != NULL)
						$$ = makeList1($1);
					else
						$$ = NIL;
				}
			| /*EMPTY*/							{ $$ = NIL; }
		;

OptTableElement:  columnDef						{ $$ = $1; }
			| TableConstraint					{ $$ = $1; }
		;

columnDef:  ColId Typename ColQualList opt_collate
				{
					ColumnDef *n = makeNode(ColumnDef);
					n->colname = $1;
					n->typename = $2;
					n->constraints = $3;

					if ($4 != NULL)
						elog(NOTICE,"CREATE TABLE/COLLATE %s not yet implemented"
							 "; clause ignored", $4);

					$$ = (Node *)n;
				}
			| ColId SERIAL ColQualList opt_collate
				{
					ColumnDef *n = makeNode(ColumnDef);
					n->colname = $1;
					n->typename = makeNode(TypeName);
					n->typename->name = xlateSqlType("integer");
					n->typename->typmod = -1;
					n->is_sequence = TRUE;
					n->constraints = $3;

					if ($4 != NULL)
						elog(NOTICE,"CREATE TABLE/COLLATE %s not yet implemented"
							 "; clause ignored", $4);

					$$ = (Node *)n;
				}
		;

ColQualList:  ColQualList ColConstraint		{ $$ = lappend($1, $2); }
			| /*EMPTY*/						{ $$ = NIL; }
		;

ColConstraint:
		CONSTRAINT name ColConstraintElem
				{
					switch (nodeTag($3))
					{
						case T_Constraint:
							{
								Constraint *n = (Constraint *)$3;
								n->name = $2;
							}
							break;
						case T_FkConstraint:
							{
								FkConstraint *n = (FkConstraint *)$3;
								n->constr_name = $2;
							}
							break;
						default:
							break;
					}
					$$ = $3;
				}
		| ColConstraintElem
				{ $$ = $1; }
		| ConstraintAttr
				{ $$ = $1; }
		;

/* DEFAULT NULL is already the default for Postgres.
 * But define it here and carry it forward into the system
 * to make it explicit.
 * - thomas 1998-09-13
 *
 * WITH NULL and NULL are not SQL92-standard syntax elements,
 * so leave them out. Use DEFAULT NULL to explicitly indicate
 * that a column may have that value. WITH NULL leads to
 * shift/reduce conflicts with WITH TIME ZONE anyway.
 * - thomas 1999-01-08
 *
 * DEFAULT expression must be b_expr not a_expr to prevent shift/reduce
 * conflict on NOT (since NOT might start a subsequent NOT NULL constraint,
 * or be part of a_expr NOT LIKE or similar constructs).
 */
ColConstraintElem:
			  NOT NULL_P
				{
					Constraint *n = makeNode(Constraint);
					n->contype = CONSTR_NOTNULL;
					n->name = NULL;
					n->raw_expr = NULL;
					n->cooked_expr = NULL;
					n->keys = NULL;
					$$ = (Node *)n;
				}
			| NULL_P
				{
					Constraint *n = makeNode(Constraint);
					n->contype = CONSTR_NULL;
					n->name = NULL;
					n->raw_expr = NULL;
					n->cooked_expr = NULL;
					n->keys = NULL;
					$$ = (Node *)n;
				}
			| UNIQUE
				{
					Constraint *n = makeNode(Constraint);
					n->contype = CONSTR_UNIQUE;
					n->name = NULL;
					n->raw_expr = NULL;
					n->cooked_expr = NULL;
					n->keys = NULL;
					$$ = (Node *)n;
				}
			| PRIMARY KEY
				{
					Constraint *n = makeNode(Constraint);
					n->contype = CONSTR_PRIMARY;
					n->name = NULL;
					n->raw_expr = NULL;
					n->cooked_expr = NULL;
					n->keys = NULL;
					$$ = (Node *)n;
				}
			| CHECK '(' a_expr ')'
				{
					Constraint *n = makeNode(Constraint);
					n->contype = CONSTR_CHECK;
					n->name = NULL;
					n->raw_expr = $3;
					n->cooked_expr = NULL;
					n->keys = NULL;
					$$ = (Node *)n;
				}
			| DEFAULT b_expr
				{
					Constraint *n = makeNode(Constraint);
					n->contype = CONSTR_DEFAULT;
					n->name = NULL;
					if (exprIsNullConstant($2))
					{
						/* DEFAULT NULL should be reported as empty expr */
						n->raw_expr = NULL;
					}
					else
					{
						n->raw_expr = $2;
					}
					n->cooked_expr = NULL;
					n->keys = NULL;
					$$ = (Node *)n;
				}
			| REFERENCES ColId opt_column_list key_match key_actions 
				{
					FkConstraint *n = makeNode(FkConstraint);
					n->constr_name		= NULL;
					n->pktable_name		= $2;
					n->fk_attrs			= NIL;
					n->pk_attrs			= $3;
					n->match_type		= $4;
					n->actions			= $5;
					n->deferrable		= FALSE;
					n->initdeferred		= FALSE;
					$$ = (Node *)n;
				}
		;

/*
 * ConstraintAttr represents constraint attributes, which we parse as if
 * they were independent constraint clauses, in order to avoid shift/reduce
 * conflicts (since NOT might start either an independent NOT NULL clause
 * or an attribute).  analyze.c is responsible for attaching the attribute
 * information to the preceding "real" constraint node, and for complaining
 * if attribute clauses appear in the wrong place or wrong combinations.
 *
 * See also ConstraintAttributeSpec, which can be used in places where
 * there is no parsing conflict.
 */
ConstraintAttr: DEFERRABLE
				{
					Constraint *n = makeNode(Constraint);
					n->contype = CONSTR_ATTR_DEFERRABLE;
					$$ = (Node *)n;
				}
			| NOT DEFERRABLE
				{
					Constraint *n = makeNode(Constraint);
					n->contype = CONSTR_ATTR_NOT_DEFERRABLE;
					$$ = (Node *)n;
				}
			| INITIALLY DEFERRED
				{
					Constraint *n = makeNode(Constraint);
					n->contype = CONSTR_ATTR_DEFERRED;
					$$ = (Node *)n;
				}
			| INITIALLY IMMEDIATE
				{
					Constraint *n = makeNode(Constraint);
					n->contype = CONSTR_ATTR_IMMEDIATE;
					$$ = (Node *)n;
				}
		;


/* ConstraintElem specifies constraint syntax which is not embedded into
 *  a column definition. ColConstraintElem specifies the embedded form.
 * - thomas 1997-12-03
 */
TableConstraint:  CONSTRAINT name ConstraintElem
				{
					switch (nodeTag($3))
					{
						case T_Constraint:
							{
								Constraint *n = (Constraint *)$3;
								n->name = $2;
							}
							break;
						case T_FkConstraint:
							{
								FkConstraint *n = (FkConstraint *)$3;
								n->constr_name = $2;
							}
							break;
						default:
							break;
					}
					$$ = $3;
				}
		| ConstraintElem
				{ $$ = $1; }
		;

ConstraintElem:  CHECK '(' a_expr ')'
				{
					Constraint *n = makeNode(Constraint);
					n->contype = CONSTR_CHECK;
					n->name = NULL;
					n->raw_expr = $3;
					n->cooked_expr = NULL;
					$$ = (Node *)n;
				}
		| UNIQUE '(' columnList ')'
				{
					Constraint *n = makeNode(Constraint);
					n->contype = CONSTR_UNIQUE;
					n->name = NULL;
					n->raw_expr = NULL;
					n->cooked_expr = NULL;
					n->keys = $3;
					$$ = (Node *)n;
				}
		| PRIMARY KEY '(' columnList ')'
				{
					Constraint *n = makeNode(Constraint);
					n->contype = CONSTR_PRIMARY;
					n->name = NULL;
					n->raw_expr = NULL;
					n->cooked_expr = NULL;
					n->keys = $4;
					$$ = (Node *)n;
				}
		| FOREIGN KEY '(' columnList ')' REFERENCES ColId opt_column_list
				key_match key_actions ConstraintAttributeSpec
				{
					FkConstraint *n = makeNode(FkConstraint);
					n->constr_name		= NULL;
					n->pktable_name		= $7;
					n->fk_attrs			= $4;
					n->pk_attrs			= $8;
					n->match_type		= $9;
					n->actions			= $10;
					n->deferrable		= ($11 & 1) != 0;
					n->initdeferred		= ($11 & 2) != 0;
					$$ = (Node *)n;
				}
		;

key_match:  MATCH FULL
			{
				$$ = "FULL";
			}
		| MATCH PARTIAL
			{
				elog(ERROR, "FOREIGN KEY/MATCH PARTIAL not yet implemented");
				$$ = "PARTIAL";
			}
		| /*EMPTY*/
			{
				$$ = "UNSPECIFIED";
			}
		;

key_actions:  key_delete				{ $$ = $1; }
		| key_update					{ $$ = $1; }
		| key_delete key_update			{ $$ = $1 | $2; }
		| key_update key_delete			{ $$ = $1 | $2; }
		| /*EMPTY*/						{ $$ = 0; }
		;

key_delete:  ON DELETE key_reference	{ $$ = $3 << FKCONSTR_ON_DELETE_SHIFT; }
		;

key_update:  ON UPDATE key_reference	{ $$ = $3 << FKCONSTR_ON_UPDATE_SHIFT; }
		;

key_reference:  NO ACTION				{ $$ = FKCONSTR_ON_KEY_NOACTION; }
		| RESTRICT						{ $$ = FKCONSTR_ON_KEY_RESTRICT; }
		| CASCADE						{ $$ = FKCONSTR_ON_KEY_CASCADE; }
		| SET NULL_P					{ $$ = FKCONSTR_ON_KEY_SETNULL; }
		| SET DEFAULT					{ $$ = FKCONSTR_ON_KEY_SETDEFAULT; }
		;

OptUnder: UNDER relation_name_list 		        { $$ = $2; }
        | /*EMPTY*/								{ $$ = NIL; } 
		;

opt_only: ONLY              	     	        { $$ = FALSE; }
        | /*EMPTY*/								{ $$ = TRUE; } 
		;

/* INHERITS is Deprecated */
OptInherit:  INHERITS '(' relation_name_list ')'		{ $$ = $3; }
		| /*EMPTY*/									{ $$ = NIL; }
		;

/*
 * Note: CREATE TABLE ... AS SELECT ... is just another spelling for
 * SELECT ... INTO.
 */

CreateAsStmt:  CREATE OptTemp TABLE relation_name OptUnder OptCreateAs AS SelectStmt
				{
					SelectStmt *n = findLeftmostSelect($8);
					if (n->into != NULL)
						elog(ERROR,"CREATE TABLE/AS SELECT may not specify INTO");
					n->istemp = $2;
					n->into = $4;
                    if ($5 != NIL)
						yyerror("CREATE TABLE/AS SELECT does not support UNDER");
					if ($6 != NIL)
						mapTargetColumns($6, n->targetList);
					$$ = $8;
				}
		;

OptCreateAs:  '(' CreateAsList ')'				{ $$ = $2; }
			| /*EMPTY*/							{ $$ = NULL; }
		;

CreateAsList:  CreateAsList ',' CreateAsElement	{ $$ = lappend($1, $3); }
			| CreateAsElement					{ $$ = makeList1($1); }
		;

CreateAsElement:  ColId
				{
					ColumnDef *n = makeNode(ColumnDef);
					n->colname = $1;
					n->typename = NULL;
					n->raw_default = NULL;
					n->cooked_default = NULL;
					n->is_not_null = FALSE;
					n->constraints = NULL;
					$$ = (Node *)n;
				}
		;


/*****************************************************************************
 *
 *		QUERY :
 *				CREATE SEQUENCE seqname
 *
 *****************************************************************************/

CreateSeqStmt:  CREATE SEQUENCE relation_name OptSeqList
				{
					CreateSeqStmt *n = makeNode(CreateSeqStmt);
					n->seqname = $3;
					n->options = $4;
					$$ = (Node *)n;
				}
		;

OptSeqList:  OptSeqList OptSeqElem
				{ $$ = lappend($1, $2); }
			|	{ $$ = NIL; }
		;

OptSeqElem:  CACHE IntegerOnly
				{
					$$ = makeNode(DefElem);
					$$->defname = "cache";
					$$->arg = (Node *)$2;
				}
			| CYCLE
				{
					$$ = makeNode(DefElem);
					$$->defname = "cycle";
					$$->arg = (Node *)NULL;
				}
			| INCREMENT IntegerOnly
				{
					$$ = makeNode(DefElem);
					$$->defname = "increment";
					$$->arg = (Node *)$2;
				}
			| MAXVALUE IntegerOnly
				{
					$$ = makeNode(DefElem);
					$$->defname = "maxvalue";
					$$->arg = (Node *)$2;
				}
			| MINVALUE IntegerOnly
				{
					$$ = makeNode(DefElem);
					$$->defname = "minvalue";
					$$->arg = (Node *)$2;
				}
			| START IntegerOnly
				{
					$$ = makeNode(DefElem);
					$$->defname = "start";
					$$->arg = (Node *)$2;
				}
		;

NumericOnly:  FloatOnly					{ $$ = $1; }
			| IntegerOnly				{ $$ = $1; }

FloatOnly:  FCONST
				{
					$$ = makeFloat($1);
				}
			| '-' FCONST
				{
					$$ = makeFloat($2);
					doNegateFloat($$);
				}
		;

IntegerOnly:  Iconst
				{
					$$ = makeInteger($1);
				}
			| '-' Iconst
				{
					$$ = makeInteger($2);
					$$->val.ival = - $$->val.ival;
				}
		;

/*****************************************************************************
 *
 *		QUERIES :
 *				CREATE PROCEDURAL LANGUAGE ...
 *				DROP PROCEDURAL LANGUAGE ...
 *
 *****************************************************************************/

CreatePLangStmt:  CREATE PLangTrusted PROCEDURAL LANGUAGE Sconst 
			HANDLER def_name LANCOMPILER Sconst
			{
				CreatePLangStmt *n = makeNode(CreatePLangStmt);
				n->plname = $5;
				n->plhandler = $7;
				n->plcompiler = $9;
				n->pltrusted = $2;
				$$ = (Node *)n;
			}
		;

PLangTrusted:  TRUSTED			{ $$ = TRUE; }
			| /*EMPTY*/			{ $$ = FALSE; }

DropPLangStmt:  DROP PROCEDURAL LANGUAGE Sconst
			{
				DropPLangStmt *n = makeNode(DropPLangStmt);
				n->plname = $4;
				$$ = (Node *)n;
			}
		;

/*****************************************************************************
 *
 *		QUERIES :
 *				CREATE TRIGGER ...
 *				DROP TRIGGER ...
 *
 *****************************************************************************/

CreateTrigStmt:  CREATE TRIGGER name TriggerActionTime TriggerEvents ON
				relation_name TriggerForSpec EXECUTE PROCEDURE
				name '(' TriggerFuncArgs ')'
				{
					CreateTrigStmt *n = makeNode(CreateTrigStmt);
					n->trigname = $3;
					n->relname = $7;
					n->funcname = $11;
					n->args = $13;
					n->before = $4;
					n->row = $8;
					memcpy (n->actions, $5, 4);
					n->lang = NULL;		/* unused */
					n->text = NULL;		/* unused */
					n->attr = NULL;		/* unused */
					n->when = NULL;		/* unused */

					n->isconstraint  = FALSE;
					n->deferrable    = FALSE;
					n->initdeferred  = FALSE;
					n->constrrelname = NULL;
					$$ = (Node *)n;
				}
		| CREATE CONSTRAINT TRIGGER name AFTER TriggerEvents ON
				relation_name OptConstrFromTable 
				ConstraintAttributeSpec
				FOR EACH ROW EXECUTE PROCEDURE name '(' TriggerFuncArgs ')'
				{
					CreateTrigStmt *n = makeNode(CreateTrigStmt);
					n->trigname = $4;
					n->relname = $8;
					n->funcname = $16;
					n->args = $18;
					n->before = FALSE;
					n->row = TRUE;
					memcpy (n->actions, $6, 4);
					n->lang = NULL;		/* unused */
					n->text = NULL;		/* unused */
					n->attr = NULL;		/* unused */
					n->when = NULL;		/* unused */

					n->isconstraint  = TRUE;
					n->deferrable = ($10 & 1) != 0;
					n->initdeferred = ($10 & 2) != 0;

					n->constrrelname = $9;
					$$ = (Node *)n;
				}
		;

TriggerActionTime:  BEFORE						{ $$ = TRUE; }
			| AFTER								{ $$ = FALSE; }
		;

TriggerEvents:	TriggerOneEvent
				{
					char *e = palloc (4);
					e[0] = $1; e[1] = 0; $$ = e;
				}
			| TriggerOneEvent OR TriggerOneEvent
				{
					char *e = palloc (4);
					e[0] = $1; e[1] = $3; e[2] = 0; $$ = e;
				}
			| TriggerOneEvent OR TriggerOneEvent OR TriggerOneEvent
				{
					char *e = palloc (4);
					e[0] = $1; e[1] = $3; e[2] = $5; e[3] = 0;
					$$ = e;
				}
		;

TriggerOneEvent:  INSERT					{ $$ = 'i'; }
			| DELETE						{ $$ = 'd'; }
			| UPDATE						{ $$ = 'u'; }
		;

TriggerForSpec:  FOR TriggerForOpt TriggerForType
				{
					$$ = $3;
				}
		;

TriggerForOpt:  EACH						{ $$ = TRUE; }
			| /*EMPTY*/						{ $$ = FALSE; }
		;

TriggerForType:  ROW						{ $$ = TRUE; }
			| STATEMENT						{ $$ = FALSE; }
		;

TriggerFuncArgs:  TriggerFuncArg
				{ $$ = makeList1($1); }
			| TriggerFuncArgs ',' TriggerFuncArg
				{ $$ = lappend($1, $3); }
			| /*EMPTY*/
				{ $$ = NIL; }
		;

TriggerFuncArg:  ICONST
				{
					char buf[64];
					sprintf (buf, "%d", $1);
					$$ = makeString(pstrdup(buf));
				}
			| FCONST
				{
					$$ = makeString($1);
				}
			| Sconst
				{
					$$ = makeString($1);
				}
			| ColId
				{
					$$ = makeString($1);
				}
		;

OptConstrFromTable:			/* Empty */
				{
					$$ = "";
				}
		| FROM relation_name
				{
					$$ = $2;
				}
		;

ConstraintAttributeSpec:  ConstraintDeferrabilitySpec
			{ $$ = $1; }
		| ConstraintDeferrabilitySpec ConstraintTimeSpec
			{
				if ($1 == 0 && $2 != 0)
					elog(ERROR, "INITIALLY DEFERRED constraint must be DEFERRABLE");
				$$ = $1 | $2;
			}
		| ConstraintTimeSpec
			{
				if ($1 != 0)
					$$ = 3;
				else
					$$ = 0;
			}
		| ConstraintTimeSpec ConstraintDeferrabilitySpec
			{
				if ($2 == 0 && $1 != 0)
					elog(ERROR, "INITIALLY DEFERRED constraint must be DEFERRABLE");
				$$ = $1 | $2;
			}
		| /* Empty */
			{ $$ = 0; }
		;

ConstraintDeferrabilitySpec: NOT DEFERRABLE
			{ $$ = 0; }
		| DEFERRABLE
			{ $$ = 1; }
		;

ConstraintTimeSpec: INITIALLY IMMEDIATE
			{ $$ = 0; }
		| INITIALLY DEFERRED
			{ $$ = 2; }
		;


DropTrigStmt:  DROP TRIGGER name ON relation_name
				{
					DropTrigStmt *n = makeNode(DropTrigStmt);
					n->trigname = $3;
					n->relname = $5;
					$$ = (Node *) n;
				}
		;


/*****************************************************************************
 *
 *		QUERY :
 *				define (type,operator,aggregate)
 *
 *****************************************************************************/

DefineStmt:  CREATE def_type def_name definition
				{
					DefineStmt *n = makeNode(DefineStmt);
					n->defType = $2;
					n->defname = $3;
					n->definition = $4;
					$$ = (Node *)n;
				}
		;

def_type:  OPERATOR							{ $$ = OPERATOR; }
		| TYPE_P							{ $$ = TYPE_P; }
		| AGGREGATE							{ $$ = AGGREGATE; }
		;

def_name:  PROCEDURE						{ $$ = "procedure"; }
		| JOIN								{ $$ = "join"; }
		| all_Op							{ $$ = $1; }
		| ColId								{ $$ = $1; }
		;

definition:  '(' def_list ')'				{ $$ = $2; }
		;

def_list:  def_elem							{ $$ = makeList1($1); }
		| def_list ',' def_elem				{ $$ = lappend($1, $3); }
		;

def_elem:  def_name '=' def_arg
				{
					$$ = makeNode(DefElem);
					$$->defname = $1;
					$$->arg = (Node *)$3;
				}
		| def_name
				{
					$$ = makeNode(DefElem);
					$$->defname = $1;
					$$->arg = (Node *)NULL;
				}
		| DEFAULT '=' def_arg
				{
					$$ = makeNode(DefElem);
					$$->defname = "default";
					$$->arg = (Node *)$3;
				}
		;

def_arg:  func_return  					{  $$ = (Node *)$1; }
		| TokenId						{  $$ = (Node *)makeString($1); }
		| all_Op						{  $$ = (Node *)makeString($1); }
		| NumericOnly					{  $$ = (Node *)$1; }
		| Sconst						{  $$ = (Node *)makeString($1); }
		;


/*****************************************************************************
 *
 *		QUERY:
 *				drop <relname1> [, <relname2> .. <relnameN> ]
 *
 *****************************************************************************/

DropStmt:  DROP TABLE relation_name_list
				{
					DropStmt *n = makeNode(DropStmt);
					n->relNames = $3;
					n->sequence = FALSE;
					$$ = (Node *)n;
				}
		| DROP SEQUENCE relation_name_list
				{
					DropStmt *n = makeNode(DropStmt);
					n->relNames = $3;
					n->sequence = TRUE;
					$$ = (Node *)n;
				}
		;

/*****************************************************************************
 *
 *		QUERY:
 *				truncate table relname
 *
 *****************************************************************************/

TruncateStmt:  TRUNCATE opt_table relation_name
				{
					TruncateStmt *n = makeNode(TruncateStmt);
					n->relName = $3;
					$$ = (Node *)n;
				}
			;

/*****************************************************************************
 *
 *  The COMMENT ON statement can take different forms based upon the type of
 *  the object associated with the comment. The form of the statement is:
 *
 *  COMMENT ON [ [ DATABASE | INDEX | RULE | SEQUENCE | TABLE | TYPE | VIEW ] 
 *               <objname> | AGGREGATE <aggname> <aggtype> | FUNCTION 
 *		 <funcname> (arg1, arg2, ...) | OPERATOR <op> 
 *		 (leftoperand_typ rightoperand_typ) | TRIGGER <triggername> ON
 *		 <relname> ] IS 'text'
 *
 *****************************************************************************/
 
CommentStmt:	COMMENT ON comment_type name IS comment_text
			{
				CommentStmt *n = makeNode(CommentStmt);
				n->objtype = $3;
				n->objname = $4;
				n->objproperty = NULL;
				n->objlist = NULL;
				n->comment = $6;
				$$ = (Node *) n;
			}
		| COMMENT ON comment_cl relation_name '.' attr_name IS comment_text
			{
				CommentStmt *n = makeNode(CommentStmt);
				n->objtype = $3;
				n->objname = $4;
				n->objproperty = $6;
				n->objlist = NULL;
				n->comment = $8;
				$$ = (Node *) n;
			}
		| COMMENT ON comment_ag name aggr_argtype IS comment_text
			{
				CommentStmt *n = makeNode(CommentStmt);
				n->objtype = $3;
				n->objname = $4;
				n->objproperty = $5;
				n->objlist = NULL;
				n->comment = $7;
				$$ = (Node *) n;
			}
		| COMMENT ON comment_fn func_name func_args IS comment_text
			{
				CommentStmt *n = makeNode(CommentStmt);
				n->objtype = $3;
				n->objname = $4;
				n->objproperty = NULL;
				n->objlist = $5;
				n->comment = $7;
				$$ = (Node *) n;
			}
		| COMMENT ON comment_op all_Op '(' oper_argtypes ')' IS comment_text
			{
				CommentStmt *n = makeNode(CommentStmt);
				n->objtype = $3;
				n->objname = $4;
				n->objproperty = NULL;
				n->objlist = $6;
				n->comment = $9;
				$$ = (Node *) n;
			}
		| COMMENT ON comment_tg name ON relation_name IS comment_text
			{
				CommentStmt *n = makeNode(CommentStmt);
				n->objtype = $3;
				n->objname = $4;
				n->objproperty = $6;
				n->objlist = NULL;
				n->comment = $8;
				$$ = (Node *) n;
			}
		;

comment_type:	DATABASE { $$ = DATABASE; }
		| INDEX { $$ = INDEX; }
		| RULE { $$ = RULE; }
		| SEQUENCE { $$ = SEQUENCE; }
		| TABLE { $$ = TABLE; }
		| TYPE_P { $$ = TYPE_P; }
		| VIEW { $$ = VIEW; }
		;		

comment_cl:	COLUMN { $$ = COLUMN; }
		;

comment_ag:	AGGREGATE { $$ = AGGREGATE; }
		;

comment_fn:	FUNCTION { $$ = FUNCTION; }
		;

comment_op:	OPERATOR { $$ = OPERATOR; }
		;

comment_tg:	TRIGGER { $$ = TRIGGER; }
		; 

comment_text:	Sconst { $$ = $1; }
		| NULL_P { $$ = NULL; }
		;
		
/*****************************************************************************
 *
 *		QUERY:
 *			fetch/move [forward | backward] [ # | all ] [ in <portalname> ]
 *			fetch [ forward | backward | absolute | relative ]
 *			      [ # | all | next | prior ] [ [ in | from ] <portalname> ]
 *
 *****************************************************************************/

FetchStmt:  FETCH direction fetch_how_many from_in name
				{
					FetchStmt *n = makeNode(FetchStmt);
					if ($2 == RELATIVE)
					{
						if ($3 == 0)
							elog(ERROR,"FETCH/RELATIVE at current position is not supported");
						$2 = FORWARD;
					}
					if ($3 < 0)
					{
						$3 = -$3;
						$2 = (($2 == FORWARD)? BACKWARD: FORWARD);
					}
					n->direction = $2;
					n->howMany = $3;
					n->portalname = $5;
					n->ismove = FALSE;
					$$ = (Node *)n;
				}
		| FETCH fetch_how_many from_in name
				{
					FetchStmt *n = makeNode(FetchStmt);
					if ($2 < 0)
					{
						n->howMany = -$2;
						n->direction = BACKWARD;
					}
					else
					{
						n->direction = FORWARD;
						n->howMany = $2;
					}
					n->portalname = $4;
					n->ismove = FALSE;
					$$ = (Node *)n;
				}
		| FETCH direction from_in name
				{
					FetchStmt *n = makeNode(FetchStmt);
					if ($2 == RELATIVE)
					{
						$2 = FORWARD;
					}
					n->direction = $2;
					n->howMany = 1;
					n->portalname = $4;
					n->ismove = FALSE;
					$$ = (Node *)n;
				}
		| FETCH from_in name
				{
					FetchStmt *n = makeNode(FetchStmt);
					n->direction = FORWARD;
					n->howMany = 1;
					n->portalname = $3;
					n->ismove = FALSE;
					$$ = (Node *)n;
				}
		| FETCH name
				{
					FetchStmt *n = makeNode(FetchStmt);
					n->direction = FORWARD;
					n->howMany = 1;
					n->portalname = $2;
					n->ismove = FALSE;
					$$ = (Node *)n;
				}

		| MOVE direction fetch_how_many from_in name
				{
					FetchStmt *n = makeNode(FetchStmt);
					if ($3 < 0)
					{
						$3 = -$3;
						$2 = (($2 == FORWARD)? BACKWARD: FORWARD);
					}
					n->direction = $2;
					n->howMany = $3;
					n->portalname = $5;
					n->ismove = TRUE;
					$$ = (Node *)n;
				}
		| MOVE fetch_how_many from_in name
				{
					FetchStmt *n = makeNode(FetchStmt);
					if ($2 < 0)
					{
						n->howMany = -$2;
						n->direction = BACKWARD;
					}
					else
					{
						n->direction = FORWARD;
						n->howMany = $2;
					}
					n->portalname = $4;
					n->ismove = TRUE;
					$$ = (Node *)n;
				}
		| MOVE direction from_in name
				{
					FetchStmt *n = makeNode(FetchStmt);
					n->direction = $2;
					n->howMany = 1;
					n->portalname = $4;
					n->ismove = TRUE;
					$$ = (Node *)n;
				}
		|	MOVE from_in name
				{
					FetchStmt *n = makeNode(FetchStmt);
					n->direction = FORWARD;
					n->howMany = 1;
					n->portalname = $3;
					n->ismove = TRUE;
					$$ = (Node *)n;
				}
		| MOVE name
				{
					FetchStmt *n = makeNode(FetchStmt);
					n->direction = FORWARD;
					n->howMany = 1;
					n->portalname = $2;
					n->ismove = TRUE;
					$$ = (Node *)n;
				}
		;

direction:	FORWARD					{ $$ = FORWARD; }
		| BACKWARD						{ $$ = BACKWARD; }
		| RELATIVE						{ $$ = RELATIVE; }
		| ABSOLUTE
			{
				elog(NOTICE,"FETCH/ABSOLUTE not supported, using RELATIVE");
				$$ = RELATIVE;
			}
		;

fetch_how_many:  Iconst					{ $$ = $1; }
		| '-' Iconst					{ $$ = - $2; }
		| ALL							{ $$ = 0; /* 0 means fetch all tuples*/ }
		| NEXT							{ $$ = 1; }
		| PRIOR							{ $$ = -1; }
		;

from_in:  IN 
	| FROM
	;


/*****************************************************************************
 *
 *		QUERY:
 *				GRANT [privileges] ON [relation_name_list] TO [GROUP] grantee
 *
 *****************************************************************************/

GrantStmt:  GRANT privileges ON relation_name_list TO grantee opt_with_grant
				{
					$$ = (Node*)makeAclStmt($2,$4,$6,'+');
				}
		;

privileges:  ALL PRIVILEGES
				{
				 $$ = aclmakepriv("rwaR",0);
				}
		| ALL
				{
				 $$ = aclmakepriv("rwaR",0);
				}
		| operation_commalist
				{
				 $$ = $1;
				}
		;

operation_commalist:  operation
				{
						$$ = aclmakepriv("",$1);
				}
		| operation_commalist ',' operation
				{
						$$ = aclmakepriv($1,$3);
				}
		;

operation:  SELECT
				{
						$$ = ACL_MODE_RD_CHR;
				}
		| INSERT
				{
						$$ = ACL_MODE_AP_CHR;
				}
		| UPDATE
				{
						$$ = ACL_MODE_WR_CHR;
				}
		| DELETE
				{
						$$ = ACL_MODE_WR_CHR;
				}
		| RULE
				{
						$$ = ACL_MODE_RU_CHR;
				}
		;

grantee:  PUBLIC
				{
						$$ = aclmakeuser("A","");
				}
		| GROUP ColId
				{
						$$ = aclmakeuser("G",$2);
				}
		| ColId
				{
						$$ = aclmakeuser("U",$1);
				}
		;

opt_with_grant:  WITH GRANT OPTION
				{
					yyerror("WITH GRANT OPTION is not supported.  Only relation owners can set privileges");
				 }
		| /*EMPTY*/
		;


/*****************************************************************************
 *
 *		QUERY:
 *				REVOKE [privileges] ON [relation_name] FROM [user]
 *
 *****************************************************************************/

RevokeStmt:  REVOKE privileges ON relation_name_list FROM grantee
				{
					$$ = (Node*)makeAclStmt($2,$4,$6,'-');
				}
		;


/*****************************************************************************
 *
 *		QUERY:
 *				create index <indexname> on <relname>
 *				  using <access> "(" (<col> with <op>)+ ")" [with
 *				  <target_list>]
 *
 *	[where <qual>] is not supported anymore
 *****************************************************************************/

IndexStmt:	CREATE index_opt_unique INDEX index_name ON relation_name
			access_method_clause '(' index_params ')' opt_with
				{
					IndexStmt *n = makeNode(IndexStmt);
					n->unique = $2;
					n->idxname = $4;
					n->relname = $6;
					n->accessMethod = $7;
					n->indexParams = $9;
					n->withClause = $11;
					n->whereClause = NULL;
					$$ = (Node *)n;
				}
		;

index_opt_unique:  UNIQUE						{ $$ = TRUE; }
		| /*EMPTY*/								{ $$ = FALSE; }
		;

access_method_clause:  USING access_method		{ $$ = $2; }
		| /*EMPTY*/								{ $$ = "btree"; }
		;

index_params:  index_list						{ $$ = $1; }
		| func_index							{ $$ = makeList1($1); }
		;

index_list:  index_list ',' index_elem			{ $$ = lappend($1, $3); }
		| index_elem							{ $$ = makeList1($1); }
		;

func_index:  func_name '(' name_list ')' opt_class
				{
					$$ = makeNode(IndexElem);
					$$->name = $1;
					$$->args = $3;
					$$->class = $5;
				}
		  ;

index_elem:  attr_name opt_class
				{
					$$ = makeNode(IndexElem);
					$$->name = $1;
					$$->args = NIL;
					$$->class = $2;
				}
		;

opt_class:  class
				{
					/*
					 * Release 7.0 removed network_ops, timespan_ops, and
					 * datetime_ops, so we suppress it from being passed to
					 * the parser so the default *_ops is used.  This can be
					 * removed in some later release.  bjm 2000/02/07
					 *
					 * Release 7.1 removes lztext_ops, so suppress that too
					 * for a while.  tgl 2000/07/30
					 */
					if (strcmp($1, "network_ops") != 0 &&
						strcmp($1, "timespan_ops") != 0 &&
						strcmp($1, "datetime_ops") != 0 &&
						strcmp($1, "lztext_ops") != 0)
						$$ = $1;
					else
						$$ = NULL;
				}
		| USING class							{ $$ = $2; }
		| /*EMPTY*/								{ $$ = NULL; }
		;


/*****************************************************************************
 *
 *		QUERY:
 *				extend index <indexname> [where <qual>]
 *
 *****************************************************************************/

ExtendStmt:  EXTEND INDEX index_name where_clause
				{
					ExtendStmt *n = makeNode(ExtendStmt);
					n->idxname = $3;
					n->whereClause = $4;
					$$ = (Node *)n;
				}
		;

/*****************************************************************************
 *
 *		QUERY:
 *				execute recipe <recipeName>
 *
 *****************************************************************************/

/* NOT USED
RecipeStmt:  EXECUTE RECIPE recipe_name
				{
					RecipeStmt *n;
					if (!IsTransactionBlock())
						elog(ERROR,"EXECUTE RECIPE may only be used in begin/end transaction blocks");

					n = makeNode(RecipeStmt);
					n->recipeName = $3;
					$$ = (Node *)n;
				}
		;
*/

/*****************************************************************************
 *
 *		QUERY:
 *				define function <fname>
 *						[(<type-1> { , <type-n>})]
 *						returns <type-r>
 *						as <filename or code in language as appropriate>
 *						language <lang> [with
 *						[  arch_pct = <percentage | pre-defined>]
 *						[, disk_pct = <percentage | pre-defined>]
 *						[, byte_pct = <percentage | pre-defined>]
 *						[, perbyte_cpu = <int | pre-defined>]
 *						[, percall_cpu = <int | pre-defined>]
 *						[, iscachable] ]
 *
 *****************************************************************************/

ProcedureStmt:	CREATE FUNCTION func_name func_args
			 RETURNS func_return AS func_as LANGUAGE Sconst opt_with
				{
					ProcedureStmt *n = makeNode(ProcedureStmt);
					n->funcname = $3;
					n->defArgs = $4;
					n->returnType = (Node *)$6;
					n->withClause = $11;
					n->as = $8;
					n->language = $10;
					$$ = (Node *)n;
				};

opt_with:  WITH definition						{ $$ = $2; }
		| /*EMPTY*/								{ $$ = NIL; }
		;

func_args:  '(' func_args_list ')'				{ $$ = $2; }
		| '(' ')'								{ $$ = NIL; }
		;

func_args_list:  func_arg
				{	$$ = makeList1(makeString($1->name)); }
		| func_args_list ',' func_arg
				{	$$ = lappend($1, makeString($3->name)); }
		;

/* Would be nice to use the full Typename production for these fields,
 * but that one sometimes dives into the catalogs looking for valid types.
 * Arguments like "opaque" are valid when defining functions,
 * so that won't work here. The only thing we give up is array notation,
 * which isn't meaningful in this context anyway.
 * - thomas 2000-03-25
 * The following productions are difficult, since it is difficult to
 * distinguish between TokenId and SimpleTypename:
		opt_arg TokenId SimpleTypename
				{
					$$ = $3;
				}
		| TokenId SimpleTypename
				{
					$$ = $2;
				}
 */
func_arg:  opt_arg SimpleTypename
				{
					/* We can catch over-specified arguments here if we want to,
					 * but for now better to silently swallow typmod, etc.
					 * - thomas 2000-03-22
					 */
					$$ = $2;
				}
		| SimpleTypename
				{
					$$ = $1;
				}
		;

opt_arg:  IN
				{
					$$ = FALSE;
				}
		| OUT
				{
					elog(ERROR, "CREATE FUNCTION/OUT parameters are not supported");
					$$ = TRUE;
				}
		| INOUT
				{
					elog(ERROR, "CREATE FUNCTION/INOUT parameters are not supported");
					$$ = FALSE;
				}
		;

func_as: Sconst
				{   $$ = makeList1(makeString($1)); }
		| Sconst ',' Sconst
				{ 	$$ = makeList2(makeString($1), makeString($3)); }
		;

func_return:  SimpleTypename
				{
					/* We can catch over-specified arguments here if we want to,
					 * but for now better to silently swallow typmod, etc.
					 * - thomas 2000-03-22
					 */
					$$ = $1;
				}
		| SETOF SimpleTypename
				{
					$$ = $2;
					$$->setof = TRUE;
				}
		;


/*****************************************************************************
 *
 *		QUERY:
 *
 *		remove function <funcname>
 *				(REMOVE FUNCTION "funcname" (arg1, arg2, ...))
 *		remove aggregate <aggname>
 *				(REMOVE AGGREGATE "aggname" "aggtype")
 *		remove operator <opname>
 *				(REMOVE OPERATOR "opname" (leftoperand_typ rightoperand_typ))
 *		remove type <typename>
 *				(REMOVE TYPE "typename")
 *		remove rule <rulename>
 *				(REMOVE RULE "rulename")
 *
 *****************************************************************************/

RemoveStmt:  DROP remove_type name
				{
					RemoveStmt *n = makeNode(RemoveStmt);
					n->removeType = $2;
					n->name = $3;
					$$ = (Node *)n;
				}
		;

remove_type:  TYPE_P							{  $$ = TYPE_P; }
		| INDEX									{  $$ = INDEX; }
		| RULE									{  $$ = RULE; }
		| VIEW									{  $$ = VIEW; }
		;


RemoveAggrStmt:  DROP AGGREGATE name aggr_argtype
				{
						RemoveAggrStmt *n = makeNode(RemoveAggrStmt);
						n->aggname = $3;
						n->aggtype = $4;
						$$ = (Node *)n;
				}
		;

aggr_argtype:  name								{ $$ = $1; }
		| '*'									{ $$ = NULL; }
		;


RemoveFuncStmt:  DROP FUNCTION func_name func_args
				{
					RemoveFuncStmt *n = makeNode(RemoveFuncStmt);
					n->funcname = $3;
					n->args = $4;
					$$ = (Node *)n;
				}
		;


RemoveOperStmt:  DROP OPERATOR all_Op '(' oper_argtypes ')'
				{
					RemoveOperStmt *n = makeNode(RemoveOperStmt);
					n->opname = $3;
					n->args = $5;
					$$ = (Node *)n;
				}
		;

oper_argtypes:	name
				{
				   elog(ERROR,"parser: argument type missing (use NONE for unary operators)");
				}
		| name ',' name
				{ $$ = makeList2(makeString($1), makeString($3)); }
		| NONE ',' name			/* left unary */
				{ $$ = makeList2(NULL, makeString($3)); }
		| name ',' NONE			/* right unary */
				{ $$ = makeList2(makeString($1), NULL); }
		;


/*****************************************************************************
 *
 *		QUERY:
 *
 *		REINDEX type <typename> [FORCE] [ALL]
 *
 *****************************************************************************/

ReindexStmt:  REINDEX reindex_type name opt_force
				{
					ReindexStmt *n = makeNode(ReindexStmt);
					if (IsTransactionBlock())
						elog(ERROR,"REINDEX command could only be used outside begin/end transaction blocks");
					n->reindexType = $2;
					n->name = $3;
					n->force = $4;
					$$ = (Node *)n;
				}
		;

reindex_type:  INDEX								{  $$ = INDEX; }
		| TABLE										{  $$ = TABLE; }
		| DATABASE									{  $$ = DATABASE; }
		;
opt_force:	FORCE									{  $$ = TRUE; }
		| /* EMPTY */								{  $$ = FALSE; }
		;


/*****************************************************************************
 *
 *		QUERY:
 *				rename <attrname1> in <relname> [*] to <attrname2>
 *				rename <relname1> to <relname2>
 *
 *****************************************************************************/

RenameStmt:  ALTER TABLE relation_name opt_inh_star
				/* "*" deprecated */
				  RENAME opt_column opt_name TO name
				{
					RenameStmt *n = makeNode(RenameStmt);
					n->relname = $3;
					n->inh = $4 || SQL_inheritance;
					n->column = $7;
					n->newname = $9;
					$$ = (Node *)n;
				}
		;

opt_name:  name							{ $$ = $1; }
		| /*EMPTY*/						{ $$ = NULL; }
		;

opt_column:  COLUMN						{ $$ = COLUMN; }
		| /*EMPTY*/						{ $$ = 0; }
		;


/*****************************************************************************
 *
 *		QUERY:	Define Rewrite Rule , Define Tuple Rule
 *				Define Rule <old rules >
 *
 *		only rewrite rule is supported -- ay 9/94
 *
 *****************************************************************************/

RuleStmt:  CREATE RULE name AS
		   { QueryIsRule=TRUE; }
		   ON event TO event_object where_clause
		   DO opt_instead RuleActionList
				{
					RuleStmt *n = makeNode(RuleStmt);
					n->rulename = $3;
					n->event = $7;
					n->object = $9;
					n->whereClause = $10;
					n->instead = $12;
					n->actions = $13;
					$$ = (Node *)n;
				}
		;

RuleActionList:  NOTHING				{ $$ = NIL; }
		| SelectStmt					{ $$ = makeList1($1); }
		| RuleActionStmt				{ $$ = makeList1($1); }
		| '[' RuleActionMulti ']'		{ $$ = $2; }
		| '(' RuleActionMulti ')'		{ $$ = $2; } 
		;

/* the thrashing around here is to discard "empty" statements... */
RuleActionMulti:  RuleActionMulti ';' RuleActionStmtOrEmpty
				{ if ($3 != (Node *) NULL)
					$$ = lappend($1, $3);
				  else
					$$ = $1;
				}
		| RuleActionStmtOrEmpty
				{ if ($1 != (Node *) NULL)
					$$ = makeList1($1);
				  else
					$$ = NIL;
				}
		;

RuleActionStmt:	InsertStmt
		| UpdateStmt
		| DeleteStmt
		| NotifyStmt
		;

RuleActionStmtOrEmpty:	RuleActionStmt
		|	/*EMPTY*/
				{ $$ = (Node *)NULL; }
		;

event_object:  relation_name '.' attr_name
				{
					$$ = makeNode(Attr);
					$$->relname = $1;
					$$->paramNo = NULL;
					$$->attrs = makeList1(makeString($3));
					$$->indirection = NIL;
				}
		| relation_name
				{
					$$ = makeNode(Attr);
					$$->relname = $1;
					$$->paramNo = NULL;
					$$->attrs = NIL;
					$$->indirection = NIL;
				}
		;

/* change me to select, update, etc. some day */
event:	SELECT							{ $$ = CMD_SELECT; }
		| UPDATE						{ $$ = CMD_UPDATE; }
		| DELETE						{ $$ = CMD_DELETE; }
		| INSERT						{ $$ = CMD_INSERT; }
		 ;

opt_instead:  INSTEAD					{ $$ = TRUE; }
		| /*EMPTY*/						{ $$ = FALSE; }
		;


/*****************************************************************************
 *
 *		QUERY:
 *				NOTIFY <relation_name>	can appear both in rule bodies and
 *				as a query-level command
 *
 *****************************************************************************/

NotifyStmt:  NOTIFY relation_name
				{
					NotifyStmt *n = makeNode(NotifyStmt);
					n->relname = $2;
					$$ = (Node *)n;
				}
		;

ListenStmt:  LISTEN relation_name
				{
					ListenStmt *n = makeNode(ListenStmt);
					n->relname = $2;
					$$ = (Node *)n;
				}
;

UnlistenStmt:  UNLISTEN relation_name
				{
					UnlistenStmt *n = makeNode(UnlistenStmt);
					n->relname = $2;
					$$ = (Node *)n;
				}
		| UNLISTEN '*'
				{
					UnlistenStmt *n = makeNode(UnlistenStmt);
					n->relname = "*";
					$$ = (Node *)n;
				}
;


/*****************************************************************************
 *
 *		Transactions:
 *
 *      BEGIN / COMMIT / ROLLBACK
 *      (also older versions END / ABORT)
 *
 *****************************************************************************/

TransactionStmt: ABORT_TRANS opt_trans
				{
					TransactionStmt *n = makeNode(TransactionStmt);
					n->command = ROLLBACK;
					$$ = (Node *)n;
				}
		| BEGIN_TRANS opt_trans
				{
					TransactionStmt *n = makeNode(TransactionStmt);
					n->command = BEGIN_TRANS;
					$$ = (Node *)n;
				}
		| COMMIT opt_trans
				{
					TransactionStmt *n = makeNode(TransactionStmt);
					n->command = COMMIT;
					$$ = (Node *)n;
				}
		| COMMIT opt_trans opt_chain
				{
					TransactionStmt *n = makeNode(TransactionStmt);
					n->command = COMMIT;
					$$ = (Node *)n;
				}
		| END_TRANS opt_trans
				{
					TransactionStmt *n = makeNode(TransactionStmt);
					n->command = COMMIT;
					$$ = (Node *)n;
				}
		| ROLLBACK opt_trans
				{
					TransactionStmt *n = makeNode(TransactionStmt);
					n->command = ROLLBACK;
					$$ = (Node *)n;
				}
		| ROLLBACK opt_trans opt_chain
				{
					TransactionStmt *n = makeNode(TransactionStmt);
					n->command = ROLLBACK;
					$$ = (Node *)n;
				}
		;

opt_trans: WORK									{ $$ = TRUE; }
		| TRANSACTION							{ $$ = TRUE; }
		| /*EMPTY*/								{ $$ = TRUE; }
		;

opt_chain: AND NO CHAIN
				{ $$ = FALSE; }
		| AND CHAIN
				{
					/* SQL99 asks that conforming dbs reject AND CHAIN
					 * if they don't support it. So we can't just ignore it.
					 * - thomas 2000-08-06
					 */
					elog(ERROR, "COMMIT/CHAIN not yet supported");
					$$ = TRUE;
				}
		;


/*****************************************************************************
 *
 *		QUERY:
 *				define view <viewname> '('target-list ')' [where <quals> ]
 *
 *****************************************************************************/

ViewStmt:  CREATE VIEW name opt_column_list AS SelectStmt
				{
					ViewStmt *n = makeNode(ViewStmt);
					n->viewname = $3;
					n->aliases = $4;
					n->query = (Query *) $6;
					$$ = (Node *)n;
				}
		;


/*****************************************************************************
 *
 *		QUERY:
 *				load "filename"
 *
 *****************************************************************************/

LoadStmt:  LOAD file_name
				{
					LoadStmt *n = makeNode(LoadStmt);
					n->filename = $2;
					$$ = (Node *)n;
				}
		;


/*****************************************************************************
 *
 *		CREATE DATABASE
 *
 *
 *****************************************************************************/

CreatedbStmt:  CREATE DATABASE database_name WITH createdb_opt_location createdb_opt_encoding
				{
					CreatedbStmt *n;

					if ($5 == NULL && $6 == -1)
						elog(ERROR, "CREATE DATABASE WITH requires at least one option");

                    n = makeNode(CreatedbStmt);
					n->dbname = $3;
					n->dbpath = $5;
					n->encoding = $6;
					$$ = (Node *)n;
				}
		| CREATE DATABASE database_name
				{
					CreatedbStmt *n = makeNode(CreatedbStmt);
					n->dbname = $3;
					n->dbpath = NULL;
#ifdef MULTIBYTE
					n->encoding = GetTemplateEncoding();
#else
					n->encoding = 0;
#endif
					$$ = (Node *)n;
				}
		;

createdb_opt_location:  LOCATION '=' Sconst		{ $$ = $3; }
		| LOCATION '=' DEFAULT					{ $$ = NULL; }
		| /*EMPTY*/								{ $$ = NULL; }
		;

createdb_opt_encoding:  ENCODING '=' Sconst
				{
#ifdef MULTIBYTE
					int i;
					i = pg_char_to_encoding($3);
					if (i == -1)
						elog(ERROR, "%s is not a valid encoding name", $3);
					$$ = i;
#else
					elog(ERROR, "Multi-byte support is not enabled");
#endif
				}
		| ENCODING '=' Iconst
				{
#ifdef MULTIBYTE
					if (!pg_get_encent_by_encoding($3))
						elog(ERROR, "%d is not a valid encoding code", $3);
					$$ = $3;
#else
					elog(ERROR, "Multi-byte support is not enabled");
#endif
				}
		| ENCODING '=' DEFAULT
				{
#ifdef MULTIBYTE
					$$ = GetTemplateEncoding();
#else
					$$ = 0;
#endif
				}
		| /*EMPTY*/
				{
#ifdef MULTIBYTE
					$$ = GetTemplateEncoding();
#else
					$$= 0;
#endif
				}
		;


/*****************************************************************************
 *
 *		DROP DATABASE
 *
 *
 *****************************************************************************/

DropdbStmt:	DROP DATABASE database_name
				{
					DropdbStmt *n = makeNode(DropdbStmt);
					n->dbname = $3;
					$$ = (Node *)n;
				}
		;


/*****************************************************************************
 *
 *		QUERY:
 *				cluster <index_name> on <relation_name>
 *
 *****************************************************************************/

ClusterStmt:  CLUSTER index_name ON relation_name
				{
				   ClusterStmt *n = makeNode(ClusterStmt);
				   n->relname = $4;
				   n->indexname = $2;
				   $$ = (Node*)n;
				}
		;

/*****************************************************************************
 *
 *		QUERY:
 *				vacuum
 *
 *****************************************************************************/

VacuumStmt:  VACUUM opt_verbose opt_analyze
				{
					VacuumStmt *n = makeNode(VacuumStmt);
					n->verbose = $2;
					n->analyze = $3;
					n->vacrel = NULL;
					n->va_spec = NIL;
					$$ = (Node *)n;
				}
		| VACUUM opt_verbose opt_analyze relation_name opt_va_list
				{
					VacuumStmt *n = makeNode(VacuumStmt);
					n->verbose = $2;
					n->analyze = $3;
					n->vacrel = $4;
					n->va_spec = $5;
					if ( $5 != NIL && !$4 )
						elog(ERROR,"VACUUM syntax error at or near \"(\""
							"\n\tRelation name must be specified");
					$$ = (Node *)n;
				}
		;

opt_verbose:  VERBOSE							{ $$ = TRUE; }
		| /*EMPTY*/								{ $$ = FALSE; }
		;

opt_analyze:  ANALYZE							{ $$ = TRUE; }
		| /*EMPTY*/								{ $$ = FALSE; }
		;

opt_va_list:  '(' va_list ')'					{ $$ = $2; }
		| /*EMPTY*/								{ $$ = NIL; }
		;

va_list:  name
				{ $$ = makeList1($1); }
		| va_list ',' name
				{ $$ = lappend($1, $3); }
		;


/*****************************************************************************
 *
 *		QUERY:
 *				EXPLAIN query
 *
 *****************************************************************************/

ExplainStmt:  EXPLAIN opt_verbose OptimizableStmt
				{
					ExplainStmt *n = makeNode(ExplainStmt);
					n->verbose = $2;
					n->query = (Query*)$3;
					$$ = (Node *)n;
				}
		;


/*****************************************************************************
 *																			 *
 *		Optimizable Stmts:													 *
 *																			 *
 *		one of the five queries processed by the planner					 *
 *																			 *
 *		[ultimately] produces query-trees as specified						 *
 *		in the query-spec document in ~postgres/ref							 *
 *																			 *
 *****************************************************************************/

OptimizableStmt:  SelectStmt
		| CursorStmt
		| UpdateStmt
		| InsertStmt
		| NotifyStmt
		| DeleteStmt					/* by default all are $$=$1 */
		;


/*****************************************************************************
 *
 *		QUERY:
 *				INSERT STATEMENTS
 *
 *****************************************************************************/

/* This rule used 'opt_column_list' between 'relation_name' and 'insert_rest'
 * originally. When the second rule of 'insert_rest' was changed to use the
 * new 'SelectStmt' rule (for INTERSECT and EXCEPT) it produced a shift/reduce
 * conflict. So I just changed the rules 'InsertStmt' and 'insert_rest' to
 * accept the same statements without any shift/reduce conflicts
 */
InsertStmt:  INSERT INTO relation_name insert_rest
				{
 					$4->relname = $3;
					$$ = (Node *) $4;
				}
		;

insert_rest:  VALUES '(' target_list ')'
				{
					$$ = makeNode(InsertStmt);
					$$->cols = NIL;
					$$->targetList = $3;
					$$->selectStmt = NULL;
				}
		| DEFAULT VALUES
				{
					$$ = makeNode(InsertStmt);
					$$->cols = NIL;
					$$->targetList = NIL;
					$$->selectStmt = NULL;
				}
		| SelectStmt
				{
					$$ = makeNode(InsertStmt);
					$$->cols = NIL;
					$$->targetList = NIL;
					$$->selectStmt = $1;
				}
		| '(' columnList ')' VALUES '(' target_list ')'
				{
					$$ = makeNode(InsertStmt);
					$$->cols = $2;
					$$->targetList = $6;
					$$->selectStmt = NULL;
				}
		| '(' columnList ')' SelectStmt
				{
					$$ = makeNode(InsertStmt);
					$$->cols = $2;
					$$->targetList = NIL;
					$$->selectStmt = $4;
				}
		;

opt_column_list:  '(' columnList ')'			{ $$ = $2; }
		| /*EMPTY*/								{ $$ = NIL; }
		;

columnList:  columnList ',' columnElem
				{ $$ = lappend($1, $3); }
		| columnElem
				{ $$ = makeList1($1); }
		;

columnElem:  ColId opt_indirection
				{
					Ident *id = makeNode(Ident);
					id->name = $1;
					id->indirection = $2;
					$$ = (Node *)id;
				}
		;


/*****************************************************************************
 *
 *		QUERY:
 *				DELETE STATEMENTS
 *
 *****************************************************************************/

DeleteStmt:  DELETE FROM opt_only relation_name where_clause
				{
					DeleteStmt *n = makeNode(DeleteStmt);
                    n->inh = $3;
					n->relname = $4;
					n->whereClause = $5;
					$$ = (Node *)n;
				}
		;

LockStmt:	LOCK_P opt_table relation_name opt_lock
				{
					LockStmt *n = makeNode(LockStmt);

					n->relname = $3;
					n->mode = $4;
					$$ = (Node *)n;
				}
		;

opt_lock:  IN lock_type MODE		{ $$ = $2; }
		| /*EMPTY*/				{ $$ = AccessExclusiveLock; }
		;

lock_type:  SHARE ROW EXCLUSIVE	{ $$ = ShareRowExclusiveLock; }
		| ROW opt_lmode			{ $$ = ($2? RowShareLock: RowExclusiveLock); }
		| ACCESS opt_lmode		{ $$ = ($2? AccessShareLock: AccessExclusiveLock); }
		| opt_lmode				{ $$ = ($1? ShareLock: ExclusiveLock); }
		;

opt_lmode:	SHARE				{ $$ = TRUE; }
		| EXCLUSIVE				{ $$ = FALSE; }
		;


/*****************************************************************************
 *
 *		QUERY:
 *				UpdateStmt (UPDATE)
 *
 *****************************************************************************/

UpdateStmt:  UPDATE opt_only relation_name
			  SET update_target_list
			  from_clause
			  where_clause
				{
					UpdateStmt *n = makeNode(UpdateStmt);
					n->inh = $2;
					n->relname = $3;
					n->targetList = $5;
					n->fromClause = $6;
					n->whereClause = $7;
					$$ = (Node *)n;
				}
		;


/*****************************************************************************
 *
 *		QUERY:
 *				CURSOR STATEMENTS
 *
 *****************************************************************************/
CursorStmt:  DECLARE name opt_cursor CURSOR FOR SelectStmt
  				{
 					SelectStmt *n = findLeftmostSelect($6);
					n->portalname = $2;
					n->binary = $3;
					$$ = $6;
				}
		;

opt_cursor:  BINARY						{ $$ = TRUE; }
		| INSENSITIVE					{ $$ = FALSE; }
		| SCROLL						{ $$ = FALSE; }
		| INSENSITIVE SCROLL			{ $$ = FALSE; }
		| /*EMPTY*/						{ $$ = FALSE; }
		;

/*****************************************************************************
 *
 *		QUERY:
 *				SELECT STATEMENTS
 *
 *****************************************************************************/

/* A complete SELECT statement looks like this.  Note sort, for_update,
 * and limit clauses can only appear once, not in each set operation.
 * 
 * The rule returns either a SelectStmt node or a SetOperationStmt tree.
 * One-time clauses are attached to the leftmost SelectStmt leaf.
 *
 * NOTE: only the leftmost SelectStmt leaf should have INTO, either.
 * However, this is not checked by the grammar; parse analysis must check it.
 */

SelectStmt:	  select_clause sort_clause for_update_clause opt_select_limit
			{
				SelectStmt *n = findLeftmostSelect($1);

				n->sortClause = $2;
				n->forUpdate = $3;
				n->limitOffset = nth(0, $4);
				n->limitCount = nth(1, $4);
				$$ = $1;
			}
		;

/* This rule parses Select statements that can appear within set operations,
 * including UNION, INTERSECT and EXCEPT.  '(' and ')' can be used to specify
 * the ordering of the set operations.  Without '(' and ')' we want the
 * operations to be ordered per the precedence specs at the head of this file.
 *
 * Since parentheses around SELECTs also appear in the expression grammar,
 * there is a parse ambiguity if parentheses are allowed at the top level of a
 * select_clause: are the parens part of the expression or part of the select?
 * We separate select_clause into two levels to resolve this: select_clause
 * can have top-level parentheses, select_subclause cannot.
 *
 * Note that sort clauses cannot be included at this level --- a sort clause
 * can only appear at the end of the complete Select, and it will be handled
 * by the topmost SelectStmt rule.  Likewise FOR UPDATE and LIMIT.
 */
select_clause: '(' select_subclause ')'
			{
				$$ = $2; 
			}
		| select_subclause
			{
				$$ = $1; 
			}
		;

select_subclause: SELECT opt_distinct target_list
			 result from_clause where_clause
			 group_clause having_clause
				{
					SelectStmt *n = makeNode(SelectStmt);
					n->distinctClause = $2;
					n->targetList = $3;
					n->istemp = (bool) ((Value *) lfirst($4))->val.ival;
					n->into = (char *) lnext($4);
					n->fromClause = $5;
					n->whereClause = $6;
					n->groupClause = $7;
					n->havingClause = $8;
					$$ = (Node *)n;
				}
		| select_clause UNION opt_all select_clause
			{	
				SetOperationStmt *n = makeNode(SetOperationStmt);
				n->op = SETOP_UNION;
				n->all = $3;
				n->larg = $1;
				n->rarg = $4;
				$$ = (Node *) n;
			}
		| select_clause INTERSECT opt_all select_clause
			{
				SetOperationStmt *n = makeNode(SetOperationStmt);
				n->op = SETOP_INTERSECT;
				n->all = $3;
				n->larg = $1;
				n->rarg = $4;
				$$ = (Node *) n;
			}
		| select_clause EXCEPT opt_all select_clause
			{
				SetOperationStmt *n = makeNode(SetOperationStmt);
				n->op = SETOP_EXCEPT;
				n->all = $3;
				n->larg = $1;
				n->rarg = $4;
				$$ = (Node *) n;
			}
		; 

		/* easy way to return two values. Can someone improve this?  bjm */
result:  INTO OptTempTableName			{ $$ = $2; }
		| /*EMPTY*/						{ $$ = lcons(makeInteger(FALSE), NIL); }
		;

/*
 * Redundancy here is needed to avoid shift/reduce conflicts,
 * since TEMP is not a reserved word.  See also OptTemp.
 *
 * The result is a cons cell (not a true list!) containing
 * a boolean and a table name.
 */
OptTempTableName:  TEMPORARY opt_table relation_name
				{ $$ = lcons(makeInteger(TRUE), (List *) $3); }
			| TEMP opt_table relation_name
				{ $$ = lcons(makeInteger(TRUE), (List *) $3); }
			| LOCAL TEMPORARY opt_table relation_name
				{ $$ = lcons(makeInteger(TRUE), (List *) $4); }
			| LOCAL TEMP opt_table relation_name
				{ $$ = lcons(makeInteger(TRUE), (List *) $4); }
			| GLOBAL TEMPORARY opt_table relation_name
				{
					elog(ERROR, "GLOBAL TEMPORARY TABLE is not currently supported");
					$$ = lcons(makeInteger(TRUE), (List *) $4);
				}
			| GLOBAL TEMP opt_table relation_name
				{
					elog(ERROR, "GLOBAL TEMPORARY TABLE is not currently supported");
					$$ = lcons(makeInteger(TRUE), (List *) $4);
				}
			| TABLE relation_name
				{ $$ = lcons(makeInteger(FALSE), (List *) $2); }
			| relation_name
				{ $$ = lcons(makeInteger(FALSE), (List *) $1); }
		;

opt_table:  TABLE								{ $$ = TRUE; }
		| /*EMPTY*/								{ $$ = FALSE; }
		;

opt_all:  ALL									{ $$ = TRUE; }
		| /*EMPTY*/								{ $$ = FALSE; }
		;

/* We use (NIL) as a placeholder to indicate that all target expressions
 * should be placed in the DISTINCT list during parsetree analysis.
 */
opt_distinct:  DISTINCT							{ $$ = makeList1(NIL); }
		| DISTINCT ON '(' expr_list ')'			{ $$ = $4; }
		| ALL									{ $$ = NIL; }
		| /*EMPTY*/								{ $$ = NIL; }
		;

sort_clause:  ORDER BY sortby_list				{ $$ = $3; }
		| /*EMPTY*/								{ $$ = NIL; }
		;

sortby_list:  sortby							{ $$ = makeList1($1); }
		| sortby_list ',' sortby				{ $$ = lappend($1, $3); }
		;

sortby: a_expr OptUseOp
				{
					$$ = makeNode(SortGroupBy);
					$$->node = $1;
					$$->useOp = $2;
				}
		;

OptUseOp:  USING all_Op							{ $$ = $2; }
		| ASC									{ $$ = "<"; }
		| DESC									{ $$ = ">"; }
		| /*EMPTY*/								{ $$ = "<"; /*default*/ }
		;


opt_select_limit:	LIMIT select_limit_value ',' select_offset_value
			{ $$ = makeList2($4, $2); }
		| LIMIT select_limit_value OFFSET select_offset_value
			{ $$ = makeList2($4, $2); }
		| LIMIT select_limit_value
			{ $$ = makeList2(NULL, $2); }
		| OFFSET select_offset_value LIMIT select_limit_value
			{ $$ = makeList2($2, $4); }
		| OFFSET select_offset_value
			{ $$ = makeList2($2, NULL); }
		| /* EMPTY */
			{ $$ = makeList2(NULL, NULL); }
		;

select_limit_value:  Iconst
			{
				Const	*n = makeNode(Const);

				if ($1 < 1)
					elog(ERROR, "Selection limit must be ALL or a positive integer value");

				n->consttype	= INT4OID;
				n->constlen		= sizeof(int4);
				n->constvalue	= (Datum)$1;
				n->constisnull	= FALSE;
				n->constbyval	= TRUE;
				n->constisset	= FALSE;
				n->constiscast	= FALSE;
				$$ = (Node *)n;
			}
		| ALL
			{
				Const	*n = makeNode(Const);

				n->consttype	= INT4OID;
				n->constlen		= sizeof(int4);
				n->constvalue	= (Datum)0;
				n->constisnull	= FALSE;
				n->constbyval	= TRUE;
				n->constisset	= FALSE;
				n->constiscast	= FALSE;
				$$ = (Node *)n;
			}
		| PARAM
			{
				Param	*n = makeNode(Param);

				n->paramkind	= PARAM_NUM;
				n->paramid		= $1;
				n->paramtype	= INT4OID;
				$$ = (Node *)n;
			}
		;

select_offset_value:	Iconst
			{
				Const	*n = makeNode(Const);

				n->consttype	= INT4OID;
				n->constlen		= sizeof(int4);
				n->constvalue	= (Datum)$1;
				n->constisnull	= FALSE;
				n->constbyval	= TRUE;
				n->constisset	= FALSE;
				n->constiscast	= FALSE;
				$$ = (Node *)n;
			}
		| PARAM
			{
				Param	*n = makeNode(Param);

				n->paramkind	= PARAM_NUM;
				n->paramid		= $1;
				n->paramtype	= INT4OID;
				$$ = (Node *)n;
			}
		;
/*
 *	jimmy bell-style recursive queries aren't supported in the
 *	current system.
 *
 *	...however, recursive addattr and rename supported.  make special
 *	cases for these.
 */
opt_inh_star:  '*'								{ $$ = TRUE; }
		| /*EMPTY*/								{ $$ = FALSE; }
		;

relation_name_list:  name_list;

name_list:  name
				{	$$ = makeList1(makeString($1)); }
		| name_list ',' name
				{	$$ = lappend($1, makeString($3)); }
		;

group_clause:  GROUP BY expr_list				{ $$ = $3; }
		| /*EMPTY*/								{ $$ = NIL; }
		;

having_clause:  HAVING a_expr
				{
					$$ = $2;
				}
		| /*EMPTY*/								{ $$ = NULL; }
		;

for_update_clause:  FOR UPDATE update_list		{ $$ = $3; }
		| FOR READ ONLY							{ $$ = NULL; }
		| /* EMPTY */							{ $$ = NULL; }
		;

update_list:  OF va_list						{ $$ = $2; }
		| /* EMPTY */							{ $$ = makeList1(NULL); }
		;

/*****************************************************************************
 *
 *	clauses common to all Optimizable Stmts:
 *		from_clause		- allow list of both JOIN expressions and table names
 *		where_clause	- qualifications for joins or restrictions
 *
 *****************************************************************************/

from_clause:  FROM from_list					{ $$ = $2; }
		| /*EMPTY*/								{ $$ = NIL; }
		;

from_list:  from_list ',' table_ref				{ $$ = lappend($1, $3); }
		| table_ref								{ $$ = makeList1($1); }
		;

/*
 * table_ref is where an alias clause can be attached.  Note we cannot make
 * alias_clause have an empty production because that causes parse conflicts
 * between table_ref := '(' joined_table ')' alias_clause
 * and joined_table := '(' joined_table ')'.  So, we must have the
 * redundant-looking productions here instead.
 *
 * Note that the SQL spec does not permit a subselect (<derived_table>)
 * without an alias clause, so we don't either.  This avoids the problem
 * of needing to invent a refname for an unlabeled subselect.
 */
table_ref:  relation_expr
				{
					$$ = (Node *) $1;
				}
		| relation_expr alias_clause
				{
					$1->name = $2;
					$$ = (Node *) $1;
				}
		| '(' select_subclause ')' alias_clause
				{
					RangeSubselect *n = makeNode(RangeSubselect);
					n->subquery = $2;
					n->name = $4;
					$$ = (Node *) n;
				}
		| joined_table
				{
					$$ = (Node *) $1;
				}
		| '(' joined_table ')' alias_clause
				{
					$2->alias = $4;
					$$ = (Node *) $2;
				}
		;

/*
 * It may seem silly to separate joined_table from table_ref, but there is
 * method in SQL92's madness: if you don't do it this way you get reduce-
 * reduce conflicts, because it's not clear to the parser generator whether
 * to expect alias_clause after ')' or not.  For the same reason we must
 * treat 'JOIN' and 'join_type JOIN' separately, rather than allowing
 * join_type to expand to empty; if we try it, the parser generator can't
 * figure out when to reduce an empty join_type right after table_ref.
 *
 * Note that a CROSS JOIN is the same as an unqualified
 * INNER JOIN, and an INNER JOIN/ON has the same shape
 * but a qualification expression to limit membership.
 * A NATURAL JOIN implicitly matches column names between
 * tables and the shape is determined by which columns are
 * in common. We'll collect columns during the later transformations.
 */

joined_table:  '(' joined_table ')'
				{
					$$ = $2;
				}
		| table_ref CROSS JOIN table_ref
				{
					/* CROSS JOIN is same as unqualified inner join */
					JoinExpr *n = makeNode(JoinExpr);
					n->jointype = JOIN_INNER;
					n->isNatural = FALSE;
					n->larg = $1;
					n->rarg = $4;
					n->using = NIL;
					n->quals = NULL;
					$$ = n;
				}
		| table_ref UNIONJOIN table_ref
				{
					/* UNION JOIN is made into 1 token to avoid shift/reduce
					 * conflict against regular UNION keyword.
					 */
					JoinExpr *n = makeNode(JoinExpr);
					n->jointype = JOIN_UNION;
					n->isNatural = FALSE;
					n->larg = $1;
					n->rarg = $3;
					n->using = NIL;
					n->quals = NULL;
					$$ = n;
				}
		| table_ref join_type JOIN table_ref join_qual
				{
					JoinExpr *n = makeNode(JoinExpr);
					n->jointype = $2;
					n->isNatural = FALSE;
					n->larg = $1;
					n->rarg = $4;
					if ($5 != NULL && IsA($5, List))
						n->using = (List *) $5;	/* USING clause */
					else
						n->quals = $5; /* ON clause */
					$$ = n;
				}
		| table_ref JOIN table_ref join_qual
				{
					/* letting join_type reduce to empty doesn't work */
					JoinExpr *n = makeNode(JoinExpr);
					n->jointype = JOIN_INNER;
					n->isNatural = FALSE;
					n->larg = $1;
					n->rarg = $3;
					if ($4 != NULL && IsA($4, List))
						n->using = (List *) $4;	/* USING clause */
					else
						n->quals = $4; /* ON clause */
					$$ = n;
				}
		| table_ref NATURAL join_type JOIN table_ref
				{
					JoinExpr *n = makeNode(JoinExpr);
					n->jointype = $3;
					n->isNatural = TRUE;
					n->larg = $1;
					n->rarg = $5;
					n->using = NIL; /* figure out which columns later... */
					n->quals = NULL; /* fill later */
					$$ = n;
				}
		| table_ref NATURAL JOIN table_ref
				{
					/* letting join_type reduce to empty doesn't work */
					JoinExpr *n = makeNode(JoinExpr);
					n->jointype = JOIN_INNER;
					n->isNatural = TRUE;
					n->larg = $1;
					n->rarg = $4;
					n->using = NIL; /* figure out which columns later... */
					n->quals = NULL; /* fill later */
					$$ = n;
				}
		;

alias_clause:  AS ColId '(' name_list ')'
				{
					$$ = makeNode(Attr);
					$$->relname = $2;
					$$->attrs = $4;
				}
		| AS ColId
				{
					$$ = makeNode(Attr);
					$$->relname = $2;
				}
		| ColId '(' name_list ')'
				{
					$$ = makeNode(Attr);
					$$->relname = $1;
					$$->attrs = $3;
				}
		| ColId
				{
					$$ = makeNode(Attr);
					$$->relname = $1;
				}
		;

join_type:  FULL join_outer						{ $$ = JOIN_FULL; }
		| LEFT join_outer						{ $$ = JOIN_LEFT; }
		| RIGHT join_outer						{ $$ = JOIN_RIGHT; }
		| INNER_P								{ $$ = JOIN_INNER; }
		;

/* OUTER is just noise... */
join_outer:  OUTER_P							{ $$ = NULL; }
		| /*EMPTY*/								{ $$ = NULL; }
		;

/* JOIN qualification clauses
 * Possibilities are:
 *  USING ( column list ) allows only unqualified column names,
 *                        which must match between tables.
 *  ON expr allows more general qualifications.
 *
 * We return USING as a List node, while an ON-expr will not be a List.
 */

join_qual:  USING '(' name_list ')'				{ $$ = (Node *) $3; }
		| ON a_expr								{ $$ = $2; }
		;


relation_expr:	relation_name
				{
    				/* default inheritance */
					$$ = makeNode(RangeVar);
					$$->relname = $1;
					$$->inh = SQL_inheritance;
					$$->name = NULL;
				}
		| relation_name '*'				%prec '='
				{
					/* inheritance query */
					$$ = makeNode(RangeVar);
					$$->relname = $1;
					$$->inh = TRUE;
					$$->name = NULL;
				}
		| ONLY relation_name			%prec '='
				{
					/* no inheritance */
					$$ = makeNode(RangeVar);
					$$->relname = $2;
					$$->inh = FALSE;
					$$->name = NULL;
                }
		;

where_clause:  WHERE a_expr						{ $$ = $2; }
		| /*EMPTY*/								{ $$ = NULL;  /* no qualifiers */ }
		;


/*****************************************************************************
 *
 *	Type syntax
 *		SQL92 introduces a large amount of type-specific syntax.
 *		Define individual clauses to handle these cases, and use
 *		 the generic case to handle regular type-extensible Postgres syntax.
 *		- thomas 1997-10-10
 *
 *****************************************************************************/

Typename:  SimpleTypename opt_array_bounds
				{
					$$ = $1;
					$$->arrayBounds = $2;

					/* Is this the name of a complex type? If so, implement
					 * it as a set.
					 */
					if (strcmp(saved_relname, $$->name) == 0)
						/* This attr is the same type as the relation
						 * being defined. The classic example: create
						 * emp(name=text,mgr=emp)
						 */
						$$->setof = TRUE;
					else if (typeTypeRelid(typenameType($$->name)) != InvalidOid)
						 /* (Eventually add in here that the set can only
						  * contain one element.)
						  */
						$$->setof = TRUE;
					else
						$$->setof = FALSE;
				}
		| SETOF SimpleTypename
				{
					$$ = $2;
					$$->setof = TRUE;
				}
		;

opt_array_bounds:	opt_array_bounds '[' ']'
				{  $$ = lappend($1, makeInteger(-1)); }
		| opt_array_bounds '[' Iconst ']'
				{  $$ = lappend($1, makeInteger($3)); }
		| /*EMPTY*/
				{  $$ = NIL; }
		;

SimpleTypename:  ConstTypename
		| ConstInterval
		;

ConstTypename:  Generic
		| Numeric
		| Geometric
		| Bit
		| Character
		| ConstDatetime
		;

Generic:  generic
				{
					$$ = makeNode(TypeName);
					$$->name = xlateSqlType($1);
					$$->typmod = -1;
				}
		;

generic:  IDENT									{ $$ = $1; }
		| TYPE_P								{ $$ = "type"; }
		;

/* SQL92 numeric data types
 * Check FLOAT() precision limits assuming IEEE floating types.
 * Provide real DECIMAL() and NUMERIC() implementations now - Jan 1998-12-30
 * - thomas 1997-09-18
 */
Numeric:  FLOAT opt_float
				{
					$$ = makeNode(TypeName);
					$$->name = xlateSqlType($2);
					$$->typmod = -1;
				}
		| DOUBLE PRECISION
				{
					$$ = makeNode(TypeName);
					$$->name = xlateSqlType("float8");
					$$->typmod = -1;
				}
		| DECIMAL opt_decimal
				{
					$$ = makeNode(TypeName);
					$$->name = xlateSqlType("numeric");
					$$->typmod = $2;
				}
		| DEC opt_decimal
				{
					$$ = makeNode(TypeName);
					$$->name = xlateSqlType("decimal");
					$$->typmod = $2;
				}
		| NUMERIC opt_numeric
				{
					$$ = makeNode(TypeName);
					$$->name = xlateSqlType("numeric");
					$$->typmod = $2;
				}
		;

Geometric:  PATH_P
				{
					$$ = makeNode(TypeName);
					$$->name = xlateSqlType("path");
					$$->typmod = -1;
				}
		;

opt_float:  '(' Iconst ')'
				{
					if ($2 < 1)
						elog(ERROR,"precision for FLOAT must be at least 1");
					else if ($2 < 7)
						$$ = xlateSqlType("float4");
					else if ($2 < 16)
						$$ = xlateSqlType("float8");
					else
						elog(ERROR,"precision for FLOAT must be less than 16");
				}
		| /*EMPTY*/
				{
					$$ = xlateSqlType("float8");
				}
		;

opt_numeric:  '(' Iconst ',' Iconst ')'
				{
					if ($2 < 1 || $2 > NUMERIC_MAX_PRECISION)
						elog(ERROR,"NUMERIC precision %d must be beween 1 and %d",
									$2, NUMERIC_MAX_PRECISION);
					if ($4 < 0 || $4 > $2)
						elog(ERROR,"NUMERIC scale %d must be between 0 and precision %d",
									$4,$2);

					$$ = (($2 << 16) | $4) + VARHDRSZ;
				}
		| '(' Iconst ')'
				{
					if ($2 < 1 || $2 > NUMERIC_MAX_PRECISION)
						elog(ERROR,"NUMERIC precision %d must be beween 1 and %d",
									$2, NUMERIC_MAX_PRECISION);

					$$ = ($2 << 16) + VARHDRSZ;
				}
		| /*EMPTY*/
				{
					/* Insert "-1" meaning "default"; may be replaced later */
					$$ = -1;
				}
		;

opt_decimal:  '(' Iconst ',' Iconst ')'
				{
					if ($2 < 1 || $2 > NUMERIC_MAX_PRECISION)
						elog(ERROR,"DECIMAL precision %d must be beween 1 and %d",
									$2, NUMERIC_MAX_PRECISION);
					if ($4 < 0 || $4 > $2)
						elog(ERROR,"DECIMAL scale %d must be between 0 and precision %d",
									$4,$2);

					$$ = (($2 << 16) | $4) + VARHDRSZ;
				}
		| '(' Iconst ')'
				{
					if ($2 < 1 || $2 > NUMERIC_MAX_PRECISION)
						elog(ERROR,"DECIMAL precision %d must be beween 1 and %d",
									$2, NUMERIC_MAX_PRECISION);

					$$ = ($2 << 16) + VARHDRSZ;
				}
		| /*EMPTY*/
				{
					/* Insert "-1" meaning "default"; may be replaced later */
					$$ = -1;
				}
		;


/*
 * SQL92 bit-field data types
 * The following implements BIT() and BIT VARYING().
 */
Bit:  bit '(' Iconst ')'
				{
					$$ = makeNode(TypeName);
					$$->name = $1;
					if ($3 < 1)
						elog(ERROR,"length for type '%s' must be at least 1",
							 $1);
					else if ($3 > (MaxAttrSize * BITS_PER_BYTE))
						elog(ERROR,"length for type '%s' cannot exceed %d",
							 $1, (MaxAttrSize * BITS_PER_BYTE));
					$$->typmod = $3;
				}
		| bit
				{
					$$ = makeNode(TypeName);
					$$->name = $1;
					/* default length, if needed, will be inserted later */
					$$->typmod = -1;
				}
		;

bit:  BIT opt_varying
				{
					char *type;

					if ($2) type = xlateSqlType("varbit");
					else type = xlateSqlType("bit");
					$$ = type;
				}


/*
 * SQL92 character data types
 * The following implements CHAR() and VARCHAR().
 */
Character:  character '(' Iconst ')'
				{
					$$ = makeNode(TypeName);
					$$->name = $1;
					if ($3 < 1)
						elog(ERROR,"length for type '%s' must be at least 1",
							 $1);
					else if ($3 > MaxAttrSize)
						elog(ERROR,"length for type '%s' cannot exceed %d",
							 $1, MaxAttrSize);

					/* we actually implement these like a varlen, so
					 * the first 4 bytes is the length. (the difference
					 * between these and "text" is that we blank-pad and
					 * truncate where necessary)
					 */
					$$->typmod = VARHDRSZ + $3;
				}
		| character
				{
					$$ = makeNode(TypeName);
					$$->name = $1;
					/* default length, if needed, will be inserted later */
					$$->typmod = -1;
				}
		;

character:  CHARACTER opt_varying opt_charset
				{
					char *type, *c;
					if (($3 == NULL) || (strcmp($3, "sql_text") == 0)) {
						if ($2) type = xlateSqlType("varchar");
						else type = xlateSqlType("bpchar");
					} else {
						if ($2) {
							c = palloc(strlen("var") + strlen($3) + 1);
							strcpy(c, "var");
							strcat(c, $3);
							type = xlateSqlType(c);
						} else {
							type = xlateSqlType($3);
						}
					};
					$$ = type;
				}
		| CHAR opt_varying						{ $$ = xlateSqlType($2 ? "varchar": "bpchar"); }
		| VARCHAR								{ $$ = xlateSqlType("varchar"); }
		| NATIONAL CHARACTER opt_varying		{ $$ = xlateSqlType($3 ? "varchar": "bpchar"); }
		| NATIONAL CHAR opt_varying				{ $$ = xlateSqlType($3 ? "varchar": "bpchar"); }
		| NCHAR opt_varying						{ $$ = xlateSqlType($2 ? "varchar": "bpchar"); }
		;

opt_varying:  VARYING							{ $$ = TRUE; }
		| /*EMPTY*/								{ $$ = FALSE; }
		;

opt_charset:  CHARACTER SET ColId				{ $$ = $3; }
		| /*EMPTY*/								{ $$ = NULL; }
		;

opt_collate:  COLLATE ColId						{ $$ = $2; }
		| /*EMPTY*/								{ $$ = NULL; }
		;

ConstDatetime:  datetime
				{
					$$ = makeNode(TypeName);
					$$->name = xlateSqlType($1);
					$$->typmod = -1;
				}
		| TIMESTAMP opt_timezone
				{
					$$ = makeNode(TypeName);
					$$->name = xlateSqlType("timestamp");
					$$->timezone = $2;
					$$->typmod = -1;
				}
		| TIME opt_timezone
				{
					$$ = makeNode(TypeName);
					if ($2)
						$$->name = xlateSqlType("timetz");
					else
						$$->name = xlateSqlType("time");
					$$->typmod = -1;
				}
		;

ConstInterval:  INTERVAL opt_interval
				{
					$$ = makeNode(TypeName);
					$$->name = xlateSqlType("interval");
					$$->typmod = -1;
				}
		;

datetime:  YEAR_P								{ $$ = "year"; }
		| MONTH_P								{ $$ = "month"; }
		| DAY_P									{ $$ = "day"; }
		| HOUR_P								{ $$ = "hour"; }
		| MINUTE_P								{ $$ = "minute"; }
		| SECOND_P								{ $$ = "second"; }
		;

opt_timezone:  WITH TIME ZONE					{ $$ = TRUE; }
		| WITHOUT TIME ZONE						{ $$ = FALSE; }
		| /*EMPTY*/								{ $$ = FALSE; }
		;

opt_interval:  datetime							{ $$ = makeList1($1); }
		| YEAR_P TO MONTH_P						{ $$ = NIL; }
		| DAY_P TO HOUR_P						{ $$ = NIL; }
		| DAY_P TO MINUTE_P						{ $$ = NIL; }
		| DAY_P TO SECOND_P						{ $$ = NIL; }
		| HOUR_P TO MINUTE_P					{ $$ = NIL; }
		| HOUR_P TO SECOND_P					{ $$ = NIL; }
		| MINUTE_P TO SECOND_P					{ $$ = NIL; }
		| /*EMPTY*/								{ $$ = NIL; }
		;


/*****************************************************************************
 *
 *	expression grammar
 *
 *****************************************************************************/

/* Expressions using row descriptors
 * Define row_descriptor to allow yacc to break the reduce/reduce conflict
 *  with singleton expressions.
 */
row_expr: '(' row_descriptor ')' IN '(' select_subclause ')'
				{
					SubLink *n = makeNode(SubLink);
					n->lefthand = $2;
					n->oper = (List *) makeA_Expr(OP, "=", NULL, NULL);
					n->useor = FALSE;
					n->subLinkType = ANY_SUBLINK;
					n->subselect = $6;
					$$ = (Node *)n;
				}
		| '(' row_descriptor ')' NOT IN '(' select_subclause ')'
				{
					SubLink *n = makeNode(SubLink);
					n->lefthand = $2;
					n->oper = (List *) makeA_Expr(OP, "<>", NULL, NULL);
					n->useor = TRUE;
					n->subLinkType = ALL_SUBLINK;
					n->subselect = $7;
					$$ = (Node *)n;
				}
		| '(' row_descriptor ')' all_Op sub_type '(' select_subclause ')'
				{
					SubLink *n = makeNode(SubLink);
					n->lefthand = $2;
					n->oper = (List *) makeA_Expr(OP, $4, NULL, NULL);
					if (strcmp($4, "<>") == 0)
						n->useor = TRUE;
					else
						n->useor = FALSE;
					n->subLinkType = $5;
					n->subselect = $7;
					$$ = (Node *)n;
				}
		| '(' row_descriptor ')' all_Op '(' select_subclause ')'
				{
					SubLink *n = makeNode(SubLink);
					n->lefthand = $2;
					n->oper = (List *) makeA_Expr(OP, $4, NULL, NULL);
					if (strcmp($4, "<>") == 0)
						n->useor = TRUE;
					else
						n->useor = FALSE;
					n->subLinkType = MULTIEXPR_SUBLINK;
					n->subselect = $6;
					$$ = (Node *)n;
				}
		| '(' row_descriptor ')' all_Op '(' row_descriptor ')'
				{
					$$ = makeRowExpr($4, $2, $6);
				}
		| '(' row_descriptor ')' OVERLAPS '(' row_descriptor ')'
				{
					FuncCall *n = makeNode(FuncCall);
					List *largs = $2;
					List *rargs = $6;
					n->funcname = xlateSqlFunc("overlaps");
					if (length(largs) == 1)
						largs = lappend(largs, $2);
					else if (length(largs) != 2)
						elog(ERROR, "Wrong number of parameters"
							 " on left side of OVERLAPS expression");
					if (length(rargs) == 1)
						rargs = lappend(rargs, $6);
					else if (length(rargs) != 2)
						elog(ERROR, "Wrong number of parameters"
							 " on right side of OVERLAPS expression");
					n->args = nconc(largs, rargs);
					n->agg_star = FALSE;
					n->agg_distinct = FALSE;
					$$ = (Node *)n;
				}
		;

row_descriptor:  row_list ',' a_expr
				{
					$$ = lappend($1, $3);
				}
		;

row_list:  row_list ',' a_expr
				{
					$$ = lappend($1, $3);
				}
		| a_expr
				{
					$$ = makeList1($1);
				}
		;

sub_type:  ANY								{ $$ = ANY_SUBLINK; }
		| SOME								{ $$ = ANY_SUBLINK; }
		| ALL								{ $$ = ALL_SUBLINK; }
		;

all_Op:  Op | MathOp;

MathOp:  '+'			{ $$ = "+"; }
		| '-'			{ $$ = "-"; }
		| '*'			{ $$ = "*"; }
		| '/'			{ $$ = "/"; }
		| '%'			{ $$ = "%"; }
		| '^'			{ $$ = "^"; }
		| '|'			{ $$ = "|"; }
		| '<'			{ $$ = "<"; }
		| '>'			{ $$ = ">"; }
		| '='			{ $$ = "="; }
		;

/*
 * General expressions
 * This is the heart of the expression syntax.
 *
 * We have two expression types: a_expr is the unrestricted kind, and
 * b_expr is a subset that must be used in some places to avoid shift/reduce
 * conflicts.  For example, we can't do BETWEEN as "BETWEEN a_expr AND a_expr"
 * because that use of AND conflicts with AND as a boolean operator.  So,
 * b_expr is used in BETWEEN and we remove boolean keywords from b_expr.
 *
 * Note that '(' a_expr ')' is a b_expr, so an unrestricted expression can
 * always be used by surrounding it with parens.
 *
 * c_expr is all the productions that are common to a_expr and b_expr;
 * it's factored out just to eliminate redundant coding.
 */
a_expr:  c_expr
				{	$$ = $1;  }
		| a_expr TYPECAST Typename
				{	$$ = makeTypeCast($1, $3); }
		/*
		 * These operators must be called out explicitly in order to make use
		 * of yacc/bison's automatic operator-precedence handling.  All other
		 * operator names are handled by the generic productions using "Op",
		 * below; and all those operators will have the same precedence.
		 *
		 * If you add more explicitly-known operators, be sure to add them
		 * also to b_expr and to the MathOp list above.
		 */
		| '+' a_expr %prec UMINUS
				{	$$ = makeA_Expr(OP, "+", NULL, $2); }
		| '-' a_expr %prec UMINUS
				{	$$ = doNegate($2); }
		| '%' a_expr
				{	$$ = makeA_Expr(OP, "%", NULL, $2); }
		| '^' a_expr
				{	$$ = makeA_Expr(OP, "^", NULL, $2); }
		| '|' a_expr
				{	$$ = makeA_Expr(OP, "|", NULL, $2); }
		| a_expr '%'
				{	$$ = makeA_Expr(OP, "%", $1, NULL); }
		| a_expr '^'
				{	$$ = makeA_Expr(OP, "^", $1, NULL); }
		| a_expr '|'
				{	$$ = makeA_Expr(OP, "|", $1, NULL); }
		| a_expr '+' a_expr
				{	$$ = makeA_Expr(OP, "+", $1, $3); }
		| a_expr '-' a_expr
				{	$$ = makeA_Expr(OP, "-", $1, $3); }
		| a_expr '*' a_expr
				{	$$ = makeA_Expr(OP, "*", $1, $3); }
		| a_expr '/' a_expr
				{	$$ = makeA_Expr(OP, "/", $1, $3); }
		| a_expr '%' a_expr
				{	$$ = makeA_Expr(OP, "%", $1, $3); }
		| a_expr '^' a_expr
				{	$$ = makeA_Expr(OP, "^", $1, $3); }
		| a_expr '|' a_expr
				{	$$ = makeA_Expr(OP, "|", $1, $3); }
		| a_expr '<' a_expr
				{	$$ = makeA_Expr(OP, "<", $1, $3); }
		| a_expr '>' a_expr
				{	$$ = makeA_Expr(OP, ">", $1, $3); }
		| a_expr '=' a_expr
				{
					/*
					 * Special-case "foo = NULL" and "NULL = foo" for
					 * compatibility with standards-broken products
					 * (like Microsoft's).  Turn these into IS NULL exprs.
					 */
					if (exprIsNullConstant($3))
						$$ = makeA_Expr(ISNULL, NULL, $1, NULL);
					else if (exprIsNullConstant($1))
						$$ = makeA_Expr(ISNULL, NULL, $3, NULL);
					else
						$$ = makeA_Expr(OP, "=", $1, $3);
				}

		| a_expr Op a_expr
				{	$$ = makeA_Expr(OP, $2, $1, $3); }
		| Op a_expr
				{	$$ = makeA_Expr(OP, $1, NULL, $2); }
		| a_expr Op
				{	$$ = makeA_Expr(OP, $2, $1, NULL); }

		| a_expr AND a_expr
				{	$$ = makeA_Expr(AND, NULL, $1, $3); }
		| a_expr OR a_expr
				{	$$ = makeA_Expr(OR, NULL, $1, $3); }
		| NOT a_expr
				{	$$ = makeA_Expr(NOT, NULL, NULL, $2); }

		| a_expr LIKE a_expr
				{	$$ = makeA_Expr(OP, "~~", $1, $3); }
		| a_expr LIKE a_expr ESCAPE a_expr
				{
					FuncCall *n = makeNode(FuncCall);
					n->funcname = "like_escape";
					n->args = makeList2($3, $5);
					n->agg_star = FALSE;
					n->agg_distinct = FALSE;
					$$ = makeA_Expr(OP, "~~", $1, (Node *) n);
				}
		| a_expr NOT LIKE a_expr
				{	$$ = makeA_Expr(OP, "!~~", $1, $4); }
		| a_expr NOT LIKE a_expr ESCAPE a_expr
				{
					FuncCall *n = makeNode(FuncCall);
					n->funcname = "like_escape";
					n->args = makeList2($4, $6);
					n->agg_star = FALSE;
					n->agg_distinct = FALSE;
					$$ = makeA_Expr(OP, "!~~", $1, (Node *) n);
				}
		| a_expr ILIKE a_expr
				{	$$ = makeA_Expr(OP, "~~*", $1, $3); }
		| a_expr ILIKE a_expr ESCAPE a_expr
				{
					FuncCall *n = makeNode(FuncCall);
					n->funcname = "like_escape";
					n->args = makeList2($3, $5);
					n->agg_star = FALSE;
					n->agg_distinct = FALSE;
					$$ = makeA_Expr(OP, "~~*", $1, (Node *) n);
				}
		| a_expr NOT ILIKE a_expr
				{	$$ = makeA_Expr(OP, "!~~*", $1, $4); }
		| a_expr NOT ILIKE a_expr ESCAPE a_expr
				{
					FuncCall *n = makeNode(FuncCall);
					n->funcname = "like_escape";
					n->args = makeList2($4, $6);
					n->agg_star = FALSE;
					n->agg_distinct = FALSE;
					$$ = makeA_Expr(OP, "!~~*", $1, (Node *) n);
				}

		| a_expr ISNULL
				{	$$ = makeA_Expr(ISNULL, NULL, $1, NULL); }
		| a_expr IS NULL_P
				{	$$ = makeA_Expr(ISNULL, NULL, $1, NULL); }
		| a_expr NOTNULL
				{	$$ = makeA_Expr(NOTNULL, NULL, $1, NULL); }
		| a_expr IS NOT NULL_P
				{	$$ = makeA_Expr(NOTNULL, NULL, $1, NULL); }
		/* IS TRUE, IS FALSE, etc used to be function calls
		 *  but let's make them expressions to allow the optimizer
		 *  a chance to eliminate them if a_expr is a constant string.
		 * - thomas 1997-12-22
		 */
		| a_expr IS TRUE_P
				{
					A_Const *n = makeNode(A_Const);
					n->val.type = T_String;
					n->val.val.str = "t";
					n->typename = makeNode(TypeName);
					n->typename->name = xlateSqlType("bool");
					n->typename->typmod = -1;
					$$ = makeA_Expr(OP, "=", $1,(Node *)n);
				}
		| a_expr IS NOT FALSE_P
				{
					A_Const *n = makeNode(A_Const);
					n->val.type = T_String;
					n->val.val.str = "t";
					n->typename = makeNode(TypeName);
					n->typename->name = xlateSqlType("bool");
					n->typename->typmod = -1;
					$$ = makeA_Expr(OP, "=", $1,(Node *)n);
				}
		| a_expr IS FALSE_P
				{
					A_Const *n = makeNode(A_Const);
					n->val.type = T_String;
					n->val.val.str = "f";
					n->typename = makeNode(TypeName);
					n->typename->name = xlateSqlType("bool");
					n->typename->typmod = -1;
					$$ = makeA_Expr(OP, "=", $1,(Node *)n);
				}
		| a_expr IS NOT TRUE_P
				{
					A_Const *n = makeNode(A_Const);
					n->val.type = T_String;
					n->val.val.str = "f";
					n->typename = makeNode(TypeName);
					n->typename->name = xlateSqlType("bool");
					n->typename->typmod = -1;
					$$ = makeA_Expr(OP, "=", $1,(Node *)n);
				}
		| a_expr BETWEEN b_expr AND b_expr
				{
					$$ = makeA_Expr(AND, NULL,
						makeA_Expr(OP, ">=", $1, $3),
						makeA_Expr(OP, "<=", $1, $5));
				}
		| a_expr NOT BETWEEN b_expr AND b_expr
				{
					$$ = makeA_Expr(OR, NULL,
						makeA_Expr(OP, "<", $1, $4),
						makeA_Expr(OP, ">", $1, $6));
				}
		| a_expr IN '(' in_expr ')'
				{
					/* in_expr returns a SubLink or a list of a_exprs */
					if (IsA($4, SubLink))
					{
							SubLink *n = (SubLink *)$4;
							n->lefthand = makeList1($1);
							n->oper = (List *) makeA_Expr(OP, "=", NULL, NULL);
							n->useor = FALSE;
							n->subLinkType = ANY_SUBLINK;
							$$ = (Node *)n;
					}
					else
					{
						Node *n = NULL;
						List *l;
						foreach(l, (List *) $4)
						{
							Node *cmp = makeA_Expr(OP, "=", $1, lfirst(l));
							if (n == NULL)
								n = cmp;
							else
								n = makeA_Expr(OR, NULL, n, cmp);
						}
						$$ = n;
					}
				}
		| a_expr NOT IN '(' in_expr ')'
				{
					/* in_expr returns a SubLink or a list of a_exprs */
					if (IsA($5, SubLink))
					{
						SubLink *n = (SubLink *)$5;
						n->lefthand = makeList1($1);
						n->oper = (List *) makeA_Expr(OP, "<>", NULL, NULL);
						n->useor = FALSE;
						n->subLinkType = ALL_SUBLINK;
						$$ = (Node *)n;
					}
					else
					{
						Node *n = NULL;
						List *l;
						foreach(l, (List *) $5)
						{
							Node *cmp = makeA_Expr(OP, "<>", $1, lfirst(l));
							if (n == NULL)
								n = cmp;
							else
								n = makeA_Expr(AND, NULL, n, cmp);
						}
						$$ = n;
					}
				}
		| a_expr all_Op sub_type '(' select_subclause ')'
				{
					SubLink *n = makeNode(SubLink);
					n->lefthand = makeList1($1);
					n->oper = (List *) makeA_Expr(OP, $2, NULL, NULL);
					n->useor = FALSE; /* doesn't matter since only one col */
					n->subLinkType = $3;
					n->subselect = $5;
					$$ = (Node *)n;
				}
		| row_expr
				{	$$ = $1;  }
		;

/*
 * Restricted expressions
 *
 * b_expr is a subset of the complete expression syntax defined by a_expr.
 *
 * Presently, AND, NOT, IS, and IN are the a_expr keywords that would
 * cause trouble in the places where b_expr is used.  For simplicity, we
 * just eliminate all the boolean-keyword-operator productions from b_expr.
 */
b_expr:  c_expr
				{	$$ = $1;  }
		| b_expr TYPECAST Typename
				{	$$ = makeTypeCast($1, $3); }
		| '+' b_expr %prec UMINUS
				{	$$ = makeA_Expr(OP, "+", NULL, $2); }
		| '-' b_expr %prec UMINUS
				{	$$ = doNegate($2); }
		| '%' b_expr
				{	$$ = makeA_Expr(OP, "%", NULL, $2); }
		| '^' b_expr
				{	$$ = makeA_Expr(OP, "^", NULL, $2); }
		| '|' b_expr
				{	$$ = makeA_Expr(OP, "|", NULL, $2); }
		| b_expr '%'
				{	$$ = makeA_Expr(OP, "%", $1, NULL); }
		| b_expr '^'
				{	$$ = makeA_Expr(OP, "^", $1, NULL); }
		| b_expr '|'
				{	$$ = makeA_Expr(OP, "|", $1, NULL); }
		| b_expr '+' b_expr
				{	$$ = makeA_Expr(OP, "+", $1, $3); }
		| b_expr '-' b_expr
				{	$$ = makeA_Expr(OP, "-", $1, $3); }
		| b_expr '*' b_expr
				{	$$ = makeA_Expr(OP, "*", $1, $3); }
		| b_expr '/' b_expr
				{	$$ = makeA_Expr(OP, "/", $1, $3); }
		| b_expr '%' b_expr
				{	$$ = makeA_Expr(OP, "%", $1, $3); }
		| b_expr '^' b_expr
				{	$$ = makeA_Expr(OP, "^", $1, $3); }
		| b_expr '|' b_expr
				{	$$ = makeA_Expr(OP, "|", $1, $3); }
		| b_expr '<' b_expr
				{	$$ = makeA_Expr(OP, "<", $1, $3); }
		| b_expr '>' b_expr
				{	$$ = makeA_Expr(OP, ">", $1, $3); }
		| b_expr '=' b_expr
				{	$$ = makeA_Expr(OP, "=", $1, $3); }

		| b_expr Op b_expr
				{	$$ = makeA_Expr(OP, $2, $1, $3); }
		| Op b_expr
				{	$$ = makeA_Expr(OP, $1, NULL, $2); }
		| b_expr Op
				{	$$ = makeA_Expr(OP, $2, $1, NULL); }
		;

/*
 * Productions that can be used in both a_expr and b_expr.
 *
 * Note: productions that refer recursively to a_expr or b_expr mostly
 * cannot appear here.  However, it's OK to refer to a_exprs that occur
 * inside parentheses, such as function arguments; that cannot introduce
 * ambiguity to the b_expr syntax.
 */
c_expr:  attr
				{	$$ = (Node *) $1;  }
		| ColId opt_indirection
				{
					/* could be a column name or a relation_name */
					Ident *n = makeNode(Ident);
					n->name = $1;
					n->indirection = $2;
					$$ = (Node *)n;
				}
		| AexprConst
				{	$$ = $1;  }
		| '(' a_expr ')'
				{	$$ = $2; }
		| CAST '(' a_expr AS Typename ')'
				{	$$ = makeTypeCast($3, $5); }
		| case_expr
				{	$$ = $1; }
		| func_name '(' ')'
				{
					FuncCall *n = makeNode(FuncCall);
					n->funcname = $1;
					n->args = NIL;
					n->agg_star = FALSE;
					n->agg_distinct = FALSE;
					$$ = (Node *)n;
				}
		| func_name '(' expr_list ')'
				{
					FuncCall *n = makeNode(FuncCall);
					n->funcname = $1;
					n->args = $3;
					n->agg_star = FALSE;
					n->agg_distinct = FALSE;
					$$ = (Node *)n;
				}
		| func_name '(' ALL expr_list ')'
				{
					FuncCall *n = makeNode(FuncCall);
					n->funcname = $1;
					n->args = $4;
					n->agg_star = FALSE;
					n->agg_distinct = FALSE;
					/* Ideally we'd mark the FuncCall node to indicate
					 * "must be an aggregate", but there's no provision
					 * for that in FuncCall at the moment.
					 */
					$$ = (Node *)n;
				}
		| func_name '(' DISTINCT expr_list ')'
				{
					FuncCall *n = makeNode(FuncCall);
					n->funcname = $1;
					n->args = $4;
					n->agg_star = FALSE;
					n->agg_distinct = TRUE;
					$$ = (Node *)n;
				}
		| func_name '(' '*' ')'
				{
					/*
					 * For now, we transform AGGREGATE(*) into AGGREGATE(1).
					 *
					 * This does the right thing for COUNT(*) (in fact,
					 * any certainly-non-null expression would do for COUNT),
					 * and there are no other aggregates in SQL92 that accept
					 * '*' as parameter.
					 *
					 * The FuncCall node is also marked agg_star = true,
					 * so that later processing can detect what the argument
					 * really was.
					 */
					FuncCall *n = makeNode(FuncCall);
					A_Const *star = makeNode(A_Const);

					star->val.type = T_Integer;
					star->val.val.ival = 1;
					n->funcname = $1;
					n->args = makeList1(star);
					n->agg_star = TRUE;
					n->agg_distinct = FALSE;
					$$ = (Node *)n;
				}
		| CURRENT_DATE
				{
					/*
					 * Translate as "date('now'::text)".
					 *
					 * We cannot use "'now'::date" because coerce_type() will
					 * immediately reduce that to a constant representing
					 * today's date.  We need to delay the conversion until
					 * runtime, else the wrong things will happen when
					 * CURRENT_DATE is used in a column default value or rule.
					 *
					 * This could be simplified if we had a way to generate
					 * an expression tree representing runtime application
					 * of type-input conversion functions...
					 */
					A_Const *s = makeNode(A_Const);
					TypeName *t = makeNode(TypeName);
					FuncCall *n = makeNode(FuncCall);

					s->val.type = T_String;
					s->val.val.str = "now";
					s->typename = t;

					t->name = xlateSqlType("text");
					t->setof = FALSE;
					t->typmod = -1;

					n->funcname = xlateSqlType("date");
					n->args = makeList1(s);
					n->agg_star = FALSE;
					n->agg_distinct = FALSE;

					$$ = (Node *)n;
				}
		| CURRENT_TIME
				{
					/*
					 * Translate as "time('now'::text)".
					 * See comments for CURRENT_DATE.
					 */
					A_Const *s = makeNode(A_Const);
					TypeName *t = makeNode(TypeName);
					FuncCall *n = makeNode(FuncCall);

					s->val.type = T_String;
					s->val.val.str = "now";
					s->typename = t;

					t->name = xlateSqlType("text");
					t->setof = FALSE;
					t->typmod = -1;

					n->funcname = xlateSqlType("time");
					n->args = makeList1(s);
					n->agg_star = FALSE;
					n->agg_distinct = FALSE;

					$$ = (Node *)n;
				}
		| CURRENT_TIME '(' Iconst ')'
				{
					/*
					 * Translate as "time('now'::text)".
					 * See comments for CURRENT_DATE.
					 */
					A_Const *s = makeNode(A_Const);
					TypeName *t = makeNode(TypeName);
					FuncCall *n = makeNode(FuncCall);

					s->val.type = T_String;
					s->val.val.str = "now";
					s->typename = t;

					t->name = xlateSqlType("text");
					t->setof = FALSE;
					t->typmod = -1;

					n->funcname = xlateSqlType("time");
					n->args = makeList1(s);
					n->agg_star = FALSE;
					n->agg_distinct = FALSE;

					if ($3 != 0)
						elog(NOTICE,"CURRENT_TIME(%d) precision not implemented"
							 "; zero used instead",$3);

					$$ = (Node *)n;
				}
		| CURRENT_TIMESTAMP
				{
					/*
					 * Translate as "timestamp('now'::text)".
					 * See comments for CURRENT_DATE.
					 */
					A_Const *s = makeNode(A_Const);
					TypeName *t = makeNode(TypeName);
					FuncCall *n = makeNode(FuncCall);

					s->val.type = T_String;
					s->val.val.str = "now";
					s->typename = t;

					t->name = xlateSqlType("text");
					t->setof = FALSE;
					t->typmod = -1;

					n->funcname = xlateSqlType("timestamp");
					n->args = makeList1(s);
					n->agg_star = FALSE;
					n->agg_distinct = FALSE;

					$$ = (Node *)n;
				}
		| CURRENT_TIMESTAMP '(' Iconst ')'
				{
					/*
					 * Translate as "timestamp('now'::text)".
					 * See comments for CURRENT_DATE.
					 */
					A_Const *s = makeNode(A_Const);
					TypeName *t = makeNode(TypeName);
					FuncCall *n = makeNode(FuncCall);

					s->val.type = T_String;
					s->val.val.str = "now";
					s->typename = t;

					t->name = xlateSqlType("text");
					t->setof = FALSE;
					t->typmod = -1;

					n->funcname = xlateSqlType("timestamp");
					n->args = makeList1(s);
					n->agg_star = FALSE;
					n->agg_distinct = FALSE;

					if ($3 != 0)
						elog(NOTICE,"CURRENT_TIMESTAMP(%d) precision not implemented"
							 "; zero used instead",$3);

					$$ = (Node *)n;
				}
		| CURRENT_USER
				{
					FuncCall *n = makeNode(FuncCall);
					n->funcname = "current_user";
					n->args = NIL;
					n->agg_star = FALSE;
					n->agg_distinct = FALSE;
					$$ = (Node *)n;
				}
		| SESSION_USER
				{
					FuncCall *n = makeNode(FuncCall);
					n->funcname = "session_user";
					n->args = NIL;
					n->agg_star = FALSE;
					n->agg_distinct = FALSE;
					$$ = (Node *)n;
				}
		| USER
				{
					FuncCall *n = makeNode(FuncCall);
					n->funcname = "current_user";
					n->args = NIL;
					n->agg_star = FALSE;
					n->agg_distinct = FALSE;
					$$ = (Node *)n;
				}
		| EXTRACT '(' extract_list ')'
				{
					FuncCall *n = makeNode(FuncCall);
					n->funcname = "date_part";
					n->args = $3;
					n->agg_star = FALSE;
					n->agg_distinct = FALSE;
					$$ = (Node *)n;
				}
		| POSITION '(' position_list ')'
				{
					FuncCall *n = makeNode(FuncCall);
					n->funcname = "strpos";
					n->args = $3;
					n->agg_star = FALSE;
					n->agg_distinct = FALSE;
					$$ = (Node *)n;
				}
		| SUBSTRING '(' substr_list ')'
				{
					FuncCall *n = makeNode(FuncCall);
					n->funcname = "substr";
					n->args = $3;
					n->agg_star = FALSE;
					n->agg_distinct = FALSE;
					$$ = (Node *)n;
				}
		/* various trim expressions are defined in SQL92 - thomas 1997-07-19 */
		| TRIM '(' BOTH trim_list ')'
				{
					FuncCall *n = makeNode(FuncCall);
					n->funcname = "btrim";
					n->args = $4;
					n->agg_star = FALSE;
					n->agg_distinct = FALSE;
					$$ = (Node *)n;
				}
		| TRIM '(' LEADING trim_list ')'
				{
					FuncCall *n = makeNode(FuncCall);
					n->funcname = "ltrim";
					n->args = $4;
					n->agg_star = FALSE;
					n->agg_distinct = FALSE;
					$$ = (Node *)n;
				}
		| TRIM '(' TRAILING trim_list ')'
				{
					FuncCall *n = makeNode(FuncCall);
					n->funcname = "rtrim";
					n->args = $4;
					n->agg_star = FALSE;
					n->agg_distinct = FALSE;
					$$ = (Node *)n;
				}
		| TRIM '(' trim_list ')'
				{
					FuncCall *n = makeNode(FuncCall);
					n->funcname = "btrim";
					n->args = $3;
					n->agg_star = FALSE;
					n->agg_distinct = FALSE;
					$$ = (Node *)n;
				}
		| '(' select_subclause ')'
				{
					SubLink *n = makeNode(SubLink);
					n->lefthand = NIL;
					n->oper = NIL;
					n->useor = FALSE;
					n->subLinkType = EXPR_SUBLINK;
					n->subselect = $2;
					$$ = (Node *)n;
				}
		| EXISTS '(' select_subclause ')'
				{
					SubLink *n = makeNode(SubLink);
					n->lefthand = NIL;
					n->oper = NIL;
					n->useor = FALSE;
					n->subLinkType = EXISTS_SUBLINK;
					n->subselect = $3;
					$$ = (Node *)n;
				}
		;

/*
 * Supporting nonterminals for expressions.
 */

opt_indirection:	opt_indirection '[' a_expr ']'
				{
					A_Indices *ai = makeNode(A_Indices);
					ai->lidx = NULL;
					ai->uidx = $3;
					$$ = lappend($1, ai);
				}
		| opt_indirection '[' a_expr ':' a_expr ']'
				{
					A_Indices *ai = makeNode(A_Indices);
					ai->lidx = $3;
					ai->uidx = $5;
					$$ = lappend($1, ai);
				}
		| /*EMPTY*/
				{	$$ = NIL; }
		;

expr_list:  a_expr
				{ $$ = makeList1($1); }
		| expr_list ',' a_expr
				{ $$ = lappend($1, $3); }
		| expr_list USING a_expr
				{ $$ = lappend($1, $3); }
		;

extract_list:  extract_arg FROM a_expr
				{
					A_Const *n = makeNode(A_Const);
					n->val.type = T_String;
					n->val.val.str = $1;
					$$ = makeList2((Node *) n, $3);
				}
		| /*EMPTY*/
				{	$$ = NIL; }
		;

extract_arg:  datetime						{ $$ = $1; }
		| TIMEZONE_HOUR						{ $$ = "tz_hour"; }
		| TIMEZONE_MINUTE					{ $$ = "tz_minute"; }
		;

/* position_list uses b_expr not a_expr to avoid conflict with general IN */

position_list:  b_expr IN b_expr
				{	$$ = makeList2($3, $1); }
		| /*EMPTY*/
				{	$$ = NIL; }
		;

substr_list:  expr_list substr_from substr_for
				{
					$$ = nconc(nconc($1,$2),$3);
				}
		| /*EMPTY*/
				{	$$ = NIL; }
		;

substr_from:  FROM expr_list
				{	$$ = $2; }
		| /*EMPTY*/
				{
					A_Const *n = makeNode(A_Const);
					n->val.type = T_Integer;
					n->val.val.ival = 1;
					$$ = makeList1((Node *)n);
				}
		;

substr_for:  FOR expr_list
				{	$$ = $2; }
		| /*EMPTY*/
				{	$$ = NIL; }
		;

trim_list:  a_expr FROM expr_list
				{ $$ = lappend($3, $1); }
		| FROM expr_list
				{ $$ = $2; }
		| expr_list
				{ $$ = $1; }
		;

in_expr:  select_subclause
				{
					SubLink *n = makeNode(SubLink);
					n->subselect = $1;
					$$ = (Node *)n;
				}
		| in_expr_nodes
				{	$$ = (Node *)$1; }
		;

in_expr_nodes:  a_expr
				{	$$ = makeList1($1); }
		| in_expr_nodes ',' a_expr
				{	$$ = lappend($1, $3); }
		;

/* Case clause
 * Define SQL92-style case clause.
 * Allow all four forms described in the standard:
 * - Full specification
 *  CASE WHEN a = b THEN c ... ELSE d END
 * - Implicit argument
 *  CASE a WHEN b THEN c ... ELSE d END
 * - Conditional NULL
 *  NULLIF(x,y)
 *  same as CASE WHEN x = y THEN NULL ELSE x END
 * - Conditional substitution from list, use first non-null argument
 *  COALESCE(a,b,...)
 * same as CASE WHEN a IS NOT NULL THEN a WHEN b IS NOT NULL THEN b ... END
 * - thomas 1998-11-09
 */
case_expr:  CASE case_arg when_clause_list case_default END_TRANS
				{
					CaseExpr *c = makeNode(CaseExpr);
					c->arg = $2;
					c->args = $3;
					c->defresult = $4;
					$$ = (Node *)c;
				}
		| NULLIF '(' a_expr ',' a_expr ')'
				{
					CaseExpr *c = makeNode(CaseExpr);
					CaseWhen *w = makeNode(CaseWhen);
/*
					A_Const *n = makeNode(A_Const);
					n->val.type = T_Null;
					w->result = (Node *)n;
*/
					w->expr = makeA_Expr(OP, "=", $3, $5);
					c->args = makeList1(w);
					c->defresult = $3;
					$$ = (Node *)c;
				}
		| COALESCE '(' expr_list ')'
				{
					CaseExpr *c = makeNode(CaseExpr);
					CaseWhen *w;
					List *l;
					foreach (l,$3)
					{
						w = makeNode(CaseWhen);
						w->expr = makeA_Expr(NOTNULL, NULL, lfirst(l), NULL);
						w->result = lfirst(l);
						c->args = lappend(c->args, w);
					}
					$$ = (Node *)c;
				}
		;

when_clause_list:  when_clause_list when_clause
				{ $$ = lappend($1, $2); }
		| when_clause
				{ $$ = makeList1($1); }
		;

when_clause:  WHEN a_expr THEN a_expr
				{
					CaseWhen *w = makeNode(CaseWhen);
					w->expr = $2;
					w->result = $4;
					$$ = (Node *)w;
				}
		;

case_default:  ELSE a_expr						{ $$ = $2; }
		| /*EMPTY*/								{ $$ = NULL; }
		;

case_arg:  a_expr
				{	$$ = $1; }
		| /*EMPTY*/
				{	$$ = NULL; }
		;

attr:  relation_name '.' attrs opt_indirection
				{
					$$ = makeNode(Attr);
					$$->relname = $1;
					$$->paramNo = NULL;
					$$->attrs = $3;
					$$->indirection = $4;
				}
		| ParamNo '.' attrs opt_indirection
				{
					$$ = makeNode(Attr);
					$$->relname = NULL;
					$$->paramNo = $1;
					$$->attrs = $3;
					$$->indirection = $4;
				}
		;

attrs:	  attr_name
				{ $$ = makeList1(makeString($1)); }
		| attrs '.' attr_name
				{ $$ = lappend($1, makeString($3)); }
		| attrs '.' '*'
				{ $$ = lappend($1, makeString("*")); }
		;


/*****************************************************************************
 *
 *	target lists
 *
 *****************************************************************************/

/* Target lists as found in SELECT ... and INSERT VALUES ( ... ) */

target_list:  target_list ',' target_el
				{	$$ = lappend($1, $3);  }
		| target_el
				{	$$ = makeList1($1);  }
		;

/* AS is not optional because shift/red conflict with unary ops */
target_el:  a_expr AS ColLabel
				{
					$$ = makeNode(ResTarget);
					$$->name = $3;
					$$->indirection = NULL;
					$$->val = (Node *)$1;
				}
		| a_expr
				{
					$$ = makeNode(ResTarget);
					$$->name = NULL;
					$$->indirection = NULL;
					$$->val = (Node *)$1;
				}
		| relation_name '.' '*'
				{
					Attr *att = makeNode(Attr);
					att->relname = $1;
					att->paramNo = NULL;
					att->attrs = makeList1(makeString("*"));
					att->indirection = NIL;
					$$ = makeNode(ResTarget);
					$$->name = NULL;
					$$->indirection = NULL;
					$$->val = (Node *)att;
				}
		| '*'
				{
					Attr *att = makeNode(Attr);
					att->relname = "*";
					att->paramNo = NULL;
					att->attrs = NULL;
					att->indirection = NIL;
					$$ = makeNode(ResTarget);
					$$->name = NULL;
					$$->indirection = NULL;
					$$->val = (Node *)att;
				}
		;

/* Target list as found in UPDATE table SET ... */

update_target_list:  update_target_list ',' update_target_el
				{	$$ = lappend($1,$3);  }
		| update_target_el
				{	$$ = makeList1($1);  }
		;

update_target_el:  ColId opt_indirection '=' a_expr
				{
					$$ = makeNode(ResTarget);
					$$->name = $1;
					$$->indirection = $2;
					$$->val = (Node *)$4;
				}
		;

/*****************************************************************************
 *
 *	Names and constants
 *
 *****************************************************************************/

relation_name:	SpecialRuleRelation
				{
					$$ = $1;
					StrNCpy(saved_relname, $1, NAMEDATALEN);
				}
		| ColId
				{
					/* disallow refs to variable system tables */
					if (strcmp(LogRelationName, $1) == 0
					   || strcmp(VariableRelationName, $1) == 0)
						elog(ERROR,"%s cannot be accessed by users",$1);
					else
						$$ = $1;
					StrNCpy(saved_relname, $1, NAMEDATALEN);
				}
		;

database_name:			ColId			{ $$ = $1; };
access_method:			ColId			{ $$ = $1; };
attr_name:				ColId			{ $$ = $1; };
class:					ColId			{ $$ = $1; };
index_name:				ColId			{ $$ = $1; };

/* Functions
 * Include date/time keywords as SQL92 extension.
 * Include TYPE as a SQL92 unreserved keyword. - thomas 1997-10-05
 */
name:					ColId			{ $$ = $1; };
func_name:				ColId			{ $$ = xlateSqlFunc($1); };

file_name:				Sconst			{ $$ = $1; };

/* Constants
 * Include TRUE/FALSE for SQL3 support. - thomas 1997-10-24
 */
AexprConst:  Iconst
				{
					A_Const *n = makeNode(A_Const);
					n->val.type = T_Integer;
					n->val.val.ival = $1;
					$$ = (Node *)n;
				}
		| FCONST
				{
					A_Const *n = makeNode(A_Const);
					n->val.type = T_Float;
					n->val.val.str = $1;
					$$ = (Node *)n;
				}
		| Sconst
				{
					A_Const *n = makeNode(A_Const);
					n->val.type = T_String;
					n->val.val.str = $1;
					$$ = (Node *)n;
				}
		/* The SimpleTypename rule formerly used Typename,
		 * but that causes reduce conflicts with subscripted column names.
		 * Now, separate into ConstTypename and ConstInterval,
		 * to allow implementing the SQL92 syntax for INTERVAL literals.
		 * - thomas 2000-06-24
		 */
		| ConstTypename Sconst
				{
					A_Const *n = makeNode(A_Const);
					n->typename = $1;
					n->val.type = T_String;
					n->val.val.str = $2;
					$$ = (Node *)n;
				}
		| ConstInterval Sconst opt_interval
				{
					A_Const *n = makeNode(A_Const);
					n->typename = $1;
					n->val.type = T_String;
					n->val.val.str = $2;
					$$ = (Node *)n;
				}
		| ParamNo
				{	$$ = (Node *)$1;  }
		| TRUE_P
				{
					A_Const *n = makeNode(A_Const);
					n->val.type = T_String;
					n->val.val.str = "t";
					n->typename = makeNode(TypeName);
					n->typename->name = xlateSqlType("bool");
					n->typename->typmod = -1;
					$$ = (Node *)n;
				}
		| FALSE_P
				{
					A_Const *n = makeNode(A_Const);
					n->val.type = T_String;
					n->val.val.str = "f";
					n->typename = makeNode(TypeName);
					n->typename->name = xlateSqlType("bool");
					n->typename->typmod = -1;
					$$ = (Node *)n;
				}
		| NULL_P
				{
					A_Const *n = makeNode(A_Const);
					n->val.type = T_Null;
					$$ = (Node *)n;
				}
		;

ParamNo:  PARAM opt_indirection
				{
					$$ = makeNode(ParamNo);
					$$->number = $1;
					$$->indirection = $2;
				}
		;

Iconst:  ICONST							{ $$ = $1; };
Sconst:  SCONST							{ $$ = $1; };
UserId:  ColId							{ $$ = $1; };

/* Column identifier
 * Include date/time keywords as SQL92 extension.
 * Include TYPE as a SQL92 unreserved keyword. - thomas 1997-10-05
 * Add other keywords. Note that as the syntax expands,
 *  some of these keywords will have to be removed from this
 *  list due to shift/reduce conflicts in yacc. If so, move
 *  down to the ColLabel entity. - thomas 1997-11-06
 * Any tokens which show up as operators will screw up the parsing if
 * allowed as identifiers, but are acceptable as ColLabels:
 *  BETWEEN, IN, IS, ISNULL, NOTNULL, OVERLAPS
 * Thanks to Tom Lane for pointing this out. - thomas 2000-03-29
 * Allow LIKE and ILIKE as TokenId (and ColId) to make sure that they
 *  are allowed in the func_name production. Otherwise, we can't define
 *  more like() and ilike() functions for new data types.
 * - thomas 2000-08-07
 */
ColId:  generic							{ $$ = $1; }
		| datetime						{ $$ = $1; }
		| TokenId						{ $$ = $1; }
		| INTERVAL						{ $$ = "interval"; }
		| NATIONAL						{ $$ = "national"; }
		| PATH_P						{ $$ = "path"; }
		| SERIAL						{ $$ = "serial"; }
		| TIME							{ $$ = "time"; }
		| TIMESTAMP						{ $$ = "timestamp"; }
		;

/* Parser tokens to be used as identifiers.
 * Tokens involving data types should appear in ColId only,
 * since they will conflict with real TypeName productions.
 */
TokenId:  ABSOLUTE						{ $$ = "absolute"; }
		| ACCESS						{ $$ = "access"; }
		| ACTION						{ $$ = "action"; }
		| ADD							{ $$ = "add"; }
		| AFTER							{ $$ = "after"; }
		| AGGREGATE						{ $$ = "aggregate"; }
		| ALTER							{ $$ = "alter"; }
		| BACKWARD						{ $$ = "backward"; }
		| BEFORE						{ $$ = "before"; }
		| BEGIN_TRANS					{ $$ = "begin"; }
		| BY							{ $$ = "by"; }
		| CACHE							{ $$ = "cache"; }
		| CASCADE						{ $$ = "cascade"; }
		| CHAIN							{ $$ = "chain"; }
		| CLOSE							{ $$ = "close"; }
		| COMMENT						{ $$ = "comment"; }
		| COMMIT						{ $$ = "commit"; }
		| COMMITTED						{ $$ = "committed"; }
		| CONSTRAINTS					{ $$ = "constraints"; }
		| CREATE						{ $$ = "create"; }
		| CREATEDB						{ $$ = "createdb"; }
		| CREATEUSER					{ $$ = "createuser"; }
		| CURSOR						{ $$ = "cursor"; }
		| CYCLE							{ $$ = "cycle"; }
		| DATABASE						{ $$ = "database"; }
		| DECLARE						{ $$ = "declare"; }
		| DEFERRED						{ $$ = "deferred"; }
		| DELETE						{ $$ = "delete"; }
		| DELIMITERS					{ $$ = "delimiters"; }
		| DOUBLE						{ $$ = "double"; }
		| DROP							{ $$ = "drop"; }
		| EACH							{ $$ = "each"; }
		| ENCODING						{ $$ = "encoding"; }
		| ESCAPE						{ $$ = "escape"; }
		| EXCLUSIVE						{ $$ = "exclusive"; }
		| EXECUTE						{ $$ = "execute"; }
		| FETCH							{ $$ = "fetch"; }
		| FORCE							{ $$ = "force"; }
		| FORWARD						{ $$ = "forward"; }
		| FUNCTION						{ $$ = "function"; }
		| GRANT							{ $$ = "grant"; }
		| HANDLER						{ $$ = "handler"; }
		| ILIKE							{ $$ = "ilike"; }
		| IMMEDIATE						{ $$ = "immediate"; }
		| INCREMENT						{ $$ = "increment"; }
		| INDEX							{ $$ = "index"; }
		| INHERITS						{ $$ = "inherits"; }
		| INSENSITIVE					{ $$ = "insensitive"; }
		| INSERT						{ $$ = "insert"; }
		| INSTEAD						{ $$ = "instead"; }
		| ISOLATION						{ $$ = "isolation"; }
		| KEY							{ $$ = "key"; }
		| LANGUAGE						{ $$ = "language"; }
		| LANCOMPILER					{ $$ = "lancompiler"; }
		| LEVEL							{ $$ = "level"; }
		| LIKE							{ $$ = "like"; }
		| LOCATION						{ $$ = "location"; }
		| MATCH							{ $$ = "match"; }
		| MAXVALUE						{ $$ = "maxvalue"; }
		| MINVALUE						{ $$ = "minvalue"; }
		| MODE							{ $$ = "mode"; }
		| NAMES							{ $$ = "names"; }
		| NEXT							{ $$ = "next"; }
		| NO							{ $$ = "no"; }
		| NOCREATEDB					{ $$ = "nocreatedb"; }
		| NOCREATEUSER					{ $$ = "nocreateuser"; }
		| NOTHING						{ $$ = "nothing"; }
		| NOTIFY						{ $$ = "notify"; }
		| OF							{ $$ = "of"; }
		| OIDS							{ $$ = "oids"; }
		| OPERATOR						{ $$ = "operator"; }
		| OPTION						{ $$ = "option"; }
		| OWNER							{ $$ = "owner"; }
		| PARTIAL						{ $$ = "partial"; }
		| PASSWORD						{ $$ = "password"; }
		| PENDANT						{ $$ = "pendant"; }
		| PRIOR							{ $$ = "prior"; }
		| PRIVILEGES					{ $$ = "privileges"; }
		| PROCEDURAL					{ $$ = "procedural"; }
		| READ							{ $$ = "read"; }
		| REINDEX						{ $$ = "reindex"; }
		| RELATIVE						{ $$ = "relative"; }
		| RENAME						{ $$ = "rename"; }
		| RESTRICT						{ $$ = "restrict"; }
		| RETURNS						{ $$ = "returns"; }
		| REVOKE						{ $$ = "revoke"; }
		| ROLLBACK						{ $$ = "rollback"; }
		| ROW							{ $$ = "row"; }
		| RULE							{ $$ = "rule"; }
		| SCHEMA						{ $$ = "schema"; }
		| SCROLL						{ $$ = "scroll"; }
		| SESSION						{ $$ = "session"; }
		| SEQUENCE						{ $$ = "sequence"; }
		| SERIALIZABLE					{ $$ = "serializable"; }
		| SET							{ $$ = "set"; }
		| SHARE							{ $$ = "share"; }
		| START							{ $$ = "start"; }
		| STATEMENT						{ $$ = "statement"; }
		| STDIN							{ $$ = "stdin"; }
		| STDOUT						{ $$ = "stdout"; }
		| SYSID							{ $$ = "sysid"; }
		| TEMP							{ $$ = "temp"; }
		| TEMPORARY						{ $$ = "temporary"; }
		| TIMEZONE_HOUR					{ $$ = "timezone_hour"; }
		| TIMEZONE_MINUTE				{ $$ = "timezone_minute"; }
		| TOAST							{ $$ = "toast"; }
		| TRIGGER						{ $$ = "trigger"; }
		| TRUNCATE						{ $$ = "truncate"; }
		| TRUSTED						{ $$ = "trusted"; }
		| UNDER 						{ $$ = "under"; }
		| UNLISTEN						{ $$ = "unlisten"; }
		| UNTIL							{ $$ = "until"; }
		| UPDATE						{ $$ = "update"; }
		| VALID							{ $$ = "valid"; }
		| VALUES						{ $$ = "values"; }
		| VARYING						{ $$ = "varying"; }
		| VERSION						{ $$ = "version"; }
		| VIEW							{ $$ = "view"; }
		| WITH							{ $$ = "with"; }
		| WITHOUT						{ $$ = "without"; }
		| WORK							{ $$ = "work"; }
		| ZONE							{ $$ = "zone"; }
		;

/* Column label
 * Allowed labels in "AS" clauses.
 * Include TRUE/FALSE SQL3 reserved words for Postgres backward
 *  compatibility. Cannot allow this for column names since the
 *  syntax would not distinguish between the constant value and
 *  a column name. - thomas 1997-10-24
 * Add other keywords to this list. Note that they appear here
 *  rather than in ColId if there was a shift/reduce conflict
 *  when used as a full identifier. - thomas 1997-11-06
 */
ColLabel:  ColId						{ $$ = $1; }
		| ABORT_TRANS					{ $$ = "abort"; }
		| ALL							{ $$ = "all"; }
		| ANALYZE						{ $$ = "analyze"; }
		| ANY							{ $$ = "any"; }
		| ASC							{ $$ = "asc"; }
		| BETWEEN						{ $$ = "between"; }
		| BINARY						{ $$ = "binary"; }
		| BIT							{ $$ = "bit"; }
		| BOTH							{ $$ = "both"; }
		| CASE							{ $$ = "case"; }
		| CAST							{ $$ = "cast"; }
		| CHAR							{ $$ = "char"; }
		| CHARACTER						{ $$ = "character"; }
		| CHECK							{ $$ = "check"; }
		| CLUSTER						{ $$ = "cluster"; }
		| COALESCE						{ $$ = "coalesce"; }
		| COLLATE						{ $$ = "collate"; }
		| COLUMN						{ $$ = "column"; }
		| CONSTRAINT					{ $$ = "constraint"; }
		| COPY							{ $$ = "copy"; }
		| CROSS							{ $$ = "cross"; }
		| CURRENT_DATE					{ $$ = "current_date"; }
		| CURRENT_TIME					{ $$ = "current_time"; }
		| CURRENT_TIMESTAMP				{ $$ = "current_timestamp"; }
		| CURRENT_USER					{ $$ = "current_user"; }
		| DEC							{ $$ = "dec"; }
		| DECIMAL						{ $$ = "decimal"; }
		| DEFAULT						{ $$ = "default"; }
		| DEFERRABLE					{ $$ = "deferrable"; }
		| DESC							{ $$ = "desc"; }
		| DISTINCT						{ $$ = "distinct"; }
		| DO							{ $$ = "do"; }
		| ELSE							{ $$ = "else"; }
		| END_TRANS						{ $$ = "end"; }
		| EXCEPT						{ $$ = "except"; }
		| EXISTS						{ $$ = "exists"; }
		| EXPLAIN						{ $$ = "explain"; }
		| EXTEND						{ $$ = "extend"; }
		| EXTRACT						{ $$ = "extract"; }
		| FALSE_P						{ $$ = "false"; }
		| FLOAT							{ $$ = "float"; }
		| FOR							{ $$ = "for"; }
		| FOREIGN						{ $$ = "foreign"; }
		| FROM							{ $$ = "from"; }
		| FULL							{ $$ = "full"; }
		| GLOBAL						{ $$ = "global"; }
		| GROUP							{ $$ = "group"; }
		| HAVING						{ $$ = "having"; }
		| INITIALLY						{ $$ = "initially"; }
		| IN							{ $$ = "in"; }
		| INNER_P						{ $$ = "inner"; }
		| INTERSECT						{ $$ = "intersect"; }
		| INTO							{ $$ = "into"; }
		| INOUT							{ $$ = "inout"; }
		| IS							{ $$ = "is"; }
		| ISNULL						{ $$ = "isnull"; }
		| JOIN							{ $$ = "join"; }
		| LEADING						{ $$ = "leading"; }
		| LEFT							{ $$ = "left"; }
		| LISTEN						{ $$ = "listen"; }
		| LOAD							{ $$ = "load"; }
		| LOCAL							{ $$ = "local"; }
		| LOCK_P						{ $$ = "lock"; }
		| MOVE							{ $$ = "move"; }
		| NATURAL						{ $$ = "natural"; }
		| NCHAR							{ $$ = "nchar"; }
		| NEW							{ $$ = "new"; }
		| NONE							{ $$ = "none"; }
		| NOT							{ $$ = "not"; }
		| NOTNULL						{ $$ = "notnull"; }
		| NULLIF						{ $$ = "nullif"; }
		| NULL_P						{ $$ = "null"; }
		| NUMERIC						{ $$ = "numeric"; }
		| OFF							{ $$ = "off"; }
		| OFFSET						{ $$ = "offset"; }
		| OLD							{ $$ = "old"; }
		| ON							{ $$ = "on"; }
		| ONLY							{ $$ = "only"; }
		| OR							{ $$ = "or"; }
		| ORDER							{ $$ = "order"; }
		| OUT							{ $$ = "out"; }
		| OUTER_P						{ $$ = "outer"; }
		| OVERLAPS						{ $$ = "overlaps"; }
		| POSITION						{ $$ = "position"; }
		| PRECISION						{ $$ = "precision"; }
		| PRIMARY						{ $$ = "primary"; }
		| PROCEDURE						{ $$ = "procedure"; }
		| PUBLIC						{ $$ = "public"; }
		| REFERENCES					{ $$ = "references"; }
		| RESET							{ $$ = "reset"; }
		| RIGHT							{ $$ = "right"; }
		| SELECT						{ $$ = "select"; }
		| SESSION_USER					{ $$ = "session_user"; }
		| SETOF							{ $$ = "setof"; }
		| SHOW							{ $$ = "show"; }
		| SOME							{ $$ = "some"; }
		| SUBSTRING						{ $$ = "substring"; }
		| TABLE							{ $$ = "table"; }
		| THEN							{ $$ = "then"; }
		| TO							{ $$ = "to"; }
		| TRANSACTION					{ $$ = "transaction"; }
		| TRIM							{ $$ = "trim"; }
		| TRUE_P						{ $$ = "true"; }
		| UNION							{ $$ = "union"; }
		| UNIQUE						{ $$ = "unique"; }
		| USER							{ $$ = "user"; }
		| USING							{ $$ = "using"; }
		| VACUUM						{ $$ = "vacuum"; }
		| VARCHAR						{ $$ = "varchar"; }
		| VERBOSE						{ $$ = "verbose"; }
		| WHEN							{ $$ = "when"; }
		| WHERE							{ $$ = "where"; }
		;

SpecialRuleRelation:  OLD
				{
					if (QueryIsRule)
						$$ = "*OLD*";
					else
						elog(ERROR,"OLD used in non-rule query");
				}
		| NEW
				{
					if (QueryIsRule)
						$$ = "*NEW*";
					else
						elog(ERROR,"NEW used in non-rule query");
				}
		;

%%

static Node *
makeA_Expr(int oper, char *opname, Node *lexpr, Node *rexpr)
{
	A_Expr *a = makeNode(A_Expr);
	a->oper = oper;
	a->opname = opname;
	a->lexpr = lexpr;
	a->rexpr = rexpr;
	return (Node *)a;
}

static Node *
makeTypeCast(Node *arg, TypeName *typename)
{
	/*
	 * If arg is an A_Const or ParamNo, just stick the typename into the
	 * field reserved for it --- unless there's something there already!
	 * (We don't want to collapse x::type1::type2 into just x::type2.)
	 * Otherwise, generate a TypeCast node.
	 */
	if (IsA(arg, A_Const) &&
		((A_Const *) arg)->typename == NULL)
	{
		((A_Const *) arg)->typename = typename;
		return arg;
	}
	else if (IsA(arg, ParamNo) &&
			 ((ParamNo *) arg)->typename == NULL)
	{
		((ParamNo *) arg)->typename = typename;
		return arg;
	}
	else
	{
		TypeCast *n = makeNode(TypeCast);
		n->arg = arg;
		n->typename = typename;
		return (Node *) n;
	}
}

/* makeRowExpr()
 * Generate separate operator nodes for a single row descriptor expression.
 * Perhaps this should go deeper in the parser someday...
 * - thomas 1997-12-22
 */
static Node *
makeRowExpr(char *opr, List *largs, List *rargs)
{
	Node *expr = NULL;
	Node *larg, *rarg;

	if (length(largs) != length(rargs))
		elog(ERROR,"Unequal number of entries in row expression");

	if (lnext(largs) != NIL)
		expr = makeRowExpr(opr,lnext(largs),lnext(rargs));

	larg = lfirst(largs);
	rarg = lfirst(rargs);

	if ((strcmp(opr, "=") == 0)
	 || (strcmp(opr, "<") == 0)
	 || (strcmp(opr, "<=") == 0)
	 || (strcmp(opr, ">") == 0)
	 || (strcmp(opr, ">=") == 0))
	{
		if (expr == NULL)
			expr = makeA_Expr(OP, opr, larg, rarg);
		else
			expr = makeA_Expr(AND, NULL, expr, makeA_Expr(OP, opr, larg, rarg));
	}
	else if (strcmp(opr, "<>") == 0)
	{
		if (expr == NULL)
			expr = makeA_Expr(OP, opr, larg, rarg);
		else
			expr = makeA_Expr(OR, NULL, expr, makeA_Expr(OP, opr, larg, rarg));
	}
	else
	{
		elog(ERROR,"Operator '%s' not implemented for row expressions",opr);
	}

	return expr;
}

static void
mapTargetColumns(List *src, List *dst)
{
	ColumnDef *s;
	ResTarget *d;

	if (length(src) != length(dst))
		elog(ERROR,"CREATE TABLE/AS SELECT has mismatched column count");

	while ((src != NIL) && (dst != NIL))
	{
		s = (ColumnDef *)lfirst(src);
		d = (ResTarget *)lfirst(dst);

		d->name = s->colname;

		src = lnext(src);
		dst = lnext(dst);
	}
	return;
} /* mapTargetColumns() */


/* findLeftmostSelect()
 *		Find the leftmost SelectStmt in a SetOperationStmt parsetree.
 */
static SelectStmt *
findLeftmostSelect(Node *node)
{
	while (node && IsA(node, SetOperationStmt))
		node = ((SetOperationStmt *) node)->larg;
	Assert(node && IsA(node, SelectStmt));
	return (SelectStmt *) node;
}


/* xlateSqlFunc()
 * Convert alternate function names to internal Postgres functions.
 *
 * Do not convert "float", since that is handled elsewhere
 *  for FLOAT(p) syntax.
 *
 * Converting "datetime" to "timestamp" and "timespan" to "interval"
 * is a temporary expedient for pre-7.0 to 7.0 compatibility;
 * these should go away for v7.1.
 */
static char *
xlateSqlFunc(char *name)
{
	if (strcmp(name,"character_length") == 0)
		return "char_length";
	else if (strcmp(name,"datetime") == 0)
		return "timestamp";
	else if (strcmp(name,"timespan") == 0)
		return "interval";
	else
		return name;
} /* xlateSqlFunc() */

/* xlateSqlType()
 * Convert alternate type names to internal Postgres types.
 *
 * NB: do NOT put "char" -> "bpchar" here, because that renders it impossible
 * to refer to our single-byte char type, even with quotes.  (Without quotes,
 * CHAR is a keyword, and the code above produces "bpchar" for it.)
 *
 * Convert "datetime" and "timespan" to allow a transition to SQL92 type names.
 * Remove this translation for v7.1 - thomas 2000-03-25
 *
 * Convert "lztext" to "text" to allow forward compatibility for anyone using
 * the undocumented "lztext" type in 7.0.  This can go away in 7.2 or later
 * - tgl 2000-07-30
 */
static char *
xlateSqlType(char *name)
{
	if ((strcmp(name,"int") == 0)
		|| (strcmp(name,"integer") == 0))
		return "int4";
	else if (strcmp(name, "smallint") == 0)
		return "int2";
	else if (strcmp(name, "bigint") == 0)
		return "int8";
	else if (strcmp(name, "real") == 0)
		return "float4";
	else if (strcmp(name, "float") == 0)
		return "float8";
	else if (strcmp(name, "decimal") == 0)
		return "numeric";
	else if (strcmp(name, "datetime") == 0)
		return "timestamp";
	else if (strcmp(name, "timespan") == 0)
		return "interval";
	else if (strcmp(name, "lztext") == 0)
		return "text";
	else if (strcmp(name, "boolean") == 0)
		return "bool";
	else
		return name;
} /* xlateSqlType() */


void parser_init(Oid *typev, int nargs)
{
	saved_relname[0] = '\0';
	QueryIsRule = FALSE;
	/*
	 * Keep enough information around to fill out the type of param nodes
	 * used in postquel functions
	 */
	param_type_info = typev;
	pfunc_num_args = nargs;
}

Oid param_type(int t)
{
	if ((t > pfunc_num_args) || (t <= 0))
		return InvalidOid;
	return param_type_info[t - 1];
}

/*
 * Test whether an a_expr is a plain NULL constant or not.
 */
static bool
exprIsNullConstant(Node *arg)
{
	if (arg && IsA(arg, A_Const))
	{
		A_Const *con = (A_Const *) arg;

		if (con->val.type == T_Null &&
			con->typename == NULL)
			return TRUE;
	}
	return FALSE;
}

/*
 * doNegate --- handle negation of a numeric constant.
 *
 * Formerly, we did this here because the optimizer couldn't cope with
 * indexquals that looked like "var = -4" --- it wants "var = const"
 * and a unary minus operator applied to a constant didn't qualify.
 * As of Postgres 7.0, that problem doesn't exist anymore because there
 * is a constant-subexpression simplifier in the optimizer.  However,
 * there's still a good reason for doing this here, which is that we can
 * postpone committing to a particular internal representation for simple
 * negative constants.  It's better to leave "-123.456" in string form
 * until we know what the desired type is.
 */
static Node *
doNegate(Node *n)
{
	if (IsA(n, A_Const))
	{
		A_Const *con = (A_Const *)n;

		if (con->val.type == T_Integer)
		{
			con->val.val.ival = -con->val.val.ival;
			return n;
		}
		if (con->val.type == T_Float)
		{
			doNegateFloat(&con->val);
			return n;
		}
	}

	return makeA_Expr(OP, "-", NULL, n);
}

static void
doNegateFloat(Value *v)
{
	char   *oldval = v->val.str;

	Assert(IsA(v, Float));
	if (*oldval == '+')
		oldval++;
	if (*oldval == '-')
		v->val.str = oldval+1;	/* just strip the '-' */
	else
	{
		char   *newval = (char *) palloc(strlen(oldval) + 2);

		*newval = '-';
		strcpy(newval+1, oldval);
		v->val.str = newval;
	}
}
