/*--------------------------------------------------------------------
 * ps_status.c
 *
 * Routines to support changing the ps display of PostgreSQL backends
 * to contain some useful information. Mechanism differs wildly across
 * platforms.
 *
 * src/backend/utils/misc/ps_status.c
 *
 * Copyright (c) 2000-2025, PostgreSQL Global Development Group
 * various details abducted from various places
 *--------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>
#if defined(__darwin__)
#include <crt_externs.h>
#endif

#include "miscadmin.h"
#include "utils/guc.h"
#include "utils/ps_status.h"

#if !defined(WIN32) || defined(_MSC_VER)
extern char **environ;
#endif

/* GUC variable */
bool		update_process_title = DEFAULT_UPDATE_PROCESS_TITLE;

/*
 * Alternative ways of updating ps display:
 *
 * PS_USE_SETPROCTITLE_FAST
 *	   use the function setproctitle_fast(const char *, ...)
 *	   (FreeBSD)
 * PS_USE_SETPROCTITLE
 *	   use the function setproctitle(const char *, ...)
 *	   (other BSDs)
 * PS_USE_CLOBBER_ARGV
 *	   write over the argv and environment area
 *	   (Linux and most SysV-like systems)
 * PS_USE_WIN32
 *	   push the string out as the name of a Windows event
 * PS_USE_NONE
 *	   don't update ps display
 *	   (This is the default, as it is safest.)
 */
#if defined(HAVE_SETPROCTITLE_FAST)
#define PS_USE_SETPROCTITLE_FAST
#elif defined(HAVE_SETPROCTITLE)
#define PS_USE_SETPROCTITLE
#elif defined(__linux__) || defined(__sun) || defined(__darwin__)
#define PS_USE_CLOBBER_ARGV
#elif defined(WIN32)
#define PS_USE_WIN32
#else
#define PS_USE_NONE
#endif


/* Different systems want the buffer padded differently */
#if defined(__linux__) || defined(__darwin__)
#define PS_PADDING '\0'
#else
#define PS_PADDING ' '
#endif


#ifndef PS_USE_NONE

#ifndef PS_USE_CLOBBER_ARGV
/* all but one option need a buffer to write their ps line in */
#define PS_BUFFER_SIZE 256
static char ps_buffer[PS_BUFFER_SIZE];
static const size_t ps_buffer_size = PS_BUFFER_SIZE;
#else							/* PS_USE_CLOBBER_ARGV */
static char *ps_buffer;			/* will point to argv area */
static size_t ps_buffer_size;	/* space determined at run time */
static size_t last_status_len;	/* use to minimize length of clobber */
#endif							/* PS_USE_CLOBBER_ARGV */

static size_t ps_buffer_cur_len;	/* nominal strlen(ps_buffer) */

static size_t ps_buffer_fixed_size; /* size of the constant prefix */

/*
 * Length of ps_buffer before the suffix was appended to the end, or 0 if we
 * didn't set a suffix.
 */
static size_t ps_buffer_nosuffix_len;

static void flush_ps_display(void);

#endif							/* not PS_USE_NONE */

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
 * that might try to hang onto a getenv() result.  But see hack for musl
 * within.)
 *
 * Note that in case of failure this cannot call elog() as that is not
 * initialized yet.  We rely on write_stderr() instead.
 */
char	  **
save_ps_display_args(int argc, char **argv)
{
	save_argc = argc;
	save_argv = argv;

#if defined(PS_USE_CLOBBER_ARGV)

	/*
	 * If we're going to overwrite the argv area, count the available space.
	 * Also move the environment strings to make additional room.
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
			{
				/*
				 * The musl dynamic linker keeps a static pointer to the
				 * initial value of LD_LIBRARY_PATH, if that is defined in the
				 * process's environment. Therefore, we must not overwrite the
				 * value of that setting and thus cannot advance end_of_area
				 * beyond it.  Musl does not define any identifying compiler
				 * symbol, so we have to do this unless we see a symbol
				 * identifying a Linux libc we know is safe.
				 */
#if defined(__linux__) && (!defined(__GLIBC__) && !defined(__UCLIBC__))
				if (strncmp(environ[i], "LD_LIBRARY_PATH=", 16) == 0)
				{
					/*
					 * We can overwrite the name, but stop at the equals sign.
					 * Future loop iterations will not find any more
					 * contiguous space, but we don't break early because we
					 * need to count the total number of environ[] entries.
					 */
					end_of_area = environ[i] + 15;
				}
				else
#endif
				{
					end_of_area = environ[i] + strlen(environ[i]);
				}
			}
		}

		ps_buffer = argv[0];
		last_status_len = ps_buffer_size = end_of_area - argv[0];

		/*
		 * move the environment out of the way
		 */
		new_environ = (char **) malloc((i + 1) * sizeof(char *));
		if (!new_environ)
		{
			write_stderr("out of memory\n");
			exit(1);
		}
		for (i = 0; environ[i] != NULL; i++)
		{
			new_environ[i] = strdup(environ[i]);
			if (!new_environ[i])
			{
				write_stderr("out of memory\n");
				exit(1);
			}
		}
		new_environ[i] = NULL;
		environ = new_environ;
	}

	/*
	 * If we're going to change the original argv[] then make a copy for
	 * argument parsing purposes.
	 *
	 * NB: do NOT think to remove the copying of argv[], even though
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
		if (!new_argv)
		{
			write_stderr("out of memory\n");
			exit(1);
		}
		for (i = 0; i < argc; i++)
		{
			new_argv[i] = strdup(argv[i]);
			if (!new_argv[i])
			{
				write_stderr("out of memory\n");
				exit(1);
			}
		}
		new_argv[argc] = NULL;

#if defined(__darwin__)

		/*
		 * macOS has a static copy of the argv pointer, which we may fix like
		 * so:
		 */
		*_NSGetArgv() = new_argv;
#endif

		argv = new_argv;
	}
#endif							/* PS_USE_CLOBBER_ARGV */

	return argv;
}

/*
 * Call this once during subprocess startup to set the identification
 * values.
 *
 * If fixed_part is NULL, a default will be obtained from MyBackendType.
 *
 * At this point, the original argv[] array may be overwritten.
 */
void
init_ps_display(const char *fixed_part)
{
#ifndef PS_USE_NONE
	bool		save_update_process_title;
#endif

	Assert(fixed_part || MyBackendType);
	if (!fixed_part)
		fixed_part = GetBackendTypeDesc(MyBackendType);

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

	/* make extra argv slots point at end_of_area (a NUL) */
	for (int i = 1; i < save_argc; i++)
		save_argv[i] = ps_buffer + ps_buffer_size;
#endif							/* PS_USE_CLOBBER_ARGV */

	/*
	 * Make fixed prefix of ps display.
	 */

#if defined(PS_USE_SETPROCTITLE) || defined(PS_USE_SETPROCTITLE_FAST)

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
				 PROGRAM_NAME_PREFIX "%s ",
				 fixed_part);
	}
	else
	{
		snprintf(ps_buffer, ps_buffer_size,
				 PROGRAM_NAME_PREFIX "%s: %s ",
				 cluster_name, fixed_part);
	}

	ps_buffer_cur_len = ps_buffer_fixed_size = strlen(ps_buffer);

	/*
	 * On the first run, force the update.
	 */
	save_update_process_title = update_process_title;
	update_process_title = true;
	set_ps_display("");
	update_process_title = save_update_process_title;
#endif							/* not PS_USE_NONE */
}

#ifndef PS_USE_NONE
/*
 * update_ps_display_precheck
 *		Helper function to determine if updating the process title is
 *		something that we need to do.
 */
static bool
update_ps_display_precheck(void)
{
	/* update_process_title=off disables updates */
	if (!update_process_title)
		return false;

	/* no ps display for stand-alone backend */
	if (!IsUnderPostmaster)
		return false;

#ifdef PS_USE_CLOBBER_ARGV
	/* If ps_buffer is a pointer, it might still be null */
	if (!ps_buffer)
		return false;
#endif

	return true;
}
#endif							/* not PS_USE_NONE */

/*
 * set_ps_display_suffix
 *		Adjust the process title to append 'suffix' onto the end with a space
 *		between it and the current process title.
 */
void
set_ps_display_suffix(const char *suffix)
{
#ifndef PS_USE_NONE
	size_t		len;

	/* first, check if we need to update the process title */
	if (!update_ps_display_precheck())
		return;

	/* if there's already a suffix, overwrite it */
	if (ps_buffer_nosuffix_len > 0)
		ps_buffer_cur_len = ps_buffer_nosuffix_len;
	else
		ps_buffer_nosuffix_len = ps_buffer_cur_len;

	len = strlen(suffix);

	/* check if we have enough space to append the suffix */
	if (ps_buffer_cur_len + len + 1 >= ps_buffer_size)
	{
		/* not enough space.  Check the buffer isn't full already */
		if (ps_buffer_cur_len < ps_buffer_size - 1)
		{
			/* append a space before the suffix */
			ps_buffer[ps_buffer_cur_len++] = ' ';

			/* just add what we can and fill the ps_buffer */
			memcpy(ps_buffer + ps_buffer_cur_len, suffix,
				   ps_buffer_size - ps_buffer_cur_len - 1);
			ps_buffer[ps_buffer_size - 1] = '\0';
			ps_buffer_cur_len = ps_buffer_size - 1;
		}
	}
	else
	{
		ps_buffer[ps_buffer_cur_len++] = ' ';
		memcpy(ps_buffer + ps_buffer_cur_len, suffix, len + 1);
		ps_buffer_cur_len = ps_buffer_cur_len + len;
	}

	Assert(strlen(ps_buffer) == ps_buffer_cur_len);

	/* and set the new title */
	flush_ps_display();
#endif							/* not PS_USE_NONE */
}

/*
 * set_ps_display_remove_suffix
 *		Remove the process display suffix added by set_ps_display_suffix
 */
void
set_ps_display_remove_suffix(void)
{
#ifndef PS_USE_NONE
	/* first, check if we need to update the process title */
	if (!update_ps_display_precheck())
		return;

	/* check we added a suffix */
	if (ps_buffer_nosuffix_len == 0)
		return;					/* no suffix */

	/* remove the suffix from ps_buffer */
	ps_buffer[ps_buffer_nosuffix_len] = '\0';
	ps_buffer_cur_len = ps_buffer_nosuffix_len;
	ps_buffer_nosuffix_len = 0;

	Assert(ps_buffer_cur_len == strlen(ps_buffer));

	/* and set the new title */
	flush_ps_display();
#endif							/* not PS_USE_NONE */
}

/*
 * Call this to update the ps status display to a fixed prefix plus an
 * indication of what you're currently doing passed in the argument.
 *
 * 'len' must be the same as strlen(activity)
 */
void
set_ps_display_with_len(const char *activity, size_t len)
{
	Assert(strlen(activity) == len);

#ifndef PS_USE_NONE
	/* first, check if we need to update the process title */
	if (!update_ps_display_precheck())
		return;

	/* wipe out any suffix when the title is completely changed */
	ps_buffer_nosuffix_len = 0;

	/* Update ps_buffer to contain both fixed part and activity */
	if (ps_buffer_fixed_size + len >= ps_buffer_size)
	{
		/* handle the case where ps_buffer doesn't have enough space */
		memcpy(ps_buffer + ps_buffer_fixed_size, activity,
			   ps_buffer_size - ps_buffer_fixed_size - 1);
		ps_buffer[ps_buffer_size - 1] = '\0';
		ps_buffer_cur_len = ps_buffer_size - 1;
	}
	else
	{
		memcpy(ps_buffer + ps_buffer_fixed_size, activity, len + 1);
		ps_buffer_cur_len = ps_buffer_fixed_size + len;
	}
	Assert(strlen(ps_buffer) == ps_buffer_cur_len);

	/* Transmit new setting to kernel, if necessary */
	flush_ps_display();
#endif							/* not PS_USE_NONE */
}

#ifndef PS_USE_NONE
static void
flush_ps_display(void)
{
#ifdef PS_USE_SETPROCTITLE
	setproctitle("%s", ps_buffer);
#elif defined(PS_USE_SETPROCTITLE_FAST)
	setproctitle_fast("%s", ps_buffer);
#endif

#ifdef PS_USE_CLOBBER_ARGV
	/* pad unused memory; need only clobber remainder of old status string */
	if (last_status_len > ps_buffer_cur_len)
		MemSet(ps_buffer + ps_buffer_cur_len, PS_PADDING,
			   last_status_len - ps_buffer_cur_len);
	last_status_len = ps_buffer_cur_len;
#endif							/* PS_USE_CLOBBER_ARGV */

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
#endif							/* PS_USE_WIN32 */
}
#endif							/* not PS_USE_NONE */

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

#ifndef PS_USE_NONE
	*displen = (int) (ps_buffer_cur_len - ps_buffer_fixed_size);

	return ps_buffer + ps_buffer_fixed_size;
#else
	*displen = 0;
	return "";
#endif
}
