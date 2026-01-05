#ifndef SNOWBALL_API_H_INCLUDED
#define SNOWBALL_API_H_INCLUDED

typedef unsigned char symbol;

/* Or replace 'char' above with 'short' for 16 bit characters.

   More precisely, replace 'char' with whatever type guarantees the
   character width you need. Note however that sizeof(symbol) should divide
   HEAD, defined in snowball_runtime.h as 2*sizeof(int), without remainder,
   otherwise there is an alignment problem. In the unlikely event of a problem
   here, consult Martin Porter.
*/

struct SN_env {
    symbol * p;
    int c; int l; int lb; int bra; int ket;
    int af;
};

#ifdef __cplusplus
extern "C" {
#endif

extern struct SN_env * SN_new_env(int alloc_size);
extern void SN_delete_env(struct SN_env * z);

extern int SN_set_current(struct SN_env * z, int size, const symbol * s);

#ifdef __cplusplus
}
#endif

#endif
