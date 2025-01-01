/*-------------------------------------------------------------------------
 *
 * filter.c
 *		Implementation of simple filter file parser
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/bin/pg_dump/filter.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "common/logging.h"
#include "common/string.h"
#include "filter.h"
#include "lib/stringinfo.h"
#include "pqexpbuffer.h"

#define		is_keyword_str(cstr, str, bytes) \
	((strlen(cstr) == (bytes)) && (pg_strncasecmp((cstr), (str), (bytes)) == 0))

/*
 * Following routines are called from pg_dump, pg_dumpall and pg_restore.
 * Since the implementation of exit_nicely is application specific, each
 * application need to pass a function pointer to the exit_nicely function to
 * use for exiting on errors.
 */

/*
 * Opens filter's file and initialize fstate structure.
 */
void
filter_init(FilterStateData *fstate, const char *filename, exit_function f_exit)
{
	fstate->filename = filename;
	fstate->lineno = 0;
	fstate->exit_nicely = f_exit;
	initStringInfo(&fstate->linebuff);

	if (strcmp(filename, "-") != 0)
	{
		fstate->fp = fopen(filename, "r");
		if (!fstate->fp)
		{
			pg_log_error("could not open filter file \"%s\": %m", filename);
			fstate->exit_nicely(1);
		}
	}
	else
		fstate->fp = stdin;
}

/*
 * Release allocated resources for the given filter.
 */
void
filter_free(FilterStateData *fstate)
{
	if (!fstate)
		return;

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
 * Translate FilterObjectType enum to string. The main purpose is for error
 * message formatting.
 */
const char *
filter_object_type_name(FilterObjectType fot)
{
	switch (fot)
	{
		case FILTER_OBJECT_TYPE_NONE:
			return "comment or empty line";
		case FILTER_OBJECT_TYPE_TABLE_DATA:
			return "table data";
		case FILTER_OBJECT_TYPE_TABLE_DATA_AND_CHILDREN:
			return "table data and children";
		case FILTER_OBJECT_TYPE_DATABASE:
			return "database";
		case FILTER_OBJECT_TYPE_EXTENSION:
			return "extension";
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
		case FILTER_OBJECT_TYPE_TABLE_AND_CHILDREN:
			return "table and children";
		case FILTER_OBJECT_TYPE_TRIGGER:
			return "trigger";
	}

	/* should never get here */
	pg_unreachable();
}

/*
 * Returns true when keyword is one of supported object types, and
 * set related objtype. Returns false, when keyword is not assigned
 * with known object type.
 */
static bool
get_object_type(const char *keyword, int size, FilterObjectType *objtype)
{
	if (is_keyword_str("table_data", keyword, size))
		*objtype = FILTER_OBJECT_TYPE_TABLE_DATA;
	else if (is_keyword_str("table_data_and_children", keyword, size))
		*objtype = FILTER_OBJECT_TYPE_TABLE_DATA_AND_CHILDREN;
	else if (is_keyword_str("database", keyword, size))
		*objtype = FILTER_OBJECT_TYPE_DATABASE;
	else if (is_keyword_str("extension", keyword, size))
		*objtype = FILTER_OBJECT_TYPE_EXTENSION;
	else if (is_keyword_str("foreign_data", keyword, size))
		*objtype = FILTER_OBJECT_TYPE_FOREIGN_DATA;
	else if (is_keyword_str("function", keyword, size))
		*objtype = FILTER_OBJECT_TYPE_FUNCTION;
	else if (is_keyword_str("index", keyword, size))
		*objtype = FILTER_OBJECT_TYPE_INDEX;
	else if (is_keyword_str("schema", keyword, size))
		*objtype = FILTER_OBJECT_TYPE_SCHEMA;
	else if (is_keyword_str("table", keyword, size))
		*objtype = FILTER_OBJECT_TYPE_TABLE;
	else if (is_keyword_str("table_and_children", keyword, size))
		*objtype = FILTER_OBJECT_TYPE_TABLE_AND_CHILDREN;
	else if (is_keyword_str("trigger", keyword, size))
		*objtype = FILTER_OBJECT_TYPE_TRIGGER;
	else
		return false;

	return true;
}


void
pg_log_filter_error(FilterStateData *fstate, const char *fmt,...)
{
	va_list		argp;
	char		buf[256];

	va_start(argp, fmt);
	vsnprintf(buf, sizeof(buf), fmt, argp);
	va_end(argp);

	if (fstate->fp == stdin)
		pg_log_error("invalid format in filter read from standard input on line %d: %s",
					 fstate->lineno, buf);
	else
		pg_log_error("invalid format in filter read from file \"%s\" on line %d: %s",
					 fstate->filename, fstate->lineno, buf);
}

/*
 * filter_get_keyword - read the next filter keyword from buffer
 *
 * Search for keywords (limited to ascii alphabetic characters) in
 * the passed in line buffer. Returns NULL when the buffer is empty or the first
 * char is not alpha. The char '_' is allowed, except as the first character.
 * The length of the found keyword is returned in the size parameter.
 */
static const char *
filter_get_keyword(const char **line, int *size)
{
	const char *ptr = *line;
	const char *result = NULL;

	/* Set returned length preemptively in case no keyword is found */
	*size = 0;

	/* Skip initial whitespace */
	while (isspace((unsigned char) *ptr))
		ptr++;

	if (isalpha((unsigned char) *ptr))
	{
		result = ptr++;

		while (isalpha((unsigned char) *ptr) || *ptr == '_')
			ptr++;

		*size = ptr - result;
	}

	*line = ptr;

	return result;
}

/*
 * read_quoted_string - read quoted possibly multi line string
 *
 * Reads a quoted string which can span over multiple lines and returns a
 * pointer to next char after ending double quotes; it will exit on errors.
 */
static const char *
read_quoted_string(FilterStateData *fstate,
				   const char *str,
				   PQExpBuffer pattern)
{
	appendPQExpBufferChar(pattern, '"');
	str++;

	while (1)
	{
		/*
		 * We can ignore \r or \n chars because the string is read by
		 * pg_get_line_buf, so these chars should be just trailing chars.
		 */
		if (*str == '\r' || *str == '\n')
		{
			str++;
			continue;
		}

		if (*str == '\0')
		{
			Assert(fstate->linebuff.data);

			if (!pg_get_line_buf(fstate->fp, &fstate->linebuff))
			{
				if (ferror(fstate->fp))
					pg_log_error("could not read from filter file \"%s\": %m",
								 fstate->filename);
				else
					pg_log_filter_error(fstate, _("unexpected end of file"));

				fstate->exit_nicely(1);
			}

			str = fstate->linebuff.data;

			appendPQExpBufferChar(pattern, '\n');
			fstate->lineno++;
		}

		if (*str == '"')
		{
			appendPQExpBufferChar(pattern, '"');
			str++;

			if (*str == '"')
			{
				appendPQExpBufferChar(pattern, '"');
				str++;
			}
			else
				break;
		}
		else if (*str == '\\')
		{
			str++;
			if (*str == 'n')
				appendPQExpBufferChar(pattern, '\n');
			else if (*str == '\\')
				appendPQExpBufferChar(pattern, '\\');

			str++;
		}
		else
			appendPQExpBufferChar(pattern, *str++);
	}

	return str;
}

/*
 * read_pattern - reads on object pattern from input
 *
 * This function will parse any valid identifier (quoted or not, qualified or
 * not), which can also includes the full signature for routines.
 * Note that this function takes special care to sanitize the detected
 * identifier (removing extraneous whitespaces or other unnecessary
 * characters).  This is necessary as most backup/restore filtering functions
 * only recognize identifiers if they are written exactly the same way as
 * they are output by the server.
 *
 * Returns a pointer to next character after the found identifier and exits
 * on error.
 */
static const char *
read_pattern(FilterStateData *fstate, const char *str, PQExpBuffer pattern)
{
	bool		skip_space = true;
	bool		found_space = false;

	/* Skip initial whitespace */
	while (isspace((unsigned char) *str))
		str++;

	if (*str == '\0')
	{
		pg_log_filter_error(fstate, _("missing object name pattern"));
		fstate->exit_nicely(1);
	}

	while (*str && *str != '#')
	{
		while (*str && !isspace((unsigned char) *str) && !strchr("#,.()\"", *str))
		{
			/*
			 * Append space only when it is allowed, and when it was found in
			 * original string.
			 */
			if (!skip_space && found_space)
			{
				appendPQExpBufferChar(pattern, ' ');
				skip_space = true;
			}

			appendPQExpBufferChar(pattern, *str++);
		}

		skip_space = false;

		if (*str == '"')
		{
			if (found_space)
				appendPQExpBufferChar(pattern, ' ');

			str = read_quoted_string(fstate, str, pattern);
		}
		else if (*str == ',')
		{
			appendPQExpBufferStr(pattern, ", ");
			skip_space = true;
			str++;
		}
		else if (*str && strchr(".()", *str))
		{
			appendPQExpBufferChar(pattern, *str++);
			skip_space = true;
		}

		found_space = false;

		/* skip ending whitespaces */
		while (isspace((unsigned char) *str))
		{
			found_space = true;
			str++;
		}
	}

	return str;
}

/*
 * filter_read_item - Read command/type/pattern triplet from a filter file
 *
 * This will parse one filter item from the filter file, and while it is a
 * row based format a pattern may span more than one line due to how object
 * names can be constructed.  The expected format of the filter file is:
 *
 * <command> <object_type> <pattern>
 *
 * command can be "include" or "exclude".
 *
 * Supported object types are described by enum FilterObjectType
 * (see function get_object_type).
 *
 * pattern can be any possibly-quoted and possibly-qualified identifier.  It
 * follows the same rules as other object include and exclude functions so it
 * can also use wildcards.
 *
 * Returns true when one filter item was successfully read and parsed.  When
 * object name contains \n chars, then more than one line from input file can
 * be processed.  Returns false when the filter file reaches EOF. In case of
 * error, the function will emit an appropriate error message and exit.
 */
bool
filter_read_item(FilterStateData *fstate,
				 char **objname,
				 FilterCommandType *comtype,
				 FilterObjectType *objtype)
{
	if (pg_get_line_buf(fstate->fp, &fstate->linebuff))
	{
		const char *str = fstate->linebuff.data;
		const char *keyword;
		int			size;
		PQExpBufferData pattern;

		fstate->lineno++;

		/* Skip initial white spaces */
		while (isspace((unsigned char) *str))
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
			keyword = filter_get_keyword(&str, &size);
			if (!keyword)
			{
				pg_log_filter_error(fstate,
									_("no filter command found (expected \"include\" or \"exclude\")"));
				fstate->exit_nicely(1);
			}

			if (is_keyword_str("include", keyword, size))
				*comtype = FILTER_COMMAND_TYPE_INCLUDE;
			else if (is_keyword_str("exclude", keyword, size))
				*comtype = FILTER_COMMAND_TYPE_EXCLUDE;
			else
			{
				pg_log_filter_error(fstate,
									_("invalid filter command (expected \"include\" or \"exclude\")"));
				fstate->exit_nicely(1);
			}

			keyword = filter_get_keyword(&str, &size);
			if (!keyword)
			{
				pg_log_filter_error(fstate, _("missing filter object type"));
				fstate->exit_nicely(1);
			}

			if (!get_object_type(keyword, size, objtype))
			{
				pg_log_filter_error(fstate,
									_("unsupported filter object type: \"%.*s\""), size, keyword);
				fstate->exit_nicely(1);
			}

			initPQExpBuffer(&pattern);

			str = read_pattern(fstate, str, &pattern);
			*objname = pattern.data;
		}
		else
		{
			*objname = NULL;
			*comtype = FILTER_COMMAND_TYPE_NONE;
			*objtype = FILTER_OBJECT_TYPE_NONE;
		}

		return true;
	}

	if (ferror(fstate->fp))
	{
		pg_log_error("could not read from filter file \"%s\": %m", fstate->filename);
		fstate->exit_nicely(1);
	}

	return false;
}
