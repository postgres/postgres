/* src/include/port/solaris.h */

/*
 * Sort this out for all operating systems some time.  The __xxx
 * symbols are defined on both GCC and Solaris CC, although GCC
 * doesn't document them.  The __xxx__ symbols are only on GCC.
 */
#if defined(__i386) && !defined(__i386__)
#define __i386__
#endif

#if defined(__amd64) && !defined(__amd64__)
#define __amd64__
#endif

#if defined(__x86_64) && !defined(__x86_64__)
#define __x86_64__
#endif

#if defined(__sparc) && !defined(__sparc__)
#define __sparc__
#endif

#if defined(__i386__)
#include <sys/isa_defs.h>
#endif

/*
 * On original Solaris, PAM conversation procs lack a "const" in their
 * declaration; but recent OpenIndiana versions put it there by default.
 * The least messy way to deal with this is to define _PAM_LEGACY_NONCONST,
 * which causes OpenIndiana to declare pam_conv per the Solaris tradition,
 * and also use that symbol to control omitting the "const" in our own code.
 */
#define _PAM_LEGACY_NONCONST 1

/*
 * Many versions of Solaris have broken strtod() --- see bug #4751182.
 * This has been fixed in current versions of Solaris:
 *
 * http://sunsolve.sun.com/search/document.do?assetkey=1-21-108993-62-1&searchclause=108993-62
 * http://sunsolve.sun.com/search/document.do?assetkey=1-21-112874-34-1&searchclause=112874-34
 *
 * However, many people might not have patched versions, so
 * still use our own fix for the buggy version.
 */
#define HAVE_BUGGY_SOLARIS_STRTOD
