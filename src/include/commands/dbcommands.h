/*-------------------------------------------------------------------------
 *
 * dbcommands.h
 *
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: dbcommands.h,v 1.16 2001/03/22 04:00:42 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef DBCOMMANDS_H
#define DBCOMMANDS_H

extern void createdb(const char *dbname, const char *dbpath,
		 const char *dbtemplate, int encoding);
extern void dropdb(const char *dbname);

#endif	 /* DBCOMMANDS_H */
