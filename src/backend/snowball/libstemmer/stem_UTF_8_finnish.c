/* Generated from finnish.sbl by Snowball 3.0.0 - https://snowballstem.org/ */

#include "stem_UTF_8_finnish.h"

#include <stddef.h>

#include "snowball_runtime.h"

struct SN_local {
    struct SN_env z;
    int i_p2;
    int i_p1;
    unsigned char b_ending_removed;
    symbol * s_x;
};

typedef struct SN_local SN_local;

#ifdef __cplusplus
extern "C" {
#endif
extern int finnish_UTF_8_stem(struct SN_env * z);
#ifdef __cplusplus
}
#endif

static int r_tidy(struct SN_env * z);
static int r_other_endings(struct SN_env * z);
static int r_t_plural(struct SN_env * z);
static int r_i_plural(struct SN_env * z);
static int r_case_ending(struct SN_env * z);
static int r_VI(struct SN_env * z);
static int r_LONG(struct SN_env * z);
static int r_possessive(struct SN_env * z);
static int r_particle_etc(struct SN_env * z);
static int r_R2(struct SN_env * z);
static int r_mark_regions(struct SN_env * z);

static const symbol s_0[] = { 'k', 's', 'e' };
static const symbol s_1[] = { 'k', 's', 'i' };
static const symbol s_2[] = { 0xC3, 0xA4 };
static const symbol s_3[] = { 0xC3, 0xB6 };
static const symbol s_4[] = { 'i', 'e' };
static const symbol s_5[] = { 'p', 'o' };
static const symbol s_6[] = { 'p', 'o' };

static const symbol s_0_0[2] = { 'p', 'a' };
static const symbol s_0_1[3] = { 's', 't', 'i' };
static const symbol s_0_2[4] = { 'k', 'a', 'a', 'n' };
static const symbol s_0_3[3] = { 'h', 'a', 'n' };
static const symbol s_0_4[3] = { 'k', 'i', 'n' };
static const symbol s_0_5[4] = { 'h', 0xC3, 0xA4, 'n' };
static const symbol s_0_6[6] = { 'k', 0xC3, 0xA4, 0xC3, 0xA4, 'n' };
static const symbol s_0_7[2] = { 'k', 'o' };
static const symbol s_0_8[3] = { 'p', 0xC3, 0xA4 };
static const symbol s_0_9[3] = { 'k', 0xC3, 0xB6 };
static const struct among a_0[10] = {
{ 2, s_0_0, 0, 1, 0},
{ 3, s_0_1, 0, 2, 0},
{ 4, s_0_2, 0, 1, 0},
{ 3, s_0_3, 0, 1, 0},
{ 3, s_0_4, 0, 1, 0},
{ 4, s_0_5, 0, 1, 0},
{ 6, s_0_6, 0, 1, 0},
{ 2, s_0_7, 0, 1, 0},
{ 3, s_0_8, 0, 1, 0},
{ 3, s_0_9, 0, 1, 0}
};

static const symbol s_1_0[3] = { 'l', 'l', 'a' };
static const symbol s_1_1[2] = { 'n', 'a' };
static const symbol s_1_2[3] = { 's', 's', 'a' };
static const symbol s_1_3[2] = { 't', 'a' };
static const symbol s_1_4[3] = { 'l', 't', 'a' };
static const symbol s_1_5[3] = { 's', 't', 'a' };
static const struct among a_1[6] = {
{ 3, s_1_0, 0, -1, 0},
{ 2, s_1_1, 0, -1, 0},
{ 3, s_1_2, 0, -1, 0},
{ 2, s_1_3, 0, -1, 0},
{ 3, s_1_4, -1, -1, 0},
{ 3, s_1_5, -2, -1, 0}
};

static const symbol s_2_0[4] = { 'l', 'l', 0xC3, 0xA4 };
static const symbol s_2_1[3] = { 'n', 0xC3, 0xA4 };
static const symbol s_2_2[4] = { 's', 's', 0xC3, 0xA4 };
static const symbol s_2_3[3] = { 't', 0xC3, 0xA4 };
static const symbol s_2_4[4] = { 'l', 't', 0xC3, 0xA4 };
static const symbol s_2_5[4] = { 's', 't', 0xC3, 0xA4 };
static const struct among a_2[6] = {
{ 4, s_2_0, 0, -1, 0},
{ 3, s_2_1, 0, -1, 0},
{ 4, s_2_2, 0, -1, 0},
{ 3, s_2_3, 0, -1, 0},
{ 4, s_2_4, -1, -1, 0},
{ 4, s_2_5, -2, -1, 0}
};

static const symbol s_3_0[3] = { 'l', 'l', 'e' };
static const symbol s_3_1[3] = { 'i', 'n', 'e' };
static const struct among a_3[2] = {
{ 3, s_3_0, 0, -1, 0},
{ 3, s_3_1, 0, -1, 0}
};

static const symbol s_4_0[3] = { 'n', 's', 'a' };
static const symbol s_4_1[3] = { 'm', 'm', 'e' };
static const symbol s_4_2[3] = { 'n', 'n', 'e' };
static const symbol s_4_3[2] = { 'n', 'i' };
static const symbol s_4_4[2] = { 's', 'i' };
static const symbol s_4_5[2] = { 'a', 'n' };
static const symbol s_4_6[2] = { 'e', 'n' };
static const symbol s_4_7[3] = { 0xC3, 0xA4, 'n' };
static const symbol s_4_8[4] = { 'n', 's', 0xC3, 0xA4 };
static const struct among a_4[9] = {
{ 3, s_4_0, 0, 3, 0},
{ 3, s_4_1, 0, 3, 0},
{ 3, s_4_2, 0, 3, 0},
{ 2, s_4_3, 0, 2, 0},
{ 2, s_4_4, 0, 1, 0},
{ 2, s_4_5, 0, 4, 0},
{ 2, s_4_6, 0, 6, 0},
{ 3, s_4_7, 0, 5, 0},
{ 4, s_4_8, 0, 3, 0}
};

static const symbol s_5_0[2] = { 'a', 'a' };
static const symbol s_5_1[2] = { 'e', 'e' };
static const symbol s_5_2[2] = { 'i', 'i' };
static const symbol s_5_3[2] = { 'o', 'o' };
static const symbol s_5_4[2] = { 'u', 'u' };
static const symbol s_5_5[4] = { 0xC3, 0xA4, 0xC3, 0xA4 };
static const symbol s_5_6[4] = { 0xC3, 0xB6, 0xC3, 0xB6 };
static const struct among a_5[7] = {
{ 2, s_5_0, 0, -1, 0},
{ 2, s_5_1, 0, -1, 0},
{ 2, s_5_2, 0, -1, 0},
{ 2, s_5_3, 0, -1, 0},
{ 2, s_5_4, 0, -1, 0},
{ 4, s_5_5, 0, -1, 0},
{ 4, s_5_6, 0, -1, 0}
};

static const symbol s_6_0[1] = { 'a' };
static const symbol s_6_1[3] = { 'l', 'l', 'a' };
static const symbol s_6_2[2] = { 'n', 'a' };
static const symbol s_6_3[3] = { 's', 's', 'a' };
static const symbol s_6_4[2] = { 't', 'a' };
static const symbol s_6_5[3] = { 'l', 't', 'a' };
static const symbol s_6_6[3] = { 's', 't', 'a' };
static const symbol s_6_7[3] = { 't', 't', 'a' };
static const symbol s_6_8[3] = { 'l', 'l', 'e' };
static const symbol s_6_9[3] = { 'i', 'n', 'e' };
static const symbol s_6_10[3] = { 'k', 's', 'i' };
static const symbol s_6_11[1] = { 'n' };
static const symbol s_6_12[3] = { 'h', 'a', 'n' };
static const symbol s_6_13[3] = { 'd', 'e', 'n' };
static const symbol s_6_14[4] = { 's', 'e', 'e', 'n' };
static const symbol s_6_15[3] = { 'h', 'e', 'n' };
static const symbol s_6_16[4] = { 't', 't', 'e', 'n' };
static const symbol s_6_17[3] = { 'h', 'i', 'n' };
static const symbol s_6_18[4] = { 's', 'i', 'i', 'n' };
static const symbol s_6_19[3] = { 'h', 'o', 'n' };
static const symbol s_6_20[4] = { 'h', 0xC3, 0xA4, 'n' };
static const symbol s_6_21[4] = { 'h', 0xC3, 0xB6, 'n' };
static const symbol s_6_22[2] = { 0xC3, 0xA4 };
static const symbol s_6_23[4] = { 'l', 'l', 0xC3, 0xA4 };
static const symbol s_6_24[3] = { 'n', 0xC3, 0xA4 };
static const symbol s_6_25[4] = { 's', 's', 0xC3, 0xA4 };
static const symbol s_6_26[3] = { 't', 0xC3, 0xA4 };
static const symbol s_6_27[4] = { 'l', 't', 0xC3, 0xA4 };
static const symbol s_6_28[4] = { 's', 't', 0xC3, 0xA4 };
static const symbol s_6_29[4] = { 't', 't', 0xC3, 0xA4 };
static const struct among a_6[30] = {
{ 1, s_6_0, 0, 8, 0},
{ 3, s_6_1, -1, -1, 0},
{ 2, s_6_2, -2, -1, 0},
{ 3, s_6_3, -3, -1, 0},
{ 2, s_6_4, -4, -1, 0},
{ 3, s_6_5, -1, -1, 0},
{ 3, s_6_6, -2, -1, 0},
{ 3, s_6_7, -3, 2, 0},
{ 3, s_6_8, 0, -1, 0},
{ 3, s_6_9, 0, -1, 0},
{ 3, s_6_10, 0, -1, 0},
{ 1, s_6_11, 0, 7, 0},
{ 3, s_6_12, -1, 1, 0},
{ 3, s_6_13, -2, -1, 1},
{ 4, s_6_14, -3, -1, 2},
{ 3, s_6_15, -4, 2, 0},
{ 4, s_6_16, -5, -1, 1},
{ 3, s_6_17, -6, 3, 0},
{ 4, s_6_18, -7, -1, 1},
{ 3, s_6_19, -8, 4, 0},
{ 4, s_6_20, -9, 5, 0},
{ 4, s_6_21, -10, 6, 0},
{ 2, s_6_22, 0, 8, 0},
{ 4, s_6_23, -1, -1, 0},
{ 3, s_6_24, -2, -1, 0},
{ 4, s_6_25, -3, -1, 0},
{ 3, s_6_26, -4, -1, 0},
{ 4, s_6_27, -1, -1, 0},
{ 4, s_6_28, -2, -1, 0},
{ 4, s_6_29, -3, 2, 0}
};

static int af_6(struct SN_env * z) {
    switch (z->af) {
        case 1: return r_VI(z);
        case 2: return r_LONG(z);
    }
    return -1;
}

static const symbol s_7_0[3] = { 'e', 'j', 'a' };
static const symbol s_7_1[3] = { 'm', 'm', 'a' };
static const symbol s_7_2[4] = { 'i', 'm', 'm', 'a' };
static const symbol s_7_3[3] = { 'm', 'p', 'a' };
static const symbol s_7_4[4] = { 'i', 'm', 'p', 'a' };
static const symbol s_7_5[3] = { 'm', 'm', 'i' };
static const symbol s_7_6[4] = { 'i', 'm', 'm', 'i' };
static const symbol s_7_7[3] = { 'm', 'p', 'i' };
static const symbol s_7_8[4] = { 'i', 'm', 'p', 'i' };
static const symbol s_7_9[4] = { 'e', 'j', 0xC3, 0xA4 };
static const symbol s_7_10[4] = { 'm', 'm', 0xC3, 0xA4 };
static const symbol s_7_11[5] = { 'i', 'm', 'm', 0xC3, 0xA4 };
static const symbol s_7_12[4] = { 'm', 'p', 0xC3, 0xA4 };
static const symbol s_7_13[5] = { 'i', 'm', 'p', 0xC3, 0xA4 };
static const struct among a_7[14] = {
{ 3, s_7_0, 0, -1, 0},
{ 3, s_7_1, 0, 1, 0},
{ 4, s_7_2, -1, -1, 0},
{ 3, s_7_3, 0, 1, 0},
{ 4, s_7_4, -1, -1, 0},
{ 3, s_7_5, 0, 1, 0},
{ 4, s_7_6, -1, -1, 0},
{ 3, s_7_7, 0, 1, 0},
{ 4, s_7_8, -1, -1, 0},
{ 4, s_7_9, 0, -1, 0},
{ 4, s_7_10, 0, 1, 0},
{ 5, s_7_11, -1, -1, 0},
{ 4, s_7_12, 0, 1, 0},
{ 5, s_7_13, -1, -1, 0}
};

static const symbol s_9_0[3] = { 'm', 'm', 'a' };
static const symbol s_9_1[4] = { 'i', 'm', 'm', 'a' };
static const struct among a_9[2] = {
{ 3, s_9_0, 0, 1, 0},
{ 4, s_9_1, -1, -1, 0}
};

static const unsigned char g_AEI[] = { 17, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8 };

static const unsigned char g_C[] = { 119, 223, 119, 1 };

static const unsigned char g_V1[] = { 17, 65, 16, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 32 };

static const unsigned char g_V2[] = { 17, 65, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 32 };

static const unsigned char g_particle_end[] = { 17, 97, 24, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 32 };

static int r_mark_regions(struct SN_env * z) {
    ((SN_local *)z)->i_p1 = z->l;
    ((SN_local *)z)->i_p2 = z->l;
    {
        int ret = out_grouping_U(z, g_V1, 97, 246, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    {
        int ret = in_grouping_U(z, g_V1, 97, 246, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    ((SN_local *)z)->i_p1 = z->c;
    {
        int ret = out_grouping_U(z, g_V1, 97, 246, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    {
        int ret = in_grouping_U(z, g_V1, 97, 246, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    ((SN_local *)z)->i_p2 = z->c;
    return 1;
}

static int r_R2(struct SN_env * z) {
    return ((SN_local *)z)->i_p2 <= z->c;
}

static int r_particle_etc(struct SN_env * z) {
    int among_var;
    {
        int v_1;
        if (z->c < ((SN_local *)z)->i_p1) return 0;
        v_1 = z->lb; z->lb = ((SN_local *)z)->i_p1;
        z->ket = z->c;
        among_var = find_among_b(z, a_0, 10, 0);
        if (!among_var) { z->lb = v_1; return 0; }
        z->bra = z->c;
        z->lb = v_1;
    }
    switch (among_var) {
        case 1:
            if (in_grouping_b_U(z, g_particle_end, 97, 246, 0)) return 0;
            break;
        case 2:
            {
                int ret = r_R2(z);
                if (ret <= 0) return ret;
            }
            break;
    }
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_possessive(struct SN_env * z) {
    int among_var;
    {
        int v_1;
        if (z->c < ((SN_local *)z)->i_p1) return 0;
        v_1 = z->lb; z->lb = ((SN_local *)z)->i_p1;
        z->ket = z->c;
        among_var = find_among_b(z, a_4, 9, 0);
        if (!among_var) { z->lb = v_1; return 0; }
        z->bra = z->c;
        z->lb = v_1;
    }
    switch (among_var) {
        case 1:
            {
                int v_2 = z->l - z->c;
                if (z->c <= z->lb || z->p[z->c - 1] != 'k') goto lab0;
                z->c--;
                return 0;
            lab0:
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
            z->ket = z->c;
            if (!(eq_s_b(z, 3, s_0))) return 0;
            z->bra = z->c;
            {
                int ret = slice_from_s(z, 3, s_1);
                if (ret < 0) return ret;
            }
            break;
        case 3:
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 4:
            if (z->c - 1 <= z->lb || z->p[z->c - 1] != 97) return 0;
            if (!find_among_b(z, a_1, 6, 0)) return 0;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 5:
            if (z->c - 2 <= z->lb || z->p[z->c - 1] != 164) return 0;
            if (!find_among_b(z, a_2, 6, 0)) return 0;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
        case 6:
            if (z->c - 2 <= z->lb || z->p[z->c - 1] != 101) return 0;
            if (!find_among_b(z, a_3, 2, 0)) return 0;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_LONG(struct SN_env * z) {
    return find_among_b(z, a_5, 7, 0) != 0;
}

static int r_VI(struct SN_env * z) {
    if (z->c <= z->lb || z->p[z->c - 1] != 'i') return 0;
    z->c--;
    return !in_grouping_b_U(z, g_V2, 97, 246, 0);
}

static int r_case_ending(struct SN_env * z) {
    int among_var;
    {
        int v_1;
        if (z->c < ((SN_local *)z)->i_p1) return 0;
        v_1 = z->lb; z->lb = ((SN_local *)z)->i_p1;
        z->ket = z->c;
        among_var = find_among_b(z, a_6, 30, af_6);
        if (!among_var) { z->lb = v_1; return 0; }
        z->bra = z->c;
        z->lb = v_1;
    }
    switch (among_var) {
        case 1:
            if (z->c <= z->lb || z->p[z->c - 1] != 'a') return 0;
            z->c--;
            break;
        case 2:
            if (z->c <= z->lb || z->p[z->c - 1] != 'e') return 0;
            z->c--;
            break;
        case 3:
            if (z->c <= z->lb || z->p[z->c - 1] != 'i') return 0;
            z->c--;
            break;
        case 4:
            if (z->c <= z->lb || z->p[z->c - 1] != 'o') return 0;
            z->c--;
            break;
        case 5:
            if (!(eq_s_b(z, 2, s_2))) return 0;
            break;
        case 6:
            if (!(eq_s_b(z, 2, s_3))) return 0;
            break;
        case 7:
            {
                int v_2 = z->l - z->c;
                {
                    int v_3 = z->l - z->c;
                    do {
                        int v_4 = z->l - z->c;
                        {
                            int ret = r_LONG(z);
                            if (ret == 0) goto lab1;
                            if (ret < 0) return ret;
                        }
                        break;
                    lab1:
                        z->c = z->l - v_4;
                        if (!(eq_s_b(z, 2, s_4))) { z->c = z->l - v_2; goto lab0; }
                    } while (0);
                    z->c = z->l - v_3;
                    {
                        int ret = skip_b_utf8(z->p, z->c, z->lb, 1);
                        if (ret < 0) { z->c = z->l - v_2; goto lab0; }
                        z->c = ret;
                    }
                }
                z->bra = z->c;
            lab0:
                ;
            }
            break;
        case 8:
            if (in_grouping_b_U(z, g_V1, 97, 246, 0)) return 0;
            if (in_grouping_b_U(z, g_C, 98, 122, 0)) return 0;
            break;
    }
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    ((SN_local *)z)->b_ending_removed = 1;
    return 1;
}

static int r_other_endings(struct SN_env * z) {
    int among_var;
    {
        int v_1;
        if (z->c < ((SN_local *)z)->i_p2) return 0;
        v_1 = z->lb; z->lb = ((SN_local *)z)->i_p2;
        z->ket = z->c;
        among_var = find_among_b(z, a_7, 14, 0);
        if (!among_var) { z->lb = v_1; return 0; }
        z->bra = z->c;
        z->lb = v_1;
    }
    switch (among_var) {
        case 1:
            {
                int v_2 = z->l - z->c;
                if (!(eq_s_b(z, 2, s_5))) goto lab0;
                return 0;
            lab0:
                z->c = z->l - v_2;
            }
            break;
    }
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_i_plural(struct SN_env * z) {
    {
        int v_1;
        if (z->c < ((SN_local *)z)->i_p1) return 0;
        v_1 = z->lb; z->lb = ((SN_local *)z)->i_p1;
        z->ket = z->c;
        if (z->c <= z->lb || (z->p[z->c - 1] != 105 && z->p[z->c - 1] != 106)) { z->lb = v_1; return 0; }
        z->c--;
        z->bra = z->c;
        z->lb = v_1;
    }
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_t_plural(struct SN_env * z) {
    int among_var;
    {
        int v_1;
        if (z->c < ((SN_local *)z)->i_p1) return 0;
        v_1 = z->lb; z->lb = ((SN_local *)z)->i_p1;
        z->ket = z->c;
        if (z->c <= z->lb || z->p[z->c - 1] != 't') { z->lb = v_1; return 0; }
        z->c--;
        z->bra = z->c;
        {
            int v_2 = z->l - z->c;
            if (in_grouping_b_U(z, g_V1, 97, 246, 0)) { z->lb = v_1; return 0; }
            z->c = z->l - v_2;
        }
        {
            int ret = slice_del(z);
            if (ret < 0) return ret;
        }
        z->lb = v_1;
    }
    {
        int v_3;
        if (z->c < ((SN_local *)z)->i_p2) return 0;
        v_3 = z->lb; z->lb = ((SN_local *)z)->i_p2;
        z->ket = z->c;
        if (z->c - 2 <= z->lb || z->p[z->c - 1] != 97) { z->lb = v_3; return 0; }
        among_var = find_among_b(z, a_9, 2, 0);
        if (!among_var) { z->lb = v_3; return 0; }
        z->bra = z->c;
        z->lb = v_3;
    }
    switch (among_var) {
        case 1:
            {
                int v_4 = z->l - z->c;
                if (!(eq_s_b(z, 2, s_6))) goto lab0;
                return 0;
            lab0:
                z->c = z->l - v_4;
            }
            break;
    }
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_tidy(struct SN_env * z) {
    {
        int v_1;
        if (z->c < ((SN_local *)z)->i_p1) return 0;
        v_1 = z->lb; z->lb = ((SN_local *)z)->i_p1;
        {
            int v_2 = z->l - z->c;
            {
                int v_3 = z->l - z->c;
                {
                    int ret = r_LONG(z);
                    if (ret == 0) goto lab0;
                    if (ret < 0) return ret;
                }
                z->c = z->l - v_3;
                z->ket = z->c;
                {
                    int ret = skip_b_utf8(z->p, z->c, z->lb, 1);
                    if (ret < 0) goto lab0;
                    z->c = ret;
                }
                z->bra = z->c;
                {
                    int ret = slice_del(z);
                    if (ret < 0) return ret;
                }
            }
        lab0:
            z->c = z->l - v_2;
        }
        {
            int v_4 = z->l - z->c;
            z->ket = z->c;
            if (in_grouping_b_U(z, g_AEI, 97, 228, 0)) goto lab1;
            z->bra = z->c;
            if (in_grouping_b_U(z, g_C, 98, 122, 0)) goto lab1;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
        lab1:
            z->c = z->l - v_4;
        }
        {
            int v_5 = z->l - z->c;
            z->ket = z->c;
            if (z->c <= z->lb || z->p[z->c - 1] != 'j') goto lab2;
            z->c--;
            z->bra = z->c;
            do {
                int v_6 = z->l - z->c;
                if (z->c <= z->lb || z->p[z->c - 1] != 'o') goto lab3;
                z->c--;
                break;
            lab3:
                z->c = z->l - v_6;
                if (z->c <= z->lb || z->p[z->c - 1] != 'u') goto lab2;
                z->c--;
            } while (0);
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
        lab2:
            z->c = z->l - v_5;
        }
        {
            int v_7 = z->l - z->c;
            z->ket = z->c;
            if (z->c <= z->lb || z->p[z->c - 1] != 'o') goto lab4;
            z->c--;
            z->bra = z->c;
            if (z->c <= z->lb || z->p[z->c - 1] != 'j') goto lab4;
            z->c--;
            {
                int ret = slice_del(z);
                if (ret < 0) return ret;
            }
        lab4:
            z->c = z->l - v_7;
        }
        z->lb = v_1;
    }
    if (in_grouping_b_U(z, g_V1, 97, 246, 1) < 0) return 0;
    z->ket = z->c;
    if (in_grouping_b_U(z, g_C, 98, 122, 0)) return 0;
    z->bra = z->c;
    {
        int ret = slice_to(z, &((SN_local *)z)->s_x);
        if (ret < 0) return ret;
    }
    if (!(eq_v_b(z, ((SN_local *)z)->s_x))) return 0;
    {
        int ret = slice_del(z);
        if (ret < 0) return ret;
    }
    return 1;
}

extern int finnish_UTF_8_stem(struct SN_env * z) {
    {
        int v_1 = z->c;
        {
            int ret = r_mark_regions(z);
            if (ret < 0) return ret;
        }
        z->c = v_1;
    }
    ((SN_local *)z)->b_ending_removed = 0;
    z->lb = z->c; z->c = z->l;
    {
        int v_2 = z->l - z->c;
        {
            int ret = r_particle_etc(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_2;
    }
    {
        int v_3 = z->l - z->c;
        {
            int ret = r_possessive(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_3;
    }
    {
        int v_4 = z->l - z->c;
        {
            int ret = r_case_ending(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_4;
    }
    {
        int v_5 = z->l - z->c;
        {
            int ret = r_other_endings(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_5;
    }
    do {
        if (!((SN_local *)z)->b_ending_removed) goto lab0;
        {
            int v_6 = z->l - z->c;
            {
                int ret = r_i_plural(z);
                if (ret < 0) return ret;
            }
            z->c = z->l - v_6;
        }
        break;
    lab0:
        {
            int v_7 = z->l - z->c;
            {
                int ret = r_t_plural(z);
                if (ret < 0) return ret;
            }
            z->c = z->l - v_7;
        }
    } while (0);
    {
        int v_8 = z->l - z->c;
        {
            int ret = r_tidy(z);
            if (ret < 0) return ret;
        }
        z->c = z->l - v_8;
    }
    z->c = z->lb;
    return 1;
}

extern struct SN_env * finnish_UTF_8_create_env(void) {
    struct SN_env * z = SN_new_env(sizeof(SN_local));
    if (z) {
        ((SN_local *)z)->b_ending_removed = 0;
        ((SN_local *)z)->s_x = NULL;
        ((SN_local *)z)->i_p2 = 0;
        ((SN_local *)z)->i_p1 = 0;

        if ((((SN_local *)z)->s_x = create_s()) == NULL) {
            finnish_UTF_8_close_env(z);
            return NULL;
        }
    }
    return z;
}

extern void finnish_UTF_8_close_env(struct SN_env * z) {
    if (z) {
        lose_s(((SN_local *)z)->s_x);
    }
    SN_delete_env(z);
}

