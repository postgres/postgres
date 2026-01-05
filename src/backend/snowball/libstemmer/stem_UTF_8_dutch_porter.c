/* Generated from dutch_porter.sbl by Snowball 3.0.0 - https://snowballstem.org/ */

#include "stem_UTF_8_dutch_porter.h"

#include <stddef.h>

#include "snowball_runtime.h"

struct SN_local {
    struct SN_env z;
    int i_p2;
    int i_p1;
    unsigned char b_e_found;
};

typedef struct SN_local SN_local;

#ifdef __cplusplus
extern "C" {
#endif
extern int dutch_porter_UTF_8_stem(struct SN_env * z);
#ifdef __cplusplus
}
#endif

static int r_standard_suffix(struct SN_env * z);
static int r_undouble(struct SN_env * z);
static int r_R2(struct SN_env * z);
static int r_R1(struct SN_env * z);
static int r_mark_regions(struct SN_env * z);
static int r_en_ending(struct SN_env * z);
static int r_e_ending(struct SN_env * z);
static int r_postlude(struct SN_env * z);
static int r_prelude(struct SN_env * z);

static const symbol s_0[] = { 'a' };
static const symbol s_1[] = { 'e' };
static const symbol s_2[] = { 'i' };
static const symbol s_3[] = { 'o' };
static const symbol s_4[] = { 'u' };
static const symbol s_5[] = { 'Y' };
static const symbol s_6[] = { 'I' };
static const symbol s_7[] = { 'Y' };
static const symbol s_8[] = { 'y' };
static const symbol s_9[] = { 'i' };
static const symbol s_10[] = { 'g', 'e', 'm' };
static const symbol s_11[] = { 'h', 'e', 'i', 'd' };
static const symbol s_12[] = { 'h', 'e', 'i', 'd' };
static const symbol s_13[] = { 'e', 'n' };
static const symbol s_14[] = { 'i', 'g' };

static const symbol s_0_1[2] = { 0xC3, 0xA1 };
static const symbol s_0_2[2] = { 0xC3, 0xA4 };
static const symbol s_0_3[2] = { 0xC3, 0xA9 };
static const symbol s_0_4[2] = { 0xC3, 0xAB };
static const symbol s_0_5[2] = { 0xC3, 0xAD };
static const symbol s_0_6[2] = { 0xC3, 0xAF };
static const symbol s_0_7[2] = { 0xC3, 0xB3 };
static const symbol s_0_8[2] = { 0xC3, 0xB6 };
static const symbol s_0_9[2] = { 0xC3, 0xBA };
static const symbol s_0_10[2] = { 0xC3, 0xBC };
static const struct among a_0[11] = {
{ 0, 0, 0, 6, 0},
{ 2, s_0_1, -1, 1, 0},
{ 2, s_0_2, -2, 1, 0},
{ 2, s_0_3, -3, 2, 0},
{ 2, s_0_4, -4, 2, 0},
{ 2, s_0_5, -5, 3, 0},
{ 2, s_0_6, -6, 3, 0},
{ 2, s_0_7, -7, 4, 0},
{ 2, s_0_8, -8, 4, 0},
{ 2, s_0_9, -9, 5, 0},
{ 2, s_0_10, -10, 5, 0}
};

static const symbol s_1_1[1] = { 'I' };
static const symbol s_1_2[1] = { 'Y' };
static const struct among a_1[3] = {
{ 0, 0, 0, 3, 0},
{ 1, s_1_1, -1, 2, 0},
{ 1, s_1_2, -2, 1, 0}
};

static const symbol s_2_0[2] = { 'd', 'd' };
static const symbol s_2_1[2] = { 'k', 'k' };
static const symbol s_2_2[2] = { 't', 't' };
static const struct among a_2[3] = {
{ 2, s_2_0, 0, -1, 0},
{ 2, s_2_1, 0, -1, 0},
{ 2, s_2_2, 0, -1, 0}
};

static const symbol s_3_0[3] = { 'e', 'n', 'e' };
static const symbol s_3_1[2] = { 's', 'e' };
static const symbol s_3_2[2] = { 'e', 'n' };
static const symbol s_3_3[5] = { 'h', 'e', 'd', 'e', 'n' };
static const symbol s_3_4[1] = { 's' };
static const struct among a_3[5] = {
{ 3, s_3_0, 0, 2, 0},
{ 2, s_3_1, 0, 3, 0},
{ 2, s_3_2, 0, 2, 0},
{ 5, s_3_3, -1, 1, 0},
{ 1, s_3_4, 0, 3, 0}
};

static const symbol s_4_0[3] = { 'e', 'n', 'd' };
static const symbol s_4_1[2] = { 'i', 'g' };
static const symbol s_4_2[3] = { 'i', 'n', 'g' };
static const symbol s_4_3[4] = { 'l', 'i', 'j', 'k' };
static const symbol s_4_4[4] = { 'b', 'a', 'a', 'r' };
static const symbol s_4_5[3] = { 'b', 'a', 'r' };
static const struct among a_4[6] = {
{ 3, s_4_0, 0, 1, 0},
{ 2, s_4_1, 0, 2, 0},
{ 3, s_4_2, 0, 1, 0},
{ 4, s_4_3, 0, 3, 0},
{ 4, s_4_4, 0, 4, 0},
{ 3, s_4_5, 0, 5, 0}
};

static const symbol s_5_0[2] = { 'a', 'a' };
static const symbol s_5_1[2] = { 'e', 'e' };
static const symbol s_5_2[2] = { 'o', 'o' };
static const symbol s_5_3[2] = { 'u', 'u' };
static const struct among a_5[4] = {
{ 2, s_5_0, 0, -1, 0},
{ 2, s_5_1, 0, -1, 0},
{ 2, s_5_2, 0, -1, 0},
{ 2, s_5_3, 0, -1, 0}
};

static const unsigned char g_v[] = { 17, 65, 16, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 128 };

static const unsigned char g_v_I[] = { 1, 0, 0, 17, 65, 16, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 128 };

static const unsigned char g_v_j[] = { 17, 67, 16, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 128 };

static int r_prelude(struct SN_env * z) {
    int among_var;
    {
        int v_1 = z->c;
        while (1) {
            int v_2 = z->c;
            z->bra = z->c;
            if (z->c + 1 >= z->l || z->p[z->c + 1] >> 5 != 5 || !((340306450 >> (z->p[z->c + 1] & 0x1f)) & 1)) among_var = 6; else
            among_var = find_among(z, a_0, 11, 0);
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
                        int ret = skip_utf8(z->p, z->c, z->l, 1);
                        if (ret < 0) goto lab0;
                        z->c = ret;
                    }
                    break;
            }
            continue;
        lab0:
            z->c = v_2;
            break;
        }
        z->c = v_1;
    }
    {
        int v_3 = z->c;
        z->bra = z->c;
        if (z->c == z->l || z->p[z->c] != 'y') { z->c = v_3; goto lab1; }
        z->c++;
        z->ket = z->c;
        {
            int ret = slice_from_s(z, 1, s_5);
            if (ret < 0) return ret;
        }
    lab1:
        ;
    }
    while (1) {
        int v_4 = z->c;
        {
            int ret = out_grouping_U(z, g_v, 97, 232, 1);
            if (ret < 0) goto lab2;
            z->c += ret;
        }
        {
            int v_5 = z->c;
            z->bra = z->c;
            do {
                int v_6 = z->c;
                if (z->c == z->l || z->p[z->c] != 'i') goto lab4;
                z->c++;
                z->ket = z->c;
                {
                    int v_7 = z->c;
                    if (in_grouping_U(z, g_v, 97, 232, 0)) goto lab5;
                    {
                        int ret = slice_from_s(z, 1, s_6);
                        if (ret < 0) return ret;
                    }
                lab5:
                    z->c = v_7;
                }
                break;
            lab4:
                z->c = v_6;
                if (z->c == z->l || z->p[z->c] != 'y') { z->c = v_5; goto lab3; }
                z->c++;
                z->ket = z->c;
                {
                    int ret = slice_from_s(z, 1, s_7);
                    if (ret < 0) return ret;
                }
            } while (0);
        lab3:
            ;
        }
        continue;
    lab2:
        z->c = v_4;
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
        {
            int ret = skip_utf8(z->p, z->c, z->l, 3);
            if (ret < 0) return 0;
            z->c = ret;
        }
        i_x = z->c;
        z->c = v_1;
    }
    {
        int ret = out_grouping_U(z, g_v, 97, 232, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    {
        int ret = in_grouping_U(z, g_v, 97, 232, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    ((SN_local *)z)->i_p1 = z->c;
    if (((SN_local *)z)->i_p1 >= i_x) goto lab0;
    ((SN_local *)z)->i_p1 = i_x;
lab0:
    {
        int ret = out_grouping_U(z, g_v, 97, 232, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    {
        int ret = in_grouping_U(z, g_v, 97, 232, 1);
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
        if (z->c >= z->l || (z->p[z->c + 0] != 73 && z->p[z->c + 0] != 89)) among_var = 3; else
        among_var = find_among(z, a_1, 3, 0);
        z->ket = z->c;
        switch (among_var) {
            case 1:
                {
                    int ret = slice_from_s(z, 1, s_8);
                    if (ret < 0) return ret;
                }
                break;
            case 2:
                {
                    int ret = slice_from_s(z, 1, s_9);
                    if (ret < 0) return ret;
                }
                break;
            case 3:
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

static int r_undouble(struct SN_env * z) {
    {
        int v_1 = z->l - z->c;
        if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((1050640 >> (z->p[z->c - 1] & 0x1f)) & 1)) return 0;
        if (!find_among_b(z, a_2, 3, 0)) return 0;
        z->c = z->l - v_1;
    }
    z->ket = z->c;
    {
        int ret = skip_b_utf8(z->p, z->c, z->lb, 1);
        if (ret < 0) return 0;
        z->c = ret;
    }
    z->bra = z->c;
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_e_ending(struct SN_env * z) {
    ((SN_local *)z)->b_e_found = 0;
    z->ket = z->c;
    if (z->c <= z->lb || z->p[z->c - 1] != 'e') return 0;
    z->c--;
    z->bra = z->c;
    {
        int ret = r_R1(z);
        if (ret <= 0) return ret;
    }
    {
        int v_1 = z->l - z->c;
        if (out_grouping_b_U(z, g_v, 97, 232, 0)) return 0;
        z->c = z->l - v_1;
    }
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    ((SN_local *)z)->b_e_found = 1;
    return r_undouble(z);
}

static int r_en_ending(struct SN_env * z) {
    {
        int ret = r_R1(z);
        if (ret <= 0) return ret;
    }
    {
        int v_1 = z->l - z->c;
        if (out_grouping_b_U(z, g_v, 97, 232, 0)) return 0;
        z->c = z->l - v_1;
        {
            int v_2 = z->l - z->c;
            if (!(eq_s_b(z, 3, s_10))) goto lab0;
            return 0;
        lab0:
            z->c = z->l - v_2;
        }
    }
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return r_undouble(z);
}

static int r_standard_suffix(struct SN_env * z) {
    int among_var;
    {
        int v_1 = z->l - z->c;
        z->ket = z->c;
        if (z->c <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((540704 >> (z->p[z->c - 1] & 0x1f)) & 1)) goto lab0;
        among_var = find_among_b(z, a_3, 5, 0);
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
                    int ret = slice_from_s(z, 4, s_11);
                    if (ret < 0) return ret;
                }
                break;
            case 2:
                {
                    int ret = r_en_ending(z);
                    if (ret == 0) goto lab0;
                    if (ret < 0) return ret;
                }
                break;
            case 3:
                {
                    int ret = r_R1(z);
                    if (ret == 0) goto lab0;
                    if (ret < 0) return ret;
                }
                if (out_grouping_b_U(z, g_v_j, 97, 232, 0)) goto lab0;
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                break;
        }
    lab0:
        z->c = z->l - v_1;
    }
    {
        int v_2 = z->l - z->c;
        {
            int ret = r_e_ending(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_2;
    }
    {
        int v_3 = z->l - z->c;
        z->ket = z->c;
        if (!(eq_s_b(z, 4, s_12))) goto lab1;
        z->bra = z->c;
        {
            int ret = r_R2(z);
            if (ret == 0) goto lab1;
            if (ret < 0) return ret;
        }
        {
            int v_4 = z->l - z->c;
            if (z->c <= z->lb || z->p[z->c - 1] != 'c') goto lab2;
            z->c--;
            goto lab1;
        lab2:
            z->c = z->l - v_4;
        }
        {
            int ret = slice_del(z);
            if (ret < 0) return ret;
        }
        z->ket = z->c;
        if (!(eq_s_b(z, 2, s_13))) goto lab1;
        z->bra = z->c;
        {
            int ret = r_en_ending(z);
            if (ret == 0) goto lab1;
            if (ret < 0) return ret;
        }
    lab1:
        z->c = z->l - v_3;
    }
    {
        int v_5 = z->l - z->c;
        z->ket = z->c;
        if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((264336 >> (z->p[z->c - 1] & 0x1f)) & 1)) goto lab3;
        among_var = find_among_b(z, a_4, 6, 0);
        if (!among_var) goto lab3;
        z->bra = z->c;
        switch (among_var) {
            case 1:
                {
                    int ret = r_R2(z);
                    if (ret == 0) goto lab3;
                    if (ret < 0) return ret;
                }
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                do {
                    int v_6 = z->l - z->c;
                    z->ket = z->c;
                    if (!(eq_s_b(z, 2, s_14))) goto lab4;
                    z->bra = z->c;
                    {
                        int ret = r_R2(z);
                        if (ret == 0) goto lab4;
                        if (ret < 0) return ret;
                    }
                    {
                        int v_7 = z->l - z->c;
                        if (z->c <= z->lb || z->p[z->c - 1] != 'e') goto lab5;
                        z->c--;
                        goto lab4;
                    lab5:
                        z->c = z->l - v_7;
                    }
                    {
                        int ret = slice_del(z);
                        if (ret < 0) return ret;
                    }
                    break;
                lab4:
                    z->c = z->l - v_6;
                    {
                        int ret = r_undouble(z);
                        if (ret == 0) goto lab3;
                        if (ret < 0) return ret;
                    }
                } while (0);
                break;
            case 2:
                {
                    int ret = r_R2(z);
                    if (ret == 0) goto lab3;
                    if (ret < 0) return ret;
                }
                {
                    int v_8 = z->l - z->c;
                    if (z->c <= z->lb || z->p[z->c - 1] != 'e') goto lab6;
                    z->c--;
                    goto lab3;
                lab6:
                    z->c = z->l - v_8;
                }
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                break;
            case 3:
                {
                    int ret = r_R2(z);
                    if (ret == 0) goto lab3;
                    if (ret < 0) return ret;
                }
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                {
                    int ret = r_e_ending(z);
                    if (ret == 0) goto lab3;
                    if (ret < 0) return ret;
                }
                break;
            case 4:
                {
                    int ret = r_R2(z);
                    if (ret == 0) goto lab3;
                    if (ret < 0) return ret;
                }
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                break;
            case 5:
                {
                    int ret = r_R2(z);
                    if (ret == 0) goto lab3;
                    if (ret < 0) return ret;
                }
                if (!((SN_local *)z)->b_e_found) goto lab3;
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
                break;
        }
    lab3:
        z->c = z->l - v_5;
    }
    {
        int v_9 = z->l - z->c;
        if (out_grouping_b_U(z, g_v_I, 73, 232, 0)) goto lab7;
        {
            int v_10 = z->l - z->c;
            if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((2129954 >> (z->p[z->c - 1] & 0x1f)) & 1)) goto lab7;
            if (!find_among_b(z, a_5, 4, 0)) goto lab7;
            if (out_grouping_b_U(z, g_v, 97, 232, 0)) goto lab7;
            z->c = z->l - v_10;
        }
        z->ket = z->c;
        {
            int ret = skip_b_utf8(z->p, z->c, z->lb, 1);
            if (ret < 0) goto lab7;
            z->c = ret;
        }
        z->bra = z->c;
        {
            int ret = slice_del(z);
            if (ret < 0) return ret;
        }
    lab7:
        z->c = z->l - v_9;
    }
    return 1;
}

extern int dutch_porter_UTF_8_stem(struct SN_env * z) {
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

extern struct SN_env * dutch_porter_UTF_8_create_env(void) {
    struct SN_env * z = SN_new_env(sizeof(SN_local));
    if (z) {
        ((SN_local *)z)->i_p2 = 0;
        ((SN_local *)z)->i_p1 = 0;
        ((SN_local *)z)->b_e_found = 0;
    }
    return z;
}

extern void dutch_porter_UTF_8_close_env(struct SN_env * z) {
    SN_delete_env(z);
}

