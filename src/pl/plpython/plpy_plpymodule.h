/*
 * src/pl/plpython/plpy_plpymodule.h
 */

#ifndef PLPY_PLPYMODULE_H
#define PLPY_PLPYMODULE_H

#include "plpython.h"
#include "utils/hsearch.h"

/* A hash table mapping sqlstates to exceptions, for speedy lookup */
extern HTAB *PLy_spi_exceptions;


PyMODINIT_FUNC PyInit_plpy(void);
extern void PLy_init_plpy(void);

#endif							/* PLPY_PLPYMODULE_H */
