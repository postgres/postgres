/* Generated from italian.sbl by Snowball 3.0.0 - https://snowballstem.org/ */

#include "stem_ISO_8859_1_italian.h"

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
extern int italian_ISO_8859_1_stem(struct SN_env * z);
#ifdef __cplusplus
}
#endif

static int r_vowel_suffix(struct SN_env * z);
static int r_verb_suffix(struct SN_env * z);
static int r_standard_suffix(struct SN_env * z);
static int r_attached_pronoun(struct SN_env * z);
static int r_R2(struct SN_env * z);
static int r_R1(struct SN_env * z);
static int r_RV(struct SN_env * z);
static int r_mark_regions(struct SN_env * z);
static int r_postlude(struct SN_env * z);
static int r_prelude(struct SN_env * z);

static const symbol s_0[] = { 0xE0 };
static const symbol s_1[] = { 0xE8 };
static const symbol s_2[] = { 0xEC };
static const symbol s_3[] = { 0xF2 };
static const symbol s_4[] = { 0xF9 };
static const symbol s_5[] = { 'q', 'U' };
static const symbol s_6[] = { 'U' };
static const symbol s_7[] = { 'I' };
static const symbol s_8[] = { 'd', 'i', 'v', 'a', 'n' };
static const symbol s_9[] = { 'i' };
static const symbol s_10[] = { 'u' };
static const symbol s_11[] = { 'e' };
static const symbol s_12[] = { 'i', 'c' };
static const symbol s_13[] = { 'l', 'o', 'g' };
static const symbol s_14[] = { 'u' };
static const symbol s_15[] = { 'e', 'n', 't', 'e' };
static const symbol s_16[] = { 'a', 't' };
static const symbol s_17[] = { 'a', 't' };
static const symbol s_18[] = { 'i', 'c' };

static const symbol s_0_1[2] = { 'q', 'u' };
static const symbol s_0_2[1] = { 0xE1 };
static const symbol s_0_3[1] = { 0xE9 };
static const symbol s_0_4[1] = { 0xED };
static const symbol s_0_5[1] = { 0xF3 };
static const symbol s_0_6[1] = { 0xFA };
static const struct among a_0[7] = {
{ 0, 0, 0, 7, 0},
{ 2, s_0_1, -1, 6, 0},
{ 1, s_0_2, -2, 1, 0},
{ 1, s_0_3, -3, 2, 0},
{ 1, s_0_4, -4, 3, 0},
{ 1, s_0_5, -5, 4, 0},
{ 1, s_0_6, -6, 5, 0}
};

static const symbol s_1_1[1] = { 'I' };
static const symbol s_1_2[1] = { 'U' };
static const struct among a_1[3] = {
{ 0, 0, 0, 3, 0},
{ 1, s_1_1, -1, 1, 0},
{ 1, s_1_2, -2, 2, 0}
};

static const symbol s_2_0[2] = { 'l', 'a' };
static const symbol s_2_1[4] = { 'c', 'e', 'l', 'a' };
static const symbol s_2_2[6] = { 'g', 'l', 'i', 'e', 'l', 'a' };
static const symbol s_2_3[4] = { 'm', 'e', 'l', 'a' };
static const symbol s_2_4[4] = { 't', 'e', 'l', 'a' };
static const symbol s_2_5[4] = { 'v', 'e', 'l', 'a' };
static const symbol s_2_6[2] = { 'l', 'e' };
static const symbol s_2_7[4] = { 'c', 'e', 'l', 'e' };
static const symbol s_2_8[6] = { 'g', 'l', 'i', 'e', 'l', 'e' };
static const symbol s_2_9[4] = { 'm', 'e', 'l', 'e' };
static const symbol s_2_10[4] = { 't', 'e', 'l', 'e' };
static const symbol s_2_11[4] = { 'v', 'e', 'l', 'e' };
static const symbol s_2_12[2] = { 'n', 'e' };
static const symbol s_2_13[4] = { 'c', 'e', 'n', 'e' };
static const symbol s_2_14[6] = { 'g', 'l', 'i', 'e', 'n', 'e' };
static const symbol s_2_15[4] = { 'm', 'e', 'n', 'e' };
static const symbol s_2_16[4] = { 's', 'e', 'n', 'e' };
static const symbol s_2_17[4] = { 't', 'e', 'n', 'e' };
static const symbol s_2_18[4] = { 'v', 'e', 'n', 'e' };
static const symbol s_2_19[2] = { 'c', 'i' };
static const symbol s_2_20[2] = { 'l', 'i' };
static const symbol s_2_21[4] = { 'c', 'e', 'l', 'i' };
static const symbol s_2_22[6] = { 'g', 'l', 'i', 'e', 'l', 'i' };
static const symbol s_2_23[4] = { 'm', 'e', 'l', 'i' };
static const symbol s_2_24[4] = { 't', 'e', 'l', 'i' };
static const symbol s_2_25[4] = { 'v', 'e', 'l', 'i' };
static const symbol s_2_26[3] = { 'g', 'l', 'i' };
static const symbol s_2_27[2] = { 'm', 'i' };
static const symbol s_2_28[2] = { 's', 'i' };
static const symbol s_2_29[2] = { 't', 'i' };
static const symbol s_2_30[2] = { 'v', 'i' };
static const symbol s_2_31[2] = { 'l', 'o' };
static const symbol s_2_32[4] = { 'c', 'e', 'l', 'o' };
static const symbol s_2_33[6] = { 'g', 'l', 'i', 'e', 'l', 'o' };
static const symbol s_2_34[4] = { 'm', 'e', 'l', 'o' };
static const symbol s_2_35[4] = { 't', 'e', 'l', 'o' };
static const symbol s_2_36[4] = { 'v', 'e', 'l', 'o' };
static const struct among a_2[37] = {
{ 2, s_2_0, 0, -1, 0},
{ 4, s_2_1, -1, -1, 0},
{ 6, s_2_2, -2, -1, 0},
{ 4, s_2_3, -3, -1, 0},
{ 4, s_2_4, -4, -1, 0},
{ 4, s_2_5, -5, -1, 0},
{ 2, s_2_6, 0, -1, 0},
{ 4, s_2_7, -1, -1, 0},
{ 6, s_2_8, -2, -1, 0},
{ 4, s_2_9, -3, -1, 0},
{ 4, s_2_10, -4, -1, 0},
{ 4, s_2_11, -5, -1, 0},
{ 2, s_2_12, 0, -1, 0},
{ 4, s_2_13, -1, -1, 0},
{ 6, s_2_14, -2, -1, 0},
{ 4, s_2_15, -3, -1, 0},
{ 4, s_2_16, -4, -1, 0},
{ 4, s_2_17, -5, -1, 0},
{ 4, s_2_18, -6, -1, 0},
{ 2, s_2_19, 0, -1, 0},
{ 2, s_2_20, 0, -1, 0},
{ 4, s_2_21, -1, -1, 0},
{ 6, s_2_22, -2, -1, 0},
{ 4, s_2_23, -3, -1, 0},
{ 4, s_2_24, -4, -1, 0},
{ 4, s_2_25, -5, -1, 0},
{ 3, s_2_26, -6, -1, 0},
{ 2, s_2_27, 0, -1, 0},
{ 2, s_2_28, 0, -1, 0},
{ 2, s_2_29, 0, -1, 0},
{ 2, s_2_30, 0, -1, 0},
{ 2, s_2_31, 0, -1, 0},
{ 4, s_2_32, -1, -1, 0},
{ 6, s_2_33, -2, -1, 0},
{ 4, s_2_34, -3, -1, 0},
{ 4, s_2_35, -4, -1, 0},
{ 4, s_2_36, -5, -1, 0}
};

static const symbol s_3_0[4] = { 'a', 'n', 'd', 'o' };
static const symbol s_3_1[4] = { 'e', 'n', 'd', 'o' };
static const symbol s_3_2[2] = { 'a', 'r' };
static const symbol s_3_3[2] = { 'e', 'r' };
static const symbol s_3_4[2] = { 'i', 'r' };
static const struct among a_3[5] = {
{ 4, s_3_0, 0, 1, 0},
{ 4, s_3_1, 0, 1, 0},
{ 2, s_3_2, 0, 2, 0},
{ 2, s_3_3, 0, 2, 0},
{ 2, s_3_4, 0, 2, 0}
};

static const symbol s_4_0[2] = { 'i', 'c' };
static const symbol s_4_1[4] = { 'a', 'b', 'i', 'l' };
static const symbol s_4_2[2] = { 'o', 's' };
static const symbol s_4_3[2] = { 'i', 'v' };
static const struct among a_4[4] = {
{ 2, s_4_0, 0, -1, 0},
{ 4, s_4_1, 0, -1, 0},
{ 2, s_4_2, 0, -1, 0},
{ 2, s_4_3, 0, 1, 0}
};

static const symbol s_5_0[2] = { 'i', 'c' };
static const symbol s_5_1[4] = { 'a', 'b', 'i', 'l' };
static const symbol s_5_2[2] = { 'i', 'v' };
static const struct among a_5[3] = {
{ 2, s_5_0, 0, 1, 0},
{ 4, s_5_1, 0, 1, 0},
{ 2, s_5_2, 0, 1, 0}
};

static const symbol s_6_0[3] = { 'i', 'c', 'a' };
static const symbol s_6_1[5] = { 'l', 'o', 'g', 'i', 'a' };
static const symbol s_6_2[3] = { 'o', 's', 'a' };
static const symbol s_6_3[4] = { 'i', 's', 't', 'a' };
static const symbol s_6_4[3] = { 'i', 'v', 'a' };
static const symbol s_6_5[4] = { 'a', 'n', 'z', 'a' };
static const symbol s_6_6[4] = { 'e', 'n', 'z', 'a' };
static const symbol s_6_7[3] = { 'i', 'c', 'e' };
static const symbol s_6_8[6] = { 'a', 't', 'r', 'i', 'c', 'e' };
static const symbol s_6_9[4] = { 'i', 'c', 'h', 'e' };
static const symbol s_6_10[5] = { 'l', 'o', 'g', 'i', 'e' };
static const symbol s_6_11[5] = { 'a', 'b', 'i', 'l', 'e' };
static const symbol s_6_12[5] = { 'i', 'b', 'i', 'l', 'e' };
static const symbol s_6_13[6] = { 'u', 's', 'i', 'o', 'n', 'e' };
static const symbol s_6_14[6] = { 'a', 'z', 'i', 'o', 'n', 'e' };
static const symbol s_6_15[6] = { 'u', 'z', 'i', 'o', 'n', 'e' };
static const symbol s_6_16[5] = { 'a', 't', 'o', 'r', 'e' };
static const symbol s_6_17[3] = { 'o', 's', 'e' };
static const symbol s_6_18[4] = { 'a', 'n', 't', 'e' };
static const symbol s_6_19[5] = { 'm', 'e', 'n', 't', 'e' };
static const symbol s_6_20[6] = { 'a', 'm', 'e', 'n', 't', 'e' };
static const symbol s_6_21[4] = { 'i', 's', 't', 'e' };
static const symbol s_6_22[3] = { 'i', 'v', 'e' };
static const symbol s_6_23[4] = { 'a', 'n', 'z', 'e' };
static const symbol s_6_24[4] = { 'e', 'n', 'z', 'e' };
static const symbol s_6_25[3] = { 'i', 'c', 'i' };
static const symbol s_6_26[6] = { 'a', 't', 'r', 'i', 'c', 'i' };
static const symbol s_6_27[4] = { 'i', 'c', 'h', 'i' };
static const symbol s_6_28[5] = { 'a', 'b', 'i', 'l', 'i' };
static const symbol s_6_29[5] = { 'i', 'b', 'i', 'l', 'i' };
static const symbol s_6_30[4] = { 'i', 's', 'm', 'i' };
static const symbol s_6_31[6] = { 'u', 's', 'i', 'o', 'n', 'i' };
static const symbol s_6_32[6] = { 'a', 'z', 'i', 'o', 'n', 'i' };
static const symbol s_6_33[6] = { 'u', 'z', 'i', 'o', 'n', 'i' };
static const symbol s_6_34[5] = { 'a', 't', 'o', 'r', 'i' };
static const symbol s_6_35[3] = { 'o', 's', 'i' };
static const symbol s_6_36[4] = { 'a', 'n', 't', 'i' };
static const symbol s_6_37[6] = { 'a', 'm', 'e', 'n', 't', 'i' };
static const symbol s_6_38[6] = { 'i', 'm', 'e', 'n', 't', 'i' };
static const symbol s_6_39[4] = { 'i', 's', 't', 'i' };
static const symbol s_6_40[3] = { 'i', 'v', 'i' };
static const symbol s_6_41[3] = { 'i', 'c', 'o' };
static const symbol s_6_42[4] = { 'i', 's', 'm', 'o' };
static const symbol s_6_43[3] = { 'o', 's', 'o' };
static const symbol s_6_44[6] = { 'a', 'm', 'e', 'n', 't', 'o' };
static const symbol s_6_45[6] = { 'i', 'm', 'e', 'n', 't', 'o' };
static const symbol s_6_46[3] = { 'i', 'v', 'o' };
static const symbol s_6_47[3] = { 'i', 't', 0xE0 };
static const symbol s_6_48[4] = { 'i', 's', 't', 0xE0 };
static const symbol s_6_49[4] = { 'i', 's', 't', 0xE8 };
static const symbol s_6_50[4] = { 'i', 's', 't', 0xEC };
static const struct among a_6[51] = {
{ 3, s_6_0, 0, 1, 0},
{ 5, s_6_1, 0, 3, 0},
{ 3, s_6_2, 0, 1, 0},
{ 4, s_6_3, 0, 1, 0},
{ 3, s_6_4, 0, 9, 0},
{ 4, s_6_5, 0, 1, 0},
{ 4, s_6_6, 0, 5, 0},
{ 3, s_6_7, 0, 1, 0},
{ 6, s_6_8, -1, 1, 0},
{ 4, s_6_9, 0, 1, 0},
{ 5, s_6_10, 0, 3, 0},
{ 5, s_6_11, 0, 1, 0},
{ 5, s_6_12, 0, 1, 0},
{ 6, s_6_13, 0, 4, 0},
{ 6, s_6_14, 0, 2, 0},
{ 6, s_6_15, 0, 4, 0},
{ 5, s_6_16, 0, 2, 0},
{ 3, s_6_17, 0, 1, 0},
{ 4, s_6_18, 0, 1, 0},
{ 5, s_6_19, 0, 1, 0},
{ 6, s_6_20, -1, 7, 0},
{ 4, s_6_21, 0, 1, 0},
{ 3, s_6_22, 0, 9, 0},
{ 4, s_6_23, 0, 1, 0},
{ 4, s_6_24, 0, 5, 0},
{ 3, s_6_25, 0, 1, 0},
{ 6, s_6_26, -1, 1, 0},
{ 4, s_6_27, 0, 1, 0},
{ 5, s_6_28, 0, 1, 0},
{ 5, s_6_29, 0, 1, 0},
{ 4, s_6_30, 0, 1, 0},
{ 6, s_6_31, 0, 4, 0},
{ 6, s_6_32, 0, 2, 0},
{ 6, s_6_33, 0, 4, 0},
{ 5, s_6_34, 0, 2, 0},
{ 3, s_6_35, 0, 1, 0},
{ 4, s_6_36, 0, 1, 0},
{ 6, s_6_37, 0, 6, 0},
{ 6, s_6_38, 0, 6, 0},
{ 4, s_6_39, 0, 1, 0},
{ 3, s_6_40, 0, 9, 0},
{ 3, s_6_41, 0, 1, 0},
{ 4, s_6_42, 0, 1, 0},
{ 3, s_6_43, 0, 1, 0},
{ 6, s_6_44, 0, 6, 0},
{ 6, s_6_45, 0, 6, 0},
{ 3, s_6_46, 0, 9, 0},
{ 3, s_6_47, 0, 8, 0},
{ 4, s_6_48, 0, 1, 0},
{ 4, s_6_49, 0, 1, 0},
{ 4, s_6_50, 0, 1, 0}
};

static const symbol s_7_0[4] = { 'i', 's', 'c', 'a' };
static const symbol s_7_1[4] = { 'e', 'n', 'd', 'a' };
static const symbol s_7_2[3] = { 'a', 't', 'a' };
static const symbol s_7_3[3] = { 'i', 't', 'a' };
static const symbol s_7_4[3] = { 'u', 't', 'a' };
static const symbol s_7_5[3] = { 'a', 'v', 'a' };
static const symbol s_7_6[3] = { 'e', 'v', 'a' };
static const symbol s_7_7[3] = { 'i', 'v', 'a' };
static const symbol s_7_8[6] = { 'e', 'r', 'e', 'b', 'b', 'e' };
static const symbol s_7_9[6] = { 'i', 'r', 'e', 'b', 'b', 'e' };
static const symbol s_7_10[4] = { 'i', 's', 'c', 'e' };
static const symbol s_7_11[4] = { 'e', 'n', 'd', 'e' };
static const symbol s_7_12[3] = { 'a', 'r', 'e' };
static const symbol s_7_13[3] = { 'e', 'r', 'e' };
static const symbol s_7_14[3] = { 'i', 'r', 'e' };
static const symbol s_7_15[4] = { 'a', 's', 's', 'e' };
static const symbol s_7_16[3] = { 'a', 't', 'e' };
static const symbol s_7_17[5] = { 'a', 'v', 'a', 't', 'e' };
static const symbol s_7_18[5] = { 'e', 'v', 'a', 't', 'e' };
static const symbol s_7_19[5] = { 'i', 'v', 'a', 't', 'e' };
static const symbol s_7_20[3] = { 'e', 't', 'e' };
static const symbol s_7_21[5] = { 'e', 'r', 'e', 't', 'e' };
static const symbol s_7_22[5] = { 'i', 'r', 'e', 't', 'e' };
static const symbol s_7_23[3] = { 'i', 't', 'e' };
static const symbol s_7_24[6] = { 'e', 'r', 'e', 's', 't', 'e' };
static const symbol s_7_25[6] = { 'i', 'r', 'e', 's', 't', 'e' };
static const symbol s_7_26[3] = { 'u', 't', 'e' };
static const symbol s_7_27[4] = { 'e', 'r', 'a', 'i' };
static const symbol s_7_28[4] = { 'i', 'r', 'a', 'i' };
static const symbol s_7_29[4] = { 'i', 's', 'c', 'i' };
static const symbol s_7_30[4] = { 'e', 'n', 'd', 'i' };
static const symbol s_7_31[4] = { 'e', 'r', 'e', 'i' };
static const symbol s_7_32[4] = { 'i', 'r', 'e', 'i' };
static const symbol s_7_33[4] = { 'a', 's', 's', 'i' };
static const symbol s_7_34[3] = { 'a', 't', 'i' };
static const symbol s_7_35[3] = { 'i', 't', 'i' };
static const symbol s_7_36[6] = { 'e', 'r', 'e', 's', 't', 'i' };
static const symbol s_7_37[6] = { 'i', 'r', 'e', 's', 't', 'i' };
static const symbol s_7_38[3] = { 'u', 't', 'i' };
static const symbol s_7_39[3] = { 'a', 'v', 'i' };
static const symbol s_7_40[3] = { 'e', 'v', 'i' };
static const symbol s_7_41[3] = { 'i', 'v', 'i' };
static const symbol s_7_42[4] = { 'i', 's', 'c', 'o' };
static const symbol s_7_43[4] = { 'a', 'n', 'd', 'o' };
static const symbol s_7_44[4] = { 'e', 'n', 'd', 'o' };
static const symbol s_7_45[4] = { 'Y', 'a', 'm', 'o' };
static const symbol s_7_46[4] = { 'i', 'a', 'm', 'o' };
static const symbol s_7_47[5] = { 'a', 'v', 'a', 'm', 'o' };
static const symbol s_7_48[5] = { 'e', 'v', 'a', 'm', 'o' };
static const symbol s_7_49[5] = { 'i', 'v', 'a', 'm', 'o' };
static const symbol s_7_50[5] = { 'e', 'r', 'e', 'm', 'o' };
static const symbol s_7_51[5] = { 'i', 'r', 'e', 'm', 'o' };
static const symbol s_7_52[6] = { 'a', 's', 's', 'i', 'm', 'o' };
static const symbol s_7_53[4] = { 'a', 'm', 'm', 'o' };
static const symbol s_7_54[4] = { 'e', 'm', 'm', 'o' };
static const symbol s_7_55[6] = { 'e', 'r', 'e', 'm', 'm', 'o' };
static const symbol s_7_56[6] = { 'i', 'r', 'e', 'm', 'm', 'o' };
static const symbol s_7_57[4] = { 'i', 'm', 'm', 'o' };
static const symbol s_7_58[3] = { 'a', 'n', 'o' };
static const symbol s_7_59[6] = { 'i', 's', 'c', 'a', 'n', 'o' };
static const symbol s_7_60[5] = { 'a', 'v', 'a', 'n', 'o' };
static const symbol s_7_61[5] = { 'e', 'v', 'a', 'n', 'o' };
static const symbol s_7_62[5] = { 'i', 'v', 'a', 'n', 'o' };
static const symbol s_7_63[6] = { 'e', 'r', 'a', 'n', 'n', 'o' };
static const symbol s_7_64[6] = { 'i', 'r', 'a', 'n', 'n', 'o' };
static const symbol s_7_65[3] = { 'o', 'n', 'o' };
static const symbol s_7_66[6] = { 'i', 's', 'c', 'o', 'n', 'o' };
static const symbol s_7_67[5] = { 'a', 'r', 'o', 'n', 'o' };
static const symbol s_7_68[5] = { 'e', 'r', 'o', 'n', 'o' };
static const symbol s_7_69[5] = { 'i', 'r', 'o', 'n', 'o' };
static const symbol s_7_70[8] = { 'e', 'r', 'e', 'b', 'b', 'e', 'r', 'o' };
static const symbol s_7_71[8] = { 'i', 'r', 'e', 'b', 'b', 'e', 'r', 'o' };
static const symbol s_7_72[6] = { 'a', 's', 's', 'e', 'r', 'o' };
static const symbol s_7_73[6] = { 'e', 's', 's', 'e', 'r', 'o' };
static const symbol s_7_74[6] = { 'i', 's', 's', 'e', 'r', 'o' };
static const symbol s_7_75[3] = { 'a', 't', 'o' };
static const symbol s_7_76[3] = { 'i', 't', 'o' };
static const symbol s_7_77[3] = { 'u', 't', 'o' };
static const symbol s_7_78[3] = { 'a', 'v', 'o' };
static const symbol s_7_79[3] = { 'e', 'v', 'o' };
static const symbol s_7_80[3] = { 'i', 'v', 'o' };
static const symbol s_7_81[2] = { 'a', 'r' };
static const symbol s_7_82[2] = { 'i', 'r' };
static const symbol s_7_83[3] = { 'e', 'r', 0xE0 };
static const symbol s_7_84[3] = { 'i', 'r', 0xE0 };
static const symbol s_7_85[3] = { 'e', 'r', 0xF2 };
static const symbol s_7_86[3] = { 'i', 'r', 0xF2 };
static const struct among a_7[87] = {
{ 4, s_7_0, 0, 1, 0},
{ 4, s_7_1, 0, 1, 0},
{ 3, s_7_2, 0, 1, 0},
{ 3, s_7_3, 0, 1, 0},
{ 3, s_7_4, 0, 1, 0},
{ 3, s_7_5, 0, 1, 0},
{ 3, s_7_6, 0, 1, 0},
{ 3, s_7_7, 0, 1, 0},
{ 6, s_7_8, 0, 1, 0},
{ 6, s_7_9, 0, 1, 0},
{ 4, s_7_10, 0, 1, 0},
{ 4, s_7_11, 0, 1, 0},
{ 3, s_7_12, 0, 1, 0},
{ 3, s_7_13, 0, 1, 0},
{ 3, s_7_14, 0, 1, 0},
{ 4, s_7_15, 0, 1, 0},
{ 3, s_7_16, 0, 1, 0},
{ 5, s_7_17, -1, 1, 0},
{ 5, s_7_18, -2, 1, 0},
{ 5, s_7_19, -3, 1, 0},
{ 3, s_7_20, 0, 1, 0},
{ 5, s_7_21, -1, 1, 0},
{ 5, s_7_22, -2, 1, 0},
{ 3, s_7_23, 0, 1, 0},
{ 6, s_7_24, 0, 1, 0},
{ 6, s_7_25, 0, 1, 0},
{ 3, s_7_26, 0, 1, 0},
{ 4, s_7_27, 0, 1, 0},
{ 4, s_7_28, 0, 1, 0},
{ 4, s_7_29, 0, 1, 0},
{ 4, s_7_30, 0, 1, 0},
{ 4, s_7_31, 0, 1, 0},
{ 4, s_7_32, 0, 1, 0},
{ 4, s_7_33, 0, 1, 0},
{ 3, s_7_34, 0, 1, 0},
{ 3, s_7_35, 0, 1, 0},
{ 6, s_7_36, 0, 1, 0},
{ 6, s_7_37, 0, 1, 0},
{ 3, s_7_38, 0, 1, 0},
{ 3, s_7_39, 0, 1, 0},
{ 3, s_7_40, 0, 1, 0},
{ 3, s_7_41, 0, 1, 0},
{ 4, s_7_42, 0, 1, 0},
{ 4, s_7_43, 0, 1, 0},
{ 4, s_7_44, 0, 1, 0},
{ 4, s_7_45, 0, 1, 0},
{ 4, s_7_46, 0, 1, 0},
{ 5, s_7_47, 0, 1, 0},
{ 5, s_7_48, 0, 1, 0},
{ 5, s_7_49, 0, 1, 0},
{ 5, s_7_50, 0, 1, 0},
{ 5, s_7_51, 0, 1, 0},
{ 6, s_7_52, 0, 1, 0},
{ 4, s_7_53, 0, 1, 0},
{ 4, s_7_54, 0, 1, 0},
{ 6, s_7_55, -1, 1, 0},
{ 6, s_7_56, -2, 1, 0},
{ 4, s_7_57, 0, 1, 0},
{ 3, s_7_58, 0, 1, 0},
{ 6, s_7_59, -1, 1, 0},
{ 5, s_7_60, -2, 1, 0},
{ 5, s_7_61, -3, 1, 0},
{ 5, s_7_62, -4, 1, 0},
{ 6, s_7_63, 0, 1, 0},
{ 6, s_7_64, 0, 1, 0},
{ 3, s_7_65, 0, 1, 0},
{ 6, s_7_66, -1, 1, 0},
{ 5, s_7_67, -2, 1, 0},
{ 5, s_7_68, -3, 1, 0},
{ 5, s_7_69, -4, 1, 0},
{ 8, s_7_70, 0, 1, 0},
{ 8, s_7_71, 0, 1, 0},
{ 6, s_7_72, 0, 1, 0},
{ 6, s_7_73, 0, 1, 0},
{ 6, s_7_74, 0, 1, 0},
{ 3, s_7_75, 0, 1, 0},
{ 3, s_7_76, 0, 1, 0},
{ 3, s_7_77, 0, 1, 0},
{ 3, s_7_78, 0, 1, 0},
{ 3, s_7_79, 0, 1, 0},
{ 3, s_7_80, 0, 1, 0},
{ 2, s_7_81, 0, 1, 0},
{ 2, s_7_82, 0, 1, 0},
{ 3, s_7_83, 0, 1, 0},
{ 3, s_7_84, 0, 1, 0},
{ 3, s_7_85, 0, 1, 0},
{ 3, s_7_86, 0, 1, 0}
};

static const unsigned char g_v[] = { 17, 65, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 128, 128, 8, 2, 1 };

static const unsigned char g_AEIO[] = { 17, 65, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 128, 128, 8, 2 };

static const unsigned char g_CG[] = { 17 };

static int r_prelude(struct SN_env * z) {
    int among_var;
    {
        int v_1 = z->c;
        while (1) {
            int v_2 = z->c;
            z->bra = z->c;
            among_var = find_among(z, a_0, 7, 0);
            z->ket = z->c;
            switch (among_var) {
                case 1:
                    {
                        int ret = slice_from_s(z, 1, s_0);
                        if (ret < 0) return ret;
                    }
                    break;
                case 2:
                    {
                        int ret = slice_from_s(z, 1, s_1);
                        if (ret < 0) return ret;
                    }
                    break;
                case 3:
                    {
                        int ret = slice_from_s(z, 1, s_2);
                        if (ret < 0) return ret;
                    }
                    break;
                case 4:
                    {
                        int ret = slice_from_s(z, 1, s_3);
                        if (ret < 0) return ret;
                    }
                    break;
                case 5:
                    {
                        int ret = slice_from_s(z, 1, s_4);
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
                    if (z->c >= z->l) goto lab0;
                    z->c++;
                    break;
            }
            continue;
        lab0:
            z->c = v_2;
            break;
        }
        z->c = v_1;
    }
    while (1) {
        int v_3 = z->c;
        while (1) {
            int v_4 = z->c;
            if (in_grouping(z, g_v, 97, 249, 0)) goto lab2;
            z->bra = z->c;
            do {
                int v_5 = z->c;
                if (z->c == z->l || z->p[z->c] != 'u') goto lab3;
                z->c++;
                z->ket = z->c;
                if (in_grouping(z, g_v, 97, 249, 0)) goto lab3;
                {
                    int ret = slice_from_s(z, 1, s_6);
                    if (ret < 0) return ret;
                }
                break;
            lab3:
                z->c = v_5;
                if (z->c == z->l || z->p[z->c] != 'i') goto lab2;
                z->c++;
                z->ket = z->c;
                if (in_grouping(z, g_v, 97, 249, 0)) goto lab2;
                {
                    int ret = slice_from_s(z, 1, s_7);
                    if (ret < 0) return ret;
                }
            } while (0);
            z->c = v_4;
            break;
        lab2:
            z->c = v_4;
            if (z->c >= z->l) goto lab1;
            z->c++;
        }
        continue;
    lab1:
        z->c = v_3;
        break;
    }
    return 1;
}

static int r_mark_regions(struct SN_env * z) {
    ((SN_local *)z)->i_pV = z->l;
    ((SN_local *)z)->i_p1 = z->l;
    ((SN_local *)z)->i_p2 = z->l;
    {
        int v_1 = z->c;
        do {
            int v_2 = z->c;
            if (in_grouping(z, g_v, 97, 249, 0)) goto lab1;
            do {
                int v_3 = z->c;
                if (out_grouping(z, g_v, 97, 249, 0)) goto lab2;
                {
                    int ret = out_grouping(z, g_v, 97, 249, 1);
                    if (ret < 0) goto lab2;
                    z->c += ret;
                }
                break;
            lab2:
                z->c = v_3;
                if (in_grouping(z, g_v, 97, 249, 0)) goto lab1;
                {
                    int ret = in_grouping(z, g_v, 97, 249, 1);
                    if (ret < 0) goto lab1;
                    z->c += ret;
                }
            } while (0);
            break;
        lab1:
            z->c = v_2;
            if (!(eq_s(z, 5, s_8))) goto lab3;
            break;
        lab3:
            z->c = v_2;
            if (out_grouping(z, g_v, 97, 249, 0)) goto lab0;
            do {
                int v_4 = z->c;
                if (out_grouping(z, g_v, 97, 249, 0)) goto lab4;
                {
                    int ret = out_grouping(z, g_v, 97, 249, 1);
                    if (ret < 0) goto lab4;
                    z->c += ret;
                }
                break;
            lab4:
                z->c = v_4;
                if (in_grouping(z, g_v, 97, 249, 0)) goto lab0;
                if (z->c >= z->l) goto lab0;
                z->c++;
            } while (0);
        } while (0);
        ((SN_local *)z)->i_pV = z->c;
    lab0:
        z->c = v_1;
    }
    {
        int v_5 = z->c;
        {
            int ret = out_grouping(z, g_v, 97, 249, 1);
            if (ret < 0) goto lab5;
            z->c += ret;
        }
        {
            int ret = in_grouping(z, g_v, 97, 249, 1);
            if (ret < 0) goto lab5;
            z->c += ret;
        }
        ((SN_local *)z)->i_p1 = z->c;
        {
            int ret = out_grouping(z, g_v, 97, 249, 1);
            if (ret < 0) goto lab5;
            z->c += ret;
        }
        {
            int ret = in_grouping(z, g_v, 97, 249, 1);
            if (ret < 0) goto lab5;
            z->c += ret;
        }
        ((SN_local *)z)->i_p2 = z->c;
    lab5:
        z->c = v_5;
    }
    return 1;
}

static int r_postlude(struct SN_env * z) {
    int among_var;
    while (1) {
        int v_1 = z->c;
        z->bra = z->c;
        if (z->c >= z->l || (z->p[z->c + 0] != 73 && z->p[z->c + 0] != 85)) among_var = 3; else
        among_var = find_among(z, a_1, 3, 0);
        z->ket = z->c;
        switch (among_var) {
            case 1:
                {
                    int ret = slice_from_s(z, 1, s_9);
                    if (ret < 0) return ret;
                }
                break;
            case 2:
                {
                    int ret = slice_from_s(z, 1, s_10);
                    if (ret < 0) return ret;
                }
                break;
            case 3:
                if (z->c >= z->l) goto lab0;
                z->c++;
                break;
        }
        continue;
    lab0:
        z->c = v_1;
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

static int r_attached_pronoun(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((33314 >> (z->p[z->c - 1] & 0x1f)) & 1)) return 0;
    if (!find_among_b(z, a_2, 37, 0)) return 0;
    z->bra = z->c;
    if (z->c - 1 <= z->lb || (z->p[z->c - 1] != 111 && z->p[z->c - 1] != 114)) return 0;
    among_var = find_among_b(z, a_3, 5, 0);
    if (!among_var) return 0;
    {
        int ret = r_RV(z);
        if (ret <= 0) return ret;
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
                int ret = slice_from_s(z, 1, s_11);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_standard_suffix(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    among_var = find_among_b(z, a_6, 51, 0);
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
                int ret = r_R2(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            {
                int v_1 = z->l - z->c;
                z->ket = z->c;
                if (!(eq_s_b(z, 2, s_12))) { z->c = z->l - v_1; goto lab0; }
                z->bra = z->c;
                {
                    int ret = r_R2(z);
                    if (ret == 0) { z->c = z->l - v_1; goto lab0; }
                    if (ret < 0) return ret;
                }
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
            lab0:
                ;
            }
            break;
        case 3:
            {
                int ret = r_R2(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_from_s(z, 3, s_13);
                if (ret < 0) return ret;
            }
            break;
        case 4:
            {
                int ret = r_R2(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_from_s(z, 1, s_14);
                if (ret < 0) return ret;
            }
            break;
        case 5:
            {
                int ret = r_R2(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_from_s(z, 4, s_15);
                if (ret < 0) return ret;
            }
            break;
        case 6:
            {
                int ret = r_RV(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 7:
            {
                int ret = r_R1(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            {
                int v_2 = z->l - z->c;
                z->ket = z->c;
                if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((4722696 >> (z->p[z->c - 1] & 0x1f)) & 1)) { z->c = z->l - v_2; goto lab1; }
                among_var = find_among_b(z, a_4, 4, 0);
                if (!among_var) { z->c = z->l - v_2; goto lab1; }
                z->bra = z->c;
                {
                    int ret = r_R2(z);
                    if (ret == 0) { z->c = z->l - v_2; goto lab1; }
                    if (ret < 0) return ret;
                }
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                switch (among_var) {
                    case 1:
                        z->ket = z->c;
                        if (!(eq_s_b(z, 2, s_16))) { z->c = z->l - v_2; goto lab1; }
                        z->bra = z->c;
                        {
                            int ret = r_R2(z);
                            if (ret == 0) { z->c = z->l - v_2; goto lab1; }
                            if (ret < 0) return ret;
                        }
                        {
                            int ret = slice_del(z);
                            if (ret < 0) return ret;
                        }
                        break;
                }
            lab1:
                ;
            }
            break;
        case 8:
            {
                int ret = r_R2(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            {
                int v_3 = z->l - z->c;
                z->ket = z->c;
                if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((4198408 >> (z->p[z->c - 1] & 0x1f)) & 1)) { z->c = z->l - v_3; goto lab2; }
                if (!find_among_b(z, a_5, 3, 0)) { z->c = z->l - v_3; goto lab2; }
                z->bra = z->c;
                {
                    int ret = r_R2(z);
                    if (ret == 0) { z->c = z->l - v_3; goto lab2; }
                    if (ret < 0) return ret;
                }
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
            lab2:
                ;
            }
            break;
        case 9:
            {
                int ret = r_R2(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            {
                int v_4 = z->l - z->c;
                z->ket = z->c;
                if (!(eq_s_b(z, 2, s_17))) { z->c = z->l - v_4; goto lab3; }
                z->bra = z->c;
                {
                    int ret = r_R2(z);
                    if (ret == 0) { z->c = z->l - v_4; goto lab3; }
                    if (ret < 0) return ret;
                }
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                z->ket = z->c;
                if (!(eq_s_b(z, 2, s_18))) { z->c = z->l - v_4; goto lab3; }
                z->bra = z->c;
                {
                    int ret = r_R2(z);
                    if (ret == 0) { z->c = z->l - v_4; goto lab3; }
                    if (ret < 0) return ret;
                }
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
            lab3:
                ;
            }
            break;
    }
    return 1;
}

static int r_verb_suffix(struct SN_env * z) {
    {
        int v_1;
        if (z->c < ((SN_local *)z)->i_pV) return 0;
        v_1 = z->lb; z->lb = ((SN_local *)z)->i_pV;
        z->ket = z->c;
        if (!find_among_b(z, a_7, 87, 0)) { z->lb = v_1; return 0; }
        z->bra = z->c;
        {
            int ret = slice_del(z);
            if (ret < 0) return ret;
        }
        z->lb = v_1;
    }
    return 1;
}

static int r_vowel_suffix(struct SN_env * z) {
    {
        int v_1 = z->l - z->c;
        z->ket = z->c;
        if (in_grouping_b(z, g_AEIO, 97, 242, 0)) { z->c = z->l - v_1; goto lab0; }
        z->bra = z->c;
        {
            int ret = r_RV(z);
            if (ret == 0) { z->c = z->l - v_1; goto lab0; }
            if (ret < 0) return ret;
        }
        {
            int ret = slice_del(z);
            if (ret < 0) return ret;
        }
        z->ket = z->c;
        if (z->c <= z->lb || z->p[z->c - 1] != 'i') { z->c = z->l - v_1; goto lab0; }
        z->c--;
        z->bra = z->c;
        {
            int ret = r_RV(z);
            if (ret == 0) { z->c = z->l - v_1; goto lab0; }
            if (ret < 0) return ret;
        }
        {
            int ret = slice_del(z);
            if (ret < 0) return ret;
        }
    lab0:
        ;
    }
    {
        int v_2 = z->l - z->c;
        z->ket = z->c;
        if (z->c <= z->lb || z->p[z->c - 1] != 'h') { z->c = z->l - v_2; goto lab1; }
        z->c--;
        z->bra = z->c;
        if (in_grouping_b(z, g_CG, 99, 103, 0)) { z->c = z->l - v_2; goto lab1; }
        {
            int ret = r_RV(z);
            if (ret == 0) { z->c = z->l - v_2; goto lab1; }
            if (ret < 0) return ret;
        }
        {
            int ret = slice_del(z);
            if (ret < 0) return ret;
        }
    lab1:
        ;
    }
    return 1;
}

extern int italian_ISO_8859_1_stem(struct SN_env * z) {
    {
        int v_1 = z->c;
        {
            int ret = r_prelude(z);
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
            int ret = r_attached_pronoun(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_2;
    }
    {
        int v_3 = z->l - z->c;
        do {
            int v_4 = z->l - z->c;
            {
                int ret = r_standard_suffix(z);
                if (ret == 0) goto lab1;
                if (ret < 0) return ret;
            }
            break;
        lab1:
            z->c = z->l - v_4;
            {
                int ret = r_verb_suffix(z);
                if (ret == 0) goto lab0;
                if (ret < 0) return ret;
            }
        } while (0);
    lab0:
        z->c = z->l - v_3;
    }
    {
        int v_5 = z->l - z->c;
        {
            int ret = r_vowel_suffix(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_5;
    }
    z->c = z->lb;
    {
        int v_6 = z->c;
        {
            int ret = r_postlude(z);
            if (ret < 0) return ret;
        }
        z->c = v_6;
    }
    return 1;
}

extern struct SN_env * italian_ISO_8859_1_create_env(void) {
    struct SN_env * z = SN_new_env(sizeof(SN_local));
    if (z) {
        ((SN_local *)z)->i_p2 = 0;
        ((SN_local *)z)->i_p1 = 0;
        ((SN_local *)z)->i_pV = 0;
    }
    return z;
}

extern void italian_ISO_8859_1_close_env(struct SN_env * z) {
    SN_delete_env(z);
}

