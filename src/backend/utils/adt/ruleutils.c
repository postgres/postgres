/**********************************************************************
 * get_ruledef.c	- Function to get a rules definition text
 *			  out of it's tuple
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/ruleutils.c,v 1.30 1999/11/07 23:08:24 momjian Exp $
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

#include <unistd.h>
#include <fcntl.h>

#include "postgres.h"

#include "catalog/pg_index.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_shadow.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"


/* ----------
 * Local data types
 * ----------
 */
typedef struct
{
	StringInfo	buf;			/* output buffer to append to */
	List	   *rangetables;	/* List of List of RangeTblEntry */
	bool		varprefix;		/* TRUE to print prefixes on Vars */
} deparse_context;

typedef struct {
	Index		rt_index;
	int			levelsup;
} check_if_rte_used_context;


/* ----------
 * Global data
 * ----------
 */
static char *rulename = NULL;
static void *plan_getrule = NULL;
static char *query_getrule = "SELECT * FROM pg_rewrite WHERE rulename = $1";
static void *plan_getview = NULL;
static char *query_getview = "SELECT * FROM pg_rewrite WHERE rulename = $1 or rulename = $2";
static void *plan_getam = NULL;
static char *query_getam = "SELECT * FROM pg_am WHERE oid = $1";
static void *plan_getopclass = NULL;
static char *query_getopclass = "SELECT * FROM pg_opclass WHERE oid = $1";


/* ----------
 * Local functions
 *
 * Most of these functions used to use fixed-size buffers to build their
 * results.  Now, they take an (already initialized) StringInfo object
 * as a parameter, and append their text output to its contents.
 * ----------
 */
static void make_ruledef(StringInfo buf, HeapTuple ruletup, TupleDesc rulettc);
static void make_viewdef(StringInfo buf, HeapTuple ruletup, TupleDesc rulettc);
static void get_query_def(Query *query, StringInfo buf, List *parentrtables);
static void get_select_query_def(Query *query, deparse_context *context);
static void get_insert_query_def(Query *query, deparse_context *context);
static void get_update_query_def(Query *query, deparse_context *context);
static void get_delete_query_def(Query *query, deparse_context *context);
static RangeTblEntry *get_rte_for_var(Var *var, deparse_context *context);
static void get_rule_expr(Node *node, deparse_context *context);
static void get_func_expr(Expr *expr, deparse_context *context);
static void get_tle_expr(TargetEntry *tle, deparse_context *context);
static void get_const_expr(Const *constval, deparse_context *context);
static void get_sublink_expr(Node *node, deparse_context *context);
static char *quote_identifier(char *ident);
static char *get_relation_name(Oid relid);
static char *get_attribute_name(Oid relid, int2 attnum);
static bool check_if_rte_used(Node *node, Index rt_index, int levelsup);
static bool check_if_rte_used_walker(Node *node,
									 check_if_rte_used_context *context);

#define inherit_marker(rte)  ((rte)->inh ? "*" : "")


/* ----------
 * get_ruledef			- Do it all and return a text
 *				  that could be used as a statement
 *				  to recreate the rule
 * ----------
 */
text *
pg_get_ruledef(NameData *rname)
{
	text	   *ruledef;
	Datum		args[1];
	char		nulls[2];
	int			spirc;
	HeapTuple	ruletup;
	TupleDesc	rulettc;
	StringInfoData	buf;
	int			len;

	/* ----------
	 * We need the rules name somewhere deep down
	 * ----------
	 */
	rulename = pstrdup(NameStr(*rname));

	/* ----------
	 * Connect to SPI manager
	 * ----------
	 */
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "get_ruledef: cannot connect to SPI manager");

	/* ----------
	 * On the first call prepare the plan to lookup pg_proc.
	 * We read pg_proc over the SPI manager instead of using
	 * the syscache to be checked for read access on pg_proc.
	 * ----------
	 */
	if (plan_getrule == NULL)
	{
		Oid			argtypes[1];
		void	   *plan;

		argtypes[0] = NAMEOID;
		plan = SPI_prepare(query_getrule, 1, argtypes);
		if (plan == NULL)
			elog(ERROR, "SPI_prepare() failed for \"%s\"", query_getrule);
		plan_getrule = SPI_saveplan(plan);
	}

	/* ----------
	 * Get the pg_rewrite tuple for this rule
	 * ----------
	 */
	args[0] = PointerGetDatum(rulename);
	nulls[0] = (rulename == NULL) ? 'n' : ' ';
	nulls[1] = '\0';
	spirc = SPI_execp(plan_getrule, args, nulls, 1);
	if (spirc != SPI_OK_SELECT)
		elog(ERROR, "failed to get pg_rewrite tuple for %s", rulename);
	if (SPI_processed != 1)
	{
		if (SPI_finish() != SPI_OK_FINISH)
			elog(ERROR, "get_ruledef: SPI_finish() failed");
		ruledef = SPI_palloc(VARHDRSZ + 1);
		VARSIZE(ruledef) = VARHDRSZ + 1;
		VARDATA(ruledef)[0] = '-';
		return ruledef;
	}

	ruletup = SPI_tuptable->vals[0];
	rulettc = SPI_tuptable->tupdesc;

	/* ----------
	 * Get the rules definition and put it into executors memory
	 * ----------
	 */
	initStringInfo(&buf);
	make_ruledef(&buf, ruletup, rulettc);
	len = buf.len + VARHDRSZ;
	ruledef = SPI_palloc(len);
	VARSIZE(ruledef) = len;
	memcpy(VARDATA(ruledef), buf.data, buf.len);
	pfree(buf.data);

	/* ----------
	 * Disconnect from SPI manager
	 * ----------
	 */
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "get_ruledef: SPI_finish() failed");

	/* ----------
	 * Easy - isn't it?
	 * ----------
	 */
	return ruledef;
}


/* ----------
 * get_viewdef			- Mainly the same thing, but we
 *				  only return the SELECT part of a view
 * ----------
 */
text *
pg_get_viewdef(NameData *rname)
{
	text	   *ruledef;
	Datum		args[2];
	char		nulls[3];
	int			spirc;
	HeapTuple	ruletup;
	TupleDesc	rulettc;
	StringInfoData	buf;
	int			len;
	char		name1[NAMEDATALEN + 5];
	char		name2[NAMEDATALEN + 5];

	/* ----------
	 * We need the rules name somewhere deep down
	 * ----------
	 */
	rulename = pstrdup(NameStr(*rname));

	/* ----------
	 * Connect to SPI manager
	 * ----------
	 */
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "get_viewdef: cannot connect to SPI manager");

	/* ----------
	 * On the first call prepare the plan to lookup pg_proc.
	 * We read pg_proc over the SPI manager instead of using
	 * the syscache to be checked for read access on pg_proc.
	 * ----------
	 */
	if (plan_getview == NULL)
	{
		Oid			argtypes[2];
		void	   *plan;

		argtypes[0] = NAMEOID;
		argtypes[1] = NAMEOID;
		plan = SPI_prepare(query_getview, 2, argtypes);
		if (plan == NULL)
			elog(ERROR, "SPI_prepare() failed for \"%s\"", query_getview);
		plan_getview = SPI_saveplan(plan);
	}

	/* ----------
	 * Get the pg_rewrite tuple for this rule
	 * ----------
	 */
	sprintf(name1, "_RET%s", rulename);
	sprintf(name2, "_ret%s", rulename);
	args[0] = PointerGetDatum(name1);
	args[1] = PointerGetDatum(name2);
	nulls[0] = ' ';
	nulls[1] = ' ';
	nulls[2] = '\0';
	spirc = SPI_execp(plan_getview, args, nulls, 1);
	if (spirc != SPI_OK_SELECT)
		elog(ERROR, "failed to get pg_rewrite tuple for view %s", rulename);
	initStringInfo(&buf);
	if (SPI_processed != 1)
		appendStringInfo(&buf, "Not a view");
	else
	{
		/* ----------
		 * Get the rules definition and put it into executors memory
		 * ----------
		 */
		ruletup = SPI_tuptable->vals[0];
		rulettc = SPI_tuptable->tupdesc;
		make_viewdef(&buf, ruletup, rulettc);
	}
	len = buf.len + VARHDRSZ;
	ruledef = SPI_palloc(len);
	VARSIZE(ruledef) = len;
	memcpy(VARDATA(ruledef), buf.data, buf.len);
	pfree(buf.data);

	/* ----------
	 * Disconnect from SPI manager
	 * ----------
	 */
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "get_viewdef: SPI_finish() failed");

	/* ----------
	 * Easy - isn't it?
	 * ----------
	 */
	return ruledef;
}


/* ----------
 * get_indexdef			- Get the definition of an index
 * ----------
 */
text *
pg_get_indexdef(Oid indexrelid)
{
	text	   *indexdef;
	HeapTuple	ht_idx;
	HeapTuple	ht_idxrel;
	HeapTuple	ht_indrel;
	HeapTuple	spi_tup;
	TupleDesc	spi_ttc;
	int			spi_fno;
	Form_pg_index idxrec;
	Form_pg_class idxrelrec;
	Form_pg_class indrelrec;
	Datum		spi_args[1];
	char		spi_nulls[2];
	int			spirc;
	int			len;
	int			keyno;
	StringInfoData	buf;
	StringInfoData	keybuf;
	char	   *sep;

	/* ----------
	 * Connect to SPI manager
	 * ----------
	 */
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "get_indexdef: cannot connect to SPI manager");

	/* ----------
	 * On the first call prepare the plans to lookup pg_am
	 * and pg_opclass.
	 * ----------
	 */
	if (plan_getam == NULL)
	{
		Oid			argtypes[1];
		void	   *plan;

		argtypes[0] = OIDOID;
		plan = SPI_prepare(query_getam, 1, argtypes);
		if (plan == NULL)
			elog(ERROR, "SPI_prepare() failed for \"%s\"", query_getam);
		plan_getam = SPI_saveplan(plan);

		argtypes[0] = OIDOID;
		plan = SPI_prepare(query_getopclass, 1, argtypes);
		if (plan == NULL)
			elog(ERROR, "SPI_prepare() failed for \"%s\"", query_getopclass);
		plan_getopclass = SPI_saveplan(plan);
	}

	/* ----------
	 * Fetch the pg_index tuple by the Oid of the index
	 * ----------
	 */
	ht_idx = SearchSysCacheTuple(INDEXRELID,
								 ObjectIdGetDatum(indexrelid), 0, 0, 0);
	if (!HeapTupleIsValid(ht_idx))
		elog(ERROR, "syscache lookup for index %u failed", indexrelid);
	idxrec = (Form_pg_index) GETSTRUCT(ht_idx);

	/* ----------
	 * Fetch the pg_class tuple of the index relation
	 * ----------
	 */
	ht_idxrel = SearchSysCacheTuple(RELOID,
						  ObjectIdGetDatum(idxrec->indexrelid), 0, 0, 0);
	if (!HeapTupleIsValid(ht_idxrel))
		elog(ERROR, "syscache lookup for relid %u failed", idxrec->indexrelid);
	idxrelrec = (Form_pg_class) GETSTRUCT(ht_idxrel);

	/* ----------
	 * Fetch the pg_class tuple of the indexed relation
	 * ----------
	 */
	ht_indrel = SearchSysCacheTuple(RELOID,
							ObjectIdGetDatum(idxrec->indrelid), 0, 0, 0);
	if (!HeapTupleIsValid(ht_indrel))
		elog(ERROR, "syscache lookup for relid %u failed", idxrec->indrelid);
	indrelrec = (Form_pg_class) GETSTRUCT(ht_indrel);

	/* ----------
	 * Get the am name for the index relation
	 * ----------
	 */
	spi_args[0] = ObjectIdGetDatum(idxrelrec->relam);
	spi_nulls[0] = ' ';
	spi_nulls[1] = '\0';
	spirc = SPI_execp(plan_getam, spi_args, spi_nulls, 1);
	if (spirc != SPI_OK_SELECT)
		elog(ERROR, "failed to get pg_am tuple for index %s",
			 idxrelrec->relname);
	if (SPI_processed != 1)
		elog(ERROR, "failed to get pg_am tuple for index %s",
			 idxrelrec->relname);
	spi_tup = SPI_tuptable->vals[0];
	spi_ttc = SPI_tuptable->tupdesc;
	spi_fno = SPI_fnumber(spi_ttc, "amname");

	/* ----------
	 * Start the index definition
	 * ----------
	 */
	initStringInfo(&buf);
	appendStringInfo(&buf, "CREATE %sINDEX %s ON %s USING %s (",
					 idxrec->indisunique ? "UNIQUE " : "",
					 quote_identifier(pstrdup(NameStr(idxrelrec->relname))),
					 quote_identifier(pstrdup(NameStr(indrelrec->relname))),
					 quote_identifier(SPI_getvalue(spi_tup, spi_ttc,
												   spi_fno)));

	/* ----------
	 * Collect the indexed attributes
	 * ----------
	 */
	initStringInfo(&keybuf);
	sep = "";
	for (keyno = 0; keyno < INDEX_MAX_KEYS; keyno++)
	{
		if (idxrec->indkey[keyno] == InvalidAttrNumber)
			break;

		appendStringInfo(&keybuf, sep);
		sep = ", ";

		/* ----------
		 * Add the indexed field name
		 * ----------
		 */
		appendStringInfo(&keybuf, "%s",
				quote_identifier(get_attribute_name(idxrec->indrelid,
													idxrec->indkey[keyno])));

		/* ----------
		 * If not a functional index, add the operator class name
		 * ----------
		 */
		if (idxrec->indproc == InvalidOid)
		{
			spi_args[0] = ObjectIdGetDatum(idxrec->indclass[keyno]);
			spi_nulls[0] = ' ';
			spi_nulls[1] = '\0';
			spirc = SPI_execp(plan_getopclass, spi_args, spi_nulls, 1);
			if (spirc != SPI_OK_SELECT)
				elog(ERROR, "failed to get pg_opclass tuple %u", idxrec->indclass[keyno]);
			if (SPI_processed != 1)
				elog(ERROR, "failed to get pg_opclass tuple %u", idxrec->indclass[keyno]);
			spi_tup = SPI_tuptable->vals[0];
			spi_ttc = SPI_tuptable->tupdesc;
			spi_fno = SPI_fnumber(spi_ttc, "opcname");
			appendStringInfo(&keybuf, " %s",
							 quote_identifier(SPI_getvalue(spi_tup, spi_ttc,
														   spi_fno)));
		}
	}

	/* ----------
	 * For functional index say 'func (attrs) opclass'
	 * ----------
	 */
	if (idxrec->indproc != InvalidOid)
	{
		HeapTuple	proctup;
		Form_pg_proc procStruct;

		proctup = SearchSysCacheTuple(PROOID,
							 ObjectIdGetDatum(idxrec->indproc), 0, 0, 0);
		if (!HeapTupleIsValid(proctup))
			elog(ERROR, "cache lookup for proc %u failed", idxrec->indproc);

		procStruct = (Form_pg_proc) GETSTRUCT(proctup);
		appendStringInfo(&buf, "%s(%s) ",
						 quote_identifier(pstrdup(NameStr(procStruct->proname))),
						 keybuf.data);

		spi_args[0] = ObjectIdGetDatum(idxrec->indclass[0]);
		spi_nulls[0] = ' ';
		spi_nulls[1] = '\0';
		spirc = SPI_execp(plan_getopclass, spi_args, spi_nulls, 1);
		if (spirc != SPI_OK_SELECT)
			elog(ERROR, "failed to get pg_opclass tuple %u", idxrec->indclass[0]);
		if (SPI_processed != 1)
			elog(ERROR, "failed to get pg_opclass tuple %u", idxrec->indclass[0]);
		spi_tup = SPI_tuptable->vals[0];
		spi_ttc = SPI_tuptable->tupdesc;
		spi_fno = SPI_fnumber(spi_ttc, "opcname");
		appendStringInfo(&buf, "%s",
						 quote_identifier(SPI_getvalue(spi_tup, spi_ttc,
													   spi_fno)));
	}
	else
		/* ----------
		 * For the others say 'attr opclass [, ...]'
		 * ----------
		 */
		appendStringInfo(&buf, "%s", keybuf.data);

	/* ----------
	 * Finish
	 * ----------
	 */
	appendStringInfo(&buf, ")");

	/* ----------
	 * Create the result in upper executor memory
	 * ----------
	 */
	len = buf.len + VARHDRSZ;
	indexdef = SPI_palloc(len);
	VARSIZE(indexdef) = len;
	memcpy(VARDATA(indexdef), buf.data, buf.len);
	pfree(buf.data);
	pfree(keybuf.data);

	/* ----------
	 * Disconnect from SPI manager
	 * ----------
	 */
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "get_viewdef: SPI_finish() failed");

	return indexdef;
}


/* ----------
 * get_userbyid			- Get a user name by usesysid and
 *				  fallback to 'unknown (UID=n)'
 * ----------
 */
NameData   *
pg_get_userbyid(int32 uid)
{
	HeapTuple	usertup;
	Form_pg_shadow user_rec;
	NameData   *result;

	/* ----------
	 * Allocate space for the result
	 * ----------
	 */
	result = (NameData *) palloc(NAMEDATALEN);
	memset(NameStr(*result), 0, NAMEDATALEN);

	/* ----------
	 * Get the pg_shadow entry and print the result
	 * ----------
	 */
	usertup = SearchSysCacheTuple(USESYSID,
								  ObjectIdGetDatum(uid), 0, 0, 0);
	if (HeapTupleIsValid(usertup))
	{
		user_rec = (Form_pg_shadow) GETSTRUCT(usertup);
		StrNCpy(NameStr(*result), NameStr(user_rec->usename), NAMEDATALEN);
	}
	else
		sprintf((char *) result, "unknown (UID=%d)", uid);

	return result;
}

/* ----------
 * deparse_expression			- General utility for deparsing expressions
 *
 * expr is the node tree to be deparsed.  It must be a transformed expression
 * tree (ie, not the raw output of gram.y).
 *
 * rangetables is a List of Lists of RangeTblEntry nodes: first sublist is for
 * varlevelsup = 0, next for varlevelsup = 1, etc.  In each sublist the first
 * item is for varno = 1, next varno = 2, etc.  (Each sublist has the same
 * format as the rtable list of a parsetree or query.)
 *
 * forceprefix is TRUE to force all Vars to be prefixed with their table names.
 * Otherwise, a prefix is printed only if there's more than one table involved
 * (and someday the code might try to print one only if there's ambiguity).
 *
 * The result is a palloc'd string.
 * ----------
 */
char *
deparse_expression(Node *expr, List *rangetables, bool forceprefix)
{
	StringInfoData	buf;
	deparse_context	context;

	initStringInfo(&buf);
	context.buf = &buf;
	context.rangetables = rangetables;
	context.varprefix = (forceprefix ||
						 length(rangetables) != 1 ||
						 length((List *) lfirst(rangetables)) != 1);

	rulename = "";				/* in case of errors */

	get_rule_expr(expr, &context);

	return buf.data;
}

/* ----------
 * make_ruledef			- reconstruct the CREATE RULE command
 *				  for a given pg_rewrite tuple
 * ----------
 */
static void
make_ruledef(StringInfo buf, HeapTuple ruletup, TupleDesc rulettc)
{
	char		ev_type;
	Oid			ev_class;
	int2		ev_attr;
	bool		is_instead;
	char	   *ev_qual;
	char	   *ev_action;
	List	   *actions = NIL;
	int			fno;
	bool		isnull;

	/* ----------
	 * Get the attribute values from the rules tuple
	 * ----------
	 */
	fno = SPI_fnumber(rulettc, "ev_type");
	ev_type = (char) SPI_getbinval(ruletup, rulettc, fno, &isnull);

	fno = SPI_fnumber(rulettc, "ev_class");
	ev_class = (Oid) SPI_getbinval(ruletup, rulettc, fno, &isnull);

	fno = SPI_fnumber(rulettc, "ev_attr");
	ev_attr = (int2) SPI_getbinval(ruletup, rulettc, fno, &isnull);

	fno = SPI_fnumber(rulettc, "is_instead");
	is_instead = (bool) SPI_getbinval(ruletup, rulettc, fno, &isnull);

	fno = SPI_fnumber(rulettc, "ev_qual");
	ev_qual = SPI_getvalue(ruletup, rulettc, fno);

	fno = SPI_fnumber(rulettc, "ev_action");
	ev_action = SPI_getvalue(ruletup, rulettc, fno);
	if (ev_action != NULL)
		actions = (List *) stringToNode(ev_action);


	/* ----------
	 * Build the rules definition text
	 * ----------
	 */
	appendStringInfo(buf, "CREATE RULE %s AS ON ",
					 quote_identifier(rulename));

	/* The event the rule is fired for */
	switch (ev_type)
	{
		case '1':
			appendStringInfo(buf, "SELECT");
			break;

		case '2':
			appendStringInfo(buf, "UPDATE");
			break;

		case '3':
			appendStringInfo(buf, "INSERT");
			break;

		case '4':
			appendStringInfo(buf, "DELETE");
			break;

		default:
			elog(ERROR, "get_ruledef: rule %s has unsupported event type %d",
				 rulename, ev_type);
			break;
	}

	/* The relation the rule is fired on */
	appendStringInfo(buf, " TO %s",
					 quote_identifier(get_relation_name(ev_class)));
	if (ev_attr > 0)
		appendStringInfo(buf, ".%s",
						 quote_identifier(get_attribute_name(ev_class,
															 ev_attr)));

	/* If the rule has an event qualification, add it */
	if (ev_qual == NULL)
		ev_qual = "";
	if (strlen(ev_qual) > 0 && strcmp(ev_qual, "<>") != 0)
	{
		Node	   *qual;
		Query	   *query;
		deparse_context	context;

		appendStringInfo(buf, " WHERE ");

		qual = stringToNode(ev_qual);
		query = (Query *) lfirst(actions);

		context.buf = buf;
		context.rangetables = lcons(query->rtable, NIL);
		context.varprefix = (length(query->rtable) != 1);

		get_rule_expr(qual, &context);
	}

	appendStringInfo(buf, " DO ");

	/* The INSTEAD keyword (if so) */
	if (is_instead)
		appendStringInfo(buf, "INSTEAD ");

	/* Finally the rules actions */
	if (length(actions) > 1)
	{
		List	   *action;
		Query	   *query;

		appendStringInfo(buf, "(");
		foreach(action, actions)
		{
			query = (Query *) lfirst(action);
			get_query_def(query, buf, NIL);
			appendStringInfo(buf, "; ");
		}
		appendStringInfo(buf, ");");
	}
	else
	{
		if (length(actions) == 0)
		{
			appendStringInfo(buf, "NOTHING;");
		}
		else
		{
			Query	   *query;

			query = (Query *) lfirst(actions);
			get_query_def(query, buf, NIL);
			appendStringInfo(buf, ";");
		}
	}
}


/* ----------
 * make_viewdef			- reconstruct the SELECT part of a
 *				  view rewrite rule
 * ----------
 */
static void
make_viewdef(StringInfo buf, HeapTuple ruletup, TupleDesc rulettc)
{
	Query	   *query;
	char		ev_type;
	Oid			ev_class;
	int2		ev_attr;
	bool		is_instead;
	char	   *ev_qual;
	char	   *ev_action;
	List	   *actions = NIL;
	int			fno;
	bool		isnull;

	/* ----------
	 * Get the attribute values from the rules tuple
	 * ----------
	 */
	fno = SPI_fnumber(rulettc, "ev_type");
	ev_type = (char) SPI_getbinval(ruletup, rulettc, fno, &isnull);

	fno = SPI_fnumber(rulettc, "ev_class");
	ev_class = (Oid) SPI_getbinval(ruletup, rulettc, fno, &isnull);

	fno = SPI_fnumber(rulettc, "ev_attr");
	ev_attr = (int2) SPI_getbinval(ruletup, rulettc, fno, &isnull);

	fno = SPI_fnumber(rulettc, "is_instead");
	is_instead = (bool) SPI_getbinval(ruletup, rulettc, fno, &isnull);

	fno = SPI_fnumber(rulettc, "ev_qual");
	ev_qual = SPI_getvalue(ruletup, rulettc, fno);

	fno = SPI_fnumber(rulettc, "ev_action");
	ev_action = SPI_getvalue(ruletup, rulettc, fno);
	if (ev_action != NULL)
		actions = (List *) stringToNode(ev_action);

	if (length(actions) != 1)
	{
		appendStringInfo(buf, "Not a view");
		return;
	}

	query = (Query *) lfirst(actions);

	if (ev_type != '1' || ev_attr >= 0 || !is_instead || strcmp(ev_qual, "<>"))
	{
		appendStringInfo(buf, "Not a view");
		return;
	}

	get_query_def(query, buf, NIL);
	appendStringInfo(buf, ";");
}


/* ----------
 * get_query_def			- Parse back one action from
 *					  the parsetree in the actions
 *					  list
 * ----------
 */
static void
get_query_def(Query *query, StringInfo buf, List *parentrtables)
{
	deparse_context	context;

	context.buf = buf;
	context.rangetables = lcons(query->rtable, parentrtables);
	context.varprefix = (parentrtables != NIL ||
						 length(query->rtable) != 1);

	switch (query->commandType)
	{
		case CMD_SELECT:
			get_select_query_def(query, &context);
			break;

		case CMD_UPDATE:
			get_update_query_def(query, &context);
			break;

		case CMD_INSERT:
			get_insert_query_def(query, &context);
			break;

		case CMD_DELETE:
			get_delete_query_def(query, &context);
			break;

		case CMD_NOTHING:
			appendStringInfo(buf, "NOTHING");
			break;

		default:
			elog(ERROR, "get_ruledef of %s: query command type %d not implemented yet",
				 rulename, query->commandType);
			break;
	}
}


/* ----------
 * get_select_query_def			- Parse back a SELECT parsetree
 * ----------
 */
static void
get_select_query_def(Query *query, deparse_context *context)
{
	StringInfo	buf = context->buf;
	char	   *sep;
	TargetEntry *tle;
	RangeTblEntry *rte;
	bool	   *rt_used;
	int			rt_length;
	int			rt_numused = 0;
	bool		rt_constonly = TRUE;
	int			i;
	List	   *l;

	/* ----------
	 * First we need to know which and how many of the
	 * range table entries in the query are used in the target list
	 * or queries qualification
	 * ----------
	 */
	rt_length = length(query->rtable);
	rt_used = palloc(sizeof(bool) * rt_length);
	for (i = 0; i < rt_length; i++)
	{
		if (check_if_rte_used((Node *) (query->targetList), i + 1, 0) ||
			check_if_rte_used(query->qual, i + 1, 0) ||
			check_if_rte_used(query->havingQual, i + 1, 0))
		{
			rt_used[i] = TRUE;
			rt_numused++;
		}
		else
			rt_used[i] = FALSE;
	}

	/* ----------
	 * Now check if any of the used rangetable entries is different
	 * from *NEW* and *CURRENT*. If so we must provide the FROM clause
	 * later.
	 * ----------
	 */
	i = 0;
	foreach(l, query->rtable)
	{
		if (!rt_used[i++])
			continue;

		rte = (RangeTblEntry *) lfirst(l);
		if (!strcmp(rte->refname, "*NEW*"))
			continue;
		if (!strcmp(rte->refname, "*CURRENT*"))
			continue;

		rt_constonly = FALSE;
		break;
	}

	/* ----------
	 * Build up the query string - first we say SELECT
	 * ----------
	 */
	appendStringInfo(buf, "SELECT");

	/* Then we tell what to select (the targetlist) */
	sep = " ";
	foreach(l, query->targetList)
	{
		bool		tell_as = FALSE;

		tle = (TargetEntry *) lfirst(l);
		appendStringInfo(buf, sep);
		sep = ", ";

		get_tle_expr(tle, context);

		/* Check if we must say AS ... */
		if (! IsA(tle->expr, Var))
			tell_as = strcmp(tle->resdom->resname, "?column?");
		else
		{
			Var		   *var = (Var *) (tle->expr);
			char	   *attname;

			rte = get_rte_for_var(var, context);
			attname = get_attribute_name(rte->relid, var->varattno);
			if (strcmp(attname, tle->resdom->resname))
				tell_as = TRUE;
		}

		/* and do if so */
		if (tell_as)
			appendStringInfo(buf, " AS %s",
							 quote_identifier(tle->resdom->resname));
	}

	/* If we need other tables than *NEW* or *CURRENT* add the FROM clause */
	if (!rt_constonly && rt_numused > 0)
	{
		sep = " FROM ";
		i = 0;
		foreach(l, query->rtable)
		{
			if (rt_used[i++])
			{
				rte = (RangeTblEntry *) lfirst(l);

				if (!strcmp(rte->refname, "*NEW*"))
					continue;

				if (!strcmp(rte->refname, "*CURRENT*"))
					continue;

				appendStringInfo(buf, sep);
				sep = ", ";
				appendStringInfo(buf, "%s%s",
								 quote_identifier(rte->relname),
								 inherit_marker(rte));
				if (strcmp(rte->relname, rte->refname) != 0)
					appendStringInfo(buf, " %s",
									 quote_identifier(rte->refname));
			}
		}
	}

	/* Add the WHERE clause if given */
	if (query->qual != NULL)
	{
		appendStringInfo(buf, " WHERE ");
		get_rule_expr(query->qual, context);
	}

	/* Add the GROUP BY CLAUSE */
	if (query->groupClause != NULL)
	{
		appendStringInfo(buf, " GROUP BY ");
		sep = "";
		foreach(l, query->groupClause)
		{
			GroupClause *grp = (GroupClause *) lfirst(l);
			Node *groupexpr;

			groupexpr = get_sortgroupclause_expr(grp,
												 query->targetList);
			appendStringInfo(buf, sep);
			get_rule_expr(groupexpr, context);
			sep = ", ";
		}
	}
}


/* ----------
 * get_insert_query_def			- Parse back an INSERT parsetree
 * ----------
 */
static void
get_insert_query_def(Query *query, deparse_context *context)
{
	StringInfo	buf = context->buf;
	char	   *sep;
	TargetEntry *tle;
	RangeTblEntry *rte;
	bool	   *rt_used;
	int			rt_length;
	int			rt_numused = 0;
	bool		rt_constonly = TRUE;
	int			i;
	List	   *l;

	/* ----------
	 * We need to know if other tables than *NEW* or *CURRENT*
	 * are used in the query. If not, it's an INSERT ... VALUES,
	 * otherwise an INSERT ... SELECT.
	 * ----------
	 */
	rt_length = length(query->rtable);
	rt_used = palloc(sizeof(bool) * rt_length);
	for (i = 0; i < rt_length; i++)
	{
		if (check_if_rte_used((Node *) (query->targetList), i + 1, 0) ||
			check_if_rte_used(query->qual, i + 1, 0) ||
			check_if_rte_used(query->havingQual, i + 1, 0))
		{
			rt_used[i] = TRUE;
			rt_numused++;
		}
		else
			rt_used[i] = FALSE;
	}

	i = 0;
	foreach(l, query->rtable)
	{
		if (!rt_used[i++])
			continue;

		rte = (RangeTblEntry *) lfirst(l);
		if (!strcmp(rte->refname, "*NEW*"))
			continue;
		if (!strcmp(rte->refname, "*CURRENT*"))
			continue;

		rt_constonly = FALSE;
		break;
	}

	/* ----------
	 * Start the query with INSERT INTO relname
	 * ----------
	 */
	rte = rt_fetch(query->resultRelation, query->rtable);
	appendStringInfo(buf, "INSERT INTO %s",
					 quote_identifier(rte->relname));

	/* Add the target list */
	sep = " (";
	foreach(l, query->targetList)
	{
		tle = (TargetEntry *) lfirst(l);

		appendStringInfo(buf, sep);
		sep = ", ";
		appendStringInfo(buf, "%s", quote_identifier(tle->resdom->resname));
	}
	appendStringInfo(buf, ") ");

	/* Add the VALUES or the SELECT */
	if (rt_constonly && query->qual == NULL)
	{
		appendStringInfo(buf, "VALUES (");
		sep = "";
		foreach(l, query->targetList)
		{
			tle = (TargetEntry *) lfirst(l);

			appendStringInfo(buf, sep);
			sep = ", ";
			get_tle_expr(tle, context);
		}
		appendStringInfo(buf, ")");
	}
	else
		get_select_query_def(query, context);
}


/* ----------
 * get_update_query_def			- Parse back an UPDATE parsetree
 * ----------
 */
static void
get_update_query_def(Query *query, deparse_context *context)
{
	StringInfo	buf = context->buf;
	char	   *sep;
	TargetEntry *tle;
	RangeTblEntry *rte;
	List	   *l;

	/* ----------
	 * Start the query with UPDATE relname SET
	 * ----------
	 */
	rte = rt_fetch(query->resultRelation, query->rtable);
	appendStringInfo(buf, "UPDATE %s%s SET ",
					 quote_identifier(rte->relname),
					 inherit_marker(rte));

	/* Add the comma separated list of 'attname = value' */
	sep = "";
	foreach(l, query->targetList)
	{
		tle = (TargetEntry *) lfirst(l);

		appendStringInfo(buf, sep);
		sep = ", ";
		appendStringInfo(buf, "%s = ",
						 quote_identifier(tle->resdom->resname));
		get_tle_expr(tle, context);
	}

	/* Finally add a WHERE clause if given */
	if (query->qual != NULL)
	{
		appendStringInfo(buf, " WHERE ");
		get_rule_expr(query->qual, context);
	}
}


/* ----------
 * get_delete_query_def			- Parse back a DELETE parsetree
 * ----------
 */
static void
get_delete_query_def(Query *query, deparse_context *context)
{
	StringInfo	buf = context->buf;
	RangeTblEntry *rte;

	/* ----------
	 * Start the query with DELETE FROM relname
	 * ----------
	 */
	rte = rt_fetch(query->resultRelation, query->rtable);
	appendStringInfo(buf, "DELETE FROM %s%s",
					 quote_identifier(rte->relname),
					 inherit_marker(rte));

	/* Add a WHERE clause if given */
	if (query->qual != NULL)
	{
		appendStringInfo(buf, " WHERE ");
		get_rule_expr(query->qual, context);
	}
}

/*
 * Find the RTE referenced by a (possibly nonlocal) Var.
 */
static RangeTblEntry *
get_rte_for_var(Var *var, deparse_context *context)
{
	List   *rtlist = context->rangetables;
	int		sup = var->varlevelsup;

	while (sup-- > 0)
		rtlist = lnext(rtlist);

	return rt_fetch(var->varno, (List *) lfirst(rtlist));
}


/* ----------
 * get_rule_expr			- Parse back an expression
 * ----------
 */
static void
get_rule_expr(Node *node, deparse_context *context)
{
	StringInfo	buf = context->buf;

	if (node == NULL)
		return;

	/* ----------
	 * Each level of get_rule_expr must emit an indivisible term
	 * (parenthesized if necessary) to ensure result is reparsed into
	 * the same expression tree.
	 *
	 * There might be some work left here to support additional node types.
	 * Can we ever see Param nodes here?
	 * ----------
	 */
	switch (nodeTag(node))
	{
		case T_Const:
			get_const_expr((Const *) node, context);
			break;

		case T_Var:
			{
				Var		   *var = (Var *) node;
				RangeTblEntry *rte = get_rte_for_var(var, context);

				if (context->varprefix)
				{
					if (!strcmp(rte->refname, "*NEW*"))
						appendStringInfo(buf, "new.");
					else if (!strcmp(rte->refname, "*CURRENT*"))
						appendStringInfo(buf, "old.");
					else
						appendStringInfo(buf, "%s.",
										 quote_identifier(rte->refname));
				}
				appendStringInfo(buf, "%s",
						quote_identifier(get_attribute_name(rte->relid,
															var->varattno)));
			}
			break;

		case T_Expr:
			{
				Expr	   *expr = (Expr *) node;
				List	   *args = expr->args;

				/* ----------
				 * Expr nodes have to be handled a bit detailed
				 * ----------
				 */
				switch (expr->opType)
				{
					case OP_EXPR:
						appendStringInfo(buf, "(");
						if (length(args) == 2)
						{
							/* binary operator */
							get_rule_expr((Node *) lfirst(args), context);
							appendStringInfo(buf, " %s ",
									get_opname(((Oper *) expr->oper)->opno));
							get_rule_expr((Node *) lsecond(args), context);
						}
						else
						{
							/* unary operator --- but which side? */
							Oid			opno = ((Oper *) expr->oper)->opno;
							HeapTuple	tp;
							Form_pg_operator optup;

							tp = SearchSysCacheTuple(OPROID,
													 ObjectIdGetDatum(opno),
													 0, 0, 0);
							Assert(HeapTupleIsValid(tp));
							optup = (Form_pg_operator) GETSTRUCT(tp);
							switch (optup->oprkind)
							{
								case 'l':
									appendStringInfo(buf, "%s ",
													 get_opname(opno));
									get_rule_expr((Node *) lfirst(args),
												  context);
									break;
								case 'r':
									get_rule_expr((Node *) lfirst(args),
												  context);
									appendStringInfo(buf, " %s",
													 get_opname(opno));
									break;
								default:
									elog(ERROR, "get_rule_expr: bogus oprkind");
							}
						}
						appendStringInfo(buf, ")");
						break;

					case OR_EXPR:
						appendStringInfo(buf, "(");
						get_rule_expr((Node *) lfirst(args), context);
						while ((args = lnext(args)) != NIL)
						{
							appendStringInfo(buf, " OR ");
							get_rule_expr((Node *) lfirst(args), context);
						}
						appendStringInfo(buf, ")");
						break;

					case AND_EXPR:
						appendStringInfo(buf, "(");
						get_rule_expr((Node *) lfirst(args), context);
						while ((args = lnext(args)) != NIL)
						{
							appendStringInfo(buf, " AND ");
							get_rule_expr((Node *) lfirst(args), context);
						}
						appendStringInfo(buf, ")");
						break;

					case NOT_EXPR:
						appendStringInfo(buf, "(NOT ");
						get_rule_expr((Node *) lfirst(args), context);
						appendStringInfo(buf, ")");
						break;

					case FUNC_EXPR:
						get_func_expr((Expr *) node, context);
						break;

					default:
						elog(ERROR, "get_rule_expr: expr opType %d not supported",
							 expr->opType);
				}
			}
			break;

		case T_Aggref:
			{
				Aggref	   *aggref = (Aggref *) node;

				appendStringInfo(buf, "%s(",
								 quote_identifier(aggref->aggname));
				get_rule_expr(aggref->target, context);
				appendStringInfo(buf, ")");
			}
			break;

		case T_Iter:
			get_rule_expr(((Iter *) node)->iterexpr, context);
			break;

		case T_ArrayRef:
			{
				ArrayRef   *aref = (ArrayRef *) node;
				List	   *lowlist;
				List	   *uplist;

				get_rule_expr(aref->refexpr, context);
				lowlist = aref->reflowerindexpr;
				foreach(uplist, aref->refupperindexpr)
				{
					appendStringInfo(buf, "[");
					if (lowlist)
					{
						get_rule_expr((Node *) lfirst(lowlist), context);
						appendStringInfo(buf, ":");
						lowlist = lnext(lowlist);
					}
					get_rule_expr((Node *) lfirst(uplist), context);
					appendStringInfo(buf, "]");
				}
				/* XXX need to do anything with refassgnexpr? */
			}
			break;

		case T_CaseExpr:
			{
				CaseExpr   *caseexpr = (CaseExpr *) node;
				List	   *temp;

				appendStringInfo(buf, "CASE");
				foreach(temp, caseexpr->args)
				{
					CaseWhen   *when = (CaseWhen *) lfirst(temp);

					appendStringInfo(buf, " WHEN ");
					get_rule_expr(when->expr, context);
					appendStringInfo(buf, " THEN ");
					get_rule_expr(when->result, context);
				}
				appendStringInfo(buf, " ELSE ");
				get_rule_expr(caseexpr->defresult, context);
				appendStringInfo(buf, " END");
			}
			break;

		case T_SubLink:
			get_sublink_expr(node, context);
			break;

		default:
			printf("\n%s\n", nodeToString(node));
			elog(ERROR, "get_ruledef of %s: unknown node type %d in get_rule_expr()",
				 rulename, nodeTag(node));
			break;
	}
}


/* ----------
 * get_func_expr			- Parse back a Func node
 * ----------
 */
static void
get_func_expr(Expr *expr, deparse_context *context)
{
	StringInfo	buf = context->buf;
	HeapTuple	proctup;
	Form_pg_proc procStruct;
	List	   *l;
	char	   *sep;
	Func	   *func = (Func *) (expr->oper);
	char	   *proname;

	/* ----------
	 * Get the functions pg_proc tuple
	 * ----------
	 */
	proctup = SearchSysCacheTuple(PROOID,
								  ObjectIdGetDatum(func->funcid),
								  0, 0, 0);
	if (!HeapTupleIsValid(proctup))
		elog(ERROR, "cache lookup for proc %u failed", func->funcid);

	procStruct = (Form_pg_proc) GETSTRUCT(proctup);
	proname = pstrdup(NameStr(procStruct->proname));

	/*
	 * nullvalue() and nonnullvalue() should get turned into special syntax
	 */
	if (procStruct->pronargs == 1 && procStruct->proargtypes[0] == InvalidOid)
	{
		if (!strcmp(proname, "nullvalue"))
		{
			appendStringInfo(buf, "(");
			get_rule_expr((Node *) lfirst(expr->args), context);
			appendStringInfo(buf, " ISNULL)");
			return;
		}
		if (!strcmp(proname, "nonnullvalue"))
		{
			appendStringInfo(buf, "(");
			get_rule_expr((Node *) lfirst(expr->args), context);
			appendStringInfo(buf, " NOTNULL)");
			return;
		}
	}

	/* ----------
	 * Build a string of proname(args)
	 * ----------
	 */
	appendStringInfo(buf, "%s(", quote_identifier(proname));
	sep = "";
	foreach(l, expr->args)
	{
		appendStringInfo(buf, sep);
		sep = ", ";
		get_rule_expr((Node *) lfirst(l), context);
	}
	appendStringInfo(buf, ")");
}


/* ----------
 * get_tle_expr
 *
 *		A target list expression is a bit different from a normal expression.
 *		If the target column has an atttypmod, the parser usually puts a
 *		padding-/cut-function call around the expression itself.
 *		We must get rid of it, otherwise dump/reload/dump... would blow up
 *		the expressions.
 * ----------
 */
static void
get_tle_expr(TargetEntry *tle, deparse_context *context)
{
	Expr	   *expr = (Expr *) (tle->expr);
	Func	   *func;
	HeapTuple	tup;
	Form_pg_proc procStruct;
	Form_pg_type typeStruct;
	Const	   *second_arg;

	/* ----------
	 * Check if the result has an atttypmod and if the
	 * expression in the targetlist entry is a function call
	 * ----------
	 */
	if (tle->resdom->restypmod < 0 ||
		! IsA(expr, Expr) ||
		expr->opType != FUNC_EXPR)
	{
		get_rule_expr(tle->expr, context);
		return;
	}

	func = (Func *) (expr->oper);

	/* ----------
	 * Get the functions pg_proc tuple
	 * ----------
	 */
	tup = SearchSysCacheTuple(PROOID,
							  ObjectIdGetDatum(func->funcid), 0, 0, 0);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup for proc %u failed", func->funcid);
	procStruct = (Form_pg_proc) GETSTRUCT(tup);

	/* ----------
	 * It must be a function with two arguments where the first
	 * is of the same type as the return value and the second is
	 * an int4.
	 * ----------
	 */
	if (procStruct->pronargs != 2 ||
		procStruct->prorettype != procStruct->proargtypes[0] ||
		procStruct->proargtypes[1] != INT4OID)
	{
		get_rule_expr(tle->expr, context);
		return;
	}

	/*
	 * Furthermore, the name of the function must be the same
	 * as the argument/result type name.
	 */
	tup = SearchSysCacheTuple(TYPOID,
							  ObjectIdGetDatum(procStruct->prorettype),
							  0, 0, 0);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup for type %u failed",
			 procStruct->prorettype);
	typeStruct = (Form_pg_type) GETSTRUCT(tup);
	if (strncmp(NameStr(procStruct->proname),
				NameStr(typeStruct->typname),
				NAMEDATALEN) != 0)
	{
		get_rule_expr(tle->expr, context);
		return;
	}

	/* ----------
	 * Finally (to be totally safe) the second argument must be a
	 * const and match the value in the results atttypmod.
	 * ----------
	 */
	second_arg = (Const *) lsecond(expr->args);
	if (! IsA(second_arg, Const) ||
		DatumGetInt32(second_arg->constvalue) != tle->resdom->restypmod)
	{
		get_rule_expr(tle->expr, context);
		return;
	}

	/* ----------
	 * Whow - got it. Now get rid of the padding function
	 * ----------
	 */
	get_rule_expr((Node *) lfirst(expr->args), context);
}


/* ----------
 * get_const_expr
 *
 *	Make a string representation of a Const
 * ----------
 */
static void
get_const_expr(Const *constval, deparse_context *context)
{
	StringInfo	buf = context->buf;
	HeapTuple	typetup;
	Form_pg_type typeStruct;
	FmgrInfo	finfo_output;
	char	   *extval;
	char	   *valptr;
	bool		isnull = FALSE;

	if (constval->constisnull)
	{
		appendStringInfo(buf, "NULL");
		return;
	}

	typetup = SearchSysCacheTuple(TYPOID,
								  ObjectIdGetDatum(constval->consttype),
								  0, 0, 0);
	if (!HeapTupleIsValid(typetup))
		elog(ERROR, "cache lookup of type %u failed", constval->consttype);

	typeStruct = (Form_pg_type) GETSTRUCT(typetup);

	fmgr_info(typeStruct->typoutput, &finfo_output);
	extval = (char *) (*fmgr_faddr(&finfo_output)) (constval->constvalue,
													&isnull, -1);

	switch (constval->consttype)
	{
		case INT2OID:
		case INT4OID:
		case OIDOID:			/* int types */
		case FLOAT4OID:
		case FLOAT8OID:			/* float types */
			/* These types are printed without quotes */
			appendStringInfo(buf, extval);
			break;
		default:
			/*
			 * We must quote any funny characters in the constant's
			 * representation.
			 * XXX Any MULTIBYTE considerations here?
			 */
			appendStringInfoChar(buf, '\'');
			for (valptr = extval; *valptr; valptr++)
			{
				char	ch = *valptr;
				if (ch == '\'' || ch == '\\')
				{
					appendStringInfoChar(buf, '\\');
					appendStringInfoChar(buf, ch);
				}
				else if (ch >= 0 && ch < ' ')
					appendStringInfo(buf, "\\%03o", (int) ch);
				else
					appendStringInfoChar(buf, ch);
			}
			appendStringInfoChar(buf, '\'');
			break;
	}

	pfree(extval);

	switch (constval->consttype)
	{
		case INT4OID:
		case FLOAT8OID:
		case UNKNOWNOID:
			/* These types can be left unlabeled */
			break;
		default:
			extval = pstrdup(NameStr(typeStruct->typname));
			appendStringInfo(buf, "::%s", quote_identifier(extval));
			pfree(extval);
			break;
	}
}


/* ----------
 * get_sublink_expr			- Parse back a sublink
 * ----------
 */
static void
get_sublink_expr(Node *node, deparse_context *context)
{
	StringInfo	buf = context->buf;
	SubLink    *sublink = (SubLink *) node;
	Query	   *query = (Query *) (sublink->subselect);
	Oper	   *oper;
	List	   *l;
	char	   *sep;

	appendStringInfo(buf, "(");

	if (sublink->lefthand != NULL)
	{
		if (length(sublink->lefthand) > 1)
			appendStringInfo(buf, "(");

		sep = "";
		foreach(l, sublink->lefthand)
		{
			appendStringInfo(buf, sep);
			sep = ", ";
			get_rule_expr((Node *) lfirst(l), context);
		}

		if (length(sublink->lefthand) > 1)
			appendStringInfo(buf, ") ");
		else
			appendStringInfo(buf, " ");
	}

	switch (sublink->subLinkType)
	{
		case EXISTS_SUBLINK:
			appendStringInfo(buf, "EXISTS ");
			break;

		case ANY_SUBLINK:
			oper = (Oper *) lfirst(sublink->oper);
			appendStringInfo(buf, "%s ANY ", get_opname(oper->opno));
			break;

		case ALL_SUBLINK:
			oper = (Oper *) lfirst(sublink->oper);
			appendStringInfo(buf, "%s ALL ", get_opname(oper->opno));
			break;

		case EXPR_SUBLINK:
			oper = (Oper *) lfirst(sublink->oper);
			appendStringInfo(buf, "%s ", get_opname(oper->opno));
			break;

		default:
			elog(ERROR, "get_sublink_expr: unsupported sublink type %d",
				 sublink->subLinkType);
			break;
	}

	appendStringInfo(buf, "(");
	get_query_def(query, buf, context->rangetables);
	appendStringInfo(buf, "))");
}

/* ----------
 * quote_identifier			- Quote an identifier only if needed
 *
 * When quotes are needed, we palloc the required space; slightly
 * space-wasteful but well worth it for notational simplicity.
 * ----------
 */
static char *
quote_identifier(char *ident)
{
	/*
	 * Can avoid quoting if ident starts with a lowercase letter and
	 * contains only lowercase letters, digits, and underscores.
	 * Otherwise, supply quotes.
	 */
	bool		safe;
	char	   *result;

	/*
	 * would like to use <ctype.h> macros here, but they might yield
	 * unwanted locale-specific results...
	 */
	safe = (ident[0] >= 'a' && ident[0] <= 'z');
	if (safe)
	{
		char	   *ptr;

		for (ptr = ident+1; *ptr; ptr++)
		{
			char	ch = *ptr;

			safe = ((ch >= 'a' && ch <= 'z') ||
					(ch >= '0' && ch <= '9') ||
					(ch == '_'));
			if (! safe)
				break;
		}
	}

	if (safe)
		return ident;			/* no change needed */

	result = (char *) palloc(strlen(ident) + 2 + 1);
	sprintf(result, "\"%s\"", ident);
	return result;
}

/* ----------
 * get_relation_name			- Get a relation name by Oid
 * ----------
 */
static char *
get_relation_name(Oid relid)
{
	HeapTuple	classtup;
	Form_pg_class classStruct;

	classtup = SearchSysCacheTuple(RELOID,
								   ObjectIdGetDatum(relid), 0, 0, 0);
	if (!HeapTupleIsValid(classtup))
		elog(ERROR, "cache lookup of relation %u failed", relid);

	classStruct = (Form_pg_class) GETSTRUCT(classtup);
	return pstrdup(NameStr(classStruct->relname));
}


/* ----------
 * get_attribute_name			- Get an attribute name by it's
 *					  relations Oid and it's attnum
 * ----------
 */
static char *
get_attribute_name(Oid relid, int2 attnum)
{
	HeapTuple	atttup;
	Form_pg_attribute attStruct;

	atttup = SearchSysCacheTuple(ATTNUM,
								 ObjectIdGetDatum(relid), (Datum) attnum,
								 0, 0);
	if (!HeapTupleIsValid(atttup))
		elog(ERROR, "cache lookup of attribute %d in relation %u failed",
			 attnum, relid);

	attStruct = (Form_pg_attribute) GETSTRUCT(atttup);
	return pstrdup(NameStr(attStruct->attname));
}


/* ----------
 * check_if_rte_used
 *		Check a targetlist or qual to see if a given rangetable entry
 *		is used in it
 * ----------
 */
static bool
check_if_rte_used(Node *node, Index rt_index, int levelsup)
{
	check_if_rte_used_context context;

	context.rt_index = rt_index;
	context.levelsup = levelsup;
	return check_if_rte_used_walker(node, &context);
}

static bool
check_if_rte_used_walker(Node *node,
						 check_if_rte_used_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		return var->varno == context->rt_index &&
			var->varlevelsup == context->levelsup;
	}
	if (IsA(node, SubLink))
	{
		SubLink    *sublink = (SubLink *) node;
		Query	   *query = (Query *) sublink->subselect;

		/* Recurse into subquery; expression_tree_walker will not */
		if (check_if_rte_used((Node *) (query->targetList),
							  context->rt_index, context->levelsup + 1) ||
			check_if_rte_used(query->qual,
							  context->rt_index, context->levelsup + 1) ||
			check_if_rte_used(query->havingQual,
							  context->rt_index, context->levelsup + 1))
			return true;
		/* fall through to let expression_tree_walker examine lefthand args */
	}
	return expression_tree_walker(node, check_if_rte_used_walker,
								  (void *) context);
}
