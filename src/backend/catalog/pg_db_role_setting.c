/*
 * pg_db_role_setting.c
 *		Routines to support manipulation of the pg_db_role_setting relation
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/backend/catalog/pg_db_role_setting.c
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_db_role_setting.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/tqual.h"

void
AlterSetting(Oid databaseid, Oid roleid, VariableSetStmt *setstmt)
{
	char	   *valuestr;
	HeapTuple	tuple;
	Relation	rel;
	ScanKeyData scankey[2];
	SysScanDesc scan;

	valuestr = ExtractSetVariableArgs(setstmt);

	/* Get the old tuple, if any. */

	rel = heap_open(DbRoleSettingRelationId, RowExclusiveLock);
	ScanKeyInit(&scankey[0],
				Anum_pg_db_role_setting_setdatabase,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(databaseid));
	ScanKeyInit(&scankey[1],
				Anum_pg_db_role_setting_setrole,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(roleid));
	scan = systable_beginscan(rel, DbRoleSettingDatidRolidIndexId, true,
							  SnapshotNow, 2, scankey);
	tuple = systable_getnext(scan);

	/*
	 * There are three cases:
	 *
	 * - in RESET ALL, request GUC to reset the settings array and update the
	 * catalog if there's anything left, delete it otherwise
	 *
	 * - in other commands, if there's a tuple in pg_db_role_setting, update
	 * it; if it ends up empty, delete it
	 *
	 * - otherwise, insert a new pg_db_role_setting tuple, but only if the
	 * command is not RESET
	 */
	if (setstmt->kind == VAR_RESET_ALL)
	{
		if (HeapTupleIsValid(tuple))
		{
			ArrayType  *new = NULL;
			Datum		datum;
			bool		isnull;

			datum = heap_getattr(tuple, Anum_pg_db_role_setting_setconfig,
								 RelationGetDescr(rel), &isnull);

			if (!isnull)
				new = GUCArrayReset(DatumGetArrayTypeP(datum));

			if (new)
			{
				Datum		repl_val[Natts_pg_db_role_setting];
				bool		repl_null[Natts_pg_db_role_setting];
				bool		repl_repl[Natts_pg_db_role_setting];
				HeapTuple	newtuple;

				memset(repl_repl, false, sizeof(repl_repl));

				repl_val[Anum_pg_db_role_setting_setconfig - 1] =
					PointerGetDatum(new);
				repl_repl[Anum_pg_db_role_setting_setconfig - 1] = true;
				repl_null[Anum_pg_db_role_setting_setconfig - 1] = false;

				newtuple = heap_modify_tuple(tuple, RelationGetDescr(rel),
											 repl_val, repl_null, repl_repl);
				simple_heap_update(rel, &tuple->t_self, newtuple);

				/* Update indexes */
				CatalogUpdateIndexes(rel, newtuple);
			}
			else
				simple_heap_delete(rel, &tuple->t_self);
		}
	}
	else if (HeapTupleIsValid(tuple))
	{
		Datum		repl_val[Natts_pg_db_role_setting];
		bool		repl_null[Natts_pg_db_role_setting];
		bool		repl_repl[Natts_pg_db_role_setting];
		HeapTuple	newtuple;
		Datum		datum;
		bool		isnull;
		ArrayType  *a;

		memset(repl_repl, false, sizeof(repl_repl));
		repl_repl[Anum_pg_db_role_setting_setconfig - 1] = true;
		repl_null[Anum_pg_db_role_setting_setconfig - 1] = false;

		/* Extract old value of setconfig */
		datum = heap_getattr(tuple, Anum_pg_db_role_setting_setconfig,
							 RelationGetDescr(rel), &isnull);
		a = isnull ? NULL : DatumGetArrayTypeP(datum);

		/* Update (valuestr is NULL in RESET cases) */
		if (valuestr)
			a = GUCArrayAdd(a, setstmt->name, valuestr);
		else
			a = GUCArrayDelete(a, setstmt->name);

		if (a)
		{
			repl_val[Anum_pg_db_role_setting_setconfig - 1] =
				PointerGetDatum(a);

			newtuple = heap_modify_tuple(tuple, RelationGetDescr(rel),
										 repl_val, repl_null, repl_repl);
			simple_heap_update(rel, &tuple->t_self, newtuple);

			/* Update indexes */
			CatalogUpdateIndexes(rel, newtuple);
		}
		else
			simple_heap_delete(rel, &tuple->t_self);
	}
	else if (valuestr)
	{
		/* non-null valuestr means it's not RESET, so insert a new tuple */
		HeapTuple	newtuple;
		Datum		values[Natts_pg_db_role_setting];
		bool		nulls[Natts_pg_db_role_setting];
		ArrayType  *a;

		memset(nulls, false, sizeof(nulls));

		a = GUCArrayAdd(NULL, setstmt->name, valuestr);

		values[Anum_pg_db_role_setting_setdatabase - 1] =
			ObjectIdGetDatum(databaseid);
		values[Anum_pg_db_role_setting_setrole - 1] = ObjectIdGetDatum(roleid);
		values[Anum_pg_db_role_setting_setconfig - 1] = PointerGetDatum(a);
		newtuple = heap_form_tuple(RelationGetDescr(rel), values, nulls);

		simple_heap_insert(rel, newtuple);

		/* Update indexes */
		CatalogUpdateIndexes(rel, newtuple);
	}

	InvokeObjectPostAlterHookArg(DbRoleSettingRelationId,
								 databaseid, 0, roleid, false);

	systable_endscan(scan);

	/* Close pg_db_role_setting, but keep lock till commit */
	heap_close(rel, NoLock);
}

/*
 * Drop some settings from the catalog.  These can be for a particular
 * database, or for a particular role.	(It is of course possible to do both
 * too, but it doesn't make sense for current uses.)
 */
void
DropSetting(Oid databaseid, Oid roleid)
{
	Relation	relsetting;
	HeapScanDesc scan;
	ScanKeyData keys[2];
	HeapTuple	tup;
	int			numkeys = 0;

	relsetting = heap_open(DbRoleSettingRelationId, RowExclusiveLock);

	if (OidIsValid(databaseid))
	{
		ScanKeyInit(&keys[numkeys],
					Anum_pg_db_role_setting_setdatabase,
					BTEqualStrategyNumber,
					F_OIDEQ,
					ObjectIdGetDatum(databaseid));
		numkeys++;
	}
	if (OidIsValid(roleid))
	{
		ScanKeyInit(&keys[numkeys],
					Anum_pg_db_role_setting_setrole,
					BTEqualStrategyNumber,
					F_OIDEQ,
					ObjectIdGetDatum(roleid));
		numkeys++;
	}

	scan = heap_beginscan(relsetting, SnapshotNow, numkeys, keys);
	while (HeapTupleIsValid(tup = heap_getnext(scan, ForwardScanDirection)))
	{
		simple_heap_delete(relsetting, &tup->t_self);
	}
	heap_endscan(scan);

	heap_close(relsetting, RowExclusiveLock);
}

/*
 * Scan pg_db_role_setting looking for applicable settings, and load them on
 * the current process.
 *
 * relsetting is pg_db_role_setting, already opened and locked.
 *
 * Note: we only consider setting for the exact databaseid/roleid combination.
 * This probably needs to be called more than once, with InvalidOid passed as
 * databaseid/roleid.
 */
void
ApplySetting(Oid databaseid, Oid roleid, Relation relsetting, GucSource source)
{
	SysScanDesc scan;
	ScanKeyData keys[2];
	HeapTuple	tup;

	ScanKeyInit(&keys[0],
				Anum_pg_db_role_setting_setdatabase,
				BTEqualStrategyNumber,
				F_OIDEQ,
				ObjectIdGetDatum(databaseid));
	ScanKeyInit(&keys[1],
				Anum_pg_db_role_setting_setrole,
				BTEqualStrategyNumber,
				F_OIDEQ,
				ObjectIdGetDatum(roleid));

	scan = systable_beginscan(relsetting, DbRoleSettingDatidRolidIndexId, true,
							  SnapshotNow, 2, keys);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		bool		isnull;
		Datum		datum;

		datum = heap_getattr(tup, Anum_pg_db_role_setting_setconfig,
							 RelationGetDescr(relsetting), &isnull);
		if (!isnull)
		{
			ArrayType  *a = DatumGetArrayTypeP(datum);

			/*
			 * We process all the options at SUSET level.  We assume that the
			 * right to insert an option into pg_db_role_setting was checked
			 * when it was inserted.
			 */
			ProcessGUCArray(a, PGC_SUSET, source, GUC_ACTION_SET);
		}
	}

	systable_endscan(scan);
}
