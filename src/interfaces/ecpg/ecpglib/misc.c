/* $Header: /cvsroot/pgsql/src/interfaces/ecpg/ecpglib/misc.c,v 1.2 2003/03/21 15:31:04 meskes Exp $ */

#include "postgres_fe.h"

#include <unistd.h>
#include "ecpgtype.h"
#include "ecpglib.h"
#include "ecpgerrno.h"
#include "extern.h"
#include "sqlca.h"

static struct sqlca sqlca_init =
{
	{
		'S', 'Q', 'L', 'C', 'A', ' ', ' ', ' '
	},
	sizeof(struct sqlca),
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

static int	simple_debug = 0;
static FILE *debugstream = NULL;

void
ECPGinit_sqlca(void)
{
	memcpy((char *) &sqlca, (char *) &sqlca_init, sizeof(sqlca));
}

bool
ECPGinit(const struct connection * con, const char *connection_name, const int lineno)
{
	ECPGinit_sqlca();
	if (con == NULL)
	{
		ECPGraise(lineno, ECPG_NO_CONN, connection_name ? connection_name : "NULL");
		return (false);
	}

	return (true);
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
	simple_debug = n;
	debugstream = dbgs;
	ECPGlog("ECPGdebug: set to %d\n", simple_debug);
}

void
ECPGlog(const char *format,...)
{
	va_list		ap;

	if (simple_debug)
	{
		char	   *f = (char *) malloc(strlen(format) + 100);

		if (!f)
			return;

		sprintf(f, "[%d]: %s", (int) getpid(), format);

		va_start(ap, format);
		vfprintf(debugstream, f, ap);
		va_end(ap);

		ECPGfree(f);
	}
}
