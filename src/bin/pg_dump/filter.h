/*-------------------------------------------------------------------------
 *
 * filter.h
 *		Common header file for the parser of filter file
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/bin/pg_dump/filter.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FILTER_H
#define FILTER_H

#include "lib/stringinfo.h"

/* Function signature for exit_nicely functions */
typedef void (*exit_function) (int status);

/*
 * State data for reading filter items from stream
 */
typedef struct
{
	FILE	   *fp;
	const char *filename;
	exit_function exit_nicely;
	int			lineno;
	StringInfoData linebuff;
} FilterStateData;

/*
 * List of command types that can be specified in filter file
 */
typedef enum
{
	FILTER_COMMAND_TYPE_NONE,
	FILTER_COMMAND_TYPE_INCLUDE,
	FILTER_COMMAND_TYPE_EXCLUDE,
} FilterCommandType;

/*
 * List of objects that can be specified in filter file
 */
typedef enum
{
	FILTER_OBJECT_TYPE_NONE,
	FILTER_OBJECT_TYPE_TABLE_DATA,
	FILTER_OBJECT_TYPE_TABLE_DATA_AND_CHILDREN,
	FILTER_OBJECT_TYPE_DATABASE,
	FILTER_OBJECT_TYPE_EXTENSION,
	FILTER_OBJECT_TYPE_FOREIGN_DATA,
	FILTER_OBJECT_TYPE_FUNCTION,
	FILTER_OBJECT_TYPE_INDEX,
	FILTER_OBJECT_TYPE_SCHEMA,
	FILTER_OBJECT_TYPE_TABLE,
	FILTER_OBJECT_TYPE_TABLE_AND_CHILDREN,
	FILTER_OBJECT_TYPE_TRIGGER,
} FilterObjectType;

extern const char *filter_object_type_name(FilterObjectType fot);
extern void filter_init(FilterStateData *fstate, const char *filename, exit_function f_exit);
extern void filter_free(FilterStateData *fstate);
extern void pg_log_filter_error(FilterStateData *fstate, const char *fmt,...)
			pg_attribute_printf(2, 3);
extern bool filter_read_item(FilterStateData *fstate, char **objname,
							 FilterCommandType *comtype, FilterObjectType *objtype);

#endif							/* FILTER_H */
