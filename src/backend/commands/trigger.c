/*-------------------------------------------------------------------------
 *
 * trigger.c
 *	  PostgreSQL TRIGGERs support code.
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/trigger.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/sysattr.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/var.h"
#include "parser/parse_clause.h"
#include "parser/parse_collate.h"
#include "parser/parse_func.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "pgstat.h"
#include "rewrite/rewriteManip.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/bytea.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/tqual.h"
#include "utils/tuplestore.h"


/* GUC variables */
int			SessionReplicationRole = SESSION_REPLICATION_ROLE_ORIGIN;

/* How many levels deep into trigger execution are we? */
static int	MyTriggerDepth = 0;

#define GetModifiedColumns(relinfo, estate) \
	(rt_fetch((relinfo)->ri_RangeTableIndex, (estate)->es_range_table)->modifiedCols)

/* Local function prototypes */
static void ConvertTriggerToFK(CreateTrigStmt *stmt, Oid funcoid);
static void SetTriggerFlags(TriggerDesc *trigdesc, Trigger *trigger);
static HeapTuple GetTupleForTrigger(EState *estate,
				   EPQState *epqstate,
				   ResultRelInfo *relinfo,
				   ItemPointer tid,
				   LockTupleMode lockmode,
				   TupleTableSlot **newSlot);
static bool TriggerEnabled(EState *estate, ResultRelInfo *relinfo,
			   Trigger *trigger, TriggerEvent event,
			   Bitmapset *modifiedCols,
			   HeapTuple oldtup, HeapTuple newtup);
static HeapTuple ExecCallTriggerFunc(TriggerData *trigdata,
					int tgindx,
					FmgrInfo *finfo,
					Instrumentation *instr,
					MemoryContext per_tuple_context);
static void AfterTriggerSaveEvent(EState *estate, ResultRelInfo *relinfo,
					  int event, bool row_trigger,
					  HeapTuple oldtup, HeapTuple newtup,
					  List *recheckIndexes, Bitmapset *modifiedCols);
static void AfterTriggerEnlargeQueryState(void);


/*
 * Create a trigger.  Returns the OID of the created trigger.
 *
 * queryString is the source text of the CREATE TRIGGER command.
 * This must be supplied if a whenClause is specified, else it can be NULL.
 *
 * relOid, if nonzero, is the relation on which the trigger should be
 * created.  If zero, the name provided in the statement will be looked up.
 *
 * refRelOid, if nonzero, is the relation to which the constraint trigger
 * refers.  If zero, the constraint relation name provided in the statement
 * will be looked up as needed.
 *
 * constraintOid, if nonzero, says that this trigger is being created
 * internally to implement that constraint.  A suitable pg_depend entry will
 * be made to link the trigger to that constraint.  constraintOid is zero when
 * executing a user-entered CREATE TRIGGER command.  (For CREATE CONSTRAINT
 * TRIGGER, we build a pg_constraint entry internally.)
 *
 * indexOid, if nonzero, is the OID of an index associated with the constraint.
 * We do nothing with this except store it into pg_trigger.tgconstrindid.
 *
 * If isInternal is true then this is an internally-generated trigger.
 * This argument sets the tgisinternal field of the pg_trigger entry, and
 * if TRUE causes us to modify the given trigger name to ensure uniqueness.
 *
 * When isInternal is not true we require ACL_TRIGGER permissions on the
 * relation, as well as ACL_EXECUTE on the trigger function.  For internal
 * triggers the caller must apply any required permission checks.
 *
 * Note: can return InvalidOid if we decided to not create a trigger at all,
 * but a foreign-key constraint.  This is a kluge for backwards compatibility.
 */
Oid
CreateTrigger(CreateTrigStmt *stmt, const char *queryString,
			  Oid relOid, Oid refRelOid, Oid constraintOid, Oid indexOid,
			  bool isInternal)
{
	int16		tgtype;
	int			ncolumns;
	int16	   *columns;
	int2vector *tgattr;
	Node	   *whenClause;
	List	   *whenRtable;
	char	   *qual;
	Datum		values[Natts_pg_trigger];
	bool		nulls[Natts_pg_trigger];
	Relation	rel;
	AclResult	aclresult;
	Relation	tgrel;
	SysScanDesc tgscan;
	ScanKeyData key;
	Relation	pgrel;
	HeapTuple	tuple;
	Oid			fargtypes[1];	/* dummy */
	Oid			funcoid;
	Oid			funcrettype;
	Oid			trigoid;
	char		internaltrigname[NAMEDATALEN];
	char	   *trigname;
	Oid			constrrelid = InvalidOid;
	ObjectAddress myself,
				referenced;

	if (OidIsValid(relOid))
		rel = heap_open(relOid, AccessExclusiveLock);
	else
		rel = heap_openrv(stmt->relation, AccessExclusiveLock);

	/*
	 * Triggers must be on tables or views, and there are additional
	 * relation-type-specific restrictions.
	 */
	if (rel->rd_rel->relkind == RELKIND_RELATION)
	{
		/* Tables can't have INSTEAD OF triggers */
		if (stmt->timing != TRIGGER_TYPE_BEFORE &&
			stmt->timing != TRIGGER_TYPE_AFTER)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a table",
							RelationGetRelationName(rel)),
					 errdetail("Tables cannot have INSTEAD OF triggers.")));
	}
	else if (rel->rd_rel->relkind == RELKIND_VIEW)
	{
		/*
		 * Views can have INSTEAD OF triggers (which we check below are
		 * row-level), or statement-level BEFORE/AFTER triggers.
		 */
		if (stmt->timing != TRIGGER_TYPE_INSTEAD && stmt->row)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a view",
							RelationGetRelationName(rel)),
					 errdetail("Views cannot have row-level BEFORE or AFTER triggers.")));
		/* Disallow TRUNCATE triggers on VIEWs */
		if (TRIGGER_FOR_TRUNCATE(stmt->events))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a view",
							RelationGetRelationName(rel)),
					 errdetail("Views cannot have TRUNCATE triggers.")));
	}
	else if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
	{
		if (stmt->timing != TRIGGER_TYPE_BEFORE &&
			stmt->timing != TRIGGER_TYPE_AFTER)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a foreign table",
							RelationGetRelationName(rel)),
			  errdetail("Foreign tables cannot have INSTEAD OF triggers.")));

		if (TRIGGER_FOR_TRUNCATE(stmt->events))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a foreign table",
							RelationGetRelationName(rel)),
				errdetail("Foreign tables cannot have TRUNCATE triggers.")));

		if (stmt->isconstraint)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a foreign table",
							RelationGetRelationName(rel)),
			  errdetail("Foreign tables cannot have constraint triggers.")));
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table or view",
						RelationGetRelationName(rel))));

	if (!allowSystemTableMods && IsSystemRelation(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(rel))));

	if (stmt->isconstraint)
	{
		/*
		 * We must take a lock on the target relation to protect against
		 * concurrent drop.  It's not clear that AccessShareLock is strong
		 * enough, but we certainly need at least that much... otherwise, we
		 * might end up creating a pg_constraint entry referencing a
		 * nonexistent table.
		 */
		if (OidIsValid(refRelOid))
		{
			LockRelationOid(refRelOid, AccessShareLock);
			constrrelid = refRelOid;
		}
		else if (stmt->constrrel != NULL)
			constrrelid = RangeVarGetRelid(stmt->constrrel, AccessShareLock,
										   false);
	}

	/* permission checks */
	if (!isInternal)
	{
		aclresult = pg_class_aclcheck(RelationGetRelid(rel), GetUserId(),
									  ACL_TRIGGER);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_CLASS,
						   RelationGetRelationName(rel));

		if (OidIsValid(constrrelid))
		{
			aclresult = pg_class_aclcheck(constrrelid, GetUserId(),
										  ACL_TRIGGER);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, ACL_KIND_CLASS,
							   get_rel_name(constrrelid));
		}
	}

	/* Compute tgtype */
	TRIGGER_CLEAR_TYPE(tgtype);
	if (stmt->row)
		TRIGGER_SETT_ROW(tgtype);
	tgtype |= stmt->timing;
	tgtype |= stmt->events;

	/* Disallow ROW-level TRUNCATE triggers */
	if (TRIGGER_FOR_ROW(tgtype) && TRIGGER_FOR_TRUNCATE(tgtype))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("TRUNCATE FOR EACH ROW triggers are not supported")));

	/* INSTEAD triggers must be row-level, and can't have WHEN or columns */
	if (TRIGGER_FOR_INSTEAD(tgtype))
	{
		if (!TRIGGER_FOR_ROW(tgtype))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("INSTEAD OF triggers must be FOR EACH ROW")));
		if (stmt->whenClause)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("INSTEAD OF triggers cannot have WHEN conditions")));
		if (stmt->columns != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("INSTEAD OF triggers cannot have column lists")));
	}

	/*
	 * Parse the WHEN clause, if any
	 */
	if (stmt->whenClause)
	{
		ParseState *pstate;
		RangeTblEntry *rte;
		List	   *varList;
		ListCell   *lc;

		/* Set up a pstate to parse with */
		pstate = make_parsestate(NULL);
		pstate->p_sourcetext = queryString;

		/*
		 * Set up RTEs for OLD and NEW references.
		 *
		 * 'OLD' must always have varno equal to 1 and 'NEW' equal to 2.
		 */
		rte = addRangeTableEntryForRelation(pstate, rel,
											makeAlias("old", NIL),
											false, false);
		addRTEtoQuery(pstate, rte, false, true, true);
		rte = addRangeTableEntryForRelation(pstate, rel,
											makeAlias("new", NIL),
											false, false);
		addRTEtoQuery(pstate, rte, false, true, true);

		/* Transform expression.  Copy to be sure we don't modify original */
		whenClause = transformWhereClause(pstate,
										  copyObject(stmt->whenClause),
										  EXPR_KIND_TRIGGER_WHEN,
										  "WHEN");
		/* we have to fix its collations too */
		assign_expr_collations(pstate, whenClause);

		/*
		 * Check for disallowed references to OLD/NEW.
		 *
		 * NB: pull_var_clause is okay here only because we don't allow
		 * subselects in WHEN clauses; it would fail to examine the contents
		 * of subselects.
		 */
		varList = pull_var_clause(whenClause,
								  PVC_REJECT_AGGREGATES,
								  PVC_REJECT_PLACEHOLDERS);
		foreach(lc, varList)
		{
			Var		   *var = (Var *) lfirst(lc);

			switch (var->varno)
			{
				case PRS2_OLD_VARNO:
					if (!TRIGGER_FOR_ROW(tgtype))
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("statement trigger's WHEN condition cannot reference column values"),
								 parser_errposition(pstate, var->location)));
					if (TRIGGER_FOR_INSERT(tgtype))
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("INSERT trigger's WHEN condition cannot reference OLD values"),
								 parser_errposition(pstate, var->location)));
					/* system columns are okay here */
					break;
				case PRS2_NEW_VARNO:
					if (!TRIGGER_FOR_ROW(tgtype))
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("statement trigger's WHEN condition cannot reference column values"),
								 parser_errposition(pstate, var->location)));
					if (TRIGGER_FOR_DELETE(tgtype))
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("DELETE trigger's WHEN condition cannot reference NEW values"),
								 parser_errposition(pstate, var->location)));
					if (var->varattno < 0 && TRIGGER_FOR_BEFORE(tgtype))
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("BEFORE trigger's WHEN condition cannot reference NEW system columns"),
								 parser_errposition(pstate, var->location)));
					break;
				default:
					/* can't happen without add_missing_from, so just elog */
					elog(ERROR, "trigger WHEN condition cannot contain references to other relations");
					break;
			}
		}

		/* we'll need the rtable for recordDependencyOnExpr */
		whenRtable = pstate->p_rtable;

		qual = nodeToString(whenClause);

		free_parsestate(pstate);
	}
	else
	{
		whenClause = NULL;
		whenRtable = NIL;
		qual = NULL;
	}

	/*
	 * Find and validate the trigger function.
	 */
	funcoid = LookupFuncName(stmt->funcname, 0, fargtypes, false);
	if (!isInternal)
	{
		aclresult = pg_proc_aclcheck(funcoid, GetUserId(), ACL_EXECUTE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_PROC,
						   NameListToString(stmt->funcname));
	}
	funcrettype = get_func_rettype(funcoid);
	if (funcrettype != TRIGGEROID)
	{
		/*
		 * We allow OPAQUE just so we can load old dump files.  When we see a
		 * trigger function declared OPAQUE, change it to TRIGGER.
		 */
		if (funcrettype == OPAQUEOID)
		{
			ereport(WARNING,
					(errmsg("changing return type of function %s from \"opaque\" to \"trigger\"",
							NameListToString(stmt->funcname))));
			SetFunctionReturnType(funcoid, TRIGGEROID);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("function %s must return type \"trigger\"",
							NameListToString(stmt->funcname))));
	}

	/*
	 * If the command is a user-entered CREATE CONSTRAINT TRIGGER command that
	 * references one of the built-in RI_FKey trigger functions, assume it is
	 * from a dump of a pre-7.3 foreign key constraint, and take steps to
	 * convert this legacy representation into a regular foreign key
	 * constraint.  Ugly, but necessary for loading old dump files.
	 */
	if (stmt->isconstraint && !isInternal &&
		list_length(stmt->args) >= 6 &&
		(list_length(stmt->args) % 2) == 0 &&
		RI_FKey_trigger_type(funcoid) != RI_TRIGGER_NONE)
	{
		/* Keep lock on target rel until end of xact */
		heap_close(rel, NoLock);

		ConvertTriggerToFK(stmt, funcoid);

		return InvalidOid;
	}

	/*
	 * If it's a user-entered CREATE CONSTRAINT TRIGGER command, make a
	 * corresponding pg_constraint entry.
	 */
	if (stmt->isconstraint && !OidIsValid(constraintOid))
	{
		/* Internal callers should have made their own constraints */
		Assert(!isInternal);
		constraintOid = CreateConstraintEntry(stmt->trigname,
											  RelationGetNamespace(rel),
											  CONSTRAINT_TRIGGER,
											  stmt->deferrable,
											  stmt->initdeferred,
											  true,
											  RelationGetRelid(rel),
											  NULL,		/* no conkey */
											  0,
											  InvalidOid,		/* no domain */
											  InvalidOid,		/* no index */
											  InvalidOid,		/* no foreign key */
											  NULL,
											  NULL,
											  NULL,
											  NULL,
											  0,
											  ' ',
											  ' ',
											  ' ',
											  NULL,		/* no exclusion */
											  NULL,		/* no check constraint */
											  NULL,
											  NULL,
											  true,		/* islocal */
											  0,		/* inhcount */
											  true,		/* isnoinherit */
											  isInternal);		/* is_internal */
	}

	/*
	 * Generate the trigger's OID now, so that we can use it in the name if
	 * needed.
	 */
	tgrel = heap_open(TriggerRelationId, RowExclusiveLock);

	trigoid = GetNewOid(tgrel);

	/*
	 * If trigger is internally generated, modify the provided trigger name to
	 * ensure uniqueness by appending the trigger OID.  (Callers will usually
	 * supply a simple constant trigger name in these cases.)
	 */
	if (isInternal)
	{
		snprintf(internaltrigname, sizeof(internaltrigname),
				 "%s_%u", stmt->trigname, trigoid);
		trigname = internaltrigname;
	}
	else
	{
		/* user-defined trigger; use the specified trigger name as-is */
		trigname = stmt->trigname;
	}

	/*
	 * Scan pg_trigger for existing triggers on relation.  We do this only to
	 * give a nice error message if there's already a trigger of the same
	 * name.  (The unique index on tgrelid/tgname would complain anyway.) We
	 * can skip this for internally generated triggers, since the name
	 * modification above should be sufficient.
	 *
	 * NOTE that this is cool only because we have AccessExclusiveLock on the
	 * relation, so the trigger set won't be changing underneath us.
	 */
	if (!isInternal)
	{
		ScanKeyInit(&key,
					Anum_pg_trigger_tgrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(RelationGetRelid(rel)));
		tgscan = systable_beginscan(tgrel, TriggerRelidNameIndexId, true,
									NULL, 1, &key);
		while (HeapTupleIsValid(tuple = systable_getnext(tgscan)))
		{
			Form_pg_trigger pg_trigger = (Form_pg_trigger) GETSTRUCT(tuple);

			if (namestrcmp(&(pg_trigger->tgname), trigname) == 0)
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
				  errmsg("trigger \"%s\" for relation \"%s\" already exists",
						 trigname, RelationGetRelationName(rel))));
		}
		systable_endscan(tgscan);
	}

	/*
	 * Build the new pg_trigger tuple.
	 */
	memset(nulls, false, sizeof(nulls));

	values[Anum_pg_trigger_tgrelid - 1] = ObjectIdGetDatum(RelationGetRelid(rel));
	values[Anum_pg_trigger_tgname - 1] = DirectFunctionCall1(namein,
												  CStringGetDatum(trigname));
	values[Anum_pg_trigger_tgfoid - 1] = ObjectIdGetDatum(funcoid);
	values[Anum_pg_trigger_tgtype - 1] = Int16GetDatum(tgtype);
	values[Anum_pg_trigger_tgenabled - 1] = CharGetDatum(TRIGGER_FIRES_ON_ORIGIN);
	values[Anum_pg_trigger_tgisinternal - 1] = BoolGetDatum(isInternal);
	values[Anum_pg_trigger_tgconstrrelid - 1] = ObjectIdGetDatum(constrrelid);
	values[Anum_pg_trigger_tgconstrindid - 1] = ObjectIdGetDatum(indexOid);
	values[Anum_pg_trigger_tgconstraint - 1] = ObjectIdGetDatum(constraintOid);
	values[Anum_pg_trigger_tgdeferrable - 1] = BoolGetDatum(stmt->deferrable);
	values[Anum_pg_trigger_tginitdeferred - 1] = BoolGetDatum(stmt->initdeferred);

	if (stmt->args)
	{
		ListCell   *le;
		char	   *args;
		int16		nargs = list_length(stmt->args);
		int			len = 0;

		foreach(le, stmt->args)
		{
			char	   *ar = strVal(lfirst(le));

			len += strlen(ar) + 4;
			for (; *ar; ar++)
			{
				if (*ar == '\\')
					len++;
			}
		}
		args = (char *) palloc(len + 1);
		args[0] = '\0';
		foreach(le, stmt->args)
		{
			char	   *s = strVal(lfirst(le));
			char	   *d = args + strlen(args);

			while (*s)
			{
				if (*s == '\\')
					*d++ = '\\';
				*d++ = *s++;
			}
			strcpy(d, "\\000");
		}
		values[Anum_pg_trigger_tgnargs - 1] = Int16GetDatum(nargs);
		values[Anum_pg_trigger_tgargs - 1] = DirectFunctionCall1(byteain,
													  CStringGetDatum(args));
	}
	else
	{
		values[Anum_pg_trigger_tgnargs - 1] = Int16GetDatum(0);
		values[Anum_pg_trigger_tgargs - 1] = DirectFunctionCall1(byteain,
														CStringGetDatum(""));
	}

	/* build column number array if it's a column-specific trigger */
	ncolumns = list_length(stmt->columns);
	if (ncolumns == 0)
		columns = NULL;
	else
	{
		ListCell   *cell;
		int			i = 0;

		columns = (int16 *) palloc(ncolumns * sizeof(int16));
		foreach(cell, stmt->columns)
		{
			char	   *name = strVal(lfirst(cell));
			int16		attnum;
			int			j;

			/* Lookup column name.  System columns are not allowed */
			attnum = attnameAttNum(rel, name, false);
			if (attnum == InvalidAttrNumber)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
					errmsg("column \"%s\" of relation \"%s\" does not exist",
						   name, RelationGetRelationName(rel))));

			/* Check for duplicates */
			for (j = i - 1; j >= 0; j--)
			{
				if (columns[j] == attnum)
					ereport(ERROR,
							(errcode(ERRCODE_DUPLICATE_COLUMN),
							 errmsg("column \"%s\" specified more than once",
									name)));
			}

			columns[i++] = attnum;
		}
	}
	tgattr = buildint2vector(columns, ncolumns);
	values[Anum_pg_trigger_tgattr - 1] = PointerGetDatum(tgattr);

	/* set tgqual if trigger has WHEN clause */
	if (qual)
		values[Anum_pg_trigger_tgqual - 1] = CStringGetTextDatum(qual);
	else
		nulls[Anum_pg_trigger_tgqual - 1] = true;

	tuple = heap_form_tuple(tgrel->rd_att, values, nulls);

	/* force tuple to have the desired OID */
	HeapTupleSetOid(tuple, trigoid);

	/*
	 * Insert tuple into pg_trigger.
	 */
	simple_heap_insert(tgrel, tuple);

	CatalogUpdateIndexes(tgrel, tuple);

	heap_freetuple(tuple);
	heap_close(tgrel, RowExclusiveLock);

	pfree(DatumGetPointer(values[Anum_pg_trigger_tgname - 1]));
	pfree(DatumGetPointer(values[Anum_pg_trigger_tgargs - 1]));
	pfree(DatumGetPointer(values[Anum_pg_trigger_tgattr - 1]));

	/*
	 * Update relation's pg_class entry.  Crucial side-effect: other backends
	 * (and this one too!) are sent SI message to make them rebuild relcache
	 * entries.
	 */
	pgrel = heap_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy1(RELOID,
								ObjectIdGetDatum(RelationGetRelid(rel)));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u",
			 RelationGetRelid(rel));

	((Form_pg_class) GETSTRUCT(tuple))->relhastriggers = true;

	simple_heap_update(pgrel, &tuple->t_self, tuple);

	CatalogUpdateIndexes(pgrel, tuple);

	heap_freetuple(tuple);
	heap_close(pgrel, RowExclusiveLock);

	/*
	 * We used to try to update the rel's relcache entry here, but that's
	 * fairly pointless since it will happen as a byproduct of the upcoming
	 * CommandCounterIncrement...
	 */

	/*
	 * Record dependencies for trigger.  Always place a normal dependency on
	 * the function.
	 */
	myself.classId = TriggerRelationId;
	myself.objectId = trigoid;
	myself.objectSubId = 0;

	referenced.classId = ProcedureRelationId;
	referenced.objectId = funcoid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	if (isInternal && OidIsValid(constraintOid))
	{
		/*
		 * Internally-generated trigger for a constraint, so make it an
		 * internal dependency of the constraint.  We can skip depending on
		 * the relation(s), as there'll be an indirect dependency via the
		 * constraint.
		 */
		referenced.classId = ConstraintRelationId;
		referenced.objectId = constraintOid;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_INTERNAL);
	}
	else
	{
		/*
		 * User CREATE TRIGGER, so place dependencies.  We make trigger be
		 * auto-dropped if its relation is dropped or if the FK relation is
		 * dropped.  (Auto drop is compatible with our pre-7.3 behavior.)
		 */
		referenced.classId = RelationRelationId;
		referenced.objectId = RelationGetRelid(rel);
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);
		if (OidIsValid(constrrelid))
		{
			referenced.classId = RelationRelationId;
			referenced.objectId = constrrelid;
			referenced.objectSubId = 0;
			recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);
		}
		/* Not possible to have an index dependency in this case */
		Assert(!OidIsValid(indexOid));

		/*
		 * If it's a user-specified constraint trigger, make the constraint
		 * internally dependent on the trigger instead of vice versa.
		 */
		if (OidIsValid(constraintOid))
		{
			referenced.classId = ConstraintRelationId;
			referenced.objectId = constraintOid;
			referenced.objectSubId = 0;
			recordDependencyOn(&referenced, &myself, DEPENDENCY_INTERNAL);
		}
	}

	/* If column-specific trigger, add normal dependencies on columns */
	if (columns != NULL)
	{
		int			i;

		referenced.classId = RelationRelationId;
		referenced.objectId = RelationGetRelid(rel);
		for (i = 0; i < ncolumns; i++)
		{
			referenced.objectSubId = columns[i];
			recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
		}
	}

	/*
	 * If it has a WHEN clause, add dependencies on objects mentioned in the
	 * expression (eg, functions, as well as any columns used).
	 */
	if (whenClause != NULL)
		recordDependencyOnExpr(&myself, whenClause, whenRtable,
							   DEPENDENCY_NORMAL);

	/* Post creation hook for new trigger */
	InvokeObjectPostCreateHookArg(TriggerRelationId, trigoid, 0,
								  isInternal);

	/* Keep lock on target rel until end of xact */
	heap_close(rel, NoLock);

	return trigoid;
}


/*
 * Convert legacy (pre-7.3) CREATE CONSTRAINT TRIGGER commands into
 * full-fledged foreign key constraints.
 *
 * The conversion is complex because a pre-7.3 foreign key involved three
 * separate triggers, which were reported separately in dumps.  While the
 * single trigger on the referencing table adds no new information, we need
 * to know the trigger functions of both of the triggers on the referenced
 * table to build the constraint declaration.  Also, due to lack of proper
 * dependency checking pre-7.3, it is possible that the source database had
 * an incomplete set of triggers resulting in an only partially enforced
 * FK constraint.  (This would happen if one of the tables had been dropped
 * and re-created, but only if the DB had been affected by a 7.0 pg_dump bug
 * that caused loss of tgconstrrelid information.)	We choose to translate to
 * an FK constraint only when we've seen all three triggers of a set.  This is
 * implemented by storing unmatched items in a list in TopMemoryContext.
 * We match triggers together by comparing the trigger arguments (which
 * include constraint name, table and column names, so should be good enough).
 */
typedef struct
{
	List	   *args;			/* list of (T_String) Values or NIL */
	Oid			funcoids[3];	/* OIDs of trigger functions */
	/* The three function OIDs are stored in the order update, delete, child */
} OldTriggerInfo;

static void
ConvertTriggerToFK(CreateTrigStmt *stmt, Oid funcoid)
{
	static List *info_list = NIL;

	static const char *const funcdescr[3] = {
		gettext_noop("Found referenced table's UPDATE trigger."),
		gettext_noop("Found referenced table's DELETE trigger."),
		gettext_noop("Found referencing table's trigger.")
	};

	char	   *constr_name;
	char	   *fk_table_name;
	char	   *pk_table_name;
	char		fk_matchtype = FKCONSTR_MATCH_SIMPLE;
	List	   *fk_attrs = NIL;
	List	   *pk_attrs = NIL;
	StringInfoData buf;
	int			funcnum;
	OldTriggerInfo *info = NULL;
	ListCell   *l;
	int			i;

	/* Parse out the trigger arguments */
	constr_name = strVal(linitial(stmt->args));
	fk_table_name = strVal(lsecond(stmt->args));
	pk_table_name = strVal(lthird(stmt->args));
	i = 0;
	foreach(l, stmt->args)
	{
		Value	   *arg = (Value *) lfirst(l);

		i++;
		if (i < 4)				/* skip constraint and table names */
			continue;
		if (i == 4)				/* handle match type */
		{
			if (strcmp(strVal(arg), "FULL") == 0)
				fk_matchtype = FKCONSTR_MATCH_FULL;
			else
				fk_matchtype = FKCONSTR_MATCH_SIMPLE;
			continue;
		}
		if (i % 2)
			fk_attrs = lappend(fk_attrs, arg);
		else
			pk_attrs = lappend(pk_attrs, arg);
	}

	/* Prepare description of constraint for use in messages */
	initStringInfo(&buf);
	appendStringInfo(&buf, "FOREIGN KEY %s(",
					 quote_identifier(fk_table_name));
	i = 0;
	foreach(l, fk_attrs)
	{
		Value	   *arg = (Value *) lfirst(l);

		if (i++ > 0)
			appendStringInfoChar(&buf, ',');
		appendStringInfoString(&buf, quote_identifier(strVal(arg)));
	}
	appendStringInfo(&buf, ") REFERENCES %s(",
					 quote_identifier(pk_table_name));
	i = 0;
	foreach(l, pk_attrs)
	{
		Value	   *arg = (Value *) lfirst(l);

		if (i++ > 0)
			appendStringInfoChar(&buf, ',');
		appendStringInfoString(&buf, quote_identifier(strVal(arg)));
	}
	appendStringInfoChar(&buf, ')');

	/* Identify class of trigger --- update, delete, or referencing-table */
	switch (funcoid)
	{
		case F_RI_FKEY_CASCADE_UPD:
		case F_RI_FKEY_RESTRICT_UPD:
		case F_RI_FKEY_SETNULL_UPD:
		case F_RI_FKEY_SETDEFAULT_UPD:
		case F_RI_FKEY_NOACTION_UPD:
			funcnum = 0;
			break;

		case F_RI_FKEY_CASCADE_DEL:
		case F_RI_FKEY_RESTRICT_DEL:
		case F_RI_FKEY_SETNULL_DEL:
		case F_RI_FKEY_SETDEFAULT_DEL:
		case F_RI_FKEY_NOACTION_DEL:
			funcnum = 1;
			break;

		default:
			funcnum = 2;
			break;
	}

	/* See if we have a match to this trigger */
	foreach(l, info_list)
	{
		info = (OldTriggerInfo *) lfirst(l);
		if (info->funcoids[funcnum] == InvalidOid &&
			equal(info->args, stmt->args))
		{
			info->funcoids[funcnum] = funcoid;
			break;
		}
	}

	if (l == NULL)
	{
		/* First trigger of set, so create a new list entry */
		MemoryContext oldContext;

		ereport(NOTICE,
		(errmsg("ignoring incomplete trigger group for constraint \"%s\" %s",
				constr_name, buf.data),
		 errdetail_internal("%s", _(funcdescr[funcnum]))));
		oldContext = MemoryContextSwitchTo(TopMemoryContext);
		info = (OldTriggerInfo *) palloc0(sizeof(OldTriggerInfo));
		info->args = copyObject(stmt->args);
		info->funcoids[funcnum] = funcoid;
		info_list = lappend(info_list, info);
		MemoryContextSwitchTo(oldContext);
	}
	else if (info->funcoids[0] == InvalidOid ||
			 info->funcoids[1] == InvalidOid ||
			 info->funcoids[2] == InvalidOid)
	{
		/* Second trigger of set */
		ereport(NOTICE,
		(errmsg("ignoring incomplete trigger group for constraint \"%s\" %s",
				constr_name, buf.data),
		 errdetail_internal("%s", _(funcdescr[funcnum]))));
	}
	else
	{
		/* OK, we have a set, so make the FK constraint ALTER TABLE cmd */
		AlterTableStmt *atstmt = makeNode(AlterTableStmt);
		AlterTableCmd *atcmd = makeNode(AlterTableCmd);
		Constraint *fkcon = makeNode(Constraint);

		ereport(NOTICE,
				(errmsg("converting trigger group into constraint \"%s\" %s",
						constr_name, buf.data),
				 errdetail_internal("%s", _(funcdescr[funcnum]))));
		fkcon->contype = CONSTR_FOREIGN;
		fkcon->location = -1;
		if (funcnum == 2)
		{
			/* This trigger is on the FK table */
			atstmt->relation = stmt->relation;
			if (stmt->constrrel)
				fkcon->pktable = stmt->constrrel;
			else
			{
				/* Work around ancient pg_dump bug that omitted constrrel */
				fkcon->pktable = makeRangeVar(NULL, pk_table_name, -1);
			}
		}
		else
		{
			/* This trigger is on the PK table */
			fkcon->pktable = stmt->relation;
			if (stmt->constrrel)
				atstmt->relation = stmt->constrrel;
			else
			{
				/* Work around ancient pg_dump bug that omitted constrrel */
				atstmt->relation = makeRangeVar(NULL, fk_table_name, -1);
			}
		}
		atstmt->cmds = list_make1(atcmd);
		atstmt->relkind = OBJECT_TABLE;
		atcmd->subtype = AT_AddConstraint;
		atcmd->def = (Node *) fkcon;
		if (strcmp(constr_name, "<unnamed>") == 0)
			fkcon->conname = NULL;
		else
			fkcon->conname = constr_name;
		fkcon->fk_attrs = fk_attrs;
		fkcon->pk_attrs = pk_attrs;
		fkcon->fk_matchtype = fk_matchtype;
		switch (info->funcoids[0])
		{
			case F_RI_FKEY_NOACTION_UPD:
				fkcon->fk_upd_action = FKCONSTR_ACTION_NOACTION;
				break;
			case F_RI_FKEY_CASCADE_UPD:
				fkcon->fk_upd_action = FKCONSTR_ACTION_CASCADE;
				break;
			case F_RI_FKEY_RESTRICT_UPD:
				fkcon->fk_upd_action = FKCONSTR_ACTION_RESTRICT;
				break;
			case F_RI_FKEY_SETNULL_UPD:
				fkcon->fk_upd_action = FKCONSTR_ACTION_SETNULL;
				break;
			case F_RI_FKEY_SETDEFAULT_UPD:
				fkcon->fk_upd_action = FKCONSTR_ACTION_SETDEFAULT;
				break;
			default:
				/* can't get here because of earlier checks */
				elog(ERROR, "confused about RI update function");
		}
		switch (info->funcoids[1])
		{
			case F_RI_FKEY_NOACTION_DEL:
				fkcon->fk_del_action = FKCONSTR_ACTION_NOACTION;
				break;
			case F_RI_FKEY_CASCADE_DEL:
				fkcon->fk_del_action = FKCONSTR_ACTION_CASCADE;
				break;
			case F_RI_FKEY_RESTRICT_DEL:
				fkcon->fk_del_action = FKCONSTR_ACTION_RESTRICT;
				break;
			case F_RI_FKEY_SETNULL_DEL:
				fkcon->fk_del_action = FKCONSTR_ACTION_SETNULL;
				break;
			case F_RI_FKEY_SETDEFAULT_DEL:
				fkcon->fk_del_action = FKCONSTR_ACTION_SETDEFAULT;
				break;
			default:
				/* can't get here because of earlier checks */
				elog(ERROR, "confused about RI delete function");
		}
		fkcon->deferrable = stmt->deferrable;
		fkcon->initdeferred = stmt->initdeferred;
		fkcon->skip_validation = false;
		fkcon->initially_valid = true;

		/* ... and execute it */
		ProcessUtility((Node *) atstmt,
					   "(generated ALTER TABLE ADD FOREIGN KEY command)",
					   PROCESS_UTILITY_SUBCOMMAND, NULL,
					   None_Receiver, NULL);

		/* Remove the matched item from the list */
		info_list = list_delete_ptr(info_list, info);
		pfree(info);
		/* We leak the copied args ... not worth worrying about */
	}
}

/*
 * Guts of trigger deletion.
 */
void
RemoveTriggerById(Oid trigOid)
{
	Relation	tgrel;
	SysScanDesc tgscan;
	ScanKeyData skey[1];
	HeapTuple	tup;
	Oid			relid;
	Relation	rel;

	tgrel = heap_open(TriggerRelationId, RowExclusiveLock);

	/*
	 * Find the trigger to delete.
	 */
	ScanKeyInit(&skey[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(trigOid));

	tgscan = systable_beginscan(tgrel, TriggerOidIndexId, true,
								NULL, 1, skey);

	tup = systable_getnext(tgscan);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "could not find tuple for trigger %u", trigOid);

	/*
	 * Open and exclusive-lock the relation the trigger belongs to.
	 */
	relid = ((Form_pg_trigger) GETSTRUCT(tup))->tgrelid;

	rel = heap_open(relid, AccessExclusiveLock);

	if (rel->rd_rel->relkind != RELKIND_RELATION &&
		rel->rd_rel->relkind != RELKIND_VIEW &&
		rel->rd_rel->relkind != RELKIND_FOREIGN_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table, view, or foreign table",
						RelationGetRelationName(rel))));

	if (!allowSystemTableMods && IsSystemRelation(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(rel))));

	/*
	 * Delete the pg_trigger tuple.
	 */
	simple_heap_delete(tgrel, &tup->t_self);

	systable_endscan(tgscan);
	heap_close(tgrel, RowExclusiveLock);

	/*
	 * We do not bother to try to determine whether any other triggers remain,
	 * which would be needed in order to decide whether it's safe to clear the
	 * relation's relhastriggers.  (In any case, there might be a concurrent
	 * process adding new triggers.)  Instead, just force a relcache inval to
	 * make other backends (and this one too!) rebuild their relcache entries.
	 * There's no great harm in leaving relhastriggers true even if there are
	 * no triggers left.
	 */
	CacheInvalidateRelcache(rel);

	/* Keep lock on trigger's rel until end of xact */
	heap_close(rel, NoLock);
}

/*
 * get_trigger_oid - Look up a trigger by name to find its OID.
 *
 * If missing_ok is false, throw an error if trigger not found.  If
 * true, just return InvalidOid.
 */
Oid
get_trigger_oid(Oid relid, const char *trigname, bool missing_ok)
{
	Relation	tgrel;
	ScanKeyData skey[2];
	SysScanDesc tgscan;
	HeapTuple	tup;
	Oid			oid;

	/*
	 * Find the trigger, verify permissions, set up object address
	 */
	tgrel = heap_open(TriggerRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_trigger_tgrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&skey[1],
				Anum_pg_trigger_tgname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(trigname));

	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndexId, true,
								NULL, 2, skey);

	tup = systable_getnext(tgscan);

	if (!HeapTupleIsValid(tup))
	{
		if (!missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("trigger \"%s\" for table \"%s\" does not exist",
							trigname, get_rel_name(relid))));
		oid = InvalidOid;
	}
	else
	{
		oid = HeapTupleGetOid(tup);
	}

	systable_endscan(tgscan);
	heap_close(tgrel, AccessShareLock);
	return oid;
}

/*
 * Perform permissions and integrity checks before acquiring a relation lock.
 */
static void
RangeVarCallbackForRenameTrigger(const RangeVar *rv, Oid relid, Oid oldrelid,
								 void *arg)
{
	HeapTuple	tuple;
	Form_pg_class form;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		return;					/* concurrently dropped */
	form = (Form_pg_class) GETSTRUCT(tuple);

	/* only tables and views can have triggers */
	if (form->relkind != RELKIND_RELATION && form->relkind != RELKIND_VIEW &&
		form->relkind != RELKIND_FOREIGN_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table, view, or foreign table",
						rv->relname)));

	/* you must own the table to rename one of its triggers */
	if (!pg_class_ownercheck(relid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS, rv->relname);
	if (!allowSystemTableMods && IsSystemClass(relid, form))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						rv->relname)));

	ReleaseSysCache(tuple);
}

/*
 *		renametrig		- changes the name of a trigger on a relation
 *
 *		trigger name is changed in trigger catalog.
 *		No record of the previous name is kept.
 *
 *		get proper relrelation from relation catalog (if not arg)
 *		scan trigger catalog
 *				for name conflict (within rel)
 *				for original trigger (if not arg)
 *		modify tgname in trigger tuple
 *		update row in catalog
 */
Oid
renametrig(RenameStmt *stmt)
{
	Oid			tgoid;
	Relation	targetrel;
	Relation	tgrel;
	HeapTuple	tuple;
	SysScanDesc tgscan;
	ScanKeyData key[2];
	Oid			relid;

	/*
	 * Look up name, check permissions, and acquire lock (which we will NOT
	 * release until end of transaction).
	 */
	relid = RangeVarGetRelidExtended(stmt->relation, AccessExclusiveLock,
									 false, false,
									 RangeVarCallbackForRenameTrigger,
									 NULL);

	/* Have lock already, so just need to build relcache entry. */
	targetrel = relation_open(relid, NoLock);

	/*
	 * Scan pg_trigger twice for existing triggers on relation.  We do this in
	 * order to ensure a trigger does not exist with newname (The unique index
	 * on tgrelid/tgname would complain anyway) and to ensure a trigger does
	 * exist with oldname.
	 *
	 * NOTE that this is cool only because we have AccessExclusiveLock on the
	 * relation, so the trigger set won't be changing underneath us.
	 */
	tgrel = heap_open(TriggerRelationId, RowExclusiveLock);

	/*
	 * First pass -- look for name conflict
	 */
	ScanKeyInit(&key[0],
				Anum_pg_trigger_tgrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&key[1],
				Anum_pg_trigger_tgname,
				BTEqualStrategyNumber, F_NAMEEQ,
				PointerGetDatum(stmt->newname));
	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndexId, true,
								NULL, 2, key);
	if (HeapTupleIsValid(tuple = systable_getnext(tgscan)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("trigger \"%s\" for relation \"%s\" already exists",
						stmt->newname, RelationGetRelationName(targetrel))));
	systable_endscan(tgscan);

	/*
	 * Second pass -- look for trigger existing with oldname and update
	 */
	ScanKeyInit(&key[0],
				Anum_pg_trigger_tgrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&key[1],
				Anum_pg_trigger_tgname,
				BTEqualStrategyNumber, F_NAMEEQ,
				PointerGetDatum(stmt->subname));
	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndexId, true,
								NULL, 2, key);
	if (HeapTupleIsValid(tuple = systable_getnext(tgscan)))
	{
		tgoid = HeapTupleGetOid(tuple);

		/*
		 * Update pg_trigger tuple with new tgname.
		 */
		tuple = heap_copytuple(tuple);	/* need a modifiable copy */

		namestrcpy(&((Form_pg_trigger) GETSTRUCT(tuple))->tgname,
				   stmt->newname);

		simple_heap_update(tgrel, &tuple->t_self, tuple);

		/* keep system catalog indexes current */
		CatalogUpdateIndexes(tgrel, tuple);

		InvokeObjectPostAlterHook(TriggerRelationId,
								  HeapTupleGetOid(tuple), 0);

		/*
		 * Invalidate relation's relcache entry so that other backends (and
		 * this one too!) are sent SI message to make them rebuild relcache
		 * entries.  (Ideally this should happen automatically...)
		 */
		CacheInvalidateRelcache(targetrel);
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("trigger \"%s\" for table \"%s\" does not exist",
						stmt->subname, RelationGetRelationName(targetrel))));
	}

	systable_endscan(tgscan);

	heap_close(tgrel, RowExclusiveLock);

	/*
	 * Close rel, but keep exclusive lock!
	 */
	relation_close(targetrel, NoLock);

	return tgoid;
}


/*
 * EnableDisableTrigger()
 *
 *	Called by ALTER TABLE ENABLE/DISABLE [ REPLICA | ALWAYS ] TRIGGER
 *	to change 'tgenabled' field for the specified trigger(s)
 *
 * rel: relation to process (caller must hold suitable lock on it)
 * tgname: trigger to process, or NULL to scan all triggers
 * fires_when: new value for tgenabled field. In addition to generic
 *			   enablement/disablement, this also defines when the trigger
 *			   should be fired in session replication roles.
 * skip_system: if true, skip "system" triggers (constraint triggers)
 *
 * Caller should have checked permissions for the table; here we also
 * enforce that superuser privilege is required to alter the state of
 * system triggers
 */
void
EnableDisableTrigger(Relation rel, const char *tgname,
					 char fires_when, bool skip_system)
{
	Relation	tgrel;
	int			nkeys;
	ScanKeyData keys[2];
	SysScanDesc tgscan;
	HeapTuple	tuple;
	bool		found;
	bool		changed;

	/* Scan the relevant entries in pg_triggers */
	tgrel = heap_open(TriggerRelationId, RowExclusiveLock);

	ScanKeyInit(&keys[0],
				Anum_pg_trigger_tgrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	if (tgname)
	{
		ScanKeyInit(&keys[1],
					Anum_pg_trigger_tgname,
					BTEqualStrategyNumber, F_NAMEEQ,
					CStringGetDatum(tgname));
		nkeys = 2;
	}
	else
		nkeys = 1;

	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndexId, true,
								NULL, nkeys, keys);

	found = changed = false;

	while (HeapTupleIsValid(tuple = systable_getnext(tgscan)))
	{
		Form_pg_trigger oldtrig = (Form_pg_trigger) GETSTRUCT(tuple);

		if (oldtrig->tgisinternal)
		{
			/* system trigger ... ok to process? */
			if (skip_system)
				continue;
			if (!superuser())
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					  errmsg("permission denied: \"%s\" is a system trigger",
							 NameStr(oldtrig->tgname))));
		}

		found = true;

		if (oldtrig->tgenabled != fires_when)
		{
			/* need to change this one ... make a copy to scribble on */
			HeapTuple	newtup = heap_copytuple(tuple);
			Form_pg_trigger newtrig = (Form_pg_trigger) GETSTRUCT(newtup);

			newtrig->tgenabled = fires_when;

			simple_heap_update(tgrel, &newtup->t_self, newtup);

			/* Keep catalog indexes current */
			CatalogUpdateIndexes(tgrel, newtup);

			heap_freetuple(newtup);

			changed = true;
		}

		InvokeObjectPostAlterHook(TriggerRelationId,
								  HeapTupleGetOid(tuple), 0);
	}

	systable_endscan(tgscan);

	heap_close(tgrel, RowExclusiveLock);

	if (tgname && !found)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("trigger \"%s\" for table \"%s\" does not exist",
						tgname, RelationGetRelationName(rel))));

	/*
	 * If we changed anything, broadcast a SI inval message to force each
	 * backend (including our own!) to rebuild relation's relcache entry.
	 * Otherwise they will fail to apply the change promptly.
	 */
	if (changed)
		CacheInvalidateRelcache(rel);
}


/*
 * Build trigger data to attach to the given relcache entry.
 *
 * Note that trigger data attached to a relcache entry must be stored in
 * CacheMemoryContext to ensure it survives as long as the relcache entry.
 * But we should be running in a less long-lived working context.  To avoid
 * leaking cache memory if this routine fails partway through, we build a
 * temporary TriggerDesc in working memory and then copy the completed
 * structure into cache memory.
 */
void
RelationBuildTriggers(Relation relation)
{
	TriggerDesc *trigdesc;
	int			numtrigs;
	int			maxtrigs;
	Trigger    *triggers;
	Relation	tgrel;
	ScanKeyData skey;
	SysScanDesc tgscan;
	HeapTuple	htup;
	MemoryContext oldContext;
	int			i;

	/*
	 * Allocate a working array to hold the triggers (the array is extended if
	 * necessary)
	 */
	maxtrigs = 16;
	triggers = (Trigger *) palloc(maxtrigs * sizeof(Trigger));
	numtrigs = 0;

	/*
	 * Note: since we scan the triggers using TriggerRelidNameIndexId, we will
	 * be reading the triggers in name order, except possibly during
	 * emergency-recovery operations (ie, IgnoreSystemIndexes). This in turn
	 * ensures that triggers will be fired in name order.
	 */
	ScanKeyInit(&skey,
				Anum_pg_trigger_tgrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(relation)));

	tgrel = heap_open(TriggerRelationId, AccessShareLock);
	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndexId, true,
								NULL, 1, &skey);

	while (HeapTupleIsValid(htup = systable_getnext(tgscan)))
	{
		Form_pg_trigger pg_trigger = (Form_pg_trigger) GETSTRUCT(htup);
		Trigger    *build;
		Datum		datum;
		bool		isnull;

		if (numtrigs >= maxtrigs)
		{
			maxtrigs *= 2;
			triggers = (Trigger *) repalloc(triggers, maxtrigs * sizeof(Trigger));
		}
		build = &(triggers[numtrigs]);

		build->tgoid = HeapTupleGetOid(htup);
		build->tgname = DatumGetCString(DirectFunctionCall1(nameout,
										 NameGetDatum(&pg_trigger->tgname)));
		build->tgfoid = pg_trigger->tgfoid;
		build->tgtype = pg_trigger->tgtype;
		build->tgenabled = pg_trigger->tgenabled;
		build->tgisinternal = pg_trigger->tgisinternal;
		build->tgconstrrelid = pg_trigger->tgconstrrelid;
		build->tgconstrindid = pg_trigger->tgconstrindid;
		build->tgconstraint = pg_trigger->tgconstraint;
		build->tgdeferrable = pg_trigger->tgdeferrable;
		build->tginitdeferred = pg_trigger->tginitdeferred;
		build->tgnargs = pg_trigger->tgnargs;
		/* tgattr is first var-width field, so OK to access directly */
		build->tgnattr = pg_trigger->tgattr.dim1;
		if (build->tgnattr > 0)
		{
			build->tgattr = (int16 *) palloc(build->tgnattr * sizeof(int16));
			memcpy(build->tgattr, &(pg_trigger->tgattr.values),
				   build->tgnattr * sizeof(int16));
		}
		else
			build->tgattr = NULL;
		if (build->tgnargs > 0)
		{
			bytea	   *val;
			char	   *p;

			val = DatumGetByteaP(fastgetattr(htup,
											 Anum_pg_trigger_tgargs,
											 tgrel->rd_att, &isnull));
			if (isnull)
				elog(ERROR, "tgargs is null in trigger for relation \"%s\"",
					 RelationGetRelationName(relation));
			p = (char *) VARDATA(val);
			build->tgargs = (char **) palloc(build->tgnargs * sizeof(char *));
			for (i = 0; i < build->tgnargs; i++)
			{
				build->tgargs[i] = pstrdup(p);
				p += strlen(p) + 1;
			}
		}
		else
			build->tgargs = NULL;
		datum = fastgetattr(htup, Anum_pg_trigger_tgqual,
							tgrel->rd_att, &isnull);
		if (!isnull)
			build->tgqual = TextDatumGetCString(datum);
		else
			build->tgqual = NULL;

		numtrigs++;
	}

	systable_endscan(tgscan);
	heap_close(tgrel, AccessShareLock);

	/* There might not be any triggers */
	if (numtrigs == 0)
	{
		pfree(triggers);
		return;
	}

	/* Build trigdesc */
	trigdesc = (TriggerDesc *) palloc0(sizeof(TriggerDesc));
	trigdesc->triggers = triggers;
	trigdesc->numtriggers = numtrigs;
	for (i = 0; i < numtrigs; i++)
		SetTriggerFlags(trigdesc, &(triggers[i]));

	/* Copy completed trigdesc into cache storage */
	oldContext = MemoryContextSwitchTo(CacheMemoryContext);
	relation->trigdesc = CopyTriggerDesc(trigdesc);
	MemoryContextSwitchTo(oldContext);

	/* Release working memory */
	FreeTriggerDesc(trigdesc);
}

/*
 * Update the TriggerDesc's hint flags to include the specified trigger
 */
static void
SetTriggerFlags(TriggerDesc *trigdesc, Trigger *trigger)
{
	int16		tgtype = trigger->tgtype;

	trigdesc->trig_insert_before_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_BEFORE, TRIGGER_TYPE_INSERT);
	trigdesc->trig_insert_after_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_AFTER, TRIGGER_TYPE_INSERT);
	trigdesc->trig_insert_instead_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_INSTEAD, TRIGGER_TYPE_INSERT);
	trigdesc->trig_insert_before_statement |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_STATEMENT,
							 TRIGGER_TYPE_BEFORE, TRIGGER_TYPE_INSERT);
	trigdesc->trig_insert_after_statement |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_STATEMENT,
							 TRIGGER_TYPE_AFTER, TRIGGER_TYPE_INSERT);
	trigdesc->trig_update_before_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_BEFORE, TRIGGER_TYPE_UPDATE);
	trigdesc->trig_update_after_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_AFTER, TRIGGER_TYPE_UPDATE);
	trigdesc->trig_update_instead_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_INSTEAD, TRIGGER_TYPE_UPDATE);
	trigdesc->trig_update_before_statement |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_STATEMENT,
							 TRIGGER_TYPE_BEFORE, TRIGGER_TYPE_UPDATE);
	trigdesc->trig_update_after_statement |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_STATEMENT,
							 TRIGGER_TYPE_AFTER, TRIGGER_TYPE_UPDATE);
	trigdesc->trig_delete_before_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_BEFORE, TRIGGER_TYPE_DELETE);
	trigdesc->trig_delete_after_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_AFTER, TRIGGER_TYPE_DELETE);
	trigdesc->trig_delete_instead_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_INSTEAD, TRIGGER_TYPE_DELETE);
	trigdesc->trig_delete_before_statement |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_STATEMENT,
							 TRIGGER_TYPE_BEFORE, TRIGGER_TYPE_DELETE);
	trigdesc->trig_delete_after_statement |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_STATEMENT,
							 TRIGGER_TYPE_AFTER, TRIGGER_TYPE_DELETE);
	/* there are no row-level truncate triggers */
	trigdesc->trig_truncate_before_statement |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_STATEMENT,
							 TRIGGER_TYPE_BEFORE, TRIGGER_TYPE_TRUNCATE);
	trigdesc->trig_truncate_after_statement |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_STATEMENT,
							 TRIGGER_TYPE_AFTER, TRIGGER_TYPE_TRUNCATE);
}

/*
 * Copy a TriggerDesc data structure.
 *
 * The copy is allocated in the current memory context.
 */
TriggerDesc *
CopyTriggerDesc(TriggerDesc *trigdesc)
{
	TriggerDesc *newdesc;
	Trigger    *trigger;
	int			i;

	if (trigdesc == NULL || trigdesc->numtriggers <= 0)
		return NULL;

	newdesc = (TriggerDesc *) palloc(sizeof(TriggerDesc));
	memcpy(newdesc, trigdesc, sizeof(TriggerDesc));

	trigger = (Trigger *) palloc(trigdesc->numtriggers * sizeof(Trigger));
	memcpy(trigger, trigdesc->triggers,
		   trigdesc->numtriggers * sizeof(Trigger));
	newdesc->triggers = trigger;

	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		trigger->tgname = pstrdup(trigger->tgname);
		if (trigger->tgnattr > 0)
		{
			int16	   *newattr;

			newattr = (int16 *) palloc(trigger->tgnattr * sizeof(int16));
			memcpy(newattr, trigger->tgattr,
				   trigger->tgnattr * sizeof(int16));
			trigger->tgattr = newattr;
		}
		if (trigger->tgnargs > 0)
		{
			char	  **newargs;
			int16		j;

			newargs = (char **) palloc(trigger->tgnargs * sizeof(char *));
			for (j = 0; j < trigger->tgnargs; j++)
				newargs[j] = pstrdup(trigger->tgargs[j]);
			trigger->tgargs = newargs;
		}
		if (trigger->tgqual)
			trigger->tgqual = pstrdup(trigger->tgqual);
		trigger++;
	}

	return newdesc;
}

/*
 * Free a TriggerDesc data structure.
 */
void
FreeTriggerDesc(TriggerDesc *trigdesc)
{
	Trigger    *trigger;
	int			i;

	if (trigdesc == NULL)
		return;

	trigger = trigdesc->triggers;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		pfree(trigger->tgname);
		if (trigger->tgnattr > 0)
			pfree(trigger->tgattr);
		if (trigger->tgnargs > 0)
		{
			while (--(trigger->tgnargs) >= 0)
				pfree(trigger->tgargs[trigger->tgnargs]);
			pfree(trigger->tgargs);
		}
		if (trigger->tgqual)
			pfree(trigger->tgqual);
		trigger++;
	}
	pfree(trigdesc->triggers);
	pfree(trigdesc);
}

/*
 * Compare two TriggerDesc structures for logical equality.
 */
#ifdef NOT_USED
bool
equalTriggerDescs(TriggerDesc *trigdesc1, TriggerDesc *trigdesc2)
{
	int			i,
				j;

	/*
	 * We need not examine the hint flags, just the trigger array itself; if
	 * we have the same triggers with the same types, the flags should match.
	 *
	 * As of 7.3 we assume trigger set ordering is significant in the
	 * comparison; so we just compare corresponding slots of the two sets.
	 *
	 * Note: comparing the stringToNode forms of the WHEN clauses means that
	 * parse column locations will affect the result.  This is okay as long as
	 * this function is only used for detecting exact equality, as for example
	 * in checking for staleness of a cache entry.
	 */
	if (trigdesc1 != NULL)
	{
		if (trigdesc2 == NULL)
			return false;
		if (trigdesc1->numtriggers != trigdesc2->numtriggers)
			return false;
		for (i = 0; i < trigdesc1->numtriggers; i++)
		{
			Trigger    *trig1 = trigdesc1->triggers + i;
			Trigger    *trig2 = trigdesc2->triggers + i;

			if (trig1->tgoid != trig2->tgoid)
				return false;
			if (strcmp(trig1->tgname, trig2->tgname) != 0)
				return false;
			if (trig1->tgfoid != trig2->tgfoid)
				return false;
			if (trig1->tgtype != trig2->tgtype)
				return false;
			if (trig1->tgenabled != trig2->tgenabled)
				return false;
			if (trig1->tgisinternal != trig2->tgisinternal)
				return false;
			if (trig1->tgconstrrelid != trig2->tgconstrrelid)
				return false;
			if (trig1->tgconstrindid != trig2->tgconstrindid)
				return false;
			if (trig1->tgconstraint != trig2->tgconstraint)
				return false;
			if (trig1->tgdeferrable != trig2->tgdeferrable)
				return false;
			if (trig1->tginitdeferred != trig2->tginitdeferred)
				return false;
			if (trig1->tgnargs != trig2->tgnargs)
				return false;
			if (trig1->tgnattr != trig2->tgnattr)
				return false;
			if (trig1->tgnattr > 0 &&
				memcmp(trig1->tgattr, trig2->tgattr,
					   trig1->tgnattr * sizeof(int16)) != 0)
				return false;
			for (j = 0; j < trig1->tgnargs; j++)
				if (strcmp(trig1->tgargs[j], trig2->tgargs[j]) != 0)
					return false;
			if (trig1->tgqual == NULL && trig2->tgqual == NULL)
				 /* ok */ ;
			else if (trig1->tgqual == NULL || trig2->tgqual == NULL)
				return false;
			else if (strcmp(trig1->tgqual, trig2->tgqual) != 0)
				return false;
		}
	}
	else if (trigdesc2 != NULL)
		return false;
	return true;
}
#endif   /* NOT_USED */

/*
 * Call a trigger function.
 *
 *		trigdata: trigger descriptor.
 *		tgindx: trigger's index in finfo and instr arrays.
 *		finfo: array of cached trigger function call information.
 *		instr: optional array of EXPLAIN ANALYZE instrumentation state.
 *		per_tuple_context: memory context to execute the function in.
 *
 * Returns the tuple (or NULL) as returned by the function.
 */
static HeapTuple
ExecCallTriggerFunc(TriggerData *trigdata,
					int tgindx,
					FmgrInfo *finfo,
					Instrumentation *instr,
					MemoryContext per_tuple_context)
{
	FunctionCallInfoData fcinfo;
	PgStat_FunctionCallUsage fcusage;
	Datum		result;
	MemoryContext oldContext;

	finfo += tgindx;

	/*
	 * We cache fmgr lookup info, to avoid making the lookup again on each
	 * call.
	 */
	if (finfo->fn_oid == InvalidOid)
		fmgr_info(trigdata->tg_trigger->tgfoid, finfo);

	Assert(finfo->fn_oid == trigdata->tg_trigger->tgfoid);

	/*
	 * If doing EXPLAIN ANALYZE, start charging time to this trigger.
	 */
	if (instr)
		InstrStartNode(instr + tgindx);

	/*
	 * Do the function evaluation in the per-tuple memory context, so that
	 * leaked memory will be reclaimed once per tuple. Note in particular that
	 * any new tuple created by the trigger function will live till the end of
	 * the tuple cycle.
	 */
	oldContext = MemoryContextSwitchTo(per_tuple_context);

	/*
	 * Call the function, passing no arguments but setting a context.
	 */
	InitFunctionCallInfoData(fcinfo, finfo, 0,
							 InvalidOid, (Node *) trigdata, NULL);

	pgstat_init_function_usage(&fcinfo, &fcusage);

	MyTriggerDepth++;
	PG_TRY();
	{
		result = FunctionCallInvoke(&fcinfo);
	}
	PG_CATCH();
	{
		MyTriggerDepth--;
		PG_RE_THROW();
	}
	PG_END_TRY();
	MyTriggerDepth--;

	pgstat_end_function_usage(&fcusage, true);

	MemoryContextSwitchTo(oldContext);

	/*
	 * Trigger protocol allows function to return a null pointer, but NOT to
	 * set the isnull result flag.
	 */
	if (fcinfo.isnull)
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("trigger function %u returned null value",
						fcinfo.flinfo->fn_oid)));

	/*
	 * If doing EXPLAIN ANALYZE, stop charging time to this trigger, and count
	 * one "tuple returned" (really the number of firings).
	 */
	if (instr)
		InstrStopNode(instr + tgindx, 1);

	return (HeapTuple) DatumGetPointer(result);
}

void
ExecBSInsertTriggers(EState *estate, ResultRelInfo *relinfo)
{
	TriggerDesc *trigdesc;
	int			i;
	TriggerData LocTriggerData;

	trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc == NULL)
		return;
	if (!trigdesc->trig_insert_before_statement)
		return;

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_INSERT |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	LocTriggerData.tg_trigtuple = NULL;
	LocTriggerData.tg_newtuple = NULL;
	LocTriggerData.tg_trigtuplebuf = InvalidBuffer;
	LocTriggerData.tg_newtuplebuf = InvalidBuffer;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];
		HeapTuple	newtuple;

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_STATEMENT,
								  TRIGGER_TYPE_BEFORE,
								  TRIGGER_TYPE_INSERT))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							NULL, NULL, NULL))
			continue;

		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));

		if (newtuple)
			ereport(ERROR,
					(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				  errmsg("BEFORE STATEMENT trigger cannot return a value")));
	}
}

void
ExecASInsertTriggers(EState *estate, ResultRelInfo *relinfo)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc && trigdesc->trig_insert_after_statement)
		AfterTriggerSaveEvent(estate, relinfo, TRIGGER_EVENT_INSERT,
							  false, NULL, NULL, NIL, NULL);
}

TupleTableSlot *
ExecBRInsertTriggers(EState *estate, ResultRelInfo *relinfo,
					 TupleTableSlot *slot)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;
	HeapTuple	slottuple = ExecMaterializeSlot(slot);
	HeapTuple	newtuple = slottuple;
	HeapTuple	oldtuple;
	TriggerData LocTriggerData;
	int			i;

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_INSERT |
		TRIGGER_EVENT_ROW |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	LocTriggerData.tg_newtuple = NULL;
	LocTriggerData.tg_newtuplebuf = InvalidBuffer;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_ROW,
								  TRIGGER_TYPE_BEFORE,
								  TRIGGER_TYPE_INSERT))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							NULL, NULL, newtuple))
			continue;

		LocTriggerData.tg_trigtuple = oldtuple = newtuple;
		LocTriggerData.tg_trigtuplebuf = InvalidBuffer;
		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));
		if (oldtuple != newtuple && oldtuple != slottuple)
			heap_freetuple(oldtuple);
		if (newtuple == NULL)
			return NULL;		/* "do nothing" */
	}

	if (newtuple != slottuple)
	{
		/*
		 * Return the modified tuple using the es_trig_tuple_slot.  We assume
		 * the tuple was allocated in per-tuple memory context, and therefore
		 * will go away by itself. The tuple table slot should not try to
		 * clear it.
		 */
		TupleTableSlot *newslot = estate->es_trig_tuple_slot;
		TupleDesc	tupdesc = RelationGetDescr(relinfo->ri_RelationDesc);

		if (newslot->tts_tupleDescriptor != tupdesc)
			ExecSetSlotDescriptor(newslot, tupdesc);
		ExecStoreTuple(newtuple, newslot, InvalidBuffer, false);
		slot = newslot;
	}
	return slot;
}

void
ExecARInsertTriggers(EState *estate, ResultRelInfo *relinfo,
					 HeapTuple trigtuple, List *recheckIndexes)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc && trigdesc->trig_insert_after_row)
		AfterTriggerSaveEvent(estate, relinfo, TRIGGER_EVENT_INSERT,
							  true, NULL, trigtuple, recheckIndexes, NULL);
}

TupleTableSlot *
ExecIRInsertTriggers(EState *estate, ResultRelInfo *relinfo,
					 TupleTableSlot *slot)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;
	HeapTuple	slottuple = ExecMaterializeSlot(slot);
	HeapTuple	newtuple = slottuple;
	HeapTuple	oldtuple;
	TriggerData LocTriggerData;
	int			i;

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_INSERT |
		TRIGGER_EVENT_ROW |
		TRIGGER_EVENT_INSTEAD;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	LocTriggerData.tg_newtuple = NULL;
	LocTriggerData.tg_newtuplebuf = InvalidBuffer;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_ROW,
								  TRIGGER_TYPE_INSTEAD,
								  TRIGGER_TYPE_INSERT))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							NULL, NULL, newtuple))
			continue;

		LocTriggerData.tg_trigtuple = oldtuple = newtuple;
		LocTriggerData.tg_trigtuplebuf = InvalidBuffer;
		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));
		if (oldtuple != newtuple && oldtuple != slottuple)
			heap_freetuple(oldtuple);
		if (newtuple == NULL)
			return NULL;		/* "do nothing" */
	}

	if (newtuple != slottuple)
	{
		/*
		 * Return the modified tuple using the es_trig_tuple_slot.  We assume
		 * the tuple was allocated in per-tuple memory context, and therefore
		 * will go away by itself. The tuple table slot should not try to
		 * clear it.
		 */
		TupleTableSlot *newslot = estate->es_trig_tuple_slot;
		TupleDesc	tupdesc = RelationGetDescr(relinfo->ri_RelationDesc);

		if (newslot->tts_tupleDescriptor != tupdesc)
			ExecSetSlotDescriptor(newslot, tupdesc);
		ExecStoreTuple(newtuple, newslot, InvalidBuffer, false);
		slot = newslot;
	}
	return slot;
}

void
ExecBSDeleteTriggers(EState *estate, ResultRelInfo *relinfo)
{
	TriggerDesc *trigdesc;
	int			i;
	TriggerData LocTriggerData;

	trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc == NULL)
		return;
	if (!trigdesc->trig_delete_before_statement)
		return;

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_DELETE |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	LocTriggerData.tg_trigtuple = NULL;
	LocTriggerData.tg_newtuple = NULL;
	LocTriggerData.tg_trigtuplebuf = InvalidBuffer;
	LocTriggerData.tg_newtuplebuf = InvalidBuffer;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];
		HeapTuple	newtuple;

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_STATEMENT,
								  TRIGGER_TYPE_BEFORE,
								  TRIGGER_TYPE_DELETE))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							NULL, NULL, NULL))
			continue;

		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));

		if (newtuple)
			ereport(ERROR,
					(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				  errmsg("BEFORE STATEMENT trigger cannot return a value")));
	}
}

void
ExecASDeleteTriggers(EState *estate, ResultRelInfo *relinfo)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc && trigdesc->trig_delete_after_statement)
		AfterTriggerSaveEvent(estate, relinfo, TRIGGER_EVENT_DELETE,
							  false, NULL, NULL, NIL, NULL);
}

bool
ExecBRDeleteTriggers(EState *estate, EPQState *epqstate,
					 ResultRelInfo *relinfo,
					 ItemPointer tupleid,
					 HeapTuple fdw_trigtuple)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;
	bool		result = true;
	TriggerData LocTriggerData;
	HeapTuple	trigtuple;
	HeapTuple	newtuple;
	TupleTableSlot *newSlot;
	int			i;

	Assert(HeapTupleIsValid(fdw_trigtuple) ^ ItemPointerIsValid(tupleid));
	if (fdw_trigtuple == NULL)
	{
		trigtuple = GetTupleForTrigger(estate, epqstate, relinfo, tupleid,
									   LockTupleExclusive, &newSlot);
		if (trigtuple == NULL)
			return false;
	}
	else
		trigtuple = fdw_trigtuple;

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_DELETE |
		TRIGGER_EVENT_ROW |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	LocTriggerData.tg_newtuple = NULL;
	LocTriggerData.tg_newtuplebuf = InvalidBuffer;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_ROW,
								  TRIGGER_TYPE_BEFORE,
								  TRIGGER_TYPE_DELETE))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							NULL, trigtuple, NULL))
			continue;

		LocTriggerData.tg_trigtuple = trigtuple;
		LocTriggerData.tg_trigtuplebuf = InvalidBuffer;
		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));
		if (newtuple == NULL)
		{
			result = false;		/* tell caller to suppress delete */
			break;
		}
		if (newtuple != trigtuple)
			heap_freetuple(newtuple);
	}
	if (trigtuple != fdw_trigtuple)
		heap_freetuple(trigtuple);

	return result;
}

void
ExecARDeleteTriggers(EState *estate, ResultRelInfo *relinfo,
					 ItemPointer tupleid,
					 HeapTuple fdw_trigtuple)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc && trigdesc->trig_delete_after_row)
	{
		HeapTuple	trigtuple;

		Assert(HeapTupleIsValid(fdw_trigtuple) ^ ItemPointerIsValid(tupleid));
		if (fdw_trigtuple == NULL)
			trigtuple = GetTupleForTrigger(estate,
										   NULL,
										   relinfo,
										   tupleid,
										   LockTupleExclusive,
										   NULL);
		else
			trigtuple = fdw_trigtuple;

		AfterTriggerSaveEvent(estate, relinfo, TRIGGER_EVENT_DELETE,
							  true, trigtuple, NULL, NIL, NULL);
		if (trigtuple != fdw_trigtuple)
			heap_freetuple(trigtuple);
	}
}

bool
ExecIRDeleteTriggers(EState *estate, ResultRelInfo *relinfo,
					 HeapTuple trigtuple)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;
	TriggerData LocTriggerData;
	HeapTuple	rettuple;
	int			i;

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_DELETE |
		TRIGGER_EVENT_ROW |
		TRIGGER_EVENT_INSTEAD;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	LocTriggerData.tg_newtuple = NULL;
	LocTriggerData.tg_newtuplebuf = InvalidBuffer;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_ROW,
								  TRIGGER_TYPE_INSTEAD,
								  TRIGGER_TYPE_DELETE))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							NULL, trigtuple, NULL))
			continue;

		LocTriggerData.tg_trigtuple = trigtuple;
		LocTriggerData.tg_trigtuplebuf = InvalidBuffer;
		LocTriggerData.tg_trigger = trigger;
		rettuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));
		if (rettuple == NULL)
			return false;		/* Delete was suppressed */
		if (rettuple != trigtuple)
			heap_freetuple(rettuple);
	}
	return true;
}

void
ExecBSUpdateTriggers(EState *estate, ResultRelInfo *relinfo)
{
	TriggerDesc *trigdesc;
	int			i;
	TriggerData LocTriggerData;
	Bitmapset  *modifiedCols;

	trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc == NULL)
		return;
	if (!trigdesc->trig_update_before_statement)
		return;

	modifiedCols = GetModifiedColumns(relinfo, estate);

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_UPDATE |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	LocTriggerData.tg_trigtuple = NULL;
	LocTriggerData.tg_newtuple = NULL;
	LocTriggerData.tg_trigtuplebuf = InvalidBuffer;
	LocTriggerData.tg_newtuplebuf = InvalidBuffer;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];
		HeapTuple	newtuple;

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_STATEMENT,
								  TRIGGER_TYPE_BEFORE,
								  TRIGGER_TYPE_UPDATE))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							modifiedCols, NULL, NULL))
			continue;

		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));

		if (newtuple)
			ereport(ERROR,
					(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				  errmsg("BEFORE STATEMENT trigger cannot return a value")));
	}
}

void
ExecASUpdateTriggers(EState *estate, ResultRelInfo *relinfo)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc && trigdesc->trig_update_after_statement)
		AfterTriggerSaveEvent(estate, relinfo, TRIGGER_EVENT_UPDATE,
							  false, NULL, NULL, NIL,
							  GetModifiedColumns(relinfo, estate));
}

TupleTableSlot *
ExecBRUpdateTriggers(EState *estate, EPQState *epqstate,
					 ResultRelInfo *relinfo,
					 ItemPointer tupleid,
					 HeapTuple fdw_trigtuple,
					 TupleTableSlot *slot)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;
	HeapTuple	slottuple = ExecMaterializeSlot(slot);
	HeapTuple	newtuple = slottuple;
	TriggerData LocTriggerData;
	HeapTuple	trigtuple;
	HeapTuple	oldtuple;
	TupleTableSlot *newSlot;
	int			i;
	Bitmapset  *modifiedCols;
	Bitmapset  *keyCols;
	LockTupleMode lockmode;

	/*
	 * Compute lock mode to use.  If columns that are part of the key have not
	 * been modified, then we can use a weaker lock, allowing for better
	 * concurrency.
	 */
	modifiedCols = GetModifiedColumns(relinfo, estate);
	keyCols = RelationGetIndexAttrBitmap(relinfo->ri_RelationDesc,
										 INDEX_ATTR_BITMAP_KEY);
	if (bms_overlap(keyCols, modifiedCols))
		lockmode = LockTupleExclusive;
	else
		lockmode = LockTupleNoKeyExclusive;

	Assert(HeapTupleIsValid(fdw_trigtuple) ^ ItemPointerIsValid(tupleid));
	if (fdw_trigtuple == NULL)
	{
		/* get a copy of the on-disk tuple we are planning to update */
		trigtuple = GetTupleForTrigger(estate, epqstate, relinfo, tupleid,
									   lockmode, &newSlot);
		if (trigtuple == NULL)
			return NULL;		/* cancel the update action */
	}
	else
	{
		trigtuple = fdw_trigtuple;
		newSlot = NULL;
	}

	/*
	 * In READ COMMITTED isolation level it's possible that target tuple was
	 * changed due to concurrent update.  In that case we have a raw subplan
	 * output tuple in newSlot, and need to run it through the junk filter to
	 * produce an insertable tuple.
	 *
	 * Caution: more than likely, the passed-in slot is the same as the
	 * junkfilter's output slot, so we are clobbering the original value of
	 * slottuple by doing the filtering.  This is OK since neither we nor our
	 * caller have any more interest in the prior contents of that slot.
	 */
	if (newSlot != NULL)
	{
		slot = ExecFilterJunk(relinfo->ri_junkFilter, newSlot);
		slottuple = ExecMaterializeSlot(slot);
		newtuple = slottuple;
	}


	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_UPDATE |
		TRIGGER_EVENT_ROW |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_ROW,
								  TRIGGER_TYPE_BEFORE,
								  TRIGGER_TYPE_UPDATE))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							modifiedCols, trigtuple, newtuple))
			continue;

		LocTriggerData.tg_trigtuple = trigtuple;
		LocTriggerData.tg_newtuple = oldtuple = newtuple;
		LocTriggerData.tg_trigtuplebuf = InvalidBuffer;
		LocTriggerData.tg_newtuplebuf = InvalidBuffer;
		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));
		if (oldtuple != newtuple && oldtuple != slottuple)
			heap_freetuple(oldtuple);
		if (newtuple == NULL)
		{
			if (trigtuple != fdw_trigtuple)
				heap_freetuple(trigtuple);
			return NULL;		/* "do nothing" */
		}
	}
	if (trigtuple != fdw_trigtuple)
		heap_freetuple(trigtuple);

	if (newtuple != slottuple)
	{
		/*
		 * Return the modified tuple using the es_trig_tuple_slot.  We assume
		 * the tuple was allocated in per-tuple memory context, and therefore
		 * will go away by itself. The tuple table slot should not try to
		 * clear it.
		 */
		TupleTableSlot *newslot = estate->es_trig_tuple_slot;
		TupleDesc	tupdesc = RelationGetDescr(relinfo->ri_RelationDesc);

		if (newslot->tts_tupleDescriptor != tupdesc)
			ExecSetSlotDescriptor(newslot, tupdesc);
		ExecStoreTuple(newtuple, newslot, InvalidBuffer, false);
		slot = newslot;
	}
	return slot;
}

void
ExecARUpdateTriggers(EState *estate, ResultRelInfo *relinfo,
					 ItemPointer tupleid,
					 HeapTuple fdw_trigtuple,
					 HeapTuple newtuple,
					 List *recheckIndexes)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc && trigdesc->trig_update_after_row)
	{
		HeapTuple	trigtuple;

		Assert(HeapTupleIsValid(fdw_trigtuple) ^ ItemPointerIsValid(tupleid));
		if (fdw_trigtuple == NULL)
			trigtuple = GetTupleForTrigger(estate,
										   NULL,
										   relinfo,
										   tupleid,
										   LockTupleExclusive,
										   NULL);
		else
			trigtuple = fdw_trigtuple;

		AfterTriggerSaveEvent(estate, relinfo, TRIGGER_EVENT_UPDATE,
							  true, trigtuple, newtuple, recheckIndexes,
							  GetModifiedColumns(relinfo, estate));
		if (trigtuple != fdw_trigtuple)
			heap_freetuple(trigtuple);
	}
}

TupleTableSlot *
ExecIRUpdateTriggers(EState *estate, ResultRelInfo *relinfo,
					 HeapTuple trigtuple, TupleTableSlot *slot)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;
	HeapTuple	slottuple = ExecMaterializeSlot(slot);
	HeapTuple	newtuple = slottuple;
	TriggerData LocTriggerData;
	HeapTuple	oldtuple;
	int			i;

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_UPDATE |
		TRIGGER_EVENT_ROW |
		TRIGGER_EVENT_INSTEAD;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_ROW,
								  TRIGGER_TYPE_INSTEAD,
								  TRIGGER_TYPE_UPDATE))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							NULL, trigtuple, newtuple))
			continue;

		LocTriggerData.tg_trigtuple = trigtuple;
		LocTriggerData.tg_newtuple = oldtuple = newtuple;
		LocTriggerData.tg_trigtuplebuf = InvalidBuffer;
		LocTriggerData.tg_newtuplebuf = InvalidBuffer;
		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));
		if (oldtuple != newtuple && oldtuple != slottuple)
			heap_freetuple(oldtuple);
		if (newtuple == NULL)
			return NULL;		/* "do nothing" */
	}

	if (newtuple != slottuple)
	{
		/*
		 * Return the modified tuple using the es_trig_tuple_slot.  We assume
		 * the tuple was allocated in per-tuple memory context, and therefore
		 * will go away by itself. The tuple table slot should not try to
		 * clear it.
		 */
		TupleTableSlot *newslot = estate->es_trig_tuple_slot;
		TupleDesc	tupdesc = RelationGetDescr(relinfo->ri_RelationDesc);

		if (newslot->tts_tupleDescriptor != tupdesc)
			ExecSetSlotDescriptor(newslot, tupdesc);
		ExecStoreTuple(newtuple, newslot, InvalidBuffer, false);
		slot = newslot;
	}
	return slot;
}

void
ExecBSTruncateTriggers(EState *estate, ResultRelInfo *relinfo)
{
	TriggerDesc *trigdesc;
	int			i;
	TriggerData LocTriggerData;

	trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc == NULL)
		return;
	if (!trigdesc->trig_truncate_before_statement)
		return;

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_TRUNCATE |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	LocTriggerData.tg_trigtuple = NULL;
	LocTriggerData.tg_newtuple = NULL;
	LocTriggerData.tg_trigtuplebuf = InvalidBuffer;
	LocTriggerData.tg_newtuplebuf = InvalidBuffer;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];
		HeapTuple	newtuple;

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_STATEMENT,
								  TRIGGER_TYPE_BEFORE,
								  TRIGGER_TYPE_TRUNCATE))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							NULL, NULL, NULL))
			continue;

		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));

		if (newtuple)
			ereport(ERROR,
					(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				  errmsg("BEFORE STATEMENT trigger cannot return a value")));
	}
}

void
ExecASTruncateTriggers(EState *estate, ResultRelInfo *relinfo)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc && trigdesc->trig_truncate_after_statement)
		AfterTriggerSaveEvent(estate, relinfo, TRIGGER_EVENT_TRUNCATE,
							  false, NULL, NULL, NIL, NULL);
}


static HeapTuple
GetTupleForTrigger(EState *estate,
				   EPQState *epqstate,
				   ResultRelInfo *relinfo,
				   ItemPointer tid,
				   LockTupleMode lockmode,
				   TupleTableSlot **newSlot)
{
	Relation	relation = relinfo->ri_RelationDesc;
	HeapTupleData tuple;
	HeapTuple	result;
	Buffer		buffer;

	if (newSlot != NULL)
	{
		HTSU_Result test;
		HeapUpdateFailureData hufd;

		*newSlot = NULL;

		/* caller must pass an epqstate if EvalPlanQual is possible */
		Assert(epqstate != NULL);

		/*
		 * lock tuple for update
		 */
ltrmark:;
		tuple.t_self = *tid;
		test = heap_lock_tuple(relation, &tuple,
							   estate->es_output_cid,
							   lockmode, LockWaitBlock,
							   false, &buffer, &hufd);
		switch (test)
		{
			case HeapTupleSelfUpdated:

				/*
				 * The target tuple was already updated or deleted by the
				 * current command, or by a later command in the current
				 * transaction.  We ignore the tuple in the former case, and
				 * throw error in the latter case, for the same reasons
				 * enumerated in ExecUpdate and ExecDelete in
				 * nodeModifyTable.c.
				 */
				if (hufd.cmax != estate->es_output_cid)
					ereport(ERROR,
							(errcode(ERRCODE_TRIGGERED_DATA_CHANGE_VIOLATION),
							 errmsg("tuple to be updated was already modified by an operation triggered by the current command"),
							 errhint("Consider using an AFTER trigger instead of a BEFORE trigger to propagate changes to other rows.")));

				/* treat it as deleted; do not process */
				ReleaseBuffer(buffer);
				return NULL;

			case HeapTupleMayBeUpdated:
				break;

			case HeapTupleUpdated:
				ReleaseBuffer(buffer);
				if (IsolationUsesXactSnapshot())
					ereport(ERROR,
							(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							 errmsg("could not serialize access due to concurrent update")));
				if (!ItemPointerEquals(&hufd.ctid, &tuple.t_self))
				{
					/* it was updated, so look at the updated version */
					TupleTableSlot *epqslot;

					epqslot = EvalPlanQual(estate,
										   epqstate,
										   relation,
										   relinfo->ri_RangeTableIndex,
										   lockmode,
										   &hufd.ctid,
										   hufd.xmax);
					if (!TupIsNull(epqslot))
					{
						*tid = hufd.ctid;
						*newSlot = epqslot;

						/*
						 * EvalPlanQual already locked the tuple, but we
						 * re-call heap_lock_tuple anyway as an easy way of
						 * re-fetching the correct tuple.  Speed is hardly a
						 * criterion in this path anyhow.
						 */
						goto ltrmark;
					}
				}

				/*
				 * if tuple was deleted or PlanQual failed for updated tuple -
				 * we must not process this tuple!
				 */
				return NULL;

			default:
				ReleaseBuffer(buffer);
				elog(ERROR, "unrecognized heap_lock_tuple status: %u", test);
				return NULL;	/* keep compiler quiet */
		}
	}
	else
	{
		Page		page;
		ItemId		lp;

		buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(tid));

		/*
		 * Although we already know this tuple is valid, we must lock the
		 * buffer to ensure that no one has a buffer cleanup lock; otherwise
		 * they might move the tuple while we try to copy it.  But we can
		 * release the lock before actually doing the heap_copytuple call,
		 * since holding pin is sufficient to prevent anyone from getting a
		 * cleanup lock they don't already hold.
		 */
		LockBuffer(buffer, BUFFER_LOCK_SHARE);

		page = BufferGetPage(buffer);
		lp = PageGetItemId(page, ItemPointerGetOffsetNumber(tid));

		Assert(ItemIdIsNormal(lp));

		tuple.t_data = (HeapTupleHeader) PageGetItem(page, lp);
		tuple.t_len = ItemIdGetLength(lp);
		tuple.t_self = *tid;
		tuple.t_tableOid = RelationGetRelid(relation);

		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	}

	result = heap_copytuple(&tuple);
	ReleaseBuffer(buffer);

	return result;
}

/*
 * Is trigger enabled to fire?
 */
static bool
TriggerEnabled(EState *estate, ResultRelInfo *relinfo,
			   Trigger *trigger, TriggerEvent event,
			   Bitmapset *modifiedCols,
			   HeapTuple oldtup, HeapTuple newtup)
{
	/* Check replication-role-dependent enable state */
	if (SessionReplicationRole == SESSION_REPLICATION_ROLE_REPLICA)
	{
		if (trigger->tgenabled == TRIGGER_FIRES_ON_ORIGIN ||
			trigger->tgenabled == TRIGGER_DISABLED)
			return false;
	}
	else	/* ORIGIN or LOCAL role */
	{
		if (trigger->tgenabled == TRIGGER_FIRES_ON_REPLICA ||
			trigger->tgenabled == TRIGGER_DISABLED)
			return false;
	}

	/*
	 * Check for column-specific trigger (only possible for UPDATE, and in
	 * fact we *must* ignore tgattr for other event types)
	 */
	if (trigger->tgnattr > 0 && TRIGGER_FIRED_BY_UPDATE(event))
	{
		int			i;
		bool		modified;

		modified = false;
		for (i = 0; i < trigger->tgnattr; i++)
		{
			if (bms_is_member(trigger->tgattr[i] - FirstLowInvalidHeapAttributeNumber,
							  modifiedCols))
			{
				modified = true;
				break;
			}
		}
		if (!modified)
			return false;
	}

	/* Check for WHEN clause */
	if (trigger->tgqual)
	{
		TupleDesc	tupdesc = RelationGetDescr(relinfo->ri_RelationDesc);
		List	  **predicate;
		ExprContext *econtext;
		TupleTableSlot *oldslot = NULL;
		TupleTableSlot *newslot = NULL;
		MemoryContext oldContext;
		int			i;

		Assert(estate != NULL);

		/*
		 * trigger is an element of relinfo->ri_TrigDesc->triggers[]; find the
		 * matching element of relinfo->ri_TrigWhenExprs[]
		 */
		i = trigger - relinfo->ri_TrigDesc->triggers;
		predicate = &relinfo->ri_TrigWhenExprs[i];

		/*
		 * If first time through for this WHEN expression, build expression
		 * nodetrees for it.  Keep them in the per-query memory context so
		 * they'll survive throughout the query.
		 */
		if (*predicate == NIL)
		{
			Node	   *tgqual;

			oldContext = MemoryContextSwitchTo(estate->es_query_cxt);
			tgqual = stringToNode(trigger->tgqual);
			/* Change references to OLD and NEW to INNER_VAR and OUTER_VAR */
			ChangeVarNodes(tgqual, PRS2_OLD_VARNO, INNER_VAR, 0);
			ChangeVarNodes(tgqual, PRS2_NEW_VARNO, OUTER_VAR, 0);
			/* ExecQual wants implicit-AND form */
			tgqual = (Node *) make_ands_implicit((Expr *) tgqual);
			*predicate = (List *) ExecPrepareExpr((Expr *) tgqual, estate);
			MemoryContextSwitchTo(oldContext);
		}

		/*
		 * We will use the EState's per-tuple context for evaluating WHEN
		 * expressions (creating it if it's not already there).
		 */
		econtext = GetPerTupleExprContext(estate);

		/*
		 * Put OLD and NEW tuples into tupleslots for expression evaluation.
		 * These slots can be shared across the whole estate, but be careful
		 * that they have the current resultrel's tupdesc.
		 */
		if (HeapTupleIsValid(oldtup))
		{
			if (estate->es_trig_oldtup_slot == NULL)
			{
				oldContext = MemoryContextSwitchTo(estate->es_query_cxt);
				estate->es_trig_oldtup_slot = ExecInitExtraTupleSlot(estate);
				MemoryContextSwitchTo(oldContext);
			}
			oldslot = estate->es_trig_oldtup_slot;
			if (oldslot->tts_tupleDescriptor != tupdesc)
				ExecSetSlotDescriptor(oldslot, tupdesc);
			ExecStoreTuple(oldtup, oldslot, InvalidBuffer, false);
		}
		if (HeapTupleIsValid(newtup))
		{
			if (estate->es_trig_newtup_slot == NULL)
			{
				oldContext = MemoryContextSwitchTo(estate->es_query_cxt);
				estate->es_trig_newtup_slot = ExecInitExtraTupleSlot(estate);
				MemoryContextSwitchTo(oldContext);
			}
			newslot = estate->es_trig_newtup_slot;
			if (newslot->tts_tupleDescriptor != tupdesc)
				ExecSetSlotDescriptor(newslot, tupdesc);
			ExecStoreTuple(newtup, newslot, InvalidBuffer, false);
		}

		/*
		 * Finally evaluate the expression, making the old and/or new tuples
		 * available as INNER_VAR/OUTER_VAR respectively.
		 */
		econtext->ecxt_innertuple = oldslot;
		econtext->ecxt_outertuple = newslot;
		if (!ExecQual(*predicate, econtext, false))
			return false;
	}

	return true;
}


/* ----------
 * After-trigger stuff
 *
 * The AfterTriggersData struct holds data about pending AFTER trigger events
 * during the current transaction tree.  (BEFORE triggers are fired
 * immediately so we don't need any persistent state about them.)  The struct
 * and most of its subsidiary data are kept in TopTransactionContext; however
 * the individual event records are kept in a separate sub-context.  This is
 * done mainly so that it's easy to tell from a memory context dump how much
 * space is being eaten by trigger events.
 *
 * Because the list of pending events can grow large, we go to some
 * considerable effort to minimize per-event memory consumption.  The event
 * records are grouped into chunks and common data for similar events in the
 * same chunk is only stored once.
 *
 * XXX We need to be able to save the per-event data in a file if it grows too
 * large.
 * ----------
 */

/* Per-trigger SET CONSTRAINT status */
typedef struct SetConstraintTriggerData
{
	Oid			sct_tgoid;
	bool		sct_tgisdeferred;
} SetConstraintTriggerData;

typedef struct SetConstraintTriggerData *SetConstraintTrigger;

/*
 * SET CONSTRAINT intra-transaction status.
 *
 * We make this a single palloc'd object so it can be copied and freed easily.
 *
 * all_isset and all_isdeferred are used to keep track
 * of SET CONSTRAINTS ALL {DEFERRED, IMMEDIATE}.
 *
 * trigstates[] stores per-trigger tgisdeferred settings.
 */
typedef struct SetConstraintStateData
{
	bool		all_isset;
	bool		all_isdeferred;
	int			numstates;		/* number of trigstates[] entries in use */
	int			numalloc;		/* allocated size of trigstates[] */
	SetConstraintTriggerData trigstates[1];		/* VARIABLE LENGTH ARRAY */
} SetConstraintStateData;

typedef SetConstraintStateData *SetConstraintState;


/*
 * Per-trigger-event data
 *
 * The actual per-event data, AfterTriggerEventData, includes DONE/IN_PROGRESS
 * status bits and up to two tuple CTIDs.  Each event record also has an
 * associated AfterTriggerSharedData that is shared across all instances of
 * similar events within a "chunk".
 *
 * For row-level triggers, we arrange not to waste storage on unneeded ctid
 * fields.  Updates of regular tables use two; inserts and deletes of regular
 * tables use one; foreign tables always use zero and save the tuple(s) to a
 * tuplestore.  AFTER_TRIGGER_FDW_FETCH directs AfterTriggerExecute() to
 * retrieve a fresh tuple or pair of tuples from that tuplestore, while
 * AFTER_TRIGGER_FDW_REUSE directs it to use the most-recently-retrieved
 * tuple(s).  This permits storing tuples once regardless of the number of
 * row-level triggers on a foreign table.
 *
 * Statement-level triggers always bear AFTER_TRIGGER_1CTID, though they
 * require no ctid field.  We lack the flag bit space to neatly represent that
 * distinct case, and it seems unlikely to be worth much trouble.
 *
 * Note: ats_firing_id is initially zero and is set to something else when
 * AFTER_TRIGGER_IN_PROGRESS is set.  It indicates which trigger firing
 * cycle the trigger will be fired in (or was fired in, if DONE is set).
 * Although this is mutable state, we can keep it in AfterTriggerSharedData
 * because all instances of the same type of event in a given event list will
 * be fired at the same time, if they were queued between the same firing
 * cycles.  So we need only ensure that ats_firing_id is zero when attaching
 * a new event to an existing AfterTriggerSharedData record.
 */
typedef uint32 TriggerFlags;

#define AFTER_TRIGGER_OFFSET			0x0FFFFFFF		/* must be low-order
														 * bits */
#define AFTER_TRIGGER_DONE				0x10000000
#define AFTER_TRIGGER_IN_PROGRESS		0x20000000
/* bits describing the size and tuple sources of this event */
#define AFTER_TRIGGER_FDW_REUSE			0x00000000
#define AFTER_TRIGGER_FDW_FETCH			0x80000000
#define AFTER_TRIGGER_1CTID				0x40000000
#define AFTER_TRIGGER_2CTID				0xC0000000
#define AFTER_TRIGGER_TUP_BITS			0xC0000000

typedef struct AfterTriggerSharedData *AfterTriggerShared;

typedef struct AfterTriggerSharedData
{
	TriggerEvent ats_event;		/* event type indicator, see trigger.h */
	Oid			ats_tgoid;		/* the trigger's ID */
	Oid			ats_relid;		/* the relation it's on */
	CommandId	ats_firing_id;	/* ID for firing cycle */
} AfterTriggerSharedData;

typedef struct AfterTriggerEventData *AfterTriggerEvent;

typedef struct AfterTriggerEventData
{
	TriggerFlags ate_flags;		/* status bits and offset to shared data */
	ItemPointerData ate_ctid1;	/* inserted, deleted, or old updated tuple */
	ItemPointerData ate_ctid2;	/* new updated tuple */
} AfterTriggerEventData;

/* AfterTriggerEventData, minus ate_ctid2 */
typedef struct AfterTriggerEventDataOneCtid
{
	TriggerFlags ate_flags;		/* status bits and offset to shared data */
	ItemPointerData ate_ctid1;	/* inserted, deleted, or old updated tuple */
}	AfterTriggerEventDataOneCtid;

/* AfterTriggerEventData, minus ate_ctid1 and ate_ctid2 */
typedef struct AfterTriggerEventDataZeroCtids
{
	TriggerFlags ate_flags;		/* status bits and offset to shared data */
}	AfterTriggerEventDataZeroCtids;

#define SizeofTriggerEvent(evt) \
	(((evt)->ate_flags & AFTER_TRIGGER_TUP_BITS) == AFTER_TRIGGER_2CTID ? \
	 sizeof(AfterTriggerEventData) : \
		((evt)->ate_flags & AFTER_TRIGGER_TUP_BITS) == AFTER_TRIGGER_1CTID ? \
		sizeof(AfterTriggerEventDataOneCtid) : \
			sizeof(AfterTriggerEventDataZeroCtids))

#define GetTriggerSharedData(evt) \
	((AfterTriggerShared) ((char *) (evt) + ((evt)->ate_flags & AFTER_TRIGGER_OFFSET)))

/*
 * To avoid palloc overhead, we keep trigger events in arrays in successively-
 * larger chunks (a slightly more sophisticated version of an expansible
 * array).  The space between CHUNK_DATA_START and freeptr is occupied by
 * AfterTriggerEventData records; the space between endfree and endptr is
 * occupied by AfterTriggerSharedData records.
 */
typedef struct AfterTriggerEventChunk
{
	struct AfterTriggerEventChunk *next;		/* list link */
	char	   *freeptr;		/* start of free space in chunk */
	char	   *endfree;		/* end of free space in chunk */
	char	   *endptr;			/* end of chunk */
	/* event data follows here */
} AfterTriggerEventChunk;

#define CHUNK_DATA_START(cptr) ((char *) (cptr) + MAXALIGN(sizeof(AfterTriggerEventChunk)))

/* A list of events */
typedef struct AfterTriggerEventList
{
	AfterTriggerEventChunk *head;
	AfterTriggerEventChunk *tail;
	char	   *tailfree;		/* freeptr of tail chunk */
} AfterTriggerEventList;

/* Macros to help in iterating over a list of events */
#define for_each_chunk(cptr, evtlist) \
	for (cptr = (evtlist).head; cptr != NULL; cptr = cptr->next)
#define for_each_event(eptr, cptr) \
	for (eptr = (AfterTriggerEvent) CHUNK_DATA_START(cptr); \
		 (char *) eptr < (cptr)->freeptr; \
		 eptr = (AfterTriggerEvent) (((char *) eptr) + SizeofTriggerEvent(eptr)))
/* Use this if no special per-chunk processing is needed */
#define for_each_event_chunk(eptr, cptr, evtlist) \
	for_each_chunk(cptr, evtlist) for_each_event(eptr, cptr)


/*
 * All per-transaction data for the AFTER TRIGGERS module.
 *
 * AfterTriggersData has the following fields:
 *
 * firing_counter is incremented for each call of afterTriggerInvokeEvents.
 * We mark firable events with the current firing cycle's ID so that we can
 * tell which ones to work on.  This ensures sane behavior if a trigger
 * function chooses to do SET CONSTRAINTS: the inner SET CONSTRAINTS will
 * only fire those events that weren't already scheduled for firing.
 *
 * state keeps track of the transaction-local effects of SET CONSTRAINTS.
 * This is saved and restored across failed subtransactions.
 *
 * events is the current list of deferred events.  This is global across
 * all subtransactions of the current transaction.  In a subtransaction
 * abort, we know that the events added by the subtransaction are at the
 * end of the list, so it is relatively easy to discard them.  The event
 * list chunks themselves are stored in event_cxt.
 *
 * query_depth is the current depth of nested AfterTriggerBeginQuery calls
 * (-1 when the stack is empty).
 *
 * query_stack[query_depth] is a list of AFTER trigger events queued by the
 * current query (and the query_stack entries below it are lists of trigger
 * events queued by calling queries).  None of these are valid until the
 * matching AfterTriggerEndQuery call occurs.  At that point we fire
 * immediate-mode triggers, and append any deferred events to the main events
 * list.
 *
 * fdw_tuplestores[query_depth] is a tuplestore containing the foreign tuples
 * needed for the current query.
 *
 * maxquerydepth is just the allocated length of query_stack and
 * fdw_tuplestores.
 *
 * state_stack is a stack of pointers to saved copies of the SET CONSTRAINTS
 * state data; each subtransaction level that modifies that state first
 * saves a copy, which we use to restore the state if we abort.
 *
 * events_stack is a stack of copies of the events head/tail pointers,
 * which we use to restore those values during subtransaction abort.
 *
 * depth_stack is a stack of copies of subtransaction-start-time query_depth,
 * which we similarly use to clean up at subtransaction abort.
 *
 * firing_stack is a stack of copies of subtransaction-start-time
 * firing_counter.  We use this to recognize which deferred triggers were
 * fired (or marked for firing) within an aborted subtransaction.
 *
 * We use GetCurrentTransactionNestLevel() to determine the correct array
 * index in these stacks.  maxtransdepth is the number of allocated entries in
 * each stack.  (By not keeping our own stack pointer, we can avoid trouble
 * in cases where errors during subxact abort cause multiple invocations
 * of AfterTriggerEndSubXact() at the same nesting depth.)
 */
typedef struct AfterTriggersData
{
	CommandId	firing_counter; /* next firing ID to assign */
	SetConstraintState state;	/* the active S C state */
	AfterTriggerEventList events;		/* deferred-event list */
	int			query_depth;	/* current query list index */
	AfterTriggerEventList *query_stack; /* events pending from each query */
	Tuplestorestate **fdw_tuplestores;	/* foreign tuples from each query */
	int			maxquerydepth;	/* allocated len of above array */
	MemoryContext event_cxt;	/* memory context for events, if any */

	/* these fields are just for resetting at subtrans abort: */

	SetConstraintState *state_stack;	/* stacked S C states */
	AfterTriggerEventList *events_stack;		/* stacked list pointers */
	int		   *depth_stack;	/* stacked query_depths */
	CommandId  *firing_stack;	/* stacked firing_counters */
	int			maxtransdepth;	/* allocated len of above arrays */
} AfterTriggersData;

static AfterTriggersData afterTriggers;

static void AfterTriggerExecute(AfterTriggerEvent event,
					Relation rel, TriggerDesc *trigdesc,
					FmgrInfo *finfo,
					Instrumentation *instr,
					MemoryContext per_tuple_context,
					TupleTableSlot *trig_tuple_slot1,
					TupleTableSlot *trig_tuple_slot2);
static SetConstraintState SetConstraintStateCreate(int numalloc);
static SetConstraintState SetConstraintStateCopy(SetConstraintState state);
static SetConstraintState SetConstraintStateAddItem(SetConstraintState state,
						  Oid tgoid, bool tgisdeferred);


/*
 * Gets the current query fdw tuplestore and initializes it if necessary
 */
static Tuplestorestate *
GetCurrentFDWTuplestore()
{
	Tuplestorestate *ret;

	ret = afterTriggers.fdw_tuplestores[afterTriggers.query_depth];
	if (ret == NULL)
	{
		MemoryContext oldcxt;
		ResourceOwner saveResourceOwner;

		/*
		 * Make the tuplestore valid until end of transaction.  This is the
		 * allocation lifespan of the associated events list, but we really
		 * only need it until AfterTriggerEndQuery().
		 */
		oldcxt = MemoryContextSwitchTo(TopTransactionContext);
		saveResourceOwner = CurrentResourceOwner;
		PG_TRY();
		{
			CurrentResourceOwner = TopTransactionResourceOwner;
			ret = tuplestore_begin_heap(false, false, work_mem);
		}
		PG_CATCH();
		{
			CurrentResourceOwner = saveResourceOwner;
			PG_RE_THROW();
		}
		PG_END_TRY();
		CurrentResourceOwner = saveResourceOwner;
		MemoryContextSwitchTo(oldcxt);

		afterTriggers.fdw_tuplestores[afterTriggers.query_depth] = ret;
	}

	return ret;
}

/* ----------
 * afterTriggerCheckState()
 *
 *	Returns true if the trigger event is actually in state DEFERRED.
 * ----------
 */
static bool
afterTriggerCheckState(AfterTriggerShared evtshared)
{
	Oid			tgoid = evtshared->ats_tgoid;
	SetConstraintState state = afterTriggers.state;
	int			i;

	/*
	 * For not-deferrable triggers (i.e. normal AFTER ROW triggers and
	 * constraints declared NOT DEFERRABLE), the state is always false.
	 */
	if ((evtshared->ats_event & AFTER_TRIGGER_DEFERRABLE) == 0)
		return false;

	/*
	 * If constraint state exists, SET CONSTRAINTS might have been executed
	 * either for this trigger or for all triggers.
	 */
	if (state != NULL)
	{
		/* Check for SET CONSTRAINTS for this specific trigger. */
		for (i = 0; i < state->numstates; i++)
		{
			if (state->trigstates[i].sct_tgoid == tgoid)
				return state->trigstates[i].sct_tgisdeferred;
		}

		/* Check for SET CONSTRAINTS ALL. */
		if (state->all_isset)
			return state->all_isdeferred;
	}

	/*
	 * Otherwise return the default state for the trigger.
	 */
	return ((evtshared->ats_event & AFTER_TRIGGER_INITDEFERRED) != 0);
}


/* ----------
 * afterTriggerAddEvent()
 *
 *	Add a new trigger event to the specified queue.
 *	The passed-in event data is copied.
 * ----------
 */
static void
afterTriggerAddEvent(AfterTriggerEventList *events,
					 AfterTriggerEvent event, AfterTriggerShared evtshared)
{
	Size		eventsize = SizeofTriggerEvent(event);
	Size		needed = eventsize + sizeof(AfterTriggerSharedData);
	AfterTriggerEventChunk *chunk;
	AfterTriggerShared newshared;
	AfterTriggerEvent newevent;

	/*
	 * If empty list or not enough room in the tail chunk, make a new chunk.
	 * We assume here that a new shared record will always be needed.
	 */
	chunk = events->tail;
	if (chunk == NULL ||
		chunk->endfree - chunk->freeptr < needed)
	{
		Size		chunksize;

		/* Create event context if we didn't already */
		if (afterTriggers.event_cxt == NULL)
			afterTriggers.event_cxt =
				AllocSetContextCreate(TopTransactionContext,
									  "AfterTriggerEvents",
									  ALLOCSET_DEFAULT_MINSIZE,
									  ALLOCSET_DEFAULT_INITSIZE,
									  ALLOCSET_DEFAULT_MAXSIZE);

		/*
		 * Chunk size starts at 1KB and is allowed to increase up to 1MB.
		 * These numbers are fairly arbitrary, though there is a hard limit at
		 * AFTER_TRIGGER_OFFSET; else we couldn't link event records to their
		 * shared records using the available space in ate_flags.  Another
		 * constraint is that if the chunk size gets too huge, the search loop
		 * below would get slow given a (not too common) usage pattern with
		 * many distinct event types in a chunk.  Therefore, we double the
		 * preceding chunk size only if there weren't too many shared records
		 * in the preceding chunk; otherwise we halve it.  This gives us some
		 * ability to adapt to the actual usage pattern of the current query
		 * while still having large chunk sizes in typical usage.  All chunk
		 * sizes used should be MAXALIGN multiples, to ensure that the shared
		 * records will be aligned safely.
		 */
#define MIN_CHUNK_SIZE 1024
#define MAX_CHUNK_SIZE (1024*1024)

#if MAX_CHUNK_SIZE > (AFTER_TRIGGER_OFFSET+1)
#error MAX_CHUNK_SIZE must not exceed AFTER_TRIGGER_OFFSET
#endif

		if (chunk == NULL)
			chunksize = MIN_CHUNK_SIZE;
		else
		{
			/* preceding chunk size... */
			chunksize = chunk->endptr - (char *) chunk;
			/* check number of shared records in preceding chunk */
			if ((chunk->endptr - chunk->endfree) <=
				(100 * sizeof(AfterTriggerSharedData)))
				chunksize *= 2; /* okay, double it */
			else
				chunksize /= 2; /* too many shared records */
			chunksize = Min(chunksize, MAX_CHUNK_SIZE);
		}
		chunk = MemoryContextAlloc(afterTriggers.event_cxt, chunksize);
		chunk->next = NULL;
		chunk->freeptr = CHUNK_DATA_START(chunk);
		chunk->endptr = chunk->endfree = (char *) chunk + chunksize;
		Assert(chunk->endfree - chunk->freeptr >= needed);

		if (events->head == NULL)
			events->head = chunk;
		else
			events->tail->next = chunk;
		events->tail = chunk;
		/* events->tailfree is now out of sync, but we'll fix it below */
	}

	/*
	 * Try to locate a matching shared-data record already in the chunk. If
	 * none, make a new one.
	 */
	for (newshared = ((AfterTriggerShared) chunk->endptr) - 1;
		 (char *) newshared >= chunk->endfree;
		 newshared--)
	{
		if (newshared->ats_tgoid == evtshared->ats_tgoid &&
			newshared->ats_relid == evtshared->ats_relid &&
			newshared->ats_event == evtshared->ats_event &&
			newshared->ats_firing_id == 0)
			break;
	}
	if ((char *) newshared < chunk->endfree)
	{
		*newshared = *evtshared;
		newshared->ats_firing_id = 0;	/* just to be sure */
		chunk->endfree = (char *) newshared;
	}

	/* Insert the data */
	newevent = (AfterTriggerEvent) chunk->freeptr;
	memcpy(newevent, event, eventsize);
	/* ... and link the new event to its shared record */
	newevent->ate_flags &= ~AFTER_TRIGGER_OFFSET;
	newevent->ate_flags |= (char *) newshared - (char *) newevent;

	chunk->freeptr += eventsize;
	events->tailfree = chunk->freeptr;
}

/* ----------
 * afterTriggerFreeEventList()
 *
 *	Free all the event storage in the given list.
 * ----------
 */
static void
afterTriggerFreeEventList(AfterTriggerEventList *events)
{
	AfterTriggerEventChunk *chunk;
	AfterTriggerEventChunk *next_chunk;

	for (chunk = events->head; chunk != NULL; chunk = next_chunk)
	{
		next_chunk = chunk->next;
		pfree(chunk);
	}
	events->head = NULL;
	events->tail = NULL;
	events->tailfree = NULL;
}

/* ----------
 * afterTriggerRestoreEventList()
 *
 *	Restore an event list to its prior length, removing all the events
 *	added since it had the value old_events.
 * ----------
 */
static void
afterTriggerRestoreEventList(AfterTriggerEventList *events,
							 const AfterTriggerEventList *old_events)
{
	AfterTriggerEventChunk *chunk;
	AfterTriggerEventChunk *next_chunk;

	if (old_events->tail == NULL)
	{
		/* restoring to a completely empty state, so free everything */
		afterTriggerFreeEventList(events);
	}
	else
	{
		*events = *old_events;
		/* free any chunks after the last one we want to keep */
		for (chunk = events->tail->next; chunk != NULL; chunk = next_chunk)
		{
			next_chunk = chunk->next;
			pfree(chunk);
		}
		/* and clean up the tail chunk to be the right length */
		events->tail->next = NULL;
		events->tail->freeptr = events->tailfree;

		/*
		 * We don't make any effort to remove now-unused shared data records.
		 * They might still be useful, anyway.
		 */
	}
}


/* ----------
 * AfterTriggerExecute()
 *
 *	Fetch the required tuples back from the heap and fire one
 *	single trigger function.
 *
 *	Frequently, this will be fired many times in a row for triggers of
 *	a single relation.  Therefore, we cache the open relation and provide
 *	fmgr lookup cache space at the caller level.  (For triggers fired at
 *	the end of a query, we can even piggyback on the executor's state.)
 *
 *	event: event currently being fired.
 *	rel: open relation for event.
 *	trigdesc: working copy of rel's trigger info.
 *	finfo: array of fmgr lookup cache entries (one per trigger in trigdesc).
 *	instr: array of EXPLAIN ANALYZE instrumentation nodes (one per trigger),
 *		or NULL if no instrumentation is wanted.
 *	per_tuple_context: memory context to call trigger function in.
 *	trig_tuple_slot1: scratch slot for tg_trigtuple (foreign tables only)
 *	trig_tuple_slot2: scratch slot for tg_newtuple (foreign tables only)
 * ----------
 */
static void
AfterTriggerExecute(AfterTriggerEvent event,
					Relation rel, TriggerDesc *trigdesc,
					FmgrInfo *finfo, Instrumentation *instr,
					MemoryContext per_tuple_context,
					TupleTableSlot *trig_tuple_slot1,
					TupleTableSlot *trig_tuple_slot2)
{
	AfterTriggerShared evtshared = GetTriggerSharedData(event);
	Oid			tgoid = evtshared->ats_tgoid;
	TriggerData LocTriggerData;
	HeapTupleData tuple1;
	HeapTupleData tuple2;
	HeapTuple	rettuple;
	Buffer		buffer1 = InvalidBuffer;
	Buffer		buffer2 = InvalidBuffer;
	int			tgindx;

	/*
	 * Locate trigger in trigdesc.
	 */
	LocTriggerData.tg_trigger = NULL;
	for (tgindx = 0; tgindx < trigdesc->numtriggers; tgindx++)
	{
		if (trigdesc->triggers[tgindx].tgoid == tgoid)
		{
			LocTriggerData.tg_trigger = &(trigdesc->triggers[tgindx]);
			break;
		}
	}
	if (LocTriggerData.tg_trigger == NULL)
		elog(ERROR, "could not find trigger %u", tgoid);

	/*
	 * If doing EXPLAIN ANALYZE, start charging time to this trigger. We want
	 * to include time spent re-fetching tuples in the trigger cost.
	 */
	if (instr)
		InstrStartNode(instr + tgindx);

	/*
	 * Fetch the required tuple(s).
	 */
	switch (event->ate_flags & AFTER_TRIGGER_TUP_BITS)
	{
		case AFTER_TRIGGER_FDW_FETCH:
			{
				Tuplestorestate *fdw_tuplestore = GetCurrentFDWTuplestore();

				if (!tuplestore_gettupleslot(fdw_tuplestore, true, false,
											 trig_tuple_slot1))
					elog(ERROR, "failed to fetch tuple1 for AFTER trigger");

				if ((evtshared->ats_event & TRIGGER_EVENT_OPMASK) ==
					TRIGGER_EVENT_UPDATE &&
					!tuplestore_gettupleslot(fdw_tuplestore, true, false,
											 trig_tuple_slot2))
					elog(ERROR, "failed to fetch tuple2 for AFTER trigger");
			}
			/* fall through */
		case AFTER_TRIGGER_FDW_REUSE:

			/*
			 * Using ExecMaterializeSlot() rather than ExecFetchSlotTuple()
			 * ensures that tg_trigtuple does not reference tuplestore memory.
			 * (It is formally possible for the trigger function to queue
			 * trigger events that add to the same tuplestore, which can push
			 * other tuples out of memory.)  The distinction is academic,
			 * because we start with a minimal tuple that ExecFetchSlotTuple()
			 * must materialize anyway.
			 */
			LocTriggerData.tg_trigtuple =
				ExecMaterializeSlot(trig_tuple_slot1);
			LocTriggerData.tg_trigtuplebuf = InvalidBuffer;

			LocTriggerData.tg_newtuple =
				((evtshared->ats_event & TRIGGER_EVENT_OPMASK) ==
				 TRIGGER_EVENT_UPDATE) ?
				ExecMaterializeSlot(trig_tuple_slot2) : NULL;
			LocTriggerData.tg_newtuplebuf = InvalidBuffer;

			break;

		default:
			if (ItemPointerIsValid(&(event->ate_ctid1)))
			{
				ItemPointerCopy(&(event->ate_ctid1), &(tuple1.t_self));
				if (!heap_fetch(rel, SnapshotAny, &tuple1, &buffer1, false, NULL))
					elog(ERROR, "failed to fetch tuple1 for AFTER trigger");
				LocTriggerData.tg_trigtuple = &tuple1;
				LocTriggerData.tg_trigtuplebuf = buffer1;
			}
			else
			{
				LocTriggerData.tg_trigtuple = NULL;
				LocTriggerData.tg_trigtuplebuf = InvalidBuffer;
			}

			/* don't touch ctid2 if not there */
			if ((event->ate_flags & AFTER_TRIGGER_TUP_BITS) ==
				AFTER_TRIGGER_2CTID &&
				ItemPointerIsValid(&(event->ate_ctid2)))
			{
				ItemPointerCopy(&(event->ate_ctid2), &(tuple2.t_self));
				if (!heap_fetch(rel, SnapshotAny, &tuple2, &buffer2, false, NULL))
					elog(ERROR, "failed to fetch tuple2 for AFTER trigger");
				LocTriggerData.tg_newtuple = &tuple2;
				LocTriggerData.tg_newtuplebuf = buffer2;
			}
			else
			{
				LocTriggerData.tg_newtuple = NULL;
				LocTriggerData.tg_newtuplebuf = InvalidBuffer;
			}
	}

	/*
	 * Setup the remaining trigger information
	 */
	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event =
		evtshared->ats_event & (TRIGGER_EVENT_OPMASK | TRIGGER_EVENT_ROW);
	LocTriggerData.tg_relation = rel;

	MemoryContextReset(per_tuple_context);

	/*
	 * Call the trigger and throw away any possibly returned updated tuple.
	 * (Don't let ExecCallTriggerFunc measure EXPLAIN time.)
	 */
	rettuple = ExecCallTriggerFunc(&LocTriggerData,
								   tgindx,
								   finfo,
								   NULL,
								   per_tuple_context);
	if (rettuple != NULL &&
		rettuple != LocTriggerData.tg_trigtuple &&
		rettuple != LocTriggerData.tg_newtuple)
		heap_freetuple(rettuple);

	/*
	 * Release buffers
	 */
	if (buffer1 != InvalidBuffer)
		ReleaseBuffer(buffer1);
	if (buffer2 != InvalidBuffer)
		ReleaseBuffer(buffer2);

	/*
	 * If doing EXPLAIN ANALYZE, stop charging time to this trigger, and count
	 * one "tuple returned" (really the number of firings).
	 */
	if (instr)
		InstrStopNode(instr + tgindx, 1);
}


/*
 * afterTriggerMarkEvents()
 *
 *	Scan the given event list for not yet invoked events.  Mark the ones
 *	that can be invoked now with the current firing ID.
 *
 *	If move_list isn't NULL, events that are not to be invoked now are
 *	transferred to move_list.
 *
 *	When immediate_only is TRUE, do not invoke currently-deferred triggers.
 *	(This will be FALSE only at main transaction exit.)
 *
 *	Returns TRUE if any invokable events were found.
 */
static bool
afterTriggerMarkEvents(AfterTriggerEventList *events,
					   AfterTriggerEventList *move_list,
					   bool immediate_only)
{
	bool		found = false;
	AfterTriggerEvent event;
	AfterTriggerEventChunk *chunk;

	for_each_event_chunk(event, chunk, *events)
	{
		AfterTriggerShared evtshared = GetTriggerSharedData(event);
		bool		defer_it = false;

		if (!(event->ate_flags &
			  (AFTER_TRIGGER_DONE | AFTER_TRIGGER_IN_PROGRESS)))
		{
			/*
			 * This trigger hasn't been called or scheduled yet. Check if we
			 * should call it now.
			 */
			if (immediate_only && afterTriggerCheckState(evtshared))
			{
				defer_it = true;
			}
			else
			{
				/*
				 * Mark it as to be fired in this firing cycle.
				 */
				evtshared->ats_firing_id = afterTriggers.firing_counter;
				event->ate_flags |= AFTER_TRIGGER_IN_PROGRESS;
				found = true;
			}
		}

		/*
		 * If it's deferred, move it to move_list, if requested.
		 */
		if (defer_it && move_list != NULL)
		{
			/* add it to move_list */
			afterTriggerAddEvent(move_list, event, evtshared);
			/* mark original copy "done" so we don't do it again */
			event->ate_flags |= AFTER_TRIGGER_DONE;
		}
	}

	return found;
}

/*
 * afterTriggerInvokeEvents()
 *
 *	Scan the given event list for events that are marked as to be fired
 *	in the current firing cycle, and fire them.
 *
 *	If estate isn't NULL, we use its result relation info to avoid repeated
 *	openings and closing of trigger target relations.  If it is NULL, we
 *	make one locally to cache the info in case there are multiple trigger
 *	events per rel.
 *
 *	When delete_ok is TRUE, it's safe to delete fully-processed events.
 *	(We are not very tense about that: we simply reset a chunk to be empty
 *	if all its events got fired.  The objective here is just to avoid useless
 *	rescanning of events when a trigger queues new events during transaction
 *	end, so it's not necessary to worry much about the case where only
 *	some events are fired.)
 *
 *	Returns TRUE if no unfired events remain in the list (this allows us
 *	to avoid repeating afterTriggerMarkEvents).
 */
static bool
afterTriggerInvokeEvents(AfterTriggerEventList *events,
						 CommandId firing_id,
						 EState *estate,
						 bool delete_ok)
{
	bool		all_fired = true;
	AfterTriggerEventChunk *chunk;
	MemoryContext per_tuple_context;
	bool		local_estate = false;
	Relation	rel = NULL;
	TriggerDesc *trigdesc = NULL;
	FmgrInfo   *finfo = NULL;
	Instrumentation *instr = NULL;
	TupleTableSlot *slot1 = NULL,
			   *slot2 = NULL;

	/* Make a local EState if need be */
	if (estate == NULL)
	{
		estate = CreateExecutorState();
		local_estate = true;
	}

	/* Make a per-tuple memory context for trigger function calls */
	per_tuple_context =
		AllocSetContextCreate(CurrentMemoryContext,
							  "AfterTriggerTupleContext",
							  ALLOCSET_DEFAULT_MINSIZE,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);

	for_each_chunk(chunk, *events)
	{
		AfterTriggerEvent event;
		bool		all_fired_in_chunk = true;

		for_each_event(event, chunk)
		{
			AfterTriggerShared evtshared = GetTriggerSharedData(event);

			/*
			 * Is it one for me to fire?
			 */
			if ((event->ate_flags & AFTER_TRIGGER_IN_PROGRESS) &&
				evtshared->ats_firing_id == firing_id)
			{
				/*
				 * So let's fire it... but first, find the correct relation if
				 * this is not the same relation as before.
				 */
				if (rel == NULL || RelationGetRelid(rel) != evtshared->ats_relid)
				{
					ResultRelInfo *rInfo;

					rInfo = ExecGetTriggerResultRel(estate, evtshared->ats_relid);
					rel = rInfo->ri_RelationDesc;
					trigdesc = rInfo->ri_TrigDesc;
					finfo = rInfo->ri_TrigFunctions;
					instr = rInfo->ri_TrigInstrument;
					if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
					{
						if (slot1 != NULL)
						{
							ExecDropSingleTupleTableSlot(slot1);
							ExecDropSingleTupleTableSlot(slot2);
						}
						slot1 = MakeSingleTupleTableSlot(rel->rd_att);
						slot2 = MakeSingleTupleTableSlot(rel->rd_att);
					}
					if (trigdesc == NULL)		/* should not happen */
						elog(ERROR, "relation %u has no triggers",
							 evtshared->ats_relid);
				}

				/*
				 * Fire it.  Note that the AFTER_TRIGGER_IN_PROGRESS flag is
				 * still set, so recursive examinations of the event list
				 * won't try to re-fire it.
				 */
				AfterTriggerExecute(event, rel, trigdesc, finfo, instr,
									per_tuple_context, slot1, slot2);

				/*
				 * Mark the event as done.
				 */
				event->ate_flags &= ~AFTER_TRIGGER_IN_PROGRESS;
				event->ate_flags |= AFTER_TRIGGER_DONE;
			}
			else if (!(event->ate_flags & AFTER_TRIGGER_DONE))
			{
				/* something remains to be done */
				all_fired = all_fired_in_chunk = false;
			}
		}

		/* Clear the chunk if delete_ok and nothing left of interest */
		if (delete_ok && all_fired_in_chunk)
		{
			chunk->freeptr = CHUNK_DATA_START(chunk);
			chunk->endfree = chunk->endptr;

			/*
			 * If it's last chunk, must sync event list's tailfree too.  Note
			 * that delete_ok must NOT be passed as true if there could be
			 * stacked AfterTriggerEventList values pointing at this event
			 * list, since we'd fail to fix their copies of tailfree.
			 */
			if (chunk == events->tail)
				events->tailfree = chunk->freeptr;
		}
	}
	if (slot1 != NULL)
	{
		ExecDropSingleTupleTableSlot(slot1);
		ExecDropSingleTupleTableSlot(slot2);
	}

	/* Release working resources */
	MemoryContextDelete(per_tuple_context);

	if (local_estate)
	{
		ListCell   *l;

		foreach(l, estate->es_trig_target_relations)
		{
			ResultRelInfo *resultRelInfo = (ResultRelInfo *) lfirst(l);

			/* Close indices and then the relation itself */
			ExecCloseIndices(resultRelInfo);
			heap_close(resultRelInfo->ri_RelationDesc, NoLock);
		}
		FreeExecutorState(estate);
	}

	return all_fired;
}


/* ----------
 * AfterTriggerBeginXact()
 *
 *	Called at transaction start (either BEGIN or implicit for single
 *	statement outside of transaction block).
 * ----------
 */
void
AfterTriggerBeginXact(void)
{
	/*
	 * Initialize after-trigger state structure to empty
	 */
	afterTriggers.firing_counter = (CommandId) 1;		/* mustn't be 0 */
	afterTriggers.query_depth = -1;

	/*
	 * Verify that there is no leftover state remaining.  If these assertions
	 * trip, it means that AfterTriggerEndXact wasn't called or didn't clean
	 * up properly.
	 */
	Assert(afterTriggers.state == NULL);
	Assert(afterTriggers.query_stack == NULL);
	Assert(afterTriggers.fdw_tuplestores == NULL);
	Assert(afterTriggers.maxquerydepth == 0);
	Assert(afterTriggers.event_cxt == NULL);
	Assert(afterTriggers.events.head == NULL);
	Assert(afterTriggers.state_stack == NULL);
	Assert(afterTriggers.events_stack == NULL);
	Assert(afterTriggers.depth_stack == NULL);
	Assert(afterTriggers.firing_stack == NULL);
	Assert(afterTriggers.maxtransdepth == 0);
}


/* ----------
 * AfterTriggerBeginQuery()
 *
 *	Called just before we start processing a single query within a
 *	transaction (or subtransaction).  Most of the real work gets deferred
 *	until somebody actually tries to queue a trigger event.
 * ----------
 */
void
AfterTriggerBeginQuery(void)
{
	/* Increase the query stack depth */
	afterTriggers.query_depth++;
}


/* ----------
 * AfterTriggerEndQuery()
 *
 *	Called after one query has been completely processed. At this time
 *	we invoke all AFTER IMMEDIATE trigger events queued by the query, and
 *	transfer deferred trigger events to the global deferred-trigger list.
 *
 *	Note that this must be called BEFORE closing down the executor
 *	with ExecutorEnd, because we make use of the EState's info about
 *	target relations.  Normally it is called from ExecutorFinish.
 * ----------
 */
void
AfterTriggerEndQuery(EState *estate)
{
	AfterTriggerEventList *events;
	Tuplestorestate *fdw_tuplestore;

	/* Must be inside a query, too */
	Assert(afterTriggers.query_depth >= 0);

	/*
	 * If we never even got as far as initializing the event stack, there
	 * certainly won't be any events, so exit quickly.
	 */
	if (afterTriggers.query_depth >= afterTriggers.maxquerydepth)
	{
		afterTriggers.query_depth--;
		return;
	}

	/*
	 * Process all immediate-mode triggers queued by the query, and move the
	 * deferred ones to the main list of deferred events.
	 *
	 * Notice that we decide which ones will be fired, and put the deferred
	 * ones on the main list, before anything is actually fired.  This ensures
	 * reasonably sane behavior if a trigger function does SET CONSTRAINTS ...
	 * IMMEDIATE: all events we have decided to defer will be available for it
	 * to fire.
	 *
	 * We loop in case a trigger queues more events at the same query level.
	 * Ordinary trigger functions, including all PL/pgSQL trigger functions,
	 * will instead fire any triggers in a dedicated query level.  Foreign key
	 * enforcement triggers do add to the current query level, thanks to their
	 * passing fire_triggers = false to SPI_execute_snapshot().  Other
	 * C-language triggers might do likewise.  Be careful here: firing a
	 * trigger could result in query_stack being repalloc'd, so we can't save
	 * its address across afterTriggerInvokeEvents calls.
	 *
	 * If we find no firable events, we don't have to increment
	 * firing_counter.
	 */
	for (;;)
	{
		events = &afterTriggers.query_stack[afterTriggers.query_depth];
		if (afterTriggerMarkEvents(events, &afterTriggers.events, true))
		{
			CommandId	firing_id = afterTriggers.firing_counter++;

			/* OK to delete the immediate events after processing them */
			if (afterTriggerInvokeEvents(events, firing_id, estate, true))
				break;			/* all fired */
		}
		else
			break;
	}

	/* Release query-local storage for events, including tuplestore if any */
	fdw_tuplestore = afterTriggers.fdw_tuplestores[afterTriggers.query_depth];
	if (fdw_tuplestore)
	{
		tuplestore_end(fdw_tuplestore);
		afterTriggers.fdw_tuplestores[afterTriggers.query_depth] = NULL;
	}
	afterTriggerFreeEventList(&afterTriggers.query_stack[afterTriggers.query_depth]);

	afterTriggers.query_depth--;
}


/* ----------
 * AfterTriggerFireDeferred()
 *
 *	Called just before the current transaction is committed. At this
 *	time we invoke all pending DEFERRED triggers.
 *
 *	It is possible for other modules to queue additional deferred triggers
 *	during pre-commit processing; therefore xact.c may have to call this
 *	multiple times.
 * ----------
 */
void
AfterTriggerFireDeferred(void)
{
	AfterTriggerEventList *events;
	bool		snap_pushed = false;

	/* Must not be inside a query */
	Assert(afterTriggers.query_depth == -1);

	/*
	 * If there are any triggers to fire, make sure we have set a snapshot for
	 * them to use.  (Since PortalRunUtility doesn't set a snap for COMMIT, we
	 * can't assume ActiveSnapshot is valid on entry.)
	 */
	events = &afterTriggers.events;
	if (events->head != NULL)
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		snap_pushed = true;
	}

	/*
	 * Run all the remaining triggers.  Loop until they are all gone, in case
	 * some trigger queues more for us to do.
	 */
	while (afterTriggerMarkEvents(events, NULL, false))
	{
		CommandId	firing_id = afterTriggers.firing_counter++;

		if (afterTriggerInvokeEvents(events, firing_id, NULL, true))
			break;				/* all fired */
	}

	/*
	 * We don't bother freeing the event list, since it will go away anyway
	 * (and more efficiently than via pfree) in AfterTriggerEndXact.
	 */

	if (snap_pushed)
		PopActiveSnapshot();
}


/* ----------
 * AfterTriggerEndXact()
 *
 *	The current transaction is finishing.
 *
 *	Any unfired triggers are canceled so we simply throw
 *	away anything we know.
 *
 *	Note: it is possible for this to be called repeatedly in case of
 *	error during transaction abort; therefore, do not complain if
 *	already closed down.
 * ----------
 */
void
AfterTriggerEndXact(bool isCommit)
{
	/*
	 * Forget the pending-events list.
	 *
	 * Since all the info is in TopTransactionContext or children thereof, we
	 * don't really need to do anything to reclaim memory.  However, the
	 * pending-events list could be large, and so it's useful to discard it as
	 * soon as possible --- especially if we are aborting because we ran out
	 * of memory for the list!
	 */
	if (afterTriggers.event_cxt)
	{
		MemoryContextDelete(afterTriggers.event_cxt);
		afterTriggers.event_cxt = NULL;
		afterTriggers.events.head = NULL;
		afterTriggers.events.tail = NULL;
		afterTriggers.events.tailfree = NULL;
	}

	/*
	 * Forget any subtransaction state as well.  Since this can't be very
	 * large, we let the eventual reset of TopTransactionContext free the
	 * memory instead of doing it here.
	 */
	afterTriggers.state_stack = NULL;
	afterTriggers.events_stack = NULL;
	afterTriggers.depth_stack = NULL;
	afterTriggers.firing_stack = NULL;
	afterTriggers.maxtransdepth = 0;


	/*
	 * Forget the query stack and constrant-related state information.  As
	 * with the subtransaction state information, we don't bother freeing the
	 * memory here.
	 */
	afterTriggers.query_stack = NULL;
	afterTriggers.fdw_tuplestores = NULL;
	afterTriggers.maxquerydepth = 0;
	afterTriggers.state = NULL;

	/* No more afterTriggers manipulation until next transaction starts. */
	afterTriggers.query_depth = -1;
}

/*
 * AfterTriggerBeginSubXact()
 *
 *	Start a subtransaction.
 */
void
AfterTriggerBeginSubXact(void)
{
	int			my_level = GetCurrentTransactionNestLevel();

	/*
	 * Allocate more space in the stacks if needed.  (Note: because the
	 * minimum nest level of a subtransaction is 2, we waste the first couple
	 * entries of each array; not worth the notational effort to avoid it.)
	 */
	while (my_level >= afterTriggers.maxtransdepth)
	{
		if (afterTriggers.maxtransdepth == 0)
		{
			MemoryContext old_cxt;

			old_cxt = MemoryContextSwitchTo(TopTransactionContext);

#define DEFTRIG_INITALLOC 8
			afterTriggers.state_stack = (SetConstraintState *)
				palloc(DEFTRIG_INITALLOC * sizeof(SetConstraintState));
			afterTriggers.events_stack = (AfterTriggerEventList *)
				palloc(DEFTRIG_INITALLOC * sizeof(AfterTriggerEventList));
			afterTriggers.depth_stack = (int *)
				palloc(DEFTRIG_INITALLOC * sizeof(int));
			afterTriggers.firing_stack = (CommandId *)
				palloc(DEFTRIG_INITALLOC * sizeof(CommandId));
			afterTriggers.maxtransdepth = DEFTRIG_INITALLOC;

			MemoryContextSwitchTo(old_cxt);
		}
		else
		{
			/* repalloc will keep the stacks in the same context */
			int			new_alloc = afterTriggers.maxtransdepth * 2;

			afterTriggers.state_stack = (SetConstraintState *)
				repalloc(afterTriggers.state_stack,
						 new_alloc * sizeof(SetConstraintState));
			afterTriggers.events_stack = (AfterTriggerEventList *)
				repalloc(afterTriggers.events_stack,
						 new_alloc * sizeof(AfterTriggerEventList));
			afterTriggers.depth_stack = (int *)
				repalloc(afterTriggers.depth_stack,
						 new_alloc * sizeof(int));
			afterTriggers.firing_stack = (CommandId *)
				repalloc(afterTriggers.firing_stack,
						 new_alloc * sizeof(CommandId));
			afterTriggers.maxtransdepth = new_alloc;
		}
	}

	/*
	 * Push the current information into the stack.  The SET CONSTRAINTS state
	 * is not saved until/unless changed.  Likewise, we don't make a
	 * per-subtransaction event context until needed.
	 */
	afterTriggers.state_stack[my_level] = NULL;
	afterTriggers.events_stack[my_level] = afterTriggers.events;
	afterTriggers.depth_stack[my_level] = afterTriggers.query_depth;
	afterTriggers.firing_stack[my_level] = afterTriggers.firing_counter;
}

/*
 * AfterTriggerEndSubXact()
 *
 *	The current subtransaction is ending.
 */
void
AfterTriggerEndSubXact(bool isCommit)
{
	int			my_level = GetCurrentTransactionNestLevel();
	SetConstraintState state;
	AfterTriggerEvent event;
	AfterTriggerEventChunk *chunk;
	CommandId	subxact_firing_id;

	/*
	 * Pop the prior state if needed.
	 */
	if (isCommit)
	{
		Assert(my_level < afterTriggers.maxtransdepth);
		/* If we saved a prior state, we don't need it anymore */
		state = afterTriggers.state_stack[my_level];
		if (state != NULL)
			pfree(state);
		/* this avoids double pfree if error later: */
		afterTriggers.state_stack[my_level] = NULL;
		Assert(afterTriggers.query_depth ==
			   afterTriggers.depth_stack[my_level]);
	}
	else
	{
		/*
		 * Aborting.  It is possible subxact start failed before calling
		 * AfterTriggerBeginSubXact, in which case we mustn't risk touching
		 * stack levels that aren't there.
		 */
		if (my_level >= afterTriggers.maxtransdepth)
			return;

		/*
		 * Release any event lists from queries being aborted, and restore
		 * query_depth to its pre-subxact value.  This assumes that a
		 * subtransaction will not add events to query levels started in a
		 * earlier transaction state.
		 */
		while (afterTriggers.query_depth > afterTriggers.depth_stack[my_level])
		{
			if (afterTriggers.query_depth < afterTriggers.maxquerydepth)
			{
				Tuplestorestate *ts;

				ts = afterTriggers.fdw_tuplestores[afterTriggers.query_depth];
				if (ts)
				{
					tuplestore_end(ts);
					afterTriggers.fdw_tuplestores[afterTriggers.query_depth] = NULL;
				}

				afterTriggerFreeEventList(&afterTriggers.query_stack[afterTriggers.query_depth]);
			}

			afterTriggers.query_depth--;
		}
		Assert(afterTriggers.query_depth ==
			   afterTriggers.depth_stack[my_level]);

		/*
		 * Restore the global deferred-event list to its former length,
		 * discarding any events queued by the subxact.
		 */
		afterTriggerRestoreEventList(&afterTriggers.events,
									 &afterTriggers.events_stack[my_level]);

		/*
		 * Restore the trigger state.  If the saved state is NULL, then this
		 * subxact didn't save it, so it doesn't need restoring.
		 */
		state = afterTriggers.state_stack[my_level];
		if (state != NULL)
		{
			pfree(afterTriggers.state);
			afterTriggers.state = state;
		}
		/* this avoids double pfree if error later: */
		afterTriggers.state_stack[my_level] = NULL;

		/*
		 * Scan for any remaining deferred events that were marked DONE or IN
		 * PROGRESS by this subxact or a child, and un-mark them. We can
		 * recognize such events because they have a firing ID greater than or
		 * equal to the firing_counter value we saved at subtransaction start.
		 * (This essentially assumes that the current subxact includes all
		 * subxacts started after it.)
		 */
		subxact_firing_id = afterTriggers.firing_stack[my_level];
		for_each_event_chunk(event, chunk, afterTriggers.events)
		{
			AfterTriggerShared evtshared = GetTriggerSharedData(event);

			if (event->ate_flags &
				(AFTER_TRIGGER_DONE | AFTER_TRIGGER_IN_PROGRESS))
			{
				if (evtshared->ats_firing_id >= subxact_firing_id)
					event->ate_flags &=
						~(AFTER_TRIGGER_DONE | AFTER_TRIGGER_IN_PROGRESS);
			}
		}
	}
}

/* ----------
 * AfterTriggerEnlargeQueryState()
 *
 *	Prepare the necessary state so that we can record AFTER trigger events
 *	queued by a query.  It is allowed to have nested queries within a
 *	(sub)transaction, so we need to have separate state for each query
 *	nesting level.
 * ----------
 */
static void
AfterTriggerEnlargeQueryState(void)
{
	int		init_depth = afterTriggers.maxquerydepth;

	Assert(afterTriggers.query_depth >= afterTriggers.maxquerydepth);

	if (afterTriggers.maxquerydepth == 0)
	{
		int			new_alloc = Max(afterTriggers.query_depth + 1, 8);

		afterTriggers.query_stack = (AfterTriggerEventList *)
			MemoryContextAlloc(TopTransactionContext,
							   new_alloc * sizeof(AfterTriggerEventList));
		afterTriggers.fdw_tuplestores = (Tuplestorestate **)
			MemoryContextAllocZero(TopTransactionContext,
								   new_alloc * sizeof(Tuplestorestate *));
		afterTriggers.maxquerydepth = new_alloc;
	}
	else
	{
		/* repalloc will keep the stack in the same context */
		int			old_alloc = afterTriggers.maxquerydepth;
		int			new_alloc = Max(afterTriggers.query_depth + 1,
									old_alloc * 2);

		afterTriggers.query_stack = (AfterTriggerEventList *)
			repalloc(afterTriggers.query_stack,
					 new_alloc * sizeof(AfterTriggerEventList));
		afterTriggers.fdw_tuplestores = (Tuplestorestate **)
			repalloc(afterTriggers.fdw_tuplestores,
					 new_alloc * sizeof(Tuplestorestate *));
		/* Clear newly-allocated slots for subsequent lazy initialization. */
		memset(afterTriggers.fdw_tuplestores + old_alloc,
			   0, (new_alloc - old_alloc) * sizeof(Tuplestorestate *));
		afterTriggers.maxquerydepth = new_alloc;
	}

	/* Initialize new query lists to empty */
	while (init_depth < afterTriggers.maxquerydepth)
	{
		AfterTriggerEventList *events;

		events = &afterTriggers.query_stack[init_depth];
		events->head = NULL;
		events->tail = NULL;
		events->tailfree = NULL;

		++init_depth;
	}
}

/*
 * Create an empty SetConstraintState with room for numalloc trigstates
 */
static SetConstraintState
SetConstraintStateCreate(int numalloc)
{
	SetConstraintState state;

	/* Behave sanely with numalloc == 0 */
	if (numalloc <= 0)
		numalloc = 1;

	/*
	 * We assume that zeroing will correctly initialize the state values.
	 */
	state = (SetConstraintState)
		MemoryContextAllocZero(TopTransactionContext,
							   sizeof(SetConstraintStateData) +
						   (numalloc - 1) *sizeof(SetConstraintTriggerData));

	state->numalloc = numalloc;

	return state;
}

/*
 * Copy a SetConstraintState
 */
static SetConstraintState
SetConstraintStateCopy(SetConstraintState origstate)
{
	SetConstraintState state;

	state = SetConstraintStateCreate(origstate->numstates);

	state->all_isset = origstate->all_isset;
	state->all_isdeferred = origstate->all_isdeferred;
	state->numstates = origstate->numstates;
	memcpy(state->trigstates, origstate->trigstates,
		   origstate->numstates * sizeof(SetConstraintTriggerData));

	return state;
}

/*
 * Add a per-trigger item to a SetConstraintState.  Returns possibly-changed
 * pointer to the state object (it will change if we have to repalloc).
 */
static SetConstraintState
SetConstraintStateAddItem(SetConstraintState state,
						  Oid tgoid, bool tgisdeferred)
{
	if (state->numstates >= state->numalloc)
	{
		int			newalloc = state->numalloc * 2;

		newalloc = Max(newalloc, 8);	/* in case original has size 0 */
		state = (SetConstraintState)
			repalloc(state,
					 sizeof(SetConstraintStateData) +
					 (newalloc - 1) *sizeof(SetConstraintTriggerData));
		state->numalloc = newalloc;
		Assert(state->numstates < state->numalloc);
	}

	state->trigstates[state->numstates].sct_tgoid = tgoid;
	state->trigstates[state->numstates].sct_tgisdeferred = tgisdeferred;
	state->numstates++;

	return state;
}

/* ----------
 * AfterTriggerSetState()
 *
 *	Execute the SET CONSTRAINTS ... utility command.
 * ----------
 */
void
AfterTriggerSetState(ConstraintsSetStmt *stmt)
{
	int			my_level = GetCurrentTransactionNestLevel();

	/* If we haven't already done so, initialize our state. */
	if (afterTriggers.state == NULL)
		afterTriggers.state = SetConstraintStateCreate(8);

	/*
	 * If in a subtransaction, and we didn't save the current state already,
	 * save it so it can be restored if the subtransaction aborts.
	 */
	if (my_level > 1 &&
		afterTriggers.state_stack[my_level] == NULL)
	{
		afterTriggers.state_stack[my_level] =
			SetConstraintStateCopy(afterTriggers.state);
	}

	/*
	 * Handle SET CONSTRAINTS ALL ...
	 */
	if (stmt->constraints == NIL)
	{
		/*
		 * Forget any previous SET CONSTRAINTS commands in this transaction.
		 */
		afterTriggers.state->numstates = 0;

		/*
		 * Set the per-transaction ALL state to known.
		 */
		afterTriggers.state->all_isset = true;
		afterTriggers.state->all_isdeferred = stmt->deferred;
	}
	else
	{
		Relation	conrel;
		Relation	tgrel;
		List	   *conoidlist = NIL;
		List	   *tgoidlist = NIL;
		ListCell   *lc;

		/*
		 * Handle SET CONSTRAINTS constraint-name [, ...]
		 *
		 * First, identify all the named constraints and make a list of their
		 * OIDs.  Since, unlike the SQL spec, we allow multiple constraints of
		 * the same name within a schema, the specifications are not
		 * necessarily unique.  Our strategy is to target all matching
		 * constraints within the first search-path schema that has any
		 * matches, but disregard matches in schemas beyond the first match.
		 * (This is a bit odd but it's the historical behavior.)
		 */
		conrel = heap_open(ConstraintRelationId, AccessShareLock);

		foreach(lc, stmt->constraints)
		{
			RangeVar   *constraint = lfirst(lc);
			bool		found;
			List	   *namespacelist;
			ListCell   *nslc;

			if (constraint->catalogname)
			{
				if (strcmp(constraint->catalogname, get_database_name(MyDatabaseId)) != 0)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cross-database references are not implemented: \"%s.%s.%s\"",
							 constraint->catalogname, constraint->schemaname,
									constraint->relname)));
			}

			/*
			 * If we're given the schema name with the constraint, look only
			 * in that schema.  If given a bare constraint name, use the
			 * search path to find the first matching constraint.
			 */
			if (constraint->schemaname)
			{
				Oid			namespaceId = LookupExplicitNamespace(constraint->schemaname,
																  false);

				namespacelist = list_make1_oid(namespaceId);
			}
			else
			{
				namespacelist = fetch_search_path(true);
			}

			found = false;
			foreach(nslc, namespacelist)
			{
				Oid			namespaceId = lfirst_oid(nslc);
				SysScanDesc conscan;
				ScanKeyData skey[2];
				HeapTuple	tup;

				ScanKeyInit(&skey[0],
							Anum_pg_constraint_conname,
							BTEqualStrategyNumber, F_NAMEEQ,
							CStringGetDatum(constraint->relname));
				ScanKeyInit(&skey[1],
							Anum_pg_constraint_connamespace,
							BTEqualStrategyNumber, F_OIDEQ,
							ObjectIdGetDatum(namespaceId));

				conscan = systable_beginscan(conrel, ConstraintNameNspIndexId,
											 true, NULL, 2, skey);

				while (HeapTupleIsValid(tup = systable_getnext(conscan)))
				{
					Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tup);

					if (con->condeferrable)
						conoidlist = lappend_oid(conoidlist,
												 HeapTupleGetOid(tup));
					else if (stmt->deferred)
						ereport(ERROR,
								(errcode(ERRCODE_WRONG_OBJECT_TYPE),
								 errmsg("constraint \"%s\" is not deferrable",
										constraint->relname)));
					found = true;
				}

				systable_endscan(conscan);

				/*
				 * Once we've found a matching constraint we do not search
				 * later parts of the search path.
				 */
				if (found)
					break;
			}

			list_free(namespacelist);

			/*
			 * Not found ?
			 */
			if (!found)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("constraint \"%s\" does not exist",
								constraint->relname)));
		}

		heap_close(conrel, AccessShareLock);

		/*
		 * Now, locate the trigger(s) implementing each of these constraints,
		 * and make a list of their OIDs.
		 */
		tgrel = heap_open(TriggerRelationId, AccessShareLock);

		foreach(lc, conoidlist)
		{
			Oid			conoid = lfirst_oid(lc);
			bool		found;
			ScanKeyData skey;
			SysScanDesc tgscan;
			HeapTuple	htup;

			found = false;

			ScanKeyInit(&skey,
						Anum_pg_trigger_tgconstraint,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(conoid));

			tgscan = systable_beginscan(tgrel, TriggerConstraintIndexId, true,
										NULL, 1, &skey);

			while (HeapTupleIsValid(htup = systable_getnext(tgscan)))
			{
				Form_pg_trigger pg_trigger = (Form_pg_trigger) GETSTRUCT(htup);

				/*
				 * Silently skip triggers that are marked as non-deferrable in
				 * pg_trigger.  This is not an error condition, since a
				 * deferrable RI constraint may have some non-deferrable
				 * actions.
				 */
				if (pg_trigger->tgdeferrable)
					tgoidlist = lappend_oid(tgoidlist,
											HeapTupleGetOid(htup));

				found = true;
			}

			systable_endscan(tgscan);

			/* Safety check: a deferrable constraint should have triggers */
			if (!found)
				elog(ERROR, "no triggers found for constraint with OID %u",
					 conoid);
		}

		heap_close(tgrel, AccessShareLock);

		/*
		 * Now we can set the trigger states of individual triggers for this
		 * xact.
		 */
		foreach(lc, tgoidlist)
		{
			Oid			tgoid = lfirst_oid(lc);
			SetConstraintState state = afterTriggers.state;
			bool		found = false;
			int			i;

			for (i = 0; i < state->numstates; i++)
			{
				if (state->trigstates[i].sct_tgoid == tgoid)
				{
					state->trigstates[i].sct_tgisdeferred = stmt->deferred;
					found = true;
					break;
				}
			}
			if (!found)
			{
				afterTriggers.state =
					SetConstraintStateAddItem(state, tgoid, stmt->deferred);
			}
		}
	}

	/*
	 * SQL99 requires that when a constraint is set to IMMEDIATE, any deferred
	 * checks against that constraint must be made when the SET CONSTRAINTS
	 * command is executed -- i.e. the effects of the SET CONSTRAINTS command
	 * apply retroactively.  We've updated the constraints state, so scan the
	 * list of previously deferred events to fire any that have now become
	 * immediate.
	 *
	 * Obviously, if this was SET ... DEFERRED then it can't have converted
	 * any unfired events to immediate, so we need do nothing in that case.
	 */
	if (!stmt->deferred)
	{
		AfterTriggerEventList *events = &afterTriggers.events;
		bool		snapshot_set = false;

		while (afterTriggerMarkEvents(events, NULL, true))
		{
			CommandId	firing_id = afterTriggers.firing_counter++;

			/*
			 * Make sure a snapshot has been established in case trigger
			 * functions need one.  Note that we avoid setting a snapshot if
			 * we don't find at least one trigger that has to be fired now.
			 * This is so that BEGIN; SET CONSTRAINTS ...; SET TRANSACTION
			 * ISOLATION LEVEL SERIALIZABLE; ... works properly.  (If we are
			 * at the start of a transaction it's not possible for any trigger
			 * events to be queued yet.)
			 */
			if (!snapshot_set)
			{
				PushActiveSnapshot(GetTransactionSnapshot());
				snapshot_set = true;
			}

			/*
			 * We can delete fired events if we are at top transaction level,
			 * but we'd better not if inside a subtransaction, since the
			 * subtransaction could later get rolled back.
			 */
			if (afterTriggerInvokeEvents(events, firing_id, NULL,
										 !IsSubTransaction()))
				break;			/* all fired */
		}

		if (snapshot_set)
			PopActiveSnapshot();
	}
}

/* ----------
 * AfterTriggerPendingOnRel()
 *		Test to see if there are any pending after-trigger events for rel.
 *
 * This is used by TRUNCATE, CLUSTER, ALTER TABLE, etc to detect whether
 * it is unsafe to perform major surgery on a relation.  Note that only
 * local pending events are examined.  We assume that having exclusive lock
 * on a rel guarantees there are no unserviced events in other backends ---
 * but having a lock does not prevent there being such events in our own.
 *
 * In some scenarios it'd be reasonable to remove pending events (more
 * specifically, mark them DONE by the current subxact) but without a lot
 * of knowledge of the trigger semantics we can't do this in general.
 * ----------
 */
bool
AfterTriggerPendingOnRel(Oid relid)
{
	AfterTriggerEvent event;
	AfterTriggerEventChunk *chunk;
	int			depth;

	/* Scan queued events */
	for_each_event_chunk(event, chunk, afterTriggers.events)
	{
		AfterTriggerShared evtshared = GetTriggerSharedData(event);

		/*
		 * We can ignore completed events.  (Even if a DONE flag is rolled
		 * back by subxact abort, it's OK because the effects of the TRUNCATE
		 * or whatever must get rolled back too.)
		 */
		if (event->ate_flags & AFTER_TRIGGER_DONE)
			continue;

		if (evtshared->ats_relid == relid)
			return true;
	}

	/*
	 * Also scan events queued by incomplete queries.  This could only matter
	 * if TRUNCATE/etc is executed by a function or trigger within an updating
	 * query on the same relation, which is pretty perverse, but let's check.
	 */
	for (depth = 0; depth <= afterTriggers.query_depth; depth++)
	{
		for_each_event_chunk(event, chunk, afterTriggers.query_stack[depth])
		{
			AfterTriggerShared evtshared = GetTriggerSharedData(event);

			if (event->ate_flags & AFTER_TRIGGER_DONE)
				continue;

			if (evtshared->ats_relid == relid)
				return true;
		}
	}

	return false;
}


/* ----------
 * AfterTriggerSaveEvent()
 *
 *	Called by ExecA[RS]...Triggers() to queue up the triggers that should
 *	be fired for an event.
 *
 *	NOTE: this is called whenever there are any triggers associated with
 *	the event (even if they are disabled).  This function decides which
 *	triggers actually need to be queued.
 * ----------
 */
static void
AfterTriggerSaveEvent(EState *estate, ResultRelInfo *relinfo,
					  int event, bool row_trigger,
					  HeapTuple oldtup, HeapTuple newtup,
					  List *recheckIndexes, Bitmapset *modifiedCols)
{
	Relation	rel = relinfo->ri_RelationDesc;
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;
	AfterTriggerEventData new_event;
	AfterTriggerSharedData new_shared;
	char		relkind = relinfo->ri_RelationDesc->rd_rel->relkind;
	int			tgtype_event;
	int			tgtype_level;
	int			i;
	Tuplestorestate *fdw_tuplestore = NULL;

	/*
	 * Check state.  We use a normal test not Assert because it is possible to
	 * reach here in the wrong state given misconfigured RI triggers, in
	 * particular deferring a cascade action trigger.
	 */
	if (afterTriggers.query_depth < 0)
		elog(ERROR, "AfterTriggerSaveEvent() called outside of query");

	/* Be sure we have enough space to record events at this query depth. */
	if (afterTriggers.query_depth >= afterTriggers.maxquerydepth)
		AfterTriggerEnlargeQueryState();

	/*
	 * Validate the event code and collect the associated tuple CTIDs.
	 *
	 * The event code will be used both as a bitmask and an array offset, so
	 * validation is important to make sure we don't walk off the edge of our
	 * arrays.
	 */
	switch (event)
	{
		case TRIGGER_EVENT_INSERT:
			tgtype_event = TRIGGER_TYPE_INSERT;
			if (row_trigger)
			{
				Assert(oldtup == NULL);
				Assert(newtup != NULL);
				ItemPointerCopy(&(newtup->t_self), &(new_event.ate_ctid1));
				ItemPointerSetInvalid(&(new_event.ate_ctid2));
			}
			else
			{
				Assert(oldtup == NULL);
				Assert(newtup == NULL);
				ItemPointerSetInvalid(&(new_event.ate_ctid1));
				ItemPointerSetInvalid(&(new_event.ate_ctid2));
			}
			break;
		case TRIGGER_EVENT_DELETE:
			tgtype_event = TRIGGER_TYPE_DELETE;
			if (row_trigger)
			{
				Assert(oldtup != NULL);
				Assert(newtup == NULL);
				ItemPointerCopy(&(oldtup->t_self), &(new_event.ate_ctid1));
				ItemPointerSetInvalid(&(new_event.ate_ctid2));
			}
			else
			{
				Assert(oldtup == NULL);
				Assert(newtup == NULL);
				ItemPointerSetInvalid(&(new_event.ate_ctid1));
				ItemPointerSetInvalid(&(new_event.ate_ctid2));
			}
			break;
		case TRIGGER_EVENT_UPDATE:
			tgtype_event = TRIGGER_TYPE_UPDATE;
			if (row_trigger)
			{
				Assert(oldtup != NULL);
				Assert(newtup != NULL);
				ItemPointerCopy(&(oldtup->t_self), &(new_event.ate_ctid1));
				ItemPointerCopy(&(newtup->t_self), &(new_event.ate_ctid2));
			}
			else
			{
				Assert(oldtup == NULL);
				Assert(newtup == NULL);
				ItemPointerSetInvalid(&(new_event.ate_ctid1));
				ItemPointerSetInvalid(&(new_event.ate_ctid2));
			}
			break;
		case TRIGGER_EVENT_TRUNCATE:
			tgtype_event = TRIGGER_TYPE_TRUNCATE;
			Assert(oldtup == NULL);
			Assert(newtup == NULL);
			ItemPointerSetInvalid(&(new_event.ate_ctid1));
			ItemPointerSetInvalid(&(new_event.ate_ctid2));
			break;
		default:
			elog(ERROR, "invalid after-trigger event code: %d", event);
			tgtype_event = 0;	/* keep compiler quiet */
			break;
	}

	if (!(relkind == RELKIND_FOREIGN_TABLE && row_trigger))
		new_event.ate_flags = (row_trigger && event == TRIGGER_EVENT_UPDATE) ?
			AFTER_TRIGGER_2CTID : AFTER_TRIGGER_1CTID;
	/* else, we'll initialize ate_flags for each trigger */

	tgtype_level = (row_trigger ? TRIGGER_TYPE_ROW : TRIGGER_TYPE_STATEMENT);

	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  tgtype_level,
								  TRIGGER_TYPE_AFTER,
								  tgtype_event))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, event,
							modifiedCols, oldtup, newtup))
			continue;

		if (relkind == RELKIND_FOREIGN_TABLE && row_trigger)
		{
			if (fdw_tuplestore == NULL)
			{
				fdw_tuplestore = GetCurrentFDWTuplestore();
				new_event.ate_flags = AFTER_TRIGGER_FDW_FETCH;
			}
			else
				/* subsequent event for the same tuple */
				new_event.ate_flags = AFTER_TRIGGER_FDW_REUSE;
		}

		/*
		 * If the trigger is a foreign key enforcement trigger, there are
		 * certain cases where we can skip queueing the event because we can
		 * tell by inspection that the FK constraint will still pass.
		 */
		if (TRIGGER_FIRED_BY_UPDATE(event))
		{
			switch (RI_FKey_trigger_type(trigger->tgfoid))
			{
				case RI_TRIGGER_PK:
					/* Update on trigger's PK table */
					if (!RI_FKey_pk_upd_check_required(trigger, rel,
													   oldtup, newtup))
					{
						/* skip queuing this event */
						continue;
					}
					break;

				case RI_TRIGGER_FK:
					/* Update on trigger's FK table */
					if (!RI_FKey_fk_upd_check_required(trigger, rel,
													   oldtup, newtup))
					{
						/* skip queuing this event */
						continue;
					}
					break;

				case RI_TRIGGER_NONE:
					/* Not an FK trigger */
					break;
			}
		}

		/*
		 * If the trigger is a deferred unique constraint check trigger, only
		 * queue it if the unique constraint was potentially violated, which
		 * we know from index insertion time.
		 */
		if (trigger->tgfoid == F_UNIQUE_KEY_RECHECK)
		{
			if (!list_member_oid(recheckIndexes, trigger->tgconstrindid))
				continue;		/* Uniqueness definitely not violated */
		}

		/*
		 * Fill in event structure and add it to the current query's queue.
		 */
		new_shared.ats_event =
			(event & TRIGGER_EVENT_OPMASK) |
			(row_trigger ? TRIGGER_EVENT_ROW : 0) |
			(trigger->tgdeferrable ? AFTER_TRIGGER_DEFERRABLE : 0) |
			(trigger->tginitdeferred ? AFTER_TRIGGER_INITDEFERRED : 0);
		new_shared.ats_tgoid = trigger->tgoid;
		new_shared.ats_relid = RelationGetRelid(rel);
		new_shared.ats_firing_id = 0;

		afterTriggerAddEvent(&afterTriggers.query_stack[afterTriggers.query_depth],
							 &new_event, &new_shared);
	}

	/*
	 * Finally, spool any foreign tuple(s).  The tuplestore squashes them to
	 * minimal tuples, so this loses any system columns.  The executor lost
	 * those columns before us, for an unrelated reason, so this is fine.
	 */
	if (fdw_tuplestore)
	{
		if (oldtup != NULL)
			tuplestore_puttuple(fdw_tuplestore, oldtup);
		if (newtup != NULL)
			tuplestore_puttuple(fdw_tuplestore, newtup);
	}
}

Datum
pg_trigger_depth(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(MyTriggerDepth);
}
