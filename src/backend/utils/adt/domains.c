/*-------------------------------------------------------------------------
 *
 * domains.c
 *	  I/O functions for domain types.
 *
 * The output functions for a domain type are just the same ones provided
 * by its underlying base type.  The input functions, however, must be
 * prepared to apply any constraints defined by the type.  So, we create
 * special input functions that invoke the base type's input function
 * and then check the constraints.
 *
 * The overhead required for constraint checking can be high, since examining
 * the catalogs to discover the constraints for a given domain is not cheap.
 * We have three mechanisms for minimizing this cost:
 *	1.  We rely on the typcache to keep up-to-date copies of the constraints.
 *	2.  In a nest of domains, we flatten the checking of all the levels
 *		into just one operation (the typcache does this for us).
 *	3.  If there are CHECK constraints, we cache a standalone ExprContext
 *		to evaluate them in.
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/domains.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "utils/expandeddatum.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


/*
 * structure to cache state across multiple calls
 */
typedef struct DomainIOData
{
	Oid			domain_type;
	/* Data needed to call base type's input function */
	Oid			typiofunc;
	Oid			typioparam;
	int32		typtypmod;
	FmgrInfo	proc;
	/* Reference to cached list of constraint items to check */
	DomainConstraintRef constraint_ref;
	/* Context for evaluating CHECK constraints in */
	ExprContext *econtext;
	/* Memory context this cache is in */
	MemoryContext mcxt;
} DomainIOData;


/*
 * domain_state_setup - initialize the cache for a new domain type.
 *
 * Note: we can't re-use the same cache struct for a new domain type,
 * since there's no provision for releasing the DomainConstraintRef.
 * If a call site needs to deal with a new domain type, we just leak
 * the old struct for the duration of the query.
 */
static DomainIOData *
domain_state_setup(Oid domainType, bool binary, MemoryContext mcxt)
{
	DomainIOData *my_extra;
	TypeCacheEntry *typentry;
	Oid			baseType;

	my_extra = (DomainIOData *) MemoryContextAlloc(mcxt, sizeof(DomainIOData));

	/*
	 * Verify that domainType represents a valid domain type.  We need to be
	 * careful here because domain_in and domain_recv can be called from SQL,
	 * possibly with incorrect arguments.  We use lookup_type_cache mainly
	 * because it will throw a clean user-facing error for a bad OID; but also
	 * it can cache the underlying base type info.
	 */
	typentry = lookup_type_cache(domainType, TYPECACHE_DOMAIN_BASE_INFO);
	if (typentry->typtype != TYPTYPE_DOMAIN)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("type %s is not a domain",
						format_type_be(domainType))));

	/* Find out the base type */
	baseType = typentry->domainBaseType;
	my_extra->typtypmod = typentry->domainBaseTypmod;

	/* Look up underlying I/O function */
	if (binary)
		getTypeBinaryInputInfo(baseType,
							   &my_extra->typiofunc,
							   &my_extra->typioparam);
	else
		getTypeInputInfo(baseType,
						 &my_extra->typiofunc,
						 &my_extra->typioparam);
	fmgr_info_cxt(my_extra->typiofunc, &my_extra->proc, mcxt);

	/* Look up constraints for domain */
	InitDomainConstraintRef(domainType, &my_extra->constraint_ref, mcxt, true);

	/* We don't make an ExprContext until needed */
	my_extra->econtext = NULL;
	my_extra->mcxt = mcxt;

	/* Mark cache valid */
	my_extra->domain_type = domainType;

	return my_extra;
}

/*
 * domain_check_input - apply the cached checks.
 *
 * This is roughly similar to the handling of CoerceToDomain nodes in
 * execExpr*.c, but we execute each constraint separately, rather than
 * compiling them in-line within a larger expression.
 */
static void
domain_check_input(Datum value, bool isnull, DomainIOData *my_extra)
{
	ExprContext *econtext = my_extra->econtext;
	ListCell   *l;

	/* Make sure we have up-to-date constraints */
	UpdateDomainConstraintRef(&my_extra->constraint_ref);

	foreach(l, my_extra->constraint_ref.constraints)
	{
		DomainConstraintState *con = (DomainConstraintState *) lfirst(l);

		switch (con->constrainttype)
		{
			case DOM_CONSTRAINT_NOTNULL:
				if (isnull)
					ereport(ERROR,
							(errcode(ERRCODE_NOT_NULL_VIOLATION),
							 errmsg("domain %s does not allow null values",
									format_type_be(my_extra->domain_type)),
							 errdatatype(my_extra->domain_type)));
				break;
			case DOM_CONSTRAINT_CHECK:
				{
					/* Make the econtext if we didn't already */
					if (econtext == NULL)
					{
						MemoryContext oldcontext;

						oldcontext = MemoryContextSwitchTo(my_extra->mcxt);
						econtext = CreateStandaloneExprContext();
						MemoryContextSwitchTo(oldcontext);
						my_extra->econtext = econtext;
					}

					/*
					 * Set up value to be returned by CoerceToDomainValue
					 * nodes.  Unlike in the generic expression case, this
					 * econtext couldn't be shared with anything else, so no
					 * need to save and restore fields.  But we do need to
					 * protect the passed-in value against being changed by
					 * called functions.  (It couldn't be a R/W expanded
					 * object for most uses, but that seems possible for
					 * domain_check().)
					 */
					econtext->domainValue_datum =
						MakeExpandedObjectReadOnly(value, isnull,
												   my_extra->constraint_ref.tcache->typlen);
					econtext->domainValue_isNull = isnull;

					if (!ExecCheck(con->check_exprstate, econtext))
						ereport(ERROR,
								(errcode(ERRCODE_CHECK_VIOLATION),
								 errmsg("value for domain %s violates check constraint \"%s\"",
										format_type_be(my_extra->domain_type),
										con->name),
								 errdomainconstraint(my_extra->domain_type,
													 con->name)));
					break;
				}
			default:
				elog(ERROR, "unrecognized constraint type: %d",
					 (int) con->constrainttype);
				break;
		}
	}

	/*
	 * Before exiting, call any shutdown callbacks and reset econtext's
	 * per-tuple memory.  This avoids leaking non-memory resources, if
	 * anything in the expression(s) has any.
	 */
	if (econtext)
		ReScanExprContext(econtext);
}


/*
 * domain_in		- input routine for any domain type.
 */
Datum
domain_in(PG_FUNCTION_ARGS)
{
	char	   *string;
	Oid			domainType;
	DomainIOData *my_extra;
	Datum		value;

	/*
	 * Since domain_in is not strict, we have to check for null inputs. The
	 * typioparam argument should never be null in normal system usage, but it
	 * could be null in a manual invocation --- if so, just return null.
	 */
	if (PG_ARGISNULL(0))
		string = NULL;
	else
		string = PG_GETARG_CSTRING(0);
	if (PG_ARGISNULL(1))
		PG_RETURN_NULL();
	domainType = PG_GETARG_OID(1);

	/*
	 * We arrange to look up the needed info just once per series of calls,
	 * assuming the domain type doesn't change underneath us (which really
	 * shouldn't happen, but cope if it does).
	 */
	my_extra = (DomainIOData *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL || my_extra->domain_type != domainType)
	{
		my_extra = domain_state_setup(domainType, false,
									  fcinfo->flinfo->fn_mcxt);
		fcinfo->flinfo->fn_extra = (void *) my_extra;
	}

	/*
	 * Invoke the base type's typinput procedure to convert the data.
	 */
	value = InputFunctionCall(&my_extra->proc,
							  string,
							  my_extra->typioparam,
							  my_extra->typtypmod);

	/*
	 * Do the necessary checks to ensure it's a valid domain value.
	 */
	domain_check_input(value, (string == NULL), my_extra);

	if (string == NULL)
		PG_RETURN_NULL();
	else
		PG_RETURN_DATUM(value);
}

/*
 * domain_recv		- binary input routine for any domain type.
 */
Datum
domain_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf;
	Oid			domainType;
	DomainIOData *my_extra;
	Datum		value;

	/*
	 * Since domain_recv is not strict, we have to check for null inputs. The
	 * typioparam argument should never be null in normal system usage, but it
	 * could be null in a manual invocation --- if so, just return null.
	 */
	if (PG_ARGISNULL(0))
		buf = NULL;
	else
		buf = (StringInfo) PG_GETARG_POINTER(0);
	if (PG_ARGISNULL(1))
		PG_RETURN_NULL();
	domainType = PG_GETARG_OID(1);

	/*
	 * We arrange to look up the needed info just once per series of calls,
	 * assuming the domain type doesn't change underneath us (which really
	 * shouldn't happen, but cope if it does).
	 */
	my_extra = (DomainIOData *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL || my_extra->domain_type != domainType)
	{
		my_extra = domain_state_setup(domainType, true,
									  fcinfo->flinfo->fn_mcxt);
		fcinfo->flinfo->fn_extra = (void *) my_extra;
	}

	/*
	 * Invoke the base type's typreceive procedure to convert the data.
	 */
	value = ReceiveFunctionCall(&my_extra->proc,
								buf,
								my_extra->typioparam,
								my_extra->typtypmod);

	/*
	 * Do the necessary checks to ensure it's a valid domain value.
	 */
	domain_check_input(value, (buf == NULL), my_extra);

	if (buf == NULL)
		PG_RETURN_NULL();
	else
		PG_RETURN_DATUM(value);
}

/*
 * domain_check - check that a datum satisfies the constraints of a
 * domain.  extra and mcxt can be passed if they are available from,
 * say, a FmgrInfo structure, or they can be NULL, in which case the
 * setup is repeated for each call.
 */
void
domain_check(Datum value, bool isnull, Oid domainType,
			 void **extra, MemoryContext mcxt)
{
	DomainIOData *my_extra = NULL;

	if (mcxt == NULL)
		mcxt = CurrentMemoryContext;

	/*
	 * We arrange to look up the needed info just once per series of calls,
	 * assuming the domain type doesn't change underneath us (which really
	 * shouldn't happen, but cope if it does).
	 */
	if (extra)
		my_extra = (DomainIOData *) *extra;
	if (my_extra == NULL || my_extra->domain_type != domainType)
	{
		my_extra = domain_state_setup(domainType, true, mcxt);
		if (extra)
			*extra = (void *) my_extra;
	}

	/*
	 * Do the necessary checks to ensure it's a valid domain value.
	 */
	domain_check_input(value, isnull, my_extra);
}

/*
 * errdatatype --- stores schema_name and datatype_name of a datatype
 * within the current errordata.
 */
int
errdatatype(Oid datatypeOid)
{
	HeapTuple	tup;
	Form_pg_type typtup;

	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(datatypeOid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", datatypeOid);
	typtup = (Form_pg_type) GETSTRUCT(tup);

	err_generic_string(PG_DIAG_SCHEMA_NAME,
					   get_namespace_name(typtup->typnamespace));
	err_generic_string(PG_DIAG_DATATYPE_NAME, NameStr(typtup->typname));

	ReleaseSysCache(tup);

	return 0;					/* return value does not matter */
}

/*
 * errdomainconstraint --- stores schema_name, datatype_name and
 * constraint_name of a domain-related constraint within the current errordata.
 */
int
errdomainconstraint(Oid datatypeOid, const char *conname)
{
	errdatatype(datatypeOid);
	err_generic_string(PG_DIAG_CONSTRAINT_NAME, conname);

	return 0;					/* return value does not matter */
}
