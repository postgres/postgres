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
 *	1.	In a nest of domains, we flatten the checking of all the levels
 *		into just one operation.
 *	2.	We cache the list of constraint items in the FmgrInfo struct
 *		passed by the caller.
 *	3.	If there are CHECK constraints, we cache a standalone ExprContext
 *		to evaluate them in.
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/domains.c,v 1.7 2008/06/06 22:35:22 alvherre Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/typecmds.h"
#include "executor/executor.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"


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
	/* List of constraint items to check */
	List	   *constraint_list;
	/* Context for evaluating CHECK constraints in */
	ExprContext *econtext;
	/* Memory context this cache is in */
	MemoryContext mcxt;
} DomainIOData;


/*
 * domain_state_setup - initialize the cache for a new domain type.
 */
static void
domain_state_setup(DomainIOData *my_extra, Oid domainType, bool binary,
				   MemoryContext mcxt)
{
	Oid			baseType;
	MemoryContext oldcontext;

	/* Mark cache invalid */
	my_extra->domain_type = InvalidOid;

	/* Find out the base type */
	my_extra->typtypmod = -1;
	baseType = getBaseTypeAndTypmod(domainType, &my_extra->typtypmod);
	if (baseType == domainType)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("type %s is not a domain",
						format_type_be(domainType))));

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
	oldcontext = MemoryContextSwitchTo(mcxt);
	my_extra->constraint_list = GetDomainConstraints(domainType);
	MemoryContextSwitchTo(oldcontext);

	/* We don't make an ExprContext until needed */
	my_extra->econtext = NULL;
	my_extra->mcxt = mcxt;

	/* Mark cache valid */
	my_extra->domain_type = domainType;
}

/*
 * domain_check_input - apply the cached checks.
 *
 * This is extremely similar to ExecEvalCoerceToDomain in execQual.c.
 */
static void
domain_check_input(Datum value, bool isnull, DomainIOData *my_extra)
{
	ExprContext *econtext = my_extra->econtext;
	ListCell   *l;

	foreach(l, my_extra->constraint_list)
	{
		DomainConstraintState *con = (DomainConstraintState *) lfirst(l);

		switch (con->constrainttype)
		{
			case DOM_CONSTRAINT_NOTNULL:
				if (isnull)
					ereport(ERROR,
							(errcode(ERRCODE_NOT_NULL_VIOLATION),
							 errmsg("domain %s does not allow null values",
									format_type_be(my_extra->domain_type))));
				break;
			case DOM_CONSTRAINT_CHECK:
				{
					Datum		conResult;
					bool		conIsNull;

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
					 * nodes.  Unlike ExecEvalCoerceToDomain, this econtext
					 * couldn't be shared with anything else, so no need to
					 * save and restore fields.
					 */
					econtext->domainValue_datum = value;
					econtext->domainValue_isNull = isnull;

					conResult = ExecEvalExprSwitchContext(con->check_expr,
														  econtext,
														  &conIsNull, NULL);

					if (!conIsNull &&
						!DatumGetBool(conResult))
						ereport(ERROR,
								(errcode(ERRCODE_CHECK_VIOLATION),
								 errmsg("value for domain %s violates check constraint \"%s\"",
										format_type_be(my_extra->domain_type),
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
	 * assuming the domain type doesn't change underneath us.
	 */
	my_extra = (DomainIOData *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL)
	{
		my_extra = (DomainIOData *) MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
													   sizeof(DomainIOData));
		domain_state_setup(my_extra, domainType, false,
						   fcinfo->flinfo->fn_mcxt);
		fcinfo->flinfo->fn_extra = (void *) my_extra;
	}
	else if (my_extra->domain_type != domainType)
		domain_state_setup(my_extra, domainType, false,
						   fcinfo->flinfo->fn_mcxt);

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
	 * assuming the domain type doesn't change underneath us.
	 */
	my_extra = (DomainIOData *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL)
	{
		my_extra = (DomainIOData *) MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
													   sizeof(DomainIOData));
		domain_state_setup(my_extra, domainType, true,
						   fcinfo->flinfo->fn_mcxt);
		fcinfo->flinfo->fn_extra = (void *) my_extra;
	}
	else if (my_extra->domain_type != domainType)
		domain_state_setup(my_extra, domainType, true,
						   fcinfo->flinfo->fn_mcxt);

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
