/*
 * Headers for handling of 'SET var TO', 'SHOW var' and 'RESET var' 
 * statements
 *
 * $Id: variable.h,v 1.4 1997/04/23 05:52:32 vadim Exp $
 *
 */

enum DateFormat { Date_Postgres, Date_SQL, Date_ISO };

/*-----------------------------------------------------------------------*/
struct PGVariables
	{
	struct
		{
		bool euro;
		enum DateFormat format;
		} date;
	};

extern struct PGVariables PGVariables;

/*-----------------------------------------------------------------------*/
bool SetPGVariable(const char *, const char *);
bool GetPGVariable(const char *);
bool ResetPGVariable(const char *);
