/*-------------------------------------------------------------------------
 * Logging framework for frontend programs
 *
 * Copyright (c) 2018-2020, PostgreSQL Global Development Group
 *
 * src/common/logging.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <unistd.h>

#include "common/logging.h"

enum pg_log_level __pg_log_level;

static const char *progname;
static int	log_flags;

static void (*log_pre_callback) (void);
static void (*log_locus_callback) (const char **, uint64 *);

static const char *sgr_error = NULL;
static const char *sgr_warning = NULL;
static const char *sgr_locus = NULL;

#define SGR_ERROR_DEFAULT "01;31"
#define SGR_WARNING_DEFAULT "01;35"
#define SGR_LOCUS_DEFAULT "01"

#define ANSI_ESCAPE_FMT "\x1b[%sm"
#define ANSI_ESCAPE_RESET "\x1b[0m"

#ifdef WIN32

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

/*
 * Attempt to enable VT100 sequence processing for colorization on Windows.
 * If current environment is not VT100-compatible or if this mode could not
 * be enabled, return false.
 */
static bool
enable_vt_processing(void)
{
	/* Check stderr */
	HANDLE		hOut = GetStdHandle(STD_ERROR_HANDLE);
	DWORD		dwMode = 0;

	if (hOut == INVALID_HANDLE_VALUE)
		return false;

	/*
	 * Look for the current console settings and check if VT100 is already
	 * enabled.
	 */
	if (!GetConsoleMode(hOut, &dwMode))
		return false;
	if ((dwMode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0)
		return true;

	dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
	if (!SetConsoleMode(hOut, dwMode))
		return false;
	return true;
}
#endif							/* WIN32 */

/*
 * This should be called before any output happens.
 */
void
pg_logging_init(const char *argv0)
{
	const char *pg_color_env = getenv("PG_COLOR");
	bool		log_color = false;
	bool		color_terminal = isatty(fileno(stderr));

#ifdef WIN32

	/*
	 * On Windows, check if environment is VT100-compatible if using a
	 * terminal.
	 */
	if (color_terminal)
		color_terminal = enable_vt_processing();
#endif

	/* usually the default, but not on Windows */
	setvbuf(stderr, NULL, _IONBF, 0);

	progname = get_progname(argv0);
	__pg_log_level = PG_LOG_INFO;

	if (pg_color_env)
	{
		if (strcmp(pg_color_env, "always") == 0 ||
			(strcmp(pg_color_env, "auto") == 0 && color_terminal))
			log_color = true;
	}

	if (log_color)
	{
		const char *pg_colors_env = getenv("PG_COLORS");

		if (pg_colors_env)
		{
			char	   *colors = strdup(pg_colors_env);

			if (colors)
			{
				for (char *token = strtok(colors, ":"); token; token = strtok(NULL, ":"))
				{
					char	   *e = strchr(token, '=');

					if (e)
					{
						char	   *name;
						char	   *value;

						*e = '\0';
						name = token;
						value = e + 1;

						if (strcmp(name, "error") == 0)
							sgr_error = strdup(value);
						if (strcmp(name, "warning") == 0)
							sgr_warning = strdup(value);
						if (strcmp(name, "locus") == 0)
							sgr_locus = strdup(value);
					}
				}

				free(colors);
			}
		}
		else
		{
			sgr_error = SGR_ERROR_DEFAULT;
			sgr_warning = SGR_WARNING_DEFAULT;
			sgr_locus = SGR_LOCUS_DEFAULT;
		}
	}
}

void
pg_logging_config(int new_flags)
{
	log_flags = new_flags;
}

void
pg_logging_set_level(enum pg_log_level new_level)
{
	__pg_log_level = new_level;
}

void
pg_logging_set_pre_callback(void (*cb) (void))
{
	log_pre_callback = cb;
}

void
pg_logging_set_locus_callback(void (*cb) (const char **filename, uint64 *lineno))
{
	log_locus_callback = cb;
}

void
pg_log_generic(enum pg_log_level level, const char *pg_restrict fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	pg_log_generic_v(level, fmt, ap);
	va_end(ap);
}

void
pg_log_generic_v(enum pg_log_level level, const char *pg_restrict fmt, va_list ap)
{
	int			save_errno = errno;
	const char *filename = NULL;
	uint64		lineno = 0;
	va_list		ap2;
	size_t		required_len;
	char	   *buf;

	Assert(progname);
	Assert(level);
	Assert(fmt);
	Assert(fmt[strlen(fmt) - 1] != '\n');

	/*
	 * Flush stdout before output to stderr, to ensure sync even when stdout
	 * is buffered.
	 */
	fflush(stdout);

	if (log_pre_callback)
		log_pre_callback();

	if (log_locus_callback)
		log_locus_callback(&filename, &lineno);

	fmt = _(fmt);

	if (!(log_flags & PG_LOG_FLAG_TERSE) || filename)
	{
		if (sgr_locus)
			fprintf(stderr, ANSI_ESCAPE_FMT, sgr_locus);
		if (!(log_flags & PG_LOG_FLAG_TERSE))
			fprintf(stderr, "%s:", progname);
		if (filename)
		{
			fprintf(stderr, "%s:", filename);
			if (lineno > 0)
				fprintf(stderr, UINT64_FORMAT ":", lineno);
		}
		fprintf(stderr, " ");
		if (sgr_locus)
			fprintf(stderr, ANSI_ESCAPE_RESET);
	}

	if (!(log_flags & PG_LOG_FLAG_TERSE))
	{
		switch (level)
		{
			case PG_LOG_FATAL:
				if (sgr_error)
					fprintf(stderr, ANSI_ESCAPE_FMT, sgr_error);
				fprintf(stderr, _("fatal: "));
				if (sgr_error)
					fprintf(stderr, ANSI_ESCAPE_RESET);
				break;
			case PG_LOG_ERROR:
				if (sgr_error)
					fprintf(stderr, ANSI_ESCAPE_FMT, sgr_error);
				fprintf(stderr, _("error: "));
				if (sgr_error)
					fprintf(stderr, ANSI_ESCAPE_RESET);
				break;
			case PG_LOG_WARNING:
				if (sgr_warning)
					fprintf(stderr, ANSI_ESCAPE_FMT, sgr_warning);
				fprintf(stderr, _("warning: "));
				if (sgr_warning)
					fprintf(stderr, ANSI_ESCAPE_RESET);
				break;
			default:
				break;
		}
	}

	errno = save_errno;

	va_copy(ap2, ap);
	required_len = vsnprintf(NULL, 0, fmt, ap2) + 1;
	va_end(ap2);

	buf = pg_malloc_extended(required_len, MCXT_ALLOC_NO_OOM);

	errno = save_errno;			/* malloc might change errno */

	if (!buf)
	{
		/* memory trouble, just print what we can and get out of here */
		vfprintf(stderr, fmt, ap);
		return;
	}

	vsnprintf(buf, required_len, fmt, ap);

	/* strip one newline, for PQerrorMessage() */
	if (required_len >= 2 && buf[required_len - 2] == '\n')
		buf[required_len - 2] = '\0';

	fprintf(stderr, "%s\n", buf);

	free(buf);
}
