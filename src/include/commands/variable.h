/*
 * Headers for handling of 'SET var TO', 'SHOW var' and 'RESET var'
 * statements
 *
 * $Id: variable.h,v 1.8 1998/10/08 18:30:27 momjian Exp $
 *
 */
#ifndef VARIABLE_H
#define VARIABLE_H 1

enum DateFormat
{
	Date_Postgres, Date_SQL, Date_ISO
};

/*-----------------------------------------------------------------------*/
struct PGVariables
{
	struct
	{
		bool		euro;
		enum DateFormat format;
	}			date;
};

extern struct PGVariables PGVariables;

/*-----------------------------------------------------------------------*/
bool		SetPGVariable(const char *, const char *);
bool		GetPGVariable(const char *);
bool		ResetPGVariable(const char *);

#endif	 /* VARIABLE_H */
