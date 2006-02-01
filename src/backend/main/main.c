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
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/main/main.c,v 1.96.2.3 2006/02/01 00:32:05 momjian Exp $
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
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/postmaster.h"
#include "tcop/tcopprot.h"
#include "utils/help_config.h"
#include "utils/pg_locale.h"
#include "utils/ps_status.h"
#ifdef WIN32
#include "libpq/pqsignal.h"
#endif

const char *progname;

int
main(int argc, char *argv[])
{
#ifndef WIN32
	struct passwd *pw;
#endif
	char	   *pw_name_persist;

	/*
	 * Place platform-specific startup hacks here.	This is the right place to
	 * put code that must be executed early in launch of either a postmaster,
	 * a standalone backend, or a standalone bootstrap run. Note that this
	 * code will NOT be executed when a backend or sub-bootstrap run is forked
	 * by the postmaster.
	 *
	 * XXX The need for code here is proof that the platform in question is
	 * too brain-dead to provide a standard C execution environment without
	 * help. Avoid adding more here, if you can.
	 */

#if defined(__alpha)			/* no __alpha__ ? */
#ifdef NOFIXADE
	int			buffer[] = {SSIN_UACPROC, UAC_SIGBUS | UAC_NOPRINT};
#endif
#endif   /* __alpha */

#ifdef WIN32
	char	   *env_locale;
#endif

	progname = get_progname(argv[0]);

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
					 argv[0], strerror(errno));
#endif
#endif   /* NOFIXADE */

#if defined(WIN32)
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
						 argv[0], err);
			exit(1);
		}
	}
#endif

#ifdef __BEOS__
	/* BeOS-specific actions on startup */
	beos_startup(argc, argv);
#endif

	/*
	 * Not-quite-so-platform-specific startup environment checks. Still best
	 * to minimize these.
	 */

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

	if ((env_locale = getenv("LC_COLLATE")) != NULL)
		pg_perm_setlocale(LC_COLLATE, env_locale);
	else
		pg_perm_setlocale(LC_COLLATE, "");

	if ((env_locale = getenv("LC_CTYPE")) != NULL)
		pg_perm_setlocale(LC_CTYPE, env_locale);
	else
		pg_perm_setlocale(LC_CTYPE, "");
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
	 * Skip permission checks if we're just trying to do --help or --version;
	 * otherwise root will get unhelpful failure messages from initdb.
	 */
	if (!(argc > 1
		  && (strcmp(argv[1], "--help") == 0 ||
			  strcmp(argv[1], "-?") == 0 ||
			  strcmp(argv[1], "--version") == 0 ||
			  strcmp(argv[1], "-V") == 0)))
	{
#ifndef WIN32
#ifndef __BEOS__

		/*
		 * Make sure we are not running as root.
		 *
		 * BeOS currently runs everything as root :-(, so this check must be
		 * temporarily disabled there...
		 */
		if (geteuid() == 0)
		{
			write_stderr("\"root\" execution of the PostgreSQL server is not permitted.\n"
						 "The server must be started under an unprivileged user ID to prevent\n"
						 "possible system security compromise.  See the documentation for\n"
				  "more information on how to properly start the server.\n");
			exit(1);
		}
#endif   /* !__BEOS__ */

		/*
		 * Also make sure that real and effective uids are the same. Executing
		 * Postgres as a setuid program from a root shell is a security hole,
		 * since on many platforms a nefarious subroutine could setuid back to
		 * root if real uid is root.  (Since nobody actually uses Postgres as
		 * a setuid program, trying to actively fix this situation seems more
		 * trouble than it's worth; we'll just expend the effort to check for
		 * it.)
		 */
		if (getuid() != geteuid())
		{
			write_stderr("%s: real and effective user IDs must match\n",
						 argv[0]);
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
#endif   /* !WIN32 */
	}

	/*
	 * Now dispatch to one of PostmasterMain, PostgresMain, GucInfoMain,
	 * SubPostmasterMain, or BootstrapMain depending on the program name (and
	 * possibly first argument) we were called with. The lack of consistency
	 * here is historical.
	 */
	if (strcmp(progname, "postmaster") == 0)
	{
		/* Called as "postmaster" */
		exit(PostmasterMain(argc, argv));
	}

	/*
	 * If the first argument begins with "-fork", then invoke
	 * SubPostmasterMain.  This is used for forking postmaster child processes
	 * on systems where we can't simply fork.
	 */
#ifdef EXEC_BACKEND
	if (argc > 1 && strncmp(argv[1], "-fork", 5) == 0)
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

	/*
	 * If the first argument is "-boot", then invoke bootstrap mode. (This
	 * path is taken only for a standalone bootstrap process.)
	 */
	if (argc > 1 && strcmp(argv[1], "-boot") == 0)
		exit(BootstrapMain(argc, argv));

	/*
	 * If the first argument is "--describe-config", then invoke runtime
	 * configuration option display mode.
	 */
	if (argc > 1 && strcmp(argv[1], "--describe-config") == 0)
		exit(GucInfoMain());

	/*
	 * Otherwise we're a standalone backend.  Invoke PostgresMain, specifying
	 * current userid as the "authenticated" Postgres user name.
	 */
#ifndef WIN32
	pw = getpwuid(geteuid());
	if (pw == NULL)
	{
		write_stderr("%s: invalid effective UID: %d\n",
					 argv[0], (int) geteuid());
		exit(1);
	}
	/* Allocate new memory because later getpwuid() calls can overwrite it */
	pw_name_persist = strdup(pw->pw_name);
#else
	{
		long		namesize = 256 /* UNLEN */ + 1;

		pw_name_persist = malloc(namesize);
		if (!GetUserName(pw_name_persist, &namesize))
		{
			write_stderr("%s: could not determine user name (GetUserName failed)\n",
						 argv[0]);
			exit(1);
		}
	}
#endif   /* WIN32 */

	exit(PostgresMain(argc, argv, pw_name_persist));
}
