/*-------------------------------------------------------------------------
 *
 * dbcommands.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: dbcommands.h,v 1.1 1996/08/28 07:23:53 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	DBCOMMANDS_H
#define	DBCOMMANDS_H

/*
 * Originally from tmp/daemon.h. The functions declared in daemon.h does not
 * exist; hence removed.	-- AY 7/29/94
 */
#define	SIGKILLDAEMON1	SIGINT
#define	SIGKILLDAEMON2	SIGTERM

extern void createdb(char *dbname);
extern void destroydb(char *dbname);
void stop_vacuum(char *dbname);

#endif	/* DBCOMMANDS_H */

