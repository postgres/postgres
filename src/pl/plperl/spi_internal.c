#include "postgres.h"
#include "executor/spi.h"
#include "utils/syscache.h"
/*
 * This kludge is necessary because of the conflicting
 * definitions of 'DEBUG' between postgres and perl.
 * we'll live.
 */

#include "spi_internal.h"

static HV* plperl_spi_execute_fetch_result(SPITupleTable*, int, int );


int
spi_DEBUG(void)
{
	return DEBUG2;
}

int
spi_LOG(void)
{
	return LOG;
}

int
spi_INFO(void)
{
	return INFO;
}

int
spi_NOTICE(void)
{
	return NOTICE;
}

int
spi_WARNING(void)
{
	return WARNING;
}

int
spi_ERROR(void)
{
	return ERROR;
}

HV*
plperl_spi_exec(char* query, int limit)
{
	HV *ret_hv;
	int spi_rv;

	spi_rv = SPI_exec(query, limit);
	ret_hv=plperl_spi_execute_fetch_result(SPI_tuptable, SPI_processed, spi_rv);

	return ret_hv;
}

static HV*
plperl_hash_from_tuple(HeapTuple tuple, TupleDesc tupdesc)
{
	int	i;
	char	*attname;
	char	*attdata;

	HV *array;

	array = newHV();

	for (i = 0; i < tupdesc->natts; i++) {
		/************************************************************
		* Get the attribute name
		************************************************************/
		attname = tupdesc->attrs[i]->attname.data;

		/************************************************************
		* Get the attributes value
		************************************************************/
		attdata = SPI_getvalue(tuple, tupdesc, i+1);
		if(attdata)
		hv_store(array, attname, strlen(attname), newSVpv(attdata,0), 0);
		else
			hv_store(array, attname, strlen(attname), newSVpv("undef",0), 0);
	}
	return array;
}

static HV*
plperl_spi_execute_fetch_result(SPITupleTable *tuptable, int processed, int status)
{
	HV *result;

	result = newHV();

	hv_store(result, "status", strlen("status"),
			 newSVpv((char*)SPI_result_code_string(status),0), 0);
	hv_store(result, "processed", strlen("processed"),
			 newSViv(processed), 0);

	if (status == SPI_OK_SELECT)
	{
		if (processed)
		{
			AV *rows;
			HV *row;
			int i;

			rows = newAV();
			for (i = 0; i < processed; i++)
			{
				row = plperl_hash_from_tuple(tuptable->vals[i], tuptable->tupdesc);
				av_store(rows, i, newRV_noinc((SV*)row));
			}
			hv_store(result, "rows", strlen("rows"),
					 newRV_noinc((SV*)rows), 0);
		}
	}

	SPI_freetuptable(tuptable);

	return result;
}
