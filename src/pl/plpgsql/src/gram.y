%{
/**********************************************************************
 * gram.y		- Parser for the PL/pgSQL
 *			  procedural language
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/pl/plpgsql/src/gram.y,v 1.4 1999/03/21 02:27:47 tgl Exp $
 *
 *    This software is copyrighted by Jan Wieck - Hamburg.
 *
 *    The author hereby grants permission  to  use,  copy,  modify,
 *    distribute,  and  license this software and its documentation
 *    for any purpose, provided that existing copyright notices are
 *    retained  in  all  copies  and  that  this notice is included
 *    verbatim in any distributions. No written agreement, license,
 *    or  royalty  fee  is required for any of the authorized uses.
 *    Modifications to this software may be  copyrighted  by  their
 *    author  and  need  not  follow  the licensing terms described
 *    here, provided that the new terms are  clearly  indicated  on
 *    the first page of each file where they apply.
 *
 *    IN NO EVENT SHALL THE AUTHOR OR DISTRIBUTORS BE LIABLE TO ANY
 *    PARTY  FOR  DIRECT,   INDIRECT,   SPECIAL,   INCIDENTAL,   OR
 *    CONSEQUENTIAL   DAMAGES  ARISING  OUT  OF  THE  USE  OF  THIS
 *    SOFTWARE, ITS DOCUMENTATION, OR ANY DERIVATIVES THEREOF, EVEN
 *    IF  THE  AUTHOR  HAVE BEEN ADVISED OF THE POSSIBILITY OF SUCH
 *    DAMAGE.
 *
 *    THE  AUTHOR  AND  DISTRIBUTORS  SPECIFICALLY   DISCLAIM   ANY
 *    WARRANTIES,  INCLUDING,  BUT  NOT  LIMITED  TO,  THE  IMPLIED
 *    WARRANTIES  OF  MERCHANTABILITY,  FITNESS  FOR  A  PARTICULAR
 *    PURPOSE,  AND NON-INFRINGEMENT.  THIS SOFTWARE IS PROVIDED ON
 *    AN "AS IS" BASIS, AND THE AUTHOR  AND  DISTRIBUTORS  HAVE  NO
 *    OBLIGATION   TO   PROVIDE   MAINTENANCE,   SUPPORT,  UPDATES,
 *    ENHANCEMENTS, OR MODIFICATIONS.
 *
 **********************************************************************/

#include "stdio.h"
#include "string.h"
#include "plpgsql.h"

#include "pl_scan.c"

static	PLpgSQL_expr	*read_sqlstmt(int until, char *s, char *sqlstart);
static	PLpgSQL_stmt	*make_select_stmt(void);
static	PLpgSQL_expr	*make_tupret_expr(PLpgSQL_row *row);

%}

%union {
	int32			ival;
	char			*str;
	struct {
	    char *name;
	    int  lineno;
	}			varname;
	struct {
	    int  nalloc;
	    int	 nused;
	    int	 *dtnums;
	}			dtlist;
	struct {
	    int  reverse;
	    PLpgSQL_expr *expr;
	}			forilow;
	struct {
	    char *label;
	    int  n_initvars;
	    int  *initvarnos;
	}			declhdr;
	PLpgSQL_type		*dtype;
	PLpgSQL_var		*var;
	PLpgSQL_row		*row;
	PLpgSQL_rec		*rec;
	PLpgSQL_recfield	*recfield;
	PLpgSQL_trigarg		*trigarg;
	PLpgSQL_expr		*expr;
	PLpgSQL_stmt		*stmt;
	PLpgSQL_stmts		*stmts;
	PLpgSQL_stmt_block	*program;
	PLpgSQL_nsitem		*nsitem;
}

%type <declhdr>	decl_sect
%type <varname>	decl_varname
%type <str>	decl_renname
%type <ival>	decl_const, decl_notnull, decl_atttypmod, decl_atttypmodval
%type <expr>	decl_defval
%type <dtype>	decl_datatype, decl_dtypename
%type <row>	decl_rowtype
%type <nsitem>	decl_aliasitem
%type <str>	decl_stmts, decl_stmt

%type <expr>	expr_until_semi, expr_until_then, expr_until_loop
%type <expr>	opt_exitcond

%type <ival>	assign_var
%type <var>	fori_var
%type <varname>	fori_varname
%type <forilow>	fori_lower
%type <rec>	fors_target

%type <str>	opt_lblname, opt_label
%type <str>	opt_exitlabel
%type <str>	execsql_start

%type <stmts>	proc_sect, proc_stmts, stmt_else, loop_body
%type <stmt>	proc_stmt, pl_block
%type <stmt>	stmt_assign, stmt_if, stmt_loop, stmt_while, stmt_exit
%type <stmt>	stmt_return, stmt_raise, stmt_execsql, stmt_fori
%type <stmt>	stmt_fors, stmt_select, stmt_perform

%type <dtlist>	raise_params
%type <ival>	raise_level, raise_param
%type <str>	raise_msg

%type <ival>	lno

	/*
	 * Keyword tokens
	 */
%token	K_ALIAS
%token	K_ASSIGN
%token	K_BEGIN
%token	K_CONSTANT
%token	K_DEBUG
%token	K_DECLARE
%token	K_DEFAULT
%token	K_DOTDOT
%token	K_ELSE
%token	K_END
%token	K_EXCEPTION
%token	K_EXIT
%token	K_FOR
%token	K_FROM
%token	K_IF
%token	K_IN
%token	K_INTO
%token	K_LOOP
%token	K_NOT
%token	K_NOTICE
%token	K_NULL
%token	K_PERFORM
%token	K_RAISE
%token	K_RECORD
%token	K_RENAME
%token	K_RETURN
%token	K_REVERSE
%token	K_SELECT
%token	K_THEN
%token	K_TO
%token	K_TYPE
%token	K_WHEN
%token	K_WHILE

	/*
	 * Other tokens
	 */
%token	T_FUNCTION
%token	T_TRIGGER
%token	T_CHAR
%token	T_BPCHAR
%token	T_VARCHAR
%token	T_LABEL
%token	T_STRING
%token	T_VARIABLE
%token	T_ROW
%token	T_ROWTYPE
%token	T_RECORD
%token	T_RECFIELD
%token	T_TGARGV
%token	T_DTYPE
%token	T_WORD
%token	T_NUMBER
%token	T_ERROR

%token	O_OPTION
%token	O_DUMP

%%

pl_function	: T_FUNCTION comp_optsect pl_block
		    {
			yylval.program = (PLpgSQL_stmt_block *)$3;
		    }
		| T_TRIGGER comp_optsect pl_block
		    {
			yylval.program = (PLpgSQL_stmt_block *)$3;
		    }
		;

comp_optsect	:
		| comp_options
		;

comp_options	: comp_options comp_option
		| comp_option
		;

comp_option	: O_OPTION O_DUMP
		    {
		        plpgsql_DumpExecTree = 1;
		    }
		;

pl_block	: decl_sect K_BEGIN lno proc_sect K_END ';'
		    {
		        PLpgSQL_stmt_block *new;

			new = malloc(sizeof(PLpgSQL_stmt_block));
			memset(new, 0, sizeof(PLpgSQL_stmt_block));

			new->cmd_type   = PLPGSQL_STMT_BLOCK;
			new->lineno     = $3;
			new->label      = $1.label;
			new->n_initvars = $1.n_initvars;
			new->initvarnos = $1.initvarnos;
			new->body       = $4;

			plpgsql_ns_pop();

			$$ = (PLpgSQL_stmt *)new;
		    }
		;


decl_sect	: opt_label
		    {
		        plpgsql_ns_setlocal(false);
			$$.label      = $1;
			$$.n_initvars = 0;
			$$.initvarnos = NULL;
			plpgsql_add_initdatums(NULL);
		    }
		| opt_label decl_start
		    {
		        plpgsql_ns_setlocal(false);
			$$.label      = $1;
			$$.n_initvars = 0;
			$$.initvarnos = NULL;
			plpgsql_add_initdatums(NULL);
		    }
		| opt_label decl_start decl_stmts
		    {
		        plpgsql_ns_setlocal(false);
			if ($3 != NULL) {
			    $$.label = $3;
			} else {
			    $$.label = $1;
			}
			$$.n_initvars = plpgsql_add_initdatums(&($$.initvarnos));
		    }
		;

decl_start	: K_DECLARE
		    {
		        plpgsql_ns_setlocal(true);
		    }
		;

decl_stmts	: decl_stmts decl_stmt
		    {
		        $$ = $2;
		    }
		| decl_stmt
		    {
		        $$ = $1;
		    }
		;

decl_stmt	: '<' '<' opt_lblname '>' '>'
		    {
			$$ = $3;
		    }
		| K_DECLARE
		    {
		        $$ = NULL;
		    }
		| decl_statement
		    {
		        $$ = NULL;
		    }
		;

decl_statement	: decl_varname decl_const decl_datatype decl_notnull decl_defval
		    {
		        PLpgSQL_var	*new;

			new = malloc(sizeof(PLpgSQL_var));

			new->dtype	= PLPGSQL_DTYPE_VAR;
			new->refname	= $1.name;
			new->lineno	= $1.lineno;

			new->datatype	= $3;
			new->isconst	= $2;
			new->notnull	= $4;
			new->default_val = $5;

			plpgsql_adddatum((PLpgSQL_datum *)new);
			plpgsql_ns_additem(PLPGSQL_NSTYPE_VAR, new->varno,
						$1.name);
		    }
		| decl_varname K_RECORD ';'
		    {
		        PLpgSQL_rec	*new;

			new = malloc(sizeof(PLpgSQL_var));

			new->dtype	= PLPGSQL_DTYPE_REC;
			new->refname	= $1.name;
			new->lineno	= $1.lineno;

			plpgsql_adddatum((PLpgSQL_datum *)new);
			plpgsql_ns_additem(PLPGSQL_NSTYPE_REC, new->recno,
						$1.name);
		    }
		| decl_varname decl_rowtype ';'
		    {
			$2->dtype	= PLPGSQL_DTYPE_ROW;
			$2->refname	= $1.name;
			$2->lineno	= $1.lineno;

			plpgsql_adddatum((PLpgSQL_datum *)$2);
			plpgsql_ns_additem(PLPGSQL_NSTYPE_ROW, $2->rowno,
						$1.name);
		    }
		| decl_varname K_ALIAS K_FOR decl_aliasitem ';'
		    {
		        plpgsql_ns_additem($4->itemtype,
					$4->itemno, $1.name);
		    }
		| K_RENAME decl_renname K_TO decl_renname ';'
		    {
		        plpgsql_ns_rename($2, $4);
		    }
		;

decl_aliasitem	: T_WORD
		    {
		        PLpgSQL_nsitem *nsi;
			char	*name;

			plpgsql_ns_setlocal(false);
			name = plpgsql_tolower(yytext);
			if (name[0] != '$') {
			    elog(ERROR, "can only alias positional parameters");
			}
			nsi = plpgsql_ns_lookup(name, NULL);
			if (nsi == NULL) {
			    elog(ERROR, "function has no parameter %s", name);
			}

			plpgsql_ns_setlocal(true);

			$$ = nsi;
		    }
		;

decl_rowtype	: T_ROW
		    {
		        $$ = yylval.row;
		    }
		;

decl_varname	: T_WORD
		    {
		        $$.name = strdup(yytext);
			$$.lineno  = yylineno;
		    }
		;

decl_renname	: T_WORD
		    {
		        $$ = plpgsql_tolower(yytext);
		    }
		;

decl_const	:
		    { $$ = 0; }
		| K_CONSTANT
		    { $$ = 1; }
		;

decl_datatype	: decl_dtypename
		    {
		        $$ = $1;
		    }
		;

decl_dtypename	: T_DTYPE
		    {
			$$ = yylval.dtype;
		    }
		| T_CHAR decl_atttypmod
		    {
		        if ($2 < 0) {
			    plpgsql_parse_word("char");
			    $$ = yylval.dtype;
			} else {
			    plpgsql_parse_word("bpchar");
			    $$ = yylval.dtype;
			    $$->atttypmod = $2;
			}
		    }
		| T_VARCHAR decl_atttypmod
		    {
		        plpgsql_parse_word("varchar");
			$$ = yylval.dtype;
			$$->atttypmod = $2;
		    }
		| T_BPCHAR '(' decl_atttypmodval ')'
		    {
		        plpgsql_parse_word("bpchar");
			$$ = yylval.dtype;
			$$->atttypmod = $3;
		    }
		;

decl_atttypmod	:
		    {
		        $$ = -1;
		    }
		| '(' decl_atttypmodval ')'
		    {
		        $$ = $2;
		    }
		;

decl_atttypmodval	: T_NUMBER
		    {
		        $$ = int2in(yytext) + VARHDRSZ;
		    }
		;

decl_notnull	:
		    { $$ = 0; }
		| K_NOT K_NULL
		    { $$ = 1; }
		;

decl_defval	: ';'
		    { $$ = NULL; }
		| decl_defkey
		    {
			int		tok;
			int		lno;
		        PLpgSQL_dstring	ds;
			PLpgSQL_expr	*expr;

			lno = yylineno;
			expr = malloc(sizeof(PLpgSQL_expr));
			plpgsql_dstring_init(&ds);
			plpgsql_dstring_append(&ds, "SELECT ");

			expr->dtype   = PLPGSQL_DTYPE_EXPR;
			expr->plan    = NULL;
			expr->nparams = 0;

			tok = yylex();
			switch (tok) {
			    case 0:
				plpgsql_error_lineno = lno;
				plpgsql_comperrinfo();
			    	elog(ERROR, "unexpected end of file");
			    case K_NULL:
			        if (yylex() != ';') {
				    plpgsql_error_lineno = lno;
				    plpgsql_comperrinfo();
				    elog(ERROR, "expectec ; after NULL");
				}
				free(expr);
				plpgsql_dstring_free(&ds);

				$$ = NULL;
				break;

			    default:
				plpgsql_dstring_append(&ds, yytext);
				while ((tok = yylex()) != ';') {
				    if (tok == 0) {
					plpgsql_error_lineno = lno;
					plpgsql_comperrinfo();
					elog(ERROR, "unterminated default value");
				    }
				    if (plpgsql_SpaceScanned) {
					plpgsql_dstring_append(&ds, " ");
				    }
				    plpgsql_dstring_append(&ds, yytext);
				}
				expr->query = strdup(plpgsql_dstring_get(&ds));
				plpgsql_dstring_free(&ds);

				$$ = expr;
				break;
			}
		    }
		;

decl_defkey	: K_ASSIGN
		| K_DEFAULT

proc_sect	:
			{
				PLpgSQL_stmts	*new;

				new = malloc(sizeof(PLpgSQL_stmts));
				memset(new, 0, sizeof(PLpgSQL_stmts));
				$$ = new;
			}
		| proc_stmts
			{
				$$ = $1;
			}
		;

proc_stmts	: proc_stmts proc_stmt
			{
				if ($1->stmts_used == $1->stmts_alloc) {
				    $1->stmts_alloc *= 2;
				    $1->stmts = realloc($1->stmts, sizeof(PLpgSQL_stmt *) * $1->stmts_alloc);
				}
				$1->stmts[$1->stmts_used++] = (struct PLpgSQL_stmt *)$2;

				$$ = $1;
			}
		| proc_stmt
			{
				PLpgSQL_stmts	*new;

				new = malloc(sizeof(PLpgSQL_stmts));
				memset(new, 0, sizeof(PLpgSQL_stmts));

				new->stmts_alloc = 64;
				new->stmts_used  = 1;
				new->stmts = malloc(sizeof(PLpgSQL_stmt *) * new->stmts_alloc);
				new->stmts[0] = (struct PLpgSQL_stmt *)$1;

				$$ = new;
			}
		;

proc_stmt	: pl_block
			{ $$ = $1; }
		| stmt_assign
			{ $$ = $1; }
		| stmt_if
			{ $$ = $1; }
		| stmt_loop
			{ $$ = $1; }
		| stmt_while
			{ $$ = $1; }
		| stmt_fori
			{ $$ = $1; }
		| stmt_fors
			{ $$ = $1; }
		| stmt_select
			{ $$ = $1; }
		| stmt_exit
			{ $$ = $1; }
		| stmt_return
			{ $$ = $1; }
		| stmt_raise
			{ $$ = $1; }
		| stmt_execsql
			{ $$ = $1; }
		| stmt_perform
			{ $$ = $1; }
		;

stmt_perform	: K_PERFORM lno expr_until_semi
		    {
		    	PLpgSQL_stmt_assign *new;

			new = malloc(sizeof(PLpgSQL_stmt_assign));
			memset(new, 0, sizeof(PLpgSQL_stmt_assign));

			new->cmd_type = PLPGSQL_STMT_ASSIGN;
			new->lineno   = $2;
			new->varno = -1;
			new->expr  = $3;

			$$ = (PLpgSQL_stmt *)new;
		    }
		;

stmt_assign	: assign_var lno K_ASSIGN expr_until_semi
		    {
			PLpgSQL_stmt_assign *new;

			new = malloc(sizeof(PLpgSQL_stmt_assign));
			memset(new, 0, sizeof(PLpgSQL_stmt_assign));

			new->cmd_type = PLPGSQL_STMT_ASSIGN;
			new->lineno   = $2;
			new->varno = $1;
			new->expr  = $4;

			$$ = (PLpgSQL_stmt *)new;
		    }
		;

assign_var	: T_VARIABLE
		    {
			if (yylval.var->isconst) {
			    plpgsql_comperrinfo();
			    elog(ERROR, "%s is declared CONSTANT", yylval.var->refname);
			}
		        $$ = yylval.var->varno;
		    }
		| T_RECFIELD
		    {
		        $$ = yylval.recfield->rfno;
		    }
		;

stmt_if		: K_IF lno expr_until_then proc_sect stmt_else K_END K_IF ';'
		    {
			PLpgSQL_stmt_if *new;

			new = malloc(sizeof(PLpgSQL_stmt_if));
			memset(new, 0, sizeof(PLpgSQL_stmt_if));

			new->cmd_type   = PLPGSQL_STMT_IF;
			new->lineno     = $2;
			new->cond       = $3;
			new->true_body  = $4;
			new->false_body = $5;

			$$ = (PLpgSQL_stmt *)new;
		    }
		;

stmt_else	:
			{
				PLpgSQL_stmts	*new;

				new = malloc(sizeof(PLpgSQL_stmts));
				memset(new, 0, sizeof(PLpgSQL_stmts));
				$$ = new;
			}
		| K_ELSE proc_sect
			{ $$ = $2; }
		;

stmt_loop	: opt_label K_LOOP lno loop_body
		    {
			PLpgSQL_stmt_loop *new;

			new = malloc(sizeof(PLpgSQL_stmt_loop));
			memset(new, 0, sizeof(PLpgSQL_stmt_loop));

			new->cmd_type = PLPGSQL_STMT_LOOP;
			new->lineno   = $3;
			new->label    = $1;
			new->body     = $4;

			plpgsql_ns_pop();

			$$ = (PLpgSQL_stmt *)new;
		    }
		;

stmt_while	: opt_label K_WHILE lno expr_until_loop loop_body
		    {
			PLpgSQL_stmt_while *new;

			new = malloc(sizeof(PLpgSQL_stmt_while));
			memset(new, 0, sizeof(PLpgSQL_stmt_while));

			new->cmd_type = PLPGSQL_STMT_WHILE;
			new->lineno   = $3;
			new->label    = $1;
			new->cond     = $4;
			new->body     = $5;

			plpgsql_ns_pop();

			$$ = (PLpgSQL_stmt *)new;
		    }
		;

stmt_fori	: opt_label K_FOR lno fori_var K_IN fori_lower expr_until_loop loop_body
		    {
			PLpgSQL_stmt_fori	*new;

			new = malloc(sizeof(PLpgSQL_stmt_fori));
			memset(new, 0, sizeof(PLpgSQL_stmt_fori));

			new->cmd_type = PLPGSQL_STMT_FORI;
			new->lineno   = $3;
			new->label    = $1;
			new->var      = $4;
			new->reverse  = $6.reverse;
			new->lower    = $6.expr;
			new->upper    = $7;
			new->body     = $8;

			plpgsql_ns_pop();

			$$ = (PLpgSQL_stmt *)new;
		    }
		;

fori_var	: fori_varname
		    {
		        PLpgSQL_var	*new;

			new = malloc(sizeof(PLpgSQL_var));

			new->dtype	= PLPGSQL_DTYPE_VAR;
			new->refname	= $1.name;
			new->lineno	= $1.lineno;

			plpgsql_parse_word("integer");

			new->datatype	= yylval.dtype;
			new->isconst	= false;
			new->notnull	= false;
			new->default_val = NULL;

			plpgsql_adddatum((PLpgSQL_datum *)new);
			plpgsql_ns_additem(PLPGSQL_NSTYPE_VAR, new->varno,
						$1.name);

			plpgsql_add_initdatums(NULL);

		        $$ = new;
		    }
		;

fori_varname	: T_VARIABLE
		    {
		        $$.name = strdup(yytext);
			$$.lineno = yylineno;
		    }
		| T_WORD
		    {
		        $$.name = strdup(yytext);
			$$.lineno = yylineno;
		    }
		;

fori_lower	:
		    {
			int			tok;
			int			lno;
			PLpgSQL_dstring	ds;
			int			nparams = 0;
			int			params[1024];
			char		buf[32];
			PLpgSQL_expr	*expr;
			int			firsttok = 1;

			lno = yylineno;
			plpgsql_dstring_init(&ds);
			plpgsql_dstring_append(&ds, "SELECT ");

			$$.reverse = 0;
			while((tok = yylex()) != K_DOTDOT) {
			    if (firsttok) {
				firsttok = 0;
				if (tok == K_REVERSE) {
				    $$.reverse = 1;
				    continue;
				}
			    }
			    if (tok == ';') break;
			    if (plpgsql_SpaceScanned) {
				plpgsql_dstring_append(&ds, " ");
			    }
			    switch (tok) {
				case T_VARIABLE:
				    params[nparams] = yylval.var->varno;
				    sprintf(buf, "$%d", ++nparams);
				    plpgsql_dstring_append(&ds, buf);
				    break;
				    
				case T_RECFIELD:
				    params[nparams] = yylval.recfield->rfno;
				    sprintf(buf, "$%d", ++nparams);
				    plpgsql_dstring_append(&ds, buf);
				    break;
				    
				case T_TGARGV:
				    params[nparams] = yylval.trigarg->dno;
				    sprintf(buf, "$%d", ++nparams);
				    plpgsql_dstring_append(&ds, buf);
				    break;
				    
				default:
				    if (tok == 0) {
					plpgsql_error_lineno = lno;
					plpgsql_comperrinfo();
					elog(ERROR, "missing .. to terminate lower bound of for loop");
				    }
				    plpgsql_dstring_append(&ds, yytext);
				    break;
			    }
			}

			expr = malloc(sizeof(PLpgSQL_expr) + sizeof(int) * nparams - 1);
			expr->dtype		= PLPGSQL_DTYPE_EXPR;
			expr->query		= strdup(plpgsql_dstring_get(&ds));
			expr->plan		= NULL;
			expr->nparams	= nparams;
			while(nparams-- > 0) {
			    expr->params[nparams] = params[nparams];
			}
			plpgsql_dstring_free(&ds);
			$$.expr = expr;
		    }

stmt_fors	: opt_label K_FOR lno fors_target K_IN K_SELECT expr_until_loop loop_body
		    {
			PLpgSQL_stmt_fors	*new;

			new = malloc(sizeof(PLpgSQL_stmt_fors));
			memset(new, 0, sizeof(PLpgSQL_stmt_fors));

			new->cmd_type = PLPGSQL_STMT_FORS;
			new->lineno   = $3;
			new->label    = $1;
			switch ($4->dtype) {
			    case PLPGSQL_DTYPE_REC:
			        new->rec = $4;
				break;
			    case PLPGSQL_DTYPE_ROW:
			        new->row = (PLpgSQL_row *)$4;
				break;
			    default:
				plpgsql_comperrinfo();
			        elog(ERROR, "unknown dtype %d in stmt_fors", $4->dtype);
			}
			new->query = $7;
			new->body  = $8;

			plpgsql_ns_pop();

			$$ = (PLpgSQL_stmt *)new;
		    }

fors_target	: T_RECORD
		    {
		        $$ = yylval.rec;
		    }
		| T_ROW
		    {
		    	$$ = (PLpgSQL_rec *)(yylval.row);
		    }
		;

stmt_select	: K_SELECT lno
		    {
		    	$$ = make_select_stmt();
			$$->lineno = $2;
		    }
		;

stmt_exit	: K_EXIT lno opt_exitlabel opt_exitcond
		    {
			PLpgSQL_stmt_exit *new;

			new = malloc(sizeof(PLpgSQL_stmt_exit));
			memset(new, 0, sizeof(PLpgSQL_stmt_exit));

			new->cmd_type = PLPGSQL_STMT_EXIT;
			new->lineno   = $2;
			new->label    = $3;
			new->cond     = $4;

			$$ = (PLpgSQL_stmt *)new;
		    }
		;

stmt_return	: K_RETURN lno
		    {
			PLpgSQL_stmt_return *new;
			PLpgSQL_expr	*expr = NULL;
			int		tok;

			new = malloc(sizeof(PLpgSQL_stmt_return));
			memset(new, 0, sizeof(PLpgSQL_stmt_return));

			if (plpgsql_curr_compile->fn_retistuple) {
			    new->retistuple = true;
			    new->retrecno   = -1;
			    switch (tok = yylex()) {
			        case K_NULL:
				    expr = NULL;
				    break;

			        case T_ROW:
				    expr = make_tupret_expr(yylval.row);
				    break;

				case T_RECORD:
				    new->retrecno = yylval.rec->recno;
				    expr = NULL;
				    break;

				default:
				    yyerror("return type mismatch in function returning table row");
				    break;
			    }
			    if (yylex() != ';') {
			        yyerror("expected ';'");
			    }
			} else {
			    new->retistuple = false;
			    expr = plpgsql_read_expression(';', ";");
			}

			new->cmd_type = PLPGSQL_STMT_RETURN;
			new->lineno   = $2;
			new->expr     = expr;

			$$ = (PLpgSQL_stmt *)new;
		    }
		;

stmt_raise	: K_RAISE lno raise_level raise_msg raise_params ';'
		    {
		        PLpgSQL_stmt_raise	*new;

			new = malloc(sizeof(PLpgSQL_stmt_raise));

			new->cmd_type	= PLPGSQL_STMT_RAISE;
			new->lineno     = $2;
			new->elog_level	= $3;
			new->message	= $4;
			new->nparams	= $5.nused;
			new->params	= malloc(sizeof(int) * $5.nused);
			memcpy(new->params, $5.dtnums, sizeof(int) * $5.nused);

			$$ = (PLpgSQL_stmt *)new;
		    }
		| K_RAISE lno raise_level raise_msg ';'
		    {
		        PLpgSQL_stmt_raise	*new;

			new = malloc(sizeof(PLpgSQL_stmt_raise));

			new->cmd_type	= PLPGSQL_STMT_RAISE;
			new->lineno     = $2;
			new->elog_level	= $3;
			new->message	= $4;
			new->nparams	= 0;
			new->params	= NULL;

			$$ = (PLpgSQL_stmt *)new;
		    }
		;

raise_msg	: T_STRING
		    {
		        $$ = strdup(yytext);
		    }
		;

raise_level	: K_EXCEPTION
		    {
		        $$ = ERROR;
		    }
		| K_NOTICE
		    {
		        $$ = NOTICE;
		    }
		| K_DEBUG
		    {
		        $$ = DEBUG;
		    }
		;

raise_params	: raise_params raise_param
		    {
		        if ($1.nused == $1.nalloc) {
			    $1.nalloc *= 2;
			    $1.dtnums = repalloc($1.dtnums, sizeof(int) * $1.nalloc);
			}
			$1.dtnums[$1.nused++] = $2;

			$$.nalloc = $1.nalloc;
			$$.nused  = $1.nused;
			$$.dtnums = $1.dtnums;
		    }
		| raise_param
		    {
		        $$.nalloc = 1;
			$$.nused  = 1;
			$$.dtnums = palloc(sizeof(int) * $$.nalloc);
			$$.dtnums[0] = $1;
		    }
		;

raise_param	: ',' T_VARIABLE
		    {
		        $$ = yylval.var->varno;
		    }
		| ',' T_RECFIELD
		    {
		        $$ = yylval.recfield->rfno;
		    }
		| ',' T_TGARGV
		    {
		        $$ = yylval.trigarg->dno;
		    }
		;

loop_body	: proc_sect K_END K_LOOP ';'
		    { $$ = $1; }
		;

stmt_execsql	: execsql_start lno
		    {
		        PLpgSQL_stmt_execsql	*new;

			new = malloc(sizeof(PLpgSQL_stmt_execsql));
			new->cmd_type = PLPGSQL_STMT_EXECSQL;
			new->lineno   = $2;
			new->sqlstmt  = read_sqlstmt(';', ";", $1);

			$$ = (PLpgSQL_stmt *)new;
		    }
		;

execsql_start	: T_WORD
		    { $$ = strdup(yytext); }
		| T_ERROR
		    { $$ = strdup(yytext); }
		;

expr_until_semi	:
		    { $$ = plpgsql_read_expression(';', ";"); }
		;

expr_until_then	:
		    { $$ = plpgsql_read_expression(K_THEN, "THEN"); }
		;

expr_until_loop	:
		    { $$ = plpgsql_read_expression(K_LOOP, "LOOP"); }
		;

opt_label	:
		    {
			plpgsql_ns_push(NULL);
			$$ = NULL;
		    }
		| '<' '<' opt_lblname '>' '>'
		    {
			plpgsql_ns_push($3);
			$$ = $3;
		    }
		;

opt_exitlabel	:
		    { $$ = NULL; }
		| T_LABEL
		    { $$ = strdup(yytext); }
		;

opt_exitcond	: ';'
		    { $$ = NULL; }
		| K_WHEN expr_until_semi
		    { $$ = $2; }
		;

opt_lblname	: T_WORD
		    { $$ = strdup(yytext); }
		;

lno		:
		    {
			plpgsql_error_lineno = yylineno;
		        $$ = yylineno;
		    }
		;

%%

PLpgSQL_expr *
plpgsql_read_expression (int until, char *s)
{
    return read_sqlstmt(until, s, "SELECT ");
}


static PLpgSQL_expr *
read_sqlstmt (int until, char *s, char *sqlstart)
{
    int			tok;
    int			lno;
    PLpgSQL_dstring	ds;
    int			nparams = 0;
    int			params[1024];
    char		buf[32];
    PLpgSQL_expr	*expr;

    lno = yylineno;
    plpgsql_dstring_init(&ds);
    plpgsql_dstring_append(&ds, sqlstart);

    while((tok = yylex()) != until) {
	if (tok == ';') break;
	if (plpgsql_SpaceScanned) {
	    plpgsql_dstring_append(&ds, " ");
	}
        switch (tok) {
	    case T_VARIABLE:
		params[nparams] = yylval.var->varno;
		sprintf(buf, "$%d", ++nparams);
		plpgsql_dstring_append(&ds, buf);
		break;
	        
	    case T_RECFIELD:
		params[nparams] = yylval.recfield->rfno;
		sprintf(buf, "$%d", ++nparams);
		plpgsql_dstring_append(&ds, buf);
		break;
	        
	    case T_TGARGV:
		params[nparams] = yylval.trigarg->dno;
		sprintf(buf, "$%d", ++nparams);
		plpgsql_dstring_append(&ds, buf);
		break;
	        
	    default:
		if (tok == 0) {
		    plpgsql_error_lineno = lno;
		    plpgsql_comperrinfo();
		    elog(ERROR, "missing %s at end of SQL statement", s);
		}
		plpgsql_dstring_append(&ds, yytext);
		break;
        }
    }

    expr = malloc(sizeof(PLpgSQL_expr) + sizeof(int) * nparams - 1);
    expr->dtype		= PLPGSQL_DTYPE_EXPR;
    expr->query		= strdup(plpgsql_dstring_get(&ds));
    expr->plan		= NULL;
    expr->nparams	= nparams;
    while(nparams-- > 0) {
        expr->params[nparams] = params[nparams];
    }
    plpgsql_dstring_free(&ds);
    
    return expr;
}


static PLpgSQL_stmt *
make_select_stmt()
{
    int			tok;
    int			lno;
    PLpgSQL_dstring	ds;
    int			nparams = 0;
    int			params[1024];
    char		buf[32];
    PLpgSQL_expr	*expr;
    PLpgSQL_row		*row = NULL;
    PLpgSQL_rec		*rec = NULL;
    PLpgSQL_stmt_select	*select;
    int			have_nexttok = 0;

    lno = yylineno;
    plpgsql_dstring_init(&ds);
    plpgsql_dstring_append(&ds, "SELECT ");

    while((tok = yylex()) != K_INTO) {
	if (tok == ';') {
	    PLpgSQL_stmt_execsql	*execsql;

	    expr = malloc(sizeof(PLpgSQL_expr) + sizeof(int) * nparams - 1);
	    expr->dtype		= PLPGSQL_DTYPE_EXPR;
	    expr->query		= strdup(plpgsql_dstring_get(&ds));
	    expr->plan		= NULL;
	    expr->nparams	= nparams;
	    while(nparams-- > 0) {
		expr->params[nparams] = params[nparams];
	    }
	    plpgsql_dstring_free(&ds);

	    execsql = malloc(sizeof(PLpgSQL_stmt_execsql));
	    execsql->cmd_type = PLPGSQL_STMT_EXECSQL;
	    execsql->sqlstmt  = expr;

	    return (PLpgSQL_stmt *)execsql;
	}

	if (plpgsql_SpaceScanned) {
	    plpgsql_dstring_append(&ds, " ");
	}
        switch (tok) {
	    case T_VARIABLE:
		params[nparams] = yylval.var->varno;
		sprintf(buf, "$%d", ++nparams);
		plpgsql_dstring_append(&ds, buf);
		break;
	        
	    case T_RECFIELD:
		params[nparams] = yylval.recfield->rfno;
		sprintf(buf, "$%d", ++nparams);
		plpgsql_dstring_append(&ds, buf);
		break;
	        
	    case T_TGARGV:
		params[nparams] = yylval.trigarg->dno;
		sprintf(buf, "$%d", ++nparams);
		plpgsql_dstring_append(&ds, buf);
		break;
	        
	    default:
		if (tok == 0) {
		    plpgsql_error_lineno = yylineno;
		    plpgsql_comperrinfo();
		    elog(ERROR, "unexpected end of file");
		}
		plpgsql_dstring_append(&ds, yytext);
		break;
        }
    }

    tok = yylex();
    switch (tok) {
        case T_ROW:
	    row = yylval.row;
	    break;

        case T_RECORD:
	    rec = yylval.rec;
	    break;

	case T_VARIABLE:
	case T_RECFIELD:
	    {
		PLpgSQL_var	*var;
		PLpgSQL_recfield *recfield;
		int		nfields = 1;
		char		*fieldnames[1024];
		int		varnos[1024];

		switch (tok) {
		    case T_VARIABLE:
			var = yylval.var;
			fieldnames[0] = strdup(yytext);
			varnos[0]     = var->varno;
			break;
		    
		    case T_RECFIELD:
			recfield = yylval.recfield;
			fieldnames[0] = strdup(yytext);
			varnos[0]     = recfield->rfno;
			break;
		}

		while ((tok = yylex()) == ',') {
		    tok = yylex();
		    switch(tok) {
			case T_VARIABLE:
			    var = yylval.var;
			    fieldnames[nfields] = strdup(yytext);
			    varnos[nfields++]   = var->varno;
			    break;

			case T_RECFIELD:
			    recfield = yylval.recfield;
			    fieldnames[0] = strdup(yytext);
			    varnos[0]     = recfield->rfno;
			    break;

			default:
			    elog(ERROR, "plpgsql: %s is not a variable or record field", yytext);
		    }
		}
		row = malloc(sizeof(PLpgSQL_row));
		row->dtype = PLPGSQL_DTYPE_ROW;
		row->refname = strdup("*internal*");
		row->lineno = yylineno;
		row->rowtypeclass = InvalidOid;
		row->nfields = nfields;
		row->fieldnames = malloc(sizeof(char *) * nfields);
		row->varnos = malloc(sizeof(int) * nfields);
		while (--nfields >= 0) {
		    row->fieldnames[nfields] = fieldnames[nfields];
		    row->varnos[nfields] = varnos[nfields];
		}

		plpgsql_adddatum((PLpgSQL_datum *)row);

		have_nexttok = 1;
	    }
	    break;

        default:
	    {
		if (plpgsql_SpaceScanned) {
		    plpgsql_dstring_append(&ds, " ");
		}
		plpgsql_dstring_append(&ds, yytext);

		while(1) {
		    tok = yylex();
		    if (tok == ';') {
			PLpgSQL_stmt_execsql	*execsql;

			expr = malloc(sizeof(PLpgSQL_expr) + sizeof(int) * nparams - 1);
			expr->dtype		= PLPGSQL_DTYPE_EXPR;
			expr->query		= strdup(plpgsql_dstring_get(&ds));
			expr->plan		= NULL;
			expr->nparams	= nparams;
			while(nparams-- > 0) {
			    expr->params[nparams] = params[nparams];
			}
			plpgsql_dstring_free(&ds);

			execsql = malloc(sizeof(PLpgSQL_stmt_execsql));
			execsql->cmd_type = PLPGSQL_STMT_EXECSQL;
			execsql->sqlstmt  = expr;

			return (PLpgSQL_stmt *)execsql;
		    }

		    if (plpgsql_SpaceScanned) {
			plpgsql_dstring_append(&ds, " ");
		    }
		    switch (tok) {
			case T_VARIABLE:
			    params[nparams] = yylval.var->varno;
			    sprintf(buf, "$%d", ++nparams);
			    plpgsql_dstring_append(&ds, buf);
			    break;
			    
			case T_RECFIELD:
			    params[nparams] = yylval.recfield->rfno;
			    sprintf(buf, "$%d", ++nparams);
			    plpgsql_dstring_append(&ds, buf);
			    break;
			    
			case T_TGARGV:
			    params[nparams] = yylval.trigarg->dno;
			    sprintf(buf, "$%d", ++nparams);
			    plpgsql_dstring_append(&ds, buf);
			    break;
			    
			default:
			    if (tok == 0) {
				plpgsql_error_lineno = yylineno;
				plpgsql_comperrinfo();
				elog(ERROR, "unexpected end of file");
			    }
			    plpgsql_dstring_append(&ds, yytext);
			    break;
		    }
		}
	    }
    }

    /************************************************************
     * Eat up the rest of the statement after the target fields
     ************************************************************/
    while(1) {
	if (!have_nexttok) {
	    tok = yylex();
	}
	have_nexttok = 0;
	if (tok == ';') {
	    break;
	}

	if (plpgsql_SpaceScanned) {
	    plpgsql_dstring_append(&ds, " ");
	}
	switch (tok) {
	    case T_VARIABLE:
		params[nparams] = yylval.var->varno;
		sprintf(buf, "$%d", ++nparams);
		plpgsql_dstring_append(&ds, buf);
		break;
		
	    case T_RECFIELD:
		params[nparams] = yylval.recfield->rfno;
		sprintf(buf, "$%d", ++nparams);
		plpgsql_dstring_append(&ds, buf);
		break;
		
	    case T_TGARGV:
		params[nparams] = yylval.trigarg->dno;
		sprintf(buf, "$%d", ++nparams);
		plpgsql_dstring_append(&ds, buf);
		break;
		
	    default:
		if (tok == 0) {
		    plpgsql_error_lineno = yylineno;
		    plpgsql_comperrinfo();
		    elog(ERROR, "unexpected end of file");
		}
		plpgsql_dstring_append(&ds, yytext);
		break;
	}
    }

    expr = malloc(sizeof(PLpgSQL_expr) + sizeof(int) * (nparams - 1));
    expr->dtype		= PLPGSQL_DTYPE_EXPR;
    expr->query		= strdup(plpgsql_dstring_get(&ds));
    expr->plan		= NULL;
    expr->nparams	= nparams;
    while(nparams-- > 0) {
        expr->params[nparams] = params[nparams];
    }
    plpgsql_dstring_free(&ds);

    select = malloc(sizeof(PLpgSQL_stmt_select));
    memset(select, 0, sizeof(PLpgSQL_stmt_select));
    select->cmd_type = PLPGSQL_STMT_SELECT;
    select->rec      = rec;
    select->row      = row;
    select->query    = expr;
    
    return (PLpgSQL_stmt *)select;
}


static PLpgSQL_expr *
make_tupret_expr(PLpgSQL_row *row)
{
    PLpgSQL_dstring	ds;
    PLpgSQL_expr	*expr;
    int			i;
    char		buf[16];

    expr = malloc(sizeof(PLpgSQL_expr) + sizeof(int) * (row->nfields - 1));
    expr->dtype		= PLPGSQL_DTYPE_EXPR;

    plpgsql_dstring_init(&ds);
    plpgsql_dstring_append(&ds, "SELECT ");

    for (i = 0; i < row->nfields; i++) {
        sprintf(buf, "%s$%d", (i > 0) ? "," : "", i + 1);
	plpgsql_dstring_append(&ds, buf);
	expr->params[i] = row->varnos[i];
    }

    expr->query         = strdup(plpgsql_dstring_get(&ds));
    expr->plan          = NULL;
    expr->plan_argtypes = NULL;
    expr->nparams       = row->nfields;

    plpgsql_dstring_free(&ds);
    return expr;
}
