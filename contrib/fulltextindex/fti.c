#include "postgres.h"
#include "executor/spi.h"
#include "commands/trigger.h"
#include <ctype.h>				/* tolower */
#include <stdio.h>				/* debugging */

/*
 * Trigger function takes 2 arguments:
		1. relation in which to store the substrings
		2. field to extract substrings from

   The relation in which to insert *must* have the following layout:

		string		varchar(#)
		id			oid

	Example:

create function fti() returns opaque as
'/home/boekhold/src/postgresql-6.2/contrib/fti/fti.so' language 'newC';

create table title_fti (string varchar(25), id oid);
create index title_fti_idx on title_fti (string);

create trigger title_fti_trigger after update or insert or delete on product
for each row execute procedure fti(title_fti, title);
								   ^^^^^^^^^
								   where to store index in
											  ^^^^^
											  which column to index

ofcourse don't forget to create an index on title_idx, column string, else
you won't notice much speedup :)

After populating 'product', try something like:

select p.* from product p, title_fti f1, title_fti f2 where
	f1.string='slippery' and f2.string='wet' and f1.id=f2.id and p.oid=f1.id;
*/

/*
	march 4 1998 Changed breakup() to return less substrings. Only breakup
				 in word parts which are in turn shortened from the start
				 of the word (ie. word, ord, rd)
				 Did allocation of substring buffer outside of breakup()
	oct. 5 1997, fixed a bug in string breakup (where there are more nonalpha
				 characters between words then 1).

	oct 4-5 1997 implemented the thing, at least the basic functionallity
				 of it all....
*/

/* IMPROVEMENTS:

   save a plan for deletes
   create a function that will make the index *after* we have populated
   the main table (probably first delete all contents to be sure there's
   nothing in it, then re-populate the fti-table)

   can we do something with operator overloading or a seperate function
   that can build the final query automatigally?
   */

extern Datum	fti(PG_FUNCTION_ARGS);
static char	   *breakup(char *, char *);
static bool		is_stopword(char *);

static bool		new_tuple = false;


/* THIS LIST MUST BE IN SORTED ORDER, A BINARY SEARCH IS USED!!!! */
char	   *StopWords[] = {		/* list of words to skip in indexing */
#ifdef SAMPLE_STOP_WORDS
	"no"
	"the",
	"yes",
#endif
};

/* stuff for caching query-plans, stolen from contrib/spi/\*.c */
typedef struct
{
	char	   *ident;
	int			nplans;
	void	  **splan;
}			EPlan;

static EPlan *InsertPlans = NULL;
static EPlan *DeletePlans = NULL;
static int	nInsertPlans = 0;
static int	nDeletePlans = 0;

static EPlan *find_plan(char *ident, EPlan ** eplan, int *nplans);

/***********************************************************************/
Datum
fti(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
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
	char		query[8192];
	Oid			oid;

	/*
	 * FILE		 *debug;
	 */

	/*
	 * debug = fopen("/dev/xconsole", "w"); fprintf(debug, "FTI: entered
	 * function\n"); fflush(debug);
	 */

	if (!CALLED_AS_TRIGGER(fcinfo))
		elog(ERROR, "Full Text Indexing: not fired by trigger manager");
	if (TRIGGER_FIRED_FOR_STATEMENT(trigdata->tg_event))
		elog(ERROR, "Full Text Indexing: can't process STATEMENT events");
	if (TRIGGER_FIRED_BEFORE(trigdata->tg_event))
		elog(ERROR, "Full Text Indexing: must be fired AFTER event");

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
		elog(ERROR, "Full Text Indexing: SPI_connect failed, returned %d\n", ret);

	nargs = trigger->tgnargs;
	if (nargs != 2)
		elog(ERROR, "Full Text Indexing: trigger can only have 2 arguments");

	args = trigger->tgargs;
	indexname = args[0];
	tupdesc = rel->rd_att;		/* what the tuple looks like (?) */

	/* get oid of current tuple, needed by all, so place here */
	oid = rettuple->t_data->t_oid;
	if (!OidIsValid(oid))
		elog(ERROR, "Full Text Indexing: oid of current tuple is NULL");

	if (isdelete)
	{
		void	   *pplan;
		Oid		   *argtypes;
		Datum		values[1];
		EPlan	   *plan;

		sprintf(query, "D%s$%s", args[0], args[1]);
		plan = find_plan(query, &DeletePlans, &nDeletePlans);
		if (plan->nplans <= 0)
		{
			argtypes = (Oid *) palloc(sizeof(Oid));

			argtypes[0] = OIDOID;

			sprintf(query, "DELETE FROM %s WHERE id = $1", indexname);
			pplan = SPI_prepare(query, 1, argtypes);
			if (!pplan)
				elog(ERROR, "Full Text Indexing: SPI_prepare returned NULL "
					 "in delete");
			pplan = SPI_saveplan(pplan);
			if (pplan == NULL)
				elog(ERROR, "Full Text Indexing: SPI_saveplan returned NULL "
					 "in delete");

			plan->splan = (void **) malloc(sizeof(void *));
			*(plan->splan) = pplan;
			plan->nplans = 1;
		}

		values[0] = oid;

		ret = SPI_execp(*(plan->splan), values, NULL, 0);
		if (ret != SPI_OK_DELETE)
			elog(ERROR, "Full Text Indexing: error executing plan in delete");
	}

	if (isinsert)
	{
		char	   *substring,
				   *column;
		void	   *pplan;
		Oid		   *argtypes;
		Datum		values[2];
		int			colnum;
		struct varlena *data;
		EPlan	   *plan;

		sprintf(query, "I%s$%s", args[0], args[1]);
		plan = find_plan(query, &InsertPlans, &nInsertPlans);

		/* no plan yet, so allocate mem for argtypes */
		if (plan->nplans <= 0)
		{
			argtypes = (Oid *) palloc(2 * sizeof(Oid));

			argtypes[0] = VARCHAROID;	/* create table t_name (string
										 * varchar, */
			argtypes[1] = OIDOID;		/* id	  oid);    */

			/* prepare plan to gain speed */
			sprintf(query, "INSERT INTO %s (string, id) VALUES ($1, $2)",
					indexname);
			pplan = SPI_prepare(query, 2, argtypes);
			if (!pplan)
				elog(ERROR, "Full Text Indexing: SPI_prepare returned NULL "
					 "in insert");

			pplan = SPI_saveplan(pplan);
			if (pplan == NULL)
				elog(ERROR, "Full Text Indexing: SPI_saveplan returned NULL"
					 " in insert");

			plan->splan = (void **) malloc(sizeof(void *));
			*(plan->splan) = pplan;
			plan->nplans = 1;
		}


		/* prepare plan for query */
		colnum = SPI_fnumber(tupdesc, args[1]);
		if (colnum == SPI_ERROR_NOATTRIBUTE)
			elog(ERROR, "Full Text Indexing: column '%s' of '%s' not found",
				 args[1], args[0]);

		/* Get the char* representation of the column with name args[1] */
		column = SPI_getvalue(rettuple, tupdesc, colnum);

		if (column)
		{						/* make sure we don't try to index NULL's */
			char	   *buff;
			char	   *string = column;

			while (*string != '\0')
			{					/* placed 'really' inline. */
				*string = tolower(*string);		/* some compilers will
												 * choke */
				string++;		/* on 'inline' keyword */
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
					elog(ERROR, "Full Text Indexing: error executing plan "
						 "in insert");
			}
			pfree(buff);
			pfree(data);
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
		if (!isalnum((int) *last_start))
		{
			while (!isalnum((int) *last_start) &&
				   last_start > string)
				last_start--;
			cur_pos = last_start;
		}

		cur_pos--;				/* substrings are at minimum 2 characters
								 * long */

		if (isalnum((int) *cur_pos))
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
	char	  **StopLow;		/* for list of stop-words */
	char	  **StopHigh;
	char	  **StopMiddle;
	int			difference;

	StopLow = &StopWords[0];	/* initialize stuff for binary search */
	StopHigh = endof(StopWords);

	if (lengthof(StopWords) == 0)
		return false;

	while (StopLow <= StopHigh)
	{
		StopMiddle = StopLow + (StopHigh - StopLow) / 2;
		difference = strcmp(*StopMiddle, text);
		if (difference == 0)
			return (true);
		else if (difference < 0)
			StopLow = StopMiddle + 1;
		else
			StopHigh = StopMiddle - 1;
	}

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
