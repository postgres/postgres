
#include <stdlib.h> /* for calloc, free */
#include "header.h"

extern struct SN_env * SN_create_env(int S_size, int I_size, int B_size)
{
    struct SN_env * z = (struct SN_env *) calloc(1, sizeof(struct SN_env));
    if (z == NULL) return NULL;
    z->p = create_s();
    if (z->p == NULL) goto error;
    if (S_size)
    {
        int i;
        z->S = (symbol * *) calloc(S_size, sizeof(symbol *));
        if (z->S == NULL) goto error;

        for (i = 0; i < S_size; i++)
        {
            z->S[i] = create_s();
            if (z->S[i] == NULL) goto error;
        }
        z->S_size = S_size;
    }

    if (I_size)
    {
        z->I = (int *) calloc(I_size, sizeof(int));
        if (z->I == NULL) goto error;
        z->I_size = I_size;
    }

    if (B_size)
    {
        z->B = (symbol *) calloc(B_size, sizeof(symbol));
        if (z->B == NULL) goto error;
        z->B_size = B_size;
    }

    return z;
error:
    SN_close_env(z);
    return NULL;
}

extern void SN_close_env(struct SN_env * z)
{
    if (z == NULL) return;
    if (z->S_size)
    {
        int i;
        for (i = 0; i < z->S_size; i++)
        {
            lose_s(z->S[i]);
        }
        free(z->S);
    }
    if (z->I_size) free(z->I);
    if (z->B_size) free(z->B);
    if (z->p) lose_s(z->p);
    free(z);
}

extern int SN_set_current(struct SN_env * z, int size, const symbol * s)
{
    int err = replace_s(z, 0, z->l, size, s, NULL);
    z->c = 0;
    return err;
}

