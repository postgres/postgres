#include "postgres.h"
#include "executor/spi.h"
#include "utils/syscache.h"
/*
 * This kludge is necessary because of the conflicting
 * definitions of 'DEBUG' between postgres and perl.
 * we'll live.
 */

#include "spi_internal.h"

static char* plperl_spi_status_string(int);

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
	 AV *rows;
	int i;

	result = newHV();
	rows = newAV();

	if (status == SPI_OK_UTILITY)
	{
		hv_store(result, "status", strlen("status"), newSVpv("SPI_OK_UTILITY",0), 0);
		hv_store(result, "processed", strlen("processed"), newSViv(processed), 0);
	}
	else if (status != SPI_OK_SELECT)
	{
		hv_store(result, "status", strlen("status"), newSVpv((char*)plperl_spi_status_string(status),0), 0);
		hv_store(result, "processed", strlen("processed"), newSViv(processed), 0);
	}
	else
	{
		hv_store(result, "status", strlen("status"), newSVpv((char*)plperl_spi_status_string(status),0), 0);
		hv_store(result, "processed", strlen("processed"), newSViv(processed), 0);
		if (processed)
		{
			HV *row;
			for (i = 0; i < processed; i++)
			{
				row = plperl_hash_from_tuple(tuptable->vals[i], tuptable->tupdesc);
				 av_store(rows, i, newRV_noinc((SV*)row));
			}
			hv_store(result, "rows", strlen("rows"), newRV_noinc((SV*)rows), 0);
			SPI_freetuptable(tuptable);
		}
	}
	return result;
}

static char*
plperl_spi_status_string(int status)
{
	switch(status){
		/*errors*/
		case SPI_ERROR_TYPUNKNOWN:
			return "SPI_ERROR_TYPUNKNOWN";
		case SPI_ERROR_NOOUTFUNC:
			return "SPI_ERROR_NOOUTFUNC";
		case SPI_ERROR_NOATTRIBUTE:
			return "SPI_ERROR_NOATTRIBUTE";
		case SPI_ERROR_TRANSACTION:
			return "SPI_ERROR_TRANSACTION";
		case SPI_ERROR_PARAM:
			return "SPI_ERROR_PARAM";
		case SPI_ERROR_ARGUMENT:
			return "SPI_ERROR_ARGUMENT";
		case SPI_ERROR_CURSOR:
			return "SPI_ERROR_CURSOR";
		case SPI_ERROR_UNCONNECTED:
			return "SPI_ERROR_UNCONNECTED";
		case SPI_ERROR_OPUNKNOWN:
			return "SPI_ERROR_OPUNKNOWN";
		case SPI_ERROR_COPY:
			return "SPI_ERROR_COPY";
		case SPI_ERROR_CONNECT:
			return "SPI_ERROR_CONNECT";
		/*ok*/
		case SPI_OK_CONNECT:
			return "SPI_OK_CONNECT";
		case SPI_OK_FINISH:
			return "SPI_OK_FINISH";
		case SPI_OK_FETCH:
			return "SPI_OK_FETCH";
		case SPI_OK_UTILITY:
			return "SPI_OK_UTILITY";
		case SPI_OK_SELECT:
			return "SPI_OK_SELECT";
		case SPI_OK_SELINTO:
			return "SPI_OK_SELINTO";
		case SPI_OK_INSERT:
			return "SPI_OK_INSERT";
		case SPI_OK_DELETE:
			return "SPI_OK_DELETE";
		case SPI_OK_UPDATE:
			return "SPI_OK_UPDATE";
		case SPI_OK_CURSOR:
			return "SPI_OK_CURSOR";
	}

	return "Unknown or Invalid code";
}

