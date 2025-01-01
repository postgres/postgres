/*-------------------------------------------------------------------------
 *
 * jsonpath.h
 *	Definitions for jsonpath datatype
 *
 * Copyright (c) 2019-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	src/include/utils/jsonpath.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef JSONPATH_H
#define JSONPATH_H

#include "executor/tablefunc.h"
#include "fmgr.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "utils/jsonb.h"

typedef struct
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	uint32		header;			/* version and flags (see below) */
	char		data[FLEXIBLE_ARRAY_MEMBER];
} JsonPath;

#define JSONPATH_VERSION	(0x01)
#define JSONPATH_LAX		(0x80000000)
#define JSONPATH_HDRSZ		(offsetof(JsonPath, data))

static inline JsonPath *
DatumGetJsonPathP(Datum d)
{
	return (JsonPath *) PG_DETOAST_DATUM(d);
}

static inline JsonPath *
DatumGetJsonPathPCopy(Datum d)
{
	return (JsonPath *) PG_DETOAST_DATUM_COPY(d);
}

#define PG_GETARG_JSONPATH_P(x)			DatumGetJsonPathP(PG_GETARG_DATUM(x))
#define PG_GETARG_JSONPATH_P_COPY(x)	DatumGetJsonPathPCopy(PG_GETARG_DATUM(x))
#define PG_RETURN_JSONPATH_P(p)			PG_RETURN_POINTER(p)

#define jspIsScalar(type) ((type) >= jpiNull && (type) <= jpiBool)

/*
 * All node's type of jsonpath expression
 *
 * These become part of the on-disk representation of the jsonpath type.
 * Therefore, to preserve pg_upgradability, the order must not be changed, and
 * new values must be added at the end.
 *
 * It is recommended that switch cases etc. in other parts of the code also
 * use this order, to maintain some consistency.
 */
typedef enum JsonPathItemType
{
	jpiNull = jbvNull,			/* NULL literal */
	jpiString = jbvString,		/* string literal */
	jpiNumeric = jbvNumeric,	/* numeric literal */
	jpiBool = jbvBool,			/* boolean literal: TRUE or FALSE */
	jpiAnd,						/* predicate && predicate */
	jpiOr,						/* predicate || predicate */
	jpiNot,						/* ! predicate */
	jpiIsUnknown,				/* (predicate) IS UNKNOWN */
	jpiEqual,					/* expr == expr */
	jpiNotEqual,				/* expr != expr */
	jpiLess,					/* expr < expr */
	jpiGreater,					/* expr > expr */
	jpiLessOrEqual,				/* expr <= expr */
	jpiGreaterOrEqual,			/* expr >= expr */
	jpiAdd,						/* expr + expr */
	jpiSub,						/* expr - expr */
	jpiMul,						/* expr * expr */
	jpiDiv,						/* expr / expr */
	jpiMod,						/* expr % expr */
	jpiPlus,					/* + expr */
	jpiMinus,					/* - expr */
	jpiAnyArray,				/* [*] */
	jpiAnyKey,					/* .* */
	jpiIndexArray,				/* [subscript, ...] */
	jpiAny,						/* .** */
	jpiKey,						/* .key */
	jpiCurrent,					/* @ */
	jpiRoot,					/* $ */
	jpiVariable,				/* $variable */
	jpiFilter,					/* ? (predicate) */
	jpiExists,					/* EXISTS (expr) predicate */
	jpiType,					/* .type() item method */
	jpiSize,					/* .size() item method */
	jpiAbs,						/* .abs() item method */
	jpiFloor,					/* .floor() item method */
	jpiCeiling,					/* .ceiling() item method */
	jpiDouble,					/* .double() item method */
	jpiDatetime,				/* .datetime() item method */
	jpiKeyValue,				/* .keyvalue() item method */
	jpiSubscript,				/* array subscript: 'expr' or 'expr TO expr' */
	jpiLast,					/* LAST array subscript */
	jpiStartsWith,				/* STARTS WITH predicate */
	jpiLikeRegex,				/* LIKE_REGEX predicate */
	jpiBigint,					/* .bigint() item method */
	jpiBoolean,					/* .boolean() item method */
	jpiDate,					/* .date() item method */
	jpiDecimal,					/* .decimal() item method */
	jpiInteger,					/* .integer() item method */
	jpiNumber,					/* .number() item method */
	jpiStringFunc,				/* .string() item method */
	jpiTime,					/* .time() item method */
	jpiTimeTz,					/* .time_tz() item method */
	jpiTimestamp,				/* .timestamp() item method */
	jpiTimestampTz,				/* .timestamp_tz() item method */
} JsonPathItemType;

/* XQuery regex mode flags for LIKE_REGEX predicate */
#define JSP_REGEX_ICASE		0x01	/* i flag, case insensitive */
#define JSP_REGEX_DOTALL	0x02	/* s flag, dot matches newline */
#define JSP_REGEX_MLINE		0x04	/* m flag, ^/$ match at newlines */
#define JSP_REGEX_WSPACE	0x08	/* x flag, ignore whitespace in pattern */
#define JSP_REGEX_QUOTE		0x10	/* q flag, no special characters */

/*
 * Support functions to parse/construct binary value.
 * Unlike many other representation of expression the first/main
 * node is not an operation but left operand of expression. That
 * allows to implement cheap follow-path descending in jsonb
 * structure and then execute operator with right operand
 */

typedef struct JsonPathItem
{
	JsonPathItemType type;

	/* position form base to next node */
	int32		nextPos;

	/*
	 * pointer into JsonPath value to current node, all positions of current
	 * are relative to this base
	 */
	char	   *base;

	union
	{
		/* classic operator with two operands: and, or etc */
		struct
		{
			int32		left;
			int32		right;
		}			args;

		/* any unary operation */
		int32		arg;

		/* storage for jpiIndexArray: indexes of array */
		struct
		{
			int32		nelems;
			struct
			{
				int32		from;
				int32		to;
			}		   *elems;
		}			array;

		/* jpiAny: levels */
		struct
		{
			uint32		first;
			uint32		last;
		}			anybounds;

		struct
		{
			char	   *data;	/* for bool, numeric and string/key */
			int32		datalen;	/* filled only for string/key */
		}			value;

		struct
		{
			int32		expr;
			char	   *pattern;
			int32		patternlen;
			uint32		flags;
		}			like_regex;
	}			content;
} JsonPathItem;

#define jspHasNext(jsp) ((jsp)->nextPos > 0)

extern void jspInit(JsonPathItem *v, JsonPath *js);
extern void jspInitByBuffer(JsonPathItem *v, char *base, int32 pos);
extern bool jspGetNext(JsonPathItem *v, JsonPathItem *a);
extern void jspGetArg(JsonPathItem *v, JsonPathItem *a);
extern void jspGetLeftArg(JsonPathItem *v, JsonPathItem *a);
extern void jspGetRightArg(JsonPathItem *v, JsonPathItem *a);
extern Numeric jspGetNumeric(JsonPathItem *v);
extern bool jspGetBool(JsonPathItem *v);
extern char *jspGetString(JsonPathItem *v, int32 *len);
extern bool jspGetArraySubscript(JsonPathItem *v, JsonPathItem *from,
								 JsonPathItem *to, int i);
extern bool jspIsMutable(JsonPath *path, List *varnames, List *varexprs);

extern const char *jspOperationName(JsonPathItemType type);

/*
 * Parsing support data structures.
 */

typedef struct JsonPathParseItem JsonPathParseItem;

struct JsonPathParseItem
{
	JsonPathItemType type;
	JsonPathParseItem *next;	/* next in path */

	union
	{

		/* classic operator with two operands: and, or etc */
		struct
		{
			JsonPathParseItem *left;
			JsonPathParseItem *right;
		}			args;

		/* any unary operation */
		JsonPathParseItem *arg;

		/* storage for jpiIndexArray: indexes of array */
		struct
		{
			int			nelems;
			struct
			{
				JsonPathParseItem *from;
				JsonPathParseItem *to;
			}		   *elems;
		}			array;

		/* jpiAny: levels */
		struct
		{
			uint32		first;
			uint32		last;
		}			anybounds;

		struct
		{
			JsonPathParseItem *expr;
			char	   *pattern;	/* could not be not null-terminated */
			uint32		patternlen;
			uint32		flags;
		}			like_regex;

		/* scalars */
		Numeric numeric;
		bool		boolean;
		struct
		{
			uint32		len;
			char	   *val;	/* could not be not null-terminated */
		}			string;
	}			value;
};

typedef struct JsonPathParseResult
{
	JsonPathParseItem *expr;
	bool		lax;
} JsonPathParseResult;

extern JsonPathParseResult *parsejsonpath(const char *str, int len,
										  struct Node *escontext);

extern bool jspConvertRegexFlags(uint32 xflags, int *result,
								 struct Node *escontext);

/*
 * Struct for details about external variables passed into jsonpath executor
 */
typedef struct JsonPathVariable
{
	char	   *name;
	int			namelen;		/* strlen(name) as cache for GetJsonPathVar() */
	Oid			typid;
	int32		typmod;
	Datum		value;
	bool		isnull;
} JsonPathVariable;


/* SQL/JSON query functions */
extern bool JsonPathExists(Datum jb, JsonPath *jp, bool *error, List *vars);
extern Datum JsonPathQuery(Datum jb, JsonPath *jp, JsonWrapper wrapper,
						   bool *empty, bool *error, List *vars,
						   const char *column_name);
extern JsonbValue *JsonPathValue(Datum jb, JsonPath *jp, bool *empty,
								 bool *error, List *vars,
								 const char *column_name);

/* For JSON_TABLE() */
extern PGDLLIMPORT const TableFuncRoutine JsonbTableRoutine;

#endif
