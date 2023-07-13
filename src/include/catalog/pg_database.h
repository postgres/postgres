/*-------------------------------------------------------------------------
 *
 * pg_database.h
 *	  definition of the "database" system catalog (pg_database)
 *
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
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
	Oid			datdba BKI_DEFAULT(POSTGRES) BKI_LOOKUP(pg_authid);

	/* character encoding */
	int32		encoding;

	/* locale provider, see pg_collation.collprovider */
	char		datlocprovider;

	/* allowed as CREATE DATABASE template? */
	bool		datistemplate;

	/* new connections allowed? */
	bool		datallowconn;

	/*
	 * Max connections allowed. Negative values have special meaning, see
	 * DATCONNLIMIT_* defines below.
	 */
	int32		datconnlimit;

	/* all Xids < this are frozen in this DB */
	TransactionId datfrozenxid;

	/* all multixacts in the DB are >= this */
	TransactionId datminmxid;

	/* default table space for this DB */
	Oid			dattablespace BKI_LOOKUP(pg_tablespace);

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	/* LC_COLLATE setting */
	text		datcollate BKI_FORCE_NOT_NULL;

	/* LC_CTYPE setting */
	text		datctype BKI_FORCE_NOT_NULL;

	/* ICU locale ID */
	text		daticulocale;

	/* provider-dependent version of collation data */
	text		datcollversion BKI_DEFAULT(_null_);

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

DECLARE_TOAST_WITH_MACRO(pg_database, 4177, 4178, PgDatabaseToastTable, PgDatabaseToastIndex);

DECLARE_UNIQUE_INDEX(pg_database_datname_index, 2671, DatabaseNameIndexId, on pg_database using btree(datname name_ops));
DECLARE_UNIQUE_INDEX_PKEY(pg_database_oid_index, 2672, DatabaseOidIndexId, on pg_database using btree(oid oid_ops));

/*
 * pg_database.dat contains an entry for template1, but not for the template0
 * or postgres databases, because those are created later in initdb.
 * However, we still want to manually assign the OIDs for template0 and
 * postgres, so declare those here.
 */
DECLARE_OID_DEFINING_MACRO(Template0DbOid, 4);
DECLARE_OID_DEFINING_MACRO(PostgresDbOid, 5);

/*
 * Special values for pg_database.datconnlimit. Normal values are >= 0.
 */
#define		  DATCONNLIMIT_UNLIMITED	-1	/* no limit */

/*
 * A database is set to invalid partway through being dropped.  Using
 * datconnlimit=-2 for this purpose isn't particularly clean, but is
 * backpatchable.
 */
#define		  DATCONNLIMIT_INVALID_DB	-2

extern bool database_is_invalid_form(Form_pg_database datform);
extern bool database_is_invalid_oid(Oid dboid);

#endif							/* PG_DATABASE_H */
