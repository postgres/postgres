/*-------------------------------------------------------------------------
 *
 * pg_propgraph_label.h
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_propgraph_label.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_PROPGRAPH_LABEL_H
#define PG_PROPGRAPH_LABEL_H

#include "catalog/genbki.h"
#include "catalog/pg_propgraph_label_d.h"

/* ----------------
 *		pg_propgraph_label definition.  cpp turns this into
 *		typedef struct FormData_pg_propgraph_label
 * ----------------
 */
BEGIN_CATALOG_STRUCT

CATALOG(pg_propgraph_label,8303,PropgraphLabelRelationId)
{
	Oid			oid;

	/* OID of the property graph relation */
	Oid			pglpgid BKI_LOOKUP(pg_class);

	/* label name */
	NameData	pgllabel;
} FormData_pg_propgraph_label;

END_CATALOG_STRUCT

/* ----------------
 *		Form_pg_propgraph_label corresponds to a pointer to a tuple with
 *		the format of pg_propgraph_label relation.
 * ----------------
 */
typedef FormData_pg_propgraph_label *Form_pg_propgraph_label;

DECLARE_UNIQUE_INDEX_PKEY(pg_propgraph_label_oid_index, 8304, PropgraphLabelObjectIndexId, pg_propgraph_label, btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_propgraph_label_graph_name_index, 8314, PropgraphLabelGraphNameIndexId, pg_propgraph_label, btree(pglpgid oid_ops, pgllabel name_ops));

MAKE_SYSCACHE(PROPGRAPHLABELOID, pg_propgraph_label_oid_index, 128);
MAKE_SYSCACHE(PROPGRAPHLABELNAME, pg_propgraph_label_graph_name_index, 128);

#endif							/* PG_PROPGRAPH_LABEL_H */
