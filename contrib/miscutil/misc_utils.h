#ifndef MISC_UTILS_H
#define MISC_UTILS_H

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

int			backend_pid(void);
int			unlisten(char *relname);
int			max(int x, int y);
int			min(int x, int y);
int			active_listeners(text *relname);

#endif
