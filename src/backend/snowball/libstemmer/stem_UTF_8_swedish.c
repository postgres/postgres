/* Generated from swedish.sbl by Snowball 3.0.0 - https://snowballstem.org/ */

#include "stem_UTF_8_swedish.h"

#include <stddef.h>

#include "snowball_runtime.h"

struct SN_local {
    struct SN_env z;
    int i_p1;
};

typedef struct SN_local SN_local;

#ifdef __cplusplus
extern "C" {
#endif
extern int swedish_UTF_8_stem(struct SN_env * z);
#ifdef __cplusplus
}
#endif

static int r_other_suffix(struct SN_env * z);
static int r_consonant_pair(struct SN_env * z);
static int r_main_suffix(struct SN_env * z);
static int r_mark_regions(struct SN_env * z);
static int r_et_condition(struct SN_env * z);

static const symbol s_0[] = { 'e', 't' };
static const symbol s_1[] = { 0xC3, 0xB6, 's' };
static const symbol s_2[] = { 'f', 'u', 'l', 'l' };

static const symbol s_0_0[3] = { 'f', 'a', 'b' };
static const symbol s_0_1[1] = { 'h' };
static const symbol s_0_2[3] = { 'p', 'a', 'k' };
static const symbol s_0_3[3] = { 'r', 'a', 'k' };
static const symbol s_0_4[4] = { 's', 't', 'a', 'k' };
static const symbol s_0_5[3] = { 'k', 'o', 'm' };
static const symbol s_0_6[3] = { 'i', 'e', 't' };
static const symbol s_0_7[3] = { 'c', 'i', 't' };
static const symbol s_0_8[3] = { 'd', 'i', 't' };
static const symbol s_0_9[4] = { 'a', 'l', 'i', 't' };
static const symbol s_0_10[4] = { 'i', 'l', 'i', 't' };
static const symbol s_0_11[3] = { 'm', 'i', 't' };
static const symbol s_0_12[3] = { 'n', 'i', 't' };
static const symbol s_0_13[3] = { 'p', 'i', 't' };
static const symbol s_0_14[3] = { 'r', 'i', 't' };
static const symbol s_0_15[3] = { 's', 'i', 't' };
static const symbol s_0_16[3] = { 't', 'i', 't' };
static const symbol s_0_17[3] = { 'u', 'i', 't' };
static const symbol s_0_18[4] = { 'i', 'v', 'i', 't' };
static const symbol s_0_19[4] = { 'k', 'v', 'i', 't' };
static const symbol s_0_20[3] = { 'x', 'i', 't' };
static const struct among a_0[21] = {
{ 3, s_0_0, 0, -1, 0},
{ 1, s_0_1, 0, -1, 0},
{ 3, s_0_2, 0, -1, 0},
{ 3, s_0_3, 0, -1, 0},
{ 4, s_0_4, 0, -1, 0},
{ 3, s_0_5, 0, -1, 0},
{ 3, s_0_6, 0, -1, 0},
{ 3, s_0_7, 0, -1, 0},
{ 3, s_0_8, 0, -1, 0},
{ 4, s_0_9, 0, -1, 0},
{ 4, s_0_10, 0, -1, 0},
{ 3, s_0_11, 0, -1, 0},
{ 3, s_0_12, 0, -1, 0},
{ 3, s_0_13, 0, -1, 0},
{ 3, s_0_14, 0, -1, 0},
{ 3, s_0_15, 0, -1, 0},
{ 3, s_0_16, 0, -1, 0},
{ 3, s_0_17, 0, -1, 0},
{ 4, s_0_18, 0, -1, 0},
{ 4, s_0_19, 0, -1, 0},
{ 3, s_0_20, 0, -1, 0}
};

static const symbol s_1_0[1] = { 'a' };
static const symbol s_1_1[4] = { 'a', 'r', 'n', 'a' };
static const symbol s_1_2[4] = { 'e', 'r', 'n', 'a' };
static const symbol s_1_3[7] = { 'h', 'e', 't', 'e', 'r', 'n', 'a' };
static const symbol s_1_4[4] = { 'o', 'r', 'n', 'a' };
static const symbol s_1_5[2] = { 'a', 'd' };
static const symbol s_1_6[1] = { 'e' };
static const symbol s_1_7[3] = { 'a', 'd', 'e' };
static const symbol s_1_8[4] = { 'a', 'n', 'd', 'e' };
static const symbol s_1_9[4] = { 'a', 'r', 'n', 'e' };
static const symbol s_1_10[3] = { 'a', 'r', 'e' };
static const symbol s_1_11[4] = { 'a', 's', 't', 'e' };
static const symbol s_1_12[2] = { 'e', 'n' };
static const symbol s_1_13[5] = { 'a', 'n', 'd', 'e', 'n' };
static const symbol s_1_14[4] = { 'a', 'r', 'e', 'n' };
static const symbol s_1_15[5] = { 'h', 'e', 't', 'e', 'n' };
static const symbol s_1_16[3] = { 'e', 'r', 'n' };
static const symbol s_1_17[2] = { 'a', 'r' };
static const symbol s_1_18[2] = { 'e', 'r' };
static const symbol s_1_19[5] = { 'h', 'e', 't', 'e', 'r' };
static const symbol s_1_20[2] = { 'o', 'r' };
static const symbol s_1_21[1] = { 's' };
static const symbol s_1_22[2] = { 'a', 's' };
static const symbol s_1_23[5] = { 'a', 'r', 'n', 'a', 's' };
static const symbol s_1_24[5] = { 'e', 'r', 'n', 'a', 's' };
static const symbol s_1_25[5] = { 'o', 'r', 'n', 'a', 's' };
static const symbol s_1_26[2] = { 'e', 's' };
static const symbol s_1_27[4] = { 'a', 'd', 'e', 's' };
static const symbol s_1_28[5] = { 'a', 'n', 'd', 'e', 's' };
static const symbol s_1_29[3] = { 'e', 'n', 's' };
static const symbol s_1_30[5] = { 'a', 'r', 'e', 'n', 's' };
static const symbol s_1_31[6] = { 'h', 'e', 't', 'e', 'n', 's' };
static const symbol s_1_32[4] = { 'e', 'r', 'n', 's' };
static const symbol s_1_33[2] = { 'a', 't' };
static const symbol s_1_34[2] = { 'e', 't' };
static const symbol s_1_35[5] = { 'a', 'n', 'd', 'e', 't' };
static const symbol s_1_36[3] = { 'h', 'e', 't' };
static const symbol s_1_37[3] = { 'a', 's', 't' };
static const struct among a_1[38] = {
{ 1, s_1_0, 0, 1, 0},
{ 4, s_1_1, -1, 1, 0},
{ 4, s_1_2, -2, 1, 0},
{ 7, s_1_3, -1, 1, 0},
{ 4, s_1_4, -4, 1, 0},
{ 2, s_1_5, 0, 1, 0},
{ 1, s_1_6, 0, 1, 0},
{ 3, s_1_7, -1, 1, 0},
{ 4, s_1_8, -2, 1, 0},
{ 4, s_1_9, -3, 1, 0},
{ 3, s_1_10, -4, 1, 0},
{ 4, s_1_11, -5, 1, 0},
{ 2, s_1_12, 0, 1, 0},
{ 5, s_1_13, -1, 1, 0},
{ 4, s_1_14, -2, 1, 0},
{ 5, s_1_15, -3, 1, 0},
{ 3, s_1_16, 0, 1, 0},
{ 2, s_1_17, 0, 1, 0},
{ 2, s_1_18, 0, 1, 0},
{ 5, s_1_19, -1, 1, 0},
{ 2, s_1_20, 0, 1, 0},
{ 1, s_1_21, 0, 2, 0},
{ 2, s_1_22, -1, 1, 0},
{ 5, s_1_23, -1, 1, 0},
{ 5, s_1_24, -2, 1, 0},
{ 5, s_1_25, -3, 1, 0},
{ 2, s_1_26, -5, 1, 0},
{ 4, s_1_27, -1, 1, 0},
{ 5, s_1_28, -2, 1, 0},
{ 3, s_1_29, -8, 1, 0},
{ 5, s_1_30, -1, 1, 0},
{ 6, s_1_31, -2, 1, 0},
{ 4, s_1_32, -11, 1, 0},
{ 2, s_1_33, 0, 1, 0},
{ 2, s_1_34, 0, 3, 0},
{ 5, s_1_35, -1, 1, 0},
{ 3, s_1_36, -2, 1, 0},
{ 3, s_1_37, 0, 1, 0}
};

static const symbol s_2_0[2] = { 'd', 'd' };
static const symbol s_2_1[2] = { 'g', 'd' };
static const symbol s_2_2[2] = { 'n', 'n' };
static const symbol s_2_3[2] = { 'd', 't' };
static const symbol s_2_4[2] = { 'g', 't' };
static const symbol s_2_5[2] = { 'k', 't' };
static const symbol s_2_6[2] = { 't', 't' };
static const struct among a_2[7] = {
{ 2, s_2_0, 0, -1, 0},
{ 2, s_2_1, 0, -1, 0},
{ 2, s_2_2, 0, -1, 0},
{ 2, s_2_3, 0, -1, 0},
{ 2, s_2_4, 0, -1, 0},
{ 2, s_2_5, 0, -1, 0},
{ 2, s_2_6, 0, -1, 0}
};

static const symbol s_3_0[2] = { 'i', 'g' };
static const symbol s_3_1[3] = { 'l', 'i', 'g' };
static const symbol s_3_2[3] = { 'e', 'l', 's' };
static const symbol s_3_3[5] = { 'f', 'u', 'l', 'l', 't' };
static const symbol s_3_4[4] = { 0xC3, 0xB6, 's', 't' };
static const struct among a_3[5] = {
{ 2, s_3_0, 0, 1, 0},
{ 3, s_3_1, -1, 1, 0},
{ 3, s_3_2, 0, 1, 0},
{ 5, s_3_3, 0, 3, 0},
{ 4, s_3_4, 0, 2, 0}
};

static const unsigned char g_v[] = { 17, 65, 16, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 24, 0, 32 };

static const unsigned char g_s_ending[] = { 119, 127, 149 };

static const unsigned char g_ost_ending[] = { 173, 58 };

static int r_mark_regions(struct SN_env * z) {
    int i_x;
    ((SN_local *)z)->i_p1 = z->l;
    {
        int v_1 = z->c;
        {
            int ret = skip_utf8(z->p, z->c, z->l, 3);
            if (ret < 0) return 0;
            z->c = ret;
        }
        i_x = z->c;
        z->c = v_1;
    }
    {
        int ret = out_grouping_U(z, g_v, 97, 246, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    {
        int ret = in_grouping_U(z, g_v, 97, 246, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    ((SN_local *)z)->i_p1 = z->c;
    if (((SN_local *)z)->i_p1 >= i_x) goto lab0;
    ((SN_local *)z)->i_p1 = i_x;
lab0:
    return 1;
}

static int r_et_condition(struct SN_env * z) {
    {
        int v_1 = z->l - z->c;
        if (out_grouping_b_U(z, g_v, 97, 246, 0)) return 0;
        if (in_grouping_b_U(z, g_v, 97, 246, 0)) return 0;
        if (z->c > z->lb) goto lab0;
        return 0;
    lab0:
        z->c = z->l - v_1;
        {
            int v_2 = z->l - z->c;
            if (z->c <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((1059076 >> (z->p[z->c - 1] & 0x1f)) & 1)) goto lab1;
            if (!find_among_b(z, a_0, 21, 0)) goto lab1;
            return 0;
        lab1:
            z->c = z->l - v_2;
        }
    }
    return 1;
}

static int r_main_suffix(struct SN_env * z) {
    int among_var;
    {
        int v_1;
        if (z->c < ((SN_local *)z)->i_p1) return 0;
        v_1 = z->lb; z->lb = ((SN_local *)z)->i_p1;
        z->ket = z->c;
        if (z->c <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((1851442 >> (z->p[z->c - 1] & 0x1f)) & 1)) { z->lb = v_1; return 0; }
        among_var = find_among_b(z, a_1, 38, 0);
        if (!among_var) { z->lb = v_1; return 0; }
        z->bra = z->c;
        z->lb = v_1;
    }
    switch (among_var) {
        case 1:
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            do {
                int v_2 = z->l - z->c;
                if (!(eq_s_b(z, 2, s_0))) goto lab0;
                {
                    int ret = r_et_condition(z);
                    if (ret == 0) goto lab0;
                    if (ret < 0) return ret;
                }
                z->bra = z->c;
                break;
            lab0:
                z->c = z->l - v_2;
                if (in_grouping_b_U(z, g_s_ending, 98, 121, 0)) return 0;
            } while (0);
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 3:
            {
                int ret = r_et_condition(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_consonant_pair(struct SN_env * z) {
    {
        int v_1;
        if (z->c < ((SN_local *)z)->i_p1) return 0;
        v_1 = z->lb; z->lb = ((SN_local *)z)->i_p1;
        {
            int v_2 = z->l - z->c;
            if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((1064976 >> (z->p[z->c - 1] & 0x1f)) & 1)) { z->lb = v_1; return 0; }
            if (!find_among_b(z, a_2, 7, 0)) { z->lb = v_1; return 0; }
            z->c = z->l - v_2;
            z->ket = z->c;
            {
                int ret = skip_b_utf8(z->p, z->c, z->lb, 1);
                if (ret < 0) { z->lb = v_1; return 0; }
                z->c = ret;
            }
            z->bra = z->c;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
        }
        z->lb = v_1;
    }
    return 1;
}

static int r_other_suffix(struct SN_env * z) {
    int among_var;
    {
        int v_1;
        if (z->c < ((SN_local *)z)->i_p1) return 0;
        v_1 = z->lb; z->lb = ((SN_local *)z)->i_p1;
        z->ket = z->c;
        if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((1572992 >> (z->p[z->c - 1] & 0x1f)) & 1)) { z->lb = v_1; return 0; }
        among_var = find_among_b(z, a_3, 5, 0);
        if (!among_var) { z->lb = v_1; return 0; }
        z->bra = z->c;
        z->lb = v_1;
    }
    switch (among_var) {
        case 1:
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            if (in_grouping_b_U(z, g_ost_ending, 105, 118, 0)) return 0;
            {
                int ret = slice_from_s(z, 3, s_1);
                if (ret < 0) return ret;
            }
            break;
        case 3:
            {
                int ret = slice_from_s(z, 4, s_2);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

extern int swedish_UTF_8_stem(struct SN_env * z) {
    {
        int v_1 = z->c;
        {
            int ret = r_mark_regions(z);
            if (ret < 0) return ret;
        }
        z->c = v_1;
    }
    z->lb = z->c; z->c = z->l;
    {
        int v_2 = z->l - z->c;
        {
            int ret = r_main_suffix(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_2;
    }
    {
        int v_3 = z->l - z->c;
        {
            int ret = r_consonant_pair(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_3;
    }
    {
        int v_4 = z->l - z->c;
        {
            int ret = r_other_suffix(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_4;
    }
    z->c = z->lb;
    return 1;
}

extern struct SN_env * swedish_UTF_8_create_env(void) {
    struct SN_env * z = SN_new_env(sizeof(SN_local));
    if (z) {
        ((SN_local *)z)->i_p1 = 0;
    }
    return z;
}

extern void swedish_UTF_8_close_env(struct SN_env * z) {
    SN_delete_env(z);
}

