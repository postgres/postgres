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
 * or the function output type in the case of a functional index).  Note
 * that types binary-compatible with the specified type will be accepted too.
 *
 * For a given <opcamid, opcintype> pair, there can be at most one row that
 * has opcdefault = true; this row is the default opclass for such data in
 * such an index.
 *
 * Normally opckeytype = InvalidOid (zero), indicating that the data stored
 * in the index is the same as the input data.  If opckeytype is nonzero
 * then it indicates that a conversion step is needed to produce the stored
 * index data, which will be of type opckeytype (which might be the same or
 * different from the input data).  Performing such a conversion is the
 * responsibility of the index access method --- not all AMs support this.
 * 
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_opclass.h,v 1.40 2001/09/28 08:09:13 thomas Exp $
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
	Oid			opcamid;		/* index access method opclass is for */
	NameData	opcname;		/* name of this opclass */
	Oid			opcintype;		/* type of input data for opclass */
	bool		opcdefault;		/* T if opclass is default for opcintype */
	Oid			opckeytype;		/* type of index data, or InvalidOid */
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
#define Natts_pg_opclass				5
#define Anum_pg_opclass_opcamid			1
#define Anum_pg_opclass_opcname			2
#define Anum_pg_opclass_opcintype		3
#define Anum_pg_opclass_opcdefault		4
#define Anum_pg_opclass_opckeytype		5

/* ----------------
 *		initial contents of pg_opclass
 * ----------------
 */

DATA(insert OID =  421 (	403		abstime_ops		 702	t	0 ));
DATA(insert OID =  422 (	402		bigbox_ops		 603	f	0 ));
DATA(insert OID =  423 (	403		bit_ops			1560	t	0 ));
DATA(insert OID =  424 (	403		bool_ops		  16	t	0 ));
DATA(insert OID =  425 (	402		box_ops			 603	t	0 ));
DATA(insert OID =  426 (	403		bpchar_ops		1042	t	0 ));
DATA(insert OID =  427 (	405		bpchar_ops		1042	t	0 ));
DATA(insert OID =  428 (	403		bytea_ops		  17	t	0 ));
DATA(insert OID =  429 (	403		char_ops		  18	t	0 ));
DATA(insert OID =  431 (	405		char_ops		  18	t	0 ));
DATA(insert OID =  432 (	403		cidr_ops		 650	t	0 ));
DATA(insert OID =  433 (	405		cidr_ops		 650	t	0 ));
DATA(insert OID =  434 (	403		date_ops		1082	t	0 ));
DATA(insert OID =  435 (	405		date_ops		1082	t	0 ));
DATA(insert OID = 1970 (	403		float4_ops		 700	t	0 ));
DATA(insert OID = 1971 (	405		float4_ops		 700	t	0 ));
DATA(insert OID = 1972 (	403		float8_ops		 701	t	0 ));
DATA(insert OID = 1973 (	405		float8_ops		 701	t	0 ));
DATA(insert OID = 1974 (	403		inet_ops		 869	t	0 ));
DATA(insert OID = 1975 (	405		inet_ops		 869	t	0 ));
DATA(insert OID = 1976 (	403		int2_ops		  21	t	0 ));
#define INT2_BTREE_OPS_OID 1976
DATA(insert OID = 1977 (	405		int2_ops		  21	t	0 ));
DATA(insert OID = 1978 (	403		int4_ops		  23	t	0 ));
#define INT4_BTREE_OPS_OID 1978
DATA(insert OID = 1979 (	405		int4_ops		  23	t	0 ));
DATA(insert OID = 1980 (	403		int8_ops		  20	t	0 ));
DATA(insert OID = 1981 (	405		int8_ops		  20	t	0 ));
DATA(insert OID = 1982 (	403		interval_ops	1186	t	0 ));
DATA(insert OID = 1983 (	405		interval_ops	1186	t	0 ));
DATA(insert OID = 1984 (	403		macaddr_ops		 829	t	0 ));
DATA(insert OID = 1985 (	405		macaddr_ops		 829	t	0 ));
DATA(insert OID = 1986 (	403		name_ops		  19	t	0 ));
DATA(insert OID = 1987 (	405		name_ops		  19	t	0 ));
DATA(insert OID = 1988 (	403		numeric_ops		1700	t	0 ));
DATA(insert OID = 1989 (	403		oid_ops			  26	t	0 ));
#define OID_BTREE_OPS_OID 1989
DATA(insert OID = 1990 (	405		oid_ops			  26	t	0 ));
DATA(insert OID = 1991 (	403		oidvector_ops	  30	t	0 ));
DATA(insert OID = 1992 (	405		oidvector_ops	  30	t	0 ));
DATA(insert OID = 1993 (	402		poly_ops		 604	t	0 ));
DATA(insert OID = 1994 (	403		text_ops		  25	t	0 ));
DATA(insert OID = 1995 (	405		text_ops		  25	t	0 ));
DATA(insert OID = 1996 (	403		time_ops		1083	t	0 ));
DATA(insert OID = 1997 (	405		time_ops		1083	t	0 ));
DATA(insert OID = 1998 (	403		timestamptz_ops	1184	t	0 ));
DATA(insert OID = 1999 (	405		timestamptz_ops	1184	t	0 ));
DATA(insert OID = 2000 (	403		timetz_ops		1266	t	0 ));
DATA(insert OID = 2001 (	405		timetz_ops		1266	t	0 ));
DATA(insert OID = 2002 (	403		varbit_ops		1562	t	0 ));
DATA(insert OID = 2003 (	403		varchar_ops		1043	t	0 ));
DATA(insert OID = 2004 (	405		varchar_ops		1043	t	0 ));
DATA(insert OID = 2039 (	403		timestamp_ops	1114	t	0 ));
DATA(insert OID = 2040 (	405		timestamp_ops	1114	t	0 ));

#endif	 /* PG_OPCLASS_H */
