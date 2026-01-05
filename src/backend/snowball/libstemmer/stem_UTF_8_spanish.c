/* Generated from spanish.sbl by Snowball 3.0.0 - https://snowballstem.org/ */

#include "stem_UTF_8_spanish.h"

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
extern int spanish_UTF_8_stem(struct SN_env * z);
#ifdef __cplusplus
}
#endif

static int r_residual_suffix(struct SN_env * z);
static int r_verb_suffix(struct SN_env * z);
static int r_y_verb_suffix(struct SN_env * z);
static int r_standard_suffix(struct SN_env * z);
static int r_attached_pronoun(struct SN_env * z);
static int r_R2(struct SN_env * z);
static int r_R1(struct SN_env * z);
static int r_RV(struct SN_env * z);
static int r_mark_regions(struct SN_env * z);
static int r_postlude(struct SN_env * z);

static const symbol s_0[] = { 'a' };
static const symbol s_1[] = { 'e' };
static const symbol s_2[] = { 'i' };
static const symbol s_3[] = { 'o' };
static const symbol s_4[] = { 'u' };
static const symbol s_5[] = { 'i', 'e', 'n', 'd', 'o' };
static const symbol s_6[] = { 'a', 'n', 'd', 'o' };
static const symbol s_7[] = { 'a', 'r' };
static const symbol s_8[] = { 'e', 'r' };
static const symbol s_9[] = { 'i', 'r' };
static const symbol s_10[] = { 'i', 'c' };
static const symbol s_11[] = { 'l', 'o', 'g' };
static const symbol s_12[] = { 'u' };
static const symbol s_13[] = { 'e', 'n', 't', 'e' };
static const symbol s_14[] = { 'a', 't' };
static const symbol s_15[] = { 'a', 't' };

static const symbol s_0_1[2] = { 0xC3, 0xA1 };
static const symbol s_0_2[2] = { 0xC3, 0xA9 };
static const symbol s_0_3[2] = { 0xC3, 0xAD };
static const symbol s_0_4[2] = { 0xC3, 0xB3 };
static const symbol s_0_5[2] = { 0xC3, 0xBA };
static const struct among a_0[6] = {
{ 0, 0, 0, 6, 0},
{ 2, s_0_1, -1, 1, 0},
{ 2, s_0_2, -2, 2, 0},
{ 2, s_0_3, -3, 3, 0},
{ 2, s_0_4, -4, 4, 0},
{ 2, s_0_5, -5, 5, 0}
};

static const symbol s_1_0[2] = { 'l', 'a' };
static const symbol s_1_1[4] = { 's', 'e', 'l', 'a' };
static const symbol s_1_2[2] = { 'l', 'e' };
static const symbol s_1_3[2] = { 'm', 'e' };
static const symbol s_1_4[2] = { 's', 'e' };
static const symbol s_1_5[2] = { 'l', 'o' };
static const symbol s_1_6[4] = { 's', 'e', 'l', 'o' };
static const symbol s_1_7[3] = { 'l', 'a', 's' };
static const symbol s_1_8[5] = { 's', 'e', 'l', 'a', 's' };
static const symbol s_1_9[3] = { 'l', 'e', 's' };
static const symbol s_1_10[3] = { 'l', 'o', 's' };
static const symbol s_1_11[5] = { 's', 'e', 'l', 'o', 's' };
static const symbol s_1_12[3] = { 'n', 'o', 's' };
static const struct among a_1[13] = {
{ 2, s_1_0, 0, -1, 0},
{ 4, s_1_1, -1, -1, 0},
{ 2, s_1_2, 0, -1, 0},
{ 2, s_1_3, 0, -1, 0},
{ 2, s_1_4, 0, -1, 0},
{ 2, s_1_5, 0, -1, 0},
{ 4, s_1_6, -1, -1, 0},
{ 3, s_1_7, 0, -1, 0},
{ 5, s_1_8, -1, -1, 0},
{ 3, s_1_9, 0, -1, 0},
{ 3, s_1_10, 0, -1, 0},
{ 5, s_1_11, -1, -1, 0},
{ 3, s_1_12, 0, -1, 0}
};

static const symbol s_2_0[4] = { 'a', 'n', 'd', 'o' };
static const symbol s_2_1[5] = { 'i', 'e', 'n', 'd', 'o' };
static const symbol s_2_2[5] = { 'y', 'e', 'n', 'd', 'o' };
static const symbol s_2_3[5] = { 0xC3, 0xA1, 'n', 'd', 'o' };
static const symbol s_2_4[6] = { 'i', 0xC3, 0xA9, 'n', 'd', 'o' };
static const symbol s_2_5[2] = { 'a', 'r' };
static const symbol s_2_6[2] = { 'e', 'r' };
static const symbol s_2_7[2] = { 'i', 'r' };
static const symbol s_2_8[3] = { 0xC3, 0xA1, 'r' };
static const symbol s_2_9[3] = { 0xC3, 0xA9, 'r' };
static const symbol s_2_10[3] = { 0xC3, 0xAD, 'r' };
static const struct among a_2[11] = {
{ 4, s_2_0, 0, 6, 0},
{ 5, s_2_1, 0, 6, 0},
{ 5, s_2_2, 0, 7, 0},
{ 5, s_2_3, 0, 2, 0},
{ 6, s_2_4, 0, 1, 0},
{ 2, s_2_5, 0, 6, 0},
{ 2, s_2_6, 0, 6, 0},
{ 2, s_2_7, 0, 6, 0},
{ 3, s_2_8, 0, 3, 0},
{ 3, s_2_9, 0, 4, 0},
{ 3, s_2_10, 0, 5, 0}
};

static const symbol s_3_0[2] = { 'i', 'c' };
static const symbol s_3_1[2] = { 'a', 'd' };
static const symbol s_3_2[2] = { 'o', 's' };
static const symbol s_3_3[2] = { 'i', 'v' };
static const struct among a_3[4] = {
{ 2, s_3_0, 0, -1, 0},
{ 2, s_3_1, 0, -1, 0},
{ 2, s_3_2, 0, -1, 0},
{ 2, s_3_3, 0, 1, 0}
};

static const symbol s_4_0[4] = { 'a', 'b', 'l', 'e' };
static const symbol s_4_1[4] = { 'i', 'b', 'l', 'e' };
static const symbol s_4_2[4] = { 'a', 'n', 't', 'e' };
static const struct among a_4[3] = {
{ 4, s_4_0, 0, 1, 0},
{ 4, s_4_1, 0, 1, 0},
{ 4, s_4_2, 0, 1, 0}
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
static const symbol s_6_1[5] = { 'a', 'n', 'c', 'i', 'a' };
static const symbol s_6_2[5] = { 'e', 'n', 'c', 'i', 'a' };
static const symbol s_6_3[5] = { 'a', 'd', 'o', 'r', 'a' };
static const symbol s_6_4[3] = { 'o', 's', 'a' };
static const symbol s_6_5[4] = { 'i', 's', 't', 'a' };
static const symbol s_6_6[3] = { 'i', 'v', 'a' };
static const symbol s_6_7[4] = { 'a', 'n', 'z', 'a' };
static const symbol s_6_8[6] = { 'l', 'o', 'g', 0xC3, 0xAD, 'a' };
static const symbol s_6_9[4] = { 'i', 'd', 'a', 'd' };
static const symbol s_6_10[4] = { 'a', 'b', 'l', 'e' };
static const symbol s_6_11[4] = { 'i', 'b', 'l', 'e' };
static const symbol s_6_12[4] = { 'a', 'n', 't', 'e' };
static const symbol s_6_13[5] = { 'm', 'e', 'n', 't', 'e' };
static const symbol s_6_14[6] = { 'a', 'm', 'e', 'n', 't', 'e' };
static const symbol s_6_15[5] = { 'a', 'c', 'i', 'o', 'n' };
static const symbol s_6_16[5] = { 'u', 'c', 'i', 'o', 'n' };
static const symbol s_6_17[6] = { 'a', 'c', 'i', 0xC3, 0xB3, 'n' };
static const symbol s_6_18[6] = { 'u', 'c', 'i', 0xC3, 0xB3, 'n' };
static const symbol s_6_19[3] = { 'i', 'c', 'o' };
static const symbol s_6_20[4] = { 'i', 's', 'm', 'o' };
static const symbol s_6_21[3] = { 'o', 's', 'o' };
static const symbol s_6_22[7] = { 'a', 'm', 'i', 'e', 'n', 't', 'o' };
static const symbol s_6_23[7] = { 'i', 'm', 'i', 'e', 'n', 't', 'o' };
static const symbol s_6_24[3] = { 'i', 'v', 'o' };
static const symbol s_6_25[4] = { 'a', 'd', 'o', 'r' };
static const symbol s_6_26[4] = { 'i', 'c', 'a', 's' };
static const symbol s_6_27[6] = { 'a', 'n', 'c', 'i', 'a', 's' };
static const symbol s_6_28[6] = { 'e', 'n', 'c', 'i', 'a', 's' };
static const symbol s_6_29[6] = { 'a', 'd', 'o', 'r', 'a', 's' };
static const symbol s_6_30[4] = { 'o', 's', 'a', 's' };
static const symbol s_6_31[5] = { 'i', 's', 't', 'a', 's' };
static const symbol s_6_32[4] = { 'i', 'v', 'a', 's' };
static const symbol s_6_33[5] = { 'a', 'n', 'z', 'a', 's' };
static const symbol s_6_34[7] = { 'l', 'o', 'g', 0xC3, 0xAD, 'a', 's' };
static const symbol s_6_35[6] = { 'i', 'd', 'a', 'd', 'e', 's' };
static const symbol s_6_36[5] = { 'a', 'b', 'l', 'e', 's' };
static const symbol s_6_37[5] = { 'i', 'b', 'l', 'e', 's' };
static const symbol s_6_38[7] = { 'a', 'c', 'i', 'o', 'n', 'e', 's' };
static const symbol s_6_39[7] = { 'u', 'c', 'i', 'o', 'n', 'e', 's' };
static const symbol s_6_40[6] = { 'a', 'd', 'o', 'r', 'e', 's' };
static const symbol s_6_41[5] = { 'a', 'n', 't', 'e', 's' };
static const symbol s_6_42[4] = { 'i', 'c', 'o', 's' };
static const symbol s_6_43[5] = { 'i', 's', 'm', 'o', 's' };
static const symbol s_6_44[4] = { 'o', 's', 'o', 's' };
static const symbol s_6_45[8] = { 'a', 'm', 'i', 'e', 'n', 't', 'o', 's' };
static const symbol s_6_46[8] = { 'i', 'm', 'i', 'e', 'n', 't', 'o', 's' };
static const symbol s_6_47[4] = { 'i', 'v', 'o', 's' };
static const struct among a_6[48] = {
{ 3, s_6_0, 0, 1, 0},
{ 5, s_6_1, 0, 2, 0},
{ 5, s_6_2, 0, 5, 0},
{ 5, s_6_3, 0, 2, 0},
{ 3, s_6_4, 0, 1, 0},
{ 4, s_6_5, 0, 1, 0},
{ 3, s_6_6, 0, 9, 0},
{ 4, s_6_7, 0, 1, 0},
{ 6, s_6_8, 0, 3, 0},
{ 4, s_6_9, 0, 8, 0},
{ 4, s_6_10, 0, 1, 0},
{ 4, s_6_11, 0, 1, 0},
{ 4, s_6_12, 0, 2, 0},
{ 5, s_6_13, 0, 7, 0},
{ 6, s_6_14, -1, 6, 0},
{ 5, s_6_15, 0, 2, 0},
{ 5, s_6_16, 0, 4, 0},
{ 6, s_6_17, 0, 2, 0},
{ 6, s_6_18, 0, 4, 0},
{ 3, s_6_19, 0, 1, 0},
{ 4, s_6_20, 0, 1, 0},
{ 3, s_6_21, 0, 1, 0},
{ 7, s_6_22, 0, 1, 0},
{ 7, s_6_23, 0, 1, 0},
{ 3, s_6_24, 0, 9, 0},
{ 4, s_6_25, 0, 2, 0},
{ 4, s_6_26, 0, 1, 0},
{ 6, s_6_27, 0, 2, 0},
{ 6, s_6_28, 0, 5, 0},
{ 6, s_6_29, 0, 2, 0},
{ 4, s_6_30, 0, 1, 0},
{ 5, s_6_31, 0, 1, 0},
{ 4, s_6_32, 0, 9, 0},
{ 5, s_6_33, 0, 1, 0},
{ 7, s_6_34, 0, 3, 0},
{ 6, s_6_35, 0, 8, 0},
{ 5, s_6_36, 0, 1, 0},
{ 5, s_6_37, 0, 1, 0},
{ 7, s_6_38, 0, 2, 0},
{ 7, s_6_39, 0, 4, 0},
{ 6, s_6_40, 0, 2, 0},
{ 5, s_6_41, 0, 2, 0},
{ 4, s_6_42, 0, 1, 0},
{ 5, s_6_43, 0, 1, 0},
{ 4, s_6_44, 0, 1, 0},
{ 8, s_6_45, 0, 1, 0},
{ 8, s_6_46, 0, 1, 0},
{ 4, s_6_47, 0, 9, 0}
};

static const symbol s_7_0[2] = { 'y', 'a' };
static const symbol s_7_1[2] = { 'y', 'e' };
static const symbol s_7_2[3] = { 'y', 'a', 'n' };
static const symbol s_7_3[3] = { 'y', 'e', 'n' };
static const symbol s_7_4[5] = { 'y', 'e', 'r', 'o', 'n' };
static const symbol s_7_5[5] = { 'y', 'e', 'n', 'd', 'o' };
static const symbol s_7_6[2] = { 'y', 'o' };
static const symbol s_7_7[3] = { 'y', 'a', 's' };
static const symbol s_7_8[3] = { 'y', 'e', 's' };
static const symbol s_7_9[4] = { 'y', 'a', 'i', 's' };
static const symbol s_7_10[5] = { 'y', 'a', 'm', 'o', 's' };
static const symbol s_7_11[3] = { 'y', 0xC3, 0xB3 };
static const struct among a_7[12] = {
{ 2, s_7_0, 0, 1, 0},
{ 2, s_7_1, 0, 1, 0},
{ 3, s_7_2, 0, 1, 0},
{ 3, s_7_3, 0, 1, 0},
{ 5, s_7_4, 0, 1, 0},
{ 5, s_7_5, 0, 1, 0},
{ 2, s_7_6, 0, 1, 0},
{ 3, s_7_7, 0, 1, 0},
{ 3, s_7_8, 0, 1, 0},
{ 4, s_7_9, 0, 1, 0},
{ 5, s_7_10, 0, 1, 0},
{ 3, s_7_11, 0, 1, 0}
};

static const symbol s_8_0[3] = { 'a', 'b', 'a' };
static const symbol s_8_1[3] = { 'a', 'd', 'a' };
static const symbol s_8_2[3] = { 'i', 'd', 'a' };
static const symbol s_8_3[3] = { 'a', 'r', 'a' };
static const symbol s_8_4[4] = { 'i', 'e', 'r', 'a' };
static const symbol s_8_5[3] = { 0xC3, 0xAD, 'a' };
static const symbol s_8_6[5] = { 'a', 'r', 0xC3, 0xAD, 'a' };
static const symbol s_8_7[5] = { 'e', 'r', 0xC3, 0xAD, 'a' };
static const symbol s_8_8[5] = { 'i', 'r', 0xC3, 0xAD, 'a' };
static const symbol s_8_9[2] = { 'a', 'd' };
static const symbol s_8_10[2] = { 'e', 'd' };
static const symbol s_8_11[2] = { 'i', 'd' };
static const symbol s_8_12[3] = { 'a', 's', 'e' };
static const symbol s_8_13[4] = { 'i', 'e', 's', 'e' };
static const symbol s_8_14[4] = { 'a', 's', 't', 'e' };
static const symbol s_8_15[4] = { 'i', 's', 't', 'e' };
static const symbol s_8_16[2] = { 'a', 'n' };
static const symbol s_8_17[4] = { 'a', 'b', 'a', 'n' };
static const symbol s_8_18[4] = { 'a', 'r', 'a', 'n' };
static const symbol s_8_19[5] = { 'i', 'e', 'r', 'a', 'n' };
static const symbol s_8_20[4] = { 0xC3, 0xAD, 'a', 'n' };
static const symbol s_8_21[6] = { 'a', 'r', 0xC3, 0xAD, 'a', 'n' };
static const symbol s_8_22[6] = { 'e', 'r', 0xC3, 0xAD, 'a', 'n' };
static const symbol s_8_23[6] = { 'i', 'r', 0xC3, 0xAD, 'a', 'n' };
static const symbol s_8_24[2] = { 'e', 'n' };
static const symbol s_8_25[4] = { 'a', 's', 'e', 'n' };
static const symbol s_8_26[5] = { 'i', 'e', 's', 'e', 'n' };
static const symbol s_8_27[4] = { 'a', 'r', 'o', 'n' };
static const symbol s_8_28[5] = { 'i', 'e', 'r', 'o', 'n' };
static const symbol s_8_29[5] = { 'a', 'r', 0xC3, 0xA1, 'n' };
static const symbol s_8_30[5] = { 'e', 'r', 0xC3, 0xA1, 'n' };
static const symbol s_8_31[5] = { 'i', 'r', 0xC3, 0xA1, 'n' };
static const symbol s_8_32[3] = { 'a', 'd', 'o' };
static const symbol s_8_33[3] = { 'i', 'd', 'o' };
static const symbol s_8_34[4] = { 'a', 'n', 'd', 'o' };
static const symbol s_8_35[5] = { 'i', 'e', 'n', 'd', 'o' };
static const symbol s_8_36[2] = { 'a', 'r' };
static const symbol s_8_37[2] = { 'e', 'r' };
static const symbol s_8_38[2] = { 'i', 'r' };
static const symbol s_8_39[2] = { 'a', 's' };
static const symbol s_8_40[4] = { 'a', 'b', 'a', 's' };
static const symbol s_8_41[4] = { 'a', 'd', 'a', 's' };
static const symbol s_8_42[4] = { 'i', 'd', 'a', 's' };
static const symbol s_8_43[4] = { 'a', 'r', 'a', 's' };
static const symbol s_8_44[5] = { 'i', 'e', 'r', 'a', 's' };
static const symbol s_8_45[4] = { 0xC3, 0xAD, 'a', 's' };
static const symbol s_8_46[6] = { 'a', 'r', 0xC3, 0xAD, 'a', 's' };
static const symbol s_8_47[6] = { 'e', 'r', 0xC3, 0xAD, 'a', 's' };
static const symbol s_8_48[6] = { 'i', 'r', 0xC3, 0xAD, 'a', 's' };
static const symbol s_8_49[2] = { 'e', 's' };
static const symbol s_8_50[4] = { 'a', 's', 'e', 's' };
static const symbol s_8_51[5] = { 'i', 'e', 's', 'e', 's' };
static const symbol s_8_52[5] = { 'a', 'b', 'a', 'i', 's' };
static const symbol s_8_53[5] = { 'a', 'r', 'a', 'i', 's' };
static const symbol s_8_54[6] = { 'i', 'e', 'r', 'a', 'i', 's' };
static const symbol s_8_55[5] = { 0xC3, 0xAD, 'a', 'i', 's' };
static const symbol s_8_56[7] = { 'a', 'r', 0xC3, 0xAD, 'a', 'i', 's' };
static const symbol s_8_57[7] = { 'e', 'r', 0xC3, 0xAD, 'a', 'i', 's' };
static const symbol s_8_58[7] = { 'i', 'r', 0xC3, 0xAD, 'a', 'i', 's' };
static const symbol s_8_59[5] = { 'a', 's', 'e', 'i', 's' };
static const symbol s_8_60[6] = { 'i', 'e', 's', 'e', 'i', 's' };
static const symbol s_8_61[6] = { 'a', 's', 't', 'e', 'i', 's' };
static const symbol s_8_62[6] = { 'i', 's', 't', 'e', 'i', 's' };
static const symbol s_8_63[4] = { 0xC3, 0xA1, 'i', 's' };
static const symbol s_8_64[4] = { 0xC3, 0xA9, 'i', 's' };
static const symbol s_8_65[6] = { 'a', 'r', 0xC3, 0xA9, 'i', 's' };
static const symbol s_8_66[6] = { 'e', 'r', 0xC3, 0xA9, 'i', 's' };
static const symbol s_8_67[6] = { 'i', 'r', 0xC3, 0xA9, 'i', 's' };
static const symbol s_8_68[4] = { 'a', 'd', 'o', 's' };
static const symbol s_8_69[4] = { 'i', 'd', 'o', 's' };
static const symbol s_8_70[4] = { 'a', 'm', 'o', 's' };
static const symbol s_8_71[7] = { 0xC3, 0xA1, 'b', 'a', 'm', 'o', 's' };
static const symbol s_8_72[7] = { 0xC3, 0xA1, 'r', 'a', 'm', 'o', 's' };
static const symbol s_8_73[8] = { 'i', 0xC3, 0xA9, 'r', 'a', 'm', 'o', 's' };
static const symbol s_8_74[6] = { 0xC3, 0xAD, 'a', 'm', 'o', 's' };
static const symbol s_8_75[8] = { 'a', 'r', 0xC3, 0xAD, 'a', 'm', 'o', 's' };
static const symbol s_8_76[8] = { 'e', 'r', 0xC3, 0xAD, 'a', 'm', 'o', 's' };
static const symbol s_8_77[8] = { 'i', 'r', 0xC3, 0xAD, 'a', 'm', 'o', 's' };
static const symbol s_8_78[4] = { 'e', 'm', 'o', 's' };
static const symbol s_8_79[6] = { 'a', 'r', 'e', 'm', 'o', 's' };
static const symbol s_8_80[6] = { 'e', 'r', 'e', 'm', 'o', 's' };
static const symbol s_8_81[6] = { 'i', 'r', 'e', 'm', 'o', 's' };
static const symbol s_8_82[7] = { 0xC3, 0xA1, 's', 'e', 'm', 'o', 's' };
static const symbol s_8_83[8] = { 'i', 0xC3, 0xA9, 's', 'e', 'm', 'o', 's' };
static const symbol s_8_84[4] = { 'i', 'm', 'o', 's' };
static const symbol s_8_85[5] = { 'a', 'r', 0xC3, 0xA1, 's' };
static const symbol s_8_86[5] = { 'e', 'r', 0xC3, 0xA1, 's' };
static const symbol s_8_87[5] = { 'i', 'r', 0xC3, 0xA1, 's' };
static const symbol s_8_88[3] = { 0xC3, 0xAD, 's' };
static const symbol s_8_89[4] = { 'a', 'r', 0xC3, 0xA1 };
static const symbol s_8_90[4] = { 'e', 'r', 0xC3, 0xA1 };
static const symbol s_8_91[4] = { 'i', 'r', 0xC3, 0xA1 };
static const symbol s_8_92[4] = { 'a', 'r', 0xC3, 0xA9 };
static const symbol s_8_93[4] = { 'e', 'r', 0xC3, 0xA9 };
static const symbol s_8_94[4] = { 'i', 'r', 0xC3, 0xA9 };
static const symbol s_8_95[3] = { 'i', 0xC3, 0xB3 };
static const struct among a_8[96] = {
{ 3, s_8_0, 0, 2, 0},
{ 3, s_8_1, 0, 2, 0},
{ 3, s_8_2, 0, 2, 0},
{ 3, s_8_3, 0, 2, 0},
{ 4, s_8_4, 0, 2, 0},
{ 3, s_8_5, 0, 2, 0},
{ 5, s_8_6, -1, 2, 0},
{ 5, s_8_7, -2, 2, 0},
{ 5, s_8_8, -3, 2, 0},
{ 2, s_8_9, 0, 2, 0},
{ 2, s_8_10, 0, 2, 0},
{ 2, s_8_11, 0, 2, 0},
{ 3, s_8_12, 0, 2, 0},
{ 4, s_8_13, 0, 2, 0},
{ 4, s_8_14, 0, 2, 0},
{ 4, s_8_15, 0, 2, 0},
{ 2, s_8_16, 0, 2, 0},
{ 4, s_8_17, -1, 2, 0},
{ 4, s_8_18, -2, 2, 0},
{ 5, s_8_19, -3, 2, 0},
{ 4, s_8_20, -4, 2, 0},
{ 6, s_8_21, -1, 2, 0},
{ 6, s_8_22, -2, 2, 0},
{ 6, s_8_23, -3, 2, 0},
{ 2, s_8_24, 0, 1, 0},
{ 4, s_8_25, -1, 2, 0},
{ 5, s_8_26, -2, 2, 0},
{ 4, s_8_27, 0, 2, 0},
{ 5, s_8_28, 0, 2, 0},
{ 5, s_8_29, 0, 2, 0},
{ 5, s_8_30, 0, 2, 0},
{ 5, s_8_31, 0, 2, 0},
{ 3, s_8_32, 0, 2, 0},
{ 3, s_8_33, 0, 2, 0},
{ 4, s_8_34, 0, 2, 0},
{ 5, s_8_35, 0, 2, 0},
{ 2, s_8_36, 0, 2, 0},
{ 2, s_8_37, 0, 2, 0},
{ 2, s_8_38, 0, 2, 0},
{ 2, s_8_39, 0, 2, 0},
{ 4, s_8_40, -1, 2, 0},
{ 4, s_8_41, -2, 2, 0},
{ 4, s_8_42, -3, 2, 0},
{ 4, s_8_43, -4, 2, 0},
{ 5, s_8_44, -5, 2, 0},
{ 4, s_8_45, -6, 2, 0},
{ 6, s_8_46, -1, 2, 0},
{ 6, s_8_47, -2, 2, 0},
{ 6, s_8_48, -3, 2, 0},
{ 2, s_8_49, 0, 1, 0},
{ 4, s_8_50, -1, 2, 0},
{ 5, s_8_51, -2, 2, 0},
{ 5, s_8_52, 0, 2, 0},
{ 5, s_8_53, 0, 2, 0},
{ 6, s_8_54, 0, 2, 0},
{ 5, s_8_55, 0, 2, 0},
{ 7, s_8_56, -1, 2, 0},
{ 7, s_8_57, -2, 2, 0},
{ 7, s_8_58, -3, 2, 0},
{ 5, s_8_59, 0, 2, 0},
{ 6, s_8_60, 0, 2, 0},
{ 6, s_8_61, 0, 2, 0},
{ 6, s_8_62, 0, 2, 0},
{ 4, s_8_63, 0, 2, 0},
{ 4, s_8_64, 0, 1, 0},
{ 6, s_8_65, -1, 2, 0},
{ 6, s_8_66, -2, 2, 0},
{ 6, s_8_67, -3, 2, 0},
{ 4, s_8_68, 0, 2, 0},
{ 4, s_8_69, 0, 2, 0},
{ 4, s_8_70, 0, 2, 0},
{ 7, s_8_71, -1, 2, 0},
{ 7, s_8_72, -2, 2, 0},
{ 8, s_8_73, -3, 2, 0},
{ 6, s_8_74, -4, 2, 0},
{ 8, s_8_75, -1, 2, 0},
{ 8, s_8_76, -2, 2, 0},
{ 8, s_8_77, -3, 2, 0},
{ 4, s_8_78, 0, 1, 0},
{ 6, s_8_79, -1, 2, 0},
{ 6, s_8_80, -2, 2, 0},
{ 6, s_8_81, -3, 2, 0},
{ 7, s_8_82, -4, 2, 0},
{ 8, s_8_83, -5, 2, 0},
{ 4, s_8_84, 0, 2, 0},
{ 5, s_8_85, 0, 2, 0},
{ 5, s_8_86, 0, 2, 0},
{ 5, s_8_87, 0, 2, 0},
{ 3, s_8_88, 0, 2, 0},
{ 4, s_8_89, 0, 2, 0},
{ 4, s_8_90, 0, 2, 0},
{ 4, s_8_91, 0, 2, 0},
{ 4, s_8_92, 0, 2, 0},
{ 4, s_8_93, 0, 2, 0},
{ 4, s_8_94, 0, 2, 0},
{ 3, s_8_95, 0, 2, 0}
};

static const symbol s_9_0[1] = { 'a' };
static const symbol s_9_1[1] = { 'e' };
static const symbol s_9_2[1] = { 'o' };
static const symbol s_9_3[2] = { 'o', 's' };
static const symbol s_9_4[2] = { 0xC3, 0xA1 };
static const symbol s_9_5[2] = { 0xC3, 0xA9 };
static const symbol s_9_6[2] = { 0xC3, 0xAD };
static const symbol s_9_7[2] = { 0xC3, 0xB3 };
static const struct among a_9[8] = {
{ 1, s_9_0, 0, 1, 0},
{ 1, s_9_1, 0, 2, 0},
{ 1, s_9_2, 0, 1, 0},
{ 2, s_9_3, 0, 1, 0},
{ 2, s_9_4, 0, 1, 0},
{ 2, s_9_5, 0, 2, 0},
{ 2, s_9_6, 0, 1, 0},
{ 2, s_9_7, 0, 1, 0}
};

static const unsigned char g_v[] = { 17, 65, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 17, 4, 10 };

static int r_mark_regions(struct SN_env * z) {
    ((SN_local *)z)->i_pV = z->l;
    ((SN_local *)z)->i_p1 = z->l;
    ((SN_local *)z)->i_p2 = z->l;
    {
        int v_1 = z->c;
        do {
            int v_2 = z->c;
            if (in_grouping_U(z, g_v, 97, 252, 0)) goto lab1;
            do {
                int v_3 = z->c;
                if (out_grouping_U(z, g_v, 97, 252, 0)) goto lab2;
                {
                    int ret = out_grouping_U(z, g_v, 97, 252, 1);
                    if (ret < 0) goto lab2;
                    z->c += ret;
                }
                break;
            lab2:
                z->c = v_3;
                if (in_grouping_U(z, g_v, 97, 252, 0)) goto lab1;
                {
                    int ret = in_grouping_U(z, g_v, 97, 252, 1);
                    if (ret < 0) goto lab1;
                    z->c += ret;
                }
            } while (0);
            break;
        lab1:
            z->c = v_2;
            if (out_grouping_U(z, g_v, 97, 252, 0)) goto lab0;
            do {
                int v_4 = z->c;
                if (out_grouping_U(z, g_v, 97, 252, 0)) goto lab3;
                {
                    int ret = out_grouping_U(z, g_v, 97, 252, 1);
                    if (ret < 0) goto lab3;
                    z->c += ret;
                }
                break;
            lab3:
                z->c = v_4;
                if (in_grouping_U(z, g_v, 97, 252, 0)) goto lab0;
                {
                    int ret = skip_utf8(z->p, z->c, z->l, 1);
                    if (ret < 0) goto lab0;
                    z->c = ret;
                }
            } while (0);
        } while (0);
        ((SN_local *)z)->i_pV = z->c;
    lab0:
        z->c = v_1;
    }
    {
        int v_5 = z->c;
        {
            int ret = out_grouping_U(z, g_v, 97, 252, 1);
            if (ret < 0) goto lab4;
            z->c += ret;
        }
        {
            int ret = in_grouping_U(z, g_v, 97, 252, 1);
            if (ret < 0) goto lab4;
            z->c += ret;
        }
        ((SN_local *)z)->i_p1 = z->c;
        {
            int ret = out_grouping_U(z, g_v, 97, 252, 1);
            if (ret < 0) goto lab4;
            z->c += ret;
        }
        {
            int ret = in_grouping_U(z, g_v, 97, 252, 1);
            if (ret < 0) goto lab4;
            z->c += ret;
        }
        ((SN_local *)z)->i_p2 = z->c;
    lab4:
        z->c = v_5;
    }
    return 1;
}

static int r_postlude(struct SN_env * z) {
    int among_var;
    while (1) {
        int v_1 = z->c;
        z->bra = z->c;
        if (z->c + 1 >= z->l || z->p[z->c + 1] >> 5 != 5 || !((67641858 >> (z->p[z->c + 1] & 0x1f)) & 1)) among_var = 6; else
        among_var = find_among(z, a_0, 6, 0);
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
    if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((557090 >> (z->p[z->c - 1] & 0x1f)) & 1)) return 0;
    if (!find_among_b(z, a_1, 13, 0)) return 0;
    z->bra = z->c;
    if (z->c - 1 <= z->lb || (z->p[z->c - 1] != 111 && z->p[z->c - 1] != 114)) return 0;
    among_var = find_among_b(z, a_2, 11, 0);
    if (!among_var) return 0;
    {
        int ret = r_RV(z);
        if (ret <= 0) return ret;
    }
    switch (among_var) {
        case 1:
            z->bra = z->c;
            {
                int ret = slice_from_s(z, 5, s_5);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            z->bra = z->c;
            {
                int ret = slice_from_s(z, 4, s_6);
                if (ret < 0) return ret;
            }
            break;
        case 3:
            z->bra = z->c;
            {
                int ret = slice_from_s(z, 2, s_7);
                if (ret < 0) return ret;
            }
            break;
        case 4:
            z->bra = z->c;
            {
                int ret = slice_from_s(z, 2, s_8);
                if (ret < 0) return ret;
            }
            break;
        case 5:
            z->bra = z->c;
            {
                int ret = slice_from_s(z, 2, s_9);
                if (ret < 0) return ret;
            }
            break;
        case 6:
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 7:
            if (z->c <= z->lb || z->p[z->c - 1] != 'u') return 0;
            z->c--;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_standard_suffix(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    if (z->c - 2 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((835634 >> (z->p[z->c - 1] & 0x1f)) & 1)) return 0;
    among_var = find_among_b(z, a_6, 48, 0);
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
                if (!(eq_s_b(z, 2, s_10))) { z->c = z->l - v_1; goto lab0; }
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
                int ret = slice_from_s(z, 3, s_11);
                if (ret < 0) return ret;
            }
            break;
        case 4:
            {
                int ret = r_R2(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_from_s(z, 1, s_12);
                if (ret < 0) return ret;
            }
            break;
        case 5:
            {
                int ret = r_R2(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_from_s(z, 4, s_13);
                if (ret < 0) return ret;
            }
            break;
        case 6:
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
                if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((4718616 >> (z->p[z->c - 1] & 0x1f)) & 1)) { z->c = z->l - v_2; goto lab1; }
                among_var = find_among_b(z, a_3, 4, 0);
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
                        if (!(eq_s_b(z, 2, s_14))) { z->c = z->l - v_2; goto lab1; }
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
        case 7:
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
                if (z->c - 3 <= z->lb || z->p[z->c - 1] != 101) { z->c = z->l - v_3; goto lab2; }
                if (!find_among_b(z, a_4, 3, 0)) { z->c = z->l - v_3; goto lab2; }
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
                int v_4 = z->l - z->c;
                z->ket = z->c;
                if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((4198408 >> (z->p[z->c - 1] & 0x1f)) & 1)) { z->c = z->l - v_4; goto lab3; }
                if (!find_among_b(z, a_5, 3, 0)) { z->c = z->l - v_4; goto lab3; }
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
                int v_5 = z->l - z->c;
                z->ket = z->c;
                if (!(eq_s_b(z, 2, s_15))) { z->c = z->l - v_5; goto lab4; }
                z->bra = z->c;
                {
                    int ret = r_R2(z);
                    if (ret == 0) { z->c = z->l - v_5; goto lab4; }
                    if (ret < 0) return ret;
                }
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
            lab4:
                ;
            }
            break;
    }
    return 1;
}

static int r_y_verb_suffix(struct SN_env * z) {
    {
        int v_1;
        if (z->c < ((SN_local *)z)->i_pV) return 0;
        v_1 = z->lb; z->lb = ((SN_local *)z)->i_pV;
        z->ket = z->c;
        if (!find_among_b(z, a_7, 12, 0)) { z->lb = v_1; return 0; }
        z->bra = z->c;
        z->lb = v_1;
    }
    if (z->c <= z->lb || z->p[z->c - 1] != 'u') return 0;
    z->c--;
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_verb_suffix(struct SN_env * z) {
    int among_var;
    {
        int v_1;
        if (z->c < ((SN_local *)z)->i_pV) return 0;
        v_1 = z->lb; z->lb = ((SN_local *)z)->i_pV;
        z->ket = z->c;
        among_var = find_among_b(z, a_8, 96, 0);
        if (!among_var) { z->lb = v_1; return 0; }
        z->bra = z->c;
        z->lb = v_1;
    }
    switch (among_var) {
        case 1:
            {
                int v_2 = z->l - z->c;
                if (z->c <= z->lb || z->p[z->c - 1] != 'u') { z->c = z->l - v_2; goto lab0; }
                z->c--;
                {
                    int v_3 = z->l - z->c;
                    if (z->c <= z->lb || z->p[z->c - 1] != 'g') { z->c = z->l - v_2; goto lab0; }
                    z->c--;
                    z->c = z->l - v_3;
                }
            lab0:
                ;
            }
            z->bra = z->c;
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

static int r_residual_suffix(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    among_var = find_among_b(z, a_9, 8, 0);
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
                int ret = r_RV(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            {
                int v_1 = z->l - z->c;
                z->ket = z->c;
                if (z->c <= z->lb || z->p[z->c - 1] != 'u') { z->c = z->l - v_1; goto lab0; }
                z->c--;
                z->bra = z->c;
                {
                    int v_2 = z->l - z->c;
                    if (z->c <= z->lb || z->p[z->c - 1] != 'g') { z->c = z->l - v_1; goto lab0; }
                    z->c--;
                    z->c = z->l - v_2;
                }
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
            break;
    }
    return 1;
}

extern int spanish_UTF_8_stem(struct SN_env * z) {
    {
        int ret = r_mark_regions(z);
        if (ret < 0) return ret;
    }
    z->lb = z->c; z->c = z->l;
    {
        int v_1 = z->l - z->c;
        {
            int ret = r_attached_pronoun(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_1;
    }
    {
        int v_2 = z->l - z->c;
        do {
            int v_3 = z->l - z->c;
            {
                int ret = r_standard_suffix(z);
                if (ret == 0) goto lab1;
                if (ret < 0) return ret;
            }
            break;
        lab1:
            z->c = z->l - v_3;
            {
                int ret = r_y_verb_suffix(z);
                if (ret == 0) goto lab2;
                if (ret < 0) return ret;
            }
            break;
        lab2:
            z->c = z->l - v_3;
            {
                int ret = r_verb_suffix(z);
                if (ret == 0) goto lab0;
                if (ret < 0) return ret;
            }
        } while (0);
    lab0:
        z->c = z->l - v_2;
    }
    {
        int v_4 = z->l - z->c;
        {
            int ret = r_residual_suffix(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_4;
    }
    z->c = z->lb;
    {
        int v_5 = z->c;
        {
            int ret = r_postlude(z);
            if (ret < 0) return ret;
        }
        z->c = v_5;
    }
    return 1;
}

extern struct SN_env * spanish_UTF_8_create_env(void) {
    struct SN_env * z = SN_new_env(sizeof(SN_local));
    if (z) {
        ((SN_local *)z)->i_p2 = 0;
        ((SN_local *)z)->i_p1 = 0;
        ((SN_local *)z)->i_pV = 0;
    }
    return z;
}

extern void spanish_UTF_8_close_env(struct SN_env * z) {
    SN_delete_env(z);
}

