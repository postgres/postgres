/*-------------------------------------------------------------------------
 *
 * test_plan_advice.c
 *	  Test pg_plan_advice by planning every query with generated advice.
 *
 * With this module loaded, every time a query is executed, we end up
 * planning it twice. The first time we plan it, we generate plan advice,
 * which we then feed back to pg_plan_advice as the supplied plan advice.
 * It is then planned a second time using that advice. This hopefully
 * allows us to detect cases where the advice is incorrect or causes
 * failures or plan changes for some reason.
 *
 * Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 *	  src/test/modules/test_plan_advice/test_plan_advice.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "fmgr.h"
#include "optimizer/optimizer.h"
#include "pg_plan_advice.h"
#include "utils/guc.h"

PG_MODULE_MAGIC;

static bool in_recursion = false;

static char *test_plan_advice_advisor(PlannerGlobal *glob,
									  Query *parse,
									  const char *query_string,
									  int cursorOptions,
									  ExplainState *es);
static DefElem *find_defelem_by_defname(List *deflist, char *defname);

/*
 * Initialize this module.
 */
void
_PG_init(void)
{
	void		(*add_advisor_fn) (pg_plan_advice_advisor_hook hook);

	/*
	 * Ask pg_plan_advice to get advice strings from test_plan_advice_advisor
	 */
	add_advisor_fn =
		load_external_function("pg_plan_advice", "pg_plan_advice_add_advisor",
							   true, NULL);

	(*add_advisor_fn) (test_plan_advice_advisor);
}

/*
 * Re-plan the given query and return the generated advice string as the
 * supplied advice.
 */
static char *
test_plan_advice_advisor(PlannerGlobal *glob, Query *parse,
						 const char *query_string, int cursorOptions,
						 ExplainState *es)
{
	PlannedStmt *pstmt;
	int			save_nestlevel = 0;
	DefElem    *pgpa_item;
	DefElem    *advice_string_item;

	/*
	 * Since this function is called from the planner and triggers planning,
	 * we need a recursion guard.
	 */
	if (in_recursion)
		return NULL;

	PG_TRY();
	{
		in_recursion = true;

		/*
		 * Planning can trigger expression evaluation, which can result in
		 * sending NOTICE messages or other output to the client. To avoid
		 * that, we set client_min_messages = ERROR in the hopes of getting
		 * the same output with and without this module.
		 *
		 * We also need to set pg_plan_advice.always_store_advice_details so
		 * that pg_plan_advice will generate an advice string, since the whole
		 * point of this function is to get access to that.
		 */
		save_nestlevel = NewGUCNestLevel();
		set_config_option("client_min_messages", "error",
						  PGC_SUSET, PGC_S_SESSION,
						  GUC_ACTION_SAVE, true, 0, false);
		set_config_option("pg_plan_advice.always_store_advice_details", "true",
						  PGC_SUSET, PGC_S_SESSION,
						  GUC_ACTION_SAVE, true, 0, false);

		/*
		 * Replan. We must copy the Query, because the planner modifies it.
		 * (As noted elsewhere, that's unfortunate; perhaps it will be fixed
		 * some day.)
		 */
		pstmt = planner(copyObject(parse), query_string, cursorOptions,
						glob->boundParams, es);
	}
	PG_FINALLY();
	{
		in_recursion = false;
	}
	PG_END_TRY();

	/* Roll back any GUC changes */
	if (save_nestlevel > 0)
		AtEOXact_GUC(false, save_nestlevel);

	/* Extract and return the advice string */
	pgpa_item = find_defelem_by_defname(pstmt->extension_state,
										"pg_plan_advice");
	if (pgpa_item == NULL)
		elog(ERROR, "extension state for pg_plan_advice not found");
	advice_string_item = find_defelem_by_defname((List *) pgpa_item->arg,
												 "advice_string");
	if (advice_string_item == NULL)
		elog(ERROR,
			 "advice string for pg_plan_advice not found in extension state");
	return strVal(advice_string_item->arg);
}

/*
 * Search a list of DefElem objects for a given defname.
 */
static DefElem *
find_defelem_by_defname(List *deflist, char *defname)
{
	foreach_node(DefElem, item, deflist)
	{
		if (strcmp(item->defname, defname) == 0)
			return item;
	}

	return NULL;
}
