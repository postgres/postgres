#ifndef __MORPH_H__
#define __MORPH_H__

void		initmorph(void);

char	   *lemmatize(char *word, int *len, int type);

bool		is_stoptype(int type);

#endif
