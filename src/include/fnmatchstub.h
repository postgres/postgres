/*-------------------------------------------------------------------------
 *
 * fnmatchstub.h
 *	  Stubs for fnmatch() in port/fnmatch.c
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/fnmatchstub.h,v 1.1 2008/11/24 09:15:16 mha Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef FNMATCHSTUB_H
#define FNMATCHSTUB_H

extern int fnmatch(const char *, const char *, int);
#define FNM_NOMATCH		1		/* Match failed. */
#define FNM_NOSYS		2		/* Function not implemented. */
#define FNM_NOESCAPE	0x01	/* Disable backslash escaping. */
#define FNM_PATHNAME	0x02	/* Slash must be matched by slash. */
#define FNM_PERIOD		0x04	/* Period must be matched by period. */
#define FNM_CASEFOLD	0x08	/* Pattern is matched case-insensitive */
#define FNM_LEADING_DIR	0x10	/* Ignore /<tail> after Imatch. */


#endif
