/*-------------------------------------------------------------------------
 *
 * pagenum.h--
 *    POSTGRES page number definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pagenum.h,v 1.3 1996/11/05 06:11:02 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	PAGENUM_H
#define PAGENUM_H


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
