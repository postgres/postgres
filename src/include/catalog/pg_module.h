/*-------------------------------------------------------------------------
 *
 * pg_module.h
 *	  definition of the "module" system catalog (pg_module)
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_module.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives module
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_MODULE_H
#define PG_MODULE_H

#include "catalog/genbki.h"
//#include "catalog/pg_module_d.h"
#include "utils/acl.h"

/* ----------------------------------------------------------------
 *		pg_module definition.
 *
 *		cpp turns this into typedef struct FormData_pg_module
 *
 *	modname				name of the module
 *	modowner			owner (creator) of the module
 *	modacl				access privilege list
 * ----------------------------------------------------------------
 */
CATALOG(pg_module,2635,ModuleRelationId)
{
	Oid			oid;			/* oid */

	NameData	modname;
	Oid			modowner BKI_DEFAULT(POSTGRES) BKI_LOOKUP(pg_authid);

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	aclitem		modacl[1];
#endif
} FormData_pg_module;

/* ----------------
 *		Form_pg_module corresponds to a pointer to a tuple with
 *		the format of pg_module relation.
 * ----------------
 */
typedef FormData_pg_module *Form_pg_module;

DECLARE_UNIQUE_INDEX(pg_module_modname_index, 2714, ModuleNameIndexId, on pg_module using btree(modname name_ops));
DECLARE_UNIQUE_INDEX_PKEY(pg_module_oid_index, 2715, ModuleOidIndexId, on pg_module using btree(oid oid_ops));

/*
 * prototypes for functions in pg_module.c
 */
extern Oid	ModuleCreate(const char *modName, Oid ownerId);

#endif							/* PG_MODULE_H */
