/*-------------------------------------------------------------------------
 *
 * pagenum.h--
 *    POSTGRES page number definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pagenum.h,v 1.1.1.1 1996/07/09 06:21:53 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	PAGENUM_H
#define PAGENUM_H

#include "c.h"
#include "storage/page.h"

typedef uint16	PageNumber;

typedef uint32	LogicalPageNumber;

#define InvalidLogicalPageNumber	0

/*
 * LogicalPageNumberIsValid --
 *	True iff the logical page number is valid.
 */
#define LogicalPageNumberIsValid(pageNumber) \
    ((bool)((pageNumber) != InvalidLogicalPageNumber))


#endif	/* PAGENUM_H */
