/*-------------------------------------------------------------------------
 *
 * pagenum.h
 *	  POSTGRES page number definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pagenum.h,v 1.8 1999/05/25 16:14:43 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PAGENUM_H
#define PAGENUM_H


typedef uint16 PageNumber;

typedef uint32 LogicalPageNumber;

#define InvalidLogicalPageNumber		0

/*
 * LogicalPageNumberIsValid
 *		True iff the logical page number is valid.
 */
#define LogicalPageNumberIsValid(pageNumber) \
	((bool)((pageNumber) != InvalidLogicalPageNumber))


#endif	 /* PAGENUM_H */
