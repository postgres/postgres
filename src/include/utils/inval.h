/*-------------------------------------------------------------------------
 *
 * inval.h--
 *	  POSTGRES cache invalidation dispatcher definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: inval.h,v 1.10 1998/10/12 00:53:36 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef INVAL_H
#define INVAL_H

#include <access/htup.h>
#include <utils/rel.h>

extern void InitLocalInvalidateData(void);

extern void DiscardInvalid(void);

extern void RegisterInvalid(bool send);

extern void RelationInvalidateHeapTuple(Relation relation, HeapTuple tuple);

/*
 * POSTGRES local cache invalidation definitions. (originates from linval.h)
 */
typedef struct InvalidationUserData
{
	struct InvalidationUserData *dataP[1];		/* VARIABLE LENGTH */
} InvalidationUserData;			/* VARIABLE LENGTH STRUCTURE */

typedef struct InvalidationEntryData
{
	InvalidationUserData *nextP;
	InvalidationUserData userData;		/* VARIABLE LENGTH ARRAY */
} InvalidationEntryData;		/* VARIABLE LENGTH STRUCTURE */

typedef Pointer InvalidationEntry;

typedef InvalidationEntry LocalInvalid;

#define EmptyLocalInvalid		NULL

#endif	 /* INVAL_H */
