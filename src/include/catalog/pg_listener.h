/*-------------------------------------------------------------------------
 *
 * pg_listener.h--
 *	  Asynchronous notification
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_listener.h,v 1.4 1997/09/08 02:35:17 momjian Exp $
 *
 * NOTES
 *	  the genbki.sh script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_LISTENER_H
#define PG_LISTENER_H

/* ----------------
 *		postgres.h contains the system type definintions and the
 *		CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------------------------------------------------------
 *		pg_listener definition.
 *
 *		cpp turns this into typedef struct FormData_pg_listener
 * ----------------------------------------------------------------
 */

CATALOG(pg_listener)
{
	NameData	relname;
	int4		listenerpid;
	int4		notification;
} FormData_pg_listener;

/* ----------------
 *		compiler constants for pg_listener
 * ----------------
 */
#define Natts_pg_listener						3
#define Anum_pg_listener_relname				1
#define Anum_pg_listener_pid					2
#define Anum_pg_listener_notify					3

/* ----------------
 *		initial contents of pg_listener are NOTHING.
 * ----------------
 */


#endif							/* PG_LISTENER_H */
