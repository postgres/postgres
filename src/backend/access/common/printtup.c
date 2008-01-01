/*-------------------------------------------------------------------------
 *
 * printtup.c
 *	  Routines to print out tuples to the destination (both frontend
 *	  clients and standalone backends are supported here).
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/common/printtup.c,v 1.101 2008/01/01 19:45:45 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/printtup.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "tcop/pquery.h"
#include "utils/lsyscache.h"


static void printtup_startup(DestReceiver *self, int operation,
				 TupleDesc typeinfo);
static void printtup(TupleTableSlot *slot, DestReceiver *self);
static void printtup_20(TupleTableSlot *slot, DestReceiver *self);
static void printtup_internal_20(TupleTableSlot *slot, DestReceiver *self);
static void printtup_shutdown(DestReceiver *self);
static void printtup_destroy(DestReceiver *self);


/* ----------------------------------------------------------------
 *		printtup / debugtup support
 * ----------------------------------------------------------------
 */

/* ----------------
 *		Private state for a printtup destination object
 *
 * NOTE: finfo is the lookup info for either typoutput or typsend, whichever
 * we are using for this column.
 * ----------------
 */
typedef struct
{								/* Per-attribute information */
	Oid			typoutput;		/* Oid for the type's text output fn */
	Oid			typsend;		/* Oid for the type's binary output fn */
	bool		typisvarlena;	/* is it varlena (ie possibly toastable)? */
	int16		format;			/* format code for this column */
	FmgrInfo	finfo;			/* Precomputed call info for output fn */
} PrinttupAttrInfo;

typedef struct
{
	DestReceiver pub;			/* publicly-known function pointers */
	Portal		portal;			/* the Portal we are printing from */
	bool		sendDescrip;	/* send RowDescription at startup? */
	TupleDesc	attrinfo;		/* The attr info we are set up for */
	int			nattrs;
	PrinttupAttrInfo *myinfo;	/* Cached info about each attr */
} DR_printtup;

/* ----------------
 *		Initialize: create a DestReceiver for printtup
 * ----------------
 */
DestReceiver *
printtup_create_DR(CommandDest dest, Portal portal)
{
	DR_printtup *self = (DR_printtup *) palloc(sizeof(DR_printtup));

	if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 3)
		self->pub.receiveSlot = printtup;
	else
	{
		/*
		 * In protocol 2.0 the Bind message does not exist, so there is no way
		 * for the columns to have different print formats; it's sufficient to
		 * look at the first one.
		 */
		if (portal->formats && portal->formats[0] != 0)
			self->pub.receiveSlot = printtup_internal_20;
		else
			self->pub.receiveSlot = printtup_20;
	}
	self->pub.rStartup = printtup_startup;
	self->pub.rShutdown = printtup_shutdown;
	self->pub.rDestroy = printtup_destroy;
	self->pub.mydest = dest;

	self->portal = portal;

	/*
	 * Send T message automatically if DestRemote, but not if
	 * DestRemoteExecute
	 */
	self->sendDescrip = (dest == DestRemote);

	self->attrinfo = NULL;
	self->nattrs = 0;
	self->myinfo = NULL;

	return (DestReceiver *) self;
}

static void
printtup_startup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	DR_printtup *myState = (DR_printtup *) self;
	Portal		portal = myState->portal;

	if (PG_PROTOCOL_MAJOR(FrontendProtocol) < 3)
	{
		/*
		 * Send portal name to frontend (obsolete cruft, gone in proto 3.0)
		 *
		 * If portal name not specified, use "blank" portal.
		 */
		const char *portalName = portal->name;

		if (portalName == NULL || portalName[0] == '\0')
			portalName = "blank";

		pq_puttextmessage('P', portalName);
	}

	/*
	 * If we are supposed to emit row descriptions, then send the tuple
	 * descriptor of the tuples.
	 */
	if (myState->sendDescrip)
		SendRowDescriptionMessage(typeinfo,
								  FetchPortalTargetList(portal),
								  portal->formats);

	/* ----------------
	 * We could set up the derived attr info at this time, but we postpone it
	 * until the first call of printtup, for 2 reasons:
	 * 1. We don't waste time (compared to the old way) if there are no
	 *	  tuples at all to output.
	 * 2. Checking in printtup allows us to handle the case that the tuples
	 *	  change type midway through (although this probably can't happen in
	 *	  the current executor).
	 * ----------------
	 */
}

/*
 * SendRowDescriptionMessage --- send a RowDescription message to the frontend
 *
 * Notes: the TupleDesc has typically been manufactured by ExecTypeFromTL()
 * or some similar function; it does not contain a full set of fields.
 * The targetlist will be NIL when executing a utility function that does
 * not have a plan.  If the targetlist isn't NIL then it is a Query node's
 * targetlist; it is up to us to ignore resjunk columns in it.	The formats[]
 * array pointer might be NULL (if we are doing Describe on a prepared stmt);
 * send zeroes for the format codes in that case.
 */
void
SendRowDescriptionMessage(TupleDesc typeinfo, List *targetlist, int16 *formats)
{
	Form_pg_attribute *attrs = typeinfo->attrs;
	int			natts = typeinfo->natts;
	int			proto = PG_PROTOCOL_MAJOR(FrontendProtocol);
	int			i;
	StringInfoData buf;
	ListCell   *tlist_item = list_head(targetlist);

	pq_beginmessage(&buf, 'T'); /* tuple descriptor message type */
	pq_sendint(&buf, natts, 2); /* # of attrs in tuples */

	for (i = 0; i < natts; ++i)
	{
		Oid			atttypid = attrs[i]->atttypid;
		int32		atttypmod = attrs[i]->atttypmod;

		pq_sendstring(&buf, NameStr(attrs[i]->attname));
		/* column ID info appears in protocol 3.0 and up */
		if (proto >= 3)
		{
			/* Do we have a non-resjunk tlist item? */
			while (tlist_item &&
				   ((TargetEntry *) lfirst(tlist_item))->resjunk)
				tlist_item = lnext(tlist_item);
			if (tlist_item)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(tlist_item);

				pq_sendint(&buf, tle->resorigtbl, 4);
				pq_sendint(&buf, tle->resorigcol, 2);
				tlist_item = lnext(tlist_item);
			}
			else
			{
				/* No info available, so send zeroes */
				pq_sendint(&buf, 0, 4);
				pq_sendint(&buf, 0, 2);
			}
		}
		/* If column is a domain, send the base type and typmod instead */
		atttypid = getBaseTypeAndTypmod(atttypid, &atttypmod);
		pq_sendint(&buf, (int) atttypid, sizeof(atttypid));
		pq_sendint(&buf, attrs[i]->attlen, sizeof(attrs[i]->attlen));
		/* typmod appears in protocol 2.0 and up */
		if (proto >= 2)
			pq_sendint(&buf, atttypmod, sizeof(atttypmod));
		/* format info appears in protocol 3.0 and up */
		if (proto >= 3)
		{
			if (formats)
				pq_sendint(&buf, formats[i], 2);
			else
				pq_sendint(&buf, 0, 2);
		}
	}
	pq_endmessage(&buf);
}

/*
 * Get the lookup info that printtup() needs
 */
static void
printtup_prepare_info(DR_printtup *myState, TupleDesc typeinfo, int numAttrs)
{
	int16	   *formats = myState->portal->formats;
	int			i;

	/* get rid of any old data */
	if (myState->myinfo)
		pfree(myState->myinfo);
	myState->myinfo = NULL;

	myState->attrinfo = typeinfo;
	myState->nattrs = numAttrs;
	if (numAttrs <= 0)
		return;

	myState->myinfo = (PrinttupAttrInfo *)
		palloc0(numAttrs * sizeof(PrinttupAttrInfo));

	for (i = 0; i < numAttrs; i++)
	{
		PrinttupAttrInfo *thisState = myState->myinfo + i;
		int16		format = (formats ? formats[i] : 0);

		thisState->format = format;
		if (format == 0)
		{
			getTypeOutputInfo(typeinfo->attrs[i]->atttypid,
							  &thisState->typoutput,
							  &thisState->typisvarlena);
			fmgr_info(thisState->typoutput, &thisState->finfo);
		}
		else if (format == 1)
		{
			getTypeBinaryOutputInfo(typeinfo->attrs[i]->atttypid,
									&thisState->typsend,
									&thisState->typisvarlena);
			fmgr_info(thisState->typsend, &thisState->finfo);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unsupported format code: %d", format)));
	}
}

/* ----------------
 *		printtup --- print a tuple in protocol 3.0
 * ----------------
 */
static void
printtup(TupleTableSlot *slot, DestReceiver *self)
{
	TupleDesc	typeinfo = slot->tts_tupleDescriptor;
	DR_printtup *myState = (DR_printtup *) self;
	StringInfoData buf;
	int			natts = typeinfo->natts;
	int			i;

	/* Set or update my derived attribute info, if needed */
	if (myState->attrinfo != typeinfo || myState->nattrs != natts)
		printtup_prepare_info(myState, typeinfo, natts);

	/* Make sure the tuple is fully deconstructed */
	slot_getallattrs(slot);

	/*
	 * Prepare a DataRow message
	 */
	pq_beginmessage(&buf, 'D');

	pq_sendint(&buf, natts, 2);

	/*
	 * send the attributes of this tuple
	 */
	for (i = 0; i < natts; ++i)
	{
		PrinttupAttrInfo *thisState = myState->myinfo + i;
		Datum		origattr = slot->tts_values[i],
					attr;

		if (slot->tts_isnull[i])
		{
			pq_sendint(&buf, -1, 4);
			continue;
		}

		/*
		 * If we have a toasted datum, forcibly detoast it here to avoid
		 * memory leakage inside the type's output routine.
		 */
		if (thisState->typisvarlena)
			attr = PointerGetDatum(PG_DETOAST_DATUM(origattr));
		else
			attr = origattr;

		if (thisState->format == 0)
		{
			/* Text output */
			char	   *outputstr;

			outputstr = OutputFunctionCall(&thisState->finfo, attr);
			pq_sendcountedtext(&buf, outputstr, strlen(outputstr), false);
			pfree(outputstr);
		}
		else
		{
			/* Binary output */
			bytea	   *outputbytes;

			outputbytes = SendFunctionCall(&thisState->finfo, attr);
			pq_sendint(&buf, VARSIZE(outputbytes) - VARHDRSZ, 4);
			pq_sendbytes(&buf, VARDATA(outputbytes),
						 VARSIZE(outputbytes) - VARHDRSZ);
			pfree(outputbytes);
		}

		/* Clean up detoasted copy, if any */
		if (attr != origattr)
			pfree(DatumGetPointer(attr));
	}

	pq_endmessage(&buf);
}

/* ----------------
 *		printtup_20 --- print a tuple in protocol 2.0
 * ----------------
 */
static void
printtup_20(TupleTableSlot *slot, DestReceiver *self)
{
	TupleDesc	typeinfo = slot->tts_tupleDescriptor;
	DR_printtup *myState = (DR_printtup *) self;
	StringInfoData buf;
	int			natts = typeinfo->natts;
	int			i,
				j,
				k;

	/* Set or update my derived attribute info, if needed */
	if (myState->attrinfo != typeinfo || myState->nattrs != natts)
		printtup_prepare_info(myState, typeinfo, natts);

	/* Make sure the tuple is fully deconstructed */
	slot_getallattrs(slot);

	/*
	 * tell the frontend to expect new tuple data (in ASCII style)
	 */
	pq_beginmessage(&buf, 'D');

	/*
	 * send a bitmap of which attributes are not null
	 */
	j = 0;
	k = 1 << 7;
	for (i = 0; i < natts; ++i)
	{
		if (!slot->tts_isnull[i])
			j |= k;				/* set bit if not null */
		k >>= 1;
		if (k == 0)				/* end of byte? */
		{
			pq_sendint(&buf, j, 1);
			j = 0;
			k = 1 << 7;
		}
	}
	if (k != (1 << 7))			/* flush last partial byte */
		pq_sendint(&buf, j, 1);

	/*
	 * send the attributes of this tuple
	 */
	for (i = 0; i < natts; ++i)
	{
		PrinttupAttrInfo *thisState = myState->myinfo + i;
		Datum		origattr = slot->tts_values[i],
					attr;
		char	   *outputstr;

		if (slot->tts_isnull[i])
			continue;

		Assert(thisState->format == 0);

		/*
		 * If we have a toasted datum, forcibly detoast it here to avoid
		 * memory leakage inside the type's output routine.
		 */
		if (thisState->typisvarlena)
			attr = PointerGetDatum(PG_DETOAST_DATUM(origattr));
		else
			attr = origattr;

		outputstr = OutputFunctionCall(&thisState->finfo, attr);
		pq_sendcountedtext(&buf, outputstr, strlen(outputstr), true);
		pfree(outputstr);

		/* Clean up detoasted copy, if any */
		if (attr != origattr)
			pfree(DatumGetPointer(attr));
	}

	pq_endmessage(&buf);
}

/* ----------------
 *		printtup_shutdown
 * ----------------
 */
static void
printtup_shutdown(DestReceiver *self)
{
	DR_printtup *myState = (DR_printtup *) self;

	if (myState->myinfo)
		pfree(myState->myinfo);
	myState->myinfo = NULL;

	myState->attrinfo = NULL;
}

/* ----------------
 *		printtup_destroy
 * ----------------
 */
static void
printtup_destroy(DestReceiver *self)
{
	pfree(self);
}

/* ----------------
 *		printatt
 * ----------------
 */
static void
printatt(unsigned attributeId,
		 Form_pg_attribute attributeP,
		 char *value)
{
	printf("\t%2d: %s%s%s%s\t(typeid = %u, len = %d, typmod = %d, byval = %c)\n",
		   attributeId,
		   NameStr(attributeP->attname),
		   value != NULL ? " = \"" : "",
		   value != NULL ? value : "",
		   value != NULL ? "\"" : "",
		   (unsigned int) (attributeP->atttypid),
		   attributeP->attlen,
		   attributeP->atttypmod,
		   attributeP->attbyval ? 't' : 'f');
}

/* ----------------
 *		debugStartup - prepare to print tuples for an interactive backend
 * ----------------
 */
void
debugStartup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	int			natts = typeinfo->natts;
	Form_pg_attribute *attinfo = typeinfo->attrs;
	int			i;

	/*
	 * show the return type of the tuples
	 */
	for (i = 0; i < natts; ++i)
		printatt((unsigned) i + 1, attinfo[i], NULL);
	printf("\t----\n");
}

/* ----------------
 *		debugtup - print one tuple for an interactive backend
 * ----------------
 */
void
debugtup(TupleTableSlot *slot, DestReceiver *self)
{
	TupleDesc	typeinfo = slot->tts_tupleDescriptor;
	int			natts = typeinfo->natts;
	int			i;
	Datum		origattr,
				attr;
	char	   *value;
	bool		isnull;
	Oid			typoutput;
	bool		typisvarlena;

	for (i = 0; i < natts; ++i)
	{
		origattr = slot_getattr(slot, i + 1, &isnull);
		if (isnull)
			continue;
		getTypeOutputInfo(typeinfo->attrs[i]->atttypid,
						  &typoutput, &typisvarlena);

		/*
		 * If we have a toasted datum, forcibly detoast it here to avoid
		 * memory leakage inside the type's output routine.
		 */
		if (typisvarlena)
			attr = PointerGetDatum(PG_DETOAST_DATUM(origattr));
		else
			attr = origattr;

		value = OidOutputFunctionCall(typoutput, attr);

		printatt((unsigned) i + 1, typeinfo->attrs[i], value);

		pfree(value);

		/* Clean up detoasted copy, if any */
		if (attr != origattr)
			pfree(DatumGetPointer(attr));
	}
	printf("\t----\n");
}

/* ----------------
 *		printtup_internal_20 --- print a binary tuple in protocol 2.0
 *
 * We use a different message type, i.e. 'B' instead of 'D' to
 * indicate a tuple in internal (binary) form.
 *
 * This is largely same as printtup_20, except we use binary formatting.
 * ----------------
 */
static void
printtup_internal_20(TupleTableSlot *slot, DestReceiver *self)
{
	TupleDesc	typeinfo = slot->tts_tupleDescriptor;
	DR_printtup *myState = (DR_printtup *) self;
	StringInfoData buf;
	int			natts = typeinfo->natts;
	int			i,
				j,
				k;

	/* Set or update my derived attribute info, if needed */
	if (myState->attrinfo != typeinfo || myState->nattrs != natts)
		printtup_prepare_info(myState, typeinfo, natts);

	/* Make sure the tuple is fully deconstructed */
	slot_getallattrs(slot);

	/*
	 * tell the frontend to expect new tuple data (in binary style)
	 */
	pq_beginmessage(&buf, 'B');

	/*
	 * send a bitmap of which attributes are not null
	 */
	j = 0;
	k = 1 << 7;
	for (i = 0; i < natts; ++i)
	{
		if (!slot->tts_isnull[i])
			j |= k;				/* set bit if not null */
		k >>= 1;
		if (k == 0)				/* end of byte? */
		{
			pq_sendint(&buf, j, 1);
			j = 0;
			k = 1 << 7;
		}
	}
	if (k != (1 << 7))			/* flush last partial byte */
		pq_sendint(&buf, j, 1);

	/*
	 * send the attributes of this tuple
	 */
	for (i = 0; i < natts; ++i)
	{
		PrinttupAttrInfo *thisState = myState->myinfo + i;
		Datum		origattr = slot->tts_values[i],
					attr;
		bytea	   *outputbytes;

		if (slot->tts_isnull[i])
			continue;

		Assert(thisState->format == 1);

		/*
		 * If we have a toasted datum, forcibly detoast it here to avoid
		 * memory leakage inside the type's output routine.
		 */
		if (thisState->typisvarlena)
			attr = PointerGetDatum(PG_DETOAST_DATUM(origattr));
		else
			attr = origattr;

		outputbytes = SendFunctionCall(&thisState->finfo, attr);
		/* We assume the result will not have been toasted */
		pq_sendint(&buf, VARSIZE(outputbytes) - VARHDRSZ, 4);
		pq_sendbytes(&buf, VARDATA(outputbytes),
					 VARSIZE(outputbytes) - VARHDRSZ);
		pfree(outputbytes);

		/* Clean up detoasted copy, if any */
		if (attr != origattr)
			pfree(DatumGetPointer(attr));
	}

	pq_endmessage(&buf);
}
