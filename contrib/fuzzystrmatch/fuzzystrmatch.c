/*
 * fuzzystrmatch.c
 *
 * Functions for "fuzzy" comparison of strings
 *
 * Joe Conway <mail@joeconway.com>
 *
 * Copyright (c) 2001-2003, PostgreSQL Global Development Group
 * ALL RIGHTS RESERVED;
 *
 * levenshtein()
 * -------------
 * Written based on a description of the algorithm by Michael Gilleland
 * found at http://www.merriampark.com/ld.htm
 * Also looked at levenshtein.c in the PHP 4.0.6 distribution for
 * inspiration.
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

#include "fuzzystrmatch.h"

/*
 * Calculates Levenshtein Distance between two strings.
 * Uses simplest and fastest cost model only, i.e. assumes a cost of 1 for
 * each deletion, substitution, or insertion.
 */
PG_FUNCTION_INFO_V1(levenshtein);
Datum
levenshtein(PG_FUNCTION_ARGS)
{
	char	   *str_s;
	char	   *str_s0;
	char	   *str_t;
	int			cols = 0;
	int			rows = 0;
	int		   *u_cells;
	int		   *l_cells;
	int		   *tmp;
	int			i;
	int			j;

	/*
	 * Fetch the arguments. str_s is referred to as the "source" cols =
	 * length of source + 1 to allow for the initialization column str_t
	 * is referred to as the "target", rows = length of target + 1 rows =
	 * length of target + 1 to allow for the initialization row
	 */
	str_s = DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(PG_GETARG_TEXT_P(0))));
	str_t = DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(PG_GETARG_TEXT_P(1))));

	cols = strlen(str_s) + 1;
	rows = strlen(str_t) + 1;

	/*
	 * Restrict the length of the strings being compared to something
	 * reasonable because we will have to perform rows * cols
	 * calculations. If longer strings need to be compared, increase
	 * MAX_LEVENSHTEIN_STRLEN to suit (but within your tolerance for speed
	 * and memory usage).
	 */
	if ((cols > MAX_LEVENSHTEIN_STRLEN + 1) || (rows > MAX_LEVENSHTEIN_STRLEN + 1))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("argument exceeds max length: %d",
						MAX_LEVENSHTEIN_STRLEN)));

	/*
	 * If either rows or cols is 0, the answer is the other value. This
	 * makes sense since it would take that many insertions the build a
	 * matching string
	 */

	if (cols == 0)
		PG_RETURN_INT32(rows);

	if (rows == 0)
		PG_RETURN_INT32(cols);

	/*
	 * Allocate two vectors of integers. One will be used for the "upper"
	 * row, the other for the "lower" row. Initialize the "upper" row to
	 * 0..cols
	 */
	u_cells = palloc(sizeof(int) * cols);
	for (i = 0; i < cols; i++)
		u_cells[i] = i;

	l_cells = palloc(sizeof(int) * cols);

	/*
	 * Use str_s0 to "rewind" the pointer to str_s in the nested for loop
	 * below
	 */
	str_s0 = str_s;

	/*
	 * Loop throught the rows, starting at row 1. Row 0 is used for the
	 * initial "upper" row.
	 */
	for (j = 1; j < rows; j++)
	{
		/*
		 * We'll always start with col 1, and initialize lower row col 0
		 * to j
		 */
		l_cells[0] = j;

		for (i = 1; i < cols; i++)
		{
			int			c = 0;
			int			c1 = 0;
			int			c2 = 0;
			int			c3 = 0;

			/*
			 * The "cost" value is 0 if the character at the current col
			 * position in the source string, matches the character at the
			 * current row position in the target string; cost is 1
			 * otherwise.
			 */
			c = ((CHAREQ(str_s, str_t)) ? 0 : 1);

			/*
			 * c1 is upper right cell plus 1
			 */
			c1 = u_cells[i] + 1;

			/*
			 * c2 is lower left cell plus 1
			 */
			c2 = l_cells[i - 1] + 1;

			/*
			 * c3 is cell diagonally above to the left plus "cost"
			 */
			c3 = u_cells[i - 1] + c;

			/*
			 * The lower right cell is set to the minimum of c1, c2, c3
			 */
			l_cells[i] = (c1 < c2 ? c1 : c2) < c3 ? (c1 < c2 ? c1 : c2) : c3;

			/*
			 * Increment the pointer to str_s
			 */
			NextChar(str_s);
		}

		/*
		 * Lower row now becomes the upper row, and the upper row gets
		 * reused as the new lower row.
		 */
		tmp = u_cells;
		u_cells = l_cells;
		l_cells = tmp;

		/*
		 * Increment the pointer to str_t
		 */
		NextChar(str_t);

		/*
		 * Rewind the pointer to str_s
		 */
		str_s = str_s0;
	}

	/*
	 * Because the final value (at position row, col) was swapped from the
	 * lower row to the upper row, that's where we'll find it.
	 */
	PG_RETURN_INT32(u_cells[cols - 1]);
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
	int			reqlen;
	char	   *str_i;
	size_t		str_i_len;
	char	   *metaph;
	text	   *result_text;
	int			retval;

	str_i = DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(PG_GETARG_TEXT_P(0))));
	str_i_len = strlen(str_i);

	if (str_i_len > MAX_METAPHONE_STRLEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("argument exceeds max length: %d",
						MAX_METAPHONE_STRLEN)));

	if (!(str_i_len > 0))
		ereport(ERROR,
				(errcode(ERRCODE_ZERO_LENGTH_CHARACTER_STRING),
				 errmsg("argument is empty string")));

	reqlen = PG_GETARG_INT32(1);
	if (reqlen > MAX_METAPHONE_STRLEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("output length exceeds max length: %d",
						MAX_METAPHONE_STRLEN)));

	if (!(reqlen > 0))
		ereport(ERROR,
				(errcode(ERRCODE_ZERO_LENGTH_CHARACTER_STRING),
				 errmsg("output cannot be empty string")));


	retval = _metaphone(str_i, reqlen, &metaph);
	if (retval == META_SUCCESS)
	{
		result_text = DatumGetTextP(DirectFunctionCall1(textin, CStringGetDatum(metaph)));
		PG_RETURN_TEXT_P(result_text);
	}
	else
	{
		/* internal error */
		elog(ERROR, "metaphone: failure");

		/*
		 * Keep the compiler quiet
		 */
		PG_RETURN_NULL();
	}
}


/*
 * Original code by Michael G Schwern starts here.
 * Code slightly modified for use as PostgreSQL
 * function (palloc, etc). Original includes
 * are rolled into fuzzystrmatch.h
 *------------------------------------------------------------------*/

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
char
Lookahead(char *word, int how_far)
{
	char		letter_ahead = '\0';	/* null by default */
	int			idx;

	for (idx = 0; word[idx] != '\0' && idx < how_far; idx++);
	/* Edge forward in the string... */

	letter_ahead = word[idx];	/* idx will be either == to how_far or at
								 * the end of the string */
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


int
_metaphone(
 /* IN */
		   char *word,
		   int max_phonemes,
 /* OUT */
		   char **phoned_word
)
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
		*phoned_word = palloc(sizeof(char) * strlen(word) +1);
		if (!*phoned_word)
			return META_ERROR;
	}
	else
	{
		*phoned_word = palloc(sizeof(char) * max_phonemes + 1);
		if (!*phoned_word)
			return META_ERROR;
	}

	/*-- The first phoneme has to be processed specially. --*/
	/* Find our first letter */
	for (; !isalpha((unsigned char) (Curr_Letter)); w_idx++)
	{
		/* On the off chance we were given nothing but crap... */
		if (Curr_Letter == '\0')
		{
			End_Phoned_Word;
			return META_SUCCESS;	/* For testing */
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
		 * THOUGHT:  It would be nice if, rather than having things
		 * like... well, SCI.  For SCI you encode the S, then have to
		 * remember to skip the C.	So the phonome SCI invades both S and
		 * C.  It would be better, IMHO, to skip the C from the S part of
		 * the encoding. Hell, I'm trying it.
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
				 * 'sh' if -CIA- or -CH, but not SCH, except SCHW. (SCHW
				 * is handled in S) S if -CI-, -CE- or -CY- dropped if
				 * -SCI-, SCE-, -SCY- (handed in S) else K
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
				 * -DGY- (handled in D) else J if in -GE-, -GI, -GY and
				 * not GG else K
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

	return (META_SUCCESS);
}	/* END metaphone */


/*
 * SQL function: soundex(text) returns text
 */
PG_FUNCTION_INFO_V1(soundex);

Datum
soundex(PG_FUNCTION_ARGS)
{
	char		outstr[SOUNDEX_LEN + 1];
	char	   *arg;

	arg = _textout(PG_GETARG_TEXT_P(0));

	_soundex(arg, outstr);

	PG_RETURN_TEXT_P(_textin(outstr));
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
