/*-------------------------------------------------------------------------
 *
 * ibit.h--
 *    POSTGRES index valid attribute bit map definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: ibit.h,v 1.2 1996/10/19 04:09:32 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	IBIT_H
#define IBIT_H

#include "utils/memutils.h"

typedef struct IndexAttributeBitMapData {
	char	bits[(MaxIndexAttributeNumber + MaxBitsPerByte - 1)
		/ MaxBitsPerByte];
} IndexAttributeBitMapData;

typedef IndexAttributeBitMapData	*IndexAttributeBitMap;

#define IndexAttributeBitMapSize	sizeof(IndexAttributeBitMapData)

/*
 * IndexAttributeBitMapIsValid --
 *	True iff attribute bit map is valid.
 */
#define	IndexAttributeBitMapIsValid(bits) PointerIsValid(bits)

#endif	/* IBIT_H */
