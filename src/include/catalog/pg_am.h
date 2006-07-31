/*-------------------------------------------------------------------------
 *
 * pg_am.h
 *	  definition of the system "access method" relation (pg_am)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/catalog/pg_am.h,v 1.46 2006/07/31 20:09:05 tgl Exp $
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
 *		CATALOG(), BKI_BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------
 *		pg_am definition.  cpp turns this into
 *		typedef struct FormData_pg_am
 * ----------------
 */
#define AccessMethodRelationId	2601

CATALOG(pg_am,2601)
{
	NameData	amname;			/* access method name */
	int2		amstrategies;	/* total NUMBER of strategies (operators) by
								 * which we can traverse/search this AM */
	int2		amsupport;		/* total NUMBER of support functions that this
								 * AM uses */
	int2		amorderstrategy;/* if this AM has a sort order, the strategy
								 * number of the sort operator. Zero if AM is
								 * not ordered. */
	bool		amcanunique;	/* does AM support UNIQUE indexes? */
	bool		amcanmulticol;	/* does AM support multi-column indexes? */
	bool		amoptionalkey;	/* can query omit key for the first column? */
	bool		amindexnulls;	/* does AM support NULL index entries? */
	bool		amstorage;		/* can storage type differ from column type? */
	bool		amclusterable;	/* does AM support cluster command? */
	regproc		aminsert;		/* "insert this tuple" function */
	regproc		ambeginscan;	/* "start new scan" function */
	regproc		amgettuple;		/* "next valid tuple" function */
	regproc		amgetmulti;		/* "fetch multiple tuples" function */
	regproc		amrescan;		/* "restart this scan" function */
	regproc		amendscan;		/* "end this scan" function */
	regproc		ammarkpos;		/* "mark current scan position" function */
	regproc		amrestrpos;		/* "restore marked scan position" function */
	regproc		ambuild;		/* "build new index" function */
	regproc		ambulkdelete;	/* bulk-delete function */
	regproc		amvacuumcleanup;	/* post-VACUUM cleanup function */
	regproc		amcostestimate; /* estimate cost of an indexscan */
	regproc		amoptions;		/* parse AM-specific parameters */
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
#define Natts_pg_am						23
#define Anum_pg_am_amname				1
#define Anum_pg_am_amstrategies			2
#define Anum_pg_am_amsupport			3
#define Anum_pg_am_amorderstrategy		4
#define Anum_pg_am_amcanunique			5
#define Anum_pg_am_amcanmulticol		6
#define Anum_pg_am_amoptionalkey		7
#define Anum_pg_am_amindexnulls			8
#define Anum_pg_am_amstorage			9
#define Anum_pg_am_amclusterable		10
#define Anum_pg_am_aminsert				11
#define Anum_pg_am_ambeginscan			12
#define Anum_pg_am_amgettuple			13
#define Anum_pg_am_amgetmulti			14
#define Anum_pg_am_amrescan				15
#define Anum_pg_am_amendscan			16
#define Anum_pg_am_ammarkpos			17
#define Anum_pg_am_amrestrpos			18
#define Anum_pg_am_ambuild				19
#define Anum_pg_am_ambulkdelete			20
#define Anum_pg_am_amvacuumcleanup		21
#define Anum_pg_am_amcostestimate		22
#define Anum_pg_am_amoptions			23

/* ----------------
 *		initial contents of pg_am
 * ----------------
 */

DATA(insert OID = 403 (  btree	5 1 1 t t t t f t btinsert btbeginscan btgettuple btgetmulti btrescan btendscan btmarkpos btrestrpos btbuild btbulkdelete btvacuumcleanup btcostestimate btoptions ));
DESCR("b-tree index access method");
#define BTREE_AM_OID 403
DATA(insert OID = 405 (  hash	1 1 0 f f f f f f hashinsert hashbeginscan hashgettuple hashgetmulti hashrescan hashendscan hashmarkpos hashrestrpos hashbuild hashbulkdelete hashvacuumcleanup hashcostestimate hashoptions ));
DESCR("hash index access method");
#define HASH_AM_OID 405
DATA(insert OID = 783 (  gist	100 7 0 f t t t t t gistinsert gistbeginscan gistgettuple gistgetmulti gistrescan gistendscan gistmarkpos gistrestrpos gistbuild gistbulkdelete gistvacuumcleanup gistcostestimate gistoptions ));
DESCR("GiST index access method");
#define GIST_AM_OID 783
DATA(insert OID = 2742 (  gin	100 4 0 f f f f t f gininsert ginbeginscan gingettuple gingetmulti ginrescan ginendscan ginmarkpos ginrestrpos ginbuild ginbulkdelete ginvacuumcleanup gincostestimate ginoptions ));
DESCR("GIN index access method");
#define GIN_AM_OID 2742

#endif   /* PG_AM_H */
