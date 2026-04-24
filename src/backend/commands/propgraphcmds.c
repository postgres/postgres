/*-------------------------------------------------------------------------
 *
 * propgraphcmds.c
 *	  property graph manipulation
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/commands/propgraphcmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_propgraph_element.h"
#include "catalog/pg_propgraph_element_label.h"
#include "catalog/pg_propgraph_label.h"
#include "catalog/pg_propgraph_label_property.h"
#include "catalog/pg_propgraph_property.h"
#include "commands/defrem.h"
#include "commands/propgraphcmds.h"
#include "commands/tablecmds.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/syscache.h"


struct element_info
{
	Oid			elementid;
	char		kind;
	Oid			relid;
	char	   *aliasname;
	ArrayType  *key;

	char	   *srcvertex;
	Oid			srcvertexid;
	Oid			srcrelid;
	ArrayType  *srckey;
	ArrayType  *srcref;
	ArrayType  *srceqop;

	char	   *destvertex;
	Oid			destvertexid;
	Oid			destrelid;
	ArrayType  *destkey;
	ArrayType  *destref;
	ArrayType  *desteqop;

	List	   *labels;
};


static ArrayType *propgraph_element_get_key(ParseState *pstate, const List *key_clause, Relation element_rel,
											const char *aliasname, int location);
static void propgraph_edge_get_ref_keys(ParseState *pstate, const List *keycols, const List *refcols,
										Relation edge_rel, Relation ref_rel,
										const char *aliasname, int location, const char *type,
										ArrayType **outkey, ArrayType **outref, ArrayType **outeqop);
static AttrNumber *array_from_column_list(ParseState *pstate, const List *colnames, int location, Relation element_rel);
static ArrayType *array_from_attnums(int numattrs, const AttrNumber *attnums);
static Oid	insert_element_record(ObjectAddress pgaddress, struct element_info *einfo);
static Oid	insert_label_record(Oid graphid, Oid peoid, const char *label);
static void insert_property_records(Oid graphid, Oid ellabeloid, Oid pgerelid, const PropGraphProperties *properties);
static void insert_property_record(Oid graphid, Oid ellabeloid, Oid pgerelid, const char *propname, const Expr *expr);
static void check_element_properties(Oid peoid);
static void check_element_label_properties(Oid ellabeloid);
static void check_all_labels_properties(Oid pgrelid);
static Oid	get_vertex_oid(ParseState *pstate, Oid pgrelid, const char *alias, int location);
static Oid	get_edge_oid(ParseState *pstate, Oid pgrelid, const char *alias, int location);
static Oid	get_element_relid(Oid peid);
static List *get_graph_label_ids(Oid graphid);
static List *get_label_element_label_ids(Oid labelid);
static List *get_element_label_property_names(Oid ellabeloid);
static List *get_graph_property_ids(Oid graphid);


/*
 * CREATE PROPERTY GRAPH
 */
ObjectAddress
CreatePropGraph(ParseState *pstate, const CreatePropGraphStmt *stmt)
{
	CreateStmt *cstmt = makeNode(CreateStmt);
	char		components_persistence;
	ListCell   *lc;
	ObjectAddress pgaddress;
	List	   *vertex_infos = NIL;
	List	   *edge_infos = NIL;
	List	   *element_aliases = NIL;
	List	   *element_oids = NIL;

	if (stmt->pgname->relpersistence == RELPERSISTENCE_UNLOGGED)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("property graphs cannot be unlogged because they do not have storage")));

	components_persistence = RELPERSISTENCE_PERMANENT;

	foreach(lc, stmt->vertex_tables)
	{
		PropGraphVertex *vertex = lfirst_node(PropGraphVertex, lc);
		struct element_info *vinfo;
		Relation	rel;

		vinfo = palloc0_object(struct element_info);
		vinfo->kind = PGEKIND_VERTEX;

		vinfo->relid = RangeVarGetRelidExtended(vertex->vtable, AccessShareLock, 0, RangeVarCallbackOwnsRelation, NULL);

		rel = table_open(vinfo->relid, NoLock);

		if (rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
			components_persistence = RELPERSISTENCE_TEMP;

		if (vertex->vtable->alias)
			vinfo->aliasname = vertex->vtable->alias->aliasname;
		else
			vinfo->aliasname = vertex->vtable->relname;

		if (list_member(element_aliases, makeString(vinfo->aliasname)))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_TABLE),
					 errmsg("alias \"%s\" used more than once as element table", vinfo->aliasname),
					 parser_errposition(pstate, vertex->location)));

		vinfo->key = propgraph_element_get_key(pstate, vertex->vkey, rel, vinfo->aliasname, vertex->location);

		vinfo->labels = vertex->labels;

		table_close(rel, NoLock);

		vertex_infos = lappend(vertex_infos, vinfo);

		element_aliases = lappend(element_aliases, makeString(vinfo->aliasname));
	}

	foreach(lc, stmt->edge_tables)
	{
		PropGraphEdge *edge = lfirst_node(PropGraphEdge, lc);
		struct element_info *einfo;
		Relation	rel;
		ListCell   *lc2;
		Oid			srcrelid;
		Oid			destrelid;
		Relation	srcrel;
		Relation	destrel;

		einfo = palloc0_object(struct element_info);
		einfo->kind = PGEKIND_EDGE;

		einfo->relid = RangeVarGetRelidExtended(edge->etable, AccessShareLock, 0, RangeVarCallbackOwnsRelation, NULL);

		rel = table_open(einfo->relid, NoLock);

		if (rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
			components_persistence = RELPERSISTENCE_TEMP;

		if (edge->etable->alias)
			einfo->aliasname = edge->etable->alias->aliasname;
		else
			einfo->aliasname = edge->etable->relname;

		if (list_member(element_aliases, makeString(einfo->aliasname)))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_TABLE),
					 errmsg("alias \"%s\" used more than once as element table", einfo->aliasname),
					 parser_errposition(pstate, edge->location)));

		einfo->key = propgraph_element_get_key(pstate, edge->ekey, rel, einfo->aliasname, edge->location);

		einfo->srcvertex = edge->esrcvertex;
		einfo->destvertex = edge->edestvertex;

		srcrelid = 0;
		destrelid = 0;
		foreach(lc2, vertex_infos)
		{
			struct element_info *vinfo = lfirst(lc2);

			if (strcmp(vinfo->aliasname, edge->esrcvertex) == 0)
				srcrelid = vinfo->relid;

			if (strcmp(vinfo->aliasname, edge->edestvertex) == 0)
				destrelid = vinfo->relid;

			if (srcrelid && destrelid)
				break;
		}
		if (!srcrelid)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("source vertex \"%s\" of edge \"%s\" does not exist",
							edge->esrcvertex, einfo->aliasname),
					 parser_errposition(pstate, edge->location)));
		if (!destrelid)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("destination vertex \"%s\" of edge \"%s\" does not exist",
							edge->edestvertex, einfo->aliasname),
					 parser_errposition(pstate, edge->location)));

		srcrel = table_open(srcrelid, NoLock);
		destrel = table_open(destrelid, NoLock);

		propgraph_edge_get_ref_keys(pstate, edge->esrckey, edge->esrcvertexcols, rel, srcrel,
									einfo->aliasname, edge->location, "SOURCE",
									&einfo->srckey, &einfo->srcref, &einfo->srceqop);
		propgraph_edge_get_ref_keys(pstate, edge->edestkey, edge->edestvertexcols, rel, destrel,
									einfo->aliasname, edge->location, "DESTINATION",
									&einfo->destkey, &einfo->destref, &einfo->desteqop);

		einfo->labels = edge->labels;

		table_close(destrel, NoLock);
		table_close(srcrel, NoLock);

		table_close(rel, NoLock);

		edge_infos = lappend(edge_infos, einfo);

		element_aliases = lappend(element_aliases, makeString(einfo->aliasname));
	}

	cstmt->relation = stmt->pgname;
	cstmt->oncommit = ONCOMMIT_NOOP;

	/*
	 * Automatically make it temporary if any component tables are temporary
	 * (see also DefineView()).
	 */
	if (stmt->pgname->relpersistence == RELPERSISTENCE_PERMANENT
		&& components_persistence == RELPERSISTENCE_TEMP)
	{
		cstmt->relation = copyObject(cstmt->relation);
		cstmt->relation->relpersistence = RELPERSISTENCE_TEMP;
		ereport(NOTICE,
				(errmsg("property graph \"%s\" will be temporary",
						stmt->pgname->relname)));
	}

	pgaddress = DefineRelation(cstmt, RELKIND_PROPGRAPH, InvalidOid, NULL, NULL);

	foreach(lc, vertex_infos)
	{
		struct element_info *vinfo = lfirst(lc);
		Oid			peoid;

		peoid = insert_element_record(pgaddress, vinfo);
		element_oids = lappend_oid(element_oids, peoid);
	}

	foreach(lc, edge_infos)
	{
		struct element_info *einfo = lfirst(lc);
		Oid			peoid;
		ListCell   *lc2;

		/*
		 * Look up the vertices again.  Now the vertices have OIDs assigned,
		 * which we need.
		 */
		foreach(lc2, vertex_infos)
		{
			struct element_info *vinfo = lfirst(lc2);

			if (strcmp(vinfo->aliasname, einfo->srcvertex) == 0)
			{
				einfo->srcvertexid = vinfo->elementid;
				einfo->srcrelid = vinfo->relid;
			}
			if (strcmp(vinfo->aliasname, einfo->destvertex) == 0)
			{
				einfo->destvertexid = vinfo->elementid;
				einfo->destrelid = vinfo->relid;
			}
			if (einfo->srcvertexid && einfo->destvertexid)
				break;
		}
		Assert(einfo->srcvertexid);
		Assert(einfo->destvertexid);
		Assert(einfo->srcrelid);
		Assert(einfo->destrelid);
		peoid = insert_element_record(pgaddress, einfo);
		element_oids = lappend_oid(element_oids, peoid);
	}

	CommandCounterIncrement();

	foreach_oid(peoid, element_oids)
		check_element_properties(peoid);
	check_all_labels_properties(pgaddress.objectId);

	return pgaddress;
}

/*
 * Process the key clause specified for an element.  If key_clause is non-NIL,
 * then it is a list of column names.  Otherwise, the primary key of the
 * relation is used.  The return value is an array of column numbers.
 */
static ArrayType *
propgraph_element_get_key(ParseState *pstate, const List *key_clause, Relation element_rel, const char *aliasname, int location)
{
	ArrayType  *a;

	if (key_clause == NIL)
	{
		Oid			pkidx = RelationGetPrimaryKeyIndex(element_rel, false);

		if (!pkidx)
			ereport(ERROR,
					errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					errmsg("no key specified and no suitable primary key exists for definition of element \"%s\"", aliasname),
					parser_errposition(pstate, location));
		else
		{
			Relation	indexDesc;

			indexDesc = index_open(pkidx, AccessShareLock);
			a = array_from_attnums(indexDesc->rd_index->indkey.dim1, indexDesc->rd_index->indkey.values);
			index_close(indexDesc, NoLock);
		}
	}
	else
	{
		a = array_from_attnums(list_length(key_clause),
							   array_from_column_list(pstate, key_clause, location, element_rel));
	}

	return a;
}

/*
 * Process the source or destination link of an edge.
 *
 * keycols and refcols are column names representing the local and referenced
 * (vertex) columns.  If they are both NIL, a matching foreign key is looked
 * up.
 *
 * edge_rel and ref_rel are the local and referenced element tables.
 *
 * aliasname, location, and type are for error messages.  type is either
 * "SOURCE" or "DESTINATION".
 *
 * The outputs are arrays of column numbers in outkey and outref.
 */
static void
propgraph_edge_get_ref_keys(ParseState *pstate, const List *keycols, const List *refcols,
							Relation edge_rel, Relation ref_rel,
							const char *aliasname, int location, const char *type,
							ArrayType **outkey, ArrayType **outref, ArrayType **outeqop)
{
	int			nkeys;
	AttrNumber *keyattnums;
	AttrNumber *refattnums;
	Oid		   *keyeqops;
	Datum	   *datums;

	Assert((keycols && refcols) || (!keycols && !refcols));

	if (keycols)
	{
		if (list_length(keycols) != list_length(refcols))
			ereport(ERROR,
					errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					errmsg("mismatching number of columns in %s vertex definition of edge \"%s\"", type, aliasname),
					parser_errposition(pstate, location));

		nkeys = list_length(keycols);
		keyattnums = array_from_column_list(pstate, keycols, location, edge_rel);
		refattnums = array_from_column_list(pstate, refcols, location, ref_rel);
		keyeqops = palloc_array(Oid, nkeys);

		for (int i = 0; i < nkeys; i++)
		{
			Oid			keytype;
			int32		keytypmod;
			Oid			keycoll;
			Oid			reftype;
			int32		reftypmod;
			Oid			refcoll;
			Oid			opc;
			Oid			opf;
			StrategyNumber strategy;

			/*
			 * Lookup equality operator to be used for edge and vertex key.
			 * Vertex key is equivalent to primary key and edge key is similar
			 * to foreign key since edge key references vertex key. Hence
			 * vertex key is used as left operand and edge key is used as
			 * right operand. The method used to find the equality operators
			 * is similar to the method used to find equality operators for
			 * FK/PK comparison in ATAddForeignKeyConstraint() except that
			 * opclass of the vertex key type is used as a starting point.
			 * Since we need only equality operators we use both BT and HASH
			 * strategies.
			 *
			 * If the required operators do not exist, we can not construct
			 * quals linking an edge to its adjacent vertexes.
			 */
			get_atttypetypmodcoll(RelationGetRelid(edge_rel), keyattnums[i], &keytype, &keytypmod, &keycoll);
			get_atttypetypmodcoll(RelationGetRelid(ref_rel), refattnums[i], &reftype, &reftypmod, &refcoll);
			keyeqops[i] = InvalidOid;
			strategy = BTEqualStrategyNumber;
			opc = GetDefaultOpClass(reftype, BTREE_AM_OID);
			if (!OidIsValid(opc))
			{
				opc = GetDefaultOpClass(reftype, HASH_AM_OID);
				strategy = HTEqualStrategyNumber;
			}
			if (OidIsValid(opc))
			{
				opf = get_opclass_family(opc);
				if (OidIsValid(opf))
				{
					keyeqops[i] = get_opfamily_member(opf, reftype, keytype, strategy);
					if (!OidIsValid(keyeqops[i]))
					{
						/* Last resort, implicit cast. */
						if (can_coerce_type(1, &keytype, &reftype, COERCION_IMPLICIT))
							keyeqops[i] = get_opfamily_member(opf, reftype, reftype, strategy);
					}
				}
			}

			if (!OidIsValid(keyeqops[i]))
				ereport(ERROR,
						errcode(ERRCODE_SYNTAX_ERROR),
						errmsg("no equality operator exists for %s key comparison of edge \"%s\"",
							   type, aliasname),
						parser_errposition(pstate, location));

			/*
			 * If collations of key attribute and referenced attribute are
			 * different, an edge may end up being adjacent to undesired
			 * vertexes.  Prohibit such a case.
			 *
			 * PK/FK allows different collations as long as they are
			 * deterministic for backward compatibility. But we can be a bit
			 * stricter here and follow SQL standard.
			 */
			if (keycoll != refcoll &&
				keycoll != DEFAULT_COLLATION_OID && refcoll != DEFAULT_COLLATION_OID &&
				OidIsValid(keycoll) && OidIsValid(refcoll))
				ereport(ERROR,
						errcode(ERRCODE_SYNTAX_ERROR),
						errmsg("collation mismatch in %s key of edge \"%s\": %s vs. %s",
							   type, aliasname,
							   get_collation_name(keycoll), get_collation_name(refcoll)),
						parser_errposition(pstate, location));
		}
	}
	else
	{
		ForeignKeyCacheInfo *fk = NULL;

		foreach_node(ForeignKeyCacheInfo, tmp, RelationGetFKeyList(edge_rel))
		{
			if (tmp->confrelid == RelationGetRelid(ref_rel))
			{
				if (fk)
					ereport(ERROR,
							errcode(ERRCODE_SYNTAX_ERROR),
							errmsg("more than one suitable foreign key exists for %s key of edge \"%s\"", type, aliasname),
							parser_errposition(pstate, location));
				fk = tmp;
			}
		}

		if (!fk)
			ereport(ERROR,
					errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("no %s key specified and no suitable foreign key exists for definition of edge \"%s\"", type, aliasname),
					parser_errposition(pstate, location));

		nkeys = fk->nkeys;
		keyattnums = fk->conkey;
		refattnums = fk->confkey;
		keyeqops = fk->conpfeqop;
	}

	*outkey = array_from_attnums(nkeys, keyattnums);
	*outref = array_from_attnums(nkeys, refattnums);
	datums = palloc_array(Datum, nkeys);
	for (int i = 0; i < nkeys; i++)
		datums[i] = ObjectIdGetDatum(keyeqops[i]);
	*outeqop = construct_array_builtin(datums, nkeys, OIDOID);
}

/*
 * Convert list of column names in the specified relation into an array of
 * column numbers.
 */
static AttrNumber *
array_from_column_list(ParseState *pstate, const List *colnames, int location, Relation element_rel)
{
	int			numattrs;
	AttrNumber *attnums;
	int			i;
	ListCell   *lc;

	numattrs = list_length(colnames);
	attnums = palloc_array(AttrNumber, numattrs);

	i = 0;
	foreach(lc, colnames)
	{
		char	   *colname = strVal(lfirst(lc));
		Oid			relid = RelationGetRelid(element_rel);
		AttrNumber	attnum;

		attnum = get_attnum(relid, colname);
		if (!attnum)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" of relation \"%s\" does not exist",
							colname, get_rel_name(relid)),
					 parser_errposition(pstate, location)));
		attnums[i++] = attnum;
	}

	for (int j = 0; j < numattrs; j++)
	{
		for (int k = j + 1; k < numattrs; k++)
		{
			if (attnums[j] == attnums[k])
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("graph key columns list must not contain duplicates"),
						 parser_errposition(pstate, location)));
		}
	}

	return attnums;
}

static ArrayType *
array_from_attnums(int numattrs, const AttrNumber *attnums)
{
	Datum	   *attnumsd;

	attnumsd = palloc_array(Datum, numattrs);

	for (int i = 0; i < numattrs; i++)
		attnumsd[i] = Int16GetDatum(attnums[i]);

	return construct_array_builtin(attnumsd, numattrs, INT2OID);
}

static void
array_of_attnums_to_objectaddrs(Oid relid, ArrayType *arr, ObjectAddresses *addrs)
{
	Datum	   *attnumsd;
	int			numattrs;

	deconstruct_array_builtin(arr, INT2OID, &attnumsd, NULL, &numattrs);

	for (int i = 0; i < numattrs; i++)
	{
		ObjectAddress referenced;

		ObjectAddressSubSet(referenced, RelationRelationId, relid, DatumGetInt16(attnumsd[i]));
		add_exact_object_address(&referenced, addrs);
	}
}

static void
array_of_opers_to_objectaddrs(ArrayType *arr, ObjectAddresses *addrs)
{
	Datum	   *opersd;
	int			numopers;

	deconstruct_array_builtin(arr, OIDOID, &opersd, NULL, &numopers);

	for (int i = 0; i < numopers; i++)
	{
		ObjectAddress referenced;

		ObjectAddressSet(referenced, OperatorRelationId, DatumGetObjectId(opersd[i]));
		add_exact_object_address(&referenced, addrs);
	}
}

/*
 * Insert a record for an element into the pg_propgraph_element catalog.  Also
 * inserts labels and properties into their respective catalogs.
 */
static Oid
insert_element_record(ObjectAddress pgaddress, struct element_info *einfo)
{
	Oid			graphid = pgaddress.objectId;
	Relation	rel;
	NameData	aliasname;
	Oid			peoid;
	Datum		values[Natts_pg_propgraph_element] = {0};
	bool		nulls[Natts_pg_propgraph_element] = {0};
	HeapTuple	tup;
	ObjectAddress myself;
	ObjectAddress referenced;
	ObjectAddresses *addrs;

	rel = table_open(PropgraphElementRelationId, RowExclusiveLock);

	peoid = GetNewOidWithIndex(rel, PropgraphElementObjectIndexId, Anum_pg_propgraph_element_oid);
	einfo->elementid = peoid;
	values[Anum_pg_propgraph_element_oid - 1] = ObjectIdGetDatum(peoid);
	values[Anum_pg_propgraph_element_pgepgid - 1] = ObjectIdGetDatum(graphid);
	values[Anum_pg_propgraph_element_pgerelid - 1] = ObjectIdGetDatum(einfo->relid);
	namestrcpy(&aliasname, einfo->aliasname);
	values[Anum_pg_propgraph_element_pgealias - 1] = NameGetDatum(&aliasname);
	values[Anum_pg_propgraph_element_pgekind - 1] = CharGetDatum(einfo->kind);
	values[Anum_pg_propgraph_element_pgesrcvertexid - 1] = ObjectIdGetDatum(einfo->srcvertexid);
	values[Anum_pg_propgraph_element_pgedestvertexid - 1] = ObjectIdGetDatum(einfo->destvertexid);
	values[Anum_pg_propgraph_element_pgekey - 1] = PointerGetDatum(einfo->key);

	if (einfo->srckey)
		values[Anum_pg_propgraph_element_pgesrckey - 1] = PointerGetDatum(einfo->srckey);
	else
		nulls[Anum_pg_propgraph_element_pgesrckey - 1] = true;
	if (einfo->srcref)
		values[Anum_pg_propgraph_element_pgesrcref - 1] = PointerGetDatum(einfo->srcref);
	else
		nulls[Anum_pg_propgraph_element_pgesrcref - 1] = true;
	if (einfo->srceqop)
		values[Anum_pg_propgraph_element_pgesrceqop - 1] = PointerGetDatum(einfo->srceqop);
	else
		nulls[Anum_pg_propgraph_element_pgesrceqop - 1] = true;
	if (einfo->destkey)
		values[Anum_pg_propgraph_element_pgedestkey - 1] = PointerGetDatum(einfo->destkey);
	else
		nulls[Anum_pg_propgraph_element_pgedestkey - 1] = true;
	if (einfo->destref)
		values[Anum_pg_propgraph_element_pgedestref - 1] = PointerGetDatum(einfo->destref);
	else
		nulls[Anum_pg_propgraph_element_pgedestref - 1] = true;
	if (einfo->desteqop)
		values[Anum_pg_propgraph_element_pgedesteqop - 1] = PointerGetDatum(einfo->desteqop);
	else
		nulls[Anum_pg_propgraph_element_pgedesteqop - 1] = true;

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	ObjectAddressSet(myself, PropgraphElementRelationId, peoid);

	/* Add dependency on the property graph */
	recordDependencyOn(&myself, &pgaddress, DEPENDENCY_AUTO);

	addrs = new_object_addresses();

	/* Add dependency on the relation */
	ObjectAddressSet(referenced, RelationRelationId, einfo->relid);
	add_exact_object_address(&referenced, addrs);
	array_of_attnums_to_objectaddrs(einfo->relid, einfo->key, addrs);

	/*
	 * Add dependencies on vertices and equality operators used for key
	 * comparison.
	 */
	if (einfo->srcvertexid)
	{
		ObjectAddressSet(referenced, PropgraphElementRelationId, einfo->srcvertexid);
		add_exact_object_address(&referenced, addrs);
		array_of_attnums_to_objectaddrs(einfo->relid, einfo->srckey, addrs);
		array_of_attnums_to_objectaddrs(einfo->srcrelid, einfo->srcref, addrs);
		array_of_opers_to_objectaddrs(einfo->srceqop, addrs);
	}
	if (einfo->destvertexid)
	{
		ObjectAddressSet(referenced, PropgraphElementRelationId, einfo->destvertexid);
		add_exact_object_address(&referenced, addrs);
		array_of_attnums_to_objectaddrs(einfo->relid, einfo->destkey, addrs);
		array_of_attnums_to_objectaddrs(einfo->destrelid, einfo->destref, addrs);
		array_of_opers_to_objectaddrs(einfo->desteqop, addrs);
	}

	record_object_address_dependencies(&myself, addrs, DEPENDENCY_NORMAL);

	table_close(rel, NoLock);

	if (einfo->labels)
	{
		ListCell   *lc;

		foreach(lc, einfo->labels)
		{
			PropGraphLabelAndProperties *lp = lfirst_node(PropGraphLabelAndProperties, lc);
			Oid			ellabeloid;

			if (lp->label)
				ellabeloid = insert_label_record(graphid, peoid, lp->label);
			else
				ellabeloid = insert_label_record(graphid, peoid, einfo->aliasname);
			insert_property_records(graphid, ellabeloid, einfo->relid, lp->properties);

			CommandCounterIncrement();
		}
	}
	else
	{
		Oid			ellabeloid;
		PropGraphProperties *pr = makeNode(PropGraphProperties);

		pr->all = true;
		pr->location = -1;

		ellabeloid = insert_label_record(graphid, peoid, einfo->aliasname);
		insert_property_records(graphid, ellabeloid, einfo->relid, pr);
	}

	return peoid;
}

/*
 * Insert records for a label into the pg_propgraph_label and
 * pg_propgraph_element_label catalogs, and register dependencies.
 *
 * Returns the OID of the new pg_propgraph_element_label record.
 */
static Oid
insert_label_record(Oid graphid, Oid peoid, const char *label)
{
	Oid			labeloid;
	Oid			ellabeloid;

	/*
	 * Insert into pg_propgraph_label if not already existing.
	 */
	labeloid = GetSysCacheOid2(PROPGRAPHLABELNAME, Anum_pg_propgraph_label_oid, ObjectIdGetDatum(graphid), CStringGetDatum(label));
	if (!labeloid)
	{
		Relation	rel;
		Datum		values[Natts_pg_propgraph_label] = {0};
		bool		nulls[Natts_pg_propgraph_label] = {0};
		NameData	labelname;
		HeapTuple	tup;
		ObjectAddress myself;
		ObjectAddress referenced;

		rel = table_open(PropgraphLabelRelationId, RowExclusiveLock);

		labeloid = GetNewOidWithIndex(rel, PropgraphLabelObjectIndexId, Anum_pg_propgraph_label_oid);
		values[Anum_pg_propgraph_label_oid - 1] = ObjectIdGetDatum(labeloid);
		values[Anum_pg_propgraph_label_pglpgid - 1] = ObjectIdGetDatum(graphid);
		namestrcpy(&labelname, label);
		values[Anum_pg_propgraph_label_pgllabel - 1] = NameGetDatum(&labelname);

		tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
		CatalogTupleInsert(rel, tup);
		heap_freetuple(tup);

		ObjectAddressSet(myself, PropgraphLabelRelationId, labeloid);

		ObjectAddressSet(referenced, RelationRelationId, graphid);
		recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);

		table_close(rel, NoLock);
	}

	/*
	 * Insert into pg_propgraph_element_label
	 */
	{
		Relation	rel;
		Datum		values[Natts_pg_propgraph_element_label] = {0};
		bool		nulls[Natts_pg_propgraph_element_label] = {0};
		HeapTuple	tup;
		ObjectAddress myself;
		ObjectAddress referenced;

		rel = table_open(PropgraphElementLabelRelationId, RowExclusiveLock);

		ellabeloid = GetNewOidWithIndex(rel, PropgraphElementLabelObjectIndexId, Anum_pg_propgraph_element_label_oid);
		values[Anum_pg_propgraph_element_label_oid - 1] = ObjectIdGetDatum(ellabeloid);
		values[Anum_pg_propgraph_element_label_pgellabelid - 1] = ObjectIdGetDatum(labeloid);
		values[Anum_pg_propgraph_element_label_pgelelid - 1] = ObjectIdGetDatum(peoid);

		tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
		CatalogTupleInsert(rel, tup);
		heap_freetuple(tup);

		ObjectAddressSet(myself, PropgraphElementLabelRelationId, ellabeloid);

		ObjectAddressSet(referenced, PropgraphLabelRelationId, labeloid);
		recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);
		ObjectAddressSet(referenced, PropgraphElementRelationId, peoid);
		recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);

		table_close(rel, NoLock);
	}

	return ellabeloid;
}

/*
 * Insert records for properties into the pg_propgraph_property catalog.
 */
static void
insert_property_records(Oid graphid, Oid ellabeloid, Oid pgerelid, const PropGraphProperties *properties)
{
	List	   *proplist = NIL;
	ParseState *pstate;
	ParseNamespaceItem *nsitem;
	List	   *tp;
	Relation	rel;
	ListCell   *lc;

	if (properties->all)
	{
		Relation	attRelation;
		SysScanDesc scan;
		ScanKeyData key[1];
		HeapTuple	attributeTuple;

		attRelation = table_open(AttributeRelationId, RowShareLock);
		ScanKeyInit(&key[0],
					Anum_pg_attribute_attrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(pgerelid));
		scan = systable_beginscan(attRelation, AttributeRelidNumIndexId,
								  true, NULL, 1, key);
		while (HeapTupleIsValid(attributeTuple = systable_getnext(scan)))
		{
			Form_pg_attribute att = (Form_pg_attribute) GETSTRUCT(attributeTuple);
			ColumnRef  *cr;
			ResTarget  *rt;

			if (att->attnum <= 0 || att->attisdropped)
				continue;

			cr = makeNode(ColumnRef);
			rt = makeNode(ResTarget);

			cr->fields = list_make1(makeString(pstrdup(NameStr(att->attname))));
			cr->location = -1;

			rt->name = pstrdup(NameStr(att->attname));
			rt->val = (Node *) cr;
			rt->location = -1;

			proplist = lappend(proplist, rt);
		}
		systable_endscan(scan);
		table_close(attRelation, RowShareLock);
	}
	else
	{
		proplist = properties->properties;

		foreach(lc, proplist)
		{
			ResTarget  *rt = lfirst_node(ResTarget, lc);

			if (!rt->name && !IsA(rt->val, ColumnRef))
				ereport(ERROR,
						errcode(ERRCODE_SYNTAX_ERROR),
						errmsg("property name required"),
						parser_errposition(NULL, rt->location));
		}
	}

	rel = table_open(pgerelid, AccessShareLock);

	pstate = make_parsestate(NULL);
	nsitem = addRangeTableEntryForRelation(pstate,
										   rel,
										   AccessShareLock,
										   NULL,
										   false,
										   true);
	addNSItemToQuery(pstate, nsitem, true, true, true);

	table_close(rel, NoLock);

	tp = transformTargetList(pstate, proplist, EXPR_KIND_PROPGRAPH_PROPERTY);
	assign_expr_collations(pstate, (Node *) tp);

	foreach(lc, tp)
	{
		TargetEntry *te = lfirst_node(TargetEntry, lc);

		insert_property_record(graphid, ellabeloid, pgerelid, te->resname, te->expr);
	}
}

/*
 * Insert records for a property into the pg_propgraph_property and
 * pg_propgraph_label_property catalogs, and register dependencies.
 */
static void
insert_property_record(Oid graphid, Oid ellabeloid, Oid pgerelid, const char *propname, const Expr *expr)
{
	Oid			propoid;
	Oid			exprtypid = exprType((const Node *) expr);
	int32		exprtypmod = exprTypmod((const Node *) expr);
	Oid			exprcollation = exprCollation((const Node *) expr);

	/*
	 * Insert into pg_propgraph_property if not already existing.
	 */
	propoid = GetSysCacheOid2(PROPGRAPHPROPNAME, Anum_pg_propgraph_property_oid, ObjectIdGetDatum(graphid), CStringGetDatum(propname));
	if (!OidIsValid(propoid))
	{
		Relation	rel;
		NameData	propnamedata;
		Datum		values[Natts_pg_propgraph_property] = {0};
		bool		nulls[Natts_pg_propgraph_property] = {0};
		HeapTuple	tup;
		ObjectAddress myself;
		ObjectAddress referenced;

		rel = table_open(PropgraphPropertyRelationId, RowExclusiveLock);

		propoid = GetNewOidWithIndex(rel, PropgraphPropertyObjectIndexId, Anum_pg_propgraph_property_oid);
		values[Anum_pg_propgraph_property_oid - 1] = ObjectIdGetDatum(propoid);
		values[Anum_pg_propgraph_property_pgppgid - 1] = ObjectIdGetDatum(graphid);
		namestrcpy(&propnamedata, propname);
		values[Anum_pg_propgraph_property_pgpname - 1] = NameGetDatum(&propnamedata);
		values[Anum_pg_propgraph_property_pgptypid - 1] = ObjectIdGetDatum(exprtypid);
		values[Anum_pg_propgraph_property_pgptypmod - 1] = Int32GetDatum(exprtypmod);
		values[Anum_pg_propgraph_property_pgpcollation - 1] = ObjectIdGetDatum(exprcollation);

		tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
		CatalogTupleInsert(rel, tup);
		heap_freetuple(tup);

		ObjectAddressSet(myself, PropgraphPropertyRelationId, propoid);

		ObjectAddressSet(referenced, RelationRelationId, graphid);
		recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);
		ObjectAddressSet(referenced, TypeRelationId, exprtypid);
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
		if (OidIsValid(exprcollation) && exprcollation != DEFAULT_COLLATION_OID)
		{
			ObjectAddressSet(referenced, CollationRelationId, exprcollation);
			recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
		}

		table_close(rel, NoLock);
	}
	else
	{
		HeapTuple	pgptup = SearchSysCache1(PROPGRAPHPROPOID, ObjectIdGetDatum(propoid));
		Form_pg_propgraph_property pgpform = (Form_pg_propgraph_property) GETSTRUCT(pgptup);
		Oid			proptypid = pgpform->pgptypid;
		int32		proptypmod = pgpform->pgptypmod;
		Oid			propcollation = pgpform->pgpcollation;

		ReleaseSysCache(pgptup);

		/*
		 * Check that in the graph, all properties with the same name have the
		 * same type (independent of which label they are on).  (See SQL/PGQ
		 * subclause "Consistency check of a tabular property graph
		 * descriptor".)
		 */
		if (proptypid != exprtypid || proptypmod != exprtypmod)
		{
			ereport(ERROR,
					errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("property \"%s\" data type mismatch: %s vs. %s",
						   propname, format_type_with_typemod(proptypid, proptypmod), format_type_with_typemod(exprtypid, exprtypmod)),
					errdetail("In a property graph, a property of the same name has to have the same data type in each label."));
		}

		/* Similarly for collation */
		if (propcollation != exprcollation)
		{
			ereport(ERROR,
					errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("property \"%s\" collation mismatch: %s vs. %s",
						   propname, get_collation_name(propcollation), get_collation_name(exprcollation)),
					errdetail("In a property graph, a property of the same name has to have the same collation in each label."));
		}
	}

	/*
	 * Insert into pg_propgraph_label_property
	 */
	{
		Relation	rel;
		Datum		values[Natts_pg_propgraph_label_property] = {0};
		bool		nulls[Natts_pg_propgraph_label_property] = {0};
		Oid			plpoid;
		HeapTuple	tup;
		ObjectAddress myself;
		ObjectAddress referenced;

		rel = table_open(PropgraphLabelPropertyRelationId, RowExclusiveLock);

		plpoid = GetNewOidWithIndex(rel, PropgraphLabelPropertyObjectIndexId, Anum_pg_propgraph_label_property_oid);
		values[Anum_pg_propgraph_label_property_oid - 1] = ObjectIdGetDatum(plpoid);
		values[Anum_pg_propgraph_label_property_plppropid - 1] = ObjectIdGetDatum(propoid);
		values[Anum_pg_propgraph_label_property_plpellabelid - 1] = ObjectIdGetDatum(ellabeloid);
		values[Anum_pg_propgraph_label_property_plpexpr - 1] = CStringGetTextDatum(nodeToString(expr));

		tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
		CatalogTupleInsert(rel, tup);
		heap_freetuple(tup);

		ObjectAddressSet(myself, PropgraphLabelPropertyRelationId, plpoid);

		ObjectAddressSet(referenced, PropgraphPropertyRelationId, propoid);
		recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);

		ObjectAddressSet(referenced, PropgraphElementLabelRelationId, ellabeloid);
		recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);

		recordDependencyOnSingleRelExpr(&myself, (Node *) copyObject(expr), pgerelid, DEPENDENCY_NORMAL, DEPENDENCY_NORMAL, false);

		table_close(rel, NoLock);
	}
}

/*
 * Check that for the given graph element, all properties with the same name
 * have the same expression for each label.  (See SQL/PGQ subclause "Creation
 * of an element table descriptor".)
 *
 * We check this after all the catalog records are already inserted.  This
 * makes it easier to share this code between CREATE PROPERTY GRAPH and ALTER
 * PROPERTY GRAPH.  We pass in the element OID so that ALTER PROPERTY GRAPH
 * only has to check the element it has just operated on.  CREATE PROPERTY
 * GRAPH checks all elements it has created.
 */
static void
check_element_properties(Oid peoid)
{
	Relation	rel1;
	ScanKeyData key1[1];
	SysScanDesc scan1;
	HeapTuple	tuple1;
	List	   *propoids = NIL;
	List	   *propexprs = NIL;

	rel1 = table_open(PropgraphElementLabelRelationId, AccessShareLock);
	ScanKeyInit(&key1[0],
				Anum_pg_propgraph_element_label_pgelelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(peoid));

	scan1 = systable_beginscan(rel1, PropgraphElementLabelElementLabelIndexId, true, NULL, 1, key1);
	while (HeapTupleIsValid(tuple1 = systable_getnext(scan1)))
	{
		Form_pg_propgraph_element_label ellabel = (Form_pg_propgraph_element_label) GETSTRUCT(tuple1);
		Relation	rel2;
		ScanKeyData key2[1];
		SysScanDesc scan2;
		HeapTuple	tuple2;

		rel2 = table_open(PropgraphLabelPropertyRelationId, AccessShareLock);
		ScanKeyInit(&key2[0],
					Anum_pg_propgraph_label_property_plpellabelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(ellabel->oid));

		scan2 = systable_beginscan(rel2, PropgraphLabelPropertyLabelPropIndexId, true, NULL, 1, key2);
		while (HeapTupleIsValid(tuple2 = systable_getnext(scan2)))
		{
			Form_pg_propgraph_label_property lprop = (Form_pg_propgraph_label_property) GETSTRUCT(tuple2);
			Oid			propoid;
			Datum		datum;
			bool		isnull;
			char	   *propexpr;
			ListCell   *lc1,
					   *lc2;
			bool		found;

			propoid = lprop->plppropid;
			datum = heap_getattr(tuple2, Anum_pg_propgraph_label_property_plpexpr, RelationGetDescr(rel2), &isnull);
			Assert(!isnull);
			propexpr = TextDatumGetCString(datum);

			found = false;
			forboth(lc1, propoids, lc2, propexprs)
			{
				if (propoid == lfirst_oid(lc1))
				{
					Node	   *na,
							   *nb;

					na = stringToNode(propexpr);
					nb = stringToNode(lfirst(lc2));

					found = true;

					if (!equal(na, nb))
					{
						HeapTuple	tuple3;
						Form_pg_propgraph_element elform;
						List	   *dpcontext;
						char	   *dpa,
								   *dpb;

						tuple3 = SearchSysCache1(PROPGRAPHELOID, ObjectIdGetDatum(peoid));
						if (!tuple3)
							elog(ERROR, "cache lookup failed for property graph element %u", peoid);
						elform = (Form_pg_propgraph_element) GETSTRUCT(tuple3);
						dpcontext = deparse_context_for(get_rel_name(elform->pgerelid), elform->pgerelid);

						dpa = deparse_expression(na, dpcontext, false, false);
						dpb = deparse_expression(nb, dpcontext, false, false);

						/*
						 * show in sorted order to keep output independent of
						 * index order
						 */
						if (strcmp(dpa, dpb) > 0)
						{
							char	   *tmp;

							tmp = dpa;
							dpa = dpb;
							dpb = tmp;
						}

						ereport(ERROR,
								errcode(ERRCODE_SYNTAX_ERROR),
								errmsg("element \"%s\" property \"%s\" expression mismatch: %s vs. %s",
									   NameStr(elform->pgealias), get_propgraph_property_name(propoid), dpa, dpb),
								errdetail("In a property graph element, a property of the same name has to have the same expression in each label."));

						ReleaseSysCache(tuple3);
					}

					break;
				}
			}

			if (!found)
			{
				propoids = lappend_oid(propoids, propoid);
				propexprs = lappend(propexprs, propexpr);
			}
		}
		systable_endscan(scan2);
		table_close(rel2, AccessShareLock);
	}

	systable_endscan(scan1);
	table_close(rel1, AccessShareLock);
}

/*
 * Check that for the given element label, all labels of the same name in the
 * graph have the same number and names of properties (independent of which
 * element they are on).  (See SQL/PGQ subclause "Consistency check of a
 * tabular property graph descriptor".)
 *
 * We check this after all the catalog records are already inserted.  This
 * makes it easier to share this code between CREATE PROPERTY GRAPH and ALTER
 * PROPERTY GRAPH.  We pass in the element label OID so that some variants of
 * ALTER PROPERTY GRAPH only have to check the element label it has just
 * operated on.  CREATE PROPERTY GRAPH and other ALTER PROPERTY GRAPH variants
 * check all labels.
 */
static void
check_element_label_properties(Oid ellabeloid)
{
	Relation	rel;
	SysScanDesc scan;
	ScanKeyData key[1];
	HeapTuple	tuple;
	Oid			labelid = InvalidOid;
	Oid			ref_ellabeloid = InvalidOid;
	List	   *myprops,
			   *refprops;
	List	   *diff1,
			   *diff2;

	rel = table_open(PropgraphElementLabelRelationId, AccessShareLock);

	/*
	 * Get element label info
	 */
	ScanKeyInit(&key[0],
				Anum_pg_propgraph_element_label_oid,
				BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(ellabeloid));
	scan = systable_beginscan(rel, PropgraphElementLabelObjectIndexId, true, NULL, 1, key);
	if (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_propgraph_element_label ellabel = (Form_pg_propgraph_element_label) GETSTRUCT(tuple);

		labelid = ellabel->pgellabelid;
	}
	systable_endscan(scan);
	if (!labelid)
		elog(ERROR, "element label %u not found", ellabeloid);

	/*
	 * Find a reference element label to fetch label properties.  The
	 * reference element label has to have the same label OID as the one being
	 * checked but a different element OID.
	 */
	ScanKeyInit(&key[0],
				Anum_pg_propgraph_element_label_pgellabelid,
				BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(labelid));
	scan = systable_beginscan(rel, PropgraphElementLabelLabelIndexId, true, NULL, 1, key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_propgraph_element_label otherellabel = (Form_pg_propgraph_element_label) GETSTRUCT(tuple);

		if (otherellabel->oid != ellabeloid)
		{
			ref_ellabeloid = otherellabel->oid;
			break;
		}
	}
	systable_endscan(scan);

	table_close(rel, AccessShareLock);

	/*
	 * If there is no previous definition of this label, then we are done.
	 */
	if (!ref_ellabeloid)
		return;

	/*
	 * Now check number and names.
	 *
	 * XXX We could provide more detail in the error messages, but that would
	 * probably only be useful for some ALTER commands, because otherwise it's
	 * not really clear which label definition is the wrong one, and so you'd
	 * have to construct a rather verbose report to be of any use.  Let's keep
	 * it simple for now.
	 */

	myprops = get_element_label_property_names(ellabeloid);
	refprops = get_element_label_property_names(ref_ellabeloid);

	if (list_length(refprops) != list_length(myprops))
		ereport(ERROR,
				errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				errmsg("mismatching number of properties in definition of label \"%s\"", get_propgraph_label_name(labelid)));

	diff1 = list_difference(myprops, refprops);
	diff2 = list_difference(refprops, myprops);

	if (diff1 || diff2)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				errmsg("mismatching properties names in definition of label \"%s\"", get_propgraph_label_name(labelid)));
}

/*
 * As above, but check all labels of a graph.
 */
static void
check_all_labels_properties(Oid pgrelid)
{
	foreach_oid(labeloid, get_graph_label_ids(pgrelid))
	{
		foreach_oid(ellabeloid, get_label_element_label_ids(labeloid))
		{
			check_element_label_properties(ellabeloid);
		}
	}
}

/*
 * ALTER PROPERTY GRAPH
 */
ObjectAddress
AlterPropGraph(ParseState *pstate, const AlterPropGraphStmt *stmt)
{
	Oid			pgrelid;
	ListCell   *lc;
	ObjectAddress pgaddress;

	pgrelid = RangeVarGetRelidExtended(stmt->pgname,
									   ShareRowExclusiveLock,
									   stmt->missing_ok ? RVR_MISSING_OK : 0,
									   RangeVarCallbackOwnsRelation,
									   NULL);
	if (pgrelid == InvalidOid)
	{
		ereport(NOTICE,
				(errmsg("relation \"%s\" does not exist, skipping",
						stmt->pgname->relname)));
		return InvalidObjectAddress;
	}

	ObjectAddressSet(pgaddress, RelationRelationId, pgrelid);

	foreach(lc, stmt->add_vertex_tables)
	{
		PropGraphVertex *vertex = lfirst_node(PropGraphVertex, lc);
		struct element_info *vinfo;
		Relation	rel;
		Oid			peoid;

		vinfo = palloc0_object(struct element_info);
		vinfo->kind = PGEKIND_VERTEX;

		vinfo->relid = RangeVarGetRelidExtended(vertex->vtable, AccessShareLock, 0, RangeVarCallbackOwnsRelation, NULL);

		rel = table_open(vinfo->relid, NoLock);

		if (rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP && get_rel_persistence(pgrelid) != RELPERSISTENCE_TEMP)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cannot add temporary element table to non-temporary property graph"),
					 errdetail("Table \"%s\" is a temporary table.", get_rel_name(vinfo->relid)),
					 parser_errposition(pstate, vertex->vtable->location)));

		if (vertex->vtable->alias)
			vinfo->aliasname = vertex->vtable->alias->aliasname;
		else
			vinfo->aliasname = vertex->vtable->relname;

		vinfo->key = propgraph_element_get_key(pstate, vertex->vkey, rel, vinfo->aliasname, vertex->location);

		vinfo->labels = vertex->labels;

		table_close(rel, NoLock);

		if (SearchSysCacheExists2(PROPGRAPHELALIAS,
								  ObjectIdGetDatum(pgrelid),
								  CStringGetDatum(vinfo->aliasname)))
			ereport(ERROR,
					errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					errmsg("alias \"%s\" already exists in property graph \"%s\"",
						   vinfo->aliasname, stmt->pgname->relname),
					parser_errposition(pstate, vertex->vtable->location));

		peoid = insert_element_record(pgaddress, vinfo);

		CommandCounterIncrement();
		check_element_properties(peoid);
		check_all_labels_properties(pgrelid);
	}

	foreach(lc, stmt->add_edge_tables)
	{
		PropGraphEdge *edge = lfirst_node(PropGraphEdge, lc);
		struct element_info *einfo;
		Relation	rel;
		Relation	srcrel;
		Relation	destrel;
		Oid			peoid;

		einfo = palloc0_object(struct element_info);
		einfo->kind = PGEKIND_EDGE;

		einfo->relid = RangeVarGetRelidExtended(edge->etable, AccessShareLock, 0, RangeVarCallbackOwnsRelation, NULL);

		rel = table_open(einfo->relid, NoLock);

		if (rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP && get_rel_persistence(pgrelid) != RELPERSISTENCE_TEMP)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cannot add temporary element table to non-temporary property graph"),
					 errdetail("Table \"%s\" is a temporary table.", get_rel_name(einfo->relid)),
					 parser_errposition(pstate, edge->etable->location)));

		if (edge->etable->alias)
			einfo->aliasname = edge->etable->alias->aliasname;
		else
			einfo->aliasname = edge->etable->relname;

		einfo->key = propgraph_element_get_key(pstate, edge->ekey, rel, einfo->aliasname, edge->location);

		einfo->srcvertexid = get_vertex_oid(pstate, pgrelid, edge->esrcvertex, edge->location);
		einfo->destvertexid = get_vertex_oid(pstate, pgrelid, edge->edestvertex, edge->location);

		einfo->srcrelid = get_element_relid(einfo->srcvertexid);
		einfo->destrelid = get_element_relid(einfo->destvertexid);

		srcrel = table_open(einfo->srcrelid, AccessShareLock);
		destrel = table_open(einfo->destrelid, AccessShareLock);

		propgraph_edge_get_ref_keys(pstate, edge->esrckey, edge->esrcvertexcols, rel, srcrel,
									einfo->aliasname, edge->location, "SOURCE",
									&einfo->srckey, &einfo->srcref, &einfo->srceqop);
		propgraph_edge_get_ref_keys(pstate, edge->edestkey, edge->edestvertexcols, rel, destrel,
									einfo->aliasname, edge->location, "DESTINATION",
									&einfo->destkey, &einfo->destref, &einfo->desteqop);

		einfo->labels = edge->labels;

		table_close(destrel, NoLock);
		table_close(srcrel, NoLock);

		table_close(rel, NoLock);

		if (SearchSysCacheExists2(PROPGRAPHELALIAS,
								  ObjectIdGetDatum(pgrelid),
								  CStringGetDatum(einfo->aliasname)))
			ereport(ERROR,
					errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					errmsg("alias \"%s\" already exists in property graph \"%s\"",
						   einfo->aliasname, stmt->pgname->relname),
					parser_errposition(pstate, edge->etable->location));

		peoid = insert_element_record(pgaddress, einfo);

		CommandCounterIncrement();
		check_element_properties(peoid);
		check_all_labels_properties(pgrelid);
	}

	foreach(lc, stmt->drop_vertex_tables)
	{
		char	   *alias = strVal(lfirst(lc));
		Oid			peoid;
		ObjectAddress obj;

		peoid = get_vertex_oid(pstate, pgrelid, alias, -1);
		ObjectAddressSet(obj, PropgraphElementRelationId, peoid);
		performDeletion(&obj, stmt->drop_behavior, 0);
	}

	foreach(lc, stmt->drop_edge_tables)
	{
		char	   *alias = strVal(lfirst(lc));
		Oid			peoid;
		ObjectAddress obj;

		peoid = get_edge_oid(pstate, pgrelid, alias, -1);
		ObjectAddressSet(obj, PropgraphElementRelationId, peoid);
		performDeletion(&obj, stmt->drop_behavior, 0);
	}

	/* Remove any orphaned pg_propgraph_label entries */
	if (stmt->drop_vertex_tables || stmt->drop_edge_tables)
	{
		foreach_oid(labeloid, get_graph_label_ids(pgrelid))
		{
			if (!get_label_element_label_ids(labeloid))
			{
				ObjectAddress obj;

				ObjectAddressSet(obj, PropgraphLabelRelationId, labeloid);
				performDeletion(&obj, stmt->drop_behavior, 0);
			}
		}
	}

	foreach(lc, stmt->add_labels)
	{
		PropGraphLabelAndProperties *lp = lfirst_node(PropGraphLabelAndProperties, lc);
		Oid			peoid;
		Oid			pgerelid;
		Oid			ellabeloid;

		Assert(lp->label);

		if (stmt->element_kind == PROPGRAPH_ELEMENT_KIND_VERTEX)
			peoid = get_vertex_oid(pstate, pgrelid, stmt->element_alias, -1);
		else
			peoid = get_edge_oid(pstate, pgrelid, stmt->element_alias, -1);

		pgerelid = get_element_relid(peoid);

		ellabeloid = insert_label_record(pgrelid, peoid, lp->label);
		insert_property_records(pgrelid, ellabeloid, pgerelid, lp->properties);

		CommandCounterIncrement();
		check_element_properties(peoid);
		check_element_label_properties(ellabeloid);
	}

	if (stmt->drop_label)
	{
		Oid			peoid;
		Oid			labeloid;
		Oid			ellabeloid;
		ObjectAddress obj;

		if (stmt->element_kind == PROPGRAPH_ELEMENT_KIND_VERTEX)
			peoid = get_vertex_oid(pstate, pgrelid, stmt->element_alias, -1);
		else
			peoid = get_edge_oid(pstate, pgrelid, stmt->element_alias, -1);

		labeloid = GetSysCacheOid2(PROPGRAPHLABELNAME,
								   Anum_pg_propgraph_label_oid,
								   ObjectIdGetDatum(pgrelid),
								   CStringGetDatum(stmt->drop_label));
		if (!labeloid)
			ereport(ERROR,
					errcode(ERRCODE_UNDEFINED_OBJECT),
					errmsg("property graph \"%s\" element \"%s\" has no label \"%s\"",
						   get_rel_name(pgrelid), stmt->element_alias, stmt->drop_label),
					parser_errposition(pstate, -1));

		ellabeloid = GetSysCacheOid2(PROPGRAPHELEMENTLABELELEMENTLABEL,
									 Anum_pg_propgraph_element_label_oid,
									 ObjectIdGetDatum(peoid),
									 ObjectIdGetDatum(labeloid));

		if (!ellabeloid)
			ereport(ERROR,
					errcode(ERRCODE_UNDEFINED_OBJECT),
					errmsg("property graph \"%s\" element \"%s\" has no label \"%s\"",
						   get_rel_name(pgrelid), stmt->element_alias, stmt->drop_label),
					parser_errposition(pstate, -1));

		ObjectAddressSet(obj, PropgraphElementLabelRelationId, ellabeloid);
		performDeletion(&obj, stmt->drop_behavior, 0);

		/* Remove any orphaned pg_propgraph_label entries */
		if (!get_label_element_label_ids(labeloid))
		{
			ObjectAddressSet(obj, PropgraphLabelRelationId, labeloid);
			performDeletion(&obj, stmt->drop_behavior, 0);
		}
	}

	if (stmt->add_properties)
	{
		Oid			peoid;
		Oid			pgerelid;
		Oid			labeloid;
		Oid			ellabeloid;

		if (stmt->element_kind == PROPGRAPH_ELEMENT_KIND_VERTEX)
			peoid = get_vertex_oid(pstate, pgrelid, stmt->element_alias, -1);
		else
			peoid = get_edge_oid(pstate, pgrelid, stmt->element_alias, -1);

		labeloid = GetSysCacheOid2(PROPGRAPHLABELNAME,
								   Anum_pg_propgraph_label_oid,
								   ObjectIdGetDatum(pgrelid),
								   CStringGetDatum(stmt->alter_label));
		if (!labeloid)
			ereport(ERROR,
					errcode(ERRCODE_UNDEFINED_OBJECT),
					errmsg("property graph \"%s\" element \"%s\" has no label \"%s\"",
						   get_rel_name(pgrelid), stmt->element_alias, stmt->alter_label),
					parser_errposition(pstate, -1));

		ellabeloid = GetSysCacheOid2(PROPGRAPHELEMENTLABELELEMENTLABEL,
									 Anum_pg_propgraph_element_label_oid,
									 ObjectIdGetDatum(peoid),
									 ObjectIdGetDatum(labeloid));
		if (!ellabeloid)
			ereport(ERROR,
					errcode(ERRCODE_UNDEFINED_OBJECT),
					errmsg("property graph \"%s\" element \"%s\" has no label \"%s\"",
						   get_rel_name(pgrelid), stmt->element_alias, stmt->alter_label),
					parser_errposition(pstate, -1));

		pgerelid = get_element_relid(peoid);

		insert_property_records(pgrelid, ellabeloid, pgerelid, stmt->add_properties);

		CommandCounterIncrement();
		check_element_properties(peoid);
		check_element_label_properties(ellabeloid);
	}

	if (stmt->drop_properties)
	{
		Oid			peoid;
		Oid			labeloid;
		Oid			ellabeloid;
		ObjectAddress obj;

		if (stmt->element_kind == PROPGRAPH_ELEMENT_KIND_VERTEX)
			peoid = get_vertex_oid(pstate, pgrelid, stmt->element_alias, -1);
		else
			peoid = get_edge_oid(pstate, pgrelid, stmt->element_alias, -1);

		labeloid = GetSysCacheOid2(PROPGRAPHLABELNAME,
								   Anum_pg_propgraph_label_oid,
								   ObjectIdGetDatum(pgrelid),
								   CStringGetDatum(stmt->alter_label));
		if (!labeloid)
			ereport(ERROR,
					errcode(ERRCODE_UNDEFINED_OBJECT),
					errmsg("property graph \"%s\" element \"%s\" has no label \"%s\"",
						   get_rel_name(pgrelid), stmt->element_alias, stmt->alter_label),
					parser_errposition(pstate, -1));

		ellabeloid = GetSysCacheOid2(PROPGRAPHELEMENTLABELELEMENTLABEL,
									 Anum_pg_propgraph_element_label_oid,
									 ObjectIdGetDatum(peoid),
									 ObjectIdGetDatum(labeloid));

		if (!ellabeloid)
			ereport(ERROR,
					errcode(ERRCODE_UNDEFINED_OBJECT),
					errmsg("property graph \"%s\" element \"%s\" has no label \"%s\"",
						   get_rel_name(pgrelid), stmt->element_alias, stmt->alter_label),
					parser_errposition(pstate, -1));

		foreach(lc, stmt->drop_properties)
		{
			char	   *propname = strVal(lfirst(lc));
			Oid			propoid;
			Oid			plpoid;

			propoid = GetSysCacheOid2(PROPGRAPHPROPNAME,
									  Anum_pg_propgraph_property_oid,
									  ObjectIdGetDatum(pgrelid),
									  CStringGetDatum(propname));
			if (!propoid)
				ereport(ERROR,
						errcode(ERRCODE_UNDEFINED_OBJECT),
						errmsg("property graph \"%s\" element \"%s\" label \"%s\" has no property \"%s\"",
							   get_rel_name(pgrelid), stmt->element_alias, stmt->alter_label, propname),
						parser_errposition(pstate, -1));

			plpoid = GetSysCacheOid2(PROPGRAPHLABELPROP, Anum_pg_propgraph_label_property_oid, ObjectIdGetDatum(ellabeloid), ObjectIdGetDatum(propoid));

			ObjectAddressSet(obj, PropgraphLabelPropertyRelationId, plpoid);
			performDeletion(&obj, stmt->drop_behavior, 0);
		}

		check_element_label_properties(ellabeloid);
	}

	/* Remove any orphaned pg_propgraph_property entries */
	if (stmt->drop_properties || stmt->drop_vertex_tables || stmt->drop_edge_tables)
	{
		foreach_oid(propoid, get_graph_property_ids(pgrelid))
		{
			Relation	rel;
			SysScanDesc scan;
			ScanKeyData key[1];

			rel = table_open(PropgraphLabelPropertyRelationId, RowShareLock);
			ScanKeyInit(&key[0],
						Anum_pg_propgraph_label_property_plppropid,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(propoid));
			/* XXX no suitable index */
			scan = systable_beginscan(rel, InvalidOid, true, NULL, 1, key);
			if (!systable_getnext(scan))
			{
				ObjectAddress obj;

				ObjectAddressSet(obj, PropgraphPropertyRelationId, propoid);
				performDeletion(&obj, stmt->drop_behavior, 0);
			}

			systable_endscan(scan);
			table_close(rel, RowShareLock);
		}
	}

	/*
	 * Invalidate relcache entry of the property graph so that the queries in
	 * the cached plans referencing the property graph will be rewritten
	 * considering changes to the property graph.
	 */
	CacheInvalidateRelcacheByRelid(pgrelid);

	return pgaddress;
}

/*
 * Get OID of vertex from graph OID and element alias.  Element must be a
 * vertex, otherwise error.
 */
static Oid
get_vertex_oid(ParseState *pstate, Oid pgrelid, const char *alias, int location)
{
	HeapTuple	tuple;
	Oid			peoid;

	tuple = SearchSysCache2(PROPGRAPHELALIAS, ObjectIdGetDatum(pgrelid), CStringGetDatum(alias));
	if (!tuple)
		ereport(ERROR,
				errcode(ERRCODE_UNDEFINED_OBJECT),
				errmsg("property graph \"%s\" has no element with alias \"%s\"",
					   get_rel_name(pgrelid), alias),
				parser_errposition(pstate, location));

	if (((Form_pg_propgraph_element) GETSTRUCT(tuple))->pgekind != PGEKIND_VERTEX)
		ereport(ERROR,
				errcode(ERRCODE_SYNTAX_ERROR),
				errmsg("element \"%s\" of property graph \"%s\" is not a vertex",
					   alias, get_rel_name(pgrelid)),
				parser_errposition(pstate, location));

	peoid = ((Form_pg_propgraph_element) GETSTRUCT(tuple))->oid;

	ReleaseSysCache(tuple);

	return peoid;
}

/*
 * Get OID of edge from graph OID and element alias.  Element must be an edge,
 * otherwise error.
 */
static Oid
get_edge_oid(ParseState *pstate, Oid pgrelid, const char *alias, int location)
{
	HeapTuple	tuple;
	Oid			peoid;

	tuple = SearchSysCache2(PROPGRAPHELALIAS, ObjectIdGetDatum(pgrelid), CStringGetDatum(alias));
	if (!tuple)
		ereport(ERROR,
				errcode(ERRCODE_UNDEFINED_OBJECT),
				errmsg("property graph \"%s\" has no element with alias \"%s\"",
					   get_rel_name(pgrelid), alias),
				parser_errposition(pstate, location));

	if (((Form_pg_propgraph_element) GETSTRUCT(tuple))->pgekind != PGEKIND_EDGE)
		ereport(ERROR,
				errcode(ERRCODE_SYNTAX_ERROR),
				errmsg("element \"%s\" of property graph \"%s\" is not an edge",
					   alias, get_rel_name(pgrelid)),
				parser_errposition(pstate, location));

	peoid = ((Form_pg_propgraph_element) GETSTRUCT(tuple))->oid;

	ReleaseSysCache(tuple);

	return peoid;
}

/*
 * Get the element table relation OID from the OID of the element.
 */
static Oid
get_element_relid(Oid peid)
{
	HeapTuple	tuple;
	Oid			pgerelid;

	tuple = SearchSysCache1(PROPGRAPHELOID, ObjectIdGetDatum(peid));
	if (!tuple)
		elog(ERROR, "cache lookup failed for property graph element %u", peid);

	pgerelid = ((Form_pg_propgraph_element) GETSTRUCT(tuple))->pgerelid;

	ReleaseSysCache(tuple);

	return pgerelid;
}

/*
 * Get a list of all label OIDs of a graph.
 */
static List *
get_graph_label_ids(Oid graphid)
{
	Relation	rel;
	SysScanDesc scan;
	ScanKeyData key[1];
	HeapTuple	tuple;
	List	   *result = NIL;

	rel = table_open(PropgraphLabelRelationId, AccessShareLock);
	ScanKeyInit(&key[0],
				Anum_pg_propgraph_label_pglpgid,
				BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(graphid));
	scan = systable_beginscan(rel, PropgraphLabelGraphNameIndexId, true, NULL, 1, key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		result = lappend_oid(result, ((Form_pg_propgraph_label) GETSTRUCT(tuple))->oid);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	return result;
}

/*
 * Get a list of all element label OIDs for a label.
 */
static List *
get_label_element_label_ids(Oid labelid)
{
	Relation	rel;
	SysScanDesc scan;
	ScanKeyData key[1];
	HeapTuple	tuple;
	List	   *result = NIL;

	rel = table_open(PropgraphElementLabelRelationId, AccessShareLock);
	ScanKeyInit(&key[0],
				Anum_pg_propgraph_element_label_pgellabelid,
				BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(labelid));
	scan = systable_beginscan(rel, PropgraphElementLabelLabelIndexId, true, NULL, 1, key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		result = lappend_oid(result, ((Form_pg_propgraph_element_label) GETSTRUCT(tuple))->oid);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	return result;
}

/*
 * Get the names of properties associated with the given element label OID.
 *
 * The result is a list of String nodes (so we can use list functions to
 * detect differences).
 */
static List *
get_element_label_property_names(Oid ellabeloid)
{
	Relation	rel;
	SysScanDesc scan;
	ScanKeyData key[1];
	HeapTuple	tuple;
	List	   *result = NIL;

	rel = table_open(PropgraphLabelPropertyRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_propgraph_label_property_plpellabelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(ellabeloid));

	scan = systable_beginscan(rel, PropgraphLabelPropertyLabelPropIndexId, true, NULL, 1, key);

	while ((tuple = systable_getnext(scan)))
	{
		Form_pg_propgraph_label_property plpform = (Form_pg_propgraph_label_property) GETSTRUCT(tuple);

		result = lappend(result, makeString(get_propgraph_property_name(plpform->plppropid)));
	}

	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	return result;
}

/*
 * Get a list of all property OIDs of a graph.
 */
static List *
get_graph_property_ids(Oid graphid)
{
	Relation	rel;
	SysScanDesc scan;
	ScanKeyData key[1];
	HeapTuple	tuple;
	List	   *result = NIL;

	rel = table_open(PropgraphPropertyRelationId, AccessShareLock);
	ScanKeyInit(&key[0],
				Anum_pg_propgraph_property_pgppgid,
				BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(graphid));
	scan = systable_beginscan(rel, PropgraphPropertyNameIndexId, true, NULL, 1, key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		result = lappend_oid(result, ((Form_pg_propgraph_property) GETSTRUCT(tuple))->oid);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	return result;
}
