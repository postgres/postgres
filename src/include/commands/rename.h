/*-------------------------------------------------------------------------
 *
 * rename.h
 *
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rename.h,v 1.15 2002/03/29 19:06:22 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RENAME_H
#define RENAME_H

extern void renameatt(Oid relid,
		  char *oldattname,
		  char *newattname,
		  bool recurse);

extern void renamerel(const RangeVar *relation,
		  const char *newrelname);

#endif   /* RENAME_H */
