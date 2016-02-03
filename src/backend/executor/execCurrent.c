/*-------------------------------------------------------------------------
 *
 * execCurrent.c
 *	  executor support for WHERE CURRENT OF cursor
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	src/backend/executor/execCurrent.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/sysattr.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/portal.h"
#include "utils/rel.h"


static char *fetch_cursor_param_value(ExprContext *econtext, int paramId);
static ScanState *search_plan_tree(PlanState *node, Oid table_oid);


/*
 * execCurrentOf
 *
 * Given a CURRENT OF expression and the OID of a table, determine which row
 * of the table is currently being scanned by the cursor named by CURRENT OF,
 * and return the row's TID into *current_tid.
 *
 * Returns TRUE if a row was identified.  Returns FALSE if the cursor is valid
 * for the table but is not currently scanning a row of the table (this is a
 * legal situation in inheritance cases).  Raises error if cursor is not a
 * valid updatable scan of the specified table.
 */
bool
execCurrentOf(CurrentOfExpr *cexpr,
			  ExprContext *econtext,
			  Oid table_oid,
			  ItemPointer current_tid)
{
	char	   *cursor_name;
	char	   *table_name;
	Portal		portal;
	QueryDesc  *queryDesc;

	/* Get the cursor name --- may have to look up a parameter reference */
	if (cexpr->cursor_name)
		cursor_name = cexpr->cursor_name;
	else
		cursor_name = fetch_cursor_param_value(econtext, cexpr->cursor_param);

	/* Fetch table name for possible use in error messages */
	table_name = get_rel_name(table_oid);
	if (table_name == NULL)
		elog(ERROR, "cache lookup failed for relation %u", table_oid);

	/* Find the cursor's portal */
	portal = GetPortalByName(cursor_name);
	if (!PortalIsValid(portal))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_CURSOR),
				 errmsg("cursor \"%s\" does not exist", cursor_name)));

	/*
	 * We have to watch out for non-SELECT queries as well as held cursors,
	 * both of which may have null queryDesc.
	 */
	if (portal->strategy != PORTAL_ONE_SELECT)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_STATE),
				 errmsg("cursor \"%s\" is not a SELECT query",
						cursor_name)));
	queryDesc = PortalGetQueryDesc(portal);
	if (queryDesc == NULL || queryDesc->estate == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_STATE),
				 errmsg("cursor \"%s\" is held from a previous transaction",
						cursor_name)));

	/*
	 * We have two different strategies depending on whether the cursor uses
	 * FOR UPDATE/SHARE or not.  The reason for supporting both is that the
	 * FOR UPDATE code is able to identify a target table in many cases where
	 * the other code can't, while the non-FOR-UPDATE case allows use of WHERE
	 * CURRENT OF with an insensitive cursor.
	 */
	if (queryDesc->estate->es_rowMarks)
	{
		ExecRowMark *erm;
		ListCell   *lc;

		/*
		 * Here, the query must have exactly one FOR UPDATE/SHARE reference to
		 * the target table, and we dig the ctid info out of that.
		 */
		erm = NULL;
		foreach(lc, queryDesc->estate->es_rowMarks)
		{
			ExecRowMark *thiserm = (ExecRowMark *) lfirst(lc);

			if (!RowMarkRequiresRowShareLock(thiserm->markType))
				continue;		/* ignore non-FOR UPDATE/SHARE items */

			if (thiserm->relid == table_oid)
			{
				if (erm)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_CURSOR_STATE),
							 errmsg("cursor \"%s\" has multiple FOR UPDATE/SHARE references to table \"%s\"",
									cursor_name, table_name)));
				erm = thiserm;
			}
		}

		if (erm == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_CURSOR_STATE),
					 errmsg("cursor \"%s\" does not have a FOR UPDATE/SHARE reference to table \"%s\"",
							cursor_name, table_name)));

		/*
		 * The cursor must have a current result row: per the SQL spec, it's
		 * an error if not.
		 */
		if (portal->atStart || portal->atEnd)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_CURSOR_STATE),
					 errmsg("cursor \"%s\" is not positioned on a row",
							cursor_name)));

		/* Return the currently scanned TID, if there is one */
		if (ItemPointerIsValid(&(erm->curCtid)))
		{
			*current_tid = erm->curCtid;
			return true;
		}

		/*
		 * This table didn't produce the cursor's current row; some other
		 * inheritance child of the same parent must have.  Signal caller to
		 * do nothing on this table.
		 */
		return false;
	}
	else
	{
		ScanState  *scanstate;
		bool		lisnull;
		Oid tuple_tableoid PG_USED_FOR_ASSERTS_ONLY;
		ItemPointer tuple_tid;

		/*
		 * Without FOR UPDATE, we dig through the cursor's plan to find the
		 * scan node.  Fail if it's not there or buried underneath
		 * aggregation.
		 */
		scanstate = search_plan_tree(queryDesc->planstate, table_oid);
		if (!scanstate)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_CURSOR_STATE),
					 errmsg("cursor \"%s\" is not a simply updatable scan of table \"%s\"",
							cursor_name, table_name)));

		/*
		 * The cursor must have a current result row: per the SQL spec, it's
		 * an error if not.  We test this at the top level, rather than at the
		 * scan node level, because in inheritance cases any one table scan
		 * could easily not be on a row. We want to return false, not raise
		 * error, if the passed-in table OID is for one of the inactive scans.
		 */
		if (portal->atStart || portal->atEnd)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_CURSOR_STATE),
					 errmsg("cursor \"%s\" is not positioned on a row",
							cursor_name)));

		/* Now OK to return false if we found an inactive scan */
		if (TupIsNull(scanstate->ss_ScanTupleSlot))
			return false;

		/* Use slot_getattr to catch any possible mistakes */
		tuple_tableoid =
			DatumGetObjectId(slot_getattr(scanstate->ss_ScanTupleSlot,
										  TableOidAttributeNumber,
										  &lisnull));
		Assert(!lisnull);
		tuple_tid = (ItemPointer)
			DatumGetPointer(slot_getattr(scanstate->ss_ScanTupleSlot,
										 SelfItemPointerAttributeNumber,
										 &lisnull));
		Assert(!lisnull);

		Assert(tuple_tableoid == table_oid);

		*current_tid = *tuple_tid;

		return true;
	}
}

/*
 * fetch_cursor_param_value
 *
 * Fetch the string value of a param, verifying it is of type REFCURSOR.
 */
static char *
fetch_cursor_param_value(ExprContext *econtext, int paramId)
{
	ParamListInfo paramInfo = econtext->ecxt_param_list_info;

	if (paramInfo &&
		paramId > 0 && paramId <= paramInfo->numParams)
	{
		ParamExternData *prm = &paramInfo->params[paramId - 1];

		/* give hook a chance in case parameter is dynamic */
		if (!OidIsValid(prm->ptype) && paramInfo->paramFetch != NULL)
			(*paramInfo->paramFetch) (paramInfo, paramId);

		if (OidIsValid(prm->ptype) && !prm->isnull)
		{
			/* safety check in case hook did something unexpected */
			if (prm->ptype != REFCURSOROID)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("type of parameter %d (%s) does not match that when preparing the plan (%s)",
								paramId,
								format_type_be(prm->ptype),
								format_type_be(REFCURSOROID))));

			/* We know that refcursor uses text's I/O routines */
			return TextDatumGetCString(prm->value);
		}
	}

	ereport(ERROR,
			(errcode(ERRCODE_UNDEFINED_OBJECT),
			 errmsg("no value found for parameter %d", paramId)));
	return NULL;
}

/*
 * search_plan_tree
 *
 * Search through a PlanState tree for a scan node on the specified table.
 * Return NULL if not found or multiple candidates.
 */
static ScanState *
search_plan_tree(PlanState *node, Oid table_oid)
{
	if (node == NULL)
		return NULL;
	switch (nodeTag(node))
	{
			/*
			 * Relation scan nodes can all be treated alike
			 */
		case T_SeqScanState:
		case T_SampleScanState:
		case T_IndexScanState:
		case T_IndexOnlyScanState:
		case T_BitmapHeapScanState:
		case T_TidScanState:
		case T_ForeignScanState:
		case T_CustomScanState:
			{
				ScanState  *sstate = (ScanState *) node;

				if (RelationGetRelid(sstate->ss_currentRelation) == table_oid)
					return sstate;
				break;
			}

			/*
			 * For Append, we must look through the members; watch out for
			 * multiple matches (possible if it was from UNION ALL)
			 */
		case T_AppendState:
			{
				AppendState *astate = (AppendState *) node;
				ScanState  *result = NULL;
				int			i;

				for (i = 0; i < astate->as_nplans; i++)
				{
					ScanState  *elem = search_plan_tree(astate->appendplans[i],
														table_oid);

					if (!elem)
						continue;
					if (result)
						return NULL;	/* multiple matches */
					result = elem;
				}
				return result;
			}

			/*
			 * Similarly for MergeAppend
			 */
		case T_MergeAppendState:
			{
				MergeAppendState *mstate = (MergeAppendState *) node;
				ScanState  *result = NULL;
				int			i;

				for (i = 0; i < mstate->ms_nplans; i++)
				{
					ScanState  *elem = search_plan_tree(mstate->mergeplans[i],
														table_oid);

					if (!elem)
						continue;
					if (result)
						return NULL;	/* multiple matches */
					result = elem;
				}
				return result;
			}

			/*
			 * Result and Limit can be descended through (these are safe
			 * because they always return their input's current row)
			 */
		case T_ResultState:
		case T_LimitState:
			return search_plan_tree(node->lefttree, table_oid);

			/*
			 * SubqueryScan too, but it keeps the child in a different place
			 */
		case T_SubqueryScanState:
			return search_plan_tree(((SubqueryScanState *) node)->subplan,
									table_oid);

		default:
			/* Otherwise, assume we can't descend through it */
			break;
	}
	return NULL;
}
