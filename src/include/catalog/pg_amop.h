/*-------------------------------------------------------------------------
 *
 * pg_amop.h
 *	  definition of the system "amop" relation (pg_amop)
 *	  along with the relation's initial contents.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_amop.h,v 1.19 1999/02/13 23:21:05 momjian Exp $
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
	regproc		amopselect;
	regproc		amopnpages;
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
/* #define Name_pg_amop					"pg_amop" */
#define Natts_pg_amop					6
#define Anum_pg_amop_amopid				1
#define Anum_pg_amop_amopclaid			2
#define Anum_pg_amop_amopopr			3
#define Anum_pg_amop_amopstrategy		4
#define Anum_pg_amop_amopselect			5
#define Anum_pg_amop_amopnpages			6

/* ----------------
 *		initial contents of pg_amop
 * ----------------
 */

/*
 *	rtree box_ops
 */

DATA(insert OID = 0 (  402 422 493 1 rtsel rtnpage ));
DATA(insert OID = 0 (  402 422 494 2 rtsel rtnpage ));
DATA(insert OID = 0 (  402 422 500 3 rtsel rtnpage ));
DATA(insert OID = 0 (  402 422 495 4 rtsel rtnpage ));
DATA(insert OID = 0 (  402 422 496 5 rtsel rtnpage ));
DATA(insert OID = 0 (  402 422 499 6 rtsel rtnpage ));
DATA(insert OID = 0 (  402 422 498 7 rtsel rtnpage ));
DATA(insert OID = 0 (  402 422 497 8 rtsel rtnpage ));

/*
 *	rtree bigbox_ops
 */

DATA(insert OID = 0 (  402 433 493 1 rtsel rtnpage ));
DATA(insert OID = 0 (  402 433 494 2 rtsel rtnpage ));
DATA(insert OID = 0 (  402 433 500 3 rtsel rtnpage ));
DATA(insert OID = 0 (  402 433 495 4 rtsel rtnpage ));
DATA(insert OID = 0 (  402 433 496 5 rtsel rtnpage ));
DATA(insert OID = 0 (  402 433 499 6 rtsel rtnpage ));
DATA(insert OID = 0 (  402 433 498 7 rtsel rtnpage ));
DATA(insert OID = 0 (  402 433 497 8 rtsel rtnpage ));

/*
 *	rtree poly_ops (supports polygons)
 */

DATA(insert OID = 0 (  402 434 485 1 rtsel rtnpage ));
DATA(insert OID = 0 (  402 434 486 2 rtsel rtnpage ));
DATA(insert OID = 0 (  402 434 487 3 rtsel rtnpage ));
DATA(insert OID = 0 (  402 434 488 4 rtsel rtnpage ));
DATA(insert OID = 0 (  402 434 489 5 rtsel rtnpage ));
DATA(insert OID = 0 (  402 434 490 6 rtsel rtnpage ));
DATA(insert OID = 0 (  402 434 491 7 rtsel rtnpage ));
DATA(insert OID = 0 (  402 434 492 8 rtsel rtnpage ));

/*
 *	rtree circle_ops (supports circles)
 */

DATA(insert OID = 0 (  402 714 1506 1 rtsel rtnpage ));
DATA(insert OID = 0 (  402 714 1507 2 rtsel rtnpage ));
DATA(insert OID = 0 (  402 714 1508 3 rtsel rtnpage ));
DATA(insert OID = 0 (  402 714 1509 4 rtsel rtnpage ));
DATA(insert OID = 0 (  402 714 1510 5 rtsel rtnpage ));
DATA(insert OID = 0 (  402 714 1511 6 rtsel rtnpage ));
DATA(insert OID = 0 (  402 714 1512 7 rtsel rtnpage ));
DATA(insert OID = 0 (  402 714 1513 8 rtsel rtnpage ));

/*
 *	nbtree int2_ops
 */

DATA(insert OID = 0 (  403 421	95 1 btreesel btreenpage ));
DATA(insert OID = 0 (  403 421 522 2 btreesel btreenpage ));
DATA(insert OID = 0 (  403 421	94 3 btreesel btreenpage ));
DATA(insert OID = 0 (  403 421 524 4 btreesel btreenpage ));
DATA(insert OID = 0 (  403 421 520 5 btreesel btreenpage ));

/*
 *	nbtree float8_ops
 */

DATA(insert OID = 0 (  403 423 672 1 btreesel btreenpage ));
DATA(insert OID = 0 (  403 423 673 2 btreesel btreenpage ));
DATA(insert OID = 0 (  403 423 670 3 btreesel btreenpage ));
DATA(insert OID = 0 (  403 423 675 4 btreesel btreenpage ));
DATA(insert OID = 0 (  403 423 674 5 btreesel btreenpage ));

/*
 *	nbtree int24_ops
 */

DATA(insert OID = 0 (  403 424 534 1 btreesel btreenpage ));
DATA(insert OID = 0 (  403 424 540 2 btreesel btreenpage ));
DATA(insert OID = 0 (  403 424 532 3 btreesel btreenpage ));
DATA(insert OID = 0 (  403 424 542 4 btreesel btreenpage ));
DATA(insert OID = 0 (  403 424 536 5 btreesel btreenpage ));

/*
 *	nbtree int42_ops
 */

DATA(insert OID = 0 (  403 425 535 1 btreesel btreenpage ));
DATA(insert OID = 0 (  403 425 541 2 btreesel btreenpage ));
DATA(insert OID = 0 (  403 425 533 3 btreesel btreenpage ));
DATA(insert OID = 0 (  403 425 543 4 btreesel btreenpage ));
DATA(insert OID = 0 (  403 425 537 5 btreesel btreenpage ));

/*
 *	nbtree int4_ops
 */

DATA(insert OID = 0 (  403 426	97 1 btreesel btreenpage ));
DATA(insert OID = 0 (  403 426 523 2 btreesel btreenpage ));
DATA(insert OID = 0 (  403 426	96 3 btreesel btreenpage ));
DATA(insert OID = 0 (  403 426 525 4 btreesel btreenpage ));
DATA(insert OID = 0 (  403 426 521 5 btreesel btreenpage ));

/*
 *	nbtree oid_ops
 */

DATA(insert OID = 0 (  403 427 609 1 btreesel btreenpage ));
DATA(insert OID = 0 (  403 427 611 2 btreesel btreenpage ));
DATA(insert OID = 0 (  403 427 607 3 btreesel btreenpage ));
DATA(insert OID = 0 (  403 427 612 4 btreesel btreenpage ));
DATA(insert OID = 0 (  403 427 610 5 btreesel btreenpage ));

/*
 *	nbtree oid8_ops
 */

DATA(insert OID = 0 (  403 435	645 1 btreesel btreenpage ));
DATA(insert OID = 0 (  403 435	647 2 btreesel btreenpage ));
DATA(insert OID = 0 (  403 435	649 3 btreesel btreenpage ));
DATA(insert OID = 0 (  403 435	648 4 btreesel btreenpage ));
DATA(insert OID = 0 (  403 435	646 5 btreesel btreenpage ));

/*
 *	nbtree float4_ops
 */

DATA(insert OID = 0 (  403 428 622 1 btreesel btreenpage ));
DATA(insert OID = 0 (  403 428 624 2 btreesel btreenpage ));
DATA(insert OID = 0 (  403 428 620 3 btreesel btreenpage ));
DATA(insert OID = 0 (  403 428 625 4 btreesel btreenpage ));
DATA(insert OID = 0 (  403 428 623 5 btreesel btreenpage ));

/*
 *	nbtree char_ops
 */

DATA(insert OID = 0 (  403 429 631 1 btreesel btreenpage ));
DATA(insert OID = 0 (  403 429 632 2 btreesel btreenpage ));
DATA(insert OID = 0 (  403 429 92 3 btreesel btreenpage ));
DATA(insert OID = 0 (  403 429 634 4 btreesel btreenpage ));
DATA(insert OID = 0 (  403 429 633 5 btreesel btreenpage ));

/*
 *	nbtree name_ops
 */

DATA(insert OID = 0 (  403 1181 660 1 btreesel btreenpage ));
DATA(insert OID = 0 (  403 1181 661 2 btreesel btreenpage ));
DATA(insert OID = 0 (  403 1181 93 3 btreesel btreenpage ));
DATA(insert OID = 0 (  403 1181 663 4 btreesel btreenpage ));
DATA(insert OID = 0 (  403 1181 662 5 btreesel btreenpage ));

/*
 *	nbtree text_ops
 */

DATA(insert OID = 0 (  403 431 664 1 btreesel btreenpage ));
DATA(insert OID = 0 (  403 431 665 2 btreesel btreenpage ));
DATA(insert OID = 0 (  403 431 98 3 btreesel btreenpage ));
DATA(insert OID = 0 (  403 431 667 4 btreesel btreenpage ));
DATA(insert OID = 0 (  403 431 666 5 btreesel btreenpage ));

/*
 *	nbtree abstime_ops
 */

DATA(insert OID = 0 (  403 432 562 1 btreesel btreenpage ));
DATA(insert OID = 0 (  403 432 564 2 btreesel btreenpage ));
DATA(insert OID = 0 (  403 432 560 3 btreesel btreenpage ));
DATA(insert OID = 0 (  403 432 565 4 btreesel btreenpage ));
DATA(insert OID = 0 (  403 432 563 5 btreesel btreenpage ));

/*
 *	nbtree bpchar_ops
 */

DATA(insert OID = 0 (  403 1076 1058 1 btreesel btreenpage ));
DATA(insert OID = 0 (  403 1076 1059 2 btreesel btreenpage ));
DATA(insert OID = 0 (  403 1076 1054 3 btreesel btreenpage ));
DATA(insert OID = 0 (  403 1076 1061 4 btreesel btreenpage ));
DATA(insert OID = 0 (  403 1076 1060 5 btreesel btreenpage ));

/*
 *	nbtree varchar_ops
 */

DATA(insert OID = 0 (  403 1077 1066 1 btreesel btreenpage ));
DATA(insert OID = 0 (  403 1077 1067 2 btreesel btreenpage ));
DATA(insert OID = 0 (  403 1077 1062 3 btreesel btreenpage ));
DATA(insert OID = 0 (  403 1077 1069 4 btreesel btreenpage ));
DATA(insert OID = 0 (  403 1077 1068 5 btreesel btreenpage ));

/*
 *	nbtree date_ops
 */

DATA(insert OID = 0 (  403 1114 1095 1 btreesel btreenpage ));
DATA(insert OID = 0 (  403 1114 1096 2 btreesel btreenpage ));
DATA(insert OID = 0 (  403 1114 1093 3 btreesel btreenpage ));
DATA(insert OID = 0 (  403 1114 1098 4 btreesel btreenpage ));
DATA(insert OID = 0 (  403 1114 1097 5 btreesel btreenpage ));


/*
 *	nbtree time_ops
 */

DATA(insert OID = 0 (  403 1115 1110 1 btreesel btreenpage ));
DATA(insert OID = 0 (  403 1115 1111 2 btreesel btreenpage ));
DATA(insert OID = 0 (  403 1115 1108 3 btreesel btreenpage ));
DATA(insert OID = 0 (  403 1115 1113 4 btreesel btreenpage ));
DATA(insert OID = 0 (  403 1115 1112 5 btreesel btreenpage ));

/*
 *	nbtree datetime_ops
 */

DATA(insert OID = 0 (  403 1312 1322 1 btreesel btreenpage ));
DATA(insert OID = 0 (  403 1312 1323 2 btreesel btreenpage ));
DATA(insert OID = 0 (  403 1312 1320 3 btreesel btreenpage ));
DATA(insert OID = 0 (  403 1312 1325 4 btreesel btreenpage ));
DATA(insert OID = 0 (  403 1312 1324 5 btreesel btreenpage ));

/*
 *	nbtree timespan_ops
 */

DATA(insert OID = 0 (  403 1313 1332 1 btreesel btreenpage ));
DATA(insert OID = 0 (  403 1313 1333 2 btreesel btreenpage ));
DATA(insert OID = 0 (  403 1313 1330 3 btreesel btreenpage ));
DATA(insert OID = 0 (  403 1313 1335 4 btreesel btreenpage ));
DATA(insert OID = 0 (  403 1313 1334 5 btreesel btreenpage ));

/*
 *	nbtree macaddr
 */

DATA(insert OID = 0 (  403 810 1222 1 btreesel btreenpage ));
DATA(insert OID = 0 (  403 810 1223 2 btreesel btreenpage ));
DATA(insert OID = 0 (  403 810 1220 3 btreesel btreenpage ));
DATA(insert OID = 0 (  403 810 1225 4 btreesel btreenpage ));
DATA(insert OID = 0 (  403 810 1224 5 btreesel btreenpage ));

/*
 *	nbtree inet
 */

DATA(insert OID = 0 (  403 935 1203 1 btreesel btreenpage ));
DATA(insert OID = 0 (  403 935 1204 2 btreesel btreenpage ));
DATA(insert OID = 0 (  403 935 1201 3 btreesel btreenpage ));
DATA(insert OID = 0 (  403 935 1206 4 btreesel btreenpage ));
DATA(insert OID = 0 (  403 935 1205 5 btreesel btreenpage ));

/*
 *	nbtree cidr
 */

DATA(insert OID = 0 (  403 652 1203 1 btreesel btreenpage ));
DATA(insert OID = 0 (  403 652 1204 2 btreesel btreenpage ));
DATA(insert OID = 0 (  403 652 1201 3 btreesel btreenpage ));
DATA(insert OID = 0 (  403 652 1206 4 btreesel btreenpage ));
DATA(insert OID = 0 (  403 652 1205 5 btreesel btreenpage ));

/*
 *	hash table _ops
 */

/* int2_ops */
DATA(insert OID = 0 (  405	421   94 1 hashsel hashnpage ));
/* float8_ops */
DATA(insert OID = 0 (  405	423  670 1 hashsel hashnpage ));
/* int4_ops */
DATA(insert OID = 0 (  405	426   96 1 hashsel hashnpage ));
/* oid_ops */
DATA(insert OID = 0 (  405	427  607 1 hashsel hashnpage ));
/* oid8_ops */
DATA(insert OID = 0 (  405	435  679 1 hashsel hashnpage ));
/* float4_ops */
DATA(insert OID = 0 (  405	428  620 1 hashsel hashnpage ));
/* char_ops */
DATA(insert OID = 0 (  405	429   92 1 hashsel hashnpage ));
/* name_ops */
DATA(insert OID = 0 (  405 1181   93 1 hashsel hashnpage ));
/* text_ops */
DATA(insert OID = 0 (  405	431   98 1 hashsel hashnpage ));
/* bpchar_ops */
DATA(insert OID = 0 (  405 1076 1054 1 hashsel hashnpage ));
/* varchar_ops */
DATA(insert OID = 0 (  405 1077 1062 1 hashsel hashnpage ));
/* date_ops */
DATA(insert OID = 0 (  405 1114 1093 1 hashsel hashnpage ));
/* time_ops */
DATA(insert OID = 0 (  405 1115 1108 1 hashsel hashnpage ));
/* datetime_ops */
DATA(insert OID = 0 (  405 1312 1320 1 hashsel hashnpage ));
/* timespan_ops */
DATA(insert OID = 0 (  405 1313 1330 1 hashsel hashnpage ));
/* macaddr_ops */
DATA(insert OID = 0 (  405 810 1220 1 hashsel hashnpage ));
/* inet_ops */
DATA(insert OID = 0 (  405 935 1201 1 hashsel hashnpage ));
/* cidr_ops */
DATA(insert OID = 0 (  405 652 820 1 hashsel hashnpage ));

#endif	 /* PG_AMOP_H */
