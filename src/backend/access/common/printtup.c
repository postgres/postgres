/*-------------------------------------------------------------------------
 *
 * printtup.c
 *	  Routines to print out tuples to the destination (both frontend
 *	  clients and standalone backends are supported here).
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/common/printtup.c,v 1.71 2003/05/08 18:16:36 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/printtup.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "utils/lsyscache.h"
#include "utils/portal.h"


static void printtup_startup(DestReceiver *self, int operation,
							 TupleDesc typeinfo);
static void printtup(HeapTuple tuple, TupleDesc typeinfo,
					 DestReceiver *self);
static void printtup_20(HeapTuple tuple, TupleDesc typeinfo,
						DestReceiver *self);
static void printtup_internal_20(HeapTuple tuple, TupleDesc typeinfo,
								 DestReceiver *self);
static void printtup_shutdown(DestReceiver *self);
static void printtup_destroy(DestReceiver *self);


/* ----------------------------------------------------------------
 *		printtup / debugtup support
 * ----------------------------------------------------------------
 */

/* ----------------
 *		Private state for a printtup destination object
 * ----------------
 */
typedef struct
{								/* Per-attribute information */
	Oid			typoutput;		/* Oid for the attribute's type output fn */
	Oid			typelem;		/* typelem value to pass to the output fn */
	bool		typisvarlena;	/* is it varlena (ie possibly toastable)? */
	FmgrInfo	finfo;			/* Precomputed call info for typoutput */
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
		self->pub.receiveTuple = printtup;
	else
	{
		/*
		 * In protocol 2.0 the Bind message does not exist, so there is
		 * no way for the columns to have different print formats; it's
		 * sufficient to look at the first one.
		 */
		if (portal->formats && portal->formats[0] != 0)
			self->pub.receiveTuple = printtup_internal_20;
		else
			self->pub.receiveTuple = printtup_20;
	}
	self->pub.startup = printtup_startup;
	self->pub.shutdown = printtup_shutdown;
	self->pub.destroy = printtup_destroy;
	self->pub.mydest = dest;

	self->portal = portal;

	/* Send T message automatically if Remote, but not if RemoteExecute */
	self->sendDescrip = (dest == Remote);

	self->attrinfo = NULL;
	self->nattrs = 0;
	self->myinfo = NULL;

	return (DestReceiver *) self;
}

static void
printtup_startup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	DR_printtup *myState = (DR_printtup *) self;
	Portal	portal = myState->portal;

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
	 * If this is a retrieve, and we are supposed to emit row descriptions,
	 * then we send back the tuple descriptor of the tuples.  
	 */
	if (operation == CMD_SELECT && myState->sendDescrip)
	{
		List	   *targetlist;

		if (portal->strategy == PORTAL_ONE_SELECT)
			targetlist = ((Query *) lfirst(portal->parseTrees))->targetList;
		else
			targetlist = NIL;

		SendRowDescriptionMessage(typeinfo, targetlist, portal->formats);
	}

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
 * targetlist; it is up to us to ignore resjunk columns in it.  The formats[]
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

	pq_beginmessage(&buf, 'T');		/* tuple descriptor message type */
	pq_sendint(&buf, natts, 2);		/* # of attrs in tuples */

	for (i = 0; i < natts; ++i)
	{
		pq_sendstring(&buf, NameStr(attrs[i]->attname));
		/* column ID info appears in protocol 3.0 and up */
		if (proto >= 3)
		{
			/* Do we have a non-resjunk tlist item? */
			while (targetlist &&
				   ((TargetEntry *) lfirst(targetlist))->resdom->resjunk)
				targetlist = lnext(targetlist);
			if (targetlist)
			{
				Resdom	   *res = ((TargetEntry *) lfirst(targetlist))->resdom;

				pq_sendint(&buf, res->resorigtbl, 4);
				pq_sendint(&buf, res->resorigcol, 2);
				targetlist = lnext(targetlist);
			}
			else
			{
				/* No info available, so send zeroes */
				pq_sendint(&buf, 0, 4);
				pq_sendint(&buf, 0, 2);
			}
		}
		pq_sendint(&buf, (int) attrs[i]->atttypid,
				   sizeof(attrs[i]->atttypid));
		pq_sendint(&buf, attrs[i]->attlen,
				   sizeof(attrs[i]->attlen));
		/* typmod appears in protocol 2.0 and up */
		if (proto >= 2)
			pq_sendint(&buf, attrs[i]->atttypmod,
					   sizeof(attrs[i]->atttypmod));
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

static void
printtup_prepare_info(DR_printtup *myState, TupleDesc typeinfo, int numAttrs)
{
	int			i;

	if (myState->myinfo)
		pfree(myState->myinfo); /* get rid of any old data */
	myState->myinfo = NULL;
	myState->attrinfo = typeinfo;
	myState->nattrs = numAttrs;
	if (numAttrs <= 0)
		return;
	myState->myinfo = (PrinttupAttrInfo *)
		palloc(numAttrs * sizeof(PrinttupAttrInfo));
	for (i = 0; i < numAttrs; i++)
	{
		PrinttupAttrInfo *thisState = myState->myinfo + i;

		if (getTypeOutputInfo(typeinfo->attrs[i]->atttypid,
							  &thisState->typoutput, &thisState->typelem,
							  &thisState->typisvarlena))
			fmgr_info(thisState->typoutput, &thisState->finfo);
	}
}

/* ----------------
 *		printtup --- print a tuple in protocol 3.0
 * ----------------
 */
static void
printtup(HeapTuple tuple, TupleDesc typeinfo, DestReceiver *self)
{
	DR_printtup *myState = (DR_printtup *) self;
	int16	   *formats = myState->portal->formats;
	StringInfoData buf;
	int			natts = tuple->t_data->t_natts;
	int			i;

	/* Set or update my derived attribute info, if needed */
	if (myState->attrinfo != typeinfo || myState->nattrs != natts)
		printtup_prepare_info(myState, typeinfo, natts);

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
		int16		format = (formats ? formats[i] : 0);
		Datum		origattr,
					attr;
		bool		isnull;
		char	   *outputstr;

		origattr = heap_getattr(tuple, i + 1, typeinfo, &isnull);
		if (isnull)
		{
			pq_sendint(&buf, -1, 4);
			continue;
		}
		if (format == 0)
		{
			if (OidIsValid(thisState->typoutput))
			{
				/*
				 * If we have a toasted datum, forcibly detoast it here to
				 * avoid memory leakage inside the type's output routine.
				 */
				if (thisState->typisvarlena)
					attr = PointerGetDatum(PG_DETOAST_DATUM(origattr));
				else
					attr = origattr;

				outputstr = DatumGetCString(FunctionCall3(&thisState->finfo,
														  attr,
									ObjectIdGetDatum(thisState->typelem),
						  Int32GetDatum(typeinfo->attrs[i]->atttypmod)));

				pq_sendcountedtext(&buf, outputstr, strlen(outputstr), false);

				/* Clean up detoasted copy, if any */
				if (attr != origattr)
					pfree(DatumGetPointer(attr));
				pfree(outputstr);
			}
			else
			{
				outputstr = "<unprintable>";
				pq_sendcountedtext(&buf, outputstr, strlen(outputstr), false);
			}
		}
		else if (format == 1)
		{
			/* XXX something similar to above */
			elog(ERROR, "Binary transmission not implemented yet");
		}
		else
		{
			elog(ERROR, "Invalid format code %d", format);
		}
	}

	pq_endmessage(&buf);
}

/* ----------------
 *		printtup_20 --- print a tuple in protocol 2.0
 * ----------------
 */
static void
printtup_20(HeapTuple tuple, TupleDesc typeinfo, DestReceiver *self)
{
	DR_printtup *myState = (DR_printtup *) self;
	StringInfoData buf;
	int			natts = tuple->t_data->t_natts;
	int			i,
				j,
				k;

	/* Set or update my derived attribute info, if needed */
	if (myState->attrinfo != typeinfo || myState->nattrs != natts)
		printtup_prepare_info(myState, typeinfo, natts);

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
		if (!heap_attisnull(tuple, i + 1))
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
		Datum		origattr,
					attr;
		bool		isnull;
		char	   *outputstr;

		origattr = heap_getattr(tuple, i + 1, typeinfo, &isnull);
		if (isnull)
			continue;
		if (OidIsValid(thisState->typoutput))
		{
			/*
			 * If we have a toasted datum, forcibly detoast it here to
			 * avoid memory leakage inside the type's output routine.
			 */
			if (thisState->typisvarlena)
				attr = PointerGetDatum(PG_DETOAST_DATUM(origattr));
			else
				attr = origattr;

			outputstr = DatumGetCString(FunctionCall3(&thisState->finfo,
													  attr,
									ObjectIdGetDatum(thisState->typelem),
						  Int32GetDatum(typeinfo->attrs[i]->atttypmod)));

			pq_sendcountedtext(&buf, outputstr, strlen(outputstr), true);

			/* Clean up detoasted copy, if any */
			if (attr != origattr)
				pfree(DatumGetPointer(attr));
			pfree(outputstr);
		}
		else
		{
			outputstr = "<unprintable>";
			pq_sendcountedtext(&buf, outputstr, strlen(outputstr), true);
		}
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
		printatt((unsigned) i + 1, attinfo[i], (char *) NULL);
	printf("\t----\n");
}

/* ----------------
 *		debugtup - print one tuple for an interactive backend
 * ----------------
 */
void
debugtup(HeapTuple tuple, TupleDesc typeinfo, DestReceiver *self)
{
	int			natts = tuple->t_data->t_natts;
	int			i;
	Datum		origattr,
				attr;
	char	   *value;
	bool		isnull;
	Oid			typoutput,
				typelem;
	bool		typisvarlena;

	for (i = 0; i < natts; ++i)
	{
		origattr = heap_getattr(tuple, i + 1, typeinfo, &isnull);
		if (isnull)
			continue;
		if (getTypeOutputInfo(typeinfo->attrs[i]->atttypid,
							  &typoutput, &typelem, &typisvarlena))
		{
			/*
			 * If we have a toasted datum, forcibly detoast it here to
			 * avoid memory leakage inside the type's output routine.
			 */
			if (typisvarlena)
				attr = PointerGetDatum(PG_DETOAST_DATUM(origattr));
			else
				attr = origattr;

			value = DatumGetCString(OidFunctionCall3(typoutput,
													 attr,
											   ObjectIdGetDatum(typelem),
						  Int32GetDatum(typeinfo->attrs[i]->atttypmod)));

			printatt((unsigned) i + 1, typeinfo->attrs[i], value);

			/* Clean up detoasted copy, if any */
			if (attr != origattr)
				pfree(DatumGetPointer(attr));
			pfree(value);
		}
	}
	printf("\t----\n");
}

/* ----------------
 *		printtup_internal_20 --- print a binary tuple in protocol 2.0
 *
 * We use a different message type, i.e. 'B' instead of 'D' to
 * indicate a tuple in internal (binary) form.
 *
 * This is largely same as printtup_20, except we don't use the typout func.
 * ----------------
 */
static void
printtup_internal_20(HeapTuple tuple, TupleDesc typeinfo, DestReceiver *self)
{
	DR_printtup *myState = (DR_printtup *) self;
	StringInfoData buf;
	int			natts = tuple->t_data->t_natts;
	int			i,
				j,
				k;

	/* Set or update my derived attribute info, if needed */
	if (myState->attrinfo != typeinfo || myState->nattrs != natts)
		printtup_prepare_info(myState, typeinfo, natts);

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
		if (!heap_attisnull(tuple, i + 1))
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
#ifdef IPORTAL_DEBUG
	fprintf(stderr, "sending tuple with %d atts\n", natts);
#endif

	for (i = 0; i < natts; ++i)
	{
		PrinttupAttrInfo *thisState = myState->myinfo + i;
		Datum		origattr,
					attr;
		bool		isnull;
		int32		len;

		origattr = heap_getattr(tuple, i + 1, typeinfo, &isnull);
		if (isnull)
			continue;
		/* send # of bytes, and opaque data */
		if (thisState->typisvarlena)
		{
			/*
			 * If we have a toasted datum, must detoast before sending.
			 */
			attr = PointerGetDatum(PG_DETOAST_DATUM(origattr));

			len = VARSIZE(attr) - VARHDRSZ;

			pq_sendint(&buf, len, VARHDRSZ);
			pq_sendbytes(&buf, VARDATA(attr), len);

#ifdef IPORTAL_DEBUG
			{
				char	   *d = VARDATA(attr);

				fprintf(stderr, "length %d data %x %x %x %x\n",
						len, *d, *(d + 1), *(d + 2), *(d + 3));
			}
#endif

			/* Clean up detoasted copy, if any */
			if (attr != origattr)
				pfree(DatumGetPointer(attr));
		}
		else
		{
			/* fixed size or cstring */
			attr = origattr;
			len = typeinfo->attrs[i]->attlen;
			if (len <= 0)
			{
				/* it's a cstring */
				Assert(len == -2 && !typeinfo->attrs[i]->attbyval);
				len = strlen(DatumGetCString(attr)) + 1;
			}
			pq_sendint(&buf, len, sizeof(int32));
			if (typeinfo->attrs[i]->attbyval)
			{
				Datum		datumBuf;

				/*
				 * We need this horsing around because we don't know how
				 * shorter data values are aligned within a Datum.
				 */
				store_att_byval(&datumBuf, attr, len);
				pq_sendbytes(&buf, (char *) &datumBuf, len);
#ifdef IPORTAL_DEBUG
				fprintf(stderr, "byval length %d data %ld\n", len,
						(long) attr);
#endif
			}
			else
			{
				pq_sendbytes(&buf, DatumGetPointer(attr), len);
#ifdef IPORTAL_DEBUG
				fprintf(stderr, "byref length %d data %p\n", len,
						DatumGetPointer(attr));
#endif
			}
		}
	}

	pq_endmessage(&buf);
}
