
/* This file was generated automatically by the Snowball to ANSI C compiler */

#include "header.h"

extern int russian_stem(struct SN_env * z);
static int r_tidy_up(struct SN_env * z);
static int r_derivational(struct SN_env * z);
static int r_noun(struct SN_env * z);
static int r_verb(struct SN_env * z);
static int r_reflexive(struct SN_env * z);
static int r_adjectival(struct SN_env * z);
static int r_adjective(struct SN_env * z);
static int r_perfective_gerund(struct SN_env * z);
static int r_R2(struct SN_env * z);
static int r_mark_regions(struct SN_env * z);

extern struct SN_env * russian_create_env(void);
extern void russian_close_env(struct SN_env * z);

static symbol s_0_0[3] = { 215, 219, 201 };
static symbol s_0_1[4] = { 201, 215, 219, 201 };
static symbol s_0_2[4] = { 217, 215, 219, 201 };
static symbol s_0_3[1] = { 215 };
static symbol s_0_4[2] = { 201, 215 };
static symbol s_0_5[2] = { 217, 215 };
static symbol s_0_6[5] = { 215, 219, 201, 211, 216 };
static symbol s_0_7[6] = { 201, 215, 219, 201, 211, 216 };
static symbol s_0_8[6] = { 217, 215, 219, 201, 211, 216 };

static struct among a_0[9] =
{
/*  0 */ { 3, s_0_0, -1, 1, 0},
/*  1 */ { 4, s_0_1, 0, 2, 0},
/*  2 */ { 4, s_0_2, 0, 2, 0},
/*  3 */ { 1, s_0_3, -1, 1, 0},
/*  4 */ { 2, s_0_4, 3, 2, 0},
/*  5 */ { 2, s_0_5, 3, 2, 0},
/*  6 */ { 5, s_0_6, -1, 1, 0},
/*  7 */ { 6, s_0_7, 6, 2, 0},
/*  8 */ { 6, s_0_8, 6, 2, 0}
};

static symbol s_1_0[2] = { 192, 192 };
static symbol s_1_1[2] = { 197, 192 };
static symbol s_1_2[2] = { 207, 192 };
static symbol s_1_3[2] = { 213, 192 };
static symbol s_1_4[2] = { 197, 197 };
static symbol s_1_5[2] = { 201, 197 };
static symbol s_1_6[2] = { 207, 197 };
static symbol s_1_7[2] = { 217, 197 };
static symbol s_1_8[2] = { 201, 200 };
static symbol s_1_9[2] = { 217, 200 };
static symbol s_1_10[3] = { 201, 205, 201 };
static symbol s_1_11[3] = { 217, 205, 201 };
static symbol s_1_12[2] = { 197, 202 };
static symbol s_1_13[2] = { 201, 202 };
static symbol s_1_14[2] = { 207, 202 };
static symbol s_1_15[2] = { 217, 202 };
static symbol s_1_16[2] = { 197, 205 };
static symbol s_1_17[2] = { 201, 205 };
static symbol s_1_18[2] = { 207, 205 };
static symbol s_1_19[2] = { 217, 205 };
static symbol s_1_20[3] = { 197, 199, 207 };
static symbol s_1_21[3] = { 207, 199, 207 };
static symbol s_1_22[2] = { 193, 209 };
static symbol s_1_23[2] = { 209, 209 };
static symbol s_1_24[3] = { 197, 205, 213 };
static symbol s_1_25[3] = { 207, 205, 213 };

static struct among a_1[26] =
{
/*  0 */ { 2, s_1_0, -1, 1, 0},
/*  1 */ { 2, s_1_1, -1, 1, 0},
/*  2 */ { 2, s_1_2, -1, 1, 0},
/*  3 */ { 2, s_1_3, -1, 1, 0},
/*  4 */ { 2, s_1_4, -1, 1, 0},
/*  5 */ { 2, s_1_5, -1, 1, 0},
/*  6 */ { 2, s_1_6, -1, 1, 0},
/*  7 */ { 2, s_1_7, -1, 1, 0},
/*  8 */ { 2, s_1_8, -1, 1, 0},
/*  9 */ { 2, s_1_9, -1, 1, 0},
/* 10 */ { 3, s_1_10, -1, 1, 0},
/* 11 */ { 3, s_1_11, -1, 1, 0},
/* 12 */ { 2, s_1_12, -1, 1, 0},
/* 13 */ { 2, s_1_13, -1, 1, 0},
/* 14 */ { 2, s_1_14, -1, 1, 0},
/* 15 */ { 2, s_1_15, -1, 1, 0},
/* 16 */ { 2, s_1_16, -1, 1, 0},
/* 17 */ { 2, s_1_17, -1, 1, 0},
/* 18 */ { 2, s_1_18, -1, 1, 0},
/* 19 */ { 2, s_1_19, -1, 1, 0},
/* 20 */ { 3, s_1_20, -1, 1, 0},
/* 21 */ { 3, s_1_21, -1, 1, 0},
/* 22 */ { 2, s_1_22, -1, 1, 0},
/* 23 */ { 2, s_1_23, -1, 1, 0},
/* 24 */ { 3, s_1_24, -1, 1, 0},
/* 25 */ { 3, s_1_25, -1, 1, 0}
};

static symbol s_2_0[2] = { 197, 205 };
static symbol s_2_1[2] = { 206, 206 };
static symbol s_2_2[2] = { 215, 219 };
static symbol s_2_3[3] = { 201, 215, 219 };
static symbol s_2_4[3] = { 217, 215, 219 };
static symbol s_2_5[1] = { 221 };
static symbol s_2_6[2] = { 192, 221 };
static symbol s_2_7[3] = { 213, 192, 221 };

static struct among a_2[8] =
{
/*  0 */ { 2, s_2_0, -1, 1, 0},
/*  1 */ { 2, s_2_1, -1, 1, 0},
/*  2 */ { 2, s_2_2, -1, 1, 0},
/*  3 */ { 3, s_2_3, 2, 2, 0},
/*  4 */ { 3, s_2_4, 2, 2, 0},
/*  5 */ { 1, s_2_5, -1, 1, 0},
/*  6 */ { 2, s_2_6, 5, 1, 0},
/*  7 */ { 3, s_2_7, 6, 2, 0}
};

static symbol s_3_0[2] = { 211, 209 };
static symbol s_3_1[2] = { 211, 216 };

static struct among a_3[2] =
{
/*  0 */ { 2, s_3_0, -1, 1, 0},
/*  1 */ { 2, s_3_1, -1, 1, 0}
};

static symbol s_4_0[1] = { 192 };
static symbol s_4_1[2] = { 213, 192 };
static symbol s_4_2[2] = { 204, 193 };
static symbol s_4_3[3] = { 201, 204, 193 };
static symbol s_4_4[3] = { 217, 204, 193 };
static symbol s_4_5[2] = { 206, 193 };
static symbol s_4_6[3] = { 197, 206, 193 };
static symbol s_4_7[3] = { 197, 212, 197 };
static symbol s_4_8[3] = { 201, 212, 197 };
static symbol s_4_9[3] = { 202, 212, 197 };
static symbol s_4_10[4] = { 197, 202, 212, 197 };
static symbol s_4_11[4] = { 213, 202, 212, 197 };
static symbol s_4_12[2] = { 204, 201 };
static symbol s_4_13[3] = { 201, 204, 201 };
static symbol s_4_14[3] = { 217, 204, 201 };
static symbol s_4_15[1] = { 202 };
static symbol s_4_16[2] = { 197, 202 };
static symbol s_4_17[2] = { 213, 202 };
static symbol s_4_18[1] = { 204 };
static symbol s_4_19[2] = { 201, 204 };
static symbol s_4_20[2] = { 217, 204 };
static symbol s_4_21[2] = { 197, 205 };
static symbol s_4_22[2] = { 201, 205 };
static symbol s_4_23[2] = { 217, 205 };
static symbol s_4_24[1] = { 206 };
static symbol s_4_25[2] = { 197, 206 };
static symbol s_4_26[2] = { 204, 207 };
static symbol s_4_27[3] = { 201, 204, 207 };
static symbol s_4_28[3] = { 217, 204, 207 };
static symbol s_4_29[2] = { 206, 207 };
static symbol s_4_30[3] = { 197, 206, 207 };
static symbol s_4_31[3] = { 206, 206, 207 };
static symbol s_4_32[2] = { 192, 212 };
static symbol s_4_33[3] = { 213, 192, 212 };
static symbol s_4_34[2] = { 197, 212 };
static symbol s_4_35[3] = { 213, 197, 212 };
static symbol s_4_36[2] = { 201, 212 };
static symbol s_4_37[2] = { 209, 212 };
static symbol s_4_38[2] = { 217, 212 };
static symbol s_4_39[2] = { 212, 216 };
static symbol s_4_40[3] = { 201, 212, 216 };
static symbol s_4_41[3] = { 217, 212, 216 };
static symbol s_4_42[3] = { 197, 219, 216 };
static symbol s_4_43[3] = { 201, 219, 216 };
static symbol s_4_44[2] = { 206, 217 };
static symbol s_4_45[3] = { 197, 206, 217 };

static struct among a_4[46] =
{
/*  0 */ { 1, s_4_0, -1, 2, 0},
/*  1 */ { 2, s_4_1, 0, 2, 0},
/*  2 */ { 2, s_4_2, -1, 1, 0},
/*  3 */ { 3, s_4_3, 2, 2, 0},
/*  4 */ { 3, s_4_4, 2, 2, 0},
/*  5 */ { 2, s_4_5, -1, 1, 0},
/*  6 */ { 3, s_4_6, 5, 2, 0},
/*  7 */ { 3, s_4_7, -1, 1, 0},
/*  8 */ { 3, s_4_8, -1, 2, 0},
/*  9 */ { 3, s_4_9, -1, 1, 0},
/* 10 */ { 4, s_4_10, 9, 2, 0},
/* 11 */ { 4, s_4_11, 9, 2, 0},
/* 12 */ { 2, s_4_12, -1, 1, 0},
/* 13 */ { 3, s_4_13, 12, 2, 0},
/* 14 */ { 3, s_4_14, 12, 2, 0},
/* 15 */ { 1, s_4_15, -1, 1, 0},
/* 16 */ { 2, s_4_16, 15, 2, 0},
/* 17 */ { 2, s_4_17, 15, 2, 0},
/* 18 */ { 1, s_4_18, -1, 1, 0},
/* 19 */ { 2, s_4_19, 18, 2, 0},
/* 20 */ { 2, s_4_20, 18, 2, 0},
/* 21 */ { 2, s_4_21, -1, 1, 0},
/* 22 */ { 2, s_4_22, -1, 2, 0},
/* 23 */ { 2, s_4_23, -1, 2, 0},
/* 24 */ { 1, s_4_24, -1, 1, 0},
/* 25 */ { 2, s_4_25, 24, 2, 0},
/* 26 */ { 2, s_4_26, -1, 1, 0},
/* 27 */ { 3, s_4_27, 26, 2, 0},
/* 28 */ { 3, s_4_28, 26, 2, 0},
/* 29 */ { 2, s_4_29, -1, 1, 0},
/* 30 */ { 3, s_4_30, 29, 2, 0},
/* 31 */ { 3, s_4_31, 29, 1, 0},
/* 32 */ { 2, s_4_32, -1, 1, 0},
/* 33 */ { 3, s_4_33, 32, 2, 0},
/* 34 */ { 2, s_4_34, -1, 1, 0},
/* 35 */ { 3, s_4_35, 34, 2, 0},
/* 36 */ { 2, s_4_36, -1, 2, 0},
/* 37 */ { 2, s_4_37, -1, 2, 0},
/* 38 */ { 2, s_4_38, -1, 2, 0},
/* 39 */ { 2, s_4_39, -1, 1, 0},
/* 40 */ { 3, s_4_40, 39, 2, 0},
/* 41 */ { 3, s_4_41, 39, 2, 0},
/* 42 */ { 3, s_4_42, -1, 1, 0},
/* 43 */ { 3, s_4_43, -1, 2, 0},
/* 44 */ { 2, s_4_44, -1, 1, 0},
/* 45 */ { 3, s_4_45, 44, 2, 0}
};

static symbol s_5_0[1] = { 192 };
static symbol s_5_1[2] = { 201, 192 };
static symbol s_5_2[2] = { 216, 192 };
static symbol s_5_3[1] = { 193 };
static symbol s_5_4[1] = { 197 };
static symbol s_5_5[2] = { 201, 197 };
static symbol s_5_6[2] = { 216, 197 };
static symbol s_5_7[2] = { 193, 200 };
static symbol s_5_8[2] = { 209, 200 };
static symbol s_5_9[3] = { 201, 209, 200 };
static symbol s_5_10[1] = { 201 };
static symbol s_5_11[2] = { 197, 201 };
static symbol s_5_12[2] = { 201, 201 };
static symbol s_5_13[3] = { 193, 205, 201 };
static symbol s_5_14[3] = { 209, 205, 201 };
static symbol s_5_15[4] = { 201, 209, 205, 201 };
static symbol s_5_16[1] = { 202 };
static symbol s_5_17[2] = { 197, 202 };
static symbol s_5_18[3] = { 201, 197, 202 };
static symbol s_5_19[2] = { 201, 202 };
static symbol s_5_20[2] = { 207, 202 };
static symbol s_5_21[2] = { 193, 205 };
static symbol s_5_22[2] = { 197, 205 };
static symbol s_5_23[3] = { 201, 197, 205 };
static symbol s_5_24[2] = { 207, 205 };
static symbol s_5_25[2] = { 209, 205 };
static symbol s_5_26[3] = { 201, 209, 205 };
static symbol s_5_27[1] = { 207 };
static symbol s_5_28[1] = { 209 };
static symbol s_5_29[2] = { 201, 209 };
static symbol s_5_30[2] = { 216, 209 };
static symbol s_5_31[1] = { 213 };
static symbol s_5_32[2] = { 197, 215 };
static symbol s_5_33[2] = { 207, 215 };
static symbol s_5_34[1] = { 216 };
static symbol s_5_35[1] = { 217 };

static struct among a_5[36] =
{
/*  0 */ { 1, s_5_0, -1, 1, 0},
/*  1 */ { 2, s_5_1, 0, 1, 0},
/*  2 */ { 2, s_5_2, 0, 1, 0},
/*  3 */ { 1, s_5_3, -1, 1, 0},
/*  4 */ { 1, s_5_4, -1, 1, 0},
/*  5 */ { 2, s_5_5, 4, 1, 0},
/*  6 */ { 2, s_5_6, 4, 1, 0},
/*  7 */ { 2, s_5_7, -1, 1, 0},
/*  8 */ { 2, s_5_8, -1, 1, 0},
/*  9 */ { 3, s_5_9, 8, 1, 0},
/* 10 */ { 1, s_5_10, -1, 1, 0},
/* 11 */ { 2, s_5_11, 10, 1, 0},
/* 12 */ { 2, s_5_12, 10, 1, 0},
/* 13 */ { 3, s_5_13, 10, 1, 0},
/* 14 */ { 3, s_5_14, 10, 1, 0},
/* 15 */ { 4, s_5_15, 14, 1, 0},
/* 16 */ { 1, s_5_16, -1, 1, 0},
/* 17 */ { 2, s_5_17, 16, 1, 0},
/* 18 */ { 3, s_5_18, 17, 1, 0},
/* 19 */ { 2, s_5_19, 16, 1, 0},
/* 20 */ { 2, s_5_20, 16, 1, 0},
/* 21 */ { 2, s_5_21, -1, 1, 0},
/* 22 */ { 2, s_5_22, -1, 1, 0},
/* 23 */ { 3, s_5_23, 22, 1, 0},
/* 24 */ { 2, s_5_24, -1, 1, 0},
/* 25 */ { 2, s_5_25, -1, 1, 0},
/* 26 */ { 3, s_5_26, 25, 1, 0},
/* 27 */ { 1, s_5_27, -1, 1, 0},
/* 28 */ { 1, s_5_28, -1, 1, 0},
/* 29 */ { 2, s_5_29, 28, 1, 0},
/* 30 */ { 2, s_5_30, 28, 1, 0},
/* 31 */ { 1, s_5_31, -1, 1, 0},
/* 32 */ { 2, s_5_32, -1, 1, 0},
/* 33 */ { 2, s_5_33, -1, 1, 0},
/* 34 */ { 1, s_5_34, -1, 1, 0},
/* 35 */ { 1, s_5_35, -1, 1, 0}
};

static symbol s_6_0[3] = { 207, 211, 212 };
static symbol s_6_1[4] = { 207, 211, 212, 216 };

static struct among a_6[2] =
{
/*  0 */ { 3, s_6_0, -1, 1, 0},
/*  1 */ { 4, s_6_1, -1, 1, 0}
};

static symbol s_7_0[4] = { 197, 202, 219, 197 };
static symbol s_7_1[1] = { 206 };
static symbol s_7_2[1] = { 216 };
static symbol s_7_3[3] = { 197, 202, 219 };

static struct among a_7[4] =
{
/*  0 */ { 4, s_7_0, -1, 1, 0},
/*  1 */ { 1, s_7_1, -1, 2, 0},
/*  2 */ { 1, s_7_2, -1, 3, 0},
/*  3 */ { 3, s_7_3, -1, 1, 0}
};

static unsigned char g_v[] = { 35, 130, 34, 18 };

static symbol s_0[] = { 193 };
static symbol s_1[] = { 209 };
static symbol s_2[] = { 193 };
static symbol s_3[] = { 209 };
static symbol s_4[] = { 193 };
static symbol s_5[] = { 209 };
static symbol s_6[] = { 206 };
static symbol s_7[] = { 206 };
static symbol s_8[] = { 206 };
static symbol s_9[] = { 201 };

static int r_mark_regions(struct SN_env * z) {
    z->I[0] = z->l;
    z->I[1] = z->l;
    {   int c = z->c; /* do, line 100 */
        while(1) { /* gopast, line 101 */
            if (!(in_grouping(z, g_v, 192, 220))) goto lab1;
            break;
        lab1:
            if (z->c >= z->l) goto lab0;
            z->c++;
        }
        z->I[0] = z->c; /* setmark pV, line 101 */
        while(1) { /* gopast, line 101 */
            if (!(out_grouping(z, g_v, 192, 220))) goto lab2;
            break;
        lab2:
            if (z->c >= z->l) goto lab0;
            z->c++;
        }
        while(1) { /* gopast, line 102 */
            if (!(in_grouping(z, g_v, 192, 220))) goto lab3;
            break;
        lab3:
            if (z->c >= z->l) goto lab0;
            z->c++;
        }
        while(1) { /* gopast, line 102 */
            if (!(out_grouping(z, g_v, 192, 220))) goto lab4;
            break;
        lab4:
            if (z->c >= z->l) goto lab0;
            z->c++;
        }
        z->I[1] = z->c; /* setmark p2, line 102 */
    lab0:
        z->c = c;
    }
    return 1;
}

static int r_R2(struct SN_env * z) {
    if (!(z->I[1] <= z->c)) return 0;
    return 1;
}

static int r_perfective_gerund(struct SN_env * z) {
    int among_var;
    z->ket = z->c; /* [, line 111 */
    among_var = find_among_b(z, a_0, 9); /* substring, line 111 */
    if (!(among_var)) return 0;
    z->bra = z->c; /* ], line 111 */
    switch(among_var) {
        case 0: return 0;
        case 1:
            {   int m = z->l - z->c; /* or, line 115 */
                if (!(eq_s_b(z, 1, s_0))) goto lab1;
                goto lab0;
            lab1:
                z->c = z->l - m;
                if (!(eq_s_b(z, 1, s_1))) return 0;
            }
        lab0:
            slice_del(z); /* delete, line 115 */
            break;
        case 2:
            slice_del(z); /* delete, line 122 */
            break;
    }
    return 1;
}

static int r_adjective(struct SN_env * z) {
    int among_var;
    z->ket = z->c; /* [, line 127 */
    among_var = find_among_b(z, a_1, 26); /* substring, line 127 */
    if (!(among_var)) return 0;
    z->bra = z->c; /* ], line 127 */
    switch(among_var) {
        case 0: return 0;
        case 1:
            slice_del(z); /* delete, line 136 */
            break;
    }
    return 1;
}

static int r_adjectival(struct SN_env * z) {
    int among_var;
    if (!r_adjective(z)) return 0; /* call adjective, line 141 */
    {   int m = z->l - z->c; /* try, line 148 */
        z->ket = z->c; /* [, line 149 */
        among_var = find_among_b(z, a_2, 8); /* substring, line 149 */
        if (!(among_var)) { z->c = z->l - m; goto lab0; }
        z->bra = z->c; /* ], line 149 */
        switch(among_var) {
            case 0: { z->c = z->l - m; goto lab0; }
            case 1:
                {   int m = z->l - z->c; /* or, line 154 */
                    if (!(eq_s_b(z, 1, s_2))) goto lab2;
                    goto lab1;
                lab2:
                    z->c = z->l - m;
                    if (!(eq_s_b(z, 1, s_3))) { z->c = z->l - m; goto lab0; }
                }
            lab1:
                slice_del(z); /* delete, line 154 */
                break;
            case 2:
                slice_del(z); /* delete, line 161 */
                break;
        }
    lab0:
        ;
    }
    return 1;
}

static int r_reflexive(struct SN_env * z) {
    int among_var;
    z->ket = z->c; /* [, line 168 */
    among_var = find_among_b(z, a_3, 2); /* substring, line 168 */
    if (!(among_var)) return 0;
    z->bra = z->c; /* ], line 168 */
    switch(among_var) {
        case 0: return 0;
        case 1:
            slice_del(z); /* delete, line 171 */
            break;
    }
    return 1;
}

static int r_verb(struct SN_env * z) {
    int among_var;
    z->ket = z->c; /* [, line 176 */
    among_var = find_among_b(z, a_4, 46); /* substring, line 176 */
    if (!(among_var)) return 0;
    z->bra = z->c; /* ], line 176 */
    switch(among_var) {
        case 0: return 0;
        case 1:
            {   int m = z->l - z->c; /* or, line 182 */
                if (!(eq_s_b(z, 1, s_4))) goto lab1;
                goto lab0;
            lab1:
                z->c = z->l - m;
                if (!(eq_s_b(z, 1, s_5))) return 0;
            }
        lab0:
            slice_del(z); /* delete, line 182 */
            break;
        case 2:
            slice_del(z); /* delete, line 190 */
            break;
    }
    return 1;
}

static int r_noun(struct SN_env * z) {
    int among_var;
    z->ket = z->c; /* [, line 199 */
    among_var = find_among_b(z, a_5, 36); /* substring, line 199 */
    if (!(among_var)) return 0;
    z->bra = z->c; /* ], line 199 */
    switch(among_var) {
        case 0: return 0;
        case 1:
            slice_del(z); /* delete, line 206 */
            break;
    }
    return 1;
}

static int r_derivational(struct SN_env * z) {
    int among_var;
    z->ket = z->c; /* [, line 215 */
    among_var = find_among_b(z, a_6, 2); /* substring, line 215 */
    if (!(among_var)) return 0;
    z->bra = z->c; /* ], line 215 */
    if (!r_R2(z)) return 0; /* call R2, line 215 */
    switch(among_var) {
        case 0: return 0;
        case 1:
            slice_del(z); /* delete, line 218 */
            break;
    }
    return 1;
}

static int r_tidy_up(struct SN_env * z) {
    int among_var;
    z->ket = z->c; /* [, line 223 */
    among_var = find_among_b(z, a_7, 4); /* substring, line 223 */
    if (!(among_var)) return 0;
    z->bra = z->c; /* ], line 223 */
    switch(among_var) {
        case 0: return 0;
        case 1:
            slice_del(z); /* delete, line 227 */
            z->ket = z->c; /* [, line 228 */
            if (!(eq_s_b(z, 1, s_6))) return 0;
            z->bra = z->c; /* ], line 228 */
            if (!(eq_s_b(z, 1, s_7))) return 0;
            slice_del(z); /* delete, line 228 */
            break;
        case 2:
            if (!(eq_s_b(z, 1, s_8))) return 0;
            slice_del(z); /* delete, line 231 */
            break;
        case 3:
            slice_del(z); /* delete, line 233 */
            break;
    }
    return 1;
}

extern int russian_stem(struct SN_env * z) {
    {   int c = z->c; /* do, line 240 */
        if (!r_mark_regions(z)) goto lab0; /* call mark_regions, line 240 */
    lab0:
        z->c = c;
    }
    z->lb = z->c; z->c = z->l; /* backwards, line 241 */

    {   int m = z->l - z->c; /* setlimit, line 241 */
        int m3;
        if (z->c < z->I[0]) return 0;
        z->c = z->I[0]; /* tomark, line 241 */
        m3 = z->lb; z->lb = z->c;
        z->c = z->l - m;
        {   int m = z->l - z->c; /* do, line 242 */
            {   int m = z->l - z->c; /* or, line 243 */
                if (!r_perfective_gerund(z)) goto lab3; /* call perfective_gerund, line 243 */
                goto lab2;
            lab3:
                z->c = z->l - m;
                {   int m = z->l - z->c; /* try, line 244 */
                    if (!r_reflexive(z)) { z->c = z->l - m; goto lab4; } /* call reflexive, line 244 */
                lab4:
                    ;
                }
                {   int m = z->l - z->c; /* or, line 245 */
                    if (!r_adjectival(z)) goto lab6; /* call adjectival, line 245 */
                    goto lab5;
                lab6:
                    z->c = z->l - m;
                    if (!r_verb(z)) goto lab7; /* call verb, line 245 */
                    goto lab5;
                lab7:
                    z->c = z->l - m;
                    if (!r_noun(z)) goto lab1; /* call noun, line 245 */
                }
            lab5:
                ;
            }
        lab2:
        lab1:
            z->c = z->l - m;
        }
        {   int m = z->l - z->c; /* try, line 248 */
            z->ket = z->c; /* [, line 248 */
            if (!(eq_s_b(z, 1, s_9))) { z->c = z->l - m; goto lab8; }
            z->bra = z->c; /* ], line 248 */
            slice_del(z); /* delete, line 248 */
        lab8:
            ;
        }
        {   int m = z->l - z->c; /* do, line 251 */
            if (!r_derivational(z)) goto lab9; /* call derivational, line 251 */
        lab9:
            z->c = z->l - m;
        }
        {   int m = z->l - z->c; /* do, line 252 */
            if (!r_tidy_up(z)) goto lab10; /* call tidy_up, line 252 */
        lab10:
            z->c = z->l - m;
        }
        z->lb = m3;
    }
    z->c = z->lb;
    return 1;
}

extern struct SN_env * russian_create_env(void) { return SN_create_env(0, 2, 0); }

extern void russian_close_env(struct SN_env * z) { SN_close_env(z); }

