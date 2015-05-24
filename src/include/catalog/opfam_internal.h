/*-------------------------------------------------------------------------
 *
 * opfam_internal.h
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/opfam_internal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef OPFAM_INTERNAL_H
#define OPFAM_INTERNAL_H

/*
 * We use lists of this struct type to keep track of both operators and
 * procedures while building or adding to an opfamily.
 */
typedef struct
{
	Oid			object;			/* operator or support proc's OID */
	int			number;			/* strategy or support proc number */
	Oid			lefttype;		/* lefttype */
	Oid			righttype;		/* righttype */
	Oid			sortfamily;		/* ordering operator's sort opfamily, or 0 */
} OpFamilyMember;

#endif   /* OPFAM_INTERNAL_H */
