/*-------------------------------------------------------------------------
 *
 * printtup.c
 *	  Routines to print out tuples to the destination (binary or non-binary
 *	  portals, frontend/interactive backend, etc.).
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/common/printtup.c,v 1.58 2001/03/22 03:59:11 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/printtup.h"
#include "catalog/pg_type.h"
#include "libpq/pqformat.h"
#include "utils/syscache.h"

static void printtup_setup(DestReceiver *self, TupleDesc typeinfo);
static void printtup(HeapTuple tuple, TupleDesc typeinfo, DestReceiver *self);
static void printtup_internal(HeapTuple tuple, TupleDesc typeinfo, DestReceiver *self);
static void printtup_cleanup(DestReceiver *self);

/* ----------------------------------------------------------------
 *		printtup / debugtup support
 * ----------------------------------------------------------------
 */

/* ----------------
 *		getTypeOutputInfo -- get info needed for printing values of a type
 * ----------------
 */
bool
getTypeOutputInfo(Oid type, Oid *typOutput, Oid *typElem,
				  bool *typIsVarlena)
{
	HeapTuple	typeTuple;
	Form_pg_type pt;

	typeTuple = SearchSysCache(TYPEOID,
							   ObjectIdGetDatum(type),
							   0, 0, 0);
	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "getTypeOutputInfo: Cache lookup of type %u failed", type);
	pt = (Form_pg_type) GETSTRUCT(typeTuple);

	*typOutput = pt->typoutput;
	*typElem = pt->typelem;
	*typIsVarlena = (!pt->typbyval) && (pt->typlen == -1);
	ReleaseSysCache(typeTuple);
	return OidIsValid(*typOutput);
}

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
	TupleDesc	attrinfo;		/* The attr info we are set up for */
	int			nattrs;
	PrinttupAttrInfo *myinfo;	/* Cached info about each attr */
} DR_printtup;

/* ----------------
 *		Initialize: create a DestReceiver for printtup
 * ----------------
 */
DestReceiver *
printtup_create_DR(bool isBinary)
{
	DR_printtup *self = (DR_printtup *) palloc(sizeof(DR_printtup));

	self->pub.receiveTuple = isBinary ? printtup_internal : printtup;
	self->pub.setup = printtup_setup;
	self->pub.cleanup = printtup_cleanup;

	self->attrinfo = NULL;
	self->nattrs = 0;
	self->myinfo = NULL;

	return (DestReceiver *) self;
}

static void
printtup_setup(DestReceiver *self, TupleDesc typeinfo)
{
	/* ----------------
	 * We could set up the derived attr info at this time, but we postpone it
	 * until the first call of printtup, for 3 reasons:
	 * 1. We don't waste time (compared to the old way) if there are no
	 *	  tuples at all to output.
	 * 2. Checking in printtup allows us to handle the case that the tuples
	 *	  change type midway through (although this probably can't happen in
	 *	  the current executor).
	 * 3. Right now, ExecutorRun passes a NULL for typeinfo anyway :-(
	 * ----------------
	 */
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
 *		printtup
 * ----------------
 */
static void
printtup(HeapTuple tuple, TupleDesc typeinfo, DestReceiver *self)
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

	/* ----------------
	 *	send the attributes of this tuple
	 * ----------------
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

			pq_sendcountedtext(&buf, outputstr, strlen(outputstr));

			/* Clean up detoasted copy, if any */
			if (attr != origattr)
				pfree(DatumGetPointer(attr));
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
printtup_cleanup(DestReceiver *self)
{
	DR_printtup *myState = (DR_printtup *) self;

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
 *		printtup_internal
 *		We use a different data prefix, e.g. 'B' instead of 'D' to
 *		indicate a tuple in internal (binary) form.
 *
 *		This is largely same as printtup, except we don't use the typout func.
 * ----------------
 */
static void
printtup_internal(HeapTuple tuple, TupleDesc typeinfo, DestReceiver *self)
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

	/* ----------------
	 *	send the attributes of this tuple
	 * ----------------
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
			/* fixed size */
			attr = origattr;
			len = typeinfo->attrs[i]->attlen;
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
