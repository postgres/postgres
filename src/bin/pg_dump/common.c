/*-------------------------------------------------------------------------
 *
 * common.c
 *	Catalog routines used by pg_dump; long ago these were shared
 *	by another dump tool, but not anymore.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/bin/pg_dump/common.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "pg_backup_archiver.h"
#include "pg_backup_utils.h"
#include "pg_dump.h"

#include <ctype.h>

#include "catalog/pg_class_d.h"
#include "fe_utils/string_utils.h"


/*
 * Variables for mapping DumpId to DumpableObject
 */
static DumpableObject **dumpIdMap = NULL;
static int	allocedDumpIds = 0;
static DumpId lastDumpId = 0;	/* Note: 0 is InvalidDumpId */

/*
 * Variables for mapping CatalogId to DumpableObject
 */
static bool catalogIdMapValid = false;
static DumpableObject **catalogIdMap = NULL;
static int	numCatalogIds = 0;

/*
 * These variables are static to avoid the notational cruft of having to pass
 * them into findTableByOid() and friends.  For each of these arrays, we build
 * a sorted-by-OID index array immediately after the objects are fetched,
 * and then we use binary search in findTableByOid() and friends.  (qsort'ing
 * the object arrays themselves would be simpler, but it doesn't work because
 * pg_dump.c may have already established pointers between items.)
 */
static DumpableObject **tblinfoindex;
static DumpableObject **typinfoindex;
static DumpableObject **funinfoindex;
static DumpableObject **oprinfoindex;
static DumpableObject **collinfoindex;
static DumpableObject **nspinfoindex;
static DumpableObject **extinfoindex;
static DumpableObject **pubinfoindex;
static int	numTables;
static int	numTypes;
static int	numFuncs;
static int	numOperators;
static int	numCollations;
static int	numNamespaces;
static int	numExtensions;
static int	numPublications;

/* This is an array of object identities, not actual DumpableObjects */
static ExtensionMemberId *extmembers;
static int	numextmembers;

static void flagInhTables(Archive *fout, TableInfo *tbinfo, int numTables,
						  InhInfo *inhinfo, int numInherits);
static void flagInhIndexes(Archive *fout, TableInfo *tblinfo, int numTables);
static void flagInhAttrs(DumpOptions *dopt, TableInfo *tblinfo, int numTables);
static DumpableObject **buildIndexArray(void *objArray, int numObjs,
										Size objSize);
static int	DOCatalogIdCompare(const void *p1, const void *p2);
static int	ExtensionMemberIdCompare(const void *p1, const void *p2);
static void findParentsByOid(TableInfo *self,
							 InhInfo *inhinfo, int numInherits);
static int	strInArray(const char *pattern, char **arr, int arr_size);
static IndxInfo *findIndexByOid(Oid oid, DumpableObject **idxinfoindex,
								int numIndexes);


/*
 * getSchemaData
 *	  Collect information about all potentially dumpable objects
 */
TableInfo *
getSchemaData(Archive *fout, int *numTablesPtr)
{
	TableInfo  *tblinfo;
	TypeInfo   *typinfo;
	FuncInfo   *funinfo;
	OprInfo    *oprinfo;
	CollInfo   *collinfo;
	NamespaceInfo *nspinfo;
	ExtensionInfo *extinfo;
	PublicationInfo *pubinfo;
	InhInfo    *inhinfo;
	int			numAggregates;
	int			numInherits;
	int			numRules;
	int			numProcLangs;
	int			numCasts;
	int			numTransforms;
	int			numAccessMethods;
	int			numOpclasses;
	int			numOpfamilies;
	int			numConversions;
	int			numTSParsers;
	int			numTSTemplates;
	int			numTSDicts;
	int			numTSConfigs;
	int			numForeignDataWrappers;
	int			numForeignServers;
	int			numDefaultACLs;
	int			numEventTriggers;

	/*
	 * We must read extensions and extension membership info first, because
	 * extension membership needs to be consultable during decisions about
	 * whether other objects are to be dumped.
	 */
	pg_log_info("reading extensions");
	extinfo = getExtensions(fout, &numExtensions);
	extinfoindex = buildIndexArray(extinfo, numExtensions, sizeof(ExtensionInfo));

	pg_log_info("identifying extension members");
	getExtensionMembership(fout, extinfo, numExtensions);

	pg_log_info("reading schemas");
	nspinfo = getNamespaces(fout, &numNamespaces);
	nspinfoindex = buildIndexArray(nspinfo, numNamespaces, sizeof(NamespaceInfo));

	/*
	 * getTables should be done as soon as possible, so as to minimize the
	 * window between starting our transaction and acquiring per-table locks.
	 * However, we have to do getNamespaces first because the tables get
	 * linked to their containing namespaces during getTables.
	 */
	pg_log_info("reading user-defined tables");
	tblinfo = getTables(fout, &numTables);
	tblinfoindex = buildIndexArray(tblinfo, numTables, sizeof(TableInfo));

	/* Do this after we've built tblinfoindex */
	getOwnedSeqs(fout, tblinfo, numTables);

	pg_log_info("reading user-defined functions");
	funinfo = getFuncs(fout, &numFuncs);
	funinfoindex = buildIndexArray(funinfo, numFuncs, sizeof(FuncInfo));

	/* this must be after getTables and getFuncs */
	pg_log_info("reading user-defined types");
	typinfo = getTypes(fout, &numTypes);
	typinfoindex = buildIndexArray(typinfo, numTypes, sizeof(TypeInfo));

	/* this must be after getFuncs, too */
	pg_log_info("reading procedural languages");
	getProcLangs(fout, &numProcLangs);

	pg_log_info("reading user-defined aggregate functions");
	getAggregates(fout, &numAggregates);

	pg_log_info("reading user-defined operators");
	oprinfo = getOperators(fout, &numOperators);
	oprinfoindex = buildIndexArray(oprinfo, numOperators, sizeof(OprInfo));

	pg_log_info("reading user-defined access methods");
	getAccessMethods(fout, &numAccessMethods);

	pg_log_info("reading user-defined operator classes");
	getOpclasses(fout, &numOpclasses);

	pg_log_info("reading user-defined operator families");
	getOpfamilies(fout, &numOpfamilies);

	pg_log_info("reading user-defined text search parsers");
	getTSParsers(fout, &numTSParsers);

	pg_log_info("reading user-defined text search templates");
	getTSTemplates(fout, &numTSTemplates);

	pg_log_info("reading user-defined text search dictionaries");
	getTSDictionaries(fout, &numTSDicts);

	pg_log_info("reading user-defined text search configurations");
	getTSConfigurations(fout, &numTSConfigs);

	pg_log_info("reading user-defined foreign-data wrappers");
	getForeignDataWrappers(fout, &numForeignDataWrappers);

	pg_log_info("reading user-defined foreign servers");
	getForeignServers(fout, &numForeignServers);

	pg_log_info("reading default privileges");
	getDefaultACLs(fout, &numDefaultACLs);

	pg_log_info("reading user-defined collations");
	collinfo = getCollations(fout, &numCollations);
	collinfoindex = buildIndexArray(collinfo, numCollations, sizeof(CollInfo));

	pg_log_info("reading user-defined conversions");
	getConversions(fout, &numConversions);

	pg_log_info("reading type casts");
	getCasts(fout, &numCasts);

	pg_log_info("reading transforms");
	getTransforms(fout, &numTransforms);

	pg_log_info("reading table inheritance information");
	inhinfo = getInherits(fout, &numInherits);

	pg_log_info("reading event triggers");
	getEventTriggers(fout, &numEventTriggers);

	/* Identify extension configuration tables that should be dumped */
	pg_log_info("finding extension tables");
	processExtensionTables(fout, extinfo, numExtensions);

	/* Link tables to parents, mark parents of target tables interesting */
	pg_log_info("finding inheritance relationships");
	flagInhTables(fout, tblinfo, numTables, inhinfo, numInherits);

	pg_log_info("reading column info for interesting tables");
	getTableAttrs(fout, tblinfo, numTables);

	pg_log_info("flagging inherited columns in subtables");
	flagInhAttrs(fout->dopt, tblinfo, numTables);

	pg_log_info("reading partitioning data");
	getPartitioningInfo(fout);

	pg_log_info("reading indexes");
	getIndexes(fout, tblinfo, numTables);

	pg_log_info("flagging indexes in partitioned tables");
	flagInhIndexes(fout, tblinfo, numTables);

	pg_log_info("reading extended statistics");
	getExtendedStatistics(fout);

	pg_log_info("reading constraints");
	getConstraints(fout, tblinfo, numTables);

	pg_log_info("reading triggers");
	getTriggers(fout, tblinfo, numTables);

	pg_log_info("reading rewrite rules");
	getRules(fout, &numRules);

	pg_log_info("reading policies");
	getPolicies(fout, tblinfo, numTables);

	pg_log_info("reading publications");
	pubinfo = getPublications(fout, &numPublications);
	pubinfoindex = buildIndexArray(pubinfo, numPublications,
								   sizeof(PublicationInfo));

	pg_log_info("reading publication membership");
	getPublicationTables(fout, tblinfo, numTables);

	pg_log_info("reading subscriptions");
	getSubscriptions(fout);

	*numTablesPtr = numTables;
	return tblinfo;
}

/* flagInhTables -
 *	 Fill in parent link fields of tables for which we need that information,
 *	 and mark parents of target tables as interesting
 *
 * Note that only direct ancestors of targets are marked interesting.
 * This is sufficient; we don't much care whether they inherited their
 * attributes or not.
 *
 * modifies tblinfo
 */
static void
flagInhTables(Archive *fout, TableInfo *tblinfo, int numTables,
			  InhInfo *inhinfo, int numInherits)
{
	int			i,
				j;

	for (i = 0; i < numTables; i++)
	{
		bool		find_parents = true;
		bool		mark_parents = true;

		/* Some kinds never have parents */
		if (tblinfo[i].relkind == RELKIND_SEQUENCE ||
			tblinfo[i].relkind == RELKIND_VIEW ||
			tblinfo[i].relkind == RELKIND_MATVIEW)
			continue;

		/*
		 * Normally, we don't bother computing anything for non-target tables.
		 * However, we must find the parents of non-root partitioned tables in
		 * any case, so that we can trace from leaf partitions up to the root
		 * (in case a leaf is to be dumped but its parents are not).  We need
		 * not mark such parents interesting for getTableAttrs, though.
		 */
		if (!tblinfo[i].dobj.dump)
		{
			mark_parents = false;

			if (!(tblinfo[i].relkind == RELKIND_PARTITIONED_TABLE &&
				  tblinfo[i].ispartition))
				find_parents = false;
		}

		/* If needed, find all the immediate parent tables. */
		if (find_parents)
			findParentsByOid(&tblinfo[i], inhinfo, numInherits);

		/*
		 * If needed, mark the parents as interesting for getTableAttrs and
		 * getIndexes.
		 */
		if (mark_parents)
		{
			int			numParents = tblinfo[i].numParents;
			TableInfo **parents = tblinfo[i].parents;

			for (j = 0; j < numParents; j++)
				parents[j]->interesting = true;
		}
	}
}

/*
 * flagInhIndexes -
 *	 Create IndexAttachInfo objects for partitioned indexes, and add
 *	 appropriate dependency links.
 */
static void
flagInhIndexes(Archive *fout, TableInfo tblinfo[], int numTables)
{
	int			i,
				j,
				k;
	DumpableObject ***parentIndexArray;

	parentIndexArray = (DumpableObject ***)
		pg_malloc0(getMaxDumpId() * sizeof(DumpableObject **));

	for (i = 0; i < numTables; i++)
	{
		TableInfo  *parenttbl;
		IndexAttachInfo *attachinfo;

		if (!tblinfo[i].ispartition || tblinfo[i].numParents == 0)
			continue;

		Assert(tblinfo[i].numParents == 1);
		parenttbl = tblinfo[i].parents[0];

		/*
		 * We need access to each parent table's index list, but there is no
		 * index to cover them outside of this function.  To avoid having to
		 * sort every parent table's indexes each time we come across each of
		 * its partitions, create an indexed array for each parent the first
		 * time it is required.
		 */
		if (parentIndexArray[parenttbl->dobj.dumpId] == NULL)
			parentIndexArray[parenttbl->dobj.dumpId] =
				buildIndexArray(parenttbl->indexes,
								parenttbl->numIndexes,
								sizeof(IndxInfo));

		attachinfo = (IndexAttachInfo *)
			pg_malloc0(tblinfo[i].numIndexes * sizeof(IndexAttachInfo));
		for (j = 0, k = 0; j < tblinfo[i].numIndexes; j++)
		{
			IndxInfo   *index = &(tblinfo[i].indexes[j]);
			IndxInfo   *parentidx;

			if (index->parentidx == 0)
				continue;

			parentidx = findIndexByOid(index->parentidx,
									   parentIndexArray[parenttbl->dobj.dumpId],
									   parenttbl->numIndexes);
			if (parentidx == NULL)
				continue;

			attachinfo[k].dobj.objType = DO_INDEX_ATTACH;
			attachinfo[k].dobj.catId.tableoid = 0;
			attachinfo[k].dobj.catId.oid = 0;
			AssignDumpId(&attachinfo[k].dobj);
			attachinfo[k].dobj.name = pg_strdup(index->dobj.name);
			attachinfo[k].dobj.namespace = index->indextable->dobj.namespace;
			attachinfo[k].parentIdx = parentidx;
			attachinfo[k].partitionIdx = index;

			/*
			 * We must state the DO_INDEX_ATTACH object's dependencies
			 * explicitly, since it will not match anything in pg_depend.
			 *
			 * Give it dependencies on both the partition index and the parent
			 * index, so that it will not be executed till both of those
			 * exist.  (There's no need to care what order those are created
			 * in.)
			 *
			 * In addition, give it dependencies on the indexes' underlying
			 * tables.  This does nothing of great value so far as serial
			 * restore ordering goes, but it ensures that a parallel restore
			 * will not try to run the ATTACH concurrently with other
			 * operations on those tables.
			 */
			addObjectDependency(&attachinfo[k].dobj, index->dobj.dumpId);
			addObjectDependency(&attachinfo[k].dobj, parentidx->dobj.dumpId);
			addObjectDependency(&attachinfo[k].dobj,
								index->indextable->dobj.dumpId);
			addObjectDependency(&attachinfo[k].dobj,
								parentidx->indextable->dobj.dumpId);

			/* keep track of the list of partitions in the parent index */
			simple_ptr_list_append(&parentidx->partattaches, &attachinfo[k].dobj);

			k++;
		}
	}

	for (i = 0; i < numTables; i++)
		if (parentIndexArray[i])
			pg_free(parentIndexArray[i]);
	pg_free(parentIndexArray);
}

/* flagInhAttrs -
 *	 for each dumpable table in tblinfo, flag its inherited attributes
 *
 * What we need to do here is:
 *
 * - Detect child columns that inherit NOT NULL bits from their parents, so
 *   that we needn't specify that again for the child.
 *
 * - Detect child columns that have DEFAULT NULL when their parents had some
 *   non-null default.  In this case, we make up a dummy AttrDefInfo object so
 *   that we'll correctly emit the necessary DEFAULT NULL clause; otherwise
 *   the backend will apply an inherited default to the column.
 *
 * - Detect child columns that have a generation expression when their parents
 *   also have one.  Generation expressions are always inherited, so there is
 *   no need to set them again in child tables, and there is no syntax for it
 *   either.  Exceptions: If it's a partition or we are in binary upgrade
 *   mode, we dump them because in those cases inherited tables are recreated
 *   standalone first and then reattached to the parent.  (See also the logic
 *   in dumpTableSchema().)  In that situation, the generation expressions
 *   must match the parent, enforced by ALTER TABLE.
 *
 * modifies tblinfo
 */
static void
flagInhAttrs(DumpOptions *dopt, TableInfo *tblinfo, int numTables)
{
	int			i,
				j,
				k;

	for (i = 0; i < numTables; i++)
	{
		TableInfo  *tbinfo = &(tblinfo[i]);
		int			numParents;
		TableInfo **parents;

		/* Some kinds never have parents */
		if (tbinfo->relkind == RELKIND_SEQUENCE ||
			tbinfo->relkind == RELKIND_VIEW ||
			tbinfo->relkind == RELKIND_MATVIEW)
			continue;

		/* Don't bother computing anything for non-target tables, either */
		if (!tbinfo->dobj.dump)
			continue;

		numParents = tbinfo->numParents;
		parents = tbinfo->parents;

		if (numParents == 0)
			continue;			/* nothing to see here, move along */

		/* For each column, search for matching column names in parent(s) */
		for (j = 0; j < tbinfo->numatts; j++)
		{
			bool		foundNotNull;	/* Attr was NOT NULL in a parent */
			bool		foundDefault;	/* Found a default in a parent */
			bool		foundGenerated;	/* Found a generated in a parent */

			/* no point in examining dropped columns */
			if (tbinfo->attisdropped[j])
				continue;

			foundNotNull = false;
			foundDefault = false;
			foundGenerated = false;
			for (k = 0; k < numParents; k++)
			{
				TableInfo  *parent = parents[k];
				int			inhAttrInd;

				inhAttrInd = strInArray(tbinfo->attnames[j],
										parent->attnames,
										parent->numatts);
				if (inhAttrInd >= 0)
				{
					foundNotNull |= parent->notnull[inhAttrInd];
					foundDefault |= (parent->attrdefs[inhAttrInd] != NULL && !parent->attgenerated[inhAttrInd]);
					foundGenerated |= parent->attgenerated[inhAttrInd];
				}
			}

			/* Remember if we found inherited NOT NULL */
			tbinfo->inhNotNull[j] = foundNotNull;

			/* Manufacture a DEFAULT NULL clause if necessary */
			if (foundDefault && tbinfo->attrdefs[j] == NULL)
			{
				AttrDefInfo *attrDef;

				attrDef = (AttrDefInfo *) pg_malloc(sizeof(AttrDefInfo));
				attrDef->dobj.objType = DO_ATTRDEF;
				attrDef->dobj.catId.tableoid = 0;
				attrDef->dobj.catId.oid = 0;
				AssignDumpId(&attrDef->dobj);
				attrDef->dobj.name = pg_strdup(tbinfo->dobj.name);
				attrDef->dobj.namespace = tbinfo->dobj.namespace;
				attrDef->dobj.dump = tbinfo->dobj.dump;

				attrDef->adtable = tbinfo;
				attrDef->adnum = j + 1;
				attrDef->adef_expr = pg_strdup("NULL");

				/* Will column be dumped explicitly? */
				if (shouldPrintColumn(dopt, tbinfo, j))
				{
					attrDef->separate = false;
					/* No dependency needed: NULL cannot have dependencies */
				}
				else
				{
					/* column will be suppressed, print default separately */
					attrDef->separate = true;
					/* ensure it comes out after the table */
					addObjectDependency(&attrDef->dobj,
										tbinfo->dobj.dumpId);
				}

				tbinfo->attrdefs[j] = attrDef;
			}

			/* Remove generation expression from child */
			if (foundGenerated && !tbinfo->ispartition && !dopt->binary_upgrade)
				tbinfo->attrdefs[j] = NULL;
		}
	}
}

/*
 * AssignDumpId
 *		Given a newly-created dumpable object, assign a dump ID,
 *		and enter the object into the lookup table.
 *
 * The caller is expected to have filled in objType and catId,
 * but not any of the other standard fields of a DumpableObject.
 */
void
AssignDumpId(DumpableObject *dobj)
{
	dobj->dumpId = ++lastDumpId;
	dobj->name = NULL;			/* must be set later */
	dobj->namespace = NULL;		/* may be set later */
	dobj->dump = DUMP_COMPONENT_ALL;	/* default assumption */
	dobj->ext_member = false;	/* default assumption */
	dobj->depends_on_ext = false;	/* default assumption */
	dobj->dependencies = NULL;
	dobj->nDeps = 0;
	dobj->allocDeps = 0;

	while (dobj->dumpId >= allocedDumpIds)
	{
		int			newAlloc;

		if (allocedDumpIds <= 0)
		{
			newAlloc = 256;
			dumpIdMap = (DumpableObject **)
				pg_malloc(newAlloc * sizeof(DumpableObject *));
		}
		else
		{
			newAlloc = allocedDumpIds * 2;
			dumpIdMap = (DumpableObject **)
				pg_realloc(dumpIdMap, newAlloc * sizeof(DumpableObject *));
		}
		memset(dumpIdMap + allocedDumpIds, 0,
			   (newAlloc - allocedDumpIds) * sizeof(DumpableObject *));
		allocedDumpIds = newAlloc;
	}
	dumpIdMap[dobj->dumpId] = dobj;

	/* mark catalogIdMap invalid, but don't rebuild it yet */
	catalogIdMapValid = false;
}

/*
 * Assign a DumpId that's not tied to a DumpableObject.
 *
 * This is used when creating a "fixed" ArchiveEntry that doesn't need to
 * participate in the sorting logic.
 */
DumpId
createDumpId(void)
{
	return ++lastDumpId;
}

/*
 * Return the largest DumpId so far assigned
 */
DumpId
getMaxDumpId(void)
{
	return lastDumpId;
}

/*
 * Find a DumpableObject by dump ID
 *
 * Returns NULL for invalid ID
 */
DumpableObject *
findObjectByDumpId(DumpId dumpId)
{
	if (dumpId <= 0 || dumpId >= allocedDumpIds)
		return NULL;			/* out of range? */
	return dumpIdMap[dumpId];
}

/*
 * Find a DumpableObject by catalog ID
 *
 * Returns NULL for unknown ID
 *
 * We use binary search in a sorted list that is built on first call.
 * If AssignDumpId() and findObjectByCatalogId() calls were freely intermixed,
 * the code would work, but possibly be very slow.  In the current usage
 * pattern that does not happen, indeed we build the list at most twice.
 */
DumpableObject *
findObjectByCatalogId(CatalogId catalogId)
{
	DumpableObject **low;
	DumpableObject **high;

	if (!catalogIdMapValid)
	{
		if (catalogIdMap)
			free(catalogIdMap);
		getDumpableObjects(&catalogIdMap, &numCatalogIds);
		if (numCatalogIds > 1)
			qsort((void *) catalogIdMap, numCatalogIds,
				  sizeof(DumpableObject *), DOCatalogIdCompare);
		catalogIdMapValid = true;
	}

	/*
	 * We could use bsearch() here, but the notational cruft of calling
	 * bsearch is nearly as bad as doing it ourselves; and the generalized
	 * bsearch function is noticeably slower as well.
	 */
	if (numCatalogIds <= 0)
		return NULL;
	low = catalogIdMap;
	high = catalogIdMap + (numCatalogIds - 1);
	while (low <= high)
	{
		DumpableObject **middle;
		int			difference;

		middle = low + (high - low) / 2;
		/* comparison must match DOCatalogIdCompare, below */
		difference = oidcmp((*middle)->catId.oid, catalogId.oid);
		if (difference == 0)
			difference = oidcmp((*middle)->catId.tableoid, catalogId.tableoid);
		if (difference == 0)
			return *middle;
		else if (difference < 0)
			low = middle + 1;
		else
			high = middle - 1;
	}
	return NULL;
}

/*
 * Find a DumpableObject by OID, in a pre-sorted array of one type of object
 *
 * Returns NULL for unknown OID
 */
static DumpableObject *
findObjectByOid(Oid oid, DumpableObject **indexArray, int numObjs)
{
	DumpableObject **low;
	DumpableObject **high;

	/*
	 * This is the same as findObjectByCatalogId except we assume we need not
	 * look at table OID because the objects are all the same type.
	 *
	 * We could use bsearch() here, but the notational cruft of calling
	 * bsearch is nearly as bad as doing it ourselves; and the generalized
	 * bsearch function is noticeably slower as well.
	 */
	if (numObjs <= 0)
		return NULL;
	low = indexArray;
	high = indexArray + (numObjs - 1);
	while (low <= high)
	{
		DumpableObject **middle;
		int			difference;

		middle = low + (high - low) / 2;
		difference = oidcmp((*middle)->catId.oid, oid);
		if (difference == 0)
			return *middle;
		else if (difference < 0)
			low = middle + 1;
		else
			high = middle - 1;
	}
	return NULL;
}

/*
 * Build an index array of DumpableObject pointers, sorted by OID
 */
static DumpableObject **
buildIndexArray(void *objArray, int numObjs, Size objSize)
{
	DumpableObject **ptrs;
	int			i;

	ptrs = (DumpableObject **) pg_malloc(numObjs * sizeof(DumpableObject *));
	for (i = 0; i < numObjs; i++)
		ptrs[i] = (DumpableObject *) ((char *) objArray + i * objSize);

	/* We can use DOCatalogIdCompare to sort since its first key is OID */
	if (numObjs > 1)
		qsort((void *) ptrs, numObjs, sizeof(DumpableObject *),
			  DOCatalogIdCompare);

	return ptrs;
}

/*
 * qsort comparator for pointers to DumpableObjects
 */
static int
DOCatalogIdCompare(const void *p1, const void *p2)
{
	const DumpableObject *obj1 = *(DumpableObject *const *) p1;
	const DumpableObject *obj2 = *(DumpableObject *const *) p2;
	int			cmpval;

	/*
	 * Compare OID first since it's usually unique, whereas there will only be
	 * a few distinct values of tableoid.
	 */
	cmpval = oidcmp(obj1->catId.oid, obj2->catId.oid);
	if (cmpval == 0)
		cmpval = oidcmp(obj1->catId.tableoid, obj2->catId.tableoid);
	return cmpval;
}

/*
 * Build an array of pointers to all known dumpable objects
 *
 * This simply creates a modifiable copy of the internal map.
 */
void
getDumpableObjects(DumpableObject ***objs, int *numObjs)
{
	int			i,
				j;

	*objs = (DumpableObject **)
		pg_malloc(allocedDumpIds * sizeof(DumpableObject *));
	j = 0;
	for (i = 1; i < allocedDumpIds; i++)
	{
		if (dumpIdMap[i])
			(*objs)[j++] = dumpIdMap[i];
	}
	*numObjs = j;
}

/*
 * Add a dependency link to a DumpableObject
 *
 * Note: duplicate dependencies are currently not eliminated
 */
void
addObjectDependency(DumpableObject *dobj, DumpId refId)
{
	if (dobj->nDeps >= dobj->allocDeps)
	{
		if (dobj->allocDeps <= 0)
		{
			dobj->allocDeps = 16;
			dobj->dependencies = (DumpId *)
				pg_malloc(dobj->allocDeps * sizeof(DumpId));
		}
		else
		{
			dobj->allocDeps *= 2;
			dobj->dependencies = (DumpId *)
				pg_realloc(dobj->dependencies,
						   dobj->allocDeps * sizeof(DumpId));
		}
	}
	dobj->dependencies[dobj->nDeps++] = refId;
}

/*
 * Remove a dependency link from a DumpableObject
 *
 * If there are multiple links, all are removed
 */
void
removeObjectDependency(DumpableObject *dobj, DumpId refId)
{
	int			i;
	int			j = 0;

	for (i = 0; i < dobj->nDeps; i++)
	{
		if (dobj->dependencies[i] != refId)
			dobj->dependencies[j++] = dobj->dependencies[i];
	}
	dobj->nDeps = j;
}


/*
 * findTableByOid
 *	  finds the entry (in tblinfo) of the table with the given oid
 *	  returns NULL if not found
 */
TableInfo *
findTableByOid(Oid oid)
{
	return (TableInfo *) findObjectByOid(oid, tblinfoindex, numTables);
}

/*
 * findTypeByOid
 *	  finds the entry (in typinfo) of the type with the given oid
 *	  returns NULL if not found
 */
TypeInfo *
findTypeByOid(Oid oid)
{
	return (TypeInfo *) findObjectByOid(oid, typinfoindex, numTypes);
}

/*
 * findFuncByOid
 *	  finds the entry (in funinfo) of the function with the given oid
 *	  returns NULL if not found
 */
FuncInfo *
findFuncByOid(Oid oid)
{
	return (FuncInfo *) findObjectByOid(oid, funinfoindex, numFuncs);
}

/*
 * findOprByOid
 *	  finds the entry (in oprinfo) of the operator with the given oid
 *	  returns NULL if not found
 */
OprInfo *
findOprByOid(Oid oid)
{
	return (OprInfo *) findObjectByOid(oid, oprinfoindex, numOperators);
}

/*
 * findCollationByOid
 *	  finds the entry (in collinfo) of the collation with the given oid
 *	  returns NULL if not found
 */
CollInfo *
findCollationByOid(Oid oid)
{
	return (CollInfo *) findObjectByOid(oid, collinfoindex, numCollations);
}

/*
 * findNamespaceByOid
 *	  finds the entry (in nspinfo) of the namespace with the given oid
 *	  returns NULL if not found
 */
NamespaceInfo *
findNamespaceByOid(Oid oid)
{
	return (NamespaceInfo *) findObjectByOid(oid, nspinfoindex, numNamespaces);
}

/*
 * findExtensionByOid
 *	  finds the entry (in extinfo) of the extension with the given oid
 *	  returns NULL if not found
 */
ExtensionInfo *
findExtensionByOid(Oid oid)
{
	return (ExtensionInfo *) findObjectByOid(oid, extinfoindex, numExtensions);
}

/*
 * findPublicationByOid
 *	  finds the entry (in pubinfo) of the publication with the given oid
 *	  returns NULL if not found
 */
PublicationInfo *
findPublicationByOid(Oid oid)
{
	return (PublicationInfo *) findObjectByOid(oid, pubinfoindex, numPublications);
}

/*
 * findIndexByOid
 *		find the entry of the index with the given oid
 *
 * This one's signature is different from the previous ones because we lack a
 * global array of all indexes, so caller must pass their array as argument.
 */
static IndxInfo *
findIndexByOid(Oid oid, DumpableObject **idxinfoindex, int numIndexes)
{
	return (IndxInfo *) findObjectByOid(oid, idxinfoindex, numIndexes);
}

/*
 * setExtensionMembership
 *	  accept and save data about which objects belong to extensions
 */
void
setExtensionMembership(ExtensionMemberId *extmems, int nextmems)
{
	/* Sort array in preparation for binary searches */
	if (nextmems > 1)
		qsort((void *) extmems, nextmems, sizeof(ExtensionMemberId),
			  ExtensionMemberIdCompare);
	/* And save */
	extmembers = extmems;
	numextmembers = nextmems;
}

/*
 * findOwningExtension
 *	  return owning extension for specified catalog ID, or NULL if none
 */
ExtensionInfo *
findOwningExtension(CatalogId catalogId)
{
	ExtensionMemberId *low;
	ExtensionMemberId *high;

	/*
	 * We could use bsearch() here, but the notational cruft of calling
	 * bsearch is nearly as bad as doing it ourselves; and the generalized
	 * bsearch function is noticeably slower as well.
	 */
	if (numextmembers <= 0)
		return NULL;
	low = extmembers;
	high = extmembers + (numextmembers - 1);
	while (low <= high)
	{
		ExtensionMemberId *middle;
		int			difference;

		middle = low + (high - low) / 2;
		/* comparison must match ExtensionMemberIdCompare, below */
		difference = oidcmp(middle->catId.oid, catalogId.oid);
		if (difference == 0)
			difference = oidcmp(middle->catId.tableoid, catalogId.tableoid);
		if (difference == 0)
			return middle->ext;
		else if (difference < 0)
			low = middle + 1;
		else
			high = middle - 1;
	}
	return NULL;
}

/*
 * qsort comparator for ExtensionMemberIds
 */
static int
ExtensionMemberIdCompare(const void *p1, const void *p2)
{
	const ExtensionMemberId *obj1 = (const ExtensionMemberId *) p1;
	const ExtensionMemberId *obj2 = (const ExtensionMemberId *) p2;
	int			cmpval;

	/*
	 * Compare OID first since it's usually unique, whereas there will only be
	 * a few distinct values of tableoid.
	 */
	cmpval = oidcmp(obj1->catId.oid, obj2->catId.oid);
	if (cmpval == 0)
		cmpval = oidcmp(obj1->catId.tableoid, obj2->catId.tableoid);
	return cmpval;
}


/*
 * findParentsByOid
 *	  find a table's parents in tblinfo[]
 */
static void
findParentsByOid(TableInfo *self,
				 InhInfo *inhinfo, int numInherits)
{
	Oid			oid = self->dobj.catId.oid;
	int			i,
				j;
	int			numParents;

	numParents = 0;
	for (i = 0; i < numInherits; i++)
	{
		if (inhinfo[i].inhrelid == oid)
			numParents++;
	}

	self->numParents = numParents;

	if (numParents > 0)
	{
		self->parents = (TableInfo **)
			pg_malloc(sizeof(TableInfo *) * numParents);
		j = 0;
		for (i = 0; i < numInherits; i++)
		{
			if (inhinfo[i].inhrelid == oid)
			{
				TableInfo  *parent;

				parent = findTableByOid(inhinfo[i].inhparent);
				if (parent == NULL)
				{
					pg_log_error("failed sanity check, parent OID %u of table \"%s\" (OID %u) not found",
								 inhinfo[i].inhparent,
								 self->dobj.name,
								 oid);
					exit_nicely(1);
				}
				self->parents[j++] = parent;
			}
		}
	}
	else
		self->parents = NULL;
}

/*
 * parseOidArray
 *	  parse a string of numbers delimited by spaces into a character array
 *
 * Note: actually this is used for both Oids and potentially-signed
 * attribute numbers.  This should cause no trouble, but we could split
 * the function into two functions with different argument types if it does.
 */

void
parseOidArray(const char *str, Oid *array, int arraysize)
{
	int			j,
				argNum;
	char		temp[100];
	char		s;

	argNum = 0;
	j = 0;
	for (;;)
	{
		s = *str++;
		if (s == ' ' || s == '\0')
		{
			if (j > 0)
			{
				if (argNum >= arraysize)
				{
					pg_log_error("could not parse numeric array \"%s\": too many numbers", str);
					exit_nicely(1);
				}
				temp[j] = '\0';
				array[argNum++] = atooid(temp);
				j = 0;
			}
			if (s == '\0')
				break;
		}
		else
		{
			if (!(isdigit((unsigned char) s) || s == '-') ||
				j >= sizeof(temp) - 1)
			{
				pg_log_error("could not parse numeric array \"%s\": invalid character in number", str);
				exit_nicely(1);
			}
			temp[j++] = s;
		}
	}

	while (argNum < arraysize)
		array[argNum++] = InvalidOid;
}


/*
 * strInArray:
 *	  takes in a string and a string array and the number of elements in the
 * string array.
 *	  returns the index if the string is somewhere in the array, -1 otherwise
 */

static int
strInArray(const char *pattern, char **arr, int arr_size)
{
	int			i;

	for (i = 0; i < arr_size; i++)
	{
		if (strcmp(pattern, arr[i]) == 0)
			return i;
	}
	return -1;
}
