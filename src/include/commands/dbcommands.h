/*-------------------------------------------------------------------------
 *
 * dbcommands.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: dbcommands.h,v 1.4 1998/07/26 04:31:23 scrappy Exp $
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

#ifdef MULTIBYTE
extern void createdb(char *dbname, char *dbpath, int encoding);
#else
extern void createdb(char *dbname, char *dbpath);
#endif
extern void destroydb(char *dbname);

#endif							/* DBCOMMANDS_H */
