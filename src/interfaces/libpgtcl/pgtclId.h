/*-------------------------------------------------------------------------
 *
 * pgtclId.h--
 *
 *	Contains Tcl "channel" interface routines, plus useful routines
 *	to convert between strings and pointers.  These are needed because
 *	everything in Tcl is a string, but in C, pointers to data structures
 *	are needed.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pgtclId.h,v 1.9 1998/09/21 01:02:04 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

extern void PgSetConnectionId(Tcl_Interp * interp, PGconn *conn);

#if TCL_MAJOR_VERSION == 7 && TCL_MINOR_VERSION == 5
/* Only Tcl 7.5 had drivers with this signature */
#define DRIVER_DEL_PROTO ClientData cData, Tcl_Interp *interp, \
	Tcl_File inFile, Tcl_File outFile
#define DRIVER_OUTPUT_PROTO ClientData cData, Tcl_File outFile, char *buf, \
	int bufSize, int *errorCodePtr
#define DRIVER_INPUT_PROTO ClientData cData, Tcl_File inFile, char *buf, \
	int bufSize, int *errorCodePtr
#else
/* Tcl 7.6 and beyond use this signature */
#define DRIVER_OUTPUT_PROTO ClientData cData, char *buf, int bufSize, \
	int *errorCodePtr
#define DRIVER_INPUT_PROTO ClientData cData, char *buf, int bufSize, \
	int *errorCodePtr
#define DRIVER_DEL_PROTO ClientData cData, Tcl_Interp *interp
#endif

extern PGconn *PgGetConnectionId(Tcl_Interp * interp, char *id,
								 Pg_ConnectionId **);
extern		PgDelConnectionId(DRIVER_DEL_PROTO);
extern int	PgOutputProc(DRIVER_OUTPUT_PROTO);
extern		PgInputProc(DRIVER_INPUT_PROTO);
extern int	PgSetResultId(Tcl_Interp * interp, char *connid, PGresult *res);
extern PGresult *PgGetResultId(Tcl_Interp * interp, char *id);
extern void PgDelResultId(Tcl_Interp * interp, char *id);
extern int	PgGetConnByResultId(Tcl_Interp * interp, char *resid);
extern void PgStartNotifyEventSource(Pg_ConnectionId * connid);
extern void PgStopNotifyEventSource(Pg_ConnectionId * connid);
extern void PgNotifyTransferEvents(Pg_ConnectionId * connid);
extern void PgNotifyInterpDelete(ClientData clientData, Tcl_Interp * interp);

/* GetFileProc is needed in Tcl 7.6 *only* ... it went away again in 8.0 */
#if TCL_MAJOR_VERSION == 7 && TCL_MINOR_VERSION >= 6
#define HAVE_TCL_GETFILEPROC 1
#else
#define HAVE_TCL_GETFILEPROC 0
#endif

#if HAVE_TCL_GETFILEPROC
extern Tcl_File PgGetFileProc(ClientData cData, int direction);
#endif

extern Tcl_ChannelType Pg_ConnType;
