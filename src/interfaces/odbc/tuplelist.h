
/* File:            tuplelist.h
 *
 * Description:     See "tuplelist.c"
 *
 * Important Note:  This structure and its functions are ONLY used in building manual result
 *                  sets for info functions (SQLTables, SQLColumns, etc.)
 *
 * Comments:        See "notice.txt" for copyright and license information.
 *
 */

#ifndef __TUPLELIST_H__
#define __TUPLELIST_H__

#include "psqlodbc.h"

struct TupleListClass_ {
	Int4 num_fields;
	Int4 num_tuples;  
	TupleNode *list_start, *list_end, *lastref;
	Int4 last_indexed;
};

#define TL_get_num_tuples(x)	(x->num_tuples)

/* Create a TupleList. Each tuple consits of fieldcnt columns */
TupleListClass *TL_Constructor(UInt4 fieldcnt);
void TL_Destructor(TupleListClass *self);  
void *TL_get_fieldval(TupleListClass *self, Int4 tupleno, Int2 fieldno);
char TL_add_tuple(TupleListClass *self, TupleNode *new_field);

#endif
