/* Generated from norwegian.sbl by Snowball 3.0.0 - https://snowballstem.org/ */

#include "stem_UTF_8_norwegian.h"

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
extern int norwegian_UTF_8_stem(struct SN_env * z);
#ifdef __cplusplus
}
#endif

static int r_other_suffix(struct SN_env * z);
static int r_consonant_pair(struct SN_env * z);
static int r_main_suffix(struct SN_env * z);
static int r_mark_regions(struct SN_env * z);

static const symbol s_0[] = { 'e', 'r' };

static const symbol s_0_1[3] = { 'i', 'n', 'd' };
static const symbol s_0_2[2] = { 'k', 'k' };
static const symbol s_0_3[2] = { 'n', 'k' };
static const symbol s_0_4[3] = { 'a', 'm', 'm' };
static const symbol s_0_5[3] = { 'o', 'm', 'm' };
static const symbol s_0_6[3] = { 'k', 'a', 'p' };
static const symbol s_0_7[4] = { 's', 'k', 'a', 'p' };
static const symbol s_0_8[2] = { 'p', 'p' };
static const symbol s_0_9[2] = { 'l', 't' };
static const symbol s_0_10[3] = { 'a', 's', 't' };
static const symbol s_0_11[4] = { 0xC3, 0xB8, 's', 't' };
static const symbol s_0_12[1] = { 'v' };
static const symbol s_0_13[3] = { 'h', 'a', 'v' };
static const symbol s_0_14[3] = { 'g', 'i', 'v' };
static const struct among a_0[15] = {
{ 0, 0, 0, 1, 0},
{ 3, s_0_1, -1, -1, 0},
{ 2, s_0_2, -2, -1, 0},
{ 2, s_0_3, -3, -1, 0},
{ 3, s_0_4, -4, -1, 0},
{ 3, s_0_5, -5, -1, 0},
{ 3, s_0_6, -6, -1, 0},
{ 4, s_0_7, -1, 1, 0},
{ 2, s_0_8, -8, -1, 0},
{ 2, s_0_9, -9, -1, 0},
{ 3, s_0_10, -10, -1, 0},
{ 4, s_0_11, -11, -1, 0},
{ 1, s_0_12, -12, -1, 0},
{ 3, s_0_13, -1, 1, 0},
{ 3, s_0_14, -2, 1, 0}
};

static const symbol s_1_0[1] = { 'a' };
static const symbol s_1_1[1] = { 'e' };
static const symbol s_1_2[3] = { 'e', 'd', 'e' };
static const symbol s_1_3[4] = { 'a', 'n', 'd', 'e' };
static const symbol s_1_4[4] = { 'e', 'n', 'd', 'e' };
static const symbol s_1_5[3] = { 'a', 'n', 'e' };
static const symbol s_1_6[3] = { 'e', 'n', 'e' };
static const symbol s_1_7[6] = { 'h', 'e', 't', 'e', 'n', 'e' };
static const symbol s_1_8[4] = { 'e', 'r', 't', 'e' };
static const symbol s_1_9[2] = { 'e', 'n' };
static const symbol s_1_10[5] = { 'h', 'e', 't', 'e', 'n' };
static const symbol s_1_11[2] = { 'a', 'r' };
static const symbol s_1_12[2] = { 'e', 'r' };
static const symbol s_1_13[5] = { 'h', 'e', 't', 'e', 'r' };
static const symbol s_1_14[1] = { 's' };
static const symbol s_1_15[2] = { 'a', 's' };
static const symbol s_1_16[2] = { 'e', 's' };
static const symbol s_1_17[4] = { 'e', 'd', 'e', 's' };
static const symbol s_1_18[5] = { 'e', 'n', 'd', 'e', 's' };
static const symbol s_1_19[4] = { 'e', 'n', 'e', 's' };
static const symbol s_1_20[7] = { 'h', 'e', 't', 'e', 'n', 'e', 's' };
static const symbol s_1_21[3] = { 'e', 'n', 's' };
static const symbol s_1_22[6] = { 'h', 'e', 't', 'e', 'n', 's' };
static const symbol s_1_23[3] = { 'e', 'r', 's' };
static const symbol s_1_24[3] = { 'e', 't', 's' };
static const symbol s_1_25[2] = { 'e', 't' };
static const symbol s_1_26[3] = { 'h', 'e', 't' };
static const symbol s_1_27[3] = { 'e', 'r', 't' };
static const symbol s_1_28[3] = { 'a', 's', 't' };
static const struct among a_1[29] = {
{ 1, s_1_0, 0, 1, 0},
{ 1, s_1_1, 0, 1, 0},
{ 3, s_1_2, -1, 1, 0},
{ 4, s_1_3, -2, 1, 0},
{ 4, s_1_4, -3, 1, 0},
{ 3, s_1_5, -4, 1, 0},
{ 3, s_1_6, -5, 1, 0},
{ 6, s_1_7, -1, 1, 0},
{ 4, s_1_8, -7, 4, 0},
{ 2, s_1_9, 0, 1, 0},
{ 5, s_1_10, -1, 1, 0},
{ 2, s_1_11, 0, 1, 0},
{ 2, s_1_12, 0, 1, 0},
{ 5, s_1_13, -1, 1, 0},
{ 1, s_1_14, 0, 3, 0},
{ 2, s_1_15, -1, 1, 0},
{ 2, s_1_16, -2, 1, 0},
{ 4, s_1_17, -1, 1, 0},
{ 5, s_1_18, -2, 1, 0},
{ 4, s_1_19, -3, 1, 0},
{ 7, s_1_20, -1, 1, 0},
{ 3, s_1_21, -7, 1, 0},
{ 6, s_1_22, -1, 1, 0},
{ 3, s_1_23, -9, 2, 0},
{ 3, s_1_24, -10, 1, 0},
{ 2, s_1_25, 0, 1, 0},
{ 3, s_1_26, -1, 1, 0},
{ 3, s_1_27, 0, 4, 0},
{ 3, s_1_28, 0, 1, 0}
};

static const symbol s_2_0[2] = { 'd', 't' };
static const symbol s_2_1[2] = { 'v', 't' };
static const struct among a_2[2] = {
{ 2, s_2_0, 0, -1, 0},
{ 2, s_2_1, 0, -1, 0}
};

static const symbol s_3_0[3] = { 'l', 'e', 'g' };
static const symbol s_3_1[4] = { 'e', 'l', 'e', 'g' };
static const symbol s_3_2[2] = { 'i', 'g' };
static const symbol s_3_3[3] = { 'e', 'i', 'g' };
static const symbol s_3_4[3] = { 'l', 'i', 'g' };
static const symbol s_3_5[4] = { 'e', 'l', 'i', 'g' };
static const symbol s_3_6[3] = { 'e', 'l', 's' };
static const symbol s_3_7[3] = { 'l', 'o', 'v' };
static const symbol s_3_8[4] = { 'e', 'l', 'o', 'v' };
static const symbol s_3_9[4] = { 's', 'l', 'o', 'v' };
static const symbol s_3_10[7] = { 'h', 'e', 't', 's', 'l', 'o', 'v' };
static const struct among a_3[11] = {
{ 3, s_3_0, 0, 1, 0},
{ 4, s_3_1, -1, 1, 0},
{ 2, s_3_2, 0, 1, 0},
{ 3, s_3_3, -1, 1, 0},
{ 3, s_3_4, -2, 1, 0},
{ 4, s_3_5, -1, 1, 0},
{ 3, s_3_6, 0, 1, 0},
{ 3, s_3_7, 0, 1, 0},
{ 4, s_3_8, -1, 1, 0},
{ 4, s_3_9, -2, 1, 0},
{ 7, s_3_10, -1, 1, 0}
};

static const unsigned char g_v[] = { 17, 65, 16, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 48, 2, 142 };

static const unsigned char g_s_ending[] = { 119, 125, 148, 1 };

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
        int ret = out_grouping_U(z, g_v, 97, 248, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    {
        int ret = in_grouping_U(z, g_v, 97, 248, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    ((SN_local *)z)->i_p1 = z->c;
    if (((SN_local *)z)->i_p1 >= i_x) goto lab0;
    ((SN_local *)z)->i_p1 = i_x;
lab0:
    return 1;
}

static int r_main_suffix(struct SN_env * z) {
    int among_var;
    {
        int v_1;
        if (z->c < ((SN_local *)z)->i_p1) return 0;
        v_1 = z->lb; z->lb = ((SN_local *)z)->i_p1;
        z->ket = z->c;
        if (z->c <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((1851426 >> (z->p[z->c - 1] & 0x1f)) & 1)) { z->lb = v_1; return 0; }
        among_var = find_among_b(z, a_1, 29, 0);
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
            if (z->c <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((5318672 >> (z->p[z->c - 1] & 0x1f)) & 1)) among_var = 1; else
            among_var = find_among_b(z, a_0, 15, 0);
            switch (among_var) {
                case 1:
                    {
                        int ret = slice_del(z);
                        if (ret < 0) return ret;
                    }
                    break;
            }
            break;
        case 3:
            do {
                int v_2 = z->l - z->c;
                if (in_grouping_b_U(z, g_s_ending, 98, 122, 0)) goto lab0;
                break;
            lab0:
                z->c = z->l - v_2;
                if (z->c <= z->lb || z->p[z->c - 1] != 'r') goto lab1;
                z->c--;
                {
                    int v_3 = z->l - z->c;
                    if (z->c <= z->lb || z->p[z->c - 1] != 'e') goto lab2;
                    z->c--;
                    goto lab1;
                lab2:
                    z->c = z->l - v_3;
                }
                break;
            lab1:
                z->c = z->l - v_2;
                if (z->c <= z->lb || z->p[z->c - 1] != 'k') return 0;
                z->c--;
                if (out_grouping_b_U(z, g_v, 97, 248, 0)) return 0;
            } while (0);
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 4:
            {
                int ret = slice_from_s(z, 2, s_0);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_consonant_pair(struct SN_env * z) {
    {
        int v_1 = z->l - z->c;
        {
            int v_2;
            if (z->c < ((SN_local *)z)->i_p1) return 0;
            v_2 = z->lb; z->lb = ((SN_local *)z)->i_p1;
            z->ket = z->c;
            if (z->c - 1 <= z->lb || z->p[z->c - 1] != 116) { z->lb = v_2; return 0; }
            if (!find_among_b(z, a_2, 2, 0)) { z->lb = v_2; return 0; }
            z->bra = z->c;
            z->lb = v_2;
        }
        z->c = z->l - v_1;
    }
    {
        int ret = skip_b_utf8(z->p, z->c, z->lb, 1);
        if (ret < 0) return 0;
        z->c = ret;
    }
    z->bra = z->c;
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_other_suffix(struct SN_env * z) {
    {
        int v_1;
        if (z->c < ((SN_local *)z)->i_p1) return 0;
        v_1 = z->lb; z->lb = ((SN_local *)z)->i_p1;
        z->ket = z->c;
        if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((4718720 >> (z->p[z->c - 1] & 0x1f)) & 1)) { z->lb = v_1; return 0; }
        if (!find_among_b(z, a_3, 11, 0)) { z->lb = v_1; return 0; }
        z->bra = z->c;
        z->lb = v_1;
    }
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return 1;
}

extern int norwegian_UTF_8_stem(struct SN_env * z) {
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

extern struct SN_env * norwegian_UTF_8_create_env(void) {
    struct SN_env * z = SN_new_env(sizeof(SN_local));
    if (z) {
        ((SN_local *)z)->i_p1 = 0;
    }
    return z;
}

extern void norwegian_UTF_8_close_env(struct SN_env * z) {
    SN_delete_env(z);
}

