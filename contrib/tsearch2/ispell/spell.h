#ifndef __SPELL_H__
#define __SPELL_H__

#include <sys/types.h>
#include <regex.h>


struct SPNode;


typedef struct {
	u_int32_t 
		val:8,
		isword:1,
		compoundallow:1,
		affix:22;
	struct SPNode *node; 
} SPNodeData;

typedef struct SPNode {
	u_int32_t 	length;
	SPNodeData	data[1];	
} SPNode;

#define SPNHRDSZ	(sizeof(u_int32_t))


typedef struct spell_struct
{
	char	   *word;
	union {
		char		flag[16];
		struct {
			int		affix;
			int 		len;
		} d;
	} p;
}	SPELL;

typedef struct aff_struct
{
	char		flag;
	char		flagflags;
	char		type;
	char		mask[33];
	char		find[16];
	char		repl[16];
	regex_t		reg;
	size_t		replen;
	char		compile;
}	AFFIX;

#define FF_CROSSPRODUCT 	0x01
#define FF_COMPOUNDWORD 	0x02
#define FF_COMPOUNDONLYAFX      0x04

struct AffixNode;

typedef struct {
	u_int32_t
		val:8,
		naff:24;
	AFFIX   **aff;
	struct AffixNode *node;
} AffixNodeData;

typedef struct AffixNode {
	u_int32_t length;
	AffixNodeData	data[1];
} AffixNode;

#define ANHRDSZ        (sizeof(u_int32_t))

typedef struct Tree_struct
{
	int			Left[256],
				Right[256];
}	Tree_struct;

typedef struct {
	char *affix;
	int len;
} CMPDAffix;

typedef struct
{
	int			maffixes;
	int			naffixes;
	AFFIX	   *Affix;
	char			compoundcontrol;

	int			nspell;
	int			mspell;
	SPELL	   *Spell;

	AffixNode	*Suffix;
	AffixNode	*Prefix;

	SPNode	*Dictionary;
	char	**AffixData;
	CMPDAffix    *CompoundAffix;

}	IspellDict;

char	  **NINormalizeWord(IspellDict * Conf, char *word);
int			NIImportAffixes(IspellDict * Conf, const char *filename);
int			NIImportDictionary(IspellDict * Conf, const char *filename);

int			NIAddSpell(IspellDict * Conf, const char *word, const char *flag);
int			NIAddAffix(IspellDict * Conf, int flag, char flagflags, const char *mask, const char *find, const char *repl, int type);
void		NISortDictionary(IspellDict * Conf);
void		NISortAffixes(IspellDict * Conf);
void		NIFree(IspellDict * Conf);

#endif
