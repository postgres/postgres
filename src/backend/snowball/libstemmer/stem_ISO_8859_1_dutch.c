/* This file was generated automatically by the Snowball to ISO C compiler */
/* http://snowballstem.org/ */

#include "header.h"

#ifdef __cplusplus
extern "C" {
#endif
extern int dutch_ISO_8859_1_stem(struct SN_env * z);
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
#ifdef __cplusplus
extern "C" {
#endif


extern struct SN_env * dutch_ISO_8859_1_create_env(void);
extern void dutch_ISO_8859_1_close_env(struct SN_env * z);


#ifdef __cplusplus
}
#endif
static const symbol s_0_1[1] = { 0xE1 };
static const symbol s_0_2[1] = { 0xE4 };
static const symbol s_0_3[1] = { 0xE9 };
static const symbol s_0_4[1] = { 0xEB };
static const symbol s_0_5[1] = { 0xED };
static const symbol s_0_6[1] = { 0xEF };
static const symbol s_0_7[1] = { 0xF3 };
static const symbol s_0_8[1] = { 0xF6 };
static const symbol s_0_9[1] = { 0xFA };
static const symbol s_0_10[1] = { 0xFC };

static const struct among a_0[11] =
{
/*  0 */ { 0, 0, -1, 6, 0},
/*  1 */ { 1, s_0_1, 0, 1, 0},
/*  2 */ { 1, s_0_2, 0, 1, 0},
/*  3 */ { 1, s_0_3, 0, 2, 0},
/*  4 */ { 1, s_0_4, 0, 2, 0},
/*  5 */ { 1, s_0_5, 0, 3, 0},
/*  6 */ { 1, s_0_6, 0, 3, 0},
/*  7 */ { 1, s_0_7, 0, 4, 0},
/*  8 */ { 1, s_0_8, 0, 4, 0},
/*  9 */ { 1, s_0_9, 0, 5, 0},
/* 10 */ { 1, s_0_10, 0, 5, 0}
};

static const symbol s_1_1[1] = { 'I' };
static const symbol s_1_2[1] = { 'Y' };

static const struct among a_1[3] =
{
/*  0 */ { 0, 0, -1, 3, 0},
/*  1 */ { 1, s_1_1, 0, 2, 0},
/*  2 */ { 1, s_1_2, 0, 1, 0}
};

static const symbol s_2_0[2] = { 'd', 'd' };
static const symbol s_2_1[2] = { 'k', 'k' };
static const symbol s_2_2[2] = { 't', 't' };

static const struct among a_2[3] =
{
/*  0 */ { 2, s_2_0, -1, -1, 0},
/*  1 */ { 2, s_2_1, -1, -1, 0},
/*  2 */ { 2, s_2_2, -1, -1, 0}
};

static const symbol s_3_0[3] = { 'e', 'n', 'e' };
static const symbol s_3_1[2] = { 's', 'e' };
static const symbol s_3_2[2] = { 'e', 'n' };
static const symbol s_3_3[5] = { 'h', 'e', 'd', 'e', 'n' };
static const symbol s_3_4[1] = { 's' };

static const struct among a_3[5] =
{
/*  0 */ { 3, s_3_0, -1, 2, 0},
/*  1 */ { 2, s_3_1, -1, 3, 0},
/*  2 */ { 2, s_3_2, -1, 2, 0},
/*  3 */ { 5, s_3_3, 2, 1, 0},
/*  4 */ { 1, s_3_4, -1, 3, 0}
};

static const symbol s_4_0[3] = { 'e', 'n', 'd' };
static const symbol s_4_1[2] = { 'i', 'g' };
static const symbol s_4_2[3] = { 'i', 'n', 'g' };
static const symbol s_4_3[4] = { 'l', 'i', 'j', 'k' };
static const symbol s_4_4[4] = { 'b', 'a', 'a', 'r' };
static const symbol s_4_5[3] = { 'b', 'a', 'r' };

static const struct among a_4[6] =
{
/*  0 */ { 3, s_4_0, -1, 1, 0},
/*  1 */ { 2, s_4_1, -1, 2, 0},
/*  2 */ { 3, s_4_2, -1, 1, 0},
/*  3 */ { 4, s_4_3, -1, 3, 0},
/*  4 */ { 4, s_4_4, -1, 4, 0},
/*  5 */ { 3, s_4_5, -1, 5, 0}
};

static const symbol s_5_0[2] = { 'a', 'a' };
static const symbol s_5_1[2] = { 'e', 'e' };
static const symbol s_5_2[2] = { 'o', 'o' };
static const symbol s_5_3[2] = { 'u', 'u' };

static const struct among a_5[4] =
{
/*  0 */ { 2, s_5_0, -1, -1, 0},
/*  1 */ { 2, s_5_1, -1, -1, 0},
/*  2 */ { 2, s_5_2, -1, -1, 0},
/*  3 */ { 2, s_5_3, -1, -1, 0}
};

static const unsigned char g_v[] = { 17, 65, 16, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 128 };

static const unsigned char g_v_I[] = { 1, 0, 0, 17, 65, 16, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 128 };

static const unsigned char g_v_j[] = { 17, 67, 16, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 128 };

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

static int r_prelude(struct SN_env * z) { /* forwardmode */
    int among_var;
    {   int c_test1 = z->c; /* test, line 42 */
        while(1) { /* repeat, line 42 */
            int c2 = z->c;
            z->bra = z->c; /* [, line 43 */
            if (z->c >= z->l || z->p[z->c + 0] >> 5 != 7 || !((340306450 >> (z->p[z->c + 0] & 0x1f)) & 1)) among_var = 6; else /* substring, line 43 */
            among_var = find_among(z, a_0, 11);
            if (!(among_var)) goto lab0;
            z->ket = z->c; /* ], line 43 */
            switch (among_var) { /* among, line 43 */
                case 1:
                    {   int ret = slice_from_s(z, 1, s_0); /* <-, line 45 */
                        if (ret < 0) return ret;
                    }
                    break;
                case 2:
                    {   int ret = slice_from_s(z, 1, s_1); /* <-, line 47 */
                        if (ret < 0) return ret;
                    }
                    break;
                case 3:
                    {   int ret = slice_from_s(z, 1, s_2); /* <-, line 49 */
                        if (ret < 0) return ret;
                    }
                    break;
                case 4:
                    {   int ret = slice_from_s(z, 1, s_3); /* <-, line 51 */
                        if (ret < 0) return ret;
                    }
                    break;
                case 5:
                    {   int ret = slice_from_s(z, 1, s_4); /* <-, line 53 */
                        if (ret < 0) return ret;
                    }
                    break;
                case 6:
                    if (z->c >= z->l) goto lab0;
                    z->c++; /* next, line 54 */
                    break;
            }
            continue;
        lab0:
            z->c = c2;
            break;
        }
        z->c = c_test1;
    }
    {   int c3 = z->c; /* try, line 57 */
        z->bra = z->c; /* [, line 57 */
        if (z->c == z->l || z->p[z->c] != 'y') { z->c = c3; goto lab1; } /* literal, line 57 */
        z->c++;
        z->ket = z->c; /* ], line 57 */
        {   int ret = slice_from_s(z, 1, s_5); /* <-, line 57 */
            if (ret < 0) return ret;
        }
    lab1:
        ;
    }
    while(1) { /* repeat, line 58 */
        int c4 = z->c;
        while(1) { /* goto, line 58 */
            int c5 = z->c;
            if (in_grouping(z, g_v, 97, 232, 0)) goto lab3; /* grouping v, line 59 */
            z->bra = z->c; /* [, line 59 */
            {   int c6 = z->c; /* or, line 59 */
                if (z->c == z->l || z->p[z->c] != 'i') goto lab5; /* literal, line 59 */
                z->c++;
                z->ket = z->c; /* ], line 59 */
                if (in_grouping(z, g_v, 97, 232, 0)) goto lab5; /* grouping v, line 59 */
                {   int ret = slice_from_s(z, 1, s_6); /* <-, line 59 */
                    if (ret < 0) return ret;
                }
                goto lab4;
            lab5:
                z->c = c6;
                if (z->c == z->l || z->p[z->c] != 'y') goto lab3; /* literal, line 60 */
                z->c++;
                z->ket = z->c; /* ], line 60 */
                {   int ret = slice_from_s(z, 1, s_7); /* <-, line 60 */
                    if (ret < 0) return ret;
                }
            }
        lab4:
            z->c = c5;
            break;
        lab3:
            z->c = c5;
            if (z->c >= z->l) goto lab2;
            z->c++; /* goto, line 58 */
        }
        continue;
    lab2:
        z->c = c4;
        break;
    }
    return 1;
}

static int r_mark_regions(struct SN_env * z) { /* forwardmode */
    z->I[0] = z->l; /* $p1 = <integer expression>, line 66 */
    z->I[1] = z->l; /* $p2 = <integer expression>, line 67 */
    {    /* gopast */ /* grouping v, line 69 */
        int ret = out_grouping(z, g_v, 97, 232, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    {    /* gopast */ /* non v, line 69 */
        int ret = in_grouping(z, g_v, 97, 232, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    z->I[0] = z->c; /* setmark p1, line 69 */
    /* try, line 70 */
    if (!(z->I[0] < 3)) goto lab0; /* $(<integer expression> < <integer expression>), line 70 */
    z->I[0] = 3; /* $p1 = <integer expression>, line 70 */
lab0:
    {    /* gopast */ /* grouping v, line 71 */
        int ret = out_grouping(z, g_v, 97, 232, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    {    /* gopast */ /* non v, line 71 */
        int ret = in_grouping(z, g_v, 97, 232, 1);
        if (ret < 0) return 0;
        z->c += ret;
    }
    z->I[1] = z->c; /* setmark p2, line 71 */
    return 1;
}

static int r_postlude(struct SN_env * z) { /* forwardmode */
    int among_var;
    while(1) { /* repeat, line 75 */
        int c1 = z->c;
        z->bra = z->c; /* [, line 77 */
        if (z->c >= z->l || (z->p[z->c + 0] != 73 && z->p[z->c + 0] != 89)) among_var = 3; else /* substring, line 77 */
        among_var = find_among(z, a_1, 3);
        if (!(among_var)) goto lab0;
        z->ket = z->c; /* ], line 77 */
        switch (among_var) { /* among, line 77 */
            case 1:
                {   int ret = slice_from_s(z, 1, s_8); /* <-, line 78 */
                    if (ret < 0) return ret;
                }
                break;
            case 2:
                {   int ret = slice_from_s(z, 1, s_9); /* <-, line 79 */
                    if (ret < 0) return ret;
                }
                break;
            case 3:
                if (z->c >= z->l) goto lab0;
                z->c++; /* next, line 80 */
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
    if (!(z->I[0] <= z->c)) return 0; /* $(<integer expression> <= <integer expression>), line 87 */
    return 1;
}

static int r_R2(struct SN_env * z) { /* backwardmode */
    if (!(z->I[1] <= z->c)) return 0; /* $(<integer expression> <= <integer expression>), line 88 */
    return 1;
}

static int r_undouble(struct SN_env * z) { /* backwardmode */
    {   int m_test1 = z->l - z->c; /* test, line 91 */
        if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((1050640 >> (z->p[z->c - 1] & 0x1f)) & 1)) return 0; /* among, line 91 */
        if (!(find_among_b(z, a_2, 3))) return 0;
        z->c = z->l - m_test1;
    }
    z->ket = z->c; /* [, line 91 */
    if (z->c <= z->lb) return 0;
    z->c--; /* next, line 91 */
    z->bra = z->c; /* ], line 91 */
    {   int ret = slice_del(z); /* delete, line 91 */
        if (ret < 0) return ret;
    }
    return 1;
}

static int r_e_ending(struct SN_env * z) { /* backwardmode */
    z->B[0] = 0; /* unset e_found, line 95 */
    z->ket = z->c; /* [, line 96 */
    if (z->c <= z->lb || z->p[z->c - 1] != 'e') return 0; /* literal, line 96 */
    z->c--;
    z->bra = z->c; /* ], line 96 */
    {   int ret = r_R1(z); /* call R1, line 96 */
        if (ret <= 0) return ret;
    }
    {   int m_test1 = z->l - z->c; /* test, line 96 */
        if (out_grouping_b(z, g_v, 97, 232, 0)) return 0; /* non v, line 96 */
        z->c = z->l - m_test1;
    }
    {   int ret = slice_del(z); /* delete, line 96 */
        if (ret < 0) return ret;
    }
    z->B[0] = 1; /* set e_found, line 97 */
    {   int ret = r_undouble(z); /* call undouble, line 98 */
        if (ret <= 0) return ret;
    }
    return 1;
}

static int r_en_ending(struct SN_env * z) { /* backwardmode */
    {   int ret = r_R1(z); /* call R1, line 102 */
        if (ret <= 0) return ret;
    }
    {   int m1 = z->l - z->c; (void)m1; /* and, line 102 */
        if (out_grouping_b(z, g_v, 97, 232, 0)) return 0; /* non v, line 102 */
        z->c = z->l - m1;
        {   int m2 = z->l - z->c; (void)m2; /* not, line 102 */
            if (!(eq_s_b(z, 3, s_10))) goto lab0; /* literal, line 102 */
            return 0;
        lab0:
            z->c = z->l - m2;
        }
    }
    {   int ret = slice_del(z); /* delete, line 102 */
        if (ret < 0) return ret;
    }
    {   int ret = r_undouble(z); /* call undouble, line 103 */
        if (ret <= 0) return ret;
    }
    return 1;
}

static int r_standard_suffix(struct SN_env * z) { /* backwardmode */
    int among_var;
    {   int m1 = z->l - z->c; (void)m1; /* do, line 107 */
        z->ket = z->c; /* [, line 108 */
        if (z->c <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((540704 >> (z->p[z->c - 1] & 0x1f)) & 1)) goto lab0; /* substring, line 108 */
        among_var = find_among_b(z, a_3, 5);
        if (!(among_var)) goto lab0;
        z->bra = z->c; /* ], line 108 */
        switch (among_var) { /* among, line 108 */
            case 1:
                {   int ret = r_R1(z); /* call R1, line 110 */
                    if (ret == 0) goto lab0;
                    if (ret < 0) return ret;
                }
                {   int ret = slice_from_s(z, 4, s_11); /* <-, line 110 */
                    if (ret < 0) return ret;
                }
                break;
            case 2:
                {   int ret = r_en_ending(z); /* call en_ending, line 113 */
                    if (ret == 0) goto lab0;
                    if (ret < 0) return ret;
                }
                break;
            case 3:
                {   int ret = r_R1(z); /* call R1, line 116 */
                    if (ret == 0) goto lab0;
                    if (ret < 0) return ret;
                }
                if (out_grouping_b(z, g_v_j, 97, 232, 0)) goto lab0; /* non v_j, line 116 */
                {   int ret = slice_del(z); /* delete, line 116 */
                    if (ret < 0) return ret;
                }
                break;
        }
    lab0:
        z->c = z->l - m1;
    }
    {   int m2 = z->l - z->c; (void)m2; /* do, line 120 */
        {   int ret = r_e_ending(z); /* call e_ending, line 120 */
            if (ret == 0) goto lab1;
            if (ret < 0) return ret;
        }
    lab1:
        z->c = z->l - m2;
    }
    {   int m3 = z->l - z->c; (void)m3; /* do, line 122 */
        z->ket = z->c; /* [, line 122 */
        if (!(eq_s_b(z, 4, s_12))) goto lab2; /* literal, line 122 */
        z->bra = z->c; /* ], line 122 */
        {   int ret = r_R2(z); /* call R2, line 122 */
            if (ret == 0) goto lab2;
            if (ret < 0) return ret;
        }
        {   int m4 = z->l - z->c; (void)m4; /* not, line 122 */
            if (z->c <= z->lb || z->p[z->c - 1] != 'c') goto lab3; /* literal, line 122 */
            z->c--;
            goto lab2;
        lab3:
            z->c = z->l - m4;
        }
        {   int ret = slice_del(z); /* delete, line 122 */
            if (ret < 0) return ret;
        }
        z->ket = z->c; /* [, line 123 */
        if (!(eq_s_b(z, 2, s_13))) goto lab2; /* literal, line 123 */
        z->bra = z->c; /* ], line 123 */
        {   int ret = r_en_ending(z); /* call en_ending, line 123 */
            if (ret == 0) goto lab2;
            if (ret < 0) return ret;
        }
    lab2:
        z->c = z->l - m3;
    }
    {   int m5 = z->l - z->c; (void)m5; /* do, line 126 */
        z->ket = z->c; /* [, line 127 */
        if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((264336 >> (z->p[z->c - 1] & 0x1f)) & 1)) goto lab4; /* substring, line 127 */
        among_var = find_among_b(z, a_4, 6);
        if (!(among_var)) goto lab4;
        z->bra = z->c; /* ], line 127 */
        switch (among_var) { /* among, line 127 */
            case 1:
                {   int ret = r_R2(z); /* call R2, line 129 */
                    if (ret == 0) goto lab4;
                    if (ret < 0) return ret;
                }
                {   int ret = slice_del(z); /* delete, line 129 */
                    if (ret < 0) return ret;
                }
                {   int m6 = z->l - z->c; (void)m6; /* or, line 130 */
                    z->ket = z->c; /* [, line 130 */
                    if (!(eq_s_b(z, 2, s_14))) goto lab6; /* literal, line 130 */
                    z->bra = z->c; /* ], line 130 */
                    {   int ret = r_R2(z); /* call R2, line 130 */
                        if (ret == 0) goto lab6;
                        if (ret < 0) return ret;
                    }
                    {   int m7 = z->l - z->c; (void)m7; /* not, line 130 */
                        if (z->c <= z->lb || z->p[z->c - 1] != 'e') goto lab7; /* literal, line 130 */
                        z->c--;
                        goto lab6;
                    lab7:
                        z->c = z->l - m7;
                    }
                    {   int ret = slice_del(z); /* delete, line 130 */
                        if (ret < 0) return ret;
                    }
                    goto lab5;
                lab6:
                    z->c = z->l - m6;
                    {   int ret = r_undouble(z); /* call undouble, line 130 */
                        if (ret == 0) goto lab4;
                        if (ret < 0) return ret;
                    }
                }
            lab5:
                break;
            case 2:
                {   int ret = r_R2(z); /* call R2, line 133 */
                    if (ret == 0) goto lab4;
                    if (ret < 0) return ret;
                }
                {   int m8 = z->l - z->c; (void)m8; /* not, line 133 */
                    if (z->c <= z->lb || z->p[z->c - 1] != 'e') goto lab8; /* literal, line 133 */
                    z->c--;
                    goto lab4;
                lab8:
                    z->c = z->l - m8;
                }
                {   int ret = slice_del(z); /* delete, line 133 */
                    if (ret < 0) return ret;
                }
                break;
            case 3:
                {   int ret = r_R2(z); /* call R2, line 136 */
                    if (ret == 0) goto lab4;
                    if (ret < 0) return ret;
                }
                {   int ret = slice_del(z); /* delete, line 136 */
                    if (ret < 0) return ret;
                }
                {   int ret = r_e_ending(z); /* call e_ending, line 136 */
                    if (ret == 0) goto lab4;
                    if (ret < 0) return ret;
                }
                break;
            case 4:
                {   int ret = r_R2(z); /* call R2, line 139 */
                    if (ret == 0) goto lab4;
                    if (ret < 0) return ret;
                }
                {   int ret = slice_del(z); /* delete, line 139 */
                    if (ret < 0) return ret;
                }
                break;
            case 5:
                {   int ret = r_R2(z); /* call R2, line 142 */
                    if (ret == 0) goto lab4;
                    if (ret < 0) return ret;
                }
                if (!(z->B[0])) goto lab4; /* Boolean test e_found, line 142 */
                {   int ret = slice_del(z); /* delete, line 142 */
                    if (ret < 0) return ret;
                }
                break;
        }
    lab4:
        z->c = z->l - m5;
    }
    {   int m9 = z->l - z->c; (void)m9; /* do, line 146 */
        if (out_grouping_b(z, g_v_I, 73, 232, 0)) goto lab9; /* non v_I, line 147 */
        {   int m_test10 = z->l - z->c; /* test, line 148 */
            if (z->c - 1 <= z->lb || z->p[z->c - 1] >> 5 != 3 || !((2129954 >> (z->p[z->c - 1] & 0x1f)) & 1)) goto lab9; /* among, line 149 */
            if (!(find_among_b(z, a_5, 4))) goto lab9;
            if (out_grouping_b(z, g_v, 97, 232, 0)) goto lab9; /* non v, line 150 */
            z->c = z->l - m_test10;
        }
        z->ket = z->c; /* [, line 152 */
        if (z->c <= z->lb) goto lab9;
        z->c--; /* next, line 152 */
        z->bra = z->c; /* ], line 152 */
        {   int ret = slice_del(z); /* delete, line 152 */
            if (ret < 0) return ret;
        }
    lab9:
        z->c = z->l - m9;
    }
    return 1;
}

extern int dutch_ISO_8859_1_stem(struct SN_env * z) { /* forwardmode */
    {   int c1 = z->c; /* do, line 159 */
        {   int ret = r_prelude(z); /* call prelude, line 159 */
            if (ret == 0) goto lab0;
            if (ret < 0) return ret;
        }
    lab0:
        z->c = c1;
    }
    {   int c2 = z->c; /* do, line 160 */
        {   int ret = r_mark_regions(z); /* call mark_regions, line 160 */
            if (ret == 0) goto lab1;
            if (ret < 0) return ret;
        }
    lab1:
        z->c = c2;
    }
    z->lb = z->c; z->c = z->l; /* backwards, line 161 */

    /* do, line 162 */
    {   int ret = r_standard_suffix(z); /* call standard_suffix, line 162 */
        if (ret == 0) goto lab2;
        if (ret < 0) return ret;
    }
lab2:
    z->c = z->lb;
    {   int c3 = z->c; /* do, line 163 */
        {   int ret = r_postlude(z); /* call postlude, line 163 */
            if (ret == 0) goto lab3;
            if (ret < 0) return ret;
        }
    lab3:
        z->c = c3;
    }
    return 1;
}

extern struct SN_env * dutch_ISO_8859_1_create_env(void) { return SN_create_env(0, 2, 1); }

extern void dutch_ISO_8859_1_close_env(struct SN_env * z) { SN_close_env(z, 0); }

