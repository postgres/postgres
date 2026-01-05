/* Generated from porter.sbl by Snowball 3.0.0 - https://snowballstem.org/ */

#include "stem_ISO_8859_1_porter.h"

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
extern int porter_ISO_8859_1_stem(struct SN_env * z);
#ifdef __cplusplus
}
#endif

static int r_Step_5b(struct SN_env * z);
static int r_Step_5a(struct SN_env * z);
static int r_Step_4(struct SN_env * z);
static int r_Step_3(struct SN_env * z);
static int r_Step_2(struct SN_env * z);
static int r_Step_1c(struct SN_env * z);
static int r_Step_1b(struct SN_env * z);
static int r_Step_1a(struct SN_env * z);
static int r_R2(struct SN_env * z);
static int r_R1(struct SN_env * z);
static int r_shortv(struct SN_env * z);

static const symbol s_0[] = { 's', 's' };
static const symbol s_1[] = { 'i' };
static const symbol s_2[] = { 'e', 'e' };
static const symbol s_3[] = { 'e' };
static const symbol s_4[] = { 'e' };
static const symbol s_5[] = { 'i' };
static const symbol s_6[] = { 't', 'i', 'o', 'n' };
static const symbol s_7[] = { 'e', 'n', 'c', 'e' };
static const symbol s_8[] = { 'a', 'n', 'c', 'e' };
static const symbol s_9[] = { 'a', 'b', 'l', 'e' };
static const symbol s_10[] = { 'e', 'n', 't' };
static const symbol s_11[] = { 'e' };
static const symbol s_12[] = { 'i', 'z', 'e' };
static const symbol s_13[] = { 'a', 't', 'e' };
static const symbol s_14[] = { 'a', 'l' };
static const symbol s_15[] = { 'f', 'u', 'l' };
static const symbol s_16[] = { 'o', 'u', 's' };
static const symbol s_17[] = { 'i', 'v', 'e' };
static const symbol s_18[] = { 'b', 'l', 'e' };
static const symbol s_19[] = { 'a', 'l' };
static const symbol s_20[] = { 'i', 'c' };
static const symbol s_21[] = { 'Y' };
static const symbol s_22[] = { 'Y' };
static const symbol s_23[] = { 'y' };

static const symbol s_0_0[1] = { 's' };
static const symbol s_0_1[3] = { 'i', 'e', 's' };
static const symbol s_0_2[4] = { 's', 's', 'e', 's' };
static const symbol s_0_3[2] = { 's', 's' };
static const struct among a_0[4] = {
{ 1, s_0_0, 0, 3, 0},
{ 3, s_0_1, -1, 2, 0},
{ 4, s_0_2, -2, 1, 0},
{ 2, s_0_3, -3, -1, 0}
};

static const symbol s_1_1[2] = { 'b', 'b' };
static const symbol s_1_2[2] = { 'd', 'd' };
static const symbol s_1_3[2] = { 'f', 'f' };
static const symbol s_1_4[2] = { 'g', 'g' };
static const symbol s_1_5[2] = { 'b', 'l' };
static const symbol s_1_6[2] = { 'm', 'm' };
static const symbol s_1_7[2] = { 'n', 'n' };
static const symbol s_1_8[2] = { 'p', 'p' };
static const symbol s_1_9[2] = { 'r', 'r' };
static const symbol s_1_10[2] = { 'a', 't' };
static const symbol s_1_11[2] = { 't', 't' };
static const symbol s_1_12[2] = { 'i', 'z' };
static const struct among a_1[13] = {
{ 0, 0, 0, 3, 0},
{ 2, s_1_1, -1, 2, 0},
{ 2, s_1_2, -2, 2, 0},
{ 2, s_1_3, -3, 2, 0},
{ 2, s_1_4, -4, 2, 0},
{ 2, s_1_5, -5, 1, 0},
{ 2, s_1_6, -6, 2, 0},
{ 2, s_1_7, -7, 2, 0},
{ 2, s_1_8, -8, 2, 0},
{ 2, s_1_9, -9, 2, 0},
{ 2, s_1_10, -10, 1, 0},
{ 2, s_1_11, -11, 2, 0},
{ 2, s_1_12, -12, 1, 0}
};

static const symbol s_2_0[2] = { 'e', 'd' };
static const symbol s_2_1[3] = { 'e', 'e', 'd' };
static const symbol s_2_2[3] = { 'i', 'n', 'g' };
static const struct among a_2[3] = {
{ 2, s_2_0, 0, 2, 0},
{ 3, s_2_1, -1, 1, 0},
{ 3, s_2_2, 0, 2, 0}
};

static const symbol s_3_0[4] = { 'a', 'n', 'c', 'i' };
static const symbol s_3_1[4] = { 'e', 'n', 'c', 'i' };
static const symbol s_3_2[4] = { 'a', 'b', 'l', 'i' };
static const symbol s_3_3[3] = { 'e', 'l', 'i' };
static const symbol s_3_4[4] = { 'a', 'l', 'l', 'i' };
static const symbol s_3_5[5] = { 'o', 'u', 's', 'l', 'i' };
static const symbol s_3_6[5] = { 'e', 'n', 't', 'l', 'i' };
static const symbol s_3_7[5] = { 'a', 'l', 'i', 't', 'i' };
static const symbol s_3_8[6] = { 'b', 'i', 'l', 'i', 't', 'i' };
static const symbol s_3_9[5] = { 'i', 'v', 'i', 't', 'i' };
static const symbol s_3_10[6] = { 't', 'i', 'o', 'n', 'a', 'l' };
static const symbol s_3_11[7] = { 'a', 't', 'i', 'o', 'n', 'a', 'l' };
static const symbol s_3_12[5] = { 'a', 'l', 'i', 's', 'm' };
static const symbol s_3_13[5] = { 'a', 't', 'i', 'o', 'n' };
static const symbol s_3_14[7] = { 'i', 'z', 'a', 't', 'i', 'o', 'n' };
static const symbol s_3_15[4] = { 'i', 'z', 'e', 'r' };
static const symbol s_3_16[4] = { 'a', 't', 'o', 'r' };
static const symbol s_3_17[7] = { 'i', 'v', 'e', 'n', 'e', 's', 's' };
static const symbol s_3_18[7] = { 'f', 'u', 'l', 'n', 'e', 's', 's' };
static const symbol s_3_19[7] = { 'o', 'u', 's', 'n', 'e', 's', 's' };
static const struct among a_3[20] = {
{ 4, s_3_0, 0, 3, 0},
{ 4, s_3_1, 0, 2, 0},
{ 4, s_3_2, 0, 4, 0},
{ 3, s_3_3, 0, 6, 0},
{ 4, s_3_4, 0, 9, 0},
{ 5, s_3_5, 0, 11, 0},
{ 5, s_3_6, 0, 5, 0},
{ 5, s_3_7, 0, 9, 0},
{ 6, s_3_8, 0, 13, 0},
{ 5, s_3_9, 0, 12, 0},
{ 6, s_3_10, 0, 1, 0},
{ 7, s_3_11, -1, 8, 0},
{ 5, s_3_12, 0, 9, 0},
{ 5, s_3_13, 0, 8, 0},
{ 7, s_3_14, -1, 7, 0},
{ 4, s_3_15, 0, 7, 0},
{ 4, s_3_16, 0, 8, 0},
{ 7, s_3_17, 0, 12, 0},
{ 7, s_3_18, 0, 10, 0},
{ 7, s_3_19, 0, 11, 0}
};

static const symbol s_4_0[5] = { 'i', 'c', 'a', 't', 'e' };
static const symbol s_4_1[5] = { 'a', 't', 'i', 'v', 'e' };
static const symbol s_4_2[5] = { 'a', 'l', 'i', 'z', 'e' };
static const symbol s_4_3[5] = { 'i', 'c', 'i', 't', 'i' };
static const symbol s_4_4[4] = { 'i', 'c', 'a', 'l' };
static const symbol s_4_5[3] = { 'f', 'u', 'l' };
static const symbol s_4_6[4] = { 'n', 'e', 's', 's' };
static const struct among a_4[7] = {
{ 5, s_4_0, 0, 2, 0},
{ 5, s_4_1, 0, 3, 0},
{ 5, s_4_2, 0, 1, 0},
{ 5, s_4_3, 0, 2, 0},
{ 4, s_4_4, 0, 2, 0},
{ 3, s_4_5, 0, 3, 0},
{ 4, s_4_6, 0, 3, 0}
};

static const symbol s_5_0[2] = { 'i', 'c' };
static const symbol s_5_1[4] = { 'a', 'n', 'c', 'e' };
static const symbol s_5_2[4] = { 'e', 'n', 'c', 'e' };
static const symbol s_5_3[4] = { 'a', 'b', 'l', 'e' };
static const symbol s_5_4[4] = { 'i', 'b', 'l', 'e' };
static const symbol s_5_5[3] = { 'a', 't', 'e' };
static const symbol s_5_6[3] = { 'i', 'v', 'e' };
static const symbol s_5_7[3] = { 'i', 'z', 'e' };
static const symbol s_5_8[3] = { 'i', 't', 'i' };
static const symbol s_5_9[2] = { 'a', 'l' };
static const symbol s_5_10[3] = { 'i', 's', 'm' };
static const symbol s_5_11[3] = { 'i', 'o', 'n' };
static const symbol s_5_12[2] = { 'e', 'r' };
static const symbol s_5_13[3] = { 'o', 'u', 's' };
static const symbol s_5_14[3] = { 'a', 'n', 't' };
static const symbol s_5_15[3] = { 'e', 'n', 't' };
static const symbol s_5_16[4] = { 'm', 'e', 'n', 't' };
static const symbol s_5_17[5] = { 'e', 'm', 'e', 'n', 't' };
static const symbol s_5_18[2] = { 'o', 'u' };
static const struct among a_5[19] = {
{ 2, s_5_0, 0, 1, 0},
{ 4, s_5_1, 0, 1, 0},
{ 4, s_5_2, 0, 1, 0},
{ 4, s_5_3, 0, 1, 0},
{ 4, s_5_4, 0, 1, 0},
{ 3, s_5_5, 0, 1, 0},
{ 3, s_5_6, 0, 1, 0},
{ 3, s_5_7, 0, 1, 0},
{ 3, s_5_8, 0, 1, 0},
{ 2, s_5_9, 0, 1, 0},
{ 3, s_5_10, 0, 1, 0},
{ 3, s_5_11, 0, 2, 0},
{ 2, s_5_12, 0, 1, 0},
{ 3, s_5_13, 0, 1, 0},
{ 3, s_5_14, 0, 1, 0},
{ 3, s_5_15, 0, 1, 0},
{ 4, s_5_16, -1, 1, 0},
{ 5, s_5_17, -1, 1, 0},
{ 2, s_5_18, 0, 1, 0}
};

static const unsigned char g_v[] = { 17, 65, 16, 1 };

static const unsigned char g_v_WXY[] = { 1, 17, 65, 208, 1 };

static int r_shortv(struct SN_env * z) {
    if (out_grouping_b(z, g_v_WXY, 89, 121, 0)) return 0;
    if (in_grouping_b(z, g_v, 97, 121, 0)) return 0;
    return !out_grouping_b(z, g_v, 97, 121, 0);
}

static int r_R1(struct SN_env * z) {
    return ((SN_local *)z)->i_p1 <= z->c;
}

static int r_R2(struct SN_env * z) {
    return ((SN_local *)z)->i_p2 <= z->c;
}

static int r_Step_1a(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    if (z->c <= z->lb || z->p[z->c - 1] != 115) return 0;
    among_var = find_among_b(z, a_0, 4, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    switch (among_var) {
        case 1:
            {
                int ret = slice_from_s(z, 2, s_0);
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
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_Step_1b(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    if (z->c - 1 <= z->lb || (z->p[z->c - 1] != 100 && z->p[z->c - 1] != 103)) return 0;
    among_var = find_among_b(z, a_2, 3, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    switch (among_var) {
        case 1:
            {
                int ret = r_R1(z);
                if (ret <= 0) return ret;
            }
            {
                int ret = slice_from_s(z, 2, s_2);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {
                int v_1 = z->l - z->c;
                {
                    int ret = out_grouping_b(z, g_v, 97, 121, 1);
                    if (ret < 0) return 0;
                    z->c -= ret;
                }
                z->c = z->l - v_1;
            }
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            {
                int v_2 = z->l - z->c;
                if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((68514004 >> (z->p[z->c - 1] & 0x1f)) & 1)) among_var = 3; else
                among_var = find_among_b(z, a_1, 13, 0);
                z->c = z->l - v_2;
            }
            switch (among_var) {
                case 1:
                    {
                        int saved_c = z->c;
                        int ret = insert_s(z, z->c, z->c, 1, s_3);
                        z->c = saved_c;
                        if (ret < 0) return ret;
                    }
                    break;
                case 2:
                    z->ket = z->c;
                    if (z->c <= z->lb) return 0;
                    z->c--;
                    z->bra = z->c;
                    {
                        int ret = slice_del(z);
                        if (ret < 0) return ret;
                    }
                    break;
                case 3:
                    if (z->c != ((SN_local *)z)->i_p1) return 0;
                    {
                        int v_3 = z->l - z->c;
                        {
                            int ret = r_shortv(z);
                            if (ret <= 0) return ret;
                        }
                        z->c = z->l - v_3;
                    }
                    {
                        int saved_c = z->c;
                        int ret = insert_s(z, z->c, z->c, 1, s_4);
                        z->c = saved_c;
                        if (ret < 0) return ret;
                    }
                    break;
            }
            break;
    }
    return 1;
}

static int r_Step_1c(struct SN_env * z) {
    z->ket = z->c;
    do {
        int v_1 = z->l - z->c;
        if (z->c <= z->lb || z->p[z->c - 1] != 'y') goto lab0;
        z->c--;
        break;
    lab0:
        z->c = z->l - v_1;
        if (z->c <= z->lb || z->p[z->c - 1] != 'Y') return 0;
        z->c--;
    } while (0);
    z->bra = z->c;
    {
        int ret = out_grouping_b(z, g_v, 97, 121, 1);
        if (ret < 0) return 0;
        z->c -= ret;
    }
    {
        int ret = slice_from_s(z, 1, s_5);
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_Step_2(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    if (z->c - 2 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((815616 >> (z->p[z->c - 1] & 0x1f)) & 1)) return 0;
    among_var = find_among_b(z, a_3, 20, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    {
        int ret = r_R1(z);
        if (ret <= 0) return ret;
    }
    switch (among_var) {
        case 1:
            {
                int ret = slice_from_s(z, 4, s_6);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {
                int ret = slice_from_s(z, 4, s_7);
                if (ret < 0) return ret;
            }
            break;
        case 3:
            {
                int ret = slice_from_s(z, 4, s_8);
                if (ret < 0) return ret;
            }
            break;
        case 4:
            {
                int ret = slice_from_s(z, 4, s_9);
                if (ret < 0) return ret;
            }
            break;
        case 5:
            {
                int ret = slice_from_s(z, 3, s_10);
                if (ret < 0) return ret;
            }
            break;
        case 6:
            {
                int ret = slice_from_s(z, 1, s_11);
                if (ret < 0) return ret;
            }
            break;
        case 7:
            {
                int ret = slice_from_s(z, 3, s_12);
                if (ret < 0) return ret;
            }
            break;
        case 8:
            {
                int ret = slice_from_s(z, 3, s_13);
                if (ret < 0) return ret;
            }
            break;
        case 9:
            {
                int ret = slice_from_s(z, 2, s_14);
                if (ret < 0) return ret;
            }
            break;
        case 10:
            {
                int ret = slice_from_s(z, 3, s_15);
                if (ret < 0) return ret;
            }
            break;
        case 11:
            {
                int ret = slice_from_s(z, 3, s_16);
                if (ret < 0) return ret;
            }
            break;
        case 12:
            {
                int ret = slice_from_s(z, 3, s_17);
                if (ret < 0) return ret;
            }
            break;
        case 13:
            {
                int ret = slice_from_s(z, 3, s_18);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_Step_3(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    if (z->c - 2 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((528928 >> (z->p[z->c - 1] & 0x1f)) & 1)) return 0;
    among_var = find_among_b(z, a_4, 7, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    {
        int ret = r_R1(z);
        if (ret <= 0) return ret;
    }
    switch (among_var) {
        case 1:
            {
                int ret = slice_from_s(z, 2, s_19);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {
                int ret = slice_from_s(z, 2, s_20);
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

static int r_Step_4(struct SN_env * z) {
    int among_var;
    z->ket = z->c;
    if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((3961384 >> (z->p[z->c - 1] & 0x1f)) & 1)) return 0;
    among_var = find_among_b(z, a_5, 19, 0);
    if (!among_var) return 0;
    z->bra = z->c;
    {
        int ret = r_R2(z);
        if (ret <= 0) return ret;
    }
    switch (among_var) {
        case 1:
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 2:
            do {
                int v_1 = z->l - z->c;
                if (z->c <= z->lb || z->p[z->c - 1] != 's') goto lab0;
                z->c--;
                break;
            lab0:
                z->c = z->l - v_1;
                if (z->c <= z->lb || z->p[z->c - 1] != 't') return 0;
                z->c--;
            } while (0);
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_Step_5a(struct SN_env * z) {
    z->ket = z->c;
    if (z->c <= z->lb || z->p[z->c - 1] != 'e') return 0;
    z->c--;
    z->bra = z->c;
    do {
        {
            int ret = r_R2(z);
            if (ret == 0) goto lab0;
            if (ret < 0) return ret;
        }
        break;
    lab0:
        {
            int ret = r_R1(z);
            if (ret <= 0) return ret;
        }
        {
            int v_1 = z->l - z->c;
            {
                int ret = r_shortv(z);
                if (ret == 0) goto lab1;
                if (ret < 0) return ret;
            }
            return 0;
        lab1:
            z->c = z->l - v_1;
        }
    } while (0);
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_Step_5b(struct SN_env * z) {
    z->ket = z->c;
    if (z->c <= z->lb || z->p[z->c - 1] != 'l') return 0;
    z->c--;
    z->bra = z->c;
    {
        int ret = r_R2(z);
        if (ret <= 0) return ret;
    }
    if (z->c <= z->lb || z->p[z->c - 1] != 'l') return 0;
    z->c--;
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return 1;
}

extern int porter_ISO_8859_1_stem(struct SN_env * z) {
    int b_Y_found;
    b_Y_found = 0;
    {
        int v_1 = z->c;
        z->bra = z->c;
        if (z->c == z->l || z->p[z->c] != 'y') goto lab0;
        z->c++;
        z->ket = z->c;
        {
            int ret = slice_from_s(z, 1, s_21);
            if (ret < 0) return ret;
        }
        b_Y_found = 1;
    lab0:
        z->c = v_1;
    }
    {
        int v_2 = z->c;
        while (1) {
            int v_3 = z->c;
            while (1) {
                int v_4 = z->c;
                if (in_grouping(z, g_v, 97, 121, 0)) goto lab3;
                z->bra = z->c;
                if (z->c == z->l || z->p[z->c] != 'y') goto lab3;
                z->c++;
                z->ket = z->c;
                z->c = v_4;
                break;
            lab3:
                z->c = v_4;
                if (z->c >= z->l) goto lab2;
                z->c++;
            }
            {
                int ret = slice_from_s(z, 1, s_22);
                if (ret < 0) return ret;
            }
            b_Y_found = 1;
            continue;
        lab2:
            z->c = v_3;
            break;
        }
        z->c = v_2;
    }
    ((SN_local *)z)->i_p1 = z->l;
    ((SN_local *)z)->i_p2 = z->l;
    {
        int v_5 = z->c;
        {
            int ret = out_grouping(z, g_v, 97, 121, 1);
            if (ret < 0) goto lab4;
            z->c += ret;
        }
        {
            int ret = in_grouping(z, g_v, 97, 121, 1);
            if (ret < 0) goto lab4;
            z->c += ret;
        }
        ((SN_local *)z)->i_p1 = z->c;
        {
            int ret = out_grouping(z, g_v, 97, 121, 1);
            if (ret < 0) goto lab4;
            z->c += ret;
        }
        {
            int ret = in_grouping(z, g_v, 97, 121, 1);
            if (ret < 0) goto lab4;
            z->c += ret;
        }
        ((SN_local *)z)->i_p2 = z->c;
    lab4:
        z->c = v_5;
    }
    z->lb = z->c; z->c = z->l;
    {
        int v_6 = z->l - z->c;
        {
            int ret = r_Step_1a(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_6;
    }
    {
        int v_7 = z->l - z->c;
        {
            int ret = r_Step_1b(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_7;
    }
    {
        int v_8 = z->l - z->c;
        {
            int ret = r_Step_1c(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_8;
    }
    {
        int v_9 = z->l - z->c;
        {
            int ret = r_Step_2(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_9;
    }
    {
        int v_10 = z->l - z->c;
        {
            int ret = r_Step_3(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_10;
    }
    {
        int v_11 = z->l - z->c;
        {
            int ret = r_Step_4(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_11;
    }
    {
        int v_12 = z->l - z->c;
        {
            int ret = r_Step_5a(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_12;
    }
    {
        int v_13 = z->l - z->c;
        {
            int ret = r_Step_5b(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_13;
    }
    z->c = z->lb;
    {
        int v_14 = z->c;
        if (!b_Y_found) goto lab5;
        while (1) {
            int v_15 = z->c;
            while (1) {
                int v_16 = z->c;
                z->bra = z->c;
                if (z->c == z->l || z->p[z->c] != 'Y') goto lab7;
                z->c++;
                z->ket = z->c;
                z->c = v_16;
                break;
            lab7:
                z->c = v_16;
                if (z->c >= z->l) goto lab6;
                z->c++;
            }
            {
                int ret = slice_from_s(z, 1, s_23);
                if (ret < 0) return ret;
            }
            continue;
        lab6:
            z->c = v_15;
            break;
        }
    lab5:
        z->c = v_14;
    }
    return 1;
}

extern struct SN_env * porter_ISO_8859_1_create_env(void) {
    struct SN_env * z = SN_new_env(sizeof(SN_local));
    if (z) {
        ((SN_local *)z)->i_p2 = 0;
        ((SN_local *)z)->i_p1 = 0;
    }
    return z;
}

extern void porter_ISO_8859_1_close_env(struct SN_env * z) {
    SN_delete_env(z);
}

