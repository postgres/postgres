/*-------------------------------------------------------------------------
 *
 * pg_hosts.h--
 *
 *	   the pg_hosts system catalog provides host-based access to the
 * backend.  Only those hosts that are in the pg_hosts
 *
 *	currently, this table is not used, instead file-based host authentication
 * is used
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_hosts.h,v 1.3 1997/09/07 04:56:46 momjian Exp $
 *
 * NOTES
 *	  the genbki.sh script reads this file and generates .bki
 *	  information from the DATA() statements.
 *-------------------------------------------------------------------------
 */

#ifndef PG_HOSTS_H
#define PG_HOSTS_H

/* ----------------
 *		postgres.h contains the system type definintions and the
 *		CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

CATALOG(pg_hosts) BOOTSTRAP
{
	NameData		dbName;
	text			address;
	text			mask;
} FormData_pg_hosts;

typedef FormData_pg_hosts *Form_pg_hosts;

#define Natts_pg_hosts			3
#define Anum_pg_hosts_dbName	1
#define Anum_pg_hosts_address	2
#define Anum_pg_hosts_mask		3

#endif							/* PG_HOSTS_H */
