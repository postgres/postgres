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
 * accomodate the possibility that the installation has been relocated from
 * the place originally configured.
 *
 * author of C translation: Andrew Dunstan     mailto:andrew@dunslane.net
 *
 * This code is released under the terms of the PostgreSQL License.
 *
 * Portions Copyright (c) 1996-2004, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/pg_config/pg_config.c,v 1.5 2004/08/29 04:13:01 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "port.h"
#include <stdio.h>

#define _(x) gettext((x))

static char *progname;

static void
help()
{
	printf(_("\n%s provides information about the installed version of PostgreSQL.\n\n"),progname);
	printf(_("Usage:\n"));
	printf(_("  %s OPTION...\n\n"),progname);
	printf(_("Options:\n"));
	printf(_("  --bindir              show location of user executables\n"));
	printf(_("  --includedir          show location of C header files of the client\n"));
	printf(_("                        interfaces\n"));
	printf(_("  --includedir-server   show location of C header files for the server\n"));
	printf(_("  --libdir              show location of object code libraries\n"));
	printf(_("  --pkglibdir           show location of dynamically loadable modules\n"));
	printf(_("  --pgxs                show location of extension makefile\n"));
	printf(_("  --configure           show options given to 'configure' script when\n"));
	printf(_("                        PostgreSQL was built\n"));
	printf(_("  --version             show the PostgreSQL version, then exit\n"));
	printf(_("  --help                show this help, then exit\n\n"));
	printf(_("Report bugs to <pgsql-bugs@postgresql.org>.\n"));
}

static void
advice()
{
	fprintf(stderr,_("\nTry \"%s --help\" for more information\n"),progname);
}


int
main (int argc, char ** argv)
{
	int i;
	int ret;
	char mypath[MAXPGPATH];
	char otherpath[MAXPGPATH];

    progname = (char *)get_progname(argv[0]);

	if (argc < 2)
	{
		fprintf(stderr,_("%s: argument required\n"),progname);
		advice();
		exit(1);
	}

	for (i=1; i < argc; i++)
	{
		if (strcmp(argv[i],"--bindir") == 0 ||
			strcmp(argv[i],"--includedir") == 0 ||
			strcmp(argv[i],"--includedir-server") == 0 ||
			strcmp(argv[i],"--libdir") == 0 ||
			strcmp(argv[i],"--pkglibdir") == 0 ||
			strcmp(argv[i],"--pgxs") == 0 ||
			strcmp(argv[i],"--configure") == 0)
		{
			/* come back to these later */
			continue; 
		}

		if (strcmp(argv[i],"--version") == 0)
		{
			printf("PostgreSQL " PG_VERSION "\n");
			exit(0);
		}
		if (strcmp(argv[i],"--help") == 0 || strcmp(argv[i],"-?") == 0)
		{
			help();
			exit(0);
		}
		fprintf(stderr,_("%s: invalid argument: %s\n"),progname,argv[i]);
		advice();
		exit(1);
	}

	ret = find_my_exec(argv[0],mypath);

	if (ret)
	{
		fprintf(stderr,"%s: could not locate my own executable\n",progname);
		exit(1);
	}

	for (i=1; i < argc; i++)
	{
		if (strcmp(argv[i],"--configure") == 0)
		{
			/* the VAL_CONFIGURE macro must be defined by the Makefile */
			printf("%s\n",VAL_CONFIGURE);
			continue;
		}

		if (strcmp(argv[i],"--bindir") == 0)
		{
			/* assume we are located in the bindir */
			char *lastsep;
			strcpy(otherpath,mypath);
			lastsep = strrchr(otherpath,'/');
			if (lastsep)
				*lastsep = '\0';
		}
		else if (strcmp(argv[i],"--includedir") == 0)
			get_include_path(mypath,otherpath);
		else if (strcmp(argv[i],"--includedir-server") ==0)
			get_includeserver_path(mypath,otherpath);
		else if (strcmp(argv[i],"--libdir") == 0)
			get_lib_path(mypath,otherpath);
		else if (strcmp(argv[i],"--pkglibdir") == 0)
			get_pkglib_path(mypath,otherpath);
		else if (strcmp(argv[i],"--pgxs") == 0)
		{
			get_pkglib_path(mypath,otherpath);
			strncat(otherpath, "/pgxs/src/makefiles/pgxs.mk", MAXPGPATH-1);
		}

		printf("%s\n",otherpath);
	}

	return 0;
}
