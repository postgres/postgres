/*------------------------------------------------------------------------
*
* geqo_px.c
*
*	 position crossover [PX] routines;
*	 PX operator according to Syswerda
*	 (The Genetic Algorithms Handbook, L Davis, ed)
*
* $PostgreSQL: pgsql/src/backend/optimizer/geqo/geqo_px.c,v 1.10 2003/11/29 22:39:49 pgsql Exp $
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

/* the px algorithm is adopted from Genitor : */
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


/* px
 *
 *	 position crossover
 */
void
px(Gene *tour1, Gene *tour2, Gene *offspring, int num_gene, City *city_table)
{

	int			num_positions;
	int			i,
				pos,
				tour2_index,
				offspring_index;

	/* initialize city table */
	for (i = 1; i <= num_gene; i++)
		city_table[i].used = 0;

	/* choose random positions that will be inherited directly from parent */
	num_positions = geqo_randint(2 * num_gene / 3, num_gene / 3);

	/* choose random position */
	for (i = 0; i < num_positions; i++)
	{
		pos = geqo_randint(num_gene - 1, 0);

		offspring[pos] = tour1[pos];	/* transfer cities to child */
		city_table[(int) tour1[pos]].used = 1;	/* mark city used */
	}

	tour2_index = 0;
	offspring_index = 0;


	/* px main part */

	while (offspring_index < num_gene)
	{

		/* next position in offspring filled */
		if (!city_table[(int) tour1[offspring_index]].used)
		{

			/* next city in tour1 not used */
			if (!city_table[(int) tour2[tour2_index]].used)
			{

				/* inherit from tour1 */
				offspring[offspring_index] = tour2[tour2_index];

				tour2_index++;
				offspring_index++;
			}
			else
			{					/* next city in tour2 has been used */
				tour2_index++;
			}

		}
		else
		{						/* next position in offspring is filled */
			offspring_index++;
		}

	}

}
