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
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/catalog/pg_amop.h,v 1.84 2008/01/01 19:45:56 momjian Exp $
 *
 * NOTES
 *	 the genbki.sh script reads this file and generates .bki
 *	 information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_AMOP_H
#define PG_AMOP_H

/* ----------------
 *		postgres.h contains the system type definitions and the
 *		CATALOG(), BKI_BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

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
	bool		amopreqcheck;	/* index hit must be rechecked */
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
#define Natts_pg_amop					7
#define Anum_pg_amop_amopfamily			1
#define Anum_pg_amop_amoplefttype		2
#define Anum_pg_amop_amoprighttype		3
#define Anum_pg_amop_amopstrategy		4
#define Anum_pg_amop_amopreqcheck		5
#define Anum_pg_amop_amopopr			6
#define Anum_pg_amop_amopmethod			7

/* ----------------
 *		initial contents of pg_amop
 * ----------------
 */

/*
 *	btree integer_ops
 */

/* default operators int2 */
DATA(insert (	1976   21 21 1 f  95	403 ));
DATA(insert (	1976   21 21 2 f  522	403 ));
DATA(insert (	1976   21 21 3 f  94	403 ));
DATA(insert (	1976   21 21 4 f  524	403 ));
DATA(insert (	1976   21 21 5 f  520	403 ));
/* crosstype operators int24 */
DATA(insert (	1976   21 23 1 f  534	403 ));
DATA(insert (	1976   21 23 2 f  540	403 ));
DATA(insert (	1976   21 23 3 f  532	403 ));
DATA(insert (	1976   21 23 4 f  542	403 ));
DATA(insert (	1976   21 23 5 f  536	403 ));
/* crosstype operators int28 */
DATA(insert (	1976   21 20 1 f  1864	403 ));
DATA(insert (	1976   21 20 2 f  1866	403 ));
DATA(insert (	1976   21 20 3 f  1862	403 ));
DATA(insert (	1976   21 20 4 f  1867	403 ));
DATA(insert (	1976   21 20 5 f  1865	403 ));
/* default operators int4 */
DATA(insert (	1976   23 23 1 f  97	403 ));
DATA(insert (	1976   23 23 2 f  523	403 ));
DATA(insert (	1976   23 23 3 f  96	403 ));
DATA(insert (	1976   23 23 4 f  525	403 ));
DATA(insert (	1976   23 23 5 f  521	403 ));
/* crosstype operators int42 */
DATA(insert (	1976   23 21 1 f  535	403 ));
DATA(insert (	1976   23 21 2 f  541	403 ));
DATA(insert (	1976   23 21 3 f  533	403 ));
DATA(insert (	1976   23 21 4 f  543	403 ));
DATA(insert (	1976   23 21 5 f  537	403 ));
/* crosstype operators int48 */
DATA(insert (	1976   23 20 1 f  37	403 ));
DATA(insert (	1976   23 20 2 f  80	403 ));
DATA(insert (	1976   23 20 3 f  15	403 ));
DATA(insert (	1976   23 20 4 f  82	403 ));
DATA(insert (	1976   23 20 5 f  76	403 ));
/* default operators int8 */
DATA(insert (	1976   20 20 1 f  412	403 ));
DATA(insert (	1976   20 20 2 f  414	403 ));
DATA(insert (	1976   20 20 3 f  410	403 ));
DATA(insert (	1976   20 20 4 f  415	403 ));
DATA(insert (	1976   20 20 5 f  413	403 ));
/* crosstype operators int82 */
DATA(insert (	1976   20 21 1 f  1870	403 ));
DATA(insert (	1976   20 21 2 f  1872	403 ));
DATA(insert (	1976   20 21 3 f  1868	403 ));
DATA(insert (	1976   20 21 4 f  1873	403 ));
DATA(insert (	1976   20 21 5 f  1871	403 ));
/* crosstype operators int84 */
DATA(insert (	1976   20 23 1 f  418	403 ));
DATA(insert (	1976   20 23 2 f  420	403 ));
DATA(insert (	1976   20 23 3 f  416	403 ));
DATA(insert (	1976   20 23 4 f  430	403 ));
DATA(insert (	1976   20 23 5 f  419	403 ));

/*
 *	btree oid_ops
 */

DATA(insert (	1989   26 26 1 f  609	403 ));
DATA(insert (	1989   26 26 2 f  611	403 ));
DATA(insert (	1989   26 26 3 f  607	403 ));
DATA(insert (	1989   26 26 4 f  612	403 ));
DATA(insert (	1989   26 26 5 f  610	403 ));

/*
 * btree tid_ops
 */

DATA(insert (	2789   27 27 1 f 2799	403 ));
DATA(insert (	2789   27 27 2 f 2801	403 ));
DATA(insert (	2789   27 27 3 f 387	403 ));
DATA(insert (	2789   27 27 4 f 2802	403 ));
DATA(insert (	2789   27 27 5 f 2800	403 ));

/*
 *	btree oidvector_ops
 */

DATA(insert (	1991   30 30 1 f  645	403 ));
DATA(insert (	1991   30 30 2 f  647	403 ));
DATA(insert (	1991   30 30 3 f  649	403 ));
DATA(insert (	1991   30 30 4 f  648	403 ));
DATA(insert (	1991   30 30 5 f  646	403 ));

/*
 *	btree float_ops
 */

/* default operators float4 */
DATA(insert (	1970   700 700 1 f	622 403 ));
DATA(insert (	1970   700 700 2 f	624 403 ));
DATA(insert (	1970   700 700 3 f	620 403 ));
DATA(insert (	1970   700 700 4 f	625 403 ));
DATA(insert (	1970   700 700 5 f	623 403 ));
/* crosstype operators float48 */
DATA(insert (	1970   700 701 1 f	1122 403 ));
DATA(insert (	1970   700 701 2 f	1124 403 ));
DATA(insert (	1970   700 701 3 f	1120 403 ));
DATA(insert (	1970   700 701 4 f	1125 403 ));
DATA(insert (	1970   700 701 5 f	1123 403 ));
/* default operators float8 */
DATA(insert (	1970   701 701 1 f	672 403 ));
DATA(insert (	1970   701 701 2 f	673 403 ));
DATA(insert (	1970   701 701 3 f	670 403 ));
DATA(insert (	1970   701 701 4 f	675 403 ));
DATA(insert (	1970   701 701 5 f	674 403 ));
/* crosstype operators float84 */
DATA(insert (	1970   701 700 1 f	1132 403 ));
DATA(insert (	1970   701 700 2 f	1134 403 ));
DATA(insert (	1970   701 700 3 f	1130 403 ));
DATA(insert (	1970   701 700 4 f	1135 403 ));
DATA(insert (	1970   701 700 5 f	1133 403 ));

/*
 *	btree char_ops
 */

DATA(insert (	429   18 18 1 f  631	403 ));
DATA(insert (	429   18 18 2 f  632	403 ));
DATA(insert (	429   18 18 3 f 92	403 ));
DATA(insert (	429   18 18 4 f  634	403 ));
DATA(insert (	429   18 18 5 f  633	403 ));

/*
 *	btree name_ops
 */

DATA(insert (	1986   19 19 1 f  660	403 ));
DATA(insert (	1986   19 19 2 f  661	403 ));
DATA(insert (	1986   19 19 3 f	93	403 ));
DATA(insert (	1986   19 19 4 f  663	403 ));
DATA(insert (	1986   19 19 5 f  662	403 ));

/*
 *	btree text_ops
 */

DATA(insert (	1994   25 25 1 f  664	403 ));
DATA(insert (	1994   25 25 2 f  665	403 ));
DATA(insert (	1994   25 25 3 f	98	403 ));
DATA(insert (	1994   25 25 4 f  667	403 ));
DATA(insert (	1994   25 25 5 f  666	403 ));

/*
 *	btree bpchar_ops
 */

DATA(insert (	426   1042 1042 1 f 1058	403 ));
DATA(insert (	426   1042 1042 2 f 1059	403 ));
DATA(insert (	426   1042 1042 3 f 1054	403 ));
DATA(insert (	426   1042 1042 4 f 1061	403 ));
DATA(insert (	426   1042 1042 5 f 1060	403 ));

/*
 *	btree bytea_ops
 */

DATA(insert (	428   17 17 1 f 1957	403 ));
DATA(insert (	428   17 17 2 f 1958	403 ));
DATA(insert (	428   17 17 3 f 1955	403 ));
DATA(insert (	428   17 17 4 f 1960	403 ));
DATA(insert (	428   17 17 5 f 1959	403 ));

/*
 *	btree abstime_ops
 */

DATA(insert (	421   702 702 1 f  562	403 ));
DATA(insert (	421   702 702 2 f  564	403 ));
DATA(insert (	421   702 702 3 f  560	403 ));
DATA(insert (	421   702 702 4 f  565	403 ));
DATA(insert (	421   702 702 5 f  563	403 ));

/*
 *	btree datetime_ops
 */

/* default operators date */
DATA(insert (	434   1082 1082 1 f 1095	403 ));
DATA(insert (	434   1082 1082 2 f 1096	403 ));
DATA(insert (	434   1082 1082 3 f 1093	403 ));
DATA(insert (	434   1082 1082 4 f 1098	403 ));
DATA(insert (	434   1082 1082 5 f 1097	403 ));
/* crosstype operators vs timestamp */
DATA(insert (	434   1082 1114 1 f 2345	403 ));
DATA(insert (	434   1082 1114 2 f 2346	403 ));
DATA(insert (	434   1082 1114 3 f 2347	403 ));
DATA(insert (	434   1082 1114 4 f 2348	403 ));
DATA(insert (	434   1082 1114 5 f 2349	403 ));
/* crosstype operators vs timestamptz */
DATA(insert (	434   1082 1184 1 f 2358	403 ));
DATA(insert (	434   1082 1184 2 f 2359	403 ));
DATA(insert (	434   1082 1184 3 f 2360	403 ));
DATA(insert (	434   1082 1184 4 f 2361	403 ));
DATA(insert (	434   1082 1184 5 f 2362	403 ));
/* default operators timestamp */
DATA(insert (	434   1114 1114 1 f 2062	403 ));
DATA(insert (	434   1114 1114 2 f 2063	403 ));
DATA(insert (	434   1114 1114 3 f 2060	403 ));
DATA(insert (	434   1114 1114 4 f 2065	403 ));
DATA(insert (	434   1114 1114 5 f 2064	403 ));
/* crosstype operators vs date */
DATA(insert (	434   1114 1082 1 f 2371	403 ));
DATA(insert (	434   1114 1082 2 f 2372	403 ));
DATA(insert (	434   1114 1082 3 f 2373	403 ));
DATA(insert (	434   1114 1082 4 f 2374	403 ));
DATA(insert (	434   1114 1082 5 f 2375	403 ));
/* crosstype operators vs timestamptz */
DATA(insert (	434   1114 1184 1 f 2534	403 ));
DATA(insert (	434   1114 1184 2 f 2535	403 ));
DATA(insert (	434   1114 1184 3 f 2536	403 ));
DATA(insert (	434   1114 1184 4 f 2537	403 ));
DATA(insert (	434   1114 1184 5 f 2538	403 ));
/* default operators timestamptz */
DATA(insert (	434   1184 1184 1 f 1322	403 ));
DATA(insert (	434   1184 1184 2 f 1323	403 ));
DATA(insert (	434   1184 1184 3 f 1320	403 ));
DATA(insert (	434   1184 1184 4 f 1325	403 ));
DATA(insert (	434   1184 1184 5 f 1324	403 ));
/* crosstype operators vs date */
DATA(insert (	434   1184 1082 1 f 2384	403 ));
DATA(insert (	434   1184 1082 2 f 2385	403 ));
DATA(insert (	434   1184 1082 3 f 2386	403 ));
DATA(insert (	434   1184 1082 4 f 2387	403 ));
DATA(insert (	434   1184 1082 5 f 2388	403 ));
/* crosstype operators vs timestamp */
DATA(insert (	434   1184 1114 1 f 2540	403 ));
DATA(insert (	434   1184 1114 2 f 2541	403 ));
DATA(insert (	434   1184 1114 3 f 2542	403 ));
DATA(insert (	434   1184 1114 4 f 2543	403 ));
DATA(insert (	434   1184 1114 5 f 2544	403 ));

/*
 *	btree time_ops
 */

DATA(insert (	1996   1083 1083 1 f 1110	403 ));
DATA(insert (	1996   1083 1083 2 f 1111	403 ));
DATA(insert (	1996   1083 1083 3 f 1108	403 ));
DATA(insert (	1996   1083 1083 4 f 1113	403 ));
DATA(insert (	1996   1083 1083 5 f 1112	403 ));

/*
 *	btree timetz_ops
 */

DATA(insert (	2000   1266 1266 1 f 1552	403 ));
DATA(insert (	2000   1266 1266 2 f 1553	403 ));
DATA(insert (	2000   1266 1266 3 f 1550	403 ));
DATA(insert (	2000   1266 1266 4 f 1555	403 ));
DATA(insert (	2000   1266 1266 5 f 1554	403 ));

/*
 *	btree interval_ops
 */

DATA(insert (	1982   1186 1186 1 f 1332	403 ));
DATA(insert (	1982   1186 1186 2 f 1333	403 ));
DATA(insert (	1982   1186 1186 3 f 1330	403 ));
DATA(insert (	1982   1186 1186 4 f 1335	403 ));
DATA(insert (	1982   1186 1186 5 f 1334	403 ));

/*
 *	btree macaddr
 */

DATA(insert (	1984   829 829 1 f 1222 403 ));
DATA(insert (	1984   829 829 2 f 1223 403 ));
DATA(insert (	1984   829 829 3 f 1220 403 ));
DATA(insert (	1984   829 829 4 f 1225 403 ));
DATA(insert (	1984   829 829 5 f 1224 403 ));

/*
 *	btree network
 */

DATA(insert (	1974   869 869 1 f 1203 403 ));
DATA(insert (	1974   869 869 2 f 1204 403 ));
DATA(insert (	1974   869 869 3 f 1201 403 ));
DATA(insert (	1974   869 869 4 f 1206 403 ));
DATA(insert (	1974   869 869 5 f 1205 403 ));

/*
 *	btree numeric
 */

DATA(insert (	1988   1700 1700 1 f 1754	403 ));
DATA(insert (	1988   1700 1700 2 f 1755	403 ));
DATA(insert (	1988   1700 1700 3 f 1752	403 ));
DATA(insert (	1988   1700 1700 4 f 1757	403 ));
DATA(insert (	1988   1700 1700 5 f 1756	403 ));

/*
 *	btree bool
 */

DATA(insert (	424   16 16 1 f 58	403 ));
DATA(insert (	424   16 16 2 f 1694	403 ));
DATA(insert (	424   16 16 3 f 91	403 ));
DATA(insert (	424   16 16 4 f 1695	403 ));
DATA(insert (	424   16 16 5 f 59	403 ));

/*
 *	btree bit
 */

DATA(insert (	423   1560 1560 1 f 1786	403 ));
DATA(insert (	423   1560 1560 2 f 1788	403 ));
DATA(insert (	423   1560 1560 3 f 1784	403 ));
DATA(insert (	423   1560 1560 4 f 1789	403 ));
DATA(insert (	423   1560 1560 5 f 1787	403 ));

/*
 *	btree varbit
 */

DATA(insert (	2002   1562 1562 1 f 1806	403 ));
DATA(insert (	2002   1562 1562 2 f 1808	403 ));
DATA(insert (	2002   1562 1562 3 f 1804	403 ));
DATA(insert (	2002   1562 1562 4 f 1809	403 ));
DATA(insert (	2002   1562 1562 5 f 1807	403 ));

/*
 *	btree text pattern
 */

DATA(insert (	2095   25 25 1 f 2314	403 ));
DATA(insert (	2095   25 25 2 f 2315	403 ));
DATA(insert (	2095   25 25 3 f 2316	403 ));
DATA(insert (	2095   25 25 4 f 2317	403 ));
DATA(insert (	2095   25 25 5 f 2318	403 ));

/*
 *	btree bpchar pattern
 */

DATA(insert (	2097   1042 1042 1 f 2326	403 ));
DATA(insert (	2097   1042 1042 2 f 2327	403 ));
DATA(insert (	2097   1042 1042 3 f 2328	403 ));
DATA(insert (	2097   1042 1042 4 f 2329	403 ));
DATA(insert (	2097   1042 1042 5 f 2330	403 ));

/*
 *	btree name pattern
 */

DATA(insert (	2098   19 19 1 f 2332	403 ));
DATA(insert (	2098   19 19 2 f 2333	403 ));
DATA(insert (	2098   19 19 3 f 2334	403 ));
DATA(insert (	2098   19 19 4 f 2335	403 ));
DATA(insert (	2098   19 19 5 f 2336	403 ));

/*
 *	btree money_ops
 */

DATA(insert (	2099   790 790 1 f	902 403 ));
DATA(insert (	2099   790 790 2 f	904 403 ));
DATA(insert (	2099   790 790 3 f	900 403 ));
DATA(insert (	2099   790 790 4 f	905 403 ));
DATA(insert (	2099   790 790 5 f	903 403 ));

/*
 *	btree reltime_ops
 */

DATA(insert (	2233   703 703 1 f	568 403 ));
DATA(insert (	2233   703 703 2 f	570 403 ));
DATA(insert (	2233   703 703 3 f	566 403 ));
DATA(insert (	2233   703 703 4 f	571 403 ));
DATA(insert (	2233   703 703 5 f	569 403 ));

/*
 *	btree tinterval_ops
 */

DATA(insert (	2234   704 704 1 f	813 403 ));
DATA(insert (	2234   704 704 2 f	815 403 ));
DATA(insert (	2234   704 704 3 f	811 403 ));
DATA(insert (	2234   704 704 4 f	816 403 ));
DATA(insert (	2234   704 704 5 f	814 403 ));

/*
 *	btree array_ops
 */

DATA(insert (	397   2277 2277 1 f 1072	403 ));
DATA(insert (	397   2277 2277 2 f 1074	403 ));
DATA(insert (	397   2277 2277 3 f 1070	403 ));
DATA(insert (	397   2277 2277 4 f 1075	403 ));
DATA(insert (	397   2277 2277 5 f 1073	403 ));

/*
 * btree uuid_ops
 */

DATA(insert (	2968  2950 2950 1 f 2974	403 ));
DATA(insert (	2968  2950 2950 2 f 2976	403 ));
DATA(insert (	2968  2950 2950 3 f 2972	403 ));
DATA(insert (	2968  2950 2950 4 f 2977	403 ));
DATA(insert (	2968  2950 2950 5 f 2975	403 ));

/*
 *	hash index _ops
 */

/* bpchar_ops */
DATA(insert (	427   1042 1042 1 f 1054	405 ));
/* char_ops */
DATA(insert (	431   18 18 1 f 92	405 ));
/* date_ops */
DATA(insert (	435   1082 1082 1 f 1093	405 ));
/* float_ops */
DATA(insert (	1971   700 700 1 f	620 405 ));
DATA(insert (	1971   701 701 1 f	670 405 ));
DATA(insert (	1971   700 701 1 f 1120 405 ));
DATA(insert (	1971   701 700 1 f 1130 405 ));
/* network_ops */
DATA(insert (	1975   869 869 1 f 1201 405 ));
/* integer_ops */
DATA(insert (	1977   21 21 1 f	94	405 ));
DATA(insert (	1977   23 23 1 f	96	405 ));
DATA(insert (	1977   20 20 1 f	410 405 ));
DATA(insert (	1977   21 23 1 f	532 405 ));
DATA(insert (	1977   21 20 1 f   1862 405 ));
DATA(insert (	1977   23 21 1 f	533 405 ));
DATA(insert (	1977   23 20 1 f	15	405 ));
DATA(insert (	1977   20 21 1 f   1868 405 ));
DATA(insert (	1977   20 23 1 f	416 405 ));
/* interval_ops */
DATA(insert (	1983   1186 1186 1 f 1330	405 ));
/* macaddr_ops */
DATA(insert (	1985   829 829 1 f 1220 405 ));
/* name_ops */
DATA(insert (	1987   19 19 1 f	93	405 ));
/* oid_ops */
DATA(insert (	1990   26 26 1 f  607	405 ));
/* oidvector_ops */
DATA(insert (	1992   30 30 1 f  649	405 ));
/* text_ops */
DATA(insert (	1995   25 25 1 f	98	405 ));
/* time_ops */
DATA(insert (	1997   1083 1083 1 f 1108	405 ));
/* timestamptz_ops */
DATA(insert (	1999   1184 1184 1 f 1320	405 ));
/* timetz_ops */
DATA(insert (	2001   1266 1266 1 f 1550	405 ));
/* timestamp_ops */
DATA(insert (	2040   1114 1114 1 f 2060	405 ));
/* bool_ops */
DATA(insert (	2222   16 16 1 f	91	405 ));
/* bytea_ops */
DATA(insert (	2223   17 17 1 f 1955	405 ));
/* int2vector_ops */
DATA(insert (	2224   22 22 1 f  386	405 ));
/* xid_ops */
DATA(insert (	2225   28 28 1 f  352	405 ));
/* cid_ops */
DATA(insert (	2226   29 29 1 f  385	405 ));
/* abstime_ops */
DATA(insert (	2227   702 702 1 f	560 405 ));
/* reltime_ops */
DATA(insert (	2228   703 703 1 f	566 405 ));
/* text_pattern_ops */
DATA(insert (	2229   25 25 1 f 2316	405 ));
/* bpchar_pattern_ops */
DATA(insert (	2231   1042 1042 1 f 2328	405 ));
/* name_pattern_ops */
DATA(insert (	2232   19 19 1 f 2334	405 ));
/* aclitem_ops */
DATA(insert (	2235   1033 1033 1 f  974	405 ));
/* uuid_ops */
DATA(insert (	2969   2950 2950 1 f 2972 405 ));
/* numeric_ops */
DATA(insert (	1998   1700 1700 1 f 1752 405 ));


/*
 *	gist box_ops
 */

DATA(insert (	2593   603 603 1  f 493 783 ));
DATA(insert (	2593   603 603 2  f 494 783 ));
DATA(insert (	2593   603 603 3  f 500 783 ));
DATA(insert (	2593   603 603 4  f 495 783 ));
DATA(insert (	2593   603 603 5  f 496 783 ));
DATA(insert (	2593   603 603 6  f 499 783 ));
DATA(insert (	2593   603 603 7  f 498 783 ));
DATA(insert (	2593   603 603 8  f 497 783 ));
DATA(insert (	2593   603 603 9  f 2571	783 ));
DATA(insert (	2593   603 603 10 f 2570	783 ));
DATA(insert (	2593   603 603 11 f 2573	783 ));
DATA(insert (	2593   603 603 12 f 2572	783 ));
DATA(insert (	2593   603 603 13 f 2863	783 ));
DATA(insert (	2593   603 603 14 f 2862	783 ));

/*
 *	gist poly_ops (supports polygons)
 */

DATA(insert (	2594   604 604 1  t 485 783 ));
DATA(insert (	2594   604 604 2  t 486 783 ));
DATA(insert (	2594   604 604 3  t 492 783 ));
DATA(insert (	2594   604 604 4  t 487 783 ));
DATA(insert (	2594   604 604 5  t 488 783 ));
DATA(insert (	2594   604 604 6  t 491 783 ));
DATA(insert (	2594   604 604 7  t 490 783 ));
DATA(insert (	2594   604 604 8  t 489 783 ));
DATA(insert (	2594   604 604 9  t 2575	783 ));
DATA(insert (	2594   604 604 10 t 2574	783 ));
DATA(insert (	2594   604 604 11 t 2577	783 ));
DATA(insert (	2594   604 604 12 t 2576	783 ));
DATA(insert (	2594   604 604 13 t 2861	783 ));
DATA(insert (	2594   604 604 14 t 2860	783 ));

/*
 *	gist circle_ops
 */

DATA(insert (	2595   718 718 1  t 1506	783 ));
DATA(insert (	2595   718 718 2  t 1507	783 ));
DATA(insert (	2595   718 718 3  t 1513	783 ));
DATA(insert (	2595   718 718 4  t 1508	783 ));
DATA(insert (	2595   718 718 5  t 1509	783 ));
DATA(insert (	2595   718 718 6  t 1512	783 ));
DATA(insert (	2595   718 718 7  t 1511	783 ));
DATA(insert (	2595   718 718 8  t 1510	783 ));
DATA(insert (	2595   718 718 9  t 2589	783 ));
DATA(insert (	2595   718 718 10 t 1515	783 ));
DATA(insert (	2595   718 718 11 t 1514	783 ));
DATA(insert (	2595   718 718 12 t 2590	783 ));
DATA(insert (	2595   718 718 13 t 2865	783 ));
DATA(insert (	2595   718 718 14 t 2864	783 ));

/*
 * gin array_ops (these anyarray operators are used with all the opclasses
 * of the family)
 */
DATA(insert (	2745   2277 2277 1	f	2750	2742 ));
DATA(insert (	2745   2277 2277 2	f	2751	2742 ));
DATA(insert (	2745   2277 2277 3	t	2752	2742 ));
DATA(insert (	2745   2277 2277 4	t	1070	2742 ));

/*
 * btree enum_ops
 */
DATA(insert (	3522   3500 3500 1	f	3518	403 ));
DATA(insert (	3522   3500 3500 2	f	3520	403 ));
DATA(insert (	3522   3500 3500 3	f	3516	403 ));
DATA(insert (	3522   3500 3500 4	f	3521	403 ));
DATA(insert (	3522   3500 3500 5	f	3519	403 ));

/*
 * hash enum_ops
 */
DATA(insert (	3523   3500 3500 1	f	3516	405 ));

/*
 * btree tsvector_ops
 */
DATA(insert (	3626   3614 3614 1 f   3627 403 ));
DATA(insert (	3626   3614 3614 2 f   3628 403 ));
DATA(insert (	3626   3614 3614 3 f   3629 403 ));
DATA(insert (	3626   3614 3614 4 f   3631 403 ));
DATA(insert (	3626   3614 3614 5 f   3632 403 ));

/*
 * GiST tsvector_ops
 */
DATA(insert (	3655   3614 3615 1	t  3636 783 ));

/*
 * GIN tsvector_ops
 */
DATA(insert (	3659   3614 3615 1	f  3636 2742 ));
DATA(insert (	3659   3614 3615 2	t  3660 2742 ));

/*
 * btree tsquery_ops
 */
DATA(insert (	3683   3615 3615 1 f   3674 403 ));
DATA(insert (	3683   3615 3615 2 f   3675 403 ));
DATA(insert (	3683   3615 3615 3 f   3676 403 ));
DATA(insert (	3683   3615 3615 4 f   3678 403 ));
DATA(insert (	3683   3615 3615 5 f   3679 403 ));

/*
 * GiST tsquery_ops
 */
DATA(insert (	3702   3615 3615 7	t  3693 783 ));
DATA(insert (	3702   3615 3615 8	t  3694 783 ));

#endif   /* PG_AMOP_H */
