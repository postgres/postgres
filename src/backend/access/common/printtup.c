/*-------------------------------------------------------------------------
 *
 * printtup.c--
 *	  Routines to print out tuples to the destination (binary or non-binary
 *	  portals, frontend/interactive backend, etc.).
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/common/printtup.c,v 1.31 1998/07/26 04:30:16 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <string.h>
#include <postgres.h>

#include <fmgr.h>
#include <access/heapam.h>
#include <access/printtup.h>
#include <catalog/pg_type.h>
#include <libpq/libpq.h>
#include <utils/syscache.h>

#ifdef MULTIBYTE
#include <mb/pg_wchar.h>
#endif

/* ----------------------------------------------------------------
 *		printtup / debugtup support
 * ----------------------------------------------------------------
 */

/* ----------------
 *		typtoout - used by printtup and debugtup
 * ----------------
 */
Oid
typtoout(Oid type)
{
	HeapTuple	typeTuple;

	typeTuple = SearchSysCacheTuple(TYPOID,
									ObjectIdGetDatum(type),
									0, 0, 0);

	if (HeapTupleIsValid(typeTuple))
		return ((Oid)
				((TypeTupleForm) GETSTRUCT(typeTuple))->typoutput);

	elog(ERROR, "typtoout: Cache lookup of type %d failed", type);
	return (InvalidOid);
}

Oid
gettypelem(Oid type)
{
	HeapTuple	typeTuple;

	typeTuple = SearchSysCacheTuple(TYPOID,
									ObjectIdGetDatum(type),
									0, 0, 0);

	if (HeapTupleIsValid(typeTuple))
		return ((Oid)
				((TypeTupleForm) GETSTRUCT(typeTuple))->typelem);

	elog(ERROR, "typtoout: Cache lookup of type %d failed", type);
	return (InvalidOid);
}

/* ----------------
 *		printtup
 * ----------------
 */
void
printtup(HeapTuple tuple, TupleDesc typeinfo)
{
	int			i,
				j,
				k;
	char	   *outputstr;
	Datum		attr;
	bool		isnull;
	Oid			typoutput;
#ifdef MULTIBYTE
	unsigned char *p;
#endif

	/* ----------------
	 *	tell the frontend to expect new tuple data
	 * ----------------
	 */
	pq_putnchar("D", 1);

	/* ----------------
	 *	send a bitmap of which attributes are null
	 * ----------------
	 */
	j = 0;
	k = 1 << 7;
	for (i = 0; i < tuple->t_natts;)
	{
		i++;					/* heap_getattr is a macro, so no
								 * increment */
		attr = heap_getattr(tuple, i, typeinfo, &isnull);
		if (!isnull)
			j |= k;
		k >>= 1;
		if (!(i & 7))
		{
			pq_putint(j, 1);
			j = 0;
			k = 1 << 7;
		}
	}
	if (i & 7)
		pq_putint(j, 1);

	/* ----------------
	 *	send the attributes of this tuple
	 * ----------------
	 */
	for (i = 0; i < tuple->t_natts; ++i)
	{
		attr = heap_getattr(tuple, i + 1, typeinfo, &isnull);
		typoutput = typtoout((Oid) typeinfo->attrs[i]->atttypid);

		if (!isnull && OidIsValid(typoutput))
		{
			outputstr = fmgr(typoutput, attr,
							 gettypelem(typeinfo->attrs[i]->atttypid),
							 typeinfo->attrs[i]->atttypmod);
#ifdef MULTIBYTE
			p = pg_server_to_client(outputstr, strlen(outputstr));
			pq_putint(strlen(p) + VARHDRSZ, VARHDRSZ);
			pq_putnchar(p, strlen(p));
#else
			pq_putint(strlen(outputstr) + VARHDRSZ, VARHDRSZ);
			pq_putnchar(outputstr, strlen(outputstr));
#endif
			pfree(outputstr);
		}
	}
}

/* ----------------
 *		printatt
 * ----------------
 */
static void
printatt(unsigned attributeId,
		 AttributeTupleForm attributeP,
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
	AttributeTupleForm *attinfo = tupleDesc->attrs;

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
debugtup(HeapTuple tuple, TupleDesc typeinfo)
{
	int			i;
	Datum		attr;
	char	   *value;
	bool		isnull;
	Oid			typoutput;

	for (i = 0; i < tuple->t_natts; ++i)
	{
		attr = heap_getattr(tuple, i + 1, typeinfo, &isnull);
		typoutput = typtoout((Oid) typeinfo->attrs[i]->atttypid);

		if (!isnull && OidIsValid(typoutput))
		{
			value = fmgr(typoutput, attr,
						 gettypelem(typeinfo->attrs[i]->atttypid),
						 typeinfo->attrs[i]->atttypmod);
			printatt((unsigned) i + 1, typeinfo->attrs[i], value);
			pfree(value);
		}
	}
	printf("\t----\n");
}

/* ----------------
 *		printtup_internal
 *		Protocol expects either T, D, C, E, or N.
 *		We use a different data prefix, e.g. 'B' instead of 'D' to
 *		indicate a tuple in internal (binary) form.
 *
 *		This is same as printtup, except we don't use the typout func.
 * ----------------
 */
void
printtup_internal(HeapTuple tuple, TupleDesc typeinfo)
{
	int			i,
				j,
				k;
	Datum		attr;
	bool		isnull;

	/* ----------------
	 *	tell the frontend to expect new tuple data
	 * ----------------
	 */
	pq_putnchar("B", 1);

	/* ----------------
	 *	send a bitmap of which attributes are null
	 * ----------------
	 */
	j = 0;
	k = 1 << 7;
	for (i = 0; i < tuple->t_natts;)
	{
		i++;					/* heap_getattr is a macro, so no
								 * increment */
		attr = heap_getattr(tuple, i, typeinfo, &isnull);
		if (!isnull)
			j |= k;
		k >>= 1;
		if (!(i & 7))
		{
			pq_putint(j, 1);
			j = 0;
			k = 1 << 7;
		}
	}
	if (i & 7)
		pq_putint(j, 1);

	/* ----------------
	 *	send the attributes of this tuple
	 * ----------------
	 */
#ifdef IPORTAL_DEBUG
	fprintf(stderr, "sending tuple with %d atts\n", tuple->t_natts);
#endif
	for (i = 0; i < tuple->t_natts; ++i)
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

#ifdef MULTIBYTE
				pq_putncharlen(VARDATA(attr), len);
#else
				pq_putint(len, VARHDRSZ);
				pq_putnchar(VARDATA(attr), len);
#endif
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

					pq_putint(len, sizeof(int32));
					switch (len)
					{
						case sizeof(int8):
							i8 = DatumGetChar(attr);
							pq_putnchar((char *) &i8, len);
							break;
						case sizeof(int16):
							i16 = DatumGetInt16(attr);
							pq_putnchar((char *) &i16, len);
							break;
						case sizeof(int32):
							i32 = DatumGetInt32(attr);
							pq_putnchar((char *) &i32, len);
							break;
					}
#ifdef IPORTAL_DEBUG
					fprintf(stderr, "byval length %d data %d\n", len, attr);
#endif
				}
				else
				{
					pq_putint(len, sizeof(int32));
					pq_putnchar(DatumGetPointer(attr), len);
#ifdef IPORTAL_DEBUG
					fprintf(stderr, "byref length %d data %x\n", len,
							DatumGetPointer(attr));
#endif
				}
			}
		}
	}
}
