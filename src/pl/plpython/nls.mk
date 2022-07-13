# src/pl/plpython/nls.mk
CATALOG_NAME     = plpython
GETTEXT_FILES    = $(wildcard *.c)
GETTEXT_TRIGGERS = $(BACKEND_COMMON_GETTEXT_TRIGGERS) PLy_elog:2 PLy_exception_set:2 PLy_exception_set_plural:2,3
GETTEXT_FLAGS    = $(BACKEND_COMMON_GETTEXT_FLAGS) \
    PLy_elog:2:c-format \
    PLy_exception_set:2:c-format \
    PLy_exception_set_plural:2:c-format \
    PLy_exception_set_plural:3:c-format
