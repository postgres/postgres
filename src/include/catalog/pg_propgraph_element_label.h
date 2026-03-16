/*-------------------------------------------------------------------------
 *
 * pg_propgraph_element_label.h
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_propgraph_element_label.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_PROPGRAPH_ELEMENT_LABEL_H
#define PG_PROPGRAPH_ELEMENT_LABEL_H

#include "catalog/genbki.h"
#include "catalog/pg_propgraph_element_label_d.h"

/* ----------------
 *		pg_propgraph_element_label definition.  cpp turns this into
 *		typedef struct FormData_pg_propgraph_element_label
 * ----------------
 */
BEGIN_CATALOG_STRUCT

CATALOG(pg_propgraph_element_label,8305,PropgraphElementLabelRelationId)
{
	Oid			oid;

	/* OID of the label */
	Oid			pgellabelid BKI_LOOKUP(pg_propgraph_label);

	/* OID of the property graph element */
	Oid			pgelelid BKI_LOOKUP(pg_propgraph_element);
} FormData_pg_propgraph_element_label;

END_CATALOG_STRUCT

/* ----------------
 *		Form_pg_propgraph_element_label corresponds to a pointer to a tuple with
 *		the format of pg_propgraph_element_label relation.
 * ----------------
 */
typedef FormData_pg_propgraph_element_label *Form_pg_propgraph_element_label;

DECLARE_UNIQUE_INDEX_PKEY(pg_propgraph_element_label_oid_index, 8312, PropgraphElementLabelObjectIndexId, pg_propgraph_element_label, btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_propgraph_element_label_element_label_index, 8313, PropgraphElementLabelElementLabelIndexId, pg_propgraph_element_label, btree(pgelelid oid_ops, pgellabelid oid_ops));
DECLARE_INDEX(pg_propgraph_element_label_label_index, 8317, PropgraphElementLabelLabelIndexId, pg_propgraph_element_label, btree(pgellabelid oid_ops));

MAKE_SYSCACHE(PROPGRAPHELEMENTLABELELEMENTLABEL, pg_propgraph_element_label_element_label_index, 128);

#endif							/* PG_PROPGRAPH_ELEMENT_LABEL_H */
