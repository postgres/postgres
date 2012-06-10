/*
 * src/pl/plpython/plpy_elog.h
 */

#ifndef PLPY_ELOG_H
#define PLPY_ELOG_H

/* global exception classes */
extern PyObject *PLy_exc_error;
extern PyObject *PLy_exc_fatal;
extern PyObject *PLy_exc_spi_error;

extern void
PLy_elog(int elevel, const char *fmt,...)
__attribute__((format(PG_PRINTF_ATTRIBUTE, 2, 3)));

extern void
PLy_exception_set(PyObject *exc, const char *fmt,...)
__attribute__((format(PG_PRINTF_ATTRIBUTE, 2, 3)));

extern void
PLy_exception_set_plural(PyObject *exc, const char *fmt_singular, const char *fmt_plural,
						 unsigned long n,...)
__attribute__((format(PG_PRINTF_ATTRIBUTE, 2, 5)))
__attribute__((format(PG_PRINTF_ATTRIBUTE, 3, 5)));

#endif   /* PLPY_ELOG_H */
