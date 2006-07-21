/*-------------------------------------------------------------------------
 *
 * pg_opclass.h
 *	  definition of the system "opclass" relation (pg_opclass)
 *	  along with the relation's initial contents.
 *
 * New definition for Postgres 7.2: the primary key for this table is
 * <opcamid, opcname> --- that is, there is a row for each valid combination
 * of opclass name and index access method type.  This row specifies the
 * expected input data type for the opclass (the type of the heap column,
 * or the expression output type in the case of an index expression).  Note
 * that types binary-coercible to the specified type will be accepted too.
 *
 * For a given <opcamid, opcintype> pair, there can be at most one row that
 * has opcdefault = true; this row is the default opclass for such data in
 * such an index.
 *
 * Normally opckeytype = InvalidOid (zero), indicating that the data stored
 * in the index is the same as the data in the indexed column.	If opckeytype
 * is nonzero then it indicates that a conversion step is needed to produce
 * the stored index data, which will be of type opckeytype (which might be
 * the same or different from the input datatype).	Performing such a
 * conversion is the responsibility of the index access method --- not all
 * AMs support this.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/catalog/pg_opclass.h,v 1.71 2006/07/21 20:51:33 tgl Exp $
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
 *		postgres.h contains the system type definitions and the
 *		CATALOG(), BKI_BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------
 *		pg_opclass definition.	cpp turns this into
 *		typedef struct FormData_pg_opclass
 * ----------------
 */
#define OperatorClassRelationId  2616

CATALOG(pg_opclass,2616)
{
	Oid			opcamid;		/* index access method opclass is for */
	NameData	opcname;		/* name of this opclass */
	Oid			opcnamespace;	/* namespace of this opclass */
	Oid			opcowner;		/* opclass owner */
	Oid			opcintype;		/* type of data indexed by opclass */
	bool		opcdefault;		/* T if opclass is default for opcintype */
	Oid			opckeytype;		/* type of data in index, or InvalidOid */
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
#define Natts_pg_opclass				7
#define Anum_pg_opclass_opcamid			1
#define Anum_pg_opclass_opcname			2
#define Anum_pg_opclass_opcnamespace	3
#define Anum_pg_opclass_opcowner		4
#define Anum_pg_opclass_opcintype		5
#define Anum_pg_opclass_opcdefault		6
#define Anum_pg_opclass_opckeytype		7

/* ----------------
 *		initial contents of pg_opclass
 * ----------------
 */

DATA(insert OID =  421 (	403		abstime_ops		PGNSP PGUID  702 t 0 ));
DATA(insert OID =  397 (	403		array_ops		PGNSP PGUID 2277 t 0 ));
#define ARRAY_BTREE_OPS_OID 397
DATA(insert OID =  423 (	403		bit_ops			PGNSP PGUID 1560 t 0 ));
DATA(insert OID =  424 (	403		bool_ops		PGNSP PGUID   16 t 0 ));
#define BOOL_BTREE_OPS_OID 424
DATA(insert OID =  426 (	403		bpchar_ops		PGNSP PGUID 1042 t 0 ));
#define BPCHAR_BTREE_OPS_OID 426
DATA(insert OID =  427 (	405		bpchar_ops		PGNSP PGUID 1042 t 0 ));
DATA(insert OID =  428 (	403		bytea_ops		PGNSP PGUID   17 t 0 ));
#define BYTEA_BTREE_OPS_OID 428
DATA(insert OID =  429 (	403		char_ops		PGNSP PGUID   18 t 0 ));
DATA(insert OID =  431 (	405		char_ops		PGNSP PGUID   18 t 0 ));
DATA(insert OID =  432 (	403		cidr_ops		PGNSP PGUID  650 t 0 ));
#define CIDR_BTREE_OPS_OID 432
DATA(insert OID =  433 (	405		cidr_ops		PGNSP PGUID  650 t 0 ));
DATA(insert OID =  434 (	403		date_ops		PGNSP PGUID 1082 t 0 ));
DATA(insert OID =  435 (	405		date_ops		PGNSP PGUID 1082 t 0 ));
DATA(insert OID = 1970 (	403		float4_ops		PGNSP PGUID  700 t 0 ));
DATA(insert OID = 1971 (	405		float4_ops		PGNSP PGUID  700 t 0 ));
DATA(insert OID = 1972 (	403		float8_ops		PGNSP PGUID  701 t 0 ));
DATA(insert OID = 1973 (	405		float8_ops		PGNSP PGUID  701 t 0 ));
DATA(insert OID = 1974 (	403		inet_ops		PGNSP PGUID  869 t 0 ));
#define INET_BTREE_OPS_OID 1974
DATA(insert OID = 1975 (	405		inet_ops		PGNSP PGUID  869 t 0 ));
DATA(insert OID = 1976 (	403		int2_ops		PGNSP PGUID   21 t 0 ));
#define INT2_BTREE_OPS_OID 1976
DATA(insert OID = 1977 (	405		int2_ops		PGNSP PGUID   21 t 0 ));
DATA(insert OID = 1978 (	403		int4_ops		PGNSP PGUID   23 t 0 ));
#define INT4_BTREE_OPS_OID 1978
DATA(insert OID = 1979 (	405		int4_ops		PGNSP PGUID   23 t 0 ));
DATA(insert OID = 1980 (	403		int8_ops		PGNSP PGUID   20 t 0 ));
DATA(insert OID = 1981 (	405		int8_ops		PGNSP PGUID   20 t 0 ));
DATA(insert OID = 1982 (	403		interval_ops	PGNSP PGUID 1186 t 0 ));
DATA(insert OID = 1983 (	405		interval_ops	PGNSP PGUID 1186 t 0 ));
DATA(insert OID = 1984 (	403		macaddr_ops		PGNSP PGUID  829 t 0 ));
DATA(insert OID = 1985 (	405		macaddr_ops		PGNSP PGUID  829 t 0 ));
DATA(insert OID = 1986 (	403		name_ops		PGNSP PGUID   19 t 0 ));
#define NAME_BTREE_OPS_OID 1986
DATA(insert OID = 1987 (	405		name_ops		PGNSP PGUID   19 t 0 ));
DATA(insert OID = 1988 (	403		numeric_ops		PGNSP PGUID 1700 t 0 ));
DATA(insert OID = 1989 (	403		oid_ops			PGNSP PGUID   26 t 0 ));
#define OID_BTREE_OPS_OID 1989
DATA(insert OID = 1990 (	405		oid_ops			PGNSP PGUID   26 t 0 ));
DATA(insert OID = 1991 (	403		oidvector_ops	PGNSP PGUID   30 t 0 ));
DATA(insert OID = 1992 (	405		oidvector_ops	PGNSP PGUID   30 t 0 ));
DATA(insert OID = 1994 (	403		text_ops		PGNSP PGUID   25 t 0 ));
#define TEXT_BTREE_OPS_OID 1994
DATA(insert OID = 1995 (	405		text_ops		PGNSP PGUID   25 t 0 ));
DATA(insert OID = 1996 (	403		time_ops		PGNSP PGUID 1083 t 0 ));
DATA(insert OID = 1997 (	405		time_ops		PGNSP PGUID 1083 t 0 ));
DATA(insert OID = 1998 (	403		timestamptz_ops PGNSP PGUID 1184 t 0 ));
DATA(insert OID = 1999 (	405		timestamptz_ops PGNSP PGUID 1184 t 0 ));
DATA(insert OID = 2000 (	403		timetz_ops		PGNSP PGUID 1266 t 0 ));
DATA(insert OID = 2001 (	405		timetz_ops		PGNSP PGUID 1266 t 0 ));
DATA(insert OID = 2002 (	403		varbit_ops		PGNSP PGUID 1562 t 0 ));
DATA(insert OID = 2003 (	403		varchar_ops		PGNSP PGUID 1043 t 0 ));
#define VARCHAR_BTREE_OPS_OID 2003
DATA(insert OID = 2004 (	405		varchar_ops		PGNSP PGUID 1043 t 0 ));
DATA(insert OID = 2039 (	403		timestamp_ops	PGNSP PGUID 1114 t 0 ));
DATA(insert OID = 2040 (	405		timestamp_ops	PGNSP PGUID 1114 t 0 ));
DATA(insert OID = 2095 (	403		text_pattern_ops	PGNSP PGUID   25 f 0 ));
#define TEXT_PATTERN_BTREE_OPS_OID 2095
DATA(insert OID = 2096 (	403		varchar_pattern_ops PGNSP PGUID 1043 f 0 ));
#define VARCHAR_PATTERN_BTREE_OPS_OID 2096
DATA(insert OID = 2097 (	403		bpchar_pattern_ops	PGNSP PGUID 1042 f 0 ));
#define BPCHAR_PATTERN_BTREE_OPS_OID 2097
DATA(insert OID = 2098 (	403		name_pattern_ops	PGNSP PGUID   19 f 0 ));
#define NAME_PATTERN_BTREE_OPS_OID 2098
DATA(insert OID = 2099 (	403		money_ops		PGNSP PGUID  790 t 0 ));
DATA(insert OID = 2222 (	405		bool_ops		PGNSP PGUID   16 t 0 ));
#define BOOL_HASH_OPS_OID 2222
DATA(insert OID = 2223 (	405		bytea_ops		PGNSP PGUID   17 t 0 ));
DATA(insert OID = 2224 (	405		int2vector_ops	PGNSP PGUID   22 t 0 ));
DATA(insert OID = 2789 (	403		tid_ops			PGNSP PGUID   27 t 0 ));
DATA(insert OID = 2225 (	405		xid_ops			PGNSP PGUID   28 t 0 ));
DATA(insert OID = 2226 (	405		cid_ops			PGNSP PGUID   29 t 0 ));
DATA(insert OID = 2227 (	405		abstime_ops		PGNSP PGUID  702 t 0 ));
DATA(insert OID = 2228 (	405		reltime_ops		PGNSP PGUID  703 t 0 ));
DATA(insert OID = 2229 (	405		text_pattern_ops	PGNSP PGUID   25 f 0 ));
DATA(insert OID = 2230 (	405		varchar_pattern_ops PGNSP PGUID 1043 f 0 ));
DATA(insert OID = 2231 (	405		bpchar_pattern_ops	PGNSP PGUID 1042 f 0 ));
DATA(insert OID = 2232 (	405		name_pattern_ops	PGNSP PGUID   19 f 0 ));
DATA(insert OID = 2233 (	403		reltime_ops		PGNSP PGUID  703 t 0 ));
DATA(insert OID = 2234 (	403		tinterval_ops	PGNSP PGUID  704 t 0 ));
DATA(insert OID = 2235 (	405		aclitem_ops		PGNSP PGUID 1033 t 0 ));
DATA(insert OID = 2593 (	783		box_ops			PGNSP PGUID  603 t 0 ));
DATA(insert OID = 2594 (	783		poly_ops		PGNSP PGUID  604 t 603 ));
DATA(insert OID = 2595 (	783		circle_ops		PGNSP PGUID  718 t 603 ));
DATA(insert OID = 2745 (	2742	_int4_ops		PGNSP PGUID  1007 t 23 ));
DATA(insert OID = 2746 (	2742	_text_ops		PGNSP PGUID  1009 t 25 ));
DATA(insert OID = 2753 (	2742	_abstime_ops	PGNSP PGUID  1023 t 702 ));
DATA(insert OID = 2754 (	2742	_bit_ops		PGNSP PGUID  1561 t 1560 ));
DATA(insert OID = 2755 (	2742	_bool_ops		PGNSP PGUID  1000 t 16 ));
DATA(insert OID = 2756 (	2742	_bpchar_ops		PGNSP PGUID  1014 t 1042 ));
DATA(insert OID = 2757 (	2742	_bytea_ops		PGNSP PGUID  1001 t 17 ));
DATA(insert OID = 2758 (	2742	_char_ops		PGNSP PGUID  1002 t 18 ));
DATA(insert OID = 2759 (	2742	_cidr_ops		PGNSP PGUID  651 t 650 ));
DATA(insert OID = 2760 (	2742	_date_ops		PGNSP PGUID  1182 t 1082 ));
DATA(insert OID = 2761 (	2742	_float4_ops		PGNSP PGUID  1021 t 700 ));
DATA(insert OID = 2762 (	2742	_float8_ops		PGNSP PGUID  1022 t 701 ));
DATA(insert OID = 2763 (	2742	_inet_ops		PGNSP PGUID  1041 t 869 ));
DATA(insert OID = 2764 (	2742	_int2_ops		PGNSP PGUID  1005 t 21 ));
DATA(insert OID = 2765 (	2742	_int8_ops		PGNSP PGUID  1016 t 20 ));
DATA(insert OID = 2766 (	2742	_interval_ops	PGNSP PGUID  1187 t 1186 ));
DATA(insert OID = 2767 (	2742	_macaddr_ops	PGNSP PGUID  1040 t 829 ));
DATA(insert OID = 2768 (	2742	_name_ops		PGNSP PGUID  1003 t 19 ));
DATA(insert OID = 2769 (	2742	_numeric_ops	PGNSP PGUID  1231 t 1700 ));
DATA(insert OID = 2770 (	2742	_oid_ops		PGNSP PGUID  1028 t 26 ));
DATA(insert OID = 2771 (	2742	_oidvector_ops	PGNSP PGUID  1013 t 30 ));
DATA(insert OID = 2772 (	2742	_time_ops		PGNSP PGUID  1183 t 1083 ));
DATA(insert OID = 2773 (	2742	_timestamptz_ops	PGNSP PGUID  1185 t 1184 ));
DATA(insert OID = 2774 (	2742	_timetz_ops		PGNSP PGUID  1270 t 1266 ));
DATA(insert OID = 2775 (	2742	_varbit_ops		PGNSP PGUID  1563 t 1562 ));
DATA(insert OID = 2776 (	2742	_varchar_ops	PGNSP PGUID  1015 t 1043 ));
DATA(insert OID = 2777 (	2742	_timestamp_ops	PGNSP PGUID  1115 t 1114 ));
DATA(insert OID = 2778 (	2742	_money_ops		PGNSP PGUID  791 t 790 ));
DATA(insert OID = 2779 (	2742	_reltime_ops	PGNSP PGUID  1024 t 703 ));
DATA(insert OID = 2780 (	2742	_tinterval_ops	PGNSP PGUID  1025 t 704 ));

#endif   /* PG_OPCLASS_H */
