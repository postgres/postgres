/*-------------------------------------------------------------------------
 *
 * define.c
 *	  Support routines for various kinds of object creation.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/define.c,v 1.76 2002/04/15 05:22:03 tgl Exp $
 *
 * DESCRIPTION
 *	  The "DefineFoo" routines take the parse tree and pick out the
 *	  appropriate arguments/flags, passing the results to the
 *	  corresponding "FooDefine" routines (in src/catalog) that do
 *	  the actual catalog-munging.  These routines also verify permission
 *	  of the user to execute the command.
 *
 * NOTES
 *	  These things must be defined and committed in the following order:
 *		"create function":
 *				input/output, recv/send procedures
 *		"create type":
 *				type
 *		"create operator":
 *				operators
 *
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <math.h>

#include "commands/defrem.h"
#include "parser/parse_type.h"


/*
 * Translate the input language name to lower case.
 *
 * Output buffer should be NAMEDATALEN long.
 */
void
case_translate_language_name(const char *input, char *output)
{
	int			i;

	for (i = 0; i < NAMEDATALEN - 1 && input[i]; ++i)
		output[i] = tolower((unsigned char) input[i]);

	output[i] = '\0';
}


/*
 * Extract a string value (otherwise uninterpreted) from a DefElem.
 */
char *
defGetString(DefElem *def)
{
	if (def->arg == NULL)
		elog(ERROR, "Define: \"%s\" requires a parameter",
			 def->defname);
	switch (nodeTag(def->arg))
	{
		case T_Integer:
			{
				char	   *str = palloc(32);

				snprintf(str, 32, "%ld", (long) intVal(def->arg));
				return str;
			}
		case T_Float:

			/*
			 * T_Float values are kept in string form, so this type cheat
			 * works (and doesn't risk losing precision)
			 */
			return strVal(def->arg);
		case T_String:
			return strVal(def->arg);
		case T_TypeName:
			return TypeNameToString((TypeName *) def->arg);
		default:
			elog(ERROR, "Define: cannot interpret argument of \"%s\"",
				 def->defname);
	}
	return NULL;				/* keep compiler quiet */
}

/*
 * Extract a numeric value (actually double) from a DefElem.
 */
double
defGetNumeric(DefElem *def)
{
	if (def->arg == NULL)
		elog(ERROR, "Define: \"%s\" requires a numeric value",
			 def->defname);
	switch (nodeTag(def->arg))
	{
		case T_Integer:
			return (double) intVal(def->arg);
		case T_Float:
			return floatVal(def->arg);
		default:
			elog(ERROR, "Define: \"%s\" requires a numeric value",
				 def->defname);
	}
	return 0;					/* keep compiler quiet */
}

/*
 * Extract a possibly-qualified name (as a List of Strings) from a DefElem.
 */
List *
defGetQualifiedName(DefElem *def)
{
	if (def->arg == NULL)
		elog(ERROR, "Define: \"%s\" requires a parameter",
			 def->defname);
	switch (nodeTag(def->arg))
	{
		case T_TypeName:
			return ((TypeName *) def->arg)->names;
		case T_String:
			/* Allow quoted name for backwards compatibility */
			return makeList1(def->arg);
		default:
			elog(ERROR, "Define: argument of \"%s\" must be a name",
				 def->defname);
	}
	return NIL;					/* keep compiler quiet */
}

/*
 * Extract a TypeName from a DefElem.
 */
TypeName *
defGetTypeName(DefElem *def)
{
	if (def->arg == NULL)
		elog(ERROR, "Define: \"%s\" requires a parameter",
			 def->defname);
	switch (nodeTag(def->arg))
	{
		case T_TypeName:
			return (TypeName *) def->arg;
		case T_String:
		{
			/* Allow quoted typename for backwards compatibility */
			TypeName   *n = makeNode(TypeName);

			n->names = makeList1(def->arg);
			n->typmod = -1;
			return n;
		}
		default:
			elog(ERROR, "Define: argument of \"%s\" must be a type name",
				 def->defname);
	}
	return NULL;				/* keep compiler quiet */
}

/*
 * Extract a type length indicator (either absolute bytes, or
 * -1 for "variable") from a DefElem.
 */
int
defGetTypeLength(DefElem *def)
{
	if (def->arg == NULL)
		elog(ERROR, "Define: \"%s\" requires a parameter",
			 def->defname);
	switch (nodeTag(def->arg))
	{
		case T_Integer:
			return intVal(def->arg);
		case T_Float:
			elog(ERROR, "Define: \"%s\" requires an integral value",
				 def->defname);
			break;
		case T_String:
			if (strcasecmp(strVal(def->arg), "variable") == 0)
				return -1;		/* variable length */
			break;
		case T_TypeName:
			/* cope if grammar chooses to believe "variable" is a typename */
			if (strcasecmp(TypeNameToString((TypeName *) def->arg),
						   "variable") == 0)
				return -1;		/* variable length */
			break;
		default:
			elog(ERROR, "Define: cannot interpret argument of \"%s\"",
				 def->defname);
	}
	elog(ERROR, "Define: invalid argument for \"%s\"",
		 def->defname);
	return 0;					/* keep compiler quiet */
}
