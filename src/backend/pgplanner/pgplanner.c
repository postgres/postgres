/*-------------------------------------------------------------------------
 *
 * pgplanner.c
 *	  Standalone PostgreSQL planner library implementation.
 *
 *	  Manages callback registration, thread-safe planning, and conversion
 *	  of callback results into PostgreSQL internal structures.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <pthread.h>

#include "pgplanner/pgplanner.h"
#include "access/htup_details.h"
#include "access/tupdesc.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type_d.h"
#include "parser/analyze.h"
#include "tcop/tcopprot.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/* ----------------------------------------------------------------
 *	Global state (protected by planner_mutex)
 * ----------------------------------------------------------------
 */
static pthread_mutex_t planner_mutex = PTHREAD_MUTEX_INITIALIZER;
static const PgPlannerCallbacks *current_callbacks = NULL;

/* ----------------------------------------------------------------
 *	pgplanner_init
 *
 *	Call once at startup before any planning calls.
 * ----------------------------------------------------------------
 */
void
pgplanner_init(void)
{
	MemoryContextInit();
}

/* ----------------------------------------------------------------
 *	pgplanner_get_callbacks
 *
 *	Returns the currently active callbacks. Only valid during a
 *	pgplanner_plan_query() call. Errors if called outside planning.
 * ----------------------------------------------------------------
 */
const PgPlannerCallbacks *
pgplanner_get_callbacks(void)
{
	if (current_callbacks == NULL)
		elog(ERROR, "pgplanner: no callbacks registered (called outside planning?)");
	return current_callbacks;
}

/* ----------------------------------------------------------------
 *	pgplanner_build_relation
 *
 *	Convert a PgPlannerRelationInfo into a PostgreSQL Relation struct.
 * ----------------------------------------------------------------
 */
Relation
pgplanner_build_relation(const PgPlannerRelationInfo *info)
{
	Relation		r;
	FormData_pg_class *form;
	int				i;

	r = palloc0(sizeof(RelationData));
	form = palloc0(sizeof(FormData_pg_class));

	r->rd_rel = form;
	r->rd_rel->relkind = info->relkind;
	r->rd_id = info->relid;
	r->rd_rel->relnatts = info->natts;
	strlcpy(NameStr(r->rd_rel->relname), info->relname, NAMEDATALEN);

	r->rd_att = CreateTemplateTupleDesc(info->natts);

	for (i = 0; i < info->natts; i++)
	{
		TupleDescInitBuiltinEntry(r->rd_att,
								  (AttrNumber) (i + 1),
								  info->columns[i].colname,
								  info->columns[i].typid,
								  info->columns[i].typmod,
								  0);
	}

	return r;
}

/* ----------------------------------------------------------------
 *	pgplanner_plan_query
 *
 *	Parse, analyze, and plan a SQL string. Thread-safe: acquires
 *	planner_mutex for the duration of the call.
 * ----------------------------------------------------------------
 */
PlannedStmt *
pgplanner_plan_query(const char *sql, const PgPlannerCallbacks *callbacks)
{
	List		*raw_parsetree_list;
	RawStmt		*parsetree;
	Query		*query;
	PlannedStmt	*result;

	pthread_mutex_lock(&planner_mutex);
	current_callbacks = callbacks;

	/* Parse */
	raw_parsetree_list = pg_parse_query(sql);
	if (list_length(raw_parsetree_list) != 1)
	{
		current_callbacks = NULL;
		pthread_mutex_unlock(&planner_mutex);
		elog(ERROR, "pgplanner: expected exactly 1 statement, got %d",
			 list_length(raw_parsetree_list));
	}

	parsetree = linitial_node(RawStmt, raw_parsetree_list);

	/* Analyze */
	query = parse_analyze_fixedparams(parsetree, sql, NULL, 0, NULL);

	/* Plan */
	result = pg_plan_query(query, sql, CURSOR_OPT_PARALLEL_OK, NULL);

	current_callbacks = NULL;
	pthread_mutex_unlock(&planner_mutex);

	return result;
}
