#include <string.h>
#include <stdio.h>
#include "postgres.h"	  /* for char16, etc. */
#include "utils/palloc.h" /* for palloc */
#include "libpq-fe.h" /* for TUPLE */

int
add_one(int arg)
{
    return(arg + 1);
}

char16 *
concat16(char16 *arg1, char16 *arg2)
{
    char16 *new_c16 = (char16 *) palloc(sizeof(char16));

    memset(new_c16, 0, sizeof(char16));
    (void) strncpy((char*)new_c16, (char*)arg1, 16);
    return (char16 *)(strncat((char*)new_c16, (char*)arg2, 16));
}

text *
copytext(text *t)
{
    /*
     * VARSIZE is the total size of the struct in bytes.
     */
    text *new_t = (text *) palloc(VARSIZE(t));
    
    memset(new_t, 0, VARSIZE(t));

    VARSIZE(new_t) = VARSIZE(t);
    /*
     * VARDATA is a pointer to the data region of the struct.
     */
    memcpy((void *) VARDATA(new_t), /* destination */
           (void *) VARDATA(t),     /* source */
           VARSIZE(t)-VARHDRSZ);              /* how many bytes */

    return(new_t);
}

bool
c_overpaid(TUPLE t,	/* the current instance of EMP */
	   int4 limit) 
{
    bool isnull = false;
    int4 salary;

    salary = (int4) GetAttributeByName(t, "salary", &isnull);

    if (isnull)
	return (false);
    return(salary > limit);
}
