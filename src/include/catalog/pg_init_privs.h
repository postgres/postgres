/*-------------------------------------------------------------------------
 *
 * pg_init_privs.h
 *	  definition of the system "initial privileges" relation (pg_init_privs)
 *
 * NOTE: an object is identified by the OID of the row that primarily
 * defines the object, plus the OID of the table that that row appears in.
 * For example, a function is identified by the OID of its pg_proc row
 * plus the pg_class OID of table pg_proc.  This allows unique identification
 * of objects without assuming that OIDs are unique across tables.
 *
 * Since attributes don't have OIDs of their own, we identify an attribute
 * privilege by the objoid+classoid of its parent table, plus an "objsubid"
 * giving the attribute column number.  "objsubid" must be zero in a privilege
 * for a table itself, so that it is distinct from any column privilege.
 * Currently, objsubid is unused and zero for all other kinds of objects.
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_init_privs.h
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
#ifndef PG_INIT_PRIVS_H
#define PG_INIT_PRIVS_H

#include "catalog/genbki.h"

/* ----------------
 *		pg_init_privs definition.  cpp turns this into
 *		typedef struct FormData_pg_init_privs
 * ----------------
 */
#define InitPrivsRelationId  3394

CATALOG(pg_init_privs,3394) BKI_WITHOUT_OIDS
{
	Oid			objoid;			/* OID of object itself */
	Oid			classoid;		/* OID of table containing object */
	int32		objsubid;		/* column number, or 0 if not used */
	char		privtype;		/* from initdb or extension? */

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	aclitem		initprivs[1] BKI_FORCE_NOT_NULL;		/* initial privs on
														 * object */
#endif
} FormData_pg_init_privs;

/* ----------------
 *		Form_pg_init_privs corresponds to a pointer to a tuple with
 *		the format of pg_init_privs relation.
 * ----------------
 */
typedef FormData_pg_init_privs *Form_pg_init_privs;

/* ----------------
 *		compiler constants for pg_init_privs
 * ----------------
 */
#define Natts_pg_init_privs				5
#define Anum_pg_init_privs_objoid		1
#define Anum_pg_init_privs_classoid		2
#define Anum_pg_init_privs_objsubid		3
#define Anum_pg_init_privs_privtype		4
#define Anum_pg_init_privs_privs		5

/*
 * It is important to know if the initial privileges are from initdb or from an
 * extension.  This enum is used to provide that differentiation and the two
 * places which populate this table (initdb and during CREATE EXTENSION, see
 * recordExtensionInitPriv()) know to use the correct values.
 */

typedef enum InitPrivsType
{
	INITPRIVS_INITDB = 'i',
	INITPRIVS_EXTENSION = 'e'
} InitPrivsType;

/* ----------------
 *		initial contents of pg_init_privs
 * ----------------
 */

/*
 *	Because the contents of this table depend on what is done with the other
 *	objects in the system (and, in particular, may change due to changes is
 *	system_views.sql), there is no initialization here.
 *
 *	The initial contents are loaded near the end of initdb.
 */

#endif   /* PG_INIT_PRIVS_H */
