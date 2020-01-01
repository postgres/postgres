/*-------------------------------------------------------------------------
 *
 * pg_foreign_data_wrapper.h
 *	  definition of the "foreign-data wrapper" system catalog (pg_foreign_data_wrapper)
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_foreign_data_wrapper.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_FOREIGN_DATA_WRAPPER_H
#define PG_FOREIGN_DATA_WRAPPER_H

#include "catalog/genbki.h"
#include "catalog/pg_foreign_data_wrapper_d.h"

/* ----------------
 *		pg_foreign_data_wrapper definition.  cpp turns this into
 *		typedef struct FormData_pg_foreign_data_wrapper
 * ----------------
 */
CATALOG(pg_foreign_data_wrapper,2328,ForeignDataWrapperRelationId)
{
	Oid			oid;			/* oid */
	NameData	fdwname;		/* foreign-data wrapper name */
	Oid			fdwowner;		/* FDW owner */
	Oid			fdwhandler;		/* handler function, or 0 if none */
	Oid			fdwvalidator;	/* option validation function, or 0 if none */

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	aclitem		fdwacl[1];		/* access permissions */
	text		fdwoptions[1];	/* FDW options */
#endif
} FormData_pg_foreign_data_wrapper;

/* ----------------
 *		Form_pg_foreign_data_wrapper corresponds to a pointer to a tuple with
 *		the format of pg_foreign_data_wrapper relation.
 * ----------------
 */
typedef FormData_pg_foreign_data_wrapper *Form_pg_foreign_data_wrapper;

#endif							/* PG_FOREIGN_DATA_WRAPPER_H */
