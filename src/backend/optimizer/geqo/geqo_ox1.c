/*------------------------------------------------------------------------
*
* geqo_ox1.c
*
*	 order crossover [OX] routines;
*	 OX1 operator according to Davis
*	 (Proc Int'l Joint Conf on AI)
*
* src/backend/optimizer/geqo/geqo_ox1.c
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

/* the ox algorithm is adopted from Genitor : */
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
#include "optimizer/geqo.h"

#if defined(OX1)

#include "optimizer/geqo_random.h"
#include "optimizer/geqo_recombination.h"

/* ox1
 *
 *	 position crossover
 */
void
ox1(PlannerInfo *root, Gene *tour1, Gene *tour2, Gene *offspring, int num_gene,
	City * city_table)
{
	int			left,
				right,
				k,
				p,
				temp;

	/* initialize city table */
	for (k = 1; k <= num_gene; k++)
		city_table[k].used = 0;

	/* select portion to copy from tour1 */
	left = geqo_randint(root, num_gene - 1, 0);
	right = geqo_randint(root, num_gene - 1, 0);

	if (left > right)
	{
		temp = left;
		left = right;
		right = temp;
	}

	/* copy portion from tour1 to offspring */
	for (k = left; k <= right; k++)
	{
		offspring[k] = tour1[k];
		city_table[(int) tour1[k]].used = 1;
	}

	k = (right + 1) % num_gene; /* index into offspring */
	p = k;						/* index into tour2 */

	/* copy stuff from tour2 to offspring */
	while (k != left)
	{
		if (!city_table[(int) tour2[p]].used)
		{
			offspring[k] = tour2[p];
			k = (k + 1) % num_gene;
			city_table[(int) tour2[p]].used = 1;
		}
		p = (p + 1) % num_gene; /* increment tour2-index */
	}
}

#endif							/* defined(OX1) */
