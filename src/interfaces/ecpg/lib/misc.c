#include <unistd.h>
#include "ecpgtype.h"
#include "ecpglib.h"
#include "ecpgerrno.h"
#include "extern.h"
#include "sqlca.h"

static struct sqlca sqlca_init =
{
	{'S', 'Q', 'L', 'C', 'A', ' ', ' ', ' '},
	sizeof(struct sqlca),
	0,
	{0, {0}},
	{'N', 'O', 'T', ' ', 'S', 'E', 'T', ' '},
	{0, 0, 0, 0, 0, 0},
	{0, 0, 0, 0, 0, 0, 0, 0},
	{0, 0, 0, 0, 0, 0, 0, 0}
};

static int	simple_debug = 0;
static FILE *debugstream = NULL;

void
init_sqlca(void)
{
	memcpy((char *) &sqlca, (char *) &sqlca_init, sizeof(sqlca));
}

bool
ecpg_init(const struct connection * con, const char *connection_name, const int lineno)
{
	init_sqlca();
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
	struct connection *con = get_connection(connection_name);

	if (!ecpg_init(con, connection_name, lineno))
		return (false);

	/* are we connected? */
	if (con->connection == NULL)
	{
		ECPGlog("ECPGdo: not connected to %s\n", con->name);
		ECPGraise(lineno, ECPG_NOT_CONN, NULL);
		return false;
	}

	return (true);
}

bool
ECPGtrans(int lineno, const char *connection_name, const char *transaction)
{
	PGresult   *res;
	struct connection *con = get_connection(connection_name);

	if (!ecpg_init(con, connection_name, lineno))
		return (false);

	ECPGlog("ECPGtrans line %d action = %s connection = %s\n", lineno, transaction, con->name);

	/* if we have no connection we just simulate the command */
	if (con && con->connection)
	{
		if ((res = PQexec(con->connection, transaction)) == NULL)
		{
			ECPGraise(lineno, ECPG_TRANS, NULL);
			return FALSE;
		}
		PQclear(res);
	}

	if (strcmp(transaction, "commit") == 0 || strcmp(transaction, "rollback") == 0)
	{
		con->committed = true;

		/* deallocate all prepared statements */
		if (!ECPGdeallocate_all(lineno))
			return false;
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

		free(f);
	}
}
