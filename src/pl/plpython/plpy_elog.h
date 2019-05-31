/*
 * src/pl/plpython/plpy_elog.h
 */

#ifndef PLPY_ELOG_H
#define PLPY_ELOG_H

#include "plpython.h"

/* global exception classes */
extern PyObject *PLy_exc_error;
extern PyObject *PLy_exc_fatal;
extern PyObject *PLy_exc_spi_error;

/*
 * PLy_elog()
 *
 * See comments at elog() about the compiler hinting.
 */
#ifdef HAVE__BUILTIN_CONSTANT_P
#define PLy_elog(elevel, ...) \
	do { \
		PLy_elog_impl(elevel, __VA_ARGS__); \
		if (__builtin_constant_p(elevel) && (elevel) >= ERROR) \
			pg_unreachable(); \
	} while(0)
#else							/* !HAVE__BUILTIN_CONSTANT_P */
#define PLy_elog(elevel, ...)  \
	do { \
		const int elevel_ = (elevel); \
		PLy_elog_impl(elevel_, __VA_ARGS__); \
		if (elevel_ >= ERROR) \
			pg_unreachable(); \
	} while(0)
#endif							/* HAVE__BUILTIN_CONSTANT_P */

extern void PLy_elog_impl(int elevel, const char *fmt,...) pg_attribute_printf(2, 3);

extern void PLy_exception_set(PyObject *exc, const char *fmt,...) pg_attribute_printf(2, 3);

extern void PLy_exception_set_plural(PyObject *exc, const char *fmt_singular, const char *fmt_plural,
									 unsigned long n,...) pg_attribute_printf(2, 5) pg_attribute_printf(3, 5);

extern void PLy_exception_set_with_details(PyObject *excclass, ErrorData *edata);

#endif							/* PLPY_ELOG_H */
