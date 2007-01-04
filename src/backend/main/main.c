/*-------------------------------------------------------------------------
 *
 * main.c
 *	  Stub main() routine for the postgres executable.
 *
 * This does some essential startup tasks for any incarnation of postgres
 * (postmaster, standalone backend, or standalone bootstrap mode) and then
 * dispatches to the proper FooMain() routine for the incarnation.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/main/main.c,v 1.105.2.1 2007/01/04 00:58:01 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <pwd.h>
#include <unistd.h>

#if defined(__alpha) && defined(__osf__)		/* no __alpha__ ? */
#include <sys/sysinfo.h>
#include "machine/hal_sysinfo.h"
#define ASSEMBLER
#include <sys/proc.h>
#undef ASSEMBLER
#endif

#if defined(__NetBSD__)
#include <sys/param.h>
#endif

#include "bootstrap/bootstrap.h"
#include "postmaster/postmaster.h"
#include "tcop/tcopprot.h"
#include "utils/help_config.h"
#include "utils/pg_locale.h"
#include "utils/ps_status.h"
#ifdef WIN32
#include "libpq/pqsignal.h"
#endif


const char *progname;


static void startup_hacks(const char *progname);
static void help(const char *progname);
static void check_root(const char *progname);
static char *get_current_username(const char *progname);



int
main(int argc, char *argv[])
{
	progname = get_progname(argv[0]);

	/*
	 * Platform-specific startup hacks
	 */
	startup_hacks(progname);

	/*
	 * Remember the physical location of the initially given argv[] array for
	 * possible use by ps display.	On some platforms, the argv[] storage must
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
	 * Set up locale information from environment.	Note that LC_CTYPE and
	 * LC_COLLATE will be overridden later from pg_control if we are in an
	 * already-initialized database.  We set them here so that they will be
	 * available to fill pg_control during initdb.	LC_MESSAGES will get set
	 * later during GUC option processing, but we set it here to allow startup
	 * error messages to be localized.
	 */

	set_pglocale_pgservice(argv[0], "postgres");

#ifdef WIN32

	/*
	 * Windows uses codepages rather than the environment, so we work around
	 * that by querying the environment explicitly first for LC_COLLATE and
	 * LC_CTYPE. We have to do this because initdb passes those values in the
	 * environment. If there is nothing there we fall back on the codepage.
	 */
	{
		char	   *env_locale;

		if ((env_locale = getenv("LC_COLLATE")) != NULL)
			pg_perm_setlocale(LC_COLLATE, env_locale);
		else
			pg_perm_setlocale(LC_COLLATE, "");

		if ((env_locale = getenv("LC_CTYPE")) != NULL)
			pg_perm_setlocale(LC_CTYPE, env_locale);
		else
			pg_perm_setlocale(LC_CTYPE, "");
	}
#else
	pg_perm_setlocale(LC_COLLATE, "");
	pg_perm_setlocale(LC_CTYPE, "");
#endif

#ifdef LC_MESSAGES
	pg_perm_setlocale(LC_MESSAGES, "");
#endif

	/*
	 * We keep these set to "C" always, except transiently in pg_locale.c; see
	 * that file for explanations.
	 */
	pg_perm_setlocale(LC_MONETARY, "C");
	pg_perm_setlocale(LC_NUMERIC, "C");
	pg_perm_setlocale(LC_TIME, "C");

	/*
	 * Now that we have absorbed as much as we wish to from the locale
	 * environment, remove any LC_ALL setting, so that the environment
	 * variables installed by pg_perm_setlocale have force.
	 */
	unsetenv("LC_ALL");

	/*
	 * Catch standard options before doing much else
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
			puts("postgres (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	/*
	 * Make sure we are not running as root.
	 */
	check_root(progname);

	/*
	 * Dispatch to one of various subprograms depending on first argument.
	 */

#ifdef EXEC_BACKEND
	if (argc > 1 && strncmp(argv[1], "--fork", 6) == 0)
		exit(SubPostmasterMain(argc, argv));
#endif

#ifdef WIN32

	/*
	 * Start our win32 signal implementation
	 *
	 * SubPostmasterMain() will do this for itself, but the remaining modes
	 * need it here
	 */
	pgwin32_signal_initialize();
#endif

	if (argc > 1 && strcmp(argv[1], "--boot") == 0)
		exit(BootstrapMain(argc, argv));

	if (argc > 1 && strcmp(argv[1], "--describe-config") == 0)
		exit(GucInfoMain());

	if (argc > 1 && strcmp(argv[1], "--single") == 0)
		exit(PostgresMain(argc, argv, get_current_username(progname)));

	exit(PostmasterMain(argc, argv));
}



/*
 * Place platform-specific startup hacks here.	This is the right
 * place to put code that must be executed early in launch of either a
 * postmaster, a standalone backend, or a standalone bootstrap run.
 * Note that this code will NOT be executed when a backend or
 * sub-bootstrap run is forked by the server.
 *
 * XXX The need for code here is proof that the platform in question
 * is too brain-dead to provide a standard C execution environment
 * without help.  Avoid adding more here, if you can.
 */
static void
startup_hacks(const char *progname)
{
#if defined(__alpha)			/* no __alpha__ ? */
#ifdef NOFIXADE
	int			buffer[] = {SSIN_UACPROC, UAC_SIGBUS | UAC_NOPRINT};
#endif
#endif   /* __alpha */


	/*
	 * On some platforms, unaligned memory accesses result in a kernel trap;
	 * the default kernel behavior is to emulate the memory access, but this
	 * results in a significant performance penalty. We ought to fix PG not to
	 * make such unaligned memory accesses, so this code disables the kernel
	 * emulation: unaligned accesses will result in SIGBUS instead.
	 */
#ifdef NOFIXADE

#if defined(ultrix4)
	syscall(SYS_sysmips, MIPS_FIXADE, 0, NULL, NULL, NULL);
#endif

#if defined(__alpha)			/* no __alpha__ ? */
	if (setsysinfo(SSI_NVPAIRS, buffer, 1, (caddr_t) NULL,
				   (unsigned long) NULL) < 0)
		write_stderr("%s: setsysinfo failed: %s\n",
					 progname, strerror(errno));
#endif
#endif   /* NOFIXADE */


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

		/* In case of general protection fault, don't show GUI popup box */
		SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
	}
#endif   /* WIN32 */
}


/*
 * Help display should match the options accepted by PostmasterMain()
 * and PostgresMain().
 */
static void
help(const char *progname)
{
	printf(_("%s is the PostgreSQL server.\n\n"), progname);
	printf(_("Usage:\n  %s [OPTION]...\n\n"), progname);
	printf(_("Options:\n"));
#ifdef USE_ASSERT_CHECKING
	printf(_("  -A 1|0          enable/disable run-time assert checking\n"));
#endif
	printf(_("  -B NBUFFERS     number of shared buffers\n"));
	printf(_("  -c NAME=VALUE   set run-time parameter\n"));
	printf(_("  -d 1-5          debugging level\n"));
	printf(_("  -D DATADIR      database directory\n"));
	printf(_("  -e              use European date input format (DMY)\n"));
	printf(_("  -F              turn fsync off\n"));
	printf(_("  -h HOSTNAME     host name or IP address to listen on\n"));
	printf(_("  -i              enable TCP/IP connections\n"));
	printf(_("  -k DIRECTORY    Unix-domain socket location\n"));
#ifdef USE_SSL
	printf(_("  -l              enable SSL connections\n"));
#endif
	printf(_("  -N MAX-CONNECT  maximum number of allowed connections\n"));
	printf(_("  -o OPTIONS      pass \"OPTIONS\" to each server process (obsolete)\n"));
	printf(_("  -p PORT         port number to listen on\n"));
	printf(_("  -s              show statistics after each query\n"));
	printf(_("  -S WORK-MEM     set amount of memory for sorts (in kB)\n"));
	printf(_("  --NAME=VALUE    set run-time parameter\n"));
	printf(_("  --describe-config  describe configuration parameters, then exit\n"));
	printf(_("  --help          show this help, then exit\n"));
	printf(_("  --version       output version information, then exit\n"));

	printf(_("\nDeveloper options:\n"));
	printf(_("  -f s|i|n|m|h    forbid use of some plan types\n"));
	printf(_("  -n              do not reinitialize shared memory after abnormal exit\n"));
	printf(_("  -O              allow system table structure changes\n"));
	printf(_("  -P              disable system indexes\n"));
	printf(_("  -t pa|pl|ex     show timings after each query\n"));
	printf(_("  -T              send SIGSTOP to all backend servers if one dies\n"));
	printf(_("  -W NUM          wait NUM seconds to allow attach from a debugger\n"));

	printf(_("\nOptions for single-user mode:\n"));
	printf(_("  --single        selects single-user mode (must be first argument)\n"));
	printf(_("  DBNAME          database name (defaults to user name)\n"));
	printf(_("  -d 0-5          override debugging level\n"));
	printf(_("  -E              echo statement before execution\n"));
	printf(_("  -j              do not use newline as interactive query delimiter\n"));
	printf(_("  -r FILENAME     send stdout and stderr to given file\n"));

	printf(_("\nOptions for bootstrapping mode:\n"));
	printf(_("  --boot          selects bootstrapping mode (must be first argument)\n"));
	printf(_("  DBNAME          database name (mandatory argument in bootstrapping mode)\n"));
	printf(_("  -r FILENAME     send stdout and stderr to given file\n"));
	printf(_("  -x NUM          internal use\n"));

	printf(_("\nPlease read the documentation for the complete list of run-time\n"
	 "configuration settings and how to set them on the command line or in\n"
			 "the configuration file.\n\n"
			 "Report bugs to <pgsql-bugs@postgresql.org>.\n"));
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
#endif   /* WIN32 */
}



static char *
get_current_username(const char *progname)
{
#ifndef WIN32
	struct passwd *pw;

	pw = getpwuid(geteuid());
	if (pw == NULL)
	{
		write_stderr("%s: invalid effective UID: %d\n",
					 progname, (int) geteuid());
		exit(1);
	}
	/* Allocate new memory because later getpwuid() calls can overwrite it. */
	return strdup(pw->pw_name);
#else
	long		namesize = 256 /* UNLEN */ + 1;
	char	   *name;

	name = malloc(namesize);
	if (!GetUserName(name, &namesize))
	{
		write_stderr("%s: could not determine user name (GetUserName failed)\n",
					 progname);
		exit(1);
	}

	return name;
#endif
}
