/*-------------------------------------------------------------------------
 *
 * dbcommands.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: dbcommands.h,v 1.12 2000/01/13 18:26:16 petere Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef DBCOMMANDS_H
#define DBCOMMANDS_H

extern void createdb(const char *dbname, const char *dbpath, int encoding);
extern void dropdb(const char *dbname);

#endif	 /* DBCOMMANDS_H */
