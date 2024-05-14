# src/pl/plpython/nls.mk
CATALOG_NAME     = plpython
GETTEXT_FILES    = plpy_cursorobject.c \
                   plpy_elog.c \
                   plpy_exec.c \
                   plpy_main.c \
                   plpy_planobject.c \
                   plpy_plpymodule.c \
                   plpy_procedure.c \
                   plpy_resultobject.c \
                   plpy_spi.c \
                   plpy_subxactobject.c \
                   plpy_typeio.c \
                   plpy_util.c
GETTEXT_TRIGGERS = $(BACKEND_COMMON_GETTEXT_TRIGGERS) \
                   PLy_elog:2 \
                   PLy_exception_set:2 \
                   PLy_exception_set_plural:2,3
GETTEXT_FLAGS    = $(BACKEND_COMMON_GETTEXT_FLAGS) \
                   PLy_elog:2:c-format \
                   PLy_exception_set:2:c-format \
                   PLy_exception_set_plural:2:c-format \
                   PLy_exception_set_plural:3:c-format
