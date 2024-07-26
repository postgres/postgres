/*-------------------------------------------------------------------------
 * injection_point.h
 *	  Definitions related to injection points.
 *
 * Copyright (c) 2001-2024, PostgreSQL Global Development Group
 *
 * src/include/utils/injection_point.h
 *-------------------------------------------------------------------------
 */

#ifndef INJECTION_POINT_H
#define INJECTION_POINT_H

/*
 * Injections points require --enable-injection-points.
 */
#ifdef USE_INJECTION_POINTS
#define INJECTION_POINT(name) InjectionPointRun(name)
#else
#define INJECTION_POINT(name) ((void) name)
#endif

/*
 * Typedef for callback function launched by an injection point.
 */
typedef void (*InjectionPointCallback) (const char *name,
										const void *private_data);

extern Size InjectionPointShmemSize(void);
extern void InjectionPointShmemInit(void);

extern void InjectionPointAttach(const char *name,
								 const char *library,
								 const char *function,
								 const void *private_data,
								 int private_data_size);
extern void InjectionPointRun(const char *name);
extern bool InjectionPointDetach(const char *name);

#ifdef EXEC_BACKEND
extern PGDLLIMPORT struct InjectionPointsCtl *ActiveInjectionPoints;
#endif

#endif							/* INJECTION_POINT_H */
