#include "postgres.h"

#include <ctype.h>

#include "executor/spi.h"
#include "commands/trigger.h"

/*
 *	Trigger function accepts variable number of arguments:
 *
 *		1. relation in which to store the substrings
 *		2. fields to extract substrings from
 *
 *	The relation in which to insert *must* have the following layout:
 *
 *		string		varchar(#)
 *		id			oid
 *
 *	 where # is the largest size of the varchar columns being indexed
 *
 *	Example:
 *
 *	-- Create the SQL function based on the compiled shared object
 *	create function fti() returns trigger as
 *	  '/usr/local/pgsql/lib/contrib/fti.so' language 'C';
 *
 *	-- Create the FTI table
 *	create table product_fti (string varchar(255), id oid) without oids;
 *
 *	-- Create an index to assist string matches
 *	create index product_fti_string_idx on product_fti (string);
 *
 *	-- Create an index to assist trigger'd deletes
 *	create index product_fti_id_idx on product_fti (id);
 *
 *	-- Create an index on the product oid column to assist joins
 *	-- between the fti table and the product table
 *	create index product_oid_idx on product (oid);
 *
 *	-- Create the trigger to perform incremental changes to the full text index.
 *	create trigger product_fti_trig after update or insert or delete on product
 *	for each row execute procedure fti(product_fti, title, artist);
 *									   ^^^^^^^^^^^
 *									   table where full text index is stored
 *													^^^^^^^^^^^^^
 *													columns to index in the base table
 *
 *	After populating 'product', try something like:
 *
 *	SELECT DISTINCT(p.*) FROM product p, product_fti f1, product_fti f2 WHERE
 *	f1.string ~ '^slippery' AND f2.string ~ '^wet' AND p.oid=f1.id AND p.oid=f2.id;
 *
 *	To check that your indicies are being used correctly, make sure you
 *	EXPLAIN SELECT ... your test query above.
 *
 * CHANGELOG
 * ---------
 *
 *	august 3 2001
 *				 Extended fti function to accept more than one column as a
 *				 parameter and all specified columns are indexed.  Changed
 *				 all uses of sprintf to snprintf.  Made error messages more
 *				 consistent.
 *
 *	march 4 1998 Changed breakup() to return less substrings. Only breakup
 *				 in word parts which are in turn shortened from the start
 *				 of the word (ie. word, ord, rd)
 *				 Did allocation of substring buffer outside of breakup()
 *
 *	oct. 5 1997, fixed a bug in string breakup (where there are more nonalpha
 *				 characters between words then 1).
 *
 *	oct 4-5 1997 implemented the thing, at least the basic functionallity
 *				 of it all....
 *
 * TODO
 * ----
 *
 *	 prevent generating duplicate words for an oid in the fti table
 *	 save a plan for deletes
 *	 create a function that will make the index *after* we have populated
 *	 the main table (probably first delete all contents to be sure there's
 *	 nothing in it, then re-populate the fti-table)
 *
 *	 can we do something with operator overloading or a seperate function
 *	 that can build the final query automagically?
 */

#define MAX_FTI_QUERY_LENGTH 8192

extern Datum fti(PG_FUNCTION_ARGS);
static char *breakup(char *, char *);
static bool is_stopword(char *);

static bool new_tuple = false;


#ifdef USE_STOP_WORDS

/* THIS LIST MUST BE IN SORTED ORDER, A BINARY SEARCH IS USED!!!! */
char	   *StopWords[] = {		/* list of words to skip in indexing */
	"no",
	"the",
	"yes"
};
#endif   /* USE_STOP_WORDS */

/* stuff for caching query-plans, stolen from contrib/spi/\*.c */
typedef struct
{
	char	   *ident;
	int			nplans;
	void	  **splan;
}	EPlan;

static EPlan *InsertPlans = NULL;
static EPlan *DeletePlans = NULL;
static int	nInsertPlans = 0;
static int	nDeletePlans = 0;

static EPlan *find_plan(char *ident, EPlan ** eplan, int *nplans);

/***********************************************************************/
PG_FUNCTION_INFO_V1(fti);

Datum
fti(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata;
	Trigger    *trigger;		/* to get trigger name */
	int			nargs;			/* # of arguments */
	char	  **args;			/* arguments */
	char	   *relname;		/* triggered relation name */
	Relation	rel;			/* triggered relation */
	char	   *indexname;		/* name of table for substrings */
	HeapTuple	rettuple = NULL;
	TupleDesc	tupdesc;		/* tuple description */
	bool		isinsert = false;
	bool		isdelete = false;
	int			ret;
	char		query[MAX_FTI_QUERY_LENGTH];
	Oid			oid;

	/*
	 * FILE		 *debug;
	 */

	/*
	 * debug = fopen("/dev/xconsole", "w"); fprintf(debug, "FTI: entered
	 * function\n"); fflush(debug);
	 */

	if (!CALLED_AS_TRIGGER(fcinfo))
		/* internal error */
		elog(ERROR, "not fired by trigger manager");

	/* It's safe to cast now that we've checked */
	trigdata = (TriggerData *) fcinfo->context;

	if (TRIGGER_FIRED_FOR_STATEMENT(trigdata->tg_event))
		ereport(ERROR,
				(errcode(ERRCODE_TRIGGERED_ACTION_EXCEPTION),
				 errmsg("can't process STATEMENT events")));

	if (TRIGGER_FIRED_BEFORE(trigdata->tg_event))
		ereport(ERROR,
				(errcode(ERRCODE_TRIGGERED_ACTION_EXCEPTION),
				 errmsg("must be fired AFTER event")));

	if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
		isinsert = true;
	if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
	{
		isdelete = true;
		isinsert = true;
	}
	if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
		isdelete = true;

	trigger = trigdata->tg_trigger;
	rel = trigdata->tg_relation;
	relname = SPI_getrelname(rel);
	rettuple = trigdata->tg_trigtuple;
	if (isdelete && isinsert)	/* is an UPDATE */
		rettuple = trigdata->tg_newtuple;

	if ((ret = SPI_connect()) < 0)
		/* internal error */
		elog(ERROR, "SPI_connect failed, returned %d", ret);

	nargs = trigger->tgnargs;
	if (nargs < 2)
		ereport(ERROR,
				(errcode(ERRCODE_TRIGGERED_ACTION_EXCEPTION),
				 errmsg("fti trigger must have at least 2 arguments")));

	args = trigger->tgargs;
	indexname = args[0];
	tupdesc = rel->rd_att;		/* what the tuple looks like (?) */

	/* get oid of current tuple, needed by all, so place here */
	oid = HeapTupleGetOid(rettuple);
	if (!OidIsValid(oid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("OID is not present"),
				 errhint("Full Text Index requires indexed tables be created WITH OIDS.")));

	if (isdelete)
	{
		void	   *pplan;
		Oid		   *argtypes;
		Datum		values[1];
		EPlan	   *plan;
		int			i;

		snprintf(query, MAX_FTI_QUERY_LENGTH, "D%s", indexname);
		for (i = 1; i < nargs; i++)
			snprintf(query, MAX_FTI_QUERY_LENGTH, "%s$%s", query, args[i]);

		plan = find_plan(query, &DeletePlans, &nDeletePlans);
		if (plan->nplans <= 0)
		{
			argtypes = (Oid *) palloc(sizeof(Oid));

			argtypes[0] = OIDOID;

			snprintf(query, MAX_FTI_QUERY_LENGTH, "DELETE FROM %s WHERE id = $1", indexname);
			pplan = SPI_prepare(query, 1, argtypes);
			if (!pplan)
				/* internal error */
				elog(ERROR, "SPI_prepare returned NULL in delete");
			pplan = SPI_saveplan(pplan);
			if (pplan == NULL)
				/* internal error */
				elog(ERROR, "SPI_saveplan returned NULL in delete");

			plan->splan = (void **) malloc(sizeof(void *));
			*(plan->splan) = pplan;
			plan->nplans = 1;
		}

		values[0] = oid;

		ret = SPI_execp(*(plan->splan), values, NULL, 0);
		if (ret != SPI_OK_DELETE)
			ereport(ERROR,
					(errcode(ERRCODE_TRIGGERED_ACTION_EXCEPTION),
					 errmsg("error executing delete")));
	}

	if (isinsert)
	{
		char	   *substring;
		char	   *column;
		void	   *pplan;
		Oid		   *argtypes;
		Datum		values[2];
		int			colnum;
		struct varlena *data;
		EPlan	   *plan;
		int			i;
		char	   *buff;
		char	   *string;

		snprintf(query, MAX_FTI_QUERY_LENGTH, "I%s", indexname);
		for (i = 1; i < nargs; i++)
			snprintf(query, MAX_FTI_QUERY_LENGTH, "%s$%s", query, args[i]);

		plan = find_plan(query, &InsertPlans, &nInsertPlans);

		/* no plan yet, so allocate mem for argtypes */
		if (plan->nplans <= 0)
		{
			argtypes = (Oid *) palloc(2 * sizeof(Oid));

			argtypes[0] = VARCHAROID;	/* create table t_name (string
										 * varchar, */
			argtypes[1] = OIDOID;		/* id	  oid);    */

			/* prepare plan to gain speed */
			snprintf(query, MAX_FTI_QUERY_LENGTH, "INSERT INTO %s (string, id) VALUES ($1, $2)",
					 indexname);
			pplan = SPI_prepare(query, 2, argtypes);
			if (!pplan)
				/* internal error */
				elog(ERROR, "SPI_prepare returned NULL in insert");

			pplan = SPI_saveplan(pplan);
			if (pplan == NULL)
				/* internal error */
				elog(ERROR, "SPI_saveplan returned NULL in insert");

			plan->splan = (void **) malloc(sizeof(void *));
			*(plan->splan) = pplan;
			plan->nplans = 1;
		}

		/* prepare plan for query */
		for (i = 0; i < nargs - 1; i++)
		{
			colnum = SPI_fnumber(tupdesc, args[i + 1]);
			if (colnum == SPI_ERROR_NOATTRIBUTE)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("column \"%s\" of \"%s\" does not exist",
								args[i + 1], indexname)));

			/* Get the char* representation of the column */
			column = SPI_getvalue(rettuple, tupdesc, colnum);

			/* make sure we don't try to index NULL's */
			if (column)
			{
				string = column;
				while (*string != '\0')
				{
					*string = tolower((unsigned char) *string);
					string++;
				}

				data = (struct varlena *) palloc(sizeof(int32) + strlen(column) +1);
				buff = palloc(strlen(column) + 1);
				/* saves lots of calls in while-loop and in breakup() */

				new_tuple = true;

				while ((substring = breakup(column, buff)))
				{
					int			l;

					l = strlen(substring);

					data->vl_len = l + sizeof(int32);
					memcpy(VARDATA(data), substring, l);
					values[0] = PointerGetDatum(data);
					values[1] = oid;

					ret = SPI_execp(*(plan->splan), values, NULL, 0);
					if (ret != SPI_OK_INSERT)
						ereport(ERROR,
							(errcode(ERRCODE_TRIGGERED_ACTION_EXCEPTION),
							 errmsg("error executing insert")));
				}
				pfree(buff);
				pfree(data);
			}
		}
	}

	SPI_finish();
	return PointerGetDatum(rettuple);
}

static char *
breakup(char *string, char *substring)
{
	static char *last_start;
	static char *cur_pos;

	if (new_tuple)
	{
		cur_pos = last_start = &string[strlen(string) - 1];
		new_tuple = false;		/* don't initialize this next time */
	}

	while (cur_pos > string)	/* don't read before start of 'string' */
	{
		/*
		 * skip pieces at the end of a string that are not alfa-numeric
		 * (ie. 'string$%^&', last_start first points to '&', and after
		 * this to 'g'
		 */
		if (!isalnum((unsigned char) *last_start))
		{
			while (!isalnum((unsigned char) *last_start) &&
				   last_start > string)
				last_start--;
			cur_pos = last_start;
		}

		cur_pos--;				/* substrings are at minimum 2 characters
								 * long */

		if (isalnum((unsigned char) *cur_pos))
		{
			/* Houston, we have a substring! :) */
			memcpy(substring, cur_pos, last_start - cur_pos + 1);
			substring[last_start - cur_pos + 1] = '\0';
			if (!is_stopword(substring))
				return substring;
		}
		else
		{
			last_start = cur_pos - 1;
			cur_pos = last_start;
		}
	}

	return NULL;				/* we've processed all of 'string' */
}

/* copied from src/backend/parser/keywords.c and adjusted for our situation*/
static bool
is_stopword(char *text)
{
#ifdef USE_STOP_WORDS
	char	  **StopLow;		/* for list of stop-words */
	char	  **StopHigh;
	char	  **StopMiddle;
	int			difference;

	StopLow = &StopWords[0];	/* initialize stuff for binary search */
	StopHigh = endof(StopWords);

	/* Loop invariant: *StopLow <= text < *StopHigh */

	while (StopLow < StopHigh)
	{
		StopMiddle = StopLow + (StopHigh - StopLow) / 2;
		difference = strcmp(*StopMiddle, text);
		if (difference == 0)
			return (true);
		else if (difference < 0)
			StopLow = StopMiddle + 1;
		else
			StopHigh = StopMiddle;
	}
#endif   /* USE_STOP_WORDS */

	return (false);
}

/* for caching of query plans, stolen from contrib/spi/\*.c */
static EPlan *
find_plan(char *ident, EPlan ** eplan, int *nplans)
{
	EPlan	   *newp;
	int			i;

	if (*nplans > 0)
	{
		for (i = 0; i < *nplans; i++)
		{
			if (strcmp((*eplan)[i].ident, ident) == 0)
				break;
		}
		if (i != *nplans)
			return (*eplan + i);
		*eplan = (EPlan *) realloc(*eplan, (i + 1) * sizeof(EPlan));
		newp = *eplan + i;
	}
	else
	{
		newp = *eplan = (EPlan *) malloc(sizeof(EPlan));
		(*nplans) = i = 0;
	}

	newp->ident = (char *) malloc(strlen(ident) + 1);
	strcpy(newp->ident, ident);
	newp->nplans = 0;
	newp->splan = NULL;
	(*nplans)++;

	return (newp);
}
