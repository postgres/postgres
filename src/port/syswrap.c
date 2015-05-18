/*-------------------------------------------------------------------------
 *
 * syswrap.c
 *	  error-throwing wrappers around POSIX functions that rarely fail
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/port/syswrap.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

/* Prevent recursion */
#undef	vsnprintf
#undef	snprintf
#undef	vsprintf
#undef	sprintf
#undef	vfprintf
#undef	fprintf
#undef	printf

/* When the libc primitives are lacking, use our own. */
#ifdef USE_REPL_SNPRINTF
#ifdef __GNUC__
#define vsnprintf(...)	pg_vsnprintf(__VA_ARGS__)
#define snprintf(...)	pg_snprintf(__VA_ARGS__)
#define vsprintf(...)	pg_vsprintf(__VA_ARGS__)
#define sprintf(...)	pg_sprintf(__VA_ARGS__)
#define vfprintf(...)	pg_vfprintf(__VA_ARGS__)
#define fprintf(...)	pg_fprintf(__VA_ARGS__)
#define printf(...)		pg_printf(__VA_ARGS__)
#else
#define vsnprintf		pg_vsnprintf
#define snprintf		pg_snprintf
#define vsprintf		pg_vsprintf
#define sprintf			pg_sprintf
#define vfprintf		pg_vfprintf
#define fprintf			pg_fprintf
#define printf			pg_printf
#endif
#endif   /* USE_REPL_SNPRINTF */

/*
 * We abort() in the frontend, rather than exit(), because libpq in particular
 * has no business calling exit().  These failures had better be rare.
 */
#ifdef FRONTEND
#define LIB_ERR(func) \
do { \
	int discard = fprintf(stderr, "%s failed: %s\n", func, strerror(errno)); \
	(void) discard; \
	abort(); \
} while (0)
#else
#define LIB_ERR(func) elog(ERROR, "%s failed: %m", func)
#endif

int
vsnprintf_throw_on_fail(char *str, size_t count, const char *fmt, va_list args)
{
	int			save_errno;
	int			ret;

	/*
	 * On HP-UX B.11.31, a call that truncates output returns -1 without
	 * setting errno.  (SUSv2 allowed this until the approval of Base Working
	 * Group Resolution BWG98-006.)  We could avoid the save and restore of
	 * errno on most platforms.
	 */
	save_errno = errno;
	errno = 0;
	ret = vsnprintf(str, count, fmt, args);
	if (ret < 0 && errno != 0)
		LIB_ERR("vsnprintf");
	errno = save_errno;
	return ret;
}

int
snprintf_throw_on_fail(char *str, size_t count, const char *fmt,...)
{
	int			ret;
	va_list		args;

	va_start(args, fmt);
	ret = vsnprintf_throw_on_fail(str, count, fmt, args);
	va_end(args);
	return ret;
}

int
vsprintf_throw_on_fail(char *str, const char *fmt, va_list args)
{
	int			ret;

	ret = vsprintf(str, fmt, args);
	if (ret < 0)
		LIB_ERR("vsprintf");
	return ret;
}

int
sprintf_throw_on_fail(char *str, const char *fmt,...)
{
	int			ret;
	va_list		args;

	va_start(args, fmt);
	ret = vsprintf_throw_on_fail(str, fmt, args);
	va_end(args);
	return ret;
}

int
vfprintf_throw_on_fail(FILE *stream, const char *fmt, va_list args)
{
	int			ret;

	ret = vfprintf(stream, fmt, args);
	if (ret < 0)
		LIB_ERR("vfprintf");
	return ret;
}

int
fprintf_throw_on_fail(FILE *stream, const char *fmt,...)
{
	int			ret;
	va_list		args;

	va_start(args, fmt);
	ret = vfprintf_throw_on_fail(stream, fmt, args);
	va_end(args);
	return ret;
}

int
printf_throw_on_fail(const char *fmt,...)
{
	int			ret;
	va_list		args;

	va_start(args, fmt);
	ret = vfprintf_throw_on_fail(stdout, fmt, args);
	va_end(args);
	return ret;
}
