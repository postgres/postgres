/*-------------------------------------------------------------------------
 *
 * dbcommands.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: dbcommands.h,v 1.8.2.1 1999/07/30 18:52:55 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef DBCOMMANDS_H
#define DBCOMMANDS_H

#include <signal.h>
#include "tcop/dest.h"

/*
 * Originally from tmp/daemon.h. The functions declared in daemon.h does not
 * exist; hence removed.		-- AY 7/29/94
 */
#define SIGKILLDAEMON1	SIGTERM

extern void createdb(char *dbname, char *dbpath, int encoding, CommandDest);
extern void destroydb(char *dbname, CommandDest);

#endif	 /* DBCOMMANDS_H */
