/*-------------------------------------------------------------------------
 *
 * copy.h--
 *	  Definitions for using the POSTGRES copy command.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: copy.h,v 1.4 1998/09/01 04:35:26 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef COPY_H
#define COPY_H


void DoCopy(char *relname, bool binary, bool oids, bool from, bool pipe, char *filename,
	   char *delim);

#endif	 /* COPY_H */
