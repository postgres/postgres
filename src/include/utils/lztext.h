/* ----------
 * lztext.h
 *
 * $Header: /cvsroot/pgsql/src/include/utils/Attic/lztext.h,v 1.4 2000/07/03 23:10:14 wieck Exp $
 *
 *	Definitions for the lztext compressed data type
 * ----------
 */

#ifndef _LZTEXT_H_
#define _LZTEXT_H_

/* ----------
 * The internal storage format of an LZ compressed text field is varattrib
 * ----------
 */
typedef varattrib lztext;

#endif	 /* _LZTEXT_H_ */
