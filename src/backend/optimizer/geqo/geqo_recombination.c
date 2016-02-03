/*------------------------------------------------------------------------
*
* geqo_recombination.c
*	 misc recombination procedures
*
* src/backend/optimizer/geqo/geqo_recombination.c
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

/* -- parts of this are adapted from D. Whitley's Genitor algorithm -- */

#include "postgres.h"

#include "optimizer/geqo_random.h"
#include "optimizer/geqo_recombination.h"


/*
 * init_tour
 *
 *	 Randomly generates a legal "traveling salesman" tour
 *	 (i.e. where each point is visited only once.)
 */
void
init_tour(PlannerInfo *root, Gene *tour, int num_gene)
{
	int			i,
				j;

	/*
	 * We must fill the tour[] array with a random permutation of the numbers
	 * 1 .. num_gene.  We can do that in one pass using the "inside-out"
	 * variant of the Fisher-Yates shuffle algorithm.  Notionally, we append
	 * each new value to the array and then swap it with a randomly-chosen
	 * array element (possibly including itself, else we fail to generate
	 * permutations with the last city last).  The swap step can be optimized
	 * by combining it with the insertion.
	 */
	if (num_gene > 0)
		tour[0] = (Gene) 1;

	for (i = 1; i < num_gene; i++)
	{
		j = geqo_randint(root, i, 0);
		/* i != j check avoids fetching uninitialized array element */
		if (i != j)
			tour[i] = tour[j];
		tour[j] = (Gene) (i + 1);
	}
}

/* alloc_city_table
 *
 *	 allocate memory for city table
 */
City *
alloc_city_table(PlannerInfo *root, int num_gene)
{
	City	   *city_table;

	/*
	 * palloc one extra location so that nodes numbered 1..n can be indexed
	 * directly; 0 will not be used
	 */
	city_table = (City *) palloc((num_gene + 1) * sizeof(City));

	return city_table;
}

/* free_city_table
 *
 *	  deallocate memory of city table
 */
void
free_city_table(PlannerInfo *root, City *city_table)
{
	pfree(city_table);
}
