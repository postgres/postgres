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
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
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
	RELOPT_TYPE_ENUM,
	RELOPT_TYPE_STRING
} relopt_type;

/* kinds supported by reloptions */
typedef enum relopt_kind
{
	RELOPT_KIND_LOCAL = 0,
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
		int			enum_val;
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

/*
 * relopt_enum_elt_def -- One member of the array of acceptable values
 * of an enum reloption.
 */
typedef struct relopt_enum_elt_def
{
	const char *string_val;
	int			symbol_val;
} relopt_enum_elt_def;

typedef struct relopt_enum
{
	relopt_gen	gen;
	relopt_enum_elt_def *members;
	int			default_val;
	const char *detailmsg;
	/* null-terminated array of members */
} relopt_enum;

/* validation routines for strings */
typedef void (*validate_string_relopt) (const char *value);
typedef Size (*fill_string_relopt) (const char *value, void *ptr);

/* validation routine for the whole option set */
typedef void (*relopts_validator) (void *parsed_options, relopt_value *vals, int nvals);

typedef struct relopt_string
{
	relopt_gen	gen;
	int			default_len;
	bool		default_isnull;
	validate_string_relopt validate_cb;
	fill_string_relopt fill_cb;
	char	   *default_val;
} relopt_string;

/* This is the table datatype for build_reloptions() */
typedef struct
{
	const char *optname;		/* option's name */
	relopt_type opttype;		/* option's datatype */
	int			offset;			/* offset of field in result struct */
} relopt_parse_elt;

/* Local reloption definition */
typedef struct local_relopt
{
	relopt_gen *option;			/* option definition */
	int			offset;			/* offset of parsed value in bytea structure */
} local_relopt;

/* Structure to hold local reloption data for build_local_reloptions() */
typedef struct local_relopts
{
	List	   *options;		/* list of local_relopt definitions */
	List	   *validators;		/* list of relopts_validator callbacks */
	Size		relopt_struct_size; /* size of parsed bytea structure */
} local_relopts;

/*
 * Utility macro to get a value for a string reloption once the options
 * are parsed.  This gets a pointer to the string value itself.  "optstruct"
 * is the StdRdOptions struct or equivalent, "member" is the struct member
 * corresponding to the string option.
 */
#define GET_STRING_RELOPTION(optstruct, member) \
	((optstruct)->member == 0 ? NULL : \
	 (char *)(optstruct) + (optstruct)->member)

extern relopt_kind add_reloption_kind(void);
extern void add_bool_reloption(bits32 kinds, const char *name, const char *desc,
							   bool default_val, LOCKMODE lockmode);
extern void add_int_reloption(bits32 kinds, const char *name, const char *desc,
							  int default_val, int min_val, int max_val,
							  LOCKMODE lockmode);
extern void add_real_reloption(bits32 kinds, const char *name, const char *desc,
							   double default_val, double min_val, double max_val,
							   LOCKMODE lockmode);
extern void add_enum_reloption(bits32 kinds, const char *name, const char *desc,
							   relopt_enum_elt_def *members, int default_val,
							   const char *detailmsg, LOCKMODE lockmode);
extern void add_string_reloption(bits32 kinds, const char *name, const char *desc,
								 const char *default_val, validate_string_relopt validator,
								 LOCKMODE lockmode);

extern void init_local_reloptions(local_relopts *relopts, Size relopt_struct_size);
extern void register_reloptions_validator(local_relopts *relopts,
										  relopts_validator validator);
extern void add_local_bool_reloption(local_relopts *relopts, const char *name,
									 const char *desc, bool default_val,
									 int offset);
extern void add_local_int_reloption(local_relopts *relopts, const char *name,
									const char *desc, int default_val,
									int min_val, int max_val, int offset);
extern void add_local_real_reloption(local_relopts *relopts, const char *name,
									 const char *desc, double default_val,
									 double min_val, double max_val,
									 int offset);
extern void add_local_enum_reloption(local_relopts *relopts,
									 const char *name, const char *desc,
									 relopt_enum_elt_def *members,
									 int default_val, const char *detailmsg,
									 int offset);
extern void add_local_string_reloption(local_relopts *relopts, const char *name,
									   const char *desc,
									   const char *default_val,
									   validate_string_relopt validator,
									   fill_string_relopt filler, int offset);

extern Datum transformRelOptions(Datum oldOptions, List *defList,
								 const char *namspace, char *validnsps[],
								 bool acceptOidsOff, bool isReset);
extern List *untransformRelOptions(Datum options);
extern bytea *extractRelOptions(HeapTuple tuple, TupleDesc tupdesc,
								amoptions_function amoptions);
extern void *build_reloptions(Datum reloptions, bool validate,
							  relopt_kind kind,
							  Size relopt_struct_size,
							  const relopt_parse_elt *relopt_elems,
							  int num_relopt_elems);
extern void *build_local_reloptions(local_relopts *relopts, Datum options,
									bool validate);

extern bytea *default_reloptions(Datum reloptions, bool validate,
								 relopt_kind kind);
extern bytea *heap_reloptions(char relkind, Datum reloptions, bool validate);
extern bytea *view_reloptions(Datum reloptions, bool validate);
extern bytea *partitioned_table_reloptions(Datum reloptions, bool validate);
extern bytea *index_reloptions(amoptions_function amoptions, Datum reloptions,
							   bool validate);
extern bytea *attribute_reloptions(Datum reloptions, bool validate);
extern bytea *tablespace_reloptions(Datum reloptions, bool validate);
extern LOCKMODE AlterTableGetRelOptionsLockLevel(List *defList);

#endif							/* RELOPTIONS_H */
