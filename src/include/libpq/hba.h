/*-------------------------------------------------------------------------
 *
 * hba.h--
 *    Interface to hba.c
 *
 *
 * $Id: hba.h,v 1.1 1996/10/11 09:12:17 bryanh Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef HBA_H
#define	HBA_H

#include <libpq/pqcomm.h>

extern int
hba_recvauth(const Port *port, const char database[], const char user[],
             const char DataDir[]);

#endif
