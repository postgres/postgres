/*
 *	util.c
 *
 *	utility functions
 *
 *	Copyright (c) 2010, PostgreSQL Global Development Group
 *	$PostgreSQL: pgsql/contrib/pg_upgrade/util.c,v 1.5 2010/07/06 19:18:55 momjian Exp $
 */

#include "pg_upgrade.h"

#include <signal.h>


/*
 * report_status()
 *
 *	Displays the result of an operation (ok, failed, error message,...)
 */
void
report_status(migratorContext *ctx, eLogType type, const char *fmt,...)
{
	va_list		args;
	char		message[MAX_STRING];

	va_start(args, fmt);
	vsnprintf(message, sizeof(message), fmt, args);
	va_end(args);

	pg_log(ctx, type, "%s\n", message);
}


/*
 * prep_status(&ctx, )
 *
 *	Displays a message that describes an operation we are about to begin.
 *	We pad the message out to MESSAGE_WIDTH characters so that all of the "ok" and
 *	"failed" indicators line up nicely.
 *
 *	A typical sequence would look like this:
 *		prep_status(&ctx,  "about to flarb the next %d files", fileCount );
 *
 *		if(( message = flarbFiles(fileCount)) == NULL)
 *		  report_status(ctx, PG_REPORT, "ok" );
 *		else
 *		  pg_log(ctx, PG_FATAL, "failed - %s", message );
 */
void
prep_status(migratorContext *ctx, const char *fmt,...)
{
	va_list		args;
	char		message[MAX_STRING];

	va_start(args, fmt);
	vsnprintf(message, sizeof(message), fmt, args);
	va_end(args);

	if (strlen(message) > 0 && message[strlen(message) - 1] == '\n')
		pg_log(ctx, PG_REPORT, "%s", message);
	else
		pg_log(ctx, PG_REPORT, "%-" MESSAGE_WIDTH "s", message);
}


void
pg_log(migratorContext *ctx, eLogType type, char *fmt,...)
{
	va_list		args;
	char		message[MAX_STRING];

	va_start(args, fmt);
	vsnprintf(message, sizeof(message), fmt, args);
	va_end(args);

	if (ctx->log_fd != NULL)
	{
		fwrite(message, strlen(message), 1, ctx->log_fd);
		/* if we are using OVERWRITE_MESSAGE, add newline */
		if (strchr(message, '\r') != NULL)
			fwrite("\n", 1, 1, ctx->log_fd);
		fflush(ctx->log_fd);
	}

	switch (type)
	{
		case PG_INFO:
			if (ctx->verbose)
				printf("%s", _(message));
			break;

		case PG_REPORT:
		case PG_WARNING:
			printf("%s", _(message));
			break;

		case PG_FATAL:
			printf("%s", "\n");
			printf("%s", _(message));
			exit_nicely(ctx, true);
			break;

		case PG_DEBUG:
			if (ctx->debug)
				fprintf(ctx->debug_fd, "%s\n", _(message));
			break;

		default:
			break;
	}
	fflush(stdout);
}


void
check_ok(migratorContext *ctx)
{
	/* all seems well */
	report_status(ctx, PG_REPORT, "ok");
	fflush(stdout);
}


/*
 * quote_identifier()
 *		Properly double-quote a SQL identifier.
 *
 * The result should be pg_free'd, but most callers don't bother because
 * memory leakage is not a big deal in this program.
 */
char *
quote_identifier(migratorContext *ctx, const char *s)
{
	char	   *result = pg_malloc(ctx, strlen(s) * 2 + 3);
	char	   *r = result;

	*r++ = '"';
	while (*s)
	{
		if (*s == '"')
			*r++ = *s;
		*r++ = *s;
		s++;
	}
	*r++ = '"';
	*r++ = '\0';

	return result;
}


/*
 * get_user_info()
 * (copied from initdb.c) find the current user
 */
int
get_user_info(migratorContext *ctx, char **user_name)
{
	int			user_id;

#ifndef WIN32
	struct passwd *pw = getpwuid(geteuid());

	user_id = geteuid();
#else							/* the windows code */
	struct passwd_win32
	{
		int			pw_uid;
		char		pw_name[128];
	}			pass_win32;
	struct passwd_win32 *pw = &pass_win32;
	DWORD		pwname_size = sizeof(pass_win32.pw_name) - 1;

	GetUserName(pw->pw_name, &pwname_size);

	user_id = 1;
#endif

	*user_name = pg_strdup(ctx, pw->pw_name);

	return user_id;
}


void
exit_nicely(migratorContext *ctx, bool need_cleanup)
{
	stop_postmaster(ctx, true, true);

	pg_free(ctx->logfile);

	if (ctx->log_fd)
		fclose(ctx->log_fd);

	if (ctx->debug_fd)
		fclose(ctx->debug_fd);

	/* terminate any running instance of postmaster */
	if (ctx->postmasterPID != 0)
		kill(ctx->postmasterPID, SIGTERM);

	if (need_cleanup)
	{
		/*
		 * FIXME must delete intermediate files
		 */
		exit(1);
	}
	else
		exit(0);
}


void *
pg_malloc(migratorContext *ctx, int n)
{
	void	   *p = malloc(n);

	if (p == NULL)
		pg_log(ctx, PG_FATAL, "%s: out of memory\n", ctx->progname);

	return p;
}


void
pg_free(void *p)
{
	if (p != NULL)
		free(p);
}


char *
pg_strdup(migratorContext *ctx, const char *s)
{
	char	   *result = strdup(s);

	if (result == NULL)
		pg_log(ctx, PG_FATAL, "%s: out of memory\n", ctx->progname);

	return result;
}


/*
 * getErrorText()
 *
 *	Returns the text of the error message for the given error number
 *
 *	This feature is factored into a separate function because it is
 *	system-dependent.
 */
const char *
getErrorText(int errNum)
{
#ifdef WIN32
	_dosmaperr(GetLastError());
#endif
	return strdup(strerror(errNum));
}


/*
 *	str2uint()
 *
 *	convert string to oid
 */
unsigned int
str2uint(const char *str)
{
	return strtoul(str, NULL, 10);
}
