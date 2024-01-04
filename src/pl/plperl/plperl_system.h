/*-------------------------------------------------------------------------
 *
 * plperl_system.h
 *	  Pull in Perl's system header files.
 *
 * We break this out as a separate header file to precisely control
 * the scope of the "system_header" pragma.  No Postgres-specific
 * declarations should be put here.  However, we do include some stuff
 * that is meant to prevent conflicts between our code and Perl.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1995, Regents of the University of California
 *
 * src/pl/plperl/plperl_system.h
 */

#ifndef PL_PERL_SYSTEM_H
#define PL_PERL_SYSTEM_H

/*
 * Newer versions of the perl headers trigger a lot of warnings with our
 * preferred compiler flags (at least -Wdeclaration-after-statement,
 * -Wshadow=compatible-local are known to be problematic). The system_header
 * pragma hides warnings from within the rest of this file, if supported.
 */
#ifdef HAVE_PRAGMA_GCC_SYSTEM_HEADER
#pragma GCC system_header
#endif

/* stop perl headers from hijacking stdio and other stuff on Windows */
#ifdef WIN32
#define WIN32IO_IS_STDIO
#endif							/* WIN32 */

/*
 * Supply a value of PERL_UNUSED_DECL that will satisfy gcc - the one
 * perl itself supplies doesn't seem to.
 */
#define PERL_UNUSED_DECL pg_attribute_unused()

/*
 * Sometimes perl carefully scribbles on our *printf macros.
 * So we undefine them here and redefine them after it's done its dirty deed.
 */
#undef vsnprintf
#undef snprintf
#undef vsprintf
#undef sprintf
#undef vfprintf
#undef fprintf
#undef vprintf
#undef printf

/*
 * Perl scribbles on the "_" macro too.
 */
#undef _

/*
 * ActivePerl 5.18 and later are MinGW-built, and their headers use GCC's
 * __inline__.  Translate to something MSVC recognizes. Also, perl.h sometimes
 * defines isnan, so undefine it here and put back the definition later if
 * perl.h doesn't.
 */
#ifdef _MSC_VER
#define __inline__ inline
#ifdef isnan
#undef isnan
#endif
/* Work around for using MSVC and Strawberry Perl >= 5.30. */
#define __builtin_expect(expr, val) (expr)
#endif

/*
 * Regarding bool, both PostgreSQL and Perl might use stdbool.h or not,
 * depending on configuration.  If both agree, things are relatively harmless.
 * If not, things get tricky.  If PostgreSQL does but Perl does not, define
 * HAS_BOOL here so that Perl does not redefine bool; this avoids compiler
 * warnings.  If PostgreSQL does not but Perl does, we need to undefine bool
 * after we include the Perl headers; see below.
 */
#ifdef PG_USE_STDBOOL
#define HAS_BOOL 1
#endif

/*
 * Get the basic Perl API.  We use PERL_NO_GET_CONTEXT mode so that our code
 * can compile against MULTIPLICITY Perl builds without including XSUB.h.
 */
#define PERL_NO_GET_CONTEXT
#include "EXTERN.h"
#include "perl.h"

/*
 * We want to include XSUB.h only within .xs files, because on some platforms
 * it undesirably redefines a lot of libc functions.  But it must appear
 * before ppport.h, so use a #define flag to control inclusion here.
 */
#ifdef PG_NEED_PERL_XSUB_H
/*
 * On Windows, win32_port.h defines macros for a lot of these same functions.
 * To avoid compiler warnings when XSUB.h redefines them, #undef our versions.
 */
#ifdef WIN32
#undef accept
#undef bind
#undef connect
#undef fopen
#undef fstat
#undef kill
#undef listen
#undef lstat
#undef mkdir
#undef open
#undef putenv
#undef recv
#undef rename
#undef select
#undef send
#undef socket
#undef stat
#undef unlink
#endif

#include "XSUB.h"
#endif

/* put back our *printf macros ... this must match src/include/port.h */
#ifdef vsnprintf
#undef vsnprintf
#endif
#ifdef snprintf
#undef snprintf
#endif
#ifdef vsprintf
#undef vsprintf
#endif
#ifdef sprintf
#undef sprintf
#endif
#ifdef vfprintf
#undef vfprintf
#endif
#ifdef fprintf
#undef fprintf
#endif
#ifdef vprintf
#undef vprintf
#endif
#ifdef printf
#undef printf
#endif

#define vsnprintf		pg_vsnprintf
#define snprintf		pg_snprintf
#define vsprintf		pg_vsprintf
#define sprintf			pg_sprintf
#define vfprintf		pg_vfprintf
#define fprintf			pg_fprintf
#define vprintf			pg_vprintf
#define printf(...)		pg_printf(__VA_ARGS__)

/*
 * Put back "_" too; but rather than making it just gettext() as the core
 * code does, make it dgettext() so that the right things will happen in
 * loadable modules (if they've set up TEXTDOMAIN correctly).  Note that
 * we can't just set TEXTDOMAIN here, because this file is used by more
 * extensions than just PL/Perl itself.
 */
#undef _
#define _(x) dgettext(TEXTDOMAIN, x)

/* put back the definition of isnan if needed */
#ifdef _MSC_VER
#ifndef isnan
#define isnan(x) _isnan(x)
#endif
#endif

/* perl version and platform portability */
#include "ppport.h"

/*
 * perl might have included stdbool.h.  If we also did that earlier (see c.h),
 * then that's fine.  If not, we probably rejected it for some reason.  In
 * that case, undef bool and proceed with our own bool.  (Note that stdbool.h
 * makes bool a macro, but our own replacement is a typedef, so the undef
 * makes ours visible again).
 */
#ifndef PG_USE_STDBOOL
#ifdef bool
#undef bool
#endif
#endif

/* supply HeUTF8 if it's missing - ppport.h doesn't supply it, unfortunately */
#ifndef HeUTF8
#define HeUTF8(he)			   ((HeKLEN(he) == HEf_SVKEY) ?			   \
								SvUTF8(HeKEY_sv(he)) :				   \
								(U32)HeKUTF8(he))
#endif

/* supply GvCV_set if it's missing - ppport.h doesn't supply it, unfortunately */
#ifndef GvCV_set
#define GvCV_set(gv, cv)		(GvCV(gv) = cv)
#endif

/* Perl 5.19.4 changed array indices from I32 to SSize_t */
#if PERL_BCDVERSION >= 0x5019004
#define AV_SIZE_MAX SSize_t_MAX
#else
#define AV_SIZE_MAX I32_MAX
#endif

#endif							/* PL_PERL_SYSTEM_H */
