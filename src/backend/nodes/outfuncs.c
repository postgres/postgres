/*-------------------------------------------------------------------------
 *
 * outfuncs.c
 *	  Output functions for Postgres tree nodes.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/nodes/outfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/attnum.h"
#include "common/shortest_dec.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "nodes/nodes.h"
#include "nodes/pg_list.h"
#include "utils/datum.h"

/* State flag that determines how nodeToStringInternal() should treat location fields */
static bool write_location_fields = false;

static void outChar(StringInfo str, char c);
static void outDouble(StringInfo str, double d);


/*
 * Macros to simplify output of different kinds of fields.  Use these
 * wherever possible to reduce the chance for silly typos.  Note that these
 * hard-wire conventions about the names of the local variables in an Out
 * routine.
 */

/* Write the label for the node type */
#define WRITE_NODE_TYPE(nodelabel) \
	appendStringInfoString(str, nodelabel)

/* Write an integer field (anything written as ":fldname %d") */
#define WRITE_INT_FIELD(fldname) \
	appendStringInfo(str, " :" CppAsString(fldname) " %d", node->fldname)

/* Write an unsigned integer field (anything written as ":fldname %u") */
#define WRITE_UINT_FIELD(fldname) \
	appendStringInfo(str, " :" CppAsString(fldname) " %u", node->fldname)

/* Write an unsigned integer field (anything written with UINT64_FORMAT) */
#define WRITE_UINT64_FIELD(fldname) \
	appendStringInfo(str, " :" CppAsString(fldname) " " UINT64_FORMAT, \
					 node->fldname)

/* Write an OID field (don't hard-wire assumption that OID is same as uint) */
#define WRITE_OID_FIELD(fldname) \
	appendStringInfo(str, " :" CppAsString(fldname) " %u", node->fldname)

/* Write a long-integer field */
#define WRITE_LONG_FIELD(fldname) \
	appendStringInfo(str, " :" CppAsString(fldname) " %ld", node->fldname)

/* Write a char field (ie, one ascii character) */
#define WRITE_CHAR_FIELD(fldname) \
	(appendStringInfo(str, " :" CppAsString(fldname) " "), \
	 outChar(str, node->fldname))

/* Write an enumerated-type field as an integer code */
#define WRITE_ENUM_FIELD(fldname, enumtype) \
	appendStringInfo(str, " :" CppAsString(fldname) " %d", \
					 (int) node->fldname)

/* Write a float field (actually, they're double) */
#define WRITE_FLOAT_FIELD(fldname) \
	(appendStringInfo(str, " :" CppAsString(fldname) " "), \
	 outDouble(str, node->fldname))

/* Write a boolean field */
#define WRITE_BOOL_FIELD(fldname) \
	appendStringInfo(str, " :" CppAsString(fldname) " %s", \
					 booltostr(node->fldname))

/* Write a character-string (possibly NULL) field */
#define WRITE_STRING_FIELD(fldname) \
	(appendStringInfoString(str, " :" CppAsString(fldname) " "), \
	 outToken(str, node->fldname))

/* Write a parse location field (actually same as INT case) */
#define WRITE_LOCATION_FIELD(fldname) \
	appendStringInfo(str, " :" CppAsString(fldname) " %d", write_location_fields ? node->fldname : -1)

/* Write a Node field */
#define WRITE_NODE_FIELD(fldname) \
	(appendStringInfoString(str, " :" CppAsString(fldname) " "), \
	 outNode(str, node->fldname))

/* Write a bitmapset field */
#define WRITE_BITMAPSET_FIELD(fldname) \
	(appendStringInfoString(str, " :" CppAsString(fldname) " "), \
	 outBitmapset(str, node->fldname))

/* Write a variable-length array (not a List) of Node pointers */
#define WRITE_NODE_ARRAY(fldname, len) \
	(appendStringInfoString(str, " :" CppAsString(fldname) " "), \
	 writeNodeArray(str, (const Node * const *) node->fldname, len))

/* Write a variable-length array of AttrNumber */
#define WRITE_ATTRNUMBER_ARRAY(fldname, len) \
	(appendStringInfoString(str, " :" CppAsString(fldname) " "), \
	 writeAttrNumberCols(str, node->fldname, len))

/* Write a variable-length array of Oid */
#define WRITE_OID_ARRAY(fldname, len) \
	(appendStringInfoString(str, " :" CppAsString(fldname) " "), \
	 writeOidCols(str, node->fldname, len))

/* Write a variable-length array of Index */
#define WRITE_INDEX_ARRAY(fldname, len) \
	(appendStringInfoString(str, " :" CppAsString(fldname) " "), \
	 writeIndexCols(str, node->fldname, len))

/* Write a variable-length array of int */
#define WRITE_INT_ARRAY(fldname, len) \
	(appendStringInfoString(str, " :" CppAsString(fldname) " "), \
	 writeIntCols(str, node->fldname, len))

/* Write a variable-length array of bool */
#define WRITE_BOOL_ARRAY(fldname, len) \
	(appendStringInfoString(str, " :" CppAsString(fldname) " "), \
	 writeBoolCols(str, node->fldname, len))

#define booltostr(x)  ((x) ? "true" : "false")


/*
 * outToken
 *	  Convert an ordinary string (eg, an identifier) into a form that
 *	  will be decoded back to a plain token by read.c's functions.
 *
 *	  If a null string pointer is given, it is encoded as '<>'.
 *	  An empty string is encoded as '""'.  To avoid ambiguity, input
 *	  strings beginning with '<' or '"' receive a leading backslash.
 */
void
outToken(StringInfo str, const char *s)
{
	if (s == NULL)
	{
		appendStringInfoString(str, "<>");
		return;
	}
	if (*s == '\0')
	{
		appendStringInfoString(str, "\"\"");
		return;
	}

	/*
	 * Look for characters or patterns that are treated specially by read.c
	 * (either in pg_strtok() or in nodeRead()), and therefore need a
	 * protective backslash.
	 */
	/* These characters only need to be quoted at the start of the string */
	if (*s == '<' ||
		*s == '"' ||
		isdigit((unsigned char) *s) ||
		((*s == '+' || *s == '-') &&
		 (isdigit((unsigned char) s[1]) || s[1] == '.')))
		appendStringInfoChar(str, '\\');
	while (*s)
	{
		/* These chars must be backslashed anywhere in the string */
		if (*s == ' ' || *s == '\n' || *s == '\t' ||
			*s == '(' || *s == ')' || *s == '{' || *s == '}' ||
			*s == '\\')
			appendStringInfoChar(str, '\\');
		appendStringInfoChar(str, *s++);
	}
}

/*
 * Convert one char.  Goes through outToken() so that special characters are
 * escaped.
 */
static void
outChar(StringInfo str, char c)
{
	char		in[2];

	/* Traditionally, we've represented \0 as <>, so keep doing that */
	if (c == '\0')
	{
		appendStringInfoString(str, "<>");
		return;
	}

	in[0] = c;
	in[1] = '\0';

	outToken(str, in);
}

/*
 * Convert a double value, attempting to ensure the value is preserved exactly.
 */
static void
outDouble(StringInfo str, double d)
{
	char		buf[DOUBLE_SHORTEST_DECIMAL_LEN];

	double_to_shortest_decimal_buf(d, buf);
	appendStringInfoString(str, buf);
}

/*
 * common implementation for scalar-array-writing functions
 *
 * The data format is either "<>" for a NULL pointer or "(item item item)".
 * fmtstr must include a leading space, and the rest of it must produce
 * something that will be seen as a single simple token by pg_strtok().
 * convfunc can be empty, or the name of a conversion macro or function.
 */
#define WRITE_SCALAR_ARRAY(fnname, datatype, fmtstr, convfunc) \
static void \
fnname(StringInfo str, const datatype *arr, int len) \
{ \
	if (arr != NULL) \
	{ \
		appendStringInfoChar(str, '('); \
		for (int i = 0; i < len; i++) \
			appendStringInfo(str, fmtstr, convfunc(arr[i])); \
		appendStringInfoChar(str, ')'); \
	} \
	else \
		appendStringInfoString(str, "<>"); \
}

WRITE_SCALAR_ARRAY(writeAttrNumberCols, AttrNumber, " %d",)
WRITE_SCALAR_ARRAY(writeOidCols, Oid, " %u",)
WRITE_SCALAR_ARRAY(writeIndexCols, Index, " %u",)
WRITE_SCALAR_ARRAY(writeIntCols, int, " %d",)
WRITE_SCALAR_ARRAY(writeBoolCols, bool, " %s", booltostr)

/*
 * Print an array (not a List) of Node pointers.
 *
 * The decoration is identical to that of scalar arrays, but we can't
 * quite use appendStringInfo() in the loop.
 */
static void
writeNodeArray(StringInfo str, const Node *const *arr, int len)
{
	if (arr != NULL)
	{
		appendStringInfoChar(str, '(');
		for (int i = 0; i < len; i++)
		{
			appendStringInfoChar(str, ' ');
			outNode(str, arr[i]);
		}
		appendStringInfoChar(str, ')');
	}
	else
		appendStringInfoString(str, "<>");
}

/*
 * Print a List.
 */
static void
_outList(StringInfo str, const List *node)
{
	const ListCell *lc;

	appendStringInfoChar(str, '(');

	if (IsA(node, IntList))
		appendStringInfoChar(str, 'i');
	else if (IsA(node, OidList))
		appendStringInfoChar(str, 'o');
	else if (IsA(node, XidList))
		appendStringInfoChar(str, 'x');

	foreach(lc, node)
	{
		/*
		 * For the sake of backward compatibility, we emit a slightly
		 * different whitespace format for lists of nodes vs. other types of
		 * lists. XXX: is this necessary?
		 */
		if (IsA(node, List))
		{
			outNode(str, lfirst(lc));
			if (lnext(node, lc))
				appendStringInfoChar(str, ' ');
		}
		else if (IsA(node, IntList))
			appendStringInfo(str, " %d", lfirst_int(lc));
		else if (IsA(node, OidList))
			appendStringInfo(str, " %u", lfirst_oid(lc));
		else if (IsA(node, XidList))
			appendStringInfo(str, " %u", lfirst_xid(lc));
		else
			elog(ERROR, "unrecognized list node type: %d",
				 (int) node->type);
	}

	appendStringInfoChar(str, ')');
}

/*
 * outBitmapset -
 *	   converts a bitmap set of integers
 *
 * Note: the output format is "(b int int ...)", similar to an integer List.
 *
 * We export this function for use by extensions that define extensible nodes.
 * That's somewhat historical, though, because calling outNode() will work.
 */
void
outBitmapset(StringInfo str, const Bitmapset *bms)
{
	int			x;

	appendStringInfoChar(str, '(');
	appendStringInfoChar(str, 'b');
	x = -1;
	while ((x = bms_next_member(bms, x)) >= 0)
		appendStringInfo(str, " %d", x);
	appendStringInfoChar(str, ')');
}

/*
 * Print the value of a Datum given its type.
 */
void
outDatum(StringInfo str, Datum value, int typlen, bool typbyval)
{
	Size		length,
				i;
	char	   *s;

	length = datumGetSize(value, typbyval, typlen);

	if (typbyval)
	{
		s = (char *) (&value);
		appendStringInfo(str, "%u [ ", (unsigned int) length);
		for (i = 0; i < (Size) sizeof(Datum); i++)
			appendStringInfo(str, "%d ", (int) (s[i]));
		appendStringInfoChar(str, ']');
	}
	else
	{
		s = (char *) DatumGetPointer(value);
		if (!PointerIsValid(s))
			appendStringInfoString(str, "0 [ ]");
		else
		{
			appendStringInfo(str, "%u [ ", (unsigned int) length);
			for (i = 0; i < length; i++)
				appendStringInfo(str, "%d ", (int) (s[i]));
			appendStringInfoChar(str, ']');
		}
	}
}


#include "outfuncs.funcs.c"


/*
 * Support functions for nodes with custom_read_write attribute or
 * special_read_write attribute
 */

static void
_outConst(StringInfo str, const Const *node)
{
	WRITE_NODE_TYPE("CONST");

	WRITE_OID_FIELD(consttype);
	WRITE_INT_FIELD(consttypmod);
	WRITE_OID_FIELD(constcollid);
	WRITE_INT_FIELD(constlen);
	WRITE_BOOL_FIELD(constbyval);
	WRITE_BOOL_FIELD(constisnull);
	WRITE_LOCATION_FIELD(location);

	appendStringInfoString(str, " :constvalue ");
	if (node->constisnull)
		appendStringInfoString(str, "<>");
	else
		outDatum(str, node->constvalue, node->constlen, node->constbyval);
}

static void
_outBoolExpr(StringInfo str, const BoolExpr *node)
{
	char	   *opstr = NULL;

	WRITE_NODE_TYPE("BOOLEXPR");

	/* do-it-yourself enum representation */
	switch (node->boolop)
	{
		case AND_EXPR:
			opstr = "and";
			break;
		case OR_EXPR:
			opstr = "or";
			break;
		case NOT_EXPR:
			opstr = "not";
			break;
	}
	appendStringInfoString(str, " :boolop ");
	outToken(str, opstr);

	WRITE_NODE_FIELD(args);
	WRITE_LOCATION_FIELD(location);
}

static void
_outForeignKeyOptInfo(StringInfo str, const ForeignKeyOptInfo *node)
{
	int			i;

	WRITE_NODE_TYPE("FOREIGNKEYOPTINFO");

	WRITE_UINT_FIELD(con_relid);
	WRITE_UINT_FIELD(ref_relid);
	WRITE_INT_FIELD(nkeys);
	WRITE_ATTRNUMBER_ARRAY(conkey, node->nkeys);
	WRITE_ATTRNUMBER_ARRAY(confkey, node->nkeys);
	WRITE_OID_ARRAY(conpfeqop, node->nkeys);
	WRITE_INT_FIELD(nmatched_ec);
	WRITE_INT_FIELD(nconst_ec);
	WRITE_INT_FIELD(nmatched_rcols);
	WRITE_INT_FIELD(nmatched_ri);
	/* for compactness, just print the number of matches per column: */
	appendStringInfoString(str, " :eclass");
	for (i = 0; i < node->nkeys; i++)
		appendStringInfo(str, " %d", (node->eclass[i] != NULL));
	appendStringInfoString(str, " :rinfos");
	for (i = 0; i < node->nkeys; i++)
		appendStringInfo(str, " %d", list_length(node->rinfos[i]));
}

static void
_outEquivalenceClass(StringInfo str, const EquivalenceClass *node)
{
	/*
	 * To simplify reading, we just chase up to the topmost merged EC and
	 * print that, without bothering to show the merge-ees separately.
	 */
	while (node->ec_merged)
		node = node->ec_merged;

	WRITE_NODE_TYPE("EQUIVALENCECLASS");

	WRITE_NODE_FIELD(ec_opfamilies);
	WRITE_OID_FIELD(ec_collation);
	WRITE_NODE_FIELD(ec_members);
	WRITE_NODE_FIELD(ec_sources);
	WRITE_NODE_FIELD(ec_derives);
	WRITE_BITMAPSET_FIELD(ec_relids);
	WRITE_BOOL_FIELD(ec_has_const);
	WRITE_BOOL_FIELD(ec_has_volatile);
	WRITE_BOOL_FIELD(ec_broken);
	WRITE_UINT_FIELD(ec_sortref);
	WRITE_UINT_FIELD(ec_min_security);
	WRITE_UINT_FIELD(ec_max_security);
}

static void
_outExtensibleNode(StringInfo str, const ExtensibleNode *node)
{
	const ExtensibleNodeMethods *methods;

	methods = GetExtensibleNodeMethods(node->extnodename, false);

	WRITE_NODE_TYPE("EXTENSIBLENODE");

	WRITE_STRING_FIELD(extnodename);

	/* serialize the private fields */
	methods->nodeOut(str, node);
}

static void
_outRangeTblEntry(StringInfo str, const RangeTblEntry *node)
{
	WRITE_NODE_TYPE("RANGETBLENTRY");

	WRITE_NODE_FIELD(alias);
	WRITE_NODE_FIELD(eref);
	WRITE_ENUM_FIELD(rtekind, RTEKind);

	switch (node->rtekind)
	{
		case RTE_RELATION:
			WRITE_OID_FIELD(relid);
			WRITE_BOOL_FIELD(inh);
			WRITE_CHAR_FIELD(relkind);
			WRITE_INT_FIELD(rellockmode);
			WRITE_UINT_FIELD(perminfoindex);
			WRITE_NODE_FIELD(tablesample);
			break;
		case RTE_SUBQUERY:
			WRITE_NODE_FIELD(subquery);
			WRITE_BOOL_FIELD(security_barrier);
			/* we re-use these RELATION fields, too: */
			WRITE_OID_FIELD(relid);
			WRITE_BOOL_FIELD(inh);
			WRITE_CHAR_FIELD(relkind);
			WRITE_INT_FIELD(rellockmode);
			WRITE_UINT_FIELD(perminfoindex);
			break;
		case RTE_JOIN:
			WRITE_ENUM_FIELD(jointype, JoinType);
			WRITE_INT_FIELD(joinmergedcols);
			WRITE_NODE_FIELD(joinaliasvars);
			WRITE_NODE_FIELD(joinleftcols);
			WRITE_NODE_FIELD(joinrightcols);
			WRITE_NODE_FIELD(join_using_alias);
			break;
		case RTE_FUNCTION:
			WRITE_NODE_FIELD(functions);
			WRITE_BOOL_FIELD(funcordinality);
			break;
		case RTE_TABLEFUNC:
			WRITE_NODE_FIELD(tablefunc);
			break;
		case RTE_VALUES:
			WRITE_NODE_FIELD(values_lists);
			WRITE_NODE_FIELD(coltypes);
			WRITE_NODE_FIELD(coltypmods);
			WRITE_NODE_FIELD(colcollations);
			break;
		case RTE_CTE:
			WRITE_STRING_FIELD(ctename);
			WRITE_UINT_FIELD(ctelevelsup);
			WRITE_BOOL_FIELD(self_reference);
			WRITE_NODE_FIELD(coltypes);
			WRITE_NODE_FIELD(coltypmods);
			WRITE_NODE_FIELD(colcollations);
			break;
		case RTE_NAMEDTUPLESTORE:
			WRITE_STRING_FIELD(enrname);
			WRITE_FLOAT_FIELD(enrtuples);
			WRITE_NODE_FIELD(coltypes);
			WRITE_NODE_FIELD(coltypmods);
			WRITE_NODE_FIELD(colcollations);
			/* we re-use these RELATION fields, too: */
			WRITE_OID_FIELD(relid);
			break;
		case RTE_RESULT:
			/* no extra fields */
			break;
		case RTE_GROUP:
			WRITE_NODE_FIELD(groupexprs);
			break;
		default:
			elog(ERROR, "unrecognized RTE kind: %d", (int) node->rtekind);
			break;
	}

	WRITE_BOOL_FIELD(lateral);
	WRITE_BOOL_FIELD(inFromCl);
	WRITE_NODE_FIELD(securityQuals);
}

static void
_outA_Expr(StringInfo str, const A_Expr *node)
{
	WRITE_NODE_TYPE("A_EXPR");

	switch (node->kind)
	{
		case AEXPR_OP:
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_OP_ANY:
			appendStringInfoString(str, " ANY");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_OP_ALL:
			appendStringInfoString(str, " ALL");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_DISTINCT:
			appendStringInfoString(str, " DISTINCT");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_NOT_DISTINCT:
			appendStringInfoString(str, " NOT_DISTINCT");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_NULLIF:
			appendStringInfoString(str, " NULLIF");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_IN:
			appendStringInfoString(str, " IN");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_LIKE:
			appendStringInfoString(str, " LIKE");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_ILIKE:
			appendStringInfoString(str, " ILIKE");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_SIMILAR:
			appendStringInfoString(str, " SIMILAR");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_BETWEEN:
			appendStringInfoString(str, " BETWEEN");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_NOT_BETWEEN:
			appendStringInfoString(str, " NOT_BETWEEN");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_BETWEEN_SYM:
			appendStringInfoString(str, " BETWEEN_SYM");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_NOT_BETWEEN_SYM:
			appendStringInfoString(str, " NOT_BETWEEN_SYM");
			WRITE_NODE_FIELD(name);
			break;
		default:
			elog(ERROR, "unrecognized A_Expr_Kind: %d", (int) node->kind);
			break;
	}

	WRITE_NODE_FIELD(lexpr);
	WRITE_NODE_FIELD(rexpr);
	WRITE_LOCATION_FIELD(location);
}

static void
_outInteger(StringInfo str, const Integer *node)
{
	appendStringInfo(str, "%d", node->ival);
}

static void
_outFloat(StringInfo str, const Float *node)
{
	/*
	 * We assume the value is a valid numeric literal and so does not need
	 * quoting.
	 */
	appendStringInfoString(str, node->fval);
}

static void
_outBoolean(StringInfo str, const Boolean *node)
{
	appendStringInfoString(str, node->boolval ? "true" : "false");
}

static void
_outString(StringInfo str, const String *node)
{
	/*
	 * We use outToken to provide escaping of the string's content, but we
	 * don't want it to convert an empty string to '""', because we're putting
	 * double quotes around the string already.
	 */
	appendStringInfoChar(str, '"');
	if (node->sval[0] != '\0')
		outToken(str, node->sval);
	appendStringInfoChar(str, '"');
}

static void
_outBitString(StringInfo str, const BitString *node)
{
	/*
	 * The lexer will always produce a string starting with 'b' or 'x'.  There
	 * might be characters following that that need escaping, but outToken
	 * won't escape the 'b' or 'x'.  This is relied on by nodeTokenType.
	 */
	Assert(node->bsval[0] == 'b' || node->bsval[0] == 'x');
	outToken(str, node->bsval);
}

static void
_outA_Const(StringInfo str, const A_Const *node)
{
	WRITE_NODE_TYPE("A_CONST");

	if (node->isnull)
		appendStringInfoString(str, " NULL");
	else
	{
		appendStringInfoString(str, " :val ");
		outNode(str, &node->val);
	}
	WRITE_LOCATION_FIELD(location);
}


/*
 * outNode -
 *	  converts a Node into ascii string and append it to 'str'
 */
void
outNode(StringInfo str, const void *obj)
{
	/* Guard against stack overflow due to overly complex expressions */
	check_stack_depth();

	if (obj == NULL)
		appendStringInfoString(str, "<>");
	else if (IsA(obj, List) || IsA(obj, IntList) || IsA(obj, OidList) ||
			 IsA(obj, XidList))
		_outList(str, obj);
	/* nodeRead does not want to see { } around these! */
	else if (IsA(obj, Integer))
		_outInteger(str, (Integer *) obj);
	else if (IsA(obj, Float))
		_outFloat(str, (Float *) obj);
	else if (IsA(obj, Boolean))
		_outBoolean(str, (Boolean *) obj);
	else if (IsA(obj, String))
		_outString(str, (String *) obj);
	else if (IsA(obj, BitString))
		_outBitString(str, (BitString *) obj);
	else if (IsA(obj, Bitmapset))
		outBitmapset(str, (Bitmapset *) obj);
	else
	{
		appendStringInfoChar(str, '{');
		switch (nodeTag(obj))
		{
#include "outfuncs.switch.c"

			default:

				/*
				 * This should be an ERROR, but it's too useful to be able to
				 * dump structures that outNode only understands part of.
				 */
				elog(WARNING, "could not dump unrecognized node type: %d",
					 (int) nodeTag(obj));
				break;
		}
		appendStringInfoChar(str, '}');
	}
}

/*
 * nodeToString -
 *	   returns the ascii representation of the Node as a palloc'd string
 *
 * write_loc_fields determines whether location fields are output with their
 * actual value rather than -1.  The actual value can be useful for debugging,
 * but for most uses, the actual value is not useful, since the original query
 * string is no longer available.
 */
static char *
nodeToStringInternal(const void *obj, bool write_loc_fields)
{
	StringInfoData str;
	bool		save_write_location_fields;

	save_write_location_fields = write_location_fields;
	write_location_fields = write_loc_fields;

	/* see stringinfo.h for an explanation of this maneuver */
	initStringInfo(&str);
	outNode(&str, obj);

	write_location_fields = save_write_location_fields;

	return str.data;
}

/*
 * Externally visible entry points
 */
char *
nodeToString(const void *obj)
{
	return nodeToStringInternal(obj, false);
}

char *
nodeToStringWithLocations(const void *obj)
{
	return nodeToStringInternal(obj, true);
}


/*
 * bmsToString -
 *	   returns the ascii representation of the Bitmapset as a palloc'd string
 */
char *
bmsToString(const Bitmapset *bms)
{
	StringInfoData str;

	/* see stringinfo.h for an explanation of this maneuver */
	initStringInfo(&str);
	outBitmapset(&str, bms);
	return str.data;
}
