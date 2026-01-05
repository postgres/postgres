#include "snowball_runtime.h"

static const struct SN_env default_SN_env;

extern struct SN_env * SN_new_env(int alloc_size)
{
    struct SN_env * z = (struct SN_env *) malloc(alloc_size);
    if (z == NULL) return NULL;
    *z = default_SN_env;
    z->p = create_s();
    if (z->p == NULL) {
        SN_delete_env(z);
        return NULL;
    }
    return z;
}

extern void SN_delete_env(struct SN_env * z)
{
    if (z == NULL) return;
    if (z->p) lose_s(z->p);
    free(z);
}

extern int SN_set_current(struct SN_env * z, int size, const symbol * s)
{
    int err = replace_s(z, 0, z->l, size, s);
    z->c = 0;
    return err;
}
