/*-------------------------------------------------------------------------
 *
 * pg_opclass.h--
 *    definition of the system "opclass" relation (pg_opclass)
 *    along with the relation's initial contents.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_opclass.h,v 1.1.1.1 1996/07/09 06:21:17 scrappy Exp $
 *
 * NOTES
 *    the genbki.sh script reads this file and generates .bki
 *    information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_OPCLASS_H
#define PG_OPCLASS_H

/* ----------------
 *	postgres.h contains the system type definintions and the
 *	CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *	can be read by both genbki.sh and the C compiler.
 * ----------------
 */
#include "postgres.h"

/* ----------------
 *	pg_opclass definition.  cpp turns this into
 *	typedef struct FormData_pg_opclass
 * ----------------
 */ 

CATALOG(pg_opclass) {
    NameData opcname;
} FormData_pg_opclass;

/* ----------------
 *	Form_pg_opclass corresponds to a pointer to a tuple with
 *	the format of pg_opclass relation.
 * ----------------
 */
typedef FormData_pg_opclass	*Form_pg_opclass;

/* ----------------
 *	compiler constants for pg_opclass
 * ----------------
 */
#define Natts_pg_opclass		1
#define Anum_pg_opclass_opcname		1

/* ----------------
 *	initial contents of pg_opclass
 * ----------------
 */

DATA(insert OID = 406 (    char2_ops ));
DATA(insert OID = 407 (    char4_ops ));
DATA(insert OID = 408 (    char8_ops ));
DATA(insert OID = 409 (    name_ops ));
DATA(insert OID = 421 (    int2_ops ));
DATA(insert OID = 422 (    box_ops ));
DATA(insert OID = 423 (    float8_ops ));
DATA(insert OID = 424 (    int24_ops ));
DATA(insert OID = 425 (    int42_ops ));
DATA(insert OID = 426 (    int4_ops ));
#define INT4_OPS_OID 426
DATA(insert OID = 427 (    oid_ops ));
DATA(insert OID = 428 (    float4_ops ));
DATA(insert OID = 429 (    char_ops ));
DATA(insert OID = 430 (    char16_ops ));
DATA(insert OID = 431 (    text_ops ));
DATA(insert OID = 432 (    abstime_ops ));
DATA(insert OID = 433 (    bigbox_ops));
DATA(insert OID = 434 (    poly_ops));
DATA(insert OID = 435 (    oidint4_ops));
DATA(insert OID = 436 (    oidname_ops));
DATA(insert OID = 437 (    oidint2_ops));
DATA(insert OID = 1076 (   bpchar_ops));
DATA(insert OID = 1077 (   varchar_ops));
DATA(insert OID = 1114 (   date_ops));
DATA(insert OID = 1115 (   time_ops));

#endif /* PG_OPCLASS_H */
