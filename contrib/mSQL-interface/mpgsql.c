#include <time.h>
#include <string.h>
#include <stdlib.h>
#include "msql.h"
#include "libpq-fe.h"

#define HNDMAX 10

PGconn	   *PGh[HNDMAX] = {
	NULL, NULL, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL, NULL
};

#define E_NOHANDLERS 0

char	   *msqlErrors[] = {
	"Out of database handlers."
};

char		msqlErrMsg[BUFSIZ],
		   *tfrom = "dunno";
PGresult   *queryres = NULL;

int
msqlConnect(char *host)
{
	int			count;

	for (count = 0; count < HNDMAX; count++)
		if (PGh[count] == NULL)
			break;

	if (count == HNDMAX)
	{
		strncpy(msqlErrMsg, msqlErrors[E_NOHANDLERS], BUFSIZ);
		return -1;
	}

	PGh[count] = malloc(sizeof(PGconn));
	PGh[count]->pghost = host ? strdup(host) : NULL;
	return count;
}

int
msqlSelectDB(int handle, char *dbname)
{
	char	   *options = calloc(1, BUFSIZ);
	char	   *e = getenv("PG_OPTIONS");

	if (e == NULL)
		e = "";

	if (PGh[handle]->pghost)
	{
		strcat(options, "host=");
		strncat(options, PGh[handle]->pghost, BUFSIZ);
		strncat(options, " ", BUFSIZ);
		free(PGh[handle]->pghost);
		PGh[handle]->pghost = NULL;
	}
	strncat(options, "dbname=", BUFSIZ);
	strncat(options, dbname, BUFSIZ);
	strncat(options, " ", BUFSIZ);
	strncat(options, e, BUFSIZ);
	free(PGh[handle]);
	PGh[handle] = PQconnectdb(options);
	free(options);
	strncpy(msqlErrMsg, PQerrorMessage(PGh[handle]), BUFSIZ);
	return (PQstatus(PGh[handle]) == CONNECTION_BAD ? -1 : 0);
}

int
msqlQuery(int handle, char *query)
{
	char	   *tq = strdup(query);
	char	   *p = tq;
	PGresult   *res;
	PGconn	   *conn = PGh[handle];
	ExecStatusType rcode;

	res = PQexec(conn, p);

	rcode = PQresultStatus(res);

	if (rcode == PGRES_TUPLES_OK)
	{
		queryres = res;
		return PQntuples(res);
	}
	else if (rcode == PGRES_FATAL_ERROR || rcode == PGRES_NONFATAL_ERROR)
	{
		PQclear(res);
		queryres = NULL;
		return -1;
	}
	else
	{
		PQclear(res);
		queryres = NULL;
		return 0;
	}
}

int
msqlCreateDB(int a, char *b)
{
	char		tbuf[BUFSIZ];

	sprintf(tbuf, "create database %s", b);
	return msqlQuery(a, tbuf) >= 0 ? 0 : -1;
}

int
msqlDropDB(int a, char *b)
{
	char		tbuf[BUFSIZ];

	sprintf(tbuf, "drop database %s", b);
	return msqlQuery(a, tbuf) >= 0 ? 0 : -1;
}

int
msqlShutdown(int a)
{
}

int
msqlGetProtoInfo(void)
{
}

int
msqlReloadAcls(int a)
{
}

char *
msqlGetServerInfo(void)
{
}

char *
msqlGetHostInfo(void)
{
}

char *
msqlUnixTimeToDate(time_t date)
{
}

char *
msqlUnixTimeToTime(time_t time)
{
}

void
msqlClose(int a)
{
	PQfinish(PGh[a]);
	PGh[a] = NULL;
	if (queryres)
	{
		free(queryres);
		queryres = NULL;
	}
}

void
msqlDataSeek(m_result * result, int count)
{
	int			c;

	result->cursor = result->queryData;
	for (c = 1; c < count; c++)
		if (result->cursor->next)
			result->cursor = result->cursor->next;
}

void
msqlFieldSeek(m_result * result, int count)
{
	int			c;

	result->fieldCursor = result->fieldData;
	for (c = 1; c < count; c++)
		if (result->fieldCursor->next)
			result->fieldCursor = result->fieldCursor->next;
}

void
msqlFreeResult(m_result * result)
{
	if (result)
	{
		/* Clears fields */
		free(result->fieldData);
		result->cursor = result->queryData;
		while (result->cursor)
		{
			int			c;
			m_row		m = result->cursor->data;

			for (c = 0; m[c]; c++)
				free(m[c]);

			result->cursor = result->cursor->next;
		}
		free(result->queryData);
		free(result);
	}
}

m_row
msqlFetchRow(m_result * row)
{
	m_data	   *r = row->cursor;

	if (r)
	{
		row->cursor = row->cursor->next;
		return (m_row) r->data;
	}
	return (m_row) NULL;
}

m_seq *
msqlGetSequenceInfo(int a, char *b)
{
}

m_field    *
msqlFetchField(m_result * mr)
{
	m_field    *m = (m_field *) mr->fieldCursor;

	if (m)
	{
		mr->fieldCursor = mr->fieldCursor->next;
		return m;
	}
	return NULL;
}

m_result   *
msqlListDBs(int a)
{
	m_result   *m;

	if (msqlQuery(a, "select datname from pg_database") > 0)
	{
		m = msqlStoreResult();
		return m;
	}
	else
		return NULL;
}

m_result   *
msqlListTables(int a)
{
	m_result   *m;
	char		tbuf[BUFSIZ];

	sprintf(tbuf, "select relname from pg_class where relkind='r' and relowner=%d", getuid());
	if (msqlQuery(a, tbuf) > 0)
	{
		m = msqlStoreResult();
		return m;
	}
	else
		return NULL;
}

m_result   *
msqlListFields(int a, char *b)
{

}

m_result   *
msqlListIndex(int a, char *b, char *c)
{
	m_result   *m;
	char		tbuf[BUFSIZ];

	sprintf(tbuf, "select relname from pg_class where relkind='i' and relowner=%d", getuid());
	if (msqlQuery(a, tbuf) > 0)
	{
		m = msqlStoreResult();
		return m;
	}
	else
		return NULL;
}

m_result   *
msqlStoreResult(void)
{
	if (queryres)
	{
		m_result   *mr = malloc(sizeof(m_result));
		m_fdata    *mf;
		m_data	   *md;
		int			count;

		mr->queryData = mr->cursor = NULL;
		mr->numRows = PQntuples(queryres);
		mr->numFields = PQnfields(queryres);

		mf = calloc(PQnfields(queryres), sizeof(m_fdata));
		for (count = 0; count < PQnfields(queryres); count++)
		{
			(m_fdata *) (mf + count)->field.name = strdup(PQfname(queryres, count));
			(m_fdata *) (mf + count)->field.table = tfrom;
			(m_fdata *) (mf + count)->field.type = CHAR_TYPE;
			(m_fdata *) (mf + count)->field.length = PQfsize(queryres, count);
			(m_fdata *) (mf + count)->next = (m_fdata *) (mf + count + 1);
		}
		(m_fdata *) (mf + count - 1)->next = NULL;

		md = calloc(PQntuples(queryres), sizeof(m_data));
		for (count = 0; count < PQntuples(queryres); count++)
		{
			m_row		rows = calloc(PQnfields(queryres) * sizeof(m_row) + 1, 1);
			int			c;

			for (c = 0; c < PQnfields(queryres); c++)
				rows[c] = strdup(PQgetvalue(queryres, count, c));
			(m_data *) (md + count)->data = rows;

			(m_data *) (md + count)->width = PQnfields(queryres);
			(m_data *) (md + count)->next = (m_data *) (md + count + 1);
		}
		(m_data *) (md + count - 1)->next = NULL;

		mr->queryData = mr->cursor = md;
		mr->fieldCursor = mr->fieldData = mf;

		return mr;
	}
	else
		return NULL;
}

time_t
msqlDateToUnixTime(char *a)
{
}

time_t
msqlTimeToUnixTime(char *b)
{
}

char *
msql_tmpnam(void)
{
	return tmpnam("/tmp/msql.XXXXXX");
}

int
msqlLoadConfigFile(char *a)
{
}
