/*
 *	util.c
 *
 *	utility functions
 *
 *	Copyright (c) 2010-2022, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/util.c
 */

#include "postgres_fe.h"

#include <signal.h>

#include "common/username.h"
#include "pg_upgrade.h"

LogOpts		log_opts;

static void pg_log_v(eLogType type, const char *fmt, va_list ap) pg_attribute_printf(2, 0);


/*
 * report_status()
 *
 *	Displays the result of an operation (ok, failed, error message,...)
 */
void
report_status(eLogType type, const char *fmt,...)
{
	va_list		args;
	char		message[MAX_STRING];

	va_start(args, fmt);
	vsnprintf(message, sizeof(message), fmt, args);
	va_end(args);

	pg_log(type, "%s\n", message);
}


void
end_progress_output(void)
{
	/*
	 * For output to a tty, erase prior contents of progress line. When either
	 * tty or verbose, indent so that report_status() output will align
	 * nicely.
	 */
	if (log_opts.isatty)
	{
		printf("\r");
		pg_log(PG_REPORT, "%-*s", MESSAGE_WIDTH, "");
	}
	else if (log_opts.verbose)
		pg_log(PG_REPORT, "%-*s", MESSAGE_WIDTH, "");
}

/*
 * Remove any logs generated internally.  To be used once when exiting.
 */
void
cleanup_output_dirs(void)
{
	fclose(log_opts.internal);

	/* Remove dump and log files? */
	if (log_opts.retain)
		return;

	(void) rmtree(log_opts.basedir, true);

	/* Remove pg_upgrade_output.d only if empty */
	switch (pg_check_dir(log_opts.rootdir))
	{
		case 0:					/* non-existent */
		case 3:					/* exists and contains a mount point */
			Assert(false);
			break;

		case 1:					/* exists and empty */
		case 2:					/* exists and contains only dot files */
			(void) rmtree(log_opts.rootdir, true);
			break;

		case 4:					/* exists */

			/*
			 * Keep the root directory as this includes some past log
			 * activity.
			 */
			break;

		default:
			/* different failure, just report it */
			pg_log(PG_WARNING, "could not access directory \"%s\": %m\n",
				   log_opts.rootdir);
			break;
	}
}

/*
 * prep_status
 *
 *	Displays a message that describes an operation we are about to begin.
 *	We pad the message out to MESSAGE_WIDTH characters so that all of the "ok" and
 *	"failed" indicators line up nicely.
 *
 *	A typical sequence would look like this:
 *		prep_status("about to flarb the next %d files", fileCount );
 *
 *		if(( message = flarbFiles(fileCount)) == NULL)
 *		  report_status(PG_REPORT, "ok" );
 *		else
 *		  pg_log(PG_FATAL, "failed - %s\n", message );
 */
void
prep_status(const char *fmt,...)
{
	va_list		args;
	char		message[MAX_STRING];

	va_start(args, fmt);
	vsnprintf(message, sizeof(message), fmt, args);
	va_end(args);

	/* trim strings */
	pg_log(PG_REPORT, "%-*s", MESSAGE_WIDTH, message);
}

/*
 * prep_status_progress
 *
 *   Like prep_status(), but for potentially longer running operations.
 *   Details about what item is currently being processed can be displayed
 *   with pg_log(PG_STATUS, ...). A typical sequence would look like this:
 *
 *   prep_status_progress("copying files");
 *   for (...)
 *     pg_log(PG_STATUS, "%s", filename);
 *   end_progress_output();
 *   report_status(PG_REPORT, "ok");
 */
void
prep_status_progress(const char *fmt,...)
{
	va_list		args;
	char		message[MAX_STRING];

	va_start(args, fmt);
	vsnprintf(message, sizeof(message), fmt, args);
	va_end(args);

	/*
	 * If outputting to a tty or in verbose, append newline. pg_log_v() will
	 * put the individual progress items onto the next line.
	 */
	if (log_opts.isatty || log_opts.verbose)
		pg_log(PG_REPORT, "%-*s\n", MESSAGE_WIDTH, message);
	else
		pg_log(PG_REPORT, "%-*s", MESSAGE_WIDTH, message);
}

static void
pg_log_v(eLogType type, const char *fmt, va_list ap)
{
	char		message[QUERY_ALLOC];

	vsnprintf(message, sizeof(message), _(fmt), ap);

	/* PG_VERBOSE and PG_STATUS are only output in verbose mode */
	/* fopen() on log_opts.internal might have failed, so check it */
	if (((type != PG_VERBOSE && type != PG_STATUS) || log_opts.verbose) &&
		log_opts.internal != NULL)
	{
		if (type == PG_STATUS)
			/* status messages need two leading spaces and a newline */
			fprintf(log_opts.internal, "  %s\n", message);
		else
			fprintf(log_opts.internal, "%s", message);
		fflush(log_opts.internal);
	}

	switch (type)
	{
		case PG_VERBOSE:
			if (log_opts.verbose)
				printf("%s", message);
			break;

		case PG_STATUS:

			/*
			 * For output to a display, do leading truncation. Append \r so
			 * that the next message is output at the start of the line.
			 *
			 * If going to non-interactive output, only display progress if
			 * verbose is enabled. Otherwise the output gets unreasonably
			 * large by default.
			 */
			if (log_opts.isatty)
				/* -2 because we use a 2-space indent */
				printf("  %s%-*.*s\r",
				/* prefix with "..." if we do leading truncation */
					   strlen(message) <= MESSAGE_WIDTH - 2 ? "" : "...",
					   MESSAGE_WIDTH - 2, MESSAGE_WIDTH - 2,
				/* optional leading truncation */
					   strlen(message) <= MESSAGE_WIDTH - 2 ? message :
					   message + strlen(message) - MESSAGE_WIDTH + 3 + 2);
			else if (log_opts.verbose)
				printf("  %s\n", message);
			break;

		case PG_REPORT:
		case PG_WARNING:
			printf("%s", message);
			break;

		case PG_FATAL:
			printf("\n%s", message);
			printf(_("Failure, exiting\n"));
			exit(1);
			break;

		default:
			break;
	}
	fflush(stdout);
}


void
pg_log(eLogType type, const char *fmt,...)
{
	va_list		args;

	va_start(args, fmt);
	pg_log_v(type, fmt, args);
	va_end(args);
}


void
pg_fatal(const char *fmt,...)
{
	va_list		args;

	va_start(args, fmt);
	pg_log_v(PG_FATAL, fmt, args);
	va_end(args);
	printf(_("Failure, exiting\n"));
	exit(1);
}


void
check_ok(void)
{
	/* all seems well */
	report_status(PG_REPORT, "ok");
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
quote_identifier(const char *s)
{
	char	   *result = pg_malloc(strlen(s) * 2 + 3);
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
 */
int
get_user_info(char **user_name_p)
{
	int			user_id;
	const char *user_name;
	char	   *errstr;

#ifndef WIN32
	user_id = geteuid();
#else
	user_id = 1;
#endif

	user_name = get_user_name(&errstr);
	if (!user_name)
		pg_fatal("%s\n", errstr);

	/* make a copy */
	*user_name_p = pg_strdup(user_name);

	return user_id;
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
