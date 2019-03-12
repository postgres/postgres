/*-------------------------------------------------------------------------
 *
 * pg_database.h
 *	  definition of the "database" system catalog (pg_database)
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
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
	/* oid */
	Oid			oid;

	/* database name */
	NameData	datname;

	/* owner of database */
	Oid			datdba BKI_DEFAULT(PGUID);

	/* character encoding */
	int32		encoding;

	/* LC_COLLATE setting */
	NameData	datcollate;

	/* LC_CTYPE setting */
	NameData	datctype;

	/* allowed as CREATE DATABASE template? */
	bool		datistemplate;

	/* new connections allowed? */
	bool		datallowconn;

	/* max connections allowed (-1=no limit) */
	int32		datconnlimit;

	/* highest OID to consider a system OID */
	Oid			datlastsysoid;

	/* all Xids < this are frozen in this DB */
	TransactionId datfrozenxid;

	/* all multixacts in the DB are >= this */
	TransactionId datminmxid;

	/* default table space for this DB */
	Oid			dattablespace BKI_LOOKUP(pg_tablespace);

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	/* access permissions */
	aclitem		datacl[1];
#endif
} FormData_pg_database;

/* ----------------
 *		Form_pg_database corresponds to a pointer to a tuple with
 *		the format of pg_database relation.
 * ----------------
 */
typedef FormData_pg_database *Form_pg_database;

#endif							/* PG_DATABASE_H */
