/* Generated from tamil.sbl by Snowball 3.0.0 - https://snowballstem.org/ */

#include "stem_UTF_8_tamil.h"

#include <stddef.h>

#include "snowball_runtime.h"

struct SN_local {
    struct SN_env z;
    unsigned char b_found_vetrumai_urupu;
};

typedef struct SN_local SN_local;

#ifdef __cplusplus
extern "C" {
#endif
extern int tamil_UTF_8_stem(struct SN_env * z);
#ifdef __cplusplus
}
#endif

static int r_has_min_length(struct SN_env * z);
static int r_remove_common_word_endings(struct SN_env * z);
static int r_remove_tense_suffixes(struct SN_env * z);
static int r_remove_tense_suffix(struct SN_env * z);
static int r_fix_endings(struct SN_env * z);
static int r_fix_ending(struct SN_env * z);
static int r_fix_va_start(struct SN_env * z);
static int r_remove_vetrumai_urupukal(struct SN_env * z);
static int r_remove_um(struct SN_env * z);
static int r_remove_command_suffixes(struct SN_env * z);
static int r_remove_pronoun_prefixes(struct SN_env * z);
static int r_remove_question_prefixes(struct SN_env * z);
static int r_remove_question_suffixes(struct SN_env * z);
static int r_remove_plural_suffix(struct SN_env * z);

static const symbol s_0[] = { 0xE0, 0xAE, 0x93 };
static const symbol s_1[] = { 0xE0, 0xAE, 0x92 };
static const symbol s_2[] = { 0xE0, 0xAE, 0x89 };
static const symbol s_3[] = { 0xE0, 0xAE, 0x8A };
static const symbol s_4[] = { 0xE0, 0xAE, 0x8E };
static const symbol s_5[] = { 0xE0, 0xAF, 0x8D };
static const symbol s_6[] = { 0xE0, 0xAE, 0xB3, 0xE0, 0xAF, 0x8D };
static const symbol s_7[] = { 0xE0, 0xAE, 0xB2, 0xE0, 0xAF, 0x8D };
static const symbol s_8[] = { 0xE0, 0xAE, 0x9F, 0xE0, 0xAF, 0x81 };
static const symbol s_9[] = { 0xE0, 0xAF, 0x88 };
static const symbol s_10[] = { 0xE0, 0xAE, 0xAE, 0xE0, 0xAF, 0x8D };
static const symbol s_11[] = { 0xE0, 0xAF, 0x8D };
static const symbol s_12[] = { 0xE0, 0xAE, 0xAE, 0xE0, 0xAF, 0x8D };
static const symbol s_13[] = { 0xE0, 0xAF, 0x8D };
static const symbol s_14[] = { 0xE0, 0xAF, 0x8D };
static const symbol s_15[] = { 0xE0, 0xAF, 0x8D };
static const symbol s_16[] = { 0xE0, 0xAF, 0x8D };
static const symbol s_17[] = { 0xE0, 0xAF, 0x81, 0xE0, 0xAE, 0x99, 0xE0, 0xAF, 0x8D };
static const symbol s_18[] = { 0xE0, 0xAF, 0x8D };
static const symbol s_19[] = { 0xE0, 0xAE, 0xB2, 0xE0, 0xAF, 0x8D };
static const symbol s_20[] = { 0xE0, 0xAE, 0xB3, 0xE0, 0xAF, 0x8D };
static const symbol s_21[] = { 0xE0, 0xAF, 0x8D };
static const symbol s_22[] = { 0xE0, 0xAF, 0x81, 0xE0, 0xAE, 0xAE, 0xE0, 0xAF, 0x8D };
static const symbol s_23[] = { 0xE0, 0xAF, 0x8D };
static const symbol s_24[] = { 0xE0, 0xAF, 0x8D };
static const symbol s_25[] = { 0xE0, 0xAF, 0x8D };
static const symbol s_26[] = { 0xE0, 0xAF, 0x8D };
static const symbol s_27[] = { 0xE0, 0xAE, 0xAE };
static const symbol s_28[] = { 0xE0, 0xAF, 0x8D };
static const symbol s_29[] = { 0xE0, 0xAF, 0x8D };
static const symbol s_30[] = { 0xE0, 0xAF, 0x8D };
static const symbol s_31[] = { 0xE0, 0xAE, 0xBF };
static const symbol s_32[] = { 0xE0, 0xAF, 0x88 };
static const symbol s_33[] = { 0xE0, 0xAF, 0x8D };
static const symbol s_34[] = { 0xE0, 0xAF, 0x8D };
static const symbol s_35[] = { 0xE0, 0xAE, 0xBF, 0xE0, 0xAE, 0xA9, 0xE0, 0xAF, 0x8D };
static const symbol s_36[] = { 0xE0, 0xAF, 0x8D };
static const symbol s_37[] = { 0xE0, 0xAE, 0x9A };
static const symbol s_38[] = { 0xE0, 0xAF, 0x8D };
static const symbol s_39[] = { 0xE0, 0xAF, 0x8D };
static const symbol s_40[] = { 0xE0, 0xAF, 0x8D };

static const symbol s_0_0[6] = { 0xE0, 0xAE, 0xB5, 0xE0, 0xAF, 0x81 };
static const symbol s_0_1[6] = { 0xE0, 0xAE, 0xB5, 0xE0, 0xAF, 0x82 };
static const symbol s_0_2[6] = { 0xE0, 0xAE, 0xB5, 0xE0, 0xAF, 0x8A };
static const symbol s_0_3[6] = { 0xE0, 0xAE, 0xB5, 0xE0, 0xAF, 0x8B };
static const struct among a_0[4] = {
{ 6, s_0_0, 0, 3, 0},
{ 6, s_0_1, 0, 4, 0},
{ 6, s_0_2, 0, 2, 0},
{ 6, s_0_3, 0, 1, 0}
};

static const symbol s_1_0[3] = { 0xE0, 0xAE, 0x95 };
static const symbol s_1_1[3] = { 0xE0, 0xAE, 0x99 };
static const symbol s_1_2[3] = { 0xE0, 0xAE, 0x9A };
static const symbol s_1_3[3] = { 0xE0, 0xAE, 0x9E };
static const symbol s_1_4[3] = { 0xE0, 0xAE, 0xA4 };
static const symbol s_1_5[3] = { 0xE0, 0xAE, 0xA8 };
static const symbol s_1_6[3] = { 0xE0, 0xAE, 0xAA };
static const symbol s_1_7[3] = { 0xE0, 0xAE, 0xAE };
static const symbol s_1_8[3] = { 0xE0, 0xAE, 0xAF };
static const symbol s_1_9[3] = { 0xE0, 0xAE, 0xB5 };
static const struct among a_1[10] = {
{ 3, s_1_0, 0, -1, 0},
{ 3, s_1_1, 0, -1, 0},
{ 3, s_1_2, 0, -1, 0},
{ 3, s_1_3, 0, -1, 0},
{ 3, s_1_4, 0, -1, 0},
{ 3, s_1_5, 0, -1, 0},
{ 3, s_1_6, 0, -1, 0},
{ 3, s_1_7, 0, -1, 0},
{ 3, s_1_8, 0, -1, 0},
{ 3, s_1_9, 0, -1, 0}
};

static const symbol s_2_0[3] = { 0xE0, 0xAF, 0x80 };
static const symbol s_2_1[3] = { 0xE0, 0xAF, 0x88 };
static const symbol s_2_2[3] = { 0xE0, 0xAE, 0xBF };
static const struct among a_2[3] = {
{ 3, s_2_0, 0, -1, 0},
{ 3, s_2_1, 0, -1, 0},
{ 3, s_2_2, 0, -1, 0}
};

static const symbol s_3_0[3] = { 0xE0, 0xAF, 0x80 };
static const symbol s_3_1[3] = { 0xE0, 0xAF, 0x81 };
static const symbol s_3_2[3] = { 0xE0, 0xAF, 0x82 };
static const symbol s_3_3[3] = { 0xE0, 0xAF, 0x86 };
static const symbol s_3_4[3] = { 0xE0, 0xAF, 0x87 };
static const symbol s_3_5[3] = { 0xE0, 0xAF, 0x88 };
static const symbol s_3_6[3] = { 0xE0, 0xAE, 0xBE };
static const symbol s_3_7[3] = { 0xE0, 0xAE, 0xBF };
static const struct among a_3[8] = {
{ 3, s_3_0, 0, -1, 0},
{ 3, s_3_1, 0, -1, 0},
{ 3, s_3_2, 0, -1, 0},
{ 3, s_3_3, 0, -1, 0},
{ 3, s_3_4, 0, -1, 0},
{ 3, s_3_5, 0, -1, 0},
{ 3, s_3_6, 0, -1, 0},
{ 3, s_3_7, 0, -1, 0}
};

static const symbol s_4_1[3] = { 0xE0, 0xAF, 0x88 };
static const symbol s_4_2[3] = { 0xE0, 0xAF, 0x8D };
static const struct among a_4[3] = {
{ 0, 0, 0, 2, 0},
{ 3, s_4_1, -1, 1, 0},
{ 3, s_4_2, -2, 1, 0}
};

static const symbol s_5_0[6] = { 0xE0, 0xAE, 0xA9, 0xE0, 0xAF, 0x81 };
static const symbol s_5_1[9] = { 0xE0, 0xAF, 0x81, 0xE0, 0xAE, 0x95, 0xE0, 0xAF, 0x8D };
static const symbol s_5_2[15] = { 0xE0, 0xAF, 0x81, 0xE0, 0xAE, 0x95, 0xE0, 0xAF, 0x8D, 0xE0, 0xAE, 0x95, 0xE0, 0xAF, 0x8D };
static const symbol s_5_3[12] = { 0xE0, 0xAE, 0x9F, 0xE0, 0xAF, 0x8D, 0xE0, 0xAE, 0x95, 0xE0, 0xAF, 0x8D };
static const symbol s_5_4[12] = { 0xE0, 0xAE, 0xB1, 0xE0, 0xAF, 0x8D, 0xE0, 0xAE, 0x95, 0xE0, 0xAF, 0x8D };
static const symbol s_5_5[6] = { 0xE0, 0xAE, 0x99, 0xE0, 0xAF, 0x8D };
static const symbol s_5_6[12] = { 0xE0, 0xAE, 0x9F, 0xE0, 0xAF, 0x8D, 0xE0, 0xAE, 0x9F, 0xE0, 0xAF, 0x8D };
static const symbol s_5_7[12] = { 0xE0, 0xAE, 0xA4, 0xE0, 0xAF, 0x8D, 0xE0, 0xAE, 0xA4, 0xE0, 0xAF, 0x8D };
static const symbol s_5_8[12] = { 0xE0, 0xAE, 0xA8, 0xE0, 0xAF, 0x8D, 0xE0, 0xAE, 0xA4, 0xE0, 0xAF, 0x8D };
static const symbol s_5_9[6] = { 0xE0, 0xAE, 0xA8, 0xE0, 0xAF, 0x8D };
static const symbol s_5_10[12] = { 0xE0, 0xAE, 0x9F, 0xE0, 0xAF, 0x8D, 0xE0, 0xAE, 0xAA, 0xE0, 0xAF, 0x8D };
static const symbol s_5_11[6] = { 0xE0, 0xAE, 0xAF, 0xE0, 0xAF, 0x8D };
static const symbol s_5_12[12] = { 0xE0, 0xAE, 0xA9, 0xE0, 0xAF, 0x8D, 0xE0, 0xAE, 0xB1, 0xE0, 0xAF, 0x8D };
static const symbol s_5_13[6] = { 0xE0, 0xAE, 0xB5, 0xE0, 0xAF, 0x8D };
static const symbol s_5_14[9] = { 0xE0, 0xAE, 0xA8, 0xE0, 0xAF, 0x8D, 0xE0, 0xAE, 0xA4 };
static const symbol s_5_15[3] = { 0xE0, 0xAE, 0xAF };
static const symbol s_5_16[3] = { 0xE0, 0xAE, 0xB5 };
static const struct among a_5[17] = {
{ 6, s_5_0, 0, 8, 0},
{ 9, s_5_1, 0, 7, 0},
{ 15, s_5_2, 0, 7, 0},
{ 12, s_5_3, 0, 3, 0},
{ 12, s_5_4, 0, 4, 0},
{ 6, s_5_5, 0, 9, 0},
{ 12, s_5_6, 0, 5, 0},
{ 12, s_5_7, 0, 6, 0},
{ 12, s_5_8, 0, 1, 0},
{ 6, s_5_9, 0, 1, 0},
{ 12, s_5_10, 0, 3, 0},
{ 6, s_5_11, 0, 2, 0},
{ 12, s_5_12, 0, 4, 0},
{ 6, s_5_13, 0, 1, 0},
{ 9, s_5_14, 0, 1, 0},
{ 3, s_5_15, 0, 1, 0},
{ 3, s_5_16, 0, 1, 0}
};

static const symbol s_6_0[3] = { 0xE0, 0xAE, 0x95 };
static const symbol s_6_1[3] = { 0xE0, 0xAE, 0x9A };
static const symbol s_6_2[3] = { 0xE0, 0xAE, 0x9F };
static const symbol s_6_3[3] = { 0xE0, 0xAE, 0xA4 };
static const symbol s_6_4[3] = { 0xE0, 0xAE, 0xAA };
static const symbol s_6_5[3] = { 0xE0, 0xAE, 0xB1 };
static const struct among a_6[6] = {
{ 3, s_6_0, 0, -1, 0},
{ 3, s_6_1, 0, -1, 0},
{ 3, s_6_2, 0, -1, 0},
{ 3, s_6_3, 0, -1, 0},
{ 3, s_6_4, 0, -1, 0},
{ 3, s_6_5, 0, -1, 0}
};

static const symbol s_7_0[3] = { 0xE0, 0xAE, 0x95 };
static const symbol s_7_1[3] = { 0xE0, 0xAE, 0x9A };
static const symbol s_7_2[3] = { 0xE0, 0xAE, 0x9F };
static const symbol s_7_3[3] = { 0xE0, 0xAE, 0xA4 };
static const symbol s_7_4[3] = { 0xE0, 0xAE, 0xAA };
static const symbol s_7_5[3] = { 0xE0, 0xAE, 0xB1 };
static const struct among a_7[6] = {
{ 3, s_7_0, 0, -1, 0},
{ 3, s_7_1, 0, -1, 0},
{ 3, s_7_2, 0, -1, 0},
{ 3, s_7_3, 0, -1, 0},
{ 3, s_7_4, 0, -1, 0},
{ 3, s_7_5, 0, -1, 0}
};

static const symbol s_8_0[3] = { 0xE0, 0xAE, 0x9E };
static const symbol s_8_1[3] = { 0xE0, 0xAE, 0xA3 };
static const symbol s_8_2[3] = { 0xE0, 0xAE, 0xA8 };
static const symbol s_8_3[3] = { 0xE0, 0xAE, 0xA9 };
static const symbol s_8_4[3] = { 0xE0, 0xAE, 0xAE };
static const symbol s_8_5[3] = { 0xE0, 0xAE, 0xAF };
static const symbol s_8_6[3] = { 0xE0, 0xAE, 0xB0 };
static const symbol s_8_7[3] = { 0xE0, 0xAE, 0xB2 };
static const symbol s_8_8[3] = { 0xE0, 0xAE, 0xB3 };
static const symbol s_8_9[3] = { 0xE0, 0xAE, 0xB4 };
static const symbol s_8_10[3] = { 0xE0, 0xAE, 0xB5 };
static const struct among a_8[11] = {
{ 3, s_8_0, 0, -1, 0},
{ 3, s_8_1, 0, -1, 0},
{ 3, s_8_2, 0, -1, 0},
{ 3, s_8_3, 0, -1, 0},
{ 3, s_8_4, 0, -1, 0},
{ 3, s_8_5, 0, -1, 0},
{ 3, s_8_6, 0, -1, 0},
{ 3, s_8_7, 0, -1, 0},
{ 3, s_8_8, 0, -1, 0},
{ 3, s_8_9, 0, -1, 0},
{ 3, s_8_10, 0, -1, 0}
};

static const symbol s_9_0[3] = { 0xE0, 0xAF, 0x80 };
static const symbol s_9_1[3] = { 0xE0, 0xAF, 0x81 };
static const symbol s_9_2[3] = { 0xE0, 0xAF, 0x82 };
static const symbol s_9_3[3] = { 0xE0, 0xAF, 0x86 };
static const symbol s_9_4[3] = { 0xE0, 0xAF, 0x87 };
static const symbol s_9_5[3] = { 0xE0, 0xAF, 0x88 };
static const symbol s_9_6[3] = { 0xE0, 0xAF, 0x8D };
static const symbol s_9_7[3] = { 0xE0, 0xAE, 0xBE };
static const symbol s_9_8[3] = { 0xE0, 0xAE, 0xBF };
static const struct among a_9[9] = {
{ 3, s_9_0, 0, -1, 0},
{ 3, s_9_1, 0, -1, 0},
{ 3, s_9_2, 0, -1, 0},
{ 3, s_9_3, 0, -1, 0},
{ 3, s_9_4, 0, -1, 0},
{ 3, s_9_5, 0, -1, 0},
{ 3, s_9_6, 0, -1, 0},
{ 3, s_9_7, 0, -1, 0},
{ 3, s_9_8, 0, -1, 0}
};

static const symbol s_10_0[3] = { 0xE0, 0xAE, 0x85 };
static const symbol s_10_1[3] = { 0xE0, 0xAE, 0x87 };
static const symbol s_10_2[3] = { 0xE0, 0xAE, 0x89 };
static const struct among a_10[3] = {
{ 3, s_10_0, 0, -1, 0},
{ 3, s_10_1, 0, -1, 0},
{ 3, s_10_2, 0, -1, 0}
};

static const symbol s_11_0[3] = { 0xE0, 0xAE, 0x95 };
static const symbol s_11_1[3] = { 0xE0, 0xAE, 0x99 };
static const symbol s_11_2[3] = { 0xE0, 0xAE, 0x9A };
static const symbol s_11_3[3] = { 0xE0, 0xAE, 0x9E };
static const symbol s_11_4[3] = { 0xE0, 0xAE, 0xA4 };
static const symbol s_11_5[3] = { 0xE0, 0xAE, 0xA8 };
static const symbol s_11_6[3] = { 0xE0, 0xAE, 0xAA };
static const symbol s_11_7[3] = { 0xE0, 0xAE, 0xAE };
static const symbol s_11_8[3] = { 0xE0, 0xAE, 0xAF };
static const symbol s_11_9[3] = { 0xE0, 0xAE, 0xB5 };
static const struct among a_11[10] = {
{ 3, s_11_0, 0, -1, 0},
{ 3, s_11_1, 0, -1, 0},
{ 3, s_11_2, 0, -1, 0},
{ 3, s_11_3, 0, -1, 0},
{ 3, s_11_4, 0, -1, 0},
{ 3, s_11_5, 0, -1, 0},
{ 3, s_11_6, 0, -1, 0},
{ 3, s_11_7, 0, -1, 0},
{ 3, s_11_8, 0, -1, 0},
{ 3, s_11_9, 0, -1, 0}
};

static const symbol s_12_0[3] = { 0xE0, 0xAE, 0x95 };
static const symbol s_12_1[3] = { 0xE0, 0xAE, 0x9A };
static const symbol s_12_2[3] = { 0xE0, 0xAE, 0x9F };
static const symbol s_12_3[3] = { 0xE0, 0xAE, 0xA4 };
static const symbol s_12_4[3] = { 0xE0, 0xAE, 0xAA };
static const symbol s_12_5[3] = { 0xE0, 0xAE, 0xB1 };
static const struct among a_12[6] = {
{ 3, s_12_0, 0, -1, 0},
{ 3, s_12_1, 0, -1, 0},
{ 3, s_12_2, 0, -1, 0},
{ 3, s_12_3, 0, -1, 0},
{ 3, s_12_4, 0, -1, 0},
{ 3, s_12_5, 0, -1, 0}
};

static const symbol s_13_0[9] = { 0xE0, 0xAE, 0x95, 0xE0, 0xAE, 0xB3, 0xE0, 0xAF, 0x8D };
static const symbol s_13_1[18] = { 0xE0, 0xAF, 0x81, 0xE0, 0xAE, 0x99, 0xE0, 0xAF, 0x8D, 0xE0, 0xAE, 0x95, 0xE0, 0xAE, 0xB3, 0xE0, 0xAF, 0x8D };
static const symbol s_13_2[15] = { 0xE0, 0xAE, 0x9F, 0xE0, 0xAF, 0x8D, 0xE0, 0xAE, 0x95, 0xE0, 0xAE, 0xB3, 0xE0, 0xAF, 0x8D };
static const symbol s_13_3[15] = { 0xE0, 0xAE, 0xB1, 0xE0, 0xAF, 0x8D, 0xE0, 0xAE, 0x95, 0xE0, 0xAE, 0xB3, 0xE0, 0xAF, 0x8D };
static const struct among a_13[4] = {
{ 9, s_13_0, 0, 4, 0},
{ 18, s_13_1, -1, 1, 0},
{ 15, s_13_2, -2, 3, 0},
{ 15, s_13_3, -3, 2, 0}
};

static const symbol s_14_0[3] = { 0xE0, 0xAF, 0x87 };
static const symbol s_14_1[3] = { 0xE0, 0xAF, 0x8B };
static const symbol s_14_2[3] = { 0xE0, 0xAE, 0xBE };
static const struct among a_14[3] = {
{ 3, s_14_0, 0, -1, 0},
{ 3, s_14_1, 0, -1, 0},
{ 3, s_14_2, 0, -1, 0}
};

static const symbol s_15_0[6] = { 0xE0, 0xAE, 0xAA, 0xE0, 0xAE, 0xBF };
static const symbol s_15_1[6] = { 0xE0, 0xAE, 0xB5, 0xE0, 0xAE, 0xBF };
static const struct among a_15[2] = {
{ 6, s_15_0, 0, -1, 0},
{ 6, s_15_1, 0, -1, 0}
};

static const symbol s_16_0[3] = { 0xE0, 0xAF, 0x80 };
static const symbol s_16_1[3] = { 0xE0, 0xAF, 0x81 };
static const symbol s_16_2[3] = { 0xE0, 0xAF, 0x82 };
static const symbol s_16_3[3] = { 0xE0, 0xAF, 0x86 };
static const symbol s_16_4[3] = { 0xE0, 0xAF, 0x87 };
static const symbol s_16_5[3] = { 0xE0, 0xAF, 0x88 };
static const symbol s_16_6[3] = { 0xE0, 0xAE, 0xBE };
static const symbol s_16_7[3] = { 0xE0, 0xAE, 0xBF };
static const struct among a_16[8] = {
{ 3, s_16_0, 0, -1, 0},
{ 3, s_16_1, 0, -1, 0},
{ 3, s_16_2, 0, -1, 0},
{ 3, s_16_3, 0, -1, 0},
{ 3, s_16_4, 0, -1, 0},
{ 3, s_16_5, 0, -1, 0},
{ 3, s_16_6, 0, -1, 0},
{ 3, s_16_7, 0, -1, 0}
};

static const symbol s_17_0[15] = { 0xE0, 0xAE, 0xAA, 0xE0, 0xAE, 0x9F, 0xE0, 0xAF, 0x8D, 0xE0, 0xAE, 0x9F, 0xE0, 0xAF, 0x81 };
static const symbol s_17_1[18] = { 0xE0, 0xAE, 0xB5, 0xE0, 0xAE, 0xBF, 0xE0, 0xAE, 0x9F, 0xE0, 0xAF, 0x8D, 0xE0, 0xAE, 0x9F, 0xE0, 0xAF, 0x81 };
static const symbol s_17_2[9] = { 0xE0, 0xAE, 0xAA, 0xE0, 0xAE, 0x9F, 0xE0, 0xAF, 0x81 };
static const symbol s_17_3[12] = { 0xE0, 0xAE, 0xB5, 0xE0, 0xAE, 0xBF, 0xE0, 0xAE, 0x9F, 0xE0, 0xAF, 0x81 };
static const symbol s_17_4[18] = { 0xE0, 0xAE, 0xAA, 0xE0, 0xAE, 0x9F, 0xE0, 0xAF, 0x8D, 0xE0, 0xAE, 0x9F, 0xE0, 0xAE, 0xA4, 0xE0, 0xAF, 0x81 };
static const symbol s_17_5[15] = { 0xE0, 0xAF, 0x86, 0xE0, 0xAE, 0xA9, 0xE0, 0xAF, 0x8D, 0xE0, 0xAE, 0xB1, 0xE0, 0xAF, 0x81 };
static const symbol s_17_6[9] = { 0xE0, 0xAF, 0x81, 0xE0, 0xAE, 0x9F, 0xE0, 0xAF, 0x88 };
static const symbol s_17_7[15] = { 0xE0, 0xAE, 0xBF, 0xE0, 0xAE, 0xB2, 0xE0, 0xAF, 0x8D, 0xE0, 0xAE, 0xB2, 0xE0, 0xAF, 0x88 };
static const symbol s_17_8[12] = { 0xE0, 0xAF, 0x81, 0xE0, 0xAE, 0x9F, 0xE0, 0xAE, 0xA9, 0xE0, 0xAF, 0x8D };
static const symbol s_17_9[15] = { 0xE0, 0xAF, 0x86, 0xE0, 0xAE, 0xA9, 0xE0, 0xAF, 0x81, 0xE0, 0xAE, 0xAE, 0xE0, 0xAF, 0x8D };
static const symbol s_17_10[12] = { 0xE0, 0xAE, 0xBF, 0xE0, 0xAE, 0x9F, 0xE0, 0xAE, 0xAE, 0xE0, 0xAF, 0x8D };
static const symbol s_17_11[21] = { 0xE0, 0xAF, 0x86, 0xE0, 0xAE, 0xB2, 0xE0, 0xAF, 0x8D, 0xE0, 0xAE, 0xB2, 0xE0, 0xAE, 0xBE, 0xE0, 0xAE, 0xAE, 0xE0, 0xAF, 0x8D };
static const symbol s_17_12[12] = { 0xE0, 0xAE, 0xAA, 0xE0, 0xAE, 0x9F, 0xE0, 0xAF, 0x8D, 0xE0, 0xAE, 0x9F };
static const symbol s_17_13[15] = { 0xE0, 0xAE, 0xAA, 0xE0, 0xAE, 0x9F, 0xE0, 0xAF, 0x8D, 0xE0, 0xAE, 0x9F, 0xE0, 0xAE, 0xA3 };
static const symbol s_17_14[6] = { 0xE0, 0xAF, 0x86, 0xE0, 0xAE, 0xA9 };
static const symbol s_17_15[9] = { 0xE0, 0xAE, 0xA4, 0xE0, 0xAE, 0xBE, 0xE0, 0xAE, 0xA9 };
static const symbol s_17_16[18] = { 0xE0, 0xAE, 0xAA, 0xE0, 0xAE, 0x9F, 0xE0, 0xAE, 0xBF, 0xE0, 0xAE, 0xA4, 0xE0, 0xAE, 0xBE, 0xE0, 0xAE, 0xA9 };
static const symbol s_17_17[12] = { 0xE0, 0xAF, 0x81, 0xE0, 0xAE, 0x9F, 0xE0, 0xAF, 0x88, 0xE0, 0xAE, 0xAF };
static const symbol s_17_18[12] = { 0xE0, 0xAE, 0xBE, 0xE0, 0xAE, 0x95, 0xE0, 0xAE, 0xBF, 0xE0, 0xAE, 0xAF };
static const symbol s_17_19[15] = { 0xE0, 0xAE, 0x95, 0xE0, 0xAF, 0x81, 0xE0, 0xAE, 0xB0, 0xE0, 0xAE, 0xBF, 0xE0, 0xAE, 0xAF };
static const symbol s_17_20[9] = { 0xE0, 0xAE, 0xB2, 0xE0, 0xAF, 0x8D, 0xE0, 0xAE, 0xB2 };
static const symbol s_17_21[12] = { 0xE0, 0xAF, 0x81, 0xE0, 0xAE, 0xB3, 0xE0, 0xAF, 0x8D, 0xE0, 0xAE, 0xB3 };
static const symbol s_17_22[9] = { 0xE0, 0xAE, 0xBE, 0xE0, 0xAE, 0x95, 0xE0, 0xAE, 0xBF };
static const symbol s_17_23[9] = { 0xE0, 0xAE, 0xAA, 0xE0, 0xAE, 0x9F, 0xE0, 0xAE, 0xBF };
static const symbol s_17_24[15] = { 0xE0, 0xAE, 0xBF, 0xE0, 0xAE, 0xA9, 0xE0, 0xAF, 0x8D, 0xE0, 0xAE, 0xB1, 0xE0, 0xAE, 0xBF };
static const symbol s_17_25[15] = { 0xE0, 0xAE, 0xAA, 0xE0, 0xAE, 0xB1, 0xE0, 0xAF, 0x8D, 0xE0, 0xAE, 0xB1, 0xE0, 0xAE, 0xBF };
static const struct among a_17[26] = {
{ 15, s_17_0, 0, 3, 0},
{ 18, s_17_1, 0, 3, 0},
{ 9, s_17_2, 0, 3, 0},
{ 12, s_17_3, 0, 3, 0},
{ 18, s_17_4, 0, 3, 0},
{ 15, s_17_5, 0, 1, 0},
{ 9, s_17_6, 0, 1, 0},
{ 15, s_17_7, 0, 1, 0},
{ 12, s_17_8, 0, 1, 0},
{ 15, s_17_9, 0, 1, 0},
{ 12, s_17_10, 0, 1, 0},
{ 21, s_17_11, 0, 3, 0},
{ 12, s_17_12, 0, 3, 0},
{ 15, s_17_13, 0, 3, 0},
{ 6, s_17_14, 0, 1, 0},
{ 9, s_17_15, 0, 3, 0},
{ 18, s_17_16, -1, 3, 0},
{ 12, s_17_17, 0, 1, 0},
{ 12, s_17_18, 0, 1, 0},
{ 15, s_17_19, 0, 3, 0},
{ 9, s_17_20, 0, 2, 0},
{ 12, s_17_21, 0, 1, 0},
{ 9, s_17_22, 0, 1, 0},
{ 9, s_17_23, 0, 3, 0},
{ 15, s_17_24, 0, 1, 0},
{ 15, s_17_25, 0, 3, 0}
};

static const symbol s_18_0[3] = { 0xE0, 0xAF, 0x80 };
static const symbol s_18_1[3] = { 0xE0, 0xAF, 0x81 };
static const symbol s_18_2[3] = { 0xE0, 0xAF, 0x82 };
static const symbol s_18_3[3] = { 0xE0, 0xAF, 0x86 };
static const symbol s_18_4[3] = { 0xE0, 0xAF, 0x87 };
static const symbol s_18_5[3] = { 0xE0, 0xAF, 0x88 };
static const symbol s_18_6[3] = { 0xE0, 0xAE, 0xBE };
static const symbol s_18_7[3] = { 0xE0, 0xAE, 0xBF };
static const struct among a_18[8] = {
{ 3, s_18_0, 0, -1, 0},
{ 3, s_18_1, 0, -1, 0},
{ 3, s_18_2, 0, -1, 0},
{ 3, s_18_3, 0, -1, 0},
{ 3, s_18_4, 0, -1, 0},
{ 3, s_18_5, 0, -1, 0},
{ 3, s_18_6, 0, -1, 0},
{ 3, s_18_7, 0, -1, 0}
};

static const symbol s_19_0[3] = { 0xE0, 0xAF, 0x80 };
static const symbol s_19_1[3] = { 0xE0, 0xAF, 0x81 };
static const symbol s_19_2[3] = { 0xE0, 0xAF, 0x82 };
static const symbol s_19_3[3] = { 0xE0, 0xAF, 0x86 };
static const symbol s_19_4[3] = { 0xE0, 0xAF, 0x87 };
static const symbol s_19_5[3] = { 0xE0, 0xAF, 0x88 };
static const symbol s_19_6[3] = { 0xE0, 0xAE, 0xBE };
static const symbol s_19_7[3] = { 0xE0, 0xAE, 0xBF };
static const struct among a_19[8] = {
{ 3, s_19_0, 0, -1, 0},
{ 3, s_19_1, 0, -1, 0},
{ 3, s_19_2, 0, -1, 0},
{ 3, s_19_3, 0, -1, 0},
{ 3, s_19_4, 0, -1, 0},
{ 3, s_19_5, 0, -1, 0},
{ 3, s_19_6, 0, -1, 0},
{ 3, s_19_7, 0, -1, 0}
};

static const symbol s_20_0[3] = { 0xE0, 0xAF, 0x80 };
static const symbol s_20_1[9] = { 0xE0, 0xAF, 0x8A, 0xE0, 0xAE, 0x9F, 0xE0, 0xAF, 0x81 };
static const symbol s_20_2[9] = { 0xE0, 0xAF, 0x8B, 0xE0, 0xAE, 0x9F, 0xE0, 0xAF, 0x81 };
static const symbol s_20_3[6] = { 0xE0, 0xAE, 0xA4, 0xE0, 0xAF, 0x81 };
static const symbol s_20_4[21] = { 0xE0, 0xAE, 0xBF, 0xE0, 0xAE, 0xB0, 0xE0, 0xAF, 0x81, 0xE0, 0xAE, 0xA8, 0xE0, 0xAF, 0x8D, 0xE0, 0xAE, 0xA4, 0xE0, 0xAF, 0x81 };
static const symbol s_20_5[15] = { 0xE0, 0xAE, 0xBF, 0xE0, 0xAE, 0xA9, 0xE0, 0xAF, 0x8D, 0xE0, 0xAE, 0xB1, 0xE0, 0xAF, 0x81 };
static const symbol s_20_6[9] = { 0xE0, 0xAF, 0x81, 0xE0, 0xAE, 0x9F, 0xE0, 0xAF, 0x88 };
static const symbol s_20_7[6] = { 0xE0, 0xAE, 0xA9, 0xE0, 0xAF, 0x88 };
static const symbol s_20_8[9] = { 0xE0, 0xAE, 0x95, 0xE0, 0xAE, 0xA3, 0xE0, 0xAF, 0x8D };
static const symbol s_20_9[12] = { 0xE0, 0xAE, 0xAE, 0xE0, 0xAF, 0x81, 0xE0, 0xAE, 0xA9, 0xE0, 0xAF, 0x8D };
static const symbol s_20_10[9] = { 0xE0, 0xAE, 0xBF, 0xE0, 0xAE, 0xA9, 0xE0, 0xAF, 0x8D };
static const symbol s_20_11[12] = { 0xE0, 0xAE, 0xBF, 0xE0, 0xAE, 0x9F, 0xE0, 0xAE, 0xAE, 0xE0, 0xAF, 0x8D };
static const symbol s_20_12[12] = { 0xE0, 0xAE, 0xAE, 0xE0, 0xAF, 0x87, 0xE0, 0xAE, 0xB1, 0xE0, 0xAF, 0x8D };
static const symbol s_20_13[9] = { 0xE0, 0xAE, 0xBF, 0xE0, 0xAE, 0xB1, 0xE0, 0xAF, 0x8D };
static const symbol s_20_14[6] = { 0xE0, 0xAE, 0xB2, 0xE0, 0xAF, 0x8D };
static const symbol s_20_15[12] = { 0xE0, 0xAE, 0xAE, 0xE0, 0xAF, 0x87, 0xE0, 0xAE, 0xB2, 0xE0, 0xAF, 0x8D };
static const symbol s_20_16[12] = { 0xE0, 0xAE, 0xBE, 0xE0, 0xAE, 0xAE, 0xE0, 0xAE, 0xB2, 0xE0, 0xAF, 0x8D };
static const symbol s_20_17[9] = { 0xE0, 0xAE, 0xBE, 0xE0, 0xAE, 0xB2, 0xE0, 0xAF, 0x8D };
static const symbol s_20_18[9] = { 0xE0, 0xAE, 0xBF, 0xE0, 0xAE, 0xB2, 0xE0, 0xAF, 0x8D };
static const symbol s_20_19[9] = { 0xE0, 0xAF, 0x81, 0xE0, 0xAE, 0xB3, 0xE0, 0xAF, 0x8D };
static const symbol s_20_20[12] = { 0xE0, 0xAE, 0x95, 0xE0, 0xAF, 0x80, 0xE0, 0xAE, 0xB4, 0xE0, 0xAF, 0x8D };
static const symbol s_20_21[9] = { 0xE0, 0xAE, 0xB5, 0xE0, 0xAE, 0xBF, 0xE0, 0xAE, 0x9F };
static const struct among a_20[22] = {
{ 3, s_20_0, 0, 7, 0},
{ 9, s_20_1, 0, 2, 0},
{ 9, s_20_2, 0, 2, 0},
{ 6, s_20_3, 0, 6, 0},
{ 21, s_20_4, -1, 2, 0},
{ 15, s_20_5, 0, 2, 0},
{ 9, s_20_6, 0, 2, 0},
{ 6, s_20_7, 0, 1, 0},
{ 9, s_20_8, 0, 1, 0},
{ 12, s_20_9, 0, 1, 0},
{ 9, s_20_10, 0, 3, 0},
{ 12, s_20_11, 0, 4, 0},
{ 12, s_20_12, 0, 1, 0},
{ 9, s_20_13, 0, 2, 0},
{ 6, s_20_14, 0, 5, 0},
{ 12, s_20_15, -1, 1, 0},
{ 12, s_20_16, -2, 2, 0},
{ 9, s_20_17, -3, 2, 0},
{ 9, s_20_18, -4, 2, 0},
{ 9, s_20_19, 0, 2, 0},
{ 12, s_20_20, 0, 1, 0},
{ 9, s_20_21, 0, 2, 0}
};

static const symbol s_21_0[3] = { 0xE0, 0xAE, 0x95 };
static const symbol s_21_1[3] = { 0xE0, 0xAE, 0x9A };
static const symbol s_21_2[3] = { 0xE0, 0xAE, 0x9F };
static const symbol s_21_3[3] = { 0xE0, 0xAE, 0xA4 };
static const symbol s_21_4[3] = { 0xE0, 0xAE, 0xAA };
static const symbol s_21_5[3] = { 0xE0, 0xAE, 0xB1 };
static const struct among a_21[6] = {
{ 3, s_21_0, 0, -1, 0},
{ 3, s_21_1, 0, -1, 0},
{ 3, s_21_2, 0, -1, 0},
{ 3, s_21_3, 0, -1, 0},
{ 3, s_21_4, 0, -1, 0},
{ 3, s_21_5, 0, -1, 0}
};

static const symbol s_22_0[3] = { 0xE0, 0xAE, 0x95 };
static const symbol s_22_1[3] = { 0xE0, 0xAE, 0x9A };
static const symbol s_22_2[3] = { 0xE0, 0xAE, 0x9F };
static const symbol s_22_3[3] = { 0xE0, 0xAE, 0xA4 };
static const symbol s_22_4[3] = { 0xE0, 0xAE, 0xAA };
static const symbol s_22_5[3] = { 0xE0, 0xAE, 0xB1 };
static const struct among a_22[6] = {
{ 3, s_22_0, 0, -1, 0},
{ 3, s_22_1, 0, -1, 0},
{ 3, s_22_2, 0, -1, 0},
{ 3, s_22_3, 0, -1, 0},
{ 3, s_22_4, 0, -1, 0},
{ 3, s_22_5, 0, -1, 0}
};

static const symbol s_23_0[3] = { 0xE0, 0xAE, 0x85 };
static const symbol s_23_1[3] = { 0xE0, 0xAE, 0x86 };
static const symbol s_23_2[3] = { 0xE0, 0xAE, 0x87 };
static const symbol s_23_3[3] = { 0xE0, 0xAE, 0x88 };
static const symbol s_23_4[3] = { 0xE0, 0xAE, 0x89 };
static const symbol s_23_5[3] = { 0xE0, 0xAE, 0x8A };
static const symbol s_23_6[3] = { 0xE0, 0xAE, 0x8E };
static const symbol s_23_7[3] = { 0xE0, 0xAE, 0x8F };
static const symbol s_23_8[3] = { 0xE0, 0xAE, 0x90 };
static const symbol s_23_9[3] = { 0xE0, 0xAE, 0x92 };
static const symbol s_23_10[3] = { 0xE0, 0xAE, 0x93 };
static const symbol s_23_11[3] = { 0xE0, 0xAE, 0x94 };
static const struct among a_23[12] = {
{ 3, s_23_0, 0, -1, 0},
{ 3, s_23_1, 0, -1, 0},
{ 3, s_23_2, 0, -1, 0},
{ 3, s_23_3, 0, -1, 0},
{ 3, s_23_4, 0, -1, 0},
{ 3, s_23_5, 0, -1, 0},
{ 3, s_23_6, 0, -1, 0},
{ 3, s_23_7, 0, -1, 0},
{ 3, s_23_8, 0, -1, 0},
{ 3, s_23_9, 0, -1, 0},
{ 3, s_23_10, 0, -1, 0},
{ 3, s_23_11, 0, -1, 0}
};

static const symbol s_24_0[3] = { 0xE0, 0xAF, 0x80 };
static const symbol s_24_1[3] = { 0xE0, 0xAF, 0x81 };
static const symbol s_24_2[3] = { 0xE0, 0xAF, 0x82 };
static const symbol s_24_3[3] = { 0xE0, 0xAF, 0x86 };
static const symbol s_24_4[3] = { 0xE0, 0xAF, 0x87 };
static const symbol s_24_5[3] = { 0xE0, 0xAF, 0x88 };
static const symbol s_24_6[3] = { 0xE0, 0xAE, 0xBE };
static const symbol s_24_7[3] = { 0xE0, 0xAE, 0xBF };
static const struct among a_24[8] = {
{ 3, s_24_0, 0, -1, 0},
{ 3, s_24_1, 0, -1, 0},
{ 3, s_24_2, 0, -1, 0},
{ 3, s_24_3, 0, -1, 0},
{ 3, s_24_4, 0, -1, 0},
{ 3, s_24_5, 0, -1, 0},
{ 3, s_24_6, 0, -1, 0},
{ 3, s_24_7, 0, -1, 0}
};

static const symbol s_25_0[6] = { 0xE0, 0xAE, 0x95, 0xE0, 0xAF, 0x81 };
static const symbol s_25_1[9] = { 0xE0, 0xAE, 0xAA, 0xE0, 0xAE, 0x9F, 0xE0, 0xAF, 0x81 };
static const symbol s_25_2[6] = { 0xE0, 0xAE, 0xA4, 0xE0, 0xAF, 0x81 };
static const symbol s_25_3[15] = { 0xE0, 0xAE, 0xBF, 0xE0, 0xAE, 0xB1, 0xE0, 0xAF, 0x8D, 0xE0, 0xAE, 0xB1, 0xE0, 0xAF, 0x81 };
static const symbol s_25_4[6] = { 0xE0, 0xAE, 0xA9, 0xE0, 0xAF, 0x88 };
static const symbol s_25_5[6] = { 0xE0, 0xAE, 0xB5, 0xE0, 0xAF, 0x88 };
static const symbol s_25_6[12] = { 0xE0, 0xAE, 0xA9, 0xE0, 0xAF, 0x86, 0xE0, 0xAE, 0xA9, 0xE0, 0xAF, 0x8D };
static const symbol s_25_7[9] = { 0xE0, 0xAF, 0x87, 0xE0, 0xAE, 0xA9, 0xE0, 0xAF, 0x8D };
static const symbol s_25_8[9] = { 0xE0, 0xAE, 0xA9, 0xE0, 0xAE, 0xA9, 0xE0, 0xAF, 0x8D };
static const symbol s_25_9[9] = { 0xE0, 0xAE, 0xAA, 0xE0, 0xAE, 0xA9, 0xE0, 0xAF, 0x8D };
static const symbol s_25_10[9] = { 0xE0, 0xAE, 0xB5, 0xE0, 0xAE, 0xA9, 0xE0, 0xAF, 0x8D };
static const symbol s_25_11[9] = { 0xE0, 0xAE, 0xBE, 0xE0, 0xAE, 0xA9, 0xE0, 0xAF, 0x8D };
static const symbol s_25_12[12] = { 0xE0, 0xAE, 0xA9, 0xE0, 0xAE, 0xBE, 0xE0, 0xAE, 0xA9, 0xE0, 0xAF, 0x8D };
static const symbol s_25_13[12] = { 0xE0, 0xAE, 0xAE, 0xE0, 0xAE, 0xBF, 0xE0, 0xAE, 0xA9, 0xE0, 0xAF, 0x8D };
static const symbol s_25_14[12] = { 0xE0, 0xAE, 0x95, 0xE0, 0xAF, 0x81, 0xE0, 0xAE, 0xAE, 0xE0, 0xAF, 0x8D };
static const symbol s_25_15[12] = { 0xE0, 0xAE, 0x9F, 0xE0, 0xAF, 0x81, 0xE0, 0xAE, 0xAE, 0xE0, 0xAF, 0x8D };
static const symbol s_25_16[12] = { 0xE0, 0xAE, 0xA4, 0xE0, 0xAF, 0x81, 0xE0, 0xAE, 0xAE, 0xE0, 0xAF, 0x8D };
static const symbol s_25_17[12] = { 0xE0, 0xAE, 0xB1, 0xE0, 0xAF, 0x81, 0xE0, 0xAE, 0xAE, 0xE0, 0xAF, 0x8D };
static const symbol s_25_18[9] = { 0xE0, 0xAF, 0x86, 0xE0, 0xAE, 0xAE, 0xE0, 0xAF, 0x8D };
static const symbol s_25_19[9] = { 0xE0, 0xAF, 0x87, 0xE0, 0xAE, 0xAE, 0xE0, 0xAF, 0x8D };
static const symbol s_25_20[9] = { 0xE0, 0xAF, 0x8B, 0xE0, 0xAE, 0xAE, 0xE0, 0xAF, 0x8D };
static const symbol s_25_21[9] = { 0xE0, 0xAE, 0xA9, 0xE0, 0xAE, 0xAE, 0xE0, 0xAF, 0x8D };
static const symbol s_25_22[9] = { 0xE0, 0xAE, 0xAA, 0xE0, 0xAE, 0xAE, 0xE0, 0xAF, 0x8D };
static const symbol s_25_23[9] = { 0xE0, 0xAE, 0xBE, 0xE0, 0xAE, 0xAE, 0xE0, 0xAF, 0x8D };
static const symbol s_25_24[9] = { 0xE0, 0xAE, 0xBE, 0xE0, 0xAE, 0xAF, 0xE0, 0xAF, 0x8D };
static const symbol s_25_25[9] = { 0xE0, 0xAF, 0x80, 0xE0, 0xAE, 0xB0, 0xE0, 0xAF, 0x8D };
static const symbol s_25_26[9] = { 0xE0, 0xAE, 0xA9, 0xE0, 0xAE, 0xB0, 0xE0, 0xAF, 0x8D };
static const symbol s_25_27[9] = { 0xE0, 0xAE, 0xAA, 0xE0, 0xAE, 0xB0, 0xE0, 0xAF, 0x8D };
static const symbol s_25_28[12] = { 0xE0, 0xAF, 0x80, 0xE0, 0xAE, 0xAF, 0xE0, 0xAE, 0xB0, 0xE0, 0xAF, 0x8D };
static const symbol s_25_29[9] = { 0xE0, 0xAE, 0xB5, 0xE0, 0xAE, 0xB0, 0xE0, 0xAF, 0x8D };
static const symbol s_25_30[9] = { 0xE0, 0xAE, 0xBE, 0xE0, 0xAE, 0xB0, 0xE0, 0xAF, 0x8D };
static const symbol s_25_31[12] = { 0xE0, 0xAE, 0xA9, 0xE0, 0xAE, 0xBE, 0xE0, 0xAE, 0xB0, 0xE0, 0xAF, 0x8D };
static const symbol s_25_32[12] = { 0xE0, 0xAE, 0xAE, 0xE0, 0xAE, 0xBE, 0xE0, 0xAE, 0xB0, 0xE0, 0xAF, 0x8D };
static const symbol s_25_33[24] = { 0xE0, 0xAE, 0x95, 0xE0, 0xAF, 0x8A, 0xE0, 0xAE, 0xA3, 0xE0, 0xAF, 0x8D, 0xE0, 0xAE, 0x9F, 0xE0, 0xAE, 0xBF, 0xE0, 0xAE, 0xB0, 0xE0, 0xAF, 0x8D };
static const symbol s_25_34[12] = { 0xE0, 0xAE, 0xA9, 0xE0, 0xAE, 0xBF, 0xE0, 0xAE, 0xB0, 0xE0, 0xAF, 0x8D };
static const symbol s_25_35[9] = { 0xE0, 0xAE, 0xA9, 0xE0, 0xAE, 0xB3, 0xE0, 0xAF, 0x8D };
static const symbol s_25_36[9] = { 0xE0, 0xAE, 0xAA, 0xE0, 0xAE, 0xB3, 0xE0, 0xAF, 0x8D };
static const symbol s_25_37[9] = { 0xE0, 0xAE, 0xB5, 0xE0, 0xAE, 0xB3, 0xE0, 0xAF, 0x8D };
static const symbol s_25_38[9] = { 0xE0, 0xAE, 0xBE, 0xE0, 0xAE, 0xB3, 0xE0, 0xAF, 0x8D };
static const symbol s_25_39[12] = { 0xE0, 0xAE, 0xA9, 0xE0, 0xAE, 0xBE, 0xE0, 0xAE, 0xB3, 0xE0, 0xAF, 0x8D };
static const symbol s_25_40[3] = { 0xE0, 0xAE, 0x95 };
static const symbol s_25_41[3] = { 0xE0, 0xAE, 0xA4 };
static const symbol s_25_42[3] = { 0xE0, 0xAE, 0xA9 };
static const symbol s_25_43[3] = { 0xE0, 0xAE, 0xAA };
static const symbol s_25_44[3] = { 0xE0, 0xAE, 0xAF };
static const symbol s_25_45[3] = { 0xE0, 0xAE, 0xBE };
static const struct among a_25[46] = {
{ 6, s_25_0, 0, 6, 0},
{ 9, s_25_1, 0, 1, 0},
{ 6, s_25_2, 0, 3, 0},
{ 15, s_25_3, 0, 1, 0},
{ 6, s_25_4, 0, 1, 0},
{ 6, s_25_5, 0, 1, 0},
{ 12, s_25_6, 0, 1, 0},
{ 9, s_25_7, 0, 5, 0},
{ 9, s_25_8, 0, 1, 0},
{ 9, s_25_9, 0, 1, 0},
{ 9, s_25_10, 0, 2, 0},
{ 9, s_25_11, 0, 4, 0},
{ 12, s_25_12, -1, 1, 0},
{ 12, s_25_13, 0, 1, 0},
{ 12, s_25_14, 0, 1, 0},
{ 12, s_25_15, 0, 5, 0},
{ 12, s_25_16, 0, 1, 0},
{ 12, s_25_17, 0, 1, 0},
{ 9, s_25_18, 0, 5, 0},
{ 9, s_25_19, 0, 5, 0},
{ 9, s_25_20, 0, 5, 0},
{ 9, s_25_21, 0, 1, 0},
{ 9, s_25_22, 0, 1, 0},
{ 9, s_25_23, 0, 5, 0},
{ 9, s_25_24, 0, 5, 0},
{ 9, s_25_25, 0, 5, 0},
{ 9, s_25_26, 0, 1, 0},
{ 9, s_25_27, 0, 1, 0},
{ 12, s_25_28, 0, 5, 0},
{ 9, s_25_29, 0, 1, 0},
{ 9, s_25_30, 0, 5, 0},
{ 12, s_25_31, -1, 1, 0},
{ 12, s_25_32, -2, 1, 0},
{ 24, s_25_33, 0, 1, 0},
{ 12, s_25_34, 0, 5, 0},
{ 9, s_25_35, 0, 1, 0},
{ 9, s_25_36, 0, 1, 0},
{ 9, s_25_37, 0, 1, 0},
{ 9, s_25_38, 0, 5, 0},
{ 12, s_25_39, -1, 1, 0},
{ 3, s_25_40, 0, 1, 0},
{ 3, s_25_41, 0, 1, 0},
{ 3, s_25_42, 0, 1, 0},
{ 3, s_25_43, 0, 1, 0},
{ 3, s_25_44, 0, 1, 0},
{ 3, s_25_45, 0, 5, 0}
};

static const symbol s_26_0[18] = { 0xE0, 0xAE, 0x95, 0xE0, 0xAE, 0xBF, 0xE0, 0xAE, 0xA9, 0xE0, 0xAF, 0x8D, 0xE0, 0xAE, 0xB1, 0xE0, 0xAF, 0x8D };
static const symbol s_26_1[21] = { 0xE0, 0xAE, 0xBE, 0xE0, 0xAE, 0xA8, 0xE0, 0xAE, 0xBF, 0xE0, 0xAE, 0xA9, 0xE0, 0xAF, 0x8D, 0xE0, 0xAE, 0xB1, 0xE0, 0xAF, 0x8D };
static const symbol s_26_2[12] = { 0xE0, 0xAE, 0x95, 0xE0, 0xAE, 0xBF, 0xE0, 0xAE, 0xB1, 0xE0, 0xAF, 0x8D };
static const symbol s_26_3[15] = { 0xE0, 0xAE, 0x95, 0xE0, 0xAE, 0xBF, 0xE0, 0xAE, 0xA9, 0xE0, 0xAF, 0x8D, 0xE0, 0xAE, 0xB1 };
static const symbol s_26_4[18] = { 0xE0, 0xAE, 0xBE, 0xE0, 0xAE, 0xA8, 0xE0, 0xAE, 0xBF, 0xE0, 0xAE, 0xA9, 0xE0, 0xAF, 0x8D, 0xE0, 0xAE, 0xB1 };
static const symbol s_26_5[9] = { 0xE0, 0xAE, 0x95, 0xE0, 0xAE, 0xBF, 0xE0, 0xAE, 0xB1 };
static const struct among a_26[6] = {
{ 18, s_26_0, 0, -1, 0},
{ 21, s_26_1, 0, -1, 0},
{ 12, s_26_2, 0, -1, 0},
{ 15, s_26_3, 0, -1, 0},
{ 18, s_26_4, 0, -1, 0},
{ 9, s_26_5, 0, -1, 0}
};

static int r_has_min_length(struct SN_env * z) {
    return len_utf8(z->p) > 4;
}

static int r_fix_va_start(struct SN_env * z) {
    int among_var;
    z->bra = z->c;
    if (z->c + 5 >= z->l || z->p[z->c + 5] >> 5 != 4 || !((3078 >> (z->p[z->c + 5] & 0x1f)) & 1)) return 0;
    among_var = find_among(z, a_0, 4, 0);
    if (!among_var) return 0;
    z->ket = z->c;
    switch (among_var) {
        case 1:
            {
                int ret = slice_from_s(z, 3, s_0);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {
                int ret = slice_from_s(z, 3, s_1);
                if (ret < 0) return ret;
            }
            break;
        case 3:
            {
                int ret = slice_from_s(z, 3, s_2);
                if (ret < 0) return ret;
            }
            break;
        case 4:
            {
                int ret = slice_from_s(z, 3, s_3);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_fix_endings(struct SN_env * z) {
    {
        int v_1 = z->c;
        while (1) {
            int v_2 = z->c;
            {
                int ret = r_fix_ending(z);
                if (ret == 0) goto lab1;
                if (ret < 0) return ret;
            }
            continue;
        lab1:
            z->c = v_2;
            break;
        }
        z->c = v_1;
    }
    return 1;
}

static int r_remove_question_prefixes(struct SN_env * z) {
    z->bra = z->c;
    if (!(eq_s(z, 3, s_4))) return 0;
    if (!find_among(z, a_1, 10, 0)) return 0;
    if (!(eq_s(z, 3, s_5))) return 0;
    z->ket = z->c;
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    {
        int v_1 = z->c;
        {
            int ret = r_fix_va_start(z);
            if (ret < 0) return ret;
        }
        z->c = v_1;
    }
    return 1;
}

static int r_fix_ending(struct SN_env * z) {
    int among_var;
    if (len_utf8(z->p) <= 3) return 0;
    z->lb = z->c; z->c = z->l;
    do {
        int v_1 = z->l - z->c;
        z->ket = z->c;
        among_var = find_among_b(z, a_5, 17, 0);
        if (!among_var) goto lab0;
        z->bra = z->c;
        switch (among_var) {
            case 1:
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                break;
            case 2:
                {
                    int v_2 = z->l - z->c;
                    if (!find_among_b(z, a_2, 3, 0)) goto lab0;
                    z->c = z->l - v_2;
                }
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                break;
            case 3:
                {
                    int ret = slice_from_s(z, 6, s_6);
                    if (ret < 0) return ret;
                }
                break;
            case 4:
                {
                    int ret = slice_from_s(z, 6, s_7);
                    if (ret < 0) return ret;
                }
                break;
            case 5:
                {
                    int ret = slice_from_s(z, 6, s_8);
                    if (ret < 0) return ret;
                }
                break;
            case 6:
                if (!((SN_local *)z)->b_found_vetrumai_urupu) goto lab0;
                {
                    int v_3 = z->l - z->c;
                    if (!(eq_s_b(z, 3, s_9))) goto lab1;
                    goto lab0;
                lab1:
                    z->c = z->l - v_3;
                }
                {
                    int ret = slice_from_s(z, 6, s_10);
                    if (ret < 0) return ret;
                }
                break;
            case 7:
                {
                    int ret = slice_from_s(z, 3, s_11);
                    if (ret < 0) return ret;
                }
                break;
            case 8:
                {
                    int v_4 = z->l - z->c;
                    if (!find_among_b(z, a_3, 8, 0)) goto lab2;
                    goto lab0;
                lab2:
                    z->c = z->l - v_4;
                }
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                break;
            case 9:
                if (z->c - 2 <= z->lb || (z->p[z->c - 1] != 136 && z->p[z->c - 1] != 141)) among_var = 2; else
                among_var = find_among_b(z, a_4, 3, 0);
                switch (among_var) {
                    case 1:
                        {
                            int ret = slice_del(z);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 2:
                        {
                            int ret = slice_from_s(z, 6, s_12);
                            if (ret < 0) return ret;
                        }
                        break;
                }
                break;
        }
        break;
    lab0:
        z->c = z->l - v_1;
        z->ket = z->c;
        if (!(eq_s_b(z, 3, s_13))) return 0;
        do {
            int v_5 = z->l - z->c;
            if (!find_among_b(z, a_6, 6, 0)) goto lab3;
            {
                int v_6 = z->l - z->c;
                if (!(eq_s_b(z, 3, s_14))) { z->c = z->l - v_6; goto lab4; }
                if (!find_among_b(z, a_7, 6, 0)) { z->c = z->l - v_6; goto lab4; }
            lab4:
                ;
            }
            z->bra = z->c;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        lab3:
            z->c = z->l - v_5;
            if (!find_among_b(z, a_8, 11, 0)) goto lab5;
            z->bra = z->c;
            if (!(eq_s_b(z, 3, s_15))) goto lab5;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        lab5:
            z->c = z->l - v_5;
            {
                int v_7 = z->l - z->c;
                if (!find_among_b(z, a_9, 9, 0)) return 0;
                z->c = z->l - v_7;
            }
            z->bra = z->c;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
        } while (0);
    } while (0);
    z->c = z->lb;
    return 1;
}

static int r_remove_pronoun_prefixes(struct SN_env * z) {
    z->bra = z->c;
    if (z->c + 2 >= z->l || z->p[z->c + 2] >> 5 != 4 || !((672 >> (z->p[z->c + 2] & 0x1f)) & 1)) return 0;
    if (!find_among(z, a_10, 3, 0)) return 0;
    if (!find_among(z, a_11, 10, 0)) return 0;
    if (!(eq_s(z, 3, s_16))) return 0;
    z->ket = z->c;
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    {
        int v_1 = z->c;
        {
            int ret = r_fix_va_start(z);
            if (ret < 0) return ret;
        }
        z->c = v_1;
    }
    return 1;
}

static int r_remove_plural_suffix(struct SN_env * z) {
    int among_var;
    z->lb = z->c; z->c = z->l;
    z->ket = z->c;
    if (z->c - 8 <= z->lb || z->p[z->c - 1] != 141) return 0;
    among_var = find_among_b(z, a_13, 4, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    switch (among_var) {
        case 1:
            do {
                int v_1 = z->l - z->c;
                if (!find_among_b(z, a_12, 6, 0)) goto lab0;
                {
                    int ret = slice_from_s(z, 9, s_17);
                    if (ret < 0) return ret;
                }
                break;
            lab0:
                z->c = z->l - v_1;
                {
                    int ret = slice_from_s(z, 3, s_18);
                    if (ret < 0) return ret;
                }
            } while (0);
            break;
        case 2:
            {
                int ret = slice_from_s(z, 6, s_19);
                if (ret < 0) return ret;
            }
            break;
        case 3:
            {
                int ret = slice_from_s(z, 6, s_20);
                if (ret < 0) return ret;
            }
            break;
        case 4:
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
    }
    z->c = z->lb;
    return 1;
}

static int r_remove_question_suffixes(struct SN_env * z) {
    {
        int ret = r_has_min_length(z);
        if (ret <= 0) return ret;
    }
    z->lb = z->c; z->c = z->l;
    {
        int v_1 = z->l - z->c;
        z->ket = z->c;
        if (!find_among_b(z, a_14, 3, 0)) goto lab0;
        z->bra = z->c;
        {
            int ret = slice_from_s(z, 3, s_21);
            if (ret < 0) return ret;
        }
    lab0:
        z->c = z->l - v_1;
    }
    z->c = z->lb;
    {
        int ret = r_fix_endings(z);
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_remove_command_suffixes(struct SN_env * z) {
    {
        int ret = r_has_min_length(z);
        if (ret <= 0) return ret;
    }
    z->lb = z->c; z->c = z->l;
    z->ket = z->c;
    if (z->c - 5 <= z->lb || z->p[z->c - 1] != 191) return 0;
    if (!find_among_b(z, a_15, 2, 0)) return 0;
    z->bra = z->c;
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    z->c = z->lb;
    return 1;
}

static int r_remove_um(struct SN_env * z) {
    {
        int ret = r_has_min_length(z);
        if (ret <= 0) return ret;
    }
    z->lb = z->c; z->c = z->l;
    z->ket = z->c;
    if (!(eq_s_b(z, 9, s_22))) return 0;
    z->bra = z->c;
    {
        int ret = slice_from_s(z, 3, s_23);
        if (ret < 0) return ret;
    }
    z->c = z->lb;
    {
        int v_1 = z->c;
        {
            int ret = r_fix_ending(z);
            if (ret < 0) return ret;
        }
        z->c = v_1;
    }
    return 1;
}

static int r_remove_common_word_endings(struct SN_env * z) {
    int among_var;
    {
        int ret = r_has_min_length(z);
        if (ret <= 0) return ret;
    }
    z->lb = z->c; z->c = z->l;
    z->ket = z->c;
    among_var = find_among_b(z, a_17, 26, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    switch (among_var) {
        case 1:
            {
                int ret = slice_from_s(z, 3, s_24);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {
                int v_1 = z->l - z->c;
                if (!find_among_b(z, a_16, 8, 0)) goto lab0;
                return 0;
            lab0:
                z->c = z->l - v_1;
            }
            {
                int ret = slice_from_s(z, 3, s_25);
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
    z->c = z->lb;
    {
        int ret = r_fix_endings(z);
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_remove_vetrumai_urupukal(struct SN_env * z) {
    int among_var;
    ((SN_local *)z)->b_found_vetrumai_urupu = 0;
    {
        int ret = r_has_min_length(z);
        if (ret <= 0) return ret;
    }
    z->lb = z->c; z->c = z->l;
    do {
        int v_1 = z->l - z->c;
        {
            int v_2 = z->l - z->c;
            z->ket = z->c;
            if (z->c - 2 <= z->lb || z->p[z->c - 1] >> 5 != 4 || !((-2147475197 >> (z->p[z->c - 1] & 0x1f)) & 1)) goto lab0;
            among_var = find_among_b(z, a_20, 22, 0);
            if (!among_var) goto lab0;
            z->bra = z->c;
            switch (among_var) {
                case 1:
                    {
                        int ret = slice_del(z);
                        if (ret < 0) return ret;
                    }
                    break;
                case 2:
                    {
                        int ret = slice_from_s(z, 3, s_26);
                        if (ret < 0) return ret;
                    }
                    break;
                case 3:
                    {
                        int v_3 = z->l - z->c;
                        if (!(eq_s_b(z, 3, s_27))) goto lab1;
                        goto lab0;
                    lab1:
                        z->c = z->l - v_3;
                    }
                    {
                        int ret = slice_from_s(z, 3, s_28);
                        if (ret < 0) return ret;
                    }
                    break;
                case 4:
                    if (len_utf8(z->p) < 7) goto lab0;
                    {
                        int ret = slice_from_s(z, 3, s_29);
                        if (ret < 0) return ret;
                    }
                    break;
                case 5:
                    {
                        int v_4 = z->l - z->c;
                        if (!find_among_b(z, a_18, 8, 0)) goto lab2;
                        goto lab0;
                    lab2:
                        z->c = z->l - v_4;
                    }
                    {
                        int ret = slice_from_s(z, 3, s_30);
                        if (ret < 0) return ret;
                    }
                    break;
                case 6:
                    {
                        int v_5 = z->l - z->c;
                        if (!find_among_b(z, a_19, 8, 0)) goto lab3;
                        goto lab0;
                    lab3:
                        z->c = z->l - v_5;
                    }
                    {
                        int ret = slice_del(z);
                        if (ret < 0) return ret;
                    }
                    break;
                case 7:
                    {
                        int ret = slice_from_s(z, 3, s_31);
                        if (ret < 0) return ret;
                    }
                    break;
            }
            z->c = z->l - v_2;
        }
        break;
    lab0:
        z->c = z->l - v_1;
        {
            int v_6 = z->l - z->c;
            z->ket = z->c;
            if (!(eq_s_b(z, 3, s_32))) return 0;
            do {
                int v_7 = z->l - z->c;
                {
                    int v_8 = z->l - z->c;
                    if (!find_among_b(z, a_21, 6, 0)) goto lab5;
                    goto lab4;
                lab5:
                    z->c = z->l - v_8;
                }
                break;
            lab4:
                z->c = z->l - v_7;
                {
                    int v_9 = z->l - z->c;
                    if (!find_among_b(z, a_22, 6, 0)) return 0;
                    if (!(eq_s_b(z, 3, s_33))) return 0;
                    z->c = z->l - v_9;
                }
            } while (0);
            z->bra = z->c;
            {
                int ret = slice_from_s(z, 3, s_34);
                if (ret < 0) return ret;
            }
            z->c = z->l - v_6;
        }
    } while (0);
    ((SN_local *)z)->b_found_vetrumai_urupu = 1;
    {
        int v_10 = z->l - z->c;
        z->ket = z->c;
        if (!(eq_s_b(z, 9, s_35))) goto lab6;
        z->bra = z->c;
        {
            int ret = slice_from_s(z, 3, s_36);
            if (ret < 0) return ret;
        }
    lab6:
        z->c = z->l - v_10;
    }
    z->c = z->lb;
    {
        int ret = r_fix_endings(z);
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_remove_tense_suffixes(struct SN_env * z) {
    while (1) {
        int v_1 = z->c;
        {
            int ret = r_remove_tense_suffix(z);
            if (ret == 0) goto lab0;
            if (ret < 0) return ret;
        }
        continue;
    lab0:
        z->c = v_1;
        break;
    }
    return 1;
}

static int r_remove_tense_suffix(struct SN_env * z) {
    int among_var;
    int b_found_a_match;
    b_found_a_match = 0;
    {
        int ret = r_has_min_length(z);
        if (ret <= 0) return ret;
    }
    z->lb = z->c; z->c = z->l;
    {
        int v_1 = z->l - z->c;
        {
            int v_2 = z->l - z->c;
            z->ket = z->c;
            among_var = find_among_b(z, a_25, 46, 0);
            if (!among_var) goto lab0;
            z->bra = z->c;
            switch (among_var) {
                case 1:
                    {
                        int ret = slice_del(z);
                        if (ret < 0) return ret;
                    }
                    break;
                case 2:
                    {
                        int v_3 = z->l - z->c;
                        if (z->c - 2 <= z->lb || z->p[z->c - 1] >> 5 != 4 || !((1951712 >> (z->p[z->c - 1] & 0x1f)) & 1)) goto lab1;
                        if (!find_among_b(z, a_23, 12, 0)) goto lab1;
                        goto lab0;
                    lab1:
                        z->c = z->l - v_3;
                    }
                    {
                        int ret = slice_del(z);
                        if (ret < 0) return ret;
                    }
                    break;
                case 3:
                    {
                        int v_4 = z->l - z->c;
                        if (!find_among_b(z, a_24, 8, 0)) goto lab2;
                        goto lab0;
                    lab2:
                        z->c = z->l - v_4;
                    }
                    {
                        int ret = slice_del(z);
                        if (ret < 0) return ret;
                    }
                    break;
                case 4:
                    {
                        int v_5 = z->l - z->c;
                        if (!(eq_s_b(z, 3, s_37))) goto lab3;
                        goto lab0;
                    lab3:
                        z->c = z->l - v_5;
                    }
                    {
                        int ret = slice_from_s(z, 3, s_38);
                        if (ret < 0) return ret;
                    }
                    break;
                case 5:
                    {
                        int ret = slice_from_s(z, 3, s_39);
                        if (ret < 0) return ret;
                    }
                    break;
                case 6:
                    {
                        int v_6 = z->l - z->c;
                        if (!(eq_s_b(z, 3, s_40))) goto lab0;
                        z->c = z->l - v_6;
                    }
                    {
                        int ret = slice_del(z);
                        if (ret < 0) return ret;
                    }
                    break;
            }
            b_found_a_match = 1;
            z->c = z->l - v_2;
        }
    lab0:
        z->c = z->l - v_1;
    }
    {
        int v_7 = z->l - z->c;
        z->ket = z->c;
        if (z->c - 8 <= z->lb || (z->p[z->c - 1] != 141 && z->p[z->c - 1] != 177)) goto lab4;
        if (!find_among_b(z, a_26, 6, 0)) goto lab4;
        z->bra = z->c;
        {
            int ret = slice_del(z);
            if (ret < 0) return ret;
        }
        b_found_a_match = 1;
    lab4:
        z->c = z->l - v_7;
    }
    z->c = z->lb;
    {
        int ret = r_fix_endings(z);
        if (ret < 0) return ret;
    }
    return b_found_a_match;
}

extern int tamil_UTF_8_stem(struct SN_env * z) {
    ((SN_local *)z)->b_found_vetrumai_urupu = 0;
    {
        int v_1 = z->c;
        {
            int ret = r_fix_ending(z);
            if (ret < 0) return ret;
        }
        z->c = v_1;
    }
    {
        int ret = r_has_min_length(z);
        if (ret <= 0) return ret;
    }
    {
        int v_2 = z->c;
        {
            int ret = r_remove_question_prefixes(z);
            if (ret < 0) return ret;
        }
        z->c = v_2;
    }
    {
        int v_3 = z->c;
        {
            int ret = r_remove_pronoun_prefixes(z);
            if (ret < 0) return ret;
        }
        z->c = v_3;
    }
    {
        int v_4 = z->c;
        {
            int ret = r_remove_question_suffixes(z);
            if (ret < 0) return ret;
        }
        z->c = v_4;
    }
    {
        int v_5 = z->c;
        {
            int ret = r_remove_um(z);
            if (ret < 0) return ret;
        }
        z->c = v_5;
    }
    {
        int v_6 = z->c;
        {
            int ret = r_remove_common_word_endings(z);
            if (ret < 0) return ret;
        }
        z->c = v_6;
    }
    {
        int v_7 = z->c;
        {
            int ret = r_remove_vetrumai_urupukal(z);
            if (ret < 0) return ret;
        }
        z->c = v_7;
    }
    {
        int v_8 = z->c;
        {
            int ret = r_remove_plural_suffix(z);
            if (ret < 0) return ret;
        }
        z->c = v_8;
    }
    {
        int v_9 = z->c;
        {
            int ret = r_remove_command_suffixes(z);
            if (ret < 0) return ret;
        }
        z->c = v_9;
    }
    {
        int v_10 = z->c;
        {
            int ret = r_remove_tense_suffixes(z);
            if (ret < 0) return ret;
        }
        z->c = v_10;
    }
    return 1;
}

extern struct SN_env * tamil_UTF_8_create_env(void) {
    struct SN_env * z = SN_new_env(sizeof(SN_local));
    if (z) {
        ((SN_local *)z)->b_found_vetrumai_urupu = 0;
    }
    return z;
}

extern void tamil_UTF_8_close_env(struct SN_env * z) {
    SN_delete_env(z);
}

