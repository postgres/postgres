// dict helper for dlfcn tables
// TODO: use QSTR

#include "postgres.h"

#include "fmgr.h"

typedef struct dict_entry_s {
    const char *key;
    int value;
} dict_entry_s;

typedef struct dict_s {
    int len;
    int cap;
    dict_entry_s *entry;
} dict_s, *dict_t;

static int
dict_find_index(dict_t dict, const char *key) {
    for (int i = 0; i < dict->len; i++) {
        if (!strcmp(dict->entry[i].key, key)) {
            return i;
        }
    }
    return -1;
}

static int
dict_find(dict_t dict, const char *key, int def) {
    int idx = dict_find_index(dict, key);
    return idx == -1 ? def : dict->entry[idx].value;
}

static void
dict_add(dict_t dict, const char *key, int value) {
   int idx = dict_find_index(dict, key);
   if (idx != -1) {
       dict->entry[idx].value = value;
       return;
   }
   if (dict->len == dict->cap) {
       dict->cap *= 2;
       dict->entry = realloc(dict->entry, dict->cap * sizeof(dict_entry_s));
   }
   dict->entry[dict->len].key = strdup(key);
   dict->entry[dict->len].value = value;
   dict->len++;
}

static dict_t
dict_new(void) {
    dict_s proto = {0, 10, malloc(10 * sizeof(dict_entry_s))};
    dict_t d = malloc(sizeof(dict_s));
    *d = proto;
    return d;
}

/* dlclose stub
static void
dict_free(dict_t dict) {
    for (int i = 0; i < dict->len; i++) {
        free(dict->entry[i].key);
    }
    free(dict->entry);
    free(dict);
}
*/

static inline int
ends_with(const char *str, const char *suffix)
{
    if (!str || !suffix)
        return 0;
    size_t lenstr = strlen(str);
    size_t lensuffix = strlen(suffix);
    if (lensuffix >  lenstr)
        return 0;
    return strncmp(str + lenstr - lensuffix, suffix, lensuffix) == 0;
}

// dlfcn.h

volatile dict_t* dltab[];
volatile int dltab_index = 0;

void *
sym_stub(void) {
    puts("DLSYM STUB");
    return NULL;
}

char *
dlerror(void) {
    return (char *)dlerror;
}

static const Pg_magic_struct Pg_magic_data = PG_MODULE_MAGIC_DATA;

const Pg_magic_struct *
STUB_Pg_magic_func(void) {
	return &Pg_magic_data;
}

void STUB__PG_init(void) {
}

void
STUB__PG_fini(void) {
}

#define PG_FUNCTION_ARGS FunctionCallInfo fcinfo

extern Datum dsnowball_init(PG_FUNCTION_ARGS);
extern void pg_finfo_dsnowball_init(void);
extern void pg_finfo_dsnowball_lexize(void);

extern void pg_finfo_plpgsql_call_handler(void);
extern void pg_finfo_plpgsql_inline_handler(void);
extern void pg_finfo_plpgsql_validator(void);

// listed in .sql
extern void plpgsql_call_handler(void);
extern void plpgsql_inline_handler(void);
extern void plpgsql_validator(void);

extern void _PG_init(void);


void *
dlopen(const char *filename, int flags) {
    dict_t tab = NULL;
    fprintf(stderr,"void *dlopen(const char *filename = %s, int flags=%d)\n", filename, flags);
    for (int i=0; i< dltab_index; i++) {
        if ( dict_find_index(dltab[i], filename) > 0 )
            return (void *)i;
    }
    printf("dlopen: new lib '%s'\n", filename );
    if ( ends_with(filename,"/plpgsql.so") ){
        puts(" ========= CALLING _PG_init =========");
        _PG_init();
    }

    tab = dict_new();
    dict_add(tab, filename, dltab_index++ );
    dltab[dltab_index] = tab;

    return (void *)dltab_index;
}

void *
dlsym(void *__restrict handle, const char *__restrict symbol) {
    void *sym = NULL;
    if ( !strcmp(symbol, "Pg_magic_func") ) {
        sym = &STUB_Pg_magic_func;
        goto report;
    }

    if ( !strcmp(symbol, "_PG_init") )
        // sym = &STUB__PG_init;
        return &STUB__PG_init;

    if ( !strcmp(symbol, "dsnowball_init") )
        //sym = &dsnowball_init;
        return  &dsnowball_init;

    if ( !strcmp(symbol, "pg_finfo_dsnowball_init") )
        return  &pg_finfo_dsnowball_init;

    if ( !strcmp(symbol, "pg_finfo_dsnowball_lexize") )
        return  &pg_finfo_dsnowball_lexize;

    if ( !strcmp(symbol, "pg_finfo_plpgsql_call_handler") )
        return  &pg_finfo_plpgsql_call_handler;

    if ( !strcmp(symbol, "pg_finfo_plpgsql_inline_handler") )
        return  &pg_finfo_plpgsql_inline_handler;

    if ( !strcmp(symbol, "pg_finfo_plpgsql_validator") )
        return  &pg_finfo_plpgsql_validator;

    if ( !strcmp(symbol, "plpgsql_call_handler") )
        return  &plpgsql_call_handler;

    if ( !strcmp(symbol, "plpgsql_inline_handler") )
        return  &plpgsql_inline_handler;

    if ( !strcmp(symbol, "plpgsql_validator") ) {
        sym = &plpgsql_validator;
        goto report;
    }

report:;
    fprintf(stderr, "void *dlsym(void *handle = %p, const char *symbol = %s) => %p\n", handle, symbol, sym);
    return sym;
}


