/* ----------
 * lztext.h
 *
 * $Header: /cvsroot/pgsql/src/include/utils/Attic/lztext.h,v 1.2 1999/12/28 13:40:53 wieck Exp $
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
typedef PGLZ_Header		lztext;

#endif /* _LZTEXT_H_ */
