/*-------------------------------------------------------------------------
 *
 * pg_inheritproc.h--
 *    definition of the system "inheritproc" relation (pg_inheritproc)
 *    along with the relation's initial contents.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_inheritproc.h,v 1.1.1.1 1996/07/09 06:21:17 scrappy Exp $
 *
 * NOTES
 *    the genbki.sh script reads this file and generates .bki
 *    information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_INHERITPROC_H
#define PG_INHERITPROC_H

/* ----------------
 *	postgres.h contains the system type definintions and the
 *	CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *	can be read by both genbki.sh and the C compiler.
 * ----------------
 */
#include "postgres.h"

/* ----------------
 *	pg_inheritproc definition.  cpp turns this into
 *	typedef struct FormData_pg_inheritproc
 * ----------------
 */ 
CATALOG(pg_inheritproc) {
     NameData 	inhproname;
     Oid 	inhargrel;
     Oid 	inhdefrel;
     Oid 	inhproc;
} FormData_pg_inheritproc;

/* ----------------
 *	Form_pg_inheritproc corresponds to a pointer to a tuple with
 *	the format of pg_inheritproc relation.
 * ----------------
 */
typedef FormData_pg_inheritproc	*Form_pg_inheritproc;

/* ----------------
 *	compiler constants for pg_inheritproc
 * ----------------
 */
#define Natts_pg_inheritproc		4
#define Anum_pg_inheritproc_inhproname	1
#define Anum_pg_inheritproc_inhargrel	2
#define Anum_pg_inheritproc_inhdefrel	3
#define Anum_pg_inheritproc_inhproc	4


#endif /* PG_INHERITPROC_H */
