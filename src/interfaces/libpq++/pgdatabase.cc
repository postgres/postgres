/*-------------------------------------------------------------------------
 *
 *   FILE
 *      pgdatabase.cpp
 *
 *   DESCRIPTION
 *      implementation of the PgDatabase class.
 *   PgDatabase encapsulates some utility routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/libpq++/Attic/pgdatabase.cc,v 1.4 1999/09/21 21:19:31 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
 
#include "pgdatabase.h"


void PgDatabase::DisplayTuples(FILE *out, int fillAlign, 
							   const char* fieldSep, int printHeader,
							   int /* quiet */) 
{
	PQprintOpt po;

	memset(&po,0,sizeof(po));

	po.align = (pqbool)fillAlign;
	po.fieldSep = (char *)fieldSep;
	po.header = (pqbool)printHeader;

	PQprint(out,pgResult,&po);
}




void PgDatabase::PrintTuples(FILE *out, int printAttName, int terseOutput,
							 int width)
{
	PQprintOpt po;

	memset(&po,0,sizeof(po));

	po.align = (pqbool)width;

	if(terseOutput) po.fieldSep = strdup("|");
	else po.fieldSep = "";

	po.header = (pqbool)printAttName;

	PQprint(out,pgResult,&po);
}



int PgDatabase::Tuples()
{ 
return PQntuples(pgResult); 
}


int PgDatabase::Fields()
{ 
return PQnfields(pgResult); 
}


const char* PgDatabase::FieldName(int field_num)
{ 
return PQfname(pgResult, field_num); 
}


int PgDatabase::FieldNum(const char* field_name)
{ 
return PQfnumber(pgResult, field_name); 
}


Oid PgDatabase::FieldType(int field_num)
{ 
return PQftype(pgResult, field_num); 
}


Oid PgDatabase::FieldType(const char* field_name)
{ 
return PQftype(pgResult, FieldNum(field_name)); 
}


short PgDatabase::FieldSize(int field_num)
{ 
return PQfsize(pgResult, field_num); 
}


short PgDatabase::FieldSize(const char* field_name)
{ 
return PQfsize(pgResult, FieldNum(field_name)); 
}


const char* PgDatabase::GetValue(int tup_num, int field_num)
{ 
return PQgetvalue(pgResult, tup_num, field_num); 
}


const char* PgDatabase::GetValue(int tup_num, const char* field_name)
{ 
return PQgetvalue(pgResult, tup_num, FieldNum(field_name)); 
}


int PgDatabase::GetIsNull(int tup_num, int field_num)
{ 
return PQgetisnull(pgResult, tup_num, field_num); 
}


int PgDatabase::GetIsNull(int tup_num, const char* field_name)
{ 
return PQgetisnull(pgResult, tup_num, FieldNum(field_name)); 
}


int PgDatabase::GetLength(int tup_num, int field_num)
{ 
return PQgetlength(pgResult, tup_num, field_num); 
}


int PgDatabase::GetLength(int tup_num, const char* field_name)
{ 
return PQgetlength(pgResult, tup_num, FieldNum(field_name)); 
}


int PgDatabase::GetLine(char* string, int length)
{ 
return PQgetline(pgConn, string, length); 
}


void PgDatabase::PutLine(const char* string)
{ 
PQputline(pgConn, string); 
}


const char* PgDatabase::OidStatus()
{ 
return PQoidStatus(pgResult); 
}


int PgDatabase::EndCopy()
{ 
return PQendcopy(pgConn); 
}


