/*
 *	PostgreSQL type definitions for managed LargeObjects.
 *
 *	$Id: lo.c,v 1.1 1998/06/16 07:07:11 momjian Exp $
 *
 */

#include <stdio.h>

#include <postgres.h>
#include <utils/palloc.h>

/* Required for largeobjects */
#include <libpq/libpq-fs.h>
#include <libpq/be-fsstubs.h>

/* Required for SPI */
#include <executor/spi.h>

/* Required for triggers */
#include <commands/trigger.h>

/* required for tolower() */

/*
 *	This is the internal storage format for managed large objects
 *
 */

typedef Oid Blob;

/*
 *	Various forward declarations:
 */

Blob		   *lo_in(char *str);	/* Create from String		*/
char	   *lo_out(Blob * addr);	/* Output oid as String		*/
Oid			lo_oid(Blob * addr);	/* Return oid as an oid		*/
Blob		   *lo(Oid oid);		/* Return Blob based on oid	*/
HeapTuple	lo_manage(void);		/* Trigger handler			*/

/*
 * This creates a large object, and set's its OID to the value in the
 * supplied string.
 *
 * If the string is empty, then a new LargeObject is created, and its oid
 * is placed in the resulting lo.
 */
Blob *
lo_in(char *str)
{
  Blob *result;
  Oid oid;
  int			count;
  
  if (strlen(str) > 0)
	{
	  
	  count = sscanf(str, "%d", &oid);
	  
	  if (count < 1)
		{
		  elog(ERROR, "lo_in: error in parsing \"%s\"", str);
		  return (NULL);
		}
	  
	  if(oid < 0)
		{
		  elog(ERROR, "lo_in: illegal oid \"%s\"", str);
		  return (NULL);
		}
	}
  else
	{
	  /*
	   * There is no Oid passed, so create a new one
	   */
	  oid = lo_creat(INV_READ|INV_WRITE);
	  if(oid == InvalidOid)
		{
		  elog(ERROR,"lo_in: InvalidOid returned from lo_creat");
		  return (NULL);
		}
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
  sprintf(result,"%d",*addr);
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
  if(addr == NULL)
	return InvalidOid;
  return (Oid)(*addr);
}

/*
 * This function is used so we can convert oid's to lo's
 *
 * ie:  insert into table values(lo_import('/path/file')::lo);
 *
 */
Blob *
lo(Oid oid)
{
  Blob *result = (Blob *) palloc(sizeof(Blob));
  *result = oid;
  return (result);
}

/*
 * This handles the trigger that protects us from orphaned large objects
 */
HeapTuple
lo_manage(void)
{
  int attnum;					/* attribute number to monitor	*/
  char **args;					/* Args containing attr name	*/
  TupleDesc	tupdesc;			/* Tuple Descriptor				*/
  HeapTuple	rettuple;			/* Tuple to be returned			*/
  bool		isdelete;			/* are we deleting?				*/
  HeapTuple newtuple=NULL;		/* The new value for tuple		*/
  HeapTuple trigtuple;			/* The original value of tuple	*/
  
  if (!CurrentTriggerData)
	elog(ERROR, "lo: triggers are not initialized");
  
  /*
   * Fetch some values from CurrentTriggerData
   */
  newtuple	= CurrentTriggerData->tg_newtuple;
  trigtuple	= CurrentTriggerData->tg_trigtuple;
  tupdesc	= CurrentTriggerData->tg_relation->rd_att;
  args		= CurrentTriggerData->tg_trigger->tgargs;
  
  /* tuple to return to Executor */
  if (TRIGGER_FIRED_BY_UPDATE(CurrentTriggerData->tg_event))
	rettuple = newtuple;
  else
	rettuple = trigtuple;
  
  /* Are we deleting the row? */
  isdelete = TRIGGER_FIRED_BY_DELETE(CurrentTriggerData->tg_event);
  
  /* Were done with it */
  CurrentTriggerData = NULL;
  
  /* Get the column were interested in */
  attnum = SPI_fnumber(tupdesc,args[0]);
  
  /*
   * Handle updates
   *
   * Here, if the value of the monitored attribute changes, then the
   * large object associated with the original value is unlinked.
   */
  if(newtuple!=NULL) {
	char *orig = SPI_getvalue(trigtuple,tupdesc,attnum);
	char *newv = SPI_getvalue(newtuple,tupdesc,attnum);
	
	if((orig != newv && (orig==NULL || newv==NULL)) || (orig!=NULL && newv!=NULL && strcmp(orig,newv)))
	  lo_unlink(atoi(orig));
	
	if(newv)
	  pfree(newv);
	if(orig)
	  pfree(orig);
  }
  
  /*
   * Handle deleting of rows
   *
   * Here, we unlink the large object associated with the managed attribute
   *
   */
  if(isdelete) {
	char *orig = SPI_getvalue(trigtuple,tupdesc,attnum);
	
	if(orig != NULL) {
	  lo_unlink(atoi(orig));
	  
	  pfree(orig);
	}
  }
  
  return (rettuple);
}
