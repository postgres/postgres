#include "postgres_fe.h"

#include "extern.h"

#define indicator_set ind_type != NULL && ind_type->type != ECPGt_NO_INDICATOR

struct ECPGstruct_member struct_no_indicator = {"no_indicator", &ecpg_no_indicator, NULL};

/* malloc + error check */
void *
mm_alloc(size_t size)
{
	void	   *ptr = malloc(size);

	if (ptr == NULL)
		mmerror(OUT_OF_MEMORY, ET_FATAL, "Out of memory\n");

	return ptr;
}

/* strdup + error check */
char *
mm_strdup(const char *string)
{
	char	   *new = strdup(string);

	if (new == NULL)
		mmerror(OUT_OF_MEMORY, ET_FATAL, "Out of memory\n");

	return new;
}

/* duplicate memberlist */
struct ECPGstruct_member *
ECPGstruct_member_dup(struct ECPGstruct_member * rm)
{
	struct ECPGstruct_member *new = NULL;

	while (rm)
	{
		struct ECPGtype *type;

		switch (rm->type->type)
		{
			case ECPGt_struct:
			case ECPGt_union:
				type = ECPGmake_struct_type(rm->type->u.members, rm->type->type, rm->type->struct_sizeof);
				break;
			case ECPGt_array:
				/* if this array does contain a struct again, we have to create the struct too */
				if (rm->type->u.element->type == ECPGt_struct)
					type = ECPGmake_struct_type(rm->type->u.element->u.members, rm->type->u.element->type, rm->type->u.element->struct_sizeof);
				else
					type = ECPGmake_array_type(ECPGmake_simple_type(rm->type->u.element->type, rm->type->u.element->size), rm->type->size);
				break;
			default:
				type = ECPGmake_simple_type(rm->type->type, rm->type->size);
				break;
		}

		ECPGmake_struct_member(rm->name, type, &new);

		rm = rm->next;
	}

	return (new);
}

/* The NAME argument is copied. The type argument is preserved as a pointer. */
void
ECPGmake_struct_member(char *name, struct ECPGtype * type, struct ECPGstruct_member ** start)
{
	struct ECPGstruct_member *ptr,
			   *ne =
	(struct ECPGstruct_member *) mm_alloc(sizeof(struct ECPGstruct_member));

	ne->name = strdup(name);
	ne->type = type;
	ne->next = NULL;

	for (ptr = *start; ptr && ptr->next; ptr = ptr->next);

	if (ptr)
		ptr->next = ne;
	else
		*start = ne;
}

struct ECPGtype *
ECPGmake_simple_type(enum ECPGttype type, char *size)
{
	struct ECPGtype *ne = (struct ECPGtype *) mm_alloc(sizeof(struct ECPGtype));

	ne->type = type;
	ne->size = size;
	ne->u.element = 0;
	ne->struct_sizeof = NULL;

	return ne;
}

struct ECPGtype *
ECPGmake_array_type(struct ECPGtype * type, char *size)
{
	struct ECPGtype *ne = ECPGmake_simple_type(ECPGt_array, size);

	ne->u.element = type;

	return ne;
}

struct ECPGtype *
ECPGmake_struct_type(struct ECPGstruct_member * rm, enum ECPGttype type, char *struct_sizeof)
{
	struct ECPGtype *ne = ECPGmake_simple_type(type, make_str("1"));

	ne->u.members = ECPGstruct_member_dup(rm);
	ne->struct_sizeof = struct_sizeof;

	return ne;
}

static const char *
get_type(enum ECPGttype type)
{
	switch (type)
	{
		case ECPGt_char:
			return ("ECPGt_char");
			break;
		case ECPGt_unsigned_char:
			return ("ECPGt_unsigned_char");
			break;
		case ECPGt_short:
			return ("ECPGt_short");
			break;
		case ECPGt_unsigned_short:
			return ("ECPGt_unsigned_short");
			break;
		case ECPGt_int:
			return ("ECPGt_int");
			break;
		case ECPGt_unsigned_int:
			return ("ECPGt_unsigned_int");
			break;
		case ECPGt_long:
			return ("ECPGt_long");
			break;
		case ECPGt_unsigned_long:
			return ("ECPGt_unsigned_long");
			break;
		case ECPGt_long_long:
			return ("ECPGt_long_long");
			break;
		case ECPGt_unsigned_long_long:
			return ("ECPGt_unsigned_long_long");
			break;
		case ECPGt_float:
			return ("ECPGt_float");
			break;
		case ECPGt_double:
			return ("ECPGt_double");
			break;
		case ECPGt_bool:
			return ("ECPGt_bool");
			break;
		case ECPGt_varchar:
			return ("ECPGt_varchar");
		case ECPGt_NO_INDICATOR:		/* no indicator */
			return ("ECPGt_NO_INDICATOR");
			break;
		case ECPGt_char_variable:		/* string that should not be
										 * quoted */
			return ("ECPGt_char_variable");
			break;
		case ECPGt_const:		/* constant string quoted */
			return ("ECPGt_const");
			break;
		case ECPGt_decimal:
			return ("ECPGt_decimal");
			break;
		case ECPGt_numeric:
			return ("ECPGt_numeric");
			break;
		case ECPGt_interval:
			return ("ECPGt_interval");
			break;
		case ECPGt_descriptor:
			return ("ECPGt_descriptor");
			break;
		case ECPGt_date:
			return ("ECPGt_date");
			break;
		case ECPGt_timestamp:
			return ("ECPGt_timestamp");
			break;
		default:
			sprintf(errortext, "illegal variable type %d\n", type);
			yyerror(errortext);
	}

	return NULL;
}

/* Dump a type.
   The type is dumped as:
   type-tag <comma>				   - enum ECPGttype
   reference-to-variable <comma>		   - char *
   size <comma>					   - long size of this field (if varchar)
   arrsize <comma>				   - long number of elements in the arr
   offset <comma>				   - offset to the next element
   Where:
   type-tag is one of the simple types or varchar.
   reference-to-variable can be a reference to a struct element.
   arrsize is the size of the array in case of array fetches. Otherwise 0.
   size is the maxsize in case it is a varchar. Otherwise it is the size of
   the variable (required to do array fetches of structs).
 */
static void ECPGdump_a_simple(FILE *o, const char *name, enum ECPGttype type,
				  char *varcharsize,
				  char *arrsiz, const char *siz, const char *prefix);
static void ECPGdump_a_struct(FILE *o, const char *name, const char *ind_name, char *arrsiz,
				  struct ECPGtype * type, struct ECPGtype * ind_type, const char *offset, const char *prefix, const char *ind_prefix);

void
ECPGdump_a_type(FILE *o, const char *name, struct ECPGtype * type,
				const char *ind_name, struct ECPGtype * ind_type,
				const char *prefix, const char *ind_prefix,
				char *arr_str_siz, const char *struct_sizeof,
				const char *ind_struct_sizeof)
{
	switch (type->type)
	{
		case ECPGt_array:
			if (indicator_set && ind_type->type != ECPGt_array)
				mmerror(INDICATOR_NOT_ARRAY, ET_FATAL, "Indicator for array/pointer has to be array/pointer.\n");
			switch (type->u.element->type)
			{
				case ECPGt_array:
					mmerror(PARSE_ERROR, ET_ERROR, "No nested arrays allowed (except strings)");		/* array of array */
					break;
				case ECPGt_struct:
				case ECPGt_union:
					ECPGdump_a_struct(o, name,
									  ind_name,
									  type->size,
									  type->u.element,
									  (ind_type->type == ECPGt_NO_INDICATOR) ? ind_type : ind_type->u.element,
									  NULL, prefix, ind_prefix);
					break;
				default:
					if (!IS_SIMPLE_TYPE(type->u.element->type))
						yyerror("Internal error: unknown datatype, please inform pgsql-bugs@postgresql.org");

					ECPGdump_a_simple(o, name,
									  type->u.element->type,
						type->u.element->size, type->size, NULL, prefix);

					if (ind_type != NULL)
					{
						if (ind_type->type == ECPGt_NO_INDICATOR)
							ECPGdump_a_simple(o, ind_name, ind_type->type, ind_type->size, make_str("-1"), NULL, ind_prefix);
						else
						{
							ECPGdump_a_simple(o, ind_name, ind_type->u.element->type,
											  ind_type->u.element->size, ind_type->size, NULL, ind_prefix);
						}
					}
			}
			break;
		case ECPGt_struct:
			if (indicator_set && ind_type->type != ECPGt_struct)
				mmerror(INDICATOR_NOT_STRUCT, ET_FATAL, "Indicator for struct has to be struct.\n");

			ECPGdump_a_struct(o, name, ind_name, make_str("1"), type, ind_type, NULL, prefix, ind_prefix);
			break;
		case ECPGt_union:		/* cannot dump a complete union */
			yyerror("Type of union has to be specified");
			break;
		case ECPGt_char_variable:
			if (indicator_set && (ind_type->type == ECPGt_struct || ind_type->type == ECPGt_array))
				mmerror(INDICATOR_NOT_SIMPLE, ET_FATAL, "Indicator for simple datatype has to be simple.\n");

			ECPGdump_a_simple(o, name, type->type, make_str("1"), (arr_str_siz && strcmp(arr_str_siz, "0") != 0) ? arr_str_siz : make_str("1"), struct_sizeof, prefix);
			ECPGdump_a_simple(o, ind_name, ind_type->type, ind_type->size, (arr_str_siz && strcmp(arr_str_siz, "0") != 0) ? arr_str_siz : make_str("-1"), ind_struct_sizeof, ind_prefix);
			break;
		case ECPGt_descriptor:
			if (indicator_set && (ind_type->type == ECPGt_struct || ind_type->type == ECPGt_array))
				mmerror(INDICATOR_NOT_SIMPLE, ET_FATAL, "Indicator for simple datatype has to be simple.\n");

			ECPGdump_a_simple(o, name, type->type, 0, make_str("-1"), NULL, prefix);
			ECPGdump_a_simple(o, ind_name, ind_type->type, ind_type->size, make_str("-1"), NULL, ind_prefix);
			break;
		default:
			if (indicator_set && (ind_type->type == ECPGt_struct || ind_type->type == ECPGt_array))
				mmerror(INDICATOR_NOT_SIMPLE, ET_FATAL, "Indicator for simple datatype has to be simple.\n");

			ECPGdump_a_simple(o, name, type->type, type->size, (arr_str_siz && strcmp(arr_str_siz, "0") != 0) ? arr_str_siz : make_str("-1"), struct_sizeof, prefix);
			if (ind_type != NULL)
				ECPGdump_a_simple(o, ind_name, ind_type->type, ind_type->size, (arr_str_siz && strcmp(arr_str_siz, "0") != 0) ? arr_str_siz : make_str("-1"), ind_struct_sizeof, ind_prefix);
			break;
	}
}


/* If siz is NULL, then the offset is 0, if not use siz as a
   string, it represents the offset needed if we are in an array of structs. */
static void
ECPGdump_a_simple(FILE *o, const char *name, enum ECPGttype type,
				  char *varcharsize,
				  char *arrsize,
				  const char *siz,
				  const char *prefix
)
{
	if (type == ECPGt_NO_INDICATOR)
		fprintf(o, "\n\tECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ");
	else if (type == ECPGt_descriptor)
		/* remember that name here already contains quotes (if needed) */
		fprintf(o, "\n\tECPGt_descriptor, %s, 0L, 0L, 0L, ", name);
	else
	{
		char	   *variable = (char *) mm_alloc(strlen(name) + ((prefix == NULL) ? 0 : strlen(prefix)) + 4);
		char	   *offset = (char *) mm_alloc(strlen(name) + strlen("sizeof(struct varchar_)") + 1 + strlen(varcharsize));

		switch (type)
		{
				/*
				 * we have to use the & operator except for arrays and
				 * pointers
				 */

			case ECPGt_varchar:

				/*
				 * we have to use the pointer except for arrays with given
				 * bounds
				 */
				if (((atoi(arrsize) > 0) ||
					 (atoi(arrsize) == 0 && strcmp(arrsize, "0") != 0)) &&
					siz == NULL)
					sprintf(variable, "(%s%s)", prefix ? prefix : "", name);
				else
					sprintf(variable, "&(%s%s)", prefix ? prefix : "", name);

				sprintf(offset, "sizeof(struct varchar_%s)", name);
				break;
			case ECPGt_char:
			case ECPGt_unsigned_char:
			case ECPGt_char_variable:

				/*
				 * we have to use the pointer except for arrays with given
				 * bounds, ecpglib will distinguish between * and []
				 */
				if ((atoi(varcharsize) > 1 ||
					 (atoi(arrsize) > 0) ||
					 (atoi(varcharsize) == 0 && strcmp(varcharsize, "0") != 0) ||
					 (atoi(arrsize) == 0 && strcmp(arrsize, "0") != 0))
					&& siz == NULL)
					sprintf(variable, "(%s%s)", prefix ? prefix : "", name);
				else
					sprintf(variable, "&(%s%s)", prefix ? prefix : "", name);

				sprintf(offset, "%s*sizeof(char)", strcmp(varcharsize, "0") == 0 ? "1" : varcharsize);
				break;
			case ECPGt_numeric:

				/*
				 * we have to use a pointer here
				 */
				sprintf(variable, "&(%s%s)", prefix ? prefix : "", name);
				sprintf(offset, "sizeof(numeric)");
				break;
			case ECPGt_interval:

				/*
				 * we have to use a pointer here
				 */
				sprintf(variable, "&(%s%s)", prefix ? prefix : "", name);
				sprintf(offset, "sizeof(interval)");
				break;
			case ECPGt_date:

				/*
				 * we have to use a pointer and translate the variable
				 * type
				 */
				sprintf(variable, "&(%s%s)", prefix ? prefix : "", name);
				sprintf(offset, "sizeof(date)");
				break;
			case ECPGt_timestamp:

				/*
				 * we have to use a pointer and translate the variable
				 * type
				 */
				sprintf(variable, "&(%s%s)", prefix ? prefix : "", name);
				sprintf(offset, "sizeof(timestamp)");
				break;
			case ECPGt_const:

				/*
				 * just dump the const as string
				 */
				sprintf(variable, "\"%s\"", name);
				sprintf(offset, "strlen(\"%s\")", name);
				break;
			default:

				/*
				 * we have to use the pointer except for arrays with given
				 * bounds
				 */
				if (((atoi(arrsize) > 0) ||
					 (atoi(arrsize) == 0 && strcmp(arrsize, "0") != 0)) &&
					siz == NULL)
					sprintf(variable, "(%s%s)", prefix ? prefix : "", name);
				else
					sprintf(variable, "&(%s%s)", prefix ? prefix : "", name);

				sprintf(offset, "sizeof(%s)", ECPGtype_name(type));
				break;
		}

		if (atoi(arrsize) < 0)
			strcpy(arrsize, "1");

		if (siz == NULL || strcmp(arrsize, "0") == 0 || strcmp(arrsize, "1") == 0)
			fprintf(o, "\n\t%s,%s,(long)%s,(long)%s,%s, ", get_type(type), variable, varcharsize, arrsize, offset);
		else
			fprintf(o, "\n\t%s,%s,(long)%s,(long)%s,%s, ", get_type(type), variable, varcharsize, arrsize, siz);

		free(variable);
		free(offset);
	}
}


/* Penetrate a struct and dump the contents. */
static void
ECPGdump_a_struct(FILE *o, const char *name, const char *ind_name, char *arrsiz, struct ECPGtype * type, struct ECPGtype * ind_type, const char *offsetarg, const char *prefix, const char *ind_prefix)
{
	/*
	 * If offset is NULL, then this is the first recursive level. If not
	 * then we are in a struct in a struct and the offset is used as
	 * offset.
	 */
	struct ECPGstruct_member *p,
			   *ind_p = NULL;
	char		obuf[BUFSIZ];
	char		pbuf[BUFSIZ],
				ind_pbuf[BUFSIZ];
	const char *offset;

	if (offsetarg == NULL)
	{
		sprintf(obuf, "sizeof(%s)", name);
		offset = obuf;
	}
	else
		offset = offsetarg;

	if (atoi(arrsiz) == 1)
		sprintf(pbuf, "%s%s.", prefix ? prefix : "", name);
	else
		sprintf(pbuf, "%s%s->", prefix ? prefix : "", name);

	prefix = pbuf;

	if (ind_type == &ecpg_no_indicator)
		ind_p = &struct_no_indicator;
	else if (ind_type != NULL)
	{
		if (atoi(arrsiz) == 1)
			sprintf(ind_pbuf, "%s%s.", ind_prefix ? ind_prefix : "", ind_name);
		else
			sprintf(ind_pbuf, "%s%s->", ind_prefix ? ind_prefix : "", ind_name);

		ind_prefix = ind_pbuf;
		ind_p = ind_type->u.members;
	}

	for (p = type->u.members; p; p = p->next)
	{
		ECPGdump_a_type(o, p->name, p->type,
						(ind_p != NULL) ? ind_p->name : NULL,
						(ind_p != NULL) ? ind_p->type : NULL,
						prefix, ind_prefix, arrsiz, type->struct_sizeof,
						(ind_p != NULL) ? ind_type->struct_sizeof : NULL);
		if (ind_p != NULL && ind_p != &struct_no_indicator)
			ind_p = ind_p->next;
	}
}

void
ECPGfree_struct_member(struct ECPGstruct_member * rm)
{
	while (rm)
	{
		struct ECPGstruct_member *p = rm;

		rm = rm->next;
		free(p->name);
		free(p->type);
		free(p);
	}
}

void
ECPGfree_type(struct ECPGtype * type)
{
	if (!IS_SIMPLE_TYPE(type->type))
	{
		switch (type->type)
		{
			case ECPGt_array:
				switch (type->u.element->type)
				{
					case ECPGt_array:
						yyerror("internal error, found multidimensional array\n");
						break;
					case ECPGt_struct:
					case ECPGt_union:
						/* Array of structs. */
						ECPGfree_struct_member(type->u.element->u.members);
						free(type->u.element);
						break;
					default:
						if (!IS_SIMPLE_TYPE(type->u.element->type))
							yyerror("Internal error: unknown datatype, please inform pgsql-bugs@postgresql.org");

						free(type->u.element);
				}
				break;
			case ECPGt_struct:
			case ECPGt_union:
				ECPGfree_struct_member(type->u.members);
				break;
			default:
				sprintf(errortext, "illegal variable type %d\n", type->type);
				yyerror(errortext);
				break;
		}
	}
	free(type);
}

const char *
get_dtype(enum ECPGdtype type)
{
	switch (type)
	{
		case ECPGd_count:
			return ("ECPGd_countr");
			break;
		case ECPGd_data:
			return ("ECPGd_data");
			break;
		case ECPGd_di_code:
			return ("ECPGd_di_code");
			break;
		case ECPGd_di_precision:
			return ("ECPGd_di_precision");
			break;
		case ECPGd_indicator:
			return ("ECPGd_indicator");
			break;
		case ECPGd_key_member:
			return ("ECPGd_key_member");
			break;
		case ECPGd_length:
			return ("ECPGd_length");
			break;
		case ECPGd_name:
			return ("ECPGd_name");
			break;
		case ECPGd_nullable:
			return ("ECPGd_nullable");
			break;
		case ECPGd_octet:
			return ("ECPGd_octet");
			break;
		case ECPGd_precision:
			return ("ECPGd_precision");
			break;
		case ECPGd_ret_length:
			return ("ECPGd_ret_length");
		case ECPGd_ret_octet:
			return ("ECPGd_ret_octet");
			break;
		case ECPGd_scale:
			return ("ECPGd_scale");
			break;
		case ECPGd_type:
			return ("ECPGd_type");
			break;
		case ECPGd_cardinality:
			return ("ECPGd_cardinality");
		default:
			sprintf(errortext, "illegal descriptor item %d\n", type);
			yyerror(errortext);
	}

	return NULL;
}
