/*-------------------------------------------------------
 *
 * $Id: Pg.xs,v 1.11 1999/02/11 23:25:16 tgl Exp $
 *
 * Copyright (c) 1997, 1998  Edmund Mergl
 *
 *-------------------------------------------------------*/

#include "EXTERN.h"
#include "perl.h"
#include "XSUB.h"
#include <string.h>
#include <stdio.h>
#include <fcntl.h>

#include "libpq-fe.h"

typedef struct pg_conn *PG_conn;
typedef struct pg_result *PG_result;

typedef struct pg_results
{
  PGresult *result;
  int row;
} PGresults;

typedef struct pg_results *PG_results;


static double
constant(name, arg)
char *name;
int arg; {
    errno = 0;
    switch (*name) {
    case 'A':
	break;
    case 'B':
	break;
    case 'C':
	break;
    case 'D':
	break;
    case 'E':
	break;
    case 'F':
	break;
    case 'G':
	break;
    case 'H':
	break;
    case 'I':
	break;
    case 'J':
	break;
    case 'K':
	break;
    case 'L':
	break;
    case 'M':
	break;
    case 'N':
	break;
    case 'O':
	break;
    case 'P':
	if (strEQ(name, "PGRES_CONNECTION_OK"))
	return 0;
	if (strEQ(name, "PGRES_CONNECTION_BAD"))
	return 1;
	if (strEQ(name, "PGRES_INV_SMGRMASK"))
	return 0x0000ffff;
	if (strEQ(name, "PGRES_INV_ARCHIVE"))
	return 0x00010000;
	if (strEQ(name, "PGRES_INV_WRITE"))
	return 0x00020000;
	if (strEQ(name, "PGRES_INV_READ"))
	return 0x00040000;
	if (strEQ(name, "PGRES_InvalidOid"))
	return 0;
	if (strEQ(name, "PGRES_EMPTY_QUERY"))
	return 0;
	if (strEQ(name, "PGRES_COMMAND_OK"))
	return 1;
	if (strEQ(name, "PGRES_TUPLES_OK"))
	return 2;
	if (strEQ(name, "PGRES_COPY_OUT"))
	return 3;
	if (strEQ(name, "PGRES_COPY_IN"))
	return 4;
	if (strEQ(name, "PGRES_BAD_RESPONSE"))
	return 5;
	if (strEQ(name, "PGRES_NONFATAL_ERROR"))
	return 6;
	if (strEQ(name, "PGRES_FATAL_ERROR"))
	return 7;
	break;
    case 'Q':
	break;
    case 'R':
	break;
    case 'S':
	break;
    case 'T':
	break;
    case 'U':
	break;
    case 'V':
	break;
    case 'W':
	break;
    case 'X':
	break;
    case 'Y':
	break;
    case 'Z':
	break;
    case 'a':
	break;
    case 'b':
	break;
    case 'c':
	break;
    case 'd':
	break;
    case 'e':
	break;
    case 'f':
	break;
    case 'g':
	break;
    case 'h':
	break;
    case 'i':
	break;
    case 'j':
	break;
    case 'k':
	break;
    case 'l':
	break;
    case 'm':
	break;
    case 'n':
	break;
    case 'o':
	break;
    case 'p':
	break;
    case 'q':
	break;
    case 'r':
	break;
    case 's':
	break;
    case 't':
	break;
    case 'u':
	break;
    case 'v':
	break;
    case 'w':
	break;
    case 'x':
	break;
    case 'y':
	break;
    case 'z':
	break;
    }
    errno = EINVAL;
    return 0;

not_there:
    errno = ENOENT;
    return 0;
}




MODULE = Pg		PACKAGE = Pg

PROTOTYPES: DISABLE


double
constant(name,arg)
	char *		name
	int		arg


PGconn *
PQconnectdb(conninfo)
	char *	conninfo
	CODE:
		/* convert dbname to lower case if not surrounded by double quotes */
		char *ptr = strstr(conninfo, "dbname");
		if (ptr) {
			while (*ptr && *ptr != '=') {
				ptr++;
			}
                        ptr++;
			while (*ptr == ' ' || *ptr == '\t') {
				ptr++;
			}
			if (*ptr == '"') {
				*ptr++ = ' ';
				while (*ptr && *ptr != '"') {
					ptr++;
				}
				if (*ptr == '"') {
					*ptr++ = ' ';
				}
			} else {
				while (*ptr && *ptr != ' ' && *ptr != '\t') {
				      *ptr = tolower(*ptr);
				      ptr++;
				}
			}
		}
		RETVAL = PQconnectdb((const char *)conninfo);
	OUTPUT:
		RETVAL


PGconn *
PQsetdbLogin(pghost, pgport, pgoptions, pgtty, dbname, login, pwd)
	char *	pghost
	char *	pgport
	char *	pgoptions
	char *	pgtty
	char *	dbname
	char *	login
	char *	pwd


PGconn *
PQsetdb(pghost, pgport, pgoptions, pgtty, dbname)
	char *	pghost
	char *	pgport
	char *	pgoptions
	char *	pgtty
	char *	dbname


HV *
PQconndefaults()
	CODE:
		PQconninfoOption *infoOption;
		RETVAL = newHV();
                if (infoOption = PQconndefaults()) {
			while (infoOption->keyword != NULL) {
				if (infoOption->val != NULL) {
					hv_store(RETVAL, infoOption->keyword, strlen(infoOption->keyword), newSVpv(infoOption->val, 0), 0);
				} else {
					hv_store(RETVAL, infoOption->keyword, strlen(infoOption->keyword), newSVpv("", 0), 0);
				}
				infoOption++;
			}
		}
	OUTPUT:
		RETVAL


void
PQfinish(conn)
	PGconn *	conn


void
PQreset(conn)
	PGconn *	conn


int
PQrequestCancel(conn)
	PGconn *	conn

char *
PQdb(conn)
	PGconn *	conn


char *
PQuser(conn)
	PGconn *	conn


char *
PQpass(conn)
	PGconn *	conn


char *
PQhost(conn)
	PGconn *	conn


char *
PQport(conn)
	PGconn *	conn


char *
PQtty(conn)
	PGconn *	conn


char *
PQoptions(conn)
	PGconn *	conn


ConnStatusType
PQstatus(conn)
	PGconn *	conn


char *
PQerrorMessage(conn)
	PGconn *	conn


int
PQsocket(conn)
	PGconn *	conn


int
PQbackendPID(conn)
	PGconn *	conn


void
PQtrace(conn, debug_port)
	PGconn *	conn
	FILE *	debug_port


void
PQuntrace(conn)
	PGconn *	conn



PGresult *
PQexec(conn, query)
	PGconn *	conn
	char *	query
	CODE:
		RETVAL = PQexec(conn, query);
		if (! RETVAL) {
			RETVAL = PQmakeEmptyPGresult(conn, PGRES_FATAL_ERROR);
		}
	OUTPUT:
		RETVAL


void
PQnotifies(conn)
	PGconn *	conn
	PREINIT:
		PGnotify *notify;
	PPCODE:
		notify = PQnotifies(conn);
		if (notify) {
			XPUSHs(sv_2mortal(newSVpv((char *)notify->relname, 0)));
			XPUSHs(sv_2mortal(newSViv(notify->be_pid)));
			free(notify);
		}


int
PQsendQuery(conn, query)
	PGconn *	conn
	char *	query


PGresult *
PQgetResult(conn)
	PGconn *	conn
	CODE:
		RETVAL = PQgetResult(conn);
		if (! RETVAL) {
			RETVAL = PQmakeEmptyPGresult(conn, PGRES_FATAL_ERROR);
		}
	OUTPUT:
		RETVAL


int
PQisBusy(conn)
	PGconn *	conn


int
PQconsumeInput(conn)
	PGconn *	conn


int
PQgetline(conn, string, length)
	PREINIT:
		SV *bufsv = SvROK(ST(1)) ? SvRV(ST(1)) : ST(1);
	INPUT:
		PGconn *	conn
		int	length
		char *	string = sv_grow(bufsv, length);
	CODE:
		RETVAL = PQgetline(conn, string, length);
	OUTPUT:
		RETVAL
		string


int
PQputline(conn, string)
	PGconn *	conn
	char *	string


int
PQgetlineAsync(conn, buffer, bufsize)
	PREINIT:
		SV *bufsv = SvROK(ST(1)) ? SvRV(ST(1)) : ST(1);
	INPUT:
		PGconn *	conn
		int	bufsize
		char *	buffer = sv_grow(bufsv, bufsize);
	CODE:
		RETVAL = PQgetlineAsync(conn, buffer, bufsize);
	OUTPUT:
		RETVAL
		buffer


int
PQputnbytes(conn, buffer, nbytes)
	PGconn *	conn
	char *	buffer
	int	nbytes


int
PQendcopy(conn)
	PGconn *	conn


PGresult *
PQmakeEmptyPGresult(conn, status)
	PGconn *	conn
	ExecStatusType	status


ExecStatusType
PQresultStatus(res)
	PGresult *	res


int
PQntuples(res)
	PGresult *	res


int
PQnfields(res)
	PGresult *	res


int
PQbinaryTuples(res)
	PGresult *	res


char *
PQfname(res, field_num)
	PGresult *	res
	int	field_num


int
PQfnumber(res, field_name)
	PGresult *	res
	char *	field_name


Oid
PQftype(res, field_num)
	PGresult *	res
	int	field_num


short
PQfsize(res, field_num)
	PGresult *	res
	int	field_num


int
PQfmod(res, field_num)
	PGresult *	res
	int	field_num


char *
PQcmdStatus(res)
	PGresult *	res


char *
PQoidStatus(res)
	PGresult *	res
	CODE:
		RETVAL = (char *)PQoidStatus(res);
	OUTPUT:
		RETVAL


char *
PQcmdTuples(res)
	PGresult *	res
	CODE:
		RETVAL = (char *)PQcmdTuples(res);
	OUTPUT:
		RETVAL


char *
PQgetvalue(res, tup_num, field_num)
	PGresult *	res
	int	tup_num
	int	field_num


int
PQgetlength(res, tup_num, field_num)
	PGresult *	res
	int	tup_num
	int	field_num


int
PQgetisnull(res, tup_num, field_num)
	PGresult *	res
	int	tup_num
	int	field_num


void
PQclear(res)
	PGresult *	res


void
PQprint(fout, res, header, align, standard, html3, expanded, pager, fieldSep, tableOpt, caption, ...)
	FILE *	fout
	PGresult *	res
	pqbool	header
	pqbool	align
	pqbool	standard
	pqbool	html3
	pqbool	expanded
	pqbool	pager
	char *	fieldSep
	char *	tableOpt
	char *	caption
	PREINIT:
		PQprintOpt ps;
		int i;
	CODE:
		ps.header    = header;
		ps.align     = align;
		ps.standard  = standard;
		ps.html3     = html3;
		ps.expanded  = expanded;
		ps.pager     = pager;
		ps.fieldSep  = fieldSep;
		ps.tableOpt  = tableOpt;
		ps.caption   = caption;
		Newz(0, ps.fieldName, items + 1 - 11, char*);
		for (i = 11; i < items; i++) {
			ps.fieldName[i - 11] = (char *)SvPV(ST(i), na);
		}
		PQprint(fout, res, &ps);
		Safefree(ps.fieldName);


void
PQdisplayTuples(res, fp, fillAlign, fieldSep, printHeader, quiet)
	PGresult *	res
	FILE *	fp
	int	fillAlign
	char *	fieldSep
	int	printHeader
	int	quiet
	CODE:
		PQdisplayTuples(res, fp, fillAlign, (const char *)fieldSep, printHeader, quiet);


void
PQprintTuples(res, fout, printAttName, terseOutput, width)
	PGresult *	res
	FILE *	fout
	int	printAttName
	int	terseOutput
	int	width


int
lo_open(conn, lobjId, mode)
	PGconn *	conn
	Oid	lobjId
	int	mode
	ALIAS:
		PQlo_open = 1


int
lo_close(conn, fd)
	PGconn *	conn
	int	fd
	ALIAS:
		PQlo_close = 1


int
lo_read(conn, fd, buf, len)
	ALIAS:
		PQlo_read = 1
	PREINIT:
		SV *bufsv = SvROK(ST(2)) ? SvRV(ST(2)) : ST(2);
	INPUT:
		PGconn *	conn
		int	fd
		int	len
		char *	buf = sv_grow(bufsv, len + 1);
	CODE:
		RETVAL = lo_read(conn, fd, buf, len);
		if (RETVAL > 0) {
			SvCUR_set(bufsv, RETVAL);
			*SvEND(bufsv) = '\0';
		}
	OUTPUT:
		RETVAL
		buf

int
lo_write(conn, fd, buf, len)
	PGconn *	conn
	int	fd
	char *	buf
	int	len
	ALIAS:
		PQlo_write = 1


int
lo_lseek(conn, fd, offset, whence)
	PGconn *	conn
	int	fd
	int	offset
	int	whence
	ALIAS:
		PQlo_lseek = 1


Oid
lo_creat(conn, mode)
	PGconn *	conn
	int	mode
	ALIAS:
		PQlo_creat = 1


int
lo_tell(conn, fd)
	PGconn *	conn
	int	fd
	ALIAS:
		PQlo_tell = 1


int
lo_unlink(conn, lobjId)
	PGconn *	conn
	Oid	lobjId
	ALIAS:
		PQlo_unlink = 1


Oid
lo_import(conn, filename)
	PGconn *	conn
	char *	filename
	ALIAS:
		PQlo_import = 1


int
lo_export(conn, lobjId, filename)
	PGconn *	conn
	Oid	lobjId
	char *	filename
	ALIAS:
		PQlo_export = 1




PG_conn
connectdb(conninfo)
	char *	conninfo
	CODE:
		/* convert dbname to lower case if not surrounded by double quotes */
		char *ptr = strstr(conninfo, "dbname");
		if (ptr) {
			ptr += 6;
			while (*ptr && *ptr++ != '=') {
				;
			}
			while (*ptr && (*ptr == ' ' || *ptr == '\t')) {
				ptr++;
			}
			if (*ptr == '"') {
				*ptr++ = ' ';
				while (*ptr && *ptr != '"') {
					ptr++;
				}
				if (*ptr == '"') {
					*ptr++ = ' ';
				}
			} else {
				while (*ptr && *ptr != ' ' && *ptr != '\t') {
					*ptr = tolower(*ptr);
					ptr++;
				}
			}
		}
		RETVAL = PQconnectdb((const char *)conninfo);
	OUTPUT:
		RETVAL


PG_conn
setdbLogin(pghost, pgport, pgoptions, pgtty, dbname, login, pwd)
	char *	pghost
	char *	pgport
	char *	pgoptions
	char *	pgtty
	char *	dbname
	char *	login
	char *	pwd
	CODE:
		RETVAL = PQsetdb(pghost, pgport, pgoptions, pgtty, dbname);
	OUTPUT:
		RETVAL


PG_conn
setdb(pghost, pgport, pgoptions, pgtty, dbname)
	char *	pghost
	char *	pgport
	char *	pgoptions
	char *	pgtty
	char *	dbname
	CODE:
		RETVAL = PQsetdb(pghost, pgport, pgoptions, pgtty, dbname);
	OUTPUT:
		RETVAL


HV *
conndefaults()
	CODE:
		PQconninfoOption *infoOption;
		RETVAL = newHV();
                if (infoOption = PQconndefaults()) {
			while (infoOption->keyword != NULL) {
				if (infoOption->val != NULL) {
					hv_store(RETVAL, infoOption->keyword, strlen(infoOption->keyword), newSVpv(infoOption->val, 0), 0);
				} else {
					hv_store(RETVAL, infoOption->keyword, strlen(infoOption->keyword), newSVpv("", 0), 0);
				}
				infoOption++;
			}
		}
	OUTPUT:
		RETVAL







MODULE = Pg		PACKAGE = PG_conn		PREFIX = PQ

PROTOTYPES: DISABLE


void
DESTROY(conn)
	PG_conn	conn
	CODE:
		/* printf("DESTROY connection\n"); */
		PQfinish(conn);


void
PQreset(conn)
	PG_conn	conn


int
PQrequestCancel(conn)
	PG_conn	conn


char *
PQdb(conn)
	PG_conn	conn


char *
PQuser(conn)
	PG_conn	conn


char *
PQpass(conn)
	PG_conn	conn


char *
PQhost(conn)
	PG_conn	conn


char *
PQport(conn)
	PG_conn	conn


char *
PQtty(conn)
	PG_conn	conn


char *
PQoptions(conn)
	PG_conn	conn


ConnStatusType
PQstatus(conn)
	PG_conn	conn


char *
PQerrorMessage(conn)
	PG_conn	conn


int
PQsocket(conn)
	PG_conn	conn


int
PQbackendPID(conn)
	PG_conn	conn


void
PQtrace(conn, debug_port)
	PG_conn	conn
	FILE *	debug_port


void
PQuntrace(conn)
	PG_conn	conn


PG_results
PQexec(conn, query)
	PG_conn	conn
	char *	query
	CODE:
		RETVAL = (PG_results)calloc(1, sizeof(PGresults));
		if (RETVAL) {
			RETVAL->result = PQexec((PGconn *)conn, query);
			if (!RETVAL->result) {
				RETVAL->result = PQmakeEmptyPGresult((PGconn *)conn, PGRES_FATAL_ERROR);
			}
		}
	OUTPUT:
		RETVAL


void
PQnotifies(conn)
	PG_conn	conn
	PREINIT:
		PGnotify *notify;
	PPCODE:
		notify = PQnotifies(conn);
		if (notify) {
			XPUSHs(sv_2mortal(newSVpv((char *)notify->relname, 0)));
			XPUSHs(sv_2mortal(newSViv(notify->be_pid)));
			free(notify);
		}


int
PQsendQuery(conn, query)
	PG_conn	conn
	char *	query


PG_results
PQgetResult(conn)
	PG_conn	conn
	CODE:
		RETVAL = (PG_results)calloc(1, sizeof(PGresults));
		if (RETVAL) {
			RETVAL->result = PQgetResult((PGconn *)conn);
			if (!RETVAL->result) {
				RETVAL->result = PQmakeEmptyPGresult((PGconn *)conn, PGRES_FATAL_ERROR);
			}
		}
	OUTPUT:
		RETVAL


int
PQisBusy(conn)
	PG_conn	conn


int
PQconsumeInput(conn)
	PG_conn	conn


int
PQgetline(conn, string, length)
	PREINIT:
		SV *bufsv = SvROK(ST(1)) ? SvRV(ST(1)) : ST(1);
	INPUT:
		PG_conn	conn
		int	length
		char *	string = sv_grow(bufsv, length);
	CODE:
		RETVAL = PQgetline(conn, string, length);
	OUTPUT:
		RETVAL
		string


int
PQputline(conn, string)
	PG_conn	conn
	char *	string


int
PQgetlineAsync(conn, buffer, bufsize)
	PREINIT:
		SV *bufsv = SvROK(ST(1)) ? SvRV(ST(1)) : ST(1);
	INPUT:
		PG_conn	conn
		int	bufsize
		char *	buffer = sv_grow(bufsv, bufsize);
	CODE:
		RETVAL = PQgetline(conn, buffer, bufsize);
	OUTPUT:
		RETVAL
		buffer


int
PQendcopy(conn)
	PG_conn	conn


PG_results
PQmakeEmptyPGresult(conn, status)
	PG_conn	conn
	ExecStatusType	status
	CODE:
		RETVAL = (PG_results)calloc(1, sizeof(PGresults));
		if (RETVAL) {
			RETVAL->result = PQmakeEmptyPGresult((PGconn *)conn, status);
		}
	OUTPUT:
		RETVAL


int
lo_open(conn, lobjId, mode)
	PG_conn	conn
	Oid	lobjId
	int	mode


int
lo_close(conn, fd)
	PG_conn	conn
	int	fd


int
lo_read(conn, fd, buf, len)
	PREINIT:
		SV *bufsv = SvROK(ST(2)) ? SvRV(ST(2)) : ST(2);
	INPUT:
		PG_conn	conn
		int	fd
		int	len
		char *	buf = sv_grow(bufsv, len + 1);
	CODE:
		RETVAL = lo_read(conn, fd, buf, len);
		if (RETVAL > 0) {
			SvCUR_set(bufsv, RETVAL);
			*SvEND(bufsv) = '\0';
		}
	OUTPUT:
		RETVAL
		buf


int
lo_write(conn, fd, buf, len)
	PG_conn	conn
	int	fd
	char *	buf
	int	len


int
lo_lseek(conn, fd, offset, whence)
	PG_conn	conn
	int	fd
	int	offset
	int	whence


Oid
lo_creat(conn, mode)
	PG_conn	conn
	int	mode


int
lo_tell(conn, fd)
	PG_conn	conn
	int	fd


int
lo_unlink(conn, lobjId)
	PG_conn	conn
	Oid	lobjId


Oid
lo_import(conn, filename)
	PG_conn	conn
	char *	filename


int
lo_export(conn, lobjId, filename)
	PG_conn	conn
	Oid	lobjId
	char *	filename




MODULE = Pg		PACKAGE = PG_results		PREFIX = PQ

PROTOTYPES: DISABLE


void
DESTROY(res)
	PG_results	res
	CODE:
		/* printf("DESTROY result\n"); */
		PQclear(res->result);
		Safefree(res);

ExecStatusType
PQresultStatus(res)
	PG_results	res
	CODE:
		RETVAL = PQresultStatus(res->result);
	OUTPUT:
		RETVAL

int
PQntuples(res)
	PG_results	res
	CODE:
		RETVAL = PQntuples(res->result);
	OUTPUT:
		RETVAL


int
PQnfields(res)
	PG_results	res
	CODE:
		RETVAL = PQnfields(res->result);
	OUTPUT:
		RETVAL


int
PQbinaryTuples(res)
	PG_results	res
	CODE:
		RETVAL = PQbinaryTuples(res->result);
	OUTPUT:
		RETVAL


char *
PQfname(res, field_num)
	PG_results	res
	int	field_num
	CODE:
		RETVAL = PQfname(res->result, field_num);
	OUTPUT:
		RETVAL


int
PQfnumber(res, field_name)
	PG_results	res
	char *	field_name
	CODE:
		RETVAL = PQfnumber(res->result, field_name);
	OUTPUT:
		RETVAL


Oid
PQftype(res, field_num)
	PG_results	res
	int	field_num
	CODE:
		RETVAL = PQftype(res->result, field_num);
	OUTPUT:
		RETVAL


short
PQfsize(res, field_num)
	PG_results	res
	int	field_num
	CODE:
		RETVAL = PQfsize(res->result, field_num);
	OUTPUT:
		RETVAL


int
PQfmod(res, field_num)
	PG_results	res
	int	field_num
	CODE:
		RETVAL = PQfmod(res->result, field_num);
	OUTPUT:
		RETVAL


char *
PQcmdStatus(res)
	PG_results	res
	CODE:
		RETVAL = PQcmdStatus(res->result);
	OUTPUT:
		RETVAL


char *
PQoidStatus(res)
	PG_results	res
	CODE:
		RETVAL = (char *)PQoidStatus(res->result);
	OUTPUT:
		RETVAL


char *
PQcmdTuples(res)
	PG_results	res
	CODE:
		RETVAL = (char *)PQcmdTuples(res->result);
	OUTPUT:
		RETVAL


char *
PQgetvalue(res, tup_num, field_num)
	PG_results	res
	int	tup_num
	int	field_num
	CODE:
		RETVAL = PQgetvalue(res->result, tup_num, field_num);
	OUTPUT:
		RETVAL


int
PQgetlength(res, tup_num, field_num)
	PG_results	res
	int	tup_num
	int	field_num
	CODE:
		RETVAL = PQgetlength(res->result, tup_num, field_num);
	OUTPUT:
		RETVAL


int
PQgetisnull(res, tup_num, field_num)
	PG_results	res
	int	tup_num
	int	field_num
	CODE:
		RETVAL = PQgetisnull(res->result, tup_num, field_num);
	OUTPUT:
		RETVAL


void
PQfetchrow(res)
	PG_results	res
	PPCODE:
		if (res && res->result) {
			int cols = PQnfields(res->result);
			if (PQntuples(res->result) > res->row) {
				int col = 0;
				EXTEND(sp, cols);
				while (col < cols) {
					if (PQgetisnull(res->result, res->row, col)) {
						PUSHs(&sv_undef);
					} else {
						char *val = PQgetvalue(res->result, res->row, col);
						PUSHs(sv_2mortal((SV*)newSVpv(val, 0)));
					}
					++col;
				}
				++res->row;
			}
		}


void
PQprint(res, fout, header, align, standard, html3, expanded, pager, fieldSep, tableOpt, caption, ...)
	FILE *	fout
	PG_results	res
	pqbool	header
	pqbool	align
	pqbool	standard
	pqbool	html3
	pqbool	expanded
	pqbool	pager
	char *	fieldSep
	char *	tableOpt
	char *	caption
	PREINIT:
		PQprintOpt ps;
		int i;
	CODE:
		ps.header    = header;
		ps.align     = align;
		ps.standard  = standard;
		ps.html3     = html3;
		ps.expanded  = expanded;
		ps.pager     = pager;
		ps.fieldSep  = fieldSep;
		ps.tableOpt  = tableOpt;
		ps.caption   = caption;
		Newz(0, ps.fieldName, items + 1 - 11, char*);
		for (i = 11; i < items; i++) {
			ps.fieldName[i - 11] = (char *)SvPV(ST(i), na);
		}
		PQprint(fout, res->result, &ps);
		Safefree(ps.fieldName);


void
PQdisplayTuples(res, fp, fillAlign, fieldSep, printHeader, quiet)
	PG_results	res
	FILE *	fp
	int	fillAlign
	char *	fieldSep
	int	printHeader
	int	quiet
	CODE:
		PQdisplayTuples(res->result, fp, fillAlign, (const char *)fieldSep, printHeader, quiet);


void
PQprintTuples(res, fout, printAttName, terseOutput, width)
	PG_results	res
	FILE *	fout
	int	printAttName
	int	terseOutput
	int	width
	CODE:
		PQprintTuples(res->result, fout, printAttName, terseOutput, width);
