/* Generated from russian.sbl by Snowball 3.0.0 - https://snowballstem.org/ */

#include "stem_UTF_8_russian.h"

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
extern int russian_UTF_8_stem(struct SN_env * z);
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

static const symbol s_0[] = { 0xD0, 0xB0 };
static const symbol s_1[] = { 0xD1, 0x8F };
static const symbol s_2[] = { 0xD0, 0xB0 };
static const symbol s_3[] = { 0xD1, 0x8F };
static const symbol s_4[] = { 0xD0, 0xB0 };
static const symbol s_5[] = { 0xD1, 0x8F };
static const symbol s_6[] = { 0xD0, 0xBD };
static const symbol s_7[] = { 0xD0, 0xBD };
static const symbol s_8[] = { 0xD0, 0xBD };
static const symbol s_9[] = { 0xD1, 0x91 };
static const symbol s_10[] = { 0xD0, 0xB5 };
static const symbol s_11[] = { 0xD0, 0xB8 };

static const symbol s_0_0[10] = { 0xD0, 0xB2, 0xD1, 0x88, 0xD0, 0xB8, 0xD1, 0x81, 0xD1, 0x8C };
static const symbol s_0_1[12] = { 0xD1, 0x8B, 0xD0, 0xB2, 0xD1, 0x88, 0xD0, 0xB8, 0xD1, 0x81, 0xD1, 0x8C };
static const symbol s_0_2[12] = { 0xD0, 0xB8, 0xD0, 0xB2, 0xD1, 0x88, 0xD0, 0xB8, 0xD1, 0x81, 0xD1, 0x8C };
static const symbol s_0_3[2] = { 0xD0, 0xB2 };
static const symbol s_0_4[4] = { 0xD1, 0x8B, 0xD0, 0xB2 };
static const symbol s_0_5[4] = { 0xD0, 0xB8, 0xD0, 0xB2 };
static const symbol s_0_6[6] = { 0xD0, 0xB2, 0xD1, 0x88, 0xD0, 0xB8 };
static const symbol s_0_7[8] = { 0xD1, 0x8B, 0xD0, 0xB2, 0xD1, 0x88, 0xD0, 0xB8 };
static const symbol s_0_8[8] = { 0xD0, 0xB8, 0xD0, 0xB2, 0xD1, 0x88, 0xD0, 0xB8 };
static const struct among a_0[9] = {
{ 10, s_0_0, 0, 1, 0},
{ 12, s_0_1, -1, 2, 0},
{ 12, s_0_2, -2, 2, 0},
{ 2, s_0_3, 0, 1, 0},
{ 4, s_0_4, -1, 2, 0},
{ 4, s_0_5, -2, 2, 0},
{ 6, s_0_6, 0, 1, 0},
{ 8, s_0_7, -1, 2, 0},
{ 8, s_0_8, -2, 2, 0}
};

static const symbol s_1_0[6] = { 0xD0, 0xB5, 0xD0, 0xBC, 0xD1, 0x83 };
static const symbol s_1_1[6] = { 0xD0, 0xBE, 0xD0, 0xBC, 0xD1, 0x83 };
static const symbol s_1_2[4] = { 0xD1, 0x8B, 0xD1, 0x85 };
static const symbol s_1_3[4] = { 0xD0, 0xB8, 0xD1, 0x85 };
static const symbol s_1_4[4] = { 0xD1, 0x83, 0xD1, 0x8E };
static const symbol s_1_5[4] = { 0xD1, 0x8E, 0xD1, 0x8E };
static const symbol s_1_6[4] = { 0xD0, 0xB5, 0xD1, 0x8E };
static const symbol s_1_7[4] = { 0xD0, 0xBE, 0xD1, 0x8E };
static const symbol s_1_8[4] = { 0xD1, 0x8F, 0xD1, 0x8F };
static const symbol s_1_9[4] = { 0xD0, 0xB0, 0xD1, 0x8F };
static const symbol s_1_10[4] = { 0xD1, 0x8B, 0xD0, 0xB5 };
static const symbol s_1_11[4] = { 0xD0, 0xB5, 0xD0, 0xB5 };
static const symbol s_1_12[4] = { 0xD0, 0xB8, 0xD0, 0xB5 };
static const symbol s_1_13[4] = { 0xD0, 0xBE, 0xD0, 0xB5 };
static const symbol s_1_14[6] = { 0xD1, 0x8B, 0xD0, 0xBC, 0xD0, 0xB8 };
static const symbol s_1_15[6] = { 0xD0, 0xB8, 0xD0, 0xBC, 0xD0, 0xB8 };
static const symbol s_1_16[4] = { 0xD1, 0x8B, 0xD0, 0xB9 };
static const symbol s_1_17[4] = { 0xD0, 0xB5, 0xD0, 0xB9 };
static const symbol s_1_18[4] = { 0xD0, 0xB8, 0xD0, 0xB9 };
static const symbol s_1_19[4] = { 0xD0, 0xBE, 0xD0, 0xB9 };
static const symbol s_1_20[4] = { 0xD1, 0x8B, 0xD0, 0xBC };
static const symbol s_1_21[4] = { 0xD0, 0xB5, 0xD0, 0xBC };
static const symbol s_1_22[4] = { 0xD0, 0xB8, 0xD0, 0xBC };
static const symbol s_1_23[4] = { 0xD0, 0xBE, 0xD0, 0xBC };
static const symbol s_1_24[6] = { 0xD0, 0xB5, 0xD0, 0xB3, 0xD0, 0xBE };
static const symbol s_1_25[6] = { 0xD0, 0xBE, 0xD0, 0xB3, 0xD0, 0xBE };
static const struct among a_1[26] = {
{ 6, s_1_0, 0, 1, 0},
{ 6, s_1_1, 0, 1, 0},
{ 4, s_1_2, 0, 1, 0},
{ 4, s_1_3, 0, 1, 0},
{ 4, s_1_4, 0, 1, 0},
{ 4, s_1_5, 0, 1, 0},
{ 4, s_1_6, 0, 1, 0},
{ 4, s_1_7, 0, 1, 0},
{ 4, s_1_8, 0, 1, 0},
{ 4, s_1_9, 0, 1, 0},
{ 4, s_1_10, 0, 1, 0},
{ 4, s_1_11, 0, 1, 0},
{ 4, s_1_12, 0, 1, 0},
{ 4, s_1_13, 0, 1, 0},
{ 6, s_1_14, 0, 1, 0},
{ 6, s_1_15, 0, 1, 0},
{ 4, s_1_16, 0, 1, 0},
{ 4, s_1_17, 0, 1, 0},
{ 4, s_1_18, 0, 1, 0},
{ 4, s_1_19, 0, 1, 0},
{ 4, s_1_20, 0, 1, 0},
{ 4, s_1_21, 0, 1, 0},
{ 4, s_1_22, 0, 1, 0},
{ 4, s_1_23, 0, 1, 0},
{ 6, s_1_24, 0, 1, 0},
{ 6, s_1_25, 0, 1, 0}
};

static const symbol s_2_0[4] = { 0xD0, 0xB2, 0xD1, 0x88 };
static const symbol s_2_1[6] = { 0xD1, 0x8B, 0xD0, 0xB2, 0xD1, 0x88 };
static const symbol s_2_2[6] = { 0xD0, 0xB8, 0xD0, 0xB2, 0xD1, 0x88 };
static const symbol s_2_3[2] = { 0xD1, 0x89 };
static const symbol s_2_4[4] = { 0xD1, 0x8E, 0xD1, 0x89 };
static const symbol s_2_5[6] = { 0xD1, 0x83, 0xD1, 0x8E, 0xD1, 0x89 };
static const symbol s_2_6[4] = { 0xD0, 0xB5, 0xD0, 0xBC };
static const symbol s_2_7[4] = { 0xD0, 0xBD, 0xD0, 0xBD };
static const struct among a_2[8] = {
{ 4, s_2_0, 0, 1, 0},
{ 6, s_2_1, -1, 2, 0},
{ 6, s_2_2, -2, 2, 0},
{ 2, s_2_3, 0, 1, 0},
{ 4, s_2_4, -1, 1, 0},
{ 6, s_2_5, -1, 2, 0},
{ 4, s_2_6, 0, 1, 0},
{ 4, s_2_7, 0, 1, 0}
};

static const symbol s_3_0[4] = { 0xD1, 0x81, 0xD1, 0x8C };
static const symbol s_3_1[4] = { 0xD1, 0x81, 0xD1, 0x8F };
static const struct among a_3[2] = {
{ 4, s_3_0, 0, 1, 0},
{ 4, s_3_1, 0, 1, 0}
};

static const symbol s_4_0[4] = { 0xD1, 0x8B, 0xD1, 0x82 };
static const symbol s_4_1[4] = { 0xD1, 0x8E, 0xD1, 0x82 };
static const symbol s_4_2[6] = { 0xD1, 0x83, 0xD1, 0x8E, 0xD1, 0x82 };
static const symbol s_4_3[4] = { 0xD1, 0x8F, 0xD1, 0x82 };
static const symbol s_4_4[4] = { 0xD0, 0xB5, 0xD1, 0x82 };
static const symbol s_4_5[6] = { 0xD1, 0x83, 0xD0, 0xB5, 0xD1, 0x82 };
static const symbol s_4_6[4] = { 0xD0, 0xB8, 0xD1, 0x82 };
static const symbol s_4_7[4] = { 0xD0, 0xBD, 0xD1, 0x8B };
static const symbol s_4_8[6] = { 0xD0, 0xB5, 0xD0, 0xBD, 0xD1, 0x8B };
static const symbol s_4_9[4] = { 0xD1, 0x82, 0xD1, 0x8C };
static const symbol s_4_10[6] = { 0xD1, 0x8B, 0xD1, 0x82, 0xD1, 0x8C };
static const symbol s_4_11[6] = { 0xD0, 0xB8, 0xD1, 0x82, 0xD1, 0x8C };
static const symbol s_4_12[6] = { 0xD0, 0xB5, 0xD1, 0x88, 0xD1, 0x8C };
static const symbol s_4_13[6] = { 0xD0, 0xB8, 0xD1, 0x88, 0xD1, 0x8C };
static const symbol s_4_14[2] = { 0xD1, 0x8E };
static const symbol s_4_15[4] = { 0xD1, 0x83, 0xD1, 0x8E };
static const symbol s_4_16[4] = { 0xD0, 0xBB, 0xD0, 0xB0 };
static const symbol s_4_17[6] = { 0xD1, 0x8B, 0xD0, 0xBB, 0xD0, 0xB0 };
static const symbol s_4_18[6] = { 0xD0, 0xB8, 0xD0, 0xBB, 0xD0, 0xB0 };
static const symbol s_4_19[4] = { 0xD0, 0xBD, 0xD0, 0xB0 };
static const symbol s_4_20[6] = { 0xD0, 0xB5, 0xD0, 0xBD, 0xD0, 0xB0 };
static const symbol s_4_21[6] = { 0xD0, 0xB5, 0xD1, 0x82, 0xD0, 0xB5 };
static const symbol s_4_22[6] = { 0xD0, 0xB8, 0xD1, 0x82, 0xD0, 0xB5 };
static const symbol s_4_23[6] = { 0xD0, 0xB9, 0xD1, 0x82, 0xD0, 0xB5 };
static const symbol s_4_24[8] = { 0xD1, 0x83, 0xD0, 0xB9, 0xD1, 0x82, 0xD0, 0xB5 };
static const symbol s_4_25[8] = { 0xD0, 0xB5, 0xD0, 0xB9, 0xD1, 0x82, 0xD0, 0xB5 };
static const symbol s_4_26[4] = { 0xD0, 0xBB, 0xD0, 0xB8 };
static const symbol s_4_27[6] = { 0xD1, 0x8B, 0xD0, 0xBB, 0xD0, 0xB8 };
static const symbol s_4_28[6] = { 0xD0, 0xB8, 0xD0, 0xBB, 0xD0, 0xB8 };
static const symbol s_4_29[2] = { 0xD0, 0xB9 };
static const symbol s_4_30[4] = { 0xD1, 0x83, 0xD0, 0xB9 };
static const symbol s_4_31[4] = { 0xD0, 0xB5, 0xD0, 0xB9 };
static const symbol s_4_32[2] = { 0xD0, 0xBB };
static const symbol s_4_33[4] = { 0xD1, 0x8B, 0xD0, 0xBB };
static const symbol s_4_34[4] = { 0xD0, 0xB8, 0xD0, 0xBB };
static const symbol s_4_35[4] = { 0xD1, 0x8B, 0xD0, 0xBC };
static const symbol s_4_36[4] = { 0xD0, 0xB5, 0xD0, 0xBC };
static const symbol s_4_37[4] = { 0xD0, 0xB8, 0xD0, 0xBC };
static const symbol s_4_38[2] = { 0xD0, 0xBD };
static const symbol s_4_39[4] = { 0xD0, 0xB5, 0xD0, 0xBD };
static const symbol s_4_40[4] = { 0xD0, 0xBB, 0xD0, 0xBE };
static const symbol s_4_41[6] = { 0xD1, 0x8B, 0xD0, 0xBB, 0xD0, 0xBE };
static const symbol s_4_42[6] = { 0xD0, 0xB8, 0xD0, 0xBB, 0xD0, 0xBE };
static const symbol s_4_43[4] = { 0xD0, 0xBD, 0xD0, 0xBE };
static const symbol s_4_44[6] = { 0xD0, 0xB5, 0xD0, 0xBD, 0xD0, 0xBE };
static const symbol s_4_45[6] = { 0xD0, 0xBD, 0xD0, 0xBD, 0xD0, 0xBE };
static const struct among a_4[46] = {
{ 4, s_4_0, 0, 2, 0},
{ 4, s_4_1, 0, 1, 0},
{ 6, s_4_2, -1, 2, 0},
{ 4, s_4_3, 0, 2, 0},
{ 4, s_4_4, 0, 1, 0},
{ 6, s_4_5, -1, 2, 0},
{ 4, s_4_6, 0, 2, 0},
{ 4, s_4_7, 0, 1, 0},
{ 6, s_4_8, -1, 2, 0},
{ 4, s_4_9, 0, 1, 0},
{ 6, s_4_10, -1, 2, 0},
{ 6, s_4_11, -2, 2, 0},
{ 6, s_4_12, 0, 1, 0},
{ 6, s_4_13, 0, 2, 0},
{ 2, s_4_14, 0, 2, 0},
{ 4, s_4_15, -1, 2, 0},
{ 4, s_4_16, 0, 1, 0},
{ 6, s_4_17, -1, 2, 0},
{ 6, s_4_18, -2, 2, 0},
{ 4, s_4_19, 0, 1, 0},
{ 6, s_4_20, -1, 2, 0},
{ 6, s_4_21, 0, 1, 0},
{ 6, s_4_22, 0, 2, 0},
{ 6, s_4_23, 0, 1, 0},
{ 8, s_4_24, -1, 2, 0},
{ 8, s_4_25, -2, 2, 0},
{ 4, s_4_26, 0, 1, 0},
{ 6, s_4_27, -1, 2, 0},
{ 6, s_4_28, -2, 2, 0},
{ 2, s_4_29, 0, 1, 0},
{ 4, s_4_30, -1, 2, 0},
{ 4, s_4_31, -2, 2, 0},
{ 2, s_4_32, 0, 1, 0},
{ 4, s_4_33, -1, 2, 0},
{ 4, s_4_34, -2, 2, 0},
{ 4, s_4_35, 0, 2, 0},
{ 4, s_4_36, 0, 1, 0},
{ 4, s_4_37, 0, 2, 0},
{ 2, s_4_38, 0, 1, 0},
{ 4, s_4_39, -1, 2, 0},
{ 4, s_4_40, 0, 1, 0},
{ 6, s_4_41, -1, 2, 0},
{ 6, s_4_42, -2, 2, 0},
{ 4, s_4_43, 0, 1, 0},
{ 6, s_4_44, -1, 2, 0},
{ 6, s_4_45, -2, 1, 0}
};

static const symbol s_5_0[2] = { 0xD1, 0x83 };
static const symbol s_5_1[4] = { 0xD1, 0x8F, 0xD1, 0x85 };
static const symbol s_5_2[6] = { 0xD0, 0xB8, 0xD1, 0x8F, 0xD1, 0x85 };
static const symbol s_5_3[4] = { 0xD0, 0xB0, 0xD1, 0x85 };
static const symbol s_5_4[2] = { 0xD1, 0x8B };
static const symbol s_5_5[2] = { 0xD1, 0x8C };
static const symbol s_5_6[2] = { 0xD1, 0x8E };
static const symbol s_5_7[4] = { 0xD1, 0x8C, 0xD1, 0x8E };
static const symbol s_5_8[4] = { 0xD0, 0xB8, 0xD1, 0x8E };
static const symbol s_5_9[2] = { 0xD1, 0x8F };
static const symbol s_5_10[4] = { 0xD1, 0x8C, 0xD1, 0x8F };
static const symbol s_5_11[4] = { 0xD0, 0xB8, 0xD1, 0x8F };
static const symbol s_5_12[2] = { 0xD0, 0xB0 };
static const symbol s_5_13[4] = { 0xD0, 0xB5, 0xD0, 0xB2 };
static const symbol s_5_14[4] = { 0xD0, 0xBE, 0xD0, 0xB2 };
static const symbol s_5_15[2] = { 0xD0, 0xB5 };
static const symbol s_5_16[4] = { 0xD1, 0x8C, 0xD0, 0xB5 };
static const symbol s_5_17[4] = { 0xD0, 0xB8, 0xD0, 0xB5 };
static const symbol s_5_18[2] = { 0xD0, 0xB8 };
static const symbol s_5_19[4] = { 0xD0, 0xB5, 0xD0, 0xB8 };
static const symbol s_5_20[4] = { 0xD0, 0xB8, 0xD0, 0xB8 };
static const symbol s_5_21[6] = { 0xD1, 0x8F, 0xD0, 0xBC, 0xD0, 0xB8 };
static const symbol s_5_22[8] = { 0xD0, 0xB8, 0xD1, 0x8F, 0xD0, 0xBC, 0xD0, 0xB8 };
static const symbol s_5_23[6] = { 0xD0, 0xB0, 0xD0, 0xBC, 0xD0, 0xB8 };
static const symbol s_5_24[2] = { 0xD0, 0xB9 };
static const symbol s_5_25[4] = { 0xD0, 0xB5, 0xD0, 0xB9 };
static const symbol s_5_26[6] = { 0xD0, 0xB8, 0xD0, 0xB5, 0xD0, 0xB9 };
static const symbol s_5_27[4] = { 0xD0, 0xB8, 0xD0, 0xB9 };
static const symbol s_5_28[4] = { 0xD0, 0xBE, 0xD0, 0xB9 };
static const symbol s_5_29[4] = { 0xD1, 0x8F, 0xD0, 0xBC };
static const symbol s_5_30[6] = { 0xD0, 0xB8, 0xD1, 0x8F, 0xD0, 0xBC };
static const symbol s_5_31[4] = { 0xD0, 0xB0, 0xD0, 0xBC };
static const symbol s_5_32[4] = { 0xD0, 0xB5, 0xD0, 0xBC };
static const symbol s_5_33[6] = { 0xD0, 0xB8, 0xD0, 0xB5, 0xD0, 0xBC };
static const symbol s_5_34[4] = { 0xD0, 0xBE, 0xD0, 0xBC };
static const symbol s_5_35[2] = { 0xD0, 0xBE };
static const struct among a_5[36] = {
{ 2, s_5_0, 0, 1, 0},
{ 4, s_5_1, 0, 1, 0},
{ 6, s_5_2, -1, 1, 0},
{ 4, s_5_3, 0, 1, 0},
{ 2, s_5_4, 0, 1, 0},
{ 2, s_5_5, 0, 1, 0},
{ 2, s_5_6, 0, 1, 0},
{ 4, s_5_7, -1, 1, 0},
{ 4, s_5_8, -2, 1, 0},
{ 2, s_5_9, 0, 1, 0},
{ 4, s_5_10, -1, 1, 0},
{ 4, s_5_11, -2, 1, 0},
{ 2, s_5_12, 0, 1, 0},
{ 4, s_5_13, 0, 1, 0},
{ 4, s_5_14, 0, 1, 0},
{ 2, s_5_15, 0, 1, 0},
{ 4, s_5_16, -1, 1, 0},
{ 4, s_5_17, -2, 1, 0},
{ 2, s_5_18, 0, 1, 0},
{ 4, s_5_19, -1, 1, 0},
{ 4, s_5_20, -2, 1, 0},
{ 6, s_5_21, -3, 1, 0},
{ 8, s_5_22, -1, 1, 0},
{ 6, s_5_23, -5, 1, 0},
{ 2, s_5_24, 0, 1, 0},
{ 4, s_5_25, -1, 1, 0},
{ 6, s_5_26, -1, 1, 0},
{ 4, s_5_27, -3, 1, 0},
{ 4, s_5_28, -4, 1, 0},
{ 4, s_5_29, 0, 1, 0},
{ 6, s_5_30, -1, 1, 0},
{ 4, s_5_31, 0, 1, 0},
{ 4, s_5_32, 0, 1, 0},
{ 6, s_5_33, -1, 1, 0},
{ 4, s_5_34, 0, 1, 0},
{ 2, s_5_35, 0, 1, 0}
};

static const symbol s_6_0[6] = { 0xD0, 0xBE, 0xD1, 0x81, 0xD1, 0x82 };
static const symbol s_6_1[8] = { 0xD0, 0xBE, 0xD1, 0x81, 0xD1, 0x82, 0xD1, 0x8C };
static const struct among a_6[2] = {
{ 6, s_6_0, 0, 1, 0},
{ 8, s_6_1, 0, 1, 0}
};

static const symbol s_7_0[6] = { 0xD0, 0xB5, 0xD0, 0xB9, 0xD1, 0x88 };
static const symbol s_7_1[2] = { 0xD1, 0x8C };
static const symbol s_7_2[8] = { 0xD0, 0xB5, 0xD0, 0xB9, 0xD1, 0x88, 0xD0, 0xB5 };
static const symbol s_7_3[2] = { 0xD0, 0xBD };
static const struct among a_7[4] = {
{ 6, s_7_0, 0, 1, 0},
{ 2, s_7_1, 0, 3, 0},
{ 8, s_7_2, 0, 1, 0},
{ 2, s_7_3, 0, 2, 0}
};

static const unsigned char g_v[] = { 33, 65, 8, 232 };

static int r_mark_regions(struct SN_env * z) {
    ((SN_local *)z)->i_pV = z->l;
    ((SN_local *)z)->i_p2 = z->l;
    {
        int v_1 = z->c;
        {
            int ret = out_grouping_U(z, g_v, 1072, 1103, 1);
            if (ret < 0) goto lab0;
            z->c += ret;
        }
        ((SN_local *)z)->i_pV = z->c;
        {
            int ret = in_grouping_U(z, g_v, 1072, 1103, 1);
            if (ret < 0) goto lab0;
            z->c += ret;
        }
        {
            int ret = out_grouping_U(z, g_v, 1072, 1103, 1);
            if (ret < 0) goto lab0;
            z->c += ret;
        }
        {
            int ret = in_grouping_U(z, g_v, 1072, 1103, 1);
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
    among_var = find_among_b(z, a_0, 9, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    switch (among_var) {
        case 1:
            do {
                int v_1 = z->l - z->c;
                if (!(eq_s_b(z, 2, s_0))) goto lab0;
                break;
            lab0:
                z->c = z->l - v_1;
                if (!(eq_s_b(z, 2, s_1))) return 0;
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
        among_var = find_among_b(z, a_2, 8, 0);
        if (!among_var) { z->c = z->l - v_1; goto lab0; }
        z->bra = z->c;
        switch (among_var) {
            case 1:
                do {
                    int v_2 = z->l - z->c;
                    if (!(eq_s_b(z, 2, s_2))) goto lab1;
                    break;
                lab1:
                    z->c = z->l - v_2;
                    if (!(eq_s_b(z, 2, s_3))) { z->c = z->l - v_1; goto lab0; }
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
    if (z->c - 3 <= z->lb || (z->p[z->c - 1] != 140 && z->p[z->c - 1] != 143)) return 0;
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
    among_var = find_among_b(z, a_4, 46, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    switch (among_var) {
        case 1:
            do {
                int v_1 = z->l - z->c;
                if (!(eq_s_b(z, 2, s_4))) goto lab0;
                break;
            lab0:
                z->c = z->l - v_1;
                if (!(eq_s_b(z, 2, s_5))) return 0;
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
    if (z->c - 5 <= z->lb || (z->p[z->c - 1] != 130 && z->p[z->c - 1] != 140)) return 0;
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
            if (!(eq_s_b(z, 2, s_6))) return 0;
            z->bra = z->c;
            if (!(eq_s_b(z, 2, s_7))) return 0;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            if (!(eq_s_b(z, 2, s_8))) return 0;
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

extern int russian_UTF_8_stem(struct SN_env * z) {
    {
        int v_1 = z->c;
        while (1) {
            int v_2 = z->c;
            while (1) {
                int v_3 = z->c;
                z->bra = z->c;
                if (!(eq_s(z, 2, s_9))) goto lab2;
                z->ket = z->c;
                z->c = v_3;
                break;
            lab2:
                z->c = v_3;
                {
                    int ret = skip_utf8(z->p, z->c, z->l, 1);
                    if (ret < 0) goto lab1;
                    z->c = ret;
                }
            }
            {
                int ret = slice_from_s(z, 2, s_10);
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
            if (!(eq_s_b(z, 2, s_11))) { z->c = z->l - v_9; goto lab8; }
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

extern struct SN_env * russian_UTF_8_create_env(void) {
    struct SN_env * z = SN_new_env(sizeof(SN_local));
    if (z) {
        ((SN_local *)z)->i_p2 = 0;
        ((SN_local *)z)->i_pV = 0;
    }
    return z;
}

extern void russian_UTF_8_close_env(struct SN_env * z) {
    SN_delete_env(z);
}

