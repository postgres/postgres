/*
 * src/pl/plpython/plpy_plpymodule.h
 */

#ifndef PLPY_PLPYMODULE_H
#define PLPY_PLPYMODULE_H

#include "utils/hsearch.h"

/* A hash table mapping sqlstates to exceptions, for speedy lookup */
extern HTAB *PLy_spi_exceptions;


#if PY_MAJOR_VERSION >= 3
PyMODINIT_FUNC PyInit_plpy(void);
#endif
extern void PLy_init_plpy(void);

#endif   /* PLPY_PLPYMODULE_H */
