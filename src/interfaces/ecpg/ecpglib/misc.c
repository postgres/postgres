/* $PostgreSQL: pgsql/src/interfaces/ecpg/ecpglib/misc.c,v 1.41 2007/11/15 21:14:45 momjian Exp $ */

#define POSTGRES_ECPG_INTERNAL
#include "postgres_fe.h"

#include <limits.h>
#include <unistd.h>
#include "ecpg-pthread-win32.h"
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

bool		ecpg_internal_regression_mode = false;

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
ecpg_init_sqlca(struct sqlca_t * sqlca)
{
	memcpy((char *) sqlca, (char *) &sqlca_init, sizeof(struct sqlca_t));
}

bool
ecpg_init(const struct connection * con, const char *connection_name, const int lineno)
{
	struct sqlca_t *sqlca = ECPGget_sqlca();

	ecpg_init_sqlca(sqlca);
	if (con == NULL)
	{
		ecpg_raise(lineno, ECPG_NO_CONN, ECPG_SQLSTATE_CONNECTION_DOES_NOT_EXIST,
				   connection_name ? connection_name : "NULL");
		return (false);
	}

	return (true);
}

#ifdef ENABLE_THREAD_SAFETY
static void
ecpg_sqlca_key_destructor(void *arg)
{
	free(arg);					/* sqlca structure allocated in ECPGget_sqlca */
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
		ecpg_init_sqlca(sqlca);
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
	struct connection *con = ecpg_get_connection(connection_name);

	if (!ecpg_init(con, connection_name, lineno))
		return (false);

	/* are we connected? */
	if (con->connection == NULL)
	{
		ecpg_raise(lineno, ECPG_NOT_CONN, ECPG_SQLSTATE_ECPG_INTERNAL_ERROR, con->name);
		return false;
	}

	return (true);
}

bool
ECPGtrans(int lineno, const char *connection_name, const char *transaction)
{
	PGresult   *res;
	struct connection *con = ecpg_get_connection(connection_name);

	if (!ecpg_init(con, connection_name, lineno))
		return (false);

	ecpg_log("ECPGtrans line %d action = %s connection = %s\n", lineno, transaction, con ? con->name : "(nil)");

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
			if (!ecpg_check_PQresult(res, lineno, con->connection, ECPG_COMPAT_PGSQL))
				return FALSE;
			PQclear(res);
		}

		res = PQexec(con->connection, transaction);
		if (!ecpg_check_PQresult(res, lineno, con->connection, ECPG_COMPAT_PGSQL))
			return FALSE;
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

	if (n > 100)
	{
		ecpg_internal_regression_mode = true;
		simple_debug = n - 100;
	}
	else
		simple_debug = n;

	debugstream = dbgs;

	ecpg_log("ECPGdebug: set to %d\n", simple_debug);

#ifdef ENABLE_THREAD_SAFETY
	pthread_mutex_unlock(&debug_init_mutex);
#endif
}

void
ecpg_log(const char *format,...)
{
	va_list		ap;
	struct sqlca_t *sqlca = ECPGget_sqlca();

	if (simple_debug)
	{
		int			bufsize = strlen(format) + 100;
		char	   *f = (char *) malloc(bufsize);

		if (f == NULL)
			return;

		/*
		 * regression tests set this environment variable to get the same
		 * output for every run.
		 */
		if (ecpg_internal_regression_mode)
			snprintf(f, bufsize, "[NO_PID]: %s", format);
		else
			snprintf(f, bufsize, "[%d]: %s", (int) getpid(), format);

#ifdef ENABLE_THREAD_SAFETY
		pthread_mutex_lock(&debug_mutex);
#endif

		va_start(ap, format);
		vfprintf(debugstream, f, ap);
		va_end(ap);

		/* dump out internal sqlca variables */
		if (ecpg_internal_regression_mode)
			fprintf(debugstream, "[NO_PID]: sqlca: code: %ld, state: %s\n",
					sqlca->sqlcode, sqlca->sqlstate);

		fflush(debugstream);

#ifdef ENABLE_THREAD_SAFETY
		pthread_mutex_unlock(&debug_mutex);
#endif

		free(f);
	}
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

#ifdef WIN32
#ifdef ENABLE_THREAD_SAFETY

void
win32_pthread_mutex(volatile pthread_mutex_t *mutex)
{
	if (mutex->handle == NULL)
	{
		while (InterlockedExchange((LONG *) & mutex->initlock, 1) == 1)
			Sleep(0);
		if (mutex->handle == NULL)
			mutex->handle = CreateMutex(NULL, FALSE, NULL);
		InterlockedExchange((LONG *) & mutex->initlock, 0);
	}
}

static pthread_mutex_t win32_pthread_once_lock = PTHREAD_MUTEX_INITIALIZER;

void
win32_pthread_once(volatile pthread_once_t *once, void (*fn) (void))
{
	if (!*once)
	{
		pthread_mutex_lock(&win32_pthread_once_lock);
		if (!*once)
		{
			*once = true;
			fn();
		}
		pthread_mutex_unlock(&win32_pthread_once_lock);
	}
}
#endif   /* ENABLE_THREAD_SAFETY */

#endif   /* WIN32 */
