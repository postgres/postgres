/*-------------------------------------------------------------------------
 *
 * pgtclCmds.h--
 *    declarations for the C functions which implement pg_* tcl commands
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pgtclCmds.h,v 1.3 1996/10/30 06:18:40 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef PGTCLCMDS_H
#define PGTCLCMDS_H

#include "tcl.h"
#include "libpq/pqcomm.h"
#include "libpq-fe.h"
#include "libpq/libpq-fs.h"

typedef struct Pg_clientData_s {
    Tcl_HashTable	dbh_hash;
    Tcl_HashTable	res_hash;
    long		dbh_count;
    long		res_count;
} Pg_clientData;


typedef struct Pg_ConnectionId_s {
    char		id[32];
    PGconn		*conn;
    Tcl_HashTable	res_hash;
} Pg_ConnectionId;


typedef struct Pg_ResultId_s {
    char		id[32];
    PGresult		*result;
    Pg_ConnectionId	*connection;
} Pg_ResultId;


/* **************************/
/* registered Tcl functions */
/* **************************/
extern int Pg_connect(
    ClientData cData, Tcl_Interp *interp, int argc, char* argv[]);
extern int Pg_disconnect(
    ClientData cData, Tcl_Interp *interp, int argc, char* argv[]);
extern int Pg_exec(
    ClientData cData, Tcl_Interp *interp, int argc, char* argv[]);
extern int Pg_select(
    ClientData cData, Tcl_Interp *interp, int argc, char* argv[]);
extern int Pg_result(
    ClientData cData, Tcl_Interp *interp, int argc, char* argv[]);
extern int Pg_lo_open(
    ClientData cData, Tcl_Interp *interp, int argc, char* argv[]);
extern int Pg_lo_close(
    ClientData cData, Tcl_Interp *interp, int argc, char* argv[]);
extern int Pg_lo_read(
    ClientData cData, Tcl_Interp *interp, int argc, char* argv[]);
extern int Pg_lo_write(
    ClientData cData, Tcl_Interp *interp, int argc, char* argv[]);
extern int Pg_lo_lseek(
    ClientData cData, Tcl_Interp *interp, int argc, char* argv[]);
extern int Pg_lo_creat(
    ClientData cData, Tcl_Interp *interp, int argc, char* argv[]);
extern int Pg_lo_tell(
    ClientData cData, Tcl_Interp *interp, int argc, char* argv[]);
extern int Pg_lo_unlink(
    ClientData cData, Tcl_Interp *interp, int argc, char* argv[]);
extern int Pg_lo_import(
    ClientData cData, Tcl_Interp *interp, int argc, char* argv[]);
extern int Pg_lo_export(
    ClientData cData, Tcl_Interp *interp, int argc, char* argv[]);


#endif /*PGTCLCMDS_H*/

