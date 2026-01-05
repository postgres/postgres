/* Generated from esperanto.sbl by Snowball 3.0.0 - https://snowballstem.org/ */

#include "stem_UTF_8_esperanto.h"

#include <stddef.h>

#include "snowball_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif
extern int esperanto_UTF_8_stem(struct SN_env * z);
#ifdef __cplusplus
}
#endif

static int r_uninflected(struct SN_env * z);
static int r_ujn_suffix(struct SN_env * z);
static int r_standard_suffix(struct SN_env * z);
static int r_pronoun(struct SN_env * z);
static int r_merged_numeral(struct SN_env * z);
static int r_long_word(struct SN_env * z);
static int r_initial_apostrophe(struct SN_env * z);
static int r_final_apostrophe(struct SN_env * z);
static int r_correlative(struct SN_env * z);
static int r_canonical_form(struct SN_env * z);

static const symbol s_0[] = { 0xC4, 0x89 };
static const symbol s_1[] = { 0xC4, 0x9D };
static const symbol s_2[] = { 0xC4, 0xA5 };
static const symbol s_3[] = { 0xC4, 0xB5 };
static const symbol s_4[] = { 0xC5, 0x9D };
static const symbol s_5[] = { 0xC5, 0xAD };
static const symbol s_6[] = { 'a' };
static const symbol s_7[] = { 'e' };
static const symbol s_8[] = { 'i' };
static const symbol s_9[] = { 'o' };
static const symbol s_10[] = { 'u' };
static const symbol s_11[] = { 's', 't' };
static const symbol s_12[] = { 'e' };
static const symbol s_13[] = { 'a' };
static const symbol s_14[] = { 'u', 'n' };
static const symbol s_15[] = { 'u' };
static const symbol s_16[] = { 'a', 0xC5, 0xAD };
static const symbol s_17[] = { 'o' };

static const symbol s_0_1[1] = { '-' };
static const symbol s_0_2[2] = { 'c', 'x' };
static const symbol s_0_3[2] = { 'g', 'x' };
static const symbol s_0_4[2] = { 'h', 'x' };
static const symbol s_0_5[2] = { 'j', 'x' };
static const symbol s_0_6[1] = { 'q' };
static const symbol s_0_7[2] = { 's', 'x' };
static const symbol s_0_8[2] = { 'u', 'x' };
static const symbol s_0_9[1] = { 'w' };
static const symbol s_0_10[1] = { 'x' };
static const symbol s_0_11[1] = { 'y' };
static const symbol s_0_12[2] = { 0xC3, 0xA1 };
static const symbol s_0_13[2] = { 0xC3, 0xA9 };
static const symbol s_0_14[2] = { 0xC3, 0xAD };
static const symbol s_0_15[2] = { 0xC3, 0xB3 };
static const symbol s_0_16[2] = { 0xC3, 0xBA };
static const struct among a_0[17] = {
{ 0, 0, 0, 14, 0},
{ 1, s_0_1, -1, 13, 0},
{ 2, s_0_2, -2, 1, 0},
{ 2, s_0_3, -3, 2, 0},
{ 2, s_0_4, -4, 3, 0},
{ 2, s_0_5, -5, 4, 0},
{ 1, s_0_6, -6, 12, 0},
{ 2, s_0_7, -7, 5, 0},
{ 2, s_0_8, -8, 6, 0},
{ 1, s_0_9, -9, 12, 0},
{ 1, s_0_10, -10, 12, 0},
{ 1, s_0_11, -11, 12, 0},
{ 2, s_0_12, -12, 7, 0},
{ 2, s_0_13, -13, 8, 0},
{ 2, s_0_14, -14, 9, 0},
{ 2, s_0_15, -15, 10, 0},
{ 2, s_0_16, -16, 11, 0}
};

static const symbol s_1_0[2] = { 'a', 's' };
static const symbol s_1_1[1] = { 'i' };
static const symbol s_1_2[2] = { 'i', 's' };
static const symbol s_1_3[2] = { 'o', 's' };
static const symbol s_1_4[1] = { 'u' };
static const symbol s_1_5[2] = { 'u', 's' };
static const struct among a_1[6] = {
{ 2, s_1_0, 0, -1, 0},
{ 1, s_1_1, 0, -1, 0},
{ 2, s_1_2, -1, -1, 0},
{ 2, s_1_3, 0, -1, 0},
{ 1, s_1_4, 0, -1, 0},
{ 2, s_1_5, -1, -1, 0}
};

static const symbol s_2_0[2] = { 'c', 'i' };
static const symbol s_2_1[2] = { 'g', 'i' };
static const symbol s_2_2[2] = { 'h', 'i' };
static const symbol s_2_3[2] = { 'l', 'i' };
static const symbol s_2_4[3] = { 'i', 'l', 'i' };
static const symbol s_2_5[4] = { 0xC5, 0x9D, 'l', 'i' };
static const symbol s_2_6[2] = { 'm', 'i' };
static const symbol s_2_7[2] = { 'n', 'i' };
static const symbol s_2_8[3] = { 'o', 'n', 'i' };
static const symbol s_2_9[2] = { 'r', 'i' };
static const symbol s_2_10[2] = { 's', 'i' };
static const symbol s_2_11[2] = { 'v', 'i' };
static const symbol s_2_12[3] = { 'i', 'v', 'i' };
static const symbol s_2_13[3] = { 0xC4, 0x9D, 'i' };
static const symbol s_2_14[3] = { 0xC5, 0x9D, 'i' };
static const symbol s_2_15[4] = { 'i', 0xC5, 0x9D, 'i' };
static const symbol s_2_16[6] = { 'm', 'a', 'l', 0xC5, 0x9D, 'i' };
static const struct among a_2[17] = {
{ 2, s_2_0, 0, -1, 0},
{ 2, s_2_1, 0, -1, 0},
{ 2, s_2_2, 0, -1, 0},
{ 2, s_2_3, 0, -1, 0},
{ 3, s_2_4, -1, -1, 0},
{ 4, s_2_5, -2, -1, 0},
{ 2, s_2_6, 0, -1, 0},
{ 2, s_2_7, 0, -1, 0},
{ 3, s_2_8, -1, -1, 0},
{ 2, s_2_9, 0, -1, 0},
{ 2, s_2_10, 0, -1, 0},
{ 2, s_2_11, 0, -1, 0},
{ 3, s_2_12, -1, -1, 0},
{ 3, s_2_13, 0, -1, 0},
{ 3, s_2_14, 0, -1, 0},
{ 4, s_2_15, -1, -1, 0},
{ 6, s_2_16, -2, -1, 0}
};

static const symbol s_3_0[3] = { 'a', 'm', 'b' };
static const symbol s_3_1[4] = { 'b', 'a', 'l', 'd' };
static const symbol s_3_2[7] = { 'm', 'a', 'l', 'b', 'a', 'l', 'd' };
static const symbol s_3_3[4] = { 'm', 'o', 'r', 'g' };
static const symbol s_3_4[8] = { 'p', 'o', 's', 't', 'm', 'o', 'r', 'g' };
static const symbol s_3_5[3] = { 'a', 'd', 'i' };
static const symbol s_3_6[4] = { 'h', 'o', 'd', 'i' };
static const symbol s_3_7[3] = { 'a', 'n', 'k' };
static const symbol s_3_8[5] = { 0xC4, 0x89, 'i', 'r', 'k' };
static const symbol s_3_9[8] = { 't', 'u', 't', 0xC4, 0x89, 'i', 'r', 'k' };
static const symbol s_3_10[5] = { 'p', 'r', 'e', 's', 'k' };
static const symbol s_3_11[5] = { 'a', 'l', 'm', 'e', 'n' };
static const symbol s_3_12[4] = { 'a', 'p', 'e', 'n' };
static const symbol s_3_13[4] = { 'h', 'i', 'e', 'r' };
static const symbol s_3_14[10] = { 'a', 'n', 't', 'a', 0xC5, 0xAD, 'h', 'i', 'e', 'r' };
static const symbol s_3_15[5] = { 'm', 'a', 'l', 'g', 'r' };
static const symbol s_3_16[5] = { 'a', 'n', 'k', 'o', 'r' };
static const symbol s_3_17[5] = { 'k', 'o', 'n', 't', 'r' };
static const symbol s_3_18[6] = { 'a', 'n', 's', 't', 'a', 't' };
static const symbol s_3_19[4] = { 'k', 'v', 'a', 'z' };
static const struct among a_3[20] = {
{ 3, s_3_0, 0, -1, 0},
{ 4, s_3_1, 0, -1, 0},
{ 7, s_3_2, -1, -1, 0},
{ 4, s_3_3, 0, -1, 0},
{ 8, s_3_4, -1, -1, 0},
{ 3, s_3_5, 0, -1, 0},
{ 4, s_3_6, 0, -1, 0},
{ 3, s_3_7, 0, -1, 0},
{ 5, s_3_8, 0, -1, 0},
{ 8, s_3_9, -1, -1, 0},
{ 5, s_3_10, 0, -1, 0},
{ 5, s_3_11, 0, -1, 0},
{ 4, s_3_12, 0, -1, 0},
{ 4, s_3_13, 0, -1, 0},
{ 10, s_3_14, -1, -1, 0},
{ 5, s_3_15, 0, -1, 0},
{ 5, s_3_16, 0, -1, 0},
{ 5, s_3_17, 0, -1, 0},
{ 6, s_3_18, 0, -1, 0},
{ 4, s_3_19, 0, -1, 0}
};

static const symbol s_4_0[4] = { 'a', 'l', 'i', 'u' };
static const symbol s_4_1[3] = { 'u', 'n', 'u' };
static const struct among a_4[2] = {
{ 4, s_4_0, 0, -1, 0},
{ 3, s_4_1, 0, -1, 0}
};

static const symbol s_5_0[3] = { 'a', 'h', 'a' };
static const symbol s_5_1[4] = { 'h', 'a', 'h', 'a' };
static const symbol s_5_2[8] = { 'h', 'a', 'l', 'e', 'l', 'u', 'j', 'a' };
static const symbol s_5_3[4] = { 'h', 'o', 'l', 'a' };
static const symbol s_5_4[6] = { 'h', 'o', 's', 'a', 'n', 'a' };
static const symbol s_5_5[6] = { 'm', 'a', 'l', 't', 'r', 'a' };
static const symbol s_5_6[4] = { 'h', 'u', 'r', 'a' };
static const symbol s_5_7[6] = { 0xC4, 0xA5, 'a', 0xC4, 0xA5, 'a' };
static const symbol s_5_8[4] = { 'e', 'k', 'd', 'e' };
static const symbol s_5_9[4] = { 'e', 'l', 'd', 'e' };
static const symbol s_5_10[5] = { 'd', 'i', 's', 'd', 'e' };
static const symbol s_5_11[3] = { 'e', 'h', 'e' };
static const symbol s_5_12[6] = { 'm', 'a', 'l', 't', 'r', 'e' };
static const symbol s_5_13[9] = { 'd', 'i', 'r', 'l', 'i', 'd', 'i', 'd', 'i' };
static const symbol s_5_14[6] = { 'm', 'a', 'l', 'p', 'l', 'i' };
static const symbol s_5_15[6] = { 'm', 'a', 'l', 0xC4, 0x89, 'i' };
static const symbol s_5_16[6] = { 'm', 'a', 'l', 'k', 'a', 'j' };
static const symbol s_5_17[4] = { 'a', 'm', 'e', 'n' };
static const symbol s_5_18[5] = { 't', 'a', 'm', 'e', 'n' };
static const symbol s_5_19[3] = { 'o', 'h', 'o' };
static const symbol s_5_20[6] = { 'm', 'a', 'l', 't', 'r', 'o' };
static const symbol s_5_21[5] = { 'm', 'i', 'n', 'u', 's' };
static const symbol s_5_22[3] = { 'u', 'h', 'u' };
static const symbol s_5_23[3] = { 'm', 'u', 'u' };
static const struct among a_5[24] = {
{ 3, s_5_0, 0, -1, 0},
{ 4, s_5_1, -1, -1, 0},
{ 8, s_5_2, 0, -1, 0},
{ 4, s_5_3, 0, -1, 0},
{ 6, s_5_4, 0, -1, 0},
{ 6, s_5_5, 0, -1, 0},
{ 4, s_5_6, 0, -1, 0},
{ 6, s_5_7, 0, -1, 0},
{ 4, s_5_8, 0, -1, 0},
{ 4, s_5_9, 0, -1, 0},
{ 5, s_5_10, 0, -1, 0},
{ 3, s_5_11, 0, -1, 0},
{ 6, s_5_12, 0, -1, 0},
{ 9, s_5_13, 0, -1, 0},
{ 6, s_5_14, 0, -1, 0},
{ 6, s_5_15, 0, -1, 0},
{ 6, s_5_16, 0, -1, 0},
{ 4, s_5_17, 0, -1, 0},
{ 5, s_5_18, -1, -1, 0},
{ 3, s_5_19, 0, -1, 0},
{ 6, s_5_20, 0, -1, 0},
{ 5, s_5_21, 0, -1, 0},
{ 3, s_5_22, 0, -1, 0},
{ 3, s_5_23, 0, -1, 0}
};

static const symbol s_6_0[3] = { 't', 'r', 'i' };
static const symbol s_6_1[2] = { 'd', 'u' };
static const symbol s_6_2[3] = { 'u', 'n', 'u' };
static const struct among a_6[3] = {
{ 3, s_6_0, 0, -1, 0},
{ 2, s_6_1, 0, -1, 0},
{ 3, s_6_2, 0, -1, 0}
};

static const symbol s_7_0[3] = { 'd', 'e', 'k' };
static const symbol s_7_1[4] = { 'c', 'e', 'n', 't' };
static const struct among a_7[2] = {
{ 3, s_7_0, 0, -1, 0},
{ 4, s_7_1, 0, -1, 0}
};

static const symbol s_8_0[1] = { 'k' };
static const symbol s_8_1[4] = { 'k', 'e', 'l', 'k' };
static const symbol s_8_2[3] = { 'n', 'e', 'n' };
static const symbol s_8_3[1] = { 't' };
static const symbol s_8_4[4] = { 'm', 'u', 'l', 't' };
static const symbol s_8_5[4] = { 's', 'a', 'm', 't' };
static const symbol s_8_6[2] = { 0xC4, 0x89 };
static const struct among a_8[7] = {
{ 1, s_8_0, 0, -1, 0},
{ 4, s_8_1, -1, -1, 0},
{ 3, s_8_2, 0, -1, 0},
{ 1, s_8_3, 0, -1, 0},
{ 4, s_8_4, -1, -1, 0},
{ 4, s_8_5, -2, -1, 0},
{ 2, s_8_6, 0, -1, 0}
};

static const symbol s_9_0[1] = { 'a' };
static const symbol s_9_1[1] = { 'e' };
static const symbol s_9_2[1] = { 'i' };
static const symbol s_9_3[1] = { 'j' };
static const symbol s_9_4[2] = { 'a', 'j' };
static const symbol s_9_5[2] = { 'o', 'j' };
static const symbol s_9_6[1] = { 'n' };
static const symbol s_9_7[2] = { 'a', 'n' };
static const symbol s_9_8[2] = { 'e', 'n' };
static const symbol s_9_9[2] = { 'j', 'n' };
static const symbol s_9_10[3] = { 'a', 'j', 'n' };
static const symbol s_9_11[3] = { 'o', 'j', 'n' };
static const symbol s_9_12[2] = { 'o', 'n' };
static const symbol s_9_13[1] = { 'o' };
static const symbol s_9_14[2] = { 'a', 's' };
static const symbol s_9_15[2] = { 'i', 's' };
static const symbol s_9_16[2] = { 'o', 's' };
static const symbol s_9_17[2] = { 'u', 's' };
static const symbol s_9_18[1] = { 'u' };
static const struct among a_9[19] = {
{ 1, s_9_0, 0, -1, 0},
{ 1, s_9_1, 0, -1, 0},
{ 1, s_9_2, 0, -1, 0},
{ 1, s_9_3, 0, 1, 0},
{ 2, s_9_4, -1, -1, 0},
{ 2, s_9_5, -2, -1, 0},
{ 1, s_9_6, 0, 1, 0},
{ 2, s_9_7, -1, -1, 0},
{ 2, s_9_8, -2, -1, 0},
{ 2, s_9_9, -3, 1, 0},
{ 3, s_9_10, -1, -1, 0},
{ 3, s_9_11, -2, -1, 0},
{ 2, s_9_12, -6, -1, 0},
{ 1, s_9_13, 0, -1, 0},
{ 2, s_9_14, 0, -1, 0},
{ 2, s_9_15, 0, -1, 0},
{ 2, s_9_16, 0, -1, 0},
{ 2, s_9_17, 0, -1, 0},
{ 1, s_9_18, 0, -1, 0}
};

static const unsigned char g_vowel[] = { 17, 65, 16 };

static const unsigned char g_aou[] = { 1, 64, 16 };

static const unsigned char g_digit[] = { 255, 3 };

static int r_canonical_form(struct SN_env * z) {
    int among_var;
    int b_foreign;
    b_foreign = 0;
    while (1) {
        int v_1 = z->c;
        z->bra = z->c;
        among_var = find_among(z, a_0, 17, 0);
        z->ket = z->c;
        switch (among_var) {
            case 1:
                {
                    int ret = slice_from_s(z, 2, s_0);
                    if (ret < 0) return ret;
                }
                break;
            case 2:
                {
                    int ret = slice_from_s(z, 2, s_1);
                    if (ret < 0) return ret;
                }
                break;
            case 3:
                {
                    int ret = slice_from_s(z, 2, s_2);
                    if (ret < 0) return ret;
                }
                break;
            case 4:
                {
                    int ret = slice_from_s(z, 2, s_3);
                    if (ret < 0) return ret;
                }
                break;
            case 5:
                {
                    int ret = slice_from_s(z, 2, s_4);
                    if (ret < 0) return ret;
                }
                break;
            case 6:
                {
                    int ret = slice_from_s(z, 2, s_5);
                    if (ret < 0) return ret;
                }
                break;
            case 7:
                {
                    int ret = slice_from_s(z, 1, s_6);
                    if (ret < 0) return ret;
                }
                b_foreign = 1;
                break;
            case 8:
                {
                    int ret = slice_from_s(z, 1, s_7);
                    if (ret < 0) return ret;
                }
                b_foreign = 1;
                break;
            case 9:
                {
                    int ret = slice_from_s(z, 1, s_8);
                    if (ret < 0) return ret;
                }
                b_foreign = 1;
                break;
            case 10:
                {
                    int ret = slice_from_s(z, 1, s_9);
                    if (ret < 0) return ret;
                }
                b_foreign = 1;
                break;
            case 11:
                {
                    int ret = slice_from_s(z, 1, s_10);
                    if (ret < 0) return ret;
                }
                b_foreign = 1;
                break;
            case 12:
                b_foreign = 1;
                break;
            case 13:
                b_foreign = 0;
                break;
            case 14:
                {
                    int ret = skip_utf8(z->p, z->c, z->l, 1);
                    if (ret < 0) goto lab0;
                    z->c = ret;
                }
                break;
        }
        continue;
    lab0:
        z->c = v_1;
        break;
    }
    return !b_foreign;
}

static int r_initial_apostrophe(struct SN_env * z) {
    z->bra = z->c;
    if (z->c == z->l || z->p[z->c] != '\'') return 0;
    z->c++;
    z->ket = z->c;
    if (!(eq_s(z, 2, s_11))) return 0;
    if (z->c >= z->l || z->p[z->c + 0] >> 5 != 3 || !((2130434 >> (z->p[z->c + 0] & 0x1f)) & 1)) return 0;
    if (!find_among(z, a_1, 6, 0)) return 0;
    if (z->c < z->l) return 0;
    {
        int ret = slice_from_s(z, 1, s_12);
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_pronoun(struct SN_env * z) {
    z->ket = z->c;
    {
        int v_1 = z->l - z->c;
        if (z->c <= z->lb || z->p[z->c - 1] != 'n') { z->c = z->l - v_1; goto lab0; }
        z->c--;
    lab0:
        ;
    }
    z->bra = z->c;
    if (z->c - 1 <= z->lb || z->p[z->c - 1] != 105) return 0;
    if (!find_among_b(z, a_2, 17, 0)) return 0;
    do {
        int v_2 = z->l - z->c;
        if (z->c > z->lb) goto lab1;
        break;
    lab1:
        z->c = z->l - v_2;
        if (z->c <= z->lb || z->p[z->c - 1] != '-') return 0;
        z->c--;
    } while (0);
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_final_apostrophe(struct SN_env * z) {
    z->ket = z->c;
    if (z->c <= z->lb || z->p[z->c - 1] != '\'') return 0;
    z->c--;
    z->bra = z->c;
    do {
        int v_1 = z->l - z->c;
        if (z->c <= z->lb || z->p[z->c - 1] != 'l') goto lab0;
        z->c--;
        if (z->c > z->lb) goto lab0;
        {
            int ret = slice_from_s(z, 1, s_13);
            if (ret < 0) return ret;
        }
        break;
    lab0:
        z->c = z->l - v_1;
        if (!(eq_s_b(z, 2, s_14))) goto lab1;
        if (z->c > z->lb) goto lab1;
        {
            int ret = slice_from_s(z, 1, s_15);
            if (ret < 0) return ret;
        }
        break;
    lab1:
        z->c = z->l - v_1;
        if (z->c - 2 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((68438676 >> (z->p[z->c - 1] & 0x1f)) & 1)) goto lab2;
        if (!find_among_b(z, a_3, 20, 0)) goto lab2;
        do {
            int v_2 = z->l - z->c;
            if (z->c > z->lb) goto lab3;
            break;
        lab3:
            z->c = z->l - v_2;
            if (z->c <= z->lb || z->p[z->c - 1] != '-') goto lab2;
            z->c--;
        } while (0);
        {
            int ret = slice_from_s(z, 3, s_16);
            if (ret < 0) return ret;
        }
        break;
    lab2:
        z->c = z->l - v_1;
        {
            int ret = slice_from_s(z, 1, s_17);
            if (ret < 0) return ret;
        }
    } while (0);
    return 1;
}

static int r_ujn_suffix(struct SN_env * z) {
    z->ket = z->c;
    {
        int v_1 = z->l - z->c;
        if (z->c <= z->lb || z->p[z->c - 1] != 'n') { z->c = z->l - v_1; goto lab0; }
        z->c--;
    lab0:
        ;
    }
    {
        int v_2 = z->l - z->c;
        if (z->c <= z->lb || z->p[z->c - 1] != 'j') { z->c = z->l - v_2; goto lab1; }
        z->c--;
    lab1:
        ;
    }
    z->bra = z->c;
    if (z->c - 2 <= z->lb || z->p[z->c - 1] != 117) return 0;
    if (!find_among_b(z, a_4, 2, 0)) return 0;
    do {
        int v_3 = z->l - z->c;
        if (z->c > z->lb) goto lab2;
        break;
    lab2:
        z->c = z->l - v_3;
        if (z->c <= z->lb || z->p[z->c - 1] != '-') return 0;
        z->c--;
    } while (0);
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_uninflected(struct SN_env * z) {
    if (z->c - 2 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((2672162 >> (z->p[z->c - 1] & 0x1f)) & 1)) return 0;
    if (!find_among_b(z, a_5, 24, 0)) return 0;
    do {
        int v_1 = z->l - z->c;
        if (z->c > z->lb) goto lab0;
        break;
    lab0:
        z->c = z->l - v_1;
        if (z->c <= z->lb || z->p[z->c - 1] != '-') return 0;
        z->c--;
    } while (0);
    return 1;
}

static int r_merged_numeral(struct SN_env * z) {
    if (z->c - 1 <= z->lb || (z->p[z->c - 1] != 105 && z->p[z->c - 1] != 117)) return 0;
    if (!find_among_b(z, a_6, 3, 0)) return 0;
    if (z->c - 2 <= z->lb || (z->p[z->c - 1] != 107 && z->p[z->c - 1] != 116)) return 0;
    return find_among_b(z, a_7, 2, 0) != 0;
}

static int r_correlative(struct SN_env * z) {
    z->ket = z->c;
    z->bra = z->c;
    {
        int v_1 = z->l - z->c;
        do {
            int v_2 = z->l - z->c;
            {
                int v_3 = z->l - z->c;
                if (z->c <= z->lb || z->p[z->c - 1] != 'n') { z->c = z->l - v_3; goto lab1; }
                z->c--;
            lab1:
                ;
            }
            z->bra = z->c;
            if (z->c <= z->lb || z->p[z->c - 1] != 'e') goto lab0;
            z->c--;
            break;
        lab0:
            z->c = z->l - v_2;
            {
                int v_4 = z->l - z->c;
                if (z->c <= z->lb || z->p[z->c - 1] != 'n') { z->c = z->l - v_4; goto lab2; }
                z->c--;
            lab2:
                ;
            }
            {
                int v_5 = z->l - z->c;
                if (z->c <= z->lb || z->p[z->c - 1] != 'j') { z->c = z->l - v_5; goto lab3; }
                z->c--;
            lab3:
                ;
            }
            z->bra = z->c;
            if (in_grouping_b_U(z, g_aou, 97, 117, 0)) return 0;
        } while (0);
        if (z->c <= z->lb || z->p[z->c - 1] != 'i') return 0;
        z->c--;
        {
            int v_6 = z->l - z->c;
            if (!find_among_b(z, a_8, 7, 0)) { z->c = z->l - v_6; goto lab4; }
        lab4:
            ;
        }
        do {
            int v_7 = z->l - z->c;
            if (z->c > z->lb) goto lab5;
            break;
        lab5:
            z->c = z->l - v_7;
            if (z->c <= z->lb || z->p[z->c - 1] != '-') return 0;
            z->c--;
        } while (0);
        z->c = z->l - v_1;
    }
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_long_word(struct SN_env * z) {
    do {
        int v_1 = z->l - z->c;
        {
            int i; for (i = 2; i > 0; i--) {
                {
                    int ret = out_grouping_b_U(z, g_vowel, 97, 117, 1);
                    if (ret < 0) goto lab0;
                    z->c -= ret;
                }
            }
        }
        break;
    lab0:
        z->c = z->l - v_1;
        while (1) {
            if (z->c <= z->lb || z->p[z->c - 1] != '-') goto lab2;
            z->c--;
            break;
        lab2:
            {
                int ret = skip_b_utf8(z->p, z->c, z->lb, 1);
                if (ret < 0) goto lab1;
                z->c = ret;
            }
        }
        {
            int ret = skip_b_utf8(z->p, z->c, z->lb, 1);
            if (ret < 0) goto lab1;
            z->c = ret;
        }
        break;
    lab1:
        z->c = z->l - v_1;
        {
            int ret = out_grouping_b_U(z, g_digit, 48, 57, 1);
            if (ret < 0) return 0;
            z->c -= ret;
        }
    } while (0);
    return 1;
}

static int r_standard_suffix(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    if (z->c <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((2672162 >> (z->p[z->c - 1] & 0x1f)) & 1)) return 0;
    among_var = find_among_b(z, a_9, 19, 0);
    if (!among_var) return 0;
    switch (among_var) {
        case 1:
            {
                int v_1 = z->l - z->c;
                do {
                    int v_2 = z->l - z->c;
                    if (z->c <= z->lb || z->p[z->c - 1] != '-') goto lab0;
                    z->c--;
                    break;
                lab0:
                    z->c = z->l - v_2;
                    if (in_grouping_b_U(z, g_digit, 48, 57, 0)) return 0;
                } while (0);
                z->c = z->l - v_1;
            }
            break;
    }
    {
        int v_3 = z->l - z->c;
        if (z->c <= z->lb || z->p[z->c - 1] != '-') { z->c = z->l - v_3; goto lab1; }
        z->c--;
    lab1:
        ;
    }
    z->bra = z->c;
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return 1;
}

extern int esperanto_UTF_8_stem(struct SN_env * z) {
    {
        int v_1 = z->c;
        {
            int ret = r_canonical_form(z);
            if (ret <= 0) return ret;
        }
        z->c = v_1;
    }
    {
        int v_2 = z->c;
        {
            int ret = r_initial_apostrophe(z);
            if (ret < 0) return ret;
        }
        z->c = v_2;
    }
    z->lb = z->c; z->c = z->l;
    {
        int v_3 = z->l - z->c;
        {
            int ret = r_pronoun(z);
            if (ret == 0) goto lab0;
            if (ret < 0) return ret;
        }
        return 0;
    lab0:
        z->c = z->l - v_3;
    }
    {
        int v_4 = z->l - z->c;
        {
            int ret = r_final_apostrophe(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_4;
    }
    {
        int v_5 = z->l - z->c;
        {
            int ret = r_correlative(z);
            if (ret == 0) goto lab1;
            if (ret < 0) return ret;
        }
        return 0;
    lab1:
        z->c = z->l - v_5;
    }
    {
        int v_6 = z->l - z->c;
        {
            int ret = r_uninflected(z);
            if (ret == 0) goto lab2;
            if (ret < 0) return ret;
        }
        return 0;
    lab2:
        z->c = z->l - v_6;
    }
    {
        int v_7 = z->l - z->c;
        {
            int ret = r_merged_numeral(z);
            if (ret == 0) goto lab3;
            if (ret < 0) return ret;
        }
        return 0;
    lab3:
        z->c = z->l - v_7;
    }
    {
        int v_8 = z->l - z->c;
        {
            int ret = r_ujn_suffix(z);
            if (ret == 0) goto lab4;
            if (ret < 0) return ret;
        }
        return 0;
    lab4:
        z->c = z->l - v_8;
    }
    {
        int v_9 = z->l - z->c;
        {
            int ret = r_long_word(z);
            if (ret <= 0) return ret;
        }
        z->c = z->l - v_9;
    }
    {
        int ret = r_standard_suffix(z);
        if (ret <= 0) return ret;
    }
    z->c = z->lb;
    return 1;
}

extern struct SN_env * esperanto_UTF_8_create_env(void) {
    return SN_new_env(sizeof(struct SN_env));
}

extern void esperanto_UTF_8_close_env(struct SN_env * z) {
    SN_delete_env(z);
}

