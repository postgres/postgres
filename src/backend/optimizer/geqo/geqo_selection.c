/*-------------------------------------------------------------------------
 *
 * geqo_selection.c
 *	  linear selection scheme for the genetic query optimizer
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/optimizer/geqo/geqo_selection.c
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

/* this is adopted from D. Whitley's Genitor algorithm */

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

#include <math.h>

#include "optimizer/geqo_copy.h"
#include "optimizer/geqo_random.h"
#include "optimizer/geqo_selection.h"

static int	linear_rand(PlannerInfo *root, int max, double bias);


/*
 * geqo_selection
 *	 according to bias described by input parameters,
 *	 first and second genes are selected from the pool
 */
void
geqo_selection(PlannerInfo *root, Chromosome *momma, Chromosome *daddy,
			   Pool *pool, double bias)
{
	int			first,
				second;

	first = linear_rand(root, pool->size, bias);
	second = linear_rand(root, pool->size, bias);

	/*
	 * Ensure we have selected different genes, except if pool size is only
	 * one, when we can't.
	 *
	 * This code was observed to hang up in an infinite loop when the
	 * platform's implementation of erand48() was broken.  We now always use
	 * our own version.
	 */
	if (pool->size > 1)
	{
		while (first == second)
			second = linear_rand(root, pool->size, bias);
	}

	geqo_copy(root, momma, &pool->data[first], pool->string_length);
	geqo_copy(root, daddy, &pool->data[second], pool->string_length);
}

/*
 * linear_rand
 *	  generates random integer between 0 and input max number
 *	  using input linear bias
 *
 *	  bias is y-intercept of linear distribution
 *
 *	  probability distribution function is: f(x) = bias - 2(bias - 1)x
 *			 bias = (prob of first rule) / (prob of middle rule)
 */
static int
linear_rand(PlannerInfo *root, int pool_size, double bias)
{
	double		index;			/* index between 0 and pop_size */
	double		max = (double) pool_size;

	/*
	 * If geqo_rand() returns exactly 1.0 then we will get exactly max from
	 * this equation, whereas we need 0 <= index < max.  Also it seems
	 * possible that roundoff error might deliver values slightly outside the
	 * range; in particular avoid passing a value slightly less than 0 to
	 * sqrt(). If we get a bad value just try again.
	 */
	do
	{
		double		sqrtval;

		sqrtval = (bias * bias) - 4.0 * (bias - 1.0) * geqo_rand(root);
		if (sqrtval > 0.0)
			sqrtval = sqrt(sqrtval);
		index = max * (bias - sqrtval) / 2.0 / (bias - 1.0);
	} while (index < 0.0 || index >= max);

	return (int) index;
}
