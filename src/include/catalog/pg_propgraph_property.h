/*-------------------------------------------------------------------------
 *
 * pg_propgraph_property.h
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_propgraph_property.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_PROPGRAPH_PROPERTY_H
#define PG_PROPGRAPH_PROPERTY_H

#include "catalog/genbki.h"
#include "catalog/pg_propgraph_property_d.h"

/* ----------------
 *		pg_propgraph_property definition.  cpp turns this into
 *		typedef struct FormData_pg_propgraph_property
 * ----------------
 */
BEGIN_CATALOG_STRUCT

CATALOG(pg_propgraph_property,8306,PropgraphPropertyRelationId)
{
	Oid			oid;

	/* OID of the property graph relation */
	Oid			pgppgid BKI_LOOKUP(pg_class);

	/* property name */
	NameData	pgpname;

	/* data type of the property */
	Oid			pgptypid BKI_LOOKUP_OPT(pg_type);

	/* typemod of the property */
	int32		pgptypmod;

	/* collation of the property */
	Oid			pgpcollation BKI_LOOKUP_OPT(pg_collation);
} FormData_pg_propgraph_property;

END_CATALOG_STRUCT

/* ----------------
 *		Form_pg_propgraph_property corresponds to a pointer to a tuple with
 *		the format of pg_propgraph_property relation.
 * ----------------
 */
typedef FormData_pg_propgraph_property *Form_pg_propgraph_property;

DECLARE_UNIQUE_INDEX_PKEY(pg_propgraph_property_oid_index, 8307, PropgraphPropertyObjectIndexId, pg_propgraph_property, btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_propgraph_property_name_index, 8308, PropgraphPropertyNameIndexId, pg_propgraph_property, btree(pgppgid oid_ops, pgpname name_ops));

MAKE_SYSCACHE(PROPGRAPHPROPOID, pg_propgraph_property_oid_index, 128);
MAKE_SYSCACHE(PROPGRAPHPROPNAME, pg_propgraph_property_name_index, 128);

#endif							/* PG_PROPGRAPH_PROPERTY_H */
