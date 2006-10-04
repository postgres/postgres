/*-------------------------------------------------------------------------
 *
 * pg_amop.h
 *	  definition of the system "amop" relation (pg_amop)
 *	  along with the relation's initial contents.
 *
 * The amop table identifies the operators associated with each index opclass.
 *
 * The primary key for this table is <amopclaid, amopsubtype, amopstrategy>.
 * amopsubtype is equal to zero for an opclass's "default" operators
 * (which normally are those that accept the opclass's opcintype on both
 * left and right sides).  Some index AMs allow nondefault operators to
 * exist for a single strategy --- for example, in the btree AM nondefault
 * operators can have right-hand input data types different from opcintype,
 * and their amopsubtype is equal to the right-hand input data type.
 *
 * We also keep a unique index on <amopclaid, amopopr>, so that we can
 * use a syscache to quickly answer questions of the form "is this operator
 * in this opclass?".  This implies that the same operator cannot be listed
 * for multiple subtypes or strategy numbers of a single opclass.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/catalog/pg_amop.h,v 1.75 2006/10/04 00:30:07 momjian Exp $
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

CATALOG(pg_amop,2602) BKI_WITHOUT_OIDS
{
	Oid			amopclaid;		/* the index opclass this entry is for */
	Oid			amopsubtype;	/* operator subtype, or zero if default */
	int2		amopstrategy;	/* operator strategy number */
	bool		amopreqcheck;	/* index hit must be rechecked */
	Oid			amopopr;		/* the operator's pg_operator OID */
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
#define Natts_pg_amop					5
#define Anum_pg_amop_amopclaid			1
#define Anum_pg_amop_amopsubtype		2
#define Anum_pg_amop_amopstrategy		3
#define Anum_pg_amop_amopreqcheck		4
#define Anum_pg_amop_amopopr			5

/* ----------------
 *		initial contents of pg_amop
 * ----------------
 */

/*
 *	btree int2_ops
 */

DATA(insert (	1976	0 1 f	95 ));
DATA(insert (	1976	0 2 f  522 ));
DATA(insert (	1976	0 3 f	94 ));
DATA(insert (	1976	0 4 f  524 ));
DATA(insert (	1976	0 5 f  520 ));
/* crosstype operators int24 */
DATA(insert (	1976   23 1 f  534 ));
DATA(insert (	1976   23 2 f  540 ));
DATA(insert (	1976   23 3 f  532 ));
DATA(insert (	1976   23 4 f  542 ));
DATA(insert (	1976   23 5 f  536 ));
/* crosstype operators int28 */
DATA(insert (	1976   20 1 f  1864 ));
DATA(insert (	1976   20 2 f  1866 ));
DATA(insert (	1976   20 3 f  1862 ));
DATA(insert (	1976   20 4 f  1867 ));
DATA(insert (	1976   20 5 f  1865 ));

/*
 *	btree int4_ops
 */

DATA(insert (	1978	0 1 f	97 ));
DATA(insert (	1978	0 2 f  523 ));
DATA(insert (	1978	0 3 f	96 ));
DATA(insert (	1978	0 4 f  525 ));
DATA(insert (	1978	0 5 f  521 ));
/* crosstype operators int42 */
DATA(insert (	1978   21 1 f  535 ));
DATA(insert (	1978   21 2 f  541 ));
DATA(insert (	1978   21 3 f  533 ));
DATA(insert (	1978   21 4 f  543 ));
DATA(insert (	1978   21 5 f  537 ));
/* crosstype operators int48 */
DATA(insert (	1978   20 1 f	37 ));
DATA(insert (	1978   20 2 f	80 ));
DATA(insert (	1978   20 3 f	15 ));
DATA(insert (	1978   20 4 f	82 ));
DATA(insert (	1978   20 5 f	76 ));

/*
 *	btree int8_ops
 */

DATA(insert (	1980	0 1 f  412 ));
DATA(insert (	1980	0 2 f  414 ));
DATA(insert (	1980	0 3 f  410 ));
DATA(insert (	1980	0 4 f  415 ));
DATA(insert (	1980	0 5 f  413 ));
/* crosstype operators int82 */
DATA(insert (	1980   21 1 f  1870 ));
DATA(insert (	1980   21 2 f  1872 ));
DATA(insert (	1980   21 3 f  1868 ));
DATA(insert (	1980   21 4 f  1873 ));
DATA(insert (	1980   21 5 f  1871 ));
/* crosstype operators int84 */
DATA(insert (	1980   23 1 f  418 ));
DATA(insert (	1980   23 2 f  420 ));
DATA(insert (	1980   23 3 f  416 ));
DATA(insert (	1980   23 4 f  430 ));
DATA(insert (	1980   23 5 f  419 ));

/*
 *	btree oid_ops
 */

DATA(insert (	1989	0 1 f  609 ));
DATA(insert (	1989	0 2 f  611 ));
DATA(insert (	1989	0 3 f  607 ));
DATA(insert (	1989	0 4 f  612 ));
DATA(insert (	1989	0 5 f  610 ));

/*
 * btree tid_ops
 */

DATA(insert (	2789	0 1 f 2799 ));
DATA(insert (	2789	0 2 f 2801 ));
DATA(insert (	2789	0 3 f 387  ));
DATA(insert (	2789	0 4 f 2802 ));
DATA(insert (	2789	0 5 f 2800 ));

/*
 *	btree oidvector_ops
 */

DATA(insert (	1991	0 1 f  645 ));
DATA(insert (	1991	0 2 f  647 ));
DATA(insert (	1991	0 3 f  649 ));
DATA(insert (	1991	0 4 f  648 ));
DATA(insert (	1991	0 5 f  646 ));

/*
 *	btree float4_ops
 */

DATA(insert (	1970	0 1 f  622 ));
DATA(insert (	1970	0 2 f  624 ));
DATA(insert (	1970	0 3 f  620 ));
DATA(insert (	1970	0 4 f  625 ));
DATA(insert (	1970	0 5 f  623 ));
/* crosstype operators float48 */
DATA(insert (	1970  701 1 f  1122 ));
DATA(insert (	1970  701 2 f  1124 ));
DATA(insert (	1970  701 3 f  1120 ));
DATA(insert (	1970  701 4 f  1125 ));
DATA(insert (	1970  701 5 f  1123 ));

/*
 *	btree float8_ops
 */

DATA(insert (	1972	0 1 f  672 ));
DATA(insert (	1972	0 2 f  673 ));
DATA(insert (	1972	0 3 f  670 ));
DATA(insert (	1972	0 4 f  675 ));
DATA(insert (	1972	0 5 f  674 ));
/* crosstype operators float84 */
DATA(insert (	1972  700 1 f  1132 ));
DATA(insert (	1972  700 2 f  1134 ));
DATA(insert (	1972  700 3 f  1130 ));
DATA(insert (	1972  700 4 f  1135 ));
DATA(insert (	1972  700 5 f  1133 ));

/*
 *	btree char_ops
 */

DATA(insert (	 429	0 1 f  631 ));
DATA(insert (	 429	0 2 f  632 ));
DATA(insert (	 429	0 3 f	92 ));
DATA(insert (	 429	0 4 f  634 ));
DATA(insert (	 429	0 5 f  633 ));

/*
 *	btree name_ops
 */

DATA(insert (	1986	0 1 f  660 ));
DATA(insert (	1986	0 2 f  661 ));
DATA(insert (	1986	0 3 f	93 ));
DATA(insert (	1986	0 4 f  663 ));
DATA(insert (	1986	0 5 f  662 ));

/*
 *	btree text_ops
 */

DATA(insert (	1994	0 1 f  664 ));
DATA(insert (	1994	0 2 f  665 ));
DATA(insert (	1994	0 3 f	98 ));
DATA(insert (	1994	0 4 f  667 ));
DATA(insert (	1994	0 5 f  666 ));

/*
 *	btree bpchar_ops
 */

DATA(insert (	 426	0 1 f 1058 ));
DATA(insert (	 426	0 2 f 1059 ));
DATA(insert (	 426	0 3 f 1054 ));
DATA(insert (	 426	0 4 f 1061 ));
DATA(insert (	 426	0 5 f 1060 ));

/*
 *	btree varchar_ops (same operators as text_ops)
 */

DATA(insert (	2003	0 1 f 664 ));
DATA(insert (	2003	0 2 f 665 ));
DATA(insert (	2003	0 3 f  98 ));
DATA(insert (	2003	0 4 f 667 ));
DATA(insert (	2003	0 5 f 666 ));

/*
 *	btree bytea_ops
 */

DATA(insert (	 428	0 1 f 1957 ));
DATA(insert (	 428	0 2 f 1958 ));
DATA(insert (	 428	0 3 f 1955 ));
DATA(insert (	 428	0 4 f 1960 ));
DATA(insert (	 428	0 5 f 1959 ));

/*
 *	btree abstime_ops
 */

DATA(insert (	 421	0 1 f  562 ));
DATA(insert (	 421	0 2 f  564 ));
DATA(insert (	 421	0 3 f  560 ));
DATA(insert (	 421	0 4 f  565 ));
DATA(insert (	 421	0 5 f  563 ));

/*
 *	btree date_ops
 */

DATA(insert (	 434	0 1 f 1095 ));
DATA(insert (	 434	0 2 f 1096 ));
DATA(insert (	 434	0 3 f 1093 ));
DATA(insert (	 434	0 4 f 1098 ));
DATA(insert (	 434	0 5 f 1097 ));
/* crosstype operators vs timestamp */
DATA(insert (	 434 1114 1 f 2345 ));
DATA(insert (	 434 1114 2 f 2346 ));
DATA(insert (	 434 1114 3 f 2347 ));
DATA(insert (	 434 1114 4 f 2348 ));
DATA(insert (	 434 1114 5 f 2349 ));
/* crosstype operators vs timestamptz */
DATA(insert (	 434 1184 1 f 2358 ));
DATA(insert (	 434 1184 2 f 2359 ));
DATA(insert (	 434 1184 3 f 2360 ));
DATA(insert (	 434 1184 4 f 2361 ));
DATA(insert (	 434 1184 5 f 2362 ));

/*
 *	btree time_ops
 */

DATA(insert (	1996	0 1 f 1110 ));
DATA(insert (	1996	0 2 f 1111 ));
DATA(insert (	1996	0 3 f 1108 ));
DATA(insert (	1996	0 4 f 1113 ));
DATA(insert (	1996	0 5 f 1112 ));

/*
 *	btree timetz_ops
 */

DATA(insert (	2000	0 1 f 1552 ));
DATA(insert (	2000	0 2 f 1553 ));
DATA(insert (	2000	0 3 f 1550 ));
DATA(insert (	2000	0 4 f 1555 ));
DATA(insert (	2000	0 5 f 1554 ));

/*
 *	btree timestamp_ops
 */

DATA(insert (	2039	0 1 f 2062 ));
DATA(insert (	2039	0 2 f 2063 ));
DATA(insert (	2039	0 3 f 2060 ));
DATA(insert (	2039	0 4 f 2065 ));
DATA(insert (	2039	0 5 f 2064 ));
/* crosstype operators vs date */
DATA(insert (	2039 1082 1 f 2371 ));
DATA(insert (	2039 1082 2 f 2372 ));
DATA(insert (	2039 1082 3 f 2373 ));
DATA(insert (	2039 1082 4 f 2374 ));
DATA(insert (	2039 1082 5 f 2375 ));
/* crosstype operators vs timestamptz */
DATA(insert (	2039 1184 1 f 2534 ));
DATA(insert (	2039 1184 2 f 2535 ));
DATA(insert (	2039 1184 3 f 2536 ));
DATA(insert (	2039 1184 4 f 2537 ));
DATA(insert (	2039 1184 5 f 2538 ));

/*
 *	btree timestamptz_ops
 */

DATA(insert (	1998	0 1 f 1322 ));
DATA(insert (	1998	0 2 f 1323 ));
DATA(insert (	1998	0 3 f 1320 ));
DATA(insert (	1998	0 4 f 1325 ));
DATA(insert (	1998	0 5 f 1324 ));
/* crosstype operators vs date */
DATA(insert (	1998 1082 1 f 2384 ));
DATA(insert (	1998 1082 2 f 2385 ));
DATA(insert (	1998 1082 3 f 2386 ));
DATA(insert (	1998 1082 4 f 2387 ));
DATA(insert (	1998 1082 5 f 2388 ));
/* crosstype operators vs timestamp */
DATA(insert (	1998 1114 1 f 2540 ));
DATA(insert (	1998 1114 2 f 2541 ));
DATA(insert (	1998 1114 3 f 2542 ));
DATA(insert (	1998 1114 4 f 2543 ));
DATA(insert (	1998 1114 5 f 2544 ));

/*
 *	btree interval_ops
 */

DATA(insert (	1982	0 1 f 1332 ));
DATA(insert (	1982	0 2 f 1333 ));
DATA(insert (	1982	0 3 f 1330 ));
DATA(insert (	1982	0 4 f 1335 ));
DATA(insert (	1982	0 5 f 1334 ));

/*
 *	btree macaddr
 */

DATA(insert (	1984	0 1 f 1222 ));
DATA(insert (	1984	0 2 f 1223 ));
DATA(insert (	1984	0 3 f 1220 ));
DATA(insert (	1984	0 4 f 1225 ));
DATA(insert (	1984	0 5 f 1224 ));

/*
 *	btree inet
 */

DATA(insert (	1974	0 1 f 1203 ));
DATA(insert (	1974	0 2 f 1204 ));
DATA(insert (	1974	0 3 f 1201 ));
DATA(insert (	1974	0 4 f 1206 ));
DATA(insert (	1974	0 5 f 1205 ));

/*
 *	btree cidr
 */

DATA(insert (	 432	0 1 f 1203 ));
DATA(insert (	 432	0 2 f 1204 ));
DATA(insert (	 432	0 3 f 1201 ));
DATA(insert (	 432	0 4 f 1206 ));
DATA(insert (	 432	0 5 f 1205 ));

/*
 *	btree numeric
 */

DATA(insert (	1988	0 1 f 1754 ));
DATA(insert (	1988	0 2 f 1755 ));
DATA(insert (	1988	0 3 f 1752 ));
DATA(insert (	1988	0 4 f 1757 ));
DATA(insert (	1988	0 5 f 1756 ));

/*
 *	btree bool
 */

DATA(insert (	 424	0 1 f	58 ));
DATA(insert (	 424	0 2 f 1694 ));
DATA(insert (	 424	0 3 f	91 ));
DATA(insert (	 424	0 4 f 1695 ));
DATA(insert (	 424	0 5 f	59 ));

/*
 *	btree bit
 */

DATA(insert (	 423	0 1 f 1786 ));
DATA(insert (	 423	0 2 f 1788 ));
DATA(insert (	 423	0 3 f 1784 ));
DATA(insert (	 423	0 4 f 1789 ));
DATA(insert (	 423	0 5 f 1787 ));

/*
 *	btree varbit
 */

DATA(insert (	2002	0 1 f 1806 ));
DATA(insert (	2002	0 2 f 1808 ));
DATA(insert (	2002	0 3 f 1804 ));
DATA(insert (	2002	0 4 f 1809 ));
DATA(insert (	2002	0 5 f 1807 ));

/*
 *	btree text pattern
 */

DATA(insert (	2095	0 1 f 2314 ));
DATA(insert (	2095	0 2 f 2315 ));
DATA(insert (	2095	0 3 f 2316 ));
DATA(insert (	2095	0 4 f 2317 ));
DATA(insert (	2095	0 5 f 2318 ));

/*
 *	btree varchar pattern (same operators as text)
 */

DATA(insert (	2096	0 1 f 2314 ));
DATA(insert (	2096	0 2 f 2315 ));
DATA(insert (	2096	0 3 f 2316 ));
DATA(insert (	2096	0 4 f 2317 ));
DATA(insert (	2096	0 5 f 2318 ));

/*
 *	btree bpchar pattern
 */

DATA(insert (	2097	0 1 f 2326 ));
DATA(insert (	2097	0 2 f 2327 ));
DATA(insert (	2097	0 3 f 2328 ));
DATA(insert (	2097	0 4 f 2329 ));
DATA(insert (	2097	0 5 f 2330 ));

/*
 *	btree name pattern
 */

DATA(insert (	2098	0 1 f 2332 ));
DATA(insert (	2098	0 2 f 2333 ));
DATA(insert (	2098	0 3 f 2334 ));
DATA(insert (	2098	0 4 f 2335 ));
DATA(insert (	2098	0 5 f 2336 ));

/*
 *	btree money_ops
 */

DATA(insert (	2099	0 1 f  902 ));
DATA(insert (	2099	0 2 f  904 ));
DATA(insert (	2099	0 3 f  900 ));
DATA(insert (	2099	0 4 f  905 ));
DATA(insert (	2099	0 5 f  903 ));

/*
 *	btree reltime_ops
 */

DATA(insert (	2233	0 1 f  568 ));
DATA(insert (	2233	0 2 f  570 ));
DATA(insert (	2233	0 3 f  566 ));
DATA(insert (	2233	0 4 f  571 ));
DATA(insert (	2233	0 5 f  569 ));

/*
 *	btree tinterval_ops
 */

DATA(insert (	2234	0 1 f  813 ));
DATA(insert (	2234	0 2 f  815 ));
DATA(insert (	2234	0 3 f  811 ));
DATA(insert (	2234	0 4 f  816 ));
DATA(insert (	2234	0 5 f  814 ));

/*
 *	btree array_ops
 */

DATA(insert (	 397	0 1 f 1072 ));
DATA(insert (	 397	0 2 f 1074 ));
DATA(insert (	 397	0 3 f 1070 ));
DATA(insert (	 397	0 4 f 1075 ));
DATA(insert (	 397	0 5 f 1073 ));

/*
 *	hash index _ops
 */

/* bpchar_ops */
DATA(insert (	 427	0 1 f 1054 ));
/* char_ops */
DATA(insert (	 431	0 1 f	92 ));
/* cidr_ops */
DATA(insert (	 433	0 1 f 1201 ));
/* date_ops */
DATA(insert (	 435	0 1 f 1093 ));
/* float4_ops */
DATA(insert (	1971	0 1 f  620 ));
/* float8_ops */
DATA(insert (	1973	0 1 f  670 ));
/* inet_ops */
DATA(insert (	1975	0 1 f 1201 ));
/* int2_ops */
DATA(insert (	1977	0 1 f	94 ));
/* int4_ops */
DATA(insert (	1979	0 1 f	96 ));
/* int8_ops */
DATA(insert (	1981	0 1 f  410 ));
/* interval_ops */
DATA(insert (	1983	0 1 f 1330 ));
/* macaddr_ops */
DATA(insert (	1985	0 1 f 1220 ));
/* name_ops */
DATA(insert (	1987	0 1 f	93 ));
/* oid_ops */
DATA(insert (	1990	0 1 f  607 ));
/* oidvector_ops */
DATA(insert (	1992	0 1 f  649 ));
/* text_ops */
DATA(insert (	1995	0 1 f	98 ));
/* time_ops */
DATA(insert (	1997	0 1 f 1108 ));
/* timestamptz_ops */
DATA(insert (	1999	0 1 f 1320 ));
/* timetz_ops */
DATA(insert (	2001	0 1 f 1550 ));
/* varchar_ops */
DATA(insert (	2004	0 1 f	98 ));
/* timestamp_ops */
DATA(insert (	2040	0 1 f 2060 ));
/* bool_ops */
DATA(insert (	2222	0 1 f	91 ));
/* bytea_ops */
DATA(insert (	2223	0 1 f 1955 ));
/* int2vector_ops */
DATA(insert (	2224	0 1 f  386 ));
/* xid_ops */
DATA(insert (	2225	0 1 f  352 ));
/* cid_ops */
DATA(insert (	2226	0 1 f  385 ));
/* abstime_ops */
DATA(insert (	2227	0 1 f  560 ));
/* reltime_ops */
DATA(insert (	2228	0 1 f  566 ));
/* text_pattern_ops */
DATA(insert (	2229	0 1 f 2316 ));
/* varchar_pattern_ops */
DATA(insert (	2230	0 1 f 2316 ));
/* bpchar_pattern_ops */
DATA(insert (	2231	0 1 f 2328 ));
/* name_pattern_ops */
DATA(insert (	2232	0 1 f 2334 ));
/* aclitem_ops */
DATA(insert (	2235	0 1 f  974 ));

/*
 *	gist box_ops
 */

DATA(insert (	2593	0 1  f	493 ));
DATA(insert (	2593	0 2  f	494 ));
DATA(insert (	2593	0 3  f	500 ));
DATA(insert (	2593	0 4  f	495 ));
DATA(insert (	2593	0 5  f	496 ));
DATA(insert (	2593	0 6  f	499 ));
DATA(insert (	2593	0 7  f	498 ));
DATA(insert (	2593	0 8  f	497 ));
DATA(insert (	2593	0 9  f	2571 ));
DATA(insert (	2593	0 10 f	2570 ));
DATA(insert (	2593	0 11 f	2573 ));
DATA(insert (	2593	0 12 f	2572 ));
DATA(insert (	2593	0 13 f	2863 ));
DATA(insert (	2593	0 14 f	2862 ));

/*
 *	gist poly_ops (supports polygons)
 */

DATA(insert (	2594	0 1  t	485 ));
DATA(insert (	2594	0 2  t	486 ));
DATA(insert (	2594	0 3  t	492 ));
DATA(insert (	2594	0 4  t	487 ));
DATA(insert (	2594	0 5  t	488 ));
DATA(insert (	2594	0 6  t	491 ));
DATA(insert (	2594	0 7  t	490 ));
DATA(insert (	2594	0 8  t	489 ));
DATA(insert (	2594	0 9  t	2575 ));
DATA(insert (	2594	0 10 t	2574 ));
DATA(insert (	2594	0 11 t	2577 ));
DATA(insert (	2594	0 12 t	2576 ));
DATA(insert (	2594	0 13 t	2861 ));
DATA(insert (	2594	0 14 t	2860 ));

/*
 *	gist circle_ops
 */

DATA(insert (	2595	0 1  t	1506 ));
DATA(insert (	2595	0 2  t	1507 ));
DATA(insert (	2595	0 3  t	1513 ));
DATA(insert (	2595	0 4  t	1508 ));
DATA(insert (	2595	0 5  t	1509 ));
DATA(insert (	2595	0 6  t	1512 ));
DATA(insert (	2595	0 7  t	1511 ));
DATA(insert (	2595	0 8  t	1510 ));
DATA(insert (	2595	0 9  t	2589 ));
DATA(insert (	2595	0 10 t	1515 ));
DATA(insert (	2595	0 11 t	1514 ));
DATA(insert (	2595	0 12 t	2590 ));
DATA(insert (	2595	0 13 t	2865 ));
DATA(insert (	2595	0 14 t	2864 ));

/*
 * gin _int4_ops
 */
DATA(insert (	2745	0 1  f	2750 ));
DATA(insert (	2745	0 2  f	2751 ));
DATA(insert (	2745	0 3  t	2752 ));
DATA(insert (	2745	0 4  t	1070 ));

/*
 * gin _text_ops
 */
DATA(insert (	2746	0 1  f	2750 ));
DATA(insert (	2746	0 2  f	2751 ));
DATA(insert (	2746	0 3  t	2752 ));
DATA(insert (	2746	0 4  t	1070 ));

/*
 * gin _abstime_ops
 */
DATA(insert (	2753	0 1  f	2750 ));
DATA(insert (	2753	0 2  f	2751 ));
DATA(insert (	2753	0 3  t	2752 ));
DATA(insert (	2753	0 4  t	1070 ));

/*
 * gin _bit_ops
 */
DATA(insert (	2754	0 1  f	2750 ));
DATA(insert (	2754	0 2  f	2751 ));
DATA(insert (	2754	0 3  t	2752 ));
DATA(insert (	2754	0 4  t	1070 ));

/*
 * gin _bool_ops
 */
DATA(insert (	2755	0 1  f	2750 ));
DATA(insert (	2755	0 2  f	2751 ));
DATA(insert (	2755	0 3  t	2752 ));
DATA(insert (	2755	0 4  t	1070 ));

/*
 * gin _bpchar_ops
 */
DATA(insert (	2756	0 1  f	2750 ));
DATA(insert (	2756	0 2  f	2751 ));
DATA(insert (	2756	0 3  t	2752 ));
DATA(insert (	2756	0 4  t	1070 ));

/*
 * gin _bytea_ops
 */
DATA(insert (	2757	0 1  f	2750 ));
DATA(insert (	2757	0 2  f	2751 ));
DATA(insert (	2757	0 3  t	2752 ));
DATA(insert (	2757	0 4  t	1070 ));

/*
 * gin _char_ops
 */
DATA(insert (	2758	0 1  f	2750 ));
DATA(insert (	2758	0 2  f	2751 ));
DATA(insert (	2758	0 3  t	2752 ));
DATA(insert (	2758	0 4  t	1070 ));

/*
 * gin _cidr_ops
 */
DATA(insert (	2759	0 1  f	2750 ));
DATA(insert (	2759	0 2  f	2751 ));
DATA(insert (	2759	0 3  t	2752 ));
DATA(insert (	2759	0 4  t	1070 ));

/*
 * gin _date_ops
 */
DATA(insert (	2760	0 1  f	2750 ));
DATA(insert (	2760	0 2  f	2751 ));
DATA(insert (	2760	0 3  t	2752 ));
DATA(insert (	2760	0 4  t	1070 ));

/*
 * gin _float4_ops
 */
DATA(insert (	2761	0 1  f	2750 ));
DATA(insert (	2761	0 2  f	2751 ));
DATA(insert (	2761	0 3  t	2752 ));
DATA(insert (	2761	0 4  t	1070 ));

/*
 * gin _float8_ops
 */
DATA(insert (	2762	0 1  f	2750 ));
DATA(insert (	2762	0 2  f	2751 ));
DATA(insert (	2762	0 3  t	2752 ));
DATA(insert (	2762	0 4  t	1070 ));

/*
 * gin _inet_ops
 */
DATA(insert (	2763	0 1  f	2750 ));
DATA(insert (	2763	0 2  f	2751 ));
DATA(insert (	2763	0 3  t	2752 ));
DATA(insert (	2763	0 4  t	1070 ));

/*
 * gin _int2_ops
 */
DATA(insert (	2764	0 1  f	2750 ));
DATA(insert (	2764	0 2  f	2751 ));
DATA(insert (	2764	0 3  t	2752 ));
DATA(insert (	2764	0 4  t	1070 ));

/*
 * gin _int8_ops
 */
DATA(insert (	2765	0 1  f	2750 ));
DATA(insert (	2765	0 2  f	2751 ));
DATA(insert (	2765	0 3  t	2752 ));
DATA(insert (	2765	0 4  t	1070 ));

/*
 * gin _interval_ops
 */
DATA(insert (	2766	0 1  f	2750 ));
DATA(insert (	2766	0 2  f	2751 ));
DATA(insert (	2766	0 3  t	2752 ));
DATA(insert (	2766	0 4  t	1070 ));

/*
 * gin _macaddr_ops
 */
DATA(insert (	2767	0 1  f	2750 ));
DATA(insert (	2767	0 2  f	2751 ));
DATA(insert (	2767	0 3  t	2752 ));
DATA(insert (	2767	0 4  t	1070 ));

/*
 * gin _name_ops
 */
DATA(insert (	2768	0 1  f	2750 ));
DATA(insert (	2768	0 2  f	2751 ));
DATA(insert (	2768	0 3  t	2752 ));
DATA(insert (	2768	0 4  t	1070 ));

/*
 * gin _numeric_ops
 */
DATA(insert (	2769	0 1  f	2750 ));
DATA(insert (	2769	0 2  f	2751 ));
DATA(insert (	2769	0 3  t	2752 ));
DATA(insert (	2769	0 4  t	1070 ));

/*
 * gin _oid_ops
 */
DATA(insert (	2770	0 1  f	2750 ));
DATA(insert (	2770	0 2  f	2751 ));
DATA(insert (	2770	0 3  t	2752 ));
DATA(insert (	2770	0 4  t	1070 ));

/*
 * gin _oidvector_ops
 */
DATA(insert (	2771	0 1  f	2750 ));
DATA(insert (	2771	0 2  f	2751 ));
DATA(insert (	2771	0 3  t	2752 ));
DATA(insert (	2771	0 4  t	1070 ));

/*
 * gin _time_ops
 */
DATA(insert (	2772	0 1  f	2750 ));
DATA(insert (	2772	0 2  f	2751 ));
DATA(insert (	2772	0 3  t	2752 ));
DATA(insert (	2772	0 4  t	1070 ));

/*
 * gin _timestamptz_ops
 */
DATA(insert (	2773	0 1  f	2750 ));
DATA(insert (	2773	0 2  f	2751 ));
DATA(insert (	2773	0 3  t	2752 ));
DATA(insert (	2773	0 4  t	1070 ));

/*
 * gin _timetz_ops
 */
DATA(insert (	2774	0 1  f	2750 ));
DATA(insert (	2774	0 2  f	2751 ));
DATA(insert (	2774	0 3  t	2752 ));
DATA(insert (	2774	0 4  t	1070 ));

/*
 * gin _varbit_ops
 */
DATA(insert (	2775	0 1  f	2750 ));
DATA(insert (	2775	0 2  f	2751 ));
DATA(insert (	2775	0 3  t	2752 ));
DATA(insert (	2775	0 4  t	1070 ));

/*
 * gin _varchar_ops
 */
DATA(insert (	2776	0 1  f	2750 ));
DATA(insert (	2776	0 2  f	2751 ));
DATA(insert (	2776	0 3  t	2752 ));
DATA(insert (	2776	0 4  t	1070 ));

/*
 * gin _timestamp_ops
 */
DATA(insert (	2777	0 1  f	2750 ));
DATA(insert (	2777	0 2  f	2751 ));
DATA(insert (	2777	0 3  t	2752 ));
DATA(insert (	2777	0 4  t	1070 ));

/*
 * gin _money_ops
 */
DATA(insert (	2778	0 1  f	2750 ));
DATA(insert (	2778	0 2  f	2751 ));
DATA(insert (	2778	0 3  t	2752 ));
DATA(insert (	2778	0 4  t	1070 ));

/*
 * gin _reltime_ops
 */
DATA(insert (	2779	0 1  f	2750 ));
DATA(insert (	2779	0 2  f	2751 ));
DATA(insert (	2779	0 3  t	2752 ));
DATA(insert (	2779	0 4  t	1070 ));

/*
 * gin _tinterval_ops
 */
DATA(insert (	2780	0 1  f	2750 ));
DATA(insert (	2780	0 2  f	2751 ));
DATA(insert (	2780	0 3  t	2752 ));
DATA(insert (	2780	0 4  t	1070 ));

#endif   /* PG_AMOP_H */
