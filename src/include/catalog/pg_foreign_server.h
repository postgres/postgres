/*-------------------------------------------------------------------------
 *
 * pg_foreign_server.h
 *	  definition of the "foreign server" system catalog (pg_foreign_server)
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
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
	Oid			srvowner;		/* server owner */
	Oid			srvfdw;			/* server FDW */

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

#endif							/* PG_FOREIGN_SERVER_H */
