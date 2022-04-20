/*
 * fuzzystrmatch.c
 *
 * Functions for "fuzzy" comparison of strings
 *
 * Joe Conway <mail@joeconway.com>
 *
 * contrib/fuzzystrmatch/fuzzystrmatch.c
 * Copyright (c) 2001-2022, PostgreSQL Global Development Group
 * ALL RIGHTS RESERVED;
 *
 * metaphone()
 * -----------
 * Modified for PostgreSQL by Joe Conway.
 * Based on CPAN's "Text-Metaphone-1.96" by Michael G Schwern <schwern@pobox.com>
 * Code slightly modified for use as PostgreSQL function (palloc, elog, etc).
 * Metaphone was originally created by Lawrence Philips and presented in article
 * in "Computer Language" December 1990 issue.
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose, without fee, and without a written agreement
 * is hereby granted, provided that the above copyright notice and this
 * paragraph and the following two paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL THE AUTHORS OR DISTRIBUTORS BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 * LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
 * DOCUMENTATION, EVEN IF THE AUTHOR OR DISTRIBUTORS HAVE BEEN ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * THE AUTHORS AND DISTRIBUTORS SPECIFICALLY DISCLAIM ANY WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE AUTHOR AND DISTRIBUTORS HAS NO OBLIGATIONS TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 */

#include "postgres.h"

#include <ctype.h>

#include "mb/pg_wchar.h"
#include "utils/builtins.h"
#include "utils/varlena.h"

PG_MODULE_MAGIC;

/*
 * Soundex
 */
static void _soundex(const char *instr, char *outstr);

#define SOUNDEX_LEN 4

/*									ABCDEFGHIJKLMNOPQRSTUVWXYZ */
static const char *soundex_table = "01230120022455012623010202";

static char
soundex_code(char letter)
{
	letter = toupper((unsigned char) letter);
	/* Defend against non-ASCII letters */
	if (letter >= 'A' && letter <= 'Z')
		return soundex_table[letter - 'A'];
	return letter;
}

/*
 * Metaphone
 */
#define MAX_METAPHONE_STRLEN		255

/*
 * Original code by Michael G Schwern starts here.
 * Code slightly modified for use as PostgreSQL function.
 */


/**************************************************************************
	metaphone -- Breaks english phrases down into their phonemes.

	Input
		word			--	An english word to be phonized
		max_phonemes	--	How many phonemes to calculate.  If 0, then it
							will phonize the entire phrase.
		phoned_word		--	The final phonized word.  (We'll allocate the
							memory.)
	Output
		error	--	A simple error flag, returns true or false

	NOTES:	ALL non-alpha characters are ignored, this includes whitespace,
	although non-alpha characters will break up phonemes.
****************************************************************************/


/*	I add modifications to the traditional metaphone algorithm that you
	might find in books.  Define this if you want metaphone to behave
	traditionally */
#undef USE_TRADITIONAL_METAPHONE

/* Special encodings */
#define  SH		'X'
#define  TH		'0'

static char Lookahead(char *word, int how_far);
static void _metaphone(char *word, int max_phonemes, char **phoned_word);

/* Metachar.h ... little bits about characters for metaphone */


/*-- Character encoding array & accessing macros --*/
/* Stolen directly out of the book... */
static const char _codes[26] = {
	1, 16, 4, 16, 9, 2, 4, 16, 9, 2, 0, 2, 2, 2, 1, 4, 0, 2, 4, 4, 1, 0, 0, 0, 8, 0
/*	a  b c	d e f g  h i j k l m n o p q r s t u v w x y z */
};

static int
getcode(char c)
{
	if (isalpha((unsigned char) c))
	{
		c = toupper((unsigned char) c);
		/* Defend against non-ASCII letters */
		if (c >= 'A' && c <= 'Z')
			return _codes[c - 'A'];
	}
	return 0;
}

#define isvowel(c)	(getcode(c) & 1)	/* AEIOU */

/* These letters are passed through unchanged */
#define NOCHANGE(c) (getcode(c) & 2)	/* FJMNR */

/* These form diphthongs when preceding H */
#define AFFECTH(c)	(getcode(c) & 4)	/* CGPST */

/* These make C and G soft */
#define MAKESOFT(c) (getcode(c) & 8)	/* EIY */

/* These prevent GH from becoming F */
#define NOGHTOF(c)	(getcode(c) & 16)	/* BDH */

PG_FUNCTION_INFO_V1(levenshtein_with_costs);
Datum
levenshtein_with_costs(PG_FUNCTION_ARGS)
{
	text	   *src = PG_GETARG_TEXT_PP(0);
	text	   *dst = PG_GETARG_TEXT_PP(1);
	int			ins_c = PG_GETARG_INT32(2);
	int			del_c = PG_GETARG_INT32(3);
	int			sub_c = PG_GETARG_INT32(4);
	const char *s_data;
	const char *t_data;
	int			s_bytes,
				t_bytes;

	/* Extract a pointer to the actual character data */
	s_data = VARDATA_ANY(src);
	t_data = VARDATA_ANY(dst);
	/* Determine length of each string in bytes */
	s_bytes = VARSIZE_ANY_EXHDR(src);
	t_bytes = VARSIZE_ANY_EXHDR(dst);

	PG_RETURN_INT32(varstr_levenshtein(s_data, s_bytes, t_data, t_bytes,
									   ins_c, del_c, sub_c, false));
}


PG_FUNCTION_INFO_V1(levenshtein);
Datum
levenshtein(PG_FUNCTION_ARGS)
{
	text	   *src = PG_GETARG_TEXT_PP(0);
	text	   *dst = PG_GETARG_TEXT_PP(1);
	const char *s_data;
	const char *t_data;
	int			s_bytes,
				t_bytes;

	/* Extract a pointer to the actual character data */
	s_data = VARDATA_ANY(src);
	t_data = VARDATA_ANY(dst);
	/* Determine length of each string in bytes */
	s_bytes = VARSIZE_ANY_EXHDR(src);
	t_bytes = VARSIZE_ANY_EXHDR(dst);

	PG_RETURN_INT32(varstr_levenshtein(s_data, s_bytes, t_data, t_bytes,
									   1, 1, 1, false));
}


PG_FUNCTION_INFO_V1(levenshtein_less_equal_with_costs);
Datum
levenshtein_less_equal_with_costs(PG_FUNCTION_ARGS)
{
	text	   *src = PG_GETARG_TEXT_PP(0);
	text	   *dst = PG_GETARG_TEXT_PP(1);
	int			ins_c = PG_GETARG_INT32(2);
	int			del_c = PG_GETARG_INT32(3);
	int			sub_c = PG_GETARG_INT32(4);
	int			max_d = PG_GETARG_INT32(5);
	const char *s_data;
	const char *t_data;
	int			s_bytes,
				t_bytes;

	/* Extract a pointer to the actual character data */
	s_data = VARDATA_ANY(src);
	t_data = VARDATA_ANY(dst);
	/* Determine length of each string in bytes */
	s_bytes = VARSIZE_ANY_EXHDR(src);
	t_bytes = VARSIZE_ANY_EXHDR(dst);

	PG_RETURN_INT32(varstr_levenshtein_less_equal(s_data, s_bytes,
												  t_data, t_bytes,
												  ins_c, del_c, sub_c,
												  max_d, false));
}


PG_FUNCTION_INFO_V1(levenshtein_less_equal);
Datum
levenshtein_less_equal(PG_FUNCTION_ARGS)
{
	text	   *src = PG_GETARG_TEXT_PP(0);
	text	   *dst = PG_GETARG_TEXT_PP(1);
	int			max_d = PG_GETARG_INT32(2);
	const char *s_data;
	const char *t_data;
	int			s_bytes,
				t_bytes;

	/* Extract a pointer to the actual character data */
	s_data = VARDATA_ANY(src);
	t_data = VARDATA_ANY(dst);
	/* Determine length of each string in bytes */
	s_bytes = VARSIZE_ANY_EXHDR(src);
	t_bytes = VARSIZE_ANY_EXHDR(dst);

	PG_RETURN_INT32(varstr_levenshtein_less_equal(s_data, s_bytes,
												  t_data, t_bytes,
												  1, 1, 1,
												  max_d, false));
}


/*
 * Calculates the metaphone of an input string.
 * Returns number of characters requested
 * (suggested value is 4)
 */
PG_FUNCTION_INFO_V1(metaphone);
Datum
metaphone(PG_FUNCTION_ARGS)
{
	char	   *str_i = TextDatumGetCString(PG_GETARG_DATUM(0));
	size_t		str_i_len = strlen(str_i);
	int			reqlen;
	char	   *metaph;

	/* return an empty string if we receive one */
	if (!(str_i_len > 0))
		PG_RETURN_TEXT_P(cstring_to_text(""));

	if (str_i_len > MAX_METAPHONE_STRLEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("argument exceeds the maximum length of %d bytes",
						MAX_METAPHONE_STRLEN)));

	reqlen = PG_GETARG_INT32(1);
	if (reqlen > MAX_METAPHONE_STRLEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("output exceeds the maximum length of %d bytes",
						MAX_METAPHONE_STRLEN)));

	if (!(reqlen > 0))
		ereport(ERROR,
				(errcode(ERRCODE_ZERO_LENGTH_CHARACTER_STRING),
				 errmsg("output cannot be empty string")));

	_metaphone(str_i, reqlen, &metaph);
	PG_RETURN_TEXT_P(cstring_to_text(metaph));
}


/*
 * Original code by Michael G Schwern starts here.
 * Code slightly modified for use as PostgreSQL
 * function (palloc, etc).
 */

/* I suppose I could have been using a character pointer instead of
 * accessing the array directly... */

/* Look at the next letter in the word */
#define Next_Letter (toupper((unsigned char) word[w_idx+1]))
/* Look at the current letter in the word */
#define Curr_Letter (toupper((unsigned char) word[w_idx]))
/* Go N letters back. */
#define Look_Back_Letter(n) \
	(w_idx >= (n) ? toupper((unsigned char) word[w_idx-(n)]) : '\0')
/* Previous letter.  I dunno, should this return null on failure? */
#define Prev_Letter (Look_Back_Letter(1))
/* Look two letters down.  It makes sure you don't walk off the string. */
#define After_Next_Letter \
	(Next_Letter != '\0' ? toupper((unsigned char) word[w_idx+2]) : '\0')
#define Look_Ahead_Letter(n) toupper((unsigned char) Lookahead(word+w_idx, n))


/* Allows us to safely look ahead an arbitrary # of letters */
/* I probably could have just used strlen... */
static char
Lookahead(char *word, int how_far)
{
	char		letter_ahead = '\0';	/* null by default */
	int			idx;

	for (idx = 0; word[idx] != '\0' && idx < how_far; idx++);
	/* Edge forward in the string... */

	letter_ahead = word[idx];	/* idx will be either == to how_far or at the
								 * end of the string */
	return letter_ahead;
}


/* phonize one letter */
#define Phonize(c)	do {(*phoned_word)[p_idx++] = c;} while (0)
/* Slap a null character on the end of the phoned word */
#define End_Phoned_Word do {(*phoned_word)[p_idx] = '\0';} while (0)
/* How long is the phoned word? */
#define Phone_Len	(p_idx)

/* Note is a letter is a 'break' in the word */
#define Isbreak(c)	(!isalpha((unsigned char) (c)))


static void
_metaphone(char *word,			/* IN */
		   int max_phonemes,
		   char **phoned_word)	/* OUT */
{
	int			w_idx = 0;		/* point in the phonization we're at. */
	int			p_idx = 0;		/* end of the phoned phrase */

	/*-- Parameter checks --*/

	/*
	 * Shouldn't be necessary, but left these here anyway jec Aug 3, 2001
	 */

	/* Negative phoneme length is meaningless */
	if (!(max_phonemes > 0))
		/* internal error */
		elog(ERROR, "metaphone: Requested output length must be > 0");

	/* Empty/null string is meaningless */
	if ((word == NULL) || !(strlen(word) > 0))
		/* internal error */
		elog(ERROR, "metaphone: Input string length must be > 0");

	/*-- Allocate memory for our phoned_phrase --*/
	if (max_phonemes == 0)
	{							/* Assume largest possible */
		*phoned_word = palloc(sizeof(char) * strlen(word) + 1);
	}
	else
	{
		*phoned_word = palloc(sizeof(char) * max_phonemes + 1);
	}

	/*-- The first phoneme has to be processed specially. --*/
	/* Find our first letter */
	for (; !isalpha((unsigned char) (Curr_Letter)); w_idx++)
	{
		/* On the off chance we were given nothing but crap... */
		if (Curr_Letter == '\0')
		{
			End_Phoned_Word;
			return;
		}
	}

	switch (Curr_Letter)
	{
			/* AE becomes E */
		case 'A':
			if (Next_Letter == 'E')
			{
				Phonize('E');
				w_idx += 2;
			}
			/* Remember, preserve vowels at the beginning */
			else
			{
				Phonize('A');
				w_idx++;
			}
			break;
			/* [GKP]N becomes N */
		case 'G':
		case 'K':
		case 'P':
			if (Next_Letter == 'N')
			{
				Phonize('N');
				w_idx += 2;
			}
			break;

			/*
			 * WH becomes H, WR becomes R W if followed by a vowel
			 */
		case 'W':
			if (Next_Letter == 'H' ||
				Next_Letter == 'R')
			{
				Phonize(Next_Letter);
				w_idx += 2;
			}
			else if (isvowel(Next_Letter))
			{
				Phonize('W');
				w_idx += 2;
			}
			/* else ignore */
			break;
			/* X becomes S */
		case 'X':
			Phonize('S');
			w_idx++;
			break;
			/* Vowels are kept */

			/*
			 * We did A already case 'A': case 'a':
			 */
		case 'E':
		case 'I':
		case 'O':
		case 'U':
			Phonize(Curr_Letter);
			w_idx++;
			break;
		default:
			/* do nothing */
			break;
	}



	/* On to the metaphoning */
	for (; Curr_Letter != '\0' &&
		 (max_phonemes == 0 || Phone_Len < max_phonemes);
		 w_idx++)
	{
		/*
		 * How many letters to skip because an earlier encoding handled
		 * multiple letters
		 */
		unsigned short int skip_letter = 0;


		/*
		 * THOUGHT:  It would be nice if, rather than having things like...
		 * well, SCI.  For SCI you encode the S, then have to remember to skip
		 * the C.  So the phonome SCI invades both S and C.  It would be
		 * better, IMHO, to skip the C from the S part of the encoding. Hell,
		 * I'm trying it.
		 */

		/* Ignore non-alphas */
		if (!isalpha((unsigned char) (Curr_Letter)))
			continue;

		/* Drop duplicates, except CC */
		if (Curr_Letter == Prev_Letter &&
			Curr_Letter != 'C')
			continue;

		switch (Curr_Letter)
		{
				/* B -> B unless in MB */
			case 'B':
				if (Prev_Letter != 'M')
					Phonize('B');
				break;

				/*
				 * 'sh' if -CIA- or -CH, but not SCH, except SCHW. (SCHW is
				 * handled in S) S if -CI-, -CE- or -CY- dropped if -SCI-,
				 * SCE-, -SCY- (handed in S) else K
				 */
			case 'C':
				if (MAKESOFT(Next_Letter))
				{				/* C[IEY] */
					if (After_Next_Letter == 'A' &&
						Next_Letter == 'I')
					{			/* CIA */
						Phonize(SH);
					}
					/* SC[IEY] */
					else if (Prev_Letter == 'S')
					{
						/* Dropped */
					}
					else
						Phonize('S');
				}
				else if (Next_Letter == 'H')
				{
#ifndef USE_TRADITIONAL_METAPHONE
					if (After_Next_Letter == 'R' ||
						Prev_Letter == 'S')
					{			/* Christ, School */
						Phonize('K');
					}
					else
						Phonize(SH);
#else
					Phonize(SH);
#endif
					skip_letter++;
				}
				else
					Phonize('K');
				break;

				/*
				 * J if in -DGE-, -DGI- or -DGY- else T
				 */
			case 'D':
				if (Next_Letter == 'G' &&
					MAKESOFT(After_Next_Letter))
				{
					Phonize('J');
					skip_letter++;
				}
				else
					Phonize('T');
				break;

				/*
				 * F if in -GH and not B--GH, D--GH, -H--GH, -H---GH else
				 * dropped if -GNED, -GN, else dropped if -DGE-, -DGI- or
				 * -DGY- (handled in D) else J if in -GE-, -GI, -GY and not GG
				 * else K
				 */
			case 'G':
				if (Next_Letter == 'H')
				{
					if (!(NOGHTOF(Look_Back_Letter(3)) ||
						  Look_Back_Letter(4) == 'H'))
					{
						Phonize('F');
						skip_letter++;
					}
					else
					{
						/* silent */
					}
				}
				else if (Next_Letter == 'N')
				{
					if (Isbreak(After_Next_Letter) ||
						(After_Next_Letter == 'E' &&
						 Look_Ahead_Letter(3) == 'D'))
					{
						/* dropped */
					}
					else
						Phonize('K');
				}
				else if (MAKESOFT(Next_Letter) &&
						 Prev_Letter != 'G')
					Phonize('J');
				else
					Phonize('K');
				break;
				/* H if before a vowel and not after C,G,P,S,T */
			case 'H':
				if (isvowel(Next_Letter) &&
					!AFFECTH(Prev_Letter))
					Phonize('H');
				break;

				/*
				 * dropped if after C else K
				 */
			case 'K':
				if (Prev_Letter != 'C')
					Phonize('K');
				break;

				/*
				 * F if before H else P
				 */
			case 'P':
				if (Next_Letter == 'H')
					Phonize('F');
				else
					Phonize('P');
				break;

				/*
				 * K
				 */
			case 'Q':
				Phonize('K');
				break;

				/*
				 * 'sh' in -SH-, -SIO- or -SIA- or -SCHW- else S
				 */
			case 'S':
				if (Next_Letter == 'I' &&
					(After_Next_Letter == 'O' ||
					 After_Next_Letter == 'A'))
					Phonize(SH);
				else if (Next_Letter == 'H')
				{
					Phonize(SH);
					skip_letter++;
				}
#ifndef USE_TRADITIONAL_METAPHONE
				else if (Next_Letter == 'C' &&
						 Look_Ahead_Letter(2) == 'H' &&
						 Look_Ahead_Letter(3) == 'W')
				{
					Phonize(SH);
					skip_letter += 2;
				}
#endif
				else
					Phonize('S');
				break;

				/*
				 * 'sh' in -TIA- or -TIO- else 'th' before H else T
				 */
			case 'T':
				if (Next_Letter == 'I' &&
					(After_Next_Letter == 'O' ||
					 After_Next_Letter == 'A'))
					Phonize(SH);
				else if (Next_Letter == 'H')
				{
					Phonize(TH);
					skip_letter++;
				}
				else
					Phonize('T');
				break;
				/* F */
			case 'V':
				Phonize('F');
				break;
				/* W before a vowel, else dropped */
			case 'W':
				if (isvowel(Next_Letter))
					Phonize('W');
				break;
				/* KS */
			case 'X':
				Phonize('K');
				if (max_phonemes == 0 || Phone_Len < max_phonemes)
					Phonize('S');
				break;
				/* Y if followed by a vowel */
			case 'Y':
				if (isvowel(Next_Letter))
					Phonize('Y');
				break;
				/* S */
			case 'Z':
				Phonize('S');
				break;
				/* No transformation */
			case 'F':
			case 'J':
			case 'L':
			case 'M':
			case 'N':
			case 'R':
				Phonize(Curr_Letter);
				break;
			default:
				/* nothing */
				break;
		}						/* END SWITCH */

		w_idx += skip_letter;
	}							/* END FOR */

	End_Phoned_Word;
}								/* END metaphone */


/*
 * SQL function: soundex(text) returns text
 */
PG_FUNCTION_INFO_V1(soundex);

Datum
soundex(PG_FUNCTION_ARGS)
{
	char		outstr[SOUNDEX_LEN + 1];
	char	   *arg;

	arg = text_to_cstring(PG_GETARG_TEXT_PP(0));

	_soundex(arg, outstr);

	PG_RETURN_TEXT_P(cstring_to_text(outstr));
}

static void
_soundex(const char *instr, char *outstr)
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

PG_FUNCTION_INFO_V1(difference);

Datum
difference(PG_FUNCTION_ARGS)
{
	char		sndx1[SOUNDEX_LEN + 1],
				sndx2[SOUNDEX_LEN + 1];
	int			i,
				result;

	_soundex(text_to_cstring(PG_GETARG_TEXT_PP(0)), sndx1);
	_soundex(text_to_cstring(PG_GETARG_TEXT_PP(1)), sndx2);

	result = 0;
	for (i = 0; i < SOUNDEX_LEN; i++)
	{
		if (sndx1[i] == sndx2[i])
			result++;
	}

	PG_RETURN_INT32(result);
}
