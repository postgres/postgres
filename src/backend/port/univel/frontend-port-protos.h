/*-------------------------------------------------------------------------
 *
 * port-protos.h--
 *    port-specific prototypes for Intel x86/Intel SVR4
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * port-protos.h,v 1.2 1995/03/17 06:40:18 andrew Exp
 *
 *-------------------------------------------------------------------------
 */
#ifndef FPORT_PROTOS_H
#define FPORT_PROTOS_H

/* port.c */
extern long random(void);
extern void srandom(int seed);
extern int strcasecmp(char *s1,char *s2);
extern int gethostname(char *name,int namelen);

#endif /* FPORT_PROTOS_H */
