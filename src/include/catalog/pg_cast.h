/*-------------------------------------------------------------------------
 *
 * pg_cast.h
 *	  definition of the system "type casts" relation (pg_cast)
 *	  along with the relation's initial contents.
 *
 *
 * Copyright (c) 2002-2003, PostgreSQL Global Development Group
 *
 * $Id: pg_cast.h,v 1.9 2003/08/04 23:59:40 tgl Exp $
 *
 * NOTES
 *	  the genbki.sh script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_CAST_H
#define PG_CAST_H

CATALOG(pg_cast)
{
	Oid			castsource;		/* source datatype for cast */
	Oid			casttarget;		/* destination datatype for cast */
	Oid			castfunc;		/* cast function; 0 = binary coercible */
	char		castcontext;	/* contexts in which cast can be used */
} FormData_pg_cast;

typedef FormData_pg_cast *Form_pg_cast;

/*
 * The allowable values for pg_cast.castcontext are specified by this enum.
 * Since castcontext is stored as a "char", we use ASCII codes for human
 * convenience in reading the table.  Note that internally to the backend,
 * these values are converted to the CoercionContext enum (see primnodes.h),
 * which is defined to sort in a convenient order; the ASCII codes don't
 * have to sort in any special order.
 */

typedef enum CoercionCodes
{
	COERCION_CODE_IMPLICIT = 'i',		/* coercion in context of
										 * expression */
	COERCION_CODE_ASSIGNMENT = 'a',		/* coercion in context of
										 * assignment */
	COERCION_CODE_EXPLICIT = 'e'	/* explicit cast operation */
} CoercionCodes;


/* ----------------
 *		compiler constants for pg_cast
 * ----------------
 */
#define Natts_pg_cast				4
#define Anum_pg_cast_castsource		1
#define Anum_pg_cast_casttarget		2
#define Anum_pg_cast_castfunc		3
#define Anum_pg_cast_castcontext	4

/* ----------------
 *		initial contents of pg_cast
 * ----------------
 */

/*
 * Numeric category: implicit casts are allowed in the direction
 * int2->int4->int8->numeric->float4->float8, while casts in the
 * reverse direction are assignment-only.
 */
DATA(insert (	20	 21  714 a ));
DATA(insert (	20	 23  480 a ));
DATA(insert (	20	700  652 i ));
DATA(insert (	20	701  482 i ));
DATA(insert (	20 1700 1781 i ));
DATA(insert (	21	 20  754 i ));
DATA(insert (	21	 23  313 i ));
DATA(insert (	21	700  236 i ));
DATA(insert (	21	701  235 i ));
DATA(insert (	21 1700 1782 i ));
DATA(insert (	23	 20  481 i ));
DATA(insert (	23	 21  314 a ));
DATA(insert (	23	700  318 i ));
DATA(insert (	23	701  316 i ));
DATA(insert (	23 1700 1740 i ));
DATA(insert (  700	 20  653 a ));
DATA(insert (  700	 21  238 a ));
DATA(insert (  700	 23  319 a ));
DATA(insert (  700	701  311 i ));
DATA(insert (  700 1700 1742 a ));
DATA(insert (  701	 20  483 a ));
DATA(insert (  701	 21  237 a ));
DATA(insert (  701	 23  317 a ));
DATA(insert (  701	700  312 a ));
DATA(insert (  701 1700 1743 a ));
DATA(insert ( 1700	 20 1779 a ));
DATA(insert ( 1700	 21 1783 a ));
DATA(insert ( 1700	 23 1744 a ));
DATA(insert ( 1700	700 1745 i ));
DATA(insert ( 1700	701 1746 i ));

/*
 * OID category: allow implicit conversion from any integral type (including
 * int8, to support OID literals > 2G) to OID, as well as assignment coercion
 * from OID to int4 or int8.  Similarly for each OID-alias type.  Also allow
 * implicit coercions between OID and each OID-alias type, as well as
 * regproc<->regprocedure and regoper<->regoperator.  (Other coercions
 * between alias types must pass through OID.)
 */
DATA(insert (	20	 26 1287 i ));
DATA(insert (	21	 26  313 i ));
DATA(insert (	23	 26    0 i ));
DATA(insert (	26	 20 1288 a ));
DATA(insert (	26	 23    0 a ));
DATA(insert (	26	 24    0 i ));
DATA(insert (	24	 26    0 i ));
DATA(insert (	20	 24 1287 i ));
DATA(insert (	21	 24  313 i ));
DATA(insert (	23	 24    0 i ));
DATA(insert (	24	 20 1288 a ));
DATA(insert (	24	 23    0 a ));
DATA(insert (	24 2202    0 i ));
DATA(insert ( 2202	 24    0 i ));
DATA(insert (	26 2202    0 i ));
DATA(insert ( 2202	 26    0 i ));
DATA(insert (	20 2202 1287 i ));
DATA(insert (	21 2202  313 i ));
DATA(insert (	23 2202    0 i ));
DATA(insert ( 2202	 20 1288 a ));
DATA(insert ( 2202	 23    0 a ));
DATA(insert (	26 2203    0 i ));
DATA(insert ( 2203	 26    0 i ));
DATA(insert (	20 2203 1287 i ));
DATA(insert (	21 2203  313 i ));
DATA(insert (	23 2203    0 i ));
DATA(insert ( 2203	 20 1288 a ));
DATA(insert ( 2203	 23    0 a ));
DATA(insert ( 2203 2204    0 i ));
DATA(insert ( 2204 2203    0 i ));
DATA(insert (	26 2204    0 i ));
DATA(insert ( 2204	 26    0 i ));
DATA(insert (	20 2204 1287 i ));
DATA(insert (	21 2204  313 i ));
DATA(insert (	23 2204    0 i ));
DATA(insert ( 2204	 20 1288 a ));
DATA(insert ( 2204	 23    0 a ));
DATA(insert (	26 2205    0 i ));
DATA(insert ( 2205	 26    0 i ));
DATA(insert (	20 2205 1287 i ));
DATA(insert (	21 2205  313 i ));
DATA(insert (	23 2205    0 i ));
DATA(insert ( 2205	 20 1288 a ));
DATA(insert ( 2205	 23    0 a ));
DATA(insert (	26 2206    0 i ));
DATA(insert ( 2206	 26    0 i ));
DATA(insert (	20 2206 1287 i ));
DATA(insert (	21 2206  313 i ));
DATA(insert (	23 2206    0 i ));
DATA(insert ( 2206	 20 1288 a ));
DATA(insert ( 2206	 23    0 a ));

/*
 * String category: this needs to be tightened up
 */
DATA(insert (	25 1042    0 i ));
DATA(insert (	25 1043    0 i ));
DATA(insert ( 1042	 25  401 i ));
DATA(insert ( 1042 1043  401 i ));
DATA(insert ( 1043	 25    0 i ));
DATA(insert ( 1043 1042    0 i ));
DATA(insert (	18	 25  946 i ));
DATA(insert (	18 1042  860 i ));
DATA(insert (	19	 25  406 i ));
DATA(insert (	19 1042  408 i ));
DATA(insert (	19 1043 1401 i ));
DATA(insert (	25	 18  944 a ));
DATA(insert (	25	 19  407 i ));
DATA(insert ( 1042	 19  409 i ));
DATA(insert ( 1043	 19 1400 i ));

/*
 * Datetime category
 */
DATA(insert (  702 1082 1179 a ));
DATA(insert (  702 1083 1364 a ));
DATA(insert (  702 1114 2023 i ));
DATA(insert (  702 1184 1173 i ));
DATA(insert (  703 1186 1177 i ));
DATA(insert ( 1082 1114 2024 i ));
DATA(insert ( 1082 1184 1174 i ));
DATA(insert ( 1083 1186 1370 i ));
DATA(insert ( 1083 1266 2047 i ));
DATA(insert ( 1114	702 2030 a ));
DATA(insert ( 1114 1082 2029 a ));
DATA(insert ( 1114 1083 1316 a ));
DATA(insert ( 1114 1184 2028 i ));
DATA(insert ( 1184	702 1180 a ));
DATA(insert ( 1184 1082 1178 a ));
DATA(insert ( 1184 1083 2019 a ));
DATA(insert ( 1184 1114 2027 a ));
DATA(insert ( 1184 1266 1388 a ));
DATA(insert ( 1186	703 1194 a ));
DATA(insert ( 1186 1083 1419 a ));
DATA(insert ( 1266 1083 2046 a ));
/* Cross-category casts between int4 and abstime, reltime */
DATA(insert (	23	702    0 e ));
DATA(insert (  702	 23    0 e ));
DATA(insert (	23	703    0 e ));
DATA(insert (  703	 23    0 e ));

/*
 * Geometric category
 */
DATA(insert (  601	600 1532 e ));
DATA(insert (  602	600 1533 e ));
DATA(insert (  602	604 1449 a ));
DATA(insert (  603	600 1534 e ));
DATA(insert (  603	601 1541 e ));
DATA(insert (  603	604 1448 a ));
DATA(insert (  603	718 1479 e ));
DATA(insert (  604	600 1540 e ));
DATA(insert (  604	602 1447 a ));
DATA(insert (  604	603 1446 e ));
DATA(insert (  604	718 1474 e ));
DATA(insert (  718	600 1416 e ));
DATA(insert (  718	603 1480 e ));
DATA(insert (  718	604 1544 e ));

/*
 * INET category
 */
DATA(insert (  650	869    0 i ));
DATA(insert (  869	650    0 i ));

/*
 * BitString category
 */
DATA(insert ( 1560 1562    0 i ));
DATA(insert ( 1562 1560    0 i ));
/* Cross-category casts between bit and int4, int8 */
DATA(insert (	20 1560 2075 e ));
DATA(insert (	23 1560 1683 e ));
DATA(insert ( 1560	 20 2076 e ));
DATA(insert ( 1560	 23 1684 e ));

/*
 * Cross-category casts to and from TEXT
 *
 * For historical reasons, most casts to TEXT are implicit.  This is BAD
 * and should be reined in.
 */
DATA(insert (	20	 25 1289 i ));
DATA(insert (	25	 20 1290 e ));
DATA(insert (	21	 25  113 i ));
DATA(insert (	25	 21  818 e ));
DATA(insert (	23	 25  112 i ));
DATA(insert (	25	 23  819 e ));
DATA(insert (	26	 25  114 i ));
DATA(insert (	25	 26  817 e ));
DATA(insert (	25	650 1714 e ));
DATA(insert (  700	 25  841 i ));
DATA(insert (	25	700  839 e ));
DATA(insert (  701	 25  840 i ));
DATA(insert (	25	701  838 e ));
DATA(insert (  829	 25  752 e ));
DATA(insert (	25	829  767 e ));
DATA(insert (  650	 25  730 e ));
DATA(insert (  869	 25  730 e ));
DATA(insert (	25	869 1713 e ));
DATA(insert ( 1082	 25  749 i ));
DATA(insert (	25 1082  748 e ));
DATA(insert ( 1083	 25  948 i ));
DATA(insert (	25 1083  837 e ));
DATA(insert ( 1114	 25 2034 i ));
DATA(insert (	25 1114 2022 e ));
DATA(insert ( 1184	 25 1192 i ));
DATA(insert (	25 1184 1191 e ));
DATA(insert ( 1186	 25 1193 i ));
DATA(insert (	25 1186 1263 e ));
DATA(insert ( 1266	 25  939 i ));
DATA(insert (	25 1266  938 e ));
DATA(insert ( 1700	 25 1688 i ));
DATA(insert (	25 1700 1686 e ));

#endif   /* PG_CAST_H */
