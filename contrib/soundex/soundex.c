/*****************************************************************************/
/* soundex.c */
/*****************************************************************************/

#include <ctype.h>
#include <string.h>
#include <stdio.h>

#include "postgres.h"			/* for char16, etc. */

#include "utils/palloc.h"		/* for palloc */

/* prototypes for soundex functions */
text	   *text_soundex(text *t);
char	   *soundex(char *instr, char *outstr);

text *
text_soundex(text *t)
{
	text	   *new_t;

	char		outstr[6 + 1];	/* max length of soundex is 6 */
	char	   *instr;

	/* make a null-terminated string */
	instr = palloc(VARSIZE(t) + 1);
	memcpy(instr, VARDATA(t), VARSIZE(t) - VARHDRSZ);
	instr[VARSIZE(t) - VARHDRSZ] = (char) 0;

	/* load soundex into outstr */
	soundex(instr, outstr);

	/* Now the outstr contains the soundex of instr */
	/* copy outstr to new_t */
	new_t = (text *) palloc(strlen(outstr) + VARHDRSZ);
	memset(new_t, 0, strlen(outstr) + 1);
	VARSIZE(new_t) = strlen(outstr) + VARHDRSZ;
	memcpy((void *) VARDATA(new_t),
		   (void *) outstr,
		   strlen(outstr));

	/* free instr */
	pfree(instr);

	return (new_t);
}

char *
soundex(char *instr, char *outstr)
{
	/* ABCDEFGHIJKLMNOPQRSTUVWXYZ */
	char	   *table = "01230120022455012623010202";
	int			count = 0;

	while (!isalpha(instr[0]) && instr[0])
		++instr;

	if (!instr[0])
	{							/* Hey!  Where'd the string go? */
		outstr[0] = (char) 0;
		return outstr;
	}

	if (toupper(instr[0]) == 'P' && toupper(instr[1]) == 'H')
	{
		instr[0] = 'F';
		instr[1] = 'A';
	}

	*outstr++ = (char) toupper(*instr++);

	while (*instr && count < 5)
	{
		if (isalpha(*instr) && *instr != *(instr - 1))
		{
			*outstr = table[toupper(instr[0]) - 'A'];
			if (*outstr != '0')
			{
				++outstr;
				++count;
			}
		}
		++instr;
	}

	*outstr = '\0';
	return (outstr);
}
