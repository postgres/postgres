/* $PostgreSQL: pgsql/contrib/tsearch2/snowball/english_stem.c,v 1.8 2006/03/11 04:38:30 momjian Exp $ */

/* This file was generated automatically by the Snowball to ANSI C compiler */

#include "header.h"

extern int	english_ISO_8859_1_stem(struct SN_env * z);
static int	r_exception2(struct SN_env * z);
static int	r_exception1(struct SN_env * z);
static int	r_Step_5(struct SN_env * z);
static int	r_Step_4(struct SN_env * z);
static int	r_Step_3(struct SN_env * z);
static int	r_Step_2(struct SN_env * z);
static int	r_Step_1c(struct SN_env * z);
static int	r_Step_1b(struct SN_env * z);
static int	r_Step_1a(struct SN_env * z);
static int	r_R2(struct SN_env * z);
static int	r_R1(struct SN_env * z);
static int	r_shortv(struct SN_env * z);
static int	r_mark_regions(struct SN_env * z);
static int	r_postlude(struct SN_env * z);
static int	r_prelude(struct SN_env * z);

extern struct SN_env *english_ISO_8859_1_create_env(void);
extern void english_ISO_8859_1_close_env(struct SN_env * z);

static symbol s_0_0[6] = {'c', 'o', 'm', 'm', 'u', 'n'};
static symbol s_0_1[5] = {'g', 'e', 'n', 'e', 'r'};

static struct among a_0[2] =
{
	 /* 0 */ {6, s_0_0, -1, -1, 0},
	 /* 1 */ {5, s_0_1, -1, -1, 0}
};

static symbol s_1_0[1] = {'\''};
static symbol s_1_1[3] = {'\'', 's', '\''};
static symbol s_1_2[2] = {'\'', 's'};

static struct among a_1[3] =
{
	 /* 0 */ {1, s_1_0, -1, 1, 0},
	 /* 1 */ {3, s_1_1, 0, 1, 0},
	 /* 2 */ {2, s_1_2, -1, 1, 0}
};

static symbol s_2_0[3] = {'i', 'e', 'd'};
static symbol s_2_1[1] = {'s'};
static symbol s_2_2[3] = {'i', 'e', 's'};
static symbol s_2_3[4] = {'s', 's', 'e', 's'};
static symbol s_2_4[2] = {'s', 's'};
static symbol s_2_5[2] = {'u', 's'};

static struct among a_2[6] =
{
	 /* 0 */ {3, s_2_0, -1, 2, 0},
	 /* 1 */ {1, s_2_1, -1, 3, 0},
	 /* 2 */ {3, s_2_2, 1, 2, 0},
	 /* 3 */ {4, s_2_3, 1, 1, 0},
	 /* 4 */ {2, s_2_4, 1, -1, 0},
	 /* 5 */ {2, s_2_5, 1, -1, 0}
};

static symbol s_3_1[2] = {'b', 'b'};
static symbol s_3_2[2] = {'d', 'd'};
static symbol s_3_3[2] = {'f', 'f'};
static symbol s_3_4[2] = {'g', 'g'};
static symbol s_3_5[2] = {'b', 'l'};
static symbol s_3_6[2] = {'m', 'm'};
static symbol s_3_7[2] = {'n', 'n'};
static symbol s_3_8[2] = {'p', 'p'};
static symbol s_3_9[2] = {'r', 'r'};
static symbol s_3_10[2] = {'a', 't'};
static symbol s_3_11[2] = {'t', 't'};
static symbol s_3_12[2] = {'i', 'z'};

static struct among a_3[13] =
{
	 /* 0 */ {0, 0, -1, 3, 0},
	 /* 1 */ {2, s_3_1, 0, 2, 0},
	 /* 2 */ {2, s_3_2, 0, 2, 0},
	 /* 3 */ {2, s_3_3, 0, 2, 0},
	 /* 4 */ {2, s_3_4, 0, 2, 0},
	 /* 5 */ {2, s_3_5, 0, 1, 0},
	 /* 6 */ {2, s_3_6, 0, 2, 0},
	 /* 7 */ {2, s_3_7, 0, 2, 0},
	 /* 8 */ {2, s_3_8, 0, 2, 0},
	 /* 9 */ {2, s_3_9, 0, 2, 0},
	 /* 10 */ {2, s_3_10, 0, 1, 0},
	 /* 11 */ {2, s_3_11, 0, 2, 0},
	 /* 12 */ {2, s_3_12, 0, 1, 0}
};

static symbol s_4_0[2] = {'e', 'd'};
static symbol s_4_1[3] = {'e', 'e', 'd'};
static symbol s_4_2[3] = {'i', 'n', 'g'};
static symbol s_4_3[4] = {'e', 'd', 'l', 'y'};
static symbol s_4_4[5] = {'e', 'e', 'd', 'l', 'y'};
static symbol s_4_5[5] = {'i', 'n', 'g', 'l', 'y'};

static struct among a_4[6] =
{
	 /* 0 */ {2, s_4_0, -1, 2, 0},
	 /* 1 */ {3, s_4_1, 0, 1, 0},
	 /* 2 */ {3, s_4_2, -1, 2, 0},
	 /* 3 */ {4, s_4_3, -1, 2, 0},
	 /* 4 */ {5, s_4_4, 3, 1, 0},
	 /* 5 */ {5, s_4_5, -1, 2, 0}
};

static symbol s_5_0[4] = {'a', 'n', 'c', 'i'};
static symbol s_5_1[4] = {'e', 'n', 'c', 'i'};
static symbol s_5_2[3] = {'o', 'g', 'i'};
static symbol s_5_3[2] = {'l', 'i'};
static symbol s_5_4[3] = {'b', 'l', 'i'};
static symbol s_5_5[4] = {'a', 'b', 'l', 'i'};
static symbol s_5_6[4] = {'a', 'l', 'l', 'i'};
static symbol s_5_7[5] = {'f', 'u', 'l', 'l', 'i'};
static symbol s_5_8[6] = {'l', 'e', 's', 's', 'l', 'i'};
static symbol s_5_9[5] = {'o', 'u', 's', 'l', 'i'};
static symbol s_5_10[5] = {'e', 'n', 't', 'l', 'i'};
static symbol s_5_11[5] = {'a', 'l', 'i', 't', 'i'};
static symbol s_5_12[6] = {'b', 'i', 'l', 'i', 't', 'i'};
static symbol s_5_13[5] = {'i', 'v', 'i', 't', 'i'};
static symbol s_5_14[6] = {'t', 'i', 'o', 'n', 'a', 'l'};
static symbol s_5_15[7] = {'a', 't', 'i', 'o', 'n', 'a', 'l'};
static symbol s_5_16[5] = {'a', 'l', 'i', 's', 'm'};
static symbol s_5_17[5] = {'a', 't', 'i', 'o', 'n'};
static symbol s_5_18[7] = {'i', 'z', 'a', 't', 'i', 'o', 'n'};
static symbol s_5_19[4] = {'i', 'z', 'e', 'r'};
static symbol s_5_20[4] = {'a', 't', 'o', 'r'};
static symbol s_5_21[7] = {'i', 'v', 'e', 'n', 'e', 's', 's'};
static symbol s_5_22[7] = {'f', 'u', 'l', 'n', 'e', 's', 's'};
static symbol s_5_23[7] = {'o', 'u', 's', 'n', 'e', 's', 's'};

static struct among a_5[24] =
{
	 /* 0 */ {4, s_5_0, -1, 3, 0},
	 /* 1 */ {4, s_5_1, -1, 2, 0},
	 /* 2 */ {3, s_5_2, -1, 13, 0},
	 /* 3 */ {2, s_5_3, -1, 16, 0},
	 /* 4 */ {3, s_5_4, 3, 12, 0},
	 /* 5 */ {4, s_5_5, 4, 4, 0},
	 /* 6 */ {4, s_5_6, 3, 8, 0},
	 /* 7 */ {5, s_5_7, 3, 14, 0},
	 /* 8 */ {6, s_5_8, 3, 15, 0},
	 /* 9 */ {5, s_5_9, 3, 10, 0},
	 /* 10 */ {5, s_5_10, 3, 5, 0},
	 /* 11 */ {5, s_5_11, -1, 8, 0},
	 /* 12 */ {6, s_5_12, -1, 12, 0},
	 /* 13 */ {5, s_5_13, -1, 11, 0},
	 /* 14 */ {6, s_5_14, -1, 1, 0},
	 /* 15 */ {7, s_5_15, 14, 7, 0},
	 /* 16 */ {5, s_5_16, -1, 8, 0},
	 /* 17 */ {5, s_5_17, -1, 7, 0},
	 /* 18 */ {7, s_5_18, 17, 6, 0},
	 /* 19 */ {4, s_5_19, -1, 6, 0},
	 /* 20 */ {4, s_5_20, -1, 7, 0},
	 /* 21 */ {7, s_5_21, -1, 11, 0},
	 /* 22 */ {7, s_5_22, -1, 9, 0},
	 /* 23 */ {7, s_5_23, -1, 10, 0}
};

static symbol s_6_0[5] = {'i', 'c', 'a', 't', 'e'};
static symbol s_6_1[5] = {'a', 't', 'i', 'v', 'e'};
static symbol s_6_2[5] = {'a', 'l', 'i', 'z', 'e'};
static symbol s_6_3[5] = {'i', 'c', 'i', 't', 'i'};
static symbol s_6_4[4] = {'i', 'c', 'a', 'l'};
static symbol s_6_5[6] = {'t', 'i', 'o', 'n', 'a', 'l'};
static symbol s_6_6[7] = {'a', 't', 'i', 'o', 'n', 'a', 'l'};
static symbol s_6_7[3] = {'f', 'u', 'l'};
static symbol s_6_8[4] = {'n', 'e', 's', 's'};

static struct among a_6[9] =
{
	 /* 0 */ {5, s_6_0, -1, 4, 0},
	 /* 1 */ {5, s_6_1, -1, 6, 0},
	 /* 2 */ {5, s_6_2, -1, 3, 0},
	 /* 3 */ {5, s_6_3, -1, 4, 0},
	 /* 4 */ {4, s_6_4, -1, 4, 0},
	 /* 5 */ {6, s_6_5, -1, 1, 0},
	 /* 6 */ {7, s_6_6, 5, 2, 0},
	 /* 7 */ {3, s_6_7, -1, 5, 0},
	 /* 8 */ {4, s_6_8, -1, 5, 0}
};

static symbol s_7_0[2] = {'i', 'c'};
static symbol s_7_1[4] = {'a', 'n', 'c', 'e'};
static symbol s_7_2[4] = {'e', 'n', 'c', 'e'};
static symbol s_7_3[4] = {'a', 'b', 'l', 'e'};
static symbol s_7_4[4] = {'i', 'b', 'l', 'e'};
static symbol s_7_5[3] = {'a', 't', 'e'};
static symbol s_7_6[3] = {'i', 'v', 'e'};
static symbol s_7_7[3] = {'i', 'z', 'e'};
static symbol s_7_8[3] = {'i', 't', 'i'};
static symbol s_7_9[2] = {'a', 'l'};
static symbol s_7_10[3] = {'i', 's', 'm'};
static symbol s_7_11[3] = {'i', 'o', 'n'};
static symbol s_7_12[2] = {'e', 'r'};
static symbol s_7_13[3] = {'o', 'u', 's'};
static symbol s_7_14[3] = {'a', 'n', 't'};
static symbol s_7_15[3] = {'e', 'n', 't'};
static symbol s_7_16[4] = {'m', 'e', 'n', 't'};
static symbol s_7_17[5] = {'e', 'm', 'e', 'n', 't'};

static struct among a_7[18] =
{
	 /* 0 */ {2, s_7_0, -1, 1, 0},
	 /* 1 */ {4, s_7_1, -1, 1, 0},
	 /* 2 */ {4, s_7_2, -1, 1, 0},
	 /* 3 */ {4, s_7_3, -1, 1, 0},
	 /* 4 */ {4, s_7_4, -1, 1, 0},
	 /* 5 */ {3, s_7_5, -1, 1, 0},
	 /* 6 */ {3, s_7_6, -1, 1, 0},
	 /* 7 */ {3, s_7_7, -1, 1, 0},
	 /* 8 */ {3, s_7_8, -1, 1, 0},
	 /* 9 */ {2, s_7_9, -1, 1, 0},
	 /* 10 */ {3, s_7_10, -1, 1, 0},
	 /* 11 */ {3, s_7_11, -1, 2, 0},
	 /* 12 */ {2, s_7_12, -1, 1, 0},
	 /* 13 */ {3, s_7_13, -1, 1, 0},
	 /* 14 */ {3, s_7_14, -1, 1, 0},
	 /* 15 */ {3, s_7_15, -1, 1, 0},
	 /* 16 */ {4, s_7_16, 15, 1, 0},
	 /* 17 */ {5, s_7_17, 16, 1, 0}
};

static symbol s_8_0[1] = {'e'};
static symbol s_8_1[1] = {'l'};

static struct among a_8[2] =
{
	 /* 0 */ {1, s_8_0, -1, 1, 0},
	 /* 1 */ {1, s_8_1, -1, 2, 0}
};

static symbol s_9_0[7] = {'s', 'u', 'c', 'c', 'e', 'e', 'd'};
static symbol s_9_1[7] = {'p', 'r', 'o', 'c', 'e', 'e', 'd'};
static symbol s_9_2[6] = {'e', 'x', 'c', 'e', 'e', 'd'};
static symbol s_9_3[7] = {'c', 'a', 'n', 'n', 'i', 'n', 'g'};
static symbol s_9_4[6] = {'i', 'n', 'n', 'i', 'n', 'g'};
static symbol s_9_5[7] = {'e', 'a', 'r', 'r', 'i', 'n', 'g'};
static symbol s_9_6[7] = {'h', 'e', 'r', 'r', 'i', 'n', 'g'};
static symbol s_9_7[6] = {'o', 'u', 't', 'i', 'n', 'g'};

static struct among a_9[8] =
{
	 /* 0 */ {7, s_9_0, -1, -1, 0},
	 /* 1 */ {7, s_9_1, -1, -1, 0},
	 /* 2 */ {6, s_9_2, -1, -1, 0},
	 /* 3 */ {7, s_9_3, -1, -1, 0},
	 /* 4 */ {6, s_9_4, -1, -1, 0},
	 /* 5 */ {7, s_9_5, -1, -1, 0},
	 /* 6 */ {7, s_9_6, -1, -1, 0},
	 /* 7 */ {6, s_9_7, -1, -1, 0}
};

static symbol s_10_0[5] = {'a', 'n', 'd', 'e', 's'};
static symbol s_10_1[5] = {'a', 't', 'l', 'a', 's'};
static symbol s_10_2[4] = {'b', 'i', 'a', 's'};
static symbol s_10_3[6] = {'c', 'o', 's', 'm', 'o', 's'};
static symbol s_10_4[5] = {'d', 'y', 'i', 'n', 'g'};
static symbol s_10_5[5] = {'e', 'a', 'r', 'l', 'y'};
static symbol s_10_6[6] = {'g', 'e', 'n', 't', 'l', 'y'};
static symbol s_10_7[4] = {'h', 'o', 'w', 'e'};
static symbol s_10_8[4] = {'i', 'd', 'l', 'y'};
static symbol s_10_9[5] = {'l', 'y', 'i', 'n', 'g'};
static symbol s_10_10[4] = {'n', 'e', 'w', 's'};
static symbol s_10_11[4] = {'o', 'n', 'l', 'y'};
static symbol s_10_12[6] = {'s', 'i', 'n', 'g', 'l', 'y'};
static symbol s_10_13[5] = {'s', 'k', 'i', 'e', 's'};
static symbol s_10_14[4] = {'s', 'k', 'i', 's'};
static symbol s_10_15[3] = {'s', 'k', 'y'};
static symbol s_10_16[5] = {'t', 'y', 'i', 'n', 'g'};
static symbol s_10_17[4] = {'u', 'g', 'l', 'y'};

static struct among a_10[18] =
{
	 /* 0 */ {5, s_10_0, -1, -1, 0},
	 /* 1 */ {5, s_10_1, -1, -1, 0},
	 /* 2 */ {4, s_10_2, -1, -1, 0},
	 /* 3 */ {6, s_10_3, -1, -1, 0},
	 /* 4 */ {5, s_10_4, -1, 3, 0},
	 /* 5 */ {5, s_10_5, -1, 9, 0},
	 /* 6 */ {6, s_10_6, -1, 7, 0},
	 /* 7 */ {4, s_10_7, -1, -1, 0},
	 /* 8 */ {4, s_10_8, -1, 6, 0},
	 /* 9 */ {5, s_10_9, -1, 4, 0},
	 /* 10 */ {4, s_10_10, -1, -1, 0},
	 /* 11 */ {4, s_10_11, -1, 10, 0},
	 /* 12 */ {6, s_10_12, -1, 11, 0},
	 /* 13 */ {5, s_10_13, -1, 2, 0},
	 /* 14 */ {4, s_10_14, -1, 1, 0},
	 /* 15 */ {3, s_10_15, -1, -1, 0},
	 /* 16 */ {5, s_10_16, -1, 5, 0},
	 /* 17 */ {4, s_10_17, -1, 8, 0}
};

static unsigned char g_v[] = {17, 65, 16, 1};

static unsigned char g_v_WXY[] = {1, 17, 65, 208, 1};

static unsigned char g_valid_LI[] = {55, 141, 2};

static symbol s_0[] = {'\''};
static symbol s_1[] = {'y'};
static symbol s_2[] = {'Y'};
static symbol s_3[] = {'y'};
static symbol s_4[] = {'Y'};
static symbol s_5[] = {'s', 's'};
static symbol s_6[] = {'i', 'e'};
static symbol s_7[] = {'i'};
static symbol s_8[] = {'e', 'e'};
static symbol s_9[] = {'e'};
static symbol s_10[] = {'e'};
static symbol s_11[] = {'y'};
static symbol s_12[] = {'Y'};
static symbol s_13[] = {'i'};
static symbol s_14[] = {'t', 'i', 'o', 'n'};
static symbol s_15[] = {'e', 'n', 'c', 'e'};
static symbol s_16[] = {'a', 'n', 'c', 'e'};
static symbol s_17[] = {'a', 'b', 'l', 'e'};
static symbol s_18[] = {'e', 'n', 't'};
static symbol s_19[] = {'i', 'z', 'e'};
static symbol s_20[] = {'a', 't', 'e'};
static symbol s_21[] = {'a', 'l'};
static symbol s_22[] = {'f', 'u', 'l'};
static symbol s_23[] = {'o', 'u', 's'};
static symbol s_24[] = {'i', 'v', 'e'};
static symbol s_25[] = {'b', 'l', 'e'};
static symbol s_26[] = {'l'};
static symbol s_27[] = {'o', 'g'};
static symbol s_28[] = {'f', 'u', 'l'};
static symbol s_29[] = {'l', 'e', 's', 's'};
static symbol s_30[] = {'t', 'i', 'o', 'n'};
static symbol s_31[] = {'a', 't', 'e'};
static symbol s_32[] = {'a', 'l'};
static symbol s_33[] = {'i', 'c'};
static symbol s_34[] = {'s'};
static symbol s_35[] = {'t'};
static symbol s_36[] = {'l'};
static symbol s_37[] = {'s', 'k', 'i'};
static symbol s_38[] = {'s', 'k', 'y'};
static symbol s_39[] = {'d', 'i', 'e'};
static symbol s_40[] = {'l', 'i', 'e'};
static symbol s_41[] = {'t', 'i', 'e'};
static symbol s_42[] = {'i', 'd', 'l'};
static symbol s_43[] = {'g', 'e', 'n', 't', 'l'};
static symbol s_44[] = {'u', 'g', 'l', 'i'};
static symbol s_45[] = {'e', 'a', 'r', 'l', 'i'};
static symbol s_46[] = {'o', 'n', 'l', 'i'};
static symbol s_47[] = {'s', 'i', 'n', 'g', 'l'};
static symbol s_48[] = {'Y'};
static symbol s_49[] = {'y'};

static int
r_prelude(struct SN_env * z)
{
	z->B[0] = 0;				/* unset Y_found, line 26 */
	{
		int			c = z->c;	/* do, line 27 */

		z->bra = z->c;			/* [, line 27 */
		if (!(eq_s(z, 1, s_0)))
			goto lab0;
		z->ket = z->c;			/* ], line 27 */
		{
			int			ret;

			ret = slice_del(z); /* delete, line 27 */
			if (ret < 0)
				return ret;
		}
lab0:
		z->c = c;
	}
	{
		int			c = z->c;	/* do, line 28 */

		z->bra = z->c;			/* [, line 28 */
		if (!(eq_s(z, 1, s_1)))
			goto lab1;
		z->ket = z->c;			/* ], line 28 */
		if (!(in_grouping(z, g_v, 97, 121)))
			goto lab1;
		{
			int			ret;

			ret = slice_from_s(z, 1, s_2);		/* <-, line 28 */
			if (ret < 0)
				return ret;
		}
		z->B[0] = 1;			/* set Y_found, line 28 */
lab1:
		z->c = c;
	}
	{
		int			c = z->c;	/* do, line 29 */

		while (1)
		{						/* repeat, line 29 */
			int			c = z->c;

			while (1)
			{					/* goto, line 29 */
				int			c = z->c;

				if (!(in_grouping(z, g_v, 97, 121)))
					goto lab4;
				z->bra = z->c;	/* [, line 29 */
				if (!(eq_s(z, 1, s_3)))
					goto lab4;
				z->ket = z->c;	/* ], line 29 */
				z->c = c;
				break;
		lab4:
				z->c = c;
				if (z->c >= z->l)
					goto lab3;
				z->c++;			/* goto, line 29 */
			}
			{
				int			ret;

				ret = slice_from_s(z, 1, s_4);	/* <-, line 29 */
				if (ret < 0)
					return ret;
			}
			z->B[0] = 1;		/* set Y_found, line 29 */
			continue;
	lab3:
			z->c = c;
			break;
		}
		z->c = c;
	}
	return 1;
}

static int
r_mark_regions(struct SN_env * z)
{
	z->I[0] = z->l;
	z->I[1] = z->l;
	{
		int			c = z->c;	/* do, line 35 */

		{
			int			c = z->c;		/* or, line 40 */

			if (!(find_among(z, a_0, 2)))
				goto lab2;		/* among, line 36 */
			goto lab1;
	lab2:
			z->c = c;
			while (1)
			{					/* gopast, line 40 */
				if (!(in_grouping(z, g_v, 97, 121)))
					goto lab3;
				break;
		lab3:
				if (z->c >= z->l)
					goto lab0;
				z->c++;			/* gopast, line 40 */
			}
			while (1)
			{					/* gopast, line 40 */
				if (!(out_grouping(z, g_v, 97, 121)))
					goto lab4;
				break;
		lab4:
				if (z->c >= z->l)
					goto lab0;
				z->c++;			/* gopast, line 40 */
			}
		}
lab1:
		z->I[0] = z->c;			/* setmark p1, line 41 */
		while (1)
		{						/* gopast, line 42 */
			if (!(in_grouping(z, g_v, 97, 121)))
				goto lab5;
			break;
	lab5:
			if (z->c >= z->l)
				goto lab0;
			z->c++;				/* gopast, line 42 */
		}
		while (1)
		{						/* gopast, line 42 */
			if (!(out_grouping(z, g_v, 97, 121)))
				goto lab6;
			break;
	lab6:
			if (z->c >= z->l)
				goto lab0;
			z->c++;				/* gopast, line 42 */
		}
		z->I[1] = z->c;			/* setmark p2, line 42 */
lab0:
		z->c = c;
	}
	return 1;
}

static int
r_shortv(struct SN_env * z)
{
	{
		int			m = z->l - z->c;

		(void) m;				/* or, line 50 */
		if (!(out_grouping_b(z, g_v_WXY, 89, 121)))
			goto lab1;
		if (!(in_grouping_b(z, g_v, 97, 121)))
			goto lab1;
		if (!(out_grouping_b(z, g_v, 97, 121)))
			goto lab1;
		goto lab0;
lab1:
		z->c = z->l - m;
		if (!(out_grouping_b(z, g_v, 97, 121)))
			return 0;
		if (!(in_grouping_b(z, g_v, 97, 121)))
			return 0;
		if (z->c > z->lb)
			return 0;			/* atlimit, line 51 */
	}
lab0:
	return 1;
}

static int
r_R1(struct SN_env * z)
{
	if (!(z->I[0] <= z->c))
		return 0;
	return 1;
}

static int
r_R2(struct SN_env * z)
{
	if (!(z->I[1] <= z->c))
		return 0;
	return 1;
}

static int
r_Step_1a(struct SN_env * z)
{
	int			among_var;

	{
		int			m = z->l - z->c;

		(void) m;				/* try, line 58 */
		z->ket = z->c;			/* [, line 59 */
		among_var = find_among_b(z, a_1, 3);	/* substring, line 59 */
		if (!(among_var))
		{
			z->c = z->l - m;
			goto lab0;
		}
		z->bra = z->c;			/* ], line 59 */
		switch (among_var)
		{
			case 0:
				{
					z->c = z->l - m;
					goto lab0;
				}
			case 1:
				{
					int			ret;

					ret = slice_del(z); /* delete, line 61 */
					if (ret < 0)
						return ret;
				}
				break;
		}
lab0:
		;
	}
	z->ket = z->c;				/* [, line 64 */
	among_var = find_among_b(z, a_2, 6);		/* substring, line 64 */
	if (!(among_var))
		return 0;
	z->bra = z->c;				/* ], line 64 */
	switch (among_var)
	{
		case 0:
			return 0;
		case 1:
			{
				int			ret;

				ret = slice_from_s(z, 2, s_5);	/* <-, line 65 */
				if (ret < 0)
					return ret;
			}
			break;
		case 2:
			{
				int			m = z->l - z->c;

				(void) m;		/* or, line 67 */
				if (z->c <= z->lb)
					goto lab2;
				z->c--;			/* next, line 67 */
				if (z->c > z->lb)
					goto lab2;	/* atlimit, line 67 */
				{
					int			ret;

					ret = slice_from_s(z, 2, s_6);		/* <-, line 67 */
					if (ret < 0)
						return ret;
				}
				goto lab1;
		lab2:
				z->c = z->l - m;
				{
					int			ret;

					ret = slice_from_s(z, 1, s_7);		/* <-, line 67 */
					if (ret < 0)
						return ret;
				}
			}
	lab1:
			break;
		case 3:
			if (z->c <= z->lb)
				return 0;
			z->c--;				/* next, line 68 */
			while (1)
			{					/* gopast, line 68 */
				if (!(in_grouping_b(z, g_v, 97, 121)))
					goto lab3;
				break;
		lab3:
				if (z->c <= z->lb)
					return 0;
				z->c--;			/* gopast, line 68 */
			}
			{
				int			ret;

				ret = slice_del(z);		/* delete, line 68 */
				if (ret < 0)
					return ret;
			}
			break;
	}
	return 1;
}

static int
r_Step_1b(struct SN_env * z)
{
	int			among_var;

	z->ket = z->c;				/* [, line 74 */
	among_var = find_among_b(z, a_4, 6);		/* substring, line 74 */
	if (!(among_var))
		return 0;
	z->bra = z->c;				/* ], line 74 */
	switch (among_var)
	{
		case 0:
			return 0;
		case 1:
			{
				int			ret = r_R1(z);

				if (ret == 0)
					return 0;	/* call R1, line 76 */
				if (ret < 0)
					return ret;
			}
			{
				int			ret;

				ret = slice_from_s(z, 2, s_8);	/* <-, line 76 */
				if (ret < 0)
					return ret;
			}
			break;
		case 2:
			{
				int			m_test = z->l - z->c;		/* test, line 79 */

				while (1)
				{				/* gopast, line 79 */
					if (!(in_grouping_b(z, g_v, 97, 121)))
						goto lab0;
					break;
			lab0:
					if (z->c <= z->lb)
						return 0;
					z->c--;		/* gopast, line 79 */
				}
				z->c = z->l - m_test;
			}
			{
				int			ret;

				ret = slice_del(z);		/* delete, line 79 */
				if (ret < 0)
					return ret;
			}
			{
				int			m_test = z->l - z->c;		/* test, line 80 */

				among_var = find_among_b(z, a_3, 13);	/* substring, line 80 */
				if (!(among_var))
					return 0;
				z->c = z->l - m_test;
			}
			switch (among_var)
			{
				case 0:
					return 0;
				case 1:
					{
						int			ret;

						{
							int			c = z->c;

							ret = insert_s(z, z->c, z->c, 1, s_9);		/* <+, line 82 */
							z->c = c;
						}
						if (ret < 0)
							return ret;
					}
					break;
				case 2:
					z->ket = z->c;		/* [, line 85 */
					if (z->c <= z->lb)
						return 0;
					z->c--;		/* next, line 85 */
					z->bra = z->c;		/* ], line 85 */
					{
						int			ret;

						ret = slice_del(z);		/* delete, line 85 */
						if (ret < 0)
							return ret;
					}
					break;
				case 3:
					if (z->c != z->I[0])
						return 0;		/* atmark, line 86 */
					{
						int			m_test = z->l - z->c;		/* test, line 86 */

						{
							int			ret = r_shortv(z);

							if (ret == 0)
								return 0;		/* call shortv, line 86 */
							if (ret < 0)
								return ret;
						}
						z->c = z->l - m_test;
					}
					{
						int			ret;

						{
							int			c = z->c;

							ret = insert_s(z, z->c, z->c, 1, s_10);		/* <+, line 86 */
							z->c = c;
						}
						if (ret < 0)
							return ret;
					}
					break;
			}
			break;
	}
	return 1;
}

static int
r_Step_1c(struct SN_env * z)
{
	z->ket = z->c;				/* [, line 93 */
	{
		int			m = z->l - z->c;

		(void) m;				/* or, line 93 */
		if (!(eq_s_b(z, 1, s_11)))
			goto lab1;
		goto lab0;
lab1:
		z->c = z->l - m;
		if (!(eq_s_b(z, 1, s_12)))
			return 0;
	}
lab0:
	z->bra = z->c;				/* ], line 93 */
	if (!(out_grouping_b(z, g_v, 97, 121)))
		return 0;
	{
		int			m = z->l - z->c;

		(void) m;				/* not, line 94 */
		if (z->c > z->lb)
			goto lab2;			/* atlimit, line 94 */
		return 0;
lab2:
		z->c = z->l - m;
	}
	{
		int			ret;

		ret = slice_from_s(z, 1, s_13); /* <-, line 95 */
		if (ret < 0)
			return ret;
	}
	return 1;
}

static int
r_Step_2(struct SN_env * z)
{
	int			among_var;

	z->ket = z->c;				/* [, line 99 */
	among_var = find_among_b(z, a_5, 24);		/* substring, line 99 */
	if (!(among_var))
		return 0;
	z->bra = z->c;				/* ], line 99 */
	{
		int			ret = r_R1(z);

		if (ret == 0)
			return 0;			/* call R1, line 99 */
		if (ret < 0)
			return ret;
	}
	switch (among_var)
	{
		case 0:
			return 0;
		case 1:
			{
				int			ret;

				ret = slice_from_s(z, 4, s_14); /* <-, line 100 */
				if (ret < 0)
					return ret;
			}
			break;
		case 2:
			{
				int			ret;

				ret = slice_from_s(z, 4, s_15); /* <-, line 101 */
				if (ret < 0)
					return ret;
			}
			break;
		case 3:
			{
				int			ret;

				ret = slice_from_s(z, 4, s_16); /* <-, line 102 */
				if (ret < 0)
					return ret;
			}
			break;
		case 4:
			{
				int			ret;

				ret = slice_from_s(z, 4, s_17); /* <-, line 103 */
				if (ret < 0)
					return ret;
			}
			break;
		case 5:
			{
				int			ret;

				ret = slice_from_s(z, 3, s_18); /* <-, line 104 */
				if (ret < 0)
					return ret;
			}
			break;
		case 6:
			{
				int			ret;

				ret = slice_from_s(z, 3, s_19); /* <-, line 106 */
				if (ret < 0)
					return ret;
			}
			break;
		case 7:
			{
				int			ret;

				ret = slice_from_s(z, 3, s_20); /* <-, line 108 */
				if (ret < 0)
					return ret;
			}
			break;
		case 8:
			{
				int			ret;

				ret = slice_from_s(z, 2, s_21); /* <-, line 110 */
				if (ret < 0)
					return ret;
			}
			break;
		case 9:
			{
				int			ret;

				ret = slice_from_s(z, 3, s_22); /* <-, line 111 */
				if (ret < 0)
					return ret;
			}
			break;
		case 10:
			{
				int			ret;

				ret = slice_from_s(z, 3, s_23); /* <-, line 113 */
				if (ret < 0)
					return ret;
			}
			break;
		case 11:
			{
				int			ret;

				ret = slice_from_s(z, 3, s_24); /* <-, line 115 */
				if (ret < 0)
					return ret;
			}
			break;
		case 12:
			{
				int			ret;

				ret = slice_from_s(z, 3, s_25); /* <-, line 117 */
				if (ret < 0)
					return ret;
			}
			break;
		case 13:
			if (!(eq_s_b(z, 1, s_26)))
				return 0;
			{
				int			ret;

				ret = slice_from_s(z, 2, s_27); /* <-, line 118 */
				if (ret < 0)
					return ret;
			}
			break;
		case 14:
			{
				int			ret;

				ret = slice_from_s(z, 3, s_28); /* <-, line 119 */
				if (ret < 0)
					return ret;
			}
			break;
		case 15:
			{
				int			ret;

				ret = slice_from_s(z, 4, s_29); /* <-, line 120 */
				if (ret < 0)
					return ret;
			}
			break;
		case 16:
			if (!(in_grouping_b(z, g_valid_LI, 99, 116)))
				return 0;
			{
				int			ret;

				ret = slice_del(z);		/* delete, line 121 */
				if (ret < 0)
					return ret;
			}
			break;
	}
	return 1;
}

static int
r_Step_3(struct SN_env * z)
{
	int			among_var;

	z->ket = z->c;				/* [, line 126 */
	among_var = find_among_b(z, a_6, 9);		/* substring, line 126 */
	if (!(among_var))
		return 0;
	z->bra = z->c;				/* ], line 126 */
	{
		int			ret = r_R1(z);

		if (ret == 0)
			return 0;			/* call R1, line 126 */
		if (ret < 0)
			return ret;
	}
	switch (among_var)
	{
		case 0:
			return 0;
		case 1:
			{
				int			ret;

				ret = slice_from_s(z, 4, s_30); /* <-, line 127 */
				if (ret < 0)
					return ret;
			}
			break;
		case 2:
			{
				int			ret;

				ret = slice_from_s(z, 3, s_31); /* <-, line 128 */
				if (ret < 0)
					return ret;
			}
			break;
		case 3:
			{
				int			ret;

				ret = slice_from_s(z, 2, s_32); /* <-, line 129 */
				if (ret < 0)
					return ret;
			}
			break;
		case 4:
			{
				int			ret;

				ret = slice_from_s(z, 2, s_33); /* <-, line 131 */
				if (ret < 0)
					return ret;
			}
			break;
		case 5:
			{
				int			ret;

				ret = slice_del(z);		/* delete, line 133 */
				if (ret < 0)
					return ret;
			}
			break;
		case 6:
			{
				int			ret = r_R2(z);

				if (ret == 0)
					return 0;	/* call R2, line 135 */
				if (ret < 0)
					return ret;
			}
			{
				int			ret;

				ret = slice_del(z);		/* delete, line 135 */
				if (ret < 0)
					return ret;
			}
			break;
	}
	return 1;
}

static int
r_Step_4(struct SN_env * z)
{
	int			among_var;

	z->ket = z->c;				/* [, line 140 */
	among_var = find_among_b(z, a_7, 18);		/* substring, line 140 */
	if (!(among_var))
		return 0;
	z->bra = z->c;				/* ], line 140 */
	{
		int			ret = r_R2(z);

		if (ret == 0)
			return 0;			/* call R2, line 140 */
		if (ret < 0)
			return ret;
	}
	switch (among_var)
	{
		case 0:
			return 0;
		case 1:
			{
				int			ret;

				ret = slice_del(z);		/* delete, line 143 */
				if (ret < 0)
					return ret;
			}
			break;
		case 2:
			{
				int			m = z->l - z->c;

				(void) m;		/* or, line 144 */
				if (!(eq_s_b(z, 1, s_34)))
					goto lab1;
				goto lab0;
		lab1:
				z->c = z->l - m;
				if (!(eq_s_b(z, 1, s_35)))
					return 0;
			}
	lab0:
			{
				int			ret;

				ret = slice_del(z);		/* delete, line 144 */
				if (ret < 0)
					return ret;
			}
			break;
	}
	return 1;
}

static int
r_Step_5(struct SN_env * z)
{
	int			among_var;

	z->ket = z->c;				/* [, line 149 */
	among_var = find_among_b(z, a_8, 2);		/* substring, line 149 */
	if (!(among_var))
		return 0;
	z->bra = z->c;				/* ], line 149 */
	switch (among_var)
	{
		case 0:
			return 0;
		case 1:
			{
				int			m = z->l - z->c;

				(void) m;		/* or, line 150 */
				{
					int			ret = r_R2(z);

					if (ret == 0)
						goto lab1;		/* call R2, line 150 */
					if (ret < 0)
						return ret;
				}
				goto lab0;
		lab1:
				z->c = z->l - m;
				{
					int			ret = r_R1(z);

					if (ret == 0)
						return 0;		/* call R1, line 150 */
					if (ret < 0)
						return ret;
				}
				{
					int			m = z->l - z->c;

					(void) m;	/* not, line 150 */
					{
						int			ret = r_shortv(z);

						if (ret == 0)
							goto lab2;	/* call shortv, line 150 */
						if (ret < 0)
							return ret;
					}
					return 0;
			lab2:
					z->c = z->l - m;
				}
			}
	lab0:
			{
				int			ret;

				ret = slice_del(z);		/* delete, line 150 */
				if (ret < 0)
					return ret;
			}
			break;
		case 2:
			{
				int			ret = r_R2(z);

				if (ret == 0)
					return 0;	/* call R2, line 151 */
				if (ret < 0)
					return ret;
			}
			if (!(eq_s_b(z, 1, s_36)))
				return 0;
			{
				int			ret;

				ret = slice_del(z);		/* delete, line 151 */
				if (ret < 0)
					return ret;
			}
			break;
	}
	return 1;
}

static int
r_exception2(struct SN_env * z)
{
	z->ket = z->c;				/* [, line 157 */
	if (!(find_among_b(z, a_9, 8)))
		return 0;				/* substring, line 157 */
	z->bra = z->c;				/* ], line 157 */
	if (z->c > z->lb)
		return 0;				/* atlimit, line 157 */
	return 1;
}

static int
r_exception1(struct SN_env * z)
{
	int			among_var;

	z->bra = z->c;				/* [, line 169 */
	among_var = find_among(z, a_10, 18);		/* substring, line 169 */
	if (!(among_var))
		return 0;
	z->ket = z->c;				/* ], line 169 */
	if (z->c < z->l)
		return 0;				/* atlimit, line 169 */
	switch (among_var)
	{
		case 0:
			return 0;
		case 1:
			{
				int			ret;

				ret = slice_from_s(z, 3, s_37); /* <-, line 173 */
				if (ret < 0)
					return ret;
			}
			break;
		case 2:
			{
				int			ret;

				ret = slice_from_s(z, 3, s_38); /* <-, line 174 */
				if (ret < 0)
					return ret;
			}
			break;
		case 3:
			{
				int			ret;

				ret = slice_from_s(z, 3, s_39); /* <-, line 175 */
				if (ret < 0)
					return ret;
			}
			break;
		case 4:
			{
				int			ret;

				ret = slice_from_s(z, 3, s_40); /* <-, line 176 */
				if (ret < 0)
					return ret;
			}
			break;
		case 5:
			{
				int			ret;

				ret = slice_from_s(z, 3, s_41); /* <-, line 177 */
				if (ret < 0)
					return ret;
			}
			break;
		case 6:
			{
				int			ret;

				ret = slice_from_s(z, 3, s_42); /* <-, line 181 */
				if (ret < 0)
					return ret;
			}
			break;
		case 7:
			{
				int			ret;

				ret = slice_from_s(z, 5, s_43); /* <-, line 182 */
				if (ret < 0)
					return ret;
			}
			break;
		case 8:
			{
				int			ret;

				ret = slice_from_s(z, 4, s_44); /* <-, line 183 */
				if (ret < 0)
					return ret;
			}
			break;
		case 9:
			{
				int			ret;

				ret = slice_from_s(z, 5, s_45); /* <-, line 184 */
				if (ret < 0)
					return ret;
			}
			break;
		case 10:
			{
				int			ret;

				ret = slice_from_s(z, 4, s_46); /* <-, line 185 */
				if (ret < 0)
					return ret;
			}
			break;
		case 11:
			{
				int			ret;

				ret = slice_from_s(z, 5, s_47); /* <-, line 186 */
				if (ret < 0)
					return ret;
			}
			break;
	}
	return 1;
}

static int
r_postlude(struct SN_env * z)
{
	if (!(z->B[0]))
		return 0;				/* Boolean test Y_found, line 202 */
	while (1)
	{							/* repeat, line 202 */
		int			c = z->c;

		while (1)
		{						/* goto, line 202 */
			int			c = z->c;

			z->bra = z->c;		/* [, line 202 */
			if (!(eq_s(z, 1, s_48)))
				goto lab1;
			z->ket = z->c;		/* ], line 202 */
			z->c = c;
			break;
	lab1:
			z->c = c;
			if (z->c >= z->l)
				goto lab0;
			z->c++;				/* goto, line 202 */
		}
		{
			int			ret;

			ret = slice_from_s(z, 1, s_49);		/* <-, line 202 */
			if (ret < 0)
				return ret;
		}
		continue;
lab0:
		z->c = c;
		break;
	}
	return 1;
}

extern int
english_ISO_8859_1_stem(struct SN_env * z)
{
	{
		int			c = z->c;	/* or, line 206 */

		{
			int			ret = r_exception1(z);

			if (ret == 0)
				goto lab1;		/* call exception1, line 206 */
			if (ret < 0)
				return ret;
		}
		goto lab0;
lab1:
		z->c = c;
		{
			int			c = z->c;		/* not, line 207 */

			{
				int			c = z->c + 3;

				if (0 > c || c > z->l)
					goto lab3;
				z->c = c;		/* hop, line 207 */
			}
			goto lab2;
	lab3:
			z->c = c;
		}
		goto lab0;
lab2:
		z->c = c;
		{
			int			c = z->c;		/* do, line 208 */

			{
				int			ret = r_prelude(z);

				if (ret == 0)
					goto lab4;	/* call prelude, line 208 */
				if (ret < 0)
					return ret;
			}
	lab4:
			z->c = c;
		}
		{
			int			c = z->c;		/* do, line 209 */

			{
				int			ret = r_mark_regions(z);

				if (ret == 0)
					goto lab5;	/* call mark_regions, line 209 */
				if (ret < 0)
					return ret;
			}
	lab5:
			z->c = c;
		}
		z->lb = z->c;
		z->c = z->l;			/* backwards, line 210 */

		{
			int			m = z->l - z->c;

			(void) m;			/* do, line 212 */
			{
				int			ret = r_Step_1a(z);

				if (ret == 0)
					goto lab6;	/* call Step_1a, line 212 */
				if (ret < 0)
					return ret;
			}
	lab6:
			z->c = z->l - m;
		}
		{
			int			m = z->l - z->c;

			(void) m;			/* or, line 214 */
			{
				int			ret = r_exception2(z);

				if (ret == 0)
					goto lab8;	/* call exception2, line 214 */
				if (ret < 0)
					return ret;
			}
			goto lab7;
	lab8:
			z->c = z->l - m;
			{
				int			m = z->l - z->c;

				(void) m;		/* do, line 216 */
				{
					int			ret = r_Step_1b(z);

					if (ret == 0)
						goto lab9;		/* call Step_1b, line 216 */
					if (ret < 0)
						return ret;
				}
		lab9:
				z->c = z->l - m;
			}
			{
				int			m = z->l - z->c;

				(void) m;		/* do, line 217 */
				{
					int			ret = r_Step_1c(z);

					if (ret == 0)
						goto lab10;		/* call Step_1c, line 217 */
					if (ret < 0)
						return ret;
				}
		lab10:
				z->c = z->l - m;
			}
			{
				int			m = z->l - z->c;

				(void) m;		/* do, line 219 */
				{
					int			ret = r_Step_2(z);

					if (ret == 0)
						goto lab11;		/* call Step_2, line 219 */
					if (ret < 0)
						return ret;
				}
		lab11:
				z->c = z->l - m;
			}
			{
				int			m = z->l - z->c;

				(void) m;		/* do, line 220 */
				{
					int			ret = r_Step_3(z);

					if (ret == 0)
						goto lab12;		/* call Step_3, line 220 */
					if (ret < 0)
						return ret;
				}
		lab12:
				z->c = z->l - m;
			}
			{
				int			m = z->l - z->c;

				(void) m;		/* do, line 221 */
				{
					int			ret = r_Step_4(z);

					if (ret == 0)
						goto lab13;		/* call Step_4, line 221 */
					if (ret < 0)
						return ret;
				}
		lab13:
				z->c = z->l - m;
			}
			{
				int			m = z->l - z->c;

				(void) m;		/* do, line 223 */
				{
					int			ret = r_Step_5(z);

					if (ret == 0)
						goto lab14;		/* call Step_5, line 223 */
					if (ret < 0)
						return ret;
				}
		lab14:
				z->c = z->l - m;
			}
		}
lab7:
		z->c = z->lb;
		{
			int			c = z->c;		/* do, line 226 */

			{
				int			ret = r_postlude(z);

				if (ret == 0)
					goto lab15; /* call postlude, line 226 */
				if (ret < 0)
					return ret;
			}
	lab15:
			z->c = c;
		}
	}
lab0:
	return 1;
}

extern struct SN_env *
english_ISO_8859_1_create_env(void)
{
	return SN_create_env(0, 2, 1);
}

extern void
english_ISO_8859_1_close_env(struct SN_env * z)
{
	SN_close_env(z);
}
