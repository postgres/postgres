#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "type.h"
#include "extern.h"

/* malloc + error check */
void *
mm_alloc(size_t size)
{
	void	   *ptr = malloc(size);

	if (ptr == NULL)
	{
		fprintf(stderr, "Out of memory\n");
		exit(OUT_OF_MEMORY);
	}

	return (ptr);
}

/* duplicate memberlist */
static struct ECPGstruct_member *
struct_member_dup(struct ECPGstruct_member * rm)
{
  struct ECPGstruct_member *new = NULL;

  while (rm)
    {
      struct ECPGtype *type;
      
      switch(rm->typ->typ)
      {
      	case ECPGt_struct:
      		type = ECPGmake_struct_type(rm->typ->u.members);
      		break;
      	case ECPGt_array:
      		type = ECPGmake_array_type(ECPGmake_simple_type(rm->typ->u.element->typ, rm->typ->u.element->size), rm->typ->size);
		break;      	
      	default:
      		type = ECPGmake_simple_type(rm->typ->typ, rm->typ->size);
      		break;
      }
      
      ECPGmake_struct_member(rm->name, type, &new);
   
      rm = rm->next;
    }

  return(new);
}

/* The NAME argument is copied. The type argument is preserved as a pointer. */
void
ECPGmake_struct_member(char *name, struct ECPGtype * type, struct ECPGstruct_member ** start)
{
	struct ECPGstruct_member *ptr,
			   *ne =
	(struct ECPGstruct_member *) mm_alloc(sizeof(struct ECPGstruct_member));

	ne->name = strdup(name);
	ne->typ = type;
	ne->next = NULL;

	for (ptr = *start; ptr && ptr->next; ptr = ptr->next);

	if (ptr)
		ptr->next = ne;
	else
		*start = ne;
}

struct ECPGtype *
ECPGmake_simple_type(enum ECPGttype typ, long siz)
{
	struct ECPGtype *ne = (struct ECPGtype *) mm_alloc(sizeof(struct ECPGtype));

	ne->typ = typ;
	ne->size = siz;
	ne->u.element = 0;

	return ne;
}

struct ECPGtype *
ECPGmake_array_type(struct ECPGtype * typ, long siz)
{
	struct ECPGtype *ne = ECPGmake_simple_type(ECPGt_array, siz);

	ne->u.element = typ;

	return ne;
}

struct ECPGtype *
ECPGmake_struct_type(struct ECPGstruct_member * rm)
{
	struct ECPGtype *ne = ECPGmake_simple_type(ECPGt_struct, 1);

	ne->u.members = struct_member_dup(rm);

	return ne;
}

static const char *get_type(enum ECPGttype typ)
{
   switch (typ)
    {
       case ECPGt_char:
           return("ECPGt_char");
           break;
       case ECPGt_unsigned_char:
           return("ECPGt_unsigned_char");
           break;
       case ECPGt_short:
           return("ECPGt_short");
           break;
       case ECPGt_unsigned_short:
           return("ECPGt_unsigned_short");
           break;
       case ECPGt_int:
           return("ECPGt_int");
           break;
       case ECPGt_unsigned_int:
           return("ECPGt_unsigned_int");
           break;
       case ECPGt_long:
           return("ECPGt_long");
           break;
       case ECPGt_unsigned_long:
           return("ECPGt_unsigned_int");
           break;
       case ECPGt_float:
           return("ECPGt_float");
           break;
       case ECPGt_double:
           return("ECPGt_double");
           break;
       case ECPGt_bool:
           return("ECPGt_bool");
           break;
       case ECPGt_varchar:
           return("ECPGt_varchar");
       case ECPGt_NO_INDICATOR: /* no indicator */
           return("ECPGt_NO_INDICATOR");
           break;
       default:
           abort();
    }
}

/* Dump a type.
   The type is dumped as:
   type-tag <comma>				   - enum ECPGttype
   reference-to-variable <comma>   - void *
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
static void
ECPGdump_a_simple(FILE *o, const char *name, enum ECPGttype typ,
				  long varcharsize,
				  long arrsiz, const char *siz, const char *prefix);
static void
ECPGdump_a_struct(FILE *o, const char *name, const char *ind_name, long arrsiz,
		  struct ECPGtype * typ, struct ECPGtype * ind_typ, const char *offset, const char *prefix, const char * ind_prefix);


void
ECPGdump_a_type(FILE *o, const char *name, struct ECPGtype * typ, const char *ind_name, struct ECPGtype * ind_typ, const char *prefix, const char *ind_prefix)
{
	if (ind_typ == NULL)
	{
		ind_typ = &ecpg_no_indicator;
		ind_name = "no_indicator";
	}
	
        switch(typ->typ)
        {
           case ECPGt_array:
               if (IS_SIMPLE_TYPE(typ->u.element->typ))
               {
                   ECPGdump_a_simple(o, name, typ->u.element->typ,
                                     typ->u.element->size, typ->size, NULL, prefix);
                   if (ind_typ == &ecpg_no_indicator)
                       ECPGdump_a_simple(o, ind_name, ind_typ->typ, ind_typ->size, -1, NULL, ind_prefix);
                   else
                       ECPGdump_a_simple(o, ind_name, ind_typ->u.element->typ,
                                     ind_typ->u.element->size, ind_typ->size, NULL, prefix);
               }
               else if (typ->u.element->typ == ECPGt_array)
               {
                   yyerror("No nested arrays allowed (except strings)");			/* Array of array, */
               }
               else if (typ->u.element->typ == ECPGt_struct)
               {
                   /* Array of structs. */
                   ECPGdump_a_struct(o, name, ind_name, typ->size, typ->u.element, ind_typ->u.element, NULL, prefix, ind_prefix);
               }
               else
               {
                   yyerror("Internal error: unknown datatype, pleqase inform pgsql-bugs@postgresql.org");
               }
               break;
           case ECPGt_struct:
               ECPGdump_a_struct(o, name, ind_name, 1, typ, ind_typ, NULL, prefix, ind_prefix);
               break;
           default:
               ECPGdump_a_simple(o, name, typ->typ, typ->size, -1, NULL, prefix);
               ECPGdump_a_simple(o, ind_name, ind_typ->typ, ind_typ->size, -1, NULL, ind_prefix);
               break;
	}
}


/* If siz is NULL, then the offset is 0, if not use siz as a
   string, it represents the offset needed if we are in an array of structs. */
static void
ECPGdump_a_simple(FILE *o, const char *name, enum ECPGttype typ,
				  long varcharsize,
				  long arrsize,
				  const char *siz,
				  const char *prefix
				  )
{
    if (typ == ECPGt_NO_INDICATOR)
        fprintf(o, "\n\tECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ");
    else
    {
        char *variable = (char *)mm_alloc(strlen(name) + ((prefix == NULL) ? 0 : strlen(prefix)) + 4);
        char *offset = (char *)mm_alloc(strlen(name) + strlen("sizeof(struct varchar_)") + 1);

        if (varcharsize == 0 || arrsize >= 0)
            sprintf(variable, "(%s%s)", prefix ? prefix : "", name);
        else
            sprintf(variable, "&(%s%s)", prefix ? prefix : "", name);

        switch (typ)
        {
	    case ECPGt_varchar:
	      sprintf(offset, "sizeof(struct varchar_%s)", name);
	      break;
	    case ECPGt_char:
	    case ECPGt_unsigned_char:
	      sprintf(offset, "%ld*sizeof(char)", varcharsize);
	      break;
	    default:
	      sprintf(offset, "sizeof(%s)", ECPGtype_name(typ));
	      break;
	}
                   
        if (arrsize < 0)
            arrsize = 1;

        if (siz == NULL)
            fprintf(o, "\n\t%s,%s,%ldL,%ldL,%s, ", get_type(typ), variable, varcharsize, arrsize, offset);
        else
            fprintf(o, "\n\t%s,%s,%ldL,%ldL,%s, ", get_type(typ), variable, varcharsize, arrsize, siz);

        free(variable);
	free(offset);
    }
}


/* Penetrate a struct and dump the contents. */
static void
ECPGdump_a_struct(FILE *o, const char *name, const char * ind_name, long arrsiz, struct ECPGtype * typ, struct ECPGtype * ind_typ, const char *offsetarg, const char *prefix, const char *ind_prefix)
{

	/*
	 * If offset is NULL, then this is the first recursive level. If not
	 * then we are in a struct in a struct and the offset is used as
	 * offset.
	 */
	struct ECPGstruct_member *p, *ind_p = NULL;
	char		obuf[BUFSIZ];
	char		pbuf[BUFSIZ], ind_pbuf[BUFSIZ];
	const char *offset;

	if (offsetarg == NULL)
	{
		sprintf(obuf, "sizeof(%s)", name);
		offset = obuf;
	}
	else
	{
		offset = offsetarg;
	}

	sprintf(pbuf, "%s%s.", prefix ? prefix : "", name);
	prefix = pbuf;
	
	sprintf(ind_pbuf, "%s%s.", ind_prefix ? ind_prefix : "", ind_name);
	ind_prefix = ind_pbuf;

	if (ind_typ != NULL) ind_p = ind_typ->u.members;
	for (p = typ->u.members; p; p = p->next)
	{
		ECPGdump_a_type(o, p->name, p->typ, (ind_p != NULL) ? ind_p->name : NULL, (ind_p != NULL) ? ind_p->typ : NULL, prefix, ind_prefix);
		if (ind_p != NULL) ind_p = ind_p->next;
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
      free(p->typ);
      free(p);
    }
}

void
ECPGfree_type(struct ECPGtype * typ)
{
	if (!IS_SIMPLE_TYPE(typ->typ))
	{
		if (typ->typ == ECPGt_array)
		{
			if (IS_SIMPLE_TYPE(typ->u.element->typ))
				free(typ->u.element);
			else if (typ->u.element->typ == ECPGt_array)
				abort();		/* Array of array, */
			else if (typ->u.element->typ == ECPGt_struct)
			{
				/* Array of structs. */
			        ECPGfree_struct_member(typ->u.element->u.members);
				free(typ->u.members);
			}
			else
				abort();
		}
		else if (typ->typ == ECPGt_struct)
		{
		        ECPGfree_struct_member(typ->u.members);
			free(typ->u.members);
		}
		else
		{
			abort();
		}
	}
	free(typ);
}
