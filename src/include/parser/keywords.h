/*-------------------------------------------------------------------------
 *
 * keywords.h
 *	  string,atom lookup thingy, reduces strcmp traffic greatly
 *	  in the bowels of the system.	Look for actual defs in lib/C/atoms.c
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: keywords.h,v 1.6 1999/02/13 23:21:55 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef KEYWORDS_H
#define KEYWORDS_H

typedef struct ScanKeyword
{
	char	   *name;
	int			value;
} ScanKeyword;

extern ScanKeyword *ScanKeywordLookup(char *text);
extern char *AtomValueGetString(int atomval);

#endif	 /* KEYWORDS_H */
