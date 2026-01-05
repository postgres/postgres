/* Generated from russian.sbl by Snowball 3.0.0 - https://snowballstem.org/ */

#include "stem_KOI8_R_russian.h"

#include <stddef.h>

#include "snowball_runtime.h"

struct SN_local {
    struct SN_env z;
    int i_p2;
    int i_pV;
};

typedef struct SN_local SN_local;

#ifdef __cplusplus
extern "C" {
#endif
extern int russian_KOI8_R_stem(struct SN_env * z);
#ifdef __cplusplus
}
#endif

static int r_tidy_up(struct SN_env * z);
static int r_derivational(struct SN_env * z);
static int r_noun(struct SN_env * z);
static int r_verb(struct SN_env * z);
static int r_reflexive(struct SN_env * z);
static int r_adjectival(struct SN_env * z);
static int r_adjective(struct SN_env * z);
static int r_perfective_gerund(struct SN_env * z);
static int r_R2(struct SN_env * z);
static int r_mark_regions(struct SN_env * z);

static const symbol s_0[] = { 0xC5 };

static const symbol s_0_0[3] = { 0xD7, 0xDB, 0xC9 };
static const symbol s_0_1[4] = { 0xC9, 0xD7, 0xDB, 0xC9 };
static const symbol s_0_2[4] = { 0xD9, 0xD7, 0xDB, 0xC9 };
static const symbol s_0_3[1] = { 0xD7 };
static const symbol s_0_4[2] = { 0xC9, 0xD7 };
static const symbol s_0_5[2] = { 0xD9, 0xD7 };
static const symbol s_0_6[5] = { 0xD7, 0xDB, 0xC9, 0xD3, 0xD8 };
static const symbol s_0_7[6] = { 0xC9, 0xD7, 0xDB, 0xC9, 0xD3, 0xD8 };
static const symbol s_0_8[6] = { 0xD9, 0xD7, 0xDB, 0xC9, 0xD3, 0xD8 };
static const struct among a_0[9] = {
{ 3, s_0_0, 0, 1, 0},
{ 4, s_0_1, -1, 2, 0},
{ 4, s_0_2, -2, 2, 0},
{ 1, s_0_3, 0, 1, 0},
{ 2, s_0_4, -1, 2, 0},
{ 2, s_0_5, -2, 2, 0},
{ 5, s_0_6, 0, 1, 0},
{ 6, s_0_7, -1, 2, 0},
{ 6, s_0_8, -2, 2, 0}
};

static const symbol s_1_0[2] = { 0xC0, 0xC0 };
static const symbol s_1_1[2] = { 0xC5, 0xC0 };
static const symbol s_1_2[2] = { 0xCF, 0xC0 };
static const symbol s_1_3[2] = { 0xD5, 0xC0 };
static const symbol s_1_4[2] = { 0xC5, 0xC5 };
static const symbol s_1_5[2] = { 0xC9, 0xC5 };
static const symbol s_1_6[2] = { 0xCF, 0xC5 };
static const symbol s_1_7[2] = { 0xD9, 0xC5 };
static const symbol s_1_8[2] = { 0xC9, 0xC8 };
static const symbol s_1_9[2] = { 0xD9, 0xC8 };
static const symbol s_1_10[3] = { 0xC9, 0xCD, 0xC9 };
static const symbol s_1_11[3] = { 0xD9, 0xCD, 0xC9 };
static const symbol s_1_12[2] = { 0xC5, 0xCA };
static const symbol s_1_13[2] = { 0xC9, 0xCA };
static const symbol s_1_14[2] = { 0xCF, 0xCA };
static const symbol s_1_15[2] = { 0xD9, 0xCA };
static const symbol s_1_16[2] = { 0xC5, 0xCD };
static const symbol s_1_17[2] = { 0xC9, 0xCD };
static const symbol s_1_18[2] = { 0xCF, 0xCD };
static const symbol s_1_19[2] = { 0xD9, 0xCD };
static const symbol s_1_20[3] = { 0xC5, 0xC7, 0xCF };
static const symbol s_1_21[3] = { 0xCF, 0xC7, 0xCF };
static const symbol s_1_22[2] = { 0xC1, 0xD1 };
static const symbol s_1_23[2] = { 0xD1, 0xD1 };
static const symbol s_1_24[3] = { 0xC5, 0xCD, 0xD5 };
static const symbol s_1_25[3] = { 0xCF, 0xCD, 0xD5 };
static const struct among a_1[26] = {
{ 2, s_1_0, 0, 1, 0},
{ 2, s_1_1, 0, 1, 0},
{ 2, s_1_2, 0, 1, 0},
{ 2, s_1_3, 0, 1, 0},
{ 2, s_1_4, 0, 1, 0},
{ 2, s_1_5, 0, 1, 0},
{ 2, s_1_6, 0, 1, 0},
{ 2, s_1_7, 0, 1, 0},
{ 2, s_1_8, 0, 1, 0},
{ 2, s_1_9, 0, 1, 0},
{ 3, s_1_10, 0, 1, 0},
{ 3, s_1_11, 0, 1, 0},
{ 2, s_1_12, 0, 1, 0},
{ 2, s_1_13, 0, 1, 0},
{ 2, s_1_14, 0, 1, 0},
{ 2, s_1_15, 0, 1, 0},
{ 2, s_1_16, 0, 1, 0},
{ 2, s_1_17, 0, 1, 0},
{ 2, s_1_18, 0, 1, 0},
{ 2, s_1_19, 0, 1, 0},
{ 3, s_1_20, 0, 1, 0},
{ 3, s_1_21, 0, 1, 0},
{ 2, s_1_22, 0, 1, 0},
{ 2, s_1_23, 0, 1, 0},
{ 3, s_1_24, 0, 1, 0},
{ 3, s_1_25, 0, 1, 0}
};

static const symbol s_2_0[2] = { 0xC5, 0xCD };
static const symbol s_2_1[2] = { 0xCE, 0xCE };
static const symbol s_2_2[2] = { 0xD7, 0xDB };
static const symbol s_2_3[3] = { 0xC9, 0xD7, 0xDB };
static const symbol s_2_4[3] = { 0xD9, 0xD7, 0xDB };
static const symbol s_2_5[1] = { 0xDD };
static const symbol s_2_6[2] = { 0xC0, 0xDD };
static const symbol s_2_7[3] = { 0xD5, 0xC0, 0xDD };
static const struct among a_2[8] = {
{ 2, s_2_0, 0, 1, 0},
{ 2, s_2_1, 0, 1, 0},
{ 2, s_2_2, 0, 1, 0},
{ 3, s_2_3, -1, 2, 0},
{ 3, s_2_4, -2, 2, 0},
{ 1, s_2_5, 0, 1, 0},
{ 2, s_2_6, -1, 1, 0},
{ 3, s_2_7, -1, 2, 0}
};

static const symbol s_3_0[2] = { 0xD3, 0xD1 };
static const symbol s_3_1[2] = { 0xD3, 0xD8 };
static const struct among a_3[2] = {
{ 2, s_3_0, 0, 1, 0},
{ 2, s_3_1, 0, 1, 0}
};

static const symbol s_4_0[1] = { 0xC0 };
static const symbol s_4_1[2] = { 0xD5, 0xC0 };
static const symbol s_4_2[2] = { 0xCC, 0xC1 };
static const symbol s_4_3[3] = { 0xC9, 0xCC, 0xC1 };
static const symbol s_4_4[3] = { 0xD9, 0xCC, 0xC1 };
static const symbol s_4_5[2] = { 0xCE, 0xC1 };
static const symbol s_4_6[3] = { 0xC5, 0xCE, 0xC1 };
static const symbol s_4_7[3] = { 0xC5, 0xD4, 0xC5 };
static const symbol s_4_8[3] = { 0xC9, 0xD4, 0xC5 };
static const symbol s_4_9[3] = { 0xCA, 0xD4, 0xC5 };
static const symbol s_4_10[4] = { 0xC5, 0xCA, 0xD4, 0xC5 };
static const symbol s_4_11[4] = { 0xD5, 0xCA, 0xD4, 0xC5 };
static const symbol s_4_12[2] = { 0xCC, 0xC9 };
static const symbol s_4_13[3] = { 0xC9, 0xCC, 0xC9 };
static const symbol s_4_14[3] = { 0xD9, 0xCC, 0xC9 };
static const symbol s_4_15[1] = { 0xCA };
static const symbol s_4_16[2] = { 0xC5, 0xCA };
static const symbol s_4_17[2] = { 0xD5, 0xCA };
static const symbol s_4_18[1] = { 0xCC };
static const symbol s_4_19[2] = { 0xC9, 0xCC };
static const symbol s_4_20[2] = { 0xD9, 0xCC };
static const symbol s_4_21[2] = { 0xC5, 0xCD };
static const symbol s_4_22[2] = { 0xC9, 0xCD };
static const symbol s_4_23[2] = { 0xD9, 0xCD };
static const symbol s_4_24[1] = { 0xCE };
static const symbol s_4_25[2] = { 0xC5, 0xCE };
static const symbol s_4_26[2] = { 0xCC, 0xCF };
static const symbol s_4_27[3] = { 0xC9, 0xCC, 0xCF };
static const symbol s_4_28[3] = { 0xD9, 0xCC, 0xCF };
static const symbol s_4_29[2] = { 0xCE, 0xCF };
static const symbol s_4_30[3] = { 0xC5, 0xCE, 0xCF };
static const symbol s_4_31[3] = { 0xCE, 0xCE, 0xCF };
static const symbol s_4_32[2] = { 0xC0, 0xD4 };
static const symbol s_4_33[3] = { 0xD5, 0xC0, 0xD4 };
static const symbol s_4_34[2] = { 0xC5, 0xD4 };
static const symbol s_4_35[3] = { 0xD5, 0xC5, 0xD4 };
static const symbol s_4_36[2] = { 0xC9, 0xD4 };
static const symbol s_4_37[2] = { 0xD1, 0xD4 };
static const symbol s_4_38[2] = { 0xD9, 0xD4 };
static const symbol s_4_39[2] = { 0xD4, 0xD8 };
static const symbol s_4_40[3] = { 0xC9, 0xD4, 0xD8 };
static const symbol s_4_41[3] = { 0xD9, 0xD4, 0xD8 };
static const symbol s_4_42[3] = { 0xC5, 0xDB, 0xD8 };
static const symbol s_4_43[3] = { 0xC9, 0xDB, 0xD8 };
static const symbol s_4_44[2] = { 0xCE, 0xD9 };
static const symbol s_4_45[3] = { 0xC5, 0xCE, 0xD9 };
static const struct among a_4[46] = {
{ 1, s_4_0, 0, 2, 0},
{ 2, s_4_1, -1, 2, 0},
{ 2, s_4_2, 0, 1, 0},
{ 3, s_4_3, -1, 2, 0},
{ 3, s_4_4, -2, 2, 0},
{ 2, s_4_5, 0, 1, 0},
{ 3, s_4_6, -1, 2, 0},
{ 3, s_4_7, 0, 1, 0},
{ 3, s_4_8, 0, 2, 0},
{ 3, s_4_9, 0, 1, 0},
{ 4, s_4_10, -1, 2, 0},
{ 4, s_4_11, -2, 2, 0},
{ 2, s_4_12, 0, 1, 0},
{ 3, s_4_13, -1, 2, 0},
{ 3, s_4_14, -2, 2, 0},
{ 1, s_4_15, 0, 1, 0},
{ 2, s_4_16, -1, 2, 0},
{ 2, s_4_17, -2, 2, 0},
{ 1, s_4_18, 0, 1, 0},
{ 2, s_4_19, -1, 2, 0},
{ 2, s_4_20, -2, 2, 0},
{ 2, s_4_21, 0, 1, 0},
{ 2, s_4_22, 0, 2, 0},
{ 2, s_4_23, 0, 2, 0},
{ 1, s_4_24, 0, 1, 0},
{ 2, s_4_25, -1, 2, 0},
{ 2, s_4_26, 0, 1, 0},
{ 3, s_4_27, -1, 2, 0},
{ 3, s_4_28, -2, 2, 0},
{ 2, s_4_29, 0, 1, 0},
{ 3, s_4_30, -1, 2, 0},
{ 3, s_4_31, -2, 1, 0},
{ 2, s_4_32, 0, 1, 0},
{ 3, s_4_33, -1, 2, 0},
{ 2, s_4_34, 0, 1, 0},
{ 3, s_4_35, -1, 2, 0},
{ 2, s_4_36, 0, 2, 0},
{ 2, s_4_37, 0, 2, 0},
{ 2, s_4_38, 0, 2, 0},
{ 2, s_4_39, 0, 1, 0},
{ 3, s_4_40, -1, 2, 0},
{ 3, s_4_41, -2, 2, 0},
{ 3, s_4_42, 0, 1, 0},
{ 3, s_4_43, 0, 2, 0},
{ 2, s_4_44, 0, 1, 0},
{ 3, s_4_45, -1, 2, 0}
};

static const symbol s_5_0[1] = { 0xC0 };
static const symbol s_5_1[2] = { 0xC9, 0xC0 };
static const symbol s_5_2[2] = { 0xD8, 0xC0 };
static const symbol s_5_3[1] = { 0xC1 };
static const symbol s_5_4[1] = { 0xC5 };
static const symbol s_5_5[2] = { 0xC9, 0xC5 };
static const symbol s_5_6[2] = { 0xD8, 0xC5 };
static const symbol s_5_7[2] = { 0xC1, 0xC8 };
static const symbol s_5_8[2] = { 0xD1, 0xC8 };
static const symbol s_5_9[3] = { 0xC9, 0xD1, 0xC8 };
static const symbol s_5_10[1] = { 0xC9 };
static const symbol s_5_11[2] = { 0xC5, 0xC9 };
static const symbol s_5_12[2] = { 0xC9, 0xC9 };
static const symbol s_5_13[3] = { 0xC1, 0xCD, 0xC9 };
static const symbol s_5_14[3] = { 0xD1, 0xCD, 0xC9 };
static const symbol s_5_15[4] = { 0xC9, 0xD1, 0xCD, 0xC9 };
static const symbol s_5_16[1] = { 0xCA };
static const symbol s_5_17[2] = { 0xC5, 0xCA };
static const symbol s_5_18[3] = { 0xC9, 0xC5, 0xCA };
static const symbol s_5_19[2] = { 0xC9, 0xCA };
static const symbol s_5_20[2] = { 0xCF, 0xCA };
static const symbol s_5_21[2] = { 0xC1, 0xCD };
static const symbol s_5_22[2] = { 0xC5, 0xCD };
static const symbol s_5_23[3] = { 0xC9, 0xC5, 0xCD };
static const symbol s_5_24[2] = { 0xCF, 0xCD };
static const symbol s_5_25[2] = { 0xD1, 0xCD };
static const symbol s_5_26[3] = { 0xC9, 0xD1, 0xCD };
static const symbol s_5_27[1] = { 0xCF };
static const symbol s_5_28[1] = { 0xD1 };
static const symbol s_5_29[2] = { 0xC9, 0xD1 };
static const symbol s_5_30[2] = { 0xD8, 0xD1 };
static const symbol s_5_31[1] = { 0xD5 };
static const symbol s_5_32[2] = { 0xC5, 0xD7 };
static const symbol s_5_33[2] = { 0xCF, 0xD7 };
static const symbol s_5_34[1] = { 0xD8 };
static const symbol s_5_35[1] = { 0xD9 };
static const struct among a_5[36] = {
{ 1, s_5_0, 0, 1, 0},
{ 2, s_5_1, -1, 1, 0},
{ 2, s_5_2, -2, 1, 0},
{ 1, s_5_3, 0, 1, 0},
{ 1, s_5_4, 0, 1, 0},
{ 2, s_5_5, -1, 1, 0},
{ 2, s_5_6, -2, 1, 0},
{ 2, s_5_7, 0, 1, 0},
{ 2, s_5_8, 0, 1, 0},
{ 3, s_5_9, -1, 1, 0},
{ 1, s_5_10, 0, 1, 0},
{ 2, s_5_11, -1, 1, 0},
{ 2, s_5_12, -2, 1, 0},
{ 3, s_5_13, -3, 1, 0},
{ 3, s_5_14, -4, 1, 0},
{ 4, s_5_15, -1, 1, 0},
{ 1, s_5_16, 0, 1, 0},
{ 2, s_5_17, -1, 1, 0},
{ 3, s_5_18, -1, 1, 0},
{ 2, s_5_19, -3, 1, 0},
{ 2, s_5_20, -4, 1, 0},
{ 2, s_5_21, 0, 1, 0},
{ 2, s_5_22, 0, 1, 0},
{ 3, s_5_23, -1, 1, 0},
{ 2, s_5_24, 0, 1, 0},
{ 2, s_5_25, 0, 1, 0},
{ 3, s_5_26, -1, 1, 0},
{ 1, s_5_27, 0, 1, 0},
{ 1, s_5_28, 0, 1, 0},
{ 2, s_5_29, -1, 1, 0},
{ 2, s_5_30, -2, 1, 0},
{ 1, s_5_31, 0, 1, 0},
{ 2, s_5_32, 0, 1, 0},
{ 2, s_5_33, 0, 1, 0},
{ 1, s_5_34, 0, 1, 0},
{ 1, s_5_35, 0, 1, 0}
};

static const symbol s_6_0[3] = { 0xCF, 0xD3, 0xD4 };
static const symbol s_6_1[4] = { 0xCF, 0xD3, 0xD4, 0xD8 };
static const struct among a_6[2] = {
{ 3, s_6_0, 0, 1, 0},
{ 4, s_6_1, 0, 1, 0}
};

static const symbol s_7_0[4] = { 0xC5, 0xCA, 0xDB, 0xC5 };
static const symbol s_7_1[1] = { 0xCE };
static const symbol s_7_2[1] = { 0xD8 };
static const symbol s_7_3[3] = { 0xC5, 0xCA, 0xDB };
static const struct among a_7[4] = {
{ 4, s_7_0, 0, 1, 0},
{ 1, s_7_1, 0, 2, 0},
{ 1, s_7_2, 0, 3, 0},
{ 3, s_7_3, 0, 1, 0}
};

static const unsigned char g_v[] = { 35, 130, 34, 18 };

static int r_mark_regions(struct SN_env * z) {
    ((SN_local *)z)->i_pV = z->l;
    ((SN_local *)z)->i_p2 = z->l;
    {
        int v_1 = z->c;
        {
            int ret = out_grouping(z, g_v, 192, 220, 1);
            if (ret < 0) goto lab0;
            z->c += ret;
        }
        ((SN_local *)z)->i_pV = z->c;
        {
            int ret = in_grouping(z, g_v, 192, 220, 1);
            if (ret < 0) goto lab0;
            z->c += ret;
        }
        {
            int ret = out_grouping(z, g_v, 192, 220, 1);
            if (ret < 0) goto lab0;
            z->c += ret;
        }
        {
            int ret = in_grouping(z, g_v, 192, 220, 1);
            if (ret < 0) goto lab0;
            z->c += ret;
        }
        ((SN_local *)z)->i_p2 = z->c;
    lab0:
        z->c = v_1;
    }
    return 1;
}

static int r_R2(struct SN_env * z) {
    return ((SN_local *)z)->i_p2 <= z->c;
}

static int r_perfective_gerund(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    if (z->c <= z->lb || z->p[z->c - 1] >> 5 != 6 || !((25166336 >> (z->p[z->c - 1] & 0x1f)) & 1)) return 0;
    among_var = find_among_b(z, a_0, 9, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    switch (among_var) {
        case 1:
            do {
                int v_1 = z->l - z->c;
                if (z->c <= z->lb || z->p[z->c - 1] != 0xC1) goto lab0;
                z->c--;
                break;
            lab0:
                z->c = z->l - v_1;
                if (z->c <= z->lb || z->p[z->c - 1] != 0xD1) return 0;
                z->c--;
            } while (0);
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_adjective(struct SN_env * z) {
    z->ket = z->c;
    if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 6 || !((2271009 >> (z->p[z->c - 1] & 0x1f)) & 1)) return 0;
    if (!find_among_b(z, a_1, 26, 0)) return 0;
    z->bra = z->c;
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_adjectival(struct SN_env * z) {
    int among_var;
    {
        int ret = r_adjective(z);
        if (ret <= 0) return ret;
    }
    {
        int v_1 = z->l - z->c;
        z->ket = z->c;
        if (z->c <= z->lb || z->p[z->c - 1] >> 5 != 6 || !((671113216 >> (z->p[z->c - 1] & 0x1f)) & 1)) { z->c = z->l - v_1; goto lab0; }
        among_var = find_among_b(z, a_2, 8, 0);
        if (!among_var) { z->c = z->l - v_1; goto lab0; }
        z->bra = z->c;
        switch (among_var) {
            case 1:
                do {
                    int v_2 = z->l - z->c;
                    if (z->c <= z->lb || z->p[z->c - 1] != 0xC1) goto lab1;
                    z->c--;
                    break;
                lab1:
                    z->c = z->l - v_2;
                    if (z->c <= z->lb || z->p[z->c - 1] != 0xD1) { z->c = z->l - v_1; goto lab0; }
                    z->c--;
                } while (0);
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                break;
            case 2:
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                break;
        }
    lab0:
        ;
    }
    return 1;
}

static int r_reflexive(struct SN_env * z) {
    z->ket = z->c;
    if (z->c - 1 <= z->lb || (z->p[z->c - 1] != 209 && z->p[z->c - 1] != 216)) return 0;
    if (!find_among_b(z, a_3, 2, 0)) return 0;
    z->bra = z->c;
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_verb(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    if (z->c <= z->lb || z->p[z->c - 1] >> 5 != 6 || !((51443235 >> (z->p[z->c - 1] & 0x1f)) & 1)) return 0;
    among_var = find_among_b(z, a_4, 46, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    switch (among_var) {
        case 1:
            do {
                int v_1 = z->l - z->c;
                if (z->c <= z->lb || z->p[z->c - 1] != 0xC1) goto lab0;
                z->c--;
                break;
            lab0:
                z->c = z->l - v_1;
                if (z->c <= z->lb || z->p[z->c - 1] != 0xD1) return 0;
                z->c--;
            } while (0);
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_noun(struct SN_env * z) {
    z->ket = z->c;
    if (z->c <= z->lb || z->p[z->c - 1] >> 5 != 6 || !((60991267 >> (z->p[z->c - 1] & 0x1f)) & 1)) return 0;
    if (!find_among_b(z, a_5, 36, 0)) return 0;
    z->bra = z->c;
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_derivational(struct SN_env * z) {
    z->ket = z->c;
    if (z->c - 2 <= z->lb || (z->p[z->c - 1] != 212 && z->p[z->c - 1] != 216)) return 0;
    if (!find_among_b(z, a_6, 2, 0)) return 0;
    z->bra = z->c;
    {
        int ret = r_R2(z);
        if (ret <= 0) return ret;
    }
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_tidy_up(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    if (z->c <= z->lb || z->p[z->c - 1] >> 5 != 6 || !((151011360 >> (z->p[z->c - 1] & 0x1f)) & 1)) return 0;
    among_var = find_among_b(z, a_7, 4, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    switch (among_var) {
        case 1:
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            z->ket = z->c;
            if (z->c <= z->lb || z->p[z->c - 1] != 0xCE) return 0;
            z->c--;
            z->bra = z->c;
            if (z->c <= z->lb || z->p[z->c - 1] != 0xCE) return 0;
            z->c--;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            if (z->c <= z->lb || z->p[z->c - 1] != 0xCE) return 0;
            z->c--;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 3:
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

extern int russian_KOI8_R_stem(struct SN_env * z) {
    {
        int v_1 = z->c;
        while (1) {
            int v_2 = z->c;
            while (1) {
                int v_3 = z->c;
                z->bra = z->c;
                if (z->c == z->l || z->p[z->c] != 0xA3) goto lab2;
                z->c++;
                z->ket = z->c;
                z->c = v_3;
                break;
            lab2:
                z->c = v_3;
                if (z->c >= z->l) goto lab1;
                z->c++;
            }
            {
                int ret = slice_from_s(z, 1, s_0);
                if (ret < 0) return ret;
            }
            continue;
        lab1:
            z->c = v_2;
            break;
        }
        z->c = v_1;
    }
    {
        int ret = r_mark_regions(z);
        if (ret < 0) return ret;
    }
    z->lb = z->c; z->c = z->l;
    {
        int v_4;
        if (z->c < ((SN_local *)z)->i_pV) return 0;
        v_4 = z->lb; z->lb = ((SN_local *)z)->i_pV;
        {
            int v_5 = z->l - z->c;
            do {
                int v_6 = z->l - z->c;
                {
                    int ret = r_perfective_gerund(z);
                    if (ret == 0) goto lab4;
                    if (ret < 0) return ret;
                }
                break;
            lab4:
                z->c = z->l - v_6;
                {
                    int v_7 = z->l - z->c;
                    {
                        int ret = r_reflexive(z);
                        if (ret == 0) { z->c = z->l - v_7; goto lab5; }
                        if (ret < 0) return ret;
                    }
                lab5:
                    ;
                }
                do {
                    int v_8 = z->l - z->c;
                    {
                        int ret = r_adjectival(z);
                        if (ret == 0) goto lab6;
                        if (ret < 0) return ret;
                    }
                    break;
                lab6:
                    z->c = z->l - v_8;
                    {
                        int ret = r_verb(z);
                        if (ret == 0) goto lab7;
                        if (ret < 0) return ret;
                    }
                    break;
                lab7:
                    z->c = z->l - v_8;
                    {
                        int ret = r_noun(z);
                        if (ret == 0) goto lab3;
                        if (ret < 0) return ret;
                    }
                } while (0);
            } while (0);
        lab3:
            z->c = z->l - v_5;
        }
        {
            int v_9 = z->l - z->c;
            z->ket = z->c;
            if (z->c <= z->lb || z->p[z->c - 1] != 0xC9) { z->c = z->l - v_9; goto lab8; }
            z->c--;
            z->bra = z->c;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
        lab8:
            ;
        }
        {
            int v_10 = z->l - z->c;
            {
                int ret = r_derivational(z);
                if (ret < 0) return ret;
            }
            z->c = z->l - v_10;
        }
        {
            int v_11 = z->l - z->c;
            {
                int ret = r_tidy_up(z);
                if (ret < 0) return ret;
            }
            z->c = z->l - v_11;
        }
        z->lb = v_4;
    }
    z->c = z->lb;
    return 1;
}

extern struct SN_env * russian_KOI8_R_create_env(void) {
    struct SN_env * z = SN_new_env(sizeof(SN_local));
    if (z) {
        ((SN_local *)z)->i_p2 = 0;
        ((SN_local *)z)->i_pV = 0;
    }
    return z;
}

extern void russian_KOI8_R_close_env(struct SN_env * z) {
    SN_delete_env(z);
}

