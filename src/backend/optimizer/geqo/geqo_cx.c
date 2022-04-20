/*------------------------------------------------------------------------
*
* geqo_cx.c
*
*	 cycle crossover [CX] routines;
*	 CX operator according to Oliver et al
*	 (Proc 2nd Int'l Conf on GA's)
*
* src/backend/optimizer/geqo/geqo_cx.c
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

/* the cx algorithm is adopted from Genitor : */
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
#include "optimizer/geqo_random.h"
#include "optimizer/geqo_recombination.h"

#if defined(CX)

/* cx
 *
 *	 cycle crossover
 */
int
cx(PlannerInfo *root, Gene *tour1, Gene *tour2, Gene *offspring,
   int num_gene, City * city_table)
{
	int			i,
				start_pos,
				curr_pos;
	int			count = 0;
	int			num_diffs = 0;

	/* initialize city table */
	for (i = 1; i <= num_gene; i++)
	{
		city_table[i].used = 0;
		city_table[tour2[i - 1]].tour2_position = i - 1;
		city_table[tour1[i - 1]].tour1_position = i - 1;
	}

	/* choose random cycle starting position */
	start_pos = geqo_randint(root, num_gene - 1, 0);

	/* child inherits first city  */
	offspring[start_pos] = tour1[start_pos];

	/* begin cycle with tour1 */
	curr_pos = start_pos;
	city_table[(int) tour1[start_pos]].used = 1;

	count++;

	/* cx main part */


/* STEP 1 */

	while (tour2[curr_pos] != tour1[start_pos])
	{
		city_table[(int) tour2[curr_pos]].used = 1;
		curr_pos = city_table[(int) tour2[curr_pos]].tour1_position;
		offspring[curr_pos] = tour1[curr_pos];
		count++;
	}


/* STEP 2 */

	/* failed to create a complete tour */
	if (count < num_gene)
	{
		for (i = 1; i <= num_gene; i++)
		{
			if (!city_table[i].used)
			{
				offspring[city_table[i].tour2_position] =
					tour2[(int) city_table[i].tour2_position];
				count++;
			}
		}
	}


/* STEP 3 */

	/* still failed to create a complete tour */
	if (count < num_gene)
	{

		/* count the number of differences between mom and offspring */
		for (i = 0; i < num_gene; i++)
			if (tour1[i] != offspring[i])
				num_diffs++;
	}

	return num_diffs;
}

#endif							/* defined(CX) */
