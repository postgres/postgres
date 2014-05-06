/*
 *	page.c
 *
 *	per-page conversion operations
 *
 *	Copyright (c) 2010-2014, PostgreSQL Global Development Group
 *	contrib/pg_upgrade/page.c
 */

#include "postgres_fe.h"

#include "pg_upgrade.h"

#include "storage/bufpage.h"


#ifdef PAGE_CONVERSION


static void getPageVersion(
			   uint16 *version, const char *pathName);
static pageCnvCtx *loadConverterPlugin(
					uint16 newPageVersion, uint16 oldPageVersion);


/*
 * setupPageConverter()
 *
 *	This function determines the PageLayoutVersion of the old cluster and
 *	the PageLayoutVersion of the new cluster.  If the versions differ, this
 *	function loads a converter plugin and returns a pointer to a pageCnvCtx
 *	object (in *result) that knows how to convert pages from the old format
 *	to the new format.  If the versions are identical, this function just
 *	returns a NULL pageCnvCtx pointer to indicate that page-by-page conversion
 *	is not required.
 */
pageCnvCtx *
setupPageConverter(void)
{
	uint16		oldPageVersion;
	uint16		newPageVersion;
	pageCnvCtx *converter;
	const char *msg;
	char		dstName[MAXPGPATH];
	char		srcName[MAXPGPATH];

	snprintf(dstName, sizeof(dstName), "%s/global/%u", new_cluster.pgdata,
			 new_cluster.pg_database_oid);
	snprintf(srcName, sizeof(srcName), "%s/global/%u", old_cluster.pgdata,
			 old_cluster.pg_database_oid);

	getPageVersion(&oldPageVersion, srcName);
	getPageVersion(&newPageVersion, dstName);

	/*
	 * If the old cluster and new cluster use the same page layouts, then we
	 * don't need a page converter.
	 */
	if (newPageVersion != oldPageVersion)
	{
		/*
		 * The clusters use differing page layouts, see if we can find a
		 * plugin that knows how to convert from the old page layout to the
		 * new page layout.
		 */

		if ((converter = loadConverterPlugin(newPageVersion, oldPageVersion)) == NULL)
			pg_fatal("could not find plugin to convert from old page layout to new page layout\n");

		return converter;
	}
	else
		return NULL;
}


/*
 * getPageVersion()
 *
 *	Retrieves the PageLayoutVersion for the given relation.
 *
 *	Returns NULL on success (and stores the PageLayoutVersion at *version),
 *	if an error occurs, this function returns an error message (in the form
 *	of a null-terminated string).
 */
static void
getPageVersion(uint16 *version, const char *pathName)
{
	int			relfd;
	PageHeaderData page;
	ssize_t		bytesRead;

	if ((relfd = open(pathName, O_RDONLY, 0)) < 0)
		pg_fatal("could not open relation %s\n", pathName);

	if ((bytesRead = read(relfd, &page, sizeof(page))) != sizeof(page))
		pg_fatal("could not read page header of %s\n", pathName);

	*version = PageGetPageLayoutVersion(&page);

	close(relfd);

	return;
}


/*
 * loadConverterPlugin()
 *
 *	This function loads a page-converter plugin library and grabs a
 *	pointer to each of the (interesting) functions provided by that
 *	plugin.  The name of the plugin library is derived from the given
 *	newPageVersion and oldPageVersion.  If a plugin is found, this
 *	function returns a pointer to a pageCnvCtx object (which will contain
 *	a collection of plugin function pointers). If the required plugin
 *	is not found, this function returns NULL.
 */
static pageCnvCtx *
loadConverterPlugin(uint16 newPageVersion, uint16 oldPageVersion)
{
	char		pluginName[MAXPGPATH];
	void	   *plugin;

	/*
	 * Try to find a plugin that can convert pages of oldPageVersion into
	 * pages of newPageVersion.  For example, if we oldPageVersion = 3 and
	 * newPageVersion is 4, we search for a plugin named:
	 * plugins/convertLayout_3_to_4.dll
	 */

	/*
	 * FIXME: we are searching for plugins relative to the current directory,
	 * we should really search relative to our own executable instead.
	 */
	snprintf(pluginName, sizeof(pluginName), "./plugins/convertLayout_%d_to_%d%s",
			 oldPageVersion, newPageVersion, DLSUFFIX);

	if ((plugin = pg_dlopen(pluginName)) == NULL)
		return NULL;
	else
	{
		pageCnvCtx *result = (pageCnvCtx *) pg_malloc(sizeof(*result));

		result->old.PageVersion = oldPageVersion;
		result->new.PageVersion = newPageVersion;

		result->startup = (pluginStartup) pg_dlsym(plugin, "init");
		result->convertFile = (pluginConvertFile) pg_dlsym(plugin, "convertFile");
		result->convertPage = (pluginConvertPage) pg_dlsym(plugin, "convertPage");
		result->shutdown = (pluginShutdown) pg_dlsym(plugin, "fini");
		result->pluginData = NULL;

		/*
		 * If the plugin has exported an initializer, go ahead and invoke it.
		 */
		if (result->startup)
			result->startup(MIGRATOR_API_VERSION, &result->pluginVersion,
						newPageVersion, oldPageVersion, &result->pluginData);

		return result;
	}
}

#endif
