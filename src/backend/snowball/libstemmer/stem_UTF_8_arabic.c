/* Generated from arabic.sbl by Snowball 3.0.0 - https://snowballstem.org/ */

#include "stem_UTF_8_arabic.h"

#include <stddef.h>

#include "snowball_runtime.h"

struct SN_local {
    struct SN_env z;
    unsigned char b_is_defined;
    unsigned char b_is_verb;
    unsigned char b_is_noun;
};

typedef struct SN_local SN_local;

#ifdef __cplusplus
extern "C" {
#endif
extern int arabic_UTF_8_stem(struct SN_env * z);
#ifdef __cplusplus
}
#endif

static int r_Checks1(struct SN_env * z);
static int r_Normalize_pre(struct SN_env * z);
static int r_Normalize_post(struct SN_env * z);
static int r_Suffix_Verb_Step2c(struct SN_env * z);
static int r_Suffix_Verb_Step2b(struct SN_env * z);
static int r_Suffix_Verb_Step2a(struct SN_env * z);
static int r_Suffix_Verb_Step1(struct SN_env * z);
static int r_Suffix_Noun_Step3(struct SN_env * z);
static int r_Suffix_Noun_Step2c2(struct SN_env * z);
static int r_Suffix_Noun_Step2c1(struct SN_env * z);
static int r_Suffix_Noun_Step2b(struct SN_env * z);
static int r_Suffix_Noun_Step2a(struct SN_env * z);
static int r_Suffix_Noun_Step1b(struct SN_env * z);
static int r_Suffix_Noun_Step1a(struct SN_env * z);
static int r_Suffix_All_alef_maqsura(struct SN_env * z);
static int r_Prefix_Step4_Verb(struct SN_env * z);
static int r_Prefix_Step3_Verb(struct SN_env * z);
static int r_Prefix_Step3b_Noun(struct SN_env * z);
static int r_Prefix_Step3a_Noun(struct SN_env * z);
static int r_Prefix_Step2(struct SN_env * z);
static int r_Prefix_Step1(struct SN_env * z);

static const symbol s_0[] = { '0' };
static const symbol s_1[] = { '1' };
static const symbol s_2[] = { '2' };
static const symbol s_3[] = { '3' };
static const symbol s_4[] = { '4' };
static const symbol s_5[] = { '5' };
static const symbol s_6[] = { '6' };
static const symbol s_7[] = { '7' };
static const symbol s_8[] = { '8' };
static const symbol s_9[] = { '9' };
static const symbol s_10[] = { 0xD8, 0xA1 };
static const symbol s_11[] = { 0xD8, 0xA3 };
static const symbol s_12[] = { 0xD8, 0xA5 };
static const symbol s_13[] = { 0xD8, 0xA6 };
static const symbol s_14[] = { 0xD8, 0xA2 };
static const symbol s_15[] = { 0xD8, 0xA4 };
static const symbol s_16[] = { 0xD8, 0xA7 };
static const symbol s_17[] = { 0xD8, 0xA8 };
static const symbol s_18[] = { 0xD8, 0xA9 };
static const symbol s_19[] = { 0xD8, 0xAA };
static const symbol s_20[] = { 0xD8, 0xAB };
static const symbol s_21[] = { 0xD8, 0xAC };
static const symbol s_22[] = { 0xD8, 0xAD };
static const symbol s_23[] = { 0xD8, 0xAE };
static const symbol s_24[] = { 0xD8, 0xAF };
static const symbol s_25[] = { 0xD8, 0xB0 };
static const symbol s_26[] = { 0xD8, 0xB1 };
static const symbol s_27[] = { 0xD8, 0xB2 };
static const symbol s_28[] = { 0xD8, 0xB3 };
static const symbol s_29[] = { 0xD8, 0xB4 };
static const symbol s_30[] = { 0xD8, 0xB5 };
static const symbol s_31[] = { 0xD8, 0xB6 };
static const symbol s_32[] = { 0xD8, 0xB7 };
static const symbol s_33[] = { 0xD8, 0xB8 };
static const symbol s_34[] = { 0xD8, 0xB9 };
static const symbol s_35[] = { 0xD8, 0xBA };
static const symbol s_36[] = { 0xD9, 0x81 };
static const symbol s_37[] = { 0xD9, 0x82 };
static const symbol s_38[] = { 0xD9, 0x83 };
static const symbol s_39[] = { 0xD9, 0x84 };
static const symbol s_40[] = { 0xD9, 0x85 };
static const symbol s_41[] = { 0xD9, 0x86 };
static const symbol s_42[] = { 0xD9, 0x87 };
static const symbol s_43[] = { 0xD9, 0x88 };
static const symbol s_44[] = { 0xD9, 0x89 };
static const symbol s_45[] = { 0xD9, 0x8A };
static const symbol s_46[] = { 0xD9, 0x84, 0xD8, 0xA7 };
static const symbol s_47[] = { 0xD9, 0x84, 0xD8, 0xA3 };
static const symbol s_48[] = { 0xD9, 0x84, 0xD8, 0xA5 };
static const symbol s_49[] = { 0xD9, 0x84, 0xD8, 0xA2 };
static const symbol s_50[] = { 0xD8, 0xA1 };
static const symbol s_51[] = { 0xD8, 0xA7 };
static const symbol s_52[] = { 0xD9, 0x88 };
static const symbol s_53[] = { 0xD9, 0x8A };
static const symbol s_54[] = { 0xD8, 0xA3 };
static const symbol s_55[] = { 0xD8, 0xA2 };
static const symbol s_56[] = { 0xD8, 0xA7 };
static const symbol s_57[] = { 0xD8, 0xA5 };
static const symbol s_58[] = { 0xD8, 0xA7 };
static const symbol s_59[] = { 0xD8, 0xA8 };
static const symbol s_60[] = { 0xD9, 0x83 };
static const symbol s_61[] = { 0xD9, 0x8A };
static const symbol s_62[] = { 0xD8, 0xAA };
static const symbol s_63[] = { 0xD9, 0x86 };
static const symbol s_64[] = { 0xD8, 0xA3 };
static const symbol s_65[] = { 0xD8, 0xA7, 0xD8, 0xB3, 0xD8, 0xAA };
static const symbol s_66[] = { 0xD9, 0x86 };
static const symbol s_67[] = { 0xD8, 0xA7, 0xD8, 0xAA };
static const symbol s_68[] = { 0xD8, 0xAA };
static const symbol s_69[] = { 0xD8, 0xA9 };
static const symbol s_70[] = { 0xD9, 0x8A };
static const symbol s_71[] = { 0xD9, 0x89 };
static const symbol s_72[] = { 0xD9, 0x8A };

static const symbol s_0_0[2] = { 0xD9, 0x80 };
static const symbol s_0_1[2] = { 0xD9, 0x8B };
static const symbol s_0_2[2] = { 0xD9, 0x8C };
static const symbol s_0_3[2] = { 0xD9, 0x8D };
static const symbol s_0_4[2] = { 0xD9, 0x8E };
static const symbol s_0_5[2] = { 0xD9, 0x8F };
static const symbol s_0_6[2] = { 0xD9, 0x90 };
static const symbol s_0_7[2] = { 0xD9, 0x91 };
static const symbol s_0_8[2] = { 0xD9, 0x92 };
static const symbol s_0_9[2] = { 0xD9, 0xA0 };
static const symbol s_0_10[2] = { 0xD9, 0xA1 };
static const symbol s_0_11[2] = { 0xD9, 0xA2 };
static const symbol s_0_12[2] = { 0xD9, 0xA3 };
static const symbol s_0_13[2] = { 0xD9, 0xA4 };
static const symbol s_0_14[2] = { 0xD9, 0xA5 };
static const symbol s_0_15[2] = { 0xD9, 0xA6 };
static const symbol s_0_16[2] = { 0xD9, 0xA7 };
static const symbol s_0_17[2] = { 0xD9, 0xA8 };
static const symbol s_0_18[2] = { 0xD9, 0xA9 };
static const symbol s_0_19[3] = { 0xEF, 0xBA, 0x80 };
static const symbol s_0_20[3] = { 0xEF, 0xBA, 0x81 };
static const symbol s_0_21[3] = { 0xEF, 0xBA, 0x82 };
static const symbol s_0_22[3] = { 0xEF, 0xBA, 0x83 };
static const symbol s_0_23[3] = { 0xEF, 0xBA, 0x84 };
static const symbol s_0_24[3] = { 0xEF, 0xBA, 0x85 };
static const symbol s_0_25[3] = { 0xEF, 0xBA, 0x86 };
static const symbol s_0_26[3] = { 0xEF, 0xBA, 0x87 };
static const symbol s_0_27[3] = { 0xEF, 0xBA, 0x88 };
static const symbol s_0_28[3] = { 0xEF, 0xBA, 0x89 };
static const symbol s_0_29[3] = { 0xEF, 0xBA, 0x8A };
static const symbol s_0_30[3] = { 0xEF, 0xBA, 0x8B };
static const symbol s_0_31[3] = { 0xEF, 0xBA, 0x8C };
static const symbol s_0_32[3] = { 0xEF, 0xBA, 0x8D };
static const symbol s_0_33[3] = { 0xEF, 0xBA, 0x8E };
static const symbol s_0_34[3] = { 0xEF, 0xBA, 0x8F };
static const symbol s_0_35[3] = { 0xEF, 0xBA, 0x90 };
static const symbol s_0_36[3] = { 0xEF, 0xBA, 0x91 };
static const symbol s_0_37[3] = { 0xEF, 0xBA, 0x92 };
static const symbol s_0_38[3] = { 0xEF, 0xBA, 0x93 };
static const symbol s_0_39[3] = { 0xEF, 0xBA, 0x94 };
static const symbol s_0_40[3] = { 0xEF, 0xBA, 0x95 };
static const symbol s_0_41[3] = { 0xEF, 0xBA, 0x96 };
static const symbol s_0_42[3] = { 0xEF, 0xBA, 0x97 };
static const symbol s_0_43[3] = { 0xEF, 0xBA, 0x98 };
static const symbol s_0_44[3] = { 0xEF, 0xBA, 0x99 };
static const symbol s_0_45[3] = { 0xEF, 0xBA, 0x9A };
static const symbol s_0_46[3] = { 0xEF, 0xBA, 0x9B };
static const symbol s_0_47[3] = { 0xEF, 0xBA, 0x9C };
static const symbol s_0_48[3] = { 0xEF, 0xBA, 0x9D };
static const symbol s_0_49[3] = { 0xEF, 0xBA, 0x9E };
static const symbol s_0_50[3] = { 0xEF, 0xBA, 0x9F };
static const symbol s_0_51[3] = { 0xEF, 0xBA, 0xA0 };
static const symbol s_0_52[3] = { 0xEF, 0xBA, 0xA1 };
static const symbol s_0_53[3] = { 0xEF, 0xBA, 0xA2 };
static const symbol s_0_54[3] = { 0xEF, 0xBA, 0xA3 };
static const symbol s_0_55[3] = { 0xEF, 0xBA, 0xA4 };
static const symbol s_0_56[3] = { 0xEF, 0xBA, 0xA5 };
static const symbol s_0_57[3] = { 0xEF, 0xBA, 0xA6 };
static const symbol s_0_58[3] = { 0xEF, 0xBA, 0xA7 };
static const symbol s_0_59[3] = { 0xEF, 0xBA, 0xA8 };
static const symbol s_0_60[3] = { 0xEF, 0xBA, 0xA9 };
static const symbol s_0_61[3] = { 0xEF, 0xBA, 0xAA };
static const symbol s_0_62[3] = { 0xEF, 0xBA, 0xAB };
static const symbol s_0_63[3] = { 0xEF, 0xBA, 0xAC };
static const symbol s_0_64[3] = { 0xEF, 0xBA, 0xAD };
static const symbol s_0_65[3] = { 0xEF, 0xBA, 0xAE };
static const symbol s_0_66[3] = { 0xEF, 0xBA, 0xAF };
static const symbol s_0_67[3] = { 0xEF, 0xBA, 0xB0 };
static const symbol s_0_68[3] = { 0xEF, 0xBA, 0xB1 };
static const symbol s_0_69[3] = { 0xEF, 0xBA, 0xB2 };
static const symbol s_0_70[3] = { 0xEF, 0xBA, 0xB3 };
static const symbol s_0_71[3] = { 0xEF, 0xBA, 0xB4 };
static const symbol s_0_72[3] = { 0xEF, 0xBA, 0xB5 };
static const symbol s_0_73[3] = { 0xEF, 0xBA, 0xB6 };
static const symbol s_0_74[3] = { 0xEF, 0xBA, 0xB7 };
static const symbol s_0_75[3] = { 0xEF, 0xBA, 0xB8 };
static const symbol s_0_76[3] = { 0xEF, 0xBA, 0xB9 };
static const symbol s_0_77[3] = { 0xEF, 0xBA, 0xBA };
static const symbol s_0_78[3] = { 0xEF, 0xBA, 0xBB };
static const symbol s_0_79[3] = { 0xEF, 0xBA, 0xBC };
static const symbol s_0_80[3] = { 0xEF, 0xBA, 0xBD };
static const symbol s_0_81[3] = { 0xEF, 0xBA, 0xBE };
static const symbol s_0_82[3] = { 0xEF, 0xBA, 0xBF };
static const symbol s_0_83[3] = { 0xEF, 0xBB, 0x80 };
static const symbol s_0_84[3] = { 0xEF, 0xBB, 0x81 };
static const symbol s_0_85[3] = { 0xEF, 0xBB, 0x82 };
static const symbol s_0_86[3] = { 0xEF, 0xBB, 0x83 };
static const symbol s_0_87[3] = { 0xEF, 0xBB, 0x84 };
static const symbol s_0_88[3] = { 0xEF, 0xBB, 0x85 };
static const symbol s_0_89[3] = { 0xEF, 0xBB, 0x86 };
static const symbol s_0_90[3] = { 0xEF, 0xBB, 0x87 };
static const symbol s_0_91[3] = { 0xEF, 0xBB, 0x88 };
static const symbol s_0_92[3] = { 0xEF, 0xBB, 0x89 };
static const symbol s_0_93[3] = { 0xEF, 0xBB, 0x8A };
static const symbol s_0_94[3] = { 0xEF, 0xBB, 0x8B };
static const symbol s_0_95[3] = { 0xEF, 0xBB, 0x8C };
static const symbol s_0_96[3] = { 0xEF, 0xBB, 0x8D };
static const symbol s_0_97[3] = { 0xEF, 0xBB, 0x8E };
static const symbol s_0_98[3] = { 0xEF, 0xBB, 0x8F };
static const symbol s_0_99[3] = { 0xEF, 0xBB, 0x90 };
static const symbol s_0_100[3] = { 0xEF, 0xBB, 0x91 };
static const symbol s_0_101[3] = { 0xEF, 0xBB, 0x92 };
static const symbol s_0_102[3] = { 0xEF, 0xBB, 0x93 };
static const symbol s_0_103[3] = { 0xEF, 0xBB, 0x94 };
static const symbol s_0_104[3] = { 0xEF, 0xBB, 0x95 };
static const symbol s_0_105[3] = { 0xEF, 0xBB, 0x96 };
static const symbol s_0_106[3] = { 0xEF, 0xBB, 0x97 };
static const symbol s_0_107[3] = { 0xEF, 0xBB, 0x98 };
static const symbol s_0_108[3] = { 0xEF, 0xBB, 0x99 };
static const symbol s_0_109[3] = { 0xEF, 0xBB, 0x9A };
static const symbol s_0_110[3] = { 0xEF, 0xBB, 0x9B };
static const symbol s_0_111[3] = { 0xEF, 0xBB, 0x9C };
static const symbol s_0_112[3] = { 0xEF, 0xBB, 0x9D };
static const symbol s_0_113[3] = { 0xEF, 0xBB, 0x9E };
static const symbol s_0_114[3] = { 0xEF, 0xBB, 0x9F };
static const symbol s_0_115[3] = { 0xEF, 0xBB, 0xA0 };
static const symbol s_0_116[3] = { 0xEF, 0xBB, 0xA1 };
static const symbol s_0_117[3] = { 0xEF, 0xBB, 0xA2 };
static const symbol s_0_118[3] = { 0xEF, 0xBB, 0xA3 };
static const symbol s_0_119[3] = { 0xEF, 0xBB, 0xA4 };
static const symbol s_0_120[3] = { 0xEF, 0xBB, 0xA5 };
static const symbol s_0_121[3] = { 0xEF, 0xBB, 0xA6 };
static const symbol s_0_122[3] = { 0xEF, 0xBB, 0xA7 };
static const symbol s_0_123[3] = { 0xEF, 0xBB, 0xA8 };
static const symbol s_0_124[3] = { 0xEF, 0xBB, 0xA9 };
static const symbol s_0_125[3] = { 0xEF, 0xBB, 0xAA };
static const symbol s_0_126[3] = { 0xEF, 0xBB, 0xAB };
static const symbol s_0_127[3] = { 0xEF, 0xBB, 0xAC };
static const symbol s_0_128[3] = { 0xEF, 0xBB, 0xAD };
static const symbol s_0_129[3] = { 0xEF, 0xBB, 0xAE };
static const symbol s_0_130[3] = { 0xEF, 0xBB, 0xAF };
static const symbol s_0_131[3] = { 0xEF, 0xBB, 0xB0 };
static const symbol s_0_132[3] = { 0xEF, 0xBB, 0xB1 };
static const symbol s_0_133[3] = { 0xEF, 0xBB, 0xB2 };
static const symbol s_0_134[3] = { 0xEF, 0xBB, 0xB3 };
static const symbol s_0_135[3] = { 0xEF, 0xBB, 0xB4 };
static const symbol s_0_136[3] = { 0xEF, 0xBB, 0xB5 };
static const symbol s_0_137[3] = { 0xEF, 0xBB, 0xB6 };
static const symbol s_0_138[3] = { 0xEF, 0xBB, 0xB7 };
static const symbol s_0_139[3] = { 0xEF, 0xBB, 0xB8 };
static const symbol s_0_140[3] = { 0xEF, 0xBB, 0xB9 };
static const symbol s_0_141[3] = { 0xEF, 0xBB, 0xBA };
static const symbol s_0_142[3] = { 0xEF, 0xBB, 0xBB };
static const symbol s_0_143[3] = { 0xEF, 0xBB, 0xBC };
static const struct among a_0[144] = {
{ 2, s_0_0, 0, 1, 0},
{ 2, s_0_1, 0, 1, 0},
{ 2, s_0_2, 0, 1, 0},
{ 2, s_0_3, 0, 1, 0},
{ 2, s_0_4, 0, 1, 0},
{ 2, s_0_5, 0, 1, 0},
{ 2, s_0_6, 0, 1, 0},
{ 2, s_0_7, 0, 1, 0},
{ 2, s_0_8, 0, 1, 0},
{ 2, s_0_9, 0, 2, 0},
{ 2, s_0_10, 0, 3, 0},
{ 2, s_0_11, 0, 4, 0},
{ 2, s_0_12, 0, 5, 0},
{ 2, s_0_13, 0, 6, 0},
{ 2, s_0_14, 0, 7, 0},
{ 2, s_0_15, 0, 8, 0},
{ 2, s_0_16, 0, 9, 0},
{ 2, s_0_17, 0, 10, 0},
{ 2, s_0_18, 0, 11, 0},
{ 3, s_0_19, 0, 12, 0},
{ 3, s_0_20, 0, 16, 0},
{ 3, s_0_21, 0, 16, 0},
{ 3, s_0_22, 0, 13, 0},
{ 3, s_0_23, 0, 13, 0},
{ 3, s_0_24, 0, 17, 0},
{ 3, s_0_25, 0, 17, 0},
{ 3, s_0_26, 0, 14, 0},
{ 3, s_0_27, 0, 14, 0},
{ 3, s_0_28, 0, 15, 0},
{ 3, s_0_29, 0, 15, 0},
{ 3, s_0_30, 0, 15, 0},
{ 3, s_0_31, 0, 15, 0},
{ 3, s_0_32, 0, 18, 0},
{ 3, s_0_33, 0, 18, 0},
{ 3, s_0_34, 0, 19, 0},
{ 3, s_0_35, 0, 19, 0},
{ 3, s_0_36, 0, 19, 0},
{ 3, s_0_37, 0, 19, 0},
{ 3, s_0_38, 0, 20, 0},
{ 3, s_0_39, 0, 20, 0},
{ 3, s_0_40, 0, 21, 0},
{ 3, s_0_41, 0, 21, 0},
{ 3, s_0_42, 0, 21, 0},
{ 3, s_0_43, 0, 21, 0},
{ 3, s_0_44, 0, 22, 0},
{ 3, s_0_45, 0, 22, 0},
{ 3, s_0_46, 0, 22, 0},
{ 3, s_0_47, 0, 22, 0},
{ 3, s_0_48, 0, 23, 0},
{ 3, s_0_49, 0, 23, 0},
{ 3, s_0_50, 0, 23, 0},
{ 3, s_0_51, 0, 23, 0},
{ 3, s_0_52, 0, 24, 0},
{ 3, s_0_53, 0, 24, 0},
{ 3, s_0_54, 0, 24, 0},
{ 3, s_0_55, 0, 24, 0},
{ 3, s_0_56, 0, 25, 0},
{ 3, s_0_57, 0, 25, 0},
{ 3, s_0_58, 0, 25, 0},
{ 3, s_0_59, 0, 25, 0},
{ 3, s_0_60, 0, 26, 0},
{ 3, s_0_61, 0, 26, 0},
{ 3, s_0_62, 0, 27, 0},
{ 3, s_0_63, 0, 27, 0},
{ 3, s_0_64, 0, 28, 0},
{ 3, s_0_65, 0, 28, 0},
{ 3, s_0_66, 0, 29, 0},
{ 3, s_0_67, 0, 29, 0},
{ 3, s_0_68, 0, 30, 0},
{ 3, s_0_69, 0, 30, 0},
{ 3, s_0_70, 0, 30, 0},
{ 3, s_0_71, 0, 30, 0},
{ 3, s_0_72, 0, 31, 0},
{ 3, s_0_73, 0, 31, 0},
{ 3, s_0_74, 0, 31, 0},
{ 3, s_0_75, 0, 31, 0},
{ 3, s_0_76, 0, 32, 0},
{ 3, s_0_77, 0, 32, 0},
{ 3, s_0_78, 0, 32, 0},
{ 3, s_0_79, 0, 32, 0},
{ 3, s_0_80, 0, 33, 0},
{ 3, s_0_81, 0, 33, 0},
{ 3, s_0_82, 0, 33, 0},
{ 3, s_0_83, 0, 33, 0},
{ 3, s_0_84, 0, 34, 0},
{ 3, s_0_85, 0, 34, 0},
{ 3, s_0_86, 0, 34, 0},
{ 3, s_0_87, 0, 34, 0},
{ 3, s_0_88, 0, 35, 0},
{ 3, s_0_89, 0, 35, 0},
{ 3, s_0_90, 0, 35, 0},
{ 3, s_0_91, 0, 35, 0},
{ 3, s_0_92, 0, 36, 0},
{ 3, s_0_93, 0, 36, 0},
{ 3, s_0_94, 0, 36, 0},
{ 3, s_0_95, 0, 36, 0},
{ 3, s_0_96, 0, 37, 0},
{ 3, s_0_97, 0, 37, 0},
{ 3, s_0_98, 0, 37, 0},
{ 3, s_0_99, 0, 37, 0},
{ 3, s_0_100, 0, 38, 0},
{ 3, s_0_101, 0, 38, 0},
{ 3, s_0_102, 0, 38, 0},
{ 3, s_0_103, 0, 38, 0},
{ 3, s_0_104, 0, 39, 0},
{ 3, s_0_105, 0, 39, 0},
{ 3, s_0_106, 0, 39, 0},
{ 3, s_0_107, 0, 39, 0},
{ 3, s_0_108, 0, 40, 0},
{ 3, s_0_109, 0, 40, 0},
{ 3, s_0_110, 0, 40, 0},
{ 3, s_0_111, 0, 40, 0},
{ 3, s_0_112, 0, 41, 0},
{ 3, s_0_113, 0, 41, 0},
{ 3, s_0_114, 0, 41, 0},
{ 3, s_0_115, 0, 41, 0},
{ 3, s_0_116, 0, 42, 0},
{ 3, s_0_117, 0, 42, 0},
{ 3, s_0_118, 0, 42, 0},
{ 3, s_0_119, 0, 42, 0},
{ 3, s_0_120, 0, 43, 0},
{ 3, s_0_121, 0, 43, 0},
{ 3, s_0_122, 0, 43, 0},
{ 3, s_0_123, 0, 43, 0},
{ 3, s_0_124, 0, 44, 0},
{ 3, s_0_125, 0, 44, 0},
{ 3, s_0_126, 0, 44, 0},
{ 3, s_0_127, 0, 44, 0},
{ 3, s_0_128, 0, 45, 0},
{ 3, s_0_129, 0, 45, 0},
{ 3, s_0_130, 0, 46, 0},
{ 3, s_0_131, 0, 46, 0},
{ 3, s_0_132, 0, 47, 0},
{ 3, s_0_133, 0, 47, 0},
{ 3, s_0_134, 0, 47, 0},
{ 3, s_0_135, 0, 47, 0},
{ 3, s_0_136, 0, 51, 0},
{ 3, s_0_137, 0, 51, 0},
{ 3, s_0_138, 0, 49, 0},
{ 3, s_0_139, 0, 49, 0},
{ 3, s_0_140, 0, 50, 0},
{ 3, s_0_141, 0, 50, 0},
{ 3, s_0_142, 0, 48, 0},
{ 3, s_0_143, 0, 48, 0}
};

static const symbol s_1_0[2] = { 0xD8, 0xA2 };
static const symbol s_1_1[2] = { 0xD8, 0xA3 };
static const symbol s_1_2[2] = { 0xD8, 0xA4 };
static const symbol s_1_3[2] = { 0xD8, 0xA5 };
static const symbol s_1_4[2] = { 0xD8, 0xA6 };
static const struct among a_1[5] = {
{ 2, s_1_0, 0, 1, 0},
{ 2, s_1_1, 0, 1, 0},
{ 2, s_1_2, 0, 1, 0},
{ 2, s_1_3, 0, 1, 0},
{ 2, s_1_4, 0, 1, 0}
};

static const symbol s_2_0[2] = { 0xD8, 0xA2 };
static const symbol s_2_1[2] = { 0xD8, 0xA3 };
static const symbol s_2_2[2] = { 0xD8, 0xA4 };
static const symbol s_2_3[2] = { 0xD8, 0xA5 };
static const symbol s_2_4[2] = { 0xD8, 0xA6 };
static const struct among a_2[5] = {
{ 2, s_2_0, 0, 1, 0},
{ 2, s_2_1, 0, 1, 0},
{ 2, s_2_2, 0, 2, 0},
{ 2, s_2_3, 0, 1, 0},
{ 2, s_2_4, 0, 3, 0}
};

static const symbol s_3_0[4] = { 0xD8, 0xA7, 0xD9, 0x84 };
static const symbol s_3_1[6] = { 0xD8, 0xA8, 0xD8, 0xA7, 0xD9, 0x84 };
static const symbol s_3_2[6] = { 0xD9, 0x83, 0xD8, 0xA7, 0xD9, 0x84 };
static const symbol s_3_3[4] = { 0xD9, 0x84, 0xD9, 0x84 };
static const struct among a_3[4] = {
{ 4, s_3_0, 0, 2, 0},
{ 6, s_3_1, 0, 1, 0},
{ 6, s_3_2, 0, 1, 0},
{ 4, s_3_3, 0, 2, 0}
};

static const symbol s_4_0[4] = { 0xD8, 0xA3, 0xD8, 0xA2 };
static const symbol s_4_1[4] = { 0xD8, 0xA3, 0xD8, 0xA3 };
static const symbol s_4_2[4] = { 0xD8, 0xA3, 0xD8, 0xA4 };
static const symbol s_4_3[4] = { 0xD8, 0xA3, 0xD8, 0xA5 };
static const symbol s_4_4[4] = { 0xD8, 0xA3, 0xD8, 0xA7 };
static const struct among a_4[5] = {
{ 4, s_4_0, 0, 2, 0},
{ 4, s_4_1, 0, 1, 0},
{ 4, s_4_2, 0, 1, 0},
{ 4, s_4_3, 0, 4, 0},
{ 4, s_4_4, 0, 3, 0}
};

static const symbol s_5_0[2] = { 0xD9, 0x81 };
static const symbol s_5_1[2] = { 0xD9, 0x88 };
static const struct among a_5[2] = {
{ 2, s_5_0, 0, 1, 0},
{ 2, s_5_1, 0, 1, 0}
};

static const symbol s_6_0[4] = { 0xD8, 0xA7, 0xD9, 0x84 };
static const symbol s_6_1[6] = { 0xD8, 0xA8, 0xD8, 0xA7, 0xD9, 0x84 };
static const symbol s_6_2[6] = { 0xD9, 0x83, 0xD8, 0xA7, 0xD9, 0x84 };
static const symbol s_6_3[4] = { 0xD9, 0x84, 0xD9, 0x84 };
static const struct among a_6[4] = {
{ 4, s_6_0, 0, 2, 0},
{ 6, s_6_1, 0, 1, 0},
{ 6, s_6_2, 0, 1, 0},
{ 4, s_6_3, 0, 2, 0}
};

static const symbol s_7_0[2] = { 0xD8, 0xA8 };
static const symbol s_7_1[4] = { 0xD8, 0xA8, 0xD8, 0xA7 };
static const symbol s_7_2[4] = { 0xD8, 0xA8, 0xD8, 0xA8 };
static const symbol s_7_3[4] = { 0xD9, 0x83, 0xD9, 0x83 };
static const struct among a_7[4] = {
{ 2, s_7_0, 0, 1, 0},
{ 4, s_7_1, -1, -1, 0},
{ 4, s_7_2, -2, 2, 0},
{ 4, s_7_3, 0, 3, 0}
};

static const symbol s_8_0[4] = { 0xD8, 0xB3, 0xD8, 0xA3 };
static const symbol s_8_1[4] = { 0xD8, 0xB3, 0xD8, 0xAA };
static const symbol s_8_2[4] = { 0xD8, 0xB3, 0xD9, 0x86 };
static const symbol s_8_3[4] = { 0xD8, 0xB3, 0xD9, 0x8A };
static const struct among a_8[4] = {
{ 4, s_8_0, 0, 4, 0},
{ 4, s_8_1, 0, 2, 0},
{ 4, s_8_2, 0, 3, 0},
{ 4, s_8_3, 0, 1, 0}
};

static const symbol s_9_0[6] = { 0xD8, 0xAA, 0xD8, 0xB3, 0xD8, 0xAA };
static const symbol s_9_1[6] = { 0xD9, 0x86, 0xD8, 0xB3, 0xD8, 0xAA };
static const symbol s_9_2[6] = { 0xD9, 0x8A, 0xD8, 0xB3, 0xD8, 0xAA };
static const struct among a_9[3] = {
{ 6, s_9_0, 0, 1, 0},
{ 6, s_9_1, 0, 1, 0},
{ 6, s_9_2, 0, 1, 0}
};

static const symbol s_10_0[2] = { 0xD9, 0x83 };
static const symbol s_10_1[4] = { 0xD9, 0x83, 0xD9, 0x85 };
static const symbol s_10_2[4] = { 0xD9, 0x87, 0xD9, 0x85 };
static const symbol s_10_3[4] = { 0xD9, 0x87, 0xD9, 0x86 };
static const symbol s_10_4[2] = { 0xD9, 0x87 };
static const symbol s_10_5[2] = { 0xD9, 0x8A };
static const symbol s_10_6[6] = { 0xD9, 0x83, 0xD9, 0x85, 0xD8, 0xA7 };
static const symbol s_10_7[6] = { 0xD9, 0x87, 0xD9, 0x85, 0xD8, 0xA7 };
static const symbol s_10_8[4] = { 0xD9, 0x86, 0xD8, 0xA7 };
static const symbol s_10_9[4] = { 0xD9, 0x87, 0xD8, 0xA7 };
static const struct among a_10[10] = {
{ 2, s_10_0, 0, 1, 0},
{ 4, s_10_1, 0, 2, 0},
{ 4, s_10_2, 0, 2, 0},
{ 4, s_10_3, 0, 2, 0},
{ 2, s_10_4, 0, 1, 0},
{ 2, s_10_5, 0, 1, 0},
{ 6, s_10_6, 0, 3, 0},
{ 6, s_10_7, 0, 3, 0},
{ 4, s_10_8, 0, 2, 0},
{ 4, s_10_9, 0, 2, 0}
};

static const symbol s_11_0[2] = { 0xD9, 0x88 };
static const symbol s_11_1[2] = { 0xD9, 0x8A };
static const symbol s_11_2[2] = { 0xD8, 0xA7 };
static const struct among a_11[3] = {
{ 2, s_11_0, 0, 1, 0},
{ 2, s_11_1, 0, 1, 0},
{ 2, s_11_2, 0, 1, 0}
};

static const symbol s_12_0[2] = { 0xD9, 0x83 };
static const symbol s_12_1[4] = { 0xD9, 0x83, 0xD9, 0x85 };
static const symbol s_12_2[4] = { 0xD9, 0x87, 0xD9, 0x85 };
static const symbol s_12_3[4] = { 0xD9, 0x83, 0xD9, 0x86 };
static const symbol s_12_4[4] = { 0xD9, 0x87, 0xD9, 0x86 };
static const symbol s_12_5[2] = { 0xD9, 0x87 };
static const symbol s_12_6[6] = { 0xD9, 0x83, 0xD9, 0x85, 0xD9, 0x88 };
static const symbol s_12_7[4] = { 0xD9, 0x86, 0xD9, 0x8A };
static const symbol s_12_8[6] = { 0xD9, 0x83, 0xD9, 0x85, 0xD8, 0xA7 };
static const symbol s_12_9[6] = { 0xD9, 0x87, 0xD9, 0x85, 0xD8, 0xA7 };
static const symbol s_12_10[4] = { 0xD9, 0x86, 0xD8, 0xA7 };
static const symbol s_12_11[4] = { 0xD9, 0x87, 0xD8, 0xA7 };
static const struct among a_12[12] = {
{ 2, s_12_0, 0, 1, 0},
{ 4, s_12_1, 0, 2, 0},
{ 4, s_12_2, 0, 2, 0},
{ 4, s_12_3, 0, 2, 0},
{ 4, s_12_4, 0, 2, 0},
{ 2, s_12_5, 0, 1, 0},
{ 6, s_12_6, 0, 3, 0},
{ 4, s_12_7, 0, 2, 0},
{ 6, s_12_8, 0, 3, 0},
{ 6, s_12_9, 0, 3, 0},
{ 4, s_12_10, 0, 2, 0},
{ 4, s_12_11, 0, 2, 0}
};

static const symbol s_13_0[2] = { 0xD9, 0x86 };
static const symbol s_13_1[4] = { 0xD9, 0x88, 0xD9, 0x86 };
static const symbol s_13_2[4] = { 0xD9, 0x8A, 0xD9, 0x86 };
static const symbol s_13_3[4] = { 0xD8, 0xA7, 0xD9, 0x86 };
static const symbol s_13_4[4] = { 0xD8, 0xAA, 0xD9, 0x86 };
static const symbol s_13_5[2] = { 0xD9, 0x8A };
static const symbol s_13_6[2] = { 0xD8, 0xA7 };
static const symbol s_13_7[6] = { 0xD8, 0xAA, 0xD9, 0x85, 0xD8, 0xA7 };
static const symbol s_13_8[4] = { 0xD9, 0x86, 0xD8, 0xA7 };
static const symbol s_13_9[4] = { 0xD8, 0xAA, 0xD8, 0xA7 };
static const symbol s_13_10[2] = { 0xD8, 0xAA };
static const struct among a_13[11] = {
{ 2, s_13_0, 0, 1, 0},
{ 4, s_13_1, -1, 3, 0},
{ 4, s_13_2, -2, 3, 0},
{ 4, s_13_3, -3, 3, 0},
{ 4, s_13_4, -4, 2, 0},
{ 2, s_13_5, 0, 1, 0},
{ 2, s_13_6, 0, 1, 0},
{ 6, s_13_7, -1, 4, 0},
{ 4, s_13_8, -2, 2, 0},
{ 4, s_13_9, -3, 2, 0},
{ 2, s_13_10, 0, 1, 0}
};

static const symbol s_14_0[4] = { 0xD8, 0xAA, 0xD9, 0x85 };
static const symbol s_14_1[4] = { 0xD9, 0x88, 0xD8, 0xA7 };
static const struct among a_14[2] = {
{ 4, s_14_0, 0, 1, 0},
{ 4, s_14_1, 0, 1, 0}
};

static const symbol s_15_0[2] = { 0xD9, 0x88 };
static const symbol s_15_1[6] = { 0xD8, 0xAA, 0xD9, 0x85, 0xD9, 0x88 };
static const struct among a_15[2] = {
{ 2, s_15_0, 0, 1, 0},
{ 6, s_15_1, -1, 2, 0}
};

static int r_Normalize_pre(struct SN_env * z) {
    int among_var;
    {
        int v_1 = z->c;
        while (1) {
            int v_2 = z->c;
            do {
                int v_3 = z->c;
                z->bra = z->c;
                among_var = find_among(z, a_0, 144, 0);
                if (!among_var) goto lab2;
                z->ket = z->c;
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
                        {
                            int ret = slice_from_s(z, 1, s_1);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 4:
                        {
                            int ret = slice_from_s(z, 1, s_2);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 5:
                        {
                            int ret = slice_from_s(z, 1, s_3);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 6:
                        {
                            int ret = slice_from_s(z, 1, s_4);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 7:
                        {
                            int ret = slice_from_s(z, 1, s_5);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 8:
                        {
                            int ret = slice_from_s(z, 1, s_6);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 9:
                        {
                            int ret = slice_from_s(z, 1, s_7);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 10:
                        {
                            int ret = slice_from_s(z, 1, s_8);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 11:
                        {
                            int ret = slice_from_s(z, 1, s_9);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 12:
                        {
                            int ret = slice_from_s(z, 2, s_10);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 13:
                        {
                            int ret = slice_from_s(z, 2, s_11);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 14:
                        {
                            int ret = slice_from_s(z, 2, s_12);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 15:
                        {
                            int ret = slice_from_s(z, 2, s_13);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 16:
                        {
                            int ret = slice_from_s(z, 2, s_14);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 17:
                        {
                            int ret = slice_from_s(z, 2, s_15);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 18:
                        {
                            int ret = slice_from_s(z, 2, s_16);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 19:
                        {
                            int ret = slice_from_s(z, 2, s_17);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 20:
                        {
                            int ret = slice_from_s(z, 2, s_18);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 21:
                        {
                            int ret = slice_from_s(z, 2, s_19);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 22:
                        {
                            int ret = slice_from_s(z, 2, s_20);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 23:
                        {
                            int ret = slice_from_s(z, 2, s_21);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 24:
                        {
                            int ret = slice_from_s(z, 2, s_22);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 25:
                        {
                            int ret = slice_from_s(z, 2, s_23);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 26:
                        {
                            int ret = slice_from_s(z, 2, s_24);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 27:
                        {
                            int ret = slice_from_s(z, 2, s_25);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 28:
                        {
                            int ret = slice_from_s(z, 2, s_26);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 29:
                        {
                            int ret = slice_from_s(z, 2, s_27);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 30:
                        {
                            int ret = slice_from_s(z, 2, s_28);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 31:
                        {
                            int ret = slice_from_s(z, 2, s_29);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 32:
                        {
                            int ret = slice_from_s(z, 2, s_30);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 33:
                        {
                            int ret = slice_from_s(z, 2, s_31);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 34:
                        {
                            int ret = slice_from_s(z, 2, s_32);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 35:
                        {
                            int ret = slice_from_s(z, 2, s_33);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 36:
                        {
                            int ret = slice_from_s(z, 2, s_34);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 37:
                        {
                            int ret = slice_from_s(z, 2, s_35);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 38:
                        {
                            int ret = slice_from_s(z, 2, s_36);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 39:
                        {
                            int ret = slice_from_s(z, 2, s_37);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 40:
                        {
                            int ret = slice_from_s(z, 2, s_38);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 41:
                        {
                            int ret = slice_from_s(z, 2, s_39);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 42:
                        {
                            int ret = slice_from_s(z, 2, s_40);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 43:
                        {
                            int ret = slice_from_s(z, 2, s_41);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 44:
                        {
                            int ret = slice_from_s(z, 2, s_42);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 45:
                        {
                            int ret = slice_from_s(z, 2, s_43);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 46:
                        {
                            int ret = slice_from_s(z, 2, s_44);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 47:
                        {
                            int ret = slice_from_s(z, 2, s_45);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 48:
                        {
                            int ret = slice_from_s(z, 4, s_46);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 49:
                        {
                            int ret = slice_from_s(z, 4, s_47);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 50:
                        {
                            int ret = slice_from_s(z, 4, s_48);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 51:
                        {
                            int ret = slice_from_s(z, 4, s_49);
                            if (ret < 0) return ret;
                        }
                        break;
                }
                break;
            lab2:
                z->c = v_3;
                {
                    int ret = skip_utf8(z->p, z->c, z->l, 1);
                    if (ret < 0) goto lab1;
                    z->c = ret;
                }
            } while (0);
            continue;
        lab1:
            z->c = v_2;
            break;
        }
        z->c = v_1;
    }
    return 1;
}

static int r_Normalize_post(struct SN_env * z) {
    int among_var;
    {
        int v_1 = z->c;
        z->lb = z->c; z->c = z->l;
        z->ket = z->c;
        if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 5 || !((124 >> (z->p[z->c - 1] & 0x1f)) & 1)) goto lab0;
        if (!find_among_b(z, a_1, 5, 0)) goto lab0;
        z->bra = z->c;
        {
            int ret = slice_from_s(z, 2, s_50);
            if (ret < 0) return ret;
        }
        z->c = z->lb;
    lab0:
        z->c = v_1;
    }
    {
        int v_2 = z->c;
        while (1) {
            int v_3 = z->c;
            do {
                int v_4 = z->c;
                z->bra = z->c;
                if (z->c + 1 >= z->l || z->p[z->c + 1] >> 5 != 5 || !((124 >> (z->p[z->c + 1] & 0x1f)) & 1)) goto lab3;
                among_var = find_among(z, a_2, 5, 0);
                if (!among_var) goto lab3;
                z->ket = z->c;
                switch (among_var) {
                    case 1:
                        {
                            int ret = slice_from_s(z, 2, s_51);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 2:
                        {
                            int ret = slice_from_s(z, 2, s_52);
                            if (ret < 0) return ret;
                        }
                        break;
                    case 3:
                        {
                            int ret = slice_from_s(z, 2, s_53);
                            if (ret < 0) return ret;
                        }
                        break;
                }
                break;
            lab3:
                z->c = v_4;
                {
                    int ret = skip_utf8(z->p, z->c, z->l, 1);
                    if (ret < 0) goto lab2;
                    z->c = ret;
                }
            } while (0);
            continue;
        lab2:
            z->c = v_3;
            break;
        }
        z->c = v_2;
    }
    return 1;
}

static int r_Checks1(struct SN_env * z) {
    int among_var;
    z->bra = z->c;
    if (z->c + 3 >= z->l || (z->p[z->c + 3] != 132 && z->p[z->c + 3] != 167)) return 0;
    among_var = find_among(z, a_3, 4, 0);
    if (!among_var) return 0;
    z->ket = z->c;
    switch (among_var) {
        case 1:
            if (len_utf8(z->p) <= 4) return 0;
            ((SN_local *)z)->b_is_noun = 1;
            ((SN_local *)z)->b_is_verb = 0;
            ((SN_local *)z)->b_is_defined = 1;
            break;
        case 2:
            if (len_utf8(z->p) <= 3) return 0;
            ((SN_local *)z)->b_is_noun = 1;
            ((SN_local *)z)->b_is_verb = 0;
            ((SN_local *)z)->b_is_defined = 1;
            break;
    }
    return 1;
}

static int r_Prefix_Step1(struct SN_env * z) {
    int among_var;
    z->bra = z->c;
    if (z->c + 3 >= z->l || z->p[z->c + 3] >> 5 != 5 || !((188 >> (z->p[z->c + 3] & 0x1f)) & 1)) return 0;
    among_var = find_among(z, a_4, 5, 0);
    if (!among_var) return 0;
    z->ket = z->c;
    switch (among_var) {
        case 1:
            if (len_utf8(z->p) <= 3) return 0;
            {
                int ret = slice_from_s(z, 2, s_54);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            if (len_utf8(z->p) <= 3) return 0;
            {
                int ret = slice_from_s(z, 2, s_55);
                if (ret < 0) return ret;
            }
            break;
        case 3:
            if (len_utf8(z->p) <= 3) return 0;
            {
                int ret = slice_from_s(z, 2, s_56);
                if (ret < 0) return ret;
            }
            break;
        case 4:
            if (len_utf8(z->p) <= 3) return 0;
            {
                int ret = slice_from_s(z, 2, s_57);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_Prefix_Step2(struct SN_env * z) {
    z->bra = z->c;
    if (z->c + 1 >= z->l || (z->p[z->c + 1] != 129 && z->p[z->c + 1] != 136)) return 0;
    if (!find_among(z, a_5, 2, 0)) return 0;
    z->ket = z->c;
    if (len_utf8(z->p) <= 3) return 0;
    {
        int v_1 = z->c;
        if (!(eq_s(z, 2, s_58))) goto lab0;
        return 0;
    lab0:
        z->c = v_1;
    }
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_Prefix_Step3a_Noun(struct SN_env * z) {
    int among_var;
    z->bra = z->c;
    if (z->c + 3 >= z->l || (z->p[z->c + 3] != 132 && z->p[z->c + 3] != 167)) return 0;
    among_var = find_among(z, a_6, 4, 0);
    if (!among_var) return 0;
    z->ket = z->c;
    switch (among_var) {
        case 1:
            if (len_utf8(z->p) <= 5) return 0;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            if (len_utf8(z->p) <= 4) return 0;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_Prefix_Step3b_Noun(struct SN_env * z) {
    int among_var;
    z->bra = z->c;
    if (z->c + 1 >= z->l || (z->p[z->c + 1] != 168 && z->p[z->c + 1] != 131)) return 0;
    among_var = find_among(z, a_7, 4, 0);
    if (!among_var) return 0;
    z->ket = z->c;
    switch (among_var) {
        case 1:
            if (len_utf8(z->p) <= 3) return 0;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            if (len_utf8(z->p) <= 3) return 0;
            {
                int ret = slice_from_s(z, 2, s_59);
                if (ret < 0) return ret;
            }
            break;
        case 3:
            if (len_utf8(z->p) <= 3) return 0;
            {
                int ret = slice_from_s(z, 2, s_60);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_Prefix_Step3_Verb(struct SN_env * z) {
    int among_var;
    z->bra = z->c;
    among_var = find_among(z, a_8, 4, 0);
    if (!among_var) return 0;
    z->ket = z->c;
    switch (among_var) {
        case 1:
            if (len_utf8(z->p) <= 4) return 0;
            {
                int ret = slice_from_s(z, 2, s_61);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            if (len_utf8(z->p) <= 4) return 0;
            {
                int ret = slice_from_s(z, 2, s_62);
                if (ret < 0) return ret;
            }
            break;
        case 3:
            if (len_utf8(z->p) <= 4) return 0;
            {
                int ret = slice_from_s(z, 2, s_63);
                if (ret < 0) return ret;
            }
            break;
        case 4:
            if (len_utf8(z->p) <= 4) return 0;
            {
                int ret = slice_from_s(z, 2, s_64);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_Prefix_Step4_Verb(struct SN_env * z) {
    z->bra = z->c;
    if (z->c + 5 >= z->l || z->p[z->c + 5] != 170) return 0;
    if (!find_among(z, a_9, 3, 0)) return 0;
    z->ket = z->c;
    if (len_utf8(z->p) <= 4) return 0;
    ((SN_local *)z)->b_is_verb = 1;
    ((SN_local *)z)->b_is_noun = 0;
    {
        int ret = slice_from_s(z, 6, s_65);
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_Suffix_Noun_Step1a(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    among_var = find_among_b(z, a_10, 10, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    switch (among_var) {
        case 1:
            if (len_utf8(z->p) < 4) return 0;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            if (len_utf8(z->p) < 5) return 0;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 3:
            if (len_utf8(z->p) < 6) return 0;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_Suffix_Noun_Step1b(struct SN_env * z) {
    z->ket = z->c;
    if (!(eq_s_b(z, 2, s_66))) return 0;
    z->bra = z->c;
    if (len_utf8(z->p) <= 5) return 0;
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_Suffix_Noun_Step2a(struct SN_env * z) {
    z->ket = z->c;
    if (!find_among_b(z, a_11, 3, 0)) return 0;
    z->bra = z->c;
    if (len_utf8(z->p) <= 4) return 0;
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_Suffix_Noun_Step2b(struct SN_env * z) {
    z->ket = z->c;
    if (!(eq_s_b(z, 4, s_67))) return 0;
    z->bra = z->c;
    if (len_utf8(z->p) < 5) return 0;
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_Suffix_Noun_Step2c1(struct SN_env * z) {
    z->ket = z->c;
    if (!(eq_s_b(z, 2, s_68))) return 0;
    z->bra = z->c;
    if (len_utf8(z->p) < 4) return 0;
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_Suffix_Noun_Step2c2(struct SN_env * z) {
    z->ket = z->c;
    if (!(eq_s_b(z, 2, s_69))) return 0;
    z->bra = z->c;
    if (len_utf8(z->p) < 4) return 0;
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_Suffix_Noun_Step3(struct SN_env * z) {
    z->ket = z->c;
    if (!(eq_s_b(z, 2, s_70))) return 0;
    z->bra = z->c;
    if (len_utf8(z->p) < 3) return 0;
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_Suffix_Verb_Step1(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    among_var = find_among_b(z, a_12, 12, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    switch (among_var) {
        case 1:
            if (len_utf8(z->p) < 4) return 0;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            if (len_utf8(z->p) < 5) return 0;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 3:
            if (len_utf8(z->p) < 6) return 0;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_Suffix_Verb_Step2a(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    among_var = find_among_b(z, a_13, 11, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    switch (among_var) {
        case 1:
            if (len_utf8(z->p) < 4) return 0;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            if (len_utf8(z->p) < 5) return 0;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 3:
            if (len_utf8(z->p) <= 5) return 0;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 4:
            if (len_utf8(z->p) < 6) return 0;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_Suffix_Verb_Step2b(struct SN_env * z) {
    z->ket = z->c;
    if (z->c - 3 <= z->lb || (z->p[z->c - 1] != 133 && z->p[z->c - 1] != 167)) return 0;
    if (!find_among_b(z, a_14, 2, 0)) return 0;
    z->bra = z->c;
    if (len_utf8(z->p) < 5) return 0;
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_Suffix_Verb_Step2c(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    if (z->c - 1 <= z->lb || z->p[z->c - 1] != 136) return 0;
    among_var = find_among_b(z, a_15, 2, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    switch (among_var) {
        case 1:
            if (len_utf8(z->p) < 4) return 0;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            if (len_utf8(z->p) < 6) return 0;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_Suffix_All_alef_maqsura(struct SN_env * z) {
    z->ket = z->c;
    if (!(eq_s_b(z, 2, s_71))) return 0;
    z->bra = z->c;
    {
        int ret = slice_from_s(z, 2, s_72);
        if (ret < 0) return ret;
    }
    return 1;
}

extern int arabic_UTF_8_stem(struct SN_env * z) {
    ((SN_local *)z)->b_is_noun = 1;
    ((SN_local *)z)->b_is_verb = 1;
    ((SN_local *)z)->b_is_defined = 0;
    {
        int v_1 = z->c;
        {
            int ret = r_Checks1(z);
            if (ret < 0) return ret;
        }
        z->c = v_1;
    }
    {
        int ret = r_Normalize_pre(z);
        if (ret < 0) return ret;
    }
    z->lb = z->c; z->c = z->l;
    {
        int v_2 = z->l - z->c;
        do {
            int v_3 = z->l - z->c;
            if (!((SN_local *)z)->b_is_verb) goto lab1;
            do {
                int v_4 = z->l - z->c;
                {
                    int v_5 = 1;
                    while (1) {
                        int v_6 = z->l - z->c;
                        {
                            int ret = r_Suffix_Verb_Step1(z);
                            if (ret == 0) goto lab3;
                            if (ret < 0) return ret;
                        }
                        v_5--;
                        continue;
                    lab3:
                        z->c = z->l - v_6;
                        break;
                    }
                    if (v_5 > 0) goto lab2;
                }
                do {
                    int v_7 = z->l - z->c;
                    {
                        int ret = r_Suffix_Verb_Step2a(z);
                        if (ret == 0) goto lab4;
                        if (ret < 0) return ret;
                    }
                    break;
                lab4:
                    z->c = z->l - v_7;
                    {
                        int ret = r_Suffix_Verb_Step2c(z);
                        if (ret == 0) goto lab5;
                        if (ret < 0) return ret;
                    }
                    break;
                lab5:
                    z->c = z->l - v_7;
                    {
                        int ret = skip_b_utf8(z->p, z->c, z->lb, 1);
                        if (ret < 0) goto lab2;
                        z->c = ret;
                    }
                } while (0);
                break;
            lab2:
                z->c = z->l - v_4;
                {
                    int ret = r_Suffix_Verb_Step2b(z);
                    if (ret == 0) goto lab6;
                    if (ret < 0) return ret;
                }
                break;
            lab6:
                z->c = z->l - v_4;
                {
                    int ret = r_Suffix_Verb_Step2a(z);
                    if (ret == 0) goto lab1;
                    if (ret < 0) return ret;
                }
            } while (0);
            break;
        lab1:
            z->c = z->l - v_3;
            if (!((SN_local *)z)->b_is_noun) goto lab7;
            {
                int v_8 = z->l - z->c;
                do {
                    int v_9 = z->l - z->c;
                    {
                        int ret = r_Suffix_Noun_Step2c2(z);
                        if (ret == 0) goto lab9;
                        if (ret < 0) return ret;
                    }
                    break;
                lab9:
                    z->c = z->l - v_9;
                    if (((SN_local *)z)->b_is_defined) goto lab10;
                    {
                        int ret = r_Suffix_Noun_Step1a(z);
                        if (ret == 0) goto lab10;
                        if (ret < 0) return ret;
                    }
                    do {
                        int v_10 = z->l - z->c;
                        {
                            int ret = r_Suffix_Noun_Step2a(z);
                            if (ret == 0) goto lab11;
                            if (ret < 0) return ret;
                        }
                        break;
                    lab11:
                        z->c = z->l - v_10;
                        {
                            int ret = r_Suffix_Noun_Step2b(z);
                            if (ret == 0) goto lab12;
                            if (ret < 0) return ret;
                        }
                        break;
                    lab12:
                        z->c = z->l - v_10;
                        {
                            int ret = r_Suffix_Noun_Step2c1(z);
                            if (ret == 0) goto lab13;
                            if (ret < 0) return ret;
                        }
                        break;
                    lab13:
                        z->c = z->l - v_10;
                        {
                            int ret = skip_b_utf8(z->p, z->c, z->lb, 1);
                            if (ret < 0) goto lab10;
                            z->c = ret;
                        }
                    } while (0);
                    break;
                lab10:
                    z->c = z->l - v_9;
                    {
                        int ret = r_Suffix_Noun_Step1b(z);
                        if (ret == 0) goto lab14;
                        if (ret < 0) return ret;
                    }
                    do {
                        int v_11 = z->l - z->c;
                        {
                            int ret = r_Suffix_Noun_Step2a(z);
                            if (ret == 0) goto lab15;
                            if (ret < 0) return ret;
                        }
                        break;
                    lab15:
                        z->c = z->l - v_11;
                        {
                            int ret = r_Suffix_Noun_Step2b(z);
                            if (ret == 0) goto lab16;
                            if (ret < 0) return ret;
                        }
                        break;
                    lab16:
                        z->c = z->l - v_11;
                        {
                            int ret = r_Suffix_Noun_Step2c1(z);
                            if (ret == 0) goto lab14;
                            if (ret < 0) return ret;
                        }
                    } while (0);
                    break;
                lab14:
                    z->c = z->l - v_9;
                    if (((SN_local *)z)->b_is_defined) goto lab17;
                    {
                        int ret = r_Suffix_Noun_Step2a(z);
                        if (ret == 0) goto lab17;
                        if (ret < 0) return ret;
                    }
                    break;
                lab17:
                    z->c = z->l - v_9;
                    {
                        int ret = r_Suffix_Noun_Step2b(z);
                        if (ret == 0) { z->c = z->l - v_8; goto lab8; }
                        if (ret < 0) return ret;
                    }
                } while (0);
            lab8:
                ;
            }
            {
                int ret = r_Suffix_Noun_Step3(z);
                if (ret == 0) goto lab7;
                if (ret < 0) return ret;
            }
            break;
        lab7:
            z->c = z->l - v_3;
            {
                int ret = r_Suffix_All_alef_maqsura(z);
                if (ret == 0) goto lab0;
                if (ret < 0) return ret;
            }
        } while (0);
    lab0:
        z->c = z->l - v_2;
    }
    z->c = z->lb;
    {
        int v_12 = z->c;
        {
            int v_13 = z->c;
            {
                int ret = r_Prefix_Step1(z);
                if (ret == 0) { z->c = v_13; goto lab19; }
                if (ret < 0) return ret;
            }
        lab19:
            ;
        }
        {
            int v_14 = z->c;
            {
                int ret = r_Prefix_Step2(z);
                if (ret == 0) { z->c = v_14; goto lab20; }
                if (ret < 0) return ret;
            }
        lab20:
            ;
        }
        do {
            int v_15 = z->c;
            {
                int ret = r_Prefix_Step3a_Noun(z);
                if (ret == 0) goto lab21;
                if (ret < 0) return ret;
            }
            break;
        lab21:
            z->c = v_15;
            if (!((SN_local *)z)->b_is_noun) goto lab22;
            {
                int ret = r_Prefix_Step3b_Noun(z);
                if (ret == 0) goto lab22;
                if (ret < 0) return ret;
            }
            break;
        lab22:
            z->c = v_15;
            if (!((SN_local *)z)->b_is_verb) goto lab18;
            {
                int v_16 = z->c;
                {
                    int ret = r_Prefix_Step3_Verb(z);
                    if (ret == 0) { z->c = v_16; goto lab23; }
                    if (ret < 0) return ret;
                }
            lab23:
                ;
            }
            {
                int ret = r_Prefix_Step4_Verb(z);
                if (ret == 0) goto lab18;
                if (ret < 0) return ret;
            }
        } while (0);
    lab18:
        z->c = v_12;
    }
    {
        int ret = r_Normalize_post(z);
        if (ret < 0) return ret;
    }
    return 1;
}

extern struct SN_env * arabic_UTF_8_create_env(void) {
    struct SN_env * z = SN_new_env(sizeof(SN_local));
    if (z) {
        ((SN_local *)z)->b_is_defined = 0;
        ((SN_local *)z)->b_is_verb = 0;
        ((SN_local *)z)->b_is_noun = 0;
    }
    return z;
}

extern void arabic_UTF_8_close_env(struct SN_env * z) {
    SN_delete_env(z);
}

