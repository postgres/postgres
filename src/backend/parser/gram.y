%{ /* -*-text-*- */

/*#define YYDEBUG 1*/
/*-------------------------------------------------------------------------
 * 
 * gram.y--
 *    POSTGRES SQL YACC rules/actions
 * 
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/parser/gram.y,v 1.37 1997/08/20 01:12:02 vadim Exp $
 *
 * HISTORY
 *    AUTHOR		DATE		MAJOR EVENT
 *    Andrew Yu		Sept, 1994	POSTQUEL to SQL conversion
 *    Andrew Yu		Oct, 1994	lispy code conversion
 *
 * NOTES
 *    CAPITALS are used to represent terminal symbols.
 *    non-capitals are used to represent non-terminals.
 *
 *    if you use list, make sure the datum is a node so that the printing
 *    routines work
 *
 * WARNING
 *    sometimes we assign constants to makeStrings. Make sure we don't free
 *    those.
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include <ctype.h>

#include "postgres.h"
#include "nodes/parsenodes.h"
#include "parser/gramparse.h"
#include "parser/catalog_utils.h"
#include "parser/parse_query.h"
#include "storage/smgr.h"
#include "utils/acl.h"
#include "catalog/catname.h"
#include "utils/elog.h"
#include "access/xact.h"

static char saved_relname[NAMEDATALEN];  /* need this for complex attributes */
static bool QueryIsRule = FALSE;
static Node *saved_In_Expr;
extern List *parsetree;

extern int CurScanPosition(void);
extern int DefaultStartPosition;
extern int CheckStartPosition;
extern char *parseString;

/*
 * If you need access to certain yacc-generated variables and find that 
 * they're static by default, uncomment the next line.  (this is not a
 * problem, yet.)
 */
/*#define __YYSCLASS*/

static char *xlateSqlType(char *);
static Node *makeA_Expr(int oper, char *opname, Node *lexpr, Node *rexpr);

/* old versions of flex define this as a macro */
#if defined(yywrap)
#undef yywrap
#endif /* yywrap */
%}


%union {
    double 		dval;
    int			ival;
    char                chr;
    char		*str;
    bool		boolean;
    List		*list;
    Node		*node;
    Value		*value;

    Attr		*attr;

    ColumnDef		*coldef;
    TypeName		*typnam;
    DefElem		*defelt;
    ParamString		*param;
    SortGroupBy		*sortgroupby;
    IndexElem		*ielem;
    RangeVar		*range;
    RelExpr		*relexp;
    TimeRange		*trange;
    A_Indices		*aind;
    ResTarget		*target;
    ParamNo		*paramno;
	
    VersionStmt		*vstmt;
    DefineStmt		*dstmt;
    PurgeStmt		*pstmt;
    RuleStmt		*rstmt;
    AppendStmt		*astmt;
}

%type <node>	stmt, 
	AddAttrStmt, ClosePortalStmt,
	CopyStmt, CreateStmt, CreateSeqStmt, DefineStmt, DestroyStmt, 
	ExtendStmt, FetchStmt,	GrantStmt,
	IndexStmt, MoveStmt, ListenStmt, OptimizableStmt, 
        ProcedureStmt, PurgeStmt,
	RecipeStmt, RemoveAggrStmt, RemoveOperStmt, RemoveFuncStmt, RemoveStmt,
	RenameStmt, RevokeStmt, RuleStmt, TransactionStmt, ViewStmt, LoadStmt,
	CreatedbStmt, DestroydbStmt, VacuumStmt, RetrieveStmt, CursorStmt,
	ReplaceStmt, AppendStmt, NotifyStmt, DeleteStmt, ClusterStmt,
	ExplainStmt, VariableSetStmt, VariableShowStmt, VariableResetStmt

%type <str>	relation_name, copy_file_name, copy_delimiter, def_name,
	database_name, access_method_clause, access_method, attr_name,
	class, index_name, name, file_name, recipe_name,
	var_name, aggr_argtype, OptDefault, CheckElem

%type <str>	opt_id, opt_portal_name,
	before_clause, after_clause, all_Op, MathOp, opt_name, opt_unique,
	result, OptUseOp, opt_class, opt_range_start, opt_range_end,
	SpecialRuleRelation

%type <str>	privileges, operation_commalist, grantee
%type <chr>	operation

%type <list>	stmtblock, stmtmulti,
	relation_name_list, OptTableElementList, tableElementList, 
	OptInherit, OptCheck, CheckList, definition,
	opt_with, def_args, def_name_list, func_argtypes,
	oper_argtypes, OptStmtList, OptStmtBlock, OptStmtMulti,
	opt_column_list, columnList, opt_va_list, va_list,
	sort_clause, sortby_list, index_params, index_list, name_list, 
	from_clause, from_list, opt_array_bounds, nest_array_bounds,
	expr_list, default_expr_list, attrs, res_target_list, res_target_list2,
	def_list, opt_indirection, group_clause, groupby_list

%type <boolean>	opt_inh_star, opt_binary, opt_instead, opt_with_copy,
		index_opt_unique, opt_verbose, opt_analyze, opt_null

%type <ival>	copy_dirn, archive_type, OptArchiveType, OptArchiveLocation, 
	def_type, opt_direction, remove_type, opt_column, event

%type <ival>	OptLocation, opt_move_where, fetch_how_many 

%type <list>	OptSeqList
%type <defelt>	OptSeqElem

%type <dstmt>	def_rest
%type <pstmt>	purge_quals
%type <astmt>	insert_rest

%type <typnam>	Typename, typname, opt_type
%type <coldef>	columnDef
%type <defelt>	def_elem
%type <node>	def_arg, columnElem, where_clause, 
		a_expr, a_expr_or_null, AexprConst,
		default_expr, default_expr_or_null,
		in_expr_nodes, not_in_expr_nodes,
		having_clause, default_expr
%type <value>	NumConst
%type <attr>	event_object, attr
%type <sortgroupby>	groupby
%type <sortgroupby>	sortby
%type <ielem>	index_elem, func_index
%type <range>	from_val
%type <relexp>	relation_expr
%type <trange>	time_range
%type <target>	res_target_el, res_target_el2
%type <paramno>	ParamNo

%type <ival>	Iconst
%type <str>	Sconst
%type <str>	Id, date, var_value


/*
 * If you make any token changes, remember to:
 *	- use "yacc -d" and update parse.h
 * 	- update the keyword table in parser/keywords.c
 */

/* Keywords */
%token	ABORT_TRANS, ACL, ADD, AFTER, AGGREGATE, ALL, ALTER, ANALYZE, 
	AND, APPEND, ARCHIVE, ARCH_STORE, AS, ASC, 
	BACKWARD, BEFORE, BEGIN_TRANS, BETWEEN, BINARY, BY, 
	CAST, CHANGE, CHECK, CLOSE, CLUSTER, COLUMN, COMMIT, COPY, CREATE,
	CURRENT, CURSOR, DATABASE, DECLARE, DEFAULT, DELETE, 
	DELIMITERS, DESC, DISTINCT, DO, DROP, END_TRANS,
	EXTEND, FETCH, FOR, FORWARD, FROM, FUNCTION, GRANT, GROUP, 
	HAVING, HEAVY, IN, INDEX, INHERITS, INSERT, INSTEAD, INTO, IS,
	ISNULL, LANGUAGE, LIGHT, LISTEN, LOAD, MERGE, MOVE, NEW, 
	NONE, NOT, NOTHING, NOTIFY, NOTNULL, 
        OIDS, ON, OPERATOR, OPTION, OR, ORDER, 
        PNULL, PRIVILEGES, PUBLIC, PURGE, P_TYPE, 
        RENAME, REPLACE, RESET, RETRIEVE, RETURNS, REVOKE, ROLLBACK, RULE, 
        SELECT, SET, SETOF, SHOW, STDIN, STDOUT, STORE, 
	TABLE, TO, TRANSACTION, UNIQUE, UPDATE, USING, VACUUM, VALUES
	VERBOSE, VERSION, VIEW, WHERE, WITH, WORK
%token	EXECUTE, RECIPE, EXPLAIN, LIKE, SEQUENCE

/* Special keywords, not in the query language - see the "lex" file */
%token <str>	IDENT, SCONST, Op 
%token <ival>	ICONST, PARAM
%token <dval>	FCONST

/* these are not real. they are here so that they gets generated as #define's*/
%token		OP

/* precedence */
%left	OR
%left	AND
%right	NOT
%right 	'='
%nonassoc LIKE
%nonassoc BETWEEN
%nonassoc IN
%nonassoc Op
%nonassoc NOTNULL
%nonassoc ISNULL
%nonassoc IS
%left  	'+' '-'
%left  	'*' '/'
%left	'|'		/* this is the relation union op, not logical or */
%right  ':'		/* Unary Operators      */
%left	';'		/* end of statement or natural log    */
%nonassoc  '<' '>'
%right   UMINUS
%left	'.'
%left  	'[' ']' 
%nonassoc TYPECAST
%nonassoc REDUCE
%%

stmtblock: stmtmulti
		{ parsetree = $1; }
	|  stmt
		{ parsetree = lcons($1,NIL); }
	;

stmtmulti: stmtmulti stmt ';'
		{ $$ = lappend($1, $2); }
	|  stmtmulti stmt
		{ $$ = lappend($1, $2); }
	|  stmt ';'
		{ $$ = lcons($1,NIL); }
	;

stmt :	  AddAttrStmt
	| ClosePortalStmt
	| CopyStmt
	| CreateStmt
	| CreateSeqStmt
	| ClusterStmt
	| DefineStmt
	| DestroyStmt
	| ExtendStmt
	| ExplainStmt
	| FetchStmt
        | GrantStmt
	| IndexStmt
	| MoveStmt
        | ListenStmt
	| ProcedureStmt
	| PurgeStmt			
	| RecipeStmt
	| RemoveAggrStmt
	| RemoveOperStmt
	| RemoveFuncStmt
	| RemoveStmt
	| RenameStmt
        | RevokeStmt
	| OptimizableStmt	
	| RuleStmt			
	| TransactionStmt
  	| ViewStmt
	| LoadStmt
	| CreatedbStmt
	| DestroydbStmt
	| VacuumStmt
	| VariableSetStmt
	| VariableShowStmt
	| VariableResetStmt
	;

/*****************************************************************************
 * 
 * Set PG internal variable
 *    SET var_name TO 'var_value'
 * 
 *****************************************************************************/

VariableSetStmt: SET var_name TO var_value
		{
		    VariableSetStmt *n = makeNode(VariableSetStmt);
		    n->name  = $2;
		    n->value = $4;
		    $$ = (Node *) n;
		}
	;

var_value:	Sconst		{ $$ = $1; }
	;

VariableShowStmt: SHOW var_name
		{
		    VariableShowStmt *n = makeNode(VariableShowStmt);
		    n->name  = $2;
		    $$ = (Node *) n;
		}
	;

VariableResetStmt: RESET var_name
		{
		    VariableResetStmt *n = makeNode(VariableResetStmt);
		    n->name  = $2;
		    $$ = (Node *) n;
		}
	;

/*****************************************************************************
 *
 *	QUERY :
 *		addattr ( attr1 = type1 .. attrn = typen ) to <relname> [*]
 *
 *****************************************************************************/

AddAttrStmt:  ALTER TABLE relation_name opt_inh_star ADD COLUMN columnDef
		{
		    AddAttrStmt *n = makeNode(AddAttrStmt);
		    n->relname = $3;
		    n->inh = $4;
		    n->colDef = $7;
		    $$ = (Node *)n;
		}
	;

columnDef:  Id Typename OptDefault opt_null
		{  
		    $$ = makeNode(ColumnDef);
		    $$->colname = $1;
		    $$->typename = $2;
		    $$->defval = $3;
		    $$->is_not_null = $4;
		}
	;

OptDefault:  DEFAULT default_expr	{ 
				    int deflen = CurScanPosition() - DefaultStartPosition;
				    char *defval;
				    
				    defval = (char*) palloc (deflen + 1);
				    memcpy (defval, 
				    		parseString + DefaultStartPosition, 
				    		deflen);
				    defval[deflen] = 0;
				    $$ = defval;
				}
	|  /*EMPTY*/		{ $$ = NULL; }
	;

default_expr_or_null: default_expr
		{ $$ = $1;}
	| Pnull
		{	
		    A_Const *n = makeNode(A_Const);
		    n->val.type = T_Null;
		    $$ = (Node *)n;
		}

default_expr:	AexprConst
		{
		    if (nodeTag($1) != T_A_Const)
		    	elog (WARN, "Cannot handle parameter in DEFAULT");
		    $$ = $1;
		}
	| '-' default_expr %prec UMINUS
		{   $$ = makeA_Expr(OP, "-", NULL, $2); }
	| default_expr '+' default_expr
		{   $$ = makeA_Expr(OP, "+", $1, $3); }
	| default_expr '-' default_expr
		{   $$ = makeA_Expr(OP, "-", $1, $3); }
	| default_expr '/' default_expr
		{   $$ = makeA_Expr(OP, "/", $1, $3); }
	| default_expr '*' default_expr
		{   $$ = makeA_Expr(OP, "*", $1, $3); }
	| default_expr '<' default_expr
		{   $$ = makeA_Expr(OP, "<", $1, $3); }
	| default_expr '>' default_expr
		{   $$ = makeA_Expr(OP, ">", $1, $3); }
	| default_expr '=' default_expr
		{   $$ = makeA_Expr(OP, "=", $1, $3); }
	| ':' default_expr
		{   $$ = makeA_Expr(OP, ":", NULL, $2); }
	| ';' default_expr
		{   $$ = makeA_Expr(OP, ";", NULL, $2); }
	| '|' default_expr
		{   $$ = makeA_Expr(OP, "|", NULL, $2); }
	| AexprConst TYPECAST Typename
		{ 
		    /* AexprConst can be either A_Const or ParamNo */
		    if (nodeTag($1) == T_A_Const) {
			((A_Const *)$1)->typename = $3;
		    }else {
		    	elog (WARN, "Cannot handle parameter in DEFAULT");
		    }
		    $$ = (Node *)$1;
		}
	| CAST AexprConst AS Typename
		{
		    /* AexprConst can be either A_Const or ParamNo */
		    if (nodeTag($2) == T_A_Const) {
			((A_Const *)$2)->typename = $4;
		    }else {
		    	elog (WARN, "Cannot handle parameter in DEFAULT");
		    }
		    $$ = (Node *)$2;
		}
	| '(' default_expr ')'
		{   $$ = $2; }
	| default_expr Op default_expr
		{   $$ = makeA_Expr(OP, $2, $1, $3); }
	| Op default_expr
		{   $$ = makeA_Expr(OP, $1, NULL, $2); }
	| default_expr Op
		{   $$ = makeA_Expr(OP, $2, $1, NULL); }
	| name '(' ')'
		{
		    FuncCall *n = makeNode(FuncCall);
		    n->funcname = $1;
		    n->args = NIL;
		    $$ = (Node *)n;
		}
	| name '(' default_expr_list ')'
		{
		    FuncCall *n = makeNode(FuncCall);
		    n->funcname = $1;
		    n->args = $3;
		    $$ = (Node *)n;
		}
	;

default_expr_list: default_expr_or_null
		{ $$ = lcons($1, NIL); }
	|  default_expr_list ',' default_expr_or_null
		{ $$ = lappend($1, $3); }
	;

opt_null:	PNULL		{ $$ = false; }
	| NOT PNULL		{ $$ = true; }
	| NOTNULL		{ $$ = true; }
	| /* EMPTY */		{ $$ = false; }
	;

	
/*****************************************************************************
 *	
 *	QUERY :
 *		close <optname>
 *	
 *****************************************************************************/

ClosePortalStmt:  CLOSE opt_id	
		{  
		    ClosePortalStmt *n = makeNode(ClosePortalStmt);
		    n->portalname = $2;
		    $$ = (Node *)n;
		}
	;


/*****************************************************************************
 *
 *	QUERY :
 *		COPY [BINARY] <relname> FROM/TO 
 *              [USING DELIMITERS <delimiter>]
 *
 *****************************************************************************/

CopyStmt:  COPY opt_binary relation_name opt_with_copy copy_dirn copy_file_name copy_delimiter
		{
		    CopyStmt *n = makeNode(CopyStmt);
		    n->binary = $2;
		    n->relname = $3;
		    n->oids = $4;
		    n->direction = $5;
		    n->filename = $6;
		    n->delimiter = $7;
		    $$ = (Node *)n;
		}
	;

copy_dirn:  TO 
		{ $$ = TO; }
	|  FROM
		{ $$ = FROM; }
	;

/* 
 * copy_file_name NULL indicates stdio is used. Whether stdin or stdout is
 * used depends on the direction. (It really doesn't make sense to copy from
 * stdout. We silently correct the "typo".	 - AY 9/94
 */
copy_file_name:  Sconst				{ $$ = $1; }
	| STDIN					{ $$ = NULL; }
	| STDOUT				{ $$ = NULL; }
	;

opt_binary: BINARY				{ $$ = TRUE; }
	|  /*EMPTY*/				{ $$ = FALSE; }
	;

opt_with_copy:  WITH OIDS			{ $$ = TRUE; }
	|  /* EMPTY */				{ $$ = FALSE; }
	;

/*
 * the default copy delimiter is tab but the user can configure it
 */
copy_delimiter: USING DELIMITERS Sconst { $$ = $3;}
	| /* EMPTY */  { $$ = "\t"; }
	;


/*****************************************************************************
 *
 *	QUERY :
 *		CREATE relname
 *
 *****************************************************************************/

CreateStmt:  CREATE TABLE relation_name '(' OptTableElementList ')'
		OptInherit OptCheck OptArchiveType OptLocation 
		OptArchiveLocation
		{
		    CreateStmt *n = makeNode(CreateStmt);
		    n->relname = $3;
		    n->tableElts = $5;
		    n->inhRelnames = $7;
		    n->check = $8;
		    n->archiveType = $9;
		    n->location = $10;
		    n->archiveLoc = $11;
		    $$ = (Node *)n;
		}
	;

OptTableElementList:  tableElementList		{ $$ = $1; }
	| /* EMPTY */				{ $$ = NULL; }
	;

tableElementList :
	  tableElementList ',' columnDef	
		{ $$ = lappend($1, $3); }
	| columnDef			
		{ $$ = lcons($1, NIL); }
	;


OptArchiveType:  ARCHIVE '=' archive_type		{ $$ = $3; }
	| /*EMPTY*/					{ $$ = ARCH_NONE; }
	;

archive_type:  HEAVY 					{ $$ = ARCH_HEAVY; }
	| LIGHT 					{ $$ = ARCH_LIGHT; }
	| NONE						{ $$ = ARCH_NONE; }
	;

OptLocation:  STORE '=' Sconst
		{  $$ = smgrin($3);  }
	| /*EMPTY*/				
		{  $$ = -1;  }
	;

OptArchiveLocation: ARCH_STORE '=' Sconst
		{  $$ = smgrin($3);  }
	| /*EMPTY*/				
		{  $$ = -1;  }
	;

OptInherit:  INHERITS '(' relation_name_list ')'	{ $$ = $3; }
	|  /*EMPTY*/					{ $$ = NIL; }
	;

OptCheck:  CheckList		{ $$ = $1; }
	| 		{ $$ = NULL; }
	;

CheckList :
	  CheckList ',' CheckElem
		{ $$ = lappend($1, $3); }
	| CheckElem
		{ $$ = lcons($1, NIL); }
	;

CheckElem:  CHECK a_expr	{ 
				    int chklen = CurScanPosition() - CheckStartPosition;
				    char *check;
				    
				    check = (char*) palloc (chklen + 1);
				    memcpy (check, 
				    		parseString + CheckStartPosition, 
				    		chklen);
				    check[chklen] = 0;
				    $$ = check;
				}
	;

/*****************************************************************************
 *
 *	QUERY :
 *		CREATE SEQUENCE seqname
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

OptSeqList:	
		OptSeqList OptSeqElem
			{ $$ = lappend($1, $2); }
	| 		{ $$ = NIL; }
	;

OptSeqElem:	IDENT NumConst
		{ 
		    $$ = makeNode(DefElem);
		    $$->defname = $1;
		    $$->arg = (Node *)$2;
		}
	|	IDENT
		{
		    $$ = makeNode(DefElem);
		    $$->defname = $1;
		    $$->arg = (Node *)NULL;
		}
	;


/*****************************************************************************
 *
 *  	QUERY :
 *		define (type,operator,aggregate)
 *
 *****************************************************************************/

DefineStmt:  CREATE def_type def_rest
		{
		    $3->defType = $2;
		    $$ = (Node *)$3;
		}
	;

def_rest:  def_name definition 
		{
		    $$ = makeNode(DefineStmt);
		    $$->defname = $1;
		    $$->definition = $2;
		}
	;

def_type:  OPERATOR 				{ $$ = OPERATOR; }
	|  Type 				{ $$ = P_TYPE; }
	|  AGGREGATE				{ $$ = AGGREGATE; }
	;

def_name:  Id 	|  MathOp |  Op
	;


definition:  '(' def_list ')'			{ $$ = $2; }
	;


def_list:  def_elem
		{ $$ = lcons($1, NIL); }
	|  def_list ',' def_elem
		{ $$ = lappend($1, $3); }
	;

def_elem:  def_name '=' def_arg
		{ 
		    $$ = makeNode(DefElem);
		    $$->defname = $1;
		    $$->arg = (Node *)$3;
		}
	|  def_name
		{
		    $$ = makeNode(DefElem);
		    $$->defname = $1;
		    $$->arg = (Node *)NULL;
		}
	;

def_arg:  Id			{  $$ = (Node *)makeString($1); }
	| all_Op		{  $$ = (Node *)makeString($1); }
	| NumConst		{  $$ = (Node *)$1; /* already a Value */ }
      	| Sconst		{  $$ = (Node *)makeString($1); }
	| SETOF Id		{ 
				   TypeName *n = makeNode(TypeName);
				   n->name = $2;
				   n->setof = TRUE;
				   n->arrayBounds = NULL;
				   $$ = (Node *)n;
				}				
	;


/*****************************************************************************
 *
 *	QUERY:	
 *		destroy <relname1> [, <relname2> .. <relnameN> ]
 *
 *****************************************************************************/

DestroyStmt:	DROP TABLE relation_name_list
		{ 
		    DestroyStmt *n = makeNode(DestroyStmt);
		    n->relNames = $3;
		    n->sequence = false;
		    $$ = (Node *)n;
		}
	|	DROP SEQUENCE relation_name_list
		{ 
		    DestroyStmt *n = makeNode(DestroyStmt);
		    n->relNames = $3;
		    n->sequence = true;
		    $$ = (Node *)n;
		}
	;


/*****************************************************************************
 *
 *	QUERY:
 *		fetch [forward | backward] [number | all ] [ in <portalname> ]
 *
 *****************************************************************************/

FetchStmt:  FETCH opt_direction fetch_how_many opt_portal_name
		{
		    FetchStmt *n = makeNode(FetchStmt);
		    n->direction = $2;
		    n->howMany = $3;
		    n->portalname = $4;
		    $$ = (Node *)n;
	        }
	;

opt_direction:  FORWARD				{ $$ = FORWARD; }
	| BACKWARD				{ $$ = BACKWARD; }
	| /*EMPTY*/				{ $$ = FORWARD; /* default */ }
	;

fetch_how_many:  Iconst			
	       { $$ = $1;
		 if ($1 <= 0) elog(WARN,"Please specify nonnegative count for fetch"); }
	|  ALL				{ $$ = 0; /* 0 means fetch all tuples*/}
	|  /*EMPTY*/			{ $$ = 1; /*default*/ }
	;

/*****************************************************************************
 *
 *	QUERY:
 *		GRANT [privileges] ON [relation_name_list] TO [GROUP] grantee
 *
 *****************************************************************************/

GrantStmt: GRANT privileges ON relation_name_list TO grantee opt_with_grant
           {
		$$ = (Node*)makeAclStmt($2,$4,$6,'+');
		free($2);
		free($6);
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
           | operation_commalist {
		$$ = $1;
		}
           ;

operation_commalist: operation {
			$$ = aclmakepriv("",$1);
			}
                   | operation_commalist ',' operation
			{
				$$ = aclmakepriv($1,$3);
				free($1);
			}
                   ;

operation:    SELECT  {
		$$ = ACL_MODE_RD_CHR;
		}
            | INSERT {
		$$ = ACL_MODE_AP_CHR;
		}
            | UPDATE {
		$$ = ACL_MODE_WR_CHR;
		}
            | DELETE {
		$$ = ACL_MODE_WR_CHR;
		}
	    | RULE {
		$$ = ACL_MODE_RU_CHR;
		}
            ;

grantee:      PUBLIC {
		$$ = aclmakeuser("A","");
		}
	    | GROUP Id {
		$$ = aclmakeuser("G",$2);
		}
            | Id {
		$$ = aclmakeuser("U",$1);
	 	}
            ;

opt_with_grant : /* empty */
            |   WITH GRANT OPTION 
                {
                    yyerror("WITH GRANT OPTION is not supported.  Only relation owners can set privileges");
                }
            ;
/*****************************************************************************
 *
 *	QUERY:
 *		REVOKE [privileges] ON [relation_name] FROM [user]
 *
 *****************************************************************************/

RevokeStmt: REVOKE privileges ON relation_name_list FROM grantee
              {
		$$ = (Node*)makeAclStmt($2,$4,$6,'-');
		free($2);
		free($6);
              }
            ;

/*****************************************************************************
 *
 *	QUERY:
 *		move [<dirn>] [<whereto>] [<portalname>]
 *
 *****************************************************************************/

MoveStmt:  MOVE opt_direction opt_move_where opt_portal_name
		{ 
		    MoveStmt *n = makeNode(MoveStmt);
		    n->direction = $2;
		    n->to = FALSE;
		    n->where = $3;
		    n->portalname = $4;
		    $$ = (Node *)n;
		}
	|  MOVE opt_direction TO Iconst opt_portal_name
		{ 
		    MoveStmt *n = makeNode(MoveStmt);
		    n->direction = $2;
		    n->to = TRUE;
		    n->where = $4;
		    n->portalname = $5;
		    $$ = (Node *)n;
		}
	;

opt_move_where: Iconst				{ $$ = $1; }
	| /*EMPTY*/				{ $$ = 1; /* default */ }
	;

opt_portal_name: IN name			{ $$ = $2;}
	| /*EMPTY*/				{ $$ = NULL; }
	;


/*****************************************************************************
 *
 *	QUERY:
 *		define [archive] index <indexname> on <relname>
 *		  using <access> "(" (<col> with <op>)+ ")" [with
 *		  <target_list>]
 *
 *  [where <qual>] is not supported anymore
 *****************************************************************************/

IndexStmt:  CREATE index_opt_unique INDEX index_name ON relation_name
	    access_method_clause '(' index_params ')' opt_with
		{
		    /* should check that access_method is valid,
		       etc ... but doesn't */
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

access_method_clause:   USING access_method      { $$ = $2; }
		      | /* empty -- 'btree' is default access method */
						 { $$ = "btree"; }
	;

index_opt_unique: UNIQUE				 { $$ = TRUE; }
		  | /*empty*/                            { $$ = FALSE; }
	;

/*****************************************************************************
 *
 *	QUERY:
 *		extend index <indexname> [where <qual>]
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
 *	QUERY:
 *		execute recipe <recipeName> 
 *
 *****************************************************************************/

RecipeStmt:  EXECUTE RECIPE recipe_name 
		{
		    RecipeStmt *n;
		    if (!IsTransactionBlock())
			elog(WARN, "EXECUTE RECIPE may only be used in begin/end transaction blocks.");

		    n = makeNode(RecipeStmt);
		    n->recipeName = $3;
		    $$ = (Node *)n;
		}
	;


/*****************************************************************************
 *
 *	QUERY:
 *              define function <fname>
 *                     (language = <lang>, returntype = <typename> 
 *                      [, arch_pct = <percentage | pre-defined>]
 *                      [, disk_pct = <percentage | pre-defined>]
 *                      [, byte_pct = <percentage | pre-defined>]
 *                      [, perbyte_cpu = <int | pre-defined>]
 *                      [, percall_cpu = <int | pre-defined>]
 *                      [, iscachable])
 *                      [arg is (<type-1> { , <type-n>})]
 *                      as <filename or code in language as appropriate>
 *
 *****************************************************************************/

ProcedureStmt:  CREATE FUNCTION def_name def_args 
		   RETURNS def_arg opt_with AS Sconst LANGUAGE Sconst
                {
		    ProcedureStmt *n = makeNode(ProcedureStmt);
		    n->funcname = $3;
		    n->defArgs = $4;
		    n->returnType = (Node *)$6;
		    n->withClause = $7;
		    n->as = $9;
		    n->language = $11;
		    $$ = (Node *)n;
		};

opt_with:  WITH definition			{ $$ = $2; }
	|  /* EMPTY */				{ $$ = NIL; }
	;

def_args:  '(' def_name_list ')'		{ $$ = $2; }
        |  '(' ')'				{ $$ = NIL; }
	;

def_name_list: 	name_list;	


/*****************************************************************************
 *
 *	QUERY:
 *		purge <relname> [before <date>] [after <date>]
 *		  or
 *		purge <relname>  [after<date>][before <date>] 
 *	
 *****************************************************************************/

PurgeStmt:  PURGE relation_name purge_quals
		{ 
		    $3->relname = $2;
		    $$ = (Node *)$3;
		}
	;

purge_quals:  before_clause
		{ 
		    $$ = makeNode(PurgeStmt);
		    $$->beforeDate = $1;
		    $$->afterDate = NULL;
		}
	|  after_clause
		{ 
		    $$ = makeNode(PurgeStmt);
		    $$->beforeDate = NULL;
		    $$->afterDate = $1;
		}
	|  before_clause after_clause
		{ 
		    $$ = makeNode(PurgeStmt);
		    $$->beforeDate = $1;
		    $$->afterDate = $2;
		}
	|  after_clause before_clause
		{ 
		    $$ = makeNode(PurgeStmt);
		    $$->beforeDate = $2;
		    $$->afterDate = $1;
		}
	|  /*EMPTY*/
		{ 
		    $$ = makeNode(PurgeStmt);
		    $$->beforeDate = NULL;
		    $$->afterDate = NULL;
		}
	;

before_clause:	BEFORE date		{ $$ = $2; }
after_clause:	AFTER date		{ $$ = $2; }


/*****************************************************************************
 *
 *	QUERY:
 *
 *	remove function <funcname>
 *		(REMOVE FUNCTION "funcname" (arg1, arg2, ...))
 *	remove aggregate <aggname>
 *		(REMOVE AGGREGATE "aggname" "aggtype")
 *	remove operator <opname>
 *		(REMOVE OPERATOR "opname" (leftoperand_typ rightoperand_typ))
 *	remove type <typename>
 *		(REMOVE TYPE "typename")
 *	remove rule <rulename>
 *		(REMOVE RULE "rulename")
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

remove_type:  Type 				{  $$ = P_TYPE; }
	|  INDEX	 			{  $$ = INDEX; }
	|  RULE 				{  $$ = RULE; }
	|  VIEW					{  $$ = VIEW; }
 	;

RemoveAggrStmt:  DROP AGGREGATE name aggr_argtype
		{
			RemoveAggrStmt *n = makeNode(RemoveAggrStmt);
			n->aggname = $3;
			n->aggtype = $4;
			$$ = (Node *)n;
		}
	;

aggr_argtype:  name				{ $$ = $1; }
	|  '*'					{ $$ = NULL; }
	;

RemoveFuncStmt:  DROP FUNCTION name '(' func_argtypes ')'
                {
		    RemoveFuncStmt *n = makeNode(RemoveFuncStmt);
		    n->funcname = $3;
		    n->args = $5;
		    $$ = (Node *)n;
	        }
          ;

func_argtypes:  name_list			{ $$ = $1; }
        |  /*EMPTY*/				{ $$ = NIL; }
	;

RemoveOperStmt:  DROP OPERATOR all_Op '(' oper_argtypes ')'
		{
		    RemoveOperStmt *n = makeNode(RemoveOperStmt);
		    n->opname = $3;
		    n->args = $5;
		    $$ = (Node *)n;
		}
        ;

all_Op: Op | MathOp;

MathOp:    '+' 	 	{ $$ = "+"; }
	|  '-'   	{ $$ = "-"; }
	|  '*'   	{ $$ = "*"; }
	|  '/'   	{ $$ = "/"; }
	|  '<'   	{ $$ = "<"; }
	|  '>'   	{ $$ = ">"; }
	|  '='   	{ $$ = "="; }
	;

oper_argtypes:  name	
		{ 
		   elog(WARN, "parser: argument type missing (use NONE for unary operators)");
		}
	| name ',' name
		{ $$ = makeList(makeString($1), makeString($3), -1); }
	| NONE ',' name		/* left unary */
		{ $$ = makeList(NULL, makeString($3), -1); }
	| name ',' NONE		/* right unary */
		{ $$ = makeList(makeString($1), NULL, -1); }
	;

/*****************************************************************************
 *
 *	QUERY:	 
 *		rename <attrname1> in <relname> [*] to <attrname2>
 *		rename <relname1> to <relname2>
 *	
 *****************************************************************************/

RenameStmt:  ALTER TABLE relation_name opt_inh_star 
		  RENAME opt_column opt_name TO name
		{ 
		    RenameStmt *n = makeNode(RenameStmt);
		    n->relname = $3;
		    n->inh = $4;
		    n->column = $7;
		    n->newname = $9;
		    $$ = (Node *)n;
		}
	;

opt_name:  name				{ $$ = $1; }
	|  /*EMPTY*/			{ $$ = NULL; }
	;

opt_column:  COLUMN			{ $$ = COLUMN; }
	| /*EMPTY*/			{ $$ = 0; }
	;


/*****************************************************************************
 *	 
 *	QUERY:	Define Rewrite Rule , Define Tuple Rule 
 *		Define Rule <old rules >
 *
 *      only rewrite rule is supported -- ay 9/94
 *	 
 *****************************************************************************/

RuleStmt:  CREATE RULE name AS 
	   { QueryIsRule=TRUE; }
	   ON event TO event_object where_clause
	   DO opt_instead OptStmtList
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

OptStmtList:  NOTHING			{ $$ = NIL; }
	| OptimizableStmt		{ $$ = lcons($1, NIL); }	
	| '[' OptStmtBlock ']'		{ $$ = $2; }
        ;

OptStmtBlock:  OptStmtMulti
               {  $$ = $1; }
	|  OptimizableStmt
		{ $$ = lcons($1, NIL); }
	;
	
OptStmtMulti:  OptStmtMulti OptimizableStmt ';'
               {  $$ = lappend($1, $2); }
	|  OptStmtMulti OptimizableStmt
               {  $$ = lappend($1, $2); }
	|  OptimizableStmt ';'
		{ $$ = lcons($1, NIL); }
	;
	
event_object: relation_name '.' attr_name
		{ 
		    $$ = makeNode(Attr);
		    $$->relname = $1;
		    $$->paramNo = NULL;
		    $$->attrs = lcons(makeString($3), NIL);
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
event: 	SELECT				{ $$ = CMD_SELECT; }
	| UPDATE 			{ $$ = CMD_UPDATE; }
	| DELETE 			{ $$ = CMD_DELETE; }
	| INSERT			{ $$ = CMD_INSERT; }
	 ;

opt_instead:  INSTEAD			{ $$ = TRUE; }
	| /* EMPTY */			{ $$ = FALSE; }
	;


/*****************************************************************************
 *
 *	QUERY:
 *		NOTIFY <relation_name>  can appear both in rule bodies and
 *		as a query-level command
 *
 *****************************************************************************/

NotifyStmt: NOTIFY relation_name 
		{
		    NotifyStmt *n = makeNode(NotifyStmt);
		    n->relname = $2;
		    $$ = (Node *)n;
		}
	;

ListenStmt: LISTEN relation_name 
		{
		    ListenStmt *n = makeNode(ListenStmt);
		    n->relname = $2;
		    $$ = (Node *)n;
		}
;


/*****************************************************************************
 *
 *	Transactions:
 *
 *	abort transaction
 *		(ABORT)
 *	begin transaction
 *		(BEGIN)
 *	end transaction
 *		(END)
 *	
 *****************************************************************************/

TransactionStmt:  ABORT_TRANS TRANSACTION
		{ 
		    TransactionStmt *n = makeNode(TransactionStmt); 
		    n->command = ABORT_TRANS; 
		    $$ = (Node *)n;
		}
	| BEGIN_TRANS TRANSACTION
		{ 
		    TransactionStmt *n = makeNode(TransactionStmt); 
		    n->command = BEGIN_TRANS;
		    $$ = (Node *)n;
		}
	| BEGIN_TRANS WORK
		{
		    TransactionStmt *n = makeNode(TransactionStmt); 
		    n->command = BEGIN_TRANS;
		    $$ = (Node *)n;
		}
	| COMMIT WORK
		{
		    TransactionStmt *n = makeNode(TransactionStmt); 
		    n->command = END_TRANS;
		    $$ = (Node *)n;
		}
	| END_TRANS TRANSACTION
		{ 
		    TransactionStmt *n = makeNode(TransactionStmt); 
		    n->command = END_TRANS;
		    $$ = (Node *)n;
		}
	| ROLLBACK WORK
		{
		    TransactionStmt *n = makeNode(TransactionStmt); 
		    n->command = ABORT_TRANS;
		    $$ = (Node *)n;
		}

	| ABORT_TRANS
		{ 
		    TransactionStmt *n = makeNode(TransactionStmt); 
		    n->command = ABORT_TRANS; 
		    $$ = (Node *)n;
		}
	| BEGIN_TRANS
		{ 
		    TransactionStmt *n = makeNode(TransactionStmt); 
		    n->command = BEGIN_TRANS;
		    $$ = (Node *)n;
		}
	| COMMIT
		{
		    TransactionStmt *n = makeNode(TransactionStmt); 
		    n->command = END_TRANS;
		    $$ = (Node *)n;
		}

	| END_TRANS
		{ 
		    TransactionStmt *n = makeNode(TransactionStmt); 
		    n->command = END_TRANS;
		    $$ = (Node *)n;
		}
	| ROLLBACK
		{
		    TransactionStmt *n = makeNode(TransactionStmt); 
		    n->command = ABORT_TRANS;
		    $$ = (Node *)n;
		}
	;


/*****************************************************************************
 *
 *	QUERY:
 *		define view <viewname> '('target-list ')' [where <quals> ]
 *
 *****************************************************************************/

ViewStmt:  CREATE VIEW name AS RetrieveStmt
		{ 
		    ViewStmt *n = makeNode(ViewStmt);
		    n->viewname = $3;
		    n->query = (Query *)$5;
		    $$ = (Node *)n;
		}
	;


/*****************************************************************************
 *
 *	QUERY:
 *		load "filename"
 *
 *****************************************************************************/

LoadStmt: LOAD file_name
                { 
		    LoadStmt *n = makeNode(LoadStmt);
		    n->filename = $2;
		    $$ = (Node *)n;
		}
        ;


/*****************************************************************************
 *
 *	QUERY:
 *	        createdb dbname
 *
 *****************************************************************************/

CreatedbStmt:  CREATE DATABASE database_name
                {
		    CreatedbStmt *n = makeNode(CreatedbStmt);
		    n->dbname = $3;
		    $$ = (Node *)n;
		}
        ;


/*****************************************************************************
 *
 *	QUERY:
 *	        destroydb dbname
 *
 *****************************************************************************/

DestroydbStmt:  DROP DATABASE database_name
                {
		    DestroydbStmt *n = makeNode(DestroydbStmt);
		    n->dbname = $3;
		    $$ = (Node *)n;
		}
        ;


/*****************************************************************************
 *
 *	QUERY:
 *		cluster <index_name> on <relation_name>
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
 *	QUERY:
 *		vacuum
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
         | VACUUM opt_verbose relation_name opt_analyze opt_va_list
               {
                   VacuumStmt *n = makeNode(VacuumStmt);
                   n->verbose = $2;
                   n->analyze = $4;
                   n->vacrel = $3;
                   n->va_spec = $5;
                   if ( $5 != NIL && !$4 )
                   	elog (WARN, "parser: syntax error at or near \"(\"");
                   $$ = (Node *)n;
               }
       ;

opt_verbose:  VERBOSE			{ $$ = TRUE; }
	| /* EMPTY */			{ $$ = FALSE; }
	;

opt_analyze:  ANALYZE			{ $$ = TRUE; }
	| /* EMPTY */			{ $$ = FALSE; }
	;

opt_va_list: '(' va_list ')'
		{ $$ = $2; }
	| /* EMPTY */
		{ $$ = NIL; }
	;	

va_list: name
		{ $$=lcons($1,NIL); }
	| va_list ',' name
		{ $$=lappend($1,$3); }
	;	
       
/*****************************************************************************
 *
 *	QUERY:
 *		EXPLAIN query
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
 *                                                                           *
 *	Optimizable Stmts:                                                   *
 *                                                                           *
 *	one of the five queries processed by the planner                     *
 *                                                                           *
 *	[ultimately] produces query-trees as specified                       *
 *	in the query-spec document in ~postgres/ref                          *
 *                                                                           *
 *****************************************************************************/

OptimizableStmt:  RetrieveStmt
	| CursorStmt	
	| ReplaceStmt
	| AppendStmt
        | NotifyStmt
        | DeleteStmt			/* by default all are $$=$1 */
	;


/*****************************************************************************
 *
 *	QUERY:
 *		INSERT STATEMENTS
 *  
 *****************************************************************************/

AppendStmt:  INSERT INTO relation_name opt_column_list insert_rest
		{
		    $5->relname = $3;
		    $5->cols = $4;
		    $$ = (Node *)$5;
                }
	;

insert_rest: VALUES '(' res_target_list2 ')'
		{
		    $$ = makeNode(AppendStmt);
		    $$->targetList = $3;
		    $$->fromClause = NIL;
		    $$->whereClause = NULL;
		}
	| SELECT res_target_list2 from_clause where_clause
		{
		    $$ = makeNode(AppendStmt);
		    $$->targetList = $2;
		    $$->fromClause = $3;
		    $$->whereClause = $4;
		}
	;

opt_column_list: '(' columnList ')'		{ $$ = $2; }
	| /*EMPTY*/				{ $$ = NIL; }
	;

columnList:	
	  columnList ',' columnElem
		{ $$ = lappend($1, $3); }
	| columnElem
		{ $$ = lcons($1, NIL); }
	;

columnElem: Id opt_indirection
	  	{
		    Ident *id = makeNode(Ident);
		    id->name = $1;
		    id->indirection = $2;
		    $$ = (Node *)id;
		}
	;

/*****************************************************************************
 *
 *	QUERY:
 *		DELETE STATEMENTS
 *
 *****************************************************************************/
   
DeleteStmt:  DELETE FROM relation_name
	     where_clause
                {
		    DeleteStmt *n = makeNode(DeleteStmt);
		    n->relname = $3;
		    n->whereClause = $4;
		    $$ = (Node *)n;
		}
        ;


/*****************************************************************************
 *
 *	QUERY:
 *		ReplaceStmt (UPDATE)
 *
 *****************************************************************************/

ReplaceStmt:  UPDATE relation_name 
	      SET res_target_list
	      from_clause
	      where_clause
                {
		    ReplaceStmt *n = makeNode(ReplaceStmt);
		    n->relname = $2;
		    n->targetList = $4;
		    n->fromClause = $5;
		    n->whereClause = $6;
		    $$ = (Node *)n;
                }
        ;
		 

/*****************************************************************************
 *
 *	QUERY:
 *		CURSOR STATEMENTS
 *
 *****************************************************************************/

CursorStmt:  DECLARE name opt_binary CURSOR FOR 
	     SELECT opt_unique res_target_list2	
	     from_clause where_clause group_clause sort_clause
		{
		    CursorStmt *n = makeNode(CursorStmt);

		    /* from PORTAL name */
		    /*
		     *  15 august 1991 -- since 3.0 postgres does locking
		     *  right, we discovered that portals were violating
		     *  locking protocol.  portal locks cannot span xacts.
		     *  as a short-term fix, we installed the check here. 
		     *				-- mao
		     */
		    if (!IsTransactionBlock())
			elog(WARN, "Named portals may only be used in begin/end transaction blocks.");

		    n->portalname = $2;
		    n->binary = $3;
		    n->unique = $7;
		    n->targetList = $8;
		    n->fromClause = $9;
		    n->whereClause = $10;
		    n->groupClause = $11;
		    n->sortClause = $12;
		    $$ = (Node *)n;
		}
	;


/*****************************************************************************
 *
 *	QUERY:
 *		SELECT STATEMENTS
 *
 *****************************************************************************/

RetrieveStmt:  SELECT opt_unique res_target_list2
	       result from_clause where_clause 
	       group_clause having_clause
	       sort_clause
  		{
		    RetrieveStmt *n = makeNode(RetrieveStmt);
		    n->unique = $2;
		    n->targetList = $3;
		    n->into = $4;
		    n->fromClause = $5;
		    n->whereClause = $6;
		    n->groupClause = $7;
		    n->havingClause = $8;
		    n->sortClause = $9;
		    $$ = (Node *)n;
		}
	;

result:  INTO TABLE relation_name
		{  $$= $3;  /* should check for archive level */  }
	| /*EMPTY*/
		{  $$ = NULL;  }
	;

opt_unique:  DISTINCT		{ $$ = "*"; }
	| DISTINCT ON Id	{ $$ = $3; }
	| /*EMPTY*/		{ $$ = NULL;}
	;

sort_clause:  ORDER BY sortby_list			{ $$ = $3; }
	|  /*EMPTY*/				{ $$ = NIL; }
	;

sortby_list:  sortby
		{ $$ = lcons($1, NIL); }
	| sortby_list ',' sortby
		{ $$ = lappend($1, $3); }
	;

sortby:  Id OptUseOp
		{ 
		    $$ = makeNode(SortGroupBy);
		    $$->resno = 0;
		    $$->range = NULL;
		    $$->name = $1;
		    $$->useOp = $2;
		}
	| Id '.' Id OptUseOp
		{
		    $$ = makeNode(SortGroupBy);
		    $$->resno = 0;
		    $$->range = $1;
		    $$->name = $3;
		    $$->useOp = $4;
		}
	| Iconst OptUseOp
		{
		    $$ = makeNode(SortGroupBy);
		    $$->resno = $1;
		    $$->range = NULL;
		    $$->name = NULL;
		    $$->useOp = $2;
		}
	;

OptUseOp:  USING Op				{ $$ = $2; }
	|  USING '<'				{ $$ = "<"; }
	|  USING '>'				{ $$ = ">"; }
	|  ASC					{ $$ = "<"; }
	|  DESC					{ $$ = ">"; }
	|  /*EMPTY*/				{ $$ = "<"; /*default*/ }
	;

index_params: index_list			{ $$ = $1; }
	| func_index				{ $$ = lcons($1,NIL); }
	;

index_list:
	  index_list ',' index_elem
		{ $$ = lappend($1, $3); }
	| index_elem
		{ $$ = lcons($1, NIL); }
	;

func_index: name '(' name_list ')' opt_type opt_class
		{
		    $$ = makeNode(IndexElem);
		    $$->name = $1;
		    $$->args = $3;
		    $$->class = $6;
	            $$->tname = $5;
		}
	  ;

index_elem:  attr_name opt_type opt_class
		{
		    $$ = makeNode(IndexElem);
		    $$->name = $1;
		    $$->args = NIL;
		    $$->class = $3;
 	            $$->tname = $2;
		}
	;

opt_type: ':' Typename                          { $$ = $2;}
        |  /*EMPTY*/                            { $$ = NULL;}
        ;

opt_class:  class
	|  WITH class				{ $$ = $2; }
	|  /*EMPTY*/				{ $$ = NULL; }
	;

/*
 *  jimmy bell-style recursive queries aren't supported in the
 *  current system.
 *
 *  ...however, recursive addattr and rename supported.  make special 
 *  cases for these.
 * 
 *  XXX i believe '*' should be the default behavior, but...
 */
opt_inh_star: '*'			{ $$ = TRUE; }
	|  /*EMPTY*/			{ $$ = FALSE; }
	;

relation_name_list:	name_list ;

name_list: name 			
		{ $$=lcons(makeString($1),NIL); }
	| name_list ',' name		
		{ $$=lappend($1,makeString($3)); }
	;	

group_clause: GROUP BY groupby_list		{ $$ = $3; }
	| /*EMPTY*/				{ $$ = NIL; }
	;

groupby_list: groupby				{ $$ = lcons($1, NIL); }
	| groupby_list ',' groupby		{ $$ = lappend($1, $3); }
	;

groupby:  Id
		{
		    $$ = makeNode(SortGroupBy);
		    $$->resno = 0;
		    $$->range = NULL;
		    $$->name = $1;
		    $$->useOp = NULL;
		}
	| Id '.' Id
		{
		    $$ = makeNode(SortGroupBy);
		    $$->resno = 0;
		    $$->range = $1;
		    $$->name = $3;
		    $$->useOp = NULL;
		}
	| Iconst
		{
		    $$ = makeNode(SortGroupBy);
		    $$->resno = $1;
		    $$->range = NULL;
		    $$->name = NULL;
		    $$->useOp = NULL;
		}
	;

having_clause: HAVING a_expr			{ $$ = $2; }
	| /*EMPTY*/				{ $$ = NULL; }
	;

/*****************************************************************************
 *  
 *  clauses common to all Optimizable Stmts:
 *  	from_clause 	-
 *      where_clause 	-
 *	
 *****************************************************************************/

from_clause:  FROM from_list 			{ $$ = $2; }
	| /*EMPTY*/				{ $$ = NIL; }
	;

from_list:  from_list ',' from_val
		{ $$ = lappend($1, $3); }
	|  from_val
		{ $$ = lcons($1, NIL); }
	;

from_val:  relation_expr AS var_name
		{
		    $$ = makeNode(RangeVar);
		    $$->relExpr = $1;
		    $$->name = $3;
		}	
	| relation_expr var_name
		{
		    $$ = makeNode(RangeVar);
		    $$->relExpr = $1;
		    $$->name = $2;
		}
	| relation_expr
		{
		    $$ = makeNode(RangeVar);
		    $$->relExpr = $1;
		    $$->name = NULL;
		}
	;

where_clause:  WHERE a_expr		{ $$ = $2; }
	| /*EMPTY*/		        { $$ = NULL;  /* no qualifiers */ } 
	;

relation_expr:  relation_name
  		{ 
		    /* normal relations */
		    $$ = makeNode(RelExpr);
		    $$->relname = $1;
		    $$->inh = FALSE;
		    $$->timeRange = NULL;
		}
	| relation_name '*'		  %prec '='
		{ 
		    /* inheiritance query */
		    $$ = makeNode(RelExpr);
		    $$->relname = $1;
		    $$->inh = TRUE;
		    $$->timeRange = NULL;
		}
	| relation_name time_range 
		{ 
		    /* time-qualified query */
		    $$ = makeNode(RelExpr);
		    $$->relname = $1;
		    $$->inh = FALSE;
		    $$->timeRange = $2;
		}
	;

	  
time_range:  '[' opt_range_start ',' opt_range_end ']'
        	{ 
		    $$ = makeNode(TimeRange);
		    $$->startDate = $2;
		    $$->endDate = $4;
		}
	| '[' date ']'
		{ 
		    $$ = makeNode(TimeRange);
		    $$->startDate = $2;
		    $$->endDate = NULL;
		}
        ;

opt_range_start:  date
	|  /*EMPTY*/				{ $$ = "epoch"; }
	;

opt_range_end:  date
	|  /*EMPTY*/				{ $$ = "now"; }
	;

opt_array_bounds:  '[' ']' nest_array_bounds
		{  $$ = lcons(makeInteger(-1), $3); }
	| '[' Iconst ']' nest_array_bounds
		{  $$ = lcons(makeInteger($2), $4); }
	| /* EMPTY */				
		{  $$ = NIL; }
	;

nest_array_bounds:  '[' ']' nest_array_bounds
		{  $$ = lcons(makeInteger(-1), $3); }
	| '[' Iconst ']' nest_array_bounds 
		{  $$ = lcons(makeInteger($2), $4); }
	| /*EMPTY*/
		{  $$ = NIL; }
	;

typname:  name  
		{
		    char *tname = xlateSqlType($1);
		    $$ = makeNode(TypeName);
		    $$->name = tname;

		    /* Is this the name of a complex type? If so, implement
		     * it as a set.
		     */
		    if (!strcmp(saved_relname, tname)) {
		 	/* This attr is the same type as the relation 
			 * being defined. The classic example: create
			 * emp(name=text,mgr=emp)
			 */
			$$->setof = TRUE;
		    }else if (get_typrelid((Type)type(tname))
				!= InvalidOid) {
			 /* (Eventually add in here that the set can only 
			  * contain one element.)
			  */
			$$->setof = TRUE;
	    	    } else {
			$$->setof = FALSE;
		    }
		}
        | SETOF name
		{
		    $$ = makeNode(TypeName);
		    $$->name = $2;
		    $$->setof = TRUE;
		}
        ;

Typename:  typname opt_array_bounds		
		{ 
		    $$ = $1;
		    $$->arrayBounds = $2;
		}
	| name '(' Iconst ')'
		{
		    /*
		     * The following implements char() and varchar().
		     * We do it here instead of the 'typname:' production
		     * because we don't want to allow arrays of varchar().
		     * I haven't thought about whether that will work or not.
		      *                             - ay 6/95
		     */
		    $$ = makeNode(TypeName);
		    if (!strcasecmp($1, "char")) {
			$$->name = "bpchar"; /*  strdup("bpchar"); */
		    } else if (!strcasecmp($1, "varchar")) {
			$$->name = "varchar"; /* strdup("varchar"); */
		    } else {
			yyerror("parse error");
		    }
		    if ($3 < 1) {
			elog(WARN, "length for '%s' type must be at least 1",
			     $1);
		    } else if ($3 > 4096) {
			/* we can store a char() of length up to the size
			   of a page (8KB) - page headers and friends but
			   just to be safe here...  - ay 6/95 */
			elog(WARN, "length for '%s' type cannot exceed 4096",
			     $1);
		    }
		    /* we actually implement this sort of like a varlen, so
		       the first 4 bytes is the length. (the difference
		       between this and "text" is that we blank-pad and 
		       truncate where necessary */
		    $$->typlen = 4 + $3;
		}
	;


/*****************************************************************************
 *
 *  expression grammar, still needs some cleanup
 *
 *****************************************************************************/

a_expr_or_null: a_expr
		{ $$ = $1;}
	| Pnull
		{	
		    A_Const *n = makeNode(A_Const);
		    n->val.type = T_Null;
		    $$ = (Node *)n;
		}
		
a_expr:  attr opt_indirection
		{
		    $1->indirection = $2;
		    $$ = (Node *)$1;
		}
	| AexprConst
		{   $$ = $1;  }
	| '-' a_expr %prec UMINUS
		{   $$ = makeA_Expr(OP, "-", NULL, $2); }
	| a_expr '+' a_expr
		{   $$ = makeA_Expr(OP, "+", $1, $3); }
	| a_expr '-' a_expr
		{   $$ = makeA_Expr(OP, "-", $1, $3); }
	| a_expr '/' a_expr
		{   $$ = makeA_Expr(OP, "/", $1, $3); }
	| a_expr '*' a_expr
		{   $$ = makeA_Expr(OP, "*", $1, $3); }
	| a_expr '<' a_expr
		{   $$ = makeA_Expr(OP, "<", $1, $3); }
	| a_expr '>' a_expr
		{   $$ = makeA_Expr(OP, ">", $1, $3); }
	| a_expr '=' a_expr
		{   $$ = makeA_Expr(OP, "=", $1, $3); }
	| ':' a_expr
		{   $$ = makeA_Expr(OP, ":", NULL, $2); }
	| ';' a_expr
		{   $$ = makeA_Expr(OP, ";", NULL, $2); }
	| '|' a_expr
		{   $$ = makeA_Expr(OP, "|", NULL, $2); }
	| AexprConst TYPECAST Typename
		{ 
		    /* AexprConst can be either A_Const or ParamNo */
		    if (nodeTag($1) == T_A_Const) {
			((A_Const *)$1)->typename = $3;
		    }else {
			((ParamNo *)$1)->typename = $3;
		    }
		    $$ = (Node *)$1;
		}
	| CAST AexprConst AS Typename
		{
		    /* AexprConst can be either A_Const or ParamNo */
		    if (nodeTag($2) == T_A_Const) {
			((A_Const *)$2)->typename = $4;
		    }else {
			((ParamNo *)$2)->typename = $4;
		    }
		    $$ = (Node *)$2;
		}
	| '(' a_expr_or_null ')'
		{   $$ = $2; }
	| a_expr Op a_expr
		{   $$ = makeA_Expr(OP, $2, $1, $3); }
	| a_expr LIKE a_expr
		{   $$ = makeA_Expr(OP, "~~", $1, $3); }
	| a_expr NOT LIKE a_expr
		{   $$ = makeA_Expr(OP, "!~~", $1, $4); }
	| Op a_expr
		{   $$ = makeA_Expr(OP, $1, NULL, $2); }
	| a_expr Op
		{   $$ = makeA_Expr(OP, $2, $1, NULL); }
	| Id
		{   /* could be a column name or a relation_name */
		    Ident *n = makeNode(Ident);
		    n->name = $1;
		    n->indirection = NULL;
		    $$ = (Node *)n;
		}
	| name '(' '*' ')'
		{
		    FuncCall *n = makeNode(FuncCall);
		    Ident *star = makeNode(Ident);

		    /* cheap hack for aggregate (eg. count) */
		    star->name = "oid"; 
		    n->funcname = $1;
		    n->args = lcons(star, NIL);
		    $$ = (Node *)n;
		}
	| name '(' ')'
		{
		    FuncCall *n = makeNode(FuncCall);
		    n->funcname = $1;
		    n->args = NIL;
		    $$ = (Node *)n;
		}
	| name '(' expr_list ')'
		{
		    FuncCall *n = makeNode(FuncCall);
		    n->funcname = $1;
		    n->args = $3;
		    $$ = (Node *)n;
		}
	| a_expr ISNULL
		{   $$ = makeA_Expr(ISNULL, NULL, $1, NULL); }
	| a_expr IS PNULL
		{   $$ = makeA_Expr(ISNULL, NULL, $1, NULL); }
	| a_expr NOTNULL
		{   $$ = makeA_Expr(NOTNULL, NULL, $1, NULL); }
	| a_expr IS NOT PNULL
		{   $$ = makeA_Expr(NOTNULL, NULL, $1, NULL); }
	| a_expr BETWEEN AexprConst AND AexprConst
		{   $$ = makeA_Expr(AND, NULL,
			makeA_Expr(OP, ">=", $1, $3),
			makeA_Expr(OP, "<=", $1,$5));
		}
	| a_expr NOT BETWEEN AexprConst AND AexprConst
		{   $$ = makeA_Expr(OR, NULL,
			makeA_Expr(OP, "<", $1, $4),
			makeA_Expr(OP, ">", $1, $6));
		}
	| a_expr IN { saved_In_Expr = $1; } '(' in_expr_nodes ')'
		{   $$ = $5; }
	| a_expr NOT IN { saved_In_Expr = $1; } '(' not_in_expr_nodes ')'
		{   $$ = $6; }
	| a_expr AND a_expr
		{   $$ = makeA_Expr(AND, NULL, $1, $3); }
	| a_expr OR a_expr
		{   $$ = makeA_Expr(OR, NULL, $1, $3); }
	| NOT a_expr
		{   $$ = makeA_Expr(NOT, NULL, NULL, $2); }
	;

opt_indirection:  '[' a_expr ']' opt_indirection 
		{
		    A_Indices *ai = makeNode(A_Indices);
		    ai->lidx = NULL;
		    ai->uidx = $2;
		    $$ = lcons(ai, $4);
		}
	| '[' a_expr ':' a_expr ']' opt_indirection 
		{
		    A_Indices *ai = makeNode(A_Indices);
		    ai->lidx = $2;
		    ai->uidx = $4;
		    $$ = lcons(ai, $6);
		}
	| /* EMPTY */			
		{   $$ = NIL; }
	;
   
expr_list: a_expr_or_null
		{ $$ = lcons($1, NIL); }
	|  expr_list ',' a_expr_or_null
		{ $$ = lappend($1, $3); }
	;

in_expr_nodes: AexprConst
		{   $$ = makeA_Expr(OP, "=", saved_In_Expr, $1); }
	|  in_expr_nodes ',' AexprConst
		{   $$ = makeA_Expr(OR, NULL, $1,
			makeA_Expr(OP, "=", saved_In_Expr, $3));
		}
	;

not_in_expr_nodes: AexprConst
		{   $$ = makeA_Expr(OP, "<>", saved_In_Expr, $1); }
	|  not_in_expr_nodes ',' AexprConst
		{   $$ = makeA_Expr(AND, NULL, $1,
			makeA_Expr(OP, "<>", saved_In_Expr, $3));
		}
	;

attr:  relation_name '.' attrs
        	{
		    $$ = makeNode(Attr);
		    $$->relname = $1;
		    $$->paramNo = NULL;
		    $$->attrs = $3;
		    $$->indirection = NULL;
		}
	| ParamNo '.' attrs
		{
		    $$ = makeNode(Attr);
		    $$->relname = NULL;
		    $$->paramNo = $1;
		    $$->attrs = $3;
		    $$->indirection = NULL;
		}
	;

attrs:    attr_name                       	
		{ $$ = lcons(makeString($1), NIL); }
	| attrs '.' attr_name             
		{ $$ = lappend($1, makeString($3)); }
	| attrs '.' '*'
		{ $$ = lappend($1, makeString("*")); }
	;


/*****************************************************************************
 *
 *  target lists
 *
 *****************************************************************************/

res_target_list:  res_target_list ',' res_target_el	
		{   $$ = lappend($1,$3);  }
	| res_target_el 			
		{   $$ = lcons($1, NIL);  }
	| '*'
		{
		    ResTarget *rt = makeNode(ResTarget);
		    Attr *att = makeNode(Attr);
		    att->relname = "*";
		    att->paramNo = NULL;
		    att->attrs = NULL;
		    att->indirection = NIL;
		    rt->name = NULL;
		    rt->indirection = NULL;
		    rt->val = (Node *)att;
		    $$ = lcons(rt, NIL);
		}
	;

res_target_el: Id opt_indirection '=' a_expr_or_null
		{
		    $$ = makeNode(ResTarget);
		    $$->name = $1;
		    $$->indirection = $2;
		    $$->val = (Node *)$4;
		}
	| attr opt_indirection
		{
		    $$ = makeNode(ResTarget);
		    $$->name = NULL;
		    $$->indirection = $2;
		    $$->val = (Node *)$1;
		}
	| relation_name '.' '*'
		{   
		    Attr *att = makeNode(Attr);
		    att->relname = $1;
		    att->paramNo = NULL;
		    att->attrs = lcons(makeString("*"), NIL);
		    att->indirection = NIL;
		    $$ = makeNode(ResTarget);
		    $$->name = NULL;
		    $$->indirection = NULL;
		    $$->val = (Node *)att;
		}
	;		 

/*
** target list for select.
** should get rid of the other but is still needed by the defunct retrieve into
** and update (uses a subset)
*/
res_target_list2:
	  res_target_list2 ',' res_target_el2	
		{   $$ = lappend($1, $3);  }
	| res_target_el2 			
		{   $$ = lcons($1, NIL);  }
	;

/* AS is not optional because shift/red conflict with unary ops */
res_target_el2: a_expr AS Id 
	  	{
		    $$ = makeNode(ResTarget);
		    $$->name = $3;
		    $$->indirection = NULL;
		    $$->val = (Node *)$1;
		}
	| a_expr_or_null
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
		    att->attrs = lcons(makeString("*"), NIL);
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

opt_id:  Id					{ $$ = $1; }
	| /* EMPTY */				{ $$ = NULL; }
	;

relation_name:  SpecialRuleRelation
          	{
                   $$ = $1;
                   strNcpy(saved_relname, $1, NAMEDATALEN-1);
	        }
	| Id
	  	{
		    /* disallow refs to magic system tables */
  		    if (strcmp(LogRelationName, $1) == 0
  		       || strcmp(VariableRelationName, $1) == 0
  		       || strcmp(TimeRelationName, $1) == 0
  		       || strcmp(MagicRelationName, $1) == 0) {
			elog(WARN, "%s cannot be accessed by users", $1);
		    } else {
			$$ = $1;
		    }
                    strNcpy(saved_relname, $1, NAMEDATALEN-1);
		}
	;

database_name:		Id		{ $$ = $1; };
access_method: 		Id		{ $$ = $1; };
attr_name: 		Id		{ $$ = $1; };
class: 			Id		{ $$ = $1; };
index_name: 		Id		{ $$ = $1; };
var_name:		Id		{ $$ = $1; };
name:			Id		{ $$ = $1; };

date:			Sconst		{ $$ = $1; };
file_name:		Sconst		{ $$ = $1; };
recipe_name:		Id		{ $$ = $1; };

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
		    n->val.val.dval = $1;
		    $$ = (Node *)n;
		}
	| Sconst
		{  
		    A_Const *n = makeNode(A_Const);
		    n->val.type = T_String;
		    n->val.val.str = $1;
		    $$ = (Node *)n;
		}
	| ParamNo
		{   $$ = (Node *)$1;  }
	;

ParamNo:  PARAM
		{
		    $$ = makeNode(ParamNo);
		    $$->number = $1;
		}
	;

NumConst:  Iconst			{ $$ = makeInteger($1); }
	|  FCONST 			{ $$ = makeFloat($1); }
	;

Iconst:  ICONST				{ $$ = $1; };
Sconst:	 SCONST				{ $$ = $1; };

Id:  IDENT           			{ $$ = $1; };

SpecialRuleRelation:  CURRENT
		{ 
		    if (QueryIsRule)
			$$ = "*CURRENT*";
		    else 
			elog(WARN,"CURRENT used in non-rule query");
		}
	| NEW
		{ 
		    if (QueryIsRule)
			$$ = "*NEW*";
		    else 
			elog(WARN,"NEW used in non-rule query"); 
		}
	;

Type:	P_TYPE;
Pnull:	PNULL;


%%

static Node *makeA_Expr(int oper, char *opname, Node *lexpr, Node *rexpr)
{
    A_Expr *a = makeNode(A_Expr);
    a->oper = oper;
    a->opname = opname;
    a->lexpr = lexpr;
    a->rexpr = rexpr;
    return (Node *)a;
}

static char *
xlateSqlType(char *name)
{
    if (!strcasecmp(name,"int") ||
	!strcasecmp(name,"integer"))
	return "int4"; /* strdup("int4") --   strdup leaks memory here */
    else if (!strcasecmp(name, "smallint"))
	return "int2";
    else if (!strcasecmp(name, "float") ||
	     !strcasecmp(name, "real"))
	return "float8";
    else
	return name;
}

void parser_init(Oid *typev, int nargs)
{
    QueryIsRule = false;
    saved_relname[0]= '\0';
    saved_In_Expr = NULL;
    
    param_type_init(typev, nargs);
}

