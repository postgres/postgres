# src/pl/plpython/nls.mk
CATALOG_NAME	:= plpython
AVAIL_LANGUAGES	:= de es fr it ja pt_BR ro tr zh_CN zh_TW
GETTEXT_FILES	:= plpython.c
GETTEXT_TRIGGERS:= errmsg errmsg_plural:1,2 errdetail errdetail_log errdetail_plural:1,2 errhint errcontext PLy_elog:2 PLy_exception_set:2 PLy_exception_set_plural:2,3
