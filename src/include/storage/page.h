/*-------------------------------------------------------------------------
 *
 * page.h
 *	  POSTGRES buffer page abstraction definitions.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: page.h,v 1.11 2001/11/05 17:46:35 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PAGE_H
#define PAGE_H

typedef Pointer Page;

/*
 * PageIsValid
 *		True iff page is valid.
 */
#define PageIsValid(page) PointerIsValid(page)

#endif   /* PAGE_H */
