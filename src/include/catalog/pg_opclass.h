/*-------------------------------------------------------------------------
 *
 * pg_opclass.h
 *	  definition of the system "opclass" relation (pg_opclass)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_opclass.h,v 1.35 2000/07/30 22:13:59 tgl Exp $
 *
 * NOTES
 *	  the genbki.sh script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_OPCLASS_H
#define PG_OPCLASS_H

/* ----------------
 *		postgres.h contains the system type definintions and the
 *		CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------
 *		pg_opclass definition.	cpp turns this into
 *		typedef struct FormData_pg_opclass
 * ----------------
 */

CATALOG(pg_opclass)
{
	NameData	opcname;		/* name of opclass defined by this row */
	Oid			opcdeftype;		/* type that opclass is default for, or 0 */
} FormData_pg_opclass;

/* ----------------
 *		Form_pg_opclass corresponds to a pointer to a tuple with
 *		the format of pg_opclass relation.
 * ----------------
 */
typedef FormData_pg_opclass *Form_pg_opclass;

/* ----------------
 *		compiler constants for pg_opclass
 * ----------------
 */
#define Natts_pg_opclass				2
#define Anum_pg_opclass_opcname			1
#define Anum_pg_opclass_opcdeftype		2

/* ----------------
 *		initial contents of pg_opclass
 * ----------------
 */

/*
 * putting _null_'s in the (fixed-length) type field is bad
 * (see the README in this directory), so just put zeros
 * in, which are invalid OID's anyway.  --djm
 */
DATA(insert OID =  421 (	int2_ops		 21   ));
DESCR("");
DATA(insert OID =  422 (	box_ops			603   ));
DESCR("");
DATA(insert OID =  423 (	float8_ops		701   ));
DESCR("");
DATA(insert OID =  426 (	int4_ops		 23   ));
DESCR("");
#define INT4_OPS_OID 426
DATA(insert OID =  427 (	oid_ops			 26   ));
DESCR("");
#define OID_OPS_OID 427
DATA(insert OID =  428 (	float4_ops		700   ));
DESCR("");
DATA(insert OID =  429 (	char_ops		 18   ));
DESCR("");
DATA(insert OID =  431 (	text_ops		 25   ));
DESCR("");
DATA(insert OID =  432 (	abstime_ops		702   ));
DESCR("");
DATA(insert OID =  433 (	bigbox_ops		  0   ));
DESCR("");
DATA(insert OID =  434 (	poly_ops		604   ));
DESCR("");
DATA(insert OID =  435 (	oidvector_ops	 30   ));
DESCR("");
DATA(insert OID =  714 (	circle_ops		718   ));
DESCR("");
DATA(insert OID =  754 (	int8_ops		 20   ));
DESCR("");
DATA(insert OID = 1076 (	bpchar_ops	   1042   ));
DESCR("");
DATA(insert OID = 1077 (	varchar_ops    1043   ));
DESCR("");
DATA(insert OID = 1114 (	date_ops	   1082   ));
DESCR("");
DATA(insert OID = 1115 (	time_ops	   1083   ));
DESCR("");
DATA(insert OID = 1181 (	name_ops		 19   ));
DESCR("");
DATA(insert OID = 1312 (	timestamp_ops  1184   ));
DESCR("");
DATA(insert OID = 1313 (	interval_ops   1186   ));
DESCR("");
DATA(insert OID = 810  (	macaddr_ops		829   ));
DESCR("");
DATA(insert OID = 935  (	inet_ops		869   ));
DESCR("");
DATA(insert OID = 652  (	cidr_ops		650   ));
DESCR("");
DATA(insert OID = 1768 (	numeric_ops    1700   ));
DESCR("");
DATA(insert OID = 1690 (	bool_ops		 16   ));
DESCR("");
DATA(insert OID = 1399 (	timetz_ops	   1266   ));
DESCR("");

#endif	 /* PG_OPCLASS_H */
