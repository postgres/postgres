/*-------------------------------------------------------------------------
 *
 * config_info.c
 *		Common code for pg_config output
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/common/config_info.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "common/config_info.h"


/*
 * get_configdata(const char *my_exec_path, size_t *configdata_len)
 *
 * Get configure-time constants. The caller is responsible
 * for pfreeing the result.
 */
ConfigData *
get_configdata(const char *my_exec_path, size_t *configdata_len)
{
	ConfigData *configdata;
	char		path[MAXPGPATH];
	char	   *lastsep;
	int			i = 0;

	/* Adjust this to match the number of items filled below */
	*configdata_len = 23;
	configdata = (ConfigData *) palloc(*configdata_len * sizeof(ConfigData));

	configdata[i].name = pstrdup("BINDIR");
	strlcpy(path, my_exec_path, sizeof(path));
	lastsep = strrchr(path, '/');
	if (lastsep)
		*lastsep = '\0';
	cleanup_path(path);
	configdata[i].setting = pstrdup(path);
	i++;

	configdata[i].name = pstrdup("DOCDIR");
	get_doc_path(my_exec_path, path);
	cleanup_path(path);
	configdata[i].setting = pstrdup(path);
	i++;

	configdata[i].name = pstrdup("HTMLDIR");
	get_html_path(my_exec_path, path);
	cleanup_path(path);
	configdata[i].setting = pstrdup(path);
	i++;

	configdata[i].name = pstrdup("INCLUDEDIR");
	get_include_path(my_exec_path, path);
	cleanup_path(path);
	configdata[i].setting = pstrdup(path);
	i++;

	configdata[i].name = pstrdup("PKGINCLUDEDIR");
	get_pkginclude_path(my_exec_path, path);
	cleanup_path(path);
	configdata[i].setting = pstrdup(path);
	i++;

	configdata[i].name = pstrdup("INCLUDEDIR-SERVER");
	get_includeserver_path(my_exec_path, path);
	cleanup_path(path);
	configdata[i].setting = pstrdup(path);
	i++;

	configdata[i].name = pstrdup("LIBDIR");
	get_lib_path(my_exec_path, path);
	cleanup_path(path);
	configdata[i].setting = pstrdup(path);
	i++;

	configdata[i].name = pstrdup("PKGLIBDIR");
	get_pkglib_path(my_exec_path, path);
	cleanup_path(path);
	configdata[i].setting = pstrdup(path);
	i++;

	configdata[i].name = pstrdup("LOCALEDIR");
	get_locale_path(my_exec_path, path);
	cleanup_path(path);
	configdata[i].setting = pstrdup(path);
	i++;

	configdata[i].name = pstrdup("MANDIR");
	get_man_path(my_exec_path, path);
	cleanup_path(path);
	configdata[i].setting = pstrdup(path);
	i++;

	configdata[i].name = pstrdup("SHAREDIR");
	get_share_path(my_exec_path, path);
	cleanup_path(path);
	configdata[i].setting = pstrdup(path);
	i++;

	configdata[i].name = pstrdup("SYSCONFDIR");
	get_etc_path(my_exec_path, path);
	cleanup_path(path);
	configdata[i].setting = pstrdup(path);
	i++;

	configdata[i].name = pstrdup("PGXS");
	get_pkglib_path(my_exec_path, path);
	strlcat(path, "/pgxs/src/makefiles/pgxs.mk", sizeof(path));
	cleanup_path(path);
	configdata[i].setting = pstrdup(path);
	i++;

	configdata[i].name = pstrdup("CONFIGURE");
	configdata[i].setting = pstrdup(CONFIGURE_ARGS);
	i++;

	configdata[i].name = pstrdup("CC");
#ifdef VAL_CC
	configdata[i].setting = pstrdup(VAL_CC);
#else
	configdata[i].setting = pstrdup(_("not recorded"));
#endif
	i++;

	configdata[i].name = pstrdup("CPPFLAGS");
#ifdef VAL_CPPFLAGS
	configdata[i].setting = pstrdup(VAL_CPPFLAGS);
#else
	configdata[i].setting = pstrdup(_("not recorded"));
#endif
	i++;

	configdata[i].name = pstrdup("CFLAGS");
#ifdef VAL_CFLAGS
	configdata[i].setting = pstrdup(VAL_CFLAGS);
#else
	configdata[i].setting = pstrdup(_("not recorded"));
#endif
	i++;

	configdata[i].name = pstrdup("CFLAGS_SL");
#ifdef VAL_CFLAGS_SL
	configdata[i].setting = pstrdup(VAL_CFLAGS_SL);
#else
	configdata[i].setting = pstrdup(_("not recorded"));
#endif
	i++;

	configdata[i].name = pstrdup("LDFLAGS");
#ifdef VAL_LDFLAGS
	configdata[i].setting = pstrdup(VAL_LDFLAGS);
#else
	configdata[i].setting = pstrdup(_("not recorded"));
#endif
	i++;

	configdata[i].name = pstrdup("LDFLAGS_EX");
#ifdef VAL_LDFLAGS_EX
	configdata[i].setting = pstrdup(VAL_LDFLAGS_EX);
#else
	configdata[i].setting = pstrdup(_("not recorded"));
#endif
	i++;

	configdata[i].name = pstrdup("LDFLAGS_SL");
#ifdef VAL_LDFLAGS_SL
	configdata[i].setting = pstrdup(VAL_LDFLAGS_SL);
#else
	configdata[i].setting = pstrdup(_("not recorded"));
#endif
	i++;

	configdata[i].name = pstrdup("LIBS");
#ifdef VAL_LIBS
	configdata[i].setting = pstrdup(VAL_LIBS);
#else
	configdata[i].setting = pstrdup(_("not recorded"));
#endif
	i++;

	configdata[i].name = pstrdup("VERSION");
	configdata[i].setting = pstrdup("PostgreSQL " PG_VERSION);
	i++;

	Assert(i == *configdata_len);

	return configdata;
}
