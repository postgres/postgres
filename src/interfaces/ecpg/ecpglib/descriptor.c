/* dynamic SQL support routines
 *
 * $PostgreSQL: pgsql/src/interfaces/ecpg/ecpglib/descriptor.c,v 1.20.2.1 2007/04/27 06:58:17 meskes Exp $
 */

#define POSTGRES_ECPG_INTERNAL
#include "postgres_fe.h"
#include "pg_type.h"

#include "ecpgtype.h"
#include "ecpglib.h"
#include "ecpgerrno.h"
#include "extern.h"
#include "sqlca.h"
#include "sql3types.h"

struct descriptor *all_descriptors = NULL;

/* old internal convenience function that might go away later */
static PGresult
		   *
ECPGresultByDescriptor(int line, const char *name)
{
	PGresult  **resultpp = ECPGdescriptor_lvalue(line, name);

	if (resultpp)
		return *resultpp;
	return NULL;
}

static unsigned int
ECPGDynamicType_DDT(Oid type)
{
	switch (type)
	{
		case DATEOID:
			return SQL3_DDT_DATE;
		case TIMEOID:
			return SQL3_DDT_TIME;
		case TIMESTAMPOID:
			return SQL3_DDT_TIMESTAMP;
		case TIMESTAMPTZOID:
			return SQL3_DDT_TIMESTAMP_WITH_TIME_ZONE;
		case TIMETZOID:
			return SQL3_DDT_TIME_WITH_TIME_ZONE;
		default:
			return SQL3_DDT_ILLEGAL;
	}
}

bool
ECPGget_desc_header(int lineno, const char *desc_name, int *count)
{
	PGresult   *ECPGresult;
	struct sqlca_t *sqlca = ECPGget_sqlca();

	ECPGinit_sqlca(sqlca);
	ECPGresult = ECPGresultByDescriptor(lineno, desc_name);
	if (!ECPGresult)
		return false;

	*count = PQnfields(ECPGresult);
	sqlca->sqlerrd[2] = 1;
	ECPGlog("ECPGget_desc_header: found %d attributes.\n", *count);
	return true;
}

static bool
get_int_item(int lineno, void *var, enum ECPGttype vartype, int value)
{
	switch (vartype)
	{
		case ECPGt_short:
			*(short *) var = (short) value;
			break;
		case ECPGt_int:
			*(int *) var = (int) value;
			break;
		case ECPGt_long:
			*(long *) var = (long) value;
			break;
		case ECPGt_unsigned_short:
			*(unsigned short *) var = (unsigned short) value;
			break;
		case ECPGt_unsigned_int:
			*(unsigned int *) var = (unsigned int) value;
			break;
		case ECPGt_unsigned_long:
			*(unsigned long *) var = (unsigned long) value;
			break;
#ifdef HAVE_LONG_LONG_INT_64
		case ECPGt_long_long:
			*(long long int *) var = (long long int) value;
			break;
		case ECPGt_unsigned_long_long:
			*(unsigned long long int *) var = (unsigned long long int) value;
			break;
#endif   /* HAVE_LONG_LONG_INT_64 */
		case ECPGt_float:
			*(float *) var = (float) value;
			break;
		case ECPGt_double:
			*(double *) var = (double) value;
			break;
		default:
			ECPGraise(lineno, ECPG_VAR_NOT_NUMERIC, ECPG_SQLSTATE_RESTRICTED_DATA_TYPE_ATTRIBUTE_VIOLATION, NULL);
			return (false);
	}

	return (true);
}

static bool
set_int_item(int lineno, int *target, const void *var, enum ECPGttype vartype)
{
	switch (vartype)
	{
		case ECPGt_short:
			*target = *(short *) var;
			break;
		case ECPGt_int:
			*target = *(int *) var;
			break;
		case ECPGt_long:
			*target = *(long *) var;
			break;
		case ECPGt_unsigned_short:
			*target = *(unsigned short *) var;
			break;
		case ECPGt_unsigned_int:
			*target = *(unsigned int *) var;
			break;
		case ECPGt_unsigned_long:
			*target = *(unsigned long *) var;
			break;
#ifdef HAVE_LONG_LONG_INT_64
		case ECPGt_long_long:
			*target = *(long long int *) var;
			break;
		case ECPGt_unsigned_long_long:
			*target = *(unsigned long long int *) var;
			break;
#endif   /* HAVE_LONG_LONG_INT_64 */
		case ECPGt_float:
			*target = *(float *) var;
			break;
		case ECPGt_double:
			*target = *(double *) var;
			break;
		default:
			ECPGraise(lineno, ECPG_VAR_NOT_NUMERIC, ECPG_SQLSTATE_RESTRICTED_DATA_TYPE_ATTRIBUTE_VIOLATION, NULL);
			return (false);
	}

	return true;
}

static bool
get_char_item(int lineno, void *var, enum ECPGttype vartype, char *value, int varcharsize)
{
	switch (vartype)
	{
		case ECPGt_char:
		case ECPGt_unsigned_char:
			strncpy((char *) var, value, varcharsize);
			break;
		case ECPGt_varchar:
			{
				struct ECPGgeneric_varchar *variable =
				(struct ECPGgeneric_varchar *) var;

				if (varcharsize == 0)
					strncpy(variable->arr, value, strlen(value));
				else
					strncpy(variable->arr, value, varcharsize);

				variable->len = strlen(value);
				if (varcharsize > 0 && variable->len > varcharsize)
					variable->len = varcharsize;
			}
			break;
		default:
			ECPGraise(lineno, ECPG_VAR_NOT_CHAR, ECPG_SQLSTATE_RESTRICTED_DATA_TYPE_ATTRIBUTE_VIOLATION, NULL);
			return (false);
	}

	return (true);
}

bool
ECPGget_desc(int lineno, const char *desc_name, int index,...)
{
	va_list		args;
	PGresult   *ECPGresult;
	enum ECPGdtype type;
	int			ntuples,
				act_tuple;
	struct variable data_var;
	struct sqlca_t *sqlca = ECPGget_sqlca();

	va_start(args, index);
	ECPGinit_sqlca(sqlca);
	ECPGresult = ECPGresultByDescriptor(lineno, desc_name);
	if (!ECPGresult)
		return (false);

	ntuples = PQntuples(ECPGresult);
	if (ntuples < 1)
	{
		ECPGraise(lineno, ECPG_NOT_FOUND, ECPG_SQLSTATE_NO_DATA, NULL);
		return (false);
	}

	if (index < 1 || index > PQnfields(ECPGresult))
	{
		ECPGraise(lineno, ECPG_INVALID_DESCRIPTOR_INDEX, ECPG_SQLSTATE_INVALID_DESCRIPTOR_INDEX, NULL);
		return (false);
	}

	ECPGlog("ECPGget_desc: reading items for tuple %d\n", index);
	--index;

	type = va_arg(args, enum ECPGdtype);

	memset(&data_var, 0, sizeof data_var);
	data_var.type = ECPGt_EORT;
	data_var.ind_type = ECPGt_NO_INDICATOR;

	while (type != ECPGd_EODT)
	{
		char		type_str[20];
		long		varcharsize;
		long		offset;
		long		arrsize;
		enum ECPGttype vartype;
		void	   *var;

		vartype = va_arg(args, enum ECPGttype);
		var = va_arg(args, void *);
		varcharsize = va_arg(args, long);
		arrsize = va_arg(args, long);
		offset = va_arg(args, long);

		switch (type)
		{
			case (ECPGd_indicator):
				data_var.ind_type = vartype;
				data_var.ind_pointer = var;
				data_var.ind_varcharsize = varcharsize;
				data_var.ind_arrsize = arrsize;
				data_var.ind_offset = offset;
				if (data_var.ind_arrsize == 0 || data_var.ind_varcharsize == 0)
					data_var.ind_value = *((void **) (data_var.ind_pointer));
				else
					data_var.ind_value = data_var.ind_pointer;
				break;

			case ECPGd_data:
				data_var.type = vartype;
				data_var.pointer = var;
				data_var.varcharsize = varcharsize;
				data_var.arrsize = arrsize;
				data_var.offset = offset;
				if (data_var.arrsize == 0 || data_var.varcharsize == 0)
					data_var.value = *((void **) (data_var.pointer));
				else
					data_var.value = data_var.pointer;
				break;

			case ECPGd_name:
				if (!get_char_item(lineno, var, vartype, PQfname(ECPGresult, index), varcharsize))
					return (false);

				ECPGlog("ECPGget_desc: NAME = %s\n", PQfname(ECPGresult, index));
				break;

			case ECPGd_nullable:
				if (!get_int_item(lineno, var, vartype, 1))
					return (false);

				break;

			case ECPGd_key_member:
				if (!get_int_item(lineno, var, vartype, 0))
					return (false);

				break;

			case ECPGd_scale:
				if (!get_int_item(lineno, var, vartype, (PQfmod(ECPGresult, index) - VARHDRSZ) & 0xffff))
					return (false);

				ECPGlog("ECPGget_desc: SCALE = %d\n", (PQfmod(ECPGresult, index) - VARHDRSZ) & 0xffff);
				break;

			case ECPGd_precision:
				if (!get_int_item(lineno, var, vartype, PQfmod(ECPGresult, index) >> 16))
					return (false);

				ECPGlog("ECPGget_desc: PRECISION = %d\n", PQfmod(ECPGresult, index) >> 16);
				break;

			case ECPGd_octet:
				if (!get_int_item(lineno, var, vartype, PQfsize(ECPGresult, index)))
					return (false);

				ECPGlog("ECPGget_desc: OCTET_LENGTH = %d\n", PQfsize(ECPGresult, index));
				break;

			case ECPGd_length:
				if (!get_int_item(lineno, var, vartype, PQfmod(ECPGresult, index) - VARHDRSZ))
					return (false);

				ECPGlog("ECPGget_desc: LENGTH = %d\n", PQfmod(ECPGresult, index) - VARHDRSZ);
				break;

			case ECPGd_type:
				if (!get_int_item(lineno, var, vartype, ECPGDynamicType(PQftype(ECPGresult, index))))
					return (false);

				ECPGlog("ECPGget_desc: TYPE = %d\n", ECPGDynamicType(PQftype(ECPGresult, index)));
				break;

			case ECPGd_di_code:
				if (!get_int_item(lineno, var, vartype, ECPGDynamicType_DDT(PQftype(ECPGresult, index))))
					return (false);

				ECPGlog("ECPGget_desc: TYPE = %d\n", ECPGDynamicType_DDT(PQftype(ECPGresult, index)));
				break;

			case ECPGd_cardinality:
				if (!get_int_item(lineno, var, vartype, PQntuples(ECPGresult)))
					return (false);

				ECPGlog("ECPGget_desc: CARDINALITY = %d\n", PQntuples(ECPGresult));
				break;

			case ECPGd_ret_length:
			case ECPGd_ret_octet:

				/*
				 * this is like ECPGstore_result
				 */
				if (arrsize > 0 && ntuples > arrsize)
				{
					ECPGlog("ECPGget_desc line %d: Incorrect number of matches: %d don't fit into array of %d\n",
							lineno, ntuples, arrsize);
					ECPGraise(lineno, ECPG_TOO_MANY_MATCHES, ECPG_SQLSTATE_CARDINALITY_VIOLATION, NULL);
					return false;
				}
				/* allocate storage if needed */
				if (arrsize == 0 && *(void **) var == NULL)
				{
					void	   *mem = (void *) ECPGalloc(offset * ntuples, lineno);

					if (!mem)
						return false;
					*(void **) var = mem;
					ECPGadd_mem(mem, lineno);
					var = mem;
				}

				for (act_tuple = 0; act_tuple < ntuples; act_tuple++)
				{
					if (!get_int_item(lineno, var, vartype, PQgetlength(ECPGresult, act_tuple, index)))
						return (false);
					var = (char *) var + offset;
					ECPGlog("ECPGget_desc: RETURNED[%d] = %d\n", act_tuple, PQgetlength(ECPGresult, act_tuple, index));
				}
				break;

			default:
				snprintf(type_str, sizeof(type_str), "%d", type);
				ECPGraise(lineno, ECPG_UNKNOWN_DESCRIPTOR_ITEM, ECPG_SQLSTATE_ECPG_INTERNAL_ERROR, type_str);
				return (false);
		}

		type = va_arg(args, enum ECPGdtype);
	}

	if (data_var.type != ECPGt_EORT)
	{
		struct statement stmt;
		char	   *oldlocale;

		/* Make sure we do NOT honor the locale for numeric input */
		/* since the database gives the standard decimal point */
		oldlocale = ECPGstrdup(setlocale(LC_NUMERIC, NULL), lineno);
		setlocale(LC_NUMERIC, "C");

		memset(&stmt, 0, sizeof stmt);
		stmt.lineno = lineno;

		/* desparate try to guess something sensible */
		stmt.connection = ECPGget_connection(NULL);
		ECPGstore_result(ECPGresult, index, &stmt, &data_var);

		setlocale(LC_NUMERIC, oldlocale);
		ECPGfree(oldlocale);
	}
	else if (data_var.ind_type != ECPGt_NO_INDICATOR && data_var.ind_pointer != NULL)

		/*
		 * ind_type != NO_INDICATOR should always have ind_pointer != NULL but
		 * since this might be changed manually in the .c file let's play it
		 * safe
		 */
	{
		/*
		 * this is like ECPGstore_result but since we don't have a data
		 * variable at hand, we can't call it
		 */
		if (data_var.ind_arrsize > 0 && ntuples > data_var.ind_arrsize)
		{
			ECPGlog("ECPGget_desc line %d: Incorrect number of matches (indicator): %d don't fit into array of %d\n",
					lineno, ntuples, data_var.ind_arrsize);
			ECPGraise(lineno, ECPG_TOO_MANY_MATCHES, ECPG_SQLSTATE_CARDINALITY_VIOLATION, NULL);
			return false;
		}

		/* allocate storage if needed */
		if (data_var.ind_arrsize == 0 && data_var.ind_value == NULL)
		{
			void	   *mem = (void *) ECPGalloc(data_var.ind_offset * ntuples, lineno);

			if (!mem)
				return false;
			*(void **) data_var.ind_pointer = mem;
			ECPGadd_mem(mem, lineno);
			data_var.ind_value = mem;
		}

		for (act_tuple = 0; act_tuple < ntuples; act_tuple++)
		{
			if (!get_int_item(lineno, data_var.ind_value, data_var.ind_type, -PQgetisnull(ECPGresult, act_tuple, index)))
				return (false);
			data_var.ind_value = (char *) data_var.ind_value + data_var.ind_offset;
			ECPGlog("ECPGget_desc: INDICATOR[%d] = %d\n", act_tuple, -PQgetisnull(ECPGresult, act_tuple, index));
		}
	}
	sqlca->sqlerrd[2] = ntuples;
	return (true);
}

bool
ECPGset_desc_header(int lineno, const char *desc_name, int count)
{
	struct descriptor *desc;

	for (desc = all_descriptors; desc; desc = desc->next)
	{
		if (strcmp(desc_name, desc->name) == 0)
			break;
	}

	if (desc == NULL)
	{
		ECPGraise(lineno, ECPG_UNKNOWN_DESCRIPTOR, ECPG_SQLSTATE_INVALID_SQL_DESCRIPTOR_NAME, desc_name);
		return false;
	}

	desc->count = count;
	return true;
}

bool
ECPGset_desc(int lineno, const char *desc_name, int index,...)
{
	va_list		args;
	struct descriptor *desc;
	struct descriptor_item *desc_item;
	struct variable *var;

	for (desc = all_descriptors; desc; desc = desc->next)
	{
		if (strcmp(desc_name, desc->name) == 0)
			break;
	}

	if (desc == NULL)
	{
		ECPGraise(lineno, ECPG_UNKNOWN_DESCRIPTOR, ECPG_SQLSTATE_INVALID_SQL_DESCRIPTOR_NAME, desc_name);
		return false;
	}

	for (desc_item = desc->items; desc_item; desc_item = desc_item->next)
	{
		if (desc_item->num == index)
			break;
	}

	if (desc_item == NULL)
	{
		desc_item = (struct descriptor_item *) ECPGalloc(sizeof(*desc_item), lineno);
		if (!desc_item)
			return false;
		desc_item->num = index;
		desc_item->next = desc->items;
		desc->items = desc_item;
	}

	if (!(var = (struct variable *) ECPGalloc(sizeof(struct variable), lineno)))
		return false;

	va_start(args, index);

	do
	{
		enum ECPGdtype itemtype;
		const char *tobeinserted = NULL;
		bool		malloced;

		itemtype = va_arg(args, enum ECPGdtype);

		if (itemtype == ECPGd_EODT)
			break;

		var->type = va_arg(args, enum ECPGttype);
		var->pointer = va_arg(args, char *);

		var->varcharsize = va_arg(args, long);
		var->arrsize = va_arg(args, long);
		var->offset = va_arg(args, long);

		if (var->arrsize == 0 || var->varcharsize == 0)
			var->value = *((char **) (var->pointer));
		else
			var->value = var->pointer;

		/*
		 * negative values are used to indicate an array without given bounds
		 */
		/* reset to zero for us */
		if (var->arrsize < 0)
			var->arrsize = 0;
		if (var->varcharsize < 0)
			var->varcharsize = 0;

		var->next = NULL;
		
		switch (itemtype)
		{
			case ECPGd_data:
				{
					if (!ECPGstore_input(lineno, true, var, &tobeinserted, &malloced, false))
					{
						ECPGfree(var);
						return false;
					}

					desc_item->data = (char *) tobeinserted;
					tobeinserted = NULL;
					break;
				}

			case ECPGd_indicator:
				set_int_item(lineno, &desc_item->indicator, var->pointer, var->type);
				break;

			case ECPGd_length:
				set_int_item(lineno, &desc_item->length, var->pointer, var->type);
				break;

			case ECPGd_precision:
				set_int_item(lineno, &desc_item->precision, var->pointer, var->type);
				break;

			case ECPGd_scale:
				set_int_item(lineno, &desc_item->scale, var->pointer, var->type);
				break;

			case ECPGd_type:
				set_int_item(lineno, &desc_item->type, var->pointer, var->type);
				break;

			default:
				{
					char		type_str[20];

					snprintf(type_str, sizeof(type_str), "%d", itemtype);
					ECPGraise(lineno, ECPG_UNKNOWN_DESCRIPTOR_ITEM, ECPG_SQLSTATE_ECPG_INTERNAL_ERROR, type_str);
					ECPGfree(var);
					return false;
				}
		}

		/*
		 * if (itemtype == ECPGd_data) { free(desc_item->data);
		 * desc_item->data = NULL; }
		 */
	}
	while (true);
	ECPGfree(var);

	return true;
}

bool
ECPGdeallocate_desc(int line, const char *name)
{
	struct descriptor *i;
	struct descriptor **lastptr = &all_descriptors;
	struct sqlca_t *sqlca = ECPGget_sqlca();

	ECPGinit_sqlca(sqlca);
	for (i = all_descriptors; i; lastptr = &i->next, i = i->next)
	{
		if (!strcmp(name, i->name))
		{
			*lastptr = i->next;
			ECPGfree(i->name);
			PQclear(i->result);
			ECPGfree(i);
			return true;
		}
	}
	ECPGraise(line, ECPG_UNKNOWN_DESCRIPTOR, ECPG_SQLSTATE_INVALID_SQL_DESCRIPTOR_NAME, name);
	return false;
}

bool
ECPGallocate_desc(int line, const char *name)
{
	struct descriptor *new;
	struct sqlca_t *sqlca = ECPGget_sqlca();

	ECPGinit_sqlca(sqlca);
	new = (struct descriptor *) ECPGalloc(sizeof(struct descriptor), line);
	if (!new)
		return false;
	new->next = all_descriptors;
	new->name = ECPGalloc(strlen(name) + 1, line);
	if (!new->name)
	{
		ECPGfree(new);
		return false;
	}
	new->count = -1;
	new->items = NULL;
	new->result = PQmakeEmptyPGresult(NULL, 0);
	if (!new->result)
	{
		ECPGfree(new->name);
		ECPGfree(new);
		ECPGraise(line, ECPG_OUT_OF_MEMORY, ECPG_SQLSTATE_ECPG_OUT_OF_MEMORY, NULL);
		return false;
	}
	strcpy(new->name, name);
	all_descriptors = new;
	return true;
}

PGresult  **
ECPGdescriptor_lvalue(int line, const char *descriptor)
{
	struct descriptor *i;

	for (i = all_descriptors; i != NULL; i = i->next)
	{
		if (!strcmp(descriptor, i->name))
			return &i->result;
	}

	ECPGraise(line, ECPG_UNKNOWN_DESCRIPTOR, ECPG_SQLSTATE_INVALID_SQL_DESCRIPTOR_NAME, (char *) descriptor);
	return NULL;
}

bool
ECPGdescribe(int line, bool input, const char *statement,...)
{
	ECPGlog("ECPGdescribe called on line %d for %s in %s\n", line, (input) ? "input" : "output", statement);
	return false;
}
