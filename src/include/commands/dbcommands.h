/*-------------------------------------------------------------------------
 *
 * dbcommands.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: dbcommands.h,v 1.6 1998/09/01 04:35:29 momjian Exp $
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
