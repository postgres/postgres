#include "postgres_fe.h"

#include "libpq-fe.h"
#include "pqexpbuffer.h"
#include "getopt_long.h"

#ifndef HAVE_OPTRESET
int			optreset;
#endif

const char *get_user_name(const char *progname);

#define _(x) gettext((x))
void		init_nls(void);

typedef void (*help_handler) (const char *);

void		handle_help_version_opts(int argc, char *argv[], const char *fixed_progname, help_handler hlp);

PGconn *connectDatabase(const char *dbname, const char *pghost, const char *pgport,
		const char *pguser, bool require_password, const char *progname);

PGresult *
			executeQuery(PGconn *conn, const char *command, const char *progname, bool echo);

int
			check_yesno_response(const char *string);
