/*-------------------------------------------------------------------------
 *
 * ibit.h
 *	  POSTGRES index valid attribute bit map definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: ibit.h,v 1.11 1999/10/23 03:13:27 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef IBIT_H
#define IBIT_H

#include "catalog/pg_index.h"

typedef struct IndexAttributeBitMapData
{
	bits8		bits[(INDEX_MAX_KEYS + 8 - 1)/8];
} IndexAttributeBitMapData;

typedef IndexAttributeBitMapData *IndexAttributeBitMap;

#define IndexAttributeBitMapSize		sizeof(IndexAttributeBitMapData)

/*
 * IndexAttributeBitMapIsValid
 *		True iff attribute bit map is valid.
 */
#define IndexAttributeBitMapIsValid(bits) PointerIsValid(bits)

#endif	 /* IBIT_H */
