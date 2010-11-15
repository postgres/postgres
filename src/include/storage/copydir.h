/*-------------------------------------------------------------------------
 *
 * copydir.h
 *	  Header for src/port/copydir.c compatibility functions.
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/copydir.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef COPYDIR_H
#define COPYDIR_H

extern void copydir(char *fromdir, char *todir, bool recurse);

#endif   /* COPYDIR_H */
