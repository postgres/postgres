/* Generated from irish.sbl by Snowball 3.0.0 - https://snowballstem.org/ */

#include "stem_UTF_8_irish.h"

#include <stddef.h>

#include "snowball_runtime.h"

struct SN_local {
    struct SN_env z;
    int i_p2;
    int i_p1;
    int i_pV;
};

typedef struct SN_local SN_local;

#ifdef __cplusplus
extern "C" {
#endif
extern int irish_UTF_8_stem(struct SN_env * z);
#ifdef __cplusplus
}
#endif

static int r_verb_sfx(struct SN_env * z);
static int r_deriv(struct SN_env * z);
static int r_noun_sfx(struct SN_env * z);
static int r_mark_regions(struct SN_env * z);
static int r_initial_morph(struct SN_env * z);
static int r_RV(struct SN_env * z);
static int r_R2(struct SN_env * z);
static int r_R1(struct SN_env * z);

static const symbol s_0[] = { 'f' };
static const symbol s_1[] = { 's' };
static const symbol s_2[] = { 'b' };
static const symbol s_3[] = { 'c' };
static const symbol s_4[] = { 'd' };
static const symbol s_5[] = { 'g' };
static const symbol s_6[] = { 'p' };
static const symbol s_7[] = { 't' };
static const symbol s_8[] = { 'm' };
static const symbol s_9[] = { 'a', 'r', 'c' };
static const symbol s_10[] = { 'g', 'i', 'n' };
static const symbol s_11[] = { 'g', 'r', 'a', 'f' };
static const symbol s_12[] = { 'p', 'a', 'i', 't', 'e' };
static const symbol s_13[] = { 0xC3, 0xB3, 'i', 'd' };

static const symbol s_0_0[2] = { 'b', '\'' };
static const symbol s_0_1[2] = { 'b', 'h' };
static const symbol s_0_2[3] = { 'b', 'h', 'f' };
static const symbol s_0_3[2] = { 'b', 'p' };
static const symbol s_0_4[2] = { 'c', 'h' };
static const symbol s_0_5[2] = { 'd', '\'' };
static const symbol s_0_6[4] = { 'd', '\'', 'f', 'h' };
static const symbol s_0_7[2] = { 'd', 'h' };
static const symbol s_0_8[2] = { 'd', 't' };
static const symbol s_0_9[2] = { 'f', 'h' };
static const symbol s_0_10[2] = { 'g', 'c' };
static const symbol s_0_11[2] = { 'g', 'h' };
static const symbol s_0_12[2] = { 'h', '-' };
static const symbol s_0_13[2] = { 'm', '\'' };
static const symbol s_0_14[2] = { 'm', 'b' };
static const symbol s_0_15[2] = { 'm', 'h' };
static const symbol s_0_16[2] = { 'n', '-' };
static const symbol s_0_17[2] = { 'n', 'd' };
static const symbol s_0_18[2] = { 'n', 'g' };
static const symbol s_0_19[2] = { 'p', 'h' };
static const symbol s_0_20[2] = { 's', 'h' };
static const symbol s_0_21[2] = { 't', '-' };
static const symbol s_0_22[2] = { 't', 'h' };
static const symbol s_0_23[2] = { 't', 's' };
static const struct among a_0[24] = {
{ 2, s_0_0, 0, 1, 0},
{ 2, s_0_1, 0, 4, 0},
{ 3, s_0_2, -1, 2, 0},
{ 2, s_0_3, 0, 8, 0},
{ 2, s_0_4, 0, 5, 0},
{ 2, s_0_5, 0, 1, 0},
{ 4, s_0_6, -1, 2, 0},
{ 2, s_0_7, 0, 6, 0},
{ 2, s_0_8, 0, 9, 0},
{ 2, s_0_9, 0, 2, 0},
{ 2, s_0_10, 0, 5, 0},
{ 2, s_0_11, 0, 7, 0},
{ 2, s_0_12, 0, 1, 0},
{ 2, s_0_13, 0, 1, 0},
{ 2, s_0_14, 0, 4, 0},
{ 2, s_0_15, 0, 10, 0},
{ 2, s_0_16, 0, 1, 0},
{ 2, s_0_17, 0, 6, 0},
{ 2, s_0_18, 0, 7, 0},
{ 2, s_0_19, 0, 8, 0},
{ 2, s_0_20, 0, 3, 0},
{ 2, s_0_21, 0, 1, 0},
{ 2, s_0_22, 0, 9, 0},
{ 2, s_0_23, 0, 3, 0}
};

static const symbol s_1_0[7] = { 0xC3, 0xAD, 'o', 'c', 'h', 't', 'a' };
static const symbol s_1_1[8] = { 'a', 0xC3, 0xAD, 'o', 'c', 'h', 't', 'a' };
static const symbol s_1_2[3] = { 'i', 'r', 'e' };
static const symbol s_1_3[4] = { 'a', 'i', 'r', 'e' };
static const symbol s_1_4[3] = { 'a', 'b', 'h' };
static const symbol s_1_5[4] = { 'e', 'a', 'b', 'h' };
static const symbol s_1_6[3] = { 'i', 'b', 'h' };
static const symbol s_1_7[4] = { 'a', 'i', 'b', 'h' };
static const symbol s_1_8[3] = { 'a', 'm', 'h' };
static const symbol s_1_9[4] = { 'e', 'a', 'm', 'h' };
static const symbol s_1_10[3] = { 'i', 'm', 'h' };
static const symbol s_1_11[4] = { 'a', 'i', 'm', 'h' };
static const symbol s_1_12[6] = { 0xC3, 0xAD, 'o', 'c', 'h', 't' };
static const symbol s_1_13[7] = { 'a', 0xC3, 0xAD, 'o', 'c', 'h', 't' };
static const symbol s_1_14[4] = { 'i', 'r', 0xC3, 0xAD };
static const symbol s_1_15[5] = { 'a', 'i', 'r', 0xC3, 0xAD };
static const struct among a_1[16] = {
{ 7, s_1_0, 0, 1, 0},
{ 8, s_1_1, -1, 1, 0},
{ 3, s_1_2, 0, 2, 0},
{ 4, s_1_3, -1, 2, 0},
{ 3, s_1_4, 0, 1, 0},
{ 4, s_1_5, -1, 1, 0},
{ 3, s_1_6, 0, 1, 0},
{ 4, s_1_7, -1, 1, 0},
{ 3, s_1_8, 0, 1, 0},
{ 4, s_1_9, -1, 1, 0},
{ 3, s_1_10, 0, 1, 0},
{ 4, s_1_11, -1, 1, 0},
{ 6, s_1_12, 0, 1, 0},
{ 7, s_1_13, -1, 1, 0},
{ 4, s_1_14, 0, 2, 0},
{ 5, s_1_15, -1, 2, 0}
};

static const symbol s_2_0[9] = { 0xC3, 0xB3, 'i', 'd', 'e', 'a', 'c', 'h', 'a' };
static const symbol s_2_1[7] = { 'p', 'a', 't', 'a', 'c', 'h', 'a' };
static const symbol s_2_2[5] = { 'a', 'c', 'h', 't', 'a' };
static const symbol s_2_3[8] = { 'a', 'r', 'c', 'a', 'c', 'h', 't', 'a' };
static const symbol s_2_4[6] = { 'e', 'a', 'c', 'h', 't', 'a' };
static const symbol s_2_5[12] = { 'g', 'r', 'a', 'f', 'a', 0xC3, 0xAD, 'o', 'c', 'h', 't', 'a' };
static const symbol s_2_6[5] = { 'p', 'a', 'i', 't', 'e' };
static const symbol s_2_7[3] = { 'a', 'c', 'h' };
static const symbol s_2_8[4] = { 'e', 'a', 'c', 'h' };
static const symbol s_2_9[8] = { 0xC3, 0xB3, 'i', 'd', 'e', 'a', 'c', 'h' };
static const symbol s_2_10[7] = { 'g', 'i', 'n', 'e', 'a', 'c', 'h' };
static const symbol s_2_11[6] = { 'p', 'a', 't', 'a', 'c', 'h' };
static const symbol s_2_12[10] = { 'g', 'r', 'a', 'f', 'a', 0xC3, 0xAD, 'o', 'c', 'h' };
static const symbol s_2_13[7] = { 'p', 'a', 't', 'a', 'i', 'g', 'h' };
static const symbol s_2_14[7] = { 0xC3, 0xB3, 'i', 'd', 'i', 'g', 'h' };
static const symbol s_2_15[8] = { 'a', 'c', 'h', 't', 0xC3, 0xBA, 'i', 'l' };
static const symbol s_2_16[9] = { 'e', 'a', 'c', 'h', 't', 0xC3, 0xBA, 'i', 'l' };
static const symbol s_2_17[6] = { 'g', 'i', 'n', 'e', 'a', 's' };
static const symbol s_2_18[5] = { 'g', 'i', 'n', 'i', 's' };
static const symbol s_2_19[4] = { 'a', 'c', 'h', 't' };
static const symbol s_2_20[7] = { 'a', 'r', 'c', 'a', 'c', 'h', 't' };
static const symbol s_2_21[5] = { 'e', 'a', 'c', 'h', 't' };
static const symbol s_2_22[11] = { 'g', 'r', 'a', 'f', 'a', 0xC3, 0xAD, 'o', 'c', 'h', 't' };
static const symbol s_2_23[10] = { 'a', 'r', 'c', 'a', 'c', 'h', 't', 'a', 0xC3, 0xAD };
static const symbol s_2_24[14] = { 'g', 'r', 'a', 'f', 'a', 0xC3, 0xAD, 'o', 'c', 'h', 't', 'a', 0xC3, 0xAD };
static const struct among a_2[25] = {
{ 9, s_2_0, 0, 6, 0},
{ 7, s_2_1, 0, 5, 0},
{ 5, s_2_2, 0, 1, 0},
{ 8, s_2_3, -1, 2, 0},
{ 6, s_2_4, -2, 1, 0},
{ 12, s_2_5, 0, 4, 0},
{ 5, s_2_6, 0, 5, 0},
{ 3, s_2_7, 0, 1, 0},
{ 4, s_2_8, -1, 1, 0},
{ 8, s_2_9, -1, 6, 0},
{ 7, s_2_10, -2, 3, 0},
{ 6, s_2_11, -4, 5, 0},
{ 10, s_2_12, 0, 4, 0},
{ 7, s_2_13, 0, 5, 0},
{ 7, s_2_14, 0, 6, 0},
{ 8, s_2_15, 0, 1, 0},
{ 9, s_2_16, -1, 1, 0},
{ 6, s_2_17, 0, 3, 0},
{ 5, s_2_18, 0, 3, 0},
{ 4, s_2_19, 0, 1, 0},
{ 7, s_2_20, -1, 2, 0},
{ 5, s_2_21, -2, 1, 0},
{ 11, s_2_22, 0, 4, 0},
{ 10, s_2_23, 0, 2, 0},
{ 14, s_2_24, 0, 4, 0}
};

static const symbol s_3_0[4] = { 'i', 'm', 'i', 'd' };
static const symbol s_3_1[5] = { 'a', 'i', 'm', 'i', 'd' };
static const symbol s_3_2[5] = { 0xC3, 0xAD, 'm', 'i', 'd' };
static const symbol s_3_3[6] = { 'a', 0xC3, 0xAD, 'm', 'i', 'd' };
static const symbol s_3_4[3] = { 'a', 'd', 'h' };
static const symbol s_3_5[4] = { 'e', 'a', 'd', 'h' };
static const symbol s_3_6[5] = { 'f', 'a', 'i', 'd', 'h' };
static const symbol s_3_7[4] = { 'f', 'i', 'd', 'h' };
static const symbol s_3_8[4] = { 0xC3, 0xA1, 'i', 'l' };
static const symbol s_3_9[3] = { 'a', 'i', 'n' };
static const symbol s_3_10[4] = { 't', 'e', 'a', 'r' };
static const symbol s_3_11[3] = { 't', 'a', 'r' };
static const struct among a_3[12] = {
{ 4, s_3_0, 0, 1, 0},
{ 5, s_3_1, -1, 1, 0},
{ 5, s_3_2, 0, 1, 0},
{ 6, s_3_3, -1, 1, 0},
{ 3, s_3_4, 0, 2, 0},
{ 4, s_3_5, -1, 2, 0},
{ 5, s_3_6, 0, 1, 0},
{ 4, s_3_7, 0, 1, 0},
{ 4, s_3_8, 0, 2, 0},
{ 3, s_3_9, 0, 2, 0},
{ 4, s_3_10, 0, 2, 0},
{ 3, s_3_11, 0, 2, 0}
};

static const unsigned char g_v[] = { 17, 65, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 17, 4, 2 };

static int r_mark_regions(struct SN_env * z) {
    ((SN_local *)z)->i_pV = z->l;
    ((SN_local *)z)->i_p1 = z->l;
    ((SN_local *)z)->i_p2 = z->l;
    {
        int v_1 = z->c;
        {
            int ret = out_grouping_U(z, g_v, 97, 250, 1);
            if (ret < 0) goto lab0;
            z->c += ret;
        }
        ((SN_local *)z)->i_pV = z->c;
        {
            int ret = in_grouping_U(z, g_v, 97, 250, 1);
            if (ret < 0) goto lab0;
            z->c += ret;
        }
        ((SN_local *)z)->i_p1 = z->c;
        {
            int ret = out_grouping_U(z, g_v, 97, 250, 1);
            if (ret < 0) goto lab0;
            z->c += ret;
        }
        {
            int ret = in_grouping_U(z, g_v, 97, 250, 1);
            if (ret < 0) goto lab0;
            z->c += ret;
        }
        ((SN_local *)z)->i_p2 = z->c;
    lab0:
        z->c = v_1;
    }
    return 1;
}

static int r_initial_morph(struct SN_env * z) {
    int among_var;
    z->bra = z->c;
    among_var = find_among(z, a_0, 24, 0);
    if (!among_var) return 0;
    z->ket = z->c;
    switch (among_var) {
        case 1:
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {
                int ret = slice_from_s(z, 1, s_0);
                if (ret < 0) return ret;
            }
            break;
        case 3:
            {
                int ret = slice_from_s(z, 1, s_1);
                if (ret < 0) return ret;
            }
            break;
        case 4:
            {
                int ret = slice_from_s(z, 1, s_2);
                if (ret < 0) return ret;
            }
            break;
        case 5:
            {
                int ret = slice_from_s(z, 1, s_3);
                if (ret < 0) return ret;
            }
            break;
        case 6:
            {
                int ret = slice_from_s(z, 1, s_4);
                if (ret < 0) return ret;
            }
            break;
        case 7:
            {
                int ret = slice_from_s(z, 1, s_5);
                if (ret < 0) return ret;
            }
            break;
        case 8:
            {
                int ret = slice_from_s(z, 1, s_6);
                if (ret < 0) return ret;
            }
            break;
        case 9:
            {
                int ret = slice_from_s(z, 1, s_7);
                if (ret < 0) return ret;
            }
            break;
        case 10:
            {
                int ret = slice_from_s(z, 1, s_8);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_RV(struct SN_env * z) {
    return ((SN_local *)z)->i_pV <= z->c;
}

static int r_R1(struct SN_env * z) {
    return ((SN_local *)z)->i_p1 <= z->c;
}

static int r_R2(struct SN_env * z) {
    return ((SN_local *)z)->i_p2 <= z->c;
}

static int r_noun_sfx(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    among_var = find_among_b(z, a_1, 16, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    switch (among_var) {
        case 1:
            {
                int ret = r_R1(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {
                int ret = r_R2(z);
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

static int r_deriv(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    among_var = find_among_b(z, a_2, 25, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    switch (among_var) {
        case 1:
            {
                int ret = r_R2(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {
                int ret = slice_from_s(z, 3, s_9);
                if (ret < 0) return ret;
            }
            break;
        case 3:
            {
                int ret = slice_from_s(z, 3, s_10);
                if (ret < 0) return ret;
            }
            break;
        case 4:
            {
                int ret = slice_from_s(z, 4, s_11);
                if (ret < 0) return ret;
            }
            break;
        case 5:
            {
                int ret = slice_from_s(z, 5, s_12);
                if (ret < 0) return ret;
            }
            break;
        case 6:
            {
                int ret = slice_from_s(z, 4, s_13);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_verb_sfx(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    if (z->c - 2 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((282896 >> (z->p[z->c - 1] & 0x1f)) & 1)) return 0;
    among_var = find_among_b(z, a_3, 12, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    switch (among_var) {
        case 1:
            {
                int ret = r_RV(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {
                int ret = r_R1(z);
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

extern int irish_UTF_8_stem(struct SN_env * z) {
    {
        int v_1 = z->c;
        {
            int ret = r_initial_morph(z);
            if (ret < 0) return ret;
        }
        z->c = v_1;
    }
    {
        int ret = r_mark_regions(z);
        if (ret < 0) return ret;
    }
    z->lb = z->c; z->c = z->l;
    {
        int v_2 = z->l - z->c;
        {
            int ret = r_noun_sfx(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_2;
    }
    {
        int v_3 = z->l - z->c;
        {
            int ret = r_deriv(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_3;
    }
    {
        int v_4 = z->l - z->c;
        {
            int ret = r_verb_sfx(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_4;
    }
    z->c = z->lb;
    return 1;
}

extern struct SN_env * irish_UTF_8_create_env(void) {
    struct SN_env * z = SN_new_env(sizeof(SN_local));
    if (z) {
        ((SN_local *)z)->i_p2 = 0;
        ((SN_local *)z)->i_p1 = 0;
        ((SN_local *)z)->i_pV = 0;
    }
    return z;
}

extern void irish_UTF_8_close_env(struct SN_env * z) {
    SN_delete_env(z);
}

