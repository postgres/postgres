#ifndef SNOWBALL_INCLUDED_SNOWBALL_RUNTIME_H
#define SNOWBALL_INCLUDED_SNOWBALL_RUNTIME_H

#include "api.h"

#define HEAD 2*sizeof(int)

#ifdef __cplusplus
/* Use reinterpret_cast<> to avoid -Wcast-align warnings from clang++. */
# define SIZE(p)        (reinterpret_cast<const int *>(p))[-1]
# define SET_SIZE(p, n) (reinterpret_cast<int *>(p))[-1] = n
# define CAPACITY(p)    (reinterpret_cast<int *>(p))[-2]
#else
# define SIZE(p)        ((const int *)(p))[-1]
# define SET_SIZE(p, n) ((int *)(p))[-1] = n
# define CAPACITY(p)    ((int *)(p))[-2]
#endif

#ifdef SNOWBALL_RUNTIME_THROW_EXCEPTIONS
# define SNOWBALL_ERR void
#else
# define SNOWBALL_ERR int
#endif

#ifdef SNOWBALL_DEBUG_COMMAND_USED
# include <stdio.h>
static void debug(struct SN_env * z, int number, int line_count) {
    int i;
    int limit = SIZE(z->p);
    if (number >= 0) printf("%3d (line %4d): [%d]'", number, line_count, limit);
    for (i = 0; i <= limit; i++) {
        if (z->lb == i) printf("{");
        if (z->bra == i) printf("[");
        if (z->c == i) printf("|");
        if (z->ket == i) printf("]");
        if (z->l == i) printf("}");
        if (i < limit) {
            int ch = z->p[i];
            if (ch == 0) ch = '#';
            printf("%c", ch);
        }
    }
    printf("'\n");
}
#endif

struct among
{
    /* Number of symbols in s. */
    int s_size;
    /* Search string. */
    const symbol * s;
    /* Delta of index to longest matching substring, or 0 if none. */
    int substring_i;
    /* Result of the lookup. */
    int result;
    /* Optional condition routine index, or 0 if none. */
    int function;
};

#ifdef __cplusplus
extern "C" {
#endif

extern symbol * create_s(void);
extern void lose_s(symbol * p);

extern int skip_utf8(const symbol * p, int c, int limit, int n);

extern int skip_b_utf8(const symbol * p, int c, int limit, int n);

extern int in_grouping_U(struct SN_env * z, const unsigned char * s, int min, int max, int repeat);
extern int in_grouping_b_U(struct SN_env * z, const unsigned char * s, int min, int max, int repeat);
extern int out_grouping_U(struct SN_env * z, const unsigned char * s, int min, int max, int repeat);
extern int out_grouping_b_U(struct SN_env * z, const unsigned char * s, int min, int max, int repeat);

extern int in_grouping(struct SN_env * z, const unsigned char * s, int min, int max, int repeat);
extern int in_grouping_b(struct SN_env * z, const unsigned char * s, int min, int max, int repeat);
extern int out_grouping(struct SN_env * z, const unsigned char * s, int min, int max, int repeat);
extern int out_grouping_b(struct SN_env * z, const unsigned char * s, int min, int max, int repeat);

extern int eq_s(struct SN_env * z, int s_size, const symbol * s);
extern int eq_s_b(struct SN_env * z, int s_size, const symbol * s);
extern int eq_v(struct SN_env * z, const symbol * p);
extern int eq_v_b(struct SN_env * z, const symbol * p);

extern int find_among(struct SN_env * z, const struct among * v, int v_size,
                      int (*)(struct SN_env *));
extern int find_among_b(struct SN_env * z, const struct among * v, int v_size,
                        int (*)(struct SN_env *));

extern SNOWBALL_ERR replace_s(struct SN_env * z, int c_bra, int c_ket, int s_size, const symbol * s);
extern SNOWBALL_ERR slice_from_s(struct SN_env * z, int s_size, const symbol * s);
extern SNOWBALL_ERR slice_from_v(struct SN_env * z, const symbol * p);
extern SNOWBALL_ERR slice_del(struct SN_env * z);

extern SNOWBALL_ERR insert_s(struct SN_env * z, int bra, int ket, int s_size, const symbol * s);
extern SNOWBALL_ERR insert_v(struct SN_env * z, int bra, int ket, const symbol * p);

extern SNOWBALL_ERR slice_to(struct SN_env * z, symbol ** p);
extern SNOWBALL_ERR assign_to(struct SN_env * z, symbol ** p);

extern int len_utf8(const symbol * p);

#ifdef __cplusplus
}
#endif

#endif
