/*-------------------------------------------------------------------------
 *
 * pg_amop.h
 *	  definition of the system "amop" relation (pg_amop)
 *	  along with the relation's initial contents.
 *
 * The amop table identifies the operators associated with each index operator
 * family and operator class (classes are subsets of families).
 *
 * The primary key for this table is <amopfamily, amoplefttype, amoprighttype,
 * amopstrategy>.  amoplefttype and amoprighttype are just copies of the
 * operator's oprleft/oprright, ie its declared input data types.  The
 * "default" operators for a particular opclass within the family are those
 * with amoplefttype = amoprighttype = opclass's opcintype.  An opfamily may
 * also contain other operators, typically cross-data-type operators.  All the
 * operators within a family are supposed to be compatible, in a way that is
 * defined by each individual index AM.
 *
 * We also keep a unique index on <amopfamily, amopopr>, so that we can use a
 * syscache to quickly answer questions of the form "is this operator in this
 * opfamily, and if so what are its semantics with respect to the family?"
 * This implies that the same operator cannot be listed for multiple strategy
 * numbers within a single opfamily.
 *
 * amopmethod is a copy of the owning opfamily's opfmethod field.  This is an
 * intentional denormalization of the catalogs to buy lookup speed.
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/catalog/pg_amop.h,v 1.93 2010/01/14 16:31:09 teodor Exp $
 *
 * NOTES
 *	 the genbki.pl script reads this file and generates .bki
 *	 information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_AMOP_H
#define PG_AMOP_H

#include "catalog/genbki.h"

/* ----------------
 *		pg_amop definition.  cpp turns this into
 *		typedef struct FormData_pg_amop
 * ----------------
 */
#define AccessMethodOperatorRelationId	2602

CATALOG(pg_amop,2602)
{
	Oid			amopfamily;		/* the index opfamily this entry is for */
	Oid			amoplefttype;	/* operator's left input data type */
	Oid			amoprighttype;	/* operator's right input data type */
	int2		amopstrategy;	/* operator strategy number */
	Oid			amopopr;		/* the operator's pg_operator OID */
	Oid			amopmethod;		/* the index access method this entry is for */
} FormData_pg_amop;

/* ----------------
 *		Form_pg_amop corresponds to a pointer to a tuple with
 *		the format of pg_amop relation.
 * ----------------
 */
typedef FormData_pg_amop *Form_pg_amop;

/* ----------------
 *		compiler constants for pg_amop
 * ----------------
 */
#define Natts_pg_amop					6
#define Anum_pg_amop_amopfamily			1
#define Anum_pg_amop_amoplefttype		2
#define Anum_pg_amop_amoprighttype		3
#define Anum_pg_amop_amopstrategy		4
#define Anum_pg_amop_amopopr			5
#define Anum_pg_amop_amopmethod			6

/* ----------------
 *		initial contents of pg_amop
 * ----------------
 */

/*
 *	btree integer_ops
 */

/* default operators int2 */
DATA(insert (	1976   21 21 1	95	403 ));
DATA(insert (	1976   21 21 2	522 403 ));
DATA(insert (	1976   21 21 3	94	403 ));
DATA(insert (	1976   21 21 4	524 403 ));
DATA(insert (	1976   21 21 5	520 403 ));
/* crosstype operators int24 */
DATA(insert (	1976   21 23 1	534 403 ));
DATA(insert (	1976   21 23 2	540 403 ));
DATA(insert (	1976   21 23 3	532 403 ));
DATA(insert (	1976   21 23 4	542 403 ));
DATA(insert (	1976   21 23 5	536 403 ));
/* crosstype operators int28 */
DATA(insert (	1976   21 20 1	1864	403 ));
DATA(insert (	1976   21 20 2	1866	403 ));
DATA(insert (	1976   21 20 3	1862	403 ));
DATA(insert (	1976   21 20 4	1867	403 ));
DATA(insert (	1976   21 20 5	1865	403 ));
/* default operators int4 */
DATA(insert (	1976   23 23 1	97	403 ));
DATA(insert (	1976   23 23 2	523 403 ));
DATA(insert (	1976   23 23 3	96	403 ));
DATA(insert (	1976   23 23 4	525 403 ));
DATA(insert (	1976   23 23 5	521 403 ));
/* crosstype operators int42 */
DATA(insert (	1976   23 21 1	535 403 ));
DATA(insert (	1976   23 21 2	541 403 ));
DATA(insert (	1976   23 21 3	533 403 ));
DATA(insert (	1976   23 21 4	543 403 ));
DATA(insert (	1976   23 21 5	537 403 ));
/* crosstype operators int48 */
DATA(insert (	1976   23 20 1	37	403 ));
DATA(insert (	1976   23 20 2	80	403 ));
DATA(insert (	1976   23 20 3	15	403 ));
DATA(insert (	1976   23 20 4	82	403 ));
DATA(insert (	1976   23 20 5	76	403 ));
/* default operators int8 */
DATA(insert (	1976   20 20 1	412 403 ));
DATA(insert (	1976   20 20 2	414 403 ));
DATA(insert (	1976   20 20 3	410 403 ));
DATA(insert (	1976   20 20 4	415 403 ));
DATA(insert (	1976   20 20 5	413 403 ));
/* crosstype operators int82 */
DATA(insert (	1976   20 21 1	1870	403 ));
DATA(insert (	1976   20 21 2	1872	403 ));
DATA(insert (	1976   20 21 3	1868	403 ));
DATA(insert (	1976   20 21 4	1873	403 ));
DATA(insert (	1976   20 21 5	1871	403 ));
/* crosstype operators int84 */
DATA(insert (	1976   20 23 1	418 403 ));
DATA(insert (	1976   20 23 2	420 403 ));
DATA(insert (	1976   20 23 3	416 403 ));
DATA(insert (	1976   20 23 4	430 403 ));
DATA(insert (	1976   20 23 5	419 403 ));

/*
 *	btree oid_ops
 */

DATA(insert (	1989   26 26 1	609 403 ));
DATA(insert (	1989   26 26 2	611 403 ));
DATA(insert (	1989   26 26 3	607 403 ));
DATA(insert (	1989   26 26 4	612 403 ));
DATA(insert (	1989   26 26 5	610 403 ));

/*
 * btree tid_ops
 */

DATA(insert (	2789   27 27 1 2799 403 ));
DATA(insert (	2789   27 27 2 2801 403 ));
DATA(insert (	2789   27 27 3 387	403 ));
DATA(insert (	2789   27 27 4 2802 403 ));
DATA(insert (	2789   27 27 5 2800 403 ));

/*
 *	btree oidvector_ops
 */

DATA(insert (	1991   30 30 1	645 403 ));
DATA(insert (	1991   30 30 2	647 403 ));
DATA(insert (	1991   30 30 3	649 403 ));
DATA(insert (	1991   30 30 4	648 403 ));
DATA(insert (	1991   30 30 5	646 403 ));

/*
 *	btree float_ops
 */

/* default operators float4 */
DATA(insert (	1970   700 700 1	622 403 ));
DATA(insert (	1970   700 700 2	624 403 ));
DATA(insert (	1970   700 700 3	620 403 ));
DATA(insert (	1970   700 700 4	625 403 ));
DATA(insert (	1970   700 700 5	623 403 ));
/* crosstype operators float48 */
DATA(insert (	1970   700 701 1	1122 403 ));
DATA(insert (	1970   700 701 2	1124 403 ));
DATA(insert (	1970   700 701 3	1120 403 ));
DATA(insert (	1970   700 701 4	1125 403 ));
DATA(insert (	1970   700 701 5	1123 403 ));
/* default operators float8 */
DATA(insert (	1970   701 701 1	672 403 ));
DATA(insert (	1970   701 701 2	673 403 ));
DATA(insert (	1970   701 701 3	670 403 ));
DATA(insert (	1970   701 701 4	675 403 ));
DATA(insert (	1970   701 701 5	674 403 ));
/* crosstype operators float84 */
DATA(insert (	1970   701 700 1	1132 403 ));
DATA(insert (	1970   701 700 2	1134 403 ));
DATA(insert (	1970   701 700 3	1130 403 ));
DATA(insert (	1970   701 700 4	1135 403 ));
DATA(insert (	1970   701 700 5	1133 403 ));

/*
 *	btree char_ops
 */

DATA(insert (	429   18 18 1  631	403 ));
DATA(insert (	429   18 18 2  632	403 ));
DATA(insert (	429   18 18 3 92	403 ));
DATA(insert (	429   18 18 4  634	403 ));
DATA(insert (	429   18 18 5  633	403 ));

/*
 *	btree name_ops
 */

DATA(insert (	1986   19 19 1	660 403 ));
DATA(insert (	1986   19 19 2	661 403 ));
DATA(insert (	1986   19 19 3	93	403 ));
DATA(insert (	1986   19 19 4	663 403 ));
DATA(insert (	1986   19 19 5	662 403 ));

/*
 *	btree text_ops
 */

DATA(insert (	1994   25 25 1	664 403 ));
DATA(insert (	1994   25 25 2	665 403 ));
DATA(insert (	1994   25 25 3	98	403 ));
DATA(insert (	1994   25 25 4	667 403 ));
DATA(insert (	1994   25 25 5	666 403 ));

/*
 *	btree bpchar_ops
 */

DATA(insert (	426   1042 1042 1 1058	403 ));
DATA(insert (	426   1042 1042 2 1059	403 ));
DATA(insert (	426   1042 1042 3 1054	403 ));
DATA(insert (	426   1042 1042 4 1061	403 ));
DATA(insert (	426   1042 1042 5 1060	403 ));

/*
 *	btree bytea_ops
 */

DATA(insert (	428   17 17 1 1957	403 ));
DATA(insert (	428   17 17 2 1958	403 ));
DATA(insert (	428   17 17 3 1955	403 ));
DATA(insert (	428   17 17 4 1960	403 ));
DATA(insert (	428   17 17 5 1959	403 ));

/*
 *	btree abstime_ops
 */

DATA(insert (	421   702 702 1  562	403 ));
DATA(insert (	421   702 702 2  564	403 ));
DATA(insert (	421   702 702 3  560	403 ));
DATA(insert (	421   702 702 4  565	403 ));
DATA(insert (	421   702 702 5  563	403 ));

/*
 *	btree datetime_ops
 */

/* default operators date */
DATA(insert (	434   1082 1082 1 1095	403 ));
DATA(insert (	434   1082 1082 2 1096	403 ));
DATA(insert (	434   1082 1082 3 1093	403 ));
DATA(insert (	434   1082 1082 4 1098	403 ));
DATA(insert (	434   1082 1082 5 1097	403 ));
/* crosstype operators vs timestamp */
DATA(insert (	434   1082 1114 1 2345	403 ));
DATA(insert (	434   1082 1114 2 2346	403 ));
DATA(insert (	434   1082 1114 3 2347	403 ));
DATA(insert (	434   1082 1114 4 2348	403 ));
DATA(insert (	434   1082 1114 5 2349	403 ));
/* crosstype operators vs timestamptz */
DATA(insert (	434   1082 1184 1 2358	403 ));
DATA(insert (	434   1082 1184 2 2359	403 ));
DATA(insert (	434   1082 1184 3 2360	403 ));
DATA(insert (	434   1082 1184 4 2361	403 ));
DATA(insert (	434   1082 1184 5 2362	403 ));
/* default operators timestamp */
DATA(insert (	434   1114 1114 1 2062	403 ));
DATA(insert (	434   1114 1114 2 2063	403 ));
DATA(insert (	434   1114 1114 3 2060	403 ));
DATA(insert (	434   1114 1114 4 2065	403 ));
DATA(insert (	434   1114 1114 5 2064	403 ));
/* crosstype operators vs date */
DATA(insert (	434   1114 1082 1 2371	403 ));
DATA(insert (	434   1114 1082 2 2372	403 ));
DATA(insert (	434   1114 1082 3 2373	403 ));
DATA(insert (	434   1114 1082 4 2374	403 ));
DATA(insert (	434   1114 1082 5 2375	403 ));
/* crosstype operators vs timestamptz */
DATA(insert (	434   1114 1184 1 2534	403 ));
DATA(insert (	434   1114 1184 2 2535	403 ));
DATA(insert (	434   1114 1184 3 2536	403 ));
DATA(insert (	434   1114 1184 4 2537	403 ));
DATA(insert (	434   1114 1184 5 2538	403 ));
/* default operators timestamptz */
DATA(insert (	434   1184 1184 1 1322	403 ));
DATA(insert (	434   1184 1184 2 1323	403 ));
DATA(insert (	434   1184 1184 3 1320	403 ));
DATA(insert (	434   1184 1184 4 1325	403 ));
DATA(insert (	434   1184 1184 5 1324	403 ));
/* crosstype operators vs date */
DATA(insert (	434   1184 1082 1 2384	403 ));
DATA(insert (	434   1184 1082 2 2385	403 ));
DATA(insert (	434   1184 1082 3 2386	403 ));
DATA(insert (	434   1184 1082 4 2387	403 ));
DATA(insert (	434   1184 1082 5 2388	403 ));
/* crosstype operators vs timestamp */
DATA(insert (	434   1184 1114 1 2540	403 ));
DATA(insert (	434   1184 1114 2 2541	403 ));
DATA(insert (	434   1184 1114 3 2542	403 ));
DATA(insert (	434   1184 1114 4 2543	403 ));
DATA(insert (	434   1184 1114 5 2544	403 ));

/*
 *	btree time_ops
 */

DATA(insert (	1996   1083 1083 1 1110 403 ));
DATA(insert (	1996   1083 1083 2 1111 403 ));
DATA(insert (	1996   1083 1083 3 1108 403 ));
DATA(insert (	1996   1083 1083 4 1113 403 ));
DATA(insert (	1996   1083 1083 5 1112 403 ));

/*
 *	btree timetz_ops
 */

DATA(insert (	2000   1266 1266 1 1552 403 ));
DATA(insert (	2000   1266 1266 2 1553 403 ));
DATA(insert (	2000   1266 1266 3 1550 403 ));
DATA(insert (	2000   1266 1266 4 1555 403 ));
DATA(insert (	2000   1266 1266 5 1554 403 ));

/*
 *	btree interval_ops
 */

DATA(insert (	1982   1186 1186 1 1332 403 ));
DATA(insert (	1982   1186 1186 2 1333 403 ));
DATA(insert (	1982   1186 1186 3 1330 403 ));
DATA(insert (	1982   1186 1186 4 1335 403 ));
DATA(insert (	1982   1186 1186 5 1334 403 ));

/*
 *	btree macaddr
 */

DATA(insert (	1984   829 829 1 1222 403 ));
DATA(insert (	1984   829 829 2 1223 403 ));
DATA(insert (	1984   829 829 3 1220 403 ));
DATA(insert (	1984   829 829 4 1225 403 ));
DATA(insert (	1984   829 829 5 1224 403 ));

/*
 *	btree network
 */

DATA(insert (	1974   869 869 1 1203 403 ));
DATA(insert (	1974   869 869 2 1204 403 ));
DATA(insert (	1974   869 869 3 1201 403 ));
DATA(insert (	1974   869 869 4 1206 403 ));
DATA(insert (	1974   869 869 5 1205 403 ));

/*
 *	btree numeric
 */

DATA(insert (	1988   1700 1700 1 1754 403 ));
DATA(insert (	1988   1700 1700 2 1755 403 ));
DATA(insert (	1988   1700 1700 3 1752 403 ));
DATA(insert (	1988   1700 1700 4 1757 403 ));
DATA(insert (	1988   1700 1700 5 1756 403 ));

/*
 *	btree bool
 */

DATA(insert (	424   16 16 1 58	403 ));
DATA(insert (	424   16 16 2 1694	403 ));
DATA(insert (	424   16 16 3 91	403 ));
DATA(insert (	424   16 16 4 1695	403 ));
DATA(insert (	424   16 16 5 59	403 ));

/*
 *	btree bit
 */

DATA(insert (	423   1560 1560 1 1786	403 ));
DATA(insert (	423   1560 1560 2 1788	403 ));
DATA(insert (	423   1560 1560 3 1784	403 ));
DATA(insert (	423   1560 1560 4 1789	403 ));
DATA(insert (	423   1560 1560 5 1787	403 ));

/*
 *	btree varbit
 */

DATA(insert (	2002   1562 1562 1 1806 403 ));
DATA(insert (	2002   1562 1562 2 1808 403 ));
DATA(insert (	2002   1562 1562 3 1804 403 ));
DATA(insert (	2002   1562 1562 4 1809 403 ));
DATA(insert (	2002   1562 1562 5 1807 403 ));

/*
 *	btree text pattern
 */

DATA(insert (	2095   25 25 1 2314 403 ));
DATA(insert (	2095   25 25 2 2315 403 ));
DATA(insert (	2095   25 25 3 98	403 ));
DATA(insert (	2095   25 25 4 2317 403 ));
DATA(insert (	2095   25 25 5 2318 403 ));

/*
 *	btree bpchar pattern
 */

DATA(insert (	2097   1042 1042 1 2326 403 ));
DATA(insert (	2097   1042 1042 2 2327 403 ));
DATA(insert (	2097   1042 1042 3 1054 403 ));
DATA(insert (	2097   1042 1042 4 2329 403 ));
DATA(insert (	2097   1042 1042 5 2330 403 ));

/*
 *	btree money_ops
 */

DATA(insert (	2099   790 790 1	902 403 ));
DATA(insert (	2099   790 790 2	904 403 ));
DATA(insert (	2099   790 790 3	900 403 ));
DATA(insert (	2099   790 790 4	905 403 ));
DATA(insert (	2099   790 790 5	903 403 ));

/*
 *	btree reltime_ops
 */

DATA(insert (	2233   703 703 1	568 403 ));
DATA(insert (	2233   703 703 2	570 403 ));
DATA(insert (	2233   703 703 3	566 403 ));
DATA(insert (	2233   703 703 4	571 403 ));
DATA(insert (	2233   703 703 5	569 403 ));

/*
 *	btree tinterval_ops
 */

DATA(insert (	2234   704 704 1	813 403 ));
DATA(insert (	2234   704 704 2	815 403 ));
DATA(insert (	2234   704 704 3	811 403 ));
DATA(insert (	2234   704 704 4	816 403 ));
DATA(insert (	2234   704 704 5	814 403 ));

/*
 *	btree array_ops
 */

DATA(insert (	397   2277 2277 1 1072	403 ));
DATA(insert (	397   2277 2277 2 1074	403 ));
DATA(insert (	397   2277 2277 3 1070	403 ));
DATA(insert (	397   2277 2277 4 1075	403 ));
DATA(insert (	397   2277 2277 5 1073	403 ));

/*
 *	btree record_ops
 */

DATA(insert (	2994  2249 2249 1 2990	403 ));
DATA(insert (	2994  2249 2249 2 2992	403 ));
DATA(insert (	2994  2249 2249 3 2988	403 ));
DATA(insert (	2994  2249 2249 4 2993	403 ));
DATA(insert (	2994  2249 2249 5 2991	403 ));

/*
 * btree uuid_ops
 */

DATA(insert (	2968  2950 2950 1 2974	403 ));
DATA(insert (	2968  2950 2950 2 2976	403 ));
DATA(insert (	2968  2950 2950 3 2972	403 ));
DATA(insert (	2968  2950 2950 4 2977	403 ));
DATA(insert (	2968  2950 2950 5 2975	403 ));

/*
 *	hash index _ops
 */

/* bpchar_ops */
DATA(insert (	427   1042 1042 1 1054	405 ));
/* char_ops */
DATA(insert (	431   18 18 1 92	405 ));
/* date_ops */
DATA(insert (	435   1082 1082 1 1093	405 ));
/* float_ops */
DATA(insert (	1971   700 700 1	620 405 ));
DATA(insert (	1971   701 701 1	670 405 ));
DATA(insert (	1971   700 701 1 1120 405 ));
DATA(insert (	1971   701 700 1 1130 405 ));
/* network_ops */
DATA(insert (	1975   869 869 1 1201 405 ));
/* integer_ops */
DATA(insert (	1977   21 21 1	94	405 ));
DATA(insert (	1977   23 23 1	96	405 ));
DATA(insert (	1977   20 20 1	410 405 ));
DATA(insert (	1977   21 23 1	532 405 ));
DATA(insert (	1977   21 20 1	 1862 405 ));
DATA(insert (	1977   23 21 1	533 405 ));
DATA(insert (	1977   23 20 1	15	405 ));
DATA(insert (	1977   20 21 1	 1868 405 ));
DATA(insert (	1977   20 23 1	416 405 ));
/* interval_ops */
DATA(insert (	1983   1186 1186 1 1330 405 ));
/* macaddr_ops */
DATA(insert (	1985   829 829 1 1220 405 ));
/* name_ops */
DATA(insert (	1987   19 19 1	93	405 ));
/* oid_ops */
DATA(insert (	1990   26 26 1	607 405 ));
/* oidvector_ops */
DATA(insert (	1992   30 30 1	649 405 ));
/* text_ops */
DATA(insert (	1995   25 25 1	98	405 ));
/* time_ops */
DATA(insert (	1997   1083 1083 1 1108 405 ));
/* timestamptz_ops */
DATA(insert (	1999   1184 1184 1 1320 405 ));
/* timetz_ops */
DATA(insert (	2001   1266 1266 1 1550 405 ));
/* timestamp_ops */
DATA(insert (	2040   1114 1114 1 2060 405 ));
/* bool_ops */
DATA(insert (	2222   16 16 1	91	405 ));
/* bytea_ops */
DATA(insert (	2223   17 17 1 1955 405 ));
/* int2vector_ops */
DATA(insert (	2224   22 22 1	386 405 ));
/* xid_ops */
DATA(insert (	2225   28 28 1	352 405 ));
/* cid_ops */
DATA(insert (	2226   29 29 1	385 405 ));
/* abstime_ops */
DATA(insert (	2227   702 702 1	560 405 ));
/* reltime_ops */
DATA(insert (	2228   703 703 1	566 405 ));
/* text_pattern_ops */
DATA(insert (	2229   25 25 1 98	405 ));
/* bpchar_pattern_ops */
DATA(insert (	2231   1042 1042 1 1054 405 ));
/* aclitem_ops */
DATA(insert (	2235   1033 1033 1	974 405 ));
/* uuid_ops */
DATA(insert (	2969   2950 2950 1 2972 405 ));
/* numeric_ops */
DATA(insert (	1998   1700 1700 1 1752 405 ));


/*
 *	gist box_ops
 */

DATA(insert (	2593   603 603 1  493 783 ));
DATA(insert (	2593   603 603 2  494 783 ));
DATA(insert (	2593   603 603 3  500 783 ));
DATA(insert (	2593   603 603 4  495 783 ));
DATA(insert (	2593   603 603 5  496 783 ));
DATA(insert (	2593   603 603 6  499 783 ));
DATA(insert (	2593   603 603 7  498 783 ));
DATA(insert (	2593   603 603 8  497 783 ));
DATA(insert (	2593   603 603 9  2571	783 ));
DATA(insert (	2593   603 603 10 2570	783 ));
DATA(insert (	2593   603 603 11 2573	783 ));
DATA(insert (	2593   603 603 12 2572	783 ));
DATA(insert (	2593   603 603 13 2863	783 ));
DATA(insert (	2593   603 603 14 2862	783 ));

/*
 * gist point_ops
 */
DATA(insert (	1029   600 600 11 506 783 ));
DATA(insert (	1029   600 600 1  507 783 ));
DATA(insert (	1029   600 600 5  508 783 ));
DATA(insert (	1029   600 600 10 509 783 ));
DATA(insert (	1029   600 600 6  510 783 ));
DATA(insert (	1029   603 600 27  433 783 ));
DATA(insert (	1029   600 603 28  511 783 ));
DATA(insert (	1029   604 600 47 757 783 ));
DATA(insert (	1029   600 604 48 756 783 ));
DATA(insert (	1029   718 600 67 759 783 ));
DATA(insert (	1029   600 718 68 758 783 ));


/*
 *	gist poly_ops (supports polygons)
 */

DATA(insert (	2594   604 604 1  485 783 ));
DATA(insert (	2594   604 604 2  486 783 ));
DATA(insert (	2594   604 604 3  492 783 ));
DATA(insert (	2594   604 604 4  487 783 ));
DATA(insert (	2594   604 604 5  488 783 ));
DATA(insert (	2594   604 604 6  491 783 ));
DATA(insert (	2594   604 604 7  490 783 ));
DATA(insert (	2594   604 604 8  489 783 ));
DATA(insert (	2594   604 604 9  2575	783 ));
DATA(insert (	2594   604 604 10 2574	783 ));
DATA(insert (	2594   604 604 11 2577	783 ));
DATA(insert (	2594   604 604 12 2576	783 ));
DATA(insert (	2594   604 604 13 2861	783 ));
DATA(insert (	2594   604 604 14 2860	783 ));

/*
 *	gist circle_ops
 */

DATA(insert (	2595   718 718 1  1506	783 ));
DATA(insert (	2595   718 718 2  1507	783 ));
DATA(insert (	2595   718 718 3  1513	783 ));
DATA(insert (	2595   718 718 4  1508	783 ));
DATA(insert (	2595   718 718 5  1509	783 ));
DATA(insert (	2595   718 718 6  1512	783 ));
DATA(insert (	2595   718 718 7  1511	783 ));
DATA(insert (	2595   718 718 8  1510	783 ));
DATA(insert (	2595   718 718 9  2589	783 ));
DATA(insert (	2595   718 718 10 1515	783 ));
DATA(insert (	2595   718 718 11 1514	783 ));
DATA(insert (	2595   718 718 12 2590	783 ));
DATA(insert (	2595   718 718 13 2865	783 ));
DATA(insert (	2595   718 718 14 2864	783 ));

/*
 * gin array_ops (these anyarray operators are used with all the opclasses
 * of the family)
 */
DATA(insert (	2745   2277 2277 1	2750	2742 ));
DATA(insert (	2745   2277 2277 2	2751	2742 ));
DATA(insert (	2745   2277 2277 3	2752	2742 ));
DATA(insert (	2745   2277 2277 4	1070	2742 ));

/*
 * btree enum_ops
 */
DATA(insert (	3522   3500 3500 1	3518	403 ));
DATA(insert (	3522   3500 3500 2	3520	403 ));
DATA(insert (	3522   3500 3500 3	3516	403 ));
DATA(insert (	3522   3500 3500 4	3521	403 ));
DATA(insert (	3522   3500 3500 5	3519	403 ));

/*
 * hash enum_ops
 */
DATA(insert (	3523   3500 3500 1	3516	405 ));

/*
 * btree tsvector_ops
 */
DATA(insert (	3626   3614 3614 1	 3627 403 ));
DATA(insert (	3626   3614 3614 2	 3628 403 ));
DATA(insert (	3626   3614 3614 3	 3629 403 ));
DATA(insert (	3626   3614 3614 4	 3631 403 ));
DATA(insert (	3626   3614 3614 5	 3632 403 ));

/*
 * GiST tsvector_ops
 */
DATA(insert (	3655   3614 3615 1	3636 783 ));

/*
 * GIN tsvector_ops
 */
DATA(insert (	3659   3614 3615 1	3636 2742 ));
DATA(insert (	3659   3614 3615 2	3660 2742 ));

/*
 * btree tsquery_ops
 */
DATA(insert (	3683   3615 3615 1	 3674 403 ));
DATA(insert (	3683   3615 3615 2	 3675 403 ));
DATA(insert (	3683   3615 3615 3	 3676 403 ));
DATA(insert (	3683   3615 3615 4	 3678 403 ));
DATA(insert (	3683   3615 3615 5	 3679 403 ));

/*
 * GiST tsquery_ops
 */
DATA(insert (	3702   3615 3615 7	3693 783 ));
DATA(insert (	3702   3615 3615 8	3694 783 ));

#endif   /* PG_AMOP_H */
