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
 *	  $PostgreSQL: pgsql/src/backend/nodes/params.c,v 1.11 2009/01/01 17:23:43 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "nodes/params.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"


/*
 * Copy a ParamListInfo structure.
 *
 * The result is allocated in CurrentMemoryContext.
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
	memcpy(retval, from, size);

	/*
	 * Flat-copy is not good enough for pass-by-ref data values, so make a
	 * pass over the array to copy those.
	 */
	for (i = 0; i < retval->numParams; i++)
	{
		ParamExternData *prm = &retval->params[i];
		int16		typLen;
		bool		typByVal;

		if (prm->isnull || !OidIsValid(prm->ptype))
			continue;
		get_typlenbyval(prm->ptype, &typLen, &typByVal);
		prm->value = datumCopy(prm->value, typByVal, typLen);
	}

	return retval;
}

/*
 * Extract an array of parameter type OIDs from a ParamListInfo.
 *
 * The result is allocated in CurrentMemoryContext.
 */
void
getParamListTypes(ParamListInfo params,
				  Oid **param_types, int *num_params)
{
	Oid		   *ptypes;
	int			i;

	if (params == NULL || params->numParams <= 0)
	{
		*param_types = NULL;
		*num_params = 0;
		return;
	}

	ptypes = (Oid *) palloc(params->numParams * sizeof(Oid));
	*param_types = ptypes;
	*num_params = params->numParams;

	for (i = 0; i < params->numParams; i++)
	{
		ParamExternData *prm = &params->params[i];

		ptypes[i] = prm->ptype;
	}
}
