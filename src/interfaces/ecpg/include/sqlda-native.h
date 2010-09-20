/*
 * src/interfaces/ecpg/include/sqlda-native.h
 */

#ifndef ECPG_SQLDA_NATIVE_H
#define ECPG_SQLDA_NATIVE_H

/*
 * Maximum length for identifiers (e.g. table names, column names,
 * function names).  Names actually are limited to one less byte than this,
 * because the length must include a trailing zero byte.
 *
 * This should be at least as much as NAMEDATALEN of the database the
 * applications run against.
 */
#define NAMEDATALEN 64

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
	struct sqlvar_struct sqlvar[1];
};

#endif   /* ECPG_SQLDA_NATIVE_H */
