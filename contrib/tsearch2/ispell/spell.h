#ifndef __SPELL_H__
#define __SPELL_H__

#include <sys/types.h>
#include <regex.h>

typedef struct spell_struct
{
	char	   *word;
	char		flag[10];
}	SPELL;

typedef struct aff_struct
{
	char		flag;
	char		type;
	char		mask[33];
	char		find[16];
	char		repl[16];
	regex_t		reg;
	size_t		replen;
	char		compile;
}	AFFIX;

typedef struct Tree_struct
{
	int			Left[256],
				Right[256];
}	Tree_struct;

typedef struct
{
	int			maffixes;
	int			naffixes;
	AFFIX	   *Affix;

	int			nspell;
	int			mspell;
	SPELL	   *Spell;
	Tree_struct SpellTree;
	Tree_struct PrefixTree;
	Tree_struct SuffixTree;

}	IspellDict;

char	  **NormalizeWord(IspellDict * Conf, char *word);
int			ImportAffixes(IspellDict * Conf, const char *filename);
int			ImportDictionary(IspellDict * Conf, const char *filename);

int			AddSpell(IspellDict * Conf, const char *word, const char *flag);
int			AddAffix(IspellDict * Conf, int flag, const char *mask, const char *find, const char *repl, int type);
void		SortDictionary(IspellDict * Conf);
void		SortAffixes(IspellDict * Conf);
void		FreeIspell(IspellDict * Conf);

#endif
