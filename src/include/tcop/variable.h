/*
 * Headers for handling of SET var TO statements
 *
 * $Id: variable.h,v 1.3 1997/04/23 03:17:52 scrappy Exp $
 *
 * $Log: variable.h,v $
 * Revision 1.3  1997/04/23 03:17:52  scrappy
 * To: Thomas Lockhart <Thomas.G.Lockhart@jpl.nasa.gov>
 * Subject: Re: [PATCHES] SET DateStyle patches
 *
 * On Tue, 22 Apr 1997, Thomas Lockhart wrote:
 *
 * > Some more patches! These (try to) finish implementing SET variable TO value
 * > for "DateStyle" (changed the name from simply "date" to be more descriptive).
 * > This is based on code from Martin and Bruce (?), which was easy to modify.
 * > The syntax is
 * >
 * > SET DateStyle TO 'iso'
 * > SET DateStyle TO 'postgres'
 * > SET DateStyle TO 'sql'
 * > SET DateStyle TO 'european'
 * > SET DateStyle TO 'noneuropean'
 * > SET DateStyle TO 'us'         (same as "noneuropean")
 * > SET DateStyle TO 'default'    (current same as "postgres,us")
 * >
 * > ("european" is just compared for the first 4 characters, and "noneuropean"
 * > is compared for the first 7 to allow less typing).
 * >
 * > Multiple arguments are allowed, so SET datestyle TO 'sql,euro' is valid.
 * >
 * > My mods also try to implement "SHOW variable" and "RESET variable", but
 * > that part just core dumps at the moment. I would guess that my errors
 * > are obvious to someone who knows what they are doing with the parser stuff,
 * > so if someone (Bruce and/or Martin??) could have it do the right thing
 * > we will have a more complete set of what we need.
 * >
 * > Also, I would like to have a floating point precision global variable to
 * > implement "SET precision TO 10" and perhaps "SET precision TO 10,2" for
 * > float8 and float4, but I don't know how to do that for integer types rather
 * > than strings. If someone is fixing the SHOW and RESET code, perhaps they can
 * > add some hooks for me to do the floats while they are at it.
 * >
 * > I've left some remnants of variable structures in the source code which
 * > I did not use in the interests of getting something working for v6.1.
 * > We'll have time to clean things up for the next release...
 *
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
bool GetPGVariable(const char *);
bool ResetPGVariable(const char *);
