#include <c.h>

void ECPGdebug(int, FILE *);
bool ECPGconnect(const char * dbname);
bool ECPGdo(int, char *, ...);
bool ECPGcommit(int);
bool ECPGrollback(int);
bool ECPGfinish();
bool ECPGstatus();

void ECPGlog(const char * format, ...);

#ifdef LIBPQ_FE_H
bool ECPGsetdb(PGconn *);
#endif

/* Here are some methods used by the lib. */
/* Returns a pointer to a string containing a simple type name. */
const char * ECPGtype_name(enum ECPGttype);

/* A generic varchar type. */
struct ECPGgeneric_varchar {
    int len;
    char arr[1];
};


