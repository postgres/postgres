
#include <limits.h>

#include "api.h"

#define MAXINT INT_MAX
#define MININT INT_MIN

#define HEAD 2*sizeof(int)

#define SIZE(p)        ((int *)(p))[-1]
#define SET_SIZE(p, n) ((int *)(p))[-1] = n
#define CAPACITY(p)    ((int *)(p))[-2]

struct among
{   int s_size;     /* number of chars in string */
    symbol * s;       /* search string */
    int substring_i;/* index to longest matching substring */
    int result;     /* result of the lookup */
    int (* function)(struct SN_env *);
};

extern symbol * create_s(void);
extern void lose_s(symbol * p);

extern int in_grouping(struct SN_env * z, unsigned char * s, int min, int max);
extern int in_grouping_b(struct SN_env * z, unsigned char * s, int min, int max);
extern int out_grouping(struct SN_env * z, unsigned char * s, int min, int max);
extern int out_grouping_b(struct SN_env * z, unsigned char * s, int min, int max);

extern int in_range(struct SN_env * z, int min, int max);
extern int in_range_b(struct SN_env * z, int min, int max);
extern int out_range(struct SN_env * z, int min, int max);
extern int out_range_b(struct SN_env * z, int min, int max);

extern int eq_s(struct SN_env * z, int s_size, symbol * s);
extern int eq_s_b(struct SN_env * z, int s_size, symbol * s);
extern int eq_v(struct SN_env * z, symbol * p);
extern int eq_v_b(struct SN_env * z, symbol * p);

extern int find_among(struct SN_env * z, struct among * v, int v_size);
extern int find_among_b(struct SN_env * z, struct among * v, int v_size);

extern symbol * increase_size(symbol * p, int n);
extern int replace_s(struct SN_env * z, int c_bra, int c_ket, int s_size, const symbol * s);
extern void slice_from_s(struct SN_env * z, int s_size, symbol * s);
extern void slice_from_v(struct SN_env * z, symbol * p);
extern void slice_del(struct SN_env * z);

extern void insert_s(struct SN_env * z, int bra, int ket, int s_size, symbol * s);
extern void insert_v(struct SN_env * z, int bra, int ket, symbol * p);

extern symbol * slice_to(struct SN_env * z, symbol * p);
extern symbol * assign_to(struct SN_env * z, symbol * p);

extern void debug(struct SN_env * z, int number, int line_count);

