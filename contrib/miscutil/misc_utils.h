#ifndef MISC_UTILS_H
#define MISC_UTILS_H

int			backend_pid(void);
int			unlisten(char *relname);
int			int4max(int x, int y);
int			int4min(int x, int y);
int			active_listeners(text *relname);

#endif
