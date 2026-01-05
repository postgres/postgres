/* Generated from estonian.sbl by Snowball 3.0.0 - https://snowballstem.org/ */

#include "stem_UTF_8_estonian.h"

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
extern int estonian_UTF_8_stem(struct SN_env * z);
#ifdef __cplusplus
}
#endif

static int r_nu(struct SN_env * z);
static int r_verb(struct SN_env * z);
static int r_verb_exceptions(struct SN_env * z);
static int r_substantive(struct SN_env * z);
static int r_degrees(struct SN_env * z);
static int r_i_plural(struct SN_env * z);
static int r_undouble_kpt(struct SN_env * z);
static int r_plural_three_first_cases(struct SN_env * z);
static int r_emphasis(struct SN_env * z);
static int r_case_ending(struct SN_env * z);
static int r_special_noun_endings(struct SN_env * z);
static int r_LONGV(struct SN_env * z);
static int r_mark_regions(struct SN_env * z);

static const symbol s_0[] = { 'a' };
static const symbol s_1[] = { 'l', 'a', 's', 'e' };
static const symbol s_2[] = { 'm', 'i', 's', 'e' };
static const symbol s_3[] = { 'l', 'i', 's', 'e' };
static const symbol s_4[] = { 'i', 'k', 'u' };
static const symbol s_5[] = { 'e' };
static const symbol s_6[] = { 't' };
static const symbol s_7[] = { 'k' };
static const symbol s_8[] = { 'p' };
static const symbol s_9[] = { 't' };
static const symbol s_10[] = { 'j', 'o', 'o' };
static const symbol s_11[] = { 's', 'a', 'a' };
static const symbol s_12[] = { 'v', 'i', 'i', 'm', 'a' };
static const symbol s_13[] = { 'k', 'e', 'e', 's', 'i' };
static const symbol s_14[] = { 'l', 0xC3, 0xB6, 0xC3, 0xB6 };
static const symbol s_15[] = { 'l', 0xC3, 0xB5, 'i' };
static const symbol s_16[] = { 'l', 'o', 'o' };
static const symbol s_17[] = { 'k', 0xC3, 0xA4, 'i', 's', 'i' };
static const symbol s_18[] = { 's', 0xC3, 0xB6, 0xC3, 0xB6 };
static const symbol s_19[] = { 't', 'o', 'o' };
static const symbol s_20[] = { 'v', 0xC3, 0xB5, 'i', 's', 'i' };
static const symbol s_21[] = { 'j', 0xC3, 0xA4, 0xC3, 0xA4, 'm', 'a' };
static const symbol s_22[] = { 'm', 0xC3, 0xBC, 0xC3, 0xBC, 's', 'i' };
static const symbol s_23[] = { 'l', 'u', 'g', 'e' };
static const symbol s_24[] = { 'p', 0xC3, 0xB5, 'd', 'e' };
static const symbol s_25[] = { 'l', 'a', 'd', 'u' };
static const symbol s_26[] = { 't', 'e', 'g', 'i' };
static const symbol s_27[] = { 'n', 0xC3, 0xA4, 'g', 'i' };

static const symbol s_0_0[2] = { 'g', 'i' };
static const symbol s_0_1[2] = { 'k', 'i' };
static const struct among a_0[2] = {
{ 2, s_0_0, 0, 1, 0},
{ 2, s_0_1, 0, 2, 0}
};

static const symbol s_1_0[2] = { 'd', 'a' };
static const symbol s_1_1[4] = { 'm', 'a', 't', 'a' };
static const symbol s_1_2[1] = { 'b' };
static const symbol s_1_3[4] = { 'k', 's', 'i', 'd' };
static const symbol s_1_4[6] = { 'n', 'u', 'k', 's', 'i', 'd' };
static const symbol s_1_5[2] = { 'm', 'e' };
static const symbol s_1_6[4] = { 's', 'i', 'm', 'e' };
static const symbol s_1_7[5] = { 'k', 's', 'i', 'm', 'e' };
static const symbol s_1_8[7] = { 'n', 'u', 'k', 's', 'i', 'm', 'e' };
static const symbol s_1_9[4] = { 'a', 'k', 's', 'e' };
static const symbol s_1_10[5] = { 'd', 'a', 'k', 's', 'e' };
static const symbol s_1_11[5] = { 't', 'a', 'k', 's', 'e' };
static const symbol s_1_12[4] = { 's', 'i', 't', 'e' };
static const symbol s_1_13[5] = { 'k', 's', 'i', 't', 'e' };
static const symbol s_1_14[7] = { 'n', 'u', 'k', 's', 'i', 't', 'e' };
static const symbol s_1_15[1] = { 'n' };
static const symbol s_1_16[3] = { 's', 'i', 'n' };
static const symbol s_1_17[4] = { 'k', 's', 'i', 'n' };
static const symbol s_1_18[6] = { 'n', 'u', 'k', 's', 'i', 'n' };
static const symbol s_1_19[4] = { 'd', 'a', 'k', 's' };
static const symbol s_1_20[4] = { 't', 'a', 'k', 's' };
static const struct among a_1[21] = {
{ 2, s_1_0, 0, 3, 0},
{ 4, s_1_1, 0, 1, 0},
{ 1, s_1_2, 0, 3, 0},
{ 4, s_1_3, 0, 1, 0},
{ 6, s_1_4, -1, 1, 0},
{ 2, s_1_5, 0, 3, 0},
{ 4, s_1_6, -1, 1, 0},
{ 5, s_1_7, -1, 1, 0},
{ 7, s_1_8, -1, 1, 0},
{ 4, s_1_9, 0, 2, 0},
{ 5, s_1_10, -1, 1, 0},
{ 5, s_1_11, -2, 1, 0},
{ 4, s_1_12, 0, 1, 0},
{ 5, s_1_13, -1, 1, 0},
{ 7, s_1_14, -1, 1, 0},
{ 1, s_1_15, 0, 3, 0},
{ 3, s_1_16, -1, 1, 0},
{ 4, s_1_17, -1, 1, 0},
{ 6, s_1_18, -1, 1, 0},
{ 4, s_1_19, 0, 1, 0},
{ 4, s_1_20, 0, 1, 0}
};

static const symbol s_2_0[2] = { 'a', 'a' };
static const symbol s_2_1[2] = { 'e', 'e' };
static const symbol s_2_2[2] = { 'i', 'i' };
static const symbol s_2_3[2] = { 'o', 'o' };
static const symbol s_2_4[2] = { 'u', 'u' };
static const symbol s_2_5[4] = { 0xC3, 0xA4, 0xC3, 0xA4 };
static const symbol s_2_6[4] = { 0xC3, 0xB5, 0xC3, 0xB5 };
static const symbol s_2_7[4] = { 0xC3, 0xB6, 0xC3, 0xB6 };
static const symbol s_2_8[4] = { 0xC3, 0xBC, 0xC3, 0xBC };
static const struct among a_2[9] = {
{ 2, s_2_0, 0, -1, 0},
{ 2, s_2_1, 0, -1, 0},
{ 2, s_2_2, 0, -1, 0},
{ 2, s_2_3, 0, -1, 0},
{ 2, s_2_4, 0, -1, 0},
{ 4, s_2_5, 0, -1, 0},
{ 4, s_2_6, 0, -1, 0},
{ 4, s_2_7, 0, -1, 0},
{ 4, s_2_8, 0, -1, 0}
};

static const symbol s_3_0[4] = { 'l', 'a', 'n', 'e' };
static const symbol s_3_1[4] = { 'l', 'i', 'n', 'e' };
static const symbol s_3_2[4] = { 'm', 'i', 'n', 'e' };
static const symbol s_3_3[5] = { 'l', 'a', 's', 's', 'e' };
static const symbol s_3_4[5] = { 'l', 'i', 's', 's', 'e' };
static const symbol s_3_5[5] = { 'm', 'i', 's', 's', 'e' };
static const symbol s_3_6[4] = { 'l', 'a', 's', 'i' };
static const symbol s_3_7[4] = { 'l', 'i', 's', 'i' };
static const symbol s_3_8[4] = { 'm', 'i', 's', 'i' };
static const symbol s_3_9[4] = { 'l', 'a', 's', 't' };
static const symbol s_3_10[4] = { 'l', 'i', 's', 't' };
static const symbol s_3_11[4] = { 'm', 'i', 's', 't' };
static const struct among a_3[12] = {
{ 4, s_3_0, 0, 1, 0},
{ 4, s_3_1, 0, 3, 0},
{ 4, s_3_2, 0, 2, 0},
{ 5, s_3_3, 0, 1, 0},
{ 5, s_3_4, 0, 3, 0},
{ 5, s_3_5, 0, 2, 0},
{ 4, s_3_6, 0, 1, 0},
{ 4, s_3_7, 0, 3, 0},
{ 4, s_3_8, 0, 2, 0},
{ 4, s_3_9, 0, 1, 0},
{ 4, s_3_10, 0, 3, 0},
{ 4, s_3_11, 0, 2, 0}
};

static const symbol s_4_0[2] = { 'g', 'a' };
static const symbol s_4_1[2] = { 't', 'a' };
static const symbol s_4_2[2] = { 'l', 'e' };
static const symbol s_4_3[3] = { 's', 's', 'e' };
static const symbol s_4_4[1] = { 'l' };
static const symbol s_4_5[1] = { 's' };
static const symbol s_4_6[2] = { 'k', 's' };
static const symbol s_4_7[1] = { 't' };
static const symbol s_4_8[2] = { 'l', 't' };
static const symbol s_4_9[2] = { 's', 't' };
static const struct among a_4[10] = {
{ 2, s_4_0, 0, 1, 0},
{ 2, s_4_1, 0, 1, 0},
{ 2, s_4_2, 0, 1, 0},
{ 3, s_4_3, 0, 1, 0},
{ 1, s_4_4, 0, 1, 0},
{ 1, s_4_5, 0, 1, 0},
{ 2, s_4_6, -1, 1, 0},
{ 1, s_4_7, 0, 2, 0},
{ 2, s_4_8, -1, 1, 0},
{ 2, s_4_9, -2, 1, 0}
};

static const symbol s_5_1[3] = { 'l', 'a', 's' };
static const symbol s_5_2[3] = { 'l', 'i', 's' };
static const symbol s_5_3[3] = { 'm', 'i', 's' };
static const symbol s_5_4[1] = { 't' };
static const struct among a_5[5] = {
{ 0, 0, 0, 2, 0},
{ 3, s_5_1, -1, 1, 0},
{ 3, s_5_2, -2, 1, 0},
{ 3, s_5_3, -3, 1, 0},
{ 1, s_5_4, -4, -1, 0}
};

static const symbol s_6_0[1] = { 'd' };
static const symbol s_6_1[3] = { 's', 'i', 'd' };
static const symbol s_6_2[2] = { 'd', 'e' };
static const symbol s_6_3[6] = { 'i', 'k', 'k', 'u', 'd', 'e' };
static const symbol s_6_4[3] = { 'i', 'k', 'e' };
static const symbol s_6_5[4] = { 'i', 'k', 'k', 'e' };
static const symbol s_6_6[2] = { 't', 'e' };
static const struct among a_6[7] = {
{ 1, s_6_0, 0, 4, 0},
{ 3, s_6_1, -1, 2, 0},
{ 2, s_6_2, 0, 4, 0},
{ 6, s_6_3, -1, 1, 0},
{ 3, s_6_4, 0, 1, 0},
{ 4, s_6_5, 0, 1, 0},
{ 2, s_6_6, 0, 3, 0}
};

static const symbol s_7_0[2] = { 'v', 'a' };
static const symbol s_7_1[2] = { 'd', 'u' };
static const symbol s_7_2[2] = { 'n', 'u' };
static const symbol s_7_3[2] = { 't', 'u' };
static const struct among a_7[4] = {
{ 2, s_7_0, 0, -1, 0},
{ 2, s_7_1, 0, -1, 0},
{ 2, s_7_2, 0, -1, 0},
{ 2, s_7_3, 0, -1, 0}
};

static const symbol s_8_0[2] = { 'k', 'k' };
static const symbol s_8_1[2] = { 'p', 'p' };
static const symbol s_8_2[2] = { 't', 't' };
static const struct among a_8[3] = {
{ 2, s_8_0, 0, 1, 0},
{ 2, s_8_1, 0, 2, 0},
{ 2, s_8_2, 0, 3, 0}
};

static const symbol s_9_0[2] = { 'm', 'a' };
static const symbol s_9_1[3] = { 'm', 'a', 'i' };
static const symbol s_9_2[1] = { 'm' };
static const struct among a_9[3] = {
{ 2, s_9_0, 0, 2, 0},
{ 3, s_9_1, 0, 1, 0},
{ 1, s_9_2, 0, 1, 0}
};

static const symbol s_10_0[4] = { 'j', 'o', 'o', 'b' };
static const symbol s_10_1[4] = { 'j', 'o', 'o', 'd' };
static const symbol s_10_2[8] = { 'j', 'o', 'o', 'd', 'a', 'k', 's', 'e' };
static const symbol s_10_3[5] = { 'j', 'o', 'o', 'm', 'a' };
static const symbol s_10_4[7] = { 'j', 'o', 'o', 'm', 'a', 't', 'a' };
static const symbol s_10_5[5] = { 'j', 'o', 'o', 'm', 'e' };
static const symbol s_10_6[4] = { 'j', 'o', 'o', 'n' };
static const symbol s_10_7[5] = { 'j', 'o', 'o', 't', 'e' };
static const symbol s_10_8[6] = { 'j', 'o', 'o', 'v', 'a', 'd' };
static const symbol s_10_9[4] = { 'j', 'u', 'u', 'a' };
static const symbol s_10_10[7] = { 'j', 'u', 'u', 'a', 'k', 's', 'e' };
static const symbol s_10_11[4] = { 'j', 0xC3, 0xA4, 'i' };
static const symbol s_10_12[5] = { 'j', 0xC3, 0xA4, 'i', 'd' };
static const symbol s_10_13[6] = { 'j', 0xC3, 0xA4, 'i', 'm', 'e' };
static const symbol s_10_14[5] = { 'j', 0xC3, 0xA4, 'i', 'n' };
static const symbol s_10_15[6] = { 'j', 0xC3, 0xA4, 'i', 't', 'e' };
static const symbol s_10_16[6] = { 'j', 0xC3, 0xA4, 0xC3, 0xA4, 'b' };
static const symbol s_10_17[6] = { 'j', 0xC3, 0xA4, 0xC3, 0xA4, 'd' };
static const symbol s_10_18[7] = { 'j', 0xC3, 0xA4, 0xC3, 0xA4, 'd', 'a' };
static const symbol s_10_19[10] = { 'j', 0xC3, 0xA4, 0xC3, 0xA4, 'd', 'a', 'k', 's', 'e' };
static const symbol s_10_20[7] = { 'j', 0xC3, 0xA4, 0xC3, 0xA4, 'd', 'i' };
static const symbol s_10_21[7] = { 'j', 0xC3, 0xA4, 0xC3, 0xA4, 'k', 's' };
static const symbol s_10_22[9] = { 'j', 0xC3, 0xA4, 0xC3, 0xA4, 'k', 's', 'i', 'd' };
static const symbol s_10_23[10] = { 'j', 0xC3, 0xA4, 0xC3, 0xA4, 'k', 's', 'i', 'm', 'e' };
static const symbol s_10_24[9] = { 'j', 0xC3, 0xA4, 0xC3, 0xA4, 'k', 's', 'i', 'n' };
static const symbol s_10_25[10] = { 'j', 0xC3, 0xA4, 0xC3, 0xA4, 'k', 's', 'i', 't', 'e' };
static const symbol s_10_26[7] = { 'j', 0xC3, 0xA4, 0xC3, 0xA4, 'm', 'a' };
static const symbol s_10_27[9] = { 'j', 0xC3, 0xA4, 0xC3, 0xA4, 'm', 'a', 't', 'a' };
static const symbol s_10_28[7] = { 'j', 0xC3, 0xA4, 0xC3, 0xA4, 'm', 'e' };
static const symbol s_10_29[6] = { 'j', 0xC3, 0xA4, 0xC3, 0xA4, 'n' };
static const symbol s_10_30[7] = { 'j', 0xC3, 0xA4, 0xC3, 0xA4, 't', 'e' };
static const symbol s_10_31[8] = { 'j', 0xC3, 0xA4, 0xC3, 0xA4, 'v', 'a', 'd' };
static const symbol s_10_32[4] = { 'j', 0xC3, 0xB5, 'i' };
static const symbol s_10_33[5] = { 'j', 0xC3, 0xB5, 'i', 'd' };
static const symbol s_10_34[6] = { 'j', 0xC3, 0xB5, 'i', 'm', 'e' };
static const symbol s_10_35[5] = { 'j', 0xC3, 0xB5, 'i', 'n' };
static const symbol s_10_36[6] = { 'j', 0xC3, 0xB5, 'i', 't', 'e' };
static const symbol s_10_37[4] = { 'k', 'e', 'e', 'b' };
static const symbol s_10_38[4] = { 'k', 'e', 'e', 'd' };
static const symbol s_10_39[8] = { 'k', 'e', 'e', 'd', 'a', 'k', 's', 'e' };
static const symbol s_10_40[5] = { 'k', 'e', 'e', 'k', 's' };
static const symbol s_10_41[7] = { 'k', 'e', 'e', 'k', 's', 'i', 'd' };
static const symbol s_10_42[8] = { 'k', 'e', 'e', 'k', 's', 'i', 'm', 'e' };
static const symbol s_10_43[7] = { 'k', 'e', 'e', 'k', 's', 'i', 'n' };
static const symbol s_10_44[8] = { 'k', 'e', 'e', 'k', 's', 'i', 't', 'e' };
static const symbol s_10_45[5] = { 'k', 'e', 'e', 'm', 'a' };
static const symbol s_10_46[7] = { 'k', 'e', 'e', 'm', 'a', 't', 'a' };
static const symbol s_10_47[5] = { 'k', 'e', 'e', 'm', 'e' };
static const symbol s_10_48[4] = { 'k', 'e', 'e', 'n' };
static const symbol s_10_49[4] = { 'k', 'e', 'e', 's' };
static const symbol s_10_50[5] = { 'k', 'e', 'e', 't', 'a' };
static const symbol s_10_51[5] = { 'k', 'e', 'e', 't', 'e' };
static const symbol s_10_52[6] = { 'k', 'e', 'e', 'v', 'a', 'd' };
static const symbol s_10_53[5] = { 'k', 0xC3, 0xA4, 'i', 'a' };
static const symbol s_10_54[8] = { 'k', 0xC3, 0xA4, 'i', 'a', 'k', 's', 'e' };
static const symbol s_10_55[5] = { 'k', 0xC3, 0xA4, 'i', 'b' };
static const symbol s_10_56[5] = { 'k', 0xC3, 0xA4, 'i', 'd' };
static const symbol s_10_57[6] = { 'k', 0xC3, 0xA4, 'i', 'd', 'i' };
static const symbol s_10_58[6] = { 'k', 0xC3, 0xA4, 'i', 'k', 's' };
static const symbol s_10_59[8] = { 'k', 0xC3, 0xA4, 'i', 'k', 's', 'i', 'd' };
static const symbol s_10_60[9] = { 'k', 0xC3, 0xA4, 'i', 'k', 's', 'i', 'm', 'e' };
static const symbol s_10_61[8] = { 'k', 0xC3, 0xA4, 'i', 'k', 's', 'i', 'n' };
static const symbol s_10_62[9] = { 'k', 0xC3, 0xA4, 'i', 'k', 's', 'i', 't', 'e' };
static const symbol s_10_63[6] = { 'k', 0xC3, 0xA4, 'i', 'm', 'a' };
static const symbol s_10_64[8] = { 'k', 0xC3, 0xA4, 'i', 'm', 'a', 't', 'a' };
static const symbol s_10_65[6] = { 'k', 0xC3, 0xA4, 'i', 'm', 'e' };
static const symbol s_10_66[5] = { 'k', 0xC3, 0xA4, 'i', 'n' };
static const symbol s_10_67[5] = { 'k', 0xC3, 0xA4, 'i', 's' };
static const symbol s_10_68[6] = { 'k', 0xC3, 0xA4, 'i', 't', 'e' };
static const symbol s_10_69[7] = { 'k', 0xC3, 0xA4, 'i', 'v', 'a', 'd' };
static const symbol s_10_70[4] = { 'l', 'a', 'o', 'b' };
static const symbol s_10_71[4] = { 'l', 'a', 'o', 'd' };
static const symbol s_10_72[5] = { 'l', 'a', 'o', 'k', 's' };
static const symbol s_10_73[7] = { 'l', 'a', 'o', 'k', 's', 'i', 'd' };
static const symbol s_10_74[8] = { 'l', 'a', 'o', 'k', 's', 'i', 'm', 'e' };
static const symbol s_10_75[7] = { 'l', 'a', 'o', 'k', 's', 'i', 'n' };
static const symbol s_10_76[8] = { 'l', 'a', 'o', 'k', 's', 'i', 't', 'e' };
static const symbol s_10_77[5] = { 'l', 'a', 'o', 'm', 'e' };
static const symbol s_10_78[4] = { 'l', 'a', 'o', 'n' };
static const symbol s_10_79[5] = { 'l', 'a', 'o', 't', 'e' };
static const symbol s_10_80[6] = { 'l', 'a', 'o', 'v', 'a', 'd' };
static const symbol s_10_81[4] = { 'l', 'o', 'e', 'b' };
static const symbol s_10_82[4] = { 'l', 'o', 'e', 'd' };
static const symbol s_10_83[5] = { 'l', 'o', 'e', 'k', 's' };
static const symbol s_10_84[7] = { 'l', 'o', 'e', 'k', 's', 'i', 'd' };
static const symbol s_10_85[8] = { 'l', 'o', 'e', 'k', 's', 'i', 'm', 'e' };
static const symbol s_10_86[7] = { 'l', 'o', 'e', 'k', 's', 'i', 'n' };
static const symbol s_10_87[8] = { 'l', 'o', 'e', 'k', 's', 'i', 't', 'e' };
static const symbol s_10_88[5] = { 'l', 'o', 'e', 'm', 'e' };
static const symbol s_10_89[4] = { 'l', 'o', 'e', 'n' };
static const symbol s_10_90[5] = { 'l', 'o', 'e', 't', 'e' };
static const symbol s_10_91[6] = { 'l', 'o', 'e', 'v', 'a', 'd' };
static const symbol s_10_92[4] = { 'l', 'o', 'o', 'b' };
static const symbol s_10_93[4] = { 'l', 'o', 'o', 'd' };
static const symbol s_10_94[5] = { 'l', 'o', 'o', 'd', 'i' };
static const symbol s_10_95[5] = { 'l', 'o', 'o', 'k', 's' };
static const symbol s_10_96[7] = { 'l', 'o', 'o', 'k', 's', 'i', 'd' };
static const symbol s_10_97[8] = { 'l', 'o', 'o', 'k', 's', 'i', 'm', 'e' };
static const symbol s_10_98[7] = { 'l', 'o', 'o', 'k', 's', 'i', 'n' };
static const symbol s_10_99[8] = { 'l', 'o', 'o', 'k', 's', 'i', 't', 'e' };
static const symbol s_10_100[5] = { 'l', 'o', 'o', 'm', 'a' };
static const symbol s_10_101[7] = { 'l', 'o', 'o', 'm', 'a', 't', 'a' };
static const symbol s_10_102[5] = { 'l', 'o', 'o', 'm', 'e' };
static const symbol s_10_103[4] = { 'l', 'o', 'o', 'n' };
static const symbol s_10_104[5] = { 'l', 'o', 'o', 't', 'e' };
static const symbol s_10_105[6] = { 'l', 'o', 'o', 'v', 'a', 'd' };
static const symbol s_10_106[4] = { 'l', 'u', 'u', 'a' };
static const symbol s_10_107[7] = { 'l', 'u', 'u', 'a', 'k', 's', 'e' };
static const symbol s_10_108[4] = { 'l', 0xC3, 0xB5, 'i' };
static const symbol s_10_109[5] = { 'l', 0xC3, 0xB5, 'i', 'd' };
static const symbol s_10_110[6] = { 'l', 0xC3, 0xB5, 'i', 'm', 'e' };
static const symbol s_10_111[5] = { 'l', 0xC3, 0xB5, 'i', 'n' };
static const symbol s_10_112[6] = { 'l', 0xC3, 0xB5, 'i', 't', 'e' };
static const symbol s_10_113[6] = { 'l', 0xC3, 0xB6, 0xC3, 0xB6, 'b' };
static const symbol s_10_114[6] = { 'l', 0xC3, 0xB6, 0xC3, 0xB6, 'd' };
static const symbol s_10_115[10] = { 'l', 0xC3, 0xB6, 0xC3, 0xB6, 'd', 'a', 'k', 's', 'e' };
static const symbol s_10_116[7] = { 'l', 0xC3, 0xB6, 0xC3, 0xB6, 'd', 'i' };
static const symbol s_10_117[7] = { 'l', 0xC3, 0xB6, 0xC3, 0xB6, 'k', 's' };
static const symbol s_10_118[9] = { 'l', 0xC3, 0xB6, 0xC3, 0xB6, 'k', 's', 'i', 'd' };
static const symbol s_10_119[10] = { 'l', 0xC3, 0xB6, 0xC3, 0xB6, 'k', 's', 'i', 'm', 'e' };
static const symbol s_10_120[9] = { 'l', 0xC3, 0xB6, 0xC3, 0xB6, 'k', 's', 'i', 'n' };
static const symbol s_10_121[10] = { 'l', 0xC3, 0xB6, 0xC3, 0xB6, 'k', 's', 'i', 't', 'e' };
static const symbol s_10_122[7] = { 'l', 0xC3, 0xB6, 0xC3, 0xB6, 'm', 'a' };
static const symbol s_10_123[9] = { 'l', 0xC3, 0xB6, 0xC3, 0xB6, 'm', 'a', 't', 'a' };
static const symbol s_10_124[7] = { 'l', 0xC3, 0xB6, 0xC3, 0xB6, 'm', 'e' };
static const symbol s_10_125[6] = { 'l', 0xC3, 0xB6, 0xC3, 0xB6, 'n' };
static const symbol s_10_126[7] = { 'l', 0xC3, 0xB6, 0xC3, 0xB6, 't', 'e' };
static const symbol s_10_127[8] = { 'l', 0xC3, 0xB6, 0xC3, 0xB6, 'v', 'a', 'd' };
static const symbol s_10_128[6] = { 'l', 0xC3, 0xBC, 0xC3, 0xBC, 'a' };
static const symbol s_10_129[9] = { 'l', 0xC3, 0xBC, 0xC3, 0xBC, 'a', 'k', 's', 'e' };
static const symbol s_10_130[6] = { 'm', 0xC3, 0xBC, 0xC3, 0xBC, 'a' };
static const symbol s_10_131[9] = { 'm', 0xC3, 0xBC, 0xC3, 0xBC, 'a', 'k', 's', 'e' };
static const symbol s_10_132[6] = { 'm', 0xC3, 0xBC, 0xC3, 0xBC, 'b' };
static const symbol s_10_133[6] = { 'm', 0xC3, 0xBC, 0xC3, 0xBC, 'd' };
static const symbol s_10_134[7] = { 'm', 0xC3, 0xBC, 0xC3, 0xBC, 'd', 'i' };
static const symbol s_10_135[7] = { 'm', 0xC3, 0xBC, 0xC3, 0xBC, 'k', 's' };
static const symbol s_10_136[9] = { 'm', 0xC3, 0xBC, 0xC3, 0xBC, 'k', 's', 'i', 'd' };
static const symbol s_10_137[10] = { 'm', 0xC3, 0xBC, 0xC3, 0xBC, 'k', 's', 'i', 'm', 'e' };
static const symbol s_10_138[9] = { 'm', 0xC3, 0xBC, 0xC3, 0xBC, 'k', 's', 'i', 'n' };
static const symbol s_10_139[10] = { 'm', 0xC3, 0xBC, 0xC3, 0xBC, 'k', 's', 'i', 't', 'e' };
static const symbol s_10_140[7] = { 'm', 0xC3, 0xBC, 0xC3, 0xBC, 'm', 'a' };
static const symbol s_10_141[9] = { 'm', 0xC3, 0xBC, 0xC3, 0xBC, 'm', 'a', 't', 'a' };
static const symbol s_10_142[7] = { 'm', 0xC3, 0xBC, 0xC3, 0xBC, 'm', 'e' };
static const symbol s_10_143[6] = { 'm', 0xC3, 0xBC, 0xC3, 0xBC, 'n' };
static const symbol s_10_144[6] = { 'm', 0xC3, 0xBC, 0xC3, 0xBC, 's' };
static const symbol s_10_145[7] = { 'm', 0xC3, 0xBC, 0xC3, 0xBC, 't', 'e' };
static const symbol s_10_146[8] = { 'm', 0xC3, 0xBC, 0xC3, 0xBC, 'v', 'a', 'd' };
static const symbol s_10_147[5] = { 'n', 0xC3, 0xA4, 'e', 'b' };
static const symbol s_10_148[5] = { 'n', 0xC3, 0xA4, 'e', 'd' };
static const symbol s_10_149[6] = { 'n', 0xC3, 0xA4, 'e', 'k', 's' };
static const symbol s_10_150[8] = { 'n', 0xC3, 0xA4, 'e', 'k', 's', 'i', 'd' };
static const symbol s_10_151[9] = { 'n', 0xC3, 0xA4, 'e', 'k', 's', 'i', 'm', 'e' };
static const symbol s_10_152[8] = { 'n', 0xC3, 0xA4, 'e', 'k', 's', 'i', 'n' };
static const symbol s_10_153[9] = { 'n', 0xC3, 0xA4, 'e', 'k', 's', 'i', 't', 'e' };
static const symbol s_10_154[6] = { 'n', 0xC3, 0xA4, 'e', 'm', 'e' };
static const symbol s_10_155[5] = { 'n', 0xC3, 0xA4, 'e', 'n' };
static const symbol s_10_156[6] = { 'n', 0xC3, 0xA4, 'e', 't', 'e' };
static const symbol s_10_157[7] = { 'n', 0xC3, 0xA4, 'e', 'v', 'a', 'd' };
static const symbol s_10_158[7] = { 'n', 0xC3, 0xA4, 'g', 'e', 'm', 'a' };
static const symbol s_10_159[9] = { 'n', 0xC3, 0xA4, 'g', 'e', 'm', 'a', 't', 'a' };
static const symbol s_10_160[5] = { 'n', 0xC3, 0xA4, 'h', 'a' };
static const symbol s_10_161[8] = { 'n', 0xC3, 0xA4, 'h', 'a', 'k', 's', 'e' };
static const symbol s_10_162[6] = { 'n', 0xC3, 0xA4, 'h', 't', 'i' };
static const symbol s_10_163[5] = { 'p', 0xC3, 0xB5, 'e', 'b' };
static const symbol s_10_164[5] = { 'p', 0xC3, 0xB5, 'e', 'd' };
static const symbol s_10_165[6] = { 'p', 0xC3, 0xB5, 'e', 'k', 's' };
static const symbol s_10_166[8] = { 'p', 0xC3, 0xB5, 'e', 'k', 's', 'i', 'd' };
static const symbol s_10_167[9] = { 'p', 0xC3, 0xB5, 'e', 'k', 's', 'i', 'm', 'e' };
static const symbol s_10_168[8] = { 'p', 0xC3, 0xB5, 'e', 'k', 's', 'i', 'n' };
static const symbol s_10_169[9] = { 'p', 0xC3, 0xB5, 'e', 'k', 's', 'i', 't', 'e' };
static const symbol s_10_170[6] = { 'p', 0xC3, 0xB5, 'e', 'm', 'e' };
static const symbol s_10_171[5] = { 'p', 0xC3, 0xB5, 'e', 'n' };
static const symbol s_10_172[6] = { 'p', 0xC3, 0xB5, 'e', 't', 'e' };
static const symbol s_10_173[7] = { 'p', 0xC3, 0xB5, 'e', 'v', 'a', 'd' };
static const symbol s_10_174[4] = { 's', 'a', 'a', 'b' };
static const symbol s_10_175[4] = { 's', 'a', 'a', 'd' };
static const symbol s_10_176[5] = { 's', 'a', 'a', 'd', 'a' };
static const symbol s_10_177[8] = { 's', 'a', 'a', 'd', 'a', 'k', 's', 'e' };
static const symbol s_10_178[5] = { 's', 'a', 'a', 'd', 'i' };
static const symbol s_10_179[5] = { 's', 'a', 'a', 'k', 's' };
static const symbol s_10_180[7] = { 's', 'a', 'a', 'k', 's', 'i', 'd' };
static const symbol s_10_181[8] = { 's', 'a', 'a', 'k', 's', 'i', 'm', 'e' };
static const symbol s_10_182[7] = { 's', 'a', 'a', 'k', 's', 'i', 'n' };
static const symbol s_10_183[8] = { 's', 'a', 'a', 'k', 's', 'i', 't', 'e' };
static const symbol s_10_184[5] = { 's', 'a', 'a', 'm', 'a' };
static const symbol s_10_185[7] = { 's', 'a', 'a', 'm', 'a', 't', 'a' };
static const symbol s_10_186[5] = { 's', 'a', 'a', 'm', 'e' };
static const symbol s_10_187[4] = { 's', 'a', 'a', 'n' };
static const symbol s_10_188[5] = { 's', 'a', 'a', 't', 'e' };
static const symbol s_10_189[6] = { 's', 'a', 'a', 'v', 'a', 'd' };
static const symbol s_10_190[3] = { 's', 'a', 'i' };
static const symbol s_10_191[4] = { 's', 'a', 'i', 'd' };
static const symbol s_10_192[5] = { 's', 'a', 'i', 'm', 'e' };
static const symbol s_10_193[4] = { 's', 'a', 'i', 'n' };
static const symbol s_10_194[5] = { 's', 'a', 'i', 't', 'e' };
static const symbol s_10_195[4] = { 's', 0xC3, 0xB5, 'i' };
static const symbol s_10_196[5] = { 's', 0xC3, 0xB5, 'i', 'd' };
static const symbol s_10_197[6] = { 's', 0xC3, 0xB5, 'i', 'm', 'e' };
static const symbol s_10_198[5] = { 's', 0xC3, 0xB5, 'i', 'n' };
static const symbol s_10_199[6] = { 's', 0xC3, 0xB5, 'i', 't', 'e' };
static const symbol s_10_200[6] = { 's', 0xC3, 0xB6, 0xC3, 0xB6, 'b' };
static const symbol s_10_201[6] = { 's', 0xC3, 0xB6, 0xC3, 0xB6, 'd' };
static const symbol s_10_202[10] = { 's', 0xC3, 0xB6, 0xC3, 0xB6, 'd', 'a', 'k', 's', 'e' };
static const symbol s_10_203[7] = { 's', 0xC3, 0xB6, 0xC3, 0xB6, 'd', 'i' };
static const symbol s_10_204[7] = { 's', 0xC3, 0xB6, 0xC3, 0xB6, 'k', 's' };
static const symbol s_10_205[9] = { 's', 0xC3, 0xB6, 0xC3, 0xB6, 'k', 's', 'i', 'd' };
static const symbol s_10_206[10] = { 's', 0xC3, 0xB6, 0xC3, 0xB6, 'k', 's', 'i', 'm', 'e' };
static const symbol s_10_207[9] = { 's', 0xC3, 0xB6, 0xC3, 0xB6, 'k', 's', 'i', 'n' };
static const symbol s_10_208[10] = { 's', 0xC3, 0xB6, 0xC3, 0xB6, 'k', 's', 'i', 't', 'e' };
static const symbol s_10_209[7] = { 's', 0xC3, 0xB6, 0xC3, 0xB6, 'm', 'a' };
static const symbol s_10_210[9] = { 's', 0xC3, 0xB6, 0xC3, 0xB6, 'm', 'a', 't', 'a' };
static const symbol s_10_211[7] = { 's', 0xC3, 0xB6, 0xC3, 0xB6, 'm', 'e' };
static const symbol s_10_212[6] = { 's', 0xC3, 0xB6, 0xC3, 0xB6, 'n' };
static const symbol s_10_213[7] = { 's', 0xC3, 0xB6, 0xC3, 0xB6, 't', 'e' };
static const symbol s_10_214[8] = { 's', 0xC3, 0xB6, 0xC3, 0xB6, 'v', 'a', 'd' };
static const symbol s_10_215[6] = { 's', 0xC3, 0xBC, 0xC3, 0xBC, 'a' };
static const symbol s_10_216[9] = { 's', 0xC3, 0xBC, 0xC3, 0xBC, 'a', 'k', 's', 'e' };
static const symbol s_10_217[4] = { 't', 'e', 'e', 'b' };
static const symbol s_10_218[4] = { 't', 'e', 'e', 'd' };
static const symbol s_10_219[5] = { 't', 'e', 'e', 'k', 's' };
static const symbol s_10_220[7] = { 't', 'e', 'e', 'k', 's', 'i', 'd' };
static const symbol s_10_221[8] = { 't', 'e', 'e', 'k', 's', 'i', 'm', 'e' };
static const symbol s_10_222[7] = { 't', 'e', 'e', 'k', 's', 'i', 'n' };
static const symbol s_10_223[8] = { 't', 'e', 'e', 'k', 's', 'i', 't', 'e' };
static const symbol s_10_224[5] = { 't', 'e', 'e', 'm', 'e' };
static const symbol s_10_225[4] = { 't', 'e', 'e', 'n' };
static const symbol s_10_226[5] = { 't', 'e', 'e', 't', 'e' };
static const symbol s_10_227[6] = { 't', 'e', 'e', 'v', 'a', 'd' };
static const symbol s_10_228[6] = { 't', 'e', 'g', 'e', 'm', 'a' };
static const symbol s_10_229[8] = { 't', 'e', 'g', 'e', 'm', 'a', 't', 'a' };
static const symbol s_10_230[4] = { 't', 'e', 'h', 'a' };
static const symbol s_10_231[7] = { 't', 'e', 'h', 'a', 'k', 's', 'e' };
static const symbol s_10_232[5] = { 't', 'e', 'h', 't', 'i' };
static const symbol s_10_233[4] = { 't', 'o', 'o', 'b' };
static const symbol s_10_234[4] = { 't', 'o', 'o', 'd' };
static const symbol s_10_235[5] = { 't', 'o', 'o', 'd', 'i' };
static const symbol s_10_236[5] = { 't', 'o', 'o', 'k', 's' };
static const symbol s_10_237[7] = { 't', 'o', 'o', 'k', 's', 'i', 'd' };
static const symbol s_10_238[8] = { 't', 'o', 'o', 'k', 's', 'i', 'm', 'e' };
static const symbol s_10_239[7] = { 't', 'o', 'o', 'k', 's', 'i', 'n' };
static const symbol s_10_240[8] = { 't', 'o', 'o', 'k', 's', 'i', 't', 'e' };
static const symbol s_10_241[5] = { 't', 'o', 'o', 'm', 'a' };
static const symbol s_10_242[7] = { 't', 'o', 'o', 'm', 'a', 't', 'a' };
static const symbol s_10_243[5] = { 't', 'o', 'o', 'm', 'e' };
static const symbol s_10_244[4] = { 't', 'o', 'o', 'n' };
static const symbol s_10_245[5] = { 't', 'o', 'o', 't', 'e' };
static const symbol s_10_246[6] = { 't', 'o', 'o', 'v', 'a', 'd' };
static const symbol s_10_247[4] = { 't', 'u', 'u', 'a' };
static const symbol s_10_248[7] = { 't', 'u', 'u', 'a', 'k', 's', 'e' };
static const symbol s_10_249[4] = { 't', 0xC3, 0xB5, 'i' };
static const symbol s_10_250[5] = { 't', 0xC3, 0xB5, 'i', 'd' };
static const symbol s_10_251[6] = { 't', 0xC3, 0xB5, 'i', 'm', 'e' };
static const symbol s_10_252[5] = { 't', 0xC3, 0xB5, 'i', 'n' };
static const symbol s_10_253[6] = { 't', 0xC3, 0xB5, 'i', 't', 'e' };
static const symbol s_10_254[4] = { 'v', 'i', 'i', 'a' };
static const symbol s_10_255[7] = { 'v', 'i', 'i', 'a', 'k', 's', 'e' };
static const symbol s_10_256[4] = { 'v', 'i', 'i', 'b' };
static const symbol s_10_257[4] = { 'v', 'i', 'i', 'd' };
static const symbol s_10_258[5] = { 'v', 'i', 'i', 'd', 'i' };
static const symbol s_10_259[5] = { 'v', 'i', 'i', 'k', 's' };
static const symbol s_10_260[7] = { 'v', 'i', 'i', 'k', 's', 'i', 'd' };
static const symbol s_10_261[8] = { 'v', 'i', 'i', 'k', 's', 'i', 'm', 'e' };
static const symbol s_10_262[7] = { 'v', 'i', 'i', 'k', 's', 'i', 'n' };
static const symbol s_10_263[8] = { 'v', 'i', 'i', 'k', 's', 'i', 't', 'e' };
static const symbol s_10_264[5] = { 'v', 'i', 'i', 'm', 'a' };
static const symbol s_10_265[7] = { 'v', 'i', 'i', 'm', 'a', 't', 'a' };
static const symbol s_10_266[5] = { 'v', 'i', 'i', 'm', 'e' };
static const symbol s_10_267[4] = { 'v', 'i', 'i', 'n' };
static const symbol s_10_268[7] = { 'v', 'i', 'i', 's', 'i', 'm', 'e' };
static const symbol s_10_269[6] = { 'v', 'i', 'i', 's', 'i', 'n' };
static const symbol s_10_270[7] = { 'v', 'i', 'i', 's', 'i', 't', 'e' };
static const symbol s_10_271[5] = { 'v', 'i', 'i', 't', 'e' };
static const symbol s_10_272[6] = { 'v', 'i', 'i', 'v', 'a', 'd' };
static const symbol s_10_273[5] = { 'v', 0xC3, 0xB5, 'i', 'b' };
static const symbol s_10_274[5] = { 'v', 0xC3, 0xB5, 'i', 'd' };
static const symbol s_10_275[6] = { 'v', 0xC3, 0xB5, 'i', 'd', 'a' };
static const symbol s_10_276[9] = { 'v', 0xC3, 0xB5, 'i', 'd', 'a', 'k', 's', 'e' };
static const symbol s_10_277[6] = { 'v', 0xC3, 0xB5, 'i', 'd', 'i' };
static const symbol s_10_278[6] = { 'v', 0xC3, 0xB5, 'i', 'k', 's' };
static const symbol s_10_279[8] = { 'v', 0xC3, 0xB5, 'i', 'k', 's', 'i', 'd' };
static const symbol s_10_280[9] = { 'v', 0xC3, 0xB5, 'i', 'k', 's', 'i', 'm', 'e' };
static const symbol s_10_281[8] = { 'v', 0xC3, 0xB5, 'i', 'k', 's', 'i', 'n' };
static const symbol s_10_282[9] = { 'v', 0xC3, 0xB5, 'i', 'k', 's', 'i', 't', 'e' };
static const symbol s_10_283[6] = { 'v', 0xC3, 0xB5, 'i', 'm', 'a' };
static const symbol s_10_284[8] = { 'v', 0xC3, 0xB5, 'i', 'm', 'a', 't', 'a' };
static const symbol s_10_285[6] = { 'v', 0xC3, 0xB5, 'i', 'm', 'e' };
static const symbol s_10_286[5] = { 'v', 0xC3, 0xB5, 'i', 'n' };
static const symbol s_10_287[5] = { 'v', 0xC3, 0xB5, 'i', 's' };
static const symbol s_10_288[6] = { 'v', 0xC3, 0xB5, 'i', 't', 'e' };
static const symbol s_10_289[7] = { 'v', 0xC3, 0xB5, 'i', 'v', 'a', 'd' };
static const struct among a_10[290] = {
{ 4, s_10_0, 0, 1, 0},
{ 4, s_10_1, 0, 1, 0},
{ 8, s_10_2, -1, 1, 0},
{ 5, s_10_3, 0, 1, 0},
{ 7, s_10_4, -1, 1, 0},
{ 5, s_10_5, 0, 1, 0},
{ 4, s_10_6, 0, 1, 0},
{ 5, s_10_7, 0, 1, 0},
{ 6, s_10_8, 0, 1, 0},
{ 4, s_10_9, 0, 1, 0},
{ 7, s_10_10, -1, 1, 0},
{ 4, s_10_11, 0, 12, 0},
{ 5, s_10_12, -1, 12, 0},
{ 6, s_10_13, -2, 12, 0},
{ 5, s_10_14, -3, 12, 0},
{ 6, s_10_15, -4, 12, 0},
{ 6, s_10_16, 0, 12, 0},
{ 6, s_10_17, 0, 12, 0},
{ 7, s_10_18, -1, 12, 0},
{ 10, s_10_19, -1, 12, 0},
{ 7, s_10_20, -3, 12, 0},
{ 7, s_10_21, 0, 12, 0},
{ 9, s_10_22, -1, 12, 0},
{ 10, s_10_23, -2, 12, 0},
{ 9, s_10_24, -3, 12, 0},
{ 10, s_10_25, -4, 12, 0},
{ 7, s_10_26, 0, 12, 0},
{ 9, s_10_27, -1, 12, 0},
{ 7, s_10_28, 0, 12, 0},
{ 6, s_10_29, 0, 12, 0},
{ 7, s_10_30, 0, 12, 0},
{ 8, s_10_31, 0, 12, 0},
{ 4, s_10_32, 0, 1, 0},
{ 5, s_10_33, -1, 1, 0},
{ 6, s_10_34, -2, 1, 0},
{ 5, s_10_35, -3, 1, 0},
{ 6, s_10_36, -4, 1, 0},
{ 4, s_10_37, 0, 4, 0},
{ 4, s_10_38, 0, 4, 0},
{ 8, s_10_39, -1, 4, 0},
{ 5, s_10_40, 0, 4, 0},
{ 7, s_10_41, -1, 4, 0},
{ 8, s_10_42, -2, 4, 0},
{ 7, s_10_43, -3, 4, 0},
{ 8, s_10_44, -4, 4, 0},
{ 5, s_10_45, 0, 4, 0},
{ 7, s_10_46, -1, 4, 0},
{ 5, s_10_47, 0, 4, 0},
{ 4, s_10_48, 0, 4, 0},
{ 4, s_10_49, 0, 4, 0},
{ 5, s_10_50, 0, 4, 0},
{ 5, s_10_51, 0, 4, 0},
{ 6, s_10_52, 0, 4, 0},
{ 5, s_10_53, 0, 8, 0},
{ 8, s_10_54, -1, 8, 0},
{ 5, s_10_55, 0, 8, 0},
{ 5, s_10_56, 0, 8, 0},
{ 6, s_10_57, -1, 8, 0},
{ 6, s_10_58, 0, 8, 0},
{ 8, s_10_59, -1, 8, 0},
{ 9, s_10_60, -2, 8, 0},
{ 8, s_10_61, -3, 8, 0},
{ 9, s_10_62, -4, 8, 0},
{ 6, s_10_63, 0, 8, 0},
{ 8, s_10_64, -1, 8, 0},
{ 6, s_10_65, 0, 8, 0},
{ 5, s_10_66, 0, 8, 0},
{ 5, s_10_67, 0, 8, 0},
{ 6, s_10_68, 0, 8, 0},
{ 7, s_10_69, 0, 8, 0},
{ 4, s_10_70, 0, 16, 0},
{ 4, s_10_71, 0, 16, 0},
{ 5, s_10_72, 0, 16, 0},
{ 7, s_10_73, -1, 16, 0},
{ 8, s_10_74, -2, 16, 0},
{ 7, s_10_75, -3, 16, 0},
{ 8, s_10_76, -4, 16, 0},
{ 5, s_10_77, 0, 16, 0},
{ 4, s_10_78, 0, 16, 0},
{ 5, s_10_79, 0, 16, 0},
{ 6, s_10_80, 0, 16, 0},
{ 4, s_10_81, 0, 14, 0},
{ 4, s_10_82, 0, 14, 0},
{ 5, s_10_83, 0, 14, 0},
{ 7, s_10_84, -1, 14, 0},
{ 8, s_10_85, -2, 14, 0},
{ 7, s_10_86, -3, 14, 0},
{ 8, s_10_87, -4, 14, 0},
{ 5, s_10_88, 0, 14, 0},
{ 4, s_10_89, 0, 14, 0},
{ 5, s_10_90, 0, 14, 0},
{ 6, s_10_91, 0, 14, 0},
{ 4, s_10_92, 0, 7, 0},
{ 4, s_10_93, 0, 7, 0},
{ 5, s_10_94, -1, 7, 0},
{ 5, s_10_95, 0, 7, 0},
{ 7, s_10_96, -1, 7, 0},
{ 8, s_10_97, -2, 7, 0},
{ 7, s_10_98, -3, 7, 0},
{ 8, s_10_99, -4, 7, 0},
{ 5, s_10_100, 0, 7, 0},
{ 7, s_10_101, -1, 7, 0},
{ 5, s_10_102, 0, 7, 0},
{ 4, s_10_103, 0, 7, 0},
{ 5, s_10_104, 0, 7, 0},
{ 6, s_10_105, 0, 7, 0},
{ 4, s_10_106, 0, 7, 0},
{ 7, s_10_107, -1, 7, 0},
{ 4, s_10_108, 0, 6, 0},
{ 5, s_10_109, -1, 6, 0},
{ 6, s_10_110, -2, 6, 0},
{ 5, s_10_111, -3, 6, 0},
{ 6, s_10_112, -4, 6, 0},
{ 6, s_10_113, 0, 5, 0},
{ 6, s_10_114, 0, 5, 0},
{ 10, s_10_115, -1, 5, 0},
{ 7, s_10_116, -2, 5, 0},
{ 7, s_10_117, 0, 5, 0},
{ 9, s_10_118, -1, 5, 0},
{ 10, s_10_119, -2, 5, 0},
{ 9, s_10_120, -3, 5, 0},
{ 10, s_10_121, -4, 5, 0},
{ 7, s_10_122, 0, 5, 0},
{ 9, s_10_123, -1, 5, 0},
{ 7, s_10_124, 0, 5, 0},
{ 6, s_10_125, 0, 5, 0},
{ 7, s_10_126, 0, 5, 0},
{ 8, s_10_127, 0, 5, 0},
{ 6, s_10_128, 0, 5, 0},
{ 9, s_10_129, -1, 5, 0},
{ 6, s_10_130, 0, 13, 0},
{ 9, s_10_131, -1, 13, 0},
{ 6, s_10_132, 0, 13, 0},
{ 6, s_10_133, 0, 13, 0},
{ 7, s_10_134, -1, 13, 0},
{ 7, s_10_135, 0, 13, 0},
{ 9, s_10_136, -1, 13, 0},
{ 10, s_10_137, -2, 13, 0},
{ 9, s_10_138, -3, 13, 0},
{ 10, s_10_139, -4, 13, 0},
{ 7, s_10_140, 0, 13, 0},
{ 9, s_10_141, -1, 13, 0},
{ 7, s_10_142, 0, 13, 0},
{ 6, s_10_143, 0, 13, 0},
{ 6, s_10_144, 0, 13, 0},
{ 7, s_10_145, 0, 13, 0},
{ 8, s_10_146, 0, 13, 0},
{ 5, s_10_147, 0, 18, 0},
{ 5, s_10_148, 0, 18, 0},
{ 6, s_10_149, 0, 18, 0},
{ 8, s_10_150, -1, 18, 0},
{ 9, s_10_151, -2, 18, 0},
{ 8, s_10_152, -3, 18, 0},
{ 9, s_10_153, -4, 18, 0},
{ 6, s_10_154, 0, 18, 0},
{ 5, s_10_155, 0, 18, 0},
{ 6, s_10_156, 0, 18, 0},
{ 7, s_10_157, 0, 18, 0},
{ 7, s_10_158, 0, 18, 0},
{ 9, s_10_159, -1, 18, 0},
{ 5, s_10_160, 0, 18, 0},
{ 8, s_10_161, -1, 18, 0},
{ 6, s_10_162, 0, 18, 0},
{ 5, s_10_163, 0, 15, 0},
{ 5, s_10_164, 0, 15, 0},
{ 6, s_10_165, 0, 15, 0},
{ 8, s_10_166, -1, 15, 0},
{ 9, s_10_167, -2, 15, 0},
{ 8, s_10_168, -3, 15, 0},
{ 9, s_10_169, -4, 15, 0},
{ 6, s_10_170, 0, 15, 0},
{ 5, s_10_171, 0, 15, 0},
{ 6, s_10_172, 0, 15, 0},
{ 7, s_10_173, 0, 15, 0},
{ 4, s_10_174, 0, 2, 0},
{ 4, s_10_175, 0, 2, 0},
{ 5, s_10_176, -1, 2, 0},
{ 8, s_10_177, -1, 2, 0},
{ 5, s_10_178, -3, 2, 0},
{ 5, s_10_179, 0, 2, 0},
{ 7, s_10_180, -1, 2, 0},
{ 8, s_10_181, -2, 2, 0},
{ 7, s_10_182, -3, 2, 0},
{ 8, s_10_183, -4, 2, 0},
{ 5, s_10_184, 0, 2, 0},
{ 7, s_10_185, -1, 2, 0},
{ 5, s_10_186, 0, 2, 0},
{ 4, s_10_187, 0, 2, 0},
{ 5, s_10_188, 0, 2, 0},
{ 6, s_10_189, 0, 2, 0},
{ 3, s_10_190, 0, 2, 0},
{ 4, s_10_191, -1, 2, 0},
{ 5, s_10_192, -2, 2, 0},
{ 4, s_10_193, -3, 2, 0},
{ 5, s_10_194, -4, 2, 0},
{ 4, s_10_195, 0, 9, 0},
{ 5, s_10_196, -1, 9, 0},
{ 6, s_10_197, -2, 9, 0},
{ 5, s_10_198, -3, 9, 0},
{ 6, s_10_199, -4, 9, 0},
{ 6, s_10_200, 0, 9, 0},
{ 6, s_10_201, 0, 9, 0},
{ 10, s_10_202, -1, 9, 0},
{ 7, s_10_203, -2, 9, 0},
{ 7, s_10_204, 0, 9, 0},
{ 9, s_10_205, -1, 9, 0},
{ 10, s_10_206, -2, 9, 0},
{ 9, s_10_207, -3, 9, 0},
{ 10, s_10_208, -4, 9, 0},
{ 7, s_10_209, 0, 9, 0},
{ 9, s_10_210, -1, 9, 0},
{ 7, s_10_211, 0, 9, 0},
{ 6, s_10_212, 0, 9, 0},
{ 7, s_10_213, 0, 9, 0},
{ 8, s_10_214, 0, 9, 0},
{ 6, s_10_215, 0, 9, 0},
{ 9, s_10_216, -1, 9, 0},
{ 4, s_10_217, 0, 17, 0},
{ 4, s_10_218, 0, 17, 0},
{ 5, s_10_219, 0, 17, 0},
{ 7, s_10_220, -1, 17, 0},
{ 8, s_10_221, -2, 17, 0},
{ 7, s_10_222, -3, 17, 0},
{ 8, s_10_223, -4, 17, 0},
{ 5, s_10_224, 0, 17, 0},
{ 4, s_10_225, 0, 17, 0},
{ 5, s_10_226, 0, 17, 0},
{ 6, s_10_227, 0, 17, 0},
{ 6, s_10_228, 0, 17, 0},
{ 8, s_10_229, -1, 17, 0},
{ 4, s_10_230, 0, 17, 0},
{ 7, s_10_231, -1, 17, 0},
{ 5, s_10_232, 0, 17, 0},
{ 4, s_10_233, 0, 10, 0},
{ 4, s_10_234, 0, 10, 0},
{ 5, s_10_235, -1, 10, 0},
{ 5, s_10_236, 0, 10, 0},
{ 7, s_10_237, -1, 10, 0},
{ 8, s_10_238, -2, 10, 0},
{ 7, s_10_239, -3, 10, 0},
{ 8, s_10_240, -4, 10, 0},
{ 5, s_10_241, 0, 10, 0},
{ 7, s_10_242, -1, 10, 0},
{ 5, s_10_243, 0, 10, 0},
{ 4, s_10_244, 0, 10, 0},
{ 5, s_10_245, 0, 10, 0},
{ 6, s_10_246, 0, 10, 0},
{ 4, s_10_247, 0, 10, 0},
{ 7, s_10_248, -1, 10, 0},
{ 4, s_10_249, 0, 10, 0},
{ 5, s_10_250, -1, 10, 0},
{ 6, s_10_251, -2, 10, 0},
{ 5, s_10_252, -3, 10, 0},
{ 6, s_10_253, -4, 10, 0},
{ 4, s_10_254, 0, 3, 0},
{ 7, s_10_255, -1, 3, 0},
{ 4, s_10_256, 0, 3, 0},
{ 4, s_10_257, 0, 3, 0},
{ 5, s_10_258, -1, 3, 0},
{ 5, s_10_259, 0, 3, 0},
{ 7, s_10_260, -1, 3, 0},
{ 8, s_10_261, -2, 3, 0},
{ 7, s_10_262, -3, 3, 0},
{ 8, s_10_263, -4, 3, 0},
{ 5, s_10_264, 0, 3, 0},
{ 7, s_10_265, -1, 3, 0},
{ 5, s_10_266, 0, 3, 0},
{ 4, s_10_267, 0, 3, 0},
{ 7, s_10_268, 0, 3, 0},
{ 6, s_10_269, 0, 3, 0},
{ 7, s_10_270, 0, 3, 0},
{ 5, s_10_271, 0, 3, 0},
{ 6, s_10_272, 0, 3, 0},
{ 5, s_10_273, 0, 11, 0},
{ 5, s_10_274, 0, 11, 0},
{ 6, s_10_275, -1, 11, 0},
{ 9, s_10_276, -1, 11, 0},
{ 6, s_10_277, -3, 11, 0},
{ 6, s_10_278, 0, 11, 0},
{ 8, s_10_279, -1, 11, 0},
{ 9, s_10_280, -2, 11, 0},
{ 8, s_10_281, -3, 11, 0},
{ 9, s_10_282, -4, 11, 0},
{ 6, s_10_283, 0, 11, 0},
{ 8, s_10_284, -1, 11, 0},
{ 6, s_10_285, 0, 11, 0},
{ 5, s_10_286, 0, 11, 0},
{ 5, s_10_287, 0, 11, 0},
{ 6, s_10_288, 0, 11, 0},
{ 7, s_10_289, 0, 11, 0}
};

static const unsigned char g_V1[] = { 17, 65, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 48, 8 };

static const unsigned char g_RV[] = { 17, 65, 16 };

static const unsigned char g_KI[] = { 117, 66, 6, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 128, 0, 0, 0, 16 };

static const unsigned char g_GI[] = { 21, 123, 243, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 48, 8 };

static int r_mark_regions(struct SN_env * z) {
    ((SN_local *)z)->i_p1 = z->l;
    {
        int ret = out_grouping_U(z, g_V1, 97, 252, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    {
        int ret = in_grouping_U(z, g_V1, 97, 252, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    ((SN_local *)z)->i_p1 = z->c;
    return 1;
}

static int r_emphasis(struct SN_env * z) {
    int among_var;
    {
        int v_1;
        if (z->c < ((SN_local *)z)->i_p1) return 0;
        v_1 = z->lb; z->lb = ((SN_local *)z)->i_p1;
        z->ket = z->c;
        if (z->c - 1 <= z->lb || z->p[z->c - 1] != 105) { z->lb = v_1; return 0; }
        among_var = find_among_b(z, a_0, 2, 0);
        if (!among_var) { z->lb = v_1; return 0; }
        z->bra = z->c;
        z->lb = v_1;
    }
    {
        int v_2 = z->l - z->c;
        {
            int ret = skip_b_utf8(z->p, z->c, z->lb, 4);
            if (ret < 0) return 0;
            z->c = ret;
        }
        z->c = z->l - v_2;
    }
    switch (among_var) {
        case 1:
            {
                int v_3 = z->l - z->c;
                if (in_grouping_b_U(z, g_GI, 97, 252, 0)) return 0;
                z->c = z->l - v_3;
                {
                    int v_4 = z->l - z->c;
                    {
                        int ret = r_LONGV(z);
                        if (ret == 0) goto lab0;
                        if (ret < 0) return ret;
                    }
                    return 0;
                lab0:
                    z->c = z->l - v_4;
                }
            }
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            if (in_grouping_b_U(z, g_KI, 98, 382, 0)) return 0;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_verb(struct SN_env * z) {
    int among_var;
    {
        int v_1;
        if (z->c < ((SN_local *)z)->i_p1) return 0;
        v_1 = z->lb; z->lb = ((SN_local *)z)->i_p1;
        z->ket = z->c;
        if (z->c <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((540726 >> (z->p[z->c - 1] & 0x1f)) & 1)) { z->lb = v_1; return 0; }
        among_var = find_among_b(z, a_1, 21, 0);
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
            {
                int ret = slice_from_s(z, 1, s_0);
                if (ret < 0) return ret;
            }
            break;
        case 3:
            if (in_grouping_b_U(z, g_V1, 97, 252, 0)) return 0;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_LONGV(struct SN_env * z) {
    return find_among_b(z, a_2, 9, 0) != 0;
}

static int r_i_plural(struct SN_env * z) {
    {
        int v_1;
        if (z->c < ((SN_local *)z)->i_p1) return 0;
        v_1 = z->lb; z->lb = ((SN_local *)z)->i_p1;
        z->ket = z->c;
        if (z->c <= z->lb || z->p[z->c - 1] != 'i') { z->lb = v_1; return 0; }
        z->c--;
        z->bra = z->c;
        z->lb = v_1;
    }
    if (in_grouping_b_U(z, g_RV, 97, 117, 0)) return 0;
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_special_noun_endings(struct SN_env * z) {
    int among_var;
    {
        int v_1;
        if (z->c < ((SN_local *)z)->i_p1) return 0;
        v_1 = z->lb; z->lb = ((SN_local *)z)->i_p1;
        z->ket = z->c;
        if (z->c - 3 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((1049120 >> (z->p[z->c - 1] & 0x1f)) & 1)) { z->lb = v_1; return 0; }
        among_var = find_among_b(z, a_3, 12, 0);
        if (!among_var) { z->lb = v_1; return 0; }
        z->bra = z->c;
        z->lb = v_1;
    }
    switch (among_var) {
        case 1:
            {
                int ret = slice_from_s(z, 4, s_1);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {
                int ret = slice_from_s(z, 4, s_2);
                if (ret < 0) return ret;
            }
            break;
        case 3:
            {
                int ret = slice_from_s(z, 4, s_3);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_case_ending(struct SN_env * z) {
    int among_var;
    {
        int v_1;
        if (z->c < ((SN_local *)z)->i_p1) return 0;
        v_1 = z->lb; z->lb = ((SN_local *)z)->i_p1;
        z->ket = z->c;
        if (z->c <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((1576994 >> (z->p[z->c - 1] & 0x1f)) & 1)) { z->lb = v_1; return 0; }
        among_var = find_among_b(z, a_4, 10, 0);
        if (!among_var) { z->lb = v_1; return 0; }
        z->bra = z->c;
        z->lb = v_1;
    }
    switch (among_var) {
        case 1:
            do {
                int v_2 = z->l - z->c;
                if (in_grouping_b_U(z, g_RV, 97, 117, 0)) goto lab0;
                break;
            lab0:
                z->c = z->l - v_2;
                {
                    int ret = r_LONGV(z);
                    if (ret <= 0) return ret;
                }
            } while (0);
            break;
        case 2:
            {
                int v_3 = z->l - z->c;
                {
                    int ret = skip_b_utf8(z->p, z->c, z->lb, 4);
                    if (ret < 0) return 0;
                    z->c = ret;
                }
                z->c = z->l - v_3;
            }
            break;
    }
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_plural_three_first_cases(struct SN_env * z) {
    int among_var;
    {
        int v_1;
        if (z->c < ((SN_local *)z)->i_p1) return 0;
        v_1 = z->lb; z->lb = ((SN_local *)z)->i_p1;
        z->ket = z->c;
        if (z->c <= z->lb || (z->p[z->c - 1] != 100 && z->p[z->c - 1] != 101)) { z->lb = v_1; return 0; }
        among_var = find_among_b(z, a_6, 7, 0);
        if (!among_var) { z->lb = v_1; return 0; }
        z->bra = z->c;
        z->lb = v_1;
    }
    switch (among_var) {
        case 1:
            {
                int ret = slice_from_s(z, 3, s_4);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {
                int v_2 = z->l - z->c;
                {
                    int ret = r_LONGV(z);
                    if (ret == 0) goto lab0;
                    if (ret < 0) return ret;
                }
                return 0;
            lab0:
                z->c = z->l - v_2;
            }
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 3:
            do {
                int v_3 = z->l - z->c;
                {
                    int v_4 = z->l - z->c;
                    {
                        int ret = skip_b_utf8(z->p, z->c, z->lb, 4);
                        if (ret < 0) goto lab1;
                        z->c = ret;
                    }
                    z->c = z->l - v_4;
                }
                if (z->c <= z->lb || (z->p[z->c - 1] != 115 && z->p[z->c - 1] != 116)) among_var = 2; else
                among_var = find_among_b(z, a_5, 5, 0);
                switch (among_var) {
                    case 1:
                        {
                            int ret = slice_from_s(z, 1, s_5);
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
                break;
            lab1:
                z->c = z->l - v_3;
                {
                    int ret = slice_from_s(z, 1, s_6);
                    if (ret < 0) return ret;
                }
            } while (0);
            break;
        case 4:
            do {
                int v_5 = z->l - z->c;
                if (in_grouping_b_U(z, g_RV, 97, 117, 0)) goto lab2;
                break;
            lab2:
                z->c = z->l - v_5;
                {
                    int ret = r_LONGV(z);
                    if (ret <= 0) return ret;
                }
            } while (0);
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_nu(struct SN_env * z) {
    {
        int v_1;
        if (z->c < ((SN_local *)z)->i_p1) return 0;
        v_1 = z->lb; z->lb = ((SN_local *)z)->i_p1;
        z->ket = z->c;
        if (z->c - 1 <= z->lb || (z->p[z->c - 1] != 97 && z->p[z->c - 1] != 117)) { z->lb = v_1; return 0; }
        if (!find_among_b(z, a_7, 4, 0)) { z->lb = v_1; return 0; }
        z->bra = z->c;
        z->lb = v_1;
    }
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_undouble_kpt(struct SN_env * z) {
    int among_var;
    if (in_grouping_b_U(z, g_V1, 97, 252, 0)) return 0;
    if (((SN_local *)z)->i_p1 > z->c) return 0;
    z->ket = z->c;
    if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((1116160 >> (z->p[z->c - 1] & 0x1f)) & 1)) return 0;
    among_var = find_among_b(z, a_8, 3, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    switch (among_var) {
        case 1:
            {
                int ret = slice_from_s(z, 1, s_7);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {
                int ret = slice_from_s(z, 1, s_8);
                if (ret < 0) return ret;
            }
            break;
        case 3:
            {
                int ret = slice_from_s(z, 1, s_9);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_degrees(struct SN_env * z) {
    int among_var;
    {
        int v_1;
        if (z->c < ((SN_local *)z)->i_p1) return 0;
        v_1 = z->lb; z->lb = ((SN_local *)z)->i_p1;
        z->ket = z->c;
        if (z->c <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((8706 >> (z->p[z->c - 1] & 0x1f)) & 1)) { z->lb = v_1; return 0; }
        among_var = find_among_b(z, a_9, 3, 0);
        if (!among_var) { z->lb = v_1; return 0; }
        z->bra = z->c;
        z->lb = v_1;
    }
    switch (among_var) {
        case 1:
            if (in_grouping_b_U(z, g_RV, 97, 117, 0)) return 0;
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

static int r_substantive(struct SN_env * z) {
    {
        int v_1 = z->l - z->c;
        {
            int ret = r_special_noun_endings(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_1;
    }
    {
        int v_2 = z->l - z->c;
        {
            int ret = r_case_ending(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_2;
    }
    {
        int v_3 = z->l - z->c;
        {
            int ret = r_plural_three_first_cases(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_3;
    }
    {
        int v_4 = z->l - z->c;
        {
            int ret = r_degrees(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_4;
    }
    {
        int v_5 = z->l - z->c;
        {
            int ret = r_i_plural(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_5;
    }
    {
        int v_6 = z->l - z->c;
        {
            int ret = r_nu(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_6;
    }
    return 1;
}

static int r_verb_exceptions(struct SN_env * z) {
    int among_var;
    z->bra = z->c;
    among_var = find_among(z, a_10, 290, 0);
    if (!among_var) return 0;
    z->ket = z->c;
    if (z->c < z->l) return 0;
    switch (among_var) {
        case 1:
            {
                int ret = slice_from_s(z, 3, s_10);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {
                int ret = slice_from_s(z, 3, s_11);
                if (ret < 0) return ret;
            }
            break;
        case 3:
            {
                int ret = slice_from_s(z, 5, s_12);
                if (ret < 0) return ret;
            }
            break;
        case 4:
            {
                int ret = slice_from_s(z, 5, s_13);
                if (ret < 0) return ret;
            }
            break;
        case 5:
            {
                int ret = slice_from_s(z, 5, s_14);
                if (ret < 0) return ret;
            }
            break;
        case 6:
            {
                int ret = slice_from_s(z, 4, s_15);
                if (ret < 0) return ret;
            }
            break;
        case 7:
            {
                int ret = slice_from_s(z, 3, s_16);
                if (ret < 0) return ret;
            }
            break;
        case 8:
            {
                int ret = slice_from_s(z, 6, s_17);
                if (ret < 0) return ret;
            }
            break;
        case 9:
            {
                int ret = slice_from_s(z, 5, s_18);
                if (ret < 0) return ret;
            }
            break;
        case 10:
            {
                int ret = slice_from_s(z, 3, s_19);
                if (ret < 0) return ret;
            }
            break;
        case 11:
            {
                int ret = slice_from_s(z, 6, s_20);
                if (ret < 0) return ret;
            }
            break;
        case 12:
            {
                int ret = slice_from_s(z, 7, s_21);
                if (ret < 0) return ret;
            }
            break;
        case 13:
            {
                int ret = slice_from_s(z, 7, s_22);
                if (ret < 0) return ret;
            }
            break;
        case 14:
            {
                int ret = slice_from_s(z, 4, s_23);
                if (ret < 0) return ret;
            }
            break;
        case 15:
            {
                int ret = slice_from_s(z, 5, s_24);
                if (ret < 0) return ret;
            }
            break;
        case 16:
            {
                int ret = slice_from_s(z, 4, s_25);
                if (ret < 0) return ret;
            }
            break;
        case 17:
            {
                int ret = slice_from_s(z, 4, s_26);
                if (ret < 0) return ret;
            }
            break;
        case 18:
            {
                int ret = slice_from_s(z, 5, s_27);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

extern int estonian_UTF_8_stem(struct SN_env * z) {
    {
        int v_1 = z->c;
        {
            int ret = r_verb_exceptions(z);
            if (ret == 0) goto lab0;
            if (ret < 0) return ret;
        }
        return 0;
    lab0:
        z->c = v_1;
    }
    {
        int v_2 = z->c;
        {
            int ret = r_mark_regions(z);
            if (ret < 0) return ret;
        }
        z->c = v_2;
    }
    z->lb = z->c; z->c = z->l;
    {
        int v_3 = z->l - z->c;
        {
            int ret = r_emphasis(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_3;
    }
    {
        int v_4 = z->l - z->c;
        do {
            int v_5 = z->l - z->c;
            {
                int ret = r_verb(z);
                if (ret == 0) goto lab2;
                if (ret < 0) return ret;
            }
            break;
        lab2:
            z->c = z->l - v_5;
            {
                int ret = r_substantive(z);
                if (ret < 0) return ret;
            }
        } while (0);
        z->c = z->l - v_4;
    }
    {
        int v_6 = z->l - z->c;
        {
            int ret = r_undouble_kpt(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_6;
    }
    z->c = z->lb;
    return 1;
}

extern struct SN_env * estonian_UTF_8_create_env(void) {
    struct SN_env * z = SN_new_env(sizeof(SN_local));
    if (z) {
        ((SN_local *)z)->i_p1 = 0;
    }
    return z;
}

extern void estonian_UTF_8_close_env(struct SN_env * z) {
    SN_delete_env(z);
}

