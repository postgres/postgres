/*-------------------------------------------------------------------------
 *
 * selfuncs.h
 *	  Selectivity functions and index cost estimation functions for
 *	  standard operators and index access methods.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: selfuncs.h,v 1.1 2001/06/25 21:11:45 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SELFUNCS_H
#define SELFUNCS_H

#include "fmgr.h"
#include "nodes/parsenodes.h"


typedef enum
{
	Pattern_Type_Like, Pattern_Type_Like_IC,
	Pattern_Type_Regex, Pattern_Type_Regex_IC
} Pattern_Type;

typedef enum
{
	Pattern_Prefix_None, Pattern_Prefix_Partial, Pattern_Prefix_Exact
} Pattern_Prefix_Status;


/* selfuncs.c */

extern Pattern_Prefix_Status pattern_fixed_prefix(char *patt,
					 Pattern_Type ptype,
					 char **prefix,
					 char **rest);
extern bool locale_is_like_safe(void);
extern char *make_greater_string(const char *str, Oid datatype);

extern Datum eqsel(PG_FUNCTION_ARGS);
extern Datum neqsel(PG_FUNCTION_ARGS);
extern Datum scalarltsel(PG_FUNCTION_ARGS);
extern Datum scalargtsel(PG_FUNCTION_ARGS);
extern Datum regexeqsel(PG_FUNCTION_ARGS);
extern Datum icregexeqsel(PG_FUNCTION_ARGS);
extern Datum likesel(PG_FUNCTION_ARGS);
extern Datum iclikesel(PG_FUNCTION_ARGS);
extern Datum regexnesel(PG_FUNCTION_ARGS);
extern Datum icregexnesel(PG_FUNCTION_ARGS);
extern Datum nlikesel(PG_FUNCTION_ARGS);
extern Datum icnlikesel(PG_FUNCTION_ARGS);

extern Datum eqjoinsel(PG_FUNCTION_ARGS);
extern Datum neqjoinsel(PG_FUNCTION_ARGS);
extern Datum scalarltjoinsel(PG_FUNCTION_ARGS);
extern Datum scalargtjoinsel(PG_FUNCTION_ARGS);
extern Datum regexeqjoinsel(PG_FUNCTION_ARGS);
extern Datum icregexeqjoinsel(PG_FUNCTION_ARGS);
extern Datum likejoinsel(PG_FUNCTION_ARGS);
extern Datum iclikejoinsel(PG_FUNCTION_ARGS);
extern Datum regexnejoinsel(PG_FUNCTION_ARGS);
extern Datum icregexnejoinsel(PG_FUNCTION_ARGS);
extern Datum nlikejoinsel(PG_FUNCTION_ARGS);
extern Datum icnlikejoinsel(PG_FUNCTION_ARGS);

Selectivity booltestsel(Query *root, BooleanTest *clause, int varRelid);
Selectivity nulltestsel(Query *root, NullTest *clause, int varRelid);

extern Datum btcostestimate(PG_FUNCTION_ARGS);
extern Datum rtcostestimate(PG_FUNCTION_ARGS);
extern Datum hashcostestimate(PG_FUNCTION_ARGS);
extern Datum gistcostestimate(PG_FUNCTION_ARGS);

#endif	 /* SELFUNCS_H */
