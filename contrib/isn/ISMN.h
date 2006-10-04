/*
 * ISMN.h
 *	  PostgreSQL type definitions for ISNs (ISBN, ISMN, ISSN, EAN13, UPC)
 *
 * Information recompiled by Kronuz on November 12, 2004
 * http://www.ismn-international.org
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/contrib/isn/ISMN.h,v 1.2 2006/10/04 00:29:45 momjian Exp $
 *
 * M-3452-4680-5 <=> (0)-3452-4680-5 <=> 0345246805 <=> 9790345246805 <=> 979-0-3452-4680-5
 *
 *			   (M counts as 3)
 * ISMN			M	3	4	 5	 2	 4	 6	  8   0
 * Weight		3	1	3	 1	 3	 1	 3	  1   3
 * Product		9 + 3 + 12 + 5 + 6 + 4 + 18 + 8 + 0 = 65
 *				65 / 10 = 6 remainder 5
 * Check digit	10 - 5 = 5
 * => M-3452-4680-5
 *
 * ISMN			9	7	 9	 0	 3	 4	  5   2   4   6    8   0
 * Weight		1	3	 1	 3	 1	 3	  1   3   1   3    1   3
 * Product		9 + 21 + 9 + 0 + 3 + 12 + 5 + 6 + 4 + 18 + 8 + 0 = 95
 *				95 / 10 = 9 remainder 5
 * Check digit	10 - 5 = 5
 * => 979-0-3452-4680-5
 *
 * Since mod10(9*1 + 7*3 + 9*1 + 0*3) = mod10(M*3) = mod10(3*3) = 9; the check digit remains the same.
 *
 */

/* where the digit set begins, and how many of them are in the table */
const unsigned ISMN_index[10][2] = {
	{0, 5},
	{5, 0},
	{5, 0},
	{5, 0},
	{5, 0},
	{5, 0},
	{5, 0},
	{5, 0},
	{5, 0},
	{5, 0},
};
const char *ISMN_range[][2] = {
	{"0-000", "0-099"},
	{"0-1000", "0-3999"},
	{"0-40000", "0-69999"},
	{"0-700000", "0-899999"},
	{"0-9000000", "0-9999999"},
	{NULL, NULL}
};
