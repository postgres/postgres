#pragma once

#define WAIT_USE_POLL 1

#define HAVE_LINUX_EIDRM_BUG
/*
 * Set the default wal_sync_method to fdatasync.  With recent Linux versions,
 * xlogdefs.h's normal rules will prefer open_datasync, which (a) doesn't
 * perform better and (b) causes outright failures on ext4 data=journal
 * filesystems, because those don't support O_DIRECT.
 */
#define PLATFORM_DEFAULT_WAL_SYNC_METHOD	WAL_SYNC_METHOD_FDATASYNC

// force the name used with --single
#if !defined(WASM_USERNAME)
#define WASM_USERNAME "postgres"
#endif



/* --------------- how to configure those when installed ? ---------------- */

// socket emulation via file, need to go in PGDATA for nodefs mount in web mode
#define PGS_ILOCK "/tmp/pglite/base/.s.PGSQL.5432.lock.in"
#define PGS_IN    "/tmp/pglite/base/.s.PGSQL.5432.in"
#define PGS_OLOCK "/tmp/pglite/base/.s.PGSQL.5432.lock.out"
#define PGS_OUT   "/tmp/pglite/base/.s.PGSQL.5432.out"



#if defined(PG_PREFIX)
#define em_xstr(s) em_str(s)
#define em_str(s) #s
#   define WASM_PREFIX em_xstr(PG_PREFIX)
#   define PG_MAIN_INCLUDE em_xstr(PATCH_MAIN)
#   define PG_PLUGIN_INCLUDE em_xstr(PATCH_PLUGIN
#   undef PG_PREFIX
#else
#   define WASM_PREFIX "/pgdata"
#   define PG_MAIN_INCLUDE "/pgdata/pg_main.c"
#   define PG_PLUGIN_INCLUDE "/pgdata/pg_plugin.h"
#endif

#include "pg_debug.h"

// #define COPY_INTERNAL
#define COPY_OFF
#define PGDLLIMPORT
#define PG_FORCE_DISABLE_INLINE


#define WASM_PGOPTS \
        "-c", "log_checkpoints=false",\
        "-c", "search_path=pg_catalog",\
        "-c", "exit_on_error=true",\
        "-c", "ignore_invalid_pages=on",\
        "-c", "temp_buffers=8MB",\
        "-c", "work_mem=4MB",\
        "-c", "fsync=on",\
        "-c", "synchronous_commit=on",\
        "-c", "wal_buffers=4MB",\
        "-c", "min_wal_size=80MB",\
        "-c", "shared_buffers=128MB"

// we want client and server in the same lib for now.
#if defined(PG_INITDB) && defined(PG_MAIN)
extern const char *progname;
#endif

// exported in ./src/fe_utils/string_utils.c
#include <stdbool.h>
extern PGDLLIMPORT bool fe_utils_quote_all_identifiers;

extern int	pg_char_to_encoding_private(const char *name);
extern const char *pg_encoding_to_char_private(int encoding);
extern int	pg_valid_server_encoding_id_private(int encoding);


#if defined(pg_char_to_encoding)
#undef pg_char_to_encoding
#endif
#define pg_char_to_encoding(encoding) pg_char_to_encoding_private(encoding)

#if defined(pg_encoding_to_char)
#undef pg_encoding_to_char
#endif
#define pg_encoding_to_char(encoding) pg_encoding_to_char_private(encoding)

#if defined(pg_valid_server_encoding_id)
#undef pg_valid_server_encoding_id
#endif
#define pg_valid_server_encoding_id(encoding) pg_valid_server_encoding_id_private(encoding)


/*
 * 'proc_exit' is a wasi system call, so change its name everywhere.
 */

#define proc_exit(arg) pg_proc_exit(arg)

/*
extern FILE* IDB_PIPE_FP;
extern int IDB_STAGE;
*/
extern FILE* SOCKET_FILE;
extern int SOCKET_DATA;
extern int pgl_pclose(FILE *stream);
/*
#if !defined(PGL_MAIN) && !defined(PGL_INITDB_MAIN)
#   if !defined(PG_EXEC)
extern int pgl_pclose(FILE *stream);
#define pclose(stream) pg_pclose(stream)
#   else
#if 1
#include <stdio.h>
int
pg_pclose(FILE *stream) {
    puts("# 118:" __FILE__ " int pg_pclose(FILE *stream) STUB");
    return 0;
}
#endif
#   endif  // PG_EXEC
#endif  // pgl
*/

/*
 * OpenPipeStream : another kind of pipe open in fd.c
 * known to try "locale -a" from collationcmds.c when in initdb.
 *
 */
#if defined(PG_FD)
#include <string.h>  // strlen
#include <unistd.h> // access+F_OK
#include <stdio.h> // FILE+fprintf

FILE *wasm_OpenPipeStream(const char *command, const char *mode);
FILE *
wasm_OpenPipeStream(const char *command, const char *mode) {

    FILE *result = NULL;
    const char *prefix = getenv("PGSYSCONFDIR");
    const char *locale = "/locale";
    char *localefile = malloc( strlen(prefix) + strlen(locale) + 1 );
    localefile = strcat(prefix,locale);
#if PGDEBUG
    fprintf(stderr, "# 232:%s: OpenPipeStream(command=%s, mode=%s)\n#\tredirected to %s\n", __FILE__, command, mode, localefile);
#endif
    if (localefile) {
        if (access(localefile, F_OK) != 0) {
            FILE *fakeloc = fopen(localefile, "w");
            {
                const char* encoding = getenv("PGCLIENTENCODING");
                fprintf(fakeloc, "C\nC.%s\nPOSIX\n%s\n", encoding, encoding);
            }
            if (fakeloc)
                fclose(fakeloc);
        }
        result = fopen(localefile, "r");
        free(localefile);
    }

    return result;
}

#else
#   define OpenPipeStream(cmd, mode) wasm_OpenPipeStream(cmd, mode)
#endif






/*
 *  handle pg_shmem.c special case
 */

#if defined(PG_SHMEM)
#include <stdio.h>  // print
#include <stdlib.h> // malloc
#include <unistd.h> // SC_
#include <sys/shm.h>
#include <sys/stat.h>

/*
 * Shared memory control operation.
 */

//extern int shmctl (int __shmid, int __cmd, struct shmid_ds *__buf);

int
shmctl (int __shmid, int __cmd, struct shmid_ds *__buf) {
	printf("FIXME: int shmctl (int __shmid=%d, int __cmd=%d, struct shmid_ds *__buf=%p)\n", __shmid, __cmd, __buf);
	return 0;
}


volatile void *FAKE_SHM ;
volatile key_t FAKE_KEY = 0;

/* Get shared memory segment.  */
// extern int shmget (key_t __key, size_t __size, int __shmflg);
int
shmget (key_t __key, size_t __size, int __shmflg) {
	printf("# FIXING: int shmget (key_t __key=%d, size_t __size=%zu, int __shmflg=%d) pagesize default=%d\n", __key, __size, __shmflg, getpagesize());
    if (!FAKE_KEY) {
    	FAKE_SHM =  malloc(__size);
        FAKE_KEY = 666;
        return (int)FAKE_KEY;
    } else {
    	printf("# ERROR: int shmget (key_t __key=%d, size_t __size=%zu, int __shmflg=%d)\n", __key, __size, __shmflg);
        //abort();
        return (int)FAKE_KEY;
    }
	return -1;
}

/* Attach shared memory segment.  */
// extern void *shmat (int __shmid, const void *__shmaddr, int __shmflg);
void *shmat (int __shmid, const void *__shmaddr, int __shmflg) {
	printf("# FIXING: void *shmat (int __shmid=%d, const void *__shmaddr=%p, int __shmflg=%d)\n", __shmid, __shmaddr, __shmflg);
	if (__shmid==666) {
		return (void *)FAKE_SHM;
    } else {
    	printf("# ERROR: void *shmat (int __shmid=%d, const void *__shmaddr=%p, int __shmflg=%d)\n", __shmid, __shmaddr, __shmflg);
        abort();
    }
	return NULL;
}

/* Detach shared memory segment.  */
// extern int shmdt (const void *__shmaddr);
int
shmdt (const void *__shmaddr) {
	puts("# FIXME: int shmdt (const void *__shmaddr)");
	return 0;
}


#endif // PG_SHMEM
