/*-------------------------------------------------------------------------
 *
 * pg_am.h
 *	  definition of the system "am" relation (pg_am)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_am.h,v 1.28 2003/08/04 02:40:10 momjian Exp $
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
 *		postgres.h contains the system type definitions and the
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
	NameData	amname;			/* access method name */
	int4		amowner;		/* usesysid of creator */
	int2		amstrategies;	/* total NUMBER of strategies (operators)
								 * by which we can traverse/search this AM */
	int2		amsupport;		/* total NUMBER of support functions that
								 * this AM uses */
	int2		amorderstrategy;/* if this AM has a sort order, the
								 * strategy number of the sort operator.
								 * Zero if AM is not ordered. */
	bool		amcanunique;	/* does AM support UNIQUE indexes? */
	bool		amcanmulticol;	/* does AM support multi-column indexes? */
	bool		amindexnulls;	/* does AM support NULL index entries? */
	bool		amconcurrent;	/* does AM support concurrent updates? */
	regproc		amgettuple;		/* "next valid tuple" function */
	regproc		aminsert;		/* "insert this tuple" function */
	regproc		ambeginscan;	/* "start new scan" function */
	regproc		amrescan;		/* "restart this scan" function */
	regproc		amendscan;		/* "end this scan" function */
	regproc		ammarkpos;		/* "mark current scan position" function */
	regproc		amrestrpos;		/* "restore marked scan position" function */
	regproc		ambuild;		/* "build new index" function */
	regproc		ambulkdelete;	/* bulk-delete function */
	regproc		amvacuumcleanup;	/* post-VACUUM cleanup function */
	regproc		amcostestimate; /* estimate cost of an indexscan */
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
#define Natts_pg_am						20
#define Anum_pg_am_amname				1
#define Anum_pg_am_amowner				2
#define Anum_pg_am_amstrategies			3
#define Anum_pg_am_amsupport			4
#define Anum_pg_am_amorderstrategy		5
#define Anum_pg_am_amcanunique			6
#define Anum_pg_am_amcanmulticol		7
#define Anum_pg_am_amindexnulls			8
#define Anum_pg_am_amconcurrent			9
#define Anum_pg_am_amgettuple			10
#define Anum_pg_am_aminsert				11
#define Anum_pg_am_ambeginscan			12
#define Anum_pg_am_amrescan				13
#define Anum_pg_am_amendscan			14
#define Anum_pg_am_ammarkpos			15
#define Anum_pg_am_amrestrpos			16
#define Anum_pg_am_ambuild				17
#define Anum_pg_am_ambulkdelete			18
#define Anum_pg_am_amvacuumcleanup		19
#define Anum_pg_am_amcostestimate		20

/* ----------------
 *		initial contents of pg_am
 * ----------------
 */

DATA(insert OID = 402 (  rtree	PGUID	8 3 0 f f f f rtgettuple rtinsert rtbeginscan rtrescan rtendscan rtmarkpos rtrestrpos rtbuild rtbulkdelete - rtcostestimate ));
DESCR("r-tree index access method");
DATA(insert OID = 403 (  btree	PGUID	5 1 1 t t t t btgettuple btinsert btbeginscan btrescan btendscan btmarkpos btrestrpos btbuild btbulkdelete btvacuumcleanup btcostestimate ));
DESCR("b-tree index access method");
#define BTREE_AM_OID 403
DATA(insert OID = 405 (  hash	PGUID	1 1 0 f f f t hashgettuple hashinsert hashbeginscan hashrescan hashendscan hashmarkpos hashrestrpos hashbuild hashbulkdelete - hashcostestimate ));
DESCR("hash index access method");
#define HASH_AM_OID 405
DATA(insert OID = 783 (  gist	PGUID 100 7 0 f t f f gistgettuple gistinsert gistbeginscan gistrescan gistendscan gistmarkpos gistrestrpos gistbuild gistbulkdelete - gistcostestimate ));
DESCR("GiST index access method");
#define GIST_AM_OID 783

#endif   /* PG_AM_H */
