/*-------------------------------------------------------------------------
 *
 * pg_am.h--
 *	  definition of the system "am" relation (pg_am)
 *	  along with the relation's initial contents.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_am.h,v 1.6 1998/08/11 05:32:43 momjian Exp $
 *
 * NOTES
 *		the genbki.sh script reads this file and generates .bki
 *		information from the DATA() statements.
 *
 *		XXX do NOT break up DATA() statements into multiple lines!
 *			the scripts are not as smart as you might think...
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_AM_H
#define PG_AM_H

/* ----------------
 *		postgres.h contains the system type definintions and the
 *		CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------
 *		pg_am definition.  cpp turns this into
 *		typedef struct FormData_pg_am
 * ----------------
 */
CATALOG(pg_am)
{
	NameData	amname;
	Oid			amowner;
	char		amkind;
	int2		amstrategies;
	int2		amsupport;
	regproc		amgettuple;
	regproc		aminsert;
	regproc		amdelete;
	regproc		amgetattr;
	regproc		amsetlock;
	regproc		amsettid;
	regproc		amfreetuple;
	regproc		ambeginscan;
	regproc		amrescan;
	regproc		amendscan;
	regproc		ammarkpos;
	regproc		amrestrpos;
	regproc		amopen;
	regproc		amclose;
	regproc		ambuild;
	regproc		amcreate;
	regproc		amdestroy;
} FormData_pg_am;

/* ----------------
 *		Form_pg_am corresponds to a pointer to a tuple with
 *		the format of pg_am relation.
 * ----------------
 */
typedef FormData_pg_am *Form_pg_am;

/* ----------------
 *		compiler constants for pg_am
 * ----------------
 */
#define Natts_pg_am						22
#define Anum_pg_am_amname				1
#define Anum_pg_am_amowner				2
#define Anum_pg_am_amkind				3
#define Anum_pg_am_amstrategies			4
#define Anum_pg_am_amsupport			5
#define Anum_pg_am_amgettuple			6
#define Anum_pg_am_aminsert				7
#define Anum_pg_am_amdelete				8
#define Anum_pg_am_amgetattr			9
#define Anum_pg_am_amsetlock			10
#define Anum_pg_am_amsettid				11
#define Anum_pg_am_amfreetuple			12
#define Anum_pg_am_ambeginscan			13
#define Anum_pg_am_amrescan				14
#define Anum_pg_am_amendscan			15
#define Anum_pg_am_ammarkpos			16
#define Anum_pg_am_amrestrpos			17
#define Anum_pg_am_amopen				18
#define Anum_pg_am_amclose				19
#define Anum_pg_am_ambuild				20
#define Anum_pg_am_amcreate				21
#define Anum_pg_am_amdestroy			22

/* ----------------
 *		initial contents of pg_am
 * ----------------
 */

DATA(insert OID = 405 (  hash PGUID "o"  1 1 hashgettuple hashinsert hashdelete - - - - hashbeginscan hashrescan hashendscan hashmarkpos hashrestrpos - - hashbuild - - ));
DESCR("");
DATA(insert OID = 402 (  rtree PGUID "o" 8 3 rtgettuple rtinsert rtdelete - - - - rtbeginscan rtrescan rtendscan rtmarkpos rtrestrpos - - rtbuild - - ));
DESCR("");
DATA(insert OID = 403 (  btree PGUID "o" 5 1 btgettuple btinsert btdelete - - - - btbeginscan btrescan btendscan btmarkpos btrestrpos - - btbuild - - ));
DESCR("");
#define BTREE_AM_OID 403
DATA(insert OID = 783 (  gist PGUID "o" 100 7 gistgettuple gistinsert gistdelete - - - - gistbeginscan gistrescan gistendscan gistmarkpos gistrestrpos - - gistbuild - - ));
DESCR("");

#endif							/* PG_AM_H */
