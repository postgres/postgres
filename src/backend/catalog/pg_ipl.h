/*-------------------------------------------------------------------------
 *
 * pg_ipl.h--
 *    definition of the system "ipl" relation (pg_ipl)
 *    along with the relation's initial contents.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_ipl.h,v 1.1.1.1 1996/07/09 06:21:17 scrappy Exp $
 *
 * NOTES
 *    the genbki.sh script reads this file and generates .bki
 *    information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_IPL_H
#define PG_IPL_H

/* ----------------
 *	postgres.h contains the system type definintions and the
 *	CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *	can be read by both genbki.sh and the C compiler.
 * ----------------
 */
#include "postgres.h"

/* ----------------
 *	pg_ipl definition.  cpp turns this into
 *	typedef struct FormData_pg_ipl
 * ----------------
 */ 
CATALOG(pg_ipl) {
     Oid 	iplrel;
     Oid 	iplipl;
     int4 	iplseqno;
} FormData_pg_ipl;

/* ----------------
 *	Form_pg_ipl corresponds to a pointer to a tuple with
 *	the format of pg_ipl relation.
 * ----------------
 */
typedef FormData_pg_ipl	*Form_pg_ipl;

/* ----------------
 *	compiler constants for pg_ipl
 * ----------------
 */
#define Natts_pg_ipl		3
#define Anum_pg_ipl_iplrel	1
#define Anum_pg_ipl_iplipl	2
#define Anum_pg_ipl_iplseqno	3


#endif /* PG_IPL_H */
