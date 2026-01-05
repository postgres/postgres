/* Generated from basque.sbl by Snowball 3.0.0 - https://snowballstem.org/ */

#include "stem_ISO_8859_1_basque.h"

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
extern int basque_ISO_8859_1_stem(struct SN_env * z);
#ifdef __cplusplus
}
#endif

static int r_R1(struct SN_env * z);
static int r_R2(struct SN_env * z);
static int r_RV(struct SN_env * z);
static int r_mark_regions(struct SN_env * z);
static int r_adjetiboak(struct SN_env * z);
static int r_izenak(struct SN_env * z);
static int r_aditzak(struct SN_env * z);

static const symbol s_0[] = { 'j', 'o', 'k' };
static const symbol s_1[] = { 't', 'r', 'a' };
static const symbol s_2[] = { 'm', 'i', 'n', 'u', 't', 'u' };
static const symbol s_3[] = { 'z' };

static const symbol s_0_0[4] = { 'i', 'd', 'e', 'a' };
static const symbol s_0_1[5] = { 'b', 'i', 'd', 'e', 'a' };
static const symbol s_0_2[5] = { 'k', 'i', 'd', 'e', 'a' };
static const symbol s_0_3[5] = { 'p', 'i', 'd', 'e', 'a' };
static const symbol s_0_4[6] = { 'k', 'u', 'n', 'd', 'e', 'a' };
static const symbol s_0_5[5] = { 'g', 'a', 'l', 'e', 'a' };
static const symbol s_0_6[6] = { 't', 'a', 'i', 'l', 'e', 'a' };
static const symbol s_0_7[7] = { 't', 'z', 'a', 'i', 'l', 'e', 'a' };
static const symbol s_0_8[5] = { 'g', 'u', 'n', 'e', 'a' };
static const symbol s_0_9[5] = { 'k', 'u', 'n', 'e', 'a' };
static const symbol s_0_10[5] = { 't', 'z', 'a', 'g', 'a' };
static const symbol s_0_11[4] = { 'g', 'a', 'i', 'a' };
static const symbol s_0_12[5] = { 'a', 'l', 'd', 'i', 'a' };
static const symbol s_0_13[6] = { 't', 'a', 'l', 'd', 'i', 'a' };
static const symbol s_0_14[5] = { 'k', 'a', 'r', 'i', 'a' };
static const symbol s_0_15[6] = { 'g', 'a', 'r', 'r', 'i', 'a' };
static const symbol s_0_16[6] = { 'k', 'a', 'r', 'r', 'i', 'a' };
static const symbol s_0_17[2] = { 'k', 'a' };
static const symbol s_0_18[5] = { 't', 'z', 'a', 'k', 'a' };
static const symbol s_0_19[2] = { 'l', 'a' };
static const symbol s_0_20[4] = { 'm', 'e', 'n', 'a' };
static const symbol s_0_21[4] = { 'p', 'e', 'n', 'a' };
static const symbol s_0_22[4] = { 'k', 'i', 'n', 'a' };
static const symbol s_0_23[5] = { 'e', 'z', 'i', 'n', 'a' };
static const symbol s_0_24[6] = { 't', 'e', 'z', 'i', 'n', 'a' };
static const symbol s_0_25[4] = { 'k', 'u', 'n', 'a' };
static const symbol s_0_26[4] = { 't', 'u', 'n', 'a' };
static const symbol s_0_27[6] = { 'k', 'i', 'z', 'u', 'n', 'a' };
static const symbol s_0_28[3] = { 'e', 'r', 'a' };
static const symbol s_0_29[4] = { 'b', 'e', 'r', 'a' };
static const symbol s_0_30[7] = { 'a', 'r', 'a', 'b', 'e', 'r', 'a' };
static const symbol s_0_31[4] = { 'k', 'e', 'r', 'a' };
static const symbol s_0_32[4] = { 'p', 'e', 'r', 'a' };
static const symbol s_0_33[4] = { 'o', 'r', 'r', 'a' };
static const symbol s_0_34[5] = { 'k', 'o', 'r', 'r', 'a' };
static const symbol s_0_35[4] = { 'd', 'u', 'r', 'a' };
static const symbol s_0_36[4] = { 'g', 'u', 'r', 'a' };
static const symbol s_0_37[4] = { 'k', 'u', 'r', 'a' };
static const symbol s_0_38[4] = { 't', 'u', 'r', 'a' };
static const symbol s_0_39[3] = { 'e', 't', 'a' };
static const symbol s_0_40[4] = { 'k', 'e', 't', 'a' };
static const symbol s_0_41[6] = { 'g', 'a', 'i', 'l', 'u', 'a' };
static const symbol s_0_42[3] = { 'e', 'z', 'a' };
static const symbol s_0_43[6] = { 'e', 'r', 'r', 'e', 'z', 'a' };
static const symbol s_0_44[3] = { 't', 'z', 'a' };
static const symbol s_0_45[6] = { 'g', 'a', 'i', 't', 'z', 'a' };
static const symbol s_0_46[6] = { 'k', 'a', 'i', 't', 'z', 'a' };
static const symbol s_0_47[6] = { 'k', 'u', 'n', 't', 'z', 'a' };
static const symbol s_0_48[3] = { 'i', 'd', 'e' };
static const symbol s_0_49[4] = { 'b', 'i', 'd', 'e' };
static const symbol s_0_50[4] = { 'k', 'i', 'd', 'e' };
static const symbol s_0_51[4] = { 'p', 'i', 'd', 'e' };
static const symbol s_0_52[5] = { 'k', 'u', 'n', 'd', 'e' };
static const symbol s_0_53[5] = { 't', 'z', 'a', 'k', 'e' };
static const symbol s_0_54[5] = { 't', 'z', 'e', 'k', 'e' };
static const symbol s_0_55[2] = { 'l', 'e' };
static const symbol s_0_56[4] = { 'g', 'a', 'l', 'e' };
static const symbol s_0_57[5] = { 't', 'a', 'i', 'l', 'e' };
static const symbol s_0_58[6] = { 't', 'z', 'a', 'i', 'l', 'e' };
static const symbol s_0_59[4] = { 'g', 'u', 'n', 'e' };
static const symbol s_0_60[4] = { 'k', 'u', 'n', 'e' };
static const symbol s_0_61[3] = { 't', 'z', 'e' };
static const symbol s_0_62[4] = { 'a', 't', 'z', 'e' };
static const symbol s_0_63[3] = { 'g', 'a', 'i' };
static const symbol s_0_64[4] = { 'a', 'l', 'd', 'i' };
static const symbol s_0_65[5] = { 't', 'a', 'l', 'd', 'i' };
static const symbol s_0_66[2] = { 'k', 'i' };
static const symbol s_0_67[3] = { 'a', 'r', 'i' };
static const symbol s_0_68[4] = { 'k', 'a', 'r', 'i' };
static const symbol s_0_69[4] = { 'l', 'a', 'r', 'i' };
static const symbol s_0_70[4] = { 't', 'a', 'r', 'i' };
static const symbol s_0_71[5] = { 'e', 't', 'a', 'r', 'i' };
static const symbol s_0_72[5] = { 'g', 'a', 'r', 'r', 'i' };
static const symbol s_0_73[5] = { 'k', 'a', 'r', 'r', 'i' };
static const symbol s_0_74[5] = { 'a', 'r', 'a', 'z', 'i' };
static const symbol s_0_75[6] = { 't', 'a', 'r', 'a', 'z', 'i' };
static const symbol s_0_76[2] = { 'a', 'n' };
static const symbol s_0_77[3] = { 'e', 'a', 'n' };
static const symbol s_0_78[4] = { 'r', 'e', 'a', 'n' };
static const symbol s_0_79[3] = { 'k', 'a', 'n' };
static const symbol s_0_80[4] = { 'e', 't', 'a', 'n' };
static const symbol s_0_81[7] = { 'a', 't', 's', 'e', 'd', 'e', 'n' };
static const symbol s_0_82[3] = { 'm', 'e', 'n' };
static const symbol s_0_83[3] = { 'p', 'e', 'n' };
static const symbol s_0_84[3] = { 'k', 'i', 'n' };
static const symbol s_0_85[5] = { 'r', 'e', 'k', 'i', 'n' };
static const symbol s_0_86[4] = { 'e', 'z', 'i', 'n' };
static const symbol s_0_87[5] = { 't', 'e', 'z', 'i', 'n' };
static const symbol s_0_88[3] = { 't', 'u', 'n' };
static const symbol s_0_89[5] = { 'k', 'i', 'z', 'u', 'n' };
static const symbol s_0_90[2] = { 'g', 'o' };
static const symbol s_0_91[3] = { 'a', 'g', 'o' };
static const symbol s_0_92[3] = { 't', 'i', 'o' };
static const symbol s_0_93[4] = { 'd', 'a', 'k', 'o' };
static const symbol s_0_94[2] = { 'o', 'r' };
static const symbol s_0_95[3] = { 'k', 'o', 'r' };
static const symbol s_0_96[4] = { 't', 'z', 'a', 't' };
static const symbol s_0_97[2] = { 'd', 'u' };
static const symbol s_0_98[5] = { 'g', 'a', 'i', 'l', 'u' };
static const symbol s_0_99[2] = { 't', 'u' };
static const symbol s_0_100[3] = { 'a', 't', 'u' };
static const symbol s_0_101[6] = { 'a', 'l', 'd', 'a', 't', 'u' };
static const symbol s_0_102[4] = { 't', 'a', 't', 'u' };
static const symbol s_0_103[6] = { 'b', 'a', 'd', 'i', 't', 'u' };
static const symbol s_0_104[2] = { 'e', 'z' };
static const symbol s_0_105[5] = { 'e', 'r', 'r', 'e', 'z' };
static const symbol s_0_106[4] = { 't', 'z', 'e', 'z' };
static const symbol s_0_107[5] = { 'g', 'a', 'i', 't', 'z' };
static const symbol s_0_108[5] = { 'k', 'a', 'i', 't', 'z' };
static const struct among a_0[109] = {
{ 4, s_0_0, 0, 1, 0},
{ 5, s_0_1, -1, 1, 0},
{ 5, s_0_2, -2, 1, 0},
{ 5, s_0_3, -3, 1, 0},
{ 6, s_0_4, 0, 1, 0},
{ 5, s_0_5, 0, 1, 0},
{ 6, s_0_6, 0, 1, 0},
{ 7, s_0_7, 0, 1, 0},
{ 5, s_0_8, 0, 1, 0},
{ 5, s_0_9, 0, 1, 0},
{ 5, s_0_10, 0, 1, 0},
{ 4, s_0_11, 0, 1, 0},
{ 5, s_0_12, 0, 1, 0},
{ 6, s_0_13, -1, 1, 0},
{ 5, s_0_14, 0, 1, 0},
{ 6, s_0_15, 0, 2, 0},
{ 6, s_0_16, 0, 1, 0},
{ 2, s_0_17, 0, 1, 0},
{ 5, s_0_18, -1, 1, 0},
{ 2, s_0_19, 0, 1, 0},
{ 4, s_0_20, 0, 1, 0},
{ 4, s_0_21, 0, 1, 0},
{ 4, s_0_22, 0, 1, 0},
{ 5, s_0_23, 0, 1, 0},
{ 6, s_0_24, -1, 1, 0},
{ 4, s_0_25, 0, 1, 0},
{ 4, s_0_26, 0, 1, 0},
{ 6, s_0_27, 0, 1, 0},
{ 3, s_0_28, 0, 1, 0},
{ 4, s_0_29, -1, 1, 0},
{ 7, s_0_30, -1, -1, 0},
{ 4, s_0_31, -3, 1, 0},
{ 4, s_0_32, -4, 1, 0},
{ 4, s_0_33, 0, 1, 0},
{ 5, s_0_34, -1, 1, 0},
{ 4, s_0_35, 0, 1, 0},
{ 4, s_0_36, 0, 1, 0},
{ 4, s_0_37, 0, 1, 0},
{ 4, s_0_38, 0, 1, 0},
{ 3, s_0_39, 0, 1, 0},
{ 4, s_0_40, -1, 1, 0},
{ 6, s_0_41, 0, 1, 0},
{ 3, s_0_42, 0, 1, 0},
{ 6, s_0_43, -1, 1, 0},
{ 3, s_0_44, 0, 2, 0},
{ 6, s_0_45, -1, 1, 0},
{ 6, s_0_46, -2, 1, 0},
{ 6, s_0_47, -3, 1, 0},
{ 3, s_0_48, 0, 1, 0},
{ 4, s_0_49, -1, 1, 0},
{ 4, s_0_50, -2, 1, 0},
{ 4, s_0_51, -3, 1, 0},
{ 5, s_0_52, 0, 1, 0},
{ 5, s_0_53, 0, 1, 0},
{ 5, s_0_54, 0, 1, 0},
{ 2, s_0_55, 0, 1, 0},
{ 4, s_0_56, -1, 1, 0},
{ 5, s_0_57, -2, 1, 0},
{ 6, s_0_58, -3, 1, 0},
{ 4, s_0_59, 0, 1, 0},
{ 4, s_0_60, 0, 1, 0},
{ 3, s_0_61, 0, 1, 0},
{ 4, s_0_62, -1, 1, 0},
{ 3, s_0_63, 0, 1, 0},
{ 4, s_0_64, 0, 1, 0},
{ 5, s_0_65, -1, 1, 0},
{ 2, s_0_66, 0, 1, 0},
{ 3, s_0_67, 0, 1, 0},
{ 4, s_0_68, -1, 1, 0},
{ 4, s_0_69, -2, 1, 0},
{ 4, s_0_70, -3, 1, 0},
{ 5, s_0_71, -1, 1, 0},
{ 5, s_0_72, 0, 2, 0},
{ 5, s_0_73, 0, 1, 0},
{ 5, s_0_74, 0, 1, 0},
{ 6, s_0_75, -1, 1, 0},
{ 2, s_0_76, 0, 1, 0},
{ 3, s_0_77, -1, 1, 0},
{ 4, s_0_78, -1, 1, 0},
{ 3, s_0_79, -3, 1, 0},
{ 4, s_0_80, -4, 1, 0},
{ 7, s_0_81, 0, -1, 0},
{ 3, s_0_82, 0, 1, 0},
{ 3, s_0_83, 0, 1, 0},
{ 3, s_0_84, 0, 1, 0},
{ 5, s_0_85, -1, 1, 0},
{ 4, s_0_86, 0, 1, 0},
{ 5, s_0_87, -1, 1, 0},
{ 3, s_0_88, 0, 1, 0},
{ 5, s_0_89, 0, 1, 0},
{ 2, s_0_90, 0, 1, 0},
{ 3, s_0_91, -1, 1, 0},
{ 3, s_0_92, 0, 1, 0},
{ 4, s_0_93, 0, 1, 0},
{ 2, s_0_94, 0, 1, 0},
{ 3, s_0_95, -1, 1, 0},
{ 4, s_0_96, 0, 1, 0},
{ 2, s_0_97, 0, 1, 0},
{ 5, s_0_98, 0, 1, 0},
{ 2, s_0_99, 0, 1, 0},
{ 3, s_0_100, -1, 1, 0},
{ 6, s_0_101, -1, 1, 0},
{ 4, s_0_102, -2, 1, 0},
{ 6, s_0_103, -4, -1, 0},
{ 2, s_0_104, 0, 1, 0},
{ 5, s_0_105, -1, 1, 0},
{ 4, s_0_106, -2, 1, 0},
{ 5, s_0_107, 0, 1, 0},
{ 5, s_0_108, 0, 1, 0}
};

static const symbol s_1_0[3] = { 'a', 'd', 'a' };
static const symbol s_1_1[4] = { 'k', 'a', 'd', 'a' };
static const symbol s_1_2[4] = { 'a', 'n', 'd', 'a' };
static const symbol s_1_3[5] = { 'd', 'e', 'n', 'd', 'a' };
static const symbol s_1_4[5] = { 'g', 'a', 'b', 'e', 'a' };
static const symbol s_1_5[5] = { 'k', 'a', 'b', 'e', 'a' };
static const symbol s_1_6[5] = { 'a', 'l', 'd', 'e', 'a' };
static const symbol s_1_7[6] = { 'k', 'a', 'l', 'd', 'e', 'a' };
static const symbol s_1_8[6] = { 't', 'a', 'l', 'd', 'e', 'a' };
static const symbol s_1_9[5] = { 'o', 'r', 'd', 'e', 'a' };
static const symbol s_1_10[5] = { 'z', 'a', 'l', 'e', 'a' };
static const symbol s_1_11[6] = { 't', 'z', 'a', 'l', 'e', 'a' };
static const symbol s_1_12[5] = { 'g', 'i', 'l', 'e', 'a' };
static const symbol s_1_13[4] = { 'e', 'm', 'e', 'a' };
static const symbol s_1_14[5] = { 'k', 'u', 'm', 'e', 'a' };
static const symbol s_1_15[3] = { 'n', 'e', 'a' };
static const symbol s_1_16[4] = { 'e', 'n', 'e', 'a' };
static const symbol s_1_17[6] = { 'z', 'i', 'o', 'n', 'e', 'a' };
static const symbol s_1_18[4] = { 'u', 'n', 'e', 'a' };
static const symbol s_1_19[5] = { 'g', 'u', 'n', 'e', 'a' };
static const symbol s_1_20[3] = { 'p', 'e', 'a' };
static const symbol s_1_21[6] = { 'a', 'u', 'r', 'r', 'e', 'a' };
static const symbol s_1_22[3] = { 't', 'e', 'a' };
static const symbol s_1_23[5] = { 'k', 'o', 't', 'e', 'a' };
static const symbol s_1_24[5] = { 'a', 'r', 't', 'e', 'a' };
static const symbol s_1_25[5] = { 'o', 's', 't', 'e', 'a' };
static const symbol s_1_26[5] = { 'e', 't', 'x', 'e', 'a' };
static const symbol s_1_27[2] = { 'g', 'a' };
static const symbol s_1_28[4] = { 'a', 'n', 'g', 'a' };
static const symbol s_1_29[4] = { 'g', 'a', 'i', 'a' };
static const symbol s_1_30[5] = { 'a', 'l', 'd', 'i', 'a' };
static const symbol s_1_31[6] = { 't', 'a', 'l', 'd', 'i', 'a' };
static const symbol s_1_32[6] = { 'h', 'a', 'n', 'd', 'i', 'a' };
static const symbol s_1_33[6] = { 'm', 'e', 'n', 'd', 'i', 'a' };
static const symbol s_1_34[4] = { 'g', 'e', 'i', 'a' };
static const symbol s_1_35[4] = { 'e', 'g', 'i', 'a' };
static const symbol s_1_36[5] = { 'd', 'e', 'g', 'i', 'a' };
static const symbol s_1_37[5] = { 't', 'e', 'g', 'i', 'a' };
static const symbol s_1_38[5] = { 'n', 'a', 'h', 'i', 'a' };
static const symbol s_1_39[4] = { 'o', 'h', 'i', 'a' };
static const symbol s_1_40[3] = { 'k', 'i', 'a' };
static const symbol s_1_41[5] = { 't', 'o', 'k', 'i', 'a' };
static const symbol s_1_42[3] = { 'o', 'i', 'a' };
static const symbol s_1_43[4] = { 'k', 'o', 'i', 'a' };
static const symbol s_1_44[4] = { 'a', 'r', 'i', 'a' };
static const symbol s_1_45[5] = { 'k', 'a', 'r', 'i', 'a' };
static const symbol s_1_46[5] = { 'l', 'a', 'r', 'i', 'a' };
static const symbol s_1_47[5] = { 't', 'a', 'r', 'i', 'a' };
static const symbol s_1_48[4] = { 'e', 'r', 'i', 'a' };
static const symbol s_1_49[5] = { 'k', 'e', 'r', 'i', 'a' };
static const symbol s_1_50[5] = { 't', 'e', 'r', 'i', 'a' };
static const symbol s_1_51[6] = { 'g', 'a', 'r', 'r', 'i', 'a' };
static const symbol s_1_52[6] = { 'l', 'a', 'r', 'r', 'i', 'a' };
static const symbol s_1_53[6] = { 'k', 'i', 'r', 'r', 'i', 'a' };
static const symbol s_1_54[5] = { 'd', 'u', 'r', 'i', 'a' };
static const symbol s_1_55[4] = { 'a', 's', 'i', 'a' };
static const symbol s_1_56[3] = { 't', 'i', 'a' };
static const symbol s_1_57[4] = { 'e', 'z', 'i', 'a' };
static const symbol s_1_58[5] = { 'b', 'i', 'z', 'i', 'a' };
static const symbol s_1_59[6] = { 'o', 'n', 't', 'z', 'i', 'a' };
static const symbol s_1_60[2] = { 'k', 'a' };
static const symbol s_1_61[4] = { 'j', 'o', 'k', 'a' };
static const symbol s_1_62[5] = { 'a', 'u', 'r', 'k', 'a' };
static const symbol s_1_63[3] = { 's', 'k', 'a' };
static const symbol s_1_64[3] = { 'x', 'k', 'a' };
static const symbol s_1_65[3] = { 'z', 'k', 'a' };
static const symbol s_1_66[6] = { 'g', 'i', 'b', 'e', 'l', 'a' };
static const symbol s_1_67[4] = { 'g', 'e', 'l', 'a' };
static const symbol s_1_68[5] = { 'k', 'a', 'i', 'l', 'a' };
static const symbol s_1_69[5] = { 's', 'k', 'i', 'l', 'a' };
static const symbol s_1_70[4] = { 't', 'i', 'l', 'a' };
static const symbol s_1_71[3] = { 'o', 'l', 'a' };
static const symbol s_1_72[2] = { 'n', 'a' };
static const symbol s_1_73[4] = { 'k', 'a', 'n', 'a' };
static const symbol s_1_74[3] = { 'e', 'n', 'a' };
static const symbol s_1_75[7] = { 'g', 'a', 'r', 'r', 'e', 'n', 'a' };
static const symbol s_1_76[7] = { 'g', 'e', 'r', 'r', 'e', 'n', 'a' };
static const symbol s_1_77[6] = { 'u', 'r', 'r', 'e', 'n', 'a' };
static const symbol s_1_78[5] = { 'z', 'a', 'i', 'n', 'a' };
static const symbol s_1_79[6] = { 't', 'z', 'a', 'i', 'n', 'a' };
static const symbol s_1_80[4] = { 'k', 'i', 'n', 'a' };
static const symbol s_1_81[4] = { 'm', 'i', 'n', 'a' };
static const symbol s_1_82[5] = { 'g', 'a', 'r', 'n', 'a' };
static const symbol s_1_83[3] = { 'u', 'n', 'a' };
static const symbol s_1_84[4] = { 'd', 'u', 'n', 'a' };
static const symbol s_1_85[5] = { 'a', 's', 'u', 'n', 'a' };
static const symbol s_1_86[6] = { 't', 'a', 's', 'u', 'n', 'a' };
static const symbol s_1_87[5] = { 'o', 'n', 'd', 'o', 'a' };
static const symbol s_1_88[6] = { 'k', 'o', 'n', 'd', 'o', 'a' };
static const symbol s_1_89[4] = { 'n', 'g', 'o', 'a' };
static const symbol s_1_90[4] = { 'z', 'i', 'o', 'a' };
static const symbol s_1_91[3] = { 'k', 'o', 'a' };
static const symbol s_1_92[5] = { 't', 'a', 'k', 'o', 'a' };
static const symbol s_1_93[4] = { 'z', 'k', 'o', 'a' };
static const symbol s_1_94[3] = { 'n', 'o', 'a' };
static const symbol s_1_95[5] = { 'z', 'i', 'n', 'o', 'a' };
static const symbol s_1_96[4] = { 'a', 'r', 'o', 'a' };
static const symbol s_1_97[5] = { 't', 'a', 'r', 'o', 'a' };
static const symbol s_1_98[5] = { 'z', 'a', 'r', 'o', 'a' };
static const symbol s_1_99[4] = { 'e', 'r', 'o', 'a' };
static const symbol s_1_100[4] = { 'o', 'r', 'o', 'a' };
static const symbol s_1_101[4] = { 'o', 's', 'o', 'a' };
static const symbol s_1_102[3] = { 't', 'o', 'a' };
static const symbol s_1_103[4] = { 't', 't', 'o', 'a' };
static const symbol s_1_104[4] = { 'z', 't', 'o', 'a' };
static const symbol s_1_105[4] = { 't', 'x', 'o', 'a' };
static const symbol s_1_106[4] = { 't', 'z', 'o', 'a' };
static const symbol s_1_107[3] = { 0xF1, 'o', 'a' };
static const symbol s_1_108[2] = { 'r', 'a' };
static const symbol s_1_109[3] = { 'a', 'r', 'a' };
static const symbol s_1_110[4] = { 'd', 'a', 'r', 'a' };
static const symbol s_1_111[5] = { 'l', 'i', 'a', 'r', 'a' };
static const symbol s_1_112[5] = { 't', 'i', 'a', 'r', 'a' };
static const symbol s_1_113[4] = { 't', 'a', 'r', 'a' };
static const symbol s_1_114[5] = { 'e', 't', 'a', 'r', 'a' };
static const symbol s_1_115[5] = { 't', 'z', 'a', 'r', 'a' };
static const symbol s_1_116[4] = { 'b', 'e', 'r', 'a' };
static const symbol s_1_117[4] = { 'k', 'e', 'r', 'a' };
static const symbol s_1_118[4] = { 'p', 'e', 'r', 'a' };
static const symbol s_1_119[3] = { 'o', 'r', 'a' };
static const symbol s_1_120[6] = { 't', 'z', 'a', 'r', 'r', 'a' };
static const symbol s_1_121[5] = { 'k', 'o', 'r', 'r', 'a' };
static const symbol s_1_122[3] = { 't', 'r', 'a' };
static const symbol s_1_123[2] = { 's', 'a' };
static const symbol s_1_124[3] = { 'o', 's', 'a' };
static const symbol s_1_125[2] = { 't', 'a' };
static const symbol s_1_126[3] = { 'e', 't', 'a' };
static const symbol s_1_127[4] = { 'k', 'e', 't', 'a' };
static const symbol s_1_128[3] = { 's', 't', 'a' };
static const symbol s_1_129[3] = { 'd', 'u', 'a' };
static const symbol s_1_130[6] = { 'm', 'e', 'n', 'd', 'u', 'a' };
static const symbol s_1_131[5] = { 'o', 'r', 'd', 'u', 'a' };
static const symbol s_1_132[5] = { 'l', 'e', 'k', 'u', 'a' };
static const symbol s_1_133[5] = { 'b', 'u', 'r', 'u', 'a' };
static const symbol s_1_134[5] = { 'd', 'u', 'r', 'u', 'a' };
static const symbol s_1_135[4] = { 't', 's', 'u', 'a' };
static const symbol s_1_136[3] = { 't', 'u', 'a' };
static const symbol s_1_137[6] = { 'm', 'e', 'n', 't', 'u', 'a' };
static const symbol s_1_138[5] = { 'e', 's', 't', 'u', 'a' };
static const symbol s_1_139[4] = { 't', 'x', 'u', 'a' };
static const symbol s_1_140[3] = { 'z', 'u', 'a' };
static const symbol s_1_141[4] = { 't', 'z', 'u', 'a' };
static const symbol s_1_142[2] = { 'z', 'a' };
static const symbol s_1_143[3] = { 'e', 'z', 'a' };
static const symbol s_1_144[5] = { 'e', 'r', 'o', 'z', 'a' };
static const symbol s_1_145[3] = { 't', 'z', 'a' };
static const symbol s_1_146[6] = { 'k', 'o', 'i', 't', 'z', 'a' };
static const symbol s_1_147[5] = { 'a', 'n', 't', 'z', 'a' };
static const symbol s_1_148[6] = { 'g', 'i', 'n', 't', 'z', 'a' };
static const symbol s_1_149[6] = { 'k', 'i', 'n', 't', 'z', 'a' };
static const symbol s_1_150[6] = { 'k', 'u', 'n', 't', 'z', 'a' };
static const symbol s_1_151[4] = { 'g', 'a', 'b', 'e' };
static const symbol s_1_152[4] = { 'k', 'a', 'b', 'e' };
static const symbol s_1_153[4] = { 'k', 'i', 'd', 'e' };
static const symbol s_1_154[4] = { 'a', 'l', 'd', 'e' };
static const symbol s_1_155[5] = { 'k', 'a', 'l', 'd', 'e' };
static const symbol s_1_156[5] = { 't', 'a', 'l', 'd', 'e' };
static const symbol s_1_157[4] = { 'o', 'r', 'd', 'e' };
static const symbol s_1_158[2] = { 'g', 'e' };
static const symbol s_1_159[4] = { 'z', 'a', 'l', 'e' };
static const symbol s_1_160[5] = { 't', 'z', 'a', 'l', 'e' };
static const symbol s_1_161[4] = { 'g', 'i', 'l', 'e' };
static const symbol s_1_162[3] = { 'e', 'm', 'e' };
static const symbol s_1_163[4] = { 'k', 'u', 'm', 'e' };
static const symbol s_1_164[2] = { 'n', 'e' };
static const symbol s_1_165[5] = { 'z', 'i', 'o', 'n', 'e' };
static const symbol s_1_166[3] = { 'u', 'n', 'e' };
static const symbol s_1_167[4] = { 'g', 'u', 'n', 'e' };
static const symbol s_1_168[2] = { 'p', 'e' };
static const symbol s_1_169[5] = { 'a', 'u', 'r', 'r', 'e' };
static const symbol s_1_170[2] = { 't', 'e' };
static const symbol s_1_171[4] = { 'k', 'o', 't', 'e' };
static const symbol s_1_172[4] = { 'a', 'r', 't', 'e' };
static const symbol s_1_173[4] = { 'o', 's', 't', 'e' };
static const symbol s_1_174[4] = { 'e', 't', 'x', 'e' };
static const symbol s_1_175[3] = { 'g', 'a', 'i' };
static const symbol s_1_176[2] = { 'd', 'i' };
static const symbol s_1_177[4] = { 'a', 'l', 'd', 'i' };
static const symbol s_1_178[5] = { 't', 'a', 'l', 'd', 'i' };
static const symbol s_1_179[5] = { 'g', 'e', 'l', 'd', 'i' };
static const symbol s_1_180[5] = { 'h', 'a', 'n', 'd', 'i' };
static const symbol s_1_181[5] = { 'm', 'e', 'n', 'd', 'i' };
static const symbol s_1_182[3] = { 'g', 'e', 'i' };
static const symbol s_1_183[3] = { 'e', 'g', 'i' };
static const symbol s_1_184[4] = { 'd', 'e', 'g', 'i' };
static const symbol s_1_185[4] = { 't', 'e', 'g', 'i' };
static const symbol s_1_186[4] = { 'n', 'a', 'h', 'i' };
static const symbol s_1_187[3] = { 'o', 'h', 'i' };
static const symbol s_1_188[2] = { 'k', 'i' };
static const symbol s_1_189[4] = { 't', 'o', 'k', 'i' };
static const symbol s_1_190[2] = { 'o', 'i' };
static const symbol s_1_191[3] = { 'g', 'o', 'i' };
static const symbol s_1_192[3] = { 'k', 'o', 'i' };
static const symbol s_1_193[3] = { 'a', 'r', 'i' };
static const symbol s_1_194[4] = { 'k', 'a', 'r', 'i' };
static const symbol s_1_195[4] = { 'l', 'a', 'r', 'i' };
static const symbol s_1_196[4] = { 't', 'a', 'r', 'i' };
static const symbol s_1_197[5] = { 'g', 'a', 'r', 'r', 'i' };
static const symbol s_1_198[5] = { 'l', 'a', 'r', 'r', 'i' };
static const symbol s_1_199[5] = { 'k', 'i', 'r', 'r', 'i' };
static const symbol s_1_200[4] = { 'd', 'u', 'r', 'i' };
static const symbol s_1_201[3] = { 'a', 's', 'i' };
static const symbol s_1_202[2] = { 't', 'i' };
static const symbol s_1_203[5] = { 'o', 'n', 't', 'z', 'i' };
static const symbol s_1_204[2] = { 0xF1, 'i' };
static const symbol s_1_205[2] = { 'a', 'k' };
static const symbol s_1_206[2] = { 'e', 'k' };
static const symbol s_1_207[5] = { 't', 'a', 'r', 'i', 'k' };
static const symbol s_1_208[5] = { 'g', 'i', 'b', 'e', 'l' };
static const symbol s_1_209[3] = { 'a', 'i', 'l' };
static const symbol s_1_210[4] = { 'k', 'a', 'i', 'l' };
static const symbol s_1_211[3] = { 'k', 'a', 'n' };
static const symbol s_1_212[3] = { 't', 'a', 'n' };
static const symbol s_1_213[4] = { 'e', 't', 'a', 'n' };
static const symbol s_1_214[2] = { 'e', 'n' };
static const symbol s_1_215[3] = { 'r', 'e', 'n' };
static const symbol s_1_216[6] = { 'g', 'a', 'r', 'r', 'e', 'n' };
static const symbol s_1_217[6] = { 'g', 'e', 'r', 'r', 'e', 'n' };
static const symbol s_1_218[5] = { 'u', 'r', 'r', 'e', 'n' };
static const symbol s_1_219[3] = { 't', 'e', 'n' };
static const symbol s_1_220[4] = { 't', 'z', 'e', 'n' };
static const symbol s_1_221[4] = { 'z', 'a', 'i', 'n' };
static const symbol s_1_222[5] = { 't', 'z', 'a', 'i', 'n' };
static const symbol s_1_223[3] = { 'k', 'i', 'n' };
static const symbol s_1_224[3] = { 'm', 'i', 'n' };
static const symbol s_1_225[3] = { 'd', 'u', 'n' };
static const symbol s_1_226[4] = { 'a', 's', 'u', 'n' };
static const symbol s_1_227[5] = { 't', 'a', 's', 'u', 'n' };
static const symbol s_1_228[5] = { 'a', 'i', 'z', 'u', 'n' };
static const symbol s_1_229[4] = { 'o', 'n', 'd', 'o' };
static const symbol s_1_230[5] = { 'k', 'o', 'n', 'd', 'o' };
static const symbol s_1_231[2] = { 'g', 'o' };
static const symbol s_1_232[3] = { 'n', 'g', 'o' };
static const symbol s_1_233[3] = { 'z', 'i', 'o' };
static const symbol s_1_234[2] = { 'k', 'o' };
static const symbol s_1_235[5] = { 't', 'r', 'a', 'k', 'o' };
static const symbol s_1_236[4] = { 't', 'a', 'k', 'o' };
static const symbol s_1_237[5] = { 'e', 't', 'a', 'k', 'o' };
static const symbol s_1_238[3] = { 'e', 'k', 'o' };
static const symbol s_1_239[6] = { 't', 'a', 'r', 'i', 'k', 'o' };
static const symbol s_1_240[3] = { 's', 'k', 'o' };
static const symbol s_1_241[4] = { 't', 'u', 'k', 'o' };
static const symbol s_1_242[8] = { 'm', 'i', 'n', 'u', 't', 'u', 'k', 'o' };
static const symbol s_1_243[3] = { 'z', 'k', 'o' };
static const symbol s_1_244[2] = { 'n', 'o' };
static const symbol s_1_245[4] = { 'z', 'i', 'n', 'o' };
static const symbol s_1_246[2] = { 'r', 'o' };
static const symbol s_1_247[3] = { 'a', 'r', 'o' };
static const symbol s_1_248[5] = { 'i', 'g', 'a', 'r', 'o' };
static const symbol s_1_249[4] = { 't', 'a', 'r', 'o' };
static const symbol s_1_250[4] = { 'z', 'a', 'r', 'o' };
static const symbol s_1_251[3] = { 'e', 'r', 'o' };
static const symbol s_1_252[4] = { 'g', 'i', 'r', 'o' };
static const symbol s_1_253[3] = { 'o', 'r', 'o' };
static const symbol s_1_254[3] = { 'o', 's', 'o' };
static const symbol s_1_255[2] = { 't', 'o' };
static const symbol s_1_256[3] = { 't', 't', 'o' };
static const symbol s_1_257[3] = { 'z', 't', 'o' };
static const symbol s_1_258[3] = { 't', 'x', 'o' };
static const symbol s_1_259[3] = { 't', 'z', 'o' };
static const symbol s_1_260[6] = { 'g', 'i', 'n', 't', 'z', 'o' };
static const symbol s_1_261[2] = { 0xF1, 'o' };
static const symbol s_1_262[2] = { 'z', 'p' };
static const symbol s_1_263[2] = { 'a', 'r' };
static const symbol s_1_264[3] = { 'd', 'a', 'r' };
static const symbol s_1_265[5] = { 'b', 'e', 'h', 'a', 'r' };
static const symbol s_1_266[5] = { 'z', 'e', 'h', 'a', 'r' };
static const symbol s_1_267[4] = { 'l', 'i', 'a', 'r' };
static const symbol s_1_268[4] = { 't', 'i', 'a', 'r' };
static const symbol s_1_269[3] = { 't', 'a', 'r' };
static const symbol s_1_270[4] = { 't', 'z', 'a', 'r' };
static const symbol s_1_271[2] = { 'o', 'r' };
static const symbol s_1_272[3] = { 'k', 'o', 'r' };
static const symbol s_1_273[2] = { 'o', 's' };
static const symbol s_1_274[3] = { 'k', 'e', 't' };
static const symbol s_1_275[2] = { 'd', 'u' };
static const symbol s_1_276[5] = { 'm', 'e', 'n', 'd', 'u' };
static const symbol s_1_277[4] = { 'o', 'r', 'd', 'u' };
static const symbol s_1_278[4] = { 'l', 'e', 'k', 'u' };
static const symbol s_1_279[4] = { 'b', 'u', 'r', 'u' };
static const symbol s_1_280[4] = { 'd', 'u', 'r', 'u' };
static const symbol s_1_281[3] = { 't', 's', 'u' };
static const symbol s_1_282[2] = { 't', 'u' };
static const symbol s_1_283[4] = { 't', 'a', 't', 'u' };
static const symbol s_1_284[5] = { 'm', 'e', 'n', 't', 'u' };
static const symbol s_1_285[4] = { 'e', 's', 't', 'u' };
static const symbol s_1_286[3] = { 't', 'x', 'u' };
static const symbol s_1_287[2] = { 'z', 'u' };
static const symbol s_1_288[3] = { 't', 'z', 'u' };
static const symbol s_1_289[6] = { 'g', 'i', 'n', 't', 'z', 'u' };
static const symbol s_1_290[1] = { 'z' };
static const symbol s_1_291[2] = { 'e', 'z' };
static const symbol s_1_292[4] = { 'e', 'r', 'o', 'z' };
static const symbol s_1_293[2] = { 't', 'z' };
static const symbol s_1_294[5] = { 'k', 'o', 'i', 't', 'z' };
static const struct among a_1[295] = {
{ 3, s_1_0, 0, 1, 0},
{ 4, s_1_1, -1, 1, 0},
{ 4, s_1_2, 0, 1, 0},
{ 5, s_1_3, 0, 1, 0},
{ 5, s_1_4, 0, 1, 0},
{ 5, s_1_5, 0, 1, 0},
{ 5, s_1_6, 0, 1, 0},
{ 6, s_1_7, -1, 1, 0},
{ 6, s_1_8, -2, 1, 0},
{ 5, s_1_9, 0, 1, 0},
{ 5, s_1_10, 0, 1, 0},
{ 6, s_1_11, -1, 1, 0},
{ 5, s_1_12, 0, 1, 0},
{ 4, s_1_13, 0, 1, 0},
{ 5, s_1_14, 0, 1, 0},
{ 3, s_1_15, 0, 1, 0},
{ 4, s_1_16, -1, 1, 0},
{ 6, s_1_17, -2, 1, 0},
{ 4, s_1_18, -3, 1, 0},
{ 5, s_1_19, -1, 1, 0},
{ 3, s_1_20, 0, 1, 0},
{ 6, s_1_21, 0, 1, 0},
{ 3, s_1_22, 0, 1, 0},
{ 5, s_1_23, -1, 1, 0},
{ 5, s_1_24, -2, 1, 0},
{ 5, s_1_25, -3, 1, 0},
{ 5, s_1_26, 0, 1, 0},
{ 2, s_1_27, 0, 1, 0},
{ 4, s_1_28, -1, 1, 0},
{ 4, s_1_29, 0, 1, 0},
{ 5, s_1_30, 0, 1, 0},
{ 6, s_1_31, -1, 1, 0},
{ 6, s_1_32, 0, 1, 0},
{ 6, s_1_33, 0, 1, 0},
{ 4, s_1_34, 0, 1, 0},
{ 4, s_1_35, 0, 1, 0},
{ 5, s_1_36, -1, 1, 0},
{ 5, s_1_37, -2, 1, 0},
{ 5, s_1_38, 0, 1, 0},
{ 4, s_1_39, 0, 1, 0},
{ 3, s_1_40, 0, 1, 0},
{ 5, s_1_41, -1, 1, 0},
{ 3, s_1_42, 0, 1, 0},
{ 4, s_1_43, -1, 1, 0},
{ 4, s_1_44, 0, 1, 0},
{ 5, s_1_45, -1, 1, 0},
{ 5, s_1_46, -2, 1, 0},
{ 5, s_1_47, -3, 1, 0},
{ 4, s_1_48, 0, 1, 0},
{ 5, s_1_49, -1, 1, 0},
{ 5, s_1_50, -2, 1, 0},
{ 6, s_1_51, 0, 2, 0},
{ 6, s_1_52, 0, 1, 0},
{ 6, s_1_53, 0, 1, 0},
{ 5, s_1_54, 0, 1, 0},
{ 4, s_1_55, 0, 1, 0},
{ 3, s_1_56, 0, 1, 0},
{ 4, s_1_57, 0, 1, 0},
{ 5, s_1_58, 0, 1, 0},
{ 6, s_1_59, 0, 1, 0},
{ 2, s_1_60, 0, 1, 0},
{ 4, s_1_61, -1, 3, 0},
{ 5, s_1_62, -2, -1, 0},
{ 3, s_1_63, -3, 1, 0},
{ 3, s_1_64, -4, 1, 0},
{ 3, s_1_65, -5, 1, 0},
{ 6, s_1_66, 0, 1, 0},
{ 4, s_1_67, 0, 1, 0},
{ 5, s_1_68, 0, 1, 0},
{ 5, s_1_69, 0, 1, 0},
{ 4, s_1_70, 0, 1, 0},
{ 3, s_1_71, 0, 1, 0},
{ 2, s_1_72, 0, 1, 0},
{ 4, s_1_73, -1, 1, 0},
{ 3, s_1_74, -2, 1, 0},
{ 7, s_1_75, -1, 1, 0},
{ 7, s_1_76, -2, 1, 0},
{ 6, s_1_77, -3, 1, 0},
{ 5, s_1_78, -6, 1, 0},
{ 6, s_1_79, -1, 1, 0},
{ 4, s_1_80, -8, 1, 0},
{ 4, s_1_81, -9, 1, 0},
{ 5, s_1_82, -10, 1, 0},
{ 3, s_1_83, -11, 1, 0},
{ 4, s_1_84, -1, 1, 0},
{ 5, s_1_85, -2, 1, 0},
{ 6, s_1_86, -1, 1, 0},
{ 5, s_1_87, 0, 1, 0},
{ 6, s_1_88, -1, 1, 0},
{ 4, s_1_89, 0, 1, 0},
{ 4, s_1_90, 0, 1, 0},
{ 3, s_1_91, 0, 1, 0},
{ 5, s_1_92, -1, 1, 0},
{ 4, s_1_93, -2, 1, 0},
{ 3, s_1_94, 0, 1, 0},
{ 5, s_1_95, -1, 1, 0},
{ 4, s_1_96, 0, 1, 0},
{ 5, s_1_97, -1, 1, 0},
{ 5, s_1_98, -2, 1, 0},
{ 4, s_1_99, 0, 1, 0},
{ 4, s_1_100, 0, 1, 0},
{ 4, s_1_101, 0, 1, 0},
{ 3, s_1_102, 0, 1, 0},
{ 4, s_1_103, -1, 1, 0},
{ 4, s_1_104, -2, 1, 0},
{ 4, s_1_105, 0, 1, 0},
{ 4, s_1_106, 0, 1, 0},
{ 3, s_1_107, 0, 1, 0},
{ 2, s_1_108, 0, 1, 0},
{ 3, s_1_109, -1, 1, 0},
{ 4, s_1_110, -1, 1, 0},
{ 5, s_1_111, -2, 1, 0},
{ 5, s_1_112, -3, 1, 0},
{ 4, s_1_113, -4, 1, 0},
{ 5, s_1_114, -1, 1, 0},
{ 5, s_1_115, -6, 1, 0},
{ 4, s_1_116, -8, 1, 0},
{ 4, s_1_117, -9, 1, 0},
{ 4, s_1_118, -10, 1, 0},
{ 3, s_1_119, -11, 2, 0},
{ 6, s_1_120, -12, 1, 0},
{ 5, s_1_121, -13, 1, 0},
{ 3, s_1_122, -14, 1, 0},
{ 2, s_1_123, 0, 1, 0},
{ 3, s_1_124, -1, 1, 0},
{ 2, s_1_125, 0, 1, 0},
{ 3, s_1_126, -1, 1, 0},
{ 4, s_1_127, -1, 1, 0},
{ 3, s_1_128, -3, 1, 0},
{ 3, s_1_129, 0, 1, 0},
{ 6, s_1_130, -1, 1, 0},
{ 5, s_1_131, -2, 1, 0},
{ 5, s_1_132, 0, 1, 0},
{ 5, s_1_133, 0, 1, 0},
{ 5, s_1_134, 0, 1, 0},
{ 4, s_1_135, 0, 1, 0},
{ 3, s_1_136, 0, 1, 0},
{ 6, s_1_137, -1, 1, 0},
{ 5, s_1_138, -2, 1, 0},
{ 4, s_1_139, 0, 1, 0},
{ 3, s_1_140, 0, 1, 0},
{ 4, s_1_141, -1, 1, 0},
{ 2, s_1_142, 0, 1, 0},
{ 3, s_1_143, -1, 1, 0},
{ 5, s_1_144, -2, 1, 0},
{ 3, s_1_145, -3, 2, 0},
{ 6, s_1_146, -1, 1, 0},
{ 5, s_1_147, -2, 1, 0},
{ 6, s_1_148, -3, 1, 0},
{ 6, s_1_149, -4, 1, 0},
{ 6, s_1_150, -5, 1, 0},
{ 4, s_1_151, 0, 1, 0},
{ 4, s_1_152, 0, 1, 0},
{ 4, s_1_153, 0, 1, 0},
{ 4, s_1_154, 0, 1, 0},
{ 5, s_1_155, -1, 1, 0},
{ 5, s_1_156, -2, 1, 0},
{ 4, s_1_157, 0, 1, 0},
{ 2, s_1_158, 0, 1, 0},
{ 4, s_1_159, 0, 1, 0},
{ 5, s_1_160, -1, 1, 0},
{ 4, s_1_161, 0, 1, 0},
{ 3, s_1_162, 0, 1, 0},
{ 4, s_1_163, 0, 1, 0},
{ 2, s_1_164, 0, 1, 0},
{ 5, s_1_165, -1, 1, 0},
{ 3, s_1_166, -2, 1, 0},
{ 4, s_1_167, -1, 1, 0},
{ 2, s_1_168, 0, 1, 0},
{ 5, s_1_169, 0, 1, 0},
{ 2, s_1_170, 0, 1, 0},
{ 4, s_1_171, -1, 1, 0},
{ 4, s_1_172, -2, 1, 0},
{ 4, s_1_173, -3, 1, 0},
{ 4, s_1_174, 0, 1, 0},
{ 3, s_1_175, 0, 1, 0},
{ 2, s_1_176, 0, 1, 0},
{ 4, s_1_177, -1, 1, 0},
{ 5, s_1_178, -1, 1, 0},
{ 5, s_1_179, -3, -1, 0},
{ 5, s_1_180, -4, 1, 0},
{ 5, s_1_181, -5, 1, 0},
{ 3, s_1_182, 0, 1, 0},
{ 3, s_1_183, 0, 1, 0},
{ 4, s_1_184, -1, 1, 0},
{ 4, s_1_185, -2, 1, 0},
{ 4, s_1_186, 0, 1, 0},
{ 3, s_1_187, 0, 1, 0},
{ 2, s_1_188, 0, 1, 0},
{ 4, s_1_189, -1, 1, 0},
{ 2, s_1_190, 0, 1, 0},
{ 3, s_1_191, -1, 1, 0},
{ 3, s_1_192, -2, 1, 0},
{ 3, s_1_193, 0, 1, 0},
{ 4, s_1_194, -1, 1, 0},
{ 4, s_1_195, -2, 1, 0},
{ 4, s_1_196, -3, 1, 0},
{ 5, s_1_197, 0, 2, 0},
{ 5, s_1_198, 0, 1, 0},
{ 5, s_1_199, 0, 1, 0},
{ 4, s_1_200, 0, 1, 0},
{ 3, s_1_201, 0, 1, 0},
{ 2, s_1_202, 0, 1, 0},
{ 5, s_1_203, 0, 1, 0},
{ 2, s_1_204, 0, 1, 0},
{ 2, s_1_205, 0, 1, 0},
{ 2, s_1_206, 0, 1, 0},
{ 5, s_1_207, 0, 1, 0},
{ 5, s_1_208, 0, 1, 0},
{ 3, s_1_209, 0, 1, 0},
{ 4, s_1_210, -1, 1, 0},
{ 3, s_1_211, 0, 1, 0},
{ 3, s_1_212, 0, 1, 0},
{ 4, s_1_213, -1, 1, 0},
{ 2, s_1_214, 0, 4, 0},
{ 3, s_1_215, -1, 2, 0},
{ 6, s_1_216, -1, 1, 0},
{ 6, s_1_217, -2, 1, 0},
{ 5, s_1_218, -3, 1, 0},
{ 3, s_1_219, -5, 4, 0},
{ 4, s_1_220, -6, 4, 0},
{ 4, s_1_221, 0, 1, 0},
{ 5, s_1_222, -1, 1, 0},
{ 3, s_1_223, 0, 1, 0},
{ 3, s_1_224, 0, 1, 0},
{ 3, s_1_225, 0, 1, 0},
{ 4, s_1_226, 0, 1, 0},
{ 5, s_1_227, -1, 1, 0},
{ 5, s_1_228, 0, 1, 0},
{ 4, s_1_229, 0, 1, 0},
{ 5, s_1_230, -1, 1, 0},
{ 2, s_1_231, 0, 1, 0},
{ 3, s_1_232, -1, 1, 0},
{ 3, s_1_233, 0, 1, 0},
{ 2, s_1_234, 0, 1, 0},
{ 5, s_1_235, -1, 5, 0},
{ 4, s_1_236, -2, 1, 0},
{ 5, s_1_237, -1, 1, 0},
{ 3, s_1_238, -4, 1, 0},
{ 6, s_1_239, -5, 1, 0},
{ 3, s_1_240, -6, 1, 0},
{ 4, s_1_241, -7, 1, 0},
{ 8, s_1_242, -1, 6, 0},
{ 3, s_1_243, -9, 1, 0},
{ 2, s_1_244, 0, 1, 0},
{ 4, s_1_245, -1, 1, 0},
{ 2, s_1_246, 0, 1, 0},
{ 3, s_1_247, -1, 1, 0},
{ 5, s_1_248, -1, -1, 0},
{ 4, s_1_249, -2, 1, 0},
{ 4, s_1_250, -3, 1, 0},
{ 3, s_1_251, -5, 1, 0},
{ 4, s_1_252, -6, 1, 0},
{ 3, s_1_253, -7, 1, 0},
{ 3, s_1_254, 0, 1, 0},
{ 2, s_1_255, 0, 1, 0},
{ 3, s_1_256, -1, 1, 0},
{ 3, s_1_257, -2, 1, 0},
{ 3, s_1_258, 0, 1, 0},
{ 3, s_1_259, 0, 1, 0},
{ 6, s_1_260, -1, 1, 0},
{ 2, s_1_261, 0, 1, 0},
{ 2, s_1_262, 0, 1, 0},
{ 2, s_1_263, 0, 1, 0},
{ 3, s_1_264, -1, 1, 0},
{ 5, s_1_265, -2, 1, 0},
{ 5, s_1_266, -3, -1, 0},
{ 4, s_1_267, -4, 1, 0},
{ 4, s_1_268, -5, 1, 0},
{ 3, s_1_269, -6, 1, 0},
{ 4, s_1_270, -7, 1, 0},
{ 2, s_1_271, 0, 2, 0},
{ 3, s_1_272, -1, 1, 0},
{ 2, s_1_273, 0, 1, 0},
{ 3, s_1_274, 0, 1, 0},
{ 2, s_1_275, 0, 1, 0},
{ 5, s_1_276, -1, 1, 0},
{ 4, s_1_277, -2, 1, 0},
{ 4, s_1_278, 0, 1, 0},
{ 4, s_1_279, 0, 2, 0},
{ 4, s_1_280, 0, 1, 0},
{ 3, s_1_281, 0, 1, 0},
{ 2, s_1_282, 0, 1, 0},
{ 4, s_1_283, -1, 4, 0},
{ 5, s_1_284, -2, 1, 0},
{ 4, s_1_285, -3, 1, 0},
{ 3, s_1_286, 0, 1, 0},
{ 2, s_1_287, 0, 1, 0},
{ 3, s_1_288, -1, 1, 0},
{ 6, s_1_289, -1, 1, 0},
{ 1, s_1_290, 0, 1, 0},
{ 2, s_1_291, -1, 1, 0},
{ 4, s_1_292, -2, 1, 0},
{ 2, s_1_293, -3, 1, 0},
{ 5, s_1_294, -1, 1, 0}
};

static const symbol s_2_0[4] = { 'z', 'l', 'e', 'a' };
static const symbol s_2_1[5] = { 'k', 'e', 'r', 'i', 'a' };
static const symbol s_2_2[2] = { 'l', 'a' };
static const symbol s_2_3[3] = { 'e', 'r', 'a' };
static const symbol s_2_4[4] = { 'd', 'a', 'd', 'e' };
static const symbol s_2_5[4] = { 't', 'a', 'd', 'e' };
static const symbol s_2_6[4] = { 'd', 'a', 't', 'e' };
static const symbol s_2_7[4] = { 't', 'a', 't', 'e' };
static const symbol s_2_8[2] = { 'g', 'i' };
static const symbol s_2_9[2] = { 'k', 'i' };
static const symbol s_2_10[2] = { 'i', 'k' };
static const symbol s_2_11[5] = { 'l', 'a', 'n', 'i', 'k' };
static const symbol s_2_12[3] = { 'r', 'i', 'k' };
static const symbol s_2_13[5] = { 'l', 'a', 'r', 'i', 'k' };
static const symbol s_2_14[4] = { 'z', 't', 'i', 'k' };
static const symbol s_2_15[2] = { 'g', 'o' };
static const symbol s_2_16[2] = { 'r', 'o' };
static const symbol s_2_17[3] = { 'e', 'r', 'o' };
static const symbol s_2_18[2] = { 't', 'o' };
static const struct among a_2[19] = {
{ 4, s_2_0, 0, 2, 0},
{ 5, s_2_1, 0, 1, 0},
{ 2, s_2_2, 0, 1, 0},
{ 3, s_2_3, 0, 1, 0},
{ 4, s_2_4, 0, 1, 0},
{ 4, s_2_5, 0, 1, 0},
{ 4, s_2_6, 0, 1, 0},
{ 4, s_2_7, 0, 1, 0},
{ 2, s_2_8, 0, 1, 0},
{ 2, s_2_9, 0, 1, 0},
{ 2, s_2_10, 0, 1, 0},
{ 5, s_2_11, -1, 1, 0},
{ 3, s_2_12, -2, 1, 0},
{ 5, s_2_13, -1, 1, 0},
{ 4, s_2_14, -4, 1, 0},
{ 2, s_2_15, 0, 1, 0},
{ 2, s_2_16, 0, 1, 0},
{ 3, s_2_17, -1, 1, 0},
{ 2, s_2_18, 0, 1, 0}
};

static const unsigned char g_v[] = { 17, 65, 16 };

static int r_mark_regions(struct SN_env * z) {
    ((SN_local *)z)->i_pV = z->l;
    ((SN_local *)z)->i_p1 = z->l;
    ((SN_local *)z)->i_p2 = z->l;
    {
        int v_1 = z->c;
        do {
            int v_2 = z->c;
            if (in_grouping(z, g_v, 97, 117, 0)) goto lab1;
            do {
                int v_3 = z->c;
                if (out_grouping(z, g_v, 97, 117, 0)) goto lab2;
                {
                    int ret = out_grouping(z, g_v, 97, 117, 1);
                    if (ret < 0) goto lab2;
                    z->c += ret;
                }
                break;
            lab2:
                z->c = v_3;
                if (in_grouping(z, g_v, 97, 117, 0)) goto lab1;
                {
                    int ret = in_grouping(z, g_v, 97, 117, 1);
                    if (ret < 0) goto lab1;
                    z->c += ret;
                }
            } while (0);
            break;
        lab1:
            z->c = v_2;
            if (out_grouping(z, g_v, 97, 117, 0)) goto lab0;
            do {
                int v_4 = z->c;
                if (out_grouping(z, g_v, 97, 117, 0)) goto lab3;
                {
                    int ret = out_grouping(z, g_v, 97, 117, 1);
                    if (ret < 0) goto lab3;
                    z->c += ret;
                }
                break;
            lab3:
                z->c = v_4;
                if (in_grouping(z, g_v, 97, 117, 0)) goto lab0;
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
            int ret = out_grouping(z, g_v, 97, 117, 1);
            if (ret < 0) goto lab4;
            z->c += ret;
        }
        {
            int ret = in_grouping(z, g_v, 97, 117, 1);
            if (ret < 0) goto lab4;
            z->c += ret;
        }
        ((SN_local *)z)->i_p1 = z->c;
        {
            int ret = out_grouping(z, g_v, 97, 117, 1);
            if (ret < 0) goto lab4;
            z->c += ret;
        }
        {
            int ret = in_grouping(z, g_v, 97, 117, 1);
            if (ret < 0) goto lab4;
            z->c += ret;
        }
        ((SN_local *)z)->i_p2 = z->c;
    lab4:
        z->c = v_5;
    }
    return 1;
}

static int r_RV(struct SN_env * z) {
    return ((SN_local *)z)->i_pV <= z->c;
}

static int r_R2(struct SN_env * z) {
    return ((SN_local *)z)->i_p2 <= z->c;
}

static int r_R1(struct SN_env * z) {
    return ((SN_local *)z)->i_p1 <= z->c;
}

static int r_aditzak(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((70566434 >> (z->p[z->c - 1] & 0x1f)) & 1)) return 0;
    among_var = find_among_b(z, a_0, 109, 0);
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

static int r_izenak(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    if (z->c <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((71162402 >> (z->p[z->c - 1] & 0x1f)) & 1)) return 0;
    among_var = find_among_b(z, a_1, 295, 0);
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
                int ret = slice_from_s(z, 3, s_0);
                if (ret < 0) return ret;
            }
            break;
        case 4:
            {
                int ret = r_R1(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 5:
            {
                int ret = slice_from_s(z, 3, s_1);
                if (ret < 0) return ret;
            }
            break;
        case 6:
            {
                int ret = slice_from_s(z, 6, s_2);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_adjetiboak(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((35362 >> (z->p[z->c - 1] & 0x1f)) & 1)) return 0;
    among_var = find_among_b(z, a_2, 19, 0);
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
                int ret = slice_from_s(z, 1, s_3);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

extern int basque_ISO_8859_1_stem(struct SN_env * z) {
    {
        int ret = r_mark_regions(z);
        if (ret < 0) return ret;
    }
    z->lb = z->c; z->c = z->l;
    while (1) {
        int v_1 = z->l - z->c;
        {
            int ret = r_aditzak(z);
            if (ret == 0) goto lab0;
            if (ret < 0) return ret;
        }
        continue;
    lab0:
        z->c = z->l - v_1;
        break;
    }
    while (1) {
        int v_2 = z->l - z->c;
        {
            int ret = r_izenak(z);
            if (ret == 0) goto lab1;
            if (ret < 0) return ret;
        }
        continue;
    lab1:
        z->c = z->l - v_2;
        break;
    }
    {
        int v_3 = z->l - z->c;
        {
            int ret = r_adjetiboak(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_3;
    }
    z->c = z->lb;
    return 1;
}

extern struct SN_env * basque_ISO_8859_1_create_env(void) {
    struct SN_env * z = SN_new_env(sizeof(SN_local));
    if (z) {
        ((SN_local *)z)->i_p2 = 0;
        ((SN_local *)z)->i_p1 = 0;
        ((SN_local *)z)->i_pV = 0;
    }
    return z;
}

extern void basque_ISO_8859_1_close_env(struct SN_env * z) {
    SN_delete_env(z);
}

