/*-------------------------------------------------------------------------
 *
 * rename.h
 *
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rename.h,v 1.13 2001/11/05 17:46:33 momjian Exp $
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

#endif   /* RENAME_H */
