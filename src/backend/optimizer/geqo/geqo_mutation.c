/*------------------------------------------------------------------------
*
* geqo_mutation.c--
*
*	 TSP mutation routines
*
* $Id: geqo_mutation.c,v 1.3 1997/09/08 02:23:57 momjian Exp $
*
*-------------------------------------------------------------------------
*/

/* contributed by:
   =*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=
   *  Martin Utesch				 * Institute of Automatic Control	   *
   =							 = University of Mining and Technology =
   *  utesch@aut.tu-freiberg.de  * Freiberg, Germany				   *
   =*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=
 */

/* this is adopted from Genitor : */
/*************************************************************/
/*															 */
/*	Copyright (c) 1990										 */
/*	Darrell L. Whitley										 */
/*	Computer Science Department								 */
/*	Colorado State University								 */
/*															 */
/*	Permission is hereby granted to copy all or any part of  */
/*	this program for free distribution.   The author's name  */
/*	and this copyright notice must be included in any copy.  */
/*															 */
/*************************************************************/

#include "postgres.h"

#include "nodes/pg_list.h"
#include "nodes/relation.h"
#include "nodes/primnodes.h"

#include "utils/palloc.h"
#include "utils/elog.h"

#include "optimizer/internal.h"
#include "optimizer/paths.h"
#include "optimizer/pathnode.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"

#include "optimizer/geqo_gene.h"
#include "optimizer/geqo_random.h"
#include "optimizer/geqo_mutation.h"

void
geqo_mutation(Gene * tour, int num_gene)
{
	int			swap1;
	int			swap2;
	int			num_swaps = geqo_randint(num_gene / 3, 0);
	Gene		temp;


	while (num_swaps > 0)
	{
		swap1 = geqo_randint(num_gene - 1, 0);
		swap2 = geqo_randint(num_gene - 1, 0);

		while (swap1 == swap2)
			swap2 = geqo_randint(num_gene - 1, 0);

		temp = tour[swap1];
		tour[swap1] = tour[swap2];
		tour[swap2] = temp;


		num_swaps -= 1;
	}
}
