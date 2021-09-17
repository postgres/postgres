/*-------------------------------------------------------------------------
 *
 * pg_module.c
 *	  routines to support manipulation of the pg_module relation
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_module.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

//#include "access/htup_details.h"
//#include "access/table.h"
#include "catalog/catalog.h"
//#include "catalog/dependency.h"
//#include "catalog/indexing.h"
//#include "catalog/objectaccess.h"
#include "catalog/pg_module.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/syscache.h"


/* ----------------
 * ModuleCreate
 *
 * Create a module with the given name and owner OID.
 *
 * ---------------
 */
Oid
ModuleCreate(const char *modName, Oid ownerId)
{
	Relation	moddesc;
	HeapTuple	tup;
	Oid			modoid;
	bool		nulls[Natts_pg_module];
	Datum		values[Natts_pg_module];
	NameData	nname;
	TupleDesc	tupDesc;
	ObjectAddress myself;
	int			i;
	Acl		   *modacl;

	/* sanity checks */
	if (!modName)
		elog(ERROR, "no module name supplied");

	/* make sure there is no existing module of same name */
	if (SearchSysCacheExists1(MODULENAME, PointerGetDatum(modName)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_MODULE),
				 errmsg("module \"%s\" already exists", modName)));

	modacl = get_user_default_acl(OBJECT_MODULE, ownerId,
								  InvalidOid);

	moddesc = table_open(ModuleRelationId, RowExclusiveLock);
	tupDesc = moddesc->rd_att;

	/* initialize nulls and values */
	for (i = 0; i < Natts_pg_module; i++)
	{
		nulls[i] = false;
		values[i] = (Datum) NULL;
	}

	modoid = GetNewOidWithIndex(moddesc, ModuleOidIndexId,
								Anum_pg_module_oid);
	values[Anum_pg_module_oid - 1] = ObjectIdGetDatum(modoid);
	namestrcpy(&nname, modName);
	values[Anum_pg_module_modname - 1] = NameGetDatum(&nname);
	values[Anum_pg_module_modowner - 1] = ObjectIdGetDatum(ownerId);
	if (modacl != NULL)
		values[Anum_pg_module_modacl - 1] = PointerGetDatum(modacl);
	else
		nulls[Anum_pg_module_modacl - 1] = true;


	tup = heap_form_tuple(tupDesc, values, nulls);

	CatalogTupleInsert(moddesc, tup);
	Assert(OidIsValid(modoid));

	table_close(moddesc, RowExclusiveLock);

	/* Record dependencies */
	myself.classId = ModuleRelationId;
	myself.objectId = modoid;
	myself.objectSubId = 0;

	/* dependency on owner */
	recordDependencyOnOwner(ModuleRelationId, modoid, ownerId);

	/* dependencies on roles mentioned in default ACL */
	recordDependencyOnNewAcl(ModuleRelationId, modoid, 0, ownerId, modacl);

	/* dependency on extension */
	recordDependencyOnCurrentExtension(&myself, false);

	/* Post creation hook for new schema */
	InvokeObjectPostCreateHook(ModuleRelationId, modoid, 0);

	return modoid;
}
