/*-------------------------------------------------------------------------
 *
 * pg_operator.h--
 *	  definition of the system "operator" relation (pg_operator)
 *	  along with the relation's initial contents.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_operator.h,v 1.18 1997/11/13 03:23:05 momjian Exp $
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

/* ----------------
 *		postgres.h contains the system type definintions and the
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
	NameData	oprname;
	Oid			oprowner;
	int2		oprprec;
	char		oprkind;
	bool		oprisleft;
	bool		oprcanhash;
	Oid			oprleft;
	Oid			oprright;
	Oid			oprresult;
	Oid			oprcom;
	Oid			oprnegate;
	Oid			oprlsortop;
	Oid			oprrsortop;
	regproc		oprcode;
	regproc		oprrest;
	regproc		oprjoin;
} FormData_pg_operator;

/* ----------------
 *		Form_pg_operator corresponds to a pointer to a tuple with
 *		the format of pg_operator relation.
 * ----------------
 */
typedef FormData_pg_operator *OperatorTupleForm;

/* ----------------
 *		compiler constants for pg_operator
 * ----------------
 */

#define Natts_pg_operator				16
#define Anum_pg_operator_oprname		1
#define Anum_pg_operator_oprowner		2
#define Anum_pg_operator_oprprec		3
#define Anum_pg_operator_oprkind		4
#define Anum_pg_operator_oprisleft		5
#define Anum_pg_operator_oprcanhash		6
#define Anum_pg_operator_oprleft		7
#define Anum_pg_operator_oprright		8
#define Anum_pg_operator_oprresult		9
#define Anum_pg_operator_oprcom			10
#define Anum_pg_operator_oprnegate		11
#define Anum_pg_operator_oprlsortop		12
#define Anum_pg_operator_oprrsortop		13
#define Anum_pg_operator_oprcode		14
#define Anum_pg_operator_oprrest		15
#define Anum_pg_operator_oprjoin		16

/* ----------------
 *		initial contents of pg_operator
 * ----------------
 */

DATA(insert OID = 58 (	"<"		   PGUID 0 b t f  16  16  16  85   91  0  0 boollt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 59 (	">"		   PGUID 0 b t f  16  16  16  85   91  0  0 boolgt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 85 (	"<>"	   PGUID 0 b t f  16  16  16  85   91  0  0 boolne neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 91 (	"="		   PGUID 0 b t t  16  16  16  91   85  0  0 booleq eqsel eqjoinsel ));
DESCR("");
#define BooleanEqualOperator   91

DATA(insert OID = 92 (	"="		   PGUID 0 b t t  18  18  16  92 630  631 631 chareq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 93 (	"="		   PGUID 0 b t t  19  19  16  93  643 660 660 nameeq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 94 (	"="		   PGUID 0 b t t  21  21  16  94 519 95 95 int2eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 95 (	"<"		   PGUID 0 b t f  21  21  16 520 524 0 0 int2lt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 96 (	"="		   PGUID 0 b t t  23  23  16  96 518 97 97 int4eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 97 (	"<"		   PGUID 0 b t f  23  23  16 521 525 0 0 int4lt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 98 (	"="		   PGUID 0 b t t  25  25  16  98 531 664 664 texteq eqsel eqjoinsel ));
DESCR("");

DATA(insert OID = 1267 (  "="		 PGUID 0 b t t	20	20	16	1267 644 645 645 char16eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 329 (  "="	   PGUID 0 b t t  1000	1000  16  329 0  0	0 array_eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 349 (  "="	   PGUID 0 b t t  1001	1001  16  349 0  0	0 array_eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 374 (  "="	   PGUID 0 b t t  1002	1002  16  374 0  0	0 array_eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 375 (  "="	   PGUID 0 b t t  1003	1003  16  375 0  0	0 array_eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 376 (  "="	   PGUID 0 b t t  1004	1004  16  376 0  0	0 array_eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 377 (  "="	   PGUID 0 b t t  1005	1005  16  377 0  0	0 array_eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 378 (  "="	   PGUID 0 b t t  1006	1006  16  378 0  0	0 array_eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 379 (  "="	   PGUID 0 b t t  1007	1007  16  379 0  0	0 array_eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 380 (  "="	   PGUID 0 b t t  1008	1008  16  380 0  0	0 array_eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 381 (  "="	   PGUID 0 b t t  1009	1009  16  381 0  0	0 array_eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 382 (  "="	   PGUID 0 b t t  1028	1028  16  382 0  0	0 array_eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 383 (  "="	   PGUID 0 b t t  1010	1010  16  383 0  0	0 array_eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 384 (  "="	   PGUID 0 b t t  1011	1011  16  384 0  0	0 array_eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 385 (  "="	   PGUID 0 b t t  1012	1012  16  385 0  0	0 array_eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 386 (  "="	   PGUID 0 b t t  1013	1013  16  386 0  0	0 array_eq eqsel eqjoinsel ));
DESCR("");
/*
DATA(insert OID = 387 (  "="	   PGUID 0 b t t  1014	1014  16  387 0  0	0 array_eq eqsel eqjoinsel ));
DESCR("");
*/
DATA(insert OID = 388 (  "="	   PGUID 0 b t t  1015	1015  16  388 0  0	0 array_eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 389 (  "="	   PGUID 0 b t t  1016	1016  16  389 0  0	0 array_eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 390 (  "="	   PGUID 0 b t t  1017	1017  16  390 0  0	0 array_eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 391 (  "="	   PGUID 0 b t t  1018	1018  16  391 0  0	0 array_eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 392 (  "="	   PGUID 0 b t t  1019	1019  16  392 0  0	0 array_eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 393 (  "="	   PGUID 0 b t t  1020	1020  16  393 0  0	0 array_eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 394 (  "="	   PGUID 0 b t t  1021	1021  16  394 0  0	0 array_eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 395 (  "="	   PGUID 0 b t t  1022	1022  16  395 0  0	0 array_eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 396 (  "="	   PGUID 0 b t t  1023	1023  16  396 0  0	0 array_eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 397 (  "="	   PGUID 0 b t t  1024	1024  16  397 0  0	0 array_eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 398 (  "="	   PGUID 0 b t t  1025	1025  16  398 0  0	0 array_eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 399 (  "="	   PGUID 0 b t t  1026	1026  16  399 0  0	0 array_eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 400 (  "="	   PGUID 0 b t t  1027	1027  16  400 0  0	0 array_eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 401 (  "="	   PGUID 0 b t t  1034	1034  16  401 0  0	0 array_eq eqsel eqjoinsel ));
DESCR("");

DATA(insert OID = 412 (  "="	   PGUID 0 b t t  409  409	16	412 415 418 418 char2eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 413 (  "="	   PGUID 0 b t t  410  410	16	413 416 419 419 char4eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 414 (  "="	   PGUID 0 b t t  411  411	16	414 417 420 420 char8eq eqsel eqjoinsel ));
DESCR("");

DATA(insert OID = 415 (  "<>"	   PGUID 0 b t f  409  409	16 415 412	0 0 char2ne neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 416 (  "<>"	   PGUID 0 b t f  410  410	16 416 413	0 0 char4ne neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 417 (  "<>"	   PGUID 0 b t f  411  411	16 417 414	0 0 char8ne neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 418 (  "<"	   PGUID 0 b t f  409  409	16 460 463	0 0 char2lt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 419 (  "<"	   PGUID 0 b t f  410  410	16 461 464	0 0 char4lt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 420 (  "<"	   PGUID 0 b t f  411  411	16 462 465	0 0 char8lt intltsel intltjoinsel ));
DESCR("");

DATA(insert OID = 457 (  "<="	   PGUID 0 b t f  409  409	16 463 460	0 0 char2le intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 458 (  "<="	   PGUID 0 b t f  410  410	16 464 461	0 0 char4le intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 459 (  "<="	   PGUID 0 b t f  411  411	16 465 462	0 0 char8le intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 460 (  ">"	   PGUID 0 b t f  409  409	16 418 457	0 0 char2gt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 461 (  ">"	   PGUID 0 b t f  410  410	16 419 458	0 0 char4gt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 462 (  ">"	   PGUID 0 b t f  411  411	16 420 459	0 0 char8gt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 463 (  ">="	   PGUID 0 b t f  409  409	16 457 418	0 0 char2ge intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 464 (  ">="	   PGUID 0 b t f  410  410	16 458 418	0 0 char4ge intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 465 (  ">="	   PGUID 0 b t f  411  411	16 459 420	0 0 char8ge intltsel intltjoinsel ));
DESCR("");

DATA(insert OID = 485 (  "<<"	   PGUID 0 b t f 604 604  16   0   0   0   0 poly_left intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 486 (  "&<"	   PGUID 0 b t f 604 604  16   0   0   0   0 poly_overleft intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 487 (  "&>"	   PGUID 0 b t f 604 604  16   0   0   0   0 poly_overright intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 488 (  ">>"	   PGUID 0 b t f 604 604  16   0   0   0   0 poly_right intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 489 (  "@"	   PGUID 0 b t f 604 604  16 490   0   0   0 poly_contained intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 490 (  "~"	   PGUID 0 b t f 604 604  16 489   0   0   0 poly_contain intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 491 (  "~="	   PGUID 0 b t f 604 604  16 491   0   0   0 poly_same intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 492 (  "&&"	   PGUID 0 b t f 604 604  16   0   0   0   0 poly_overlap intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 493 (  "<<"	   PGUID 0 b t f 603 603  16   0   0   0   0 box_left intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 494 (  "&<"	   PGUID 0 b t f 603 603  16   0   0   0   0 box_overleft intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 495 (  "&>"	   PGUID 0 b t f 603 603  16   0   0   0   0 box_overright intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 496 (  ">>"	   PGUID 0 b t f 603 603  16   0   0   0   0 box_right intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 497 (  "@"	   PGUID 0 b t f 603 603  16 498   0   0   0 box_contained intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 498 (  "~"	   PGUID 0 b t f 603 603  16 497   0   0   0 box_contain intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 499 (  "~="	   PGUID 0 b t f 603 603  16 499   0   0   0 box_same intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 500 (  "&&"	   PGUID 0 b t f 603 603  16   0   0   0   0 box_overlap intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 501 (  ">="	   PGUID 0 b t f 603 603  16 505 504   0   0 box_ge areasel areajoinsel ));
DESCR("");
DATA(insert OID = 502 (  ">"	   PGUID 0 b t f 603 603  16 504 505   0   0 box_gt areasel areajoinsel ));
DESCR("");
DATA(insert OID = 503 (  "="	   PGUID 0 b t t 603 603  16 503   0   0   0 box_eq areasel areajoinsel ));
DESCR("");
DATA(insert OID = 504 (  "<"	   PGUID 0 b t f 603 603  16 502 501   0   0 box_lt areasel areajoinsel ));
DESCR("");
DATA(insert OID = 505 (  "<="	   PGUID 0 b t f 603 603  16 501 502   0   0 box_le areasel areajoinsel ));
DESCR("");
DATA(insert OID = 506 (  ">^"	   PGUID 0 b t f 600 600  16   0   0   0   0 point_above intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 507 (  "<<"	   PGUID 0 b t f 600 600  16   0   0   0   0 point_left intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 508 (  ">>"	   PGUID 0 b t f 600 600  16   0   0   0   0 point_right intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 509 (  "<^"	   PGUID 0 b t f 600 600  16   0   0   0   0 point_below intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 510 (  "~="	   PGUID 0 b t f 600 600  16 510   0   0   0 point_eq intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 511 (  "@"	   PGUID 0 b t f 600 603  16   0   0   0   0 on_pb intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 512 (  "@"	   PGUID 0 b t f 600 602  16   0   0   0   0 on_ppath intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 513 (  "@@"	   PGUID 0 l t f   0 603 600   0   0   0   0 box_center intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 514 (  "*"	   PGUID 0 b t f  23  23  23 514   0   0   0 int4mul intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 515 (  "!"	   PGUID 0 r t f  23   0  23   0   0   0   0 int4fac intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 516 (  "!!"	   PGUID 0 l t f   0  23  23   0   0   0   0 int4fac intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 517 (  "<->"	   PGUID 0 b t f 600 600 701 517   0   0   0 point_distance intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 518 (  "<>"	   PGUID 0 b t f  23  23  16 518  96  0  0 int4ne neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 519 (  "<>"	   PGUID 0 b t f  21  21  16 519  94  0  0 int2ne neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 520 (  ">"	   PGUID 0 b t f  21  21  16  95   0  0  0 int2gt intgtsel intgtjoinsel ));
DESCR("");
DATA(insert OID = 521 (  ">"	   PGUID 0 b t f  23  23  16  97   0  0  0 int4gt intgtsel intgtjoinsel ));
DESCR("");
DATA(insert OID = 522 (  "<="	   PGUID 0 b t f  21  21  16 524 520  0  0 int2le intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 523 (  "<="	   PGUID 0 b t f  23  23  16 525 521  0  0 int4le intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 524 (  ">="	   PGUID 0 b t f  21  21  16 522  95  0  0 int2ge intgtsel intgtjoinsel ));
DESCR("");
DATA(insert OID = 525 (  ">="	   PGUID 0 b t f  23  23  16 523  97  0  0 int4ge intgtsel intgtjoinsel ));
DESCR("");
DATA(insert OID = 526 (  "*"	   PGUID 0 b t f  21  21  21 526   0  0  0 int2mul intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 527 (  "/"	   PGUID 0 b t f  21  21  21   0   0  0  0 int2div intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 528 (  "/"	   PGUID 0 b t f  23  23  23   0   0  0  0 int4div intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 529 (  "%"	   PGUID 0 b t f  21  21  21   6   0  0  0 int2mod intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 530 (  "%"	   PGUID 0 b t f  23  23  23   6   0  0  0 int4mod intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 531 (  "<>"	   PGUID 0 b t f  25  25  16 531  98   0   0 textne neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 532 (  "="	   PGUID 0 b t t  21  23  16 533 538  95  97 int24eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 533 (  "="	   PGUID 0 b t t  23  21  16 532 539  97  95 int42eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 534 (  "<"	   PGUID 0 b t f  21  23  16 537 542  0  0 int24lt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 535 (  "<"	   PGUID 0 b t f  23  21  16 536 543  0  0 int42lt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 536 (  ">"	   PGUID 0 b t f  21  23  16 535 540  0  0 int24gt intgtsel intgtjoinsel ));
DESCR("");
DATA(insert OID = 537 (  ">"	   PGUID 0 b t f  23  21  16 534 541  0  0 int42gt intgtsel intgtjoinsel ));
DESCR("");
DATA(insert OID = 538 (  "<>"	   PGUID 0 b t f  21  23  16 539 532  0  0 int24ne neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 539 (  "<>"	   PGUID 0 b t f  23  21  16 538 533  0  0 int42ne neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 540 (  "<="	   PGUID 0 b t f  21  23  16 543 536  0  0 int24le intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 541 (  "<="	   PGUID 0 b t f  23  21  16 542 537  0  0 int42le intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 542 (  ">="	   PGUID 0 b t f  21  23  16 541 534  0  0 int24ge intgtsel intgtjoinsel ));
DESCR("");
DATA(insert OID = 543 (  ">="	   PGUID 0 b t f  23  21  16 540 535  0  0 int42ge intgtsel intgtjoinsel ));
DESCR("");
DATA(insert OID = 544 (  "*"	   PGUID 0 b t f  21  23  23 545   0  0  0 int24mul intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 545 (  "*"	   PGUID 0 b t f  23  21  23 544   0  0  0 int42mul intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 546 (  "/"	   PGUID 0 b t f  21  23  23   0   0  0  0 int24div intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 547 (  "/"	   PGUID 0 b t f  23  21  23   0   0  0  0 int42div intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 548 (  "%"	   PGUID 0 b t f  21  23  23   6   0  0  0 int24mod intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 549 (  "%"	   PGUID 0 b t f  23  21  23   6   0  0  0 int42mod intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 550 (  "+"	   PGUID 0 b t f  21  21  21 550   0   0   0 int2pl intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 551 (  "+"	   PGUID 0 b t f  23  23  23 551   0   0   0 int4pl intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 552 (  "+"	   PGUID 0 b t f  21  23  23 553   0   0   0 int24pl intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 553 (  "+"	   PGUID 0 b t f  23  21  23 552   0   0   0 int42pl intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 554 (  "-"	   PGUID 0 b t f  21  21  21   0   0   0   0 int2mi intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 555 (  "-"	   PGUID 0 b t f  23  23  23   0   0   0   0 int4mi intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 556 (  "-"	   PGUID 0 b t f  21  23  23   0   0   0   0 int24mi intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 557 (  "-"	   PGUID 0 b t f  23  21  23   0   0   0   0 int42mi intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 558 (  "-"	   PGUID 0 l t f   0  23  23   0   0   0   0 int4um intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 559 (  "-"	   PGUID 0 l t f   0  21  21   0   0   0   0 int2um intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 560 (  "="	   PGUID 0 b t t 702 702  16 560 561 562 562 abstimeeq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 561 (  "<>"	   PGUID 0 b t f 702 702  16 561 560 0 0 abstimene neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 562 (  "<"	   PGUID 0 b t f 702 702  16 563 565 0 0 abstimelt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 563 (  ">"	   PGUID 0 b t f 702 702  16 562 564 0 0 abstimegt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 564 (  "<="	   PGUID 0 b t f 702 702  16 565 563 0 0 abstimele intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 565 (  ">="	   PGUID 0 b t f 702 702  16 564 562 0 0 abstimege intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 566 (  "="	   PGUID 0 b t t 703 703  16 566 567 568 568 reltimeeq - - ));
DESCR("");
DATA(insert OID = 567 (  "<>"	   PGUID 0 b t f 703 703  16 567 566 0 0 reltimene - - ));
DESCR("");
DATA(insert OID = 568 (  "<"	   PGUID 0 b t f 703 703  16 569 571 0 0 reltimelt - - ));
DESCR("");
DATA(insert OID = 569 (  ">"	   PGUID 0 b t f 703 703  16 568 570 0 0 reltimegt - - ));
DESCR("");
DATA(insert OID = 570 (  "<="	   PGUID 0 b t f 703 703  16 571 569 0 0 reltimele - - ));
DESCR("");
DATA(insert OID = 571 (  ">="	   PGUID 0 b t f 703 703  16 570 568 0 0 reltimege - - ));
DESCR("");
DATA(insert OID = 572 (  "~="	   PGUID 0 b t t 704 704  16 572   0   0   0 intervalsame - - ));
DESCR("");
DATA(insert OID = 573 (  "<<"	   PGUID 0 b t f 704 704  16   0   0   0   0 intervalct - - ));
DESCR("");
DATA(insert OID = 574 (  "&&"	   PGUID 0 b t f 704 704  16   0   0   0   0 intervalov - - ));
DESCR("");
DATA(insert OID = 575 (  "#="	   PGUID 0 b t f 704 703  16   0 576   0 568 intervalleneq - - ));
DESCR("");
DATA(insert OID = 576 (  "#<>"	   PGUID 0 b t f 704 703  16   0 575   0 568 intervallenne - - ));
DESCR("");
DATA(insert OID = 577 (  "#<"	   PGUID 0 b t f 704 703  16   0 580   0 568 intervallenlt - - ));
DESCR("");
DATA(insert OID = 578 (  "#>"	   PGUID 0 b t f 704 703  16   0 579   0 568 intervallengt - - ));
DESCR("");
DATA(insert OID = 579 (  "#<="	   PGUID 0 b t f 704 703  16   0 578   0 568 intervallenle - - ));
DESCR("");
DATA(insert OID = 580 (  "#>="	   PGUID 0 b t f 704 703  16   0 577   0 568 intervallenge - - ));
DESCR("");
DATA(insert OID = 581 (  "+"	   PGUID 0 b t f 702 703 702 581   0 0 0 timepl - - ));
DESCR("");
DATA(insert OID = 582 (  "-"	   PGUID 0 b t f 702 703 702   0   0 0 0 timemi - - ));
DESCR("");
DATA(insert OID = 583 (  "<?>"	   PGUID 0 b t f 702 704  16   0   0 562   0 ininterval - - ));
DESCR("");
DATA(insert OID = 584 (  "-"	   PGUID 0 l t f   0 700 700   0   0   0   0 float4um - - ));
DESCR("");
DATA(insert OID = 585 (  "-"	   PGUID 0 l t f   0 701 701   0   0   0   0 float8um - - ));
DESCR("");
DATA(insert OID = 586 (  "+"	   PGUID 0 b t f 700 700 700 586   0   0   0 float4pl - - ));
DESCR("");
DATA(insert OID = 587 (  "-"	   PGUID 0 b t f 700 700 700   0   0   0   0 float4mi - - ));
DESCR("");
DATA(insert OID = 588 (  "/"	   PGUID 0 b t f 700 700 700   0   0   0   0 float4div - - ));
DESCR("");
DATA(insert OID = 589 (  "*"	   PGUID 0 b t f 700 700 700 589   0   0   0 float4mul - - ));
DESCR("");
DATA(insert OID = 590 (  "@"	   PGUID 0 l t f   0 700 700   0   0   0   0 float4abs - - ));
DESCR("");
DATA(insert OID = 591 (  "+"	   PGUID 0 b t f 701 701 701 591   0   0   0 float8pl - - ));
DESCR("");
DATA(insert OID = 592 (  "-"	   PGUID 0 b t f 701 701 701   0   0   0   0 float8mi - - ));
DESCR("");
DATA(insert OID = 593 (  "/"	   PGUID 0 b t f 701 701 701   0   0   0   0 float8div - - ));
DESCR("");
DATA(insert OID = 594 (  "*"	   PGUID 0 b t f 701 701 701 594   0   0   0 float8mul - - ));
DESCR("");
DATA(insert OID = 595 (  "@"	   PGUID 0 l t f   0 701 701   0   0   0   0 float8abs - - ));
DESCR("");
DATA(insert OID = 596 (  "|/"	   PGUID 0 l t f   0 701 701   0   0   0   0 dsqrt - - ));
DESCR("");
DATA(insert OID = 597 (  "||/"	   PGUID 0 l t f   0 701 701   0   0   0   0 dcbrt - - ));
DESCR("");
DATA(insert OID = 598 (  "%"	   PGUID 0 l t f   0 701 701   0   0   0   0 dtrunc - - ));
DESCR("");
DATA(insert OID = 599 (  "%"	   PGUID 0 r t f 701   0 701   0   0   0   0 dround - - ));
DESCR("");
DATA(insert OID = 1282 (  ":"		PGUID 0 l t f	0 701 701	0	0	0	0 dexp - - ));
DESCR("");
DATA(insert OID = 1283 (  ";"		PGUID 0 l t f	0 701 701	0	0	0	0 dlog1 - - ));
DESCR("");
DATA(insert OID = 1284 (  "|"		PGUID 0 l t f	0 704 702	0	0	0	0 intervalstart - - ));
DESCR("");
DATA(insert OID = 606 (  "<#>"		PGUID 0 b t f 702 702 704	0	0	0	0 mktinterval - - ));
DESCR("");
DATA(insert OID = 607 (  "="	   PGUID 0 b t t  26  26  16 607 608 97 97 oideq eqsel eqjoinsel ));
DESCR("");
#define OIDEqualOperator 607	/* XXX planner/prep/semanopt.c crock */
DATA(insert OID = 608 (  "<>"	   PGUID 0 b t f  26  26  16 608 607  0  0 oidne neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 609 (  "<"	   PGUID 0 b t f  26  26  16 610 612  0  0 int4lt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 610 (  ">"	   PGUID 0 b t f  26  26  16 609 611  0  0 int4gt intgtsel intgtjoinsel ));
DESCR("");
DATA(insert OID = 611 (  "<="	   PGUID 0 b t f  26  26  16 612 610  0  0 int4le intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 612 (  ">="	   PGUID 0 b t f  26  26  16 611 609  0  0 int4ge intgtsel intgtjoinsel ));
DESCR("");

DATA(insert OID = 613 (  "<->"	   PGUID 0 b t f 600 603 701   0   0  0  0 dist_pl - - ));
DESCR("");
DATA(insert OID = 614 (  "<->"	   PGUID 0 b t f 600 601 701   0   0  0  0 dist_ps - - ));
DESCR("");
DATA(insert OID = 615 (  "<->"	   PGUID 0 b t f 600 603 701   0   0  0  0 dist_pb - - ));
DESCR("");
DATA(insert OID = 616 (  "<->"	   PGUID 0 b t f 601 628 701   0   0  0  0 dist_sl - - ));
DESCR("");
DATA(insert OID = 617 (  "<->"	   PGUID 0 b t f 601 603 701   0   0  0  0 dist_sb - - ));
DESCR("");
DATA(insert OID = 618 (  "<->"	   PGUID 0 b t f 600 602 701   0   0  0  0 dist_ppath - - ));
DESCR("");

DATA(insert OID = 619 (  "<"	   PGUID 0 b t f 704 704  16   0   0  0  0 intervalct - - ));
DESCR("");

DATA(insert OID = 620 (  "="	   PGUID 0 b t t  700  700	16 620 621	622 622 float4eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 621 (  "<>"	   PGUID 0 b t f  700  700	16 621 620	0 0 float4ne neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 622 (  "<"	   PGUID 0 b t f  700  700	16 623 625	0 0 float4lt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 623 (  ">"	   PGUID 0 b t f  700  700	16 622 624	0 0 float4gt intgtsel intgtjoinsel ));
DESCR("");
DATA(insert OID = 624 (  "<="	   PGUID 0 b t f  700  700	16 625 623	0 0 float4le intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 625 (  ">="	   PGUID 0 b t f  700  700	16 624 622	0 0 float4ge intgtsel intgtjoinsel ));
DESCR("");
DATA(insert OID = 626 (  "!!="	   PGUID 0 b t f  23   19	16 0   0	0	0	int4notin "-"	  "-"));
DESCR("");
DATA(insert OID = 627 (  "!!="	   PGUID 0 b t f  26   19	16 0   0	0	0	oidnotin "-"	 "-"));
DESCR("");
#define OIDNotInOperator 627	/* XXX planner/prep/semanopt.c crock */
DATA(insert OID = 630 (  "<>"	   PGUID 0 b t f  18  18  16 630  92  0 0 charne neqsel neqjoinsel ));
DESCR("");

DATA(insert OID = 631 (  "<"	   PGUID 0 b t f  18  18  16 633 634  0 0 charlt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 632 (  "<="	   PGUID 0 b t f  18  18  16 634 633  0 0 charle intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 633 (  ">"	   PGUID 0 b t f  18  18  16 631 632  0 0 chargt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 634 (  ">="	   PGUID 0 b t f  18  18  16 632 631  0 0 charge intltsel intltjoinsel ));
DESCR("");

DATA(insert OID = 635 (  "+"	   PGUID 0 b t f  18  18  18 0 0  0 0 charpl eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 636 (  "-"	   PGUID 0 b t f  18  18  18 0 0  0 0 charmi eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 637 (  "*"	   PGUID 0 b t f  18  18  18 0 0  0 0 charmul eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 638 (  "/"	   PGUID 0 b t f  18  18  18 0 0  0 0 chardiv eqsel eqjoinsel ));
DESCR("");

DATA(insert OID = 639 (  "~"	   PGUID 0 b t f  19  25  16 0 640	0 0 nameregexeq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 640 (  "!~"	   PGUID 0 b t f  19  25  16 0 639	0 0 nameregexne neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 641 (  "~"	   PGUID 0 b t f  25  25  16 0 642	0 0 textregexeq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 642 (  "!~"	   PGUID 0 b t f  25  25  16 0 641	0 0 textregexne eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 643 (  "<>"	   PGUID 0 b t f  19  19  16 643 93 0 0 namene neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 644 (  "<>"	   PGUID 0 b t f  20  20  16 644 1267 0 0 char16ne neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 645 (  "<"	   PGUID 0 b t f  20  20  16 647 648  0 0 char16lt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 646 (  "<="	   PGUID 0 b t f  20  20  16 648 647  0 0 char16le intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 647 (  ">"	   PGUID 0 b t f  20  20  16 645 646  0 0 char16gt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 648 (  ">="	   PGUID 0 b t f  20  20  16 646 645  0 0 char16ge intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 649 (  "~"	   PGUID 0 b t f  20  25  16 0 650	0 0 char16regexeq intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 650 (  "!~"	   PGUID 0 b t f  20  25  16 650 0	0 0 char16regexne intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 651 (  "~~"	   PGUID 0 b t f  20  25  16 0 651	0 0 char16like eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 652 (  "!~~"	   PGUID 0 b t f  20  25  16 651 0	0 0 char16nlike neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 654 (  "||"	   PGUID 0 b t f  25  25  25   0 0	0 0 textcat - - ));
DESCR("");

DATA(insert OID = 660 (  "<"	   PGUID 0 b t f  19  19  16 662 663  0 0 namelt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 661 (  "<="	   PGUID 0 b t f  19  19  16 663 662  0 0 namele intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 662 (  ">"	   PGUID 0 b t f  19  19  16 660 661  0 0 namegt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 663 (  ">="	   PGUID 0 b t f  19  19  16 661 660  0 0 namege intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 664 (  "<"	   PGUID 0 b t f  25  25  16 666 667  0 0 text_lt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 665 (  "<="	   PGUID 0 b t f  25  25  16 667 666  0 0 text_le intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 666 (  ">"	   PGUID 0 b t f  25  25  16 664 665  0 0 text_gt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 667 (  ">="	   PGUID 0 b t f  25  25  16 665 664  0 0 text_ge intltsel intltjoinsel ));
DESCR("");

DATA(insert OID = 670 (  "="	   PGUID 0 b t f  701  701	16 670 671	0 0 float8eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 671 (  "<>"	   PGUID 0 b t f  701  701	16 671 670	0 0 float8ne neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 672 (  "<"	   PGUID 0 b t f  701  701	16 674 675	0 0 float8lt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 673 (  "<="	   PGUID 0 b t f  701  701	16 675 674	0 0 float8le intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 674 (  ">"	   PGUID 0 b t f  701  701	16 672 673	0 0 float8gt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 675 (  ">="	   PGUID 0 b t f  701  701	16 673 672	0 0 float8ge intltsel intltjoinsel ));
DESCR("");

DATA(insert OID = 676 (  "<"	   PGUID 0 b t f  911  911	16 680 679	0 0 oidnamelt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 677 (  "<="	   PGUID 0 b t f  911  911	16 679 680	0 0 oidnamele intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 678 (  "="	   PGUID 0 b t f  911  911	16 678 681	0 0 oidnameeq intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 679 (  ">="	   PGUID 0 b t f  911  911	16 677 676	0 0 oidnamege intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 680 (  ">"	   PGUID 0 b t f  911  911	16 676 677	0 0 oidnamegt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 681 (  "<>"	   PGUID 0 b t f  911  911	16 681 678	0 0 oidnamene intltsel intltjoinsel ));
DESCR("");

DATA(insert OID = 697 (  "~"	   PGUID 0 b t f  411  25  16 0 698  0 0 char8regexeq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 698 (  "!~"	   PGUID 0 b t f  411  25  16 0 697  0 0 char8regexne neqsel neqjoinsel ));
DESCR("");

DATA(insert OID = 706 (  "<->"	   PGUID 0 b t f 603 603 701 706   0  0  0 box_distance intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 707 (  "<->"	   PGUID 0 b t f 602 602 701 707   0  0  0 path_distance intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 708 (  "<->"	   PGUID 0 b t f 628 628 701 708   0  0  0 line_distance intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 709 (  "<->"	   PGUID 0 b t f 601 601 701 709   0  0  0 lseg_distance intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 712 (  "<->"	   PGUID 0 b t f 604 604 701 712   0  0  0 poly_distance intltsel intltjoinsel ));
DESCR("");

/* add translation/rotation/scaling operators for geometric types. - thomas 97/05/10 */
DATA(insert OID = 731 (  "+"	   PGUID 0 b t f  600  600	600  731  0 0 0 point_add - - ));
DESCR("");
DATA(insert OID = 732 (  "-"	   PGUID 0 b t f  600  600	600    0  0 0 0 point_sub - - ));
DESCR("");
DATA(insert OID = 733 (  "*"	   PGUID 0 b t f  600  600	600  733  0 0 0 point_mul - - ));
DESCR("");
DATA(insert OID = 734 (  "/"	   PGUID 0 b t f  600  600	600    0  0 0 0 point_div - - ));
DESCR("");
DATA(insert OID = 735 (  "+"	   PGUID 0 b t f  602  602	602  735  0 0 0 path_add - - ));
DESCR("");
DATA(insert OID = 736 (  "+"	   PGUID 0 b t f  602  600	602  736  0 0 0 path_add_pt - - ));
DESCR("");
DATA(insert OID = 737 (  "-"	   PGUID 0 b t f  602  600	602    0  0 0 0 path_sub_pt - - ));
DESCR("");
DATA(insert OID = 738 (  "*"	   PGUID 0 b t f  602  600	602  738  0 0 0 path_mul_pt - - ));
DESCR("");
DATA(insert OID = 739 (  "/"	   PGUID 0 b t f  602  600	602    0  0 0 0 path_div_pt - - ));
DESCR("");
DATA(insert OID = 754 (  "@"	   PGUID 0 b t f  600  602	 16  755  0 0 0 pt_contained_path - - ));
DESCR("");
DATA(insert OID = 755 (  "~"	   PGUID 0 b t f  602  600	 16  754  0 0 0 path_contain_pt - - ));
DESCR("");
DATA(insert OID = 756 (  "@"	   PGUID 0 b t f  600  604	 16  757  0 0 0 pt_contained_poly - - ));
DESCR("");
DATA(insert OID = 757 (  "~"	   PGUID 0 b t f  604  600	 16  756  0 0 0 poly_contain_pt - - ));
DESCR("");
DATA(insert OID = 758 (  "@"	   PGUID 0 b t f  600  718	 16  759  0 0 0 pt_contained_circle - - ));
DESCR("");
DATA(insert OID = 759 (  "~"	   PGUID 0 b t f  718  600	 16  758  0 0 0 circle_contain_pt - - ));
DESCR("");

/* additional operators for geometric types - thomas 1997-07-09 */
DATA(insert OID =  792 (  "="	   PGUID 0 b t f  602  602	701  792  0 0 0 path_n_eq intltsel intltjoinsel ));
DESCR("");
DATA(insert OID =  793 (  "<"	   PGUID 0 b t f  602  602	701  796  0 0 0 path_n_lt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID =  794 (  ">"	   PGUID 0 b t f  602  602	701  795  0 0 0 path_n_gt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID =  795 (  "<="	   PGUID 0 b t f  602  602	701  794  0 0 0 path_n_le intltsel intltjoinsel ));
DESCR("");
DATA(insert OID =  796 (  ">="	   PGUID 0 b t f  602  602	701  793  0 0 0 path_n_ge intltsel intltjoinsel ));
DESCR("");
DATA(insert OID =  797 (  "#"	   PGUID 0 l t f	0  602	 23    0  0 0 0 path_npoints - - ));
DESCR("");
DATA(insert OID =  798 (  "?#"	   PGUID 0 b t f  602  602	 16    0  0 0 0 path_inter - - ));
DESCR("");
DATA(insert OID =  799 (  "??"	   PGUID 0 l t f	0  602	701    0  0 0 0 path_length - - ));
DESCR("");
DATA(insert OID =  800 (  ">^"	   PGUID 0 b t f  603  603	 16    0  0 0 0 box_above intltsel intltjoinsel ));
DESCR("");
DATA(insert OID =  801 (  "<^"	   PGUID 0 b t f  603  603	 16    0  0 0 0 box_below intltsel intltjoinsel ));
DESCR("");
DATA(insert OID =  802 (  "?#"	   PGUID 0 b t f  603  603	 16    0  0 0 0 box_overlap - - ));
DESCR("");
DATA(insert OID =  803 (  "#"	   PGUID 0 b t f  603  603	603    0  0 0 0 box_intersect - - ));
DESCR("");
DATA(insert OID =  804 (  "+"	   PGUID 0 b t f  603  600	603  804  0 0 0 box_add - - ));
DESCR("");
DATA(insert OID =  805 (  "-"	   PGUID 0 b t f  603  600	603    0  0 0 0 box_sub - - ));
DESCR("");
DATA(insert OID =  806 (  "*"	   PGUID 0 b t f  603  600	603  806  0 0 0 box_mul - - ));
DESCR("");
DATA(insert OID =  807 (  "/"	   PGUID 0 b t f  603  600	603    0  0 0 0 box_div - - ));
DESCR("");
DATA(insert OID =  808 (  "?-"	   PGUID 0 b t f  600  600	 16  808  0 0 0 point_horiz - - ));
DESCR("");
DATA(insert OID =  809 (  "?|"	   PGUID 0 b t f  600  600	 16  809  0 0 0 point_vert - - ));
DESCR("");

DATA(insert OID = 811 (  "="	   PGUID 0 b t t 704 704  16 811   0   0   0 intervaleq - - ));
DESCR("");
DATA(insert OID = 812 (  "<>"	   PGUID 0 b t t 704 704  16 812   0   0   0 intervalne - - ));
DESCR("");
DATA(insert OID = 813 (  "<"	   PGUID 0 b t t 704 704  16 813   0   0   0 intervallt - - ));
DESCR("");
DATA(insert OID = 814 (  ">"	   PGUID 0 b t t 704 704  16 814   0   0   0 intervalgt - - ));
DESCR("");
DATA(insert OID = 815 (  "<="	   PGUID 0 b t t 704 704  16 815   0   0   0 intervalle - - ));
DESCR("");
DATA(insert OID = 816 (  ">="	   PGUID 0 b t t 704 704  16 816   0   0   0 intervalge - - ));
DESCR("");

DATA(insert OID = 830 (  "<"	   PGUID 0 b t f  810  810	16 834 833	0 0 oidint2lt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 831 (  "<="	   PGUID 0 b t f  810  810	16 833 834	0 0 oidint2le intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 832 (  "="	   PGUID 0 b t f  810  810	16 832 835	0 0 oidint2eq intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 833 (  ">="	   PGUID 0 b t f  810  810	16 831 830	0 0 oidint2ge intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 834 (  ">"	   PGUID 0 b t f  810  810	16 830 831	0 0 oidint2gt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 835 (  "<>"	   PGUID 0 b t f  810  810	16 835 832	0 0 oidint2ne intltsel intltjoinsel ));
DESCR("");

DATA(insert OID = 839 (  "~"	   PGUID 0 b t f  409  25  16 0 841  0 0 char2regexeq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 841 (  "!~"	   PGUID 0 b t f  409  25  16 0 839  0 0 char2regexne neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 840 (  "~"	   PGUID 0 b t f  410  25  16 0 842  0 0 char4regexeq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 842 (  "!~"	   PGUID 0 b t f  410  25  16 0 840  0 0 char4regexne neqsel neqjoinsel ));
DESCR("");

DATA(insert OID = 843 (  "*"	   PGUID 0 b t f  790  700	790 845   0   0   0 cash_mul_flt4 - - ));
DESCR("");
DATA(insert OID = 844 (  "/"	   PGUID 0 b t f  790  700	790   0   0   0   0 cash_div_flt4 - - ));
DESCR("");
DATA(insert OID = 845 (  "*"	   PGUID 0 b t f  700  790	790 843   0   0   0 flt4_mul_cash - - ));
DESCR("");

DATA(insert OID = 900 (  "="	   PGUID 0 b t t  790  790	16 900 901	902 902 cash_eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 901 (  "<>"	   PGUID 0 b t f  790  790	16 901 900	0 0 cash_ne neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 902 (  "<"	   PGUID 0 b t f  790  790	16 903 905	0 0 cash_lt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 903 (  ">"	   PGUID 0 b t f  790  790	16 902 904	0 0 cash_gt intgtsel intgtjoinsel ));
DESCR("");
DATA(insert OID = 904 (  "<="	   PGUID 0 b t f  790  790	16 905 903	0 0 cash_le intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 905 (  ">="	   PGUID 0 b t f  790  790	16 904 902	0 0 cash_ge intgtsel intgtjoinsel ));
DESCR("");
DATA(insert OID = 906 (  "+"	   PGUID 0 b t f  790  790	790 906   0   0   0 cash_pl - - ));
DESCR("");
DATA(insert OID = 907 (  "-"	   PGUID 0 b t f  790  790	790   0   0   0   0 cash_mi - - ));
DESCR("");
DATA(insert OID = 908 (  "*"	   PGUID 0 b t f  790  701	790 916   0   0   0 cash_mul_flt8 - - ));
DESCR("");
DATA(insert OID = 909 (  "/"	   PGUID 0 b t f  790  701	790   0   0   0   0 cash_div_flt8 - - ));
DESCR("");
DATA(insert OID = 912 (  "*"	   PGUID 0 b t f  790  23	790 917   0   0   0 cash_mul_int4 - - ));
DESCR("");
DATA(insert OID = 913 (  "/"	   PGUID 0 b t f  790  23	790   0   0   0   0 cash_div_int4 - - ));
DESCR("");
DATA(insert OID = 914 (  "*"	   PGUID 0 b t f  790  21	790 918   0   0   0 cash_mul_int2 - - ));
DESCR("");
DATA(insert OID = 915 (  "/"	   PGUID 0 b t f  790  21	790   0   0   0   0 cash_div_int2 - - ));
DESCR("");
DATA(insert OID = 916 (  "*"	   PGUID 0 b t f  701  790	790 908   0   0   0 flt8_mul_cash - - ));
DESCR("");
DATA(insert OID = 917 (  "*"	   PGUID 0 b t f  23  790	790 912   0   0   0 int4_mul_cash - - ));
DESCR("");
DATA(insert OID = 918 (  "*"	   PGUID 0 b t f  21  790	790 914   0   0   0 int2_mul_cash - - ));
DESCR("");

DATA(insert OID = 930 (  "<"	   PGUID 0 b t f  910  910	16 934 933	0 0 oidint4lt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 931 (  "<="	   PGUID 0 b t f  910  910	16 933 934	0 0 oidint4le intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 932 (  "="	   PGUID 0 b t f  910  910	16 932 935	0 0 oidint4eq intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 933 (  ">="	   PGUID 0 b t f  910  910	16 931 930	0 0 oidint4ge intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 934 (  ">"	   PGUID 0 b t f  910  910	16 930 931	0 0 oidint4gt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 935 (  "<>"	   PGUID 0 b t f  910  910	16 935 932	0 0 oidint4ne intltsel intltjoinsel ));
DESCR("");

DATA(insert OID = 965 (  "^"	   PGUID 0 b t f  701  701	701 0 0 0 0 dpow - - ));
DESCR("");
DATA(insert OID = 966 (  "+"	   PGUID 0 b t f 1034 1033 1034 0 0 0 0 aclinsert	intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 967 (  "-"	   PGUID 0 b t f 1034 1033 1034 0 0 0 0 aclremove	intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 968 (  "~"	   PGUID 0 b t f 1034 1033	 16 0 0 0 0 aclcontains intltsel intltjoinsel ));
DESCR("");

/* additional geometric operators - thomas 1997-07-09 */
DATA(insert OID =  969 (  "@@"	   PGUID 0 l t f	0  601	600    0  0 0 0 lseg_center - - ));
DESCR("");
DATA(insert OID =  970 (  "@@"	   PGUID 0 l t f	0  602	600    0  0 0 0 path_center - - ));
DESCR("");
DATA(insert OID =  971 (  "@@"	   PGUID 0 l t f	0  604	600    0  0 0 0 poly_center - - ));
DESCR("");

DATA(insert OID =  974 (  "||"	   PGUID 0 b t f 1042 1042 1042    0  0 0 0 textcat - - ));
DESCR("");
DATA(insert OID =  979 (  "||"	   PGUID 0 b t f 1043 1043 1043    0  0 0 0 textcat - - ));
DESCR("");

DATA(insert OID = 1054 ( "="	   PGUID 0 b t t 1042 1042	 16 1054 1057 1058 1058 bpchareq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 1055 (  "~"	   PGUID 0 b t f 1042	25	 16    0 1056  0 0 textregexeq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 1056 ( "!~"	   PGUID 0 b t f 1042	25	 16    0 1055  0 0 textregexne neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 1057 ( "<>"	   PGUID 0 b t f 1042 1042	 16 1057 1054  0 0 bpcharne neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 1058 ( "<"	   PGUID 0 b t f 1042 1042	 16 1060 1061  0 0 bpcharlt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1059 ( "<="	   PGUID 0 b t f 1042 1042	 16 1061 1060  0 0 bpcharle intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1060 ( ">"	   PGUID 0 b t f 1042 1042	 16 1058 1059  0 0 bpchargt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1061 ( ">="	   PGUID 0 b t f 1042 1042	 16 1059 1058  0 0 bpcharge intltsel intltjoinsel ));
DESCR("");

DATA(insert OID = 1062 ( "="	   PGUID 0 b t t 1043 1043	16	1062 1065 1066 1066 varchareq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 1063 (  "~"	   PGUID 0 b t f 1043	25	16 0 1064  0 0 textregexeq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 1064 ( "!~"	   PGUID 0 b t f 1043	25	16 0 1063  0 0 textregexne neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 1065 ( "<>"	   PGUID 0 b t f 1043 1043	16 1065 1062  0 0 varcharne neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 1066 ( "<"	   PGUID 0 b t f 1043 1043	16 1068 1069  0 0 varcharlt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1067 ( "<="	   PGUID 0 b t f 1043 1043	16 1069 1068  0 0 varcharle intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1068 ( ">"	   PGUID 0 b t f 1043 1043	16 1066 1067  0 0 varchargt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1069 ( ">="	   PGUID 0 b t f 1043 1043	16 1067 1066  0 0 varcharge intltsel intltjoinsel ));
DESCR("");

/* date operators */
DATA(insert OID = 1093 ( "="	   PGUID 0 b t t  1082	1082   16 1093 1094 1095 1095 date_eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 1094 ( "<>"	   PGUID 0 b t f  1082	1082   16 1094 1093  0 0 date_ne neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 1095 ( "<"	   PGUID 0 b t f  1082	1082   16 1097 1098  0 0 date_lt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1096 ( "<="	   PGUID 0 b t f  1082	1082   16 1098 1097  0 0 date_le intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1097 ( ">"	   PGUID 0 b t f  1082	1082   16 1095 1096  0 0 date_gt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1098 ( ">="	   PGUID 0 b t f  1082	1082   16 1096 1065  0 0 date_ge intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1099 ( "-"	   PGUID 0 b t f  1082	1082   23 0 0 0 0 date_mi - - ));
DESCR("");
DATA(insert OID = 1100 ( "+"	   PGUID 0 b t f  1082	  23 1082 0 0 0 0 date_pli - - ));
DESCR("");
DATA(insert OID = 1101 ( "-"	   PGUID 0 b t f  1082	  23 1082 0 0 0 0 date_mii - - ));
DESCR("");

/* time operators */
DATA(insert OID = 1108 ( "="	   PGUID 0 b t t  1083	1083  16 1108 1109 1110 1110 time_eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 1109 ( "<>"	   PGUID 0 b t f  1083	1083  16 1109 1108	0 0 time_ne neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 1110 ( "<"	   PGUID 0 b t f  1083	1083  16 1112 1113	0 0 time_lt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1111 ( "<="	   PGUID 0 b t f  1083	1083  16 1113 1112	0 0 time_le intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1112 ( ">"	   PGUID 0 b t f  1083	1083  16 1110 1111	0 0 time_gt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1113 ( ">="	   PGUID 0 b t f  1083	1083  16 1111 1065	0 0 time_ge intltsel intltjoinsel ));
DESCR("");

/* datetime operators */
/* name, owner, prec, kind, isleft, canhash, left, right, result, com, negate, lsortop, rsortop, oprcode, operrest, oprjoin */
DATA(insert OID = 1320 (  "="	   PGUID 0 b t f 1184 1184	 16 1320 1321 1322 1322 datetime_eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 1321 (  "<>"	   PGUID 0 b t f 1184 1184	 16 1321 1320 0 0 datetime_ne neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 1322 (  "<"	   PGUID 0 b t f 1184 1184	 16 1325 1325 0 0 datetime_lt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1323 (  "<="	   PGUID 0 b t f 1184 1184	 16 1324 1324 0 0 datetime_le intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1324 (  ">"	   PGUID 0 b t f 1184 1184	 16 1323 1323 0 0 datetime_gt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1325 (  ">="	   PGUID 0 b t f 1184 1184	 16 1322 1322 0 0 datetime_ge intltsel intltjoinsel ));
DESCR("");

DATA(insert OID = 1327 (  "+"	   PGUID 0 b t f 1184 1186 1184 1327	0 0 0 datetime_pl_span - - ));
DESCR("");
DATA(insert OID = 1328 (  "-"	   PGUID 0 b t f 1184 1184 1186    0	0 0 0 datetime_mi - - ));
DESCR("");
DATA(insert OID = 1329 (  "-"	   PGUID 0 b t f 1184 1186 1184    0	0 0 0 datetime_mi_span - - ));
DESCR("");

/* timespan operators */
DATA(insert OID = 1330 (  "="	   PGUID 0 b t f 1186 1186	 16 1330 1331 1332 1332 timespan_eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 1331 (  "<>"	   PGUID 0 b t f 1186 1186	 16 1331 1330 0 0 timespan_ne neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 1332 (  "<"	   PGUID 0 b t f 1186 1186	 16 1335 1335 0 0 timespan_lt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1333 (  "<="	   PGUID 0 b t f 1186 1186	 16 1334 1334 0 0 timespan_le intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1334 (  ">"	   PGUID 0 b t f 1186 1186	 16 1333 1333 0 0 timespan_gt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1335 (  ">="	   PGUID 0 b t f 1186 1186	 16 1332 1332 0 0 timespan_ge intltsel intltjoinsel ));
DESCR("");

DATA(insert OID = 1336 (  "-"	   PGUID 0 l t f	0 1186 1186    0	0 0 0 timespan_um 0 0 ));
DESCR("");
DATA(insert OID = 1337 (  "+"	   PGUID 0 b t f 1186 1186 1186 1337	0 0 0 timespan_pl - - ));
DESCR("");
DATA(insert OID = 1338 (  "-"	   PGUID 0 b t f 1186 1186 1186    0	0 0 0 timespan_mi - - ));
DESCR("");

/* float48 operators */
DATA(insert OID = 1116 (  "+"		PGUID 0 b t f 700 701 701 1116	 0	 0	 0 float48pl - - ));
DESCR("");
DATA(insert OID = 1117 (  "-"		PGUID 0 b t f 700 701 701	 0	 0	 0	 0 float48mi - - ));
DESCR("");
DATA(insert OID = 1118 (  "/"		PGUID 0 b t f 700 701 701	 0	 0	 0	 0 float48div - - ));
DESCR("");
DATA(insert OID = 1119 (  "*"		PGUID 0 b t f 700 701 701 1119	 0	 0	 0 float48mul - - ));
DESCR("");
DATA(insert OID = 1120 (  "="		PGUID 0 b t t  700	701  16 1120 1121  1122 1122 float48eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 1121 (  "<>"		PGUID 0 b t f  700	701  16 1121 1120  0 0 float48ne neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 1122 (  "<"		PGUID 0 b t f  700	701  16 1123 1125  0 0 float48lt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1123 (  ">"		PGUID 0 b t f  700	701  16 1122 1124  0 0 float48gt intgtsel intgtjoinsel ));
DESCR("");
DATA(insert OID = 1124 (  "<="		PGUID 0 b t f  700	701  16 1125 1123  0 0 float48le intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1125 (  ">="		PGUID 0 b t f  700	701  16 1124 1122  0 0 float48ge intgtsel intgtjoinsel ));
DESCR("");

/* float84 operators */
DATA(insert OID = 1126 (  "+"		PGUID 0 b t f 701 700 701 1126	 0	 0	 0 float84pl - - ));
DESCR("");
DATA(insert OID = 1127 (  "-"		PGUID 0 b t f 701 700 701	 0	 0	 0	 0 float84mi - - ));
DESCR("");
DATA(insert OID = 1128 (  "/"		PGUID 0 b t f 701 700 701	 0	 0	 0	 0 float84div - - ));
DESCR("");
DATA(insert OID = 1129 (  "*"		PGUID 0 b t f 701 700 701 1129	 0	 0	 0 float84mul - - ));
DESCR("");
DATA(insert OID = 1130 (  "="		PGUID 0 b t t  701	700  16 1130 1131  1132 1132 float84eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 1131 (  "<>"		PGUID 0 b t f  701	700  16 1131 1130  0 0 float84ne neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 1132 (  "<"		PGUID 0 b t f  701	700  16 1133 1135  0 0 float84lt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1133 (  ">"		PGUID 0 b t f  701	700  16 1132 1134  0 0 float84gt intgtsel intgtjoinsel ));
DESCR("");
DATA(insert OID = 1134 (  "<="		PGUID 0 b t f  701	700  16 1135 1133  0 0 float84le intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1135 (  ">="		PGUID 0 b t f  701	700  16 1134 1132  0 0 float84ge intgtsel intgtjoinsel ));
DESCR("");

/* int4 and oid equality */
DATA(insert OID = 1136 (  "="		PGUID 0 b t t 23 26 16 1137 0 0 0 int4eqoid eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 1137 (  "="		PGUID 0 b t t 26 23 16 1136 0 0 0 oideqint4 eqsel eqjoinsel ));
DESCR("");

/* LIKE hacks by Keith Parks. */
DATA(insert OID = 1201 (  "~~"	  PGUID 0 b t f  409  25  16 0 1202 0 0 char2like eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 1202 (  "!~~"   PGUID 0 b t f  409  25  16 0 1201 0 0 char2nlike neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 1203 (  "~~"	  PGUID 0 b t f  410  25  16 0 1204 0 0 char4like eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 1204 (  "!~~"   PGUID 0 b t f  410  25  16 0 1203 0 0 char4nlike neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 1205 (  "~~"	  PGUID 0 b t f  411  25  16 0 1206 0 0 char8like eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 1206 (  "!~~"   PGUID 0 b t f  411  25  16 0 1205 0 0 char8nlike neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 1207 (  "~~"	  PGUID 0 b t f  19   25  16 0 1208 0 0 namelike eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 1208 (  "!~~"   PGUID 0 b t f  19   25  16 0 1207 0 0 namenlike neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 1209 (  "~~"	  PGUID 0 b t f  25   25  16 0 1210 0 0 textlike eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 1210 (  "!~~"   PGUID 0 b t f  25   25  16 0 1209 0 0 textnlike neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 1211 (  "~~"	  PGUID 0 b t f  1042 25  16 0 1212 0 0 textlike eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 1212 (  "!~~"   PGUID 0 b t f  1042 25  16 0 1211 0 0 textnlike neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 1213 (  "~~"	  PGUID 0 b t f  1043 25  16 0 1214 0 0 textlike eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 1214 (  "!~~"   PGUID 0 b t f  1043 25  16 0 1213 0 0 textnlike neqsel neqjoinsel ));
DESCR("");

/* case-insensitive LIKE hacks */
DATA(insert OID = 1220 (  "~*"		 PGUID 0 b t f	409  25  16 0 1221	0 0 char2icregexeq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 1221 (  "!~*"		 PGUID 0 b t f	409  25  16 0 1220	0 0 char2icregexne neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 1222 (  "~*"		 PGUID 0 b t f	410  25  16 0 1223	0 0 char4icregexeq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 1223 (  "!~*"		 PGUID 0 b t f	410  25  16 0 1222	0 0 char4icregexne neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 1224 (  "~*"		 PGUID 0 b t f	411  25  16 0 1225	0 0 char8icregexeq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 1225 (  "!~*"		 PGUID 0 b t f	411  25  16 0 1224	0 0 char8icregexne neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 1226 (  "~*"		 PGUID 0 b t f	19	25	16 0 1227  0 0 nameicregexeq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 1227 (  "!~*"		 PGUID 0 b t f	19	25	16 0 1226  0 0 nameicregexne neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 1228 (  "~*"		 PGUID 0 b t f	25	25	16 0 1229  0 0 texticregexeq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 1229 (  "!~*"		 PGUID 0 b t f	25	25	16 0 1228  0 0 texticregexne eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 1230 (  "~*"		 PGUID 0 b t f	20	25	16 0 1231  0 0 char16icregexeq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 1231 (  "!~*"		 PGUID 0 b t f	20	25	16 0 1230  0 0 char16icregexne neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 1232 (  "~*"		PGUID 0 b t f  1043  25  16 0 1233	0 0 texticregexeq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 1233 ( "!~*"		PGUID 0 b t f  1043  25  16 0 1232	0 0 texticregexne neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 1234 (  "~*"		PGUID 0 b t f  1042  25  16 0 1235	0 0 texticregexeq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 1235 ( "!~*"		PGUID 0 b t f  1042  25  16 0 1234	0 0 texticregexne neqsel neqjoinsel ));
DESCR("");

DATA(insert OID = 1300 (  "="		PGUID 0 b t t  1296 1296 16 1300 1301 1302 1302 timestampeq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 1301 (  "<>"		PGUID 0 b t f  1296 1296 16 1301 1300 0 0 timestampne neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 1302 (  "<"		PGUID 0 b t f  1296 1296 16 1303 1305 0 0 timestamplt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1303 (  ">"		PGUID 0 b t f  1296 1296 16 1302 1304 0 0 timestampgt intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1304 (  "<="		PGUID 0 b t f  1296 1296 16 1305 1303 0 0 timestample intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1305 (  ">="		PGUID 0 b t f  1296 1296 16 1304 1302 0 0 timestampge intltsel intltjoinsel ));
DESCR("");

/* additional geometric operators - thomas 97/04/18 */
DATA(insert OID = 1420 (  "@@"	  PGUID 0 l t f    0  718  600	  0    0	0	 0 circle_center - - ));
DESCR("");
DATA(insert OID = 1500 (  "="	  PGUID 0 b t t  718  718	16 1500 1501 1502 1502 circle_eq eqsel eqjoinsel ));
DESCR("");
DATA(insert OID = 1501 (  "<>"	  PGUID 0 b t f  718  718	16 1501 1500	0	 0 circle_ne neqsel neqjoinsel ));
DESCR("");
DATA(insert OID = 1502 (  "<"	  PGUID 0 b t f  718  718	16	  0    0	0	 0 circle_lt areasel areajoinsel ));
DESCR("");
DATA(insert OID = 1503 (  ">"	  PGUID 0 b t f  718  718	16	  0    0	0	 0 circle_gt areasel areajoinsel ));
DESCR("");
DATA(insert OID = 1504 (  "<="	  PGUID 0 b t f  718  718	16	  0    0	0	 0 circle_le areasel areajoinsel ));
DESCR("");
DATA(insert OID = 1505 (  ">="	  PGUID 0 b t f  718  718	16	  0    0	0	 0 circle_ge areasel areajoinsel ));
DESCR("");

DATA(insert OID = 1506 (  "<<"	  PGUID 0 b t f  718  718	16	  0    0	0	 0 circle_left intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1507 (  "&<"	  PGUID 0 b t f  718  718	16	  0    0	0	 0 circle_overleft intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1508 (  "&>"	  PGUID 0 b t f  718  718	16	  0    0	0	 0 circle_overright intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1509 (  ">>"	  PGUID 0 b t f  718  718	16	  0    0	0	 0 circle_right intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1510 (  "@"	  PGUID 0 b t f  718  718	16 1511    0	0	 0 circle_contained intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1511 (  "~"	  PGUID 0 b t f  718  718	16 1510    0	0	 0 circle_contain intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1512 (  "~="	  PGUID 0 b t f  718  718	16 1512    0	0	 0 circle_same intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1513 (  "&&"	  PGUID 0 b t f  718  718	16	  0    0	0	 0 circle_overlap intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1514 (  ">^"	  PGUID 0 b t f  718  718	16	  0    0	0	 0 circle_above intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1515 (  "<^"	  PGUID 0 b t f  718  718	16	  0    0	0	 0 circle_below intltsel intltjoinsel ));
DESCR("");

DATA(insert OID = 1516 (  "+"	  PGUID 0 b t f  718  600  718 1516    0	0	 0 circle_add_pt - - ));
DESCR("");
DATA(insert OID = 1517 (  "-"	  PGUID 0 b t f  718  600  718	  0    0	0	 0 circle_sub_pt - - ));
DESCR("");
DATA(insert OID = 1518 (  "*"	  PGUID 0 b t f  718  600  718 1518    0	0	 0 circle_mul_pt - - ));
DESCR("");
DATA(insert OID = 1519 (  "/"	  PGUID 0 b t f  718  600  718	  0    0	0	 0 circle_div_pt - - ));
DESCR("");

DATA(insert OID = 1520 (  "<->"   PGUID 0 b t f  718  718  701 1520    0	0	 0 circle_distance intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1521 (  "#"	  PGUID 0 l t f    0  604	23	  0    0	0	 0 poly_npoints - - ));
DESCR("");
DATA(insert OID = 1522 (  "<->"   PGUID 0 b t f  600  718  701 1522    0	0	 0 dist_pc intltsel intltjoinsel ));
DESCR("");
DATA(insert OID = 1523 (  "<->"   PGUID 0 b t f  718  604  701 1523    0	0	 0 dist_cpoly intltsel intltjoinsel ));
DESCR("");

/* additional geometric operators - thomas 1997-07-09 */
DATA(insert OID = 1524 (  "<->"   PGUID 0 b t f  628  603  701 1524  0 0 0 dist_lb - - ));
DESCR("");

DATA(insert OID = 1525 (  "?#"	  PGUID 0 b t f  601  601	16 1525  0 0 0 lseg_intersect - - ));
DESCR("");
DATA(insert OID = 1526 (  "?||"   PGUID 0 b t f  601  601	16 1526  0 0 0 lseg_parallel - - ));
DESCR("");
DATA(insert OID = 1527 (  "?-|"   PGUID 0 b t f  601  601	16 1527  0 0 0 lseg_perp - - ));
DESCR("");
DATA(insert OID = 1528 (  "?-"	  PGUID 0 l t f    0  601	16 1528  0 0 0 lseg_horizontal - - ));
DESCR("");
DATA(insert OID = 1529 (  "?|"	  PGUID 0 l t f    0  601	16 1529  0 0 0 lseg_vertical - - ));
DESCR("");
DATA(insert OID = 1535 (  "="	  PGUID 0 b t f  601  601	16 1535  0 0 0 lseg_eq intltsel - ));
DESCR("");
DATA(insert OID = 1536 (  "#"	  PGUID 0 b t f  601  601  600 1536  0 0 0 lseg_interpt - - ));
DESCR("");
DATA(insert OID = 1537 (  "?#"	  PGUID 0 b t f  601  628	16 1537  0 0 0 inter_sl - - ));
DESCR("");
DATA(insert OID = 1538 (  "?#"	  PGUID 0 b t f  601  603	16 1538  0 0 0 inter_sb - - ));
DESCR("");
DATA(insert OID = 1539 (  "?#"	  PGUID 0 b t f  628  603	16 1539  0 0 0 inter_lb - - ));
DESCR("");

DATA(insert OID = 1546 (  "@"	  PGUID 0 b t f  600  628	16	  0  0 0 0 on_pl - - ));
DESCR("");
DATA(insert OID = 1547 (  "@"	  PGUID 0 b t f  600  601	16	  0  0 0 0 on_ps - - ));
DESCR("");
DATA(insert OID = 1548 (  "@"	  PGUID 0 b t f  601  628	16	  0  0 0 0 on_sl - - ));
DESCR("");
DATA(insert OID = 1549 (  "@"	  PGUID 0 b t f  601  603	16	  0  0 0 0 on_sb - - ));
DESCR("");

DATA(insert OID = 1557 (  "##"	  PGUID 0 b t f  600  628  600	  0  0 0 0 close_pl - - ));
DESCR("");
DATA(insert OID = 1558 (  "##"	  PGUID 0 b t f  600  601  600	  0  0 0 0 close_ps - - ));
DESCR("");
DATA(insert OID = 1559 (  "##"	  PGUID 0 b t f  600  603  600	  0  0 0 0 close_pb - - ));
DESCR("");

DATA(insert OID = 1566 (  "##"	  PGUID 0 b t f  601  628  600	  0  0 0 0 close_sl - - ));
DESCR("");
DATA(insert OID = 1567 (  "##"	  PGUID 0 b t f  601  603  600	  0  0 0 0 close_sb - - ));
DESCR("");
DATA(insert OID = 1568 (  "##"	  PGUID 0 b t f  628  603  600	  0  0 0 0 close_lb - - ));
DESCR("");
DATA(insert OID = 1585 (  "/"	  PGUID 0 b t f 1186 1186 1186	  0  0 0 0 timespan_div - - ));
DESCR("");

/*
 * function prototypes
 */
extern void
OperatorCreate(char *operatorName,
			   char *leftTypeName,
			   char *rightTypeName,
			   char *procedureName,
			   uint16 precedence,
			   bool isLeftAssociative,
			   char *commutatorName,
			   char *negatorName,
			   char *restrictionName,
			   char *joinName,
			   bool canHash,
			   char *leftSortName,
			   char *rightSortName);

#endif							/* PG_OPERATOR_H */
