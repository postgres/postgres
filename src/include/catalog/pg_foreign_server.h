/*-------------------------------------------------------------------------
 *
 * pg_foreign_server.h
 *	  definition of the system "foreign server" relation (pg_foreign_server)
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/catalog/pg_foreign_server.h,v 1.5 2010/01/05 01:06:56 tgl Exp $
 *
 * NOTES
 *	  the genbki.pl script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_FOREIGN_SERVER_H
#define PG_FOREIGN_SERVER_H

#include "catalog/genbki.h"

/* ----------------
 *		pg_foreign_server definition.  cpp turns this into
 *		typedef struct FormData_pg_foreign_server
 * ----------------
 */
#define ForeignServerRelationId 1417

CATALOG(pg_foreign_server,1417)
{
	NameData	srvname;		/* foreign server name */
	Oid			srvowner;		/* server owner */
	Oid			srvfdw;			/* server FDW */

	/*
	 * VARIABLE LENGTH FIELDS start here.  These fields may be NULL, too.
	 */
	text		srvtype;
	text		srvversion;
	aclitem		srvacl[1];		/* access permissions */
	text		srvoptions[1];	/* FDW-specific options */
} FormData_pg_foreign_server;

/* ----------------
 *		Form_pg_foreign_server corresponds to a pointer to a tuple with
 *		the format of pg_foreign_server relation.
 * ----------------
 */
typedef FormData_pg_foreign_server *Form_pg_foreign_server;

/* ----------------
 *		compiler constants for pg_foreign_server
 * ----------------
 */

#define Natts_pg_foreign_server					7
#define Anum_pg_foreign_server_srvname			1
#define Anum_pg_foreign_server_srvowner			2
#define Anum_pg_foreign_server_srvfdw			3
#define Anum_pg_foreign_server_srvtype			4
#define Anum_pg_foreign_server_srvversion		5
#define Anum_pg_foreign_server_srvacl			6
#define Anum_pg_foreign_server_srvoptions		7

#endif   /* PG_FOREIGN_SERVER_H */
