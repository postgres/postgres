/* File:			environ.h
 *
 * Description:		See "environ.c"
 *
 * Comments:		See "notice.txt" for copyright and license information.
 *
 */

#ifndef __ENVIRON_H__
#define __ENVIRON_H__

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "psqlodbc.h"

#ifndef WIN32
#include "iodbc.h"
#include "isql.h"
#include "isqlext.h"
#else
#include <windows.h>
#include <sql.h>
#include <sqlext.h>
#endif

#define ENV_ALLOC_ERROR 1

/**********		Environment Handle	*************/
struct EnvironmentClass_
{
	char	   *errormsg;
	int			errornumber;
};

/*	Environment prototypes */
EnvironmentClass *EN_Constructor(void);
char		EN_Destructor(EnvironmentClass *self);
char		EN_get_error(EnvironmentClass *self, int *number, char **message);
char		EN_add_connection(EnvironmentClass *self, ConnectionClass *conn);
char		EN_remove_connection(EnvironmentClass *self, ConnectionClass *conn);
void		EN_log_error(char *func, char *desc, EnvironmentClass *self);

#endif
