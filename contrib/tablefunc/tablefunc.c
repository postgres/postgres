/*
 * tablefunc
 *
 * Sample to demonstrate C functions which return setof scalar
 * and setof composite.
 * Joe Conway <mail@joeconway.com>
 *
 * Copyright 2002 by PostgreSQL Global Development Group
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose, without fee, and without a written agreement
 * is hereby granted, provided that the above copyright notice and this
 * paragraph and the following two paragraphs appear in all copies.
 * 
 * IN NO EVENT SHALL THE AUTHORS OR DISTRIBUTORS BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 * LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
 * DOCUMENTATION, EVEN IF THE AUTHOR OR DISTRIBUTORS HAVE BEEN ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 * 
 * THE AUTHORS AND DISTRIBUTORS SPECIFICALLY DISCLAIM ANY WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE AUTHOR AND DISTRIBUTORS HAS NO OBLIGATIONS TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 */
#include <stdlib.h>
#include <math.h>

#include "postgres.h"

#include "fmgr.h"
#include "funcapi.h"
#include "executor/spi.h" 
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"

#include "tablefunc.h"

static bool compatTupleDescs(TupleDesc tupdesc1, TupleDesc tupdesc2);
static void get_normal_pair(float8 *x1, float8 *x2);
static TupleDesc make_crosstab_tupledesc(TupleDesc spi_tupdesc, int num_catagories);

typedef struct
{
	float8	mean;		/* mean of the distribution */
	float8	stddev;		/* stddev of the distribution */
	float8	carry_val;	/* hold second generated value */
	bool	use_carry;	/* use second generated value */
}	normal_rand_fctx;

typedef struct
{
	SPITupleTable  *spi_tuptable;	/* sql results from user query */
	char		   *lastrowid;		/* rowid of the last tuple sent */
}	crosstab_fctx;

#define GET_TEXT(cstrp) DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum(cstrp)))
#define GET_STR(textp) DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(textp)))
#define xpfree(var_) \
	do { \
		if (var_ != NULL) \
		{ \
			pfree(var_); \
			var_ = NULL; \
		} \
	} while (0)

/*
 * normal_rand - return requested number of random values
 * with a Gaussian (Normal) distribution.
 *
 * inputs are int numvals, float8 lower_bound, and float8 upper_bound
 * returns float8
 */
PG_FUNCTION_INFO_V1(normal_rand);
Datum
normal_rand(PG_FUNCTION_ARGS)
{
	FuncCallContext	   *funcctx;
	int					call_cntr;
	int					max_calls;
	normal_rand_fctx   *fctx;
	float8				mean;
	float8				stddev;
	float8				carry_val;
	bool				use_carry;
	MemoryContext		oldcontext;

	/* stuff done only on the first call of the function */
 	if(SRF_IS_FIRSTCALL())
 	{
		/* create a function context for cross-call persistence */
 		funcctx = SRF_FIRSTCALL_INIT();

		/* switch to memory context appropriate for multiple function calls */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* total number of tuples to be returned */
		funcctx->max_calls = PG_GETARG_UINT32(0);

		/* allocate memory for user context */
		fctx = (normal_rand_fctx *) palloc(sizeof(normal_rand_fctx));

		/*
		 * Use fctx to keep track of upper and lower bounds
		 * from call to call. It will also be used to carry over
		 * the spare value we get from the Box-Muller algorithm
		 * so that we only actually calculate a new value every
		 * other call.
		 */
		fctx->mean = PG_GETARG_FLOAT8(1);
		fctx->stddev = PG_GETARG_FLOAT8(2);
		fctx->carry_val = 0;
		fctx->use_carry = false;

		funcctx->user_fctx = fctx;

		/*
		 * we might actually get passed a negative number, but for this
		 * purpose it doesn't matter, just cast it as an unsigned value
		 */
		srandom(PG_GETARG_UINT32(3));

		MemoryContextSwitchTo(oldcontext);
    }

	/* stuff done on every call of the function */
 	funcctx = SRF_PERCALL_SETUP();

	call_cntr = funcctx->call_cntr;
	max_calls = funcctx->max_calls;
	fctx = funcctx->user_fctx;
	mean = fctx->mean;
	stddev = fctx->stddev;
	carry_val = fctx->carry_val;
	use_carry = fctx->use_carry;
 
 	if (call_cntr < max_calls)	/* do when there is more left to send */
 	{
		float8		result;

		if(use_carry)
		{
			/*
			 * reset use_carry and use second value obtained on last pass
			 */
			fctx->use_carry = false;
			result = carry_val;
		}
		else
		{
			float8		normval_1;
			float8		normval_2;

			/* Get the next two normal values */
			get_normal_pair(&normval_1, &normval_2);

			/* use the first */
			result = mean + (stddev * normval_1);

			/* and save the second */
			fctx->carry_val = mean + (stddev * normval_2);
			fctx->use_carry = true;
		}

		/* send the result */
 		SRF_RETURN_NEXT(funcctx, Float8GetDatum(result));
 	}
 	else	/* do when there is no more left */
 	{
 		SRF_RETURN_DONE(funcctx);
 	}
}

/*
 * get_normal_pair()
 * Assigns normally distributed (Gaussian) values to a pair of provided
 * parameters, with mean 0, standard deviation 1.
 *
 * This routine implements Algorithm P (Polar method for normal deviates)
 * from Knuth's _The_Art_of_Computer_Programming_, Volume 2, 3rd ed., pages
 * 122-126. Knuth cites his source as "The polar method", G. E. P. Box, M. E.
 * Muller, and G. Marsaglia, _Annals_Math,_Stat._ 29 (1958), 610-611.
 *
 */
static void
get_normal_pair(float8 *x1, float8 *x2)
{
	float8	u1, u2, v1, v2, s;

	for(;;)
	{
		u1 = (float8) random() / (float8) RAND_MAX;
		u2 = (float8) random() / (float8) RAND_MAX;

		v1 = (2.0 * u1) - 1.0;
		v2 = (2.0 * u2) - 1.0;

		s = pow(v1, 2) + pow(v2, 2);

		if (s >= 1.0)
			continue;

		if (s == 0)
		{
			*x1 = 0;
			*x2 = 0;
		}
		else
		{
			*x1 = v1 * sqrt((-2.0 * log(s)) / s);
			*x2 = v2 * sqrt((-2.0 * log(s)) / s);
		}

		return;
	}
}

/*
 * crosstab - create a crosstab of rowids and values columns from a
 * SQL statement returning one rowid column, one category column,
 * and one value column.
 *
 * e.g. given sql which produces:
 *
 *			rowid	cat		value
 *			------+-------+-------
 * 			row1	cat1	val1
 * 			row1	cat2	val2
 * 			row1	cat3	val3
 * 			row1	cat4	val4
 * 			row2	cat1	val5
 * 			row2	cat2	val6
 * 			row2	cat3	val7
 * 			row2	cat4	val8
 *
 * crosstab returns:
 *					<===== values columns =====>
 *			rowid	cat1	cat2	cat3	cat4
 *			------+-------+-------+-------+-------
 * 			row1	val1	val2	val3	val4
 * 			row2	val5	val6	val7	val8
 *
 * NOTES:
 * 1. SQL result must be ordered by 1,2.
 * 2. The number of values columns depends on the tuple description
 *    of the function's declared return type.
 * 2. Missing values (i.e. not enough adjacent rows of same rowid to
 *    fill the number of result values columns) are filled in with nulls.
 * 3. Extra values (i.e. too many adjacent rows of same rowid to fill
 *    the number of result values columns) are skipped.
 * 4. Rows with all nulls in the values columns are skipped.
 */
PG_FUNCTION_INFO_V1(crosstab);
Datum
crosstab(PG_FUNCTION_ARGS)
{
	FuncCallContext	   *funcctx;
	TupleDesc			ret_tupdesc;
	int					call_cntr;
	int					max_calls;
	TupleTableSlot	   *slot;
	AttInMetadata	   *attinmeta;
	SPITupleTable	   *spi_tuptable = NULL;
	TupleDesc			spi_tupdesc;
	char			   *lastrowid = NULL;
	crosstab_fctx	   *fctx;
	int					i;
	int					num_categories;
	MemoryContext		oldcontext;

	/* stuff done only on the first call of the function */
 	if(SRF_IS_FIRSTCALL())
 	{
		char		   *sql = GET_STR(PG_GETARG_TEXT_P(0));
		Oid 			funcid = fcinfo->flinfo->fn_oid;
		Oid 			functypeid;
		char			functyptype;
		TupleDesc		tupdesc = NULL;
		int				ret;
		int				proc;

		/* create a function context for cross-call persistence */
 		funcctx = SRF_FIRSTCALL_INIT();

		/* switch to memory context appropriate for multiple function calls */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* Connect to SPI manager */
		if ((ret = SPI_connect()) < 0)
			elog(ERROR, "crosstab: SPI_connect returned %d", ret);

		/* Retrieve the desired rows */
		ret = SPI_exec(sql, 0);
		proc = SPI_processed;

		/* Check for qualifying tuples */
		if ((ret == SPI_OK_SELECT) && (proc > 0))
		{
			spi_tuptable = SPI_tuptable;
			spi_tupdesc = spi_tuptable->tupdesc;

			/*
			 * The provided SQL query must always return three columns.
			 *
			 * 1. rowname	the label or identifier for each row in the final
			 *				result
			 * 2. category	the label or identifier for each column in the
			 *				final result
			 * 3. values	the value for each column in the final result
			 */
			if (spi_tupdesc->natts != 3)
				elog(ERROR, "crosstab: provided SQL must return 3 columns;"
								" a rowid, a category, and a values column");
		}
		else
		{
			/* no qualifying tuples */
			SPI_finish();
	 		SRF_RETURN_DONE(funcctx);
		}

		/* SPI switches context on us, so reset it */
		MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* get the typeid that represents our return type */
		functypeid = get_func_rettype(funcid);

		/* check typtype to see if we have a predetermined return type */
		functyptype = get_typtype(functypeid);
		
		if (functyptype == 'c')
		{
			/* Build a tuple description for a functypeid tuple */
			tupdesc = TypeGetTupleDesc(functypeid, NIL);
		}
		else if (functyptype == 'p' && functypeid == RECORDOID)
		{
			if (fcinfo->nargs != 2)
				elog(ERROR, "Wrong number of arguments specified for function");
			else
			{
				int	num_catagories = PG_GETARG_INT32(1);

				tupdesc = make_crosstab_tupledesc(spi_tupdesc, num_catagories);
			}
		}
		else if (functyptype == 'b')
			elog(ERROR, "Invalid kind of return type specified for function");
		else
			elog(ERROR, "Unknown kind of return type specified for function");

		/*
		 * Check that return tupdesc is compatible with the one we got
		 * from ret_relname, at least based on number and type of
		 * attributes
		 */
		if (!compatTupleDescs(tupdesc, spi_tupdesc))
			elog(ERROR, "crosstab: return and sql tuple descriptions are"
									" incompatible");

		/* allocate a slot for a tuple with this tupdesc */
		slot = TupleDescGetSlot(tupdesc);

		/* assign slot to function context */
		funcctx->slot = slot;

		/*
		 * Generate attribute metadata needed later to produce tuples from raw
		 * C strings
		 */
		attinmeta = TupleDescGetAttInMetadata(tupdesc);
		funcctx->attinmeta = attinmeta;

		/* allocate memory for user context */
		fctx = (crosstab_fctx *) palloc(sizeof(crosstab_fctx));

		/*
		 * Save spi data for use across calls
		 */
		fctx->spi_tuptable = spi_tuptable;
		fctx->lastrowid = NULL;
		funcctx->user_fctx = fctx;

		/* total number of tuples to be returned */
		funcctx->max_calls = proc;

		MemoryContextSwitchTo(oldcontext);
    }

	/* stuff done on every call of the function */
 	funcctx = SRF_PERCALL_SETUP();

	/*
	 * initialize per-call variables
	 */
	call_cntr = funcctx->call_cntr;
	max_calls = funcctx->max_calls;

	/* return slot for our tuple */
	slot = funcctx->slot;

	/* user context info */
	fctx = (crosstab_fctx *) funcctx->user_fctx;
	lastrowid = fctx->lastrowid;
	spi_tuptable = fctx->spi_tuptable;

	/* the sql tuple */
	spi_tupdesc = spi_tuptable->tupdesc;

	/* attribute return type and return tuple description */
	attinmeta = funcctx->attinmeta;
	ret_tupdesc = attinmeta->tupdesc;

	/* the return tuple always must have 1 rowid + num_categories columns */
	num_categories = ret_tupdesc->natts - 1;
 
	if (call_cntr < max_calls)	/* do when there is more left to send */
 	{
		HeapTuple	tuple;
		Datum		result;
		char	  **values;
		bool		allnulls = true;

		while (true)
		{
			/* allocate space */
			values = (char **) palloc((1 + num_categories) * sizeof(char *));

			/* and make sure it's clear */
			memset(values, '\0', (1 + num_categories) * sizeof(char *));

			/*
			 * now loop through the sql results and assign each value
			 * in sequence to the next category
			 */
			for (i = 0; i < num_categories; i++)
			{
				HeapTuple	spi_tuple;
				char	   *rowid = NULL;

				/* see if we've gone too far already */
				if (call_cntr >= max_calls)
					break;

				/* get the next sql result tuple */
				spi_tuple = spi_tuptable->vals[call_cntr];

				/* get the rowid from the current sql result tuple */
				rowid = SPI_getvalue(spi_tuple, spi_tupdesc, 1);

				/* 
				 * If this is the first pass through the values for this rowid
				 * set it, otherwise make sure it hasn't changed on us. Also
				 * check to see if the rowid is the same as that of the last
				 * tuple sent -- if so, skip this tuple entirely
				 */
				if (i == 0)
					values[0] = pstrdup(rowid);

				if ((rowid != NULL) && (strcmp(rowid, values[0]) == 0))
				{
					if ((lastrowid != NULL) && (strcmp(rowid, lastrowid) == 0))
						break;
					else if (allnulls == true)
						allnulls = false;

					/*
					 * Get the next category item value, which is alway attribute
					 * number three.
					 *
					 * Be careful to sssign the value to the array index based
					 * on which category we are presently processing.
					 */
					values[1 + i] = SPI_getvalue(spi_tuple, spi_tupdesc, 3);

					/*
					 * increment the counter since we consume a row
					 * for each category, but not for last pass
					 * because the API will do that for us
					 */
					if (i < (num_categories - 1))
						call_cntr = ++funcctx->call_cntr;
				}
				else
				{
					/*
					 * We'll fill in NULLs for the missing values,
					 * but we need to decrement the counter since
					 * this sql result row doesn't belong to the current
					 * output tuple.
					 */
					call_cntr = --funcctx->call_cntr;
					break;
				}

				if (rowid != NULL)
					xpfree(rowid);
			}

			xpfree(fctx->lastrowid);

			if (values[0] != NULL)
			{
				/* switch to memory context appropriate for multiple function calls */
				oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

				lastrowid = fctx->lastrowid = pstrdup(values[0]);
				MemoryContextSwitchTo(oldcontext);
			}

			if (!allnulls)
			{
				/* build the tuple */
				tuple = BuildTupleFromCStrings(attinmeta, values);

				/* make the tuple into a datum */
				result = TupleGetDatum(slot, tuple);

				/* Clean up */
				for (i = 0; i < num_categories + 1; i++)
					if (values[i] != NULL)
						xpfree(values[i]);
				xpfree(values);

		 		SRF_RETURN_NEXT(funcctx, result);
			}
			else
			{
				/*
				 * Skipping this tuple entirely, but we need to advance
				 * the counter like the API would if we had returned
				 * one.
				 */
				call_cntr = ++funcctx->call_cntr;

				/* we'll start over at the top */
				xpfree(values);

				/* see if we've gone too far already */
				if (call_cntr >= max_calls)
				{
					/* release SPI related resources */
					SPI_finish();
			 		SRF_RETURN_DONE(funcctx);
				}
			}
		}
	}
 	else	/* do when there is no more left */
 	{
		/* release SPI related resources */
		SPI_finish();
 		SRF_RETURN_DONE(funcctx);
 	}
}

/*
 * Check if two tupdescs match in type of attributes
 */
static bool
compatTupleDescs(TupleDesc ret_tupdesc, TupleDesc sql_tupdesc)
{
	int			i;
	Form_pg_attribute	ret_attr;
	Oid					ret_atttypid;
	Form_pg_attribute	sql_attr;
	Oid					sql_atttypid;

	/* check the rowid types match */
	ret_atttypid = ret_tupdesc->attrs[0]->atttypid;
	sql_atttypid = sql_tupdesc->attrs[0]->atttypid;
	if (ret_atttypid != sql_atttypid)
		elog(ERROR, "compatTupleDescs: SQL rowid datatype does not match"
						" return rowid datatype");

	/*
	 *	- attribute [1] of the sql tuple is the category;
	 *		no need to check it
	 *	- attribute [2] of the sql tuple should match
	 *		attributes [1] to [natts] of the return tuple
	 */
	sql_attr = sql_tupdesc->attrs[2];
	for (i = 1; i < ret_tupdesc->natts; i++)
	{
		ret_attr = ret_tupdesc->attrs[i];

		if (ret_attr->atttypid != sql_attr->atttypid)
			return false;
	}

	/* OK, the two tupdescs are compatible for our purposes */
	return true;
}

static TupleDesc
make_crosstab_tupledesc(TupleDesc spi_tupdesc, int num_catagories)
{
	Form_pg_attribute	sql_attr;
	Oid					sql_atttypid;
	TupleDesc			tupdesc;
	int					natts;
	AttrNumber			attnum;
	char				attname[NAMEDATALEN];
	int					i;

	/*
	 * We need to build a tuple description with one column
	 * for the rowname, and num_catagories columns for the values.
	 * Each must be of the same type as the corresponding
	 * spi result input column.
	 */
	natts = num_catagories + 1;
	tupdesc = CreateTemplateTupleDesc(natts, WITHOUTOID);

	/* first the rowname column */
	attnum = 1;

	sql_attr = spi_tupdesc->attrs[0];
	sql_atttypid = sql_attr->atttypid;

	strcpy(attname, "rowname");

	TupleDescInitEntry(tupdesc, attnum, attname, sql_atttypid,
					   -1, 0, false);

	/* now the catagory values columns */
	sql_attr = spi_tupdesc->attrs[2];
	sql_atttypid = sql_attr->atttypid;

	for (i = 0; i < num_catagories; i++)
	{
		attnum++;

		sprintf(attname, "category_%d", i + 1);
		TupleDescInitEntry(tupdesc, attnum, attname, sql_atttypid,
						   -1, 0, false);
	}

	return tupdesc;
}

