/*
 * fuzzystrmatch.h
 *
 * Functions for "fuzzy" comparison of strings
 *
 * Joe Conway <mail@joeconway.com>
 *
 * Copyright (c) 2001-2005, PostgreSQL Global Development Group
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

#ifndef FUZZYSTRMATCH_H
#define FUZZYSTRMATCH_H

#include "postgres.h"

#include <ctype.h>

#include "fmgr.h"
#include "utils/builtins.h"



/*
 * External declarations
 */
extern Datum levenshtein(PG_FUNCTION_ARGS);
extern Datum metaphone(PG_FUNCTION_ARGS);
extern Datum soundex(PG_FUNCTION_ARGS);
extern Datum difference(PG_FUNCTION_ARGS);

/*
 * Soundex
 */
static void _soundex(const char *instr, char *outstr);

#define SOUNDEX_LEN 4
#define _textin(str) DirectFunctionCall1(textin, CStringGetDatum(str))
#define _textout(str) DatumGetPointer(DirectFunctionCall1(textout, PointerGetDatum(str)))

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
 * Levenshtein
 */
#define STRLEN(p) strlen(p)
#define CHAREQ(p1, p2) (*(p1) == *(p2))
#define NextChar(p) ((p)++)
#define MAX_LEVENSHTEIN_STRLEN		255


/*
 * Metaphone
 */
#define MAX_METAPHONE_STRLEN		255

/*
 * Original code by Michael G Schwern starts here.
 * Code slightly modified for use as PostgreSQL
 * function (combined *.h into here).
 *------------------------------------------------------------------*/

/**************************************************************************
	metaphone -- Breaks english phrases down into their phonemes.

	Input
		word			--	An english word to be phonized
		max_phonemes	--	How many phonemes to calculate.  If 0, then it
							will phonize the entire phrase.
		phoned_word		--	The final phonized word.  (We'll allocate the
							memory.)
	Output
		error	--	A simple error flag, returns TRUE or FALSE

	NOTES:	ALL non-alpha characters are ignored, this includes whitespace,
	although non-alpha characters will break up phonemes.
****************************************************************************/


/**************************************************************************
	my constants -- constants I like

	Probably redundant.

***************************************************************************/

#define META_ERROR			FALSE
#define META_SUCCESS		TRUE
#define META_FAILURE		FALSE


/*	I add modifications to the traditional metaphone algorithm that you
	might find in books.  Define this if you want metaphone to behave
	traditionally */
#undef USE_TRADITIONAL_METAPHONE

/* Special encodings */
#define  SH		'X'
#define  TH		'0'

char		Lookahead(char *word, int how_far);
int
_metaphone(
 /* IN */
		   char *word,
		   int max_phonemes,
 /* OUT */
		   char **phoned_word
);

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

/* These form dipthongs when preceding H */
#define AFFECTH(c)	(getcode(c) & 4)	/* CGPST */

/* These make C and G soft */
#define MAKESOFT(c) (getcode(c) & 8)	/* EIY */

/* These prevent GH from becoming F */
#define NOGHTOF(c)	(getcode(c) & 16)	/* BDH */

#endif   /* FUZZYSTRMATCH_H */
