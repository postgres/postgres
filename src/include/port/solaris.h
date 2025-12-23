/* src/include/port/solaris.h */

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
