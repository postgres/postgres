/*-------------------------------------------------------------------------
 *
 * scansup.c--
 *    support routines for the lex/flex scanner, used by both the normal
 * backend as well as the bootstrap backend
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/parser/scansup.c,v 1.4 1996/11/04 04:04:58 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "config.h"

#include <ctype.h>
#include <string.h>
#include "c.h"
#include "postgres.h"
#include "miscadmin.h"
#include "utils/elog.h"
#include "parser/scansup.h"

/*
 *	Scanner error handler.
 */
static void
serror(char *str)
{
    elog(WARN, "*** scanner error: %s\n", str);
}

/* ----------------
 *	scanstr
 * 
 * if the string passed in has escaped codes, map the escape codes to actual
 * chars
 *
 * also, remove leading and ending quotes '"' if any 
 *
 * the string passed in must be non-null
 *
 * the string returned is a pointer to static storage and should NOT
 * be freed by the CALLER.
 * ----------------
 */

char* 
scanstr(char *s)
{
    static char newStr[MAX_PARSE_BUFFER]; 
    int len, i, start, j;
    char delimiter;
    
    if (s == NULL || s[0] == '\0')
	return s;

    len = strlen(s);
    start = 0;

    /* remove leading and trailing quotes, if any */
    /* the normal backend lexer only accepts single quotes, but the
       bootstrap lexer accepts double quotes */
    delimiter = 0;
    if (s[0] == '"' || s[0] == '\''){
	delimiter = s[0];
	start = 1;
    }
    if (delimiter != 0)  {
	if (s[len-1] == delimiter) 
	    len = len - 1;
	else
	    serror("mismatched quote delimiters");
    }

    for (i = start, j = 0; i < len ; i++) {
	if (s[i] == '\'') {
	    i = i + 1;
	    if (s[i] == '\'')
		newStr[j] = '\'';
	}
	else {
	    if (s[i] == '\\') {
		i = i + 1;
		switch (s[i]) {
		case '\\':
		    newStr[j] = '\\';
		    break;
		case 'b':
		    newStr[j] = '\b';
		    break;
		case 'f':
		    newStr[j] = '\f';
		    break;
		case 'n':
		    newStr[j] = '\n';
		    break;
		case 'r':
		    newStr[j] = '\r';
		    break;
		case 't':
		    newStr[j] = '\t';
		    break;
		case '"':
		    newStr[j] = '"';
		    break;
		case '\'':
		    newStr[j] = '\'';
		    break;
		case '0': 
		case '1': 
		case '2': 
		case '3': 
		case '4': 
		case '5':
		case '6': 
		case '7':
		    {
			char octal[4];
			int k;
			long octVal;

			for (k=0; 
			     s[i+k] >= '0' && s[i+k] <= '7' && k < 3;
			     k++)
			    octal[k] = s[i+k];
			i += k-1;
			octal[3] = '\0';
		    
			octVal = strtol(octal,0,8);
/*			elog (NOTICE, "octal = %s octVal = %d, %od", octal, octVal, octVal);*/
			if (octVal <= 0377) {
			    newStr[j] = ((char)octVal);
			    break;
			}
		    }
		default:
                    newStr[j] = s[i];
		} /* switch */
	    } /* s[i] == '\\' */
	    else
		newStr[j] = s[i];
	}
	j++;
    }
    newStr[j] = '\0';
    return newStr;
}

