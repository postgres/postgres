/*-------------------------------------------------------------------------
 *
 * rename.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rename.h,v 1.7 2000/01/22 14:20:54 petere Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RENAME_H
#define RENAME_H

extern void renameatt(char *relname,
		  char *oldattname,
		  char *newattname,
		  char *userName, int recurse);

extern void renamerel(const char *oldrelname,
		  const char *newrelname);

#endif	 /* RENAME_H */
