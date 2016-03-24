/*-------------------------------------------------------------------------
 *
 * pg_am.h
 *	  definition of the system "access method" relation (pg_am)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_am.h
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
	regproc		amhandler;		/* handler function */
	char		amtype;			/* see AMTYPE_xxx constants below */
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
#define Natts_pg_am						3
#define Anum_pg_am_amname				1
#define Anum_pg_am_amhandler			2
#define Anum_pg_am_amtype				3

/* ----------------
 *		compiler constant for amtype
 * ----------------
 */
#define AMTYPE_INDEX					'i'		/* index access method */

/* ----------------
 *		initial contents of pg_am
 * ----------------
 */

DATA(insert OID = 403 (  btree		bthandler	i ));
DESCR("b-tree index access method");
#define BTREE_AM_OID 403
DATA(insert OID = 405 (  hash		hashhandler i ));
DESCR("hash index access method");
#define HASH_AM_OID 405
DATA(insert OID = 783 (  gist		gisthandler i ));
DESCR("GiST index access method");
#define GIST_AM_OID 783
DATA(insert OID = 2742 (  gin		ginhandler	i ));
DESCR("GIN index access method");
#define GIN_AM_OID 2742
DATA(insert OID = 4000 (  spgist	spghandler	i ));
DESCR("SP-GiST index access method");
#define SPGIST_AM_OID 4000
DATA(insert OID = 3580 (  brin		brinhandler i ));
DESCR("block range index (BRIN) access method");
#define BRIN_AM_OID 3580

#endif   /* PG_AM_H */
