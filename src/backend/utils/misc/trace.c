/*-------------------------------------------------------------------------
 *
 * trace.c
 *
 *	  Conditional trace ans logging functions.
 *
 *	  Massimo Dal Zotto <dz@cs.unitn.it>
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#ifdef USE_SYSLOG
#include <syslog.h>
#endif

#include "postgres.h"
#include "miscadmin.h"
#include "utils/trace.h"
#include "libpq/pqsignal.h"

#ifdef USE_SYSLOG
/*
 * Global option to control the use of syslog(3) for logging:
 *
 *		0	stdout/stderr only
 *		1	stdout/stderr + syslog
 *		2	syslog only
 */
#define UseSyslog		pg_options[OPT_SYSLOG]
#define PG_LOG_FACILITY LOG_LOCAL0
#define PG_LOG_IDENT	"postgres"
#else
#define UseSyslog 0
#endif

/*
 * Trace option names, must match the constants in trace_opts[].
 */
static char *opt_names[] = {
	"all",
	"verbose",
	"query",
	"plan",
	"parse",
	"rewritten",
	"pretty_plan",
	"pretty_parse",
	"pretty_rewritten",
	"parserstats",
	"plannerstats",
	"executorstats",
	"shortlocks",				/* currently unused but needed, see lock.c */
	"locks",
	"userlocks",
	"spinlocks",
	"notify",
	"malloc",
	"palloc",
	"lock_debug_oidmin",
	"lock_debug_relid",
	"lock_read_priority",		/* lock priority, see lock.c */
	"deadlock_timeout",			/* deadlock timeout, see proc.c */
	"syslog",					/* use syslog for error messages */
	"hostlookup",				/* enable hostname lookup in ps_status */
	"showportnumber",			/* show port number in ps_status */
};

/*
 * Array of trace flags which can be set or reset independently.
 */
int			pg_options[NUM_PG_OPTIONS] = {0};

/*
 * Print a timestamp and a message to stdout if the trace flag
 * indexed by the flag value is set.
 */
int
tprintf(int flag, const char *fmt,...)
{
	va_list		ap;
	char		line[ELOG_MAXLEN + TIMESTAMP_SIZE + 1];

#ifdef USE_SYSLOG
	int			log_level;

#endif

	if ((flag == TRACE_ALL) || (pg_options[TRACE_ALL] > 0))
	{
		/* uconditional trace or trace all option set */
	}
	else if (pg_options[TRACE_ALL] == 0)
	{
		if ((flag < 0) || (flag >= NUM_PG_OPTIONS) || (!pg_options[flag]))
			return 0;
	}
	else if (pg_options[TRACE_ALL] < 0)
		return 0;

	va_start(ap, fmt);
#ifdef ELOG_TIMESTAMPS
	strcpy(line, tprintf_timestamp());
#endif
	vsnprintf(line + TIMESTAMP_SIZE, ELOG_MAXLEN, fmt, ap);
	va_end(ap);

#ifdef USE_SYSLOG
	log_level = ((flag == TRACE_ALL) ? LOG_INFO : LOG_DEBUG);
	write_syslog(log_level, line + TIMESTAMP_SIZE);
#endif

	if (UseSyslog <= 1)
	{
		puts(line);
		fflush(stdout);
	}

	return 1;
}

/*
 * Print a timestamp and a message to stdout or to syslog.
 */
int
tprintf1(const char *fmt,...)
{
	va_list		ap;
	char		line[ELOG_MAXLEN + TIMESTAMP_SIZE + 1];

	va_start(ap, fmt);
#ifdef ELOG_TIMESTAMPS
	strcpy(line, tprintf_timestamp());
#endif
	vsnprintf(line + TIMESTAMP_SIZE, ELOG_MAXLEN, fmt, ap);
	va_end(ap);

#ifdef USE_SYSLOG
	write_syslog(LOG_INFO, line + TIMESTAMP_SIZE);
#endif

	if (UseSyslog <= 1)
	{
		puts(line);
		fflush(stdout);
	}

	return 1;
}

/*
 * Print a timestamp and a message to stderr.
 */
int
eprintf(const char *fmt,...)
{
	va_list		ap;
	char		line[ELOG_MAXLEN + TIMESTAMP_SIZE + 1];

	va_start(ap, fmt);
#ifdef ELOG_TIMESTAMPS
	strcpy(line, tprintf_timestamp());
#endif
	vsnprintf(line + TIMESTAMP_SIZE, ELOG_MAXLEN, fmt, ap);
	va_end(ap);

#ifdef USE_SYSLOG
	write_syslog(LOG_ERR, line + TIMESTAMP_SIZE);
#endif

	if (UseSyslog <= 1)
	{
		fputs(line, stderr);
		fputc('\n', stderr);
		fflush(stderr);
	}

	return 1;
}

#ifdef USE_SYSLOG
/*
 * Write a message line to syslog if the syslog option is set.
 */
void
write_syslog(int level, char *line)
{
	static int	openlog_done = 0;

	if (UseSyslog >= 1)
	{
		if (!openlog_done)
		{
			openlog_done = 1;
			openlog(PG_LOG_IDENT, LOG_PID | LOG_NDELAY, PG_LOG_FACILITY);
		}
		syslog(level, "%s", line);
	}
}

#endif

#ifdef ELOG_TIMESTAMPS
/*
 * Return a timestamp string like "980119.17:25:59.902 [21974] "
 */
char *
tprintf_timestamp()
{
	struct timeval tv;
	struct tm  *time;
	time_t		tm;
	static char timestamp[32],
				pid[8];

	gettimeofday(&tv, DST_NONE);
	tm = tv.tv_sec;
	time = localtime(&tm);

	sprintf(pid, "[%d]", MyProcPid);
	sprintf(timestamp, "%02d%02d%02d.%02d:%02d:%02d.%03d %7s ",
			time->tm_year, time->tm_mon + 1, time->tm_mday,
			time->tm_hour, time->tm_min, time->tm_sec,
			tv.tv_usec / 1000, pid);

	return timestamp;
}

#endif

int
option_flag(int flag)
{
	if ((flag < 0) || (flag >= NUM_PG_OPTIONS))
		return 0;
	return pg_options[flag];
}

int
set_option_flag(int flag, int value)
{
	if ((flag < 0) || (flag >= NUM_PG_OPTIONS))
		return -1;

	pg_options[flag] = value;
	return value;
}

/*
 * Parse an option string like "name,name+,name-,name=value".
 * Single options are delimited by ',',space,tab,newline or cr.
 *
 * If 'secure' is false, the option string came from a remote client via
 * connection "debug options" field --- do not obey any requests that
 * might potentially be security loopholes.
 */
void
parse_options(char *str, bool secure)
{
	char	   *s,
			   *name;
	int			i,
				len,
				val,
				is_comment;

	Assert((sizeof(opt_names) / sizeof(char *)) == NUM_PG_OPTIONS);

	str = strdup(str);
	for (s = str; *s;)
	{
		is_comment = 0;
		name = s;
		val = 1;
		for (; *s; s++)
		{
			switch (*s)
			{
				case '#':
					is_comment = 1;
					break;
				case '=':
					*s++ = '\0';
					val = strtol(s, &s, 10);
					goto setval;
				case '-':
					*s++ = '\0';
					val = 0;
					goto setval;
				case '+':
					*s++ = '\0';
					val = 1;
					goto setval;
				case ' ':
				case ',':
				case '\t':
				case '\n':
				case '\r':
					*s = ',';
					val = 1;
					goto setval;
			}
		}
setval:
		for (; *s; s++)
		{
			if (*s == ',')
			{
				*s++ = '\0';
				break;
			}
		}
		len = strlen(name);
		if (len == 0)
			continue;
		for (i = 0; i < NUM_PG_OPTIONS; i++)
		{
			if (strncmp(name, opt_names[i], len) == 0)
			{
				pg_options[i] = val;
				break;
			}
		}
		if (!is_comment && (i >= NUM_PG_OPTIONS))
			fprintf(stderr, "invalid option: %s\n", name);
	}
	free(str);
}

#define BUF_SIZE 4096

void
read_pg_options(SIGNAL_ARGS)
{
	int			fd;
	int			n;
	int			verbose;
	char		buffer[BUF_SIZE];
	char		c;
	char	   *s,
			   *p;

	if (!DataDir)
	{
		fprintf(stderr, "read_pg_options: DataDir not defined\n");
		return;
	}

	snprintf(buffer, BUF_SIZE - 1, "%s/%s", DataDir, "pg_options");
#ifndef __CYGWIN32__
	if ((fd = open(buffer, O_RDONLY)) < 0)
#else
	if ((fd = open(buffer, O_RDONLY | O_BINARY)) < 0)
#endif
		return;

	if ((n = read(fd, buffer, BUF_SIZE - 1)) > 0)
	{
		/* collpse buffer in place removing comments and spaces */
		for (s = buffer, p = buffer, c = '\0'; s < (buffer + n);)
		{
			switch (*s)
			{
				case '#':
					while ((s < (buffer + n)) && (*s++ != '\n'));
					break;
				case ' ':
				case '\t':
				case '\n':
				case '\r':
					if (c != ',')
						c = *p++ = ',';
					s++;
					break;
				default:
					c = *p++ = *s++;
					break;
			}
		}
		if (c == ',')
			p--;
		*p = '\0';
		verbose = pg_options[TRACE_VERBOSE];
		parse_options(buffer, true);
		verbose |= pg_options[TRACE_VERBOSE];
		if (verbose || postgres_signal_arg == SIGHUP)
			tprintf(TRACE_ALL, "read_pg_options: %s", buffer);
	}

	close(fd);
}

/*
 * Local variables:
 *	tab-width: 4
 *	c-indent-level: 4
 *	c-basic-offset: 4
 * End:
 */
