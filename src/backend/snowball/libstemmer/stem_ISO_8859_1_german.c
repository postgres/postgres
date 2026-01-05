/* Generated from german.sbl by Snowball 3.0.0 - https://snowballstem.org/ */

#include "stem_ISO_8859_1_german.h"

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
extern int german_ISO_8859_1_stem(struct SN_env * z);
#ifdef __cplusplus
}
#endif

static int r_standard_suffix(struct SN_env * z);
static int r_R2(struct SN_env * z);
static int r_R1(struct SN_env * z);
static int r_mark_regions(struct SN_env * z);
static int r_postlude(struct SN_env * z);
static int r_prelude(struct SN_env * z);

static const symbol s_0[] = { 'U' };
static const symbol s_1[] = { 'Y' };
static const symbol s_2[] = { 's', 's' };
static const symbol s_3[] = { 0xE4 };
static const symbol s_4[] = { 0xF6 };
static const symbol s_5[] = { 0xFC };
static const symbol s_6[] = { 'y' };
static const symbol s_7[] = { 'u' };
static const symbol s_8[] = { 'a' };
static const symbol s_9[] = { 'o' };
static const symbol s_10[] = { 's', 'y', 's', 't' };
static const symbol s_11[] = { 'n', 'i', 's' };
static const symbol s_12[] = { 'l' };
static const symbol s_13[] = { 'i', 'g' };
static const symbol s_14[] = { 'e', 'r' };
static const symbol s_15[] = { 'e', 'n' };

static const symbol s_0_1[2] = { 'a', 'e' };
static const symbol s_0_2[2] = { 'o', 'e' };
static const symbol s_0_3[2] = { 'q', 'u' };
static const symbol s_0_4[2] = { 'u', 'e' };
static const symbol s_0_5[1] = { 0xDF };
static const struct among a_0[6] = {
{ 0, 0, 0, 5, 0},
{ 2, s_0_1, -1, 2, 0},
{ 2, s_0_2, -2, 3, 0},
{ 2, s_0_3, -3, -1, 0},
{ 2, s_0_4, -4, 4, 0},
{ 1, s_0_5, -5, 1, 0}
};

static const symbol s_1_1[1] = { 'U' };
static const symbol s_1_2[1] = { 'Y' };
static const symbol s_1_3[1] = { 0xE4 };
static const symbol s_1_4[1] = { 0xF6 };
static const symbol s_1_5[1] = { 0xFC };
static const struct among a_1[6] = {
{ 0, 0, 0, 5, 0},
{ 1, s_1_1, -1, 2, 0},
{ 1, s_1_2, -2, 1, 0},
{ 1, s_1_3, -3, 3, 0},
{ 1, s_1_4, -4, 4, 0},
{ 1, s_1_5, -5, 2, 0}
};

static const symbol s_2_0[1] = { 'e' };
static const symbol s_2_1[2] = { 'e', 'm' };
static const symbol s_2_2[2] = { 'e', 'n' };
static const symbol s_2_3[7] = { 'e', 'r', 'i', 'n', 'n', 'e', 'n' };
static const symbol s_2_4[4] = { 'e', 'r', 'i', 'n' };
static const symbol s_2_5[2] = { 'l', 'n' };
static const symbol s_2_6[3] = { 'e', 'r', 'n' };
static const symbol s_2_7[2] = { 'e', 'r' };
static const symbol s_2_8[1] = { 's' };
static const symbol s_2_9[2] = { 'e', 's' };
static const symbol s_2_10[3] = { 'l', 'n', 's' };
static const struct among a_2[11] = {
{ 1, s_2_0, 0, 3, 0},
{ 2, s_2_1, 0, 1, 0},
{ 2, s_2_2, 0, 3, 0},
{ 7, s_2_3, -1, 2, 0},
{ 4, s_2_4, 0, 2, 0},
{ 2, s_2_5, 0, 5, 0},
{ 3, s_2_6, 0, 2, 0},
{ 2, s_2_7, 0, 2, 0},
{ 1, s_2_8, 0, 4, 0},
{ 2, s_2_9, -1, 3, 0},
{ 3, s_2_10, -2, 5, 0}
};

static const symbol s_3_0[4] = { 't', 'i', 'c', 'k' };
static const symbol s_3_1[4] = { 'p', 'l', 'a', 'n' };
static const symbol s_3_2[6] = { 'g', 'e', 'o', 'r', 'd', 'n' };
static const symbol s_3_3[6] = { 'i', 'n', 't', 'e', 'r', 'n' };
static const symbol s_3_4[2] = { 't', 'r' };
static const struct among a_3[5] = {
{ 4, s_3_0, 0, -1, 0},
{ 4, s_3_1, 0, -1, 0},
{ 6, s_3_2, 0, -1, 0},
{ 6, s_3_3, 0, -1, 0},
{ 2, s_3_4, 0, -1, 0}
};

static const symbol s_4_0[2] = { 'e', 'n' };
static const symbol s_4_1[2] = { 'e', 'r' };
static const symbol s_4_2[2] = { 'e', 't' };
static const symbol s_4_3[2] = { 's', 't' };
static const symbol s_4_4[3] = { 'e', 's', 't' };
static const struct among a_4[5] = {
{ 2, s_4_0, 0, 1, 0},
{ 2, s_4_1, 0, 1, 0},
{ 2, s_4_2, 0, 3, 0},
{ 2, s_4_3, 0, 2, 0},
{ 3, s_4_4, -1, 1, 0}
};

static const symbol s_5_0[2] = { 'i', 'g' };
static const symbol s_5_1[4] = { 'l', 'i', 'c', 'h' };
static const struct among a_5[2] = {
{ 2, s_5_0, 0, 1, 0},
{ 4, s_5_1, 0, 1, 0}
};

static const symbol s_6_0[3] = { 'e', 'n', 'd' };
static const symbol s_6_1[2] = { 'i', 'g' };
static const symbol s_6_2[3] = { 'u', 'n', 'g' };
static const symbol s_6_3[4] = { 'l', 'i', 'c', 'h' };
static const symbol s_6_4[4] = { 'i', 's', 'c', 'h' };
static const symbol s_6_5[2] = { 'i', 'k' };
static const symbol s_6_6[4] = { 'h', 'e', 'i', 't' };
static const symbol s_6_7[4] = { 'k', 'e', 'i', 't' };
static const struct among a_6[8] = {
{ 3, s_6_0, 0, 1, 0},
{ 2, s_6_1, 0, 2, 0},
{ 3, s_6_2, 0, 1, 0},
{ 4, s_6_3, 0, 3, 0},
{ 4, s_6_4, 0, 2, 0},
{ 2, s_6_5, 0, 2, 0},
{ 4, s_6_6, 0, 3, 0},
{ 4, s_6_7, 0, 4, 0}
};

static const unsigned char g_v[] = { 17, 65, 16, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 32, 8 };

static const unsigned char g_et_ending[] = { 1, 128, 198, 227, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 128 };

static const unsigned char g_s_ending[] = { 117, 30, 5 };

static const unsigned char g_st_ending[] = { 117, 30, 4 };

static int r_prelude(struct SN_env * z) {
    int among_var;
    {
        int v_1 = z->c;
        while (1) {
            int v_2 = z->c;
            while (1) {
                int v_3 = z->c;
                if (in_grouping(z, g_v, 97, 252, 0)) goto lab1;
                z->bra = z->c;
                do {
                    int v_4 = z->c;
                    if (z->c == z->l || z->p[z->c] != 'u') goto lab2;
                    z->c++;
                    z->ket = z->c;
                    if (in_grouping(z, g_v, 97, 252, 0)) goto lab2;
                    {
                        int ret = slice_from_s(z, 1, s_0);
                        if (ret < 0) return ret;
                    }
                    break;
                lab2:
                    z->c = v_4;
                    if (z->c == z->l || z->p[z->c] != 'y') goto lab1;
                    z->c++;
                    z->ket = z->c;
                    if (in_grouping(z, g_v, 97, 252, 0)) goto lab1;
                    {
                        int ret = slice_from_s(z, 1, s_1);
                        if (ret < 0) return ret;
                    }
                } while (0);
                z->c = v_3;
                break;
            lab1:
                z->c = v_3;
                if (z->c >= z->l) goto lab0;
                z->c++;
            }
            continue;
        lab0:
            z->c = v_2;
            break;
        }
        z->c = v_1;
    }
    while (1) {
        int v_5 = z->c;
        z->bra = z->c;
        among_var = find_among(z, a_0, 6, 0);
        z->ket = z->c;
        switch (among_var) {
            case 1:
                {
                    int ret = slice_from_s(z, 2, s_2);
                    if (ret < 0) return ret;
                }
                break;
            case 2:
                {
                    int ret = slice_from_s(z, 1, s_3);
                    if (ret < 0) return ret;
                }
                break;
            case 3:
                {
                    int ret = slice_from_s(z, 1, s_4);
                    if (ret < 0) return ret;
                }
                break;
            case 4:
                {
                    int ret = slice_from_s(z, 1, s_5);
                    if (ret < 0) return ret;
                }
                break;
            case 5:
                if (z->c >= z->l) goto lab3;
                z->c++;
                break;
        }
        continue;
    lab3:
        z->c = v_5;
        break;
    }
    return 1;
}

static int r_mark_regions(struct SN_env * z) {
    int i_x;
    ((SN_local *)z)->i_p1 = z->l;
    ((SN_local *)z)->i_p2 = z->l;
    {
        int v_1 = z->c;
        if (z->c + 3 > z->l) return 0;
        z->c += 3;
        i_x = z->c;
        z->c = v_1;
    }
    {
        int ret = out_grouping(z, g_v, 97, 252, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    {
        int ret = in_grouping(z, g_v, 97, 252, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    ((SN_local *)z)->i_p1 = z->c;
    if (((SN_local *)z)->i_p1 >= i_x) goto lab0;
    ((SN_local *)z)->i_p1 = i_x;
lab0:
    {
        int ret = out_grouping(z, g_v, 97, 252, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    {
        int ret = in_grouping(z, g_v, 97, 252, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    ((SN_local *)z)->i_p2 = z->c;
    return 1;
}

static int r_postlude(struct SN_env * z) {
    int among_var;
    while (1) {
        int v_1 = z->c;
        z->bra = z->c;
        among_var = find_among(z, a_1, 6, 0);
        z->ket = z->c;
        switch (among_var) {
            case 1:
                {
                    int ret = slice_from_s(z, 1, s_6);
                    if (ret < 0) return ret;
                }
                break;
            case 2:
                {
                    int ret = slice_from_s(z, 1, s_7);
                    if (ret < 0) return ret;
                }
                break;
            case 3:
                {
                    int ret = slice_from_s(z, 1, s_8);
                    if (ret < 0) return ret;
                }
                break;
            case 4:
                {
                    int ret = slice_from_s(z, 1, s_9);
                    if (ret < 0) return ret;
                }
                break;
            case 5:
                if (z->c >= z->l) goto lab0;
                z->c++;
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

static int r_standard_suffix(struct SN_env * z) {
    int among_var;
    {
        int v_1 = z->l - z->c;
        z->ket = z->c;
        if (z->c <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((811040 >> (z->p[z->c - 1] & 0x1f)) & 1)) goto lab0;
        among_var = find_among_b(z, a_2, 11, 0);
        if (!among_var) goto lab0;
        z->bra = z->c;
        {
            int ret = r_R1(z);
            if (ret == 0) goto lab0;
            if (ret < 0) return ret;
        }
        switch (among_var) {
            case 1:
                {
                    int v_2 = z->l - z->c;
                    if (!(eq_s_b(z, 4, s_10))) goto lab1;
                    goto lab0;
                lab1:
                    z->c = z->l - v_2;
                }
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                break;
            case 2:
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                break;
            case 3:
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                {
                    int v_3 = z->l - z->c;
                    z->ket = z->c;
                    if (z->c <= z->lb || z->p[z->c - 1] != 's') { z->c = z->l - v_3; goto lab2; }
                    z->c--;
                    z->bra = z->c;
                    if (!(eq_s_b(z, 3, s_11))) { z->c = z->l - v_3; goto lab2; }
                    {
                        int ret = slice_del(z);
                        if (ret < 0) return ret;
                    }
                lab2:
                    ;
                }
                break;
            case 4:
                if (in_grouping_b(z, g_s_ending, 98, 116, 0)) goto lab0;
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                break;
            case 5:
                {
                    int ret = slice_from_s(z, 1, s_12);
                    if (ret < 0) return ret;
                }
                break;
        }
    lab0:
        z->c = z->l - v_1;
    }
    {
        int v_4 = z->l - z->c;
        z->ket = z->c;
        if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((1327104 >> (z->p[z->c - 1] & 0x1f)) & 1)) goto lab3;
        among_var = find_among_b(z, a_4, 5, 0);
        if (!among_var) goto lab3;
        z->bra = z->c;
        {
            int ret = r_R1(z);
            if (ret == 0) goto lab3;
            if (ret < 0) return ret;
        }
        switch (among_var) {
            case 1:
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                break;
            case 2:
                if (in_grouping_b(z, g_st_ending, 98, 116, 0)) goto lab3;
                if (z->c - 3 < z->lb) goto lab3;
                z->c -= 3;
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                break;
            case 3:
                {
                    int v_5 = z->l - z->c;
                    if (in_grouping_b(z, g_et_ending, 85, 228, 0)) goto lab3;
                    z->c = z->l - v_5;
                }
                {
                    int v_6 = z->l - z->c;
                    if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((280576 >> (z->p[z->c - 1] & 0x1f)) & 1)) goto lab4;
                    if (!find_among_b(z, a_3, 5, 0)) goto lab4;
                    goto lab3;
                lab4:
                    z->c = z->l - v_6;
                }
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                break;
        }
    lab3:
        z->c = z->l - v_4;
    }
    {
        int v_7 = z->l - z->c;
        z->ket = z->c;
        if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((1051024 >> (z->p[z->c - 1] & 0x1f)) & 1)) goto lab5;
        among_var = find_among_b(z, a_6, 8, 0);
        if (!among_var) goto lab5;
        z->bra = z->c;
        {
            int ret = r_R2(z);
            if (ret == 0) goto lab5;
            if (ret < 0) return ret;
        }
        switch (among_var) {
            case 1:
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                {
                    int v_8 = z->l - z->c;
                    z->ket = z->c;
                    if (!(eq_s_b(z, 2, s_13))) { z->c = z->l - v_8; goto lab6; }
                    z->bra = z->c;
                    {
                        int v_9 = z->l - z->c;
                        if (z->c <= z->lb || z->p[z->c - 1] != 'e') goto lab7;
                        z->c--;
                        { z->c = z->l - v_8; goto lab6; }
                    lab7:
                        z->c = z->l - v_9;
                    }
                    {
                        int ret = r_R2(z);
                        if (ret == 0) { z->c = z->l - v_8; goto lab6; }
                        if (ret < 0) return ret;
                    }
                    {
                        int ret = slice_del(z);
                        if (ret < 0) return ret;
                    }
                lab6:
                    ;
                }
                break;
            case 2:
                {
                    int v_10 = z->l - z->c;
                    if (z->c <= z->lb || z->p[z->c - 1] != 'e') goto lab8;
                    z->c--;
                    goto lab5;
                lab8:
                    z->c = z->l - v_10;
                }
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                break;
            case 3:
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                {
                    int v_11 = z->l - z->c;
                    z->ket = z->c;
                    do {
                        int v_12 = z->l - z->c;
                        if (!(eq_s_b(z, 2, s_14))) goto lab10;
                        break;
                    lab10:
                        z->c = z->l - v_12;
                        if (!(eq_s_b(z, 2, s_15))) { z->c = z->l - v_11; goto lab9; }
                    } while (0);
                    z->bra = z->c;
                    {
                        int ret = r_R1(z);
                        if (ret == 0) { z->c = z->l - v_11; goto lab9; }
                        if (ret < 0) return ret;
                    }
                    {
                        int ret = slice_del(z);
                        if (ret < 0) return ret;
                    }
                lab9:
                    ;
                }
                break;
            case 4:
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                {
                    int v_13 = z->l - z->c;
                    z->ket = z->c;
                    if (z->c - 1 <= z->lb || (z->p[z->c - 1] != 103 && z->p[z->c - 1] != 104)) { z->c = z->l - v_13; goto lab11; }
                    if (!find_among_b(z, a_5, 2, 0)) { z->c = z->l - v_13; goto lab11; }
                    z->bra = z->c;
                    {
                        int ret = r_R2(z);
                        if (ret == 0) { z->c = z->l - v_13; goto lab11; }
                        if (ret < 0) return ret;
                    }
                    {
                        int ret = slice_del(z);
                        if (ret < 0) return ret;
                    }
                lab11:
                    ;
                }
                break;
        }
    lab5:
        z->c = z->l - v_7;
    }
    return 1;
}

extern int german_ISO_8859_1_stem(struct SN_env * z) {
    {
        int v_1 = z->c;
        {
            int ret = r_prelude(z);
            if (ret < 0) return ret;
        }
        z->c = v_1;
    }
    {
        int v_2 = z->c;
        {
            int ret = r_mark_regions(z);
            if (ret < 0) return ret;
        }
        z->c = v_2;
    }
    z->lb = z->c; z->c = z->l;
    {
        int ret = r_standard_suffix(z);
        if (ret < 0) return ret;
    }
    z->c = z->lb;
    {
        int v_3 = z->c;
        {
            int ret = r_postlude(z);
            if (ret < 0) return ret;
        }
        z->c = v_3;
    }
    return 1;
}

extern struct SN_env * german_ISO_8859_1_create_env(void) {
    struct SN_env * z = SN_new_env(sizeof(SN_local));
    if (z) {
        ((SN_local *)z)->i_p2 = 0;
        ((SN_local *)z)->i_p1 = 0;
    }
    return z;
}

extern void german_ISO_8859_1_close_env(struct SN_env * z) {
    SN_delete_env(z);
}

