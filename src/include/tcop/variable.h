/*
 * Headers for handling of SET var TO statements
 *
 * $Id: variable.h,v 1.2 1997/04/17 13:50:57 scrappy Exp $
 *
 * $Log: variable.h,v $
 * Revision 1.2  1997/04/17 13:50:57  scrappy
 * From: "Martin J. Laubach" <mjl@CSlab.tuwien.ac.at>
 * Subject: [HACKERS] Patch: set date to euro/us postgres/iso/sql
 *
 *   Here a patch that implements a SET date for use by the datetime
 * stuff. The syntax is
 *
 *         SET date TO 'val[,val,...]'
 *
 *   where val is us (us dates), euro (european dates), postgres,
 * iso or sql.
 *
 *   Thomas is working on the integration in his datetime module.
 * I just needed to get the patch out before it went stale :)
 *
 * Revision 1.1  1997/04/10 16:53:30  mjl
 * Initial revision
 *
 */
/*-----------------------------------------------------------------------*/

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
const char *GetPGVariable(const char *);

/*-----------------------------------------------------------------------*/
bool SetPGVariable(const char *, const char *);
const char *GetPGVariable(const char *);
