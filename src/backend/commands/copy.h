/*-------------------------------------------------------------------------
 *
 * copy.h--
 *    Definitions for using the POSTGRES copy command.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: copy.h,v 1.2 1996/08/24 20:48:16 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef COPY_H
#define	COPY_H

#include "postgres.h"

void DoCopy(char *relname, bool binary, bool oids, bool from, bool pipe, char *filename,
	    char *delim);

#endif	/* COPY_H */
