/*-------------------------------------------------------------------------
 *
 * explain_format.c
 *	  Format routines for explaining query execution plans
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/explain_format.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/explain.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "utils/json.h"
#include "utils/xml.h"

/* OR-able flags for ExplainXMLTag() */
#define X_OPENING 0
#define X_CLOSING 1
#define X_CLOSE_IMMEDIATE 2
#define X_NOWHITESPACE 4

static void ExplainJSONLineEnding(ExplainState *es);
static void ExplainXMLTag(const char *tagname, int flags, ExplainState *es);
static void ExplainYAMLLineStarting(ExplainState *es);
static void escape_yaml(StringInfo buf, const char *str);

/*
 * Explain a property, such as sort keys or targets, that takes the form of
 * a list of unlabeled items.  "data" is a list of C strings.
 */
void
ExplainPropertyList(const char *qlabel, List *data, ExplainState *es)
{
	ListCell   *lc;
	bool		first = true;

	switch (es->format)
	{
		case EXPLAIN_FORMAT_TEXT:
			ExplainIndentText(es);
			appendStringInfo(es->str, "%s: ", qlabel);
			foreach(lc, data)
			{
				if (!first)
					appendStringInfoString(es->str, ", ");
				appendStringInfoString(es->str, (const char *) lfirst(lc));
				first = false;
			}
			appendStringInfoChar(es->str, '\n');
			break;

		case EXPLAIN_FORMAT_XML:
			ExplainXMLTag(qlabel, X_OPENING, es);
			foreach(lc, data)
			{
				char	   *str;

				appendStringInfoSpaces(es->str, es->indent * 2 + 2);
				appendStringInfoString(es->str, "<Item>");
				str = escape_xml((const char *) lfirst(lc));
				appendStringInfoString(es->str, str);
				pfree(str);
				appendStringInfoString(es->str, "</Item>\n");
			}
			ExplainXMLTag(qlabel, X_CLOSING, es);
			break;

		case EXPLAIN_FORMAT_JSON:
			ExplainJSONLineEnding(es);
			appendStringInfoSpaces(es->str, es->indent * 2);
			escape_json(es->str, qlabel);
			appendStringInfoString(es->str, ": [");
			foreach(lc, data)
			{
				if (!first)
					appendStringInfoString(es->str, ", ");
				escape_json(es->str, (const char *) lfirst(lc));
				first = false;
			}
			appendStringInfoChar(es->str, ']');
			break;

		case EXPLAIN_FORMAT_YAML:
			ExplainYAMLLineStarting(es);
			appendStringInfo(es->str, "%s: ", qlabel);
			foreach(lc, data)
			{
				appendStringInfoChar(es->str, '\n');
				appendStringInfoSpaces(es->str, es->indent * 2 + 2);
				appendStringInfoString(es->str, "- ");
				escape_yaml(es->str, (const char *) lfirst(lc));
			}
			break;
	}
}

/*
 * Explain a property that takes the form of a list of unlabeled items within
 * another list.  "data" is a list of C strings.
 */
void
ExplainPropertyListNested(const char *qlabel, List *data, ExplainState *es)
{
	ListCell   *lc;
	bool		first = true;

	switch (es->format)
	{
		case EXPLAIN_FORMAT_TEXT:
		case EXPLAIN_FORMAT_XML:
			ExplainPropertyList(qlabel, data, es);
			return;

		case EXPLAIN_FORMAT_JSON:
			ExplainJSONLineEnding(es);
			appendStringInfoSpaces(es->str, es->indent * 2);
			appendStringInfoChar(es->str, '[');
			foreach(lc, data)
			{
				if (!first)
					appendStringInfoString(es->str, ", ");
				escape_json(es->str, (const char *) lfirst(lc));
				first = false;
			}
			appendStringInfoChar(es->str, ']');
			break;

		case EXPLAIN_FORMAT_YAML:
			ExplainYAMLLineStarting(es);
			appendStringInfoString(es->str, "- [");
			foreach(lc, data)
			{
				if (!first)
					appendStringInfoString(es->str, ", ");
				escape_yaml(es->str, (const char *) lfirst(lc));
				first = false;
			}
			appendStringInfoChar(es->str, ']');
			break;
	}
}

/*
 * Explain a simple property.
 *
 * If "numeric" is true, the value is a number (or other value that
 * doesn't need quoting in JSON).
 *
 * If unit is non-NULL the text format will display it after the value.
 *
 * This usually should not be invoked directly, but via one of the datatype
 * specific routines ExplainPropertyText, ExplainPropertyInteger, etc.
 */
static void
ExplainProperty(const char *qlabel, const char *unit, const char *value,
				bool numeric, ExplainState *es)
{
	switch (es->format)
	{
		case EXPLAIN_FORMAT_TEXT:
			ExplainIndentText(es);
			if (unit)
				appendStringInfo(es->str, "%s: %s %s\n", qlabel, value, unit);
			else
				appendStringInfo(es->str, "%s: %s\n", qlabel, value);
			break;

		case EXPLAIN_FORMAT_XML:
			{
				char	   *str;

				appendStringInfoSpaces(es->str, es->indent * 2);
				ExplainXMLTag(qlabel, X_OPENING | X_NOWHITESPACE, es);
				str = escape_xml(value);
				appendStringInfoString(es->str, str);
				pfree(str);
				ExplainXMLTag(qlabel, X_CLOSING | X_NOWHITESPACE, es);
				appendStringInfoChar(es->str, '\n');
			}
			break;

		case EXPLAIN_FORMAT_JSON:
			ExplainJSONLineEnding(es);
			appendStringInfoSpaces(es->str, es->indent * 2);
			escape_json(es->str, qlabel);
			appendStringInfoString(es->str, ": ");
			if (numeric)
				appendStringInfoString(es->str, value);
			else
				escape_json(es->str, value);
			break;

		case EXPLAIN_FORMAT_YAML:
			ExplainYAMLLineStarting(es);
			appendStringInfo(es->str, "%s: ", qlabel);
			if (numeric)
				appendStringInfoString(es->str, value);
			else
				escape_yaml(es->str, value);
			break;
	}
}

/*
 * Explain a string-valued property.
 */
void
ExplainPropertyText(const char *qlabel, const char *value, ExplainState *es)
{
	ExplainProperty(qlabel, NULL, value, false, es);
}

/*
 * Explain an integer-valued property.
 */
void
ExplainPropertyInteger(const char *qlabel, const char *unit, int64 value,
					   ExplainState *es)
{
	char		buf[32];

	snprintf(buf, sizeof(buf), INT64_FORMAT, value);
	ExplainProperty(qlabel, unit, buf, true, es);
}

/*
 * Explain an unsigned integer-valued property.
 */
void
ExplainPropertyUInteger(const char *qlabel, const char *unit, uint64 value,
						ExplainState *es)
{
	char		buf[32];

	snprintf(buf, sizeof(buf), UINT64_FORMAT, value);
	ExplainProperty(qlabel, unit, buf, true, es);
}

/*
 * Explain a float-valued property, using the specified number of
 * fractional digits.
 */
void
ExplainPropertyFloat(const char *qlabel, const char *unit, double value,
					 int ndigits, ExplainState *es)
{
	char	   *buf;

	buf = psprintf("%.*f", ndigits, value);
	ExplainProperty(qlabel, unit, buf, true, es);
	pfree(buf);
}

/*
 * Explain a bool-valued property.
 */
void
ExplainPropertyBool(const char *qlabel, bool value, ExplainState *es)
{
	ExplainProperty(qlabel, NULL, value ? "true" : "false", true, es);
}

/*
 * Open a group of related objects.
 *
 * objtype is the type of the group object, labelname is its label within
 * a containing object (if any).
 *
 * If labeled is true, the group members will be labeled properties,
 * while if it's false, they'll be unlabeled objects.
 */
void
ExplainOpenGroup(const char *objtype, const char *labelname,
				 bool labeled, ExplainState *es)
{
	switch (es->format)
	{
		case EXPLAIN_FORMAT_TEXT:
			/* nothing to do */
			break;

		case EXPLAIN_FORMAT_XML:
			ExplainXMLTag(objtype, X_OPENING, es);
			es->indent++;
			break;

		case EXPLAIN_FORMAT_JSON:
			ExplainJSONLineEnding(es);
			appendStringInfoSpaces(es->str, 2 * es->indent);
			if (labelname)
			{
				escape_json(es->str, labelname);
				appendStringInfoString(es->str, ": ");
			}
			appendStringInfoChar(es->str, labeled ? '{' : '[');

			/*
			 * In JSON format, the grouping_stack is an integer list.  0 means
			 * we've emitted nothing at this grouping level, 1 means we've
			 * emitted something (and so the next item needs a comma). See
			 * ExplainJSONLineEnding().
			 */
			es->grouping_stack = lcons_int(0, es->grouping_stack);
			es->indent++;
			break;

		case EXPLAIN_FORMAT_YAML:

			/*
			 * In YAML format, the grouping stack is an integer list.  0 means
			 * we've emitted nothing at this grouping level AND this grouping
			 * level is unlabeled and must be marked with "- ".  See
			 * ExplainYAMLLineStarting().
			 */
			ExplainYAMLLineStarting(es);
			if (labelname)
			{
				appendStringInfo(es->str, "%s: ", labelname);
				es->grouping_stack = lcons_int(1, es->grouping_stack);
			}
			else
			{
				appendStringInfoString(es->str, "- ");
				es->grouping_stack = lcons_int(0, es->grouping_stack);
			}
			es->indent++;
			break;
	}
}

/*
 * Close a group of related objects.
 * Parameters must match the corresponding ExplainOpenGroup call.
 */
void
ExplainCloseGroup(const char *objtype, const char *labelname,
				  bool labeled, ExplainState *es)
{
	switch (es->format)
	{
		case EXPLAIN_FORMAT_TEXT:
			/* nothing to do */
			break;

		case EXPLAIN_FORMAT_XML:
			es->indent--;
			ExplainXMLTag(objtype, X_CLOSING, es);
			break;

		case EXPLAIN_FORMAT_JSON:
			es->indent--;
			appendStringInfoChar(es->str, '\n');
			appendStringInfoSpaces(es->str, 2 * es->indent);
			appendStringInfoChar(es->str, labeled ? '}' : ']');
			es->grouping_stack = list_delete_first(es->grouping_stack);
			break;

		case EXPLAIN_FORMAT_YAML:
			es->indent--;
			es->grouping_stack = list_delete_first(es->grouping_stack);
			break;
	}
}

/*
 * Open a group of related objects, without emitting actual data.
 *
 * Prepare the formatting state as though we were beginning a group with
 * the identified properties, but don't actually emit anything.  Output
 * subsequent to this call can be redirected into a separate output buffer,
 * and then eventually appended to the main output buffer after doing a
 * regular ExplainOpenGroup call (with the same parameters).
 *
 * The extra "depth" parameter is the new group's depth compared to current.
 * It could be more than one, in case the eventual output will be enclosed
 * in additional nesting group levels.  We assume we don't need to track
 * formatting state for those levels while preparing this group's output.
 *
 * There is no ExplainCloseSetAsideGroup --- in current usage, we always
 * pop this state with ExplainSaveGroup.
 */
void
ExplainOpenSetAsideGroup(const char *objtype, const char *labelname,
						 bool labeled, int depth, ExplainState *es)
{
	switch (es->format)
	{
		case EXPLAIN_FORMAT_TEXT:
			/* nothing to do */
			break;

		case EXPLAIN_FORMAT_XML:
			es->indent += depth;
			break;

		case EXPLAIN_FORMAT_JSON:
			es->grouping_stack = lcons_int(0, es->grouping_stack);
			es->indent += depth;
			break;

		case EXPLAIN_FORMAT_YAML:
			if (labelname)
				es->grouping_stack = lcons_int(1, es->grouping_stack);
			else
				es->grouping_stack = lcons_int(0, es->grouping_stack);
			es->indent += depth;
			break;
	}
}

/*
 * Pop one level of grouping state, allowing for a re-push later.
 *
 * This is typically used after ExplainOpenSetAsideGroup; pass the
 * same "depth" used for that.
 *
 * This should not emit any output.  If state needs to be saved,
 * save it at *state_save.  Currently, an integer save area is sufficient
 * for all formats, but we might need to revisit that someday.
 */
void
ExplainSaveGroup(ExplainState *es, int depth, int *state_save)
{
	switch (es->format)
	{
		case EXPLAIN_FORMAT_TEXT:
			/* nothing to do */
			break;

		case EXPLAIN_FORMAT_XML:
			es->indent -= depth;
			break;

		case EXPLAIN_FORMAT_JSON:
			es->indent -= depth;
			*state_save = linitial_int(es->grouping_stack);
			es->grouping_stack = list_delete_first(es->grouping_stack);
			break;

		case EXPLAIN_FORMAT_YAML:
			es->indent -= depth;
			*state_save = linitial_int(es->grouping_stack);
			es->grouping_stack = list_delete_first(es->grouping_stack);
			break;
	}
}

/*
 * Re-push one level of grouping state, undoing the effects of ExplainSaveGroup.
 */
void
ExplainRestoreGroup(ExplainState *es, int depth, int *state_save)
{
	switch (es->format)
	{
		case EXPLAIN_FORMAT_TEXT:
			/* nothing to do */
			break;

		case EXPLAIN_FORMAT_XML:
			es->indent += depth;
			break;

		case EXPLAIN_FORMAT_JSON:
			es->grouping_stack = lcons_int(*state_save, es->grouping_stack);
			es->indent += depth;
			break;

		case EXPLAIN_FORMAT_YAML:
			es->grouping_stack = lcons_int(*state_save, es->grouping_stack);
			es->indent += depth;
			break;
	}
}

/*
 * Emit a "dummy" group that never has any members.
 *
 * objtype is the type of the group object, labelname is its label within
 * a containing object (if any).
 */
void
ExplainDummyGroup(const char *objtype, const char *labelname, ExplainState *es)
{
	switch (es->format)
	{
		case EXPLAIN_FORMAT_TEXT:
			/* nothing to do */
			break;

		case EXPLAIN_FORMAT_XML:
			ExplainXMLTag(objtype, X_CLOSE_IMMEDIATE, es);
			break;

		case EXPLAIN_FORMAT_JSON:
			ExplainJSONLineEnding(es);
			appendStringInfoSpaces(es->str, 2 * es->indent);
			if (labelname)
			{
				escape_json(es->str, labelname);
				appendStringInfoString(es->str, ": ");
			}
			escape_json(es->str, objtype);
			break;

		case EXPLAIN_FORMAT_YAML:
			ExplainYAMLLineStarting(es);
			if (labelname)
			{
				escape_yaml(es->str, labelname);
				appendStringInfoString(es->str, ": ");
			}
			else
			{
				appendStringInfoString(es->str, "- ");
			}
			escape_yaml(es->str, objtype);
			break;
	}
}

/*
 * Emit the start-of-output boilerplate.
 *
 * This is just enough different from processing a subgroup that we need
 * a separate pair of subroutines.
 */
void
ExplainBeginOutput(ExplainState *es)
{
	switch (es->format)
	{
		case EXPLAIN_FORMAT_TEXT:
			/* nothing to do */
			break;

		case EXPLAIN_FORMAT_XML:
			appendStringInfoString(es->str,
								   "<explain xmlns=\"http://www.postgresql.org/2009/explain\">\n");
			es->indent++;
			break;

		case EXPLAIN_FORMAT_JSON:
			/* top-level structure is an array of plans */
			appendStringInfoChar(es->str, '[');
			es->grouping_stack = lcons_int(0, es->grouping_stack);
			es->indent++;
			break;

		case EXPLAIN_FORMAT_YAML:
			es->grouping_stack = lcons_int(0, es->grouping_stack);
			break;
	}
}

/*
 * Emit the end-of-output boilerplate.
 */
void
ExplainEndOutput(ExplainState *es)
{
	switch (es->format)
	{
		case EXPLAIN_FORMAT_TEXT:
			/* nothing to do */
			break;

		case EXPLAIN_FORMAT_XML:
			es->indent--;
			appendStringInfoString(es->str, "</explain>");
			break;

		case EXPLAIN_FORMAT_JSON:
			es->indent--;
			appendStringInfoString(es->str, "\n]");
			es->grouping_stack = list_delete_first(es->grouping_stack);
			break;

		case EXPLAIN_FORMAT_YAML:
			es->grouping_stack = list_delete_first(es->grouping_stack);
			break;
	}
}

/*
 * Put an appropriate separator between multiple plans
 */
void
ExplainSeparatePlans(ExplainState *es)
{
	switch (es->format)
	{
		case EXPLAIN_FORMAT_TEXT:
			/* add a blank line */
			appendStringInfoChar(es->str, '\n');
			break;

		case EXPLAIN_FORMAT_XML:
		case EXPLAIN_FORMAT_JSON:
		case EXPLAIN_FORMAT_YAML:
			/* nothing to do */
			break;
	}
}

/*
 * Emit opening or closing XML tag.
 *
 * "flags" must contain X_OPENING, X_CLOSING, or X_CLOSE_IMMEDIATE.
 * Optionally, OR in X_NOWHITESPACE to suppress the whitespace we'd normally
 * add.
 *
 * XML restricts tag names more than our other output formats, eg they can't
 * contain white space or slashes.  Replace invalid characters with dashes,
 * so that for example "I/O Read Time" becomes "I-O-Read-Time".
 */
static void
ExplainXMLTag(const char *tagname, int flags, ExplainState *es)
{
	const char *s;
	const char *valid = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.";

	if ((flags & X_NOWHITESPACE) == 0)
		appendStringInfoSpaces(es->str, 2 * es->indent);
	appendStringInfoCharMacro(es->str, '<');
	if ((flags & X_CLOSING) != 0)
		appendStringInfoCharMacro(es->str, '/');
	for (s = tagname; *s; s++)
		appendStringInfoChar(es->str, strchr(valid, *s) ? *s : '-');
	if ((flags & X_CLOSE_IMMEDIATE) != 0)
		appendStringInfoString(es->str, " /");
	appendStringInfoCharMacro(es->str, '>');
	if ((flags & X_NOWHITESPACE) == 0)
		appendStringInfoCharMacro(es->str, '\n');
}

/*
 * Indent a text-format line.
 *
 * We indent by two spaces per indentation level.  However, when emitting
 * data for a parallel worker there might already be data on the current line
 * (cf. ExplainOpenWorker); in that case, don't indent any more.
 */
void
ExplainIndentText(ExplainState *es)
{
	Assert(es->format == EXPLAIN_FORMAT_TEXT);
	if (es->str->len == 0 || es->str->data[es->str->len - 1] == '\n')
		appendStringInfoSpaces(es->str, es->indent * 2);
}

/*
 * Emit a JSON line ending.
 *
 * JSON requires a comma after each property but the last.  To facilitate this,
 * in JSON format, the text emitted for each property begins just prior to the
 * preceding line-break (and comma, if applicable).
 */
static void
ExplainJSONLineEnding(ExplainState *es)
{
	Assert(es->format == EXPLAIN_FORMAT_JSON);
	if (linitial_int(es->grouping_stack) != 0)
		appendStringInfoChar(es->str, ',');
	else
		linitial_int(es->grouping_stack) = 1;
	appendStringInfoChar(es->str, '\n');
}

/*
 * Indent a YAML line.
 *
 * YAML lines are ordinarily indented by two spaces per indentation level.
 * The text emitted for each property begins just prior to the preceding
 * line-break, except for the first property in an unlabeled group, for which
 * it begins immediately after the "- " that introduces the group.  The first
 * property of the group appears on the same line as the opening "- ".
 */
static void
ExplainYAMLLineStarting(ExplainState *es)
{
	Assert(es->format == EXPLAIN_FORMAT_YAML);
	if (linitial_int(es->grouping_stack) == 0)
	{
		linitial_int(es->grouping_stack) = 1;
	}
	else
	{
		appendStringInfoChar(es->str, '\n');
		appendStringInfoSpaces(es->str, es->indent * 2);
	}
}

/*
 * YAML is a superset of JSON; unfortunately, the YAML quoting rules are
 * ridiculously complicated -- as documented in sections 5.3 and 7.3.3 of
 * http://yaml.org/spec/1.2/spec.html -- so we chose to just quote everything.
 * Empty strings, strings with leading or trailing whitespace, and strings
 * containing a variety of special characters must certainly be quoted or the
 * output is invalid; and other seemingly harmless strings like "0xa" or
 * "true" must be quoted, lest they be interpreted as a hexadecimal or Boolean
 * constant rather than a string.
 */
static void
escape_yaml(StringInfo buf, const char *str)
{
	escape_json(buf, str);
}
