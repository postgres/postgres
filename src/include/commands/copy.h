/*-------------------------------------------------------------------------
 *
 * copy.h
 *	  Definitions for using the POSTGRES copy command.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: copy.h,v 1.10 2000/02/13 18:59:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef COPY_H
#define COPY_H

extern int lineno;

void DoCopy(char *relname, bool binary, bool oids, bool from, bool pipe,
			char *filename, char *delim, char *null_print);

#endif	 /* COPY_H */
