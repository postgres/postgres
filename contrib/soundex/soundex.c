/* $Header: /cvsroot/pgsql/contrib/soundex/Attic/soundex.c,v 1.9 2000/12/03 20:45:31 tgl Exp $ */
#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include <ctype.h>
#include <string.h>
#include <stdio.h>


Datum text_soundex(PG_FUNCTION_ARGS);

static void soundex(const char *instr, char *outstr);

#define SOUNDEX_LEN 4


#define _textin(str) DirectFunctionCall1(textin, CStringGetDatum(str))
#define _textout(str) DatumGetPointer(DirectFunctionCall1(textout, PointerGetDatum(str)))


#ifndef SOUNDEX_TEST
/*
 * SQL function: text_soundex(text) returns text
 */
PG_FUNCTION_INFO_V1(text_soundex);

Datum
text_soundex(PG_FUNCTION_ARGS)
{
	char		outstr[SOUNDEX_LEN + 1];
	char	   *arg;

	arg = _textout(PG_GETARG_TEXT_P(0));

	soundex(arg, outstr);

	PG_RETURN_TEXT_P(_textin(outstr));
}

#endif /* not SOUNDEX_TEST */


/*                                  ABCDEFGHIJKLMNOPQRSTUVWXYZ */
static const char *soundex_table = "01230120022455012623010202";
#define soundex_code(letter) soundex_table[toupper((unsigned char) (letter)) - 'A']


static void
soundex(const char *instr, char *outstr)
{
	int			count;

	AssertArg(instr);
	AssertArg(outstr);

	outstr[SOUNDEX_LEN] = '\0';

	/* Skip leading non-alphabetic characters */
	while (!isalpha((unsigned char) instr[0]) && instr[0])
		++instr;

	/* No string left */
	if (!instr[0])
	{
		outstr[0] = (char) 0;
		return;
	}

	/* Take the first letter as is */
	*outstr++ = (char) toupper((unsigned char) *instr++);

	count = 1;
	while (*instr && count < SOUNDEX_LEN)
	{
		if (isalpha((unsigned char) *instr) &&
			soundex_code(*instr) != soundex_code(*(instr - 1)))
		{
			*outstr = soundex_code(instr[0]);
			if (*outstr != '0')
			{
				++outstr;
				++count;
			}
		}
		++instr;
	}

	/* Fill with 0's */
	while (count < SOUNDEX_LEN)
	{
		*outstr = '0';
		++outstr;
		++count;
	}
}



#ifdef SOUNDEX_TEST
int
main (int argc, char *argv[])
{
	if (argc < 2)
	{
		fprintf(stderr, "usage: %s string\n", argv[0]);
		return 1;
	}
	else
	{
		char output[SOUNDEX_LEN + 1];

		soundex(argv[1], output);
		printf("soundex(%s) = %s\n", argv[1], output);
		return 0;
	}
}
#endif /* SOUNDEX_TEST */
