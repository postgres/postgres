/*-------------------------------------------------------------------------
 *
 * dbcommands.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: dbcommands.h,v 1.4 1997/09/08 02:38:05 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef DBCOMMANDS_H
#define DBCOMMANDS_H

/*
 * Originally from tmp/daemon.h. The functions declared in daemon.h does not
 * exist; hence removed.		-- AY 7/29/94
 */
#define SIGKILLDAEMON1	SIGINT
#define SIGKILLDAEMON2	SIGTERM

extern void createdb(char *dbname);
extern void destroydb(char *dbname);

#endif							/* DBCOMMANDS_H */
