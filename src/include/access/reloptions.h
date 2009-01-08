/*-------------------------------------------------------------------------
 *
 * reloptions.h
 *	  Core support for relation options (pg_class.reloptions)
 *
 * Note: the functions dealing with text-array reloptions values declare
 * them as Datum, not ArrayType *, to avoid needing to include array.h
 * into a lot of low-level code.
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/reloptions.h,v 1.9 2009/01/08 19:34:41 alvherre Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELOPTIONS_H
#define RELOPTIONS_H

#include "nodes/pg_list.h"

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
	RELOPT_KIND_HEAP,
	/* XXX do we need a separate kind for TOAST tables? */
	RELOPT_KIND_BTREE,
	RELOPT_KIND_HASH,
	RELOPT_KIND_GIN,
	RELOPT_KIND_GIST,
	/* if you add a new kind, make sure you update "last_default" too */
	RELOPT_KIND_LAST_DEFAULT = RELOPT_KIND_GIST,
	RELOPT_KIND_MAX = 255
} relopt_kind;

/* generic struct to hold shared data */
typedef struct relopt_gen
{
	const char *name;	/* must be first (used as list termination marker) */
	const char *desc;
	relopt_kind	kind;
	int			namelen;
	relopt_type	type;
} relopt_gen;

/* holds a parsed value */
typedef struct relopt_value
{
	relopt_gen *gen;
	bool		isset;
	union
	{
		bool	bool_val;
		int		int_val;
		double	real_val;
		char   *string_val;	/* allocated separately */
	} values;
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

typedef void (*validate_string_relopt) (char *value, bool validate);

typedef struct relopt_string
{
	relopt_gen	gen;
	int			default_len;
	bool		default_isnull;
	validate_string_relopt	validate_cb;
	char		default_val[1];	/* variable length */
} relopt_string;

/*
 * These macros exist for the convenience of amoptions writers.  See
 * default_reloptions for an example of the intended usage.  Beware of
 * multiple evaluation of arguments!
 *
 * Most of the time there's no need to call HAVE_RELOPTION manually, but it's
 * possible that an amoptions routine needs to walk the array with a different
 * purpose (say, to compute the size of a struct to allocate beforehand.)
 *
 * The last argument in the HANDLE_*_RELOPTION macros allows the caller to
 * determine whether the option was set (true), or its value acquired from
 * defaults (false); it can be passed as (char *) NULL if the caller does not
 * need this information.
 */
#define HAVE_RELOPTION(optname, option) \
	(pg_strncasecmp(option.gen->name, optname, option.gen->namelen + 1) == 0)

#define HANDLE_INT_RELOPTION(optname, var, option, wasset)			\
	do {															\
		if (HAVE_RELOPTION(optname, option))						\
		{															\
			if (option.isset)										\
				var = option.values.int_val; 						\
			else													\
				var = ((relopt_int *) option.gen)->default_val; 	\
			(wasset) != NULL ? *(wasset) = option.isset : (dummyret)NULL; \
			continue;												\
		}															\
	} while (0)

#define HANDLE_BOOL_RELOPTION(optname, var, option, wasset)			\
	do {															\
		if (HAVE_RELOPTION(optname, option))						\
		{															\
			if (option.isset)										\
				var = option.values.bool_val; 						\
			else													\
				var = ((relopt_bool *) option.gen)->default_val;	\
			(wasset) != NULL ? *(wasset) = option.isset : (dummyret) NULL; \
			continue;												\
		}															\
	} while (0)

#define HANDLE_REAL_RELOPTION(optname, var, option, wasset) 		\
	do {															\
		if (HAVE_RELOPTION(optname, option))						\
		{															\
			if (option.isset)										\
				var = option.values.real_val; 						\
			else													\
				var = ((relopt_real *) option.gen)->default_val;	\
			(wasset) != NULL ? *(wasset) = option.isset : (dummyret) NULL; \
			continue;												\
		}															\
	} while (0)

/*
 * Note that this assumes that the variable is already allocated at the tail of
 * reloptions structure (StdRdOptions or other).
 *
 * "base" is a pointer to the reloptions structure, and "offset" is an integer
 * variable that must be initialized to sizeof(reloptions structure).  This
 * struct must have been allocated with enough space to hold any string option
 * present, including terminating \0 for every option.  SET_VARSIZE() must be
 * called on the struct with this offset as the second argument, after all the
 * string options have been processed.
 */
#define HANDLE_STRING_RELOPTION(optname, var, option, base, offset, wasset)	\
	do {														\
		if (HAVE_RELOPTION(optname, option))						\
		{															\
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
			continue;												\
		}															\
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


extern int add_reloption_kind(void);
extern void add_bool_reloption(int kind, char *name, char *desc,
				   bool default_val);
extern void add_int_reloption(int kind, char *name, char *desc,
				  int default_val, int min_val, int max_val);
extern void add_real_reloption(int kind, char *name, char *desc,
				   double default_val, double min_val, double max_val);
extern void add_string_reloption(int kind, char *name, char *desc,
					 char *default_val, validate_string_relopt validator);
			
extern Datum transformRelOptions(Datum oldOptions, List *defList,
					bool ignoreOids, bool isReset);
extern List *untransformRelOptions(Datum options);
extern relopt_value *parseRelOptions(Datum options, bool validate,
				relopt_kind kind, int *numrelopts);

extern bytea *default_reloptions(Datum reloptions, bool validate,
				   relopt_kind kind);
extern bytea *heap_reloptions(char relkind, Datum reloptions, bool validate);
extern bytea *index_reloptions(RegProcedure amoptions, Datum reloptions,
				bool validate);

#endif   /* RELOPTIONS_H */
