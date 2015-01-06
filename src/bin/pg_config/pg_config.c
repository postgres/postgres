/*-------------------------------------------------------------------------
 *
 * pg_config.c
 *
 * This program reports various pieces of information about the
 * installed version of PostgreSQL.  Packages that interface to
 * PostgreSQL can use it to configure their build.
 *
 * This is a C implementation of the previous shell script written by
 * Peter Eisentraut <peter_e@gmx.net>, with adjustments made to
 * accommodate the possibility that the installation has been relocated from
 * the place originally configured.
 *
 * author of C translation: Andrew Dunstan	   mailto:andrew@dunslane.net
 *
 * This code is released under the terms of the PostgreSQL License.
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 *
 * src/bin/pg_config/pg_config.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "port.h"

static const char *progname;
static char mypath[MAXPGPATH];


/*
 * This function cleans up the paths for use with either cmd.exe or Msys
 * on Windows. We need them to use filenames without spaces, for which a
 * short filename is the safest equivalent, eg:
 *		C:/Progra~1/
 */
static void
cleanup_path(char *path)
{
#ifdef WIN32
	char	   *ptr;

	/*
	 * GetShortPathName() will fail if the path does not exist, or short names
	 * are disabled on this file system.  In both cases, we just return the
	 * original path.  This is particularly useful for --sysconfdir, which
	 * might not exist.
	 */
	GetShortPathName(path, path, MAXPGPATH - 1);

	/* Replace '\' with '/' */
	for (ptr = path; *ptr; ptr++)
	{
		if (*ptr == '\\')
			*ptr = '/';
	}
#endif
}


/*
 * For each piece of information known to pg_config, we define a subroutine
 * to print it.  This is probably overkill, but it avoids code duplication
 * and accidentally omitting items from the "all" display.
 */

static void
show_bindir(bool all)
{
	char		path[MAXPGPATH];
	char	   *lastsep;

	if (all)
		printf("BINDIR = ");
	/* assume we are located in the bindir */
	strcpy(path, mypath);
	lastsep = strrchr(path, '/');

	if (lastsep)
		*lastsep = '\0';

	cleanup_path(path);
	printf("%s\n", path);
}

static void
show_docdir(bool all)
{
	char		path[MAXPGPATH];

	if (all)
		printf("DOCDIR = ");
	get_doc_path(mypath, path);
	cleanup_path(path);
	printf("%s\n", path);
}

static void
show_htmldir(bool all)
{
	char		path[MAXPGPATH];

	if (all)
		printf("HTMLDIR = ");
	get_html_path(mypath, path);
	cleanup_path(path);
	printf("%s\n", path);
}

static void
show_includedir(bool all)
{
	char		path[MAXPGPATH];

	if (all)
		printf("INCLUDEDIR = ");
	get_include_path(mypath, path);
	cleanup_path(path);
	printf("%s\n", path);
}

static void
show_pkgincludedir(bool all)
{
	char		path[MAXPGPATH];

	if (all)
		printf("PKGINCLUDEDIR = ");
	get_pkginclude_path(mypath, path);
	cleanup_path(path);
	printf("%s\n", path);
}

static void
show_includedir_server(bool all)
{
	char		path[MAXPGPATH];

	if (all)
		printf("INCLUDEDIR-SERVER = ");
	get_includeserver_path(mypath, path);
	cleanup_path(path);
	printf("%s\n", path);
}

static void
show_libdir(bool all)
{
	char		path[MAXPGPATH];

	if (all)
		printf("LIBDIR = ");
	get_lib_path(mypath, path);
	cleanup_path(path);
	printf("%s\n", path);
}

static void
show_pkglibdir(bool all)
{
	char		path[MAXPGPATH];

	if (all)
		printf("PKGLIBDIR = ");
	get_pkglib_path(mypath, path);
	cleanup_path(path);
	printf("%s\n", path);
}

static void
show_localedir(bool all)
{
	char		path[MAXPGPATH];

	if (all)
		printf("LOCALEDIR = ");
	get_locale_path(mypath, path);
	cleanup_path(path);
	printf("%s\n", path);
}

static void
show_mandir(bool all)
{
	char		path[MAXPGPATH];

	if (all)
		printf("MANDIR = ");
	get_man_path(mypath, path);
	cleanup_path(path);
	printf("%s\n", path);
}

static void
show_sharedir(bool all)
{
	char		path[MAXPGPATH];

	if (all)
		printf("SHAREDIR = ");
	get_share_path(mypath, path);
	cleanup_path(path);
	printf("%s\n", path);
}

static void
show_sysconfdir(bool all)
{
	char		path[MAXPGPATH];

	if (all)
		printf("SYSCONFDIR = ");
	get_etc_path(mypath, path);
	cleanup_path(path);
	printf("%s\n", path);
}

static void
show_pgxs(bool all)
{
	char		path[MAXPGPATH];

	if (all)
		printf("PGXS = ");
	get_pkglib_path(mypath, path);
	strlcat(path, "/pgxs/src/makefiles/pgxs.mk", sizeof(path));
	cleanup_path(path);
	printf("%s\n", path);
}

static void
show_configure(bool all)
{
#ifdef VAL_CONFIGURE
	if (all)
		printf("CONFIGURE = ");
	printf("%s\n", VAL_CONFIGURE);
#else
	if (!all)
	{
		fprintf(stderr, _("not recorded\n"));
		exit(1);
	}
#endif
}

static void
show_cc(bool all)
{
#ifdef VAL_CC
	if (all)
		printf("CC = ");
	printf("%s\n", VAL_CC);
#else
	if (!all)
	{
		fprintf(stderr, _("not recorded\n"));
		exit(1);
	}
#endif
}

static void
show_cppflags(bool all)
{
#ifdef VAL_CPPFLAGS
	if (all)
		printf("CPPFLAGS = ");
	printf("%s\n", VAL_CPPFLAGS);
#else
	if (!all)
	{
		fprintf(stderr, _("not recorded\n"));
		exit(1);
	}
#endif
}

static void
show_cflags(bool all)
{
#ifdef VAL_CFLAGS
	if (all)
		printf("CFLAGS = ");
	printf("%s\n", VAL_CFLAGS);
#else
	if (!all)
	{
		fprintf(stderr, _("not recorded\n"));
		exit(1);
	}
#endif
}

static void
show_cflags_sl(bool all)
{
#ifdef VAL_CFLAGS_SL
	if (all)
		printf("CFLAGS_SL = ");
	printf("%s\n", VAL_CFLAGS_SL);
#else
	if (!all)
	{
		fprintf(stderr, _("not recorded\n"));
		exit(1);
	}
#endif
}

static void
show_ldflags(bool all)
{
#ifdef VAL_LDFLAGS
	if (all)
		printf("LDFLAGS = ");
	printf("%s\n", VAL_LDFLAGS);
#else
	if (!all)
	{
		fprintf(stderr, _("not recorded\n"));
		exit(1);
	}
#endif
}

static void
show_ldflags_ex(bool all)
{
#ifdef VAL_LDFLAGS_EX
	if (all)
		printf("LDFLAGS_EX = ");
	printf("%s\n", VAL_LDFLAGS_EX);
#else
	if (!all)
	{
		fprintf(stderr, _("not recorded\n"));
		exit(1);
	}
#endif
}

static void
show_ldflags_sl(bool all)
{
#ifdef VAL_LDFLAGS_SL
	if (all)
		printf("LDFLAGS_SL = ");
	printf("%s\n", VAL_LDFLAGS_SL);
#else
	if (!all)
	{
		fprintf(stderr, _("not recorded\n"));
		exit(1);
	}
#endif
}

static void
show_libs(bool all)
{
#ifdef VAL_LIBS
	if (all)
		printf("LIBS = ");
	printf("%s\n", VAL_LIBS);
#else
	if (!all)
	{
		fprintf(stderr, _("not recorded\n"));
		exit(1);
	}
#endif
}

static void
show_version(bool all)
{
	if (all)
		printf("VERSION = ");
	printf("PostgreSQL " PG_VERSION "\n");
}


/*
 * Table of known information items
 *
 * Be careful to keep this in sync with the help() display.
 */
typedef struct
{
	const char *switchname;
	void		(*show_func) (bool all);
} InfoItem;

static const InfoItem info_items[] = {
	{"--bindir", show_bindir},
	{"--docdir", show_docdir},
	{"--htmldir", show_htmldir},
	{"--includedir", show_includedir},
	{"--pkgincludedir", show_pkgincludedir},
	{"--includedir-server", show_includedir_server},
	{"--libdir", show_libdir},
	{"--pkglibdir", show_pkglibdir},
	{"--localedir", show_localedir},
	{"--mandir", show_mandir},
	{"--sharedir", show_sharedir},
	{"--sysconfdir", show_sysconfdir},
	{"--pgxs", show_pgxs},
	{"--configure", show_configure},
	{"--cc", show_cc},
	{"--cppflags", show_cppflags},
	{"--cflags", show_cflags},
	{"--cflags_sl", show_cflags_sl},
	{"--ldflags", show_ldflags},
	{"--ldflags_ex", show_ldflags_ex},
	{"--ldflags_sl", show_ldflags_sl},
	{"--libs", show_libs},
	{"--version", show_version},
	{NULL, NULL}
};


static void
help(void)
{
	printf(_("\n%s provides information about the installed version of PostgreSQL.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]...\n\n"), progname);
	printf(_("Options:\n"));
	printf(_("  --bindir              show location of user executables\n"));
	printf(_("  --docdir              show location of documentation files\n"));
	printf(_("  --htmldir             show location of HTML documentation files\n"));
	printf(_("  --includedir          show location of C header files of the client\n"
			 "                        interfaces\n"));
	printf(_("  --pkgincludedir       show location of other C header files\n"));
	printf(_("  --includedir-server   show location of C header files for the server\n"));
	printf(_("  --libdir              show location of object code libraries\n"));
	printf(_("  --pkglibdir           show location of dynamically loadable modules\n"));
	printf(_("  --localedir           show location of locale support files\n"));
	printf(_("  --mandir              show location of manual pages\n"));
	printf(_("  --sharedir            show location of architecture-independent support files\n"));
	printf(_("  --sysconfdir          show location of system-wide configuration files\n"));
	printf(_("  --pgxs                show location of extension makefile\n"));
	printf(_("  --configure           show options given to \"configure\" script when\n"
			 "                        PostgreSQL was built\n"));
	printf(_("  --cc                  show CC value used when PostgreSQL was built\n"));
	printf(_("  --cppflags            show CPPFLAGS value used when PostgreSQL was built\n"));
	printf(_("  --cflags              show CFLAGS value used when PostgreSQL was built\n"));
	printf(_("  --cflags_sl           show CFLAGS_SL value used when PostgreSQL was built\n"));
	printf(_("  --ldflags             show LDFLAGS value used when PostgreSQL was built\n"));
	printf(_("  --ldflags_ex          show LDFLAGS_EX value used when PostgreSQL was built\n"));
	printf(_("  --ldflags_sl          show LDFLAGS_SL value used when PostgreSQL was built\n"));
	printf(_("  --libs                show LIBS value used when PostgreSQL was built\n"));
	printf(_("  --version             show the PostgreSQL version\n"));
	printf(_("  -?, --help            show this help, then exit\n"));
	printf(_("\nWith no arguments, all known items are shown.\n\n"));
	printf(_("Report bugs to <pgsql-bugs@postgresql.org>.\n"));
}

static void
advice(void)
{
	fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
}

static void
show_all(void)
{
	int			i;

	for (i = 0; info_items[i].switchname != NULL; i++)
	{
		(*info_items[i].show_func) (true);
	}
}

int
main(int argc, char **argv)
{
	int			i;
	int			j;
	int			ret;

	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_config"));

	progname = get_progname(argv[0]);

	/* check for --help */
	for (i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-?") == 0)
		{
			help();
			exit(0);
		}
	}

	ret = find_my_exec(argv[0], mypath);

	if (ret)
	{
		fprintf(stderr, _("%s: could not find own program executable\n"), progname);
		exit(1);
	}

	/* no arguments -> print everything */
	if (argc < 2)
	{
		show_all();
		exit(0);
	}

	for (i = 1; i < argc; i++)
	{
		for (j = 0; info_items[j].switchname != NULL; j++)
		{
			if (strcmp(argv[i], info_items[j].switchname) == 0)
			{
				(*info_items[j].show_func) (false);
				break;
			}
		}
		if (info_items[j].switchname == NULL)
		{
			fprintf(stderr, _("%s: invalid argument: %s\n"),
					progname, argv[i]);
			advice();
			exit(1);
		}
	}

	return 0;
}
