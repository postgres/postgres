/*-------------------------------------------------------------------------
 *
 * page.h--
 *	  POSTGRES buffer page abstraction definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: page.h,v 1.3 1997/09/07 05:01:30 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PAGE_H
#define PAGE_H

typedef Pointer Page;

/*
 * PageIsValid --
 *		True iff page is valid.
 */
#define PageIsValid(page) PointerIsValid(page)

#endif							/* PAGE_H */
