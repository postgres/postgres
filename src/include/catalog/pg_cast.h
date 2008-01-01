/*-------------------------------------------------------------------------
 *
 * pg_cast.h
 *	  definition of the system "type casts" relation (pg_cast)
 *	  along with the relation's initial contents.
 *
 * As of Postgres 8.0, pg_cast describes not only type coercion functions
 * but also length coercion functions.
 *
 *
 * Copyright (c) 2002-2008, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/include/catalog/pg_cast.h,v 1.38 2008/01/01 19:45:56 momjian Exp $
 *
 * NOTES
 *	  the genbki.sh script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_CAST_H
#define PG_CAST_H

#define CastRelationId	2605

CATALOG(pg_cast,2605)
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
	COERCION_CODE_IMPLICIT = 'i',		/* coercion in context of expression */
	COERCION_CODE_ASSIGNMENT = 'a',		/* coercion in context of assignment */
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
 *
 * Note: this table has OIDs, but we don't bother to assign them manually,
 * since nothing needs to know the specific OID of any built-in cast.
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

/* Allow explicit coercions between int4 and bool */
DATA(insert (	23	16	2557 e ));
DATA(insert (	16	23	2558 e ));

/*
 * OID category: allow implicit conversion from any integral type (including
 * int8, to support OID literals > 2G) to OID, as well as assignment coercion
 * from OID to int4 or int8.  Similarly for each OID-alias type.  Also allow
 * implicit coercions between OID and each OID-alias type, as well as
 * regproc<->regprocedure and regoper<->regoperator.  (Other coercions
 * between alias types must pass through OID.)	Lastly, there are implicit
 * casts from text and varchar to regclass, which exist mainly to support
 * legacy forms of nextval() and related functions.
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
DATA(insert (	26 3734    0 i ));
DATA(insert ( 3734	 26    0 i ));
DATA(insert (	20 3734 1287 i ));
DATA(insert (	21 3734  313 i ));
DATA(insert (	23 3734    0 i ));
DATA(insert ( 3734	 20 1288 a ));
DATA(insert ( 3734	 23    0 a ));
DATA(insert (	26 3769    0 i ));
DATA(insert ( 3769	 26    0 i ));
DATA(insert (	20 3769 1287 i ));
DATA(insert (	21 3769  313 i ));
DATA(insert (	23 3769    0 i ));
DATA(insert ( 3769	 20 1288 a ));
DATA(insert ( 3769	 23    0 a ));
DATA(insert (	25 2205 1079 i ));
DATA(insert ( 1043 2205 1079 i ));

/*
 * String category
 */
DATA(insert (	25 1042    0 i ));
DATA(insert (	25 1043    0 i ));
DATA(insert ( 1042	 25  401 i ));
DATA(insert ( 1042 1043  401 i ));
DATA(insert ( 1043	 25    0 i ));
DATA(insert ( 1043 1042    0 i ));
DATA(insert (	18	 25  946 i ));
DATA(insert (	18 1042  860 a ));
DATA(insert (	18 1043  946 a ));
DATA(insert (	19	 25  406 i ));
DATA(insert (	19 1042  408 a ));
DATA(insert (	19 1043 1401 a ));
DATA(insert (	25	 18  944 a ));
DATA(insert ( 1042	 18  944 a ));
DATA(insert ( 1043	 18  944 a ));
DATA(insert (	25	 19  407 i ));
DATA(insert ( 1042	 19  409 i ));
DATA(insert ( 1043	 19 1400 i ));

/* Allow explicit coercions between int4 and "char" */
DATA(insert (	18	 23   77 e ));
DATA(insert (	23	 18   78 e ));

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
DATA(insert (  869	650 1715 a ));

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
 * We need entries here only for a few specialized cases where the behavior
 * of the cast function differs from the datatype's I/O functions.  Otherwise,
 * parse_coerce.c will generate CoerceViaIO operations without any prompting.
 *
 * Note that the castcontext values specified here should be no stronger than
 * parse_coerce.c's automatic casts ('a' to text, 'e' from text) else odd
 * behavior will ensue when the automatic cast is applied instead of the
 * pg_cast entry!
 */
DATA(insert (  650	 25  730 a ));
DATA(insert (  869	 25  730 a ));
DATA(insert (	16	 25 2971 a ));
DATA(insert (  142	 25    0 a ));
DATA(insert (	25	142 2896 e ));

/*
 * Cross-category casts to and from VARCHAR
 *
 * We support all the same casts as for TEXT.
 */
DATA(insert (  650 1043  730 a ));
DATA(insert (  869 1043  730 a ));
DATA(insert (	16 1043 2971 a ));
DATA(insert (  142 1043    0 a ));
DATA(insert ( 1043	142 2896 e ));

/*
 * Cross-category casts to and from BPCHAR
 *
 * We support all the same casts as for TEXT.
 */
DATA(insert (  650 1042  730 a ));
DATA(insert (  869 1042  730 a ));
DATA(insert (	16 1042 2971 a ));
DATA(insert (  142 1042    0 a ));
DATA(insert ( 1042	142 2896 e ));

/*
 * Length-coercion functions
 */
DATA(insert ( 1042 1042  668 i ));
DATA(insert ( 1043 1043  669 i ));
DATA(insert ( 1083 1083 1968 i ));
DATA(insert ( 1114 1114 1961 i ));
DATA(insert ( 1184 1184 1967 i ));
DATA(insert ( 1186 1186 1200 i ));
DATA(insert ( 1266 1266 1969 i ));
DATA(insert ( 1560 1560 1685 i ));
DATA(insert ( 1562 1562 1687 i ));
DATA(insert ( 1700 1700 1703 i ));

#endif   /* PG_CAST_H */
