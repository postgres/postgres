/*-------------------------------------------------------------------------
 *
 * datum.c
 *	  POSTGRES Datum (abstract data type) manipulation routines.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/datum.c,v 1.18 2000/07/12 02:37:19 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * In the implementation of the next routines we assume the following:
 *
 * A) if a type is "byVal" then all the information is stored in the
 * Datum itself (i.e. no pointers involved!). In this case the
 * length of the type is always greater than zero and not more than
 * "sizeof(Datum)"
 * B) if a type is not "byVal" and it has a fixed length, then
 * the "Datum" always contain a pointer to a stream of bytes.
 * The number of significant bytes are always equal to the length of the
 * type.
 * C) if a type is not "byVal" and is of variable length (i.e. it has
 * length == -1) then "Datum" always points to a "struct varlena".
 * This varlena structure has information about the actual length of this
 * particular instance of the type and about its value.
 *
 * Note that we do not treat "toasted" datums specially; therefore what
 * will be copied or compared is the compressed data or toast reference.
 */

#include "postgres.h"

#include "utils/datum.h"

/*-------------------------------------------------------------------------
 * datumGetSize
 *
 * Find the "real" size of a datum, given the datum value,
 * whether it is a "by value", and its length.
 *
 * To cut a long story short, usually the real size is equal to the
 * type length, with the exception of variable length types which have
 * a length equal to -1. In this case, we have to look at the value of
 * the datum itself (which is a pointer to a 'varlena' struct) to find
 * its size.
 *-------------------------------------------------------------------------
 */
Size
datumGetSize(Datum value, bool typByVal, int typLen)
{
	Size		size;

	if (typByVal)
	{
		/* Pass-by-value types are always fixed-length */
		Assert(typLen > 0 && typLen <= sizeof(Datum));
		size = (Size) typLen;
	}
	else
	{
		if (typLen == -1)
		{
			/* Assume it is a varlena datatype */
			struct varlena *s = (struct varlena *) DatumGetPointer(value);

			if (!PointerIsValid(s))
				elog(ERROR, "datumGetSize: Invalid Datum Pointer");
			size = (Size) VARSIZE(s);
		}
		else
		{
			/* Fixed-length pass-by-ref type */
			size = (Size) typLen;
		}
	}

	return size;
}

/*-------------------------------------------------------------------------
 * datumCopy
 *
 * make a copy of a datum
 *
 * If the datatype is pass-by-reference, memory is obtained with palloc().
 *-------------------------------------------------------------------------
 */
Datum
datumCopy(Datum value, bool typByVal, int typLen)
{
	Datum		res;

	if (typByVal)
		res = value;
	else
	{
		Size		realSize;
		char	   *s;

		if (DatumGetPointer(value) == NULL)
			return PointerGetDatum(NULL);

		realSize = datumGetSize(value, typByVal, typLen);

		s = (char *) palloc(realSize);
		memcpy(s, DatumGetPointer(value), realSize);
		res = PointerGetDatum(s);
	}
	return res;
}

/*-------------------------------------------------------------------------
 * datumFree
 *
 * Free the space occupied by a datum CREATED BY "datumCopy"
 *
 * NOTE: DO NOT USE THIS ROUTINE with datums returned by amgetattr() etc.
 * ONLY datums created by "datumCopy" can be freed!
 *-------------------------------------------------------------------------
 */
#ifdef NOT_USED
void
datumFree(Datum value, bool typByVal, int typLen)
{
	if (!typByVal)
	{
		Pointer		s = DatumGetPointer(value);

		pfree(s);
	}
}

#endif

/*-------------------------------------------------------------------------
 * datumIsEqual
 *
 * Return true if two datums are equal, false otherwise
 *
 * NOTE: XXX!
 * We just compare the bytes of the two values, one by one.
 * This routine will return false if there are 2 different
 * representations of the same value (something along the lines
 * of say the representation of zero in one's complement arithmetic).
 * Also, it will probably not give the answer you want if either
 * datum has been "toasted".
 *-------------------------------------------------------------------------
 */
bool
datumIsEqual(Datum value1, Datum value2, bool typByVal, int typLen)
{
	bool	res;

	if (typByVal)
	{
		/*
		 * just compare the two datums. NOTE: just comparing "len" bytes
		 * will not do the work, because we do not know how these bytes
		 * are aligned inside the "Datum".
		 */
		res = (value1 == value2);
	}
	else
	{
		Size		size1,
					size2;
		char	   *s1,
				   *s2;

		/*
		 * Compare the bytes pointed by the pointers stored in the datums.
		 */
		size1 = datumGetSize(value1, typByVal, typLen);
		size2 = datumGetSize(value2, typByVal, typLen);
		if (size1 != size2)
			return false;
		s1 = (char *) DatumGetPointer(value1);
		s2 = (char *) DatumGetPointer(value2);
		res = (memcmp(s1, s2, size1) == 0);
	}
	return res;
}
