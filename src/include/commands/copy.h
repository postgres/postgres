/*-------------------------------------------------------------------------
 *
 * copy.h
 *	  Definitions for using the POSTGRES copy command.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: copy.h,v 1.5 1999/02/13 23:21:18 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef COPY_H
#define COPY_H


void DoCopy(char *relname, bool binary, bool oids, bool from, bool pipe, char *filename,
	   char *delim);

#endif	 /* COPY_H */
