/*------------------------------------------------------------------------
*
* geqo_pmx.c
*
*	 partially matched crossover [PMX] routines;
*	 PMX operator according to Goldberg & Lingle
*	 (Proc Int'l Conf on GA's)
*
* $PostgreSQL: pgsql/src/backend/optimizer/geqo/geqo_pmx.c,v 1.10 2003/11/29 22:39:49 pgsql Exp $
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

/* the pmx algorithm is adopted from Genitor : */
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


/* pmx
 *
 *	 partially matched crossover
 */
void
pmx(Gene *tour1, Gene *tour2, Gene *offspring, int num_gene)
{
	int		   *failed = (int *) palloc((num_gene + 1) * sizeof(int));
	int		   *from = (int *) palloc((num_gene + 1) * sizeof(int));
	int		   *indx = (int *) palloc((num_gene + 1) * sizeof(int));
	int		   *check_list = (int *) palloc((num_gene + 1) * sizeof(int));

	int			left,
				right,
				temp,
				i,
				j,
				k;
	int			mx_fail,
				found,
				mx_hold;


/* no mutation so start up the pmx replacement algorithm */
/* initialize failed[], from[], check_list[] */
	for (k = 0; k < num_gene; k++)
	{
		failed[k] = -1;
		from[k] = -1;
		check_list[k + 1] = 0;
	}

/* locate crossover points */
	left = geqo_randint(num_gene - 1, 0);
	right = geqo_randint(num_gene - 1, 0);

	if (left > right)
	{
		temp = left;
		left = right;
		right = temp;
	}


/* copy tour2 into offspring */
	for (k = 0; k < num_gene; k++)
	{
		offspring[k] = tour2[k];
		from[k] = DAD;
		check_list[tour2[k]]++;
	}

/* copy tour1 into offspring */
	for (k = left; k <= right; k++)
	{
		check_list[offspring[k]]--;
		offspring[k] = tour1[k];
		from[k] = MOM;
		check_list[tour1[k]]++;
	}


/* pmx main part */

	mx_fail = 0;

/* STEP 1 */

	for (k = left; k <= right; k++)
	{							/* for all elements in the tour1-2 */

		if (tour1[k] == tour2[k])
			found = 1;			/* find match in tour2 */

		else
		{
			found = 0;			/* substitute elements */

			j = 0;
			while (!(found) && (j < num_gene))
			{
				if ((offspring[j] == tour1[k]) && (from[j] == DAD))
				{

					check_list[offspring[j]]--;
					offspring[j] = tour2[k];
					found = 1;
					check_list[tour2[k]]++;
				}

				j++;
			}

		}

		if (!(found))
		{						/* failed to replace gene */
			failed[mx_fail] = (int) tour1[k];
			indx[mx_fail] = k;
			mx_fail++;
		}

	}							/* ... for */


/* STEP 2 */

	/* see if any genes could not be replaced */
	if (mx_fail > 0)
	{
		mx_hold = mx_fail;

		for (k = 0; k < mx_hold; k++)
		{
			found = 0;

			j = 0;
			while (!(found) && (j < num_gene))
			{

				if ((failed[k] == (int) offspring[j]) && (from[j] == DAD))
				{
					check_list[offspring[j]]--;
					offspring[j] = tour2[indx[k]];
					check_list[tour2[indx[k]]]++;

					found = 1;
					failed[k] = -1;
					mx_fail--;
				}

				j++;
			}

		}						/* ... for	 */

	}							/* ... if	 */


/* STEP 3 */

	for (k = 1; k <= num_gene; k++)
	{

		if (check_list[k] > 1)
		{
			i = 0;

			while (i < num_gene)
			{
				if ((offspring[i] == (Gene) k) && (from[i] == DAD))
				{
					j = 1;

					while (j <= num_gene)
					{
						if (check_list[j] == 0)
						{
							offspring[i] = (Gene) j;
							check_list[k]--;
							check_list[j]++;
							i = num_gene + 1;
							j = i;
						}

						j++;
					}

				}				/* ... if	 */

				i++;
			}					/* end while */

		}
	}							/* ... for	 */

	pfree(failed);
	pfree(from);
	pfree(indx);
	pfree(check_list);
}
