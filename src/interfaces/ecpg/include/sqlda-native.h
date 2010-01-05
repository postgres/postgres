/*
 * $PostgreSQL: pgsql/src/interfaces/ecpg/include/sqlda-native.h,v 1.1 2010/01/05 16:38:23 meskes Exp $
 */

#ifndef ECPG_SQLDA_NATIVE_H
#define ECPG_SQLDA_NATIVE_H

#include "postgres_fe.h"

struct sqlname
{
	short		length;
	char		data[NAMEDATALEN];
};

struct sqlvar_struct
{
	short		sqltype;
	short		sqllen;
	char	   *sqldata;
	short	   *sqlind;
	struct sqlname sqlname;
};

struct sqlda_struct
{
	char		sqldaid[8];
	long		sqldabc;
	short		sqln;
	short		sqld;
	struct sqlda_struct *desc_next;
	struct sqlvar_struct	sqlvar[1];
};

#endif /* ECPG_SQLDA_NATIVE_H */
