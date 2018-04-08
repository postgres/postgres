/*-------------------------------------------------------------------------
 *
 * pg_database.h
 *	  definition of the system "database" relation (pg_database)
 *
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_database.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_DATABASE_H
#define PG_DATABASE_H

#include "catalog/genbki.h"
#include "catalog/pg_database_d.h"

/* ----------------
 *		pg_database definition.  cpp turns this into
 *		typedef struct FormData_pg_database
 * ----------------
 */
CATALOG(pg_database,1262,DatabaseRelationId) BKI_SHARED_RELATION BKI_ROWTYPE_OID(1248,DatabaseRelation_Rowtype_Id) BKI_SCHEMA_MACRO
{
	NameData	datname;		/* database name */
	Oid			datdba;			/* owner of database */
	int32		encoding;		/* character encoding */
	NameData	datcollate;		/* LC_COLLATE setting */
	NameData	datctype;		/* LC_CTYPE setting */
	bool		datistemplate;	/* allowed as CREATE DATABASE template? */
	bool		datallowconn;	/* new connections allowed? */
	int32		datconnlimit;	/* max connections allowed (-1=no limit) */
	Oid			datlastsysoid;	/* highest OID to consider a system OID */
	TransactionId datfrozenxid; /* all Xids < this are frozen in this DB */
	TransactionId datminmxid;	/* all multixacts in the DB are >= this */
	Oid			dattablespace;	/* default table space for this DB */

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	aclitem		datacl[1];		/* access permissions */
#endif
} FormData_pg_database;

/* ----------------
 *		Form_pg_database corresponds to a pointer to a tuple with
 *		the format of pg_database relation.
 * ----------------
 */
typedef FormData_pg_database *Form_pg_database;

#endif							/* PG_DATABASE_H */
