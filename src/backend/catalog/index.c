/*-------------------------------------------------------------------------
 *
 * index.c--
 *	  code to create and destroy POSTGRES index relations
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/index.c,v 1.55 1998/08/26 17:12:09 momjian Exp $
 *
 *
 * INTERFACE ROUTINES
 *		index_create()			- Create a cataloged index relation
 *		index_destroy()			- Removes index relation from catalogs
 *
 * NOTES
 *	  Much of this code uses hardcoded sequential heap relation scans
 *	  to fetch information from the catalogs.  These should all be
 *	  rewritten to use the system caches lookup routines like
 *	  SearchSysCacheTuple, which can do efficient lookup and
 *	  caching.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/istrat.h"
#include "access/xact.h"
#include "bootstrap/bootstrap.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "optimizer/prep.h"
#include "parser/parse_func.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/mcxt.h"
#include "utils/relcache.h"
#include "utils/syscache.h"
#include "utils/tqual.h"

#ifndef HAVE_MEMMOVE
#include <regex/utils.h>
#else
#include <string.h>
#endif

/*
 * macros used in guessing how many tuples are on a page.
 */
#define AVG_TUPLE_SIZE 8
#define NTUPLES_PER_PAGE(natts) (BLCKSZ/((natts)*AVG_TUPLE_SIZE))

/* non-export function prototypes */
static Oid
RelationNameGetObjectId(char *relationName, Relation pg_class);
static Oid	GetHeapRelationOid(char *heapRelationName, char *indexRelationName);
static TupleDesc BuildFuncTupleDesc(FuncIndexInfo *funcInfo);
static TupleDesc
ConstructTupleDescriptor(Oid heapoid, Relation heapRelation,
						 List *attributeList,
						 int numatts, AttrNumber attNums[]);

static void ConstructIndexReldesc(Relation indexRelation, Oid amoid);
static Oid	UpdateRelationRelation(Relation indexRelation);
static void
InitializeAttributeOids(Relation indexRelation,
						int numatts,
						Oid indexoid);
static void
			AppendAttributeTuples(Relation indexRelation, int numatts);
static void
UpdateIndexRelation(Oid indexoid, Oid heapoid,
					FuncIndexInfo *funcInfo, int natts,
				  AttrNumber attNums[], Oid classOids[], Node *predicate,
					List *attributeList, bool islossy, bool unique);
static void
DefaultBuild(Relation heapRelation, Relation indexRelation,
			 int numberOfAttributes, AttrNumber attributeNumber[],
			 IndexStrategy indexStrategy, uint16 parameterCount,
	   Datum parameter[], FuncIndexInfoPtr funcInfo, PredInfo *predInfo);

/* ----------------------------------------------------------------
 *	  sysatts is a structure containing attribute tuple forms
 *	  for system attributes (numbered -1, -2, ...).  This really
 *	  should be generated or eliminated or moved elsewhere. -cim 1/19/91
 *
 * typedef struct FormData_pg_attribute {
 *		Oid				attrelid;
 *		NameData		attname;
 *		Oid				atttypid;
 *		uint32			attnvals;
 *		int16			attlen;
 *		AttrNumber		attnum;
 *		uint32			attnelems;
 *		int32			attcacheoff;
 *		int32			atttypmod;
 *		bool			attbyval;
 *		bool			attisset;
 *		char			attalign;
 *		bool			attnotnull;
 *		bool			atthasdef;
 * } FormData_pg_attribute;
 *
 * ----------------------------------------------------------------
 */
static FormData_pg_attribute sysatts[] = {
	{0, {"ctid"}, TIDOID, 0, 6, -1, 0, -1, -1, '\0', '\0', 'i', '\0', '\0'},
	{0, {"oid"}, OIDOID, 0, 4, -2, 0, -1, -1, '\001', '\0', 'i', '\0', '\0'},
	{0, {"xmin"}, XIDOID, 0, 4, -3, 0, -1, -1, '\001', '\0', 'i', '\0', '\0'},
	{0, {"cmin"}, CIDOID, 0, 4, -4, 0, -1, -1, '\001', '\0', 'i', '\0', '\0'},
	{0, {"xmax"}, XIDOID, 0, 4, -5, 0, -1, -1, '\001', '\0', 'i', '\0', '\0'},
	{0, {"cmax"}, CIDOID, 0, 4, -6, 0, -1, -1, '\001', '\0', 'i', '\0', '\0'},
};

/* ----------------------------------------------------------------
 * RelationNameGetObjectId --
 *		Returns the object identifier for a relation given its name.
 *
 * ----------------------------------------------------------------
 */
static Oid
RelationNameGetObjectId(char *relationName,
						Relation pg_class)
{
	HeapScanDesc pg_class_scan;
	HeapTuple	pg_class_tuple;
	Oid			relationObjectId;
	ScanKeyData key;

	/*
	 * If this isn't bootstrap time, we can use the system catalogs to
	 * speed this up.
	 */

	if (!IsBootstrapProcessingMode())
	{
		HeapTuple 	tuple;
	
		tuple = SearchSysCacheTuple(RELNAME,
									PointerGetDatum(relationName),
									0, 0, 0);
	
		if (HeapTupleIsValid(tuple))
			return tuple->t_oid;
		else
			return InvalidOid;
	}

	/* ----------------
	 *	BOOTSTRAP TIME, do this the hard way.
	 *	begin a scan of pg_class for the named relation
	 * ----------------
	 */
	ScanKeyEntryInitialize(&key, 0, Anum_pg_class_relname,
						   F_NAMEEQ,
						   PointerGetDatum(relationName));

	pg_class_scan = heap_beginscan(pg_class, 0, SnapshotNow, 1, &key);

	/* ----------------
	 *	if we find the named relation, fetch its relation id
	 *	(the oid of the tuple we found).
	 * ----------------
	 */
	pg_class_tuple = heap_getnext(pg_class_scan, 0);

	if (!HeapTupleIsValid(pg_class_tuple))
		relationObjectId = InvalidOid;
	else
		relationObjectId = pg_class_tuple->t_oid;

	/* ----------------
	 *	cleanup and return results
	 * ----------------
	 */
	heap_endscan(pg_class_scan);

	return relationObjectId;
}


/* ----------------------------------------------------------------
 *		GetHeapRelationOid
 * ----------------------------------------------------------------
 */
static Oid
GetHeapRelationOid(char *heapRelationName, char *indexRelationName)
{
	Relation	pg_class;
	Oid			indoid;
	Oid			heapoid;

	/* ----------------
	 *	open pg_class and get the oid of the relation
	 *	corresponding to the name of the index relation.
	 * ----------------
	 */
	pg_class = heap_openr(RelationRelationName);

	indoid = RelationNameGetObjectId(indexRelationName, pg_class);

	if (OidIsValid(indoid))
		elog(ERROR, "Cannot create index: '%s' already exists",
			 indexRelationName);

	heapoid = RelationNameGetObjectId(heapRelationName, pg_class);

	if (!OidIsValid(heapoid))
		elog(ERROR, "Cannot create index on '%s': relation does not exist",
			 heapRelationName);

	heap_close(pg_class);

	return heapoid;
}

static TupleDesc
BuildFuncTupleDesc(FuncIndexInfo *funcInfo)
{
	HeapTuple	tuple;
	TupleDesc	funcTupDesc;
	Oid			retType;
	char	   *funcname;
	int4		nargs;
	Oid		   *argtypes;

	/*
	 * Allocate and zero a tuple descriptor.
	 */
	funcTupDesc = CreateTemplateTupleDesc(1);
	funcTupDesc->attrs[0] = (AttributeTupleForm) palloc(ATTRIBUTE_TUPLE_SIZE);
	MemSet(funcTupDesc->attrs[0], 0, ATTRIBUTE_TUPLE_SIZE);

	/*
	 * Lookup the function for the return type.
	 */
	funcname = FIgetname(funcInfo);
	nargs = FIgetnArgs(funcInfo);
	argtypes = FIgetArglist(funcInfo);
	tuple = SearchSysCacheTuple(PRONAME,
								PointerGetDatum(funcname),
								Int32GetDatum(nargs),
								PointerGetDatum(argtypes),
								0);

	if (!HeapTupleIsValid(tuple))
		func_error("BuildFuncTupleDesc", funcname, nargs, argtypes, NULL);

	retType = ((Form_pg_proc) GETSTRUCT(tuple))->prorettype;

	/*
	 * Look up the return type in pg_type for the type length.
	 */
	tuple = SearchSysCacheTuple(TYPOID,
								ObjectIdGetDatum(retType),
								0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "Function %s return type does not exist", FIgetname(funcInfo));

	/*
	 * Assign some of the attributes values. Leave the rest as 0.
	 */
	funcTupDesc->attrs[0]->attlen = ((TypeTupleForm) GETSTRUCT(tuple))->typlen;
	funcTupDesc->attrs[0]->atttypid = retType;
	funcTupDesc->attrs[0]->attnum = 1;
	funcTupDesc->attrs[0]->attbyval = ((TypeTupleForm) GETSTRUCT(tuple))->typbyval;
	funcTupDesc->attrs[0]->attcacheoff = -1;
	funcTupDesc->attrs[0]->atttypmod = -1;
	funcTupDesc->attrs[0]->attalign = ((TypeTupleForm) GETSTRUCT(tuple))->typalign;

	/*
	 * make the attributes name the same as the functions
	 */
	namestrcpy(&funcTupDesc->attrs[0]->attname, funcname);

	return (funcTupDesc);
}

/* ----------------------------------------------------------------
 *		ConstructTupleDescriptor
 * ----------------------------------------------------------------
 */
static TupleDesc
ConstructTupleDescriptor(Oid heapoid,
						 Relation heapRelation,
						 List *attributeList,
						 int numatts,
						 AttrNumber attNums[])
{
	TupleDesc	heapTupDesc;
	TupleDesc	indexTupDesc;
	IndexElem  *IndexKey;
	TypeName   *IndexKeyType;
	AttrNumber	atnum;			/* attributeNumber[attributeOffset] */
	AttrNumber	atind;
	int			natts;			/* RelationTupleForm->relnatts */
	char	   *from;			/* used to simplify memcpy below */
	char	   *to;				/* used to simplify memcpy below */
	int			i;

	/* ----------------
	 *	allocate the new tuple descriptor
	 * ----------------
	 */
	natts = RelationGetRelationTupleForm(heapRelation)->relnatts;

	indexTupDesc = CreateTemplateTupleDesc(numatts);

	/* ----------------
	 *
	 * ----------------
	 */

	/* ----------------
	 *	  for each attribute we are indexing, obtain its attribute
	 *	  tuple form from either the static table of system attribute
	 *	  tuple forms or the relation tuple descriptor
	 * ----------------
	 */
	for (i = 0; i < numatts; i += 1)
	{

		/* ----------------
		 *	 get the attribute number and make sure it's valid
		 * ----------------
		 */
		atnum = attNums[i];
		if (atnum > natts)
			elog(ERROR, "Cannot create index: attribute %d does not exist",
				 atnum);
		if (attributeList)
		{
			IndexKey = (IndexElem *) lfirst(attributeList);
			IndexKeyType = IndexKey->typename;
			attributeList = lnext(attributeList);
		}
		else
			IndexKeyType = NULL;

		indexTupDesc->attrs[i] = (AttributeTupleForm) palloc(ATTRIBUTE_TUPLE_SIZE);

		/* ----------------
		 *	 determine which tuple descriptor to copy
		 * ----------------
		 */
		if (!AttrNumberIsForUserDefinedAttr(atnum))
		{

			/* ----------------
			 *	  here we are indexing on a system attribute (-1...-12)
			 *	  so we convert atnum into a usable index 0...11 so we can
			 *	  use it to dereference the array sysatts[] which stores
			 *	  tuple descriptor information for system attributes.
			 * ----------------
			 */
			if (atnum <= FirstLowInvalidHeapAttributeNumber || atnum >= 0)
				elog(ERROR, "Cannot create index on system attribute: attribute number out of range (%d)", atnum);
			atind = (-atnum) - 1;

			from = (char *) (&sysatts[atind]);

		}
		else
		{
			/* ----------------
			 *	  here we are indexing on a normal attribute (1...n)
			 * ----------------
			 */
			heapTupDesc = RelationGetTupleDescriptor(heapRelation);
			atind = AttrNumberGetAttrOffset(atnum);

			from = (char *) (heapTupDesc->attrs[atind]);
		}

		/* ----------------
		 *	 now that we've determined the "from", let's copy
		 *	 the tuple desc data...
		 * ----------------
		 */

		to = (char *) (indexTupDesc->attrs[i]);
		memcpy(to, from, ATTRIBUTE_TUPLE_SIZE);

		((AttributeTupleForm) to)->attnum = i + 1;

		((AttributeTupleForm) to)->attnotnull = false;
		((AttributeTupleForm) to)->atthasdef = false;
		((AttributeTupleForm) to)->attcacheoff = -1;
		((AttributeTupleForm) to)->atttypmod = -1;
		((AttributeTupleForm) to)->attalign = 'i';

		/*
		 * if the keytype is defined, we need to change the tuple form's
		 * atttypid & attlen field to match that of the key's type
		 */
		if (IndexKeyType != NULL)
		{
			HeapTuple	tup;

			tup = SearchSysCacheTuple(TYPNAME,
									  PointerGetDatum(IndexKeyType->name),
									  0, 0, 0);
			if (!HeapTupleIsValid(tup))
				elog(ERROR, "create index: type '%s' undefined",
					 IndexKeyType->name);
			((AttributeTupleForm) to)->atttypid = tup->t_oid;
			((AttributeTupleForm) to)->attbyval =
				((TypeTupleForm) GETSTRUCT(tup))->typbyval;
			((AttributeTupleForm) to)->attlen =
				((TypeTupleForm) GETSTRUCT(tup))->typlen;
			((AttributeTupleForm) to)->attalign =
				((TypeTupleForm) GETSTRUCT(tup))->typalign;
			((AttributeTupleForm) to)->atttypmod = IndexKeyType->typmod;
		}


		/* ----------------
		 *	  now we have to drop in the proper relation descriptor
		 *	  into the copied tuple form's attrelid and we should be
		 *	  all set.
		 * ----------------
		 */
		((AttributeTupleForm) to)->attrelid = heapoid;
	}

	return indexTupDesc;
}

/* ----------------------------------------------------------------
 * AccessMethodObjectIdGetAccessMethodTupleForm --
 *		Returns the formated access method tuple given its object identifier.
 *
 * XXX ADD INDEXING
 *
 * Note:
 *		Assumes object identifier is valid.
 * ----------------------------------------------------------------
 */
Form_pg_am
AccessMethodObjectIdGetAccessMethodTupleForm(Oid accessMethodObjectId)
{
	Relation	pg_am_desc;
	HeapScanDesc pg_am_scan;
	HeapTuple	pg_am_tuple;
	ScanKeyData key;
	Form_pg_am	form;

	/* ----------------
	 *	form a scan key for the pg_am relation
	 * ----------------
	 */
	ScanKeyEntryInitialize(&key, 0, ObjectIdAttributeNumber,
						   F_OIDEQ,
						   ObjectIdGetDatum(accessMethodObjectId));

	/* ----------------
	 *	fetch the desired access method tuple
	 * ----------------
	 */
	pg_am_desc = heap_openr(AccessMethodRelationName);
	pg_am_scan = heap_beginscan(pg_am_desc, 0, SnapshotNow, 1, &key);

	pg_am_tuple = heap_getnext(pg_am_scan, 0);

	/* ----------------
	 *	return NULL if not found
	 * ----------------
	 */
	if (!HeapTupleIsValid(pg_am_tuple))
	{
		heap_endscan(pg_am_scan);
		heap_close(pg_am_desc);
		return (NULL);
	}

	/* ----------------
	 *	if found am tuple, then copy the form and return the copy
	 * ----------------
	 */
	form = (Form_pg_am) palloc(sizeof *form);
	memcpy(form, GETSTRUCT(pg_am_tuple), sizeof *form);

	heap_endscan(pg_am_scan);
	heap_close(pg_am_desc);

	return (form);
}

/* ----------------------------------------------------------------
 *		ConstructIndexReldesc
 * ----------------------------------------------------------------
 */
static void
ConstructIndexReldesc(Relation indexRelation, Oid amoid)
{
	extern GlobalMemory CacheCxt;
	MemoryContext oldcxt;

	/* ----------------
	 *	  here we make certain to allocate the access method
	 *	  tuple within the cache context lest it vanish when the
	 *	  context changes
	 * ----------------
	 */
	if (!CacheCxt)
		CacheCxt = CreateGlobalMemory("Cache");

	oldcxt = MemoryContextSwitchTo((MemoryContext) CacheCxt);

	indexRelation->rd_am =
		AccessMethodObjectIdGetAccessMethodTupleForm(amoid);

	MemoryContextSwitchTo(oldcxt);

	/* ----------------
	 *	 XXX missing the initialization of some other fields
	 * ----------------
	 */

	indexRelation->rd_rel->relowner = GetUserId();

	indexRelation->rd_rel->relam = amoid;
	indexRelation->rd_rel->reltuples = 1;		/* XXX */
	indexRelation->rd_rel->relkind = RELKIND_INDEX;
}

/* ----------------------------------------------------------------
 *		UpdateRelationRelation
 * ----------------------------------------------------------------
 */
static Oid
UpdateRelationRelation(Relation indexRelation)
{
	Relation	pg_class;
	HeapTuple	tuple;
	Oid			tupleOid;
	Relation	idescs[Num_pg_class_indices];

	pg_class = heap_openr(RelationRelationName);

	/* XXX Natts_pg_class_fixed is a hack - see pg_class.h */
	tuple = heap_addheader(Natts_pg_class_fixed,
						   sizeof(*indexRelation->rd_rel),
						   (char *) indexRelation->rd_rel);

	/* ----------------
	 *	the new tuple must have the same oid as the relcache entry for the
	 *	index.	sure would be embarassing to do this sort of thing in polite
	 *	company.
	 * ----------------
	 */
	tuple->t_oid = RelationGetRelid(indexRelation);
	heap_insert(pg_class, tuple);

	/*
	 * During normal processing, we need to make sure that the system
	 * catalog indices are correct.  Bootstrap (initdb) time doesn't
	 * require this, because we make sure that the indices are correct
	 * just before exiting.
	 */

	if (!IsBootstrapProcessingMode())
	{
		CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices, idescs);
		CatalogIndexInsert(idescs, Num_pg_class_indices, pg_class, tuple);
		CatalogCloseIndices(Num_pg_class_indices, idescs);
	}

	tupleOid = tuple->t_oid;
	pfree(tuple);
	heap_close(pg_class);

	return (tupleOid);
}

/* ----------------------------------------------------------------
 *		InitializeAttributeOids
 * ----------------------------------------------------------------
 */
static void
InitializeAttributeOids(Relation indexRelation,
						int numatts,
						Oid indexoid)
{
	TupleDesc	tupleDescriptor;
	int			i;

	tupleDescriptor = RelationGetTupleDescriptor(indexRelation);

	for (i = 0; i < numatts; i += 1)
		tupleDescriptor->attrs[i]->attrelid = indexoid;
}

/* ----------------------------------------------------------------
 *		AppendAttributeTuples
 *
 *		XXX For now, only change the ATTNUM attribute value
 * ----------------------------------------------------------------
 */
static void
AppendAttributeTuples(Relation indexRelation, int numatts)
{
	Relation	pg_attribute;
	HeapTuple	init_tuple, cur_tuple = NULL, new_tuple;
	bool		hasind;
	Relation	idescs[Num_pg_attr_indices];

	Datum		value[Natts_pg_attribute];
	char		nullv[Natts_pg_attribute];
	char		replace[Natts_pg_attribute];

	TupleDesc	indexTupDesc;
	int			i;

	/* ----------------
	 *	open the attribute relation
	 *	XXX ADD INDEXING
	 * ----------------
	 */
	pg_attribute = heap_openr(AttributeRelationName);

	/* ----------------
	 *	initialize null[], replace[] and value[]
	 * ----------------
	 */
	MemSet(nullv, ' ', Natts_pg_attribute);
	MemSet(replace, ' ', Natts_pg_attribute);

	/* ----------------
	 *	create the first attribute tuple.
	 *	XXX For now, only change the ATTNUM attribute value
	 * ----------------
	 */
	replace[Anum_pg_attribute_attnum - 1] = 'r';
	replace[Anum_pg_attribute_attcacheoff - 1] = 'r';

	value[Anum_pg_attribute_attnum - 1] = Int16GetDatum(1);
	value[Anum_pg_attribute_attcacheoff - 1] = Int32GetDatum(-1);

	init_tuple = heap_addheader(Natts_pg_attribute,
						   sizeof *(indexRelation->rd_att->attrs[0]),
						   (char *) (indexRelation->rd_att->attrs[0]));

	hasind = false;
	if (!IsBootstrapProcessingMode() && pg_attribute->rd_rel->relhasindex)
	{
		hasind = true;
		CatalogOpenIndices(Num_pg_attr_indices, Name_pg_attr_indices, idescs);
	}

	/* ----------------
	 *	insert the first attribute tuple.
	 * ----------------
	 */
	cur_tuple = heap_modifytuple(init_tuple,
							 pg_attribute,
							 value,
							 nullv,
							 replace);
	pfree(init_tuple);
	
	heap_insert(pg_attribute, cur_tuple);
	if (hasind)
		CatalogIndexInsert(idescs, Num_pg_attr_indices, pg_attribute, cur_tuple);

	/* ----------------
	 *	now we use the information in the index cur_tuple
	 *	descriptor to form the remaining attribute tuples.
	 * ----------------
	 */
	indexTupDesc = RelationGetTupleDescriptor(indexRelation);

	for (i = 1; i < numatts; i += 1)
	{
		/* ----------------
		 *	process the remaining attributes...
		 * ----------------
		 */
		memmove(GETSTRUCT(cur_tuple),
				(char *) indexTupDesc->attrs[i],
				sizeof(FormData_pg_attribute));

		value[Anum_pg_attribute_attnum - 1] = Int16GetDatum(i + 1);

		new_tuple = heap_modifytuple(cur_tuple,
									pg_attribute,
									value,
									nullv,
									replace);
		pfree(cur_tuple);

		heap_insert(pg_attribute,new_tuple);
		if (hasind)
			CatalogIndexInsert(idescs, Num_pg_attr_indices, pg_attribute, new_tuple);

		/* ----------------
		 *	ModifyHeapTuple returns a new copy of a cur_tuple
		 *	so we free the original and use the copy..
		 * ----------------
		 */
		 cur_tuple = new_tuple;
	}

	if (cur_tuple)
		pfree(cur_tuple);
	heap_close(pg_attribute);
	if (hasind)
		CatalogCloseIndices(Num_pg_attr_indices, idescs);

}

/* ----------------------------------------------------------------
 *		UpdateIndexRelation
 * ----------------------------------------------------------------
 */
static void
UpdateIndexRelation(Oid indexoid,
					Oid heapoid,
					FuncIndexInfo *funcInfo,
					int natts,
					AttrNumber attNums[],
					Oid classOids[],
					Node *predicate,
					List *attributeList,
					bool islossy,
					bool unique)
{
	IndexTupleForm indexForm;
	IndexElem  *IndexKey;
	char	   *predString;
	text	   *predText;
	int			predLen,
				itupLen;
	Relation	pg_index;
	HeapTuple	tuple;
	int			i;

	/* ----------------
	 *	allocate an IndexTupleForm big enough to hold the
	 *	index-predicate (if any) in string form
	 * ----------------
	 */
	if (predicate != NULL)
	{
		predString = nodeToString(predicate);
		predText = (text *) fmgr(F_TEXTIN, predString);
		pfree(predString);
	}
	else
		predText = (text *) fmgr(F_TEXTIN, "");
	predLen = VARSIZE(predText);
	itupLen = predLen + sizeof(FormData_pg_index);
	indexForm = (IndexTupleForm) palloc(itupLen);

	memmove((char *) &indexForm->indpred, (char *) predText, predLen);

	/* ----------------
	 *	store the oid information into the index tuple form
	 * ----------------
	 */
	indexForm->indrelid = heapoid;
	indexForm->indexrelid = indexoid;
	indexForm->indproc = (PointerIsValid(funcInfo)) ?
		FIgetProcOid(funcInfo) : InvalidOid;
	indexForm->indislossy = islossy;
	indexForm->indisunique = unique;

	indexForm->indhaskeytype = 0;
	while (attributeList != NIL)
	{
		IndexKey = (IndexElem *) lfirst(attributeList);
		if (IndexKey->typename != NULL)
		{
			indexForm->indhaskeytype = 1;
			break;
		}
		attributeList = lnext(attributeList);
	}

	MemSet((char *) &indexForm->indkey[0], 0, sizeof indexForm->indkey);
	MemSet((char *) &indexForm->indclass[0], 0, sizeof indexForm->indclass);

	/* ----------------
	 *	copy index key and op class information
	 * ----------------
	 */
	for (i = 0; i < natts; i += 1)
	{
		indexForm->indkey[i] = attNums[i];
		indexForm->indclass[i] = classOids[i];
	}

	/*
	 * If we have a functional index, add all attribute arguments
	 */
	if (PointerIsValid(funcInfo))
	{
		for (i = 1; i < FIgetnArgs(funcInfo); i++)
			indexForm->indkey[i] = attNums[i];
	}

	indexForm->indisclustered = '\0';	/* XXX constant */

	/* ----------------
	 *	open the system catalog index relation
	 * ----------------
	 */
	pg_index = heap_openr(IndexRelationName);

	/* ----------------
	 *	form a tuple to insert into pg_index
	 * ----------------
	 */
	tuple = heap_addheader(Natts_pg_index,
						   itupLen,
						   (char *) indexForm);

	/* ----------------
	 *	insert the tuple into the pg_index
	 *	XXX ADD INDEX TUPLES TOO
	 * ----------------
	 */
	heap_insert(pg_index, tuple);

	/* ----------------
	 *	close the relation and free the tuple
	 * ----------------
	 */
	heap_close(pg_index);
	pfree(predText);
	pfree(indexForm);
	pfree(tuple);
}

/* ----------------------------------------------------------------
 *		UpdateIndexPredicate
 * ----------------------------------------------------------------
 */
void
UpdateIndexPredicate(Oid indexoid, Node *oldPred, Node *predicate)
{
	Node	   *newPred;
	char	   *predString;
	text	   *predText;
	Relation	pg_index;
	HeapTuple	tuple;
	HeapTuple	newtup;
	int			i;
	Datum		values[Natts_pg_index];
	char		nulls[Natts_pg_index];
	char		replace[Natts_pg_index];

	/*
	 * Construct newPred as a CNF expression equivalent to the OR of the
	 * original partial-index predicate ("oldPred") and the extension
	 * predicate ("predicate").
	 *
	 * This should really try to process the result to change things like
	 * "a>2 OR a>1" to simply "a>1", but for now all it does is make sure
	 * that if the extension predicate is NULL (i.e., it is being extended
	 * to be a complete index), then newPred will be NULL - in effect,
	 * changing "a>2 OR TRUE" to "TRUE". --Nels, Jan '93
	 */
	newPred = NULL;
	if (predicate != NULL)
	{
		newPred =
			(Node *) make_orclause(lcons(make_andclause((List *) predicate),
								  lcons(make_andclause((List *) oldPred),
										NIL)));
		newPred = (Node *) cnfify((Expr *) newPred, true);
	}

	/* translate the index-predicate to string form */
	if (newPred != NULL)
	{
		predString = nodeToString(newPred);
		predText = (text *) fmgr(F_TEXTIN, predString);
		pfree(predString);
	}
	else
		predText = (text *) fmgr(F_TEXTIN, "");

	/* open the index system catalog relation */
	pg_index = heap_openr(IndexRelationName);

	tuple = SearchSysCacheTuple(INDEXRELID,
								  ObjectIdGetDatum(indexoid),
								  0, 0, 0);
	Assert(HeapTupleIsValid(tuple));
	
	for (i = 0; i < Natts_pg_index; i++)
	{
		nulls[i] = heap_attisnull(tuple, i + 1) ? 'n' : ' ';
		replace[i] = ' ';
		values[i] = (Datum) NULL;
	}

	replace[Anum_pg_index_indpred - 1] = 'r';
	values[Anum_pg_index_indpred - 1] = (Datum) predText;

	newtup = heap_modifytuple(tuple, pg_index, values, nulls, replace);

	heap_replace(pg_index, &newtup->t_ctid, newtup);

	pfree(newtup);
	heap_close(pg_index);
	pfree(predText);
}

/* ----------------------------------------------------------------
 *		InitIndexStrategy
 * ----------------------------------------------------------------
 */
void
InitIndexStrategy(int numatts,
				  Relation indexRelation,
				  Oid accessMethodObjectId)
{
	IndexStrategy strategy;
	RegProcedure *support;
	uint16		amstrategies;
	uint16		amsupport;
	Oid			attrelid;
	Size		strsize;
	extern GlobalMemory CacheCxt;

	/* ----------------
	 *	get information from the index relation descriptor
	 * ----------------
	 */
	attrelid = indexRelation->rd_att->attrs[0]->attrelid;
	amstrategies = indexRelation->rd_am->amstrategies;
	amsupport = indexRelation->rd_am->amsupport;

	/* ----------------
	 *	get the size of the strategy
	 * ----------------
	 */
	strsize = AttributeNumberGetIndexStrategySize(numatts, amstrategies);

	/* ----------------
	 *	allocate the new index strategy structure
	 *
	 *	the index strategy has to be allocated in the same
	 *	context as the relation descriptor cache or else
	 *	it will be lost at the end of the transaction.
	 * ----------------
	 */
	if (!CacheCxt)
		CacheCxt = CreateGlobalMemory("Cache");

	strategy = (IndexStrategy)
		MemoryContextAlloc((MemoryContext) CacheCxt, strsize);

	if (amsupport > 0)
	{
		strsize = numatts * (amsupport * sizeof(RegProcedure));
		support = (RegProcedure *) MemoryContextAlloc((MemoryContext) CacheCxt,
													  strsize);
	}
	else
		support = (RegProcedure *) NULL;

	/* ----------------
	 *	fill in the index strategy structure with information
	 *	from the catalogs.	Note: we use heap override mode
	 *	in order to be allowed to see the correct information in the
	 *	catalogs, even though our transaction has not yet committed.
	 * ----------------
	 */
	setheapoverride(true);

	IndexSupportInitialize(strategy, support,
						   attrelid, accessMethodObjectId,
						   amstrategies, amsupport, numatts);

	setheapoverride(false);

	/* ----------------
	 *	store the strategy information in the index reldesc
	 * ----------------
	 */
	RelationSetIndexSupport(indexRelation, strategy, support);
}


/* ----------------------------------------------------------------
 *		index_create
 * ----------------------------------------------------------------
 */
void
index_create(char *heapRelationName,
			 char *indexRelationName,
			 FuncIndexInfo *funcInfo,
			 List *attributeList,
			 Oid accessMethodObjectId,
			 int numatts,
			 AttrNumber attNums[],
			 Oid classObjectId[],
			 uint16 parameterCount,
			 Datum *parameter,
			 Node *predicate,
			 bool islossy,
			 bool unique)
{
	Relation	heapRelation;
	Relation	indexRelation;
	TupleDesc	indexTupDesc;
	Oid			heapoid;
	Oid			indexoid;
	PredInfo   *predInfo;

	/* ----------------
	 *	check parameters
	 * ----------------
	 */
	if (numatts < 1)
		elog(ERROR, "must index at least one attribute");

	/* ----------------
	 *	  get heap relation oid and open the heap relation
	 *	  XXX ADD INDEXING
	 * ----------------
	 */
	heapoid = GetHeapRelationOid(heapRelationName, indexRelationName);

	heapRelation = heap_open(heapoid);

	/* ----------------
	 * write lock heap to guarantee exclusive access
	 * ----------------
	RelationSetLockForWrite(heapRelation);
	 *				  ^^^^^
	 * Does it have any sense ?		- vadim 10/27/97
	 */

	RelationSetLockForRead(heapRelation);

	/* ----------------
	 *	  construct new tuple descriptor
	 * ----------------
	 */
	if (PointerIsValid(funcInfo))
		indexTupDesc = BuildFuncTupleDesc(funcInfo);
	else
		indexTupDesc = ConstructTupleDescriptor(heapoid,
												heapRelation,
												attributeList,
												numatts,
												attNums);

	/* ----------------
	 *	create the index relation
	 * ----------------
	 */
	indexRelation = heap_create(indexRelationName,
								indexTupDesc);

	/* ----------------
	 *	  construct the index relation descriptor
	 *
	 *	  XXX should have a proper way to create cataloged relations
	 * ----------------
	 */
	ConstructIndexReldesc(indexRelation, accessMethodObjectId);

	/* ----------------
	 *	  add index to catalogs
	 *	  (append RELATION tuple)
	 * ----------------
	 */
	indexoid = UpdateRelationRelation(indexRelation);

	/* ----------------
	 * Now get the index procedure (only relevant for functional indices).
	 * ----------------
	 */

	if (PointerIsValid(funcInfo))
	{
		HeapTuple	proc_tup;

		proc_tup = SearchSysCacheTuple(PRONAME,
									PointerGetDatum(FIgetname(funcInfo)),
									 Int32GetDatum(FIgetnArgs(funcInfo)),
								 PointerGetDatum(FIgetArglist(funcInfo)),
									   0);

		if (!HeapTupleIsValid(proc_tup))
		{
			func_error("index_create", FIgetname(funcInfo),
					   FIgetnArgs(funcInfo), FIgetArglist(funcInfo), NULL);
		}
		FIgetProcOid(funcInfo) = proc_tup->t_oid;
	}

	/* ----------------
	 *	now update the object id's of all the attribute
	 *	tuple forms in the index relation's tuple descriptor
	 * ----------------
	 */
	InitializeAttributeOids(indexRelation, numatts, indexoid);

	/* ----------------
	 *	  append ATTRIBUTE tuples
	 * ----------------
	 */
	AppendAttributeTuples(indexRelation, numatts);

	/* ----------------
	 *	  update pg_index
	 *	  (append INDEX tuple)
	 *
	 *	  Note that this stows away a representation of "predicate".
	 *	  (Or, could define a rule to maintain the predicate) --Nels, Feb '92
	 * ----------------
	 */
	UpdateIndexRelation(indexoid, heapoid, funcInfo,
						numatts, attNums, classObjectId, predicate,
						attributeList, islossy, unique);

	predInfo = (PredInfo *) palloc(sizeof(PredInfo));
	predInfo->pred = predicate;
	predInfo->oldPred = NULL;

	/* ----------------
	 *	  initialize the index strategy
	 * ----------------
	 */
	InitIndexStrategy(numatts, indexRelation, accessMethodObjectId);

	/*
	 * If this is bootstrap (initdb) time, then we don't actually fill in
	 * the index yet.  We'll be creating more indices and classes later,
	 * so we delay filling them in until just before we're done with
	 * bootstrapping.  Otherwise, we call the routine that constructs the
	 * index.  The heap and index relations are closed by index_build().
	 */
	if (IsBootstrapProcessingMode())
	{
		index_register(heapRelationName, indexRelationName, numatts, attNums,
					   parameterCount, parameter, funcInfo, predInfo);
	}
	else
	{
		heapRelation = heap_openr(heapRelationName);
		index_build(heapRelation, indexRelation, numatts, attNums,
					parameterCount, parameter, funcInfo, predInfo);
	}
}

/* ----------------------------------------------------------------
 *		index_destroy
 *
 *		XXX break into modules like index_create
 * ----------------------------------------------------------------
 */
void
index_destroy(Oid indexId)
{
	Relation	userindexRelation;
	Relation	indexRelation;
	Relation	relationRelation;
	Relation	attributeRelation;
	HeapTuple	tuple;
	int16		attnum;
	
	Assert(OidIsValid(indexId));

	/* why open it here?  bjm 1998/08/20 */
	userindexRelation = index_open(indexId);

	/* ----------------
	 * fix RELATION relation
	 * ----------------
	 */
	relationRelation = heap_openr(RelationRelationName);

	tuple = SearchSysCacheTupleCopy(RELOID,
									ObjectIdGetDatum(indexId),
									0, 0, 0);

	AssertState(HeapTupleIsValid(tuple));

	heap_delete(relationRelation, &tuple->t_ctid);
	pfree(tuple);
	heap_close(relationRelation);

	/* ----------------
	 * fix ATTRIBUTE relation
	 * ----------------
	 */
	attributeRelation = heap_openr(AttributeRelationName);

	attnum = 1; /* indexes start at 1 */

	while (HeapTupleIsValid(tuple = SearchSysCacheTupleCopy(ATTNUM,
									ObjectIdGetDatum(indexId),
									Int16GetDatum(attnum),
									0, 0)))
	{
		heap_delete(attributeRelation, &tuple->t_ctid);
		pfree(tuple);
		attnum++;
	}
	heap_close(attributeRelation);

	/* ----------------
	 * fix INDEX relation
	 * ----------------
	 */
	tuple = SearchSysCacheTupleCopy(INDEXRELID,
									  ObjectIdGetDatum(indexId),
									  0, 0, 0);

	if (!HeapTupleIsValid(tuple))
		elog(NOTICE, "IndexRelationDestroy: %s's INDEX tuple missing",
			 RelationGetRelationName(userindexRelation));

	indexRelation = heap_openr(IndexRelationName);
			         
	heap_delete(indexRelation, &tuple->t_ctid);
	pfree(tuple);
	heap_close(indexRelation);

	/*
	 * flush cache and physically remove the file
	 */
	ReleaseRelationBuffers(userindexRelation);

	if (FileNameUnlink(relpath(userindexRelation->rd_rel->relname.data)) < 0)
		elog(ERROR, "amdestroyr: unlink: %m");

	index_close(userindexRelation);
	RelationForgetRelation(RelationGetRelid(userindexRelation));
}

/* ----------------------------------------------------------------
 *						index_build support
 * ----------------------------------------------------------------
 */
/* ----------------
 *		FormIndexDatum
 * ----------------
 */
void
FormIndexDatum(int numberOfAttributes,
			   AttrNumber attributeNumber[],
			   HeapTuple heapTuple,
			   TupleDesc heapDescriptor,
			   Datum *datum,
			   char *nullv,
			   FuncIndexInfoPtr fInfo)
{
	AttrNumber	i;
	int			offset;
	bool		isNull;

	/* ----------------
	 *	for each attribute we need from the heap tuple,
	 *	get the attribute and stick it into the datum and
	 *	null arrays.
	 * ----------------
	 */

	for (i = 1; i <= numberOfAttributes; i++)
	{
		offset = AttrNumberGetAttrOffset(i);

		datum[offset] =
			PointerGetDatum(GetIndexValue(heapTuple,
										  heapDescriptor,
										  offset,
										  attributeNumber,
										  fInfo,
										  &isNull));

		nullv[offset] = (isNull) ? 'n' : ' ';
	}
}


/* ----------------
 *		UpdateStats
 * ----------------
 */
void
UpdateStats(Oid relid, long reltuples, bool hasindex)
{
	Relation	whichRel;
	Relation	pg_class;
	HeapTuple	tuple;
	HeapTuple	newtup;
	long		relpages;
	int			i;
	Form_pg_class rd_rel;
	Relation	idescs[Num_pg_class_indices];
	Datum		values[Natts_pg_class];
	char		nulls[Natts_pg_class];
	char		replace[Natts_pg_class];
	HeapScanDesc	pg_class_scan = NULL;

	/* ----------------
	 * This routine handles updates for both the heap and index relation
	 * statistics.	In order to guarantee that we're able to *see* the index
	 * relation tuple, we bump the command counter id here.  The index
	 * relation tuple was created in the current transaction.
	 * ----------------
	 */
	CommandCounterIncrement();

	/* ----------------
	 * CommandCounterIncrement() flushes invalid cache entries, including
	 * those for the heap and index relations for which we're updating
	 * statistics.	Now that the cache is flushed, it's safe to open the
	 * relation again.	We need the relation open in order to figure out
	 * how many blocks it contains.
	 * ----------------
	 */

	whichRel = RelationIdGetRelation(relid);

	if (!RelationIsValid(whichRel))
		elog(ERROR, "UpdateStats: cannot open relation id %d", relid);

	/* ----------------
	 * Find the RELATION relation tuple for the given relation.
	 * ----------------
	 */
	pg_class = heap_openr(RelationRelationName);
	if (!RelationIsValid(pg_class))
		elog(ERROR, "UpdateStats: could not open RELATION relation");


	if (!IsBootstrapProcessingMode())
	{
		tuple = SearchSysCacheTupleCopy(RELOID,
										ObjectIdGetDatum(relid),
										0, 0, 0);
	}
	else
	{
		ScanKeyData key[1];

		ScanKeyEntryInitialize(&key[0], 0,
								ObjectIdAttributeNumber,
								F_OIDEQ,
								ObjectIdGetDatum(relid));

		pg_class_scan = heap_beginscan(pg_class, 0, SnapshotNow, 1, key);
		tuple = heap_getnext(pg_class_scan, 0);
	}
	
	if (!HeapTupleIsValid(tuple))
	{
		if (IsBootstrapProcessingMode())
			heap_endscan(pg_class_scan);
		heap_close(pg_class);
		elog(ERROR, "UpdateStats: cannot scan RELATION relation");
	}

	/* ----------------
	 *	update statistics
	 * ----------------
	 */
	relpages = RelationGetNumberOfBlocks(whichRel);

	/*
	 * We shouldn't have to do this, but we do...  Modify the reldesc in
	 * place with the new values so that the cache contains the latest
	 * copy.
	 */

	whichRel->rd_rel->relhasindex = hasindex;
	whichRel->rd_rel->relpages = relpages;
	whichRel->rd_rel->reltuples = reltuples;

	for (i = 0; i < Natts_pg_class; i++)
	{
		nulls[i] = heap_attisnull(tuple, i + 1) ? 'n' : ' ';
		replace[i] = ' ';
		values[i] = (Datum) NULL;
	}

	/*
	 * If reltuples wasn't supplied take an educated guess.
	 */
	if (reltuples == 0)
		reltuples = relpages * NTUPLES_PER_PAGE(whichRel->rd_rel->relnatts);

	if (IsBootstrapProcessingMode())
	{

		/*
		 * At bootstrap time, we don't need to worry about concurrency or
		 * visibility of changes, so we cheat.
		 */
		rd_rel = (Form_pg_class) GETSTRUCT(tuple);
		rd_rel->relpages = relpages;
		rd_rel->reltuples = reltuples;
		rd_rel->relhasindex = hasindex;
		WriteBuffer(pg_class_scan->rs_cbuf);
	}
	else
	{
		/* during normal processing, work harder */
		replace[Anum_pg_class_relpages - 1] = 'r';
		values[Anum_pg_class_relpages - 1] = (Datum) relpages;
		replace[Anum_pg_class_reltuples - 1] = 'r';
		values[Anum_pg_class_reltuples - 1] = (Datum) reltuples;
		replace[Anum_pg_class_relhasindex - 1] = 'r';
		values[Anum_pg_class_relhasindex - 1] = CharGetDatum(hasindex);

		newtup = heap_modifytuple(tuple, pg_class, values, nulls, replace);
		heap_replace(pg_class, &tuple->t_ctid, newtup);
		pfree(newtup);
		CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices, idescs);
		CatalogIndexInsert(idescs, Num_pg_class_indices, pg_class, newtup);
		CatalogCloseIndices(Num_pg_class_indices, idescs);
	}

	if (!IsBootstrapProcessingMode())
		pfree(tuple);
	else
		heap_endscan(pg_class_scan);
	
	heap_close(pg_class);
	heap_close(whichRel);
}


/* -------------------------
 *		FillDummyExprContext
 *			Sets up dummy ExprContext and TupleTableSlot objects for use
 *			with ExecQual.
 * -------------------------
 */
void
FillDummyExprContext(ExprContext *econtext,
					 TupleTableSlot *slot,
					 TupleDesc tupdesc,
					 Buffer buffer)
{
	econtext->ecxt_scantuple = slot;
	econtext->ecxt_innertuple = NULL;
	econtext->ecxt_outertuple = NULL;
	econtext->ecxt_param_list_info = NULL;
	econtext->ecxt_range_table = NULL;

	slot->ttc_tupleDescriptor = tupdesc;
	slot->ttc_buffer = buffer;
	slot->ttc_shouldFree = false;

}


/* ----------------
 *		DefaultBuild
 * ----------------
 */
static void
DefaultBuild(Relation heapRelation,
			 Relation indexRelation,
			 int numberOfAttributes,
			 AttrNumber attributeNumber[],
			 IndexStrategy indexStrategy,		/* not used */
			 uint16 parameterCount,		/* not used */
			 Datum parameter[], /* not used */
			 FuncIndexInfoPtr funcInfo,
			 PredInfo *predInfo)
{
	HeapScanDesc scan;
	HeapTuple	heapTuple;
	IndexTuple	indexTuple;
	TupleDesc	heapDescriptor;
	TupleDesc	indexDescriptor;
	Datum	   *datum;
	char	   *nullv;
	long		reltuples,
				indtuples;

#ifndef OMIT_PARTIAL_INDEX
	ExprContext *econtext;
	TupleTable	tupleTable;
	TupleTableSlot *slot;

#endif
	Node	   *predicate;
	Node	   *oldPred;

	InsertIndexResult insertResult;

	/* ----------------
	 *	more & better checking is needed
	 * ----------------
	 */
	Assert(OidIsValid(indexRelation->rd_rel->relam));	/* XXX */

	/* ----------------
	 *	get the tuple descriptors from the relations so we know
	 *	how to form the index tuples..
	 * ----------------
	 */
	heapDescriptor = RelationGetTupleDescriptor(heapRelation);
	indexDescriptor = RelationGetTupleDescriptor(indexRelation);

	/* ----------------
	 *	datum and null are arrays in which we collect the index attributes
	 *	when forming a new index tuple.
	 * ----------------
	 */
	datum = (Datum *) palloc(numberOfAttributes * sizeof *datum);
	nullv = (char *) palloc(numberOfAttributes * sizeof *nullv);

	/*
	 * If this is a predicate (partial) index, we will need to evaluate
	 * the predicate using ExecQual, which requires the current tuple to
	 * be in a slot of a TupleTable.  In addition, ExecQual must have an
	 * ExprContext referring to that slot.	Here, we initialize dummy
	 * TupleTable and ExprContext objects for this purpose. --Nels, Feb
	 * '92
	 */

	predicate = predInfo->pred;
	oldPred = predInfo->oldPred;

#ifndef OMIT_PARTIAL_INDEX
	if (predicate != NULL || oldPred != NULL)
	{
		tupleTable = ExecCreateTupleTable(1);
		slot = ExecAllocTableSlot(tupleTable);
		econtext = makeNode(ExprContext);
					/* last parameter was junk being sent bjm 1998/08/17 */
		FillDummyExprContext(econtext, slot, heapDescriptor, InvalidBuffer);
	}
	else
	{
		econtext = NULL;
		tupleTable = 0;
		slot = NULL;
	}
#endif							/* OMIT_PARTIAL_INDEX */

	/* ----------------
	 *	Ok, begin our scan of the base relation.
	 * ----------------
	 */
	scan = heap_beginscan(heapRelation, /* relation */
						  0,	/* start at end */
						  SnapshotNow,/* seeself */
						  0,	/* number of keys */
						  (ScanKey) NULL);		/* scan key */

	reltuples = indtuples = 0;

	/* ----------------
	 *	for each tuple in the base relation, we create an index
	 *	tuple and add it to the index relation.  We keep a running
	 *	count of the number of tuples so that we can update pg_class
	 *	with correct statistics when we're done building the index.
	 * ----------------
	 */
	while (HeapTupleIsValid(heapTuple = heap_getnext(scan, 0)))
	{
		reltuples++;

		/*
		 * If oldPred != NULL, this is an EXTEND INDEX command, so skip
		 * this tuple if it was already in the existing partial index
		 */
		if (oldPred != NULL)
		{
#ifndef OMIT_PARTIAL_INDEX
			/* SetSlotContents(slot, heapTuple); */
			slot->val = heapTuple;
			if (ExecQual((List *) oldPred, econtext) == true)
			{
				indtuples++;
				continue;
			}
#endif							/* OMIT_PARTIAL_INDEX */
		}

		/*
		 * Skip this tuple if it doesn't satisfy the partial-index
		 * predicate
		 */
		if (predicate != NULL)
		{
#ifndef OMIT_PARTIAL_INDEX
			/* SetSlotContents(slot, heapTuple); */
			slot->val = heapTuple;
			if (ExecQual((List *) predicate, econtext) == false)
				continue;
#endif							/* OMIT_PARTIAL_INDEX */
		}

		indtuples++;

		/* ----------------
		 *	FormIndexDatum fills in its datum and null parameters
		 *	with attribute information taken from the given heap tuple.
		 * ----------------
		 */
		FormIndexDatum(numberOfAttributes,		/* num attributes */
					   attributeNumber, /* array of att nums to extract */
					   heapTuple,		/* tuple from base relation */
					   heapDescriptor,	/* heap tuple's descriptor */
					   datum,	/* return: array of attributes */
					   nullv,	/* return: array of char's */
					   funcInfo);

		indexTuple = index_formtuple(indexDescriptor,
									 datum,
									 nullv);

		indexTuple->t_tid = heapTuple->t_ctid;

		insertResult = index_insert(indexRelation, datum, nullv,
									&(heapTuple->t_ctid), heapRelation);

		if (insertResult)
			pfree(insertResult);
		pfree(indexTuple);
	}

	heap_endscan(scan);

	if (predicate != NULL || oldPred != NULL)
	{
#ifndef OMIT_PARTIAL_INDEX
		ExecDestroyTupleTable(tupleTable, false);
#endif							/* OMIT_PARTIAL_INDEX */
	}

	pfree(nullv);
	pfree(datum);

	/*
	 * Okay, now update the reltuples and relpages statistics for both the
	 * heap relation and the index.  These statistics are used by the
	 * planner to choose a scan type.  They are maintained generally by
	 * the vacuum daemon, but we update them here to make the index useful
	 * as soon as possible.
	 */
	UpdateStats(RelationGetRelid(heapRelation), reltuples, true);
	UpdateStats(RelationGetRelid(indexRelation), indtuples, false);
	if (oldPred != NULL)
	{
		if (indtuples == reltuples)
			predicate = NULL;
		UpdateIndexPredicate(RelationGetRelid(indexRelation),
							 oldPred, predicate);
	}
}

/* ----------------
 *		index_build
 * ----------------
 */
void
index_build(Relation heapRelation,
			Relation indexRelation,
			int numberOfAttributes,
			AttrNumber attributeNumber[],
			uint16 parameterCount,
			Datum *parameter,
			FuncIndexInfo *funcInfo,
			PredInfo *predInfo)
{
	RegProcedure procedure;

	/* ----------------
	 *	sanity checks
	 * ----------------
	 */
	Assert(RelationIsValid(indexRelation));
	Assert(PointerIsValid(indexRelation->rd_am));

	procedure = indexRelation->rd_am->ambuild;

	/* ----------------
	 *	use the access method build procedure if supplied..
	 * ----------------
	 */
	if (RegProcedureIsValid(procedure))
		fmgr(procedure,
			 heapRelation,
			 indexRelation,
			 numberOfAttributes,
			 attributeNumber,
			 RelationGetIndexStrategy(indexRelation),
			 parameterCount,
			 parameter,
			 funcInfo,
			 predInfo);
	else
		DefaultBuild(heapRelation,
					 indexRelation,
					 numberOfAttributes,
					 attributeNumber,
					 RelationGetIndexStrategy(indexRelation),
					 parameterCount,
					 parameter,
					 funcInfo,
					 predInfo);
}

/*
 * IndexIsUnique: given an index's relation OID, see if it
 * is unique using the system cache.
 */
bool
IndexIsUnique(Oid indexId)
{
	HeapTuple	tuple;
	IndexTupleForm index;

	tuple = SearchSysCacheTuple(INDEXRELID,
								ObjectIdGetDatum(indexId),
								0, 0, 0);
	if (!HeapTupleIsValid(tuple))
	{
		elog(ERROR, "IndexIsUnique: can't find index id %d",
			 indexId);
	}
	index = (IndexTupleForm) GETSTRUCT(tuple);
	Assert(index->indexrelid == indexId);

	return index->indisunique;
}

/*
 * IndexIsUniqueNoCache: same as above function, but don't use the
 * system cache.  if we are called from btbuild, the transaction
 * that is adding the entry to pg_index has not been committed yet.
 * the system cache functions will do a heap scan, but only with
 * NowTimeQual, not SelfTimeQual, so it won't find tuples added
 * by the current transaction (which is good, because if the transaction
 * is aborted, you don't want the tuples sitting around in the cache).
 * so anyway, we have to do our own scan with SelfTimeQual.
 * this is only called when a new index is created, so it's OK
 * if it's slow.
 */
bool
IndexIsUniqueNoCache(Oid indexId)
{
	Relation	pg_index;
	ScanKeyData skey[1];
	HeapScanDesc scandesc;
	HeapTuple	tuple;
	IndexTupleForm index;
	bool		isunique;

	pg_index = heap_openr(IndexRelationName);

	ScanKeyEntryInitialize(&skey[0], (bits16) 0x0,
						   Anum_pg_index_indexrelid,
						   (RegProcedure) F_OIDEQ,
						   ObjectIdGetDatum(indexId));

	scandesc = heap_beginscan(pg_index, 0, SnapshotSelf, 1, skey);

	/* NO CACHE */
	tuple = heap_getnext(scandesc, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "IndexIsUniqueNoCache: can't find index id %d", indexId);

	index = (IndexTupleForm) GETSTRUCT(tuple);
	Assert(index->indexrelid == indexId);
	isunique = index->indisunique;

	heap_endscan(scandesc);
	heap_close(pg_index);
	return isunique;
}
