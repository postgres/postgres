/**********************************************************************
 * pl_comp.c		- Compiler part of the PL/pgSQL
 *			  procedural language
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/pl/plpgsql/src/pl_comp.c,v 1.58 2003/05/05 16:46:27 tgl Exp $
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

#include "plpgsql.h"

#include <ctype.h>
#include <setjmp.h>

#include "pl.tab.h"

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/namespace.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_class.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "parser/gramparse.h"
#include "parser/parse_type.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/syscache.h"


/* ----------
 * Variables in the parser that shouldn't go into plpgsql.h
 * ----------
 */
extern PLPGSQL_YYSTYPE plpgsql_yylval;

/* ----------
 * Our own local and global variables
 * ----------
 */
static int	datums_alloc;
int			plpgsql_nDatums;
PLpgSQL_datum **plpgsql_Datums;
static int	datums_last = 0;

int			plpgsql_error_lineno;
char	   *plpgsql_error_funcname;
int			plpgsql_DumpExecTree = 0;

PLpgSQL_function *plpgsql_curr_compile;


static void plpgsql_compile_error_callback(void *arg);
static PLpgSQL_type *build_datatype(HeapTuple typeTup, int32 typmod);


/*
 * This routine is a crock, and so is everyplace that calls it.  The problem
 * is that the compiled form of a plpgsql function is allocated permanently
 * (mostly via malloc()) and never released until backend exit.  Subsidiary
 * data structures such as fmgr info records therefore must live forever
 * as well.  A better implementation would store all this stuff in a per-
 * function memory context that could be reclaimed at need.  In the meantime,
 * fmgr_info_cxt must be called specifying TopMemoryContext so that whatever
 * it might allocate, and whatever the eventual function might allocate using
 * fn_mcxt, will live forever too.
 */
static void
perm_fmgr_info(Oid functionId, FmgrInfo *finfo)
{
	fmgr_info_cxt(functionId, finfo, TopMemoryContext);
}


/* ----------
 * plpgsql_compile		Given a pg_proc's oid, make
 *						an execution tree for it.
 * ----------
 */
PLpgSQL_function *
plpgsql_compile(Oid fn_oid, int functype)
{
	int			parse_rc;
	HeapTuple	procTup;
	Form_pg_proc procStruct;
	HeapTuple	typeTup;
	Form_pg_type typeStruct;
	char	   *proc_source;
	PLpgSQL_function *function;
	PLpgSQL_var *var;
	PLpgSQL_row *row;
	PLpgSQL_rec *rec;
	int			i;
	int			arg_varnos[FUNC_MAX_ARGS];
	ErrorContextCallback plerrcontext;

	/*
	 * Lookup the pg_proc tuple by Oid
	 */
	procTup = SearchSysCache(PROCOID,
							 ObjectIdGetDatum(fn_oid),
							 0, 0, 0);
	if (!HeapTupleIsValid(procTup))
		elog(ERROR, "plpgsql: cache lookup for proc %u failed", fn_oid);

	/*
	 * Setup the scanner input and error info.  We assume that this function
	 * cannot be invoked recursively, so there's no need to save and restore
	 * the static variables used here.
	 */
	procStruct = (Form_pg_proc) GETSTRUCT(procTup);
	proc_source = DatumGetCString(DirectFunctionCall1(textout,
								  PointerGetDatum(&procStruct->prosrc)));
	plpgsql_scanner_init(proc_source, functype);
	pfree(proc_source);

	plpgsql_error_funcname = pstrdup(NameStr(procStruct->proname));
	plpgsql_error_lineno = 0;

	/*
	 * Setup error traceback support for ereport()
	 */
	plerrcontext.callback = plpgsql_compile_error_callback;
	plerrcontext.arg = NULL;
	plerrcontext.previous = error_context_stack;
	error_context_stack = &plerrcontext;

	/*
	 * Initialize the compiler
	 */
	plpgsql_ns_init();
	plpgsql_ns_push(NULL);
	plpgsql_DumpExecTree = 0;

	datums_alloc = 128;
	plpgsql_nDatums = 0;
	plpgsql_Datums = palloc(sizeof(PLpgSQL_datum *) * datums_alloc);
	datums_last = 0;

	/*
	 * Create the new function node
	 */
	function = malloc(sizeof(PLpgSQL_function));
	memset(function, 0, sizeof(PLpgSQL_function));
	plpgsql_curr_compile = function;

	function->fn_name = strdup(NameStr(procStruct->proname));
	function->fn_oid = fn_oid;
	function->fn_xmin = HeapTupleHeaderGetXmin(procTup->t_data);
	function->fn_cmin = HeapTupleHeaderGetCmin(procTup->t_data);
	function->fn_functype = functype;

	switch (functype)
	{
		case T_FUNCTION:

			/*
			 * Normal function has a defined returntype
			 */
			function->fn_rettype = procStruct->prorettype;
			function->fn_retset = procStruct->proretset;

			/*
			 * Lookup the functions return type
			 */
			typeTup = SearchSysCache(TYPEOID,
								ObjectIdGetDatum(procStruct->prorettype),
									 0, 0, 0);
			if (!HeapTupleIsValid(typeTup))
				elog(ERROR, "cache lookup for return type %u failed",
					 procStruct->prorettype);
			typeStruct = (Form_pg_type) GETSTRUCT(typeTup);

			/* Disallow pseudotype result, except VOID or RECORD */
			if (typeStruct->typtype == 'p')
			{
				if (procStruct->prorettype == VOIDOID ||
					procStruct->prorettype == RECORDOID)
					 /* okay */ ;
				else if (procStruct->prorettype == TRIGGEROID)
					elog(ERROR, "plpgsql functions cannot return type %s"
						 "\n\texcept when used as triggers",
						 format_type_be(procStruct->prorettype));
				else
					elog(ERROR, "plpgsql functions cannot return type %s",
						 format_type_be(procStruct->prorettype));
			}

			if (typeStruct->typrelid != InvalidOid ||
				procStruct->prorettype == RECORDOID)
				function->fn_retistuple = true;
			else
			{
				function->fn_retbyval = typeStruct->typbyval;
				function->fn_rettyplen = typeStruct->typlen;
				function->fn_rettypelem = typeStruct->typelem;
				perm_fmgr_info(typeStruct->typinput, &(function->fn_retinput));
			}
			ReleaseSysCache(typeTup);

			/*
			 * Create the variables for the procedures parameters
			 */
			for (i = 0; i < procStruct->pronargs; i++)
			{
				char		buf[32];

				snprintf(buf, sizeof(buf), "$%d", i + 1);		/* name for variable */

				/*
				 * Get the parameters type
				 */
				typeTup = SearchSysCache(TYPEOID,
							ObjectIdGetDatum(procStruct->proargtypes[i]),
										 0, 0, 0);
				if (!HeapTupleIsValid(typeTup))
					elog(ERROR, "cache lookup for argument type %u failed",
						 procStruct->proargtypes[i]);
				typeStruct = (Form_pg_type) GETSTRUCT(typeTup);

				/* Disallow pseudotype argument */
				if (typeStruct->typtype == 'p')
					elog(ERROR, "plpgsql functions cannot take type %s",
						 format_type_be(procStruct->proargtypes[i]));

				if (typeStruct->typrelid != InvalidOid)
				{
					/*
					 * For tuple type parameters, we set up a record of
					 * that type
					 */
					row = plpgsql_build_rowtype(typeStruct->typrelid);

					row->refname = strdup(buf);

					plpgsql_adddatum((PLpgSQL_datum *) row);
					plpgsql_ns_additem(PLPGSQL_NSTYPE_ROW, row->rowno,
									   row->refname);

					arg_varnos[i] = row->rowno;
				}
				else
				{
					/*
					 * Normal parameters get a var node
					 */
					var = malloc(sizeof(PLpgSQL_var));
					memset(var, 0, sizeof(PLpgSQL_var));

					var->dtype = PLPGSQL_DTYPE_VAR;
					var->refname = strdup(buf);
					var->lineno = 0;
					var->datatype = build_datatype(typeTup, -1);
					var->isconst = true;
					var->notnull = false;
					var->default_val = NULL;

					plpgsql_adddatum((PLpgSQL_datum *) var);
					plpgsql_ns_additem(PLPGSQL_NSTYPE_VAR, var->varno,
									   var->refname);

					arg_varnos[i] = var->varno;
				}
				ReleaseSysCache(typeTup);
			}
			break;

		case T_TRIGGER:

			/*
			 * Trigger procedures return type is unknown yet
			 */
			function->fn_rettype = InvalidOid;
			function->fn_retbyval = false;
			function->fn_retistuple = true;
			function->fn_retset = false;

			/*
			 * Add the record for referencing NEW
			 */
			rec = malloc(sizeof(PLpgSQL_rec));
			memset(rec, 0, sizeof(PLpgSQL_rec));
			rec->dtype = PLPGSQL_DTYPE_REC;
			rec->refname = strdup("new");
			rec->tup = NULL;
			rec->tupdesc = NULL;
			rec->freetup = false;
			plpgsql_adddatum((PLpgSQL_datum *) rec);
			plpgsql_ns_additem(PLPGSQL_NSTYPE_REC, rec->recno, rec->refname);
			function->new_varno = rec->recno;

			/*
			 * Add the record for referencing OLD
			 */
			rec = malloc(sizeof(PLpgSQL_rec));
			memset(rec, 0, sizeof(PLpgSQL_rec));
			rec->dtype = PLPGSQL_DTYPE_REC;
			rec->refname = strdup("old");
			rec->tup = NULL;
			rec->tupdesc = NULL;
			rec->freetup = false;
			plpgsql_adddatum((PLpgSQL_datum *) rec);
			plpgsql_ns_additem(PLPGSQL_NSTYPE_REC, rec->recno, rec->refname);
			function->old_varno = rec->recno;

			/*
			 * Add the variable tg_name
			 */
			var = malloc(sizeof(PLpgSQL_var));
			memset(var, 0, sizeof(PLpgSQL_var));

			var->dtype = PLPGSQL_DTYPE_VAR;
			var->refname = strdup("tg_name");
			var->lineno = 0;
			var->datatype = plpgsql_parse_datatype("name");
			var->isconst = false;
			var->notnull = false;
			var->default_val = NULL;

			plpgsql_adddatum((PLpgSQL_datum *) var);
			plpgsql_ns_additem(PLPGSQL_NSTYPE_VAR, var->varno, var->refname);
			function->tg_name_varno = var->varno;

			/*
			 * Add the variable tg_when
			 */
			var = malloc(sizeof(PLpgSQL_var));
			memset(var, 0, sizeof(PLpgSQL_var));

			var->dtype = PLPGSQL_DTYPE_VAR;
			var->refname = strdup("tg_when");
			var->lineno = 0;
			var->datatype = plpgsql_parse_datatype("text");
			var->isconst = false;
			var->notnull = false;
			var->default_val = NULL;

			plpgsql_adddatum((PLpgSQL_datum *) var);
			plpgsql_ns_additem(PLPGSQL_NSTYPE_VAR, var->varno, var->refname);
			function->tg_when_varno = var->varno;

			/*
			 * Add the variable tg_level
			 */
			var = malloc(sizeof(PLpgSQL_var));
			memset(var, 0, sizeof(PLpgSQL_var));

			var->dtype = PLPGSQL_DTYPE_VAR;
			var->refname = strdup("tg_level");
			var->lineno = 0;
			var->datatype = plpgsql_parse_datatype("text");
			var->isconst = false;
			var->notnull = false;
			var->default_val = NULL;

			plpgsql_adddatum((PLpgSQL_datum *) var);
			plpgsql_ns_additem(PLPGSQL_NSTYPE_VAR, var->varno, var->refname);
			function->tg_level_varno = var->varno;

			/*
			 * Add the variable tg_op
			 */
			var = malloc(sizeof(PLpgSQL_var));
			memset(var, 0, sizeof(PLpgSQL_var));

			var->dtype = PLPGSQL_DTYPE_VAR;
			var->refname = strdup("tg_op");
			var->lineno = 0;
			var->datatype = plpgsql_parse_datatype("text");
			var->isconst = false;
			var->notnull = false;
			var->default_val = NULL;

			plpgsql_adddatum((PLpgSQL_datum *) var);
			plpgsql_ns_additem(PLPGSQL_NSTYPE_VAR, var->varno, var->refname);
			function->tg_op_varno = var->varno;

			/*
			 * Add the variable tg_relid
			 */
			var = malloc(sizeof(PLpgSQL_var));
			memset(var, 0, sizeof(PLpgSQL_var));

			var->dtype = PLPGSQL_DTYPE_VAR;
			var->refname = strdup("tg_relid");
			var->lineno = 0;
			var->datatype = plpgsql_parse_datatype("oid");
			var->isconst = false;
			var->notnull = false;
			var->default_val = NULL;

			plpgsql_adddatum((PLpgSQL_datum *) var);
			plpgsql_ns_additem(PLPGSQL_NSTYPE_VAR, var->varno, var->refname);
			function->tg_relid_varno = var->varno;

			/*
			 * Add the variable tg_relname
			 */
			var = malloc(sizeof(PLpgSQL_var));
			memset(var, 0, sizeof(PLpgSQL_var));

			var->dtype = PLPGSQL_DTYPE_VAR;
			var->refname = strdup("tg_relname");
			var->lineno = 0;
			var->datatype = plpgsql_parse_datatype("name");
			var->isconst = false;
			var->notnull = false;
			var->default_val = NULL;

			plpgsql_adddatum((PLpgSQL_datum *) var);
			plpgsql_ns_additem(PLPGSQL_NSTYPE_VAR, var->varno, var->refname);
			function->tg_relname_varno = var->varno;

			/*
			 * Add the variable tg_nargs
			 */
			var = malloc(sizeof(PLpgSQL_var));
			memset(var, 0, sizeof(PLpgSQL_var));

			var->dtype = PLPGSQL_DTYPE_VAR;
			var->refname = strdup("tg_nargs");
			var->lineno = 0;
			var->datatype = plpgsql_parse_datatype("int4");
			var->isconst = false;
			var->notnull = false;
			var->default_val = NULL;

			plpgsql_adddatum((PLpgSQL_datum *) var);
			plpgsql_ns_additem(PLPGSQL_NSTYPE_VAR, var->varno, var->refname);
			function->tg_nargs_varno = var->varno;

			break;

		default:
			elog(ERROR, "unknown function type %u in plpgsql_compile()",
				 functype);
			break;
	}

	/*
	 * Create the magic FOUND variable.
	 */
	var = malloc(sizeof(PLpgSQL_var));
	memset(var, 0, sizeof(PLpgSQL_var));

	var->dtype = PLPGSQL_DTYPE_VAR;
	var->refname = strdup("found");
	var->lineno = 0;
	var->datatype = plpgsql_parse_datatype("bool");
	var->isconst = false;
	var->notnull = false;
	var->default_val = NULL;

	plpgsql_adddatum((PLpgSQL_datum *) var);
	plpgsql_ns_additem(PLPGSQL_NSTYPE_VAR, var->varno, var->refname);
	function->found_varno = var->varno;

	/*
	 * Forget about the above created variables
	 */
	plpgsql_add_initdatums(NULL);

	/*
	 * Now parse the functions text
	 */
	parse_rc = plpgsql_yyparse();
	if (parse_rc != 0)
		elog(ERROR, "plpgsql: parser returned %d ???", parse_rc);

	plpgsql_scanner_finish();

	/*
	 * If that was successful, complete the functions info.
	 */
	function->fn_nargs = procStruct->pronargs;
	for (i = 0; i < function->fn_nargs; i++)
		function->fn_argvarnos[i] = arg_varnos[i];
	function->ndatums = plpgsql_nDatums;
	function->datums = malloc(sizeof(PLpgSQL_datum *) * plpgsql_nDatums);
	for (i = 0; i < plpgsql_nDatums; i++)
		function->datums[i] = plpgsql_Datums[i];
	function->action = plpgsql_yylval.program;

	ReleaseSysCache(procTup);

	/*
	 * Pop the error context stack
	 */
	error_context_stack = plerrcontext.previous;
	plpgsql_error_funcname = NULL;
	plpgsql_error_lineno = 0;

	/*
	 * Finally return the compiled function
	 */
	if (plpgsql_DumpExecTree)
		plpgsql_dumptree(function);
	return function;
}


/*
 * error context callback to let us supply a call-stack traceback
 */
static void
plpgsql_compile_error_callback(void *arg)
{
	if (plpgsql_error_funcname)
		errcontext("compile of PL/pgSQL function %s near line %d",
				   plpgsql_error_funcname, plpgsql_error_lineno);
}


/* ----------
 * plpgsql_parse_word		The scanner calls this to postparse
 *				any single word not found by a
 *				keyword rule.
 * ----------
 */
int
plpgsql_parse_word(char *word)
{
	PLpgSQL_nsitem *nse;
	char	   *cp[1];

	/* Do case conversion and word separation */
	plpgsql_convert_ident(word, cp, 1);

	/*
	 * Recognize tg_argv when compiling triggers
	 */
	if (plpgsql_curr_compile->fn_functype == T_TRIGGER)
	{
		if (strcmp(cp[0], "tg_argv") == 0)
		{
			int			save_spacescanned = plpgsql_SpaceScanned;
			PLpgSQL_trigarg *trigarg;

			trigarg = malloc(sizeof(PLpgSQL_trigarg));
			memset(trigarg, 0, sizeof(PLpgSQL_trigarg));
			trigarg->dtype = PLPGSQL_DTYPE_TRIGARG;

			if (plpgsql_yylex() != '[')
				plpgsql_yyerror("expected [");

			trigarg->argnum = plpgsql_read_expression(']', "]");

			plpgsql_adddatum((PLpgSQL_datum *) trigarg);
			plpgsql_yylval.variable = (PLpgSQL_datum *) trigarg;

			plpgsql_SpaceScanned = save_spacescanned;
			pfree(cp[0]);
			return T_VARIABLE;
		}
	}

	/*
	 * Do a lookup on the compilers namestack
	 */
	nse = plpgsql_ns_lookup(cp[0], NULL);
	if (nse != NULL)
	{
		pfree(cp[0]);
		switch (nse->itemtype)
		{
			case PLPGSQL_NSTYPE_LABEL:
				return T_LABEL;

			case PLPGSQL_NSTYPE_VAR:
				plpgsql_yylval.var = (PLpgSQL_var *) (plpgsql_Datums[nse->itemno]);
				return T_VARIABLE;

			case PLPGSQL_NSTYPE_REC:
				plpgsql_yylval.rec = (PLpgSQL_rec *) (plpgsql_Datums[nse->itemno]);
				return T_RECORD;

			case PLPGSQL_NSTYPE_ROW:
				plpgsql_yylval.row = (PLpgSQL_row *) (plpgsql_Datums[nse->itemno]);
				return T_ROW;

			default:
				return T_ERROR;
		}
	}

	/*
	 * Nothing found - up to now it's a word without any special meaning
	 * for us.
	 */
	pfree(cp[0]);
	return T_WORD;
}


/* ----------
 * plpgsql_parse_dblword		Same lookup for two words
 *					separated by a dot.
 * ----------
 */
int
plpgsql_parse_dblword(char *word)
{
	PLpgSQL_nsitem *ns;
	char	   *cp[2];

	/* Do case conversion and word separation */
	plpgsql_convert_ident(word, cp, 2);

	/*
	 * Lookup the first word
	 */
	ns = plpgsql_ns_lookup(cp[0], NULL);
	if (ns == NULL)
	{
		pfree(cp[0]);
		pfree(cp[1]);
		return T_ERROR;
	}

	switch (ns->itemtype)
	{
		case PLPGSQL_NSTYPE_LABEL:

			/*
			 * First word is a label, so second word could be a variable,
			 * record or row in that bodies namestack. Anything else could
			 * only be something in a query given to the SPI manager and
			 * T_ERROR will get eaten up by the collector routines.
			 */
			ns = plpgsql_ns_lookup(cp[1], cp[0]);
			pfree(cp[0]);
			pfree(cp[1]);
			if (ns == NULL)
				return T_ERROR;
			switch (ns->itemtype)
			{
				case PLPGSQL_NSTYPE_VAR:
					plpgsql_yylval.var = (PLpgSQL_var *) (plpgsql_Datums[ns->itemno]);
					return T_VARIABLE;

				case PLPGSQL_NSTYPE_REC:
					plpgsql_yylval.rec = (PLpgSQL_rec *) (plpgsql_Datums[ns->itemno]);
					return T_RECORD;

				case PLPGSQL_NSTYPE_ROW:
					plpgsql_yylval.row = (PLpgSQL_row *) (plpgsql_Datums[ns->itemno]);
					return T_ROW;

				default:
					return T_ERROR;
			}
			break;

		case PLPGSQL_NSTYPE_REC:
			{
				/*
				 * First word is a record name, so second word must be a
				 * field in this record.
				 */
				PLpgSQL_recfield *new;

				new = malloc(sizeof(PLpgSQL_recfield));
				new->dtype = PLPGSQL_DTYPE_RECFIELD;
				new->fieldname = strdup(cp[1]);
				new->recparentno = ns->itemno;

				plpgsql_adddatum((PLpgSQL_datum *) new);

				plpgsql_yylval.variable = (PLpgSQL_datum *) new;

				pfree(cp[0]);
				pfree(cp[1]);
				return T_VARIABLE;
			}

		case PLPGSQL_NSTYPE_ROW:
			{
				/*
				 * First word is a row name, so second word must be a
				 * field in this row.
				 */
				PLpgSQL_row *row;
				int			i;

				row = (PLpgSQL_row *) (plpgsql_Datums[ns->itemno]);
				for (i = 0; i < row->nfields; i++)
				{
					if (strcmp(row->fieldnames[i], cp[1]) == 0)
					{
						plpgsql_yylval.var = (PLpgSQL_var *) (plpgsql_Datums[row->varnos[i]]);
						pfree(cp[0]);
						pfree(cp[1]);
						return T_VARIABLE;
					}
				}
				elog(ERROR, "row %s doesn't have a field %s",
					 cp[0], cp[1]);
			}

		default:
			break;
	}

	pfree(cp[0]);
	pfree(cp[1]);
	return T_ERROR;
}


/* ----------
 * plpgsql_parse_tripword		Same lookup for three words
 *					separated by dots.
 * ----------
 */
int
plpgsql_parse_tripword(char *word)
{
	PLpgSQL_nsitem *ns;
	char	   *cp[3];

	/* Do case conversion and word separation */
	plpgsql_convert_ident(word, cp, 3);

	/*
	 * Lookup the first word - it must be a label
	 */
	ns = plpgsql_ns_lookup(cp[0], NULL);
	if (ns == NULL)
	{
		pfree(cp[0]);
		pfree(cp[1]);
		pfree(cp[2]);
		return T_ERROR;
	}
	if (ns->itemtype != PLPGSQL_NSTYPE_LABEL)
	{
		pfree(cp[0]);
		pfree(cp[1]);
		pfree(cp[2]);
		return T_ERROR;
	}

	/*
	 * First word is a label, so second word could be a record or row
	 */
	ns = plpgsql_ns_lookup(cp[1], cp[0]);
	if (ns == NULL)
	{
		pfree(cp[0]);
		pfree(cp[1]);
		pfree(cp[2]);
		return T_ERROR;
	}

	switch (ns->itemtype)
	{
		case PLPGSQL_NSTYPE_REC:
			{
				/*
				 * This word is a record name, so third word must be a
				 * field in this record.
				 */
				PLpgSQL_recfield *new;

				new = malloc(sizeof(PLpgSQL_recfield));
				new->dtype = PLPGSQL_DTYPE_RECFIELD;
				new->fieldname = strdup(cp[2]);
				new->recparentno = ns->itemno;

				plpgsql_adddatum((PLpgSQL_datum *) new);

				plpgsql_yylval.variable = (PLpgSQL_datum *) new;

				pfree(cp[0]);
				pfree(cp[1]);
				pfree(cp[2]);
				return T_VARIABLE;
			}

		case PLPGSQL_NSTYPE_ROW:
			{
				/*
				 * This word is a row name, so third word must be a field
				 * in this row.
				 */
				PLpgSQL_row *row;
				int			i;

				row = (PLpgSQL_row *) (plpgsql_Datums[ns->itemno]);
				for (i = 0; i < row->nfields; i++)
				{
					if (strcmp(row->fieldnames[i], cp[2]) == 0)
					{
						plpgsql_yylval.var = (PLpgSQL_var *) (plpgsql_Datums[row->varnos[i]]);
						pfree(cp[0]);
						pfree(cp[1]);
						pfree(cp[2]);
						return T_VARIABLE;
					}
				}
				elog(ERROR, "row %s.%s doesn't have a field %s",
					 cp[0], cp[1], cp[2]);
			}

		default:
			break;
	}

	pfree(cp[0]);
	pfree(cp[1]);
	pfree(cp[2]);
	return T_ERROR;
}


/* ----------
 * plpgsql_parse_wordtype	The scanner found word%TYPE. word can be
 *				a variable name or a basetype.
 * ----------
 */
int
plpgsql_parse_wordtype(char *word)
{
	PLpgSQL_nsitem *nse;
	bool		old_nsstate;
	Oid			typeOid;
	char	   *cp[2];
	int			i;

	/* Do case conversion and word separation */
	/* We convert %type to .type momentarily to keep converter happy */
	i = strlen(word) - 5;
	Assert(word[i] == '%');
	word[i] = '.';
	plpgsql_convert_ident(word, cp, 2);
	word[i] = '%';
	pfree(cp[1]);

	/*
	 * Do a lookup on the compilers namestack. But ensure it moves up to
	 * the toplevel.
	 */
	old_nsstate = plpgsql_ns_setlocal(false);
	nse = plpgsql_ns_lookup(cp[0], NULL);
	plpgsql_ns_setlocal(old_nsstate);

	if (nse != NULL)
	{
		pfree(cp[0]);
		switch (nse->itemtype)
		{
			case PLPGSQL_NSTYPE_VAR:
				plpgsql_yylval.dtype = ((PLpgSQL_var *) (plpgsql_Datums[nse->itemno]))->datatype;
				return T_DTYPE;

			default:
				return T_ERROR;
		}
	}

	/*
	 * Word wasn't found on the namestack. Try to find a data type with
	 * that name, but ignore pg_type entries that are in fact class types.
	 */
	typeOid = LookupTypeName(makeTypeName(cp[0]));
	if (OidIsValid(typeOid))
	{
		HeapTuple	typeTup;

		typeTup = SearchSysCache(TYPEOID,
								 ObjectIdGetDatum(typeOid),
								 0, 0, 0);
		if (HeapTupleIsValid(typeTup))
		{
			Form_pg_type typeStruct = (Form_pg_type) GETSTRUCT(typeTup);

			if (!typeStruct->typisdefined ||
				typeStruct->typrelid != InvalidOid)
			{
				ReleaseSysCache(typeTup);
				pfree(cp[0]);
				return T_ERROR;
			}

			plpgsql_yylval.dtype = build_datatype(typeTup, -1);

			ReleaseSysCache(typeTup);
			pfree(cp[0]);
			return T_DTYPE;
		}
	}

	/*
	 * Nothing found - up to now it's a word without any special meaning
	 * for us.
	 */
	pfree(cp[0]);
	return T_ERROR;
}


/* ----------
 * plpgsql_parse_dblwordtype		Same lookup for word.word%TYPE
 * ----------
 */
int
plpgsql_parse_dblwordtype(char *word)
{
	PLpgSQL_nsitem *nse;
	bool		old_nsstate;
	Oid			classOid;
	HeapTuple	classtup;
	Form_pg_class classStruct;
	HeapTuple	attrtup;
	Form_pg_attribute attrStruct;
	HeapTuple	typetup;
	char	   *cp[3];
	int			i;

	/* Do case conversion and word separation */
	/* We convert %type to .type momentarily to keep converter happy */
	i = strlen(word) - 5;
	Assert(word[i] == '%');
	word[i] = '.';
	plpgsql_convert_ident(word, cp, 3);
	word[i] = '%';
	pfree(cp[2]);

	/*
	 * Lookup the first word
	 */
	nse = plpgsql_ns_lookup(cp[0], NULL);

	/*
	 * If this is a label lookup the second word in that labels namestack
	 * level
	 */
	if (nse != NULL)
	{
		if (nse->itemtype == PLPGSQL_NSTYPE_LABEL)
		{
			old_nsstate = plpgsql_ns_setlocal(false);
			nse = plpgsql_ns_lookup(cp[1], cp[0]);
			plpgsql_ns_setlocal(old_nsstate);

			pfree(cp[0]);
			pfree(cp[1]);

			if (nse != NULL)
			{
				switch (nse->itemtype)
				{
					case PLPGSQL_NSTYPE_VAR:
						plpgsql_yylval.dtype = ((PLpgSQL_var *) (plpgsql_Datums[nse->itemno]))->datatype;
						return T_DTYPE;

					default:
						return T_ERROR;
				}
			}
			return T_ERROR;
		}
		pfree(cp[0]);
		pfree(cp[1]);
		return T_ERROR;
	}

	/*
	 * First word could also be a table name
	 */
	classOid = RelnameGetRelid(cp[0]);
	if (!OidIsValid(classOid))
	{
		pfree(cp[0]);
		pfree(cp[1]);
		return T_ERROR;
	}
	classtup = SearchSysCache(RELOID,
							  ObjectIdGetDatum(classOid),
							  0, 0, 0);
	if (!HeapTupleIsValid(classtup))
	{
		pfree(cp[0]);
		pfree(cp[1]);
		return T_ERROR;
	}

	/*
	 * It must be a relation, sequence, view, or type
	 */
	classStruct = (Form_pg_class) GETSTRUCT(classtup);
	if (classStruct->relkind != RELKIND_RELATION &&
		classStruct->relkind != RELKIND_SEQUENCE &&
		classStruct->relkind != RELKIND_VIEW &&
		classStruct->relkind != RELKIND_COMPOSITE_TYPE)
	{
		ReleaseSysCache(classtup);
		pfree(cp[0]);
		pfree(cp[1]);
		return T_ERROR;
	}

	/*
	 * Fetch the named table field and it's type
	 */
	attrtup = SearchSysCacheAttName(classOid, cp[1]);
	if (!HeapTupleIsValid(attrtup))
	{
		ReleaseSysCache(classtup);
		pfree(cp[0]);
		pfree(cp[1]);
		return T_ERROR;
	}
	attrStruct = (Form_pg_attribute) GETSTRUCT(attrtup);

	typetup = SearchSysCache(TYPEOID,
							 ObjectIdGetDatum(attrStruct->atttypid),
							 0, 0, 0);
	if (!HeapTupleIsValid(typetup))
		elog(ERROR, "cache lookup for type %u of %s.%s failed",
			 attrStruct->atttypid, cp[0], cp[1]);

	/*
	 * Found that - build a compiler type struct and return it
	 */
	plpgsql_yylval.dtype = build_datatype(typetup, attrStruct->atttypmod);

	ReleaseSysCache(classtup);
	ReleaseSysCache(attrtup);
	ReleaseSysCache(typetup);
	pfree(cp[0]);
	pfree(cp[1]);
	return T_DTYPE;
}

/* ----------
 * plpgsql_parse_tripwordtype		Same lookup for word.word.word%TYPE
 * ----------
 */
#define TYPE_JUNK_LEN	5

int
plpgsql_parse_tripwordtype(char *word)
{
	Oid			classOid;
	HeapTuple	classtup;
	Form_pg_class classStruct;
	HeapTuple	attrtup;
	Form_pg_attribute attrStruct;
	HeapTuple	typetup;
	char	   *cp[2];
	char	   *colname[1];
	int			qualified_att_len;
	int			numdots = 0;
	int			i;
	RangeVar   *relvar;

	/* Do case conversion and word separation */
	qualified_att_len = strlen(word) - TYPE_JUNK_LEN;
	Assert(word[qualified_att_len] == '%');

	for (i = 0; i < qualified_att_len; i++)
	{
		if (word[i] == '.' && ++numdots == 2)
		{
			cp[0] = (char *) palloc((i + 1) * sizeof(char));
			memset(cp[0], 0, (i + 1) * sizeof(char));
			memcpy(cp[0], word, i * sizeof(char));

			/* qualified_att_len - one based position + 1 (null terminator) */
			cp[1] = (char *) palloc((qualified_att_len - i) * sizeof(char));
			memset(cp[1], 0, (qualified_att_len - i) * sizeof(char));
			memcpy(cp[1], &word[i + 1], (qualified_att_len - i - 1) * sizeof(char));

			break;
		}
	}

	relvar = makeRangeVarFromNameList(stringToQualifiedNameList(cp[0], "plpgsql_parse_tripwordtype"));
	classOid = RangeVarGetRelid(relvar, true);
	if (!OidIsValid(classOid))
	{
		pfree(cp[0]);
		pfree(cp[1]);
		return T_ERROR;
	}
	classtup = SearchSysCache(RELOID,
							  ObjectIdGetDatum(classOid),
							  0, 0, 0);
	if (!HeapTupleIsValid(classtup))
	{
		pfree(cp[0]);
		pfree(cp[1]);
		return T_ERROR;
	}

	/*
	 * It must be a relation, sequence, view, or type
	 */
	classStruct = (Form_pg_class) GETSTRUCT(classtup);
	if (classStruct->relkind != RELKIND_RELATION &&
		classStruct->relkind != RELKIND_SEQUENCE &&
		classStruct->relkind != RELKIND_VIEW &&
		classStruct->relkind != RELKIND_COMPOSITE_TYPE)
	{
		ReleaseSysCache(classtup);
		pfree(cp[0]);
		pfree(cp[1]);
		return T_ERROR;
	}

	/*
	 * Fetch the named table field and it's type
	 */
	plpgsql_convert_ident(cp[1], colname, 1);
	attrtup = SearchSysCacheAttName(classOid, colname[0]);
	pfree(colname[0]);

	if (!HeapTupleIsValid(attrtup))
	{
		ReleaseSysCache(classtup);
		pfree(cp[0]);
		pfree(cp[1]);
		return T_ERROR;
	}
	attrStruct = (Form_pg_attribute) GETSTRUCT(attrtup);

	typetup = SearchSysCache(TYPEOID,
							 ObjectIdGetDatum(attrStruct->atttypid),
							 0, 0, 0);
	if (!HeapTupleIsValid(typetup))
		elog(ERROR, "cache lookup for type %u of %s.%s failed",
			 attrStruct->atttypid, cp[0], cp[1]);

	/*
	 * Found that - build a compiler type struct and return it
	 */
	plpgsql_yylval.dtype = build_datatype(typetup, attrStruct->atttypmod);

	ReleaseSysCache(classtup);
	ReleaseSysCache(attrtup);
	ReleaseSysCache(typetup);
	pfree(cp[0]);
	pfree(cp[1]);
	return T_DTYPE;
}

/* ----------
 * plpgsql_parse_wordrowtype		Scanner found word%ROWTYPE.
 *					So word must be a table name.
 * ----------
 */
int
plpgsql_parse_wordrowtype(char *word)
{
	Oid			classOid;
	char	   *cp[2];
	int			i;

	/* Do case conversion and word separation */
	/* We convert %rowtype to .rowtype momentarily to keep converter happy */
	i = strlen(word) - 8;
	Assert(word[i] == '%');
	word[i] = '.';
	plpgsql_convert_ident(word, cp, 2);
	word[i] = '%';

	/* Lookup the relation */
	classOid = RelnameGetRelid(cp[0]);
	if (!OidIsValid(classOid))
		elog(ERROR, "%s: no such class", cp[0]);

	/*
	 * Build and return the complete row definition
	 */
	plpgsql_yylval.row = plpgsql_build_rowtype(classOid);

	pfree(cp[0]);
	pfree(cp[1]);

	return T_ROW;
}

/* ----------
 * plpgsql_parse_dblwordrowtype		Scanner found word.word%ROWTYPE.
 *			So word must be namespace qualified a table name.
 * ----------
 */
#define ROWTYPE_JUNK_LEN	8

int
plpgsql_parse_dblwordrowtype(char *word)
{
	Oid			classOid;
	char	   *cp;
	int			i;
	RangeVar   *relvar;

	/* Do case conversion and word separation */
	/* We convert %rowtype to .rowtype momentarily to keep converter happy */
	i = strlen(word) - ROWTYPE_JUNK_LEN;
	Assert(word[i] == '%');

	cp = (char *) palloc((i + 1) * sizeof(char));
	memset(cp, 0, (i + 1) * sizeof(char));
	memcpy(cp, word, i * sizeof(char));

	/* Lookup the relation */
	relvar = makeRangeVarFromNameList(stringToQualifiedNameList(cp, "plpgsql_parse_dblwordrowtype"));
	classOid = RangeVarGetRelid(relvar, true);
	if (!OidIsValid(classOid))
		elog(ERROR, "%s: no such class", cp);

	/*
	 * Build and return the complete row definition
	 */
	plpgsql_yylval.row = plpgsql_build_rowtype(classOid);

	pfree(cp);

	return T_ROW;
}

/*
 * Build a rowtype data structure given the pg_class OID.
 */
PLpgSQL_row *
plpgsql_build_rowtype(Oid classOid)
{
	PLpgSQL_row *row;
	HeapTuple	classtup;
	Form_pg_class classStruct;
	const char *relname;
	int			i;

	/*
	 * Fetch the pg_class tuple.
	 */
	classtup = SearchSysCache(RELOID,
							  ObjectIdGetDatum(classOid),
							  0, 0, 0);
	if (!HeapTupleIsValid(classtup))
		elog(ERROR, "cache lookup failed for relation %u", classOid);
	classStruct = (Form_pg_class) GETSTRUCT(classtup);
	relname = NameStr(classStruct->relname);

	/* accept relation, sequence, view, or type pg_class entries */
	if (classStruct->relkind != RELKIND_RELATION &&
		classStruct->relkind != RELKIND_SEQUENCE &&
		classStruct->relkind != RELKIND_VIEW &&
		classStruct->relkind != RELKIND_COMPOSITE_TYPE)
		elog(ERROR, "%s isn't a table", relname);

	/*
	 * Create a row datum entry and all the required variables that it
	 * will point to.
	 */
	row = malloc(sizeof(PLpgSQL_row));
	memset(row, 0, sizeof(PLpgSQL_row));

	row->dtype = PLPGSQL_DTYPE_ROW;
	row->nfields = classStruct->relnatts;
	row->rowtypeclass = classStruct->reltype;
	row->fieldnames = malloc(sizeof(char *) * row->nfields);
	row->varnos = malloc(sizeof(int) * row->nfields);

	for (i = 0; i < row->nfields; i++)
	{
		HeapTuple	attrtup;
		Form_pg_attribute attrStruct;
		HeapTuple	typetup;
		const char *attname;
		PLpgSQL_var *var;

		/*
		 * Get the attribute and it's type
		 */
		attrtup = SearchSysCache(ATTNUM,
								 ObjectIdGetDatum(classOid),
								 Int16GetDatum(i + 1),
								 0, 0);
		if (!HeapTupleIsValid(attrtup))
			elog(ERROR, "cache lookup for attribute %d of class %s failed",
				 i + 1, relname);
		attrStruct = (Form_pg_attribute) GETSTRUCT(attrtup);

		attname = NameStr(attrStruct->attname);

		typetup = SearchSysCache(TYPEOID,
								 ObjectIdGetDatum(attrStruct->atttypid),
								 0, 0, 0);
		if (!HeapTupleIsValid(typetup))
			elog(ERROR, "cache lookup for type %u of %s.%s failed",
				 attrStruct->atttypid, relname, attname);

		/*
		 * Create the internal variable
		 *
		 * We know if the table definitions contain a default value or if the
		 * field is declared in the table as NOT NULL. But it's possible
		 * to create a table field as NOT NULL without a default value and
		 * that would lead to problems later when initializing the
		 * variables due to entering a block at execution time. Thus we
		 * ignore this information for now.
		 */
		var = malloc(sizeof(PLpgSQL_var));
		memset(var, 0, sizeof(PLpgSQL_var));
		var->dtype = PLPGSQL_DTYPE_VAR;
		var->refname = malloc(strlen(relname) + strlen(attname) + 2);
		strcpy(var->refname, relname);
		strcat(var->refname, ".");
		strcat(var->refname, attname);
		var->datatype = build_datatype(typetup, attrStruct->atttypmod);
		var->isconst = false;
		var->notnull = false;
		var->default_val = NULL;
		var->value = (Datum) 0;
		var->isnull = true;
		var->freeval = false;

		plpgsql_adddatum((PLpgSQL_datum *) var);

		/*
		 * Add the variable to the row.
		 */
		row->fieldnames[i] = strdup(attname);
		row->varnos[i] = var->varno;

		ReleaseSysCache(typetup);
		ReleaseSysCache(attrtup);
	}

	ReleaseSysCache(classtup);

	return row;
}


/* ----------
 * plpgsql_parse_datatype			Scanner found something that should
 *					be a datatype name.
 * ----------
 */
PLpgSQL_type *
plpgsql_parse_datatype(char *string)
{
	Oid			type_id;
	int32		typmod;
	HeapTuple	typeTup;
	PLpgSQL_type *typ;

	/* Let the main parser try to parse it under standard SQL rules */
	parseTypeString(string, &type_id, &typmod);

	/* Okay, build a PLpgSQL_type data structure for it */
	typeTup = SearchSysCache(TYPEOID,
							 ObjectIdGetDatum(type_id),
							 0, 0, 0);
	if (!HeapTupleIsValid(typeTup))
		elog(ERROR, "cache lookup failed for type %u", type_id);

	typ = build_datatype(typeTup, typmod);

	ReleaseSysCache(typeTup);

	return typ;
}

/*
 * Utility subroutine to make a PLpgSQL_type struct given a pg_type entry
 */
static PLpgSQL_type *
build_datatype(HeapTuple typeTup, int32 typmod)
{
	Form_pg_type typeStruct = (Form_pg_type) GETSTRUCT(typeTup);
	PLpgSQL_type *typ;

	typ = (PLpgSQL_type *) malloc(sizeof(PLpgSQL_type));

	typ->typname = strdup(NameStr(typeStruct->typname));
	typ->typoid = HeapTupleGetOid(typeTup);
	typ->typlen = typeStruct->typlen;
	typ->typbyval = typeStruct->typbyval;
	typ->typrelid = typeStruct->typrelid;
	typ->typelem = typeStruct->typelem;
	perm_fmgr_info(typeStruct->typinput, &(typ->typinput));
	typ->atttypmod = typmod;

	return typ;
}


/* ----------
 * plpgsql_adddatum			Add a variable, record or row
 *					to the compilers datum list.
 * ----------
 */
void
plpgsql_adddatum(PLpgSQL_datum * new)
{
	if (plpgsql_nDatums == datums_alloc)
	{
		datums_alloc *= 2;
		plpgsql_Datums = repalloc(plpgsql_Datums, sizeof(PLpgSQL_datum *) * datums_alloc);
	}

	new->dno = plpgsql_nDatums;
	plpgsql_Datums[plpgsql_nDatums++] = new;
}


/* ----------
 * plpgsql_add_initdatums		Put all datum entries created
 *					since the last call into the
 *					finishing code block so the
 *					block knows which variables to
 *					reinitialize when entered.
 * ----------
 */
int
plpgsql_add_initdatums(int **varnos)
{
	int			i;
	int			n = 0;

	for (i = datums_last; i < plpgsql_nDatums; i++)
	{
		switch (plpgsql_Datums[i]->dtype)
		{
			case PLPGSQL_DTYPE_VAR:
				n++;
				break;

			default:
				break;
		}
	}

	if (varnos != NULL)
	{
		*varnos = (int *) malloc(sizeof(int) * n);

		n = 0;
		for (i = datums_last; i < plpgsql_nDatums; i++)
		{
			switch (plpgsql_Datums[i]->dtype)
			{
				case PLPGSQL_DTYPE_VAR:
					(*varnos)[n++] = plpgsql_Datums[i]->dno;

				default:
					break;
			}
		}
	}

	datums_last = plpgsql_nDatums;
	return n;
}


/* ---------
 * plpgsql_yyerror			Handle parser error
 * ---------
 */

void
plpgsql_yyerror(const char *s)
{
	plpgsql_error_lineno = plpgsql_scanner_lineno();
	elog(ERROR, "%s at or near \"%s\"", s, plpgsql_yytext);
}
