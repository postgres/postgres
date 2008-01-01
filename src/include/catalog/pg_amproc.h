/*-------------------------------------------------------------------------
 *
 * pg_amproc.h
 *	  definition of the system "amproc" relation (pg_amproc)
 *	  along with the relation's initial contents.
 *
 * The amproc table identifies support procedures associated with index
 * operator families and classes.  These procedures can't be listed in pg_amop
 * since they are not the implementation of any indexable operator.
 *
 * The primary key for this table is <amprocfamily, amproclefttype,
 * amprocrighttype, amprocnum>.  The "default" support functions for a
 * particular opclass within the family are those with amproclefttype =
 * amprocrighttype = opclass's opcintype.  These are the ones loaded into the
 * relcache for an index and typically used for internal index operations.
 * Other support functions are typically used to handle cross-type indexable
 * operators with oprleft/oprright matching the entry's amproclefttype and
 * amprocrighttype. The exact behavior depends on the index AM, however, and
 * some don't pay attention to non-default functions at all.
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/catalog/pg_amproc.h,v 1.70 2008/01/01 19:45:56 momjian Exp $
 *
 * NOTES
 *	  the genbki.sh script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_AMPROC_H
#define PG_AMPROC_H

/* ----------------
 *		postgres.h contains the system type definitions and the
 *		CATALOG(), BKI_BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------
 *		pg_amproc definition.  cpp turns this into
 *		typedef struct FormData_pg_amproc
 * ----------------
 */
#define AccessMethodProcedureRelationId  2603

CATALOG(pg_amproc,2603)
{
	Oid			amprocfamily;	/* the index opfamily this entry is for */
	Oid			amproclefttype; /* procedure's left input data type */
	Oid			amprocrighttype;	/* procedure's right input data type */
	int2		amprocnum;		/* support procedure index */
	regproc		amproc;			/* OID of the proc */
} FormData_pg_amproc;

/* ----------------
 *		Form_pg_amproc corresponds to a pointer to a tuple with
 *		the format of pg_amproc relation.
 * ----------------
 */
typedef FormData_pg_amproc *Form_pg_amproc;

/* ----------------
 *		compiler constants for pg_amproc
 * ----------------
 */
#define Natts_pg_amproc					5
#define Anum_pg_amproc_amprocfamily		1
#define Anum_pg_amproc_amproclefttype	2
#define Anum_pg_amproc_amprocrighttype	3
#define Anum_pg_amproc_amprocnum		4
#define Anum_pg_amproc_amproc			5

/* ----------------
 *		initial contents of pg_amproc
 * ----------------
 */

/* btree */
DATA(insert (	397   2277 2277 1 382 ));
DATA(insert (	421   702 702 1 357 ));
DATA(insert (	423   1560 1560 1 1596 ));
DATA(insert (	424   16 16 1 1693 ));
DATA(insert (	426   1042 1042 1 1078 ));
DATA(insert (	428   17 17 1 1954 ));
DATA(insert (	429   18 18 1 358 ));
DATA(insert (	434   1082 1082 1 1092 ));
DATA(insert (	434   1082 1114 1 2344 ));
DATA(insert (	434   1082 1184 1 2357 ));
DATA(insert (	434   1114 1114 1 2045 ));
DATA(insert (	434   1114 1082 1 2370 ));
DATA(insert (	434   1114 1184 1 2526 ));
DATA(insert (	434   1184 1184 1 1314 ));
DATA(insert (	434   1184 1082 1 2383 ));
DATA(insert (	434   1184 1114 1 2533 ));
DATA(insert (	1970   700 700 1 354 ));
DATA(insert (	1970   700 701 1 2194 ));
DATA(insert (	1970   701 701 1 355 ));
DATA(insert (	1970   701 700 1 2195 ));
DATA(insert (	1974   869 869 1 926 ));
DATA(insert (	1976   21 21 1 350 ));
DATA(insert (	1976   21 23 1 2190 ));
DATA(insert (	1976   21 20 1 2192 ));
DATA(insert (	1976   23 23 1 351 ));
DATA(insert (	1976   23 20 1 2188 ));
DATA(insert (	1976   23 21 1 2191 ));
DATA(insert (	1976   20 20 1 842 ));
DATA(insert (	1976   20 23 1 2189 ));
DATA(insert (	1976   20 21 1 2193 ));
DATA(insert (	1982   1186 1186 1 1315 ));
DATA(insert (	1984   829 829 1 836 ));
DATA(insert (	1986   19 19 1 359 ));
DATA(insert (	1988   1700 1700 1 1769 ));
DATA(insert (	1989   26 26 1 356 ));
DATA(insert (	1991   30 30 1 404 ));
DATA(insert (	1994   25 25 1 360 ));
DATA(insert (	1996   1083 1083 1 1107 ));
DATA(insert (	2000   1266 1266 1 1358 ));
DATA(insert (	2002   1562 1562 1 1672 ));
DATA(insert (	2095   25 25 1 2166 ));
DATA(insert (	2097   1042 1042 1 2180 ));
DATA(insert (	2098   19 19 1 2187 ));
DATA(insert (	2099   790 790 1  377 ));
DATA(insert (	2233   703 703 1  380 ));
DATA(insert (	2234   704 704 1  381 ));
DATA(insert (	2789   27 27 1 2794 ));
DATA(insert (	2968   2950 2950 1 2960 ));
DATA(insert (	3522   3500 3500 1 3514 ));


/* hash */
DATA(insert (	427   1042 1042 1 1080 ));
DATA(insert (	431   18 18 1 454 ));
DATA(insert (	435   1082 1082 1 450 ));
DATA(insert (	1971   700 700 1 451 ));
DATA(insert (	1971   701 701 1 452 ));
DATA(insert (	1975   869 869 1 422 ));
DATA(insert (	1977   21 21 1 449 ));
DATA(insert (	1977   23 23 1 450 ));
DATA(insert (	1977   20 20 1 949 ));
DATA(insert (	1983   1186 1186 1 1697 ));
DATA(insert (	1985   829 829 1 399 ));
DATA(insert (	1987   19 19 1 455 ));
DATA(insert (	1990   26 26 1 453 ));
DATA(insert (	1992   30 30 1 457 ));
DATA(insert (	1995   25 25 1 400 ));
DATA(insert (	1997   1083 1083 1 1688 ));
DATA(insert (	1998   1700 1700 1 432 ));
DATA(insert (	1999   1184 1184 1 2039 ));
DATA(insert (	2001   1266 1266 1 1696 ));
DATA(insert (	2040   1114 1114 1 2039 ));
DATA(insert (	2222   16 16 1 454 ));
DATA(insert (	2223   17 17 1 456 ));
DATA(insert (	2224   22 22 1 398 ));
DATA(insert (	2225   28 28 1 450 ));
DATA(insert (	2226   29 29 1 450 ));
DATA(insert (	2227   702 702 1 450 ));
DATA(insert (	2228   703 703 1 450 ));
DATA(insert (	2229   25 25 1 456 ));
DATA(insert (	2231   1042 1042 1 456 ));
DATA(insert (	2232   19 19 1 455 ));
DATA(insert (	2235   1033 1033 1 329 ));
DATA(insert (	2969   2950 2950 1 2963 ));
DATA(insert (	3523   3500 3500 1 3515 ));


/* gist */
DATA(insert (	2593   603 603 1 2578 ));
DATA(insert (	2593   603 603 2 2583 ));
DATA(insert (	2593   603 603 3 2579 ));
DATA(insert (	2593   603 603 4 2580 ));
DATA(insert (	2593   603 603 5 2581 ));
DATA(insert (	2593   603 603 6 2582 ));
DATA(insert (	2593   603 603 7 2584 ));
DATA(insert (	2594   604 604 1 2585 ));
DATA(insert (	2594   604 604 2 2583 ));
DATA(insert (	2594   604 604 3 2586 ));
DATA(insert (	2594   604 604 4 2580 ));
DATA(insert (	2594   604 604 5 2581 ));
DATA(insert (	2594   604 604 6 2582 ));
DATA(insert (	2594   604 604 7 2584 ));
DATA(insert (	2595   718 718 1 2591 ));
DATA(insert (	2595   718 718 2 2583 ));
DATA(insert (	2595   718 718 3 2592 ));
DATA(insert (	2595   718 718 4 2580 ));
DATA(insert (	2595   718 718 5 2581 ));
DATA(insert (	2595   718 718 6 2582 ));
DATA(insert (	2595   718 718 7 2584 ));
DATA(insert (	3655   3614 3614 1 3654 ));
DATA(insert (	3655   3614 3614 2 3651 ));
DATA(insert (	3655   3614 3614 3 3648 ));
DATA(insert (	3655   3614 3614 4 3649 ));
DATA(insert (	3655   3614 3614 5 3653 ));
DATA(insert (	3655   3614 3614 6 3650 ));
DATA(insert (	3655   3614 3614 7 3652 ));
DATA(insert (	3702   3615 3615 1 3701 ));
DATA(insert (	3702   3615 3615 2 3698 ));
DATA(insert (	3702   3615 3615 3 3695 ));
DATA(insert (	3702   3615 3615 4 3696 ));
DATA(insert (	3702   3615 3615 5 3700 ));
DATA(insert (	3702   3615 3615 6 3697 ));
DATA(insert (	3702   3615 3615 7 3699 ));


/* gin */
DATA(insert (	2745   1007 1007 1	351 ));
DATA(insert (	2745   1007 1007 2 2743 ));
DATA(insert (	2745   1007 1007 3 2774 ));
DATA(insert (	2745   1007 1007 4 2744 ));
DATA(insert (	2745   1009 1009 1	360 ));
DATA(insert (	2745   1009 1009 2 2743 ));
DATA(insert (	2745   1009 1009 3 2774 ));
DATA(insert (	2745   1009 1009 4 2744 ));
DATA(insert (	2745   1015 1015 1	360 ));
DATA(insert (	2745   1015 1015 2 2743 ));
DATA(insert (	2745   1015 1015 3 2774 ));
DATA(insert (	2745   1015 1015 4 2744 ));
DATA(insert (	2745   1023 1023 1 357 ));
DATA(insert (	2745   1023 1023 2 2743 ));
DATA(insert (	2745   1023 1023 3 2774 ));
DATA(insert (	2745   1023 1023 4 2744 ));
DATA(insert (	2745   1561 1561 1 1596 ));
DATA(insert (	2745   1561 1561 2 2743 ));
DATA(insert (	2745   1561 1561 3 2774 ));
DATA(insert (	2745   1561 1561 4 2744 ));
DATA(insert (	2745   1000 1000 1 1693 ));
DATA(insert (	2745   1000 1000 2 2743 ));
DATA(insert (	2745   1000 1000 3 2774 ));
DATA(insert (	2745   1000 1000 4 2744 ));
DATA(insert (	2745   1014 1014 1 1078 ));
DATA(insert (	2745   1014 1014 2 2743 ));
DATA(insert (	2745   1014 1014 3 2774 ));
DATA(insert (	2745   1014 1014 4 2744 ));
DATA(insert (	2745   1001 1001 1 1954 ));
DATA(insert (	2745   1001 1001 2 2743 ));
DATA(insert (	2745   1001 1001 3 2774 ));
DATA(insert (	2745   1001 1001 4 2744 ));
DATA(insert (	2745   1002 1002 1 358 ));
DATA(insert (	2745   1002 1002 2 2743 ));
DATA(insert (	2745   1002 1002 3 2774 ));
DATA(insert (	2745   1002 1002 4 2744 ));
DATA(insert (	2745   1182 1182 1 1092 ));
DATA(insert (	2745   1182 1182 2 2743 ));
DATA(insert (	2745   1182 1182 3 2774 ));
DATA(insert (	2745   1182 1182 4 2744 ));
DATA(insert (	2745   1021 1021 1 354 ));
DATA(insert (	2745   1021 1021 2 2743 ));
DATA(insert (	2745   1021 1021 3 2774 ));
DATA(insert (	2745   1021 1021 4 2744 ));
DATA(insert (	2745   1022 1022 1 355 ));
DATA(insert (	2745   1022 1022 2 2743 ));
DATA(insert (	2745   1022 1022 3 2774 ));
DATA(insert (	2745   1022 1022 4 2744 ));
DATA(insert (	2745   1041 1041 1 926 ));
DATA(insert (	2745   1041 1041 2 2743 ));
DATA(insert (	2745   1041 1041 3 2774 ));
DATA(insert (	2745   1041 1041 4 2744 ));
DATA(insert (	2745   651 651 1 926 ));
DATA(insert (	2745   651 651 2 2743 ));
DATA(insert (	2745   651 651 3 2774 ));
DATA(insert (	2745   651 651 4 2744 ));
DATA(insert (	2745   1005 1005 1 350 ));
DATA(insert (	2745   1005 1005 2 2743 ));
DATA(insert (	2745   1005 1005 3 2774 ));
DATA(insert (	2745   1005 1005 4 2744 ));
DATA(insert (	2745   1016 1016 1 842 ));
DATA(insert (	2745   1016 1016 2 2743 ));
DATA(insert (	2745   1016 1016 3 2774 ));
DATA(insert (	2745   1016 1016 4 2744 ));
DATA(insert (	2745   1187 1187 1 1315 ));
DATA(insert (	2745   1187 1187 2 2743 ));
DATA(insert (	2745   1187 1187 3 2774 ));
DATA(insert (	2745   1187 1187 4 2744 ));
DATA(insert (	2745   1040 1040 1 836 ));
DATA(insert (	2745   1040 1040 2 2743 ));
DATA(insert (	2745   1040 1040 3 2774 ));
DATA(insert (	2745   1040 1040 4 2744 ));
DATA(insert (	2745   1003 1003 1 359 ));
DATA(insert (	2745   1003 1003 2 2743 ));
DATA(insert (	2745   1003 1003 3 2774 ));
DATA(insert (	2745   1003 1003 4 2744 ));
DATA(insert (	2745   1231 1231 1 1769 ));
DATA(insert (	2745   1231 1231 2 2743 ));
DATA(insert (	2745   1231 1231 3 2774 ));
DATA(insert (	2745   1231 1231 4 2744 ));
DATA(insert (	2745   1028 1028 1 356 ));
DATA(insert (	2745   1028 1028 2 2743 ));
DATA(insert (	2745   1028 1028 3 2774 ));
DATA(insert (	2745   1028 1028 4 2744 ));
DATA(insert (	2745   1013 1013 1 404 ));
DATA(insert (	2745   1013 1013 2 2743 ));
DATA(insert (	2745   1013 1013 3 2774 ));
DATA(insert (	2745   1013 1013 4 2744 ));
DATA(insert (	2745   1183 1183 1 1107 ));
DATA(insert (	2745   1183 1183 2 2743 ));
DATA(insert (	2745   1183 1183 3 2774 ));
DATA(insert (	2745   1183 1183 4 2744 ));
DATA(insert (	2745   1185 1185 1 1314 ));
DATA(insert (	2745   1185 1185 2 2743 ));
DATA(insert (	2745   1185 1185 3 2774 ));
DATA(insert (	2745   1185 1185 4 2744 ));
DATA(insert (	2745   1270 1270 1 1358 ));
DATA(insert (	2745   1270 1270 2 2743 ));
DATA(insert (	2745   1270 1270 3 2774 ));
DATA(insert (	2745   1270 1270 4 2744 ));
DATA(insert (	2745   1563 1563 1 1672 ));
DATA(insert (	2745   1563 1563 2 2743 ));
DATA(insert (	2745   1563 1563 3 2774 ));
DATA(insert (	2745   1563 1563 4 2744 ));
DATA(insert (	2745   1115 1115 1 2045 ));
DATA(insert (	2745   1115 1115 2 2743 ));
DATA(insert (	2745   1115 1115 3 2774 ));
DATA(insert (	2745   1115 1115 4 2744 ));
DATA(insert (	2745   791 791 1 377 ));
DATA(insert (	2745   791 791 2 2743 ));
DATA(insert (	2745   791 791 3 2774 ));
DATA(insert (	2745   791 791 4 2744 ));
DATA(insert (	2745   1024 1024 1 380 ));
DATA(insert (	2745   1024 1024 2 2743 ));
DATA(insert (	2745   1024 1024 3 2774 ));
DATA(insert (	2745   1024 1024 4 2744 ));
DATA(insert (	2745   1025 1025 1 381 ));
DATA(insert (	2745   1025 1025 2 2743 ));
DATA(insert (	2745   1025 1025 3 2774 ));
DATA(insert (	2745   1025 1025 4 2744 ));
DATA(insert (	3659   3614 3614 1 360 ));
DATA(insert (	3659   3614 3614 2 3656 ));
DATA(insert (	3659   3614 3614 3 3657 ));
DATA(insert (	3659   3614 3614 4 3658 ));
DATA(insert (	3626   3614 3614 1 3622 ));
DATA(insert (	3683   3615 3615 1 3668 ));

#endif   /* PG_AMPROC_H */
