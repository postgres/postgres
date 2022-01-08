/*-------------------------------------------------------------------------
 *
 * pg_foreign_server.h
 *	  definition of the "foreign server" system catalog (pg_foreign_server)
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_foreign_server.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_FOREIGN_SERVER_H
#define PG_FOREIGN_SERVER_H

#include "catalog/genbki.h"
#include "catalog/pg_foreign_server_d.h"

/* ----------------
 *		pg_foreign_server definition.  cpp turns this into
 *		typedef struct FormData_pg_foreign_server
 * ----------------
 */
CATALOG(pg_foreign_server,1417,ForeignServerRelationId)
{
	Oid			oid;			/* oid */
	NameData	srvname;		/* foreign server name */
	Oid			srvowner BKI_LOOKUP(pg_authid); /* server owner */
	Oid			srvfdw BKI_LOOKUP(pg_foreign_data_wrapper); /* server FDW */

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	text		srvtype;
	text		srvversion;
	aclitem		srvacl[1];		/* access permissions */
	text		srvoptions[1];	/* FDW-specific options */
#endif
} FormData_pg_foreign_server;

/* ----------------
 *		Form_pg_foreign_server corresponds to a pointer to a tuple with
 *		the format of pg_foreign_server relation.
 * ----------------
 */
typedef FormData_pg_foreign_server *Form_pg_foreign_server;

DECLARE_TOAST(pg_foreign_server, 4151, 4152);

DECLARE_UNIQUE_INDEX_PKEY(pg_foreign_server_oid_index, 113, ForeignServerOidIndexId, on pg_foreign_server using btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_foreign_server_name_index, 549, ForeignServerNameIndexId, on pg_foreign_server using btree(srvname name_ops));

#endif							/* PG_FOREIGN_SERVER_H */
