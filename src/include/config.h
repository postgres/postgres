
/* the purpose of this file is to reduce the use of #ifdef's through
   the code base by those porting the software, an dto facilitate the
   eventual use of autoconf to build the server 
*/

#if defined(WIN32)
#  define NO_UNISTD_H
#  define USES_WINSOCK 
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

