/*------------------------------------------------------------------------
*
* minspantree.c--
*    routine to sort a join graph which is including cycles
*
* Copyright (c) 1994, Regents of the University of California
*
*
* IDENTIFICATION
*    $Header: /cvsroot/pgsql/src/backend/optimizer/geqo/Attic/minspantree.c,v 1.1 1997/02/19 12:57:50 scrappy Exp $
*
*-------------------------------------------------------------------------
*/

#include <values.h>

#include "postgres.h"

#include "nodes/pg_list.h"
#include "nodes/relation.h"
#include "nodes/primnodes.h"

#include "utils/palloc.h"

#include "optimizer/cost.h"

/*
 include "optimizer/geqo/tsp.h"
 */

#include "optimizer/geqo/geqo_gene.h"
#include "optimizer/geqo/geqo.h"

/*
 * minspantree--
 *       The function minspantree computes the minimum spanning tree
 *	for a given number of nodes and a given distance function.
 *	For each pair of nodes found to be connected, a given
 *	function is called. Nodes are denoted by the integer numbers
 *	1 .. number_of_joins, where number_of_joins is the number of nodes.
*/

void
minspantree(Query *root, List *join_rels, Rel *garel)
{
  int number_of_rels  = length(root->base_relation_list_);
  int number_of_joins = length(join_rels);
  int *connectto;
  /* connectto[i] = 0, if node i is already connected  */
  /* to the tree, otherwise connectto[i] is the node   */
  /* nearest to i, which is already connected.	     */

  Cost *disttoconnect; /* disttoconnect[i]: distance between i and connectto[i] */

  Cost dist, /* temporary */
    mindist; /* minimal distance between connected and unconnected node */

  Cost mstlength = 0.0; /* the total length of the minimum spanning tree */

  int count;
  int n,	/* newly attached node */
    nextn,	/* next node to be attached */
    tempn;

  int i, id1, id2;
  List *r = NIL;
  Rel  *joinrel = NULL;
  Rel **tmprel_array;


  /* allocate memory for matrix tmprel_array[x][y] */
  tmprel_array = (Rel **) palloc((number_of_rels+1)*sizeof(Rel *));
  for (i=0; i<=number_of_rels; i++)
	   (tmprel_array[i] = (Rel *) palloc ((number_of_rels+1)*sizeof(Rel)));

  /* read relations of join-relations into tmprel_array */

  foreach(r, join_rels) {
	joinrel = (Rel *)lfirst(r);
	id1 = (int)lfirst(joinrel->relids);
	id2 = (int)lsecond(joinrel->relids);

	if (id1 > id2) {
	   tmprel_array[id2][id1] = *(Rel *)joinrel;
	   }
	else {
	   tmprel_array[id1][id2] = *(Rel *)joinrel; /* ever reached? */
	   }
        }

  /* Trivial special cases handled first */
  /* garel is global in "tsp.h" */ 

  if (number_of_joins <= 2)
  {
    i=1;
    foreach(r, join_rels) {
      garel[i] = *(Rel *)lfirst(r);
      i++;
      }
  }


  else if (number_of_joins == 3)
  {
      Rel *rel12 = (Rel *) &tmprel_array[1][2];
      Rel *rel13 = (Rel *) &tmprel_array[1][3];
      Rel *rel23 = (Rel *) &tmprel_array[2][3];
      if (rel12->cheapestpath->path_cost > rel13->cheapestpath->path_cost)
      {
      	garel[1] = tmprel_array[1][3];
	if (rel12->cheapestpath->path_cost > rel23->cheapestpath->path_cost)
	{
      	  garel[2] = tmprel_array[2][3];
	}
	else
	{
      	  garel[2] = tmprel_array[1][2];
	}
      }
      else
      {
      	garel[1] = tmprel_array[1][2];
	if (rel13->cheapestpath->path_cost > rel23->cheapestpath->path_cost)
	{
      	  garel[2] = tmprel_array[2][3];
	}
	else
	{
      	  garel[2] = tmprel_array[1][3];
	}
      }
    }


  /* now the general case */
  else
  {
  connectto     = (int *)  palloc((number_of_rels+1)*sizeof(int));
  disttoconnect = (Cost *) palloc((number_of_rels+1)*sizeof(Cost));

  nextn = 2;
  for (tempn = 2; tempn <= number_of_rels; tempn++ )
  {
    connectto[tempn] = 1;
    disttoconnect[tempn] = (Cost) MAXFLOAT;
  }

  joinrel = NULL;
  n = 1;
  i = 1;
  for (count = 2; count <= number_of_rels; count++ )
  {
    connectto[n] = 0;
    mindist = (Cost) MAXFLOAT;
    for (tempn = 2; tempn <= number_of_rels; tempn++ )
    {
      if (connectto[tempn] != 0)
      {
        if (n > tempn) {
           joinrel = (Rel *) &tmprel_array[tempn][n];
	   }
	else {
           joinrel = (Rel *) &tmprel_array[n][tempn];
	   }
	dist = joinrel->cheapestpath->path_cost;

	if (dist < disttoconnect[tempn])
	{
	  disttoconnect[tempn] = dist;
	  connectto[tempn] = n;
	}
	if (disttoconnect[tempn] < mindist)
	{
	  mindist = disttoconnect[tempn];
	  nextn = tempn;
	}
      }
    }
    n = nextn;
    if (n > connectto[n]) {
       garel[i] = tmprel_array[connectto[n]][n];
       }
    else {
       garel[i] = tmprel_array[n][connectto[n]];
       }
    i++;
  }

  pfree(connectto);
  pfree(disttoconnect);

  }

  for (i=0; i<=number_of_rels; i++) pfree(tmprel_array[i]);
  pfree(tmprel_array);
}

