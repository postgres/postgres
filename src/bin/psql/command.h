#ifndef COMMAND_H
#define COMMAND_H

#include <config.h>
#include <c.h>

#include <pqexpbuffer.h>

#include "settings.h"
#include "print.h"



typedef enum _backslashResult
{
	CMD_UNKNOWN = 0,			/* not done parsing yet (internal only) */
	CMD_SEND,					/* query complete; send off */
	CMD_SKIP_LINE,				/* keep building query */
	CMD_TERMINATE,				/* quit program */
	CMD_NEWEDIT,				/* query buffer was changed (e.g., via \e) */
	CMD_ERROR					/* the execution of the backslash command
								 * resulted in an error */
}			backslashResult;



backslashResult
HandleSlashCmds(const char *line,
				PQExpBuffer query_buf,
				const char **end_of_cmd, int encoding);

bool
process_file(const char *filename, int encoding);

bool
do_pset(const char *param,
		const char *value,
		printQueryOpt * popt,
		bool quiet);

#endif
