/*-------------------------------------------------------------------------
 *
 * copy.h
 *	  Definitions for using the POSTGRES copy command.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: copy.h,v 1.13 2001/06/08 21:16:48 petere Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef COPY_H
#define COPY_H

extern int	copy_lineno;

void DoCopy(char *relname, bool binary, bool oids, bool from, bool pipe,
	   char *filename, char *delim, char *null_print);

#endif	 /* COPY_H */
