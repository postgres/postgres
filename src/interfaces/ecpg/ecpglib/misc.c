/* $Header: /cvsroot/pgsql/src/interfaces/ecpg/ecpglib/misc.c,v 1.3 2003/06/15 04:07:58 momjian Exp $ */

#define POSTGRES_ECPG_INTERNAL
#include "postgres_fe.h"

#include <unistd.h>
#ifdef USE_THREADS
#include <pthread.h>
#endif
#include "ecpgtype.h"
#include "ecpglib.h"
#include "ecpgerrno.h"
#include "extern.h"
#include "sqlca.h"

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
		0, 0, 0, 0, 0, 0, 0, 0
	}
};

#ifdef USE_THREADS
static pthread_key_t   sqlca_key;
static pthread_once_t  sqlca_key_once = PTHREAD_ONCE_INIT;
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
		0, 0, 0, 0, 0, 0, 0, 0
	}
};
#endif

#ifdef USE_THREADS
static pthread_mutex_t debug_mutex    = PTHREAD_MUTEX_INITIALIZER;
#endif
static int simple_debug = 0;
static FILE *debugstream = NULL;

void ECPGinit_sqlca(struct sqlca_t *sqlca)
{
	memcpy((char *)sqlca, (char *)&sqlca_init, sizeof(struct sqlca_t));
}

bool
ECPGinit(const struct connection * con, const char *connection_name, const int lineno)
{
	struct sqlca_t *sqlca = ECPGget_sqlca();
	ECPGinit_sqlca(sqlca);
	if (con == NULL)
	{
		ECPGraise(lineno, ECPG_NO_CONN, connection_name ? connection_name : "NULL");
		return (false);
	}

	return (true);
}

#ifdef USE_THREADS
static void ecpg_sqlca_key_init(void)
{
  pthread_key_create(&sqlca_key, NULL);
}
#endif

struct sqlca_t *ECPGget_sqlca(void)
{
#ifdef USE_THREADS
  struct sqlca_t *sqlca;

  pthread_once(&sqlca_key_once, ecpg_sqlca_key_init);

  sqlca = pthread_getspecific(&sqlca_key);
  if( sqlca == NULL )
    {
      sqlca = malloc(sizeof(struct sqlca_t));
      ECPGinit_sqlca(sqlca);
      pthread_setspecific(&sqlca_key, sqlca);
    }
  return( sqlca );
#else
  return( &sqlca );
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
		ECPGraise(lineno, ECPG_NOT_CONN, con->name);
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

	ECPGlog("ECPGtrans line %d action = %s connection = %s\n", lineno, transaction, con->name);

	/* if we have no connection we just simulate the command */
	if (con && con->connection)
	{
		/*
		 * if we are not in autocommit mode, already have committed the
		 * transaction and get another commit, just ignore it
		 */
		if (!con->committed || con->autocommit)
		{
			if ((res = PQexec(con->connection, transaction)) == NULL)
			{
				ECPGraise(lineno, ECPG_TRANS, NULL);
				return FALSE;
			}
			PQclear(res);
		}
	}

	if (strcmp(transaction, "commit") == 0 || strcmp(transaction, "rollback") == 0)
	{
		con->committed = true;

#if 0
		/* deallocate all prepared statements */
		if (!ECPGdeallocate_all(lineno))
			return false;
#endif
	}

	return true;
}


void
ECPGdebug(int n, FILE *dbgs)
{
#ifdef USE_THREADS
	pthread_mutex_lock(&debug_mutex);
#endif

	simple_debug = n;
	debugstream = dbgs;
	ECPGlog("ECPGdebug: set to %d\n", simple_debug);

#ifdef USE_THREADS
	pthread_mutex_unlock(&debug_mutex);
#endif
}

void
ECPGlog(const char *format,...)
{
	va_list		ap;

#ifdef USE_THREADS
	pthread_mutex_lock(&debug_mutex);
#endif

	if( simple_debug )
	{
		char *f = (char *)malloc(strlen(format) + 100);
		if( f == NULL )
		  {
#ifdef USE_THREADS
			pthread_mutex_unlock(&debug_mutex);
#endif
			return;
		  }

		sprintf(f, "[%d]: %s", (int) getpid(), format);

		va_start(ap, format);
		vfprintf(debugstream, f, ap);
		va_end(ap);

		ECPGfree(f);
	}

#ifdef USE_THREADS
	pthread_mutex_unlock(&debug_mutex);
#endif
}
