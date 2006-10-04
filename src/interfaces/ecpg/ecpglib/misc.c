/* $PostgreSQL: pgsql/src/interfaces/ecpg/ecpglib/misc.c,v 1.32 2006/10/04 00:30:11 momjian Exp $ */

#define POSTGRES_ECPG_INTERNAL
#include "postgres_fe.h"

#include <limits.h>
#include <unistd.h>
#ifdef ENABLE_THREAD_SAFETY
#include <pthread.h>
#endif
#include "ecpgtype.h"
#include "ecpglib.h"
#include "ecpgerrno.h"
#include "extern.h"
#include "sqlca.h"
#include "pgtypes_numeric.h"
#include "pgtypes_date.h"
#include "pgtypes_timestamp.h"
#include "pgtypes_interval.h"

#ifdef HAVE_LONG_LONG_INT_64
#ifndef LONG_LONG_MIN
#ifdef LLONG_MIN
#define LONG_LONG_MIN LLONG_MIN
#else
#define LONG_LONG_MIN LONGLONG_MIN
#endif
#endif
#endif

static struct sqlca_t sqlca_init =
{
	{
		'S', 'Q', 'L', 'C', 'A', ' ', ' ', ' '
	},
	sizeof(struct sqlca_t),
	0,
	{
		0,
		{
			0
		}
	},
	{
		'N', 'O', 'T', ' ', 'S', 'E', 'T', ' '
	},
	{
		0, 0, 0, 0, 0, 0
	},
	{
		0, 0, 0, 0, 0, 0, 0, 0
	},
	{
		'0', '0', '0', '0', '0'
	}
};

#ifdef ENABLE_THREAD_SAFETY
static pthread_key_t sqlca_key;
static pthread_once_t sqlca_key_once = PTHREAD_ONCE_INIT;
#else
static struct sqlca_t sqlca =
{
	{
		'S', 'Q', 'L', 'C', 'A', ' ', ' ', ' '
	},
	sizeof(struct sqlca_t),
	0,
	{
		0,
		{
			0
		}
	},
	{
		'N', 'O', 'T', ' ', 'S', 'E', 'T', ' '
	},
	{
		0, 0, 0, 0, 0, 0
	},
	{
		0, 0, 0, 0, 0, 0, 0, 0
	},
	{
		'0', '0', '0', '0', '0'
	}
};
#endif

#ifdef ENABLE_THREAD_SAFETY
static pthread_mutex_t debug_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t debug_init_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif
static int	simple_debug = 0;
static FILE *debugstream = NULL;

void
ECPGinit_sqlca(struct sqlca_t * sqlca)
{
	memcpy((char *) sqlca, (char *) &sqlca_init, sizeof(struct sqlca_t));
}

bool
ECPGinit(const struct connection * con, const char *connection_name, const int lineno)
{
	struct sqlca_t *sqlca = ECPGget_sqlca();

	ECPGinit_sqlca(sqlca);
	if (con == NULL)
	{
		ECPGraise(lineno, ECPG_NO_CONN, ECPG_SQLSTATE_CONNECTION_DOES_NOT_EXIST,
				  connection_name ? connection_name : "NULL");
		return (false);
	}

	return (true);
}

#ifdef ENABLE_THREAD_SAFETY
static void
ecpg_sqlca_key_destructor(void *arg)
{
	if (arg != NULL)
		free(arg);				/* sqlca structure allocated in ECPGget_sqlca */
}

static void
ecpg_sqlca_key_init(void)
{
	pthread_key_create(&sqlca_key, ecpg_sqlca_key_destructor);
}
#endif

struct sqlca_t *
ECPGget_sqlca(void)
{
#ifdef ENABLE_THREAD_SAFETY
	struct sqlca_t *sqlca;

	pthread_once(&sqlca_key_once, ecpg_sqlca_key_init);

	sqlca = pthread_getspecific(sqlca_key);
	if (sqlca == NULL)
	{
		sqlca = malloc(sizeof(struct sqlca_t));
		ECPGinit_sqlca(sqlca);
		pthread_setspecific(sqlca_key, sqlca);
	}
	return (sqlca);
#else
	return (&sqlca);
#endif
}

bool
ECPGstatus(int lineno, const char *connection_name)
{
	struct connection *con = ECPGget_connection(connection_name);

	if (!ECPGinit(con, connection_name, lineno))
		return (false);

	/* are we connected? */
	if (con->connection == NULL)
	{
		ECPGraise(lineno, ECPG_NOT_CONN, ECPG_SQLSTATE_ECPG_INTERNAL_ERROR, con->name);
		return false;
	}

	return (true);
}

bool
ECPGtrans(int lineno, const char *connection_name, const char *transaction)
{
	PGresult   *res;
	struct connection *con = ECPGget_connection(connection_name);

	if (!ECPGinit(con, connection_name, lineno))
		return (false);

	ECPGlog("ECPGtrans line %d action = %s connection = %s\n", lineno, transaction, con ? con->name : "(nil)");

	/* if we have no connection we just simulate the command */
	if (con && con->connection)
	{
		/*
		 * If we got a transaction command but have no open transaction, we
		 * have to start one, unless we are in autocommit, where the
		 * developers have to take care themselves. However, if the command is
		 * a begin statement, we just execute it once.
		 */
		if (con->committed && !con->autocommit && strncmp(transaction, "begin", 5) != 0 && strncmp(transaction, "start", 5) != 0)
		{
			res = PQexec(con->connection, "begin transaction");
			if (res == NULL || PQresultStatus(res) != PGRES_COMMAND_OK)
			{
				ECPGraise(lineno, ECPG_TRANS, ECPG_SQLSTATE_TRANSACTION_RESOLUTION_UNKNOWN, NULL);
				return FALSE;
			}
			PQclear(res);
		}

		res = PQexec(con->connection, transaction);
		if (res == NULL || PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			ECPGraise(lineno, ECPG_TRANS, ECPG_SQLSTATE_TRANSACTION_RESOLUTION_UNKNOWN, NULL);
			return FALSE;
		}
		PQclear(res);
	}

	if (strcmp(transaction, "commit") == 0 || strcmp(transaction, "rollback") == 0)
		con->committed = true;
	else
		con->committed = false;

	return true;
}


void
ECPGdebug(int n, FILE *dbgs)
{
#ifdef ENABLE_THREAD_SAFETY
	pthread_mutex_lock(&debug_init_mutex);
#endif

	simple_debug = n;
	debugstream = dbgs;
	ECPGlog("ECPGdebug: set to %d\n", simple_debug);

#ifdef ENABLE_THREAD_SAFETY
	pthread_mutex_unlock(&debug_init_mutex);
#endif
}

void
ECPGlog(const char *format,...)
{
	va_list		ap;
	struct sqlca_t *sqlca = ECPGget_sqlca();

#ifdef ENABLE_THREAD_SAFETY
	pthread_mutex_lock(&debug_mutex);
#endif

	if (simple_debug)
	{
		int			bufsize = strlen(format) + 100;
		char	   *f = (char *) malloc(bufsize);

		if (f == NULL)
		{
#ifdef ENABLE_THREAD_SAFETY
			pthread_mutex_unlock(&debug_mutex);
#endif
			return;
		}

		/*
		 * regression tests set this environment variable to get the same
		 * output for every run.
		 */
		if (getenv("ECPG_REGRESSION"))
			snprintf(f, bufsize, "[NO_PID]: %s", format);
		else
			snprintf(f, bufsize, "[%d]: %s", (int) getpid(), format);

		va_start(ap, format);
		vfprintf(debugstream, f, ap);
		va_end(ap);

		/* dump out internal sqlca variables */
		if (getenv("ECPG_REGRESSION"))
			fprintf(debugstream, "[NO_PID]: sqlca: code: %ld, state: %s\n",
					sqlca->sqlcode, sqlca->sqlstate);

		fflush(debugstream);

		ECPGfree(f);
	}

#ifdef ENABLE_THREAD_SAFETY
	pthread_mutex_unlock(&debug_mutex);
#endif
}

void
ECPGset_noind_null(enum ECPGttype type, void *ptr)
{
	switch (type)
	{
		case ECPGt_char:
		case ECPGt_unsigned_char:
			*((char *) ptr) = '\0';
			break;
		case ECPGt_short:
		case ECPGt_unsigned_short:
			*((short int *) ptr) = SHRT_MIN;
			break;
		case ECPGt_int:
		case ECPGt_unsigned_int:
			*((int *) ptr) = INT_MIN;
			break;
		case ECPGt_long:
		case ECPGt_unsigned_long:
		case ECPGt_date:
			*((long *) ptr) = LONG_MIN;
			break;
#ifdef HAVE_LONG_LONG_INT_64
		case ECPGt_long_long:
		case ECPGt_unsigned_long_long:
			*((long long *) ptr) = LONG_LONG_MIN;
			break;
#endif   /* HAVE_LONG_LONG_INT_64 */
		case ECPGt_float:
			memset((char *) ptr, 0xff, sizeof(float));
			break;
		case ECPGt_double:
			memset((char *) ptr, 0xff, sizeof(double));
			break;
		case ECPGt_varchar:
			*(((struct ECPGgeneric_varchar *) ptr)->arr) = 0x00;
			((struct ECPGgeneric_varchar *) ptr)->len = 0;
			break;
		case ECPGt_decimal:
			memset((char *) ptr, 0, sizeof(decimal));
			((decimal *) ptr)->sign = NUMERIC_NAN;
			break;
		case ECPGt_numeric:
			memset((char *) ptr, 0, sizeof(numeric));
			((numeric *) ptr)->sign = NUMERIC_NAN;
			break;
		case ECPGt_interval:
			memset((char *) ptr, 0xff, sizeof(interval));
			break;
		case ECPGt_timestamp:
			memset((char *) ptr, 0xff, sizeof(timestamp));
			break;
		default:
			break;
	}
}

static bool
_check(unsigned char *ptr, int length)
{
	for (; length > 0 && ptr[--length] == 0xff;);
	if (length <= 0)
		return true;
	return false;
}

bool
ECPGis_noind_null(enum ECPGttype type, void *ptr)
{
	switch (type)
	{
		case ECPGt_char:
		case ECPGt_unsigned_char:
			if (*((char *) ptr) == '\0')
				return true;
			break;
		case ECPGt_short:
		case ECPGt_unsigned_short:
			if (*((short int *) ptr) == SHRT_MIN)
				return true;
			break;
		case ECPGt_int:
		case ECPGt_unsigned_int:
			if (*((int *) ptr) == INT_MIN)
				return true;
			break;
		case ECPGt_long:
		case ECPGt_unsigned_long:
		case ECPGt_date:
			if (*((long *) ptr) == LONG_MIN)
				return true;
			break;
#ifdef HAVE_LONG_LONG_INT_64
		case ECPGt_long_long:
		case ECPGt_unsigned_long_long:
			if (*((long long *) ptr) == LONG_LONG_MIN)
				return true;
			break;
#endif   /* HAVE_LONG_LONG_INT_64 */
		case ECPGt_float:
			return (_check(ptr, sizeof(float)));
			break;
		case ECPGt_double:
			return (_check(ptr, sizeof(double)));
			break;
		case ECPGt_varchar:
			if (*(((struct ECPGgeneric_varchar *) ptr)->arr) == 0x00)
				return true;
			break;
		case ECPGt_decimal:
			if (((decimal *) ptr)->sign == NUMERIC_NAN)
				return true;
			break;
		case ECPGt_numeric:
			if (((numeric *) ptr)->sign == NUMERIC_NAN)
				return true;
			break;
		case ECPGt_interval:
			return (_check(ptr, sizeof(interval)));
			break;
		case ECPGt_timestamp:
			return (_check(ptr, sizeof(timestamp)));
			break;
		default:
			break;
	}

	return false;
}
