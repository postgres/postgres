/*-------------------------------------------------------------------------
 *
 * rename.h
 *
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rename.h,v 1.10 2001/01/24 19:43:23 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RENAME_H
#define RENAME_H

extern void renameatt(char *relname,
		  char *oldattname,
		  char *newattname,
		  int recurse);

extern void renamerel(const char *oldrelname,
		  const char *newrelname);

#endif	 /* RENAME_H */
