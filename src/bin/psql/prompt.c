#include <config.h>
#include <c.h>
#include "prompt.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <libpq-fe.h>

#include "settings.h"
#include "common.h"

#ifdef WIN32
#define popen(x,y) _popen(x,y)
#define pclose(x) _pclose(x)
#endif



/*--------------------------
 * get_prompt
 *
 * Returns a statically allocated prompt made by interpolating certain
 * tcsh style escape sequences into pset->vars "prompt1|2|3".
 * (might not be completely multibyte safe)
 *
 * Defined interpolations are:
 * %M - database server hostname (or "." if not TCP/IP)
 * %m - like %M but hostname truncated after first dot
 * %> - database server port number (or "." if not TCP/IP)
 * %n - database user name
 * %/ - current database
 * %~ - like %/ but "~" when database name equals user name
 * %# - "#" if the username is postgres, ">" otherwise
 * %R - in prompt1 normally =, or ^ if single line mode,
 *          or a ! if session is not connected to a database;
 *      in prompt2 -, *, ', or ";
 *      in prompt3 nothing
 * %? - the error code of the last query (not yet implemented)
 * %% - a percent sign
 *
 * %[0-9]          - the character with the given decimal code
 * %0[0-7]         - the character with the given octal code
 * %0x[0-9A-Fa-f]  - the character with the given hexadecimal code
 *
 * %`command`      - The result of executing command in /bin/sh with trailing
 *                   newline stripped.
 * %$name$         - The value of the psql/environment/magic varible 'name'
 *                   (same rules as for, e.g., \echo $foo)
 * (those will not be rescanned for more escape sequences!)
 *
 *
 * If the application-wide prompts became NULL somehow, the returned string
 * will be empty (not NULL!). Do not free() the result of this function unless
 * you want trouble.
 *--------------------------
 */
const char *
get_prompt(PsqlSettings *pset, promptStatus_t status)
{
#define MAX_PROMPT_SIZE 256
    static char destination[MAX_PROMPT_SIZE+1];
    char buf[MAX_PROMPT_SIZE+1];
    bool esc = false;
    const char *p;
    const char * prompt_string;

    if (GetVariable(pset->vars, "quiet"))
	return "";

    if (status == PROMPT_READY)
	prompt_string = GetVariable(pset->vars, "prompt1");
    else if (status == PROMPT_CONTINUE || status == PROMPT_SINGLEQUOTE || status == PROMPT_DOUBLEQUOTE || status == PROMPT_COMMENT)
	prompt_string = GetVariable(pset->vars, "prompt2");
    else if (status == PROMPT_COPY)
	prompt_string = GetVariable(pset->vars, "prompt3");
    else
	prompt_string = "? ";


    destination[0] = '\0';

    for (p = prompt_string;
	 p && *p && strlen(destination)<MAX_PROMPT_SIZE;
	 p++)
    {
	MemSet(buf, 0, MAX_PROMPT_SIZE+1);
	if (esc)
	{
	    switch (*p)
	    {
	    case '%':
		strcpy(buf, "%");
		break;

		/* Current database */
	    case '/':
		if (pset->db)
		    strncpy(buf, PQdb(pset->db), MAX_PROMPT_SIZE);
		break;
	    case '~': {
		const char * var;
		if (pset->db) {
		    if ( strcmp(PQdb(pset->db), PQuser(pset->db))==0 ||
			( (var = getenv("PGDATABASE")) && strcmp(var, PQdb(pset->db))==0) )
			strcpy(buf, "~");
		    else
			strncpy(buf, PQdb(pset->db), MAX_PROMPT_SIZE);
		}
		break;
	    }
		/* DB server hostname (long/short) */
	    case 'M':
	    case 'm':
		if (pset->db) {
		    if (PQhost(pset->db)) {
			strncpy(buf, PQhost(pset->db), MAX_PROMPT_SIZE);
			if (*p == 'm')
			    buf[strcspn(buf,".")] = '\0';
		    }
		    else
			buf[0] = '.';
		}
		break;
		/* DB server port number */
	    case '>':
		if (pset->db) {
		    if (PQhost(pset->db))
			strncpy(buf, PQport(pset->db), MAX_PROMPT_SIZE);
		    else
			buf[0] = '.';
		}
		break;
		/* DB server user name */
	    case 'n':
		if (pset->db)
		    strncpy(buf, PQuser(pset->db), MAX_PROMPT_SIZE);
		break;

	    case '0': case '1': case '2': case '3': case '4':
	    case '5': case '6': case '7': case '8': case '9':
	    {
		long int l;
		char * end;
		l = strtol(p, &end, 0);
		sprintf(buf, "%c", (unsigned char)l);
		p = end-1;
		break;
	    }

	    case 'R':
		switch(status) {
		case PROMPT_READY:
		    if (!pset->db)
			buf[0] = '!';
		    else if (!GetVariableBool(pset->vars, "singleline"))
			buf[0] = '=';
		    else
			buf[0] = '^';
		    break;
		case PROMPT_CONTINUE:
		    buf[0] = '-';
		    break;
		case PROMPT_SINGLEQUOTE:
		    buf[0] = '\'';
		    break;
		case PROMPT_DOUBLEQUOTE:
		    buf[0] = '"';
		    break;
		case PROMPT_COMMENT:
		    buf[0] = '*';
		    break;
		default:
		    buf[0] = '\0';
		    break;
		}

	    case '?':
		/* not here yet */
		break;

	    case '#':
	    {
		if (pset->db && strcmp(PQuser(pset->db), "postgres")==0)
		    buf[0] = '#';
		else
		    buf[0] = '>';
		
		break;
	    }

	    /* execute command */
	    case '`':
	    {
		FILE * fd = NULL;
		char * file = strdup(p+1);
		int cmdend;
		cmdend = strcspn(file, "`");
		file[cmdend] = '\0';
		if (file)
		    fd = popen(file, "r");
		if (fd) {
		    fgets(buf, MAX_PROMPT_SIZE-1, fd);
		    pclose(fd);
		}
		if (buf[strlen(buf)-1] == '\n')
		    buf[strlen(buf)-1] = '\0';
		free(file);
		p += cmdend+1;
		break;
	    }

	    /* interpolate variable */
	    case '$':
	    {
		char *name;
		const char *val;
		int nameend;
		name = strdup(p+1);
		nameend = strcspn(name, "$");
		name[nameend] = '\0';
		val = interpolate_var(name, pset);
		if (val)
		    strncpy(buf, val, MAX_PROMPT_SIZE);
		free(name);
		p += nameend+1;
		break;
	    }
		

	    default:
		buf[0] = *p;
		buf[1] = '\0';

	    }
	    esc = false;
	}
	else if (*p == '%')
	    esc = true;
	else
	{
	    buf[0] = *p;
	    buf[1] = '\0';
	    esc = false;
	}

	if (!esc) {
	    strncat(destination, buf, MAX_PROMPT_SIZE-strlen(destination));
	}
    }

    destination[MAX_PROMPT_SIZE] = '\0';
    return destination;
}

