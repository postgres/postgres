/*-------------------------------------------------------------------------
 *
 * params.c
 *	  Support functions for plan parameter lists.
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/nodes/params.c,v 1.4 2004/12/31 21:59:55 pgsql Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "nodes/params.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"


/*
 * Copy a ParamList.
 *
 * The result is allocated in CurrentMemoryContext.
 */
ParamListInfo
copyParamList(ParamListInfo from)
{
	ParamListInfo retval;
	int			i,
				size;

	if (from == NULL)
		return NULL;

	size = 0;
	while (from[size].kind != PARAM_INVALID)
		size++;

	retval = (ParamListInfo) palloc0((size + 1) * sizeof(ParamListInfoData));

	for (i = 0; i < size; i++)
	{
		/* copy metadata */
		retval[i].kind = from[i].kind;
		if (from[i].kind == PARAM_NAMED)
			retval[i].name = pstrdup(from[i].name);
		retval[i].id = from[i].id;
		retval[i].ptype = from[i].ptype;

		/* copy value */
		retval[i].isnull = from[i].isnull;
		if (from[i].isnull)
		{
			retval[i].value = from[i].value;	/* nulls just copy */
		}
		else
		{
			int16		typLen;
			bool		typByVal;

			get_typlenbyval(from[i].ptype, &typLen, &typByVal);
			retval[i].value = datumCopy(from[i].value, typByVal, typLen);
		}
	}

	retval[size].kind = PARAM_INVALID;

	return retval;
}

/*
 * Search a ParamList for a given parameter.
 *
 * On success, returns a pointer to the parameter's entry.
 * On failure, returns NULL if noError is true, else ereports the error.
 */
ParamListInfo
lookupParam(ParamListInfo paramList, int thisParamKind,
			const char *thisParamName, AttrNumber thisParamId,
			bool noError)
{
	if (paramList != NULL)
	{
		while (paramList->kind != PARAM_INVALID)
		{
			if (thisParamKind == paramList->kind)
			{
				switch (thisParamKind)
				{
					case PARAM_NAMED:
						if (strcmp(paramList->name, thisParamName) == 0)
							return paramList;
						break;
					case PARAM_NUM:
						if (paramList->id == thisParamId)
							return paramList;
						break;
					default:
						elog(ERROR, "unrecognized paramkind: %d",
							 thisParamKind);
				}
			}
			paramList++;
		}
	}

	if (!noError)
	{
		if (thisParamKind == PARAM_NAMED)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("no value found for parameter \"%s\"",
							thisParamName)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("no value found for parameter %d",
							thisParamId)));
	}

	return NULL;
}
