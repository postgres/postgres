/*-------------------------------------------------------------------------
 *
 * dbcommands.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: dbcommands.h,v 1.7 1999/02/13 23:21:18 momjian Exp $
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

extern void createdb(char *dbname, char *dbpath, int encoding);
extern void destroydb(char *dbname);

#endif	 /* DBCOMMANDS_H */
