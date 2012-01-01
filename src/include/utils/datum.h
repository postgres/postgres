/*-------------------------------------------------------------------------
 *
 * datum.h
 *	  POSTGRES Datum (abstract data type) manipulation routines.
 *
 * These routines are driven by the 'typbyval' and 'typlen' information,
 * which must previously have been obtained by the caller for the datatype
 * of the Datum.  (We do it this way because in most situations the caller
 * can look up the info just once and use it for many per-datum operations.)
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/datum.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DATUM_H
#define DATUM_H

/*
 * datumGetSize - find the "real" length of a datum
 */
extern Size datumGetSize(Datum value, bool typByVal, int typLen);

/*
 * datumCopy - make a copy of a datum.
 *
 * If the datatype is pass-by-reference, memory is obtained with palloc().
 */
extern Datum datumCopy(Datum value, bool typByVal, int typLen);

/*
 * datumFree - free a datum previously allocated by datumCopy, if any.
 *
 * Does nothing if datatype is pass-by-value.
 */
extern void datumFree(Datum value, bool typByVal, int typLen);

/*
 * datumIsEqual
 * return true if two datums of the same type are equal, false otherwise.
 *
 * XXX : See comments in the code for restrictions!
 */
extern bool datumIsEqual(Datum value1, Datum value2,
			 bool typByVal, int typLen);

#endif   /* DATUM_H */
