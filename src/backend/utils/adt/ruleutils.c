/**********************************************************************
 * get_ruledef.c	- Function to get a rules definition text
 *			  out of it's tuple
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/ruleutils.c,v 1.4 1998/10/02 16:27:51 momjian Exp $
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

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include "executor/spi.h"
#include "commands/trigger.h"
#include "utils/elog.h"
#include "utils/builtins.h"
#include "nodes/nodes.h"
#include "optimizer/clauses.h"
#include "utils/syscache.h"
#include "utils/lsyscache.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "catalog/pg_shadow.h"
#include "catalog/pg_index.h"
#include "catalog/pg_opclass.h"
#include "fmgr.h"


/* ----------
 * Local data types
 * ----------
 */
typedef struct QryHier {
	struct QryHier		*parent;
	Query			*query;
} QryHier;


/* ----------
 * Global data
 * ----------
 */
static char *rulename;
static void *plan_getrule = NULL;
static char *query_getrule = "SELECT * FROM pg_rewrite WHERE rulename = $1";
static void *plan_getview = NULL;
static char *query_getview = "SELECT * FROM pg_rewrite WHERE rulename = $1 or rulename = $2";
static void *plan_getam = NULL;
static char *query_getam = "SELECT * FROM pg_am WHERE oid = $1";
static void *plan_getopclass = NULL;
static char *query_getopclass = "SELECT * FROM pg_opclass WHERE oid = $1";


/* ----------
 * Global functions
 * ----------
 */
text	   *pg_get_ruledef(NameData *rname);
text	   *pg_get_viewdef(NameData *rname);
text       *pg_get_indexdef(Oid indexrelid);
NameData   *pg_get_userbyid(int4 uid);


/* ----------
 * Local functions
 * ----------
 */
static char *make_ruledef(HeapTuple ruletup, TupleDesc rulettc);
static char *make_viewdef(HeapTuple ruletup, TupleDesc rulettc);
static char *get_query_def(Query *query, QryHier *parentqh);
static char *get_select_query_def(Query *query, QryHier *qh);
static char *get_insert_query_def(Query *query, QryHier *qh);
static char *get_update_query_def(Query *query, QryHier *qh);
static char *get_delete_query_def(Query *query, QryHier *qh);
static char *get_rule_expr(QryHier *qh, int rt_index, Node *node, bool varprefix);
static char *get_func_expr(QryHier *qh, int rt_index, Expr *expr, bool varprefix);
static char *get_tle_expr(QryHier *qh, int rt_index, TargetEntry *tle, bool varprefix);
static char *get_const_expr(Const *constval);
static char *get_sublink_expr(QryHier *qh, int rt_index, Node *node, bool varprefix);
static char *get_relation_name(Oid relid);
static char *get_attribute_name(Oid relid, int2 attnum);
static bool check_if_rte_used(int rt_index, Node *node, int sup);


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
	char	   *tmp;
	int			len;

	/* ----------
	 * We need the rules name somewhere deep down
	 * ----------
	 */
	rulename = nameout(rname);

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
	tmp = make_ruledef(ruletup, rulettc);
	len = strlen(tmp) + VARHDRSZ;
	ruledef = SPI_palloc(len);
	VARSIZE(ruledef) = len;
	memcpy(VARDATA(ruledef), tmp, len - VARHDRSZ);

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
	char	   *tmp;
	int			len;
	char		name1[NAMEDATALEN + 5];
	char		name2[NAMEDATALEN + 5];

	/* ----------
	 * We need the rules name somewhere deep down
	 * ----------
	 */
	rulename = nameout(rname);

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
	if (SPI_processed != 1)
		tmp = "Not a view";
	else
	{
		/* ----------
		 * Get the rules definition and put it into executors memory
		 * ----------
		 */
		ruletup = SPI_tuptable->vals[0];
		rulettc = SPI_tuptable->tupdesc;
		tmp = make_viewdef(ruletup, rulettc);
	}
	len = strlen(tmp) + VARHDRSZ;
	ruledef = SPI_palloc(len);
	VARSIZE(ruledef) = len;
	memcpy(VARDATA(ruledef), tmp, len - VARHDRSZ);

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
 * get_viewdef			- Mainly the same thing, but we
 *				  only return the SELECT part of a view
 * ----------
 */
text       *
pg_get_indexdef(Oid indexrelid)
{
	text		*indexdef;
	HeapTuple	ht_idx;
	HeapTuple	ht_idxrel;
	HeapTuple	ht_indrel;
	HeapTuple	spi_tup;
	TupleDesc	spi_ttc;
	int		spi_fno;
	Form_pg_index	idxrec;
	Form_pg_class	idxrelrec;
	Form_pg_class	indrelrec;
	Datum		spi_args[1];
	char		spi_nulls[2];
	int		spirc;
	int		len;
	int		keyno;
	char		buf[8192];
	char		keybuf[8192];
	char		*sep;

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
		elog(ERROR, "syscache lookup for index %d failed", indexrelid);
	idxrec = (Form_pg_index)GETSTRUCT(ht_idx);

	/* ----------
	 * Fetch the pg_class tuple of the index relation
	 * ----------
	 */
	ht_idxrel = SearchSysCacheTuple(RELOID,
			ObjectIdGetDatum(idxrec->indexrelid), 0, 0, 0);
	if (!HeapTupleIsValid(ht_idxrel))
		elog(ERROR, "syscache lookup for relid %d failed", idxrec->indexrelid);
	idxrelrec = (Form_pg_class)GETSTRUCT(ht_idxrel);

	/* ----------
	 * Fetch the pg_class tuple of the indexed relation
	 * ----------
	 */
	ht_indrel = SearchSysCacheTuple(RELOID,
			ObjectIdGetDatum(idxrec->indrelid), 0, 0, 0);
	if (!HeapTupleIsValid(ht_indrel))
		elog(ERROR, "syscache lookup for relid %d failed", idxrec->indrelid);
	indrelrec = (Form_pg_class)GETSTRUCT(ht_indrel);

	/* ----------
	 * Get the am name for the index relation
	 * ----------
	 */
	spi_args[0] = ObjectIdGetDatum(idxrelrec->relam);
	spi_nulls[0] = ' ';
	spi_nulls[1] = '\0';
	spirc = SPI_execp(plan_getam, spi_args, spi_nulls, 1);
	if (spirc != SPI_OK_SELECT)
		elog(ERROR, "failed to get pg_am tuple for index %s", nameout(&(idxrelrec->relname)));
	if (SPI_processed != 1)
		elog(ERROR, "failed to get pg_am tuple for index %s", nameout(&(idxrelrec->relname)));
	spi_tup = SPI_tuptable->vals[0];
	spi_ttc = SPI_tuptable->tupdesc;
	spi_fno = SPI_fnumber(spi_ttc, "amname");

	/* ----------
	 * Start the index definition
	 * ----------
	 */
	sprintf(buf, "CREATE %sINDEX %s ON %s USING %s (",
		idxrec->indisunique ? "UNIQUE " : "",
		nameout(&(idxrelrec->relname)),
		nameout(&(indrelrec->relname)),
		SPI_getvalue(spi_tup, spi_ttc, spi_fno));
			
	/* ----------
	 * Collect the indexed attributes
	 * ----------
	 */
	sep = "";
	keybuf[0] = '\0';
	for (keyno = 0; keyno < INDEX_MAX_KEYS; keyno++)
	{
		if (idxrec->indkey[keyno] == InvalidAttrNumber)
			break;

		strcat(keybuf, sep);
		sep = ", ";

		/* ----------
		 * Add the indexed field name
		 * ----------
		 */
		if (idxrec->indkey[keyno] == ObjectIdAttributeNumber - 1)
			strcat(keybuf, "oid");
		else
			strcat(keybuf, get_attribute_name(idxrec->indrelid,
						idxrec->indkey[keyno]));

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
				elog(ERROR, "failed to get pg_opclass tuple %d", idxrec->indclass[keyno]);
			if (SPI_processed != 1)
				elog(ERROR, "failed to get pg_opclass tuple %d", idxrec->indclass[keyno]);
			spi_tup = SPI_tuptable->vals[0];
			spi_ttc = SPI_tuptable->tupdesc;
			spi_fno = SPI_fnumber(spi_ttc, "opcname");
			strcat(keybuf, " ");
			strcat(keybuf, SPI_getvalue(spi_tup, spi_ttc, spi_fno));
		}
	}

	/* ----------
	 * For functional index say 'func (attrs) opclass'
	 * ----------
	 */
	if (idxrec->indproc != InvalidOid)
	{
		HeapTuple	proctup;
		Form_pg_proc	procStruct;

		proctup = SearchSysCacheTuple(PROOID,
								ObjectIdGetDatum(idxrec->indproc), 0, 0, 0);
		if (!HeapTupleIsValid(proctup))
			elog(ERROR, "cache lookup for proc %d failed", idxrec->indproc);

		procStruct = (Form_pg_proc) GETSTRUCT(proctup);
		strcat(buf, nameout(&(procStruct->proname)));
		strcat(buf, " (");
		strcat(buf, keybuf);
		strcat(buf, ") ");

		spi_args[0] = ObjectIdGetDatum(idxrec->indclass[0]);
		spi_nulls[0] = ' ';
		spi_nulls[1] = '\0';
		spirc = SPI_execp(plan_getopclass, spi_args, spi_nulls, 1);
		if (spirc != SPI_OK_SELECT)
			elog(ERROR, "failed to get pg_opclass tuple %d", idxrec->indclass[0]);
		if (SPI_processed != 1)
			elog(ERROR, "failed to get pg_opclass tuple %d", idxrec->indclass[0]);
		spi_tup = SPI_tuptable->vals[0];
		spi_ttc = SPI_tuptable->tupdesc;
		spi_fno = SPI_fnumber(spi_ttc, "opcname");
		strcat(buf, SPI_getvalue(spi_tup, spi_ttc, spi_fno));
	}
	else
	/* ----------
	 * For the others say 'attr opclass [, ...]'
	 * ----------
	 */
	{
		strcat(buf, keybuf);
	}

	/* ----------
	 * Finish
	 * ----------
	 */
	strcat(buf, ")");

	/* ----------
	 * Create the result in upper executor memory
	 * ----------
	 */
	len = strlen(buf) + VARHDRSZ;
	indexdef = SPI_palloc(len);
	VARSIZE(indexdef) = len;
	memcpy(VARDATA(indexdef), buf, len - VARHDRSZ);

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
pg_get_userbyid(int4 uid)
{
	HeapTuple	usertup;
	Form_pg_shadow	user_rec;
	NameData	*result;

	/* ----------
	 * Allocate space for the result
	 * ----------
	 */
	result = (NameData *) palloc(NAMEDATALEN);
	memset(result->data, 0, NAMEDATALEN);

	/* ----------
	 * Get the pg_shadow entry and print the result
	 * ----------
	 */
	usertup = SearchSysCacheTuple(USESYSID,
			ObjectIdGetDatum(uid), 0, 0, 0);
	if (HeapTupleIsValid(usertup))
	{
		user_rec = (Form_pg_shadow)GETSTRUCT(usertup);
		StrNCpy(result->data, (&(user_rec->usename))->data, NAMEDATALEN);
	}
	else
	{
		sprintf((char *)result, "unknown (UID=%d)", uid);
	}

	return result;
}


/* ----------
 * make_ruledef			- reconstruct the CREATE RULE command
 *				  for a given pg_rewrite tuple
 * ----------
 */
static char *
make_ruledef(HeapTuple ruletup, TupleDesc rulettc)
{
	char	   *buf;
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
	 * Allocate space for the returned rule definition text
	 * ----------
	 */
	buf = palloc(8192);

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
	strcpy(buf, "CREATE RULE ");

	/* The rule name */
	strcat(buf, rulename);
	strcat(buf, " AS ON ");

	/* The event the rule is fired for */
	switch (ev_type)
	{
		case '1':
			strcat(buf, "SELECT TO ");
			break;

		case '2':
			strcat(buf, "UPDATE TO ");
			break;

		case '3':
			strcat(buf, "INSERT TO ");
			break;

		case '4':
			strcat(buf, "DELETE TO ");
			break;

		default:
			elog(ERROR, "get_ruledef: rule %s has unsupported event type %d",
				 rulename, ev_type);
			break;
	}

	/* The relation the rule is fired on */
	strcat(buf, get_relation_name(ev_class));
	if (ev_attr > 0)
	{
		strcat(buf, ".");
		strcat(buf, get_attribute_name(ev_class, ev_attr));
	}

	/* If the rule has an event qualification, add it */
	if (ev_qual == NULL)
		ev_qual = "";
	if (strlen(ev_qual) > 0)
	{
		Node	   *qual;
		Query	   *query;
		QryHier    qh;

		qual = stringToNode(ev_qual);
		query = (Query *) lfirst(actions);
		qh.parent = NULL;
		qh.query  = query;

		strcat(buf, " WHERE ");
		strcat(buf, get_rule_expr(&qh, 0, qual, TRUE));
	}

	strcat(buf, " DO ");

	/* The INSTEAD keyword (if so) */
	if (is_instead)
		strcat(buf, "INSTEAD ");

	/* Finally the rules actions */
	if (length(actions) > 1)
	{
		List	   *action;
		Query	   *query;

		strcat(buf, "(");
		foreach(action, actions)
		{
			query = (Query *) lfirst(action);
			strcat(buf, get_query_def(query, NULL));
			strcat(buf, "; ");
		}
		strcat(buf, ");");
	}
	else
	{
		if (length(actions) == 0)
		{
			strcat(buf, "NOTHING;");
		}
		else
		{
			Query	   *query;

			query = (Query *) lfirst(actions);
			strcat(buf, get_query_def(query, NULL));
			strcat(buf, ";");
		}
	}

	/* ----------
	 * That's it
	 * ----------
	 */
	return buf;
}


/* ----------
 * make_viewdef			- reconstruct the SELECT part of a
 *				  view rewrite rule
 * ----------
 */
static char *
make_viewdef(HeapTuple ruletup, TupleDesc rulettc)
{
	char		buf[8192];
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
		return "Not a view";

	query = (Query *) lfirst(actions);

	if (ev_type != '1' || ev_attr >= 0 || !is_instead || strcmp(ev_qual, ""))
		return "Not a view";

	strcpy(buf, get_query_def(query, NULL));
	strcat(buf, ";");

	/* ----------
	 * That's it
	 * ----------
	 */
	return pstrdup(buf);
}


/* ----------
 * get_query_def			- Parse back one action from
 *					  the parsetree in the actions
 *					  list
 * ----------
 */
static char *
get_query_def(Query *query, QryHier *parentqh)
{
	QryHier		qh;

	qh.parent = parentqh;
	qh.query  = query;

	switch (query->commandType)
	{
			case CMD_SELECT:
			return get_select_query_def(query, &qh);
			break;

		case CMD_UPDATE:
			return get_update_query_def(query, &qh);
			break;

		case CMD_INSERT:
			return get_insert_query_def(query, &qh);
			break;

		case CMD_DELETE:
			return get_delete_query_def(query, &qh);
			break;

		case CMD_NOTHING:
			return "NOTHING";
			break;

		default:
			elog(ERROR, "get_ruledef of %s: query command type %d not implemented yet",
				 rulename, query->commandType);
			break;
	}

	return NULL;
}


/* ----------
 * get_select_query_def			- Parse back a SELECT parsetree
 * ----------
 */
static char *
get_select_query_def(Query *query, QryHier *qh)
{
	char		buf[8192];
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
	 * First we need need to know which and how many of the
	 * range table entries in the query are used in the target list
	 * or queries qualification
	 * ----------
	 */
	rt_length = length(query->rtable);
	rt_used = palloc(sizeof(bool) * rt_length);
	for (i = 0; i < rt_length; i++)
	{
		if (check_if_rte_used(i + 1, (Node *) (query->targetList), 0))
		{
			rt_used[i] = TRUE;
			rt_numused++;
		}
		else
		{
			if (check_if_rte_used(i + 1, (Node *) (query->qual), 0))
			{
				rt_used[i] = TRUE;
				rt_numused++;
			}
			else
				rt_used[i] = FALSE;
		}
	}

	/* ----------
	 * Now check if any of the used rangetable entries is different
	 * from *NEW* and *CURRENT*. If so we must omit the FROM clause
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
	strcpy(buf, "SELECT");

	/* Then we tell what to select (the targetlist) */
	sep = " ";
	foreach(l, query->targetList)
	{
		bool		tell_as = FALSE;

		tle = (TargetEntry *) lfirst(l);
		strcat(buf, sep);
		sep = ", ";

		strcat(buf, get_tle_expr(qh, 0, tle, (rt_numused > 1)));

		/* Check if we must say AS ... */
		if (nodeTag(tle->expr) != T_Var)
			tell_as = strcmp(tle->resdom->resname, "?column?");
		else
		{
			Var		   *var = (Var *) (tle->expr);
			char	   *attname;

			rte = (RangeTblEntry *) nth(var->varno - 1, query->rtable);
			attname = get_attribute_name(rte->relid, var->varattno);
			if (strcmp(attname, tle->resdom->resname))
				tell_as = TRUE;
		}

		/* and do if so */
		if (tell_as)
		{
			strcat(buf, " AS ");
			strcat(buf, tle->resdom->resname);
		}
	}

	/* If we need other tables that *NEW* or *CURRENT* add the FROM clause */
	if (!rt_constonly && rt_numused > 0)
	{
		strcat(buf, " FROM");

		i = 0;
		sep = " ";
		foreach(l, query->rtable)
		{
			if (rt_used[i++])
			{
				rte = (RangeTblEntry *) lfirst(l);

				if (!strcmp(rte->refname, "*NEW*"))
					continue;

				if (!strcmp(rte->refname, "*CURRENT*"))
					continue;

				strcat(buf, sep);
				sep = ", ";
				strcat(buf, rte->relname);
				if (strcmp(rte->relname, rte->refname) != 0)
				{
					strcat(buf, " ");
					strcat(buf, rte->refname);
				}
			}
		}
	}

	/* Add the WHERE clause if given */
	if (query->qual != NULL)
	{
		strcat(buf, " WHERE ");
		strcat(buf, get_rule_expr(qh, 0, query->qual, (rt_numused > 1)));
	}

	/* Add the GROUP BY CLAUSE */
	if (query->groupClause != NULL)
	{
		strcat(buf, " GROUP BY ");
		sep = "";
		foreach(l, query->groupClause)
		{
			strcat(buf, sep);
			sep = ", ";
			strcat(buf, get_rule_expr(qh, 0, lfirst(l), (rt_numused > 1)));
		}
	}

	/* ----------
	 * Copy the query string into allocated space and return it
	 * ----------
	 */
	return pstrdup(buf);
}


/* ----------
 * get_insert_query_def			- Parse back an INSERT parsetree
 * ----------
 */
static char *
get_insert_query_def(Query *query, QryHier *qh)
{
	char		buf[8192];
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
		if (check_if_rte_used(i + 1, (Node *) (query->targetList), 0))
		{
			rt_used[i] = TRUE;
			rt_numused++;
		}
		else
		{
			if (check_if_rte_used(i + 1, (Node *) (query->qual), 0))
			{
				rt_used[i] = TRUE;
				rt_numused++;
			}
			else
				rt_used[i] = FALSE;
		}
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
	rte = (RangeTblEntry *) nth(query->resultRelation - 1, query->rtable);
	strcpy(buf, "INSERT INTO ");
	strcat(buf, rte->relname);

	/* Add the target list */
	sep = " (";
	foreach(l, query->targetList)
	{
		tle = (TargetEntry *) lfirst(l);

		strcat(buf, sep);
		sep = ", ";
		strcat(buf, tle->resdom->resname);
	}
	strcat(buf, ") ");

	/* Add the VALUES or the SELECT */
	if (rt_constonly && query->qual == NULL)
	{
		strcat(buf, "VALUES (");
		sep = "";
		foreach(l, query->targetList)
		{
			tle = (TargetEntry *) lfirst(l);

			strcat(buf, sep);
			sep = ", ";
			strcat(buf, get_tle_expr(qh, 0, tle, (rt_numused > 1)));
		}
		strcat(buf, ")");
	}
	else
		strcat(buf, get_query_def(query, qh));

	/* ----------
	 * Copy the query string into allocated space and return it
	 * ----------
	 */
	return pstrdup(buf);
}


/* ----------
 * get_update_query_def			- Parse back an UPDATE parsetree
 * ----------
 */
static char *
get_update_query_def(Query *query, QryHier *qh)
{
	char		buf[8192];
	char	   *sep;
	TargetEntry *tle;
	RangeTblEntry *rte;
	List	   *l;

	/* ----------
	 * Start the query with UPDATE relname SET
	 * ----------
	 */
	rte = (RangeTblEntry *) nth(query->resultRelation - 1, query->rtable);
	strcpy(buf, "UPDATE ");
	strcat(buf, rte->relname);
	strcat(buf, " SET ");

	/* Add the comma separated list of 'attname = value' */
	sep = "";
	foreach(l, query->targetList)
	{
		tle = (TargetEntry *) lfirst(l);

		strcat(buf, sep);
		sep = ", ";
		strcat(buf, tle->resdom->resname);
		strcat(buf, " = ");
		strcat(buf, get_tle_expr(qh, query->resultRelation,
								 tle, TRUE));
	}

	/* Finally add a WHERE clause if given */
	if (query->qual != NULL)
	{
		strcat(buf, " WHERE ");
		strcat(buf, get_rule_expr(qh, query->resultRelation,
								  query->qual, TRUE));
	}

	/* ----------
	 * Copy the query string into allocated space and return it
	 * ----------
	 */
	return pstrdup(buf);
}


/* ----------
 * get_delete_query_def			- Parse back a DELETE parsetree
 * ----------
 */
static char *
get_delete_query_def(Query *query, QryHier *qh)
{
	char		buf[8192];
	RangeTblEntry *rte;

	/* ----------
	 * Start the query with DELETE FROM relname
	 * ----------
	 */
	rte = (RangeTblEntry *) nth(query->resultRelation - 1, query->rtable);
	strcpy(buf, "DELETE FROM ");
	strcat(buf, rte->relname);

	/* Add a WHERE clause if given */
	if (query->qual != NULL)
	{
		strcat(buf, " WHERE ");
		strcat(buf, get_rule_expr(qh, 0, query->qual, FALSE));
	}

	/* ----------
	 * Copy the query string into allocated space and return it
	 * ----------
	 */
	return pstrdup(buf);
}


/* ----------
 * get_rule_expr			- Parse back an expression
 * ----------
 */
static char *
get_rule_expr(QryHier *qh, int rt_index, Node *node, bool varprefix)
{
	char		buf[8192];

	if (node == NULL)
		return pstrdup("");

	buf[0] = '\0';

	/* ----------
	 * Up to now I don't know if all the node types below
	 * can really occur in rules actions and qualifications.
	 * There might be some work left.
	 * ----------
	 */
	switch (nodeTag(node))
	{
		case T_TargetEntry:
			{
				TargetEntry *tle = (TargetEntry *) node;

				return get_rule_expr(qh, rt_index,
									 (Node *) (tle->expr), varprefix);
			}
			break;

		case T_Aggreg:
			{
				Aggreg	   *agg = (Aggreg *) node;

				strcat(buf, agg->aggname);
				strcat(buf, "(");
				strcat(buf, get_rule_expr(qh, rt_index,
									 (Node *) (agg->target), varprefix));
				strcat(buf, ")");
				return pstrdup(buf);
			}
			break;

		case T_GroupClause:
			{
				GroupClause *grp = (GroupClause *) node;

				return get_rule_expr(qh, rt_index,
									 (Node *) (grp->entry), varprefix);
			}
			break;

		case T_Expr:
			{
				Expr	   *expr = (Expr *) node;

				/* ----------
				 * Expr nodes have to be handled a bit detailed
				 * ----------
				 */
				switch (expr->opType)
				{
					case OP_EXPR:
						strcat(buf, get_rule_expr(qh, rt_index,
											   (Node *) get_leftop(expr),
												  varprefix));
						strcat(buf, " ");
						strcat(buf, get_opname(((Oper *) expr->oper)->opno));
						strcat(buf, " ");
						strcat(buf, get_rule_expr(qh, rt_index,
											  (Node *) get_rightop(expr),
												  varprefix));
						return pstrdup(buf);
						break;

					case OR_EXPR:
						strcat(buf, "(");
						strcat(buf, get_rule_expr(qh, rt_index,
											   (Node *) get_leftop(expr),
												  varprefix));
						strcat(buf, ") OR (");
						strcat(buf, get_rule_expr(qh, rt_index,
											  (Node *) get_rightop(expr),
												  varprefix));
						strcat(buf, ")");
						return pstrdup(buf);
						break;

					case AND_EXPR:
						strcat(buf, "(");
						strcat(buf, get_rule_expr(qh, rt_index,
											   (Node *) get_leftop(expr),
												  varprefix));
						strcat(buf, ") AND (");
						strcat(buf, get_rule_expr(qh, rt_index,
											  (Node *) get_rightop(expr),
												  varprefix));
						strcat(buf, ")");
						return pstrdup(buf);
						break;

					case NOT_EXPR:
						strcat(buf, "NOT (");
						strcat(buf, get_rule_expr(qh, rt_index,
											   (Node *) get_leftop(expr),
												  varprefix));
						strcat(buf, ")");
						return pstrdup(buf);
						break;

					case FUNC_EXPR:
						return get_func_expr(qh, rt_index,
											 (Expr *) node,
											 varprefix);
						break;

					default:
						printf("\n%s\n", nodeToString(node));
						elog(ERROR, "Expr not yet supported");
				}
			}
			break;

		case T_Var:
			{
				Var		   *var = (Var *) node;
				RangeTblEntry *rte;
				int sup = var->varlevelsup;

				while(sup-- > 0) qh = qh->parent;
				rte = (RangeTblEntry *) nth(var->varno - 1, qh->query->rtable);

				if (qh->parent == NULL && var->varlevelsup > 0)
					rte = (RangeTblEntry *) nth(var->varno + 1, qh->query->rtable);

				if (!strcmp(rte->refname, "*NEW*"))
					strcat(buf, "new.");
				else
				{
					if (!strcmp(rte->refname, "*CURRENT*"))
						strcat(buf, "current.");
					else
					{
						if (strcmp(rte->relname, rte->refname) != 0)
						{
							strcat(buf, rte->refname);
							strcat(buf, ".");
						}
					}
				}
				strcat(buf, get_attribute_name(rte->relid, var->varattno));

				return pstrdup(buf);
			}
			break;

		case T_List:
			{
				printf("\n%s\n", nodeToString(node));
				elog(ERROR, "List not yet supported");
			}
			break;

		case T_SubLink:
			{
				return get_sublink_expr(qh, rt_index, node, varprefix);
			}
			break;

		case T_Const:
			return get_const_expr((Const *) node);
			break;

		default:
			printf("\n%s\n", nodeToString(node));
			elog(ERROR, "get_ruledef of %s: unknown node type %d get_rule_expr()",
				 rulename, nodeTag(node));
			break;
	}

	return FALSE;
}


/* ----------
 * get_func_expr			- Parse back a Func node
 * ----------
 */
static char *
get_func_expr(QryHier *qh, int rt_index, Expr *expr, bool varprefix)
{
	char		buf[8192];
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
								ObjectIdGetDatum(func->funcid), 0, 0, 0);
	if (!HeapTupleIsValid(proctup))
		elog(ERROR, "cache lookup for proc %d failed", func->funcid);

	procStruct = (Form_pg_proc) GETSTRUCT(proctup);
	proname = nameout(&(procStruct->proname));

	if (procStruct->pronargs == 1 && procStruct->proargtypes[0] == InvalidOid)
	{
		if (!strcmp(proname, "nullvalue"))
		{
			strcpy(buf, "(");
			strcat(buf, get_rule_expr(qh, rt_index, lfirst(expr->args),
									  varprefix));
			strcat(buf, ") ISNULL");
			return pstrdup(buf);
		}
		if (!strcmp(proname, "nonnullvalue"))
		{
			strcpy(buf, "(");
			strcat(buf, get_rule_expr(qh, rt_index, lfirst(expr->args),
									  varprefix));
			strcat(buf, ") NOTNULL");
			return pstrdup(buf);
		}
	}

	/* ----------
	 * Build a string of proname(args)
	 * ----------
	 */
	strcpy(buf, proname);
	strcat(buf, "(");
	sep = "";
	foreach(l, expr->args)
	{
		strcat(buf, sep);
		sep = ", ";
		strcat(buf, get_rule_expr(qh, rt_index, lfirst(l), varprefix));
	}
	strcat(buf, ")");

	/* ----------
	 * Copy the function call string into allocated space and return it
	 * ----------
	 */
	return pstrdup(buf);
}


/* ----------
 * get_tle_expr				- A target list expression is a bit
 *					  different from a normal expression.
 *					  If the target column has an
 *					  an atttypmod, the parser usually
 *					  puts a padding-/cut-function call
 *					  around the expression itself. We
 *					  we must get rid of it, otherwise
 *					  dump/reload/dump... would blow up
 *					  the expressions.
 * ----------
 */
static char *
get_tle_expr(QryHier *qh, int rt_index, TargetEntry *tle, bool varprefix)
{
	HeapTuple	proctup;
	Form_pg_proc procStruct;
	Expr	   *expr;
	Func	   *func;
	Const	   *second_arg;

	/* ----------
	 * Check if the result has an atttypmod and if the
	 * expression in the targetlist entry is a function call
	 * ----------
	 */
	if (tle->resdom->restypmod < 0)
		return get_rule_expr(qh, rt_index, tle->expr, varprefix);
	if (nodeTag(tle->expr) != T_Expr)
		return get_rule_expr(qh, rt_index, tle->expr, varprefix);
	expr = (Expr *) (tle->expr);
	if (expr->opType != FUNC_EXPR)
		return get_rule_expr(qh, rt_index, tle->expr, varprefix);

	func = (Func *) (expr->oper);

	/* ----------
	 * Get the functions pg_proc tuple
	 * ----------
	 */
	proctup = SearchSysCacheTuple(PROOID,
								ObjectIdGetDatum(func->funcid), 0, 0, 0);
	if (!HeapTupleIsValid(proctup))
		elog(ERROR, "cache lookup for proc %d failed", func->funcid);

	procStruct = (Form_pg_proc) GETSTRUCT(proctup);

	/* ----------
	 * It must be a function with two arguments where the first
	 * is of the same type as the return value and the second is
	 * an int4.
	 * ----------
	 */
	if (procStruct->pronargs != 2)
		return get_rule_expr(qh, rt_index, tle->expr, varprefix);
	if (procStruct->prorettype != procStruct->proargtypes[0])
		return get_rule_expr(qh, rt_index, tle->expr, varprefix);
	if (procStruct->proargtypes[1] != INT4OID)
		return get_rule_expr(qh, rt_index, tle->expr, varprefix);

	/* ----------
	 * Finally (to be totally safe) the second argument must be a
	 * const and match the value in the results atttypmod.
	 * ----------
	 */
	second_arg = (Const *) nth(1, expr->args);
	if (nodeTag((Node *) second_arg) != T_Const)
		return get_rule_expr(qh, rt_index, tle->expr, varprefix);
	if ((int4) (second_arg->constvalue) != tle->resdom->restypmod)
		return get_rule_expr(qh, rt_index, tle->expr, varprefix);

	/* ----------
	 * Whow - got it. Now get rid of the padding function
	 * ----------
	 */
	return get_rule_expr(qh, rt_index, lfirst(expr->args), varprefix);
}


/* ----------
 * get_const_expr			- Make a string representation
 *					  with the type cast out of a Const
 * ----------
 */
char *
get_const_expr(Const *constval)
{
	HeapTuple	typetup;
	Form_pg_type typeStruct;
	FmgrInfo	finfo_output;
	char	   *extval;
	bool		isnull = FALSE;
	char		buf[8192];
	char		namebuf[64];

	if (constval->constisnull)
		return "NULL";

	typetup = SearchSysCacheTuple(TYPOID,
						 ObjectIdGetDatum(constval->consttype), 0, 0, 0);
	if (!HeapTupleIsValid(typetup))
		elog(ERROR, "cache lookup of type %d failed", constval->consttype);

	typeStruct = (Form_pg_type) GETSTRUCT(typetup);

	fmgr_info(typeStruct->typoutput, &finfo_output);
	extval = (char *) (*fmgr_faddr(&finfo_output)) (constval->constvalue,
													&isnull, -1);

	sprintf(namebuf, "::%s", nameout(&(typeStruct->typname)));
	if (strcmp(namebuf, "::unknown") == 0)
		namebuf[0] = '\0';
	sprintf(buf, "'%s'%s", extval, namebuf);
	return pstrdup(buf);
}


/* ----------
 * get_sublink_expr			- Parse back a sublink
 * ----------
 */
static char *
get_sublink_expr(QryHier *qh, int rt_index, Node *node, bool varprefix)
{
	SubLink    *sublink = (SubLink *) node;
	Query	   *query = (Query *) (sublink->subselect);
	Expr       *expr;
	List	   *l;
	char	   *sep;
	char       buf[8192];

	buf[0] = '\0';

	if (sublink->lefthand != NULL)
	{
		if (length(sublink->lefthand) > 1)
			strcat(buf, "(");

		sep = "";
		foreach(l, sublink->lefthand)
		{
			strcat(buf, sep);
			sep = ", ";
			strcat(buf, get_rule_expr(qh, rt_index,
									  lfirst(l), varprefix));
		}

		if (length(sublink->lefthand) > 1)
			strcat(buf, ") ");
		else
			strcat(buf, " ");
	}

	switch (sublink->subLinkType) {
		case EXISTS_SUBLINK:
			strcat(buf, "EXISTS ");
			break;

		case ANY_SUBLINK:
			expr = (Expr *)lfirst(sublink->oper);
			strcat(buf, get_opname(((Oper *) (expr->oper))->opno));
			strcat(buf, " ANY ");
			break;

		case ALL_SUBLINK:
			expr = (Expr *)lfirst(sublink->oper);
			strcat(buf, get_opname(((Oper *) (expr->oper))->opno));
			strcat(buf, " ALL ");
			break;

		case EXPR_SUBLINK:
			expr = (Expr *)lfirst(sublink->oper);
			strcat(buf, get_opname(((Oper *) (expr->oper))->opno));
			strcat(buf, " ");
			break;

		default:
			elog(ERROR, "unupported sublink type %d",
					sublink->subLinkType);
			break;
	}

	strcat(buf, "(");
	strcat(buf, get_query_def(query, qh));
	strcat(buf, ")");

	return pstrdup(buf);
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
		elog(ERROR, "cache lookup of relation %d failed", relid);

	classStruct = (Form_pg_class) GETSTRUCT(classtup);
	return nameout(&(classStruct->relname));
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
						  ObjectIdGetDatum(relid), (Datum) attnum, 0, 0);
	if (!HeapTupleIsValid(atttup))
		elog(ERROR, "cache lookup of attribute %d in relation %d failed",
			 attnum, relid);

	attStruct = (Form_pg_attribute) GETSTRUCT(atttup);
	return nameout(&(attStruct->attname));
}


/* ----------
 * check_if_rte_used			- Check a targetlist or qual
 *					  if a given rangetable entry
 *					  is used in it
 * ----------
 */
static bool
check_if_rte_used(int rt_index, Node *node, int sup)
{
	if (node == NULL)
		return FALSE;

	switch (nodeTag(node))
	{
		case T_TargetEntry:
			{
				TargetEntry *tle = (TargetEntry *) node;

				return check_if_rte_used(rt_index,
										 (Node *) (tle->expr), sup);
			}
			break;

		case T_Aggreg:
			{
				Aggreg	   *agg = (Aggreg *) node;

				return check_if_rte_used(rt_index,
										 (Node *) (agg->target), sup);
			}
			break;

		case T_GroupClause:
			{
				GroupClause *grp = (GroupClause *) node;

				return check_if_rte_used(rt_index,
										 (Node *) (grp->entry), sup);
			}
			break;

		case T_Expr:
			{
				Expr	   *expr = (Expr *) node;

				return check_if_rte_used(rt_index,
										 (Node *) (expr->args), sup);
			}
			break;

		case T_Var:
			{
				Var		   *var = (Var *) node;

				return var->varno == rt_index && var->varlevelsup == sup;
			}
			break;

		case T_List:
			{
				List	   *l;

				foreach(l, (List *) node)
				{
					if (check_if_rte_used(rt_index, lfirst(l), sup))
						return TRUE;
				}
				return FALSE;
			}
			break;

		case T_SubLink:
			{
				SubLink    *sublink = (SubLink *) node;
				Query	   *query = (Query *) sublink->subselect;

				if (check_if_rte_used(rt_index, (Node *) (query->qual), sup + 1))
					return TRUE;

				if (check_if_rte_used(rt_index, (Node *) (sublink->lefthand), sup))
					return TRUE;

				return FALSE;
			}
			break;

		case T_Const:
			return FALSE;
			break;

		default:
			elog(ERROR, "get_ruledef of %s: unknown node type %d in check_if_rte_used()",
				 rulename, nodeTag(node));
			break;
	}

	return FALSE;
}
