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
 *	  $Header: /cvsroot/pgsql/src/interfaces/libpq++/Attic/pgdatabase.cc,v 1.10 2001/05/09 17:29:10 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
 
#include "pgdatabase.h"


using namespace std;


// OBSOLESCENT (uses PQprint(), which is no longer being maintained)
void PgDatabase::DisplayTuples(FILE *out, 
	bool fillAlign, 
	const char* fieldSep, 
	bool printHeader,
	bool /* quiet */) const
{
	PQprintOpt po;

	po.header = printHeader;
	po.align = fillAlign;
	po.standard = po.html3 = po.expanded = po.pager = 0;
	po.fieldSep = const_cast<char *>(fieldSep);
	po.tableOpt = po.caption = 0;
	po.fieldName = 0;

	PQprint(out,pgResult,&po);
}



// OBSOLESCENT (uses PQprint(), which is no longer being maintained)
void PgDatabase::PrintTuples(FILE *out, 
	bool printAttName, 
	bool terseOutput,
	bool fillAlign) const
{
	PQprintOpt po;

	po.header = printAttName;
	po.align = fillAlign;
	po.standard = po.html3 = po.expanded = po.pager = 0;
	po.tableOpt = po.caption = 0;
	po.fieldSep = const_cast<char *>(terseOutput ? "" : "|");
	po.fieldName = 0;

	PQprint(out,pgResult,&po);
}



int PgDatabase::Tuples() const
{ 
return PQntuples(pgResult); 
}


int PgDatabase::CmdTuples() const
{
const char *a = PQcmdTuples(pgResult);
return a[0] ? atoi(a) : -1;
}


// TODO: Make const?
int PgDatabase::Fields()
{ 
return PQnfields(pgResult); 
}


const char* PgDatabase::FieldName(int field_num) const
{ 
return PQfname(pgResult, field_num); 
}


int PgDatabase::FieldNum(const char* field_name) const
{ 
return PQfnumber(pgResult, field_name); 
}


Oid PgDatabase::FieldType(int field_num) const
{ 
return PQftype(pgResult, field_num); 
}


Oid PgDatabase::FieldType(const char* field_name) const
{ 
return PQftype(pgResult, FieldNum(field_name)); 
}


short PgDatabase::FieldSize(int field_num) const
{ 
return PQfsize(pgResult, field_num); 
}


short PgDatabase::FieldSize(const char* field_name) const
{ 
return PQfsize(pgResult, FieldNum(field_name)); 
}


const char* PgDatabase::GetValue(int tup_num, int field_num) const
{ 
return PQgetvalue(pgResult, tup_num, field_num); 
}


const char* PgDatabase::GetValue(int tup_num, const char* field_name) const
{ 
return PQgetvalue(pgResult, tup_num, FieldNum(field_name)); 
}


bool PgDatabase::GetIsNull(int tup_num, int field_num) const
{ 
return PQgetisnull(pgResult, tup_num, field_num); 
}


bool PgDatabase::GetIsNull(int tup_num, const char* field_name) const
{ 
return PQgetisnull(pgResult, tup_num, FieldNum(field_name)); 
}


int PgDatabase::GetLength(int tup_num, int field_num) const
{ 
return PQgetlength(pgResult, tup_num, field_num); 
}


int PgDatabase::GetLength(int tup_num, const char* field_name) const
{ 
return PQgetlength(pgResult, tup_num, FieldNum(field_name)); 
}


int PgDatabase::GetLine(char str[], int length)
{ 
return PQgetline(pgConn, str, length); 
}


void PgDatabase::PutLine(const char str[])
{ 
PQputline(pgConn, str); 
}


const char* PgDatabase::OidStatus() const
{ 
return PQoidStatus(pgResult); 
}


int PgDatabase::EndCopy()
{ 
return PQendcopy(pgConn); 
}


