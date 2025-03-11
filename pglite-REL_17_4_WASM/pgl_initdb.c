#pragma once

#include <stdio.h> // FILE+fprintf
#ifndef PGL_INITDB_MAIN
#define PGL_INITDB_MAIN
#endif

/*
 * and now popen will return predefined slot from a file list
 * as file handle in initdb.c
 */



/*
 * popen is routed via pg_popen to stderr or a IDB_PIPE_* file
 * link a pclose replacement when we are in exec.c ( PG_EXEC defined )
 */

extern FILE * pgl_popen(const char *command, const char *type);
#define popen(command, mode) pgl_popen(command, mode)
// #define popen_check(command, mode) pgl_popen(command, mode)

extern int pgl_pclose(FILE *stream);
#define pclose(stream) pgl_pclose(stream)
#define pclose_check(stream) pgl_pclose(stream)


int
pg_chmod(const char * path, int mode_t) {
    return 0;
}

#ifdef FRONTEND
#undef FRONTEND
#endif

#define FRONTEND
#   include "../postgresql/src/common/logging.c"
#undef FRONTEND


#include "../postgresql/src/interfaces/libpq/pqexpbuffer.c"

#define sync_pgdata(...)
#define icu_language_tag(loc_str) icu_language_tag_idb(loc_str)
#define icu_validate_locale(loc_str) icu_validate_locale_idb(loc_str)


#include "../postgresql/src/bin/initdb/initdb.c"

void use_socketfile(void) {
    is_repl = true;
    is_embed = false;
}



