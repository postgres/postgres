/*-------------------------------------------------------------------------
 *
 * execFlatten.c--
 *    This file handles the nodes associated with flattening sets in the
 *    target list of queries containing functions returning sets.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/executor/Attic/execFlatten.c,v 1.1.1.1 1996/07/09 06:21:24 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

/*
 * ExecEvalIter() -
 *   Iterate through the all return tuples/base types from a function one
 *   at time (i.e. one per ExecEvalIter call).  Not really needed for
 *   postquel functions, but for reasons of orthogonality, these nodes
 *   exist above pq functions as well as c functions.
 *
 * ExecEvalFjoin() -
 *   Given N Iter nodes return a vector of all combinations of results
 *   one at a time (i.e. one result vector per ExecEvalFjoin call).  This
 *   node does the actual flattening work.
 */
#include "postgres.h"
#include "nodes/primnodes.h"
#include "nodes/relation.h"
#include "nodes/execnodes.h"
#include "executor/executor.h"
#include "executor/execFlatten.h"

Datum
ExecEvalIter(Iter *iterNode,
	     ExprContext *econtext,
	     bool *resultIsNull,
	     bool *iterIsDone)
{
    Node *expression;
    
    expression = iterNode->iterexpr;
    
    /*
     * Really Iter nodes are only needed for C functions, postquel function
     * by their nature return 1 result at a time.  For now we are only worrying
     * about postquel functions, c functions will come later.
     */
    return ExecEvalExpr(expression, econtext, resultIsNull, iterIsDone);
}

void
ExecEvalFjoin(TargetEntry *tlist,
	      ExprContext *econtext,
	      bool *isNullVect,
	      bool *fj_isDone)
{

#ifdef SETS_FIXED
    bool     isDone;
    int      curNode;
    List     *tlistP;

    Fjoin    *fjNode     = tlist->fjoin;
    DatumPtr resVect    = fjNode->fj_results;
    BoolPtr  alwaysDone = fjNode->fj_alwaysDone;
    
    if (fj_isDone) *fj_isDone = false;
    /*
     * For the next tuple produced by the plan, we need to re-initialize
     * the Fjoin node.
     */
    if (!fjNode->fj_initialized)
	{
	    /*
	     * Initialize all of the Outer nodes
	     */
	    curNode = 1;
	    foreach(tlistP, lnext(tlist))
		{
		    TargetEntry *tle = lfirst(tlistP);
		    
		    resVect[curNode] = ExecEvalIter((Iter*)tle->expr,
						    econtext,
						    &isNullVect[curNode],
						    &isDone);
		    if (isDone)
			isNullVect[curNode] = alwaysDone[curNode] = true;
		    else
			alwaysDone[curNode] = false;
		    
		    curNode++;
		}
	    
	    /*
	     * Initialize the inner node
	     */
	    resVect[0] = ExecEvalIter((Iter*)fjNode->fj_innerNode->expr,
				      econtext,
				      &isNullVect[0],
				      &isDone);
	    if (isDone)
		isNullVect[0] = alwaysDone[0] = true;
	    else
		alwaysDone[0] = false;
	    
	    /*
	     * Mark the Fjoin as initialized now.
	     */
	    fjNode->fj_initialized = TRUE;
	    
	    /*
	     * If the inner node is always done, then we are done for now
	     */
	    if (isDone)
		return;
	}
    else
	{
	    /*
	     * If we're already initialized, all we need to do is get the
	     * next inner result and pair it up with the existing outer node
	     * result vector.  Watch out for the degenerate case, where the
	     * inner node never returns results.
	     */
	    
	    /*
	     * Fill in nulls for every function that is always done.
	     */
	    for (curNode=fjNode->fj_nNodes-1; curNode >= 0; curNode--)
		isNullVect[curNode] = alwaysDone[curNode];
	    
	    if (alwaysDone[0] == true)
		{
		    *fj_isDone = FjoinBumpOuterNodes(tlist,
						     econtext,
						     resVect,
						     isNullVect);
		    return;
		}
	    else
		resVect[0] = ExecEvalIter((Iter*)fjNode->fj_innerNode->expr,
					  econtext,
					  &isNullVect[0],
					  &isDone);
	}
    
    /*
     * if the inner node is done
     */
    if (isDone)
	{
	    *fj_isDone = FjoinBumpOuterNodes(tlist,
					     econtext,
					     resVect,
					     isNullVect);
	    if (*fj_isDone)
		return;
	    
	    resVect[0] = ExecEvalIter((Iter*)fjNode->fj_innerNode->expr,
				      econtext,
				      &isNullVect[0],
				      &isDone);
	    
	}
#endif
    return;
}

bool
FjoinBumpOuterNodes(TargetEntry *tlist,
		    ExprContext *econtext,
		    DatumPtr results,
		    char *nulls)
{
#ifdef SETS_FIXED
    bool   funcIsDone = true;
    Fjoin  *fjNode    = tlist->fjoin;
    char *alwaysDone = fjNode->fj_alwaysDone;
    List   *outerList  = lnext(tlist);
    List   *trailers   = lnext(tlist);
    int    trailNode  = 1;
    int    curNode    = 1;
    
    /*
     * Run through list of functions until we get to one that isn't yet
     * done returning values.  Watch out for funcs that are always done.
     */
    while ((funcIsDone == true) && (outerList != NIL))
	{
	    TargetEntry *tle = lfirst(outerList);
	    
	    if (alwaysDone[curNode] == true)
		nulls[curNode] = 'n';
	    else
		results[curNode] = ExecEvalIter((Iter)tle->expr,
						econtext,
						&nulls[curNode],
						&funcIsDone);
	    curNode++;
	    outerList = lnext(outerList);
	}
    
    /*
     * If every function is done, then we are done flattening.
     * Mark the Fjoin node unitialized, it is time to get the
     * next tuple from the plan and redo all of the flattening.
     */
    if (funcIsDone)
	{
	    set_fj_initialized(fjNode, false);
	    return (true);
	}
    
    /*
     * We found a function that wasn't done.  Now re-run every function
     * before it.  As usual watch out for functions that are always done.
     */
    trailNode = 1;
    while (trailNode != curNode-1)
	{
	    TargetEntry *tle = lfirst(trailers);
	    
	    if (alwaysDone[trailNode] != true)
		results[trailNode] = ExecEvalIter((Iter)tle->expr,
						  econtext,
						  &nulls[trailNode],
						  &funcIsDone);
	    trailNode++;
	    trailers = lnext(trailers);
	}
    return false;
#endif
    return false;
}
