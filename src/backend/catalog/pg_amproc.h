/*-------------------------------------------------------------------------
 *
 * pg_amproc.h--
 *    definition of the system "amproc" relation (pg_amproce)
 *    along with the relation's initial contents.  The amproc
 *    catalog is used to store procedures used by indexed access
 *    methods that aren't associated with operators.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_amproc.h,v 1.1.1.1 1996/07/09 06:21:16 scrappy Exp $
 *
 * NOTES
 *    the genbki.sh script reads this file and generates .bki
 *    information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_AMPROC_H
#define PG_AMPROC_H

/* ----------------
 *	postgres.h contains the system type definintions and the
 *	CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *	can be read by both genbki.sh and the C compiler.
 * ----------------
 */
#include "postgres.h"

/* ----------------
 *	pg_amproc definition.  cpp turns this into
 *	typedef struct FormData_pg_amproc
 * ----------------
 */ 
CATALOG(pg_amproc) {
    Oid 	amid;
    Oid 	amopclaid;
    Oid 	amproc;
    int2 	amprocnum;
} FormData_pg_amproc;

/* ----------------
 *	Form_pg_amproc corresponds to a pointer to a tuple with
 *	the format of pg_amproc relation.
 * ----------------
 */
typedef FormData_pg_amproc	*Form_pg_amproc;

/* ----------------
 *	compiler constants for pg_amproc
 * ----------------
 */
#define Natts_pg_amproc			4
#define Anum_pg_amproc_amid		1
#define Anum_pg_amproc_amopclaid	2
#define Anum_pg_amproc_amproc		3
#define Anum_pg_amproc_amprocnum	4

/* ----------------
 *	initial contents of pg_amproc
 * ----------------
 */

DATA(insert OID = 0 (402 422 193 1));
DATA(insert OID = 0 (402 422 194 2));
DATA(insert OID = 0 (402 422 195 3));
DATA(insert OID = 0 (402 433 193 1));
DATA(insert OID = 0 (402 433 194 2));
DATA(insert OID = 0 (402 433 196 3));
DATA(insert OID = 0 (402 434 197 1));
DATA(insert OID = 0 (402 434 198 2));
DATA(insert OID = 0 (402 434 199 3));
DATA(insert OID = 0 (403 421 350 1));
DATA(insert OID = 0 (403 423 355 1));
DATA(insert OID = 0 (403 424 353 1));
DATA(insert OID = 0 (403 425 352 1));
DATA(insert OID = 0 (403 426 351 1));
DATA(insert OID = 0 (403 427 356 1));
DATA(insert OID = 0 (403 428 354 1));
DATA(insert OID = 0 (403 429 358 1));
DATA(insert OID = 0 (403 406 689 1));
DATA(insert OID = 0 (403 407 690 1));
DATA(insert OID = 0 (403 408 691 1));
DATA(insert OID = 0 (403 409 359 1));
DATA(insert OID = 0 (403 430 374 1));
DATA(insert OID = 0 (403 431 360 1));
DATA(insert OID = 0 (403 432 357 1));
DATA(insert OID = 0 (403 435 928 1));
DATA(insert OID = 0 (403 436 948 1));
DATA(insert OID = 0 (403 437 828 1));
DATA(insert OID = 0 (403 1076 1078 1));
DATA(insert OID = 0 (403 1077 1079 1));
DATA(insert OID = 0 (403 1114 1092 1));
DATA(insert OID = 0 (403 1115 1107 1));

BKI_BEGIN
#ifdef NOBTREE
BKI_END
DATA(insert OID = 0 (404 421 350 1));
DATA(insert OID = 0 (404 423 355 1));
DATA(insert OID = 0 (404 424 353 1));
DATA(insert OID = 0 (404 425 352 1));
DATA(insert OID = 0 (404 426 351 1));
DATA(insert OID = 0 (404 427 356 1));
DATA(insert OID = 0 (404 428 354 1));
DATA(insert OID = 0 (404 429 358 1));
DATA(insert OID = 0 (404 406 689 1));
DATA(insert OID = 0 (404 407 690 1));
DATA(insert OID = 0 (404 408 691 1));
DATA(insert OID = 0 (404 409 359 1));
DATA(insert OID = 0 (404 430 374 1));
DATA(insert OID = 0 (404 431 360 1));
DATA(insert OID = 0 (404 432 357 1));
BKI_BEGIN
#endif /* NOBTREE */
BKI_END

DATA(insert OID = 0 (405 421 449 1));
DATA(insert OID = 0 (405 423 452 1));
DATA(insert OID = 0 (405 426 450 1));
DATA(insert OID = 0 (405 427 453 1));
DATA(insert OID = 0 (405 428 451 1));
DATA(insert OID = 0 (405 429 454 1));
DATA(insert OID = 0 (405 406 692 1));
DATA(insert OID = 0 (405 407 693 1));
DATA(insert OID = 0 (405 408 694 1));
DATA(insert OID = 0 (405 409 455 1));
DATA(insert OID = 0 (405 430 499 1));
DATA(insert OID = 0 (405 431 456 1));
DATA(insert OID = 0 (405 1076 1080 1));
DATA(insert OID = 0 (405 1077 1081 1));

#endif /* PG_AMPROC_H */
