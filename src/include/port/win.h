/* $Header: /cvsroot/pgsql/src/include/port/Attic/win.h,v 1.15 2003/03/21 17:18:34 petere Exp $ */

#define HAS_TEST_AND_SET

#ifdef BUILDING_DLL
#define DLLIMPORT __declspec (dllexport)
#else
#define DLLIMPORT __declspec (dllimport)
#endif

#if defined(_DLL)
#define DLLIMPORT __declspec (dllexport)
#else
#define DLLIMPORT __declspec (dllimport)
#endif
