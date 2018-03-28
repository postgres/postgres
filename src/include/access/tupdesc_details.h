/*-------------------------------------------------------------------------
 *
 * tupdesc_details.h
 *	  POSTGRES tuple descriptor definitions we can't include everywhere
 *
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/tupdesc_details.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef TUPDESC_DETAILS_H
#define TUPDESC_DETAILS_H

/*
 * Structure used to represent value to be used when the attribute is not
 * present at all in a tuple, i.e. when the column was created after the tuple
 */

typedef struct attrMissing
{
	bool		ammissingPresent;	/* true if non-NULL missing value exists */
	Datum		ammissing;		/* value when attribute is missing */
} AttrMissing;

#endif							/* TUPDESC_DETAILS_H */
