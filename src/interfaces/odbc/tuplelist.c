/* Module:			tuplelist.c
 *
 * Description:		This module contains functions for creating a manual result set
 *					(the TupleList) and retrieving data from it for a specific row/column.
 *
 * Classes:			TupleListClass (Functions prefix: "TL_")
 *
 * API functions:	none
 *
 * Comments:		See "notice.txt" for copyright and license information.
 *
 */

#include <stdlib.h>
#include "tuplelist.h"
#include "tuple.h"

TupleListClass *
TL_Constructor(UInt4 fieldcnt)
{
	TupleListClass *rv;

	mylog("in TL_Constructor\n");

	rv = (TupleListClass *) malloc(sizeof(TupleListClass));
	if (rv)
	{

		rv->num_fields = fieldcnt;
		rv->num_tuples = 0;
		rv->list_start = NULL;
		rv->list_end = NULL;
		rv->lastref = NULL;
		rv->last_indexed = -1;
	}

	mylog("exit TL_Constructor\n");

	return rv;
}

void
TL_Destructor(TupleListClass * self)
{
	int			lf;
	TupleNode  *node,
			   *tp;

	mylog("TupleList: in DESTRUCTOR\n");

	node = self->list_start;
	while (node != NULL)
	{
		for (lf = 0; lf < self->num_fields; lf++)
			if (node->tuple[lf].value != NULL)
				free(node->tuple[lf].value);
		tp = node->next;
		free(node);
		node = tp;
	}

	free(self);

	mylog("TupleList: exit DESTRUCTOR\n");
}


void *
TL_get_fieldval(TupleListClass * self, Int4 tupleno, Int2 fieldno)
{
	Int4		lf;
	Int4		delta,
				from_end;
	char		end_is_closer,
				start_is_closer;
	TupleNode  *rv;

	if (self->last_indexed == -1)
		/* we have an empty tuple list */
		return NULL;

	/* some more sanity checks */
	if ((tupleno >= self->num_tuples) || (tupleno < 0))
		/* illegal tuple number range */
		return NULL;

	if ((fieldno >= self->num_fields) || (fieldno < 0))
		/* illegel field number range */
		return NULL;

	/*
	 * check if we are accessing the same tuple that was used in the last
	 * fetch (e.g: for fetching all the fields one after another. Do this
	 * to speed things up
	 */
	if (tupleno == self->last_indexed)
		return self->lastref->tuple[fieldno].value;

	/* now for the tricky part... */

	/*
	 * Since random access is quite inefficient for linked lists we use
	 * the lastref pointer that points to the last element referenced by a
	 * get_fieldval() call in conjunction with the its index number that
	 * is stored in last_indexed. (So we use some locality of reference
	 * principle to speed things up)
	 */

	delta = tupleno - self->last_indexed;
	/* if delta is positive, we have to go forward */

	/*
	 * now check if we are closer to the start or the end of the list than
	 * to our last_indexed pointer
	 */
	from_end = (self->num_tuples - 1) - tupleno;

	start_is_closer = labs(delta) > tupleno;

	/*
	 * true if we are closer to the start of the list than to the
	 * last_indexed pointer
	 */

	end_is_closer = labs(delta) > from_end;
	/* true if we are closer at the end of the list */

	if (end_is_closer)
	{
		/* scanning from the end is the shortest way. so we do that... */
		rv = self->list_end;
		for (lf = 0; lf < from_end; lf++)
			rv = rv->prev;
	}
	else if (start_is_closer)
	{

		/*
		 * the shortest way is to start the search from the head of the
		 * list
		 */
		rv = self->list_start;
		for (lf = 0; lf < tupleno; lf++)
			rv = rv->next;
	}
	else
	{
		/* the closest way is starting from our lastref - pointer */
		rv = self->lastref;

		/*
		 * at first determine whether we have to search forward or
		 * backwards
		 */
		if (delta < 0)
		{
			/* we have to search backwards */
			for (lf = 0; lf < (-1) * delta; lf++)
				rv = rv->prev;
		}
		else
		{
			/* ok, we have to search forward... */
			for (lf = 0; lf < delta; lf++)
				rv = rv->next;
		}
	}

	/*
	 * now we have got our return pointer, so update the lastref and the
	 * last_indexed values
	 */
	self->lastref = rv;
	self->last_indexed = tupleno;

	return rv->tuple[fieldno].value;
}



char
TL_add_tuple(TupleListClass * self, TupleNode * new_field)
{

	/*
	 * we append the tuple at the end of the doubly linked list of the
	 * tuples we have already read in
	 */

	new_field->prev = NULL;
	new_field->next = NULL;

	if (self->list_start == NULL)
	{
		/* the list is empty, we have to add the first tuple */
		self->list_start = new_field;
		self->list_end = new_field;
		self->lastref = new_field;
		self->last_indexed = 0;
	}
	else
	{

		/*
		 * there is already an element in the list, so add the new one at
		 * the end of the list
		 */
		self->list_end->next = new_field;
		new_field->prev = self->list_end;
		self->list_end = new_field;
	}
	self->num_tuples++;

	/* this method of building a list cannot fail, so we return 1 */
	return 1;
}
