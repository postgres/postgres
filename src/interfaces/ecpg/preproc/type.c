/* src/interfaces/ecpg/preproc/type.c */

#include "postgres_fe.h"

#include "preproc_extern.h"

#define indicator_set ind_type != NULL && ind_type->type != ECPGt_NO_INDICATOR

static struct ECPGstruct_member struct_no_indicator = {"no_indicator", &ecpg_no_indicator, NULL};

/* duplicate memberlist */
struct ECPGstruct_member *
ECPGstruct_member_dup(struct ECPGstruct_member *rm)
{
	struct ECPGstruct_member *new = NULL;

	while (rm)
	{
		struct ECPGtype *type;

		switch (rm->type->type)
		{
			case ECPGt_struct:
			case ECPGt_union:
				type = ECPGmake_struct_type(rm->type->u.members, rm->type->type, rm->type->type_name, rm->type->struct_sizeof);
				break;
			case ECPGt_array:

				/*
				 * if this array does contain a struct again, we have to
				 * create the struct too
				 */
				if (rm->type->u.element->type == ECPGt_struct || rm->type->u.element->type == ECPGt_union)
					type = ECPGmake_struct_type(rm->type->u.element->u.members, rm->type->u.element->type, rm->type->u.element->type_name, rm->type->u.element->struct_sizeof);
				else
					type = ECPGmake_array_type(ECPGmake_simple_type(rm->type->u.element->type, rm->type->u.element->size, rm->type->u.element->counter), rm->type->size);
				break;
			default:
				type = ECPGmake_simple_type(rm->type->type, rm->type->size, rm->type->counter);
				break;
		}

		ECPGmake_struct_member(rm->name, type, &new);

		rm = rm->next;
	}

	return new;
}

/* The NAME argument is copied. The type argument is preserved as a pointer. */
void
ECPGmake_struct_member(const char *name, struct ECPGtype *type, struct ECPGstruct_member **start)
{
	struct ECPGstruct_member *ptr,
			   *ne =
		(struct ECPGstruct_member *) mm_alloc(sizeof(struct ECPGstruct_member));

	ne->name = mm_strdup(name);
	ne->type = type;
	ne->next = NULL;

	for (ptr = *start; ptr && ptr->next; ptr = ptr->next);

	if (ptr)
		ptr->next = ne;
	else
		*start = ne;
}

struct ECPGtype *
ECPGmake_simple_type(enum ECPGttype type, const char *size, int counter)
{
	struct ECPGtype *ne = (struct ECPGtype *) mm_alloc(sizeof(struct ECPGtype));

	ne->type = type;
	ne->type_name = NULL;
	ne->size = mm_strdup(size);
	ne->u.element = NULL;
	ne->struct_sizeof = NULL;
	ne->counter = counter;		/* only needed for varchar and bytea */

	return ne;
}

struct ECPGtype *
ECPGmake_array_type(struct ECPGtype *type, const char *size)
{
	struct ECPGtype *ne = ECPGmake_simple_type(ECPGt_array, size, 0);

	ne->u.element = type;

	return ne;
}

struct ECPGtype *
ECPGmake_struct_type(struct ECPGstruct_member *rm, enum ECPGttype type, char *type_name, char *struct_sizeof)
{
	struct ECPGtype *ne = ECPGmake_simple_type(type, "1", 0);

	ne->type_name = mm_strdup(type_name);
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
			return "ECPGt_char";
			break;
		case ECPGt_unsigned_char:
			return "ECPGt_unsigned_char";
			break;
		case ECPGt_short:
			return "ECPGt_short";
			break;
		case ECPGt_unsigned_short:
			return "ECPGt_unsigned_short";
			break;
		case ECPGt_int:
			return "ECPGt_int";
			break;
		case ECPGt_unsigned_int:
			return "ECPGt_unsigned_int";
			break;
		case ECPGt_long:
			return "ECPGt_long";
			break;
		case ECPGt_unsigned_long:
			return "ECPGt_unsigned_long";
			break;
		case ECPGt_long_long:
			return "ECPGt_long_long";
			break;
		case ECPGt_unsigned_long_long:
			return "ECPGt_unsigned_long_long";
			break;
		case ECPGt_float:
			return "ECPGt_float";
			break;
		case ECPGt_double:
			return "ECPGt_double";
			break;
		case ECPGt_bool:
			return "ECPGt_bool";
			break;
		case ECPGt_varchar:
			return "ECPGt_varchar";
		case ECPGt_bytea:
			return "ECPGt_bytea";
		case ECPGt_NO_INDICATOR:	/* no indicator */
			return "ECPGt_NO_INDICATOR";
			break;
		case ECPGt_char_variable:	/* string that should not be quoted */
			return "ECPGt_char_variable";
			break;
		case ECPGt_const:		/* constant string quoted */
			return "ECPGt_const";
			break;
		case ECPGt_decimal:
			return "ECPGt_decimal";
			break;
		case ECPGt_numeric:
			return "ECPGt_numeric";
			break;
		case ECPGt_interval:
			return "ECPGt_interval";
			break;
		case ECPGt_descriptor:
			return "ECPGt_descriptor";
			break;
		case ECPGt_sqlda:
			return "ECPGt_sqlda";
			break;
		case ECPGt_date:
			return "ECPGt_date";
			break;
		case ECPGt_timestamp:
			return "ECPGt_timestamp";
			break;
		case ECPGt_string:
			return "ECPGt_string";
			break;
		default:
			mmerror(PARSE_ERROR, ET_ERROR, "unrecognized variable type code %d", type);
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
							  char *arrsize, const char *size, const char *prefix, int counter);
static void ECPGdump_a_struct(FILE *o, const char *name, const char *ind_name, char *arrsize,
							  struct ECPGtype *type, struct ECPGtype *ind_type, const char *prefix, const char *ind_prefix);

void
ECPGdump_a_type(FILE *o, const char *name, struct ECPGtype *type, const int brace_level,
				const char *ind_name, struct ECPGtype *ind_type, const int ind_brace_level,
				const char *prefix, const char *ind_prefix,
				char *arr_str_size, const char *struct_sizeof,
				const char *ind_struct_sizeof)
{
	struct variable *var;

	if (type->type != ECPGt_descriptor && type->type != ECPGt_sqlda &&
		type->type != ECPGt_char_variable && type->type != ECPGt_const &&
		brace_level >= 0)
	{
		char	   *str;

		str = mm_strdup(name);
		var = find_variable(str);
		free(str);

		if ((var->type->type != type->type) ||
			(var->type->type_name && !type->type_name) ||
			(!var->type->type_name && type->type_name) ||
			(var->type->type_name && type->type_name && strcmp(var->type->type_name, type->type_name) != 0))
			mmerror(PARSE_ERROR, ET_ERROR, "variable \"%s\" is hidden by a local variable of a different type", name);
		else if (var->brace_level != brace_level)
			mmerror(PARSE_ERROR, ET_WARNING, "variable \"%s\" is hidden by a local variable", name);

		if (ind_name && ind_type && ind_type->type != ECPGt_NO_INDICATOR && ind_brace_level >= 0)
		{
			str = mm_strdup(ind_name);
			var = find_variable(str);
			free(str);

			if ((var->type->type != ind_type->type) ||
				(var->type->type_name && !ind_type->type_name) ||
				(!var->type->type_name && ind_type->type_name) ||
				(var->type->type_name && ind_type->type_name && strcmp(var->type->type_name, ind_type->type_name) != 0))
				mmerror(PARSE_ERROR, ET_ERROR, "indicator variable \"%s\" is hidden by a local variable of a different type", ind_name);
			else if (var->brace_level != ind_brace_level)
				mmerror(PARSE_ERROR, ET_WARNING, "indicator variable \"%s\" is hidden by a local variable", ind_name);
		}
	}

	switch (type->type)
	{
		case ECPGt_array:
			if (indicator_set && ind_type->type != ECPGt_array)
				mmfatal(INDICATOR_NOT_ARRAY, "indicator for array/pointer has to be array/pointer");
			switch (type->u.element->type)
			{
				case ECPGt_array:
					mmerror(PARSE_ERROR, ET_ERROR, "nested arrays are not supported (except strings)"); /* array of array */
					break;
				case ECPGt_struct:
				case ECPGt_union:
					ECPGdump_a_struct(o, name,
									  ind_name,
									  type->size,
									  type->u.element,
									  (ind_type == NULL) ? NULL : ((ind_type->type == ECPGt_NO_INDICATOR) ? ind_type : ind_type->u.element),
									  prefix, ind_prefix);
					break;
				default:
					if (!IS_SIMPLE_TYPE(type->u.element->type))
						base_yyerror("internal error: unknown datatype, please report this to <" PACKAGE_BUGREPORT ">");

					ECPGdump_a_simple(o, name,
									  type->u.element->type,
									  type->u.element->size, type->size, struct_sizeof ? struct_sizeof : NULL,
									  prefix, type->u.element->counter);

					if (ind_type != NULL)
					{
						if (ind_type->type == ECPGt_NO_INDICATOR)
						{
							char	   *str_neg_one = mm_strdup("-1");

							ECPGdump_a_simple(o, ind_name, ind_type->type, ind_type->size, str_neg_one, NULL, ind_prefix, 0);
							free(str_neg_one);
						}
						else
						{
							ECPGdump_a_simple(o, ind_name, ind_type->u.element->type,
											  ind_type->u.element->size, ind_type->size, NULL, ind_prefix, 0);
						}
					}
			}
			break;
		case ECPGt_struct:
			{
				char	   *str_one = mm_strdup("1");

				if (indicator_set && ind_type->type != ECPGt_struct)
					mmfatal(INDICATOR_NOT_STRUCT, "indicator for struct has to be a struct");

				ECPGdump_a_struct(o, name, ind_name, str_one, type, ind_type, prefix, ind_prefix);
				free(str_one);
			}
			break;
		case ECPGt_union:		/* cannot dump a complete union */
			base_yyerror("type of union has to be specified");
			break;
		case ECPGt_char_variable:
			{
				/*
				 * Allocate for each, as there are code-paths where the values
				 * get stomped on.
				 */
				char	   *str_varchar_one = mm_strdup("1");
				char	   *str_arr_one = mm_strdup("1");
				char	   *str_neg_one = mm_strdup("-1");

				if (indicator_set && (ind_type->type == ECPGt_struct || ind_type->type == ECPGt_array))
					mmfatal(INDICATOR_NOT_SIMPLE, "indicator for simple data type has to be simple");

				ECPGdump_a_simple(o, name, type->type, str_varchar_one, (arr_str_size && strcmp(arr_str_size, "0") != 0) ? arr_str_size : str_arr_one, struct_sizeof, prefix, 0);
				if (ind_type != NULL)
					ECPGdump_a_simple(o, ind_name, ind_type->type, ind_type->size, (arr_str_size && strcmp(arr_str_size, "0") != 0) ? arr_str_size : str_neg_one, ind_struct_sizeof, ind_prefix, 0);

				free(str_varchar_one);
				free(str_arr_one);
				free(str_neg_one);
			}
			break;
		case ECPGt_descriptor:
			{
				/*
				 * Allocate for each, as there are code-paths where the values
				 * get stomped on.
				 */
				char	   *str_neg_one = mm_strdup("-1");
				char	   *ind_type_neg_one = mm_strdup("-1");

				if (indicator_set && (ind_type->type == ECPGt_struct || ind_type->type == ECPGt_array))
					mmfatal(INDICATOR_NOT_SIMPLE, "indicator for simple data type has to be simple");

				ECPGdump_a_simple(o, name, type->type, NULL, str_neg_one, NULL, prefix, 0);
				if (ind_type != NULL)
					ECPGdump_a_simple(o, ind_name, ind_type->type, ind_type->size, ind_type_neg_one, NULL, ind_prefix, 0);

				free(str_neg_one);
				free(ind_type_neg_one);
			}
			break;
		default:
			{
				/*
				 * Allocate for each, as there are code-paths where the values
				 * get stomped on.
				 */
				char	   *str_neg_one = mm_strdup("-1");
				char	   *ind_type_neg_one = mm_strdup("-1");

				if (indicator_set && (ind_type->type == ECPGt_struct || ind_type->type == ECPGt_array))
					mmfatal(INDICATOR_NOT_SIMPLE, "indicator for simple data type has to be simple");

				ECPGdump_a_simple(o, name, type->type, type->size, (arr_str_size && strcmp(arr_str_size, "0") != 0) ? arr_str_size : str_neg_one, struct_sizeof, prefix, type->counter);
				if (ind_type != NULL)
					ECPGdump_a_simple(o, ind_name, ind_type->type, ind_type->size, (arr_str_size && strcmp(arr_str_size, "0") != 0) ? arr_str_size : ind_type_neg_one, ind_struct_sizeof, ind_prefix, 0);

				free(str_neg_one);
				free(ind_type_neg_one);
			}
			break;
	}
}


/* If size is NULL, then the offset is 0, if not use size as a
   string, it represents the offset needed if we are in an array of structs. */
static void
ECPGdump_a_simple(FILE *o, const char *name, enum ECPGttype type,
				  char *varcharsize,
				  char *arrsize,
				  const char *size,
				  const char *prefix,
				  int counter)
{
	if (type == ECPGt_NO_INDICATOR)
		fprintf(o, "\n\tECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ");
	else if (type == ECPGt_descriptor)
		/* remember that name here already contains quotes (if needed) */
		fprintf(o, "\n\tECPGt_descriptor, %s, 1L, 1L, 1L, ", name);
	else if (type == ECPGt_sqlda)
		fprintf(o, "\n\tECPGt_sqlda, &%s, 0L, 0L, 0L, ", name);
	else
	{
		char	   *variable = (char *) mm_alloc(strlen(name) + ((prefix == NULL) ? 0 : strlen(prefix)) + 4);
		char	   *offset = (char *) mm_alloc(strlen(name) + strlen("sizeof(struct varchar_)") + 1 + strlen(varcharsize) + sizeof(int) * CHAR_BIT * 10 / 3);
		char	   *struct_name;

		switch (type)
		{
				/*
				 * we have to use the & operator except for arrays and
				 * pointers
				 */

			case ECPGt_varchar:
			case ECPGt_bytea:

				/*
				 * we have to use the pointer except for arrays with given
				 * bounds
				 */
				if (((atoi(arrsize) > 0) ||
					 (atoi(arrsize) == 0 && strcmp(arrsize, "0") != 0)) &&
					size == NULL)
					sprintf(variable, "(%s%s)", prefix ? prefix : "", name);
				else
					sprintf(variable, "&(%s%s)", prefix ? prefix : "", name);

				/*
				 * If we created a varchar structure automatically, counter is
				 * greater than 0.
				 */
				if (type == ECPGt_varchar)
					struct_name = "struct varchar";
				else
					struct_name = "struct bytea";

				if (counter)
					sprintf(offset, "sizeof(%s_%d)", struct_name, counter);
				else
					sprintf(offset, "sizeof(%s)", struct_name);
				break;
			case ECPGt_char:
			case ECPGt_unsigned_char:
			case ECPGt_char_variable:
			case ECPGt_string:
				{
					char	   *sizeof_name = "char";

					/*
					 * we have to use the pointer except for arrays with given
					 * bounds, ecpglib will distinguish between * and []
					 */
					if ((atoi(varcharsize) > 1 ||
						 (atoi(arrsize) > 0) ||
						 (atoi(varcharsize) == 0 && strcmp(varcharsize, "0") != 0) ||
						 (atoi(arrsize) == 0 && strcmp(arrsize, "0") != 0))
						&& size == NULL)
					{
						sprintf(variable, "(%s%s)", prefix ? prefix : "", name);
						if ((type == ECPGt_char || type == ECPGt_unsigned_char) &&
							strcmp(varcharsize, "0") == 0)
						{
							/*
							 * If this is an array of char *, the offset would
							 * be sizeof(char *) and not sizeof(char).
							 */
							sizeof_name = "char *";
						}
					}
					else
						sprintf(variable, "&(%s%s)", prefix ? prefix : "", name);

					sprintf(offset, "(%s)*sizeof(%s)", strcmp(varcharsize, "0") == 0 ? "1" : varcharsize, sizeof_name);
					break;
				}
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
				 * we have to use a pointer and translate the variable type
				 */
				sprintf(variable, "&(%s%s)", prefix ? prefix : "", name);
				sprintf(offset, "sizeof(date)");
				break;
			case ECPGt_timestamp:

				/*
				 * we have to use a pointer and translate the variable type
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
					size == NULL)
					sprintf(variable, "(%s%s)", prefix ? prefix : "", name);
				else
					sprintf(variable, "&(%s%s)", prefix ? prefix : "", name);

				sprintf(offset, "sizeof(%s)", ecpg_type_name(type));
				break;
		}

		/*
		 * Array size would be -1 for addresses of members within structure,
		 * when pointer to structure is being dumped.
		 */
		if (atoi(arrsize) < 0 && !size)
			strcpy(arrsize, "1");

		/*
		 * If size i.e. the size of structure of which this variable is part
		 * of, that gives the offset to the next element, if required
		 */
		if (size == NULL || strlen(size) == 0)
			fprintf(o, "\n\t%s,%s,(long)%s,(long)%s,%s, ", get_type(type), variable, varcharsize, arrsize, offset);
		else
			fprintf(o, "\n\t%s,%s,(long)%s,(long)%s,%s, ", get_type(type), variable, varcharsize, arrsize, size);

		free(variable);
		free(offset);
	}
}


/* Penetrate a struct and dump the contents. */
static void
ECPGdump_a_struct(FILE *o, const char *name, const char *ind_name, char *arrsize, struct ECPGtype *type, struct ECPGtype *ind_type, const char *prefix, const char *ind_prefix)
{
	/*
	 * If offset is NULL, then this is the first recursive level. If not then
	 * we are in a struct and the offset is used as offset.
	 */
	struct ECPGstruct_member *p,
			   *ind_p = NULL;
	char	   *pbuf = (char *) mm_alloc(strlen(name) + ((prefix == NULL) ? 0 : strlen(prefix)) + 3);
	char	   *ind_pbuf = (char *) mm_alloc(strlen(ind_name) + ((ind_prefix == NULL) ? 0 : strlen(ind_prefix)) + 3);

	if (atoi(arrsize) == 1)
		sprintf(pbuf, "%s%s.", prefix ? prefix : "", name);
	else
		sprintf(pbuf, "%s%s->", prefix ? prefix : "", name);

	prefix = pbuf;

	if (ind_type == &ecpg_no_indicator)
		ind_p = &struct_no_indicator;
	else if (ind_type != NULL)
	{
		if (atoi(arrsize) == 1)
			sprintf(ind_pbuf, "%s%s.", ind_prefix ? ind_prefix : "", ind_name);
		else
			sprintf(ind_pbuf, "%s%s->", ind_prefix ? ind_prefix : "", ind_name);

		ind_prefix = ind_pbuf;
		ind_p = ind_type->u.members;
	}

	for (p = type->u.members; p; p = p->next)
	{
		ECPGdump_a_type(o, p->name, p->type, -1,
						(ind_p != NULL) ? ind_p->name : NULL,
						(ind_p != NULL) ? ind_p->type : NULL,
						-1,
						prefix, ind_prefix, arrsize, type->struct_sizeof,
						(ind_p != NULL) ? ind_type->struct_sizeof : NULL);
		if (ind_p != NULL && ind_p != &struct_no_indicator)
		{
			ind_p = ind_p->next;
			if (ind_p == NULL && p->next != NULL)
			{
				mmerror(PARSE_ERROR, ET_WARNING, "indicator struct \"%s\" has too few members", ind_name);
				ind_p = &struct_no_indicator;
			}
		}
	}

	if (ind_type != NULL && ind_p != NULL && ind_p != &struct_no_indicator)
	{
		mmerror(PARSE_ERROR, ET_WARNING, "indicator struct \"%s\" has too many members", ind_name);
	}

	free(pbuf);
	free(ind_pbuf);
}

void
ECPGfree_struct_member(struct ECPGstruct_member *rm)
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
ECPGfree_type(struct ECPGtype *type)
{
	if (!IS_SIMPLE_TYPE(type->type))
	{
		switch (type->type)
		{
			case ECPGt_array:
				switch (type->u.element->type)
				{
					case ECPGt_array:
						base_yyerror("internal error: found multidimensional array\n");
						break;
					case ECPGt_struct:
					case ECPGt_union:
						/* Array of structs. */
						ECPGfree_struct_member(type->u.element->u.members);
						free(type->u.element);
						break;
					default:
						if (!IS_SIMPLE_TYPE(type->u.element->type))
							base_yyerror("internal error: unknown datatype, please report this to <" PACKAGE_BUGREPORT ">");

						free(type->u.element);
				}
				break;
			case ECPGt_struct:
			case ECPGt_union:
				ECPGfree_struct_member(type->u.members);
				break;
			default:
				mmerror(PARSE_ERROR, ET_ERROR, "unrecognized variable type code %d", type->type);
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
			return "ECPGd_count";
			break;
		case ECPGd_data:
			return "ECPGd_data";
			break;
		case ECPGd_di_code:
			return "ECPGd_di_code";
			break;
		case ECPGd_di_precision:
			return "ECPGd_di_precision";
			break;
		case ECPGd_indicator:
			return "ECPGd_indicator";
			break;
		case ECPGd_key_member:
			return "ECPGd_key_member";
			break;
		case ECPGd_length:
			return "ECPGd_length";
			break;
		case ECPGd_name:
			return "ECPGd_name";
			break;
		case ECPGd_nullable:
			return "ECPGd_nullable";
			break;
		case ECPGd_octet:
			return "ECPGd_octet";
			break;
		case ECPGd_precision:
			return "ECPGd_precision";
			break;
		case ECPGd_ret_length:
			return "ECPGd_ret_length";
		case ECPGd_ret_octet:
			return "ECPGd_ret_octet";
			break;
		case ECPGd_scale:
			return "ECPGd_scale";
			break;
		case ECPGd_type:
			return "ECPGd_type";
			break;
		case ECPGd_cardinality:
			return "ECPGd_cardinality";
		default:
			mmerror(PARSE_ERROR, ET_ERROR, "unrecognized descriptor item code %d", type);
	}

	return NULL;
}
