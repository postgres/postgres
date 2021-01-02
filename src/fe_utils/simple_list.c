/*-------------------------------------------------------------------------
 *
 * Simple list facilities for frontend code
 *
 * Data structures for simple lists of OIDs and strings.  The support for
 * these is very primitive compared to the backend's List facilities, but
 * it's all we need in, eg, pg_dump.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/fe_utils/simple_list.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "fe_utils/simple_list.h"


/*
 * Append an OID to the list.
 */
void
simple_oid_list_append(SimpleOidList *list, Oid val)
{
	SimpleOidListCell *cell;

	cell = (SimpleOidListCell *) pg_malloc(sizeof(SimpleOidListCell));
	cell->next = NULL;
	cell->val = val;

	if (list->tail)
		list->tail->next = cell;
	else
		list->head = cell;
	list->tail = cell;
}

/*
 * Is OID present in the list?
 */
bool
simple_oid_list_member(SimpleOidList *list, Oid val)
{
	SimpleOidListCell *cell;

	for (cell = list->head; cell; cell = cell->next)
	{
		if (cell->val == val)
			return true;
	}
	return false;
}

/*
 * Append a string to the list.
 *
 * The given string is copied, so it need not survive past the call.
 */
void
simple_string_list_append(SimpleStringList *list, const char *val)
{
	SimpleStringListCell *cell;

	cell = (SimpleStringListCell *)
		pg_malloc(offsetof(SimpleStringListCell, val) + strlen(val) + 1);

	cell->next = NULL;
	cell->touched = false;
	strcpy(cell->val, val);

	if (list->tail)
		list->tail->next = cell;
	else
		list->head = cell;
	list->tail = cell;
}

/*
 * Is string present in the list?
 *
 * If found, the "touched" field of the first match is set true.
 */
bool
simple_string_list_member(SimpleStringList *list, const char *val)
{
	SimpleStringListCell *cell;

	for (cell = list->head; cell; cell = cell->next)
	{
		if (strcmp(cell->val, val) == 0)
		{
			cell->touched = true;
			return true;
		}
	}
	return false;
}

/*
 * Destroy an OID list
 */
void
simple_oid_list_destroy(SimpleOidList *list)
{
	SimpleOidListCell *cell;

	cell = list->head;
	while (cell != NULL)
	{
		SimpleOidListCell *next;

		next = cell->next;
		pg_free(cell);
		cell = next;
	}
}

/*
 * Destroy a string list
 */
void
simple_string_list_destroy(SimpleStringList *list)
{
	SimpleStringListCell *cell;

	cell = list->head;
	while (cell != NULL)
	{
		SimpleStringListCell *next;

		next = cell->next;
		pg_free(cell);
		cell = next;
	}
}

/*
 * Find first not-touched list entry, if there is one.
 */
const char *
simple_string_list_not_touched(SimpleStringList *list)
{
	SimpleStringListCell *cell;

	for (cell = list->head; cell; cell = cell->next)
	{
		if (!cell->touched)
			return cell->val;
	}
	return NULL;
}

/*
 * Append a pointer to the list.
 *
 * Caller must ensure that the pointer remains valid.
 */
void
simple_ptr_list_append(SimplePtrList *list, void *ptr)
{
	SimplePtrListCell *cell;

	cell = (SimplePtrListCell *) pg_malloc(sizeof(SimplePtrListCell));
	cell->next = NULL;
	cell->ptr = ptr;

	if (list->tail)
		list->tail->next = cell;
	else
		list->head = cell;
	list->tail = cell;
}
