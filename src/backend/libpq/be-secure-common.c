/*-------------------------------------------------------------------------
 *
 * be-secure-common.c
 *
 * common implementation-independent SSL support code
 *
 * While be-secure.c contains the interfaces that the rest of the
 * communications code calls, this file contains support routines that are
 * used by the library-specific implementations such as be-secure-openssl.c.
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/libpq/be-secure-common.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "libpq/libpq.h"
#include "storage/fd.h"

/*
 * Run ssl_passphrase_command
 *
 * prompt will be substituted for %p.  is_server_start determines the loglevel
 * of error messages.
 *
 * The result will be put in buffer buf, which is of size size.  The return
 * value is the length of the actual result.
 */
int
run_ssl_passphrase_command(const char *prompt, bool is_server_start, char *buf, int size)
{
	int			loglevel = is_server_start ? ERROR : LOG;
	StringInfoData command;
	char	   *p;
	FILE	   *fh;
	int			pclose_rc;
	size_t		len = 0;

	Assert(prompt);
	Assert(size > 0);
	buf[0] = '\0';

	initStringInfo(&command);

	for (p = ssl_passphrase_command; *p; p++)
	{
		if (p[0] == '%')
		{
			switch (p[1])
			{
				case 'p':
					appendStringInfoString(&command, prompt);
					p++;
					break;
				case '%':
					appendStringInfoChar(&command, '%');
					p++;
					break;
				default:
					appendStringInfoChar(&command, p[0]);
			}
		}
		else
			appendStringInfoChar(&command, p[0]);
	}

	fh = OpenPipeStream(command.data, "r");
	if (fh == NULL)
	{
		ereport(loglevel,
				(errcode_for_file_access(),
				 errmsg("could not execute command \"%s\": %m",
						command.data)));
		goto error;
	}

	if (!fgets(buf, size, fh))
	{
		if (ferror(fh))
		{
			ereport(loglevel,
					(errcode_for_file_access(),
					 errmsg("could not read from command \"%s\": %m",
							command.data)));
			goto error;
		}
	}

	pclose_rc = ClosePipeStream(fh);
	if (pclose_rc == -1)
	{
		ereport(loglevel,
				(errcode_for_file_access(),
				 errmsg("could not close pipe to external command: %m")));
		goto error;
	}
	else if (pclose_rc != 0)
	{
		ereport(loglevel,
				(errcode_for_file_access(),
				 errmsg("command \"%s\" failed",
						command.data),
				 errdetail_internal("%s", wait_result_to_str(pclose_rc))));
		goto error;
	}

	/* strip trailing newline */
	len = strlen(buf);
	if (buf[len - 1] == '\n')
		buf[len-- -1] = '\0';

error:
	pfree(command.data);
	return len;
}
