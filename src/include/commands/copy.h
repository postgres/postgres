/*-------------------------------------------------------------------------
 *
 * copy.h--
 *    Definitions for using the POSTGRES copy command.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: copy.h,v 1.1 1996/08/28 07:21:44 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef COPY_H
#define	COPY_H

#include "postgres.h"

void DoCopy(char *relname, bool binary, bool oids, bool from, bool pipe, char *filename,
	    char *delim);

#endif	/* COPY_H */
