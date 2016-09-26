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
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_amproc.h
 *
 * NOTES
 *	  the genbki.pl script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_AMPROC_H
#define PG_AMPROC_H

#include "catalog/genbki.h"

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
	int16		amprocnum;		/* support procedure index */
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
DATA(insert (	426   1042 1042 2 3328 ));
DATA(insert (	428   17 17 1 1954 ));
DATA(insert (	428   17 17 2 3331 ));
DATA(insert (	429   18 18 1 358 ));
DATA(insert (	434   1082 1082 1 1092 ));
DATA(insert (	434   1082 1082 2 3136 ));
DATA(insert (	434   1082 1114 1 2344 ));
DATA(insert (	434   1082 1184 1 2357 ));
DATA(insert (	434   1114 1114 1 2045 ));
DATA(insert (	434   1114 1114 2 3137 ));
DATA(insert (	434   1114 1082 1 2370 ));
DATA(insert (	434   1114 1184 1 2526 ));
DATA(insert (	434   1184 1184 1 1314 ));
DATA(insert (	434   1184 1184 2 3137 ));
DATA(insert (	434   1184 1082 1 2383 ));
DATA(insert (	434   1184 1114 1 2533 ));
DATA(insert (	1970   700 700 1 354 ));
DATA(insert (	1970   700 700 2 3132 ));
DATA(insert (	1970   700 701 1 2194 ));
DATA(insert (	1970   701 701 1 355 ));
DATA(insert (	1970   701 701 2 3133 ));
DATA(insert (	1970   701 700 1 2195 ));
DATA(insert (	1974   869 869 1 926 ));
DATA(insert (	1976   21 21 1 350 ));
DATA(insert (	1976   21 21 2 3129 ));
DATA(insert (	1976   21 23 1 2190 ));
DATA(insert (	1976   21 20 1 2192 ));
DATA(insert (	1976   23 23 1 351 ));
DATA(insert (	1976   23 23 2 3130 ));
DATA(insert (	1976   23 20 1 2188 ));
DATA(insert (	1976   23 21 1 2191 ));
DATA(insert (	1976   20 20 1 842 ));
DATA(insert (	1976   20 20 2 3131 ));
DATA(insert (	1976   20 23 1 2189 ));
DATA(insert (	1976   20 21 1 2193 ));
DATA(insert (	1982   1186 1186 1 1315 ));
DATA(insert (	1984   829 829 1 836 ));
DATA(insert (	1986   19 19 1 359 ));
DATA(insert (	1986   19 19 2 3135 ));
DATA(insert (	1988   1700 1700 1 1769 ));
DATA(insert (	1988   1700 1700 2 3283 ));
DATA(insert (	1989   26 26 1 356 ));
DATA(insert (	1989   26 26 2 3134 ));
DATA(insert (	1991   30 30 1 404 ));
DATA(insert (	1994   25 25 1 360 ));
DATA(insert (	1994   25 25 2 3255 ));
DATA(insert (	1996   1083 1083 1 1107 ));
DATA(insert (	2000   1266 1266 1 1358 ));
DATA(insert (	2002   1562 1562 1 1672 ));
DATA(insert (	2095   25 25 1 2166 ));
DATA(insert (	2095   25 25 2 3332 ));
DATA(insert (	2097   1042 1042 1 2180 ));
DATA(insert (	2097   1042 1042 2 3333 ));
DATA(insert (	2099   790 790 1  377 ));
DATA(insert (	2233   703 703 1  380 ));
DATA(insert (	2234   704 704 1  381 ));
DATA(insert (	2789   27 27 1 2794 ));
DATA(insert (	2968   2950 2950 1 2960 ));
DATA(insert (	2968   2950 2950 2 3300 ));
DATA(insert (	2994   2249 2249 1 2987 ));
DATA(insert (	3194   2249 2249 1 3187 ));
DATA(insert (	3253   3220 3220 1 3251 ));
DATA(insert (	3522   3500 3500 1 3514 ));
DATA(insert (	3626   3614 3614 1 3622 ));
DATA(insert (	3683   3615 3615 1 3668 ));
DATA(insert (	3901   3831 3831 1 3870 ));
DATA(insert (	4033   3802 3802 1 4044 ));


/* hash */
DATA(insert (	427   1042 1042 1 1080 ));
DATA(insert (	431   18 18 1 454 ));
DATA(insert (	435   1082 1082 1 450 ));
DATA(insert (	627   2277 2277 1 626 ));
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
DATA(insert (	2229   25 25 1 400 ));
DATA(insert (	2231   1042 1042 1 1080 ));
DATA(insert (	2235   1033 1033 1 329 ));
DATA(insert (	2969   2950 2950 1 2963 ));
DATA(insert (	3254   3220 3220 1 3252 ));
DATA(insert (	3523   3500 3500 1 3515 ));
DATA(insert (	3903   3831 3831 1 3902 ));
DATA(insert (	4034   3802 3802 1 4045 ));


/* gist */
DATA(insert (	1029   600 600 1 2179 ));
DATA(insert (	1029   600 600 2 2583 ));
DATA(insert (	1029   600 600 3 1030 ));
DATA(insert (	1029   600 600 4 2580 ));
DATA(insert (	1029   600 600 5 2581 ));
DATA(insert (	1029   600 600 6 2582 ));
DATA(insert (	1029   600 600 7 2584 ));
DATA(insert (	1029   600 600 8 3064 ));
DATA(insert (	1029   600 600 9 3282 ));
DATA(insert (	2593   603 603 1 2578 ));
DATA(insert (	2593   603 603 2 2583 ));
DATA(insert (	2593   603 603 3 2579 ));
DATA(insert (	2593   603 603 4 2580 ));
DATA(insert (	2593   603 603 5 2581 ));
DATA(insert (	2593   603 603 6 2582 ));
DATA(insert (	2593   603 603 7 2584 ));
DATA(insert (	2593   603 603 9 3281 ));
DATA(insert (	2594   604 604 1 2585 ));
DATA(insert (	2594   604 604 2 2583 ));
DATA(insert (	2594   604 604 3 2586 ));
DATA(insert (	2594   604 604 4 2580 ));
DATA(insert (	2594   604 604 5 2581 ));
DATA(insert (	2594   604 604 6 2582 ));
DATA(insert (	2594   604 604 7 2584 ));
DATA(insert (	2594   604 604 8 3288 ));
DATA(insert (	2595   718 718 1 2591 ));
DATA(insert (	2595   718 718 2 2583 ));
DATA(insert (	2595   718 718 3 2592 ));
DATA(insert (	2595   718 718 4 2580 ));
DATA(insert (	2595   718 718 5 2581 ));
DATA(insert (	2595   718 718 6 2582 ));
DATA(insert (	2595   718 718 7 2584 ));
DATA(insert (	2595   718 718 8 3280 ));
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
DATA(insert (	3919   3831 3831 1 3875 ));
DATA(insert (	3919   3831 3831 2 3876 ));
DATA(insert (	3919   3831 3831 3 3877 ));
DATA(insert (	3919   3831 3831 4 3878 ));
DATA(insert (	3919   3831 3831 5 3879 ));
DATA(insert (	3919   3831 3831 6 3880 ));
DATA(insert (	3919   3831 3831 7 3881 ));
DATA(insert (	3919   3831 3831 9 3996 ));
DATA(insert (	3550   869 869 1 3553 ));
DATA(insert (	3550   869 869 2 3554 ));
DATA(insert (	3550   869 869 3 3555 ));
DATA(insert (	3550   869 869 4 3556 ));
DATA(insert (	3550   869 869 5 3557 ));
DATA(insert (	3550   869 869 6 3558 ));
DATA(insert (	3550   869 869 7 3559 ));
DATA(insert (	3550   869 869 9 3573 ));


/* gin */
DATA(insert (	2745   2277 2277 2 2743 ));
DATA(insert (	2745   2277 2277 3 2774 ));
DATA(insert (	2745   2277 2277 4 2744 ));
DATA(insert (	2745   2277 2277 6 3920 ));
DATA(insert (	3659   3614 3614 1 3724 ));
DATA(insert (	3659   3614 3614 2 3656 ));
DATA(insert (	3659   3614 3614 3 3657 ));
DATA(insert (	3659   3614 3614 4 3658 ));
DATA(insert (	3659   3614 3614 5 2700 ));
DATA(insert (	3659   3614 3614 6 3921 ));
DATA(insert (	4036   3802 3802 1 3480 ));
DATA(insert (	4036   3802 3802 2 3482 ));
DATA(insert (	4036   3802 3802 3 3483 ));
DATA(insert (	4036   3802 3802 4 3484 ));
DATA(insert (	4036   3802 3802 6 3488 ));
DATA(insert (	4037   3802 3802 1 351 ));
DATA(insert (	4037   3802 3802 2 3485 ));
DATA(insert (	4037   3802 3802 3 3486 ));
DATA(insert (	4037   3802 3802 4 3487 ));
DATA(insert (	4037   3802 3802 6 3489 ));

/* sp-gist */
DATA(insert (	3474   3831 3831 1 3469 ));
DATA(insert (	3474   3831 3831 2 3470 ));
DATA(insert (	3474   3831 3831 3 3471 ));
DATA(insert (	3474   3831 3831 4 3472 ));
DATA(insert (	3474   3831 3831 5 3473 ));
DATA(insert (	3794   869 869 1 3795 ));
DATA(insert (	3794   869 869 2 3796 ));
DATA(insert (	3794   869 869 3 3797 ));
DATA(insert (	3794   869 869 4 3798 ));
DATA(insert (	3794   869 869 5 3799 ));
DATA(insert (	4015   600 600 1 4018 ));
DATA(insert (	4015   600 600 2 4019 ));
DATA(insert (	4015   600 600 3 4020 ));
DATA(insert (	4015   600 600 4 4021 ));
DATA(insert (	4015   600 600 5 4022 ));
DATA(insert (	4016   600 600 1 4023 ));
DATA(insert (	4016   600 600 2 4024 ));
DATA(insert (	4016   600 600 3 4025 ));
DATA(insert (	4016   600 600 4 4026 ));
DATA(insert (	4016   600 600 5 4022 ));
DATA(insert (	4017   25 25 1 4027 ));
DATA(insert (	4017   25 25 2 4028 ));
DATA(insert (	4017   25 25 3 4029 ));
DATA(insert (	4017   25 25 4 4030 ));
DATA(insert (	4017   25 25 5 4031 ));
DATA(insert (	5000   603 603 1 5012 ));
DATA(insert (	5000   603 603 2 5013 ));
DATA(insert (	5000   603 603 3 5014 ));
DATA(insert (	5000   603 603 4 5015 ));
DATA(insert (	5000   603 603 5 5016 ));

/* BRIN opclasses */
/* minmax bytea */
DATA(insert (	4064	17	  17  1  3383 ));
DATA(insert (	4064	17	  17  2  3384 ));
DATA(insert (	4064	17	  17  3  3385 ));
DATA(insert (	4064	17	  17  4  3386 ));
/* minmax "char" */
DATA(insert (	4062	18	  18  1  3383 ));
DATA(insert (	4062	18	  18  2  3384 ));
DATA(insert (	4062	18	  18  3  3385 ));
DATA(insert (	4062	18	  18  4  3386 ));
/* minmax name */
DATA(insert (	4065	19	  19  1  3383 ));
DATA(insert (	4065	19	  19  2  3384 ));
DATA(insert (	4065	19	  19  3  3385 ));
DATA(insert (	4065	19	  19  4  3386 ));
/* minmax integer: int2, int4, int8 */
DATA(insert (	4054	20	  20  1  3383 ));
DATA(insert (	4054	20	  20  2  3384 ));
DATA(insert (	4054	20	  20  3  3385 ));
DATA(insert (	4054	20	  20  4  3386 ));
DATA(insert (	4054	20	  21  1  3383 ));
DATA(insert (	4054	20	  21  2  3384 ));
DATA(insert (	4054	20	  21  3  3385 ));
DATA(insert (	4054	20	  21  4  3386 ));
DATA(insert (	4054	20	  23  1  3383 ));
DATA(insert (	4054	20	  23  2  3384 ));
DATA(insert (	4054	20	  23  3  3385 ));
DATA(insert (	4054	20	  23  4  3386 ));

DATA(insert (	4054	21	  21  1  3383 ));
DATA(insert (	4054	21	  21  2  3384 ));
DATA(insert (	4054	21	  21  3  3385 ));
DATA(insert (	4054	21	  21  4  3386 ));
DATA(insert (	4054	21	  20  1  3383 ));
DATA(insert (	4054	21	  20  2  3384 ));
DATA(insert (	4054	21	  20  3  3385 ));
DATA(insert (	4054	21	  20  4  3386 ));
DATA(insert (	4054	21	  23  1  3383 ));
DATA(insert (	4054	21	  23  2  3384 ));
DATA(insert (	4054	21	  23  3  3385 ));
DATA(insert (	4054	21	  23  4  3386 ));

DATA(insert (	4054	23	  23  1  3383 ));
DATA(insert (	4054	23	  23  2  3384 ));
DATA(insert (	4054	23	  23  3  3385 ));
DATA(insert (	4054	23	  23  4  3386 ));
DATA(insert (	4054	23	  20  1  3383 ));
DATA(insert (	4054	23	  20  2  3384 ));
DATA(insert (	4054	23	  20  3  3385 ));
DATA(insert (	4054	23	  20  4  3386 ));
DATA(insert (	4054	23	  21  1  3383 ));
DATA(insert (	4054	23	  21  2  3384 ));
DATA(insert (	4054	23	  21  3  3385 ));
DATA(insert (	4054	23	  21  4  3386 ));

/* minmax text */
DATA(insert (	4056	25	  25  1  3383 ));
DATA(insert (	4056	25	  25  2  3384 ));
DATA(insert (	4056	25	  25  3  3385 ));
DATA(insert (	4056	25	  25  4  3386 ));
/* minmax oid */
DATA(insert (	4068	26	  26  1  3383 ));
DATA(insert (	4068	26	  26  2  3384 ));
DATA(insert (	4068	26	  26  3  3385 ));
DATA(insert (	4068	26	  26  4  3386 ));
/* minmax tid */
DATA(insert (	4069	27	  27  1  3383 ));
DATA(insert (	4069	27	  27  2  3384 ));
DATA(insert (	4069	27	  27  3  3385 ));
DATA(insert (	4069	27	  27  4  3386 ));
/* minmax float */
DATA(insert (	4070   700	 700  1  3383 ));
DATA(insert (	4070   700	 700  2  3384 ));
DATA(insert (	4070   700	 700  3  3385 ));
DATA(insert (	4070   700	 700  4  3386 ));

DATA(insert (	4070   700	 701  1  3383 ));
DATA(insert (	4070   700	 701  2  3384 ));
DATA(insert (	4070   700	 701  3  3385 ));
DATA(insert (	4070   700	 701  4  3386 ));

DATA(insert (	4070   701	 701  1  3383 ));
DATA(insert (	4070   701	 701  2  3384 ));
DATA(insert (	4070   701	 701  3  3385 ));
DATA(insert (	4070   701	 701  4  3386 ));

DATA(insert (	4070   701	 700  1  3383 ));
DATA(insert (	4070   701	 700  2  3384 ));
DATA(insert (	4070   701	 700  3  3385 ));
DATA(insert (	4070   701	 700  4  3386 ));

/* minmax abstime */
DATA(insert (	4072   702	 702  1  3383 ));
DATA(insert (	4072   702	 702  2  3384 ));
DATA(insert (	4072   702	 702  3  3385 ));
DATA(insert (	4072   702	 702  4  3386 ));
/* minmax reltime */
DATA(insert (	4073   703	 703  1  3383 ));
DATA(insert (	4073   703	 703  2  3384 ));
DATA(insert (	4073   703	 703  3  3385 ));
DATA(insert (	4073   703	 703  4  3386 ));
/* minmax macaddr */
DATA(insert (	4074   829	 829  1  3383 ));
DATA(insert (	4074   829	 829  2  3384 ));
DATA(insert (	4074   829	 829  3  3385 ));
DATA(insert (	4074   829	 829  4  3386 ));
/* minmax inet */
DATA(insert (	4075   869	 869  1  3383 ));
DATA(insert (	4075   869	 869  2  3384 ));
DATA(insert (	4075   869	 869  3  3385 ));
DATA(insert (	4075   869	 869  4  3386 ));
/* inclusion inet */
DATA(insert (	4102   869	 869  1  4105 ));
DATA(insert (	4102   869	 869  2  4106 ));
DATA(insert (	4102   869	 869  3  4107 ));
DATA(insert (	4102   869	 869  4  4108 ));
DATA(insert (	4102   869	 869 11  4063 ));
DATA(insert (	4102   869	 869 12  4071 ));
DATA(insert (	4102   869	 869 13   930 ));
/* minmax character */
DATA(insert (	4076  1042	1042  1  3383 ));
DATA(insert (	4076  1042	1042  2  3384 ));
DATA(insert (	4076  1042	1042  3  3385 ));
DATA(insert (	4076  1042	1042  4  3386 ));
/* minmax time without time zone */
DATA(insert (	4077  1083	1083  1  3383 ));
DATA(insert (	4077  1083	1083  2  3384 ));
DATA(insert (	4077  1083	1083  3  3385 ));
DATA(insert (	4077  1083	1083  4  3386 ));
/* minmax datetime (date, timestamp, timestamptz) */
DATA(insert (	4059  1114	1114  1  3383 ));
DATA(insert (	4059  1114	1114  2  3384 ));
DATA(insert (	4059  1114	1114  3  3385 ));
DATA(insert (	4059  1114	1114  4  3386 ));
DATA(insert (	4059  1114	1184  1  3383 ));
DATA(insert (	4059  1114	1184  2  3384 ));
DATA(insert (	4059  1114	1184  3  3385 ));
DATA(insert (	4059  1114	1184  4  3386 ));
DATA(insert (	4059  1114	1082  1  3383 ));
DATA(insert (	4059  1114	1082  2  3384 ));
DATA(insert (	4059  1114	1082  3  3385 ));
DATA(insert (	4059  1114	1082  4  3386 ));

DATA(insert (	4059  1184	1184  1  3383 ));
DATA(insert (	4059  1184	1184  2  3384 ));
DATA(insert (	4059  1184	1184  3  3385 ));
DATA(insert (	4059  1184	1184  4  3386 ));
DATA(insert (	4059  1184	1114  1  3383 ));
DATA(insert (	4059  1184	1114  2  3384 ));
DATA(insert (	4059  1184	1114  3  3385 ));
DATA(insert (	4059  1184	1114  4  3386 ));
DATA(insert (	4059  1184	1082  1  3383 ));
DATA(insert (	4059  1184	1082  2  3384 ));
DATA(insert (	4059  1184	1082  3  3385 ));
DATA(insert (	4059  1184	1082  4  3386 ));

DATA(insert (	4059  1082	1082  1  3383 ));
DATA(insert (	4059  1082	1082  2  3384 ));
DATA(insert (	4059  1082	1082  3  3385 ));
DATA(insert (	4059  1082	1082  4  3386 ));
DATA(insert (	4059  1082	1114  1  3383 ));
DATA(insert (	4059  1082	1114  2  3384 ));
DATA(insert (	4059  1082	1114  3  3385 ));
DATA(insert (	4059  1082	1114  4  3386 ));
DATA(insert (	4059  1082	1184  1  3383 ));
DATA(insert (	4059  1082	1184  2  3384 ));
DATA(insert (	4059  1082	1184  3  3385 ));
DATA(insert (	4059  1082	1184  4  3386 ));

/* minmax interval */
DATA(insert (	4078  1186	1186  1  3383 ));
DATA(insert (	4078  1186	1186  2  3384 ));
DATA(insert (	4078  1186	1186  3  3385 ));
DATA(insert (	4078  1186	1186  4  3386 ));
/* minmax time with time zone */
DATA(insert (	4058  1266	1266  1  3383 ));
DATA(insert (	4058  1266	1266  2  3384 ));
DATA(insert (	4058  1266	1266  3  3385 ));
DATA(insert (	4058  1266	1266  4  3386 ));
/* minmax bit */
DATA(insert (	4079  1560	1560  1  3383 ));
DATA(insert (	4079  1560	1560  2  3384 ));
DATA(insert (	4079  1560	1560  3  3385 ));
DATA(insert (	4079  1560	1560  4  3386 ));
/* minmax bit varying */
DATA(insert (	4080  1562	1562  1  3383 ));
DATA(insert (	4080  1562	1562  2  3384 ));
DATA(insert (	4080  1562	1562  3  3385 ));
DATA(insert (	4080  1562	1562  4  3386 ));
/* minmax numeric */
DATA(insert (	4055  1700	1700  1  3383 ));
DATA(insert (	4055  1700	1700  2  3384 ));
DATA(insert (	4055  1700	1700  3  3385 ));
DATA(insert (	4055  1700	1700  4  3386 ));
/* minmax uuid */
DATA(insert (	4081  2950	2950  1  3383 ));
DATA(insert (	4081  2950	2950  2  3384 ));
DATA(insert (	4081  2950	2950  3  3385 ));
DATA(insert (	4081  2950	2950  4  3386 ));
/* inclusion range types */
DATA(insert (	4103  3831	3831  1  4105 ));
DATA(insert (	4103  3831	3831  2  4106 ));
DATA(insert (	4103  3831	3831  3  4107 ));
DATA(insert (	4103  3831	3831  4  4108 ));
DATA(insert (	4103  3831	3831  11 4057 ));
DATA(insert (	4103  3831	3831  13 3859 ));
DATA(insert (	4103  3831	3831  14 3850 ));
/* minmax pg_lsn */
DATA(insert (	4082  3220	3220  1  3383 ));
DATA(insert (	4082  3220	3220  2  3384 ));
DATA(insert (	4082  3220	3220  3  3385 ));
DATA(insert (	4082  3220	3220  4  3386 ));
/* inclusion box */
DATA(insert (	4104   603	 603  1  4105 ));
DATA(insert (	4104   603	 603  2  4106 ));
DATA(insert (	4104   603	 603  3  4107 ));
DATA(insert (	4104   603	 603  4  4108 ));
DATA(insert (	4104   603	 603  11 4067 ));
DATA(insert (	4104   603	 603  13  187 ));

#endif   /* PG_AMPROC_H */
