/*------------------------------------------------------------------------
*
* geqo_params.c
*	 routines for determining necessary genetic optimization parameters
*
* Copyright (c) 1994, Regents of the University of California
*
* $Id: geqo_params.c,v 1.13 1999/02/13 23:16:10 momjian Exp $
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

#include <stdio.h>
#include <time.h>
#include <math.h>
#include <ctype.h>
#include <string.h>

#include "postgres.h"
#include "miscadmin.h"

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
#include "optimizer/geqo.h"

#include "storage/fd.h"

#define POOL_TAG		"Pool_Size"
#define TRIAL_TAG		"Generations"
#define RAND_TAG		"Random_Seed"
#define BIAS_TAG		"Selection_Bias"

#define EFFORT_TAG		"Effort"/* optimization effort and */
#define LOW				 "low"	/* corresponding tags */
#define MEDIUM		  "medium"
#define HIGH		  "high"

#define MAX_TOKEN 80			/* Maximum size of one token in the  *
								 * configuration file				 */

static int	gimme_pool_size(int string_length);
static int	gimme_number_generations(int pool_size, int effort);
static int	next_token(FILE *, char *, int);

/*
 * geqo_param
 *		 get ga parameters out of "$PGDATA/pg_geqo" file.
 */
void
geqo_params(int string_length)
{
	int			i;

	char		buf[MAX_TOKEN];
	FILE	   *file;

	char	   *conf_file;

/* these static variables are used to signal that a value has been set */
	int			pool_size = 0;
	int			number_trials = 0;
	int			random_seed = 0;
	int			selection_bias = 0;
	int			effort = 0;


	/* put together the full pathname to the config file */
	conf_file = (char *) palloc((strlen(DataDir) + strlen(GEQO_FILE) + 2) * sizeof(char));

	sprintf(conf_file, "%s/%s", DataDir, GEQO_FILE);

	/* open the config file */
#ifndef __CYGWIN32__
	file = AllocateFile(conf_file, "r");
#else
	file = AllocateFile(conf_file, "rb");
#endif
	if (file)
	{

		/*
		 * empty and comment line stuff
		 */
		while ((i = next_token(file, buf, sizeof(buf))) != EOF)
		{
			/* If only token on the line, ignore */
			if (i == '\n')
				continue;

			/* Comment -- read until end of line then next line */
			if (buf[0] == '#')
			{
				while (next_token(file, buf, sizeof(buf)) == 0);
				continue;
			}

			/*
			 * get ga parameters by parsing
			 */

			/*------------------------------------------------- pool size */
			if (strcmp(buf, POOL_TAG) == 0)
			{
				i = next_token(file, buf, sizeof(buf)); /* get next token */

				if (i != EOF)	/* only ignore if we got no text at all */
				{
					if (sscanf(buf, "%d", &PoolSize) == 1)
						pool_size = 1;
				}

			}

			/*------------------------------------------------- number of trials */
			else if (strcmp(buf, TRIAL_TAG) == 0)
			{
				i = next_token(file, buf, sizeof(buf));

				if (i != EOF)
				{
					if (sscanf(buf, "%d", &Generations) == 1)
						number_trials = 1;
				}

			}

			/*------------------------------------------------- optimization effort */
			else if (strcmp(buf, EFFORT_TAG) == 0)
			{
				i = next_token(file, buf, sizeof(buf));

				if (i != EOF)
				{
					if (strcmp(buf, LOW) == 0)
						effort = LOW_EFFORT;
					else if (strcmp(buf, MEDIUM) == 0)
						effort = MEDIUM_EFFORT;
					else if (strcmp(buf, HIGH) == 0)
						effort = HIGH_EFFORT;
				}

			}

			/*------------------------------------------- random seed */
			else if (strcmp(buf, RAND_TAG) == 0)
			{
				i = next_token(file, buf, sizeof(buf));

				if (i != EOF)
				{
					if (sscanf(buf, "%ld", &RandomSeed) == 1)
						random_seed = 1;
				}

			}

			/*------------------------------------------- selection bias */
			else if (strcmp(buf, BIAS_TAG) == 0)
			{
				i = next_token(file, buf, sizeof(buf));

				if (i != EOF)
				{
					if (sscanf(buf, "%lf", &SelectionBias) == 1)
						selection_bias = 1;
				}

			}

			/* unrecognized tags */
			else
			{
				if (i != EOF)
				{
				}

				elog(DEBUG, "geqo_params: unknown parameter type \"%s\"\nin file \'%s\'", buf, conf_file);

				/* if not at end-of-line, keep reading til we are */
				while (i == 0)
					i = next_token(file, buf, sizeof(buf));
			}
		}

		FreeFile(file);

		pfree(conf_file);
	}

	else
		elog(DEBUG, "geqo_params: ga parameter file\n\'%s\'\ndoes not exist or permissions are not setup correctly", conf_file);

	/*
	 * parameter checkings follow
	 */

	/**************** PoolSize: essential ****************/
	if (!(pool_size))
	{
		PoolSize = gimme_pool_size(string_length);

		elog(DEBUG, "geqo_params: no pool size specified;\nusing computed value of %d", PoolSize);
	}


	/**************** Effort: essential ****************/
	if (!(effort))
	{
		if (PoolSize == MAX_POOL)
			effort = HIGH_EFFORT;
		else
			effort = MEDIUM_EFFORT;

		elog(DEBUG, "geqo_params: no optimization effort specified;\nusing value of %d", effort);

	}

	/**************** Generations: essential ****************/
	if (!(number_trials))
	{
		Generations = gimme_number_generations(PoolSize, effort);

		elog(DEBUG, "geqo_params: no number of trials specified;\nusing computed value of %d", Generations);

	}

	/* RandomSeed: */
	if (!(random_seed))
	{
		RandomSeed = (long) time(NULL);

		elog(DEBUG, "geqo_params: no random seed specified;\nusing computed value of %ld", RandomSeed);
	}

	/* SelectionBias: */
	if (!(selection_bias))
	{
		SelectionBias = SELECTION_BIAS;

		elog(DEBUG, "geqo_params: no selection bias specified;\nusing default value of %f", SelectionBias);
	}

}


/*
 * Grab one token out of fp.  Defined as the next string of non-whitespace
 * in the file.  After we get the token, continue reading until EOF, end of
 * line or the next token.	If it's the last token on the line, return '\n'
 * for the value.  If we get EOF before reading a token, return EOF.  In all
 * other cases return 0.
 */
static int
next_token(FILE *fp, char *buf, int bufsz)
{
	int			c;
	char	   *eb = buf + (bufsz - 1);

	/* Discard inital whitespace */
	while (isspace(c = getc(fp)));

	/* EOF seen before any token so return EOF */
	if (c == EOF)
		return -1;

	/* Form a token in buf */
	do
	{
		if (buf < eb)
			*buf++ = c;
		c = getc(fp);
	} while (!isspace(c) && c != EOF);
	*buf = '\0';

	/* Discard trailing tabs and spaces */
	while (c == ' ' || c == '\t')
		c = getc(fp);

	/* Put back the char that was non-whitespace (putting back EOF is ok) */
	ungetc(c, fp);

	/* If we ended with a newline, return that, otherwise return 0 */
	return c == '\n' ? '\n' : 0;
}

/* gimme_pool_size
 *	 compute good estimation for pool size
 *	 according to number of involved rels in a query
 */
static int
gimme_pool_size(int string_length)
{
	double		exponent;
	double		size;

	exponent = (double) string_length + 1.0;

	size = pow(2.0, exponent);

	if (size < MIN_POOL)
		return MIN_POOL;
	else if (size > MAX_POOL)
		return MAX_POOL;
	else
		return (int) ceil(size);
}

/* gimme_number_generations
 *	 compute good estimation for number of generations size
 *	 for convergence
 */
static int
gimme_number_generations(int pool_size, int effort)
{
	int			number_gens;

	number_gens = (int) ceil(geqo_log((double) pool_size, 2.0));

	return effort * number_gens;
}
