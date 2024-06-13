# src/bin/pg_ctl/nls.mk
CATALOG_NAME     = pg_ctl
GETTEXT_FILES    = pg_ctl.c \
                   ../../common/controldata_utils.c \
                   ../../common/exec.c \
                   ../../common/fe_memutils.c \
                   ../../common/wait_error.c \
                   ../../port/path.c
