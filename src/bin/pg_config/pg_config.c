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
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 *
 * src/bin/pg_config/pg_config.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "common/config_info.h"
#include "port.h"

static const char *progname;

/*
 * Table of known information items
 *
 * Be careful to keep this in sync with the help() display.
 */
typedef struct
{
	const char *switchname;
	const char *configname;
} InfoItem;

static const InfoItem info_items[] = {
	{"--bindir", "BINDIR"},
	{"--docdir", "DOCDIR"},
	{"--htmldir", "HTMLDIR"},
	{"--includedir", "INCLUDEDIR"},
	{"--pkgincludedir", "PKGINCLUDEDIR"},
	{"--includedir-server", "INCLUDEDIR-SERVER"},
	{"--libdir", "LIBDIR"},
	{"--pkglibdir", "PKGLIBDIR"},
	{"--localedir", "LOCALEDIR"},
	{"--mandir", "MANDIR"},
	{"--sharedir", "SHAREDIR"},
	{"--sysconfdir", "SYSCONFDIR"},
	{"--pgxs", "PGXS"},
	{"--configure", "CONFIGURE"},
	{"--cc", "CC"},
	{"--cppflags", "CPPFLAGS"},
	{"--cflags", "CFLAGS"},
	{"--cflags_sl", "CFLAGS_SL"},
	{"--ldflags", "LDFLAGS"},
	{"--ldflags_ex", "LDFLAGS_EX"},
	{"--ldflags_sl", "LDFLAGS_SL"},
	{"--libs", "LIBS"},
	{"--version", "VERSION"},
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
	printf(_("Report bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}

static void
advice(void)
{
	fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
}

static void
show_item(const char *configname,
		  ConfigData *configdata,
		  size_t configdata_len)
{
	int			i;

	for (i = 0; i < configdata_len; i++)
	{
		if (strcmp(configname, configdata[i].name) == 0)
			printf("%s\n", configdata[i].setting);
	}
}

int
main(int argc, char **argv)
{
	ConfigData *configdata;
	size_t		configdata_len;
	char		my_exec_path[MAXPGPATH];
	int			i;
	int			j;

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

	if (find_my_exec(argv[0], my_exec_path) < 0)
	{
		fprintf(stderr, _("%s: could not find own program executable\n"), progname);
		exit(1);
	}

	configdata = get_configdata(my_exec_path, &configdata_len);
	/* no arguments -> print everything */
	if (argc < 2)
	{
		for (i = 0; i < configdata_len; i++)
			printf("%s = %s\n", configdata[i].name, configdata[i].setting);
		exit(0);
	}

	/* otherwise print requested items */
	for (i = 1; i < argc; i++)
	{
		for (j = 0; info_items[j].switchname != NULL; j++)
		{
			if (strcmp(argv[i], info_items[j].switchname) == 0)
			{
				show_item(info_items[j].configname,
						  configdata, configdata_len);
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
