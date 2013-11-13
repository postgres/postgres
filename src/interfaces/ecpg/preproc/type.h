/*
 * src/interfaces/ecpg/preproc/type.h
 */
#ifndef _ECPG_PREPROC_TYPE_H
#define _ECPG_PREPROC_TYPE_H

#include "ecpgtype.h"

struct ECPGtype;
struct ECPGstruct_member
{
	char	   *name;
	struct ECPGtype *type;
	struct ECPGstruct_member *next;
};

struct ECPGtype
{
	enum ECPGttype type;
	char	   *type_name;		/* For struct and union types it is the struct
								 * name */
	char	   *size;			/* For array it is the number of elements. For
								 * varchar it is the maxsize of the area. */
	char	   *struct_sizeof;	/* For a struct this is the sizeof() type as
								 * string */
	union
	{
		struct ECPGtype *element;		/* For an array this is the type of
										 * the element */
		struct ECPGstruct_member *members;		/* A pointer to a list of
												 * members. */
	}			u;
	int			counter;
};

/* Everything is malloced. */
void		ECPGmake_struct_member(char *, struct ECPGtype *, struct ECPGstruct_member **);
struct ECPGtype *ECPGmake_simple_type(enum ECPGttype, char *, int);
struct ECPGtype *ECPGmake_array_type(struct ECPGtype *, char *);
struct ECPGtype *ECPGmake_struct_type(struct ECPGstruct_member *, enum ECPGttype, char *, char *);
struct ECPGstruct_member *ECPGstruct_member_dup(struct ECPGstruct_member *);

/* Frees a type. */
void		ECPGfree_struct_member(struct ECPGstruct_member *);
void		ECPGfree_type(struct ECPGtype *);

/* Dump a type.
   The type is dumped as:
   type-tag <comma> reference-to-variable <comma> arrsize <comma> size <comma>
   Where:
   type-tag is one of the simple types or varchar.
   reference-to-variable can be a reference to a struct element.
   arrsize is the size of the array in case of array fetches. Otherwise 0.
   size is the maxsize in case it is a varchar. Otherwise it is the size of
	   the variable (required to do array fetches of structs).
 */
void ECPGdump_a_type(FILE *, const char *, struct ECPGtype *, const int,
				const char *, struct ECPGtype *, const int,
				const char *, const char *, char *,
				const char *, const char *);

/* A simple struct to keep a variable and its type. */
struct ECPGtemp_type
{
	struct ECPGtype *type;
	const char *name;
};

extern const char *ecpg_type_name(enum ECPGttype type);

/* some stuff for whenever statements */
enum WHEN_TYPE
{
	W_NOTHING,
	W_CONTINUE,
	W_BREAK,
	W_SQLPRINT,
	W_GOTO,
	W_DO,
	W_STOP
};

struct when
{
	enum WHEN_TYPE code;
	char	   *command;
	char	   *str;
};

struct index
{
	char	   *index1;
	char	   *index2;
	char	   *str;
};

struct su_symbol
{
	char	   *su;
	char	   *symbol;
};

struct prep
{
	char	   *name;
	char	   *stmt;
	char	   *type;
};

struct this_type
{
	enum ECPGttype type_enum;
	char	   *type_str;
	char	   *type_dimension;
	char	   *type_index;
	char	   *type_sizeof;
};

struct _include_path
{
	char	   *path;
	struct _include_path *next;
};

struct cursor
{
	char	   *name;
	char	   *function;
	char	   *command;
	char	   *connection;
	bool		opened;
	struct arguments *argsinsert;
	struct arguments *argsinsert_oos;
	struct arguments *argsresult;
	struct arguments *argsresult_oos;
	struct cursor *next;
};

struct typedefs
{
	char	   *name;
	struct this_type *type;
	struct ECPGstruct_member *struct_member_list;
	int			brace_level;
	struct typedefs *next;
};

struct _defines
{
	char	   *old;
	char	   *new;
	int			pertinent;
	void	   *used;
	struct _defines *next;
};

/* This is a linked list of the variable names and types. */
struct variable
{
	char	   *name;
	struct ECPGtype *type;
	int			brace_level;
	struct variable *next;
};

struct arguments
{
	struct variable *variable;
	struct variable *indicator;
	struct arguments *next;
};

struct descriptor
{
	char	   *name;
	char	   *connection;
	struct descriptor *next;
};

struct assignment
{
	char	   *variable;
	enum ECPGdtype value;
	struct assignment *next;
};

enum errortype
{
	ET_WARNING, ET_ERROR
};

struct fetch_desc
{
	char	   *str;
	char	   *name;
};

#endif   /* _ECPG_PREPROC_TYPE_H */
