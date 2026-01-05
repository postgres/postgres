/* Generated from catalan.sbl by Snowball 3.0.0 - https://snowballstem.org/ */

#include "stem_UTF_8_catalan.h"

#include <stddef.h>

#include "snowball_runtime.h"

struct SN_local {
    struct SN_env z;
    int i_p2;
    int i_p1;
};

typedef struct SN_local SN_local;

#ifdef __cplusplus
extern "C" {
#endif
extern int catalan_UTF_8_stem(struct SN_env * z);
#ifdef __cplusplus
}
#endif

static int r_residual_suffix(struct SN_env * z);
static int r_verb_suffix(struct SN_env * z);
static int r_standard_suffix(struct SN_env * z);
static int r_attached_pronoun(struct SN_env * z);
static int r_R2(struct SN_env * z);
static int r_R1(struct SN_env * z);
static int r_mark_regions(struct SN_env * z);
static int r_cleaning(struct SN_env * z);

static const symbol s_0[] = { 'a' };
static const symbol s_1[] = { 'e' };
static const symbol s_2[] = { 'i' };
static const symbol s_3[] = { 'o' };
static const symbol s_4[] = { 'u' };
static const symbol s_5[] = { '.' };
static const symbol s_6[] = { 'l', 'o', 'g' };
static const symbol s_7[] = { 'i', 'c' };
static const symbol s_8[] = { 'c' };
static const symbol s_9[] = { 'i', 'c' };

static const symbol s_0_1[2] = { 0xC2, 0xB7 };
static const symbol s_0_2[2] = { 0xC3, 0xA0 };
static const symbol s_0_3[2] = { 0xC3, 0xA1 };
static const symbol s_0_4[2] = { 0xC3, 0xA8 };
static const symbol s_0_5[2] = { 0xC3, 0xA9 };
static const symbol s_0_6[2] = { 0xC3, 0xAC };
static const symbol s_0_7[2] = { 0xC3, 0xAD };
static const symbol s_0_8[2] = { 0xC3, 0xAF };
static const symbol s_0_9[2] = { 0xC3, 0xB2 };
static const symbol s_0_10[2] = { 0xC3, 0xB3 };
static const symbol s_0_11[2] = { 0xC3, 0xBA };
static const symbol s_0_12[2] = { 0xC3, 0xBC };
static const struct among a_0[13] = {
{ 0, 0, 0, 7, 0},
{ 2, s_0_1, -1, 6, 0},
{ 2, s_0_2, -2, 1, 0},
{ 2, s_0_3, -3, 1, 0},
{ 2, s_0_4, -4, 2, 0},
{ 2, s_0_5, -5, 2, 0},
{ 2, s_0_6, -6, 3, 0},
{ 2, s_0_7, -7, 3, 0},
{ 2, s_0_8, -8, 3, 0},
{ 2, s_0_9, -9, 4, 0},
{ 2, s_0_10, -10, 4, 0},
{ 2, s_0_11, -11, 5, 0},
{ 2, s_0_12, -12, 5, 0}
};

static const symbol s_1_0[2] = { 'l', 'a' };
static const symbol s_1_1[3] = { '-', 'l', 'a' };
static const symbol s_1_2[4] = { 's', 'e', 'l', 'a' };
static const symbol s_1_3[2] = { 'l', 'e' };
static const symbol s_1_4[2] = { 'm', 'e' };
static const symbol s_1_5[3] = { '-', 'm', 'e' };
static const symbol s_1_6[2] = { 's', 'e' };
static const symbol s_1_7[3] = { '-', 't', 'e' };
static const symbol s_1_8[2] = { 'h', 'i' };
static const symbol s_1_9[3] = { '\'', 'h', 'i' };
static const symbol s_1_10[2] = { 'l', 'i' };
static const symbol s_1_11[3] = { '-', 'l', 'i' };
static const symbol s_1_12[2] = { '\'', 'l' };
static const symbol s_1_13[2] = { '\'', 'm' };
static const symbol s_1_14[2] = { '-', 'm' };
static const symbol s_1_15[2] = { '\'', 'n' };
static const symbol s_1_16[2] = { '-', 'n' };
static const symbol s_1_17[2] = { 'h', 'o' };
static const symbol s_1_18[3] = { '\'', 'h', 'o' };
static const symbol s_1_19[2] = { 'l', 'o' };
static const symbol s_1_20[4] = { 's', 'e', 'l', 'o' };
static const symbol s_1_21[2] = { '\'', 's' };
static const symbol s_1_22[3] = { 'l', 'a', 's' };
static const symbol s_1_23[5] = { 's', 'e', 'l', 'a', 's' };
static const symbol s_1_24[3] = { 'l', 'e', 's' };
static const symbol s_1_25[4] = { '-', 'l', 'e', 's' };
static const symbol s_1_26[3] = { '\'', 'l', 's' };
static const symbol s_1_27[3] = { '-', 'l', 's' };
static const symbol s_1_28[3] = { '\'', 'n', 's' };
static const symbol s_1_29[3] = { '-', 'n', 's' };
static const symbol s_1_30[3] = { 'e', 'n', 's' };
static const symbol s_1_31[3] = { 'l', 'o', 's' };
static const symbol s_1_32[5] = { 's', 'e', 'l', 'o', 's' };
static const symbol s_1_33[3] = { 'n', 'o', 's' };
static const symbol s_1_34[4] = { '-', 'n', 'o', 's' };
static const symbol s_1_35[3] = { 'v', 'o', 's' };
static const symbol s_1_36[2] = { 'u', 's' };
static const symbol s_1_37[3] = { '-', 'u', 's' };
static const symbol s_1_38[2] = { '\'', 't' };
static const struct among a_1[39] = {
{ 2, s_1_0, 0, 1, 0},
{ 3, s_1_1, -1, 1, 0},
{ 4, s_1_2, -2, 1, 0},
{ 2, s_1_3, 0, 1, 0},
{ 2, s_1_4, 0, 1, 0},
{ 3, s_1_5, -1, 1, 0},
{ 2, s_1_6, 0, 1, 0},
{ 3, s_1_7, 0, 1, 0},
{ 2, s_1_8, 0, 1, 0},
{ 3, s_1_9, -1, 1, 0},
{ 2, s_1_10, 0, 1, 0},
{ 3, s_1_11, -1, 1, 0},
{ 2, s_1_12, 0, 1, 0},
{ 2, s_1_13, 0, 1, 0},
{ 2, s_1_14, 0, 1, 0},
{ 2, s_1_15, 0, 1, 0},
{ 2, s_1_16, 0, 1, 0},
{ 2, s_1_17, 0, 1, 0},
{ 3, s_1_18, -1, 1, 0},
{ 2, s_1_19, 0, 1, 0},
{ 4, s_1_20, -1, 1, 0},
{ 2, s_1_21, 0, 1, 0},
{ 3, s_1_22, 0, 1, 0},
{ 5, s_1_23, -1, 1, 0},
{ 3, s_1_24, 0, 1, 0},
{ 4, s_1_25, -1, 1, 0},
{ 3, s_1_26, 0, 1, 0},
{ 3, s_1_27, 0, 1, 0},
{ 3, s_1_28, 0, 1, 0},
{ 3, s_1_29, 0, 1, 0},
{ 3, s_1_30, 0, 1, 0},
{ 3, s_1_31, 0, 1, 0},
{ 5, s_1_32, -1, 1, 0},
{ 3, s_1_33, 0, 1, 0},
{ 4, s_1_34, -1, 1, 0},
{ 3, s_1_35, 0, 1, 0},
{ 2, s_1_36, 0, 1, 0},
{ 3, s_1_37, -1, 1, 0},
{ 2, s_1_38, 0, 1, 0}
};

static const symbol s_2_0[3] = { 'i', 'c', 'a' };
static const symbol s_2_1[7] = { 'l', 0xC3, 0xB3, 'g', 'i', 'c', 'a' };
static const symbol s_2_2[4] = { 'e', 'n', 'c', 'a' };
static const symbol s_2_3[3] = { 'a', 'd', 'a' };
static const symbol s_2_4[5] = { 'a', 'n', 'c', 'i', 'a' };
static const symbol s_2_5[5] = { 'e', 'n', 'c', 'i', 'a' };
static const symbol s_2_6[6] = { 0xC3, 0xA8, 'n', 'c', 'i', 'a' };
static const symbol s_2_7[5] = { 0xC3, 0xAD, 'c', 'i', 'a' };
static const symbol s_2_8[5] = { 'l', 'o', 'g', 'i', 'a' };
static const symbol s_2_9[4] = { 'i', 'n', 'i', 'a' };
static const symbol s_2_10[6] = { 0xC3, 0xAD, 'i', 'n', 'i', 'a' };
static const symbol s_2_11[4] = { 'e', 'r', 'i', 'a' };
static const symbol s_2_12[5] = { 0xC3, 0xA0, 'r', 'i', 'a' };
static const symbol s_2_13[7] = { 'a', 't', 0xC3, 0xB2, 'r', 'i', 'a' };
static const symbol s_2_14[4] = { 'a', 'l', 'l', 'a' };
static const symbol s_2_15[4] = { 'e', 'l', 'l', 'a' };
static const symbol s_2_16[6] = { 0xC3, 0xAD, 'v', 'o', 'l', 'a' };
static const symbol s_2_17[3] = { 'i', 'm', 'a' };
static const symbol s_2_18[7] = { 0xC3, 0xAD, 's', 's', 'i', 'm', 'a' };
static const symbol s_2_19[9] = { 'q', 'u', 0xC3, 0xAD, 's', 's', 'i', 'm', 'a' };
static const symbol s_2_20[3] = { 'a', 'n', 'a' };
static const symbol s_2_21[3] = { 'i', 'n', 'a' };
static const symbol s_2_22[3] = { 'e', 'r', 'a' };
static const symbol s_2_23[5] = { 's', 'f', 'e', 'r', 'a' };
static const symbol s_2_24[3] = { 'o', 'r', 'a' };
static const symbol s_2_25[4] = { 'd', 'o', 'r', 'a' };
static const symbol s_2_26[5] = { 'a', 'd', 'o', 'r', 'a' };
static const symbol s_2_27[5] = { 'a', 'd', 'u', 'r', 'a' };
static const symbol s_2_28[3] = { 'e', 's', 'a' };
static const symbol s_2_29[3] = { 'o', 's', 'a' };
static const symbol s_2_30[4] = { 'a', 's', 's', 'a' };
static const symbol s_2_31[4] = { 'e', 's', 's', 'a' };
static const symbol s_2_32[4] = { 'i', 's', 's', 'a' };
static const symbol s_2_33[3] = { 'e', 't', 'a' };
static const symbol s_2_34[3] = { 'i', 't', 'a' };
static const symbol s_2_35[3] = { 'o', 't', 'a' };
static const symbol s_2_36[4] = { 'i', 's', 't', 'a' };
static const symbol s_2_37[7] = { 'i', 'a', 'l', 'i', 's', 't', 'a' };
static const symbol s_2_38[7] = { 'i', 'o', 'n', 'i', 's', 't', 'a' };
static const symbol s_2_39[3] = { 'i', 'v', 'a' };
static const symbol s_2_40[5] = { 'a', 't', 'i', 'v', 'a' };
static const symbol s_2_41[4] = { 'n', 0xC3, 0xA7, 'a' };
static const symbol s_2_42[6] = { 'l', 'o', 'g', 0xC3, 0xAD, 'a' };
static const symbol s_2_43[2] = { 'i', 'c' };
static const symbol s_2_44[6] = { 0xC3, 0xAD, 's', 't', 'i', 'c' };
static const symbol s_2_45[3] = { 'e', 'n', 'c' };
static const symbol s_2_46[3] = { 'e', 's', 'c' };
static const symbol s_2_47[2] = { 'u', 'd' };
static const symbol s_2_48[4] = { 'a', 't', 'g', 'e' };
static const symbol s_2_49[3] = { 'b', 'l', 'e' };
static const symbol s_2_50[4] = { 'a', 'b', 'l', 'e' };
static const symbol s_2_51[4] = { 'i', 'b', 'l', 'e' };
static const symbol s_2_52[4] = { 'i', 's', 'm', 'e' };
static const symbol s_2_53[7] = { 'i', 'a', 'l', 'i', 's', 'm', 'e' };
static const symbol s_2_54[7] = { 'i', 'o', 'n', 'i', 's', 'm', 'e' };
static const symbol s_2_55[6] = { 'i', 'v', 'i', 's', 'm', 'e' };
static const symbol s_2_56[4] = { 'a', 'i', 'r', 'e' };
static const symbol s_2_57[4] = { 'i', 'c', 't', 'e' };
static const symbol s_2_58[4] = { 'i', 's', 't', 'e' };
static const symbol s_2_59[3] = { 'i', 'c', 'i' };
static const symbol s_2_60[4] = { 0xC3, 0xAD, 'c', 'i' };
static const symbol s_2_61[4] = { 'l', 'o', 'g', 'i' };
static const symbol s_2_62[3] = { 'a', 'r', 'i' };
static const symbol s_2_63[4] = { 't', 'o', 'r', 'i' };
static const symbol s_2_64[2] = { 'a', 'l' };
static const symbol s_2_65[2] = { 'i', 'l' };
static const symbol s_2_66[3] = { 'a', 'l', 'l' };
static const symbol s_2_67[3] = { 'e', 'l', 'l' };
static const symbol s_2_68[5] = { 0xC3, 0xAD, 'v', 'o', 'l' };
static const symbol s_2_69[4] = { 'i', 's', 'a', 'm' };
static const symbol s_2_70[5] = { 'i', 's', 's', 'e', 'm' };
static const symbol s_2_71[6] = { 0xC3, 0xAC, 's', 's', 'e', 'm' };
static const symbol s_2_72[6] = { 0xC3, 0xAD, 's', 's', 'e', 'm' };
static const symbol s_2_73[6] = { 0xC3, 0xAD, 's', 's', 'i', 'm' };
static const symbol s_2_74[8] = { 'q', 'u', 0xC3, 0xAD, 's', 's', 'i', 'm' };
static const symbol s_2_75[4] = { 'a', 'm', 'e', 'n' };
static const symbol s_2_76[6] = { 0xC3, 0xAC, 's', 's', 'i', 'n' };
static const symbol s_2_77[2] = { 'a', 'r' };
static const symbol s_2_78[6] = { 'i', 'f', 'i', 'c', 'a', 'r' };
static const symbol s_2_79[4] = { 'e', 'g', 'a', 'r' };
static const symbol s_2_80[4] = { 'e', 'j', 'a', 'r' };
static const symbol s_2_81[4] = { 'i', 't', 'a', 'r' };
static const symbol s_2_82[5] = { 'i', 't', 'z', 'a', 'r' };
static const symbol s_2_83[3] = { 'f', 'e', 'r' };
static const symbol s_2_84[2] = { 'o', 'r' };
static const symbol s_2_85[3] = { 'd', 'o', 'r' };
static const symbol s_2_86[3] = { 'd', 'u', 'r' };
static const symbol s_2_87[5] = { 'd', 'o', 'r', 'a', 's' };
static const symbol s_2_88[3] = { 'i', 'c', 's' };
static const symbol s_2_89[7] = { 'l', 0xC3, 0xB3, 'g', 'i', 'c', 's' };
static const symbol s_2_90[3] = { 'u', 'd', 's' };
static const symbol s_2_91[4] = { 'n', 'c', 'e', 's' };
static const symbol s_2_92[4] = { 'a', 'd', 'e', 's' };
static const symbol s_2_93[6] = { 'a', 'n', 'c', 'i', 'e', 's' };
static const symbol s_2_94[6] = { 'e', 'n', 'c', 'i', 'e', 's' };
static const symbol s_2_95[7] = { 0xC3, 0xA8, 'n', 'c', 'i', 'e', 's' };
static const symbol s_2_96[6] = { 0xC3, 0xAD, 'c', 'i', 'e', 's' };
static const symbol s_2_97[6] = { 'l', 'o', 'g', 'i', 'e', 's' };
static const symbol s_2_98[5] = { 'i', 'n', 'i', 'e', 's' };
static const symbol s_2_99[6] = { 0xC3, 0xAD, 'n', 'i', 'e', 's' };
static const symbol s_2_100[5] = { 'e', 'r', 'i', 'e', 's' };
static const symbol s_2_101[6] = { 0xC3, 0xA0, 'r', 'i', 'e', 's' };
static const symbol s_2_102[8] = { 'a', 't', 0xC3, 0xB2, 'r', 'i', 'e', 's' };
static const symbol s_2_103[4] = { 'b', 'l', 'e', 's' };
static const symbol s_2_104[5] = { 'a', 'b', 'l', 'e', 's' };
static const symbol s_2_105[5] = { 'i', 'b', 'l', 'e', 's' };
static const symbol s_2_106[4] = { 'i', 'm', 'e', 's' };
static const symbol s_2_107[8] = { 0xC3, 0xAD, 's', 's', 'i', 'm', 'e', 's' };
static const symbol s_2_108[10] = { 'q', 'u', 0xC3, 0xAD, 's', 's', 'i', 'm', 'e', 's' };
static const symbol s_2_109[6] = { 'f', 'o', 'r', 'm', 'e', 's' };
static const symbol s_2_110[5] = { 'i', 's', 'm', 'e', 's' };
static const symbol s_2_111[8] = { 'i', 'a', 'l', 'i', 's', 'm', 'e', 's' };
static const symbol s_2_112[4] = { 'i', 'n', 'e', 's' };
static const symbol s_2_113[4] = { 'e', 'r', 'e', 's' };
static const symbol s_2_114[4] = { 'o', 'r', 'e', 's' };
static const symbol s_2_115[5] = { 'd', 'o', 'r', 'e', 's' };
static const symbol s_2_116[6] = { 'i', 'd', 'o', 'r', 'e', 's' };
static const symbol s_2_117[5] = { 'd', 'u', 'r', 'e', 's' };
static const symbol s_2_118[4] = { 'e', 's', 'e', 's' };
static const symbol s_2_119[4] = { 'o', 's', 'e', 's' };
static const symbol s_2_120[5] = { 'a', 's', 's', 'e', 's' };
static const symbol s_2_121[5] = { 'i', 'c', 't', 'e', 's' };
static const symbol s_2_122[4] = { 'i', 't', 'e', 's' };
static const symbol s_2_123[4] = { 'o', 't', 'e', 's' };
static const symbol s_2_124[5] = { 'i', 's', 't', 'e', 's' };
static const symbol s_2_125[8] = { 'i', 'a', 'l', 'i', 's', 't', 'e', 's' };
static const symbol s_2_126[8] = { 'i', 'o', 'n', 'i', 's', 't', 'e', 's' };
static const symbol s_2_127[5] = { 'i', 'q', 'u', 'e', 's' };
static const symbol s_2_128[9] = { 'l', 0xC3, 0xB3, 'g', 'i', 'q', 'u', 'e', 's' };
static const symbol s_2_129[4] = { 'i', 'v', 'e', 's' };
static const symbol s_2_130[6] = { 'a', 't', 'i', 'v', 'e', 's' };
static const symbol s_2_131[7] = { 'l', 'o', 'g', 0xC3, 0xAD, 'e', 's' };
static const symbol s_2_132[10] = { 'a', 'l', 'l', 'e', 'n', 'g', 0xC3, 0xBC, 'e', 's' };
static const symbol s_2_133[4] = { 'i', 'c', 'i', 's' };
static const symbol s_2_134[5] = { 0xC3, 0xAD, 'c', 'i', 's' };
static const symbol s_2_135[5] = { 'l', 'o', 'g', 'i', 's' };
static const symbol s_2_136[4] = { 'a', 'r', 'i', 's' };
static const symbol s_2_137[5] = { 't', 'o', 'r', 'i', 's' };
static const symbol s_2_138[2] = { 'l', 's' };
static const symbol s_2_139[3] = { 'a', 'l', 's' };
static const symbol s_2_140[4] = { 'e', 'l', 'l', 's' };
static const symbol s_2_141[3] = { 'i', 'm', 's' };
static const symbol s_2_142[7] = { 0xC3, 0xAD, 's', 's', 'i', 'm', 's' };
static const symbol s_2_143[9] = { 'q', 'u', 0xC3, 0xAD, 's', 's', 'i', 'm', 's' };
static const symbol s_2_144[4] = { 'i', 'o', 'n', 's' };
static const symbol s_2_145[5] = { 'c', 'i', 'o', 'n', 's' };
static const symbol s_2_146[6] = { 'a', 'c', 'i', 'o', 'n', 's' };
static const symbol s_2_147[4] = { 'e', 's', 'o', 's' };
static const symbol s_2_148[4] = { 'o', 's', 'o', 's' };
static const symbol s_2_149[5] = { 'a', 's', 's', 'o', 's' };
static const symbol s_2_150[5] = { 'i', 's', 's', 'o', 's' };
static const symbol s_2_151[3] = { 'e', 'r', 's' };
static const symbol s_2_152[3] = { 'o', 'r', 's' };
static const symbol s_2_153[4] = { 'd', 'o', 'r', 's' };
static const symbol s_2_154[5] = { 'a', 'd', 'o', 'r', 's' };
static const symbol s_2_155[5] = { 'i', 'd', 'o', 'r', 's' };
static const symbol s_2_156[3] = { 'a', 't', 's' };
static const symbol s_2_157[5] = { 'i', 't', 'a', 't', 's' };
static const symbol s_2_158[8] = { 'b', 'i', 'l', 'i', 't', 'a', 't', 's' };
static const symbol s_2_159[7] = { 'i', 'v', 'i', 't', 'a', 't', 's' };
static const symbol s_2_160[9] = { 'a', 't', 'i', 'v', 'i', 't', 'a', 't', 's' };
static const symbol s_2_161[6] = { 0xC3, 0xAF, 't', 'a', 't', 's' };
static const symbol s_2_162[3] = { 'e', 't', 's' };
static const symbol s_2_163[4] = { 'a', 'n', 't', 's' };
static const symbol s_2_164[4] = { 'e', 'n', 't', 's' };
static const symbol s_2_165[5] = { 'm', 'e', 'n', 't', 's' };
static const symbol s_2_166[6] = { 'a', 'm', 'e', 'n', 't', 's' };
static const symbol s_2_167[3] = { 'o', 't', 's' };
static const symbol s_2_168[3] = { 'u', 't', 's' };
static const symbol s_2_169[3] = { 'i', 'u', 's' };
static const symbol s_2_170[5] = { 't', 'r', 'i', 'u', 's' };
static const symbol s_2_171[5] = { 'a', 't', 'i', 'u', 's' };
static const symbol s_2_172[3] = { 0xC3, 0xA8, 's' };
static const symbol s_2_173[3] = { 0xC3, 0xA9, 's' };
static const symbol s_2_174[3] = { 0xC3, 0xAD, 's' };
static const symbol s_2_175[4] = { 'd', 0xC3, 0xAD, 's' };
static const symbol s_2_176[3] = { 0xC3, 0xB3, 's' };
static const symbol s_2_177[4] = { 'i', 't', 'a', 't' };
static const symbol s_2_178[7] = { 'b', 'i', 'l', 'i', 't', 'a', 't' };
static const symbol s_2_179[6] = { 'i', 'v', 'i', 't', 'a', 't' };
static const symbol s_2_180[8] = { 'a', 't', 'i', 'v', 'i', 't', 'a', 't' };
static const symbol s_2_181[5] = { 0xC3, 0xAF, 't', 'a', 't' };
static const symbol s_2_182[2] = { 'e', 't' };
static const symbol s_2_183[3] = { 'a', 'n', 't' };
static const symbol s_2_184[3] = { 'e', 'n', 't' };
static const symbol s_2_185[4] = { 'i', 'e', 'n', 't' };
static const symbol s_2_186[4] = { 'm', 'e', 'n', 't' };
static const symbol s_2_187[5] = { 'a', 'm', 'e', 'n', 't' };
static const symbol s_2_188[7] = { 'i', 's', 'a', 'm', 'e', 'n', 't' };
static const symbol s_2_189[2] = { 'o', 't' };
static const symbol s_2_190[5] = { 'i', 's', 's', 'e', 'u' };
static const symbol s_2_191[6] = { 0xC3, 0xAC, 's', 's', 'e', 'u' };
static const symbol s_2_192[6] = { 0xC3, 0xAD, 's', 's', 'e', 'u' };
static const symbol s_2_193[4] = { 't', 'r', 'i', 'u' };
static const symbol s_2_194[6] = { 0xC3, 0xAD, 's', 's', 'i', 'u' };
static const symbol s_2_195[4] = { 'a', 't', 'i', 'u' };
static const symbol s_2_196[2] = { 0xC3, 0xB3 };
static const symbol s_2_197[3] = { 'i', 0xC3, 0xB3 };
static const symbol s_2_198[4] = { 'c', 'i', 0xC3, 0xB3 };
static const symbol s_2_199[5] = { 'a', 'c', 'i', 0xC3, 0xB3 };
static const struct among a_2[200] = {
{ 3, s_2_0, 0, 4, 0},
{ 7, s_2_1, -1, 3, 0},
{ 4, s_2_2, 0, 1, 0},
{ 3, s_2_3, 0, 2, 0},
{ 5, s_2_4, 0, 1, 0},
{ 5, s_2_5, 0, 1, 0},
{ 6, s_2_6, 0, 1, 0},
{ 5, s_2_7, 0, 1, 0},
{ 5, s_2_8, 0, 3, 0},
{ 4, s_2_9, 0, 1, 0},
{ 6, s_2_10, -1, 1, 0},
{ 4, s_2_11, 0, 1, 0},
{ 5, s_2_12, 0, 1, 0},
{ 7, s_2_13, 0, 1, 0},
{ 4, s_2_14, 0, 1, 0},
{ 4, s_2_15, 0, 1, 0},
{ 6, s_2_16, 0, 1, 0},
{ 3, s_2_17, 0, 1, 0},
{ 7, s_2_18, -1, 1, 0},
{ 9, s_2_19, -1, 5, 0},
{ 3, s_2_20, 0, 1, 0},
{ 3, s_2_21, 0, 1, 0},
{ 3, s_2_22, 0, 1, 0},
{ 5, s_2_23, -1, 1, 0},
{ 3, s_2_24, 0, 1, 0},
{ 4, s_2_25, -1, 1, 0},
{ 5, s_2_26, -1, 1, 0},
{ 5, s_2_27, 0, 1, 0},
{ 3, s_2_28, 0, 1, 0},
{ 3, s_2_29, 0, 1, 0},
{ 4, s_2_30, 0, 1, 0},
{ 4, s_2_31, 0, 1, 0},
{ 4, s_2_32, 0, 1, 0},
{ 3, s_2_33, 0, 1, 0},
{ 3, s_2_34, 0, 1, 0},
{ 3, s_2_35, 0, 1, 0},
{ 4, s_2_36, 0, 1, 0},
{ 7, s_2_37, -1, 1, 0},
{ 7, s_2_38, -2, 1, 0},
{ 3, s_2_39, 0, 1, 0},
{ 5, s_2_40, -1, 1, 0},
{ 4, s_2_41, 0, 1, 0},
{ 6, s_2_42, 0, 3, 0},
{ 2, s_2_43, 0, 4, 0},
{ 6, s_2_44, -1, 1, 0},
{ 3, s_2_45, 0, 1, 0},
{ 3, s_2_46, 0, 1, 0},
{ 2, s_2_47, 0, 1, 0},
{ 4, s_2_48, 0, 1, 0},
{ 3, s_2_49, 0, 1, 0},
{ 4, s_2_50, -1, 1, 0},
{ 4, s_2_51, -2, 1, 0},
{ 4, s_2_52, 0, 1, 0},
{ 7, s_2_53, -1, 1, 0},
{ 7, s_2_54, -2, 1, 0},
{ 6, s_2_55, -3, 1, 0},
{ 4, s_2_56, 0, 1, 0},
{ 4, s_2_57, 0, 1, 0},
{ 4, s_2_58, 0, 1, 0},
{ 3, s_2_59, 0, 1, 0},
{ 4, s_2_60, 0, 1, 0},
{ 4, s_2_61, 0, 3, 0},
{ 3, s_2_62, 0, 1, 0},
{ 4, s_2_63, 0, 1, 0},
{ 2, s_2_64, 0, 1, 0},
{ 2, s_2_65, 0, 1, 0},
{ 3, s_2_66, 0, 1, 0},
{ 3, s_2_67, 0, 1, 0},
{ 5, s_2_68, 0, 1, 0},
{ 4, s_2_69, 0, 1, 0},
{ 5, s_2_70, 0, 1, 0},
{ 6, s_2_71, 0, 1, 0},
{ 6, s_2_72, 0, 1, 0},
{ 6, s_2_73, 0, 1, 0},
{ 8, s_2_74, -1, 5, 0},
{ 4, s_2_75, 0, 1, 0},
{ 6, s_2_76, 0, 1, 0},
{ 2, s_2_77, 0, 1, 0},
{ 6, s_2_78, -1, 1, 0},
{ 4, s_2_79, -2, 1, 0},
{ 4, s_2_80, -3, 1, 0},
{ 4, s_2_81, -4, 1, 0},
{ 5, s_2_82, -5, 1, 0},
{ 3, s_2_83, 0, 1, 0},
{ 2, s_2_84, 0, 1, 0},
{ 3, s_2_85, -1, 1, 0},
{ 3, s_2_86, 0, 1, 0},
{ 5, s_2_87, 0, 1, 0},
{ 3, s_2_88, 0, 4, 0},
{ 7, s_2_89, -1, 3, 0},
{ 3, s_2_90, 0, 1, 0},
{ 4, s_2_91, 0, 1, 0},
{ 4, s_2_92, 0, 2, 0},
{ 6, s_2_93, 0, 1, 0},
{ 6, s_2_94, 0, 1, 0},
{ 7, s_2_95, 0, 1, 0},
{ 6, s_2_96, 0, 1, 0},
{ 6, s_2_97, 0, 3, 0},
{ 5, s_2_98, 0, 1, 0},
{ 6, s_2_99, 0, 1, 0},
{ 5, s_2_100, 0, 1, 0},
{ 6, s_2_101, 0, 1, 0},
{ 8, s_2_102, 0, 1, 0},
{ 4, s_2_103, 0, 1, 0},
{ 5, s_2_104, -1, 1, 0},
{ 5, s_2_105, -2, 1, 0},
{ 4, s_2_106, 0, 1, 0},
{ 8, s_2_107, -1, 1, 0},
{ 10, s_2_108, -1, 5, 0},
{ 6, s_2_109, 0, 1, 0},
{ 5, s_2_110, 0, 1, 0},
{ 8, s_2_111, -1, 1, 0},
{ 4, s_2_112, 0, 1, 0},
{ 4, s_2_113, 0, 1, 0},
{ 4, s_2_114, 0, 1, 0},
{ 5, s_2_115, -1, 1, 0},
{ 6, s_2_116, -1, 1, 0},
{ 5, s_2_117, 0, 1, 0},
{ 4, s_2_118, 0, 1, 0},
{ 4, s_2_119, 0, 1, 0},
{ 5, s_2_120, 0, 1, 0},
{ 5, s_2_121, 0, 1, 0},
{ 4, s_2_122, 0, 1, 0},
{ 4, s_2_123, 0, 1, 0},
{ 5, s_2_124, 0, 1, 0},
{ 8, s_2_125, -1, 1, 0},
{ 8, s_2_126, -2, 1, 0},
{ 5, s_2_127, 0, 4, 0},
{ 9, s_2_128, -1, 3, 0},
{ 4, s_2_129, 0, 1, 0},
{ 6, s_2_130, -1, 1, 0},
{ 7, s_2_131, 0, 3, 0},
{ 10, s_2_132, 0, 1, 0},
{ 4, s_2_133, 0, 1, 0},
{ 5, s_2_134, 0, 1, 0},
{ 5, s_2_135, 0, 3, 0},
{ 4, s_2_136, 0, 1, 0},
{ 5, s_2_137, 0, 1, 0},
{ 2, s_2_138, 0, 1, 0},
{ 3, s_2_139, -1, 1, 0},
{ 4, s_2_140, -2, 1, 0},
{ 3, s_2_141, 0, 1, 0},
{ 7, s_2_142, -1, 1, 0},
{ 9, s_2_143, -1, 5, 0},
{ 4, s_2_144, 0, 1, 0},
{ 5, s_2_145, -1, 1, 0},
{ 6, s_2_146, -1, 2, 0},
{ 4, s_2_147, 0, 1, 0},
{ 4, s_2_148, 0, 1, 0},
{ 5, s_2_149, 0, 1, 0},
{ 5, s_2_150, 0, 1, 0},
{ 3, s_2_151, 0, 1, 0},
{ 3, s_2_152, 0, 1, 0},
{ 4, s_2_153, -1, 1, 0},
{ 5, s_2_154, -1, 1, 0},
{ 5, s_2_155, -2, 1, 0},
{ 3, s_2_156, 0, 1, 0},
{ 5, s_2_157, -1, 1, 0},
{ 8, s_2_158, -1, 1, 0},
{ 7, s_2_159, -2, 1, 0},
{ 9, s_2_160, -1, 1, 0},
{ 6, s_2_161, -5, 1, 0},
{ 3, s_2_162, 0, 1, 0},
{ 4, s_2_163, 0, 1, 0},
{ 4, s_2_164, 0, 1, 0},
{ 5, s_2_165, -1, 1, 0},
{ 6, s_2_166, -1, 1, 0},
{ 3, s_2_167, 0, 1, 0},
{ 3, s_2_168, 0, 1, 0},
{ 3, s_2_169, 0, 1, 0},
{ 5, s_2_170, -1, 1, 0},
{ 5, s_2_171, -2, 1, 0},
{ 3, s_2_172, 0, 1, 0},
{ 3, s_2_173, 0, 1, 0},
{ 3, s_2_174, 0, 1, 0},
{ 4, s_2_175, -1, 1, 0},
{ 3, s_2_176, 0, 1, 0},
{ 4, s_2_177, 0, 1, 0},
{ 7, s_2_178, -1, 1, 0},
{ 6, s_2_179, -2, 1, 0},
{ 8, s_2_180, -1, 1, 0},
{ 5, s_2_181, 0, 1, 0},
{ 2, s_2_182, 0, 1, 0},
{ 3, s_2_183, 0, 1, 0},
{ 3, s_2_184, 0, 1, 0},
{ 4, s_2_185, -1, 1, 0},
{ 4, s_2_186, -2, 1, 0},
{ 5, s_2_187, -1, 1, 0},
{ 7, s_2_188, -1, 1, 0},
{ 2, s_2_189, 0, 1, 0},
{ 5, s_2_190, 0, 1, 0},
{ 6, s_2_191, 0, 1, 0},
{ 6, s_2_192, 0, 1, 0},
{ 4, s_2_193, 0, 1, 0},
{ 6, s_2_194, 0, 1, 0},
{ 4, s_2_195, 0, 1, 0},
{ 2, s_2_196, 0, 1, 0},
{ 3, s_2_197, -1, 1, 0},
{ 4, s_2_198, -1, 1, 0},
{ 5, s_2_199, -1, 1, 0}
};

static const symbol s_3_0[3] = { 'a', 'b', 'a' };
static const symbol s_3_1[4] = { 'e', 's', 'c', 'a' };
static const symbol s_3_2[4] = { 'i', 's', 'c', 'a' };
static const symbol s_3_3[5] = { 0xC3, 0xAF, 's', 'c', 'a' };
static const symbol s_3_4[3] = { 'a', 'd', 'a' };
static const symbol s_3_5[3] = { 'i', 'd', 'a' };
static const symbol s_3_6[3] = { 'u', 'd', 'a' };
static const symbol s_3_7[4] = { 0xC3, 0xAF, 'd', 'a' };
static const symbol s_3_8[2] = { 'i', 'a' };
static const symbol s_3_9[4] = { 'a', 'r', 'i', 'a' };
static const symbol s_3_10[4] = { 'i', 'r', 'i', 'a' };
static const symbol s_3_11[3] = { 'a', 'r', 'a' };
static const symbol s_3_12[4] = { 'i', 'e', 'r', 'a' };
static const symbol s_3_13[3] = { 'i', 'r', 'a' };
static const symbol s_3_14[5] = { 'a', 'd', 'o', 'r', 'a' };
static const symbol s_3_15[4] = { 0xC3, 0xAF, 'r', 'a' };
static const symbol s_3_16[3] = { 'a', 'v', 'a' };
static const symbol s_3_17[3] = { 'i', 'x', 'a' };
static const symbol s_3_18[4] = { 'i', 't', 'z', 'a' };
static const symbol s_3_19[3] = { 0xC3, 0xAD, 'a' };
static const symbol s_3_20[5] = { 'a', 'r', 0xC3, 0xAD, 'a' };
static const symbol s_3_21[5] = { 'e', 'r', 0xC3, 0xAD, 'a' };
static const symbol s_3_22[5] = { 'i', 'r', 0xC3, 0xAD, 'a' };
static const symbol s_3_23[3] = { 0xC3, 0xAF, 'a' };
static const symbol s_3_24[3] = { 'i', 's', 'c' };
static const symbol s_3_25[4] = { 0xC3, 0xAF, 's', 'c' };
static const symbol s_3_26[2] = { 'a', 'd' };
static const symbol s_3_27[2] = { 'e', 'd' };
static const symbol s_3_28[2] = { 'i', 'd' };
static const symbol s_3_29[2] = { 'i', 'e' };
static const symbol s_3_30[2] = { 'r', 'e' };
static const symbol s_3_31[3] = { 'd', 'r', 'e' };
static const symbol s_3_32[3] = { 'a', 's', 'e' };
static const symbol s_3_33[4] = { 'i', 'e', 's', 'e' };
static const symbol s_3_34[4] = { 'a', 's', 't', 'e' };
static const symbol s_3_35[4] = { 'i', 's', 't', 'e' };
static const symbol s_3_36[2] = { 'i', 'i' };
static const symbol s_3_37[3] = { 'i', 'n', 'i' };
static const symbol s_3_38[5] = { 'e', 's', 'q', 'u', 'i' };
static const symbol s_3_39[4] = { 'e', 'i', 'x', 'i' };
static const symbol s_3_40[4] = { 'i', 't', 'z', 'i' };
static const symbol s_3_41[2] = { 'a', 'm' };
static const symbol s_3_42[2] = { 'e', 'm' };
static const symbol s_3_43[4] = { 'a', 'r', 'e', 'm' };
static const symbol s_3_44[4] = { 'i', 'r', 'e', 'm' };
static const symbol s_3_45[5] = { 0xC3, 0xA0, 'r', 'e', 'm' };
static const symbol s_3_46[5] = { 0xC3, 0xAD, 'r', 'e', 'm' };
static const symbol s_3_47[6] = { 0xC3, 0xA0, 's', 's', 'e', 'm' };
static const symbol s_3_48[6] = { 0xC3, 0xA9, 's', 's', 'e', 'm' };
static const symbol s_3_49[5] = { 'i', 'g', 'u', 'e', 'm' };
static const symbol s_3_50[6] = { 0xC3, 0xAF, 'g', 'u', 'e', 'm' };
static const symbol s_3_51[4] = { 'a', 'v', 'e', 'm' };
static const symbol s_3_52[5] = { 0xC3, 0xA0, 'v', 'e', 'm' };
static const symbol s_3_53[5] = { 0xC3, 0xA1, 'v', 'e', 'm' };
static const symbol s_3_54[6] = { 'i', 'r', 0xC3, 0xAC, 'e', 'm' };
static const symbol s_3_55[4] = { 0xC3, 0xAD, 'e', 'm' };
static const symbol s_3_56[6] = { 'a', 'r', 0xC3, 0xAD, 'e', 'm' };
static const symbol s_3_57[6] = { 'i', 'r', 0xC3, 0xAD, 'e', 'm' };
static const symbol s_3_58[5] = { 'a', 's', 's', 'i', 'm' };
static const symbol s_3_59[5] = { 'e', 's', 's', 'i', 'm' };
static const symbol s_3_60[5] = { 'i', 's', 's', 'i', 'm' };
static const symbol s_3_61[6] = { 0xC3, 0xA0, 's', 's', 'i', 'm' };
static const symbol s_3_62[6] = { 0xC3, 0xA8, 's', 's', 'i', 'm' };
static const symbol s_3_63[6] = { 0xC3, 0xA9, 's', 's', 'i', 'm' };
static const symbol s_3_64[6] = { 0xC3, 0xAD, 's', 's', 'i', 'm' };
static const symbol s_3_65[3] = { 0xC3, 0xAF, 'm' };
static const symbol s_3_66[2] = { 'a', 'n' };
static const symbol s_3_67[4] = { 'a', 'b', 'a', 'n' };
static const symbol s_3_68[5] = { 'a', 'r', 'i', 'a', 'n' };
static const symbol s_3_69[4] = { 'a', 'r', 'a', 'n' };
static const symbol s_3_70[5] = { 'i', 'e', 'r', 'a', 'n' };
static const symbol s_3_71[4] = { 'i', 'r', 'a', 'n' };
static const symbol s_3_72[4] = { 0xC3, 0xAD, 'a', 'n' };
static const symbol s_3_73[6] = { 'a', 'r', 0xC3, 0xAD, 'a', 'n' };
static const symbol s_3_74[6] = { 'e', 'r', 0xC3, 0xAD, 'a', 'n' };
static const symbol s_3_75[6] = { 'i', 'r', 0xC3, 0xAD, 'a', 'n' };
static const symbol s_3_76[2] = { 'e', 'n' };
static const symbol s_3_77[3] = { 'i', 'e', 'n' };
static const symbol s_3_78[5] = { 'a', 'r', 'i', 'e', 'n' };
static const symbol s_3_79[5] = { 'i', 'r', 'i', 'e', 'n' };
static const symbol s_3_80[4] = { 'a', 'r', 'e', 'n' };
static const symbol s_3_81[4] = { 'e', 'r', 'e', 'n' };
static const symbol s_3_82[4] = { 'i', 'r', 'e', 'n' };
static const symbol s_3_83[5] = { 0xC3, 0xA0, 'r', 'e', 'n' };
static const symbol s_3_84[5] = { 0xC3, 0xAF, 'r', 'e', 'n' };
static const symbol s_3_85[4] = { 'a', 's', 'e', 'n' };
static const symbol s_3_86[5] = { 'i', 'e', 's', 'e', 'n' };
static const symbol s_3_87[5] = { 'a', 's', 's', 'e', 'n' };
static const symbol s_3_88[5] = { 'e', 's', 's', 'e', 'n' };
static const symbol s_3_89[5] = { 'i', 's', 's', 'e', 'n' };
static const symbol s_3_90[6] = { 0xC3, 0xA9, 's', 's', 'e', 'n' };
static const symbol s_3_91[6] = { 0xC3, 0xAF, 's', 's', 'e', 'n' };
static const symbol s_3_92[6] = { 'e', 's', 'q', 'u', 'e', 'n' };
static const symbol s_3_93[6] = { 'i', 's', 'q', 'u', 'e', 'n' };
static const symbol s_3_94[7] = { 0xC3, 0xAF, 's', 'q', 'u', 'e', 'n' };
static const symbol s_3_95[4] = { 'a', 'v', 'e', 'n' };
static const symbol s_3_96[4] = { 'i', 'x', 'e', 'n' };
static const symbol s_3_97[5] = { 'e', 'i', 'x', 'e', 'n' };
static const symbol s_3_98[5] = { 0xC3, 0xAF, 'x', 'e', 'n' };
static const symbol s_3_99[4] = { 0xC3, 0xAF, 'e', 'n' };
static const symbol s_3_100[2] = { 'i', 'n' };
static const symbol s_3_101[4] = { 'i', 'n', 'i', 'n' };
static const symbol s_3_102[3] = { 's', 'i', 'n' };
static const symbol s_3_103[4] = { 'i', 's', 'i', 'n' };
static const symbol s_3_104[5] = { 'a', 's', 's', 'i', 'n' };
static const symbol s_3_105[5] = { 'e', 's', 's', 'i', 'n' };
static const symbol s_3_106[5] = { 'i', 's', 's', 'i', 'n' };
static const symbol s_3_107[6] = { 0xC3, 0xAF, 's', 's', 'i', 'n' };
static const symbol s_3_108[6] = { 'e', 's', 'q', 'u', 'i', 'n' };
static const symbol s_3_109[5] = { 'e', 'i', 'x', 'i', 'n' };
static const symbol s_3_110[4] = { 'a', 'r', 'o', 'n' };
static const symbol s_3_111[5] = { 'i', 'e', 'r', 'o', 'n' };
static const symbol s_3_112[5] = { 'a', 'r', 0xC3, 0xA1, 'n' };
static const symbol s_3_113[5] = { 'e', 'r', 0xC3, 0xA1, 'n' };
static const symbol s_3_114[5] = { 'i', 'r', 0xC3, 0xA1, 'n' };
static const symbol s_3_115[4] = { 'i', 0xC3, 0xAF, 'n' };
static const symbol s_3_116[3] = { 'a', 'd', 'o' };
static const symbol s_3_117[3] = { 'i', 'd', 'o' };
static const symbol s_3_118[4] = { 'a', 'n', 'd', 'o' };
static const symbol s_3_119[5] = { 'i', 'e', 'n', 'd', 'o' };
static const symbol s_3_120[2] = { 'i', 'o' };
static const symbol s_3_121[3] = { 'i', 'x', 'o' };
static const symbol s_3_122[4] = { 'e', 'i', 'x', 'o' };
static const symbol s_3_123[4] = { 0xC3, 0xAF, 'x', 'o' };
static const symbol s_3_124[4] = { 'i', 't', 'z', 'o' };
static const symbol s_3_125[2] = { 'a', 'r' };
static const symbol s_3_126[4] = { 't', 'z', 'a', 'r' };
static const symbol s_3_127[2] = { 'e', 'r' };
static const symbol s_3_128[5] = { 'e', 'i', 'x', 'e', 'r' };
static const symbol s_3_129[2] = { 'i', 'r' };
static const symbol s_3_130[4] = { 'a', 'd', 'o', 'r' };
static const symbol s_3_131[2] = { 'a', 's' };
static const symbol s_3_132[4] = { 'a', 'b', 'a', 's' };
static const symbol s_3_133[4] = { 'a', 'd', 'a', 's' };
static const symbol s_3_134[4] = { 'i', 'd', 'a', 's' };
static const symbol s_3_135[4] = { 'a', 'r', 'a', 's' };
static const symbol s_3_136[5] = { 'i', 'e', 'r', 'a', 's' };
static const symbol s_3_137[4] = { 0xC3, 0xAD, 'a', 's' };
static const symbol s_3_138[6] = { 'a', 'r', 0xC3, 0xAD, 'a', 's' };
static const symbol s_3_139[6] = { 'e', 'r', 0xC3, 0xAD, 'a', 's' };
static const symbol s_3_140[6] = { 'i', 'r', 0xC3, 0xAD, 'a', 's' };
static const symbol s_3_141[3] = { 'i', 'd', 's' };
static const symbol s_3_142[2] = { 'e', 's' };
static const symbol s_3_143[4] = { 'a', 'd', 'e', 's' };
static const symbol s_3_144[4] = { 'i', 'd', 'e', 's' };
static const symbol s_3_145[4] = { 'u', 'd', 'e', 's' };
static const symbol s_3_146[5] = { 0xC3, 0xAF, 'd', 'e', 's' };
static const symbol s_3_147[5] = { 'a', 't', 'g', 'e', 's' };
static const symbol s_3_148[3] = { 'i', 'e', 's' };
static const symbol s_3_149[5] = { 'a', 'r', 'i', 'e', 's' };
static const symbol s_3_150[5] = { 'i', 'r', 'i', 'e', 's' };
static const symbol s_3_151[4] = { 'a', 'r', 'e', 's' };
static const symbol s_3_152[4] = { 'i', 'r', 'e', 's' };
static const symbol s_3_153[6] = { 'a', 'd', 'o', 'r', 'e', 's' };
static const symbol s_3_154[5] = { 0xC3, 0xAF, 'r', 'e', 's' };
static const symbol s_3_155[4] = { 'a', 's', 'e', 's' };
static const symbol s_3_156[5] = { 'i', 'e', 's', 'e', 's' };
static const symbol s_3_157[5] = { 'a', 's', 's', 'e', 's' };
static const symbol s_3_158[5] = { 'e', 's', 's', 'e', 's' };
static const symbol s_3_159[5] = { 'i', 's', 's', 'e', 's' };
static const symbol s_3_160[6] = { 0xC3, 0xAF, 's', 's', 'e', 's' };
static const symbol s_3_161[4] = { 'q', 'u', 'e', 's' };
static const symbol s_3_162[6] = { 'e', 's', 'q', 'u', 'e', 's' };
static const symbol s_3_163[7] = { 0xC3, 0xAF, 's', 'q', 'u', 'e', 's' };
static const symbol s_3_164[4] = { 'a', 'v', 'e', 's' };
static const symbol s_3_165[4] = { 'i', 'x', 'e', 's' };
static const symbol s_3_166[5] = { 'e', 'i', 'x', 'e', 's' };
static const symbol s_3_167[5] = { 0xC3, 0xAF, 'x', 'e', 's' };
static const symbol s_3_168[4] = { 0xC3, 0xAF, 'e', 's' };
static const symbol s_3_169[5] = { 'a', 'b', 'a', 'i', 's' };
static const symbol s_3_170[5] = { 'a', 'r', 'a', 'i', 's' };
static const symbol s_3_171[6] = { 'i', 'e', 'r', 'a', 'i', 's' };
static const symbol s_3_172[5] = { 0xC3, 0xAD, 'a', 'i', 's' };
static const symbol s_3_173[7] = { 'a', 'r', 0xC3, 0xAD, 'a', 'i', 's' };
static const symbol s_3_174[7] = { 'e', 'r', 0xC3, 0xAD, 'a', 'i', 's' };
static const symbol s_3_175[7] = { 'i', 'r', 0xC3, 0xAD, 'a', 'i', 's' };
static const symbol s_3_176[5] = { 'a', 's', 'e', 'i', 's' };
static const symbol s_3_177[6] = { 'i', 'e', 's', 'e', 'i', 's' };
static const symbol s_3_178[6] = { 'a', 's', 't', 'e', 'i', 's' };
static const symbol s_3_179[6] = { 'i', 's', 't', 'e', 'i', 's' };
static const symbol s_3_180[4] = { 'i', 'n', 'i', 's' };
static const symbol s_3_181[3] = { 's', 'i', 's' };
static const symbol s_3_182[4] = { 'i', 's', 'i', 's' };
static const symbol s_3_183[5] = { 'a', 's', 's', 'i', 's' };
static const symbol s_3_184[5] = { 'e', 's', 's', 'i', 's' };
static const symbol s_3_185[5] = { 'i', 's', 's', 'i', 's' };
static const symbol s_3_186[6] = { 0xC3, 0xAF, 's', 's', 'i', 's' };
static const symbol s_3_187[6] = { 'e', 's', 'q', 'u', 'i', 's' };
static const symbol s_3_188[5] = { 'e', 'i', 'x', 'i', 's' };
static const symbol s_3_189[5] = { 'i', 't', 'z', 'i', 's' };
static const symbol s_3_190[4] = { 0xC3, 0xA1, 'i', 's' };
static const symbol s_3_191[6] = { 'a', 'r', 0xC3, 0xA9, 'i', 's' };
static const symbol s_3_192[6] = { 'e', 'r', 0xC3, 0xA9, 'i', 's' };
static const symbol s_3_193[6] = { 'i', 'r', 0xC3, 0xA9, 'i', 's' };
static const symbol s_3_194[3] = { 'a', 'm', 's' };
static const symbol s_3_195[4] = { 'a', 'd', 'o', 's' };
static const symbol s_3_196[4] = { 'i', 'd', 'o', 's' };
static const symbol s_3_197[4] = { 'a', 'm', 'o', 's' };
static const symbol s_3_198[7] = { 0xC3, 0xA1, 'b', 'a', 'm', 'o', 's' };
static const symbol s_3_199[7] = { 0xC3, 0xA1, 'r', 'a', 'm', 'o', 's' };
static const symbol s_3_200[8] = { 'i', 0xC3, 0xA9, 'r', 'a', 'm', 'o', 's' };
static const symbol s_3_201[6] = { 0xC3, 0xAD, 'a', 'm', 'o', 's' };
static const symbol s_3_202[8] = { 'a', 'r', 0xC3, 0xAD, 'a', 'm', 'o', 's' };
static const symbol s_3_203[8] = { 'e', 'r', 0xC3, 0xAD, 'a', 'm', 'o', 's' };
static const symbol s_3_204[8] = { 'i', 'r', 0xC3, 0xAD, 'a', 'm', 'o', 's' };
static const symbol s_3_205[6] = { 'a', 'r', 'e', 'm', 'o', 's' };
static const symbol s_3_206[6] = { 'e', 'r', 'e', 'm', 'o', 's' };
static const symbol s_3_207[6] = { 'i', 'r', 'e', 'm', 'o', 's' };
static const symbol s_3_208[7] = { 0xC3, 0xA1, 's', 'e', 'm', 'o', 's' };
static const symbol s_3_209[8] = { 'i', 0xC3, 0xA9, 's', 'e', 'm', 'o', 's' };
static const symbol s_3_210[4] = { 'i', 'm', 'o', 's' };
static const symbol s_3_211[5] = { 'a', 'd', 'o', 'r', 's' };
static const symbol s_3_212[3] = { 'a', 's', 's' };
static const symbol s_3_213[5] = { 'e', 'r', 'a', 's', 's' };
static const symbol s_3_214[3] = { 'e', 's', 's' };
static const symbol s_3_215[3] = { 'a', 't', 's' };
static const symbol s_3_216[3] = { 'i', 't', 's' };
static const symbol s_3_217[4] = { 'e', 'n', 't', 's' };
static const symbol s_3_218[3] = { 0xC3, 0xA0, 's' };
static const symbol s_3_219[5] = { 'a', 'r', 0xC3, 0xA0, 's' };
static const symbol s_3_220[5] = { 'i', 'r', 0xC3, 0xA0, 's' };
static const symbol s_3_221[5] = { 'a', 'r', 0xC3, 0xA1, 's' };
static const symbol s_3_222[5] = { 'e', 'r', 0xC3, 0xA1, 's' };
static const symbol s_3_223[5] = { 'i', 'r', 0xC3, 0xA1, 's' };
static const symbol s_3_224[3] = { 0xC3, 0xA9, 's' };
static const symbol s_3_225[5] = { 'a', 'r', 0xC3, 0xA9, 's' };
static const symbol s_3_226[3] = { 0xC3, 0xAD, 's' };
static const symbol s_3_227[4] = { 'i', 0xC3, 0xAF, 's' };
static const symbol s_3_228[2] = { 'a', 't' };
static const symbol s_3_229[2] = { 'i', 't' };
static const symbol s_3_230[3] = { 'a', 'n', 't' };
static const symbol s_3_231[3] = { 'e', 'n', 't' };
static const symbol s_3_232[3] = { 'i', 'n', 't' };
static const symbol s_3_233[2] = { 'u', 't' };
static const symbol s_3_234[3] = { 0xC3, 0xAF, 't' };
static const symbol s_3_235[2] = { 'a', 'u' };
static const symbol s_3_236[4] = { 'e', 'r', 'a', 'u' };
static const symbol s_3_237[3] = { 'i', 'e', 'u' };
static const symbol s_3_238[4] = { 'i', 'n', 'e', 'u' };
static const symbol s_3_239[4] = { 'a', 'r', 'e', 'u' };
static const symbol s_3_240[4] = { 'i', 'r', 'e', 'u' };
static const symbol s_3_241[5] = { 0xC3, 0xA0, 'r', 'e', 'u' };
static const symbol s_3_242[5] = { 0xC3, 0xAD, 'r', 'e', 'u' };
static const symbol s_3_243[5] = { 'a', 's', 's', 'e', 'u' };
static const symbol s_3_244[5] = { 'e', 's', 's', 'e', 'u' };
static const symbol s_3_245[7] = { 'e', 'r', 'e', 's', 's', 'e', 'u' };
static const symbol s_3_246[6] = { 0xC3, 0xA0, 's', 's', 'e', 'u' };
static const symbol s_3_247[6] = { 0xC3, 0xA9, 's', 's', 'e', 'u' };
static const symbol s_3_248[5] = { 'i', 'g', 'u', 'e', 'u' };
static const symbol s_3_249[6] = { 0xC3, 0xAF, 'g', 'u', 'e', 'u' };
static const symbol s_3_250[5] = { 0xC3, 0xA0, 'v', 'e', 'u' };
static const symbol s_3_251[5] = { 0xC3, 0xA1, 'v', 'e', 'u' };
static const symbol s_3_252[5] = { 'i', 't', 'z', 'e', 'u' };
static const symbol s_3_253[4] = { 0xC3, 0xAC, 'e', 'u' };
static const symbol s_3_254[6] = { 'i', 'r', 0xC3, 0xAC, 'e', 'u' };
static const symbol s_3_255[4] = { 0xC3, 0xAD, 'e', 'u' };
static const symbol s_3_256[6] = { 'a', 'r', 0xC3, 0xAD, 'e', 'u' };
static const symbol s_3_257[6] = { 'i', 'r', 0xC3, 0xAD, 'e', 'u' };
static const symbol s_3_258[5] = { 'a', 's', 's', 'i', 'u' };
static const symbol s_3_259[5] = { 'i', 's', 's', 'i', 'u' };
static const symbol s_3_260[6] = { 0xC3, 0xA0, 's', 's', 'i', 'u' };
static const symbol s_3_261[6] = { 0xC3, 0xA8, 's', 's', 'i', 'u' };
static const symbol s_3_262[6] = { 0xC3, 0xA9, 's', 's', 'i', 'u' };
static const symbol s_3_263[6] = { 0xC3, 0xAD, 's', 's', 'i', 'u' };
static const symbol s_3_264[3] = { 0xC3, 0xAF, 'u' };
static const symbol s_3_265[2] = { 'i', 'x' };
static const symbol s_3_266[3] = { 'e', 'i', 'x' };
static const symbol s_3_267[3] = { 0xC3, 0xAF, 'x' };
static const symbol s_3_268[3] = { 'i', 't', 'z' };
static const symbol s_3_269[3] = { 'i', 0xC3, 0xA0 };
static const symbol s_3_270[4] = { 'a', 'r', 0xC3, 0xA0 };
static const symbol s_3_271[4] = { 'i', 'r', 0xC3, 0xA0 };
static const symbol s_3_272[5] = { 'i', 't', 'z', 0xC3, 0xA0 };
static const symbol s_3_273[4] = { 'a', 'r', 0xC3, 0xA1 };
static const symbol s_3_274[4] = { 'e', 'r', 0xC3, 0xA1 };
static const symbol s_3_275[4] = { 'i', 'r', 0xC3, 0xA1 };
static const symbol s_3_276[4] = { 'i', 'r', 0xC3, 0xA8 };
static const symbol s_3_277[4] = { 'a', 'r', 0xC3, 0xA9 };
static const symbol s_3_278[4] = { 'e', 'r', 0xC3, 0xA9 };
static const symbol s_3_279[4] = { 'i', 'r', 0xC3, 0xA9 };
static const symbol s_3_280[2] = { 0xC3, 0xAD };
static const symbol s_3_281[3] = { 'i', 0xC3, 0xAF };
static const symbol s_3_282[3] = { 'i', 0xC3, 0xB3 };
static const struct among a_3[283] = {
{ 3, s_3_0, 0, 1, 0},
{ 4, s_3_1, 0, 1, 0},
{ 4, s_3_2, 0, 1, 0},
{ 5, s_3_3, 0, 1, 0},
{ 3, s_3_4, 0, 1, 0},
{ 3, s_3_5, 0, 1, 0},
{ 3, s_3_6, 0, 1, 0},
{ 4, s_3_7, 0, 1, 0},
{ 2, s_3_8, 0, 1, 0},
{ 4, s_3_9, -1, 1, 0},
{ 4, s_3_10, -2, 1, 0},
{ 3, s_3_11, 0, 1, 0},
{ 4, s_3_12, 0, 1, 0},
{ 3, s_3_13, 0, 1, 0},
{ 5, s_3_14, 0, 1, 0},
{ 4, s_3_15, 0, 1, 0},
{ 3, s_3_16, 0, 1, 0},
{ 3, s_3_17, 0, 1, 0},
{ 4, s_3_18, 0, 1, 0},
{ 3, s_3_19, 0, 1, 0},
{ 5, s_3_20, -1, 1, 0},
{ 5, s_3_21, -2, 1, 0},
{ 5, s_3_22, -3, 1, 0},
{ 3, s_3_23, 0, 1, 0},
{ 3, s_3_24, 0, 1, 0},
{ 4, s_3_25, 0, 1, 0},
{ 2, s_3_26, 0, 1, 0},
{ 2, s_3_27, 0, 1, 0},
{ 2, s_3_28, 0, 1, 0},
{ 2, s_3_29, 0, 1, 0},
{ 2, s_3_30, 0, 1, 0},
{ 3, s_3_31, -1, 1, 0},
{ 3, s_3_32, 0, 1, 0},
{ 4, s_3_33, 0, 1, 0},
{ 4, s_3_34, 0, 1, 0},
{ 4, s_3_35, 0, 1, 0},
{ 2, s_3_36, 0, 1, 0},
{ 3, s_3_37, 0, 1, 0},
{ 5, s_3_38, 0, 1, 0},
{ 4, s_3_39, 0, 1, 0},
{ 4, s_3_40, 0, 1, 0},
{ 2, s_3_41, 0, 1, 0},
{ 2, s_3_42, 0, 1, 0},
{ 4, s_3_43, -1, 1, 0},
{ 4, s_3_44, -2, 1, 0},
{ 5, s_3_45, -3, 1, 0},
{ 5, s_3_46, -4, 1, 0},
{ 6, s_3_47, -5, 1, 0},
{ 6, s_3_48, -6, 1, 0},
{ 5, s_3_49, -7, 1, 0},
{ 6, s_3_50, -8, 1, 0},
{ 4, s_3_51, -9, 1, 0},
{ 5, s_3_52, -10, 1, 0},
{ 5, s_3_53, -11, 1, 0},
{ 6, s_3_54, -12, 1, 0},
{ 4, s_3_55, -13, 1, 0},
{ 6, s_3_56, -1, 1, 0},
{ 6, s_3_57, -2, 1, 0},
{ 5, s_3_58, 0, 1, 0},
{ 5, s_3_59, 0, 1, 0},
{ 5, s_3_60, 0, 1, 0},
{ 6, s_3_61, 0, 1, 0},
{ 6, s_3_62, 0, 1, 0},
{ 6, s_3_63, 0, 1, 0},
{ 6, s_3_64, 0, 1, 0},
{ 3, s_3_65, 0, 1, 0},
{ 2, s_3_66, 0, 1, 0},
{ 4, s_3_67, -1, 1, 0},
{ 5, s_3_68, -2, 1, 0},
{ 4, s_3_69, -3, 1, 0},
{ 5, s_3_70, -4, 1, 0},
{ 4, s_3_71, -5, 1, 0},
{ 4, s_3_72, -6, 1, 0},
{ 6, s_3_73, -1, 1, 0},
{ 6, s_3_74, -2, 1, 0},
{ 6, s_3_75, -3, 1, 0},
{ 2, s_3_76, 0, 1, 0},
{ 3, s_3_77, -1, 1, 0},
{ 5, s_3_78, -1, 1, 0},
{ 5, s_3_79, -2, 1, 0},
{ 4, s_3_80, -4, 1, 0},
{ 4, s_3_81, -5, 1, 0},
{ 4, s_3_82, -6, 1, 0},
{ 5, s_3_83, -7, 1, 0},
{ 5, s_3_84, -8, 1, 0},
{ 4, s_3_85, -9, 1, 0},
{ 5, s_3_86, -10, 1, 0},
{ 5, s_3_87, -11, 1, 0},
{ 5, s_3_88, -12, 1, 0},
{ 5, s_3_89, -13, 1, 0},
{ 6, s_3_90, -14, 1, 0},
{ 6, s_3_91, -15, 1, 0},
{ 6, s_3_92, -16, 1, 0},
{ 6, s_3_93, -17, 1, 0},
{ 7, s_3_94, -18, 1, 0},
{ 4, s_3_95, -19, 1, 0},
{ 4, s_3_96, -20, 1, 0},
{ 5, s_3_97, -1, 1, 0},
{ 5, s_3_98, -22, 1, 0},
{ 4, s_3_99, -23, 1, 0},
{ 2, s_3_100, 0, 1, 0},
{ 4, s_3_101, -1, 1, 0},
{ 3, s_3_102, -2, 1, 0},
{ 4, s_3_103, -1, 1, 0},
{ 5, s_3_104, -2, 1, 0},
{ 5, s_3_105, -3, 1, 0},
{ 5, s_3_106, -4, 1, 0},
{ 6, s_3_107, -5, 1, 0},
{ 6, s_3_108, -8, 1, 0},
{ 5, s_3_109, -9, 1, 0},
{ 4, s_3_110, 0, 1, 0},
{ 5, s_3_111, 0, 1, 0},
{ 5, s_3_112, 0, 1, 0},
{ 5, s_3_113, 0, 1, 0},
{ 5, s_3_114, 0, 1, 0},
{ 4, s_3_115, 0, 1, 0},
{ 3, s_3_116, 0, 1, 0},
{ 3, s_3_117, 0, 1, 0},
{ 4, s_3_118, 0, 2, 0},
{ 5, s_3_119, 0, 1, 0},
{ 2, s_3_120, 0, 1, 0},
{ 3, s_3_121, 0, 1, 0},
{ 4, s_3_122, -1, 1, 0},
{ 4, s_3_123, 0, 1, 0},
{ 4, s_3_124, 0, 1, 0},
{ 2, s_3_125, 0, 1, 0},
{ 4, s_3_126, -1, 1, 0},
{ 2, s_3_127, 0, 1, 0},
{ 5, s_3_128, -1, 1, 0},
{ 2, s_3_129, 0, 1, 0},
{ 4, s_3_130, 0, 1, 0},
{ 2, s_3_131, 0, 1, 0},
{ 4, s_3_132, -1, 1, 0},
{ 4, s_3_133, -2, 1, 0},
{ 4, s_3_134, -3, 1, 0},
{ 4, s_3_135, -4, 1, 0},
{ 5, s_3_136, -5, 1, 0},
{ 4, s_3_137, -6, 1, 0},
{ 6, s_3_138, -1, 1, 0},
{ 6, s_3_139, -2, 1, 0},
{ 6, s_3_140, -3, 1, 0},
{ 3, s_3_141, 0, 1, 0},
{ 2, s_3_142, 0, 1, 0},
{ 4, s_3_143, -1, 1, 0},
{ 4, s_3_144, -2, 1, 0},
{ 4, s_3_145, -3, 1, 0},
{ 5, s_3_146, -4, 1, 0},
{ 5, s_3_147, -5, 1, 0},
{ 3, s_3_148, -6, 1, 0},
{ 5, s_3_149, -1, 1, 0},
{ 5, s_3_150, -2, 1, 0},
{ 4, s_3_151, -9, 1, 0},
{ 4, s_3_152, -10, 1, 0},
{ 6, s_3_153, -11, 1, 0},
{ 5, s_3_154, -12, 1, 0},
{ 4, s_3_155, -13, 1, 0},
{ 5, s_3_156, -14, 1, 0},
{ 5, s_3_157, -15, 1, 0},
{ 5, s_3_158, -16, 1, 0},
{ 5, s_3_159, -17, 1, 0},
{ 6, s_3_160, -18, 1, 0},
{ 4, s_3_161, -19, 1, 0},
{ 6, s_3_162, -1, 1, 0},
{ 7, s_3_163, -2, 1, 0},
{ 4, s_3_164, -22, 1, 0},
{ 4, s_3_165, -23, 1, 0},
{ 5, s_3_166, -1, 1, 0},
{ 5, s_3_167, -25, 1, 0},
{ 4, s_3_168, -26, 1, 0},
{ 5, s_3_169, 0, 1, 0},
{ 5, s_3_170, 0, 1, 0},
{ 6, s_3_171, 0, 1, 0},
{ 5, s_3_172, 0, 1, 0},
{ 7, s_3_173, -1, 1, 0},
{ 7, s_3_174, -2, 1, 0},
{ 7, s_3_175, -3, 1, 0},
{ 5, s_3_176, 0, 1, 0},
{ 6, s_3_177, 0, 1, 0},
{ 6, s_3_178, 0, 1, 0},
{ 6, s_3_179, 0, 1, 0},
{ 4, s_3_180, 0, 1, 0},
{ 3, s_3_181, 0, 1, 0},
{ 4, s_3_182, -1, 1, 0},
{ 5, s_3_183, -2, 1, 0},
{ 5, s_3_184, -3, 1, 0},
{ 5, s_3_185, -4, 1, 0},
{ 6, s_3_186, -5, 1, 0},
{ 6, s_3_187, 0, 1, 0},
{ 5, s_3_188, 0, 1, 0},
{ 5, s_3_189, 0, 1, 0},
{ 4, s_3_190, 0, 1, 0},
{ 6, s_3_191, 0, 1, 0},
{ 6, s_3_192, 0, 1, 0},
{ 6, s_3_193, 0, 1, 0},
{ 3, s_3_194, 0, 1, 0},
{ 4, s_3_195, 0, 1, 0},
{ 4, s_3_196, 0, 1, 0},
{ 4, s_3_197, 0, 1, 0},
{ 7, s_3_198, -1, 1, 0},
{ 7, s_3_199, -2, 1, 0},
{ 8, s_3_200, -3, 1, 0},
{ 6, s_3_201, -4, 1, 0},
{ 8, s_3_202, -1, 1, 0},
{ 8, s_3_203, -2, 1, 0},
{ 8, s_3_204, -3, 1, 0},
{ 6, s_3_205, 0, 1, 0},
{ 6, s_3_206, 0, 1, 0},
{ 6, s_3_207, 0, 1, 0},
{ 7, s_3_208, 0, 1, 0},
{ 8, s_3_209, 0, 1, 0},
{ 4, s_3_210, 0, 1, 0},
{ 5, s_3_211, 0, 1, 0},
{ 3, s_3_212, 0, 1, 0},
{ 5, s_3_213, -1, 1, 0},
{ 3, s_3_214, 0, 1, 0},
{ 3, s_3_215, 0, 1, 0},
{ 3, s_3_216, 0, 1, 0},
{ 4, s_3_217, 0, 1, 0},
{ 3, s_3_218, 0, 1, 0},
{ 5, s_3_219, -1, 1, 0},
{ 5, s_3_220, -2, 1, 0},
{ 5, s_3_221, 0, 1, 0},
{ 5, s_3_222, 0, 1, 0},
{ 5, s_3_223, 0, 1, 0},
{ 3, s_3_224, 0, 1, 0},
{ 5, s_3_225, -1, 1, 0},
{ 3, s_3_226, 0, 1, 0},
{ 4, s_3_227, 0, 1, 0},
{ 2, s_3_228, 0, 1, 0},
{ 2, s_3_229, 0, 1, 0},
{ 3, s_3_230, 0, 1, 0},
{ 3, s_3_231, 0, 1, 0},
{ 3, s_3_232, 0, 1, 0},
{ 2, s_3_233, 0, 1, 0},
{ 3, s_3_234, 0, 1, 0},
{ 2, s_3_235, 0, 1, 0},
{ 4, s_3_236, -1, 1, 0},
{ 3, s_3_237, 0, 1, 0},
{ 4, s_3_238, 0, 1, 0},
{ 4, s_3_239, 0, 1, 0},
{ 4, s_3_240, 0, 1, 0},
{ 5, s_3_241, 0, 1, 0},
{ 5, s_3_242, 0, 1, 0},
{ 5, s_3_243, 0, 1, 0},
{ 5, s_3_244, 0, 1, 0},
{ 7, s_3_245, -1, 1, 0},
{ 6, s_3_246, 0, 1, 0},
{ 6, s_3_247, 0, 1, 0},
{ 5, s_3_248, 0, 1, 0},
{ 6, s_3_249, 0, 1, 0},
{ 5, s_3_250, 0, 1, 0},
{ 5, s_3_251, 0, 1, 0},
{ 5, s_3_252, 0, 1, 0},
{ 4, s_3_253, 0, 1, 0},
{ 6, s_3_254, -1, 1, 0},
{ 4, s_3_255, 0, 1, 0},
{ 6, s_3_256, -1, 1, 0},
{ 6, s_3_257, -2, 1, 0},
{ 5, s_3_258, 0, 1, 0},
{ 5, s_3_259, 0, 1, 0},
{ 6, s_3_260, 0, 1, 0},
{ 6, s_3_261, 0, 1, 0},
{ 6, s_3_262, 0, 1, 0},
{ 6, s_3_263, 0, 1, 0},
{ 3, s_3_264, 0, 1, 0},
{ 2, s_3_265, 0, 1, 0},
{ 3, s_3_266, -1, 1, 0},
{ 3, s_3_267, 0, 1, 0},
{ 3, s_3_268, 0, 1, 0},
{ 3, s_3_269, 0, 1, 0},
{ 4, s_3_270, 0, 1, 0},
{ 4, s_3_271, 0, 1, 0},
{ 5, s_3_272, 0, 1, 0},
{ 4, s_3_273, 0, 1, 0},
{ 4, s_3_274, 0, 1, 0},
{ 4, s_3_275, 0, 1, 0},
{ 4, s_3_276, 0, 1, 0},
{ 4, s_3_277, 0, 1, 0},
{ 4, s_3_278, 0, 1, 0},
{ 4, s_3_279, 0, 1, 0},
{ 2, s_3_280, 0, 1, 0},
{ 3, s_3_281, 0, 1, 0},
{ 3, s_3_282, 0, 1, 0}
};

static const symbol s_4_0[1] = { 'a' };
static const symbol s_4_1[1] = { 'e' };
static const symbol s_4_2[1] = { 'i' };
static const symbol s_4_3[3] = { 0xC3, 0xAF, 'n' };
static const symbol s_4_4[1] = { 'o' };
static const symbol s_4_5[2] = { 'i', 'r' };
static const symbol s_4_6[1] = { 's' };
static const symbol s_4_7[2] = { 'i', 's' };
static const symbol s_4_8[2] = { 'o', 's' };
static const symbol s_4_9[3] = { 0xC3, 0xAF, 's' };
static const symbol s_4_10[2] = { 'i', 't' };
static const symbol s_4_11[2] = { 'e', 'u' };
static const symbol s_4_12[2] = { 'i', 'u' };
static const symbol s_4_13[3] = { 'i', 'q', 'u' };
static const symbol s_4_14[3] = { 'i', 't', 'z' };
static const symbol s_4_15[2] = { 0xC3, 0xA0 };
static const symbol s_4_16[2] = { 0xC3, 0xA1 };
static const symbol s_4_17[2] = { 0xC3, 0xA9 };
static const symbol s_4_18[2] = { 0xC3, 0xAC };
static const symbol s_4_19[2] = { 0xC3, 0xAD };
static const symbol s_4_20[2] = { 0xC3, 0xAF };
static const symbol s_4_21[2] = { 0xC3, 0xB3 };
static const struct among a_4[22] = {
{ 1, s_4_0, 0, 1, 0},
{ 1, s_4_1, 0, 1, 0},
{ 1, s_4_2, 0, 1, 0},
{ 3, s_4_3, 0, 1, 0},
{ 1, s_4_4, 0, 1, 0},
{ 2, s_4_5, 0, 1, 0},
{ 1, s_4_6, 0, 1, 0},
{ 2, s_4_7, -1, 1, 0},
{ 2, s_4_8, -2, 1, 0},
{ 3, s_4_9, -3, 1, 0},
{ 2, s_4_10, 0, 1, 0},
{ 2, s_4_11, 0, 1, 0},
{ 2, s_4_12, 0, 1, 0},
{ 3, s_4_13, 0, 2, 0},
{ 3, s_4_14, 0, 1, 0},
{ 2, s_4_15, 0, 1, 0},
{ 2, s_4_16, 0, 1, 0},
{ 2, s_4_17, 0, 1, 0},
{ 2, s_4_18, 0, 1, 0},
{ 2, s_4_19, 0, 1, 0},
{ 2, s_4_20, 0, 1, 0},
{ 2, s_4_21, 0, 1, 0}
};

static const unsigned char g_v[] = { 17, 65, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 128, 129, 81, 6, 10 };

static int r_mark_regions(struct SN_env * z) {
    ((SN_local *)z)->i_p1 = z->l;
    ((SN_local *)z)->i_p2 = z->l;
    {
        int v_1 = z->c;
        {
            int ret = out_grouping_U(z, g_v, 97, 252, 1);
            if (ret < 0) goto lab0;
            z->c += ret;
        }
        {
            int ret = in_grouping_U(z, g_v, 97, 252, 1);
            if (ret < 0) goto lab0;
            z->c += ret;
        }
        ((SN_local *)z)->i_p1 = z->c;
        {
            int ret = out_grouping_U(z, g_v, 97, 252, 1);
            if (ret < 0) goto lab0;
            z->c += ret;
        }
        {
            int ret = in_grouping_U(z, g_v, 97, 252, 1);
            if (ret < 0) goto lab0;
            z->c += ret;
        }
        ((SN_local *)z)->i_p2 = z->c;
    lab0:
        z->c = v_1;
    }
    return 1;
}

static int r_cleaning(struct SN_env * z) {
    int among_var;
    while (1) {
        int v_1 = z->c;
        z->bra = z->c;
        if (z->c + 1 >= z->l || z->p[z->c + 1] >> 5 != 5 || !((344765187 >> (z->p[z->c + 1] & 0x1f)) & 1)) among_var = 7; else
        among_var = find_among(z, a_0, 13, 0);
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
                    int ret = slice_from_s(z, 1, s_5);
                    if (ret < 0) return ret;
                }
                break;
            case 7:
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

static int r_R1(struct SN_env * z) {
    return ((SN_local *)z)->i_p1 <= z->c;
}

static int r_R2(struct SN_env * z) {
    return ((SN_local *)z)->i_p2 <= z->c;
}

static int r_attached_pronoun(struct SN_env * z) {
    z->ket = z->c;
    if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((1634850 >> (z->p[z->c - 1] & 0x1f)) & 1)) return 0;
    if (!find_among_b(z, a_1, 39, 0)) return 0;
    z->bra = z->c;
    {
        int ret = r_R1(z);
        if (ret <= 0) return ret;
    }
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_standard_suffix(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    among_var = find_among_b(z, a_2, 200, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    switch (among_var) {
        case 1:
            {
                int ret = r_R1(z);
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
            break;
        case 3:
            {
                int ret = r_R2(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_from_s(z, 3, s_6);
                if (ret < 0) return ret;
            }
            break;
        case 4:
            {
                int ret = r_R2(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_from_s(z, 2, s_7);
                if (ret < 0) return ret;
            }
            break;
        case 5:
            {
                int ret = r_R1(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_from_s(z, 1, s_8);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_verb_suffix(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    among_var = find_among_b(z, a_3, 283, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    switch (among_var) {
        case 1:
            {
                int ret = r_R1(z);
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
            break;
    }
    return 1;
}

static int r_residual_suffix(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    among_var = find_among_b(z, a_4, 22, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    switch (among_var) {
        case 1:
            {
                int ret = r_R1(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {
                int ret = r_R1(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_from_s(z, 2, s_9);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

extern int catalan_UTF_8_stem(struct SN_env * z) {
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
            int ret = r_cleaning(z);
            if (ret < 0) return ret;
        }
        z->c = v_5;
    }
    return 1;
}

extern struct SN_env * catalan_UTF_8_create_env(void) {
    struct SN_env * z = SN_new_env(sizeof(SN_local));
    if (z) {
        ((SN_local *)z)->i_p2 = 0;
        ((SN_local *)z)->i_p1 = 0;
    }
    return z;
}

extern void catalan_UTF_8_close_env(struct SN_env * z) {
    SN_delete_env(z);
}

