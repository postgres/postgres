/*-------------------------------------------------------------------------
 *
 * copy.h
 *	  Definitions for using the POSTGRES copy command.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: copy.h,v 1.16 2001/11/05 17:46:33 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef COPY_H
#define COPY_H

extern int	copy_lineno;

void DoCopy(char *relname, bool binary, bool oids, bool from, bool pipe,
	   char *filename, char *delim, char *null_print);

#endif   /* COPY_H */
