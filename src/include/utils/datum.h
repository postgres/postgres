/*-------------------------------------------------------------------------
 *
 * datum.h--
 *	  POSTGRES abstract data type datum representation definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: datum.h,v 1.4 1997/09/08 02:39:36 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef DATUM_H
#define DATUM_H


/*--------------------------------------------------------
 * SOME NOT VERY PORTABLE ROUTINES ???
 *--------------------------------------------------------
 *
 * In the implementation of the next routines we assume the following:
 *
 * A) if a type is "byVal" then all the information is stored in the
 * Datum itself (i.e. no pointers involved!). In this case the
 * length of the type is always greater than zero and less than
 * "sizeof(Datum)"
 * B) if a type is not "byVal" and it has a fixed length, then
 * the "Datum" always contain a pointer to a stream of bytes.
 * The number of significant bytes are always equal to the length of thr
 * type.
 * C) if a type is not "byVal" and is of variable length (i.e. it has
 * length == -1) then "Datum" always points to a "struct varlena".
 * This varlena structure has information about the actual length of this
 * particular instance of the type and about its value.
 */

/*---------------
 * datumGetSize
 * find the "real" length of a datum
 */
extern Size datumGetSize(Datum value, Oid type, bool byVal, Size len);

/*---------------
 * datumCopy
 * make a copy of a datum.
 */
extern Datum datumCopy(Datum value, Oid type, bool byVal, Size len);

/*---------------
 * datumFree
 * free space that *might* have been palloced by "datumCopy"
 */
extern void datumFree(Datum value, Oid type, bool byVal, Size len);

/*---------------
 * datumIsEqual
 * return true if thwo datums are equal, false otherwise.
 * XXX : See comments in the code for restrictions!
 */
extern		bool
datumIsEqual(Datum value1, Datum value2, Oid type,
			 bool byVal, Size len);

#endif							/* DATUM_H */
