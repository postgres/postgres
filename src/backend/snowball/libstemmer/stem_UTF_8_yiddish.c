/* Generated from yiddish.sbl by Snowball 3.0.0 - https://snowballstem.org/ */

#include "stem_UTF_8_yiddish.h"

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
extern int yiddish_UTF_8_stem(struct SN_env * z);
#ifdef __cplusplus
}
#endif

static int r_standard_suffix(struct SN_env * z);
static int r_R1plus3(struct SN_env * z);
static int r_R1(struct SN_env * z);
static int r_mark_regions(struct SN_env * z);
static int r_prelude(struct SN_env * z);

static const symbol s_0[] = { 0xD6, 0xBC };
static const symbol s_1[] = { 0xD7, 0xB0 };
static const symbol s_2[] = { 0xD6, 0xB4 };
static const symbol s_3[] = { 0xD7, 0xB1 };
static const symbol s_4[] = { 0xD6, 0xB4 };
static const symbol s_5[] = { 0xD7, 0xB2 };
static const symbol s_6[] = { 0xD7, 0x9B };
static const symbol s_7[] = { 0xD7, 0x9E };
static const symbol s_8[] = { 0xD7, 0xA0 };
static const symbol s_9[] = { 0xD7, 0xA4 };
static const symbol s_10[] = { 0xD7, 0xA6 };
static const symbol s_11[] = { 0xD7, 0x92, 0xD7, 0xA2 };
static const symbol s_12[] = { 0xD7, 0x9C, 0xD7, 0x98 };
static const symbol s_13[] = { 0xD7, 0x91, 0xD7, 0xA0 };
static const symbol s_14[] = { 'G', 'E' };
static const symbol s_15[] = { 0xD7, 0xA6, 0xD7, 0x95, 0xD7, 0x92, 0xD7, 0xA0 };
static const symbol s_16[] = { 0xD7, 0xA6, 0xD7, 0x95, 0xD7, 0xA7, 0xD7, 0x98 };
static const symbol s_17[] = { 0xD7, 0xA6, 0xD7, 0x95, 0xD7, 0xA7, 0xD7, 0xA0 };
static const symbol s_18[] = { 0xD7, 0x92, 0xD7, 0xA2, 0xD7, 0x91, 0xD7, 0xA0 };
static const symbol s_19[] = { 0xD7, 0x92, 0xD7, 0xA2 };
static const symbol s_20[] = { 'G', 'E' };
static const symbol s_21[] = { 0xD7, 0xA6, 0xD7, 0x95 };
static const symbol s_22[] = { 'T', 'S', 'U' };
static const symbol s_23[] = { 0xD7, 0x99, 0xD7, 0xA2 };
static const symbol s_24[] = { 0xD7, 0x92, 0xD7, 0xB2 };
static const symbol s_25[] = { 0xD7, 0xA0, 0xD7, 0xA2, 0xD7, 0x9E };
static const symbol s_26[] = { 0xD7, 0x9E, 0xD7, 0xB2, 0xD7, 0x93 };
static const symbol s_27[] = { 0xD7, 0x91, 0xD7, 0xB2, 0xD7, 0x98 };
static const symbol s_28[] = { 0xD7, 0x91, 0xD7, 0xB2, 0xD7, 0xA1 };
static const symbol s_29[] = { 0xD7, 0xB0, 0xD7, 0xB2, 0xD7, 0x96 };
static const symbol s_30[] = { 0xD7, 0x98, 0xD7, 0xA8, 0xD7, 0xB2, 0xD7, 0x91 };
static const symbol s_31[] = { 0xD7, 0x9C, 0xD7, 0xB2, 0xD7, 0x98 };
static const symbol s_32[] = { 0xD7, 0xA7, 0xD7, 0x9C, 0xD7, 0xB2, 0xD7, 0x91 };
static const symbol s_33[] = { 0xD7, 0xA8, 0xD7, 0xB2, 0xD7, 0x91 };
static const symbol s_34[] = { 0xD7, 0xA8, 0xD7, 0xB2, 0xD7, 0xA1 };
static const symbol s_35[] = { 0xD7, 0xA9, 0xD7, 0xB0, 0xD7, 0xB2, 0xD7, 0x92 };
static const symbol s_36[] = { 0xD7, 0xA9, 0xD7, 0x9E, 0xD7, 0xB2, 0xD7, 0xA1 };
static const symbol s_37[] = { 0xD7, 0xA9, 0xD7, 0xA0, 0xD7, 0xB2, 0xD7, 0x93 };
static const symbol s_38[] = { 0xD7, 0xA9, 0xD7, 0xA8, 0xD7, 0xB2, 0xD7, 0x91 };
static const symbol s_39[] = { 0xD7, 0x91, 0xD7, 0x99, 0xD7, 0xA0, 0xD7, 0x93 };
static const symbol s_40[] = { 0xD7, 0xB0, 0xD7, 0x99, 0xD7, 0x98, 0xD7, 0xA9 };
static const symbol s_41[] = { 0xD7, 0x96, 0xD7, 0x99, 0xD7, 0xA0, 0xD7, 0x92 };
static const symbol s_42[] = { 0xD7, 0x98, 0xD7, 0xA8, 0xD7, 0x99, 0xD7, 0xA0, 0xD7, 0xA7 };
static const symbol s_43[] = { 0xD7, 0xA6, 0xD7, 0xB0, 0xD7, 0x99, 0xD7, 0xA0, 0xD7, 0x92 };
static const symbol s_44[] = { 0xD7, 0xA9, 0xD7, 0x9C, 0xD7, 0x99, 0xD7, 0xA0, 0xD7, 0x92 };
static const symbol s_45[] = { 0xD7, 0x91, 0xD7, 0xB2, 0xD7, 0x92 };
static const symbol s_46[] = { 0xD7, 0x94, 0xD7, 0xB2, 0xD7, 0x91 };
static const symbol s_47[] = { 0xD7, 0xA4, 0xD7, 0x90, 0xD7, 0xA8, 0xD7, 0x9C, 0xD7, 0x99, 0xD7, 0xA8 };
static const symbol s_48[] = { 0xD7, 0xA9, 0xD7, 0x98, 0xD7, 0xB2 };
static const symbol s_49[] = { 0xD7, 0xA9, 0xD7, 0xB0, 0xD7, 0xA2, 0xD7, 0xA8 };
static const symbol s_50[] = { 0xD7, 0x98 };
static const symbol s_51[] = { 0xD7, 0x91, 0xD7, 0xA8, 0xD7, 0x90, 0xD7, 0x9B };
static const symbol s_52[] = { 0xD7, 0x92, 0xD7, 0xA2 };
static const symbol s_53[] = { 0xD7, 0x91, 0xD7, 0xA8, 0xD7, 0xA2, 0xD7, 0xA0, 0xD7, 0x92 };
static const symbol s_54[] = { 0xD7, 0x92, 0xD7, 0xB2 };
static const symbol s_55[] = { 0xD7, 0xA0, 0xD7, 0xA2, 0xD7, 0x9E };
static const symbol s_56[] = { 0xD7, 0xA9, 0xD7, 0xA8, 0xD7, 0xB2, 0xD7, 0x91 };
static const symbol s_57[] = { 0xD7, 0x9E, 0xD7, 0xB2, 0xD7, 0x93 };
static const symbol s_58[] = { 0xD7, 0x91, 0xD7, 0xB2, 0xD7, 0x98 };
static const symbol s_59[] = { 0xD7, 0x91, 0xD7, 0xB2, 0xD7, 0xA1 };
static const symbol s_60[] = { 0xD7, 0xB0, 0xD7, 0xB2, 0xD7, 0x96 };
static const symbol s_61[] = { 0xD7, 0x98, 0xD7, 0xA8, 0xD7, 0xB2, 0xD7, 0x91 };
static const symbol s_62[] = { 0xD7, 0x9C, 0xD7, 0xB2, 0xD7, 0x98 };
static const symbol s_63[] = { 0xD7, 0xA7, 0xD7, 0x9C, 0xD7, 0xB2, 0xD7, 0x91 };
static const symbol s_64[] = { 0xD7, 0xA8, 0xD7, 0xB2, 0xD7, 0x91 };
static const symbol s_65[] = { 0xD7, 0xA8, 0xD7, 0xB2, 0xD7, 0xA1 };
static const symbol s_66[] = { 0xD7, 0xA9, 0xD7, 0xB0, 0xD7, 0xB2, 0xD7, 0x92 };
static const symbol s_67[] = { 0xD7, 0xA9, 0xD7, 0x9E, 0xD7, 0xB2, 0xD7, 0xA1 };
static const symbol s_68[] = { 0xD7, 0xA9, 0xD7, 0xA0, 0xD7, 0xB2, 0xD7, 0x93 };
static const symbol s_69[] = { 0xD7, 0x91, 0xD7, 0x99, 0xD7, 0xA0, 0xD7, 0x93 };
static const symbol s_70[] = { 0xD7, 0xB0, 0xD7, 0x99, 0xD7, 0x98, 0xD7, 0xA9 };
static const symbol s_71[] = { 0xD7, 0x96, 0xD7, 0x99, 0xD7, 0xA0, 0xD7, 0x92 };
static const symbol s_72[] = { 0xD7, 0x98, 0xD7, 0xA8, 0xD7, 0x99, 0xD7, 0xA0, 0xD7, 0xA7 };
static const symbol s_73[] = { 0xD7, 0xA6, 0xD7, 0xB0, 0xD7, 0x99, 0xD7, 0xA0, 0xD7, 0x92 };
static const symbol s_74[] = { 0xD7, 0xA9, 0xD7, 0x9C, 0xD7, 0x99, 0xD7, 0xA0, 0xD7, 0x92 };
static const symbol s_75[] = { 0xD7, 0x91, 0xD7, 0xB2, 0xD7, 0x92 };
static const symbol s_76[] = { 0xD7, 0x94, 0xD7, 0xB2, 0xD7, 0x91 };
static const symbol s_77[] = { 0xD7, 0xA4, 0xD7, 0x90, 0xD7, 0xA8, 0xD7, 0x9C, 0xD7, 0x99, 0xD7, 0xA8 };
static const symbol s_78[] = { 0xD7, 0xA9, 0xD7, 0x98, 0xD7, 0xB2 };
static const symbol s_79[] = { 0xD7, 0xA9, 0xD7, 0xB0, 0xD7, 0xA2, 0xD7, 0xA8 };
static const symbol s_80[] = { 0xD7, 0x91, 0xD7, 0xA8, 0xD7, 0xA2, 0xD7, 0xA0, 0xD7, 0x92 };
static const symbol s_81[] = { 0xD7, 0x94 };
static const symbol s_82[] = { 0xD7, 0x92 };
static const symbol s_83[] = { 0xD7, 0xA9 };
static const symbol s_84[] = { 0xD7, 0x99, 0xD7, 0xA1 };
static const symbol s_85[] = { 'G', 'E' };
static const symbol s_86[] = { 'T', 'S', 'U' };

static const symbol s_0_0[4] = { 0xD7, 0x95, 0xD7, 0x95 };
static const symbol s_0_1[4] = { 0xD7, 0x95, 0xD7, 0x99 };
static const symbol s_0_2[4] = { 0xD7, 0x99, 0xD7, 0x99 };
static const symbol s_0_3[2] = { 0xD7, 0x9A };
static const symbol s_0_4[2] = { 0xD7, 0x9D };
static const symbol s_0_5[2] = { 0xD7, 0x9F };
static const symbol s_0_6[2] = { 0xD7, 0xA3 };
static const symbol s_0_7[2] = { 0xD7, 0xA5 };
static const struct among a_0[8] = {
{ 4, s_0_0, 0, 1, 0},
{ 4, s_0_1, 0, 2, 0},
{ 4, s_0_2, 0, 3, 0},
{ 2, s_0_3, 0, 4, 0},
{ 2, s_0_4, 0, 5, 0},
{ 2, s_0_5, 0, 6, 0},
{ 2, s_0_6, 0, 7, 0},
{ 2, s_0_7, 0, 8, 0}
};

static const symbol s_1_0[10] = { 0xD7, 0x90, 0xD7, 0x93, 0xD7, 0x95, 0xD7, 0xA8, 0xD7, 0x9B };
static const symbol s_1_1[8] = { 0xD7, 0x90, 0xD7, 0x94, 0xD7, 0x99, 0xD7, 0xA0 };
static const symbol s_1_2[8] = { 0xD7, 0x90, 0xD7, 0x94, 0xD7, 0xA2, 0xD7, 0xA8 };
static const symbol s_1_3[8] = { 0xD7, 0x90, 0xD7, 0x94, 0xD7, 0xB2, 0xD7, 0x9E };
static const symbol s_1_4[6] = { 0xD7, 0x90, 0xD7, 0x95, 0xD7, 0x9E };
static const symbol s_1_5[12] = { 0xD7, 0x90, 0xD7, 0x95, 0xD7, 0xA0, 0xD7, 0x98, 0xD7, 0xA2, 0xD7, 0xA8 };
static const symbol s_1_6[10] = { 0xD7, 0x90, 0xD7, 0x99, 0xD7, 0x91, 0xD7, 0xA2, 0xD7, 0xA8 };
static const symbol s_1_7[4] = { 0xD7, 0x90, 0xD7, 0xA0 };
static const symbol s_1_8[6] = { 0xD7, 0x90, 0xD7, 0xA0, 0xD7, 0x98 };
static const symbol s_1_9[14] = { 0xD7, 0x90, 0xD7, 0xA0, 0xD7, 0x98, 0xD7, 0xA7, 0xD7, 0xA2, 0xD7, 0x92, 0xD7, 0xA0 };
static const symbol s_1_10[12] = { 0xD7, 0x90, 0xD7, 0xA0, 0xD7, 0x99, 0xD7, 0x93, 0xD7, 0xA2, 0xD7, 0xA8 };
static const symbol s_1_11[4] = { 0xD7, 0x90, 0xD7, 0xA4 };
static const symbol s_1_12[8] = { 0xD7, 0x90, 0xD7, 0xA4, 0xD7, 0x99, 0xD7, 0xA8 };
static const symbol s_1_13[10] = { 0xD7, 0x90, 0xD7, 0xA7, 0xD7, 0xA2, 0xD7, 0x92, 0xD7, 0xA0 };
static const symbol s_1_14[8] = { 0xD7, 0x90, 0xD7, 0xA8, 0xD7, 0x90, 0xD7, 0xA4 };
static const symbol s_1_15[8] = { 0xD7, 0x90, 0xD7, 0xA8, 0xD7, 0x95, 0xD7, 0x9E };
static const symbol s_1_16[14] = { 0xD7, 0x90, 0xD7, 0xA8, 0xD7, 0x95, 0xD7, 0xA0, 0xD7, 0x98, 0xD7, 0xA2, 0xD7, 0xA8 };
static const symbol s_1_17[12] = { 0xD7, 0x90, 0xD7, 0xA8, 0xD7, 0x99, 0xD7, 0x91, 0xD7, 0xA2, 0xD7, 0xA8 };
static const symbol s_1_18[8] = { 0xD7, 0x90, 0xD7, 0xA8, 0xD7, 0xB1, 0xD7, 0xA1 };
static const symbol s_1_19[8] = { 0xD7, 0x90, 0xD7, 0xA8, 0xD7, 0xB1, 0xD7, 0xA4 };
static const symbol s_1_20[8] = { 0xD7, 0x90, 0xD7, 0xA8, 0xD7, 0xB2, 0xD7, 0xA0 };
static const symbol s_1_21[8] = { 0xD7, 0x90, 0xD7, 0xB0, 0xD7, 0xA2, 0xD7, 0xA7 };
static const symbol s_1_22[6] = { 0xD7, 0x90, 0xD7, 0xB1, 0xD7, 0xA1 };
static const symbol s_1_23[6] = { 0xD7, 0x90, 0xD7, 0xB1, 0xD7, 0xA4 };
static const symbol s_1_24[6] = { 0xD7, 0x90, 0xD7, 0xB2, 0xD7, 0xA0 };
static const symbol s_1_25[4] = { 0xD7, 0x91, 0xD7, 0x90 };
static const symbol s_1_26[4] = { 0xD7, 0x91, 0xD7, 0xB2 };
static const symbol s_1_27[8] = { 0xD7, 0x93, 0xD7, 0x95, 0xD7, 0xA8, 0xD7, 0x9B };
static const symbol s_1_28[6] = { 0xD7, 0x93, 0xD7, 0xA2, 0xD7, 0xA8 };
static const symbol s_1_29[6] = { 0xD7, 0x9E, 0xD7, 0x99, 0xD7, 0x98 };
static const symbol s_1_30[6] = { 0xD7, 0xA0, 0xD7, 0x90, 0xD7, 0x9B };
static const symbol s_1_31[6] = { 0xD7, 0xA4, 0xD7, 0x90, 0xD7, 0xA8 };
static const symbol s_1_32[10] = { 0xD7, 0xA4, 0xD7, 0x90, 0xD7, 0xA8, 0xD7, 0x91, 0xD7, 0xB2 };
static const symbol s_1_33[10] = { 0xD7, 0xA4, 0xD7, 0x90, 0xD7, 0xA8, 0xD7, 0xB1, 0xD7, 0xA1 };
static const symbol s_1_34[16] = { 0xD7, 0xA4, 0xD7, 0x95, 0xD7, 0xA0, 0xD7, 0x90, 0xD7, 0xA0, 0xD7, 0x93, 0xD7, 0xA2, 0xD7, 0xA8 };
static const symbol s_1_35[4] = { 0xD7, 0xA6, 0xD7, 0x95 };
static const symbol s_1_36[14] = { 0xD7, 0xA6, 0xD7, 0x95, 0xD7, 0x96, 0xD7, 0x90, 0xD7, 0x9E, 0xD7, 0xA2, 0xD7, 0xA0 };
static const symbol s_1_37[10] = { 0xD7, 0xA6, 0xD7, 0x95, 0xD7, 0xA0, 0xD7, 0xB1, 0xD7, 0xA4 };
static const symbol s_1_38[10] = { 0xD7, 0xA6, 0xD7, 0x95, 0xD7, 0xA8, 0xD7, 0x99, 0xD7, 0xA7 };
static const symbol s_1_39[4] = { 0xD7, 0xA6, 0xD7, 0xA2 };
static const struct among a_1[40] = {
{ 10, s_1_0, 0, 1, 0},
{ 8, s_1_1, 0, 1, 0},
{ 8, s_1_2, 0, 1, 0},
{ 8, s_1_3, 0, 1, 0},
{ 6, s_1_4, 0, 1, 0},
{ 12, s_1_5, 0, 1, 0},
{ 10, s_1_6, 0, 1, 0},
{ 4, s_1_7, 0, 1, 0},
{ 6, s_1_8, -1, 1, 0},
{ 14, s_1_9, -1, 1, 0},
{ 12, s_1_10, -3, 1, 0},
{ 4, s_1_11, 0, 1, 0},
{ 8, s_1_12, -1, 1, 0},
{ 10, s_1_13, 0, 1, 0},
{ 8, s_1_14, 0, 1, 0},
{ 8, s_1_15, 0, 1, 0},
{ 14, s_1_16, 0, 1, 0},
{ 12, s_1_17, 0, 1, 0},
{ 8, s_1_18, 0, 1, 0},
{ 8, s_1_19, 0, 1, 0},
{ 8, s_1_20, 0, 1, 0},
{ 8, s_1_21, 0, 1, 0},
{ 6, s_1_22, 0, 1, 0},
{ 6, s_1_23, 0, 1, 0},
{ 6, s_1_24, 0, 1, 0},
{ 4, s_1_25, 0, 1, 0},
{ 4, s_1_26, 0, 1, 0},
{ 8, s_1_27, 0, 1, 0},
{ 6, s_1_28, 0, 1, 0},
{ 6, s_1_29, 0, 1, 0},
{ 6, s_1_30, 0, 1, 0},
{ 6, s_1_31, 0, 1, 0},
{ 10, s_1_32, -1, 1, 0},
{ 10, s_1_33, -2, 1, 0},
{ 16, s_1_34, 0, 1, 0},
{ 4, s_1_35, 0, 1, 0},
{ 14, s_1_36, -1, 1, 0},
{ 10, s_1_37, -2, 1, 0},
{ 10, s_1_38, -3, 1, 0},
{ 4, s_1_39, 0, 1, 0}
};

static const symbol s_2_0[6] = { 0xD7, 0x93, 0xD7, 0x96, 0xD7, 0xA9 };
static const symbol s_2_1[6] = { 0xD7, 0xA9, 0xD7, 0x98, 0xD7, 0xA8 };
static const symbol s_2_2[6] = { 0xD7, 0xA9, 0xD7, 0x98, 0xD7, 0xA9 };
static const symbol s_2_3[6] = { 0xD7, 0xA9, 0xD7, 0xA4, 0xD7, 0xA8 };
static const struct among a_2[4] = {
{ 6, s_2_0, 0, -1, 0},
{ 6, s_2_1, 0, -1, 0},
{ 6, s_2_2, 0, -1, 0},
{ 6, s_2_3, 0, -1, 0}
};

static const symbol s_3_0[8] = { 0xD7, 0xA7, 0xD7, 0x9C, 0xD7, 0x99, 0xD7, 0x91 };
static const symbol s_3_1[6] = { 0xD7, 0xA8, 0xD7, 0x99, 0xD7, 0x91 };
static const symbol s_3_2[8] = { 0xD7, 0x98, 0xD7, 0xA8, 0xD7, 0x99, 0xD7, 0x91 };
static const symbol s_3_3[8] = { 0xD7, 0xA9, 0xD7, 0xA8, 0xD7, 0x99, 0xD7, 0x91 };
static const symbol s_3_4[6] = { 0xD7, 0x94, 0xD7, 0xB1, 0xD7, 0x91 };
static const symbol s_3_5[8] = { 0xD7, 0xA9, 0xD7, 0xB0, 0xD7, 0x99, 0xD7, 0x92 };
static const symbol s_3_6[8] = { 0xD7, 0x92, 0xD7, 0x90, 0xD7, 0xA0, 0xD7, 0x92 };
static const symbol s_3_7[8] = { 0xD7, 0x96, 0xD7, 0x95, 0xD7, 0xA0, 0xD7, 0x92 };
static const symbol s_3_8[10] = { 0xD7, 0xA9, 0xD7, 0x9C, 0xD7, 0x95, 0xD7, 0xA0, 0xD7, 0x92 };
static const symbol s_3_9[10] = { 0xD7, 0xA6, 0xD7, 0xB0, 0xD7, 0x95, 0xD7, 0xA0, 0xD7, 0x92 };
static const symbol s_3_10[6] = { 0xD7, 0x91, 0xD7, 0xB1, 0xD7, 0x92 };
static const symbol s_3_11[8] = { 0xD7, 0x91, 0xD7, 0x95, 0xD7, 0xA0, 0xD7, 0x93 };
static const symbol s_3_12[6] = { 0xD7, 0xB0, 0xD7, 0x99, 0xD7, 0x96 };
static const symbol s_3_13[6] = { 0xD7, 0x91, 0xD7, 0x99, 0xD7, 0x98 };
static const symbol s_3_14[6] = { 0xD7, 0x9C, 0xD7, 0x99, 0xD7, 0x98 };
static const symbol s_3_15[6] = { 0xD7, 0x9E, 0xD7, 0x99, 0xD7, 0x98 };
static const symbol s_3_16[8] = { 0xD7, 0xA9, 0xD7, 0xA0, 0xD7, 0x99, 0xD7, 0x98 };
static const symbol s_3_17[6] = { 0xD7, 0xA0, 0xD7, 0x95, 0xD7, 0x9E };
static const symbol s_3_18[8] = { 0xD7, 0xA9, 0xD7, 0x98, 0xD7, 0x90, 0xD7, 0xA0 };
static const symbol s_3_19[6] = { 0xD7, 0x91, 0xD7, 0x99, 0xD7, 0xA1 };
static const symbol s_3_20[8] = { 0xD7, 0xA9, 0xD7, 0x9E, 0xD7, 0x99, 0xD7, 0xA1 };
static const symbol s_3_21[6] = { 0xD7, 0xA8, 0xD7, 0x99, 0xD7, 0xA1 };
static const symbol s_3_22[10] = { 0xD7, 0x98, 0xD7, 0xA8, 0xD7, 0x95, 0xD7, 0xA0, 0xD7, 0xA7 };
static const symbol s_3_23[12] = { 0xD7, 0xA4, 0xD7, 0x90, 0xD7, 0xA8, 0xD7, 0x9C, 0xD7, 0xB1, 0xD7, 0xA8 };
static const symbol s_3_24[8] = { 0xD7, 0xA9, 0xD7, 0xB0, 0xD7, 0xB1, 0xD7, 0xA8 };
static const symbol s_3_25[8] = { 0xD7, 0xB0, 0xD7, 0x95, 0xD7, 0x98, 0xD7, 0xA9 };
static const struct among a_3[26] = {
{ 8, s_3_0, 0, 9, 0},
{ 6, s_3_1, 0, 10, 0},
{ 8, s_3_2, -1, 7, 0},
{ 8, s_3_3, -2, 15, 0},
{ 6, s_3_4, 0, 23, 0},
{ 8, s_3_5, 0, 12, 0},
{ 8, s_3_6, 0, 1, 0},
{ 8, s_3_7, 0, 18, 0},
{ 10, s_3_8, 0, 21, 0},
{ 10, s_3_9, 0, 20, 0},
{ 6, s_3_10, 0, 22, 0},
{ 8, s_3_11, 0, 16, 0},
{ 6, s_3_12, 0, 6, 0},
{ 6, s_3_13, 0, 4, 0},
{ 6, s_3_14, 0, 8, 0},
{ 6, s_3_15, 0, 3, 0},
{ 8, s_3_16, 0, 14, 0},
{ 6, s_3_17, 0, 2, 0},
{ 8, s_3_18, 0, 25, 0},
{ 6, s_3_19, 0, 5, 0},
{ 8, s_3_20, 0, 13, 0},
{ 6, s_3_21, 0, 11, 0},
{ 10, s_3_22, 0, 19, 0},
{ 12, s_3_23, 0, 24, 0},
{ 8, s_3_24, 0, 26, 0},
{ 8, s_3_25, 0, 17, 0}
};

static const symbol s_4_0[6] = { 0xD7, 0x95, 0xD7, 0xA0, 0xD7, 0x92 };
static const symbol s_4_1[6] = { 0xD7, 0xA1, 0xD7, 0x98, 0xD7, 0x95 };
static const symbol s_4_2[2] = { 0xD7, 0x98 };
static const symbol s_4_3[10] = { 0xD7, 0x91, 0xD7, 0xA8, 0xD7, 0x90, 0xD7, 0x9B, 0xD7, 0x98 };
static const symbol s_4_4[4] = { 0xD7, 0xA1, 0xD7, 0x98 };
static const symbol s_4_5[6] = { 0xD7, 0x99, 0xD7, 0xA1, 0xD7, 0x98 };
static const symbol s_4_6[4] = { 0xD7, 0xA2, 0xD7, 0x98 };
static const symbol s_4_7[8] = { 0xD7, 0xA9, 0xD7, 0x90, 0xD7, 0xA4, 0xD7, 0x98 };
static const symbol s_4_8[6] = { 0xD7, 0x94, 0xD7, 0xB2, 0xD7, 0x98 };
static const symbol s_4_9[6] = { 0xD7, 0xA7, 0xD7, 0xB2, 0xD7, 0x98 };
static const symbol s_4_10[8] = { 0xD7, 0x99, 0xD7, 0xA7, 0xD7, 0xB2, 0xD7, 0x98 };
static const symbol s_4_11[6] = { 0xD7, 0x9C, 0xD7, 0xA2, 0xD7, 0x9B };
static const symbol s_4_12[8] = { 0xD7, 0xA2, 0xD7, 0x9C, 0xD7, 0xA2, 0xD7, 0x9B };
static const symbol s_4_13[6] = { 0xD7, 0x99, 0xD7, 0x96, 0xD7, 0x9E };
static const symbol s_4_14[4] = { 0xD7, 0x99, 0xD7, 0x9E };
static const symbol s_4_15[4] = { 0xD7, 0xA2, 0xD7, 0x9E };
static const symbol s_4_16[8] = { 0xD7, 0xA2, 0xD7, 0xA0, 0xD7, 0xA2, 0xD7, 0x9E };
static const symbol s_4_17[10] = { 0xD7, 0x98, 0xD7, 0xA2, 0xD7, 0xA0, 0xD7, 0xA2, 0xD7, 0x9E };
static const symbol s_4_18[2] = { 0xD7, 0xA0 };
static const symbol s_4_19[10] = { 0xD7, 0xA7, 0xD7, 0x9C, 0xD7, 0x99, 0xD7, 0x91, 0xD7, 0xA0 };
static const symbol s_4_20[8] = { 0xD7, 0xA8, 0xD7, 0x99, 0xD7, 0x91, 0xD7, 0xA0 };
static const symbol s_4_21[10] = { 0xD7, 0x98, 0xD7, 0xA8, 0xD7, 0x99, 0xD7, 0x91, 0xD7, 0xA0 };
static const symbol s_4_22[10] = { 0xD7, 0xA9, 0xD7, 0xA8, 0xD7, 0x99, 0xD7, 0x91, 0xD7, 0xA0 };
static const symbol s_4_23[8] = { 0xD7, 0x94, 0xD7, 0xB1, 0xD7, 0x91, 0xD7, 0xA0 };
static const symbol s_4_24[10] = { 0xD7, 0xA9, 0xD7, 0xB0, 0xD7, 0x99, 0xD7, 0x92, 0xD7, 0xA0 };
static const symbol s_4_25[10] = { 0xD7, 0x96, 0xD7, 0x95, 0xD7, 0xA0, 0xD7, 0x92, 0xD7, 0xA0 };
static const symbol s_4_26[12] = { 0xD7, 0xA9, 0xD7, 0x9C, 0xD7, 0x95, 0xD7, 0xA0, 0xD7, 0x92, 0xD7, 0xA0 };
static const symbol s_4_27[12] = { 0xD7, 0xA6, 0xD7, 0xB0, 0xD7, 0x95, 0xD7, 0xA0, 0xD7, 0x92, 0xD7, 0xA0 };
static const symbol s_4_28[8] = { 0xD7, 0x91, 0xD7, 0xB1, 0xD7, 0x92, 0xD7, 0xA0 };
static const symbol s_4_29[10] = { 0xD7, 0x91, 0xD7, 0x95, 0xD7, 0xA0, 0xD7, 0x93, 0xD7, 0xA0 };
static const symbol s_4_30[8] = { 0xD7, 0xB0, 0xD7, 0x99, 0xD7, 0x96, 0xD7, 0xA0 };
static const symbol s_4_31[4] = { 0xD7, 0x98, 0xD7, 0xA0 };
static const symbol s_4_32[10] = { 'G', 'E', 0xD7, 0x91, 0xD7, 0x99, 0xD7, 0x98, 0xD7, 0xA0 };
static const symbol s_4_33[10] = { 'G', 'E', 0xD7, 0x9C, 0xD7, 0x99, 0xD7, 0x98, 0xD7, 0xA0 };
static const symbol s_4_34[10] = { 'G', 'E', 0xD7, 0x9E, 0xD7, 0x99, 0xD7, 0x98, 0xD7, 0xA0 };
static const symbol s_4_35[10] = { 0xD7, 0xA9, 0xD7, 0xA0, 0xD7, 0x99, 0xD7, 0x98, 0xD7, 0xA0 };
static const symbol s_4_36[6] = { 0xD7, 0xA1, 0xD7, 0x98, 0xD7, 0xA0 };
static const symbol s_4_37[8] = { 0xD7, 0x99, 0xD7, 0xA1, 0xD7, 0x98, 0xD7, 0xA0 };
static const symbol s_4_38[6] = { 0xD7, 0xA2, 0xD7, 0x98, 0xD7, 0xA0 };
static const symbol s_4_39[10] = { 'G', 'E', 0xD7, 0x91, 0xD7, 0x99, 0xD7, 0xA1, 0xD7, 0xA0 };
static const symbol s_4_40[10] = { 0xD7, 0xA9, 0xD7, 0x9E, 0xD7, 0x99, 0xD7, 0xA1, 0xD7, 0xA0 };
static const symbol s_4_41[10] = { 'G', 'E', 0xD7, 0xA8, 0xD7, 0x99, 0xD7, 0xA1, 0xD7, 0xA0 };
static const symbol s_4_42[4] = { 0xD7, 0xA2, 0xD7, 0xA0 };
static const symbol s_4_43[12] = { 0xD7, 0x92, 0xD7, 0x90, 0xD7, 0xA0, 0xD7, 0x92, 0xD7, 0xA2, 0xD7, 0xA0 };
static const symbol s_4_44[8] = { 0xD7, 0xA2, 0xD7, 0x9C, 0xD7, 0xA2, 0xD7, 0xA0 };
static const symbol s_4_45[10] = { 0xD7, 0xA0, 0xD7, 0x95, 0xD7, 0x9E, 0xD7, 0xA2, 0xD7, 0xA0 };
static const symbol s_4_46[10] = { 0xD7, 0x99, 0xD7, 0x96, 0xD7, 0x9E, 0xD7, 0xA2, 0xD7, 0xA0 };
static const symbol s_4_47[12] = { 0xD7, 0xA9, 0xD7, 0x98, 0xD7, 0x90, 0xD7, 0xA0, 0xD7, 0xA2, 0xD7, 0xA0 };
static const symbol s_4_48[12] = { 0xD7, 0x98, 0xD7, 0xA8, 0xD7, 0x95, 0xD7, 0xA0, 0xD7, 0xA7, 0xD7, 0xA0 };
static const symbol s_4_49[14] = { 0xD7, 0xA4, 0xD7, 0x90, 0xD7, 0xA8, 0xD7, 0x9C, 0xD7, 0xB1, 0xD7, 0xA8, 0xD7, 0xA0 };
static const symbol s_4_50[10] = { 0xD7, 0xA9, 0xD7, 0xB0, 0xD7, 0xB1, 0xD7, 0xA8, 0xD7, 0xA0 };
static const symbol s_4_51[10] = { 0xD7, 0xB0, 0xD7, 0x95, 0xD7, 0x98, 0xD7, 0xA9, 0xD7, 0xA0 };
static const symbol s_4_52[6] = { 0xD7, 0x92, 0xD7, 0xB2, 0xD7, 0xA0 };
static const symbol s_4_53[2] = { 0xD7, 0xA1 };
static const symbol s_4_54[4] = { 0xD7, 0x98, 0xD7, 0xA1 };
static const symbol s_4_55[6] = { 0xD7, 0xA2, 0xD7, 0x98, 0xD7, 0xA1 };
static const symbol s_4_56[4] = { 0xD7, 0xA0, 0xD7, 0xA1 };
static const symbol s_4_57[6] = { 0xD7, 0x98, 0xD7, 0xA0, 0xD7, 0xA1 };
static const symbol s_4_58[6] = { 0xD7, 0xA2, 0xD7, 0xA0, 0xD7, 0xA1 };
static const symbol s_4_59[4] = { 0xD7, 0xA2, 0xD7, 0xA1 };
static const symbol s_4_60[6] = { 0xD7, 0x99, 0xD7, 0xA2, 0xD7, 0xA1 };
static const symbol s_4_61[8] = { 0xD7, 0xA2, 0xD7, 0x9C, 0xD7, 0xA2, 0xD7, 0xA1 };
static const symbol s_4_62[6] = { 0xD7, 0xA2, 0xD7, 0xA8, 0xD7, 0xA1 };
static const symbol s_4_63[10] = { 0xD7, 0xA2, 0xD7, 0xA0, 0xD7, 0xA2, 0xD7, 0xA8, 0xD7, 0xA1 };
static const symbol s_4_64[2] = { 0xD7, 0xA2 };
static const symbol s_4_65[4] = { 0xD7, 0x98, 0xD7, 0xA2 };
static const symbol s_4_66[6] = { 0xD7, 0xA1, 0xD7, 0x98, 0xD7, 0xA2 };
static const symbol s_4_67[6] = { 0xD7, 0xA2, 0xD7, 0x98, 0xD7, 0xA2 };
static const symbol s_4_68[4] = { 0xD7, 0x99, 0xD7, 0xA2 };
static const symbol s_4_69[6] = { 0xD7, 0xA2, 0xD7, 0x9C, 0xD7, 0xA2 };
static const symbol s_4_70[6] = { 0xD7, 0xA2, 0xD7, 0xA0, 0xD7, 0xA2 };
static const symbol s_4_71[8] = { 0xD7, 0x98, 0xD7, 0xA2, 0xD7, 0xA0, 0xD7, 0xA2 };
static const symbol s_4_72[4] = { 0xD7, 0xA2, 0xD7, 0xA8 };
static const symbol s_4_73[6] = { 0xD7, 0x98, 0xD7, 0xA2, 0xD7, 0xA8 };
static const symbol s_4_74[8] = { 0xD7, 0xA1, 0xD7, 0x98, 0xD7, 0xA2, 0xD7, 0xA8 };
static const symbol s_4_75[8] = { 0xD7, 0xA2, 0xD7, 0x98, 0xD7, 0xA2, 0xD7, 0xA8 };
static const symbol s_4_76[8] = { 0xD7, 0xA2, 0xD7, 0xA0, 0xD7, 0xA2, 0xD7, 0xA8 };
static const symbol s_4_77[10] = { 0xD7, 0x98, 0xD7, 0xA2, 0xD7, 0xA0, 0xD7, 0xA2, 0xD7, 0xA8 };
static const symbol s_4_78[4] = { 0xD7, 0x95, 0xD7, 0xAA };
static const struct among a_4[79] = {
{ 6, s_4_0, 0, 1, 0},
{ 6, s_4_1, 0, 1, 0},
{ 2, s_4_2, 0, 1, 0},
{ 10, s_4_3, -1, 31, 0},
{ 4, s_4_4, -2, 1, 0},
{ 6, s_4_5, -1, 33, 0},
{ 4, s_4_6, -4, 1, 0},
{ 8, s_4_7, -5, 1, 0},
{ 6, s_4_8, -6, 1, 0},
{ 6, s_4_9, -7, 1, 0},
{ 8, s_4_10, -1, 1, 0},
{ 6, s_4_11, 0, 1, 0},
{ 8, s_4_12, -1, 1, 0},
{ 6, s_4_13, 0, 1, 0},
{ 4, s_4_14, 0, 1, 0},
{ 4, s_4_15, 0, 1, 0},
{ 8, s_4_16, -1, 3, 0},
{ 10, s_4_17, -1, 4, 0},
{ 2, s_4_18, 0, 1, 0},
{ 10, s_4_19, -1, 14, 0},
{ 8, s_4_20, -2, 15, 0},
{ 10, s_4_21, -1, 12, 0},
{ 10, s_4_22, -2, 7, 0},
{ 8, s_4_23, -5, 27, 0},
{ 10, s_4_24, -6, 17, 0},
{ 10, s_4_25, -7, 22, 0},
{ 12, s_4_26, -8, 25, 0},
{ 12, s_4_27, -9, 24, 0},
{ 8, s_4_28, -10, 26, 0},
{ 10, s_4_29, -11, 20, 0},
{ 8, s_4_30, -12, 11, 0},
{ 4, s_4_31, -13, 4, 0},
{ 10, s_4_32, -1, 9, 0},
{ 10, s_4_33, -2, 13, 0},
{ 10, s_4_34, -3, 8, 0},
{ 10, s_4_35, -4, 19, 0},
{ 6, s_4_36, -5, 1, 0},
{ 8, s_4_37, -1, 1, 0},
{ 6, s_4_38, -7, 1, 0},
{ 10, s_4_39, -21, 10, 0},
{ 10, s_4_40, -22, 18, 0},
{ 10, s_4_41, -23, 16, 0},
{ 4, s_4_42, -24, 1, 0},
{ 12, s_4_43, -1, 5, 0},
{ 8, s_4_44, -2, 1, 0},
{ 10, s_4_45, -3, 6, 0},
{ 10, s_4_46, -4, 1, 0},
{ 12, s_4_47, -5, 29, 0},
{ 12, s_4_48, -30, 23, 0},
{ 14, s_4_49, -31, 28, 0},
{ 10, s_4_50, -32, 30, 0},
{ 10, s_4_51, -33, 21, 0},
{ 6, s_4_52, -34, 5, 0},
{ 2, s_4_53, 0, 1, 0},
{ 4, s_4_54, -1, 4, 0},
{ 6, s_4_55, -1, 1, 0},
{ 4, s_4_56, -3, 1, 0},
{ 6, s_4_57, -1, 4, 0},
{ 6, s_4_58, -2, 3, 0},
{ 4, s_4_59, -6, 1, 0},
{ 6, s_4_60, -1, 2, 0},
{ 8, s_4_61, -2, 1, 0},
{ 6, s_4_62, -9, 1, 0},
{ 10, s_4_63, -1, 1, 0},
{ 2, s_4_64, 0, 1, 0},
{ 4, s_4_65, -1, 4, 0},
{ 6, s_4_66, -1, 1, 0},
{ 6, s_4_67, -2, 1, 0},
{ 4, s_4_68, -4, -1, 0},
{ 6, s_4_69, -5, 1, 0},
{ 6, s_4_70, -6, 3, 0},
{ 8, s_4_71, -1, 4, 0},
{ 4, s_4_72, 0, 1, 0},
{ 6, s_4_73, -1, 4, 0},
{ 8, s_4_74, -1, 1, 0},
{ 8, s_4_75, -2, 1, 0},
{ 8, s_4_76, -4, 3, 0},
{ 10, s_4_77, -1, 4, 0},
{ 4, s_4_78, 0, 32, 0}
};

static const symbol s_5_0[6] = { 0xD7, 0x95, 0xD7, 0xA0, 0xD7, 0x92 };
static const symbol s_5_1[8] = { 0xD7, 0xA9, 0xD7, 0x90, 0xD7, 0xA4, 0xD7, 0x98 };
static const symbol s_5_2[6] = { 0xD7, 0x94, 0xD7, 0xB2, 0xD7, 0x98 };
static const symbol s_5_3[6] = { 0xD7, 0xA7, 0xD7, 0xB2, 0xD7, 0x98 };
static const symbol s_5_4[8] = { 0xD7, 0x99, 0xD7, 0xA7, 0xD7, 0xB2, 0xD7, 0x98 };
static const symbol s_5_5[2] = { 0xD7, 0x9C };
static const struct among a_5[6] = {
{ 6, s_5_0, 0, 1, 0},
{ 8, s_5_1, 0, 1, 0},
{ 6, s_5_2, 0, 1, 0},
{ 6, s_5_3, 0, 1, 0},
{ 8, s_5_4, -1, 1, 0},
{ 2, s_5_5, 0, 2, 0}
};

static const symbol s_6_0[4] = { 0xD7, 0x99, 0xD7, 0x92 };
static const symbol s_6_1[4] = { 0xD7, 0x99, 0xD7, 0xA7 };
static const symbol s_6_2[6] = { 0xD7, 0x93, 0xD7, 0x99, 0xD7, 0xA7 };
static const symbol s_6_3[8] = { 0xD7, 0xA0, 0xD7, 0x93, 0xD7, 0x99, 0xD7, 0xA7 };
static const symbol s_6_4[10] = { 0xD7, 0xA2, 0xD7, 0xA0, 0xD7, 0x93, 0xD7, 0x99, 0xD7, 0xA7 };
static const symbol s_6_5[8] = { 0xD7, 0x91, 0xD7, 0x9C, 0xD7, 0x99, 0xD7, 0xA7 };
static const symbol s_6_6[8] = { 0xD7, 0x92, 0xD7, 0x9C, 0xD7, 0x99, 0xD7, 0xA7 };
static const symbol s_6_7[6] = { 0xD7, 0xA0, 0xD7, 0x99, 0xD7, 0xA7 };
static const symbol s_6_8[4] = { 0xD7, 0x99, 0xD7, 0xA9 };
static const struct among a_6[9] = {
{ 4, s_6_0, 0, 1, 0},
{ 4, s_6_1, 0, 1, 0},
{ 6, s_6_2, -1, 1, 0},
{ 8, s_6_3, -1, 1, 0},
{ 10, s_6_4, -1, 1, 0},
{ 8, s_6_5, -4, -1, 0},
{ 8, s_6_6, -5, -1, 0},
{ 6, s_6_7, -6, 1, 0},
{ 4, s_6_8, 0, 1, 0}
};

static const unsigned char g_niked[] = { 255, 155, 6 };

static const unsigned char g_vowel[] = { 33, 2, 4, 0, 6 };

static const unsigned char g_consonant[] = { 239, 254, 253, 131 };

static int r_prelude(struct SN_env * z) {
    int among_var;
    {
        int v_1 = z->c;
        while (1) {
            int v_2 = z->c;
            while (1) {
                int v_3 = z->c;
                z->bra = z->c;
                among_var = find_among(z, a_0, 8, 0);
                if (!among_var) goto lab2;
                z->ket = z->c;
                switch (among_var) {
                    case 1:
                        {
                            int v_4 = z->c;
                            if (!(eq_s(z, 2, s_0))) goto lab3;
                            goto lab2;
                        lab3:
                            z->c = v_4;
                        }
                        {
                            int ret = slice_from_s(z, 2, s_1);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 2:
                        {
                            int v_5 = z->c;
                            if (!(eq_s(z, 2, s_2))) goto lab4;
                            goto lab2;
                        lab4:
                            z->c = v_5;
                        }
                        {
                            int ret = slice_from_s(z, 2, s_3);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 3:
                        {
                            int v_6 = z->c;
                            if (!(eq_s(z, 2, s_4))) goto lab5;
                            goto lab2;
                        lab5:
                            z->c = v_6;
                        }
                        {
                            int ret = slice_from_s(z, 2, s_5);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 4:
                        {
                            int ret = slice_from_s(z, 2, s_6);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 5:
                        {
                            int ret = slice_from_s(z, 2, s_7);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 6:
                        {
                            int ret = slice_from_s(z, 2, s_8);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 7:
                        {
                            int ret = slice_from_s(z, 2, s_9);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 8:
                        {
                            int ret = slice_from_s(z, 2, s_10);
                            if (ret < 0) return ret;
                        }
                        break;
                }
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
            continue;
        lab1:
            z->c = v_2;
            break;
        }
        z->c = v_1;
    }
    {
        int v_7 = z->c;
        while (1) {
            int v_8 = z->c;
            while (1) {
                int v_9 = z->c;
                z->bra = z->c;
                if (in_grouping_U(z, g_niked, 1456, 1474, 0)) goto lab8;
                z->ket = z->c;
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                z->c = v_9;
                break;
            lab8:
                z->c = v_9;
                {
                    int ret = skip_utf8(z->p, z->c, z->l, 1);
                    if (ret < 0) goto lab7;
                    z->c = ret;
                }
            }
            continue;
        lab7:
            z->c = v_8;
            break;
        }
        z->c = v_7;
    }
    return 1;
}

static int r_mark_regions(struct SN_env * z) {
    int i_x;
    ((SN_local *)z)->i_p1 = z->l;
    {
        int v_1 = z->c;
        z->bra = z->c;
        if (!(eq_s(z, 4, s_11))) { z->c = v_1; goto lab0; }
        z->ket = z->c;
        {
            int v_2 = z->c;
            do {
                int v_3 = z->c;
                if (!(eq_s(z, 4, s_12))) goto lab2;
                break;
            lab2:
                z->c = v_3;
                if (!(eq_s(z, 4, s_13))) goto lab3;
                break;
            lab3:
                z->c = v_3;
                if (z->c < z->l) goto lab1;
            } while (0);
            { z->c = v_1; goto lab0; }
        lab1:
            z->c = v_2;
        }
        {
            int ret = slice_from_s(z, 2, s_14);
            if (ret < 0) return ret;
        }
    lab0:
        ;
    }
    {
        int v_4 = z->c;
        if (!find_among(z, a_1, 40, 0)) { z->c = v_4; goto lab4; }
        do {
            int v_5 = z->c;
            {
                int v_6 = z->c;
                do {
                    int v_7 = z->c;
                    if (!(eq_s(z, 8, s_15))) goto lab6;
                    break;
                lab6:
                    z->c = v_7;
                    if (!(eq_s(z, 8, s_16))) goto lab7;
                    break;
                lab7:
                    z->c = v_7;
                    if (!(eq_s(z, 8, s_17))) goto lab5;
                } while (0);
                if (z->c < z->l) goto lab5;
                z->c = v_6;
            }
            break;
        lab5:
            z->c = v_5;
            {
                int v_8 = z->c;
                if (!(eq_s(z, 8, s_18))) goto lab8;
                z->c = v_8;
            }
            break;
        lab8:
            z->c = v_5;
            z->bra = z->c;
            if (!(eq_s(z, 4, s_19))) goto lab9;
            z->ket = z->c;
            {
                int ret = slice_from_s(z, 2, s_20);
                if (ret < 0) return ret;
            }
            break;
        lab9:
            z->c = v_5;
            z->bra = z->c;
            if (!(eq_s(z, 4, s_21))) { z->c = v_4; goto lab4; }
            z->ket = z->c;
            {
                int ret = slice_from_s(z, 3, s_22);
                if (ret < 0) return ret;
            }
        } while (0);
    lab4:
        ;
    }
    {
        int v_9 = z->c;
        {
            int ret = skip_utf8(z->p, z->c, z->l, 3);
            if (ret < 0) return 0;
            z->c = ret;
        }
        i_x = z->c;
        z->c = v_9;
    }
    {
        int v_10 = z->c;
        if (z->c + 5 >= z->l || (z->p[z->c + 5] != 169 && z->p[z->c + 5] != 168)) { z->c = v_10; goto lab10; }
        if (!find_among(z, a_2, 4, 0)) { z->c = v_10; goto lab10; }
    lab10:
        ;
    }
    {
        int v_11 = z->c;
        if (in_grouping_U(z, g_consonant, 1489, 1520, 0)) goto lab11;
        if (in_grouping_U(z, g_consonant, 1489, 1520, 0)) goto lab11;
        if (in_grouping_U(z, g_consonant, 1489, 1520, 0)) goto lab11;
        ((SN_local *)z)->i_p1 = z->c;
        return 0;
    lab11:
        z->c = v_11;
    }
    {
        int ret = out_grouping_U(z, g_vowel, 1488, 1522, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    if (in_grouping_U(z, g_vowel, 1488, 1522, 1) < 0) return 0;
    ((SN_local *)z)->i_p1 = z->c;
    if (((SN_local *)z)->i_p1 >= i_x) goto lab12;
    ((SN_local *)z)->i_p1 = i_x;
lab12:
    return 1;
}

static int r_R1(struct SN_env * z) {
    return ((SN_local *)z)->i_p1 <= z->c;
}

static int r_R1plus3(struct SN_env * z) {
    return ((SN_local *)z)->i_p1 <= (z->c + 6);
}

static int r_standard_suffix(struct SN_env * z) {
    int among_var;
    {
        int v_1 = z->l - z->c;
        z->ket = z->c;
        among_var = find_among_b(z, a_4, 79, 0);
        if (!among_var) goto lab0;
        z->bra = z->c;
        switch (among_var) {
            case 1:
                {
                    int ret = r_R1(z);
                    if (ret == 0) goto lab0;
                    if (ret < 0) return ret;
                }
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                break;
            case 2:
                {
                    int ret = r_R1(z);
                    if (ret == 0) goto lab0;
                    if (ret < 0) return ret;
                }
                {
                    int ret = slice_from_s(z, 4, s_23);
                    if (ret < 0) return ret;
                }
                break;
            case 3:
                {
                    int ret = r_R1(z);
                    if (ret == 0) goto lab0;
                    if (ret < 0) return ret;
                }
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                z->ket = z->c;
                among_var = find_among_b(z, a_3, 26, 0);
                if (!among_var) goto lab0;
                z->bra = z->c;
                switch (among_var) {
                    case 1:
                        {
                            int ret = slice_from_s(z, 4, s_24);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 2:
                        {
                            int ret = slice_from_s(z, 6, s_25);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 3:
                        {
                            int ret = slice_from_s(z, 6, s_26);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 4:
                        {
                            int ret = slice_from_s(z, 6, s_27);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 5:
                        {
                            int ret = slice_from_s(z, 6, s_28);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 6:
                        {
                            int ret = slice_from_s(z, 6, s_29);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 7:
                        {
                            int ret = slice_from_s(z, 8, s_30);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 8:
                        {
                            int ret = slice_from_s(z, 6, s_31);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 9:
                        {
                            int ret = slice_from_s(z, 8, s_32);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 10:
                        {
                            int ret = slice_from_s(z, 6, s_33);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 11:
                        {
                            int ret = slice_from_s(z, 6, s_34);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 12:
                        {
                            int ret = slice_from_s(z, 8, s_35);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 13:
                        {
                            int ret = slice_from_s(z, 8, s_36);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 14:
                        {
                            int ret = slice_from_s(z, 8, s_37);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 15:
                        {
                            int ret = slice_from_s(z, 8, s_38);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 16:
                        {
                            int ret = slice_from_s(z, 8, s_39);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 17:
                        {
                            int ret = slice_from_s(z, 8, s_40);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 18:
                        {
                            int ret = slice_from_s(z, 8, s_41);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 19:
                        {
                            int ret = slice_from_s(z, 10, s_42);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 20:
                        {
                            int ret = slice_from_s(z, 10, s_43);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 21:
                        {
                            int ret = slice_from_s(z, 10, s_44);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 22:
                        {
                            int ret = slice_from_s(z, 6, s_45);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 23:
                        {
                            int ret = slice_from_s(z, 6, s_46);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 24:
                        {
                            int ret = slice_from_s(z, 12, s_47);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 25:
                        {
                            int ret = slice_from_s(z, 6, s_48);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 26:
                        {
                            int ret = slice_from_s(z, 8, s_49);
                            if (ret < 0) return ret;
                        }
                        break;
                }
                break;
            case 4:
                do {
                    int v_2 = z->l - z->c;
                    {
                        int ret = r_R1(z);
                        if (ret == 0) goto lab1;
                        if (ret < 0) return ret;
                    }
                    {
                        int ret = slice_del(z);
                        if (ret < 0) return ret;
                    }
                    break;
                lab1:
                    z->c = z->l - v_2;
                    {
                        int ret = slice_from_s(z, 2, s_50);
                        if (ret < 0) return ret;
                    }
                } while (0);
                z->ket = z->c;
                if (!(eq_s_b(z, 8, s_51))) goto lab0;
                {
                    int v_3 = z->l - z->c;
                    if (!(eq_s_b(z, 4, s_52))) { z->c = z->l - v_3; goto lab2; }
                lab2:
                    ;
                }
                z->bra = z->c;
                {
                    int ret = slice_from_s(z, 10, s_53);
                    if (ret < 0) return ret;
                }
                break;
            case 5:
                {
                    int ret = slice_from_s(z, 4, s_54);
                    if (ret < 0) return ret;
                }
                break;
            case 6:
                {
                    int ret = slice_from_s(z, 6, s_55);
                    if (ret < 0) return ret;
                }
                break;
            case 7:
                {
                    int ret = slice_from_s(z, 8, s_56);
                    if (ret < 0) return ret;
                }
                break;
            case 8:
                {
                    int ret = slice_from_s(z, 6, s_57);
                    if (ret < 0) return ret;
                }
                break;
            case 9:
                {
                    int ret = slice_from_s(z, 6, s_58);
                    if (ret < 0) return ret;
                }
                break;
            case 10:
                {
                    int ret = slice_from_s(z, 6, s_59);
                    if (ret < 0) return ret;
                }
                break;
            case 11:
                {
                    int ret = slice_from_s(z, 6, s_60);
                    if (ret < 0) return ret;
                }
                break;
            case 12:
                {
                    int ret = slice_from_s(z, 8, s_61);
                    if (ret < 0) return ret;
                }
                break;
            case 13:
                {
                    int ret = slice_from_s(z, 6, s_62);
                    if (ret < 0) return ret;
                }
                break;
            case 14:
                {
                    int ret = slice_from_s(z, 8, s_63);
                    if (ret < 0) return ret;
                }
                break;
            case 15:
                {
                    int ret = slice_from_s(z, 6, s_64);
                    if (ret < 0) return ret;
                }
                break;
            case 16:
                {
                    int ret = slice_from_s(z, 6, s_65);
                    if (ret < 0) return ret;
                }
                break;
            case 17:
                {
                    int ret = slice_from_s(z, 8, s_66);
                    if (ret < 0) return ret;
                }
                break;
            case 18:
                {
                    int ret = slice_from_s(z, 8, s_67);
                    if (ret < 0) return ret;
                }
                break;
            case 19:
                {
                    int ret = slice_from_s(z, 8, s_68);
                    if (ret < 0) return ret;
                }
                break;
            case 20:
                {
                    int ret = slice_from_s(z, 8, s_69);
                    if (ret < 0) return ret;
                }
                break;
            case 21:
                {
                    int ret = slice_from_s(z, 8, s_70);
                    if (ret < 0) return ret;
                }
                break;
            case 22:
                {
                    int ret = slice_from_s(z, 8, s_71);
                    if (ret < 0) return ret;
                }
                break;
            case 23:
                {
                    int ret = slice_from_s(z, 10, s_72);
                    if (ret < 0) return ret;
                }
                break;
            case 24:
                {
                    int ret = slice_from_s(z, 10, s_73);
                    if (ret < 0) return ret;
                }
                break;
            case 25:
                {
                    int ret = slice_from_s(z, 10, s_74);
                    if (ret < 0) return ret;
                }
                break;
            case 26:
                {
                    int ret = slice_from_s(z, 6, s_75);
                    if (ret < 0) return ret;
                }
                break;
            case 27:
                {
                    int ret = slice_from_s(z, 6, s_76);
                    if (ret < 0) return ret;
                }
                break;
            case 28:
                {
                    int ret = slice_from_s(z, 12, s_77);
                    if (ret < 0) return ret;
                }
                break;
            case 29:
                {
                    int ret = slice_from_s(z, 6, s_78);
                    if (ret < 0) return ret;
                }
                break;
            case 30:
                {
                    int ret = slice_from_s(z, 8, s_79);
                    if (ret < 0) return ret;
                }
                break;
            case 31:
                {
                    int ret = slice_from_s(z, 10, s_80);
                    if (ret < 0) return ret;
                }
                break;
            case 32:
                {
                    int ret = r_R1(z);
                    if (ret == 0) goto lab0;
                    if (ret < 0) return ret;
                }
                {
                    int ret = slice_from_s(z, 2, s_81);
                    if (ret < 0) return ret;
                }
                break;
            case 33:
                do {
                    int v_4 = z->l - z->c;
                    do {
                        int v_5 = z->l - z->c;
                        if (!(eq_s_b(z, 2, s_82))) goto lab4;
                        break;
                    lab4:
                        z->c = z->l - v_5;
                        if (!(eq_s_b(z, 2, s_83))) goto lab3;
                    } while (0);
                    {
                        int v_6 = z->l - z->c;
                        {
                            int ret = r_R1plus3(z);
                            if (ret == 0) { z->c = z->l - v_6; goto lab5; }
                            if (ret < 0) return ret;
                        }
                        {
                            int ret = slice_from_s(z, 4, s_84);
                            if (ret < 0) return ret;
                        }
                    lab5:
                        ;
                    }
                    break;
                lab3:
                    z->c = z->l - v_4;
                    {
                        int ret = r_R1(z);
                        if (ret == 0) goto lab0;
                        if (ret < 0) return ret;
                    }
                    {
                        int ret = slice_del(z);
                        if (ret < 0) return ret;
                    }
                } while (0);
                break;
        }
    lab0:
        z->c = z->l - v_1;
    }
    {
        int v_7 = z->l - z->c;
        z->ket = z->c;
        if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 4 || !((285474816 >> (z->p[z->c - 1] & 0x1f)) & 1)) goto lab6;
        among_var = find_among_b(z, a_5, 6, 0);
        if (!among_var) goto lab6;
        z->bra = z->c;
        switch (among_var) {
            case 1:
                {
                    int ret = r_R1(z);
                    if (ret == 0) goto lab6;
                    if (ret < 0) return ret;
                }
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                break;
            case 2:
                {
                    int ret = r_R1(z);
                    if (ret == 0) goto lab6;
                    if (ret < 0) return ret;
                }
                if (in_grouping_b_U(z, g_consonant, 1489, 1520, 0)) goto lab6;
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                break;
        }
    lab6:
        z->c = z->l - v_7;
    }
    {
        int v_8 = z->l - z->c;
        z->ket = z->c;
        among_var = find_among_b(z, a_6, 9, 0);
        if (!among_var) goto lab7;
        z->bra = z->c;
        switch (among_var) {
            case 1:
                {
                    int ret = r_R1(z);
                    if (ret == 0) goto lab7;
                    if (ret < 0) return ret;
                }
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                break;
        }
    lab7:
        z->c = z->l - v_8;
    }
    {
        int v_9 = z->l - z->c;
        while (1) {
            int v_10 = z->l - z->c;
            while (1) {
                int v_11 = z->l - z->c;
                z->ket = z->c;
                do {
                    int v_12 = z->l - z->c;
                    if (!(eq_s_b(z, 2, s_85))) goto lab11;
                    break;
                lab11:
                    z->c = z->l - v_12;
                    if (!(eq_s_b(z, 3, s_86))) goto lab10;
                } while (0);
                z->bra = z->c;
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                z->c = z->l - v_11;
                break;
            lab10:
                z->c = z->l - v_11;
                {
                    int ret = skip_b_utf8(z->p, z->c, z->lb, 1);
                    if (ret < 0) goto lab9;
                    z->c = ret;
                }
            }
            continue;
        lab9:
            z->c = z->l - v_10;
            break;
        }
        z->c = z->l - v_9;
    }
    return 1;
}

extern int yiddish_UTF_8_stem(struct SN_env * z) {
    {
        int ret = r_prelude(z);
        if (ret < 0) return ret;
    }
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
        int ret = r_standard_suffix(z);
        if (ret < 0) return ret;
    }
    z->c = z->lb;
    return 1;
}

extern struct SN_env * yiddish_UTF_8_create_env(void) {
    struct SN_env * z = SN_new_env(sizeof(SN_local));
    if (z) {
        ((SN_local *)z)->i_p1 = 0;
    }
    return z;
}

extern void yiddish_UTF_8_close_env(struct SN_env * z) {
    SN_delete_env(z);
}

