/*-------------------------------------------------------------------------
 *
 * pagenum.h
 *	  POSTGRES page number definitions.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pagenum.h,v 1.10 2001/01/24 19:43:28 momjian Exp $
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
