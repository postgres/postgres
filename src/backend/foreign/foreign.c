/*-------------------------------------------------------------------------
 *
 * foreign.c
 *		  support for foreign-data wrappers, servers and user mappings.
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  $PostgreSQL: pgsql/src/backend/foreign/foreign.c,v 1.5 2009/06/11 16:14:18 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/reloptions.h"
#include "catalog/namespace.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_type.h"
#include "catalog/pg_user_mapping.h"
#include "foreign/foreign.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"


extern Datum pg_options_to_table(PG_FUNCTION_ARGS);
extern Datum postgresql_fdw_validator(PG_FUNCTION_ARGS);



/*
 * GetForeignDataWrapper -	look up the foreign-data wrapper by OID.
 */
ForeignDataWrapper *
GetForeignDataWrapper(Oid fdwid)
{
	Form_pg_foreign_data_wrapper fdwform;
	ForeignDataWrapper *fdw;
	Datum		datum;
	HeapTuple	tp;
	bool		isnull;

	tp = SearchSysCache(FOREIGNDATAWRAPPEROID,
						ObjectIdGetDatum(fdwid),
						0, 0, 0);

	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for foreign-data wrapper %u", fdwid);

	fdwform = (Form_pg_foreign_data_wrapper) GETSTRUCT(tp);

	fdw = palloc(sizeof(ForeignDataWrapper));
	fdw->fdwid = fdwid;
	fdw->owner = fdwform->fdwowner;
	fdw->fdwname = pstrdup(NameStr(fdwform->fdwname));
	fdw->fdwvalidator = fdwform->fdwvalidator;

	/* Extract the options */
	datum = SysCacheGetAttr(FOREIGNDATAWRAPPEROID,
							tp,
							Anum_pg_foreign_data_wrapper_fdwoptions,
							&isnull);
	fdw->options = untransformRelOptions(datum);

	ReleaseSysCache(tp);

	return fdw;
}


/*
 * GetForeignDataWrapperOidByName - look up the foreign-data wrapper
 * OID by name.
 */
Oid
GetForeignDataWrapperOidByName(const char *fdwname, bool missing_ok)
{
	Oid			fdwId;

	fdwId = GetSysCacheOid(FOREIGNDATAWRAPPERNAME,
						   CStringGetDatum(fdwname),
						   0, 0, 0);

	if (!OidIsValid(fdwId) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
			 errmsg("foreign-data wrapper \"%s\" does not exist", fdwname)));

	return fdwId;
}


/*
 * GetForeignDataWrapperByName - look up the foreign-data wrapper
 * definition by name.
 */
ForeignDataWrapper *
GetForeignDataWrapperByName(const char *fdwname, bool missing_ok)
{
	Oid			fdwId = GetForeignDataWrapperOidByName(fdwname, missing_ok);

	if (!OidIsValid(fdwId) && missing_ok)
		return NULL;

	return GetForeignDataWrapper(fdwId);
}


/*
 * GetForeignServer - look up the foreign server definition.
 */
ForeignServer *
GetForeignServer(Oid serverid)
{
	Form_pg_foreign_server serverform;
	ForeignServer *server;
	HeapTuple	tp;
	Datum		datum;
	bool		isnull;

	tp = SearchSysCache(FOREIGNSERVEROID,
						ObjectIdGetDatum(serverid),
						0, 0, 0);

	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for foreign server %u", serverid);

	serverform = (Form_pg_foreign_server) GETSTRUCT(tp);

	server = palloc(sizeof(ForeignServer));
	server->serverid = serverid;
	server->servername = pstrdup(NameStr(serverform->srvname));
	server->owner = serverform->srvowner;
	server->fdwid = serverform->srvfdw;

	/* Extract server type */
	datum = SysCacheGetAttr(FOREIGNSERVEROID,
							tp,
							Anum_pg_foreign_server_srvtype,
							&isnull);
	server->servertype = isnull ? NULL : pstrdup(TextDatumGetCString(datum));

	/* Extract server version */
	datum = SysCacheGetAttr(FOREIGNSERVEROID,
							tp,
							Anum_pg_foreign_server_srvversion,
							&isnull);
	server->serverversion = isnull ? NULL : pstrdup(TextDatumGetCString(datum));

	/* Extract the srvoptions */
	datum = SysCacheGetAttr(FOREIGNSERVEROID,
							tp,
							Anum_pg_foreign_server_srvoptions,
							&isnull);

	/* untransformRelOptions does exactly what we want - avoid duplication */
	server->options = untransformRelOptions(datum);

	ReleaseSysCache(tp);

	return server;
}


/*
 * GetForeignServerByName - look up the foreign server oid by name.
 */
Oid
GetForeignServerOidByName(const char *srvname, bool missing_ok)
{
	Oid			serverid;

	serverid = GetSysCacheOid(FOREIGNSERVERNAME,
							  CStringGetDatum(srvname),
							  0, 0, 0);

	if (!OidIsValid(serverid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("server \"%s\" does not exist", srvname)));

	return serverid;
}


/*
 * GetForeignServerByName - look up the foreign server definition by name.
 */
ForeignServer *
GetForeignServerByName(const char *srvname, bool missing_ok)
{
	Oid			serverid = GetForeignServerOidByName(srvname, missing_ok);

	if (!OidIsValid(serverid) && missing_ok)
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
	Form_pg_user_mapping umform;
	Datum		datum;
	HeapTuple	tp;
	bool		isnull;
	UserMapping *um;

	tp = SearchSysCache(USERMAPPINGUSERSERVER,
						ObjectIdGetDatum(userid),
						ObjectIdGetDatum(serverid),
						0, 0);

	if (!HeapTupleIsValid(tp))
	{
		/* Not found for the specific user -- try PUBLIC */
		tp = SearchSysCache(USERMAPPINGUSERSERVER,
							ObjectIdGetDatum(InvalidOid),
							ObjectIdGetDatum(serverid),
							0, 0);
	}

	if (!HeapTupleIsValid(tp))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("user mapping not found for \"%s\"",
						MappingUserName(userid))));

	umform = (Form_pg_user_mapping) GETSTRUCT(tp);

	/* Extract the umoptions */
	datum = SysCacheGetAttr(USERMAPPINGUSERSERVER,
							tp,
							Anum_pg_user_mapping_umoptions,
							&isnull);

	um = palloc(sizeof(UserMapping));
	um->userid = userid;
	um->serverid = serverid;
	um->options = untransformRelOptions(datum);

	ReleaseSysCache(tp);

	return um;
}


/*
 * deflist_to_tuplestore - Helper function to convert DefElem list to
 * tuplestore usable in SRF.
 */
static void
deflist_to_tuplestore(ReturnSetInfo *rsinfo, List *options)
{
	ListCell   *cell;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	Datum		values[2];
	bool		nulls[2] = {0};
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize) ||
		rsinfo->expectedDesc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/*
	 * Now prepare the result set.
	 */
	tupdesc = CreateTupleDescCopy(rsinfo->expectedDesc);
	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	foreach(cell, options)
	{
		DefElem    *def = lfirst(cell);

		values[0] = CStringGetTextDatum(def->defname);
		values[1] = CStringGetTextDatum(((Value *) def->arg)->val.str);
		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	MemoryContextSwitchTo(oldcontext);
}


/*
 * Convert options array to name/value table.  Useful for information
 * schema and pg_dump.
 */
Datum
pg_options_to_table(PG_FUNCTION_ARGS)
{
	Datum		array = PG_GETARG_DATUM(0);

	deflist_to_tuplestore((ReturnSetInfo *) fcinfo->resultinfo, untransformRelOptions(array));

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
static struct ConnectionOption libpq_conninfo_options[] = {
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
	struct ConnectionOption *opt;

	for (opt = libpq_conninfo_options; opt->optname; opt++)
		if ((context == opt->optcontext || context == InvalidOid) && strcmp(opt->optname, option) == 0)
			return true;
	return false;
}


/*
 * Validate the generic option given to SERVER or USER MAPPING.
 * Raise an ERROR if the option or its value is considered
 * invalid.
 *
 * Valid server options are all libpq conninfo options except
 * user and password -- these may only appear in USER MAPPING options.
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
			struct ConnectionOption *opt;
			StringInfoData buf;

			/*
			 * Unknown option specified, complain about it. Provide a hint
			 * with list of valid options for the object.
			 */
			initStringInfo(&buf);
			for (opt = libpq_conninfo_options; opt->optname; opt++)
				if (catalog == InvalidOid || catalog == opt->optcontext)
					appendStringInfo(&buf, "%s%s", (buf.len > 0) ? ", " : "",
									 opt->optname);

			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("invalid option \"%s\"", def->defname),
				errhint("Valid options in this context are: %s", buf.data)));

			PG_RETURN_BOOL(false);
		}
	}

	PG_RETURN_BOOL(true);
}
