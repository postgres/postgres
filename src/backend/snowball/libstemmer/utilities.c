#include "snowball_runtime.h"

#ifdef SNOWBALL_RUNTIME_THROW_EXCEPTIONS
# include <new>
# include <stdexcept>
# define SNOWBALL_RETURN_OK return
# define SNOWBALL_RETURN_OR_THROW(R, E) throw E
# define SNOWBALL_PROPAGATE_ERR(F) F
#else
# define SNOWBALL_RETURN_OK return 0
# define SNOWBALL_RETURN_OR_THROW(R, E) return R
# define SNOWBALL_PROPAGATE_ERR(F) do { \
        int snowball_err = F; \
        if (snowball_err < 0) return snowball_err; \
    } while (0)
#endif

#define CREATE_SIZE 1

extern symbol * create_s(void) {
    symbol * p;
    void * mem = malloc(HEAD + (CREATE_SIZE + 1) * sizeof(symbol));
    if (mem == NULL)
        SNOWBALL_RETURN_OR_THROW(NULL, std::bad_alloc());
    p = (symbol *) (HEAD + (char *) mem);
    CAPACITY(p) = CREATE_SIZE;
    SET_SIZE(p, 0);
    return p;
}

extern void lose_s(symbol * p) {
    if (p == NULL) return;
    free((char *) p - HEAD);
}

/*
   new_p = skip_utf8(p, c, l, n); skips n characters forwards from p + c.
   new_p is the new position, or -1 on failure.

   -- used to implement hop and next in the utf8 case.
*/

extern int skip_utf8(const symbol * p, int c, int limit, int n) {
    int b;
    if (n < 0) return -1;
    for (; n > 0; n--) {
        if (c >= limit) return -1;
        b = p[c++];
        if (b >= 0xC0) {   /* 1100 0000 */
            while (c < limit) {
                b = p[c];
                if (b >= 0xC0 || b < 0x80) break;
                /* break unless b is 10------ */
                c++;
            }
        }
    }
    return c;
}

/*
   new_p = skip_b_utf8(p, c, lb, n); skips n characters backwards from p + c - 1
   new_p is the new position, or -1 on failure.

   -- used to implement hop and next in the utf8 case.
*/

extern int skip_b_utf8(const symbol * p, int c, int limit, int n) {
    int b;
    if (n < 0) return -1;
    for (; n > 0; n--) {
        if (c <= limit) return -1;
        b = p[--c];
        if (b >= 0x80) {   /* 1000 0000 */
            while (c > limit) {
                b = p[c];
                if (b >= 0xC0) break; /* 1100 0000 */
                c--;
            }
        }
    }
    return c;
}

/* Code for character groupings: utf8 cases */

static int get_utf8(const symbol * p, int c, int l, int * slot) {
    int b0, b1, b2;
    if (c >= l) return 0;
    b0 = p[c++];
    if (b0 < 0xC0 || c == l) {   /* 1100 0000 */
        *slot = b0;
        return 1;
    }
    b1 = p[c++] & 0x3F;
    if (b0 < 0xE0 || c == l) {   /* 1110 0000 */
        *slot = (b0 & 0x1F) << 6 | b1;
        return 2;
    }
    b2 = p[c++] & 0x3F;
    if (b0 < 0xF0 || c == l) {   /* 1111 0000 */
        *slot = (b0 & 0xF) << 12 | b1 << 6 | b2;
        return 3;
    }
    *slot = (b0 & 0x7) << 18 | b1 << 12 | b2 << 6 | (p[c] & 0x3F);
    return 4;
}

static int get_b_utf8(const symbol * p, int c, int lb, int * slot) {
    int a, b;
    if (c <= lb) return 0;
    b = p[--c];
    if (b < 0x80 || c == lb) {   /* 1000 0000 */
        *slot = b;
        return 1;
    }
    a = b & 0x3F;
    b = p[--c];
    if (b >= 0xC0 || c == lb) {   /* 1100 0000 */
        *slot = (b & 0x1F) << 6 | a;
        return 2;
    }
    a |= (b & 0x3F) << 6;
    b = p[--c];
    if (b >= 0xE0 || c == lb) {   /* 1110 0000 */
        *slot = (b & 0xF) << 12 | a;
        return 3;
    }
    *slot = (p[--c] & 0x7) << 18 | (b & 0x3F) << 12 | a;
    return 4;
}

extern int in_grouping_U(struct SN_env * z, const unsigned char * s, int min, int max, int repeat) {
    do {
        int ch;
        int w = get_utf8(z->p, z->c, z->l, & ch);
        if (!w) return -1;
        if (ch > max || (ch -= min) < 0 || (s[ch >> 3] & (0X1 << (ch & 0X7))) == 0)
            return w;
        z->c += w;
    } while (repeat);
    return 0;
}

extern int in_grouping_b_U(struct SN_env * z, const unsigned char * s, int min, int max, int repeat) {
    do {
        int ch;
        int w = get_b_utf8(z->p, z->c, z->lb, & ch);
        if (!w) return -1;
        if (ch > max || (ch -= min) < 0 || (s[ch >> 3] & (0X1 << (ch & 0X7))) == 0)
            return w;
        z->c -= w;
    } while (repeat);
    return 0;
}

extern int out_grouping_U(struct SN_env * z, const unsigned char * s, int min, int max, int repeat) {
    do {
        int ch;
        int w = get_utf8(z->p, z->c, z->l, & ch);
        if (!w) return -1;
        if (!(ch > max || (ch -= min) < 0 || (s[ch >> 3] & (0X1 << (ch & 0X7))) == 0))
            return w;
        z->c += w;
    } while (repeat);
    return 0;
}

extern int out_grouping_b_U(struct SN_env * z, const unsigned char * s, int min, int max, int repeat) {
    do {
        int ch;
        int w = get_b_utf8(z->p, z->c, z->lb, & ch);
        if (!w) return -1;
        if (!(ch > max || (ch -= min) < 0 || (s[ch >> 3] & (0X1 << (ch & 0X7))) == 0))
            return w;
        z->c -= w;
    } while (repeat);
    return 0;
}

/* Code for character groupings: non-utf8 cases */

extern int in_grouping(struct SN_env * z, const unsigned char * s, int min, int max, int repeat) {
    do {
        int ch;
        if (z->c >= z->l) return -1;
        ch = z->p[z->c];
        if (ch > max || (ch -= min) < 0 || (s[ch >> 3] & (0X1 << (ch & 0X7))) == 0)
            return 1;
        z->c++;
    } while (repeat);
    return 0;
}

extern int in_grouping_b(struct SN_env * z, const unsigned char * s, int min, int max, int repeat) {
    do {
        int ch;
        if (z->c <= z->lb) return -1;
        ch = z->p[z->c - 1];
        if (ch > max || (ch -= min) < 0 || (s[ch >> 3] & (0X1 << (ch & 0X7))) == 0)
            return 1;
        z->c--;
    } while (repeat);
    return 0;
}

extern int out_grouping(struct SN_env * z, const unsigned char * s, int min, int max, int repeat) {
    do {
        int ch;
        if (z->c >= z->l) return -1;
        ch = z->p[z->c];
        if (!(ch > max || (ch -= min) < 0 || (s[ch >> 3] & (0X1 << (ch & 0X7))) == 0))
            return 1;
        z->c++;
    } while (repeat);
    return 0;
}

extern int out_grouping_b(struct SN_env * z, const unsigned char * s, int min, int max, int repeat) {
    do {
        int ch;
        if (z->c <= z->lb) return -1;
        ch = z->p[z->c - 1];
        if (!(ch > max || (ch -= min) < 0 || (s[ch >> 3] & (0X1 << (ch & 0X7))) == 0))
            return 1;
        z->c--;
    } while (repeat);
    return 0;
}

extern int eq_s(struct SN_env * z, int s_size, const symbol * s) {
    if (z->l - z->c < s_size || memcmp(z->p + z->c, s, s_size * sizeof(symbol)) != 0) return 0;
    z->c += s_size; return 1;
}

extern int eq_s_b(struct SN_env * z, int s_size, const symbol * s) {
    if (z->c - z->lb < s_size || memcmp(z->p + z->c - s_size, s, s_size * sizeof(symbol)) != 0) return 0;
    z->c -= s_size; return 1;
}

extern int eq_v(struct SN_env * z, const symbol * p) {
    return eq_s(z, SIZE(p), p);
}

extern int eq_v_b(struct SN_env * z, const symbol * p) {
    return eq_s_b(z, SIZE(p), p);
}

extern int find_among(struct SN_env * z, const struct among * v, int v_size,
                      int (*call_among_func)(struct SN_env*)) {

    int i = 0;
    int j = v_size;

    int c = z->c; int l = z->l;
    const symbol * q = z->p + c;

    const struct among * w;

    int common_i = 0;
    int common_j = 0;

    int first_key_inspected = 0;

    while (1) {
        int k = i + ((j - i) >> 1);
        int diff = 0;
        int common = common_i < common_j ? common_i : common_j; /* smaller */
        w = v + k;
        {
            int i2; for (i2 = common; i2 < w->s_size; i2++) {
                if (c + common == l) { diff = -1; break; }
                diff = q[common] - w->s[i2];
                if (diff != 0) break;
                common++;
            }
        }
        if (diff < 0) {
            j = k;
            common_j = common;
        } else {
            i = k;
            common_i = common;
        }
        if (j - i <= 1) {
            if (i > 0) break; /* v->s has been inspected */
            if (j == i) break; /* only one item in v */

            /* - but now we need to go round once more to get
               v->s inspected. This looks messy, but is actually
               the optimal approach.  */

            if (first_key_inspected) break;
            first_key_inspected = 1;
        }
    }
    w = v + i;
    while (1) {
        if (common_i >= w->s_size) {
            z->c = c + w->s_size;
            if (!w->function) return w->result;
            z->af = w->function;
            if (call_among_func(z)) {
                z->c = c + w->s_size;
                return w->result;
            }
        }
        if (!w->substring_i) return 0;
        w += w->substring_i;
    }
}

/* find_among_b is for backwards processing. Same comments apply */

extern int find_among_b(struct SN_env * z, const struct among * v, int v_size,
                        int (*call_among_func)(struct SN_env*)) {

    int i = 0;
    int j = v_size;

    int c = z->c; int lb = z->lb;
    const symbol * q = z->p + c - 1;

    const struct among * w;

    int common_i = 0;
    int common_j = 0;

    int first_key_inspected = 0;

    while (1) {
        int k = i + ((j - i) >> 1);
        int diff = 0;
        int common = common_i < common_j ? common_i : common_j;
        w = v + k;
        {
            int i2; for (i2 = w->s_size - 1 - common; i2 >= 0; i2--) {
                if (c - common == lb) { diff = -1; break; }
                diff = q[- common] - w->s[i2];
                if (diff != 0) break;
                common++;
            }
        }
        if (diff < 0) { j = k; common_j = common; }
                 else { i = k; common_i = common; }
        if (j - i <= 1) {
            if (i > 0) break;
            if (j == i) break;
            if (first_key_inspected) break;
            first_key_inspected = 1;
        }
    }
    w = v + i;
    while (1) {
        if (common_i >= w->s_size) {
            z->c = c - w->s_size;
            if (!w->function) return w->result;
            z->af = w->function;
            if (call_among_func(z)) {
                z->c = c - w->s_size;
                return w->result;
            }
        }
        if (!w->substring_i) return 0;
        w += w->substring_i;
    }
}


/* Increase the size of the buffer pointed to by p to at least n symbols.
 * On success, returns 0.  If insufficient memory, returns -1.
 */
static int increase_size(symbol ** p, int n) {
    int new_size = n + 20;
    void * mem = realloc((char *) *p - HEAD,
                         HEAD + (new_size + 1) * sizeof(symbol));
    symbol * q;
    if (mem == NULL) return -1;
    q = (symbol *) (HEAD + (char *)mem);
    CAPACITY(q) = new_size;
    *p = q;
    return 0;
}

/* to replace symbols between c_bra and c_ket in z->p by the
   s_size symbols at s.
   Returns 0 on success, -1 on error.
*/
extern SNOWBALL_ERR replace_s(struct SN_env * z, int c_bra, int c_ket, int s_size, const symbol * s)
{
    int adjustment = s_size - (c_ket - c_bra);
    if (adjustment != 0) {
        int len = SIZE(z->p);
        if (adjustment + len > CAPACITY(z->p)) {
            SNOWBALL_PROPAGATE_ERR(increase_size(&z->p, adjustment + len));
        }
        memmove(z->p + c_ket + adjustment,
                z->p + c_ket,
                (len - c_ket) * sizeof(symbol));
        SET_SIZE(z->p, adjustment + len);
        z->l += adjustment;
        if (z->c >= c_ket)
            z->c += adjustment;
        else if (z->c > c_bra)
            z->c = c_bra;
    }
    if (s_size) memmove(z->p + c_bra, s, s_size * sizeof(symbol));
    SNOWBALL_RETURN_OK;
}

# define REPLACE_S(Z, B, K, SIZE, S) \
    SNOWBALL_PROPAGATE_ERR(replace_s(Z, B, K, SIZE, S))

static SNOWBALL_ERR slice_check(struct SN_env * z) {

    if (z->bra < 0 ||
        z->bra > z->ket ||
        z->ket > z->l ||
        z->l > SIZE(z->p)) /* this line could be removed */
    {
#if 0
        fprintf(stderr, "faulty slice operation:\n");
        debug(z, -1, 0);
#endif
        SNOWBALL_RETURN_OR_THROW(-1, std::logic_error("Snowball slice invalid"));
    }
    SNOWBALL_RETURN_OK;
}

# define SLICE_CHECK(Z) SNOWBALL_PROPAGATE_ERR(slice_check(Z))

extern SNOWBALL_ERR slice_from_s(struct SN_env * z, int s_size, const symbol * s) {
    SLICE_CHECK(z);
    REPLACE_S(z, z->bra, z->ket, s_size, s);
    z->ket = z->bra + s_size;
    SNOWBALL_RETURN_OK;
}

extern SNOWBALL_ERR slice_from_v(struct SN_env * z, const symbol * p) {
    return slice_from_s(z, SIZE(p), p);
}

extern SNOWBALL_ERR slice_del(struct SN_env * z) {
    SLICE_CHECK(z);
    {
        int slice_size = z->ket - z->bra;
        if (slice_size != 0) {
            int len = SIZE(z->p);
            memmove(z->p + z->bra,
                    z->p + z->ket,
                    (len - z->ket) * sizeof(symbol));
            SET_SIZE(z->p, len - slice_size);
            z->l -= slice_size;
            if (z->c >= z->ket)
                z->c -= slice_size;
            else if (z->c > z->bra)
                z->c = z->bra;
        }
    }
    z->ket = z->bra;
    SNOWBALL_RETURN_OK;
}

extern SNOWBALL_ERR insert_s(struct SN_env * z, int bra, int ket, int s_size, const symbol * s) {
    REPLACE_S(z, bra, ket, s_size, s);
    if (bra <= z->ket) {
        int adjustment = s_size - (ket - bra);
        z->ket += adjustment;
        if (bra <= z->bra) z->bra += adjustment;
    }
    SNOWBALL_RETURN_OK;
}

extern SNOWBALL_ERR insert_v(struct SN_env * z, int bra, int ket, const symbol * p) {
    return insert_s(z, bra, ket, SIZE(p), p);
}

extern SNOWBALL_ERR slice_to(struct SN_env * z, symbol ** p) {
    SLICE_CHECK(z);
    {
        int len = z->ket - z->bra;
        if (CAPACITY(*p) < len) {
            SNOWBALL_PROPAGATE_ERR(increase_size(p, len));
        }
        memmove(*p, z->p + z->bra, len * sizeof(symbol));
        SET_SIZE(*p, len);
    }
    SNOWBALL_RETURN_OK;
}

extern SNOWBALL_ERR assign_to(struct SN_env * z, symbol ** p) {
    int len = z->l;
    if (CAPACITY(*p) < len) {
        SNOWBALL_PROPAGATE_ERR(increase_size(p, len));
    }
    memmove(*p, z->p, len * sizeof(symbol));
    SET_SIZE(*p, len);
    SNOWBALL_RETURN_OK;
}

extern int len_utf8(const symbol * p) {
    int size = SIZE(p);
    int len = 0;
    while (size--) {
        symbol b = *p++;
        if (b >= 0xC0 || b < 0x80) ++len;
    }
    return len;
}
