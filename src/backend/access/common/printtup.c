/*-------------------------------------------------------------------------
 *
 * printtup.c
 *	  Routines to print out tuples to the destination (binary or non-binary
 *	  portals, frontend/interactive backend, etc.).
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/common/printtup.c,v 1.44 1999/04/25 19:27:43 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <string.h>

#include "postgres.h"

#include "fmgr.h"
#include "access/heapam.h"
#include "access/printtup.h"
#include "catalog/pg_type.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "utils/syscache.h"

static void printtup_setup(DestReceiver* self, TupleDesc typeinfo);
static void printtup(HeapTuple tuple, TupleDesc typeinfo, DestReceiver* self);
static void printtup_cleanup(DestReceiver* self);

/* ----------------------------------------------------------------
 *		printtup / debugtup support
 * ----------------------------------------------------------------
 */

/* ----------------
 *		getTypeOutAndElem -- get both typoutput and typelem for a type
 *
 * We used to fetch these with two separate function calls,
 * typtoout() and gettypelem(), which each called SearchSysCacheTuple.
 * This way takes half the time.
 * ----------------
 */
int
getTypeOutAndElem(Oid type, Oid* typOutput, Oid* typElem)
{
	HeapTuple	typeTuple;

	typeTuple = SearchSysCacheTuple(TYPOID,
									ObjectIdGetDatum(type),
									0, 0, 0);

	if (HeapTupleIsValid(typeTuple))
	{
		Form_pg_type pt = (Form_pg_type) GETSTRUCT(typeTuple);
		*typOutput = (Oid) pt->typoutput;
		*typElem = (Oid) pt->typelem;
		return OidIsValid(*typOutput);
	}

	elog(ERROR, "getTypeOutAndElem: Cache lookup of type %d failed", type);

	*typOutput = InvalidOid;
	*typElem = InvalidOid;
	return 0;
}

/* ----------------
 *		Private state for a printtup destination object
 * ----------------
 */
typedef struct {				/* Per-attribute information */
	Oid			typoutput;		/* Oid for the attribute's type output fn */
	Oid			typelem;		/* typelem value to pass to the output fn */
	FmgrInfo	finfo;			/* Precomputed call info for typoutput */
} PrinttupAttrInfo;

typedef struct {
	DestReceiver		pub;		/* publicly-known function pointers */
	TupleDesc			attrinfo;	/* The attr info we are set up for */
	int					nattrs;
	PrinttupAttrInfo   *myinfo;		/* Cached info about each attr */
} DR_printtup;

/* ----------------
 *		Initialize: create a DestReceiver for printtup
 * ----------------
 */
DestReceiver*
printtup_create_DR()
{
	DR_printtup* self = (DR_printtup*) palloc(sizeof(DR_printtup));

	self->pub.receiveTuple = printtup;
	self->pub.setup = printtup_setup;
	self->pub.cleanup = printtup_cleanup;

	self->attrinfo = NULL;
	self->nattrs = 0;
	self->myinfo = NULL;

	return (DestReceiver*) self;
}

static void
printtup_setup(DestReceiver* self, TupleDesc typeinfo)
{
	/* ----------------
	 * We could set up the derived attr info at this time, but we postpone it
	 * until the first call of printtup, for 3 reasons:
	 * 1. We don't waste time (compared to the old way) if there are no
	 *    tuples at all to output.
	 * 2. Checking in printtup allows us to handle the case that the tuples
	 *    change type midway through (although this probably can't happen in
	 *    the current executor).
	 * 3. Right now, ExecutorRun passes a NULL for typeinfo anyway :-(
	 * ----------------
	 */
}

static void
printtup_prepare_info(DR_printtup* myState, TupleDesc typeinfo, int numAttrs)
{
	int i;

	if (myState->myinfo)
		pfree(myState->myinfo);	/* get rid of any old data */
	myState->myinfo = NULL;
	myState->attrinfo = typeinfo;
	myState->nattrs = numAttrs;
	if (numAttrs <= 0)
		return;
	myState->myinfo = (PrinttupAttrInfo*)
		palloc(numAttrs * sizeof(PrinttupAttrInfo));
	for (i = 0; i < numAttrs; i++)
	{
		PrinttupAttrInfo* thisState = myState->myinfo + i;
		if (getTypeOutAndElem((Oid) typeinfo->attrs[i]->atttypid,
							  &thisState->typoutput, &thisState->typelem))
			fmgr_info(thisState->typoutput, &thisState->finfo);
	}
}

/* ----------------
 *		printtup
 * ----------------
 */
static void
printtup(HeapTuple tuple, TupleDesc typeinfo, DestReceiver* self)
{
	DR_printtup *myState = (DR_printtup*) self;
	StringInfoData buf;
	int			i,
				j,
				k;
	char	   *outputstr;
	Datum		attr;
	bool		isnull;

	/* Set or update my derived attribute info, if needed */
	if (myState->attrinfo != typeinfo ||
		myState->nattrs != tuple->t_data->t_natts)
		printtup_prepare_info(myState, typeinfo, tuple->t_data->t_natts);

	/* ----------------
	 *	tell the frontend to expect new tuple data (in ASCII style)
	 * ----------------
	 */
	pq_beginmessage(&buf);
	pq_sendbyte(&buf, 'D');

	/* ----------------
	 *	send a bitmap of which attributes are not null
	 * ----------------
	 */
	j = 0;
	k = 1 << 7;
	for (i = 0; i < tuple->t_data->t_natts; ++i)
	{
		if (! heap_attisnull(tuple, i + 1))
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

	/* ----------------
	 *	send the attributes of this tuple
	 * ----------------
	 */
	for (i = 0; i < tuple->t_data->t_natts; ++i)
	{
		PrinttupAttrInfo* thisState = myState->myinfo + i;
		attr = heap_getattr(tuple, i + 1, typeinfo, &isnull);
		if (isnull)
			continue;
		if (OidIsValid(thisState->typoutput))
		{
			outputstr = (char *) (*fmgr_faddr(&thisState->finfo))
				(attr, thisState->typelem, typeinfo->attrs[i]->atttypmod);
			pq_sendcountedtext(&buf, outputstr, strlen(outputstr));
			pfree(outputstr);
		}
		else
		{
			outputstr = "<unprintable>";
			pq_sendcountedtext(&buf, outputstr, strlen(outputstr));
		}
	}

	pq_endmessage(&buf);
}

/* ----------------
 *		printtup_cleanup
 * ----------------
 */
static void
printtup_cleanup(DestReceiver* self)
{
	DR_printtup* myState = (DR_printtup*) self;
	if (myState->myinfo)
		pfree(myState->myinfo);
	pfree(myState);
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
		   attributeP->attname.data,
		   value != NULL ? " = \"" : "",
		   value != NULL ? value : "",
		   value != NULL ? "\"" : "",
		   (unsigned int) (attributeP->atttypid),
		   attributeP->attlen,
		   attributeP->atttypmod,
		   attributeP->attbyval ? 't' : 'f');
}

/* ----------------
 *		showatts
 * ----------------
 */
void
showatts(char *name, TupleDesc tupleDesc)
{
	int			i;
	int			natts = tupleDesc->natts;
	Form_pg_attribute *attinfo = tupleDesc->attrs;

	puts(name);
	for (i = 0; i < natts; ++i)
		printatt((unsigned) i + 1, attinfo[i], (char *) NULL);
	printf("\t----\n");
}

/* ----------------
 *		debugtup
 * ----------------
 */
void
debugtup(HeapTuple tuple, TupleDesc typeinfo, DestReceiver* self)
{
	int			i;
	Datum		attr;
	char	   *value;
	bool		isnull;
	Oid			typoutput,
				typelem;

	for (i = 0; i < tuple->t_data->t_natts; ++i)
	{
		attr = heap_getattr(tuple, i + 1, typeinfo, &isnull);
		if (isnull)
			continue;
		if (getTypeOutAndElem((Oid) typeinfo->attrs[i]->atttypid,
							  &typoutput, &typelem))
		{
			value = fmgr(typoutput, attr, typelem,
						 typeinfo->attrs[i]->atttypmod);
			printatt((unsigned) i + 1, typeinfo->attrs[i], value);
			pfree(value);
		}
	}
	printf("\t----\n");
}

/* ----------------
 *		printtup_internal
 *		We use a different data prefix, e.g. 'B' instead of 'D' to
 *		indicate a tuple in internal (binary) form.
 *
 *		This is same as printtup, except we don't use the typout func,
 *		and therefore have no need for persistent state.
 * ----------------
 */
void
printtup_internal(HeapTuple tuple, TupleDesc typeinfo, DestReceiver* self)
{
	StringInfoData buf;
	int			i,
				j,
				k;
	Datum		attr;
	bool		isnull;

	/* ----------------
	 *	tell the frontend to expect new tuple data (in binary style)
	 * ----------------
	 */
	pq_beginmessage(&buf);
	pq_sendbyte(&buf, 'B');

	/* ----------------
	 *	send a bitmap of which attributes are not null
	 * ----------------
	 */
	j = 0;
	k = 1 << 7;
	for (i = 0; i < tuple->t_data->t_natts; ++i)
	{
		if (! heap_attisnull(tuple, i + 1))
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

	/* ----------------
	 *	send the attributes of this tuple
	 * ----------------
	 */
#ifdef IPORTAL_DEBUG
	fprintf(stderr, "sending tuple with %d atts\n", tuple->t_data->t_natts);
#endif
	for (i = 0; i < tuple->t_data->t_natts; ++i)
	{
		int32		len = typeinfo->attrs[i]->attlen;

		attr = heap_getattr(tuple, i + 1, typeinfo, &isnull);
		if (!isnull)
		{
			/* # of bytes, and opaque data */
			if (len == -1)
			{
				/* variable length, assume a varlena structure */
				len = VARSIZE(attr) - VARHDRSZ;

				pq_sendint(&buf, len, VARHDRSZ);
				pq_sendbytes(&buf, VARDATA(attr), len);

#ifdef IPORTAL_DEBUG
				{
					char	   *d = VARDATA(attr);

					fprintf(stderr, "length %d data %x%x%x%x\n",
							len, *d, *(d + 1), *(d + 2), *(d + 3));
				}
#endif
			}
			else
			{
				/* fixed size */
				if (typeinfo->attrs[i]->attbyval)
				{
					int8		i8;
					int16		i16;
					int32		i32;

					pq_sendint(&buf, len, sizeof(int32));
					switch (len)
					{
						case sizeof(int8):
							i8 = DatumGetChar(attr);
							pq_sendbytes(&buf, (char *) &i8, len);
							break;
						case sizeof(int16):
							i16 = DatumGetInt16(attr);
							pq_sendbytes(&buf, (char *) &i16, len);
							break;
						case sizeof(int32):
							i32 = DatumGetInt32(attr);
							pq_sendbytes(&buf, (char *) &i32, len);
							break;
					}
#ifdef IPORTAL_DEBUG
					fprintf(stderr, "byval length %d data %d\n", len, attr);
#endif
				}
				else
				{
					pq_sendint(&buf, len, sizeof(int32));
					pq_sendbytes(&buf, DatumGetPointer(attr), len);
#ifdef IPORTAL_DEBUG
					fprintf(stderr, "byref length %d data %x\n", len,
							DatumGetPointer(attr));
#endif
				}
			}
		}
	}

	pq_endmessage(&buf);
}
