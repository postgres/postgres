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
 * $Id: pgtclId.h,v 1.2 1996/10/30 06:18:42 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

extern void PgSetConnectionId(Pg_clientData *cd, char *id, PGconn *conn);
extern PGconn *PgGetConnectionId(Pg_clientData *cd, char *id);
extern void PgDelConnectionId(Pg_clientData *cd, char *id);
extern void PgSetResultId(Pg_clientData *cd, char *id, char *connid, PGresult *res);
extern PGresult *PgGetResultId(Pg_clientData *cd, char *id);
extern void PgDelResultId(Pg_clientData *cd, char *id);
extern void PgGetConnByResultId(Pg_clientData *cd, char *id, char *resid);
