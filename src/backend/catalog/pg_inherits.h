/*-------------------------------------------------------------------------
 *
 * pg_inherits.h--
 *    definition of the system "inherits" relation (pg_inherits)
 *    along with the relation's initial contents.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_inherits.h,v 1.1.1.1 1996/07/09 06:21:17 scrappy Exp $
 *
 * NOTES
 *    the genbki.sh script reads this file and generates .bki
 *    information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_INHERITS_H
#define PG_INHERITS_H

/* ----------------
 *	postgres.h contains the system type definintions and the
 *	CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *	can be read by both genbki.sh and the C compiler.
 * ----------------
 */
#include "postgres.h"

/* ----------------
 *	pg_inherits definition.  cpp turns this into
 *	typedef struct FormData_pg_inherits
 * ----------------
 */ 
CATALOG(pg_inherits) {
    Oid 	inhrel;
    Oid 	inhparent;
    int4 	inhseqno;
} FormData_pg_inherits;

/* ----------------
 *	Form_pg_inherits corresponds to a pointer to a tuple with
 *	the format of pg_inherits relation.
 * ----------------
 */
typedef FormData_pg_inherits	*InheritsTupleForm;

/* ----------------
 *	compiler constants for pg_inherits
 * ----------------
 */
#define Natts_pg_inherits		3
#define Anum_pg_inherits_inhrel		1
#define Anum_pg_inherits_inhparent	2
#define Anum_pg_inherits_inhseqno	3


#endif /* PG_INHERITS_H */
