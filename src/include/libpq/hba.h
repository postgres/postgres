/*-------------------------------------------------------------------------
 *
 * hba.h--
 *    Interface to hba.c
 *
 *
 * $Id: hba.h,v 1.2 1996/11/06 10:29:58 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef HBA_H
#define	HBA_H


extern int
hba_recvauth(const Port *port, const char database[], const char user[],
             const char DataDir[]);

#endif
