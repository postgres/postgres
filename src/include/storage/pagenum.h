/*-------------------------------------------------------------------------
 *
 * pagenum.h--
 *	  POSTGRES page number definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pagenum.h,v 1.4 1997/09/07 05:01:32 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PAGENUM_H
#define PAGENUM_H


typedef uint16	PageNumber;

typedef uint32	LogicalPageNumber;

#define InvalidLogicalPageNumber		0

/*
 * LogicalPageNumberIsValid --
 *		True iff the logical page number is valid.
 */
#define LogicalPageNumberIsValid(pageNumber) \
	((bool)((pageNumber) != InvalidLogicalPageNumber))


#endif							/* PAGENUM_H */
