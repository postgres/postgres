/*-------------------------------------------------------------------------
 *
 * pg_operator.h
 *	  definition of the system "operator" relation (pg_operator)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_operator.h,v 1.121 2003/08/17 19:58:06 tgl Exp $
 *
 * NOTES
 *	  the genbki.sh script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *	  XXX do NOT break up DATA() statements into multiple lines!
 *		  the scripts are not as smart as you might think...
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_OPERATOR_H
#define PG_OPERATOR_H

#include "nodes/pg_list.h"

/* ----------------
 *		postgres.h contains the system type definitions and the
 *		CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------
 *		pg_operator definition.  cpp turns this into
 *		typedef struct FormData_pg_operator
 * ----------------
 */
CATALOG(pg_operator)
{
	NameData	oprname;		/* name of operator */
	Oid			oprnamespace;	/* OID of namespace containing this oper */
	int4		oprowner;		/* oper owner */
	char		oprkind;		/* 'l', 'r', or 'b' */
	bool		oprcanhash;		/* can be used in hash join? */
	Oid			oprleft;		/* left arg type, or 0 if 'l' oprkind */
	Oid			oprright;		/* right arg type, or 0 if 'r' oprkind */
	Oid			oprresult;		/* result datatype */
	Oid			oprcom;			/* OID of commutator oper, or 0 if none */
	Oid			oprnegate;		/* OID of negator oper, or 0 if none */
	Oid			oprlsortop;		/* OID of left sortop, if mergejoinable */
	Oid			oprrsortop;		/* OID of right sortop, if mergejoinable */
	Oid			oprltcmpop;		/* OID of "l<r" oper, if mergejoinable */
	Oid			oprgtcmpop;		/* OID of "l>r" oper, if mergejoinable */
	regproc		oprcode;		/* OID of underlying function */
	regproc		oprrest;		/* OID of restriction estimator, or 0 */
	regproc		oprjoin;		/* OID of join estimator, or 0 */
} FormData_pg_operator;

/* ----------------
 *		Form_pg_operator corresponds to a pointer to a tuple with
 *		the format of pg_operator relation.
 * ----------------
 */
typedef FormData_pg_operator *Form_pg_operator;

/* ----------------
 *		compiler constants for pg_operator
 * ----------------
 */

#define Natts_pg_operator				17
#define Anum_pg_operator_oprname		1
#define Anum_pg_operator_oprnamespace	2
#define Anum_pg_operator_oprowner		3
#define Anum_pg_operator_oprkind		4
#define Anum_pg_operator_oprcanhash		5
#define Anum_pg_operator_oprleft		6
#define Anum_pg_operator_oprright		7
#define Anum_pg_operator_oprresult		8
#define Anum_pg_operator_oprcom			9
#define Anum_pg_operator_oprnegate		10
#define Anum_pg_operator_oprlsortop		11
#define Anum_pg_operator_oprrsortop		12
#define Anum_pg_operator_oprltcmpop		13
#define Anum_pg_operator_oprgtcmpop		14
#define Anum_pg_operator_oprcode		15
#define Anum_pg_operator_oprrest		16
#define Anum_pg_operator_oprjoin		17

/* ----------------
 *		initial contents of pg_operator
 * ----------------
 */

DATA(insert OID =  15 ( "="		   PGNSP PGUID b f	23	20	16 416	36	97 412	37	76 int48eq eqsel eqjoinsel ));
DATA(insert OID =  36 ( "<>"	   PGNSP PGUID b f	23	20	16 417	15	 0	 0	 0	 0 int48ne neqsel neqjoinsel ));
DATA(insert OID =  37 ( "<"		   PGNSP PGUID b f	23	20	16 419	82	 0	 0	 0	 0 int48lt scalarltsel scalarltjoinsel ));
DATA(insert OID =  76 ( ">"		   PGNSP PGUID b f	23	20	16 418	80	 0	 0	 0	 0 int48gt scalargtsel scalargtjoinsel ));
DATA(insert OID =  80 ( "<="	   PGNSP PGUID b f	23	20	16 430	76	 0	 0	 0	 0 int48le scalarltsel scalarltjoinsel ));
DATA(insert OID =  82 ( ">="	   PGNSP PGUID b f	23	20	16 420	37	 0	 0	 0	 0 int48ge scalargtsel scalargtjoinsel ));

DATA(insert OID =  58 ( "<"		   PGNSP PGUID b f	16	16	16	59	 1695	0	0	0	0 boollt scalarltsel scalarltjoinsel ));
DATA(insert OID =  59 ( ">"		   PGNSP PGUID b f	16	16	16	58	 1694	0	0	0	0 boolgt scalargtsel scalargtjoinsel ));
DATA(insert OID =  85 ( "<>"	   PGNSP PGUID b f	16	16	16	85	91	 0	 0	 0	 0 boolne neqsel neqjoinsel ));
DATA(insert OID =  91 ( "="		   PGNSP PGUID b t	16	16	16	91	85	58	58	58	59 booleq eqsel eqjoinsel ));
#define BooleanEqualOperator   91
DATA(insert OID = 1694 (  "<="	   PGNSP PGUID b f	16	16	16 1695 59	0  0   0   0 boolle scalarltsel scalarltjoinsel ));
DATA(insert OID = 1695 (  ">="	   PGNSP PGUID b f	16	16	16 1694 58	0  0   0   0 boolge scalargtsel scalargtjoinsel ));

DATA(insert OID =  92 ( "="		   PGNSP PGUID b t	18	18	16	92 630 631 631 631 633 chareq eqsel eqjoinsel ));
DATA(insert OID =  93 ( "="		   PGNSP PGUID b t	19	19	16	93 643 660 660 660 662 nameeq eqsel eqjoinsel ));
DATA(insert OID =  94 ( "="		   PGNSP PGUID b t	21	21	16	94 519	95	95	95 520 int2eq eqsel eqjoinsel ));
DATA(insert OID =  95 ( "<"		   PGNSP PGUID b f	21	21	16 520 524	 0	 0	 0	 0 int2lt scalarltsel scalarltjoinsel ));
DATA(insert OID =  96 ( "="		   PGNSP PGUID b t	23	23	16	96 518	97	97	97 521 int4eq eqsel eqjoinsel ));
DATA(insert OID =  97 ( "<"		   PGNSP PGUID b f	23	23	16 521 525	 0	 0	 0	 0 int4lt scalarltsel scalarltjoinsel ));
DATA(insert OID =  98 ( "="		   PGNSP PGUID b t	25	25	16	98 531 664 664 664 666 texteq eqsel eqjoinsel ));

DATA(insert OID = 349 (  "||"	   PGNSP PGUID b f 2277 2283 2277	0 0 0 0 0 0 array_append   -	   -	 ));
DATA(insert OID = 374 (  "||"	   PGNSP PGUID b f 2283 2277 2277	0 0 0 0 0 0 array_prepend  -	   -	 ));
DATA(insert OID = 375 (  "||"	   PGNSP PGUID b f 2277 2277 2277	0 0 0 0 0 0 array_cat	   -	   -	 ));

DATA(insert OID = 352 (  "="	   PGNSP PGUID b t	28	28	16 352	 0	 0	 0	 0	 0 xideq eqsel eqjoinsel ));
DATA(insert OID = 353 (  "="	   PGNSP PGUID b f	28	23	16	 0	 0	 0	 0	 0	 0 xideqint4 eqsel eqjoinsel ));
DATA(insert OID = 385 (  "="	   PGNSP PGUID b t	29	29	16 385	 0	 0	 0	 0	 0 cideq eqsel eqjoinsel ));
DATA(insert OID = 386 (  "="	   PGNSP PGUID b t	22	22	16 386	 0	 0	 0	 0	 0 int2vectoreq eqsel eqjoinsel ));
DATA(insert OID = 387 (  "="	   PGNSP PGUID b f	27	27	16 387	 0	 0	 0	 0	 0 tideq eqsel eqjoinsel ));
#define TIDEqualOperator   387
DATA(insert OID = 388 (  "!"	   PGNSP PGUID r f	20	 0	20	 0	 0	 0	 0	 0	 0 int8fac - - ));
DATA(insert OID = 389 (  "!!"	   PGNSP PGUID l f	 0	20	20	 0	 0	 0	 0	 0	 0 int8fac - - ));

DATA(insert OID = 410 ( "="		   PGNSP PGUID b t	20	20	16 410 411 412 412 412 413 int8eq eqsel eqjoinsel ));
DATA(insert OID = 411 ( "<>"	   PGNSP PGUID b f	20	20	16 411 410 0 0 0 0 int8ne neqsel neqjoinsel ));
DATA(insert OID = 412 ( "<"		   PGNSP PGUID b f	20	20	16 413 415 0 0 0 0 int8lt scalarltsel scalarltjoinsel ));
DATA(insert OID = 413 ( ">"		   PGNSP PGUID b f	20	20	16 412 414 0 0 0 0 int8gt scalargtsel scalargtjoinsel ));
DATA(insert OID = 414 ( "<="	   PGNSP PGUID b f	20	20	16 415 413 0 0 0 0 int8le scalarltsel scalarltjoinsel ));
DATA(insert OID = 415 ( ">="	   PGNSP PGUID b f	20	20	16 414 412 0 0 0 0 int8ge scalargtsel scalargtjoinsel ));

DATA(insert OID = 416 ( "="		   PGNSP PGUID b f	20	23	16	15 417 412 97 418 419 int84eq eqsel eqjoinsel ));
DATA(insert OID = 417 ( "<>"	   PGNSP PGUID b f	20	23	16	36 416 0 0 0 0 int84ne neqsel neqjoinsel ));
DATA(insert OID = 418 ( "<"		   PGNSP PGUID b f	20	23	16	76 430 0 0 0 0 int84lt scalarltsel scalarltjoinsel ));
DATA(insert OID = 419 ( ">"		   PGNSP PGUID b f	20	23	16	37 420 0 0 0 0 int84gt scalargtsel scalargtjoinsel ));
DATA(insert OID = 420 ( "<="	   PGNSP PGUID b f	20	23	16	82 419 0 0 0 0 int84le scalarltsel scalarltjoinsel ));
DATA(insert OID = 430 ( ">="	   PGNSP PGUID b f	20	23	16	80 418 0 0 0 0 int84ge scalargtsel scalargtjoinsel ));
DATA(insert OID = 439 (  "%"	   PGNSP PGUID b f	20	20	20	 0	 0 0 0 0 0 int8mod - - ));
DATA(insert OID = 473 (  "@"	   PGNSP PGUID l f	 0	20	20	 0	 0 0 0 0 0 int8abs - - ));

DATA(insert OID = 484 (  "-"	   PGNSP PGUID l f	 0	20	20	 0	 0	 0	 0	 0	 0 int8um - - ));
DATA(insert OID = 485 (  "<<"	   PGNSP PGUID b f 604 604	16	 0	 0	 0	 0	 0	 0 poly_left positionsel positionjoinsel ));
DATA(insert OID = 486 (  "&<"	   PGNSP PGUID b f 604 604	16	 0	 0	 0	 0	 0	 0 poly_overleft positionsel positionjoinsel ));
DATA(insert OID = 487 (  "&>"	   PGNSP PGUID b f 604 604	16	 0	 0	 0	 0	 0	 0 poly_overright positionsel positionjoinsel ));
DATA(insert OID = 488 (  ">>"	   PGNSP PGUID b f 604 604	16	 0	 0	 0	 0	 0	 0 poly_right positionsel positionjoinsel ));
DATA(insert OID = 489 (  "@"	   PGNSP PGUID b f 604 604	16 490	 0	 0	 0	 0	 0 poly_contained contsel contjoinsel ));
DATA(insert OID = 490 (  "~"	   PGNSP PGUID b f 604 604	16 489	 0	 0	 0	 0	 0 poly_contain contsel contjoinsel ));
DATA(insert OID = 491 (  "~="	   PGNSP PGUID b f 604 604	16 491	 0	 0	 0	 0	 0 poly_same eqsel eqjoinsel ));
DATA(insert OID = 492 (  "&&"	   PGNSP PGUID b f 604 604	16 492	 0	 0	 0	 0	 0 poly_overlap areasel areajoinsel ));
DATA(insert OID = 493 (  "<<"	   PGNSP PGUID b f 603 603	16	 0	 0	 0	 0	 0	 0 box_left positionsel positionjoinsel ));
DATA(insert OID = 494 (  "&<"	   PGNSP PGUID b f 603 603	16	 0	 0	 0	 0	 0	 0 box_overleft positionsel positionjoinsel ));
DATA(insert OID = 495 (  "&>"	   PGNSP PGUID b f 603 603	16	 0	 0	 0	 0	 0	 0 box_overright positionsel positionjoinsel ));
DATA(insert OID = 496 (  ">>"	   PGNSP PGUID b f 603 603	16	 0	 0	 0	 0	 0	 0 box_right positionsel positionjoinsel ));
DATA(insert OID = 497 (  "@"	   PGNSP PGUID b f 603 603	16 498	 0	 0	 0	 0	 0 box_contained contsel contjoinsel ));
DATA(insert OID = 498 (  "~"	   PGNSP PGUID b f 603 603	16 497	 0	 0	 0	 0	 0 box_contain contsel contjoinsel ));
DATA(insert OID = 499 (  "~="	   PGNSP PGUID b f 603 603	16 499	 0	 0	 0	 0	 0 box_same eqsel eqjoinsel ));
DATA(insert OID = 500 (  "&&"	   PGNSP PGUID b f 603 603	16 500	 0	 0	 0	 0	 0 box_overlap areasel areajoinsel ));
DATA(insert OID = 501 (  ">="	   PGNSP PGUID b f 603 603	16 505 504	 0	 0	 0	 0 box_ge areasel areajoinsel ));
DATA(insert OID = 502 (  ">"	   PGNSP PGUID b f 603 603	16 504 505	 0	 0	 0	 0 box_gt areasel areajoinsel ));
DATA(insert OID = 503 (  "="	   PGNSP PGUID b f 603 603	16 503	 0 504 504 504 502 box_eq eqsel eqjoinsel ));
DATA(insert OID = 504 (  "<"	   PGNSP PGUID b f 603 603	16 502 501	 0	 0	 0	 0 box_lt areasel areajoinsel ));
DATA(insert OID = 505 (  "<="	   PGNSP PGUID b f 603 603	16 501 502	 0	 0	 0	 0 box_le areasel areajoinsel ));
DATA(insert OID = 506 (  ">^"	   PGNSP PGUID b f 600 600	16	 0	 0	 0	 0	 0	 0 point_above positionsel positionjoinsel ));
DATA(insert OID = 507 (  "<<"	   PGNSP PGUID b f 600 600	16	 0	 0	 0	 0	 0	 0 point_left positionsel positionjoinsel ));
DATA(insert OID = 508 (  ">>"	   PGNSP PGUID b f 600 600	16	 0	 0	 0	 0	 0	 0 point_right positionsel positionjoinsel ));
DATA(insert OID = 509 (  "<^"	   PGNSP PGUID b f 600 600	16	 0	 0	 0	 0	 0	 0 point_below positionsel positionjoinsel ));
DATA(insert OID = 510 (  "~="	   PGNSP PGUID b f 600 600	16 510 713	 0	 0	 0	 0 point_eq eqsel eqjoinsel ));
DATA(insert OID = 511 (  "@"	   PGNSP PGUID b f 600 603	16	 0	 0	 0	 0	 0	 0 on_pb - - ));
DATA(insert OID = 512 (  "@"	   PGNSP PGUID b f 600 602	16 755	 0	 0	 0	 0	 0 on_ppath - - ));
DATA(insert OID = 513 (  "@@"	   PGNSP PGUID l f	 0 603 600	 0	 0	 0	 0	 0	 0 box_center - - ));
DATA(insert OID = 514 (  "*"	   PGNSP PGUID b f	23	23	23 514	 0	 0	 0	 0	 0 int4mul - - ));
DATA(insert OID = 515 (  "!"	   PGNSP PGUID r f	23	 0	23	 0	 0	 0	 0	 0	 0 int4fac - - ));
DATA(insert OID = 516 (  "!!"	   PGNSP PGUID l f	 0	23	23	 0	 0	 0	 0	 0	 0 int4fac - - ));
DATA(insert OID = 517 (  "<->"	   PGNSP PGUID b f 600 600 701 517	 0	 0	 0	 0	 0 point_distance - - ));
DATA(insert OID = 518 (  "<>"	   PGNSP PGUID b f	23	23	16 518	96	0  0   0   0 int4ne neqsel neqjoinsel ));
DATA(insert OID = 519 (  "<>"	   PGNSP PGUID b f	21	21	16 519	94	0  0   0   0 int2ne neqsel neqjoinsel ));
DATA(insert OID = 520 (  ">"	   PGNSP PGUID b f	21	21	16	95 522	0  0   0   0 int2gt scalargtsel scalargtjoinsel ));
DATA(insert OID = 521 (  ">"	   PGNSP PGUID b f	23	23	16	97 523	0  0   0   0 int4gt scalargtsel scalargtjoinsel ));
DATA(insert OID = 522 (  "<="	   PGNSP PGUID b f	21	21	16 524 520	0  0   0   0 int2le scalarltsel scalarltjoinsel ));
DATA(insert OID = 523 (  "<="	   PGNSP PGUID b f	23	23	16 525 521	0  0   0   0 int4le scalarltsel scalarltjoinsel ));
DATA(insert OID = 524 (  ">="	   PGNSP PGUID b f	21	21	16 522	95	0  0   0   0 int2ge scalargtsel scalargtjoinsel ));
DATA(insert OID = 525 (  ">="	   PGNSP PGUID b f	23	23	16 523	97	0  0   0   0 int4ge scalargtsel scalargtjoinsel ));
DATA(insert OID = 526 (  "*"	   PGNSP PGUID b f	21	21	21 526	 0	0  0   0   0 int2mul - - ));
DATA(insert OID = 527 (  "/"	   PGNSP PGUID b f	21	21	21	 0	 0	0  0   0   0 int2div - - ));
DATA(insert OID = 528 (  "/"	   PGNSP PGUID b f	23	23	23	 0	 0	0  0   0   0 int4div - - ));
DATA(insert OID = 529 (  "%"	   PGNSP PGUID b f	21	21	21	 0	 0	0  0   0   0 int2mod - - ));
DATA(insert OID = 530 (  "%"	   PGNSP PGUID b f	23	23	23	 0	 0	0  0   0   0 int4mod - - ));
DATA(insert OID = 531 (  "<>"	   PGNSP PGUID b f	25	25	16 531	98	0	0	0	0 textne neqsel neqjoinsel ));
DATA(insert OID = 532 (  "="	   PGNSP PGUID b f	21	23	16 533 538	 95  97 534 536 int24eq eqsel eqjoinsel ));
DATA(insert OID = 533 (  "="	   PGNSP PGUID b f	23	21	16 532 539	 97  95 535 537 int42eq eqsel eqjoinsel ));
DATA(insert OID = 534 (  "<"	   PGNSP PGUID b f	21	23	16 537 542	0  0   0   0 int24lt scalarltsel scalarltjoinsel ));
DATA(insert OID = 535 (  "<"	   PGNSP PGUID b f	23	21	16 536 543	0  0   0   0 int42lt scalarltsel scalarltjoinsel ));
DATA(insert OID = 536 (  ">"	   PGNSP PGUID b f	21	23	16 535 540	0  0   0   0 int24gt scalargtsel scalargtjoinsel ));
DATA(insert OID = 537 (  ">"	   PGNSP PGUID b f	23	21	16 534 541	0  0   0   0 int42gt scalargtsel scalargtjoinsel ));
DATA(insert OID = 538 (  "<>"	   PGNSP PGUID b f	21	23	16 539 532	0  0   0   0 int24ne neqsel neqjoinsel ));
DATA(insert OID = 539 (  "<>"	   PGNSP PGUID b f	23	21	16 538 533	0  0   0   0 int42ne neqsel neqjoinsel ));
DATA(insert OID = 540 (  "<="	   PGNSP PGUID b f	21	23	16 543 536	0  0   0   0 int24le scalarltsel scalarltjoinsel ));
DATA(insert OID = 541 (  "<="	   PGNSP PGUID b f	23	21	16 542 537	0  0   0   0 int42le scalarltsel scalarltjoinsel ));
DATA(insert OID = 542 (  ">="	   PGNSP PGUID b f	21	23	16 541 534	0  0   0   0 int24ge scalargtsel scalargtjoinsel ));
DATA(insert OID = 543 (  ">="	   PGNSP PGUID b f	23	21	16 540 535	0  0   0   0 int42ge scalargtsel scalargtjoinsel ));
DATA(insert OID = 544 (  "*"	   PGNSP PGUID b f	21	23	23 545	 0	 0	 0	 0	 0 int24mul - - ));
DATA(insert OID = 545 (  "*"	   PGNSP PGUID b f	23	21	23 544	 0	 0	 0	 0	 0 int42mul - - ));
DATA(insert OID = 546 (  "/"	   PGNSP PGUID b f	21	23	23	 0	 0	 0	 0	 0	 0 int24div - - ));
DATA(insert OID = 547 (  "/"	   PGNSP PGUID b f	23	21	23	 0	 0	 0	 0	 0	 0 int42div - - ));
DATA(insert OID = 548 (  "%"	   PGNSP PGUID b f	21	23	23	 0	 0	 0	 0	 0	 0 int24mod - - ));
DATA(insert OID = 549 (  "%"	   PGNSP PGUID b f	23	21	23	 0	 0	 0	 0	 0	 0 int42mod - - ));
DATA(insert OID = 550 (  "+"	   PGNSP PGUID b f	21	21	21 550	 0	 0	 0	 0	 0 int2pl - - ));
DATA(insert OID = 551 (  "+"	   PGNSP PGUID b f	23	23	23 551	 0	 0	 0	 0	 0 int4pl - - ));
DATA(insert OID = 552 (  "+"	   PGNSP PGUID b f	21	23	23 553	 0	 0	 0	 0	 0 int24pl - - ));
DATA(insert OID = 553 (  "+"	   PGNSP PGUID b f	23	21	23 552	 0	 0	 0	 0	 0 int42pl - - ));
DATA(insert OID = 554 (  "-"	   PGNSP PGUID b f	21	21	21	 0	 0	 0	 0	 0	 0 int2mi - - ));
DATA(insert OID = 555 (  "-"	   PGNSP PGUID b f	23	23	23	 0	 0	 0	 0	 0	 0 int4mi - - ));
DATA(insert OID = 556 (  "-"	   PGNSP PGUID b f	21	23	23	 0	 0	 0	 0	 0	 0 int24mi - - ));
DATA(insert OID = 557 (  "-"	   PGNSP PGUID b f	23	21	23	 0	 0	 0	 0	 0	 0 int42mi - - ));
DATA(insert OID = 558 (  "-"	   PGNSP PGUID l f	 0	23	23	 0	 0	 0	 0	 0	 0 int4um - - ));
DATA(insert OID = 559 (  "-"	   PGNSP PGUID l f	 0	21	21	 0	 0	 0	 0	 0	 0 int2um - - ));
DATA(insert OID = 560 (  "="	   PGNSP PGUID b t 702 702	16 560 561 562 562 562 563 abstimeeq eqsel eqjoinsel ));
DATA(insert OID = 561 (  "<>"	   PGNSP PGUID b f 702 702	16 561 560 0 0 0 0 abstimene neqsel neqjoinsel ));
DATA(insert OID = 562 (  "<"	   PGNSP PGUID b f 702 702	16 563 565 0 0 0 0 abstimelt scalarltsel scalarltjoinsel ));
DATA(insert OID = 563 (  ">"	   PGNSP PGUID b f 702 702	16 562 564 0 0 0 0 abstimegt scalargtsel scalargtjoinsel ));
DATA(insert OID = 564 (  "<="	   PGNSP PGUID b f 702 702	16 565 563 0 0 0 0 abstimele scalarltsel scalarltjoinsel ));
DATA(insert OID = 565 (  ">="	   PGNSP PGUID b f 702 702	16 564 562 0 0 0 0 abstimege scalargtsel scalargtjoinsel ));
DATA(insert OID = 566 (  "="	   PGNSP PGUID b t 703 703	16 566 567 568 568 568 569 reltimeeq eqsel eqjoinsel ));
DATA(insert OID = 567 (  "<>"	   PGNSP PGUID b f 703 703	16 567 566 0 0 0 0 reltimene neqsel neqjoinsel ));
DATA(insert OID = 568 (  "<"	   PGNSP PGUID b f 703 703	16 569 571 0 0 0 0 reltimelt scalarltsel scalarltjoinsel ));
DATA(insert OID = 569 (  ">"	   PGNSP PGUID b f 703 703	16 568 570 0 0 0 0 reltimegt scalargtsel scalargtjoinsel ));
DATA(insert OID = 570 (  "<="	   PGNSP PGUID b f 703 703	16 571 569 0 0 0 0 reltimele scalarltsel scalarltjoinsel ));
DATA(insert OID = 571 (  ">="	   PGNSP PGUID b f 703 703	16 570 568 0 0 0 0 reltimege scalargtsel scalargtjoinsel ));
DATA(insert OID = 572 (  "~="	   PGNSP PGUID b f 704 704	16 572	 0	 0	 0	 0	 0 tintervalsame eqsel eqjoinsel ));
DATA(insert OID = 573 (  "<<"	   PGNSP PGUID b f 704 704	16	 0	 0	 0	 0	 0	 0 tintervalct - - ));
DATA(insert OID = 574 (  "&&"	   PGNSP PGUID b f 704 704	16 574	 0	 0	 0	 0	 0 tintervalov - - ));
DATA(insert OID = 575 (  "#="	   PGNSP PGUID b f 704 703	16	 0 576	 0	 0	 0	 0 tintervalleneq - - ));
DATA(insert OID = 576 (  "#<>"	   PGNSP PGUID b f 704 703	16	 0 575	 0	 0	 0	 0 tintervallenne - - ));
DATA(insert OID = 577 (  "#<"	   PGNSP PGUID b f 704 703	16	 0 580	 0	 0	 0	 0 tintervallenlt - - ));
DATA(insert OID = 578 (  "#>"	   PGNSP PGUID b f 704 703	16	 0 579	 0	 0	 0	 0 tintervallengt - - ));
DATA(insert OID = 579 (  "#<="	   PGNSP PGUID b f 704 703	16	 0 578	 0	 0	 0	 0 tintervallenle - - ));
DATA(insert OID = 580 (  "#>="	   PGNSP PGUID b f 704 703	16	 0 577	 0	 0	 0	 0 tintervallenge - - ));
DATA(insert OID = 581 (  "+"	   PGNSP PGUID b f 702 703 702	 0	 0 0 0 0 0 timepl - - ));
DATA(insert OID = 582 (  "-"	   PGNSP PGUID b f 702 703 702	 0	 0 0 0 0 0 timemi - - ));
DATA(insert OID = 583 (  "<?>"	   PGNSP PGUID b f 702 704	16	 0	 0	 0	 0	 0	 0 intinterval - - ));
DATA(insert OID = 584 (  "-"	   PGNSP PGUID l f	 0 700 700	 0	 0	 0	 0	 0	 0 float4um - - ));
DATA(insert OID = 585 (  "-"	   PGNSP PGUID l f	 0 701 701	 0	 0	 0	 0	 0	 0 float8um - - ));
DATA(insert OID = 586 (  "+"	   PGNSP PGUID b f 700 700 700 586	 0	 0	 0	 0	 0 float4pl - - ));
DATA(insert OID = 587 (  "-"	   PGNSP PGUID b f 700 700 700	 0	 0	 0	 0	 0	 0 float4mi - - ));
DATA(insert OID = 588 (  "/"	   PGNSP PGUID b f 700 700 700	 0	 0	 0	 0	 0	 0 float4div - - ));
DATA(insert OID = 589 (  "*"	   PGNSP PGUID b f 700 700 700 589	 0	 0	 0	 0	 0 float4mul - - ));
DATA(insert OID = 590 (  "@"	   PGNSP PGUID l f	 0 700 700	 0	 0	 0	 0	 0	 0 float4abs - - ));
DATA(insert OID = 591 (  "+"	   PGNSP PGUID b f 701 701 701 591	 0	 0	 0	 0	 0 float8pl - - ));
DATA(insert OID = 592 (  "-"	   PGNSP PGUID b f 701 701 701	 0	 0	 0	 0	 0	 0 float8mi - - ));
DATA(insert OID = 593 (  "/"	   PGNSP PGUID b f 701 701 701	 0	 0	 0	 0	 0	 0 float8div - - ));
DATA(insert OID = 594 (  "*"	   PGNSP PGUID b f 701 701 701 594	 0	 0	 0	 0	 0 float8mul - - ));
DATA(insert OID = 595 (  "@"	   PGNSP PGUID l f	 0 701 701	 0	 0	 0	 0	 0	 0 float8abs - - ));
DATA(insert OID = 596 (  "|/"	   PGNSP PGUID l f	 0 701 701	 0	 0	 0	 0	 0	 0 dsqrt - - ));
DATA(insert OID = 597 (  "||/"	   PGNSP PGUID l f	 0 701 701	 0	 0	 0	 0	 0	 0 dcbrt - - ));
DATA(insert OID = 598 (  "%"	   PGNSP PGUID l f	 0 701 701	 0	 0	 0	 0	 0	 0 dtrunc - - ));
DATA(insert OID = 599 (  "%"	   PGNSP PGUID r f 701	 0 701	 0	 0	 0	 0	 0	 0 dround - - ));
DATA(insert OID = 1284 (  "|"	   PGNSP PGUID l f	 0 704 702	0  0   0   0   0   0 tintervalstart - - ));
DATA(insert OID = 606 (  "<#>"	   PGNSP PGUID b f 702 702 704	0  0   0   0   0   0 mktinterval - - ));
DATA(insert OID = 607 (  "="	   PGNSP PGUID b t	26	26	16 607 608 609 609 609 610 oideq eqsel eqjoinsel ));
#define MIN_OIDCMP 607			/* used by cache code */
DATA(insert OID = 608 (  "<>"	   PGNSP PGUID b f	26	26	16 608 607	0  0   0   0 oidne neqsel neqjoinsel ));
DATA(insert OID = 609 (  "<"	   PGNSP PGUID b f	26	26	16 610 612	0  0   0   0 oidlt scalarltsel scalarltjoinsel ));
DATA(insert OID = 610 (  ">"	   PGNSP PGUID b f	26	26	16 609 611	0  0   0   0 oidgt scalargtsel scalargtjoinsel ));
DATA(insert OID = 611 (  "<="	   PGNSP PGUID b f	26	26	16 612 610	0  0   0   0 oidle scalarltsel scalarltjoinsel ));
DATA(insert OID = 612 (  ">="	   PGNSP PGUID b f	26	26	16 611 609	0  0   0   0 oidge scalargtsel scalargtjoinsel ));
#define MAX_OIDCMP 612			/* used by cache code */

DATA(insert OID = 644 (  "<>"	   PGNSP PGUID b f	30	30	16 644 649	 0	 0	 0	 0 oidvectorne neqsel neqjoinsel ));
DATA(insert OID = 645 (  "<"	   PGNSP PGUID b f	30	30	16 646 648	 0	 0	 0	 0 oidvectorlt scalarltsel scalarltjoinsel ));
DATA(insert OID = 646 (  ">"	   PGNSP PGUID b f	30	30	16 645 647	 0	 0	 0	 0 oidvectorgt scalargtsel scalargtjoinsel ));
DATA(insert OID = 647 (  "<="	   PGNSP PGUID b f	30	30	16 648 646	 0	 0	 0	 0 oidvectorle scalarltsel scalarltjoinsel ));
DATA(insert OID = 648 (  ">="	   PGNSP PGUID b f	30	30	16 647 645	 0	 0	 0	 0 oidvectorge scalargtsel scalargtjoinsel ));
DATA(insert OID = 649 (  "="	   PGNSP PGUID b t	30	30	16 649 644 645 645 645 646 oidvectoreq eqsel eqjoinsel ));

DATA(insert OID = 613 (  "<->"	   PGNSP PGUID b f 600 628 701	 0	 0	0  0   0   0 dist_pl - - ));
DATA(insert OID = 614 (  "<->"	   PGNSP PGUID b f 600 601 701	 0	 0	0  0   0   0 dist_ps - - ));
DATA(insert OID = 615 (  "<->"	   PGNSP PGUID b f 600 603 701	 0	 0	0  0   0   0 dist_pb - - ));
DATA(insert OID = 616 (  "<->"	   PGNSP PGUID b f 601 628 701	 0	 0	0  0   0   0 dist_sl - - ));
DATA(insert OID = 617 (  "<->"	   PGNSP PGUID b f 601 603 701	 0	 0	0  0   0   0 dist_sb - - ));
DATA(insert OID = 618 (  "<->"	   PGNSP PGUID b f 600 602 701	 0	 0	0  0   0   0 dist_ppath - - ));

DATA(insert OID = 620 (  "="	   PGNSP PGUID b t	700  700	16 620 621	622 622 622 623 float4eq eqsel eqjoinsel ));
DATA(insert OID = 621 (  "<>"	   PGNSP PGUID b f	700  700	16 621 620	0 0   0   0 float4ne neqsel neqjoinsel ));
DATA(insert OID = 622 (  "<"	   PGNSP PGUID b f	700  700	16 623 625	0 0   0   0 float4lt scalarltsel scalarltjoinsel ));
DATA(insert OID = 623 (  ">"	   PGNSP PGUID b f	700  700	16 622 624	0 0   0   0 float4gt scalargtsel scalargtjoinsel ));
DATA(insert OID = 624 (  "<="	   PGNSP PGUID b f	700  700	16 625 623	0 0   0   0 float4le scalarltsel scalarltjoinsel ));
DATA(insert OID = 625 (  ">="	   PGNSP PGUID b f	700  700	16 624 622	0 0   0   0 float4ge scalargtsel scalargtjoinsel ));
DATA(insert OID = 626 (  "!!="	   PGNSP PGUID b f	23	 25 16 0   0	0	0	0	0	int4notin - - ));
DATA(insert OID = 627 (  "!!="	   PGNSP PGUID b f	26	 25 16 0   0	0	0	0	0	oidnotin - - ));
DATA(insert OID = 630 (  "<>"	   PGNSP PGUID b f	18	18	16 630	92	0 0 0 0 charne neqsel neqjoinsel ));

DATA(insert OID = 631 (  "<"	   PGNSP PGUID b f	18	18	16 633 634	0 0 0 0 charlt scalarltsel scalarltjoinsel ));
DATA(insert OID = 632 (  "<="	   PGNSP PGUID b f	18	18	16 634 633	0 0 0 0 charle scalarltsel scalarltjoinsel ));
DATA(insert OID = 633 (  ">"	   PGNSP PGUID b f	18	18	16 631 632	0 0 0 0 chargt scalargtsel scalargtjoinsel ));
DATA(insert OID = 634 (  ">="	   PGNSP PGUID b f	18	18	16 632 631	0 0 0 0 charge scalargtsel scalargtjoinsel ));

DATA(insert OID = 635 (  "+"	   PGNSP PGUID b f	18	18	18 0 0	0 0 0 0 charpl - - ));
DATA(insert OID = 636 (  "-"	   PGNSP PGUID b f	18	18	18 0 0	0 0 0 0 charmi - - ));
DATA(insert OID = 637 (  "*"	   PGNSP PGUID b f	18	18	18 0 0	0 0 0 0 charmul - - ));
DATA(insert OID = 638 (  "/"	   PGNSP PGUID b f	18	18	18 0 0	0 0 0 0 chardiv - - ));

DATA(insert OID = 639 (  "~"	   PGNSP PGUID b f	19	25	16 0 640	0 0   0   0 nameregexeq regexeqsel regexeqjoinsel ));
#define OID_NAME_REGEXEQ_OP		639
DATA(insert OID = 640 (  "!~"	   PGNSP PGUID b f	19	25	16 0 639	0 0   0   0 nameregexne regexnesel regexnejoinsel ));
DATA(insert OID = 641 (  "~"	   PGNSP PGUID b f	25	25	16 0 642	0 0   0   0 textregexeq regexeqsel regexeqjoinsel ));
#define OID_TEXT_REGEXEQ_OP		641
DATA(insert OID = 642 (  "!~"	   PGNSP PGUID b f	25	25	16 0 641	0 0   0   0 textregexne regexnesel regexnejoinsel ));
DATA(insert OID = 643 (  "<>"	   PGNSP PGUID b f	19	19	16 643 93 0 0 0 0 namene neqsel neqjoinsel ));
DATA(insert OID = 654 (  "||"	   PGNSP PGUID b f	25	25	25	 0 0	0 0   0   0 textcat - - ));

DATA(insert OID = 660 (  "<"	   PGNSP PGUID b f	19	19	16 662 663	0 0 0 0 namelt scalarltsel scalarltjoinsel ));
DATA(insert OID = 661 (  "<="	   PGNSP PGUID b f	19	19	16 663 662	0 0 0 0 namele scalarltsel scalarltjoinsel ));
DATA(insert OID = 662 (  ">"	   PGNSP PGUID b f	19	19	16 660 661	0 0 0 0 namegt scalargtsel scalargtjoinsel ));
DATA(insert OID = 663 (  ">="	   PGNSP PGUID b f	19	19	16 661 660	0 0 0 0 namege scalargtsel scalargtjoinsel ));
DATA(insert OID = 664 (  "<"	   PGNSP PGUID b f	25	25	16 666 667	0 0 0 0 text_lt scalarltsel scalarltjoinsel ));
DATA(insert OID = 665 (  "<="	   PGNSP PGUID b f	25	25	16 667 666	0 0 0 0 text_le scalarltsel scalarltjoinsel ));
DATA(insert OID = 666 (  ">"	   PGNSP PGUID b f	25	25	16 664 665	0 0 0 0 text_gt scalargtsel scalargtjoinsel ));
DATA(insert OID = 667 (  ">="	   PGNSP PGUID b f	25	25	16 665 664	0 0 0 0 text_ge scalargtsel scalargtjoinsel ));

DATA(insert OID = 670 (  "="	   PGNSP PGUID b t	701  701	16 670 671 672 672 672 674 float8eq eqsel eqjoinsel ));
DATA(insert OID = 671 (  "<>"	   PGNSP PGUID b f	701  701	16 671 670	0 0   0   0 float8ne neqsel neqjoinsel ));
DATA(insert OID = 672 (  "<"	   PGNSP PGUID b f	701  701	16 674 675	0 0   0   0 float8lt scalarltsel scalarltjoinsel ));
DATA(insert OID = 673 (  "<="	   PGNSP PGUID b f	701  701	16 675 674	0 0   0   0 float8le scalarltsel scalarltjoinsel ));
DATA(insert OID = 674 (  ">"	   PGNSP PGUID b f	701  701	16 672 673	0 0   0   0 float8gt scalargtsel scalargtjoinsel ));
DATA(insert OID = 675 (  ">="	   PGNSP PGUID b f	701  701	16 673 672	0 0   0   0 float8ge scalargtsel scalargtjoinsel ));

DATA(insert OID = 682 (  "@"	   PGNSP PGUID l f	 0	21	21	 0	 0	 0	 0	 0	 0 int2abs - - ));
DATA(insert OID = 684 (  "+"	   PGNSP PGUID b f	20	20	20 684	 0	 0	 0	 0	 0 int8pl - - ));
DATA(insert OID = 685 (  "-"	   PGNSP PGUID b f	20	20	20	 0	 0	 0	 0	 0	 0 int8mi - - ));
DATA(insert OID = 686 (  "*"	   PGNSP PGUID b f	20	20	20 686	 0	 0	 0	 0	 0 int8mul - - ));
DATA(insert OID = 687 (  "/"	   PGNSP PGUID b f	20	20	20	 0	 0	 0	 0	 0	 0 int8div - - ));
DATA(insert OID = 688 (  "+"	   PGNSP PGUID b f	20	23	20 692	 0	 0	 0	 0	 0 int84pl - - ));
DATA(insert OID = 689 (  "-"	   PGNSP PGUID b f	20	23	20	 0	 0	 0	 0	 0	 0 int84mi - - ));
DATA(insert OID = 690 (  "*"	   PGNSP PGUID b f	20	23	20 694	 0	 0	 0	 0	 0 int84mul - - ));
DATA(insert OID = 691 (  "/"	   PGNSP PGUID b f	20	23	20	 0	 0	 0	 0	 0	 0 int84div - - ));
DATA(insert OID = 692 (  "+"	   PGNSP PGUID b f	23	20	20 688	 0	 0	 0	 0	 0 int48pl - - ));
DATA(insert OID = 693 (  "-"	   PGNSP PGUID b f	23	20	20	 0	 0	 0	 0	 0	 0 int48mi - - ));
DATA(insert OID = 694 (  "*"	   PGNSP PGUID b f	23	20	20 690	 0	 0	 0	 0	 0 int48mul - - ));
DATA(insert OID = 695 (  "/"	   PGNSP PGUID b f	23	20	20	 0	 0	 0	 0	 0	 0 int48div - - ));

DATA(insert OID = 706 (  "<->"	   PGNSP PGUID b f 603 603 701 706	 0	0  0   0   0 box_distance - - ));
DATA(insert OID = 707 (  "<->"	   PGNSP PGUID b f 602 602 701 707	 0	0  0   0   0 path_distance - - ));
DATA(insert OID = 708 (  "<->"	   PGNSP PGUID b f 628 628 701 708	 0	0  0   0   0 line_distance - - ));
DATA(insert OID = 709 (  "<->"	   PGNSP PGUID b f 601 601 701 709	 0	0  0   0   0 lseg_distance - - ));
DATA(insert OID = 712 (  "<->"	   PGNSP PGUID b f 604 604 701 712	 0	0  0   0   0 poly_distance - - ));

DATA(insert OID = 713 (  "<>"	   PGNSP PGUID b f 600 600	16 713 510	0  0   0   0 point_ne neqsel neqjoinsel ));

/* add translation/rotation/scaling operators for geometric types. - thomas 97/05/10 */
DATA(insert OID = 731 (  "+"	   PGNSP PGUID b f	600  600	600  731  0 0 0 0 0 point_add - - ));
DATA(insert OID = 732 (  "-"	   PGNSP PGUID b f	600  600	600    0  0 0 0 0 0 point_sub - - ));
DATA(insert OID = 733 (  "*"	   PGNSP PGUID b f	600  600	600  733  0 0 0 0 0 point_mul - - ));
DATA(insert OID = 734 (  "/"	   PGNSP PGUID b f	600  600	600    0  0 0 0 0 0 point_div - - ));
DATA(insert OID = 735 (  "+"	   PGNSP PGUID b f	602  602	602  735  0 0 0 0 0 path_add - - ));
DATA(insert OID = 736 (  "+"	   PGNSP PGUID b f	602  600	602    0  0 0 0 0 0 path_add_pt - - ));
DATA(insert OID = 737 (  "-"	   PGNSP PGUID b f	602  600	602    0  0 0 0 0 0 path_sub_pt - - ));
DATA(insert OID = 738 (  "*"	   PGNSP PGUID b f	602  600	602    0  0 0 0 0 0 path_mul_pt - - ));
DATA(insert OID = 739 (  "/"	   PGNSP PGUID b f	602  600	602    0  0 0 0 0 0 path_div_pt - - ));
DATA(insert OID = 755 (  "~"	   PGNSP PGUID b f	602  600	 16  512  0 0 0 0 0 path_contain_pt - - ));
DATA(insert OID = 756 (  "@"	   PGNSP PGUID b f	600  604	 16  757  0 0 0 0 0 pt_contained_poly - - ));
DATA(insert OID = 757 (  "~"	   PGNSP PGUID b f	604  600	 16  756  0 0 0 0 0 poly_contain_pt - - ));
DATA(insert OID = 758 (  "@"	   PGNSP PGUID b f	600  718	 16  759  0 0 0 0 0 pt_contained_circle - - ));
DATA(insert OID = 759 (  "~"	   PGNSP PGUID b f	718  600	 16  758  0 0 0 0 0 circle_contain_pt - - ));

DATA(insert OID = 773 (  "@"	   PGNSP PGUID l f	 0	23	23	 0	 0	 0	 0	 0	 0 int4abs - - ));

/* additional operators for geometric types - thomas 1997-07-09 */
DATA(insert OID =  792 (  "="	   PGNSP PGUID b f	602  602	 16  792  0 0 0 0 0 path_n_eq eqsel eqjoinsel ));
DATA(insert OID =  793 (  "<"	   PGNSP PGUID b f	602  602	 16  794  0 0 0 0 0 path_n_lt - - ));
DATA(insert OID =  794 (  ">"	   PGNSP PGUID b f	602  602	 16  793  0 0 0 0 0 path_n_gt - - ));
DATA(insert OID =  795 (  "<="	   PGNSP PGUID b f	602  602	 16  796  0 0 0 0 0 path_n_le - - ));
DATA(insert OID =  796 (  ">="	   PGNSP PGUID b f	602  602	 16  795  0 0 0 0 0 path_n_ge - - ));
DATA(insert OID =  797 (  "#"	   PGNSP PGUID l f	0  602	 23    0  0 0 0 0 0 path_npoints - - ));
DATA(insert OID =  798 (  "?#"	   PGNSP PGUID b f	602  602	 16    0  0 0 0 0 0 path_inter - - ));
DATA(insert OID =  799 (  "@-@"    PGNSP PGUID l f	0  602	701    0  0 0 0 0 0 path_length - - ));
DATA(insert OID =  800 (  ">^"	   PGNSP PGUID b f	603  603	 16    0  0 0 0 0 0 box_above positionsel positionjoinsel ));
DATA(insert OID =  801 (  "<^"	   PGNSP PGUID b f	603  603	 16    0  0 0 0 0 0 box_below positionsel positionjoinsel ));
DATA(insert OID =  802 (  "?#"	   PGNSP PGUID b f	603  603	 16    0  0 0 0 0 0 box_overlap areasel areajoinsel ));
DATA(insert OID =  803 (  "#"	   PGNSP PGUID b f	603  603	603    0  0 0 0 0 0 box_intersect - - ));
DATA(insert OID =  804 (  "+"	   PGNSP PGUID b f	603  600	603    0  0 0 0 0 0 box_add - - ));
DATA(insert OID =  805 (  "-"	   PGNSP PGUID b f	603  600	603    0  0 0 0 0 0 box_sub - - ));
DATA(insert OID =  806 (  "*"	   PGNSP PGUID b f	603  600	603    0  0 0 0 0 0 box_mul - - ));
DATA(insert OID =  807 (  "/"	   PGNSP PGUID b f	603  600	603    0  0 0 0 0 0 box_div - - ));
DATA(insert OID =  808 (  "?-"	   PGNSP PGUID b f	600  600	 16  808  0 0 0 0 0 point_horiz - - ));
DATA(insert OID =  809 (  "?|"	   PGNSP PGUID b f	600  600	 16  809  0 0 0 0 0 point_vert - - ));

DATA(insert OID = 811 (  "="	   PGNSP PGUID b f 704 704	16 811 812	 0	 0	 0	 0 tintervaleq eqsel eqjoinsel ));
DATA(insert OID = 812 (  "<>"	   PGNSP PGUID b f 704 704	16 812 811	 0	 0	 0	 0 tintervalne neqsel neqjoinsel ));
DATA(insert OID = 813 (  "<"	   PGNSP PGUID b f 704 704	16 814 816	 0	 0	 0	 0 tintervallt scalarltsel scalarltjoinsel ));
DATA(insert OID = 814 (  ">"	   PGNSP PGUID b f 704 704	16 813 815	 0	 0	 0	 0 tintervalgt scalargtsel scalargtjoinsel ));
DATA(insert OID = 815 (  "<="	   PGNSP PGUID b f 704 704	16 816 814	 0	 0	 0	 0 tintervalle scalarltsel scalarltjoinsel ));
DATA(insert OID = 816 (  ">="	   PGNSP PGUID b f 704 704	16 815 813	 0	 0	 0	 0 tintervalge scalargtsel scalargtjoinsel ));

DATA(insert OID = 843 (  "*"	   PGNSP PGUID b f	790  700	790 845   0   0   0   0   0 cash_mul_flt4 - - ));
DATA(insert OID = 844 (  "/"	   PGNSP PGUID b f	790  700	790   0   0   0   0   0   0 cash_div_flt4 - - ));
DATA(insert OID = 845 (  "*"	   PGNSP PGUID b f	700  790	790 843   0   0   0   0   0 flt4_mul_cash - - ));

DATA(insert OID = 900 (  "="	   PGNSP PGUID b f	790  790	16 900 901	902 902 902 903 cash_eq eqsel eqjoinsel ));
DATA(insert OID = 901 (  "<>"	   PGNSP PGUID b f	790  790	16 901 900	0 0   0   0 cash_ne neqsel neqjoinsel ));
DATA(insert OID = 902 (  "<"	   PGNSP PGUID b f	790  790	16 903 905	0 0   0   0 cash_lt scalarltsel scalarltjoinsel ));
DATA(insert OID = 903 (  ">"	   PGNSP PGUID b f	790  790	16 902 904	0 0   0   0 cash_gt scalargtsel scalargtjoinsel ));
DATA(insert OID = 904 (  "<="	   PGNSP PGUID b f	790  790	16 905 903	0 0   0   0 cash_le scalarltsel scalarltjoinsel ));
DATA(insert OID = 905 (  ">="	   PGNSP PGUID b f	790  790	16 904 902	0 0   0   0 cash_ge scalargtsel scalargtjoinsel ));
DATA(insert OID = 906 (  "+"	   PGNSP PGUID b f	790  790	790 906   0   0   0   0   0 cash_pl - - ));
DATA(insert OID = 907 (  "-"	   PGNSP PGUID b f	790  790	790   0   0   0   0   0   0 cash_mi - - ));
DATA(insert OID = 908 (  "*"	   PGNSP PGUID b f	790  701	790 916   0   0   0   0   0 cash_mul_flt8 - - ));
DATA(insert OID = 909 (  "/"	   PGNSP PGUID b f	790  701	790   0   0   0   0   0   0 cash_div_flt8 - - ));
DATA(insert OID = 912 (  "*"	   PGNSP PGUID b f	790  23 790 917   0   0   0   0   0 cash_mul_int4 - - ));
DATA(insert OID = 913 (  "/"	   PGNSP PGUID b f	790  23 790   0   0   0   0   0   0 cash_div_int4 - - ));
DATA(insert OID = 914 (  "*"	   PGNSP PGUID b f	790  21 790 918   0   0   0   0   0 cash_mul_int2 - - ));
DATA(insert OID = 915 (  "/"	   PGNSP PGUID b f	790  21 790   0   0   0   0   0   0 cash_div_int2 - - ));
DATA(insert OID = 916 (  "*"	   PGNSP PGUID b f	701  790	790 908   0   0   0   0   0 flt8_mul_cash - - ));
DATA(insert OID = 917 (  "*"	   PGNSP PGUID b f	23	790 790 912   0   0   0   0   0 int4_mul_cash - - ));
DATA(insert OID = 918 (  "*"	   PGNSP PGUID b f	21	790 790 914   0   0   0   0   0 int2_mul_cash - - ));

DATA(insert OID = 965 (  "^"	   PGNSP PGUID b f	701  701	701 0 0 0 0 0 0 dpow - - ));
DATA(insert OID = 966 (  "+"	   PGNSP PGUID b f 1034 1033 1034 0 0 0 0 0 0 aclinsert - - ));
DATA(insert OID = 967 (  "-"	   PGNSP PGUID b f 1034 1033 1034 0 0 0 0 0 0 aclremove - - ));
DATA(insert OID = 968 (  "~"	   PGNSP PGUID b f 1034 1033	 16 0 0 0 0 0 0 aclcontains - - ));
DATA(insert OID = 974 (  "="	   PGNSP PGUID b t 1033 1033	 16 974 0 0 0 0 0 aclitemeq eqsel eqjoinsel ));

/* additional geometric operators - thomas 1997-07-09 */
DATA(insert OID =  969 (  "@@"	   PGNSP PGUID l f	0  601	600    0  0 0 0 0 0 lseg_center - - ));
DATA(insert OID =  970 (  "@@"	   PGNSP PGUID l f	0  602	600    0  0 0 0 0 0 path_center - - ));
DATA(insert OID =  971 (  "@@"	   PGNSP PGUID l f	0  604	600    0  0 0 0 0 0 poly_center - - ));

DATA(insert OID = 1054 ( "="	   PGNSP PGUID b t 1042 1042	 16 1054 1057 1058 1058 1058 1060 bpchareq eqsel eqjoinsel ));
DATA(insert OID = 1055 ( "~"	   PGNSP PGUID b f 1042 25	 16    0 1056  0 0 0 0 bpcharregexeq regexeqsel regexeqjoinsel ));
#define OID_BPCHAR_REGEXEQ_OP		1055
DATA(insert OID = 1056 ( "!~"	   PGNSP PGUID b f 1042 25	 16    0 1055  0 0 0 0 bpcharregexne regexnesel regexnejoinsel ));
DATA(insert OID = 1057 ( "<>"	   PGNSP PGUID b f 1042 1042	 16 1057 1054  0 0 0 0 bpcharne neqsel neqjoinsel ));
DATA(insert OID = 1058 ( "<"	   PGNSP PGUID b f 1042 1042	 16 1060 1061  0 0 0 0 bpcharlt scalarltsel scalarltjoinsel ));
DATA(insert OID = 1059 ( "<="	   PGNSP PGUID b f 1042 1042	 16 1061 1060  0 0 0 0 bpcharle scalarltsel scalarltjoinsel ));
DATA(insert OID = 1060 ( ">"	   PGNSP PGUID b f 1042 1042	 16 1058 1059  0 0 0 0 bpchargt scalargtsel scalargtjoinsel ));
DATA(insert OID = 1061 ( ">="	   PGNSP PGUID b f 1042 1042	 16 1059 1058  0 0 0 0 bpcharge scalargtsel scalargtjoinsel ));

/* generic array comparison operators */
DATA(insert OID = 1070 (  "="	   PGNSP PGUID b f 2277 2277 16 1070 1071  1072 1072 1072 1073 array_eq eqsel eqjoinsel ));
#define ARRAY_EQ_OP 1070
DATA(insert OID = 1071 (  "<>"	   PGNSP PGUID b f 2277 2277 16 1071 1070  0 0 0 0 array_ne neqsel neqjoinsel ));
DATA(insert OID = 1072 (  "<"	   PGNSP PGUID b f 2277 2277 16 1073 1075  0 0 0 0 array_lt scalarltsel scalarltjoinsel ));
#define ARRAY_LT_OP 1072
DATA(insert OID = 1073 (  ">"	   PGNSP PGUID b f 2277 2277 16 1072 1074  0 0 0 0 array_gt scalargtsel scalargtjoinsel ));
#define ARRAY_GT_OP 1073
DATA(insert OID = 1074 (  "<="	   PGNSP PGUID b f 2277 2277 16 1075 1073  0 0 0 0 array_le scalarltsel scalarltjoinsel ));
DATA(insert OID = 1075 (  ">="	   PGNSP PGUID b f 2277 2277 16 1074 1072  0 0 0 0 array_ge scalargtsel scalargtjoinsel ));

/* date operators */
DATA(insert OID = 1076 ( "+"	   PGNSP PGUID b f	1082	1186 1114 0 0 0 0 0 0 date_pl_interval - - ));
DATA(insert OID = 1077 ( "-"	   PGNSP PGUID b f	1082	1186 1114 0 0 0 0 0 0 date_mi_interval - - ));
DATA(insert OID = 1093 ( "="	   PGNSP PGUID b t	1082	1082   16 1093 1094 1095 1095 1095 1097 date_eq eqsel eqjoinsel ));
DATA(insert OID = 1094 ( "<>"	   PGNSP PGUID b f	1082	1082   16 1094 1093  0 0 0 0 date_ne neqsel neqjoinsel ));
DATA(insert OID = 1095 ( "<"	   PGNSP PGUID b f	1082	1082   16 1097 1098  0 0 0 0 date_lt scalarltsel scalarltjoinsel ));
DATA(insert OID = 1096 ( "<="	   PGNSP PGUID b f	1082	1082   16 1098 1097  0 0 0 0 date_le scalarltsel scalarltjoinsel ));
DATA(insert OID = 1097 ( ">"	   PGNSP PGUID b f	1082	1082   16 1095 1096  0 0 0 0 date_gt scalargtsel scalargtjoinsel ));
DATA(insert OID = 1098 ( ">="	   PGNSP PGUID b f	1082	1082   16 1096 1095  0 0 0 0 date_ge scalargtsel scalargtjoinsel ));
DATA(insert OID = 1099 ( "-"	   PGNSP PGUID b f	1082	1082   23 0 0 0 0 0 0 date_mi - - ));
DATA(insert OID = 1100 ( "+"	   PGNSP PGUID b f	1082	  23 1082 0 0 0 0 0 0 date_pli - - ));
DATA(insert OID = 1101 ( "-"	   PGNSP PGUID b f	1082	  23 1082 0 0 0 0 0 0 date_mii - - ));

/* time operators */
DATA(insert OID = 1108 ( "="	   PGNSP PGUID b t	1083	1083  16 1108 1109 1110 1110 1110 1112 time_eq eqsel eqjoinsel ));
DATA(insert OID = 1109 ( "<>"	   PGNSP PGUID b f	1083	1083  16 1109 1108	0 0   0   0 time_ne neqsel neqjoinsel ));
DATA(insert OID = 1110 ( "<"	   PGNSP PGUID b f	1083	1083  16 1112 1113	0 0   0   0 time_lt scalarltsel scalarltjoinsel ));
DATA(insert OID = 1111 ( "<="	   PGNSP PGUID b f	1083	1083  16 1113 1112	0 0   0   0 time_le scalarltsel scalarltjoinsel ));
DATA(insert OID = 1112 ( ">"	   PGNSP PGUID b f	1083	1083  16 1110 1111	0 0   0   0 time_gt scalargtsel scalargtjoinsel ));
DATA(insert OID = 1113 ( ">="	   PGNSP PGUID b f	1083	1083  16 1111 1110	0 0   0   0 time_ge scalargtsel scalargtjoinsel ));
DATA(insert OID = 1269 (  "-"	   PGNSP PGUID b f	1186 1083 1083 0 0 0 0 0 0 interval_mi_time - - ));

/* timetz operators */
DATA(insert OID = 1295 (  "-"	   PGNSP PGUID b f	1186 1266 1266 0 0 0 0 0 0 interval_mi_timetz - - ));
DATA(insert OID = 1550 ( "="	   PGNSP PGUID b t	1266 1266	16 1550 1551 1552 1552 1552 1554 timetz_eq eqsel eqjoinsel ));
DATA(insert OID = 1551 ( "<>"	   PGNSP PGUID b f	1266 1266	16 1551 1550	0 0   0   0 timetz_ne neqsel neqjoinsel ));
DATA(insert OID = 1552 ( "<"	   PGNSP PGUID b f	1266 1266	16 1554 1555	0 0   0   0 timetz_lt scalarltsel scalarltjoinsel ));
DATA(insert OID = 1553 ( "<="	   PGNSP PGUID b f	1266 1266	16 1555 1554	0 0   0   0 timetz_le scalarltsel scalarltjoinsel ));
DATA(insert OID = 1554 ( ">"	   PGNSP PGUID b f	1266 1266	16 1552 1553	0 0   0   0 timetz_gt scalargtsel scalargtjoinsel ));
DATA(insert OID = 1555 ( ">="	   PGNSP PGUID b f	1266 1266	16 1553 1552	0 0   0   0 timetz_ge scalargtsel scalargtjoinsel ));

/* float48 operators */
DATA(insert OID = 1116 (  "+"		PGNSP PGUID b f 700 701 701 1126	 0	 0	 0	 0	 0 float48pl - - ));
DATA(insert OID = 1117 (  "-"		PGNSP PGUID b f 700 701 701  0	 0	 0	 0	 0	 0 float48mi - - ));
DATA(insert OID = 1118 (  "/"		PGNSP PGUID b f 700 701 701  0	 0	 0	 0	 0	 0 float48div - - ));
DATA(insert OID = 1119 (  "*"		PGNSP PGUID b f 700 701 701 1129	 0	 0	 0	 0	 0 float48mul - - ));
DATA(insert OID = 1120 (  "="		PGNSP PGUID b f  700	701  16 1130 1121  622	672 1122 1123 float48eq eqsel eqjoinsel ));
DATA(insert OID = 1121 (  "<>"		PGNSP PGUID b f  700	701  16 1131 1120  0 0 0 0 float48ne neqsel neqjoinsel ));
DATA(insert OID = 1122 (  "<"		PGNSP PGUID b f  700	701  16 1133 1125  0 0 0 0 float48lt scalarltsel scalarltjoinsel ));
DATA(insert OID = 1123 (  ">"		PGNSP PGUID b f  700	701  16 1132 1124  0 0 0 0 float48gt scalargtsel scalargtjoinsel ));
DATA(insert OID = 1124 (  "<="		PGNSP PGUID b f  700	701  16 1135 1123  0 0 0 0 float48le scalarltsel scalarltjoinsel ));
DATA(insert OID = 1125 (  ">="		PGNSP PGUID b f  700	701  16 1134 1122  0 0 0 0 float48ge scalargtsel scalargtjoinsel ));

/* float84 operators */
DATA(insert OID = 1126 (  "+"		PGNSP PGUID b f 701 700 701 1116	 0	 0	 0	 0	 0 float84pl - - ));
DATA(insert OID = 1127 (  "-"		PGNSP PGUID b f 701 700 701  0	 0	 0	 0	 0	 0 float84mi - - ));
DATA(insert OID = 1128 (  "/"		PGNSP PGUID b f 701 700 701  0	 0	 0	 0	 0	 0 float84div - - ));
DATA(insert OID = 1129 (  "*"		PGNSP PGUID b f 701 700 701 1119	 0	 0	 0	 0	 0 float84mul - - ));
DATA(insert OID = 1130 (  "="		PGNSP PGUID b f  701	700  16 1120 1131  672 622 1132 1133 float84eq eqsel eqjoinsel ));
DATA(insert OID = 1131 (  "<>"		PGNSP PGUID b f  701	700  16 1121 1130  0 0 0 0 float84ne neqsel neqjoinsel ));
DATA(insert OID = 1132 (  "<"		PGNSP PGUID b f  701	700  16 1123 1135  0 0 0 0 float84lt scalarltsel scalarltjoinsel ));
DATA(insert OID = 1133 (  ">"		PGNSP PGUID b f  701	700  16 1122 1134  0 0 0 0 float84gt scalargtsel scalargtjoinsel ));
DATA(insert OID = 1134 (  "<="		PGNSP PGUID b f  701	700  16 1125 1133  0 0 0 0 float84le scalarltsel scalarltjoinsel ));
DATA(insert OID = 1135 (  ">="		PGNSP PGUID b f  701	700  16 1124 1132  0 0 0 0 float84ge scalargtsel scalargtjoinsel ));

DATA(insert OID = 1158 (  "!"		PGNSP PGUID r f 21	  0   23 0 0 0 0 0 0 int2fac - - ));
DATA(insert OID = 1175 (  "!!"		PGNSP PGUID l f  0	 21   23 0 0 0 0 0 0 int2fac - - ));

/* LIKE hacks by Keith Parks. */
DATA(insert OID = 1207 (  "~~"	  PGNSP PGUID b f  19	25	16 0 1208 0 0 0 0 namelike likesel likejoinsel ));
#define OID_NAME_LIKE_OP		1207
DATA(insert OID = 1208 (  "!~~"   PGNSP PGUID b f  19	25	16 0 1207 0 0 0 0 namenlike nlikesel nlikejoinsel ));
DATA(insert OID = 1209 (  "~~"	  PGNSP PGUID b f  25	25	16 0 1210 0 0 0 0 textlike likesel likejoinsel ));
#define OID_TEXT_LIKE_OP		1209
DATA(insert OID = 1210 (  "!~~"   PGNSP PGUID b f  25	25	16 0 1209 0 0 0 0 textnlike nlikesel nlikejoinsel ));
DATA(insert OID = 1211 (  "~~"	  PGNSP PGUID b f  1042 25	16 0 1212 0 0 0 0 bpcharlike likesel likejoinsel ));
#define OID_BPCHAR_LIKE_OP		1211
DATA(insert OID = 1212 (  "!~~"   PGNSP PGUID b f  1042 25	16 0 1211 0 0 0 0 bpcharnlike nlikesel nlikejoinsel ));

/* case-insensitive regex hacks */
DATA(insert OID = 1226 (  "~*"		 PGNSP PGUID b f	19	25	16 0 1227  0 0 0 0 nameicregexeq icregexeqsel icregexeqjoinsel ));
#define OID_NAME_ICREGEXEQ_OP		1226
DATA(insert OID = 1227 (  "!~*"		 PGNSP PGUID b f	19	25	16 0 1226  0 0 0 0 nameicregexne icregexnesel icregexnejoinsel ));
DATA(insert OID = 1228 (  "~*"		 PGNSP PGUID b f	25	25	16 0 1229  0 0 0 0 texticregexeq icregexeqsel icregexeqjoinsel ));
#define OID_TEXT_ICREGEXEQ_OP		1228
DATA(insert OID = 1229 (  "!~*"		 PGNSP PGUID b f	25	25	16 0 1228  0 0 0 0 texticregexne icregexnesel icregexnejoinsel ));
DATA(insert OID = 1234 (  "~*"		PGNSP PGUID b f  1042  25  16 0 1235	0 0   0   0 bpcharicregexeq icregexeqsel icregexeqjoinsel ));
#define OID_BPCHAR_ICREGEXEQ_OP		1234
DATA(insert OID = 1235 ( "!~*"		PGNSP PGUID b f  1042  25  16 0 1234	0 0   0   0 bpcharicregexne icregexnesel icregexnejoinsel ));

/* timestamptz operators */
DATA(insert OID = 1320 (  "="	   PGNSP PGUID b t 1184 1184	 16 1320 1321 1322 1322 1322 1324 timestamptz_eq eqsel eqjoinsel ));
DATA(insert OID = 1321 (  "<>"	   PGNSP PGUID b f 1184 1184	 16 1321 1320 0 0 0 0 timestamptz_ne neqsel neqjoinsel ));
DATA(insert OID = 1322 (  "<"	   PGNSP PGUID b f 1184 1184	 16 1324 1325 0 0 0 0 timestamptz_lt scalarltsel scalarltjoinsel ));
DATA(insert OID = 1323 (  "<="	   PGNSP PGUID b f 1184 1184	 16 1325 1324 0 0 0 0 timestamptz_le scalarltsel scalarltjoinsel ));
DATA(insert OID = 1324 (  ">"	   PGNSP PGUID b f 1184 1184	 16 1322 1323 0 0 0 0 timestamptz_gt scalargtsel scalargtjoinsel ));
DATA(insert OID = 1325 (  ">="	   PGNSP PGUID b f 1184 1184	 16 1323 1322 0 0 0 0 timestamptz_ge scalargtsel scalargtjoinsel ));
DATA(insert OID = 1327 (  "+"	   PGNSP PGUID b f 1184 1186 1184	 0	0 0 0 0 0 timestamptz_pl_span - - ));
DATA(insert OID = 1328 (  "-"	   PGNSP PGUID b f 1184 1184 1186	 0	0 0 0 0 0 timestamptz_mi - - ));
DATA(insert OID = 1329 (  "-"	   PGNSP PGUID b f 1184 1186 1184	 0	0 0 0 0 0 timestamptz_mi_span - - ));

/* interval operators */
DATA(insert OID = 1330 (  "="	   PGNSP PGUID b t 1186 1186	 16 1330 1331 1332 1332 1332 1334 interval_eq eqsel eqjoinsel ));
DATA(insert OID = 1331 (  "<>"	   PGNSP PGUID b f 1186 1186	 16 1331 1330 0 0 0 0 interval_ne neqsel neqjoinsel ));
DATA(insert OID = 1332 (  "<"	   PGNSP PGUID b f 1186 1186	 16 1334 1335 0 0 0 0 interval_lt scalarltsel scalarltjoinsel ));
DATA(insert OID = 1333 (  "<="	   PGNSP PGUID b f 1186 1186	 16 1335 1334 0 0 0 0 interval_le scalarltsel scalarltjoinsel ));
DATA(insert OID = 1334 (  ">"	   PGNSP PGUID b f 1186 1186	 16 1332 1333 0 0 0 0 interval_gt scalargtsel scalargtjoinsel ));
DATA(insert OID = 1335 (  ">="	   PGNSP PGUID b f 1186 1186	 16 1333 1332 0 0 0 0 interval_ge scalargtsel scalargtjoinsel ));

DATA(insert OID = 1336 (  "-"	   PGNSP PGUID l f	0 1186 1186    0	0 0 0 0 0 interval_um - - ));
DATA(insert OID = 1337 (  "+"	   PGNSP PGUID b f 1186 1186 1186 1337	0 0 0 0 0 interval_pl - - ));
DATA(insert OID = 1338 (  "-"	   PGNSP PGUID b f 1186 1186 1186	 0	0 0 0 0 0 interval_mi - - ));

DATA(insert OID = 1360 (  "+"	   PGNSP PGUID b f 1082 1083 1114	 0	0 0 0 0 0 datetime_pl - - ));
DATA(insert OID = 1361 (  "+"	   PGNSP PGUID b f 1082 1266 1184	 0	0 0 0 0 0 datetimetz_pl - - ));
DATA(insert OID = 1363 (  "+"	   PGNSP PGUID b f 1083 1082 1114	 0	0 0 0 0 0 timedate_pl - - ));
DATA(insert OID = 1366 (  "+"	   PGNSP PGUID b f 1266 1082 1184	 0	0 0 0 0 0 timetzdate_pl - - ));

DATA(insert OID = 1399 (  "-"	   PGNSP PGUID b f 1083 1083 1186	 0	0 0 0 0 0 time_mi_time - - ));

/* additional geometric operators - thomas 97/04/18 */
DATA(insert OID = 1420 (  "@@"	  PGNSP PGUID l f	 0	718 600   0    0	0	 0	 0	 0 circle_center - - ));
DATA(insert OID = 1500 (  "="	  PGNSP PGUID b f  718	718 16 1500 1501 1502 1502 1502 1503 circle_eq eqsel eqjoinsel ));
DATA(insert OID = 1501 (  "<>"	  PGNSP PGUID b f  718	718 16 1501 1500	0	 0	 0	 0 circle_ne neqsel neqjoinsel ));
DATA(insert OID = 1502 (  "<"	  PGNSP PGUID b f  718	718 16 1503 1505	0	 0	 0	 0 circle_lt areasel areajoinsel ));
DATA(insert OID = 1503 (  ">"	  PGNSP PGUID b f  718	718 16 1502 1504	0	 0	 0	 0 circle_gt areasel areajoinsel ));
DATA(insert OID = 1504 (  "<="	  PGNSP PGUID b f  718	718 16 1505 1503	0	 0	 0	 0 circle_le areasel areajoinsel ));
DATA(insert OID = 1505 (  ">="	  PGNSP PGUID b f  718	718 16 1504 1502	0	 0	 0	 0 circle_ge areasel areajoinsel ));

DATA(insert OID = 1506 (  "<<"	  PGNSP PGUID b f  718	718 16	  0    0	0	 0	 0	 0 circle_left positionsel positionjoinsel ));
DATA(insert OID = 1507 (  "&<"	  PGNSP PGUID b f  718	718 16	  0    0	0	 0	 0	 0 circle_overleft positionsel positionjoinsel ));
DATA(insert OID = 1508 (  "&>"	  PGNSP PGUID b f  718	718 16	  0    0	0	 0	 0	 0 circle_overright positionsel positionjoinsel ));
DATA(insert OID = 1509 (  ">>"	  PGNSP PGUID b f  718	718 16	  0    0	0	 0	 0	 0 circle_right positionsel positionjoinsel ));
DATA(insert OID = 1510 (  "@"	  PGNSP PGUID b f  718	718 16 1511    0	0	 0	 0	 0 circle_contained contsel contjoinsel ));
DATA(insert OID = 1511 (  "~"	  PGNSP PGUID b f  718	718 16 1510    0	0	 0	 0	 0 circle_contain contsel contjoinsel ));
DATA(insert OID = 1512 (  "~="	  PGNSP PGUID b f  718	718 16 1512    0	0	 0	 0	 0 circle_same eqsel eqjoinsel ));
DATA(insert OID = 1513 (  "&&"	  PGNSP PGUID b f  718	718 16 1513    0	0	 0	 0	 0 circle_overlap areasel areajoinsel ));
DATA(insert OID = 1514 (  ">^"	  PGNSP PGUID b f  718	718 16	  0    0	0	 0	 0	 0 circle_above positionsel positionjoinsel ));
DATA(insert OID = 1515 (  "<^"	  PGNSP PGUID b f  718	718 16	  0    0	0	 0	 0	 0 circle_below positionsel positionjoinsel ));

DATA(insert OID = 1516 (  "+"	  PGNSP PGUID b f  718	600  718	  0    0	0	 0	 0	 0 circle_add_pt - - ));
DATA(insert OID = 1517 (  "-"	  PGNSP PGUID b f  718	600  718	  0    0	0	 0	 0	 0 circle_sub_pt - - ));
DATA(insert OID = 1518 (  "*"	  PGNSP PGUID b f  718	600  718	  0    0	0	 0	 0	 0 circle_mul_pt - - ));
DATA(insert OID = 1519 (  "/"	  PGNSP PGUID b f  718	600  718	  0    0	0	 0	 0	 0 circle_div_pt - - ));

DATA(insert OID = 1520 (  "<->"   PGNSP PGUID b f  718	718  701 1520	 0	0	 0	 0	 0 circle_distance - - ));
DATA(insert OID = 1521 (  "#"	  PGNSP PGUID l f	 0	604 23	  0    0	0	 0	 0	 0 poly_npoints - - ));
DATA(insert OID = 1522 (  "<->"   PGNSP PGUID b f  600	718  701	  0    0	0	 0	 0	 0 dist_pc - - ));
DATA(insert OID = 1523 (  "<->"   PGNSP PGUID b f  718	604  701	  0    0	0	 0	 0	 0 dist_cpoly - - ));

/* additional geometric operators - thomas 1997-07-09 */
DATA(insert OID = 1524 (  "<->"   PGNSP PGUID b f  628	603  701	  0  0 0 0 0 0 dist_lb - - ));

DATA(insert OID = 1525 (  "?#"	  PGNSP PGUID b f  601	601 16 1525  0 0 0 0 0 lseg_intersect - - ));
DATA(insert OID = 1526 (  "?||"   PGNSP PGUID b f  601	601 16 1526  0 0 0 0 0 lseg_parallel - - ));
DATA(insert OID = 1527 (  "?-|"   PGNSP PGUID b f  601	601 16 1527  0 0 0 0 0 lseg_perp - - ));
DATA(insert OID = 1528 (  "?-"	  PGNSP PGUID l f	 0	601 16	  0  0 0 0 0 0 lseg_horizontal - - ));
DATA(insert OID = 1529 (  "?|"	  PGNSP PGUID l f	 0	601 16	  0  0 0 0 0 0 lseg_vertical - - ));
DATA(insert OID = 1535 (  "="	  PGNSP PGUID b f  601	601 16 1535 1586 0 0 0 0 lseg_eq eqsel eqjoinsel ));
DATA(insert OID = 1536 (  "#"	  PGNSP PGUID b f  601	601  600 1536  0 0 0 0 0 lseg_interpt - - ));
DATA(insert OID = 1537 (  "?#"	  PGNSP PGUID b f  601	628 16	  0  0 0 0 0 0 inter_sl - - ));
DATA(insert OID = 1538 (  "?#"	  PGNSP PGUID b f  601	603 16	  0  0 0 0 0 0 inter_sb - - ));
DATA(insert OID = 1539 (  "?#"	  PGNSP PGUID b f  628	603 16	  0  0 0 0 0 0 inter_lb - - ));

DATA(insert OID = 1546 (  "@"	  PGNSP PGUID b f  600	628 16	  0  0 0 0 0 0 on_pl - - ));
DATA(insert OID = 1547 (  "@"	  PGNSP PGUID b f  600	601 16	  0  0 0 0 0 0 on_ps - - ));
DATA(insert OID = 1548 (  "@"	  PGNSP PGUID b f  601	628 16	  0  0 0 0 0 0 on_sl - - ));
DATA(insert OID = 1549 (  "@"	  PGNSP PGUID b f  601	603 16	  0  0 0 0 0 0 on_sb - - ));

DATA(insert OID = 1557 (  "##"	  PGNSP PGUID b f  600	628  600	  0  0 0 0 0 0 close_pl - - ));
DATA(insert OID = 1558 (  "##"	  PGNSP PGUID b f  600	601  600	  0  0 0 0 0 0 close_ps - - ));
DATA(insert OID = 1559 (  "##"	  PGNSP PGUID b f  600	603  600	  0  0 0 0 0 0 close_pb - - ));

DATA(insert OID = 1566 (  "##"	  PGNSP PGUID b f  601	628  600	  0  0 0 0 0 0 close_sl - - ));
DATA(insert OID = 1567 (  "##"	  PGNSP PGUID b f  601	603  600	  0  0 0 0 0 0 close_sb - - ));
DATA(insert OID = 1568 (  "##"	  PGNSP PGUID b f  628	603  600	  0  0 0 0 0 0 close_lb - - ));
DATA(insert OID = 1577 (  "##"	  PGNSP PGUID b f  628	601  600	  0  0 0 0 0 0 close_ls - - ));
DATA(insert OID = 1578 (  "##"	  PGNSP PGUID b f  601	601  600	  0  0 0 0 0 0 close_lseg - - ));
DATA(insert OID = 1583 (  "*"	  PGNSP PGUID b f 1186	701 1186	  0  0 0 0 0 0 interval_mul - - ));
DATA(insert OID = 1584 (  "*"	  PGNSP PGUID b f  701 1186 1186	  0  0 0 0 0 0 mul_d_interval - - ));
DATA(insert OID = 1585 (  "/"	  PGNSP PGUID b f 1186	701 1186	  0  0 0 0 0 0 interval_div - - ));

DATA(insert OID = 1586 (  "<>"	  PGNSP PGUID b f  601	601 16 1586 1535 0 0 0 0 lseg_ne neqsel neqjoinsel ));
DATA(insert OID = 1587 (  "<"	  PGNSP PGUID b f  601	601 16 1589 1590 0 0 0 0 lseg_lt - - ));
DATA(insert OID = 1588 (  "<="	  PGNSP PGUID b f  601	601 16 1590 1589 0 0 0 0 lseg_le - - ));
DATA(insert OID = 1589 (  ">"	  PGNSP PGUID b f  601	601 16 1587 1588 0 0 0 0 lseg_gt - - ));
DATA(insert OID = 1590 (  ">="	  PGNSP PGUID b f  601	601 16 1588 1587 0 0 0 0 lseg_ge - - ));

DATA(insert OID = 1591 (  "@-@"   PGNSP PGUID l f 0  601	701    0  0 0 0 0 0 lseg_length - - ));

DATA(insert OID = 1611 (  "?#"	  PGNSP PGUID b f  628	628 16 1611  0 0 0 0 0 line_intersect - - ));
DATA(insert OID = 1612 (  "?||"   PGNSP PGUID b f  628	628 16 1612  0 0 0 0 0 line_parallel - - ));
DATA(insert OID = 1613 (  "?-|"   PGNSP PGUID b f  628	628 16 1613  0 0 0 0 0 line_perp - - ));
DATA(insert OID = 1614 (  "?-"	  PGNSP PGUID l f	 0	628 16	  0  0 0 0 0 0 line_horizontal - - ));
DATA(insert OID = 1615 (  "?|"	  PGNSP PGUID l f	 0	628 16	  0  0 0 0 0 0 line_vertical - - ));
DATA(insert OID = 1616 (  "="	  PGNSP PGUID b f  628	628 16 1616  0 0 0 0 0 line_eq eqsel eqjoinsel ));
DATA(insert OID = 1617 (  "#"	  PGNSP PGUID b f  628	628  600 1617  0 0 0 0 0 line_interpt - - ));

/* MAC type */
DATA(insert OID = 1220 (  "="	   PGNSP PGUID b t 829 829	 16 1220 1221 1222 1222 1222 1224 macaddr_eq eqsel eqjoinsel ));
DATA(insert OID = 1221 (  "<>"	   PGNSP PGUID b f 829 829	 16 1221 1220	 0	  0   0   0 macaddr_ne neqsel neqjoinsel ));
DATA(insert OID = 1222 (  "<"	   PGNSP PGUID b f 829 829	 16 1224 1225	 0	  0   0   0 macaddr_lt scalarltsel scalarltjoinsel ));
DATA(insert OID = 1223 (  "<="	   PGNSP PGUID b f 829 829	 16 1225 1224	 0	  0   0   0 macaddr_le scalarltsel scalarltjoinsel ));
DATA(insert OID = 1224 (  ">"	   PGNSP PGUID b f 829 829	 16 1222 1223	 0	  0   0   0 macaddr_gt scalargtsel scalargtjoinsel ));
DATA(insert OID = 1225 (  ">="	   PGNSP PGUID b f 829 829	 16 1223 1222	 0	  0   0   0 macaddr_ge scalargtsel scalargtjoinsel ));

/* INET type */
DATA(insert OID = 1201 (  "="	   PGNSP PGUID b t 869 869	 16 1201 1202 1203 1203 1203 1205 network_eq eqsel eqjoinsel ));
DATA(insert OID = 1202 (  "<>"	   PGNSP PGUID b f 869 869	 16 1202 1201	 0	  0   0   0 network_ne neqsel neqjoinsel ));
DATA(insert OID = 1203 (  "<"	   PGNSP PGUID b f 869 869	 16 1205 1206	 0	  0   0   0 network_lt scalarltsel scalarltjoinsel ));
DATA(insert OID = 1204 (  "<="	   PGNSP PGUID b f 869 869	 16 1206 1205	 0	  0   0   0 network_le scalarltsel scalarltjoinsel ));
DATA(insert OID = 1205 (  ">"	   PGNSP PGUID b f 869 869	 16 1203 1204	 0	  0   0   0 network_gt scalargtsel scalargtjoinsel ));
DATA(insert OID = 1206 (  ">="	   PGNSP PGUID b f 869 869	 16 1204 1203	 0	  0   0   0 network_ge scalargtsel scalargtjoinsel ));
DATA(insert OID = 931  (  "<<"	   PGNSP PGUID b f 869 869	 16 933		0	 0	  0   0   0 network_sub - - ));
#define OID_INET_SUB_OP				  931
DATA(insert OID = 932  (  "<<="    PGNSP PGUID b f 869 869	 16 934		0	 0	  0   0   0 network_subeq - - ));
#define OID_INET_SUBEQ_OP				932
DATA(insert OID = 933  (  ">>"	   PGNSP PGUID b f 869 869	 16 931		0	 0	  0   0   0 network_sup - - ));
#define OID_INET_SUP_OP				  933
DATA(insert OID = 934  (  ">>="    PGNSP PGUID b f 869 869	 16 932		0	 0	  0   0   0 network_supeq - - ));
#define OID_INET_SUPEQ_OP				934

/* CIDR type */
DATA(insert OID = 820 (  "="	   PGNSP PGUID b t 650 650	 16 820 821 822 822 822 824 network_eq eqsel eqjoinsel ));
DATA(insert OID = 821 (  "<>"	   PGNSP PGUID b f 650 650	 16 821 820   0   0   0   0 network_ne neqsel neqjoinsel ));
DATA(insert OID = 822 (  "<"	   PGNSP PGUID b f 650 650	 16 824 825   0   0   0   0 network_lt scalarltsel scalarltjoinsel ));
DATA(insert OID = 823 (  "<="	   PGNSP PGUID b f 650 650	 16 825 824   0   0   0   0 network_le scalarltsel scalarltjoinsel ));
DATA(insert OID = 824 (  ">"	   PGNSP PGUID b f 650 650	 16 822 823   0   0   0   0 network_gt scalargtsel scalargtjoinsel ));
DATA(insert OID = 825 (  ">="	   PGNSP PGUID b f 650 650	 16 823 822   0   0   0   0 network_ge scalargtsel scalargtjoinsel ));
DATA(insert OID = 826 (  "<<"	   PGNSP PGUID b f 650 650	 16 828   0   0   0   0   0 network_sub - - ));
#define OID_CIDR_SUB_OP		826
DATA(insert OID = 827 (  "<<="	   PGNSP PGUID b f 650 650	 16 1004  0   0   0   0   0 network_subeq - - ));
#define OID_CIDR_SUBEQ_OP	827
DATA(insert OID = 828 (  ">>"	   PGNSP PGUID b f 650 650	 16 826   0   0   0   0   0 network_sup - - ));
#define OID_CIDR_SUP_OP		828
DATA(insert OID = 1004 ( ">>="	   PGNSP PGUID b f 650 650	 16 827   0   0   0   0   0 network_supeq - - ));
#define OID_CIDR_SUPEQ_OP	1004

/* case-insensitive LIKE hacks */
DATA(insert OID = 1625 (  "~~*"   PGNSP PGUID b f  19	25	16 0 1626 0 0 0 0 nameiclike iclikesel iclikejoinsel ));
#define OID_NAME_ICLIKE_OP		1625
DATA(insert OID = 1626 (  "!~~*"  PGNSP PGUID b f  19	25	16 0 1625 0 0 0 0 nameicnlike icnlikesel icnlikejoinsel ));
DATA(insert OID = 1627 (  "~~*"   PGNSP PGUID b f  25	25	16 0 1628 0 0 0 0 texticlike iclikesel iclikejoinsel ));
#define OID_TEXT_ICLIKE_OP		1627
DATA(insert OID = 1628 (  "!~~*"  PGNSP PGUID b f  25	25	16 0 1627 0 0 0 0 texticnlike icnlikesel icnlikejoinsel ));
DATA(insert OID = 1629 (  "~~*"   PGNSP PGUID b f  1042 25	16 0 1630 0 0 0 0 bpchariclike iclikesel iclikejoinsel ));
#define OID_BPCHAR_ICLIKE_OP	1629
DATA(insert OID = 1630 (  "!~~*"  PGNSP PGUID b f  1042 25	16 0 1629 0 0 0 0 bpcharicnlike icnlikesel icnlikejoinsel ));

/* NUMERIC type - OID's 1700-1799 */
DATA(insert OID = 1751 (  "-"	   PGNSP PGUID l f	0 1700 1700    0	0 0 0 0 0 numeric_uminus - - ));
DATA(insert OID = 1752 (  "="	   PGNSP PGUID b f 1700 1700	 16 1752 1753 1754 1754 1754 1756 numeric_eq eqsel eqjoinsel ));
DATA(insert OID = 1753 (  "<>"	   PGNSP PGUID b f 1700 1700	 16 1753 1752 0 0 0 0 numeric_ne neqsel neqjoinsel ));
DATA(insert OID = 1754 (  "<"	   PGNSP PGUID b f 1700 1700	 16 1756 1757 0 0 0 0 numeric_lt scalarltsel scalarltjoinsel ));
DATA(insert OID = 1755 (  "<="	   PGNSP PGUID b f 1700 1700	 16 1757 1756 0 0 0 0 numeric_le scalarltsel scalarltjoinsel ));
DATA(insert OID = 1756 (  ">"	   PGNSP PGUID b f 1700 1700	 16 1754 1755 0 0 0 0 numeric_gt scalargtsel scalargtjoinsel ));
DATA(insert OID = 1757 (  ">="	   PGNSP PGUID b f 1700 1700	 16 1755 1754 0 0 0 0 numeric_ge scalargtsel scalargtjoinsel ));
DATA(insert OID = 1758 (  "+"	   PGNSP PGUID b f 1700 1700 1700 1758	0 0 0 0 0 numeric_add - - ));
DATA(insert OID = 1759 (  "-"	   PGNSP PGUID b f 1700 1700 1700	 0	0 0 0 0 0 numeric_sub - - ));
DATA(insert OID = 1760 (  "*"	   PGNSP PGUID b f 1700 1700 1700 1760	0 0 0 0 0 numeric_mul - - ));
DATA(insert OID = 1761 (  "/"	   PGNSP PGUID b f 1700 1700 1700	 0	0 0 0 0 0 numeric_div - - ));
DATA(insert OID = 1762 (  "%"	   PGNSP PGUID b f 1700 1700 1700	 0	0 0 0 0 0 numeric_mod - - ));
DATA(insert OID = 1763 (  "@"	   PGNSP PGUID l f	0 1700 1700    0	0 0 0 0 0 numeric_abs - - ));

DATA(insert OID = 1784 (  "="	  PGNSP PGUID b f 1560 1560 16 1784 1785 1786 1786 1786 1787 biteq eqsel eqjoinsel ));
DATA(insert OID = 1785 (  "<>"	  PGNSP PGUID b f 1560 1560 16 1785 1784	0	 0	 0	 0 bitne neqsel neqjoinsel ));
DATA(insert OID = 1786 (  "<"	  PGNSP PGUID b f 1560 1560 16 1787 1789	0	 0	 0	 0 bitlt scalarltsel scalarltjoinsel ));
DATA(insert OID = 1787 (  ">"	  PGNSP PGUID b f 1560 1560 16 1786 1788	0	 0	 0	 0 bitgt scalargtsel scalargtjoinsel ));
DATA(insert OID = 1788 (  "<="	  PGNSP PGUID b f 1560 1560 16 1789 1787	0	 0	 0	 0 bitle scalarltsel scalarltjoinsel ));
DATA(insert OID = 1789 (  ">="	  PGNSP PGUID b f 1560 1560 16 1788 1786	0	 0	 0	 0 bitge scalargtsel scalargtjoinsel ));
DATA(insert OID = 1791 (  "&"	  PGNSP PGUID b f 1560 1560 1560 1791	 0	0	 0	 0	 0 bitand - - ));
DATA(insert OID = 1792 (  "|"	  PGNSP PGUID b f 1560 1560 1560 1792	 0	0	 0	 0	 0 bitor - - ));
DATA(insert OID = 1793 (  "#"	  PGNSP PGUID b f 1560 1560 1560 1793	 0	0	 0	 0	 0 bitxor - - ));
DATA(insert OID = 1794 (  "~"	  PGNSP PGUID l f	 0 1560 1560	  0    0	0	 0	 0	 0 bitnot - - ));
DATA(insert OID = 1795 (  "<<"	  PGNSP PGUID b f 1560	 23 1560	  0    0	0	 0	 0	 0 bitshiftleft - - ));
DATA(insert OID = 1796 (  ">>"	  PGNSP PGUID b f 1560	 23 1560	  0    0	0	 0	 0	 0 bitshiftright - - ));
DATA(insert OID = 1797 (  "||"	  PGNSP PGUID b f 1560 1560 1560	  0    0	0	 0	 0	 0 bitcat - - ));

DATA(insert OID = 1800 (  "+"	   PGNSP PGUID b f 1083 1186 1083	 0	0 0 0 0 0 time_pl_interval - - ));
DATA(insert OID = 1801 (  "-"	   PGNSP PGUID b f 1083 1186 1083	 0	0 0 0 0 0 time_mi_interval - - ));
DATA(insert OID = 1802 (  "+"	   PGNSP PGUID b f 1266 1186 1266	 0	0 0 0 0 0 timetz_pl_interval - - ));
DATA(insert OID = 1803 (  "-"	   PGNSP PGUID b f 1266 1186 1266	 0	0 0 0 0 0 timetz_mi_interval - - ));

DATA(insert OID = 1804 (  "="	  PGNSP PGUID b f 1562 1562 16 1804 1805 1806 1806 1806 1807 varbiteq eqsel eqjoinsel ));
DATA(insert OID = 1805 (  "<>"	  PGNSP PGUID b f 1562 1562 16 1805 1804	0	 0	 0	 0 varbitne neqsel neqjoinsel ));
DATA(insert OID = 1806 (  "<"	  PGNSP PGUID b f 1562 1562 16 1807 1809	0	 0	 0	 0 varbitlt scalarltsel scalarltjoinsel ));
DATA(insert OID = 1807 (  ">"	  PGNSP PGUID b f 1562 1562 16 1806 1808	0	 0	 0	 0 varbitgt scalargtsel scalargtjoinsel ));
DATA(insert OID = 1808 (  "<="	  PGNSP PGUID b f 1562 1562 16 1809 1807	0	 0	 0	 0 varbitle scalarltsel scalarltjoinsel ));
DATA(insert OID = 1809 (  ">="	  PGNSP PGUID b f 1562 1562 16 1808 1806	0	 0	 0	 0 varbitge scalargtsel scalargtjoinsel ));

DATA(insert OID = 1849 (  "+"	   PGNSP PGUID b f 1186 1083 1083	 0	0 0 0 0 0 interval_pl_time - - ));

DATA(insert OID = 1862 ( "="	   PGNSP PGUID b f	21	20	16 1868  1863  95 412 1864 1865 int28eq eqsel eqjoinsel ));
DATA(insert OID = 1863 ( "<>"	   PGNSP PGUID b f	21	20	16 1869  1862	0	0	0	0 int28ne neqsel neqjoinsel ));
DATA(insert OID = 1864 ( "<"	   PGNSP PGUID b f	21	20	16 1871  1867	0	0	0	0 int28lt scalarltsel scalarltjoinsel ));
DATA(insert OID = 1865 ( ">"	   PGNSP PGUID b f	21	20	16 1870  1866	0	0	0	0 int28gt scalargtsel scalargtjoinsel ));
DATA(insert OID = 1866 ( "<="	   PGNSP PGUID b f	21	20	16 1873  1865	0	0	0	0 int28le scalarltsel scalarltjoinsel ));
DATA(insert OID = 1867 ( ">="	   PGNSP PGUID b f	21	20	16 1872  1864	0	0	0	0 int28ge scalargtsel scalargtjoinsel ));

DATA(insert OID = 1868 ( "="	   PGNSP PGUID b f	20	21	16	1862 1869 412 95 1870 1871 int82eq eqsel eqjoinsel ));
DATA(insert OID = 1869 ( "<>"	   PGNSP PGUID b f	20	21	16	1863 1868	0  0   0   0 int82ne neqsel neqjoinsel ));
DATA(insert OID = 1870 ( "<"	   PGNSP PGUID b f	20	21	16	1865 1873	0  0   0   0 int82lt scalarltsel scalarltjoinsel ));
DATA(insert OID = 1871 ( ">"	   PGNSP PGUID b f	20	21	16	1864 1872	0  0   0   0 int82gt scalargtsel scalargtjoinsel ));
DATA(insert OID = 1872 ( "<="	   PGNSP PGUID b f	20	21	16	1867 1871	0  0   0   0 int82le scalarltsel scalarltjoinsel ));
DATA(insert OID = 1873 ( ">="	   PGNSP PGUID b f	20	21	16	1866 1870	0  0   0   0 int82ge scalargtsel scalargtjoinsel ));

DATA(insert OID = 1874 ( "&"	   PGNSP PGUID b f	21	21	21	1874	  0   0  0	 0	 0 int2and - - ));
DATA(insert OID = 1875 ( "|"	   PGNSP PGUID b f	21	21	21	1875	  0   0  0	 0	 0 int2or - - ));
DATA(insert OID = 1876 ( "#"	   PGNSP PGUID b f	21	21	21	1876	  0   0  0	 0	 0 int2xor - - ));
DATA(insert OID = 1877 ( "~"	   PGNSP PGUID l f	 0	21	21	 0	  0   0  0	 0	 0 int2not - - ));
DATA(insert OID = 1878 ( "<<"	   PGNSP PGUID b f	21	23	21	 0	  0   0  0	 0	 0 int2shl - - ));
DATA(insert OID = 1879 ( ">>"	   PGNSP PGUID b f	21	23	21	 0	  0   0  0	 0	 0 int2shr - - ));

DATA(insert OID = 1880 ( "&"	   PGNSP PGUID b f	23	23	23	1880	  0   0  0	 0	 0 int4and - - ));
DATA(insert OID = 1881 ( "|"	   PGNSP PGUID b f	23	23	23	1881	  0   0  0	 0	 0 int4or - - ));
DATA(insert OID = 1882 ( "#"	   PGNSP PGUID b f	23	23	23	1882	  0   0  0	 0	 0 int4xor - - ));
DATA(insert OID = 1883 ( "~"	   PGNSP PGUID l f	 0	23	23	 0	  0   0  0	 0	 0 int4not - - ));
DATA(insert OID = 1884 ( "<<"	   PGNSP PGUID b f	23	23	23	 0	  0   0  0	 0	 0 int4shl - - ));
DATA(insert OID = 1885 ( ">>"	   PGNSP PGUID b f	23	23	23	 0	  0   0  0	 0	 0 int4shr - - ));

DATA(insert OID = 1886 ( "&"	   PGNSP PGUID b f	20	20	20	1886	  0   0  0	 0	 0 int8and - - ));
DATA(insert OID = 1887 ( "|"	   PGNSP PGUID b f	20	20	20	1887	  0   0  0	 0	 0 int8or - - ));
DATA(insert OID = 1888 ( "#"	   PGNSP PGUID b f	20	20	20	1888	  0   0  0	 0	 0 int8xor - - ));
DATA(insert OID = 1889 ( "~"	   PGNSP PGUID l f	 0	20	20	 0	  0   0  0	 0	 0 int8not - - ));
DATA(insert OID = 1890 ( "<<"	   PGNSP PGUID b f	20	23	20	 0	  0   0  0	 0	 0 int8shl - - ));
DATA(insert OID = 1891 ( ">>"	   PGNSP PGUID b f	20	23	20	 0	  0   0  0	 0	 0 int8shr - - ));

DATA(insert OID = 1916 (  "+"	   PGNSP PGUID l f	 0	20	20	0	0	0	0	0	0 int8up - - ));
DATA(insert OID = 1917 (  "+"	   PGNSP PGUID l f	 0	21	21	0	0	0	0	0	0 int2up - - ));
DATA(insert OID = 1918 (  "+"	   PGNSP PGUID l f	 0	23	23	0	0	0	0	0	0 int4up - - ));
DATA(insert OID = 1919 (  "+"	   PGNSP PGUID l f	 0	700 700 0	0	0	0	0	0 float4up - - ));
DATA(insert OID = 1920 (  "+"	   PGNSP PGUID l f	 0	701 701 0	0	0	0	0	0 float8up - - ));
DATA(insert OID = 1921 (  "+"	   PGNSP PGUID l f	 0 1700 1700	0	0	0	0	0	0 numeric_uplus - - ));

/* bytea operators */
DATA(insert OID = 1955 ( "="	   PGNSP PGUID b t 17 17	16 1955 1956 1957 1957 1957 1959 byteaeq eqsel eqjoinsel ));
DATA(insert OID = 1956 ( "<>"	   PGNSP PGUID b f 17 17	16 1956 1955 0	  0   0   0 byteane neqsel neqjoinsel ));
DATA(insert OID = 1957 ( "<"	   PGNSP PGUID b f 17 17	16 1959 1960 0	  0   0   0 bytealt scalarltsel scalarltjoinsel ));
DATA(insert OID = 1958 ( "<="	   PGNSP PGUID b f 17 17	16 1960 1959 0	  0   0   0 byteale scalarltsel scalarltjoinsel ));
DATA(insert OID = 1959 ( ">"	   PGNSP PGUID b f 17 17	16 1957 1958 0	  0   0   0 byteagt scalargtsel scalargtjoinsel ));
DATA(insert OID = 1960 ( ">="	   PGNSP PGUID b f 17 17	16 1958 1957 0	  0   0   0 byteage scalargtsel scalargtjoinsel ));
DATA(insert OID = 2016 (  "~~"	   PGNSP PGUID b f 17 17	16 0	2017 0	  0   0   0 bytealike likesel likejoinsel ));
#define OID_BYTEA_LIKE_OP		2016
DATA(insert OID = 2017 (  "!~~"    PGNSP PGUID b f 17 17	16 0	2016 0	  0   0   0 byteanlike nlikesel nlikejoinsel ));
DATA(insert OID = 2018 (  "||"	   PGNSP PGUID b f 17 17	17 0	0	 0	  0   0   0 byteacat - - ));

/* timestamp operators */
DATA(insert OID = 2060 (  "="	   PGNSP PGUID b t 1114 1114	 16 2060 2061 2062 2062 2062 2064 timestamp_eq eqsel eqjoinsel ));
DATA(insert OID = 2061 (  "<>"	   PGNSP PGUID b f 1114 1114	 16 2061 2060 0 0 0 0 timestamp_ne neqsel neqjoinsel ));
DATA(insert OID = 2062 (  "<"	   PGNSP PGUID b f 1114 1114	 16 2064 2065 0 0 0 0 timestamp_lt scalarltsel scalarltjoinsel ));
DATA(insert OID = 2063 (  "<="	   PGNSP PGUID b f 1114 1114	 16 2065 2064 0 0 0 0 timestamp_le scalarltsel scalarltjoinsel ));
DATA(insert OID = 2064 (  ">"	   PGNSP PGUID b f 1114 1114	 16 2062 2063 0 0 0 0 timestamp_gt scalargtsel scalargtjoinsel ));
DATA(insert OID = 2065 (  ">="	   PGNSP PGUID b f 1114 1114	 16 2063 2062 0 0 0 0 timestamp_ge scalargtsel scalargtjoinsel ));
DATA(insert OID = 2066 (  "+"	   PGNSP PGUID b f 1114 1186 1114	 0	0 0 0 0 0 timestamp_pl_span - - ));
DATA(insert OID = 2067 (  "-"	   PGNSP PGUID b f 1114 1114 1186	 0	0 0 0 0 0 timestamp_mi - - ));
DATA(insert OID = 2068 (  "-"	   PGNSP PGUID b f 1114 1186 1114	 0	0 0 0 0 0 timestamp_mi_span - - ));

/* character-by-character (not collation order) comparison operators for character types */

DATA(insert OID = 2314 ( "~<~"	PGNSP PGUID b f 25 25 16 2318 2317 0 0 0 0 text_pattern_lt scalarltsel scalarltjoinsel ));
DATA(insert OID = 2315 ( "~<=~" PGNSP PGUID b f 25 25 16 2317 2318 0 0 0 0 text_pattern_le scalarltsel scalarltjoinsel ));
DATA(insert OID = 2316 ( "~=~"	PGNSP PGUID b t 25 25 16 2316 2319 2314 2314 2314 2318 text_pattern_eq eqsel eqjoinsel ));
DATA(insert OID = 2317 ( "~>=~" PGNSP PGUID b f 25 25 16 2315 2314 0 0 0 0 text_pattern_ge scalargtsel scalargtjoinsel ));
DATA(insert OID = 2318 ( "~>~"	PGNSP PGUID b f 25 25 16 2314 2315 0 0 0 0 text_pattern_gt scalargtsel scalargtjoinsel ));
DATA(insert OID = 2319 ( "~<>~" PGNSP PGUID b f 25 25 16 2319 2316 0 0 0 0 text_pattern_ne neqsel neqjoinsel ));

DATA(insert OID = 2326 ( "~<~"	PGNSP PGUID b f 1042 1042 16 2330 2329 0 0 0 0 bpchar_pattern_lt scalarltsel scalarltjoinsel ));
DATA(insert OID = 2327 ( "~<=~" PGNSP PGUID b f 1042 1042 16 2329 2330 0 0 0 0 bpchar_pattern_le scalarltsel scalarltjoinsel ));
DATA(insert OID = 2328 ( "~=~"	PGNSP PGUID b t 1042 1042 16 2328 2331 2326 2326 2326 2330 bpchar_pattern_eq eqsel eqjoinsel ));
DATA(insert OID = 2329 ( "~>=~" PGNSP PGUID b f 1042 1042 16 2327 2326 0 0 0 0 bpchar_pattern_ge scalargtsel scalargtjoinsel ));
DATA(insert OID = 2330 ( "~>~"	PGNSP PGUID b f 1042 1042 16 2326 2327 0 0 0 0 bpchar_pattern_gt scalargtsel scalargtjoinsel ));
DATA(insert OID = 2331 ( "~<>~" PGNSP PGUID b f 1042 1042 16 2331 2328 0 0 0 0 bpchar_pattern_ne neqsel neqjoinsel ));

DATA(insert OID = 2332 ( "~<~"	PGNSP PGUID b f 19 19 16 2336 2335 0 0 0 0 name_pattern_lt scalarltsel scalarltjoinsel ));
DATA(insert OID = 2333 ( "~<=~" PGNSP PGUID b f 19 19 16 2335 2336 0 0 0 0 name_pattern_le scalarltsel scalarltjoinsel ));
DATA(insert OID = 2334 ( "~=~"	PGNSP PGUID b t 19 19 16 2334 2337 2332 2332 2332 2336 name_pattern_eq eqsel eqjoinsel ));
DATA(insert OID = 2335 ( "~>=~" PGNSP PGUID b f 19 19 16 2333 2332 0 0 0 0 name_pattern_ge scalargtsel scalargtjoinsel ));
DATA(insert OID = 2336 ( "~>~"	PGNSP PGUID b f 19 19 16 2332 2333 0 0 0 0 name_pattern_gt scalargtsel scalargtjoinsel ));
DATA(insert OID = 2337 ( "~<>~" PGNSP PGUID b f 19 19 16 2337 2334 0 0 0 0 name_pattern_ne neqsel neqjoinsel ));



/*
 * function prototypes
 */
extern void OperatorCreate(const char *operatorName,
			   Oid operatorNamespace,
			   Oid leftTypeId,
			   Oid rightTypeId,
			   List *procedureName,
			   List *commutatorName,
			   List *negatorName,
			   List *restrictionName,
			   List *joinName,
			   bool canHash,
			   List *leftSortName,
			   List *rightSortName,
			   List *ltCompareName,
			   List *gtCompareName);

#endif   /* PG_OPERATOR_H */
