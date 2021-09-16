/*-------------------------------------------------------------------------
 *
 * module.c
 *	  code to support accessing and searching modules
 *
 * This is separate from pg_module.c, which contains the routines that
 * directly manipulate the pg_module system catalog.  This module
 * provides routines associated with defining a "module search path"
 * and implementing search-path-controlled searches.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/catalog/module.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/dependency.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_ts_dict.h"
#include "catalog/pg_ts_parser.h"
#include "catalog/pg_ts_template.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_func.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/sinvaladt.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/varlena.h"


/* LIFTED FROM namespace.c. This needs to be looked at and made useful or thrown away */

/*
 * The module search path is a possibly-empty list of namespace OIDs.
 * In addition to the explicit list, implicitly-searched namespaces
 * may be included:
 *
 * 1. If a TEMP table namespace has been initialized in this session, it
 * is implicitly searched first.  (The only time this doesn't happen is
 * when we are obeying an override search path spec that says not to use the
 * temp namespace, or the temp namespace is included in the explicit list.)
 *
 * 2. The system catalog namespace is always searched.  If the system
 * namespace is present in the explicit path then it will be searched in
 * the specified order; otherwise it will be searched after TEMP tables and
 * *before* the explicit list.  (It might seem that the system namespace
 * should be implicitly last, but this behavior appears to be required by
 * SQL99.  Also, this provides a way to search the system namespace first
 * without thereby making it the default creation target namespace.)
 *
 * For security reasons, searches using the search path will ignore the temp
 * namespace when searching for any object type other than relations and
 * types.  (We must allow types since temp tables have rowtypes.)
 *
 * The default creation target namespace is always the first element of the
 * explicit list.  If the explicit list is empty, there is no default target.
 *
 * The textual specification of search_path can include "$user" to refer to
 * the namespace named the same as the current user, if any.  (This is just
 * ignored if there is no such namespace.)	Also, it can include "pg_temp"
 * to refer to the current backend's temp namespace.  This is usually also
 * ignorable if the temp namespace hasn't been set up, but there's a special
 * case: if "pg_temp" appears first then it should be the default creation
 * target.  We kluge this case a little bit so that the temp namespace isn't
 * set up until the first attempt to create something in it.  (The reason for
 * klugery is that we can't create the temp namespace outside a transaction,
 * but initial GUC processing of search_path happens outside a transaction.)
 * activeTempCreationPending is true if "pg_temp" appears first in the string
 * but is not reflected in activeCreationNamespace because the namespace isn't
 * set up yet.
 *
 * In bootstrap mode, the search path is set equal to "pg_catalog", so that
 * the system namespace is the only one searched or inserted into.
 * initdb is also careful to set search_path to "pg_catalog" for its
 * post-bootstrap standalone backend runs.  Otherwise the default search
 * path is determined by GUC.  The factory default path contains the PUBLIC
 * namespace (if it exists), preceded by the user's personal namespace
 * (if one exists).
 *
 * We support a stack of "override" search path settings for use within
 * specific sections of backend code.  namespace_search_path is ignored
 * whenever the override stack is nonempty.  activeSearchPath is always
 * the actually active path; it points either to the search list of the
 * topmost stack entry, or to baseSearchPath which is the list derived
 * from namespace_search_path.
 *
 * If baseSearchPathValid is false, then baseSearchPath (and other
 * derived variables) need to be recomputed from namespace_search_path.
 * We mark it invalid upon an assignment to namespace_search_path or receipt
 * of a syscache invalidation event for pg_namespace.  The recomputation
 * is done during the next non-overridden lookup attempt.  Note that an
 * override spec is never subject to recomputation.
 *
 * Any namespaces mentioned in namespace_search_path that are not readable
 * by the current user ID are simply left out of baseSearchPath; so
 * we have to be willing to recompute the path when current userid changes.
 * namespaceUser is the userid the path has been computed for.
 *
 * Note: all data pointed to by these List variables is in TopMemoryContext.
 *
 * activePathGeneration is incremented whenever the effective values of
 * activeSearchPath/activeCreationNamespace/activeTempCreationPending change.
 * This can be used to quickly detect whether any change has happened since
 * a previous examination of the search path state.
 */

/* These variables define the actually active state: */

static List *activeSearchPath = NIL;

/* default place to create stuff; if InvalidOid, no default */
static Oid	activeCreationNamespace = InvalidOid;

/* if true, activeCreationNamespace is wrong, it should be temp namespace */
static bool activeTempCreationPending = false;

/* current generation counter; make sure this is never zero */
static uint64 activePathGeneration = 1;

/* These variables are the values last derived from namespace_search_path: */

static List *baseSearchPath = NIL;

static Oid	baseCreationNamespace = InvalidOid;

static bool baseTempCreationPending = false;

static Oid	namespaceUser = InvalidOid;

/* The above four values are valid only if baseSearchPathValid */
static bool baseSearchPathValid = true;

/* Override requests are remembered in a stack of OverrideStackEntry structs */

typedef struct
{
	List	   *searchPath;		/* the desired search path */
	Oid			creationNamespace;	/* the desired creation namespace */
	int			nestLevel;		/* subtransaction nesting level */
} OverrideStackEntry;

static List *overrideStack = NIL;

/*
 * myTempNamespace is InvalidOid until and unless a TEMP namespace is set up
 * in a particular backend session (this happens when a CREATE TEMP TABLE
 * command is first executed).  Thereafter it's the OID of the temp namespace.
 *
 * myTempToastNamespace is the OID of the namespace for my temp tables' toast
 * tables.  It is set when myTempNamespace is, and is InvalidOid before that.
 *
 * myTempNamespaceSubID shows whether we've created the TEMP namespace in the
 * current subtransaction.  The flag propagates up the subtransaction tree,
 * so the main transaction will correctly recognize the flag if all
 * intermediate subtransactions commit.  When it is InvalidSubTransactionId,
 * we either haven't made the TEMP namespace yet, or have successfully
 * committed its creation, depending on whether myTempNamespace is valid.
 */
static Oid	myTempNamespace = InvalidOid;

static Oid	myTempToastNamespace = InvalidOid;

static SubTransactionId myTempNamespaceSubID = InvalidSubTransactionId;

/*
 * This is the user's textual search path specification --- it's the value
 * of the GUC variable 'search_path'.
 */
char	   *namespace_search_path = NULL;


/* Local functions */
static void recomputeModulePath(void);
static void AccessTempTableNamespace(bool force);
static void InitTempTableNamespace(void);
static void RemoveTempRelations(Oid tempNamespaceId);
static void RemoveTempRelationsCallback(int code, Datum arg);
static void NamespaceCallback(Datum arg, int cacheid, uint32 hashvalue);
static bool MatchNamedCall(HeapTuple proctup, int nargs, List *argnames,
						   bool include_out_arguments, int pronargs,
						   int **argnumbers);


/*
 * RangeVarGetRelidExtended
 *		Given a RangeVar describing an existing relation,
 *		select the proper namespace and look up the relation OID.
 *
 * If the schema or relation is not found, return InvalidOid if flags contains
 * RVR_MISSING_OK, otherwise raise an error.
 *
 * If flags contains RVR_NOWAIT, throw an error if we'd have to wait for a
 * lock.
 *
 * If flags contains RVR_SKIP_LOCKED, return InvalidOid if we'd have to wait
 * for a lock.
 *
 * flags cannot contain both RVR_NOWAIT and RVR_SKIP_LOCKED.
 *
 * Note that if RVR_MISSING_OK and RVR_SKIP_LOCKED are both specified, a
 * return value of InvalidOid could either mean the relation is missing or it
 * could not be locked.
 *
 * Callback allows caller to check permissions or acquire additional locks
 * prior to grabbing the relation lock.
 */
Oid
RangeVarGetRelidExtended(const RangeVar *relation, LOCKMODE lockmode,
						 uint32 flags,
						 RangeVarGetRelidCallback callback, void *callback_arg)
{
	uint64		inval_count;
	Oid			relId;
	Oid			oldRelId = InvalidOid;
	bool		retry = false;
	bool		missing_ok = (flags & RVR_MISSING_OK) != 0;

	/* verify that flags do no conflict */
	Assert(!((flags & RVR_NOWAIT) && (flags & RVR_SKIP_LOCKED)));

	/*
	 * We check the catalog name and then ignore it.
	 */
	if (relation->catalogname)
	{
		if (strcmp(relation->catalogname, get_database_name(MyDatabaseId)) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cross-database references are not implemented: \"%s.%s.%s\"",
							relation->catalogname, relation->schemaname,
							relation->relname)));
	}

	/*
	 * DDL operations can change the results of a name lookup.  Since all such
	 * operations will generate invalidation messages, we keep track of
	 * whether any such messages show up while we're performing the operation,
	 * and retry until either (1) no more invalidation messages show up or (2)
	 * the answer doesn't change.
	 *
	 * But if lockmode = NoLock, then we assume that either the caller is OK
	 * with the answer changing under them, or that they already hold some
	 * appropriate lock, and therefore return the first answer we get without
	 * checking for invalidation messages.  Also, if the requested lock is
	 * already held, LockRelationOid will not AcceptInvalidationMessages, so
	 * we may fail to notice a change.  We could protect against that case by
	 * calling AcceptInvalidationMessages() before beginning this loop, but
	 * that would add a significant amount overhead, so for now we don't.
	 */
	for (;;)
	{
		/*
		 * Remember this value, so that, after looking up the relation name
		 * and locking its OID, we can check whether any invalidation messages
		 * have been processed that might require a do-over.
		 */
		inval_count = SharedInvalidMessageCounter;

		/*
		 * Some non-default relpersistence value may have been specified.  The
		 * parser never generates such a RangeVar in simple DML, but it can
		 * happen in contexts such as "CREATE TEMP TABLE foo (f1 int PRIMARY
		 * KEY)".  Such a command will generate an added CREATE INDEX
		 * operation, which must be careful to find the temp table, even when
		 * pg_temp is not first in the search path.
		 */
		if (relation->relpersistence == RELPERSISTENCE_TEMP)
		{
			if (!OidIsValid(myTempNamespace))
				relId = InvalidOid; /* this probably can't happen? */
			else
			{
				if (relation->schemaname)
				{
					Oid			namespaceId;

					namespaceId = LookupExplicitNamespace(relation->schemaname, missing_ok);

					/*
					 * For missing_ok, allow a non-existent schema name to
					 * return InvalidOid.
					 */
					if (namespaceId != myTempNamespace)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
								 errmsg("temporary tables cannot specify a schema name")));
				}

				relId = get_relname_relid(relation->relname, myTempNamespace);
			}
		}
		else if (relation->schemaname)
		{
			Oid			namespaceId;

			/* use exact schema given */
			namespaceId = LookupExplicitNamespace(relation->schemaname, missing_ok);
			if (missing_ok && !OidIsValid(namespaceId))
				relId = InvalidOid;
			else
				relId = get_relname_relid(relation->relname, namespaceId);
		}
		else
		{
			/* search the namespace path */
			relId = RelnameGetRelid(relation->relname);
		}

		/*
		 * Invoke caller-supplied callback, if any.
		 *
		 * This callback is a good place to check permissions: we haven't
		 * taken the table lock yet (and it's really best to check permissions
		 * before locking anything!), but we've gotten far enough to know what
		 * OID we think we should lock.  Of course, concurrent DDL might
		 * change things while we're waiting for the lock, but in that case
		 * the callback will be invoked again for the new OID.
		 */
		if (callback)
			callback(relation, relId, oldRelId, callback_arg);

		/*
		 * If no lock requested, we assume the caller knows what they're
		 * doing.  They should have already acquired a heavyweight lock on
		 * this relation earlier in the processing of this same statement, so
		 * it wouldn't be appropriate to AcceptInvalidationMessages() here, as
		 * that might pull the rug out from under them.
		 */
		if (lockmode == NoLock)
			break;

		/*
		 * If, upon retry, we get back the same OID we did last time, then the
		 * invalidation messages we processed did not change the final answer.
		 * So we're done.
		 *
		 * If we got a different OID, we've locked the relation that used to
		 * have this name rather than the one that does now.  So release the
		 * lock.
		 */
		if (retry)
		{
			if (relId == oldRelId)
				break;
			if (OidIsValid(oldRelId))
				UnlockRelationOid(oldRelId, lockmode);
		}

		/*
		 * Lock relation.  This will also accept any pending invalidation
		 * messages.  If we got back InvalidOid, indicating not found, then
		 * there's nothing to lock, but we accept invalidation messages
		 * anyway, to flush any negative catcache entries that may be
		 * lingering.
		 */
		if (!OidIsValid(relId))
			AcceptInvalidationMessages();
		else if (!(flags & (RVR_NOWAIT | RVR_SKIP_LOCKED)))
			LockRelationOid(relId, lockmode);
		else if (!ConditionalLockRelationOid(relId, lockmode))
		{
			int			elevel = (flags & RVR_SKIP_LOCKED) ? DEBUG1 : ERROR;

			if (relation->schemaname)
				ereport(elevel,
						(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
						 errmsg("could not obtain lock on relation \"%s.%s\"",
								relation->schemaname, relation->relname)));
			else
				ereport(elevel,
						(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
						 errmsg("could not obtain lock on relation \"%s\"",
								relation->relname)));

			return InvalidOid;
		}

		/*
		 * If no invalidation message were processed, we're done!
		 */
		if (inval_count == SharedInvalidMessageCounter)
			break;

		/*
		 * Something may have changed.  Let's repeat the name lookup, to make
		 * sure this name still references the same relation it did
		 * previously.
		 */
		retry = true;
		oldRelId = relId;
	}

	if (!OidIsValid(relId))
	{
		int			elevel = missing_ok ? DEBUG1 : ERROR;

		if (relation->schemaname)
			ereport(elevel,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("relation \"%s.%s\" does not exist",
							relation->schemaname, relation->relname)));
		else
			ereport(elevel,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("relation \"%s\" does not exist",
							relation->relname)));
	}
	return relId;
}

/*
 * FuncnameGetCandidates
 *		Given a possibly-qualified function name and argument count,
 *		retrieve a list of the possible matches.
 *
 * If nargs is -1, we return all functions matching the given name,
 * regardless of argument count.  (argnames must be NIL, and expand_variadic
 * and expand_defaults must be false, in this case.)
 *
 * If argnames isn't NIL, we are considering a named- or mixed-notation call,
 * and only functions having all the listed argument names will be returned.
 * (We assume that length(argnames) <= nargs and all the passed-in names are
 * distinct.)  The returned structs will include an argnumbers array showing
 * the actual argument index for each logical argument position.
 *
 * If expand_variadic is true, then variadic functions having the same number
 * or fewer arguments will be retrieved, with the variadic argument and any
 * additional argument positions filled with the variadic element type.
 * nvargs in the returned struct is set to the number of such arguments.
 * If expand_variadic is false, variadic arguments are not treated specially,
 * and the returned nvargs will always be zero.
 *
 * If expand_defaults is true, functions that could match after insertion of
 * default argument values will also be retrieved.  In this case the returned
 * structs could have nargs > passed-in nargs, and ndargs is set to the number
 * of additional args (which can be retrieved from the function's
 * proargdefaults entry).
 *
 * If include_out_arguments is true, then OUT-mode arguments are considered to
 * be included in the argument list.  Their types are included in the returned
 * arrays, and argnumbers are indexes in proallargtypes not proargtypes.
 * We also set nominalnargs to be the length of proallargtypes not proargtypes.
 * Otherwise OUT-mode arguments are ignored.
 *
 * It is not possible for nvargs and ndargs to both be nonzero in the same
 * list entry, since default insertion allows matches to functions with more
 * than nargs arguments while the variadic transformation requires the same
 * number or less.
 *
 * When argnames isn't NIL, the returned args[] type arrays are not ordered
 * according to the functions' declarations, but rather according to the call:
 * first any positional arguments, then the named arguments, then defaulted
 * arguments (if needed and allowed by expand_defaults).  The argnumbers[]
 * array can be used to map this back to the catalog information.
 * argnumbers[k] is set to the proargtypes or proallargtypes index of the
 * k'th call argument.
 *
 * We search a single namespace if the function name is qualified, else
 * all namespaces in the search path.  In the multiple-namespace case,
 * we arrange for entries in earlier namespaces to mask identical entries in
 * later namespaces.
 *
 * When expanding variadics, we arrange for non-variadic functions to mask
 * variadic ones if the expanded argument list is the same.  It is still
 * possible for there to be conflicts between different variadic functions,
 * however.
 *
 * It is guaranteed that the return list will never contain multiple entries
 * with identical argument lists.  When expand_defaults is true, the entries
 * could have more than nargs positions, but we still guarantee that they are
 * distinct in the first nargs positions.  However, if argnames isn't NIL or
 * either expand_variadic or expand_defaults is true, there might be multiple
 * candidate functions that expand to identical argument lists.  Rather than
 * throw error here, we report such situations by returning a single entry
 * with oid = 0 that represents a set of such conflicting candidates.
 * The caller might end up discarding such an entry anyway, but if it selects
 * such an entry it should react as though the call were ambiguous.
 *
 * If missing_ok is true, an empty list (NULL) is returned if the name was
 * schema-qualified with a schema that does not exist.  Likewise if no
 * candidate is found for other reasons.
 */
FuncCandidateList
FuncnameGetCandidates(List *names, int nargs, List *argnames,
					  bool expand_variadic, bool expand_defaults,
					  bool include_out_arguments, bool missing_ok)
{
	FuncCandidateList resultList = NULL;
	bool		any_special = false;
	char	   *schemaname;
	char	   *funcname;
	Oid			namespaceId;
	CatCList   *catlist;
	int			i;

	/* check for caller error */
	Assert(nargs >= 0 || !(expand_variadic | expand_defaults));

	/* deconstruct the name list */
	DeconstructQualifiedNameWithModule(names, &schemaname, &funcname);

	if (schemaname)
	{
		/* use exact schema given */
		namespaceId = LookupExplicitNamespace(schemaname, missing_ok);
		if (!OidIsValid(namespaceId))
			return NULL;
	}
	else
	{
		/* flag to indicate we need namespace search */
		namespaceId = InvalidOid;
		recomputeModulePath();
	}

	/* Search syscache by name only */
	catlist = SearchSysCacheList1(PROCNAMEARGSNSP, CStringGetDatum(funcname));

	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	proctup = &catlist->members[i]->tuple;
		Form_pg_proc procform = (Form_pg_proc) GETSTRUCT(proctup);
		Oid		   *proargtypes = procform->proargtypes.values;
		int			pronargs = procform->pronargs;
		int			effective_nargs;
		int			pathpos = 0;
		bool		variadic;
		bool		use_defaults;
		Oid			va_elem_type;
		int		   *argnumbers = NULL;
		FuncCandidateList newResult;

		if (OidIsValid(moduleId))
		{
			/* Consider only procs in specified module */
			if (procform->pronamespace != moduleId)
				continue;
		}
		else if (OidIsValid(namespaceId))
		{
			/* Consider only procs in specified schema */			
			if (procform->pronamespace != namespaceId)
				continue;
		}
		else
		{
			/*
			 * Consider only procs that are in the search path and are not in
			 * the temp namespace.
			 */
			ListCell   *nsp;

			foreach(nsp, activeSearchPath)
			{
				if (procform->pronamespace == lfirst_oid(nsp) &&
					procform->pronamespace != myTempNamespace)
					break;
				pathpos++;
			}
			if (nsp == NULL)
				continue;		/* proc is not in search path */
		}

		/*
		 * If we are asked to match to OUT arguments, then use the
		 * proallargtypes array (which includes those); otherwise use
		 * proargtypes (which doesn't).  Of course, if proallargtypes is null,
		 * we always use proargtypes.
		 */
		if (include_out_arguments)
		{
			Datum		proallargtypes;
			bool		isNull;

			proallargtypes = SysCacheGetAttr(PROCNAMEARGSNSP, proctup,
											 Anum_pg_proc_proallargtypes,
											 &isNull);
			if (!isNull)
			{
				ArrayType  *arr = DatumGetArrayTypeP(proallargtypes);

				pronargs = ARR_DIMS(arr)[0];
				if (ARR_NDIM(arr) != 1 ||
					pronargs < 0 ||
					ARR_HASNULL(arr) ||
					ARR_ELEMTYPE(arr) != OIDOID)
					elog(ERROR, "proallargtypes is not a 1-D Oid array or it contains nulls");
				Assert(pronargs >= procform->pronargs);
				proargtypes = (Oid *) ARR_DATA_PTR(arr);
			}
		}

		if (argnames != NIL)
		{
			/*
			 * Call uses named or mixed notation
			 *
			 * Named or mixed notation can match a variadic function only if
			 * expand_variadic is off; otherwise there is no way to match the
			 * presumed-nameless parameters expanded from the variadic array.
			 */
			if (OidIsValid(procform->provariadic) && expand_variadic)
				continue;
			va_elem_type = InvalidOid;
			variadic = false;

			/*
			 * Check argument count.
			 */
			Assert(nargs >= 0); /* -1 not supported with argnames */

			if (pronargs > nargs && expand_defaults)
			{
				/* Ignore if not enough default expressions */
				if (nargs + procform->pronargdefaults < pronargs)
					continue;
				use_defaults = true;
			}
			else
				use_defaults = false;

			/* Ignore if it doesn't match requested argument count */
			if (pronargs != nargs && !use_defaults)
				continue;

			/* Check for argument name match, generate positional mapping */
			if (!MatchNamedCall(proctup, nargs, argnames,
								include_out_arguments, pronargs,
								&argnumbers))
				continue;

			/* Named argument matching is always "special" */
			any_special = true;
		}
		else
		{
			/*
			 * Call uses positional notation
			 *
			 * Check if function is variadic, and get variadic element type if
			 * so.  If expand_variadic is false, we should just ignore
			 * variadic-ness.
			 */
			if (pronargs <= nargs && expand_variadic)
			{
				va_elem_type = procform->provariadic;
				variadic = OidIsValid(va_elem_type);
				any_special |= variadic;
			}
			else
			{
				va_elem_type = InvalidOid;
				variadic = false;
			}

			/*
			 * Check if function can match by using parameter defaults.
			 */
			if (pronargs > nargs && expand_defaults)
			{
				/* Ignore if not enough default expressions */
				if (nargs + procform->pronargdefaults < pronargs)
					continue;
				use_defaults = true;
				any_special = true;
			}
			else
				use_defaults = false;

			/* Ignore if it doesn't match requested argument count */
			if (nargs >= 0 && pronargs != nargs && !variadic && !use_defaults)
				continue;
		}

		/*
		 * We must compute the effective argument list so that we can easily
		 * compare it to earlier results.  We waste a palloc cycle if it gets
		 * masked by an earlier result, but really that's a pretty infrequent
		 * case so it's not worth worrying about.
		 */
		effective_nargs = Max(pronargs, nargs);
		newResult = (FuncCandidateList)
			palloc(offsetof(struct _FuncCandidateList, args) +
				   effective_nargs * sizeof(Oid));
		newResult->pathpos = pathpos;
		newResult->oid = procform->oid;
		newResult->nominalnargs = pronargs;
		newResult->nargs = effective_nargs;
		newResult->argnumbers = argnumbers;
		if (argnumbers)
		{
			/* Re-order the argument types into call's logical order */
			int			i;

			for (i = 0; i < pronargs; i++)
				newResult->args[i] = proargtypes[argnumbers[i]];
		}
		else
		{
			/* Simple positional case, just copy proargtypes as-is */
			memcpy(newResult->args, proargtypes, pronargs * sizeof(Oid));
		}
		if (variadic)
		{
			int			i;

			newResult->nvargs = effective_nargs - pronargs + 1;
			/* Expand variadic argument into N copies of element type */
			for (i = pronargs - 1; i < effective_nargs; i++)
				newResult->args[i] = va_elem_type;
		}
		else
			newResult->nvargs = 0;
		newResult->ndargs = use_defaults ? pronargs - nargs : 0;

		/*
		 * Does it have the same arguments as something we already accepted?
		 * If so, decide what to do to avoid returning duplicate argument
		 * lists.  We can skip this check for the single-namespace case if no
		 * special (named, variadic or defaults) match has been made, since
		 * then the unique index on pg_proc guarantees all the matches have
		 * different argument lists.
		 */
		if (resultList != NULL &&
			(any_special || !OidIsValid(namespaceId)))
		{
			/*
			 * If we have an ordered list from SearchSysCacheList (the normal
			 * case), then any conflicting proc must immediately adjoin this
			 * one in the list, so we only need to look at the newest result
			 * item.  If we have an unordered list, we have to scan the whole
			 * result list.  Also, if either the current candidate or any
			 * previous candidate is a special match, we can't assume that
			 * conflicts are adjacent.
			 *
			 * We ignore defaulted arguments in deciding what is a match.
			 */
			FuncCandidateList prevResult;

			if (catlist->ordered && !any_special)
			{
				/* ndargs must be 0 if !any_special */
				if (effective_nargs == resultList->nargs &&
					memcmp(newResult->args,
						   resultList->args,
						   effective_nargs * sizeof(Oid)) == 0)
					prevResult = resultList;
				else
					prevResult = NULL;
			}
			else
			{
				int			cmp_nargs = newResult->nargs - newResult->ndargs;

				for (prevResult = resultList;
					 prevResult;
					 prevResult = prevResult->next)
				{
					if (cmp_nargs == prevResult->nargs - prevResult->ndargs &&
						memcmp(newResult->args,
							   prevResult->args,
							   cmp_nargs * sizeof(Oid)) == 0)
						break;
				}
			}

			if (prevResult)
			{
				/*
				 * We have a match with a previous result.  Decide which one
				 * to keep, or mark it ambiguous if we can't decide.  The
				 * logic here is preference > 0 means prefer the old result,
				 * preference < 0 means prefer the new, preference = 0 means
				 * ambiguous.
				 */
				int			preference;

				if (pathpos != prevResult->pathpos)
				{
					/*
					 * Prefer the one that's earlier in the search path.
					 */
					preference = pathpos - prevResult->pathpos;
				}
				else if (variadic && prevResult->nvargs == 0)
				{
					/*
					 * With variadic functions we could have, for example,
					 * both foo(numeric) and foo(variadic numeric[]) in the
					 * same namespace; if so we prefer the non-variadic match
					 * on efficiency grounds.
					 */
					preference = 1;
				}
				else if (!variadic && prevResult->nvargs > 0)
				{
					preference = -1;
				}
				else
				{
					/*----------
					 * We can't decide.  This can happen with, for example,
					 * both foo(numeric, variadic numeric[]) and
					 * foo(variadic numeric[]) in the same namespace, or
					 * both foo(int) and foo (int, int default something)
					 * in the same namespace, or both foo(a int, b text)
					 * and foo(b text, a int) in the same namespace.
					 *----------
					 */
					preference = 0;
				}

				if (preference > 0)
				{
					/* keep previous result */
					pfree(newResult);
					continue;
				}
				else if (preference < 0)
				{
					/* remove previous result from the list */
					if (prevResult == resultList)
						resultList = prevResult->next;
					else
					{
						FuncCandidateList prevPrevResult;

						for (prevPrevResult = resultList;
							 prevPrevResult;
							 prevPrevResult = prevPrevResult->next)
						{
							if (prevResult == prevPrevResult->next)
							{
								prevPrevResult->next = prevResult->next;
								break;
							}
						}
						Assert(prevPrevResult); /* assert we found it */
					}
					pfree(prevResult);
					/* fall through to add newResult to list */
				}
				else
				{
					/* mark old result as ambiguous, discard new */
					prevResult->oid = InvalidOid;
					pfree(newResult);
					continue;
				}
			}
		}

		/*
		 * Okay to add it to result list
		 */
		newResult->next = resultList;
		resultList = newResult;
	}

	ReleaseSysCacheList(catlist);

	return resultList;
}

/*
 * MatchNamedCall
 *		Given a pg_proc heap tuple and a call's list of argument names,
 *		check whether the function could match the call.
 *
 * The call could match if all supplied argument names are accepted by
 * the function, in positions after the last positional argument, and there
 * are defaults for all unsupplied arguments.
 *
 * If include_out_arguments is true, we are treating OUT arguments as
 * included in the argument list.  pronargs is the number of arguments
 * we're considering (the length of either proargtypes or proallargtypes).
 *
 * The number of positional arguments is nargs - list_length(argnames).
 * Note caller has already done basic checks on argument count.
 *
 * On match, return true and fill *argnumbers with a palloc'd array showing
 * the mapping from call argument positions to actual function argument
 * numbers.  Defaulted arguments are included in this map, at positions
 * after the last supplied argument.
 */
static bool
MatchNamedCall(HeapTuple proctup, int nargs, List *argnames,
			   bool include_out_arguments, int pronargs,
			   int **argnumbers)
{
	Form_pg_proc procform = (Form_pg_proc) GETSTRUCT(proctup);
	int			numposargs = nargs - list_length(argnames);
	int			pronallargs;
	Oid		   *p_argtypes;
	char	  **p_argnames;
	char	   *p_argmodes;
	bool		arggiven[FUNC_MAX_ARGS];
	bool		isnull;
	int			ap;				/* call args position */
	int			pp;				/* proargs position */
	ListCell   *lc;

	Assert(argnames != NIL);
	Assert(numposargs >= 0);
	Assert(nargs <= pronargs);

	/* Ignore this function if its proargnames is null */
	(void) SysCacheGetAttr(PROCOID, proctup, Anum_pg_proc_proargnames,
						   &isnull);
	if (isnull)
		return false;

	/* OK, let's extract the argument names and types */
	pronallargs = get_func_arg_info(proctup,
									&p_argtypes, &p_argnames, &p_argmodes);
	Assert(p_argnames != NULL);

	Assert(include_out_arguments ? (pronargs == pronallargs) : (pronargs <= pronallargs));

	/* initialize state for matching */
	*argnumbers = (int *) palloc(pronargs * sizeof(int));
	memset(arggiven, false, pronargs * sizeof(bool));

	/* there are numposargs positional args before the named args */
	for (ap = 0; ap < numposargs; ap++)
	{
		(*argnumbers)[ap] = ap;
		arggiven[ap] = true;
	}

	/* now examine the named args */
	foreach(lc, argnames)
	{
		char	   *argname = (char *) lfirst(lc);
		bool		found;
		int			i;

		pp = 0;
		found = false;
		for (i = 0; i < pronallargs; i++)
		{
			/* consider only input params, except with include_out_arguments */
			if (!include_out_arguments &&
				p_argmodes &&
				(p_argmodes[i] != FUNC_PARAM_IN &&
				 p_argmodes[i] != FUNC_PARAM_INOUT &&
				 p_argmodes[i] != FUNC_PARAM_VARIADIC))
				continue;
			if (p_argnames[i] && strcmp(p_argnames[i], argname) == 0)
			{
				/* fail if argname matches a positional argument */
				if (arggiven[pp])
					return false;
				arggiven[pp] = true;
				(*argnumbers)[ap] = pp;
				found = true;
				break;
			}
			/* increase pp only for considered parameters */
			pp++;
		}
		/* if name isn't in proargnames, fail */
		if (!found)
			return false;
		ap++;
	}

	Assert(ap == nargs);		/* processed all actual parameters */

	/* Check for default arguments */
	if (nargs < pronargs)
	{
		int			first_arg_with_default = pronargs - procform->pronargdefaults;

		for (pp = numposargs; pp < pronargs; pp++)
		{
			if (arggiven[pp])
				continue;
			/* fail if arg not given and no default available */
			if (pp < first_arg_with_default)
				return false;
			(*argnumbers)[ap++] = pp;
		}
	}

	Assert(ap == pronargs);		/* processed all function parameters */

	return true;
}

/*
 * FunctionIsVisible
 *		Determine whether a function (identified by OID) is visible in the
 *		current search path.  Visible means "would be found by searching
 *		for the unqualified function name with exact argument matches".
 */
bool
FunctionIsVisible(Oid funcid)
{
	HeapTuple	proctup;
	Form_pg_proc procform;
	Oid			pronamespace;
	bool		visible;

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(proctup))
		elog(ERROR, "cache lookup failed for function %u", funcid);
	procform = (Form_pg_proc) GETSTRUCT(proctup);

	recomputeModulePath();

	/*
	 * Quick check: if it ain't in the path at all, it ain't visible. Items in
	 * the system namespace are surely in the path and so we needn't even do
	 * list_member_oid() for them.
	 */
	pronamespace = procform->pronamespace;
	if (pronamespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, pronamespace))
		visible = false;
	else
	{
		/*
		 * If it is in the path, it might still not be visible; it could be
		 * hidden by another proc of the same name and arguments earlier in
		 * the path.  So we must do a slow check to see if this is the same
		 * proc that would be found by FuncnameGetCandidates.
		 */
		char	   *proname = NameStr(procform->proname);
		int			nargs = procform->pronargs;
		FuncCandidateList clist;

		visible = false;

		clist = FuncnameGetCandidates(list_make1(makeString(proname)),
									  nargs, NIL, false, false, false, false);
		for (; clist; clist = clist->next)
		{
			if (memcmp(clist->args, procform->proargtypes.values,
					   nargs * sizeof(Oid)) == 0)
			{
				/* Found the expected entry; is it the right proc? */
				visible = (clist->oid == funcid);
				break;
			}
		}
	}

	ReleaseSysCache(proctup);

	return visible;
}


/*
 * OpernameGetOprid
 *		Given a possibly-qualified operator name and exact input datatypes,
 *		look up the operator.  Returns InvalidOid if not found.
 *
 * Pass oprleft = InvalidOid for a prefix op.
 *
 * If the operator name is not schema-qualified, it is sought in the current
 * namespace search path.  If the name is schema-qualified and the given
 * schema does not exist, InvalidOid is returned.
 */
Oid
OpernameGetOprid(List *names, Oid oprleft, Oid oprright)
{
	char	   *schemaname;
	char	   *opername;
	CatCList   *catlist;
	ListCell   *l;

	/* deconstruct the name list */
	DeconstructQualifiedName(names, &schemaname, &opername);

	if (schemaname)
	{
		/* search only in exact schema given */
		Oid			namespaceId;

		namespaceId = LookupExplicitNamespace(schemaname, true);
		if (OidIsValid(namespaceId))
		{
			HeapTuple	opertup;

			opertup = SearchSysCache4(OPERNAMENSP,
									  CStringGetDatum(opername),
									  ObjectIdGetDatum(oprleft),
									  ObjectIdGetDatum(oprright),
									  ObjectIdGetDatum(namespaceId));
			if (HeapTupleIsValid(opertup))
			{
				Form_pg_operator operclass = (Form_pg_operator) GETSTRUCT(opertup);
				Oid			result = operclass->oid;

				ReleaseSysCache(opertup);
				return result;
			}
		}

		return InvalidOid;
	}

	/* Search syscache by name and argument types */
	catlist = SearchSysCacheList3(OPERNAMENSP,
								  CStringGetDatum(opername),
								  ObjectIdGetDatum(oprleft),
								  ObjectIdGetDatum(oprright));

	if (catlist->n_members == 0)
	{
		/* no hope, fall out early */
		ReleaseSysCacheList(catlist);
		return InvalidOid;
	}

	/*
	 * We have to find the list member that is first in the search path, if
	 * there's more than one.  This doubly-nested loop looks ugly, but in
	 * practice there should usually be few catlist members.
	 */
	recomputeModulePath();

	foreach(l, activeSearchPath)
	{
		Oid			namespaceId = lfirst_oid(l);
		int			i;

		if (namespaceId == myTempNamespace)
			continue;			/* do not look in temp namespace */

		for (i = 0; i < catlist->n_members; i++)
		{
			HeapTuple	opertup = &catlist->members[i]->tuple;
			Form_pg_operator operform = (Form_pg_operator) GETSTRUCT(opertup);

			if (operform->oprnamespace == namespaceId)
			{
				Oid			result = operform->oid;

				ReleaseSysCacheList(catlist);
				return result;
			}
		}
	}

	ReleaseSysCacheList(catlist);
	return InvalidOid;
}

/*
 * OpernameGetCandidates
 *		Given a possibly-qualified operator name and operator kind,
 *		retrieve a list of the possible matches.
 *
 * If oprkind is '\0', we return all operators matching the given name,
 * regardless of arguments.
 *
 * We search a single namespace if the operator name is qualified, else
 * all namespaces in the search path.  The return list will never contain
 * multiple entries with identical argument lists --- in the multiple-
 * namespace case, we arrange for entries in earlier namespaces to mask
 * identical entries in later namespaces.
 *
 * The returned items always have two args[] entries --- the first will be
 * InvalidOid for a prefix oprkind.  nargs is always 2, too.
 */
FuncCandidateList
OpernameGetCandidates(List *names, char oprkind, bool missing_schema_ok)
{
	FuncCandidateList resultList = NULL;
	char	   *resultSpace = NULL;
	int			nextResult = 0;
	char	   *schemaname;
	char	   *opername;
	Oid			namespaceId;
	CatCList   *catlist;
	int			i;

	/* deconstruct the name list */
	DeconstructQualifiedName(names, &schemaname, &opername);

	if (schemaname)
	{
		/* use exact schema given */
		namespaceId = LookupExplicitNamespace(schemaname, missing_schema_ok);
		if (missing_schema_ok && !OidIsValid(namespaceId))
			return NULL;
	}
	else
	{
		/* flag to indicate we need namespace search */
		namespaceId = InvalidOid;
		recomputeModulePath();
	}

	/* Search syscache by name only */
	catlist = SearchSysCacheList1(OPERNAMENSP, CStringGetDatum(opername));

	/*
	 * In typical scenarios, most if not all of the operators found by the
	 * catcache search will end up getting returned; and there can be quite a
	 * few, for common operator names such as '=' or '+'.  To reduce the time
	 * spent in palloc, we allocate the result space as an array large enough
	 * to hold all the operators.  The original coding of this routine did a
	 * separate palloc for each operator, but profiling revealed that the
	 * pallocs used an unreasonably large fraction of parsing time.
	 */
#define SPACE_PER_OP MAXALIGN(offsetof(struct _FuncCandidateList, args) + \
							  2 * sizeof(Oid))

	if (catlist->n_members > 0)
		resultSpace = palloc(catlist->n_members * SPACE_PER_OP);

	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	opertup = &catlist->members[i]->tuple;
		Form_pg_operator operform = (Form_pg_operator) GETSTRUCT(opertup);
		int			pathpos = 0;
		FuncCandidateList newResult;

		/* Ignore operators of wrong kind, if specific kind requested */
		if (oprkind && operform->oprkind != oprkind)
			continue;

		if (OidIsValid(namespaceId))
		{
			/* Consider only opers in specified namespace */
			if (operform->oprnamespace != namespaceId)
				continue;
			/* No need to check args, they must all be different */
		}
		else
		{
			/*
			 * Consider only opers that are in the search path and are not in
			 * the temp namespace.
			 */
			ListCell   *nsp;

			foreach(nsp, activeSearchPath)
			{
				if (operform->oprnamespace == lfirst_oid(nsp) &&
					operform->oprnamespace != myTempNamespace)
					break;
				pathpos++;
			}
			if (nsp == NULL)
				continue;		/* oper is not in search path */

			/*
			 * Okay, it's in the search path, but does it have the same
			 * arguments as something we already accepted?	If so, keep only
			 * the one that appears earlier in the search path.
			 *
			 * If we have an ordered list from SearchSysCacheList (the normal
			 * case), then any conflicting oper must immediately adjoin this
			 * one in the list, so we only need to look at the newest result
			 * item.  If we have an unordered list, we have to scan the whole
			 * result list.
			 */
			if (resultList)
			{
				FuncCandidateList prevResult;

				if (catlist->ordered)
				{
					if (operform->oprleft == resultList->args[0] &&
						operform->oprright == resultList->args[1])
						prevResult = resultList;
					else
						prevResult = NULL;
				}
				else
				{
					for (prevResult = resultList;
						 prevResult;
						 prevResult = prevResult->next)
					{
						if (operform->oprleft == prevResult->args[0] &&
							operform->oprright == prevResult->args[1])
							break;
					}
				}
				if (prevResult)
				{
					/* We have a match with a previous result */
					Assert(pathpos != prevResult->pathpos);
					if (pathpos > prevResult->pathpos)
						continue;	/* keep previous result */
					/* replace previous result */
					prevResult->pathpos = pathpos;
					prevResult->oid = operform->oid;
					continue;	/* args are same, of course */
				}
			}
		}

		/*
		 * Okay to add it to result list
		 */
		newResult = (FuncCandidateList) (resultSpace + nextResult);
		nextResult += SPACE_PER_OP;

		newResult->pathpos = pathpos;
		newResult->oid = operform->oid;
		newResult->nominalnargs = 2;
		newResult->nargs = 2;
		newResult->nvargs = 0;
		newResult->ndargs = 0;
		newResult->argnumbers = NULL;
		newResult->args[0] = operform->oprleft;
		newResult->args[1] = operform->oprright;
		newResult->next = resultList;
		resultList = newResult;
	}

	ReleaseSysCacheList(catlist);

	return resultList;
}

/*
 * OperatorIsVisible
 *		Determine whether an operator (identified by OID) is visible in the
 *		current search path.  Visible means "would be found by searching
 *		for the unqualified operator name with exact argument matches".
 */
bool
OperatorIsVisible(Oid oprid)
{
	HeapTuple	oprtup;
	Form_pg_operator oprform;
	Oid			oprnamespace;
	bool		visible;

	oprtup = SearchSysCache1(OPEROID, ObjectIdGetDatum(oprid));
	if (!HeapTupleIsValid(oprtup))
		elog(ERROR, "cache lookup failed for operator %u", oprid);
	oprform = (Form_pg_operator) GETSTRUCT(oprtup);

	recomputeModulePath();

	/*
	 * Quick check: if it ain't in the path at all, it ain't visible. Items in
	 * the system namespace are surely in the path and so we needn't even do
	 * list_member_oid() for them.
	 */
	oprnamespace = oprform->oprnamespace;
	if (oprnamespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, oprnamespace))
		visible = false;
	else
	{
		/*
		 * If it is in the path, it might still not be visible; it could be
		 * hidden by another operator of the same name and arguments earlier
		 * in the path.  So we must do a slow check to see if this is the same
		 * operator that would be found by OpernameGetOprid.
		 */
		char	   *oprname = NameStr(oprform->oprname);

		visible = (OpernameGetOprid(list_make1(makeString(oprname)),
									oprform->oprleft, oprform->oprright)
				   == oprid);
	}

	ReleaseSysCache(oprtup);

	return visible;
}


/*
 * OpclassnameGetOpcid
 *		Try to resolve an unqualified index opclass name.
 *		Returns OID if opclass found in search path, else InvalidOid.
 *
 * This is essentially the same as TypenameGetTypid, but we have to have
 * an extra argument for the index AM OID.
 */
Oid
OpclassnameGetOpcid(Oid amid, const char *opcname)
{
	Oid			opcid;
	ListCell   *l;

	recomputeModulePath();

	foreach(l, activeSearchPath)
	{
		Oid			namespaceId = lfirst_oid(l);

		if (namespaceId == myTempNamespace)
			continue;			/* do not look in temp namespace */

		opcid = GetSysCacheOid3(CLAAMNAMENSP, Anum_pg_opclass_oid,
								ObjectIdGetDatum(amid),
								PointerGetDatum(opcname),
								ObjectIdGetDatum(namespaceId));
		if (OidIsValid(opcid))
			return opcid;
	}

	/* Not found in path */
	return InvalidOid;
}

/*
 * OpclassIsVisible
 *		Determine whether an opclass (identified by OID) is visible in the
 *		current search path.  Visible means "would be found by searching
 *		for the unqualified opclass name".
 */
bool
OpclassIsVisible(Oid opcid)
{
	HeapTuple	opctup;
	Form_pg_opclass opcform;
	Oid			opcnamespace;
	bool		visible;

	opctup = SearchSysCache1(CLAOID, ObjectIdGetDatum(opcid));
	if (!HeapTupleIsValid(opctup))
		elog(ERROR, "cache lookup failed for opclass %u", opcid);
	opcform = (Form_pg_opclass) GETSTRUCT(opctup);

	recomputeModulePath();

	/*
	 * Quick check: if it ain't in the path at all, it ain't visible. Items in
	 * the system namespace are surely in the path and so we needn't even do
	 * list_member_oid() for them.
	 */
	opcnamespace = opcform->opcnamespace;
	if (opcnamespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, opcnamespace))
		visible = false;
	else
	{
		/*
		 * If it is in the path, it might still not be visible; it could be
		 * hidden by another opclass of the same name earlier in the path. So
		 * we must do a slow check to see if this opclass would be found by
		 * OpclassnameGetOpcid.
		 */
		char	   *opcname = NameStr(opcform->opcname);

		visible = (OpclassnameGetOpcid(opcform->opcmethod, opcname) == opcid);
	}

	ReleaseSysCache(opctup);

	return visible;
}

/*
 * OpfamilynameGetOpfid
 *		Try to resolve an unqualified index opfamily name.
 *		Returns OID if opfamily found in search path, else InvalidOid.
 *
 * This is essentially the same as TypenameGetTypid, but we have to have
 * an extra argument for the index AM OID.
 */
Oid
OpfamilynameGetOpfid(Oid amid, const char *opfname)
{
	Oid			opfid;
	ListCell   *l;

	recomputeModulePath();

	foreach(l, activeSearchPath)
	{
		Oid			namespaceId = lfirst_oid(l);

		if (namespaceId == myTempNamespace)
			continue;			/* do not look in temp namespace */

		opfid = GetSysCacheOid3(OPFAMILYAMNAMENSP, Anum_pg_opfamily_oid,
								ObjectIdGetDatum(amid),
								PointerGetDatum(opfname),
								ObjectIdGetDatum(namespaceId));
		if (OidIsValid(opfid))
			return opfid;
	}

	/* Not found in path */
	return InvalidOid;
}

/*
 * OpfamilyIsVisible
 *		Determine whether an opfamily (identified by OID) is visible in the
 *		current search path.  Visible means "would be found by searching
 *		for the unqualified opfamily name".
 */
bool
OpfamilyIsVisible(Oid opfid)
{
	HeapTuple	opftup;
	Form_pg_opfamily opfform;
	Oid			opfnamespace;
	bool		visible;

	opftup = SearchSysCache1(OPFAMILYOID, ObjectIdGetDatum(opfid));
	if (!HeapTupleIsValid(opftup))
		elog(ERROR, "cache lookup failed for opfamily %u", opfid);
	opfform = (Form_pg_opfamily) GETSTRUCT(opftup);

	recomputeModulePath();

	/*
	 * Quick check: if it ain't in the path at all, it ain't visible. Items in
	 * the system namespace are surely in the path and so we needn't even do
	 * list_member_oid() for them.
	 */
	opfnamespace = opfform->opfnamespace;
	if (opfnamespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, opfnamespace))
		visible = false;
	else
	{
		/*
		 * If it is in the path, it might still not be visible; it could be
		 * hidden by another opfamily of the same name earlier in the path. So
		 * we must do a slow check to see if this opfamily would be found by
		 * OpfamilynameGetOpfid.
		 */
		char	   *opfname = NameStr(opfform->opfname);

		visible = (OpfamilynameGetOpfid(opfform->opfmethod, opfname) == opfid);
	}

	ReleaseSysCache(opftup);

	return visible;
}

/*
 * lookup_collation
 *		If there's a collation of the given name/namespace, and it works
 *		with the given encoding, return its OID.  Else return InvalidOid.
 */
static Oid
lookup_collation(const char *collname, Oid collnamespace, int32 encoding)
{
	Oid			collid;
	HeapTuple	colltup;
	Form_pg_collation collform;

	/* Check for encoding-specific entry (exact match) */
	collid = GetSysCacheOid3(COLLNAMEENCNSP, Anum_pg_collation_oid,
							 PointerGetDatum(collname),
							 Int32GetDatum(encoding),
							 ObjectIdGetDatum(collnamespace));
	if (OidIsValid(collid))
		return collid;

	/*
	 * Check for any-encoding entry.  This takes a bit more work: while libc
	 * collations with collencoding = -1 do work with all encodings, ICU
	 * collations only work with certain encodings, so we have to check that
	 * aspect before deciding it's a match.
	 */
	colltup = SearchSysCache3(COLLNAMEENCNSP,
							  PointerGetDatum(collname),
							  Int32GetDatum(-1),
							  ObjectIdGetDatum(collnamespace));
	if (!HeapTupleIsValid(colltup))
		return InvalidOid;
	collform = (Form_pg_collation) GETSTRUCT(colltup);
	if (collform->collprovider == COLLPROVIDER_ICU)
	{
		if (is_encoding_supported_by_icu(encoding))
			collid = collform->oid;
		else
			collid = InvalidOid;
	}
	else
	{
		collid = collform->oid;
	}
	ReleaseSysCache(colltup);
	return collid;
}

/*
 * CollationGetCollid
 *		Try to resolve an unqualified collation name.
 *		Returns OID if collation found in search path, else InvalidOid.
 *
 * Note that this will only find collations that work with the current
 * database's encoding.
 */
Oid
CollationGetCollid(const char *collname)
{
	int32		dbencoding = GetDatabaseEncoding();
	ListCell   *l;

	recomputeModulePath();

	foreach(l, activeSearchPath)
	{
		Oid			namespaceId = lfirst_oid(l);
		Oid			collid;

		if (namespaceId == myTempNamespace)
			continue;			/* do not look in temp namespace */

		collid = lookup_collation(collname, namespaceId, dbencoding);
		if (OidIsValid(collid))
			return collid;
	}

	/* Not found in path */
	return InvalidOid;
}

/*
 * CollationIsVisible
 *		Determine whether a collation (identified by OID) is visible in the
 *		current search path.  Visible means "would be found by searching
 *		for the unqualified collation name".
 *
 * Note that only collations that work with the current database's encoding
 * will be considered visible.
 */
bool
CollationIsVisible(Oid collid)
{
	HeapTuple	colltup;
	Form_pg_collation collform;
	Oid			collnamespace;
	bool		visible;

	colltup = SearchSysCache1(COLLOID, ObjectIdGetDatum(collid));
	if (!HeapTupleIsValid(colltup))
		elog(ERROR, "cache lookup failed for collation %u", collid);
	collform = (Form_pg_collation) GETSTRUCT(colltup);

	recomputeModulePath();

	/*
	 * Quick check: if it ain't in the path at all, it ain't visible. Items in
	 * the system namespace are surely in the path and so we needn't even do
	 * list_member_oid() for them.
	 */
	collnamespace = collform->collnamespace;
	if (collnamespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, collnamespace))
		visible = false;
	else
	{
		/*
		 * If it is in the path, it might still not be visible; it could be
		 * hidden by another collation of the same name earlier in the path,
		 * or it might not work with the current DB encoding.  So we must do a
		 * slow check to see if this collation would be found by
		 * CollationGetCollid.
		 */
		char	   *collname = NameStr(collform->collname);

		visible = (CollationGetCollid(collname) == collid);
	}

	ReleaseSysCache(colltup);

	return visible;
}


/*
 * ConversionGetConid
 *		Try to resolve an unqualified conversion name.
 *		Returns OID if conversion found in search path, else InvalidOid.
 *
 * This is essentially the same as RelnameGetRelid.
 */
Oid
ConversionGetConid(const char *conname)
{
	Oid			conid;
	ListCell   *l;

	recomputeModulePath();

	foreach(l, activeSearchPath)
	{
		Oid			namespaceId = lfirst_oid(l);

		if (namespaceId == myTempNamespace)
			continue;			/* do not look in temp namespace */

		conid = GetSysCacheOid2(CONNAMENSP, Anum_pg_conversion_oid,
								PointerGetDatum(conname),
								ObjectIdGetDatum(namespaceId));
		if (OidIsValid(conid))
			return conid;
	}

	/* Not found in path */
	return InvalidOid;
}

/*
 * ConversionIsVisible
 *		Determine whether a conversion (identified by OID) is visible in the
 *		current search path.  Visible means "would be found by searching
 *		for the unqualified conversion name".
 */
bool
ConversionIsVisible(Oid conid)
{
	HeapTuple	contup;
	Form_pg_conversion conform;
	Oid			connamespace;
	bool		visible;

	contup = SearchSysCache1(CONVOID, ObjectIdGetDatum(conid));
	if (!HeapTupleIsValid(contup))
		elog(ERROR, "cache lookup failed for conversion %u", conid);
	conform = (Form_pg_conversion) GETSTRUCT(contup);

	recomputeModulePath();

	/*
	 * Quick check: if it ain't in the path at all, it ain't visible. Items in
	 * the system namespace are surely in the path and so we needn't even do
	 * list_member_oid() for them.
	 */
	connamespace = conform->connamespace;
	if (connamespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, connamespace))
		visible = false;
	else
	{
		/*
		 * If it is in the path, it might still not be visible; it could be
		 * hidden by another conversion of the same name earlier in the path.
		 * So we must do a slow check to see if this conversion would be found
		 * by ConversionGetConid.
		 */
		char	   *conname = NameStr(conform->conname);

		visible = (ConversionGetConid(conname) == conid);
	}

	ReleaseSysCache(contup);

	return visible;
}

/*
 * get_module_oid - find a module by possibly qualified name
 *
 * If not found, returns InvalidOid if missing_ok, else throws error
 */
/*
Oid
get_module_oid(List *names, bool missing_ok)
{
	char	   *schemaname;
	char	   *modulename;
	Oid			namespaceId;
	Oid			moduleId = InvalidOid;
	ListCell   *l;

	// deconstruct the name list. Treat the module as an object
	DeconstructQualifiedName(names, &schemaname, &modulename);

	if (schemaname)
	{
		// use exact schema given
		namespaceId = LookupExplicitNamespace(schemaname, missing_ok);
		if (missing_ok && !OidIsValid(namespaceId))
			moduleId = InvalidOid;
		else
			moduleId = GetSysCacheOid2(NAMESPACENAME, Anum_pg_namespace_oid,
						  CStringGetDatum(modulename), ObjectIdGetDatum(namespaceId));
	}
	else
	{
		// search for it in search path
		recomputeNamespacePath();

		foreach(l, activeSearchPath)
		{
			namespaceId = lfirst_oid(l);

			if (namespaceId == myTempNamespace)
				continue;		// do not look in temp namespace
			moduleId = GetSysCacheOid2(NAMESPACENAME, Anum_pg_namespace_oid,
						  CStringGetDatum(modulename), ObjectIdGetDatum(namespaceId));
			if (OidIsValid(moduleId))
				break;
		}
	}

	if (!OidIsValid(moduleId) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("module \"%s\" does not exist",
						NameListToString(names))));

	return moduleId;
}
*/


/*
 * get_ts_dict_oid - find a TS dictionary by possibly qualified name
 *
 * If not found, returns InvalidOid if missing_ok, else throws error
 */
Oid
get_ts_dict_oid(List *names, bool missing_ok)
{
	char	   *schemaname;
	char	   *dict_name;
	Oid			namespaceId;
	Oid			dictoid = InvalidOid;
	ListCell   *l;

	/* deconstruct the name list */
	DeconstructQualifiedName(names, &schemaname, &dict_name);

	if (schemaname)
	{
		/* use exact schema given */
		namespaceId = LookupExplicitNamespace(schemaname, missing_ok);
		if (missing_ok && !OidIsValid(namespaceId))
			dictoid = InvalidOid;
		else
			dictoid = GetSysCacheOid2(TSDICTNAMENSP, Anum_pg_ts_dict_oid,
									  PointerGetDatum(dict_name),
									  ObjectIdGetDatum(namespaceId));
	}
	else
	{
		/* search for it in search path */
		recomputeNamespacePath();

		foreach(l, activeSearchPath)
		{
			namespaceId = lfirst_oid(l);

			if (namespaceId == myTempNamespace)
				continue;		/* do not look in temp namespace */

			dictoid = GetSysCacheOid2(TSDICTNAMENSP, Anum_pg_ts_dict_oid,
									  PointerGetDatum(dict_name),
									  ObjectIdGetDatum(namespaceId));
			if (OidIsValid(dictoid))
				break;
		}
	}

	if (!OidIsValid(dictoid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("text search dictionary \"%s\" does not exist",
						NameListToString(names))));

	return dictoid;
}

/*
 * TSDictionaryIsVisible
 *		Determine whether a dictionary (identified by OID) is visible in the
 *		current search path.  Visible means "would be found by searching
 *		for the unqualified dictionary name".
 */
bool
TSDictionaryIsVisible(Oid dictId)
{
	HeapTuple	tup;
	Form_pg_ts_dict form;
	Oid			namespace;
	bool		visible;

	tup = SearchSysCache1(TSDICTOID, ObjectIdGetDatum(dictId));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for text search dictionary %u",
			 dictId);
	form = (Form_pg_ts_dict) GETSTRUCT(tup);

	recomputeNamespacePath();

	/*
	 * Quick check: if it ain't in the path at all, it ain't visible. Items in
	 * the system namespace are surely in the path and so we needn't even do
	 * list_member_oid() for them.
	 */
	namespace = form->dictnamespace;
	if (namespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, namespace))
		visible = false;
	else
	{
		/*
		 * If it is in the path, it might still not be visible; it could be
		 * hidden by another dictionary of the same name earlier in the path.
		 * So we must do a slow check for conflicting dictionaries.
		 */
		char	   *name = NameStr(form->dictname);
		ListCell   *l;

		visible = false;
		foreach(l, activeSearchPath)
		{
			Oid			namespaceId = lfirst_oid(l);

			if (namespaceId == myTempNamespace)
				continue;		/* do not look in temp namespace */

			if (namespaceId == namespace)
			{
				/* Found it first in path */
				visible = true;
				break;
			}
			if (SearchSysCacheExists2(TSDICTNAMENSP,
									  PointerGetDatum(name),
									  ObjectIdGetDatum(namespaceId)))
			{
				/* Found something else first in path */
				break;
			}
		}
	}

	ReleaseSysCache(tup);

	return visible;
}

/*
 * get_ts_template_oid - find a TS template by possibly qualified name
 *
 * If not found, returns InvalidOid if missing_ok, else throws error
 */
Oid
get_ts_template_oid(List *names, bool missing_ok)
{
	char	   *schemaname;
	char	   *template_name;
	Oid			namespaceId;
	Oid			tmploid = InvalidOid;
	ListCell   *l;

	/* deconstruct the name list */
	DeconstructQualifiedName(names, &schemaname, &template_name);

	if (schemaname)
	{
		/* use exact schema given */
		namespaceId = LookupExplicitNamespace(schemaname, missing_ok);
		if (missing_ok && !OidIsValid(namespaceId))
			tmploid = InvalidOid;
		else
			tmploid = GetSysCacheOid2(TSTEMPLATENAMENSP, Anum_pg_ts_template_oid,
									  PointerGetDatum(template_name),
									  ObjectIdGetDatum(namespaceId));
	}
	else
	{
		/* search for it in search path */
		recomputeNamespacePath();

		foreach(l, activeSearchPath)
		{
			namespaceId = lfirst_oid(l);

			if (namespaceId == myTempNamespace)
				continue;		/* do not look in temp namespace */

			tmploid = GetSysCacheOid2(TSTEMPLATENAMENSP, Anum_pg_ts_template_oid,
									  PointerGetDatum(template_name),
									  ObjectIdGetDatum(namespaceId));
			if (OidIsValid(tmploid))
				break;
		}
	}

	if (!OidIsValid(tmploid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("text search template \"%s\" does not exist",
						NameListToString(names))));

	return tmploid;
}

/*
 * TSTemplateIsVisible
 *		Determine whether a template (identified by OID) is visible in the
 *		current search path.  Visible means "would be found by searching
 *		for the unqualified template name".
 */
bool
TSTemplateIsVisible(Oid tmplId)
{
	HeapTuple	tup;
	Form_pg_ts_template form;
	Oid			namespace;
	bool		visible;

	tup = SearchSysCache1(TSTEMPLATEOID, ObjectIdGetDatum(tmplId));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for text search template %u", tmplId);
	form = (Form_pg_ts_template) GETSTRUCT(tup);

	recomputeNamespacePath();

	/*
	 * Quick check: if it ain't in the path at all, it ain't visible. Items in
	 * the system namespace are surely in the path and so we needn't even do
	 * list_member_oid() for them.
	 */
	namespace = form->tmplnamespace;
	if (namespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, namespace))
		visible = false;
	else
	{
		/*
		 * If it is in the path, it might still not be visible; it could be
		 * hidden by another template of the same name earlier in the path. So
		 * we must do a slow check for conflicting templates.
		 */
		char	   *name = NameStr(form->tmplname);
		ListCell   *l;

		visible = false;
		foreach(l, activeSearchPath)
		{
			Oid			namespaceId = lfirst_oid(l);

			if (namespaceId == myTempNamespace)
				continue;		/* do not look in temp namespace */

			if (namespaceId == namespace)
			{
				/* Found it first in path */
				visible = true;
				break;
			}
			if (SearchSysCacheExists2(TSTEMPLATENAMENSP,
									  PointerGetDatum(name),
									  ObjectIdGetDatum(namespaceId)))
			{
				/* Found something else first in path */
				break;
			}
		}
	}

	ReleaseSysCache(tup);

	return visible;
}

/*
 * get_ts_config_oid - find a TS config by possibly qualified name
 *
 * If not found, returns InvalidOid if missing_ok, else throws error
 */
Oid
get_ts_config_oid(List *names, bool missing_ok)
{
	char	   *schemaname;
	char	   *config_name;
	Oid			namespaceId;
	Oid			cfgoid = InvalidOid;
	ListCell   *l;

	/* deconstruct the name list */
	DeconstructQualifiedName(names, &schemaname, &config_name);

	if (schemaname)
	{
		/* use exact schema given */
		namespaceId = LookupExplicitNamespace(schemaname, missing_ok);
		if (missing_ok && !OidIsValid(namespaceId))
			cfgoid = InvalidOid;
		else
			cfgoid = GetSysCacheOid2(TSCONFIGNAMENSP, Anum_pg_ts_config_oid,
									 PointerGetDatum(config_name),
									 ObjectIdGetDatum(namespaceId));
	}
	else
	{
		/* search for it in search path */
		recomputeNamespacePath();

		foreach(l, activeSearchPath)
		{
			namespaceId = lfirst_oid(l);

			if (namespaceId == myTempNamespace)
				continue;		/* do not look in temp namespace */

			cfgoid = GetSysCacheOid2(TSCONFIGNAMENSP, Anum_pg_ts_config_oid,
									 PointerGetDatum(config_name),
									 ObjectIdGetDatum(namespaceId));
			if (OidIsValid(cfgoid))
				break;
		}
	}

	if (!OidIsValid(cfgoid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("text search configuration \"%s\" does not exist",
						NameListToString(names))));

	return cfgoid;
}

/*
 * TSConfigIsVisible
 *		Determine whether a text search configuration (identified by OID)
 *		is visible in the current search path.  Visible means "would be found
 *		by searching for the unqualified text search configuration name".
 */
bool
TSConfigIsVisible(Oid cfgid)
{
	HeapTuple	tup;
	Form_pg_ts_config form;
	Oid			namespace;
	bool		visible;

	tup = SearchSysCache1(TSCONFIGOID, ObjectIdGetDatum(cfgid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for text search configuration %u",
			 cfgid);
	form = (Form_pg_ts_config) GETSTRUCT(tup);

	recomputeNamespacePath();

	/*
	 * Quick check: if it ain't in the path at all, it ain't visible. Items in
	 * the system namespace are surely in the path and so we needn't even do
	 * list_member_oid() for them.
	 */
	namespace = form->cfgnamespace;
	if (namespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, namespace))
		visible = false;
	else
	{
		/*
		 * If it is in the path, it might still not be visible; it could be
		 * hidden by another configuration of the same name earlier in the
		 * path. So we must do a slow check for conflicting configurations.
		 */
		char	   *name = NameStr(form->cfgname);
		ListCell   *l;

		visible = false;
		foreach(l, activeSearchPath)
		{
			Oid			namespaceId = lfirst_oid(l);

			if (namespaceId == myTempNamespace)
				continue;		/* do not look in temp namespace */

			if (namespaceId == namespace)
			{
				/* Found it first in path */
				visible = true;
				break;
			}
			if (SearchSysCacheExists2(TSCONFIGNAMENSP,
									  PointerGetDatum(name),
									  ObjectIdGetDatum(namespaceId)))
			{
				/* Found something else first in path */
				break;
			}
		}
	}

	ReleaseSysCache(tup);

	return visible;
}

/*
 * DeconstructQualifiedNameWithModule
 *		Given a possibly-qualified name expressed as a list of String nodes,
 *		extract the schema name, module name and object name.
 *
 * *nspname_p is set to NULL if there is no explicit schema name.
 * *modname_p is set to NULL if there is no explicit module name.
 */
void
DeconstructQualifiedNameWithModule(List *names,
						 char **nspname_p,
						 char **modname_p,
						 char **objname_p)
{
	char	   *catalogname;
	char	   *nspname = NULL;
	char	   *modulename = NULL;
	char	   *objname = NULL;

	switch (list_length(names))
	{
		case 1:
			objname = strVal(linitial(names));
			break;
		case 2:
			nspname = strVal(linitial(names));
			objname = strVal(lsecond(names));
			break;
		case 3:
				/*
				 * Since we don't allow cross-database references, check if the
				 * first element is the current catalog and if is different assume
				 * the first element is a schema
				 */
				if (strcmp(strVal(linitial(names)), get_database_name(MyDatabaseId)) != 0)
				{
					nspname = strVal(linitial(names));
					modulename = strVal(lsecond(names));
				}
				else
				{
					catalogname = strVal(linitial(names));
					nspname = strVal(lsecond(names));
				}

				objname = strVal(lthird(names));
			break;
		case 4:
				catalogname = strVal(linitial(names));
				nspname = strVal(lsecond(names));
				modulename = strVal(lthird(names));
				objname = strVal(lfourth(names));

				/*
				 * We check the catalog name and then ignore it.
				 */
				if (strcmp(catalogname, get_database_name(MyDatabaseId)) != 0)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cross-database references are not implemented: %s",
									NameListToString(names))));
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("improper qualified name (too many dotted names): %s",
							NameListToString(names))));
			break;
	}

	*nspname_p = nspname;
	if (modname_p)
		*modname_p = modulename;
	*objname_p = objname;
}
/*
 * LookupNamespaceNoError
 *		Look up a schema name.
 *
 * Returns the namespace OID, or InvalidOid if not found.
 *
 * Note this does NOT perform any permissions check --- callers are
 * responsible for being sure that an appropriate check is made.
 * In the majority of cases LookupExplicitNamespace is preferable.
 */
Oid
LookupModuleNoError(const char *modname)
{
	return get_module_oid(modname, true);
}

/*
 * LookupExplicitNamespace
 *		Process an explicitly-specified schema name: look up the schema
 *		and verify we have USAGE (lookup) rights in it.
 *
 * Returns the namespace OID
 */
Oid
LookupExplicitModule(const char *modname, bool missing_ok)
{
	Oid			moduleId;
	AclResult	aclresult;

	moduleId = get_module_oid(modname, missing_ok);
	if (missing_ok && !OidIsValid(moduleId))
		return InvalidOid;

	aclresult = pg_module_aclcheck(moduleId, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   modname);
	/* Schema search hook for this lookup */
	InvokeNamespaceSearchHook(moduleId, true);

	return moduleId;
}

/*
 * LookupCreationNamespace
 *		Look up the schema and verify we have CREATE rights on it.
 *
 * This is just like LookupExplicitNamespace except for the different
 * permission check, and that we are willing to create pg_temp if needed.
 *
 * Note: calling this may result in a CommandCounterIncrement operation,
 * if we have to create or clean out the temp namespace.
 */
Oid
LookupCreationModule(const char *modname)
{
	Oid			moduleId;
	AclResult	aclresult;

	moduleId = get_module_oid(modname, false);

	aclresult = pg_module_aclcheck(moduleId, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_MODULE,
					   modname);

	return moduleId;
}


/*
 * get_module_oid - given a namespace name, look up the OID
 *
 * If missing_ok is false, throw an error if namespace name not found.  If
 * true, just return InvalidOid.
 */
Oid
get_module_oid(const char *modname, bool missing_ok)
{
	Oid			oid;

	oid = GetSysCacheOid1(MODULENAME, Anum_pg_module_oid,
						  CStringGetDatum(modname));
	if (!OidIsValid(oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_MODULE),
				 errmsg("module \"%s\" does not exist", modname)));

	return oid;
}

/*
 * makeRangeVarFromNameList
 *		Utility routine to convert a qualified-name list into RangeVar form.
 */
RangeVar *
makeRangeVarFromNameList(List *names)
{
	RangeVar   *rel = makeRangeVar(NULL, NULL, -1);

	switch (list_length(names))
	{
		case 1:
			rel->relname = strVal(linitial(names));
			break;
		case 2:
			rel->schemaname = strVal(linitial(names));
			rel->relname = strVal(lsecond(names));
			break;
		case 3:
			rel->catalogname = strVal(linitial(names));
			rel->schemaname = strVal(lsecond(names));
			rel->relname = strVal(lthird(names));
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("improper relation name (too many dotted names): %s",
							NameListToString(names))));
			break;
	}

	return rel;
}

/*
 * NameListToString
 *		Utility routine to convert a qualified-name list into a string.
 *
 * This is used primarily to form error messages, and so we do not quote
 * the list elements, for the sake of legibility.
 *
 * In most scenarios the list elements should always be String values,
 * but we also allow A_Star for the convenience of ColumnRef processing.
 */
char *
NameListToString(List *names)
{
	StringInfoData string;
	ListCell   *l;

	initStringInfo(&string);

	foreach(l, names)
	{
		Node	   *name = (Node *) lfirst(l);

		if (l != list_head(names))
			appendStringInfoChar(&string, '.');

		if (IsA(name, String))
			appendStringInfoString(&string, strVal(name));
		else if (IsA(name, A_Star))
			appendStringInfoChar(&string, '*');
		else
			elog(ERROR, "unexpected node type in name list: %d",
				 (int) nodeTag(name));
	}

	return string.data;
}

/*
 * NameListToQuotedString
 *		Utility routine to convert a qualified-name list into a string.
 *
 * Same as above except that names will be double-quoted where necessary,
 * so the string could be re-parsed (eg, by textToQualifiedNameList).
 */
char *
NameListToQuotedString(List *names)
{
	StringInfoData string;
	ListCell   *l;

	initStringInfo(&string);

	foreach(l, names)
	{
		if (l != list_head(names))
			appendStringInfoChar(&string, '.');
		appendStringInfoString(&string, quote_identifier(strVal(lfirst(l))));
	}

	return string.data;
}


/*
 * recomputeModulePath - recompute path derived variables if needed.
 */
static void
recomputeModulePath(void)
{
	Oid			roleid = GetUserId();
	char	   *rawname;
	List	   *namelist;
	List	   *oidlist;
	List	   *newpath;
	ListCell   *l;
	bool		temp_missing;
	Oid			firstNS;
	bool		pathChanged;
	MemoryContext oldcxt;

	/* Do nothing if an override search spec is active. */
	if (overrideStack)
		return;

	/* Do nothing if path is already valid. */
	if (baseSearchPathValid && moduleUser == roleid)
		return;

	/* Need a modifiable copy of namespace_search_path string */
	rawname = pstrdup(namespace_search_path);

	/* Parse string into list of identifiers */
	if (!SplitIdentifierString(rawname, ',', &namelist))
	{
		/* syntax error in name list */
		/* this should not happen if GUC checked check_search_path */
		elog(ERROR, "invalid list syntax");
	}

	/*
	 * Convert the list of names to a list of OIDs.  If any names are not
	 * recognizable or we don't have read access, just leave them out of the
	 * list.  (We can't raise an error, since the search_path setting has
	 * already been accepted.)	Don't make duplicate entries, either.
	 */
	oidlist = NIL;
	temp_missing = false;
	foreach(l, namelist)
	{
		char	   *curname = (char *) lfirst(l);
		Oid			moduleId;

		if (strcmp(curname, "$user") == 0)
		{
			/* $user --- substitute namespace matching user name, if any */
			HeapTuple	tuple;

			tuple = SearchSysCache1(AUTHOID, ObjectIdGetDatum(roleid));
			if (HeapTupleIsValid(tuple))
			{
				char	   *rname;

				rname = NameStr(((Form_pg_authid) GETSTRUCT(tuple))->rolname);
				moduleId = get_module_oid(rname, true);
				ReleaseSysCache(tuple);
				if (OidIsValid(moduleId) &&
					!list_member_oid(oidlist, moduleId) &&
					pg_module_aclcheck(moduleId, roleid,
										  ACL_USAGE) == ACLCHECK_OK &&
					InvokeNamespaceSearchHook(moduleId, false))
					oidlist = lappend_oid(oidlist, moduleId);
			}
		}
		else if (strcmp(curname, "pg_temp") == 0)
		{
			/* pg_temp --- substitute temp namespace, if any */
			if (OidIsValid(myTempNamespace))
			{
				if (!list_member_oid(oidlist, myTempNamespace) &&
					InvokeNamespaceSearchHook(myTempNamespace, false))
					oidlist = lappend_oid(oidlist, myTempNamespace);
			}
			else
			{
				/* If it ought to be the creation namespace, set flag */
				if (oidlist == NIL)
					temp_missing = true;
			}
		}
		else
		{
			/* normal namespace reference */
			moduleId = get_module_oid(curname, true);
			if (OidIsValid(moduleId) &&
				!list_member_oid(oidlist, moduleId) &&
				pg_module_aclcheck(moduleId, roleid,
									  ACL_USAGE) == ACLCHECK_OK &&
				InvokeNamespaceSearchHook(moduleId, false))
				oidlist = lappend_oid(oidlist, moduleId);
		}
	}

	/*
	 * Remember the first member of the explicit list.  (Note: this is
	 * nominally wrong if temp_missing, but we need it anyway to distinguish
	 * explicit from implicit mention of pg_catalog.)
	 */
	if (oidlist == NIL)
		firstNS = InvalidOid;
	else
		firstNS = linitial_oid(oidlist);

	/*
	 * Add any implicitly-searched namespaces to the list.  Note these go on
	 * the front, not the back; also notice that we do not check USAGE
	 * permissions for these.
	 */
	if (!list_member_oid(oidlist, PG_CATALOG_NAMESPACE))
		oidlist = lcons_oid(PG_CATALOG_NAMESPACE, oidlist);

	if (OidIsValid(myTempNamespace) &&
		!list_member_oid(oidlist, myTempNamespace))
		oidlist = lcons_oid(myTempNamespace, oidlist);

	/*
	 * We want to detect the case where the effective value of the base search
	 * path variables didn't change.  As long as we're doing so, we can avoid
	 * copying the OID list unnecessarily.
	 */
	if (baseCreationNamespace == firstNS &&
		baseTempCreationPending == temp_missing &&
		equal(oidlist, baseSearchPath))
	{
		pathChanged = false;
	}
	else
	{
		pathChanged = true;

		/* Must save OID list in permanent storage. */
		oldcxt = MemoryContextSwitchTo(TopMemoryContext);
		newpath = list_copy(oidlist);
		MemoryContextSwitchTo(oldcxt);

		/* Now safe to assign to state variables. */
		list_free(baseSearchPath);
		baseSearchPath = newpath;
		baseCreationNamespace = firstNS;
		baseTempCreationPending = temp_missing;
	}

	/* Mark the path valid. */
	baseSearchPathValid = true;
	namespaceUser = roleid;

	/* And make it active. */
	activeSearchPath = baseSearchPath;
	activeCreationNamespace = baseCreationNamespace;
	activeTempCreationPending = baseTempCreationPending;

	/*
	 * Bump the generation only if something actually changed.  (Notice that
	 * what we compared to was the old state of the base path variables; so
	 * this does not deal with the situation where we have just popped an
	 * override path and restored the prior state of the base path.  Instead
	 * we rely on the override-popping logic to have bumped the generation.)
	 */
	if (pathChanged)
		activePathGeneration++;

	/* Clean up. */
	pfree(rawname);
	list_free(namelist);
	list_free(oidlist);
}



/*
 * InitializeSearchPath: initialize module during InitPostgres.
 *
 * This is called after we are up enough to be able to do catalog lookups.
 */
void
InitializeSearchPath(void)
{
	if (IsBootstrapProcessingMode())
	{
		/*
		 * In bootstrap mode, the search path must be 'pg_catalog' so that
		 * tables are created in the proper namespace; ignore the GUC setting.
		 */
		MemoryContext oldcxt;

		oldcxt = MemoryContextSwitchTo(TopMemoryContext);
		baseSearchPath = list_make1_oid(PG_CATALOG_NAMESPACE);
		MemoryContextSwitchTo(oldcxt);
		baseCreationNamespace = PG_CATALOG_NAMESPACE;
		baseTempCreationPending = false;
		baseSearchPathValid = true;
		namespaceUser = GetUserId();
		activeSearchPath = baseSearchPath;
		activeCreationNamespace = baseCreationNamespace;
		activeTempCreationPending = baseTempCreationPending;
		activePathGeneration++; /* pro forma */
	}
	else
	{
		/*
		 * In normal mode, arrange for a callback on any syscache invalidation
		 * of pg_namespace rows.
		 */
		CacheRegisterSyscacheCallback(NAMESPACEOID,
									  NamespaceCallback,
									  (Datum) 0);
		/* Force search path to be recomputed on next use */
		baseSearchPathValid = false;
	}
}

/*
 * NamespaceCallback
 *		Syscache inval callback function
 */
static void
NamespaceCallback(Datum arg, int cacheid, uint32 hashvalue)
{
	/* Force search path to be recomputed on next use */
	baseSearchPathValid = false;
}

/*
 * Fetch the active search path. The return value is a palloc'ed list
 * of OIDs; the caller is responsible for freeing this storage as
 * appropriate.
 *
 * The returned list includes the implicitly-prepended namespaces only if
 * includeImplicit is true.
 *
 * Note: calling this may result in a CommandCounterIncrement operation,
 * if we have to create or clean out the temp namespace.
 */
List *
fetch_search_path(bool includeImplicit)
{
	List	   *result;

	recomputeModulePath();

	/*
	 * If the temp namespace should be first, force it to exist.  This is so
	 * that callers can trust the result to reflect the actual default
	 * creation namespace.  It's a bit bogus to do this here, since
	 * current_schema() is supposedly a stable function without side-effects,
	 * but the alternatives seem worse.
	 */
	if (activeTempCreationPending)
	{
		AccessTempTableNamespace(true);
		recomputeModulePath();
	}

	result = list_copy(activeSearchPath);
	if (!includeImplicit)
	{
		while (result && linitial_oid(result) != activeCreationNamespace)
			result = list_delete_first(result);
	}

	return result;
}

/*
 * Fetch the active search path into a caller-allocated array of OIDs.
 * Returns the number of path entries.  (If this is more than sarray_len,
 * then the data didn't fit and is not all stored.)
 *
 * The returned list always includes the implicitly-prepended namespaces,
 * but never includes the temp namespace.  (This is suitable for existing
 * users, which would want to ignore the temp namespace anyway.)  This
 * definition allows us to not worry about initializing the temp namespace.
 */
int
fetch_search_path_array(Oid *sarray, int sarray_len)
{
	int			count = 0;
	ListCell   *l;

	recomputeModulePath();

	foreach(l, activeSearchPath)
	{
		Oid			namespaceId = lfirst_oid(l);

		if (namespaceId == myTempNamespace)
			continue;			/* do not include temp namespace */

		if (count < sarray_len)
			sarray[count] = namespaceId;
		count++;
	}

	return count;
}
