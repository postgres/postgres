/*-------------------------------------------------------------------------
 *
 * module.h--
 *	  this file contains general "module" stuff  that used to be
 *	  spread out between the following files:
 *
 *		enbl.h					module enable stuff
 *		trace.h					module trace stuff (now gone)
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: module.h,v 1.2 1997/09/07 05:02:44 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef MODULE_H
#define MODULE_H

/*
 * prototypes for functions in init/enbl.c
 */
extern bool		BypassEnable(int *enableCountInOutP, bool on);

#endif							/* MODULE_H */
