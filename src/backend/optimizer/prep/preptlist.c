/*-------------------------------------------------------------------------
 *
 * preptlist.c
 *	  Routines to preprocess the parse tree target list
 *
 * This module takes care of altering the query targetlist as needed for
 * INSERT, UPDATE, and DELETE queries.	For INSERT and UPDATE queries,
 * the targetlist must contain an entry for each attribute of the target
 * relation in the correct order.  For both UPDATE and DELETE queries,
 * we need a junk targetlist entry holding the CTID attribute --- the
 * executor relies on this to find the tuple to be replaced/deleted.
 * We may also need junk tlist entries for Vars used in the RETURNING list.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/prep/preptlist.c,v 1.84 2006/10/04 00:29:55 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "optimizer/prep.h"
#include "optimizer/subselect.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "parser/analyze.h"
#include "parser/parsetree.h"
#include "parser/parse_coerce.h"


static List *expand_targetlist(List *tlist, int command_type,
				  Index result_relation, List *range_table);


/*
 * preprocess_targetlist
 *	  Driver for preprocessing the parse tree targetlist.
 *
 *	  Returns the new targetlist.
 */
List *
preprocess_targetlist(PlannerInfo *root, List *tlist)
{
	Query	   *parse = root->parse;
	int			result_relation = parse->resultRelation;
	List	   *range_table = parse->rtable;
	CmdType		command_type = parse->commandType;

	/*
	 * Sanity check: if there is a result relation, it'd better be a real
	 * relation not a subquery.  Else parser or rewriter messed up.
	 */
	if (result_relation)
	{
		RangeTblEntry *rte = rt_fetch(result_relation, range_table);

		if (rte->subquery != NULL || rte->relid == InvalidOid)
			elog(ERROR, "subquery cannot be result relation");
	}

	/*
	 * for heap_formtuple to work, the targetlist must match the exact order
	 * of the attributes. We also need to fill in any missing attributes. -ay
	 * 10/94
	 */
	if (command_type == CMD_INSERT || command_type == CMD_UPDATE)
		tlist = expand_targetlist(tlist, command_type,
								  result_relation, range_table);

	/*
	 * for "update" and "delete" queries, add ctid of the result relation into
	 * the target list so that the ctid will propagate through execution and
	 * ExecutePlan() will be able to identify the right tuple to replace or
	 * delete.	This extra field is marked "junk" so that it is not stored
	 * back into the tuple.
	 */
	if (command_type == CMD_UPDATE || command_type == CMD_DELETE)
	{
		TargetEntry *tle;
		Var		   *var;

		var = makeVar(result_relation, SelfItemPointerAttributeNumber,
					  TIDOID, -1, 0);

		tle = makeTargetEntry((Expr *) var,
							  list_length(tlist) + 1,
							  pstrdup("ctid"),
							  true);

		/*
		 * For an UPDATE, expand_targetlist already created a fresh tlist. For
		 * DELETE, better do a listCopy so that we don't destructively modify
		 * the original tlist (is this really necessary?).
		 */
		if (command_type == CMD_DELETE)
			tlist = list_copy(tlist);

		tlist = lappend(tlist, tle);
	}

	/*
	 * Add TID targets for rels selected FOR UPDATE/SHARE.	The executor uses
	 * the TID to know which rows to lock, much as for UPDATE or DELETE.
	 */
	if (parse->rowMarks)
	{
		ListCell   *l;

		/*
		 * We've got trouble if the FOR UPDATE/SHARE appears inside grouping,
		 * since grouping renders a reference to individual tuple CTIDs
		 * invalid.  This is also checked at parse time, but that's
		 * insufficient because of rule substitution, query pullup, etc.
		 */
		CheckSelectLocking(parse);

		/*
		 * Currently the executor only supports FOR UPDATE/SHARE at top level
		 */
		if (PlannerQueryLevel > 1)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			errmsg("SELECT FOR UPDATE/SHARE is not allowed in subqueries")));

		foreach(l, parse->rowMarks)
		{
			RowMarkClause *rc = (RowMarkClause *) lfirst(l);
			Var		   *var;
			char	   *resname;
			TargetEntry *tle;

			var = makeVar(rc->rti,
						  SelfItemPointerAttributeNumber,
						  TIDOID,
						  -1,
						  0);

			resname = (char *) palloc(32);
			snprintf(resname, 32, "ctid%u", rc->rti);

			tle = makeTargetEntry((Expr *) var,
								  list_length(tlist) + 1,
								  resname,
								  true);

			tlist = lappend(tlist, tle);
		}
	}

	/*
	 * If the query has a RETURNING list, add resjunk entries for any Vars
	 * used in RETURNING that belong to other relations.  We need to do this
	 * to make these Vars available for the RETURNING calculation.	Vars that
	 * belong to the result rel don't need to be added, because they will be
	 * made to refer to the actual heap tuple.
	 */
	if (parse->returningList && list_length(parse->rtable) > 1)
	{
		List	   *vars;
		ListCell   *l;

		vars = pull_var_clause((Node *) parse->returningList, false);
		foreach(l, vars)
		{
			Var		   *var = (Var *) lfirst(l);
			TargetEntry *tle;

			if (var->varno == result_relation)
				continue;		/* don't need it */

			if (tlist_member((Node *) var, tlist))
				continue;		/* already got it */

			tle = makeTargetEntry((Expr *) var,
								  list_length(tlist) + 1,
								  NULL,
								  true);

			tlist = lappend(tlist, tle);
		}
		list_free(vars);
	}

	return tlist;
}

/*****************************************************************************
 *
 *		TARGETLIST EXPANSION
 *
 *****************************************************************************/

/*
 * expand_targetlist
 *	  Given a target list as generated by the parser and a result relation,
 *	  add targetlist entries for any missing attributes, and ensure the
 *	  non-junk attributes appear in proper field order.
 *
 * NOTE: if you are tempted to put more processing here, consider whether
 * it shouldn't go in the rewriter's rewriteTargetList() instead.
 */
static List *
expand_targetlist(List *tlist, int command_type,
				  Index result_relation, List *range_table)
{
	List	   *new_tlist = NIL;
	ListCell   *tlist_item;
	Relation	rel;
	int			attrno,
				numattrs;

	tlist_item = list_head(tlist);

	/*
	 * The rewriter should have already ensured that the TLEs are in correct
	 * order; but we have to insert TLEs for any missing attributes.
	 *
	 * Scan the tuple description in the relation's relcache entry to make
	 * sure we have all the user attributes in the right order.  We assume
	 * that the rewriter already acquired at least AccessShareLock on the
	 * relation, so we need no lock here.
	 */
	rel = heap_open(getrelid(result_relation, range_table), NoLock);

	numattrs = RelationGetNumberOfAttributes(rel);

	for (attrno = 1; attrno <= numattrs; attrno++)
	{
		Form_pg_attribute att_tup = rel->rd_att->attrs[attrno - 1];
		TargetEntry *new_tle = NULL;

		if (tlist_item != NULL)
		{
			TargetEntry *old_tle = (TargetEntry *) lfirst(tlist_item);

			if (!old_tle->resjunk && old_tle->resno == attrno)
			{
				new_tle = old_tle;
				tlist_item = lnext(tlist_item);
			}
		}

		if (new_tle == NULL)
		{
			/*
			 * Didn't find a matching tlist entry, so make one.
			 *
			 * For INSERT, generate a NULL constant.  (We assume the rewriter
			 * would have inserted any available default value.) Also, if the
			 * column isn't dropped, apply any domain constraints that might
			 * exist --- this is to catch domain NOT NULL.
			 *
			 * For UPDATE, generate a Var reference to the existing value of
			 * the attribute, so that it gets copied to the new tuple. But
			 * generate a NULL for dropped columns (we want to drop any old
			 * values).
			 *
			 * When generating a NULL constant for a dropped column, we label
			 * it INT4 (any other guaranteed-to-exist datatype would do as
			 * well). We can't label it with the dropped column's datatype
			 * since that might not exist anymore.	It does not really matter
			 * what we claim the type is, since NULL is NULL --- its
			 * representation is datatype-independent.	This could perhaps
			 * confuse code comparing the finished plan to the target
			 * relation, however.
			 */
			Oid			atttype = att_tup->atttypid;
			int32		atttypmod = att_tup->atttypmod;
			Node	   *new_expr;

			switch (command_type)
			{
				case CMD_INSERT:
					if (!att_tup->attisdropped)
					{
						new_expr = (Node *) makeConst(atttype,
													  att_tup->attlen,
													  (Datum) 0,
													  true,		/* isnull */
													  att_tup->attbyval);
						new_expr = coerce_to_domain(new_expr,
													InvalidOid, -1,
													atttype,
													COERCE_IMPLICIT_CAST,
													false,
													false);
					}
					else
					{
						/* Insert NULL for dropped column */
						new_expr = (Node *) makeConst(INT4OID,
													  sizeof(int32),
													  (Datum) 0,
													  true,		/* isnull */
													  true /* byval */ );
					}
					break;
				case CMD_UPDATE:
					if (!att_tup->attisdropped)
					{
						new_expr = (Node *) makeVar(result_relation,
													attrno,
													atttype,
													atttypmod,
													0);
					}
					else
					{
						/* Insert NULL for dropped column */
						new_expr = (Node *) makeConst(INT4OID,
													  sizeof(int32),
													  (Datum) 0,
													  true,		/* isnull */
													  true /* byval */ );
					}
					break;
				default:
					elog(ERROR, "unrecognized command_type: %d",
						 (int) command_type);
					new_expr = NULL;	/* keep compiler quiet */
					break;
			}

			new_tle = makeTargetEntry((Expr *) new_expr,
									  attrno,
									  pstrdup(NameStr(att_tup->attname)),
									  false);
		}

		new_tlist = lappend(new_tlist, new_tle);
	}

	/*
	 * The remaining tlist entries should be resjunk; append them all to the
	 * end of the new tlist, making sure they have resnos higher than the last
	 * real attribute.	(Note: although the rewriter already did such
	 * renumbering, we have to do it again here in case we are doing an UPDATE
	 * in a table with dropped columns, or an inheritance child table with
	 * extra columns.)
	 */
	while (tlist_item)
	{
		TargetEntry *old_tle = (TargetEntry *) lfirst(tlist_item);

		if (!old_tle->resjunk)
			elog(ERROR, "targetlist is not sorted correctly");
		/* Get the resno right, but don't copy unnecessarily */
		if (old_tle->resno != attrno)
		{
			old_tle = flatCopyTargetEntry(old_tle);
			old_tle->resno = attrno;
		}
		new_tlist = lappend(new_tlist, old_tle);
		attrno++;
		tlist_item = lnext(tlist_item);
	}

	heap_close(rel, NoLock);

	return new_tlist;
}
