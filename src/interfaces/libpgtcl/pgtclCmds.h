/*-------------------------------------------------------------------------
 *
 * pgtclCmds.h--
 *	  declarations for the C functions which implement pg_* tcl commands
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pgtclCmds.h,v 1.9 1998/03/15 08:02:59 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef PGTCLCMDS_H
#define PGTCLCMDS_H

#include "tcl.h"
#include "libpq/pqcomm.h"
#include "libpq-fe.h"
#include "libpq/libpq-fs.h"

#define RES_HARD_MAX 128
#define RES_START 16

typedef struct Pg_ConnectionId_s {
    char		id[32];
    PGconn		*conn;
    int			res_max;	/* Max number of results allocated */
    int			res_hardmax;	/* Absolute max to allow */
    int			res_count;	/* Current count of active results */
    int			res_last;	/* Optimize where to start looking */
    int			res_copy;	/* Query result with active copy */
    int			res_copyStatus; /* Copying status */
    PGresult		**results;	/* The results */
    
    Tcl_HashTable	notify_hash;
} Pg_ConnectionId;


#define RES_COPY_NONE	0
#define RES_COPY_INPROGRESS	1
#define RES_COPY_FIN	2


/* **************************/
/* registered Tcl functions */
/* **************************/
extern int Pg_conndefaults(
    ClientData cData, Tcl_Interp *interp, int argc, char* argv[]);
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
extern int Pg_listen(
    ClientData cData, Tcl_Interp *interp, int argc, char* argv[]);
extern int Pg_notifies(
    ClientData cData, Tcl_Interp *interp, int argc, char* argv[]);


#endif /*PGTCLCMDS_H*/

