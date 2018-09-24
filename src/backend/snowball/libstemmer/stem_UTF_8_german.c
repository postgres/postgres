/* This file was generated automatically by the Snowball to ISO C compiler */
/* http://snowballstem.org/ */

#include "header.h"

#ifdef __cplusplus
extern "C" {
#endif
extern int german_UTF_8_stem(struct SN_env * z);
#ifdef __cplusplus
}
#endif
static int r_standard_suffix(struct SN_env * z);
static int r_R2(struct SN_env * z);
static int r_R1(struct SN_env * z);
static int r_mark_regions(struct SN_env * z);
static int r_postlude(struct SN_env * z);
static int r_prelude(struct SN_env * z);
#ifdef __cplusplus
extern "C" {
#endif


extern struct SN_env * german_UTF_8_create_env(void);
extern void german_UTF_8_close_env(struct SN_env * z);


#ifdef __cplusplus
}
#endif
static const symbol s_0_1[1] = { 'U' };
static const symbol s_0_2[1] = { 'Y' };
static const symbol s_0_3[2] = { 0xC3, 0xA4 };
static const symbol s_0_4[2] = { 0xC3, 0xB6 };
static const symbol s_0_5[2] = { 0xC3, 0xBC };

static const struct among a_0[6] =
{
/*  0 */ { 0, 0, -1, 5, 0},
/*  1 */ { 1, s_0_1, 0, 2, 0},
/*  2 */ { 1, s_0_2, 0, 1, 0},
/*  3 */ { 2, s_0_3, 0, 3, 0},
/*  4 */ { 2, s_0_4, 0, 4, 0},
/*  5 */ { 2, s_0_5, 0, 2, 0}
};

static const symbol s_1_0[1] = { 'e' };
static const symbol s_1_1[2] = { 'e', 'm' };
static const symbol s_1_2[2] = { 'e', 'n' };
static const symbol s_1_3[3] = { 'e', 'r', 'n' };
static const symbol s_1_4[2] = { 'e', 'r' };
static const symbol s_1_5[1] = { 's' };
static const symbol s_1_6[2] = { 'e', 's' };

static const struct among a_1[7] =
{
/*  0 */ { 1, s_1_0, -1, 2, 0},
/*  1 */ { 2, s_1_1, -1, 1, 0},
/*  2 */ { 2, s_1_2, -1, 2, 0},
/*  3 */ { 3, s_1_3, -1, 1, 0},
/*  4 */ { 2, s_1_4, -1, 1, 0},
/*  5 */ { 1, s_1_5, -1, 3, 0},
/*  6 */ { 2, s_1_6, 5, 2, 0}
};

static const symbol s_2_0[2] = { 'e', 'n' };
static const symbol s_2_1[2] = { 'e', 'r' };
static const symbol s_2_2[2] = { 's', 't' };
static const symbol s_2_3[3] = { 'e', 's', 't' };

static const struct among a_2[4] =
{
/*  0 */ { 2, s_2_0, -1, 1, 0},
/*  1 */ { 2, s_2_1, -1, 1, 0},
/*  2 */ { 2, s_2_2, -1, 2, 0},
/*  3 */ { 3, s_2_3, 2, 1, 0}
};

static const symbol s_3_0[2] = { 'i', 'g' };
static const symbol s_3_1[4] = { 'l', 'i', 'c', 'h' };

static const struct among a_3[2] =
{
/*  0 */ { 2, s_3_0, -1, 1, 0},
/*  1 */ { 4, s_3_1, -1, 1, 0}
};

static const symbol s_4_0[3] = { 'e', 'n', 'd' };
static const symbol s_4_1[2] = { 'i', 'g' };
static const symbol s_4_2[3] = { 'u', 'n', 'g' };
static const symbol s_4_3[4] = { 'l', 'i', 'c', 'h' };
static const symbol s_4_4[4] = { 'i', 's', 'c', 'h' };
static const symbol s_4_5[2] = { 'i', 'k' };
static const symbol s_4_6[4] = { 'h', 'e', 'i', 't' };
static const symbol s_4_7[4] = { 'k', 'e', 'i', 't' };

static const struct among a_4[8] =
{
/*  0 */ { 3, s_4_0, -1, 1, 0},
/*  1 */ { 2, s_4_1, -1, 2, 0},
/*  2 */ { 3, s_4_2, -1, 1, 0},
/*  3 */ { 4, s_4_3, -1, 3, 0},
/*  4 */ { 4, s_4_4, -1, 2, 0},
/*  5 */ { 2, s_4_5, -1, 2, 0},
/*  6 */ { 4, s_4_6, -1, 3, 0},
/*  7 */ { 4, s_4_7, -1, 4, 0}
};

static const unsigned char g_v[] = { 17, 65, 16, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 32, 8 };

static const unsigned char g_s_ending[] = { 117, 30, 5 };

static const unsigned char g_st_ending[] = { 117, 30, 4 };

static const symbol s_0[] = { 0xC3, 0x9F };
static const symbol s_1[] = { 's', 's' };
static const symbol s_2[] = { 'U' };
static const symbol s_3[] = { 'Y' };
static const symbol s_4[] = { 'y' };
static const symbol s_5[] = { 'u' };
static const symbol s_6[] = { 'a' };
static const symbol s_7[] = { 'o' };
static const symbol s_8[] = { 'n', 'i', 's' };
static const symbol s_9[] = { 'i', 'g' };
static const symbol s_10[] = { 'e', 'r' };
static const symbol s_11[] = { 'e', 'n' };

static int r_prelude(struct SN_env * z) { /* forwardmode */
    {   int c_test1 = z->c; /* test, line 35 */
        while(1) { /* repeat, line 35 */
            int c2 = z->c;
            {   int c3 = z->c; /* or, line 38 */
                z->bra = z->c; /* [, line 37 */
                if (!(eq_s(z, 2, s_0))) goto lab2; /* literal, line 37 */
                z->ket = z->c; /* ], line 37 */
                {   int ret = slice_from_s(z, 2, s_1); /* <-, line 37 */
                    if (ret < 0) return ret;
                }
                goto lab1;
            lab2:
                z->c = c3;
                {   int ret = skip_utf8(z->p, z->c, 0, z->l, 1);
                    if (ret < 0) goto lab0;
                    z->c = ret; /* next, line 38 */
                }
            }
        lab1:
            continue;
        lab0:
            z->c = c2;
            break;
        }
        z->c = c_test1;
    }
    while(1) { /* repeat, line 41 */
        int c4 = z->c;
        while(1) { /* goto, line 41 */
            int c5 = z->c;
            if (in_grouping_U(z, g_v, 97, 252, 0)) goto lab4; /* grouping v, line 42 */
            z->bra = z->c; /* [, line 42 */
            {   int c6 = z->c; /* or, line 42 */
                if (z->c == z->l || z->p[z->c] != 'u') goto lab6; /* literal, line 42 */
                z->c++;
                z->ket = z->c; /* ], line 42 */
                if (in_grouping_U(z, g_v, 97, 252, 0)) goto lab6; /* grouping v, line 42 */
                {   int ret = slice_from_s(z, 1, s_2); /* <-, line 42 */
                    if (ret < 0) return ret;
                }
                goto lab5;
            lab6:
                z->c = c6;
                if (z->c == z->l || z->p[z->c] != 'y') goto lab4; /* literal, line 43 */
                z->c++;
                z->ket = z->c; /* ], line 43 */
                if (in_grouping_U(z, g_v, 97, 252, 0)) goto lab4; /* grouping v, line 43 */
                {   int ret = slice_from_s(z, 1, s_3); /* <-, line 43 */
                    if (ret < 0) return ret;
                }
            }
        lab5:
            z->c = c5;
            break;
        lab4:
            z->c = c5;
            {   int ret = skip_utf8(z->p, z->c, 0, z->l, 1);
                if (ret < 0) goto lab3;
                z->c = ret; /* goto, line 41 */
            }
        }
        continue;
    lab3:
        z->c = c4;
        break;
    }
    return 1;
}

static int r_mark_regions(struct SN_env * z) { /* forwardmode */
    z->I[0] = z->l; /* $p1 = <integer expression>, line 49 */
    z->I[1] = z->l; /* $p2 = <integer expression>, line 50 */
    {   int c_test1 = z->c; /* test, line 52 */
        {   int ret = skip_utf8(z->p, z->c, 0, z->l, + 3); /* hop, line 52 */
            if (ret < 0) return 0;
            z->c = ret;
        }
        z->I[2] = z->c; /* setmark x, line 52 */
        z->c = c_test1;
    }
    {    /* gopast */ /* grouping v, line 54 */
        int ret = out_grouping_U(z, g_v, 97, 252, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    {    /* gopast */ /* non v, line 54 */
        int ret = in_grouping_U(z, g_v, 97, 252, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    z->I[0] = z->c; /* setmark p1, line 54 */
    /* try, line 55 */
    if (!(z->I[0] < z->I[2])) goto lab0; /* $(<integer expression> < <integer expression>), line 55 */
    z->I[0] = z->I[2]; /* $p1 = <integer expression>, line 55 */
lab0:
    {    /* gopast */ /* grouping v, line 56 */
        int ret = out_grouping_U(z, g_v, 97, 252, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    {    /* gopast */ /* non v, line 56 */
        int ret = in_grouping_U(z, g_v, 97, 252, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    z->I[1] = z->c; /* setmark p2, line 56 */
    return 1;
}

static int r_postlude(struct SN_env * z) { /* forwardmode */
    int among_var;
    while(1) { /* repeat, line 60 */
        int c1 = z->c;
        z->bra = z->c; /* [, line 62 */
        among_var = find_among(z, a_0, 6); /* substring, line 62 */
        if (!(among_var)) goto lab0;
        z->ket = z->c; /* ], line 62 */
        switch (among_var) { /* among, line 62 */
            case 1:
                {   int ret = slice_from_s(z, 1, s_4); /* <-, line 63 */
                    if (ret < 0) return ret;
                }
                break;
            case 2:
                {   int ret = slice_from_s(z, 1, s_5); /* <-, line 64 */
                    if (ret < 0) return ret;
                }
                break;
            case 3:
                {   int ret = slice_from_s(z, 1, s_6); /* <-, line 65 */
                    if (ret < 0) return ret;
                }
                break;
            case 4:
                {   int ret = slice_from_s(z, 1, s_7); /* <-, line 66 */
                    if (ret < 0) return ret;
                }
                break;
            case 5:
                {   int ret = skip_utf8(z->p, z->c, 0, z->l, 1);
                    if (ret < 0) goto lab0;
                    z->c = ret; /* next, line 68 */
                }
                break;
        }
        continue;
    lab0:
        z->c = c1;
        break;
    }
    return 1;
}

static int r_R1(struct SN_env * z) { /* backwardmode */
    if (!(z->I[0] <= z->c)) return 0; /* $(<integer expression> <= <integer expression>), line 75 */
    return 1;
}

static int r_R2(struct SN_env * z) { /* backwardmode */
    if (!(z->I[1] <= z->c)) return 0; /* $(<integer expression> <= <integer expression>), line 76 */
    return 1;
}

static int r_standard_suffix(struct SN_env * z) { /* backwardmode */
    int among_var;
    {   int m1 = z->l - z->c; (void)m1; /* do, line 79 */
        z->ket = z->c; /* [, line 80 */
        if (z->c <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((811040 >> (z->p[z->c - 1] & 0x1f)) & 1)) goto lab0; /* substring, line 80 */
        among_var = find_among_b(z, a_1, 7);
        if (!(among_var)) goto lab0;
        z->bra = z->c; /* ], line 80 */
        {   int ret = r_R1(z); /* call R1, line 80 */
            if (ret == 0) goto lab0;
            if (ret < 0) return ret;
        }
        switch (among_var) { /* among, line 80 */
            case 1:
                {   int ret = slice_del(z); /* delete, line 82 */
                    if (ret < 0) return ret;
                }
                break;
            case 2:
                {   int ret = slice_del(z); /* delete, line 85 */
                    if (ret < 0) return ret;
                }
                {   int m2 = z->l - z->c; (void)m2; /* try, line 86 */
                    z->ket = z->c; /* [, line 86 */
                    if (z->c <= z->lb || z->p[z->c - 1] != 's') { z->c = z->l - m2; goto lab1; } /* literal, line 86 */
                    z->c--;
                    z->bra = z->c; /* ], line 86 */
                    if (!(eq_s_b(z, 3, s_8))) { z->c = z->l - m2; goto lab1; } /* literal, line 86 */
                    {   int ret = slice_del(z); /* delete, line 86 */
                        if (ret < 0) return ret;
                    }
                lab1:
                    ;
                }
                break;
            case 3:
                if (in_grouping_b_U(z, g_s_ending, 98, 116, 0)) goto lab0; /* grouping s_ending, line 89 */
                {   int ret = slice_del(z); /* delete, line 89 */
                    if (ret < 0) return ret;
                }
                break;
        }
    lab0:
        z->c = z->l - m1;
    }
    {   int m3 = z->l - z->c; (void)m3; /* do, line 93 */
        z->ket = z->c; /* [, line 94 */
        if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((1327104 >> (z->p[z->c - 1] & 0x1f)) & 1)) goto lab2; /* substring, line 94 */
        among_var = find_among_b(z, a_2, 4);
        if (!(among_var)) goto lab2;
        z->bra = z->c; /* ], line 94 */
        {   int ret = r_R1(z); /* call R1, line 94 */
            if (ret == 0) goto lab2;
            if (ret < 0) return ret;
        }
        switch (among_var) { /* among, line 94 */
            case 1:
                {   int ret = slice_del(z); /* delete, line 96 */
                    if (ret < 0) return ret;
                }
                break;
            case 2:
                if (in_grouping_b_U(z, g_st_ending, 98, 116, 0)) goto lab2; /* grouping st_ending, line 99 */
                {   int ret = skip_utf8(z->p, z->c, z->lb, z->l, - 3); /* hop, line 99 */
                    if (ret < 0) goto lab2;
                    z->c = ret;
                }
                {   int ret = slice_del(z); /* delete, line 99 */
                    if (ret < 0) return ret;
                }
                break;
        }
    lab2:
        z->c = z->l - m3;
    }
    {   int m4 = z->l - z->c; (void)m4; /* do, line 103 */
        z->ket = z->c; /* [, line 104 */
        if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((1051024 >> (z->p[z->c - 1] & 0x1f)) & 1)) goto lab3; /* substring, line 104 */
        among_var = find_among_b(z, a_4, 8);
        if (!(among_var)) goto lab3;
        z->bra = z->c; /* ], line 104 */
        {   int ret = r_R2(z); /* call R2, line 104 */
            if (ret == 0) goto lab3;
            if (ret < 0) return ret;
        }
        switch (among_var) { /* among, line 104 */
            case 1:
                {   int ret = slice_del(z); /* delete, line 106 */
                    if (ret < 0) return ret;
                }
                {   int m5 = z->l - z->c; (void)m5; /* try, line 107 */
                    z->ket = z->c; /* [, line 107 */
                    if (!(eq_s_b(z, 2, s_9))) { z->c = z->l - m5; goto lab4; } /* literal, line 107 */
                    z->bra = z->c; /* ], line 107 */
                    {   int m6 = z->l - z->c; (void)m6; /* not, line 107 */
                        if (z->c <= z->lb || z->p[z->c - 1] != 'e') goto lab5; /* literal, line 107 */
                        z->c--;
                        { z->c = z->l - m5; goto lab4; }
                    lab5:
                        z->c = z->l - m6;
                    }
                    {   int ret = r_R2(z); /* call R2, line 107 */
                        if (ret == 0) { z->c = z->l - m5; goto lab4; }
                        if (ret < 0) return ret;
                    }
                    {   int ret = slice_del(z); /* delete, line 107 */
                        if (ret < 0) return ret;
                    }
                lab4:
                    ;
                }
                break;
            case 2:
                {   int m7 = z->l - z->c; (void)m7; /* not, line 110 */
                    if (z->c <= z->lb || z->p[z->c - 1] != 'e') goto lab6; /* literal, line 110 */
                    z->c--;
                    goto lab3;
                lab6:
                    z->c = z->l - m7;
                }
                {   int ret = slice_del(z); /* delete, line 110 */
                    if (ret < 0) return ret;
                }
                break;
            case 3:
                {   int ret = slice_del(z); /* delete, line 113 */
                    if (ret < 0) return ret;
                }
                {   int m8 = z->l - z->c; (void)m8; /* try, line 114 */
                    z->ket = z->c; /* [, line 115 */
                    {   int m9 = z->l - z->c; (void)m9; /* or, line 115 */
                        if (!(eq_s_b(z, 2, s_10))) goto lab9; /* literal, line 115 */
                        goto lab8;
                    lab9:
                        z->c = z->l - m9;
                        if (!(eq_s_b(z, 2, s_11))) { z->c = z->l - m8; goto lab7; } /* literal, line 115 */
                    }
                lab8:
                    z->bra = z->c; /* ], line 115 */
                    {   int ret = r_R1(z); /* call R1, line 115 */
                        if (ret == 0) { z->c = z->l - m8; goto lab7; }
                        if (ret < 0) return ret;
                    }
                    {   int ret = slice_del(z); /* delete, line 115 */
                        if (ret < 0) return ret;
                    }
                lab7:
                    ;
                }
                break;
            case 4:
                {   int ret = slice_del(z); /* delete, line 119 */
                    if (ret < 0) return ret;
                }
                {   int m10 = z->l - z->c; (void)m10; /* try, line 120 */
                    z->ket = z->c; /* [, line 121 */
                    if (z->c - 1 <= z->lb || (z->p[z->c - 1] != 103 && z->p[z->c - 1] != 104)) { z->c = z->l - m10; goto lab10; } /* substring, line 121 */
                    if (!(find_among_b(z, a_3, 2))) { z->c = z->l - m10; goto lab10; }
                    z->bra = z->c; /* ], line 121 */
                    {   int ret = r_R2(z); /* call R2, line 121 */
                        if (ret == 0) { z->c = z->l - m10; goto lab10; }
                        if (ret < 0) return ret;
                    }
                    {   int ret = slice_del(z); /* delete, line 123 */
                        if (ret < 0) return ret;
                    }
                lab10:
                    ;
                }
                break;
        }
    lab3:
        z->c = z->l - m4;
    }
    return 1;
}

extern int german_UTF_8_stem(struct SN_env * z) { /* forwardmode */
    {   int c1 = z->c; /* do, line 134 */
        {   int ret = r_prelude(z); /* call prelude, line 134 */
            if (ret == 0) goto lab0;
            if (ret < 0) return ret;
        }
    lab0:
        z->c = c1;
    }
    {   int c2 = z->c; /* do, line 135 */
        {   int ret = r_mark_regions(z); /* call mark_regions, line 135 */
            if (ret == 0) goto lab1;
            if (ret < 0) return ret;
        }
    lab1:
        z->c = c2;
    }
    z->lb = z->c; z->c = z->l; /* backwards, line 136 */

    /* do, line 137 */
    {   int ret = r_standard_suffix(z); /* call standard_suffix, line 137 */
        if (ret == 0) goto lab2;
        if (ret < 0) return ret;
    }
lab2:
    z->c = z->lb;
    {   int c3 = z->c; /* do, line 138 */
        {   int ret = r_postlude(z); /* call postlude, line 138 */
            if (ret == 0) goto lab3;
            if (ret < 0) return ret;
        }
    lab3:
        z->c = c3;
    }
    return 1;
}

extern struct SN_env * german_UTF_8_create_env(void) { return SN_create_env(0, 3, 0); }

extern void german_UTF_8_close_env(struct SN_env * z) { SN_close_env(z, 0); }

