/*-------------------------------------------------------------------------
 * unicode_category.c
 *		Determine general category of Unicode characters.
 *
 * Portions Copyright (c) 2017-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/common/unicode_category.c
 *
 *-------------------------------------------------------------------------
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "common/unicode_category.h"
#include "common/unicode_category_table.h"

/*
 * Unicode general category for the given codepoint.
 */
pg_unicode_category
unicode_category(pg_wchar ucs)
{
	int			min = 0;
	int			mid;
	int			max = lengthof(unicode_categories) - 1;

	Assert(ucs <= 0x10ffff);

	while (max >= min)
	{
		mid = (min + max) / 2;
		if (ucs > unicode_categories[mid].last)
			min = mid + 1;
		else if (ucs < unicode_categories[mid].first)
			max = mid - 1;
		else
			return unicode_categories[mid].category;
	}

	return PG_U_UNASSIGNED;
}

/*
 * Description of Unicode general category.
 */
const char *
unicode_category_string(pg_unicode_category category)
{
	switch (category)
	{
		case PG_U_UNASSIGNED:
			return "Unassigned";
		case PG_U_UPPERCASE_LETTER:
			return "Uppercase_Letter";
		case PG_U_LOWERCASE_LETTER:
			return "Lowercase_Letter";
		case PG_U_TITLECASE_LETTER:
			return "Titlecase_Letter";
		case PG_U_MODIFIER_LETTER:
			return "Modifier_Letter";
		case PG_U_OTHER_LETTER:
			return "Other_Letter";
		case PG_U_NONSPACING_MARK:
			return "Nonspacing_Mark";
		case PG_U_ENCLOSING_MARK:
			return "Enclosing_Mark";
		case PG_U_SPACING_MARK:
			return "Spacing_Mark";
		case PG_U_DECIMAL_NUMBER:
			return "Decimal_Number";
		case PG_U_LETTER_NUMBER:
			return "Letter_Number";
		case PG_U_OTHER_NUMBER:
			return "Other_Number";
		case PG_U_SPACE_SEPARATOR:
			return "Space_Separator";
		case PG_U_LINE_SEPARATOR:
			return "Line_Separator";
		case PG_U_PARAGRAPH_SEPARATOR:
			return "Paragraph_Separator";
		case PG_U_CONTROL:
			return "Control";
		case PG_U_FORMAT:
			return "Format";
		case PG_U_PRIVATE_USE:
			return "Private_Use";
		case PG_U_SURROGATE:
			return "Surrogate";
		case PG_U_DASH_PUNCTUATION:
			return "Dash_Punctuation";
		case PG_U_OPEN_PUNCTUATION:
			return "Open_Punctuation";
		case PG_U_CLOSE_PUNCTUATION:
			return "Close_Punctuation";
		case PG_U_CONNECTOR_PUNCTUATION:
			return "Connector_Punctuation";
		case PG_U_OTHER_PUNCTUATION:
			return "Other_Punctuation";
		case PG_U_MATH_SYMBOL:
			return "Math_Symbol";
		case PG_U_CURRENCY_SYMBOL:
			return "Currency_Symbol";
		case PG_U_MODIFIER_SYMBOL:
			return "Modifier_Symbol";
		case PG_U_OTHER_SYMBOL:
			return "Other_Symbol";
		case PG_U_INITIAL_PUNCTUATION:
			return "Initial_Punctuation";
		case PG_U_FINAL_PUNCTUATION:
			return "Final_Punctuation";
	}

	Assert(false);
	return "Unrecognized";		/* keep compiler quiet */
}

/*
 * Short code for Unicode general category.
 */
const char *
unicode_category_abbrev(pg_unicode_category category)
{
	switch (category)
	{
		case PG_U_UNASSIGNED:
			return "Cn";
		case PG_U_UPPERCASE_LETTER:
			return "Lu";
		case PG_U_LOWERCASE_LETTER:
			return "Ll";
		case PG_U_TITLECASE_LETTER:
			return "Lt";
		case PG_U_MODIFIER_LETTER:
			return "Lm";
		case PG_U_OTHER_LETTER:
			return "Lo";
		case PG_U_NONSPACING_MARK:
			return "Mn";
		case PG_U_ENCLOSING_MARK:
			return "Me";
		case PG_U_SPACING_MARK:
			return "Mc";
		case PG_U_DECIMAL_NUMBER:
			return "Nd";
		case PG_U_LETTER_NUMBER:
			return "Nl";
		case PG_U_OTHER_NUMBER:
			return "No";
		case PG_U_SPACE_SEPARATOR:
			return "Zs";
		case PG_U_LINE_SEPARATOR:
			return "Zl";
		case PG_U_PARAGRAPH_SEPARATOR:
			return "Zp";
		case PG_U_CONTROL:
			return "Cc";
		case PG_U_FORMAT:
			return "Cf";
		case PG_U_PRIVATE_USE:
			return "Co";
		case PG_U_SURROGATE:
			return "Cs";
		case PG_U_DASH_PUNCTUATION:
			return "Pd";
		case PG_U_OPEN_PUNCTUATION:
			return "Ps";
		case PG_U_CLOSE_PUNCTUATION:
			return "Pe";
		case PG_U_CONNECTOR_PUNCTUATION:
			return "Pc";
		case PG_U_OTHER_PUNCTUATION:
			return "Po";
		case PG_U_MATH_SYMBOL:
			return "Sm";
		case PG_U_CURRENCY_SYMBOL:
			return "Sc";
		case PG_U_MODIFIER_SYMBOL:
			return "Sk";
		case PG_U_OTHER_SYMBOL:
			return "So";
		case PG_U_INITIAL_PUNCTUATION:
			return "Pi";
		case PG_U_FINAL_PUNCTUATION:
			return "Pf";
	}

	Assert(false);
	return "??";				/* keep compiler quiet */
}
