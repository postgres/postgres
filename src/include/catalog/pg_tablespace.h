/*-------------------------------------------------------------------------
 *
 * pg_tablespace.h
 *	  definition of the system "tablespace" relation (pg_tablespace)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/catalog/pg_tablespace.h,v 1.8 2006/03/05 15:58:55 momjian Exp $
 *
 * NOTES
 *	  the genbki.sh script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TABLESPACE_H
#define PG_TABLESPACE_H

/* ----------------
 *		postgres.h contains the system type definitions and the
 *		CATALOG(), BKI_BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------
 *		pg_tablespace definition.  cpp turns this into
 *		typedef struct FormData_pg_tablespace
 * ----------------
 */
#define TableSpaceRelationId  1213

CATALOG(pg_tablespace,1213) BKI_SHARED_RELATION
{
	NameData	spcname;		/* tablespace name */
	Oid			spcowner;		/* owner of tablespace */
	text		spclocation;	/* physical location (VAR LENGTH) */
	aclitem		spcacl[1];		/* access permissions (VAR LENGTH) */
} FormData_pg_tablespace;

/* ----------------
 *		Form_pg_tablespace corresponds to a pointer to a tuple with
 *		the format of pg_tablespace relation.
 * ----------------
 */
typedef FormData_pg_tablespace *Form_pg_tablespace;

/* ----------------
 *		compiler constants for pg_tablespace
 * ----------------
 */

#define Natts_pg_tablespace				4
#define Anum_pg_tablespace_spcname		1
#define Anum_pg_tablespace_spcowner		2
#define Anum_pg_tablespace_spclocation	3
#define Anum_pg_tablespace_spcacl		4

DATA(insert OID = 1663 ( pg_default PGUID "" _null_ ));
DATA(insert OID = 1664 ( pg_global	PGUID "" _null_ ));

#define DEFAULTTABLESPACE_OID 1663
#define GLOBALTABLESPACE_OID 1664

#endif   /* PG_TABLESPACE_H */
