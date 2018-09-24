/* This file was generated automatically by the Snowball to ISO C compiler */
/* http://snowballstem.org/ */

#include "header.h"

#ifdef __cplusplus
extern "C" {
#endif
extern int indonesian_ISO_8859_1_stem(struct SN_env * z);
#ifdef __cplusplus
}
#endif
static int r_VOWEL(struct SN_env * z);
static int r_SUFFIX_I_OK(struct SN_env * z);
static int r_SUFFIX_AN_OK(struct SN_env * z);
static int r_SUFFIX_KAN_OK(struct SN_env * z);
static int r_KER(struct SN_env * z);
static int r_remove_suffix(struct SN_env * z);
static int r_remove_second_order_prefix(struct SN_env * z);
static int r_remove_first_order_prefix(struct SN_env * z);
static int r_remove_possessive_pronoun(struct SN_env * z);
static int r_remove_particle(struct SN_env * z);
#ifdef __cplusplus
extern "C" {
#endif


extern struct SN_env * indonesian_ISO_8859_1_create_env(void);
extern void indonesian_ISO_8859_1_close_env(struct SN_env * z);


#ifdef __cplusplus
}
#endif
static const symbol s_0_0[3] = { 'k', 'a', 'h' };
static const symbol s_0_1[3] = { 'l', 'a', 'h' };
static const symbol s_0_2[3] = { 'p', 'u', 'n' };

static const struct among a_0[3] =
{
/*  0 */ { 3, s_0_0, -1, 1, 0},
/*  1 */ { 3, s_0_1, -1, 1, 0},
/*  2 */ { 3, s_0_2, -1, 1, 0}
};

static const symbol s_1_0[3] = { 'n', 'y', 'a' };
static const symbol s_1_1[2] = { 'k', 'u' };
static const symbol s_1_2[2] = { 'm', 'u' };

static const struct among a_1[3] =
{
/*  0 */ { 3, s_1_0, -1, 1, 0},
/*  1 */ { 2, s_1_1, -1, 1, 0},
/*  2 */ { 2, s_1_2, -1, 1, 0}
};

static const symbol s_2_0[1] = { 'i' };
static const symbol s_2_1[2] = { 'a', 'n' };
static const symbol s_2_2[3] = { 'k', 'a', 'n' };

static const struct among a_2[3] =
{
/*  0 */ { 1, s_2_0, -1, 1, r_SUFFIX_I_OK},
/*  1 */ { 2, s_2_1, -1, 1, r_SUFFIX_AN_OK},
/*  2 */ { 3, s_2_2, 1, 1, r_SUFFIX_KAN_OK}
};

static const symbol s_3_0[2] = { 'd', 'i' };
static const symbol s_3_1[2] = { 'k', 'e' };
static const symbol s_3_2[2] = { 'm', 'e' };
static const symbol s_3_3[3] = { 'm', 'e', 'm' };
static const symbol s_3_4[3] = { 'm', 'e', 'n' };
static const symbol s_3_5[4] = { 'm', 'e', 'n', 'g' };
static const symbol s_3_6[4] = { 'm', 'e', 'n', 'y' };
static const symbol s_3_7[3] = { 'p', 'e', 'm' };
static const symbol s_3_8[3] = { 'p', 'e', 'n' };
static const symbol s_3_9[4] = { 'p', 'e', 'n', 'g' };
static const symbol s_3_10[4] = { 'p', 'e', 'n', 'y' };
static const symbol s_3_11[3] = { 't', 'e', 'r' };

static const struct among a_3[12] =
{
/*  0 */ { 2, s_3_0, -1, 1, 0},
/*  1 */ { 2, s_3_1, -1, 2, 0},
/*  2 */ { 2, s_3_2, -1, 1, 0},
/*  3 */ { 3, s_3_3, 2, 5, 0},
/*  4 */ { 3, s_3_4, 2, 1, 0},
/*  5 */ { 4, s_3_5, 4, 1, 0},
/*  6 */ { 4, s_3_6, 4, 3, r_VOWEL},
/*  7 */ { 3, s_3_7, -1, 6, 0},
/*  8 */ { 3, s_3_8, -1, 2, 0},
/*  9 */ { 4, s_3_9, 8, 2, 0},
/* 10 */ { 4, s_3_10, 8, 4, r_VOWEL},
/* 11 */ { 3, s_3_11, -1, 1, 0}
};

static const symbol s_4_0[2] = { 'b', 'e' };
static const symbol s_4_1[7] = { 'b', 'e', 'l', 'a', 'j', 'a', 'r' };
static const symbol s_4_2[3] = { 'b', 'e', 'r' };
static const symbol s_4_3[2] = { 'p', 'e' };
static const symbol s_4_4[7] = { 'p', 'e', 'l', 'a', 'j', 'a', 'r' };
static const symbol s_4_5[3] = { 'p', 'e', 'r' };

static const struct among a_4[6] =
{
/*  0 */ { 2, s_4_0, -1, 3, r_KER},
/*  1 */ { 7, s_4_1, 0, 4, 0},
/*  2 */ { 3, s_4_2, 0, 3, 0},
/*  3 */ { 2, s_4_3, -1, 1, 0},
/*  4 */ { 7, s_4_4, 3, 2, 0},
/*  5 */ { 3, s_4_5, 3, 1, 0}
};

static const unsigned char g_vowel[] = { 17, 65, 16 };

static const symbol s_0[] = { 'e', 'r' };
static const symbol s_1[] = { 's' };
static const symbol s_2[] = { 's' };
static const symbol s_3[] = { 'p' };
static const symbol s_4[] = { 'p' };
static const symbol s_5[] = { 'a', 'j', 'a', 'r' };
static const symbol s_6[] = { 'a', 'j', 'a', 'r' };

static int r_remove_particle(struct SN_env * z) { /* backwardmode */
    z->ket = z->c; /* [, line 51 */
    if (z->c - 2 <= z->lb || (z->p[z->c - 1] != 104 && z->p[z->c - 1] != 110)) return 0; /* substring, line 51 */
    if (!(find_among_b(z, a_0, 3))) return 0;
    z->bra = z->c; /* ], line 51 */
    {   int ret = slice_del(z); /* delete, line 52 */
        if (ret < 0) return ret;
    }
    z->I[0] -= 1; /* $measure -= <integer expression>, line 52 */
    return 1;
}

static int r_remove_possessive_pronoun(struct SN_env * z) { /* backwardmode */
    z->ket = z->c; /* [, line 57 */
    if (z->c - 1 <= z->lb || (z->p[z->c - 1] != 97 && z->p[z->c - 1] != 117)) return 0; /* substring, line 57 */
    if (!(find_among_b(z, a_1, 3))) return 0;
    z->bra = z->c; /* ], line 57 */
    {   int ret = slice_del(z); /* delete, line 58 */
        if (ret < 0) return ret;
    }
    z->I[0] -= 1; /* $measure -= <integer expression>, line 58 */
    return 1;
}

static int r_SUFFIX_KAN_OK(struct SN_env * z) { /* backwardmode */
    /* and, line 85 */
    if (!(z->I[1] != 3)) return 0; /* $(<integer expression> != <integer expression>), line 85 */
    if (!(z->I[1] != 2)) return 0; /* $(<integer expression> != <integer expression>), line 85 */
    return 1;
}

static int r_SUFFIX_AN_OK(struct SN_env * z) { /* backwardmode */
    if (!(z->I[1] != 1)) return 0; /* $(<integer expression> != <integer expression>), line 89 */
    return 1;
}

static int r_SUFFIX_I_OK(struct SN_env * z) { /* backwardmode */
    if (!(z->I[1] <= 2)) return 0; /* $(<integer expression> <= <integer expression>), line 93 */
    {   int m1 = z->l - z->c; (void)m1; /* not, line 128 */
        if (z->c <= z->lb || z->p[z->c - 1] != 's') goto lab0; /* literal, line 128 */
        z->c--;
        return 0;
    lab0:
        z->c = z->l - m1;
    }
    return 1;
}

static int r_remove_suffix(struct SN_env * z) { /* backwardmode */
    z->ket = z->c; /* [, line 132 */
    if (z->c <= z->lb || (z->p[z->c - 1] != 105 && z->p[z->c - 1] != 110)) return 0; /* substring, line 132 */
    if (!(find_among_b(z, a_2, 3))) return 0;
    z->bra = z->c; /* ], line 132 */
    {   int ret = slice_del(z); /* delete, line 134 */
        if (ret < 0) return ret;
    }
    z->I[0] -= 1; /* $measure -= <integer expression>, line 134 */
    return 1;
}

static int r_VOWEL(struct SN_env * z) { /* forwardmode */
    if (in_grouping(z, g_vowel, 97, 117, 0)) return 0; /* grouping vowel, line 141 */
    return 1;
}

static int r_KER(struct SN_env * z) { /* forwardmode */
    if (out_grouping(z, g_vowel, 97, 117, 0)) return 0; /* non vowel, line 143 */
    if (!(eq_s(z, 2, s_0))) return 0; /* literal, line 143 */
    return 1;
}

static int r_remove_first_order_prefix(struct SN_env * z) { /* forwardmode */
    int among_var;
    z->bra = z->c; /* [, line 146 */
    if (z->c + 1 >= z->l || (z->p[z->c + 1] != 105 && z->p[z->c + 1] != 101)) return 0; /* substring, line 146 */
    among_var = find_among(z, a_3, 12);
    if (!(among_var)) return 0;
    z->ket = z->c; /* ], line 146 */
    switch (among_var) { /* among, line 146 */
        case 1:
            {   int ret = slice_del(z); /* delete, line 147 */
                if (ret < 0) return ret;
            }
            z->I[1] = 1; /* $prefix = <integer expression>, line 147 */
            z->I[0] -= 1; /* $measure -= <integer expression>, line 147 */
            break;
        case 2:
            {   int ret = slice_del(z); /* delete, line 148 */
                if (ret < 0) return ret;
            }
            z->I[1] = 3; /* $prefix = <integer expression>, line 148 */
            z->I[0] -= 1; /* $measure -= <integer expression>, line 148 */
            break;
        case 3:
            z->I[1] = 1; /* $prefix = <integer expression>, line 149 */
            {   int ret = slice_from_s(z, 1, s_1); /* <-, line 149 */
                if (ret < 0) return ret;
            }
            z->I[0] -= 1; /* $measure -= <integer expression>, line 149 */
            break;
        case 4:
            z->I[1] = 3; /* $prefix = <integer expression>, line 150 */
            {   int ret = slice_from_s(z, 1, s_2); /* <-, line 150 */
                if (ret < 0) return ret;
            }
            z->I[0] -= 1; /* $measure -= <integer expression>, line 150 */
            break;
        case 5:
            z->I[1] = 1; /* $prefix = <integer expression>, line 151 */
            z->I[0] -= 1; /* $measure -= <integer expression>, line 151 */
            {   int c1 = z->c; /* or, line 151 */
                {   int c2 = z->c; /* and, line 151 */
                    if (in_grouping(z, g_vowel, 97, 117, 0)) goto lab1; /* grouping vowel, line 151 */
                    z->c = c2;
                    {   int ret = slice_from_s(z, 1, s_3); /* <-, line 151 */
                        if (ret < 0) return ret;
                    }
                }
                goto lab0;
            lab1:
                z->c = c1;
                {   int ret = slice_del(z); /* delete, line 151 */
                    if (ret < 0) return ret;
                }
            }
        lab0:
            break;
        case 6:
            z->I[1] = 3; /* $prefix = <integer expression>, line 152 */
            z->I[0] -= 1; /* $measure -= <integer expression>, line 152 */
            {   int c3 = z->c; /* or, line 152 */
                {   int c4 = z->c; /* and, line 152 */
                    if (in_grouping(z, g_vowel, 97, 117, 0)) goto lab3; /* grouping vowel, line 152 */
                    z->c = c4;
                    {   int ret = slice_from_s(z, 1, s_4); /* <-, line 152 */
                        if (ret < 0) return ret;
                    }
                }
                goto lab2;
            lab3:
                z->c = c3;
                {   int ret = slice_del(z); /* delete, line 152 */
                    if (ret < 0) return ret;
                }
            }
        lab2:
            break;
    }
    return 1;
}

static int r_remove_second_order_prefix(struct SN_env * z) { /* forwardmode */
    int among_var;
    z->bra = z->c; /* [, line 162 */
    if (z->c + 1 >= z->l || z->p[z->c + 1] != 101) return 0; /* substring, line 162 */
    among_var = find_among(z, a_4, 6);
    if (!(among_var)) return 0;
    z->ket = z->c; /* ], line 162 */
    switch (among_var) { /* among, line 162 */
        case 1:
            {   int ret = slice_del(z); /* delete, line 163 */
                if (ret < 0) return ret;
            }
            z->I[1] = 2; /* $prefix = <integer expression>, line 163 */
            z->I[0] -= 1; /* $measure -= <integer expression>, line 163 */
            break;
        case 2:
            {   int ret = slice_from_s(z, 4, s_5); /* <-, line 164 */
                if (ret < 0) return ret;
            }
            z->I[0] -= 1; /* $measure -= <integer expression>, line 164 */
            break;
        case 3:
            {   int ret = slice_del(z); /* delete, line 165 */
                if (ret < 0) return ret;
            }
            z->I[1] = 4; /* $prefix = <integer expression>, line 165 */
            z->I[0] -= 1; /* $measure -= <integer expression>, line 165 */
            break;
        case 4:
            {   int ret = slice_from_s(z, 4, s_6); /* <-, line 166 */
                if (ret < 0) return ret;
            }
            z->I[1] = 4; /* $prefix = <integer expression>, line 166 */
            z->I[0] -= 1; /* $measure -= <integer expression>, line 166 */
            break;
    }
    return 1;
}

extern int indonesian_ISO_8859_1_stem(struct SN_env * z) { /* forwardmode */
    z->I[0] = 0; /* $measure = <integer expression>, line 172 */
    {   int c1 = z->c; /* do, line 173 */
        while(1) { /* repeat, line 173 */
            int c2 = z->c;
            {    /* gopast */ /* grouping vowel, line 173 */
                int ret = out_grouping(z, g_vowel, 97, 117, 1);
                if (ret < 0) goto lab1;
                z->c += ret;
            }
            z->I[0] += 1; /* $measure += <integer expression>, line 173 */
            continue;
        lab1:
            z->c = c2;
            break;
        }
        z->c = c1;
    }
    if (!(z->I[0] > 2)) return 0; /* $(<integer expression> > <integer expression>), line 174 */
    z->I[1] = 0; /* $prefix = <integer expression>, line 175 */
    z->lb = z->c; z->c = z->l; /* backwards, line 176 */

    {   int m3 = z->l - z->c; (void)m3; /* do, line 177 */
        {   int ret = r_remove_particle(z); /* call remove_particle, line 177 */
            if (ret == 0) goto lab2;
            if (ret < 0) return ret;
        }
    lab2:
        z->c = z->l - m3;
    }
    if (!(z->I[0] > 2)) return 0; /* $(<integer expression> > <integer expression>), line 178 */
    {   int m4 = z->l - z->c; (void)m4; /* do, line 179 */
        {   int ret = r_remove_possessive_pronoun(z); /* call remove_possessive_pronoun, line 179 */
            if (ret == 0) goto lab3;
            if (ret < 0) return ret;
        }
    lab3:
        z->c = z->l - m4;
    }
    z->c = z->lb;
    if (!(z->I[0] > 2)) return 0; /* $(<integer expression> > <integer expression>), line 181 */
    {   int c5 = z->c; /* or, line 188 */
        {   int c_test6 = z->c; /* test, line 182 */
            {   int ret = r_remove_first_order_prefix(z); /* call remove_first_order_prefix, line 183 */
                if (ret == 0) goto lab5;
                if (ret < 0) return ret;
            }
            {   int c7 = z->c; /* do, line 184 */
                {   int c_test8 = z->c; /* test, line 185 */
                    if (!(z->I[0] > 2)) goto lab6; /* $(<integer expression> > <integer expression>), line 185 */
                    z->lb = z->c; z->c = z->l; /* backwards, line 185 */

                    {   int ret = r_remove_suffix(z); /* call remove_suffix, line 185 */
                        if (ret == 0) goto lab6;
                        if (ret < 0) return ret;
                    }
                    z->c = z->lb;
                    z->c = c_test8;
                }
                if (!(z->I[0] > 2)) goto lab6; /* $(<integer expression> > <integer expression>), line 186 */
                {   int ret = r_remove_second_order_prefix(z); /* call remove_second_order_prefix, line 186 */
                    if (ret == 0) goto lab6;
                    if (ret < 0) return ret;
                }
            lab6:
                z->c = c7;
            }
            z->c = c_test6;
        }
        goto lab4;
    lab5:
        z->c = c5;
        {   int c9 = z->c; /* do, line 189 */
            {   int ret = r_remove_second_order_prefix(z); /* call remove_second_order_prefix, line 189 */
                if (ret == 0) goto lab7;
                if (ret < 0) return ret;
            }
        lab7:
            z->c = c9;
        }
        {   int c10 = z->c; /* do, line 190 */
            if (!(z->I[0] > 2)) goto lab8; /* $(<integer expression> > <integer expression>), line 190 */
            z->lb = z->c; z->c = z->l; /* backwards, line 190 */

            {   int ret = r_remove_suffix(z); /* call remove_suffix, line 190 */
                if (ret == 0) goto lab8;
                if (ret < 0) return ret;
            }
            z->c = z->lb;
        lab8:
            z->c = c10;
        }
    }
lab4:
    return 1;
}

extern struct SN_env * indonesian_ISO_8859_1_create_env(void) { return SN_create_env(0, 2, 0); }

extern void indonesian_ISO_8859_1_close_env(struct SN_env * z) { SN_close_env(z, 0); }

