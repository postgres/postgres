/*-------------------------------------------------------------------------
 *
 * pg_tde_defs.c
 *      The configure script generates config.h which contains the package_* defs
 *      and these defines conflicts with the PG defines.
 *      This file is used to provide the package version string to the extension
 *      without including the config.h file.
 *
 * IDENTIFICATION
 *    contrib/pg_tde/src/pg_tde_defs.c
 *
 *-------------------------------------------------------------------------
 */


#include "config.h"
#include "pg_tde_defs.h"


/* Returns package version */
const char*
pg_tde_package_string(void)
{
	return PACKAGE_STRING;
}

const char *
pg_tde_package_name(void)
{
	return PACKAGE_NAME;
}
const char*
pg_tde_package_version(void)
{
	return PACKAGE_VERSION;
}
