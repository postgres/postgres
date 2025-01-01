/*-------------------------------------------------------------------------
 *
 * unicode_category.h
 *	  Routines for determining the category of Unicode characters.
 *
 * These definitions can be used by both frontend and backend code.
 *
 * Copyright (c) 2017-2025, PostgreSQL Global Development Group
 *
 * src/include/common/unicode_category.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef UNICODE_CATEGORY_H
#define UNICODE_CATEGORY_H

#include "mb/pg_wchar.h"

/*
 * Unicode General Category Values
 *
 * See: https://www.unicode.org/reports/tr44/#General_Category_Values
 *
 * The Unicode stability policy guarantees: "The enumeration of
 * General_Category property values is fixed. No new values will be
 * added". See: https://www.unicode.org/policies/stability_policy.html
 *
 * Numeric values chosen to match corresponding ICU UCharCategory.
 */
typedef enum pg_unicode_category
{
	PG_U_UNASSIGNED = 0,		/* Cn */
	PG_U_UPPERCASE_LETTER = 1,	/* Lu */
	PG_U_LOWERCASE_LETTER = 2,	/* Ll */
	PG_U_TITLECASE_LETTER = 3,	/* Lt */
	PG_U_MODIFIER_LETTER = 4,	/* Lm */
	PG_U_OTHER_LETTER = 5,		/* Lo */
	PG_U_NONSPACING_MARK = 6,	/* Mn */
	PG_U_ENCLOSING_MARK = 7,	/* Me */
	PG_U_SPACING_MARK = 8,		/* Mc */
	PG_U_DECIMAL_NUMBER = 9,	/* Nd */
	PG_U_LETTER_NUMBER = 10,	/* Nl */
	PG_U_OTHER_NUMBER = 11,		/* No */
	PG_U_SPACE_SEPARATOR = 12,	/* Zs */
	PG_U_LINE_SEPARATOR = 13,	/* Zl */
	PG_U_PARAGRAPH_SEPARATOR = 14,	/* Zp */
	PG_U_CONTROL = 15,			/* Cc */
	PG_U_FORMAT = 16,			/* Cf */
	PG_U_PRIVATE_USE = 17,		/* Co */
	PG_U_SURROGATE = 18,		/* Cs */
	PG_U_DASH_PUNCTUATION = 19, /* Pd */
	PG_U_OPEN_PUNCTUATION = 20, /* Ps */
	PG_U_CLOSE_PUNCTUATION = 21,	/* Pe */
	PG_U_CONNECTOR_PUNCTUATION = 22,	/* Pc */
	PG_U_OTHER_PUNCTUATION = 23,	/* Po */
	PG_U_MATH_SYMBOL = 24,		/* Sm */
	PG_U_CURRENCY_SYMBOL = 25,	/* Sc */
	PG_U_MODIFIER_SYMBOL = 26,	/* Sk */
	PG_U_OTHER_SYMBOL = 27,		/* So */
	PG_U_INITIAL_PUNCTUATION = 28,	/* Pi */
	PG_U_FINAL_PUNCTUATION = 29 /* Pf */
} pg_unicode_category;

extern pg_unicode_category unicode_category(pg_wchar code);
extern const char *unicode_category_string(pg_unicode_category category);
extern const char *unicode_category_abbrev(pg_unicode_category category);

extern bool pg_u_prop_alphabetic(pg_wchar code);
extern bool pg_u_prop_lowercase(pg_wchar code);
extern bool pg_u_prop_uppercase(pg_wchar code);
extern bool pg_u_prop_cased(pg_wchar code);
extern bool pg_u_prop_case_ignorable(pg_wchar code);
extern bool pg_u_prop_white_space(pg_wchar code);
extern bool pg_u_prop_hex_digit(pg_wchar code);
extern bool pg_u_prop_join_control(pg_wchar code);

extern bool pg_u_isdigit(pg_wchar code, bool posix);
extern bool pg_u_isalpha(pg_wchar code);
extern bool pg_u_isalnum(pg_wchar code, bool posix);
extern bool pg_u_isword(pg_wchar code);
extern bool pg_u_isupper(pg_wchar code);
extern bool pg_u_islower(pg_wchar code);
extern bool pg_u_isblank(pg_wchar code);
extern bool pg_u_iscntrl(pg_wchar code);
extern bool pg_u_isgraph(pg_wchar code);
extern bool pg_u_isprint(pg_wchar code);
extern bool pg_u_ispunct(pg_wchar code, bool posix);
extern bool pg_u_isspace(pg_wchar code);
extern bool pg_u_isxdigit(pg_wchar code, bool posix);

#endif							/* UNICODE_CATEGORY_H */
