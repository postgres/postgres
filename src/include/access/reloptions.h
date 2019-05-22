/*-------------------------------------------------------------------------
 *
 * reloptions.h
 *	  Core support for relation and tablespace options (pg_class.reloptions
 *	  and pg_tablespace.spcoptions)
 *
 * Note: the functions dealing with text-array reloptions values declare
 * them as Datum, not ArrayType *, to avoid needing to include array.h
 * into a lot of low-level code.
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/reloptions.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELOPTIONS_H
#define RELOPTIONS_H

#include "access/amapi.h"
#include "access/htup.h"
#include "access/tupdesc.h"
#include "nodes/pg_list.h"
#include "storage/lock.h"

/* types supported by reloptions */
typedef enum relopt_type
{
	RELOPT_TYPE_BOOL,
	RELOPT_TYPE_INT,
	RELOPT_TYPE_REAL,
	RELOPT_TYPE_STRING
} relopt_type;

/* kinds supported by reloptions */
typedef enum relopt_kind
{
	RELOPT_KIND_HEAP = (1 << 0),
	RELOPT_KIND_TOAST = (1 << 1),
	RELOPT_KIND_BTREE = (1 << 2),
	RELOPT_KIND_HASH = (1 << 3),
	RELOPT_KIND_GIN = (1 << 4),
	RELOPT_KIND_GIST = (1 << 5),
	RELOPT_KIND_ATTRIBUTE = (1 << 6),
	RELOPT_KIND_TABLESPACE = (1 << 7),
	RELOPT_KIND_SPGIST = (1 << 8),
	RELOPT_KIND_VIEW = (1 << 9),
	RELOPT_KIND_BRIN = (1 << 10),
	RELOPT_KIND_PARTITIONED = (1 << 11),
	/* if you add a new kind, make sure you update "last_default" too */
	RELOPT_KIND_LAST_DEFAULT = RELOPT_KIND_PARTITIONED,
	/* some compilers treat enums as signed ints, so we can't use 1 << 31 */
	RELOPT_KIND_MAX = (1 << 30)
} relopt_kind;

/* reloption namespaces allowed for heaps -- currently only TOAST */
#define HEAP_RELOPT_NAMESPACES { "toast", NULL }

/* generic struct to hold shared data */
typedef struct relopt_gen
{
	const char *name;			/* must be first (used as list termination
								 * marker) */
	const char *desc;
	bits32		kinds;
	LOCKMODE	lockmode;
	int			namelen;
	relopt_type type;
} relopt_gen;

/* holds a parsed value */
typedef struct relopt_value
{
	relopt_gen *gen;
	bool		isset;
	union
	{
		bool		bool_val;
		int			int_val;
		double		real_val;
		char	   *string_val; /* allocated separately */
	}			values;
} relopt_value;

/* reloptions records for specific variable types */
typedef struct relopt_bool
{
	relopt_gen	gen;
	bool		default_val;
} relopt_bool;

typedef struct relopt_int
{
	relopt_gen	gen;
	int			default_val;
	int			min;
	int			max;
} relopt_int;

typedef struct relopt_real
{
	relopt_gen	gen;
	double		default_val;
	double		min;
	double		max;
} relopt_real;

/* validation routines for strings */
typedef void (*validate_string_relopt) (const char *value);

typedef struct relopt_string
{
	relopt_gen	gen;
	int			default_len;
	bool		default_isnull;
	validate_string_relopt validate_cb;
	char	   *default_val;
} relopt_string;

/* This is the table datatype for fillRelOptions */
typedef struct
{
	const char *optname;		/* option's name */
	relopt_type opttype;		/* option's datatype */
	int			offset;			/* offset of field in result struct */
} relopt_parse_elt;


/*
 * These macros exist for the convenience of amoptions writers (but consider
 * using fillRelOptions, which is a lot simpler).  Beware of multiple
 * evaluation of arguments!
 *
 * The last argument in the HANDLE_*_RELOPTION macros allows the caller to
 * determine whether the option was set (true), or its value acquired from
 * defaults (false); it can be passed as (char *) NULL if the caller does not
 * need this information.
 *
 * optname is the option name (a string), var is the variable
 * on which the value should be stored (e.g. StdRdOptions->fillfactor), and
 * option is a relopt_value pointer.
 *
 * The normal way to use this is to loop on the relopt_value array returned by
 * parseRelOptions:
 * for (i = 0; options[i].gen->name; i++)
 * {
 *		if (HAVE_RELOPTION("fillfactor", options[i])
 *		{
 *			HANDLE_INT_RELOPTION("fillfactor", rdopts->fillfactor, options[i], &isset);
 *			continue;
 *		}
 *		if (HAVE_RELOPTION("default_row_acl", options[i])
 *		{
 *			...
 *		}
 *		...
 *		if (validate)
 *			ereport(ERROR,
 *					(errmsg("unknown option")));
 *	}
 *
 *	Note that this is more or less the same that fillRelOptions does, so only
 *	use this if you need to do something non-standard within some option's
 *	code block.
 */
#define HAVE_RELOPTION(optname, option) \
	(strncmp(option.gen->name, optname, option.gen->namelen + 1) == 0)

#define HANDLE_INT_RELOPTION(optname, var, option, wasset)		\
	do {														\
		if (option.isset)										\
			var = option.values.int_val;						\
		else													\
			var = ((relopt_int *) option.gen)->default_val;		\
		(wasset) != NULL ? *(wasset) = option.isset : (dummyret)NULL; \
	} while (0)

#define HANDLE_BOOL_RELOPTION(optname, var, option, wasset)			\
	do {															\
		if (option.isset)										\
			var = option.values.bool_val;						\
		else													\
			var = ((relopt_bool *) option.gen)->default_val;	\
		(wasset) != NULL ? *(wasset) = option.isset : (dummyret) NULL; \
	} while (0)

#define HANDLE_REAL_RELOPTION(optname, var, option, wasset)		\
	do {														\
		if (option.isset)										\
			var = option.values.real_val;						\
		else													\
			var = ((relopt_real *) option.gen)->default_val;	\
		(wasset) != NULL ? *(wasset) = option.isset : (dummyret) NULL; \
	} while (0)

/*
 * Note that this assumes that the variable is already allocated at the tail of
 * reloptions structure (StdRdOptions or equivalent).
 *
 * "base" is a pointer to the reloptions structure, and "offset" is an integer
 * variable that must be initialized to sizeof(reloptions structure).  This
 * struct must have been allocated with enough space to hold any string option
 * present, including terminating \0 for every option.  SET_VARSIZE() must be
 * called on the struct with this offset as the second argument, after all the
 * string options have been processed.
 */
#define HANDLE_STRING_RELOPTION(optname, var, option, base, offset, wasset) \
	do {														\
		relopt_string *optstring = (relopt_string *) option.gen;\
		char *string_val;										\
		if (option.isset)										\
			string_val = option.values.string_val;				\
		else if (!optstring->default_isnull)					\
			string_val = optstring->default_val;				\
		else													\
			string_val = NULL;									\
		(wasset) != NULL ? *(wasset) = option.isset : (dummyret) NULL; \
		if (string_val == NULL)									\
			var = 0;											\
		else													\
		{														\
			strcpy(((char *)(base)) + (offset), string_val);	\
			var = (offset);										\
			(offset) += strlen(string_val) + 1;					\
		}														\
	} while (0)

/*
 * For use during amoptions: get the strlen of a string option
 * (either default or the user defined value)
 */
#define GET_STRING_RELOPTION_LEN(option) \
	((option).isset ? strlen((option).values.string_val) : \
	 ((relopt_string *) (option).gen)->default_len)

/*
 * For use by code reading options already parsed: get a pointer to the string
 * value itself.  "optstruct" is the StdRdOption struct or equivalent, "member"
 * is the struct member corresponding to the string option
 */
#define GET_STRING_RELOPTION(optstruct, member) \
	((optstruct)->member == 0 ? NULL : \
	 (char *)(optstruct) + (optstruct)->member)


extern relopt_kind add_reloption_kind(void);
extern void add_bool_reloption(bits32 kinds, const char *name, const char *desc,
							   bool default_val);
extern void add_int_reloption(bits32 kinds, const char *name, const char *desc,
							  int default_val, int min_val, int max_val);
extern void add_real_reloption(bits32 kinds, const char *name, const char *desc,
							   double default_val, double min_val, double max_val);
extern void add_string_reloption(bits32 kinds, const char *name, const char *desc,
								 const char *default_val, validate_string_relopt validator);

extern Datum transformRelOptions(Datum oldOptions, List *defList,
								 const char *namspace, char *validnsps[],
								 bool acceptOidsOff, bool isReset);
extern List *untransformRelOptions(Datum options);
extern bytea *extractRelOptions(HeapTuple tuple, TupleDesc tupdesc,
								amoptions_function amoptions);
extern relopt_value *parseRelOptions(Datum options, bool validate,
									 relopt_kind kind, int *numrelopts);
extern void *allocateReloptStruct(Size base, relopt_value *options,
								  int numoptions);
extern void fillRelOptions(void *rdopts, Size basesize,
						   relopt_value *options, int numoptions,
						   bool validate,
						   const relopt_parse_elt *elems, int nelems);

extern bytea *default_reloptions(Datum reloptions, bool validate,
								 relopt_kind kind);
extern bytea *heap_reloptions(char relkind, Datum reloptions, bool validate);
extern bytea *view_reloptions(Datum reloptions, bool validate);
extern bytea *index_reloptions(amoptions_function amoptions, Datum reloptions,
							   bool validate);
extern bytea *attribute_reloptions(Datum reloptions, bool validate);
extern bytea *tablespace_reloptions(Datum reloptions, bool validate);
extern LOCKMODE AlterTableGetRelOptionsLockLevel(List *defList);

#endif							/* RELOPTIONS_H */
