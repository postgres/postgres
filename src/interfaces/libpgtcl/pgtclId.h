/*-------------------------------------------------------------------------
*
* pgtclId.h--
*    useful routines to convert between strings and pointers
*  Needed because everything in tcl is a string, but often, pointers
*  to data structures are needed.
*    
*
* Copyright (c) 1994, Regents of the University of California
*
* $Id: pgtclId.h,v 1.6 1998/03/15 08:03:00 scrappy Exp $
*
*-------------------------------------------------------------------------
*/
  
extern void PgSetConnectionId(Tcl_Interp *interp, PGconn *conn);

#if (TCL_MAJOR_VERSION == 7 && TCL_MINOR_VERSION == 5)
# define DRIVER_DEL_PROTO ClientData cData, Tcl_Interp *interp, \
 	Tcl_File inFile, Tcl_File outFile
# define DRIVER_OUTPUT_PROTO ClientData cData, Tcl_File outFile, char *buf, \
	int bufSize, int *errorCodePtr
# define DRIVER_INPUT_PROTO ClientData cData, Tcl_File inFile, char *buf, \
	int bufSize, int *errorCodePtr
#else
# define DRIVER_OUTPUT_PROTO ClientData cData, char *buf, int bufSize, \
	int *errorCodePtr
# define DRIVER_INPUT_PROTO ClientData cData, char *buf, int bufSize, \
	int *errorCodePtr
# define DRIVER_DEL_PROTO ClientData cData, Tcl_Interp *interp
#endif

extern PGconn *PgGetConnectionId(Tcl_Interp *interp, char *id, \
	Pg_ConnectionId **);
extern PgDelConnectionId(DRIVER_DEL_PROTO);
extern int PgOutputProc(DRIVER_OUTPUT_PROTO);
extern PgInputProc(DRIVER_INPUT_PROTO);
extern int PgSetResultId(Tcl_Interp *interp, char *connid, PGresult *res);
extern PGresult *PgGetResultId(Tcl_Interp *interp, char *id);
extern void PgDelResultId(Tcl_Interp *interp, char *id);
extern int PgGetConnByResultId(Tcl_Interp *interp, char *resid);

#if (TCL_MAJOR_VERSION < 8)
extern Tcl_File PgGetFileProc(ClientData cData, int direction);
#endif

extern Tcl_ChannelType Pg_ConnType;
