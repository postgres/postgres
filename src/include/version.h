/*-------------------------------------------------------------------------
 *
 * version.h--
 *    this file contains the interface to version.c.
 *    Also some parameters.
 *
 * $Id: version.h,v 1.2 1997/04/26 05:07:12 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef VERSION_H
#define VERSION_H

void
ValidatePgVersion(const char *path, char **reason_p);

void
SetPgVersion(const char *path, char **reason_p);

#define	PG_RELEASE	6
#define PG_VERSION	1
#define	PG_VERFILE	"PG_VERSION"

#endif
