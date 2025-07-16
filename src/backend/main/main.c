/*-------------------------------------------------------------------------
 *
 * main.c
 *	  Stub main() routine for the postgres executable.
 *
 * This does some essential startup tasks for any incarnation of postgres
 * (postmaster, standalone backend, standalone bootstrap process, or a
 * separately exec'd child of a postmaster) and then dispatches to the
 * proper FooMain() routine for the incarnation.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/main/main.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#if defined(WIN32)
#include <crtdbg.h>
#endif

#if defined(__NetBSD__)
#include <sys/param.h>
#endif

#include "bootstrap/bootstrap.h"
#include "common/username.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "tcop/tcopprot.h"
#include "utils/help_config.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/ps_status.h"


const char *progname;
static bool reached_main = false;

/* names of special must-be-first options for dispatching to subprograms */
static const char *const DispatchOptionNames[] =
{
	[DISPATCH_CHECK] = "check",
	[DISPATCH_BOOT] = "boot",
	[DISPATCH_FORKCHILD] = "forkchild",
	[DISPATCH_DESCRIBE_CONFIG] = "describe-config",
	[DISPATCH_SINGLE] = "single",
	/* DISPATCH_POSTMASTER has no name */
};

StaticAssertDecl(lengthof(DispatchOptionNames) == DISPATCH_POSTMASTER,
				 "array length mismatch");

static void startup_hacks(const char *progname);
static void init_locale(const char *categoryname, int category, const char *locale);
static void help(const char *progname);
static void check_root(const char *progname);


/*
 * Any Postgres server process begins execution here.
 */
int
main(int argc, char *argv[])
{
	bool		do_check_root = true;
	DispatchOption dispatch_option = DISPATCH_POSTMASTER;

	reached_main = true;

	/*
	 * If supported on the current platform, set up a handler to be called if
	 * the backend/postmaster crashes with a fatal signal or exception.
	 */
#if defined(WIN32)
	pgwin32_install_crashdump_handler();
#endif

	progname = get_progname(argv[0]);

	/*
	 * Platform-specific startup hacks
	 */
	startup_hacks(progname);

	/*
	 * Remember the physical location of the initially given argv[] array for
	 * possible use by ps display.  On some platforms, the argv[] storage must
	 * be overwritten in order to set the process title for ps. In such cases
	 * save_ps_display_args makes and returns a new copy of the argv[] array.
	 *
	 * save_ps_display_args may also move the environment strings to make
	 * extra room. Therefore this should be done as early as possible during
	 * startup, to avoid entanglements with code that might save a getenv()
	 * result pointer.
	 */
	argv = save_ps_display_args(argc, argv);

	/*
	 * Fire up essential subsystems: error and memory management
	 *
	 * Code after this point is allowed to use elog/ereport, though
	 * localization of messages may not work right away, and messages won't go
	 * anywhere but stderr until GUC settings get loaded.
	 */
	MyProcPid = getpid();
	MemoryContextInit();

	/*
	 * Set reference point for stack-depth checking.  (There's no point in
	 * enabling this before error reporting works.)
	 */
	(void) set_stack_base();

	/*
	 * Set up locale information
	 */
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("postgres"));

	/*
	 * Collation is handled by pg_locale.c, and the behavior is dependent on
	 * the provider. strcoll(), etc., should not be called directly.
	 */
	init_locale("LC_COLLATE", LC_COLLATE, "C");

	/*
	 * In the postmaster, absorb the environment value for LC_CTYPE.
	 * Individual backends will change it later to pg_database.datctype, but
	 * the postmaster cannot do that.  If we leave it set to "C" then message
	 * localization might not work well in the postmaster.
	 */
	init_locale("LC_CTYPE", LC_CTYPE, "");

	/*
	 * LC_MESSAGES will get set later during GUC option processing, but we set
	 * it here to allow startup error messages to be localized.
	 */
#ifdef LC_MESSAGES
	init_locale("LC_MESSAGES", LC_MESSAGES, "");
#endif

	/* We keep these set to "C" always.  See pg_locale.c for explanation. */
	init_locale("LC_MONETARY", LC_MONETARY, "C");
	init_locale("LC_NUMERIC", LC_NUMERIC, "C");
	init_locale("LC_TIME", LC_TIME, "C");

	/*
	 * Now that we have absorbed as much as we wish to from the locale
	 * environment, remove any LC_ALL setting, so that the environment
	 * variables installed by pg_perm_setlocale have force.
	 */
	unsetenv("LC_ALL");

	/*
	 * Catch standard options before doing much else, in particular before we
	 * insist on not being root.
	 */
	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			help(progname);
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			fputs(PG_BACKEND_VERSIONSTR, stdout);
			exit(0);
		}

		/*
		 * In addition to the above, we allow "--describe-config" and "-C var"
		 * to be called by root.  This is reasonably safe since these are
		 * read-only activities.  The -C case is important because pg_ctl may
		 * try to invoke it while still holding administrator privileges on
		 * Windows.  Note that while -C can normally be in any argv position,
		 * if you want to bypass the root check you must put it first.  This
		 * reduces the risk that we might misinterpret some other mode's -C
		 * switch as being the postmaster/postgres one.
		 */
		if (strcmp(argv[1], "--describe-config") == 0)
			do_check_root = false;
		else if (argc > 2 && strcmp(argv[1], "-C") == 0)
			do_check_root = false;
	}

	/*
	 * Make sure we are not running as root, unless it's safe for the selected
	 * option.
	 */
	if (do_check_root)
		check_root(progname);

	/*
	 * Dispatch to one of various subprograms depending on first argument.
	 */

	if (argc > 1 && argv[1][0] == '-' && argv[1][1] == '-')
		dispatch_option = parse_dispatch_option(&argv[1][2]);

	switch (dispatch_option)
	{
		case DISPATCH_CHECK:
			BootstrapModeMain(argc, argv, true);
			break;
		case DISPATCH_BOOT:
			BootstrapModeMain(argc, argv, false);
			break;
		case DISPATCH_FORKCHILD:
#ifdef EXEC_BACKEND
			SubPostmasterMain(argc, argv);
#else
			Assert(false);		/* should never happen */
#endif
			break;
		case DISPATCH_DESCRIBE_CONFIG:
			GucInfoMain();
			break;
		case DISPATCH_SINGLE:
			PostgresSingleUserMain(argc, argv,
								   strdup(get_user_name_or_exit(progname)));
			break;
		case DISPATCH_POSTMASTER:
			PostmasterMain(argc, argv);
			break;
	}

	/* the functions above should not return */
	abort();
}

/*
 * Returns the matching DispatchOption value for the given option name.  If no
 * match is found, DISPATCH_POSTMASTER is returned.
 */
DispatchOption
parse_dispatch_option(const char *name)
{
	for (int i = 0; i < lengthof(DispatchOptionNames); i++)
	{
		/*
		 * Unlike the other dispatch options, "forkchild" takes an argument,
		 * so we just look for the prefix for that one.  For non-EXEC_BACKEND
		 * builds, we never want to return DISPATCH_FORKCHILD, so skip over it
		 * in that case.
		 */
		if (i == DISPATCH_FORKCHILD)
		{
#ifdef EXEC_BACKEND
			if (strncmp(DispatchOptionNames[DISPATCH_FORKCHILD], name,
						strlen(DispatchOptionNames[DISPATCH_FORKCHILD])) == 0)
				return DISPATCH_FORKCHILD;
#endif
			continue;
		}

		if (strcmp(DispatchOptionNames[i], name) == 0)
			return (DispatchOption) i;
	}

	/* no match means this is a postmaster */
	return DISPATCH_POSTMASTER;
}

/*
 * Place platform-specific startup hacks here.  This is the right
 * place to put code that must be executed early in the launch of any new
 * server process.  Note that this code will NOT be executed when a backend
 * or sub-bootstrap process is forked, unless we are in a fork/exec
 * environment (ie EXEC_BACKEND is defined).
 *
 * XXX The need for code here is proof that the platform in question
 * is too brain-dead to provide a standard C execution environment
 * without help.  Avoid adding more here, if you can.
 */
static void
startup_hacks(const char *progname)
{
	/*
	 * Windows-specific execution environment hacking.
	 */
#ifdef WIN32
	{
		WSADATA		wsaData;
		int			err;

		/* Make output streams unbuffered by default */
		setvbuf(stdout, NULL, _IONBF, 0);
		setvbuf(stderr, NULL, _IONBF, 0);

		/* Prepare Winsock */
		err = WSAStartup(MAKEWORD(2, 2), &wsaData);
		if (err != 0)
		{
			write_stderr("%s: WSAStartup failed: %d\n",
						 progname, err);
			exit(1);
		}

		/*
		 * By default abort() only generates a crash-dump in *non* debug
		 * builds. As our Assert() / ExceptionalCondition() uses abort(),
		 * leaving the default in place would make debugging harder.
		 *
		 * MINGW's own C runtime doesn't have _set_abort_behavior(). When
		 * targeting Microsoft's UCRT with mingw, it never links to the debug
		 * version of the library and thus doesn't need the call to
		 * _set_abort_behavior() either.
		 */
#if !defined(__MINGW32__) && !defined(__MINGW64__)
		_set_abort_behavior(_CALL_REPORTFAULT | _WRITE_ABORT_MSG,
							_CALL_REPORTFAULT | _WRITE_ABORT_MSG);
#endif							/* !defined(__MINGW32__) &&
								 * !defined(__MINGW64__) */

		/*
		 * SEM_FAILCRITICALERRORS causes more errors to be reported to
		 * callers.
		 *
		 * We used to also specify SEM_NOGPFAULTERRORBOX, but that prevents
		 * windows crash reporting from working. Which includes registered
		 * just-in-time debuggers, making it unnecessarily hard to debug
		 * problems on windows. Now we try to disable sources of popups
		 * separately below (note that SEM_NOGPFAULTERRORBOX did not actually
		 * prevent all sources of such popups).
		 */
		SetErrorMode(SEM_FAILCRITICALERRORS);

		/*
		 * Show errors on stderr instead of popup box (note this doesn't
		 * affect errors originating in the C runtime, see below).
		 */
		_set_error_mode(_OUT_TO_STDERR);

		/*
		 * In DEBUG builds, errors, including assertions, C runtime errors are
		 * reported via _CrtDbgReport. By default such errors are displayed
		 * with a popup (even with NOGPFAULTERRORBOX), preventing forward
		 * progress. Instead report such errors stderr (and the debugger).
		 * This is C runtime specific and thus the above incantations aren't
		 * sufficient to suppress these popups.
		 */
		_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
		_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
		_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
		_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
		_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
		_CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
	}
#endif							/* WIN32 */
}


/*
 * Make the initial permanent setting for a locale category.  If that fails,
 * perhaps due to LC_foo=invalid in the environment, use locale C.  If even
 * that fails, perhaps due to out-of-memory, the entire startup fails with it.
 * When this returns, we are guaranteed to have a setting for the given
 * category's environment variable.
 */
static void
init_locale(const char *categoryname, int category, const char *locale)
{
	if (pg_perm_setlocale(category, locale) == NULL &&
		pg_perm_setlocale(category, "C") == NULL)
		elog(FATAL, "could not adopt \"%s\" locale nor C locale for %s",
			 locale, categoryname);
}



/*
 * Help display should match the options accepted by PostmasterMain()
 * and PostgresMain().
 *
 * XXX On Windows, non-ASCII localizations of these messages only display
 * correctly if the console output code page covers the necessary characters.
 * Messages emitted in write_console() do not exhibit this problem.
 */
static void
help(const char *progname)
{
	printf(_("%s is the PostgreSQL server.\n\n"), progname);
	printf(_("Usage:\n  %s [OPTION]...\n\n"), progname);
	printf(_("Options:\n"));
	printf(_("  -B NBUFFERS        number of shared buffers\n"));
	printf(_("  -c NAME=VALUE      set run-time parameter\n"));
	printf(_("  -C NAME            print value of run-time parameter, then exit\n"));
	printf(_("  -d 1-5             debugging level\n"));
	printf(_("  -D DATADIR         database directory\n"));
	printf(_("  -e                 use European date input format (DMY)\n"));
	printf(_("  -F                 turn fsync off\n"));
	printf(_("  -h HOSTNAME        host name or IP address to listen on\n"));
	printf(_("  -i                 enable TCP/IP connections (deprecated)\n"));
	printf(_("  -k DIRECTORY       Unix-domain socket location\n"));
#ifdef USE_SSL
	printf(_("  -l                 enable SSL connections\n"));
#endif
	printf(_("  -N MAX-CONNECT     maximum number of allowed connections\n"));
	printf(_("  -p PORT            port number to listen on\n"));
	printf(_("  -s                 show statistics after each query\n"));
	printf(_("  -S WORK-MEM        set amount of memory for sorts (in kB)\n"));
	printf(_("  -V, --version      output version information, then exit\n"));
	printf(_("  --NAME=VALUE       set run-time parameter\n"));
	printf(_("  --describe-config  describe configuration parameters, then exit\n"));
	printf(_("  -?, --help         show this help, then exit\n"));

	printf(_("\nDeveloper options:\n"));
	printf(_("  -f s|i|o|b|t|n|m|h forbid use of some plan types\n"));
	printf(_("  -O                 allow system table structure changes\n"));
	printf(_("  -P                 disable system indexes\n"));
	printf(_("  -t pa|pl|ex        show timings after each query\n"));
	printf(_("  -T                 send SIGABRT to all backend processes if one dies\n"));
	printf(_("  -W NUM             wait NUM seconds to allow attach from a debugger\n"));

	printf(_("\nOptions for single-user mode:\n"));
	printf(_("  --single           selects single-user mode (must be first argument)\n"));
	printf(_("  DBNAME             database name (defaults to user name)\n"));
	printf(_("  -d 0-5             override debugging level\n"));
	printf(_("  -E                 echo statement before execution\n"));
	printf(_("  -j                 do not use newline as interactive query delimiter\n"));
	printf(_("  -r FILENAME        send stdout and stderr to given file\n"));

	printf(_("\nOptions for bootstrapping mode:\n"));
	printf(_("  --boot             selects bootstrapping mode (must be first argument)\n"));
	printf(_("  --check            selects check mode (must be first argument)\n"));
	printf(_("  DBNAME             database name (mandatory argument in bootstrapping mode)\n"));
	printf(_("  -r FILENAME        send stdout and stderr to given file\n"));

	printf(_("\nPlease read the documentation for the complete list of run-time\n"
			 "configuration settings and how to set them on the command line or in\n"
			 "the configuration file.\n\n"
			 "Report bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}



static void
check_root(const char *progname)
{
#ifndef WIN32
	if (geteuid() == 0)
	{
		write_stderr("\"root\" execution of the PostgreSQL server is not permitted.\n"
					 "The server must be started under an unprivileged user ID to prevent\n"
					 "possible system security compromise.  See the documentation for\n"
					 "more information on how to properly start the server.\n");
		exit(1);
	}

	/*
	 * Also make sure that real and effective uids are the same. Executing as
	 * a setuid program from a root shell is a security hole, since on many
	 * platforms a nefarious subroutine could setuid back to root if real uid
	 * is root.  (Since nobody actually uses postgres as a setuid program,
	 * trying to actively fix this situation seems more trouble than it's
	 * worth; we'll just expend the effort to check for it.)
	 */
	if (getuid() != geteuid())
	{
		write_stderr("%s: real and effective user IDs must match\n",
					 progname);
		exit(1);
	}
#else							/* WIN32 */
	if (pgwin32_is_admin())
	{
		write_stderr("Execution of PostgreSQL by a user with administrative permissions is not\n"
					 "permitted.\n"
					 "The server must be started under an unprivileged user ID to prevent\n"
					 "possible system security compromises.  See the documentation for\n"
					 "more information on how to properly start the server.\n");
		exit(1);
	}
#endif							/* WIN32 */
}

/*
 * At least on linux, set_ps_display() breaks /proc/$pid/environ. The
 * sanitizer library uses /proc/$pid/environ to implement getenv() as it wants
 * to work independent of libc. When just using undefined and alignment
 * sanitizers, the sanitizer library is only initialized when the first error
 * occurs, by which time we've often already called set_ps_display(),
 * preventing the sanitizer libraries from seeing the options.
 *
 * We can work around that by defining __ubsan_default_options, a weak symbol
 * libsanitizer uses to get defaults from the application, and return
 * getenv("UBSAN_OPTIONS"). But only if main already was reached, so that we
 * don't end up relying on a not-yet-working getenv().
 *
 * As this function won't get called when not running a sanitizer, it doesn't
 * seem necessary to only compile it conditionally.
 */
const char *__ubsan_default_options(void);
const char *
__ubsan_default_options(void)
{
	/* don't call libc before it's guaranteed to be initialized */
	if (!reached_main)
		return "";

	return getenv("UBSAN_OPTIONS");
}
