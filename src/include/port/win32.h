#define USES_WINSOCK
#define NOFILE		  100

/* defines for dynamic linking on Win32 platform */
#ifdef __CYGWIN__

#if __GNUC__ && ! defined (__declspec)
#error You need egcs 1.1 or newer for compiling!
#endif

#ifdef BUILDING_DLL
#define DLLIMPORT __declspec (dllexport)
#else							/* not BUILDING_DLL */
#define DLLIMPORT __declspec (dllimport)
#endif

#elif defined(WIN32) && defined(_MSC_VER)		/* not CYGWIN */

#if defined(_DLL)
#define DLLIMPORT __declspec (dllexport)
#else							/* not _DLL */
#define DLLIMPORT __declspec (dllimport)
#endif

#else							/* not CYGWIN, not MSVC */

#define DLLIMPORT

#endif
