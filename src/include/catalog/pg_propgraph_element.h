/*-------------------------------------------------------------------------
 *
 * pg_propgraph_element.h
 *	  definition of the "property graph elements" system catalog (pg_propgraph_element)
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_propgraph_element.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_PROPGRAPH_ELEMENT_H
#define PG_PROPGRAPH_ELEMENT_H

#include "catalog/genbki.h"
#include "catalog/pg_propgraph_element_d.h"

/* ----------------
 *		pg_propgraph_element definition.  cpp turns this into
 *		typedef struct FormData_pg_propgraph_element
 * ----------------
 */
BEGIN_CATALOG_STRUCT

CATALOG(pg_propgraph_element,8299,PropgraphElementRelationId)
{
	Oid			oid;

	/* OID of the property graph relation */
	Oid			pgepgid BKI_LOOKUP(pg_class);

	/* OID of the element table */
	Oid			pgerelid BKI_LOOKUP(pg_class);

	/* element alias */
	NameData	pgealias;

	/* vertex or edge? -- see PGEKIND_* below */
	char		pgekind;

	/* for edges: source vertex */
	Oid			pgesrcvertexid BKI_LOOKUP_OPT(pg_propgraph_element);

	/* for edges: destination vertex */
	Oid			pgedestvertexid BKI_LOOKUP_OPT(pg_propgraph_element);

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	/* element key (column numbers in pgerelid relation) */
	int16		pgekey[1] BKI_FORCE_NOT_NULL;

	/*
	 * for edges: source vertex key (column numbers in pgerelid relation)
	 */
	int16		pgesrckey[1];

	/*
	 * for edges: source vertex table referenced columns (column numbers in
	 * relation reached via pgesrcvertexid)
	 */
	int16		pgesrcref[1];

	/*
	 * for edges: Oids of the equality operators for comparing source keys
	 */
	Oid			pgesrceqop[1];

	/*
	 * for edges: destination vertex key (column numbers in pgerelid relation)
	 */
	int16		pgedestkey[1];

	/*
	 * for edges: destination vertex table referenced columns (column numbers
	 * in relation reached via pgedestvertexid)
	 */
	int16		pgedestref[1];

	/*
	 * for edges: Oids of the equality operators for comparing destination
	 * keys
	 */
	Oid			pgedesteqop[1];
#endif
} FormData_pg_propgraph_element;

END_CATALOG_STRUCT

/* ----------------
 *		Form_pg_propgraph_element corresponds to a pointer to a tuple with
 *		the format of pg_propgraph_element relation.
 * ----------------
 */
typedef FormData_pg_propgraph_element *Form_pg_propgraph_element;

DECLARE_TOAST(pg_propgraph_element, 8315, 8316);

DECLARE_UNIQUE_INDEX_PKEY(pg_propgraph_element_oid_index, 8300, PropgraphElementObjectIndexId, pg_propgraph_element, btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_propgraph_element_alias_index, 8301, PropgraphElementAliasIndexId, pg_propgraph_element, btree(pgepgid oid_ops, pgealias name_ops));

MAKE_SYSCACHE(PROPGRAPHELOID, pg_propgraph_element_oid_index, 128);
MAKE_SYSCACHE(PROPGRAPHELALIAS, pg_propgraph_element_alias_index, 128);

#ifdef EXPOSE_TO_CLIENT_CODE

/*
 * Symbolic values for pgekind column
 */
#define PGEKIND_VERTEX 'v'
#define PGEKIND_EDGE 'e'

#endif							/* EXPOSE_TO_CLIENT_CODE */

#endif							/* PG_PROPGRAPH_ELEMENT_H */
