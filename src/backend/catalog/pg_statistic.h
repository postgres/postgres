/*-------------------------------------------------------------------------
 *
 * pg_statistic.h--
 *    definition of the system "statistic" relation (pg_statistic)
 *    along with the relation's initial contents.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_statistic.h,v 1.1.1.1 1996/07/09 06:21:18 scrappy Exp $
 *
 * NOTES
 *    the genbki.sh script reads this file and generates .bki
 *    information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_STATISTIC_H
#define PG_STATISTIC_H

/* ----------------
 *	postgres.h contains the system type definintions and the
 *	CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *	can be read by both genbki.sh and the C compiler.
 * ----------------
 */
#include "postgres.h"

/* ----------------
 *	pg_statistic definition.  cpp turns this into
 *	typedef struct FormData_pg_statistic
 * ----------------
 */ 
CATALOG(pg_statistic) {
    Oid 	starelid;
    int2 	staattnum;
    Oid 	staop;
    text 	stalokey;	/* VARIABLE LENGTH FIELD */
    text 	stahikey;	/* VARIABLE LENGTH FIELD */
} FormData_pg_statistic;

/* ----------------
 *	Form_pg_statistic corresponds to a pointer to a tuple with
 *	the format of pg_statistic relation.
 * ----------------
 */
typedef FormData_pg_statistic	*Form_pg_statistic;

/* ----------------
 *	compiler constants for pg_statistic
 * ----------------
 */
#define Natts_pg_statistic		5
#define Anum_pg_statistic_starelid	1
#define Anum_pg_statistic_staattnum	2
#define Anum_pg_statistic_staop		3
#define Anum_pg_statistic_stalokey	4
#define Anum_pg_statistic_stahikey	5

#endif /* PG_STATISTIC_H */
