/*-------------------------------------------------------------------------
 *
 * pg_log.h
 *	  the system log relation "pg_log" is not a "heap" relation.
 *	  it is automatically created by the transam/ code and the
 *	  information here is all bogus and is just here to make the
 *	  relcache code happy.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_log.h,v 1.6 1999/02/13 23:21:11 momjian Exp $
 *
 * NOTES
 *	  The structures and macros used by the transam/ code
 *	  to access pg_log should some day go here -cim 6/18/90
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_LOG_H
#define PG_LOG_H

/* ----------------
 *		postgres.h contains the system type definintions and the
 *		CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

CATALOG(pg_log) BOOTSTRAP
{
	Oid			logfoo;
} FormData_pg_log;

typedef FormData_pg_log *Form_pg_log;

#define Natts_pg_log			1
#define Anum_pg_log_logfoo		1

#endif	 /* PG_LOG_H */
