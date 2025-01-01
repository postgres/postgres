/*-------------------------------------------------------------------------
 *
 * percentrepl.c
 *	  Common routines to replace percent placeholders in strings
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/common/percentrepl.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#include "common/logging.h"
#endif

#include "common/percentrepl.h"
#include "lib/stringinfo.h"

/*
 * replace_percent_placeholders
 *
 * Replace percent-letter placeholders in input string with the supplied
 * values.  For example, to replace %f with foo and %b with bar, call
 *
 * replace_percent_placeholders(instr, "param_name", "bf", bar, foo);
 *
 * The return value is palloc'd.
 *
 * "%%" is replaced by a single "%".
 *
 * This throws an error for an unsupported placeholder or a "%" at the end of
 * the input string.
 *
 * A value may be NULL.  If the corresponding placeholder is found in the
 * input string, it will be treated as if an unsupported placeholder was used.
 * This allows callers to share a "letters" specification but vary the
 * actually supported placeholders at run time.
 *
 * This functions is meant for cases where all the values are readily
 * available or cheap to compute and most invocations will use most values
 * (for example for archive_command).  Also, it requires that all values are
 * strings.  It won't be a good match for things like log prefixes or prompts
 * that use a mix of data types and any invocation will only use a few of the
 * possible values.
 *
 * param_name is the name of the underlying GUC parameter, for error
 * reporting.  At the moment, this function is only used for GUC parameters.
 * If other kinds of uses were added, the error reporting would need to be
 * revised.
 */
char *
replace_percent_placeholders(const char *instr, const char *param_name, const char *letters,...)
{
	StringInfoData result;

	initStringInfo(&result);

	for (const char *sp = instr; *sp; sp++)
	{
		if (*sp == '%')
		{
			if (sp[1] == '%')
			{
				/* Convert %% to a single % */
				sp++;
				appendStringInfoChar(&result, *sp);
			}
			else if (sp[1] == '\0')
			{
				/* Incomplete escape sequence, expected a character afterward */
#ifdef FRONTEND
				pg_log_error("invalid value for parameter \"%s\": \"%s\"", param_name, instr);
				pg_log_error_detail("String ends unexpectedly after escape character \"%%\".");
				exit(1);
#else
				ereport(ERROR,
						errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("invalid value for parameter \"%s\": \"%s\"", param_name, instr),
						errdetail("String ends unexpectedly after escape character \"%%\"."));
#endif
			}
			else
			{
				/* Look up placeholder character */
				bool		found = false;
				va_list		ap;

				sp++;

				va_start(ap, letters);
				for (const char *lp = letters; *lp; lp++)
				{
					char	   *val = va_arg(ap, char *);

					if (*sp == *lp)
					{
						if (val)
						{
							appendStringInfoString(&result, val);
							found = true;
						}
						/* If val is NULL, we will report an error. */
						break;
					}
				}
				va_end(ap);
				if (!found)
				{
					/* Unknown placeholder */
#ifdef FRONTEND
					pg_log_error("invalid value for parameter \"%s\": \"%s\"", param_name, instr);
					pg_log_error_detail("String contains unexpected placeholder \"%%%c\".", *sp);
					exit(1);
#else
					ereport(ERROR,
							errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("invalid value for parameter \"%s\": \"%s\"", param_name, instr),
							errdetail("String contains unexpected placeholder \"%%%c\".", *sp));
#endif
				}
			}
		}
		else
		{
			appendStringInfoChar(&result, *sp);
		}
	}

	return result.data;
}
