/*
 * functions needed for descriptor handling
 */

#include "postgres.h"
#include "extern.h" 

/*
 * assignment handling function (descriptor)
 */
 
struct assignment *assignments;

void push_assignment(char *var, enum ECPGdtype value)
{
	struct assignment *new = (struct assignment *)mm_alloc(sizeof(struct assignment));
	
	new->next = assignments;
	new->variable = mm_alloc(strlen(var) + 1);
	strcpy(new->variable, var);
	new->value = value;
	assignments = new;
}

static void
drop_assignments(void)
{
	while (assignments)
	{
		struct assignment *old_head = assignments;

		assignments = old_head->next;
		free(old_head->variable);
		free(old_head);
	}
}

static void ECPGnumeric_lvalue(FILE *f,char *name)
{
	const struct variable *v=find_variable(name);

	switch(v->type->typ)
	{
		case ECPGt_short:
		case ECPGt_int: 
		case ECPGt_long:
		case ECPGt_unsigned_short:
		case ECPGt_unsigned_int:
		case ECPGt_unsigned_long:
			fputs(name,yyout);
			break;
		default:
			snprintf(errortext,sizeof errortext,"variable %s: numeric type needed"
					,name);
			mmerror(ET_ERROR,errortext);
			break;
	}	 
}

static void ECPGstring_buffer(FILE *f, char *name)
{ 
	const struct variable *v = find_variable(name);

	switch(v->type->typ)
	{
		case ECPGt_varchar:
			fprintf(yyout,"%s.arr",name);
			break;
			
		case ECPGt_char:
		case ECPGt_unsigned_char:
			fputs(name,yyout);
			break;
			
		default:
			snprintf(errortext,sizeof errortext,"variable %s: character type needed"
					,name);
			mmerror(ET_ERROR,errortext);
			break;
	}
}

static void ECPGstring_length(FILE *f,char *name)
{
	const struct variable *v=find_variable(name);
	
	switch(v->type->typ)
	{	case ECPGt_varchar:
		case ECPGt_char:
		case ECPGt_unsigned_char:
		    if (!v->type->size) 
		    {	snprintf(errortext,sizeof errortext,"zero length char variable %s for assignment",
		    									v->name);
		    	mmerror(ET_ERROR,errortext);
		    }
			fprintf(yyout,"%ld",v->type->size);
			break;
		default:
			snprintf(errortext,sizeof errortext,"variable %s: character type needed"
					,name);
			mmerror(ET_ERROR,errortext);
			break;
	}
}

static void ECPGdata_assignment(char *variable,char *index_plus_1)
{
	const struct variable *v=find_variable(variable);

	fprintf(yyout,"\t\t\tif (!PQgetisnull(ECPGresult,0,(%s)-1))\n",index_plus_1);
	switch(v->type->typ)
	{
		case ECPGt_short:
		case ECPGt_int: /* use the same conversion as ecpglib does */
		case ECPGt_long:
			fprintf(yyout,"\t\t\t\t%s=strtol(PQgetvalue(ECPGresult,0,(%s)-1),NULL,10);\n"
											,variable,index_plus_1);
			break;
		case ECPGt_unsigned_short:
		case ECPGt_unsigned_int:
		case ECPGt_unsigned_long:
			fprintf(yyout,"\t\t\t\t%s=strtoul(PQgetvalue(ECPGresult,0,(%s)-1),NULL,10);\n"
											,variable,index_plus_1);
			break;
		case ECPGt_float:
		case ECPGt_double:
			fprintf(yyout,"\t\t\t\t%s=strtod(PQgetvalue(ECPGresult,0,(%s)-1),NULL);\n"
											,variable,index_plus_1);
			break;
			
		case ECPGt_bool:
			fprintf(yyout,"\t\t\t\t%s=PQgetvalue(ECPGresult,0,(%s)-1)[0]=='t';\n"
											,variable,index_plus_1);
			break;
		
		case ECPGt_varchar:
			fprintf(yyout,"\t\t\t{\tstrncpy(%s.arr,PQgetvalue(ECPGresult,0,(%s)-1),%ld);\n"
											,variable,index_plus_1,v->type->size);
			fprintf(yyout,"\t\t\t\t%s.len=strlen(PQgetvalue(ECPGresult,0,(%s)-1)\n"
											,variable,index_plus_1);
			fprintf(yyout,"\t\t\t\tif (%s.len>%ld) { %s.len=%ld; sqlca.sqlwarn[0]=sqlca.sqlwarn[1]='W'; }\n"
											,variable,v->type->size,variable,v->type->size);
			fputs("\t\t\t}\n",yyout);											
			break;
			
		case ECPGt_char:
		case ECPGt_unsigned_char:
		    if (!v->type->size) 
		    {
			snprintf(errortext,sizeof errortext,"zero length char variable %s for DATA assignment",
		    									v->name);
		    	mmerror(ET_ERROR,errortext);
		    }
			fprintf(yyout,"\t\t\t{\tstrncpy(%s,PQgetvalue(ECPGresult,0,(%s)-1),%ld);\n"
											,variable,index_plus_1,v->type->size);
			fprintf(yyout,"\t\t\t\tif (strlen(PQgetvalue(ECPGresult,0,(%s)-1))>=%ld)\n"
				"\t\t\t\t{ %s[%ld]=0; sqlca.sqlwarn[0]=sqlca.sqlwarn[1]='W'; }\n"
											,index_plus_1,v->type->size,variable,v->type->size-1);
			fputs("\t\t\t}\n",yyout);											
			break;
			
		default:
			snprintf(errortext,sizeof errortext,"unknown variable type %d for DATA assignment"
					,v->type->typ);
			mmerror(ET_ERROR,errortext);
			break;
	}
}

/*
 * descriptor name lookup
 */
 
static struct descriptor *descriptors;

void add_descriptor(char *name,char *connection)
{
	struct descriptor *new = (struct descriptor *)mm_alloc(sizeof(struct descriptor));
	
	new->next = descriptors;
	new->name = mm_alloc(strlen(name) + 1);
	strcpy(new->name,name);
	if (connection) 
	{
		new->connection = mm_alloc(strlen(connection) + 1);
		strcpy(new->connection, connection);
	}
	else new->connection = connection;
	descriptors = new;
}

void
drop_descriptor(char *name,char *connection)
{
	struct descriptor *i;
	struct descriptor **lastptr=&descriptors;
	
	for (i=descriptors;i;lastptr=&i->next,i=i->next)
	{
		if (!strcmp(name,i->name))
		{
			if ((!connection && !i->connection) 
				|| (connection && i->connection 
					&& !strcmp(connection,i->connection)))
			{
				*lastptr=i->next;
				if (i->connection) free(i->connection);
				free(i->name);
				free(i);
				return;
			}
		}
	}
	snprintf(errortext,sizeof errortext,"unknown descriptor %s",name);
	mmerror(ET_WARN,errortext);
}

struct descriptor
*lookup_descriptor(char *name, char *connection)
{
	struct descriptor *i;
	
	for (i = descriptors; i; i = i->next)
	{
		if (!strcmp(name, i->name))
		{
			if ((!connection && !i->connection) 
				|| (connection && i->connection 
					&& !strcmp(connection,i->connection)))
			{
				return i;
			}
		}
	}
	snprintf(errortext, sizeof errortext, "unknown descriptor %s", name);
	mmerror(ET_WARN, errortext);
	return NULL;
}

void
output_get_descr_header(char *desc_name)
{
	struct assignment *results;

	fprintf(yyout, "{ ECPGget_desc_header(%d, \"%s\", &(", yylineno, desc_name);
	for (results = assignments; results != NULL; results = results->next)
	{
		if (results->value == ECPGd_count)
			ECPGnumeric_lvalue(yyout,results->variable);
		else
		{
			snprintf(errortext, sizeof errortext, "unknown descriptor header item '%d'", results->value);
			mmerror(ET_WARN, errortext);
		}
	}
	
	drop_assignments();
	fprintf(yyout, "));\n");
	whenever_action(3);
}

void
output_get_descr(char *desc_name, char *index)
{
	struct assignment *results;

	fprintf(yyout, "{ ECPGget_desc(%d,\"%s\",%s,", yylineno, desc_name, index);	
	for (results = assignments; results != NULL; results = results->next)
	{
		const struct variable *v = find_variable(results->variable);
		
		switch (results->value)
		{
			case ECPGd_nullable:
				mmerror(ET_WARN,"nullable is always 1");
				break;
			case ECPGd_key_member:
				mmerror(ET_WARN,"key_member is always 0");
				break;
			default:
				break; 
		}
		fprintf(yyout, "%s,", get_dtype(results->value));
		ECPGdump_a_type(yyout, v->name, v->type, NULL, NULL, NULL, NULL);
	}
	drop_assignments();
	fputs("ECPGd_EODT);\n",yyout);
	
	whenever_action(2|1);
}
