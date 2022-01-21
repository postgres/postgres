/*-------------------------------------------------------------------------
 *
 * params.c
 *	  Support for finding the values associated with Param nodes.
 *
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/nodes/params.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xact.h"
#include "fmgr.h"
#include "mb/stringinfo_mb.h"
#include "nodes/params.h"
#include "parser/parse_node.h"
#include "storage/shmem.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"


static void paramlist_parser_setup(ParseState *pstate, void *arg);
static Node *paramlist_param_ref(ParseState *pstate, ParamRef *pref);


/*
 * Allocate and initialize a new ParamListInfo structure.
 *
 * To make a new structure for the "dynamic" way (with hooks), pass 0 for
 * numParams and set numParams manually.
 *
 * A default parserSetup function is supplied automatically.  Callers may
 * override it if they choose.  (Note that most use-cases for ParamListInfos
 * will never use the parserSetup function anyway.)
 */
ParamListInfo
makeParamList(int numParams)
{
	ParamListInfo retval;
	Size		size;

	size = offsetof(ParamListInfoData, params) +
		numParams * sizeof(ParamExternData);

	retval = (ParamListInfo) palloc(size);
	retval->paramFetch = NULL;
	retval->paramFetchArg = NULL;
	retval->paramCompile = NULL;
	retval->paramCompileArg = NULL;
	retval->parserSetup = paramlist_parser_setup;
	retval->parserSetupArg = (void *) retval;
	retval->paramValuesStr = NULL;
	retval->numParams = numParams;

	return retval;
}

/*
 * Copy a ParamListInfo structure.
 *
 * The result is allocated in CurrentMemoryContext.
 *
 * Note: the intent of this function is to make a static, self-contained
 * set of parameter values.  If dynamic parameter hooks are present, we
 * intentionally do not copy them into the result.  Rather, we forcibly
 * instantiate all available parameter values and copy the datum values.
 *
 * paramValuesStr is not copied, either.
 */
ParamListInfo
copyParamList(ParamListInfo from)
{
	ParamListInfo retval;

	if (from == NULL || from->numParams <= 0)
		return NULL;

	retval = makeParamList(from->numParams);

	for (int i = 0; i < from->numParams; i++)
	{
		ParamExternData *oprm;
		ParamExternData *nprm = &retval->params[i];
		ParamExternData prmdata;
		int16		typLen;
		bool		typByVal;

		/* give hook a chance in case parameter is dynamic */
		if (from->paramFetch != NULL)
			oprm = from->paramFetch(from, i + 1, false, &prmdata);
		else
			oprm = &from->params[i];

		/* flat-copy the parameter info */
		*nprm = *oprm;

		/* need datumCopy in case it's a pass-by-reference datatype */
		if (nprm->isnull || !OidIsValid(nprm->ptype))
			continue;
		get_typlenbyval(nprm->ptype, &typLen, &typByVal);
		nprm->value = datumCopy(nprm->value, typByVal, typLen);
	}

	return retval;
}


/*
 * Set up to parse a query containing references to parameters
 * sourced from a ParamListInfo.
 */
static void
paramlist_parser_setup(ParseState *pstate, void *arg)
{
	pstate->p_paramref_hook = paramlist_param_ref;
	/* no need to use p_coerce_param_hook */
	pstate->p_ref_hook_state = arg;
}

/*
 * Transform a ParamRef using parameter type data from a ParamListInfo.
 */
static Node *
paramlist_param_ref(ParseState *pstate, ParamRef *pref)
{
	ParamListInfo paramLI = (ParamListInfo) pstate->p_ref_hook_state;
	int			paramno = pref->number;
	ParamExternData *prm;
	ParamExternData prmdata;
	Param	   *param;

	/* check parameter number is valid */
	if (paramno <= 0 || paramno > paramLI->numParams)
		return NULL;

	/* give hook a chance in case parameter is dynamic */
	if (paramLI->paramFetch != NULL)
		prm = paramLI->paramFetch(paramLI, paramno, false, &prmdata);
	else
		prm = &paramLI->params[paramno - 1];

	if (!OidIsValid(prm->ptype))
		return NULL;

	param = makeNode(Param);
	param->paramkind = PARAM_EXTERN;
	param->paramid = paramno;
	param->paramtype = prm->ptype;
	param->paramtypmod = -1;
	param->paramcollid = get_typcollation(param->paramtype);
	param->location = pref->location;

	return (Node *) param;
}

/*
 * Estimate the amount of space required to serialize a ParamListInfo.
 */
Size
EstimateParamListSpace(ParamListInfo paramLI)
{
	int			i;
	Size		sz = sizeof(int);

	if (paramLI == NULL || paramLI->numParams <= 0)
		return sz;

	for (i = 0; i < paramLI->numParams; i++)
	{
		ParamExternData *prm;
		ParamExternData prmdata;
		Oid			typeOid;
		int16		typLen;
		bool		typByVal;

		/* give hook a chance in case parameter is dynamic */
		if (paramLI->paramFetch != NULL)
			prm = paramLI->paramFetch(paramLI, i + 1, false, &prmdata);
		else
			prm = &paramLI->params[i];

		typeOid = prm->ptype;

		sz = add_size(sz, sizeof(Oid)); /* space for type OID */
		sz = add_size(sz, sizeof(uint16));	/* space for pflags */

		/* space for datum/isnull */
		if (OidIsValid(typeOid))
			get_typlenbyval(typeOid, &typLen, &typByVal);
		else
		{
			/* If no type OID, assume by-value, like copyParamList does. */
			typLen = sizeof(Datum);
			typByVal = true;
		}
		sz = add_size(sz,
					  datumEstimateSpace(prm->value, prm->isnull, typByVal, typLen));
	}

	return sz;
}

/*
 * Serialize a ParamListInfo structure into caller-provided storage.
 *
 * We write the number of parameters first, as a 4-byte integer, and then
 * write details for each parameter in turn.  The details for each parameter
 * consist of a 4-byte type OID, 2 bytes of flags, and then the datum as
 * serialized by datumSerialize().  The caller is responsible for ensuring
 * that there is enough storage to store the number of bytes that will be
 * written; use EstimateParamListSpace to find out how many will be needed.
 * *start_address is updated to point to the byte immediately following those
 * written.
 *
 * RestoreParamList can be used to recreate a ParamListInfo based on the
 * serialized representation; this will be a static, self-contained copy
 * just as copyParamList would create.
 *
 * paramValuesStr is not included.
 */
void
SerializeParamList(ParamListInfo paramLI, char **start_address)
{
	int			nparams;
	int			i;

	/* Write number of parameters. */
	if (paramLI == NULL || paramLI->numParams <= 0)
		nparams = 0;
	else
		nparams = paramLI->numParams;
	memcpy(*start_address, &nparams, sizeof(int));
	*start_address += sizeof(int);

	/* Write each parameter in turn. */
	for (i = 0; i < nparams; i++)
	{
		ParamExternData *prm;
		ParamExternData prmdata;
		Oid			typeOid;
		int16		typLen;
		bool		typByVal;

		/* give hook a chance in case parameter is dynamic */
		if (paramLI->paramFetch != NULL)
			prm = paramLI->paramFetch(paramLI, i + 1, false, &prmdata);
		else
			prm = &paramLI->params[i];

		typeOid = prm->ptype;

		/* Write type OID. */
		memcpy(*start_address, &typeOid, sizeof(Oid));
		*start_address += sizeof(Oid);

		/* Write flags. */
		memcpy(*start_address, &prm->pflags, sizeof(uint16));
		*start_address += sizeof(uint16);

		/* Write datum/isnull. */
		if (OidIsValid(typeOid))
			get_typlenbyval(typeOid, &typLen, &typByVal);
		else
		{
			/* If no type OID, assume by-value, like copyParamList does. */
			typLen = sizeof(Datum);
			typByVal = true;
		}
		datumSerialize(prm->value, prm->isnull, typByVal, typLen,
					   start_address);
	}
}

/*
 * Copy a ParamListInfo structure.
 *
 * The result is allocated in CurrentMemoryContext.
 *
 * Note: the intent of this function is to make a static, self-contained
 * set of parameter values.  If dynamic parameter hooks are present, we
 * intentionally do not copy them into the result.  Rather, we forcibly
 * instantiate all available parameter values and copy the datum values.
 */
ParamListInfo
RestoreParamList(char **start_address)
{
	ParamListInfo paramLI;
	int			nparams;

	memcpy(&nparams, *start_address, sizeof(int));
	*start_address += sizeof(int);

	paramLI = makeParamList(nparams);

	for (int i = 0; i < nparams; i++)
	{
		ParamExternData *prm = &paramLI->params[i];

		/* Read type OID. */
		memcpy(&prm->ptype, *start_address, sizeof(Oid));
		*start_address += sizeof(Oid);

		/* Read flags. */
		memcpy(&prm->pflags, *start_address, sizeof(uint16));
		*start_address += sizeof(uint16);

		/* Read datum/isnull. */
		prm->value = datumRestore(start_address, &prm->isnull);
	}

	return paramLI;
}

/*
 * BuildParamLogString
 *		Return a string that represents the parameter list, for logging.
 *
 * If caller already knows textual representations for some parameters, it can
 * pass an array of exactly params->numParams values as knownTextValues, which
 * can contain NULLs for any unknown individual values.  NULL can be given if
 * no parameters are known.
 *
 * If maxlen is >= 0, that's the maximum number of bytes of any one
 * parameter value to be printed; an ellipsis is added if the string is
 * longer.  (Added quotes are not considered in this calculation.)
 */
char *
BuildParamLogString(ParamListInfo params, char **knownTextValues, int maxlen)
{
	MemoryContext tmpCxt,
				oldCxt;
	StringInfoData buf;

	/*
	 * NB: think not of returning params->paramValuesStr!  It may have been
	 * generated with a different maxlen, and so be unsuitable.  Besides that,
	 * this is the function used to create that string.
	 */

	/*
	 * No work if the param fetch hook is in use.  Also, it's not possible to
	 * do this in an aborted transaction.  (It might be possible to improve on
	 * this last point when some knownTextValues exist, but it seems tricky.)
	 */
	if (params->paramFetch != NULL ||
		IsAbortedTransactionBlockState())
		return NULL;

	/* Initialize the output stringinfo, in caller's memory context */
	initStringInfo(&buf);

	/* Use a temporary context to call output functions, just in case */
	tmpCxt = AllocSetContextCreate(CurrentMemoryContext,
								   "BuildParamLogString",
								   ALLOCSET_DEFAULT_SIZES);
	oldCxt = MemoryContextSwitchTo(tmpCxt);

	for (int paramno = 0; paramno < params->numParams; paramno++)
	{
		ParamExternData *param = &params->params[paramno];

		appendStringInfo(&buf,
						 "%s$%d = ",
						 paramno > 0 ? ", " : "",
						 paramno + 1);

		if (param->isnull || !OidIsValid(param->ptype))
			appendStringInfoString(&buf, "NULL");
		else
		{
			if (knownTextValues != NULL && knownTextValues[paramno] != NULL)
				appendStringInfoStringQuoted(&buf, knownTextValues[paramno],
											 maxlen);
			else
			{
				Oid			typoutput;
				bool		typisvarlena;
				char	   *pstring;

				getTypeOutputInfo(param->ptype, &typoutput, &typisvarlena);
				pstring = OidOutputFunctionCall(typoutput, param->value);
				appendStringInfoStringQuoted(&buf, pstring, maxlen);
			}
		}
	}

	MemoryContextSwitchTo(oldCxt);
	MemoryContextDelete(tmpCxt);

	return buf.data;
}

/*
 * ParamsErrorCallback - callback for printing parameters in error context
 *
 * Note that this is a no-op unless BuildParamLogString has been called
 * beforehand.
 */
void
ParamsErrorCallback(void *arg)
{
	ParamsErrorCbData *data = (ParamsErrorCbData *) arg;

	if (data == NULL ||
		data->params == NULL ||
		data->params->paramValuesStr == NULL)
		return;

	if (data->portalName && data->portalName[0] != '\0')
		errcontext("portal \"%s\" with parameters: %s",
				   data->portalName, data->params->paramValuesStr);
	else
		errcontext("unnamed portal with parameters: %s",
				   data->params->paramValuesStr);
}
