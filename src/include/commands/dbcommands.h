/*-------------------------------------------------------------------------
 *
 * dbcommands.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: dbcommands.h,v 1.8 1999/03/16 03:24:18 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef DBCOMMANDS_H
#define DBCOMMANDS_H

/*
 * Originally from tmp/daemon.h. The functions declared in daemon.h does not
 * exist; hence removed.		-- AY 7/29/94
 */
#define SIGKILLDAEMON1	SIGTERM

extern void createdb(char *dbname, char *dbpath, int encoding, CommandDest);
extern void destroydb(char *dbname, CommandDest);

#endif	 /* DBCOMMANDS_H */
