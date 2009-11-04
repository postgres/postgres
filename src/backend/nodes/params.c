/*-------------------------------------------------------------------------
 *
 * params.c
 *	  Support for finding the values associated with Param nodes.
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/nodes/params.c,v 1.12 2009/11/04 22:26:06 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "nodes/params.h"
#include "parser/parse_param.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"


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
copyParamList(ParamListInfo from)
{
	ParamListInfo retval;
	Size		size;
	int			i;

	if (from == NULL || from->numParams <= 0)
		return NULL;

	/* sizeof(ParamListInfoData) includes the first array element */
	size = sizeof(ParamListInfoData) +
		(from->numParams - 1) *sizeof(ParamExternData);

	retval = (ParamListInfo) palloc(size);
	retval->paramFetch = NULL;
	retval->paramFetchArg = NULL;
	retval->parserSetup = NULL;
	retval->parserSetupArg = NULL;
	retval->numParams = from->numParams;

	for (i = 0; i < from->numParams; i++)
	{
		ParamExternData *oprm = &from->params[i];
		ParamExternData *nprm = &retval->params[i];
		int16		typLen;
		bool		typByVal;

		/* give hook a chance in case parameter is dynamic */
		if (!OidIsValid(oprm->ptype) && from->paramFetch != NULL)
			(*from->paramFetch) (from, i+1);

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
 * Set up the parser to treat the given list of run-time parameters
 * as available external parameters during parsing of a new query.
 *
 * Note that the parser doesn't actually care about the *values* of the given
 * parameters, only about their *types*.  Also, the code that originally
 * provided the ParamListInfo may have provided a setupHook, which should
 * override applying parse_fixed_parameters().
 */
void
setupParserWithParamList(struct ParseState *pstate,
						 ParamListInfo params)
{
	if (params == NULL)			/* no params, nothing to do */
		return;

	/* If there is a parserSetup hook, it gets to do this */
	if (params->parserSetup != NULL)
	{
		(*params->parserSetup) (pstate, params->parserSetupArg);
		return;
	}

	/* Else, treat any available parameters as being of fixed type */
	if (params->numParams > 0)
	{
		Oid		   *ptypes;
		int			i;

		ptypes = (Oid *) palloc(params->numParams * sizeof(Oid));
		for (i = 0; i < params->numParams; i++)
		{
			ParamExternData *prm = &params->params[i];

			/* give hook a chance in case parameter is dynamic */
			if (!OidIsValid(prm->ptype) && params->paramFetch != NULL)
				(*params->paramFetch) (params, i+1);

			ptypes[i] = prm->ptype;
		}
		parse_fixed_parameters(pstate, ptypes, params->numParams);
	}
}
