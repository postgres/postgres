/* Copyright comment */
%{
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "catalog/catname.h"

#include "type.h"
#include "extern.h"

/*
 * Variables containing simple states.
 */
static int	struct_level = 0;
static char	*do_str = NULL, errortext[128];
static int	do_length = 0;
static int      QueryIsRule = 0;

/* temporarily store record members while creating the data structure */
struct ECPGrecord_member *record_member_list[128] = { NULL };

/* keep a list of cursors */
struct cursor *cur = NULL;

/*
 * Handle the filename and line numbering.
 */
char * input_filename = NULL;

static void
output_line_number()
{
    if (input_filename)
       fprintf(yyout, "\n#line %d \"%s\"\n", yylineno, input_filename);
}

/*
 * store the whenever action here
 */
static struct when when_error, when_nf;

static void
print_action(struct when *w)
{
	switch (w->code)
	{
		case W_SQLPRINT: fprintf(yyout, "sqlprint();");
                                 break;
		case W_GOTO:	 fprintf(yyout, "goto %s;", w->command);
				 break;
		case W_DO:	 fprintf(yyout, "%s;", w->command);
				 break;
		case W_STOP:	 fprintf(yyout, "exit (1);");
				 break;
		default:	 fprintf(yyout, "{/* %d not implemented yet */}", w->code);
				 break;
	}
}

static void
whenever_action()
{
	if (when_nf.code != W_NOTHING)
	{
		fprintf(yyout, "\nif (SQLCODE > 0) ");
		print_action(&when_nf);
	}
	if (when_error.code != W_NOTHING)
        {
                fprintf(yyout, "\nif (SQLCODE < 0) ");
		print_action(&when_error);
        }
	output_line_number();
}

/*
 * Handling of variables.
 */

/*
 * brace level counter
 */
int braces_open;

/* This is a linked list of the variable names and types. */
struct variable
{
    char * name;
    struct ECPGtype * type;
    int brace_level;
    struct variable * next;
};

static struct variable * allvariables = NULL;

static struct variable *
find_variable(char * name)
{
    struct variable * p;
    char * errorstring = (char *) mm_alloc(strlen(name) + 100);

    for (p = allvariables; p; p = p->next)
    {
        if (strcmp(p->name, name) == 0)
	    return p;
    }

    sprintf(errorstring, "The variable :%s is not declared", name);
    yyerror(errorstring);
    free (errorstring);

    return NULL;
}


static void
new_variable(const char * name, struct ECPGtype * type)
{
    struct variable * p = (struct variable*) mm_alloc(sizeof(struct variable));

    p->name = strdup(name);
    p->type = type;
    p->brace_level = braces_open;

    p->next = allvariables;
    allvariables = p;
}

static void
remove_variables(int brace_level)
{
    struct variable * p, *prev;

    for (p = prev = allvariables; p; p = p ? p->next : NULL)
    {
	if (p->brace_level >= brace_level)
	{
	    /* remove it */
	    if (p == allvariables)
		prev = allvariables = p->next;
	    else
		prev->next = p->next;

	    ECPGfree_type(p->type);
	    free(p->name);
	    free(p);
	    p = prev;
	}
	else
	    prev = p;
    }
}


/*
 * Here are the variables that need to be handled on every request.
 * These are of two kinds: input and output.
 * I will make two lists for them.
 */
struct arguments {
    struct variable * variable;
    struct variable * indicator;
    struct arguments * next;
};


static struct arguments * argsinsert = NULL;
static struct arguments * argsresult = NULL;

static struct ECPGtype ecpg_no_indicator = {ECPGt_NO_INDICATOR, 0L, {NULL}};
static struct variable no_indicator = {"no_indicator", &ecpg_no_indicator, 0, NULL};

static void
reset_variables(void)
{
    argsinsert = NULL;
    argsresult = NULL;
}


/* Add a variable to a request. */
static void
add_variable(struct arguments ** list, struct variable * var, struct variable * ind)
{
    struct arguments * p = (struct arguments *)mm_alloc(sizeof(struct arguments));
    p->variable = var;
    p->indicator = ind;
    p->next = *list;
    *list = p;
}


/* Dump out a list of all the variable on this list.
   This is a recursive function that works from the end of the list and
   deletes the list as we go on.
 */
static void
dump_variables(struct arguments * list)
{
    if (list == NULL)
    {
        return;
    }

    /* The list is build up from the beginning so lets first dump the
       end of the list:
     */

    dump_variables(list->next);

    /* Then the current element and its indicator */
    ECPGdump_a_type(yyout, list->variable->name, list->variable->type, list->indicator->name, list->indicator->type, NULL, NULL);

    /* Then release the list element. */
    free(list);
}

static void
check_indicator(struct ECPGtype *var)
{
	/* make sure this is a valid indicator variable */
	switch (var->typ)
	{
		struct ECPGrecord_member *p;

		case ECPGt_short:
		case ECPGt_int:
		case ECPGt_long:
		case ECPGt_unsigned_short:
		case ECPGt_unsigned_int:
		case ECPGt_unsigned_long:
			break;

		case ECPGt_record:
			for (p = var->u.members; p; p = p->next)
				check_indicator(p->typ);
			break;

		case ECPGt_array:
			check_indicator(var->u.element);
			break;
		default: 
			yyerror ("indicator variable must be integer type");
			break;
	}
}

static char *
cat2_str(const char *str1, const char *str2)
{ 
	char * res_str  = (char *)mm_alloc(strlen(str1) + strlen(str2) + 1);

	strcpy(res_str, str1);
	strcat(res_str, str2);
	return(res_str);
}

static char *
make2_str(const char *str1, const char *str2)
{ 
	char * res_str  = (char *)mm_alloc(strlen(str1) + strlen(str2) + 2);

	strcpy(res_str, str1);
	strcat(res_str, " ");
	strcat(res_str, str2);
	return(res_str);
}

static char *
cat3_str(const char *str1, const char *str2, const char * str3)
{    
        char * res_str  = (char *)mm_alloc(strlen(str1) + strlen(str2) + strlen(str3) + 1);
     
        strcpy(res_str, str1);
        strcat(res_str, str2);
	strcat(res_str, str3);
        return(res_str);
}    

static char *
make3_str(const char *str1, const char *str2, const char * str3)
{    
        char * res_str  = (char *)mm_alloc(strlen(str1) + strlen(str2) + strlen(str3) + 3);
     
        strcpy(res_str, str1);
	strcat(res_str, " ");
        strcat(res_str, str2);
	strcat(res_str, " ");
	strcat(res_str, str3);
        return(res_str);
}    

static char *
cat4_str(const char *str1, const char *str2, const char *str3, const char *str4)
{    
        char * res_str  = (char *)mm_alloc(strlen(str1) + strlen(str2) + strlen(str3) + strlen(str4) + 1);
     
        strcpy(res_str, str1);
        strcat(res_str, str2);
	strcat(res_str, str3);
	strcat(res_str, str4);
        return(res_str);
}

static char *
make4_str(const char *str1, const char *str2, const char *str3, const char *str4)
{    
        char * res_str  = (char *)mm_alloc(strlen(str1) + strlen(str2) + strlen(str3) + strlen(str4) + 4);
     
        strcpy(res_str, str1);
	strcat(res_str, " ");
        strcat(res_str, str2);
	strcat(res_str, " ");
	strcat(res_str, str3);
	strcat(res_str, " ");
	strcat(res_str, str4);
        return(res_str);
}

static char *
cat5_str(const char *str1, const char *str2, const char *str3, const char *str4, const char *str5)
{    
        char * res_str  = (char *)mm_alloc(strlen(str1) + strlen(str2) + strlen(str3) + strlen(str4) + strlen(str5) + 1);
     
        strcpy(res_str, str1);
        strcat(res_str, str2);
	strcat(res_str, str3);
	strcat(res_str, str4);
	strcat(res_str, str5);
        return(res_str);
}    

static char *
make5_str(const char *str1, const char *str2, const char *str3, const char *str4, const char *str5)
{    
        char * res_str  = (char *)mm_alloc(strlen(str1) + strlen(str2) + strlen(str3) + strlen(str4) + strlen(str5) + 5);
     
        strcpy(res_str, str1);
	strcat(res_str, " ");
        strcat(res_str, str2);
	strcat(res_str, " ");
	strcat(res_str, str3);
	strcat(res_str, " ");
	strcat(res_str, str4);
	strcat(res_str, " ");
	strcat(res_str, str5);
        return(res_str);
}    

static char *
make_name(void)
{
	char * name = (char *)mm_alloc(yyleng + 1);

	strncpy(name, yytext, yyleng);
	name[yyleng] = '\0';
	return(name);
}

static void
output_statement(const char * stmt)
{
	fprintf(yyout, "ECPGdo(__LINE__, \"%s\", ", stmt);

	/* dump variables to C file*/
	dump_variables(argsinsert);
	fputs("ECPGt_EOIT, ", yyout);
	dump_variables(argsresult);
	fputs("ECPGt_EORT);", yyout);
	whenever_action();
}
%}

%union {
	double                  dval;
        int                     ival;
	char *                  str;
	struct ECPGtemp_type    type;
	struct when             action;
	int			tagname;
	enum ECPGttype		type_enum;
}

/* special embedded SQL token */
%token		SQL_CONNECT SQL_CONTINUE SQL_FOUND SQL_GO SQL_GOTO
%token		SQL_IMMEDIATE SQL_INDICATOR SQL_OPEN
%token		SQL_SECTION SQL_SEMI SQL_SQLERROR SQL_SQLPRINT SQL_START
%token		SQL_STOP SQL_WHENEVER

/* C token */
%token		S_ANYTHING S_AUTO S_BOOL S_CHAR S_CONST S_DOUBLE S_EXTERN
%token		S_FLOAT S_INT
%token		S_LONG S_REGISTER S_SHORT S_SIGNED S_STATIC S_STRUCT S_UNSIGNED
%token		S_VARCHAR

/* I need this and don't know where it is defined inside the backend */
%token		TYPECAST

/* Keywords (in SQL92 reserved words) */
%token  ACTION, ADD, ALL, ALTER, AND, ANY AS, ASC,
                BEGIN_TRANS, BETWEEN, BOTH, BY,
                CASCADE, CAST, CHAR, CHARACTER, CHECK, CLOSE, COLLATE, COLUMN, COMMIT, 
                CONSTRAINT, CREATE, CROSS, CURRENT, CURRENT_DATE, CURRENT_TIME, 
                CURRENT_TIMESTAMP, CURRENT_USER, CURSOR,
                DAY_P, DECIMAL, DECLARE, DEFAULT, DELETE, DESC, DISTINCT, DOUBLE, DROP,
                END_TRANS, EXECUTE, EXISTS, EXTRACT,
                FETCH, FLOAT, FOR, FOREIGN, FROM, FULL,
                GRANT, GROUP, HAVING, HOUR_P,
                IN, INNER_P, INSERT, INTERVAL, INTO, IS,
                JOIN, KEY, LANGUAGE, LEADING, LEFT, LIKE, LOCAL,
                MATCH, MINUTE_P, MONTH_P,
                NATIONAL, NATURAL, NCHAR, NO, NOT, NOTIFY, NULL_P, NUMERIC,
                ON, OPTION, OR, ORDER, OUTER_P,
                PARTIAL, POSITION, PRECISION, PRIMARY, PRIVILEGES, PROCEDURE, PUBLIC,
                REFERENCES, REVOKE, RIGHT, ROLLBACK,
                SECOND_P, SELECT, SET, SUBSTRING,
                TABLE, TIME, TIMESTAMP, TO, TRAILING, TRANSACTION, TRIM,
                UNION, UNIQUE, UPDATE, USING,
                VALUES, VARCHAR, VARYING, VIEW,
                WHERE, WITH, WORK, YEAR_P, ZONE

/* Keywords (in SQL3 reserved words) */
%token  FALSE_P, TRIGGER, TRUE_P

/* Keywords (in SQL92 non-reserved words) */
%token  TYPE_P

/* Keywords for Postgres support (not in SQL92 reserved words) */
%token  ABORT_TRANS, AFTER, AGGREGATE, ANALYZE,
                BACKWARD, BEFORE, BINARY, CACHE, CLUSTER, COPY, CYCLE,
                DATABASE, DELIMITERS, DO, EACH, EXPLAIN, EXTEND,
                FORWARD, FUNCTION, HANDLER,
                INCREMENT, INDEX, INHERITS, INSTEAD, ISNULL,
                LANCOMPILER, LISTEN, LOAD, LOCK_P, LOCATION, MAXVALUE, MINVALUE, MOVE,
                NEW, NONE, NOTHING, NOTNULL, OIDS, OPERATOR, PROCEDURAL,
                RECIPE, RENAME, RESET, RETURNS, ROW, RULE,
                SEQUENCE, SETOF, SHOW, START, STATEMENT, STDIN, STDOUT, TRUSTED,
                VACUUM, VERBOSE, VERSION

/* Keywords (obsolete; retain through next version for parser - thomas 1997-12-0 4) */
%token  ARCHIVE

/*
 * Tokens for pg_passwd support.  The CREATEDB and CREATEUSER tokens should go a way
 * when some sort of pg_privileges relation is introduced.
 *
 *                                    Todd A. Brandys
 */
%token  USER, PASSWORD, CREATEDB, NOCREATEDB, CREATEUSER, NOCREATEUSER, VALID, UNTIL

/* Special keywords, not in the query language - see the "lex" file */
%token <str>    IDENT SCONST Op
%token <ival>   ICONST PARAM
%token <dval>   FCONST

/* these are not real. they are here so that they get generated as #define's*/
%token                  OP

/* precedence */
%left		OR
%left		AND
%right		NOT
%right		'='
%nonassoc	'<' '>'
%nonassoc	LIKE
%nonassoc	BETWEEN
%nonassoc	IN
%nonassoc	Op				/* multi-character ops and user-defined operators */
%nonassoc	NOTNULL
%nonassoc	ISNULL
%nonassoc	IS
%left		'+' '-'
%left		'*' '/'
%left		'|'				/* this is the relation union op, not logical or */
/* Unary Operators */
%right		':'
%left		';'				/* end of statement or natural log */
%right		UMINUS
%left		'.'
%left		'[' ']'
%nonassoc	TYPECAST
%nonassoc	REDUCE
%left		UNION

%type  <str>	Iconst Sconst TransactionStmt CreateStmt UserId
%type  <str>	CreateAsElement OptCreateAs CreateAsList CreateAsStmt
%type  <str>	OptArchiveType OptInherit key_reference key_action
%type  <str>    key_match constraint_expr ColLabel SpecialRuleRelation
%type  <str> 	ColId default_expr ColQualifier columnDef ColQualList
%type  <str>    ColConstraint ColConstraintElem default_list
%type  <str>    OptTableElementList OptTableElement TableConstraint
%type  <str>    ConstraintElem key_actions constraint_list TypeId
%type  <str>    res_target_list res_target_el res_target_list2
%type  <str>    res_target_el2 opt_id relation_name database_name
%type  <str>    access_method attr_name class index_name name func_name
%type  <str>    file_name recipe_name AexprConst ParamNo NumConst TypeId
%type  <str>	in_expr_nodes not_in_expr_nodes a_expr b_expr
%type  <str> 	opt_indirection expr_list extract_list extract_arg
%type  <str>	position_list position_expr substr_list substr_from
%type  <str>	trim_list in_expr substr_for not_in_expr attr attrs
%type  <str>	Typename Array Generic Numeric generic opt_float opt_numeric
%type  <str> 	opt_decimal Character character opt_varying opt_charset
%type  <str>	opt_collate Datetime datetime opt_timezone opt_interval
%type  <str>	numeric a_expr_or_null row_expr row_descriptor row_list
%type  <str>	SelectStmt union_clause select_list SubSelect result
%type  <str>	opt_table opt_union opt_unique sort_clause sortby_list
%type  <str>	sortby OptUseOp opt_inh_star relation_name_list name_list
%type  <str>	group_clause groupby_list groupby having_clause from_clause
%type  <str>	from_list from_val join_expr join_outer join_spec join_list
%type  <str> 	join_using where_clause relation_expr opt_array_bounds
%type  <str>	nest_array_bounds opt_column_list insert_rest InsertStmt
%type  <str>    columnList DeleteStmt LockStmt UpdateStmt CursorStmt
%type  <str>    NotifyStmt columnElem copy_dirn OptimizableStmt
%type  <str>    copy_delimiter ListenStmt CopyStmt copy_file_name opt_binary
%type  <str>    opt_with_copy FetchStmt opt_direction fetch_how_many opt_portal_name
%type  <str>    ClosePortalStmt DestroyStmt VacuumStmt opt_verbose
%type  <str>    opt_analyze opt_va_list va_list ExplainStmt index_params
%type  <str>    index_list func_index index_elem opt_type opt_class access_method_clause
%type  <str>    index_opt_unique IndexStmt set_opt func_return def_rest
%type  <str>    func_args_list func_args opt_with ProcedureStmt def_arg
%type  <str>    def_elem def_list definition def_name def_type DefineStmt
%type  <str>    opt_instead event event_object OptStmtMulti OptStmtBlock
%type  <str>    OptStmtList RuleStmt opt_column opt_name oper_argtypes
%type  <str>    MathOp RemoveOperStmt RemoveFuncStmt aggr_argtype
%type  <str>    RemoveAggrStmt remove_type RemoveStmt ExtendStmt RecipeStmt
%type  <str>    RemoveOperStmt RenameStmt all_Op user_valid_clause
%type  <str>    VariableSetStmt var_value zone_value VariableShowStmt
%type  <str>    VariableResetStmt AddAttrStmt alter_clause DropUserStmt
%type  <str>    user_passwd_clause user_createdb_clause
%type  <str>    user_createuser_clause user_group_list user_group_clause
%type  <str>    CreateUserStmt AlterUserStmt CreateSeqStmt OptSeqList
%type  <str>    OptSeqElem TriggerForSpec TriggerForOpt TriggerForType
%type  <str>	TriggerFuncArgs DropTrigStmt TriggerOneEvent TriggerEvents
%type  <str>    TriggerActionTime CreateTrigStmt DropPLangStmt PLangTrusted
%type  <str>    CreatePLangStmt IntegerOnly TriggerFuncArgs TriggerFuncArg
%type  <str>    ViewStmt LoadStmt CreatedbStmt opt_database location
%type  <str>    DestroydbStmt ClusterStmt grantee RevokeStmt
%type  <str>	GrantStmt privileges operation_commalist operation

%type  <str>	ECPGWhenever ECPGConnect db_name ECPGOpen open_opts
%type  <str>	indicator ECPGExecute c_expr
%type  <str>	stmt symbol

%type  <action> action

%%
prog: statements;

statements: /* empty */
	| statements statement

statement: ecpgstart stmt SQL_SEMI
	| ECPGDeclaration
	| c_anything
	| blockstart
	| blockend

stmt:  AddAttrStmt			{ output_statement($1); }
		| AlterUserStmt		{ output_statement($1); }
		| ClosePortalStmt	{ output_statement($1); }
		| CopyStmt		{ output_statement($1); }
		| CreateStmt		{ output_statement($1); }
		| CreateAsStmt		{ output_statement($1); }
		| CreateSeqStmt		{ output_statement($1); }
		| CreatePLangStmt	{ output_statement($1); }
		| CreateTrigStmt	{ output_statement($1); }
		| CreateUserStmt	{ output_statement($1); }
  		| ClusterStmt		{ output_statement($1); }
		| DefineStmt 		{ output_statement($1); }
		| DestroyStmt		{ output_statement($1); }
		| DropPLangStmt		{ output_statement($1); }
		| DropTrigStmt		{ output_statement($1); }
		| DropUserStmt		{ output_statement($1); }
		| ExtendStmt 		{ output_statement($1); }
		| ExplainStmt		{ output_statement($1); }
		| FetchStmt		{ output_statement($1); }
		| GrantStmt		{ output_statement($1); }
		| IndexStmt		{ output_statement($1); }
		| ListenStmt		{ output_statement($1); }
		| LockStmt		{ output_statement($1); }
		| ProcedureStmt		{ output_statement($1); }
 		| RecipeStmt		{ output_statement($1); }
		| RemoveAggrStmt	{ output_statement($1); }
		| RemoveOperStmt	{ output_statement($1); }
		| RemoveFuncStmt	{ output_statement($1); }
		| RemoveStmt		{ output_statement($1); }
		| RenameStmt		{ output_statement($1); }
		| RevokeStmt		{ output_statement($1); }
		| OptimizableStmt	{ /* already written out */ }
		| RuleStmt		{ output_statement($1); }
		| TransactionStmt	{
						fprintf(yyout, "ECPGtrans(__LINE__, \"%s\");", $1);
						whenever_action();
					}
		| ViewStmt		{ output_statement($1); }
		| LoadStmt		{ output_statement($1); }
		| CreatedbStmt		{ output_statement($1); }
		| DestroydbStmt		{ output_statement($1); }
		| VacuumStmt		{ output_statement($1); }
		| VariableSetStmt	{ output_statement($1); }
		| VariableShowStmt	{ output_statement($1); }
		| VariableResetStmt	{ output_statement($1); }
		| ECPGConnect		{
						fprintf(yyout, "ECPGconnect(\"%s\");", $1); 
						whenever_action();
					} 
/*		| ECPGDisconnect	*/
		| ECPGExecute		{
						fprintf(yyout, "ECPGdo(__LINE__, %s, ECPGt_EOIT, ECPGt_EORT);", $1);
						whenever_action();
					}
		| ECPGOpen		{ output_statement($1); }
		| ECPGWhenever		{
						fputs($1, yyout);
						output_line_number();
					}

/*
 * We start with a lot of stuff that's very similar to the backend's parsing
 */

/*****************************************************************************
 *
 * Create a new Postgres DBMS user
 *
 *
 *****************************************************************************/

CreateUserStmt:  CREATE USER UserId user_passwd_clause user_createdb_clause
			user_createuser_clause user_group_clause user_valid_clause
				{
					$$ = make3_str(make5_str("create user", $3, $4, $5, $6), $7, $8);
				}
		;

/*****************************************************************************
 *
 * Alter a postresql DBMS user
 *
 *
 *****************************************************************************/

AlterUserStmt:  ALTER USER UserId user_passwd_clause user_createdb_clause
			user_createuser_clause user_group_clause user_valid_clause
				{
					$$ = make3_str(make5_str("alter user", $3, $4, $5, $6), $7, $8);
				}
		;

/*****************************************************************************
 *
 * Drop a postresql DBMS user
 *
 *
 *****************************************************************************/

DropUserStmt:  DROP USER UserId
				{
					$$ = make2_str("drop user", $3);
				}
		;

user_passwd_clause:  WITH PASSWORD UserId	{ $$ = make2_str("with password", $3); }
			| /*EMPTY*/		{ $$ = ""; }
		;

user_createdb_clause:  CREATEDB
				{
					$$ = "createdb";
				}
			| NOCREATEDB
				{
					$$ = "nocreatedb";
				}
			| /*EMPTY*/		{ $$ = ""; }
		;

user_createuser_clause:  CREATEUSER
				{
					$$ = "createuser";
				}
			| NOCREATEUSER
				{
					$$ = "nocreateuser";
				}
			| /*EMPTY*/		{ $$ = NULL; }
		;

user_group_list:  user_group_list ',' UserId
				{
					$$ = make3_str($1, ",", $3);
				}
			| UserId
				{
					$$ = $1;
				}
		;

user_group_clause:  IN GROUP user_group_list	{ $$ = make2_str("in group", $3); }
			| /*EMPTY*/		{ $$ = ""; }
		;

user_valid_clause:  VALID UNTIL SCONST			{ $$ = make2_str("valid until", $3);; }
			| /*EMPTY*/			{ $$ = ""; }
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
					$$ = make4_str("set", $2, "to", $4);
				}
		| SET ColId '=' var_value
				{
					$$ = make4_str("set", $2, "=", $4);
				}
		| SET TIME ZONE zone_value
				{
					$$ = make2_str("set time zone", $4);
				}
		;

var_value:  Sconst			{ $$ = $1; }
		| DEFAULT			{ $$ = "default"; }
		;

zone_value:  Sconst			{ $$ = $1; }
		| DEFAULT			{ $$ = "default"; }
		| LOCAL				{ $$ = "local"; }
		;

VariableShowStmt:  SHOW ColId
				{
					$$ = make2_str("show", $2);
				}
		| SHOW TIME ZONE
				{
					$$ = "show time zone";
				}
		;

VariableResetStmt:	RESET ColId
				{
					$$ = make2_str("reset", $2);
				}
		| RESET TIME ZONE
				{
					$$ = "reset time zone";
				}
		;


/*****************************************************************************
 *
 *		QUERY :
 *				addattr ( attr1 = type1 .. attrn = typen ) to <relname> [*]
 *
 *****************************************************************************/

AddAttrStmt:  ALTER TABLE relation_name opt_inh_star alter_clause
				{
					$$ = make4_str("alter table", $3, $4, $5);
				}
		;

alter_clause:  ADD opt_column columnDef
				{
					$$ = make3_str("add", $2, $3);
				}
			| ADD '(' OptTableElementList ')'
				{
					$$ = cat3_str("add(", $3, ")");
				}
			| DROP opt_column ColId
				{	yyerror("ALTER TABLE/DROP COLUMN not yet implemented"); }
			| ALTER opt_column ColId SET DEFAULT default_expr
				{	yyerror("ALTER TABLE/ALTER COLUMN/SET DEFAULT not yet implemented"); }
			| ALTER opt_column ColId DROP DEFAULT
				{	yyerror("ALTER TABLE/ALTER COLUMN/DROP DEFAULT not yet implemented"); }
			| ADD ConstraintElem
				{	yyerror("ALTER TABLE/ADD CONSTRAINT not yet implemented"); }
		;

/*****************************************************************************
 *
 *		QUERY :
 *				close <optname>
 *
 *****************************************************************************/

ClosePortalStmt:  CLOSE opt_id
				{
					$$ = make2_str("close", $2);
				}
		;


/*****************************************************************************
 *
 *		QUERY :
 *				COPY [BINARY] <relname> FROM/TO
 *				[USING DELIMITERS <delimiter>]
 *
 *****************************************************************************/

CopyStmt:  COPY opt_binary relation_name opt_with_copy copy_dirn copy_file_name copy_delimiter
				{
					$$ = make3_str(make5_str("copy", $2, $3, $4, $5), $6, $7);
				}
		;

copy_dirn:	TO
				{ $$ = "to"; }
		| FROM
				{ $$ = "from"; }
		;

/*
 * copy_file_name NULL indicates stdio is used. Whether stdin or stdout is
 * used depends on the direction. (It really doesn't make sense to copy from
 * stdout. We silently correct the "typo".		 - AY 9/94
 */
copy_file_name:  Sconst					{ $$ = $1; }
		| STDIN					{ $$ = "stdin"; }
		| STDOUT				{ $$ = "stdout"; }
		;

opt_binary:  BINARY					{ $$ = "binary"; }
		| /*EMPTY*/				{ $$ = ""; }
		;

opt_with_copy:	WITH OIDS				{ $$ = "with oids"; }
		| /*EMPTY*/				{ $$ = ""; }
		;

/*
 * the default copy delimiter is tab but the user can configure it
 */
copy_delimiter:  USING DELIMITERS Sconst		{ $$ = make2_str("using delimiters", $3); }
		| /*EMPTY*/				{ $$ = ""; }
		;



/*****************************************************************************
 *
 *		QUERY :
 *				CREATE relname
 *
 *****************************************************************************/

CreateStmt:  CREATE TABLE relation_name '(' OptTableElementList ')'
				OptInherit OptArchiveType
				{
					$$ = make5_str("create table", $3,  cat3_str("(", $5, ")"), $7, $8);
				}
		;

OptTableElementList:  OptTableElementList ',' OptTableElement
				{
					$$ = make3_str($1, ",", $3);
				}
			| OptTableElement
				{
					$$ = $1;
				}
			| /*EMPTY*/	{ $$ = ""; }
		;

OptTableElement:  columnDef		{ $$ = $1; }
			| TableConstraint	{ $$ = $1; }
		;

columnDef:  ColId Typename ColQualifier
				{
					$$ = make3_str($1, $2, $3);
				}
		;

ColQualifier:  ColQualList	{ $$ = $1; }
			| /*EMPTY*/	{ $$ = ""; }
		;

ColQualList:  ColQualList ColConstraint	{ $$ = make2_str($1,$2); }
			| ColConstraint		{ $$ = $1; }
		;

ColConstraint:
		CONSTRAINT name ColConstraintElem
				{
					$$ = make3_str("constraint", $2, $3);
				}
		| ColConstraintElem
				{ $$ = $1; }
		;

ColConstraintElem:  CHECK '(' constraint_expr ')'
				{
					$$ = cat3_str("check(", $3, ")");
				}
			| DEFAULT default_expr
				{
					$$ = make2_str("default", $2);
				}
			| NOT NULL_P
				{
					$$ = "not null";
				}
			| UNIQUE
				{
					$$ = "unique";
				}
			| PRIMARY KEY
				{
					$$ = "primary key";
				}
			| REFERENCES ColId opt_column_list key_match key_actions
				{
					fprintf(stderr, "CREATE TABLE/FOREIGN KEY clause ignored; not yet implemented");
					$$ = "";
				}
		;

default_list:  default_list ',' default_expr
				{
					$$ = make3_str($1, ",", $3);
				}
			| default_expr
				{
					$$ = $1;
				}
		;

default_expr:  AexprConst
				{	$$ = $1; }
			| NULL_P
				{	$$ = "null"; }
			| '-' default_expr %prec UMINUS
				{	$$ = make2_str("-", $2); }
			| default_expr '+' default_expr
				{	$$ = make3_str($1, "+", $3); }
			| default_expr '-' default_expr
				{	$$ = make3_str($1, "-", $3); }
			| default_expr '/' default_expr
				{	$$ = make3_str($1, "/", $3); }
			| default_expr '*' default_expr
				{	$$ = make3_str($1, "*", $3); }
			| default_expr '=' default_expr
				{	yyerror("boolean expressions not supported in DEFAULT"); }
			| default_expr '<' default_expr
				{	yyerror("boolean expressions not supported in DEFAULT"); }
			| default_expr '>' default_expr
				{	yyerror("boolean expressions not supported in DEFAULT"); }
/* not possible in embedded sql 
			| ':' default_expr
				{	$$ = make2_str(":", $2); }
*/
			| ';' default_expr
				{	$$ = make2_str(";", $2); }
			| '|' default_expr
				{	$$ = make2_str("|", $2); }
			| default_expr TYPECAST Typename
				{	$$ = make3_str($1, "::", $3); }
			| CAST '(' default_expr AS Typename ')'
				{
					$$ = make3_str(cat2_str("cast(", $3) , "as", cat2_str($5, ")"));
				}
			| '(' default_expr ')'
				{	$$ = cat3_str("(", $2, ")"); }
			| func_name '(' ')'
				{	$$ = make2_str($1, "()"); }
			| func_name '(' default_list ')'
				{	$$ = make2_str($1, cat3_str("(", $3, ")")); }
			| default_expr Op default_expr
				{
					if (!strcmp("<=", $2) || !strcmp(">=", $2))
						yyerror("boolean expressions not supported in DEFAULT");
					$$ = make3_str($1, $2, $3);
				}
			| Op default_expr
				{	$$ = make2_str($1, $2); }
			| default_expr Op
				{	$$ = make2_str($1, $2); }
			/* XXX - thomas 1997-10-07 v6.2 function-specific code to be changed */
			| CURRENT_DATE
				{	$$ = "current_date"; }
			| CURRENT_TIME
				{	$$ = "current_time"; }
			| CURRENT_TIME '(' Iconst ')'
				{
					if ($3 != 0)
						fprintf(stderr, "CURRENT_TIME(%s) precision not implemented; zero used instead",$3);
					$$ = "current_time";
				}
			| CURRENT_TIMESTAMP
				{	$$ = "current_timestamp"; }
			| CURRENT_TIMESTAMP '(' Iconst ')'
				{
					if ($3 != 0)
						fprintf(stderr, "CURRENT_TIMESTAMP(%s) precision not implemented; zero used instead",$3);
					$$ = "current_timestamp";
				}
			| CURRENT_USER
				{	$$ = "current user"; }
		;

/* ConstraintElem specifies constraint syntax which is not embedded into
 *  a column definition. ColConstraintElem specifies the embedded form.
 * - thomas 1997-12-03
 */
TableConstraint:  CONSTRAINT name ConstraintElem
				{
						$$ = make3_str("constraint", $2, $3);
				}
		| ConstraintElem
				{ $$ = $1; }
		;

ConstraintElem:  CHECK '(' constraint_expr ')'
				{
					$$ = cat3_str("check(", $3, ")");
				}
		| UNIQUE '(' columnList ')'
				{
					$$ = cat3_str("unique(", $3, ")");
				}
		| PRIMARY KEY '(' columnList ')'
				{
					$$ = cat3_str("primary key(", $4, ")");
				}
		| FOREIGN KEY '(' columnList ')' REFERENCES ColId opt_column_list key_match key_actions
				{
					fprintf(stderr, "CREATE TABLE/FOREIGN KEY clause ignored; not yet implemented");
					$$ = "";
				}
		;

constraint_list:  constraint_list ',' constraint_expr
				{
					$$ = make3_str($1, ",", $3);
				}
			| constraint_expr
				{
					$$ = $1;
				}
		;

constraint_expr:  AexprConst
				{	$$ = $1; }
			| NULL_P
				{	$$ = "null"; }
			| ColId
				{
					$$ = $1;
				}
			| '-' constraint_expr %prec UMINUS
				{	$$ = make2_str("-", $2); }
			| constraint_expr '+' constraint_expr
				{	$$ = make3_str($1, "+", $3); }
			| constraint_expr '-' constraint_expr
				{	$$ = make3_str($1, "-", $3); }
			| constraint_expr '/' constraint_expr
				{	$$ = make3_str($1, "/", $3); }
			| constraint_expr '*' constraint_expr
				{	$$ = make3_str($1, "*", $3); }
			| constraint_expr '=' constraint_expr
				{	$$ = make3_str($1, "=", $3); }
			| constraint_expr '<' constraint_expr
				{	$$ = make3_str($1, "<", $3); }
			| constraint_expr '>' constraint_expr
				{	$$ = make3_str($1, ">", $3); }
/* this one doesn't work with embedded sql anyway
			| ':' constraint_expr
				{	$$ = make2_str(":", $2); }
*/
			| ';' constraint_expr
				{	$$ = make2_str(";", $2); }
			| '|' constraint_expr
				{	$$ = make2_str("|", $2); }
			| constraint_expr TYPECAST Typename
				{
					$$ = make3_str($1, "::", $3);
				}
			| CAST '(' constraint_expr AS Typename ')'
				{
					$$ = make3_str(cat2_str("cast(", $3), "as", cat2_str($5, ")")); 
				}
			| '(' constraint_expr ')'
				{	$$ = cat3_str("(", $2, ")"); }
			| func_name '(' ')'
				{
				{	$$ = make2_str($1, "()"); }
				}
			| func_name '(' constraint_list ')'
				{
					$$ = make2_str($1, cat3_str("(", $3,
")"));
				}
			| constraint_expr Op constraint_expr
				{	$$ = make3_str($1, $2, $3); }
			| constraint_expr LIKE constraint_expr
				{	$$ = make3_str($1, "like", $3); }
			| constraint_expr AND constraint_expr
				{	$$ = make3_str($1, "and", $3); }
			| constraint_expr OR constraint_expr
				{	$$ = make3_str($1, "or", $3); }
			| NOT constraint_expr
				{	$$ = make2_str("not", $2); }
			| Op constraint_expr
				{	$$ = make2_str($1, $2); }
			| constraint_expr Op
				{	$$ = make2_str($1, $2); }
			| constraint_expr ISNULL
				{	$$ = make2_str($1, "isnull"); }
			| constraint_expr IS NULL_P
				{	$$ = make2_str($1, "is null"); }
			| constraint_expr NOTNULL
				{	$$ = make2_str($1, "notnull"); }
			| constraint_expr IS NOT NULL_P
				{	$$ = make2_str($1, "is not null"); }
			| constraint_expr IS TRUE_P
				{	$$ = make2_str($1, "is true"); }
			| constraint_expr IS FALSE_P
				{	$$ = make2_str($1, "is false"); }
			| constraint_expr IS NOT TRUE_P
				{	$$ = make2_str($1, "is not true"); }
			| constraint_expr IS NOT FALSE_P
				{	$$ = make2_str($1, "is not false"); }
		;

key_match:  MATCH FULL					{ $$ = "match full"; }
		| MATCH PARTIAL					{ $$ = "match partial"; }
		| /*EMPTY*/					{ $$ = ""; }
		;

key_actions:  key_action key_action		{ $$ = make2_str($1, $2); }
		| key_action					{ $$ = $1; }
		| /*EMPTY*/					{ $$ = ""; }
		;

key_action:  ON DELETE key_reference	{ $$ = make2_str("on delete", $3); }
		| ON UPDATE key_reference		{ $$ = make2_str("on update", $3); }
		;

key_reference:  NO ACTION	{ $$ = "no action"; }
		| CASCADE	{ $$ = "cascade"; }
		| SET DEFAULT	{ $$ = "set default"; }
		| SET NULL_P	{ $$ = "set null"; }
		;

OptInherit:  INHERITS '(' relation_name_list ')' { $$ = cat3_str("inherits (", $3, ")"); }
		| /*EMPTY*/ { $$ = ""; }
		;

/*
 *	"ARCHIVE" keyword was removed in 6.3, but we keep it for now
 *  so people can upgrade with old pg_dump scripts. - momjian 1997-11-20(?)
 */
OptArchiveType:  ARCHIVE '=' NONE { $$ = "archive = none"; }
		| /*EMPTY*/	  { $$ = ""; }			
		;

CreateAsStmt:  CREATE TABLE relation_name OptCreateAs AS SubSelect
		{
			$$ = make5_str("create table", $3, $4, "as", $6); 
		}
		;

OptCreateAs:  '(' CreateAsList ')' { $$ = cat3_str("(", $2, ")"); }
			| /*EMPTY*/ { $$ = ""; }	
		;

CreateAsList:  CreateAsList ',' CreateAsElement	{ $$ = make3_str($1, ",", $3); }
			| CreateAsElement	{ $$ = $1; }
		;

CreateAsElement:  ColId { $$ = $1; }
		;

/*****************************************************************************
 *
 *		QUERY :
 *				CREATE SEQUENCE seqname
 *
 *****************************************************************************/

CreateSeqStmt:  CREATE SEQUENCE relation_name OptSeqList
				{
					$$ = make3_str("create sequence", $3, $4);
				}
		;

OptSeqList:  OptSeqList OptSeqElem
				{ $$ = make2_str($1, $2); }
			|	{ $$ = ""; }
		;

OptSeqElem:  CACHE IntegerOnly
				{
					$$ = make2_str("cache", $2);
				}
			| CYCLE
				{
					$$ = "cycle";
				}
			| INCREMENT IntegerOnly
				{
					$$ = make2_str("increment", $2);
				}
			| MAXVALUE IntegerOnly
				{
					$$ = make2_str("maxvalue", $2);
				}
			| MINVALUE IntegerOnly
				{
					$$ = make2_str("minvalue", $2);
				}
			| START IntegerOnly
				{
					$$ = make2_str("start", $2);
				}
		;

IntegerOnly:  Iconst
				{
					$$ = $1;
				}
			| '-' Iconst
				{
					$$ = make2_str("-", $2);
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
				$$ = make4_str(make5_str("create", $2, "precedural language", $5, "handler"), $7, "langcompiler", $9);
			}
		;

PLangTrusted:		TRUSTED { $$ = "trusted"; }
			|	{ $$ = ""; }

DropPLangStmt:  DROP PROCEDURAL LANGUAGE Sconst
			{
				$$ = make2_str("drop procedural language", $4);
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
					$$ = make2_str(make5_str(make5_str("create trigger", $3, $4, $5, "on"), $7, $8, "execute procedure", $11), cat3_str("(", $13, ")"));
				}
		;

TriggerActionTime:  BEFORE				{ $$ = "before"; }
			| AFTER				{ $$ = "after"; }
		;

TriggerEvents:	TriggerOneEvent
				{
					$$ = $1;
				}
			| TriggerOneEvent OR TriggerOneEvent
				{
					$$ = make3_str($1, "or", $3);
				}
			| TriggerOneEvent OR TriggerOneEvent OR TriggerOneEvent
				{
					$$ = make5_str($1, "or", $3, "or", $5);
				}
		;

TriggerOneEvent:  INSERT				{ $$ = "insert"; }
			| DELETE			{ $$ = "delete"; }
			| UPDATE			{ $$ = "update"; }
		;

TriggerForSpec:  FOR TriggerForOpt TriggerForType
				{
					$$ = make3_str("for", $2, $3);
				}
		;

TriggerForOpt:  EACH					{ $$ = "each"; }
			| /*EMPTY*/			{ $$ = ""; }
		;

TriggerForType:  ROW					{ $$ = "row"; }
			| STATEMENT			{ $$ = "statement"; }
		;

TriggerFuncArgs:  TriggerFuncArg
				{ $$ = $1 }
			| TriggerFuncArgs ',' TriggerFuncArg
				{ $$ = make3_str($1, ",", $3); }
			| /*EMPTY*/
				{ $$ = ""; }
		;

TriggerFuncArg:  Iconst
				{
					$$ = $1;
				}
			| FCONST
				{
					$$ = make_name();
				}
			| Sconst	{  $$ = $1; }
			| IDENT		{  $$ = $1; }
		;

DropTrigStmt:  DROP TRIGGER name ON relation_name
				{
					$$ = make4_str("drop trigger", $3, "on", $5);
				}
		;

/*****************************************************************************
 *
 *		QUERY :
 *				define (type,operator,aggregate)
 *
 *****************************************************************************/

DefineStmt:  CREATE def_type def_rest
				{
					$$ = make3_str("create", $2, $3);
				}
		;

def_rest:  def_name definition
				{
					$$ = make2_str($1, $2);
				}
		;

def_type:  OPERATOR		{ $$ = "operator"; }
		| TYPE_P	{ $$ = "type"; }
		| AGGREGATE	{ $$ = "aggregate"; }
		;

def_name:  PROCEDURE		{ $$ = "procedure"; }
		| JOIN		{ $$ = "join"; }
		| ColId		{ $$ = $1; }
		| MathOp	{ $$ = $1; }
		| Op		{ $$ = $1; }
		;

definition:  '(' def_list ')'				{ $$ = cat3_str("(", $2, ")"); }
		;

def_list:  def_elem					{ $$ = $1; }
		| def_list ',' def_elem			{ $$ = make3_str($1, ",", $3); }
		;

def_elem:  def_name '=' def_arg	{
					$$ = make3_str($1, "=", $3);
				}
		| def_name
				{
					$$ = $1;
				}
		| DEFAULT '=' def_arg
				{
					$$ = make2_str("default =", $3);
				}
		;

def_arg:  ColId			{  $$ = $1; }
		| all_Op	{  $$ = $1; }
		| NumConst	{  $$ = $1; /* already a Value */ }
		| Sconst	{  $$ = $1; }
		| SETOF ColId
				{
					$$ = make2_str("setof", $2);
				}
		;

/*****************************************************************************
 *
 *		QUERY:
 *				destroy <relname1> [, <relname2> .. <relnameN> ]
 *
 *****************************************************************************/

DestroyStmt:  DROP TABLE relation_name_list
				{
					$$ = make2_str("drop table", $3);
				}
		| DROP SEQUENCE relation_name_list
				{
					$$ = make2_str("drop sequence", $3);
				}
		;



/*****************************************************************************
 *
 *		QUERY:
 *			fetch/move [forward | backward] [number | all ] [ in <portalname> ]
 *
 *****************************************************************************/

FetchStmt:	FETCH opt_direction fetch_how_many opt_portal_name INTO into_list
				{
					$$ = make4_str("fetch", $2, $3, $4);
				}
		|	MOVE opt_direction fetch_how_many opt_portal_name
				{
					$$ = make4_str("fetch", $2, $3, $4);
				}
		;

opt_direction:	FORWARD		{ $$ = "forward"; }
		| BACKWARD	{ $$ = "backward"; }
		| /*EMPTY*/	{ $$ = ""; /* default */ }
		;

fetch_how_many:  Iconst
			   { $$ = $1;
				 if (atol($1) <= 0) yyerror("Please specify nonnegative count for fetch"); }
		| ALL		{ $$ = "all"; }
		| /*EMPTY*/	{ $$ = ""; /*default*/ }
		;

opt_portal_name:  IN name		{ $$ = make2_str("in", $2); }
		| name			{ $$ = make2_str("in", $1); }
		| /*EMPTY*/		{ $$ = ""; }
		;


/*****************************************************************************
 *
 *		QUERY:
 *				GRANT [privileges] ON [relation_name_list] TO [GROUP] grantee
 *
 *****************************************************************************/

GrantStmt:  GRANT privileges ON relation_name_list TO grantee opt_with_grant
				{
					$$ = make2_str(make5_str("grant", $2, "on", $4, "to"), $6);
				}
		;

privileges:  ALL PRIVILEGES
				{
				 $$ = "all privileges";
				}
		| ALL
				{
				 $$ = "all";
				}
		| operation_commalist
				{
				 $$ = $1;
				}
		;

operation_commalist:  operation
				{
						$$ = $1;
				}
		| operation_commalist ',' operation
				{
						$$ = make3_str($1, ",", $3);
				}
		;

operation:  SELECT
				{
						$$ = "select";
				}
		| INSERT
				{
						$$ = "insert";
				}
		| UPDATE
				{
						$$ = "update";
				}
		| DELETE
				{
						$$ = "delete";
				}
		| RULE
				{
						$$ = "rule";
				}
		;

grantee:  PUBLIC
				{
						$$ = "public";
				}
		| GROUP ColId
				{
						$$ = make2_str("group", $2);
				}
		| ColId
				{
						$$ = $1;
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
					$$ = make2_str(make5_str("revoke", $2, "on", $4, "from"), $6);
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
					/* should check that access_method is valid,
					   etc ... but doesn't */
					$$ = make5_str(make5_str("create", $2, "index", $4, "on"), $6, $7, cat3_str("(", $9, ")"), $11);
				}
		;

index_opt_unique:  UNIQUE	{ $$ = "unique"; }
		| /*EMPTY*/	{ $$ = ""; }
		;

access_method_clause:  USING access_method	{ $$ = make2_str("using", $2); }
		| /*EMPTY*/			{ $$ = ""; }
		;

index_params:  index_list			{ $$ = $1; }
		| func_index			{ $$ = $1; }
		;

index_list:  index_list ',' index_elem		{ $$ = make3_str($1, ",", $3); }
		| index_elem			{ $$ = $1; }
		;

func_index:  func_name '(' name_list ')' opt_type opt_class
				{
					$$ = make4_str($1, cat3_str("(", $3, ")"), $5, $6);
				}
		  ;

index_elem:  attr_name opt_type opt_class
				{
					$$ = make3_str($1, $3, $3);
				}
		;

opt_type:  ':' Typename		{ $$ = make2_str(":", $2); }
		| FOR Typename	{ $$ = make2_str("for", $2); }
		| /*EMPTY*/	{ $$ = ""; }
		;

/* opt_class "WITH class" conflicts with preceeding opt_type
 *  for Typename of "TIMESTAMP WITH TIME ZONE"
 * So, remove "WITH class" from the syntax. OK??
 * - thomas 1997-10-12
 *		| WITH class							{ $$ = $2; }
 */
opt_class:  class				{ $$ = $1; }
		| USING class			{ $$ = make2_str("using", $2); }
		| /*EMPTY*/			{ $$ = ""; }
		;

/*****************************************************************************
 *
 *		QUERY:
 *				extend index <indexname> [where <qual>]
 *
 *****************************************************************************/

ExtendStmt:  EXTEND INDEX index_name where_clause
				{
					$$ = make3_str("extend index", $3, $4);
				}
		;


/*****************************************************************************
 *
 *		QUERY:
 *				execute recipe <recipeName>
 *
 *****************************************************************************/

RecipeStmt:  EXECUTE RECIPE recipe_name
				{
					$$ = make2_str("execute recipe", $3);
				}
		;

/*****************************************************************************
 *
 *		QUERY:
 *				define function <fname>
 *					   (language = <lang>, returntype = <typename>
 *						[, arch_pct = <percentage | pre-defined>]
 *						[, disk_pct = <percentage | pre-defined>]
 *						[, byte_pct = <percentage | pre-defined>]
 *						[, perbyte_cpu = <int | pre-defined>]
 *						[, percall_cpu = <int | pre-defined>]
 *						[, iscachable])
 *						[arg is (<type-1> { , <type-n>})]
 *						as <filename or code in language as appropriate>
 *
 *****************************************************************************/

ProcedureStmt:	CREATE FUNCTION func_name func_args
			 RETURNS func_return opt_with AS Sconst LANGUAGE Sconst
				{
					$$ = make2_str(make5_str(make5_str("create function", $3, $4, "returns", $6), $7, "as", $9, "language"), $11);
				}

opt_with:  WITH definition			{ $$ = make2_str("with", $2); }
		| /*EMPTY*/			{ $$ = ""; }
		;

func_args:  '(' func_args_list ')'		{ $$ = cat3_str("(", $2, ")"); }
		| '(' ')'			{ $$ = "()"; }
		;

func_args_list:  TypeId				{ $$ = $1; }
		| func_args_list ',' TypeId
				{	$$ = make3_str($1, ",", $3); }
		;

func_return:  set_opt TypeId
				{
					$$ = make2_str($1, $2);
				}
		;

set_opt:  SETOF					{ $$ = "setof"; }
		| /*EMPTY*/			{ $$ = ""; }
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
					$$ = make3_str("drop", $2, $3);;
				}
		;

remove_type:  TYPE_P		{  $$ = "type"; }
		| INDEX		{  $$ = "index"; }
		| RULE		{  $$ = "rule"; }
		| VIEW		{  $$ = "view"; }
		;


RemoveAggrStmt:  DROP AGGREGATE name aggr_argtype
				{
						$$ = make3_str("drop aggregate", $3, $4);
				}
		;

aggr_argtype:  name			{ $$ = $1; }
		| '*'			{ $$ = "*"; }
		;


RemoveFuncStmt:  DROP FUNCTION func_name func_args
				{
						$$ = make3_str("drop function", $3, $4);
				}
		;


RemoveOperStmt:  DROP OPERATOR all_Op '(' oper_argtypes ')'
				{
					$$ = make3_str("drop operator", $3, cat3_str("(", $5, ")"));
				}
		;

all_Op:  Op | MathOp;

MathOp:	'+'				{ $$ = "+"; }
		| '-'			{ $$ = "-"; }
		| '*'			{ $$ = "*"; }
		| '/'			{ $$ = "/"; }
		| '<'			{ $$ = "<"; }
		| '>'			{ $$ = ">"; }
		| '='			{ $$ = "="; }
		;

oper_argtypes:	name
				{
				   yyerror("parser: argument type missing (use NONE for unary operators)");
				}
		| name ',' name
				{ $$ = make3_str($1, ",", $3); }
		| NONE ',' name			/* left unary */
				{ $$ = make2_str("none,", $3); }
		| name ',' NONE			/* right unary */
				{ $$ = make2_str($1, ", none"); }
		;


/*****************************************************************************
 *
 *		QUERY:
 *				rename <attrname1> in <relname> [*] to <attrname2>
 *				rename <relname1> to <relname2>
 *
 *****************************************************************************/

RenameStmt:  ALTER TABLE relation_name opt_inh_star
				  RENAME opt_column opt_name TO name
				{
					$$ = make4_str(make5_str("alter table", $3, $4, "rename", $6), $7, "to", $9);
				}
		;

opt_name:  name							{ $$ = $1; }
		| /*EMPTY*/					{ $$ = ""; }
		;

opt_column:  COLUMN					{ $$ = "colmunn"; }
		| /*EMPTY*/				{ $$ = ""; }
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
		   { QueryIsRule=1; }
		   ON event TO event_object where_clause
		   DO opt_instead OptStmtList
				{
					$$ = make2_str(make5_str(make5_str("create rule", $3, "as on", $7, "to"), $9, $10, "do", $12), $13);
				}
		;

OptStmtList:  NOTHING					{ $$ = "nothing"; }
		| OptimizableStmt			{ $$ = $1; }
		| '[' OptStmtBlock ']'			{ $$ = make3_str("[", $2, "]"); }
		;

OptStmtBlock:  OptStmtMulti
				{  $$ = $1; }
		| OptimizableStmt
				{ $$ = $1; }
		;

OptStmtMulti:  OptStmtMulti OptimizableStmt ';'
				{  $$ = make3_str($1, $2, ";"); }
		| OptStmtMulti OptimizableStmt
				{  $$ = make2_str($1, $2); }
		| OptimizableStmt ';'
				{ $$ = $1; }
		;

event_object:  relation_name '.' attr_name
				{
					$$ = make3_str($1, ",", $3);
				}
		| relation_name
				{
					$$ = $1;
				}
		;

/* change me to select, update, etc. some day */
event:	SELECT					{ $$ = "select"; }
		| UPDATE			{ $$ = "update"; }
		| DELETE			{ $$ = "delete"; }
		| INSERT			{ $$ = "insert"; }
		 ;

opt_instead:  INSTEAD					{ $$ = "instead"; }
		| /*EMPTY*/				{ $$ = ""; }
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
					$$ = make2_str("notify", $2);
				}
		;

ListenStmt:  LISTEN relation_name
				{
					$$ = make2_str("listen", $2);
                                }
;

/*****************************************************************************
 *
 *              Transactions:
 *
 *              abort transaction
 *                              (ABORT)
 *              begin transaction
 *                              (BEGIN)
 *              end transaction  
 *                              (END)
 *
 *****************************************************************************/
TransactionStmt:  ABORT_TRANS TRANSACTION	{ $$ = "rollback"; }
	| BEGIN_TRANS TRANSACTION		{ $$ = "begin transaction"; }
	| BEGIN_TRANS WORK			{ $$ = "begin transaction"; }
	| COMMIT WORK				{ $$ = "commit"; }
	| END_TRANS TRANSACTION			{ $$ = "commit"; }
	| ROLLBACK WORK				{ $$ = "rollback"; }
	| ABORT_TRANS				{ $$ = "rollback"; }
	| COMMIT				{ $$ = "commit"; }
	| ROLLBACK				{ $$ = "rollback"; }

/*****************************************************************************
 *
 *		QUERY:
 *				define view <viewname> '('target-list ')' [where <quals> ]
 *
 *****************************************************************************/

ViewStmt:  CREATE VIEW name AS SelectStmt
				{
					$$ = make4_str("create view", $3, "as", $5);
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
					$$ = make2_str("load", $2);
				}
		;


/*****************************************************************************
 *
 *		QUERY:
 *				createdb dbname
 *
 *****************************************************************************/

CreatedbStmt:  CREATE DATABASE database_name opt_database
				{
					$$ = make3_str("create database", $3, $4);
				}
		;

opt_database:  WITH LOCATION '=' location	{ $$ = make2_str("with location =", $4); }
		| /*EMPTY*/			{ $$ = ""; }
		;

location:  Sconst				{ $$ = $1; }
		| DEFAULT			{ $$ = "default"; }
		| /*EMPTY*/			{ $$ = ""; }
		;

/*****************************************************************************
 *
 *		QUERY:
 *				destroydb dbname
 *
 *****************************************************************************/

DestroydbStmt:	DROP DATABASE database_name
				{
					$$ = make2_str("drop database", $3);
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
				   $$ = make4_str("cluster", $2, "on", $4);
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
					$$ = make3_str("vacuum", $2, $3);
				}
		| VACUUM opt_verbose opt_analyze relation_name opt_va_list
				{
					if ( strlen($5) > 0 && strlen($4) == 0 )
						yyerror("parser: syntax error at or near \"(\"");
					$$ = make5_str("vacuum", $2, $3, $4, $5);
				}
		;

opt_verbose:  VERBOSE					{ $$ = "verbose"; }
		| /*EMPTY*/				{ $$ = ""; }
		;

opt_analyze:  ANALYZE					{ $$ = "analyse"; }
		| /*EMPTY*/				{ $$ = ""; }
		;

opt_va_list:  '(' va_list ')'				{ $$ = cat3_str("(", $2, ")"); }
		| /*EMPTY*/				{ $$ = ""; }
		;

va_list:  name
				{ $$=$1; }
		| va_list ',' name
				{ $$=make3_str($1, ",", $3); }
		;


/*****************************************************************************
 *
 *		QUERY:
 *				EXPLAIN query
 *
 *****************************************************************************/

ExplainStmt:  EXPLAIN opt_verbose OptimizableStmt
				{
					$$ = make3_str("explain", $2, $3);
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

OptimizableStmt:  SelectStmt { output_statement($1); }
		| CursorStmt { fputs($1, yyout); output_line_number(); }
		| UpdateStmt { output_statement($1); }
		| InsertStmt { output_statement($1); }
		| NotifyStmt { output_statement($1); }
		| DeleteStmt { output_statement($1); }
		;


/*****************************************************************************
 *
 *		QUERY:
 *				INSERT STATEMENTS
 *
 *****************************************************************************/

InsertStmt:  INSERT INTO relation_name opt_column_list insert_rest
				{
					$$ = make4_str("insert into", $3, $4, $5);
				}
		;

insert_rest:  VALUES '(' res_target_list2 ')'
				{
					$$ = cat3_str("values(", $3, ")");
				}
		| SELECT opt_unique res_target_list2
			 from_clause where_clause
			 group_clause having_clause
			 union_clause
				{
					$$ = make4_str(make5_str("select", $2, $3, $4, $5), $6, $7, $8);
				}
		;

opt_column_list:  '(' columnList ')'			{ $$ = cat3_str("(", $2, ")"); }
		| /*EMPTY*/				{ $$ = ""; }
		;

columnList:
		  columnList ',' columnElem
				{ $$ = make3_str($1, ",", $3); }
		| columnElem
				{ $$ = $1; }
		;

columnElem:  ColId opt_indirection
				{
					$$ = make2_str($1, $2);
				}
		;


/*****************************************************************************
 *
 *		QUERY:
 *				DELETE STATEMENTS
 *
 *****************************************************************************/

DeleteStmt:  DELETE FROM relation_name
			 where_clause
				{
					$$ = make3_str("delete from", $3, $4);
				}
		;

/*
 *	Total hack to just lock a table inside a transaction.
 *	Is it worth making this a separate command, with
 *	its own node type and file.  I don't think so. bjm 1998/1/22
 */
LockStmt:  LOCK_P opt_table relation_name
				{
					$$ = make3_str("lock", $2, $3);
				}
		;


/*****************************************************************************
 *
 *		QUERY:
 *				UpdateStmt (UPDATE)
 *
 *****************************************************************************/

UpdateStmt:  UPDATE relation_name
			  SET res_target_list
			  from_clause
			  where_clause
				{
					$$ = make2_str(make5_str("update", $2, "set", $4, $5), $6);
				}
		;


/*****************************************************************************
 *
 *		QUERY:
 *				CURSOR STATEMENTS
 *
 *****************************************************************************/
CursorStmt:  DECLARE name opt_binary CURSOR FOR
 			 SELECT opt_unique res_target_list2
			 from_clause where_clause
			 group_clause having_clause
			 union_clause sort_clause
				{
					struct cursor *ptr, *this = (struct cursor *) mm_alloc(sizeof(struct cursor));

					this->name = $2;
					this->command = make4_str(make5_str(make5_str("declare", $2, $3, "cursor for select", $7), $8, $9, $10, $11), $12, $13, $14);
					this->next = NULL;

					for (ptr = cur; ptr != NULL; ptr = ptr->next)
					{
						if (strcmp(this->name, ptr->name) == 0)
						{
							/* re-definition */
							free(ptr->command);
							ptr->command = this->command;
							break;
						}
					}

					if (ptr == NULL)
					{
						/* initial definition */
						this->next = cur;
						cur = this;
					}

					$$ = make5_str("/* declare cursor\"", $2, "\"statement has been moved to location of open cursor \"", $2, "\"statement. */");
				}
		;



/*****************************************************************************
 *
 *		QUERY:
 *				SELECT STATEMENTS
 *
 *****************************************************************************/

SelectStmt:  SELECT opt_unique res_target_list2
			 result from_clause where_clause
			 group_clause having_clause
			 union_clause sort_clause
				{
					$$ = make2_str(make5_str(make5_str("select", $2, $3, $4, $5), $6, $7, $8, $9), $10);
				}
		;

union_clause:  UNION opt_union select_list
				{
					$$ = make3_str("union", $2, $3);
				}
		| /*EMPTY*/
				{ $$ = ""; }
		;

select_list:  select_list UNION opt_union SubSelect
				{
					$$ = make4_str($1, "union", $3, $4);
				}
		| SubSelect
				{ $$ = $1; }
		;

SubSelect:	SELECT opt_unique res_target_list2
			 from_clause where_clause
			 group_clause having_clause
				{
					$$ = make3_str(make5_str("select", $2, $3, $4, $5), $6, $7);
				}
		;

result:  INTO opt_table relation_name			{ $$= make3_str("into", $2, $3); }
		| INTO into_list			{ $$ = ""; }
		| /*EMPTY*/				{ $$ = ""; }
		;

opt_table:  TABLE					{ $$ = "table"; }
		| /*EMPTY*/				{ $$ = ""; }
		;

opt_union:  ALL						{ $$ = "all"; }
		| /*EMPTY*/				{ $$ = ""; }
		;

opt_unique:  DISTINCT					{ $$ = "distinct"; }
		| DISTINCT ON ColId			{ $$ = make2_str("distinct on", $3); }
		| ALL					{ $$ = "all"; }
		| /*EMPTY*/				{ $$ = ""; }
		;

sort_clause:  ORDER BY sortby_list			{ $$ = make2_str("order by", $3); }
		| /*EMPTY*/				{ $$ = ""; }
		;

sortby_list:  sortby					{ $$ = $1; }
		| sortby_list ',' sortby		{ $$ = make3_str($1, ",", $3); }
		;

sortby:  ColId OptUseOp
				{
					$$ = make2_str($1, $2);
				}
		| ColId '.' ColId OptUseOp
				{
					$$ = make4_str($1, ".", $3, $4);
				}
		| Iconst OptUseOp
				{
					$$ = make2_str($1, $2);
				}
		;

OptUseOp:  USING Op				{ $$ = make2_str("using", $2); }
		| USING '<'			{ $$ = "using <"; }
		| USING '>'			{ $$ = "using >"; }
		| ASC				{ $$ = "asc"; }
		| DESC				{ $$ = "desc"; }
		| /*EMPTY*/			{ $$ = ""; }
		;

/*
 *	jimmy bell-style recursive queries aren't supported in the
 *	current system.
 *
 *	...however, recursive addattr and rename supported.  make special
 *	cases for these.
 */
opt_inh_star:  '*'					{ $$ = "*"; }
		| /*EMPTY*/				{ $$ = ""; }
		;

relation_name_list:  name_list { $$ = $1; };

name_list:  name
				{	$$ = $1; }
		| name_list ',' name
				{	$$ = make3_str($1, ",", $3); }
		;

group_clause:  GROUP BY groupby_list			{ $$ = make2_str("groub by", $3); }
		| /*EMPTY*/				{ $$ = ""; }
		;

groupby_list:  groupby					{ $$ = $1; }
		| groupby_list ',' groupby		{ $$ = make3_str($1, ",", $3); }
		;

groupby:  ColId
				{
					$$ = $1;
				}
		| ColId '.' ColId
				{
					$$ = make3_str($1, ",", $3);
				}
		| Iconst
				{
					$$ = $1;
				}
		;

having_clause:  HAVING a_expr
				{
					yyerror("HAVING clause not yet implemented");
/*					$$ = make2_str("having", $2); use this line instead to enable HAVING */
				}
		| /*EMPTY*/		{ $$ = ""; }
		;


/*****************************************************************************
 *
 *	clauses common to all Optimizable Stmts:
 *		from_clause		-
 *		where_clause	-
 *
 *****************************************************************************/

from_clause:  FROM '(' relation_expr join_expr JOIN relation_expr join_spec ')'
				{
					yyerror("JOIN not yet implemented");
				}
		| FROM from_list	{ $$ = make2_str("from", $2); }
		| /*EMPTY*/		{ $$ = ""; }
		;

from_list:	from_list ',' from_val
				{ $$ = make3_str($1, ",", $3); }
		| from_val CROSS JOIN from_val
				{ yyerror("CROSS JOIN not yet implemented"); }
		| from_val
				{ $$ = $1; }
		;

from_val:  relation_expr AS ColLabel
				{
					$$ = make3_str($1, "as", $3);
				}
		| relation_expr ColId
				{
					$$ = make2_str($1, $2);
				}
		| relation_expr
				{
					$$ = $1;
				}
		;

join_expr:  NATURAL join_expr					{ $$ = make2_str("natural", $2); }
		| FULL join_outer
				{ yyerror("FULL OUTER JOIN not yet implemented"); }
		| LEFT join_outer
				{ yyerror("LEFT OUTER JOIN not yet implemented"); }
		| RIGHT join_outer
				{ yyerror("RIGHT OUTER JOIN not yet implemented"); }
		| OUTER_P
				{ yyerror("OUTER JOIN not yet implemented"); }
		| INNER_P
				{ yyerror("INNER JOIN not yet implemented"); }
		| UNION
				{ yyerror("UNION JOIN not yet implemented"); }
		| /*EMPTY*/
				{ yyerror("INNER JOIN not yet implemented"); }
		;

join_outer:  OUTER_P				{ $$ = "outer"; }
		| /*EMPTY*/			{ $$ = "";  /* no qualifiers */ }
		;

join_spec:	ON '(' a_expr ')'			{ $$ = cat3_str("on (", $3, ")"); }
		| USING '(' join_list ')'		{ $$ = cat3_str("using (", $3, ")"); }
		| /*EMPTY*/				{ $$ = "";  /* no qualifiers */ }
		;

join_list:  join_using					{ $$ = $1; }
		| join_list ',' join_using		{ $$ = make3_str($1, ",", $3); }
		;

join_using:  ColId
				{
					$$ = $1;
				}
		| ColId '.' ColId
				{
					$$ = make3_str($1, ".", $3);
				}
		| Iconst
				{
					$$ = $1;;
				}
		;

where_clause:  WHERE a_expr			{ $$ = make2_str("where", $2); }
		| /*EMPTY*/				{ $$ = "";  /* no qualifiers */ }
		;

relation_expr:	relation_name
				{
					/* normal relations */
					$$ = $1;
				}
		| relation_name '*'				  %prec '='
				{
					/* inheritance query */
					$$ = make2_str($1, "*");
				}

opt_array_bounds:  '[' ']' nest_array_bounds
				{  $$ = make2_str("[]", $3); }
		| '[' Iconst ']' nest_array_bounds
				{  $$ = make4_str("[", $2, "]", $4); }
		| /* EMPTY */
				{  $$ = ""; }
		;

nest_array_bounds:	'[' ']' nest_array_bounds
				{  $$ = make2_str("[]", $3); }
		| '[' Iconst ']' nest_array_bounds
				{  $$ = make4_str("[", $2, "]", $4); }
		| /*EMPTY*/
				{  $$ = ""; }
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

Typename:  Array opt_array_bounds
				{
					$$ = make2_str($1, $2);
				}
		| Character	{ $$ = $1; }
		| SETOF Array
				{
					$$ = make2_str("setof", $2);
				}
		;

Array:  Generic
		| Datetime	{ $$ = $1; }
		| Numeric	{ $$ = $1; }
		;

Generic:  generic
				{
					$$ = $1;
				}
		;

generic:  IDENT					{ $$ = $1; }
		| TYPE_P			{ $$ = "type"; }
		;

/* SQL92 numeric data types
 * Check FLOAT() precision limits assuming IEEE floating types.
 * Provide rudimentary DECIMAL() and NUMERIC() implementations
 *  by checking parameters and making sure they match what is possible with INTEGER.
 * - thomas 1997-09-18
 */
Numeric:  FLOAT opt_float
				{
					$$ = make2_str("float", $2);
				}
		| DOUBLE PRECISION
				{
					$$ = "double precision";
				}
		| DECIMAL opt_decimal
				{
					$$ = make2_str("decimal", $2);
				}
		| NUMERIC opt_numeric
				{
					$$ = make2_str("numeric", $2);
				}
		;

numeric:  FLOAT
				{	$$ = "float"; }
		| DOUBLE PRECISION
				{	$$ = "double precision"; }
		| DECIMAL
				{	$$ = "decimal"; }
		| NUMERIC
				{	$$ = "numeric"; }
		;

opt_float:  '(' Iconst ')'
				{
					if (atol($2) < 1)
						yyerror("precision for FLOAT must be at least 1");
					else if (atol($2) >= 16)
						yyerror("precision for FLOAT must be less than 16");
					$$ = cat3_str("(", $2, ")");
				}
		| /*EMPTY*/
				{
					$$ = "";
				}
		;

opt_numeric:  '(' Iconst ',' Iconst ')'
				{
					if (atol($2) != 9) {
						sprintf(errortext, "NUMERIC precision %s must be 9", $2);
						yyerror(errortext);
					}
					if (atol($4) != 0) {
						sprintf(errortext, "NUMERIC scale %s must be zero", $4);
						yyerror(errortext);
					}
					$$ = make3_str(cat2_str("(", $2), ",", cat2_str($4, ")"));
				}
		| '(' Iconst ')'
				{
					if (atol($2) != 9) {
						sprintf("NUMERIC precision %s must be 9",$2);
						yyerror(errortext);
					}
					$$ = cat3_str("(", $2, ")");
				}
		| /*EMPTY*/
				{
					$$ = "";
				}
		;

opt_decimal:  '(' Iconst ',' Iconst ')'
				{
					if (atol($2) != 9) {
						sprintf(errortext, "DECIMAL precision %s exceeds implementation limit of 9", $2);
						yyerror(errortext);
					}
					if (atol($4) != 0) {
						sprintf(errortext, "DECIMAL scale %s must be zero",$4);
                                                yyerror(errortext);
                                        }
					$$ = make3_str(cat2_str("(", $2), ",", cat2_str($4, ")"));
				}
		| '(' Iconst ')'
				{
					if (atol($2) != 9) {
						sprintf(errortext, "DECIMAL precision %s exceeds implementation limit of 9",$2);
                                                yyerror(errortext);
                                        }
					$$ = cat3_str("(", $2, ")");
				}
		| /*EMPTY*/
				{
					$$ = "";
				}
		;

/* SQL92 character data types
 * The following implements CHAR() and VARCHAR().
 * We do it here instead of the 'Generic' production
 * because we don't want to allow arrays of VARCHAR().
 * I haven't thought about whether that will work or not.
 *								- ay 6/95
 */
Character:  character '(' Iconst ')'
				{
					if (strncasecmp($1, "char", strlen("char")) && strncasecmp($1, "varchar", strlen("varchar")))
						yyerror("parse error");
					if (atol($3) < 1) {
						sprintf(errortext, "length for '%s' type must be at least 1",$1);
						yyerror(errortext);
					}
					else if (atol($3) > 4096) {
						/* we can store a char() of length up to the size
						 * of a page (8KB) - page headers and friends but
						 * just to be safe here...	- ay 6/95
						 * XXX note this hardcoded limit - thomas 1997-07-13
						 */
						sprintf(errortext, "length for type '%s' cannot exceed 4096",$1);
						yyerror(errortext);
					}

					$$ = make2_str($1, cat3_str("(", $3, ")"));
				}
		| character
				{
					$$ = $1;
				}
		;

character:  CHARACTER opt_varying opt_charset opt_collate
				{
					if (strlen($4) > 0) {
						sprintf(errortext, "COLLATE %s not yet implemented",$4);
						yyerror(errortext);
					}
					$$ = make4_str("character", $2, $3, $4);
				}
		| CHAR opt_varying	{ $$ = make2_str("char", $2); }
		| VARCHAR		{ $$ = "varchar"; }
		| NATIONAL CHARACTER opt_varying { $$ = make2_str("national character", $3); }
		| NCHAR opt_varying		{ $$ = make2_str("nchar", $2); }
		;

opt_varying:  VARYING			{ $$ = "varying"; }
		| /*EMPTY*/			{ $$ = ""; }
		;

opt_charset:  CHARACTER SET ColId	{ $$ = make2_str("character set", $3); }
		| /*EMPTY*/				{ $$ = ""; }
		;

opt_collate:  COLLATE ColId		{ $$ = make2_str("collate", $2); }
		| /*EMPTY*/					{ $$ = ""; }
		;

Datetime:  datetime
				{
					$$ = $1;
				}
		| TIMESTAMP opt_timezone
				{
					$$ = make2_str("timestamp", $2);
				}
		| TIME
				{
					$$ = "time";
				}
		| INTERVAL opt_interval
				{
					$$ = make2_str("interval", $2);
				}
		;

datetime:  YEAR_P								{ $$ = "year"; }
		| MONTH_P								{ $$ = "month"; }
		| DAY_P									{ $$ = "day"; }
		| HOUR_P								{ $$ = "hour"; }
		| MINUTE_P								{ $$ = "minute"; }
		| SECOND_P								{ $$ = "second"; }
		;

opt_timezone:  WITH TIME ZONE				{ $$ = "with time zone"; }
		| /*EMPTY*/					{ $$ = ""; }
		;

opt_interval:  datetime					{ $$ = $1; }
		| YEAR_P TO MONTH_P			{ $$ = "year to #month"; }
		| DAY_P TO HOUR_P			{ $$ = "day to hour"; }
		| DAY_P TO MINUTE_P			{ $$ = "day to minute"; }
		| DAY_P TO SECOND_P			{ $$ = "day to second"; }
		| HOUR_P TO MINUTE_P			{ $$ = "hour to minute"; }
		| HOUR_P TO SECOND_P			{ $$ = "hour to second"; }
		| /*EMPTY*/					{ $$ = ""; }
		;


/*****************************************************************************
 *
 *	expression grammar, still needs some cleanup
 *
 *****************************************************************************/

a_expr_or_null:  a_expr
				{ $$ = $1; }
		| NULL_P
				{
					$$ = "null";
				}
		;

/* Expressions using row descriptors
 * Define row_descriptor to allow yacc to break the reduce/reduce conflict
 *  with singleton expressions.
 */
row_expr: '(' row_descriptor ')' IN '(' SubSelect ')'
				{
					$$ = cat5_str("(", $2, ") in (", $6, ")");
				}
		| '(' row_descriptor ')' NOT IN '(' SubSelect ')'
				{
					$$ = cat5_str("(", $2, ") not in (", $7, ")");
				}
		| '(' row_descriptor ')' Op '(' SubSelect ')'
				{
					$$ = cat3_str(cat5_str("(", $2, ")", $4, "("), $6, ")");
				}
		| '(' row_descriptor ')' '+' '(' SubSelect ')'
				{
					$$ = cat5_str("(", $2, ")+(", $6, ")");
				}
		| '(' row_descriptor ')' '-' '(' SubSelect ')'
				{
					$$ = cat5_str("(", $2, ")-(", $6, ")");
				}
		| '(' row_descriptor ')' '/' '(' SubSelect ')'
				{
					$$ = cat5_str("(", $2, ")/(", $6, ")");
				}
		| '(' row_descriptor ')' '*' '(' SubSelect ')'
				{
					$$ = cat5_str("(", $2, ")*(", $6, ")");
				}
		| '(' row_descriptor ')' '<' '(' SubSelect ')'
				{
					$$ = cat5_str("(", $2, ")<(", $6, ")");
				}
		| '(' row_descriptor ')' '>' '(' SubSelect ')'
				{
					$$ = cat5_str("(", $2, ")>(", $6, ")");
				}
		| '(' row_descriptor ')' '=' '(' SubSelect ')'
				{
					$$ = cat5_str("(", $2, ")=(", $6, ")");
				}
		| '(' row_descriptor ')' Op ANY '(' SubSelect ')'
				{
					$$ = make3_str(cat3_str("(", $2, ")"), $4, cat3_str("any(", $7, ")"));
				}
		| '(' row_descriptor ')' '+' ANY '(' SubSelect ')'
				{
					$$ = cat5_str("(", $2, ")+any(", $7, ")");
				}
		| '(' row_descriptor ')' '-' ANY '(' SubSelect ')'
				{
					$$ = cat5_str("(", $2, ")-any(", $7, ")");
				}
		| '(' row_descriptor ')' '/' ANY '(' SubSelect ')'
				{
					$$ = cat5_str("(", $2, ")/any(", $7, ")");
				}
		| '(' row_descriptor ')' '*' ANY '(' SubSelect ')'
				{
					$$ = cat5_str("(", $2, ")*any(", $7, ")");
				}
		| '(' row_descriptor ')' '<' ANY '(' SubSelect ')'
				{
					$$ = cat5_str("(", $2, ")<any(", $7, ")");
				}
		| '(' row_descriptor ')' '>' ANY '(' SubSelect ')'
				{
					$$ = cat5_str("(", $2, ")>any(", $7, ")");
				}
		| '(' row_descriptor ')' '=' ANY '(' SubSelect ')'
				{
					$$ = cat5_str("(", $2, ")=any(", $7, ")");
				}
		| '(' row_descriptor ')' Op ALL '(' SubSelect ')'
				{
					$$ = make3_str(cat3_str("(", $2, ")"), $4, cat3_str("all(", $7, ")"));
				}
		| '(' row_descriptor ')' '+' ALL '(' SubSelect ')'
				{
					$$ = cat5_str("(", $2, ")+all(", $7, ")");
				}
		| '(' row_descriptor ')' '-' ALL '(' SubSelect ')'
				{
					$$ = cat5_str("(", $2, ")-all(", $7, ")");
				}
		| '(' row_descriptor ')' '/' ALL '(' SubSelect ')'
				{
					$$ = cat5_str("(", $2, ")/all(", $7, ")");
				}
		| '(' row_descriptor ')' '*' ALL '(' SubSelect ')'
				{
					$$ = cat5_str("(", $2, ")*all(", $7, ")");
				}
		| '(' row_descriptor ')' '<' ALL '(' SubSelect ')'
				{
					$$ = cat5_str("(", $2, ")<all(", $7, ")");
				}
		| '(' row_descriptor ')' '>' ALL '(' SubSelect ')'
				{
					$$ = cat5_str("(", $2, ")>all(", $7, ")");
				}
		| '(' row_descriptor ')' '=' ALL '(' SubSelect ')'
				{
					$$ = cat5_str("(", $2, ")=all(", $7, ")");
				}
		| '(' row_descriptor ')' Op '(' row_descriptor ')'
				{
					$$ = make3_str(cat3_str("(", $2, ")"), $4, cat3_str("(", $6, ")"));
				}
		| '(' row_descriptor ')' '+' '(' row_descriptor ')'
				{
					$$ = cat5_str("(", $2, ")+(", $6, ")");
				}
		| '(' row_descriptor ')' '-' '(' row_descriptor ')'
				{
					$$ = cat5_str("(", $2, ")-(", $6, ")");
				}
		| '(' row_descriptor ')' '/' '(' row_descriptor ')'
				{
					$$ = cat5_str("(", $2, ")/(", $6, ")");
				}
		| '(' row_descriptor ')' '*' '(' row_descriptor ')'
				{
					$$ = cat5_str("(", $2, ")*(", $6, ")");
				}
		| '(' row_descriptor ')' '<' '(' row_descriptor ')'
				{
					$$ = cat5_str("(", $2, ")<(", $6, ")");
				}
		| '(' row_descriptor ')' '>' '(' row_descriptor ')'
				{
					$$ = cat5_str("(", $2, ")>(", $6, ")");
				}
		| '(' row_descriptor ')' '=' '(' row_descriptor ')'
				{
					$$ = cat5_str("(", $2, ")=(", $6, ")");
				}
		;

row_descriptor:  row_list ',' a_expr
				{
					$$ = make3_str($1, ",", $3);
				}
		;

row_list:  row_list ',' a_expr
				{
					$$ = make3_str($1, ",", $3);
				}
		| a_expr
				{
					$$ = $1;
				}
		;

/*
 * This is the heart of the expression syntax.
 * Note that the BETWEEN clause looks similar to a boolean expression
 *  and so we must define b_expr which is almost the same as a_expr
 *  but without the boolean expressions.
 * All operations are allowed in a BETWEEN clause if surrounded by parens.
 */

a_expr:  attr opt_indirection
				{
					$$ = make2_str($1, $2);
				}
		| row_expr
				{	$$ = $1;  }
		| AexprConst
				{	$$ = $1;  }
		| ColId
				{
					$$ = $1;
				}
		| '-' a_expr %prec UMINUS
				{	$$ = make2_str("-", $2); }
		| a_expr '+' a_expr
				{	$$ = make3_str($1, "+", $3); }
		| a_expr '-' a_expr
				{	$$ = make3_str($1, "-", $3); }
		| a_expr '/' a_expr
				{	$$ = make3_str($1, "/", $3); }
		| a_expr '*' a_expr
				{	$$ = make3_str($1, "*", $3); }
		| a_expr '<' a_expr
				{	$$ = make3_str($1, "<", $3); }
		| a_expr '>' a_expr
				{	$$ = make3_str($1, ">", $3); }
		| a_expr '=' a_expr
				{	$$ = make3_str($1, "=", $3); }
/* not possible in embedded sql		| ':' a_expr
				{	$$ = make2_str(":", $2); }
*/
		| ';' a_expr
				{	$$ = make2_str(";", $2); }
		| '|' a_expr
				{	$$ = make2_str("|", $2); }
		| a_expr TYPECAST Typename
				{
					$$ = make3_str($1, "::", $3);
				}
		| CAST '(' a_expr AS Typename ')'
				{
					$$ = make3_str(cat2_str("cast(", $3), "as", cat2_str($5, ")"));
				}
		| '(' a_expr_or_null ')'
				{	$$ = cat3_str("(", $2, ")"); }
		| a_expr Op a_expr
				{	$$ = make3_str($1, $2, $3);	}
		| a_expr LIKE a_expr
				{	$$ = make3_str($1, "like", $3); }
		| a_expr NOT LIKE a_expr
				{	$$ = make3_str($1, "not like", $4); }
		| Op a_expr
				{	$$ = make2_str($1, $2); }
		| a_expr Op
				{	$$ = make2_str($1, $2); }
		| func_name '(' '*' ')'
				{
					$$ = make2_str($1, "(*)"); 
				}
		| func_name '(' ')'
				{
					$$ = make2_str($1, "()"); 
				}
		| func_name '(' expr_list ')'
				{
					$$ = cat4_str($1, "(", $3, ")"); 
				}
		| CURRENT_DATE
				{
					$$ = "current_date";
				}
		| CURRENT_TIME
				{
					$$ = "current_time";
				}
		| CURRENT_TIME '(' Iconst ')'
				{
					if (atol($3) != 0)
						fprintf(stderr,"CURRENT_TIME(%s) precision not implemented; zero used instead", $3);
					$$ = "current_time";
				}
		| CURRENT_TIMESTAMP
				{
					$$ = "current_timestamp";
				}
		| CURRENT_TIMESTAMP '(' Iconst ')'
				{
					if (atol($3) != 0)
						fprintf(stderr,"CURRENT_TIMESTAMP(%s) precision not implemented; zero used instead",$3);
					$$ = "current_timestamp";
				}
		| CURRENT_USER
				{
					$$ = "current_user";
				}
		| EXISTS '(' SubSelect ')'
				{
					$$ = cat3_str("exists(", $3, ")");
				}
		| EXTRACT '(' extract_list ')'
				{
					$$ = cat3_str("extract(", $3, ")");
				}
		| POSITION '(' position_list ')'
				{
					$$ = cat3_str("position(", $3, ")");
				}
		| SUBSTRING '(' substr_list ')'
				{
					$$ = cat3_str("substring(", $3, ")");
				}
		/* various trim expressions are defined in SQL92 - thomas 1997-07-19 */
		| TRIM '(' BOTH trim_list ')'
				{
					$$ = cat3_str("trim(both", $4, ")");
				}
		| TRIM '(' LEADING trim_list ')'
				{
					$$ = cat3_str("trim(leading", $4, ")");
				}
		| TRIM '(' TRAILING trim_list ')'
				{
					$$ = cat3_str("trim(trailing", $4, ")");
				}
		| TRIM '(' trim_list ')'
				{
					$$ = cat3_str("trim(", $3, ")");
				}
		| a_expr ISNULL
				{	$$ = make2_str($1, "isnull"); }
		| a_expr IS NULL_P
				{	$$ = make2_str($1, "is null"); }
		| a_expr NOTNULL
				{	$$ = make2_str($1, "notnull"); }
		| a_expr IS NOT NULL_P
				{	$$ = make2_str($1, "is not null"); }
		/* IS TRUE, IS FALSE, etc used to be function calls
		 *  but let's make them expressions to allow the optimizer
		 *  a chance to eliminate them if a_expr is a constant string.
		 * - thomas 1997-12-22
		 */
		| a_expr IS TRUE_P
				{
				{	$$ = make2_str($1, "is true"); }
				}
		| a_expr IS NOT FALSE_P
				{
				{	$$ = make2_str($1, "is not false"); }
				}
		| a_expr IS FALSE_P
				{
				{	$$ = make2_str($1, "is false"); }
				}
		| a_expr IS NOT TRUE_P
				{
				{	$$ = make2_str($1, "is not true"); }
				}
		| a_expr BETWEEN b_expr AND b_expr
				{
					$$ = make5_str($1, "between", $3, "and", $5); 
				}
		| a_expr NOT BETWEEN b_expr AND b_expr
				{
					$$ = make5_str($1, "not between", $4, "and", $6); 
				}
		| a_expr IN '(' in_expr ')'
				{
					$$ = cat4_str($1, "in (", $4, ")"); 
				}
		| a_expr NOT IN '(' not_in_expr ')'
				{
					$$ = cat4_str($1, "not in (", $5, ")"); 
				}
		| a_expr Op '(' SubSelect ')'
				{
					$$ = make3_str($1, $2, cat3_str("(", $4, ")")); 
				}
		| a_expr '+' '(' SubSelect ')'
				{
					$$ = cat4_str($1, "+(", $4, ")"); 
				}
		| a_expr '-' '(' SubSelect ')'
				{
					$$ = cat4_str($1, "-(", $4, ")"); 
				}
		| a_expr '/' '(' SubSelect ')'
				{
					$$ = cat4_str($1, "/(", $4, ")"); 
				}
		| a_expr '*' '(' SubSelect ')'
				{
					$$ = cat4_str($1, "*(", $4, ")"); 
				}
		| a_expr '<' '(' SubSelect ')'
				{
					$$ = cat4_str($1, "<(", $4, ")"); 
				}
		| a_expr '>' '(' SubSelect ')'
				{
					$$ = cat4_str($1, ">(", $4, ")"); 
				}
		| a_expr '=' '(' SubSelect ')'
				{
					$$ = cat4_str($1, "=(", $4, ")"); 
				}
		| a_expr Op ANY '(' SubSelect ')'
				{
					$$ = make3_str($1, $2, cat3_str("any(", $5, ")")); 
				}
		| a_expr '+' ANY '(' SubSelect ')'
				{
					$$ = cat4_str($1, "+any(", $5, ")"); 
				}
		| a_expr '-' ANY '(' SubSelect ')'
				{
					$$ = cat4_str($1, "-any(", $5, ")"); 
				}
		| a_expr '/' ANY '(' SubSelect ')'
				{
					$$ = cat4_str($1, "/any(", $5, ")"); 
				}
		| a_expr '*' ANY '(' SubSelect ')'
				{
					$$ = cat4_str($1, "*any(", $5, ")"); 
				}
		| a_expr '<' ANY '(' SubSelect ')'
				{
					$$ = cat4_str($1, "<any(", $5, ")"); 
				}
		| a_expr '>' ANY '(' SubSelect ')'
				{
					$$ = cat4_str($1, ">any(", $5, ")"); 
				}
		| a_expr '=' ANY '(' SubSelect ')'
				{
					$$ = cat4_str($1, "=any(", $5, ")"); 
				}
		| a_expr Op ALL '(' SubSelect ')'
				{
					$$ = make3_str($1, $2, cat3_str("all (", $5, ")")); 
				}
		| a_expr '+' ALL '(' SubSelect ')'
				{
					$$ = cat4_str($1, "+all(", $5, ")"); 
				}
		| a_expr '-' ALL '(' SubSelect ')'
				{
					$$ = cat4_str($1, "-all(", $5, ")"); 
				}
		| a_expr '/' ALL '(' SubSelect ')'
				{
					$$ = cat4_str($1, "/all(", $5, ")"); 
				}
		| a_expr '*' ALL '(' SubSelect ')'
				{
					$$ = cat4_str($1, "*all(", $5, ")"); 
				}
		| a_expr '<' ALL '(' SubSelect ')'
				{
					$$ = cat4_str($1, "<all(", $5, ")"); 
				}
		| a_expr '>' ALL '(' SubSelect ')'
				{
					$$ = cat4_str($1, ">all(", $5, ")"); 
				}
		| a_expr '=' ALL '(' SubSelect ')'
				{
					$$ = cat4_str($1, "=all(", $5, ")"); 
				}
		| a_expr AND a_expr
				{	$$ = make3_str($1, "and", $3); }
		| a_expr OR a_expr
				{	$$ = make3_str($1, "or", $3); }
		| NOT a_expr
				{	$$ = make2_str("not", $2); }
		| cinputvariable
			        { $$ = ";;"; }
		;

/*
 * b_expr is a subset of the complete expression syntax
 *  defined by a_expr. b_expr is used in BETWEEN clauses
 *  to eliminate parser ambiguities stemming from the AND keyword.
 */

b_expr:  attr opt_indirection
				{
					$$ = make2_str($1, $2);
				}
		| AexprConst
				{	$$ = $1;  }
		| ColId
				{
					$$ = $1;
				}
		| '-' b_expr %prec UMINUS
				{	$$ = make2_str("-", $2); }
		| b_expr '+' b_expr
				{	$$ = make3_str($1, "+", $3); }
		| b_expr '-' b_expr
				{	$$ = make3_str($1, "-", $3); }
		| b_expr '/' b_expr
				{	$$ = make3_str($1, "/", $3); }
		| b_expr '*' b_expr
				{	$$ = make3_str($1, "*", $3); }
/* not possible in embedded sql		| ':' b_expr
				{	$$ = make2_str(":", $2); }
*/
		| ';' b_expr
				{	$$ = make2_str(";", $2); }
		| '|' b_expr
				{	$$ = make2_str("|", $2); }
		| b_expr TYPECAST Typename
				{
					$$ = make3_str($1, "::", $3);
				}
		| CAST '(' b_expr AS Typename ')'
				{
					$$ = make3_str(cat2_str("cast(", $3), "as", cat2_str($5, ")"));
				}
		| '(' a_expr ')'
				{	$$ = cat3_str("(", $2, ")"); }
		| b_expr Op b_expr
				{	$$ = make3_str($1, $2, $3);	}
		| Op b_expr
				{	$$ = make2_str($1, $2); }
		| b_expr Op
				{	$$ = make2_str($1, $2); }
		| func_name '(' ')'
				{
					$$ = make2_str($1, "()"); 
				}
		| func_name '(' expr_list ')'
				{
					$$ = cat4_str($1, "(", $3, ")"); 
				}
		| CURRENT_DATE
				{
					$$ = "current_date";
				}
		| CURRENT_TIME
				{
					$$ = "current_time";
				}
		| CURRENT_TIME '(' Iconst ')'
				{
					if ($3 != 0)
						fprintf(stderr,"CURRENT_TIME(%s) precision not implemented; zero used instead", $3);
					$$ = "current_time";
				}
		| CURRENT_TIMESTAMP
				{
					$$ = "current_timestamp";
				}
		| CURRENT_TIMESTAMP '(' Iconst ')'
				{
					if (atol($3) != 0)
						fprintf(stderr,"CURRENT_TIMESTAMP(%s) precision not implemented; zero used instead",$3);
					$$ = "current_timestamp";
				}
		| CURRENT_USER
				{
					$$ = "current_user";
				}
		| POSITION '(' position_list ')'
				{
					$$ = cat3_str("position (", $3, ")");
				}
		| SUBSTRING '(' substr_list ')'
				{
					$$ = cat3_str("substring (", $3, ")");
				}
		/* various trim expressions are defined in SQL92 - thomas 1997-07-19 */
		| TRIM '(' BOTH trim_list ')'
				{
					$$ = cat3_str("trim(both", $4, ")");
				}
		| TRIM '(' LEADING trim_list ')'
				{
					$$ = cat3_str("trim(leading", $4, ")");
				}
		| TRIM '(' TRAILING trim_list ')'
				{
					$$ = cat3_str("trim(trailing", $4, ")");
				}
		| TRIM '(' trim_list ')'
				{
					$$ = cat3_str("trim(", $3, ")");
				}
		| civariableonly
			        { $$ = ";;"; }
		;

opt_indirection:  '[' c_expr ']' opt_indirection
				{
					$$ = make4_str("[", $2, "]", $4);
				}
		| '[' c_expr ':' c_expr ']' opt_indirection
				{
					$$ = make2_str(make5_str("[", $2, ":", $4, "]"), $6);
				}
		| /* EMPTY */
				{	$$ = ""; }
		;

expr_list:  a_expr_or_null
				{ $$ = $1; }
		| expr_list ',' a_expr_or_null
				{ $$ = make3_str($1, ",", $3); }
		| expr_list USING a_expr
				{ $$ = make3_str($1, "using", $3); }
		;

extract_list:  extract_arg FROM a_expr
				{
					$$ = make3_str($1, "from", $3);
				}
		| /* EMPTY */
				{	$$ = ""; }
		| cinputvariable
			        { $$ = ";;"; }
		;

/* Add in TIMEZONE_HOUR and TIMEZONE_MINUTE for SQL92 compliance
 *  for next release. Just set up extract_arg for now...
 * - thomas 1998-04-08
 */
extract_arg:  datetime
				{	$$ = $1; }
		;

position_list:  position_expr IN position_expr
				{	$$ = make3_str($1, "in", $3); }
		| /* EMPTY */
				{	$$ = ""; }
		;

position_expr:  attr opt_indirection
				{
					$$ = make2_str($1, $2);
				}
		| AexprConst
				{	$$ = $1;  }
		| '-' position_expr %prec UMINUS
				{	$$ = make2_str("-", $2); }
		| position_expr '+' position_expr
				{	$$ = make3_str($1, "+", $3); }
		| position_expr '-' position_expr
				{	$$ = make3_str($1, "-", $3); }
		| position_expr '/' position_expr
				{	$$ = make3_str($1, "/", $3); }
		| position_expr '*' position_expr
				{	$$ = make3_str($1, "*", $3); }
		| '|' position_expr
				{	$$ = make2_str("|", $2); }
		| position_expr TYPECAST Typename
				{
					$$ = make3_str($1, "::", $3);
				}
		| CAST '(' position_expr AS Typename ')'
				{
					$$ = make3_str(cat2_str("cast(", $3), "as", cat2_str($5, ")"));
				}
		| '(' position_expr ')'
				{	$$ = cat3_str("(", $2, ")"); }
		| position_expr Op position_expr
				{	$$ = make3_str($1, $2, $3); }
		| Op position_expr
				{	$$ = make2_str($1, $2); }
		| position_expr Op
				{	$$ = make2_str($1, $2); }
		| ColId
				{
					$$ = $1;
				}
		| func_name '(' ')'
				{
					$$ = make2_str($1, "()");
				}
		| func_name '(' expr_list ')'
				{
					$$ = cat4_str($1, "(", $3, ")");
				}
		| POSITION '(' position_list ')'
				{
					$$ = cat3_str("position(", $3, ")");
				}
		| SUBSTRING '(' substr_list ')'
				{
					$$ = cat3_str("substring(", $3, ")");
				}
		/* various trim expressions are defined in SQL92 - thomas 1997-07-19 */
		| TRIM '(' BOTH trim_list ')'
				{
					$$ = cat3_str("trim(both", $4, ")");
				}
		| TRIM '(' LEADING trim_list ')'
				{
					$$ = cat3_str("trim(leading", $4, ")");
				}
		| TRIM '(' TRAILING trim_list ')'
				{
					$$ = cat3_str("trim(trailing", $4, ")");
				}
		| TRIM '(' trim_list ')'
				{
					$$ = cat3_str("trim(", $3, ")");
				}
		;

substr_list:  expr_list substr_from substr_for
				{
					$$ = make3_str($1, $2, $3);
				}
		| /* EMPTY */
				{	$$ = ""; }
		;

substr_from:  FROM expr_list
				{	$$ = make2_str("from", $2); }
		| /* EMPTY */
				{
					$$ = "";
				}
		;

substr_for:  FOR expr_list
				{	$$ = make2_str("for", $2); }
		| /* EMPTY */
				{	$$ = ""; }
		;

trim_list:  a_expr FROM expr_list
				{ $$ = make3_str($1, "from", $3); }
		| FROM expr_list
				{ $$ = make2_str("from", $2); }
		| expr_list
				{ $$ = $1; }
		;

in_expr:  SubSelect
				{
					$$ = $1;
				}
		| in_expr_nodes
				{	$$ = $1; }
		;

in_expr_nodes:  AexprConst
				{	$$ = $1; }
		| in_expr_nodes ',' AexprConst
				{	$$ = make3_str($1, ",", $3);}
		;

not_in_expr:  SubSelect
				{
					$$ = $1; 
				}
		| not_in_expr_nodes
				{	$$ = $1; }
		;

not_in_expr_nodes:  AexprConst
				{	$$ = $1; }
		| not_in_expr_nodes ',' AexprConst
				{	$$ = make3_str($1, ",", $3);}
		;

attr:  relation_name '.' attrs
				{
					$$ = make3_str($1, ".", $3);
				}
		| ParamNo '.' attrs
				{
					$$ = make3_str($1, ".", $3);
				}
		;

attrs:	  attr_name
				{ $$ = $1; }
		| attrs '.' attr_name
				{ $$ = make3_str($1, ".", $3); }
		| attrs '.' '*'
				{ $$ = make2_str($1, ".*"); }
		;


/*****************************************************************************
 *
 *	target lists
 *
 *****************************************************************************/

res_target_list:  res_target_list ',' res_target_el
				{	$$ = make3_str($1, ",",$3);  }
		| res_target_el
				{	$$ = $1;  }
		| '*'		{ $$ = "*"; }
		;

res_target_el:  ColId opt_indirection '=' a_expr_or_null
				{
					$$ = make4_str($1, $2, "=", $4);
				}
		| attr opt_indirection
				{
					$$ = make2_str($1, $2);
				}
		| relation_name '.' '*'
				{
					$$ = make2_str($1, ".*");
				}
		;

/*
** target list for select.
** should get rid of the other but is still needed by the defunct select into
** and update (uses a subset)
*/
res_target_list2:  res_target_list2 ',' res_target_el2
				{	$$ = make3_str($1, ",", $3);  }
		| res_target_el2
				{	$$ = $1;  }
		;

/* AS is not optional because shift/red conflict with unary ops */
res_target_el2:  a_expr_or_null AS ColLabel
				{
					$$ = make3_str($1, "as", $3);
				}
		| a_expr_or_null
				{
					$$ = $1;
				}
		| relation_name '.' '*'
				{
					$$ = make2_str($1, ".*");
				}
		| '*'
				{
					$$ = "*";
				}
		;

opt_id:  ColId									{ $$ = $1; }
		| /* EMPTY */							{ $$ = ""; }
		;

relation_name:	SpecialRuleRelation
				{
					$$ = $1;
				}
		| ColId
				{
					/* disallow refs to variable system tables */
					if (strcmp(LogRelationName, $1) == 0
					   || strcmp(VariableRelationName, $1) == 0) {
						sprintf(errortext, "%s cannot be accessed by users",$1);
						yyerror(errortext);
					}
					else
						$$ = $1;
				}
		;

database_name:			ColId			{ $$ = $1; };
access_method:			IDENT			{ $$ = $1; };
attr_name:				ColId			{ $$ = $1; };
class:					IDENT			{ $$ = $1; };
index_name:				ColId			{ $$ = $1; };

/* Functions
 * Include date/time keywords as SQL92 extension.
 * Include TYPE as a SQL92 unreserved keyword. - thomas 1997-10-05
 */
name:					ColId			{ $$ = $1; };
func_name:				ColId			{ $$ = $1; };

file_name:				Sconst			{ $$ = $1; };
recipe_name:			IDENT			{ $$ = $1; };

/* Constants
 * Include TRUE/FALSE for SQL3 support. - thomas 1997-10-24
 */
AexprConst:  Iconst
				{
					$$ = $1;
				}
		| FCONST
				{
					$$ = make_name();
				}
		| Sconst
				{
					$$ = $1;
				}
		| Typename Sconst
				{
					$$ = make2_str($1, $2);
				}
		| ParamNo
				{	$$ = $1;  }
		| TRUE_P
				{
					$$ = "true";
				}
		| FALSE_P
				{
					$$ = "false";
				}
		;

ParamNo:  PARAM
				{
					$$ = make_name();
				}
		;

NumConst:  Iconst						{ $$ = $1; }
		| FCONST						{ $$ = make_name(); }
		;

Iconst:  ICONST                                 { $$ = make_name();};
Sconst:  SCONST                                 {
							$$ = (char *)mm_alloc(strlen($1) + 3);
							$$[0]='\'';
				     		        strcpy($$+1, $1);
							$$[strlen($1)+2]='\0';
							$$[strlen($1)+1]='\'';
						}
UserId:  IDENT                                  { $$ = $1;};

/* Column and type identifier
 * Does not include explicit datetime types
 *  since these must be decoupled in Typename syntax.
 * Use ColId for most identifiers. - thomas 1997-10-21
 */
TypeId:  ColId
			{	$$ = $1; }
		| numeric
			{	$$ = $1; }
		| character
			{	$$ = $1; }
		;
/* Column identifier
 * Include date/time keywords as SQL92 extension.
 * Include TYPE as a SQL92 unreserved keyword. - thomas 1997-10-05
 * Add other keywords. Note that as the syntax expands,
 *  some of these keywords will have to be removed from this
 *  list due to shift/reduce conflicts in yacc. If so, move
 *  down to the ColLabel entity. - thomas 1997-11-06
 */
ColId:  IDENT							{ $$ = $1; }
		| datetime						{ $$ = $1; }
		| ACTION						{ $$ = "action"; }
		| CACHE							{ $$ = "cache"; }
		| CYCLE							{ $$ = "cycle"; }
		| DATABASE						{ $$ = "database"; }
		| DELIMITERS					{ $$ = "delimiters"; }
		| DOUBLE						{ $$ = "double"; }
		| EACH							{ $$ = "each"; }
		| FUNCTION						{ $$ = "function"; }
		| INCREMENT						{ $$ = "increment"; }
		| INDEX							{ $$ = "index"; }
		| KEY							{ $$ = "key"; }
		| LANGUAGE						{ $$ = "language"; }
		| LOCATION						{ $$ = "location"; }
		| MATCH							{ $$ = "match"; }
		| MAXVALUE						{ $$ = "maxvalue"; }
		| MINVALUE						{ $$ = "minvalue"; }
		| OPERATOR						{ $$ = "operator"; }
		| OPTION						{ $$ = "option"; }
		| PASSWORD						{ $$ = "password"; }
		| PRIVILEGES					{ $$ = "privileges"; }
		| RECIPE						{ $$ = "recipe"; }
		| ROW							{ $$ = "row"; }
		| START							{ $$ = "start"; }
		| STATEMENT						{ $$ = "statement"; }
		| TIME							{ $$ = "time"; }
		| TRIGGER						{ $$ = "trigger"; }
		| TYPE_P						{ $$ = "type"; }
		| USER							{ $$ = "user"; }
		| VALID							{ $$ = "valid"; }
		| VERSION						{ $$ = "version"; }
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
		| ARCHIVE						{ $$ = "archive"; }
		| CLUSTER						{ $$ = "cluster"; }
		| CONSTRAINT					{ $$ = "constraint"; }
		| CROSS							{ $$ = "cross"; }
		| FOREIGN						{ $$ = "foreign"; }
		| GROUP							{ $$ = "group"; }
		| LOAD							{ $$ = "load"; }
		| ORDER							{ $$ = "order"; }
		| POSITION						{ $$ = "position"; }
		| PRECISION						{ $$ = "precision"; }
		| TABLE							{ $$ = "table"; }
		| TRANSACTION					{ $$ = "transaction"; }
		| TRUE_P						{ $$ = "true"; }
		| FALSE_P						{ $$ = "false"; }
		;

SpecialRuleRelation:  CURRENT
				{
					if (QueryIsRule)
						$$ = "current";
					else
						yyerror("CURRENT used in non-rule query");
				}
		| NEW
				{
					if (QueryIsRule)
						$$ = "new";
					else
						yyerror("NEW used in non-rule query");
				}
		;

/*
 * and now special embedded SQL stuff
 */

/*
 * variable declaration inside the exec sql declare block
 */
ECPGDeclaration: sql_startdeclare variable_declarations sql_enddeclare {}

sql_startdeclare : ecpgstart BEGIN_TRANS DECLARE SQL_SECTION SQL_SEMI {
	fputs("/* exec sql begin declare section */\n", yyout);
	output_line_number();
 }

sql_enddeclare: ecpgstart END_TRANS DECLARE SQL_SECTION SQL_SEMI {
    fputs("/* exec sql end declare section */\n", yyout); 
    output_line_number();
}

variable_declarations : /* empty */
                      | variable_declarations variable_declaration;

/* Here is where we can enter support for typedef. */
variable_declaration: type initializer ';'     { 
    /* don't worry about our list when we're working on a struct */
    if (struct_level == 0)
    {
        new_variable($<type>1.name, $<type>1.typ);
        free((void *)$<type>1.name);
    }
    fputs(";", yyout); 
}

initializer : /*empty */
            | '=' {fwrite(yytext, yyleng, 1, yyout);} vartext;

type : maybe_storage_clause type_detailed { $<type>$ = $<type>2; };
type_detailed : varchar_type { $<type>$ = $<type>1; }
	      | simple_type { $<type>$ = $<type>1; }
	      | string_type { $<type>$ = $<type>1; }
/*	      | array_type {$<type>$ = $<type>1; }
	      | pointer_type {$<type>$ = $<type>1; }*/
	      | struct_type {$<type>$ = $<type>1; };

varchar_type : varchar_tag symbol index {
    if ($<ival>3 > 0L)
	fprintf(yyout, "struct varchar_%s { int len; char arr[%d]; } %s", $2, $<ival>3, $2);
    else
	fprintf(yyout, "struct varchar_%s { int len; char arr[]; } %s", $2, $2);
    if (struct_level == 0)
    {
	$<type>$.name = $2;
	$<type>$.typ = ECPGmake_varchar_type(ECPGt_varchar, $<ival>3);
    }
    else
	ECPGmake_record_member($2, ECPGmake_varchar_type(ECPGt_varchar, $<ival>3), &(record_member_list[struct_level-1]));
}

varchar_tag: S_VARCHAR /*| S_VARCHAR2 */;

simple_type : simple_tag symbol {
    fprintf(yyout, "%s %s", ECPGtype_name($<type_enum>1), $2);
    if (struct_level == 0)
    {
	$<type>$.name = $2;
	$<type>$.typ = ECPGmake_simple_type($<type_enum>1, 1);
    }
    else
        ECPGmake_record_member($2, ECPGmake_simple_type($<type_enum>1, 1), &(record_member_list[struct_level-1]));
}

string_type : char_tag symbol index {
    if ($<ival>3 > 0L)
	    fprintf(yyout, "%s %s [%d]", ECPGtype_name($<type_enum>1), $2, $<ival>3);
    else
	    fprintf(yyout, "%s %s []", ECPGtype_name($<type_enum>1), $2);
    if (struct_level == 0)
    {
	$<type>$.name = $2;
	$<type>$.typ = ECPGmake_simple_type($<type_enum>1, $<ival>3);
    }
    else
	ECPGmake_record_member($2, ECPGmake_simple_type($<type_enum>1, $<ival>3), &(record_member_list[struct_level-1]));
}
	|	char_tag '*' symbol {
    fprintf(yyout, "%s *%s", ECPGtype_name($<type_enum>1), $3);
    if (struct_level == 0)
    {
	$<type>$.name = $3;
	$<type>$.typ = ECPGmake_simple_type($<type_enum>1, 0);
    }
    else
	ECPGmake_record_member($3, ECPGmake_simple_type($<type_enum>1, 0), &(record_member_list[struct_level-1]));
}
	|	char_tag symbol {
    fprintf(yyout, "%s %s", ECPGtype_name($<type_enum>1), $2);
    if (struct_level == 0)
    {
	$<type>$.name = $2;
	$<type>$.typ = ECPGmake_simple_type($<type_enum>1, 1);
    }
    else
        ECPGmake_record_member($2, ECPGmake_simple_type($<type_enum>1, 1), &(record_member_list[struct_level-1]));
}

char_tag : S_CHAR { $<type_enum>$ = ECPGt_char; }
           | S_UNSIGNED S_CHAR { $<type_enum>$ = ECPGt_unsigned_char; }

/*
array_type : simple_tag symbol index {
    if ($<ival>3 > 0)
	    fprintf(yyout, "%s %s [%ld]", ECPGtype_name($<type_enum>1), $2, $<ival>3);
    else
	    fprintf(yyout, "%s %s []", ECPGtype_name($<type_enum>1), $2);
    if (struct_level == 0)
    {
	$<type>$.name = $2;
	$<type>$.typ = ECPGmake_array_type(ECPGmake_simple_type($<type_enum>1), $<ival>3);
    }
    else
	ECPGmake_record_member($2, ECPGmake_array_type(ECPGmake_simple_type($<type_enum>1), $<ival>3), &(record_member_list[struct_level-1]));
}

pointer_type : simple_tag '*' symbol {
    fprintf(yyout, "%s * %s", ECPGtype_name($<type_enum>1), $3);
    if (struct_level == 0)
    {
	$<type>$.name = $3;
	$<type>$.typ = ECPGmake_array_type(ECPGmake_simple_type($<type_enum>1), 0);
    }
    else
	ECPGmake_record_member($3, ECPGmake_array_type(ECPGmake_simple_type($<type_enum>1), 0), &(record_member_list[struct_level-1]));
}
*/

s_struct : S_STRUCT symbol {
    struct_level++;
    fprintf(yyout, "struct %s {", $2);
}

struct_type : s_struct '{' variable_declarations '}' symbol {
    struct_level--;
    if (struct_level == 0)
    {
	$<type>$.name = $5;
	$<type>$.typ = ECPGmake_record_type(record_member_list[struct_level]);
    }
    else
	ECPGmake_record_member($5, ECPGmake_record_type(record_member_list[struct_level]), &(record_member_list[struct_level-1])); 
    fprintf(yyout, "} %s", $5);
    record_member_list[struct_level] = NULL;
}

simple_tag : S_SHORT { $<type_enum>$ = ECPGt_short; }
           | S_UNSIGNED S_SHORT { $<type_enum>$ = ECPGt_unsigned_short; }
	   | S_INT { $<type_enum>$ = ECPGt_int; }
           | S_UNSIGNED S_INT { $<type_enum>$ = ECPGt_unsigned_int; }
	   | S_LONG { $<type_enum>$ = ECPGt_long; }
           | S_UNSIGNED S_LONG { $<type_enum>$ = ECPGt_unsigned_long; }
           | S_FLOAT { $<type_enum>$ = ECPGt_float; }
           | S_DOUBLE { $<type_enum>$ = ECPGt_double; }
	   | S_BOOL { $<type_enum>$ = ECPGt_bool; };

maybe_storage_clause : S_EXTERN { fwrite(yytext, yyleng, 1, yyout); }
		       | S_STATIC { fwrite(yytext, yyleng, 1, yyout); }
		       | S_SIGNED { fwrite(yytext, yyleng, 1, yyout); }
		       | S_CONST { fwrite(yytext, yyleng, 1, yyout); }
		       | S_REGISTER { fwrite(yytext, yyleng, 1, yyout); }
		       | S_AUTO { fwrite(yytext, yyleng, 1, yyout); }
                       | /* empty */ { };
  	 
index : '[' Iconst ']' { $<ival>$ = atol($2); }
	| '[' ']' { $<ival>$ = 0L; }

/*
 * the exec sql connect statement: connect to the given database 
 */
ECPGConnect : SQL_CONNECT db_name { $$ = $2; }

db_name : database_name { $$ = $1; }
	| ':' name { /* check if we have a char variable */
			struct variable *p = find_variable($2);
			enum ECPGttype typ = p->type->typ;

			/* if array see what's inside */
			if (typ == ECPGt_array)
				typ = p->type->u.element->typ;

			if (typ != ECPGt_char && typ != ECPGt_unsigned_char)
				yyerror("invalid datatype");
			$$ = $2;
	}

/*
 * execute a given string as sql command
 */
ECPGExecute : EXECUTE SQL_IMMEDIATE  ':' name { $$ = $4; };

/*
 * open is an open cursor, at the moment this has to be removed
 */
ECPGOpen: SQL_OPEN name open_opts {
		struct cursor *ptr;

		for (ptr = cur; ptr != NULL; ptr=ptr->next)
		{
			if (strcmp(ptr->name, $2) == 0)
			{
				$$ = ptr->command;
				break;
			}
		}

		if (ptr == NULL)
		{
			sprintf(errortext, "unknown cursor %s opened", $2);
			yyerror(errortext);
		}
};

open_opts: /* empty */		{ $$ = ""; }
	| USING ':' name	{
					yyerror ("open cursor with variables not implemented yet");
				}

/*
 * whenever statement: decide what to do in case of error/no dat
 */
ECPGWhenever: SQL_WHENEVER SQL_SQLERROR action {
	when_error.code = $<action>3.code;
	when_error.command = $<action>3.command;
	$$ = make3_str("/* exec sql whenever sqlerror ", $3.str, "; */\n");
}
	| SQL_WHENEVER NOT SQL_FOUND action {
	when_nf.code = $<action>4.code;
	when_nf.command = $<action>4.command;
	$$ = make3_str("/* exec sql whenever not found ", $4.str, "; */\n");
}

action : SQL_CONTINUE {
	$<action>$.code = W_NOTHING;
	$<action>$.command = NULL;
	$<action>$.str = "continue";
}
       | SQL_SQLPRINT {
	$<action>$.code = W_SQLPRINT;
	$<action>$.command = NULL;
	$<action>$.str = "sqlprint";
}
       | SQL_STOP {
	$<action>$.code = W_STOP;
	$<action>$.command = NULL;
	$<action>$.str = "stop";
}
       | SQL_GOTO name {
        $<action>$.code = W_GOTO;
        $<action>$.command = $2;
	$<action>$.str = make2_str("goto ", $2);
}
       | SQL_GO TO name {
        $<action>$.code = W_GOTO;
        $<action>$.command = $3;
	$<action>$.str = make2_str("goto ", $3);
}
       | DO name '(' {
	do_str = (char *) mm_alloc(do_length = strlen($2) + 4);
	sprintf(do_str, "%s (", $2);
} dotext ')' {
	do_str[strlen(do_str)+1]='\0';
	do_str[strlen(do_str)]=')';
	$<action>$.code = W_DO;
	$<action>$.command = do_str;
	$<action>$.str = make2_str("do ", do_str);
	do_str = NULL;
	do_length = 0;
}

/* some other stuff for ecpg */

c_expr:  attr opt_indirection
				{
					$$ = make2_str($1, $2);
				}
		| row_expr
				{	$$ = $1;  }
		| AexprConst
				{	$$ = $1;  }
		| ColId
				{
					$$ = $1;
				}
		| '-' c_expr %prec UMINUS
				{	$$ = make2_str("-", $2); }
		| a_expr '+' c_expr
				{	$$ = make3_str($1, "+", $3); }
		| a_expr '-' c_expr
				{	$$ = make3_str($1, "-", $3); }
		| a_expr '/' c_expr
				{	$$ = make3_str($1, "/", $3); }
		| a_expr '*' c_expr
				{	$$ = make3_str($1, "*", $3); }
		| a_expr '<' c_expr
				{	$$ = make3_str($1, "<", $3); }
		| a_expr '>' c_expr
				{	$$ = make3_str($1, ">", $3); }
		| a_expr '=' c_expr
				{	$$ = make3_str($1, "=", $3); }
	/*	| ':' c_expr
				{	$$ = make2_str(":", $2); }*/
		| ';' c_expr
				{	$$ = make2_str(";", $2); }
		| '|' c_expr
				{	$$ = make2_str("|", $2); }
		| a_expr TYPECAST Typename
				{
					$$ = make3_str($1, "::", $3);
				}
		| CAST '(' a_expr AS Typename ')'
				{
					$$ = make3_str(cat2_str("cast(", $3), "as", cat2_str($5, ")"));
				}
		| '(' a_expr_or_null ')'
				{	$$ = cat3_str("(", $2, ")"); }
		| a_expr Op c_expr
				{	$$ = make3_str($1, $2, $3);	}
		| a_expr LIKE c_expr
				{	$$ = make3_str($1, "like", $3); }
		| a_expr NOT LIKE c_expr
				{	$$ = make3_str($1, "not like", $4); }
		| Op c_expr
				{	$$ = make2_str($1, $2); }
		| a_expr Op
				{	$$ = make2_str($1, $2); }
		| func_name '(' '*' ')'
				{
					$$ = make2_str($1, "(*)"); 
				}
		| func_name '(' ')'
				{
					$$ = make2_str($1, "()"); 
				}
		| func_name '(' expr_list ')'
				{
					$$ = cat4_str($1, "(", $3, ")"); 
				}
		| CURRENT_DATE
				{
					$$ = "current_date";
				}
		| CURRENT_TIME
				{
					$$ = "current_time";
				}
		| CURRENT_TIME '(' Iconst ')'
				{
					if (atol($3) != 0)
						fprintf(stderr,"CURRENT_TIME(%s) precision not implemented; zero used instead", $3);
					$$ = "current_time";
				}
		| CURRENT_TIMESTAMP
				{
					$$ = "current_timestamp";
				}
		| CURRENT_TIMESTAMP '(' Iconst ')'
				{
					if (atol($3) != 0)
						fprintf(stderr,"CURRENT_TIMESTAMP(%s) precision not implemented; zero used instead",$3);
					$$ = "current_timestamp";
				}
		| CURRENT_USER
				{
					$$ = "current_user";
				}
		| EXISTS '(' SubSelect ')'
				{
					$$ = cat3_str("exists(", $3, ")");
				}
		| EXTRACT '(' extract_list ')'
				{
					$$ = cat3_str("extract(", $3, ")");
				}
		| POSITION '(' position_list ')'
				{
					$$ = cat3_str("position(", $3, ")");
				}
		| SUBSTRING '(' substr_list ')'
				{
					$$ = cat3_str("substring(", $3, ")");
				}
		/* various trim expressions are defined in SQL92 - thomas 1997-07-19 */
		| TRIM '(' BOTH trim_list ')'
				{
					$$ = cat3_str("trim(both", $4, ")");
				}
		| TRIM '(' LEADING trim_list ')'
				{
					$$ = cat3_str("trim(leading", $4, ")");
				}
		| TRIM '(' TRAILING trim_list ')'
				{
					$$ = cat3_str("trim(trailing", $4, ")");
				}
		| TRIM '(' trim_list ')'
				{
					$$ = cat3_str("trim(", $3, ")");
				}
		| a_expr ISNULL
				{	$$ = make2_str($1, "isnull"); }
		| a_expr IS NULL_P
				{	$$ = make2_str($1, "is null"); }
		| a_expr NOTNULL
				{	$$ = make2_str($1, "notnull"); }
		| a_expr IS NOT NULL_P
				{	$$ = make2_str($1, "is not null"); }
		/* IS TRUE, IS FALSE, etc used to be function calls
		 *  but let's make them expressions to allow the optimizer
		 *  a chance to eliminate them if a_expr is a constant string.
		 * - thomas 1997-12-22
		 */
		| a_expr IS TRUE_P
				{
				{	$$ = make2_str($1, "is true"); }
				}
		| a_expr IS NOT FALSE_P
				{
				{	$$ = make2_str($1, "is not false"); }
				}
		| a_expr IS FALSE_P
				{
				{	$$ = make2_str($1, "is false"); }
				}
		| a_expr IS NOT TRUE_P
				{
				{	$$ = make2_str($1, "is not true"); }
				}
		| a_expr BETWEEN b_expr AND b_expr
				{
					$$ = make5_str($1, "between", $3, "and", $5); 
				}
		| a_expr NOT BETWEEN b_expr AND b_expr
				{
					$$ = make5_str($1, "not between", $4, "and", $6); 
				}
		| a_expr IN '(' in_expr ')'
				{
					$$ = cat4_str($1, "in (", $4, ")"); 
				}
		| a_expr NOT IN '(' not_in_expr ')'
				{
					$$ = cat4_str($1, "not in (", $5, ")"); 
				}
		| a_expr Op '(' SubSelect ')'
				{
					$$ = make3_str($1, $2, cat3_str("(", $4,
")")); 
				}
		| a_expr '+' '(' SubSelect ')'
				{
					$$ = cat4_str($1, "+(", $4, ")"); 
				}
		| a_expr '-' '(' SubSelect ')'
				{
					$$ = cat4_str($1, "-(", $4, ")"); 
				}
		| a_expr '/' '(' SubSelect ')'
				{
					$$ = cat4_str($1, "/(", $4, ")"); 
				}
		| a_expr '*' '(' SubSelect ')'
				{
					$$ = cat4_str($1, "*(", $4, ")"); 
				}
		| a_expr '<' '(' SubSelect ')'
				{
					$$ = cat4_str($1, "<(", $4, ")"); 
				}
		| a_expr '>' '(' SubSelect ')'
				{
					$$ = cat4_str($1, ">(", $4, ")"); 
				}
		| a_expr '=' '(' SubSelect ')'
				{
					$$ = cat4_str($1, "=(", $4, ")"); 
				}
		| a_expr Op ANY '(' SubSelect ')'
				{
					$$ = make3_str($1, $2, cat3_str("any (", $5, ")")); 
				}
		| a_expr '+' ANY '(' SubSelect ')'
				{
					$$ = cat4_str($1, "+any(", $5, ")"); 
				}
		| a_expr '-' ANY '(' SubSelect ')'
				{
					$$ = cat4_str($1, "-any(", $5, ")"); 
				}
		| a_expr '/' ANY '(' SubSelect ')'
				{
					$$ = cat4_str($1, "/any(", $5, ")"); 
				}
		| a_expr '*' ANY '(' SubSelect ')'
				{
					$$ = cat4_str($1, "*any(", $5, ")"); 
				}
		| a_expr '<' ANY '(' SubSelect ')'
				{
					$$ = cat4_str($1, "<any(", $5, ")"); 
				}
		| a_expr '>' ANY '(' SubSelect ')'
				{
					$$ = cat4_str($1, ">any(", $5, ")"); 
				}
		| a_expr '=' ANY '(' SubSelect ')'
				{
					$$ = cat4_str($1, "=any(", $5, ")"); 
				}
		| a_expr Op ALL '(' SubSelect ')'
				{
					$$ = cat3_str($1, $2, cat3_str("all (", $5, ")")); 
				}
		| a_expr '+' ALL '(' SubSelect ')'
				{
					$$ = cat4_str($1, "+all(", $5, ")"); 
				}
		| a_expr '-' ALL '(' SubSelect ')'
				{
					$$ = cat4_str($1, "-all(", $5, ")"); 
				}
		| a_expr '/' ALL '(' SubSelect ')'
				{
					$$ = cat4_str($1, "/all(", $5, ")"); 
				}
		| a_expr '*' ALL '(' SubSelect ')'
				{
					$$ = cat4_str($1, "*all(", $5, ")"); 
				}
		| a_expr '<' ALL '(' SubSelect ')'
				{
					$$ = cat4_str($1, "<all(", $5, ")"); 
				}
		| a_expr '>' ALL '(' SubSelect ')'
				{
					$$ = cat4_str($1, ">all(", $5, ")"); 
				}
		| a_expr '=' ALL '(' SubSelect ')'
				{
					$$ = cat4_str($1, "=all(", $5, ")"); 
				}
		| a_expr AND c_expr
				{	$$ = make3_str($1, "and", $3); }
		| a_expr OR c_expr
				{	$$ = make3_str($1, "or", $3); }
		| NOT c_expr
				{	$$ = make2_str("not", $2); }
		| civariableonly
			        { $$ = ";;"; }
		;

into_list : coutputvariable | into_list ',' coutputvariable;

ecpgstart: SQL_START { reset_variables();}

dotext: /* empty */
	| dotext sql_anything {
                if (strlen(do_str) + yyleng + 1 >= do_length)
                        do_str = mm_realloc(do_str, do_length += yyleng);

                strcat(do_str, yytext);
}

vartext: both_anything { fwrite(yytext, yyleng, 1, yyout); }
        | vartext both_anything { fwrite(yytext, yyleng, 1, yyout); }

coutputvariable : ':' name indicator {
		add_variable(&argsresult, find_variable($2), ($3 == NULL) ? &no_indicator : find_variable($3)); 
}

cinputvariable : ':' name indicator {
		add_variable(&argsinsert, find_variable($2), ($3 == NULL) ? &no_indicator : find_variable($3)); 
}

civariableonly : ':' name {
		add_variable(&argsinsert, find_variable($2), &no_indicator); 
}

indicator: /* empty */			{ $$ = NULL; }
	| ':' name		 	{ check_indicator((find_variable($2))->type); $$ = $2; }
	| SQL_INDICATOR ':' name 	{ check_indicator((find_variable($3))->type); $$ = $3; }
	| SQL_INDICATOR name		{ check_indicator((find_variable($2))->type); $$ = $2; }

/*
 * C stuff
 */

symbol: IDENT	{ $$ = $1; }

c_anything: both_anything	{ fwrite(yytext, yyleng, 1, yyout); }
	| ';'			{ fputc(';', yyout); }

sql_anything: IDENT {} | ICONST {} | FCONST {}

both_anything: IDENT {} | ICONST {} | FCONST {}
	| S_AUTO | S_BOOL | S_CHAR | S_CONST | S_DOUBLE | S_EXTERN | S_FLOAT
	| S_INT	| S_LONG | S_REGISTER | S_SHORT	| S_SIGNED | S_STATIC
	| S_STRUCT | S_UNSIGNED	| S_VARCHAR | S_ANYTHING
	| '[' | ']' | '(' | ')' | '='

blockstart : '{' {
    braces_open++;
    fputc('{', yyout);
}

blockend : '}' {
    remove_variables(braces_open--);
    fputc('}', yyout);
}

%%

void yyerror(char * error)
{
    fprintf(stderr, "%s in line %d\n", error, yylineno);
    exit(1);
}
