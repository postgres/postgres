/* Generated from turkish.sbl by Snowball 3.0.0 - https://snowballstem.org/ */

#include "stem_UTF_8_turkish.h"

#include <stddef.h>

#include "snowball_runtime.h"

struct SN_local {
    struct SN_env z;
    unsigned char b_continue_stemming_noun_suffixes;
};

typedef struct SN_local SN_local;

#ifdef __cplusplus
extern "C" {
#endif
extern int turkish_UTF_8_stem(struct SN_env * z);
#ifdef __cplusplus
}
#endif

static int r_stem_suffix_chain_before_ki(struct SN_env * z);
static int r_stem_noun_suffixes(struct SN_env * z);
static int r_stem_nominal_verb_suffixes(struct SN_env * z);
static int r_remove_proper_noun_suffix(struct SN_env * z);
static int r_postlude(struct SN_env * z);
static int r_post_process_last_consonants(struct SN_env * z);
static int r_more_than_one_syllable_word(struct SN_env * z);
static int r_mark_suffix_with_optional_s_consonant(struct SN_env * z);
static int r_mark_suffix_with_optional_n_consonant(struct SN_env * z);
static int r_mark_suffix_with_optional_U_vowel(struct SN_env * z);
static int r_mark_suffix_with_optional_y_consonant(struct SN_env * z);
static int r_mark_ysA(struct SN_env * z);
static int r_mark_ymUs_(struct SN_env * z);
static int r_mark_yken(struct SN_env * z);
static int r_mark_yDU(struct SN_env * z);
static int r_mark_yUz(struct SN_env * z);
static int r_mark_yUm(struct SN_env * z);
static int r_mark_yU(struct SN_env * z);
static int r_mark_ylA(struct SN_env * z);
static int r_mark_yA(struct SN_env * z);
static int r_mark_possessives(struct SN_env * z);
static int r_mark_sUnUz(struct SN_env * z);
static int r_mark_sUn(struct SN_env * z);
static int r_mark_sU(struct SN_env * z);
static int r_mark_nUz(struct SN_env * z);
static int r_mark_nUn(struct SN_env * z);
static int r_mark_nU(struct SN_env * z);
static int r_mark_ndAn(struct SN_env * z);
static int r_mark_ndA(struct SN_env * z);
static int r_mark_ncA(struct SN_env * z);
static int r_mark_nA(struct SN_env * z);
static int r_mark_lArI(struct SN_env * z);
static int r_mark_lAr(struct SN_env * z);
static int r_mark_ki(struct SN_env * z);
static int r_mark_DUr(struct SN_env * z);
static int r_mark_DAn(struct SN_env * z);
static int r_mark_DA(struct SN_env * z);
static int r_mark_cAsInA(struct SN_env * z);
static int r_is_reserved_word(struct SN_env * z);
static int r_check_vowel_harmony(struct SN_env * z);
static int r_append_U_to_stems_ending_with_d_or_g(struct SN_env * z);

static const symbol s_0[] = { 0xC4, 0xB1 };
static const symbol s_1[] = { 0xC3, 0xB6 };
static const symbol s_2[] = { 0xC3, 0xBC };
static const symbol s_3[] = { 'k', 'i' };
static const symbol s_4[] = { 'k', 'e', 'n' };
static const symbol s_5[] = { 'p' };
static const symbol s_6[] = { 0xC3, 0xA7 };
static const symbol s_7[] = { 't' };
static const symbol s_8[] = { 'k' };
static const symbol s_9[] = { 0xC4, 0xB1 };
static const symbol s_10[] = { 0xC4, 0xB1 };
static const symbol s_11[] = { 'i' };
static const symbol s_12[] = { 'u' };
static const symbol s_13[] = { 0xC3, 0xB6 };
static const symbol s_14[] = { 0xC3, 0xBC };
static const symbol s_15[] = { 0xC3, 0xBC };
static const symbol s_16[] = { 'a', 'd' };
static const symbol s_17[] = { 's', 'o', 'y' };

static const symbol s_0_0[1] = { 'm' };
static const symbol s_0_1[1] = { 'n' };
static const symbol s_0_2[3] = { 'm', 'i', 'z' };
static const symbol s_0_3[3] = { 'n', 'i', 'z' };
static const symbol s_0_4[3] = { 'm', 'u', 'z' };
static const symbol s_0_5[3] = { 'n', 'u', 'z' };
static const symbol s_0_6[4] = { 'm', 0xC4, 0xB1, 'z' };
static const symbol s_0_7[4] = { 'n', 0xC4, 0xB1, 'z' };
static const symbol s_0_8[4] = { 'm', 0xC3, 0xBC, 'z' };
static const symbol s_0_9[4] = { 'n', 0xC3, 0xBC, 'z' };
static const struct among a_0[10] = {
{ 1, s_0_0, 0, -1, 0},
{ 1, s_0_1, 0, -1, 0},
{ 3, s_0_2, 0, -1, 0},
{ 3, s_0_3, 0, -1, 0},
{ 3, s_0_4, 0, -1, 0},
{ 3, s_0_5, 0, -1, 0},
{ 4, s_0_6, 0, -1, 0},
{ 4, s_0_7, 0, -1, 0},
{ 4, s_0_8, 0, -1, 0},
{ 4, s_0_9, 0, -1, 0}
};

static const symbol s_1_0[4] = { 'l', 'e', 'r', 'i' };
static const symbol s_1_1[5] = { 'l', 'a', 'r', 0xC4, 0xB1 };
static const struct among a_1[2] = {
{ 4, s_1_0, 0, -1, 0},
{ 5, s_1_1, 0, -1, 0}
};

static const symbol s_2_0[2] = { 'n', 'i' };
static const symbol s_2_1[2] = { 'n', 'u' };
static const symbol s_2_2[3] = { 'n', 0xC4, 0xB1 };
static const symbol s_2_3[3] = { 'n', 0xC3, 0xBC };
static const struct among a_2[4] = {
{ 2, s_2_0, 0, -1, 0},
{ 2, s_2_1, 0, -1, 0},
{ 3, s_2_2, 0, -1, 0},
{ 3, s_2_3, 0, -1, 0}
};

static const symbol s_3_0[2] = { 'i', 'n' };
static const symbol s_3_1[2] = { 'u', 'n' };
static const symbol s_3_2[3] = { 0xC4, 0xB1, 'n' };
static const symbol s_3_3[3] = { 0xC3, 0xBC, 'n' };
static const struct among a_3[4] = {
{ 2, s_3_0, 0, -1, 0},
{ 2, s_3_1, 0, -1, 0},
{ 3, s_3_2, 0, -1, 0},
{ 3, s_3_3, 0, -1, 0}
};

static const symbol s_5_0[2] = { 'n', 'a' };
static const symbol s_5_1[2] = { 'n', 'e' };
static const struct among a_5[2] = {
{ 2, s_5_0, 0, -1, 0},
{ 2, s_5_1, 0, -1, 0}
};

static const symbol s_6_0[2] = { 'd', 'a' };
static const symbol s_6_1[2] = { 't', 'a' };
static const symbol s_6_2[2] = { 'd', 'e' };
static const symbol s_6_3[2] = { 't', 'e' };
static const struct among a_6[4] = {
{ 2, s_6_0, 0, -1, 0},
{ 2, s_6_1, 0, -1, 0},
{ 2, s_6_2, 0, -1, 0},
{ 2, s_6_3, 0, -1, 0}
};

static const symbol s_7_0[3] = { 'n', 'd', 'a' };
static const symbol s_7_1[3] = { 'n', 'd', 'e' };
static const struct among a_7[2] = {
{ 3, s_7_0, 0, -1, 0},
{ 3, s_7_1, 0, -1, 0}
};

static const symbol s_8_0[3] = { 'd', 'a', 'n' };
static const symbol s_8_1[3] = { 't', 'a', 'n' };
static const symbol s_8_2[3] = { 'd', 'e', 'n' };
static const symbol s_8_3[3] = { 't', 'e', 'n' };
static const struct among a_8[4] = {
{ 3, s_8_0, 0, -1, 0},
{ 3, s_8_1, 0, -1, 0},
{ 3, s_8_2, 0, -1, 0},
{ 3, s_8_3, 0, -1, 0}
};

static const symbol s_9_0[4] = { 'n', 'd', 'a', 'n' };
static const symbol s_9_1[4] = { 'n', 'd', 'e', 'n' };
static const struct among a_9[2] = {
{ 4, s_9_0, 0, -1, 0},
{ 4, s_9_1, 0, -1, 0}
};

static const symbol s_10_0[2] = { 'l', 'a' };
static const symbol s_10_1[2] = { 'l', 'e' };
static const struct among a_10[2] = {
{ 2, s_10_0, 0, -1, 0},
{ 2, s_10_1, 0, -1, 0}
};

static const symbol s_11_0[2] = { 'c', 'a' };
static const symbol s_11_1[2] = { 'c', 'e' };
static const struct among a_11[2] = {
{ 2, s_11_0, 0, -1, 0},
{ 2, s_11_1, 0, -1, 0}
};

static const symbol s_12_0[2] = { 'i', 'm' };
static const symbol s_12_1[2] = { 'u', 'm' };
static const symbol s_12_2[3] = { 0xC4, 0xB1, 'm' };
static const symbol s_12_3[3] = { 0xC3, 0xBC, 'm' };
static const struct among a_12[4] = {
{ 2, s_12_0, 0, -1, 0},
{ 2, s_12_1, 0, -1, 0},
{ 3, s_12_2, 0, -1, 0},
{ 3, s_12_3, 0, -1, 0}
};

static const symbol s_13_0[3] = { 's', 'i', 'n' };
static const symbol s_13_1[3] = { 's', 'u', 'n' };
static const symbol s_13_2[4] = { 's', 0xC4, 0xB1, 'n' };
static const symbol s_13_3[4] = { 's', 0xC3, 0xBC, 'n' };
static const struct among a_13[4] = {
{ 3, s_13_0, 0, -1, 0},
{ 3, s_13_1, 0, -1, 0},
{ 4, s_13_2, 0, -1, 0},
{ 4, s_13_3, 0, -1, 0}
};

static const symbol s_14_0[2] = { 'i', 'z' };
static const symbol s_14_1[2] = { 'u', 'z' };
static const symbol s_14_2[3] = { 0xC4, 0xB1, 'z' };
static const symbol s_14_3[3] = { 0xC3, 0xBC, 'z' };
static const struct among a_14[4] = {
{ 2, s_14_0, 0, -1, 0},
{ 2, s_14_1, 0, -1, 0},
{ 3, s_14_2, 0, -1, 0},
{ 3, s_14_3, 0, -1, 0}
};

static const symbol s_15_0[5] = { 's', 'i', 'n', 'i', 'z' };
static const symbol s_15_1[5] = { 's', 'u', 'n', 'u', 'z' };
static const symbol s_15_2[7] = { 's', 0xC4, 0xB1, 'n', 0xC4, 0xB1, 'z' };
static const symbol s_15_3[7] = { 's', 0xC3, 0xBC, 'n', 0xC3, 0xBC, 'z' };
static const struct among a_15[4] = {
{ 5, s_15_0, 0, -1, 0},
{ 5, s_15_1, 0, -1, 0},
{ 7, s_15_2, 0, -1, 0},
{ 7, s_15_3, 0, -1, 0}
};

static const symbol s_16_0[3] = { 'l', 'a', 'r' };
static const symbol s_16_1[3] = { 'l', 'e', 'r' };
static const struct among a_16[2] = {
{ 3, s_16_0, 0, -1, 0},
{ 3, s_16_1, 0, -1, 0}
};

static const symbol s_17_0[3] = { 'n', 'i', 'z' };
static const symbol s_17_1[3] = { 'n', 'u', 'z' };
static const symbol s_17_2[4] = { 'n', 0xC4, 0xB1, 'z' };
static const symbol s_17_3[4] = { 'n', 0xC3, 0xBC, 'z' };
static const struct among a_17[4] = {
{ 3, s_17_0, 0, -1, 0},
{ 3, s_17_1, 0, -1, 0},
{ 4, s_17_2, 0, -1, 0},
{ 4, s_17_3, 0, -1, 0}
};

static const symbol s_18_0[3] = { 'd', 'i', 'r' };
static const symbol s_18_1[3] = { 't', 'i', 'r' };
static const symbol s_18_2[3] = { 'd', 'u', 'r' };
static const symbol s_18_3[3] = { 't', 'u', 'r' };
static const symbol s_18_4[4] = { 'd', 0xC4, 0xB1, 'r' };
static const symbol s_18_5[4] = { 't', 0xC4, 0xB1, 'r' };
static const symbol s_18_6[4] = { 'd', 0xC3, 0xBC, 'r' };
static const symbol s_18_7[4] = { 't', 0xC3, 0xBC, 'r' };
static const struct among a_18[8] = {
{ 3, s_18_0, 0, -1, 0},
{ 3, s_18_1, 0, -1, 0},
{ 3, s_18_2, 0, -1, 0},
{ 3, s_18_3, 0, -1, 0},
{ 4, s_18_4, 0, -1, 0},
{ 4, s_18_5, 0, -1, 0},
{ 4, s_18_6, 0, -1, 0},
{ 4, s_18_7, 0, -1, 0}
};

static const symbol s_19_0[7] = { 'c', 'a', 's', 0xC4, 0xB1, 'n', 'a' };
static const symbol s_19_1[6] = { 'c', 'e', 's', 'i', 'n', 'e' };
static const struct among a_19[2] = {
{ 7, s_19_0, 0, -1, 0},
{ 6, s_19_1, 0, -1, 0}
};

static const symbol s_20_0[2] = { 'd', 'i' };
static const symbol s_20_1[2] = { 't', 'i' };
static const symbol s_20_2[3] = { 'd', 'i', 'k' };
static const symbol s_20_3[3] = { 't', 'i', 'k' };
static const symbol s_20_4[3] = { 'd', 'u', 'k' };
static const symbol s_20_5[3] = { 't', 'u', 'k' };
static const symbol s_20_6[4] = { 'd', 0xC4, 0xB1, 'k' };
static const symbol s_20_7[4] = { 't', 0xC4, 0xB1, 'k' };
static const symbol s_20_8[4] = { 'd', 0xC3, 0xBC, 'k' };
static const symbol s_20_9[4] = { 't', 0xC3, 0xBC, 'k' };
static const symbol s_20_10[3] = { 'd', 'i', 'm' };
static const symbol s_20_11[3] = { 't', 'i', 'm' };
static const symbol s_20_12[3] = { 'd', 'u', 'm' };
static const symbol s_20_13[3] = { 't', 'u', 'm' };
static const symbol s_20_14[4] = { 'd', 0xC4, 0xB1, 'm' };
static const symbol s_20_15[4] = { 't', 0xC4, 0xB1, 'm' };
static const symbol s_20_16[4] = { 'd', 0xC3, 0xBC, 'm' };
static const symbol s_20_17[4] = { 't', 0xC3, 0xBC, 'm' };
static const symbol s_20_18[3] = { 'd', 'i', 'n' };
static const symbol s_20_19[3] = { 't', 'i', 'n' };
static const symbol s_20_20[3] = { 'd', 'u', 'n' };
static const symbol s_20_21[3] = { 't', 'u', 'n' };
static const symbol s_20_22[4] = { 'd', 0xC4, 0xB1, 'n' };
static const symbol s_20_23[4] = { 't', 0xC4, 0xB1, 'n' };
static const symbol s_20_24[4] = { 'd', 0xC3, 0xBC, 'n' };
static const symbol s_20_25[4] = { 't', 0xC3, 0xBC, 'n' };
static const symbol s_20_26[2] = { 'd', 'u' };
static const symbol s_20_27[2] = { 't', 'u' };
static const symbol s_20_28[3] = { 'd', 0xC4, 0xB1 };
static const symbol s_20_29[3] = { 't', 0xC4, 0xB1 };
static const symbol s_20_30[3] = { 'd', 0xC3, 0xBC };
static const symbol s_20_31[3] = { 't', 0xC3, 0xBC };
static const struct among a_20[32] = {
{ 2, s_20_0, 0, -1, 0},
{ 2, s_20_1, 0, -1, 0},
{ 3, s_20_2, 0, -1, 0},
{ 3, s_20_3, 0, -1, 0},
{ 3, s_20_4, 0, -1, 0},
{ 3, s_20_5, 0, -1, 0},
{ 4, s_20_6, 0, -1, 0},
{ 4, s_20_7, 0, -1, 0},
{ 4, s_20_8, 0, -1, 0},
{ 4, s_20_9, 0, -1, 0},
{ 3, s_20_10, 0, -1, 0},
{ 3, s_20_11, 0, -1, 0},
{ 3, s_20_12, 0, -1, 0},
{ 3, s_20_13, 0, -1, 0},
{ 4, s_20_14, 0, -1, 0},
{ 4, s_20_15, 0, -1, 0},
{ 4, s_20_16, 0, -1, 0},
{ 4, s_20_17, 0, -1, 0},
{ 3, s_20_18, 0, -1, 0},
{ 3, s_20_19, 0, -1, 0},
{ 3, s_20_20, 0, -1, 0},
{ 3, s_20_21, 0, -1, 0},
{ 4, s_20_22, 0, -1, 0},
{ 4, s_20_23, 0, -1, 0},
{ 4, s_20_24, 0, -1, 0},
{ 4, s_20_25, 0, -1, 0},
{ 2, s_20_26, 0, -1, 0},
{ 2, s_20_27, 0, -1, 0},
{ 3, s_20_28, 0, -1, 0},
{ 3, s_20_29, 0, -1, 0},
{ 3, s_20_30, 0, -1, 0},
{ 3, s_20_31, 0, -1, 0}
};

static const symbol s_21_0[2] = { 's', 'a' };
static const symbol s_21_1[2] = { 's', 'e' };
static const symbol s_21_2[3] = { 's', 'a', 'k' };
static const symbol s_21_3[3] = { 's', 'e', 'k' };
static const symbol s_21_4[3] = { 's', 'a', 'm' };
static const symbol s_21_5[3] = { 's', 'e', 'm' };
static const symbol s_21_6[3] = { 's', 'a', 'n' };
static const symbol s_21_7[3] = { 's', 'e', 'n' };
static const struct among a_21[8] = {
{ 2, s_21_0, 0, -1, 0},
{ 2, s_21_1, 0, -1, 0},
{ 3, s_21_2, 0, -1, 0},
{ 3, s_21_3, 0, -1, 0},
{ 3, s_21_4, 0, -1, 0},
{ 3, s_21_5, 0, -1, 0},
{ 3, s_21_6, 0, -1, 0},
{ 3, s_21_7, 0, -1, 0}
};

static const symbol s_22_0[4] = { 'm', 'i', 0xC5, 0x9F };
static const symbol s_22_1[4] = { 'm', 'u', 0xC5, 0x9F };
static const symbol s_22_2[5] = { 'm', 0xC4, 0xB1, 0xC5, 0x9F };
static const symbol s_22_3[5] = { 'm', 0xC3, 0xBC, 0xC5, 0x9F };
static const struct among a_22[4] = {
{ 4, s_22_0, 0, -1, 0},
{ 4, s_22_1, 0, -1, 0},
{ 5, s_22_2, 0, -1, 0},
{ 5, s_22_3, 0, -1, 0}
};

static const symbol s_23_0[1] = { 'b' };
static const symbol s_23_1[1] = { 'c' };
static const symbol s_23_2[1] = { 'd' };
static const symbol s_23_3[2] = { 0xC4, 0x9F };
static const struct among a_23[4] = {
{ 1, s_23_0, 0, 1, 0},
{ 1, s_23_1, 0, 2, 0},
{ 1, s_23_2, 0, 3, 0},
{ 2, s_23_3, 0, 4, 0}
};

static const unsigned char g_vowel[] = { 17, 65, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 8, 0, 0, 0, 0, 0, 0, 1 };

static const unsigned char g_U[] = { 1, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 1 };

static const unsigned char g_vowel1[] = { 1, 64, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };

static const unsigned char g_vowel2[] = { 17, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 130 };

static const unsigned char g_vowel3[] = { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };

static const unsigned char g_vowel4[] = { 17 };

static const unsigned char g_vowel5[] = { 65 };

static const unsigned char g_vowel6[] = { 65 };

static int r_check_vowel_harmony(struct SN_env * z) {
    {
        int v_1 = z->l - z->c;
        if (out_grouping_b_U(z, g_vowel, 97, 305, 1) < 0) return 0;
        do {
            int v_2 = z->l - z->c;
            if (z->c <= z->lb || z->p[z->c - 1] != 'a') goto lab0;
            z->c--;
            if (out_grouping_b_U(z, g_vowel1, 97, 305, 1) < 0) goto lab0;
            break;
        lab0:
            z->c = z->l - v_2;
            if (z->c <= z->lb || z->p[z->c - 1] != 'e') goto lab1;
            z->c--;
            if (out_grouping_b_U(z, g_vowel2, 101, 252, 1) < 0) goto lab1;
            break;
        lab1:
            z->c = z->l - v_2;
            if (!(eq_s_b(z, 2, s_0))) goto lab2;
            if (out_grouping_b_U(z, g_vowel3, 97, 305, 1) < 0) goto lab2;
            break;
        lab2:
            z->c = z->l - v_2;
            if (z->c <= z->lb || z->p[z->c - 1] != 'i') goto lab3;
            z->c--;
            if (out_grouping_b_U(z, g_vowel4, 101, 105, 1) < 0) goto lab3;
            break;
        lab3:
            z->c = z->l - v_2;
            if (z->c <= z->lb || z->p[z->c - 1] != 'o') goto lab4;
            z->c--;
            if (out_grouping_b_U(z, g_vowel5, 111, 117, 1) < 0) goto lab4;
            break;
        lab4:
            z->c = z->l - v_2;
            if (!(eq_s_b(z, 2, s_1))) goto lab5;
            if (out_grouping_b_U(z, g_vowel6, 246, 252, 1) < 0) goto lab5;
            break;
        lab5:
            z->c = z->l - v_2;
            if (z->c <= z->lb || z->p[z->c - 1] != 'u') goto lab6;
            z->c--;
            if (out_grouping_b_U(z, g_vowel5, 111, 117, 1) < 0) goto lab6;
            break;
        lab6:
            z->c = z->l - v_2;
            if (!(eq_s_b(z, 2, s_2))) return 0;
            if (out_grouping_b_U(z, g_vowel6, 246, 252, 1) < 0) return 0;
        } while (0);
        z->c = z->l - v_1;
    }
    return 1;
}

static int r_mark_suffix_with_optional_n_consonant(struct SN_env * z) {
    do {
        int v_1 = z->l - z->c;
        if (z->c <= z->lb || z->p[z->c - 1] != 'n') goto lab0;
        z->c--;
        {
            int v_2 = z->l - z->c;
            if (in_grouping_b_U(z, g_vowel, 97, 305, 0)) goto lab0;
            z->c = z->l - v_2;
        }
        break;
    lab0:
        z->c = z->l - v_1;
        {
            int v_3 = z->l - z->c;
            if (z->c <= z->lb || z->p[z->c - 1] != 'n') goto lab1;
            z->c--;
            return 0;
        lab1:
            z->c = z->l - v_3;
        }
        {
            int v_4 = z->l - z->c;
            {
                int ret = skip_b_utf8(z->p, z->c, z->lb, 1);
                if (ret < 0) return 0;
                z->c = ret;
            }
            if (in_grouping_b_U(z, g_vowel, 97, 305, 0)) return 0;
            z->c = z->l - v_4;
        }
    } while (0);
    return 1;
}

static int r_mark_suffix_with_optional_s_consonant(struct SN_env * z) {
    do {
        int v_1 = z->l - z->c;
        if (z->c <= z->lb || z->p[z->c - 1] != 's') goto lab0;
        z->c--;
        {
            int v_2 = z->l - z->c;
            if (in_grouping_b_U(z, g_vowel, 97, 305, 0)) goto lab0;
            z->c = z->l - v_2;
        }
        break;
    lab0:
        z->c = z->l - v_1;
        {
            int v_3 = z->l - z->c;
            if (z->c <= z->lb || z->p[z->c - 1] != 's') goto lab1;
            z->c--;
            return 0;
        lab1:
            z->c = z->l - v_3;
        }
        {
            int v_4 = z->l - z->c;
            {
                int ret = skip_b_utf8(z->p, z->c, z->lb, 1);
                if (ret < 0) return 0;
                z->c = ret;
            }
            if (in_grouping_b_U(z, g_vowel, 97, 305, 0)) return 0;
            z->c = z->l - v_4;
        }
    } while (0);
    return 1;
}

static int r_mark_suffix_with_optional_y_consonant(struct SN_env * z) {
    do {
        int v_1 = z->l - z->c;
        if (z->c <= z->lb || z->p[z->c - 1] != 'y') goto lab0;
        z->c--;
        {
            int v_2 = z->l - z->c;
            if (in_grouping_b_U(z, g_vowel, 97, 305, 0)) goto lab0;
            z->c = z->l - v_2;
        }
        break;
    lab0:
        z->c = z->l - v_1;
        {
            int v_3 = z->l - z->c;
            if (z->c <= z->lb || z->p[z->c - 1] != 'y') goto lab1;
            z->c--;
            return 0;
        lab1:
            z->c = z->l - v_3;
        }
        {
            int v_4 = z->l - z->c;
            {
                int ret = skip_b_utf8(z->p, z->c, z->lb, 1);
                if (ret < 0) return 0;
                z->c = ret;
            }
            if (in_grouping_b_U(z, g_vowel, 97, 305, 0)) return 0;
            z->c = z->l - v_4;
        }
    } while (0);
    return 1;
}

static int r_mark_suffix_with_optional_U_vowel(struct SN_env * z) {
    do {
        int v_1 = z->l - z->c;
        if (in_grouping_b_U(z, g_U, 105, 305, 0)) goto lab0;
        {
            int v_2 = z->l - z->c;
            if (out_grouping_b_U(z, g_vowel, 97, 305, 0)) goto lab0;
            z->c = z->l - v_2;
        }
        break;
    lab0:
        z->c = z->l - v_1;
        {
            int v_3 = z->l - z->c;
            if (in_grouping_b_U(z, g_U, 105, 305, 0)) goto lab1;
            return 0;
        lab1:
            z->c = z->l - v_3;
        }
        {
            int v_4 = z->l - z->c;
            {
                int ret = skip_b_utf8(z->p, z->c, z->lb, 1);
                if (ret < 0) return 0;
                z->c = ret;
            }
            if (out_grouping_b_U(z, g_vowel, 97, 305, 0)) return 0;
            z->c = z->l - v_4;
        }
    } while (0);
    return 1;
}

static int r_mark_possessives(struct SN_env * z) {
    if (z->c <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((67133440 >> (z->p[z->c - 1] & 0x1f)) & 1)) return 0;
    if (!find_among_b(z, a_0, 10, 0)) return 0;
    return r_mark_suffix_with_optional_U_vowel(z);
}

static int r_mark_sU(struct SN_env * z) {
    {
        int ret = r_check_vowel_harmony(z);
        if (ret <= 0) return ret;
    }
    if (in_grouping_b_U(z, g_U, 105, 305, 0)) return 0;
    return r_mark_suffix_with_optional_s_consonant(z);
}

static int r_mark_lArI(struct SN_env * z) {
    if (z->c - 3 <= z->lb || (z->p[z->c - 1] != 105 && z->p[z->c - 1] != 177)) return 0;
    return find_among_b(z, a_1, 2, 0) != 0;
}

static int r_mark_yU(struct SN_env * z) {
    {
        int ret = r_check_vowel_harmony(z);
        if (ret <= 0) return ret;
    }
    if (in_grouping_b_U(z, g_U, 105, 305, 0)) return 0;
    return r_mark_suffix_with_optional_y_consonant(z);
}

static int r_mark_nU(struct SN_env * z) {
    {
        int ret = r_check_vowel_harmony(z);
        if (ret <= 0) return ret;
    }
    return find_among_b(z, a_2, 4, 0) != 0;
}

static int r_mark_nUn(struct SN_env * z) {
    {
        int ret = r_check_vowel_harmony(z);
        if (ret <= 0) return ret;
    }
    if (z->c - 1 <= z->lb || z->p[z->c - 1] != 110) return 0;
    if (!find_among_b(z, a_3, 4, 0)) return 0;
    return r_mark_suffix_with_optional_n_consonant(z);
}

static int r_mark_yA(struct SN_env * z) {
    {
        int ret = r_check_vowel_harmony(z);
        if (ret <= 0) return ret;
    }
    if (z->c <= z->lb || (z->p[z->c - 1] != 97 && z->p[z->c - 1] != 101)) return 0;
    z->c--;
    return r_mark_suffix_with_optional_y_consonant(z);
}

static int r_mark_nA(struct SN_env * z) {
    {
        int ret = r_check_vowel_harmony(z);
        if (ret <= 0) return ret;
    }
    if (z->c - 1 <= z->lb || (z->p[z->c - 1] != 97 && z->p[z->c - 1] != 101)) return 0;
    return find_among_b(z, a_5, 2, 0) != 0;
}

static int r_mark_DA(struct SN_env * z) {
    {
        int ret = r_check_vowel_harmony(z);
        if (ret <= 0) return ret;
    }
    if (z->c - 1 <= z->lb || (z->p[z->c - 1] != 97 && z->p[z->c - 1] != 101)) return 0;
    return find_among_b(z, a_6, 4, 0) != 0;
}

static int r_mark_ndA(struct SN_env * z) {
    {
        int ret = r_check_vowel_harmony(z);
        if (ret <= 0) return ret;
    }
    if (z->c - 2 <= z->lb || (z->p[z->c - 1] != 97 && z->p[z->c - 1] != 101)) return 0;
    return find_among_b(z, a_7, 2, 0) != 0;
}

static int r_mark_DAn(struct SN_env * z) {
    {
        int ret = r_check_vowel_harmony(z);
        if (ret <= 0) return ret;
    }
    if (z->c - 2 <= z->lb || z->p[z->c - 1] != 110) return 0;
    return find_among_b(z, a_8, 4, 0) != 0;
}

static int r_mark_ndAn(struct SN_env * z) {
    {
        int ret = r_check_vowel_harmony(z);
        if (ret <= 0) return ret;
    }
    if (z->c - 3 <= z->lb || z->p[z->c - 1] != 110) return 0;
    return find_among_b(z, a_9, 2, 0) != 0;
}

static int r_mark_ylA(struct SN_env * z) {
    {
        int ret = r_check_vowel_harmony(z);
        if (ret <= 0) return ret;
    }
    if (z->c - 1 <= z->lb || (z->p[z->c - 1] != 97 && z->p[z->c - 1] != 101)) return 0;
    if (!find_among_b(z, a_10, 2, 0)) return 0;
    return r_mark_suffix_with_optional_y_consonant(z);
}

static int r_mark_ki(struct SN_env * z) {
    return eq_s_b(z, 2, s_3);
}

static int r_mark_ncA(struct SN_env * z) {
    {
        int ret = r_check_vowel_harmony(z);
        if (ret <= 0) return ret;
    }
    if (z->c - 1 <= z->lb || (z->p[z->c - 1] != 97 && z->p[z->c - 1] != 101)) return 0;
    if (!find_among_b(z, a_11, 2, 0)) return 0;
    return r_mark_suffix_with_optional_n_consonant(z);
}

static int r_mark_yUm(struct SN_env * z) {
    {
        int ret = r_check_vowel_harmony(z);
        if (ret <= 0) return ret;
    }
    if (z->c - 1 <= z->lb || z->p[z->c - 1] != 109) return 0;
    if (!find_among_b(z, a_12, 4, 0)) return 0;
    return r_mark_suffix_with_optional_y_consonant(z);
}

static int r_mark_sUn(struct SN_env * z) {
    {
        int ret = r_check_vowel_harmony(z);
        if (ret <= 0) return ret;
    }
    if (z->c - 2 <= z->lb || z->p[z->c - 1] != 110) return 0;
    return find_among_b(z, a_13, 4, 0) != 0;
}

static int r_mark_yUz(struct SN_env * z) {
    {
        int ret = r_check_vowel_harmony(z);
        if (ret <= 0) return ret;
    }
    if (z->c - 1 <= z->lb || z->p[z->c - 1] != 122) return 0;
    if (!find_among_b(z, a_14, 4, 0)) return 0;
    return r_mark_suffix_with_optional_y_consonant(z);
}

static int r_mark_sUnUz(struct SN_env * z) {
    if (z->c - 4 <= z->lb || z->p[z->c - 1] != 122) return 0;
    return find_among_b(z, a_15, 4, 0) != 0;
}

static int r_mark_lAr(struct SN_env * z) {
    {
        int ret = r_check_vowel_harmony(z);
        if (ret <= 0) return ret;
    }
    if (z->c - 2 <= z->lb || z->p[z->c - 1] != 114) return 0;
    return find_among_b(z, a_16, 2, 0) != 0;
}

static int r_mark_nUz(struct SN_env * z) {
    {
        int ret = r_check_vowel_harmony(z);
        if (ret <= 0) return ret;
    }
    if (z->c - 2 <= z->lb || z->p[z->c - 1] != 122) return 0;
    return find_among_b(z, a_17, 4, 0) != 0;
}

static int r_mark_DUr(struct SN_env * z) {
    {
        int ret = r_check_vowel_harmony(z);
        if (ret <= 0) return ret;
    }
    if (z->c - 2 <= z->lb || z->p[z->c - 1] != 114) return 0;
    return find_among_b(z, a_18, 8, 0) != 0;
}

static int r_mark_cAsInA(struct SN_env * z) {
    if (z->c - 5 <= z->lb || (z->p[z->c - 1] != 97 && z->p[z->c - 1] != 101)) return 0;
    return find_among_b(z, a_19, 2, 0) != 0;
}

static int r_mark_yDU(struct SN_env * z) {
    {
        int ret = r_check_vowel_harmony(z);
        if (ret <= 0) return ret;
    }
    if (!find_among_b(z, a_20, 32, 0)) return 0;
    return r_mark_suffix_with_optional_y_consonant(z);
}

static int r_mark_ysA(struct SN_env * z) {
    if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((26658 >> (z->p[z->c - 1] & 0x1f)) & 1)) return 0;
    if (!find_among_b(z, a_21, 8, 0)) return 0;
    return r_mark_suffix_with_optional_y_consonant(z);
}

static int r_mark_ymUs_(struct SN_env * z) {
    {
        int ret = r_check_vowel_harmony(z);
        if (ret <= 0) return ret;
    }
    if (z->c - 3 <= z->lb || z->p[z->c - 1] != 159) return 0;
    if (!find_among_b(z, a_22, 4, 0)) return 0;
    return r_mark_suffix_with_optional_y_consonant(z);
}

static int r_mark_yken(struct SN_env * z) {
    if (!(eq_s_b(z, 3, s_4))) return 0;
    return r_mark_suffix_with_optional_y_consonant(z);
}

static int r_stem_nominal_verb_suffixes(struct SN_env * z) {
    z->ket = z->c;
    ((SN_local *)z)->b_continue_stemming_noun_suffixes = 1;
    do {
        int v_1 = z->l - z->c;
        do {
            int v_2 = z->l - z->c;
            {
                int ret = r_mark_ymUs_(z);
                if (ret == 0) goto lab1;
                if (ret < 0) return ret;
            }
            break;
        lab1:
            z->c = z->l - v_2;
            {
                int ret = r_mark_yDU(z);
                if (ret == 0) goto lab2;
                if (ret < 0) return ret;
            }
            break;
        lab2:
            z->c = z->l - v_2;
            {
                int ret = r_mark_ysA(z);
                if (ret == 0) goto lab3;
                if (ret < 0) return ret;
            }
            break;
        lab3:
            z->c = z->l - v_2;
            {
                int ret = r_mark_yken(z);
                if (ret == 0) goto lab0;
                if (ret < 0) return ret;
            }
        } while (0);
        break;
    lab0:
        z->c = z->l - v_1;
        {
            int ret = r_mark_cAsInA(z);
            if (ret == 0) goto lab4;
            if (ret < 0) return ret;
        }
        do {
            int v_3 = z->l - z->c;
            {
                int ret = r_mark_sUnUz(z);
                if (ret == 0) goto lab5;
                if (ret < 0) return ret;
            }
            break;
        lab5:
            z->c = z->l - v_3;
            {
                int ret = r_mark_lAr(z);
                if (ret == 0) goto lab6;
                if (ret < 0) return ret;
            }
            break;
        lab6:
            z->c = z->l - v_3;
            {
                int ret = r_mark_yUm(z);
                if (ret == 0) goto lab7;
                if (ret < 0) return ret;
            }
            break;
        lab7:
            z->c = z->l - v_3;
            {
                int ret = r_mark_sUn(z);
                if (ret == 0) goto lab8;
                if (ret < 0) return ret;
            }
            break;
        lab8:
            z->c = z->l - v_3;
            {
                int ret = r_mark_yUz(z);
                if (ret == 0) goto lab9;
                if (ret < 0) return ret;
            }
            break;
        lab9:
            z->c = z->l - v_3;
        } while (0);
        {
            int ret = r_mark_ymUs_(z);
            if (ret == 0) goto lab4;
            if (ret < 0) return ret;
        }
        break;
    lab4:
        z->c = z->l - v_1;
        {
            int ret = r_mark_lAr(z);
            if (ret == 0) goto lab10;
            if (ret < 0) return ret;
        }
        z->bra = z->c;
        {
            int ret = slice_del(z);
            if (ret < 0) return ret;
        }
        {
            int v_4 = z->l - z->c;
            z->ket = z->c;
            do {
                int v_5 = z->l - z->c;
                {
                    int ret = r_mark_DUr(z);
                    if (ret == 0) goto lab12;
                    if (ret < 0) return ret;
                }
                break;
            lab12:
                z->c = z->l - v_5;
                {
                    int ret = r_mark_yDU(z);
                    if (ret == 0) goto lab13;
                    if (ret < 0) return ret;
                }
                break;
            lab13:
                z->c = z->l - v_5;
                {
                    int ret = r_mark_ysA(z);
                    if (ret == 0) goto lab14;
                    if (ret < 0) return ret;
                }
                break;
            lab14:
                z->c = z->l - v_5;
                {
                    int ret = r_mark_ymUs_(z);
                    if (ret == 0) { z->c = z->l - v_4; goto lab11; }
                    if (ret < 0) return ret;
                }
            } while (0);
        lab11:
            ;
        }
        ((SN_local *)z)->b_continue_stemming_noun_suffixes = 0;
        break;
    lab10:
        z->c = z->l - v_1;
        {
            int ret = r_mark_nUz(z);
            if (ret == 0) goto lab15;
            if (ret < 0) return ret;
        }
        do {
            int v_6 = z->l - z->c;
            {
                int ret = r_mark_yDU(z);
                if (ret == 0) goto lab16;
                if (ret < 0) return ret;
            }
            break;
        lab16:
            z->c = z->l - v_6;
            {
                int ret = r_mark_ysA(z);
                if (ret == 0) goto lab15;
                if (ret < 0) return ret;
            }
        } while (0);
        break;
    lab15:
        z->c = z->l - v_1;
        do {
            int v_7 = z->l - z->c;
            {
                int ret = r_mark_sUnUz(z);
                if (ret == 0) goto lab18;
                if (ret < 0) return ret;
            }
            break;
        lab18:
            z->c = z->l - v_7;
            {
                int ret = r_mark_yUz(z);
                if (ret == 0) goto lab19;
                if (ret < 0) return ret;
            }
            break;
        lab19:
            z->c = z->l - v_7;
            {
                int ret = r_mark_sUn(z);
                if (ret == 0) goto lab20;
                if (ret < 0) return ret;
            }
            break;
        lab20:
            z->c = z->l - v_7;
            {
                int ret = r_mark_yUm(z);
                if (ret == 0) goto lab17;
                if (ret < 0) return ret;
            }
        } while (0);
        z->bra = z->c;
        {
            int ret = slice_del(z);
            if (ret < 0) return ret;
        }
        {
            int v_8 = z->l - z->c;
            z->ket = z->c;
            {
                int ret = r_mark_ymUs_(z);
                if (ret == 0) { z->c = z->l - v_8; goto lab21; }
                if (ret < 0) return ret;
            }
        lab21:
            ;
        }
        break;
    lab17:
        z->c = z->l - v_1;
        {
            int ret = r_mark_DUr(z);
            if (ret <= 0) return ret;
        }
        z->bra = z->c;
        {
            int ret = slice_del(z);
            if (ret < 0) return ret;
        }
        {
            int v_9 = z->l - z->c;
            z->ket = z->c;
            do {
                int v_10 = z->l - z->c;
                {
                    int ret = r_mark_sUnUz(z);
                    if (ret == 0) goto lab23;
                    if (ret < 0) return ret;
                }
                break;
            lab23:
                z->c = z->l - v_10;
                {
                    int ret = r_mark_lAr(z);
                    if (ret == 0) goto lab24;
                    if (ret < 0) return ret;
                }
                break;
            lab24:
                z->c = z->l - v_10;
                {
                    int ret = r_mark_yUm(z);
                    if (ret == 0) goto lab25;
                    if (ret < 0) return ret;
                }
                break;
            lab25:
                z->c = z->l - v_10;
                {
                    int ret = r_mark_sUn(z);
                    if (ret == 0) goto lab26;
                    if (ret < 0) return ret;
                }
                break;
            lab26:
                z->c = z->l - v_10;
                {
                    int ret = r_mark_yUz(z);
                    if (ret == 0) goto lab27;
                    if (ret < 0) return ret;
                }
                break;
            lab27:
                z->c = z->l - v_10;
            } while (0);
            {
                int ret = r_mark_ymUs_(z);
                if (ret == 0) { z->c = z->l - v_9; goto lab22; }
                if (ret < 0) return ret;
            }
        lab22:
            ;
        }
    } while (0);
    z->bra = z->c;
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_stem_suffix_chain_before_ki(struct SN_env * z) {
    z->ket = z->c;
    {
        int ret = r_mark_ki(z);
        if (ret <= 0) return ret;
    }
    do {
        int v_1 = z->l - z->c;
        {
            int ret = r_mark_DA(z);
            if (ret == 0) goto lab0;
            if (ret < 0) return ret;
        }
        z->bra = z->c;
        {
            int ret = slice_del(z);
            if (ret < 0) return ret;
        }
        {
            int v_2 = z->l - z->c;
            z->ket = z->c;
            do {
                int v_3 = z->l - z->c;
                {
                    int ret = r_mark_lAr(z);
                    if (ret == 0) goto lab2;
                    if (ret < 0) return ret;
                }
                z->bra = z->c;
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                {
                    int v_4 = z->l - z->c;
                    {
                        int ret = r_stem_suffix_chain_before_ki(z);
                        if (ret == 0) { z->c = z->l - v_4; goto lab3; }
                        if (ret < 0) return ret;
                    }
                lab3:
                    ;
                }
                break;
            lab2:
                z->c = z->l - v_3;
                {
                    int ret = r_mark_possessives(z);
                    if (ret == 0) { z->c = z->l - v_2; goto lab1; }
                    if (ret < 0) return ret;
                }
                z->bra = z->c;
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                {
                    int v_5 = z->l - z->c;
                    z->ket = z->c;
                    {
                        int ret = r_mark_lAr(z);
                        if (ret == 0) { z->c = z->l - v_5; goto lab4; }
                        if (ret < 0) return ret;
                    }
                    z->bra = z->c;
                    {
                        int ret = slice_del(z);
                        if (ret < 0) return ret;
                    }
                    {
                        int ret = r_stem_suffix_chain_before_ki(z);
                        if (ret == 0) { z->c = z->l - v_5; goto lab4; }
                        if (ret < 0) return ret;
                    }
                lab4:
                    ;
                }
            } while (0);
        lab1:
            ;
        }
        break;
    lab0:
        z->c = z->l - v_1;
        {
            int ret = r_mark_nUn(z);
            if (ret == 0) goto lab5;
            if (ret < 0) return ret;
        }
        z->bra = z->c;
        {
            int ret = slice_del(z);
            if (ret < 0) return ret;
        }
        {
            int v_6 = z->l - z->c;
            z->ket = z->c;
            do {
                int v_7 = z->l - z->c;
                {
                    int ret = r_mark_lArI(z);
                    if (ret == 0) goto lab7;
                    if (ret < 0) return ret;
                }
                z->bra = z->c;
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                break;
            lab7:
                z->c = z->l - v_7;
                z->ket = z->c;
                do {
                    int v_8 = z->l - z->c;
                    {
                        int ret = r_mark_possessives(z);
                        if (ret == 0) goto lab9;
                        if (ret < 0) return ret;
                    }
                    break;
                lab9:
                    z->c = z->l - v_8;
                    {
                        int ret = r_mark_sU(z);
                        if (ret == 0) goto lab8;
                        if (ret < 0) return ret;
                    }
                } while (0);
                z->bra = z->c;
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                {
                    int v_9 = z->l - z->c;
                    z->ket = z->c;
                    {
                        int ret = r_mark_lAr(z);
                        if (ret == 0) { z->c = z->l - v_9; goto lab10; }
                        if (ret < 0) return ret;
                    }
                    z->bra = z->c;
                    {
                        int ret = slice_del(z);
                        if (ret < 0) return ret;
                    }
                    {
                        int ret = r_stem_suffix_chain_before_ki(z);
                        if (ret == 0) { z->c = z->l - v_9; goto lab10; }
                        if (ret < 0) return ret;
                    }
                lab10:
                    ;
                }
                break;
            lab8:
                z->c = z->l - v_7;
                {
                    int ret = r_stem_suffix_chain_before_ki(z);
                    if (ret == 0) { z->c = z->l - v_6; goto lab6; }
                    if (ret < 0) return ret;
                }
            } while (0);
        lab6:
            ;
        }
        break;
    lab5:
        z->c = z->l - v_1;
        {
            int ret = r_mark_ndA(z);
            if (ret <= 0) return ret;
        }
        do {
            int v_10 = z->l - z->c;
            {
                int ret = r_mark_lArI(z);
                if (ret == 0) goto lab11;
                if (ret < 0) return ret;
            }
            z->bra = z->c;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        lab11:
            z->c = z->l - v_10;
            {
                int ret = r_mark_sU(z);
                if (ret == 0) goto lab12;
                if (ret < 0) return ret;
            }
            z->bra = z->c;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            {
                int v_11 = z->l - z->c;
                z->ket = z->c;
                {
                    int ret = r_mark_lAr(z);
                    if (ret == 0) { z->c = z->l - v_11; goto lab13; }
                    if (ret < 0) return ret;
                }
                z->bra = z->c;
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                {
                    int ret = r_stem_suffix_chain_before_ki(z);
                    if (ret == 0) { z->c = z->l - v_11; goto lab13; }
                    if (ret < 0) return ret;
                }
            lab13:
                ;
            }
            break;
        lab12:
            z->c = z->l - v_10;
            {
                int ret = r_stem_suffix_chain_before_ki(z);
                if (ret <= 0) return ret;
            }
        } while (0);
    } while (0);
    return 1;
}

static int r_stem_noun_suffixes(struct SN_env * z) {
    do {
        int v_1 = z->l - z->c;
        z->ket = z->c;
        {
            int ret = r_mark_lAr(z);
            if (ret == 0) goto lab0;
            if (ret < 0) return ret;
        }
        z->bra = z->c;
        {
            int ret = slice_del(z);
            if (ret < 0) return ret;
        }
        {
            int v_2 = z->l - z->c;
            {
                int ret = r_stem_suffix_chain_before_ki(z);
                if (ret == 0) { z->c = z->l - v_2; goto lab1; }
                if (ret < 0) return ret;
            }
        lab1:
            ;
        }
        break;
    lab0:
        z->c = z->l - v_1;
        z->ket = z->c;
        {
            int ret = r_mark_ncA(z);
            if (ret == 0) goto lab2;
            if (ret < 0) return ret;
        }
        z->bra = z->c;
        {
            int ret = slice_del(z);
            if (ret < 0) return ret;
        }
        {
            int v_3 = z->l - z->c;
            do {
                int v_4 = z->l - z->c;
                z->ket = z->c;
                {
                    int ret = r_mark_lArI(z);
                    if (ret == 0) goto lab4;
                    if (ret < 0) return ret;
                }
                z->bra = z->c;
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                break;
            lab4:
                z->c = z->l - v_4;
                z->ket = z->c;
                do {
                    int v_5 = z->l - z->c;
                    {
                        int ret = r_mark_possessives(z);
                        if (ret == 0) goto lab6;
                        if (ret < 0) return ret;
                    }
                    break;
                lab6:
                    z->c = z->l - v_5;
                    {
                        int ret = r_mark_sU(z);
                        if (ret == 0) goto lab5;
                        if (ret < 0) return ret;
                    }
                } while (0);
                z->bra = z->c;
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                {
                    int v_6 = z->l - z->c;
                    z->ket = z->c;
                    {
                        int ret = r_mark_lAr(z);
                        if (ret == 0) { z->c = z->l - v_6; goto lab7; }
                        if (ret < 0) return ret;
                    }
                    z->bra = z->c;
                    {
                        int ret = slice_del(z);
                        if (ret < 0) return ret;
                    }
                    {
                        int ret = r_stem_suffix_chain_before_ki(z);
                        if (ret == 0) { z->c = z->l - v_6; goto lab7; }
                        if (ret < 0) return ret;
                    }
                lab7:
                    ;
                }
                break;
            lab5:
                z->c = z->l - v_4;
                z->ket = z->c;
                {
                    int ret = r_mark_lAr(z);
                    if (ret == 0) { z->c = z->l - v_3; goto lab3; }
                    if (ret < 0) return ret;
                }
                z->bra = z->c;
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                {
                    int ret = r_stem_suffix_chain_before_ki(z);
                    if (ret == 0) { z->c = z->l - v_3; goto lab3; }
                    if (ret < 0) return ret;
                }
            } while (0);
        lab3:
            ;
        }
        break;
    lab2:
        z->c = z->l - v_1;
        z->ket = z->c;
        do {
            int v_7 = z->l - z->c;
            {
                int ret = r_mark_ndA(z);
                if (ret == 0) goto lab9;
                if (ret < 0) return ret;
            }
            break;
        lab9:
            z->c = z->l - v_7;
            {
                int ret = r_mark_nA(z);
                if (ret == 0) goto lab8;
                if (ret < 0) return ret;
            }
        } while (0);
        do {
            int v_8 = z->l - z->c;
            {
                int ret = r_mark_lArI(z);
                if (ret == 0) goto lab10;
                if (ret < 0) return ret;
            }
            z->bra = z->c;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        lab10:
            z->c = z->l - v_8;
            {
                int ret = r_mark_sU(z);
                if (ret == 0) goto lab11;
                if (ret < 0) return ret;
            }
            z->bra = z->c;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            {
                int v_9 = z->l - z->c;
                z->ket = z->c;
                {
                    int ret = r_mark_lAr(z);
                    if (ret == 0) { z->c = z->l - v_9; goto lab12; }
                    if (ret < 0) return ret;
                }
                z->bra = z->c;
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                {
                    int ret = r_stem_suffix_chain_before_ki(z);
                    if (ret == 0) { z->c = z->l - v_9; goto lab12; }
                    if (ret < 0) return ret;
                }
            lab12:
                ;
            }
            break;
        lab11:
            z->c = z->l - v_8;
            {
                int ret = r_stem_suffix_chain_before_ki(z);
                if (ret == 0) goto lab8;
                if (ret < 0) return ret;
            }
        } while (0);
        break;
    lab8:
        z->c = z->l - v_1;
        z->ket = z->c;
        do {
            int v_10 = z->l - z->c;
            {
                int ret = r_mark_ndAn(z);
                if (ret == 0) goto lab14;
                if (ret < 0) return ret;
            }
            break;
        lab14:
            z->c = z->l - v_10;
            {
                int ret = r_mark_nU(z);
                if (ret == 0) goto lab13;
                if (ret < 0) return ret;
            }
        } while (0);
        do {
            int v_11 = z->l - z->c;
            {
                int ret = r_mark_sU(z);
                if (ret == 0) goto lab15;
                if (ret < 0) return ret;
            }
            z->bra = z->c;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            {
                int v_12 = z->l - z->c;
                z->ket = z->c;
                {
                    int ret = r_mark_lAr(z);
                    if (ret == 0) { z->c = z->l - v_12; goto lab16; }
                    if (ret < 0) return ret;
                }
                z->bra = z->c;
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                {
                    int ret = r_stem_suffix_chain_before_ki(z);
                    if (ret == 0) { z->c = z->l - v_12; goto lab16; }
                    if (ret < 0) return ret;
                }
            lab16:
                ;
            }
            break;
        lab15:
            z->c = z->l - v_11;
            {
                int ret = r_mark_lArI(z);
                if (ret == 0) goto lab13;
                if (ret < 0) return ret;
            }
        } while (0);
        break;
    lab13:
        z->c = z->l - v_1;
        z->ket = z->c;
        {
            int ret = r_mark_DAn(z);
            if (ret == 0) goto lab17;
            if (ret < 0) return ret;
        }
        z->bra = z->c;
        {
            int ret = slice_del(z);
            if (ret < 0) return ret;
        }
        {
            int v_13 = z->l - z->c;
            z->ket = z->c;
            do {
                int v_14 = z->l - z->c;
                {
                    int ret = r_mark_possessives(z);
                    if (ret == 0) goto lab19;
                    if (ret < 0) return ret;
                }
                z->bra = z->c;
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                {
                    int v_15 = z->l - z->c;
                    z->ket = z->c;
                    {
                        int ret = r_mark_lAr(z);
                        if (ret == 0) { z->c = z->l - v_15; goto lab20; }
                        if (ret < 0) return ret;
                    }
                    z->bra = z->c;
                    {
                        int ret = slice_del(z);
                        if (ret < 0) return ret;
                    }
                    {
                        int ret = r_stem_suffix_chain_before_ki(z);
                        if (ret == 0) { z->c = z->l - v_15; goto lab20; }
                        if (ret < 0) return ret;
                    }
                lab20:
                    ;
                }
                break;
            lab19:
                z->c = z->l - v_14;
                {
                    int ret = r_mark_lAr(z);
                    if (ret == 0) goto lab21;
                    if (ret < 0) return ret;
                }
                z->bra = z->c;
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                {
                    int v_16 = z->l - z->c;
                    {
                        int ret = r_stem_suffix_chain_before_ki(z);
                        if (ret == 0) { z->c = z->l - v_16; goto lab22; }
                        if (ret < 0) return ret;
                    }
                lab22:
                    ;
                }
                break;
            lab21:
                z->c = z->l - v_14;
                {
                    int ret = r_stem_suffix_chain_before_ki(z);
                    if (ret == 0) { z->c = z->l - v_13; goto lab18; }
                    if (ret < 0) return ret;
                }
            } while (0);
        lab18:
            ;
        }
        break;
    lab17:
        z->c = z->l - v_1;
        z->ket = z->c;
        do {
            int v_17 = z->l - z->c;
            {
                int ret = r_mark_nUn(z);
                if (ret == 0) goto lab24;
                if (ret < 0) return ret;
            }
            break;
        lab24:
            z->c = z->l - v_17;
            {
                int ret = r_mark_ylA(z);
                if (ret == 0) goto lab23;
                if (ret < 0) return ret;
            }
        } while (0);
        z->bra = z->c;
        {
            int ret = slice_del(z);
            if (ret < 0) return ret;
        }
        {
            int v_18 = z->l - z->c;
            do {
                int v_19 = z->l - z->c;
                z->ket = z->c;
                {
                    int ret = r_mark_lAr(z);
                    if (ret == 0) goto lab26;
                    if (ret < 0) return ret;
                }
                z->bra = z->c;
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                {
                    int ret = r_stem_suffix_chain_before_ki(z);
                    if (ret == 0) goto lab26;
                    if (ret < 0) return ret;
                }
                break;
            lab26:
                z->c = z->l - v_19;
                z->ket = z->c;
                do {
                    int v_20 = z->l - z->c;
                    {
                        int ret = r_mark_possessives(z);
                        if (ret == 0) goto lab28;
                        if (ret < 0) return ret;
                    }
                    break;
                lab28:
                    z->c = z->l - v_20;
                    {
                        int ret = r_mark_sU(z);
                        if (ret == 0) goto lab27;
                        if (ret < 0) return ret;
                    }
                } while (0);
                z->bra = z->c;
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                {
                    int v_21 = z->l - z->c;
                    z->ket = z->c;
                    {
                        int ret = r_mark_lAr(z);
                        if (ret == 0) { z->c = z->l - v_21; goto lab29; }
                        if (ret < 0) return ret;
                    }
                    z->bra = z->c;
                    {
                        int ret = slice_del(z);
                        if (ret < 0) return ret;
                    }
                    {
                        int ret = r_stem_suffix_chain_before_ki(z);
                        if (ret == 0) { z->c = z->l - v_21; goto lab29; }
                        if (ret < 0) return ret;
                    }
                lab29:
                    ;
                }
                break;
            lab27:
                z->c = z->l - v_19;
                {
                    int ret = r_stem_suffix_chain_before_ki(z);
                    if (ret == 0) { z->c = z->l - v_18; goto lab25; }
                    if (ret < 0) return ret;
                }
            } while (0);
        lab25:
            ;
        }
        break;
    lab23:
        z->c = z->l - v_1;
        z->ket = z->c;
        {
            int ret = r_mark_lArI(z);
            if (ret == 0) goto lab30;
            if (ret < 0) return ret;
        }
        z->bra = z->c;
        {
            int ret = slice_del(z);
            if (ret < 0) return ret;
        }
        break;
    lab30:
        z->c = z->l - v_1;
        {
            int ret = r_stem_suffix_chain_before_ki(z);
            if (ret == 0) goto lab31;
            if (ret < 0) return ret;
        }
        break;
    lab31:
        z->c = z->l - v_1;
        z->ket = z->c;
        do {
            int v_22 = z->l - z->c;
            {
                int ret = r_mark_DA(z);
                if (ret == 0) goto lab33;
                if (ret < 0) return ret;
            }
            break;
        lab33:
            z->c = z->l - v_22;
            {
                int ret = r_mark_yU(z);
                if (ret == 0) goto lab34;
                if (ret < 0) return ret;
            }
            break;
        lab34:
            z->c = z->l - v_22;
            {
                int ret = r_mark_yA(z);
                if (ret == 0) goto lab32;
                if (ret < 0) return ret;
            }
        } while (0);
        z->bra = z->c;
        {
            int ret = slice_del(z);
            if (ret < 0) return ret;
        }
        {
            int v_23 = z->l - z->c;
            z->ket = z->c;
            do {
                int v_24 = z->l - z->c;
                {
                    int ret = r_mark_possessives(z);
                    if (ret == 0) goto lab36;
                    if (ret < 0) return ret;
                }
                z->bra = z->c;
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                {
                    int v_25 = z->l - z->c;
                    z->ket = z->c;
                    {
                        int ret = r_mark_lAr(z);
                        if (ret == 0) { z->c = z->l - v_25; goto lab37; }
                        if (ret < 0) return ret;
                    }
                lab37:
                    ;
                }
                break;
            lab36:
                z->c = z->l - v_24;
                {
                    int ret = r_mark_lAr(z);
                    if (ret == 0) { z->c = z->l - v_23; goto lab35; }
                    if (ret < 0) return ret;
                }
            } while (0);
            z->bra = z->c;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            z->ket = z->c;
            {
                int ret = r_stem_suffix_chain_before_ki(z);
                if (ret == 0) { z->c = z->l - v_23; goto lab35; }
                if (ret < 0) return ret;
            }
        lab35:
            ;
        }
        break;
    lab32:
        z->c = z->l - v_1;
        z->ket = z->c;
        do {
            int v_26 = z->l - z->c;
            {
                int ret = r_mark_possessives(z);
                if (ret == 0) goto lab38;
                if (ret < 0) return ret;
            }
            break;
        lab38:
            z->c = z->l - v_26;
            {
                int ret = r_mark_sU(z);
                if (ret <= 0) return ret;
            }
        } while (0);
        z->bra = z->c;
        {
            int ret = slice_del(z);
            if (ret < 0) return ret;
        }
        {
            int v_27 = z->l - z->c;
            z->ket = z->c;
            {
                int ret = r_mark_lAr(z);
                if (ret == 0) { z->c = z->l - v_27; goto lab39; }
                if (ret < 0) return ret;
            }
            z->bra = z->c;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            {
                int ret = r_stem_suffix_chain_before_ki(z);
                if (ret == 0) { z->c = z->l - v_27; goto lab39; }
                if (ret < 0) return ret;
            }
        lab39:
            ;
        }
    } while (0);
    return 1;
}

static int r_post_process_last_consonants(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    among_var = find_among_b(z, a_23, 4, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    switch (among_var) {
        case 1:
            {
                int ret = slice_from_s(z, 1, s_5);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {
                int ret = slice_from_s(z, 2, s_6);
                if (ret < 0) return ret;
            }
            break;
        case 3:
            {
                int ret = slice_from_s(z, 1, s_7);
                if (ret < 0) return ret;
            }
            break;
        case 4:
            {
                int ret = slice_from_s(z, 1, s_8);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_append_U_to_stems_ending_with_d_or_g(struct SN_env * z) {
    z->ket = z->c;
    z->bra = z->c;
    do {
        int v_1 = z->l - z->c;
        if (z->c <= z->lb || z->p[z->c - 1] != 'd') goto lab0;
        z->c--;
        break;
    lab0:
        z->c = z->l - v_1;
        if (z->c <= z->lb || z->p[z->c - 1] != 'g') return 0;
        z->c--;
    } while (0);
    if (out_grouping_b_U(z, g_vowel, 97, 305, 1) < 0) return 0;
    do {
        int v_2 = z->l - z->c;
        do {
            int v_3 = z->l - z->c;
            if (z->c <= z->lb || z->p[z->c - 1] != 'a') goto lab2;
            z->c--;
            break;
        lab2:
            z->c = z->l - v_3;
            if (!(eq_s_b(z, 2, s_9))) goto lab1;
        } while (0);
        {
            int ret = slice_from_s(z, 2, s_10);
            if (ret < 0) return ret;
        }
        break;
    lab1:
        z->c = z->l - v_2;
        do {
            int v_4 = z->l - z->c;
            if (z->c <= z->lb || z->p[z->c - 1] != 'e') goto lab4;
            z->c--;
            break;
        lab4:
            z->c = z->l - v_4;
            if (z->c <= z->lb || z->p[z->c - 1] != 'i') goto lab3;
            z->c--;
        } while (0);
        {
            int ret = slice_from_s(z, 1, s_11);
            if (ret < 0) return ret;
        }
        break;
    lab3:
        z->c = z->l - v_2;
        do {
            int v_5 = z->l - z->c;
            if (z->c <= z->lb || z->p[z->c - 1] != 'o') goto lab6;
            z->c--;
            break;
        lab6:
            z->c = z->l - v_5;
            if (z->c <= z->lb || z->p[z->c - 1] != 'u') goto lab5;
            z->c--;
        } while (0);
        {
            int ret = slice_from_s(z, 1, s_12);
            if (ret < 0) return ret;
        }
        break;
    lab5:
        z->c = z->l - v_2;
        do {
            int v_6 = z->l - z->c;
            if (!(eq_s_b(z, 2, s_13))) goto lab7;
            break;
        lab7:
            z->c = z->l - v_6;
            if (!(eq_s_b(z, 2, s_14))) return 0;
        } while (0);
        {
            int ret = slice_from_s(z, 2, s_15);
            if (ret < 0) return ret;
        }
    } while (0);
    return 1;
}

static int r_is_reserved_word(struct SN_env * z) {
    if (!(eq_s_b(z, 2, s_16))) return 0;
    {
        int v_1 = z->l - z->c;
        if (!(eq_s_b(z, 3, s_17))) { z->c = z->l - v_1; goto lab0; }
    lab0:
        ;
    }
    if (z->c > z->lb) return 0;
    return 1;
}

static int r_remove_proper_noun_suffix(struct SN_env * z) {
    {
        int v_1 = z->c;
        z->bra = z->c;
        while (1) {
            int v_2 = z->c;
            {
                int v_3 = z->c;
                if (z->c == z->l || z->p[z->c] != '\'') goto lab2;
                z->c++;
                goto lab1;
            lab2:
                z->c = v_3;
            }
            z->c = v_2;
            break;
        lab1:
            z->c = v_2;
            {
                int ret = skip_utf8(z->p, z->c, z->l, 1);
                if (ret < 0) goto lab0;
                z->c = ret;
            }
        }
        z->ket = z->c;
        {
            int ret = slice_del(z);
            if (ret < 0) return ret;
        }
    lab0:
        z->c = v_1;
    }
    {
        int v_4 = z->c;
        {
            int ret = skip_utf8(z->p, z->c, z->l, 2);
            if (ret < 0) goto lab3;
            z->c = ret;
        }
        while (1) {
            int v_5 = z->c;
            if (z->c == z->l || z->p[z->c] != '\'') goto lab4;
            z->c++;
            z->c = v_5;
            break;
        lab4:
            z->c = v_5;
            {
                int ret = skip_utf8(z->p, z->c, z->l, 1);
                if (ret < 0) goto lab3;
                z->c = ret;
            }
        }
        z->bra = z->c;
        z->c = z->l;
        z->ket = z->c;
        {
            int ret = slice_del(z);
            if (ret < 0) return ret;
        }
    lab3:
        z->c = v_4;
    }
    return 1;
}

static int r_more_than_one_syllable_word(struct SN_env * z) {
    {
        int v_1 = z->c;
        {
            int i; for (i = 2; i > 0; i--) {
                {
                    int ret = out_grouping_U(z, g_vowel, 97, 305, 1);
                    if (ret < 0) return 0;
                    z->c += ret;
                }
            }
        }
        z->c = v_1;
    }
    return 1;
}

static int r_postlude(struct SN_env * z) {
    z->lb = z->c; z->c = z->l;
    {
        int v_1 = z->l - z->c;
        {
            int ret = r_is_reserved_word(z);
            if (ret == 0) goto lab0;
            if (ret < 0) return ret;
        }
        return 0;
    lab0:
        z->c = z->l - v_1;
    }
    {
        int v_2 = z->l - z->c;
        {
            int ret = r_append_U_to_stems_ending_with_d_or_g(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_2;
    }
    {
        int v_3 = z->l - z->c;
        {
            int ret = r_post_process_last_consonants(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_3;
    }
    z->c = z->lb;
    return 1;
}

extern int turkish_UTF_8_stem(struct SN_env * z) {
    {
        int ret = r_remove_proper_noun_suffix(z);
        if (ret < 0) return ret;
    }
    {
        int ret = r_more_than_one_syllable_word(z);
        if (ret <= 0) return ret;
    }
    z->lb = z->c; z->c = z->l;
    {
        int v_1 = z->l - z->c;
        {
            int ret = r_stem_nominal_verb_suffixes(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_1;
    }
    if (!((SN_local *)z)->b_continue_stemming_noun_suffixes) return 0;
    {
        int v_2 = z->l - z->c;
        {
            int ret = r_stem_noun_suffixes(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_2;
    }
    z->c = z->lb;
    return r_postlude(z);
}

extern struct SN_env * turkish_UTF_8_create_env(void) {
    struct SN_env * z = SN_new_env(sizeof(SN_local));
    if (z) {
        ((SN_local *)z)->b_continue_stemming_noun_suffixes = 0;
    }
    return z;
}

extern void turkish_UTF_8_close_env(struct SN_env * z) {
    SN_delete_env(z);
}

