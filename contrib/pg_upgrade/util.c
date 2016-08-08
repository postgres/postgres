/*
 *	util.c
 *
 *	utility functions
 *
 *	Copyright (c) 2010-2012, PostgreSQL Global Development Group
 *	contrib/pg_upgrade/util.c
 */

#include "postgres.h"

#include "pg_upgrade.h"

#include <signal.h>


LogOpts		log_opts;

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

	if (strlen(message) > 0 && message[strlen(message) - 1] == '\n')
		pg_log(PG_REPORT, "%s", message);
	else
		pg_log(PG_REPORT, "%-" MESSAGE_WIDTH "s", message);
}


void
pg_log(eLogType type, char *fmt,...)
{
	va_list		args;
	char		message[MAX_STRING];

	va_start(args, fmt);
	vsnprintf(message, sizeof(message), fmt, args);
	va_end(args);

	/* PG_VERBOSE is only output in verbose mode */
	/* fopen() on log_opts.internal might have failed, so check it */
	if ((type != PG_VERBOSE || log_opts.verbose) && log_opts.internal != NULL)
	{
		/*
		 * There's nothing much we can do about it if fwrite fails, but some
		 * platforms declare fwrite with warn_unused_result.  Do a little
		 * dance with casting to void to shut up the compiler in such cases.
		 */
		size_t		rc;

		rc = fwrite(message, strlen(message), 1, log_opts.internal);
		/* if we are using OVERWRITE_MESSAGE, add newline to log file */
		if (strchr(message, '\r') != NULL)
			rc = fwrite("\n", 1, 1, log_opts.internal);
		(void) rc;
		fflush(log_opts.internal);
	}

	switch (type)
	{
		case PG_VERBOSE:
			if (log_opts.verbose)
				printf("%s", _(message));
			break;

		case PG_REPORT:
		case PG_WARNING:
			printf("%s", _(message));
			break;

		case PG_FATAL:
			printf("\n%s", _(message));
			printf("Failure, exiting\n");
			exit(1);
			break;

		default:
			break;
	}
	fflush(stdout);
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
 * Append the given string to the shell command being built in the buffer,
 * with suitable shell-style quoting to create exactly one argument.
 *
 * Forbid LF or CR characters, which have scant practical use beyond designing
 * security breaches.  The Windows command shell is unusable as a conduit for
 * arguments containing LF or CR characters.  A future major release should
 * reject those characters in CREATE ROLE and CREATE DATABASE, because use
 * there eventually leads to errors here.
 */
void
appendShellString(PQExpBuffer buf, const char *str)
{
	const char *p;

#ifndef WIN32
	appendPQExpBufferChar(buf, '\'');
	for (p = str; *p; p++)
	{
		if (*p == '\n' || *p == '\r')
		{
			fprintf(stderr,
					_("shell command argument contains a newline or carriage return: \"%s\"\n"),
					str);
			exit(EXIT_FAILURE);
		}

		if (*p == '\'')
			appendPQExpBufferStr(buf, "'\"'\"'");
		else
			appendPQExpBufferChar(buf, *p);
	}
	appendPQExpBufferChar(buf, '\'');
#else							/* WIN32 */
	int			backslash_run_length = 0;

	/*
	 * A Windows system() argument experiences two layers of interpretation.
	 * First, cmd.exe interprets the string.  Its behavior is undocumented,
	 * but a caret escapes any byte except LF or CR that would otherwise have
	 * special meaning.  Handling of a caret before LF or CR differs between
	 * "cmd.exe /c" and other modes, and it is unusable here.
	 *
	 * Second, the new process parses its command line to construct argv (see
	 * https://msdn.microsoft.com/en-us/library/17w5ykft.aspx).  This treats
	 * backslash-double quote sequences specially.
	 */
	appendPQExpBufferStr(buf, "^\"");
	for (p = str; *p; p++)
	{
		if (*p == '\n' || *p == '\r')
		{
			fprintf(stderr,
					_("shell command argument contains a newline or carriage return: \"%s\"\n"),
					str);
			exit(EXIT_FAILURE);
		}

		/* Change N backslashes before a double quote to 2N+1 backslashes. */
		if (*p == '"')
		{
			while (backslash_run_length)
			{
				appendPQExpBufferStr(buf, "^\\");
				backslash_run_length--;
			}
			appendPQExpBufferStr(buf, "^\\");
		}
		else if (*p == '\\')
			backslash_run_length++;
		else
			backslash_run_length = 0;

		/*
		 * Decline to caret-escape the most mundane characters, to ease
		 * debugging and lest we approach the command length limit.
		 */
		if (!((*p >= 'a' && *p <= 'z') ||
			  (*p >= 'A' && *p <= 'Z') ||
			  (*p >= '0' && *p <= '9')))
			appendPQExpBufferChar(buf, '^');
		appendPQExpBufferChar(buf, *p);
	}

	/*
	 * Change N backslashes at end of argument to 2N backslashes, because they
	 * precede the double quote that terminates the argument.
	 */
	while (backslash_run_length)
	{
		appendPQExpBufferStr(buf, "^\\");
		backslash_run_length--;
	}
	appendPQExpBufferStr(buf, "^\"");
#endif   /* WIN32 */
}


/*
 * Append the given string to the buffer, with suitable quoting for passing
 * the string as a value, in a keyword/pair value in a libpq connection
 * string
 */
void
appendConnStrVal(PQExpBuffer buf, const char *str)
{
	const char *s;
	bool		needquotes;

	/*
	 * If the string is one or more plain ASCII characters, no need to quote
	 * it. This is quite conservative, but better safe than sorry.
	 */
	needquotes = true;
	for (s = str; *s; s++)
	{
		if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
			  (*s >= '0' && *s <= '9') || *s == '_' || *s == '.'))
		{
			needquotes = true;
			break;
		}
		needquotes = false;
	}

	if (needquotes)
	{
		appendPQExpBufferChar(buf, '\'');
		while (*str)
		{
			/* ' and \ must be escaped by to \' and \\ */
			if (*str == '\'' || *str == '\\')
				appendPQExpBufferChar(buf, '\\');

			appendPQExpBufferChar(buf, *str);
			str++;
		}
		appendPQExpBufferChar(buf, '\'');
	}
	else
		appendPQExpBufferStr(buf, str);
}


/*
 * Append a psql meta-command that connects to the given database with the
 * then-current connection's user, host and port.
 */
void
appendPsqlMetaConnect(PQExpBuffer buf, const char *dbname)
{
	const char *s;
	bool		complex;

	/*
	 * If the name is plain ASCII characters, emit a trivial "\connect "foo"".
	 * For other names, even many not technically requiring it, skip to the
	 * general case.  No database has a zero-length name.
	 */
	complex = false;
	for (s = dbname; *s; s++)
	{
		if (*s == '\n' || *s == '\r')
		{
			fprintf(stderr,
					_("database name contains a newline or carriage return: \"%s\"\n"),
					dbname);
			exit(EXIT_FAILURE);
		}

		if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
			  (*s >= '0' && *s <= '9') || *s == '_' || *s == '.'))
		{
			complex = true;
		}
	}

	appendPQExpBufferStr(buf, "\\connect ");
	if (complex)
	{
		PQExpBufferData connstr;

		initPQExpBuffer(&connstr);
		appendPQExpBuffer(&connstr, "dbname=");
		appendConnStrVal(&connstr, dbname);

		appendPQExpBuffer(buf, "-reuse-previous=on ");

		/*
		 * As long as the name does not contain a newline, SQL identifier
		 * quoting satisfies the psql meta-command parser.  Prefer not to
		 * involve psql-interpreted single quotes, which behaved differently
		 * before PostgreSQL 9.2.
		 */
		appendPQExpBufferStr(buf, quote_identifier(connstr.data));

		termPQExpBuffer(&connstr);
	}
	else
		appendPQExpBufferStr(buf, quote_identifier(dbname));
	appendPQExpBufferChar(buf, '\n');
}


/*
 * get_user_info()
 * (copied from initdb.c) find the current user
 */
int
get_user_info(char **user_name)
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

	*user_name = pg_strdup(pw->pw_name);

	return user_id;
}


void *
pg_malloc(size_t size)
{
	void	   *p;

	/* Avoid unportable behavior of malloc(0) */
	if (size == 0)
		size = 1;
	p = malloc(size);
	if (p == NULL)
		pg_log(PG_FATAL, "%s: out of memory\n", os_info.progname);
	return p;
}

void *
pg_realloc(void *ptr, size_t size)
{
	void	   *p;

	/* Avoid unportable behavior of realloc(NULL, 0) */
	if (ptr == NULL && size == 0)
		size = 1;
	p = realloc(ptr, size);
	if (p == NULL)
		pg_log(PG_FATAL, "%s: out of memory\n", os_info.progname);
	return p;
}


void
pg_free(void *ptr)
{
	if (ptr != NULL)
		free(ptr);
}


char *
pg_strdup(const char *s)
{
	char	   *result = strdup(s);

	if (result == NULL)
		pg_log(PG_FATAL, "%s: out of memory\n", os_info.progname);

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
	/* _dosmaperr sets errno, so we copy errno here */
	errNum = errno;
#endif
	return pg_strdup(strerror(errNum));
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


/*
 *	pg_putenv()
 *
 *	This is like putenv(), but takes two arguments.
 *	It also does unsetenv() if val is NULL.
 */
void
pg_putenv(const char *var, const char *val)
{
	if (val)
	{
#ifndef WIN32
		char	   *envstr = (char *) pg_malloc(strlen(var) +
												strlen(val) + 2);

		sprintf(envstr, "%s=%s", var, val);
		putenv(envstr);

		/*
		 * Do not free envstr because it becomes part of the environment on
		 * some operating systems.  See port/unsetenv.c::unsetenv.
		 */
#else
		SetEnvironmentVariableA(var, val);
#endif
	}
	else
	{
#ifndef WIN32
		unsetenv(var);
#else
		SetEnvironmentVariableA(var, "");
#endif
	}
}
