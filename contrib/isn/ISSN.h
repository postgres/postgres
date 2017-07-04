/*
 * ISSN.h
 *	  PostgreSQL type definitions for ISNs (ISBN, ISMN, ISSN, EAN13, UPC)
 *
 * Information recompiled by Kronuz on November 12, 2004
 * http://www.issn.org/
 *
 * IDENTIFICATION
 *	  contrib/isn/ISSN.h
 *
 * 1144-875X <=> 1144875(X) <=> 1144875 <=> (977)1144875 <=> 9771144875(00) <=> 977114487500(7) <=> 977-1144-875-00-7
 *
 *
 * ISSN			1	1	4	 4	  8    7	5
 * Weight		8	7	6	 5	  4    3	2
 * Product		8 + 7 + 24 + 20 + 32 + 21 + 10 = 122
 *				122 / 11 = 11 remainder 1
 * Check digit	11 - 1 = 10 = X
 * => 1144-875X
 *
 * ISSN			9	7	 7	 1	 1	 4	  4   8    7   5	0	0
 * Weight		1	3	 1	 3	 1	 3	  1   3    1   3	1	3
 * Product		9 + 21 + 7 + 3 + 1 + 12 + 4 + 24 + 7 + 15 + 0 + 0 = 103
 *				103 / 10 = 10 remainder 3
 * Check digit	10 - 3 = 7
 * => 977-1144875-00-7 ??  <- supplemental number (number of the week, month, etc.)
 *				  ^^ 00 for non-daily publications (01=Monday, 02=Tuesday, ...)
 *
 * The hyphenation is always in after the four digits of the ISSN code.
 *
 */

/* where the digit set begins, and how many of them are in the table */
const unsigned ISSN_index[10][2] = {
	{0, 1},
	{0, 1},
	{0, 1},
	{0, 1},
	{0, 1},
	{0, 1},
	{0, 1},
	{0, 1},
	{0, 1},
	{0, 1},
};
const char *ISSN_range[][2] = {
	{"0000-000", "9999-999"},
	{NULL, NULL}
};
