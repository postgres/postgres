/* Copyright comment */
%{
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "catalog/catname.h"

#include "type.h"
#include "extern.h"

#define STRUCT_DEPTH 128

/*
 * Variables containing simple states.
 */
static int	struct_level = 0;
static char	errortext[128];
static int      QueryIsRule = 0;
static enum ECPGttype actual_type[STRUCT_DEPTH];
static char     *actual_storage[STRUCT_DEPTH];

/* temporarily store struct members while creating the data structure */
struct ECPGstruct_member *struct_member_list[STRUCT_DEPTH] = { NULL };

/* keep a list of cursors */
struct cursor *cur = NULL;

struct ECPGtype ecpg_no_indicator = {ECPGt_NO_INDICATOR, 0L, {NULL}};
struct variable no_indicator = {"no_indicator", &ecpg_no_indicator, 0, NULL};

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
		case W_BREAK:	 fprintf(yyout, "break;");
				 break;
		default:	 fprintf(yyout, "{/* %d not implemented yet */}", w->code);
				 break;
	}
}

static void
whenever_action(int mode)
{
	if (mode == 1 && when_nf.code != W_NOTHING)
	{
		output_line_number();
		fprintf(yyout, "\nif (sqlca.sqlcode == ECPG_NOT_FOUND) ");
		print_action(&when_nf);
	}
	if (when_error.code != W_NOTHING)
        {
		output_line_number();
                fprintf(yyout, "\nif (sqlca.sqlcode < 0) ");
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

static struct variable * allvariables = NULL;

static struct variable *
new_variable(const char * name, struct ECPGtype * type)
{
    struct variable * p = (struct variable*) mm_alloc(sizeof(struct variable));

    p->name = strdup(name);
    p->type = type;
    p->brace_level = braces_open;

    p->next = allvariables;
    allvariables = p;

    return(p);
}

static struct variable * find_variable(char * name);

static struct variable *
find_struct_member(char *name, char *str, struct ECPGstruct_member *members)
{
    char *next = strchr(++str, '.'), c = '\0';

    if (next != NULL)
    {
	c = *next;
	*next = '\0';
    }

    for (; members; members = members->next)
    {
        if (strcmp(members->name, str) == 0)
	{
		if (c == '\0')
		{
			/* found the end */
			switch (members->typ->typ)
			{
			   case ECPGt_array:
				return(new_variable(name, ECPGmake_array_type(members->typ->u.element, members->typ->size)));
			   case ECPGt_struct:
				return(new_variable(name, ECPGmake_struct_type(members->typ->u.members)));
			   default:
				return(new_variable(name, ECPGmake_simple_type(members->typ->typ, members->typ->size)));
			}
		}
		else
		{
			*next = c;
			if (c == '-')
			{
				next++;
				return(find_struct_member(name, next, members->typ->u.element->u.members));
			}
			else return(find_struct_member(name, next, members->typ->u.members));
		}
	}
    }

    return(NULL);
}

static struct variable *
find_struct(char * name, char *next)
{
    struct variable * p;
    char c = *next;

    /* first get the mother structure entry */
    *next = '\0';
    p = find_variable(name);

    /* restore the name, we will need it later on */
    *next = c;
    if (c == '-')
    {
	next++;
	return (find_struct_member(name, next, p->type->u.element->u.members));
    }
    else return (find_struct_member(name, next, p->type->u.members));
}

static struct variable *
find_simple(char * name)
{
    struct variable * p;

    for (p = allvariables; p; p = p->next)
    {
        if (strcmp(p->name, name) == 0)
	    return p;
    }

    return(NULL);
}

/* Note that this function will end the program in case of an unknown */
/* variable */
static struct variable *
find_variable(char * name)
{
    char * next;
    struct variable * p;

    if ((next = strchr(name, '.')) != NULL)
	p = find_struct(name, next);
    else if ((next = strstr(name, "->")) != NULL)
	p = find_struct(name, next);
    else
	p = find_simple(name);

    if (p == NULL)
    {
	sprintf(errortext, "The variable %s is not declared", name);
	yyerror(errortext);
    }

    return(p);
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
    ECPGdump_a_type(yyout, list->variable->name, list->variable->type,
	(list->indicator->type->typ != ECPGt_NO_INDICATOR) ? list->indicator->name : NULL,
	(list->indicator->type->typ != ECPGt_NO_INDICATOR) ? list->indicator->type : NULL, NULL, NULL);

    /* Then release the list element. */
    free(list);
}

static void
check_indicator(struct ECPGtype *var)
{
	/* make sure this is a valid indicator variable */
	switch (var->typ)
	{
		struct ECPGstruct_member *p;

		case ECPGt_short:
		case ECPGt_int:
		case ECPGt_long:
		case ECPGt_unsigned_short:
		case ECPGt_unsigned_int:
		case ECPGt_unsigned_long:
			break;

		case ECPGt_struct:
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
make1_str(const char *str)
{
        char * res_str = (char *)mm_alloc(strlen(str) + 1);

	strcpy(res_str, str);
	return (res_str);
}

static char *
make2_str(char *str1, char *str2)
{ 
	char * res_str  = (char *)mm_alloc(strlen(str1) + strlen(str2) + 1);

	strcpy(res_str, str1);
	strcat(res_str, str2);
	free(str1);
	free(str2);
	return(res_str);
}

static char *
cat2_str(char *str1, char *str2)
{ 
	char * res_str  = (char *)mm_alloc(strlen(str1) + strlen(str2) + 2);

	strcpy(res_str, str1);
	strcat(res_str, " ");
	strcat(res_str, str2);
	free(str1);
	free(str2);
	return(res_str);
}

static char *
make3_str(char *str1, char *str2, char * str3)
{    
        char * res_str  = (char *)mm_alloc(strlen(str1) + strlen(str2) + strlen(str3) + 1);
     
        strcpy(res_str, str1);
        strcat(res_str, str2);
	strcat(res_str, str3);
	free(str1);
	free(str2);
	free(str3);
        return(res_str);
}    

static char *
cat3_str(char *str1, char *str2, char * str3)
{    
        char * res_str  = (char *)mm_alloc(strlen(str1) + strlen(str2) + strlen(str3) + 3);
     
        strcpy(res_str, str1);
	strcat(res_str, " ");
        strcat(res_str, str2);
	strcat(res_str, " ");
	strcat(res_str, str3);
	free(str1);
	free(str2);
	free(str3);
        return(res_str);
}    

static char *
make4_str(char *str1, char *str2, char *str3, char *str4)
{    
        char * res_str  = (char *)mm_alloc(strlen(str1) + strlen(str2) + strlen(str3) + strlen(str4) + 1);
     
        strcpy(res_str, str1);
        strcat(res_str, str2);
	strcat(res_str, str3);
	strcat(res_str, str4);
	free(str1);
	free(str2);
	free(str3);
	free(str4);
        return(res_str);
}

static char *
cat4_str(char *str1, char *str2, char *str3, char *str4)
{    
        char * res_str  = (char *)mm_alloc(strlen(str1) + strlen(str2) + strlen(str3) + strlen(str4) + 4);
     
        strcpy(res_str, str1);
	strcat(res_str, " ");
        strcat(res_str, str2);
	strcat(res_str, " ");
	strcat(res_str, str3);
	strcat(res_str, " ");
	strcat(res_str, str4);
	free(str1);
	free(str2);
	free(str3);
	free(str4);
        return(res_str);
}

static char *
make5_str(char *str1, char *str2, char *str3, char *str4, char *str5)
{    
        char * res_str  = (char *)mm_alloc(strlen(str1) + strlen(str2) + strlen(str3) + strlen(str4) + strlen(str5) + 1);
     
        strcpy(res_str, str1);
        strcat(res_str, str2);
	strcat(res_str, str3);
	strcat(res_str, str4);
	strcat(res_str, str5);
	free(str1);
	free(str2);
	free(str3);
	free(str4);
	free(str5);
        return(res_str);
}    

static char *
cat5_str(char *str1, char *str2, char *str3, char *str4, char *str5)
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
	free(str1);
	free(str2);
	free(str3);
	free(str4);
	free(str5);
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
output_statement(char * stmt, int mode)
{
	int i, j=strlen(stmt);

	fputs("ECPGdo(__LINE__, \"", yyout);

	/* do this char by char as we have to filter '\"' */
	for (i = 0;i < j; i++)
		if (stmt[i] != '\"')
			fputc(stmt[i], yyout);
	fputs("\", ", yyout);

	/* dump variables to C file*/
	dump_variables(argsinsert);
	fputs("ECPGt_EOIT, ", yyout);
	dump_variables(argsresult);
	fputs("ECPGt_EORT);", yyout);
	whenever_action(mode);
	free(stmt);
}

%}

%union {
	double                  dval;
        int                     ival;
	char *                  str;
	struct when             action;
	struct index		index;
	int			tagname;
	struct this_type	type;
	enum ECPGttype		type_enum;
}

/* special embedded SQL token */
%token		SQL_BREAK SQL_CALL SQL_CONNECT SQL_CONNECTION SQL_CONTINUE
%token		SQL_DISCONNECT SQL_FOUND SQL_GO SQL_GOTO
%token		SQL_IDENTIFIED SQL_IMMEDIATE SQL_INDICATOR SQL_OPEN SQL_RELEASE
%token		SQL_SECTION SQL_SEMI SQL_SQLERROR SQL_SQLPRINT SQL_START
%token		SQL_STOP SQL_WHENEVER

/* C token */
%token		S_ANYTHING S_AUTO S_BOOL S_CHAR S_CONST S_DOUBLE S_ENUM S_EXTERN
%token		S_FLOAT S_INT S
%token		S_LONG S_REGISTER S_SHORT S_SIGNED S_STATIC S_STRUCT 
%token		S_UNSIGNED S_VARCHAR

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
                TABLE, TIME, TIMESTAMP, TIMEZONE_HOUR, TIMEZONE_MINUTE,
		TO, TRAILING, TRANSACTION, TRIM,
                UNION, UNIQUE, UPDATE, USER, USING,
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
%token  PASSWORD, CREATEDB, NOCREATEDB, CREATEUSER, NOCREATEUSER, VALID, UNTIL

/* Special keywords, not in the query language - see the "lex" file */
%token <str>    IDENT SCONST Op CSTRING CVARIABLE CPP_LINE
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
%type  <str> 	join_using where_clause relation_expr row_op sub_type
%type  <str>	opt_column_list insert_rest InsertStmt OptimizableStmt
%type  <str>    columnList DeleteStmt LockStmt UpdateStmt CursorStmt
%type  <str>    NotifyStmt columnElem copy_dirn 
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

%type  <str>	ECPGWhenever ECPGConnect connection_target ECPGOpen open_opts
%type  <str>	indicator ECPGExecute c_expr variable_list dotext
%type  <str>    storage_clause opt_initializer vartext c_anything blockstart
%type  <str>    blockend variable_list variable var_anything do_anything
%type  <str>	opt_pointer cvariable ECPGDisconnect dis_name
%type  <str>	stmt symbol opt_symbol ECPGRelease execstring server_name
%type  <str>	connection_object opt_server opt_port c_thing
%type  <str>    user_name opt_user char_variable ora_user ident
%type  <str>    db_prefix server opt_options opt_connection_name
%type  <str>	ECPGSetConnection c_line cpp_line s_enum
%type  <str>	enum_type

%type  <type_enum> simple_type

%type  <type>	type

%type  <action> action

%type  <index>	opt_array_bounds nest_array_bounds

%%
prog: statements;

statements: /* empty */
	| statements statement

statement: ecpgstart stmt SQL_SEMI
	| ECPGDeclaration
	| c_thing 			{ fprintf(yyout, "%s", $1); free($1); }
	| cpp_line			{ fprintf(yyout, "%s", $1); free($1); }
	| blockstart			{ fputs($1, yyout); free($1); }
	| blockend			{ fputs($1, yyout); free($1); }

stmt:  AddAttrStmt			{ output_statement($1, 0); }
		| AlterUserStmt		{ output_statement($1, 0); }
		| ClosePortalStmt	{ output_statement($1, 0); }
		| CopyStmt		{ output_statement($1, 0); }
		| CreateStmt		{ output_statement($1, 0); }
		| CreateAsStmt		{ output_statement($1, 0); }
		| CreateSeqStmt		{ output_statement($1, 0); }
		| CreatePLangStmt	{ output_statement($1, 0); }
		| CreateTrigStmt	{ output_statement($1, 0); }
		| CreateUserStmt	{ output_statement($1, 0); }
  		| ClusterStmt		{ output_statement($1, 0); }
		| DefineStmt 		{ output_statement($1, 0); }
		| DestroyStmt		{ output_statement($1, 0); }
		| DropPLangStmt		{ output_statement($1, 0); }
		| DropTrigStmt		{ output_statement($1, 0); }
		| DropUserStmt		{ output_statement($1, 0); }
		| ExtendStmt 		{ output_statement($1, 0); }
		| ExplainStmt		{ output_statement($1, 0); }
		| FetchStmt		{ output_statement($1, 1); }
		| GrantStmt		{ output_statement($1, 0); }
		| IndexStmt		{ output_statement($1, 0); }
		| ListenStmt		{ output_statement($1, 0); }
		| LockStmt		{ output_statement($1, 0); }
		| ProcedureStmt		{ output_statement($1, 0); }
 		| RecipeStmt		{ output_statement($1, 0); }
		| RemoveAggrStmt	{ output_statement($1, 0); }
		| RemoveOperStmt	{ output_statement($1, 0); }
		| RemoveFuncStmt	{ output_statement($1, 0); }
		| RemoveStmt		{ output_statement($1, 0); }
		| RenameStmt		{ output_statement($1, 0); }
		| RevokeStmt		{ output_statement($1, 0); }
                | OptimizableStmt	{
						if (strncmp($1, "/* declare" , sizeof("/* declare")-1) == 0)
						{
							fputs($1, yyout);
							output_line_number();
							free($1);
						}
						else
							output_statement($1, 1);
					}
		| RuleStmt		{ output_statement($1, 0); }
		| TransactionStmt	{
						fprintf(yyout, "ECPGtrans(__LINE__, \"%s\");", $1);
						whenever_action(0);
						free($1);
					}
		| ViewStmt		{ output_statement($1, 0); }
		| LoadStmt		{ output_statement($1, 0); }
		| CreatedbStmt		{ output_statement($1, 0); }
		| DestroydbStmt		{ output_statement($1, 0); }
		| VacuumStmt		{ output_statement($1, 0); }
		| VariableSetStmt	{ output_statement($1, 0); }
		| VariableShowStmt	{ output_statement($1, 0); }
		| VariableResetStmt	{ output_statement($1, 0); }
		| ECPGConnect		{
						fprintf(yyout, "ECPGconnect(__LINE__, %s);", $1);
						whenever_action(0);
						free($1);
					} 
		| ECPGDisconnect	{
						fprintf(yyout, "ECPGdisconnect(__LINE__, \"%s\");", $1); 
						whenever_action(0);
						free($1);
					} 
		| ECPGExecute		{
						fprintf(yyout, "ECPGdo(__LINE__, %s, ECPGt_EOIT, ECPGt_EORT);", $1);
						whenever_action(0);
						free($1);
					}
		| ECPGOpen		{ output_statement($1, 0); }
		| ECPGRelease		{ /* output already done */ }
		| ECPGSetConnection     {
						fprintf(yyout, "ECPGsetconn(__LINE__, %s);", $1);
						whenever_action(0);
                                       		free($1);
					}
		| ECPGWhenever		{
						fputs($1, yyout);
						output_line_number();
						free($1);
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
					$$ = cat3_str(cat5_str(make1_str("create user"), $3, $4, $5, $6), $7, $8);
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
					$$ = cat3_str(cat5_str(make1_str("alter user"), $3, $4, $5, $6), $7, $8);
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
					$$ = cat2_str(make1_str("drop user"), $3);
				}
		;

user_passwd_clause:  WITH PASSWORD UserId	{ $$ = cat2_str(make1_str("with password") , $3); }
			| /*EMPTY*/		{ $$ = make1_str(""); }
		;

user_createdb_clause:  CREATEDB
				{
					$$ = make1_str("createdb");
				}
			| NOCREATEDB
				{
					$$ = make1_str("nocreatedb");
				}
			| /*EMPTY*/		{ $$ = make1_str(""); }
		;

user_createuser_clause:  CREATEUSER
				{
					$$ = make1_str("createuser");
				}
			| NOCREATEUSER
				{
					$$ = make1_str("nocreateuser");
				}
			| /*EMPTY*/		{ $$ = NULL; }
		;

user_group_list:  user_group_list ',' UserId
				{
					$$ = cat3_str($1, make1_str(","), $3);
				}
			| UserId
				{
					$$ = $1;
				}
		;

user_group_clause:  IN GROUP user_group_list	{ $$ = cat2_str(make1_str("in group"), $3); }
			| /*EMPTY*/		{ $$ = make1_str(""); }
		;

user_valid_clause:  VALID UNTIL Sconst			{ $$ = cat2_str(make1_str("valid until"), $3);; }
			| /*EMPTY*/			{ $$ = make1_str(""); }
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
					$$ = cat4_str(make1_str("set"), $2, make1_str("to"), $4);
				}
		| SET ColId '=' var_value
				{
					$$ = cat4_str(make1_str("set"), $2, make1_str("="), $4);
				}
		| SET TIME ZONE zone_value
				{
					$$ = cat2_str(make1_str("set time zone"), $4);
				}

		;

var_value:  Sconst			{ $$ = $1; }
		| DEFAULT			{ $$ = make1_str("default"); }
		;

zone_value:  Sconst			{ $$ = $1; }
		| DEFAULT			{ $$ = make1_str("default"); }
		| LOCAL				{ $$ = make1_str("local"); }
		;

VariableShowStmt:  SHOW ColId
				{
					$$ = cat2_str(make1_str("show"), $2);
				}
		| SHOW TIME ZONE
				{
					$$ = make1_str("show time zone");
				}
		;

VariableResetStmt:	RESET ColId
				{
					$$ = cat2_str(make1_str("reset"), $2);
				}
		| RESET TIME ZONE
				{
					$$ = make1_str("reset time zone");
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
					$$ = cat4_str(make1_str("alter table"), $3, $4, $5);
				}
		;

alter_clause:  ADD opt_column columnDef
				{
					$$ = cat3_str(make1_str("add"), $2, $3);
				}
			| ADD '(' OptTableElementList ')'
				{
					$$ = make3_str(make1_str("add("), $3, make1_str(")"));
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
					$$ = cat2_str(make1_str("close"), $2);
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
					$$ = cat3_str(cat5_str(make1_str("copy"), $2, $3, $4, $5), $6, $7);
				}
		;

copy_dirn:	TO
				{ $$ = make1_str("to"); }
		| FROM
				{ $$ = make1_str("from"); }
		;

/*
 * copy_file_name NULL indicates stdio is used. Whether stdin or stdout is
 * used depends on the direction. (It really doesn't make sense to copy from
 * stdout. We silently correct the "typo".		 - AY 9/94
 */
copy_file_name:  Sconst					{ $$ = $1; }
		| STDIN					{ $$ = make1_str("stdin"); }
		| STDOUT				{ $$ = make1_str("stdout"); }
		;

opt_binary:  BINARY					{ $$ = make1_str("binary"); }
		| /*EMPTY*/				{ $$ = make1_str(""); }
		;

opt_with_copy:	WITH OIDS				{ $$ = make1_str("with oids"); }
		| /*EMPTY*/				{ $$ = make1_str(""); }
		;

/*
 * the default copy delimiter is tab but the user can configure it
 */
copy_delimiter:  USING DELIMITERS Sconst		{ $$ = cat2_str(make1_str("using delimiters"), $3); }
		| /*EMPTY*/				{ $$ = make1_str(""); }
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
					$$ = cat5_str(make1_str("create table"), $3,  make3_str(make1_str("("), $5, make1_str(")")), $7, $8);
				}
		;

OptTableElementList:  OptTableElementList ',' OptTableElement
				{
					$$ = cat3_str($1, make1_str(","), $3);
				}
			| OptTableElement
				{
					$$ = $1;
				}
			| /*EMPTY*/	{ $$ = make1_str(""); }
		;

OptTableElement:  columnDef		{ $$ = $1; }
			| TableConstraint	{ $$ = $1; }
		;

columnDef:  ColId Typename ColQualifier
				{
					$$ = cat3_str($1, $2, $3);
				}
		;

ColQualifier:  ColQualList	{ $$ = $1; }
			| /*EMPTY*/	{ $$ = make1_str(""); }
		;

ColQualList:  ColQualList ColConstraint	{ $$ = cat2_str($1,$2); }
			| ColConstraint		{ $$ = $1; }
		;

ColConstraint:
		CONSTRAINT name ColConstraintElem
				{
					$$ = cat3_str(make1_str("constraint"), $2, $3);
				}
		| ColConstraintElem
				{ $$ = $1; }
		;

ColConstraintElem:  CHECK '(' constraint_expr ')'
				{
					$$ = make3_str(make1_str("check("), $3, make1_str(")"));
				}
			| DEFAULT default_expr
				{
					$$ = cat2_str(make1_str("default"), $2);
				}
			| NOT NULL_P
				{
					$$ = make1_str("not null");
				}
			| UNIQUE
				{
					$$ = make1_str("unique");
				}
			| PRIMARY KEY
				{
					$$ = make1_str("primary key");
				}
			| REFERENCES ColId opt_column_list key_match key_actions
				{
					fprintf(stderr, "CREATE TABLE/FOREIGN KEY clause ignored; not yet implemented");
					$$ = make1_str("");
				}
		;

default_list:  default_list ',' default_expr
				{
					$$ = cat3_str($1, make1_str(","), $3);
				}
			| default_expr
				{
					$$ = $1;
				}
		;

default_expr:  AexprConst
				{	$$ = $1; }
			| NULL_P
				{	$$ = make1_str("null"); }
			| '-' default_expr %prec UMINUS
				{	$$ = cat2_str(make1_str("-"), $2); }
			| default_expr '+' default_expr
				{	$$ = cat3_str($1, make1_str("+"), $3); }
			| default_expr '-' default_expr
				{	$$ = cat3_str($1, make1_str("-"), $3); }
			| default_expr '/' default_expr
				{	$$ = cat3_str($1, make1_str("/"), $3); }
			| default_expr '*' default_expr
				{	$$ = cat3_str($1, make1_str("*"), $3); }
			| default_expr '=' default_expr
				{	yyerror("boolean expressions not supported in DEFAULT"); }
			| default_expr '<' default_expr
				{	yyerror("boolean expressions not supported in DEFAULT"); }
			| default_expr '>' default_expr
				{	yyerror("boolean expressions not supported in DEFAULT"); }
/* not possible in embedded sql 
			| ':' default_expr
				{	$$ = cat2_str(make1_str(":"), $2); }
*/
			| ';' default_expr
				{	$$ = cat2_str(make1_str(";"), $2); }
			| '|' default_expr
				{	$$ = cat2_str(make1_str("|"), $2); }
			| default_expr TYPECAST Typename
				{	$$ = cat3_str($1, make1_str("::"), $3); }
			| CAST '(' default_expr AS Typename ')'
				{
					$$ = cat3_str(make2_str(make1_str("cast("), $3) , make1_str("as"), make2_str($5, make1_str(")")));
				}
			| '(' default_expr ')'
				{	$$ = make3_str(make1_str("("), $2, make1_str(")")); }
			| func_name '(' ')'
				{	$$ = cat2_str($1, make1_str("()")); }
			| func_name '(' default_list ')'
				{	$$ = cat2_str($1, make3_str(make1_str("("), $3, make1_str(")"))); }
			| default_expr Op default_expr
				{
					if (!strcmp("<=", $2) || !strcmp(">=", $2))
						yyerror("boolean expressions not supported in DEFAULT");
					$$ = cat3_str($1, $2, $3);
				}
			| Op default_expr
				{	$$ = cat2_str($1, $2); }
			| default_expr Op
				{	$$ = cat2_str($1, $2); }
			/* XXX - thomas 1997-10-07 v6.2 function-specific code to be changed */
			| CURRENT_DATE
				{	$$ = make1_str("current_date"); }
			| CURRENT_TIME
				{	$$ = make1_str("current_time"); }
			| CURRENT_TIME '(' Iconst ')'
				{
					if ($3 != 0)
						fprintf(stderr, "CURRENT_TIME(%s) precision not implemented; zero used instead",$3);
					$$ = "current_time";
				}
			| CURRENT_TIMESTAMP
				{	$$ = make1_str("current_timestamp"); }
			| CURRENT_TIMESTAMP '(' Iconst ')'
				{
					if ($3 != 0)
						fprintf(stderr, "CURRENT_TIMESTAMP(%s) precision not implemented; zero used instead",$3);
					$$ = "current_timestamp";
				}
			| CURRENT_USER
				{	$$ = make1_str("current_user"); }
			| USER
				{       $$ = make1_str("user"); }
		;

/* ConstraintElem specifies constraint syntax which is not embedded into
 *  a column definition. ColConstraintElem specifies the embedded form.
 * - thomas 1997-12-03
 */
TableConstraint:  CONSTRAINT name ConstraintElem
				{
						$$ = cat3_str(make1_str("constraint"), $2, $3);
				}
		| ConstraintElem
				{ $$ = $1; }
		;

ConstraintElem:  CHECK '(' constraint_expr ')'
				{
					$$ = make3_str(make1_str("check("), $3, make1_str(")"));
				}
		| UNIQUE '(' columnList ')'
				{
					$$ = make3_str(make1_str("unique("), $3, make1_str(")"));
				}
		| PRIMARY KEY '(' columnList ')'
				{
					$$ = make3_str(make1_str("primary key("), $4, make1_str(")"));
				}
		| FOREIGN KEY '(' columnList ')' REFERENCES ColId opt_column_list key_match key_actions
				{
					fprintf(stderr, "CREATE TABLE/FOREIGN KEY clause ignored; not yet implemented");
					$$ = "";
				}
		;

constraint_list:  constraint_list ',' constraint_expr
				{
					$$ = cat3_str($1, make1_str(","), $3);
				}
			| constraint_expr
				{
					$$ = $1;
				}
		;

constraint_expr:  AexprConst
				{	$$ = $1; }
			| NULL_P
				{	$$ = make1_str("null"); }
			| ColId
				{
					$$ = $1;
				}
			| '-' constraint_expr %prec UMINUS
				{	$$ = cat2_str(make1_str("-"), $2); }
			| constraint_expr '+' constraint_expr
				{	$$ = cat3_str($1, make1_str("+"), $3); }
			| constraint_expr '-' constraint_expr
				{	$$ = cat3_str($1, make1_str("-"), $3); }
			| constraint_expr '/' constraint_expr
				{	$$ = cat3_str($1, make1_str("/"), $3); }
			| constraint_expr '*' constraint_expr
				{	$$ = cat3_str($1, make1_str("*"), $3); }
			| constraint_expr '=' constraint_expr
				{	$$ = cat3_str($1, make1_str("="), $3); }
			| constraint_expr '<' constraint_expr
				{	$$ = cat3_str($1, make1_str("<"), $3); }
			| constraint_expr '>' constraint_expr
				{	$$ = cat3_str($1, make1_str(">"), $3); }
/* this one doesn't work with embedded sql anyway
			| ':' constraint_expr
				{	$$ = cat2_str(make1_str(":"), $2); }
*/
			| ';' constraint_expr
				{	$$ = cat2_str(make1_str(";"), $2); }
			| '|' constraint_expr
				{	$$ = cat2_str(make1_str("|"), $2); }
			| constraint_expr TYPECAST Typename
				{
					$$ = cat3_str($1, make1_str("::"), $3);
				}
			| CAST '(' constraint_expr AS Typename ')'
				{
					$$ = cat3_str(make2_str(make1_str("cast("), $3), make1_str("as"), make2_str($5, make1_str(")"))); 
				}
			| '(' constraint_expr ')'
				{	$$ = make3_str(make1_str("("), $2, make1_str(")")); }
			| func_name '(' ')'
				{
				{	$$ = cat2_str($1, make1_str("()")); }
				}
			| func_name '(' constraint_list ')'
				{
					$$ = cat2_str($1, make3_str(make1_str("("), $3, make1_str(")")));
				}
			| constraint_expr Op constraint_expr
				{	$$ = cat3_str($1, $2, $3); }
			| constraint_expr LIKE constraint_expr
				{	$$ = cat3_str($1, make1_str("like"), $3); }
			| constraint_expr AND constraint_expr
				{	$$ = cat3_str($1, make1_str("and"), $3); }
			| constraint_expr OR constraint_expr
				{	$$ = cat3_str($1, make1_str("or"), $3); }
			| NOT constraint_expr
				{	$$ = cat2_str(make1_str("not"), $2); }
			| Op constraint_expr
				{	$$ = cat2_str($1, $2); }
			| constraint_expr Op
				{	$$ = cat2_str($1, $2); }
			| constraint_expr ISNULL
				{	$$ = cat2_str($1, make1_str("isnull")); }
			| constraint_expr IS NULL_P
				{	$$ = cat2_str($1, make1_str("is null")); }
			| constraint_expr NOTNULL
				{	$$ = cat2_str($1, make1_str("notnull")); }
			| constraint_expr IS NOT NULL_P
				{	$$ = cat2_str($1, make1_str("is not null")); }
			| constraint_expr IS TRUE_P
				{	$$ = cat2_str($1, make1_str("is true")); }
			| constraint_expr IS FALSE_P
				{	$$ = cat2_str($1, make1_str("is false")); }
			| constraint_expr IS NOT TRUE_P
				{	$$ = cat2_str($1, make1_str("is not true")); }
			| constraint_expr IS NOT FALSE_P
				{	$$ = cat2_str($1, make1_str("is not false")); }
		;

key_match:  MATCH FULL					{ $$ = make1_str("match full"); }
		| MATCH PARTIAL					{ $$ = make1_str("match partial"); }
		| /*EMPTY*/					{ $$ = make1_str(""); }
		;

key_actions:  key_action key_action		{ $$ = cat2_str($1, $2); }
		| key_action					{ $$ = $1; }
		| /*EMPTY*/					{ $$ = make1_str(""); }
		;

key_action:  ON DELETE key_reference	{ $$ = cat2_str(make1_str("on delete"), $3); }
		| ON UPDATE key_reference		{ $$ = cat2_str(make1_str("on update"), $3); }
		;

key_reference:  NO ACTION	{ $$ = make1_str("no action"); }
		| CASCADE	{ $$ = make1_str("cascade"); }
		| SET DEFAULT	{ $$ = make1_str("set default"); }
		| SET NULL_P	{ $$ = make1_str("set null"); }
		;

OptInherit:  INHERITS '(' relation_name_list ')' { $$ = make3_str(make1_str("inherits ("), $3, make1_str(")")); }
		| /*EMPTY*/ { $$ = make1_str(""); }
		;

/*
 *	"ARCHIVE" keyword was removed in 6.3, but we keep it for now
 *  so people can upgrade with old pg_dump scripts. - momjian 1997-11-20(?)
 */
OptArchiveType:  ARCHIVE '=' NONE { $$ = make1_str("archive = none"); }
		| /*EMPTY*/	  { $$ = make1_str(""); }			
		;

CreateAsStmt:  CREATE TABLE relation_name OptCreateAs AS SubSelect
		{
			$$ = cat5_str(make1_str("create table"), $3, $4, make1_str("as"), $6); 
		}
		;

OptCreateAs:  '(' CreateAsList ')' { $$ = make3_str(make1_str("("), $2, make1_str(")")); }
			| /*EMPTY*/ { $$ = make1_str(""); }	
		;

CreateAsList:  CreateAsList ',' CreateAsElement	{ $$ = cat3_str($1, make1_str(","), $3); }
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
					$$ = cat3_str(make1_str("create sequence"), $3, $4);
				}
		;

OptSeqList:  OptSeqList OptSeqElem
				{ $$ = cat2_str($1, $2); }
			|	{ $$ = make1_str(""); }
		;

OptSeqElem:  CACHE IntegerOnly
				{
					$$ = cat2_str(make1_str("cache"), $2);
				}
			| CYCLE
				{
					$$ = make1_str("cycle");
				}
			| INCREMENT IntegerOnly
				{
					$$ = cat2_str(make1_str("increment"), $2);
				}
			| MAXVALUE IntegerOnly
				{
					$$ = cat2_str(make1_str("maxvalue"), $2);
				}
			| MINVALUE IntegerOnly
				{
					$$ = cat2_str(make1_str("minvalue"), $2);
				}
			| START IntegerOnly
				{
					$$ = cat2_str(make1_str("start"), $2);
				}
		;

IntegerOnly:  Iconst
				{
					$$ = $1;
				}
			| '-' Iconst
				{
					$$ = cat2_str(make1_str("-"), $2);
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
				$$ = cat4_str(cat5_str(make1_str("create"), $2, make1_str("precedural language"), $5, make1_str("handler")), $7, make1_str("langcompiler"), $9);
			}
		;

PLangTrusted:		TRUSTED { $$ = make1_str("trusted"); }
			|	{ $$ = make1_str(""); }

DropPLangStmt:  DROP PROCEDURAL LANGUAGE Sconst
			{
				$$ = cat2_str(make1_str("drop procedural language"), $4);
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
					$$ = cat2_str(cat5_str(cat5_str(make1_str("create trigger"), $3, $4, $5, make1_str("on")), $7, $8, make1_str("execute procedure"), $11), make3_str(make1_str("("), $13, make1_str(")")));
				}
		;

TriggerActionTime:  BEFORE				{ $$ = make1_str("before"); }
			| AFTER				{ $$ = make1_str("after"); }
		;

TriggerEvents:	TriggerOneEvent
				{
					$$ = $1;
				}
			| TriggerOneEvent OR TriggerOneEvent
				{
					$$ = cat3_str($1, make1_str("or"), $3);
				}
			| TriggerOneEvent OR TriggerOneEvent OR TriggerOneEvent
				{
					$$ = cat5_str($1, make1_str("or"), $3, make1_str("or"), $5);
				}
		;

TriggerOneEvent:  INSERT				{ $$ = make1_str("insert"); }
			| DELETE			{ $$ = make1_str("delete"); }
			| UPDATE			{ $$ = make1_str("update"); }
		;

TriggerForSpec:  FOR TriggerForOpt TriggerForType
				{
					$$ = cat3_str(make1_str("for"), $2, $3);
				}
		;

TriggerForOpt:  EACH					{ $$ = make1_str("each"); }
			| /*EMPTY*/			{ $$ = make1_str(""); }
		;

TriggerForType:  ROW					{ $$ = make1_str("row"); }
			| STATEMENT			{ $$ = make1_str("statement"); }
		;

TriggerFuncArgs:  TriggerFuncArg
				{ $$ = $1 }
			| TriggerFuncArgs ',' TriggerFuncArg
				{ $$ = cat3_str($1, make1_str(","), $3); }
			| /*EMPTY*/
				{ $$ = make1_str(""); }
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
			| ident		{  $$ = $1; }
		;

DropTrigStmt:  DROP TRIGGER name ON relation_name
				{
					$$ = cat4_str(make1_str("drop trigger"), $3, make1_str("on"), $5);
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
					$$ = cat3_str(make1_str("create"), $2, $3);
				}
		;

def_rest:  def_name definition
				{
					$$ = cat2_str($1, $2);
				}
		;

def_type:  OPERATOR		{ $$ = make1_str("operator"); }
		| TYPE_P	{ $$ = make1_str("type"); }
		| AGGREGATE	{ $$ = make1_str("aggregate"); }
		;

def_name:  PROCEDURE		{ $$ = make1_str("procedure"); }
		| JOIN		{ $$ = make1_str("join"); }
		| ColId		{ $$ = $1; }
		| MathOp	{ $$ = $1; }
		| Op		{ $$ = $1; }
		;

definition:  '(' def_list ')'				{ $$ = make3_str(make1_str("("), $2, make1_str(")")); }
		;

def_list:  def_elem					{ $$ = $1; }
		| def_list ',' def_elem			{ $$ = cat3_str($1, make1_str(","), $3); }
		;

def_elem:  def_name '=' def_arg	{
					$$ = cat3_str($1, make1_str("="), $3);
				}
		| def_name
				{
					$$ = $1;
				}
		| DEFAULT '=' def_arg
				{
					$$ = cat2_str(make1_str("default ="), $3);
				}
		;

def_arg:  ColId			{  $$ = $1; }
		| all_Op	{  $$ = $1; }
		| NumConst	{  $$ = $1; /* already a Value */ }
		| Sconst	{  $$ = $1; }
		| SETOF ColId
				{
					$$ = cat2_str(make1_str("setof"), $2);
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
					$$ = cat2_str(make1_str("drop table"), $3);
				}
		| DROP SEQUENCE relation_name_list
				{
					$$ = cat2_str(make1_str("drop sequence"), $3);
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
					$$ = cat4_str(make1_str("fetch"), $2, $3, $4);
				}
		|	MOVE opt_direction fetch_how_many opt_portal_name
				{
					$$ = cat4_str(make1_str("fetch"), $2, $3, $4);
				}
		;

opt_direction:	FORWARD		{ $$ = make1_str("forward"); }
		| BACKWARD	{ $$ = make1_str("backward"); }
		| /*EMPTY*/	{ $$ = make1_str(""); /* default */ }
		;

fetch_how_many:  Iconst
			   { $$ = $1;
				 if (atol($1) <= 0) yyerror("Please specify nonnegative count for fetch"); }
		| ALL		{ $$ = make1_str("all"); }
		| /*EMPTY*/	{ $$ = make1_str(""); /*default*/ }
		;

opt_portal_name:  IN name		{ $$ = cat2_str(make1_str("in"), $2); }
		| name			{ $$ = cat2_str(make1_str("in"), $1); }
		| /*EMPTY*/		{ $$ = make1_str(""); }
		;


/*****************************************************************************
 *
 *		QUERY:
 *				GRANT [privileges] ON [relation_name_list] TO [GROUP] grantee
 *
 *****************************************************************************/

GrantStmt:  GRANT privileges ON relation_name_list TO grantee opt_with_grant
				{
					$$ = cat2_str(cat5_str(make1_str("grant"), $2, make1_str("on"), $4, make1_str("to")), $6);
				}
		;

privileges:  ALL PRIVILEGES
				{
				 $$ = make1_str("all privileges");
				}
		| ALL
				{
				 $$ = make1_str("all");
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
						$$ = cat3_str($1, make1_str(","), $3);
				}
		;

operation:  SELECT
				{
						$$ = make1_str("select");
				}
		| INSERT
				{
						$$ = make1_str("insert");
				}
		| UPDATE
				{
						$$ = make1_str("update");
				}
		| DELETE
				{
						$$ = make1_str("delete");
				}
		| RULE
				{
						$$ = make1_str("rule");
				}
		;

grantee:  PUBLIC
				{
						$$ = make1_str("public");
				}
		| GROUP ColId
				{
						$$ = cat2_str(make1_str("group"), $2);
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
					$$ = cat2_str(cat5_str(make1_str("revoke"), $2, make1_str("on"), $4, make1_str("from")), $6);
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
					$$ = cat5_str(cat5_str(make1_str("create"), $2, make1_str("index"), $4, make1_str("on")), $6, $7, make3_str(make1_str("("), $9, make1_str(")")), $11);
				}
		;

index_opt_unique:  UNIQUE	{ $$ = make1_str("unique"); }
		| /*EMPTY*/	{ $$ = make1_str(""); }
		;

access_method_clause:  USING access_method	{ $$ = cat2_str(make1_str("using"), $2); }
		| /*EMPTY*/			{ $$ = make1_str(""); }
		;

index_params:  index_list			{ $$ = $1; }
		| func_index			{ $$ = $1; }
		;

index_list:  index_list ',' index_elem		{ $$ = cat3_str($1, make1_str(","), $3); }
		| index_elem			{ $$ = $1; }
		;

func_index:  func_name '(' name_list ')' opt_type opt_class
				{
					$$ = cat4_str($1, make3_str(make1_str("("), $3, ")"), $5, $6);
				}
		  ;

index_elem:  attr_name opt_type opt_class
				{
					$$ = cat3_str($1, $2, $3);
				}
		;

opt_type:  ':' Typename		{ $$ = cat2_str(make1_str(":"), $2); }
		| FOR Typename	{ $$ = cat2_str(make1_str("for"), $2); }
		| /*EMPTY*/	{ $$ = make1_str(""); }
		;

/* opt_class "WITH class" conflicts with preceeding opt_type
 *  for Typename of "TIMESTAMP WITH TIME ZONE"
 * So, remove "WITH class" from the syntax. OK??
 * - thomas 1997-10-12
 *		| WITH class							{ $$ = $2; }
 */
opt_class:  class				{ $$ = $1; }
		| USING class			{ $$ = cat2_str(make1_str("using"), $2); }
		| /*EMPTY*/			{ $$ = make1_str(""); }
		;

/*****************************************************************************
 *
 *		QUERY:
 *				extend index <indexname> [where <qual>]
 *
 *****************************************************************************/

ExtendStmt:  EXTEND INDEX index_name where_clause
				{
					$$ = cat3_str(make1_str("extend index"), $3, $4);
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
					$$ = cat2_str(make1_str("execute recipe"), $3);
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
					$$ = cat2_str(cat5_str(cat5_str(make1_str("create function"), $3, $4, make1_str("returns"), $6), $7, make1_str("as"), $9, make1_str("language")), $11);
				}

opt_with:  WITH definition			{ $$ = cat2_str(make1_str("with"), $2); }
		| /*EMPTY*/			{ $$ = make1_str(""); }
		;

func_args:  '(' func_args_list ')'		{ $$ = make3_str(make1_str("("), $2, make1_str(")")); }
		| '(' ')'			{ $$ = make1_str("()"); }
		;

func_args_list:  TypeId				{ $$ = $1; }
		| func_args_list ',' TypeId
				{	$$ = cat3_str($1, make1_str(","), $3); }
		;

func_return:  set_opt TypeId
				{
					$$ = cat2_str($1, $2);
				}
		;

set_opt:  SETOF					{ $$ = make1_str("setof"); }
		| /*EMPTY*/			{ $$ = make1_str(""); }
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
					$$ = cat3_str(make1_str("drop"), $2, $3);;
				}
		;

remove_type:  TYPE_P		{  $$ = make1_str("type"); }
		| INDEX		{  $$ = make1_str("index"); }
		| RULE		{  $$ = make1_str("rule"); }
		| VIEW		{  $$ = make1_str("view"); }
		;


RemoveAggrStmt:  DROP AGGREGATE name aggr_argtype
				{
						$$ = cat3_str(make1_str("drop aggregate"), $3, $4);
				}
		;

aggr_argtype:  name			{ $$ = $1; }
		| '*'			{ $$ = make1_str("*"); }
		;


RemoveFuncStmt:  DROP FUNCTION func_name func_args
				{
						$$ = cat3_str(make1_str("drop function"), $3, $4);
				}
		;


RemoveOperStmt:  DROP OPERATOR all_Op '(' oper_argtypes ')'
				{
					$$ = cat3_str(make1_str("drop operator"), $3, make3_str(make1_str("("), $5, make1_str(")")));
				}
		;

all_Op:  Op | MathOp;

MathOp:	'+'				{ $$ = make1_str("+"); }
		| '-'			{ $$ = make1_str("-"); }
		| '*'			{ $$ = make1_str("*"); }
		| '/'			{ $$ = make1_str("/"); }
		| '<'			{ $$ = make1_str("<"); }
		| '>'			{ $$ = make1_str(">"); }
		| '='			{ $$ = make1_str("="); }
		;

oper_argtypes:	name
				{
				   yyerror("parser: argument type missing (use NONE for unary operators)");
				}
		| name ',' name
				{ $$ = cat3_str($1, make1_str(","), $3); }
		| NONE ',' name			/* left unary */
				{ $$ = cat2_str(make1_str("none,"), $3); }
		| name ',' NONE			/* right unary */
				{ $$ = cat2_str($1, make1_str(", none")); }
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
					$$ = cat4_str(cat5_str(make1_str("alter table"), $3, $4, make1_str("rename"), $6), $7, make1_str("to"), $9);
				}
		;

opt_name:  name							{ $$ = $1; }
		| /*EMPTY*/					{ $$ = make1_str(""); }
		;

opt_column:  COLUMN					{ $$ = make1_str("colmunn"); }
		| /*EMPTY*/				{ $$ = make1_str(""); }
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
					$$ = cat2_str(cat5_str(cat5_str(make1_str("create rule"), $3, make1_str("as on"), $7, make1_str("to")), $9, $10, make1_str("do"), $12), $13);
				}
		;

OptStmtList:  NOTHING					{ $$ = make1_str("nothing"); }
		| OptimizableStmt			{ $$ = $1; }
		| '[' OptStmtBlock ']'			{ $$ = cat3_str(make1_str("["), $2, make1_str("]")); }
		;

OptStmtBlock:  OptStmtMulti
				{  $$ = $1; }
		| OptimizableStmt
				{ $$ = $1; }
		;

OptStmtMulti:  OptStmtMulti OptimizableStmt ';'
				{  $$ = cat3_str($1, $2, make1_str(";")); }
		| OptStmtMulti OptimizableStmt
				{  $$ = cat2_str($1, $2); }
		| OptimizableStmt ';'
				{ $$ = cat2_str($1, make1_str(";")); }
		;

event_object:  relation_name '.' attr_name
				{
					$$ = make3_str($1, make1_str("."), $3);
				}
		| relation_name
				{
					$$ = $1;
				}
		;

/* change me to select, update, etc. some day */
event:	SELECT					{ $$ = make1_str("select"); }
		| UPDATE			{ $$ = make1_str("update"); }
		| DELETE			{ $$ = make1_str("delete"); }
		| INSERT			{ $$ = make1_str("insert"); }
		 ;

opt_instead:  INSTEAD					{ $$ = make1_str("instead"); }
		| /*EMPTY*/				{ $$ = make1_str(""); }
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
					$$ = cat2_str(make1_str("notify"), $2);
				}
		;

ListenStmt:  LISTEN relation_name
				{
					$$ = cat2_str(make1_str("listen"), $2);
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
TransactionStmt:  ABORT_TRANS TRANSACTION	{ $$ = make1_str("rollback"); }
	| BEGIN_TRANS TRANSACTION		{ $$ = make1_str("begin transaction"); }
	| BEGIN_TRANS WORK			{ $$ = make1_str("begin transaction"); }
	| COMMIT WORK				{ $$ = make1_str("commit"); }
	| END_TRANS TRANSACTION			{ $$ = make1_str("commit"); }
	| ROLLBACK WORK				{ $$ = make1_str("rollback"); }
	| ABORT_TRANS				{ $$ = make1_str("rollback"); }
	| COMMIT				{ $$ = make1_str("commit"); }
	| ROLLBACK				{ $$ = make1_str("rollback"); }

/*****************************************************************************
 *
 *		QUERY:
 *				define view <viewname> '('target-list ')' [where <quals> ]
 *
 *****************************************************************************/

ViewStmt:  CREATE VIEW name AS SelectStmt
				{
					$$ = cat4_str(make1_str("create view"), $3, make1_str("as"), $5);
				}
		;


/*****************************************************************************
 *
 *		QUERY:
 *				load make1_str("filename")
 *
 *****************************************************************************/

LoadStmt:  LOAD file_name
				{
					$$ = cat2_str(make1_str("load"), $2);
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
					$$ = cat3_str(make1_str("create database"), $3, $4);
				}
		;

opt_database:  WITH LOCATION '=' location	{ $$ = cat2_str(make1_str("with location ="), $4); }
		| /*EMPTY*/			{ $$ = make1_str(""); }
		;

location:  Sconst				{ $$ = $1; }
		| DEFAULT			{ $$ = make1_str("default"); }
		| /*EMPTY*/			{ $$ = make1_str(""); }
		;

/*****************************************************************************
 *
 *		QUERY:
 *				destroydb dbname
 *
 *****************************************************************************/

DestroydbStmt:	DROP DATABASE database_name
				{
					$$ = cat2_str(make1_str("drop database"), $3);
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
				   $$ = cat4_str(make1_str("cluster"), $2, make1_str("on"), $4);
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
					$$ = cat3_str(make1_str("vacuum"), $2, $3);
				}
		| VACUUM opt_verbose opt_analyze relation_name opt_va_list
				{
					if ( strlen($5) > 0 && strlen($4) == 0 )
						yyerror("parser: syntax error at or near \"(\"");
					$$ = cat5_str(make1_str("vacuum"), $2, $3, $4, $5);
				}
		;

opt_verbose:  VERBOSE					{ $$ = make1_str("verbose"); }
		| /*EMPTY*/				{ $$ = make1_str(""); }
		;

opt_analyze:  ANALYZE					{ $$ = make1_str("analyse"); }
		| /*EMPTY*/				{ $$ = make1_str(""); }
		;

opt_va_list:  '(' va_list ')'				{ $$ = make3_str(make1_str("("), $2, make1_str(")")); }
		| /*EMPTY*/				{ $$ = make1_str(""); }
		;

va_list:  name
				{ $$=$1; }
		| va_list ',' name
				{ $$=cat3_str($1, make1_str(","), $3); }
		;


/*****************************************************************************
 *
 *		QUERY:
 *				EXPLAIN query
 *
 *****************************************************************************/

ExplainStmt:  EXPLAIN opt_verbose OptimizableStmt
				{
					$$ = cat3_str(make1_str("explain"), $2, $3);
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
		| DeleteStmt
		;


/*****************************************************************************
 *
 *		QUERY:
 *				INSERT STATEMENTS
 *
 *****************************************************************************/

InsertStmt:  INSERT INTO relation_name opt_column_list insert_rest
				{
					$$ = cat4_str(make1_str("insert into"), $3, $4, $5);
				}
		;

insert_rest:  VALUES '(' res_target_list2 ')'
				{
					$$ = make3_str(make1_str("values("), $3, make1_str(")"));
				}
		| SELECT opt_unique res_target_list2
			 from_clause where_clause
			 group_clause having_clause
			 union_clause
				{
					$$ = cat4_str(cat5_str(make1_str("select"), $2, $3, $4, $5), $6, $7, $8);
				}
		;

opt_column_list:  '(' columnList ')'			{ $$ = make3_str(make1_str("("), $2, make1_str(")")); }
		| /*EMPTY*/				{ $$ = make1_str(""); }
		;

columnList:
		  columnList ',' columnElem
				{ $$ = cat3_str($1, make1_str(","), $3); }
		| columnElem
				{ $$ = $1; }
		;

columnElem:  ColId opt_indirection
				{
					$$ = cat2_str($1, $2);
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
					$$ = cat3_str(make1_str("delete from"), $3, $4);
				}
		;

/*
 *	Total hack to just lock a table inside a transaction.
 *	Is it worth making this a separate command, with
 *	its own node type and file.  I don't think so. bjm 1998/1/22
 */
LockStmt:  LOCK_P opt_table relation_name
				{
					$$ = cat3_str(make1_str("lock"), $2, $3);
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
					$$ = cat2_str(cat5_str(make1_str("update"), $2, make1_str("set"), $4, $5), $6);
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

					this->name = strdup($2);
					this->command = cat4_str(cat5_str(cat5_str(make1_str("declare"), strdup($2), $3, make1_str("cursor for select"), $7), $8, $9, $10, $11), $12, $13, $14);
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

					$$ = make5_str(make1_str("/* declare cursor \""), $2, make1_str("\" statement has been moved to location of open cursor \""), strdup($2), make1_str("\" statement. */"));
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
					$$ = cat2_str(cat5_str(cat5_str(make1_str("select"), $2, $3, $4, $5), $6, $7, $8, $9), $10);
				}
		;

union_clause:  UNION opt_union select_list
				{
					$$ = cat3_str(make1_str("union"), $2, $3);
				}
		| /*EMPTY*/
				{ $$ = make1_str(""); }
		;

select_list:  select_list UNION opt_union SubSelect
				{
					$$ = cat4_str($1, make1_str("union"), $3, $4);
				}
		| SubSelect
				{ $$ = $1; }
		;

SubSelect:	SELECT opt_unique res_target_list2
			 from_clause where_clause
			 group_clause having_clause
				{
					$$ = cat3_str(cat5_str(make1_str("select"), $2, $3, $4, $5), $6, $7);
				}
		;

result:  INTO opt_table relation_name			{ $$= cat3_str(make1_str("into"), $2, $3); }
		| INTO into_list			{ $$ = make1_str(""); }
		| /*EMPTY*/				{ $$ = make1_str(""); }
		;

opt_table:  TABLE					{ $$ = make1_str("table"); }
		| /*EMPTY*/				{ $$ = make1_str(""); }
		;

opt_union:  ALL						{ $$ = make1_str("all"); }
		| /*EMPTY*/				{ $$ = make1_str(""); }
		;

opt_unique:  DISTINCT					{ $$ = make1_str("distinct"); }
		| DISTINCT ON ColId			{ $$ = cat2_str(make1_str("distinct on"), $3); }
		| ALL					{ $$ = make1_str("all"); }
		| /*EMPTY*/				{ $$ = make1_str(""); }
		;

sort_clause:  ORDER BY sortby_list			{ $$ = cat2_str(make1_str("order by"), $3); }
		| /*EMPTY*/				{ $$ = make1_str(""); }
		;

sortby_list:  sortby					{ $$ = $1; }
		| sortby_list ',' sortby		{ $$ = cat3_str($1, make1_str(","), $3); }
		;

sortby:  ColId OptUseOp
				{
					$$ = cat2_str($1, $2);
				}
		| ColId '.' ColId OptUseOp
				{
					$$ = cat2_str(make3_str($1, make1_str("."), $3), $4);
				}
		| Iconst OptUseOp
				{
					$$ = cat2_str($1, $2);
				}
		;

OptUseOp:  USING Op				{ $$ = cat2_str(make1_str("using"), $2); }
		| USING '<'			{ $$ = make1_str("using <"); }
		| USING '>'			{ $$ = make1_str("using >"); }
		| ASC				{ $$ = make1_str("asc"); }
		| DESC				{ $$ = make1_str("desc"); }
		| /*EMPTY*/			{ $$ = make1_str(""); }
		;

/*
 *	jimmy bell-style recursive queries aren't supported in the
 *	current system.
 *
 *	...however, recursive addattr and rename supported.  make special
 *	cases for these.
 */
opt_inh_star:  '*'					{ $$ = make1_str("*"); }
		| /*EMPTY*/				{ $$ = make1_str(""); }
		;

relation_name_list:  name_list { $$ = $1; };

name_list:  name
				{	$$ = $1; }
		| name_list ',' name
				{	$$ = cat3_str($1, make1_str(","), $3); }
		;

group_clause:  GROUP BY groupby_list			{ $$ = cat2_str(make1_str("groub by"), $3); }
		| /*EMPTY*/				{ $$ = make1_str(""); }
		;

groupby_list:  groupby					{ $$ = $1; }
		| groupby_list ',' groupby		{ $$ = cat3_str($1, make1_str(","), $3); }
		;

groupby:  ColId
				{
					$$ = $1;
				}
		| ColId '.' ColId
				{
					$$ = make3_str($1, make1_str(","), $3);
				}
		| Iconst
				{
					$$ = $1;
				}
		;

having_clause:  HAVING a_expr
				{
#if FALSE
					yyerror("HAVING clause not yet implemented");
#endif
					$$ = cat2_str(make1_str("having"), $2);
				}
		| /*EMPTY*/		{ $$ = make1_str(""); }
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
		| FROM from_list	{ $$ = cat2_str(make1_str("from"), $2); }
		| /*EMPTY*/		{ $$ = make1_str(""); }
		;

from_list:	from_list ',' from_val
				{ $$ = cat3_str($1, make1_str(","), $3); }
		| from_val CROSS JOIN from_val
				{ yyerror("CROSS JOIN not yet implemented"); }
		| from_val
				{ $$ = $1; }
		;

from_val:  relation_expr AS ColLabel
				{
					$$ = cat3_str($1, make1_str("as"), $3);
				}
		| relation_expr ColId
				{
					$$ = cat2_str($1, $2);
				}
		| relation_expr
				{
					$$ = $1;
				}
		;

join_expr:  NATURAL join_expr					{ $$ = cat2_str(make1_str("natural"), $2); }
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

join_outer:  OUTER_P				{ $$ = make1_str("outer"); }
		| /*EMPTY*/			{ $$ = make1_str("");  /* no qualifiers */ }
		;

join_spec:	ON '(' a_expr ')'			{ $$ = make3_str(make1_str("on ("), $3, make1_str(")")); }
		| USING '(' join_list ')'		{ $$ = make3_str(make1_str("using ("), $3, make1_str(")")); }
		| /*EMPTY*/				{ $$ = make1_str("");  /* no qualifiers */ }
		;

join_list:  join_using					{ $$ = $1; }
		| join_list ',' join_using		{ $$ = cat3_str($1, make1_str(","), $3); }
		;

join_using:  ColId
				{
					$$ = $1;
				}
		| ColId '.' ColId
				{
					$$ = make3_str($1, make1_str("."), $3);
				}
		| Iconst
				{
					$$ = $1;;
				}
		;

where_clause:  WHERE a_expr			{ $$ = cat2_str(make1_str("where"), $2); }
		| /*EMPTY*/				{ $$ = make1_str("");  /* no qualifiers */ }
		;

relation_expr:	relation_name
				{
					/* normal relations */
					$$ = $1;
				}
		| relation_name '*'				  %prec '='
				{
					/* inheritance query */
					$$ = cat2_str($1, make1_str("*"));
				}

opt_array_bounds:  '[' ']' nest_array_bounds
			{
                            $$.index1 = 0;
                            $$.index2 = $3.index1;
                            $$.str = cat2_str(make1_str("[]"), $3.str);
                        }
		| '[' Iconst ']' nest_array_bounds
			{
                            $$.index1 = atol($2);
                            $$.index2 = $4.index1;
                            $$.str = cat4_str(make1_str("["), $2, make1_str("]"), $4.str);
                        }
		| /* EMPTY */
			{
                            $$.index1 = -1;
                            $$.index2 = -1;
                            $$.str= make1_str("");
                        }
		;

nest_array_bounds:	'[' ']' nest_array_bounds
                        {
                            $$.index1 = 0;
                            $$.index2 = $3.index1;
                            $$.str = cat2_str(make1_str("[]"), $3.str);
                        }
		| '[' Iconst ']' nest_array_bounds
			{
                            $$.index1 = atol($2);
                            $$.index2 = $4.index1;
                            $$.str = cat4_str(make1_str("["), $2, make1_str("]"), $4.str);
                        }
		| /* EMPTY */
			{
                            $$.index1 = -1;
                            $$.index2 = -1;
                            $$.str= make1_str("");
                        }
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
					$$ = cat2_str($1, $2.str);
				}
		| Character	{ $$ = $1; }
		| SETOF Array
				{
					$$ = cat2_str(make1_str("setof"), $2);
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

generic:  ident					{ $$ = $1; }
		| TYPE_P			{ $$ = make1_str("type"); }
		;

/* SQL92 numeric data types
 * Check FLOAT() precision limits assuming IEEE floating types.
 * Provide rudimentary DECIMAL() and NUMERIC() implementations
 *  by checking parameters and making sure they match what is possible with INTEGER.
 * - thomas 1997-09-18
 */
Numeric:  FLOAT opt_float
				{
					$$ = cat2_str(make1_str("float"), $2);
				}
		| DOUBLE PRECISION
				{
					$$ = make1_str("double precision");
				}
		| DECIMAL opt_decimal
				{
					$$ = cat2_str(make1_str("decimal"), $2);
				}
		| NUMERIC opt_numeric
				{
					$$ = cat2_str(make1_str("numeric"), $2);
				}
		;

numeric:  FLOAT
				{	$$ = make1_str("float"); }
		| DOUBLE PRECISION
				{	$$ = make1_str("double precision"); }
		| DECIMAL
				{	$$ = make1_str("decimal"); }
		| NUMERIC
				{	$$ = make1_str("numeric"); }
		;

opt_float:  '(' Iconst ')'
				{
					if (atol($2) < 1)
						yyerror("precision for FLOAT must be at least 1");
					else if (atol($2) >= 16)
						yyerror("precision for FLOAT must be less than 16");
					$$ = make3_str(make1_str("("), $2, make1_str(")"));
				}
		| /*EMPTY*/
				{
					$$ = make1_str("");
				}
		;

opt_numeric:  '(' Iconst ',' Iconst ')'
				{
					if (atol($2) != 9) {
						sprintf(errortext, make1_str("NUMERIC precision %s must be 9"), $2);
						yyerror(errortext);
					}
					if (atol($4) != 0) {
						sprintf(errortext, "NUMERIC scale %s must be zero", $4);
						yyerror(errortext);
					}
					$$ = cat3_str(make2_str(make1_str("("), $2), make1_str(","), make2_str($4, make1_str(")")));
				}
		| '(' Iconst ')'
				{
					if (atol($2) != 9) {
						sprintf("NUMERIC precision %s must be 9",$2);
						yyerror(errortext);
					}
					$$ = make3_str(make1_str("("), $2, make1_str(")"));
				}
		| /*EMPTY*/
				{
					$$ = make1_str("");
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
					$$ = cat3_str(make2_str(make1_str("("), $2), make1_str(","), make2_str($4, make1_str(")")));
				}
		| '(' Iconst ')'
				{
					if (atol($2) != 9) {
						sprintf(errortext, "DECIMAL precision %s exceeds implementation limit of 9",$2);
                                                yyerror(errortext);
                                        }
					$$ = make3_str(make1_str("("), $2, make1_str(")"));
				}
		| /*EMPTY*/
				{
					$$ = make1_str("");
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
						yyerror("internal parsing error; unrecognized character type");
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

					$$ = cat2_str($1, make3_str(make1_str("("), $3, make1_str(")")));
				}
		| character
				{
					$$ = $1;
				}
		;

character:  CHARACTER opt_varying opt_charset opt_collate
				{
					if (strlen($4) > 0) 
						fprintf(stderr, "COLLATE %s not yet implemented",$4);

					$$ = cat4_str(make1_str("character"), $2, $3, $4);
				}
		| CHAR opt_varying	{ $$ = cat2_str(make1_str("char"), $2); }
		| VARCHAR		{ $$ = make1_str("varchar"); }
		| NATIONAL CHARACTER opt_varying { $$ = cat2_str(make1_str("national character"), $3); }
		| NCHAR opt_varying		{ $$ = cat2_str(make1_str("nchar"), $2); }
		;

opt_varying:  VARYING			{ $$ = make1_str("varying"); }
		| /*EMPTY*/			{ $$ = make1_str(""); }
		;

opt_charset:  CHARACTER SET ColId	{ $$ = cat2_str(make1_str("character set"), $3); }
		| /*EMPTY*/				{ $$ = make1_str(""); }
		;

opt_collate:  COLLATE ColId		{ $$ = cat2_str(make1_str("collate"), $2); }
		| /*EMPTY*/					{ $$ = make1_str(""); }
		;

Datetime:  datetime
				{
					$$ = $1;
				}
		| TIMESTAMP opt_timezone
				{
					$$ = cat2_str(make1_str("timestamp"), $2);
				}
		| TIME
				{
					$$ = make1_str("time");
				}
		| INTERVAL opt_interval
				{
					$$ = cat2_str(make1_str("interval"), $2);
				}
		;

datetime:  YEAR_P								{ $$ = make1_str("year"); }
		| MONTH_P								{ $$ = make1_str("month"); }
		| DAY_P									{ $$ = make1_str("day"); }
		| HOUR_P								{ $$ = make1_str("hour"); }
		| MINUTE_P								{ $$ = make1_str("minute"); }
		| SECOND_P								{ $$ = make1_str("second"); }
		;

opt_timezone:  WITH TIME ZONE				{ $$ = make1_str("with time zone"); }
		| /*EMPTY*/					{ $$ = make1_str(""); }
		;

opt_interval:  datetime					{ $$ = $1; }
		| YEAR_P TO MONTH_P			{ $$ = make1_str("year to #month"); }
		| DAY_P TO HOUR_P			{ $$ = make1_str("day to hour"); }
		| DAY_P TO MINUTE_P			{ $$ = make1_str("day to minute"); }
		| DAY_P TO SECOND_P			{ $$ = make1_str("day to second"); }
		| HOUR_P TO MINUTE_P			{ $$ = make1_str("hour to minute"); }
		| MINUTE_P TO SECOND_P			{ $$ = make1_str("minute to second"); }
		| HOUR_P TO SECOND_P			{ $$ = make1_str("hour to second"); }
		| /*EMPTY*/					{ $$ = make1_str(""); }
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
					$$ = make1_str("null");
				}
		;

/* Expressions using row descriptors
 * Define row_descriptor to allow yacc to break the reduce/reduce conflict
 *  with singleton expressions.
 * Eliminated lots of code by defining row_op and sub_type clauses.
 * However, can not consolidate EXPR_LINK case with others subselects
 *  due to shift/reduce conflict with the non-subselect clause (the parser
 *  would have to look ahead more than one token to resolve the conflict).
 * - thomas 1998-05-09
 */
row_expr: '(' row_descriptor ')' IN '(' SubSelect ')'
				{
					$$ = make5_str(make1_str("("), $2, make1_str(") in ("), $6, make1_str(")"));
				}
		| '(' row_descriptor ')' NOT IN '(' SubSelect ')'
				{
					$$ = make5_str(make1_str("("), $2, make1_str(") not in ("), $7, make1_str(")"));
				}
		| '(' row_descriptor ')' row_op sub_type  '(' SubSelect ')'
				{
					$$ = make4_str(make5_str(make1_str("("), $2, make1_str(")"), $4, $5), make1_str("("), $7, make1_str(")"));
				}
		| '(' row_descriptor ')' row_op '(' SubSelect ')'
				{
					$$ = make3_str(make5_str(make1_str("("), $2, make1_str(")"), $4, make1_str("(")), $6, make1_str(")"));
				}
		| '(' row_descriptor ')' row_op '(' row_descriptor ')'
				{
					$$ = cat3_str(make3_str(make1_str("("), $2, make1_str(")")), $4, make3_str(make1_str("("), $6, make1_str(")")));
				}
		;

row_descriptor:  row_list ',' a_expr
				{
					$$ = cat3_str($1, make1_str(","), $3);
				}
		;

row_op:  Op			{ $$ = $1; }
	| '<'                   { $$ = "<"; }
        | '='                   { $$ = "="; }
        | '>'                   { $$ = ">"; }
        | '+'                   { $$ = "+"; }
        | '-'                   { $$ = "-"; }
        | '*'                   { $$ = "*"; }
        | '/'                   { $$ = "/"; }
              ;

sub_type:  ANY                  { $$ = make1_str("ANY"); }
         | ALL                  { $$ = make1_str("ALL"); }
              ;


row_list:  row_list ',' a_expr
				{
					$$ = cat3_str($1, make1_str(","), $3);
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
					$$ = cat2_str($1, $2);
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
				{	$$ = cat2_str(make1_str("-"), $2); }
		| a_expr '+' a_expr
				{	$$ = cat3_str($1, make1_str("+"), $3); }
		| a_expr '-' a_expr
				{	$$ = cat3_str($1, make1_str("-"), $3); }
		| a_expr '/' a_expr
				{	$$ = cat3_str($1, make1_str("/"), $3); }
		| a_expr '*' a_expr
				{	$$ = cat3_str($1, make1_str("*"), $3); }
		| a_expr '<' a_expr
				{	$$ = cat3_str($1, make1_str("<"), $3); }
		| a_expr '>' a_expr
				{	$$ = cat3_str($1, make1_str(">"), $3); }
		| a_expr '=' a_expr
				{	$$ = cat3_str($1, make1_str("="), $3); }
/* not possible in embedded sql		| ':' a_expr
				{	$$ = cat2_str(make1_str(":"), $2); }
*/
		| ';' a_expr
				{	$$ = cat2_str(make1_str(";"), $2); }
		| '|' a_expr
				{	$$ = cat2_str(make1_str("|"), $2); }
		| a_expr TYPECAST Typename
				{
					$$ = cat3_str($1, make1_str("::"), $3);
				}
		| CAST '(' a_expr AS Typename ')'
				{
					$$ = cat3_str(make2_str(make1_str("cast("), $3), make1_str("as"), make2_str($5, make1_str(")")));
				}
		| '(' a_expr_or_null ')'
				{	$$ = make3_str(make1_str("("), $2, make1_str(")")); }
		| a_expr Op a_expr
				{	$$ = cat3_str($1, $2, $3);	}
		| a_expr LIKE a_expr
				{	$$ = cat3_str($1, make1_str("like"), $3); }
		| a_expr NOT LIKE a_expr
				{	$$ = cat3_str($1, make1_str("not like"), $4); }
		| Op a_expr
				{	$$ = cat2_str($1, $2); }
		| a_expr Op
				{	$$ = cat2_str($1, $2); }
		| func_name '(' '*' ')'
				{
					$$ = cat2_str($1, make1_str("(*)")); 
				}
		| func_name '(' ')'
				{
					$$ = cat2_str($1, make1_str("()")); 
				}
		| func_name '(' expr_list ')'
				{
					$$ = make4_str($1, make1_str("("), $3, make1_str(")")); 
				}
		| CURRENT_DATE
				{
					$$ = make1_str("current_date");
				}
		| CURRENT_TIME
				{
					$$ = make1_str("current_time");
				}
		| CURRENT_TIME '(' Iconst ')'
				{
					if (atol($3) != 0)
						fprintf(stderr,"CURRENT_TIME(%s) precision not implemented; zero used instead", $3);
					$$ = make1_str("current_time");
				}
		| CURRENT_TIMESTAMP
				{
					$$ = make1_str("current_timestamp");
				}
		| CURRENT_TIMESTAMP '(' Iconst ')'
				{
					if (atol($3) != 0)
						fprintf(stderr,"CURRENT_TIMESTAMP(%s) precision not implemented; zero used instead",$3);
					$$ = make1_str("current_timestamp");
				}
		| CURRENT_USER
				{
					$$ = make1_str("current_user");
				}
		| USER
				{
  		     		        $$ = make1_str("user");
			     	}

		| EXISTS '(' SubSelect ')'
				{
					$$ = make3_str(make1_str("exists("), $3, make1_str(")"));
				}
		| EXTRACT '(' extract_list ')'
				{
					$$ = make3_str(make1_str("extract("), $3, make1_str(")"));
				}
		| POSITION '(' position_list ')'
				{
					$$ = make3_str(make1_str("position("), $3, make1_str(")"));
				}
		| SUBSTRING '(' substr_list ')'
				{
					$$ = make3_str(make1_str("substring("), $3, make1_str(")"));
				}
		/* various trim expressions are defined in SQL92 - thomas 1997-07-19 */
		| TRIM '(' BOTH trim_list ')'
				{
					$$ = make3_str(make1_str("trim(both"), $4, make1_str(")"));
				}
		| TRIM '(' LEADING trim_list ')'
				{
					$$ = make3_str(make1_str("trim(leading"), $4, make1_str(")"));
				}
		| TRIM '(' TRAILING trim_list ')'
				{
					$$ = make3_str(make1_str("trim(trailing"), $4, make1_str(")"));
				}
		| TRIM '(' trim_list ')'
				{
					$$ = make3_str(make1_str("trim("), $3, make1_str(")"));
				}
		| a_expr ISNULL
				{	$$ = cat2_str($1, make1_str("isnull")); }
		| a_expr IS NULL_P
				{	$$ = cat2_str($1, make1_str("is null")); }
		| a_expr NOTNULL
				{	$$ = cat2_str($1, make1_str("notnull")); }
		| a_expr IS NOT NULL_P
				{	$$ = cat2_str($1, make1_str("is not null")); }
		/* IS TRUE, IS FALSE, etc used to be function calls
		 *  but let's make them expressions to allow the optimizer
		 *  a chance to eliminate them if a_expr is a constant string.
		 * - thomas 1997-12-22
		 */
		| a_expr IS TRUE_P
				{
				{	$$ = cat2_str($1, make1_str("is true")); }
				}
		| a_expr IS NOT FALSE_P
				{
				{	$$ = cat2_str($1, make1_str("is not false")); }
				}
		| a_expr IS FALSE_P
				{
				{	$$ = cat2_str($1, make1_str("is false")); }
				}
		| a_expr IS NOT TRUE_P
				{
				{	$$ = cat2_str($1, make1_str("is not true")); }
				}
		| a_expr BETWEEN b_expr AND b_expr
				{
					$$ = cat5_str($1, make1_str("between"), $3, make1_str("and"), $5); 
				}
		| a_expr NOT BETWEEN b_expr AND b_expr
				{
					$$ = cat5_str($1, make1_str("not between"), $4, make1_str("and"), $6); 
				}
		| a_expr IN '(' in_expr ')'
				{
					$$ = make4_str($1, make1_str("in ("), $4, make1_str(")")); 
				}
		| a_expr NOT IN '(' not_in_expr ')'
				{
					$$ = make4_str($1, make1_str("not in ("), $5, make1_str(")")); 
				}
		| a_expr Op '(' SubSelect ')'
				{
					$$ = cat3_str($1, $2, make3_str(make1_str("("), $4, make1_str(")"))); 
				}
		| a_expr '+' '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("+("), $4, make1_str(")")); 
				}
		| a_expr '-' '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("-("), $4, make1_str(")")); 
				}
		| a_expr '/' '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("/("), $4, make1_str(")")); 
				}
		| a_expr '*' '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("*("), $4, make1_str(")")); 
				}
		| a_expr '<' '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("<("), $4, make1_str(")")); 
				}
		| a_expr '>' '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str(">("), $4, make1_str(")")); 
				}
		| a_expr '=' '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("=("), $4, make1_str(")")); 
				}
		| a_expr Op ANY '(' SubSelect ')'
				{
					$$ = cat3_str($1, $2, make3_str(make1_str("any("), $5, make1_str(")"))); 
				}
		| a_expr '+' ANY '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("+any("), $5, make1_str(")")); 
				}
		| a_expr '-' ANY '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("-any("), $5, make1_str(")")); 
				}
		| a_expr '/' ANY '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("/any("), $5, make1_str(")")); 
				}
		| a_expr '*' ANY '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("*any("), $5, make1_str(")")); 
				}
		| a_expr '<' ANY '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("<any("), $5, make1_str(")")); 
				}
		| a_expr '>' ANY '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str(">any("), $5, make1_str(")")); 
				}
		| a_expr '=' ANY '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("=any("), $5, make1_str(")")); 
				}
		| a_expr Op ALL '(' SubSelect ')'
				{
					$$ = cat3_str($1, $2, make3_str(make1_str("all ("), $5, make1_str(")"))); 
				}
		| a_expr '+' ALL '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("+all("), $5, make1_str(")")); 
				}
		| a_expr '-' ALL '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("-all("), $5, make1_str(")")); 
				}
		| a_expr '/' ALL '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("/all("), $5, make1_str(")")); 
				}
		| a_expr '*' ALL '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("*all("), $5, make1_str(")")); 
				}
		| a_expr '<' ALL '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("<all("), $5, make1_str(")")); 
				}
		| a_expr '>' ALL '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str(">all("), $5, make1_str(")")); 
				}
		| a_expr '=' ALL '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("=all("), $5, make1_str(")")); 
				}
		| a_expr AND a_expr
				{	$$ = cat3_str($1, make1_str("and"), $3); }
		| a_expr OR a_expr
				{	$$ = cat3_str($1, make1_str("or"), $3); }
		| NOT a_expr
				{	$$ = cat2_str(make1_str("not"), $2); }
		| cinputvariable
			        { $$ = make1_str(";;"); }
		;

/*
 * b_expr is a subset of the complete expression syntax
 *  defined by a_expr. b_expr is used in BETWEEN clauses
 *  to eliminate parser ambiguities stemming from the AND keyword.
 */

b_expr:  attr opt_indirection
				{
					$$ = cat2_str($1, $2);
				}
		| AexprConst
				{	$$ = $1;  }
		| ColId
				{
					$$ = $1;
				}
		| '-' b_expr %prec UMINUS
				{	$$ = cat2_str(make1_str("-"), $2); }
		| b_expr '+' b_expr
				{	$$ = cat3_str($1, make1_str("+"), $3); }
		| b_expr '-' b_expr
				{	$$ = cat3_str($1, make1_str("-"), $3); }
		| b_expr '/' b_expr
				{	$$ = cat3_str($1, make1_str("/"), $3); }
		| b_expr '*' b_expr
				{	$$ = cat3_str($1, make1_str("*"), $3); }
/* not possible in embedded sql		| ':' b_expr
				{	$$ = cat2_str(make1_str(":"), $2); }
*/
		| ';' b_expr
				{	$$ = cat2_str(make1_str(";"), $2); }
		| '|' b_expr
				{	$$ = cat2_str(make1_str("|"), $2); }
		| b_expr TYPECAST Typename
				{
					$$ = cat3_str($1, make1_str("::"), $3);
				}
		| CAST '(' b_expr AS Typename ')'
				{
					$$ = cat3_str(make2_str(make1_str("cast("), $3), make1_str("as"), make2_str($5, make1_str(")")));
				}
		| '(' a_expr ')'
				{	$$ = make3_str(make1_str("("), $2, make1_str(")")); }
		| b_expr Op b_expr
				{	$$ = cat3_str($1, $2, $3);	}
		| Op b_expr
				{	$$ = cat2_str($1, $2); }
		| b_expr Op
				{	$$ = cat2_str($1, $2); }
		| func_name '(' ')'
				{
					$$ = cat2_str($1, make1_str("()")); 
				}
		| func_name '(' expr_list ')'
				{
					$$ = make4_str($1, make1_str("("), $3, make1_str(")")); 
				}
		| CURRENT_DATE
				{
					$$ = make1_str("current_date");
				}
		| CURRENT_TIME
				{
					$$ = make1_str("current_time");
				}
		| CURRENT_TIME '(' Iconst ')'
				{
					if ($3 != 0)
						fprintf(stderr,"CURRENT_TIME(%s) precision not implemented; zero used instead", $3);
					$$ = make1_str("current_time");
				}
		| CURRENT_TIMESTAMP
				{
					$$ = make1_str("current_timestamp");
				}
		| CURRENT_TIMESTAMP '(' Iconst ')'
				{
					if (atol($3) != 0)
						fprintf(stderr,"CURRENT_TIMESTAMP(%s) precision not implemented; zero used instead",$3);
					$$ = make1_str("current_timestamp");
				}
		| CURRENT_USER
				{
					$$ = make1_str("current_user");
				}
		| USER
				{
					$$ = make1_str("user");
				}
		| POSITION '(' position_list ')'
				{
					$$ = make3_str(make1_str("position ("), $3, make1_str(")"));
				}
		| SUBSTRING '(' substr_list ')'
				{
					$$ = make3_str(make1_str("substring ("), $3, make1_str(")"));
				}
		/* various trim expressions are defined in SQL92 - thomas 1997-07-19 */
		| TRIM '(' BOTH trim_list ')'
				{
					$$ = make3_str(make1_str("trim(both"), $4, make1_str(")"));
				}
		| TRIM '(' LEADING trim_list ')'
				{
					$$ = make3_str(make1_str("trim(leading"), $4, make1_str(")"));
				}
		| TRIM '(' TRAILING trim_list ')'
				{
					$$ = make3_str(make1_str("trim(trailing"), $4, make1_str(")"));
				}
		| TRIM '(' trim_list ')'
				{
					$$ = make3_str(make1_str("trim("), $3, make1_str(")"));
				}
		| civariableonly
			        { $$ = make1_str(";;"); }
		;

opt_indirection:  '[' c_expr ']' opt_indirection
				{
					$$ = cat4_str(make1_str("["), $2, make1_str("]"), $4);
				}
		| '[' c_expr ':' c_expr ']' opt_indirection
				{
					$$ = cat2_str(cat5_str(make1_str("["), $2, make1_str(":"), $4, make1_str("]")), $6);
				}
		| /* EMPTY */
				{	$$ = make1_str(""); }
		;

expr_list:  a_expr_or_null
				{ $$ = $1; }
		| expr_list ',' a_expr_or_null
				{ $$ = cat3_str($1, make1_str(","), $3); }
		| expr_list USING a_expr
				{ $$ = cat3_str($1, make1_str("using"), $3); }
		;

extract_list:  extract_arg FROM a_expr
				{
					$$ = cat3_str($1, make1_str("from"), $3);
				}
		| /* EMPTY */
				{	$$ = make1_str(""); }
		| cinputvariable
			        { $$ = make1_str(";;"); }
		;

extract_arg:  datetime		{ $$ = $1; }
	| TIMEZONE_HOUR 	{ $$ = make1_str("timezone_hour"); }	
	| TIMEZONE_MINUTE 	{ $$ = make1_str("timezone_minute"); }	
		;

position_list:  position_expr IN position_expr
				{	$$ = cat3_str($1, make1_str("in"), $3); }
		| /* EMPTY */
				{	$$ = make1_str(""); }
		;

position_expr:  attr opt_indirection
				{
					$$ = cat2_str($1, $2);
				}
		| AexprConst
				{	$$ = $1;  }
		| '-' position_expr %prec UMINUS
				{	$$ = cat2_str(make1_str("-"), $2); }
		| position_expr '+' position_expr
				{	$$ = cat3_str($1, make1_str("+"), $3); }
		| position_expr '-' position_expr
				{	$$ = cat3_str($1, make1_str("-"), $3); }
		| position_expr '/' position_expr
				{	$$ = cat3_str($1, make1_str("/"), $3); }
		| position_expr '*' position_expr
				{	$$ = cat3_str($1, make1_str("*"), $3); }
		| '|' position_expr
				{	$$ = cat2_str(make1_str("|"), $2); }
		| position_expr TYPECAST Typename
				{
					$$ = cat3_str($1, make1_str("::"), $3);
				}
		| CAST '(' position_expr AS Typename ')'
				{
					$$ = cat3_str(make2_str(make1_str("cast("), $3), make1_str("as"), make2_str($5, make1_str(")")));
				}
		| '(' position_expr ')'
				{	$$ = make3_str(make1_str("("), $2, make1_str(")")); }
		| position_expr Op position_expr
				{	$$ = cat3_str($1, $2, $3); }
		| Op position_expr
				{	$$ = cat2_str($1, $2); }
		| position_expr Op
				{	$$ = cat2_str($1, $2); }
		| ColId
				{
					$$ = $1;
				}
		| func_name '(' ')'
				{
					$$ = cat2_str($1, make1_str("()"));
				}
		| func_name '(' expr_list ')'
				{
					$$ = make4_str($1, make1_str("("), $3, make1_str(")"));
				}
		| POSITION '(' position_list ')'
				{
					$$ = make3_str(make1_str("position("), $3, make1_str(")"));
				}
		| SUBSTRING '(' substr_list ')'
				{
					$$ = make3_str(make1_str("substring("), $3, make1_str(")"));
				}
		/* various trim expressions are defined in SQL92 - thomas 1997-07-19 */
		| TRIM '(' BOTH trim_list ')'
				{
					$$ = make3_str(make1_str("trim(both"), $4, make1_str(")"));
				}
		| TRIM '(' LEADING trim_list ')'
				{
					$$ = make3_str(make1_str("trim(leading"), $4, make1_str(")"));
				}
		| TRIM '(' TRAILING trim_list ')'
				{
					$$ = make3_str(make1_str("trim(trailing"), $4, make1_str(")"));
				}
		| TRIM '(' trim_list ')'
				{
					$$ = make3_str(make1_str("trim("), $3, make1_str(")"));
				}
		;

substr_list:  expr_list substr_from substr_for
				{
					$$ = cat3_str($1, $2, $3);
				}
		| /* EMPTY */
				{	$$ = make1_str(""); }
		;

substr_from:  FROM expr_list
				{	$$ = cat2_str(make1_str("from"), $2); }
		| /* EMPTY */
				{
					$$ = make1_str("");
				}
		;

substr_for:  FOR expr_list
				{	$$ = cat2_str(make1_str("for"), $2); }
		| /* EMPTY */
				{	$$ = make1_str(""); }
		;

trim_list:  a_expr FROM expr_list
				{ $$ = cat3_str($1, make1_str("from"), $3); }
		| FROM expr_list
				{ $$ = cat2_str(make1_str("from"), $2); }
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
				{	$$ = cat3_str($1, make1_str(","), $3);}
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
				{	$$ = cat3_str($1, make1_str(","), $3);}
		;

attr:  relation_name '.' attrs
				{
					$$ = make3_str($1, make1_str("."), $3);
				}
		| ParamNo '.' attrs
				{
					$$ = make3_str($1, make1_str("."), $3);
				}
		;

attrs:	  attr_name
				{ $$ = $1; }
		| attrs '.' attr_name
				{ $$ = make3_str($1, make1_str("."), $3); }
		| attrs '.' '*'
				{ $$ = make2_str($1, make1_str(".*")); }
		;


/*****************************************************************************
 *
 *	target lists
 *
 *****************************************************************************/

res_target_list:  res_target_list ',' res_target_el
				{	$$ = cat3_str($1, make1_str(","),$3);  }
		| res_target_el
				{	$$ = $1;  }
		| '*'		{ $$ = make1_str("*"); }
		;

res_target_el:  ColId opt_indirection '=' a_expr_or_null
				{
					$$ = cat4_str($1, $2, make1_str("="), $4);
				}
		| attr opt_indirection
				{
					$$ = cat2_str($1, $2);
				}
		| relation_name '.' '*'
				{
					$$ = make2_str($1, make1_str(".*"));
				}
		;

/*
** target list for select.
** should get rid of the other but is still needed by the defunct select into
** and update (uses a subset)
*/
res_target_list2:  res_target_list2 ',' res_target_el2
				{	$$ = cat3_str($1, make1_str(","), $3);  }
		| res_target_el2
				{	$$ = $1;  }
		;

/* AS is not optional because shift/red conflict with unary ops */
res_target_el2:  a_expr_or_null AS ColLabel
				{
					$$ = cat3_str($1, make1_str("as"), $3);
				}
		| a_expr_or_null
				{
					$$ = $1;
				}
		| relation_name '.' '*'
				{
					$$ = make2_str($1, make1_str(".*"));
				}
		| '*'
				{
					$$ = make1_str("*");
				}
		;

opt_id:  ColId									{ $$ = $1; }
		| /* EMPTY */							{ $$ = make1_str(""); }
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
						sprintf(errortext, make1_str("%s cannot be accessed by users"),$1);
						yyerror(errortext);
					}
					else
						$$ = $1;
				}
		;

database_name:			ColId			{ $$ = $1; };
access_method:			ident			{ $$ = $1; };
attr_name:				ColId			{ $$ = $1; };
class:					ident			{ $$ = $1; };
index_name:				ColId			{ $$ = $1; };

/* Functions
 * Include date/time keywords as SQL92 extension.
 * Include TYPE as a SQL92 unreserved keyword. - thomas 1997-10-05
 */
name:					ColId			{ $$ = $1; };
func_name:				ColId			{ $$ = $1; };

file_name:				Sconst			{ $$ = $1; };
recipe_name:			ident			{ $$ = $1; };

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
					$$ = cat2_str($1, $2);
				}
		| ParamNo
				{	$$ = $1;  }
		| TRUE_P
				{
					$$ = make1_str("true");
				}
		| FALSE_P
				{
					$$ = make1_str("false");
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
							free($1);
						}
UserId:  ident                                  { $$ = $1;};

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
ColId:  ident							{ $$ = $1; }
		| datetime						{ $$ = $1; }
		| ACTION						{ $$ = make1_str("action"); }
		| CACHE							{ $$ = make1_str("cache"); }
		| CYCLE							{ $$ = make1_str("cycle"); }
		| DATABASE						{ $$ = make1_str("database"); }
		| DELIMITERS					{ $$ = make1_str("delimiters"); }
		| DOUBLE						{ $$ = make1_str("double"); }
		| EACH							{ $$ = make1_str("each"); }
		| FUNCTION						{ $$ = make1_str("function"); }
		| INCREMENT						{ $$ = make1_str("increment"); }
		| INDEX							{ $$ = make1_str("index"); }
		| KEY							{ $$ = make1_str("key"); }
		| LANGUAGE						{ $$ = make1_str("language"); }
		| LOCATION						{ $$ = make1_str("location"); }
		| MATCH							{ $$ = make1_str("match"); }
		| MAXVALUE						{ $$ = make1_str("maxvalue"); }
		| MINVALUE						{ $$ = make1_str("minvalue"); }
		| OPERATOR						{ $$ = make1_str("operator"); }
		| OPTION						{ $$ = make1_str("option"); }
		| PASSWORD						{ $$ = make1_str("password"); }
		| PRIVILEGES					{ $$ = make1_str("privileges"); }
		| RECIPE						{ $$ = make1_str("recipe"); }
		| ROW							{ $$ = make1_str("row"); }
		| START							{ $$ = make1_str("start"); }
		| STATEMENT						{ $$ = make1_str("statement"); }
		| TIME							{ $$ = make1_str("time"); }
		| TIMEZONE_HOUR                                 { $$ = make1_str("timezone_hour"); }
                | TIMEZONE_MINUTE                               { $$ = make1_str("timezone_minute"); }
		| TRIGGER						{ $$ = make1_str("trigger"); }
		| TYPE_P						{ $$ = make1_str("type"); }
		| VALID							{ $$ = make1_str("valid"); }
		| VERSION						{ $$ = make1_str("version"); }
		| ZONE							{ $$ = make1_str("zone"); }
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
		| ARCHIVE						{ $$ = make1_str("archive"); }
		| CLUSTER						{ $$ = make1_str("cluster"); }
		| CONSTRAINT					{ $$ = make1_str("constraint"); }
		| CROSS							{ $$ = make1_str("cross"); }
		| FOREIGN						{ $$ = make1_str("foreign"); }
		| GROUP							{ $$ = make1_str("group"); }
		| LOAD							{ $$ = make1_str("load"); }
		| ORDER							{ $$ = make1_str("order"); }
		| POSITION						{ $$ = make1_str("position"); }
		| PRECISION						{ $$ = make1_str("precision"); }
		| TABLE							{ $$ = make1_str("table"); }
		| TRANSACTION					{ $$ = make1_str("transaction"); }
		| TRUE_P						{ $$ = make1_str("true"); }
		| FALSE_P						{ $$ = make1_str("false"); }
		;

SpecialRuleRelation:  CURRENT
				{
					if (QueryIsRule)
						$$ = make1_str("current");
					else
						yyerror("CURRENT used in non-rule query");
				}
		| NEW
				{
					if (QueryIsRule)
						$$ = make1_str("new");
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

variable_declarations: /* empty */
	| declaration variable_declarations;

declaration: storage_clause type
	{
		actual_storage[struct_level] = $1;
		actual_type[struct_level] = $2.type_enum;
		if ($2.type_enum != ECPGt_varchar && $2.type_enum != ECPGt_struct)
			fprintf(yyout, "%s %s", $1, $2.type_str);
		free($2.type_str);
	}
	variable_list ';' { fputc(';', yyout); }

storage_clause : S_EXTERN	{ $$ = "extern"; }
       | S_STATIC		{ $$ = "static"; }
       | S_SIGNED		{ $$ = "signed"; }
       | S_CONST		{ $$ = "const"; }
       | S_REGISTER		{ $$ = "register"; }
       | S_AUTO			{ $$ = "auto"; }
       | /* empty */		{ $$ = ""; }

type: simple_type
		{
			$$.type_enum = $1;
			$$.type_str = strdup(ECPGtype_name($1));
		}
	| struct_type
		{
			$$.type_enum = ECPGt_struct;
			$$.type_str = make1_str("");
		}
	| enum_type
		{
			$$.type_str = $1;
			$$.type_enum = ECPGt_int;
		}

enum_type: s_enum '{' c_line '}'
	{
		$$ = cat4_str($1, make1_str("{"), $3, make1_str("}"));
	}
	
s_enum: S_ENUM opt_symbol	{ $$ = cat2_str(make1_str("enum"), $2); }

struct_type: s_struct '{' variable_declarations '}'
	{
	    ECPGfree_struct_member(struct_member_list[struct_level]);
	    free(actual_storage[struct_level--]);
	    fputs("} ", yyout);
	}

s_struct : S_STRUCT opt_symbol
        {
            struct_member_list[struct_level++] = NULL;
            if (struct_level >= STRUCT_DEPTH)
                 yyerror("Too many levels in nested structure definition");
	    fprintf(yyout, "struct %s {", $2);
	    free($2);
	}

opt_symbol: /* empty */ 	{ $$ = make1_str(""); }
	| symbol		{ $$ = $1; }

simple_type: S_SHORT		{ $$ = ECPGt_short; }
           | S_UNSIGNED S_SHORT { $$ = ECPGt_unsigned_short; }
	   | S_INT 		{ $$ = ECPGt_int; }
           | S_UNSIGNED S_INT	{ $$ = ECPGt_unsigned_int; }
	   | S_LONG		{ $$ = ECPGt_long; }
           | S_UNSIGNED S_LONG	{ $$ = ECPGt_unsigned_long; }
           | S_FLOAT		{ $$ = ECPGt_float; }
           | S_DOUBLE		{ $$ = ECPGt_double; }
	   | S_BOOL		{ $$ = ECPGt_bool; };
	   | S_CHAR		{ $$ = ECPGt_char; }
           | S_UNSIGNED S_CHAR	{ $$ = ECPGt_unsigned_char; }
	   | S_VARCHAR		{ $$ = ECPGt_varchar; }

variable_list: variable 
	| variable_list ','
	{
		if (actual_type[struct_level] != ECPGt_varchar)
			fputs(", ", yyout);
		else
			fputs(";\n ", yyout);
	} variable

variable: opt_pointer symbol opt_array_bounds opt_initializer
		{
			struct ECPGtype * type;
                        int dimension = $3.index1; /* dimension of array */
                        int length = $3.index2;    /* lenght of string */
                        char dim[14L];

			switch (actual_type[struct_level])
			{
			   case ECPGt_struct:
			       /* pointer has to get dimension 0 */
        	               if (strlen($1) > 0)
			       {
				    length = dimension;
                	            dimension = 0;
			       }

                               if (length >= 0)
                                   yyerror("No multi-dimensional array support for structures");

                               if (dimension == 1 || dimension < 0)
                                   type = ECPGmake_struct_type(struct_member_list[struct_level]);
                               else
                                   type = ECPGmake_array_type(ECPGmake_struct_type(struct_member_list[struct_level]), dimension); 

                               fprintf(yyout, "%s%s%s%s", $1, $2, $3.str, $4);
                               break;
                           case ECPGt_varchar:
			       /* pointer has to get length 0 */
        	               if (strlen($1) > 0)
                	            length=0;

                               /* one index is the string length */
                               if (length < 0)
                               {
                                   length = dimension;
                                   dimension = 1;
                               }

                               if (dimension == 1)
                                   type = ECPGmake_simple_type(actual_type[struct_level], length);
                               else
                                   type = ECPGmake_array_type(ECPGmake_simple_type(actual_type[struct_level], length), dimension);

                               switch(dimension)
                               {
                                  case 0:
                                      strcpy("[]", dim);
                                      break;
                                  case 1:
                                      *dim = '\0';
                                      break;
                                  default:
                                      sprintf(dim, "[%d]", dimension);
                                      break;
                                }
                               if (length > 0)
                                   fprintf(yyout, "%s struct varchar_%s { int len; char arr[%d]; } %s%s", actual_storage[struct_level], $2, length, $2, dim);
                               else
                                   fprintf(yyout, "%s struct varchar_%s { int len; char *arr; } %s%s", actual_storage[struct_level], $2, $2, dim);
                               break;
                           case ECPGt_char:
                           case ECPGt_unsigned_char:
			       /* pointer has to get length 0 */
        	               if (strlen($1) > 0)
                	            length=0;

                               /* one index is the string length */
                               if (length < 0)
                               {
                                   length = (dimension < 0) ? 1 : dimension;
                                   dimension = 1;
                               }

                               if (dimension == 1)
                                   type = ECPGmake_simple_type(actual_type[struct_level], length);
                               else
                                   type = ECPGmake_array_type(ECPGmake_simple_type(actual_type[struct_level], length), dimension);

                               fprintf(yyout, "%s%s%s%s", $1, $2, $3.str, $4);
                               break;
                           default:
			       /* a pointer has dimension = 0 */
        	               if (strlen($1) > 0) {
                	            length = dimension;
				    dimension = 0;
			       }

                               if (length >= 0)
                                   yyerror("No multi-dimensional array support for simple data types");

                               if (dimension == 1 || dimension < 0)
                                   type = ECPGmake_simple_type(actual_type[struct_level], 1);
                               else
                                   type = ECPGmake_array_type(ECPGmake_simple_type(actual_type[struct_level], 1), dimension);

                               fprintf(yyout, "%s%s%s%s", $1, $2, $3.str, $4);
                               break;
			}

			if (struct_level == 0)
				new_variable($2, type);
			else
				ECPGmake_struct_member($2, type, &(struct_member_list[struct_level - 1]));

			free($1);
			free($2);
			free($3.str);
			free($4);
		}

opt_initializer: /* empty */		{ $$ = make1_str(""); }
	| '=' vartext			{ $$ = make2_str(make1_str("="), $2); }

opt_pointer: /* empty */	{ $$ = make1_str(""); }
	| '*'			{ $$ = make1_str("*"); }

/*
 * the exec sql connect statement: connect to the given database 
 */
ECPGConnect: SQL_CONNECT TO connection_target opt_connection_name opt_user
		{
			$$ = make5_str($3, make1_str(","), $5, make1_str(","), $4);
                }
	| SQL_CONNECT TO DEFAULT
        	{
                	$$ = make1_str("NULL,NULL,NULL,\"DEFAULT\"");
                }
      /* also allow ORACLE syntax */
        | SQL_CONNECT ora_user
                {
		       $$ = make3_str(make1_str("NULL,"), $2, make1_str(",NULL"));
		}

connection_target: database_name opt_server opt_port
                {
		  /* old style: dbname[@server][:port] */
		  if (strlen($2) > 0 && *($2) != '@')
		  {
		    sprintf(errortext, "parse error at or near '%s'", $2);
		    yyerror(errortext);
		  }

		  $$ = make5_str(make1_str("\""), $1, $2, $3, make1_str("\""));
		}
        |  db_prefix server opt_port '/' database_name opt_options
                {
		  /* new style: esql:postgresql://server[:port][/dbname] */
                  if (strncmp($2, "://", 3) != 0)
		  {
		    sprintf(errortext, "parse error at or near '%s'", $2);
		    yyerror(errortext);
		  }
	
		  $$ = make4_str(make5_str(make1_str("\""), $1, $2, $3, make1_str("/")), $5, $6, make1_str("\""));
		}
	| char_variable
                {
		  $$ = $1;
		}
	| Sconst
		{
		  $$ = strdup($1);
		  $$[0] = '\"';
		  $$[strlen($$) - 1] = '\"';
		  free($1);
		}

db_prefix: ident cvariable
                {
		  if (strcmp($2, "postgresql") != 0 && strcmp($2, "postgres") != 0)
		  {
		    sprintf(errortext, "parse error at or near '%s'", $2);
		    yyerror(errortext);	
		  }

		  if (strcmp($1, "esql") != 0 && strcmp($1, "ecpg") != 0 && strcmp($1, "sql") != 0 && strcmp($1, "isql") != 0 && strcmp($1, "proc") != 0)
		  {
		    sprintf(errortext, "Illegal connection type %s", $1);
		    yyerror(errortext);
		  }

		  $$ = make3_str($1, make1_str(":"), $2);
		}
        
server: Op server_name
                {
		  if (strcmp($1, "@") != 0 && strcmp($1, "://") != 0)
		  {
		    sprintf(errortext, "parse error at or near '%s'", $1);
		    yyerror(errortext);
		  }

		  $$ = make2_str($1, $2);
	        }

opt_server: server { $$ = $1; }
        | /* empty */ { $$ = make1_str(""); }

server_name: ColId   { $$ = $1; }
        | ColId '.' server_name { $$ = make3_str($1, make1_str("."), $3); }

opt_port: ':' Iconst { $$ = make2_str(make1_str(":"), $2); }
        | /* empty */ { $$ = make1_str(""); }

opt_connection_name: AS connection_target { $$ = $2; }
        | /* empty */ { $$ = make1_str("NULL"); }

opt_user: USER ora_user { $$ = $2; }
          | /* empty */ { $$ = make1_str("NULL,NULL"); }

ora_user: user_name
		{
                        $$ = make2_str($1, make1_str(",NULL"));
	        }
	| user_name '/' ColId
		{
        		$$ = make3_str($1, make1_str(","), $3);
                }
        | user_name SQL_IDENTIFIED BY user_name
                {
        		$$ = make3_str($1, make1_str(","), $4);
                }
        | user_name USING user_name
                {
        		$$ = make3_str($1, make1_str(","), $3);
                }

user_name: UserId       { if ($1[0] == '\"')
				$$ = $1;
			  else
				$$ = make3_str(make1_str("\""), $1, make1_str("\""));
			}
        | char_variable { $$ = $1; }
        | SCONST        { $$ = make3_str(make1_str("\""), $1, make1_str("\"")); }

char_variable: cvariable
		{ /* check if we have a char variable */
			struct variable *p = find_variable($1);
			enum ECPGttype typ = p->type->typ;

			/* if array see what's inside */
			if (typ == ECPGt_array)
				typ = p->type->u.element->typ;

                        switch (typ)
                        {
                            case ECPGt_char:
                            case ECPGt_unsigned_char:
                                $$ = $1;
                                break;
                            case ECPGt_varchar:
                                $$ = make2_str($1, make1_str(".arr"));
                                break;
                            default:
                                yyerror("invalid datatype");
                                break;
                        }
		}

opt_options: Op ColId
		{
			if (strlen($1) == 0)
				yyerror("parse error");
				
			if (strcmp($1, "?") != 0)
			{
				sprintf(errortext, "parse error at or near %s", $1);
				yyerror(errortext);
			}
			
			$$ = make2_str(make1_str("?"), $2);
		}
	| /* empty */ { $$ = make1_str(""); }

/*
 * the exec sql disconnect statement: disconnect from the given database 
 */
ECPGDisconnect: SQL_DISCONNECT dis_name { $$ = $2; }

dis_name: connection_object	{ $$ = $1; }
	| CURRENT	{ $$ = make1_str("CURRENT"); }
	| ALL		{ $$ = make1_str("ALL"); }
	| /* empty */	{ $$ = make1_str("CURRENT"); }

connection_object: connection_target { $$ = $1; }
	| DEFAULT	{ $$ = make1_str("DEFAULT"); }

/*
 * execute a given string as sql command
 */
ECPGExecute : EXECUTE SQL_IMMEDIATE execstring { $$ = $3; };

execstring: cvariable |
	CSTRING	 { $$ = make3_str(make1_str("\""), $1, make1_str("\"")); };

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

open_opts: /* empty */		{ $$ = make1_str(""); }
	| USING cvariable	{
					yyerror ("open cursor with variables not implemented yet");
				}

/*
 * for compatibility with ORACLE we will also allow the keyword RELEASE
 * after a transaction statement to disconnect from the database.
 */

ECPGRelease: TransactionStmt SQL_RELEASE
	{
		if (strncmp($1, "begin", 5) == 0)
                        yyerror("RELEASE does not make sense when beginning a transaction");

		fprintf(yyout, "ECPGtrans(__LINE__, \"%s\");", $1);
		whenever_action(0);
		fprintf(yyout, "ECPGdisconnect(\"\");"); 
		whenever_action(0);
		free($1);
	}

/* 
 * set the actual connection, this needs a differnet handling as the other
 * set commands
 */
ECPGSetConnection:  SET SQL_CONNECTION connection_object
           		{
				$$ = $3;
                        }
/*
 * whenever statement: decide what to do in case of error/no data found
 * according to SQL standards we miss: SQLSTATE, CONSTRAINT, SQLEXCEPTION
 * and SQLWARNING

 */
ECPGWhenever: SQL_WHENEVER SQL_SQLERROR action {
	when_error.code = $<action>3.code;
	when_error.command = $<action>3.command;
	$$ = cat3_str(make1_str("/* exec sql whenever sqlerror "), $3.str, make1_str("; */\n"));
}
	| SQL_WHENEVER NOT SQL_FOUND action {
	when_nf.code = $<action>4.code;
	when_nf.command = $<action>4.command;
	$$ = cat3_str(make1_str("/* exec sql whenever not found "), $4.str, make1_str("; */\n"));
}

action : SQL_CONTINUE {
	$<action>$.code = W_NOTHING;
	$<action>$.command = NULL;
	$<action>$.str = make1_str("continue");
}
       | SQL_SQLPRINT {
	$<action>$.code = W_SQLPRINT;
	$<action>$.command = NULL;
	$<action>$.str = make1_str("sqlprint");
}
       | SQL_STOP {
	$<action>$.code = W_STOP;
	$<action>$.command = NULL;
	$<action>$.str = make1_str("stop");
}
       | SQL_GOTO name {
        $<action>$.code = W_GOTO;
        $<action>$.command = $2;
	$<action>$.str = cat2_str(make1_str("goto "), $2);
}
       | SQL_GO TO name {
        $<action>$.code = W_GOTO;
        $<action>$.command = $3;
	$<action>$.str = cat2_str(make1_str("goto "), $3);
}
       | DO name '(' dotext ')' {
	$<action>$.code = W_DO;
	$<action>$.command = make4_str($2, make1_str("("), $4, make1_str(")"));
	$<action>$.str = cat2_str(make1_str("do"), strdup($<action>$.command));
}
       | DO SQL_BREAK {
        $<action>$.code = W_BREAK;
        $<action>$.command = NULL;
        $<action>$.str = make1_str("break");
}
       | SQL_CALL name '(' dotext ')' {
	$<action>$.code = W_DO;
	$<action>$.command = make4_str($2, make1_str("("), $4, make1_str(")"));
	$<action>$.str = cat2_str(make1_str("call"), strdup($<action>$.command));
}

/* some other stuff for ecpg */

c_expr:  attr opt_indirection
				{
					$$ = cat2_str($1, $2);
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
				{	$$ = cat2_str(make1_str("-"), $2); }
		| a_expr '+' c_expr
				{	$$ = cat3_str($1, make1_str("+"), $3); }
		| a_expr '-' c_expr
				{	$$ = cat3_str($1, make1_str("-"), $3); }
		| a_expr '/' c_expr
				{	$$ = cat3_str($1, make1_str("/"), $3); }
		| a_expr '*' c_expr
				{	$$ = cat3_str($1, make1_str("*"), $3); }
		| a_expr '<' c_expr
				{	$$ = cat3_str($1, make1_str("<"), $3); }
		| a_expr '>' c_expr
				{	$$ = cat3_str($1, make1_str(">"), $3); }
		| a_expr '=' c_expr
				{	$$ = cat3_str($1, make1_str("="), $3); }
	/*	| ':' c_expr
				{	$$ = cat2_str(make1_str(":"), $2); }*/
		| ';' c_expr
				{	$$ = cat2_str(make1_str(";"), $2); }
		| '|' c_expr
				{	$$ = cat2_str(make1_str("|"), $2); }
		| a_expr TYPECAST Typename
				{
					$$ = cat3_str($1, make1_str("::"), $3);
				}
		| CAST '(' a_expr AS Typename ')'
				{
					$$ = cat3_str(make2_str(make1_str("cast("), $3), make1_str("as"), make2_str($5, make1_str(")")));
				}
		| '(' a_expr_or_null ')'
				{	$$ = make3_str(make1_str("("), $2, make1_str(")")); }
		| a_expr Op c_expr
				{	$$ = cat3_str($1, $2, $3);	}
		| a_expr LIKE c_expr
				{	$$ = cat3_str($1, make1_str("like"), $3); }
		| a_expr NOT LIKE c_expr
				{	$$ = cat3_str($1, make1_str("not like"), $4); }
		| Op c_expr
				{	$$ = cat2_str($1, $2); }
		| a_expr Op
				{	$$ = cat2_str($1, $2); }
		| func_name '(' '*' ')'
				{
					$$ = cat2_str($1, make1_str("(*)")); 
				}
		| func_name '(' ')'
				{
					$$ = cat2_str($1, make1_str("()")); 
				}
		| func_name '(' expr_list ')'
				{
					$$ = make4_str($1, make1_str("("), $3, make1_str(")")); 
				}
		| CURRENT_DATE
				{
					$$ = make1_str("current_date");
				}
		| CURRENT_TIME
				{
					$$ = make1_str("current_time");
				}
		| CURRENT_TIME '(' Iconst ')'
				{
					if (atol($3) != 0)
						fprintf(stderr,"CURRENT_TIME(%s) precision not implemented; zero used instead", $3);
					$$ = make1_str("current_time");
				}
		| CURRENT_TIMESTAMP
				{
					$$ = make1_str("current_timestamp");
				}
		| CURRENT_TIMESTAMP '(' Iconst ')'
				{
					if (atol($3) != 0)
						fprintf(stderr,"CURRENT_TIMESTAMP(%s) precision not implemented; zero used instead",$3);
					$$ = make1_str("current_timestamp");
				}
		| CURRENT_USER
				{
					$$ = make1_str("current_user");
				}
		| EXISTS '(' SubSelect ')'
				{
					$$ = make3_str(make1_str("exists("), $3, make1_str(")"));
				}
		| EXTRACT '(' extract_list ')'
				{
					$$ = make3_str(make1_str("extract("), $3, make1_str(")"));
				}
		| POSITION '(' position_list ')'
				{
					$$ = make3_str(make1_str("position("), $3, make1_str(")"));
				}
		| SUBSTRING '(' substr_list ')'
				{
					$$ = make3_str(make1_str("substring("), $3, make1_str(")"));
				}
		/* various trim expressions are defined in SQL92 - thomas 1997-07-19 */
		| TRIM '(' BOTH trim_list ')'
				{
					$$ = make3_str(make1_str("trim(both"), $4, make1_str(")"));
				}
		| TRIM '(' LEADING trim_list ')'
				{
					$$ = make3_str(make1_str("trim(leading"), $4, make1_str(")"));
				}
		| TRIM '(' TRAILING trim_list ')'
				{
					$$ = make3_str(make1_str("trim(trailing"), $4, make1_str(")"));
				}
		| TRIM '(' trim_list ')'
				{
					$$ = make3_str(make1_str("trim("), $3, make1_str(")"));
				}
		| a_expr ISNULL
				{	$$ = cat2_str($1, make1_str("isnull")); }
		| a_expr IS NULL_P
				{	$$ = cat2_str($1, make1_str("is null")); }
		| a_expr NOTNULL
				{	$$ = cat2_str($1, make1_str("notnull")); }
		| a_expr IS NOT NULL_P
				{	$$ = cat2_str($1, make1_str("is not null")); }
		/* IS TRUE, IS FALSE, etc used to be function calls
		 *  but let's make them expressions to allow the optimizer
		 *  a chance to eliminate them if a_expr is a constant string.
		 * - thomas 1997-12-22
		 */
		| a_expr IS TRUE_P
				{
				{	$$ = cat2_str($1, make1_str("is true")); }
				}
		| a_expr IS NOT FALSE_P
				{
				{	$$ = cat2_str($1, make1_str("is not false")); }
				}
		| a_expr IS FALSE_P
				{
				{	$$ = cat2_str($1, make1_str("is false")); }
				}
		| a_expr IS NOT TRUE_P
				{
				{	$$ = cat2_str($1, make1_str("is not true")); }
				}
		| a_expr BETWEEN b_expr AND b_expr
				{
					$$ = cat5_str($1, make1_str("between"), $3, make1_str("and"), $5); 
				}
		| a_expr NOT BETWEEN b_expr AND b_expr
				{
					$$ = cat5_str($1, make1_str("not between"), $4, make1_str("and"), $6); 
				}
		| a_expr IN '(' in_expr ')'
				{
					$$ = make4_str($1, make1_str("in ("), $4, make1_str(")")); 
				}
		| a_expr NOT IN '(' not_in_expr ')'
				{
					$$ = make4_str($1, make1_str("not in ("), $5, make1_str(")")); 
				}
		| a_expr Op '(' SubSelect ')'
				{
					$$ = cat3_str($1, $2, make3_str(make1_str("("), $4, make1_str(")"))); 
				}
		| a_expr '+' '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("+("), $4, make1_str(")")); 
				}
		| a_expr '-' '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("-("), $4, make1_str(")")); 
				}
		| a_expr '/' '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("/("), $4, make1_str(")")); 
				}
		| a_expr '*' '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("*("), $4, make1_str(")")); 
				}
		| a_expr '<' '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("<("), $4, make1_str(")")); 
				}
		| a_expr '>' '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str(">("), $4, make1_str(")")); 
				}
		| a_expr '=' '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("=("), $4, make1_str(")")); 
				}
		| a_expr Op ANY '(' SubSelect ')'
				{
					$$ = cat3_str($1, $2, make3_str(make1_str("any ("), $5, make1_str(")"))); 
				}
		| a_expr '+' ANY '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("+any("), $5, make1_str(")")); 
				}
		| a_expr '-' ANY '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("-any("), $5, make1_str(")")); 
				}
		| a_expr '/' ANY '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("/any("), $5, make1_str(")")); 
				}
		| a_expr '*' ANY '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("*any("), $5, make1_str(")")); 
				}
		| a_expr '<' ANY '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("<any("), $5, make1_str(")")); 
				}
		| a_expr '>' ANY '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str(">any("), $5, make1_str(")")); 
				}
		| a_expr '=' ANY '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("=any("), $5, make1_str(")")); 
				}
		| a_expr Op ALL '(' SubSelect ')'
				{
					$$ = make3_str($1, $2, make3_str(make1_str("all ("), $5, make1_str(")"))); 
				}
		| a_expr '+' ALL '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("+all("), $5, make1_str(")")); 
				}
		| a_expr '-' ALL '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("-all("), $5, make1_str(")")); 
				}
		| a_expr '/' ALL '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("/all("), $5, make1_str(")")); 
				}
		| a_expr '*' ALL '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("*all("), $5, make1_str(")")); 
				}
		| a_expr '<' ALL '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("<all("), $5, make1_str(")")); 
				}
		| a_expr '>' ALL '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str(">all("), $5, make1_str(")")); 
				}
		| a_expr '=' ALL '(' SubSelect ')'
				{
					$$ = make4_str($1, make1_str("=all("), $5, make1_str(")")); 
				}
		| a_expr AND c_expr
				{	$$ = cat3_str($1, make1_str("and"), $3); }
		| a_expr OR c_expr
				{	$$ = cat3_str($1, make1_str("or"), $3); }
		| NOT c_expr
				{	$$ = cat2_str(make1_str("not"), $2); }
		| civariableonly
			        { $$ = make1_str(";;"); }
		;

into_list : coutputvariable | into_list ',' coutputvariable;

ecpgstart: SQL_START { reset_variables();}

dotext: /* empty */		{ $$ = make1_str(""); }
	| dotext do_anything	{ $$ = make2_str($1, $2); }

vartext: var_anything		{ $$ = $1; }
        | vartext var_anything { $$ = make2_str($1, $2); }

coutputvariable : cvariable indicator {
		add_variable(&argsresult, find_variable($1), ($2 == NULL) ? &no_indicator : find_variable($2)); 
}

cinputvariable : cvariable indicator {
		add_variable(&argsinsert, find_variable($1), ($2 == NULL) ? &no_indicator : find_variable($2)); 
}

civariableonly : cvariable {
		add_variable(&argsinsert, find_variable($1), &no_indicator); 
}

cvariable: CVARIABLE			{ $$ = $1; }

indicator: /* empty */			{ $$ = NULL; }
	| cvariable		 	{ check_indicator((find_variable($1))->type); $$ = $1; }
	| SQL_INDICATOR cvariable 	{ check_indicator((find_variable($2))->type); $$ = $2; }
	| SQL_INDICATOR name		{ check_indicator((find_variable($2))->type); $$ = $2; }

ident: IDENT	{ $$ = $1; }
	| CSTRING	{ $$ = $1; }
/*
 * C stuff
 */

symbol: IDENT	{ $$ = $1; }

cpp_line: CPP_LINE	{ $$ = $1; }

c_line: c_anything { $$ = $1; }
	| c_line c_anything
		{
			$$ = make2_str($1, $2);
		}

c_thing: c_anything | ';' { $$ = make1_str(";"); }

c_anything:  IDENT 	{ $$ = $1; }
	| CSTRING	{ $$ = make3_str(make1_str("\""), $1, make1_str("\"")); }
	| Iconst	{ $$ = $1; }
	| FCONST	{ $$ = make_name(); }
	| '*'		{ $$ = make1_str("*"); }
	| S_AUTO	{ $$ = make1_str("auto"); }
	| S_BOOL	{ $$ = make1_str("bool"); }
	| S_CHAR	{ $$ = make1_str("char"); }
	| S_CONST	{ $$ = make1_str("const"); }
	| S_DOUBLE	{ $$ = make1_str("double"); }
	| S_EXTERN	{ $$ = make1_str("extern"); }
	| S_FLOAT	{ $$ = make1_str("float"); }
        | S_INT		{ $$ = make1_str("int"); }
	| S_LONG	{ $$ = make1_str("long"); }
	| S_REGISTER	{ $$ = make1_str("register"); }
	| S_SHORT	{ $$ = make1_str("short"); }
	| S_SIGNED	{ $$ = make1_str("signed"); }
	| S_STATIC	{ $$ = make1_str("static"); }
        | S_STRUCT	{ $$ = make1_str("struct"); }
	| S_UNSIGNED	{ $$ = make1_str("unsigned"); }
	| S_VARCHAR	{ $$ = make1_str("varchar"); }
	| S_ANYTHING	{ $$ = make_name(); }
        | '['		{ $$ = make1_str("["); }
	| ']'		{ $$ = make1_str("]"); }
	| '('		{ $$ = make1_str("("); }
	| ')'		{ $$ = make1_str(")"); }
	| '='		{ $$ = make1_str("="); }
	| ','		{ $$ = make1_str(","); }

do_anything: IDENT	{ $$ = $1; }
        | CSTRING       { $$ = make3_str(make1_str("\""), $1, make1_str("\""));}
        | Iconst        { $$ = $1; }
	| FCONST	{ $$ = make_name(); }
	| ','		{ $$ = make1_str(","); }

var_anything: IDENT 		{ $$ = $1; }
	| CSTRING       	{ $$ = make3_str(make1_str("\""), $1, make1_str("\"")); }
	| Iconst		{ $$ = $1; }
	| FCONST		{ $$ = make_name(); }
	| '{' c_line '}'	{ $$ = make3_str(make1_str("{"), $2, make1_str("}")); }

blockstart : '{' {
    braces_open++;
    $$ = make1_str("{");
}

blockend : '}' {
    remove_variables(braces_open--);
    $$ = make1_str("}");
}

%%

void yyerror(char * error)
{
    fprintf(stderr, "%s in line %d of file %s\n", error, yylineno, input_filename);
    exit(PARSE_ERROR);
}
