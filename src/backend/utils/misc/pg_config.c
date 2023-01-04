/*-------------------------------------------------------------------------
 *
 * pg_config.c
 *		Expose same output as pg_config except as an SRF
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/misc/pg_config.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_type.h"
#include "common/config_info.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "port.h"
#include "utils/builtins.h"

Datum
pg_config(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	ConfigData *configdata;
	size_t		configdata_len;
	int			i = 0;

	/* initialize our tuplestore */
	InitMaterializedSRF(fcinfo, 0);

	configdata = get_configdata(my_exec_path, &configdata_len);
	for (i = 0; i < configdata_len; i++)
	{
		Datum		values[2];
		bool		nulls[2];

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		values[0] = CStringGetTextDatum(configdata[i].name);
		values[1] = CStringGetTextDatum(configdata[i].setting);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	return (Datum) 0;
}
