/*-------------------------------------------------------------------------
 *
 * pg_propgraph_label_property.h
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_propgraph_label_property.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_PROPGRAPH_LABEL_PROPERTY_H
#define PG_PROPGRAPH_LABEL_PROPERTY_H

#include "catalog/genbki.h"
#include "catalog/pg_propgraph_label_property_d.h"

/* ----------------
 *		pg_propgraph_label_property definition.  cpp turns this into
 *		typedef struct FormData_pg_propgraph_label_property
 * ----------------
 */
BEGIN_CATALOG_STRUCT

CATALOG(pg_propgraph_label_property,8318,PropgraphLabelPropertyRelationId)
{
	Oid			oid;

	/* OID of the property */
	Oid			plppropid BKI_LOOKUP(pg_propgraph_property);

	/* OID of the element label */
	Oid			plpellabelid BKI_LOOKUP(pg_propgraph_element_label);

#ifdef CATALOG_VARLEN			/* variable-length fields start here */

	/* property expression */
	pg_node_tree plpexpr BKI_FORCE_NOT_NULL;

#endif
} FormData_pg_propgraph_label_property;

END_CATALOG_STRUCT

/* ----------------
 *		Form_pg_propgraph_label_property corresponds to a pointer to a tuple with
 *		the format of pg_propgraph_label_property relation.
 * ----------------
 */
typedef FormData_pg_propgraph_label_property *Form_pg_propgraph_label_property;

DECLARE_TOAST(pg_propgraph_label_property, 8319, 8320);

DECLARE_UNIQUE_INDEX_PKEY(pg_propgraph_label_property_oid_index, 8328, PropgraphLabelPropertyObjectIndexId, pg_propgraph_label_property, btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_propgraph_label_property_label_prop_index, 8329, PropgraphLabelPropertyLabelPropIndexId, pg_propgraph_label_property, btree(plpellabelid oid_ops, plppropid oid_ops));

MAKE_SYSCACHE(PROPGRAPHLABELPROP, pg_propgraph_label_property_label_prop_index, 128);

#endif							/* PG_PROPGRAPH_LABEL_PROPERTY_H */
