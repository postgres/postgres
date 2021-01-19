/*-------------------------------------------------------------------------
 *
 * execCurrent.c
 *	  executor support for WHERE CURRENT OF cursor
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	src/backend/executor/execCurrent.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relscan.h"
#include "access/sysattr.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/portal.h"
#include "utils/rel.h"


static char *fetch_cursor_param_value(ExprContext *econtext, int paramId);
static ScanState *search_plan_tree(PlanState *node, Oid table_oid,
								   bool *pending_rescan);


/*
 * execCurrentOf
 *
 * Given a CURRENT OF expression and the OID of a table, determine which row
 * of the table is currently being scanned by the cursor named by CURRENT OF,
 * and return the row's TID into *current_tid.
 *
 * Returns true if a row was identified.  Returns false if the cursor is valid
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
	queryDesc = portal->queryDesc;
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
	if (queryDesc->estate->es_rowmarks)
	{
		ExecRowMark *erm;
		Index		i;

		/*
		 * Here, the query must have exactly one FOR UPDATE/SHARE reference to
		 * the target table, and we dig the ctid info out of that.
		 */
		erm = NULL;
		for (i = 0; i < queryDesc->estate->es_range_table_size; i++)
		{
			ExecRowMark *thiserm = queryDesc->estate->es_rowmarks[i];

			if (thiserm == NULL ||
				!RowMarkRequiresRowShareLock(thiserm->markType))
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
		/*
		 * Without FOR UPDATE, we dig through the cursor's plan to find the
		 * scan node.  Fail if it's not there or buried underneath
		 * aggregation.
		 */
		ScanState  *scanstate;
		bool		pending_rescan = false;

		scanstate = search_plan_tree(queryDesc->planstate, table_oid,
									 &pending_rescan);
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

		/*
		 * Now OK to return false if we found an inactive scan.  It is
		 * inactive either if it's not positioned on a row, or there's a
		 * rescan pending for it.
		 */
		if (TupIsNull(scanstate->ss_ScanTupleSlot) || pending_rescan)
			return false;

		/*
		 * Extract TID of the scan's current row.  The mechanism for this is
		 * in principle scan-type-dependent, but for most scan types, we can
		 * just dig the TID out of the physical scan tuple.
		 */
		if (IsA(scanstate, IndexOnlyScanState))
		{
			/*
			 * For IndexOnlyScan, the tuple stored in ss_ScanTupleSlot may be
			 * a virtual tuple that does not have the ctid column, so we have
			 * to get the TID from xs_ctup.t_self.
			 */
			IndexScanDesc scan = ((IndexOnlyScanState *) scanstate)->ioss_ScanDesc;

			*current_tid = scan->xs_heaptid;
		}
		else
		{
			/*
			 * Default case: try to fetch TID from the scan node's current
			 * tuple.  As an extra cross-check, verify tableoid in the current
			 * tuple.  If the scan hasn't provided a physical tuple, we have
			 * to fail.
			 */
			Datum		ldatum;
			bool		lisnull;
			ItemPointer tuple_tid;

#ifdef USE_ASSERT_CHECKING
			ldatum = slot_getsysattr(scanstate->ss_ScanTupleSlot,
									 TableOidAttributeNumber,
									 &lisnull);
			if (lisnull)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_CURSOR_STATE),
						 errmsg("cursor \"%s\" is not a simply updatable scan of table \"%s\"",
								cursor_name, table_name)));
			Assert(DatumGetObjectId(ldatum) == table_oid);
#endif

			ldatum = slot_getsysattr(scanstate->ss_ScanTupleSlot,
									 SelfItemPointerAttributeNumber,
									 &lisnull);
			if (lisnull)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_CURSOR_STATE),
						 errmsg("cursor \"%s\" is not a simply updatable scan of table \"%s\"",
								cursor_name, table_name)));
			tuple_tid = (ItemPointer) DatumGetPointer(ldatum);

			*current_tid = *tuple_tid;
		}

		Assert(ItemPointerIsValid(current_tid));

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
		ParamExternData *prm;
		ParamExternData prmdata;

		/* give hook a chance in case parameter is dynamic */
		if (paramInfo->paramFetch != NULL)
			prm = paramInfo->paramFetch(paramInfo, paramId, false, &prmdata);
		else
			prm = &paramInfo->params[paramId - 1];

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
 *
 * CAUTION: this function is not charged simply with finding some candidate
 * scan, but with ensuring that that scan returned the plan tree's current
 * output row.  That's why we must reject multiple-match cases.
 *
 * If a candidate is found, set *pending_rescan to true if that candidate
 * or any node above it has a pending rescan action, i.e. chgParam != NULL.
 * That indicates that we shouldn't consider the node to be positioned on a
 * valid tuple, even if its own state would indicate that it is.  (Caller
 * must initialize *pending_rescan to false, and should not trust its state
 * if multiple candidates are found.)
 */
static ScanState *
search_plan_tree(PlanState *node, Oid table_oid,
				 bool *pending_rescan)
{
	ScanState  *result = NULL;

	if (node == NULL)
		return NULL;
	switch (nodeTag(node))
	{
			/*
			 * Relation scan nodes can all be treated alike: check to see if
			 * they are scanning the specified table.
			 *
			 * ForeignScan and CustomScan might not have a currentRelation, in
			 * which case we just ignore them.  (We dare not descend to any
			 * child plan nodes they might have, since we do not know the
			 * relationship of such a node's current output tuple to the
			 * children's current outputs.)
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

				if (sstate->ss_currentRelation &&
					RelationGetRelid(sstate->ss_currentRelation) == table_oid)
					result = sstate;
				break;
			}

			/*
			 * For Append, we can check each input node.  It is safe to
			 * descend to the inputs because only the input that resulted in
			 * the Append's current output node could be positioned on a tuple
			 * at all; the other inputs are either at EOF or not yet started.
			 * Hence, if the desired table is scanned by some
			 * currently-inactive input node, we will find that node but then
			 * our caller will realize that it didn't emit the tuple of
			 * interest.
			 *
			 * We do need to watch out for multiple matches (possible if
			 * Append was from UNION ALL rather than an inheritance tree).
			 *
			 * Note: we can NOT descend through MergeAppend similarly, since
			 * its inputs are likely all active, and we don't know which one
			 * returned the current output tuple.  (Perhaps that could be
			 * fixed if we were to let this code know more about MergeAppend's
			 * internal state, but it does not seem worth the trouble.  Users
			 * should not expect plans for ORDER BY queries to be considered
			 * simply-updatable, since they won't be if the sorting is
			 * implemented by a Sort node.)
			 */
		case T_AppendState:
			{
				AppendState *astate = (AppendState *) node;
				int			i;

				for (i = 0; i < astate->as_nplans; i++)
				{
					ScanState  *elem = search_plan_tree(astate->appendplans[i],
														table_oid,
														pending_rescan);

					if (!elem)
						continue;
					if (result)
						return NULL;	/* multiple matches */
					result = elem;
				}
				break;
			}

			/*
			 * Result and Limit can be descended through (these are safe
			 * because they always return their input's current row)
			 */
		case T_ResultState:
		case T_LimitState:
			result = search_plan_tree(node->lefttree,
									  table_oid,
									  pending_rescan);
			break;

			/*
			 * SubqueryScan too, but it keeps the child in a different place
			 */
		case T_SubqueryScanState:
			result = search_plan_tree(((SubqueryScanState *) node)->subplan,
									  table_oid,
									  pending_rescan);
			break;

		default:
			/* Otherwise, assume we can't descend through it */
			break;
	}

	/*
	 * If we found a candidate at or below this node, then this node's
	 * chgParam indicates a pending rescan that will affect the candidate.
	 */
	if (result && node->chgParam != NULL)
		*pending_rescan = true;

	return result;
}
