/*
 * stopword library
 * Teodor Sigaev <teodor@sigaev.ru>
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "postgres.h"
#include "common.h"
#include "dict.h"

#define STOPBUFLEN	4096

char *
lowerstr(char *str)
{
	char	   *ptr = str;

	while (*ptr)
	{
		*ptr = tolower(*(unsigned char *) ptr);
		ptr++;
	}
	return str;
}

void
freestoplist(StopList * s)
{
	char	  **ptr = s->stop;

	if (ptr)
		while (*ptr && s->len > 0)
		{
			free(*ptr);
			ptr++;
			s->len--;
			free(s->stop);
		}
	memset(s, 0, sizeof(StopList));
}

void
readstoplist(text *in, StopList * s)
{
	char	  **stop = NULL;

	s->len = 0;
	if (in && VARSIZE(in) - VARHDRSZ > 0)
	{
		char	   *filename = text2char(in);
		FILE	   *hin = NULL;
		char		buf[STOPBUFLEN];
		int			reallen = 0;

		if ((hin = fopen(filename, "r")) == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("could not open file \"%s\": %m",
							filename)));

		while (fgets(buf, STOPBUFLEN, hin))
		{
			buf[strlen(buf) - 1] = '\0';
			if (*buf == '\0')
				continue;

			if (s->len >= reallen)
			{
				char	  **tmp;

				reallen = (reallen) ? reallen * 2 : 16;
				tmp = (char **) realloc((void *) stop, sizeof(char *) * reallen);
				if (!tmp)
				{
					freestoplist(s);
					fclose(hin);
					ereport(ERROR,
							(errcode(ERRCODE_OUT_OF_MEMORY),
							 errmsg("out of memory")));
				}
				stop = tmp;
			}

			stop[s->len] = strdup(buf);
			if (!stop[s->len])
			{
				freestoplist(s);
				fclose(hin);
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory")));
			}
			if (s->wordop)
				stop[s->len] = (s->wordop) (stop[s->len]);

			(s->len)++;
		}
		fclose(hin);
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
	if (s->wordop)
		key = (*(s->wordop)) (key);
	return (s->stop && s->len > 0 && bsearch(&key, s->stop, s->len, sizeof(char *), comparestr)) ? true : false;
}
