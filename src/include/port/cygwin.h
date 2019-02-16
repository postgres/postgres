/* src/include/port/cygwin.h */

#include <cygwin/version.h>

/*
 * Check for b20.1 and disable AF_UNIX family socket support.
 */
#if CYGWIN_VERSION_DLL_MAJOR < 1001
#undef HAVE_UNIX_SOCKETS
#endif

#ifdef BUILDING_DLL
#define PGDLLIMPORT __declspec (dllexport)
#else
#define PGDLLIMPORT __declspec (dllimport)
#endif

#define PGDLLEXPORT

/*
 * Cygwin has a strtof() which is literally just (float)strtod(), which means
 * we get misrounding _and_ silent over/underflow. Using our wrapper doesn't
 * fix the misrounding but does fix the error checks, which cuts down on the
 * number of test variant files needed.
 */
#define HAVE_BUGGY_STRTOF 1
