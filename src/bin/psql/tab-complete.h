#ifndef TAB_COMPLETE_H
#define TAB_COMPLETE_H

#include <libpq-fe.h>

void initialize_readline(PGconn ** conn);

#endif
