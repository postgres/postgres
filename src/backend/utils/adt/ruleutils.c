/**********************************************************************
 * ruleutils.c	- Functions to convert stored expressions/querytrees
 *				back to source text
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/ruleutils.c,v 1.73 2001/02/21 18:53:47 tgl Exp $
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

#include "postgres.h"

#include <unistd.h>
#include <fcntl.h>

#include "catalog/pg_index.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_shadow.h"
#include "commands/view.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/tlist.h"
#include "parser/keywords.h"
#include "parser/parse_expr.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"


/* ----------
 * Local data types
 * ----------
 */

/* Context info needed for invoking a recursive querytree display routine */
typedef struct
{
	StringInfo	buf;			/* output buffer to append to */
	List	   *namespaces;		/* List of deparse_namespace nodes */
	bool		varprefix;		/* TRUE to print prefixes on Vars */
} deparse_context;

/*
 * Each level of query context around a subtree needs a level of Var namespace.
 * The rangetable is the list of actual RTEs, and the namespace indicates
 * which parts of the rangetable are accessible (and under what aliases)
 * in the expression currently being looked at.  A Var having varlevelsup=N
 * refers to the N'th item (counting from 0) in the current context's
 * namespaces list.
 */
typedef struct
{
	List	   *rtable;			/* List of RangeTblEntry nodes */
	List	   *namespace;		/* List of joinlist items (RangeTblRef and
								 * JoinExpr nodes) */
} deparse_namespace;


/* ----------
 * Global data
 * ----------
 */
static char *rulename = NULL;
static void *plan_getrule = NULL;
static char *query_getrule = "SELECT * FROM pg_rewrite WHERE rulename = $1";
static void *plan_getview = NULL;
static char *query_getview = "SELECT * FROM pg_rewrite WHERE rulename = $1";
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
static void get_query_def(Query *query, StringInfo buf, List *parentnamespace);
static void get_select_query_def(Query *query, deparse_context *context);
static void get_insert_query_def(Query *query, deparse_context *context);
static void get_update_query_def(Query *query, deparse_context *context);
static void get_delete_query_def(Query *query, deparse_context *context);
static void get_utility_query_def(Query *query, deparse_context *context);
static void get_basic_select_query(Query *query, deparse_context *context);
static void get_setop_query(Node *setOp, Query *query,
							deparse_context *context, bool toplevel);
static bool simple_distinct(List *distinctClause, List *targetList);
static void get_names_for_var(Var *var, deparse_context *context,
							  char **refname, char **attname);
static bool get_alias_for_case(CaseExpr *caseexpr, deparse_context *context,
							   char **refname, char **attname);
static bool find_alias_in_namespace(Node *nsnode, Node *expr,
									List *rangetable, int levelsup,
									char **refname, char **attname);
static bool phony_equal(Node *expr1, Node *expr2, int levelsup);
static void get_rule_expr(Node *node, deparse_context *context);
static void get_func_expr(Expr *expr, deparse_context *context);
static void get_tle_expr(TargetEntry *tle, deparse_context *context);
static void get_const_expr(Const *constval, deparse_context *context);
static void get_sublink_expr(Node *node, deparse_context *context);
static void get_from_clause(Query *query, deparse_context *context);
static void get_from_clause_item(Node *jtnode, Query *query,
								 deparse_context *context);
static bool tleIsArrayAssign(TargetEntry *tle);
static char *quote_identifier(char *ident);
static char *get_relation_name(Oid relid);
static char *get_relid_attribute_name(Oid relid, AttrNumber attnum);

#define only_marker(rte)  ((rte)->inh ? "" : "ONLY ")


/* ----------
 * get_ruledef			- Do it all and return a text
 *				  that could be used as a statement
 *				  to recreate the rule
 * ----------
 */
Datum
pg_get_ruledef(PG_FUNCTION_ARGS)
{
	Name		rname = PG_GETARG_NAME(0);
	text	   *ruledef;
	Datum		args[1];
	char		nulls[2];
	int			spirc;
	HeapTuple	ruletup;
	TupleDesc	rulettc;
	StringInfoData buf;
	int			len;

	/* ----------
	 * We need the rules name somewhere deep down: rulename is global
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
		ruledef = palloc(VARHDRSZ + 1);
		VARATT_SIZEP(ruledef) = VARHDRSZ + 1;
		VARDATA(ruledef)[0] = '-';
		PG_RETURN_TEXT_P(ruledef);
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
	VARATT_SIZEP(ruledef) = len;
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
	PG_RETURN_TEXT_P(ruledef);
}


/* ----------
 * get_viewdef			- Mainly the same thing, but we
 *				  only return the SELECT part of a view
 * ----------
 */
Datum
pg_get_viewdef(PG_FUNCTION_ARGS)
{
	Name		vname = PG_GETARG_NAME(0);
	text	   *ruledef;
	Datum		args[1];
	char		nulls[1];
	int			spirc;
	HeapTuple	ruletup;
	TupleDesc	rulettc;
	StringInfoData buf;
	int			len;
	char	   *name;

	/* ----------
	 * We need the view name somewhere deep down
	 * ----------
	 */
	rulename = pstrdup(NameStr(*vname));

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
		Oid			argtypes[1];
		void	   *plan;

		argtypes[0] = NAMEOID;
		plan = SPI_prepare(query_getview, 1, argtypes);
		if (plan == NULL)
			elog(ERROR, "SPI_prepare() failed for \"%s\"", query_getview);
		plan_getview = SPI_saveplan(plan);
	}

	/* ----------
	 * Get the pg_rewrite tuple for this rule: rulename is actually viewname here
	 * ----------
	 */
	name = MakeRetrieveViewRuleName(rulename);
	args[0] = PointerGetDatum(name);
	nulls[0] = ' ';
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
	VARATT_SIZEP(ruledef) = len;
	memcpy(VARDATA(ruledef), buf.data, buf.len);
	pfree(buf.data);
	pfree(name);

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
	PG_RETURN_TEXT_P(ruledef);
}


/* ----------
 * get_indexdef			- Get the definition of an index
 * ----------
 */
Datum
pg_get_indexdef(PG_FUNCTION_ARGS)
{
	Oid			indexrelid = PG_GETARG_OID(0);
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
	StringInfoData buf;
	StringInfoData keybuf;
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
	ht_idx = SearchSysCache(INDEXRELID,
							ObjectIdGetDatum(indexrelid),
							0, 0, 0);
	if (!HeapTupleIsValid(ht_idx))
		elog(ERROR, "syscache lookup for index %u failed", indexrelid);
	idxrec = (Form_pg_index) GETSTRUCT(ht_idx);

	/* ----------
	 * Fetch the pg_class tuple of the index relation
	 * ----------
	 */
	ht_idxrel = SearchSysCache(RELOID,
							   ObjectIdGetDatum(idxrec->indexrelid),
							   0, 0, 0);
	if (!HeapTupleIsValid(ht_idxrel))
		elog(ERROR, "syscache lookup for relid %u failed", idxrec->indexrelid);
	idxrelrec = (Form_pg_class) GETSTRUCT(ht_idxrel);

	/* ----------
	 * Fetch the pg_class tuple of the indexed relation
	 * ----------
	 */
	ht_indrel = SearchSysCache(RELOID,
							   ObjectIdGetDatum(idxrec->indrelid),
							   0, 0, 0);
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
			 NameStr(idxrelrec->relname));
	if (SPI_processed != 1)
		elog(ERROR, "failed to get pg_am tuple for index %s",
			 NameStr(idxrelrec->relname));
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
				quote_identifier(get_relid_attribute_name(idxrec->indrelid,
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

		proctup = SearchSysCache(PROCOID,
								 ObjectIdGetDatum(idxrec->indproc),
								 0, 0, 0);
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
		ReleaseSysCache(proctup);
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
	 * Create the result in upper executor memory, and free objects
	 * ----------
	 */
	len = buf.len + VARHDRSZ;
	indexdef = SPI_palloc(len);
	VARATT_SIZEP(indexdef) = len;
	memcpy(VARDATA(indexdef), buf.data, buf.len);

	pfree(buf.data);
	pfree(keybuf.data);
	ReleaseSysCache(ht_idx);
	ReleaseSysCache(ht_idxrel);
	ReleaseSysCache(ht_indrel);

	/* ----------
	 * Disconnect from SPI manager
	 * ----------
	 */
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "get_viewdef: SPI_finish() failed");

	PG_RETURN_TEXT_P(indexdef);
}


/* ----------
 * get_userbyid			- Get a user name by usesysid and
 *				  fallback to 'unknown (UID=n)'
 * ----------
 */
Datum
pg_get_userbyid(PG_FUNCTION_ARGS)
{
	int32		uid = PG_GETARG_INT32(0);
	Name		result;
	HeapTuple	usertup;
	Form_pg_shadow user_rec;

	/* ----------
	 * Allocate space for the result
	 * ----------
	 */
	result = (Name) palloc(NAMEDATALEN);
	memset(NameStr(*result), 0, NAMEDATALEN);

	/* ----------
	 * Get the pg_shadow entry and print the result
	 * ----------
	 */
	usertup = SearchSysCache(SHADOWSYSID,
							 ObjectIdGetDatum(uid),
							 0, 0, 0);
	if (HeapTupleIsValid(usertup))
	{
		user_rec = (Form_pg_shadow) GETSTRUCT(usertup);
		StrNCpy(NameStr(*result), NameStr(user_rec->usename), NAMEDATALEN);
		ReleaseSysCache(usertup);
	}
	else
		sprintf(NameStr(*result), "unknown (UID=%d)", uid);

	PG_RETURN_NAME(result);
}

/* ----------
 * deparse_expression			- General utility for deparsing expressions
 *
 * expr is the node tree to be deparsed.  It must be a transformed expression
 * tree (ie, not the raw output of gram.y).
 *
 * dpcontext is a list of deparse_namespace nodes representing the context
 * for interpreting Vars in the node tree.
 *
 * forceprefix is TRUE to force all Vars to be prefixed with their table names.
 *
 * The result is a palloc'd string.
 * ----------
 */
char *
deparse_expression(Node *expr, List *dpcontext, bool forceprefix)
{
	StringInfoData buf;
	deparse_context context;

	initStringInfo(&buf);
	context.buf = &buf;
	context.namespaces = dpcontext;
	context.varprefix = forceprefix;

	rulename = "";				/* in case of errors */

	get_rule_expr(expr, &context);

	return buf.data;
}

/* ----------
 * deparse_context_for			- Build deparse context for a single relation
 *
 * Given the name and OID of a relation, build deparsing context for an
 * expression referencing only that relation (as varno 1, varlevelsup 0).
 * This is presently sufficient for the external uses of deparse_expression.
 * ----------
 */
List *
deparse_context_for(char *relname, Oid relid)
{
	deparse_namespace *dpns;
	RangeTblEntry *rte;
	RangeTblRef *rtr;

	dpns = (deparse_namespace *) palloc(sizeof(deparse_namespace));

	/* Build a minimal RTE for the rel */
	rte = makeNode(RangeTblEntry);
	rte->relname = relname;
	rte->relid = relid;
	rte->eref = makeNode(Attr);
	rte->eref->relname = relname;
	rte->inh = false;
	rte->inFromCl = true;
	/* Build one-element rtable */
	dpns->rtable = makeList1(rte);

	/* Build a namespace list referencing this RTE only */
	rtr = makeNode(RangeTblRef);
	rtr->rtindex = 1;
	dpns->namespace = makeList1(rtr);

	/* Return a one-deep namespace stack */
	return makeList1(dpns);
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
						 quote_identifier(get_relid_attribute_name(ev_class,
																   ev_attr)));

	/* If the rule has an event qualification, add it */
	if (ev_qual == NULL)
		ev_qual = "";
	if (strlen(ev_qual) > 0 && strcmp(ev_qual, "<>") != 0)
	{
		Node	   *qual;
		Query	   *query;
		deparse_context context;
		deparse_namespace dpns;

		appendStringInfo(buf, " WHERE ");

		qual = stringToNode(ev_qual);
		query = (Query *) lfirst(actions);

		context.buf = buf;
		context.namespaces = makeList1(&dpns);
		context.varprefix = (length(query->rtable) != 1);
		dpns.rtable = query->rtable;
		dpns.namespace = query->jointree ? query->jointree->fromlist : NIL;

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

	if (ev_type != '1' || ev_attr >= 0 || !is_instead ||
		strcmp(ev_qual, "<>") != 0)
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
get_query_def(Query *query, StringInfo buf, List *parentnamespace)
{
	deparse_context context;
	deparse_namespace dpns;

	context.buf = buf;
	context.namespaces = lcons(&dpns, parentnamespace);
	context.varprefix = (parentnamespace != NIL ||
						 length(query->rtable) != 1);
	dpns.rtable = query->rtable;
	dpns.namespace = query->jointree ? query->jointree->fromlist : NIL;

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

		case CMD_UTILITY:
			get_utility_query_def(query, &context);
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
	bool		shortform_orderby;
	char	   *sep;
	List	   *l;

	/* ----------
	 * If the Query node has a setOperations tree, then it's the top
	 * level of a UNION/INTERSECT/EXCEPT query; only the ORDER BY and
	 * LIMIT fields are interesting in the top query itself.
	 * ----------
	 */
	if (query->setOperations)
	{
		get_setop_query(query->setOperations, query, context, true);
		/* ORDER BY clauses must be simple in this case */
		shortform_orderby = true;
	}
	else
	{
		get_basic_select_query(query, context);
		shortform_orderby = false;
	}

	/* Add the ORDER BY clause if given */
	if (query->sortClause != NIL)
	{
		appendStringInfo(buf, " ORDER BY ");
		sep = "";
		foreach(l, query->sortClause)
		{
			SortClause *srt = (SortClause *) lfirst(l);
			TargetEntry *sorttle;
			char	   *opname;

			sorttle = get_sortgroupclause_tle(srt,
											  query->targetList);
			appendStringInfo(buf, sep);
			if (shortform_orderby)
				appendStringInfo(buf, "%d", sorttle->resdom->resno);
			else
				get_rule_expr(sorttle->expr, context);
			opname = get_opname(srt->sortop);
			if (strcmp(opname, "<") != 0)
			{
				if (strcmp(opname, ">") == 0)
					appendStringInfo(buf, " DESC");
				else
					appendStringInfo(buf, " USING %s", opname);
			}
			sep = ", ";
		}
	}

	/* Add the LIMIT clause if given */
	if (query->limitOffset != NULL)
	{
		appendStringInfo(buf, " OFFSET ");
		get_rule_expr(query->limitOffset, context);
	}
	if (query->limitCount != NULL)
	{
		appendStringInfo(buf, " LIMIT ");
		if (IsA(query->limitCount, Const) &&
			((Const *) query->limitCount)->constisnull)
			appendStringInfo(buf, "ALL");
		else
			get_rule_expr(query->limitCount, context);
	}
}

static void
get_basic_select_query(Query *query, deparse_context *context)
{
	StringInfo	buf = context->buf;
	char	   *sep;
	List	   *l;

	/* ----------
	 * Build up the query string - first we say SELECT
	 * ----------
	 */
	appendStringInfo(buf, "SELECT");

	/* Add the DISTINCT clause if given */
	if (query->distinctClause != NIL)
	{
		if (simple_distinct(query->distinctClause, query->targetList))
		{
			appendStringInfo(buf, " DISTINCT");
		}
		else
		{
			appendStringInfo(buf, " DISTINCT ON (");
			sep = "";
			foreach(l, query->distinctClause)
			{
				SortClause *srt = (SortClause *) lfirst(l);
				Node	   *sortexpr;

				sortexpr = get_sortgroupclause_expr(srt,
													query->targetList);
				appendStringInfo(buf, sep);
				get_rule_expr(sortexpr, context);
				sep = ", ";
			}
			appendStringInfo(buf, ")");
		}
	}

	/* Then we tell what to select (the targetlist) */
	sep = " ";
	foreach(l, query->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);
		bool		tell_as = false;

		if (tle->resdom->resjunk)
			continue;			/* ignore junk entries */

		appendStringInfo(buf, sep);
		sep = ", ";

		/* Do NOT use get_tle_expr here; see its comments! */
		get_rule_expr(tle->expr, context);

		/* Check if we must say AS ... */
		if (!IsA(tle->expr, Var))
			tell_as = (strcmp(tle->resdom->resname, "?column?") != 0);
		else
		{
			Var		   *var = (Var *) (tle->expr);
			char	   *refname;
			char	   *attname;

			get_names_for_var(var, context, &refname, &attname);
			tell_as = (strcmp(attname, tle->resdom->resname) != 0);
		}

		/* and do if so */
		if (tell_as)
			appendStringInfo(buf, " AS %s",
							 quote_identifier(tle->resdom->resname));
	}

	/* Add the FROM clause if needed */
	get_from_clause(query, context);

	/* Add the WHERE clause if given */
	if (query->jointree->quals != NULL)
	{
		appendStringInfo(buf, " WHERE ");
		get_rule_expr(query->jointree->quals, context);
	}

	/* Add the GROUP BY clause if given */
	if (query->groupClause != NULL)
	{
		appendStringInfo(buf, " GROUP BY ");
		sep = "";
		foreach(l, query->groupClause)
		{
			GroupClause *grp = (GroupClause *) lfirst(l);
			Node	   *groupexpr;

			groupexpr = get_sortgroupclause_expr(grp,
												 query->targetList);
			appendStringInfo(buf, sep);
			get_rule_expr(groupexpr, context);
			sep = ", ";
		}
	}

	/* Add the HAVING clause if given */
	if (query->havingQual != NULL)
	{
		appendStringInfo(buf, " HAVING ");
		get_rule_expr(query->havingQual, context);
	}
}

static void
get_setop_query(Node *setOp, Query *query, deparse_context *context,
				bool toplevel)
{
	StringInfo	buf = context->buf;

	if (IsA(setOp, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) setOp;
		RangeTblEntry *rte = rt_fetch(rtr->rtindex, query->rtable);
		Query  *subquery = rte->subquery;

		Assert(subquery != NULL);
		get_query_def(subquery, buf, context->namespaces);
	}
	else if (IsA(setOp, SetOperationStmt))
	{
		SetOperationStmt *op = (SetOperationStmt *) setOp;

		/* Must suppress parens at top level of a setop tree because
		 * of grammar limitations...
		 */
		if (! toplevel)
			appendStringInfo(buf, "(");
		get_setop_query(op->larg, query, context, false);
		switch (op->op)
		{
			case SETOP_UNION:
				appendStringInfo(buf, " UNION ");
				break;
			case SETOP_INTERSECT:
				appendStringInfo(buf, " INTERSECT ");
				break;
			case SETOP_EXCEPT:
				appendStringInfo(buf, " EXCEPT ");
				break;
			default:
				elog(ERROR, "get_setop_query: unexpected set op %d",
					 (int) op->op);
		}
		if (op->all)
			appendStringInfo(buf, "ALL ");
		get_setop_query(op->rarg, query, context, false);
		if (! toplevel)
			appendStringInfo(buf, ")");
	}
	else
	{
		elog(ERROR, "get_setop_query: unexpected node %d",
			 (int) nodeTag(setOp));
	}
}

/*
 * Detect whether a DISTINCT list can be represented as just DISTINCT
 * or needs DISTINCT ON.  It's simple if it contains exactly the nonjunk
 * targetlist items.
 */
static bool
simple_distinct(List *distinctClause, List *targetList)
{
	while (targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(targetList);

		if (! tle->resdom->resjunk)
		{
			if (distinctClause == NIL)
				return false;
			if (((SortClause *) lfirst(distinctClause))->tleSortGroupRef !=
				tle->resdom->ressortgroupref)
				return false;
			distinctClause = lnext(distinctClause);
		}
		targetList = lnext(targetList);
	}
	if (distinctClause != NIL)
		return false;
	return true;
}


/* ----------
 * get_insert_query_def			- Parse back an INSERT parsetree
 * ----------
 */
static void
get_insert_query_def(Query *query, deparse_context *context)
{
	StringInfo	buf = context->buf;
	RangeTblEntry *select_rte = NULL;
	RangeTblEntry *rte;
	char	   *sep;
	List	   *l;

	/* ----------
	 * If it's an INSERT ... SELECT there will be a single subquery RTE
	 * for the SELECT.
	 * ----------
	 */
	foreach(l, query->rtable)
	{
		rte = (RangeTblEntry *) lfirst(l);
		if (rte->subquery == NULL)
			continue;
		if (select_rte)
			elog(ERROR, "get_insert_query_def: too many RTEs in INSERT!");
		select_rte = rte;
	}

	/* ----------
	 * Start the query with INSERT INTO relname
	 * ----------
	 */
	rte = rt_fetch(query->resultRelation, query->rtable);
	appendStringInfo(buf, "INSERT INTO %s",
					 quote_identifier(rte->relname));

	/* Add the insert-column-names list */
	sep = " (";
	foreach(l, query->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->resdom->resjunk)
			continue;			/* ignore junk entries */

		appendStringInfo(buf, sep);
		sep = ", ";
		appendStringInfo(buf, "%s", quote_identifier(tle->resdom->resname));
	}
	appendStringInfo(buf, ") ");

	/* Add the VALUES or the SELECT */
	if (select_rte == NULL)
	{
		appendStringInfo(buf, "VALUES (");
		sep = "";
		foreach(l, query->targetList)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(l);

			if (tle->resdom->resjunk)
				continue;		/* ignore junk entries */

			appendStringInfo(buf, sep);
			sep = ", ";
			get_tle_expr(tle, context);
		}
		appendStringInfoChar(buf, ')');
	}
	else
	{
		get_query_def(select_rte->subquery, buf, NIL);
	}
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
	RangeTblEntry *rte;
	List	   *l;

	/* ----------
	 * Start the query with UPDATE relname SET
	 * ----------
	 */
	rte = rt_fetch(query->resultRelation, query->rtable);
	appendStringInfo(buf, "UPDATE %s%s SET ",
					 only_marker(rte),
					 quote_identifier(rte->relname));

	/* Add the comma separated list of 'attname = value' */
	sep = "";
	foreach(l, query->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->resdom->resjunk)
			continue;			/* ignore junk entries */

		appendStringInfo(buf, sep);
		sep = ", ";
		/*
		 * If the update expression is an array assignment, we mustn't
		 * put out "attname =" here; it will come out of the display
		 * of the ArrayRef node instead.
		 */
		if (! tleIsArrayAssign(tle))
			appendStringInfo(buf, "%s = ",
							 quote_identifier(tle->resdom->resname));
		get_tle_expr(tle, context);
	}

	/* Add the FROM clause if needed */
	get_from_clause(query, context);

	/* Finally add a WHERE clause if given */
	if (query->jointree->quals != NULL)
	{
		appendStringInfo(buf, " WHERE ");
		get_rule_expr(query->jointree->quals, context);
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
					 only_marker(rte),
					 quote_identifier(rte->relname));

	/* Add a WHERE clause if given */
	if (query->jointree->quals != NULL)
	{
		appendStringInfo(buf, " WHERE ");
		get_rule_expr(query->jointree->quals, context);
	}
}


/* ----------
 * get_utility_query_def			- Parse back a UTILITY parsetree
 * ----------
 */
static void
get_utility_query_def(Query *query, deparse_context *context)
{
	StringInfo	buf = context->buf;

	if (query->utilityStmt && IsA(query->utilityStmt, NotifyStmt))
	{
		NotifyStmt *stmt = (NotifyStmt *) query->utilityStmt;
		appendStringInfo(buf, "NOTIFY %s", quote_identifier(stmt->relname));
	}
	else
		elog(ERROR, "get_utility_query_def: unexpected statement type");
}


/*
 * Get the relation refname and attname for a (possibly nonlocal) Var.
 *
 * This is trickier than it ought to be because of the possibility of aliases
 * and limited scope of refnames.  We have to try to return the correct alias
 * with respect to the current namespace given by the context.
 */
static void
get_names_for_var(Var *var, deparse_context *context,
				  char **refname, char **attname)
{
	List	   *nslist = context->namespaces;
	int			sup = var->varlevelsup;
	deparse_namespace *dpns;
	RangeTblEntry *rte;

	/* Find appropriate nesting depth */
	while (sup-- > 0 && nslist != NIL)
		nslist = lnext(nslist);
	if (nslist == NIL)
		elog(ERROR, "get_names_for_var: bogus varlevelsup %d",
			 var->varlevelsup);
	dpns = (deparse_namespace *) lfirst(nslist);

	/* Scan namespace to see if we can find an alias for the var */
	if (find_alias_in_namespace((Node *) dpns->namespace, (Node *) var,
								dpns->rtable, var->varlevelsup,
								refname, attname))
		return;

	/*
	 * Otherwise, fall back on the rangetable entry.  This should happen
	 * only for uses of special RTEs like *NEW* and *OLD*, which won't
	 * get placed in our namespace.
	 */
	rte = rt_fetch(var->varno, dpns->rtable);
	*refname = rte->eref->relname;
	*attname = get_rte_attribute_name(rte, var->varattno);
}

/*
 * Check to see if a CASE expression matches a FULL JOIN's output expression.
 * If so, return the refname and alias it should be expressed as.
 */
static bool
get_alias_for_case(CaseExpr *caseexpr, deparse_context *context,
				   char **refname, char **attname)
{
	List	   *nslist;
	int			sup;

	/*
	 * This could be done more efficiently if we first groveled through the
	 * CASE to find varlevelsup values, but it's probably not worth the
	 * trouble.  All this code will go away someday anyway ...
	 */

	sup = 0;
	foreach(nslist, context->namespaces)
	{
		deparse_namespace *dpns = (deparse_namespace *) lfirst(nslist);

		if (find_alias_in_namespace((Node *) dpns->namespace,
									(Node *) caseexpr,
									dpns->rtable, sup,
									refname, attname))
			return true;
		sup++;
	}
	return false;
}

/*
 * Recursively scan a namespace (same representation as a jointree) to see
 * if we can find an alias for the given expression.  If so, return the
 * correct alias refname and attname.  The expression may be either a plain
 * Var or a CASE expression (which may be a FULL JOIN reference).
 */
static bool
find_alias_in_namespace(Node *nsnode, Node *expr,
						List *rangetable, int levelsup,
						char **refname, char **attname)
{
	if (nsnode == NULL)
		return false;
	if (IsA(nsnode, RangeTblRef))
	{
		if (IsA(expr, Var))
		{
			Var		   *var = (Var *) expr;
			int			rtindex = ((RangeTblRef *) nsnode)->rtindex;

			if (var->varno == rtindex && var->varlevelsup == levelsup)
			{
				RangeTblEntry *rte = rt_fetch(rtindex, rangetable);

				*refname = rte->eref->relname;
				*attname = get_rte_attribute_name(rte, var->varattno);
				return true;
			}
		}
	}
	else if (IsA(nsnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) nsnode;

		if (j->alias)
		{
			List	   *vlist;
			List	   *nlist;

			/*
			 * Does the expr match any of the output columns of the join?
			 *
			 * We can't just use equal() here, because the given expr may
			 * have nonzero levelsup, whereas the saved expression in the
			 * JoinExpr should have zero levelsup.
			 */
			nlist = j->colnames;
			foreach(vlist, j->colvars)
			{
				if (phony_equal(lfirst(vlist), expr, levelsup))
				{
					*refname = j->alias->relname;
					*attname = strVal(lfirst(nlist));
					return true;
				}
				nlist = lnext(nlist);
			}
			/*
			 * Tables within an aliased join are invisible from outside
			 * the join, according to the scope rules of SQL92 (the join
			 * is considered a subquery).  So, stop here.
			 */
			return false;
		}
		if (find_alias_in_namespace(j->larg, expr,
									rangetable, levelsup,
									refname, attname))
			return true;
		if (find_alias_in_namespace(j->rarg, expr,
									rangetable, levelsup,
									refname, attname))
			return true;
	}
	else if (IsA(nsnode, List))
	{
		List	   *l;

		foreach(l, (List *) nsnode)
		{
			if (find_alias_in_namespace(lfirst(l), expr,
										rangetable, levelsup,
										refname, attname))
				return true;
		}
	}
	else
		elog(ERROR, "find_alias_in_namespace: unexpected node type %d",
			 nodeTag(nsnode));
	return false;
}

/*
 * Check for equality of two expressions, with the proviso that all Vars in
 * expr1 should have varlevelsup = 0, while all Vars in expr2 should have
 * varlevelsup = levelsup.
 *
 * In reality we only need to support equality checks on Vars and the type
 * of CASE expression that is used for FULL JOIN outputs, so not all node
 * types need be handled here.
 *
 * Otherwise, this code is a straight ripoff from equalfuncs.c.
 */
static bool
phony_equal(Node *expr1, Node *expr2, int levelsup)
{
	if (expr1 == NULL || expr2 == NULL)
		return (expr1 == expr2);
	if (nodeTag(expr1) != nodeTag(expr2))
		return false;
	if (IsA(expr1, Var))
	{
		Var	   *a = (Var *) expr1;
		Var	   *b = (Var *) expr2;

		if (a->varno != b->varno)
			return false;
		if (a->varattno != b->varattno)
			return false;
		if (a->vartype != b->vartype)
			return false;
		if (a->vartypmod != b->vartypmod)
			return false;
		if (a->varlevelsup != 0 || b->varlevelsup != levelsup)
			return false;
		if (a->varnoold != b->varnoold)
			return false;
		if (a->varoattno != b->varoattno)
			return false;
		return true;
	}
	if (IsA(expr1, CaseExpr))
	{
		CaseExpr	   *a = (CaseExpr *) expr1;
		CaseExpr	   *b = (CaseExpr *) expr2;

		if (a->casetype != b->casetype)
			return false;
		if (!phony_equal(a->arg, b->arg, levelsup))
			return false;
		if (!phony_equal((Node *) a->args, (Node *) b->args, levelsup))
			return false;
		if (!phony_equal(a->defresult, b->defresult, levelsup))
			return false;
		return true;
	}
	if (IsA(expr1, CaseWhen))
	{
		CaseWhen	   *a = (CaseWhen *) expr1;
		CaseWhen	   *b = (CaseWhen *) expr2;

		if (!phony_equal(a->expr, b->expr, levelsup))
			return false;
		if (!phony_equal(a->result, b->result, levelsup))
			return false;
		return true;
	}
	if (IsA(expr1, Expr))
	{
		Expr	   *a = (Expr *) expr1;
		Expr	   *b = (Expr *) expr2;

		if (a->opType != b->opType)
			return false;
		if (!phony_equal(a->oper, b->oper, levelsup))
			return false;
		if (!phony_equal((Node *) a->args, (Node *) b->args, levelsup))
			return false;
		return true;
	}
	if (IsA(expr1, Func))
	{
		Func	   *a = (Func *) expr1;
		Func	   *b = (Func *) expr2;

		if (a->funcid != b->funcid)
			return false;
		if (a->functype != b->functype)
			return false;
		return true;
	}
	if (IsA(expr1, List))
	{
		List	   *la = (List *) expr1;
		List	   *lb = (List *) expr2;
		List	   *l;

		if (length(la) != length(lb))
			return false;
		foreach(l, la)
		{
			if (!phony_equal(lfirst(l), lfirst(lb), levelsup))
				return false;
			lb = lnext(lb);
		}
		return true;
	}
	/* If we get here, there was something weird in a JOIN's colvars list */
	elog(ERROR, "phony_equal: unexpected node type %d", nodeTag(expr1));
	return false;
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
				char	   *refname;
				char	   *attname;

				get_names_for_var(var, context, &refname, &attname);
				if (context->varprefix)
				{
					if (strcmp(refname, "*NEW*") == 0)
						appendStringInfo(buf, "new.");
					else if (strcmp(refname, "*OLD*") == 0)
						appendStringInfo(buf, "old.");
					else
						appendStringInfo(buf, "%s.",
										 quote_identifier(refname));
				}
				appendStringInfo(buf, "%s", quote_identifier(attname));
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
						appendStringInfoChar(buf, '(');
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

							tp = SearchSysCache(OPEROID,
												ObjectIdGetDatum(opno),
												0, 0, 0);
							if (!HeapTupleIsValid(tp))
								elog(ERROR, "cache lookup for operator %u failed", opno);
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
							ReleaseSysCache(tp);
						}
						appendStringInfoChar(buf, ')');
						break;

					case OR_EXPR:
						appendStringInfoChar(buf, '(');
						get_rule_expr((Node *) lfirst(args), context);
						while ((args = lnext(args)) != NIL)
						{
							appendStringInfo(buf, " OR ");
							get_rule_expr((Node *) lfirst(args), context);
						}
						appendStringInfoChar(buf, ')');
						break;

					case AND_EXPR:
						appendStringInfoChar(buf, '(');
						get_rule_expr((Node *) lfirst(args), context);
						while ((args = lnext(args)) != NIL)
						{
							appendStringInfo(buf, " AND ");
							get_rule_expr((Node *) lfirst(args), context);
						}
						appendStringInfoChar(buf, ')');
						break;

					case NOT_EXPR:
						appendStringInfo(buf, "(NOT ");
						get_rule_expr((Node *) lfirst(args), context);
						appendStringInfoChar(buf, ')');
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

				appendStringInfo(buf, "%s(%s",
								 quote_identifier(aggref->aggname),
								 aggref->aggdistinct ? "DISTINCT " : "");
				if (aggref->aggstar)
					appendStringInfo(buf, "*");
				else
					get_rule_expr(aggref->target, context);
				appendStringInfoChar(buf, ')');
			}
			break;

		case T_Iter:
			get_rule_expr(((Iter *) node)->iterexpr, context);
			break;

		case T_ArrayRef:
			{
				ArrayRef   *aref = (ArrayRef *) node;
				bool		savevarprefix = context->varprefix;
				List	   *lowlist;
				List	   *uplist;

				/*
				 * If we are doing UPDATE array[n] = expr, we need to
				 * suppress any prefix on the array name.  Currently,
				 * that is the only context in which we will see a non-null
				 * refassgnexpr --- but someday a smarter test may be needed.
				 */
				if (aref->refassgnexpr)
					context->varprefix = false;
				get_rule_expr(aref->refexpr, context);
				context->varprefix = savevarprefix;
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
				if (aref->refassgnexpr)
				{
					appendStringInfo(buf, " = ");
					get_rule_expr(aref->refassgnexpr, context);
				}
			}
			break;

		case T_FieldSelect:
			{
				FieldSelect *fselect = (FieldSelect *) node;
				HeapTuple	typetup;
				Form_pg_type typeStruct;
				Oid			typrelid;
				char	   *fieldname;

				/* we do NOT parenthesize the arg expression, for now */
				get_rule_expr(fselect->arg, context);
				typetup = SearchSysCache(TYPEOID,
										 ObjectIdGetDatum(exprType(fselect->arg)),
										 0, 0, 0);
				if (!HeapTupleIsValid(typetup))
					elog(ERROR, "cache lookup of type %u failed",
						 exprType(fselect->arg));
				typeStruct = (Form_pg_type) GETSTRUCT(typetup);
				typrelid = typeStruct->typrelid;
				if (!OidIsValid(typrelid))
					elog(ERROR, "Argument type %s of FieldSelect is not a tuple type",
						 NameStr(typeStruct->typname));
				fieldname = get_relid_attribute_name(typrelid,
													 fselect->fieldnum);
				appendStringInfo(buf, ".%s", quote_identifier(fieldname));
				ReleaseSysCache(typetup);
			}
			break;

		case T_RelabelType:
			{
				RelabelType *relabel = (RelabelType *) node;
				HeapTuple	typetup;
				Form_pg_type typeStruct;
				char	   *extval;

				appendStringInfoChar(buf, '(');
				get_rule_expr(relabel->arg, context);
				typetup = SearchSysCache(TYPEOID,
								   ObjectIdGetDatum(relabel->resulttype),
										 0, 0, 0);
				if (!HeapTupleIsValid(typetup))
					elog(ERROR, "cache lookup of type %u failed",
						 relabel->resulttype);
				typeStruct = (Form_pg_type) GETSTRUCT(typetup);
				extval = pstrdup(NameStr(typeStruct->typname));
				appendStringInfo(buf, ")::%s", quote_identifier(extval));
				pfree(extval);
				ReleaseSysCache(typetup);
			}
			break;

		case T_CaseExpr:
			{
				CaseExpr   *caseexpr = (CaseExpr *) node;
				List	   *temp;
				char	   *refname;
				char	   *attname;

				/* Hack for providing aliases for FULL JOIN outputs */
				if (get_alias_for_case(caseexpr, context,
									   &refname, &attname))
				{
					if (context->varprefix)
						appendStringInfo(buf, "%s.",
										 quote_identifier(refname));
					appendStringInfo(buf, "%s", quote_identifier(attname));
					break;
				}

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
	Func	   *func = (Func *) (expr->oper);
	Oid			funcoid = func->funcid;
	HeapTuple	proctup;
	Form_pg_proc procStruct;
	char	   *proname;
	int32		coercedTypmod;
	List	   *l;
	char	   *sep;

	/*
	 * nullvalue() and nonnullvalue() should get turned into special
	 * syntax
	 */
	if (funcoid == F_NULLVALUE)
	{
		appendStringInfoChar(buf, '(');
		get_rule_expr((Node *) lfirst(expr->args), context);
		appendStringInfo(buf, " ISNULL)");
		return;
	}
	if (funcoid == F_NONNULLVALUE)
	{
		appendStringInfoChar(buf, '(');
		get_rule_expr((Node *) lfirst(expr->args), context);
		appendStringInfo(buf, " NOTNULL)");
		return;
	}

	/*
	 * Get the functions pg_proc tuple
	 */
	proctup = SearchSysCache(PROCOID,
							 ObjectIdGetDatum(funcoid),
							 0, 0, 0);
	if (!HeapTupleIsValid(proctup))
		elog(ERROR, "cache lookup for proc %u failed", funcoid);

	procStruct = (Form_pg_proc) GETSTRUCT(proctup);
	proname = NameStr(procStruct->proname);

	/*
	 * Check to see if function is a length-coercion function for some
	 * datatype.  If so, display the operation as a type cast.
	 */
	if (exprIsLengthCoercion((Node *) expr, &coercedTypmod))
	{
		Node	   *arg = lfirst(expr->args);

		/*
		 * Strip off any RelabelType on the input, so we don't print
		 * redundancies like x::bpchar::char(8).
		 *
		 * XXX Are there any cases where this is a bad idea?
		 */
		if (IsA(arg, RelabelType))
			arg = ((RelabelType *) arg)->arg;
		appendStringInfoChar(buf, '(');
		get_rule_expr(arg, context);
		appendStringInfo(buf, ")::");

		/*
		 * Show typename with appropriate length decoration. Note that
		 * since exprIsLengthCoercion succeeded, the function name is the
		 * same as its output type name.
		 */
		if (strcmp(proname, "bpchar") == 0)
		{
			if (coercedTypmod > (int32) VARHDRSZ)
				appendStringInfo(buf, "char(%d)", coercedTypmod - VARHDRSZ);
			else
				appendStringInfo(buf, "char");
		}
		else if (strcmp(proname, "varchar") == 0)
		{
			if (coercedTypmod > (int32) VARHDRSZ)
				appendStringInfo(buf, "varchar(%d)", coercedTypmod - VARHDRSZ);
			else
				appendStringInfo(buf, "varchar");
		}
		else if (strcmp(proname, "numeric") == 0)
		{
			if (coercedTypmod >= (int32) VARHDRSZ)
				appendStringInfo(buf, "numeric(%d,%d)",
							 ((coercedTypmod - VARHDRSZ) >> 16) & 0xffff,
								 (coercedTypmod - VARHDRSZ) & 0xffff);
			else
				appendStringInfo(buf, "numeric");
		}
		else
			appendStringInfo(buf, "%s", quote_identifier(proname));

		ReleaseSysCache(proctup);
		return;
	}

	/*
	 * Normal function: display as proname(args)
	 */
	appendStringInfo(buf, "%s(", quote_identifier(proname));
	sep = "";
	foreach(l, expr->args)
	{
		appendStringInfo(buf, sep);
		sep = ", ";
		get_rule_expr((Node *) lfirst(l), context);
	}
	appendStringInfoChar(buf, ')');

	ReleaseSysCache(proctup);
}


/* ----------
 * get_tle_expr
 *
 *		In an INSERT or UPDATE targetlist item, the parser may have inserted
 *		a length-coercion function call to coerce the value to the right
 *		length for the target column.  We want to suppress the output of
 *		that function call, otherwise dump/reload/dump... would blow up the
 *		expression by adding more and more layers of length-coercion calls.
 *
 * As of 7.0, this hack is no longer absolutely essential, because the parser
 * is now smart enough not to add a redundant length coercion function call.
 * But we still suppress the function call just for neatness of displayed
 * rules.
 *
 * Note that this hack must NOT be applied to SELECT targetlist items;
 * any length coercion appearing there is something the user actually wrote.
 * ----------
 */
static void
get_tle_expr(TargetEntry *tle, deparse_context *context)
{
	Expr	   *expr = (Expr *) (tle->expr);
	int32		coercedTypmod;

	/*
	 * If top level is a length coercion to the correct length, suppress
	 * it; else dump the expression normally.
	 */
	if (tle->resdom->restypmod >= 0 &&
		exprIsLengthCoercion((Node *) expr, &coercedTypmod) &&
		coercedTypmod == tle->resdom->restypmod)
		get_rule_expr((Node *) lfirst(expr->args), context);
	else
		get_rule_expr(tle->expr, context);
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
	char	   *extval;
	char	   *valptr;

	typetup = SearchSysCache(TYPEOID,
							 ObjectIdGetDatum(constval->consttype),
							 0, 0, 0);
	if (!HeapTupleIsValid(typetup))
		elog(ERROR, "cache lookup of type %u failed", constval->consttype);

	typeStruct = (Form_pg_type) GETSTRUCT(typetup);

	if (constval->constisnull)
	{

		/*
		 * Always label the type of a NULL constant.  This not only
		 * prevents misdecisions about the type, but it ensures that our
		 * output is a valid b_expr.
		 */
		extval = pstrdup(NameStr(typeStruct->typname));
		appendStringInfo(buf, "NULL::%s", quote_identifier(extval));
		pfree(extval);
		ReleaseSysCache(typetup);
		return;
	}

	extval = DatumGetCString(OidFunctionCall3(typeStruct->typoutput,
							 constval->constvalue,
							 ObjectIdGetDatum(typeStruct->typelem),
							 Int32GetDatum(-1)));

	switch (constval->consttype)
	{
		case INT2OID:
		case INT4OID:
		case OIDOID:			/* int types */
		case FLOAT4OID:
		case FLOAT8OID: /* float types */
			/* These types are printed without quotes */
			appendStringInfo(buf, extval);
			break;
		default:

			/*
			 * We must quote any funny characters in the constant's
			 * representation. XXX Any MULTIBYTE considerations here?
			 */
			appendStringInfoChar(buf, '\'');
			for (valptr = extval; *valptr; valptr++)
			{
				char		ch = *valptr;

				if (ch == '\'' || ch == '\\')
				{
					appendStringInfoChar(buf, '\\');
					appendStringInfoChar(buf, ch);
				}
				else if (((unsigned char) ch) < ((unsigned char) ' '))
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

	ReleaseSysCache(typetup);
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
	List	   *l;
	char	   *sep;
	Oper	   *oper;
	bool		need_paren;

	appendStringInfoChar(buf, '(');

	if (sublink->lefthand != NIL)
	{
		need_paren = (length(sublink->lefthand) > 1);
		if (need_paren)
			appendStringInfoChar(buf, '(');

		sep = "";
		foreach(l, sublink->lefthand)
		{
			appendStringInfo(buf, sep);
			sep = ", ";
			get_rule_expr((Node *) lfirst(l), context);
		}

		if (need_paren)
			appendStringInfo(buf, ") ");
		else
			appendStringInfoChar(buf, ' ');
	}

	need_paren = true;

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

		case MULTIEXPR_SUBLINK:
			oper = (Oper *) lfirst(sublink->oper);
			appendStringInfo(buf, "%s ", get_opname(oper->opno));
			break;

		case EXPR_SUBLINK:
			need_paren = false;
			break;

		default:
			elog(ERROR, "get_sublink_expr: unsupported sublink type %d",
				 sublink->subLinkType);
			break;
	}

	if (need_paren)
		appendStringInfoChar(buf, '(');

	get_query_def(query, buf, context->namespaces);

	if (need_paren)
		appendStringInfo(buf, "))");
	else
		appendStringInfoChar(buf, ')');
}


/* ----------
 * get_from_clause			- Parse back a FROM clause
 * ----------
 */
static void
get_from_clause(Query *query, deparse_context *context)
{
	StringInfo	buf = context->buf;
	char	   *sep;
	List	   *l;

	/*
	 * We use the query's jointree as a guide to what to print.  However,
	 * we must ignore auto-added RTEs that are marked not inFromCl.
	 * (These can only appear at the top level of the jointree, so it's
	 * sufficient to check here.)
	 * Also ignore the rule pseudo-RTEs for NEW and OLD.
	 */
	sep = " FROM ";

	foreach(l, query->jointree->fromlist)
	{
		Node   *jtnode = (Node *) lfirst(l);

		if (IsA(jtnode, RangeTblRef))
		{
			int			varno = ((RangeTblRef *) jtnode)->rtindex;
			RangeTblEntry *rte = rt_fetch(varno, query->rtable);

			if (!rte->inFromCl)
				continue;
			if (strcmp(rte->eref->relname, "*NEW*") == 0)
				continue;
			if (strcmp(rte->eref->relname, "*OLD*") == 0)
				continue;
		}

		appendStringInfo(buf, sep);
		get_from_clause_item(jtnode, query, context);
		sep = ", ";
	}
}

static void
get_from_clause_item(Node *jtnode, Query *query, deparse_context *context)
{
	StringInfo	buf = context->buf;
	deparse_namespace *dpns;
	List	   *sv_namespace;

	/*
	 * FROM-clause items have limited visibility of query's namespace.
	 * Save and restore the outer namespace setting while we munge it.
	 */
	dpns = (deparse_namespace *) lfirst(context->namespaces);
	sv_namespace = dpns->namespace;
	dpns->namespace = NIL;

	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;
		RangeTblEntry *rte = rt_fetch(varno, query->rtable);

		if (rte->relname)
		{
			/* Normal relation RTE */
			appendStringInfo(buf, "%s%s",
							 only_marker(rte),
							 quote_identifier(rte->relname));
		}
		else
		{
			/* Subquery RTE */
			Assert(rte->subquery != NULL);
			appendStringInfoChar(buf, '(');
			get_query_def(rte->subquery, buf, context->namespaces);
			appendStringInfoChar(buf, ')');
		}
		if (rte->alias != NULL)
		{
			appendStringInfo(buf, " %s",
							 quote_identifier(rte->alias->relname));
			if (rte->alias->attrs != NIL)
			{
				List	   *col;

				appendStringInfo(buf, "(");
				foreach(col, rte->alias->attrs)
				{
					if (col != rte->alias->attrs)
						appendStringInfo(buf, ", ");
					appendStringInfo(buf, "%s",
									 quote_identifier(strVal(lfirst(col))));
				}
				appendStringInfoChar(buf, ')');
			}
		}
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		appendStringInfoChar(buf, '(');
		get_from_clause_item(j->larg, query, context);
		if (j->isNatural)
			appendStringInfo(buf, " NATURAL");
		switch (j->jointype)
		{
			case JOIN_INNER:
				if (j->quals)
					appendStringInfo(buf, " JOIN ");
				else
					appendStringInfo(buf, " CROSS JOIN ");
				break;
			case JOIN_LEFT:
				appendStringInfo(buf, " LEFT JOIN ");
				break;
			case JOIN_FULL:
				appendStringInfo(buf, " FULL JOIN ");
				break;
			case JOIN_RIGHT:
				appendStringInfo(buf, " RIGHT JOIN ");
				break;
			case JOIN_UNION:
				appendStringInfo(buf, " UNION JOIN ");
				break;
			default:
				elog(ERROR, "get_from_clause_item: unknown join type %d",
					 (int) j->jointype);
		}
		get_from_clause_item(j->rarg, query, context);
		if (! j->isNatural)
		{
			if (j->using)
			{
				List	   *col;

				appendStringInfo(buf, " USING (");
				foreach(col, j->using)
				{
					if (col != j->using)
						appendStringInfo(buf, ", ");
					appendStringInfo(buf, "%s",
									 quote_identifier(strVal(lfirst(col))));
				}
				appendStringInfoChar(buf, ')');
			}
			else if (j->quals)
			{
				dpns->namespace = makeList2(j->larg, j->rarg);
				appendStringInfo(buf, " ON (");
				get_rule_expr(j->quals, context);
				appendStringInfoChar(buf, ')');
			}
		}
		appendStringInfoChar(buf, ')');
		/* Yes, it's correct to put alias after the right paren ... */
		if (j->alias != NULL)
		{
			appendStringInfo(buf, " %s",
							 quote_identifier(j->alias->relname));
			if (j->alias->attrs != NIL)
			{
				List	   *col;

				appendStringInfo(buf, "(");
				foreach(col, j->alias->attrs)
				{
					if (col != j->alias->attrs)
						appendStringInfo(buf, ", ");
					appendStringInfo(buf, "%s",
									 quote_identifier(strVal(lfirst(col))));
				}
				appendStringInfoChar(buf, ')');
			}
		}
	}
	else
		elog(ERROR, "get_from_clause_item: unexpected node type %d",
			 nodeTag(jtnode));

	dpns->namespace = sv_namespace;
}


/* ----------
 * tleIsArrayAssign			- check for array assignment
 * ----------
 */
static bool
tleIsArrayAssign(TargetEntry *tle)
{
	ArrayRef   *aref;

	if (tle->expr == NULL || !IsA(tle->expr, ArrayRef))
		return false;
	aref = (ArrayRef *) tle->expr;
	if (aref->refassgnexpr == NULL)
		return false;
	/*
	 * Currently, it should only be possible to see non-null refassgnexpr
	 * if we are indeed looking at an "UPDATE array[n] = expr" situation.
	 * So aref->refexpr ought to match the tle's target.
	 */
	if (aref->refexpr == NULL || !IsA(aref->refexpr, Var) ||
		((Var *) aref->refexpr)->varattno != tle->resdom->resno)
		elog(NOTICE, "tleIsArrayAssign: I'm confused ...");
	return true;
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
	 * contains only lowercase letters, digits, and underscores, *and* is
	 * not any SQL keyword.  Otherwise, supply quotes.
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

		for (ptr = ident + 1; *ptr; ptr++)
		{
			char		ch = *ptr;

			safe = ((ch >= 'a' && ch <= 'z') ||
					(ch >= '0' && ch <= '9') ||
					(ch == '_'));
			if (!safe)
				break;
		}
	}

	if (safe)
	{

		/*
		 * Check for keyword.  This test is overly strong, since many of
		 * the "keywords" known to the parser are usable as column names,
		 * but the parser doesn't provide any easy way to test for whether
		 * an identifier is safe or not... so be safe not sorry.
		 *
		 * Note: ScanKeywordLookup() does case-insensitive comparison,
		 * but that's fine, since we already know we have all-lower-case.
		 */
		if (ScanKeywordLookup(ident) != NULL)
			safe = false;
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
	char	   *result;

	classtup = SearchSysCache(RELOID,
							  ObjectIdGetDatum(relid),
							  0, 0, 0);
	if (!HeapTupleIsValid(classtup))
		elog(ERROR, "cache lookup of relation %u failed", relid);

	classStruct = (Form_pg_class) GETSTRUCT(classtup);
	result = pstrdup(NameStr(classStruct->relname));
	ReleaseSysCache(classtup);
	return result;
}


/* ----------
 * get_relid_attribute_name
 *		Get an attribute name by its relations Oid and its attnum
 *
 * Same as underlying syscache routine get_attname(), except that error
 * is handled by elog() instead of returning NULL.
 * ----------
 */
static char *
get_relid_attribute_name(Oid relid, AttrNumber attnum)
{
	char	   *attname;

	attname = get_attname(relid, attnum);
	if (attname == NULL)
		elog(ERROR, "cache lookup of attribute %d in relation %u failed",
			 attnum, relid);
	return attname;
}
