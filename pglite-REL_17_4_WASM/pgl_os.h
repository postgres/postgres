#pragma once

// to override chmod()
#include <sys/stat.h>

extern int pg_chmod(const char * path, int mode_t);
// initdb chmod is not supported by wasi, so just don't use it anywhere
// #if defined(__wasi__)
#define chmod(path, mode) pg_chmod(path, mode)
//#endif


#include <stdio.h> // FILE

FILE* IDB_PIPE_FP = NULL;
int IDB_STAGE = 0;


/*
 * and now popen will return predefined slot from a file list
 * as file handle in initdb.c
 */

FILE *pgl_popen(const char *command, const char *type) {
    if (IDB_STAGE>1) {
    	fprintf(stderr,"# popen[%s]\n", command);
    	return stderr;
    }

    if (!IDB_STAGE) {
        fprintf(stderr,"# popen[%s] (BOOT)\n", command);
        IDB_PIPE_FP = fopen( IDB_PIPE_BOOT, "w");
        IDB_STAGE = 1;
    } else {
        fprintf(stderr,"# popen[%s] (SINGLE)\n", command);
        IDB_PIPE_FP = fopen( IDB_PIPE_SINGLE, "w");
        IDB_STAGE = 2;
    }

    return IDB_PIPE_FP;
}

#define popen(command, mode) pgl_popen(command, mode)

int
pgl_pclose(FILE *stream) {
    if (IDB_STAGE==1)
        fprintf(stderr,"# pg_pclose(%s) 133:" __FILE__ "\n" , IDB_PIPE_BOOT);
    if (IDB_STAGE==2)
        fprintf(stderr,"# pg_pclose(%s) 135:" __FILE__ "\n" , IDB_PIPE_SINGLE);

    if (IDB_PIPE_FP) {
        fflush(IDB_PIPE_FP);
        fclose(IDB_PIPE_FP);
        IDB_PIPE_FP = NULL;
    }
    return 0;
}



