/*
 * connstrings.c
 *		connecting string processing functions
 *
 *	Copyright (c) 2012-2015, PostgreSQL Global Development Group
 *
 *	src/include/common/connstrings.c
 */
#include "postgres_fe.h"

#include <string.h>

#include "common/connstrings.h"


/* The connection URI must start with either of the following designators: */
static const char uri_designator[] = "postgresql://";
static const char short_uri_designator[] = "postgres://";


/*
 * Checks if connection string starts with either of the valid URI prefix
 * designators.
 *
 * Returns the URI prefix length, 0 if the string doesn't contain a URI prefix.
 */
int
libpq_connstring_uri_prefix_length(const char *connstr)
{
	if (strncmp(connstr, uri_designator,
				sizeof(uri_designator) - 1) == 0)
		return sizeof(uri_designator) - 1;

	if (strncmp(connstr, short_uri_designator,
				sizeof(short_uri_designator) - 1) == 0)
		return sizeof(short_uri_designator) - 1;

	return 0;
}

/*
 * Recognized connection string either starts with a valid URI prefix or
 * contains a "=" in it.
 *
 * Must be consistent with parse_connection_string: anything for which this
 * returns true should at least look like it's parseable by that routine.
 */
bool
libpq_connstring_is_recognized(const char *connstr)
{
	return libpq_connstring_uri_prefix_length(connstr) != 0 ||
		strchr(connstr, '=') != NULL;
}
