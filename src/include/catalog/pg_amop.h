/*-------------------------------------------------------------------------
 *
 * pg_amop.h
 *	  definition of the system "amop" relation (pg_amop)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_amop.h,v 1.29 2000/01/26 05:57:56 momjian Exp $
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
 *		postgres.h contains the system type definintions and the
 *		CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------
 *		pg_amop definition.  cpp turns this into
 *		typedef struct FormData_pg_amop
 * ----------------
 */
CATALOG(pg_amop)
{
	Oid			amopid;
	Oid			amopclaid;
	Oid			amopopr;
	int2		amopstrategy;
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
#define Natts_pg_amop					4
#define Anum_pg_amop_amopid				1
#define Anum_pg_amop_amopclaid			2
#define Anum_pg_amop_amopopr			3
#define Anum_pg_amop_amopstrategy		4

/* ----------------
 *		initial contents of pg_amop
 * ----------------
 */

/*
 *	rtree box_ops
 */

DATA(insert OID = 0 (  402 422 493 1 ));
DATA(insert OID = 0 (  402 422 494 2 ));
DATA(insert OID = 0 (  402 422 500 3 ));
DATA(insert OID = 0 (  402 422 495 4 ));
DATA(insert OID = 0 (  402 422 496 5 ));
DATA(insert OID = 0 (  402 422 499 6 ));
DATA(insert OID = 0 (  402 422 498 7 ));
DATA(insert OID = 0 (  402 422 497 8 ));

/*
 *	rtree bigbox_ops
 */

DATA(insert OID = 0 (  402 433 493 1 ));
DATA(insert OID = 0 (  402 433 494 2 ));
DATA(insert OID = 0 (  402 433 500 3 ));
DATA(insert OID = 0 (  402 433 495 4 ));
DATA(insert OID = 0 (  402 433 496 5 ));
DATA(insert OID = 0 (  402 433 499 6 ));
DATA(insert OID = 0 (  402 433 498 7 ));
DATA(insert OID = 0 (  402 433 497 8 ));

/*
 *	rtree poly_ops (supports polygons)
 */

DATA(insert OID = 0 (  402 434 485 1 ));
DATA(insert OID = 0 (  402 434 486 2 ));
DATA(insert OID = 0 (  402 434 492 3 ));
DATA(insert OID = 0 (  402 434 487 4 ));
DATA(insert OID = 0 (  402 434 488 5 ));
DATA(insert OID = 0 (  402 434 491 6 ));
DATA(insert OID = 0 (  402 434 490 7 ));
DATA(insert OID = 0 (  402 434 489 8 ));

/*
 *	rtree circle_ops (supports circles)
 */

DATA(insert OID = 0 (  402 714 1506 1 ));
DATA(insert OID = 0 (  402 714 1507 2 ));
DATA(insert OID = 0 (  402 714 1513 3 ));
DATA(insert OID = 0 (  402 714 1508 4 ));
DATA(insert OID = 0 (  402 714 1509 5 ));
DATA(insert OID = 0 (  402 714 1512 6 ));
DATA(insert OID = 0 (  402 714 1511 7 ));
DATA(insert OID = 0 (  402 714 1510 8 ));

/*
 *	nbtree int2_ops
 */

DATA(insert OID = 0 (  403 421	95 1 ));
DATA(insert OID = 0 (  403 421 522 2 ));
DATA(insert OID = 0 (  403 421	94 3 ));
DATA(insert OID = 0 (  403 421 524 4 ));
DATA(insert OID = 0 (  403 421 520 5 ));

/*
 *	nbtree float8_ops
 */

DATA(insert OID = 0 (  403 423 672 1 ));
DATA(insert OID = 0 (  403 423 673 2 ));
DATA(insert OID = 0 (  403 423 670 3 ));
DATA(insert OID = 0 (  403 423 675 4 ));
DATA(insert OID = 0 (  403 423 674 5 ));

/*
 *	nbtree int24_ops
 */

DATA(insert OID = 0 (  403 424 534 1 ));
DATA(insert OID = 0 (  403 424 540 2 ));
DATA(insert OID = 0 (  403 424 532 3 ));
DATA(insert OID = 0 (  403 424 542 4 ));
DATA(insert OID = 0 (  403 424 536 5 ));

/*
 *	nbtree int42_ops
 */

DATA(insert OID = 0 (  403 425 535 1 ));
DATA(insert OID = 0 (  403 425 541 2 ));
DATA(insert OID = 0 (  403 425 533 3 ));
DATA(insert OID = 0 (  403 425 543 4 ));
DATA(insert OID = 0 (  403 425 537 5 ));

/*
 *	nbtree int4_ops
 */

DATA(insert OID = 0 (  403 426	97 1 ));
DATA(insert OID = 0 (  403 426 523 2 ));
DATA(insert OID = 0 (  403 426	96 3 ));
DATA(insert OID = 0 (  403 426 525 4 ));
DATA(insert OID = 0 (  403 426 521 5 ));

/*
 *	nbtree int8_ops
 */

DATA(insert OID = 0 (  403 754 412 1 ));
DATA(insert OID = 0 (  403 754 414 2 ));
DATA(insert OID = 0 (  403 754 410 3 ));
DATA(insert OID = 0 (  403 754 415 4 ));
DATA(insert OID = 0 (  403 754 413 5 ));

/*
 *	nbtree oid_ops
 */

DATA(insert OID = 0 (  403 427 609 1 ));
DATA(insert OID = 0 (  403 427 611 2 ));
DATA(insert OID = 0 (  403 427 607 3 ));
DATA(insert OID = 0 (  403 427 612 4 ));
DATA(insert OID = 0 (  403 427 610 5 ));

/*
 *	nbtree oidvector_ops
 */

DATA(insert OID = 0 (  403 435	645 1 ));
DATA(insert OID = 0 (  403 435	647 2 ));
DATA(insert OID = 0 (  403 435	649 3 ));
DATA(insert OID = 0 (  403 435	648 4 ));
DATA(insert OID = 0 (  403 435	646 5 ));

/*
 *	nbtree float4_ops
 */

DATA(insert OID = 0 (  403 428 622 1 ));
DATA(insert OID = 0 (  403 428 624 2 ));
DATA(insert OID = 0 (  403 428 620 3 ));
DATA(insert OID = 0 (  403 428 625 4 ));
DATA(insert OID = 0 (  403 428 623 5 ));

/*
 *	nbtree char_ops
 */

DATA(insert OID = 0 (  403 429 631 1 ));
DATA(insert OID = 0 (  403 429 632 2 ));
DATA(insert OID = 0 (  403 429 92 3 ));
DATA(insert OID = 0 (  403 429 634 4 ));
DATA(insert OID = 0 (  403 429 633 5 ));

/*
 *	nbtree name_ops
 */

DATA(insert OID = 0 (  403 1181 660 1 ));
DATA(insert OID = 0 (  403 1181 661 2 ));
DATA(insert OID = 0 (  403 1181 93 3 ));
DATA(insert OID = 0 (  403 1181 663 4 ));
DATA(insert OID = 0 (  403 1181 662 5 ));

/*
 *	nbtree text_ops
 */

DATA(insert OID = 0 (  403 431 664 1 ));
DATA(insert OID = 0 (  403 431 665 2 ));
DATA(insert OID = 0 (  403 431 98 3 ));
DATA(insert OID = 0 (  403 431 667 4 ));
DATA(insert OID = 0 (  403 431 666 5 ));

/*
 *	nbtree abstime_ops
 */

DATA(insert OID = 0 (  403 432 562 1 ));
DATA(insert OID = 0 (  403 432 564 2 ));
DATA(insert OID = 0 (  403 432 560 3 ));
DATA(insert OID = 0 (  403 432 565 4 ));
DATA(insert OID = 0 (  403 432 563 5 ));

/*
 *	nbtree bpchar_ops
 */

DATA(insert OID = 0 (  403 1076 1058 1 ));
DATA(insert OID = 0 (  403 1076 1059 2 ));
DATA(insert OID = 0 (  403 1076 1054 3 ));
DATA(insert OID = 0 (  403 1076 1061 4 ));
DATA(insert OID = 0 (  403 1076 1060 5 ));

/*
 *	nbtree varchar_ops
 */

DATA(insert OID = 0 (  403 1077 1066 1 ));
DATA(insert OID = 0 (  403 1077 1067 2 ));
DATA(insert OID = 0 (  403 1077 1062 3 ));
DATA(insert OID = 0 (  403 1077 1069 4 ));
DATA(insert OID = 0 (  403 1077 1068 5 ));

/*
 *	nbtree date_ops
 */

DATA(insert OID = 0 (  403 1114 1095 1 ));
DATA(insert OID = 0 (  403 1114 1096 2 ));
DATA(insert OID = 0 (  403 1114 1093 3 ));
DATA(insert OID = 0 (  403 1114 1098 4 ));
DATA(insert OID = 0 (  403 1114 1097 5 ));


/*
 *	nbtree time_ops
 */

DATA(insert OID = 0 (  403 1115 1110 1 ));
DATA(insert OID = 0 (  403 1115 1111 2 ));
DATA(insert OID = 0 (  403 1115 1108 3 ));
DATA(insert OID = 0 (  403 1115 1113 4 ));
DATA(insert OID = 0 (  403 1115 1112 5 ));

/*
 *	nbtree datetime_ops
 */

DATA(insert OID = 0 (  403 1312 1322 1 ));
DATA(insert OID = 0 (  403 1312 1323 2 ));
DATA(insert OID = 0 (  403 1312 1320 3 ));
DATA(insert OID = 0 (  403 1312 1325 4 ));
DATA(insert OID = 0 (  403 1312 1324 5 ));

/*
 *	nbtree timespan_ops
 */

DATA(insert OID = 0 (  403 1313 1332 1 ));
DATA(insert OID = 0 (  403 1313 1333 2 ));
DATA(insert OID = 0 (  403 1313 1330 3 ));
DATA(insert OID = 0 (  403 1313 1335 4 ));
DATA(insert OID = 0 (  403 1313 1334 5 ));

/*
 *	nbtree macaddr
 */

DATA(insert OID = 0 (  403 810 1222 1 ));
DATA(insert OID = 0 (  403 810 1223 2 ));
DATA(insert OID = 0 (  403 810 1220 3 ));
DATA(insert OID = 0 (  403 810 1225 4 ));
DATA(insert OID = 0 (  403 810 1224 5 ));

/*
 *	nbtree inet
 */

DATA(insert OID = 0 (  403 935 1203 1 ));
DATA(insert OID = 0 (  403 935 1204 2 ));
DATA(insert OID = 0 (  403 935 1201 3 ));
DATA(insert OID = 0 (  403 935 1206 4 ));
DATA(insert OID = 0 (  403 935 1205 5 ));

/*
 *	nbtree cidr
 */

DATA(insert OID = 0 (  403 652 822 1 ));
DATA(insert OID = 0 (  403 652 823 2 ));
DATA(insert OID = 0 (  403 652 820 3 ));
DATA(insert OID = 0 (  403 652 825 4 ));
DATA(insert OID = 0 (  403 652 824 5 ));

/*
 *	nbtree numeric
 */

DATA(insert OID = 0 (  403 1768 1754 1 ));
DATA(insert OID = 0 (  403 1768 1755 2 ));
DATA(insert OID = 0 (  403 1768 1752 3 ));
DATA(insert OID = 0 (  403 1768 1757 4 ));
DATA(insert OID = 0 (  403 1768 1756 5 ));

/*
 *	hash table _ops
 */

/* int2_ops */
DATA(insert OID = 0 (  405	421   94 1 ));
/* float8_ops */
DATA(insert OID = 0 (  405	423  670 1 ));
/* int4_ops */
DATA(insert OID = 0 (  405	426   96 1 ));
/* int8_ops */
DATA(insert OID = 0 (  405	754  410 1 ));
/* oid_ops */
DATA(insert OID = 0 (  405	427  607 1 ));
/* oidvector_ops */
DATA(insert OID = 0 (  405	435  649 1 ));
/* float4_ops */
DATA(insert OID = 0 (  405	428  620 1 ));
/* char_ops */
DATA(insert OID = 0 (  405	429   92 1 ));
/* name_ops */
DATA(insert OID = 0 (  405 1181   93 1 ));
/* text_ops */
DATA(insert OID = 0 (  405	431   98 1 ));
/* bpchar_ops */
DATA(insert OID = 0 (  405 1076 1054 1 ));
/* varchar_ops */
DATA(insert OID = 0 (  405 1077 1062 1 ));
/* date_ops */
DATA(insert OID = 0 (  405 1114 1093 1 ));
/* time_ops */
DATA(insert OID = 0 (  405 1115 1108 1 ));
/* datetime_ops */
DATA(insert OID = 0 (  405 1312 1320 1 ));
/* timespan_ops */
DATA(insert OID = 0 (  405 1313 1330 1 ));
/* macaddr_ops */
DATA(insert OID = 0 (  405 810 1220 1 ));
/* inet_ops */
DATA(insert OID = 0 (  405 935 1201 1 ));
/* cidr_ops */
DATA(insert OID = 0 (  405 652 820 1 ));

#endif	 /* PG_AMOP_H */
