/*
 * functions needed for descriptor handling
 */

#include "postgres.h"
#include "extern.h" 

/*
 * assignment handling function (descriptor)
 */
 
struct assignment *assignments;

void push_assignment(char *var,char *value)
{
	struct assignment *new=(struct assignment *)mm_alloc(sizeof(struct assignment));
	
	new->next=assignments;
	new->variable=mm_alloc(strlen(var)+1);
	strcpy(new->variable,var);
	new->value=mm_alloc(strlen(value)+1);
	strcpy(new->value,value);
	assignments=new;
}

static void
drop_assignments(void)
{	while (assignments)
	{	struct assignment *old_head=assignments;

		assignments=old_head->next;
		free(old_head->variable);
		free(old_head->value);
		free(old_head);
	}
}

/* XXX: these should be more accurate (consider ECPGdump_a_* ) */
static void ECPGnumeric_lvalue(FILE *f,char *name)
{	const struct variable *v=find_variable(name);

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

static void ECPGstring_buffer(FILE *f,char *name)
{ 
	const struct variable *v=find_variable(name);

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

void
output_get_descr_header(char *desc_name)
{
	struct assignment *results;

	fprintf(yyout,"{\tPGresult *ECPGresult=ECPGresultByDescriptor(%d, \"%s\");\n" ,yylineno,desc_name);
	fputs("\tif (ECPGresult)\n\t{",yyout);
	for (results=assignments;results!=NULL;results=results->next)
	{
		if (!strcasecmp(results->value,"count"))
		{
			fputs("\t\t",yyout);
			ECPGnumeric_lvalue(yyout,results->variable);
			fputs("=PQnfields(ECPGresult);\n",yyout);
		}
		else
		{	snprintf(errortext,sizeof errortext,"unknown descriptor header item '%s'",results->value);
			mmerror(ET_WARN,errortext);
		}
	}
	drop_assignments();
	fputs("}",yyout);
	
	whenever_action(2|1);
}

void
output_get_descr(char *desc_name)
{
	struct assignment *results;
	int flags=0;
	const int DATA_SEEN=1;
	const int INDICATOR_SEEN=2;
	
	fprintf(yyout,"{\tPGresult *ECPGresult=ECPGresultByDescriptor(%d, \"%s\");\n"
													,yylineno,desc_name);
	fputs("\tif (ECPGresult)\n\t{",yyout);
	fprintf(yyout,"\tif (PQntuples(ECPGresult)<1) ECPGraise(%d,ECPG_NOT_FOUND);\n",yylineno);
	fprintf(yyout,"\t\telse if (%s<1 || %s>PQnfields(ECPGresult))\n"
			"\t\t\tECPGraise(%d,ECPG_INVALID_DESCRIPTOR_INDEX);\n"
				,descriptor_index,descriptor_index,yylineno);
	fputs("\t\telse\n\t\t{\n",yyout);
	for (results=assignments;results!=NULL;results=results->next)
	{
		if (!strcasecmp(results->value,"type"))
		{
			fputs("\t\t\t",yyout);
			ECPGnumeric_lvalue(yyout,results->variable);
			fprintf(yyout,"=ECPGDynamicType(PQftype(ECPGresult,(%s)-1));\n",descriptor_index);
		}
		else if (!strcasecmp(results->value,"datetime_interval_code"))
		{
			fputs("\t\t\t",yyout);
			ECPGnumeric_lvalue(yyout,results->variable);
			fprintf(yyout,"=ECPGDynamicType_DDT(PQftype(ECPGresult,(%s)-1));\n",descriptor_index);
		}
		else if (!strcasecmp(results->value,"length"))
		{
			fputs("\t\t\t",yyout);
			ECPGnumeric_lvalue(yyout,results->variable);
			fprintf(yyout,"=PQfmod(ECPGresult,(%s)-1)-VARHDRSZ;\n",descriptor_index);
		}
		else if (!strcasecmp(results->value,"octet_length"))
		{
			fputs("\t\t\t",yyout);
			ECPGnumeric_lvalue(yyout,results->variable);
			fprintf(yyout,"=PQfsize(ECPGresult,(%s)-1);\n",descriptor_index);
		}
		else if (!strcasecmp(results->value,"returned_length")
			|| !strcasecmp(results->value,"returned_octet_length"))
		{
			fputs("\t\t\t",yyout);
			ECPGnumeric_lvalue(yyout,results->variable);
			fprintf(yyout,"=PQgetlength(ECPGresult,0,(%s)-1);\n",descriptor_index);
		}
		else if (!strcasecmp(results->value,"precision"))
		{
			fputs("\t\t\t",yyout);
			ECPGnumeric_lvalue(yyout,results->variable);
			fprintf(yyout,"=PQfmod(ECPGresult,(%s)-1)>>16;\n",descriptor_index);
		}
		else if (!strcasecmp(results->value,"scale"))
		{
			fputs("\t\t\t",yyout);
			ECPGnumeric_lvalue(yyout,results->variable);
			fprintf(yyout,"=(PQfmod(ECPGresult,(%s)-1)-VARHDRSZ)&0xffff;\n",descriptor_index);
		}
		else if (!strcasecmp(results->value,"nullable"))
		{
			mmerror(ET_WARN,"nullable is always 1");
			fputs("\t\t\t",yyout);
			ECPGnumeric_lvalue(yyout,results->variable);
			fprintf(yyout,"=1;\n");
		}
		else if (!strcasecmp(results->value,"key_member"))
		{
			mmerror(ET_WARN,"key_member is always 0");
			fputs("\t\t\t",yyout);
			ECPGnumeric_lvalue(yyout,results->variable);
			fprintf(yyout,"=0;\n");
		}
		else if (!strcasecmp(results->value,"name"))
		{
			fputs("\t\t\tstrncpy(",yyout);
			ECPGstring_buffer(yyout,results->variable);
			fprintf(yyout,",PQfname(ECPGresult,(%s)-1),",descriptor_index);
			ECPGstring_length(yyout,results->variable);
			fputs(");\n",yyout);
		}
		else if (!strcasecmp(results->value,"indicator"))
		{
			flags|=INDICATOR_SEEN;
			fputs("\t\t\t",yyout);
			ECPGnumeric_lvalue(yyout,results->variable);
			fprintf(yyout,"=-PQgetisnull(ECPGresult,0,(%s)-1);\n",descriptor_index);
		}
		else if (!strcasecmp(results->value,"data"))
		{
			flags|=DATA_SEEN;
			ECPGdata_assignment(results->variable,descriptor_index);
		}
		else
		{
			snprintf(errortext,sizeof errortext,"unknown descriptor header item '%s'",results->value);
			mmerror(ET_WARN,errortext);
		}
	}
	if (flags==DATA_SEEN) /* no indicator */
	{
		fprintf(yyout,"\t\t\tif (PQgetisnull(ECPGresult,0,(%s)-1))\n"
					"\t\t\t\tECPGraise(%d,ECPG_MISSING_INDICATOR);\n"
				,descriptor_index,yylineno);
	}
	drop_assignments();
	fputs("\t\t}\n\t}\n",yyout);
	
	whenever_action(2|1);
}

/*
 * descriptor name lookup
 */
 
static struct descriptor *descriptors;

void add_descriptor(char *name,char *connection)
{
	struct descriptor *new=(struct descriptor *)mm_alloc(sizeof(struct descriptor));
	
	new->next=descriptors;
	new->name=mm_alloc(strlen(name)+1);
	strcpy(new->name,name);
	if (connection) 
	{	new->connection=mm_alloc(strlen(connection)+1);
		strcpy(new->connection,connection);
	}
	else new->connection=connection;
	descriptors=new;
}

void drop_descriptor(char *name,char *connection)
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

struct descriptor *lookup_descriptor(char *name,char *connection)
{
	struct descriptor *i;
	
	for (i=descriptors;i;i=i->next)
	{
		if (!strcmp(name,i->name))
		{
			if ((!connection && !i->connection) 
				|| (connection && i->connection 
					&& !strcmp(connection,i->connection)))
			{
				return i;
			}
		}
	}
	snprintf(errortext,sizeof errortext,"unknown descriptor %s",name);
	mmerror(ET_WARN,errortext);
	return NULL;
}

void
output_statement_desc(char * stmt, int mode)
{
	int i, j=strlen(stmt);

	fprintf(yyout, "{ ECPGdo_descriptor(__LINE__, %s, \"%s\", \"", 
		connection ? connection : "NULL", descriptor_name);

	/* do this char by char as we have to filter '\"' */
	for (i = 0;i < j; i++) {
		if (stmt[i] != '\"')
			fputc(stmt[i], yyout);
		else
			fputs("\\\"", yyout);
	}

	fputs("\");", yyout);

	mode |= 2;
	whenever_action(mode);
	free(stmt);
	if (connection != NULL)
		free(connection);
	free(descriptor_name);
}
