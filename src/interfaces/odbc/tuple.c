
/* Module:			tuple.c
 *
 * Description:		This module contains functions for setting the data for individual
 *					fields (TupleField structure) of a manual result set.
 *
 * Important Note:	These functions are ONLY used in building manual result sets for
 *					info functions (SQLTables, SQLColumns, etc.)
 *
 * Classes:			n/a
 *
 * API functions:	none
 *
 * Comments:		See "notice.txt" for copyright and license information.
 *
 */

#include "tuple.h"
#include <string.h>
#include <stdlib.h>

void
set_tuplefield_null(TupleField *tuple_field)
{
	tuple_field->len = 0;
	tuple_field->value = NULL;	/* strdup(""); */
}

void
set_tuplefield_string(TupleField *tuple_field, char *string)
{
	tuple_field->len = strlen(string);
	tuple_field->value = malloc(strlen(string) + 1);
	strcpy(tuple_field->value, string);
}


void
set_tuplefield_int2(TupleField *tuple_field, Int2 value)
{
	char		buffer[10];


	sprintf(buffer, "%d", value);

	tuple_field->len = strlen(buffer) + 1;
	/* +1 ... is this correct (better be on the save side-...) */
	tuple_field->value = strdup(buffer);
}

void
set_tuplefield_int4(TupleField *tuple_field, Int4 value)
{
	char		buffer[15];

	sprintf(buffer, "%ld", value);

	tuple_field->len = strlen(buffer) + 1;
	/* +1 ... is this correct (better be on the save side-...) */
	tuple_field->value = strdup(buffer);
}
