%{
/*-------------------------------------------------------------------------
 *
 * gram.y				- Parser for the PL/pgSQL procedural language
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/pl/plpgsql/src/gram.y,v 1.133 2009/11/09 00:26:55 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "plpgsql.h"

#include "catalog/pg_type.h"
#include "parser/parser.h"
#include "parser/parse_type.h"
#include "parser/scansup.h"


/*
 * We track token locations in terms of byte offsets from the start of the
 * source string, not the column number/line number representation that
 * bison uses by default.  Also, to minimize overhead we track only one
 * location (usually the first token location) for each construct, not
 * the beginning and ending locations as bison does by default.  It's
 * therefore sufficient to make YYLTYPE an int.
 */
#define YYLTYPE  int

/* Location tracking support --- simpler than bison's default */
#define YYLLOC_DEFAULT(Current, Rhs, N) \
	do { \
		if (N) \
			(Current) = (Rhs)[1]; \
		else \
			(Current) = (Rhs)[0]; \
	} while (0)

/*
 * Bison doesn't allocate anything that needs to live across parser calls,
 * so we can easily have it use palloc instead of malloc.  This prevents
 * memory leaks if we error out during parsing.  Note this only works with
 * bison >= 2.0.  However, in bison 1.875 the default is to use alloca()
 * if possible, so there's not really much problem anyhow, at least if
 * you're building with gcc.
 */
#define YYMALLOC palloc
#define YYFREE   pfree


typedef struct
{
	int			location;
	int			leaderlen;
} sql_error_callback_arg;

#define parser_errposition(pos)  plpgsql_scanner_errposition(pos)

static PLpgSQL_expr		*read_sql_construct(int until,
											int until2,
											int until3,
											const char *expected,
											const char *sqlstart,
											bool isexpression,
											bool valid_sql,
											int *startloc,
											int *endtoken);
static PLpgSQL_expr		*read_sql_expression2(int until, int until2,
											  const char *expected,
											  int *endtoken);
static	PLpgSQL_expr	*read_sql_stmt(const char *sqlstart);
static	PLpgSQL_type	*read_datatype(int tok);
static	PLpgSQL_stmt	*make_execsql_stmt(int firsttoken, int location);
static	PLpgSQL_stmt_fetch *read_fetch_direction(void);
static	void			 complete_direction(PLpgSQL_stmt_fetch *fetch,
											bool *check_FROM);
static	PLpgSQL_stmt	*make_return_stmt(int location);
static	PLpgSQL_stmt	*make_return_next_stmt(int location);
static	PLpgSQL_stmt	*make_return_query_stmt(int location);
static  PLpgSQL_stmt 	*make_case(int location, PLpgSQL_expr *t_expr,
								   List *case_when_list, List *else_stmts);
static	void			 check_assignable(PLpgSQL_datum *datum, int location);
static	void			 read_into_target(PLpgSQL_rec **rec, PLpgSQL_row **row,
										  bool *strict);
static	PLpgSQL_row		*read_into_scalar_list(const char *initial_name,
											   PLpgSQL_datum *initial_datum,
											   int initial_location);
static PLpgSQL_row		*make_scalar_list1(const char *initial_name,
										   PLpgSQL_datum *initial_datum,
										   int lineno, int location);
static	void			 check_sql_expr(const char *stmt, int location,
										int leaderlen);
static	void			 plpgsql_sql_error_callback(void *arg);
static PLpgSQL_type		*parse_datatype(const char *string, int location);
static	char			*parse_string_token(const char *token, int location);
static	char			*check_label(const char *yytxt);
static	void			 check_labels(const char *start_label,
									  const char *end_label,
									  int end_location);
static PLpgSQL_expr 	*read_cursor_args(PLpgSQL_var *cursor,
										  int until, const char *expected);
static List				*read_raise_options(void);

%}

%expect 0
%name-prefix="plpgsql_yy"
%locations

%union {
		int32					ival;
		bool					boolean;
		char					*str;
		struct
		{
			char *name;
			int  lineno;
		}						varname;
		struct
		{
			char *name;
			int  lineno;
			PLpgSQL_datum   *scalar;
			PLpgSQL_rec     *rec;
			PLpgSQL_row     *row;
		}						forvariable;
		struct
		{
			char *label;
			int  n_initvars;
			int  *initvarnos;
		}						declhdr;
		struct
		{
			List *stmts;
			char *end_label;
			int   end_label_location;
		}						loop_body;
		List					*list;
		PLpgSQL_type			*dtype;
		PLpgSQL_datum			*datum;
		PLpgSQL_var				*var;
		PLpgSQL_expr			*expr;
		PLpgSQL_stmt			*stmt;
		PLpgSQL_stmt_block		*program;
		PLpgSQL_condition		*condition;
		PLpgSQL_exception		*exception;
		PLpgSQL_exception_block	*exception_block;
		PLpgSQL_nsitem			*nsitem;
		PLpgSQL_diag_item		*diagitem;
		PLpgSQL_stmt_fetch		*fetch;
		PLpgSQL_case_when		*casewhen;
}

%type <declhdr> decl_sect
%type <varname> decl_varname
%type <boolean>	decl_const decl_notnull exit_type
%type <expr>	decl_defval decl_cursor_query
%type <dtype>	decl_datatype
%type <datum>	decl_cursor_args
%type <list>	decl_cursor_arglist
%type <nsitem>	decl_aliasitem
%type <str>		decl_stmts decl_stmt

%type <expr>	expr_until_semi expr_until_rightbracket
%type <expr>	expr_until_then expr_until_loop opt_expr_until_when
%type <expr>	opt_exitcond

%type <ival>	assign_var
%type <var>		cursor_variable
%type <datum>	decl_cursor_arg
%type <forvariable>	for_variable
%type <stmt>	for_control

%type <str>		any_identifier any_name opt_block_label opt_label

%type <list>	proc_sect proc_stmts stmt_else
%type <loop_body>	loop_body
%type <stmt>	proc_stmt pl_block
%type <stmt>	stmt_assign stmt_if stmt_loop stmt_while stmt_exit
%type <stmt>	stmt_return stmt_raise stmt_execsql
%type <stmt>	stmt_dynexecute stmt_for stmt_perform stmt_getdiag
%type <stmt>	stmt_open stmt_fetch stmt_move stmt_close stmt_null
%type <stmt>	stmt_case

%type <list>	proc_exceptions
%type <exception_block> exception_sect
%type <exception>	proc_exception
%type <condition>	proc_conditions proc_condition

%type <casewhen>	case_when
%type <list>	case_when_list opt_case_else

%type <list>	getdiag_list
%type <diagitem> getdiag_list_item
%type <ival>	getdiag_kind getdiag_target

%type <ival>	opt_scrollable
%type <fetch>   opt_fetch_direction

		/*
		 * Keyword tokens
		 */
%token	K_ALIAS
%token	K_ALL
%token	K_ASSIGN
%token	K_BEGIN
%token	K_BY
%token	K_CASE
%token	K_CLOSE
%token	K_CONSTANT
%token	K_CONTINUE
%token	K_CURSOR
%token	K_DECLARE
%token	K_DEFAULT
%token	K_DIAGNOSTICS
%token	K_DOTDOT
%token	K_ELSE
%token	K_ELSIF
%token	K_END
%token	K_EXCEPTION
%token	K_EXECUTE
%token	K_EXIT
%token	K_FOR
%token	K_FETCH
%token	K_FROM
%token	K_GET
%token	K_IF
%token	K_IN
%token	K_INSERT
%token	K_INTO
%token	K_IS
%token	K_LOOP
%token	K_MOVE
%token	K_NOSCROLL
%token	K_NOT
%token	K_NULL
%token	K_OPEN
%token	K_OR
%token	K_PERFORM
%token	K_ROW_COUNT
%token	K_RAISE
%token	K_RESULT_OID
%token	K_RETURN
%token	K_REVERSE
%token	K_SCROLL
%token	K_STRICT
%token	K_THEN
%token	K_TO
%token	K_TYPE
%token	K_USING
%token	K_WHEN
%token	K_WHILE

		/*
		 * Other tokens
		 */
%token	T_STRING
%token	T_NUMBER
%token	T_DATUM					/* a VAR, ROW, REC, or RECFIELD variable */
%token	T_WORD					/* unrecognized simple identifier */
%token	T_DBLWORD				/* unrecognized ident.ident */
%token	T_TRIPWORD				/* unrecognized ident.ident.ident */

%token	O_OPTION
%token	O_DUMP

%%

pl_function		: comp_optsect pl_block opt_semi
					{
						yylval.program = (PLpgSQL_stmt_block *) $2;
					}
				;

comp_optsect	:
				| comp_options
				;

comp_options	: comp_options comp_option
				| comp_option
				;

comp_option		: O_OPTION O_DUMP
					{
						plpgsql_DumpExecTree = true;
					}
				;

opt_semi		:
				| ';'
				;

pl_block		: decl_sect K_BEGIN proc_sect exception_sect K_END opt_label
					{
						PLpgSQL_stmt_block *new;

						new = palloc0(sizeof(PLpgSQL_stmt_block));

						new->cmd_type	= PLPGSQL_STMT_BLOCK;
						new->lineno		= plpgsql_location_to_lineno(@2);
						new->label		= $1.label;
						new->n_initvars = $1.n_initvars;
						new->initvarnos = $1.initvarnos;
						new->body		= $3;
						new->exceptions	= $4;

						check_labels($1.label, $6, @6);
						plpgsql_ns_pop();

						$$ = (PLpgSQL_stmt *)new;
					}
				;


decl_sect		: opt_block_label
					{
						/* done with decls, so resume identifier lookup */
						plpgsql_LookupIdentifiers = true;
						$$.label	  = $1;
						$$.n_initvars = 0;
						$$.initvarnos = NULL;
					}
				| opt_block_label decl_start
					{
						plpgsql_LookupIdentifiers = true;
						$$.label	  = $1;
						$$.n_initvars = 0;
						$$.initvarnos = NULL;
					}
				| opt_block_label decl_start decl_stmts
					{
						plpgsql_LookupIdentifiers = true;
						if ($3 != NULL)
							$$.label = $3;
						else
							$$.label = $1;
						/* Remember variables declared in decl_stmts */
						$$.n_initvars = plpgsql_add_initdatums(&($$.initvarnos));
					}
				;

decl_start		: K_DECLARE
					{
						/* Forget any variables created before block */
						plpgsql_add_initdatums(NULL);
						/*
						 * Disable scanner lookup of identifiers while
						 * we process the decl_stmts
						 */
						plpgsql_LookupIdentifiers = false;
					}
				;

decl_stmts		: decl_stmts decl_stmt
					{	$$ = $2;	}
				| decl_stmt
					{	$$ = $1;	}
				;

decl_stmt		: '<' '<' any_name '>' '>'
					{	$$ = $3;	}
				| K_DECLARE
					{	$$ = NULL;	}
				| decl_statement
					{	$$ = NULL;	}
				;

decl_statement	: decl_varname decl_const decl_datatype decl_notnull decl_defval
					{
						PLpgSQL_variable	*var;

						var = plpgsql_build_variable($1.name, $1.lineno,
													 $3, true);
						if ($2)
						{
							if (var->dtype == PLPGSQL_DTYPE_VAR)
								((PLpgSQL_var *) var)->isconst = $2;
							else
								ereport(ERROR,
										(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
										 errmsg("row or record variable cannot be CONSTANT"),
										 parser_errposition(@2)));
						}
						if ($4)
						{
							if (var->dtype == PLPGSQL_DTYPE_VAR)
								((PLpgSQL_var *) var)->notnull = $4;
							else
								ereport(ERROR,
										(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
										 errmsg("row or record variable cannot be NOT NULL"),
										 parser_errposition(@4)));

						}
						if ($5 != NULL)
						{
							if (var->dtype == PLPGSQL_DTYPE_VAR)
								((PLpgSQL_var *) var)->default_val = $5;
							else
								ereport(ERROR,
										(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
										 errmsg("default value for row or record variable is not supported"),
										 parser_errposition(@5)));
						}
					}
				| decl_varname K_ALIAS K_FOR decl_aliasitem ';'
					{
						plpgsql_ns_additem($4->itemtype,
										   $4->itemno, $1.name);
					}
				| decl_varname opt_scrollable K_CURSOR
					{ plpgsql_ns_push($1.name); }
				  decl_cursor_args decl_is_for decl_cursor_query
					{
						PLpgSQL_var *new;
						PLpgSQL_expr *curname_def;
						char		buf[1024];
						char		*cp1;
						char		*cp2;

						/* pop local namespace for cursor args */
						plpgsql_ns_pop();

						new = (PLpgSQL_var *)
							plpgsql_build_variable($1.name, $1.lineno,
												   plpgsql_build_datatype(REFCURSOROID,
																		  -1),
												   true);

						curname_def = palloc0(sizeof(PLpgSQL_expr));

						curname_def->dtype = PLPGSQL_DTYPE_EXPR;
						strcpy(buf, "SELECT ");
						cp1 = new->refname;
						cp2 = buf + strlen(buf);
						/*
						 * Don't trust standard_conforming_strings here;
						 * it might change before we use the string.
						 */
						if (strchr(cp1, '\\') != NULL)
							*cp2++ = ESCAPE_STRING_SYNTAX;
						*cp2++ = '\'';
						while (*cp1)
						{
							if (SQL_STR_DOUBLE(*cp1, true))
								*cp2++ = *cp1;
							*cp2++ = *cp1++;
						}
						strcpy(cp2, "'::pg_catalog.refcursor");
						curname_def->query = pstrdup(buf);
						new->default_val = curname_def;

						new->cursor_explicit_expr = $7;
						if ($5 == NULL)
							new->cursor_explicit_argrow = -1;
						else
							new->cursor_explicit_argrow = $5->dno;
						new->cursor_options = CURSOR_OPT_FAST_PLAN | $2;
					}
				;

opt_scrollable :
					{
						$$ = 0;
					}
				| K_NOSCROLL
					{
						$$ = CURSOR_OPT_NO_SCROLL;
					}
				| K_SCROLL
					{
						$$ = CURSOR_OPT_SCROLL;
					}
				;

decl_cursor_query :
					{
						$$ = read_sql_stmt("");
					}
				;

decl_cursor_args :
					{
						$$ = NULL;
					}
				| '(' decl_cursor_arglist ')'
					{
						PLpgSQL_row *new;
						int i;
						ListCell *l;

						new = palloc0(sizeof(PLpgSQL_row));
						new->dtype = PLPGSQL_DTYPE_ROW;
						new->lineno = plpgsql_location_to_lineno(@1);
						new->rowtupdesc = NULL;
						new->nfields = list_length($2);
						new->fieldnames = palloc(new->nfields * sizeof(char *));
						new->varnos = palloc(new->nfields * sizeof(int));

						i = 0;
						foreach (l, $2)
						{
							PLpgSQL_variable *arg = (PLpgSQL_variable *) lfirst(l);
							new->fieldnames[i] = arg->refname;
							new->varnos[i] = arg->dno;
							i++;
						}
						list_free($2);

						plpgsql_adddatum((PLpgSQL_datum *) new);
						$$ = (PLpgSQL_datum *) new;
					}
				;

decl_cursor_arglist : decl_cursor_arg
					{
						$$ = list_make1($1);
					}
				| decl_cursor_arglist ',' decl_cursor_arg
					{
						$$ = lappend($1, $3);
					}
				;

decl_cursor_arg : decl_varname decl_datatype
					{
						$$ = (PLpgSQL_datum *)
							plpgsql_build_variable($1.name, $1.lineno,
												   $2, true);
					}
				;

decl_is_for		:	K_IS |		/* Oracle */
					K_FOR;		/* SQL standard */

decl_aliasitem	: T_WORD
					{
						char	*name[1];
						PLpgSQL_nsitem *nsi;

						plpgsql_convert_ident(yytext, name, 1);

						nsi = plpgsql_ns_lookup(plpgsql_ns_top(), false,
												name[0], NULL, NULL,
												NULL);
						if (nsi == NULL)
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_OBJECT),
									 errmsg("variable \"%s\" does not exist",
											name[0]),
									 parser_errposition(@1)));

						pfree(name[0]);

						$$ = nsi;
					}
				| T_DBLWORD
					{
						char	*name[2];
						PLpgSQL_nsitem *nsi;

						plpgsql_convert_ident(yytext, name, 2);

						nsi = plpgsql_ns_lookup(plpgsql_ns_top(), false,
												name[0], name[1], NULL,
												NULL);
						if (nsi == NULL)
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_OBJECT),
									 errmsg("variable \"%s.%s\" does not exist",
											name[0], name[1]),
									 parser_errposition(@1)));

						pfree(name[0]);
						pfree(name[1]);

						$$ = nsi;
					}
				;

decl_varname	: T_WORD
					{
						char	*name;

						plpgsql_convert_ident(yytext, &name, 1);
						$$.name = name;
						$$.lineno = plpgsql_location_to_lineno(@1);
						/*
						 * Check to make sure name isn't already declared
						 * in the current block.
						 */
						if (plpgsql_ns_lookup(plpgsql_ns_top(), true,
											  name, NULL, NULL,
											  NULL) != NULL)
							yyerror("duplicate declaration");
					}
				;

decl_const		:
					{ $$ = false; }
				| K_CONSTANT
					{ $$ = true; }
				;

decl_datatype	:
					{
						/*
						 * If there's a lookahead token, read_datatype
						 * should consume it.
						 */
						$$ = read_datatype(yychar);
						yyclearin;
					}
				;

decl_notnull	:
					{ $$ = false; }
				| K_NOT K_NULL
					{ $$ = true; }
				;

decl_defval		: ';'
					{ $$ = NULL; }
				| decl_defkey
					{
						$$ = plpgsql_read_expression(';', ";");
					}
				;

decl_defkey		: K_ASSIGN
				| K_DEFAULT
				;

proc_sect		:
					{ $$ = NIL; }
				| proc_stmts
					{ $$ = $1; }
				;

proc_stmts		: proc_stmts proc_stmt
						{
							if ($2 == NULL)
								$$ = $1;
							else
								$$ = lappend($1, $2);
						}
				| proc_stmt
						{
							if ($1 == NULL)
								$$ = NIL;
							else
								$$ = list_make1($1);
						}
				;

proc_stmt		: pl_block ';'
						{ $$ = $1; }
				| stmt_assign
						{ $$ = $1; }
				| stmt_if
						{ $$ = $1; }
				| stmt_case
						{ $$ = $1; }
				| stmt_loop
						{ $$ = $1; }
				| stmt_while
						{ $$ = $1; }
				| stmt_for
						{ $$ = $1; }
				| stmt_exit
						{ $$ = $1; }
				| stmt_return
						{ $$ = $1; }
				| stmt_raise
						{ $$ = $1; }
				| stmt_execsql
						{ $$ = $1; }
				| stmt_dynexecute
						{ $$ = $1; }
				| stmt_perform
						{ $$ = $1; }
				| stmt_getdiag
						{ $$ = $1; }
				| stmt_open
						{ $$ = $1; }
				| stmt_fetch
						{ $$ = $1; }
				| stmt_move
						{ $$ = $1; }
				| stmt_close
						{ $$ = $1; }
				| stmt_null
						{ $$ = $1; }
				;

stmt_perform	: K_PERFORM expr_until_semi
					{
						PLpgSQL_stmt_perform *new;

						new = palloc0(sizeof(PLpgSQL_stmt_perform));
						new->cmd_type = PLPGSQL_STMT_PERFORM;
						new->lineno   = plpgsql_location_to_lineno(@1);
						new->expr  = $2;

						$$ = (PLpgSQL_stmt *)new;
					}
				;

stmt_assign		: assign_var K_ASSIGN expr_until_semi
					{
						PLpgSQL_stmt_assign *new;

						new = palloc0(sizeof(PLpgSQL_stmt_assign));
						new->cmd_type = PLPGSQL_STMT_ASSIGN;
						new->lineno   = plpgsql_location_to_lineno(@1);
						new->varno = $1;
						new->expr  = $3;

						$$ = (PLpgSQL_stmt *)new;
					}
				;

stmt_getdiag	: K_GET K_DIAGNOSTICS getdiag_list ';'
					{
						PLpgSQL_stmt_getdiag	 *new;

						new = palloc0(sizeof(PLpgSQL_stmt_getdiag));
						new->cmd_type = PLPGSQL_STMT_GETDIAG;
						new->lineno   = plpgsql_location_to_lineno(@1);
						new->diag_items  = $3;

						$$ = (PLpgSQL_stmt *)new;
					}
				;

getdiag_list : getdiag_list ',' getdiag_list_item
					{
						$$ = lappend($1, $3);
					}
				| getdiag_list_item
					{
						$$ = list_make1($1);
					}
				;

getdiag_list_item : getdiag_target K_ASSIGN getdiag_kind
					{
						PLpgSQL_diag_item *new;

						new = palloc(sizeof(PLpgSQL_diag_item));
						new->target = $1;
						new->kind = $3;

						$$ = new;
					}
				;

getdiag_kind : K_ROW_COUNT
					{
						$$ = PLPGSQL_GETDIAG_ROW_COUNT;
					}
				| K_RESULT_OID
					{
						$$ = PLPGSQL_GETDIAG_RESULT_OID;
					}
				;

getdiag_target	: T_DATUM
					{
						check_assignable(yylval.datum, @1);
						if (yylval.datum->dtype == PLPGSQL_DTYPE_ROW ||
							yylval.datum->dtype == PLPGSQL_DTYPE_REC)
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("\"%s\" is not a scalar variable",
											yytext),
									 parser_errposition(@1)));
						$$ = yylval.datum->dno;
					}
				| T_WORD
					{
						/* just to give a better message than "syntax error" */
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("\"%s\" is not a known variable",
										yytext),
								 parser_errposition(@1)));
					}
				;


assign_var		: T_DATUM
					{
						check_assignable(yylval.datum, @1);
						$$ = yylval.datum->dno;
					}
				| assign_var '[' expr_until_rightbracket
					{
						PLpgSQL_arrayelem	*new;

						new = palloc0(sizeof(PLpgSQL_arrayelem));
						new->dtype		= PLPGSQL_DTYPE_ARRAYELEM;
						new->subscript	= $3;
						new->arrayparentno = $1;

						plpgsql_adddatum((PLpgSQL_datum *) new);

						$$ = new->dno;
					}
				;

stmt_if			: K_IF expr_until_then proc_sect stmt_else K_END K_IF ';'
					{
						PLpgSQL_stmt_if *new;

						new = palloc0(sizeof(PLpgSQL_stmt_if));
						new->cmd_type	= PLPGSQL_STMT_IF;
						new->lineno		= plpgsql_location_to_lineno(@1);
						new->cond		= $2;
						new->true_body	= $3;
						new->false_body = $4;

						$$ = (PLpgSQL_stmt *)new;
					}
				;

stmt_else		:
					{
						$$ = NIL;
					}
				| K_ELSIF expr_until_then proc_sect stmt_else
					{
						/*----------
						 * Translate the structure:	   into:
						 *
						 * IF c1 THEN				   IF c1 THEN
						 *	 ...						   ...
						 * ELSIF c2 THEN			   ELSE
						 *								   IF c2 THEN
						 *	 ...							   ...
						 * ELSE							   ELSE
						 *	 ...							   ...
						 * END IF						   END IF
						 *							   END IF
						 *----------
						 */
						PLpgSQL_stmt_if *new_if;

						/* first create a new if-statement */
						new_if = palloc0(sizeof(PLpgSQL_stmt_if));
						new_if->cmd_type	= PLPGSQL_STMT_IF;
						new_if->lineno		= plpgsql_location_to_lineno(@1);
						new_if->cond		= $2;
						new_if->true_body	= $3;
						new_if->false_body	= $4;

						/* wrap the if-statement in a "container" list */
						$$ = list_make1(new_if);
					}

				| K_ELSE proc_sect
					{
						$$ = $2;
					}
				;

stmt_case		: K_CASE opt_expr_until_when case_when_list opt_case_else K_END K_CASE ';'
					{
						$$ = make_case(@1, $2, $3, $4);
					}
				;

opt_expr_until_when	:
					{
						PLpgSQL_expr *expr = NULL;
						int	tok = yylex();

						if (tok != K_WHEN)
						{
							plpgsql_push_back_token(tok);
							expr = plpgsql_read_expression(K_WHEN, "WHEN");
						}
						plpgsql_push_back_token(K_WHEN);
						$$ = expr;
					}
			    ;

case_when_list	: case_when_list case_when
					{
						$$ = lappend($1, $2);
					}
				| case_when
					{
						$$ = list_make1($1);
					}
				;

case_when		: K_WHEN expr_until_then proc_sect
					{
						PLpgSQL_case_when *new = palloc(sizeof(PLpgSQL_case_when));

						new->lineno	= plpgsql_location_to_lineno(@1);
						new->expr	= $2;
						new->stmts	= $3;
						$$ = new;
					}
				;

opt_case_else	:
					{
						$$ = NIL;
					}
				| K_ELSE proc_sect
					{
						/*
						 * proc_sect could return an empty list, but we
						 * must distinguish that from not having ELSE at all.
						 * Simplest fix is to return a list with one NULL
						 * pointer, which make_case() must take care of.
						 */
						if ($2 != NIL)
							$$ = $2;
						else
							$$ = list_make1(NULL);
					}
				;

stmt_loop		: opt_block_label K_LOOP loop_body
					{
						PLpgSQL_stmt_loop *new;

						new = palloc0(sizeof(PLpgSQL_stmt_loop));
						new->cmd_type = PLPGSQL_STMT_LOOP;
						new->lineno   = plpgsql_location_to_lineno(@2);
						new->label	  = $1;
						new->body	  = $3.stmts;

						check_labels($1, $3.end_label, $3.end_label_location);
						plpgsql_ns_pop();

						$$ = (PLpgSQL_stmt *)new;
					}
				;

stmt_while		: opt_block_label K_WHILE expr_until_loop loop_body
					{
						PLpgSQL_stmt_while *new;

						new = palloc0(sizeof(PLpgSQL_stmt_while));
						new->cmd_type = PLPGSQL_STMT_WHILE;
						new->lineno   = plpgsql_location_to_lineno(@2);
						new->label	  = $1;
						new->cond	  = $3;
						new->body	  = $4.stmts;

						check_labels($1, $4.end_label, $4.end_label_location);
						plpgsql_ns_pop();

						$$ = (PLpgSQL_stmt *)new;
					}
				;

stmt_for		: opt_block_label K_FOR for_control loop_body
					{
						/* This runs after we've scanned the loop body */
						if ($3->cmd_type == PLPGSQL_STMT_FORI)
						{
							PLpgSQL_stmt_fori		*new;

							new = (PLpgSQL_stmt_fori *) $3;
							new->lineno   = plpgsql_location_to_lineno(@2);
							new->label	  = $1;
							new->body	  = $4.stmts;
							$$ = (PLpgSQL_stmt *) new;
						}
						else
						{
							PLpgSQL_stmt_forq		*new;

							Assert($3->cmd_type == PLPGSQL_STMT_FORS ||
								   $3->cmd_type == PLPGSQL_STMT_FORC ||
								   $3->cmd_type == PLPGSQL_STMT_DYNFORS);
							/* forq is the common supertype of all three */
							new = (PLpgSQL_stmt_forq *) $3;
							new->lineno   = plpgsql_location_to_lineno(@2);
							new->label	  = $1;
							new->body	  = $4.stmts;
							$$ = (PLpgSQL_stmt *) new;
						}

						check_labels($1, $4.end_label, $4.end_label_location);
						/* close namespace started in opt_block_label */
						plpgsql_ns_pop();
					}
				;

for_control		: for_variable K_IN
					{
						int			tok = yylex();
						int			tokloc = yylloc;

						if (tok == K_EXECUTE)
						{
							/* EXECUTE means it's a dynamic FOR loop */
							PLpgSQL_stmt_dynfors	*new;
							PLpgSQL_expr			*expr;
							int						term;

							expr = read_sql_expression2(K_LOOP, K_USING,
														"LOOP or USING",
														&term);

							new = palloc0(sizeof(PLpgSQL_stmt_dynfors));
							new->cmd_type = PLPGSQL_STMT_DYNFORS;
							if ($1.rec)
							{
								new->rec = $1.rec;
								check_assignable((PLpgSQL_datum *) new->rec, @1);
							}
							else if ($1.row)
							{
								new->row = $1.row;
								check_assignable((PLpgSQL_datum *) new->row, @1);
							}
							else if ($1.scalar)
							{
								/* convert single scalar to list */
								new->row = make_scalar_list1($1.name, $1.scalar,
															 $1.lineno, @1);
								/* no need for check_assignable */
							}
							else
							{
								ereport(ERROR,
										(errcode(ERRCODE_DATATYPE_MISMATCH),
										 errmsg("loop variable of loop over rows must be a record or row variable or list of scalar variables"),
										 parser_errposition(@1)));
							}
							new->query = expr;

							if (term == K_USING)
							{
								do
								{
									expr = read_sql_expression2(',', K_LOOP,
																", or LOOP",
																&term);
									new->params = lappend(new->params, expr);
								} while (term == ',');
							}

							$$ = (PLpgSQL_stmt *) new;
						}
						else if (tok == T_DATUM &&
								 yylval.datum->dtype == PLPGSQL_DTYPE_VAR &&
								 ((PLpgSQL_var *) yylval.datum)->datatype->typoid == REFCURSOROID)
						{
							/* It's FOR var IN cursor */
							PLpgSQL_stmt_forc	*new;
							PLpgSQL_var			*cursor = (PLpgSQL_var *) yylval.datum;
							char				*varname;

							new = (PLpgSQL_stmt_forc *) palloc0(sizeof(PLpgSQL_stmt_forc));
							new->cmd_type = PLPGSQL_STMT_FORC;
							new->curvar = cursor->dno;

							/* Should have had a single variable name */
							if ($1.scalar && $1.row)
								ereport(ERROR,
										(errcode(ERRCODE_SYNTAX_ERROR),
										 errmsg("cursor FOR loop must have only one target variable"),
										 parser_errposition(@1)));

							/* can't use an unbound cursor this way */
							if (cursor->cursor_explicit_expr == NULL)
								ereport(ERROR,
										(errcode(ERRCODE_SYNTAX_ERROR),
										 errmsg("cursor FOR loop must use a bound cursor variable"),
										 parser_errposition(tokloc)));

							/* collect cursor's parameters if any */
							new->argquery = read_cursor_args(cursor,
															 K_LOOP,
															 "LOOP");

							/* create loop's private RECORD variable */
							plpgsql_convert_ident($1.name, &varname, 1);
							new->rec = plpgsql_build_record(varname,
															$1.lineno,
															true);

							$$ = (PLpgSQL_stmt *) new;
						}
						else
						{
							PLpgSQL_expr	*expr1;
							int				expr1loc;
							bool			reverse = false;

							/*
							 * We have to distinguish between two
							 * alternatives: FOR var IN a .. b and FOR
							 * var IN query. Unfortunately this is
							 * tricky, since the query in the second
							 * form needn't start with a SELECT
							 * keyword.  We use the ugly hack of
							 * looking for two periods after the first
							 * token. We also check for the REVERSE
							 * keyword, which means it must be an
							 * integer loop.
							 */
							if (tok == K_REVERSE)
								reverse = true;
							else
								plpgsql_push_back_token(tok);

							/*
							 * Read tokens until we see either a ".."
							 * or a LOOP. The text we read may not
							 * necessarily be a well-formed SQL
							 * statement, so we need to invoke
							 * read_sql_construct directly.
							 */
							expr1 = read_sql_construct(K_DOTDOT,
													   K_LOOP,
													   0,
													   "LOOP",
													   "SELECT ",
													   true,
													   false,
													   &expr1loc,
													   &tok);

							if (tok == K_DOTDOT)
							{
								/* Saw "..", so it must be an integer loop */
								PLpgSQL_expr		*expr2;
								PLpgSQL_expr		*expr_by;
								PLpgSQL_var			*fvar;
								PLpgSQL_stmt_fori	*new;
								char				*varname;

								/* Check first expression is well-formed */
								check_sql_expr(expr1->query, expr1loc, 7);

								/* Read and check the second one */
								expr2 = read_sql_expression2(K_LOOP, K_BY,
															 "LOOP",
															 &tok);

								/* Get the BY clause if any */
								if (tok == K_BY)
									expr_by = plpgsql_read_expression(K_LOOP,
																	  "LOOP");
								else
									expr_by = NULL;

								/* Should have had a single variable name */
								if ($1.scalar && $1.row)
									ereport(ERROR,
											(errcode(ERRCODE_SYNTAX_ERROR),
											 errmsg("integer FOR loop must have only one target variable"),
											 parser_errposition(@1)));

								/* create loop's private variable */
								plpgsql_convert_ident($1.name, &varname, 1);
								fvar = (PLpgSQL_var *)
									plpgsql_build_variable(varname,
														   $1.lineno,
														   plpgsql_build_datatype(INT4OID,
																				  -1),
														   true);

								new = palloc0(sizeof(PLpgSQL_stmt_fori));
								new->cmd_type = PLPGSQL_STMT_FORI;
								new->var	  = fvar;
								new->reverse  = reverse;
								new->lower	  = expr1;
								new->upper	  = expr2;
								new->step	  = expr_by;

								$$ = (PLpgSQL_stmt *) new;
							}
							else
							{
								/*
								 * No "..", so it must be a query loop. We've
								 * prefixed an extra SELECT to the query text,
								 * so we need to remove that before performing
								 * syntax checking.
								 */
								char				*tmp_query;
								PLpgSQL_stmt_fors	*new;

								if (reverse)
									ereport(ERROR,
											(errcode(ERRCODE_SYNTAX_ERROR),
											 errmsg("cannot specify REVERSE in query FOR loop"),
											 parser_errposition(tokloc)));

								Assert(strncmp(expr1->query, "SELECT ", 7) == 0);
								tmp_query = pstrdup(expr1->query + 7);
								pfree(expr1->query);
								expr1->query = tmp_query;

								check_sql_expr(expr1->query, expr1loc, 0);

								new = palloc0(sizeof(PLpgSQL_stmt_fors));
								new->cmd_type = PLPGSQL_STMT_FORS;
								if ($1.rec)
								{
									new->rec = $1.rec;
									check_assignable((PLpgSQL_datum *) new->rec, @1);
								}
								else if ($1.row)
								{
									new->row = $1.row;
									check_assignable((PLpgSQL_datum *) new->row, @1);
								}
								else if ($1.scalar)
								{
									/* convert single scalar to list */
									new->row = make_scalar_list1($1.name, $1.scalar,
																 $1.lineno, @1);
									/* no need for check_assignable */
								}
								else
								{
									ereport(ERROR,
											(errcode(ERRCODE_SYNTAX_ERROR),
											 errmsg("loop variable of loop over rows must be a record or row variable or list of scalar variables"),
											 parser_errposition(@1)));
								}

								new->query = expr1;
								$$ = (PLpgSQL_stmt *) new;
							}
						}
					}
				;

/*
 * Processing the for_variable is tricky because we don't yet know if the
 * FOR is an integer FOR loop or a loop over query results.  In the former
 * case, the variable is just a name that we must instantiate as a loop
 * local variable, regardless of any other definition it might have.
 * Therefore, we always save the actual identifier into $$.name where it
 * can be used for that case.  We also save the outer-variable definition,
 * if any, because that's what we need for the loop-over-query case.  Note
 * that we must NOT apply check_assignable() or any other semantic check
 * until we know what's what.
 *
 * However, if we see a comma-separated list of names, we know that it
 * can't be an integer FOR loop and so it's OK to check the variables
 * immediately.  In particular, for T_WORD followed by comma, we should
 * complain that the name is not known rather than say it's a syntax error.
 * Note that the non-error result of this case sets *both* $$.scalar and
 * $$.row; see the for_control production.
 */
for_variable	: T_DATUM
					{
						$$.name = pstrdup(yytext);
						$$.lineno = plpgsql_location_to_lineno(@1);
						if (yylval.datum->dtype == PLPGSQL_DTYPE_ROW)
						{
							$$.scalar = NULL;
							$$.rec = NULL;
							$$.row = (PLpgSQL_row *) yylval.datum;
						}
						else if (yylval.datum->dtype == PLPGSQL_DTYPE_REC)
						{
							$$.scalar = NULL;
							$$.rec = (PLpgSQL_rec *) yylval.datum;
							$$.row = NULL;
						}
						else
						{
							int			tok;

							$$.scalar = yylval.datum;
							$$.rec = NULL;
							$$.row = NULL;
							/* check for comma-separated list */
							tok = yylex();
							plpgsql_push_back_token(tok);
							if (tok == ',')
								$$.row = read_into_scalar_list($$.name,
															   $$.scalar,
															   @1);
						}
					}
				| T_WORD
					{
						int			tok;

						$$.name = pstrdup(yytext);
						$$.lineno = plpgsql_location_to_lineno(@1);
						$$.scalar = NULL;
						$$.rec = NULL;
						$$.row = NULL;
						/* check for comma-separated list */
						tok = yylex();
						plpgsql_push_back_token(tok);
						if (tok == ',')
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("\"%s\" is not a known variable",
											$$.name),
									 parser_errposition(@1)));
					}
				;

stmt_exit		: exit_type opt_label opt_exitcond
					{
						PLpgSQL_stmt_exit *new;

						new = palloc0(sizeof(PLpgSQL_stmt_exit));
						new->cmd_type = PLPGSQL_STMT_EXIT;
						new->is_exit  = $1;
						new->lineno	  = plpgsql_location_to_lineno(@1);
						new->label	  = $2;
						new->cond	  = $3;

						$$ = (PLpgSQL_stmt *)new;
					}
				;

exit_type		: K_EXIT
					{
						$$ = true;
					}
				| K_CONTINUE
					{
						$$ = false;
					}
				;

stmt_return		: K_RETURN
					{
						int	tok;

						tok = yylex();
						if (tok == 0)
							yyerror("unexpected end of function definition");

						/*
						 * To avoid making NEXT and QUERY effectively be
						 * reserved words within plpgsql, recognize them
						 * via yytext.
						 */
						if (pg_strcasecmp(yytext, "next") == 0)
						{
							$$ = make_return_next_stmt(@1);
						}
						else if (pg_strcasecmp(yytext, "query") == 0)
						{
							$$ = make_return_query_stmt(@1);
						}
						else
						{
							plpgsql_push_back_token(tok);
							$$ = make_return_stmt(@1);
						}
					}
				;

stmt_raise		: K_RAISE
					{
						PLpgSQL_stmt_raise		*new;
						int	tok;

						new = palloc(sizeof(PLpgSQL_stmt_raise));

						new->cmd_type	= PLPGSQL_STMT_RAISE;
						new->lineno		= plpgsql_location_to_lineno(@1);
						new->elog_level = ERROR;	/* default */
						new->condname	= NULL;
						new->message	= NULL;
						new->params		= NIL;
						new->options	= NIL;

						tok = yylex();
						if (tok == 0)
							yyerror("unexpected end of function definition");

						/*
						 * We could have just RAISE, meaning to re-throw
						 * the current error.
						 */
						if (tok != ';')
						{
							/*
							 * First is an optional elog severity level.
							 * Most of these are not plpgsql keywords,
							 * so we rely on examining yytext.
							 */
							if (pg_strcasecmp(yytext, "exception") == 0)
							{
								new->elog_level = ERROR;
								tok = yylex();
							}
							else if (pg_strcasecmp(yytext, "warning") == 0)
							{
								new->elog_level = WARNING;
								tok = yylex();
							}
							else if (pg_strcasecmp(yytext, "notice") == 0)
							{
								new->elog_level = NOTICE;
								tok = yylex();
							}
							else if (pg_strcasecmp(yytext, "info") == 0)
							{
								new->elog_level = INFO;
								tok = yylex();
							}
							else if (pg_strcasecmp(yytext, "log") == 0)
							{
								new->elog_level = LOG;
								tok = yylex();
							}
							else if (pg_strcasecmp(yytext, "debug") == 0)
							{
								new->elog_level = DEBUG1;
								tok = yylex();
							}
							if (tok == 0)
								yyerror("unexpected end of function definition");

							/*
							 * Next we can have a condition name, or
							 * equivalently SQLSTATE 'xxxxx', or a string
							 * literal that is the old-style message format,
							 * or USING to start the option list immediately.
							 */
							if (tok == T_STRING)
							{
								/* old style message and parameters */
								new->message = parse_string_token(yytext, yylloc);
								/*
								 * We expect either a semi-colon, which
								 * indicates no parameters, or a comma that
								 * begins the list of parameter expressions,
								 * or USING to begin the options list.
								 */
								tok = yylex();
								if (tok != ',' && tok != ';' && tok != K_USING)
									yyerror("syntax error");

								while (tok == ',')
								{
									PLpgSQL_expr *expr;

									expr = read_sql_construct(',', ';', K_USING,
															  ", or ; or USING",
															  "SELECT ",
															  true, true,
															  NULL, &tok);
									new->params = lappend(new->params, expr);
								}
							}
							else if (tok != K_USING)
							{
								/* must be condition name or SQLSTATE */
								if (pg_strcasecmp(yytext, "sqlstate") == 0)
								{
									/* next token should be a string literal */
									char   *sqlstatestr;

									if (yylex() != T_STRING)
										yyerror("syntax error");
									sqlstatestr = parse_string_token(yytext, yylloc);

									if (strlen(sqlstatestr) != 5)
										yyerror("invalid SQLSTATE code");
									if (strspn(sqlstatestr, "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ") != 5)
										yyerror("invalid SQLSTATE code");
									new->condname = sqlstatestr;
								}
								else
								{
									char   *cname;

									if (tok != T_WORD)
										yyerror("syntax error");
									plpgsql_convert_ident(yytext, &cname, 1);
									plpgsql_recognize_err_condition(cname,
																	false);
									new->condname = cname;
								}
								tok = yylex();
								if (tok != ';' && tok != K_USING)
									yyerror("syntax error");
							}

							if (tok == K_USING)
								new->options = read_raise_options();
						}

						$$ = (PLpgSQL_stmt *)new;
					}
				;

loop_body		: proc_sect K_END K_LOOP opt_label ';'
					{
						$$.stmts = $1;
						$$.end_label = $4;
						$$.end_label_location = @4;
					}
				;

/*
 * T_WORD+T_DBLWORD+T_TRIPWORD match any initial identifier that is not a
 * known plpgsql variable.  The latter two cases are probably syntax errors,
 * but we'll let the core parser decide that.
 */
stmt_execsql	: K_INSERT
					{ $$ = make_execsql_stmt(K_INSERT, @1); }
				| T_WORD
					{ $$ = make_execsql_stmt(T_WORD, @1); }
				| T_DBLWORD
					{ $$ = make_execsql_stmt(T_DBLWORD, @1); }
				| T_TRIPWORD
					{ $$ = make_execsql_stmt(T_TRIPWORD, @1); }
				;

stmt_dynexecute : K_EXECUTE
					{
						PLpgSQL_stmt_dynexecute *new;
						PLpgSQL_expr *expr;
						int endtoken;

						expr = read_sql_construct(K_INTO, K_USING, ';',
												  "INTO or USING or ;",
												  "SELECT ",
												  true, true,
												  NULL, &endtoken);

						new = palloc(sizeof(PLpgSQL_stmt_dynexecute));
						new->cmd_type = PLPGSQL_STMT_DYNEXECUTE;
						new->lineno = plpgsql_location_to_lineno(@1);
						new->query = expr;
						new->into = false;
						new->strict = false;
						new->rec = NULL;
						new->row = NULL;
						new->params = NIL;

						/* If we found "INTO", collect the argument */
						if (endtoken == K_INTO)
						{
							new->into = true;
							read_into_target(&new->rec, &new->row, &new->strict);
							endtoken = yylex();
							if (endtoken != ';' && endtoken != K_USING)
								yyerror("syntax error");
						}

						/* If we found "USING", collect the argument(s) */
						if (endtoken == K_USING)
						{
							do
							{
								expr = read_sql_expression2(',', ';',
															", or ;",
															&endtoken);
								new->params = lappend(new->params, expr);
							} while (endtoken == ',');
						}

						$$ = (PLpgSQL_stmt *)new;
					}
				;


stmt_open		: K_OPEN cursor_variable
					{
						PLpgSQL_stmt_open *new;
						int				  tok;

						new = palloc0(sizeof(PLpgSQL_stmt_open));
						new->cmd_type = PLPGSQL_STMT_OPEN;
						new->lineno = plpgsql_location_to_lineno(@1);
						new->curvar = $2->dno;
						new->cursor_options = CURSOR_OPT_FAST_PLAN;

						if ($2->cursor_explicit_expr == NULL)
						{
							/* be nice if we could use opt_scrollable here */
						    tok = yylex();
							if (tok == K_NOSCROLL)
							{
								new->cursor_options |= CURSOR_OPT_NO_SCROLL;
								tok = yylex();
							}
							else if (tok == K_SCROLL)
							{
								new->cursor_options |= CURSOR_OPT_SCROLL;
								tok = yylex();
							}

							if (tok != K_FOR)
								ereport(ERROR,
										(errcode(ERRCODE_SYNTAX_ERROR),
										 errmsg("syntax error at \"%s\"",
												yytext),
										 errdetail("Expected \"FOR\", to open a cursor for an unbound cursor variable."),
										 parser_errposition(yylloc)));

							tok = yylex();
							if (tok == K_EXECUTE)
							{
								new->dynquery = read_sql_stmt("SELECT ");
							}
							else
							{
								plpgsql_push_back_token(tok);
								new->query = read_sql_stmt("");
							}
						}
						else
						{
							/* predefined cursor query, so read args */
							new->argquery = read_cursor_args($2, ';', ";");
						}

						$$ = (PLpgSQL_stmt *)new;
					}
				;

stmt_fetch		: K_FETCH opt_fetch_direction cursor_variable K_INTO
					{
						PLpgSQL_stmt_fetch *fetch = $2;
						PLpgSQL_rec	   *rec;
						PLpgSQL_row	   *row;

						/* We have already parsed everything through the INTO keyword */
						read_into_target(&rec, &row, NULL);

						if (yylex() != ';')
							yyerror("syntax error");

						/*
						 * We don't allow multiple rows in PL/pgSQL's FETCH
						 * statement, only in MOVE.
						 */
						if (fetch->returns_multiple_rows)
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									 errmsg("FETCH statement cannot return multiple rows"),
									 parser_errposition(@1)));

						fetch->lineno = plpgsql_location_to_lineno(@1);
						fetch->rec		= rec;
						fetch->row		= row;
						fetch->curvar	= $3->dno;
						fetch->is_move	= false;

						$$ = (PLpgSQL_stmt *)fetch;
					}
				;

stmt_move		: K_MOVE opt_fetch_direction cursor_variable ';'
					{
						PLpgSQL_stmt_fetch *fetch = $2;

						fetch->lineno = plpgsql_location_to_lineno(@1);
						fetch->curvar	= $3->dno;
						fetch->is_move	= true;

						$$ = (PLpgSQL_stmt *)fetch;
					}
				;

opt_fetch_direction	:
					{
						$$ = read_fetch_direction();
					}
				;

stmt_close		: K_CLOSE cursor_variable ';'
					{
						PLpgSQL_stmt_close *new;

						new = palloc(sizeof(PLpgSQL_stmt_close));
						new->cmd_type = PLPGSQL_STMT_CLOSE;
						new->lineno = plpgsql_location_to_lineno(@1);
						new->curvar = $2->dno;

						$$ = (PLpgSQL_stmt *)new;
					}
				;

stmt_null		: K_NULL ';'
					{
						/* We do not bother building a node for NULL */
						$$ = NULL;
					}
				;

cursor_variable	: T_DATUM
					{
						if (yylval.datum->dtype != PLPGSQL_DTYPE_VAR)
							ereport(ERROR,
									(errcode(ERRCODE_DATATYPE_MISMATCH),
									 errmsg("cursor variable must be a simple variable"),
									 parser_errposition(@1)));

						if (((PLpgSQL_var *) yylval.datum)->datatype->typoid != REFCURSOROID)
							ereport(ERROR,
									(errcode(ERRCODE_DATATYPE_MISMATCH),
									 errmsg("variable \"%s\" must be of type cursor or refcursor",
											((PLpgSQL_var *) yylval.datum)->refname),
									 parser_errposition(@1)));
						$$ = (PLpgSQL_var *) yylval.datum;
					}
				| T_WORD
					{
						/* just to give a better message than "syntax error" */
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("\"%s\" is not a known variable",
										yytext),
								 parser_errposition(@1)));
					}
				;

exception_sect	:
					{ $$ = NULL; }
				| K_EXCEPTION
					{
						/*
						 * We use a mid-rule action to add these
						 * special variables to the namespace before
						 * parsing the WHEN clauses themselves.  The
						 * scope of the names extends to the end of the
						 * current block.
						 */
						int			lineno = plpgsql_location_to_lineno(@1);
						PLpgSQL_exception_block *new = palloc(sizeof(PLpgSQL_exception_block));
						PLpgSQL_variable *var;

						var = plpgsql_build_variable("sqlstate", lineno,
													 plpgsql_build_datatype(TEXTOID, -1),
													 true);
						((PLpgSQL_var *) var)->isconst = true;
						new->sqlstate_varno = var->dno;

						var = plpgsql_build_variable("sqlerrm", lineno,
													 plpgsql_build_datatype(TEXTOID, -1),
													 true);
						((PLpgSQL_var *) var)->isconst = true;
						new->sqlerrm_varno = var->dno;

						$<exception_block>$ = new;
					}
					proc_exceptions
					{
						PLpgSQL_exception_block *new = $<exception_block>2;
						new->exc_list = $3;

						$$ = new;
					}
				;

proc_exceptions	: proc_exceptions proc_exception
						{
							$$ = lappend($1, $2);
						}
				| proc_exception
						{
							$$ = list_make1($1);
						}
				;

proc_exception	: K_WHEN proc_conditions K_THEN proc_sect
					{
						PLpgSQL_exception *new;

						new = palloc0(sizeof(PLpgSQL_exception));
						new->lineno     = plpgsql_location_to_lineno(@1);
						new->conditions = $2;
						new->action	    = $4;

						$$ = new;
					}
				;

proc_conditions	: proc_conditions K_OR proc_condition
						{
							PLpgSQL_condition	*old;

							for (old = $1; old->next != NULL; old = old->next)
								/* skip */ ;
							old->next = $3;
							$$ = $1;
						}
				| proc_condition
						{
							$$ = $1;
						}
				;

proc_condition	: any_name
						{
							if (strcmp($1, "sqlstate") != 0)
							{
								$$ = plpgsql_parse_err_condition($1);
							}
							else
							{
								PLpgSQL_condition *new;
								char   *sqlstatestr;

								/* next token should be a string literal */
								if (yylex() != T_STRING)
									yyerror("syntax error");
								sqlstatestr = parse_string_token(yytext, yylloc);

								if (strlen(sqlstatestr) != 5)
									yyerror("invalid SQLSTATE code");
								if (strspn(sqlstatestr, "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ") != 5)
									yyerror("invalid SQLSTATE code");

								new = palloc(sizeof(PLpgSQL_condition));
								new->sqlerrstate =
									MAKE_SQLSTATE(sqlstatestr[0],
												  sqlstatestr[1],
												  sqlstatestr[2],
												  sqlstatestr[3],
												  sqlstatestr[4]);
								new->condname = sqlstatestr;
								new->next = NULL;

								$$ = new;
							}
						}
				;

expr_until_semi :
					{ $$ = plpgsql_read_expression(';', ";"); }
				;

expr_until_rightbracket :
					{ $$ = plpgsql_read_expression(']', "]"); }
				;

expr_until_then :
					{ $$ = plpgsql_read_expression(K_THEN, "THEN"); }
				;

expr_until_loop :
					{ $$ = plpgsql_read_expression(K_LOOP, "LOOP"); }
				;

opt_block_label	:
					{
						plpgsql_ns_push(NULL);
						$$ = NULL;
					}
				| '<' '<' any_name '>' '>'
					{
						plpgsql_ns_push($3);
						$$ = $3;
					}
				;

opt_label	:
					{
						$$ = NULL;
					}
				| any_identifier
					{
						$$ = check_label($1);
					}
				;

opt_exitcond	: ';'
					{ $$ = NULL; }
				| K_WHEN expr_until_semi
					{ $$ = $2; }
				;

/*
 * need both options because scanner will have tried to resolve as variable
 */
any_identifier	: T_WORD
					{
						$$ = yytext;
					}
				| T_DATUM
					{
						$$ = yytext;
					}
				;

any_name		: any_identifier
					{
						char	*name;

						plpgsql_convert_ident($1, &name, 1);
						$$ = name;
					}
				;

%%


/* Convenience routine to read an expression with one possible terminator */
PLpgSQL_expr *
plpgsql_read_expression(int until, const char *expected)
{
	return read_sql_construct(until, 0, 0, expected,
							  "SELECT ", true, true, NULL, NULL);
}

/* Convenience routine to read an expression with two possible terminators */
static PLpgSQL_expr *
read_sql_expression2(int until, int until2, const char *expected,
					 int *endtoken)
{
	return read_sql_construct(until, until2, 0, expected,
							  "SELECT ", true, true, NULL, endtoken);
}

/* Convenience routine to read a SQL statement that must end with ';' */
static PLpgSQL_expr *
read_sql_stmt(const char *sqlstart)
{
	return read_sql_construct(';', 0, 0, ";",
							  sqlstart, false, true, NULL, NULL);
}

/*
 * Read a SQL construct and build a PLpgSQL_expr for it.
 *
 * until:		token code for expected terminator
 * until2:		token code for alternate terminator (pass 0 if none)
 * until3:		token code for another alternate terminator (pass 0 if none)
 * expected:	text to use in complaining that terminator was not found
 * sqlstart:	text to prefix to the accumulated SQL text
 * isexpression: whether to say we're reading an "expression" or a "statement"
 * valid_sql:   whether to check the syntax of the expr (prefixed with sqlstart)
 * startloc:	if not NULL, location of first token is stored at *startloc
 * endtoken:	if not NULL, ending token is stored at *endtoken
 *				(this is only interesting if until2 or until3 isn't zero)
 */
static PLpgSQL_expr *
read_sql_construct(int until,
				   int until2,
				   int until3,
				   const char *expected,
				   const char *sqlstart,
				   bool isexpression,
				   bool valid_sql,
				   int *startloc,
				   int *endtoken)
{
	int					tok;
	StringInfoData		ds;
	bool				save_LookupIdentifiers;
	int					startlocation = -1;
	int					parenlevel = 0;
	PLpgSQL_expr		*expr;

	initStringInfo(&ds);
	appendStringInfoString(&ds, sqlstart);

	/* no need to lookup identifiers within the SQL text */
	save_LookupIdentifiers = plpgsql_LookupIdentifiers;
	plpgsql_LookupIdentifiers = false;

	for (;;)
	{
		tok = yylex();
		if (startlocation < 0)			/* remember loc of first token */
			startlocation = yylloc;
		if (tok == until && parenlevel == 0)
			break;
		if (tok == until2 && parenlevel == 0)
			break;
		if (tok == until3 && parenlevel == 0)
			break;
		if (tok == '(' || tok == '[')
			parenlevel++;
		else if (tok == ')' || tok == ']')
		{
			parenlevel--;
			if (parenlevel < 0)
				yyerror("mismatched parentheses");
		}
		/*
		 * End of function definition is an error, and we don't expect to
		 * hit a semicolon either (unless it's the until symbol, in which
		 * case we should have fallen out above).
		 */
		if (tok == 0 || tok == ';')
		{
			if (parenlevel != 0)
				yyerror("mismatched parentheses");
			if (isexpression)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("missing \"%s\" at end of SQL expression",
								expected),
						 parser_errposition(yylloc)));
			else
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("missing \"%s\" at end of SQL statement",
								expected),
						 parser_errposition(yylloc)));
		}
	}

	plpgsql_LookupIdentifiers = save_LookupIdentifiers;

	if (startloc)
		*startloc = startlocation;
	if (endtoken)
		*endtoken = tok;

	/* give helpful complaint about empty input */
	if (startlocation >= yylloc)
	{
		if (isexpression)
			yyerror("missing expression");
		else
			yyerror("missing SQL statement");
	}

	plpgsql_append_source_text(&ds, startlocation, yylloc);

	/* trim any trailing whitespace, for neatness */
	while (ds.len > 0 && scanner_isspace(ds.data[ds.len - 1]))
		ds.data[--ds.len] = '\0';

	expr = palloc0(sizeof(PLpgSQL_expr));
	expr->dtype			= PLPGSQL_DTYPE_EXPR;
	expr->query			= pstrdup(ds.data);
	expr->plan			= NULL;
	expr->paramnos		= NULL;
	expr->ns            = plpgsql_ns_top();
	pfree(ds.data);

	if (valid_sql)
		check_sql_expr(expr->query, startlocation, strlen(sqlstart));

	return expr;
}

static PLpgSQL_type *
read_datatype(int tok)
{
	StringInfoData		ds;
	char			   *type_name;
	int					startlocation;
	PLpgSQL_type		*result;
	int					parenlevel = 0;

	/* Should always be called with LookupIdentifiers off */
	Assert(!plpgsql_LookupIdentifiers);

	initStringInfo(&ds);

	/* Often there will be a lookahead token, but if not, get one */
	if (tok == YYEMPTY)
		tok = yylex();

	startlocation = yylloc;

	/*
	 * If we have a single, double, or triple identifier, check for %TYPE
	 * and %ROWTYPE constructs.
	 */
	if (tok == T_WORD)
	{
		appendStringInfoString(&ds, yytext);
		tok = yylex();
		if (tok == '%')
		{
			tok = yylex();
			if (pg_strcasecmp(yytext, "type") == 0)
			{
				result = plpgsql_parse_wordtype(ds.data);
				if (result)
				{
					pfree(ds.data);
					return result;
				}
			}
			else if (pg_strcasecmp(yytext, "rowtype") == 0)
			{
				result = plpgsql_parse_wordrowtype(ds.data);
				if (result)
				{
					pfree(ds.data);
					return result;
				}
			}
		}
	}
	else if (tok == T_DBLWORD)
	{
		appendStringInfoString(&ds, yytext);
		tok = yylex();
		if (tok == '%')
		{
			tok = yylex();
			if (pg_strcasecmp(yytext, "type") == 0)
			{
				result = plpgsql_parse_dblwordtype(ds.data);
				if (result)
				{
					pfree(ds.data);
					return result;
				}
			}
			else if (pg_strcasecmp(yytext, "rowtype") == 0)
			{
				result = plpgsql_parse_dblwordrowtype(ds.data);
				if (result)
				{
					pfree(ds.data);
					return result;
				}
			}
		}
	}
	else if (tok == T_TRIPWORD)
	{
		appendStringInfoString(&ds, yytext);
		tok = yylex();
		if (tok == '%')
		{
			tok = yylex();
			if (pg_strcasecmp(yytext, "type") == 0)
			{
				result = plpgsql_parse_tripwordtype(ds.data);
				if (result)
				{
					pfree(ds.data);
					return result;
				}
			}
			/* there's no tripword rowtype construct */
		}
	}

	/* flush temporary usage of ds for rowtype checks */
	resetStringInfo(&ds);

	while (tok != ';')
	{
		if (tok == 0)
		{
			if (parenlevel != 0)
				yyerror("mismatched parentheses");
			else
				yyerror("incomplete data type declaration");
		}
		/* Possible followers for datatype in a declaration */
		if (tok == K_NOT || tok == K_ASSIGN || tok == K_DEFAULT)
			break;
		/* Possible followers for datatype in a cursor_arg list */
		if ((tok == ',' || tok == ')') && parenlevel == 0)
			break;
		if (tok == '(')
			parenlevel++;
		else if (tok == ')')
			parenlevel--;

		tok = yylex();
	}

	/* set up ds to contain complete typename text */
	plpgsql_append_source_text(&ds, startlocation, yylloc);
	type_name = ds.data;

	if (type_name[0] == '\0')
		yyerror("missing data type declaration");

	result = parse_datatype(type_name, startlocation);

	pfree(ds.data);

	plpgsql_push_back_token(tok);

	return result;
}

static PLpgSQL_stmt *
make_execsql_stmt(int firsttoken, int location)
{
	StringInfoData		ds;
	bool				save_LookupIdentifiers;
	PLpgSQL_stmt_execsql *execsql;
	PLpgSQL_expr		*expr;
	PLpgSQL_row			*row = NULL;
	PLpgSQL_rec			*rec = NULL;
	int					tok;
	int					prev_tok;
	bool				have_into = false;
	bool				have_strict = false;
	int					into_start_loc = -1;
	int					into_end_loc = -1;

	initStringInfo(&ds);

	/* no need to lookup identifiers within the SQL text */
	save_LookupIdentifiers = plpgsql_LookupIdentifiers;
	plpgsql_LookupIdentifiers = false;

	/*
	 * We have to special-case the sequence INSERT INTO, because we don't want
	 * that to be taken as an INTO-variables clause.  Fortunately, this is the
	 * only valid use of INTO in a pl/pgsql SQL command, and INTO is already a
	 * fully reserved word in the main grammar.  We have to treat it that way
	 * anywhere in the string, not only at the start; consider CREATE RULE
	 * containing an INSERT statement.
	 */
	tok = firsttoken;
	for (;;)
	{
		prev_tok = tok;
		tok = yylex();
		if (have_into && into_end_loc < 0)
			into_end_loc = yylloc;		/* token after the INTO part */
		if (tok == ';')
			break;
		if (tok == 0)
			yyerror("unexpected end of function definition");

		if (tok == K_INTO && prev_tok != K_INSERT)
		{
			if (have_into)
				yyerror("INTO specified more than once");
			have_into = true;
			into_start_loc = yylloc;
			plpgsql_LookupIdentifiers = true;
			read_into_target(&rec, &row, &have_strict);
			plpgsql_LookupIdentifiers = false;
		}
	}

	plpgsql_LookupIdentifiers = save_LookupIdentifiers;

	if (have_into)
	{
		/*
		 * Insert an appropriate number of spaces corresponding to the
		 * INTO text, so that locations within the redacted SQL statement
		 * still line up with those in the original source text.
		 */
		plpgsql_append_source_text(&ds, location, into_start_loc);
		appendStringInfoSpaces(&ds, into_end_loc - into_start_loc);
		plpgsql_append_source_text(&ds, into_end_loc, yylloc);
	}
	else
		plpgsql_append_source_text(&ds, location, yylloc);

	/* trim any trailing whitespace, for neatness */
	while (ds.len > 0 && scanner_isspace(ds.data[ds.len - 1]))
		ds.data[--ds.len] = '\0';

	expr = palloc0(sizeof(PLpgSQL_expr));
	expr->dtype			= PLPGSQL_DTYPE_EXPR;
	expr->query			= pstrdup(ds.data);
	expr->plan			= NULL;
	expr->paramnos		= NULL;
	expr->ns            = plpgsql_ns_top();
	pfree(ds.data);

	check_sql_expr(expr->query, location, 0);

	execsql = palloc(sizeof(PLpgSQL_stmt_execsql));
	execsql->cmd_type = PLPGSQL_STMT_EXECSQL;
	execsql->lineno  = plpgsql_location_to_lineno(location);
	execsql->sqlstmt = expr;
	execsql->into	 = have_into;
	execsql->strict	 = have_strict;
	execsql->rec	 = rec;
	execsql->row	 = row;

	return (PLpgSQL_stmt *) execsql;
}


/*
 * Read FETCH or MOVE direction clause (everything through FROM/IN).
 */
static PLpgSQL_stmt_fetch *
read_fetch_direction(void)
{
	PLpgSQL_stmt_fetch *fetch;
	int			tok;
	bool		check_FROM = true;

	/*
	 * We create the PLpgSQL_stmt_fetch struct here, but only fill in
	 * the fields arising from the optional direction clause
	 */
	fetch = (PLpgSQL_stmt_fetch *) palloc0(sizeof(PLpgSQL_stmt_fetch));
	fetch->cmd_type = PLPGSQL_STMT_FETCH;
	/* set direction defaults: */
	fetch->direction = FETCH_FORWARD;
	fetch->how_many  = 1;
	fetch->expr      = NULL;
	fetch->returns_multiple_rows = false;

	/*
	 * Most of the direction keywords are not plpgsql keywords, so we
	 * rely on examining yytext ...
	 */
	tok = yylex();
	if (tok == 0)
		yyerror("unexpected end of function definition");

	if (pg_strcasecmp(yytext, "next") == 0)
	{
		/* use defaults */
	}
	else if (pg_strcasecmp(yytext, "prior") == 0)
	{
		fetch->direction = FETCH_BACKWARD;
	}
	else if (pg_strcasecmp(yytext, "first") == 0)
	{
		fetch->direction = FETCH_ABSOLUTE;
	}
	else if (pg_strcasecmp(yytext, "last") == 0)
	{
		fetch->direction = FETCH_ABSOLUTE;
		fetch->how_many  = -1;
	}
	else if (pg_strcasecmp(yytext, "absolute") == 0)
	{
		fetch->direction = FETCH_ABSOLUTE;
		fetch->expr = read_sql_expression2(K_FROM, K_IN,
										   "FROM or IN",
										   NULL);
		check_FROM = false;
	}
	else if (pg_strcasecmp(yytext, "relative") == 0)
	{
		fetch->direction = FETCH_RELATIVE;
		fetch->expr = read_sql_expression2(K_FROM, K_IN,
										   "FROM or IN",
										   NULL);
		check_FROM = false;
	}
	else if (pg_strcasecmp(yytext, "all") == 0)
	{
		fetch->how_many = FETCH_ALL;
		fetch->returns_multiple_rows = true;
	}
	else if (pg_strcasecmp(yytext, "forward") == 0)
	{
		complete_direction(fetch, &check_FROM);
	}
	else if (pg_strcasecmp(yytext, "backward") == 0)
	{
		fetch->direction = FETCH_BACKWARD;
		complete_direction(fetch, &check_FROM);
	}
	else if (tok == K_FROM || tok == K_IN)
	{
		/* empty direction */
		check_FROM = false;
	}
	else if (tok == T_DATUM)
	{
		/* Assume there's no direction clause and tok is a cursor name */
		plpgsql_push_back_token(tok);
		check_FROM = false;
	}
	else
	{
		/*
		 * Assume it's a count expression with no preceding keyword.
		 * Note: we allow this syntax because core SQL does, but we don't
		 * document it because of the ambiguity with the omitted-direction
		 * case.  For instance, "MOVE n IN c" will fail if n is a variable.
		 * Perhaps this can be improved someday, but it's hardly worth a
		 * lot of work.
		 */
		plpgsql_push_back_token(tok);
		fetch->expr = read_sql_expression2(K_FROM, K_IN,
										   "FROM or IN",
										   NULL);
		fetch->returns_multiple_rows = true;
		check_FROM = false;
	}

	/* check FROM or IN keyword after direction's specification */
	if (check_FROM)
	{
		tok = yylex();
		if (tok != K_FROM && tok != K_IN)
			yyerror("expected FROM or IN");
	}

	return fetch;
}

/*
 * Process remainder of FETCH/MOVE direction after FORWARD or BACKWARD.
 * Allows these cases:
 *   FORWARD expr,  FORWARD ALL,  FORWARD
 *   BACKWARD expr, BACKWARD ALL, BACKWARD
 */
static void
complete_direction(PLpgSQL_stmt_fetch *fetch,  bool *check_FROM)
{
	int			tok;

	tok = yylex();
	if (tok == 0)
		yyerror("unexpected end of function definition");

	if (tok == K_FROM || tok == K_IN)
	{
		*check_FROM = false;
		return;
	}

	if (tok == K_ALL)
	{
		fetch->how_many = FETCH_ALL;
		fetch->returns_multiple_rows = true;
		*check_FROM = true;
		return;
	}

	plpgsql_push_back_token(tok);
	fetch->expr = read_sql_expression2(K_FROM, K_IN,
									   "FROM or IN",
									   NULL);
	fetch->returns_multiple_rows = true;
	*check_FROM = false;
}


static PLpgSQL_stmt *
make_return_stmt(int location)
{
	PLpgSQL_stmt_return *new;

	new = palloc0(sizeof(PLpgSQL_stmt_return));
	new->cmd_type = PLPGSQL_STMT_RETURN;
	new->lineno   = plpgsql_location_to_lineno(location);
	new->expr	  = NULL;
	new->retvarno = -1;

	if (plpgsql_curr_compile->fn_retset)
	{
		if (yylex() != ';')
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("RETURN cannot have a parameter in function returning set"),
					 errhint("Use RETURN NEXT or RETURN QUERY."),
					 parser_errposition(yylloc)));
	}
	else if (plpgsql_curr_compile->out_param_varno >= 0)
	{
		if (yylex() != ';')
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("RETURN cannot have a parameter in function with OUT parameters"),
					 parser_errposition(yylloc)));
		new->retvarno = plpgsql_curr_compile->out_param_varno;
	}
	else if (plpgsql_curr_compile->fn_rettype == VOIDOID)
	{
		if (yylex() != ';')
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("RETURN cannot have a parameter in function returning void"),
					 parser_errposition(yylloc)));
	}
	else if (plpgsql_curr_compile->fn_retistuple)
	{
		switch (yylex())
		{
			case K_NULL:
				/* we allow this to support RETURN NULL in triggers */
				break;

			case T_DATUM:
				if (yylval.datum->dtype == PLPGSQL_DTYPE_ROW ||
					yylval.datum->dtype == PLPGSQL_DTYPE_REC)
					new->retvarno = yylval.datum->dno;
				else
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("RETURN must specify a record or row variable in function returning row"),
							 parser_errposition(yylloc)));
				break;

			default:
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("RETURN must specify a record or row variable in function returning row"),
						 parser_errposition(yylloc)));
				break;
		}
		if (yylex() != ';')
			yyerror("syntax error");
	}
	else
	{
		/*
		 * Note that a well-formed expression is _required_ here;
		 * anything else is a compile-time error.
		 */
		new->expr = plpgsql_read_expression(';', ";");
	}

	return (PLpgSQL_stmt *) new;
}


static PLpgSQL_stmt *
make_return_next_stmt(int location)
{
	PLpgSQL_stmt_return_next *new;

	if (!plpgsql_curr_compile->fn_retset)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("cannot use RETURN NEXT in a non-SETOF function"),
				 parser_errposition(location)));

	new = palloc0(sizeof(PLpgSQL_stmt_return_next));
	new->cmd_type	= PLPGSQL_STMT_RETURN_NEXT;
	new->lineno		= plpgsql_location_to_lineno(location);
	new->expr		= NULL;
	new->retvarno	= -1;

	if (plpgsql_curr_compile->out_param_varno >= 0)
	{
		if (yylex() != ';')
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("RETURN NEXT cannot have a parameter in function with OUT parameters"),
					 parser_errposition(yylloc)));
		new->retvarno = plpgsql_curr_compile->out_param_varno;
	}
	else if (plpgsql_curr_compile->fn_retistuple)
	{
		switch (yylex())
		{
			case T_DATUM:
				if (yylval.datum->dtype == PLPGSQL_DTYPE_ROW ||
					yylval.datum->dtype == PLPGSQL_DTYPE_REC)
					new->retvarno = yylval.datum->dno;
				else
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("RETURN NEXT must specify a record or row variable in function returning row"),
							 parser_errposition(yylloc)));
				break;

			default:
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("RETURN NEXT must specify a record or row variable in function returning row"),
						 parser_errposition(yylloc)));
				break;
		}
		if (yylex() != ';')
			yyerror("syntax error");
	}
	else
		new->expr = plpgsql_read_expression(';', ";");

	return (PLpgSQL_stmt *) new;
}


static PLpgSQL_stmt *
make_return_query_stmt(int location)
{
	PLpgSQL_stmt_return_query *new;
	int			tok;

	if (!plpgsql_curr_compile->fn_retset)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("cannot use RETURN QUERY in a non-SETOF function"),
				 parser_errposition(location)));

	new = palloc0(sizeof(PLpgSQL_stmt_return_query));
	new->cmd_type = PLPGSQL_STMT_RETURN_QUERY;
	new->lineno = plpgsql_location_to_lineno(location);

	/* check for RETURN QUERY EXECUTE */
	if ((tok = yylex()) != K_EXECUTE)
	{
		/* ordinary static query */
		plpgsql_push_back_token(tok);
		new->query = read_sql_stmt("");
	}
	else
	{
		/* dynamic SQL */
		int		term;

		new->dynquery = read_sql_expression2(';', K_USING, "; or USING",
											 &term);
		if (term == K_USING)
		{
			do
			{
				PLpgSQL_expr *expr;

				expr = read_sql_expression2(',', ';', ", or ;", &term);
				new->params = lappend(new->params, expr);
			} while (term == ',');
		}
	}

	return (PLpgSQL_stmt *) new;
}


static void
check_assignable(PLpgSQL_datum *datum, int location)
{
	switch (datum->dtype)
	{
		case PLPGSQL_DTYPE_VAR:
			if (((PLpgSQL_var *) datum)->isconst)
				ereport(ERROR,
						(errcode(ERRCODE_ERROR_IN_ASSIGNMENT),
						 errmsg("\"%s\" is declared CONSTANT",
								((PLpgSQL_var *) datum)->refname),
						 parser_errposition(location)));
			break;
		case PLPGSQL_DTYPE_ROW:
			/* always assignable? */
			break;
		case PLPGSQL_DTYPE_REC:
			/* always assignable?  What about NEW/OLD? */
			break;
		case PLPGSQL_DTYPE_RECFIELD:
			/* always assignable? */
			break;
		case PLPGSQL_DTYPE_ARRAYELEM:
			/* always assignable? */
			break;
		default:
			elog(ERROR, "unrecognized dtype: %d", datum->dtype);
			break;
	}
}

/*
 * Read the argument of an INTO clause.  On entry, we have just read the
 * INTO keyword.
 */
static void
read_into_target(PLpgSQL_rec **rec, PLpgSQL_row **row, bool *strict)
{
	int			tok;

	/* Set default results */
	*rec = NULL;
	*row = NULL;
	if (strict)
		*strict = false;

	tok = yylex();
	if (strict && tok == K_STRICT)
	{
		*strict = true;
		tok = yylex();
	}

	switch (tok)
	{
		case T_DATUM:
			if (yylval.datum->dtype == PLPGSQL_DTYPE_ROW)
			{
				check_assignable(yylval.datum, yylloc);
				*row = (PLpgSQL_row *) yylval.datum;
			}
			else if (yylval.datum->dtype == PLPGSQL_DTYPE_REC)
			{
				check_assignable(yylval.datum, yylloc);
				*rec = (PLpgSQL_rec *) yylval.datum;
			}
			else
			{
				*row = read_into_scalar_list(yytext, yylval.datum, yylloc);
			}
			break;

		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("syntax error at \"%s\"", yytext),
					 errdetail("Expected record variable, row variable, "
							   "or list of scalar variables following INTO."),
					 parser_errposition(yylloc)));
	}
}

/*
 * Given the first datum and name in the INTO list, continue to read
 * comma-separated scalar variables until we run out. Then construct
 * and return a fake "row" variable that represents the list of
 * scalars.
 */
static PLpgSQL_row *
read_into_scalar_list(const char *initial_name,
					  PLpgSQL_datum *initial_datum,
					  int initial_location)
{
	int				 nfields;
	char			*fieldnames[1024];
	int				 varnos[1024];
	PLpgSQL_row		*row;
	int				 tok;

	check_assignable(initial_datum, initial_location);
	fieldnames[0] = pstrdup(initial_name);
	varnos[0]	  = initial_datum->dno;
	nfields		  = 1;

	while ((tok = yylex()) == ',')
	{
		/* Check for array overflow */
		if (nfields >= 1024)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("too many INTO variables specified"),
					 parser_errposition(yylloc)));

		tok = yylex();
		switch (tok)
		{
			case T_DATUM:
				check_assignable(yylval.datum, yylloc);
				if (yylval.datum->dtype == PLPGSQL_DTYPE_ROW ||
					yylval.datum->dtype == PLPGSQL_DTYPE_REC)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("\"%s\" is not a scalar variable",
									yytext),
							 parser_errposition(yylloc)));
				fieldnames[nfields] = pstrdup(yytext);
				varnos[nfields++]	= yylval.datum->dno;
				break;

			default:
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("\"%s\" is not a known variable",
								yytext),
						 parser_errposition(yylloc)));
		}
	}

	/*
	 * We read an extra, non-comma token from yylex(), so push it
	 * back onto the input stream
	 */
	plpgsql_push_back_token(tok);

	row = palloc(sizeof(PLpgSQL_row));
	row->dtype = PLPGSQL_DTYPE_ROW;
	row->refname = pstrdup("*internal*");
	row->lineno = plpgsql_location_to_lineno(initial_location);
	row->rowtupdesc = NULL;
	row->nfields = nfields;
	row->fieldnames = palloc(sizeof(char *) * nfields);
	row->varnos = palloc(sizeof(int) * nfields);
	while (--nfields >= 0)
	{
		row->fieldnames[nfields] = fieldnames[nfields];
		row->varnos[nfields] = varnos[nfields];
	}

	plpgsql_adddatum((PLpgSQL_datum *)row);

	return row;
}

/*
 * Convert a single scalar into a "row" list.  This is exactly
 * like read_into_scalar_list except we never consume any input.
 *
 * Note: lineno could be computed from location, but since callers
 * have it at hand already, we may as well pass it in.
 */
static PLpgSQL_row *
make_scalar_list1(const char *initial_name,
				  PLpgSQL_datum *initial_datum,
				  int lineno, int location)
{
	PLpgSQL_row		*row;

	check_assignable(initial_datum, location);

	row = palloc(sizeof(PLpgSQL_row));
	row->dtype = PLPGSQL_DTYPE_ROW;
	row->refname = pstrdup("*internal*");
	row->lineno = lineno;
	row->rowtupdesc = NULL;
	row->nfields = 1;
	row->fieldnames = palloc(sizeof(char *));
	row->varnos = palloc(sizeof(int));
	row->fieldnames[0] = pstrdup(initial_name);
	row->varnos[0] = initial_datum->dno;

	plpgsql_adddatum((PLpgSQL_datum *)row);

	return row;
}

/*
 * When the PL/PgSQL parser expects to see a SQL statement, it is very
 * liberal in what it accepts; for example, we often assume an
 * unrecognized keyword is the beginning of a SQL statement. This
 * avoids the need to duplicate parts of the SQL grammar in the
 * PL/PgSQL grammar, but it means we can accept wildly malformed
 * input. To try and catch some of the more obviously invalid input,
 * we run the strings we expect to be SQL statements through the main
 * SQL parser.
 *
 * We only invoke the raw parser (not the analyzer); this doesn't do
 * any database access and does not check any semantic rules, it just
 * checks for basic syntactic correctness. We do this here, rather
 * than after parsing has finished, because a malformed SQL statement
 * may cause the PL/PgSQL parser to become confused about statement
 * borders. So it is best to bail out as early as we can.
 *
 * It is assumed that "stmt" represents a copy of the function source text
 * beginning at offset "location", with leader text of length "leaderlen"
 * (typically "SELECT ") prefixed to the source text.  We use this assumption
 * to transpose any error cursor position back to the function source text.
 * If no error cursor is provided, we'll just point at "location".
 */
static void
check_sql_expr(const char *stmt, int location, int leaderlen)
{
	sql_error_callback_arg cbarg;
	ErrorContextCallback  syntax_errcontext;
	MemoryContext oldCxt;

	if (!plpgsql_check_syntax)
		return;

	cbarg.location = location;
	cbarg.leaderlen = leaderlen;

	syntax_errcontext.callback = plpgsql_sql_error_callback;
	syntax_errcontext.arg = &cbarg;
	syntax_errcontext.previous = error_context_stack;
	error_context_stack = &syntax_errcontext;

	oldCxt = MemoryContextSwitchTo(compile_tmp_cxt);
	(void) raw_parser(stmt);
	MemoryContextSwitchTo(oldCxt);

	/* Restore former ereport callback */
	error_context_stack = syntax_errcontext.previous;
}

static void
plpgsql_sql_error_callback(void *arg)
{
	sql_error_callback_arg *cbarg = (sql_error_callback_arg *) arg;
	int			errpos;

	/*
	 * First, set up internalerrposition to point to the start of the
	 * statement text within the function text.  Note this converts
	 * location (a byte offset) to a character number.
	 */
	parser_errposition(cbarg->location);

	/*
	 * If the core parser provided an error position, transpose it.
	 * Note we are dealing with 1-based character numbers at this point.
	 */
	errpos = geterrposition();
	if (errpos > cbarg->leaderlen)
	{
		int		myerrpos = getinternalerrposition();

		if (myerrpos > 0)		/* safety check */
			internalerrposition(myerrpos + errpos - cbarg->leaderlen - 1);
	}

	/* In any case, flush errposition --- we want internalerrpos only */
	errposition(0);
}

/*
 * Parse a SQL datatype name and produce a PLpgSQL_type structure.
 *
 * The heavy lifting is done elsewhere.  Here we are only concerned
 * with setting up an errcontext link that will let us give an error
 * cursor pointing into the plpgsql function source, if necessary.
 * This is handled the same as in check_sql_expr(), and we likewise
 * expect that the given string is a copy from the source text.
 */
static PLpgSQL_type *
parse_datatype(const char *string, int location)
{
	Oid			type_id;
	int32		typmod;
	sql_error_callback_arg cbarg;
	ErrorContextCallback  syntax_errcontext;

	cbarg.location = location;
	cbarg.leaderlen = 0;

	syntax_errcontext.callback = plpgsql_sql_error_callback;
	syntax_errcontext.arg = &cbarg;
	syntax_errcontext.previous = error_context_stack;
	error_context_stack = &syntax_errcontext;

	/* Let the main parser try to parse it under standard SQL rules */
	parseTypeString(string, &type_id, &typmod);

	/* Restore former ereport callback */
	error_context_stack = syntax_errcontext.previous;

	/* Okay, build a PLpgSQL_type data structure for it */
	return plpgsql_build_datatype(type_id, typmod);
}

/*
 * Convert a string-literal token to the represented string value.
 *
 * To do this, we need to invoke the core lexer.  Here we are only concerned
 * with setting up an errcontext link, which is handled the same as
 * in check_sql_expr().
 */
static char *
parse_string_token(const char *token, int location)
{
	char	   *result;
	sql_error_callback_arg cbarg;
	ErrorContextCallback  syntax_errcontext;

	cbarg.location = location;
	cbarg.leaderlen = 0;

	syntax_errcontext.callback = plpgsql_sql_error_callback;
	syntax_errcontext.arg = &cbarg;
	syntax_errcontext.previous = error_context_stack;
	error_context_stack = &syntax_errcontext;

	result = pg_parse_string_token(token);

	/* Restore former ereport callback */
	error_context_stack = syntax_errcontext.previous;

	return result;
}

static char *
check_label(const char *yytxt)
{
	char	   *label_name;

	plpgsql_convert_ident(yytxt, &label_name, 1);
	if (plpgsql_ns_lookup_label(plpgsql_ns_top(), label_name) == NULL)
		yyerror("label does not exist");
	return label_name;
}

static void
check_labels(const char *start_label, const char *end_label, int end_location)
{
	if (end_label)
	{
		if (!start_label)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("end label \"%s\" specified for unlabelled block",
							end_label),
					 parser_errposition(end_location)));

		if (strcmp(start_label, end_label) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("end label \"%s\" differs from block's label \"%s\"",
							end_label, start_label),
					 parser_errposition(end_location)));
	}
}

/*
 * Read the arguments (if any) for a cursor, followed by the until token
 *
 * If cursor has no args, just swallow the until token and return NULL.
 * If it does have args, we expect to see "( expr [, expr ...] )" followed
 * by the until token.  Consume all that and return a SELECT query that
 * evaluates the expression(s) (without the outer parens).
 */
static PLpgSQL_expr *
read_cursor_args(PLpgSQL_var *cursor, int until, const char *expected)
{
	PLpgSQL_expr *expr;
	int			tok;

	tok = yylex();
	if (cursor->cursor_explicit_argrow < 0)
	{
		/* No arguments expected */
		if (tok == '(')
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("cursor \"%s\" has no arguments",
							cursor->refname),
					 parser_errposition(yylloc)));

		if (tok != until)
			yyerror("syntax error");

		return NULL;
	}

	/* Else better provide arguments */
	if (tok != '(')
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cursor \"%s\" has arguments",
						cursor->refname),
				 parser_errposition(yylloc)));

	/*
	 * Read expressions until the matching ')'.
	 */
	expr = plpgsql_read_expression(')', ")");

	/* Next we'd better find the until token */
	tok = yylex();
	if (tok != until)
		yyerror("syntax error");

	return expr;
}

/*
 * Parse RAISE ... USING options
 */
static List *
read_raise_options(void)
{
	List	   *result = NIL;

	for (;;)
	{
		PLpgSQL_raise_option *opt;
		int		tok;

		if ((tok = yylex()) == 0)
			yyerror("unexpected end of function definition");

		opt = (PLpgSQL_raise_option *) palloc(sizeof(PLpgSQL_raise_option));

		if (pg_strcasecmp(yytext, "errcode") == 0)
			opt->opt_type = PLPGSQL_RAISEOPTION_ERRCODE;
		else if (pg_strcasecmp(yytext, "message") == 0)
			opt->opt_type = PLPGSQL_RAISEOPTION_MESSAGE;
		else if (pg_strcasecmp(yytext, "detail") == 0)
			opt->opt_type = PLPGSQL_RAISEOPTION_DETAIL;
		else if (pg_strcasecmp(yytext, "hint") == 0)
			opt->opt_type = PLPGSQL_RAISEOPTION_HINT;
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized RAISE statement option \"%s\"",
							yytext),
					 parser_errposition(yylloc)));

		if (yylex() != K_ASSIGN)
			yyerror("syntax error, expected \"=\"");

		opt->expr = read_sql_expression2(',', ';', ", or ;", &tok);

		result = lappend(result, opt);

		if (tok == ';')
			break;
	}

	return result;
}

/*
 * Fix up CASE statement
 */
static PLpgSQL_stmt *
make_case(int location, PLpgSQL_expr *t_expr,
		  List *case_when_list, List *else_stmts)
{
	PLpgSQL_stmt_case 	*new;

	new = palloc(sizeof(PLpgSQL_stmt_case));
	new->cmd_type = PLPGSQL_STMT_CASE;
	new->lineno = plpgsql_location_to_lineno(location);
	new->t_expr = t_expr;
	new->t_varno = 0;
	new->case_when_list = case_when_list;
	new->have_else = (else_stmts != NIL);
	/* Get rid of list-with-NULL hack */
	if (list_length(else_stmts) == 1 && linitial(else_stmts) == NULL)
		new->else_stmts = NIL;
	else
		new->else_stmts = else_stmts;

	/*
	 * When test expression is present, we create a var for it and then
	 * convert all the WHEN expressions to "VAR IN (original_expression)".
	 * This is a bit klugy, but okay since we haven't yet done more than
	 * read the expressions as text.  (Note that previous parsing won't
	 * have complained if the WHEN ... THEN expression contained multiple
	 * comma-separated values.)
	 */
	if (t_expr)
	{
		char	varname[32];
		PLpgSQL_var *t_var;
		ListCell *l;

		/* use a name unlikely to collide with any user names */
		snprintf(varname, sizeof(varname), "__Case__Variable_%d__",
				 plpgsql_nDatums);

		/*
		 * We don't yet know the result datatype of t_expr.  Build the
		 * variable as if it were INT4; we'll fix this at runtime if needed.
		 */
		t_var = (PLpgSQL_var *)
			plpgsql_build_variable(varname, new->lineno,
								   plpgsql_build_datatype(INT4OID, -1),
								   true);
		new->t_varno = t_var->dno;

		foreach(l, case_when_list)
		{
			PLpgSQL_case_when *cwt = (PLpgSQL_case_when *) lfirst(l);
			PLpgSQL_expr *expr = cwt->expr;
			StringInfoData	ds;

			/* copy expression query without SELECT keyword (expr->query + 7) */
			Assert(strncmp(expr->query, "SELECT ", 7) == 0);

			/* And do the string hacking */
			initStringInfo(&ds);

			appendStringInfo(&ds, "SELECT \"%s\" IN (%s)",
							 varname, expr->query + 7);

			pfree(expr->query);
			expr->query = pstrdup(ds.data);
			/* Adjust expr's namespace to include the case variable */
			expr->ns = plpgsql_ns_top();

			pfree(ds.data);
		}
	}

	return (PLpgSQL_stmt *) new;
}


/* Needed to avoid conflict between different prefix settings: */
#undef yylex

#include "pl_scan.c"
