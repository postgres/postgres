#ifndef __SPELL_H__
#define __SPELL_H__

#include "c.h"

#include <sys/types.h>

#include "regex/regex.h"

#include "regis.h"
#include "dict.h"

struct SPNode;


typedef struct
{
	uint32
				val:8,
				isword:1,
				compoundallow:1,
				affix:22;
	struct SPNode *node;
}	SPNodeData;

typedef struct SPNode
{
	uint32		length;
	SPNodeData	data[1];
}	SPNode;

#define SPNHDRSZ	(offsetof(SPNode,data))


typedef struct spell_struct
{
	union
	{
		char		flag[16];
		struct
		{
			int			affix;
			int			len;
		}			d;
	}			p;
	char		word[1];
}	SPELL;

#define SPELLHDRSZ	(offsetof(SPELL, word))

typedef struct aff_struct
{
	uint32
				flag:8,
				type:2,
				compile:1,
				flagflags:3,
				issimple:1,
				isregis:1,
				unused:1,
				replen:16;
	char	   *mask;
	char	   *find;
	char	   *repl;
	union
	{
		regex_t		regex;
		Regis		regis;
	}			reg;
}	AFFIX;

#define FF_CROSSPRODUCT		0x01
#define FF_COMPOUNDWORD		0x02
#define FF_COMPOUNDONLYAFX		0x04
#define FF_SUFFIX				2
#define FF_PREFIX				1

struct AffixNode;

typedef struct
{
	uint32
				val:8,
				naff:24;
	AFFIX	  **aff;
	struct AffixNode *node;
}	AffixNodeData;

typedef struct AffixNode
{
	uint32		isvoid:1,
				length:31;
	AffixNodeData data[1];
}	AffixNode;

#define ANHRDSZ		   (offsetof(AffixNode, data))

typedef struct
{
	char	   *affix;
	int			len;
}	CMPDAffix;

typedef struct
{
	int			maffixes;
	int			naffixes;
	AFFIX	   *Affix;
	char		compoundcontrol;

	int			nspell;
	int			mspell;
	SPELL	  **Spell;

	AffixNode  *Suffix;
	AffixNode  *Prefix;

	SPNode	   *Dictionary;
	char	  **AffixData;
	CMPDAffix  *CompoundAffix;

}	IspellDict;

TSLexeme   *NINormalizeWord(IspellDict * Conf, char *word);
int			NIImportAffixes(IspellDict * Conf, const char *filename);
int			NIImportOOAffixes(IspellDict * Conf, const char *filename);
int			NIImportDictionary(IspellDict * Conf, const char *filename);

int			NIAddSpell(IspellDict * Conf, const char *word, const char *flag);
int			NIAddAffix(IspellDict * Conf, int flag, char flagflags, const char *mask, const char *find, const char *repl, int type);
void		NISortDictionary(IspellDict * Conf);
void		NISortAffixes(IspellDict * Conf);
void		NIFree(IspellDict * Conf);

#endif
