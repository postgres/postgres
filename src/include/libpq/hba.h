/*-------------------------------------------------------------------------
 *
 * hba.h--
 *	  Interface to hba.c
 *
 *
 * $Id: hba.h,v 1.4 1997/09/07 04:58:19 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef HBA_H
#define HBA_H

#include <libpq/pqcomm.h>

#define CONF_FILE "pg_hba.conf"
 /* Name of the config file  */

#define MAP_FILE "pg_ident.conf"
 /* Name of the usermap file */

#define OLD_CONF_FILE "pg_hba"
 /* Name of the config file in prior releases of Postgres. */

#define MAX_LINES 255
 /* Maximum number of config lines that can apply to one database	 */

#define MAX_TOKEN 80
/* Maximum size of one token in the configuration file	*/

#define USERMAP_NAME_SIZE 16	/* Max size of a usermap name */

#define IDENT_PORT 113
 /* Standard TCP port number for Ident service.  Assigned by IANA */

#define IDENT_USERNAME_MAX 512
 /* Max size of username ident server can return */

enum Userauth
{
	Trust, Ident, Password
};

extern int
hba_recvauth(const Port * port, const char database[], const char user[],
			 const char DataDir[]);
void
find_hba_entry(const char DataDir[], const struct in_addr ip_addr,
			   const char database[],
			   bool * host_ok_p, enum Userauth * userauth_p,
			   char usermap_name[], bool find_password_entries);

#endif
