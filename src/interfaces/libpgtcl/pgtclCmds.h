/*-------------------------------------------------------------------------
 *
 * pgtclCmds.h
 *	  declarations for the C functions which implement pg_* tcl commands
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pgtclCmds.h,v 1.31 2003/08/04 02:40:16 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef PGTCLCMDS_H
#define PGTCLCMDS_H

#include <tcl.h>

#include "libpq-fe.h"

/* Hack to deal with Tcl 8.4 const-ification without losing compatibility */
#ifndef CONST84
#define CONST84
#endif

#define RES_HARD_MAX 128
#define RES_START 16

/*
 * From Tcl version 8.0 on we can make large object access binary.
 */
#ifdef TCL_MAJOR_VERSION
#if (TCL_MAJOR_VERSION >= 8)
#define PGTCL_USE_TCLOBJ
#endif
#endif

/*
 * Each Pg_ConnectionId has a list of Pg_TclNotifies structs, one for each
 * Tcl interpreter that has executed any pg_listens on the connection.
 * We need this arrangement to be able to clean up if an interpreter is
 * deleted while the connection remains open.  A free side benefit is that
 * multiple interpreters can be registered to listen for the same notify
 * name.  (All their callbacks will be called, but in an unspecified order.)
 *
 * We use the same approach for pg_on_connection_loss callbacks, but they
 * are not kept in a hashtable since there's no name associated.
 */

typedef struct Pg_TclNotifies_s
{
	struct Pg_TclNotifies_s *next;		/* list link */
	Tcl_Interp *interp;			/* This Tcl interpreter */

	/*
	 * NB: if interp == NULL, the interpreter is gone but we haven't yet
	 * got round to deleting the Pg_TclNotifies structure.
	 */
	Tcl_HashTable notify_hash;	/* Active pg_listen requests */

	char	   *conn_loss_cmd;	/* pg_on_connection_loss cmd, or NULL */
}	Pg_TclNotifies;

typedef struct Pg_ConnectionId_s
{
	char		id[32];
	PGconn	   *conn;
	int			res_max;		/* Max number of results allocated */
	int			res_hardmax;	/* Absolute max to allow */
	int			res_count;		/* Current count of active results */
	int			res_last;		/* Optimize where to start looking */
	int			res_copy;		/* Query result with active copy */
	int			res_copyStatus; /* Copying status */
	PGresult  **results;		/* The results */

	Pg_TclNotifies *notify_list;	/* head of list of notify info */
	int			notifier_running;		/* notify event source is live */
#if TCL_MAJOR_VERSION >= 8
	Tcl_Channel notifier_channel;		/* Tcl_Channel on which notifier
										 * is listening */
#else
	int			notifier_socket;	/* PQsocket on which notifier is listening */
#endif
}	Pg_ConnectionId;

/* Values of res_copyStatus */
#define RES_COPY_NONE	0
#define RES_COPY_INPROGRESS 1
#define RES_COPY_FIN	2


/* **************************/
/* registered Tcl functions */
/* **************************/
extern int Pg_conndefaults(ClientData cData, Tcl_Interp *interp,
				int argc, CONST84 char *argv[]);
extern int Pg_connect(ClientData cData, Tcl_Interp *interp,
		   int argc, CONST84 char *argv[]);
extern int Pg_disconnect(ClientData cData, Tcl_Interp *interp,
			  int argc, CONST84 char *argv[]);
extern int Pg_exec(ClientData cData, Tcl_Interp *interp,
		int argc, CONST84 char *argv[]);
extern int Pg_execute(ClientData cData, Tcl_Interp *interp,
		   int argc, CONST84 char *argv[]);
extern int Pg_select(ClientData cData, Tcl_Interp *interp,
		  int argc, CONST84 char *argv[]);
extern int Pg_result(ClientData cData, Tcl_Interp *interp,
		  int argc, CONST84 char *argv[]);
extern int Pg_lo_open(ClientData cData, Tcl_Interp *interp,
		   int argc, CONST84 char *argv[]);
extern int Pg_lo_close(ClientData cData, Tcl_Interp *interp,
			int argc, CONST84 char *argv[]);

#ifdef PGTCL_USE_TCLOBJ
extern int Pg_lo_read(ClientData cData, Tcl_Interp *interp, int objc,
		   Tcl_Obj *CONST objv[]);
extern int Pg_lo_write(ClientData cData, Tcl_Interp *interp, int objc,
			Tcl_Obj *CONST objv[]);

#else
extern int Pg_lo_read(ClientData cData, Tcl_Interp *interp,
		   int argc, CONST84 char *argv[]);
extern int Pg_lo_write(ClientData cData, Tcl_Interp *interp,
			int argc, CONST84 char *argv[]);
#endif
extern int Pg_lo_lseek(ClientData cData, Tcl_Interp *interp,
			int argc, CONST84 char *argv[]);
extern int Pg_lo_creat(ClientData cData, Tcl_Interp *interp,
			int argc, CONST84 char *argv[]);
extern int Pg_lo_tell(ClientData cData, Tcl_Interp *interp,
		   int argc, CONST84 char *argv[]);
extern int Pg_lo_unlink(ClientData cData, Tcl_Interp *interp,
			 int argc, CONST84 char *argv[]);
extern int Pg_lo_import(ClientData cData, Tcl_Interp *interp,
			 int argc, CONST84 char *argv[]);
extern int Pg_lo_export(ClientData cData, Tcl_Interp *interp,
			 int argc, CONST84 char *argv[]);
extern int Pg_listen(ClientData cData, Tcl_Interp *interp,
		  int argc, CONST84 char *argv[]);
extern int Pg_on_connection_loss(ClientData cData, Tcl_Interp *interp,
					  int argc, CONST84 char *argv[]);

#endif   /* PGTCLCMDS_H */
