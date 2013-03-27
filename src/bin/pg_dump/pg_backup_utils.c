/*-------------------------------------------------------------------------
 *
 * pg_backup_utils.c
 *	Utility routines shared by pg_dump and pg_restore
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pg_dump/pg_backup_utils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "pg_backup_utils.h"
#include "parallel.h"

/* Globals exported by this file */
const char *progname = NULL;

#define MAX_ON_EXIT_NICELY				20

static struct
{
	on_exit_nicely_callback function;
	void	   *arg;
}	on_exit_nicely_list[MAX_ON_EXIT_NICELY];

static int	on_exit_nicely_index;

/*
 * Parse a --section=foo command line argument.
 *
 * Set or update the bitmask in *dumpSections according to arg.
 * dumpSections is initialised as DUMP_UNSECTIONED by pg_dump and
 * pg_restore so they can know if this has even been called.
 */
void
set_dump_section(const char *arg, int *dumpSections)
{
	/* if this is the first call, clear all the bits */
	if (*dumpSections == DUMP_UNSECTIONED)
		*dumpSections = 0;

	if (strcmp(arg, "pre-data") == 0)
		*dumpSections |= DUMP_PRE_DATA;
	else if (strcmp(arg, "data") == 0)
		*dumpSections |= DUMP_DATA;
	else if (strcmp(arg, "post-data") == 0)
		*dumpSections |= DUMP_POST_DATA;
	else
	{
		fprintf(stderr, _("%s: unrecognized section name: \"%s\"\n"),
				progname, arg);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit_nicely(1);
	}
}


/*
 * Write a printf-style message to stderr.
 *
 * The program name is prepended, if "progname" has been set.
 * Also, if modulename isn't NULL, that's included too.
 * Note that we'll try to translate the modulename and the fmt string.
 */
void
write_msg(const char *modulename, const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	vwrite_msg(modulename, fmt, ap);
	va_end(ap);
}

/*
 * As write_msg, but pass a va_list not variable arguments.
 */
void
vwrite_msg(const char *modulename, const char *fmt, va_list ap)
{
	if (progname)
	{
		if (modulename)
			fprintf(stderr, "%s: [%s] ", progname, _(modulename));
		else
			fprintf(stderr, "%s: ", progname);
	}
	vfprintf(stderr, _(fmt), ap);
}

/* Register a callback to be run when exit_nicely is invoked. */
void
on_exit_nicely(on_exit_nicely_callback function, void *arg)
{
	if (on_exit_nicely_index >= MAX_ON_EXIT_NICELY)
		exit_horribly(NULL, "out of on_exit_nicely slots\n");
	on_exit_nicely_list[on_exit_nicely_index].function = function;
	on_exit_nicely_list[on_exit_nicely_index].arg = arg;
	on_exit_nicely_index++;
}

/*
 * Run accumulated on_exit_nicely callbacks in reverse order and then exit
 * quietly.  This needs to be thread-safe.
 */
void
exit_nicely(int code)
{
	int			i;

	for (i = on_exit_nicely_index - 1; i >= 0; i--)
		(*on_exit_nicely_list[i].function) (code,
											on_exit_nicely_list[i].arg);

#ifdef WIN32
	if (parallel_init_done && GetCurrentThreadId() != mainThreadId)
		ExitThread(code);
#endif

	exit(code);
}
