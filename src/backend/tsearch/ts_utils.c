/*-------------------------------------------------------------------------
 *
 * ts_utils.c
 *		various support functions
 *
 * Portions Copyright (c) 1996-2007, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/tsearch/ts_utils.c,v 1.2 2007/08/22 01:39:44 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>

#include "miscadmin.h"
#include "storage/fd.h"
#include "tsearch/ts_locale.h"
#include "tsearch/ts_public.h"
#include "tsearch/ts_utils.h"
#include "utils/builtins.h"


/*
 * Given the base name and extension of a tsearch config file, return
 * its full path name.  The base name is assumed to be user-supplied,
 * and is checked to prevent pathname attacks.  The extension is assumed
 * to be safe.
 *
 * The result is a palloc'd string.
 */
char *
get_tsearch_config_filename(const char *basename,
							const char *extension)
{
	char		sharepath[MAXPGPATH];
	char	   *result;
	const char *p;

	/*
	 * We enforce that the basename is all alpha characters.  This may be
	 * overly restrictive, but we don't want to allow access to anything
	 * outside the tsearch_data directory, so for instance '/' *must* be
	 * rejected.  This is the same test used for timezonesets names.
	 */
	for (p = basename; *p; p++)
	{
		if (!isalpha((unsigned char) *p))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid text search configuration file name \"%s\"",
							basename)));
	}

	get_share_path(my_exec_path, sharepath);
	result = palloc(MAXPGPATH);
	snprintf(result, MAXPGPATH, "%s/tsearch_data/%s.%s",
			 sharepath, basename, extension);

	return result;
}

#define STOPBUFLEN	4096

void
readstoplist(char *in, StopList * s)
{
	char	  **stop = NULL;

	s->len = 0;
	if (in && *in)
	{
		char	   *filename = get_tsearch_config_filename(in, "stop");
		FILE	   *hin;
		char		buf[STOPBUFLEN];
		int			reallen = 0;
		int			line = 0;

		if ((hin = AllocateFile(filename, "r")) == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("could not open stopword file \"%s\": %m",
							filename)));

		while (fgets(buf, STOPBUFLEN, hin))
		{
			char	   *pbuf = buf;

			line++;
			while (*pbuf && !isspace(*pbuf))
				pbuf++;
			*pbuf = '\0';

			if (*buf == '\0')
				continue;

			if (!pg_verifymbstr(buf, strlen(buf), true))
			{
				FreeFile(hin);
				ereport(ERROR,
						(errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
						 errmsg("invalid multibyte encoding at line %d in file \"%s\"",
								line, filename)));
			}

			if (s->len >= reallen)
			{
				if (reallen == 0)
				{
					reallen = 16;
					stop = (char **) palloc(sizeof(char *) * reallen);
				}
				else
				{
					reallen *= 2;
					stop = (char **) repalloc((void *) stop, sizeof(char *) * reallen);
				}
			}


			if (s->wordop)
				stop[s->len] = s->wordop(buf);
			else
				stop[s->len] = pstrdup(buf);

			(s->len)++;
		}
		FreeFile(hin);
		pfree(filename);
	}

	s->stop = stop;
}

static int
comparestr(const void *a, const void *b)
{
	return strcmp(*(char **) a, *(char **) b);
}

void
sortstoplist(StopList * s)
{
	if (s->stop && s->len > 0)
		qsort(s->stop, s->len, sizeof(char *), comparestr);
}

bool
searchstoplist(StopList * s, char *key)
{
	return (s->stop && s->len > 0 &&
			bsearch(&key, s->stop, s->len,
					sizeof(char *), comparestr)) ? true : false;
}

char *
pnstrdup(const char *in, int len)
{
	char	   *out = palloc(len + 1);

	memcpy(out, in, len);
	out[len] = '\0';
	return out;
}
