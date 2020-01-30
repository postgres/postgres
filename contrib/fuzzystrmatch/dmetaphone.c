/*
 * This is a port of the Double Metaphone algorithm for use in PostgreSQL.
 *
 * contrib/fuzzystrmatch/dmetaphone.c
 *
 * Double Metaphone computes 2 "sounds like" strings - a primary and an
 * alternate. In most cases they are the same, but for foreign names
 * especially they can be a bit different, depending on pronunciation.
 *
 * Information on using Double Metaphone can be found at
 *	 http://www.codeproject.com/string/dmetaphone1.asp
 * and the original article describing it can be found at
 *	 http://drdobbs.com/184401251
 *
 * For PostgreSQL we provide 2 functions - one for the primary and one for
 * the alternate. That way the functions are pure text->text mappings that
 * are useful in functional indexes. These are 'dmetaphone' for the
 * primary and 'dmetaphone_alt' for the alternate.
 *
 * Assuming that dmetaphone.so is in $libdir, the SQL to set up the
 * functions looks like this:
 *
 * CREATE FUNCTION dmetaphone (text) RETURNS text
 *	  LANGUAGE C IMMUTABLE STRICT
 *	  AS '$libdir/dmetaphone', 'dmetaphone';
 *
 * CREATE FUNCTION dmetaphone_alt (text) RETURNS text
 *	  LANGUAGE C IMMUTABLE STRICT
 *	  AS '$libdir/dmetaphone', 'dmetaphone_alt';
 *
 * Note that you have to declare the functions IMMUTABLE if you want to
 * use them in functional indexes, and you have to declare them as STRICT
 * as they do not check for NULL input, and will segfault if given NULL input.
 * (See below for alternative ) Declaring them as STRICT means PostgreSQL
 * will never call them with NULL, but instead assume the result is NULL,
 * which is what we (I) want.
 *
 * Alternatively, compile with -DDMETAPHONE_NOSTRICT and the functions
 * will detect NULL input and return NULL. The you don't have to declare them
 * as STRICT.
 *
 * There is a small inefficiency here - each function call actually computes
 * both the primary and the alternate and then throws away the one it doesn't
 * need. That's the way the perl module was written, because perl can handle
 * a list return more easily than we can in PostgreSQL. The result has been
 * fast enough for my needs, but it could maybe be optimized a bit to remove
 * that behaviour.
 *
 */


/***************************** COPYRIGHT NOTICES ***********************

Most of this code is directly from the Text::DoubleMetaphone perl module
version 0.05 available from https://www.cpan.org/.
It bears this copyright notice:


  Copyright 2000, Maurice Aubrey <maurice@hevanet.com>.
  All rights reserved.

  This code is based heavily on the C++ implementation by
  Lawrence Philips and incorporates several bug fixes courtesy
  of Kevin Atkinson <kevina@users.sourceforge.net>.

  This module is free software; you may redistribute it and/or
  modify it under the same terms as Perl itself.

The remaining code is authored by Andrew Dunstan <amdunstan@ncshp.org> and
<andrew@dunslane.net> and is covered this copyright:

  Copyright 2003, North Carolina State Highway Patrol.
  All rights reserved.

  Permission to use, copy, modify, and distribute this software and its
  documentation for any purpose, without fee, and without a written agreement
  is hereby granted, provided that the above copyright notice and this
  paragraph and the following two paragraphs appear in all copies.

  IN NO EVENT SHALL THE NORTH CAROLINA STATE HIGHWAY PATROL BE LIABLE TO ANY
  PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES,
  INCLUDING LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
  DOCUMENTATION, EVEN IF THE NORTH CAROLINA STATE HIGHWAY PATROL HAS BEEN
  ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  THE NORTH CAROLINA STATE HIGHWAY PATROL SPECIFICALLY DISCLAIMS ANY
  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED
  HEREUNDER IS ON AN "AS IS" BASIS, AND THE NORTH CAROLINA STATE HIGHWAY PATROL
  HAS NO OBLIGATIONS TO PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
  MODIFICATIONS.

***********************************************************************/


/* include these first, according to the docs */
#ifndef DMETAPHONE_MAIN

#include "postgres.h"

#include "utils/builtins.h"

/* turn off assertions for embedded function */
#define NDEBUG

#else							/* DMETAPHONE_MAIN */

/* we need these if we didn't get them from postgres.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#endif							/* DMETAPHONE_MAIN */

#include <assert.h>
#include <ctype.h>

/* prototype for the main function we got from the perl module */
static void DoubleMetaphone(char *, char **);

#ifndef DMETAPHONE_MAIN

/*
 * The PostgreSQL visible dmetaphone function.
 */

PG_FUNCTION_INFO_V1(dmetaphone);

Datum
dmetaphone(PG_FUNCTION_ARGS)
{
	text	   *arg;
	char	   *aptr,
			   *codes[2],
			   *code;

#ifdef DMETAPHONE_NOSTRICT
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();
#endif
	arg = PG_GETARG_TEXT_PP(0);
	aptr = text_to_cstring(arg);

	DoubleMetaphone(aptr, codes);
	code = codes[0];
	if (!code)
		code = "";

	PG_RETURN_TEXT_P(cstring_to_text(code));
}

/*
 * The PostgreSQL visible dmetaphone_alt function.
 */

PG_FUNCTION_INFO_V1(dmetaphone_alt);

Datum
dmetaphone_alt(PG_FUNCTION_ARGS)
{
	text	   *arg;
	char	   *aptr,
			   *codes[2],
			   *code;

#ifdef DMETAPHONE_NOSTRICT
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();
#endif
	arg = PG_GETARG_TEXT_PP(0);
	aptr = text_to_cstring(arg);

	DoubleMetaphone(aptr, codes);
	code = codes[1];
	if (!code)
		code = "";

	PG_RETURN_TEXT_P(cstring_to_text(code));
}


/* here is where we start the code imported from the perl module */

/* all memory handling is done with these macros */

#define META_MALLOC(v,n,t) \
		  (v = (t*)palloc(((n)*sizeof(t))))

#define META_REALLOC(v,n,t) \
					  (v = (t*)repalloc((v),((n)*sizeof(t))))

/*
 * Don't do pfree - it seems to cause a SIGSEGV sometimes - which might have just
 * been caused by reloading the module in development.
 * So we rely on context cleanup - Tom Lane says pfree shouldn't be necessary
 * in a case like this.
 */

#define META_FREE(x) ((void)true)	/* pfree((x)) */
#else							/* not defined DMETAPHONE_MAIN */

/* use the standard malloc library when not running in PostgreSQL */

#define META_MALLOC(v,n,t) \
		  (v = (t*)malloc(((n)*sizeof(t))))

#define META_REALLOC(v,n,t) \
					  (v = (t*)realloc((v),((n)*sizeof(t))))

#define META_FREE(x) free((x))
#endif							/* defined DMETAPHONE_MAIN */



/* this typedef was originally in the perl module's .h file */

typedef struct
{
	char	   *str;
	int			length;
	int			bufsize;
	int			free_string_on_destroy;
}

metastring;

/*
 * remaining perl module funcs unchanged except for declaring them static
 * and reformatting to PostgreSQL indentation and to fit in 80 cols.
 *
 */

static metastring *
NewMetaString(const char *init_str)
{
	metastring *s;
	char		empty_string[] = "";

	META_MALLOC(s, 1, metastring);
	assert(s != NULL);

	if (init_str == NULL)
		init_str = empty_string;
	s->length = strlen(init_str);
	/* preallocate a bit more for potential growth */
	s->bufsize = s->length + 7;

	META_MALLOC(s->str, s->bufsize, char);
	assert(s->str != NULL);

	memcpy(s->str, init_str, s->length + 1);
	s->free_string_on_destroy = 1;

	return s;
}


static void
DestroyMetaString(metastring *s)
{
	if (s == NULL)
		return;

	if (s->free_string_on_destroy && (s->str != NULL))
		META_FREE(s->str);

	META_FREE(s);
}


static void
IncreaseBuffer(metastring *s, int chars_needed)
{
	META_REALLOC(s->str, (s->bufsize + chars_needed + 10), char);
	assert(s->str != NULL);
	s->bufsize = s->bufsize + chars_needed + 10;
}


static void
MakeUpper(metastring *s)
{
	char	   *i;

	for (i = s->str; *i; i++)
		*i = toupper((unsigned char) *i);
}


static int
IsVowel(metastring *s, int pos)
{
	char		c;

	if ((pos < 0) || (pos >= s->length))
		return 0;

	c = *(s->str + pos);
	if ((c == 'A') || (c == 'E') || (c == 'I') || (c == 'O') ||
		(c == 'U') || (c == 'Y'))
		return 1;

	return 0;
}


static int
SlavoGermanic(metastring *s)
{
	if ((char *) strstr(s->str, "W"))
		return 1;
	else if ((char *) strstr(s->str, "K"))
		return 1;
	else if ((char *) strstr(s->str, "CZ"))
		return 1;
	else if ((char *) strstr(s->str, "WITZ"))
		return 1;
	else
		return 0;
}


static char
GetAt(metastring *s, int pos)
{
	if ((pos < 0) || (pos >= s->length))
		return '\0';

	return ((char) *(s->str + pos));
}


static void
SetAt(metastring *s, int pos, char c)
{
	if ((pos < 0) || (pos >= s->length))
		return;

	*(s->str + pos) = c;
}


/*
   Caveats: the START value is 0 based
*/
static int
StringAt(metastring *s, int start, int length,...)
{
	char	   *test;
	char	   *pos;
	va_list		ap;

	if ((start < 0) || (start >= s->length))
		return 0;

	pos = (s->str + start);
	va_start(ap, length);

	do
	{
		test = va_arg(ap, char *);
		if (*test && (strncmp(pos, test, length) == 0))
		{
			va_end(ap);
			return 1;
		}
	}
	while (strcmp(test, "") != 0);

	va_end(ap);

	return 0;
}


static void
MetaphAdd(metastring *s, const char *new_str)
{
	int			add_length;

	if (new_str == NULL)
		return;

	add_length = strlen(new_str);
	if ((s->length + add_length) > (s->bufsize - 1))
		IncreaseBuffer(s, add_length);

	strcat(s->str, new_str);
	s->length += add_length;
}


static void
DoubleMetaphone(char *str, char **codes)
{
	int			length;
	metastring *original;
	metastring *primary;
	metastring *secondary;
	int			current;
	int			last;

	current = 0;
	/* we need the real length and last prior to padding */
	length = strlen(str);
	last = length - 1;
	original = NewMetaString(str);
	/* Pad original so we can index beyond end */
	MetaphAdd(original, "     ");

	primary = NewMetaString("");
	secondary = NewMetaString("");
	primary->free_string_on_destroy = 0;
	secondary->free_string_on_destroy = 0;

	MakeUpper(original);

	/* skip these when at start of word */
	if (StringAt(original, 0, 2, "GN", "KN", "PN", "WR", "PS", ""))
		current += 1;

	/* Initial 'X' is pronounced 'Z' e.g. 'Xavier' */
	if (GetAt(original, 0) == 'X')
	{
		MetaphAdd(primary, "S");	/* 'Z' maps to 'S' */
		MetaphAdd(secondary, "S");
		current += 1;
	}

	/* main loop */
	while ((primary->length < 4) || (secondary->length < 4))
	{
		if (current >= length)
			break;

		switch (GetAt(original, current))
		{
			case 'A':
			case 'E':
			case 'I':
			case 'O':
			case 'U':
			case 'Y':
				if (current == 0)
				{
					/* all init vowels now map to 'A' */
					MetaphAdd(primary, "A");
					MetaphAdd(secondary, "A");
				}
				current += 1;
				break;

			case 'B':

				/* "-mb", e.g", "dumb", already skipped over... */
				MetaphAdd(primary, "P");
				MetaphAdd(secondary, "P");

				if (GetAt(original, current + 1) == 'B')
					current += 2;
				else
					current += 1;
				break;

			case '\xc7':		/* C with cedilla */
				MetaphAdd(primary, "S");
				MetaphAdd(secondary, "S");
				current += 1;
				break;

			case 'C':
				/* various germanic */
				if ((current > 1)
					&& !IsVowel(original, current - 2)
					&& StringAt(original, (current - 1), 3, "ACH", "")
					&& ((GetAt(original, current + 2) != 'I')
						&& ((GetAt(original, current + 2) != 'E')
							|| StringAt(original, (current - 2), 6, "BACHER",
										"MACHER", ""))))
				{
					MetaphAdd(primary, "K");
					MetaphAdd(secondary, "K");
					current += 2;
					break;
				}

				/* special case 'caesar' */
				if ((current == 0)
					&& StringAt(original, current, 6, "CAESAR", ""))
				{
					MetaphAdd(primary, "S");
					MetaphAdd(secondary, "S");
					current += 2;
					break;
				}

				/* italian 'chianti' */
				if (StringAt(original, current, 4, "CHIA", ""))
				{
					MetaphAdd(primary, "K");
					MetaphAdd(secondary, "K");
					current += 2;
					break;
				}

				if (StringAt(original, current, 2, "CH", ""))
				{
					/* find 'michael' */
					if ((current > 0)
						&& StringAt(original, current, 4, "CHAE", ""))
					{
						MetaphAdd(primary, "K");
						MetaphAdd(secondary, "X");
						current += 2;
						break;
					}

					/* greek roots e.g. 'chemistry', 'chorus' */
					if ((current == 0)
						&& (StringAt(original, (current + 1), 5,
									 "HARAC", "HARIS", "")
							|| StringAt(original, (current + 1), 3, "HOR",
										"HYM", "HIA", "HEM", ""))
						&& !StringAt(original, 0, 5, "CHORE", ""))
					{
						MetaphAdd(primary, "K");
						MetaphAdd(secondary, "K");
						current += 2;
						break;
					}

					/* germanic, greek, or otherwise 'ch' for 'kh' sound */
					if ((StringAt(original, 0, 4, "VAN ", "VON ", "")
						 || StringAt(original, 0, 3, "SCH", ""))
					/* 'architect but not 'arch', 'orchestra', 'orchid' */
						|| StringAt(original, (current - 2), 6, "ORCHES",
									"ARCHIT", "ORCHID", "")
						|| StringAt(original, (current + 2), 1, "T", "S",
									"")
						|| ((StringAt(original, (current - 1), 1,
									  "A", "O", "U", "E", "")
							 || (current == 0))

					/*
					 * e.g., 'wachtler', 'wechsler', but not 'tichner'
					 */
							&& StringAt(original, (current + 2), 1, "L", "R",
										"N", "M", "B", "H", "F", "V", "W",
										" ", "")))
					{
						MetaphAdd(primary, "K");
						MetaphAdd(secondary, "K");
					}
					else
					{
						if (current > 0)
						{
							if (StringAt(original, 0, 2, "MC", ""))
							{
								/* e.g., "McHugh" */
								MetaphAdd(primary, "K");
								MetaphAdd(secondary, "K");
							}
							else
							{
								MetaphAdd(primary, "X");
								MetaphAdd(secondary, "K");
							}
						}
						else
						{
							MetaphAdd(primary, "X");
							MetaphAdd(secondary, "X");
						}
					}
					current += 2;
					break;
				}
				/* e.g, 'czerny' */
				if (StringAt(original, current, 2, "CZ", "")
					&& !StringAt(original, (current - 2), 4, "WICZ", ""))
				{
					MetaphAdd(primary, "S");
					MetaphAdd(secondary, "X");
					current += 2;
					break;
				}

				/* e.g., 'focaccia' */
				if (StringAt(original, (current + 1), 3, "CIA", ""))
				{
					MetaphAdd(primary, "X");
					MetaphAdd(secondary, "X");
					current += 3;
					break;
				}

				/* double 'C', but not if e.g. 'McClellan' */
				if (StringAt(original, current, 2, "CC", "")
					&& !((current == 1) && (GetAt(original, 0) == 'M')))
				{
					/* 'bellocchio' but not 'bacchus' */
					if (StringAt(original, (current + 2), 1, "I", "E", "H", "")
						&& !StringAt(original, (current + 2), 2, "HU", ""))
					{
						/* 'accident', 'accede' 'succeed' */
						if (((current == 1)
							 && (GetAt(original, current - 1) == 'A'))
							|| StringAt(original, (current - 1), 5, "UCCEE",
										"UCCES", ""))
						{
							MetaphAdd(primary, "KS");
							MetaphAdd(secondary, "KS");
							/* 'bacci', 'bertucci', other italian */
						}
						else
						{
							MetaphAdd(primary, "X");
							MetaphAdd(secondary, "X");
						}
						current += 3;
						break;
					}
					else
					{			/* Pierce's rule */
						MetaphAdd(primary, "K");
						MetaphAdd(secondary, "K");
						current += 2;
						break;
					}
				}

				if (StringAt(original, current, 2, "CK", "CG", "CQ", ""))
				{
					MetaphAdd(primary, "K");
					MetaphAdd(secondary, "K");
					current += 2;
					break;
				}

				if (StringAt(original, current, 2, "CI", "CE", "CY", ""))
				{
					/* italian vs. english */
					if (StringAt
						(original, current, 3, "CIO", "CIE", "CIA", ""))
					{
						MetaphAdd(primary, "S");
						MetaphAdd(secondary, "X");
					}
					else
					{
						MetaphAdd(primary, "S");
						MetaphAdd(secondary, "S");
					}
					current += 2;
					break;
				}

				/* else */
				MetaphAdd(primary, "K");
				MetaphAdd(secondary, "K");

				/* name sent in 'mac caffrey', 'mac gregor */
				if (StringAt(original, (current + 1), 2, " C", " Q", " G", ""))
					current += 3;
				else if (StringAt(original, (current + 1), 1, "C", "K", "Q", "")
						 && !StringAt(original, (current + 1), 2,
									  "CE", "CI", ""))
					current += 2;
				else
					current += 1;
				break;

			case 'D':
				if (StringAt(original, current, 2, "DG", ""))
				{
					if (StringAt(original, (current + 2), 1,
								 "I", "E", "Y", ""))
					{
						/* e.g. 'edge' */
						MetaphAdd(primary, "J");
						MetaphAdd(secondary, "J");
						current += 3;
						break;
					}
					else
					{
						/* e.g. 'edgar' */
						MetaphAdd(primary, "TK");
						MetaphAdd(secondary, "TK");
						current += 2;
						break;
					}
				}

				if (StringAt(original, current, 2, "DT", "DD", ""))
				{
					MetaphAdd(primary, "T");
					MetaphAdd(secondary, "T");
					current += 2;
					break;
				}

				/* else */
				MetaphAdd(primary, "T");
				MetaphAdd(secondary, "T");
				current += 1;
				break;

			case 'F':
				if (GetAt(original, current + 1) == 'F')
					current += 2;
				else
					current += 1;
				MetaphAdd(primary, "F");
				MetaphAdd(secondary, "F");
				break;

			case 'G':
				if (GetAt(original, current + 1) == 'H')
				{
					if ((current > 0) && !IsVowel(original, current - 1))
					{
						MetaphAdd(primary, "K");
						MetaphAdd(secondary, "K");
						current += 2;
						break;
					}

					if (current < 3)
					{
						/* 'ghislane', ghiradelli */
						if (current == 0)
						{
							if (GetAt(original, current + 2) == 'I')
							{
								MetaphAdd(primary, "J");
								MetaphAdd(secondary, "J");
							}
							else
							{
								MetaphAdd(primary, "K");
								MetaphAdd(secondary, "K");
							}
							current += 2;
							break;
						}
					}

					/*
					 * Parker's rule (with some further refinements) - e.g.,
					 * 'hugh'
					 */
					if (((current > 1)
						 && StringAt(original, (current - 2), 1,
									 "B", "H", "D", ""))
					/* e.g., 'bough' */
						|| ((current > 2)
							&& StringAt(original, (current - 3), 1,
										"B", "H", "D", ""))
					/* e.g., 'broughton' */
						|| ((current > 3)
							&& StringAt(original, (current - 4), 1,
										"B", "H", "")))
					{
						current += 2;
						break;
					}
					else
					{
						/*
						 * e.g., 'laugh', 'McLaughlin', 'cough', 'gough',
						 * 'rough', 'tough'
						 */
						if ((current > 2)
							&& (GetAt(original, current - 1) == 'U')
							&& StringAt(original, (current - 3), 1, "C",
										"G", "L", "R", "T", ""))
						{
							MetaphAdd(primary, "F");
							MetaphAdd(secondary, "F");
						}
						else if ((current > 0)
								 && GetAt(original, current - 1) != 'I')
						{


							MetaphAdd(primary, "K");
							MetaphAdd(secondary, "K");
						}

						current += 2;
						break;
					}
				}

				if (GetAt(original, current + 1) == 'N')
				{
					if ((current == 1) && IsVowel(original, 0)
						&& !SlavoGermanic(original))
					{
						MetaphAdd(primary, "KN");
						MetaphAdd(secondary, "N");
					}
					else
						/* not e.g. 'cagney' */
						if (!StringAt(original, (current + 2), 2, "EY", "")
							&& (GetAt(original, current + 1) != 'Y')
							&& !SlavoGermanic(original))
					{
						MetaphAdd(primary, "N");
						MetaphAdd(secondary, "KN");
					}
					else
					{
						MetaphAdd(primary, "KN");
						MetaphAdd(secondary, "KN");
					}
					current += 2;
					break;
				}

				/* 'tagliaro' */
				if (StringAt(original, (current + 1), 2, "LI", "")
					&& !SlavoGermanic(original))
				{
					MetaphAdd(primary, "KL");
					MetaphAdd(secondary, "L");
					current += 2;
					break;
				}

				/* -ges-,-gep-,-gel-, -gie- at beginning */
				if ((current == 0)
					&& ((GetAt(original, current + 1) == 'Y')
						|| StringAt(original, (current + 1), 2, "ES", "EP",
									"EB", "EL", "EY", "IB", "IL", "IN", "IE",
									"EI", "ER", "")))
				{
					MetaphAdd(primary, "K");
					MetaphAdd(secondary, "J");
					current += 2;
					break;
				}

				/* -ger-,  -gy- */
				if ((StringAt(original, (current + 1), 2, "ER", "")
					 || (GetAt(original, current + 1) == 'Y'))
					&& !StringAt(original, 0, 6,
								 "DANGER", "RANGER", "MANGER", "")
					&& !StringAt(original, (current - 1), 1, "E", "I", "")
					&& !StringAt(original, (current - 1), 3, "RGY", "OGY", ""))
				{
					MetaphAdd(primary, "K");
					MetaphAdd(secondary, "J");
					current += 2;
					break;
				}

				/* italian e.g, 'biaggi' */
				if (StringAt(original, (current + 1), 1, "E", "I", "Y", "")
					|| StringAt(original, (current - 1), 4,
								"AGGI", "OGGI", ""))
				{
					/* obvious germanic */
					if ((StringAt(original, 0, 4, "VAN ", "VON ", "")
						 || StringAt(original, 0, 3, "SCH", ""))
						|| StringAt(original, (current + 1), 2, "ET", ""))
					{
						MetaphAdd(primary, "K");
						MetaphAdd(secondary, "K");
					}
					else
					{
						/* always soft if french ending */
						if (StringAt
							(original, (current + 1), 4, "IER ", ""))
						{
							MetaphAdd(primary, "J");
							MetaphAdd(secondary, "J");
						}
						else
						{
							MetaphAdd(primary, "J");
							MetaphAdd(secondary, "K");
						}
					}
					current += 2;
					break;
				}

				if (GetAt(original, current + 1) == 'G')
					current += 2;
				else
					current += 1;
				MetaphAdd(primary, "K");
				MetaphAdd(secondary, "K");
				break;

			case 'H':
				/* only keep if first & before vowel or btw. 2 vowels */
				if (((current == 0) || IsVowel(original, current - 1))
					&& IsVowel(original, current + 1))
				{
					MetaphAdd(primary, "H");
					MetaphAdd(secondary, "H");
					current += 2;
				}
				else
					/* also takes care of 'HH' */
					current += 1;
				break;

			case 'J':
				/* obvious spanish, 'jose', 'san jacinto' */
				if (StringAt(original, current, 4, "JOSE", "")
					|| StringAt(original, 0, 4, "SAN ", ""))
				{
					if (((current == 0)
						 && (GetAt(original, current + 4) == ' '))
						|| StringAt(original, 0, 4, "SAN ", ""))
					{
						MetaphAdd(primary, "H");
						MetaphAdd(secondary, "H");
					}
					else
					{
						MetaphAdd(primary, "J");
						MetaphAdd(secondary, "H");
					}
					current += 1;
					break;
				}

				if ((current == 0)
					&& !StringAt(original, current, 4, "JOSE", ""))
				{
					MetaphAdd(primary, "J");	/* Yankelovich/Jankelowicz */
					MetaphAdd(secondary, "A");
				}
				else
				{
					/* spanish pron. of e.g. 'bajador' */
					if (IsVowel(original, current - 1)
						&& !SlavoGermanic(original)
						&& ((GetAt(original, current + 1) == 'A')
							|| (GetAt(original, current + 1) == 'O')))
					{
						MetaphAdd(primary, "J");
						MetaphAdd(secondary, "H");
					}
					else
					{
						if (current == last)
						{
							MetaphAdd(primary, "J");
							MetaphAdd(secondary, "");
						}
						else
						{
							if (!StringAt(original, (current + 1), 1, "L", "T",
										  "K", "S", "N", "M", "B", "Z", "")
								&& !StringAt(original, (current - 1), 1,
											 "S", "K", "L", ""))
							{
								MetaphAdd(primary, "J");
								MetaphAdd(secondary, "J");
							}
						}
					}
				}

				if (GetAt(original, current + 1) == 'J')	/* it could happen! */
					current += 2;
				else
					current += 1;
				break;

			case 'K':
				if (GetAt(original, current + 1) == 'K')
					current += 2;
				else
					current += 1;
				MetaphAdd(primary, "K");
				MetaphAdd(secondary, "K");
				break;

			case 'L':
				if (GetAt(original, current + 1) == 'L')
				{
					/* spanish e.g. 'cabrillo', 'gallegos' */
					if (((current == (length - 3))
						 && StringAt(original, (current - 1), 4, "ILLO",
									 "ILLA", "ALLE", ""))
						|| ((StringAt(original, (last - 1), 2, "AS", "OS", "")
							 || StringAt(original, last, 1, "A", "O", ""))
							&& StringAt(original, (current - 1), 4,
										"ALLE", "")))
					{
						MetaphAdd(primary, "L");
						MetaphAdd(secondary, "");
						current += 2;
						break;
					}
					current += 2;
				}
				else
					current += 1;
				MetaphAdd(primary, "L");
				MetaphAdd(secondary, "L");
				break;

			case 'M':
				if ((StringAt(original, (current - 1), 3, "UMB", "")
					 && (((current + 1) == last)
						 || StringAt(original, (current + 2), 2, "ER", "")))
				/* 'dumb','thumb' */
					|| (GetAt(original, current + 1) == 'M'))
					current += 2;
				else
					current += 1;
				MetaphAdd(primary, "M");
				MetaphAdd(secondary, "M");
				break;

			case 'N':
				if (GetAt(original, current + 1) == 'N')
					current += 2;
				else
					current += 1;
				MetaphAdd(primary, "N");
				MetaphAdd(secondary, "N");
				break;

			case '\xd1':		/* N with tilde */
				current += 1;
				MetaphAdd(primary, "N");
				MetaphAdd(secondary, "N");
				break;

			case 'P':
				if (GetAt(original, current + 1) == 'H')
				{
					MetaphAdd(primary, "F");
					MetaphAdd(secondary, "F");
					current += 2;
					break;
				}

				/* also account for "campbell", "raspberry" */
				if (StringAt(original, (current + 1), 1, "P", "B", ""))
					current += 2;
				else
					current += 1;
				MetaphAdd(primary, "P");
				MetaphAdd(secondary, "P");
				break;

			case 'Q':
				if (GetAt(original, current + 1) == 'Q')
					current += 2;
				else
					current += 1;
				MetaphAdd(primary, "K");
				MetaphAdd(secondary, "K");
				break;

			case 'R':
				/* french e.g. 'rogier', but exclude 'hochmeier' */
				if ((current == last)
					&& !SlavoGermanic(original)
					&& StringAt(original, (current - 2), 2, "IE", "")
					&& !StringAt(original, (current - 4), 2, "ME", "MA", ""))
				{
					MetaphAdd(primary, "");
					MetaphAdd(secondary, "R");
				}
				else
				{
					MetaphAdd(primary, "R");
					MetaphAdd(secondary, "R");
				}

				if (GetAt(original, current + 1) == 'R')
					current += 2;
				else
					current += 1;
				break;

			case 'S':
				/* special cases 'island', 'isle', 'carlisle', 'carlysle' */
				if (StringAt(original, (current - 1), 3, "ISL", "YSL", ""))
				{
					current += 1;
					break;
				}

				/* special case 'sugar-' */
				if ((current == 0)
					&& StringAt(original, current, 5, "SUGAR", ""))
				{
					MetaphAdd(primary, "X");
					MetaphAdd(secondary, "S");
					current += 1;
					break;
				}

				if (StringAt(original, current, 2, "SH", ""))
				{
					/* germanic */
					if (StringAt
						(original, (current + 1), 4, "HEIM", "HOEK", "HOLM",
						 "HOLZ", ""))
					{
						MetaphAdd(primary, "S");
						MetaphAdd(secondary, "S");
					}
					else
					{
						MetaphAdd(primary, "X");
						MetaphAdd(secondary, "X");
					}
					current += 2;
					break;
				}

				/* italian & armenian */
				if (StringAt(original, current, 3, "SIO", "SIA", "")
					|| StringAt(original, current, 4, "SIAN", ""))
				{
					if (!SlavoGermanic(original))
					{
						MetaphAdd(primary, "S");
						MetaphAdd(secondary, "X");
					}
					else
					{
						MetaphAdd(primary, "S");
						MetaphAdd(secondary, "S");
					}
					current += 3;
					break;
				}

				/*
				 * german & anglicisations, e.g. 'smith' match 'schmidt',
				 * 'snider' match 'schneider' also, -sz- in slavic language
				 * although in hungarian it is pronounced 's'
				 */
				if (((current == 0)
					 && StringAt(original, (current + 1), 1,
								 "M", "N", "L", "W", ""))
					|| StringAt(original, (current + 1), 1, "Z", ""))
				{
					MetaphAdd(primary, "S");
					MetaphAdd(secondary, "X");
					if (StringAt(original, (current + 1), 1, "Z", ""))
						current += 2;
					else
						current += 1;
					break;
				}

				if (StringAt(original, current, 2, "SC", ""))
				{
					/* Schlesinger's rule */
					if (GetAt(original, current + 2) == 'H')
					{
						/* dutch origin, e.g. 'school', 'schooner' */
						if (StringAt(original, (current + 3), 2,
									 "OO", "ER", "EN",
									 "UY", "ED", "EM", ""))
						{
							/* 'schermerhorn', 'schenker' */
							if (StringAt(original, (current + 3), 2,
										 "ER", "EN", ""))
							{
								MetaphAdd(primary, "X");
								MetaphAdd(secondary, "SK");
							}
							else
							{
								MetaphAdd(primary, "SK");
								MetaphAdd(secondary, "SK");
							}
							current += 3;
							break;
						}
						else
						{
							if ((current == 0) && !IsVowel(original, 3)
								&& (GetAt(original, 3) != 'W'))
							{
								MetaphAdd(primary, "X");
								MetaphAdd(secondary, "S");
							}
							else
							{
								MetaphAdd(primary, "X");
								MetaphAdd(secondary, "X");
							}
							current += 3;
							break;
						}
					}

					if (StringAt(original, (current + 2), 1,
								 "I", "E", "Y", ""))
					{
						MetaphAdd(primary, "S");
						MetaphAdd(secondary, "S");
						current += 3;
						break;
					}
					/* else */
					MetaphAdd(primary, "SK");
					MetaphAdd(secondary, "SK");
					current += 3;
					break;
				}

				/* french e.g. 'resnais', 'artois' */
				if ((current == last)
					&& StringAt(original, (current - 2), 2, "AI", "OI", ""))
				{
					MetaphAdd(primary, "");
					MetaphAdd(secondary, "S");
				}
				else
				{
					MetaphAdd(primary, "S");
					MetaphAdd(secondary, "S");
				}

				if (StringAt(original, (current + 1), 1, "S", "Z", ""))
					current += 2;
				else
					current += 1;
				break;

			case 'T':
				if (StringAt(original, current, 4, "TION", ""))
				{
					MetaphAdd(primary, "X");
					MetaphAdd(secondary, "X");
					current += 3;
					break;
				}

				if (StringAt(original, current, 3, "TIA", "TCH", ""))
				{
					MetaphAdd(primary, "X");
					MetaphAdd(secondary, "X");
					current += 3;
					break;
				}

				if (StringAt(original, current, 2, "TH", "")
					|| StringAt(original, current, 3, "TTH", ""))
				{
					/* special case 'thomas', 'thames' or germanic */
					if (StringAt(original, (current + 2), 2, "OM", "AM", "")
						|| StringAt(original, 0, 4, "VAN ", "VON ", "")
						|| StringAt(original, 0, 3, "SCH", ""))
					{
						MetaphAdd(primary, "T");
						MetaphAdd(secondary, "T");
					}
					else
					{
						MetaphAdd(primary, "0");
						MetaphAdd(secondary, "T");
					}
					current += 2;
					break;
				}

				if (StringAt(original, (current + 1), 1, "T", "D", ""))
					current += 2;
				else
					current += 1;
				MetaphAdd(primary, "T");
				MetaphAdd(secondary, "T");
				break;

			case 'V':
				if (GetAt(original, current + 1) == 'V')
					current += 2;
				else
					current += 1;
				MetaphAdd(primary, "F");
				MetaphAdd(secondary, "F");
				break;

			case 'W':
				/* can also be in middle of word */
				if (StringAt(original, current, 2, "WR", ""))
				{
					MetaphAdd(primary, "R");
					MetaphAdd(secondary, "R");
					current += 2;
					break;
				}

				if ((current == 0)
					&& (IsVowel(original, current + 1)
						|| StringAt(original, current, 2, "WH", "")))
				{
					/* Wasserman should match Vasserman */
					if (IsVowel(original, current + 1))
					{
						MetaphAdd(primary, "A");
						MetaphAdd(secondary, "F");
					}
					else
					{
						/* need Uomo to match Womo */
						MetaphAdd(primary, "A");
						MetaphAdd(secondary, "A");
					}
				}

				/* Arnow should match Arnoff */
				if (((current == last) && IsVowel(original, current - 1))
					|| StringAt(original, (current - 1), 5, "EWSKI", "EWSKY",
								"OWSKI", "OWSKY", "")
					|| StringAt(original, 0, 3, "SCH", ""))
				{
					MetaphAdd(primary, "");
					MetaphAdd(secondary, "F");
					current += 1;
					break;
				}

				/* polish e.g. 'filipowicz' */
				if (StringAt(original, current, 4, "WICZ", "WITZ", ""))
				{
					MetaphAdd(primary, "TS");
					MetaphAdd(secondary, "FX");
					current += 4;
					break;
				}

				/* else skip it */
				current += 1;
				break;

			case 'X':
				/* french e.g. breaux */
				if (!((current == last)
					  && (StringAt(original, (current - 3), 3,
								   "IAU", "EAU", "")
						  || StringAt(original, (current - 2), 2,
									  "AU", "OU", ""))))
				{
					MetaphAdd(primary, "KS");
					MetaphAdd(secondary, "KS");
				}


				if (StringAt(original, (current + 1), 1, "C", "X", ""))
					current += 2;
				else
					current += 1;
				break;

			case 'Z':
				/* chinese pinyin e.g. 'zhao' */
				if (GetAt(original, current + 1) == 'H')
				{
					MetaphAdd(primary, "J");
					MetaphAdd(secondary, "J");
					current += 2;
					break;
				}
				else if (StringAt(original, (current + 1), 2,
								  "ZO", "ZI", "ZA", "")
						 || (SlavoGermanic(original)
							 && ((current > 0)
								 && GetAt(original, current - 1) != 'T')))
				{
					MetaphAdd(primary, "S");
					MetaphAdd(secondary, "TS");
				}
				else
				{
					MetaphAdd(primary, "S");
					MetaphAdd(secondary, "S");
				}

				if (GetAt(original, current + 1) == 'Z')
					current += 2;
				else
					current += 1;
				break;

			default:
				current += 1;
		}

		/*
		 * printf("PRIMARY: %s\n", primary->str); printf("SECONDARY: %s\n",
		 * secondary->str);
		 */
	}


	if (primary->length > 4)
		SetAt(primary, 4, '\0');

	if (secondary->length > 4)
		SetAt(secondary, 4, '\0');

	*codes = primary->str;
	*++codes = secondary->str;

	DestroyMetaString(original);
	DestroyMetaString(primary);
	DestroyMetaString(secondary);
}

#ifdef DMETAPHONE_MAIN

/* just for testing - not part of the perl code */

main(int argc, char **argv)
{
	char	   *codes[2];

	if (argc > 1)
	{
		DoubleMetaphone(argv[1], codes);
		printf("%s|%s\n", codes[0], codes[1]);
	}
}

#endif
