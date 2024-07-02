/*
 * ISSN.h
 *	  PostgreSQL type definitions for ISNs (ISBN, ISMN, ISSN, EAN13, UPC)
 *
 * No information available for UPC prefixes
 *
 *
 * IDENTIFICATION
 *	  contrib/isn/UPC.h
 *
 */

/* where the digit set begins, and how many of them are in the table */
static const unsigned UPC_index[10][2] = {
	{0, 0},
	{0, 0},
	{0, 0},
	{0, 0},
	{0, 0},
	{0, 0},
	{0, 0},
	{0, 0},
	{0, 0},
	{0, 0},
};
static const char *UPC_range[][2] = {
	{NULL, NULL}
};
