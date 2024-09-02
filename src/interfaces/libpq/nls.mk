# src/interfaces/libpq/nls.mk
CATALOG_NAME     = libpq
GETTEXT_FILES    = fe-auth.c \
                   fe-auth-scram.c \
                   fe-cancel.c \
                   fe-connect.c \
                   fe-exec.c \
                   fe-gssapi-common.c \
                   fe-lobj.c \
                   fe-misc.c \
                   fe-protocol3.c \
                   fe-secure.c \
                   fe-secure-common.c \
                   fe-secure-gssapi.c \
                   fe-secure-openssl.c \
                   win32.c
GETTEXT_TRIGGERS = libpq_append_conn_error:2 \
                   libpq_append_error:2 \
                   libpq_gettext \
                   libpq_ngettext:1,2 \
                   pqInternalNotice:2
GETTEXT_FLAGS    = libpq_append_conn_error:2:c-format \
                   libpq_append_error:2:c-format \
                   libpq_gettext:1:pass-c-format \
                   libpq_ngettext:1:pass-c-format \
                   libpq_ngettext:2:pass-c-format \
                   pqInternalNotice:2:c-format
