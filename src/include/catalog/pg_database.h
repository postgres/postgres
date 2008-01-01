/*-------------------------------------------------------------------------
 *
 * pg_database.h
 *	  definition of the system "database" relation (pg_database)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/catalog/pg_database.h,v 1.46 2008/01/01 19:45:56 momjian Exp $
 *
 * NOTES
 *	  the genbki.sh script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_DATABASE_H
#define PG_DATABASE_H

/* ----------------
 *		postgres.h contains the system type definitions and the
 *		CATALOG(), BKI_BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------
 *		pg_database definition.  cpp turns this into
 *		typedef struct FormData_pg_database
 * ----------------
 */
#define DatabaseRelationId	1262

CATALOG(pg_database,1262) BKI_SHARED_RELATION
{
	NameData	datname;		/* database name */
	Oid			datdba;			/* owner of database */
	int4		encoding;		/* character encoding */
	bool		datistemplate;	/* allowed as CREATE DATABASE template? */
	bool		datallowconn;	/* new connections allowed? */
	int4		datconnlimit;	/* max connections allowed (-1=no limit) */
	Oid			datlastsysoid;	/* highest OID to consider a system OID */
	TransactionId datfrozenxid; /* all Xids < this are frozen in this DB */
	Oid			dattablespace;	/* default table space for this DB */
	text		datconfig[1];	/* database-specific GUC (VAR LENGTH) */
	aclitem		datacl[1];		/* access permissions (VAR LENGTH) */
} FormData_pg_database;

/* ----------------
 *		Form_pg_database corresponds to a pointer to a tuple with
 *		the format of pg_database relation.
 * ----------------
 */
typedef FormData_pg_database *Form_pg_database;

/* ----------------
 *		compiler constants for pg_database
 * ----------------
 */
#define Natts_pg_database				11
#define Anum_pg_database_datname		1
#define Anum_pg_database_datdba			2
#define Anum_pg_database_encoding		3
#define Anum_pg_database_datistemplate	4
#define Anum_pg_database_datallowconn	5
#define Anum_pg_database_datconnlimit	6
#define Anum_pg_database_datlastsysoid	7
#define Anum_pg_database_datfrozenxid	8
#define Anum_pg_database_dattablespace	9
#define Anum_pg_database_datconfig		10
#define Anum_pg_database_datacl			11

DATA(insert OID = 1 (  template1 PGUID ENCODING t t -1 0 0 1663 _null_ _null_ ));
SHDESCR("default template database");
#define TemplateDbOid			1

#endif   /* PG_DATABASE_H */
