#include <stdlib.h>

#include "header.h"

extern struct SN_env *
SN_create_env(int S_size, int I_size, int B_size)
{
	struct SN_env *z = (struct SN_env *) calloc(1, sizeof(struct SN_env));
	struct SN_env *z2 = z;

	if (!z)
		return z;

	z->p = create_s();
	if (!z->p)
		z = NULL;

	if (z && S_size)
	{
		if ((z->S = (symbol * *) calloc(S_size, sizeof(symbol *))))
		{
			int			i;

			for (i = 0; i < S_size; i++)
			{
				if (!(z->S[i] = create_s()))
				{
					z = NULL;
					break;
				}
			}
			z2->S_size = i;
		}
		else
			z = NULL;
	}

	if (z && I_size)
	{
		z->I = (int *) calloc(I_size, sizeof(int));
		if (z->I)
			z->I_size = I_size;
		else
			z = NULL;
	}

	if (z && B_size)
	{
		z->B = (symbol *) calloc(B_size, sizeof(symbol));
		if (z->B)
			z->B_size = B_size;
		else
			z = NULL;
	}

	if (!z)
		SN_close_env(z2);

	return z;
}

extern void
SN_close_env(struct SN_env * z)
{
	if (z->S && z->S_size)
	{
		{
			int			i;

			for (i = 0; i < z->S_size; i++)
				lose_s(z->S[i]);
		}
		free(z->S);
	}
	if (z->I_size)
		free(z->I);
	if (z->B_size)
		free(z->B);
	if (z->p)
		lose_s(z->p);
	free(z);
}

extern void
SN_set_current(struct SN_env * z, int size, const symbol * s)
{
	replace_s(z, 0, z->l, size, s);
	z->c = 0;
}
