/*
 * tablefunc
 *
 * Sample to demonstrate C functions which return setof scalar
 * and setof composite.
 * Joe Conway <mail@joeconway.com>
 * And contributors:
 * Nabil Sayegh <postgresql@e-trolley.de>
 *
 * Copyright (c) 2002-2008, PostgreSQL Global Development Group
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
#include "postgres.h"

#include <math.h>

#include "fmgr.h"
#include "funcapi.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"

#include "tablefunc.h"

PG_MODULE_MAGIC;

static HTAB *load_categories_hash(char *cats_sql, MemoryContext per_query_ctx);
static Tuplestorestate *get_crosstab_tuplestore(char *sql,
						HTAB *crosstab_hash,
						TupleDesc tupdesc,
						MemoryContext per_query_ctx);
static void validateConnectbyTupleDesc(TupleDesc tupdesc, bool show_branch, bool show_serial);
static bool compatCrosstabTupleDescs(TupleDesc tupdesc1, TupleDesc tupdesc2);
static bool compatConnectbyTupleDescs(TupleDesc tupdesc1, TupleDesc tupdesc2);
static void get_normal_pair(float8 *x1, float8 *x2);
static Tuplestorestate *connectby(char *relname,
		  char *key_fld,
		  char *parent_key_fld,
		  char *orderby_fld,
		  char *branch_delim,
		  char *start_with,
		  int max_depth,
		  bool show_branch,
		  bool show_serial,
		  MemoryContext per_query_ctx,
		  AttInMetadata *attinmeta);
static Tuplestorestate *build_tuplestore_recursively(char *key_fld,
							 char *parent_key_fld,
							 char *relname,
							 char *orderby_fld,
							 char *branch_delim,
							 char *start_with,
							 char *branch,
							 int level,
							 int *serial,
							 int max_depth,
							 bool show_branch,
							 bool show_serial,
							 MemoryContext per_query_ctx,
							 AttInMetadata *attinmeta,
							 Tuplestorestate *tupstore);
static char *quote_literal_cstr(char *rawstr);

typedef struct
{
	float8		mean;			/* mean of the distribution */
	float8		stddev;			/* stddev of the distribution */
	float8		carry_val;		/* hold second generated value */
	bool		use_carry;		/* use second generated value */
}	normal_rand_fctx;

typedef struct
{
	SPITupleTable *spi_tuptable;	/* sql results from user query */
	char	   *lastrowid;		/* rowid of the last tuple sent */
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

#define xpstrdup(tgtvar_, srcvar_) \
	do { \
		if (srcvar_) \
			tgtvar_ = pstrdup(srcvar_); \
		else \
			tgtvar_ = NULL; \
	} while (0)

#define xstreq(tgtvar_, srcvar_) \
	(((tgtvar_ == NULL) && (srcvar_ == NULL)) || \
	 ((tgtvar_ != NULL) && (srcvar_ != NULL) && (strcmp(tgtvar_, srcvar_) == 0)))

/* sign, 10 digits, '\0' */
#define INT32_STRLEN	12

/* stored info for a crosstab category */
typedef struct crosstab_cat_desc
{
	char	   *catname;		/* full category name */
	int			attidx;			/* zero based */
}	crosstab_cat_desc;

#define MAX_CATNAME_LEN			NAMEDATALEN
#define INIT_CATS				64

#define crosstab_HashTableLookup(HASHTAB, CATNAME, CATDESC) \
do { \
	crosstab_HashEnt *hentry; char key[MAX_CATNAME_LEN]; \
	\
	MemSet(key, 0, MAX_CATNAME_LEN); \
	snprintf(key, MAX_CATNAME_LEN - 1, "%s", CATNAME); \
	hentry = (crosstab_HashEnt*) hash_search(HASHTAB, \
										 key, HASH_FIND, NULL); \
	if (hentry) \
		CATDESC = hentry->catdesc; \
	else \
		CATDESC = NULL; \
} while(0)

#define crosstab_HashTableInsert(HASHTAB, CATDESC) \
do { \
	crosstab_HashEnt *hentry; bool found; char key[MAX_CATNAME_LEN]; \
	\
	MemSet(key, 0, MAX_CATNAME_LEN); \
	snprintf(key, MAX_CATNAME_LEN - 1, "%s", CATDESC->catname); \
	hentry = (crosstab_HashEnt*) hash_search(HASHTAB, \
										 key, HASH_ENTER, &found); \
	if (found) \
		ereport(ERROR, \
				(errcode(ERRCODE_DUPLICATE_OBJECT), \
				 errmsg("duplicate category name"))); \
	hentry->catdesc = CATDESC; \
} while(0)

/* hash table */
typedef struct crosstab_hashent
{
	char		internal_catname[MAX_CATNAME_LEN];
	crosstab_cat_desc *catdesc;
}	crosstab_HashEnt;

/*
 * normal_rand - return requested number of random values
 * with a Gaussian (Normal) distribution.
 *
 * inputs are int numvals, float8 mean, and float8 stddev
 * returns setof float8
 */
PG_FUNCTION_INFO_V1(normal_rand);
Datum
normal_rand(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	int			call_cntr;
	int			max_calls;
	normal_rand_fctx *fctx;
	float8		mean;
	float8		stddev;
	float8		carry_val;
	bool		use_carry;
	MemoryContext oldcontext;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * switch to memory context appropriate for multiple function calls
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* total number of tuples to be returned */
		funcctx->max_calls = PG_GETARG_UINT32(0);

		/* allocate memory for user context */
		fctx = (normal_rand_fctx *) palloc(sizeof(normal_rand_fctx));

		/*
		 * Use fctx to keep track of upper and lower bounds from call to call.
		 * It will also be used to carry over the spare value we get from the
		 * Box-Muller algorithm so that we only actually calculate a new value
		 * every other call.
		 */
		fctx->mean = PG_GETARG_FLOAT8(1);
		fctx->stddev = PG_GETARG_FLOAT8(2);
		fctx->carry_val = 0;
		fctx->use_carry = false;

		funcctx->user_fctx = fctx;

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

		if (use_carry)
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
	else
		/* do when there is no more left */
		SRF_RETURN_DONE(funcctx);
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
	float8		u1,
				u2,
				v1,
				v2,
				s;

	do
	{
		u1 = (float8) random() / (float8) MAX_RANDOM_VALUE;
		u2 = (float8) random() / (float8) MAX_RANDOM_VALUE;

		v1 = (2.0 * u1) - 1.0;
		v2 = (2.0 * u2) - 1.0;

		s = v1 * v1 + v2 * v2;
	} while (s >= 1.0);

	if (s == 0)
	{
		*x1 = 0;
		*x2 = 0;
	}
	else
	{
		s = sqrt((-2.0 * log(s)) / s);
		*x1 = v1 * s;
		*x2 = v2 * s;
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
 *			row1	cat1	val1
 *			row1	cat2	val2
 *			row1	cat3	val3
 *			row1	cat4	val4
 *			row2	cat1	val5
 *			row2	cat2	val6
 *			row2	cat3	val7
 *			row2	cat4	val8
 *
 * crosstab returns:
 *					<===== values columns =====>
 *			rowid	cat1	cat2	cat3	cat4
 *			------+-------+-------+-------+-------
 *			row1	val1	val2	val3	val4
 *			row2	val5	val6	val7	val8
 *
 * NOTES:
 * 1. SQL result must be ordered by 1,2.
 * 2. The number of values columns depends on the tuple description
 *	  of the function's declared return type.  The return type's columns
 *	  must match the datatypes of the SQL query's result.  The datatype
 *	  of the category column can be anything, however.
 * 3. Missing values (i.e. not enough adjacent rows of same rowid to
 *	  fill the number of result values columns) are filled in with nulls.
 * 4. Extra values (i.e. too many adjacent rows of same rowid to fill
 *	  the number of result values columns) are skipped.
 * 5. Rows with all nulls in the values columns are skipped.
 */
PG_FUNCTION_INFO_V1(crosstab);
Datum
crosstab(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	TupleDesc	ret_tupdesc;
	int			call_cntr;
	int			max_calls;
	AttInMetadata *attinmeta;
	SPITupleTable *spi_tuptable = NULL;
	TupleDesc	spi_tupdesc;
	char	   *lastrowid = NULL;
	crosstab_fctx *fctx;
	int			i;
	int			num_categories;
	bool		firstpass = false;
	MemoryContext oldcontext;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		char	   *sql = GET_STR(PG_GETARG_TEXT_P(0));
		TupleDesc	tupdesc;
		int			ret;
		int			proc;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/* Connect to SPI manager */
		if ((ret = SPI_connect()) < 0)
			/* internal error */
			elog(ERROR, "crosstab: SPI_connect returned %d", ret);

		/* Retrieve the desired rows */
		ret = SPI_execute(sql, true, 0);
		proc = SPI_processed;

		/* Check for qualifying tuples */
		if ((ret == SPI_OK_SELECT) && (proc > 0))
		{
			spi_tuptable = SPI_tuptable;
			spi_tupdesc = spi_tuptable->tupdesc;

			/*----------
			 * The provided SQL query must always return three columns.
			 *
			 * 1. rowname
			 *	the label or identifier for each row in the final result
			 * 2. category
			 *	the label or identifier for each column in the final result
			 * 3. values
			 *	the value for each column in the final result
			 *----------
			 */
			if (spi_tupdesc->natts != 3)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid source data SQL statement"),
						 errdetail("The provided SQL must return 3 "
								   "columns: rowid, category, and values.")));
		}
		else
		{
			/* no qualifying tuples */
			SPI_finish();
			SRF_RETURN_DONE(funcctx);
		}

		/* get a tuple descriptor for our result type */
		switch (get_call_result_type(fcinfo, NULL, &tupdesc))
		{
			case TYPEFUNC_COMPOSITE:
				/* success */
				break;
			case TYPEFUNC_RECORD:
				/* failed to determine actual type of RECORD */
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("function returning record called in context "
								"that cannot accept type record")));
				break;
			default:
				/* result type isn't composite */
				elog(ERROR, "return type must be a row type");
				break;
		}

		/*
		 * Check that return tupdesc is compatible with the data we got from
		 * SPI, at least based on number and type of attributes
		 */
		if (!compatCrosstabTupleDescs(tupdesc, spi_tupdesc))
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("return and sql tuple descriptions are " \
							"incompatible")));

		/*
		 * switch to memory context appropriate for multiple function calls
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* make sure we have a persistent copy of the tupdesc */
		tupdesc = CreateTupleDescCopy(tupdesc);

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
		firstpass = true;
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();

	/*
	 * initialize per-call variables
	 */
	call_cntr = funcctx->call_cntr;
	max_calls = funcctx->max_calls;

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
		bool		skip_tuple = false;

		while (true)
		{
			/* allocate space */
			values = (char **) palloc((1 + num_categories) * sizeof(char *));

			/* and make sure it's clear */
			memset(values, '\0', (1 + num_categories) * sizeof(char *));

			/*
			 * now loop through the sql results and assign each value in
			 * sequence to the next category
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
				 * If this is the first pass through the values for this
				 * rowid, set the first column to rowid
				 */
				if (i == 0)
				{
					xpstrdup(values[0], rowid);

					/*
					 * Check to see if the rowid is the same as that of the
					 * last tuple sent -- if so, skip this tuple entirely
					 */
					if (!firstpass && xstreq(lastrowid, rowid))
					{
						skip_tuple = true;
						break;
					}
				}

				/*
				 * If rowid hasn't changed on us, continue building the ouput
				 * tuple.
				 */
				if (xstreq(rowid, values[0]))
				{
					/*
					 * Get the next category item value, which is always
					 * attribute number three.
					 *
					 * Be careful to assign the value to the array index based
					 * on which category we are presently processing.
					 */
					values[1 + i] = SPI_getvalue(spi_tuple, spi_tupdesc, 3);

					/*
					 * increment the counter since we consume a row for each
					 * category, but not for last pass because the API will do
					 * that for us
					 */
					if (i < (num_categories - 1))
						call_cntr = ++funcctx->call_cntr;
				}
				else
				{
					/*
					 * We'll fill in NULLs for the missing values, but we need
					 * to decrement the counter since this sql result row
					 * doesn't belong to the current output tuple.
					 */
					call_cntr = --funcctx->call_cntr;
					break;
				}
				xpfree(rowid);
			}

			/*
			 * switch to memory context appropriate for multiple function
			 * calls
			 */
			oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

			xpfree(fctx->lastrowid);
			xpstrdup(fctx->lastrowid, values[0]);
			lastrowid = fctx->lastrowid;

			MemoryContextSwitchTo(oldcontext);

			if (!skip_tuple)
			{
				/* build the tuple */
				tuple = BuildTupleFromCStrings(attinmeta, values);

				/* make the tuple into a datum */
				result = HeapTupleGetDatum(tuple);

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
				 * Skipping this tuple entirely, but we need to advance the
				 * counter like the API would if we had returned one.
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

				/* need to reset this before the next tuple is started */
				skip_tuple = false;
			}
		}
	}
	else
		/* do when there is no more left */
	{
		/* release SPI related resources */
		SPI_finish();
		SRF_RETURN_DONE(funcctx);
	}
}

/*
 * crosstab_hash - reimplement crosstab as materialized function and
 * properly deal with missing values (i.e. don't pack remaining
 * values to the left)
 *
 * crosstab - create a crosstab of rowids and values columns from a
 * SQL statement returning one rowid column, one category column,
 * and one value column.
 *
 * e.g. given sql which produces:
 *
 *			rowid	cat		value
 *			------+-------+-------
 *			row1	cat1	val1
 *			row1	cat2	val2
 *			row1	cat4	val4
 *			row2	cat1	val5
 *			row2	cat2	val6
 *			row2	cat3	val7
 *			row2	cat4	val8
 *
 * crosstab returns:
 *					<===== values columns =====>
 *			rowid	cat1	cat2	cat3	cat4
 *			------+-------+-------+-------+-------
 *			row1	val1	val2	null	val4
 *			row2	val5	val6	val7	val8
 *
 * NOTES:
 * 1. SQL result must be ordered by 1.
 * 2. The number of values columns depends on the tuple description
 *	  of the function's declared return type.
 * 3. Missing values (i.e. missing category) are filled in with nulls.
 * 4. Extra values (i.e. not in category results) are skipped.
 */
PG_FUNCTION_INFO_V1(crosstab_hash);
Datum
crosstab_hash(PG_FUNCTION_ARGS)
{
	char	   *sql = GET_STR(PG_GETARG_TEXT_P(0));
	char	   *cats_sql = GET_STR(PG_GETARG_TEXT_P(1));
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	HTAB	   *crosstab_hash;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* get the requested return tuple description */
	tupdesc = CreateTupleDescCopy(rsinfo->expectedDesc);

	/*
	 * Check to make sure we have a reasonable tuple descriptor
	 *
	 * Note we will attempt to coerce the values into whatever the return
	 * attribute type is and depend on the "in" function to complain if
	 * needed.
	 */
	if (tupdesc->natts < 2)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("query-specified return tuple and " \
						"crosstab function are not compatible")));

	/* load up the categories hash table */
	crosstab_hash = load_categories_hash(cats_sql, per_query_ctx);

	/* let the caller know we're sending back a tuplestore */
	rsinfo->returnMode = SFRM_Materialize;

	/* now go build it */
	rsinfo->setResult = get_crosstab_tuplestore(sql,
												crosstab_hash,
												tupdesc,
												per_query_ctx);

	/*
	 * SFRM_Materialize mode expects us to return a NULL Datum. The actual
	 * tuples are in our tuplestore and passed back through rsinfo->setResult.
	 * rsinfo->setDesc is set to the tuple description that we actually used
	 * to build our tuples with, so the caller can verify we did what it was
	 * expecting.
	 */
	rsinfo->setDesc = tupdesc;
	MemoryContextSwitchTo(oldcontext);

	return (Datum) 0;
}

/*
 * load up the categories hash table
 */
static HTAB *
load_categories_hash(char *cats_sql, MemoryContext per_query_ctx)
{
	HTAB	   *crosstab_hash;
	HASHCTL		ctl;
	int			ret;
	int			proc;
	MemoryContext SPIcontext;

	/* initialize the category hash table */
	MemSet(&ctl, 0, sizeof(ctl));
	ctl.keysize = MAX_CATNAME_LEN;
	ctl.entrysize = sizeof(crosstab_HashEnt);
	ctl.hcxt = per_query_ctx;

	/*
	 * use INIT_CATS, defined above as a guess of how many hash table entries
	 * to create, initially
	 */
	crosstab_hash = hash_create("crosstab hash",
								INIT_CATS,
								&ctl,
								HASH_ELEM | HASH_CONTEXT);

	/* Connect to SPI manager */
	if ((ret = SPI_connect()) < 0)
		/* internal error */
		elog(ERROR, "load_categories_hash: SPI_connect returned %d", ret);

	/* Retrieve the category name rows */
	ret = SPI_execute(cats_sql, true, 0);
	proc = SPI_processed;

	/* Check for qualifying tuples */
	if ((ret == SPI_OK_SELECT) && (proc > 0))
	{
		SPITupleTable *spi_tuptable = SPI_tuptable;
		TupleDesc	spi_tupdesc = spi_tuptable->tupdesc;
		int			i;

		/*
		 * The provided categories SQL query must always return one column:
		 * category - the label or identifier for each column
		 */
		if (spi_tupdesc->natts != 1)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("provided \"categories\" SQL must " \
							"return 1 column of at least one row")));

		for (i = 0; i < proc; i++)
		{
			crosstab_cat_desc *catdesc;
			char	   *catname;
			HeapTuple	spi_tuple;

			/* get the next sql result tuple */
			spi_tuple = spi_tuptable->vals[i];

			/* get the category from the current sql result tuple */
			catname = SPI_getvalue(spi_tuple, spi_tupdesc, 1);

			SPIcontext = MemoryContextSwitchTo(per_query_ctx);

			catdesc = (crosstab_cat_desc *) palloc(sizeof(crosstab_cat_desc));
			catdesc->catname = catname;
			catdesc->attidx = i;

			/* Add the proc description block to the hashtable */
			crosstab_HashTableInsert(crosstab_hash, catdesc);

			MemoryContextSwitchTo(SPIcontext);
		}
	}

	if (SPI_finish() != SPI_OK_FINISH)
		/* internal error */
		elog(ERROR, "load_categories_hash: SPI_finish() failed");

	return crosstab_hash;
}

/*
 * create and populate the crosstab tuplestore using the provided source query
 */
static Tuplestorestate *
get_crosstab_tuplestore(char *sql,
						HTAB *crosstab_hash,
						TupleDesc tupdesc,
						MemoryContext per_query_ctx)
{
	Tuplestorestate *tupstore;
	int			num_categories = hash_get_num_entries(crosstab_hash);
	AttInMetadata *attinmeta = TupleDescGetAttInMetadata(tupdesc);
	char	  **values;
	HeapTuple	tuple;
	int			ret;
	int			proc;
	MemoryContext SPIcontext;

	/* initialize our tuplestore */
	tupstore = tuplestore_begin_heap(true, false, work_mem);

	/* Connect to SPI manager */
	if ((ret = SPI_connect()) < 0)
		/* internal error */
		elog(ERROR, "get_crosstab_tuplestore: SPI_connect returned %d", ret);

	/* Now retrieve the crosstab source rows */
	ret = SPI_execute(sql, true, 0);
	proc = SPI_processed;

	/* Check for qualifying tuples */
	if ((ret == SPI_OK_SELECT) && (proc > 0))
	{
		SPITupleTable *spi_tuptable = SPI_tuptable;
		TupleDesc	spi_tupdesc = spi_tuptable->tupdesc;
		int			ncols = spi_tupdesc->natts;
		char	   *rowid;
		char	   *lastrowid = NULL;
		bool		firstpass = true;
		int			i,
					j;
		int			result_ncols;

		if (num_categories == 0)
		{
			/* no qualifying category tuples */
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("provided \"categories\" SQL must " \
							"return 1 column of at least one row")));
		}

		/*
		 * The provided SQL query must always return at least three columns:
		 *
		 * 1. rowname	the label for each row - column 1 in the final result
		 * 2. category	the label for each value-column in the final result 3.
		 * value	 the values used to populate the value-columns
		 *
		 * If there are more than three columns, the last two are taken as
		 * "category" and "values". The first column is taken as "rowname".
		 * Additional columns (2 thru N-2) are assumed the same for the same
		 * "rowname", and are copied into the result tuple from the first time
		 * we encounter a particular rowname.
		 */
		if (ncols < 3)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid source data SQL statement"),
					 errdetail("The provided SQL must return 3 " \
							   " columns; rowid, category, and values.")));

		result_ncols = (ncols - 2) + num_categories;

		/* Recheck to make sure we tuple descriptor still looks reasonable */
		if (tupdesc->natts != result_ncols)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("invalid return type"),
					 errdetail("Query-specified return " \
							   "tuple has %d columns but crosstab " \
							   "returns %d.", tupdesc->natts, result_ncols)));

		/* allocate space */
		values = (char **) palloc(result_ncols * sizeof(char *));

		/* and make sure it's clear */
		memset(values, '\0', result_ncols * sizeof(char *));

		for (i = 0; i < proc; i++)
		{
			HeapTuple	spi_tuple;
			crosstab_cat_desc *catdesc;
			char	   *catname;

			/* get the next sql result tuple */
			spi_tuple = spi_tuptable->vals[i];

			/* get the rowid from the current sql result tuple */
			rowid = SPI_getvalue(spi_tuple, spi_tupdesc, 1);

			/*
			 * if we're on a new output row, grab the column values up to
			 * column N-2 now
			 */
			if (firstpass || !xstreq(lastrowid, rowid))
			{
				/*
				 * a new row means we need to flush the old one first, unless
				 * we're on the very first row
				 */
				if (!firstpass)
				{
					/* rowid changed, flush the previous output row */
					tuple = BuildTupleFromCStrings(attinmeta, values);

					/* switch to appropriate context while storing the tuple */
					SPIcontext = MemoryContextSwitchTo(per_query_ctx);
					tuplestore_puttuple(tupstore, tuple);
					MemoryContextSwitchTo(SPIcontext);

					for (j = 0; j < result_ncols; j++)
						xpfree(values[j]);
				}

				values[0] = rowid;
				for (j = 1; j < ncols - 2; j++)
					values[j] = SPI_getvalue(spi_tuple, spi_tupdesc, j + 1);

				/* we're no longer on the first pass */
				firstpass = false;
			}

			/* look up the category and fill in the appropriate column */
			catname = SPI_getvalue(spi_tuple, spi_tupdesc, ncols - 1);

			if (catname != NULL)
			{
				crosstab_HashTableLookup(crosstab_hash, catname, catdesc);

				if (catdesc)
					values[catdesc->attidx + ncols - 2] =
						SPI_getvalue(spi_tuple, spi_tupdesc, ncols);
			}

			xpfree(lastrowid);
			xpstrdup(lastrowid, rowid);
		}

		/* flush the last output row */
		tuple = BuildTupleFromCStrings(attinmeta, values);

		/* switch to appropriate context while storing the tuple */
		SPIcontext = MemoryContextSwitchTo(per_query_ctx);
		tuplestore_puttuple(tupstore, tuple);
		MemoryContextSwitchTo(SPIcontext);
	}

	if (SPI_finish() != SPI_OK_FINISH)
		/* internal error */
		elog(ERROR, "get_crosstab_tuplestore: SPI_finish() failed");

	tuplestore_donestoring(tupstore);

	return tupstore;
}

/*
 * connectby_text - produce a result set from a hierarchical (parent/child)
 * table.
 *
 * e.g. given table foo:
 *
 *			keyid	parent_keyid pos
 *			------+------------+--
 *			row1	NULL		 0
 *			row2	row1		 0
 *			row3	row1		 0
 *			row4	row2		 1
 *			row5	row2		 0
 *			row6	row4		 0
 *			row7	row3		 0
 *			row8	row6		 0
 *			row9	row5		 0
 *
 *
 * connectby(text relname, text keyid_fld, text parent_keyid_fld
 *			  [, text orderby_fld], text start_with, int max_depth
 *			  [, text branch_delim])
 * connectby('foo', 'keyid', 'parent_keyid', 'pos', 'row2', 0, '~') returns:
 *
 *		keyid	parent_id	level	 branch				serial
 *		------+-----------+--------+-----------------------
 *		row2	NULL		  0		  row2				  1
 *		row5	row2		  1		  row2~row5			  2
 *		row9	row5		  2		  row2~row5~row9	  3
 *		row4	row2		  1		  row2~row4			  4
 *		row6	row4		  2		  row2~row4~row6	  5
 *		row8	row6		  3		  row2~row4~row6~row8 6
 *
 */
PG_FUNCTION_INFO_V1(connectby_text);

#define CONNECTBY_NCOLS					4
#define CONNECTBY_NCOLS_NOBRANCH		3

Datum
connectby_text(PG_FUNCTION_ARGS)
{
	char	   *relname = GET_STR(PG_GETARG_TEXT_P(0));
	char	   *key_fld = GET_STR(PG_GETARG_TEXT_P(1));
	char	   *parent_key_fld = GET_STR(PG_GETARG_TEXT_P(2));
	char	   *start_with = GET_STR(PG_GETARG_TEXT_P(3));
	int			max_depth = PG_GETARG_INT32(4);
	char	   *branch_delim = NULL;
	bool		show_branch = false;
	bool		show_serial = false;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	AttInMetadata *attinmeta;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));

	if (fcinfo->nargs == 6)
	{
		branch_delim = GET_STR(PG_GETARG_TEXT_P(5));
		show_branch = true;
	}
	else
		/* default is no show, tilde for the delimiter */
		branch_delim = pstrdup("~");

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* get the requested return tuple description */
	tupdesc = CreateTupleDescCopy(rsinfo->expectedDesc);

	/* does it meet our needs */
	validateConnectbyTupleDesc(tupdesc, show_branch, show_serial);

	/* OK, use it then */
	attinmeta = TupleDescGetAttInMetadata(tupdesc);

	/* OK, go to work */
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = connectby(relname,
								  key_fld,
								  parent_key_fld,
								  NULL,
								  branch_delim,
								  start_with,
								  max_depth,
								  show_branch,
								  show_serial,
								  per_query_ctx,
								  attinmeta);
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	/*
	 * SFRM_Materialize mode expects us to return a NULL Datum. The actual
	 * tuples are in our tuplestore and passed back through rsinfo->setResult.
	 * rsinfo->setDesc is set to the tuple description that we actually used
	 * to build our tuples with, so the caller can verify we did what it was
	 * expecting.
	 */
	return (Datum) 0;
}

PG_FUNCTION_INFO_V1(connectby_text_serial);
Datum
connectby_text_serial(PG_FUNCTION_ARGS)
{
	char	   *relname = GET_STR(PG_GETARG_TEXT_P(0));
	char	   *key_fld = GET_STR(PG_GETARG_TEXT_P(1));
	char	   *parent_key_fld = GET_STR(PG_GETARG_TEXT_P(2));
	char	   *orderby_fld = GET_STR(PG_GETARG_TEXT_P(3));
	char	   *start_with = GET_STR(PG_GETARG_TEXT_P(4));
	int			max_depth = PG_GETARG_INT32(5);
	char	   *branch_delim = NULL;
	bool		show_branch = false;
	bool		show_serial = true;

	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	AttInMetadata *attinmeta;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));

	if (fcinfo->nargs == 7)
	{
		branch_delim = GET_STR(PG_GETARG_TEXT_P(6));
		show_branch = true;
	}
	else
		/* default is no show, tilde for the delimiter */
		branch_delim = pstrdup("~");

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* get the requested return tuple description */
	tupdesc = CreateTupleDescCopy(rsinfo->expectedDesc);

	/* does it meet our needs */
	validateConnectbyTupleDesc(tupdesc, show_branch, show_serial);

	/* OK, use it then */
	attinmeta = TupleDescGetAttInMetadata(tupdesc);

	/* OK, go to work */
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = connectby(relname,
								  key_fld,
								  parent_key_fld,
								  orderby_fld,
								  branch_delim,
								  start_with,
								  max_depth,
								  show_branch,
								  show_serial,
								  per_query_ctx,
								  attinmeta);
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	/*
	 * SFRM_Materialize mode expects us to return a NULL Datum. The actual
	 * tuples are in our tuplestore and passed back through rsinfo->setResult.
	 * rsinfo->setDesc is set to the tuple description that we actually used
	 * to build our tuples with, so the caller can verify we did what it was
	 * expecting.
	 */
	return (Datum) 0;
}


/*
 * connectby - does the real work for connectby_text()
 */
static Tuplestorestate *
connectby(char *relname,
		  char *key_fld,
		  char *parent_key_fld,
		  char *orderby_fld,
		  char *branch_delim,
		  char *start_with,
		  int max_depth,
		  bool show_branch,
		  bool show_serial,
		  MemoryContext per_query_ctx,
		  AttInMetadata *attinmeta)
{
	Tuplestorestate *tupstore = NULL;
	int			ret;
	MemoryContext oldcontext;

	int			serial = 1;

	/* Connect to SPI manager */
	if ((ret = SPI_connect()) < 0)
		/* internal error */
		elog(ERROR, "connectby: SPI_connect returned %d", ret);

	/* switch to longer term context to create the tuple store */
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* initialize our tuplestore */
	tupstore = tuplestore_begin_heap(true, false, work_mem);

	MemoryContextSwitchTo(oldcontext);

	/* now go get the whole tree */
	tupstore = build_tuplestore_recursively(key_fld,
											parent_key_fld,
											relname,
											orderby_fld,
											branch_delim,
											start_with,
											start_with, /* current_branch */
											0,	/* initial level is 0 */
											&serial,	/* initial serial is 1 */
											max_depth,
											show_branch,
											show_serial,
											per_query_ctx,
											attinmeta,
											tupstore);

	SPI_finish();

	return tupstore;
}

static Tuplestorestate *
build_tuplestore_recursively(char *key_fld,
							 char *parent_key_fld,
							 char *relname,
							 char *orderby_fld,
							 char *branch_delim,
							 char *start_with,
							 char *branch,
							 int level,
							 int *serial,
							 int max_depth,
							 bool show_branch,
							 bool show_serial,
							 MemoryContext per_query_ctx,
							 AttInMetadata *attinmeta,
							 Tuplestorestate *tupstore)
{
	TupleDesc	tupdesc = attinmeta->tupdesc;
	MemoryContext oldcontext;
	int			ret;
	int			proc;
	int			serial_column;
	StringInfoData sql;
	char	  **values;
	char	   *current_key;
	char	   *current_key_parent;
	char		current_level[INT32_STRLEN];
	char		serial_str[INT32_STRLEN];
	char	   *current_branch;
	HeapTuple	tuple;

	if (max_depth > 0 && level > max_depth)
		return tupstore;

	initStringInfo(&sql);

	/* Build initial sql statement */
	if (!show_serial)
	{
		appendStringInfo(&sql, "SELECT %s, %s FROM %s WHERE %s = %s AND %s IS NOT NULL AND %s <> %s",
						 key_fld,
						 parent_key_fld,
						 relname,
						 parent_key_fld,
						 quote_literal_cstr(start_with),
						 key_fld, key_fld, parent_key_fld);
		serial_column = 0;
	}
	else
	{
		appendStringInfo(&sql, "SELECT %s, %s FROM %s WHERE %s = %s AND %s IS NOT NULL AND %s <> %s ORDER BY %s",
						 key_fld,
						 parent_key_fld,
						 relname,
						 parent_key_fld,
						 quote_literal_cstr(start_with),
						 key_fld, key_fld, parent_key_fld,
						 orderby_fld);
		serial_column = 1;
	}

	if (show_branch)
		values = (char **) palloc((CONNECTBY_NCOLS + serial_column) * sizeof(char *));
	else
		values = (char **) palloc((CONNECTBY_NCOLS_NOBRANCH + serial_column) * sizeof(char *));

	/* First time through, do a little setup */
	if (level == 0)
	{
		/* root value is the one we initially start with */
		values[0] = start_with;

		/* root value has no parent */
		values[1] = NULL;

		/* root level is 0 */
		sprintf(current_level, "%d", level);
		values[2] = current_level;

		/* root branch is just starting root value */
		if (show_branch)
			values[3] = start_with;

		/* root starts the serial with 1 */
		if (show_serial)
		{
			sprintf(serial_str, "%d", (*serial)++);
			if (show_branch)
				values[4] = serial_str;
			else
				values[3] = serial_str;
		}

		/* construct the tuple */
		tuple = BuildTupleFromCStrings(attinmeta, values);

		/* switch to long lived context while storing the tuple */
		oldcontext = MemoryContextSwitchTo(per_query_ctx);

		/* now store it */
		tuplestore_puttuple(tupstore, tuple);

		/* now reset the context */
		MemoryContextSwitchTo(oldcontext);

		/* increment level */
		level++;
	}

	/* Retrieve the desired rows */
	ret = SPI_execute(sql.data, true, 0);
	proc = SPI_processed;

	/* Check for qualifying tuples */
	if ((ret == SPI_OK_SELECT) && (proc > 0))
	{
		HeapTuple	spi_tuple;
		SPITupleTable *tuptable = SPI_tuptable;
		TupleDesc	spi_tupdesc = tuptable->tupdesc;
		int			i;
		StringInfoData branchstr;
		StringInfoData chk_branchstr;
		StringInfoData chk_current_key;

		/* First time through, do a little more setup */
		if (level == 0)
		{
			/*
			 * Check that return tupdesc is compatible with the one we got
			 * from the query, but only at level 0 -- no need to check more
			 * than once
			 */

			if (!compatConnectbyTupleDescs(tupdesc, spi_tupdesc))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("invalid return type"),
						 errdetail("Return and SQL tuple descriptions are " \
								   "incompatible.")));
		}

		initStringInfo(&branchstr);
		initStringInfo(&chk_branchstr);
		initStringInfo(&chk_current_key);

		for (i = 0; i < proc; i++)
		{
			/* initialize branch for this pass */
			appendStringInfo(&branchstr, "%s", branch);
			appendStringInfo(&chk_branchstr, "%s%s%s", branch_delim, branch, branch_delim);

			/* get the next sql result tuple */
			spi_tuple = tuptable->vals[i];

			/* get the current key and parent */
			current_key = SPI_getvalue(spi_tuple, spi_tupdesc, 1);
			appendStringInfo(&chk_current_key, "%s%s%s", branch_delim, current_key, branch_delim);
			current_key_parent = pstrdup(SPI_getvalue(spi_tuple, spi_tupdesc, 2));

			/* get the current level */
			sprintf(current_level, "%d", level);

			/* check to see if this key is also an ancestor */
			if (strstr(chk_branchstr.data, chk_current_key.data))
				elog(ERROR, "infinite recursion detected");

			/* OK, extend the branch */
			appendStringInfo(&branchstr, "%s%s", branch_delim, current_key);
			current_branch = branchstr.data;

			/* build a tuple */
			values[0] = pstrdup(current_key);
			values[1] = current_key_parent;
			values[2] = current_level;
			if (show_branch)
				values[3] = current_branch;
			if (show_serial)
			{
				sprintf(serial_str, "%d", (*serial)++);
				if (show_branch)
					values[4] = serial_str;
				else
					values[3] = serial_str;
			}

			tuple = BuildTupleFromCStrings(attinmeta, values);

			xpfree(current_key);
			xpfree(current_key_parent);

			/* switch to long lived context while storing the tuple */
			oldcontext = MemoryContextSwitchTo(per_query_ctx);

			/* store the tuple for later use */
			tuplestore_puttuple(tupstore, tuple);

			/* now reset the context */
			MemoryContextSwitchTo(oldcontext);

			heap_freetuple(tuple);

			/* recurse using current_key_parent as the new start_with */
			tupstore = build_tuplestore_recursively(key_fld,
													parent_key_fld,
													relname,
													orderby_fld,
													branch_delim,
													values[0],
													current_branch,
													level + 1,
													serial,
													max_depth,
													show_branch,
													show_serial,
													per_query_ctx,
													attinmeta,
													tupstore);

			/* reset branch for next pass */
			resetStringInfo(&branchstr);
			resetStringInfo(&chk_branchstr);
			resetStringInfo(&chk_current_key);
		}

		xpfree(branchstr.data);
		xpfree(chk_branchstr.data);
		xpfree(chk_current_key.data);
	}

	return tupstore;
}

/*
 * Check expected (query runtime) tupdesc suitable for Connectby
 */
static void
validateConnectbyTupleDesc(TupleDesc tupdesc, bool show_branch, bool show_serial)
{
	int			serial_column = 0;

	if (show_serial)
		serial_column = 1;

	/* are there the correct number of columns */
	if (show_branch)
	{
		if (tupdesc->natts != (CONNECTBY_NCOLS + serial_column))
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("invalid return type"),
					 errdetail("Query-specified return tuple has " \
							   "wrong number of columns.")));
	}
	else
	{
		if (tupdesc->natts != CONNECTBY_NCOLS_NOBRANCH + serial_column)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("invalid return type"),
					 errdetail("Query-specified return tuple has " \
							   "wrong number of columns.")));
	}

	/* check that the types of the first two columns match */
	if (tupdesc->attrs[0]->atttypid != tupdesc->attrs[1]->atttypid)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("invalid return type"),
				 errdetail("First two columns must be the same type.")));

	/* check that the type of the third column is INT4 */
	if (tupdesc->attrs[2]->atttypid != INT4OID)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("invalid return type"),
				 errdetail("Third column must be type %s.",
						   format_type_be(INT4OID))));

	/* check that the type of the fourth column is TEXT if applicable */
	if (show_branch && tupdesc->attrs[3]->atttypid != TEXTOID)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("invalid return type"),
				 errdetail("Fourth column must be type %s.",
						   format_type_be(TEXTOID))));

	/* check that the type of the fifth column is INT4 */
	if (show_branch && show_serial && tupdesc->attrs[4]->atttypid != INT4OID)
		elog(ERROR, "query-specified return tuple not valid for Connectby: "
			 "fifth column must be type %s", format_type_be(INT4OID));

	/* check that the type of the fifth column is INT4 */
	if (!show_branch && show_serial && tupdesc->attrs[3]->atttypid != INT4OID)
		elog(ERROR, "query-specified return tuple not valid for Connectby: "
			 "fourth column must be type %s", format_type_be(INT4OID));

	/* OK, the tupdesc is valid for our purposes */
}

/*
 * Check if spi sql tupdesc and return tupdesc are compatible
 */
static bool
compatConnectbyTupleDescs(TupleDesc ret_tupdesc, TupleDesc sql_tupdesc)
{
	Oid			ret_atttypid;
	Oid			sql_atttypid;

	/* check the key_fld types match */
	ret_atttypid = ret_tupdesc->attrs[0]->atttypid;
	sql_atttypid = sql_tupdesc->attrs[0]->atttypid;
	if (ret_atttypid != sql_atttypid)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("invalid return type"),
				 errdetail("SQL key field datatype does " \
						   "not match return key field datatype.")));

	/* check the parent_key_fld types match */
	ret_atttypid = ret_tupdesc->attrs[1]->atttypid;
	sql_atttypid = sql_tupdesc->attrs[1]->atttypid;
	if (ret_atttypid != sql_atttypid)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("invalid return type"),
				 errdetail("SQL parent key field datatype does " \
						   "not match return parent key field datatype.")));

	/* OK, the two tupdescs are compatible for our purposes */
	return true;
}

/*
 * Check if two tupdescs match in type of attributes
 */
static bool
compatCrosstabTupleDescs(TupleDesc ret_tupdesc, TupleDesc sql_tupdesc)
{
	int			i;
	Form_pg_attribute ret_attr;
	Oid			ret_atttypid;
	Form_pg_attribute sql_attr;
	Oid			sql_atttypid;

	/* check the rowid types match */
	ret_atttypid = ret_tupdesc->attrs[0]->atttypid;
	sql_atttypid = sql_tupdesc->attrs[0]->atttypid;
	if (ret_atttypid != sql_atttypid)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("invalid return type"),
				 errdetail("SQL rowid datatype does not match " \
						   "return rowid datatype.")));

	/*
	 * - attribute [1] of the sql tuple is the category; no need to check it -
	 * attribute [2] of the sql tuple should match attributes [1] to [natts]
	 * of the return tuple
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

/*
 * Return a properly quoted literal value.
 * Uses quote_literal in quote.c
 */
static char *
quote_literal_cstr(char *rawstr)
{
	text	   *rawstr_text;
	text	   *result_text;
	char	   *result;

	rawstr_text = DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum(rawstr)));
	result_text = DatumGetTextP(DirectFunctionCall1(quote_literal, PointerGetDatum(rawstr_text)));
	result = DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(result_text)));

	return result;
}
