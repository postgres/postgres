/**********************************************************************
 * plpgsql.h		- Definitions for the PL/pgSQL
 *			  procedural language
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/pl/plpgsql/src/plpgsql.h,v 1.3 1999/01/27 16:15:22 wieck Exp $
 *
 *	  This software is copyrighted by Jan Wieck - Hamburg.
 *
 *	  The author hereby grants permission  to  use,  copy,	modify,
 *	  distribute,  and	license this software and its documentation
 *	  for any purpose, provided that existing copyright notices are
 *	  retained	in	all  copies  and  that	this notice is included
 *	  verbatim in any distributions. No written agreement, license,
 *	  or  royalty  fee	is required for any of the authorized uses.
 *	  Modifications to this software may be  copyrighted  by  their
 *	  author  and  need  not  follow  the licensing terms described
 *	  here, provided that the new terms are  clearly  indicated  on
 *	  the first page of each file where they apply.
 *
 *	  IN NO EVENT SHALL THE AUTHOR OR DISTRIBUTORS BE LIABLE TO ANY
 *	  PARTY  FOR  DIRECT,	INDIRECT,	SPECIAL,   INCIDENTAL,	 OR
 *	  CONSEQUENTIAL   DAMAGES  ARISING	OUT  OF  THE  USE  OF  THIS
 *	  SOFTWARE, ITS DOCUMENTATION, OR ANY DERIVATIVES THEREOF, EVEN
 *	  IF  THE  AUTHOR  HAVE BEEN ADVISED OF THE POSSIBILITY OF SUCH
 *	  DAMAGE.
 *
 *	  THE  AUTHOR  AND	DISTRIBUTORS  SPECIFICALLY	 DISCLAIM	ANY
 *	  WARRANTIES,  INCLUDING,  BUT	NOT  LIMITED  TO,  THE	IMPLIED
 *	  WARRANTIES  OF  MERCHANTABILITY,	FITNESS  FOR  A  PARTICULAR
 *	  PURPOSE,	AND NON-INFRINGEMENT.  THIS SOFTWARE IS PROVIDED ON
 *	  AN "AS IS" BASIS, AND THE AUTHOR	AND  DISTRIBUTORS  HAVE  NO
 *	  OBLIGATION   TO	PROVIDE   MAINTENANCE,	 SUPPORT,  UPDATES,
 *	  ENHANCEMENTS, OR MODIFICATIONS.
 *
 **********************************************************************/
#ifndef PLPGSQL_H
#define PLPGSQL_H

#include <stdio.h>
#include <stdarg.h>
#include "postgres.h"
#include "executor/spi.h"
#include "commands/trigger.h"
#include "fmgr.h"

/**********************************************************************
 * Definitions
 **********************************************************************/

/* ----------
 * Compilers namestack item types
 * ----------
 */
enum
{
	PLPGSQL_NSTYPE_LABEL,
	PLPGSQL_NSTYPE_VAR,
	PLPGSQL_NSTYPE_ROW,
	PLPGSQL_NSTYPE_REC,
	PLPGSQL_NSTYPE_RECFIELD
};

/* ----------
 * Datum array node types
 * ----------
 */
enum
{
	PLPGSQL_DTYPE_VAR,
	PLPGSQL_DTYPE_ROW,
	PLPGSQL_DTYPE_REC,
	PLPGSQL_DTYPE_RECFIELD,
	PLPGSQL_DTYPE_EXPR,
	PLPGSQL_DTYPE_TRIGARG
};

/* ----------
 * Execution tree node types
 * ----------
 */
enum
{
	PLPGSQL_STMT_BLOCK,
	PLPGSQL_STMT_ASSIGN,
	PLPGSQL_STMT_IF,
	PLPGSQL_STMT_LOOP,
	PLPGSQL_STMT_WHILE,
	PLPGSQL_STMT_FORI,
	PLPGSQL_STMT_FORS,
	PLPGSQL_STMT_SELECT,
	PLPGSQL_STMT_EXIT,
	PLPGSQL_STMT_RETURN,
	PLPGSQL_STMT_RAISE,
	PLPGSQL_STMT_EXECSQL
};


/* ----------
 * Execution node return codes
 * ----------
 */
enum
{
	PLPGSQL_RC_OK,
	PLPGSQL_RC_EXIT,
	PLPGSQL_RC_RETURN
};

/**********************************************************************
 * Node and structure definitions
 **********************************************************************/


typedef struct
{								/* Dynamic string control structure */
	int			alloc;
	int			used;
	char	   *value;
}			PLpgSQL_dstring;


typedef struct
{								/* Postgres base data type		*/
	char	   *typname;
	Oid			typoid;
	FmgrInfo	typinput;
	bool		typbyval;
	int16		atttypmod;
}			PLpgSQL_type;


typedef struct
{								/* Generic datum array item		*/
	int			dtype;
	int			dno;
}			PLpgSQL_datum;


typedef struct
{								/* SQL Query to plan and execute	*/
	int			dtype;
	int			exprno;
	char	   *query;
	void	   *plan;
	Node	   *plan_simple_expr;
	Oid			plan_simple_type;
	Oid		   *plan_argtypes;
	int			nparams;
	int			params[1];
}			PLpgSQL_expr;


typedef struct
{								/* Local variable			*/
	int			dtype;
	int			varno;
	char	   *refname;
	int			lineno;

	PLpgSQL_type *datatype;
	int			isconst;
	int			notnull;
	PLpgSQL_expr *default_val;

	Datum		value;
	bool		isnull;
	int			shouldfree;
}			PLpgSQL_var;


typedef struct
{								/* Rowtype				*/
	int			dtype;
	int			rowno;
	char	   *refname;
	int			lineno;
	Oid			rowtypeclass;

	int			nfields;
	char	  **fieldnames;
	int		   *varnos;
}			PLpgSQL_row;


typedef struct
{								/* Record of undefined structure	*/
	int			dtype;
	int			recno;
	char	   *refname;
	int			lineno;

	HeapTuple	tup;
	TupleDesc	tupdesc;
}			PLpgSQL_rec;


typedef struct
{								/* Field in record			*/
	int			dtype;
	int			rfno;
	char	   *fieldname;
	int			recno;
}			PLpgSQL_recfield;


typedef struct
{								/* Positional argument to trigger	*/
	int			dtype;
	int			dno;
	PLpgSQL_expr *argnum;
}			PLpgSQL_trigarg;


typedef struct
{								/* Item in the compilers namestack	*/
	int			itemtype;
	int			itemno;
	char		name[1];
}			PLpgSQL_nsitem;


typedef struct PLpgSQL_ns
{								/* Compiler namestack level		*/
	int			items_alloc;
	int			items_used;
	PLpgSQL_nsitem **items;
	struct PLpgSQL_ns *upper;
}			PLpgSQL_ns;


typedef struct
{								/* List of execution nodes		*/
	int			stmts_alloc;
	int			stmts_used;
	struct PLpgSQL_stmt **stmts;
}			PLpgSQL_stmts;


typedef struct
{								/* Generic execution node		*/
	int			cmd_type;
	int			lineno;
}			PLpgSQL_stmt;


typedef struct
{								/* Block of statements			*/
	int			cmd_type;
	int			lineno;
	char	   *label;
	PLpgSQL_stmts *body;
	int			n_initvars;
	int		   *initvarnos;
}			PLpgSQL_stmt_block;


typedef struct
{								/* Assign statement			*/
	int			cmd_type;
	int			lineno;
	int			varno;
	PLpgSQL_expr *expr;
}			PLpgSQL_stmt_assign;


typedef struct
{								/* IF statement				*/
	int			cmd_type;
	int			lineno;
	PLpgSQL_expr *cond;
	PLpgSQL_stmts *true_body;
	PLpgSQL_stmts *false_body;
}			PLpgSQL_stmt_if;


typedef struct
{								/* Unconditional LOOP statement		*/
	int			cmd_type;
	int			lineno;
	char	   *label;
	PLpgSQL_stmts *body;
}			PLpgSQL_stmt_loop;


typedef struct
{								/* WHILE cond LOOP statement		*/
	int			cmd_type;
	int			lineno;
	char	   *label;
	PLpgSQL_expr *cond;
	PLpgSQL_stmts *body;
}			PLpgSQL_stmt_while;


typedef struct
{								/* FOR statement with integer loopvar	*/
	int			cmd_type;
	int			lineno;
	char	   *label;
	PLpgSQL_var *var;
	PLpgSQL_expr *lower;
	PLpgSQL_expr *upper;
	int			reverse;
	PLpgSQL_stmts *body;
}			PLpgSQL_stmt_fori;


typedef struct
{								/* FOR statement running over SELECT	*/
	int			cmd_type;
	int			lineno;
	char	   *label;
	PLpgSQL_rec *rec;
	PLpgSQL_row *row;
	PLpgSQL_expr *query;
	PLpgSQL_stmts *body;
}			PLpgSQL_stmt_fors;


typedef struct
{								/* SELECT ... INTO statement		*/
	int			cmd_type;
	int			lineno;
	PLpgSQL_rec *rec;
	PLpgSQL_row *row;
	PLpgSQL_expr *query;
}			PLpgSQL_stmt_select;


typedef struct
{								/* EXIT statement			*/
	int			cmd_type;
	int			lineno;
	char	   *label;
	PLpgSQL_expr *cond;
}			PLpgSQL_stmt_exit;


typedef struct
{								/* RETURN statement			*/
	int			cmd_type;
	int			lineno;
	bool		retistuple;
	PLpgSQL_expr *expr;
	int			retrecno;
}			PLpgSQL_stmt_return;


typedef struct
{								/* RAISE statement			*/
	int			cmd_type;
	int			lineno;
	int			elog_level;
	char	   *message;
	int			nparams;
	int		   *params;
}			PLpgSQL_stmt_raise;


typedef struct
{								/* Generic SQL statement to execute */
	int			cmd_type;
	int			lineno;
	PLpgSQL_expr *sqlstmt;
}			PLpgSQL_stmt_execsql;


typedef struct PLpgSQL_function
{								/* Complete compiled function	  */
	Oid			fn_oid;
	char	   *fn_name;
	int			fn_functype;
	Oid			fn_rettype;
	int			fn_rettyplen;
	bool		fn_retbyval;
	FmgrInfo	fn_retinput;
	bool		fn_retistuple;
	bool		fn_retset;

	int			fn_nargs;
	int			fn_argvarnos[MAXFMGRARGS];
	int			found_varno;
	int			new_varno;
	int			old_varno;
	int			tg_name_varno;
	int			tg_when_varno;
	int			tg_level_varno;
	int			tg_op_varno;
	int			tg_relid_varno;
	int			tg_relname_varno;
	int			tg_nargs_varno;

	int			ndatums;
	PLpgSQL_datum **datums;
	PLpgSQL_stmt_block *action;
	struct PLpgSQL_function *next;
}			PLpgSQL_function;


typedef struct
{								/* Runtime execution data	*/
	Datum		retval;
	bool		retisnull;
	Oid			rettype;
	bool		retistuple;
	TupleDesc	rettupdesc;
	bool		retisset;
	char	   *exitlabel;

	int			trig_nargs;
	Datum	   *trig_argv;

	int			found_varno;
	int			ndatums;
	PLpgSQL_datum **datums;
}			PLpgSQL_execstate;


/**********************************************************************
 * Global variable declarations
 **********************************************************************/

extern int	plpgsql_DumpExecTree;
extern int	plpgsql_SpaceScanned;
extern int	plpgsql_nDatums;
extern PLpgSQL_datum **plpgsql_Datums;

extern int	plpgsql_error_lineno;
extern char *plpgsql_error_funcname;

extern PLpgSQL_function *plpgsql_curr_compile;


/**********************************************************************
 * Function declarations
 **********************************************************************/


extern char *pstrdup(char *s);


/* ----------
 * Functions in pl_comp.c
 * ----------
 */
extern PLpgSQL_function *plpgsql_compile(Oid fn_oid, int functype);
extern int	plpgsql_parse_word(char *word);
extern int	plpgsql_parse_dblword(char *string);
extern int	plpgsql_parse_tripword(char *string);
extern int	plpgsql_parse_wordtype(char *string);
extern int	plpgsql_parse_dblwordtype(char *string);
extern int	plpgsql_parse_wordrowtype(char *string);
extern void plpgsql_adddatum(PLpgSQL_datum * new);
extern int	plpgsql_add_initdatums(int **varnos);
extern void plpgsql_comperrinfo(void);


/* ----------
 * Functions in pl_exec.c
 * ----------
 */
extern Datum plpgsql_exec_function(PLpgSQL_function * func,
					  FmgrValues *args, bool *isNull);
extern HeapTuple plpgsql_exec_trigger(PLpgSQL_function * func,
					 TriggerData *trigdata);


/* ----------
 * Functions for the dynamic string handling in pl_funcs.c
 * ----------
 */
extern void plpgsql_dstring_init(PLpgSQL_dstring * ds);
extern void plpgsql_dstring_free(PLpgSQL_dstring * ds);
extern void plpgsql_dstring_append(PLpgSQL_dstring * ds, char *str);
extern char *plpgsql_dstring_get(PLpgSQL_dstring * ds);

/* ----------
 * Functions for the namestack handling in pl_funcs.c
 * ----------
 */
extern void plpgsql_ns_init(void);
extern bool plpgsql_ns_setlocal(bool flag);
extern void plpgsql_ns_push(char *label);
extern void plpgsql_ns_pop(void);
extern void plpgsql_ns_additem(int itemtype, int itemno, char *name);
extern PLpgSQL_nsitem *plpgsql_ns_lookup(char *name, char *nsname);
extern void plpgsql_ns_rename(char *oldname, char *newname);

/* ----------
 * Other functions in pl_funcs.c
 * ----------
 */
extern void plpgsql_dumptree(PLpgSQL_function * func);
extern char *plpgsql_tolower(char *s);

/* ----------
 * Externs in gram.y and scan.l
 * ----------
 */
extern PLpgSQL_expr *plpgsql_read_expression(int until, char *s);
extern void plpgsql_yyrestart(FILE *fp);
extern int	plpgsql_yylex();
extern void plpgsql_setinput(char *s, int functype);
extern int	plpgsql_yyparse();


#endif	 /* PLPGSQL_H */
