/* This file was generated automatically by the Snowball to ISO C compiler */
/* http://snowballstem.org/ */

#include "header.h"

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
#ifdef __cplusplus
extern "C" {
#endif


extern struct SN_env * russian_UTF_8_create_env(void);
extern void russian_UTF_8_close_env(struct SN_env * z);


#ifdef __cplusplus
}
#endif
static const symbol s_0_0[10] = { 0xD0, 0xB2, 0xD1, 0x88, 0xD0, 0xB8, 0xD1, 0x81, 0xD1, 0x8C };
static const symbol s_0_1[12] = { 0xD1, 0x8B, 0xD0, 0xB2, 0xD1, 0x88, 0xD0, 0xB8, 0xD1, 0x81, 0xD1, 0x8C };
static const symbol s_0_2[12] = { 0xD0, 0xB8, 0xD0, 0xB2, 0xD1, 0x88, 0xD0, 0xB8, 0xD1, 0x81, 0xD1, 0x8C };
static const symbol s_0_3[2] = { 0xD0, 0xB2 };
static const symbol s_0_4[4] = { 0xD1, 0x8B, 0xD0, 0xB2 };
static const symbol s_0_5[4] = { 0xD0, 0xB8, 0xD0, 0xB2 };
static const symbol s_0_6[6] = { 0xD0, 0xB2, 0xD1, 0x88, 0xD0, 0xB8 };
static const symbol s_0_7[8] = { 0xD1, 0x8B, 0xD0, 0xB2, 0xD1, 0x88, 0xD0, 0xB8 };
static const symbol s_0_8[8] = { 0xD0, 0xB8, 0xD0, 0xB2, 0xD1, 0x88, 0xD0, 0xB8 };

static const struct among a_0[9] =
{
/*  0 */ { 10, s_0_0, -1, 1, 0},
/*  1 */ { 12, s_0_1, 0, 2, 0},
/*  2 */ { 12, s_0_2, 0, 2, 0},
/*  3 */ { 2, s_0_3, -1, 1, 0},
/*  4 */ { 4, s_0_4, 3, 2, 0},
/*  5 */ { 4, s_0_5, 3, 2, 0},
/*  6 */ { 6, s_0_6, -1, 1, 0},
/*  7 */ { 8, s_0_7, 6, 2, 0},
/*  8 */ { 8, s_0_8, 6, 2, 0}
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

static const struct among a_1[26] =
{
/*  0 */ { 6, s_1_0, -1, 1, 0},
/*  1 */ { 6, s_1_1, -1, 1, 0},
/*  2 */ { 4, s_1_2, -1, 1, 0},
/*  3 */ { 4, s_1_3, -1, 1, 0},
/*  4 */ { 4, s_1_4, -1, 1, 0},
/*  5 */ { 4, s_1_5, -1, 1, 0},
/*  6 */ { 4, s_1_6, -1, 1, 0},
/*  7 */ { 4, s_1_7, -1, 1, 0},
/*  8 */ { 4, s_1_8, -1, 1, 0},
/*  9 */ { 4, s_1_9, -1, 1, 0},
/* 10 */ { 4, s_1_10, -1, 1, 0},
/* 11 */ { 4, s_1_11, -1, 1, 0},
/* 12 */ { 4, s_1_12, -1, 1, 0},
/* 13 */ { 4, s_1_13, -1, 1, 0},
/* 14 */ { 6, s_1_14, -1, 1, 0},
/* 15 */ { 6, s_1_15, -1, 1, 0},
/* 16 */ { 4, s_1_16, -1, 1, 0},
/* 17 */ { 4, s_1_17, -1, 1, 0},
/* 18 */ { 4, s_1_18, -1, 1, 0},
/* 19 */ { 4, s_1_19, -1, 1, 0},
/* 20 */ { 4, s_1_20, -1, 1, 0},
/* 21 */ { 4, s_1_21, -1, 1, 0},
/* 22 */ { 4, s_1_22, -1, 1, 0},
/* 23 */ { 4, s_1_23, -1, 1, 0},
/* 24 */ { 6, s_1_24, -1, 1, 0},
/* 25 */ { 6, s_1_25, -1, 1, 0}
};

static const symbol s_2_0[4] = { 0xD0, 0xB2, 0xD1, 0x88 };
static const symbol s_2_1[6] = { 0xD1, 0x8B, 0xD0, 0xB2, 0xD1, 0x88 };
static const symbol s_2_2[6] = { 0xD0, 0xB8, 0xD0, 0xB2, 0xD1, 0x88 };
static const symbol s_2_3[2] = { 0xD1, 0x89 };
static const symbol s_2_4[4] = { 0xD1, 0x8E, 0xD1, 0x89 };
static const symbol s_2_5[6] = { 0xD1, 0x83, 0xD1, 0x8E, 0xD1, 0x89 };
static const symbol s_2_6[4] = { 0xD0, 0xB5, 0xD0, 0xBC };
static const symbol s_2_7[4] = { 0xD0, 0xBD, 0xD0, 0xBD };

static const struct among a_2[8] =
{
/*  0 */ { 4, s_2_0, -1, 1, 0},
/*  1 */ { 6, s_2_1, 0, 2, 0},
/*  2 */ { 6, s_2_2, 0, 2, 0},
/*  3 */ { 2, s_2_3, -1, 1, 0},
/*  4 */ { 4, s_2_4, 3, 1, 0},
/*  5 */ { 6, s_2_5, 4, 2, 0},
/*  6 */ { 4, s_2_6, -1, 1, 0},
/*  7 */ { 4, s_2_7, -1, 1, 0}
};

static const symbol s_3_0[4] = { 0xD1, 0x81, 0xD1, 0x8C };
static const symbol s_3_1[4] = { 0xD1, 0x81, 0xD1, 0x8F };

static const struct among a_3[2] =
{
/*  0 */ { 4, s_3_0, -1, 1, 0},
/*  1 */ { 4, s_3_1, -1, 1, 0}
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

static const struct among a_4[46] =
{
/*  0 */ { 4, s_4_0, -1, 2, 0},
/*  1 */ { 4, s_4_1, -1, 1, 0},
/*  2 */ { 6, s_4_2, 1, 2, 0},
/*  3 */ { 4, s_4_3, -1, 2, 0},
/*  4 */ { 4, s_4_4, -1, 1, 0},
/*  5 */ { 6, s_4_5, 4, 2, 0},
/*  6 */ { 4, s_4_6, -1, 2, 0},
/*  7 */ { 4, s_4_7, -1, 1, 0},
/*  8 */ { 6, s_4_8, 7, 2, 0},
/*  9 */ { 4, s_4_9, -1, 1, 0},
/* 10 */ { 6, s_4_10, 9, 2, 0},
/* 11 */ { 6, s_4_11, 9, 2, 0},
/* 12 */ { 6, s_4_12, -1, 1, 0},
/* 13 */ { 6, s_4_13, -1, 2, 0},
/* 14 */ { 2, s_4_14, -1, 2, 0},
/* 15 */ { 4, s_4_15, 14, 2, 0},
/* 16 */ { 4, s_4_16, -1, 1, 0},
/* 17 */ { 6, s_4_17, 16, 2, 0},
/* 18 */ { 6, s_4_18, 16, 2, 0},
/* 19 */ { 4, s_4_19, -1, 1, 0},
/* 20 */ { 6, s_4_20, 19, 2, 0},
/* 21 */ { 6, s_4_21, -1, 1, 0},
/* 22 */ { 6, s_4_22, -1, 2, 0},
/* 23 */ { 6, s_4_23, -1, 1, 0},
/* 24 */ { 8, s_4_24, 23, 2, 0},
/* 25 */ { 8, s_4_25, 23, 2, 0},
/* 26 */ { 4, s_4_26, -1, 1, 0},
/* 27 */ { 6, s_4_27, 26, 2, 0},
/* 28 */ { 6, s_4_28, 26, 2, 0},
/* 29 */ { 2, s_4_29, -1, 1, 0},
/* 30 */ { 4, s_4_30, 29, 2, 0},
/* 31 */ { 4, s_4_31, 29, 2, 0},
/* 32 */ { 2, s_4_32, -1, 1, 0},
/* 33 */ { 4, s_4_33, 32, 2, 0},
/* 34 */ { 4, s_4_34, 32, 2, 0},
/* 35 */ { 4, s_4_35, -1, 2, 0},
/* 36 */ { 4, s_4_36, -1, 1, 0},
/* 37 */ { 4, s_4_37, -1, 2, 0},
/* 38 */ { 2, s_4_38, -1, 1, 0},
/* 39 */ { 4, s_4_39, 38, 2, 0},
/* 40 */ { 4, s_4_40, -1, 1, 0},
/* 41 */ { 6, s_4_41, 40, 2, 0},
/* 42 */ { 6, s_4_42, 40, 2, 0},
/* 43 */ { 4, s_4_43, -1, 1, 0},
/* 44 */ { 6, s_4_44, 43, 2, 0},
/* 45 */ { 6, s_4_45, 43, 1, 0}
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

static const struct among a_5[36] =
{
/*  0 */ { 2, s_5_0, -1, 1, 0},
/*  1 */ { 4, s_5_1, -1, 1, 0},
/*  2 */ { 6, s_5_2, 1, 1, 0},
/*  3 */ { 4, s_5_3, -1, 1, 0},
/*  4 */ { 2, s_5_4, -1, 1, 0},
/*  5 */ { 2, s_5_5, -1, 1, 0},
/*  6 */ { 2, s_5_6, -1, 1, 0},
/*  7 */ { 4, s_5_7, 6, 1, 0},
/*  8 */ { 4, s_5_8, 6, 1, 0},
/*  9 */ { 2, s_5_9, -1, 1, 0},
/* 10 */ { 4, s_5_10, 9, 1, 0},
/* 11 */ { 4, s_5_11, 9, 1, 0},
/* 12 */ { 2, s_5_12, -1, 1, 0},
/* 13 */ { 4, s_5_13, -1, 1, 0},
/* 14 */ { 4, s_5_14, -1, 1, 0},
/* 15 */ { 2, s_5_15, -1, 1, 0},
/* 16 */ { 4, s_5_16, 15, 1, 0},
/* 17 */ { 4, s_5_17, 15, 1, 0},
/* 18 */ { 2, s_5_18, -1, 1, 0},
/* 19 */ { 4, s_5_19, 18, 1, 0},
/* 20 */ { 4, s_5_20, 18, 1, 0},
/* 21 */ { 6, s_5_21, 18, 1, 0},
/* 22 */ { 8, s_5_22, 21, 1, 0},
/* 23 */ { 6, s_5_23, 18, 1, 0},
/* 24 */ { 2, s_5_24, -1, 1, 0},
/* 25 */ { 4, s_5_25, 24, 1, 0},
/* 26 */ { 6, s_5_26, 25, 1, 0},
/* 27 */ { 4, s_5_27, 24, 1, 0},
/* 28 */ { 4, s_5_28, 24, 1, 0},
/* 29 */ { 4, s_5_29, -1, 1, 0},
/* 30 */ { 6, s_5_30, 29, 1, 0},
/* 31 */ { 4, s_5_31, -1, 1, 0},
/* 32 */ { 4, s_5_32, -1, 1, 0},
/* 33 */ { 6, s_5_33, 32, 1, 0},
/* 34 */ { 4, s_5_34, -1, 1, 0},
/* 35 */ { 2, s_5_35, -1, 1, 0}
};

static const symbol s_6_0[6] = { 0xD0, 0xBE, 0xD1, 0x81, 0xD1, 0x82 };
static const symbol s_6_1[8] = { 0xD0, 0xBE, 0xD1, 0x81, 0xD1, 0x82, 0xD1, 0x8C };

static const struct among a_6[2] =
{
/*  0 */ { 6, s_6_0, -1, 1, 0},
/*  1 */ { 8, s_6_1, -1, 1, 0}
};

static const symbol s_7_0[6] = { 0xD0, 0xB5, 0xD0, 0xB9, 0xD1, 0x88 };
static const symbol s_7_1[2] = { 0xD1, 0x8C };
static const symbol s_7_2[8] = { 0xD0, 0xB5, 0xD0, 0xB9, 0xD1, 0x88, 0xD0, 0xB5 };
static const symbol s_7_3[2] = { 0xD0, 0xBD };

static const struct among a_7[4] =
{
/*  0 */ { 6, s_7_0, -1, 1, 0},
/*  1 */ { 2, s_7_1, -1, 3, 0},
/*  2 */ { 8, s_7_2, -1, 1, 0},
/*  3 */ { 2, s_7_3, -1, 2, 0}
};

static const unsigned char g_v[] = { 33, 65, 8, 232 };

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

static int r_mark_regions(struct SN_env * z) { /* forwardmode */
    z->I[0] = z->l; /* $pV = <integer expression>, line 61 */
    z->I[1] = z->l; /* $p2 = <integer expression>, line 62 */
    {   int c1 = z->c; /* do, line 63 */
        {    /* gopast */ /* grouping v, line 64 */
            int ret = out_grouping_U(z, g_v, 1072, 1103, 1);
            if (ret < 0) goto lab0;
            z->c += ret;
        }
        z->I[0] = z->c; /* setmark pV, line 64 */
        {    /* gopast */ /* non v, line 64 */
            int ret = in_grouping_U(z, g_v, 1072, 1103, 1);
            if (ret < 0) goto lab0;
            z->c += ret;
        }
        {    /* gopast */ /* grouping v, line 65 */
            int ret = out_grouping_U(z, g_v, 1072, 1103, 1);
            if (ret < 0) goto lab0;
            z->c += ret;
        }
        {    /* gopast */ /* non v, line 65 */
            int ret = in_grouping_U(z, g_v, 1072, 1103, 1);
            if (ret < 0) goto lab0;
            z->c += ret;
        }
        z->I[1] = z->c; /* setmark p2, line 65 */
    lab0:
        z->c = c1;
    }
    return 1;
}

static int r_R2(struct SN_env * z) { /* backwardmode */
    if (!(z->I[1] <= z->c)) return 0; /* $(<integer expression> <= <integer expression>), line 71 */
    return 1;
}

static int r_perfective_gerund(struct SN_env * z) { /* backwardmode */
    int among_var;
    z->ket = z->c; /* [, line 74 */
    among_var = find_among_b(z, a_0, 9); /* substring, line 74 */
    if (!(among_var)) return 0;
    z->bra = z->c; /* ], line 74 */
    switch (among_var) { /* among, line 74 */
        case 1:
            {   int m1 = z->l - z->c; (void)m1; /* or, line 78 */
                if (!(eq_s_b(z, 2, s_0))) goto lab1; /* literal, line 78 */
                goto lab0;
            lab1:
                z->c = z->l - m1;
                if (!(eq_s_b(z, 2, s_1))) return 0; /* literal, line 78 */
            }
        lab0:
            {   int ret = slice_del(z); /* delete, line 78 */
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {   int ret = slice_del(z); /* delete, line 85 */
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_adjective(struct SN_env * z) { /* backwardmode */
    z->ket = z->c; /* [, line 90 */
    if (!(find_among_b(z, a_1, 26))) return 0; /* substring, line 90 */
    z->bra = z->c; /* ], line 90 */
    {   int ret = slice_del(z); /* delete, line 99 */
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_adjectival(struct SN_env * z) { /* backwardmode */
    int among_var;
    {   int ret = r_adjective(z); /* call adjective, line 104 */
        if (ret <= 0) return ret;
    }
    {   int m1 = z->l - z->c; (void)m1; /* try, line 111 */
        z->ket = z->c; /* [, line 112 */
        among_var = find_among_b(z, a_2, 8); /* substring, line 112 */
        if (!(among_var)) { z->c = z->l - m1; goto lab0; }
        z->bra = z->c; /* ], line 112 */
        switch (among_var) { /* among, line 112 */
            case 1:
                {   int m2 = z->l - z->c; (void)m2; /* or, line 117 */
                    if (!(eq_s_b(z, 2, s_2))) goto lab2; /* literal, line 117 */
                    goto lab1;
                lab2:
                    z->c = z->l - m2;
                    if (!(eq_s_b(z, 2, s_3))) { z->c = z->l - m1; goto lab0; } /* literal, line 117 */
                }
            lab1:
                {   int ret = slice_del(z); /* delete, line 117 */
                    if (ret < 0) return ret;
                }
                break;
            case 2:
                {   int ret = slice_del(z); /* delete, line 124 */
                    if (ret < 0) return ret;
                }
                break;
        }
    lab0:
        ;
    }
    return 1;
}

static int r_reflexive(struct SN_env * z) { /* backwardmode */
    z->ket = z->c; /* [, line 131 */
    if (z->c - 3 <= z->lb || (z->p[z->c - 1] != 140 && z->p[z->c - 1] != 143)) return 0; /* substring, line 131 */
    if (!(find_among_b(z, a_3, 2))) return 0;
    z->bra = z->c; /* ], line 131 */
    {   int ret = slice_del(z); /* delete, line 134 */
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_verb(struct SN_env * z) { /* backwardmode */
    int among_var;
    z->ket = z->c; /* [, line 139 */
    among_var = find_among_b(z, a_4, 46); /* substring, line 139 */
    if (!(among_var)) return 0;
    z->bra = z->c; /* ], line 139 */
    switch (among_var) { /* among, line 139 */
        case 1:
            {   int m1 = z->l - z->c; (void)m1; /* or, line 145 */
                if (!(eq_s_b(z, 2, s_4))) goto lab1; /* literal, line 145 */
                goto lab0;
            lab1:
                z->c = z->l - m1;
                if (!(eq_s_b(z, 2, s_5))) return 0; /* literal, line 145 */
            }
        lab0:
            {   int ret = slice_del(z); /* delete, line 145 */
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {   int ret = slice_del(z); /* delete, line 153 */
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_noun(struct SN_env * z) { /* backwardmode */
    z->ket = z->c; /* [, line 162 */
    if (!(find_among_b(z, a_5, 36))) return 0; /* substring, line 162 */
    z->bra = z->c; /* ], line 162 */
    {   int ret = slice_del(z); /* delete, line 169 */
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_derivational(struct SN_env * z) { /* backwardmode */
    z->ket = z->c; /* [, line 178 */
    if (z->c - 5 <= z->lb || (z->p[z->c - 1] != 130 && z->p[z->c - 1] != 140)) return 0; /* substring, line 178 */
    if (!(find_among_b(z, a_6, 2))) return 0;
    z->bra = z->c; /* ], line 178 */
    {   int ret = r_R2(z); /* call R2, line 178 */
        if (ret <= 0) return ret;
    }
    {   int ret = slice_del(z); /* delete, line 181 */
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_tidy_up(struct SN_env * z) { /* backwardmode */
    int among_var;
    z->ket = z->c; /* [, line 186 */
    among_var = find_among_b(z, a_7, 4); /* substring, line 186 */
    if (!(among_var)) return 0;
    z->bra = z->c; /* ], line 186 */
    switch (among_var) { /* among, line 186 */
        case 1:
            {   int ret = slice_del(z); /* delete, line 190 */
                if (ret < 0) return ret;
            }
            z->ket = z->c; /* [, line 191 */
            if (!(eq_s_b(z, 2, s_6))) return 0; /* literal, line 191 */
            z->bra = z->c; /* ], line 191 */
            if (!(eq_s_b(z, 2, s_7))) return 0; /* literal, line 191 */
            {   int ret = slice_del(z); /* delete, line 191 */
                if (ret < 0) return ret;
            }
            break;
        case 2:
            if (!(eq_s_b(z, 2, s_8))) return 0; /* literal, line 194 */
            {   int ret = slice_del(z); /* delete, line 194 */
                if (ret < 0) return ret;
            }
            break;
        case 3:
            {   int ret = slice_del(z); /* delete, line 196 */
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

extern int russian_UTF_8_stem(struct SN_env * z) { /* forwardmode */
    {   int c1 = z->c; /* do, line 205 */
        while(1) { /* repeat, line 205 */
            int c2 = z->c;
            while(1) { /* goto, line 205 */
                int c3 = z->c;
                z->bra = z->c; /* [, line 205 */
                if (!(eq_s(z, 2, s_9))) goto lab2; /* literal, line 205 */
                z->ket = z->c; /* ], line 205 */
                z->c = c3;
                break;
            lab2:
                z->c = c3;
                {   int ret = skip_utf8(z->p, z->c, 0, z->l, 1);
                    if (ret < 0) goto lab1;
                    z->c = ret; /* goto, line 205 */
                }
            }
            {   int ret = slice_from_s(z, 2, s_10); /* <-, line 205 */
                if (ret < 0) return ret;
            }
            continue;
        lab1:
            z->c = c2;
            break;
        }
        z->c = c1;
    }
    /* do, line 207 */
    {   int ret = r_mark_regions(z); /* call mark_regions, line 207 */
        if (ret == 0) goto lab3;
        if (ret < 0) return ret;
    }
lab3:
    z->lb = z->c; z->c = z->l; /* backwards, line 208 */


    {   int mlimit4; /* setlimit, line 208 */
        if (z->c < z->I[0]) return 0;
        mlimit4 = z->lb; z->lb = z->I[0];
        {   int m5 = z->l - z->c; (void)m5; /* do, line 209 */
            {   int m6 = z->l - z->c; (void)m6; /* or, line 210 */
                {   int ret = r_perfective_gerund(z); /* call perfective_gerund, line 210 */
                    if (ret == 0) goto lab6;
                    if (ret < 0) return ret;
                }
                goto lab5;
            lab6:
                z->c = z->l - m6;
                {   int m7 = z->l - z->c; (void)m7; /* try, line 211 */
                    {   int ret = r_reflexive(z); /* call reflexive, line 211 */
                        if (ret == 0) { z->c = z->l - m7; goto lab7; }
                        if (ret < 0) return ret;
                    }
                lab7:
                    ;
                }
                {   int m8 = z->l - z->c; (void)m8; /* or, line 212 */
                    {   int ret = r_adjectival(z); /* call adjectival, line 212 */
                        if (ret == 0) goto lab9;
                        if (ret < 0) return ret;
                    }
                    goto lab8;
                lab9:
                    z->c = z->l - m8;
                    {   int ret = r_verb(z); /* call verb, line 212 */
                        if (ret == 0) goto lab10;
                        if (ret < 0) return ret;
                    }
                    goto lab8;
                lab10:
                    z->c = z->l - m8;
                    {   int ret = r_noun(z); /* call noun, line 212 */
                        if (ret == 0) goto lab4;
                        if (ret < 0) return ret;
                    }
                }
            lab8:
                ;
            }
        lab5:
        lab4:
            z->c = z->l - m5;
        }
        {   int m9 = z->l - z->c; (void)m9; /* try, line 215 */
            z->ket = z->c; /* [, line 215 */
            if (!(eq_s_b(z, 2, s_11))) { z->c = z->l - m9; goto lab11; } /* literal, line 215 */
            z->bra = z->c; /* ], line 215 */
            {   int ret = slice_del(z); /* delete, line 215 */
                if (ret < 0) return ret;
            }
        lab11:
            ;
        }
        {   int m10 = z->l - z->c; (void)m10; /* do, line 218 */
            {   int ret = r_derivational(z); /* call derivational, line 218 */
                if (ret == 0) goto lab12;
                if (ret < 0) return ret;
            }
        lab12:
            z->c = z->l - m10;
        }
        {   int m11 = z->l - z->c; (void)m11; /* do, line 219 */
            {   int ret = r_tidy_up(z); /* call tidy_up, line 219 */
                if (ret == 0) goto lab13;
                if (ret < 0) return ret;
            }
        lab13:
            z->c = z->l - m11;
        }
        z->lb = mlimit4;
    }
    z->c = z->lb;
    return 1;
}

extern struct SN_env * russian_UTF_8_create_env(void) { return SN_create_env(0, 2, 0); }

extern void russian_UTF_8_close_env(struct SN_env * z) { SN_close_env(z, 0); }

