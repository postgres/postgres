/*------------------------------------------------------------------------
*
* geqo_ox2.c
*
*	 order crossover [OX] routines;
*	 OX2 operator according to Syswerda
*	 (The Genetic Algorithms Handbook, ed L Davis)
*
* $Id: geqo_ox2.c,v 1.8 1999/07/16 04:59:10 momjian Exp $
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
#include "optimizer/geqo_random.h"
#include "optimizer/geqo_recombination.h"


/* ox2
 *
 *	 position crossover
 */
void
ox2(Gene *tour1, Gene *tour2, Gene *offspring, int num_gene, City *city_table)
{
	int			k,
				j,
				count,
				pos,
				select,
				num_positions;

	/* initialize city table */
	for (k = 1; k <= num_gene; k++)
	{
		city_table[k].used = 0;
		city_table[k - 1].select_list = -1;
	}

	/* determine the number of positions to be inherited from tour1  */
	num_positions = geqo_randint(2 * num_gene / 3, num_gene / 3);

	/* make a list of selected cities */
	for (k = 0; k < num_positions; k++)
	{
		pos = geqo_randint(num_gene - 1, 0);
		city_table[pos].select_list = (int) tour1[pos];
		city_table[(int) tour1[pos]].used = 1;	/* mark used */
	}


	count = 0;
	k = 0;

	/* consolidate the select list to adjacent positions */
	while (count < num_positions)
	{
		if (city_table[k].select_list == -1)
		{
			j = k + 1;
			while ((city_table[j].select_list == -1) && (j < num_gene))
				j++;

			city_table[k].select_list = city_table[j].select_list;
			city_table[j].select_list = -1;
			count++;
		}
		else
			count++;
		k++;
	}

	select = 0;

	for (k = 0; k < num_gene; k++)
	{
		if (city_table[(int) tour2[k]].used)
		{
			offspring[k] = (Gene) city_table[select].select_list;
			select++;			/* next city in  the select list   */
		}
		else
/* city isn't used yet, so inherit from tour2 */
			offspring[k] = tour2[k];
	}

}
