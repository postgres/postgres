/*-------------------------------------------------------------------------
 *
 * creatinh.c
 *	  POSTGRES create/destroy relation with inheritance utility code.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/Attic/creatinh.c,v 1.67 2000/11/16 22:30:18 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/heap.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_ipl.h"
#include "catalog/pg_type.h"
#include "commands/creatinh.h"
#include "miscadmin.h"
#include "utils/syscache.h"
#include "optimizer/clauses.h"

/* ----------------
 *		local stuff
 * ----------------
 */

static int checkAttrExists(const char *attributeName,
				const char *attributeType, List *schema);
static List *MergeAttributes(List *schema, List *supers, List **supconstr);
static void StoreCatalogInheritance(Oid relationId, List *supers);
static void
setRelhassubclassInRelation(Oid relationId, bool relhassubclass);


/* ----------------------------------------------------------------
 *		DefineRelation
 *				Creates a new relation.
 * ----------------------------------------------------------------
 */
void
DefineRelation(CreateStmt *stmt, char relkind)
{
	char	   *relname = palloc(NAMEDATALEN);
	List	   *schema = stmt->tableElts;
	int			numberOfAttributes;
	Oid			relationId;
	Relation	rel;
	List	   *inheritList;
	TupleDesc	descriptor;
	List	   *old_constraints;
	List	   *rawDefaults;
	List	   *listptr;
	int			i;
	AttrNumber	attnum;

	if (strlen(stmt->relname) >= NAMEDATALEN)
		elog(ERROR, "the relation name %s is >= %d characters long",
			 stmt->relname, NAMEDATALEN);
	StrNCpy(relname, stmt->relname, NAMEDATALEN);

	/* ----------------
	 *	Handle parameters
	 *	XXX parameter handling missing below.
	 * ----------------
	 */
	inheritList = stmt->inhRelnames;

	/* ----------------
	 *	generate relation schema, including inherited attributes.
	 * ----------------
	 */
	schema = MergeAttributes(schema, inheritList, &old_constraints);

	numberOfAttributes = length(schema);
	if (numberOfAttributes <= 0)
	{
		elog(ERROR, "DefineRelation: %s",
			 "please inherit from a relation or define an attribute");
	}

	/* ----------------
	 *	create a relation descriptor from the relation schema
	 *	and create the relation.  Note that in this stage only
	 *	inherited (pre-cooked) defaults and constraints will be
	 *	included into the new relation.  (BuildDescForRelation
	 *	takes care of the inherited defaults, but we have to copy
	 *	inherited constraints here.)
	 * ----------------
	 */
	descriptor = BuildDescForRelation(schema, relname);

	if (old_constraints != NIL)
	{
		ConstrCheck *check = (ConstrCheck *) palloc(length(old_constraints) *
													sizeof(ConstrCheck));
		int			ncheck = 0;

		foreach(listptr, old_constraints)
		{
			Constraint *cdef = (Constraint *) lfirst(listptr);

			if (cdef->contype != CONSTR_CHECK)
				continue;

			if (cdef->name != NULL)
			{
				for (i = 0; i < ncheck; i++)
				{
					if (strcmp(check[i].ccname, cdef->name) == 0)
						elog(ERROR, "Duplicate CHECK constraint name: '%s'",
							 cdef->name);
				}
				check[ncheck].ccname = cdef->name;
			}
			else
			{
				check[ncheck].ccname = (char *) palloc(NAMEDATALEN);
				snprintf(check[ncheck].ccname, NAMEDATALEN, "$%d", ncheck + 1);
			}
			Assert(cdef->raw_expr == NULL && cdef->cooked_expr != NULL);
			check[ncheck].ccbin = pstrdup(cdef->cooked_expr);
			ncheck++;
		}
		if (ncheck > 0)
		{
			if (descriptor->constr == NULL)
			{
				descriptor->constr = (TupleConstr *) palloc(sizeof(TupleConstr));
				descriptor->constr->defval = NULL;
				descriptor->constr->num_defval = 0;
				descriptor->constr->has_not_null = false;
			}
			descriptor->constr->num_check = ncheck;
			descriptor->constr->check = check;
		}
	}

	relationId = heap_create_with_catalog(relname, descriptor,
										  relkind, stmt->istemp,
										  allowSystemTableMods);

	StoreCatalogInheritance(relationId, inheritList);

	/*
	 * We must bump the command counter to make the newly-created relation
	 * tuple visible for opening.
	 */
	CommandCounterIncrement();

	/*
	 * Open the new relation and acquire exclusive lock on it.  This isn't
	 * really necessary for locking out other backends (since they can't
	 * see the new rel anyway until we commit), but it keeps the lock manager
	 * from complaining about deadlock risks.
	 */
	rel = heap_openr(relname, AccessExclusiveLock);

	/*
	 * Now add any newly specified column default values and CHECK
	 * constraints to the new relation.  These are passed to us in the
	 * form of raw parsetrees; we need to transform them to executable
	 * expression trees before they can be added. The most convenient way
	 * to do that is to apply the parser's transformExpr routine, but
	 * transformExpr doesn't work unless we have a pre-existing relation.
	 * So, the transformation has to be postponed to this final step of
	 * CREATE TABLE.
	 *
	 * First, scan schema to find new column defaults.
	 */
	rawDefaults = NIL;
	attnum = 0;

	foreach(listptr, schema)
	{
		ColumnDef  *colDef = lfirst(listptr);
		RawColumnDefault *rawEnt;

		attnum++;

		if (colDef->raw_default == NULL)
			continue;
		Assert(colDef->cooked_default == NULL);

		rawEnt = (RawColumnDefault *) palloc(sizeof(RawColumnDefault));
		rawEnt->attnum = attnum;
		rawEnt->raw_default = colDef->raw_default;
		rawDefaults = lappend(rawDefaults, rawEnt);
	}

	/*
	 * Parse and add the defaults/constraints, if any.
	 */
	if (rawDefaults || stmt->constraints)
		AddRelationRawConstraints(rel, rawDefaults, stmt->constraints);

	/*
	 * Clean up.  We keep lock on new relation (although it shouldn't be
	 * visible to anyone else anyway, until commit).
	 */
	heap_close(rel, NoLock);
}

/*
 * RemoveRelation
 *		Deletes a new relation.
 *
 * Exceptions:
 *		BadArg if name is invalid.
 *
 * Note:
 *		If the relation has indices defined on it, then the index relations
 * themselves will be destroyed, too.
 */
void
RemoveRelation(char *name)
{
	AssertArg(name);
	heap_drop_with_catalog(name, allowSystemTableMods);
}

/*
 * TruncateRelation --
 *				  Removes all the rows from a relation
 *
 * Exceptions:
 *				  BadArg if name is invalid
 *
 * Note:
 *				  Rows are removed, indices are truncated and reconstructed.
 */
void
TruncateRelation(char *name)
{
	AssertArg(name);
	heap_truncate(name);
}

/*
 * complementary static functions for MergeAttributes().
 * Varattnos of pg_relcheck.rcbin should be rewritten when
 * subclasses inherit the constraints from the super class.
 * Note that these functions rewrite varattnos while walking
 * through a node tree. 
 */
static bool
change_varattnos_walker(Node *node, const AttrNumber *newattno)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var	*var = (Var *) node;

		if (var->varlevelsup == 0 && var->varno == 1)
		{
			/*
			 * ??? the following may be a problem when the
			 * node is multiply referenced though
			 * stringToNode() doesn't create such a node
			 * currently. 
			 */
			Assert(newattno[var->varattno - 1] > 0);
			var->varattno = newattno[var->varattno - 1];
		}
		return false;
	}
	return expression_tree_walker(node, change_varattnos_walker,
								  (void *) newattno);
}

static bool
change_varattnos_of_a_node(Node *node, const AttrNumber *newattno)
{
	return change_varattnos_walker(node, newattno);
}

/*
 * MergeAttributes
 *		Returns new schema given initial schema and supers.
 *
 *
 * 'schema' is the column/attribute definition for the table. (It's a list
 *		of ColumnDef's.) It is destructively changed.
 * 'inheritList' is the list of inherited relations (a list of Value(str)'s).
 *
 * Notes:
 *	  The order in which the attributes are inherited is very important.
 *	  Intuitively, the inherited attributes should come first. If a table
 *	  inherits from multiple parents, the order of those attributes are
 *	  according to the order of the parents specified in CREATE TABLE.
 *
 *	  Here's an example:
 *
 *		create table person (name text, age int4, location point);
 *		create table emp (salary int4, manager text) inherits(person);
 *		create table student (gpa float8) inherits (person);
 *		create table stud_emp (percent int4) inherits (emp, student);
 *
 *	  the order of the attributes of stud_emp is as follow:
 *
 *
 *							person {1:name, 2:age, 3:location}
 *							/	 \
 *			   {6:gpa}	student   emp {4:salary, 5:manager}
 *							\	 /
 *						   stud_emp {7:percent}
 */
static List *
MergeAttributes(List *schema, List *supers, List **supconstr)
{
	List	   *entry;
	List	   *inhSchema = NIL;
	List	   *constraints = NIL;
	int		attnums;

	/*
	 * Validates that there are no duplications. Validity checking of
	 * types occurs later.
	 */
	foreach(entry, schema)
	{
		ColumnDef  *coldef = lfirst(entry);
		List	   *rest;

		foreach(rest, lnext(entry))
		{

			/*
			 * check for duplicated names within the new relation
			 */
			ColumnDef  *restdef = lfirst(rest);

			if (!strcmp(coldef->colname, restdef->colname))
			{
				elog(ERROR, "CREATE TABLE: attribute \"%s\" duplicated",
					 coldef->colname);
			}
		}
	}
	foreach(entry, supers)
	{
		List	   *rest;

		foreach(rest, lnext(entry))
		{
			if (!strcmp(strVal(lfirst(entry)), strVal(lfirst(rest))))
			{
				elog(ERROR, "CREATE TABLE: inherited relation \"%s\" duplicated",
					 strVal(lfirst(entry)));
			}
		}
	}

	/*
	 * merge the inherited attributes into the schema
	 */
	attnums = 0;
	foreach(entry, supers)
	{
		char	   *name = strVal(lfirst(entry));
		Relation	relation;
		List	   *partialResult = NIL;
		AttrNumber	attrno;
		TupleDesc	tupleDesc;
		TupleConstr *constr;
		AttrNumber	*newattno, *partialAttidx;
		Node		*expr;
		int		i, attidx, attno_exist;

		relation = heap_openr(name, AccessShareLock);
		setRelhassubclassInRelation(relation->rd_id, true);
		tupleDesc = RelationGetDescr(relation);
		/* allocate a new attribute number table and initialize */
		newattno = (AttrNumber *) palloc(tupleDesc->natts * sizeof(AttrNumber));
		for (i = 0; i < tupleDesc->natts; i++)
			newattno [i] = 0;
		/*
		 * searching and storing order are different.
		 * another table is needed.
		 */ 
		partialAttidx = (AttrNumber *) palloc(tupleDesc->natts * sizeof(AttrNumber));
		for (i = 0; i < tupleDesc->natts; i++)
			partialAttidx [i] = 0;
		constr = tupleDesc->constr;

		if (relation->rd_rel->relkind != RELKIND_RELATION)
			elog(ERROR, "CREATE TABLE: inherited relation \"%s\" is not a table", name);

		attidx = 0;
		for (attrno = relation->rd_rel->relnatts - 1; attrno >= 0; attrno--)
		{
			Form_pg_attribute attribute = tupleDesc->attrs[attrno];
			char	   *attributeName;
			char	   *attributeType;
			HeapTuple	tuple;
			ColumnDef  *def;
			TypeName   *typename;

			/*
			 * form name, type and constraints
			 */
			attributeName = NameStr(attribute->attname);
			tuple = SearchSysCache(TYPEOID,
								   ObjectIdGetDatum(attribute->atttypid),
								   0, 0, 0);
			if (!HeapTupleIsValid(tuple))
				elog(ERROR, "CREATE TABLE: cache lookup failed for type %u",
					 attribute->atttypid);
			attributeType = pstrdup(NameStr(((Form_pg_type) GETSTRUCT(tuple))->typname));
			ReleaseSysCache(tuple);

			/*
			 * check validity
			 *
			 */
			if (checkAttrExists(attributeName, attributeType, schema) != 0)
				elog(ERROR, "CREATE TABLE: attribute \"%s\" already exists in inherited schema",
					 attributeName);

			if (0 < (attno_exist = checkAttrExists(attributeName, attributeType, inhSchema)))
			{

				/*
				 * this entry already exists
				 */
				newattno[attribute->attnum - 1] = attno_exist;
				continue;
			}
			attidx++;
			partialAttidx[attribute->attnum - 1] = attidx;

			/*
			 * add an entry to the schema
			 */
			def = makeNode(ColumnDef);
			typename = makeNode(TypeName);
			def->colname = pstrdup(attributeName);
			typename->name = pstrdup(attributeType);
			typename->typmod = attribute->atttypmod;
			def->typename = typename;
			def->is_not_null = attribute->attnotnull;
			def->raw_default = NULL;
			def->cooked_default = NULL;
			if (attribute->atthasdef)
			{
				AttrDefault *attrdef;
				int			i;

				Assert(constr != NULL);

				attrdef = constr->defval;
				for (i = 0; i < constr->num_defval; i++)
				{
					if (attrdef[i].adnum == attrno + 1)
					{
						def->cooked_default = pstrdup(attrdef[i].adbin);
						break;
					}
				}
				Assert(def->cooked_default != NULL);
			}
			partialResult = lcons(def, partialResult);
		}
		for (i = 0; i < tupleDesc->natts; i++)
		{
			if (partialAttidx[i] > 0)
				newattno[i] = attnums + attidx + 1 - partialAttidx[i];
		}
		attnums += attidx;
		pfree(partialAttidx);

		if (constr && constr->num_check > 0)
		{
			ConstrCheck *check = constr->check;
			int			i;

			for (i = 0; i < constr->num_check; i++)
			{
				Constraint *cdef = makeNode(Constraint);

				cdef->contype = CONSTR_CHECK;
				if (check[i].ccname[0] == '$')
					cdef->name = NULL;
				else
					cdef->name = pstrdup(check[i].ccname);
				cdef->raw_expr = NULL;
				/* adjust varattnos of ccbin here */
				expr = stringToNode(check[i].ccbin);
				change_varattnos_of_a_node(expr, newattno);
				cdef->cooked_expr = nodeToString(expr);
				constraints = lappend(constraints, cdef);
			}
		}
		pfree(newattno);

		/*
		 * Close the parent rel, but keep our AccessShareLock on it until
		 * xact commit.  That will prevent someone else from deleting or
		 * ALTERing the parent before the child is committed.
		 */
		heap_close(relation, NoLock);

		/*
		 * wants the inherited schema to appear in the order they are
		 * specified in CREATE TABLE
		 */
		inhSchema = nconc(inhSchema, partialResult);
	}

	/*
	 * put the inherited schema before our the schema for this table
	 */
	schema = nconc(inhSchema, schema);
	*supconstr = constraints;
	return schema;
}

/*
 * StoreCatalogInheritance
 *		Updates the system catalogs with proper inheritance information.
 */
static void
StoreCatalogInheritance(Oid relationId, List *supers)
{
	Relation	relation;
	TupleDesc	desc;
	int16		seqNumber;
	List	   *entry;
	List	   *idList;
	HeapTuple	tuple;

	/* ----------------
	 *	sanity checks
	 * ----------------
	 */
	AssertArg(OidIsValid(relationId));

	if (supers == NIL)
		return;

	/* ----------------
	 * Catalog INHERITS information.
	 * ----------------
	 */
	relation = heap_openr(InheritsRelationName, RowExclusiveLock);
	desc = RelationGetDescr(relation);

	seqNumber = 1;
	idList = NIL;
	foreach(entry, supers)
	{
		Oid			entryOid;
		Datum		datum[Natts_pg_inherits];
		char		nullarr[Natts_pg_inherits];

		entryOid = GetSysCacheOid(RELNAME,
								  PointerGetDatum(strVal(lfirst(entry))),
								  0, 0, 0);
		if (!OidIsValid(entryOid))
			elog(ERROR, "StoreCatalogInheritance: cache lookup failed for relation \"%s\"",
				 strVal(lfirst(entry)));

		/*
		 * build idList for use below
		 */
		idList = lappendi(idList, entryOid);

		datum[0] = ObjectIdGetDatum(relationId);	/* inhrel */
		datum[1] = ObjectIdGetDatum(entryOid);		/* inhparent */
		datum[2] = Int16GetDatum(seqNumber);		/* inhseqno */

		nullarr[0] = ' ';
		nullarr[1] = ' ';
		nullarr[2] = ' ';

		tuple = heap_formtuple(desc, datum, nullarr);

		heap_insert(relation, tuple);

		if (RelationGetForm(relation)->relhasindex)
		{
			Relation	idescs[Num_pg_inherits_indices];

			CatalogOpenIndices(Num_pg_inherits_indices, Name_pg_inherits_indices, idescs);
			CatalogIndexInsert(idescs, Num_pg_inherits_indices, relation, tuple);
			CatalogCloseIndices(Num_pg_inherits_indices, idescs);
		}

		heap_freetuple(tuple);

		seqNumber += 1;
	}

	heap_close(relation, RowExclusiveLock);

	/* ----------------
	 * Catalog IPL information.
	 *
	 * Algorithm:
	 *	0. list superclasses (by Oid) in order given (see idList).
	 *	1. append after each relationId, its superclasses, recursively.
	 *	3. remove all but last of duplicates.
	 *	4. store result.
	 * ----------------
	 */

	/* ----------------
	 *	1.
	 * ----------------
	 */
	foreach(entry, idList)
	{
		HeapTuple	tuple;
		Oid			id;
		int16		number;
		List	   *next;
		List	   *current;

		id = (Oid) lfirsti(entry);
		current = entry;
		next = lnext(entry);

		for (number = 1;; number += 1)
		{
			tuple = SearchSysCache(INHRELID,
								   ObjectIdGetDatum(id),
								   Int16GetDatum(number),
								   0, 0);
			if (!HeapTupleIsValid(tuple))
				break;

			lnext(current) = lconsi(((Form_pg_inherits)
									 GETSTRUCT(tuple))->inhparent,
									NIL);

			ReleaseSysCache(tuple);

			current = lnext(current);
		}
		lnext(current) = next;
	}

	/* ----------------
	 *	2.
	 * ----------------
	 */
	foreach(entry, idList)
	{
		Oid			name;
		List	   *rest;
		bool		found = false;

again:
		name = lfirsti(entry);
		foreach(rest, lnext(entry))
		{
			if (name == lfirsti(rest))
			{
				found = true;
				break;
			}
		}
		if (found)
		{

			/*
			 * entry list must be of length >= 2 or else no match
			 *
			 * so, remove this entry.
			 */
			lfirst(entry) = lfirst(lnext(entry));
			lnext(entry) = lnext(lnext(entry));

			found = false;
			goto again;
		}
	}

	/* ----------------
	 *	3.
	 * ----------------
	 */
	relation = heap_openr(InheritancePrecidenceListRelationName, RowExclusiveLock);
	desc = RelationGetDescr(relation);

	seqNumber = 1;

	foreach(entry, idList)
	{
		Datum		datum[Natts_pg_ipl];
		char		nullarr[Natts_pg_ipl];

		datum[0] = ObjectIdGetDatum(relationId);		/* iplrel */
		datum[1] = ObjectIdGetDatum(lfirsti(entry));
		/* iplinherits */
		datum[2] = Int16GetDatum(seqNumber);	/* iplseqno */

		nullarr[0] = ' ';
		nullarr[1] = ' ';
		nullarr[2] = ' ';

		tuple = heap_formtuple(desc, datum, nullarr);

		heap_insert(relation, tuple);
		heap_freetuple(tuple);

		seqNumber += 1;
	}

	heap_close(relation, RowExclusiveLock);
}



/*
 * returns the index(star with 1) if attribute already exists in schema, 0 otherwise.
 */
static int
checkAttrExists(const char *attributeName, const char *attributeType, List *schema)
{
	List	   *s;
	int	i = 0;

	foreach(s, schema)
	{
		ColumnDef  *def = lfirst(s);

		++i;
		if (strcmp(attributeName, def->colname) == 0)
		{

			/*
			 * attribute exists. Make sure the types are the same.
			 */
			if (strcmp(attributeType, def->typename->name) != 0)
				elog(ERROR, "CREATE TABLE: attribute \"%s\" type conflict (%s and %s)",
					 attributeName, attributeType, def->typename->name);
			return i;
		}
	}
	return 0;
}


static void
setRelhassubclassInRelation(Oid relationId, bool relhassubclass)
{
	Relation		relationRelation;
	HeapTuple       tuple;
	Relation        idescs[Num_pg_class_indices];

	/*
	 * Fetch a modifiable copy of the tuple, modify it, update pg_class.
	 */
	relationRelation = heap_openr(RelationRelationName, RowExclusiveLock);
	tuple = SearchSysCacheCopy(RELOID,
							   ObjectIdGetDatum(relationId),
							   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "setRelhassubclassInRelation: cache lookup failed for relation %u", relationId);

	((Form_pg_class) GETSTRUCT(tuple))->relhassubclass = relhassubclass;
	heap_update(relationRelation, &tuple->t_self, tuple, NULL);

	/* keep the catalog indices up to date */
	CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices, idescs);
	CatalogIndexInsert(idescs, Num_pg_class_indices, relationRelation, tuple);
	CatalogCloseIndices(Num_pg_class_indices, idescs);

	heap_freetuple(tuple);
	heap_close(relationRelation, RowExclusiveLock);
}
