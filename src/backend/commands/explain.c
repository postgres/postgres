/*-------------------------------------------------------------------------
 *
 * explain.c--
 *    Explain the query execution plan
 *
 * Copyright (c) 1994-5, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/commands/explain.c,v 1.3 1996/11/03 23:57:32 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <string.h>

#include "postgres.h"
#include "parser/catalog_utils.h"
#include "parser/parse_query.h"	    /* for MakeTimeRange() */
#include "nodes/plannodes.h"
#include "tcop/tcopprot.h"
#include "utils/palloc.h"
#include "lib/stringinfo.h"
#include "commands/explain.h"
#include "optimizer/planner.h"
#include "access/xact.h"

typedef struct ExplainState {
    /* options */
    int		printCost;	/* print cost */
    int		printNodes;	/* do nodeToString() instead */
    /* other states */
    List 	*rtable;	/* range table */
} ExplainState;

static char *Explain_PlanToString(Plan *plan, ExplainState *es);

/*
 * ExplainQuery -
 *    print out the execution plan for a given query
 *
 */
void
ExplainQuery(Query *query, List *options, CommandDest dest)
{
    char *s;
    Plan *plan;
    ExplainState *es;
    int len;

    if (IsAbortedTransactionBlockState()) {
	char *tag = "*ABORT STATE*";
	EndCommand(tag, dest);
		
	elog(NOTICE, "(transaction aborted): %s",
	     "queries ignored until END");
		
	return;
    }

    /* plan the queries (XXX we've ignored rewrite!!) */
    plan = planner(query);

    /* pg_plan could have failed */
    if (plan == NULL)
	return;

    es = (ExplainState*)malloc(sizeof(ExplainState));
    memset(es, 0, sizeof(ExplainState));

    /* parse options */
    while (options) {
	char *ostr = strVal(lfirst(options));
	if (!strcasecmp(ostr, "cost"))
	    es->printCost = 1;
	else if (!strcasecmp(ostr, "full_plan"))
	    es->printNodes = 1;

	options = lnext(options);
    }
    es->rtable = query->rtable;

    if (es->printNodes) {
	s = nodeToString(plan);
    } else {
	s = Explain_PlanToString(plan, es);
    }

    /* output the plan */
    len = strlen(s);
    elog(NOTICE, "QUERY PLAN:\n\n%.*s", ELOG_MAXLEN-64, s);
    len -= ELOG_MAXLEN-64;
    while (len > 0) {
	s += ELOG_MAXLEN-64;
	elog(NOTICE, "%.*s", ELOG_MAXLEN-64, s);
	len -= ELOG_MAXLEN-64;
    }
    free(es);
}

/*****************************************************************************
 *
 *****************************************************************************/

/*
 * explain_outNode -
 *    converts a Node into ascii string and append it to 'str'
 */
static void
explain_outNode(StringInfo str, Plan *plan, int indent, ExplainState *es)
{
    char *pname;
    char buf[1000];
    int i;
    
    if (plan==NULL) {
	appendStringInfo(str, "\n");
	return;
    }

    switch(nodeTag(plan)) {
    case T_Result:
	pname = "Result";
	break;
    case T_Append:
	pname = "Append";
	break;
    case T_NestLoop:
	pname = "Nested Loop";
	break;
    case T_MergeJoin:
	pname = "Merge Join";
	break;
    case T_HashJoin:
	pname = "Hash Join";
	break;
    case T_SeqScan:
	pname = "Seq Scan";
	break;
    case T_IndexScan:
	pname = "Index Scan";
	break;
    case T_Temp:
	pname = "Temp Scan";
	break;
    case T_Sort:
	pname = "Sort";
	break;
    case T_Agg:
	pname = "Aggregate";
	break;
    case T_Unique:
	pname = "Unique";
	break;
    case T_Hash:
	pname = "Hash";
	break;
    case T_Tee:
	pname = "Tee";
	break;
    default:
	pname = NULL;
	break;
    }

    for(i=0; i < indent; i++)
	appendStringInfo(str, "  ");

    appendStringInfo(str, pname);
    switch(nodeTag(plan)) {
    case T_SeqScan:
    case T_IndexScan:
	if (((Scan*)plan)->scanrelid > 0) {
	    RangeTblEntry *rte = nth(((Scan*)plan)->scanrelid-1, es->rtable);
	    sprintf(buf, " on %.*s", NAMEDATALEN, rte->refname);
	    appendStringInfo(str, buf);
	}
	break;
    default:
	break;
    }
    if (es->printCost) {
	sprintf(buf, "  (cost=%.2f size=%d width=%d)",
		plan->cost, plan->plan_size, plan->plan_width);
	appendStringInfo(str, buf);
    }
    appendStringInfo(str, "\n");

    /* lefttree */
    if (outerPlan(plan)) {
	for(i=0; i < indent; i++)
	    appendStringInfo(str, "  ");
	appendStringInfo(str, "  -> ");
	explain_outNode(str, outerPlan(plan), indent+1, es);
    }

    /* righttree */
    if (innerPlan(plan)) {
	for(i=0; i < indent; i++)
	    appendStringInfo(str, "  ");
	appendStringInfo(str, "  -> ");
	explain_outNode(str, innerPlan(plan), indent+1, es);
    }
    return;
}

static char *
Explain_PlanToString(Plan *plan, ExplainState *es)
{
    StringInfo str;
    char *s;
    
    if (plan==NULL)
	return "";
    Assert(plan!=NULL);
    str = makeStringInfo();
    explain_outNode(str, plan, 0, es);
    s = str->data;
    pfree(str);

    return s;
}
