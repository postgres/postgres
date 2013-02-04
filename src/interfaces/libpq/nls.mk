# src/interfaces/libpq/nls.mk
CATALOG_NAME     = libpq
AVAIL_LANGUAGES  = cs de es fr it ja pl pt_BR ru tr zh_CN zh_TW
GETTEXT_FILES    = fe-auth.c fe-connect.c fe-exec.c fe-lobj.c fe-misc.c fe-protocol2.c fe-protocol3.c fe-secure.c win32.c
GETTEXT_TRIGGERS = libpq_gettext pqInternalNotice:2
GETTEXT_FLAGS    = libpq_gettext:1:pass-c-format pqInternalNotice:2:c-format
