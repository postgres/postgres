/*-------------------------------------------------------------------------
 *
 * creatinh.c
 *	  POSTGRES create/destroy relation with inheritance utility code.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/Attic/creatinh.c,v 1.75 2001/03/30 20:50:36 tgl Exp $
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
#include "optimizer/clauses.h"
#include "utils/acl.h"
#include "utils/syscache.h"
#include "utils/temprel.h"

/* ----------------
 *		local stuff
 * ----------------
 */

static List *MergeAttributes(List *schema, List *supers, bool istemp,
				List **supOids, List **supconstr);
static bool change_varattnos_of_a_node(Node *node, const AttrNumber *newattno);
static void StoreCatalogInheritance(Oid relationId, List *supers);
static int findAttrByName(const char *attributeName, List *schema);
static void setRelhassubclassInRelation(Oid relationId, bool relhassubclass);


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
	TupleDesc	descriptor;
	List	   *inheritOids;
	List	   *old_constraints;
	List	   *rawDefaults;
	List	   *listptr;
	int			i;
	AttrNumber	attnum;

	/*
	 * Truncate relname to appropriate length (probably a waste of time,
	 * as parser should have done this already).
	 */
	StrNCpy(relname, stmt->relname, NAMEDATALEN);

	/*
	 * Look up inheritance ancestors and generate relation schema,
	 * including inherited attributes.
	 */
	schema = MergeAttributes(schema, stmt->inhRelnames, stmt->istemp,
							 &inheritOids, &old_constraints);

	numberOfAttributes = length(schema);
	if (numberOfAttributes <= 0)
		elog(ERROR, "DefineRelation: please inherit from a relation or define an attribute");

	/*
	 * Create a relation descriptor from the relation schema and create
	 * the relation.  Note that in this stage only inherited (pre-cooked)
	 * defaults and constraints will be included into the new relation.
	 * (BuildDescForRelation takes care of the inherited defaults, but we
	 * have to copy inherited constraints here.)
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

	StoreCatalogInheritance(relationId, inheritOids);

	/*
	 * We must bump the command counter to make the newly-created relation
	 * tuple visible for opening.
	 */
	CommandCounterIncrement();

	/*
	 * Open the new relation and acquire exclusive lock on it.	This isn't
	 * really necessary for locking out other backends (since they can't
	 * see the new rel anyway until we commit), but it keeps the lock
	 * manager from complaining about deadlock risks.
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

/*----------
 * MergeAttributes
 *		Returns new schema given initial schema and superclasses.
 *
 * Input arguments:
 * 'schema' is the column/attribute definition for the table. (It's a list
 *		of ColumnDef's.) It is destructively changed.
 * 'supers' is a list of names (as Value objects) of parent relations.
 * 'istemp' is TRUE if we are creating a temp relation.
 *
 * Output arguments:
 * 'supOids' receives an integer list of the OIDs of the parent relations.
 * 'supconstr' receives a list of constraints belonging to the parents,
 *		updated as necessary to be valid for the child.
 *
 * Return value:
 * Completed schema list.
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
 *	  The order of the attributes of stud_emp is:
 *
 *							person {1:name, 2:age, 3:location}
 *							/	 \
 *			   {6:gpa}	student   emp {4:salary, 5:manager}
 *							\	 /
 *						   stud_emp {7:percent}
 *
 *	   If the same attribute name appears multiple times, then it appears
 *	   in the result table in the proper location for its first appearance.
 *----------
 */
static List *
MergeAttributes(List *schema, List *supers, bool istemp,
				List **supOids, List **supconstr)
{
	List	   *entry;
	List	   *inhSchema = NIL;
	List	   *parentOids = NIL;
	List	   *constraints = NIL;
	int			child_attno;

	/*
	 * Check for duplicate names in the explicit list of attributes.
	 *
	 * Although we might consider merging such entries in the same way that
	 * we handle name conflicts for inherited attributes, it seems to make
	 * more sense to assume such conflicts are errors.
	 */
	foreach(entry, schema)
	{
		ColumnDef  *coldef = lfirst(entry);
		List	   *rest;

		foreach(rest, lnext(entry))
		{
			ColumnDef  *restdef = lfirst(rest);

			if (strcmp(coldef->colname, restdef->colname) == 0)
			{
				elog(ERROR, "CREATE TABLE: attribute \"%s\" duplicated",
					 coldef->colname);
			}
		}
	}
	/*
	 * Reject duplicate names in the list of parents, too.
	 */
	foreach(entry, supers)
	{
		List	   *rest;

		foreach(rest, lnext(entry))
		{
			if (strcmp(strVal(lfirst(entry)), strVal(lfirst(rest))) == 0)
			{
				elog(ERROR, "CREATE TABLE: inherited relation \"%s\" duplicated",
					 strVal(lfirst(entry)));
			}
		}
	}

	/*
	 * Scan the parents left-to-right, and merge their attributes to form
	 * a list of inherited attributes (inhSchema).
	 */
	child_attno = 0;
	foreach(entry, supers)
	{
		char	   *name = strVal(lfirst(entry));
		Relation	relation;
		TupleDesc	tupleDesc;
		TupleConstr *constr;
		AttrNumber *newattno;
		AttrNumber	parent_attno;

		relation = heap_openr(name, AccessShareLock);

		if (relation->rd_rel->relkind != RELKIND_RELATION)
			elog(ERROR, "CREATE TABLE: inherited relation \"%s\" is not a table", name);
		/* Permanent rels cannot inherit from temporary ones */
		if (!istemp && is_temp_rel_name(name))
			elog(ERROR, "CREATE TABLE: cannot inherit from temp relation \"%s\"", name);

		/*
		 * We should have an UNDER permission flag for this, but for now,
		 * demand that creator of a child table own the parent.
		 */
		if (!pg_ownercheck(GetUserId(), name, RELNAME))
			elog(ERROR, "you do not own table \"%s\"", name);

		parentOids = lappendi(parentOids, relation->rd_id);
		setRelhassubclassInRelation(relation->rd_id, true);

		tupleDesc = RelationGetDescr(relation);
		constr = tupleDesc->constr;

		/*
		 * newattno[] will contain the child-table attribute numbers for
		 * the attributes of this parent table.  (They are not the same
		 * for parents after the first one.)
		 */
		newattno = (AttrNumber *) palloc(tupleDesc->natts*sizeof(AttrNumber));

		for (parent_attno = 1; parent_attno <= tupleDesc->natts;
			 parent_attno++)
		{
			Form_pg_attribute attribute = tupleDesc->attrs[parent_attno - 1];
			char	   *attributeName;
			char	   *attributeType;
			HeapTuple	tuple;
			int			exist_attno;
			ColumnDef  *def;
			TypeName   *typename;

			/*
			 * Get name and type name of attribute
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
			 * Does it conflict with some previously inherited column?
			 */
			exist_attno = findAttrByName(attributeName, inhSchema);
			if (exist_attno > 0)
			{
				/*
				 * Yes, try to merge the two column definitions.
				 * They must have the same type and typmod.
				 */
				elog(NOTICE, "CREATE TABLE: merging multiple inherited definitions of attribute \"%s\"",
					 attributeName);
				def = (ColumnDef *) nth(exist_attno - 1, inhSchema);
				if (strcmp(def->typename->name, attributeType) != 0 ||
					def->typename->typmod != attribute->atttypmod)
					elog(ERROR, "CREATE TABLE: inherited attribute \"%s\" type conflict (%s and %s)",
						 attributeName, def->typename->name, attributeType);
				/* Merge of NOT NULL constraints = OR 'em together */
				def->is_not_null |= attribute->attnotnull;
				/* Default and other constraints are handled below */
				newattno[parent_attno - 1] = exist_attno;
			}
			else
			{
				/*
				 * No, create a new inherited column
				 */
				def = makeNode(ColumnDef);
				def->colname = pstrdup(attributeName);
				typename = makeNode(TypeName);
				typename->name = attributeType;
				typename->typmod = attribute->atttypmod;
				def->typename = typename;
				def->is_not_null = attribute->attnotnull;
				def->is_sequence = false;
				def->raw_default = NULL;
				def->cooked_default = NULL;
				def->constraints = NIL;
				inhSchema = lappend(inhSchema, def);
				newattno[parent_attno - 1] = ++child_attno;
			}
			/*
			 * Copy default if any, overriding any default from earlier parent
			 */
			if (attribute->atthasdef)
			{
				AttrDefault *attrdef;
				int			i;

				def->raw_default = NULL;
				def->cooked_default = NULL;

				Assert(constr != NULL);
				attrdef = constr->defval;
				for (i = 0; i < constr->num_defval; i++)
				{
					if (attrdef[i].adnum == parent_attno)
					{
						/*
						 * if default expr could contain any vars, we'd
						 * need to fix 'em, but it can't ...
						 */
						def->cooked_default = pstrdup(attrdef[i].adbin);
						break;
					}
				}
				Assert(def->cooked_default != NULL);
			}
		}
		/*
		 * Now copy the constraints of this parent, adjusting attnos using
		 * the completed newattno[] map
		 */
		if (constr && constr->num_check > 0)
		{
			ConstrCheck *check = constr->check;
			int			i;

			for (i = 0; i < constr->num_check; i++)
			{
				Constraint *cdef = makeNode(Constraint);
				Node	   *expr;

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
	}

	/*
	 * If we had no inherited attributes, the result schema is just the
	 * explicitly declared columns.  Otherwise, we need to merge the
	 * declared columns into the inherited schema list.
	 */
	if (inhSchema != NIL)
	{
		foreach(entry, schema)
		{
			ColumnDef  *newdef = lfirst(entry);
			char	   *attributeName = newdef->colname;
			char	   *attributeType = newdef->typename->name;
			int			exist_attno;

			/*
			 * Does it conflict with some previously inherited column?
			 */
			exist_attno = findAttrByName(attributeName, inhSchema);
			if (exist_attno > 0)
			{
				ColumnDef  *def;

				/*
				 * Yes, try to merge the two column definitions.
				 * They must have the same type and typmod.
				 */
				elog(NOTICE, "CREATE TABLE: merging attribute \"%s\" with inherited definition",
					 attributeName);
				def = (ColumnDef *) nth(exist_attno - 1, inhSchema);
				if (strcmp(def->typename->name, attributeType) != 0 ||
					def->typename->typmod != newdef->typename->typmod)
					elog(ERROR, "CREATE TABLE: attribute \"%s\" type conflict (%s and %s)",
						 attributeName, def->typename->name, attributeType);
				/* Merge of NOT NULL constraints = OR 'em together */
				def->is_not_null |= newdef->is_not_null;
				/* If new def has a default, override previous default */
				if (newdef->raw_default != NULL)
				{
					def->raw_default = newdef->raw_default;
					def->cooked_default = newdef->cooked_default;
				}
			}
			else
			{
				/*
				 * No, attach new column to result schema
				 */
				inhSchema = lappend(inhSchema, newdef);
			}
		}

		schema = inhSchema;
	}

	*supOids = parentOids;
	*supconstr = constraints;
	return schema;
}

/*
 * complementary static functions for MergeAttributes().
 *
 * Varattnos of pg_relcheck.rcbin must be rewritten when subclasses inherit
 * constraints from parent classes, since the inherited attributes could
 * be given different column numbers in multiple-inheritance cases.
 *
 * Note that the passed node tree is modified in place!
 */
static bool
change_varattnos_walker(Node *node, const AttrNumber *newattno)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varlevelsup == 0 && var->varno == 1 &&
			var->varattno > 0)
		{

			/*
			 * ??? the following may be a problem when the node is
			 * multiply referenced though stringToNode() doesn't create
			 * such a node currently.
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
 * StoreCatalogInheritance
 *		Updates the system catalogs with proper inheritance information.
 *
 * supers is an integer list of the OIDs of the new relation's direct
 * ancestors.  NB: it is destructively changed to include indirect ancestors.
 */
static void
StoreCatalogInheritance(Oid relationId, List *supers)
{
	Relation	relation;
	TupleDesc	desc;
	int16		seqNumber;
	List	   *entry;
	HeapTuple	tuple;

	/*
	 * sanity checks
	 */
	AssertArg(OidIsValid(relationId));

	if (supers == NIL)
		return;

	/*
	 * Catalog INHERITS information using direct ancestors only.
	 */
	relation = heap_openr(InheritsRelationName, RowExclusiveLock);
	desc = RelationGetDescr(relation);

	seqNumber = 1;
	foreach(entry, supers)
	{
		Oid			entryOid = lfirsti(entry);
		Datum		datum[Natts_pg_inherits];
		char		nullarr[Natts_pg_inherits];

		datum[0] = ObjectIdGetDatum(relationId);		/* inhrel */
		datum[1] = ObjectIdGetDatum(entryOid);	/* inhparent */
		datum[2] = Int16GetDatum(seqNumber);	/* inhseqno */

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
	 * Expand supers list to include indirect ancestors as well.
	 *
	 * Algorithm:
	 *	0. begin with list of direct superclasses.
	 *	1. append after each relationId, its superclasses, recursively.
	 *	2. remove all but last of duplicates.
	 * ----------------
	 */

	/*
	 * 1. append after each relationId, its superclasses, recursively.
	 */
	foreach(entry, supers)
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

	/*
	 * 2. remove all but last of duplicates.
	 */
	foreach(entry, supers)
	{
		Oid			thisone;
		bool		found;
		List	   *rest;

again:
		thisone = lfirsti(entry);
		found = false;
		foreach(rest, lnext(entry))
		{
			if (thisone == lfirsti(rest))
			{
				found = true;
				break;
			}
		}
		if (found)
		{

			/*
			 * found a later duplicate, so remove this entry.
			 */
			lfirsti(entry) = lfirsti(lnext(entry));
			lnext(entry) = lnext(lnext(entry));

			goto again;
		}
	}

	/*
	 * Catalog IPL information using expanded list.
	 */
	relation = heap_openr(InheritancePrecidenceListRelationName, RowExclusiveLock);
	desc = RelationGetDescr(relation);

	seqNumber = 1;

	foreach(entry, supers)
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
 * Look for an existing schema entry with the given name.
 *
 * Returns the index (starting with 1) if attribute already exists in schema,
 * 0 if it doesn't.
 */
static int
findAttrByName(const char *attributeName, List *schema)
{
	List	   *s;
	int			i = 0;

	foreach(s, schema)
	{
		ColumnDef  *def = lfirst(s);

		++i;
		if (strcmp(attributeName, def->colname) == 0)
			return i;
	}
	return 0;
}

/*
 * Update a relation's pg_class.relhassubclass entry to the given value
 */
static void
setRelhassubclassInRelation(Oid relationId, bool relhassubclass)
{
	Relation	relationRelation;
	HeapTuple	tuple;
	Relation	idescs[Num_pg_class_indices];

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
	simple_heap_update(relationRelation, &tuple->t_self, tuple);

	/* keep the catalog indices up to date */
	CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices, idescs);
	CatalogIndexInsert(idescs, Num_pg_class_indices, relationRelation, tuple);
	CatalogCloseIndices(Num_pg_class_indices, idescs);

	heap_freetuple(tuple);
	heap_close(relationRelation, RowExclusiveLock);
}
