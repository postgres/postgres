/*-------------------------------------------------------------------------
 *
 * analyze.c--
 *	  transform the parse tree into a query tree
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/analyze.c,v 1.51 1997/11/26 01:11:03 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "postgres.h"
#include "access/heapam.h"
#include "nodes/makefuncs.h"
#include "nodes/memnodes.h"
#include "nodes/pg_list.h"
#include "parser/analyze.h"
#include "parser/parse_agg.h"
#include "parser/parse_clause.h"
#include "parser/parse_node.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "utils/builtins.h"
#include "utils/mcxt.h"

static Query *transformStmt(ParseState *pstate, Node *stmt);
static Query *transformDeleteStmt(ParseState *pstate, DeleteStmt *stmt);
static Query *transformInsertStmt(ParseState *pstate, AppendStmt *stmt);
static Query *transformIndexStmt(ParseState *pstate, IndexStmt *stmt);
static Query *transformExtendStmt(ParseState *pstate, ExtendStmt *stmt);
static Query *transformRuleStmt(ParseState *query, RuleStmt *stmt);
static Query *transformSelectStmt(ParseState *pstate, RetrieveStmt *stmt);
static Query *transformUpdateStmt(ParseState *pstate, ReplaceStmt *stmt);
static Query *transformCursorStmt(ParseState *pstate, CursorStmt *stmt);


/*
 * parse_analyze -
 *	  analyze a list of parse trees and transform them if necessary.
 *
 * Returns a list of transformed parse trees. Optimizable statements are
 * all transformed to Query while the rest stays the same.
 *
 * CALLER is responsible for freeing the QueryTreeList* returned
 */
QueryTreeList *
parse_analyze(List *pl)
{
	QueryTreeList *result;
	ParseState *pstate;
	int			i = 0;

	result = malloc(sizeof(QueryTreeList));
	result->len = length(pl);
	result->qtrees = (Query **) malloc(result->len * sizeof(Query *));

	while (pl != NIL)
	{
		pstate = make_parsestate();
		result->qtrees[i++] = transformStmt(pstate, lfirst(pl));
		pl = lnext(pl);
		if (pstate->p_target_relation != NULL)
			heap_close(pstate->p_target_relation);
		free(pstate);
	}

	return result;
}

/*
 * transformStmt -
 *	  transform a Parse tree. If it is an optimizable statement, turn it
 *	  into a Query tree.
 */
static Query *
transformStmt(ParseState *pstate, Node *parseTree)
{
	Query	   *result = NULL;

	switch (nodeTag(parseTree))
	{
			/*------------------------
			 *	Non-optimizable statements
			 *------------------------
			 */
		case T_IndexStmt:
			result = transformIndexStmt(pstate, (IndexStmt *) parseTree);
			break;

		case T_ExtendStmt:
			result = transformExtendStmt(pstate, (ExtendStmt *) parseTree);
			break;

		case T_RuleStmt:
			result = transformRuleStmt(pstate, (RuleStmt *) parseTree);
			break;

		case T_ViewStmt:
			{
				ViewStmt   *n = (ViewStmt *) parseTree;

				n->query = (Query *) transformStmt(pstate, (Node *) n->query);
				result = makeNode(Query);
				result->commandType = CMD_UTILITY;
				result->utilityStmt = (Node *) n;
			}
			break;

		case T_VacuumStmt:
			{
				MemoryContext oldcontext;

				/*
				 * make sure that this Query is allocated in TopMemory
				 * context because vacuum spans transactions and we don't
				 * want to lose the vacuum Query due to end-of-transaction
				 * free'ing
				 */
				oldcontext = MemoryContextSwitchTo(TopMemoryContext);
				result = makeNode(Query);
				result->commandType = CMD_UTILITY;
				result->utilityStmt = (Node *) parseTree;
				MemoryContextSwitchTo(oldcontext);
				break;

			}
		case T_ExplainStmt:
			{
				ExplainStmt *n = (ExplainStmt *) parseTree;

				result = makeNode(Query);
				result->commandType = CMD_UTILITY;
				n->query = transformStmt(pstate, (Node *) n->query);
				result->utilityStmt = (Node *) parseTree;
			}
			break;

			/*------------------------
			 *	Optimizable statements
			 *------------------------
			 */
		case T_AppendStmt:
			result = transformInsertStmt(pstate, (AppendStmt *) parseTree);
			break;

		case T_DeleteStmt:
			result = transformDeleteStmt(pstate, (DeleteStmt *) parseTree);
			break;

		case T_ReplaceStmt:
			result = transformUpdateStmt(pstate, (ReplaceStmt *) parseTree);
			break;

		case T_CursorStmt:
			result = transformCursorStmt(pstate, (CursorStmt *) parseTree);
			break;

		case T_RetrieveStmt:
			result = transformSelectStmt(pstate, (RetrieveStmt *) parseTree);
			break;

		default:

			/*
			 * other statments don't require any transformation-- just
			 * return the original parsetree
			 */
			result = makeNode(Query);
			result->commandType = CMD_UTILITY;
			result->utilityStmt = (Node *) parseTree;
			break;
	}
	return result;
}

/*
 * transformDeleteStmt -
 *	  transforms a Delete Statement
 */
static Query *
transformDeleteStmt(ParseState *pstate, DeleteStmt *stmt)
{
	Query	   *qry = makeNode(Query);

	qry->commandType = CMD_DELETE;

	/* set up a range table */
	makeRangeTable(pstate, stmt->relname, NULL);

	qry->uniqueFlag = NULL;

	/* fix where clause */
	qry->qual = transformWhereClause(pstate, stmt->whereClause);

	qry->rtable = pstate->p_rtable;
	qry->resultRelation = refnameRangeTablePosn(pstate->p_rtable, stmt->relname);

	/* make sure we don't have aggregates in the where clause */
	if (pstate->p_numAgg > 0)
		parseCheckAggregates(pstate, qry);

	return (Query *) qry;
}

/*
 * transformInsertStmt -
 *	  transform an Insert Statement
 */
static Query *
transformInsertStmt(ParseState *pstate, AppendStmt *stmt)
{
	Query	   *qry = makeNode(Query);	/* make a new query tree */
	List	   *icolumns;

	qry->commandType = CMD_INSERT;
	pstate->p_is_insert = true;

	/* set up a range table */
	makeRangeTable(pstate, stmt->relname, stmt->fromClause);

	qry->uniqueFlag = NULL;

	/* fix the target list */
	icolumns = pstate->p_insert_columns = makeTargetNames(pstate, stmt->cols);
	
	qry->targetList = transformTargetList(pstate, stmt->targetList);
	
	/* DEFAULT handling */
	if (length(qry->targetList) < pstate->p_target_relation->rd_att->natts &&
		pstate->p_target_relation->rd_att->constr &&
		pstate->p_target_relation->rd_att->constr->num_defval > 0)
	{
		AttributeTupleForm	   *att = pstate->p_target_relation->rd_att->attrs;
		AttrDefault			   *defval = pstate->p_target_relation->rd_att->constr->defval;
		int						ndef = pstate->p_target_relation->rd_att->constr->num_defval;
		
		/* 
		 * if stmt->cols == NIL then makeTargetNames returns list of all 
		 * attrs: have to shorter icolumns list...
		 */
		if (stmt->cols == NIL)
		{
			List   *extrl;
			int		i = length(qry->targetList);
			
			foreach (extrl, icolumns)
			{
				if (--i <= 0)
					break;
			}
			freeList (lnext(extrl));
			lnext(extrl) = NIL;
		}
		
		while (ndef-- > 0)
		{
			List		   *tl;
			Ident		   *id;
			TargetEntry	   *te;
			
			foreach (tl, icolumns)
			{
				id = (Ident *) lfirst(tl);
				if (!namestrcmp(&(att[defval[ndef].adnum - 1]->attname), id->name))
					break;
			}
			if (tl != NIL)		/* something given for this attr */
				continue;
			/* 
			 * Nothing given for this attr with DEFAULT expr, so
			 * add new TargetEntry to qry->targetList. 
			 * Note, that we set resno to defval[ndef].adnum:
			 * it's what transformTargetList()->make_targetlist_expr()
			 * does for INSERT ... SELECT. But for INSERT ... VALUES
			 * pstate->p_last_resno is used. It doesn't matter for 
			 * "normal" using (planner creates proper target list
			 * in preptlist.c), but may break RULEs in some way.
			 * It seems better to create proper target list here...
			 */
			te = makeNode(TargetEntry);
			te->resdom = makeResdom(defval[ndef].adnum,
									att[defval[ndef].adnum - 1]->atttypid,
									att[defval[ndef].adnum - 1]->attlen,
									pstrdup(nameout(&(att[defval[ndef].adnum - 1]->attname))),
									0, 0, 0);
			te->fjoin = NULL;
			te->expr = (Node *) stringToNode(defval[ndef].adbin);
			qry->targetList = lappend (qry->targetList, te);
		}
	}
	
	/* fix where clause */
	qry->qual = transformWhereClause(pstate, stmt->whereClause);

	/* now the range table will not change */
	qry->rtable = pstate->p_rtable;
	qry->resultRelation = refnameRangeTablePosn(pstate->p_rtable, stmt->relname);

	if (pstate->p_numAgg > 0)
		finalizeAggregates(pstate, qry);

	return (Query *) qry;
}

/*
 * transformIndexStmt -
 *	  transforms the qualification of the index statement
 */
static Query *
transformIndexStmt(ParseState *pstate, IndexStmt *stmt)
{
	Query	   *q;

	q = makeNode(Query);
	q->commandType = CMD_UTILITY;

	/* take care of the where clause */
	stmt->whereClause = transformWhereClause(pstate, stmt->whereClause);
	stmt->rangetable = pstate->p_rtable;

	q->utilityStmt = (Node *) stmt;

	return q;
}

/*
 * transformExtendStmt -
 *	  transform the qualifications of the Extend Index Statement
 *
 */
static Query *
transformExtendStmt(ParseState *pstate, ExtendStmt *stmt)
{
	Query	   *q;

	q = makeNode(Query);
	q->commandType = CMD_UTILITY;

	/* take care of the where clause */
	stmt->whereClause = transformWhereClause(pstate, stmt->whereClause);
	stmt->rangetable = pstate->p_rtable;

	q->utilityStmt = (Node *) stmt;
	return q;
}

/*
 * transformRuleStmt -
 *	  transform a Create Rule Statement. The actions is a list of parse
 *	  trees which is transformed into a list of query trees.
 */
static Query *
transformRuleStmt(ParseState *pstate, RuleStmt *stmt)
{
	Query	   *q;
	List	   *actions;

	q = makeNode(Query);
	q->commandType = CMD_UTILITY;

	actions = stmt->actions;

	/*
	 * transform each statment, like parse_analyze()
	 */
	while (actions != NIL)
	{

		/*
		 * NOTE: 'CURRENT' must always have a varno equal to 1 and 'NEW'
		 * equal to 2.
		 */
		addRangeTableEntry(pstate, stmt->object->relname, "*CURRENT*",
						   FALSE, FALSE);
		addRangeTableEntry(pstate, stmt->object->relname, "*NEW*",
						   FALSE, FALSE);

		pstate->p_last_resno = 1;
		pstate->p_is_rule = true;		/* for expand all */
		pstate->p_numAgg = 0;
		pstate->p_aggs = NULL;

		lfirst(actions) = transformStmt(pstate, lfirst(actions));
		actions = lnext(actions);
	}

	/* take care of the where clause */
	stmt->whereClause = transformWhereClause(pstate, stmt->whereClause);

	q->utilityStmt = (Node *) stmt;
	return q;
}


/*
 * transformSelectStmt -
 *	  transforms a Select Statement
 *
 */
static Query *
transformSelectStmt(ParseState *pstate, RetrieveStmt *stmt)
{
	Query	   *qry = makeNode(Query);

	qry->commandType = CMD_SELECT;

	/* set up a range table */
	makeRangeTable(pstate, NULL, stmt->fromClause);

	qry->uniqueFlag = stmt->unique;

	qry->into = stmt->into;
	qry->isPortal = FALSE;

	/* fix the target list */
	qry->targetList = transformTargetList(pstate, stmt->targetList);

	/* fix where clause */
	qry->qual = transformWhereClause(pstate, stmt->whereClause);

	/* check subselect clause */
	if (stmt->selectClause)
		elog(NOTICE, "UNION not yet supported; using first SELECT only", NULL);

	/* check subselect clause */
	if (stmt->havingClause)
		elog(NOTICE, "HAVING not yet supported; ignore clause", NULL);

	/* fix order clause */
	qry->sortClause = transformSortClause(pstate,
										  stmt->sortClause,
										  qry->targetList,
										  qry->uniqueFlag);

	/* fix group by clause */
	qry->groupClause = transformGroupClause(pstate,
											stmt->groupClause,
											qry->targetList);
	qry->rtable = pstate->p_rtable;

	if (pstate->p_numAgg > 0)
		finalizeAggregates(pstate, qry);

	return (Query *) qry;
}

/*
 * transformUpdateStmt -
 *	  transforms an update statement
 *
 */
static Query *
transformUpdateStmt(ParseState *pstate, ReplaceStmt *stmt)
{
	Query	   *qry = makeNode(Query);

	qry->commandType = CMD_UPDATE;
	pstate->p_is_update = true;

	/*
	 * the FROM clause is non-standard SQL syntax. We used to be able to
	 * do this with REPLACE in POSTQUEL so we keep the feature.
	 */
	makeRangeTable(pstate, stmt->relname, stmt->fromClause);

	/* fix the target list */
	qry->targetList = transformTargetList(pstate, stmt->targetList);

	/* fix where clause */
	qry->qual = transformWhereClause(pstate, stmt->whereClause);

	qry->rtable = pstate->p_rtable;
	qry->resultRelation = refnameRangeTablePosn(pstate->p_rtable, stmt->relname);

	/* make sure we don't have aggregates in the where clause */
	if (pstate->p_numAgg > 0)
		parseCheckAggregates(pstate, qry);

	return (Query *) qry;
}

/*
 * transformCursorStmt -
 *	  transform a Create Cursor Statement
 *
 */
static Query *
transformCursorStmt(ParseState *pstate, CursorStmt *stmt)
{
	Query	   *qry = makeNode(Query);

	/*
	 * in the old days, a cursor statement is a 'retrieve into portal'; If
	 * you change the following, make sure you also go through the code in
	 * various places that tests the kind of operation.
	 */
	qry->commandType = CMD_SELECT;

	/* set up a range table */
	makeRangeTable(pstate, NULL, stmt->fromClause);

	qry->uniqueFlag = stmt->unique;

	qry->into = stmt->portalname;
	qry->isPortal = TRUE;
	qry->isBinary = stmt->binary;		/* internal portal */

	/* fix the target list */
	qry->targetList = transformTargetList(pstate, stmt->targetList);

	/* fix where clause */
	qry->qual = transformWhereClause(pstate, stmt->whereClause);

	/* fix order clause */
	qry->sortClause = transformSortClause(pstate,
										  stmt->sortClause,
										  qry->targetList,
										  qry->uniqueFlag);
	/* fix group by clause */
	qry->groupClause = transformGroupClause(pstate,
											stmt->groupClause,
											qry->targetList);

	qry->rtable = pstate->p_rtable;

	if (pstate->p_numAgg > 0)
		finalizeAggregates(pstate, qry);

	return (Query *) qry;
}
