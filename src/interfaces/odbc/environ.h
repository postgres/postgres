/* File:			environ.h
 *
 * Description:		See "environ.c"
 *
 * Comments:		See "notice.txt" for copyright and license information.
 *
 */

#ifndef __ENVIRON_H__
#define __ENVIRON_H__

#include "psqlodbc.h"

#define ENV_ALLOC_ERROR 1

/**********		Environment Handle	*************/
struct EnvironmentClass_
{
	char	   *errormsg;
	int		errornumber;
	Int4	flag;
};

/*	Environment prototypes */
EnvironmentClass *EN_Constructor(void);
char		EN_Destructor(EnvironmentClass *self);
char		EN_get_error(EnvironmentClass *self, int *number, char **message);
char		EN_add_connection(EnvironmentClass *self, ConnectionClass *conn);
char		EN_remove_connection(EnvironmentClass *self, ConnectionClass *conn);
void		EN_log_error(char *func, char *desc, EnvironmentClass *self);

#define	EN_OV_ODBC2	1L
#define	EN_is_odbc2(env) ((env->flag & EN_OV_ODBC2) != 0)
#define	EN_is_odbc3(env) ((env->flag & EN_OV_ODBC2) == 0)
#define EN_set_odbc2(env) (env->flag |= EN_OV_ODBC2)
#define EN_set_odbc3(env) (env->flag &= EN_OV_ODBC2)

#endif
