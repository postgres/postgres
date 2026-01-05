/* Generated from polish.sbl by Snowball 3.0.0 - https://snowballstem.org/ */

#include "stem_ISO_8859_2_polish.h"

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
extern int polish_ISO_8859_2_stem(struct SN_env * z);
#ifdef __cplusplus
}
#endif

static int r_R1(struct SN_env * z);
static int r_normalize_consonant(struct SN_env * z);
static int r_remove_endings(struct SN_env * z);
static int r_mark_regions(struct SN_env * z);

static const symbol s_0[] = { 's' };
static const symbol s_1[] = { 's' };
static const symbol s_2[] = { 0xB3 };
static const symbol s_3[] = { 's' };
static const symbol s_4[] = { 'c' };
static const symbol s_5[] = { 'n' };
static const symbol s_6[] = { 's' };
static const symbol s_7[] = { 'z' };

static const symbol s_0_0[6] = { 'b', 'y', 0xB6, 'c', 'i', 'e' };
static const symbol s_0_1[3] = { 'b', 'y', 'm' };
static const symbol s_0_2[2] = { 'b', 'y' };
static const symbol s_0_3[5] = { 'b', 'y', 0xB6, 'm', 'y' };
static const symbol s_0_4[3] = { 'b', 'y', 0xB6 };
static const struct among a_0[5] = {
{ 6, s_0_0, 0, 1, 0},
{ 3, s_0_1, 0, 1, 0},
{ 2, s_0_2, 0, 1, 0},
{ 5, s_0_3, 0, 1, 0},
{ 3, s_0_4, 0, 1, 0}
};

static const symbol s_1_0[2] = { 0xB1, 'c' };
static const symbol s_1_1[4] = { 'a', 'j', 0xB1, 'c' };
static const symbol s_1_2[4] = { 's', 'z', 0xB1, 'c' };
static const symbol s_1_3[2] = { 's', 'z' };
static const symbol s_1_4[5] = { 'i', 'e', 'j', 's', 'z' };
static const struct among a_1[5] = {
{ 2, s_1_0, 0, 1, 0},
{ 4, s_1_1, -1, 1, 0},
{ 4, s_1_2, -2, 2, 0},
{ 2, s_1_3, 0, 1, 0},
{ 5, s_1_4, -1, 1, 0}
};

static const symbol s_2_0[1] = { 'a' };
static const symbol s_2_1[3] = { 0xB1, 'c', 'a' };
static const symbol s_2_2[5] = { 'a', 'j', 0xB1, 'c', 'a' };
static const symbol s_2_3[5] = { 's', 'z', 0xB1, 'c', 'a' };
static const symbol s_2_4[2] = { 'i', 'a' };
static const symbol s_2_5[3] = { 's', 'z', 'a' };
static const symbol s_2_6[6] = { 'i', 'e', 'j', 's', 'z', 'a' };
static const symbol s_2_7[3] = { 'a', 0xB3, 'a' };
static const symbol s_2_8[4] = { 'i', 'a', 0xB3, 'a' };
static const symbol s_2_9[3] = { 'i', 0xB3, 'a' };
static const symbol s_2_10[2] = { 0xB1, 'c' };
static const symbol s_2_11[4] = { 'a', 'j', 0xB1, 'c' };
static const symbol s_2_12[1] = { 'e' };
static const symbol s_2_13[3] = { 0xB1, 'c', 'e' };
static const symbol s_2_14[5] = { 'a', 'j', 0xB1, 'c', 'e' };
static const symbol s_2_15[5] = { 's', 'z', 0xB1, 'c', 'e' };
static const symbol s_2_16[2] = { 'i', 'e' };
static const symbol s_2_17[3] = { 'c', 'i', 'e' };
static const symbol s_2_18[4] = { 'a', 'c', 'i', 'e' };
static const symbol s_2_19[4] = { 'e', 'c', 'i', 'e' };
static const symbol s_2_20[4] = { 'i', 'c', 'i', 'e' };
static const symbol s_2_21[5] = { 'a', 'j', 'c', 'i', 'e' };
static const symbol s_2_22[6] = { 'l', 'i', 0xB6, 'c', 'i', 'e' };
static const symbol s_2_23[7] = { 'a', 'l', 'i', 0xB6, 'c', 'i', 'e' };
static const symbol s_2_24[8] = { 'i', 'e', 'l', 'i', 0xB6, 'c', 'i', 'e' };
static const symbol s_2_25[7] = { 'i', 'l', 'i', 0xB6, 'c', 'i', 'e' };
static const symbol s_2_26[6] = { 0xB3, 'y', 0xB6, 'c', 'i', 'e' };
static const symbol s_2_27[7] = { 'a', 0xB3, 'y', 0xB6, 'c', 'i', 'e' };
static const symbol s_2_28[8] = { 'i', 'a', 0xB3, 'y', 0xB6, 'c', 'i', 'e' };
static const symbol s_2_29[7] = { 'i', 0xB3, 'y', 0xB6, 'c', 'i', 'e' };
static const symbol s_2_30[3] = { 's', 'z', 'e' };
static const symbol s_2_31[6] = { 'i', 'e', 'j', 's', 'z', 'e' };
static const symbol s_2_32[3] = { 'a', 'c', 'h' };
static const symbol s_2_33[4] = { 'i', 'a', 'c', 'h' };
static const symbol s_2_34[3] = { 'i', 'c', 'h' };
static const symbol s_2_35[3] = { 'y', 'c', 'h' };
static const symbol s_2_36[1] = { 'i' };
static const symbol s_2_37[3] = { 'a', 'l', 'i' };
static const symbol s_2_38[4] = { 'i', 'e', 'l', 'i' };
static const symbol s_2_39[3] = { 'i', 'l', 'i' };
static const symbol s_2_40[3] = { 'a', 'm', 'i' };
static const symbol s_2_41[4] = { 'i', 'a', 'm', 'i' };
static const symbol s_2_42[3] = { 'i', 'm', 'i' };
static const symbol s_2_43[3] = { 'y', 'm', 'i' };
static const symbol s_2_44[3] = { 'o', 'w', 'i' };
static const symbol s_2_45[4] = { 'i', 'o', 'w', 'i' };
static const symbol s_2_46[2] = { 'a', 'j' };
static const symbol s_2_47[2] = { 'e', 'j' };
static const symbol s_2_48[3] = { 'i', 'e', 'j' };
static const symbol s_2_49[2] = { 'a', 'm' };
static const symbol s_2_50[4] = { 'a', 0xB3, 'a', 'm' };
static const symbol s_2_51[5] = { 'i', 'a', 0xB3, 'a', 'm' };
static const symbol s_2_52[4] = { 'i', 0xB3, 'a', 'm' };
static const symbol s_2_53[2] = { 'e', 'm' };
static const symbol s_2_54[3] = { 'i', 'e', 'm' };
static const symbol s_2_55[4] = { 'a', 0xB3, 'e', 'm' };
static const symbol s_2_56[5] = { 'i', 'a', 0xB3, 'e', 'm' };
static const symbol s_2_57[4] = { 'i', 0xB3, 'e', 'm' };
static const symbol s_2_58[2] = { 'i', 'm' };
static const symbol s_2_59[2] = { 'o', 'm' };
static const symbol s_2_60[3] = { 'i', 'o', 'm' };
static const symbol s_2_61[2] = { 'y', 'm' };
static const symbol s_2_62[1] = { 'o' };
static const symbol s_2_63[3] = { 'e', 'g', 'o' };
static const symbol s_2_64[4] = { 'i', 'e', 'g', 'o' };
static const symbol s_2_65[3] = { 'a', 0xB3, 'o' };
static const symbol s_2_66[4] = { 'i', 'a', 0xB3, 'o' };
static const symbol s_2_67[3] = { 'i', 0xB3, 'o' };
static const symbol s_2_68[1] = { 'u' };
static const symbol s_2_69[2] = { 'i', 'u' };
static const symbol s_2_70[3] = { 'e', 'm', 'u' };
static const symbol s_2_71[4] = { 'i', 'e', 'm', 'u' };
static const symbol s_2_72[2] = { 0xF3, 'w' };
static const symbol s_2_73[1] = { 'y' };
static const symbol s_2_74[3] = { 'a', 'm', 'y' };
static const symbol s_2_75[3] = { 'e', 'm', 'y' };
static const symbol s_2_76[3] = { 'i', 'm', 'y' };
static const symbol s_2_77[5] = { 'l', 'i', 0xB6, 'm', 'y' };
static const symbol s_2_78[6] = { 'a', 'l', 'i', 0xB6, 'm', 'y' };
static const symbol s_2_79[7] = { 'i', 'e', 'l', 'i', 0xB6, 'm', 'y' };
static const symbol s_2_80[6] = { 'i', 'l', 'i', 0xB6, 'm', 'y' };
static const symbol s_2_81[5] = { 0xB3, 'y', 0xB6, 'm', 'y' };
static const symbol s_2_82[6] = { 'a', 0xB3, 'y', 0xB6, 'm', 'y' };
static const symbol s_2_83[7] = { 'i', 'a', 0xB3, 'y', 0xB6, 'm', 'y' };
static const symbol s_2_84[6] = { 'i', 0xB3, 'y', 0xB6, 'm', 'y' };
static const symbol s_2_85[3] = { 'a', 0xB3, 'y' };
static const symbol s_2_86[4] = { 'i', 'a', 0xB3, 'y' };
static const symbol s_2_87[3] = { 'i', 0xB3, 'y' };
static const symbol s_2_88[3] = { 'a', 's', 'z' };
static const symbol s_2_89[3] = { 'e', 's', 'z' };
static const symbol s_2_90[3] = { 'i', 's', 'z' };
static const symbol s_2_91[1] = { 0xB1 };
static const symbol s_2_92[3] = { 0xB1, 'c', 0xB1 };
static const symbol s_2_93[5] = { 'a', 'j', 0xB1, 'c', 0xB1 };
static const symbol s_2_94[5] = { 's', 'z', 0xB1, 'c', 0xB1 };
static const symbol s_2_95[2] = { 'i', 0xB1 };
static const symbol s_2_96[3] = { 'a', 'j', 0xB1 };
static const symbol s_2_97[3] = { 's', 'z', 0xB1 };
static const symbol s_2_98[6] = { 'i', 'e', 'j', 's', 'z', 0xB1 };
static const symbol s_2_99[2] = { 'a', 0xB3 };
static const symbol s_2_100[3] = { 'i', 'a', 0xB3 };
static const symbol s_2_101[2] = { 'i', 0xB3 };
static const symbol s_2_102[3] = { 0xB3, 'a', 0xB6 };
static const symbol s_2_103[4] = { 'a', 0xB3, 'a', 0xB6 };
static const symbol s_2_104[5] = { 'i', 'a', 0xB3, 'a', 0xB6 };
static const symbol s_2_105[4] = { 'i', 0xB3, 'a', 0xB6 };
static const symbol s_2_106[3] = { 0xB3, 'e', 0xB6 };
static const symbol s_2_107[4] = { 'a', 0xB3, 'e', 0xB6 };
static const symbol s_2_108[5] = { 'i', 'a', 0xB3, 'e', 0xB6 };
static const symbol s_2_109[4] = { 'i', 0xB3, 'e', 0xB6 };
static const symbol s_2_110[2] = { 'a', 0xE6 };
static const symbol s_2_111[3] = { 'i', 'e', 0xE6 };
static const symbol s_2_112[2] = { 'i', 0xE6 };
static const symbol s_2_113[2] = { 0xB1, 0xE6 };
static const symbol s_2_114[3] = { 'a', 0xB6, 0xE6 };
static const symbol s_2_115[3] = { 'e', 0xB6, 0xE6 };
static const symbol s_2_116[1] = { 0xEA };
static const symbol s_2_117[3] = { 's', 'z', 0xEA };
static const struct among a_2[118] = {
{ 1, s_2_0, 0, 1, 1},
{ 3, s_2_1, -1, 1, 0},
{ 5, s_2_2, -1, 1, 0},
{ 5, s_2_3, -2, 2, 0},
{ 2, s_2_4, -4, 1, 1},
{ 3, s_2_5, -5, 1, 0},
{ 6, s_2_6, -1, 1, 0},
{ 3, s_2_7, -7, 1, 0},
{ 4, s_2_8, -1, 1, 0},
{ 3, s_2_9, -9, 1, 0},
{ 2, s_2_10, 0, 1, 0},
{ 4, s_2_11, -1, 1, 0},
{ 1, s_2_12, 0, 1, 1},
{ 3, s_2_13, -1, 1, 0},
{ 5, s_2_14, -1, 1, 0},
{ 5, s_2_15, -2, 2, 0},
{ 2, s_2_16, -4, 1, 1},
{ 3, s_2_17, -1, 1, 0},
{ 4, s_2_18, -1, 1, 0},
{ 4, s_2_19, -2, 1, 0},
{ 4, s_2_20, -3, 1, 0},
{ 5, s_2_21, -4, 1, 0},
{ 6, s_2_22, -5, 4, 0},
{ 7, s_2_23, -1, 1, 0},
{ 8, s_2_24, -2, 1, 0},
{ 7, s_2_25, -3, 1, 0},
{ 6, s_2_26, -9, 4, 0},
{ 7, s_2_27, -1, 1, 0},
{ 8, s_2_28, -1, 1, 0},
{ 7, s_2_29, -3, 1, 0},
{ 3, s_2_30, -18, 1, 0},
{ 6, s_2_31, -1, 1, 0},
{ 3, s_2_32, 0, 1, 1},
{ 4, s_2_33, -1, 1, 1},
{ 3, s_2_34, 0, 5, 0},
{ 3, s_2_35, 0, 5, 0},
{ 1, s_2_36, 0, 1, 1},
{ 3, s_2_37, -1, 1, 0},
{ 4, s_2_38, -2, 1, 0},
{ 3, s_2_39, -3, 1, 0},
{ 3, s_2_40, -4, 1, 1},
{ 4, s_2_41, -1, 1, 1},
{ 3, s_2_42, -6, 5, 0},
{ 3, s_2_43, -7, 5, 0},
{ 3, s_2_44, -8, 1, 1},
{ 4, s_2_45, -1, 1, 1},
{ 2, s_2_46, 0, 1, 0},
{ 2, s_2_47, 0, 5, 0},
{ 3, s_2_48, -1, 5, 0},
{ 2, s_2_49, 0, 1, 0},
{ 4, s_2_50, -1, 1, 0},
{ 5, s_2_51, -1, 1, 0},
{ 4, s_2_52, -3, 1, 0},
{ 2, s_2_53, 0, 1, 1},
{ 3, s_2_54, -1, 1, 1},
{ 4, s_2_55, -2, 1, 0},
{ 5, s_2_56, -1, 1, 0},
{ 4, s_2_57, -4, 1, 0},
{ 2, s_2_58, 0, 5, 0},
{ 2, s_2_59, 0, 1, 1},
{ 3, s_2_60, -1, 1, 1},
{ 2, s_2_61, 0, 5, 0},
{ 1, s_2_62, 0, 1, 1},
{ 3, s_2_63, -1, 5, 0},
{ 4, s_2_64, -1, 5, 0},
{ 3, s_2_65, -3, 1, 0},
{ 4, s_2_66, -1, 1, 0},
{ 3, s_2_67, -5, 1, 0},
{ 1, s_2_68, 0, 1, 1},
{ 2, s_2_69, -1, 1, 1},
{ 3, s_2_70, -2, 5, 0},
{ 4, s_2_71, -1, 5, 0},
{ 2, s_2_72, 0, 1, 1},
{ 1, s_2_73, 0, 5, 0},
{ 3, s_2_74, -1, 1, 0},
{ 3, s_2_75, -2, 1, 0},
{ 3, s_2_76, -3, 1, 0},
{ 5, s_2_77, -4, 4, 0},
{ 6, s_2_78, -1, 1, 0},
{ 7, s_2_79, -2, 1, 0},
{ 6, s_2_80, -3, 1, 0},
{ 5, s_2_81, -8, 4, 0},
{ 6, s_2_82, -1, 1, 0},
{ 7, s_2_83, -1, 1, 0},
{ 6, s_2_84, -3, 1, 0},
{ 3, s_2_85, -12, 1, 0},
{ 4, s_2_86, -1, 1, 0},
{ 3, s_2_87, -14, 1, 0},
{ 3, s_2_88, 0, 1, 0},
{ 3, s_2_89, 0, 1, 0},
{ 3, s_2_90, 0, 1, 0},
{ 1, s_2_91, 0, 1, 1},
{ 3, s_2_92, -1, 1, 0},
{ 5, s_2_93, -1, 1, 0},
{ 5, s_2_94, -2, 2, 0},
{ 2, s_2_95, -4, 1, 1},
{ 3, s_2_96, -5, 1, 0},
{ 3, s_2_97, -6, 3, 0},
{ 6, s_2_98, -1, 1, 0},
{ 2, s_2_99, 0, 1, 0},
{ 3, s_2_100, -1, 1, 0},
{ 2, s_2_101, 0, 1, 0},
{ 3, s_2_102, 0, 4, 0},
{ 4, s_2_103, -1, 1, 0},
{ 5, s_2_104, -1, 1, 0},
{ 4, s_2_105, -3, 1, 0},
{ 3, s_2_106, 0, 4, 0},
{ 4, s_2_107, -1, 1, 0},
{ 5, s_2_108, -1, 1, 0},
{ 4, s_2_109, -3, 1, 0},
{ 2, s_2_110, 0, 1, 0},
{ 3, s_2_111, 0, 1, 0},
{ 2, s_2_112, 0, 1, 0},
{ 2, s_2_113, 0, 1, 0},
{ 3, s_2_114, 0, 1, 0},
{ 3, s_2_115, 0, 1, 0},
{ 1, s_2_116, 0, 1, 0},
{ 3, s_2_117, -1, 2, 0}
};

static const symbol s_3_0[1] = { 0xB6 };
static const symbol s_3_1[1] = { 0xBC };
static const symbol s_3_2[1] = { 0xE6 };
static const symbol s_3_3[1] = { 0xF1 };
static const struct among a_3[4] = {
{ 1, s_3_0, 0, 3, 0},
{ 1, s_3_1, 0, 4, 0},
{ 1, s_3_2, 0, 1, 0},
{ 1, s_3_3, 0, 2, 0}
};

static const unsigned char g_v[] = { 17, 65, 16, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 2, 4 };

static int r_mark_regions(struct SN_env * z) {
    ((SN_local *)z)->i_p1 = z->l;
    {
        int ret = out_grouping(z, g_v, 97, 243, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    {
        int ret = in_grouping(z, g_v, 97, 243, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    ((SN_local *)z)->i_p1 = z->c;
    return 1;
}

static int r_R1(struct SN_env * z) {
    return ((SN_local *)z)->i_p1 <= z->c;
}

static int r_remove_endings(struct SN_env * z) {
    int among_var;
    {
        int v_1 = z->l - z->c;
        {
            int v_2;
            if (z->c < ((SN_local *)z)->i_p1) goto lab0;
            v_2 = z->lb; z->lb = ((SN_local *)z)->i_p1;
            z->ket = z->c;
            if (!find_among_b(z, a_0, 5, 0)) { z->lb = v_2; goto lab0; }
            z->bra = z->c;
            z->lb = v_2;
        }
        {
            int ret = slice_del(z);
            if (ret < 0) return ret;
        }
    lab0:
        z->c = z->l - v_1;
    }
    z->ket = z->c;
    among_var = find_among_b(z, a_2, 118, r_R1);
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
                int ret = slice_from_s(z, 1, s_0);
                if (ret < 0) return ret;
            }
            break;
        case 3:
            do {
                int v_3 = z->l - z->c;
                {
                    int v_4 = z->l - z->c;
                    {
                        int ret = r_R1(z);
                        if (ret == 0) goto lab1;
                        if (ret < 0) return ret;
                    }
                    z->c = z->l - v_4;
                    {
                        int ret = slice_del(z);
                        if (ret < 0) return ret;
                    }
                }
                break;
            lab1:
                z->c = z->l - v_3;
                {
                    int ret = slice_from_s(z, 1, s_1);
                    if (ret < 0) return ret;
                }
            } while (0);
            break;
        case 4:
            {
                int ret = slice_from_s(z, 1, s_2);
                if (ret < 0) return ret;
            }
            break;
        case 5:
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            {
                int v_5 = z->l - z->c;
                z->ket = z->c;
                if (z->c - 1 <= z->lb || (z->p[z->c - 1] != 99 && z->p[z->c - 1] != 122)) { z->c = z->l - v_5; goto lab2; }
                among_var = find_among_b(z, a_1, 5, 0);
                if (!among_var) { z->c = z->l - v_5; goto lab2; }
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
                            int ret = slice_from_s(z, 1, s_3);
                            if (ret < 0) return ret;
                        }
                        break;
                }
            lab2:
                ;
            }
            break;
    }
    return 1;
}

static int r_normalize_consonant(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    among_var = find_among_b(z, a_3, 4, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    if (z->c > z->lb) goto lab0;
    return 0;
lab0:
    switch (among_var) {
        case 1:
            {
                int ret = slice_from_s(z, 1, s_4);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {
                int ret = slice_from_s(z, 1, s_5);
                if (ret < 0) return ret;
            }
            break;
        case 3:
            {
                int ret = slice_from_s(z, 1, s_6);
                if (ret < 0) return ret;
            }
            break;
        case 4:
            {
                int ret = slice_from_s(z, 1, s_7);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

extern int polish_ISO_8859_2_stem(struct SN_env * z) {
    {
        int v_1 = z->c;
        {
            int ret = r_mark_regions(z);
            if (ret < 0) return ret;
        }
        z->c = v_1;
    }
    do {
        int v_2 = z->c;
        if (z->c + 2 > z->l) goto lab0;
        z->c += 2;
        z->lb = z->c; z->c = z->l;
        {
            int ret = r_remove_endings(z);
            if (ret == 0) goto lab0;
            if (ret < 0) return ret;
        }
        z->c = z->lb;
        break;
    lab0:
        z->c = v_2;
        z->lb = z->c; z->c = z->l;
        {
            int ret = r_normalize_consonant(z);
            if (ret <= 0) return ret;
        }
        z->c = z->lb;
    } while (0);
    return 1;
}

extern struct SN_env * polish_ISO_8859_2_create_env(void) {
    struct SN_env * z = SN_new_env(sizeof(SN_local));
    if (z) {
        ((SN_local *)z)->i_p1 = 0;
    }
    return z;
}

extern void polish_ISO_8859_2_close_env(struct SN_env * z) {
    SN_delete_env(z);
}

