/* File:			tuple.h
 *
 * Description:		See "tuple.c"
 *
 * Important NOTE:	The TupleField structure is used both to hold backend data and
 *					manual result set data.  The "set_" functions and the TupleNode
 *					structure are only used for manual result sets by info routines.
 *
 * Comments:		See "notice.txt" for copyright and license information.
 *
 */

#ifndef __TUPLE_H__
#define __TUPLE_H__

#include "psqlodbc.h"

/*	Used by backend data AND manual result sets */
struct TupleField_
{
	Int4		len;			/* length of the current Tuple */
	void	   *value;			/* an array representing the value */
};

/*	Used ONLY for manual result sets */
struct TupleNode_
{
	struct TupleNode_ *prev,
			   *next;
	TupleField	tuple[1];
};

/*	keyset(TID + OID) info */
struct KeySet_
{
	UWORD	status;
	UWORD	offset;
	UDWORD	blocknum;
	UDWORD	oid;
};
#define	KEYSET_INFO_PUBLIC	0x0f
#define	DRV_SELF_ADDED		(1L << 4)
#define	DRV_SELF_DELETED	(1L << 5)
#define	DRV_SELF_UPDATED	(1L << 6)

/*	These macros are wrappers for the corresponding set_tuplefield functions
	but these handle automatic NULL determination and call set_tuplefield_null()
	if appropriate for the datatype (used by SQLGetTypeInfo).
*/
#define set_nullfield_string(FLD, VAL)		((VAL) ? set_tuplefield_string(FLD, (VAL)) : set_tuplefield_null(FLD))
#define set_nullfield_int2(FLD, VAL)		((VAL) != -1 ? set_tuplefield_int2(FLD, (VAL)) : set_tuplefield_null(FLD))
#define set_nullfield_int4(FLD, VAL)		((VAL) != -1 ? set_tuplefield_int4(FLD, (VAL)) : set_tuplefield_null(FLD))

void		set_tuplefield_null(TupleField *tuple_field);
void		set_tuplefield_string(TupleField *tuple_field, char *string);
void		set_tuplefield_int2(TupleField *tuple_field, Int2 value);
void		set_tuplefield_int4(TupleField *tuple_field, Int4 value);

#endif
