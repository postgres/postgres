/*-------------------------------------------------------------------------
 *
 * config_info.c
 *		Common code for pg_config output
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
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

#include "miscadmin.h"
#include "common/config_info.h"

static size_t configdata_names_len(void);

static const char *const configdata_names[] =
{
	"BINDIR",
	"DOCDIR",
	"HTMLDIR",
	"INCLUDEDIR",
	"PKGINCLUDEDIR",
	"INCLUDEDIR-SERVER",
	"LIBDIR",
	"PKGLIBDIR",
	"LOCALEDIR",
	"MANDIR",
	"SHAREDIR",
	"SYSCONFDIR",
	"PGXS",
	"CONFIGURE",
	"CC",
	"CPPFLAGS",
	"CFLAGS",
	"CFLAGS_SL",
	"LDFLAGS",
	"LDFLAGS_EX",
	"LDFLAGS_SL",
	"LIBS",
	"VERSION",
	NULL
};

static size_t
configdata_names_len(void)
{
	size_t	i = 0;

	while (configdata_names[i])
		i++;

	return i;
}

/*
 * get_configdata(char *my_exec_path, size_t *configdata_len)
 *
 * Get configure-time constants. The caller is responsible
 * for pfreeing the result.
 */
ConfigData *
get_configdata(char *my_exec_path, size_t *configdata_len)
{
	ConfigData	   *configdata;
	char			path[MAXPGPATH];
	char		   *lastsep;
	int				i;

	*configdata_len = configdata_names_len();
	configdata = palloc(*configdata_len * sizeof(ConfigData));

	/*
	 * initialize configdata names
	 *
	 * These better be in sync with the settings manually
	 * defined below.
	 */
	for (i = 0; i < *configdata_len; i++)
		configdata[i].name = pstrdup(configdata_names[i]);

	strcpy(path, my_exec_path);
	lastsep = strrchr(path, '/');
	if (lastsep)
		*lastsep = '\0';
	cleanup_path(path);
	configdata[0].setting = pstrdup(path);

	get_doc_path(my_exec_path, path);
	cleanup_path(path);
	configdata[1].setting = pstrdup(path);

	get_html_path(my_exec_path, path);
	cleanup_path(path);
	configdata[2].setting = pstrdup(path);

	get_include_path(my_exec_path, path);
	cleanup_path(path);
	configdata[3].setting = pstrdup(path);

	get_pkginclude_path(my_exec_path, path);
	cleanup_path(path);
	configdata[4].setting = pstrdup(path);

	get_includeserver_path(my_exec_path, path);
	cleanup_path(path);
	configdata[5].setting = pstrdup(path);

	get_lib_path(my_exec_path, path);
	cleanup_path(path);
	configdata[6].setting = pstrdup(path);

	get_pkglib_path(my_exec_path, path);
	cleanup_path(path);
	configdata[7].setting = pstrdup(path);

	get_locale_path(my_exec_path, path);
	cleanup_path(path);
	configdata[8].setting = pstrdup(path);

	get_man_path(my_exec_path, path);
	cleanup_path(path);
	configdata[9].setting = pstrdup(path);

	get_share_path(my_exec_path, path);
	cleanup_path(path);
	configdata[10].setting = pstrdup(path);

	get_etc_path(my_exec_path, path);
	cleanup_path(path);
	configdata[11].setting = pstrdup(path);

	get_pkglib_path(my_exec_path, path);
	strlcat(path, "/pgxs/src/makefiles/pgxs.mk", sizeof(path));
	cleanup_path(path);
	configdata[12].setting = pstrdup(path);

#ifdef VAL_CONFIGURE
	configdata[13].setting = pstrdup(VAL_CONFIGURE);
#else
	configdata[13].setting = pstrdup(_("not recorded"));
#endif

#ifdef VAL_CC
	configdata[14].setting = pstrdup(VAL_CC);
#else
	configdata[14].setting = pstrdup(_("not recorded"));
#endif

#ifdef VAL_CPPFLAGS
	configdata[15].setting = pstrdup(VAL_CPPFLAGS);
#else
	configdata[15].setting = pstrdup(_("not recorded"));
#endif

#ifdef VAL_CFLAGS
	configdata[16].setting = pstrdup(VAL_CFLAGS);
#else
	configdata[16].setting = pstrdup(_("not recorded"));
#endif

#ifdef VAL_CFLAGS_SL
	configdata[17].setting = pstrdup(VAL_CFLAGS_SL);
#else
	configdata[17].setting = pstrdup(_("not recorded"));
#endif

#ifdef VAL_LDFLAGS
	configdata[18].setting = pstrdup(VAL_LDFLAGS);
#else
	configdata[18].setting = pstrdup(_("not recorded"));
#endif

#ifdef VAL_LDFLAGS_EX
	configdata[19].setting = pstrdup(VAL_LDFLAGS_EX);
#else
	configdata[19].setting = pstrdup(_("not recorded"));
#endif

#ifdef VAL_LDFLAGS_SL
	configdata[20].setting = pstrdup(VAL_LDFLAGS_SL);
#else
	configdata[20].setting = pstrdup(_("not recorded"));
#endif

#ifdef VAL_LIBS
	configdata[21].setting = pstrdup(VAL_LIBS);
#else
	configdata[21].setting = pstrdup(_("not recorded"));
#endif

	configdata[22].setting = pstrdup("PostgreSQL " PG_VERSION);

	return configdata;
}
