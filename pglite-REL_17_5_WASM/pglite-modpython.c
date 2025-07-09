#include <stdio.h>
#include <errno.h>
#include <unistd.h>

#ifdef __wii__
#   include <gccore.h>
#   include <wiiuse/wpad.h>
#   include <fat.h>
#endif

#include "w2c2/w2c2_base.h"
#include "wasi/wasi.h"
#include "${WASM2C}.h"

void
trap(Trap trap) {
    fprintf(stderr, "TRAP: %s\n", trapDescription(trap));
#ifdef __wii__
    VIDEO_WaitVSync();
#endif
    abort();
}

U32
wasi_snapshot_preview1__fd_renumber(void* ctx, U32 from, U32 to) {
    return -1;
}

wasmMemory*
wasiMemory(void* instance) {
    return ${WASM2C}_memory((${WASM2C}Instance*)instance);
}

extern char **environ;
volatile int skip_main = 0;
//char** __environ = NULL;
${WASM2C}Instance instance;

#include "${WASM2C}.c"

#define WASM_PREFIX "/tmp/pglite"
#define WASM_USERNAME "postgres"
#define PGDB WASM_PREFIX "/base"


char **g_argv;
int g_argc;

char **copy_argv(int argc, char *argv[]) {
    // calculate the contiguous argv buffer size
    int length=0;
    size_t ptr_args = argc + 1;
    for (int i = 0; i < argc; i++) {
        length += (strlen(argv[i]) + 1);
    }
    char** new_argv = (char**)malloc((ptr_args) * sizeof(char*) + length);

    // copy argv into the contiguous buffer
    length = 0;
    for (int i = 0; i < argc; i++) {
        new_argv[i] = &(((char*)new_argv)[(ptr_args * sizeof(char*)) + length]);
        strcpy(new_argv[i], argv[i]);
        length += (strlen(argv[i]) + 1);
    }

    // insert NULL terminating ptr at the end of the ptr array
    new_argv[ptr_args-1] = NULL;
    return (new_argv);
}

int pre_main(int tmp_argc, char* tmp_argv[]) {
    puts("# 84: pre_main");
    // __environ =  environ;

        setenv("EMBED", "wasi", 1);
        setenv("REPL", "N", 1);

	    setenv("PGSYSCONFDIR", WASM_PREFIX, 1);
    	setenv("PGCLIENTENCODING", "UTF8", 1);

        setenv("TZ", "UTC", 1);
        setenv("PGTZ", "UTC", 1);
	    setenv("PGUSER", WASM_USERNAME , 1);
	    setenv("PGDATA", PGDB , 1);
	    setenv("PGDATABASE", "template1" , 1);
        setenv("PG_COLOR", "always", 0);
    puts("# 87: global argc/argv");
        g_argv = copy_argv(tmp_argc, tmp_argv);
        g_argc = sizeof(g_argv) / sizeof(char*) - 1;


#   if defined(__MWERKS__) && defined(macintosh)
        MaxApplZone();
        MoreMasters();
        MoreMasters();
        argc = ccommand(&argv);
#   elif defined(__wii__)
        /* TODO: get interactive console working */
        wiiInitVideo();
        fatInitDefault();
#   endif

    /* Initialize WASI */
    if (!wasiInit(g_argc, g_argv, environ)) {
        fprintf(stderr, "failed to init WASI\n");
        return 1;
    }

    if (!wasiFileDescriptorAdd(-1, "/", NULL)) {
        fprintf(stderr, "failed to add preopen\n");
        return 1;
    }

#   ifdef __MSL__
        SIOUXSetTitle("\p${WASM2C}");
#   endif

    ${WASM2C}Instantiate(&instance, NULL);

    return 0;
}

// #define REACTOR

#if defined(REACTOR)
    #define STARTPROC(i) ${WASM2C}_setup(i)
    // #define STARTPROC(i) ${WASM2C}_pg_initdb(i)
#else
    #define STARTPROC(i) ${WASM2C}__start(&instance);
#endif


int main(int argc, char* argv[]);

void do_main() {
    puts("# 128: do_main Begin");
    STARTPROC(&instance);
    puts("# 134: do_main End");
}

#if defined(__PYDK__)
#   include "Python.h"

    static PyObject * ${WASM2C}_info(PyObject *self, PyObject *args, PyObject *kwds)
    {
        puts("${WASM2C} test function : return 42");
        return Py_BuildValue("i", 42);
    }

    static PyObject * Begin(PyObject *self, PyObject *args, PyObject *kwds) {
        puts("PyInit_${WASM2C}");

        char *tmp_argv[] = { "/tmp/pglite/bin/postgres", "--single", "template1", NULL};
        int tmp_argc = sizeof(tmp_argv) / sizeof(char*) - 1;
puts("167");
        pre_main(tmp_argc, tmp_argv);
puts("169");

//${WASM2C}__start(&instance);
        Py_RETURN_NONE;
    }

    static PyObject * End(PyObject *self, PyObject *args, PyObject *kwds) {
puts("164: do main");
        do_main();
puts("FREE");
        ${WASM2C}FreeInstance(&instance);
        Py_RETURN_NONE;
    }

#   include "${WASM2C}.pymod"

    static PyMethodDef mod_${WASM2C}_methods[] = {
#include "${WASM2C}.def"
        {NULL, NULL, 0, NULL}
    };

    static struct PyModuleDef mod_${WASM2C} = {
        PyModuleDef_HEAD_INIT,
        "${WASM2C}",
        NULL,
        -1,
        mod_${WASM2C}_methods,
        NULL, // m_slots
        NULL, // m_traverse
        NULL, // m_clear
        NULL, // m_free
    };


    PyMODINIT_FUNC
    PyInit_${WASM2C}(void) {
        PyObject *${WASM2C}_mod = PyModule_Create(&mod_${WASM2C});
#       ifdef Py_GIL_DISABLED
            PyUnstable_Module_SetGIL(${WASM2C}_mod, Py_MOD_GIL_NOT_USED);
#       endif
        return ${WASM2C}_mod;
    }
#endif


int main(int argc, char* argv[]) {
    if (!skip_main) {
puts("216");
        char *tmp_argv[] = { "/tmp/pglite/bin/postgres", "--single", "template1", NULL};
        int tmp_argc = sizeof(tmp_argv) / sizeof(char*) - 1;
        skip_main = pre_main(tmp_argc, tmp_argv);
puts("220");
        if (!skip_main) {
            do_main();
puts("FREE");
            ${WASM2C}FreeInstance(&instance);
        }
    } else {
        puts(" -- main skipped --");
     }
    return skip_main;
}
