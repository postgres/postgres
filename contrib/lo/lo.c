/*
 *	PostgreSQL type definitions for managed LargeObjects.
 *
 *	$Header: /cvsroot/pgsql/contrib/lo/lo.c,v 1.13 2003/07/24 17:52:30 tgl Exp $
 *
 */

#include "postgres.h"

/* Required for largeobjects */
#include "libpq/libpq-fs.h"
#include "libpq/be-fsstubs.h"

/* Required for SPI */
#include "executor/spi.h"

/* Required for triggers */
#include "commands/trigger.h"


#define atooid(x)  ((Oid) strtoul((x), NULL, 10))


/*
 *	This is the internal storage format for managed large objects
 *
 */

typedef Oid Blob;

/*
 *	Various forward declarations:
 */

Blob	   *lo_in(char *str);	/* Create from String		*/
char	   *lo_out(Blob * addr);	/* Output oid as String		*/
Oid			lo_oid(Blob * addr);	/* Return oid as an oid		*/
Blob	   *lo(Oid oid);		/* Return Blob based on oid */
Datum		lo_manage(PG_FUNCTION_ARGS);		/* Trigger handler		   */

/*
 * This creates a large object, and sets its OID to the value in the
 * supplied string.
 *
 * If the string is empty, then a new LargeObject is created, and its oid
 * is placed in the resulting lo.
 */
Blob *
lo_in(char *str)
{
	Blob	   *result;
	Oid			oid;
	int			count;

	if (strlen(str) > 0)
	{
		count = sscanf(str, "%u", &oid);

		if (count < 1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("error in parsing \"%s\"", str)));

		if (oid == InvalidOid)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("illegal oid: \"%s\"", str)));
	}
	else
	{
		/*
		 * There is no Oid passed, so create a new one
		 */
		oid = DatumGetObjectId(DirectFunctionCall1(lo_creat,
								   Int32GetDatum(INV_READ | INV_WRITE)));
		if (oid == InvalidOid)
			/* internal error */
			elog(ERROR, "InvalidOid returned from lo_creat");
	}

	result = (Blob *) palloc(sizeof(Blob));

	*result = oid;

	return (result);
}

/*
 * This simply outputs the Oid of the Blob as a string.
 */
char *
lo_out(Blob * addr)
{
	char	   *result;

	if (addr == NULL)
		return (NULL);

	result = (char *) palloc(32);
	snprintf(result, 32, "%u", *addr);
	return (result);
}

/*
 * This function converts Blob to oid.
 *
 * eg: select lo_export(raster::oid,'/path/file') from table;
 *
 */
Oid
lo_oid(Blob * addr)
{
	if (addr == NULL)
		return InvalidOid;
	return (Oid) (*addr);
}

/*
 * This function is used so we can convert oid's to lo's
 *
 * ie:	insert into table values(lo_import('/path/file')::lo);
 *
 */
Blob *
lo(Oid oid)
{
	Blob	   *result = (Blob *) palloc(sizeof(Blob));

	*result = oid;
	return (result);
}

/*
 * This handles the trigger that protects us from orphaned large objects
 */
PG_FUNCTION_INFO_V1(lo_manage);

Datum
lo_manage(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	int			attnum;			/* attribute number to monitor	*/
	char	  **args;			/* Args containing attr name	*/
	TupleDesc	tupdesc;		/* Tuple Descriptor				*/
	HeapTuple	rettuple;		/* Tuple to be returned			*/
	bool		isdelete;		/* are we deleting?				*/
	HeapTuple	newtuple = NULL;	/* The new value for tuple		*/
	HeapTuple	trigtuple;		/* The original value of tuple	*/

	if (!CALLED_AS_TRIGGER(fcinfo))
		/* internal error */
		elog(ERROR, "not fired by trigger manager");

	/*
	 * Fetch some values from trigdata
	 */
	newtuple = trigdata->tg_newtuple;
	trigtuple = trigdata->tg_trigtuple;
	tupdesc = trigdata->tg_relation->rd_att;
	args = trigdata->tg_trigger->tgargs;

	/* tuple to return to Executor */
	if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		rettuple = newtuple;
	else
		rettuple = trigtuple;

	/* Are we deleting the row? */
	isdelete = TRIGGER_FIRED_BY_DELETE(trigdata->tg_event);

	/* Get the column were interested in */
	attnum = SPI_fnumber(tupdesc, args[0]);

	/*
	 * Handle updates
	 *
	 * Here, if the value of the monitored attribute changes, then the large
	 * object associated with the original value is unlinked.
	 */
	if (newtuple != NULL)
	{
		char	   *orig = SPI_getvalue(trigtuple, tupdesc, attnum);
		char	   *newv = SPI_getvalue(newtuple, tupdesc, attnum);

		if (orig != NULL && (newv == NULL || strcmp(orig, newv)))
			DirectFunctionCall1(lo_unlink,
								ObjectIdGetDatum(atooid(orig)));

		if (newv)
			pfree(newv);
		if (orig)
			pfree(orig);
	}

	/*
	 * Handle deleting of rows
	 *
	 * Here, we unlink the large object associated with the managed attribute
	 *
	 */
	if (isdelete)
	{
		char	   *orig = SPI_getvalue(trigtuple, tupdesc, attnum);

		if (orig != NULL)
		{
			DirectFunctionCall1(lo_unlink,
								ObjectIdGetDatum(atooid(orig)));

			pfree(orig);
		}
	}

	return PointerGetDatum(rettuple);
}
