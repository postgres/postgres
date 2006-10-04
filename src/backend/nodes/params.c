/*-------------------------------------------------------------------------
 *
 * params.c
 *	  Support for finding the values associated with Param nodes.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/nodes/params.c,v 1.7 2006/10/04 00:29:53 momjian Exp $
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
