/*-------------------------------------------------------------------------
 * injection_point.h
 *	  Definitions related to injection points.
 *
 * Copyright (c) 2001-2025, PostgreSQL Global Development Group
 *
 * src/include/utils/injection_point.h
 *-------------------------------------------------------------------------
 */

#ifndef INJECTION_POINT_H
#define INJECTION_POINT_H

#include "nodes/pg_list.h"

/*
 * Injection point data, used when retrieving a list of all the attached
 * injection points.
 */
typedef struct InjectionPointData
{
	const char *name;
	const char *library;
	const char *function;
} InjectionPointData;

/*
 * Injection points require --enable-injection-points.
 */
#ifdef USE_INJECTION_POINTS
#define INJECTION_POINT_LOAD(name) InjectionPointLoad(name)
#define INJECTION_POINT(name, arg) InjectionPointRun(name, arg)
#define INJECTION_POINT_CACHED(name, arg) InjectionPointCached(name, arg)
#define IS_INJECTION_POINT_ATTACHED(name) IsInjectionPointAttached(name)
#else
#define INJECTION_POINT_LOAD(name) ((void) name)
#define INJECTION_POINT(name, arg) ((void) name)
#define INJECTION_POINT_CACHED(name, arg) ((void) name)
#define IS_INJECTION_POINT_ATTACHED(name) (false)
#endif

/*
 * Typedef for callback function launched by an injection point.
 */
typedef void (*InjectionPointCallback) (const char *name,
										const void *private_data,
										void *arg);

extern Size InjectionPointShmemSize(void);
extern void InjectionPointShmemInit(void);

extern void InjectionPointAttach(const char *name,
								 const char *library,
								 const char *function,
								 const void *private_data,
								 int private_data_size);
extern void InjectionPointLoad(const char *name);
extern void InjectionPointRun(const char *name, void *arg);
extern void InjectionPointCached(const char *name, void *arg);
extern bool IsInjectionPointAttached(const char *name);
extern bool InjectionPointDetach(const char *name);

/* Get the current set of injection points attached */
extern List *InjectionPointList(void);

#ifdef EXEC_BACKEND
extern PGDLLIMPORT struct InjectionPointsCtl *ActiveInjectionPoints;
#endif

#endif							/* INJECTION_POINT_H */
