/*-------------------------------------------------------------------------
 *
 * pg_am.h
 *	  definition of the system "access method" relation (pg_am)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/catalog/pg_am.h,v 1.65 2010/01/05 01:06:56 tgl Exp $
 *
 * NOTES
 *		the genbki.pl script reads this file and generates .bki
 *		information from the DATA() statements.
 *
 *		XXX do NOT break up DATA() statements into multiple lines!
 *			the scripts are not as smart as you might think...
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_AM_H
#define PG_AM_H

#include "catalog/genbki.h"

/* ----------------
 *		pg_am definition.  cpp turns this into
 *		typedef struct FormData_pg_am
 * ----------------
 */
#define AccessMethodRelationId	2601

CATALOG(pg_am,2601)
{
	NameData	amname;			/* access method name */
	int2		amstrategies;	/* total number of strategies (operators) by
								 * which we can traverse/search this AM. Zero
								 * if AM does not have a fixed set of strategy
								 * assignments. */
	int2		amsupport;		/* total number of support functions that this
								 * AM uses */
	bool		amcanorder;		/* does AM support ordered scan results? */
	bool		amcanbackward;	/* does AM support backward scan? */
	bool		amcanunique;	/* does AM support UNIQUE indexes? */
	bool		amcanmulticol;	/* does AM support multi-column indexes? */
	bool		amoptionalkey;	/* can query omit key for the first column? */
	bool		amindexnulls;	/* does AM support NULL index entries? */
	bool		amsearchnulls;	/* can AM search for NULL/NOT NULL entries? */
	bool		amstorage;		/* can storage type differ from column type? */
	bool		amclusterable;	/* does AM support cluster command? */
	Oid			amkeytype;		/* type of data in index, or InvalidOid */
	regproc		aminsert;		/* "insert this tuple" function */
	regproc		ambeginscan;	/* "start new scan" function */
	regproc		amgettuple;		/* "next valid tuple" function, or 0 */
	regproc		amgetbitmap;	/* "fetch all valid tuples" function, or 0 */
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
#define Natts_pg_am						26
#define Anum_pg_am_amname				1
#define Anum_pg_am_amstrategies			2
#define Anum_pg_am_amsupport			3
#define Anum_pg_am_amcanorder			4
#define Anum_pg_am_amcanbackward		5
#define Anum_pg_am_amcanunique			6
#define Anum_pg_am_amcanmulticol		7
#define Anum_pg_am_amoptionalkey		8
#define Anum_pg_am_amindexnulls			9
#define Anum_pg_am_amsearchnulls		10
#define Anum_pg_am_amstorage			11
#define Anum_pg_am_amclusterable		12
#define Anum_pg_am_amkeytype			13
#define Anum_pg_am_aminsert				14
#define Anum_pg_am_ambeginscan			15
#define Anum_pg_am_amgettuple			16
#define Anum_pg_am_amgetbitmap			17
#define Anum_pg_am_amrescan				18
#define Anum_pg_am_amendscan			19
#define Anum_pg_am_ammarkpos			20
#define Anum_pg_am_amrestrpos			21
#define Anum_pg_am_ambuild				22
#define Anum_pg_am_ambulkdelete			23
#define Anum_pg_am_amvacuumcleanup		24
#define Anum_pg_am_amcostestimate		25
#define Anum_pg_am_amoptions			26

/* ----------------
 *		initial contents of pg_am
 * ----------------
 */

DATA(insert OID = 403 (  btree	5 1 t t t t t t t f t 0 btinsert btbeginscan btgettuple btgetbitmap btrescan btendscan btmarkpos btrestrpos btbuild btbulkdelete btvacuumcleanup btcostestimate btoptions ));
DESCR("b-tree index access method");
#define BTREE_AM_OID 403
DATA(insert OID = 405 (  hash	1 1 f t f f f f f f f 23 hashinsert hashbeginscan hashgettuple hashgetbitmap hashrescan hashendscan hashmarkpos hashrestrpos hashbuild hashbulkdelete hashvacuumcleanup hashcostestimate hashoptions ));
DESCR("hash index access method");
#define HASH_AM_OID 405
DATA(insert OID = 783 (  gist	0 7 f f f t t t t t t 0 gistinsert gistbeginscan gistgettuple gistgetbitmap gistrescan gistendscan gistmarkpos gistrestrpos gistbuild gistbulkdelete gistvacuumcleanup gistcostestimate gistoptions ));
DESCR("GiST index access method");
#define GIST_AM_OID 783
DATA(insert OID = 2742 (  gin	0 5 f f f t t f f t f 0 gininsert ginbeginscan - gingetbitmap ginrescan ginendscan ginmarkpos ginrestrpos ginbuild ginbulkdelete ginvacuumcleanup gincostestimate ginoptions ));
DESCR("GIN index access method");
#define GIN_AM_OID 2742

#endif   /* PG_AM_H */
