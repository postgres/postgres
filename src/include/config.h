
/* the purpose of this file is to reduce the use of #ifdef's through
   the code base by those porting the software, an dto facilitate the
   eventual use of autoconf to build the server 
*/

#ifndef CONFIG_H
#define CONFIG_H

#define BLCKSZ	8192


#if defined(win32)
#  define WIN32
#  define NO_UNISTD_H
#  define USES_WINSOCK 
#  define NOFILE	100
#endif /* WIN32 */

#if defined(__FreeBSD__) || defined(__NetBSD__)
#  define USE_LIMITS_H
#endif

#if defined(bsdi)
#  define USE_LIMITS_H
#endif

#if defined(bsdi_2_1)
#  define USE_LIMITS_H
#endif

#if defined(aix)
#  define NEED_SYS_SELECT_H
#endif

#if defined(irix5)
#  define NO_VFORK
#endif

/*  Debug and various "defines" that should be documented */

/* found in function aclparse() in src/backend/utils/adt/acl.c */
/* #define ACLDEBUG */

/* found in src/backend/utils/adt/arrayfuncs.c */
/* #define LOARRAY */
#define ESCAPE_PATCH
#define ARRAY_PATCH


/* found in src/backend/utils/adt/date.c */
/* #define DATEDEBUG */

/* found in src/backend/utils/adt/datetimes.c */
/* #define USE_SHORT_YEAR */
/* #define AMERICAN_STYLE */

/*----------------------------------------*/
/* found in src/backend/utils/adt/float.c */
/*------------------------------------------------------*/
/* defining unsafe floats's will make float4 and float8 */
/* ops faster at the cost of safety, of course!         */
/*------------------------------------------------------*/
/* #define UNSAFE_FLOATS */

#endif /* CONFIG_H */

