/*-------------------------------------------------------------------------
 *
 * foreign.c
 *		  support for foreign-data wrappers, servers and user mappings.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/backend/foreign/foreign.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/reloptions.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_user_mapping.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/varlena.h"


/*
 * GetForeignDataWrapper -	look up the foreign-data wrapper by OID.
 */
ForeignDataWrapper *
GetForeignDataWrapper(Oid fdwid)
{
	return GetForeignDataWrapperExtended(fdwid, 0);
}


/*
 * GetForeignDataWrapperExtended -	look up the foreign-data wrapper
 * by OID. If flags uses FDW_MISSING_OK, return NULL if the object cannot
 * be found instead of raising an error.
 */
ForeignDataWrapper *
GetForeignDataWrapperExtended(Oid fdwid, bits16 flags)
{
	Form_pg_foreign_data_wrapper fdwform;
	ForeignDataWrapper *fdw;
	Datum		datum;
	HeapTuple	tp;
	bool		isnull;

	tp = SearchSysCache1(FOREIGNDATAWRAPPEROID, ObjectIdGetDatum(fdwid));

	if (!HeapTupleIsValid(tp))
	{
		if ((flags & FDW_MISSING_OK) == 0)
			elog(ERROR, "cache lookup failed for foreign-data wrapper %u", fdwid);
		return NULL;
	}

	fdwform = (Form_pg_foreign_data_wrapper) GETSTRUCT(tp);

	fdw = (ForeignDataWrapper *) palloc(sizeof(ForeignDataWrapper));
	fdw->fdwid = fdwid;
	fdw->owner = fdwform->fdwowner;
	fdw->fdwname = pstrdup(NameStr(fdwform->fdwname));
	fdw->fdwhandler = fdwform->fdwhandler;
	fdw->fdwvalidator = fdwform->fdwvalidator;

	/* Extract the fdwoptions */
	datum = SysCacheGetAttr(FOREIGNDATAWRAPPEROID,
							tp,
							Anum_pg_foreign_data_wrapper_fdwoptions,
							&isnull);
	if (isnull)
		fdw->options = NIL;
	else
		fdw->options = untransformRelOptions(datum);

	ReleaseSysCache(tp);

	return fdw;
}


/*
 * GetForeignDataWrapperByName - look up the foreign-data wrapper
 * definition by name.
 */
ForeignDataWrapper *
GetForeignDataWrapperByName(const char *fdwname, bool missing_ok)
{
	Oid			fdwId = get_foreign_data_wrapper_oid(fdwname, missing_ok);

	if (!OidIsValid(fdwId))
		return NULL;

	return GetForeignDataWrapper(fdwId);
}


/*
 * GetForeignServer - look up the foreign server definition.
 */
ForeignServer *
GetForeignServer(Oid serverid)
{
	return GetForeignServerExtended(serverid, 0);
}


/*
 * GetForeignServerExtended - look up the foreign server definition. If
 * flags uses FSV_MISSING_OK, return NULL if the object cannot be found
 * instead of raising an error.
 */
ForeignServer *
GetForeignServerExtended(Oid serverid, bits16 flags)
{
	Form_pg_foreign_server serverform;
	ForeignServer *server;
	HeapTuple	tp;
	Datum		datum;
	bool		isnull;

	tp = SearchSysCache1(FOREIGNSERVEROID, ObjectIdGetDatum(serverid));

	if (!HeapTupleIsValid(tp))
	{
		if ((flags & FSV_MISSING_OK) == 0)
			elog(ERROR, "cache lookup failed for foreign server %u", serverid);
		return NULL;
	}

	serverform = (Form_pg_foreign_server) GETSTRUCT(tp);

	server = (ForeignServer *) palloc(sizeof(ForeignServer));
	server->serverid = serverid;
	server->servername = pstrdup(NameStr(serverform->srvname));
	server->owner = serverform->srvowner;
	server->fdwid = serverform->srvfdw;

	/* Extract server type */
	datum = SysCacheGetAttr(FOREIGNSERVEROID,
							tp,
							Anum_pg_foreign_server_srvtype,
							&isnull);
	server->servertype = isnull ? NULL : TextDatumGetCString(datum);

	/* Extract server version */
	datum = SysCacheGetAttr(FOREIGNSERVEROID,
							tp,
							Anum_pg_foreign_server_srvversion,
							&isnull);
	server->serverversion = isnull ? NULL : TextDatumGetCString(datum);

	/* Extract the srvoptions */
	datum = SysCacheGetAttr(FOREIGNSERVEROID,
							tp,
							Anum_pg_foreign_server_srvoptions,
							&isnull);
	if (isnull)
		server->options = NIL;
	else
		server->options = untransformRelOptions(datum);

	ReleaseSysCache(tp);

	return server;
}


/*
 * GetForeignServerByName - look up the foreign server definition by name.
 */
ForeignServer *
GetForeignServerByName(const char *srvname, bool missing_ok)
{
	Oid			serverid = get_foreign_server_oid(srvname, missing_ok);

	if (!OidIsValid(serverid))
		return NULL;

	return GetForeignServer(serverid);
}


/*
 * GetUserMapping - look up the user mapping.
 *
 * If no mapping is found for the supplied user, we also look for
 * PUBLIC mappings (userid == InvalidOid).
 */
UserMapping *
GetUserMapping(Oid userid, Oid serverid)
{
	Datum		datum;
	HeapTuple	tp;
	bool		isnull;
	UserMapping *um;

	tp = SearchSysCache2(USERMAPPINGUSERSERVER,
						 ObjectIdGetDatum(userid),
						 ObjectIdGetDatum(serverid));

	if (!HeapTupleIsValid(tp))
	{
		/* Not found for the specific user -- try PUBLIC */
		tp = SearchSysCache2(USERMAPPINGUSERSERVER,
							 ObjectIdGetDatum(InvalidOid),
							 ObjectIdGetDatum(serverid));
	}

	if (!HeapTupleIsValid(tp))
	{
		ForeignServer *server = GetForeignServer(serverid);

		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("user mapping not found for user \"%s\", server \"%s\"",
						MappingUserName(userid), server->servername)));
	}

	um = (UserMapping *) palloc(sizeof(UserMapping));
	um->umid = ((Form_pg_user_mapping) GETSTRUCT(tp))->oid;
	um->userid = userid;
	um->serverid = serverid;

	/* Extract the umoptions */
	datum = SysCacheGetAttr(USERMAPPINGUSERSERVER,
							tp,
							Anum_pg_user_mapping_umoptions,
							&isnull);
	if (isnull)
		um->options = NIL;
	else
		um->options = untransformRelOptions(datum);

	ReleaseSysCache(tp);

	return um;
}


/*
 * GetForeignTable - look up the foreign table definition by relation oid.
 */
ForeignTable *
GetForeignTable(Oid relid)
{
	Form_pg_foreign_table tableform;
	ForeignTable *ft;
	HeapTuple	tp;
	Datum		datum;
	bool		isnull;

	tp = SearchSysCache1(FOREIGNTABLEREL, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for foreign table %u", relid);
	tableform = (Form_pg_foreign_table) GETSTRUCT(tp);

	ft = (ForeignTable *) palloc(sizeof(ForeignTable));
	ft->relid = relid;
	ft->serverid = tableform->ftserver;

	/* Extract the ftoptions */
	datum = SysCacheGetAttr(FOREIGNTABLEREL,
							tp,
							Anum_pg_foreign_table_ftoptions,
							&isnull);
	if (isnull)
		ft->options = NIL;
	else
		ft->options = untransformRelOptions(datum);

	ReleaseSysCache(tp);

	return ft;
}


/*
 * GetForeignColumnOptions - Get attfdwoptions of given relation/attnum
 * as list of DefElem.
 */
List *
GetForeignColumnOptions(Oid relid, AttrNumber attnum)
{
	List	   *options;
	HeapTuple	tp;
	Datum		datum;
	bool		isnull;

	tp = SearchSysCache2(ATTNUM,
						 ObjectIdGetDatum(relid),
						 Int16GetDatum(attnum));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for attribute %d of relation %u",
			 attnum, relid);
	datum = SysCacheGetAttr(ATTNUM,
							tp,
							Anum_pg_attribute_attfdwoptions,
							&isnull);
	if (isnull)
		options = NIL;
	else
		options = untransformRelOptions(datum);

	ReleaseSysCache(tp);

	return options;
}


/*
 * GetFdwRoutine - call the specified foreign-data wrapper handler routine
 * to get its FdwRoutine struct.
 */
FdwRoutine *
GetFdwRoutine(Oid fdwhandler)
{
	Datum		datum;
	FdwRoutine *routine;

	/* Check if the access to foreign tables is restricted */
	if (unlikely((restrict_nonsystem_relation_kind & RESTRICT_RELKIND_FOREIGN_TABLE) != 0))
	{
		/* there must not be built-in FDW handler  */
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("access to non-system foreign table is restricted")));
	}

	datum = OidFunctionCall0(fdwhandler);
	routine = (FdwRoutine *) DatumGetPointer(datum);

	if (routine == NULL || !IsA(routine, FdwRoutine))
		elog(ERROR, "foreign-data wrapper handler function %u did not return an FdwRoutine struct",
			 fdwhandler);

	return routine;
}


/*
 * GetForeignServerIdByRelId - look up the foreign server
 * for the given foreign table, and return its OID.
 */
Oid
GetForeignServerIdByRelId(Oid relid)
{
	HeapTuple	tp;
	Form_pg_foreign_table tableform;
	Oid			serverid;

	tp = SearchSysCache1(FOREIGNTABLEREL, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for foreign table %u", relid);
	tableform = (Form_pg_foreign_table) GETSTRUCT(tp);
	serverid = tableform->ftserver;
	ReleaseSysCache(tp);

	return serverid;
}


/*
 * GetFdwRoutineByServerId - look up the handler of the foreign-data wrapper
 * for the given foreign server, and retrieve its FdwRoutine struct.
 */
FdwRoutine *
GetFdwRoutineByServerId(Oid serverid)
{
	HeapTuple	tp;
	Form_pg_foreign_data_wrapper fdwform;
	Form_pg_foreign_server serverform;
	Oid			fdwid;
	Oid			fdwhandler;

	/* Get foreign-data wrapper OID for the server. */
	tp = SearchSysCache1(FOREIGNSERVEROID, ObjectIdGetDatum(serverid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for foreign server %u", serverid);
	serverform = (Form_pg_foreign_server) GETSTRUCT(tp);
	fdwid = serverform->srvfdw;
	ReleaseSysCache(tp);

	/* Get handler function OID for the FDW. */
	tp = SearchSysCache1(FOREIGNDATAWRAPPEROID, ObjectIdGetDatum(fdwid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for foreign-data wrapper %u", fdwid);
	fdwform = (Form_pg_foreign_data_wrapper) GETSTRUCT(tp);
	fdwhandler = fdwform->fdwhandler;

	/* Complain if FDW has been set to NO HANDLER. */
	if (!OidIsValid(fdwhandler))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("foreign-data wrapper \"%s\" has no handler",
						NameStr(fdwform->fdwname))));

	ReleaseSysCache(tp);

	/* And finally, call the handler function. */
	return GetFdwRoutine(fdwhandler);
}


/*
 * GetFdwRoutineByRelId - look up the handler of the foreign-data wrapper
 * for the given foreign table, and retrieve its FdwRoutine struct.
 */
FdwRoutine *
GetFdwRoutineByRelId(Oid relid)
{
	Oid			serverid;

	/* Get server OID for the foreign table. */
	serverid = GetForeignServerIdByRelId(relid);

	/* Now retrieve server's FdwRoutine struct. */
	return GetFdwRoutineByServerId(serverid);
}

/*
 * GetFdwRoutineForRelation - look up the handler of the foreign-data wrapper
 * for the given foreign table, and retrieve its FdwRoutine struct.
 *
 * This function is preferred over GetFdwRoutineByRelId because it caches
 * the data in the relcache entry, saving a number of catalog lookups.
 *
 * If makecopy is true then the returned data is freshly palloc'd in the
 * caller's memory context.  Otherwise, it's a pointer to the relcache data,
 * which will be lost in any relcache reset --- so don't rely on it long.
 */
FdwRoutine *
GetFdwRoutineForRelation(Relation relation, bool makecopy)
{
	FdwRoutine *fdwroutine;
	FdwRoutine *cfdwroutine;

	if (relation->rd_fdwroutine == NULL)
	{
		/* Get the info by consulting the catalogs and the FDW code */
		fdwroutine = GetFdwRoutineByRelId(RelationGetRelid(relation));

		/* Save the data for later reuse in CacheMemoryContext */
		cfdwroutine = (FdwRoutine *) MemoryContextAlloc(CacheMemoryContext,
														sizeof(FdwRoutine));
		memcpy(cfdwroutine, fdwroutine, sizeof(FdwRoutine));
		relation->rd_fdwroutine = cfdwroutine;

		/* Give back the locally palloc'd copy regardless of makecopy */
		return fdwroutine;
	}

	/* We have valid cached data --- does the caller want a copy? */
	if (makecopy)
	{
		fdwroutine = (FdwRoutine *) palloc(sizeof(FdwRoutine));
		memcpy(fdwroutine, relation->rd_fdwroutine, sizeof(FdwRoutine));
		return fdwroutine;
	}

	/* Only a short-lived reference is needed, so just hand back cached copy */
	return relation->rd_fdwroutine;
}


/*
 * IsImportableForeignTable - filter table names for IMPORT FOREIGN SCHEMA
 *
 * Returns true if given table name should be imported according to the
 * statement's import filter options.
 */
bool
IsImportableForeignTable(const char *tablename,
						 ImportForeignSchemaStmt *stmt)
{
	ListCell   *lc;

	switch (stmt->list_type)
	{
		case FDW_IMPORT_SCHEMA_ALL:
			return true;

		case FDW_IMPORT_SCHEMA_LIMIT_TO:
			foreach(lc, stmt->table_list)
			{
				RangeVar   *rv = (RangeVar *) lfirst(lc);

				if (strcmp(tablename, rv->relname) == 0)
					return true;
			}
			return false;

		case FDW_IMPORT_SCHEMA_EXCEPT:
			foreach(lc, stmt->table_list)
			{
				RangeVar   *rv = (RangeVar *) lfirst(lc);

				if (strcmp(tablename, rv->relname) == 0)
					return false;
			}
			return true;
	}
	return false;				/* shouldn't get here */
}


/*
 * pg_options_to_table - Convert options array to name/value table
 *
 * This is useful to provide details for information_schema and pg_dump.
 */
Datum
pg_options_to_table(PG_FUNCTION_ARGS)
{
	Datum		array = PG_GETARG_DATUM(0);
	ListCell   *cell;
	List	   *options;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	options = untransformRelOptions(array);
	rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	/* prepare the result set */
	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);

	foreach(cell, options)
	{
		DefElem    *def = lfirst(cell);
		Datum		values[2];
		bool		nulls[2];

		values[0] = CStringGetTextDatum(def->defname);
		nulls[0] = false;
		if (def->arg)
		{
			values[1] = CStringGetTextDatum(strVal(def->arg));
			nulls[1] = false;
		}
		else
		{
			values[1] = (Datum) 0;
			nulls[1] = true;
		}
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);
	}

	return (Datum) 0;
}


/*
 * Describes the valid options for postgresql FDW, server, and user mapping.
 */
struct ConnectionOption
{
	const char *optname;
	Oid			optcontext;		/* Oid of catalog in which option may appear */
};

/*
 * Copied from fe-connect.c PQconninfoOptions.
 *
 * The list is small - don't bother with bsearch if it stays so.
 */
static const struct ConnectionOption libpq_conninfo_options[] = {
	{"authtype", ForeignServerRelationId},
	{"service", ForeignServerRelationId},
	{"user", UserMappingRelationId},
	{"password", UserMappingRelationId},
	{"connect_timeout", ForeignServerRelationId},
	{"dbname", ForeignServerRelationId},
	{"host", ForeignServerRelationId},
	{"hostaddr", ForeignServerRelationId},
	{"port", ForeignServerRelationId},
	{"tty", ForeignServerRelationId},
	{"options", ForeignServerRelationId},
	{"requiressl", ForeignServerRelationId},
	{"sslmode", ForeignServerRelationId},
	{"gsslib", ForeignServerRelationId},
	{"gssdelegation", ForeignServerRelationId},
	{NULL, InvalidOid}
};


/*
 * Check if the provided option is one of libpq conninfo options.
 * context is the Oid of the catalog the option came from, or 0 if we
 * don't care.
 */
static bool
is_conninfo_option(const char *option, Oid context)
{
	const struct ConnectionOption *opt;

	for (opt = libpq_conninfo_options; opt->optname; opt++)
		if (context == opt->optcontext && strcmp(opt->optname, option) == 0)
			return true;
	return false;
}


/*
 * Validate the generic option given to SERVER or USER MAPPING.
 * Raise an ERROR if the option or its value is considered invalid.
 *
 * Valid server options are all libpq conninfo options except
 * user and password -- these may only appear in USER MAPPING options.
 *
 * Caution: this function is deprecated, and is now meant only for testing
 * purposes, because the list of options it knows about doesn't necessarily
 * square with those known to whichever libpq instance you might be using.
 * Inquire of libpq itself, instead.
 */
Datum
postgresql_fdw_validator(PG_FUNCTION_ARGS)
{
	List	   *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid			catalog = PG_GETARG_OID(1);

	ListCell   *cell;

	foreach(cell, options_list)
	{
		DefElem    *def = lfirst(cell);

		if (!is_conninfo_option(def->defname, catalog))
		{
			const struct ConnectionOption *opt;
			const char *closest_match;
			ClosestMatchState match_state;
			bool		has_valid_options = false;

			/*
			 * Unknown option specified, complain about it. Provide a hint
			 * with a valid option that looks similar, if there is one.
			 */
			initClosestMatch(&match_state, def->defname, 4);
			for (opt = libpq_conninfo_options; opt->optname; opt++)
			{
				if (catalog == opt->optcontext)
				{
					has_valid_options = true;
					updateClosestMatch(&match_state, opt->optname);
				}
			}

			closest_match = getClosestMatch(&match_state);
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("invalid option \"%s\"", def->defname),
					 has_valid_options ? closest_match ?
					 errhint("Perhaps you meant the option \"%s\".",
							 closest_match) : 0 :
					 errhint("There are no valid options in this context.")));

			PG_RETURN_BOOL(false);
		}
	}

	PG_RETURN_BOOL(true);
}


/*
 * get_foreign_data_wrapper_oid - given a FDW name, look up the OID
 *
 * If missing_ok is false, throw an error if name not found.  If true, just
 * return InvalidOid.
 */
Oid
get_foreign_data_wrapper_oid(const char *fdwname, bool missing_ok)
{
	Oid			oid;

	oid = GetSysCacheOid1(FOREIGNDATAWRAPPERNAME,
						  Anum_pg_foreign_data_wrapper_oid,
						  CStringGetDatum(fdwname));
	if (!OidIsValid(oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("foreign-data wrapper \"%s\" does not exist",
						fdwname)));
	return oid;
}


/*
 * get_foreign_server_oid - given a server name, look up the OID
 *
 * If missing_ok is false, throw an error if name not found.  If true, just
 * return InvalidOid.
 */
Oid
get_foreign_server_oid(const char *servername, bool missing_ok)
{
	Oid			oid;

	oid = GetSysCacheOid1(FOREIGNSERVERNAME, Anum_pg_foreign_server_oid,
						  CStringGetDatum(servername));
	if (!OidIsValid(oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("server \"%s\" does not exist", servername)));
	return oid;
}

/*
 * Get a copy of an existing local path for a given join relation.
 *
 * This function is usually helpful to obtain an alternate local path for EPQ
 * checks.
 *
 * Right now, this function only supports unparameterized foreign joins, so we
 * only search for unparameterized path in the given list of paths. Since we
 * are searching for a path which can be used to construct an alternative local
 * plan for a foreign join, we look for only MergeJoin, HashJoin or NestLoop
 * paths.
 *
 * If the inner or outer subpath of the chosen path is a ForeignScan, we
 * replace it with its outer subpath.  For this reason, and also because the
 * planner might free the original path later, the path returned by this
 * function is a shallow copy of the original.  There's no need to copy
 * the substructure, so we don't.
 *
 * Since the plan created using this path will presumably only be used to
 * execute EPQ checks, efficiency of the path is not a concern. But since the
 * path list in RelOptInfo is anyway sorted by total cost we are likely to
 * choose the most efficient path, which is all for the best.
 */
Path *
GetExistingLocalJoinPath(RelOptInfo *joinrel)
{
	ListCell   *lc;

	Assert(IS_JOIN_REL(joinrel));

	foreach(lc, joinrel->pathlist)
	{
		Path	   *path = (Path *) lfirst(lc);
		JoinPath   *joinpath = NULL;

		/* Skip parameterized paths. */
		if (path->param_info != NULL)
			continue;

		switch (path->pathtype)
		{
			case T_HashJoin:
				{
					HashPath   *hash_path = makeNode(HashPath);

					memcpy(hash_path, path, sizeof(HashPath));
					joinpath = (JoinPath *) hash_path;
				}
				break;

			case T_NestLoop:
				{
					NestPath   *nest_path = makeNode(NestPath);

					memcpy(nest_path, path, sizeof(NestPath));
					joinpath = (JoinPath *) nest_path;
				}
				break;

			case T_MergeJoin:
				{
					MergePath  *merge_path = makeNode(MergePath);

					memcpy(merge_path, path, sizeof(MergePath));
					joinpath = (JoinPath *) merge_path;
				}
				break;

			default:

				/*
				 * Just skip anything else. We don't know if corresponding
				 * plan would build the output row from whole-row references
				 * of base relations and execute the EPQ checks.
				 */
				break;
		}

		/* This path isn't good for us, check next. */
		if (!joinpath)
			continue;

		/*
		 * If either inner or outer path is a ForeignPath corresponding to a
		 * pushed down join, replace it with the fdw_outerpath, so that we
		 * maintain path for EPQ checks built entirely of local join
		 * strategies.
		 */
		if (IsA(joinpath->outerjoinpath, ForeignPath))
		{
			ForeignPath *foreign_path;

			foreign_path = (ForeignPath *) joinpath->outerjoinpath;
			if (IS_JOIN_REL(foreign_path->path.parent))
				joinpath->outerjoinpath = foreign_path->fdw_outerpath;
		}

		if (IsA(joinpath->innerjoinpath, ForeignPath))
		{
			ForeignPath *foreign_path;

			foreign_path = (ForeignPath *) joinpath->innerjoinpath;
			if (IS_JOIN_REL(foreign_path->path.parent))
				joinpath->innerjoinpath = foreign_path->fdw_outerpath;
		}

		return (Path *) joinpath;
	}
	return NULL;
}
