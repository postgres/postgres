/*-------------------------------------------------------------------------
 *
 * copy.h
 *	  Definitions for using the POSTGRES copy command.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: copy.h,v 1.9 2000/01/26 05:58:00 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef COPY_H
#define COPY_H


void DoCopy(char *relname, bool binary, bool oids, bool from, bool pipe,
			char *filename, char *delim, char *null_print);

#endif	 /* COPY_H */
