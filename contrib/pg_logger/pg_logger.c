/* pg_logger: stdin-to-syslog gateway for postgresql.
 *
 * Copyright 2001 by Nathan Myers <ncm@nospam.cantrip.org>
 * This software is distributed free of charge with no warranty of any kind.
 * You have permission to make copies for any purpose, provided that (1)
 * this copyright notice is retained unchanged, and (2) you agree to
 * absolve the author of all responsibility for all consequences arising
 * from any use.
 */

#include <stdio.h>
#include <stddef.h>
#include <syslog.h>
#include <string.h>

struct
{
	const char *tag;
	int			size;
	int			priority;
}			tags[] =

{
	{
		"", 0, LOG_NOTICE
	},
	{
		"emerg:", sizeof("emerg"), LOG_EMERG
	},
	{
		"alert:", sizeof("alert"), LOG_ALERT
	},
	{
		"crit:", sizeof("crit"), LOG_CRIT
	},
	{
		"err:", sizeof("err"), LOG_ERR
	},
	{
		"error:", sizeof("error"), LOG_ERR
	},
	{
		"warning:", sizeof("warning"), LOG_WARNING
	},
	{
		"notice:", sizeof("notice"), LOG_NOTICE
	},
	{
		"info:", sizeof("info"), LOG_INFO
	},
	{
		"debug:", sizeof("debug"), LOG_DEBUG
	}
};

int
main()
{
	char		buf[301];
	int			c;
	char	   *pos = buf;
	const char *colon = 0;

#ifndef DEBUG
	openlog("postgresql", LOG_CONS, LOG_LOCAL1);
#endif
	while ((c = getchar()) != EOF)
	{
		if (c == '\r')
			continue;
		if (c == '\n')
		{
			int			level = sizeof(tags) / sizeof(*tags);
			char	   *bol;

			if (colon == 0 || (size_t) (colon - buf) > sizeof("warning"))
				level = 1;
			*pos = 0;
			while (--level)
			{
				if (pos - buf >= tags[level].size
				 && strncmp(buf, tags[level].tag, tags[level].size) == 0)
					break;
			}
			bol = buf + tags[level].size;
			if (bol > buf && *bol == ' ')
				++bol;
			if (pos - bol > 0)
			{
#ifndef DEBUG
				syslog(tags[level].priority, "%s", bol);
#else
				printf("%d/%s\n", tags[level].priority, bol);
#endif
			}
			pos = buf;
			colon = (char const *) 0;
			continue;
		}
		if (c == ':' && !colon)
			colon = pos;
		if ((size_t) (pos - buf) < sizeof(buf) - 1)
			*pos++ = c;
	}
	return 0;
}
