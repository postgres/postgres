/*-------------------------------------------------------------------------
 *
 * scansup.c
 *	  support routines for the lex/flex scanner, used by both the normal
 * backend as well as the bootstrap backend
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/scansup.c,v 1.12 1999/02/13 23:17:12 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "config.h"

#include <ctype.h>
#include <string.h>

#include "postgres.h"
#include "miscadmin.h"
#include "parser/scansup.h"
#include "utils/elog.h"

/* ----------------
 *		scanstr
 *
 * if the string passed in has escaped codes, map the escape codes to actual
 * chars
 *
 * the string returned is a pointer to static storage and should NOT
 * be freed by the caller.
 * ----------------
 */

char *
scanstr(char *s)
{
	static char newStr[MAX_PARSE_BUFFER];
	int			len,
				i,
				j;

	if (s == NULL || s[0] == '\0')
		return s;

	len = strlen(s);

	for (i = 0, j = 0; i < len; i++)
	{
		if (s[i] == '\'')
		{
			/* Note: if scanner is working right, unescaped quotes can only
			 * appear in pairs, so there should be another character.
			 */
			i++;
			newStr[j] = s[i];
		}
		else if (s[i] == '\\')
		{
			i++;
			switch (s[i])
			{
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
				case '0':
				case '1':
				case '2':
				case '3':
				case '4':
				case '5':
				case '6':
				case '7':
					{
						int			k;
						long		octVal = 0;

						for (k = 0;
							 s[i + k] >= '0' && s[i + k] <= '7' && k < 3;
							 k++)
							octVal = (octVal << 3) + (s[i + k] - '0');
						i += k - 1;
						newStr[j] = ((char) octVal);
					}
					break;
				default:
					newStr[j] = s[i];
					break;
			}					/* switch */
		}						/* s[i] == '\\' */
		else
			newStr[j] = s[i];
		j++;
	}
	newStr[j] = '\0';
	return newStr;
}
