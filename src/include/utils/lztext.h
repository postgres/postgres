/* ----------
 * lztext.h
 *
 * $Header: /cvsroot/pgsql/src/include/utils/Attic/lztext.h,v 1.3 2000/04/12 17:16:55 momjian Exp $
 *
 *	Definitions for the lztext compressed data type
 * ----------
 */

#ifndef _LZTEXT_H_
#define _LZTEXT_H_

#include "utils/pg_lzcompress.h"


/* ----------
 * The internal storage format of an LZ compressed text field
 * ----------
 */
typedef PGLZ_Header lztext;

#endif	 /* _LZTEXT_H_ */
