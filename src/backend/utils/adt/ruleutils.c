/**********************************************************************
 * ruleutils.c	- Functions to convert stored expressions/querytrees
 *				back to source text
 *
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/ruleutils.c,v 1.235.2.6 2008/06/06 17:59:45 tgl Exp $
 **********************************************************************/

#include "postgres.h"

#include <unistd.h>
#include <fcntl.h>

#include "access/genam.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_trigger.h"
#include "commands/tablespace.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/tlist.h"
#include "parser/gramparse.h"
#include "parser/keywords.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteManip.h"
#include "rewrite/rewriteSupport.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"


/* ----------
 * Pretty formatting constants
 * ----------
 */

/* Indent counts */
#define PRETTYINDENT_STD		8
#define PRETTYINDENT_JOIN	   13
#define PRETTYINDENT_JOIN_ON	(PRETTYINDENT_JOIN-PRETTYINDENT_STD)
#define PRETTYINDENT_VAR		4

/* Pretty flags */
#define PRETTYFLAG_PAREN		1
#define PRETTYFLAG_INDENT		2

/* macro to test if pretty action needed */
#define PRETTY_PAREN(context)	((context)->prettyFlags & PRETTYFLAG_PAREN)
#define PRETTY_INDENT(context)	((context)->prettyFlags & PRETTYFLAG_INDENT)


/* ----------
 * Local data types
 * ----------
 */

/* Context info needed for invoking a recursive querytree display routine */
typedef struct
{
	StringInfo	buf;			/* output buffer to append to */
	List	   *namespaces;		/* List of deparse_namespace nodes */
	int			prettyFlags;	/* enabling of pretty-print functions */
	int			indentLevel;	/* current indent level for prettyprint */
	bool		varprefix;		/* TRUE to print prefixes on Vars */
} deparse_context;

/*
 * Each level of query context around a subtree needs a level of Var namespace.
 * A Var having varlevelsup=N refers to the N'th item (counting from 0) in
 * the current context's namespaces list.
 *
 * The rangetable is the list of actual RTEs from the query tree.
 *
 * For deparsing plan trees, we allow two special RTE entries that are not
 * part of the rtable list (partly because they don't have consecutively
 * allocated varnos).
 */
typedef struct
{
	List	   *rtable;			/* List of RangeTblEntry nodes */
	int			outer_varno;	/* varno for outer_rte */
	RangeTblEntry *outer_rte;	/* special RangeTblEntry, or NULL */
	int			inner_varno;	/* varno for inner_rte */
	RangeTblEntry *inner_rte;	/* special RangeTblEntry, or NULL */
} deparse_namespace;


/* ----------
 * Global data
 * ----------
 */
static void *plan_getrulebyoid = NULL;
static char *query_getrulebyoid = "SELECT * FROM pg_catalog.pg_rewrite WHERE oid = $1";
static void *plan_getviewrule = NULL;
static char *query_getviewrule = "SELECT * FROM pg_catalog.pg_rewrite WHERE ev_class = $1 AND rulename = $2";


/* ----------
 * Local functions
 *
 * Most of these functions used to use fixed-size buffers to build their
 * results.  Now, they take an (already initialized) StringInfo object
 * as a parameter, and append their text output to its contents.
 * ----------
 */
static char *deparse_expression_pretty(Node *expr, List *dpcontext,
						  bool forceprefix, bool showimplicit,
						  int prettyFlags, int startIndent);
static char *pg_get_viewdef_worker(Oid viewoid, int prettyFlags);
static void decompile_column_index_array(Datum column_index_array, Oid relId,
							 StringInfo buf);
static char *pg_get_ruledef_worker(Oid ruleoid, int prettyFlags);
static char *pg_get_indexdef_worker(Oid indexrelid, int colno, bool showTblSpc,
					   int prettyFlags);
static char *pg_get_constraintdef_worker(Oid constraintId, bool fullCommand,
							int prettyFlags);
static char *pg_get_expr_worker(text *expr, Oid relid, char *relname,
				   int prettyFlags);
static Oid	get_constraint_index(Oid constraintId);
static void make_ruledef(StringInfo buf, HeapTuple ruletup, TupleDesc rulettc,
			 int prettyFlags);
static void make_viewdef(StringInfo buf, HeapTuple ruletup, TupleDesc rulettc,
			 int prettyFlags);
static void get_query_def(Query *query, StringInfo buf, List *parentnamespace,
			  TupleDesc resultDesc, int prettyFlags, int startIndent);
static void get_values_def(List *values_lists, deparse_context *context);
static void get_select_query_def(Query *query, deparse_context *context,
					 TupleDesc resultDesc);
static void get_insert_query_def(Query *query, deparse_context *context);
static void get_update_query_def(Query *query, deparse_context *context);
static void get_delete_query_def(Query *query, deparse_context *context);
static void get_utility_query_def(Query *query, deparse_context *context);
static void get_basic_select_query(Query *query, deparse_context *context,
					   TupleDesc resultDesc);
static void get_target_list(List *targetList, deparse_context *context,
				TupleDesc resultDesc);
static void get_setop_query(Node *setOp, Query *query,
				deparse_context *context,
				TupleDesc resultDesc);
static Node *get_rule_sortgroupclause(SortClause *srt, List *tlist,
						 bool force_colno,
						 deparse_context *context);
static void get_names_for_var(Var *var, int levelsup, deparse_context *context,
				  char **schemaname, char **refname, char **attname);
static RangeTblEntry *find_rte_by_refname(const char *refname,
					deparse_context *context);
static const char *get_simple_binary_op_name(OpExpr *expr);
static bool isSimpleNode(Node *node, Node *parentNode, int prettyFlags);
static void appendStringInfoSpaces(StringInfo buf, int count);
static void appendContextKeyword(deparse_context *context, const char *str,
					 int indentBefore, int indentAfter, int indentPlus);
static void get_rule_expr(Node *node, deparse_context *context,
			  bool showimplicit);
static void get_oper_expr(OpExpr *expr, deparse_context *context);
static void get_func_expr(FuncExpr *expr, deparse_context *context,
			  bool showimplicit);
static void get_agg_expr(Aggref *aggref, deparse_context *context);
static void get_const_expr(Const *constval, deparse_context *context,
						   int showtype);
static void get_sublink_expr(SubLink *sublink, deparse_context *context);
static void get_from_clause(Query *query, const char *prefix,
				deparse_context *context);
static void get_from_clause_item(Node *jtnode, Query *query,
					 deparse_context *context);
static void get_from_clause_alias(Alias *alias, RangeTblEntry *rte,
					  deparse_context *context);
static void get_from_clause_coldeflist(List *names, List *types, List *typmods,
						   deparse_context *context);
static void get_opclass_name(Oid opclass, Oid actual_datatype,
				 StringInfo buf);
static Node *processIndirection(Node *node, deparse_context *context,
				   bool printit);
static void printSubscripts(ArrayRef *aref, deparse_context *context);
static char *generate_relation_name(Oid relid);
static char *generate_function_name(Oid funcid, int nargs, Oid *argtypes);
static char *generate_operator_name(Oid operid, Oid arg1, Oid arg2);
static text *string_to_text(char *str);
static char *flatten_reloptions(Oid relid);

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
	Oid			ruleoid = PG_GETARG_OID(0);

	PG_RETURN_TEXT_P(string_to_text(pg_get_ruledef_worker(ruleoid, 0)));
}


Datum
pg_get_ruledef_ext(PG_FUNCTION_ARGS)
{
	Oid			ruleoid = PG_GETARG_OID(0);
	bool		pretty = PG_GETARG_BOOL(1);
	int			prettyFlags;

	prettyFlags = pretty ? PRETTYFLAG_PAREN | PRETTYFLAG_INDENT : 0;
	PG_RETURN_TEXT_P(string_to_text(pg_get_ruledef_worker(ruleoid, prettyFlags)));
}


static char *
pg_get_ruledef_worker(Oid ruleoid, int prettyFlags)
{
	Datum		args[1];
	char		nulls[1];
	int			spirc;
	HeapTuple	ruletup;
	TupleDesc	rulettc;
	StringInfoData buf;

	/*
	 * Do this first so that string is alloc'd in outer context not SPI's.
	 */
	initStringInfo(&buf);

	/*
	 * Connect to SPI manager
	 */
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	/*
	 * On the first call prepare the plan to lookup pg_rewrite. We read
	 * pg_rewrite over the SPI manager instead of using the syscache to be
	 * checked for read access on pg_rewrite.
	 */
	if (plan_getrulebyoid == NULL)
	{
		Oid			argtypes[1];
		void	   *plan;

		argtypes[0] = OIDOID;
		plan = SPI_prepare(query_getrulebyoid, 1, argtypes);
		if (plan == NULL)
			elog(ERROR, "SPI_prepare failed for \"%s\"", query_getrulebyoid);
		plan_getrulebyoid = SPI_saveplan(plan);
	}

	/*
	 * Get the pg_rewrite tuple for this rule
	 */
	args[0] = ObjectIdGetDatum(ruleoid);
	nulls[0] = ' ';
	spirc = SPI_execute_plan(plan_getrulebyoid, args, nulls, true, 1);
	if (spirc != SPI_OK_SELECT)
		elog(ERROR, "failed to get pg_rewrite tuple for rule %u", ruleoid);
	if (SPI_processed != 1)
		appendStringInfo(&buf, "-");
	else
	{
		/*
		 * Get the rule's definition and put it into executor's memory
		 */
		ruletup = SPI_tuptable->vals[0];
		rulettc = SPI_tuptable->tupdesc;
		make_ruledef(&buf, ruletup, rulettc, prettyFlags);
	}

	/*
	 * Disconnect from SPI manager
	 */
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

	return buf.data;
}


/* ----------
 * get_viewdef			- Mainly the same thing, but we
 *				  only return the SELECT part of a view
 * ----------
 */
Datum
pg_get_viewdef(PG_FUNCTION_ARGS)
{
	/* By OID */
	Oid			viewoid = PG_GETARG_OID(0);

	PG_RETURN_TEXT_P(string_to_text(pg_get_viewdef_worker(viewoid, 0)));
}


Datum
pg_get_viewdef_ext(PG_FUNCTION_ARGS)
{
	/* By OID */
	Oid			viewoid = PG_GETARG_OID(0);
	bool		pretty = PG_GETARG_BOOL(1);
	int			prettyFlags;

	prettyFlags = pretty ? PRETTYFLAG_PAREN | PRETTYFLAG_INDENT : 0;
	PG_RETURN_TEXT_P(string_to_text(pg_get_viewdef_worker(viewoid, prettyFlags)));
}

Datum
pg_get_viewdef_name(PG_FUNCTION_ARGS)
{
	/* By qualified name */
	text	   *viewname = PG_GETARG_TEXT_P(0);
	RangeVar   *viewrel;
	Oid			viewoid;

	viewrel = makeRangeVarFromNameList(textToQualifiedNameList(viewname));
	viewoid = RangeVarGetRelid(viewrel, false);

	PG_RETURN_TEXT_P(string_to_text(pg_get_viewdef_worker(viewoid, 0)));
}


Datum
pg_get_viewdef_name_ext(PG_FUNCTION_ARGS)
{
	/* By qualified name */
	text	   *viewname = PG_GETARG_TEXT_P(0);
	bool		pretty = PG_GETARG_BOOL(1);
	int			prettyFlags;
	RangeVar   *viewrel;
	Oid			viewoid;

	prettyFlags = pretty ? PRETTYFLAG_PAREN | PRETTYFLAG_INDENT : 0;
	viewrel = makeRangeVarFromNameList(textToQualifiedNameList(viewname));
	viewoid = RangeVarGetRelid(viewrel, false);

	PG_RETURN_TEXT_P(string_to_text(pg_get_viewdef_worker(viewoid, prettyFlags)));
}

/*
 * Common code for by-OID and by-name variants of pg_get_viewdef
 */
static char *
pg_get_viewdef_worker(Oid viewoid, int prettyFlags)
{
	Datum		args[2];
	char		nulls[2];
	int			spirc;
	HeapTuple	ruletup;
	TupleDesc	rulettc;
	StringInfoData buf;

	/*
	 * Do this first so that string is alloc'd in outer context not SPI's.
	 */
	initStringInfo(&buf);

	/*
	 * Connect to SPI manager
	 */
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	/*
	 * On the first call prepare the plan to lookup pg_rewrite. We read
	 * pg_rewrite over the SPI manager instead of using the syscache to be
	 * checked for read access on pg_rewrite.
	 */
	if (plan_getviewrule == NULL)
	{
		Oid			argtypes[2];
		void	   *plan;

		argtypes[0] = OIDOID;
		argtypes[1] = NAMEOID;
		plan = SPI_prepare(query_getviewrule, 2, argtypes);
		if (plan == NULL)
			elog(ERROR, "SPI_prepare failed for \"%s\"", query_getviewrule);
		plan_getviewrule = SPI_saveplan(plan);
	}

	/*
	 * Get the pg_rewrite tuple for the view's SELECT rule
	 */
	args[0] = ObjectIdGetDatum(viewoid);
	args[1] = PointerGetDatum(ViewSelectRuleName);
	nulls[0] = ' ';
	nulls[1] = ' ';
	spirc = SPI_execute_plan(plan_getviewrule, args, nulls, true, 2);
	if (spirc != SPI_OK_SELECT)
		elog(ERROR, "failed to get pg_rewrite tuple for view %u", viewoid);
	if (SPI_processed != 1)
		appendStringInfo(&buf, "Not a view");
	else
	{
		/*
		 * Get the rule's definition and put it into executor's memory
		 */
		ruletup = SPI_tuptable->vals[0];
		rulettc = SPI_tuptable->tupdesc;
		make_viewdef(&buf, ruletup, rulettc, prettyFlags);
	}

	/*
	 * Disconnect from SPI manager
	 */
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

	return buf.data;
}

/* ----------
 * get_triggerdef			- Get the definition of a trigger
 * ----------
 */
Datum
pg_get_triggerdef(PG_FUNCTION_ARGS)
{
	Oid			trigid = PG_GETARG_OID(0);
	HeapTuple	ht_trig;
	Form_pg_trigger trigrec;
	StringInfoData buf;
	Relation	tgrel;
	ScanKeyData skey[1];
	SysScanDesc tgscan;
	int			findx = 0;
	char	   *tgname;

	/*
	 * Fetch the pg_trigger tuple by the Oid of the trigger
	 */
	tgrel = heap_open(TriggerRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(trigid));

	tgscan = systable_beginscan(tgrel, TriggerOidIndexId, true,
								SnapshotNow, 1, skey);

	ht_trig = systable_getnext(tgscan);

	if (!HeapTupleIsValid(ht_trig))
		elog(ERROR, "could not find tuple for trigger %u", trigid);

	trigrec = (Form_pg_trigger) GETSTRUCT(ht_trig);

	/*
	 * Start the trigger definition. Note that the trigger's name should never
	 * be schema-qualified, but the trigger rel's name may be.
	 */
	initStringInfo(&buf);

	tgname = NameStr(trigrec->tgname);
	appendStringInfo(&buf, "CREATE %sTRIGGER %s ",
					 trigrec->tgisconstraint ? "CONSTRAINT " : "",
					 quote_identifier(tgname));

	if (TRIGGER_FOR_BEFORE(trigrec->tgtype))
		appendStringInfo(&buf, "BEFORE");
	else
		appendStringInfo(&buf, "AFTER");
	if (TRIGGER_FOR_INSERT(trigrec->tgtype))
	{
		appendStringInfo(&buf, " INSERT");
		findx++;
	}
	if (TRIGGER_FOR_DELETE(trigrec->tgtype))
	{
		if (findx > 0)
			appendStringInfo(&buf, " OR DELETE");
		else
			appendStringInfo(&buf, " DELETE");
		findx++;
	}
	if (TRIGGER_FOR_UPDATE(trigrec->tgtype))
	{
		if (findx > 0)
			appendStringInfo(&buf, " OR UPDATE");
		else
			appendStringInfo(&buf, " UPDATE");
	}
	appendStringInfo(&buf, " ON %s ",
					 generate_relation_name(trigrec->tgrelid));

	if (trigrec->tgisconstraint)
	{
		if (trigrec->tgconstrrelid != InvalidOid)
			appendStringInfo(&buf, "FROM %s ",
							 generate_relation_name(trigrec->tgconstrrelid));
		if (!trigrec->tgdeferrable)
			appendStringInfo(&buf, "NOT ");
		appendStringInfo(&buf, "DEFERRABLE INITIALLY ");
		if (trigrec->tginitdeferred)
			appendStringInfo(&buf, "DEFERRED ");
		else
			appendStringInfo(&buf, "IMMEDIATE ");

	}

	if (TRIGGER_FOR_ROW(trigrec->tgtype))
		appendStringInfo(&buf, "FOR EACH ROW ");
	else
		appendStringInfo(&buf, "FOR EACH STATEMENT ");

	appendStringInfo(&buf, "EXECUTE PROCEDURE %s(",
					 generate_function_name(trigrec->tgfoid, 0, NULL));

	if (trigrec->tgnargs > 0)
	{
		bytea	   *val;
		bool		isnull;
		char	   *p;
		int			i;

		val = DatumGetByteaP(fastgetattr(ht_trig,
										Anum_pg_trigger_tgargs,
										tgrel->rd_att, &isnull));
		if (isnull)
			elog(ERROR, "tgargs is null for trigger %u", trigid);
		p = (char *) VARDATA(val);
		for (i = 0; i < trigrec->tgnargs; i++)
		{
			if (i > 0)
				appendStringInfo(&buf, ", ");

			/*
			 * We form the string literal according to the prevailing setting
			 * of standard_conforming_strings; we never use E''. User is
			 * responsible for making sure result is used correctly.
			 */
			appendStringInfoChar(&buf, '\'');
			while (*p)
			{
				char		ch = *p++;

				if (SQL_STR_DOUBLE(ch, !standard_conforming_strings))
					appendStringInfoChar(&buf, ch);
				appendStringInfoChar(&buf, ch);
			}
			appendStringInfoChar(&buf, '\'');
			/* advance p to next string embedded in tgargs */
			p++;
		}
	}

	/* We deliberately do not put semi-colon at end */
	appendStringInfo(&buf, ")");

	/* Clean up */
	systable_endscan(tgscan);

	heap_close(tgrel, AccessShareLock);

	PG_RETURN_TEXT_P(string_to_text(buf.data));
}

/* ----------
 * get_indexdef			- Get the definition of an index
 *
 * In the extended version, there is a colno argument as well as pretty bool.
 *	if colno == 0, we want a complete index definition.
 *	if colno > 0, we only want the Nth index key's variable or expression.
 *
 * Note that the SQL-function versions of this omit any info about the
 * index tablespace; this is intentional because pg_dump wants it that way.
 * However pg_get_indexdef_string() includes index tablespace if not default.
 * ----------
 */
Datum
pg_get_indexdef(PG_FUNCTION_ARGS)
{
	Oid			indexrelid = PG_GETARG_OID(0);

	PG_RETURN_TEXT_P(string_to_text(pg_get_indexdef_worker(indexrelid, 0,
														   false, 0)));
}

Datum
pg_get_indexdef_ext(PG_FUNCTION_ARGS)
{
	Oid			indexrelid = PG_GETARG_OID(0);
	int32		colno = PG_GETARG_INT32(1);
	bool		pretty = PG_GETARG_BOOL(2);
	int			prettyFlags;

	prettyFlags = pretty ? PRETTYFLAG_PAREN | PRETTYFLAG_INDENT : 0;
	PG_RETURN_TEXT_P(string_to_text(pg_get_indexdef_worker(indexrelid, colno,
														 false, prettyFlags)));
}

/* Internal version that returns a palloc'd C string */
char *
pg_get_indexdef_string(Oid indexrelid)
{
	return pg_get_indexdef_worker(indexrelid, 0, true, 0);
}

static char *
pg_get_indexdef_worker(Oid indexrelid, int colno, bool showTblSpc,
					   int prettyFlags)
{
	HeapTuple	ht_idx;
	HeapTuple	ht_idxrel;
	HeapTuple	ht_am;
	Form_pg_index idxrec;
	Form_pg_class idxrelrec;
	Form_pg_am	amrec;
	List	   *indexprs;
	ListCell   *indexpr_item;
	List	   *context;
	Oid			indrelid;
	int			keyno;
	Oid			keycoltype;
	Datum		indclassDatum;
	bool		isnull;
	oidvector  *indclass;
	StringInfoData buf;
	char	   *str;
	char	   *sep;

	/*
	 * Fetch the pg_index tuple by the Oid of the index
	 */
	ht_idx = SearchSysCache(INDEXRELID,
							ObjectIdGetDatum(indexrelid),
							0, 0, 0);
	if (!HeapTupleIsValid(ht_idx))
		elog(ERROR, "cache lookup failed for index %u", indexrelid);
	idxrec = (Form_pg_index) GETSTRUCT(ht_idx);

	indrelid = idxrec->indrelid;
	Assert(indexrelid == idxrec->indexrelid);

	/* Must get indclass the hard way */
	indclassDatum = SysCacheGetAttr(INDEXRELID, ht_idx,
									Anum_pg_index_indclass, &isnull);
	Assert(!isnull);
	indclass = (oidvector *) DatumGetPointer(indclassDatum);

	/*
	 * Fetch the pg_class tuple of the index relation
	 */
	ht_idxrel = SearchSysCache(RELOID,
							   ObjectIdGetDatum(indexrelid),
							   0, 0, 0);
	if (!HeapTupleIsValid(ht_idxrel))
		elog(ERROR, "cache lookup failed for relation %u", indexrelid);
	idxrelrec = (Form_pg_class) GETSTRUCT(ht_idxrel);

	/*
	 * Fetch the pg_am tuple of the index' access method
	 */
	ht_am = SearchSysCache(AMOID,
						   ObjectIdGetDatum(idxrelrec->relam),
						   0, 0, 0);
	if (!HeapTupleIsValid(ht_am))
		elog(ERROR, "cache lookup failed for access method %u",
			 idxrelrec->relam);
	amrec = (Form_pg_am) GETSTRUCT(ht_am);

	/*
	 * Get the index expressions, if any.  (NOTE: we do not use the relcache
	 * versions of the expressions and predicate, because we want to display
	 * non-const-folded expressions.)
	 */
	if (!heap_attisnull(ht_idx, Anum_pg_index_indexprs))
	{
		Datum		exprsDatum;
		bool		isnull;
		char	   *exprsString;

		exprsDatum = SysCacheGetAttr(INDEXRELID, ht_idx,
									 Anum_pg_index_indexprs, &isnull);
		Assert(!isnull);
		exprsString = DatumGetCString(DirectFunctionCall1(textout,
														  exprsDatum));
		indexprs = (List *) stringToNode(exprsString);
		pfree(exprsString);
	}
	else
		indexprs = NIL;

	indexpr_item = list_head(indexprs);

	context = deparse_context_for(get_rel_name(indrelid), indrelid);

	/*
	 * Start the index definition.	Note that the index's name should never be
	 * schema-qualified, but the indexed rel's name may be.
	 */
	initStringInfo(&buf);

	if (!colno)
		appendStringInfo(&buf, "CREATE %sINDEX %s ON %s USING %s (",
						 idxrec->indisunique ? "UNIQUE " : "",
						 quote_identifier(NameStr(idxrelrec->relname)),
						 generate_relation_name(indrelid),
						 quote_identifier(NameStr(amrec->amname)));

	/*
	 * Report the indexed attributes
	 */
	sep = "";
	for (keyno = 0; keyno < idxrec->indnatts; keyno++)
	{
		AttrNumber	attnum = idxrec->indkey.values[keyno];

		if (!colno)
			appendStringInfoString(&buf, sep);
		sep = ", ";

		if (attnum != 0)
		{
			/* Simple index column */
			char	   *attname;

			attname = get_relid_attribute_name(indrelid, attnum);
			if (!colno || colno == keyno + 1)
				appendStringInfoString(&buf, quote_identifier(attname));
			keycoltype = get_atttype(indrelid, attnum);
		}
		else
		{
			/* expressional index */
			Node	   *indexkey;

			if (indexpr_item == NULL)
				elog(ERROR, "too few entries in indexprs list");
			indexkey = (Node *) lfirst(indexpr_item);
			indexpr_item = lnext(indexpr_item);
			/* Deparse */
			str = deparse_expression_pretty(indexkey, context, false, false,
											prettyFlags, 0);
			if (!colno || colno == keyno + 1)
			{
				/* Need parens if it's not a bare function call */
				if (indexkey && IsA(indexkey, FuncExpr) &&
				 ((FuncExpr *) indexkey)->funcformat == COERCE_EXPLICIT_CALL)
					appendStringInfoString(&buf, str);
				else
					appendStringInfo(&buf, "(%s)", str);
			}
			keycoltype = exprType(indexkey);
		}

		/*
		 * Add the operator class name
		 */
		if (!colno)
			get_opclass_name(indclass->values[keyno], keycoltype,
							 &buf);
	}

	if (!colno)
	{
		appendStringInfoChar(&buf, ')');

		/*
		 * If it has options, append "WITH (options)"
		 */
		str = flatten_reloptions(indexrelid);
		if (str)
		{
			appendStringInfo(&buf, " WITH (%s)", str);
			pfree(str);
		}

		/*
		 * If it's in a nondefault tablespace, say so, but only if requested
		 */
		if (showTblSpc)
		{
			Oid			tblspc;

			tblspc = get_rel_tablespace(indexrelid);
			if (OidIsValid(tblspc))
				appendStringInfo(&buf, " TABLESPACE %s",
								 quote_identifier(get_tablespace_name(tblspc)));
		}

		/*
		 * If it's a partial index, decompile and append the predicate
		 */
		if (!heap_attisnull(ht_idx, Anum_pg_index_indpred))
		{
			Node	   *node;
			Datum		predDatum;
			bool		isnull;
			char	   *predString;

			/* Convert text string to node tree */
			predDatum = SysCacheGetAttr(INDEXRELID, ht_idx,
										Anum_pg_index_indpred, &isnull);
			Assert(!isnull);
			predString = DatumGetCString(DirectFunctionCall1(textout,
															 predDatum));
			node = (Node *) stringToNode(predString);
			pfree(predString);

			/* Deparse */
			str = deparse_expression_pretty(node, context, false, false,
											prettyFlags, 0);
			appendStringInfo(&buf, " WHERE %s", str);
		}
	}

	/* Clean up */
	ReleaseSysCache(ht_idx);
	ReleaseSysCache(ht_idxrel);
	ReleaseSysCache(ht_am);

	return buf.data;
}


/*
 * pg_get_constraintdef
 *
 * Returns the definition for the constraint, ie, everything that needs to
 * appear after "ALTER TABLE ... ADD CONSTRAINT <constraintname>".
 */
Datum
pg_get_constraintdef(PG_FUNCTION_ARGS)
{
	Oid			constraintId = PG_GETARG_OID(0);

	PG_RETURN_TEXT_P(string_to_text(pg_get_constraintdef_worker(constraintId,
																false, 0)));
}

Datum
pg_get_constraintdef_ext(PG_FUNCTION_ARGS)
{
	Oid			constraintId = PG_GETARG_OID(0);
	bool		pretty = PG_GETARG_BOOL(1);
	int			prettyFlags;

	prettyFlags = pretty ? PRETTYFLAG_PAREN | PRETTYFLAG_INDENT : 0;
	PG_RETURN_TEXT_P(string_to_text(pg_get_constraintdef_worker(constraintId,
													   false, prettyFlags)));
}

/* Internal version that returns a palloc'd C string */
char *
pg_get_constraintdef_string(Oid constraintId)
{
	return pg_get_constraintdef_worker(constraintId, true, 0);
}

static char *
pg_get_constraintdef_worker(Oid constraintId, bool fullCommand,
							int prettyFlags)
{
	StringInfoData buf;
	Relation	conDesc;
	SysScanDesc conscan;
	ScanKeyData skey[1];
	HeapTuple	tup;
	Form_pg_constraint conForm;

	/*
	 * Fetch the pg_constraint row.  There's no syscache for pg_constraint so
	 * we must do it the hard way.
	 */
	conDesc = heap_open(ConstraintRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(constraintId));

	conscan = systable_beginscan(conDesc, ConstraintOidIndexId, true,
								 SnapshotNow, 1, skey);

	tup = systable_getnext(conscan);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "could not find tuple for constraint %u", constraintId);
	conForm = (Form_pg_constraint) GETSTRUCT(tup);

	initStringInfo(&buf);

	if (fullCommand && OidIsValid(conForm->conrelid))
	{
		appendStringInfo(&buf, "ALTER TABLE ONLY %s ADD CONSTRAINT %s ",
						 generate_relation_name(conForm->conrelid),
						 quote_identifier(NameStr(conForm->conname)));
	}

	switch (conForm->contype)
	{
		case CONSTRAINT_FOREIGN:
			{
				Datum		val;
				bool		isnull;
				const char *string;

				/* Start off the constraint definition */
				appendStringInfo(&buf, "FOREIGN KEY (");

				/* Fetch and build referencing-column list */
				val = heap_getattr(tup, Anum_pg_constraint_conkey,
								   RelationGetDescr(conDesc), &isnull);
				if (isnull)
					elog(ERROR, "null conkey for constraint %u",
						 constraintId);

				decompile_column_index_array(val, conForm->conrelid, &buf);

				/* add foreign relation name */
				appendStringInfo(&buf, ") REFERENCES %s(",
								 generate_relation_name(conForm->confrelid));

				/* Fetch and build referenced-column list */
				val = heap_getattr(tup, Anum_pg_constraint_confkey,
								   RelationGetDescr(conDesc), &isnull);
				if (isnull)
					elog(ERROR, "null confkey for constraint %u",
						 constraintId);

				decompile_column_index_array(val, conForm->confrelid, &buf);

				appendStringInfo(&buf, ")");

				/* Add match type */
				switch (conForm->confmatchtype)
				{
					case FKCONSTR_MATCH_FULL:
						string = " MATCH FULL";
						break;
					case FKCONSTR_MATCH_PARTIAL:
						string = " MATCH PARTIAL";
						break;
					case FKCONSTR_MATCH_UNSPECIFIED:
						string = "";
						break;
					default:
						elog(ERROR, "unrecognized confmatchtype: %d",
							 conForm->confmatchtype);
						string = "";	/* keep compiler quiet */
						break;
				}
				appendStringInfoString(&buf, string);

				/* Add ON UPDATE and ON DELETE clauses, if needed */
				switch (conForm->confupdtype)
				{
					case FKCONSTR_ACTION_NOACTION:
						string = NULL;	/* suppress default */
						break;
					case FKCONSTR_ACTION_RESTRICT:
						string = "RESTRICT";
						break;
					case FKCONSTR_ACTION_CASCADE:
						string = "CASCADE";
						break;
					case FKCONSTR_ACTION_SETNULL:
						string = "SET NULL";
						break;
					case FKCONSTR_ACTION_SETDEFAULT:
						string = "SET DEFAULT";
						break;
					default:
						elog(ERROR, "unrecognized confupdtype: %d",
							 conForm->confupdtype);
						string = NULL;	/* keep compiler quiet */
						break;
				}
				if (string)
					appendStringInfo(&buf, " ON UPDATE %s", string);

				switch (conForm->confdeltype)
				{
					case FKCONSTR_ACTION_NOACTION:
						string = NULL;	/* suppress default */
						break;
					case FKCONSTR_ACTION_RESTRICT:
						string = "RESTRICT";
						break;
					case FKCONSTR_ACTION_CASCADE:
						string = "CASCADE";
						break;
					case FKCONSTR_ACTION_SETNULL:
						string = "SET NULL";
						break;
					case FKCONSTR_ACTION_SETDEFAULT:
						string = "SET DEFAULT";
						break;
					default:
						elog(ERROR, "unrecognized confdeltype: %d",
							 conForm->confdeltype);
						string = NULL;	/* keep compiler quiet */
						break;
				}
				if (string)
					appendStringInfo(&buf, " ON DELETE %s", string);

				if (conForm->condeferrable)
					appendStringInfo(&buf, " DEFERRABLE");
				if (conForm->condeferred)
					appendStringInfo(&buf, " INITIALLY DEFERRED");

				break;
			}
		case CONSTRAINT_PRIMARY:
		case CONSTRAINT_UNIQUE:
			{
				Datum		val;
				bool		isnull;
				Oid			indexId;

				/* Start off the constraint definition */
				if (conForm->contype == CONSTRAINT_PRIMARY)
					appendStringInfo(&buf, "PRIMARY KEY (");
				else
					appendStringInfo(&buf, "UNIQUE (");

				/* Fetch and build target column list */
				val = heap_getattr(tup, Anum_pg_constraint_conkey,
								   RelationGetDescr(conDesc), &isnull);
				if (isnull)
					elog(ERROR, "null conkey for constraint %u",
						 constraintId);

				decompile_column_index_array(val, conForm->conrelid, &buf);

				appendStringInfo(&buf, ")");

				indexId = get_constraint_index(constraintId);

				/* XXX why do we only print these bits if fullCommand? */
				if (fullCommand && OidIsValid(indexId))
				{
					char	   *options = flatten_reloptions(indexId);
					Oid			tblspc;

					if (options)
					{
						appendStringInfo(&buf, " WITH (%s)", options);
						pfree(options);
					}

					tblspc = get_rel_tablespace(indexId);
					if (OidIsValid(tblspc))
						appendStringInfo(&buf, " USING INDEX TABLESPACE %s",
										 quote_identifier(get_tablespace_name(tblspc)));
				}

				break;
			}
		case CONSTRAINT_CHECK:
			{
				Datum		val;
				bool		isnull;
				char	   *conbin;
				char	   *consrc;
				Node	   *expr;
				List	   *context;

				/* Fetch constraint expression in parsetree form */
				val = heap_getattr(tup, Anum_pg_constraint_conbin,
								   RelationGetDescr(conDesc), &isnull);
				if (isnull)
					elog(ERROR, "null conbin for constraint %u",
						 constraintId);

				conbin = DatumGetCString(DirectFunctionCall1(textout, val));
				expr = stringToNode(conbin);

				/* Set up deparsing context for Var nodes in constraint */
				if (conForm->conrelid != InvalidOid)
				{
					/* relation constraint */
					context = deparse_context_for(get_rel_name(conForm->conrelid),
												  conForm->conrelid);
				}
				else
				{
					/* domain constraint --- can't have Vars */
					context = NIL;
				}

				consrc = deparse_expression_pretty(expr, context, false, false,
												   prettyFlags, 0);

				/*
				 * Now emit the constraint definition.	There are cases where
				 * the constraint expression will be fully parenthesized and
				 * we don't need the outer parens ... but there are other
				 * cases where we do need 'em.  Be conservative for now.
				 *
				 * Note that simply checking for leading '(' and trailing ')'
				 * would NOT be good enough, consider "(x > 0) AND (y > 0)".
				 */
				appendStringInfo(&buf, "CHECK (%s)", consrc);

				break;
			}
		default:
			elog(ERROR, "invalid constraint type \"%c\"", conForm->contype);
			break;
	}

	/* Cleanup */
	systable_endscan(conscan);
	heap_close(conDesc, AccessShareLock);

	return buf.data;
}


/*
 * Convert an int16[] Datum into a comma-separated list of column names
 * for the indicated relation; append the list to buf.
 */
static void
decompile_column_index_array(Datum column_index_array, Oid relId,
							 StringInfo buf)
{
	Datum	   *keys;
	int			nKeys;
	int			j;

	/* Extract data from array of int16 */
	deconstruct_array(DatumGetArrayTypeP(column_index_array),
					  INT2OID, 2, true, 's',
					  &keys, NULL, &nKeys);

	for (j = 0; j < nKeys; j++)
	{
		char	   *colName;

		colName = get_relid_attribute_name(relId, DatumGetInt16(keys[j]));

		if (j == 0)
			appendStringInfoString(buf, quote_identifier(colName));
		else
			appendStringInfo(buf, ", %s", quote_identifier(colName));
	}
}


/* ----------
 * get_expr			- Decompile an expression tree
 *
 * Input: an expression tree in nodeToString form, and a relation OID
 *
 * Output: reverse-listed expression
 *
 * Currently, the expression can only refer to a single relation, namely
 * the one specified by the second parameter.  This is sufficient for
 * partial indexes, column default expressions, etc.
 * ----------
 */
Datum
pg_get_expr(PG_FUNCTION_ARGS)
{
	text	   *expr = PG_GETARG_TEXT_P(0);
	Oid			relid = PG_GETARG_OID(1);
	char	   *relname;

	/* Get the name for the relation */
	relname = get_rel_name(relid);
	if (relname == NULL)
		PG_RETURN_NULL();		/* should we raise an error? */

	PG_RETURN_TEXT_P(string_to_text(pg_get_expr_worker(expr, relid, relname, 0)));
}

Datum
pg_get_expr_ext(PG_FUNCTION_ARGS)
{
	text	   *expr = PG_GETARG_TEXT_P(0);
	Oid			relid = PG_GETARG_OID(1);
	bool		pretty = PG_GETARG_BOOL(2);
	int			prettyFlags;
	char	   *relname;

	prettyFlags = pretty ? PRETTYFLAG_PAREN | PRETTYFLAG_INDENT : 0;

	/* Get the name for the relation */
	relname = get_rel_name(relid);
	if (relname == NULL)
		PG_RETURN_NULL();		/* should we raise an error? */

	PG_RETURN_TEXT_P(string_to_text(pg_get_expr_worker(expr, relid, relname, prettyFlags)));
}

static char *
pg_get_expr_worker(text *expr, Oid relid, char *relname, int prettyFlags)
{
	Node	   *node;
	List	   *context;
	char	   *exprstr;
	char	   *str;

	/* Convert input TEXT object to C string */
	exprstr = DatumGetCString(DirectFunctionCall1(textout,
												  PointerGetDatum(expr)));

	/* Convert expression to node tree */
	node = (Node *) stringToNode(exprstr);

	/* Deparse */
	context = deparse_context_for(relname, relid);
	str = deparse_expression_pretty(node, context, false, false,
									prettyFlags, 0);

	return str;
}


/* ----------
 * get_userbyid			- Get a user name by roleid and
 *				  fallback to 'unknown (OID=n)'
 * ----------
 */
Datum
pg_get_userbyid(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	Name		result;
	HeapTuple	roletup;
	Form_pg_authid role_rec;

	/*
	 * Allocate space for the result
	 */
	result = (Name) palloc(NAMEDATALEN);
	memset(NameStr(*result), 0, NAMEDATALEN);

	/*
	 * Get the pg_authid entry and print the result
	 */
	roletup = SearchSysCache(AUTHOID,
							 ObjectIdGetDatum(roleid),
							 0, 0, 0);
	if (HeapTupleIsValid(roletup))
	{
		role_rec = (Form_pg_authid) GETSTRUCT(roletup);
		StrNCpy(NameStr(*result), NameStr(role_rec->rolname), NAMEDATALEN);
		ReleaseSysCache(roletup);
	}
	else
		sprintf(NameStr(*result), "unknown (OID=%u)", roleid);

	PG_RETURN_NAME(result);
}


/*
 * pg_get_serial_sequence
 *		Get the name of the sequence used by a serial column,
 *		formatted suitably for passing to setval, nextval or currval.
 *		First parameter is not treated as double-quoted, second parameter
 *		is --- see documentation for reason.
 */
Datum
pg_get_serial_sequence(PG_FUNCTION_ARGS)
{
	text	   *tablename = PG_GETARG_TEXT_P(0);
	text	   *columnname = PG_GETARG_TEXT_P(1);
	RangeVar   *tablerv;
	Oid			tableOid;
	char	   *column;
	AttrNumber	attnum;
	Oid			sequenceId = InvalidOid;
	Relation	depRel;
	ScanKeyData key[3];
	SysScanDesc scan;
	HeapTuple	tup;

	/* Get the OID of the table */
	tablerv = makeRangeVarFromNameList(textToQualifiedNameList(tablename));
	tableOid = RangeVarGetRelid(tablerv, false);

	/* Get the number of the column */
	column = DatumGetCString(DirectFunctionCall1(textout,
											   PointerGetDatum(columnname)));

	attnum = get_attnum(tableOid, column);
	if (attnum == InvalidAttrNumber)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						column, tablerv->relname)));

	/* Search the dependency table for the dependent sequence */
	depRel = heap_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(tableOid));
	ScanKeyInit(&key[2],
				Anum_pg_depend_refobjsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(attnum));

	scan = systable_beginscan(depRel, DependReferenceIndexId, true,
							  SnapshotNow, 3, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend deprec = (Form_pg_depend) GETSTRUCT(tup);

		/*
		 * We assume any auto dependency of a sequence on a column must be
		 * what we are looking for.  (We need the relkind test because indexes
		 * can also have auto dependencies on columns.)
		 */
		if (deprec->classid == RelationRelationId &&
			deprec->objsubid == 0 &&
			deprec->deptype == DEPENDENCY_AUTO &&
			get_rel_relkind(deprec->objid) == RELKIND_SEQUENCE)
		{
			sequenceId = deprec->objid;
			break;
		}
	}

	systable_endscan(scan);
	heap_close(depRel, AccessShareLock);

	if (OidIsValid(sequenceId))
	{
		HeapTuple	classtup;
		Form_pg_class classtuple;
		char	   *nspname;
		char	   *result;

		/* Get the sequence's pg_class entry */
		classtup = SearchSysCache(RELOID,
								  ObjectIdGetDatum(sequenceId),
								  0, 0, 0);
		if (!HeapTupleIsValid(classtup))
			elog(ERROR, "cache lookup failed for relation %u", sequenceId);
		classtuple = (Form_pg_class) GETSTRUCT(classtup);

		/* Get the namespace */
		nspname = get_namespace_name(classtuple->relnamespace);
		if (!nspname)
			elog(ERROR, "cache lookup failed for namespace %u",
				 classtuple->relnamespace);

		/* And construct the result string */
		result = quote_qualified_identifier(nspname,
											NameStr(classtuple->relname));

		ReleaseSysCache(classtup);

		PG_RETURN_TEXT_P(string_to_text(result));
	}

	PG_RETURN_NULL();
}


/*
 * get_constraint_index
 *		Given the OID of a unique or primary-key constraint, return the
 *		OID of the underlying unique index.
 *
 * Return InvalidOid if the index couldn't be found; this suggests the
 * given OID is bogus, but we leave it to caller to decide what to do.
 */
static Oid
get_constraint_index(Oid constraintId)
{
	Oid			indexId = InvalidOid;
	Relation	depRel;
	ScanKeyData key[3];
	SysScanDesc scan;
	HeapTuple	tup;

	/* Search the dependency table for the dependent index */
	depRel = heap_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(ConstraintRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(constraintId));
	ScanKeyInit(&key[2],
				Anum_pg_depend_refobjsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(0));

	scan = systable_beginscan(depRel, DependReferenceIndexId, true,
							  SnapshotNow, 3, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend deprec = (Form_pg_depend) GETSTRUCT(tup);

		/*
		 * We assume any internal dependency of an index on the constraint
		 * must be what we are looking for.  (The relkind test is just
		 * paranoia; there shouldn't be any such dependencies otherwise.)
		 */
		if (deprec->classid == RelationRelationId &&
			deprec->objsubid == 0 &&
			deprec->deptype == DEPENDENCY_INTERNAL &&
			get_rel_relkind(deprec->objid) == RELKIND_INDEX)
		{
			indexId = deprec->objid;
			break;
		}
	}

	systable_endscan(scan);
	heap_close(depRel, AccessShareLock);

	return indexId;
}


/*
 * deparse_expression			- General utility for deparsing expressions
 *
 * calls deparse_expression_pretty with all prettyPrinting disabled
 */
char *
deparse_expression(Node *expr, List *dpcontext,
				   bool forceprefix, bool showimplicit)
{
	return deparse_expression_pretty(expr, dpcontext, forceprefix,
									 showimplicit, 0, 0);
}

/* ----------
 * deparse_expression_pretty	- General utility for deparsing expressions
 *
 * expr is the node tree to be deparsed.  It must be a transformed expression
 * tree (ie, not the raw output of gram.y).
 *
 * dpcontext is a list of deparse_namespace nodes representing the context
 * for interpreting Vars in the node tree.
 *
 * forceprefix is TRUE to force all Vars to be prefixed with their table names.
 *
 * showimplicit is TRUE to force all implicit casts to be shown explicitly.
 *
 * tries to pretty up the output according to prettyFlags and startIndent.
 *
 * The result is a palloc'd string.
 * ----------
 */
static char *
deparse_expression_pretty(Node *expr, List *dpcontext,
						  bool forceprefix, bool showimplicit,
						  int prettyFlags, int startIndent)
{
	StringInfoData buf;
	deparse_context context;

	initStringInfo(&buf);
	context.buf = &buf;
	context.namespaces = dpcontext;
	context.varprefix = forceprefix;
	context.prettyFlags = prettyFlags;
	context.indentLevel = startIndent;

	get_rule_expr(expr, &context, showimplicit);

	return buf.data;
}

/* ----------
 * deparse_context_for			- Build deparse context for a single relation
 *
 * Given the reference name (alias) and OID of a relation, build deparsing
 * context for an expression referencing only that relation (as varno 1,
 * varlevelsup 0).	This is sufficient for many uses of deparse_expression.
 * ----------
 */
List *
deparse_context_for(const char *aliasname, Oid relid)
{
	deparse_namespace *dpns;
	RangeTblEntry *rte;

	dpns = (deparse_namespace *) palloc(sizeof(deparse_namespace));

	/* Build a minimal RTE for the rel */
	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_RELATION;
	rte->relid = relid;
	rte->eref = makeAlias(aliasname, NIL);
	rte->inh = false;
	rte->inFromCl = true;

	/* Build one-element rtable */
	dpns->rtable = list_make1(rte);
	dpns->outer_varno = dpns->inner_varno = 0;
	dpns->outer_rte = dpns->inner_rte = NULL;

	/* Return a one-deep namespace stack */
	return list_make1(dpns);
}

/*
 * deparse_context_for_plan		- Build deparse context for a plan node
 *
 * The plan node may contain references to one or two subplans or outer
 * join plan nodes.  For these, pass the varno used plus a context node
 * made with deparse_context_for_subplan.  (Pass 0/NULL for unused inputs.)
 *
 * The plan's rangetable list must also be passed.  We actually prefer to use
 * the rangetable to resolve simple Vars, but the subplan inputs are needed
 * for Vars that reference expressions computed in subplan target lists.
 */
List *
deparse_context_for_plan(int outer_varno, Node *outercontext,
						 int inner_varno, Node *innercontext,
						 List *rtable)
{
	deparse_namespace *dpns;

	dpns = (deparse_namespace *) palloc(sizeof(deparse_namespace));

	dpns->rtable = rtable;
	dpns->outer_varno = outer_varno;
	dpns->outer_rte = (RangeTblEntry *) outercontext;
	dpns->inner_varno = inner_varno;
	dpns->inner_rte = (RangeTblEntry *) innercontext;

	/* Return a one-deep namespace stack */
	return list_make1(dpns);
}

/*
 * deparse_context_for_subplan	- Build deparse context for a plan node
 *
 * Helper routine to build one of the inputs for deparse_context_for_plan.
 * Pass the name to be used to reference the subplan, plus the Plan node.
 * (subplan really ought to be declared as "Plan *", but we use "Node *"
 * to avoid having to include plannodes.h in builtins.h.)
 *
 * The returned node is actually a RangeTblEntry, but we declare it as just
 * Node to discourage callers from assuming anything.
 */
Node *
deparse_context_for_subplan(const char *name, Node *subplan)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);

	/*
	 * We create an RTE_SPECIAL RangeTblEntry, and store the subplan in its
	 * funcexpr field.	RTE_SPECIAL nodes shouldn't appear in deparse contexts
	 * otherwise.
	 */
	rte->rtekind = RTE_SPECIAL;
	rte->relid = InvalidOid;
	rte->funcexpr = subplan;
	rte->alias = NULL;
	rte->eref = makeAlias(name, NIL);
	rte->inh = false;
	rte->inFromCl = true;

	return (Node *) rte;
}

/* ----------
 * make_ruledef			- reconstruct the CREATE RULE command
 *				  for a given pg_rewrite tuple
 * ----------
 */
static void
make_ruledef(StringInfo buf, HeapTuple ruletup, TupleDesc rulettc,
			 int prettyFlags)
{
	char	   *rulename;
	char		ev_type;
	Oid			ev_class;
	int2		ev_attr;
	bool		is_instead;
	char	   *ev_qual;
	char	   *ev_action;
	List	   *actions = NIL;
	int			fno;
	Datum		dat;
	bool		isnull;

	/*
	 * Get the attribute values from the rules tuple
	 */
	fno = SPI_fnumber(rulettc, "rulename");
	dat = SPI_getbinval(ruletup, rulettc, fno, &isnull);
	Assert(!isnull);
	rulename = NameStr(*(DatumGetName(dat)));

	fno = SPI_fnumber(rulettc, "ev_type");
	dat = SPI_getbinval(ruletup, rulettc, fno, &isnull);
	Assert(!isnull);
	ev_type = DatumGetChar(dat);

	fno = SPI_fnumber(rulettc, "ev_class");
	dat = SPI_getbinval(ruletup, rulettc, fno, &isnull);
	Assert(!isnull);
	ev_class = DatumGetObjectId(dat);

	fno = SPI_fnumber(rulettc, "ev_attr");
	dat = SPI_getbinval(ruletup, rulettc, fno, &isnull);
	Assert(!isnull);
	ev_attr = DatumGetInt16(dat);

	fno = SPI_fnumber(rulettc, "is_instead");
	dat = SPI_getbinval(ruletup, rulettc, fno, &isnull);
	Assert(!isnull);
	is_instead = DatumGetBool(dat);

	/* these could be nulls */
	fno = SPI_fnumber(rulettc, "ev_qual");
	ev_qual = SPI_getvalue(ruletup, rulettc, fno);

	fno = SPI_fnumber(rulettc, "ev_action");
	ev_action = SPI_getvalue(ruletup, rulettc, fno);
	if (ev_action != NULL)
		actions = (List *) stringToNode(ev_action);

	/*
	 * Build the rules definition text
	 */
	appendStringInfo(buf, "CREATE RULE %s AS",
					 quote_identifier(rulename));

	if (prettyFlags & PRETTYFLAG_INDENT)
		appendStringInfoString(buf, "\n    ON ");
	else
		appendStringInfoString(buf, " ON ");

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
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("rule \"%s\" has unsupported event type %d",
							rulename, ev_type)));
			break;
	}

	/* The relation the rule is fired on */
	appendStringInfo(buf, " TO %s", generate_relation_name(ev_class));
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

		if (prettyFlags & PRETTYFLAG_INDENT)
			appendStringInfoString(buf, "\n  ");
		appendStringInfo(buf, " WHERE ");

		qual = stringToNode(ev_qual);

		/*
		 * We need to make a context for recognizing any Vars in the qual
		 * (which can only be references to OLD and NEW).  Use the rtable of
		 * the first query in the action list for this purpose.
		 */
		query = (Query *) linitial(actions);

		/*
		 * If the action is INSERT...SELECT, OLD/NEW have been pushed down
		 * into the SELECT, and that's what we need to look at. (Ugly kluge
		 * ... try to fix this when we redesign querytrees.)
		 */
		query = getInsertSelectQuery(query, NULL);

		/* Must acquire locks right away; see notes in get_query_def() */
		AcquireRewriteLocks(query);

		context.buf = buf;
		context.namespaces = list_make1(&dpns);
		context.varprefix = (list_length(query->rtable) != 1);
		context.prettyFlags = prettyFlags;
		context.indentLevel = PRETTYINDENT_STD;
		dpns.rtable = query->rtable;
		dpns.outer_varno = dpns.inner_varno = 0;
		dpns.outer_rte = dpns.inner_rte = NULL;

		get_rule_expr(qual, &context, false);
	}

	appendStringInfo(buf, " DO ");

	/* The INSTEAD keyword (if so) */
	if (is_instead)
		appendStringInfo(buf, "INSTEAD ");

	/* Finally the rules actions */
	if (list_length(actions) > 1)
	{
		ListCell   *action;
		Query	   *query;

		appendStringInfo(buf, "(");
		foreach(action, actions)
		{
			query = (Query *) lfirst(action);
			get_query_def(query, buf, NIL, NULL, prettyFlags, 0);
			if (prettyFlags)
				appendStringInfo(buf, ";\n");
			else
				appendStringInfo(buf, "; ");
		}
		appendStringInfo(buf, ");");
	}
	else if (list_length(actions) == 0)
	{
		appendStringInfo(buf, "NOTHING;");
	}
	else
	{
		Query	   *query;

		query = (Query *) linitial(actions);
		get_query_def(query, buf, NIL, NULL, prettyFlags, 0);
		appendStringInfo(buf, ";");
	}
}


/* ----------
 * make_viewdef			- reconstruct the SELECT part of a
 *				  view rewrite rule
 * ----------
 */
static void
make_viewdef(StringInfo buf, HeapTuple ruletup, TupleDesc rulettc,
			 int prettyFlags)
{
	Query	   *query;
	char		ev_type;
	Oid			ev_class;
	int2		ev_attr;
	bool		is_instead;
	char	   *ev_qual;
	char	   *ev_action;
	List	   *actions = NIL;
	Relation	ev_relation;
	int			fno;
	bool		isnull;

	/*
	 * Get the attribute values from the rules tuple
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

	if (list_length(actions) != 1)
	{
		appendStringInfo(buf, "Not a view");
		return;
	}

	query = (Query *) linitial(actions);

	if (ev_type != '1' || ev_attr >= 0 || !is_instead ||
		strcmp(ev_qual, "<>") != 0 || query->commandType != CMD_SELECT)
	{
		appendStringInfo(buf, "Not a view");
		return;
	}

	ev_relation = heap_open(ev_class, AccessShareLock);

	get_query_def(query, buf, NIL, RelationGetDescr(ev_relation),
				  prettyFlags, 0);
	appendStringInfo(buf, ";");

	heap_close(ev_relation, AccessShareLock);
}


/* ----------
 * get_query_def			- Parse back one query parsetree
 *
 * If resultDesc is not NULL, then it is the output tuple descriptor for
 * the view represented by a SELECT query.
 * ----------
 */
static void
get_query_def(Query *query, StringInfo buf, List *parentnamespace,
			  TupleDesc resultDesc, int prettyFlags, int startIndent)
{
	deparse_context context;
	deparse_namespace dpns;

	/*
	 * Before we begin to examine the query, acquire locks on referenced
	 * relations, and fix up deleted columns in JOIN RTEs.	This ensures
	 * consistent results.	Note we assume it's OK to scribble on the passed
	 * querytree!
	 */
	AcquireRewriteLocks(query);

	context.buf = buf;
	context.namespaces = lcons(&dpns, list_copy(parentnamespace));
	context.varprefix = (parentnamespace != NIL ||
						 list_length(query->rtable) != 1);
	context.prettyFlags = prettyFlags;
	context.indentLevel = startIndent;

	dpns.rtable = query->rtable;
	dpns.outer_varno = dpns.inner_varno = 0;
	dpns.outer_rte = dpns.inner_rte = NULL;

	switch (query->commandType)
	{
		case CMD_SELECT:
			get_select_query_def(query, &context, resultDesc);
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
			elog(ERROR, "unrecognized query command type: %d",
				 query->commandType);
			break;
	}
}

/* ----------
 * get_values_def			- Parse back a VALUES list
 * ----------
 */
static void
get_values_def(List *values_lists, deparse_context *context)
{
	StringInfo	buf = context->buf;
	bool		first_list = true;
	ListCell   *vtl;

	appendStringInfoString(buf, "VALUES ");

	foreach(vtl, values_lists)
	{
		List	   *sublist = (List *) lfirst(vtl);
		bool		first_col = true;
		ListCell   *lc;

		if (first_list)
			first_list = false;
		else
			appendStringInfoString(buf, ", ");

		appendStringInfoChar(buf, '(');
		foreach(lc, sublist)
		{
			Node	   *col = (Node *) lfirst(lc);

			if (first_col)
				first_col = false;
			else
				appendStringInfoChar(buf, ',');

			/*
			 * Strip any top-level nodes representing indirection assignments,
			 * then print the result.
			 */
			get_rule_expr(processIndirection(col, context, false),
						  context, false);
		}
		appendStringInfoChar(buf, ')');
	}
}

/* ----------
 * get_select_query_def			- Parse back a SELECT parsetree
 * ----------
 */
static void
get_select_query_def(Query *query, deparse_context *context,
					 TupleDesc resultDesc)
{
	StringInfo	buf = context->buf;
	bool		force_colno;
	char	   *sep;
	ListCell   *l;

	/*
	 * If the Query node has a setOperations tree, then it's the top level of
	 * a UNION/INTERSECT/EXCEPT query; only the ORDER BY and LIMIT fields are
	 * interesting in the top query itself.
	 */
	if (query->setOperations)
	{
		get_setop_query(query->setOperations, query, context, resultDesc);
		/* ORDER BY clauses must be simple in this case */
		force_colno = true;
	}
	else
	{
		get_basic_select_query(query, context, resultDesc);
		force_colno = false;
	}

	/* Add the ORDER BY clause if given */
	if (query->sortClause != NIL)
	{
		appendContextKeyword(context, " ORDER BY ",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 1);
		sep = "";
		foreach(l, query->sortClause)
		{
			SortClause *srt = (SortClause *) lfirst(l);
			Node	   *sortexpr;
			Oid			sortcoltype;
			TypeCacheEntry *typentry;

			appendStringInfoString(buf, sep);
			sortexpr = get_rule_sortgroupclause(srt, query->targetList,
												force_colno, context);
			sortcoltype = exprType(sortexpr);
			/* See whether operator is default < or > for datatype */
			typentry = lookup_type_cache(sortcoltype,
										 TYPECACHE_LT_OPR | TYPECACHE_GT_OPR);
			if (srt->sortop == typentry->lt_opr)
				 /* ASC is default, so emit nothing */ ;
			else if (srt->sortop == typentry->gt_opr)
				appendStringInfo(buf, " DESC");
			else
				appendStringInfo(buf, " USING %s",
								 generate_operator_name(srt->sortop,
														sortcoltype,
														sortcoltype));
			sep = ", ";
		}
	}

	/* Add the LIMIT clause if given */
	if (query->limitOffset != NULL)
	{
		appendContextKeyword(context, " OFFSET ",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 0);
		get_rule_expr(query->limitOffset, context, false);
	}
	if (query->limitCount != NULL)
	{
		appendContextKeyword(context, " LIMIT ",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 0);
		if (IsA(query->limitCount, Const) &&
			((Const *) query->limitCount)->constisnull)
			appendStringInfo(buf, "ALL");
		else
			get_rule_expr(query->limitCount, context, false);
	}

	/* Add FOR UPDATE/SHARE clauses if present */
	foreach(l, query->rowMarks)
	{
		RowMarkClause *rc = (RowMarkClause *) lfirst(l);
		RangeTblEntry *rte = rt_fetch(rc->rti, query->rtable);

		if (rc->forUpdate)
			appendContextKeyword(context, " FOR UPDATE",
								 -PRETTYINDENT_STD, PRETTYINDENT_STD, 0);
		else
			appendContextKeyword(context, " FOR SHARE",
								 -PRETTYINDENT_STD, PRETTYINDENT_STD, 0);
		appendStringInfo(buf, " OF %s",
						 quote_identifier(rte->eref->aliasname));
		if (rc->noWait)
			appendStringInfo(buf, " NOWAIT");
	}
}

static void
get_basic_select_query(Query *query, deparse_context *context,
					   TupleDesc resultDesc)
{
	StringInfo	buf = context->buf;
	char	   *sep;
	ListCell   *l;

	if (PRETTY_INDENT(context))
	{
		context->indentLevel += PRETTYINDENT_STD;
		appendStringInfoChar(buf, ' ');
	}

	/*
	 * If the query looks like SELECT * FROM (VALUES ...), then print just the
	 * VALUES part.  This reverses what transformValuesClause() did at parse
	 * time.  If the jointree contains just a single VALUES RTE, we assume
	 * this case applies (without looking at the targetlist...)
	 */
	if (list_length(query->jointree->fromlist) == 1)
	{
		RangeTblRef *rtr = (RangeTblRef *) linitial(query->jointree->fromlist);

		if (IsA(rtr, RangeTblRef))
		{
			RangeTblEntry *rte = rt_fetch(rtr->rtindex, query->rtable);

			if (rte->rtekind == RTE_VALUES)
			{
				get_values_def(rte->values_lists, context);
				return;
			}
		}
	}

	/*
	 * Build up the query string - first we say SELECT
	 */
	appendStringInfo(buf, "SELECT");

	/* Add the DISTINCT clause if given */
	if (query->distinctClause != NIL)
	{
		if (has_distinct_on_clause(query))
		{
			appendStringInfo(buf, " DISTINCT ON (");
			sep = "";
			foreach(l, query->distinctClause)
			{
				SortClause *srt = (SortClause *) lfirst(l);

				appendStringInfoString(buf, sep);
				get_rule_sortgroupclause(srt, query->targetList,
										 false, context);
				sep = ", ";
			}
			appendStringInfo(buf, ")");
		}
		else
			appendStringInfo(buf, " DISTINCT");
	}

	/* Then we tell what to select (the targetlist) */
	get_target_list(query->targetList, context, resultDesc);

	/* Add the FROM clause if needed */
	get_from_clause(query, " FROM ", context);

	/* Add the WHERE clause if given */
	if (query->jointree->quals != NULL)
	{
		appendContextKeyword(context, " WHERE ",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 1);
		get_rule_expr(query->jointree->quals, context, false);
	}

	/* Add the GROUP BY clause if given */
	if (query->groupClause != NULL)
	{
		appendContextKeyword(context, " GROUP BY ",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 1);
		sep = "";
		foreach(l, query->groupClause)
		{
			GroupClause *grp = (GroupClause *) lfirst(l);

			appendStringInfoString(buf, sep);
			get_rule_sortgroupclause(grp, query->targetList,
									 false, context);
			sep = ", ";
		}
	}

	/* Add the HAVING clause if given */
	if (query->havingQual != NULL)
	{
		appendContextKeyword(context, " HAVING ",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 0);
		get_rule_expr(query->havingQual, context, false);
	}
}

/* ----------
 * get_target_list			- Parse back a SELECT target list
 *
 * This is also used for RETURNING lists in INSERT/UPDATE/DELETE.
 * ----------
 */
static void
get_target_list(List *targetList, deparse_context *context,
				TupleDesc resultDesc)
{
	StringInfo	buf = context->buf;
	char	   *sep;
	int			colno;
	ListCell   *l;

	sep = " ";
	colno = 0;
	foreach(l, targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);
		char	   *colname;
		char	   *attname;

		if (tle->resjunk)
			continue;			/* ignore junk entries */

		appendStringInfoString(buf, sep);
		sep = ", ";
		colno++;

		/*
		 * We special-case Var nodes rather than using get_rule_expr. This is
		 * needed because get_rule_expr will display a whole-row Var as
		 * "foo.*", which is the preferred notation in most contexts, but at
		 * the top level of a SELECT list it's not right (the parser will
		 * expand that notation into multiple columns, yielding behavior
		 * different from a whole-row Var).  We want just "foo", instead.
		 */
		if (tle->expr && IsA(tle->expr, Var))
		{
			Var		   *var = (Var *) (tle->expr);
			char	   *schemaname;
			char	   *refname;

			get_names_for_var(var, 0, context,
							  &schemaname, &refname, &attname);
			if (refname && (context->varprefix || attname == NULL))
			{
				if (schemaname)
					appendStringInfo(buf, "%s.",
									 quote_identifier(schemaname));

				if (strcmp(refname, "*NEW*") == 0)
					appendStringInfoString(buf, "new");
				else if (strcmp(refname, "*OLD*") == 0)
					appendStringInfoString(buf, "old");
				else
					appendStringInfoString(buf, quote_identifier(refname));

				if (attname)
					appendStringInfoChar(buf, '.');
			}
			if (attname)
				appendStringInfoString(buf, quote_identifier(attname));
			else
			{
				/*
				 * In the whole-row Var case, refname is what the default AS
				 * name would be.
				 */
				attname = refname;
			}
		}
		else
		{
			get_rule_expr((Node *) tle->expr, context, true);
			/* We'll show the AS name unless it's this: */
			attname = "?column?";
		}

		/*
		 * Figure out what the result column should be called.	In the context
		 * of a view, use the view's tuple descriptor (so as to pick up the
		 * effects of any column RENAME that's been done on the view).
		 * Otherwise, just use what we can find in the TLE.
		 */
		if (resultDesc && colno <= resultDesc->natts)
			colname = NameStr(resultDesc->attrs[colno - 1]->attname);
		else
			colname = tle->resname;

		/* Show AS unless the column's name is correct as-is */
		if (colname)			/* resname could be NULL */
		{
			if (attname == NULL || strcmp(attname, colname) != 0)
				appendStringInfo(buf, " AS %s", quote_identifier(colname));
		}
	}
}

static void
get_setop_query(Node *setOp, Query *query, deparse_context *context,
				TupleDesc resultDesc)
{
	StringInfo	buf = context->buf;
	bool		need_paren;

	if (IsA(setOp, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) setOp;
		RangeTblEntry *rte = rt_fetch(rtr->rtindex, query->rtable);
		Query	   *subquery = rte->subquery;

		Assert(subquery != NULL);
		Assert(subquery->setOperations == NULL);
		/* Need parens if ORDER BY, FOR UPDATE, or LIMIT; see gram.y */
		need_paren = (subquery->sortClause ||
					  subquery->rowMarks ||
					  subquery->limitOffset ||
					  subquery->limitCount);
		if (need_paren)
			appendStringInfoChar(buf, '(');
		get_query_def(subquery, buf, context->namespaces, resultDesc,
					  context->prettyFlags, context->indentLevel);
		if (need_paren)
			appendStringInfoChar(buf, ')');
	}
	else if (IsA(setOp, SetOperationStmt))
	{
		SetOperationStmt *op = (SetOperationStmt *) setOp;

		/*
		 * We force parens whenever nesting two SetOperationStmts. There are
		 * some cases in which parens are needed around a leaf query too, but
		 * those are more easily handled at the next level down (see code
		 * above).
		 */
		need_paren = !IsA(op->larg, RangeTblRef);

		if (need_paren)
			appendStringInfoChar(buf, '(');
		get_setop_query(op->larg, query, context, resultDesc);
		if (need_paren)
			appendStringInfoChar(buf, ')');

		if (!PRETTY_INDENT(context))
			appendStringInfoChar(buf, ' ');
		switch (op->op)
		{
			case SETOP_UNION:
				appendContextKeyword(context, "UNION ",
									 -PRETTYINDENT_STD, PRETTYINDENT_STD, 0);
				break;
			case SETOP_INTERSECT:
				appendContextKeyword(context, "INTERSECT ",
									 -PRETTYINDENT_STD, PRETTYINDENT_STD, 0);
				break;
			case SETOP_EXCEPT:
				appendContextKeyword(context, "EXCEPT ",
									 -PRETTYINDENT_STD, PRETTYINDENT_STD, 0);
				break;
			default:
				elog(ERROR, "unrecognized set op: %d",
					 (int) op->op);
		}
		if (op->all)
			appendStringInfo(buf, "ALL ");

		if (PRETTY_INDENT(context))
			appendContextKeyword(context, "", 0, 0, 0);

		need_paren = !IsA(op->rarg, RangeTblRef);

		if (need_paren)
			appendStringInfoChar(buf, '(');
		get_setop_query(op->rarg, query, context, resultDesc);
		if (need_paren)
			appendStringInfoChar(buf, ')');
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(setOp));
	}
}

/*
 * Display a sort/group clause.
 *
 * Also returns the expression tree, so caller need not find it again.
 */
static Node *
get_rule_sortgroupclause(SortClause *srt, List *tlist, bool force_colno,
						 deparse_context *context)
{
	StringInfo	buf = context->buf;
	TargetEntry *tle;
	Node	   *expr;

	tle = get_sortgroupclause_tle(srt, tlist);
	expr = (Node *) tle->expr;

	/*
	 * Use column-number form if requested by caller.  Otherwise, if
	 * expression is a constant, force it to be dumped with an explicit
	 * cast as decoration --- this is because a simple integer constant
	 * is ambiguous (and will be misinterpreted by findTargetlistEntry())
	 * if we dump it without any decoration.  Otherwise, just dump the
	 * expression normally.
	 */
	if (force_colno)
	{
		Assert(!tle->resjunk);
		appendStringInfo(buf, "%d", tle->resno);
	}
	else if (expr && IsA(expr, Const))
		get_const_expr((Const *) expr, context, 1);
	else
		get_rule_expr(expr, context, true);

	return expr;
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
	RangeTblEntry *values_rte = NULL;
	RangeTblEntry *rte;
	char	   *sep;
	ListCell   *values_cell;
	ListCell   *l;
	List	   *strippedexprs;

	/*
	 * If it's an INSERT ... SELECT or VALUES (...), (...), ... there will be
	 * a single RTE for the SELECT or VALUES.
	 */
	foreach(l, query->rtable)
	{
		rte = (RangeTblEntry *) lfirst(l);

		if (rte->rtekind == RTE_SUBQUERY)
		{
			if (select_rte)
				elog(ERROR, "too many subquery RTEs in INSERT");
			select_rte = rte;
		}

		if (rte->rtekind == RTE_VALUES)
		{
			if (values_rte)
				elog(ERROR, "too many values RTEs in INSERT");
			values_rte = rte;
		}
	}
	if (select_rte && values_rte)
		elog(ERROR, "both subquery and values RTEs in INSERT");

	/*
	 * Start the query with INSERT INTO relname
	 */
	rte = rt_fetch(query->resultRelation, query->rtable);
	Assert(rte->rtekind == RTE_RELATION);

	if (PRETTY_INDENT(context))
	{
		context->indentLevel += PRETTYINDENT_STD;
		appendStringInfoChar(buf, ' ');
	}
	appendStringInfo(buf, "INSERT INTO %s (",
					 generate_relation_name(rte->relid));

	/*
	 * Add the insert-column-names list.  To handle indirection properly, we
	 * need to look for indirection nodes in the top targetlist (if it's
	 * INSERT ... SELECT or INSERT ... single VALUES), or in the first
	 * expression list of the VALUES RTE (if it's INSERT ... multi VALUES). We
	 * assume that all the expression lists will have similar indirection in
	 * the latter case.
	 */
	if (values_rte)
		values_cell = list_head((List *) linitial(values_rte->values_lists));
	else
		values_cell = NULL;
	strippedexprs = NIL;
	sep = "";
	foreach(l, query->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->resjunk)
			continue;			/* ignore junk entries */

		appendStringInfoString(buf, sep);
		sep = ", ";

		/*
		 * Put out name of target column; look in the catalogs, not at
		 * tle->resname, since resname will fail to track RENAME.
		 */
		appendStringInfoString(buf,
						quote_identifier(get_relid_attribute_name(rte->relid,
															   tle->resno)));

		/*
		 * Print any indirection needed (subfields or subscripts), and strip
		 * off the top-level nodes representing the indirection assignments.
		 */
		if (values_cell)
		{
			/* we discard the stripped expression in this case */
			processIndirection((Node *) lfirst(values_cell), context, true);
			values_cell = lnext(values_cell);
		}
		else
		{
			/* we keep a list of the stripped expressions in this case */
			strippedexprs = lappend(strippedexprs,
									processIndirection((Node *) tle->expr,
													   context, true));
		}
	}
	appendStringInfo(buf, ") ");

	if (select_rte)
	{
		/* Add the SELECT */
		get_query_def(select_rte->subquery, buf, NIL, NULL,
					  context->prettyFlags, context->indentLevel);
	}
	else if (values_rte)
	{
		/* Add the multi-VALUES expression lists */
		get_values_def(values_rte->values_lists, context);
	}
	else
	{
		/* Add the single-VALUES expression list */
		appendContextKeyword(context, "VALUES (",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 2);
		get_rule_expr((Node *) strippedexprs, context, false);
		appendStringInfoChar(buf, ')');
	}

	/* Add RETURNING if present */
	if (query->returningList)
	{
		appendContextKeyword(context, " RETURNING",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 1);
		get_target_list(query->returningList, context, NULL);
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
	ListCell   *l;

	/*
	 * Start the query with UPDATE relname SET
	 */
	rte = rt_fetch(query->resultRelation, query->rtable);
	Assert(rte->rtekind == RTE_RELATION);
	if (PRETTY_INDENT(context))
	{
		appendStringInfoChar(buf, ' ');
		context->indentLevel += PRETTYINDENT_STD;
	}
	appendStringInfo(buf, "UPDATE %s%s",
					 only_marker(rte),
					 generate_relation_name(rte->relid));
	if (rte->alias != NULL)
		appendStringInfo(buf, " %s",
						 quote_identifier(rte->alias->aliasname));
	appendStringInfoString(buf, " SET ");

	/* Add the comma separated list of 'attname = value' */
	sep = "";
	foreach(l, query->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);
		Node	   *expr;

		if (tle->resjunk)
			continue;			/* ignore junk entries */

		appendStringInfoString(buf, sep);
		sep = ", ";

		/*
		 * Put out name of target column; look in the catalogs, not at
		 * tle->resname, since resname will fail to track RENAME.
		 */
		appendStringInfoString(buf,
						quote_identifier(get_relid_attribute_name(rte->relid,
															   tle->resno)));

		/*
		 * Print any indirection needed (subfields or subscripts), and strip
		 * off the top-level nodes representing the indirection assignments.
		 */
		expr = processIndirection((Node *) tle->expr, context, true);

		appendStringInfo(buf, " = ");

		get_rule_expr(expr, context, false);
	}

	/* Add the FROM clause if needed */
	get_from_clause(query, " FROM ", context);

	/* Add a WHERE clause if given */
	if (query->jointree->quals != NULL)
	{
		appendContextKeyword(context, " WHERE ",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 1);
		get_rule_expr(query->jointree->quals, context, false);
	}

	/* Add RETURNING if present */
	if (query->returningList)
	{
		appendContextKeyword(context, " RETURNING",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 1);
		get_target_list(query->returningList, context, NULL);
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

	/*
	 * Start the query with DELETE FROM relname
	 */
	rte = rt_fetch(query->resultRelation, query->rtable);
	Assert(rte->rtekind == RTE_RELATION);
	if (PRETTY_INDENT(context))
	{
		appendStringInfoChar(buf, ' ');
		context->indentLevel += PRETTYINDENT_STD;
	}
	appendStringInfo(buf, "DELETE FROM %s%s",
					 only_marker(rte),
					 generate_relation_name(rte->relid));
	if (rte->alias != NULL)
		appendStringInfo(buf, " %s",
						 quote_identifier(rte->alias->aliasname));

	/* Add the USING clause if given */
	get_from_clause(query, " USING ", context);

	/* Add a WHERE clause if given */
	if (query->jointree->quals != NULL)
	{
		appendContextKeyword(context, " WHERE ",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 1);
		get_rule_expr(query->jointree->quals, context, false);
	}

	/* Add RETURNING if present */
	if (query->returningList)
	{
		appendContextKeyword(context, " RETURNING",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 1);
		get_target_list(query->returningList, context, NULL);
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

		appendContextKeyword(context, "",
							 0, PRETTYINDENT_STD, 1);
		appendStringInfo(buf, "NOTIFY %s",
					   quote_qualified_identifier(stmt->relation->schemaname,
												  stmt->relation->relname));
	}
	else
	{
		/* Currently only NOTIFY utility commands can appear in rules */
		elog(ERROR, "unexpected utility statement type");
	}
}


/*
 * Get the RTE referenced by a (possibly nonlocal) Var.
 *
 * The appropriate attribute number is stored into *attno
 * (do not assume that var->varattno is what to use).
 *
 * In some cases (currently only when recursing into an unnamed join)
 * the Var's varlevelsup has to be interpreted with respect to a context
 * above the current one; levelsup indicates the offset.
 */
static RangeTblEntry *
get_rte_for_var(Var *var, int levelsup, deparse_context *context,
				AttrNumber *attno)
{
	RangeTblEntry *rte;
	int			netlevelsup;
	deparse_namespace *dpns;

	/* default assumption */
	*attno = var->varattno;

	/* Find appropriate nesting depth */
	netlevelsup = var->varlevelsup + levelsup;
	if (netlevelsup >= list_length(context->namespaces))
		elog(ERROR, "bogus varlevelsup: %d offset %d",
			 var->varlevelsup, levelsup);
	dpns = (deparse_namespace *) list_nth(context->namespaces,
										  netlevelsup);

	/*
	 * Try to find the relevant RTE in this rtable.  In a plan tree, it's
	 * likely that varno is OUTER, INNER, or 0, in which case we try to use
	 * varnoold instead.  If the Var references an expression computed by a
	 * subplan, varnoold will be 0, and we fall back to looking at the special
	 * subplan RTEs.
	 */
	if (var->varno >= 1 && var->varno <= list_length(dpns->rtable))
		rte = rt_fetch(var->varno, dpns->rtable);
	else if (var->varnoold >= 1 && var->varnoold <= list_length(dpns->rtable))
	{
		rte = rt_fetch(var->varnoold, dpns->rtable);
		*attno = var->varoattno;
	}
	else if (var->varno == dpns->outer_varno)
		rte = dpns->outer_rte;
	else if (var->varno == dpns->inner_varno)
		rte = dpns->inner_rte;
	else
		rte = NULL;
	if (rte == NULL)
		elog(ERROR, "bogus varno: %d", var->varno);
	return rte;
}


/*
 * Get the schemaname, refname and attname for a (possibly nonlocal) Var.
 *
 * In some cases (currently only when recursing into an unnamed join)
 * the Var's varlevelsup has to be interpreted with respect to a context
 * above the current one; levelsup indicates the offset.
 *
 * schemaname is usually returned as NULL.	It will be non-null only if
 * use of the unqualified refname would find the wrong RTE.
 *
 * refname will be returned as NULL if the Var references an unnamed join.
 * In this case the Var *must* be displayed without any qualification.
 *
 * attname will be returned as NULL if the Var represents a whole tuple
 * of the relation.  (Typically we'd want to display the Var as "foo.*",
 * but it's convenient to return NULL to make it easier for callers to
 * distinguish this case.)
 */
static void
get_names_for_var(Var *var, int levelsup, deparse_context *context,
				  char **schemaname, char **refname, char **attname)
{
	RangeTblEntry *rte;
	AttrNumber	attnum;

	/* Find appropriate RTE */
	rte = get_rte_for_var(var, levelsup, context, &attnum);

	/* Emit results */
	*schemaname = NULL;			/* default assumptions */
	*refname = rte->eref->aliasname;

	/* Exceptions occur only if the RTE is alias-less */
	if (rte->alias == NULL)
	{
		if (rte->rtekind == RTE_RELATION)
		{
			/*
			 * It's possible that use of the bare refname would find another
			 * more-closely-nested RTE, or be ambiguous, in which case we need
			 * to specify the schemaname to avoid these errors.
			 */
			if (find_rte_by_refname(rte->eref->aliasname, context) != rte)
				*schemaname =
					get_namespace_name(get_rel_namespace(rte->relid));
		}
		else if (rte->rtekind == RTE_JOIN)
		{
			/*
			 * If it's an unnamed join, look at the expansion of the alias
			 * variable.  If it's a simple reference to one of the input vars
			 * then recursively find the name of that var, instead. (This
			 * allows correct decompiling of cases where there are identically
			 * named columns on both sides of the join.) When it's not a
			 * simple reference, we have to just return the unqualified
			 * variable name (this can only happen with columns that were
			 * merged by USING or NATURAL clauses).
			 */
			if (attnum > 0)
			{
				Var		   *aliasvar;

				aliasvar = (Var *) list_nth(rte->joinaliasvars, attnum - 1);
				if (IsA(aliasvar, Var))
				{
					get_names_for_var(aliasvar,
									  var->varlevelsup + levelsup, context,
									  schemaname, refname, attname);
					return;
				}
			}
			/* Unnamed join has neither schemaname nor refname */
			*refname = NULL;
		}
		else if (rte->rtekind == RTE_SPECIAL)
		{
			/*
			 * This case occurs during EXPLAIN when we are looking at a
			 * deparse context node set up by deparse_context_for_subplan().
			 * If the subplan tlist provides a name, use it, but usually we'll
			 * end up with "?columnN?".
			 */
			List	   *tlist = ((Plan *) rte->funcexpr)->targetlist;
			TargetEntry *tle = get_tle_by_resno(tlist, attnum);

			if (tle && tle->resname)
			{
				*attname = tle->resname;
			}
			else
			{
				char		buf[32];

				snprintf(buf, sizeof(buf), "?column%d?", attnum);
				*attname = pstrdup(buf);
			}
			return;
		}
	}

	if (attnum == InvalidAttrNumber)
		*attname = NULL;
	else
		*attname = get_rte_attribute_name(rte, attnum);
}


/*
 * Get the name of a field of a Var of type RECORD.
 *
 * Since no actual table or view column is allowed to have type RECORD, such
 * a Var must refer to a JOIN or FUNCTION RTE or to a subquery output.	We
 * drill down to find the ultimate defining expression and attempt to infer
 * the field name from it.	We ereport if we can't determine the name.
 *
 * levelsup is an extra offset to interpret the Var's varlevelsup correctly.
 *
 * Note: this has essentially the same logic as the parser's
 * expandRecordVariable() function, but we are dealing with a different
 * representation of the input context, and we only need one field name not
 * a TupleDesc.  Also, we have a special case for RTE_SPECIAL so that we can
 * deal with displaying RECORD-returning functions in subplan targetlists.
 */
static const char *
get_name_for_var_field(Var *var, int fieldno,
					   int levelsup, deparse_context *context)
{
	RangeTblEntry *rte;
	AttrNumber	attnum;
	TupleDesc	tupleDesc;
	Node	   *expr;

	/* Check my caller didn't mess up */
	Assert(IsA(var, Var));
	Assert(var->vartype == RECORDOID);

	/* Find appropriate RTE */
	rte = get_rte_for_var(var, levelsup, context, &attnum);

	if (attnum == InvalidAttrNumber)
	{
		/* Var is whole-row reference to RTE, so select the right field */
		return get_rte_attribute_name(rte, fieldno);
	}

	expr = (Node *) var;		/* default if we can't drill down */

	switch (rte->rtekind)
	{
		case RTE_RELATION:
		case RTE_VALUES:

			/*
			 * This case should not occur: a column of a table or values list
			 * shouldn't have type RECORD.  Fall through and fail (most
			 * likely) at the bottom.
			 */
			break;
		case RTE_SUBQUERY:
			{
				/* Subselect-in-FROM: examine sub-select's output expr */
				TargetEntry *ste = get_tle_by_resno(rte->subquery->targetList,
													attnum);

				if (ste == NULL || ste->resjunk)
					elog(ERROR, "subquery %s does not have attribute %d",
						 rte->eref->aliasname, attnum);
				expr = (Node *) ste->expr;
				if (IsA(expr, Var))
				{
					/*
					 * Recurse into the sub-select to see what its Var refers
					 * to.	We have to build an additional level of namespace
					 * to keep in step with varlevelsup in the subselect.
					 */
					deparse_namespace mydpns;
					const char *result;

					mydpns.rtable = rte->subquery->rtable;
					mydpns.outer_varno = mydpns.inner_varno = 0;
					mydpns.outer_rte = mydpns.inner_rte = NULL;

					context->namespaces = lcons(&mydpns, context->namespaces);

					result = get_name_for_var_field((Var *) expr, fieldno,
													0, context);

					context->namespaces = list_delete_first(context->namespaces);

					return result;
				}
				/* else fall through to inspect the expression */
			}
			break;
		case RTE_JOIN:
			/* Join RTE --- recursively inspect the alias variable */
			Assert(attnum > 0 && attnum <= list_length(rte->joinaliasvars));
			expr = (Node *) list_nth(rte->joinaliasvars, attnum - 1);
			if (IsA(expr, Var))
				return get_name_for_var_field((Var *) expr, fieldno,
											  var->varlevelsup + levelsup,
											  context);
			/* else fall through to inspect the expression */
			break;
		case RTE_FUNCTION:

			/*
			 * We couldn't get here unless a function is declared with one of
			 * its result columns as RECORD, which is not allowed.
			 */
			break;
		case RTE_SPECIAL:
			{
				/*
				 * We are looking at a deparse context node set up by
				 * deparse_context_for_subplan().  The Var must refer to an
				 * expression computed by this subplan (or possibly one of its
				 * inputs), rather than any simple attribute of an RTE entry.
				 * Look into the subplan's target list to get the referenced
				 * expression, digging down as far as needed to find something
				 * that's not a Var, and then pass it to
				 * get_expr_result_type().
				 */
				Plan	   *subplan = (Plan *) rte->funcexpr;

				for (;;)
				{
					TargetEntry *ste;

					ste = get_tle_by_resno(subplan->targetlist,
										   ((Var *) expr)->varattno);
					if (!ste || !ste->expr)
						break;
					expr = (Node *) ste->expr;
					if (!IsA(expr, Var))
						break;
					if (((Var *) expr)->varno == INNER)
						subplan = innerPlan(subplan);
					else
						subplan = outerPlan(subplan);
					if (!subplan)
						break;
				}
			}
			break;
	}

	/*
	 * We now have an expression we can't expand any more, so see if
	 * get_expr_result_type() can do anything with it.	If not, pass to
	 * lookup_rowtype_tupdesc() which will probably fail, but will give an
	 * appropriate error message while failing.
	 */
	if (get_expr_result_type(expr, NULL, &tupleDesc) != TYPEFUNC_COMPOSITE)
		tupleDesc = lookup_rowtype_tupdesc_copy(exprType(expr),
												exprTypmod(expr));

	/* Got the tupdesc, so we can extract the field name */
	Assert(fieldno >= 1 && fieldno <= tupleDesc->natts);
	return NameStr(tupleDesc->attrs[fieldno - 1]->attname);
}


/*
 * find_rte_by_refname		- look up an RTE by refname in a deparse context
 *
 * Returns NULL if there is no matching RTE or the refname is ambiguous.
 *
 * NOTE: this code is not really correct since it does not take account of
 * the fact that not all the RTEs in a rangetable may be visible from the
 * point where a Var reference appears.  For the purposes we need, however,
 * the only consequence of a false match is that we might stick a schema
 * qualifier on a Var that doesn't really need it.  So it seems close
 * enough.
 */
static RangeTblEntry *
find_rte_by_refname(const char *refname, deparse_context *context)
{
	RangeTblEntry *result = NULL;
	ListCell   *nslist;

	foreach(nslist, context->namespaces)
	{
		deparse_namespace *dpns = (deparse_namespace *) lfirst(nslist);
		ListCell   *rtlist;

		foreach(rtlist, dpns->rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(rtlist);

			if (strcmp(rte->eref->aliasname, refname) == 0)
			{
				if (result)
					return NULL;	/* it's ambiguous */
				result = rte;
			}
		}
		if (dpns->outer_rte &&
			strcmp(dpns->outer_rte->eref->aliasname, refname) == 0)
		{
			if (result)
				return NULL;	/* it's ambiguous */
			result = dpns->outer_rte;
		}
		if (dpns->inner_rte &&
			strcmp(dpns->inner_rte->eref->aliasname, refname) == 0)
		{
			if (result)
				return NULL;	/* it's ambiguous */
			result = dpns->inner_rte;
		}
		if (result)
			break;
	}
	return result;
}


/*
 * get_simple_binary_op_name
 *
 * helper function for isSimpleNode
 * will return single char binary operator name, or NULL if it's not
 */
static const char *
get_simple_binary_op_name(OpExpr *expr)
{
	List	   *args = expr->args;

	if (list_length(args) == 2)
	{
		/* binary operator */
		Node	   *arg1 = (Node *) linitial(args);
		Node	   *arg2 = (Node *) lsecond(args);
		const char *op;

		op = generate_operator_name(expr->opno, exprType(arg1), exprType(arg2));
		if (strlen(op) == 1)
			return op;
	}
	return NULL;
}


/*
 * isSimpleNode - check if given node is simple (doesn't need parenthesizing)
 *
 *	true   : simple in the context of parent node's type
 *	false  : not simple
 */
static bool
isSimpleNode(Node *node, Node *parentNode, int prettyFlags)
{
	if (!node)
		return false;

	switch (nodeTag(node))
	{
		case T_Var:
		case T_Const:
		case T_Param:
		case T_CoerceToDomainValue:
		case T_SetToDefault:
			/* single words: always simple */
			return true;

		case T_ArrayRef:
		case T_ArrayExpr:
		case T_RowExpr:
		case T_CoalesceExpr:
		case T_MinMaxExpr:
		case T_NullIfExpr:
		case T_Aggref:
		case T_FuncExpr:
			/* function-like: name(..) or name[..] */
			return true;

			/* CASE keywords act as parentheses */
		case T_CaseExpr:
			return true;

		case T_FieldSelect:

			/*
			 * appears simple since . has top precedence, unless parent is
			 * T_FieldSelect itself!
			 */
			return (IsA(parentNode, FieldSelect) ? false : true);

		case T_FieldStore:

			/*
			 * treat like FieldSelect (probably doesn't matter)
			 */
			return (IsA(parentNode, FieldStore) ? false : true);

		case T_CoerceToDomain:
			/* maybe simple, check args */
			return isSimpleNode((Node *) ((CoerceToDomain *) node)->arg,
								node, prettyFlags);
		case T_RelabelType:
			return isSimpleNode((Node *) ((RelabelType *) node)->arg,
								node, prettyFlags);
		case T_ConvertRowtypeExpr:
			return isSimpleNode((Node *) ((ConvertRowtypeExpr *) node)->arg,
								node, prettyFlags);

		case T_OpExpr:
			{
				/* depends on parent node type; needs further checking */
				if (prettyFlags & PRETTYFLAG_PAREN && IsA(parentNode, OpExpr))
				{
					const char *op;
					const char *parentOp;
					bool		is_lopriop;
					bool		is_hipriop;
					bool		is_lopriparent;
					bool		is_hipriparent;

					op = get_simple_binary_op_name((OpExpr *) node);
					if (!op)
						return false;

					/* We know only the basic operators + - and * / % */
					is_lopriop = (strchr("+-", *op) != NULL);
					is_hipriop = (strchr("*/%", *op) != NULL);
					if (!(is_lopriop || is_hipriop))
						return false;

					parentOp = get_simple_binary_op_name((OpExpr *) parentNode);
					if (!parentOp)
						return false;

					is_lopriparent = (strchr("+-", *parentOp) != NULL);
					is_hipriparent = (strchr("*/%", *parentOp) != NULL);
					if (!(is_lopriparent || is_hipriparent))
						return false;

					if (is_hipriop && is_lopriparent)
						return true;	/* op binds tighter than parent */

					if (is_lopriop && is_hipriparent)
						return false;

					/*
					 * Operators are same priority --- can skip parens only if
					 * we have (a - b) - c, not a - (b - c).
					 */
					if (node == (Node *) linitial(((OpExpr *) parentNode)->args))
						return true;

					return false;
				}
				/* else do the same stuff as for T_SubLink et al. */
				/* FALL THROUGH */
			}

		case T_SubLink:
		case T_NullTest:
		case T_BooleanTest:
		case T_DistinctExpr:
			switch (nodeTag(parentNode))
			{
				case T_FuncExpr:
					{
						/* special handling for casts */
						CoercionForm type = ((FuncExpr *) parentNode)->funcformat;

						if (type == COERCE_EXPLICIT_CAST ||
							type == COERCE_IMPLICIT_CAST)
							return false;
						return true;	/* own parentheses */
					}
				case T_BoolExpr:		/* lower precedence */
				case T_ArrayRef:		/* other separators */
				case T_ArrayExpr:		/* other separators */
				case T_RowExpr:	/* other separators */
				case T_CoalesceExpr:	/* own parentheses */
				case T_MinMaxExpr:		/* own parentheses */
				case T_NullIfExpr:		/* other separators */
				case T_Aggref:	/* own parentheses */
				case T_CaseExpr:		/* other separators */
					return true;
				default:
					return false;
			}

		case T_BoolExpr:
			switch (nodeTag(parentNode))
			{
				case T_BoolExpr:
					if (prettyFlags & PRETTYFLAG_PAREN)
					{
						BoolExprType type;
						BoolExprType parentType;

						type = ((BoolExpr *) node)->boolop;
						parentType = ((BoolExpr *) parentNode)->boolop;
						switch (type)
						{
							case NOT_EXPR:
							case AND_EXPR:
								if (parentType == AND_EXPR || parentType == OR_EXPR)
									return true;
								break;
							case OR_EXPR:
								if (parentType == OR_EXPR)
									return true;
								break;
						}
					}
					return false;
				case T_FuncExpr:
					{
						/* special handling for casts */
						CoercionForm type = ((FuncExpr *) parentNode)->funcformat;

						if (type == COERCE_EXPLICIT_CAST ||
							type == COERCE_IMPLICIT_CAST)
							return false;
						return true;	/* own parentheses */
					}
				case T_ArrayRef:		/* other separators */
				case T_ArrayExpr:		/* other separators */
				case T_RowExpr:	/* other separators */
				case T_CoalesceExpr:	/* own parentheses */
				case T_MinMaxExpr:		/* own parentheses */
				case T_NullIfExpr:		/* other separators */
				case T_Aggref:	/* own parentheses */
				case T_CaseExpr:		/* other separators */
					return true;
				default:
					return false;
			}

		default:
			break;
	}
	/* those we don't know: in dubio complexo */
	return false;
}


/*
 * appendStringInfoSpaces - append spaces to buffer
 */
static void
appendStringInfoSpaces(StringInfo buf, int count)
{
	while (count-- > 0)
		appendStringInfoChar(buf, ' ');
}

/*
 * appendContextKeyword - append a keyword to buffer
 *
 * If prettyPrint is enabled, perform a line break, and adjust indentation.
 * Otherwise, just append the keyword.
 */
static void
appendContextKeyword(deparse_context *context, const char *str,
					 int indentBefore, int indentAfter, int indentPlus)
{
	if (PRETTY_INDENT(context))
	{
		context->indentLevel += indentBefore;

		appendStringInfoChar(context->buf, '\n');
		appendStringInfoSpaces(context->buf,
							   Max(context->indentLevel, 0) + indentPlus);
		appendStringInfoString(context->buf, str);

		context->indentLevel += indentAfter;
		if (context->indentLevel < 0)
			context->indentLevel = 0;
	}
	else
		appendStringInfoString(context->buf, str);
}

/*
 * get_rule_expr_paren	- deparse expr using get_rule_expr,
 * embracing the string with parentheses if necessary for prettyPrint.
 *
 * Never embrace if prettyFlags=0, because it's done in the calling node.
 *
 * Any node that does *not* embrace its argument node by sql syntax (with
 * parentheses, non-operator keywords like CASE/WHEN/ON, or comma etc) should
 * use get_rule_expr_paren instead of get_rule_expr so parentheses can be
 * added.
 */
static void
get_rule_expr_paren(Node *node, deparse_context *context,
					bool showimplicit, Node *parentNode)
{
	bool		need_paren;

	need_paren = PRETTY_PAREN(context) &&
		!isSimpleNode(node, parentNode, context->prettyFlags);

	if (need_paren)
		appendStringInfoChar(context->buf, '(');

	get_rule_expr(node, context, showimplicit);

	if (need_paren)
		appendStringInfoChar(context->buf, ')');
}


/* ----------
 * get_rule_expr			- Parse back an expression
 *
 * Note: showimplicit determines whether we display any implicit cast that
 * is present at the top of the expression tree.  It is a passed argument,
 * not a field of the context struct, because we change the value as we
 * recurse down into the expression.  In general we suppress implicit casts
 * when the result type is known with certainty (eg, the arguments of an
 * OR must be boolean).  We display implicit casts for arguments of functions
 * and operators, since this is needed to be certain that the same function
 * or operator will be chosen when the expression is re-parsed.
 * ----------
 */
static void
get_rule_expr(Node *node, deparse_context *context,
			  bool showimplicit)
{
	StringInfo	buf = context->buf;

	if (node == NULL)
		return;

	/*
	 * Each level of get_rule_expr must emit an indivisible term
	 * (parenthesized if necessary) to ensure result is reparsed into the same
	 * expression tree.  The only exception is that when the input is a List,
	 * we emit the component items comma-separated with no surrounding
	 * decoration; this is convenient for most callers.
	 *
	 * There might be some work left here to support additional node types.
	 */
	switch (nodeTag(node))
	{
		case T_Var:
			{
				Var		   *var = (Var *) node;
				char	   *schemaname;
				char	   *refname;
				char	   *attname;

				get_names_for_var(var, 0, context,
								  &schemaname, &refname, &attname);
				if (refname && (context->varprefix || attname == NULL))
				{
					if (schemaname)
						appendStringInfo(buf, "%s.",
										 quote_identifier(schemaname));

					if (strcmp(refname, "*NEW*") == 0)
						appendStringInfoString(buf, "new.");
					else if (strcmp(refname, "*OLD*") == 0)
						appendStringInfoString(buf, "old.");
					else
						appendStringInfo(buf, "%s.",
										 quote_identifier(refname));
				}
				if (attname)
					appendStringInfoString(buf, quote_identifier(attname));
				else
					appendStringInfoString(buf, "*");
			}
			break;

		case T_Const:
			get_const_expr((Const *) node, context, 0);
			break;

		case T_Param:
			appendStringInfo(buf, "$%d", ((Param *) node)->paramid);
			break;

		case T_Aggref:
			get_agg_expr((Aggref *) node, context);
			break;

		case T_ArrayRef:
			{
				ArrayRef   *aref = (ArrayRef *) node;
				bool		need_parens;

				/*
				 * Parenthesize the argument unless it's a simple Var or a
				 * FieldSelect.  (In particular, if it's another ArrayRef, we
				 * *must* parenthesize to avoid confusion.)
				 */
				need_parens = !IsA(aref->refexpr, Var) &&
					!IsA(aref->refexpr, FieldSelect);
				if (need_parens)
					appendStringInfoChar(buf, '(');
				get_rule_expr((Node *) aref->refexpr, context, showimplicit);
				if (need_parens)
					appendStringInfoChar(buf, ')');
				printSubscripts(aref, context);

				/*
				 * Array assignment nodes should have been handled in
				 * processIndirection().
				 */
				if (aref->refassgnexpr)
					elog(ERROR, "unexpected refassgnexpr");
			}
			break;

		case T_FuncExpr:
			get_func_expr((FuncExpr *) node, context, showimplicit);
			break;

		case T_OpExpr:
			get_oper_expr((OpExpr *) node, context);
			break;

		case T_DistinctExpr:
			{
				DistinctExpr *expr = (DistinctExpr *) node;
				List	   *args = expr->args;
				Node	   *arg1 = (Node *) linitial(args);
				Node	   *arg2 = (Node *) lsecond(args);

				if (!PRETTY_PAREN(context))
					appendStringInfoChar(buf, '(');
				get_rule_expr_paren(arg1, context, true, node);
				appendStringInfo(buf, " IS DISTINCT FROM ");
				get_rule_expr_paren(arg2, context, true, node);
				if (!PRETTY_PAREN(context))
					appendStringInfoChar(buf, ')');
			}
			break;

		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *expr = (ScalarArrayOpExpr *) node;
				List	   *args = expr->args;
				Node	   *arg1 = (Node *) linitial(args);
				Node	   *arg2 = (Node *) lsecond(args);

				if (!PRETTY_PAREN(context))
					appendStringInfoChar(buf, '(');
				get_rule_expr_paren(arg1, context, true, node);
				appendStringInfo(buf, " %s %s (",
								 generate_operator_name(expr->opno,
														exprType(arg1),
										   get_element_type(exprType(arg2))),
								 expr->useOr ? "ANY" : "ALL");
				get_rule_expr_paren(arg2, context, true, node);
				appendStringInfoChar(buf, ')');
				if (!PRETTY_PAREN(context))
					appendStringInfoChar(buf, ')');
			}
			break;

		case T_BoolExpr:
			{
				BoolExpr   *expr = (BoolExpr *) node;
				Node	   *first_arg = linitial(expr->args);
				ListCell   *arg = lnext(list_head(expr->args));

				switch (expr->boolop)
				{
					case AND_EXPR:
						if (!PRETTY_PAREN(context))
							appendStringInfoChar(buf, '(');
						get_rule_expr_paren(first_arg, context,
											false, node);
						while (arg)
						{
							appendStringInfo(buf, " AND ");
							get_rule_expr_paren((Node *) lfirst(arg), context,
												false, node);
							arg = lnext(arg);
						}
						if (!PRETTY_PAREN(context))
							appendStringInfoChar(buf, ')');
						break;

					case OR_EXPR:
						if (!PRETTY_PAREN(context))
							appendStringInfoChar(buf, '(');
						get_rule_expr_paren(first_arg, context,
											false, node);
						while (arg)
						{
							appendStringInfo(buf, " OR ");
							get_rule_expr_paren((Node *) lfirst(arg), context,
												false, node);
							arg = lnext(arg);
						}
						if (!PRETTY_PAREN(context))
							appendStringInfoChar(buf, ')');
						break;

					case NOT_EXPR:
						if (!PRETTY_PAREN(context))
							appendStringInfoChar(buf, '(');
						appendStringInfo(buf, "NOT ");
						get_rule_expr_paren(first_arg, context,
											false, node);
						if (!PRETTY_PAREN(context))
							appendStringInfoChar(buf, ')');
						break;

					default:
						elog(ERROR, "unrecognized boolop: %d",
							 (int) expr->boolop);
				}
			}
			break;

		case T_SubLink:
			get_sublink_expr((SubLink *) node, context);
			break;

		case T_SubPlan:
			{
				/*
				 * We cannot see an already-planned subplan in rule deparsing,
				 * only while EXPLAINing a query plan. For now, just punt.
				 */
				if (((SubPlan *) node)->useHashTable)
					appendStringInfo(buf, "(hashed subplan)");
				else
					appendStringInfo(buf, "(subplan)");
			}
			break;

		case T_FieldSelect:
			{
				FieldSelect *fselect = (FieldSelect *) node;
				Node	   *arg = (Node *) fselect->arg;
				int			fno = fselect->fieldnum;
				const char *fieldname;
				bool		need_parens;

				/*
				 * Parenthesize the argument unless it's an ArrayRef or
				 * another FieldSelect.  Note in particular that it would be
				 * WRONG to not parenthesize a Var argument; simplicity is not
				 * the issue here, having the right number of names is.
				 */
				need_parens = !IsA(arg, ArrayRef) &&!IsA(arg, FieldSelect);
				if (need_parens)
					appendStringInfoChar(buf, '(');
				get_rule_expr(arg, context, true);
				if (need_parens)
					appendStringInfoChar(buf, ')');

				/*
				 * If it's a Var of type RECORD, we have to find what the Var
				 * refers to; otherwise we can use get_expr_result_type. If
				 * that fails, we try lookup_rowtype_tupdesc, which will
				 * probably fail too, but will ereport an acceptable message.
				 */
				if (IsA(arg, Var) &&
					((Var *) arg)->vartype == RECORDOID)
					fieldname = get_name_for_var_field((Var *) arg, fno,
													   0, context);
				else
				{
					TupleDesc	tupdesc;

					if (get_expr_result_type(arg, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
						tupdesc = lookup_rowtype_tupdesc_copy(exprType(arg),
															exprTypmod(arg));
					Assert(tupdesc);
					/* Got the tupdesc, so we can extract the field name */
					Assert(fno >= 1 && fno <= tupdesc->natts);
					fieldname = NameStr(tupdesc->attrs[fno - 1]->attname);
				}

				appendStringInfo(buf, ".%s", quote_identifier(fieldname));
			}
			break;

		case T_FieldStore:

			/*
			 * We shouldn't see FieldStore here; it should have been stripped
			 * off by processIndirection().
			 */
			elog(ERROR, "unexpected FieldStore");
			break;

		case T_RelabelType:
			{
				RelabelType *relabel = (RelabelType *) node;
				Node	   *arg = (Node *) relabel->arg;

				if (relabel->relabelformat == COERCE_IMPLICIT_CAST &&
					!showimplicit)
				{
					/* don't show the implicit cast */
					get_rule_expr_paren(arg, context, false, node);
				}
				else
				{
					if (!PRETTY_PAREN(context))
						appendStringInfoChar(buf, '(');
					get_rule_expr_paren(arg, context, false, node);
					if (!PRETTY_PAREN(context))
						appendStringInfoChar(buf, ')');
					appendStringInfo(buf, "::%s",
								format_type_with_typemod(relabel->resulttype,
													 relabel->resulttypmod));
				}
			}
			break;

		case T_ConvertRowtypeExpr:
			{
				ConvertRowtypeExpr *convert = (ConvertRowtypeExpr *) node;
				Node	   *arg = (Node *) convert->arg;

				if (convert->convertformat == COERCE_IMPLICIT_CAST &&
					!showimplicit)
				{
					/* don't show the implicit cast */
					get_rule_expr_paren(arg, context, false, node);
				}
				else
				{
					if (!PRETTY_PAREN(context))
						appendStringInfoChar(buf, '(');
					get_rule_expr_paren(arg, context, false, node);
					if (!PRETTY_PAREN(context))
						appendStringInfoChar(buf, ')');
					appendStringInfo(buf, "::%s",
						  format_type_with_typemod(convert->resulttype, -1));
				}
			}
			break;

		case T_CaseExpr:
			{
				CaseExpr   *caseexpr = (CaseExpr *) node;
				ListCell   *temp;

				appendContextKeyword(context, "CASE",
									 0, PRETTYINDENT_VAR, 0);
				if (caseexpr->arg)
				{
					appendStringInfoChar(buf, ' ');
					get_rule_expr((Node *) caseexpr->arg, context, true);
				}
				foreach(temp, caseexpr->args)
				{
					CaseWhen   *when = (CaseWhen *) lfirst(temp);
					Node	   *w = (Node *) when->expr;

					if (!PRETTY_INDENT(context))
						appendStringInfoChar(buf, ' ');
					appendContextKeyword(context, "WHEN ",
										 0, 0, 0);
					if (caseexpr->arg)
					{
						/*
						 * The parser should have produced WHEN clauses of the
						 * form "CaseTestExpr = RHS"; we want to show just the
						 * RHS.  If the user wrote something silly like "CASE
						 * boolexpr WHEN TRUE THEN ...", then the optimizer's
						 * simplify_boolean_equality() may have reduced this
						 * to just "CaseTestExpr" or "NOT CaseTestExpr", for
						 * which we have to show "TRUE" or "FALSE".  Also,
						 * depending on context the original CaseTestExpr
						 * might have been reduced to a Const (but we won't
						 * see "WHEN Const").
						 */
						if (IsA(w, OpExpr))
						{
							Node	   *rhs;

							Assert(IsA(linitial(((OpExpr *) w)->args),
									   CaseTestExpr) ||
								   IsA(linitial(((OpExpr *) w)->args),
									   Const));
							rhs = (Node *) lsecond(((OpExpr *) w)->args);
							get_rule_expr(rhs, context, false);
						}
						else if (IsA(w, CaseTestExpr))
							appendStringInfo(buf, "TRUE");
						else if (not_clause(w))
						{
							Assert(IsA(get_notclausearg((Expr *) w),
									   CaseTestExpr));
							appendStringInfo(buf, "FALSE");
						}
						else
							elog(ERROR, "unexpected CASE WHEN clause: %d",
								 (int) nodeTag(w));
					}
					else
						get_rule_expr(w, context, false);
					appendStringInfo(buf, " THEN ");
					get_rule_expr((Node *) when->result, context, true);
				}
				if (!PRETTY_INDENT(context))
					appendStringInfoChar(buf, ' ');
				appendContextKeyword(context, "ELSE ",
									 0, 0, 0);
				get_rule_expr((Node *) caseexpr->defresult, context, true);
				if (!PRETTY_INDENT(context))
					appendStringInfoChar(buf, ' ');
				appendContextKeyword(context, "END",
									 -PRETTYINDENT_VAR, 0, 0);
			}
			break;

		case T_ArrayExpr:
			{
				ArrayExpr  *arrayexpr = (ArrayExpr *) node;

				appendStringInfo(buf, "ARRAY[");
				get_rule_expr((Node *) arrayexpr->elements, context, true);
				appendStringInfoChar(buf, ']');
			}
			break;

		case T_RowExpr:
			{
				RowExpr    *rowexpr = (RowExpr *) node;
				TupleDesc	tupdesc = NULL;
				ListCell   *arg;
				int			i;
				char	   *sep;

				/*
				 * If it's a named type and not RECORD, we may have to skip
				 * dropped columns and/or claim there are NULLs for added
				 * columns.
				 */
				if (rowexpr->row_typeid != RECORDOID)
				{
					tupdesc = lookup_rowtype_tupdesc(rowexpr->row_typeid, -1);
					Assert(list_length(rowexpr->args) <= tupdesc->natts);
				}

				/*
				 * SQL99 allows "ROW" to be omitted when there is more than
				 * one column, but for simplicity we always print it.
				 */
				appendStringInfo(buf, "ROW(");
				sep = "";
				i = 0;
				foreach(arg, rowexpr->args)
				{
					Node	   *e = (Node *) lfirst(arg);

					if (tupdesc == NULL ||
						!tupdesc->attrs[i]->attisdropped)
					{
						appendStringInfoString(buf, sep);
						get_rule_expr(e, context, true);
						sep = ", ";
					}
					i++;
				}
				if (tupdesc != NULL)
				{
					while (i < tupdesc->natts)
					{
						if (!tupdesc->attrs[i]->attisdropped)
						{
							appendStringInfoString(buf, sep);
							appendStringInfo(buf, "NULL");
							sep = ", ";
						}
						i++;
					}

					ReleaseTupleDesc(tupdesc);
				}
				appendStringInfo(buf, ")");
				if (rowexpr->row_format == COERCE_EXPLICIT_CAST)
					appendStringInfo(buf, "::%s",
						  format_type_with_typemod(rowexpr->row_typeid, -1));
			}
			break;

		case T_RowCompareExpr:
			{
				RowCompareExpr *rcexpr = (RowCompareExpr *) node;
				ListCell   *arg;
				char	   *sep;

				/*
				 * SQL99 allows "ROW" to be omitted when there is more than
				 * one column, but for simplicity we always print it.
				 */
				appendStringInfo(buf, "(ROW(");
				sep = "";
				foreach(arg, rcexpr->largs)
				{
					Node	   *e = (Node *) lfirst(arg);

					appendStringInfoString(buf, sep);
					get_rule_expr(e, context, true);
					sep = ", ";
				}

				/*
				 * We assume that the name of the first-column operator will
				 * do for all the rest too.  This is definitely open to
				 * failure, eg if some but not all operators were renamed
				 * since the construct was parsed, but there seems no way to
				 * be perfect.
				 */
				appendStringInfo(buf, ") %s ROW(",
						  generate_operator_name(linitial_oid(rcexpr->opnos),
										   exprType(linitial(rcexpr->largs)),
										 exprType(linitial(rcexpr->rargs))));
				sep = "";
				foreach(arg, rcexpr->rargs)
				{
					Node	   *e = (Node *) lfirst(arg);

					appendStringInfoString(buf, sep);
					get_rule_expr(e, context, true);
					sep = ", ";
				}
				appendStringInfo(buf, "))");
			}
			break;

		case T_CoalesceExpr:
			{
				CoalesceExpr *coalesceexpr = (CoalesceExpr *) node;

				appendStringInfo(buf, "COALESCE(");
				get_rule_expr((Node *) coalesceexpr->args, context, true);
				appendStringInfoChar(buf, ')');
			}
			break;

		case T_MinMaxExpr:
			{
				MinMaxExpr *minmaxexpr = (MinMaxExpr *) node;

				switch (minmaxexpr->op)
				{
					case IS_GREATEST:
						appendStringInfo(buf, "GREATEST(");
						break;
					case IS_LEAST:
						appendStringInfo(buf, "LEAST(");
						break;
				}
				get_rule_expr((Node *) minmaxexpr->args, context, true);
				appendStringInfoChar(buf, ')');
			}
			break;

		case T_NullIfExpr:
			{
				NullIfExpr *nullifexpr = (NullIfExpr *) node;

				appendStringInfo(buf, "NULLIF(");
				get_rule_expr((Node *) nullifexpr->args, context, true);
				appendStringInfoChar(buf, ')');
			}
			break;

		case T_NullTest:
			{
				NullTest   *ntest = (NullTest *) node;

				if (!PRETTY_PAREN(context))
					appendStringInfoChar(buf, '(');
				get_rule_expr_paren((Node *) ntest->arg, context, true, node);
				switch (ntest->nulltesttype)
				{
					case IS_NULL:
						appendStringInfo(buf, " IS NULL");
						break;
					case IS_NOT_NULL:
						appendStringInfo(buf, " IS NOT NULL");
						break;
					default:
						elog(ERROR, "unrecognized nulltesttype: %d",
							 (int) ntest->nulltesttype);
				}
				if (!PRETTY_PAREN(context))
					appendStringInfoChar(buf, ')');
			}
			break;

		case T_BooleanTest:
			{
				BooleanTest *btest = (BooleanTest *) node;

				if (!PRETTY_PAREN(context))
					appendStringInfoChar(buf, '(');
				get_rule_expr_paren((Node *) btest->arg, context, false, node);
				switch (btest->booltesttype)
				{
					case IS_TRUE:
						appendStringInfo(buf, " IS TRUE");
						break;
					case IS_NOT_TRUE:
						appendStringInfo(buf, " IS NOT TRUE");
						break;
					case IS_FALSE:
						appendStringInfo(buf, " IS FALSE");
						break;
					case IS_NOT_FALSE:
						appendStringInfo(buf, " IS NOT FALSE");
						break;
					case IS_UNKNOWN:
						appendStringInfo(buf, " IS UNKNOWN");
						break;
					case IS_NOT_UNKNOWN:
						appendStringInfo(buf, " IS NOT UNKNOWN");
						break;
					default:
						elog(ERROR, "unrecognized booltesttype: %d",
							 (int) btest->booltesttype);
				}
				if (!PRETTY_PAREN(context))
					appendStringInfoChar(buf, ')');
			}
			break;

		case T_CoerceToDomain:
			{
				CoerceToDomain *ctest = (CoerceToDomain *) node;
				Node	   *arg = (Node *) ctest->arg;

				if (ctest->coercionformat == COERCE_IMPLICIT_CAST &&
					!showimplicit)
				{
					/* don't show the implicit cast */
					get_rule_expr(arg, context, false);
				}
				else
				{
					if (!PRETTY_PAREN(context))
						appendStringInfoChar(buf, '(');
					get_rule_expr_paren(arg, context, false, node);
					if (!PRETTY_PAREN(context))
						appendStringInfoChar(buf, ')');
					appendStringInfo(buf, "::%s",
								  format_type_with_typemod(ctest->resulttype,
													   ctest->resulttypmod));
				}
			}
			break;

		case T_CoerceToDomainValue:
			appendStringInfo(buf, "VALUE");
			break;

		case T_SetToDefault:
			appendStringInfo(buf, "DEFAULT");
			break;

		case T_List:
			{
				char	   *sep;
				ListCell   *l;

				sep = "";
				foreach(l, (List *) node)
				{
					appendStringInfoString(buf, sep);
					get_rule_expr((Node *) lfirst(l), context, showimplicit);
					sep = ", ";
				}
			}
			break;

		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
			break;
	}
}


/*
 * get_oper_expr			- Parse back an OpExpr node
 */
static void
get_oper_expr(OpExpr *expr, deparse_context *context)
{
	StringInfo	buf = context->buf;
	Oid			opno = expr->opno;
	List	   *args = expr->args;

	if (!PRETTY_PAREN(context))
		appendStringInfoChar(buf, '(');
	if (list_length(args) == 2)
	{
		/* binary operator */
		Node	   *arg1 = (Node *) linitial(args);
		Node	   *arg2 = (Node *) lsecond(args);

		get_rule_expr_paren(arg1, context, true, (Node *) expr);
		appendStringInfo(buf, " %s ",
						 generate_operator_name(opno,
												exprType(arg1),
												exprType(arg2)));
		get_rule_expr_paren(arg2, context, true, (Node *) expr);
	}
	else
	{
		/* unary operator --- but which side? */
		Node	   *arg = (Node *) linitial(args);
		HeapTuple	tp;
		Form_pg_operator optup;

		tp = SearchSysCache(OPEROID,
							ObjectIdGetDatum(opno),
							0, 0, 0);
		if (!HeapTupleIsValid(tp))
			elog(ERROR, "cache lookup failed for operator %u", opno);
		optup = (Form_pg_operator) GETSTRUCT(tp);
		switch (optup->oprkind)
		{
			case 'l':
				appendStringInfo(buf, "%s ",
								 generate_operator_name(opno,
														InvalidOid,
														exprType(arg)));
				get_rule_expr_paren(arg, context, true, (Node *) expr);
				break;
			case 'r':
				get_rule_expr_paren(arg, context, true, (Node *) expr);
				appendStringInfo(buf, " %s",
								 generate_operator_name(opno,
														exprType(arg),
														InvalidOid));
				break;
			default:
				elog(ERROR, "bogus oprkind: %d", optup->oprkind);
		}
		ReleaseSysCache(tp);
	}
	if (!PRETTY_PAREN(context))
		appendStringInfoChar(buf, ')');
}

/*
 * get_func_expr			- Parse back a FuncExpr node
 */
static void
get_func_expr(FuncExpr *expr, deparse_context *context,
			  bool showimplicit)
{
	StringInfo	buf = context->buf;
	Oid			funcoid = expr->funcid;
	Oid			argtypes[FUNC_MAX_ARGS];
	int			nargs;
	ListCell   *l;

	/*
	 * If the function call came from an implicit coercion, then just show the
	 * first argument --- unless caller wants to see implicit coercions.
	 */
	if (expr->funcformat == COERCE_IMPLICIT_CAST && !showimplicit)
	{
		get_rule_expr_paren((Node *) linitial(expr->args), context,
							false, (Node *) expr);
		return;
	}

	/*
	 * If the function call came from a cast, then show the first argument
	 * plus an explicit cast operation.
	 */
	if (expr->funcformat == COERCE_EXPLICIT_CAST ||
		expr->funcformat == COERCE_IMPLICIT_CAST)
	{
		Node	   *arg = linitial(expr->args);
		Oid			rettype = expr->funcresulttype;
		int32		coercedTypmod;

		/* Get the typmod if this is a length-coercion function */
		(void) exprIsLengthCoercion((Node *) expr, &coercedTypmod);

		if (!PRETTY_PAREN(context))
			appendStringInfoChar(buf, '(');
		get_rule_expr_paren(arg, context, false, (Node *) expr);
		if (!PRETTY_PAREN(context))
			appendStringInfoChar(buf, ')');
		appendStringInfo(buf, "::%s",
						 format_type_with_typemod(rettype, coercedTypmod));

		return;
	}

	/*
	 * Normal function: display as proname(args).  First we need to extract
	 * the argument datatypes.
	 */
	nargs = 0;
	foreach(l, expr->args)
	{
		if (nargs >= FUNC_MAX_ARGS)
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
					 errmsg("too many arguments")));
		argtypes[nargs] = exprType((Node *) lfirst(l));
		nargs++;
	}

	appendStringInfo(buf, "%s(",
					 generate_function_name(funcoid, nargs, argtypes));
	get_rule_expr((Node *) expr->args, context, true);
	appendStringInfoChar(buf, ')');
}

/*
 * get_agg_expr			- Parse back an Aggref node
 */
static void
get_agg_expr(Aggref *aggref, deparse_context *context)
{
	StringInfo	buf = context->buf;
	Oid			argtypes[FUNC_MAX_ARGS];
	int			nargs;
	ListCell   *l;

	nargs = 0;
	foreach(l, aggref->args)
	{
		if (nargs >= FUNC_MAX_ARGS)
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
					 errmsg("too many arguments")));
		argtypes[nargs] = exprType((Node *) lfirst(l));
		nargs++;
	}

	appendStringInfo(buf, "%s(%s",
				   generate_function_name(aggref->aggfnoid, nargs, argtypes),
					 aggref->aggdistinct ? "DISTINCT " : "");
	/* aggstar can be set only in zero-argument aggregates */
	if (aggref->aggstar)
		appendStringInfoChar(buf, '*');
	else
		get_rule_expr((Node *) aggref->args, context, true);
	appendStringInfoChar(buf, ')');
}


/* ----------
 * get_const_expr
 *
 *	Make a string representation of a Const
 *
 * showtype can be -1 to never show "::typename" decoration, or +1 to always
 * show it, or 0 to show it only if the constant wouldn't be assumed to be
 * the right type by default.
 * ----------
 */
static void
get_const_expr(Const *constval, deparse_context *context, int showtype)
{
	StringInfo	buf = context->buf;
	Oid			typoutput;
	bool		typIsVarlena;
	char	   *extval;
	char	   *valptr;
	bool		isfloat = false;
	bool		needlabel;

	if (constval->constisnull)
	{
		/*
		 * Always label the type of a NULL constant to prevent misdecisions
		 * about type when reparsing.
		 */
		appendStringInfo(buf, "NULL");
		if (showtype >= 0)
			appendStringInfo(buf, "::%s",
							 format_type_with_typemod(constval->consttype,
													  -1));
		return;
	}

	getTypeOutputInfo(constval->consttype,
					  &typoutput, &typIsVarlena);

	extval = OidOutputFunctionCall(typoutput, constval->constvalue);

	switch (constval->consttype)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case OIDOID:
		case FLOAT4OID:
		case FLOAT8OID:
		case NUMERICOID:
			{
				/*
				 * These types are printed without quotes unless they contain
				 * values that aren't accepted by the scanner unquoted (e.g.,
				 * 'NaN').	Note that strtod() and friends might accept NaN,
				 * so we can't use that to test.
				 *
				 * In reality we only need to defend against infinity and NaN,
				 * so we need not get too crazy about pattern matching here.
				 *
				 * There is a special-case gotcha: if the constant is signed,
				 * we need to parenthesize it, else the parser might see a
				 * leading plus/minus as binding less tightly than adjacent
				 * operators --- particularly, the cast that we might attach
				 * below.
				 */
				if (strspn(extval, "0123456789+-eE.") == strlen(extval))
				{
					if (extval[0] == '+' || extval[0] == '-')
						appendStringInfo(buf, "(%s)", extval);
					else
						appendStringInfoString(buf, extval);
					if (strcspn(extval, "eE.") != strlen(extval))
						isfloat = true; /* it looks like a float */
				}
				else
					appendStringInfo(buf, "'%s'", extval);
			}
			break;

		case BITOID:
		case VARBITOID:
			appendStringInfo(buf, "B'%s'", extval);
			break;

		case BOOLOID:
			if (strcmp(extval, "t") == 0)
				appendStringInfo(buf, "true");
			else
				appendStringInfo(buf, "false");
			break;

		default:

			/*
			 * We form the string literal according to the prevailing setting
			 * of standard_conforming_strings; we never use E''. User is
			 * responsible for making sure result is used correctly.
			 */
			appendStringInfoChar(buf, '\'');
			for (valptr = extval; *valptr; valptr++)
			{
				char		ch = *valptr;

				if (SQL_STR_DOUBLE(ch, !standard_conforming_strings))
					appendStringInfoChar(buf, ch);
				appendStringInfoChar(buf, ch);
			}
			appendStringInfoChar(buf, '\'');
			break;
	}

	pfree(extval);

	if (showtype < 0)
		return;

	/*
	 * For showtype == 0, append ::typename unless the constant will be
	 * implicitly typed as the right type when it is read in.
	 *
	 * XXX this code has to be kept in sync with the behavior of the parser,
	 * especially make_const.
	 */
	switch (constval->consttype)
	{
		case BOOLOID:
		case INT4OID:
		case UNKNOWNOID:
			/* These types can be left unlabeled */
			needlabel = false;
			break;
		case NUMERICOID:
			/* Float-looking constants will be typed as numeric */
			needlabel = !isfloat;
			break;
		default:
			needlabel = true;
			break;
	}
	if (needlabel || showtype > 0)
		appendStringInfo(buf, "::%s",
						 format_type_with_typemod(constval->consttype, -1));
}


/* ----------
 * get_sublink_expr			- Parse back a sublink
 * ----------
 */
static void
get_sublink_expr(SubLink *sublink, deparse_context *context)
{
	StringInfo	buf = context->buf;
	Query	   *query = (Query *) (sublink->subselect);
	char	   *opname = NULL;
	bool		need_paren;

	if (sublink->subLinkType == ARRAY_SUBLINK)
		appendStringInfo(buf, "ARRAY(");
	else
		appendStringInfoChar(buf, '(');

	/*
	 * Note that we print the name of only the first operator, when there are
	 * multiple combining operators.  This is an approximation that could go
	 * wrong in various scenarios (operators in different schemas, renamed
	 * operators, etc) but there is not a whole lot we can do about it, since
	 * the syntax allows only one operator to be shown.
	 */
	if (sublink->testexpr)
	{
		if (IsA(sublink->testexpr, OpExpr))
		{
			/* single combining operator */
			OpExpr	   *opexpr = (OpExpr *) sublink->testexpr;

			get_rule_expr(linitial(opexpr->args), context, true);
			opname = generate_operator_name(opexpr->opno,
											exprType(linitial(opexpr->args)),
											exprType(lsecond(opexpr->args)));
		}
		else if (IsA(sublink->testexpr, BoolExpr))
		{
			/* multiple combining operators, = or <> cases */
			char	   *sep;
			ListCell   *l;

			appendStringInfoChar(buf, '(');
			sep = "";
			foreach(l, ((BoolExpr *) sublink->testexpr)->args)
			{
				OpExpr	   *opexpr = (OpExpr *) lfirst(l);

				Assert(IsA(opexpr, OpExpr));
				appendStringInfoString(buf, sep);
				get_rule_expr(linitial(opexpr->args), context, true);
				if (!opname)
					opname = generate_operator_name(opexpr->opno,
											exprType(linitial(opexpr->args)),
											exprType(lsecond(opexpr->args)));
				sep = ", ";
			}
			appendStringInfoChar(buf, ')');
		}
		else if (IsA(sublink->testexpr, RowCompareExpr))
		{
			/* multiple combining operators, < <= > >= cases */
			RowCompareExpr *rcexpr = (RowCompareExpr *) sublink->testexpr;

			appendStringInfoChar(buf, '(');
			get_rule_expr((Node *) rcexpr->largs, context, true);
			opname = generate_operator_name(linitial_oid(rcexpr->opnos),
											exprType(linitial(rcexpr->largs)),
										  exprType(linitial(rcexpr->rargs)));
			appendStringInfoChar(buf, ')');
		}
		else
			elog(ERROR, "unrecognized testexpr type: %d",
				 (int) nodeTag(sublink->testexpr));
	}

	need_paren = true;

	switch (sublink->subLinkType)
	{
		case EXISTS_SUBLINK:
			appendStringInfo(buf, "EXISTS ");
			break;

		case ANY_SUBLINK:
			if (strcmp(opname, "=") == 0)		/* Represent = ANY as IN */
				appendStringInfo(buf, " IN ");
			else
				appendStringInfo(buf, " %s ANY ", opname);
			break;

		case ALL_SUBLINK:
			appendStringInfo(buf, " %s ALL ", opname);
			break;

		case ROWCOMPARE_SUBLINK:
			appendStringInfo(buf, " %s ", opname);
			break;

		case EXPR_SUBLINK:
		case ARRAY_SUBLINK:
			need_paren = false;
			break;

		default:
			elog(ERROR, "unrecognized sublink type: %d",
				 (int) sublink->subLinkType);
			break;
	}

	if (need_paren)
		appendStringInfoChar(buf, '(');

	get_query_def(query, buf, context->namespaces, NULL,
				  context->prettyFlags, context->indentLevel);

	if (need_paren)
		appendStringInfo(buf, "))");
	else
		appendStringInfoChar(buf, ')');
}


/* ----------
 * get_from_clause			- Parse back a FROM clause
 *
 * "prefix" is the keyword that denotes the start of the list of FROM
 * elements. It is FROM when used to parse back SELECT and UPDATE, but
 * is USING when parsing back DELETE.
 * ----------
 */
static void
get_from_clause(Query *query, const char *prefix, deparse_context *context)
{
	StringInfo	buf = context->buf;
	bool		first = true;
	ListCell   *l;

	/*
	 * We use the query's jointree as a guide to what to print.  However, we
	 * must ignore auto-added RTEs that are marked not inFromCl. (These can
	 * only appear at the top level of the jointree, so it's sufficient to
	 * check here.)  This check also ensures we ignore the rule pseudo-RTEs
	 * for NEW and OLD.
	 */
	foreach(l, query->jointree->fromlist)
	{
		Node	   *jtnode = (Node *) lfirst(l);

		if (IsA(jtnode, RangeTblRef))
		{
			int			varno = ((RangeTblRef *) jtnode)->rtindex;
			RangeTblEntry *rte = rt_fetch(varno, query->rtable);

			if (!rte->inFromCl)
				continue;
		}

		if (first)
		{
			appendContextKeyword(context, prefix,
								 -PRETTYINDENT_STD, PRETTYINDENT_STD, 2);
			first = false;
		}
		else
			appendStringInfoString(buf, ", ");

		get_from_clause_item(jtnode, query, context);
	}
}

static void
get_from_clause_item(Node *jtnode, Query *query, deparse_context *context)
{
	StringInfo	buf = context->buf;

	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;
		RangeTblEntry *rte = rt_fetch(varno, query->rtable);
		bool		gavealias = false;

		switch (rte->rtekind)
		{
			case RTE_RELATION:
				/* Normal relation RTE */
				appendStringInfo(buf, "%s%s",
								 only_marker(rte),
								 generate_relation_name(rte->relid));
				break;
			case RTE_SUBQUERY:
				/* Subquery RTE */
				appendStringInfoChar(buf, '(');
				get_query_def(rte->subquery, buf, context->namespaces, NULL,
							  context->prettyFlags, context->indentLevel);
				appendStringInfoChar(buf, ')');
				break;
			case RTE_FUNCTION:
				/* Function RTE */
				get_rule_expr(rte->funcexpr, context, true);
				break;
			case RTE_VALUES:
				/* Values list RTE */
				get_values_def(rte->values_lists, context);
				break;
			default:
				elog(ERROR, "unrecognized RTE kind: %d", (int) rte->rtekind);
				break;
		}

		if (rte->alias != NULL)
		{
			appendStringInfo(buf, " %s",
							 quote_identifier(rte->alias->aliasname));
			gavealias = true;
		}
		else if (rte->rtekind == RTE_RELATION &&
				 strcmp(rte->eref->aliasname, get_rel_name(rte->relid)) != 0)
		{
			/*
			 * Apparently the rel has been renamed since the rule was made.
			 * Emit a fake alias clause so that variable references will still
			 * work.  This is not a 100% solution but should work in most
			 * reasonable situations.
			 */
			appendStringInfo(buf, " %s",
							 quote_identifier(rte->eref->aliasname));
			gavealias = true;
		}
		else if (rte->rtekind == RTE_FUNCTION)
		{
			/*
			 * For a function RTE, always give an alias. This covers possible
			 * renaming of the function and/or instability of the
			 * FigureColname rules for things that aren't simple functions.
			 */
			appendStringInfo(buf, " %s",
							 quote_identifier(rte->eref->aliasname));
			gavealias = true;
		}

		if (rte->rtekind == RTE_FUNCTION)
		{
			if (rte->funccoltypes != NIL)
			{
				/* Function returning RECORD, reconstruct the columndefs */
				if (!gavealias)
					appendStringInfo(buf, " AS ");
				get_from_clause_coldeflist(rte->eref->colnames,
										   rte->funccoltypes,
										   rte->funccoltypmods,
										   context);
			}
			else
			{
				/*
				 * For a function RTE, always emit a complete column alias
				 * list; this is to protect against possible instability of
				 * the default column names (eg, from altering parameter
				 * names).
				 */
				get_from_clause_alias(rte->eref, rte, context);
			}
		}
		else
		{
			/*
			 * For non-function RTEs, just report whatever the user originally
			 * gave as column aliases.
			 */
			get_from_clause_alias(rte->alias, rte, context);
		}
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		bool		need_paren_on_right;

		need_paren_on_right = PRETTY_PAREN(context) &&
			!IsA(j->rarg, RangeTblRef) &&
			!(IsA(j->rarg, JoinExpr) &&((JoinExpr *) j->rarg)->alias != NULL);

		if (!PRETTY_PAREN(context) || j->alias != NULL)
			appendStringInfoChar(buf, '(');

		get_from_clause_item(j->larg, query, context);

		if (j->isNatural)
		{
			if (!PRETTY_INDENT(context))
				appendStringInfoChar(buf, ' ');
			switch (j->jointype)
			{
				case JOIN_INNER:
					appendContextKeyword(context, "NATURAL JOIN ",
										 -PRETTYINDENT_JOIN,
										 PRETTYINDENT_JOIN, 0);
					break;
				case JOIN_LEFT:
					appendContextKeyword(context, "NATURAL LEFT JOIN ",
										 -PRETTYINDENT_JOIN,
										 PRETTYINDENT_JOIN, 0);
					break;
				case JOIN_FULL:
					appendContextKeyword(context, "NATURAL FULL JOIN ",
										 -PRETTYINDENT_JOIN,
										 PRETTYINDENT_JOIN, 0);
					break;
				case JOIN_RIGHT:
					appendContextKeyword(context, "NATURAL RIGHT JOIN ",
										 -PRETTYINDENT_JOIN,
										 PRETTYINDENT_JOIN, 0);
					break;
				default:
					elog(ERROR, "unrecognized join type: %d",
						 (int) j->jointype);
			}
		}
		else
		{
			switch (j->jointype)
			{
				case JOIN_INNER:
					if (j->quals)
						appendContextKeyword(context, " JOIN ",
											 -PRETTYINDENT_JOIN,
											 PRETTYINDENT_JOIN, 2);
					else
						appendContextKeyword(context, " CROSS JOIN ",
											 -PRETTYINDENT_JOIN,
											 PRETTYINDENT_JOIN, 1);
					break;
				case JOIN_LEFT:
					appendContextKeyword(context, " LEFT JOIN ",
										 -PRETTYINDENT_JOIN,
										 PRETTYINDENT_JOIN, 2);
					break;
				case JOIN_FULL:
					appendContextKeyword(context, " FULL JOIN ",
										 -PRETTYINDENT_JOIN,
										 PRETTYINDENT_JOIN, 2);
					break;
				case JOIN_RIGHT:
					appendContextKeyword(context, " RIGHT JOIN ",
										 -PRETTYINDENT_JOIN,
										 PRETTYINDENT_JOIN, 2);
					break;
				default:
					elog(ERROR, "unrecognized join type: %d",
						 (int) j->jointype);
			}
		}

		if (need_paren_on_right)
			appendStringInfoChar(buf, '(');
		get_from_clause_item(j->rarg, query, context);
		if (need_paren_on_right)
			appendStringInfoChar(buf, ')');

		context->indentLevel -= PRETTYINDENT_JOIN_ON;

		if (!j->isNatural)
		{
			if (j->using)
			{
				ListCell   *col;

				appendStringInfo(buf, " USING (");
				foreach(col, j->using)
				{
					if (col != list_head(j->using))
						appendStringInfo(buf, ", ");
					appendStringInfoString(buf,
									  quote_identifier(strVal(lfirst(col))));
				}
				appendStringInfoChar(buf, ')');
			}
			else if (j->quals)
			{
				appendStringInfo(buf, " ON ");
				if (!PRETTY_PAREN(context))
					appendStringInfoChar(buf, '(');
				get_rule_expr(j->quals, context, false);
				if (!PRETTY_PAREN(context))
					appendStringInfoChar(buf, ')');
			}
		}
		if (!PRETTY_PAREN(context) || j->alias != NULL)
			appendStringInfoChar(buf, ')');

		/* Yes, it's correct to put alias after the right paren ... */
		if (j->alias != NULL)
		{
			appendStringInfo(buf, " %s",
							 quote_identifier(j->alias->aliasname));
			get_from_clause_alias(j->alias,
								  rt_fetch(j->rtindex, query->rtable),
								  context);
		}
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
}

/*
 * get_from_clause_alias - reproduce column alias list
 *
 * This is tricky because we must ignore dropped columns.
 */
static void
get_from_clause_alias(Alias *alias, RangeTblEntry *rte,
					  deparse_context *context)
{
	StringInfo	buf = context->buf;
	ListCell   *col;
	AttrNumber	attnum;
	bool		first = true;

	if (alias == NULL || alias->colnames == NIL)
		return;					/* definitely nothing to do */

	attnum = 0;
	foreach(col, alias->colnames)
	{
		attnum++;
		if (get_rte_attribute_is_dropped(rte, attnum))
			continue;
		if (first)
		{
			appendStringInfoChar(buf, '(');
			first = false;
		}
		else
			appendStringInfo(buf, ", ");
		appendStringInfoString(buf,
							   quote_identifier(strVal(lfirst(col))));
	}
	if (!first)
		appendStringInfoChar(buf, ')');
}

/*
 * get_from_clause_coldeflist - reproduce FROM clause coldeflist
 *
 * The coldeflist is appended immediately (no space) to buf.  Caller is
 * responsible for ensuring that an alias or AS is present before it.
 */
static void
get_from_clause_coldeflist(List *names, List *types, List *typmods,
						   deparse_context *context)
{
	StringInfo	buf = context->buf;
	ListCell   *l1;
	ListCell   *l2;
	ListCell   *l3;
	int			i = 0;

	appendStringInfoChar(buf, '(');

	l2 = list_head(types);
	l3 = list_head(typmods);
	foreach(l1, names)
	{
		char	   *attname = strVal(lfirst(l1));
		Oid			atttypid;
		int32		atttypmod;

		atttypid = lfirst_oid(l2);
		l2 = lnext(l2);
		atttypmod = lfirst_int(l3);
		l3 = lnext(l3);

		if (i > 0)
			appendStringInfo(buf, ", ");
		appendStringInfo(buf, "%s %s",
						 quote_identifier(attname),
						 format_type_with_typemod(atttypid, atttypmod));
		i++;
	}

	appendStringInfoChar(buf, ')');
}

/*
 * get_opclass_name			- fetch name of an index operator class
 *
 * The opclass name is appended (after a space) to buf.
 *
 * Output is suppressed if the opclass is the default for the given
 * actual_datatype.  (If you don't want this behavior, just pass
 * InvalidOid for actual_datatype.)
 */
static void
get_opclass_name(Oid opclass, Oid actual_datatype,
				 StringInfo buf)
{
	HeapTuple	ht_opc;
	Form_pg_opclass opcrec;
	char	   *opcname;
	char	   *nspname;
	bool		isvisible;

	/* Domains use their base type's default opclass */
	if (OidIsValid(actual_datatype))
		actual_datatype = getBaseType(actual_datatype);

	ht_opc = SearchSysCache(CLAOID,
							ObjectIdGetDatum(opclass),
							0, 0, 0);
	if (!HeapTupleIsValid(ht_opc))
		elog(ERROR, "cache lookup failed for opclass %u", opclass);
	opcrec = (Form_pg_opclass) GETSTRUCT(ht_opc);

	/*
	 * Special case for ARRAY_OPS: pretend it is default for any array type
	 */
	if (OidIsValid(actual_datatype))
	{
		if (opcrec->opcintype == ANYARRAYOID &&
			OidIsValid(get_element_type(actual_datatype)))
			actual_datatype = opcrec->opcintype;
	}

	/* Must force use of opclass name if not in search path */
	isvisible = OpclassIsVisible(opclass);

	if (actual_datatype != opcrec->opcintype || !opcrec->opcdefault ||
		!isvisible)
	{
		/* Okay, we need the opclass name.	Do we need to qualify it? */
		opcname = NameStr(opcrec->opcname);
		if (isvisible)
			appendStringInfo(buf, " %s", quote_identifier(opcname));
		else
		{
			nspname = get_namespace_name(opcrec->opcnamespace);
			appendStringInfo(buf, " %s.%s",
							 quote_identifier(nspname),
							 quote_identifier(opcname));
		}
	}
	ReleaseSysCache(ht_opc);
}

/*
 * processIndirection - take care of array and subfield assignment
 *
 * We strip any top-level FieldStore or assignment ArrayRef nodes that
 * appear in the input, and return the subexpression that's to be assigned.
 * If printit is true, we also print out the appropriate decoration for the
 * base column name (that the caller just printed).
 */
static Node *
processIndirection(Node *node, deparse_context *context, bool printit)
{
	StringInfo	buf = context->buf;

	for (;;)
	{
		if (node == NULL)
			break;
		if (IsA(node, FieldStore))
		{
			FieldStore *fstore = (FieldStore *) node;
			Oid			typrelid;
			char	   *fieldname;

			/* lookup tuple type */
			typrelid = get_typ_typrelid(fstore->resulttype);
			if (!OidIsValid(typrelid))
				elog(ERROR, "argument type %s of FieldStore is not a tuple type",
					 format_type_be(fstore->resulttype));

			/*
			 * Print the field name.  Note we assume here that there's only
			 * one field being assigned to.  This is okay in stored rules but
			 * could be wrong in executable target lists.  Presently no
			 * problem since explain.c doesn't print plan targetlists, but
			 * someday may have to think of something ...
			 */
			fieldname = get_relid_attribute_name(typrelid,
											linitial_int(fstore->fieldnums));
			if (printit)
				appendStringInfo(buf, ".%s", quote_identifier(fieldname));

			/*
			 * We ignore arg since it should be an uninteresting reference to
			 * the target column or subcolumn.
			 */
			node = (Node *) linitial(fstore->newvals);
		}
		else if (IsA(node, ArrayRef))
		{
			ArrayRef   *aref = (ArrayRef *) node;

			if (aref->refassgnexpr == NULL)
				break;
			if (printit)
				printSubscripts(aref, context);

			/*
			 * We ignore refexpr since it should be an uninteresting reference
			 * to the target column or subcolumn.
			 */
			node = (Node *) aref->refassgnexpr;
		}
		else
			break;
	}

	return node;
}

static void
printSubscripts(ArrayRef *aref, deparse_context *context)
{
	StringInfo	buf = context->buf;
	ListCell   *lowlist_item;
	ListCell   *uplist_item;

	lowlist_item = list_head(aref->reflowerindexpr);	/* could be NULL */
	foreach(uplist_item, aref->refupperindexpr)
	{
		appendStringInfoChar(buf, '[');
		if (lowlist_item)
		{
			get_rule_expr((Node *) lfirst(lowlist_item), context, false);
			appendStringInfoChar(buf, ':');
			lowlist_item = lnext(lowlist_item);
		}
		get_rule_expr((Node *) lfirst(uplist_item), context, false);
		appendStringInfoChar(buf, ']');
	}
}

/*
 * quote_identifier			- Quote an identifier only if needed
 *
 * When quotes are needed, we palloc the required space; slightly
 * space-wasteful but well worth it for notational simplicity.
 */
const char *
quote_identifier(const char *ident)
{
	/*
	 * Can avoid quoting if ident starts with a lowercase letter or underscore
	 * and contains only lowercase letters, digits, and underscores, *and* is
	 * not any SQL keyword.  Otherwise, supply quotes.
	 */
	int			nquotes = 0;
	bool		safe;
	const char *ptr;
	char	   *result;
	char	   *optr;

	/*
	 * would like to use <ctype.h> macros here, but they might yield unwanted
	 * locale-specific results...
	 */
	safe = ((ident[0] >= 'a' && ident[0] <= 'z') || ident[0] == '_');

	for (ptr = ident; *ptr; ptr++)
	{
		char		ch = *ptr;

		if ((ch >= 'a' && ch <= 'z') ||
			(ch >= '0' && ch <= '9') ||
			(ch == '_'))
		{
			/* okay */
		}
		else
		{
			safe = false;
			if (ch == '"')
				nquotes++;
		}
	}

	if (safe)
	{
		/*
		 * Check for keyword.  This test is overly strong, since many of the
		 * "keywords" known to the parser are usable as column names, but the
		 * parser doesn't provide any easy way to test for whether an
		 * identifier is safe or not... so be safe not sorry.
		 *
		 * Note: ScanKeywordLookup() does case-insensitive comparison, but
		 * that's fine, since we already know we have all-lower-case.
		 */
		if (ScanKeywordLookup(ident) != NULL)
			safe = false;
	}

	if (safe)
		return ident;			/* no change needed */

	result = (char *) palloc(strlen(ident) + nquotes + 2 + 1);

	optr = result;
	*optr++ = '"';
	for (ptr = ident; *ptr; ptr++)
	{
		char		ch = *ptr;

		if (ch == '"')
			*optr++ = '"';
		*optr++ = ch;
	}
	*optr++ = '"';
	*optr = '\0';

	return result;
}

/*
 * quote_qualified_identifier	- Quote a possibly-qualified identifier
 *
 * Return a name of the form namespace.ident, or just ident if namespace
 * is NULL, quoting each component if necessary.  The result is palloc'd.
 */
char *
quote_qualified_identifier(const char *namespace,
						   const char *ident)
{
	StringInfoData buf;

	initStringInfo(&buf);
	if (namespace)
		appendStringInfo(&buf, "%s.", quote_identifier(namespace));
	appendStringInfoString(&buf, quote_identifier(ident));
	return buf.data;
}

/*
 * generate_relation_name
 *		Compute the name to display for a relation specified by OID
 *
 * The result includes all necessary quoting and schema-prefixing.
 */
static char *
generate_relation_name(Oid relid)
{
	HeapTuple	tp;
	Form_pg_class reltup;
	char	   *nspname;
	char	   *result;

	tp = SearchSysCache(RELOID,
						ObjectIdGetDatum(relid),
						0, 0, 0);
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	reltup = (Form_pg_class) GETSTRUCT(tp);

	/* Qualify the name if not visible in search path */
	if (RelationIsVisible(relid))
		nspname = NULL;
	else
		nspname = get_namespace_name(reltup->relnamespace);

	result = quote_qualified_identifier(nspname, NameStr(reltup->relname));

	ReleaseSysCache(tp);

	return result;
}

/*
 * generate_function_name
 *		Compute the name to display for a function specified by OID,
 *		given that it is being called with the specified actual arg types.
 *		(Arg types matter because of ambiguous-function resolution rules.)
 *
 * The result includes all necessary quoting and schema-prefixing.
 */
static char *
generate_function_name(Oid funcid, int nargs, Oid *argtypes)
{
	HeapTuple	proctup;
	Form_pg_proc procform;
	char	   *proname;
	char	   *nspname;
	char	   *result;
	FuncDetailCode p_result;
	Oid			p_funcid;
	Oid			p_rettype;
	bool		p_retset;
	Oid		   *p_true_typeids;

	proctup = SearchSysCache(PROCOID,
							 ObjectIdGetDatum(funcid),
							 0, 0, 0);
	if (!HeapTupleIsValid(proctup))
		elog(ERROR, "cache lookup failed for function %u", funcid);
	procform = (Form_pg_proc) GETSTRUCT(proctup);
	proname = NameStr(procform->proname);
	Assert(nargs == procform->pronargs);

	/*
	 * The idea here is to schema-qualify only if the parser would fail to
	 * resolve the correct function given the unqualified func name with the
	 * specified argtypes.
	 */
	p_result = func_get_detail(list_make1(makeString(proname)),
							   NIL, nargs, argtypes,
							   &p_funcid, &p_rettype,
							   &p_retset, &p_true_typeids);
	if ((p_result == FUNCDETAIL_NORMAL || p_result == FUNCDETAIL_AGGREGATE) &&
		p_funcid == funcid)
		nspname = NULL;
	else
		nspname = get_namespace_name(procform->pronamespace);

	result = quote_qualified_identifier(nspname, proname);

	ReleaseSysCache(proctup);

	return result;
}

/*
 * generate_operator_name
 *		Compute the name to display for an operator specified by OID,
 *		given that it is being called with the specified actual arg types.
 *		(Arg types matter because of ambiguous-operator resolution rules.
 *		Pass InvalidOid for unused arg of a unary operator.)
 *
 * The result includes all necessary quoting and schema-prefixing,
 * plus the OPERATOR() decoration needed to use a qualified operator name
 * in an expression.
 */
static char *
generate_operator_name(Oid operid, Oid arg1, Oid arg2)
{
	StringInfoData buf;
	HeapTuple	opertup;
	Form_pg_operator operform;
	char	   *oprname;
	char	   *nspname;
	Operator	p_result;

	initStringInfo(&buf);

	opertup = SearchSysCache(OPEROID,
							 ObjectIdGetDatum(operid),
							 0, 0, 0);
	if (!HeapTupleIsValid(opertup))
		elog(ERROR, "cache lookup failed for operator %u", operid);
	operform = (Form_pg_operator) GETSTRUCT(opertup);
	oprname = NameStr(operform->oprname);

	/*
	 * The idea here is to schema-qualify only if the parser would fail to
	 * resolve the correct operator given the unqualified op name with the
	 * specified argtypes.
	 */
	switch (operform->oprkind)
	{
		case 'b':
			p_result = oper(NULL, list_make1(makeString(oprname)), arg1, arg2,
							true, -1);
			break;
		case 'l':
			p_result = left_oper(NULL, list_make1(makeString(oprname)), arg2,
								 true, -1);
			break;
		case 'r':
			p_result = right_oper(NULL, list_make1(makeString(oprname)), arg1,
								  true, -1);
			break;
		default:
			elog(ERROR, "unrecognized oprkind: %d", operform->oprkind);
			p_result = NULL;	/* keep compiler quiet */
			break;
	}

	if (p_result != NULL && oprid(p_result) == operid)
		nspname = NULL;
	else
	{
		nspname = get_namespace_name(operform->oprnamespace);
		appendStringInfo(&buf, "OPERATOR(%s.", quote_identifier(nspname));
	}

	appendStringInfoString(&buf, oprname);

	if (nspname)
		appendStringInfoChar(&buf, ')');

	if (p_result != NULL)
		ReleaseSysCache(p_result);

	ReleaseSysCache(opertup);

	return buf.data;
}

/*
 * Given a C string, produce a TEXT datum.
 *
 * We assume that the input was palloc'd and may be freed.
 */
static text *
string_to_text(char *str)
{
	text	   *result;
	int			slen = strlen(str);
	int			tlen;

	tlen = slen + VARHDRSZ;
	result = (text *) palloc(tlen);
	VARATT_SIZEP(result) = tlen;
	memcpy(VARDATA(result), str, slen);

	pfree(str);

	return result;
}

/*
 * Generate a C string representing a relation's reloptions, or NULL if none.
 */
static char *
flatten_reloptions(Oid relid)
{
	char	   *result = NULL;
	HeapTuple	tuple;
	Datum		reloptions;
	bool		isnull;

	tuple = SearchSysCache(RELOID,
						   ObjectIdGetDatum(relid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);

	reloptions = SysCacheGetAttr(RELOID, tuple,
								 Anum_pg_class_reloptions, &isnull);
	if (!isnull)
	{
		Datum		sep,
					txt;

		/*
		 * We want to use array_to_text(reloptions, ', ') --- but
		 * DirectFunctionCall2(array_to_text) does not work, because
		 * array_to_text() relies on flinfo to be valid.  So use
		 * OidFunctionCall2.
		 */
		sep = DirectFunctionCall1(textin, CStringGetDatum(", "));
		txt = OidFunctionCall2(F_ARRAY_TO_TEXT, reloptions, sep);
		result = DatumGetCString(DirectFunctionCall1(textout, txt));
	}

	ReleaseSysCache(tuple);

	return result;
}
