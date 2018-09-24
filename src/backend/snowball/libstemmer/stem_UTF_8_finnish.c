/* This file was generated automatically by the Snowball to ISO C compiler */
/* http://snowballstem.org/ */

#include "header.h"

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
#ifdef __cplusplus
extern "C" {
#endif


extern struct SN_env * finnish_UTF_8_create_env(void);
extern void finnish_UTF_8_close_env(struct SN_env * z);


#ifdef __cplusplus
}
#endif
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

static const struct among a_0[10] =
{
/*  0 */ { 2, s_0_0, -1, 1, 0},
/*  1 */ { 3, s_0_1, -1, 2, 0},
/*  2 */ { 4, s_0_2, -1, 1, 0},
/*  3 */ { 3, s_0_3, -1, 1, 0},
/*  4 */ { 3, s_0_4, -1, 1, 0},
/*  5 */ { 4, s_0_5, -1, 1, 0},
/*  6 */ { 6, s_0_6, -1, 1, 0},
/*  7 */ { 2, s_0_7, -1, 1, 0},
/*  8 */ { 3, s_0_8, -1, 1, 0},
/*  9 */ { 3, s_0_9, -1, 1, 0}
};

static const symbol s_1_0[3] = { 'l', 'l', 'a' };
static const symbol s_1_1[2] = { 'n', 'a' };
static const symbol s_1_2[3] = { 's', 's', 'a' };
static const symbol s_1_3[2] = { 't', 'a' };
static const symbol s_1_4[3] = { 'l', 't', 'a' };
static const symbol s_1_5[3] = { 's', 't', 'a' };

static const struct among a_1[6] =
{
/*  0 */ { 3, s_1_0, -1, -1, 0},
/*  1 */ { 2, s_1_1, -1, -1, 0},
/*  2 */ { 3, s_1_2, -1, -1, 0},
/*  3 */ { 2, s_1_3, -1, -1, 0},
/*  4 */ { 3, s_1_4, 3, -1, 0},
/*  5 */ { 3, s_1_5, 3, -1, 0}
};

static const symbol s_2_0[4] = { 'l', 'l', 0xC3, 0xA4 };
static const symbol s_2_1[3] = { 'n', 0xC3, 0xA4 };
static const symbol s_2_2[4] = { 's', 's', 0xC3, 0xA4 };
static const symbol s_2_3[3] = { 't', 0xC3, 0xA4 };
static const symbol s_2_4[4] = { 'l', 't', 0xC3, 0xA4 };
static const symbol s_2_5[4] = { 's', 't', 0xC3, 0xA4 };

static const struct among a_2[6] =
{
/*  0 */ { 4, s_2_0, -1, -1, 0},
/*  1 */ { 3, s_2_1, -1, -1, 0},
/*  2 */ { 4, s_2_2, -1, -1, 0},
/*  3 */ { 3, s_2_3, -1, -1, 0},
/*  4 */ { 4, s_2_4, 3, -1, 0},
/*  5 */ { 4, s_2_5, 3, -1, 0}
};

static const symbol s_3_0[3] = { 'l', 'l', 'e' };
static const symbol s_3_1[3] = { 'i', 'n', 'e' };

static const struct among a_3[2] =
{
/*  0 */ { 3, s_3_0, -1, -1, 0},
/*  1 */ { 3, s_3_1, -1, -1, 0}
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

static const struct among a_4[9] =
{
/*  0 */ { 3, s_4_0, -1, 3, 0},
/*  1 */ { 3, s_4_1, -1, 3, 0},
/*  2 */ { 3, s_4_2, -1, 3, 0},
/*  3 */ { 2, s_4_3, -1, 2, 0},
/*  4 */ { 2, s_4_4, -1, 1, 0},
/*  5 */ { 2, s_4_5, -1, 4, 0},
/*  6 */ { 2, s_4_6, -1, 6, 0},
/*  7 */ { 3, s_4_7, -1, 5, 0},
/*  8 */ { 4, s_4_8, -1, 3, 0}
};

static const symbol s_5_0[2] = { 'a', 'a' };
static const symbol s_5_1[2] = { 'e', 'e' };
static const symbol s_5_2[2] = { 'i', 'i' };
static const symbol s_5_3[2] = { 'o', 'o' };
static const symbol s_5_4[2] = { 'u', 'u' };
static const symbol s_5_5[4] = { 0xC3, 0xA4, 0xC3, 0xA4 };
static const symbol s_5_6[4] = { 0xC3, 0xB6, 0xC3, 0xB6 };

static const struct among a_5[7] =
{
/*  0 */ { 2, s_5_0, -1, -1, 0},
/*  1 */ { 2, s_5_1, -1, -1, 0},
/*  2 */ { 2, s_5_2, -1, -1, 0},
/*  3 */ { 2, s_5_3, -1, -1, 0},
/*  4 */ { 2, s_5_4, -1, -1, 0},
/*  5 */ { 4, s_5_5, -1, -1, 0},
/*  6 */ { 4, s_5_6, -1, -1, 0}
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

static const struct among a_6[30] =
{
/*  0 */ { 1, s_6_0, -1, 8, 0},
/*  1 */ { 3, s_6_1, 0, -1, 0},
/*  2 */ { 2, s_6_2, 0, -1, 0},
/*  3 */ { 3, s_6_3, 0, -1, 0},
/*  4 */ { 2, s_6_4, 0, -1, 0},
/*  5 */ { 3, s_6_5, 4, -1, 0},
/*  6 */ { 3, s_6_6, 4, -1, 0},
/*  7 */ { 3, s_6_7, 4, 2, 0},
/*  8 */ { 3, s_6_8, -1, -1, 0},
/*  9 */ { 3, s_6_9, -1, -1, 0},
/* 10 */ { 3, s_6_10, -1, -1, 0},
/* 11 */ { 1, s_6_11, -1, 7, 0},
/* 12 */ { 3, s_6_12, 11, 1, 0},
/* 13 */ { 3, s_6_13, 11, -1, r_VI},
/* 14 */ { 4, s_6_14, 11, -1, r_LONG},
/* 15 */ { 3, s_6_15, 11, 2, 0},
/* 16 */ { 4, s_6_16, 11, -1, r_VI},
/* 17 */ { 3, s_6_17, 11, 3, 0},
/* 18 */ { 4, s_6_18, 11, -1, r_VI},
/* 19 */ { 3, s_6_19, 11, 4, 0},
/* 20 */ { 4, s_6_20, 11, 5, 0},
/* 21 */ { 4, s_6_21, 11, 6, 0},
/* 22 */ { 2, s_6_22, -1, 8, 0},
/* 23 */ { 4, s_6_23, 22, -1, 0},
/* 24 */ { 3, s_6_24, 22, -1, 0},
/* 25 */ { 4, s_6_25, 22, -1, 0},
/* 26 */ { 3, s_6_26, 22, -1, 0},
/* 27 */ { 4, s_6_27, 26, -1, 0},
/* 28 */ { 4, s_6_28, 26, -1, 0},
/* 29 */ { 4, s_6_29, 26, 2, 0}
};

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

static const struct among a_7[14] =
{
/*  0 */ { 3, s_7_0, -1, -1, 0},
/*  1 */ { 3, s_7_1, -1, 1, 0},
/*  2 */ { 4, s_7_2, 1, -1, 0},
/*  3 */ { 3, s_7_3, -1, 1, 0},
/*  4 */ { 4, s_7_4, 3, -1, 0},
/*  5 */ { 3, s_7_5, -1, 1, 0},
/*  6 */ { 4, s_7_6, 5, -1, 0},
/*  7 */ { 3, s_7_7, -1, 1, 0},
/*  8 */ { 4, s_7_8, 7, -1, 0},
/*  9 */ { 4, s_7_9, -1, -1, 0},
/* 10 */ { 4, s_7_10, -1, 1, 0},
/* 11 */ { 5, s_7_11, 10, -1, 0},
/* 12 */ { 4, s_7_12, -1, 1, 0},
/* 13 */ { 5, s_7_13, 12, -1, 0}
};

static const symbol s_8_0[1] = { 'i' };
static const symbol s_8_1[1] = { 'j' };

static const struct among a_8[2] =
{
/*  0 */ { 1, s_8_0, -1, -1, 0},
/*  1 */ { 1, s_8_1, -1, -1, 0}
};

static const symbol s_9_0[3] = { 'm', 'm', 'a' };
static const symbol s_9_1[4] = { 'i', 'm', 'm', 'a' };

static const struct among a_9[2] =
{
/*  0 */ { 3, s_9_0, -1, 1, 0},
/*  1 */ { 4, s_9_1, 0, -1, 0}
};

static const unsigned char g_AEI[] = { 17, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8 };

static const unsigned char g_C[] = { 119, 223, 119, 1 };

static const unsigned char g_V1[] = { 17, 65, 16, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 32 };

static const unsigned char g_V2[] = { 17, 65, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 32 };

static const unsigned char g_particle_end[] = { 17, 97, 24, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 32 };

static const symbol s_0[] = { 'k', 's', 'e' };
static const symbol s_1[] = { 'k', 's', 'i' };
static const symbol s_2[] = { 0xC3, 0xA4 };
static const symbol s_3[] = { 0xC3, 0xB6 };
static const symbol s_4[] = { 'i', 'e' };
static const symbol s_5[] = { 'p', 'o' };
static const symbol s_6[] = { 'p', 'o' };

static int r_mark_regions(struct SN_env * z) { /* forwardmode */
    z->I[0] = z->l; /* $p1 = <integer expression>, line 44 */
    z->I[1] = z->l; /* $p2 = <integer expression>, line 45 */
    if (out_grouping_U(z, g_V1, 97, 246, 1) < 0) return 0; /* goto */ /* grouping V1, line 47 */
    {    /* gopast */ /* non V1, line 47 */
        int ret = in_grouping_U(z, g_V1, 97, 246, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    z->I[0] = z->c; /* setmark p1, line 47 */
    if (out_grouping_U(z, g_V1, 97, 246, 1) < 0) return 0; /* goto */ /* grouping V1, line 48 */
    {    /* gopast */ /* non V1, line 48 */
        int ret = in_grouping_U(z, g_V1, 97, 246, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    z->I[1] = z->c; /* setmark p2, line 48 */
    return 1;
}

static int r_R2(struct SN_env * z) { /* backwardmode */
    if (!(z->I[1] <= z->c)) return 0; /* $(<integer expression> <= <integer expression>), line 53 */
    return 1;
}

static int r_particle_etc(struct SN_env * z) { /* backwardmode */
    int among_var;

    {   int mlimit1; /* setlimit, line 56 */
        if (z->c < z->I[0]) return 0;
        mlimit1 = z->lb; z->lb = z->I[0];
        z->ket = z->c; /* [, line 56 */
        among_var = find_among_b(z, a_0, 10); /* substring, line 56 */
        if (!(among_var)) { z->lb = mlimit1; return 0; }
        z->bra = z->c; /* ], line 56 */
        z->lb = mlimit1;
    }
    switch (among_var) { /* among, line 57 */
        case 1:
            if (in_grouping_b_U(z, g_particle_end, 97, 246, 0)) return 0; /* grouping particle_end, line 63 */
            break;
        case 2:
            {   int ret = r_R2(z); /* call R2, line 65 */
                if (ret <= 0) return ret;
            }
            break;
    }
    {   int ret = slice_del(z); /* delete, line 67 */
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_possessive(struct SN_env * z) { /* backwardmode */
    int among_var;

    {   int mlimit1; /* setlimit, line 70 */
        if (z->c < z->I[0]) return 0;
        mlimit1 = z->lb; z->lb = z->I[0];
        z->ket = z->c; /* [, line 70 */
        among_var = find_among_b(z, a_4, 9); /* substring, line 70 */
        if (!(among_var)) { z->lb = mlimit1; return 0; }
        z->bra = z->c; /* ], line 70 */
        z->lb = mlimit1;
    }
    switch (among_var) { /* among, line 71 */
        case 1:
            {   int m2 = z->l - z->c; (void)m2; /* not, line 73 */
                if (z->c <= z->lb || z->p[z->c - 1] != 'k') goto lab0; /* literal, line 73 */
                z->c--;
                return 0;
            lab0:
                z->c = z->l - m2;
            }
            {   int ret = slice_del(z); /* delete, line 73 */
                if (ret < 0) return ret;
            }
            break;
        case 2:
            {   int ret = slice_del(z); /* delete, line 75 */
                if (ret < 0) return ret;
            }
            z->ket = z->c; /* [, line 75 */
            if (!(eq_s_b(z, 3, s_0))) return 0; /* literal, line 75 */
            z->bra = z->c; /* ], line 75 */
            {   int ret = slice_from_s(z, 3, s_1); /* <-, line 75 */
                if (ret < 0) return ret;
            }
            break;
        case 3:
            {   int ret = slice_del(z); /* delete, line 79 */
                if (ret < 0) return ret;
            }
            break;
        case 4:
            if (z->c - 1 <= z->lb || z->p[z->c - 1] != 97) return 0; /* among, line 82 */
            if (!(find_among_b(z, a_1, 6))) return 0;
            {   int ret = slice_del(z); /* delete, line 82 */
                if (ret < 0) return ret;
            }
            break;
        case 5:
            if (z->c - 2 <= z->lb || z->p[z->c - 1] != 164) return 0; /* among, line 84 */
            if (!(find_among_b(z, a_2, 6))) return 0;
            {   int ret = slice_del(z); /* delete, line 85 */
                if (ret < 0) return ret;
            }
            break;
        case 6:
            if (z->c - 2 <= z->lb || z->p[z->c - 1] != 101) return 0; /* among, line 87 */
            if (!(find_among_b(z, a_3, 2))) return 0;
            {   int ret = slice_del(z); /* delete, line 87 */
                if (ret < 0) return ret;
            }
            break;
    }
    return 1;
}

static int r_LONG(struct SN_env * z) { /* backwardmode */
    if (!(find_among_b(z, a_5, 7))) return 0; /* among, line 92 */
    return 1;
}

static int r_VI(struct SN_env * z) { /* backwardmode */
    if (z->c <= z->lb || z->p[z->c - 1] != 'i') return 0; /* literal, line 94 */
    z->c--;
    if (in_grouping_b_U(z, g_V2, 97, 246, 0)) return 0; /* grouping V2, line 94 */
    return 1;
}

static int r_case_ending(struct SN_env * z) { /* backwardmode */
    int among_var;

    {   int mlimit1; /* setlimit, line 97 */
        if (z->c < z->I[0]) return 0;
        mlimit1 = z->lb; z->lb = z->I[0];
        z->ket = z->c; /* [, line 97 */
        among_var = find_among_b(z, a_6, 30); /* substring, line 97 */
        if (!(among_var)) { z->lb = mlimit1; return 0; }
        z->bra = z->c; /* ], line 97 */
        z->lb = mlimit1;
    }
    switch (among_var) { /* among, line 98 */
        case 1:
            if (z->c <= z->lb || z->p[z->c - 1] != 'a') return 0; /* literal, line 99 */
            z->c--;
            break;
        case 2:
            if (z->c <= z->lb || z->p[z->c - 1] != 'e') return 0; /* literal, line 100 */
            z->c--;
            break;
        case 3:
            if (z->c <= z->lb || z->p[z->c - 1] != 'i') return 0; /* literal, line 101 */
            z->c--;
            break;
        case 4:
            if (z->c <= z->lb || z->p[z->c - 1] != 'o') return 0; /* literal, line 102 */
            z->c--;
            break;
        case 5:
            if (!(eq_s_b(z, 2, s_2))) return 0; /* literal, line 103 */
            break;
        case 6:
            if (!(eq_s_b(z, 2, s_3))) return 0; /* literal, line 104 */
            break;
        case 7:
            {   int m2 = z->l - z->c; (void)m2; /* try, line 112 */
                {   int m3 = z->l - z->c; (void)m3; /* and, line 114 */
                    {   int m4 = z->l - z->c; (void)m4; /* or, line 113 */
                        {   int ret = r_LONG(z); /* call LONG, line 112 */
                            if (ret == 0) goto lab2;
                            if (ret < 0) return ret;
                        }
                        goto lab1;
                    lab2:
                        z->c = z->l - m4;
                        if (!(eq_s_b(z, 2, s_4))) { z->c = z->l - m2; goto lab0; } /* literal, line 113 */
                    }
                lab1:
                    z->c = z->l - m3;
                    {   int ret = skip_utf8(z->p, z->c, z->lb, 0, -1);
                        if (ret < 0) { z->c = z->l - m2; goto lab0; }
                        z->c = ret; /* next, line 114 */
                    }
                }
                z->bra = z->c; /* ], line 114 */
            lab0:
                ;
            }
            break;
        case 8:
            if (in_grouping_b_U(z, g_V1, 97, 246, 0)) return 0; /* grouping V1, line 120 */
            if (in_grouping_b_U(z, g_C, 98, 122, 0)) return 0; /* grouping C, line 120 */
            break;
    }
    {   int ret = slice_del(z); /* delete, line 139 */
        if (ret < 0) return ret;
    }
    z->B[0] = 1; /* set ending_removed, line 140 */
    return 1;
}

static int r_other_endings(struct SN_env * z) { /* backwardmode */
    int among_var;

    {   int mlimit1; /* setlimit, line 143 */
        if (z->c < z->I[1]) return 0;
        mlimit1 = z->lb; z->lb = z->I[1];
        z->ket = z->c; /* [, line 143 */
        among_var = find_among_b(z, a_7, 14); /* substring, line 143 */
        if (!(among_var)) { z->lb = mlimit1; return 0; }
        z->bra = z->c; /* ], line 143 */
        z->lb = mlimit1;
    }
    switch (among_var) { /* among, line 144 */
        case 1:
            {   int m2 = z->l - z->c; (void)m2; /* not, line 147 */
                if (!(eq_s_b(z, 2, s_5))) goto lab0; /* literal, line 147 */
                return 0;
            lab0:
                z->c = z->l - m2;
            }
            break;
    }
    {   int ret = slice_del(z); /* delete, line 152 */
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_i_plural(struct SN_env * z) { /* backwardmode */

    {   int mlimit1; /* setlimit, line 155 */
        if (z->c < z->I[0]) return 0;
        mlimit1 = z->lb; z->lb = z->I[0];
        z->ket = z->c; /* [, line 155 */
        if (z->c <= z->lb || (z->p[z->c - 1] != 105 && z->p[z->c - 1] != 106)) { z->lb = mlimit1; return 0; } /* substring, line 155 */
        if (!(find_among_b(z, a_8, 2))) { z->lb = mlimit1; return 0; }
        z->bra = z->c; /* ], line 155 */
        z->lb = mlimit1;
    }
    {   int ret = slice_del(z); /* delete, line 159 */
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_t_plural(struct SN_env * z) { /* backwardmode */
    int among_var;

    {   int mlimit1; /* setlimit, line 162 */
        if (z->c < z->I[0]) return 0;
        mlimit1 = z->lb; z->lb = z->I[0];
        z->ket = z->c; /* [, line 163 */
        if (z->c <= z->lb || z->p[z->c - 1] != 't') { z->lb = mlimit1; return 0; } /* literal, line 163 */
        z->c--;
        z->bra = z->c; /* ], line 163 */
        {   int m_test2 = z->l - z->c; /* test, line 163 */
            if (in_grouping_b_U(z, g_V1, 97, 246, 0)) { z->lb = mlimit1; return 0; } /* grouping V1, line 163 */
            z->c = z->l - m_test2;
        }
        {   int ret = slice_del(z); /* delete, line 164 */
            if (ret < 0) return ret;
        }
        z->lb = mlimit1;
    }

    {   int mlimit3; /* setlimit, line 166 */
        if (z->c < z->I[1]) return 0;
        mlimit3 = z->lb; z->lb = z->I[1];
        z->ket = z->c; /* [, line 166 */
        if (z->c - 2 <= z->lb || z->p[z->c - 1] != 97) { z->lb = mlimit3; return 0; } /* substring, line 166 */
        among_var = find_among_b(z, a_9, 2);
        if (!(among_var)) { z->lb = mlimit3; return 0; }
        z->bra = z->c; /* ], line 166 */
        z->lb = mlimit3;
    }
    switch (among_var) { /* among, line 167 */
        case 1:
            {   int m4 = z->l - z->c; (void)m4; /* not, line 168 */
                if (!(eq_s_b(z, 2, s_6))) goto lab0; /* literal, line 168 */
                return 0;
            lab0:
                z->c = z->l - m4;
            }
            break;
    }
    {   int ret = slice_del(z); /* delete, line 171 */
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_tidy(struct SN_env * z) { /* backwardmode */

    {   int mlimit1; /* setlimit, line 174 */
        if (z->c < z->I[0]) return 0;
        mlimit1 = z->lb; z->lb = z->I[0];
        {   int m2 = z->l - z->c; (void)m2; /* do, line 175 */
            {   int m3 = z->l - z->c; (void)m3; /* and, line 175 */
                {   int ret = r_LONG(z); /* call LONG, line 175 */
                    if (ret == 0) goto lab0;
                    if (ret < 0) return ret;
                }
                z->c = z->l - m3;
                z->ket = z->c; /* [, line 175 */
                {   int ret = skip_utf8(z->p, z->c, z->lb, 0, -1);
                    if (ret < 0) goto lab0;
                    z->c = ret; /* next, line 175 */
                }
                z->bra = z->c; /* ], line 175 */
                {   int ret = slice_del(z); /* delete, line 175 */
                    if (ret < 0) return ret;
                }
            }
        lab0:
            z->c = z->l - m2;
        }
        {   int m4 = z->l - z->c; (void)m4; /* do, line 176 */
            z->ket = z->c; /* [, line 176 */
            if (in_grouping_b_U(z, g_AEI, 97, 228, 0)) goto lab1; /* grouping AEI, line 176 */
            z->bra = z->c; /* ], line 176 */
            if (in_grouping_b_U(z, g_C, 98, 122, 0)) goto lab1; /* grouping C, line 176 */
            {   int ret = slice_del(z); /* delete, line 176 */
                if (ret < 0) return ret;
            }
        lab1:
            z->c = z->l - m4;
        }
        {   int m5 = z->l - z->c; (void)m5; /* do, line 177 */
            z->ket = z->c; /* [, line 177 */
            if (z->c <= z->lb || z->p[z->c - 1] != 'j') goto lab2; /* literal, line 177 */
            z->c--;
            z->bra = z->c; /* ], line 177 */
            {   int m6 = z->l - z->c; (void)m6; /* or, line 177 */
                if (z->c <= z->lb || z->p[z->c - 1] != 'o') goto lab4; /* literal, line 177 */
                z->c--;
                goto lab3;
            lab4:
                z->c = z->l - m6;
                if (z->c <= z->lb || z->p[z->c - 1] != 'u') goto lab2; /* literal, line 177 */
                z->c--;
            }
        lab3:
            {   int ret = slice_del(z); /* delete, line 177 */
                if (ret < 0) return ret;
            }
        lab2:
            z->c = z->l - m5;
        }
        {   int m7 = z->l - z->c; (void)m7; /* do, line 178 */
            z->ket = z->c; /* [, line 178 */
            if (z->c <= z->lb || z->p[z->c - 1] != 'o') goto lab5; /* literal, line 178 */
            z->c--;
            z->bra = z->c; /* ], line 178 */
            if (z->c <= z->lb || z->p[z->c - 1] != 'j') goto lab5; /* literal, line 178 */
            z->c--;
            {   int ret = slice_del(z); /* delete, line 178 */
                if (ret < 0) return ret;
            }
        lab5:
            z->c = z->l - m7;
        }
        z->lb = mlimit1;
    }
    if (in_grouping_b_U(z, g_V1, 97, 246, 1) < 0) return 0; /* goto */ /* non V1, line 180 */
    z->ket = z->c; /* [, line 180 */
    if (in_grouping_b_U(z, g_C, 98, 122, 0)) return 0; /* grouping C, line 180 */
    z->bra = z->c; /* ], line 180 */
    z->S[0] = slice_to(z, z->S[0]); /* -> x, line 180 */
    if (z->S[0] == 0) return -1; /* -> x, line 180 */
    if (!(eq_v_b(z, z->S[0]))) return 0; /* name x, line 180 */
    {   int ret = slice_del(z); /* delete, line 180 */
        if (ret < 0) return ret;
    }
    return 1;
}

extern int finnish_UTF_8_stem(struct SN_env * z) { /* forwardmode */
    {   int c1 = z->c; /* do, line 186 */
        {   int ret = r_mark_regions(z); /* call mark_regions, line 186 */
            if (ret == 0) goto lab0;
            if (ret < 0) return ret;
        }
    lab0:
        z->c = c1;
    }
    z->B[0] = 0; /* unset ending_removed, line 187 */
    z->lb = z->c; z->c = z->l; /* backwards, line 188 */

    {   int m2 = z->l - z->c; (void)m2; /* do, line 189 */
        {   int ret = r_particle_etc(z); /* call particle_etc, line 189 */
            if (ret == 0) goto lab1;
            if (ret < 0) return ret;
        }
    lab1:
        z->c = z->l - m2;
    }
    {   int m3 = z->l - z->c; (void)m3; /* do, line 190 */
        {   int ret = r_possessive(z); /* call possessive, line 190 */
            if (ret == 0) goto lab2;
            if (ret < 0) return ret;
        }
    lab2:
        z->c = z->l - m3;
    }
    {   int m4 = z->l - z->c; (void)m4; /* do, line 191 */
        {   int ret = r_case_ending(z); /* call case_ending, line 191 */
            if (ret == 0) goto lab3;
            if (ret < 0) return ret;
        }
    lab3:
        z->c = z->l - m4;
    }
    {   int m5 = z->l - z->c; (void)m5; /* do, line 192 */
        {   int ret = r_other_endings(z); /* call other_endings, line 192 */
            if (ret == 0) goto lab4;
            if (ret < 0) return ret;
        }
    lab4:
        z->c = z->l - m5;
    }
    /* or, line 193 */
    if (!(z->B[0])) goto lab6; /* Boolean test ending_removed, line 193 */
    {   int m6 = z->l - z->c; (void)m6; /* do, line 193 */
        {   int ret = r_i_plural(z); /* call i_plural, line 193 */
            if (ret == 0) goto lab7;
            if (ret < 0) return ret;
        }
    lab7:
        z->c = z->l - m6;
    }
    goto lab5;
lab6:
    {   int m7 = z->l - z->c; (void)m7; /* do, line 193 */
        {   int ret = r_t_plural(z); /* call t_plural, line 193 */
            if (ret == 0) goto lab8;
            if (ret < 0) return ret;
        }
    lab8:
        z->c = z->l - m7;
    }
lab5:
    {   int m8 = z->l - z->c; (void)m8; /* do, line 194 */
        {   int ret = r_tidy(z); /* call tidy, line 194 */
            if (ret == 0) goto lab9;
            if (ret < 0) return ret;
        }
    lab9:
        z->c = z->l - m8;
    }
    z->c = z->lb;
    return 1;
}

extern struct SN_env * finnish_UTF_8_create_env(void) { return SN_create_env(1, 2, 1); }

extern void finnish_UTF_8_close_env(struct SN_env * z) { SN_close_env(z, 1); }

