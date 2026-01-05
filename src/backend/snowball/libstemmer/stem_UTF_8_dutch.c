/* Generated from dutch.sbl by Snowball 3.0.0 - https://snowballstem.org/ */

#include "stem_UTF_8_dutch.h"

#include <stddef.h>

#include "snowball_runtime.h"

struct SN_local {
    struct SN_env z;
    int i_p2;
    int i_p1;
    unsigned char b_GE_removed;
    symbol * s_ch;
};

typedef struct SN_local SN_local;

#ifdef __cplusplus
extern "C" {
#endif
extern int dutch_UTF_8_stem(struct SN_env * z);
#ifdef __cplusplus
}
#endif

static int r_measure(struct SN_env * z);
static int r_Lose_infix(struct SN_env * z);
static int r_Lose_prefix(struct SN_env * z);
static int r_Step_1c(struct SN_env * z);
static int r_Step_6(struct SN_env * z);
static int r_Step_7(struct SN_env * z);
static int r_Step_4(struct SN_env * z);
static int r_Step_3(struct SN_env * z);
static int r_Step_2(struct SN_env * z);
static int r_Step_1(struct SN_env * z);
static int r_lengthen_V(struct SN_env * z);
static int r_VX(struct SN_env * z);
static int r_V(struct SN_env * z);
static int r_C(struct SN_env * z);
static int r_R2(struct SN_env * z);
static int r_R1(struct SN_env * z);

static const symbol s_0[] = { 'i', 'j' };
static const symbol s_1[] = { 'i', 'j' };
static const symbol s_2[] = { 'i', 'j' };
static const symbol s_3[] = { 'e', 0xC3, 0xAB, 'e' };
static const symbol s_4[] = { 'i', 'e', 'e' };
static const symbol s_5[] = { 'i', 'e' };
static const symbol s_6[] = { 'a', 'r' };
static const symbol s_7[] = { 'e', 'r' };
static const symbol s_8[] = { 'e' };
static const symbol s_9[] = { 0xC3, 0xA9 };
static const symbol s_10[] = { 'a', 'u' };
static const symbol s_11[] = { 'h', 'e', 'd' };
static const symbol s_12[] = { 'h', 'e', 'i', 'd' };
static const symbol s_13[] = { 'n', 'd' };
static const symbol s_14[] = { 'n', 'd' };
static const symbol s_15[] = { '\'', 't' };
static const symbol s_16[] = { 'e', 't' };
static const symbol s_17[] = { 'r', 'n', 't' };
static const symbol s_18[] = { 'r', 'n' };
static const symbol s_19[] = { 'i', 'n', 'k' };
static const symbol s_20[] = { 'i', 'n', 'g' };
static const symbol s_21[] = { 'm', 'p' };
static const symbol s_22[] = { 'm' };
static const symbol s_23[] = { 'g' };
static const symbol s_24[] = { 'l', 'i', 'j', 'k' };
static const symbol s_25[] = { 'i', 's', 'c', 'h' };
static const symbol s_26[] = { 't' };
static const symbol s_27[] = { 's' };
static const symbol s_28[] = { 'r' };
static const symbol s_29[] = { 'l' };
static const symbol s_30[] = { 'e', 'n' };
static const symbol s_31[] = { 'i', 'e', 'f' };
static const symbol s_32[] = { 'e', 'e', 'r' };
static const symbol s_33[] = { 'r' };
static const symbol s_34[] = { 'i', 'l', 'd' };
static const symbol s_35[] = { 'e', 'r' };
static const symbol s_36[] = { 'a', 'a', 'r' };
static const symbol s_37[] = { 'f' };
static const symbol s_38[] = { 'g' };
static const symbol s_39[] = { 't' };
static const symbol s_40[] = { 'd' };
static const symbol s_41[] = { 'i', 'e' };
static const symbol s_42[] = { 'e', 'e', 'r' };
static const symbol s_43[] = { 'n' };
static const symbol s_44[] = { 'l' };
static const symbol s_45[] = { 'r' };
static const symbol s_46[] = { 't', 'e', 'e', 'r' };
static const symbol s_47[] = { 'l', 'i', 'j', 'k' };
static const symbol s_48[] = { 'i', 'n', 'n' };
static const symbol s_49[] = { 'k' };
static const symbol s_50[] = { 'f' };
static const symbol s_51[] = { 'p' };
static const symbol s_52[] = { 'b' };
static const symbol s_53[] = { 'c' };
static const symbol s_54[] = { 'd' };
static const symbol s_55[] = { 'f' };
static const symbol s_56[] = { 'g' };
static const symbol s_57[] = { 'h' };
static const symbol s_58[] = { 'j' };
static const symbol s_59[] = { 'k' };
static const symbol s_60[] = { 'l' };
static const symbol s_61[] = { 'm' };
static const symbol s_62[] = { 'n' };
static const symbol s_63[] = { 'p' };
static const symbol s_64[] = { 'q' };
static const symbol s_65[] = { 'r' };
static const symbol s_66[] = { 's' };
static const symbol s_67[] = { 't' };
static const symbol s_68[] = { 'v' };
static const symbol s_69[] = { 'w' };
static const symbol s_70[] = { 'x' };
static const symbol s_71[] = { 'z' };
static const symbol s_72[] = { 'i', 'n' };
static const symbol s_73[] = { 'n' };
static const symbol s_74[] = { 'e', 'n' };
static const symbol s_75[] = { 'g', 'e' };
static const symbol s_76[] = { 'i', 'j' };
static const symbol s_77[] = { 'i', 'j' };
static const symbol s_78[] = { 'e' };
static const symbol s_79[] = { 'i' };
static const symbol s_80[] = { 'g', 'e' };
static const symbol s_81[] = { 'i', 'j' };
static const symbol s_82[] = { 'i', 'j' };
static const symbol s_83[] = { 'e' };
static const symbol s_84[] = { 'i' };
static const symbol s_85[] = { 'i', 'j' };
static const symbol s_86[] = { 'i', 'j' };

static const symbol s_0_0[1] = { 'a' };
static const symbol s_0_1[1] = { 'e' };
static const symbol s_0_2[1] = { 'o' };
static const symbol s_0_3[1] = { 'u' };
static const symbol s_0_4[2] = { 0xC3, 0xA0 };
static const symbol s_0_5[2] = { 0xC3, 0xA1 };
static const symbol s_0_6[2] = { 0xC3, 0xA2 };
static const symbol s_0_7[2] = { 0xC3, 0xA4 };
static const symbol s_0_8[2] = { 0xC3, 0xA8 };
static const symbol s_0_9[2] = { 0xC3, 0xA9 };
static const symbol s_0_10[2] = { 0xC3, 0xAA };
static const symbol s_0_11[3] = { 'e', 0xC3, 0xAB };
static const symbol s_0_12[3] = { 'i', 0xC3, 0xAB };
static const symbol s_0_13[2] = { 0xC3, 0xB2 };
static const symbol s_0_14[2] = { 0xC3, 0xB3 };
static const symbol s_0_15[2] = { 0xC3, 0xB4 };
static const symbol s_0_16[2] = { 0xC3, 0xB6 };
static const symbol s_0_17[2] = { 0xC3, 0xB9 };
static const symbol s_0_18[2] = { 0xC3, 0xBA };
static const symbol s_0_19[2] = { 0xC3, 0xBB };
static const symbol s_0_20[2] = { 0xC3, 0xBC };
static const struct among a_0[21] = {
{ 1, s_0_0, 0, 1, 0},
{ 1, s_0_1, 0, 2, 0},
{ 1, s_0_2, 0, 1, 0},
{ 1, s_0_3, 0, 1, 0},
{ 2, s_0_4, 0, 1, 0},
{ 2, s_0_5, 0, 1, 0},
{ 2, s_0_6, 0, 1, 0},
{ 2, s_0_7, 0, 1, 0},
{ 2, s_0_8, 0, 2, 0},
{ 2, s_0_9, 0, 2, 0},
{ 2, s_0_10, 0, 2, 0},
{ 3, s_0_11, 0, 3, 0},
{ 3, s_0_12, 0, 4, 0},
{ 2, s_0_13, 0, 1, 0},
{ 2, s_0_14, 0, 1, 0},
{ 2, s_0_15, 0, 1, 0},
{ 2, s_0_16, 0, 1, 0},
{ 2, s_0_17, 0, 1, 0},
{ 2, s_0_18, 0, 1, 0},
{ 2, s_0_19, 0, 1, 0},
{ 2, s_0_20, 0, 1, 0}
};

static const symbol s_1_0[3] = { 'n', 'd', 'e' };
static const symbol s_1_1[2] = { 'e', 'n' };
static const symbol s_1_2[1] = { 's' };
static const symbol s_1_3[2] = { '\'', 's' };
static const symbol s_1_4[2] = { 'e', 's' };
static const symbol s_1_5[3] = { 'i', 'e', 's' };
static const symbol s_1_6[3] = { 'a', 'u', 's' };
static const symbol s_1_7[3] = { 0xC3, 0xA9, 's' };
static const struct among a_1[8] = {
{ 3, s_1_0, 0, 8, 0},
{ 2, s_1_1, 0, 7, 0},
{ 1, s_1_2, 0, 2, 0},
{ 2, s_1_3, -1, 1, 0},
{ 2, s_1_4, -2, 4, 0},
{ 3, s_1_5, -1, 3, 0},
{ 3, s_1_6, -4, 6, 0},
{ 3, s_1_7, -5, 5, 0}
};

static const symbol s_2_0[2] = { 'd', 'e' };
static const symbol s_2_1[2] = { 'g', 'e' };
static const symbol s_2_2[5] = { 'i', 's', 'c', 'h', 'e' };
static const symbol s_2_3[2] = { 'j', 'e' };
static const symbol s_2_4[5] = { 'l', 'i', 'j', 'k', 'e' };
static const symbol s_2_5[2] = { 'l', 'e' };
static const symbol s_2_6[3] = { 'e', 'n', 'e' };
static const symbol s_2_7[2] = { 'r', 'e' };
static const symbol s_2_8[2] = { 's', 'e' };
static const symbol s_2_9[2] = { 't', 'e' };
static const symbol s_2_10[4] = { 'i', 'e', 'v', 'e' };
static const struct among a_2[11] = {
{ 2, s_2_0, 0, 5, 0},
{ 2, s_2_1, 0, 2, 0},
{ 5, s_2_2, 0, 4, 0},
{ 2, s_2_3, 0, 1, 0},
{ 5, s_2_4, 0, 3, 0},
{ 2, s_2_5, 0, 9, 0},
{ 3, s_2_6, 0, 10, 0},
{ 2, s_2_7, 0, 8, 0},
{ 2, s_2_8, 0, 7, 0},
{ 2, s_2_9, 0, 6, 0},
{ 4, s_2_10, 0, 11, 0}
};

static const symbol s_3_0[4] = { 'h', 'e', 'i', 'd' };
static const symbol s_3_1[3] = { 'f', 'i', 'e' };
static const symbol s_3_2[3] = { 'g', 'i', 'e' };
static const symbol s_3_3[4] = { 'a', 't', 'i', 'e' };
static const symbol s_3_4[4] = { 'i', 's', 'm', 'e' };
static const symbol s_3_5[3] = { 'i', 'n', 'g' };
static const symbol s_3_6[4] = { 'a', 'r', 'i', 'j' };
static const symbol s_3_7[4] = { 'e', 'r', 'i', 'j' };
static const symbol s_3_8[3] = { 's', 'e', 'l' };
static const symbol s_3_9[4] = { 'r', 'd', 'e', 'r' };
static const symbol s_3_10[4] = { 's', 't', 'e', 'r' };
static const symbol s_3_11[5] = { 'i', 't', 'e', 'i', 't' };
static const symbol s_3_12[3] = { 'd', 's', 't' };
static const symbol s_3_13[3] = { 't', 's', 't' };
static const struct among a_3[14] = {
{ 4, s_3_0, 0, 3, 0},
{ 3, s_3_1, 0, 7, 0},
{ 3, s_3_2, 0, 8, 0},
{ 4, s_3_3, 0, 1, 0},
{ 4, s_3_4, 0, 5, 0},
{ 3, s_3_5, 0, 5, 0},
{ 4, s_3_6, 0, 6, 0},
{ 4, s_3_7, 0, 5, 0},
{ 3, s_3_8, 0, 3, 0},
{ 4, s_3_9, 0, 4, 0},
{ 4, s_3_10, 0, 3, 0},
{ 5, s_3_11, 0, 2, 0},
{ 3, s_3_12, 0, 10, 0},
{ 3, s_3_13, 0, 9, 0}
};

static const symbol s_4_0[3] = { 'e', 'n', 'd' };
static const symbol s_4_1[5] = { 'a', 't', 'i', 'e', 'f' };
static const symbol s_4_2[4] = { 'e', 'r', 'i', 'g' };
static const symbol s_4_3[6] = { 'a', 'c', 'h', 't', 'i', 'g' };
static const symbol s_4_4[6] = { 'i', 'o', 'n', 'e', 'e', 'l' };
static const symbol s_4_5[4] = { 'b', 'a', 'a', 'r' };
static const symbol s_4_6[4] = { 'l', 'a', 'a', 'r' };
static const symbol s_4_7[4] = { 'n', 'a', 'a', 'r' };
static const symbol s_4_8[4] = { 'r', 'a', 'a', 'r' };
static const symbol s_4_9[6] = { 'e', 'r', 'i', 'g', 'e', 'r' };
static const symbol s_4_10[8] = { 'a', 'c', 'h', 't', 'i', 'g', 'e', 'r' };
static const symbol s_4_11[6] = { 'l', 'i', 'j', 'k', 'e', 'r' };
static const symbol s_4_12[4] = { 't', 'a', 'n', 't' };
static const symbol s_4_13[6] = { 'e', 'r', 'i', 'g', 's', 't' };
static const symbol s_4_14[8] = { 'a', 'c', 'h', 't', 'i', 'g', 's', 't' };
static const symbol s_4_15[6] = { 'l', 'i', 'j', 'k', 's', 't' };
static const struct among a_4[16] = {
{ 3, s_4_0, 0, 9, 0},
{ 5, s_4_1, 0, 2, 0},
{ 4, s_4_2, 0, 9, 0},
{ 6, s_4_3, 0, 3, 0},
{ 6, s_4_4, 0, 1, 0},
{ 4, s_4_5, 0, 3, 0},
{ 4, s_4_6, 0, 5, 0},
{ 4, s_4_7, 0, 4, 0},
{ 4, s_4_8, 0, 6, 0},
{ 6, s_4_9, 0, 9, 0},
{ 8, s_4_10, 0, 3, 0},
{ 6, s_4_11, 0, 8, 0},
{ 4, s_4_12, 0, 7, 0},
{ 6, s_4_13, 0, 9, 0},
{ 8, s_4_14, 0, 3, 0},
{ 6, s_4_15, 0, 8, 0}
};

static const symbol s_5_0[2] = { 'i', 'g' };
static const symbol s_5_1[4] = { 'i', 'g', 'e', 'r' };
static const symbol s_5_2[4] = { 'i', 'g', 's', 't' };
static const struct among a_5[3] = {
{ 2, s_5_0, 0, 1, 0},
{ 4, s_5_1, 0, 1, 0},
{ 4, s_5_2, 0, 1, 0}
};

static const symbol s_6_0[2] = { 'f', 't' };
static const symbol s_6_1[2] = { 'k', 't' };
static const symbol s_6_2[2] = { 'p', 't' };
static const struct among a_6[3] = {
{ 2, s_6_0, 0, 2, 0},
{ 2, s_6_1, 0, 1, 0},
{ 2, s_6_2, 0, 3, 0}
};

static const symbol s_7_0[2] = { 'b', 'b' };
static const symbol s_7_1[2] = { 'c', 'c' };
static const symbol s_7_2[2] = { 'd', 'd' };
static const symbol s_7_3[2] = { 'f', 'f' };
static const symbol s_7_4[2] = { 'g', 'g' };
static const symbol s_7_5[2] = { 'h', 'h' };
static const symbol s_7_6[2] = { 'j', 'j' };
static const symbol s_7_7[2] = { 'k', 'k' };
static const symbol s_7_8[2] = { 'l', 'l' };
static const symbol s_7_9[2] = { 'm', 'm' };
static const symbol s_7_10[2] = { 'n', 'n' };
static const symbol s_7_11[2] = { 'p', 'p' };
static const symbol s_7_12[2] = { 'q', 'q' };
static const symbol s_7_13[2] = { 'r', 'r' };
static const symbol s_7_14[2] = { 's', 's' };
static const symbol s_7_15[2] = { 't', 't' };
static const symbol s_7_16[1] = { 'v' };
static const symbol s_7_17[2] = { 'v', 'v' };
static const symbol s_7_18[2] = { 'w', 'w' };
static const symbol s_7_19[2] = { 'x', 'x' };
static const symbol s_7_20[1] = { 'z' };
static const symbol s_7_21[2] = { 'z', 'z' };
static const struct among a_7[22] = {
{ 2, s_7_0, 0, 1, 0},
{ 2, s_7_1, 0, 2, 0},
{ 2, s_7_2, 0, 3, 0},
{ 2, s_7_3, 0, 4, 0},
{ 2, s_7_4, 0, 5, 0},
{ 2, s_7_5, 0, 6, 0},
{ 2, s_7_6, 0, 7, 0},
{ 2, s_7_7, 0, 8, 0},
{ 2, s_7_8, 0, 9, 0},
{ 2, s_7_9, 0, 10, 0},
{ 2, s_7_10, 0, 11, 0},
{ 2, s_7_11, 0, 12, 0},
{ 2, s_7_12, 0, 13, 0},
{ 2, s_7_13, 0, 14, 0},
{ 2, s_7_14, 0, 15, 0},
{ 2, s_7_15, 0, 16, 0},
{ 1, s_7_16, 0, 4, 0},
{ 2, s_7_17, -1, 17, 0},
{ 2, s_7_18, 0, 18, 0},
{ 2, s_7_19, 0, 19, 0},
{ 1, s_7_20, 0, 15, 0},
{ 2, s_7_21, -1, 20, 0}
};

static const symbol s_8_0[1] = { 'd' };
static const symbol s_8_1[1] = { 't' };
static const struct among a_8[2] = {
{ 1, s_8_0, 0, 1, 0},
{ 1, s_8_1, 0, 2, 0}
};

static const symbol s_9_1[3] = { 'e', 'f', 't' };
static const symbol s_9_2[3] = { 'v', 'a', 'a' };
static const symbol s_9_3[3] = { 'v', 'a', 'l' };
static const symbol s_9_4[4] = { 'v', 'a', 'l', 'i' };
static const symbol s_9_5[4] = { 'v', 'a', 'r', 'e' };
static const struct among a_9[6] = {
{ 0, 0, 0, -1, 0},
{ 3, s_9_1, -1, 1, 0},
{ 3, s_9_2, -2, 1, 0},
{ 3, s_9_3, -3, 1, 0},
{ 4, s_9_4, -1, -1, 0},
{ 4, s_9_5, -5, 1, 0}
};

static const symbol s_10_0[2] = { 0xC3, 0xAB };
static const symbol s_10_1[2] = { 0xC3, 0xAF };
static const struct among a_10[2] = {
{ 2, s_10_0, 0, 1, 0},
{ 2, s_10_1, 0, 2, 0}
};

static const symbol s_11_0[2] = { 0xC3, 0xAB };
static const symbol s_11_1[2] = { 0xC3, 0xAF };
static const struct among a_11[2] = {
{ 2, s_11_0, 0, 1, 0},
{ 2, s_11_1, 0, 2, 0}
};

static const unsigned char g_E[] = { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 120 };

static const unsigned char g_AIOU[] = { 1, 65, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 128, 11, 120, 46, 15 };

static const unsigned char g_AEIOU[] = { 17, 65, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 128, 139, 127, 46, 15 };

static const unsigned char g_v[] = { 17, 65, 16, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 128, 139, 127, 46, 15 };

static const unsigned char g_v_WX[] = { 17, 65, 208, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 128, 139, 127, 46, 15 };

static int r_R1(struct SN_env * z) {
    return ((SN_local *)z)->i_p1 <= z->c;
}

static int r_R2(struct SN_env * z) {
    return ((SN_local *)z)->i_p2 <= z->c;
}

static int r_V(struct SN_env * z) {
    {
        int v_1 = z->l - z->c;
        do {
            int v_2 = z->l - z->c;
            if (in_grouping_b_U(z, g_v, 97, 252, 0)) goto lab0;
            break;
        lab0:
            z->c = z->l - v_2;
            if (!(eq_s_b(z, 2, s_0))) return 0;
        } while (0);
        z->c = z->l - v_1;
    }
    return 1;
}

static int r_VX(struct SN_env * z) {
    {
        int v_1 = z->l - z->c;
        {
            int ret = skip_b_utf8(z->p, z->c, z->lb, 1);
            if (ret < 0) return 0;
            z->c = ret;
        }
        do {
            int v_2 = z->l - z->c;
            if (in_grouping_b_U(z, g_v, 97, 252, 0)) goto lab0;
            break;
        lab0:
            z->c = z->l - v_2;
            if (!(eq_s_b(z, 2, s_1))) return 0;
        } while (0);
        z->c = z->l - v_1;
    }
    return 1;
}

static int r_C(struct SN_env * z) {
    {
        int v_1 = z->l - z->c;
        {
            int v_2 = z->l - z->c;
            if (!(eq_s_b(z, 2, s_2))) goto lab0;
            return 0;
        lab0:
            z->c = z->l - v_2;
        }
        if (out_grouping_b_U(z, g_v, 97, 252, 0)) return 0;
        z->c = z->l - v_1;
    }
    return 1;
}

static int r_lengthen_V(struct SN_env * z) {
    int among_var;
    {
        int v_1 = z->l - z->c;
        if (out_grouping_b_U(z, g_v_WX, 97, 252, 0)) goto lab0;
        z->ket = z->c;
        among_var = find_among_b(z, a_0, 21, 0);
        if (!among_var) goto lab0;
        z->bra = z->c;
        switch (among_var) {
            case 1:
                {
                    int v_2 = z->l - z->c;
                    do {
                        int v_3 = z->l - z->c;
                        if (out_grouping_b_U(z, g_AEIOU, 97, 252, 0)) goto lab1;
                        break;
                    lab1:
                        z->c = z->l - v_3;
                        if (z->c > z->lb) goto lab0;
                    } while (0);
                    z->c = z->l - v_2;
                }
                {
                    int ret = slice_to(z, &((SN_local *)z)->s_ch);
                    if (ret < 0) return ret;
                }
                {
                    int saved_c = z->c;
                    int ret = insert_v(z, z->c, z->c, ((SN_local *)z)->s_ch);
                    z->c = saved_c;
                    if (ret < 0) return ret;
                }
                break;
            case 2:
                {
                    int v_4 = z->l - z->c;
                    do {
                        int v_5 = z->l - z->c;
                        if (out_grouping_b_U(z, g_AEIOU, 97, 252, 0)) goto lab2;
                        break;
                    lab2:
                        z->c = z->l - v_5;
                        if (z->c > z->lb) goto lab0;
                    } while (0);
                    {
                        int v_6 = z->l - z->c;
                        do {
                            int v_7 = z->l - z->c;
                            if (in_grouping_b_U(z, g_AIOU, 97, 252, 0)) goto lab4;
                            break;
                        lab4:
                            z->c = z->l - v_7;
                            if (in_grouping_b_U(z, g_E, 101, 235, 0)) goto lab3;
                            if (z->c > z->lb) goto lab3;
                        } while (0);
                        goto lab0;
                    lab3:
                        z->c = z->l - v_6;
                    }
                    {
                        int v_8 = z->l - z->c;
                        {
                            int ret = skip_b_utf8(z->p, z->c, z->lb, 1);
                            if (ret < 0) goto lab5;
                            z->c = ret;
                        }
                        if (in_grouping_b_U(z, g_AIOU, 97, 252, 0)) goto lab5;
                        if (out_grouping_b_U(z, g_AEIOU, 97, 252, 0)) goto lab5;
                        goto lab0;
                    lab5:
                        z->c = z->l - v_8;
                    }
                    z->c = z->l - v_4;
                }
                {
                    int ret = slice_to(z, &((SN_local *)z)->s_ch);
                    if (ret < 0) return ret;
                }
                {
                    int saved_c = z->c;
                    int ret = insert_v(z, z->c, z->c, ((SN_local *)z)->s_ch);
                    z->c = saved_c;
                    if (ret < 0) return ret;
                }
                break;
            case 3:
                {
                    int ret = slice_from_s(z, 4, s_3);
                    if (ret < 0) return ret;
                }
                break;
            case 4:
                {
                    int ret = slice_from_s(z, 3, s_4);
                    if (ret < 0) return ret;
                }
                break;
        }
    lab0:
        z->c = z->l - v_1;
    }
    return 1;
}

static int r_Step_1(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    if (z->c <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((540704 >> (z->p[z->c - 1] & 0x1f)) & 1)) return 0;
    among_var = find_among_b(z, a_1, 8, 0);
    if (!among_var) return 0;
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
                int ret = r_R1(z);
                if (ret <= 0) return ret;
            }
            {
                int v_1 = z->l - z->c;
                if (z->c <= z->lb || z->p[z->c - 1] != 't') goto lab0;
                z->c--;
                {
                    int ret = r_R1(z);
                    if (ret == 0) goto lab0;
                    if (ret < 0) return ret;
                }
                return 0;
            lab0:
                z->c = z->l - v_1;
            }
            {
                int ret = r_C(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 3:
            {
                int ret = r_R1(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_from_s(z, 2, s_5);
                if (ret < 0) return ret;
            }
            break;
        case 4:
            do {
                int v_2 = z->l - z->c;
                {
                    int v_3 = z->l - z->c;
                    if (!(eq_s_b(z, 2, s_6))) goto lab1;
                    {
                        int ret = r_R1(z);
                        if (ret == 0) goto lab1;
                        if (ret < 0) return ret;
                    }
                    {
                        int ret = r_C(z);
                        if (ret == 0) goto lab1;
                        if (ret < 0) return ret;
                    }
                    z->c = z->l - v_3;
                }
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                {
                    int ret = r_lengthen_V(z);
                    if (ret < 0) return ret;
                }
                break;
            lab1:
                z->c = z->l - v_2;
                {
                    int v_4 = z->l - z->c;
                    if (!(eq_s_b(z, 2, s_7))) goto lab2;
                    {
                        int ret = r_R1(z);
                        if (ret == 0) goto lab2;
                        if (ret < 0) return ret;
                    }
                    {
                        int ret = r_C(z);
                        if (ret == 0) goto lab2;
                        if (ret < 0) return ret;
                    }
                    z->c = z->l - v_4;
                }
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                break;
            lab2:
                z->c = z->l - v_2;
                {
                    int ret = r_R1(z);
                    if (ret <= 0) return ret;
                }
                {
                    int ret = r_C(z);
                    if (ret <= 0) return ret;
                }
                {
                    int ret = slice_from_s(z, 1, s_8);
                    if (ret < 0) return ret;
                }
            } while (0);
            break;
        case 5:
            {
                int ret = r_R1(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_from_s(z, 2, s_9);
                if (ret < 0) return ret;
            }
            break;
        case 6:
            {
                int ret = r_R1(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = r_V(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_from_s(z, 2, s_10);
                if (ret < 0) return ret;
            }
            break;
        case 7:
            do {
                int v_5 = z->l - z->c;
                if (!(eq_s_b(z, 3, s_11))) goto lab3;
                {
                    int ret = r_R1(z);
                    if (ret == 0) goto lab3;
                    if (ret < 0) return ret;
                }
                z->bra = z->c;
                {
                    int ret = slice_from_s(z, 4, s_12);
                    if (ret < 0) return ret;
                }
                break;
            lab3:
                z->c = z->l - v_5;
                if (!(eq_s_b(z, 2, s_13))) goto lab4;
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                break;
            lab4:
                z->c = z->l - v_5;
                if (z->c <= z->lb || z->p[z->c - 1] != 'd') goto lab5;
                z->c--;
                {
                    int ret = r_R1(z);
                    if (ret == 0) goto lab5;
                    if (ret < 0) return ret;
                }
                {
                    int ret = r_C(z);
                    if (ret == 0) goto lab5;
                    if (ret < 0) return ret;
                }
                z->bra = z->c;
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                break;
            lab5:
                z->c = z->l - v_5;
                do {
                    int v_6 = z->l - z->c;
                    if (z->c <= z->lb || z->p[z->c - 1] != 'i') goto lab7;
                    z->c--;
                    break;
                lab7:
                    z->c = z->l - v_6;
                    if (z->c <= z->lb || z->p[z->c - 1] != 'j') goto lab6;
                    z->c--;
                } while (0);
                {
                    int ret = r_V(z);
                    if (ret == 0) goto lab6;
                    if (ret < 0) return ret;
                }
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                break;
            lab6:
                z->c = z->l - v_5;
                {
                    int ret = r_R1(z);
                    if (ret <= 0) return ret;
                }
                {
                    int ret = r_C(z);
                    if (ret <= 0) return ret;
                }
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                {
                    int ret = r_lengthen_V(z);
                    if (ret <= 0) return ret;
                }
            } while (0);
            break;
        case 8:
            {
                int ret = slice_from_s(z, 2, s_14);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_Step_2(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    if (z->c - 1 <= z->lb || z->p[z->c - 1] != 101) return 0;
    among_var = find_among_b(z, a_2, 11, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    switch (among_var) {
        case 1:
            do {
                int v_1 = z->l - z->c;
                if (!(eq_s_b(z, 2, s_15))) goto lab0;
                z->bra = z->c;
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                break;
            lab0:
                z->c = z->l - v_1;
                if (!(eq_s_b(z, 2, s_16))) goto lab1;
                z->bra = z->c;
                {
                    int ret = r_R1(z);
                    if (ret == 0) goto lab1;
                    if (ret < 0) return ret;
                }
                {
                    int ret = r_C(z);
                    if (ret == 0) goto lab1;
                    if (ret < 0) return ret;
                }
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                break;
            lab1:
                z->c = z->l - v_1;
                if (!(eq_s_b(z, 3, s_17))) goto lab2;
                z->bra = z->c;
                {
                    int ret = slice_from_s(z, 2, s_18);
                    if (ret < 0) return ret;
                }
                break;
            lab2:
                z->c = z->l - v_1;
                if (z->c <= z->lb || z->p[z->c - 1] != 't') goto lab3;
                z->c--;
                z->bra = z->c;
                {
                    int ret = r_R1(z);
                    if (ret == 0) goto lab3;
                    if (ret < 0) return ret;
                }
                {
                    int ret = r_VX(z);
                    if (ret == 0) goto lab3;
                    if (ret < 0) return ret;
                }
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                break;
            lab3:
                z->c = z->l - v_1;
                if (!(eq_s_b(z, 3, s_19))) goto lab4;
                z->bra = z->c;
                {
                    int ret = slice_from_s(z, 3, s_20);
                    if (ret < 0) return ret;
                }
                break;
            lab4:
                z->c = z->l - v_1;
                if (!(eq_s_b(z, 2, s_21))) goto lab5;
                z->bra = z->c;
                {
                    int ret = slice_from_s(z, 1, s_22);
                    if (ret < 0) return ret;
                }
                break;
            lab5:
                z->c = z->l - v_1;
                if (z->c <= z->lb || z->p[z->c - 1] != '\'') goto lab6;
                z->c--;
                z->bra = z->c;
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
            lab6:
                z->c = z->l - v_1;
                z->bra = z->c;
                {
                    int ret = r_R1(z);
                    if (ret <= 0) return ret;
                }
                {
                    int ret = r_C(z);
                    if (ret <= 0) return ret;
                }
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
            } while (0);
            break;
        case 2:
            {
                int ret = r_R1(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_from_s(z, 1, s_23);
                if (ret < 0) return ret;
            }
            break;
        case 3:
            {
                int ret = r_R1(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_from_s(z, 4, s_24);
                if (ret < 0) return ret;
            }
            break;
        case 4:
            {
                int ret = r_R1(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_from_s(z, 4, s_25);
                if (ret < 0) return ret;
            }
            break;
        case 5:
            {
                int ret = r_R1(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = r_C(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 6:
            {
                int ret = r_R1(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_from_s(z, 1, s_26);
                if (ret < 0) return ret;
            }
            break;
        case 7:
            {
                int ret = r_R1(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_from_s(z, 1, s_27);
                if (ret < 0) return ret;
            }
            break;
        case 8:
            {
                int ret = r_R1(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_from_s(z, 1, s_28);
                if (ret < 0) return ret;
            }
            break;
        case 9:
            {
                int ret = r_R1(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            {
                int ret = insert_s(z, z->c, z->c, 1, s_29);
                if (ret < 0) return ret;
            }
            {
                int ret = r_lengthen_V(z);
                if (ret <= 0) return ret;
            }
            break;
        case 10:
            {
                int ret = r_R1(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = r_C(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            {
                int ret = insert_s(z, z->c, z->c, 2, s_30);
                if (ret < 0) return ret;
            }
            {
                int ret = r_lengthen_V(z);
                if (ret <= 0) return ret;
            }
            break;
        case 11:
            {
                int ret = r_R1(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = r_C(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_from_s(z, 3, s_31);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_Step_3(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    if (z->c - 2 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((1316016 >> (z->p[z->c - 1] & 0x1f)) & 1)) return 0;
    among_var = find_among_b(z, a_3, 14, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    switch (among_var) {
        case 1:
            {
                int ret = r_R1(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_from_s(z, 3, s_32);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {
                int ret = r_R1(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            {
                int ret = r_lengthen_V(z);
                if (ret <= 0) return ret;
            }
            break;
        case 3:
            {
                int ret = r_R1(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 4:
            {
                int ret = slice_from_s(z, 1, s_33);
                if (ret < 0) return ret;
            }
            break;
        case 5:
            do {
                int v_1 = z->l - z->c;
                if (!(eq_s_b(z, 3, s_34))) goto lab0;
                {
                    int ret = slice_from_s(z, 2, s_35);
                    if (ret < 0) return ret;
                }
                break;
            lab0:
                z->c = z->l - v_1;
                {
                    int ret = r_R1(z);
                    if (ret <= 0) return ret;
                }
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                {
                    int ret = r_lengthen_V(z);
                    if (ret <= 0) return ret;
                }
            } while (0);
            break;
        case 6:
            {
                int ret = r_R1(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = r_C(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_from_s(z, 3, s_36);
                if (ret < 0) return ret;
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
                int ret = insert_s(z, z->c, z->c, 1, s_37);
                if (ret < 0) return ret;
            }
            {
                int ret = r_lengthen_V(z);
                if (ret <= 0) return ret;
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
                int ret = insert_s(z, z->c, z->c, 1, s_38);
                if (ret < 0) return ret;
            }
            {
                int ret = r_lengthen_V(z);
                if (ret <= 0) return ret;
            }
            break;
        case 9:
            {
                int ret = r_R1(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = r_C(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_from_s(z, 1, s_39);
                if (ret < 0) return ret;
            }
            break;
        case 10:
            {
                int ret = r_R1(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = r_C(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_from_s(z, 1, s_40);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_Step_4(struct SN_env * z) {
    int among_var;
    do {
        int v_1 = z->l - z->c;
        z->ket = z->c;
        if (z->c - 2 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((1315024 >> (z->p[z->c - 1] & 0x1f)) & 1)) goto lab0;
        among_var = find_among_b(z, a_4, 16, 0);
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
                    int ret = slice_from_s(z, 2, s_41);
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
                    int ret = slice_from_s(z, 3, s_42);
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
                break;
            case 4:
                {
                    int ret = r_R1(z);
                    if (ret == 0) goto lab0;
                    if (ret < 0) return ret;
                }
                {
                    int ret = r_V(z);
                    if (ret == 0) goto lab0;
                    if (ret < 0) return ret;
                }
                {
                    int ret = slice_from_s(z, 1, s_43);
                    if (ret < 0) return ret;
                }
                break;
            case 5:
                {
                    int ret = r_R1(z);
                    if (ret == 0) goto lab0;
                    if (ret < 0) return ret;
                }
                {
                    int ret = r_V(z);
                    if (ret == 0) goto lab0;
                    if (ret < 0) return ret;
                }
                {
                    int ret = slice_from_s(z, 1, s_44);
                    if (ret < 0) return ret;
                }
                break;
            case 6:
                {
                    int ret = r_R1(z);
                    if (ret == 0) goto lab0;
                    if (ret < 0) return ret;
                }
                {
                    int ret = r_V(z);
                    if (ret == 0) goto lab0;
                    if (ret < 0) return ret;
                }
                {
                    int ret = slice_from_s(z, 1, s_45);
                    if (ret < 0) return ret;
                }
                break;
            case 7:
                {
                    int ret = r_R1(z);
                    if (ret == 0) goto lab0;
                    if (ret < 0) return ret;
                }
                {
                    int ret = slice_from_s(z, 4, s_46);
                    if (ret < 0) return ret;
                }
                break;
            case 8:
                {
                    int ret = r_R1(z);
                    if (ret == 0) goto lab0;
                    if (ret < 0) return ret;
                }
                {
                    int ret = slice_from_s(z, 4, s_47);
                    if (ret < 0) return ret;
                }
                break;
            case 9:
                {
                    int ret = r_R1(z);
                    if (ret == 0) goto lab0;
                    if (ret < 0) return ret;
                }
                {
                    int ret = r_C(z);
                    if (ret == 0) goto lab0;
                    if (ret < 0) return ret;
                }
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                {
                    int ret = r_lengthen_V(z);
                    if (ret < 0) return ret;
                }
                break;
        }
        break;
    lab0:
        z->c = z->l - v_1;
        z->ket = z->c;
        if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((1310848 >> (z->p[z->c - 1] & 0x1f)) & 1)) return 0;
        if (!find_among_b(z, a_5, 3, 0)) return 0;
        z->bra = z->c;
        {
            int ret = r_R1(z);
            if (ret <= 0) return ret;
        }
        {
            int v_2 = z->l - z->c;
            if (!(eq_s_b(z, 3, s_48))) goto lab1;
            if (z->c > z->lb) goto lab1;
            return 0;
        lab1:
            z->c = z->l - v_2;
        }
        {
            int ret = r_C(z);
            if (ret <= 0) return ret;
        }
        {
            int ret = slice_del(z);
            if (ret < 0) return ret;
        }
        {
            int ret = r_lengthen_V(z);
            if (ret <= 0) return ret;
        }
    } while (0);
    return 1;
}

static int r_Step_7(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    if (z->c - 1 <= z->lb || z->p[z->c - 1] != 116) return 0;
    among_var = find_among_b(z, a_6, 3, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    switch (among_var) {
        case 1:
            {
                int ret = slice_from_s(z, 1, s_49);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {
                int ret = slice_from_s(z, 1, s_50);
                if (ret < 0) return ret;
            }
            break;
        case 3:
            {
                int ret = slice_from_s(z, 1, s_51);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_Step_6(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    if (z->c <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((98532828 >> (z->p[z->c - 1] & 0x1f)) & 1)) return 0;
    among_var = find_among_b(z, a_7, 22, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    switch (among_var) {
        case 1:
            {
                int ret = slice_from_s(z, 1, s_52);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {
                int ret = slice_from_s(z, 1, s_53);
                if (ret < 0) return ret;
            }
            break;
        case 3:
            {
                int ret = slice_from_s(z, 1, s_54);
                if (ret < 0) return ret;
            }
            break;
        case 4:
            {
                int ret = slice_from_s(z, 1, s_55);
                if (ret < 0) return ret;
            }
            break;
        case 5:
            {
                int ret = slice_from_s(z, 1, s_56);
                if (ret < 0) return ret;
            }
            break;
        case 6:
            {
                int ret = slice_from_s(z, 1, s_57);
                if (ret < 0) return ret;
            }
            break;
        case 7:
            {
                int ret = slice_from_s(z, 1, s_58);
                if (ret < 0) return ret;
            }
            break;
        case 8:
            {
                int ret = slice_from_s(z, 1, s_59);
                if (ret < 0) return ret;
            }
            break;
        case 9:
            {
                int ret = slice_from_s(z, 1, s_60);
                if (ret < 0) return ret;
            }
            break;
        case 10:
            {
                int ret = slice_from_s(z, 1, s_61);
                if (ret < 0) return ret;
            }
            break;
        case 11:
            {
                int v_1 = z->l - z->c;
                if (z->c <= z->lb || z->p[z->c - 1] != 'i') goto lab0;
                z->c--;
                if (z->c > z->lb) goto lab0;
                return 0;
            lab0:
                z->c = z->l - v_1;
            }
            {
                int ret = slice_from_s(z, 1, s_62);
                if (ret < 0) return ret;
            }
            break;
        case 12:
            {
                int ret = slice_from_s(z, 1, s_63);
                if (ret < 0) return ret;
            }
            break;
        case 13:
            {
                int ret = slice_from_s(z, 1, s_64);
                if (ret < 0) return ret;
            }
            break;
        case 14:
            {
                int ret = slice_from_s(z, 1, s_65);
                if (ret < 0) return ret;
            }
            break;
        case 15:
            {
                int ret = slice_from_s(z, 1, s_66);
                if (ret < 0) return ret;
            }
            break;
        case 16:
            {
                int ret = slice_from_s(z, 1, s_67);
                if (ret < 0) return ret;
            }
            break;
        case 17:
            {
                int ret = slice_from_s(z, 1, s_68);
                if (ret < 0) return ret;
            }
            break;
        case 18:
            {
                int ret = slice_from_s(z, 1, s_69);
                if (ret < 0) return ret;
            }
            break;
        case 19:
            {
                int ret = slice_from_s(z, 1, s_70);
                if (ret < 0) return ret;
            }
            break;
        case 20:
            {
                int ret = slice_from_s(z, 1, s_71);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_Step_1c(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    if (z->c <= z->lb || (z->p[z->c - 1] != 100 && z->p[z->c - 1] != 116)) return 0;
    among_var = find_among_b(z, a_8, 2, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    {
        int ret = r_R1(z);
        if (ret <= 0) return ret;
    }
    {
        int ret = r_C(z);
        if (ret <= 0) return ret;
    }
    switch (among_var) {
        case 1:
            {
                int v_1 = z->l - z->c;
                if (z->c <= z->lb || z->p[z->c - 1] != 'n') goto lab0;
                z->c--;
                {
                    int ret = r_R1(z);
                    if (ret == 0) goto lab0;
                    if (ret < 0) return ret;
                }
                return 0;
            lab0:
                z->c = z->l - v_1;
            }
            do {
                int v_2 = z->l - z->c;
                if (!(eq_s_b(z, 2, s_72))) goto lab1;
                if (z->c > z->lb) goto lab1;
                {
                    int ret = slice_from_s(z, 1, s_73);
                    if (ret < 0) return ret;
                }
                break;
            lab1:
                z->c = z->l - v_2;
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
            } while (0);
            break;
        case 2:
            {
                int v_3 = z->l - z->c;
                if (z->c <= z->lb || z->p[z->c - 1] != 'h') goto lab2;
                z->c--;
                {
                    int ret = r_R1(z);
                    if (ret == 0) goto lab2;
                    if (ret < 0) return ret;
                }
                return 0;
            lab2:
                z->c = z->l - v_3;
            }
            {
                int v_4 = z->l - z->c;
                if (!(eq_s_b(z, 2, s_74))) goto lab3;
                if (z->c > z->lb) goto lab3;
                return 0;
            lab3:
                z->c = z->l - v_4;
            }
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_Lose_prefix(struct SN_env * z) {
    int among_var;
    z->bra = z->c;
    if (!(eq_s(z, 2, s_75))) return 0;
    z->ket = z->c;
    {
        int v_1 = z->c;
        {
            int ret = skip_utf8(z->p, z->c, z->l, 3);
            if (ret < 0) return 0;
            z->c = ret;
        }
        z->c = v_1;
    }
    {
        int v_2 = z->c;
        while (1) {
            int v_3 = z->c;
            do {
                int v_4 = z->c;
                if (!(eq_s(z, 2, s_76))) goto lab1;
                break;
            lab1:
                z->c = v_4;
                if (in_grouping_U(z, g_v, 97, 252, 0)) goto lab0;
            } while (0);
            break;
        lab0:
            z->c = v_3;
            {
                int ret = skip_utf8(z->p, z->c, z->l, 1);
                if (ret < 0) return 0;
                z->c = ret;
            }
        }
        while (1) {
            int v_5 = z->c;
            do {
                int v_6 = z->c;
                if (!(eq_s(z, 2, s_77))) goto lab3;
                break;
            lab3:
                z->c = v_6;
                if (in_grouping_U(z, g_v, 97, 252, 0)) goto lab2;
            } while (0);
            continue;
        lab2:
            z->c = v_5;
            break;
        }
        if (z->c < z->l) goto lab4;
        return 0;
    lab4:
        z->c = v_2;
    }
    if (z->c + 2 >= z->l || z->p[z->c + 2] >> 5 != 3 || !((1314818 >> (z->p[z->c + 2] & 0x1f)) & 1)) among_var = -1; else
    among_var = find_among(z, a_9, 6, 0);
    switch (among_var) {
        case 1:
            return 0;
            break;
    }
    ((SN_local *)z)->b_GE_removed = 1;
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    {
        int v_7 = z->c;
        z->bra = z->c;
        if (z->c + 1 >= z->l || (z->p[z->c + 1] != 171 && z->p[z->c + 1] != 175)) goto lab5;
        among_var = find_among(z, a_10, 2, 0);
        if (!among_var) goto lab5;
        z->ket = z->c;
        switch (among_var) {
            case 1:
                {
                    int ret = slice_from_s(z, 1, s_78);
                    if (ret < 0) return ret;
                }
                break;
            case 2:
                {
                    int ret = slice_from_s(z, 1, s_79);
                    if (ret < 0) return ret;
                }
                break;
        }
    lab5:
        z->c = v_7;
    }
    return 1;
}

static int r_Lose_infix(struct SN_env * z) {
    int among_var;
    {
        int ret = skip_utf8(z->p, z->c, z->l, 1);
        if (ret < 0) return 0;
        z->c = ret;
    }
    while (1) {
        z->bra = z->c;
        if (!(eq_s(z, 2, s_80))) goto lab0;
        z->ket = z->c;
        break;
    lab0:
        {
            int ret = skip_utf8(z->p, z->c, z->l, 1);
            if (ret < 0) return 0;
            z->c = ret;
        }
    }
    {
        int v_1 = z->c;
        {
            int ret = skip_utf8(z->p, z->c, z->l, 3);
            if (ret < 0) return 0;
            z->c = ret;
        }
        z->c = v_1;
    }
    {
        int v_2 = z->c;
        while (1) {
            int v_3 = z->c;
            do {
                int v_4 = z->c;
                if (!(eq_s(z, 2, s_81))) goto lab2;
                break;
            lab2:
                z->c = v_4;
                if (in_grouping_U(z, g_v, 97, 252, 0)) goto lab1;
            } while (0);
            break;
        lab1:
            z->c = v_3;
            {
                int ret = skip_utf8(z->p, z->c, z->l, 1);
                if (ret < 0) return 0;
                z->c = ret;
            }
        }
        while (1) {
            int v_5 = z->c;
            do {
                int v_6 = z->c;
                if (!(eq_s(z, 2, s_82))) goto lab4;
                break;
            lab4:
                z->c = v_6;
                if (in_grouping_U(z, g_v, 97, 252, 0)) goto lab3;
            } while (0);
            continue;
        lab3:
            z->c = v_5;
            break;
        }
        if (z->c < z->l) goto lab5;
        return 0;
    lab5:
        z->c = v_2;
    }
    ((SN_local *)z)->b_GE_removed = 1;
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    {
        int v_7 = z->c;
        z->bra = z->c;
        if (z->c + 1 >= z->l || (z->p[z->c + 1] != 171 && z->p[z->c + 1] != 175)) goto lab6;
        among_var = find_among(z, a_11, 2, 0);
        if (!among_var) goto lab6;
        z->ket = z->c;
        switch (among_var) {
            case 1:
                {
                    int ret = slice_from_s(z, 1, s_83);
                    if (ret < 0) return ret;
                }
                break;
            case 2:
                {
                    int ret = slice_from_s(z, 1, s_84);
                    if (ret < 0) return ret;
                }
                break;
        }
    lab6:
        z->c = v_7;
    }
    return 1;
}

static int r_measure(struct SN_env * z) {
    ((SN_local *)z)->i_p1 = z->l;
    ((SN_local *)z)->i_p2 = z->l;
    {
        int v_1 = z->c;
        while (1) {
            if (out_grouping_U(z, g_v, 97, 252, 0)) goto lab1;
            continue;
        lab1:
            break;
        }
        {
            int v_2 = 1;
            while (1) {
                int v_3 = z->c;
                do {
                    int v_4 = z->c;
                    if (!(eq_s(z, 2, s_85))) goto lab3;
                    break;
                lab3:
                    z->c = v_4;
                    if (in_grouping_U(z, g_v, 97, 252, 0)) goto lab2;
                } while (0);
                v_2--;
                continue;
            lab2:
                z->c = v_3;
                break;
            }
            if (v_2 > 0) goto lab0;
        }
        if (out_grouping_U(z, g_v, 97, 252, 0)) goto lab0;
        ((SN_local *)z)->i_p1 = z->c;
        while (1) {
            if (out_grouping_U(z, g_v, 97, 252, 0)) goto lab4;
            continue;
        lab4:
            break;
        }
        {
            int v_5 = 1;
            while (1) {
                int v_6 = z->c;
                do {
                    int v_7 = z->c;
                    if (!(eq_s(z, 2, s_86))) goto lab6;
                    break;
                lab6:
                    z->c = v_7;
                    if (in_grouping_U(z, g_v, 97, 252, 0)) goto lab5;
                } while (0);
                v_5--;
                continue;
            lab5:
                z->c = v_6;
                break;
            }
            if (v_5 > 0) goto lab0;
        }
        if (out_grouping_U(z, g_v, 97, 252, 0)) goto lab0;
        ((SN_local *)z)->i_p2 = z->c;
    lab0:
        z->c = v_1;
    }
    return 1;
}

extern int dutch_UTF_8_stem(struct SN_env * z) {
    int b_stemmed;
    b_stemmed = 0;
    {
        int ret = r_measure(z);
        if (ret <= 0) return ret;
    }
    z->lb = z->c; z->c = z->l;
    {
        int v_1 = z->l - z->c;
        {
            int ret = r_Step_1(z);
            if (ret == 0) goto lab0;
            if (ret < 0) return ret;
        }
        b_stemmed = 1;
    lab0:
        z->c = z->l - v_1;
    }
    {
        int v_2 = z->l - z->c;
        {
            int ret = r_Step_2(z);
            if (ret == 0) goto lab1;
            if (ret < 0) return ret;
        }
        b_stemmed = 1;
    lab1:
        z->c = z->l - v_2;
    }
    {
        int v_3 = z->l - z->c;
        {
            int ret = r_Step_3(z);
            if (ret == 0) goto lab2;
            if (ret < 0) return ret;
        }
        b_stemmed = 1;
    lab2:
        z->c = z->l - v_3;
    }
    {
        int v_4 = z->l - z->c;
        {
            int ret = r_Step_4(z);
            if (ret == 0) goto lab3;
            if (ret < 0) return ret;
        }
        b_stemmed = 1;
    lab3:
        z->c = z->l - v_4;
    }
    z->c = z->lb;
    ((SN_local *)z)->b_GE_removed = 0;
    {
        int v_5 = z->c;
        {
            int v_6 = z->c;
            {
                int ret = r_Lose_prefix(z);
                if (ret == 0) goto lab4;
                if (ret < 0) return ret;
            }
            z->c = v_6;
            {
                int ret = r_measure(z);
                if (ret < 0) return ret;
            }
        }
    lab4:
        z->c = v_5;
    }
    z->lb = z->c; z->c = z->l;
    {
        int v_7 = z->l - z->c;
        if (!((SN_local *)z)->b_GE_removed) goto lab5;
        b_stemmed = 1;
        {
            int ret = r_Step_1c(z);
            if (ret == 0) goto lab5;
            if (ret < 0) return ret;
        }
    lab5:
        z->c = z->l - v_7;
    }
    z->c = z->lb;
    ((SN_local *)z)->b_GE_removed = 0;
    {
        int v_8 = z->c;
        {
            int v_9 = z->c;
            {
                int ret = r_Lose_infix(z);
                if (ret == 0) goto lab6;
                if (ret < 0) return ret;
            }
            z->c = v_9;
            {
                int ret = r_measure(z);
                if (ret < 0) return ret;
            }
        }
    lab6:
        z->c = v_8;
    }
    z->lb = z->c; z->c = z->l;
    {
        int v_10 = z->l - z->c;
        if (!((SN_local *)z)->b_GE_removed) goto lab7;
        b_stemmed = 1;
        {
            int ret = r_Step_1c(z);
            if (ret == 0) goto lab7;
            if (ret < 0) return ret;
        }
    lab7:
        z->c = z->l - v_10;
    }
    z->c = z->lb;
    z->lb = z->c; z->c = z->l;
    {
        int v_11 = z->l - z->c;
        {
            int ret = r_Step_7(z);
            if (ret == 0) goto lab8;
            if (ret < 0) return ret;
        }
        b_stemmed = 1;
    lab8:
        z->c = z->l - v_11;
    }
    {
        int v_12 = z->l - z->c;
        if (!b_stemmed) goto lab9;
        {
            int ret = r_Step_6(z);
            if (ret == 0) goto lab9;
            if (ret < 0) return ret;
        }
    lab9:
        z->c = z->l - v_12;
    }
    z->c = z->lb;
    return 1;
}

extern struct SN_env * dutch_UTF_8_create_env(void) {
    struct SN_env * z = SN_new_env(sizeof(SN_local));
    if (z) {
        ((SN_local *)z)->b_GE_removed = 0;
        ((SN_local *)z)->i_p2 = 0;
        ((SN_local *)z)->i_p1 = 0;
        ((SN_local *)z)->s_ch = NULL;

        if ((((SN_local *)z)->s_ch = create_s()) == NULL) {
            dutch_UTF_8_close_env(z);
            return NULL;
        }
    }
    return z;
}

extern void dutch_UTF_8_close_env(struct SN_env * z) {
    if (z) {
        lose_s(((SN_local *)z)->s_ch);
    }
    SN_delete_env(z);
}

