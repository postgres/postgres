/* 
 * ISSN.h
 *	  PostgreSQL type definitions for ISNs (ISBN, ISMN, ISSN, EAN13, UPC)
 *
 * No information available for UPC prefixes
 * 
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/contrib/isn/UPC.h,v 1.1 2006/09/09 04:07:52 tgl Exp $
 *
 */

/* where the digit set begins, and how many of them are in the table */
const unsigned UPC_index[10][2] = {
	{0,		0},
	{0,		0},
	{0,		0},
	{0,		0},
	{0,		0},
	{0,		0},
	{0,		0},
	{0,		0},
	{0,		0},
	{0,		0},
};
const char *UPC_range[][2] = {
	{NULL, NULL}
};
