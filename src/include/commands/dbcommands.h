/*-------------------------------------------------------------------------
 *
 * dbcommands.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: dbcommands.h,v 1.3 1998/07/24 03:32:19 scrappy Exp $
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

#ifdef MB
extern void createdb(char *dbname, char *dbpath, int encoding);
#else
extern void createdb(char *dbname, char *dbpath);
#endif
extern void destroydb(char *dbname);

#endif							/* DBCOMMANDS_H */
