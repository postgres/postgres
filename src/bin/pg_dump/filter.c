/*-------------------------------------------------------------------------
 *
 * Implementation of simple filter file parser
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pg_dump/filter.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "filter.h"

#include "common/logging.h"
#include "common/fe_memutils.h"
#include "common/string.h"
#include "lib/stringinfo.h"
#include "pqexpbuffer.h"

#define		is_keyword_str(cstr, str, bytes) \
	((strlen(cstr) == bytes) && (pg_strncasecmp(cstr, str, bytes) == 0))

/*
 * Following routines are called from pg_dump, pg_dumpall and pg_restore.
 * Unfortunatelly, implementation of exit_nicely in pg_dump and pg_restore
 * is different from implementation of this rutine in pg_dumpall. So instead
 * direct calling exit_nicely we have to return some error flag (in this
 * case NULL), and exit_nicelly will be executed from caller's routine.
 */

/*
 * Simple routines - just don't repeat same code
 *
 * Returns true, when filter's file is opened
 */
bool
filter_init(FilterStateData *fstate, const char *filename)
{
	fstate->filename = filename;
	fstate->lineno = 0;
	initStringInfo(&fstate->linebuff);

	if (strcmp(filename, "-") != 0)
	{
		fstate->fp = fopen(filename, "r");
		if (!fstate->fp)
		{
			pg_log_error("could not open filter file \"%s\": %m", filename);
			return false;
		}
	}
	else
		fstate->fp = stdin;

	fstate->is_error = false;

	return true;
}

/*
 * Release allocated sources for filter
 */
void
filter_free_sources(FilterStateData *fstate)
{
	free(fstate->linebuff.data);
	fstate->linebuff.data = NULL;

	if (fstate->fp && fstate->fp != stdin)
	{
		if (fclose(fstate->fp) != 0)
			pg_log_error("could not close filter file \"%s\": %m", fstate->filename);

		fstate->fp = NULL;
	}
}

/*
 * log_format_error - Emit error message
 *
 * This is mostly a convenience routine to avoid duplicating file closing code
 * in multiple callsites.
 */
void
log_invalid_filter_format(FilterStateData *fstate, char *message)
{
	if (fstate->fp != stdin)
	{
		pg_log_error("invalid format of filter file \"%s\" on line %d: %s",
					 fstate->filename,
					 fstate->lineno,
					 message);
	}
	else
		pg_log_error("invalid format of filter on line %d: %s",
					 fstate->lineno,
					 message);

	fstate->is_error = true;
}

static const char *
filter_object_type_name(FilterObjectType fot)
{
	switch (fot)
	{
		case FILTER_OBJECT_TYPE_NONE:
			return "comment or empty line";
		case FILTER_OBJECT_TYPE_DATA:
			return "data";
		case FILTER_OBJECT_TYPE_DATABASE:
			return "database";
		case FILTER_OBJECT_TYPE_FOREIGN_DATA:
			return "foreign data";
		case FILTER_OBJECT_TYPE_FUNCTION:
			return "function";
		case FILTER_OBJECT_TYPE_INDEX:
			return "index";
		case FILTER_OBJECT_TYPE_SCHEMA:
			return "schema";
		case FILTER_OBJECT_TYPE_TABLE:
			return "table";
		case FILTER_OBJECT_TYPE_TRIGGER:
			return "trigger";
	}

	return "unknown object type";
}

/*
 * Helper routine to reduce duplicated code
 */
void
log_unsupported_filter_object_type(FilterStateData *fstate,
									const char *appname,
									FilterObjectType fot)
{
	PQExpBuffer str = createPQExpBuffer();

	printfPQExpBuffer(str,
					  "The application \"%s\" doesn't support filter for object type \"%s\".",
					  appname,
					  filter_object_type_name(fot));

	log_invalid_filter_format(fstate, str->data);
}

/*
 * filter_get_keyword - read the next filter keyword from buffer
 *
 * Search for keywords (limited to ascii alphabetic characters) in
 * the passed in line buffer. Returns NULL, when the buffer is empty or first
 * char is not alpha. The length of the found keyword is returned in the size
 * parameter.
 */
static const char *
filter_get_keyword(const char **line, int *size)
{
	const char *ptr = *line;
	const char *result = NULL;

	/* Set returnlength preemptively in case no keyword is found */
	*size = 0;

	/* Skip initial whitespace */
	while (isspace(*ptr))
		ptr++;

	if (isascii(*ptr) && isalpha(*ptr))
	{
		result = ptr++;

		while (isascii(*ptr) && (isalpha(*ptr) || *ptr == '_'))
			ptr++;

		*size = ptr - result;
	}

	*line = ptr;

	return result;
}

/*
 * filter_get_pattern - Read an object identifier pattern from the buffer
 *
 * Parses an object identifier pattern from the passed in buffer and sets
 * objname to a string with object identifier pattern. Returns pointer to the
 * first character after the pattern.
 */
static char *
filter_get_pattern(FilterStateData *fstate,
				   char *str,
				   char **objname)
{
	/* Skip whitespace */
	while (isspace(*str))
		str++;

	if (*str == '\0')
	{
		log_invalid_filter_format(fstate, "missing object name pattern");
		return NULL;
	}

	/*
	 * If the object name pattern has been quoted, we must take care to parse
	 * out the entire quoted pattern, which may contain whitespace and can span
	 * many lines.
	 */
	if (*str == '"')
	{
		PQExpBuffer quoted_name = createPQExpBuffer();

		appendPQExpBufferChar(quoted_name, '"');
		str++;

		while (1)
		{
			if (*str == '\0')
			{
				Assert(fstate->linebuff.data);

				if (!pg_get_line_buf(fstate->fp, &fstate->linebuff))
				{
					if (ferror(fstate->fp))
					{
						pg_log_error("could not read from filter file \"%s\": %m",
									 fstate->filename);
						fstate->is_error = true;
					}
					else
						log_invalid_filter_format(fstate, "unexpected end of file");

					return NULL;
				}

				str = fstate->linebuff.data;
				(void) pg_strip_crlf(str);

				appendPQExpBufferChar(quoted_name, '\n');
				fstate->lineno++;
			}

			if (*str == '"')
			{
				appendPQExpBufferChar(quoted_name, '"');
				str++;

				if (*str == '"')
				{
					appendPQExpBufferChar(quoted_name, '"');
					str++;
				}
				else
					break;
			}
			else if (*str == '\\')
			{
				str++;
				if (*str == 'n')
					appendPQExpBufferChar(quoted_name, '\n');
				else if (*str == '\\')
					appendPQExpBufferChar(quoted_name, '\\');

				str++;
			}
			else
				appendPQExpBufferChar(quoted_name, *str++);
		}

		*objname = pg_strdup(quoted_name->data);
		destroyPQExpBuffer(quoted_name);
	}
	else
	{
		char	   *startptr = str++;

		/* Simple variant, read to EOL or to first whitespace */
		while (*str && !isspace(*str))
			str++;

		*objname = pnstrdup(startptr, str - startptr);
	}

	return str;
}

/*
 * read_filter_item - Read command/type/pattern triplet from filter file
 *
 * This will parse one filter item from the filter file, and while it is a
 * row based format a pattern may span more than one line due to how object
 * names can be constructed.  The expected format of the filter file is:
 *
 * <command> <object_type> <pattern>
 *
 * Where command is "include" or "exclude", and object_type is one of:
 * "table", "schema", "foreign_data", "data", "database", "function",
 * "trigger" or "index". The pattern is either simple without any
 * whitespace, or properly quoted in case there may be whitespace in the
 * object name. The pattern handling follows the same rules as other object
 * include and exclude functions; it can use wildcards. Returns true, when
 * one filter item was successfully read and parsed.  When object name
 * contains \n chars, then more than one line from input file can be
 * processed. Returns false when the filter file reaches EOF. In case of
 * errors, the function wont return but will exit with an appropriate error
 * message.
 */
bool
filter_read_item(FilterStateData *fstate,
				 bool *is_include,
				 char **objname,
				 FilterObjectType *objtype)
{
	Assert(!fstate->is_error);

	if (pg_get_line_buf(fstate->fp, &fstate->linebuff))
	{
		char	   *str = fstate->linebuff.data;
		const char *keyword;
		int			size;

		fstate->lineno++;

		(void) pg_strip_crlf(str);

		/* Skip initial white spaces */
		while (isspace(*str))
			str++;

		/*
		 * Skip empty lines or lines where the first non-whitespace character
		 * is a hash indicating a comment.
		 */
		if (*str != '\0' && *str != '#')
		{
			/*
			 * First we expect sequence of two keywords, {include|exclude}
			 * followed by the object type to operate on.
			 */
			keyword = filter_get_keyword((const char **) &str, &size);
			if (!keyword)
			{
				log_invalid_filter_format(fstate,
										   "no filter command found (expected \"include\" or \"exclude\")");
				return false;
			}

			if (is_keyword_str("include", keyword, size))
				*is_include = true;
			else if (is_keyword_str("exclude", keyword, size))
				*is_include = false;
			else
			{
				log_invalid_filter_format(fstate,
										  "invalid filter command (expected \"include\" or \"exclude\")");
				return false;
			}

			keyword = filter_get_keyword((const char **) &str, &size);
			if (!keyword)
			{
				log_invalid_filter_format(fstate, "missing filter object type");
				return false;
			}

			if (is_keyword_str("data", keyword, size))
				*objtype = FILTER_OBJECT_TYPE_DATA;
			else if (is_keyword_str("database", keyword, size))
				*objtype = FILTER_OBJECT_TYPE_DATABASE;
			else if (is_keyword_str("foreign_data",keyword, size))
				*objtype = FILTER_OBJECT_TYPE_FOREIGN_DATA;
			else if (is_keyword_str("function", keyword, size))
				*objtype = FILTER_OBJECT_TYPE_FUNCTION;
			else if (is_keyword_str("index", keyword, size))
				*objtype = FILTER_OBJECT_TYPE_INDEX;
			else if (is_keyword_str("schema", keyword, size))
				*objtype = FILTER_OBJECT_TYPE_SCHEMA;
			else if (is_keyword_str("table", keyword, size))
				*objtype = FILTER_OBJECT_TYPE_TABLE;
			else if (is_keyword_str("trigger", keyword, size))
				*objtype = FILTER_OBJECT_TYPE_TRIGGER;
			else
			{
				PQExpBuffer str = createPQExpBuffer();

				printfPQExpBuffer(str, "unsupported filter object type: \"%.*s\"", size, keyword);
				log_invalid_filter_format(fstate, str->data);
				return false;
			}

			str = filter_get_pattern(fstate, str, objname);
			if (!str)
				return false;

			/*
			 * Look for any content after the object identifier. Comments and
			 * whitespace are allowed, other content may indicate that the
			 * user needed to quote the object name so exit with an invalid
			 * format error.
			 */
			while (isspace(*str))
				str++;

			if (*str != '\0' && *str != '#')
			{
				log_invalid_filter_format(fstate,
										  "unexpected extra data after pattern");
				return false;
			}
		}
		else
		{
			*objname = NULL;
			*objtype = FILTER_OBJECT_TYPE_NONE;
		}

		return true;
	}

	if (ferror(fstate->fp))
	{
		pg_log_error("could not read from filter file \"%s\": %m", fstate->filename);
		fstate->is_error = true;
	}

	return false;
}
