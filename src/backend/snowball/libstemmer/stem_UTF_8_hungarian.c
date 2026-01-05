/* Generated from hungarian.sbl by Snowball 3.0.0 - https://snowballstem.org/ */

#include "stem_UTF_8_hungarian.h"

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
extern int hungarian_UTF_8_stem(struct SN_env * z);
#ifdef __cplusplus
}
#endif

static int r_double(struct SN_env * z);
static int r_undouble(struct SN_env * z);
static int r_factive(struct SN_env * z);
static int r_instrum(struct SN_env * z);
static int r_plur_owner(struct SN_env * z);
static int r_sing_owner(struct SN_env * z);
static int r_owned(struct SN_env * z);
static int r_plural(struct SN_env * z);
static int r_case_other(struct SN_env * z);
static int r_case_special(struct SN_env * z);
static int r_case(struct SN_env * z);
static int r_v_ending(struct SN_env * z);
static int r_R1(struct SN_env * z);
static int r_mark_regions(struct SN_env * z);

static const symbol s_0[] = { 'a' };
static const symbol s_1[] = { 'e' };
static const symbol s_2[] = { 'e' };
static const symbol s_3[] = { 'a' };
static const symbol s_4[] = { 'a' };
static const symbol s_5[] = { 'e' };
static const symbol s_6[] = { 'a' };
static const symbol s_7[] = { 'e' };
static const symbol s_8[] = { 'e' };
static const symbol s_9[] = { 'a' };
static const symbol s_10[] = { 'a' };
static const symbol s_11[] = { 'e' };
static const symbol s_12[] = { 'a' };
static const symbol s_13[] = { 'e' };

static const symbol s_0_0[2] = { 0xC3, 0xA1 };
static const symbol s_0_1[2] = { 0xC3, 0xA9 };
static const struct among a_0[2] = {
{ 2, s_0_0, 0, 1, 0},
{ 2, s_0_1, 0, 2, 0}
};

static const symbol s_1_0[2] = { 'b', 'b' };
static const symbol s_1_1[2] = { 'c', 'c' };
static const symbol s_1_2[2] = { 'd', 'd' };
static const symbol s_1_3[2] = { 'f', 'f' };
static const symbol s_1_4[2] = { 'g', 'g' };
static const symbol s_1_5[2] = { 'j', 'j' };
static const symbol s_1_6[2] = { 'k', 'k' };
static const symbol s_1_7[2] = { 'l', 'l' };
static const symbol s_1_8[2] = { 'm', 'm' };
static const symbol s_1_9[2] = { 'n', 'n' };
static const symbol s_1_10[2] = { 'p', 'p' };
static const symbol s_1_11[2] = { 'r', 'r' };
static const symbol s_1_12[3] = { 'c', 'c', 's' };
static const symbol s_1_13[2] = { 's', 's' };
static const symbol s_1_14[3] = { 'z', 'z', 's' };
static const symbol s_1_15[2] = { 't', 't' };
static const symbol s_1_16[2] = { 'v', 'v' };
static const symbol s_1_17[3] = { 'g', 'g', 'y' };
static const symbol s_1_18[3] = { 'l', 'l', 'y' };
static const symbol s_1_19[3] = { 'n', 'n', 'y' };
static const symbol s_1_20[3] = { 't', 't', 'y' };
static const symbol s_1_21[3] = { 's', 's', 'z' };
static const symbol s_1_22[2] = { 'z', 'z' };
static const struct among a_1[23] = {
{ 2, s_1_0, 0, -1, 0},
{ 2, s_1_1, 0, -1, 0},
{ 2, s_1_2, 0, -1, 0},
{ 2, s_1_3, 0, -1, 0},
{ 2, s_1_4, 0, -1, 0},
{ 2, s_1_5, 0, -1, 0},
{ 2, s_1_6, 0, -1, 0},
{ 2, s_1_7, 0, -1, 0},
{ 2, s_1_8, 0, -1, 0},
{ 2, s_1_9, 0, -1, 0},
{ 2, s_1_10, 0, -1, 0},
{ 2, s_1_11, 0, -1, 0},
{ 3, s_1_12, 0, -1, 0},
{ 2, s_1_13, 0, -1, 0},
{ 3, s_1_14, 0, -1, 0},
{ 2, s_1_15, 0, -1, 0},
{ 2, s_1_16, 0, -1, 0},
{ 3, s_1_17, 0, -1, 0},
{ 3, s_1_18, 0, -1, 0},
{ 3, s_1_19, 0, -1, 0},
{ 3, s_1_20, 0, -1, 0},
{ 3, s_1_21, 0, -1, 0},
{ 2, s_1_22, 0, -1, 0}
};

static const symbol s_2_0[2] = { 'a', 'l' };
static const symbol s_2_1[2] = { 'e', 'l' };
static const struct among a_2[2] = {
{ 2, s_2_0, 0, 1, 0},
{ 2, s_2_1, 0, 1, 0}
};

static const symbol s_3_0[2] = { 'b', 'a' };
static const symbol s_3_1[2] = { 'r', 'a' };
static const symbol s_3_2[2] = { 'b', 'e' };
static const symbol s_3_3[2] = { 'r', 'e' };
static const symbol s_3_4[2] = { 'i', 'g' };
static const symbol s_3_5[3] = { 'n', 'a', 'k' };
static const symbol s_3_6[3] = { 'n', 'e', 'k' };
static const symbol s_3_7[3] = { 'v', 'a', 'l' };
static const symbol s_3_8[3] = { 'v', 'e', 'l' };
static const symbol s_3_9[2] = { 'u', 'l' };
static const symbol s_3_10[4] = { 'b', 0xC5, 0x91, 'l' };
static const symbol s_3_11[4] = { 'r', 0xC5, 0x91, 'l' };
static const symbol s_3_12[4] = { 't', 0xC5, 0x91, 'l' };
static const symbol s_3_13[4] = { 'n', 0xC3, 0xA1, 'l' };
static const symbol s_3_14[4] = { 'n', 0xC3, 0xA9, 'l' };
static const symbol s_3_15[4] = { 'b', 0xC3, 0xB3, 'l' };
static const symbol s_3_16[4] = { 'r', 0xC3, 0xB3, 'l' };
static const symbol s_3_17[4] = { 't', 0xC3, 0xB3, 'l' };
static const symbol s_3_18[3] = { 0xC3, 0xBC, 'l' };
static const symbol s_3_19[1] = { 'n' };
static const symbol s_3_20[2] = { 'a', 'n' };
static const symbol s_3_21[3] = { 'b', 'a', 'n' };
static const symbol s_3_22[2] = { 'e', 'n' };
static const symbol s_3_23[3] = { 'b', 'e', 'n' };
static const symbol s_3_24[7] = { 'k', 0xC3, 0xA9, 'p', 'p', 'e', 'n' };
static const symbol s_3_25[2] = { 'o', 'n' };
static const symbol s_3_26[3] = { 0xC3, 0xB6, 'n' };
static const symbol s_3_27[5] = { 'k', 0xC3, 0xA9, 'p', 'p' };
static const symbol s_3_28[3] = { 'k', 'o', 'r' };
static const symbol s_3_29[1] = { 't' };
static const symbol s_3_30[2] = { 'a', 't' };
static const symbol s_3_31[2] = { 'e', 't' };
static const symbol s_3_32[5] = { 'k', 0xC3, 0xA9, 'n', 't' };
static const symbol s_3_33[7] = { 'a', 'n', 'k', 0xC3, 0xA9, 'n', 't' };
static const symbol s_3_34[7] = { 'e', 'n', 'k', 0xC3, 0xA9, 'n', 't' };
static const symbol s_3_35[7] = { 'o', 'n', 'k', 0xC3, 0xA9, 'n', 't' };
static const symbol s_3_36[2] = { 'o', 't' };
static const symbol s_3_37[4] = { 0xC3, 0xA9, 'r', 't' };
static const symbol s_3_38[3] = { 0xC3, 0xB6, 't' };
static const symbol s_3_39[3] = { 'h', 'e', 'z' };
static const symbol s_3_40[3] = { 'h', 'o', 'z' };
static const symbol s_3_41[4] = { 'h', 0xC3, 0xB6, 'z' };
static const symbol s_3_42[3] = { 'v', 0xC3, 0xA1 };
static const symbol s_3_43[3] = { 'v', 0xC3, 0xA9 };
static const struct among a_3[44] = {
{ 2, s_3_0, 0, -1, 0},
{ 2, s_3_1, 0, -1, 0},
{ 2, s_3_2, 0, -1, 0},
{ 2, s_3_3, 0, -1, 0},
{ 2, s_3_4, 0, -1, 0},
{ 3, s_3_5, 0, -1, 0},
{ 3, s_3_6, 0, -1, 0},
{ 3, s_3_7, 0, -1, 0},
{ 3, s_3_8, 0, -1, 0},
{ 2, s_3_9, 0, -1, 0},
{ 4, s_3_10, 0, -1, 0},
{ 4, s_3_11, 0, -1, 0},
{ 4, s_3_12, 0, -1, 0},
{ 4, s_3_13, 0, -1, 0},
{ 4, s_3_14, 0, -1, 0},
{ 4, s_3_15, 0, -1, 0},
{ 4, s_3_16, 0, -1, 0},
{ 4, s_3_17, 0, -1, 0},
{ 3, s_3_18, 0, -1, 0},
{ 1, s_3_19, 0, -1, 0},
{ 2, s_3_20, -1, -1, 0},
{ 3, s_3_21, -1, -1, 0},
{ 2, s_3_22, -3, -1, 0},
{ 3, s_3_23, -1, -1, 0},
{ 7, s_3_24, -2, -1, 0},
{ 2, s_3_25, -6, -1, 0},
{ 3, s_3_26, -7, -1, 0},
{ 5, s_3_27, 0, -1, 0},
{ 3, s_3_28, 0, -1, 0},
{ 1, s_3_29, 0, -1, 0},
{ 2, s_3_30, -1, -1, 0},
{ 2, s_3_31, -2, -1, 0},
{ 5, s_3_32, -3, -1, 0},
{ 7, s_3_33, -1, -1, 0},
{ 7, s_3_34, -2, -1, 0},
{ 7, s_3_35, -3, -1, 0},
{ 2, s_3_36, -7, -1, 0},
{ 4, s_3_37, -8, -1, 0},
{ 3, s_3_38, -9, -1, 0},
{ 3, s_3_39, 0, -1, 0},
{ 3, s_3_40, 0, -1, 0},
{ 4, s_3_41, 0, -1, 0},
{ 3, s_3_42, 0, -1, 0},
{ 3, s_3_43, 0, -1, 0}
};

static const symbol s_4_0[3] = { 0xC3, 0xA1, 'n' };
static const symbol s_4_1[3] = { 0xC3, 0xA9, 'n' };
static const symbol s_4_2[8] = { 0xC3, 0xA1, 'n', 'k', 0xC3, 0xA9, 'n', 't' };
static const struct among a_4[3] = {
{ 3, s_4_0, 0, 2, 0},
{ 3, s_4_1, 0, 1, 0},
{ 8, s_4_2, 0, 2, 0}
};

static const symbol s_5_0[4] = { 's', 't', 'u', 'l' };
static const symbol s_5_1[5] = { 'a', 's', 't', 'u', 'l' };
static const symbol s_5_2[6] = { 0xC3, 0xA1, 's', 't', 'u', 'l' };
static const symbol s_5_3[5] = { 's', 't', 0xC3, 0xBC, 'l' };
static const symbol s_5_4[6] = { 'e', 's', 't', 0xC3, 0xBC, 'l' };
static const symbol s_5_5[7] = { 0xC3, 0xA9, 's', 't', 0xC3, 0xBC, 'l' };
static const struct among a_5[6] = {
{ 4, s_5_0, 0, 1, 0},
{ 5, s_5_1, -1, 1, 0},
{ 6, s_5_2, -2, 2, 0},
{ 5, s_5_3, 0, 1, 0},
{ 6, s_5_4, -1, 1, 0},
{ 7, s_5_5, -2, 3, 0}
};

static const symbol s_6_0[2] = { 0xC3, 0xA1 };
static const symbol s_6_1[2] = { 0xC3, 0xA9 };
static const struct among a_6[2] = {
{ 2, s_6_0, 0, 1, 0},
{ 2, s_6_1, 0, 1, 0}
};

static const symbol s_7_0[1] = { 'k' };
static const symbol s_7_1[2] = { 'a', 'k' };
static const symbol s_7_2[2] = { 'e', 'k' };
static const symbol s_7_3[2] = { 'o', 'k' };
static const symbol s_7_4[3] = { 0xC3, 0xA1, 'k' };
static const symbol s_7_5[3] = { 0xC3, 0xA9, 'k' };
static const symbol s_7_6[3] = { 0xC3, 0xB6, 'k' };
static const struct among a_7[7] = {
{ 1, s_7_0, 0, 3, 0},
{ 2, s_7_1, -1, 3, 0},
{ 2, s_7_2, -2, 3, 0},
{ 2, s_7_3, -3, 3, 0},
{ 3, s_7_4, -4, 1, 0},
{ 3, s_7_5, -5, 2, 0},
{ 3, s_7_6, -6, 3, 0}
};

static const symbol s_8_0[3] = { 0xC3, 0xA9, 'i' };
static const symbol s_8_1[5] = { 0xC3, 0xA1, 0xC3, 0xA9, 'i' };
static const symbol s_8_2[5] = { 0xC3, 0xA9, 0xC3, 0xA9, 'i' };
static const symbol s_8_3[2] = { 0xC3, 0xA9 };
static const symbol s_8_4[3] = { 'k', 0xC3, 0xA9 };
static const symbol s_8_5[4] = { 'a', 'k', 0xC3, 0xA9 };
static const symbol s_8_6[4] = { 'e', 'k', 0xC3, 0xA9 };
static const symbol s_8_7[4] = { 'o', 'k', 0xC3, 0xA9 };
static const symbol s_8_8[5] = { 0xC3, 0xA1, 'k', 0xC3, 0xA9 };
static const symbol s_8_9[5] = { 0xC3, 0xA9, 'k', 0xC3, 0xA9 };
static const symbol s_8_10[5] = { 0xC3, 0xB6, 'k', 0xC3, 0xA9 };
static const symbol s_8_11[4] = { 0xC3, 0xA9, 0xC3, 0xA9 };
static const struct among a_8[12] = {
{ 3, s_8_0, 0, 1, 0},
{ 5, s_8_1, -1, 3, 0},
{ 5, s_8_2, -2, 2, 0},
{ 2, s_8_3, 0, 1, 0},
{ 3, s_8_4, -1, 1, 0},
{ 4, s_8_5, -1, 1, 0},
{ 4, s_8_6, -2, 1, 0},
{ 4, s_8_7, -3, 1, 0},
{ 5, s_8_8, -4, 3, 0},
{ 5, s_8_9, -5, 2, 0},
{ 5, s_8_10, -6, 1, 0},
{ 4, s_8_11, -8, 2, 0}
};

static const symbol s_9_0[1] = { 'a' };
static const symbol s_9_1[2] = { 'j', 'a' };
static const symbol s_9_2[1] = { 'd' };
static const symbol s_9_3[2] = { 'a', 'd' };
static const symbol s_9_4[2] = { 'e', 'd' };
static const symbol s_9_5[2] = { 'o', 'd' };
static const symbol s_9_6[3] = { 0xC3, 0xA1, 'd' };
static const symbol s_9_7[3] = { 0xC3, 0xA9, 'd' };
static const symbol s_9_8[3] = { 0xC3, 0xB6, 'd' };
static const symbol s_9_9[1] = { 'e' };
static const symbol s_9_10[2] = { 'j', 'e' };
static const symbol s_9_11[2] = { 'n', 'k' };
static const symbol s_9_12[3] = { 'u', 'n', 'k' };
static const symbol s_9_13[4] = { 0xC3, 0xA1, 'n', 'k' };
static const symbol s_9_14[4] = { 0xC3, 0xA9, 'n', 'k' };
static const symbol s_9_15[4] = { 0xC3, 0xBC, 'n', 'k' };
static const symbol s_9_16[2] = { 'u', 'k' };
static const symbol s_9_17[3] = { 'j', 'u', 'k' };
static const symbol s_9_18[5] = { 0xC3, 0xA1, 'j', 'u', 'k' };
static const symbol s_9_19[3] = { 0xC3, 0xBC, 'k' };
static const symbol s_9_20[4] = { 'j', 0xC3, 0xBC, 'k' };
static const symbol s_9_21[6] = { 0xC3, 0xA9, 'j', 0xC3, 0xBC, 'k' };
static const symbol s_9_22[1] = { 'm' };
static const symbol s_9_23[2] = { 'a', 'm' };
static const symbol s_9_24[2] = { 'e', 'm' };
static const symbol s_9_25[2] = { 'o', 'm' };
static const symbol s_9_26[3] = { 0xC3, 0xA1, 'm' };
static const symbol s_9_27[3] = { 0xC3, 0xA9, 'm' };
static const symbol s_9_28[1] = { 'o' };
static const symbol s_9_29[2] = { 0xC3, 0xA1 };
static const symbol s_9_30[2] = { 0xC3, 0xA9 };
static const struct among a_9[31] = {
{ 1, s_9_0, 0, 1, 0},
{ 2, s_9_1, -1, 1, 0},
{ 1, s_9_2, 0, 1, 0},
{ 2, s_9_3, -1, 1, 0},
{ 2, s_9_4, -2, 1, 0},
{ 2, s_9_5, -3, 1, 0},
{ 3, s_9_6, -4, 2, 0},
{ 3, s_9_7, -5, 3, 0},
{ 3, s_9_8, -6, 1, 0},
{ 1, s_9_9, 0, 1, 0},
{ 2, s_9_10, -1, 1, 0},
{ 2, s_9_11, 0, 1, 0},
{ 3, s_9_12, -1, 1, 0},
{ 4, s_9_13, -2, 2, 0},
{ 4, s_9_14, -3, 3, 0},
{ 4, s_9_15, -4, 1, 0},
{ 2, s_9_16, 0, 1, 0},
{ 3, s_9_17, -1, 1, 0},
{ 5, s_9_18, -1, 2, 0},
{ 3, s_9_19, 0, 1, 0},
{ 4, s_9_20, -1, 1, 0},
{ 6, s_9_21, -1, 3, 0},
{ 1, s_9_22, 0, 1, 0},
{ 2, s_9_23, -1, 1, 0},
{ 2, s_9_24, -2, 1, 0},
{ 2, s_9_25, -3, 1, 0},
{ 3, s_9_26, -4, 2, 0},
{ 3, s_9_27, -5, 3, 0},
{ 1, s_9_28, 0, 1, 0},
{ 2, s_9_29, 0, 2, 0},
{ 2, s_9_30, 0, 3, 0}
};

static const symbol s_10_0[2] = { 'i', 'd' };
static const symbol s_10_1[3] = { 'a', 'i', 'd' };
static const symbol s_10_2[4] = { 'j', 'a', 'i', 'd' };
static const symbol s_10_3[3] = { 'e', 'i', 'd' };
static const symbol s_10_4[4] = { 'j', 'e', 'i', 'd' };
static const symbol s_10_5[4] = { 0xC3, 0xA1, 'i', 'd' };
static const symbol s_10_6[4] = { 0xC3, 0xA9, 'i', 'd' };
static const symbol s_10_7[1] = { 'i' };
static const symbol s_10_8[2] = { 'a', 'i' };
static const symbol s_10_9[3] = { 'j', 'a', 'i' };
static const symbol s_10_10[2] = { 'e', 'i' };
static const symbol s_10_11[3] = { 'j', 'e', 'i' };
static const symbol s_10_12[3] = { 0xC3, 0xA1, 'i' };
static const symbol s_10_13[3] = { 0xC3, 0xA9, 'i' };
static const symbol s_10_14[4] = { 'i', 't', 'e', 'k' };
static const symbol s_10_15[5] = { 'e', 'i', 't', 'e', 'k' };
static const symbol s_10_16[6] = { 'j', 'e', 'i', 't', 'e', 'k' };
static const symbol s_10_17[6] = { 0xC3, 0xA9, 'i', 't', 'e', 'k' };
static const symbol s_10_18[2] = { 'i', 'k' };
static const symbol s_10_19[3] = { 'a', 'i', 'k' };
static const symbol s_10_20[4] = { 'j', 'a', 'i', 'k' };
static const symbol s_10_21[3] = { 'e', 'i', 'k' };
static const symbol s_10_22[4] = { 'j', 'e', 'i', 'k' };
static const symbol s_10_23[4] = { 0xC3, 0xA1, 'i', 'k' };
static const symbol s_10_24[4] = { 0xC3, 0xA9, 'i', 'k' };
static const symbol s_10_25[3] = { 'i', 'n', 'k' };
static const symbol s_10_26[4] = { 'a', 'i', 'n', 'k' };
static const symbol s_10_27[5] = { 'j', 'a', 'i', 'n', 'k' };
static const symbol s_10_28[4] = { 'e', 'i', 'n', 'k' };
static const symbol s_10_29[5] = { 'j', 'e', 'i', 'n', 'k' };
static const symbol s_10_30[5] = { 0xC3, 0xA1, 'i', 'n', 'k' };
static const symbol s_10_31[5] = { 0xC3, 0xA9, 'i', 'n', 'k' };
static const symbol s_10_32[5] = { 'a', 'i', 't', 'o', 'k' };
static const symbol s_10_33[6] = { 'j', 'a', 'i', 't', 'o', 'k' };
static const symbol s_10_34[6] = { 0xC3, 0xA1, 'i', 't', 'o', 'k' };
static const symbol s_10_35[2] = { 'i', 'm' };
static const symbol s_10_36[3] = { 'a', 'i', 'm' };
static const symbol s_10_37[4] = { 'j', 'a', 'i', 'm' };
static const symbol s_10_38[3] = { 'e', 'i', 'm' };
static const symbol s_10_39[4] = { 'j', 'e', 'i', 'm' };
static const symbol s_10_40[4] = { 0xC3, 0xA1, 'i', 'm' };
static const symbol s_10_41[4] = { 0xC3, 0xA9, 'i', 'm' };
static const struct among a_10[42] = {
{ 2, s_10_0, 0, 1, 0},
{ 3, s_10_1, -1, 1, 0},
{ 4, s_10_2, -1, 1, 0},
{ 3, s_10_3, -3, 1, 0},
{ 4, s_10_4, -1, 1, 0},
{ 4, s_10_5, -5, 2, 0},
{ 4, s_10_6, -6, 3, 0},
{ 1, s_10_7, 0, 1, 0},
{ 2, s_10_8, -1, 1, 0},
{ 3, s_10_9, -1, 1, 0},
{ 2, s_10_10, -3, 1, 0},
{ 3, s_10_11, -1, 1, 0},
{ 3, s_10_12, -5, 2, 0},
{ 3, s_10_13, -6, 3, 0},
{ 4, s_10_14, 0, 1, 0},
{ 5, s_10_15, -1, 1, 0},
{ 6, s_10_16, -1, 1, 0},
{ 6, s_10_17, -3, 3, 0},
{ 2, s_10_18, 0, 1, 0},
{ 3, s_10_19, -1, 1, 0},
{ 4, s_10_20, -1, 1, 0},
{ 3, s_10_21, -3, 1, 0},
{ 4, s_10_22, -1, 1, 0},
{ 4, s_10_23, -5, 2, 0},
{ 4, s_10_24, -6, 3, 0},
{ 3, s_10_25, 0, 1, 0},
{ 4, s_10_26, -1, 1, 0},
{ 5, s_10_27, -1, 1, 0},
{ 4, s_10_28, -3, 1, 0},
{ 5, s_10_29, -1, 1, 0},
{ 5, s_10_30, -5, 2, 0},
{ 5, s_10_31, -6, 3, 0},
{ 5, s_10_32, 0, 1, 0},
{ 6, s_10_33, -1, 1, 0},
{ 6, s_10_34, 0, 2, 0},
{ 2, s_10_35, 0, 1, 0},
{ 3, s_10_36, -1, 1, 0},
{ 4, s_10_37, -1, 1, 0},
{ 3, s_10_38, -3, 1, 0},
{ 4, s_10_39, -1, 1, 0},
{ 4, s_10_40, -5, 2, 0},
{ 4, s_10_41, -6, 3, 0}
};

static const unsigned char g_v[] = { 17, 65, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 17, 36, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1 };

static int r_mark_regions(struct SN_env * z) {
    ((SN_local *)z)->i_p1 = z->l;
    do {
        int v_1 = z->c;
        if (in_grouping_U(z, g_v, 97, 369, 0)) goto lab0;
        {
            int v_2 = z->c;
            {
                int ret = in_grouping_U(z, g_v, 97, 369, 1);
                if (ret < 0) goto lab1;
                z->c += ret;
            }
            ((SN_local *)z)->i_p1 = z->c;
        lab1:
            z->c = v_2;
        }
        break;
    lab0:
        z->c = v_1;
        {
            int ret = out_grouping_U(z, g_v, 97, 369, 1);
            if (ret < 0) return 0;
            z->c += ret;
        }
        ((SN_local *)z)->i_p1 = z->c;
    } while (0);
    return 1;
}

static int r_R1(struct SN_env * z) {
    return ((SN_local *)z)->i_p1 <= z->c;
}

static int r_v_ending(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    if (z->c - 1 <= z->lb || (z->p[z->c - 1] != 161 && z->p[z->c - 1] != 169)) return 0;
    among_var = find_among_b(z, a_0, 2, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    {
        int ret = r_R1(z);
        if (ret <= 0) return ret;
    }
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
    }
    return 1;
}

static int r_double(struct SN_env * z) {
    {
        int v_1 = z->l - z->c;
        if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((106790108 >> (z->p[z->c - 1] & 0x1f)) & 1)) return 0;
        if (!find_among_b(z, a_1, 23, 0)) return 0;
        z->c = z->l - v_1;
    }
    return 1;
}

static int r_undouble(struct SN_env * z) {
    {
        int ret = skip_b_utf8(z->p, z->c, z->lb, 1);
        if (ret < 0) return 0;
        z->c = ret;
    }
    z->ket = z->c;
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

static int r_instrum(struct SN_env * z) {
    z->ket = z->c;
    if (z->c - 1 <= z->lb || z->p[z->c - 1] != 108) return 0;
    if (!find_among_b(z, a_2, 2, 0)) return 0;
    z->bra = z->c;
    {
        int ret = r_R1(z);
        if (ret <= 0) return ret;
    }
    {
        int ret = r_double(z);
        if (ret <= 0) return ret;
    }
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return r_undouble(z);
}

static int r_case(struct SN_env * z) {
    z->ket = z->c;
    if (!find_among_b(z, a_3, 44, 0)) return 0;
    z->bra = z->c;
    {
        int ret = r_R1(z);
        if (ret <= 0) return ret;
    }
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return r_v_ending(z);
}

static int r_case_special(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    if (z->c - 2 <= z->lb || (z->p[z->c - 1] != 110 && z->p[z->c - 1] != 116)) return 0;
    among_var = find_among_b(z, a_4, 3, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    {
        int ret = r_R1(z);
        if (ret <= 0) return ret;
    }
    switch (among_var) {
        case 1:
            {
                int ret = slice_from_s(z, 1, s_2);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {
                int ret = slice_from_s(z, 1, s_3);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_case_other(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    if (z->c - 3 <= z->lb || z->p[z->c - 1] != 108) return 0;
    among_var = find_among_b(z, a_5, 6, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    {
        int ret = r_R1(z);
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
                int ret = slice_from_s(z, 1, s_4);
                if (ret < 0) return ret;
            }
            break;
        case 3:
            {
                int ret = slice_from_s(z, 1, s_5);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_factive(struct SN_env * z) {
    z->ket = z->c;
    if (z->c - 1 <= z->lb || (z->p[z->c - 1] != 161 && z->p[z->c - 1] != 169)) return 0;
    if (!find_among_b(z, a_6, 2, 0)) return 0;
    z->bra = z->c;
    {
        int ret = r_R1(z);
        if (ret <= 0) return ret;
    }
    {
        int ret = r_double(z);
        if (ret <= 0) return ret;
    }
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return r_undouble(z);
}

static int r_plural(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    if (z->c <= z->lb || z->p[z->c - 1] != 107) return 0;
    among_var = find_among_b(z, a_7, 7, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    {
        int ret = r_R1(z);
        if (ret <= 0) return ret;
    }
    switch (among_var) {
        case 1:
            {
                int ret = slice_from_s(z, 1, s_6);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {
                int ret = slice_from_s(z, 1, s_7);
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

static int r_owned(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    if (z->c - 1 <= z->lb || (z->p[z->c - 1] != 105 && z->p[z->c - 1] != 169)) return 0;
    among_var = find_among_b(z, a_8, 12, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    {
        int ret = r_R1(z);
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

static int r_sing_owner(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    among_var = find_among_b(z, a_9, 31, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    {
        int ret = r_R1(z);
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
                int ret = slice_from_s(z, 1, s_10);
                if (ret < 0) return ret;
            }
            break;
        case 3:
            {
                int ret = slice_from_s(z, 1, s_11);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_plur_owner(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    if (z->c <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((10768 >> (z->p[z->c - 1] & 0x1f)) & 1)) return 0;
    among_var = find_among_b(z, a_10, 42, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    {
        int ret = r_R1(z);
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
                int ret = slice_from_s(z, 1, s_12);
                if (ret < 0) return ret;
            }
            break;
        case 3:
            {
                int ret = slice_from_s(z, 1, s_13);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

extern int hungarian_UTF_8_stem(struct SN_env * z) {
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
            int ret = r_instrum(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_2;
    }
    {
        int v_3 = z->l - z->c;
        {
            int ret = r_case(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_3;
    }
    {
        int v_4 = z->l - z->c;
        {
            int ret = r_case_special(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_4;
    }
    {
        int v_5 = z->l - z->c;
        {
            int ret = r_case_other(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_5;
    }
    {
        int v_6 = z->l - z->c;
        {
            int ret = r_factive(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_6;
    }
    {
        int v_7 = z->l - z->c;
        {
            int ret = r_owned(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_7;
    }
    {
        int v_8 = z->l - z->c;
        {
            int ret = r_sing_owner(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_8;
    }
    {
        int v_9 = z->l - z->c;
        {
            int ret = r_plur_owner(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_9;
    }
    {
        int v_10 = z->l - z->c;
        {
            int ret = r_plural(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_10;
    }
    z->c = z->lb;
    return 1;
}

extern struct SN_env * hungarian_UTF_8_create_env(void) {
    struct SN_env * z = SN_new_env(sizeof(SN_local));
    if (z) {
        ((SN_local *)z)->i_p1 = 0;
    }
    return z;
}

extern void hungarian_UTF_8_close_env(struct SN_env * z) {
    SN_delete_env(z);
}

