/* This file was generated automatically by the Snowball to ISO C compiler */
/* http://snowballstem.org/ */

#include "header.h"

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
#ifdef __cplusplus
extern "C" {
#endif


extern struct SN_env * arabic_UTF_8_create_env(void);
extern void arabic_UTF_8_close_env(struct SN_env * z);


#ifdef __cplusplus
}
#endif
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

static const struct among a_0[144] =
{
/*  0 */ { 2, s_0_0, -1, 1, 0},
/*  1 */ { 2, s_0_1, -1, 1, 0},
/*  2 */ { 2, s_0_2, -1, 1, 0},
/*  3 */ { 2, s_0_3, -1, 1, 0},
/*  4 */ { 2, s_0_4, -1, 1, 0},
/*  5 */ { 2, s_0_5, -1, 1, 0},
/*  6 */ { 2, s_0_6, -1, 1, 0},
/*  7 */ { 2, s_0_7, -1, 1, 0},
/*  8 */ { 2, s_0_8, -1, 1, 0},
/*  9 */ { 2, s_0_9, -1, 2, 0},
/* 10 */ { 2, s_0_10, -1, 3, 0},
/* 11 */ { 2, s_0_11, -1, 4, 0},
/* 12 */ { 2, s_0_12, -1, 5, 0},
/* 13 */ { 2, s_0_13, -1, 6, 0},
/* 14 */ { 2, s_0_14, -1, 7, 0},
/* 15 */ { 2, s_0_15, -1, 8, 0},
/* 16 */ { 2, s_0_16, -1, 9, 0},
/* 17 */ { 2, s_0_17, -1, 10, 0},
/* 18 */ { 2, s_0_18, -1, 11, 0},
/* 19 */ { 3, s_0_19, -1, 12, 0},
/* 20 */ { 3, s_0_20, -1, 16, 0},
/* 21 */ { 3, s_0_21, -1, 16, 0},
/* 22 */ { 3, s_0_22, -1, 13, 0},
/* 23 */ { 3, s_0_23, -1, 13, 0},
/* 24 */ { 3, s_0_24, -1, 17, 0},
/* 25 */ { 3, s_0_25, -1, 17, 0},
/* 26 */ { 3, s_0_26, -1, 14, 0},
/* 27 */ { 3, s_0_27, -1, 14, 0},
/* 28 */ { 3, s_0_28, -1, 15, 0},
/* 29 */ { 3, s_0_29, -1, 15, 0},
/* 30 */ { 3, s_0_30, -1, 15, 0},
/* 31 */ { 3, s_0_31, -1, 15, 0},
/* 32 */ { 3, s_0_32, -1, 18, 0},
/* 33 */ { 3, s_0_33, -1, 18, 0},
/* 34 */ { 3, s_0_34, -1, 19, 0},
/* 35 */ { 3, s_0_35, -1, 19, 0},
/* 36 */ { 3, s_0_36, -1, 19, 0},
/* 37 */ { 3, s_0_37, -1, 19, 0},
/* 38 */ { 3, s_0_38, -1, 20, 0},
/* 39 */ { 3, s_0_39, -1, 20, 0},
/* 40 */ { 3, s_0_40, -1, 21, 0},
/* 41 */ { 3, s_0_41, -1, 21, 0},
/* 42 */ { 3, s_0_42, -1, 21, 0},
/* 43 */ { 3, s_0_43, -1, 21, 0},
/* 44 */ { 3, s_0_44, -1, 22, 0},
/* 45 */ { 3, s_0_45, -1, 22, 0},
/* 46 */ { 3, s_0_46, -1, 22, 0},
/* 47 */ { 3, s_0_47, -1, 22, 0},
/* 48 */ { 3, s_0_48, -1, 23, 0},
/* 49 */ { 3, s_0_49, -1, 23, 0},
/* 50 */ { 3, s_0_50, -1, 23, 0},
/* 51 */ { 3, s_0_51, -1, 23, 0},
/* 52 */ { 3, s_0_52, -1, 24, 0},
/* 53 */ { 3, s_0_53, -1, 24, 0},
/* 54 */ { 3, s_0_54, -1, 24, 0},
/* 55 */ { 3, s_0_55, -1, 24, 0},
/* 56 */ { 3, s_0_56, -1, 25, 0},
/* 57 */ { 3, s_0_57, -1, 25, 0},
/* 58 */ { 3, s_0_58, -1, 25, 0},
/* 59 */ { 3, s_0_59, -1, 25, 0},
/* 60 */ { 3, s_0_60, -1, 26, 0},
/* 61 */ { 3, s_0_61, -1, 26, 0},
/* 62 */ { 3, s_0_62, -1, 27, 0},
/* 63 */ { 3, s_0_63, -1, 27, 0},
/* 64 */ { 3, s_0_64, -1, 28, 0},
/* 65 */ { 3, s_0_65, -1, 28, 0},
/* 66 */ { 3, s_0_66, -1, 29, 0},
/* 67 */ { 3, s_0_67, -1, 29, 0},
/* 68 */ { 3, s_0_68, -1, 30, 0},
/* 69 */ { 3, s_0_69, -1, 30, 0},
/* 70 */ { 3, s_0_70, -1, 30, 0},
/* 71 */ { 3, s_0_71, -1, 30, 0},
/* 72 */ { 3, s_0_72, -1, 31, 0},
/* 73 */ { 3, s_0_73, -1, 31, 0},
/* 74 */ { 3, s_0_74, -1, 31, 0},
/* 75 */ { 3, s_0_75, -1, 31, 0},
/* 76 */ { 3, s_0_76, -1, 32, 0},
/* 77 */ { 3, s_0_77, -1, 32, 0},
/* 78 */ { 3, s_0_78, -1, 32, 0},
/* 79 */ { 3, s_0_79, -1, 32, 0},
/* 80 */ { 3, s_0_80, -1, 33, 0},
/* 81 */ { 3, s_0_81, -1, 33, 0},
/* 82 */ { 3, s_0_82, -1, 33, 0},
/* 83 */ { 3, s_0_83, -1, 33, 0},
/* 84 */ { 3, s_0_84, -1, 34, 0},
/* 85 */ { 3, s_0_85, -1, 34, 0},
/* 86 */ { 3, s_0_86, -1, 34, 0},
/* 87 */ { 3, s_0_87, -1, 34, 0},
/* 88 */ { 3, s_0_88, -1, 35, 0},
/* 89 */ { 3, s_0_89, -1, 35, 0},
/* 90 */ { 3, s_0_90, -1, 35, 0},
/* 91 */ { 3, s_0_91, -1, 35, 0},
/* 92 */ { 3, s_0_92, -1, 36, 0},
/* 93 */ { 3, s_0_93, -1, 36, 0},
/* 94 */ { 3, s_0_94, -1, 36, 0},
/* 95 */ { 3, s_0_95, -1, 36, 0},
/* 96 */ { 3, s_0_96, -1, 37, 0},
/* 97 */ { 3, s_0_97, -1, 37, 0},
/* 98 */ { 3, s_0_98, -1, 37, 0},
/* 99 */ { 3, s_0_99, -1, 37, 0},
/*100 */ { 3, s_0_100, -1, 38, 0},
/*101 */ { 3, s_0_101, -1, 38, 0},
/*102 */ { 3, s_0_102, -1, 38, 0},
/*103 */ { 3, s_0_103, -1, 38, 0},
/*104 */ { 3, s_0_104, -1, 39, 0},
/*105 */ { 3, s_0_105, -1, 39, 0},
/*106 */ { 3, s_0_106, -1, 39, 0},
/*107 */ { 3, s_0_107, -1, 39, 0},
/*108 */ { 3, s_0_108, -1, 40, 0},
/*109 */ { 3, s_0_109, -1, 40, 0},
/*110 */ { 3, s_0_110, -1, 40, 0},
/*111 */ { 3, s_0_111, -1, 40, 0},
/*112 */ { 3, s_0_112, -1, 41, 0},
/*113 */ { 3, s_0_113, -1, 41, 0},
/*114 */ { 3, s_0_114, -1, 41, 0},
/*115 */ { 3, s_0_115, -1, 41, 0},
/*116 */ { 3, s_0_116, -1, 42, 0},
/*117 */ { 3, s_0_117, -1, 42, 0},
/*118 */ { 3, s_0_118, -1, 42, 0},
/*119 */ { 3, s_0_119, -1, 42, 0},
/*120 */ { 3, s_0_120, -1, 43, 0},
/*121 */ { 3, s_0_121, -1, 43, 0},
/*122 */ { 3, s_0_122, -1, 43, 0},
/*123 */ { 3, s_0_123, -1, 43, 0},
/*124 */ { 3, s_0_124, -1, 44, 0},
/*125 */ { 3, s_0_125, -1, 44, 0},
/*126 */ { 3, s_0_126, -1, 44, 0},
/*127 */ { 3, s_0_127, -1, 44, 0},
/*128 */ { 3, s_0_128, -1, 45, 0},
/*129 */ { 3, s_0_129, -1, 45, 0},
/*130 */ { 3, s_0_130, -1, 46, 0},
/*131 */ { 3, s_0_131, -1, 46, 0},
/*132 */ { 3, s_0_132, -1, 47, 0},
/*133 */ { 3, s_0_133, -1, 47, 0},
/*134 */ { 3, s_0_134, -1, 47, 0},
/*135 */ { 3, s_0_135, -1, 47, 0},
/*136 */ { 3, s_0_136, -1, 51, 0},
/*137 */ { 3, s_0_137, -1, 51, 0},
/*138 */ { 3, s_0_138, -1, 49, 0},
/*139 */ { 3, s_0_139, -1, 49, 0},
/*140 */ { 3, s_0_140, -1, 50, 0},
/*141 */ { 3, s_0_141, -1, 50, 0},
/*142 */ { 3, s_0_142, -1, 48, 0},
/*143 */ { 3, s_0_143, -1, 48, 0}
};

static const symbol s_1_0[2] = { 0xD8, 0xA2 };
static const symbol s_1_1[2] = { 0xD8, 0xA3 };
static const symbol s_1_2[2] = { 0xD8, 0xA4 };
static const symbol s_1_3[2] = { 0xD8, 0xA5 };
static const symbol s_1_4[2] = { 0xD8, 0xA6 };

static const struct among a_1[5] =
{
/*  0 */ { 2, s_1_0, -1, 1, 0},
/*  1 */ { 2, s_1_1, -1, 1, 0},
/*  2 */ { 2, s_1_2, -1, 1, 0},
/*  3 */ { 2, s_1_3, -1, 1, 0},
/*  4 */ { 2, s_1_4, -1, 1, 0}
};

static const symbol s_2_0[2] = { 0xD8, 0xA2 };
static const symbol s_2_1[2] = { 0xD8, 0xA3 };
static const symbol s_2_2[2] = { 0xD8, 0xA4 };
static const symbol s_2_3[2] = { 0xD8, 0xA5 };
static const symbol s_2_4[2] = { 0xD8, 0xA6 };

static const struct among a_2[5] =
{
/*  0 */ { 2, s_2_0, -1, 1, 0},
/*  1 */ { 2, s_2_1, -1, 1, 0},
/*  2 */ { 2, s_2_2, -1, 2, 0},
/*  3 */ { 2, s_2_3, -1, 1, 0},
/*  4 */ { 2, s_2_4, -1, 3, 0}
};

static const symbol s_3_0[4] = { 0xD8, 0xA7, 0xD9, 0x84 };
static const symbol s_3_1[6] = { 0xD8, 0xA8, 0xD8, 0xA7, 0xD9, 0x84 };
static const symbol s_3_2[6] = { 0xD9, 0x83, 0xD8, 0xA7, 0xD9, 0x84 };
static const symbol s_3_3[4] = { 0xD9, 0x84, 0xD9, 0x84 };

static const struct among a_3[4] =
{
/*  0 */ { 4, s_3_0, -1, 2, 0},
/*  1 */ { 6, s_3_1, -1, 1, 0},
/*  2 */ { 6, s_3_2, -1, 1, 0},
/*  3 */ { 4, s_3_3, -1, 2, 0}
};

static const symbol s_4_0[4] = { 0xD8, 0xA3, 0xD8, 0xA2 };
static const symbol s_4_1[4] = { 0xD8, 0xA3, 0xD8, 0xA3 };
static const symbol s_4_2[4] = { 0xD8, 0xA3, 0xD8, 0xA4 };
static const symbol s_4_3[4] = { 0xD8, 0xA3, 0xD8, 0xA5 };
static const symbol s_4_4[4] = { 0xD8, 0xA3, 0xD8, 0xA7 };

static const struct among a_4[5] =
{
/*  0 */ { 4, s_4_0, -1, 2, 0},
/*  1 */ { 4, s_4_1, -1, 1, 0},
/*  2 */ { 4, s_4_2, -1, 1, 0},
/*  3 */ { 4, s_4_3, -1, 4, 0},
/*  4 */ { 4, s_4_4, -1, 3, 0}
};

static const symbol s_5_0[2] = { 0xD9, 0x81 };
static const symbol s_5_1[2] = { 0xD9, 0x88 };

static const struct among a_5[2] =
{
/*  0 */ { 2, s_5_0, -1, 1, 0},
/*  1 */ { 2, s_5_1, -1, 1, 0}
};

static const symbol s_6_0[4] = { 0xD8, 0xA7, 0xD9, 0x84 };
static const symbol s_6_1[6] = { 0xD8, 0xA8, 0xD8, 0xA7, 0xD9, 0x84 };
static const symbol s_6_2[6] = { 0xD9, 0x83, 0xD8, 0xA7, 0xD9, 0x84 };
static const symbol s_6_3[4] = { 0xD9, 0x84, 0xD9, 0x84 };

static const struct among a_6[4] =
{
/*  0 */ { 4, s_6_0, -1, 2, 0},
/*  1 */ { 6, s_6_1, -1, 1, 0},
/*  2 */ { 6, s_6_2, -1, 1, 0},
/*  3 */ { 4, s_6_3, -1, 2, 0}
};

static const symbol s_7_0[2] = { 0xD8, 0xA8 };
static const symbol s_7_1[4] = { 0xD8, 0xA8, 0xD8, 0xA8 };
static const symbol s_7_2[4] = { 0xD9, 0x83, 0xD9, 0x83 };

static const struct among a_7[3] =
{
/*  0 */ { 2, s_7_0, -1, 1, 0},
/*  1 */ { 4, s_7_1, 0, 2, 0},
/*  2 */ { 4, s_7_2, -1, 3, 0}
};

static const symbol s_8_0[4] = { 0xD8, 0xB3, 0xD8, 0xA3 };
static const symbol s_8_1[4] = { 0xD8, 0xB3, 0xD8, 0xAA };
static const symbol s_8_2[4] = { 0xD8, 0xB3, 0xD9, 0x86 };
static const symbol s_8_3[4] = { 0xD8, 0xB3, 0xD9, 0x8A };

static const struct among a_8[4] =
{
/*  0 */ { 4, s_8_0, -1, 4, 0},
/*  1 */ { 4, s_8_1, -1, 2, 0},
/*  2 */ { 4, s_8_2, -1, 3, 0},
/*  3 */ { 4, s_8_3, -1, 1, 0}
};

static const symbol s_9_0[6] = { 0xD8, 0xAA, 0xD8, 0xB3, 0xD8, 0xAA };
static const symbol s_9_1[6] = { 0xD9, 0x86, 0xD8, 0xB3, 0xD8, 0xAA };
static const symbol s_9_2[6] = { 0xD9, 0x8A, 0xD8, 0xB3, 0xD8, 0xAA };

static const struct among a_9[3] =
{
/*  0 */ { 6, s_9_0, -1, 1, 0},
/*  1 */ { 6, s_9_1, -1, 1, 0},
/*  2 */ { 6, s_9_2, -1, 1, 0}
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

static const struct among a_10[10] =
{
/*  0 */ { 2, s_10_0, -1, 1, 0},
/*  1 */ { 4, s_10_1, -1, 2, 0},
/*  2 */ { 4, s_10_2, -1, 2, 0},
/*  3 */ { 4, s_10_3, -1, 2, 0},
/*  4 */ { 2, s_10_4, -1, 1, 0},
/*  5 */ { 2, s_10_5, -1, 1, 0},
/*  6 */ { 6, s_10_6, -1, 3, 0},
/*  7 */ { 6, s_10_7, -1, 3, 0},
/*  8 */ { 4, s_10_8, -1, 2, 0},
/*  9 */ { 4, s_10_9, -1, 2, 0}
};

static const symbol s_11_0[2] = { 0xD9, 0x86 };

static const struct among a_11[1] =
{
/*  0 */ { 2, s_11_0, -1, 1, 0}
};

static const symbol s_12_0[2] = { 0xD9, 0x88 };
static const symbol s_12_1[2] = { 0xD9, 0x8A };
static const symbol s_12_2[2] = { 0xD8, 0xA7 };

static const struct among a_12[3] =
{
/*  0 */ { 2, s_12_0, -1, 1, 0},
/*  1 */ { 2, s_12_1, -1, 1, 0},
/*  2 */ { 2, s_12_2, -1, 1, 0}
};

static const symbol s_13_0[4] = { 0xD8, 0xA7, 0xD8, 0xAA };

static const struct among a_13[1] =
{
/*  0 */ { 4, s_13_0, -1, 1, 0}
};

static const symbol s_14_0[2] = { 0xD8, 0xAA };

static const struct among a_14[1] =
{
/*  0 */ { 2, s_14_0, -1, 1, 0}
};

static const symbol s_15_0[2] = { 0xD8, 0xA9 };

static const struct among a_15[1] =
{
/*  0 */ { 2, s_15_0, -1, 1, 0}
};

static const symbol s_16_0[2] = { 0xD9, 0x8A };

static const struct among a_16[1] =
{
/*  0 */ { 2, s_16_0, -1, 1, 0}
};

static const symbol s_17_0[2] = { 0xD9, 0x83 };
static const symbol s_17_1[4] = { 0xD9, 0x83, 0xD9, 0x85 };
static const symbol s_17_2[4] = { 0xD9, 0x87, 0xD9, 0x85 };
static const symbol s_17_3[4] = { 0xD9, 0x83, 0xD9, 0x86 };
static const symbol s_17_4[4] = { 0xD9, 0x87, 0xD9, 0x86 };
static const symbol s_17_5[2] = { 0xD9, 0x87 };
static const symbol s_17_6[6] = { 0xD9, 0x83, 0xD9, 0x85, 0xD9, 0x88 };
static const symbol s_17_7[4] = { 0xD9, 0x86, 0xD9, 0x8A };
static const symbol s_17_8[6] = { 0xD9, 0x83, 0xD9, 0x85, 0xD8, 0xA7 };
static const symbol s_17_9[6] = { 0xD9, 0x87, 0xD9, 0x85, 0xD8, 0xA7 };
static const symbol s_17_10[4] = { 0xD9, 0x86, 0xD8, 0xA7 };
static const symbol s_17_11[4] = { 0xD9, 0x87, 0xD8, 0xA7 };

static const struct among a_17[12] =
{
/*  0 */ { 2, s_17_0, -1, 1, 0},
/*  1 */ { 4, s_17_1, -1, 2, 0},
/*  2 */ { 4, s_17_2, -1, 2, 0},
/*  3 */ { 4, s_17_3, -1, 2, 0},
/*  4 */ { 4, s_17_4, -1, 2, 0},
/*  5 */ { 2, s_17_5, -1, 1, 0},
/*  6 */ { 6, s_17_6, -1, 3, 0},
/*  7 */ { 4, s_17_7, -1, 2, 0},
/*  8 */ { 6, s_17_8, -1, 3, 0},
/*  9 */ { 6, s_17_9, -1, 3, 0},
/* 10 */ { 4, s_17_10, -1, 2, 0},
/* 11 */ { 4, s_17_11, -1, 2, 0}
};

static const symbol s_18_0[2] = { 0xD9, 0x86 };
static const symbol s_18_1[4] = { 0xD9, 0x88, 0xD9, 0x86 };
static const symbol s_18_2[4] = { 0xD9, 0x8A, 0xD9, 0x86 };
static const symbol s_18_3[4] = { 0xD8, 0xA7, 0xD9, 0x86 };
static const symbol s_18_4[4] = { 0xD8, 0xAA, 0xD9, 0x86 };
static const symbol s_18_5[2] = { 0xD9, 0x8A };
static const symbol s_18_6[2] = { 0xD8, 0xA7 };
static const symbol s_18_7[6] = { 0xD8, 0xAA, 0xD9, 0x85, 0xD8, 0xA7 };
static const symbol s_18_8[4] = { 0xD9, 0x86, 0xD8, 0xA7 };
static const symbol s_18_9[4] = { 0xD8, 0xAA, 0xD8, 0xA7 };
static const symbol s_18_10[2] = { 0xD8, 0xAA };

static const struct among a_18[11] =
{
/*  0 */ { 2, s_18_0, -1, 1, 0},
/*  1 */ { 4, s_18_1, 0, 3, 0},
/*  2 */ { 4, s_18_2, 0, 3, 0},
/*  3 */ { 4, s_18_3, 0, 3, 0},
/*  4 */ { 4, s_18_4, 0, 2, 0},
/*  5 */ { 2, s_18_5, -1, 1, 0},
/*  6 */ { 2, s_18_6, -1, 1, 0},
/*  7 */ { 6, s_18_7, 6, 4, 0},
/*  8 */ { 4, s_18_8, 6, 2, 0},
/*  9 */ { 4, s_18_9, 6, 2, 0},
/* 10 */ { 2, s_18_10, -1, 1, 0}
};

static const symbol s_19_0[4] = { 0xD8, 0xAA, 0xD9, 0x85 };
static const symbol s_19_1[4] = { 0xD9, 0x88, 0xD8, 0xA7 };

static const struct among a_19[2] =
{
/*  0 */ { 4, s_19_0, -1, 1, 0},
/*  1 */ { 4, s_19_1, -1, 1, 0}
};

static const symbol s_20_0[2] = { 0xD9, 0x88 };
static const symbol s_20_1[6] = { 0xD8, 0xAA, 0xD9, 0x85, 0xD9, 0x88 };

static const struct among a_20[2] =
{
/*  0 */ { 2, s_20_0, -1, 1, 0},
/*  1 */ { 6, s_20_1, 0, 2, 0}
};

static const symbol s_21_0[2] = { 0xD9, 0x89 };

static const struct among a_21[1] =
{
/*  0 */ { 2, s_21_0, -1, 1, 0}
};

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
static const symbol s_58[] = { 0xD9, 0x81, 0xD8, 0xA7 };
static const symbol s_59[] = { 0xD9, 0x88, 0xD8, 0xA7 };
static const symbol s_60[] = { 0xD8, 0xA8, 0xD8, 0xA7 };
static const symbol s_61[] = { 0xD8, 0xA8 };
static const symbol s_62[] = { 0xD9, 0x83 };
static const symbol s_63[] = { 0xD9, 0x8A };
static const symbol s_64[] = { 0xD8, 0xAA };
static const symbol s_65[] = { 0xD9, 0x86 };
static const symbol s_66[] = { 0xD8, 0xA3 };
static const symbol s_67[] = { 0xD8, 0xA7, 0xD8, 0xB3, 0xD8, 0xAA };
static const symbol s_68[] = { 0xD9, 0x8A };

static int r_Normalize_pre(struct SN_env * z) { /* forwardmode */
    int among_var;
    {   int c1 = z->c; /* do, line 247 */
        while(1) { /* repeat, line 247 */
            int c2 = z->c;
            {   int c3 = z->c; /* or, line 311 */
                z->bra = z->c; /* [, line 249 */
                among_var = find_among(z, a_0, 144); /* substring, line 249 */
                if (!(among_var)) goto lab3;
                z->ket = z->c; /* ], line 249 */
                switch (among_var) { /* among, line 249 */
                    case 1:
                        {   int ret = slice_del(z); /* delete, line 250 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 2:
                        {   int ret = slice_from_s(z, 1, s_0); /* <-, line 254 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 3:
                        {   int ret = slice_from_s(z, 1, s_1); /* <-, line 255 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 4:
                        {   int ret = slice_from_s(z, 1, s_2); /* <-, line 256 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 5:
                        {   int ret = slice_from_s(z, 1, s_3); /* <-, line 257 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 6:
                        {   int ret = slice_from_s(z, 1, s_4); /* <-, line 258 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 7:
                        {   int ret = slice_from_s(z, 1, s_5); /* <-, line 259 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 8:
                        {   int ret = slice_from_s(z, 1, s_6); /* <-, line 260 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 9:
                        {   int ret = slice_from_s(z, 1, s_7); /* <-, line 261 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 10:
                        {   int ret = slice_from_s(z, 1, s_8); /* <-, line 262 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 11:
                        {   int ret = slice_from_s(z, 1, s_9); /* <-, line 263 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 12:
                        {   int ret = slice_from_s(z, 2, s_10); /* <-, line 266 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 13:
                        {   int ret = slice_from_s(z, 2, s_11); /* <-, line 267 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 14:
                        {   int ret = slice_from_s(z, 2, s_12); /* <-, line 268 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 15:
                        {   int ret = slice_from_s(z, 2, s_13); /* <-, line 269 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 16:
                        {   int ret = slice_from_s(z, 2, s_14); /* <-, line 270 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 17:
                        {   int ret = slice_from_s(z, 2, s_15); /* <-, line 271 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 18:
                        {   int ret = slice_from_s(z, 2, s_16); /* <-, line 272 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 19:
                        {   int ret = slice_from_s(z, 2, s_17); /* <-, line 273 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 20:
                        {   int ret = slice_from_s(z, 2, s_18); /* <-, line 274 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 21:
                        {   int ret = slice_from_s(z, 2, s_19); /* <-, line 275 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 22:
                        {   int ret = slice_from_s(z, 2, s_20); /* <-, line 276 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 23:
                        {   int ret = slice_from_s(z, 2, s_21); /* <-, line 277 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 24:
                        {   int ret = slice_from_s(z, 2, s_22); /* <-, line 278 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 25:
                        {   int ret = slice_from_s(z, 2, s_23); /* <-, line 279 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 26:
                        {   int ret = slice_from_s(z, 2, s_24); /* <-, line 280 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 27:
                        {   int ret = slice_from_s(z, 2, s_25); /* <-, line 281 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 28:
                        {   int ret = slice_from_s(z, 2, s_26); /* <-, line 282 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 29:
                        {   int ret = slice_from_s(z, 2, s_27); /* <-, line 283 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 30:
                        {   int ret = slice_from_s(z, 2, s_28); /* <-, line 284 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 31:
                        {   int ret = slice_from_s(z, 2, s_29); /* <-, line 285 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 32:
                        {   int ret = slice_from_s(z, 2, s_30); /* <-, line 286 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 33:
                        {   int ret = slice_from_s(z, 2, s_31); /* <-, line 287 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 34:
                        {   int ret = slice_from_s(z, 2, s_32); /* <-, line 288 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 35:
                        {   int ret = slice_from_s(z, 2, s_33); /* <-, line 289 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 36:
                        {   int ret = slice_from_s(z, 2, s_34); /* <-, line 290 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 37:
                        {   int ret = slice_from_s(z, 2, s_35); /* <-, line 291 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 38:
                        {   int ret = slice_from_s(z, 2, s_36); /* <-, line 292 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 39:
                        {   int ret = slice_from_s(z, 2, s_37); /* <-, line 293 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 40:
                        {   int ret = slice_from_s(z, 2, s_38); /* <-, line 294 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 41:
                        {   int ret = slice_from_s(z, 2, s_39); /* <-, line 295 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 42:
                        {   int ret = slice_from_s(z, 2, s_40); /* <-, line 296 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 43:
                        {   int ret = slice_from_s(z, 2, s_41); /* <-, line 297 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 44:
                        {   int ret = slice_from_s(z, 2, s_42); /* <-, line 298 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 45:
                        {   int ret = slice_from_s(z, 2, s_43); /* <-, line 299 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 46:
                        {   int ret = slice_from_s(z, 2, s_44); /* <-, line 300 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 47:
                        {   int ret = slice_from_s(z, 2, s_45); /* <-, line 301 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 48:
                        {   int ret = slice_from_s(z, 4, s_46); /* <-, line 304 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 49:
                        {   int ret = slice_from_s(z, 4, s_47); /* <-, line 305 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 50:
                        {   int ret = slice_from_s(z, 4, s_48); /* <-, line 306 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 51:
                        {   int ret = slice_from_s(z, 4, s_49); /* <-, line 307 */
                            if (ret < 0) return ret;
                        }
                        break;
                }
                goto lab2;
            lab3:
                z->c = c3;
                {   int ret = skip_utf8(z->p, z->c, 0, z->l, 1);
                    if (ret < 0) goto lab1;
                    z->c = ret; /* next, line 312 */
                }
            }
        lab2:
            continue;
        lab1:
            z->c = c2;
            break;
        }
        z->c = c1;
    }
    return 1;
}

static int r_Normalize_post(struct SN_env * z) { /* forwardmode */
    int among_var;
    {   int c1 = z->c; /* do, line 318 */
        z->lb = z->c; z->c = z->l; /* backwards, line 320 */

        z->ket = z->c; /* [, line 321 */
        if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 5 || !((124 >> (z->p[z->c - 1] & 0x1f)) & 1)) goto lab0; /* substring, line 321 */
        if (!(find_among_b(z, a_1, 5))) goto lab0;
        z->bra = z->c; /* ], line 321 */
        {   int ret = slice_from_s(z, 2, s_50); /* <-, line 322 */
            if (ret < 0) return ret;
        }
        z->c = z->lb;
    lab0:
        z->c = c1;
    }
    {   int c2 = z->c; /* do, line 329 */
        while(1) { /* repeat, line 329 */
            int c3 = z->c;
            {   int c4 = z->c; /* or, line 338 */
                z->bra = z->c; /* [, line 332 */
                if (z->c + 1 >= z->l || z->p[z->c + 1] >> 5 != 5 || !((124 >> (z->p[z->c + 1] & 0x1f)) & 1)) goto lab4; /* substring, line 332 */
                among_var = find_among(z, a_2, 5);
                if (!(among_var)) goto lab4;
                z->ket = z->c; /* ], line 332 */
                switch (among_var) { /* among, line 332 */
                    case 1:
                        {   int ret = slice_from_s(z, 2, s_51); /* <-, line 333 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 2:
                        {   int ret = slice_from_s(z, 2, s_52); /* <-, line 334 */
                            if (ret < 0) return ret;
                        }
                        break;
                    case 3:
                        {   int ret = slice_from_s(z, 2, s_53); /* <-, line 335 */
                            if (ret < 0) return ret;
                        }
                        break;
                }
                goto lab3;
            lab4:
                z->c = c4;
                {   int ret = skip_utf8(z->p, z->c, 0, z->l, 1);
                    if (ret < 0) goto lab2;
                    z->c = ret; /* next, line 339 */
                }
            }
        lab3:
            continue;
        lab2:
            z->c = c3;
            break;
        }
        z->c = c2;
    }
    return 1;
}

static int r_Checks1(struct SN_env * z) { /* forwardmode */
    int among_var;
    z->bra = z->c; /* [, line 345 */
    if (z->c + 3 >= z->l || (z->p[z->c + 3] != 132 && z->p[z->c + 3] != 167)) return 0; /* substring, line 345 */
    among_var = find_among(z, a_3, 4);
    if (!(among_var)) return 0;
    z->ket = z->c; /* ], line 345 */
    switch (among_var) { /* among, line 345 */
        case 1:
            if (!(len_utf8(z->p) > 4)) return 0; /* $(<integer expression> > <integer expression>), line 346 */
            z->B[0] = 1; /* set is_noun, line 346 */
            z->B[1] = 0; /* unset is_verb, line 346 */
            z->B[2] = 1; /* set is_defined, line 346 */
            break;
        case 2:
            if (!(len_utf8(z->p) > 3)) return 0; /* $(<integer expression> > <integer expression>), line 347 */
            z->B[0] = 1; /* set is_noun, line 347 */
            z->B[1] = 0; /* unset is_verb, line 347 */
            z->B[2] = 1; /* set is_defined, line 347 */
            break;
    }
    return 1;
}

static int r_Prefix_Step1(struct SN_env * z) { /* forwardmode */
    int among_var;
    z->bra = z->c; /* [, line 354 */
    if (z->c + 3 >= z->l || z->p[z->c + 3] >> 5 != 5 || !((188 >> (z->p[z->c + 3] & 0x1f)) & 1)) return 0; /* substring, line 354 */
    among_var = find_among(z, a_4, 5);
    if (!(among_var)) return 0;
    z->ket = z->c; /* ], line 354 */
    switch (among_var) { /* among, line 354 */
        case 1:
            if (!(len_utf8(z->p) > 3)) return 0; /* $(<integer expression> > <integer expression>), line 355 */
            {   int ret = slice_from_s(z, 2, s_54); /* <-, line 355 */
                if (ret < 0) return ret;
            }
            break;
        case 2:
            if (!(len_utf8(z->p) > 3)) return 0; /* $(<integer expression> > <integer expression>), line 356 */
            {   int ret = slice_from_s(z, 2, s_55); /* <-, line 356 */
                if (ret < 0) return ret;
            }
            break;
        case 3:
            if (!(len_utf8(z->p) > 3)) return 0; /* $(<integer expression> > <integer expression>), line 358 */
            {   int ret = slice_from_s(z, 2, s_56); /* <-, line 358 */
                if (ret < 0) return ret;
            }
            break;
        case 4:
            if (!(len_utf8(z->p) > 3)) return 0; /* $(<integer expression> > <integer expression>), line 359 */
            {   int ret = slice_from_s(z, 2, s_57); /* <-, line 359 */
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_Prefix_Step2(struct SN_env * z) { /* forwardmode */
    {   int c1 = z->c; /* not, line 365 */
        if (!(eq_s(z, 4, s_58))) goto lab0; /* literal, line 365 */
        return 0;
    lab0:
        z->c = c1;
    }
    {   int c2 = z->c; /* not, line 366 */
        if (!(eq_s(z, 4, s_59))) goto lab1; /* literal, line 366 */
        return 0;
    lab1:
        z->c = c2;
    }
    z->bra = z->c; /* [, line 367 */
    if (z->c + 1 >= z->l || (z->p[z->c + 1] != 129 && z->p[z->c + 1] != 136)) return 0; /* substring, line 367 */
    if (!(find_among(z, a_5, 2))) return 0;
    z->ket = z->c; /* ], line 367 */
    if (!(len_utf8(z->p) > 3)) return 0; /* $(<integer expression> > <integer expression>), line 368 */
    {   int ret = slice_del(z); /* delete, line 368 */
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_Prefix_Step3a_Noun(struct SN_env * z) { /* forwardmode */
    int among_var;
    z->bra = z->c; /* [, line 374 */
    if (z->c + 3 >= z->l || (z->p[z->c + 3] != 132 && z->p[z->c + 3] != 167)) return 0; /* substring, line 374 */
    among_var = find_among(z, a_6, 4);
    if (!(among_var)) return 0;
    z->ket = z->c; /* ], line 374 */
    switch (among_var) { /* among, line 374 */
        case 1:
            if (!(len_utf8(z->p) > 5)) return 0; /* $(<integer expression> > <integer expression>), line 375 */
            {   int ret = slice_del(z); /* delete, line 375 */
                if (ret < 0) return ret;
            }
            break;
        case 2:
            if (!(len_utf8(z->p) > 4)) return 0; /* $(<integer expression> > <integer expression>), line 376 */
            {   int ret = slice_del(z); /* delete, line 376 */
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_Prefix_Step3b_Noun(struct SN_env * z) { /* forwardmode */
    int among_var;
    {   int c1 = z->c; /* not, line 381 */
        if (!(eq_s(z, 4, s_60))) goto lab0; /* literal, line 381 */
        return 0;
    lab0:
        z->c = c1;
    }
    z->bra = z->c; /* [, line 382 */
    if (z->c + 1 >= z->l || (z->p[z->c + 1] != 168 && z->p[z->c + 1] != 131)) return 0; /* substring, line 382 */
    among_var = find_among(z, a_7, 3);
    if (!(among_var)) return 0;
    z->ket = z->c; /* ], line 382 */
    switch (among_var) { /* among, line 382 */
        case 1:
            if (!(len_utf8(z->p) > 3)) return 0; /* $(<integer expression> > <integer expression>), line 383 */
            {   int ret = slice_del(z); /* delete, line 383 */
                if (ret < 0) return ret;
            }
            break;
        case 2:
            if (!(len_utf8(z->p) > 3)) return 0; /* $(<integer expression> > <integer expression>), line 385 */
            {   int ret = slice_from_s(z, 2, s_61); /* <-, line 385 */
                if (ret < 0) return ret;
            }
            break;
        case 3:
            if (!(len_utf8(z->p) > 3)) return 0; /* $(<integer expression> > <integer expression>), line 386 */
            {   int ret = slice_from_s(z, 2, s_62); /* <-, line 386 */
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_Prefix_Step3_Verb(struct SN_env * z) { /* forwardmode */
    int among_var;
    z->bra = z->c; /* [, line 392 */
    among_var = find_among(z, a_8, 4); /* substring, line 392 */
    if (!(among_var)) return 0;
    z->ket = z->c; /* ], line 392 */
    switch (among_var) { /* among, line 392 */
        case 1:
            if (!(len_utf8(z->p) > 4)) return 0; /* $(<integer expression> > <integer expression>), line 394 */
            {   int ret = slice_from_s(z, 2, s_63); /* <-, line 394 */
                if (ret < 0) return ret;
            }
            break;
        case 2:
            if (!(len_utf8(z->p) > 4)) return 0; /* $(<integer expression> > <integer expression>), line 395 */
            {   int ret = slice_from_s(z, 2, s_64); /* <-, line 395 */
                if (ret < 0) return ret;
            }
            break;
        case 3:
            if (!(len_utf8(z->p) > 4)) return 0; /* $(<integer expression> > <integer expression>), line 396 */
            {   int ret = slice_from_s(z, 2, s_65); /* <-, line 396 */
                if (ret < 0) return ret;
            }
            break;
        case 4:
            if (!(len_utf8(z->p) > 4)) return 0; /* $(<integer expression> > <integer expression>), line 397 */
            {   int ret = slice_from_s(z, 2, s_66); /* <-, line 397 */
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_Prefix_Step4_Verb(struct SN_env * z) { /* forwardmode */
    z->bra = z->c; /* [, line 402 */
    if (z->c + 5 >= z->l || z->p[z->c + 5] != 170) return 0; /* substring, line 402 */
    if (!(find_among(z, a_9, 3))) return 0;
    z->ket = z->c; /* ], line 402 */
    if (!(len_utf8(z->p) > 4)) return 0; /* $(<integer expression> > <integer expression>), line 403 */
    z->B[1] = 1; /* set is_verb, line 403 */
    z->B[0] = 0; /* unset is_noun, line 403 */
    {   int ret = slice_from_s(z, 6, s_67); /* <-, line 403 */
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_Suffix_Noun_Step1a(struct SN_env * z) { /* backwardmode */
    int among_var;
    z->ket = z->c; /* [, line 411 */
    among_var = find_among_b(z, a_10, 10); /* substring, line 411 */
    if (!(among_var)) return 0;
    z->bra = z->c; /* ], line 411 */
    switch (among_var) { /* among, line 411 */
        case 1:
            if (!(len_utf8(z->p) >= 4)) return 0; /* $(<integer expression> >= <integer expression>), line 412 */
            {   int ret = slice_del(z); /* delete, line 412 */
                if (ret < 0) return ret;
            }
            break;
        case 2:
            if (!(len_utf8(z->p) >= 5)) return 0; /* $(<integer expression> >= <integer expression>), line 413 */
            {   int ret = slice_del(z); /* delete, line 413 */
                if (ret < 0) return ret;
            }
            break;
        case 3:
            if (!(len_utf8(z->p) >= 6)) return 0; /* $(<integer expression> >= <integer expression>), line 414 */
            {   int ret = slice_del(z); /* delete, line 414 */
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_Suffix_Noun_Step1b(struct SN_env * z) { /* backwardmode */
    z->ket = z->c; /* [, line 418 */
    if (z->c - 1 <= z->lb || z->p[z->c - 1] != 134) return 0; /* substring, line 418 */
    if (!(find_among_b(z, a_11, 1))) return 0;
    z->bra = z->c; /* ], line 418 */
    if (!(len_utf8(z->p) > 5)) return 0; /* $(<integer expression> > <integer expression>), line 419 */
    {   int ret = slice_del(z); /* delete, line 419 */
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_Suffix_Noun_Step2a(struct SN_env * z) { /* backwardmode */
    z->ket = z->c; /* [, line 424 */
    if (!(find_among_b(z, a_12, 3))) return 0; /* substring, line 424 */
    z->bra = z->c; /* ], line 424 */
    if (!(len_utf8(z->p) > 4)) return 0; /* $(<integer expression> > <integer expression>), line 425 */
    {   int ret = slice_del(z); /* delete, line 425 */
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_Suffix_Noun_Step2b(struct SN_env * z) { /* backwardmode */
    z->ket = z->c; /* [, line 430 */
    if (z->c - 3 <= z->lb || z->p[z->c - 1] != 170) return 0; /* substring, line 430 */
    if (!(find_among_b(z, a_13, 1))) return 0;
    z->bra = z->c; /* ], line 430 */
    if (!(len_utf8(z->p) >= 5)) return 0; /* $(<integer expression> >= <integer expression>), line 431 */
    {   int ret = slice_del(z); /* delete, line 431 */
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_Suffix_Noun_Step2c1(struct SN_env * z) { /* backwardmode */
    z->ket = z->c; /* [, line 436 */
    if (z->c - 1 <= z->lb || z->p[z->c - 1] != 170) return 0; /* substring, line 436 */
    if (!(find_among_b(z, a_14, 1))) return 0;
    z->bra = z->c; /* ], line 436 */
    if (!(len_utf8(z->p) >= 4)) return 0; /* $(<integer expression> >= <integer expression>), line 437 */
    {   int ret = slice_del(z); /* delete, line 437 */
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_Suffix_Noun_Step2c2(struct SN_env * z) { /* backwardmode */
    z->ket = z->c; /* [, line 441 */
    if (z->c - 1 <= z->lb || z->p[z->c - 1] != 169) return 0; /* substring, line 441 */
    if (!(find_among_b(z, a_15, 1))) return 0;
    z->bra = z->c; /* ], line 441 */
    if (!(len_utf8(z->p) >= 4)) return 0; /* $(<integer expression> >= <integer expression>), line 442 */
    {   int ret = slice_del(z); /* delete, line 442 */
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_Suffix_Noun_Step3(struct SN_env * z) { /* backwardmode */
    z->ket = z->c; /* [, line 446 */
    if (z->c - 1 <= z->lb || z->p[z->c - 1] != 138) return 0; /* substring, line 446 */
    if (!(find_among_b(z, a_16, 1))) return 0;
    z->bra = z->c; /* ], line 446 */
    if (!(len_utf8(z->p) >= 3)) return 0; /* $(<integer expression> >= <integer expression>), line 447 */
    {   int ret = slice_del(z); /* delete, line 447 */
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_Suffix_Verb_Step1(struct SN_env * z) { /* backwardmode */
    int among_var;
    z->ket = z->c; /* [, line 452 */
    among_var = find_among_b(z, a_17, 12); /* substring, line 452 */
    if (!(among_var)) return 0;
    z->bra = z->c; /* ], line 452 */
    switch (among_var) { /* among, line 452 */
        case 1:
            if (!(len_utf8(z->p) >= 4)) return 0; /* $(<integer expression> >= <integer expression>), line 453 */
            {   int ret = slice_del(z); /* delete, line 453 */
                if (ret < 0) return ret;
            }
            break;
        case 2:
            if (!(len_utf8(z->p) >= 5)) return 0; /* $(<integer expression> >= <integer expression>), line 454 */
            {   int ret = slice_del(z); /* delete, line 454 */
                if (ret < 0) return ret;
            }
            break;
        case 3:
            if (!(len_utf8(z->p) >= 6)) return 0; /* $(<integer expression> >= <integer expression>), line 455 */
            {   int ret = slice_del(z); /* delete, line 455 */
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_Suffix_Verb_Step2a(struct SN_env * z) { /* backwardmode */
    int among_var;
    z->ket = z->c; /* [, line 459 */
    among_var = find_among_b(z, a_18, 11); /* substring, line 459 */
    if (!(among_var)) return 0;
    z->bra = z->c; /* ], line 459 */
    switch (among_var) { /* among, line 459 */
        case 1:
            if (!(len_utf8(z->p) >= 4)) return 0; /* $(<integer expression> >= <integer expression>), line 460 */
            {   int ret = slice_del(z); /* delete, line 460 */
                if (ret < 0) return ret;
            }
            break;
        case 2:
            if (!(len_utf8(z->p) >= 5)) return 0; /* $(<integer expression> >= <integer expression>), line 462 */
            {   int ret = slice_del(z); /* delete, line 462 */
                if (ret < 0) return ret;
            }
            break;
        case 3:
            if (!(len_utf8(z->p) > 5)) return 0; /* $(<integer expression> > <integer expression>), line 463 */
            {   int ret = slice_del(z); /* delete, line 463 */
                if (ret < 0) return ret;
            }
            break;
        case 4:
            if (!(len_utf8(z->p) >= 6)) return 0; /* $(<integer expression> >= <integer expression>), line 464 */
            {   int ret = slice_del(z); /* delete, line 464 */
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_Suffix_Verb_Step2b(struct SN_env * z) { /* backwardmode */
    z->ket = z->c; /* [, line 469 */
    if (z->c - 3 <= z->lb || (z->p[z->c - 1] != 133 && z->p[z->c - 1] != 167)) return 0; /* substring, line 469 */
    if (!(find_among_b(z, a_19, 2))) return 0;
    z->bra = z->c; /* ], line 469 */
    if (!(len_utf8(z->p) >= 5)) return 0; /* $(<integer expression> >= <integer expression>), line 470 */
    {   int ret = slice_del(z); /* delete, line 470 */
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_Suffix_Verb_Step2c(struct SN_env * z) { /* backwardmode */
    int among_var;
    z->ket = z->c; /* [, line 476 */
    if (z->c - 1 <= z->lb || z->p[z->c - 1] != 136) return 0; /* substring, line 476 */
    among_var = find_among_b(z, a_20, 2);
    if (!(among_var)) return 0;
    z->bra = z->c; /* ], line 476 */
    switch (among_var) { /* among, line 476 */
        case 1:
            if (!(len_utf8(z->p) >= 4)) return 0; /* $(<integer expression> >= <integer expression>), line 477 */
            {   int ret = slice_del(z); /* delete, line 477 */
                if (ret < 0) return ret;
            }
            break;
        case 2:
            if (!(len_utf8(z->p) >= 6)) return 0; /* $(<integer expression> >= <integer expression>), line 478 */
            {   int ret = slice_del(z); /* delete, line 478 */
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_Suffix_All_alef_maqsura(struct SN_env * z) { /* backwardmode */
    z->ket = z->c; /* [, line 483 */
    if (z->c - 1 <= z->lb || z->p[z->c - 1] != 137) return 0; /* substring, line 483 */
    if (!(find_among_b(z, a_21, 1))) return 0;
    z->bra = z->c; /* ], line 483 */
    {   int ret = slice_from_s(z, 2, s_68); /* <-, line 484 */
        if (ret < 0) return ret;
    }
    return 1;
}

extern int arabic_UTF_8_stem(struct SN_env * z) { /* forwardmode */
    z->B[0] = 1; /* set is_noun, line 493 */
    z->B[1] = 1; /* set is_verb, line 494 */
    z->B[2] = 0; /* unset is_defined, line 495 */
    {   int c1 = z->c; /* do, line 498 */
        {   int ret = r_Checks1(z); /* call Checks1, line 498 */
            if (ret == 0) goto lab0;
            if (ret < 0) return ret;
        }
    lab0:
        z->c = c1;
    }
    /* do, line 501 */
    {   int ret = r_Normalize_pre(z); /* call Normalize_pre, line 501 */
        if (ret == 0) goto lab1;
        if (ret < 0) return ret;
    }
lab1:
    z->lb = z->c; z->c = z->l; /* backwards, line 504 */

    {   int m2 = z->l - z->c; (void)m2; /* do, line 506 */
        {   int m3 = z->l - z->c; (void)m3; /* or, line 520 */
            if (!(z->B[1])) goto lab4; /* Boolean test is_verb, line 509 */
            {   int m4 = z->l - z->c; (void)m4; /* or, line 515 */
                {   int i = 1;
                    while(1) { /* atleast, line 512 */
                        int m5 = z->l - z->c; (void)m5;
                        {   int ret = r_Suffix_Verb_Step1(z); /* call Suffix_Verb_Step1, line 512 */
                            if (ret == 0) goto lab7;
                            if (ret < 0) return ret;
                        }
                        i--;
                        continue;
                    lab7:
                        z->c = z->l - m5;
                        break;
                    }
                    if (i > 0) goto lab6;
                }
                {   int m6 = z->l - z->c; (void)m6; /* or, line 513 */
                    {   int ret = r_Suffix_Verb_Step2a(z); /* call Suffix_Verb_Step2a, line 513 */
                        if (ret == 0) goto lab9;
                        if (ret < 0) return ret;
                    }
                    goto lab8;
                lab9:
                    z->c = z->l - m6;
                    {   int ret = r_Suffix_Verb_Step2c(z); /* call Suffix_Verb_Step2c, line 513 */
                        if (ret == 0) goto lab10;
                        if (ret < 0) return ret;
                    }
                    goto lab8;
                lab10:
                    z->c = z->l - m6;
                    {   int ret = skip_utf8(z->p, z->c, z->lb, 0, -1);
                        if (ret < 0) goto lab6;
                        z->c = ret; /* next, line 513 */
                    }
                }
            lab8:
                goto lab5;
            lab6:
                z->c = z->l - m4;
                {   int ret = r_Suffix_Verb_Step2b(z); /* call Suffix_Verb_Step2b, line 515 */
                    if (ret == 0) goto lab11;
                    if (ret < 0) return ret;
                }
                goto lab5;
            lab11:
                z->c = z->l - m4;
                {   int ret = r_Suffix_Verb_Step2a(z); /* call Suffix_Verb_Step2a, line 516 */
                    if (ret == 0) goto lab4;
                    if (ret < 0) return ret;
                }
            }
        lab5:
            goto lab3;
        lab4:
            z->c = z->l - m3;
            if (!(z->B[0])) goto lab12; /* Boolean test is_noun, line 521 */
            {   int m7 = z->l - z->c; (void)m7; /* try, line 524 */
                {   int m8 = z->l - z->c; (void)m8; /* or, line 526 */
                    {   int ret = r_Suffix_Noun_Step2c2(z); /* call Suffix_Noun_Step2c2, line 525 */
                        if (ret == 0) goto lab15;
                        if (ret < 0) return ret;
                    }
                    goto lab14;
                lab15:
                    z->c = z->l - m8;
                    /* not, line 526 */
                    if (!(z->B[2])) goto lab17; /* Boolean test is_defined, line 526 */
                    goto lab16;
                lab17:
                    {   int ret = r_Suffix_Noun_Step1a(z); /* call Suffix_Noun_Step1a, line 526 */
                        if (ret == 0) goto lab16;
                        if (ret < 0) return ret;
                    }
                    {   int m9 = z->l - z->c; (void)m9; /* or, line 528 */
                        {   int ret = r_Suffix_Noun_Step2a(z); /* call Suffix_Noun_Step2a, line 527 */
                            if (ret == 0) goto lab19;
                            if (ret < 0) return ret;
                        }
                        goto lab18;
                    lab19:
                        z->c = z->l - m9;
                        {   int ret = r_Suffix_Noun_Step2b(z); /* call Suffix_Noun_Step2b, line 528 */
                            if (ret == 0) goto lab20;
                            if (ret < 0) return ret;
                        }
                        goto lab18;
                    lab20:
                        z->c = z->l - m9;
                        {   int ret = r_Suffix_Noun_Step2c1(z); /* call Suffix_Noun_Step2c1, line 529 */
                            if (ret == 0) goto lab21;
                            if (ret < 0) return ret;
                        }
                        goto lab18;
                    lab21:
                        z->c = z->l - m9;
                        {   int ret = skip_utf8(z->p, z->c, z->lb, 0, -1);
                            if (ret < 0) goto lab16;
                            z->c = ret; /* next, line 530 */
                        }
                    }
                lab18:
                    goto lab14;
                lab16:
                    z->c = z->l - m8;
                    {   int ret = r_Suffix_Noun_Step1b(z); /* call Suffix_Noun_Step1b, line 531 */
                        if (ret == 0) goto lab22;
                        if (ret < 0) return ret;
                    }
                    {   int m10 = z->l - z->c; (void)m10; /* or, line 533 */
                        {   int ret = r_Suffix_Noun_Step2a(z); /* call Suffix_Noun_Step2a, line 532 */
                            if (ret == 0) goto lab24;
                            if (ret < 0) return ret;
                        }
                        goto lab23;
                    lab24:
                        z->c = z->l - m10;
                        {   int ret = r_Suffix_Noun_Step2b(z); /* call Suffix_Noun_Step2b, line 533 */
                            if (ret == 0) goto lab25;
                            if (ret < 0) return ret;
                        }
                        goto lab23;
                    lab25:
                        z->c = z->l - m10;
                        {   int ret = r_Suffix_Noun_Step2c1(z); /* call Suffix_Noun_Step2c1, line 534 */
                            if (ret == 0) goto lab22;
                            if (ret < 0) return ret;
                        }
                    }
                lab23:
                    goto lab14;
                lab22:
                    z->c = z->l - m8;
                    /* not, line 535 */
                    if (!(z->B[2])) goto lab27; /* Boolean test is_defined, line 535 */
                    goto lab26;
                lab27:
                    {   int ret = r_Suffix_Noun_Step2a(z); /* call Suffix_Noun_Step2a, line 535 */
                        if (ret == 0) goto lab26;
                        if (ret < 0) return ret;
                    }
                    goto lab14;
                lab26:
                    z->c = z->l - m8;
                    {   int ret = r_Suffix_Noun_Step2b(z); /* call Suffix_Noun_Step2b, line 536 */
                        if (ret == 0) { z->c = z->l - m7; goto lab13; }
                        if (ret < 0) return ret;
                    }
                }
            lab14:
            lab13:
                ;
            }
            {   int ret = r_Suffix_Noun_Step3(z); /* call Suffix_Noun_Step3, line 538 */
                if (ret == 0) goto lab12;
                if (ret < 0) return ret;
            }
            goto lab3;
        lab12:
            z->c = z->l - m3;
            {   int ret = r_Suffix_All_alef_maqsura(z); /* call Suffix_All_alef_maqsura, line 544 */
                if (ret == 0) goto lab2;
                if (ret < 0) return ret;
            }
        }
    lab3:
    lab2:
        z->c = z->l - m2;
    }
    z->c = z->lb;
    {   int c11 = z->c; /* do, line 549 */
        {   int c12 = z->c; /* try, line 550 */
            {   int ret = r_Prefix_Step1(z); /* call Prefix_Step1, line 550 */
                if (ret == 0) { z->c = c12; goto lab29; }
                if (ret < 0) return ret;
            }
        lab29:
            ;
        }
        {   int c13 = z->c; /* try, line 551 */
            {   int ret = r_Prefix_Step2(z); /* call Prefix_Step2, line 551 */
                if (ret == 0) { z->c = c13; goto lab30; }
                if (ret < 0) return ret;
            }
        lab30:
            ;
        }
        {   int c14 = z->c; /* or, line 553 */
            {   int ret = r_Prefix_Step3a_Noun(z); /* call Prefix_Step3a_Noun, line 552 */
                if (ret == 0) goto lab32;
                if (ret < 0) return ret;
            }
            goto lab31;
        lab32:
            z->c = c14;
            if (!(z->B[0])) goto lab33; /* Boolean test is_noun, line 553 */
            {   int ret = r_Prefix_Step3b_Noun(z); /* call Prefix_Step3b_Noun, line 553 */
                if (ret == 0) goto lab33;
                if (ret < 0) return ret;
            }
            goto lab31;
        lab33:
            z->c = c14;
            if (!(z->B[1])) goto lab28; /* Boolean test is_verb, line 554 */
            {   int c15 = z->c; /* try, line 554 */
                {   int ret = r_Prefix_Step3_Verb(z); /* call Prefix_Step3_Verb, line 554 */
                    if (ret == 0) { z->c = c15; goto lab34; }
                    if (ret < 0) return ret;
                }
            lab34:
                ;
            }
            {   int ret = r_Prefix_Step4_Verb(z); /* call Prefix_Step4_Verb, line 554 */
                if (ret == 0) goto lab28;
                if (ret < 0) return ret;
            }
        }
    lab31:
    lab28:
        z->c = c11;
    }
    /* do, line 559 */
    {   int ret = r_Normalize_post(z); /* call Normalize_post, line 559 */
        if (ret == 0) goto lab35;
        if (ret < 0) return ret;
    }
lab35:
    return 1;
}

extern struct SN_env * arabic_UTF_8_create_env(void) { return SN_create_env(0, 0, 3); }

extern void arabic_UTF_8_close_env(struct SN_env * z) { SN_close_env(z, 0); }

