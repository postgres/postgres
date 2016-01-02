/*--------------------------------------------------------------------
 * ps_status.c
 *
 * Routines to support changing the ps display of PostgreSQL backends
 * to contain some useful information. Mechanism differs wildly across
 * platforms.
 *
 * src/backend/utils/misc/ps_status.c
 *
 * Copyright (c) 2000-2016, PostgreSQL Global Development Group
 * various details abducted from various places
 *--------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>
#ifdef HAVE_SYS_PSTAT_H
#include <sys/pstat.h>			/* for HP-UX */
#endif
#ifdef HAVE_PS_STRINGS
#include <machine/vmparam.h>	/* for old BSD */
#include <sys/exec.h>
#endif
#if defined(__darwin__)
#include <crt_externs.h>
#endif

#include "libpq/libpq.h"
#include "miscadmin.h"
#include "utils/ps_status.h"
#include "utils/guc.h"

extern char **environ;
bool		update_process_title = true;


/*
 * Alternative ways of updating ps display:
 *
 * PS_USE_SETPROCTITLE
 *	   use the function setproctitle(const char *, ...)
 *	   (newer BSD systems)
 * PS_USE_PSTAT
 *	   use the pstat(PSTAT_SETCMD, )
 *	   (HPUX)
 * PS_USE_PS_STRINGS
 *	   assign PS_STRINGS->ps_argvstr = "string"
 *	   (some BSD systems)
 * PS_USE_CHANGE_ARGV
 *	   assign argv[0] = "string"
 *	   (some other BSD systems)
 * PS_USE_CLOBBER_ARGV
 *	   write over the argv and environment area
 *	   (Linux and most SysV-like systems)
 * PS_USE_WIN32
 *	   push the string out as the name of a Windows event
 * PS_USE_NONE
 *	   don't update ps display
 *	   (This is the default, as it is safest.)
 */
#if defined(HAVE_SETPROCTITLE)
#define PS_USE_SETPROCTITLE
#elif defined(HAVE_PSTAT) && defined(PSTAT_SETCMD)
#define PS_USE_PSTAT
#elif defined(HAVE_PS_STRINGS)
#define PS_USE_PS_STRINGS
#elif (defined(BSD) || defined(__hurd__)) && !defined(__darwin__)
#define PS_USE_CHANGE_ARGV
#elif defined(__linux__) || defined(_AIX) || defined(__sgi) || (defined(sun) && !defined(BSD)) || defined(__svr5__) || defined(__darwin__)
#define PS_USE_CLOBBER_ARGV
#elif defined(WIN32)
#define PS_USE_WIN32
#else
#define PS_USE_NONE
#endif


/* Different systems want the buffer padded differently */
#if defined(_AIX) || defined(__linux__) || defined(__darwin__)
#define PS_PADDING '\0'
#else
#define PS_PADDING ' '
#endif


#ifndef PS_USE_CLOBBER_ARGV
/* all but one option need a buffer to write their ps line in */
#define PS_BUFFER_SIZE 256
static char ps_buffer[PS_BUFFER_SIZE];
static const size_t ps_buffer_size = PS_BUFFER_SIZE;
#else							/* PS_USE_CLOBBER_ARGV */
static char *ps_buffer;			/* will point to argv area */
static size_t ps_buffer_size;	/* space determined at run time */
static size_t last_status_len;	/* use to minimize length of clobber */
#endif   /* PS_USE_CLOBBER_ARGV */

static size_t ps_buffer_cur_len;	/* nominal strlen(ps_buffer) */

static size_t ps_buffer_fixed_size;		/* size of the constant prefix */

/* save the original argv[] location here */
static int	save_argc;
static char **save_argv;


/*
 * Call this early in startup to save the original argc/argv values.
 * If needed, we make a copy of the original argv[] array to preserve it
 * from being clobbered by subsequent ps_display actions.
 *
 * (The original argv[] will not be overwritten by this routine, but may be
 * overwritten during init_ps_display.  Also, the physical location of the
 * environment strings may be moved, so this should be called before any code
 * that might try to hang onto a getenv() result.)
 */
char	  **
save_ps_display_args(int argc, char **argv)
{
	save_argc = argc;
	save_argv = argv;

#if defined(PS_USE_CLOBBER_ARGV)

	/*
	 * If we're going to overwrite the argv area, count the available space.
	 * Also move the environment to make additional room.
	 */
	{
		char	   *end_of_area = NULL;
		char	  **new_environ;
		int			i;

		/*
		 * check for contiguous argv strings
		 */
		for (i = 0; i < argc; i++)
		{
			if (i == 0 || end_of_area + 1 == argv[i])
				end_of_area = argv[i] + strlen(argv[i]);
		}

		if (end_of_area == NULL)	/* probably can't happen? */
		{
			ps_buffer = NULL;
			ps_buffer_size = 0;
			return argv;
		}

		/*
		 * check for contiguous environ strings following argv
		 */
		for (i = 0; environ[i] != NULL; i++)
		{
			if (end_of_area + 1 == environ[i])
				end_of_area = environ[i] + strlen(environ[i]);
		}

		ps_buffer = argv[0];
		last_status_len = ps_buffer_size = end_of_area - argv[0];

		/*
		 * move the environment out of the way
		 */
		new_environ = (char **) malloc((i + 1) * sizeof(char *));
		for (i = 0; environ[i] != NULL; i++)
			new_environ[i] = strdup(environ[i]);
		new_environ[i] = NULL;
		environ = new_environ;
	}
#endif   /* PS_USE_CLOBBER_ARGV */

#if defined(PS_USE_CHANGE_ARGV) || defined(PS_USE_CLOBBER_ARGV)

	/*
	 * If we're going to change the original argv[] then make a copy for
	 * argument parsing purposes.
	 *
	 * (NB: do NOT think to remove the copying of argv[], even though
	 * postmaster.c finishes looking at argv[] long before we ever consider
	 * changing the ps display.  On some platforms, getopt() keeps pointers
	 * into the argv array, and will get horribly confused when it is
	 * re-called to analyze a subprocess' argument string if the argv storage
	 * has been clobbered meanwhile.  Other platforms have other dependencies
	 * on argv[].
	 */
	{
		char	  **new_argv;
		int			i;

		new_argv = (char **) malloc((argc + 1) * sizeof(char *));
		for (i = 0; i < argc; i++)
			new_argv[i] = strdup(argv[i]);
		new_argv[argc] = NULL;

#if defined(__darwin__)

		/*
		 * Darwin (and perhaps other NeXT-derived platforms?) has a static
		 * copy of the argv pointer, which we may fix like so:
		 */
		*_NSGetArgv() = new_argv;
#endif

		argv = new_argv;
	}
#endif   /* PS_USE_CHANGE_ARGV or PS_USE_CLOBBER_ARGV */

	return argv;
}

/*
 * Call this once during subprocess startup to set the identification
 * values.  At this point, the original argv[] array may be overwritten.
 */
void
init_ps_display(const char *username, const char *dbname,
				const char *host_info, const char *initial_str)
{
	Assert(username);
	Assert(dbname);
	Assert(host_info);

#ifndef PS_USE_NONE
	/* no ps display for stand-alone backend */
	if (!IsUnderPostmaster)
		return;

	/* no ps display if you didn't call save_ps_display_args() */
	if (!save_argv)
		return;

#ifdef PS_USE_CLOBBER_ARGV
	/* If ps_buffer is a pointer, it might still be null */
	if (!ps_buffer)
		return;
#endif

	/*
	 * Overwrite argv[] to point at appropriate space, if needed
	 */

#ifdef PS_USE_CHANGE_ARGV
	save_argv[0] = ps_buffer;
	save_argv[1] = NULL;
#endif   /* PS_USE_CHANGE_ARGV */

#ifdef PS_USE_CLOBBER_ARGV
	{
		int			i;

		/* make extra argv slots point at end_of_area (a NUL) */
		for (i = 1; i < save_argc; i++)
			save_argv[i] = ps_buffer + ps_buffer_size;
	}
#endif   /* PS_USE_CLOBBER_ARGV */

	/*
	 * Make fixed prefix of ps display.
	 */

#ifdef PS_USE_SETPROCTITLE

	/*
	 * apparently setproctitle() already adds a `progname:' prefix to the ps
	 * line
	 */
#define PROGRAM_NAME_PREFIX ""
#else
#define PROGRAM_NAME_PREFIX "postgres: "
#endif

	if (*cluster_name == '\0')
	{
		snprintf(ps_buffer, ps_buffer_size,
				 PROGRAM_NAME_PREFIX "%s %s %s ",
				 username, dbname, host_info);
	}
	else
	{
		snprintf(ps_buffer, ps_buffer_size,
				 PROGRAM_NAME_PREFIX "%s: %s %s %s ",
				 cluster_name, username, dbname, host_info);
	}

	ps_buffer_cur_len = ps_buffer_fixed_size = strlen(ps_buffer);

	set_ps_display(initial_str, true);
#endif   /* not PS_USE_NONE */
}



/*
 * Call this to update the ps status display to a fixed prefix plus an
 * indication of what you're currently doing passed in the argument.
 */
void
set_ps_display(const char *activity, bool force)
{
#ifndef PS_USE_NONE
	/* update_process_title=off disables updates, unless force = true */
	if (!force && !update_process_title)
		return;

	/* no ps display for stand-alone backend */
	if (!IsUnderPostmaster)
		return;

#ifdef PS_USE_CLOBBER_ARGV
	/* If ps_buffer is a pointer, it might still be null */
	if (!ps_buffer)
		return;
#endif

	/* Update ps_buffer to contain both fixed part and activity */
	strlcpy(ps_buffer + ps_buffer_fixed_size, activity,
			ps_buffer_size - ps_buffer_fixed_size);
	ps_buffer_cur_len = strlen(ps_buffer);

	/* Transmit new setting to kernel, if necessary */

#ifdef PS_USE_SETPROCTITLE
	setproctitle("%s", ps_buffer);
#endif

#ifdef PS_USE_PSTAT
	{
		union pstun pst;

		pst.pst_command = ps_buffer;
		pstat(PSTAT_SETCMD, pst, ps_buffer_cur_len, 0, 0);
	}
#endif   /* PS_USE_PSTAT */

#ifdef PS_USE_PS_STRINGS
	PS_STRINGS->ps_nargvstr = 1;
	PS_STRINGS->ps_argvstr = ps_buffer;
#endif   /* PS_USE_PS_STRINGS */

#ifdef PS_USE_CLOBBER_ARGV
	/* pad unused memory; need only clobber remainder of old status string */
	if (last_status_len > ps_buffer_cur_len)
		MemSet(ps_buffer + ps_buffer_cur_len, PS_PADDING,
			   last_status_len - ps_buffer_cur_len);
	last_status_len = ps_buffer_cur_len;
#endif   /* PS_USE_CLOBBER_ARGV */

#ifdef PS_USE_WIN32
	{
		/*
		 * Win32 does not support showing any changed arguments. To make it at
		 * all possible to track which backend is doing what, we create a
		 * named object that can be viewed with for example Process Explorer.
		 */
		static HANDLE ident_handle = INVALID_HANDLE_VALUE;
		char		name[PS_BUFFER_SIZE + 32];

		if (ident_handle != INVALID_HANDLE_VALUE)
			CloseHandle(ident_handle);

		sprintf(name, "pgident(%d): %s", MyProcPid, ps_buffer);

		ident_handle = CreateEvent(NULL, TRUE, FALSE, name);
	}
#endif   /* PS_USE_WIN32 */
#endif   /* not PS_USE_NONE */
}


/*
 * Returns what's currently in the ps display, in case someone needs
 * it.  Note that only the activity part is returned.  On some platforms
 * the string will not be null-terminated, so return the effective
 * length into *displen.
 */
const char *
get_ps_display(int *displen)
{
#ifdef PS_USE_CLOBBER_ARGV
	/* If ps_buffer is a pointer, it might still be null */
	if (!ps_buffer)
	{
		*displen = 0;
		return "";
	}
#endif

	*displen = (int) (ps_buffer_cur_len - ps_buffer_fixed_size);

	return ps_buffer + ps_buffer_fixed_size;
}
