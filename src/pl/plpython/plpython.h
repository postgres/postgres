#ifndef PLPYTHON_NEW_H
#define PLPYTHON_NEW_H

#define DEBUG_EXC 0
#define DEBUG_LEVEL 0

#define DECLARE_N_EXC(N) int rv_##N; sigjmp_buf buf_##N
#define TRAP_N_EXC(N) ((rv_##N = sigsetjmp(Warn_restart, 1)) != 0)

#if !DEBUG_EXC
# define RESTORE_N_EXC(N) memcpy(&Warn_restart, &(buf_##N), sizeof(sigjmp_buf))
# define SAVE_N_EXC(N) memcpy(&(buf_##N), &Warn_restart, sizeof(sigjmp_buf))
# define RERAISE_N_EXC(N) siglongjmp(Warn_restart, rv_##N)
# define RAISE_EXC(V) siglongjmp(Warn_restart, (V))
#else
# define RESTORE_N_EXC(N) do { \
   elog(NOTICE, "exception (%d,%d) restore at %s:%d",\
        PLy_call_level, exc_save_calls, __FUNCTION__, (__LINE__));\
   exc_save_calls -= 1; \
   memcpy(&Warn_restart, &(buf_##N), sizeof(sigjmp_buf)); } while (0)
# define SAVE_N_EXC(N) do { \
   exc_save_calls += 1; \
   elog(NOTICE, "exception (%d,%d) save at %s:%d", \
        PLy_call_level, exc_save_calls, __FUNCTION__, (__LINE__)); \
   memcpy(&(buf_##N), &Warn_restart, sizeof(sigjmp_buf)); } while (0)
# define RERAISE_N_EXC(N) do { \
   elog(NOTICE, "exception (%d,%d) reraise at %s:%d", \
   PLy_call_level, exc_save_calls, __FUNCTION__, (__LINE__)); \
   siglongjmp(Warn_restart, rv_##N); } while (0)
#define RAISE_EXC(V) do { \
   elog(NOTICE, "exception (%d,%d) raise at %s:%d", \
   PLy_call_level, exc_save_calls, __FUNCTION__, (__LINE__)); \
   siglongjmp(Warn_restart, (V)); } while (0)
#endif

#define DECLARE_EXC() DECLARE_N_EXC(save_restart)
#define SAVE_EXC() SAVE_N_EXC(save_restart)
#define RERAISE_EXC() RERAISE_N_EXC(save_restart)
#define RESTORE_EXC() RESTORE_N_EXC(save_restart)
#define TRAP_EXC() TRAP_N_EXC(save_restart)

#if DEBUG_LEVEL
# define CALL_LEVEL_INC() do { PLy_call_level += 1; \
    elog(NOTICE, "Level: %d", PLy_call_level); } while (0)
# define CALL_LEVEL_DEC() do { elog(NOTICE, "Level: %d", PLy_call_level); \
    PLy_call_level -= 1; } while (0)
#else
# define CALL_LEVEL_INC() do { PLy_call_level += 1; } while (0)
# define CALL_LEVEL_DEC() do { PLy_call_level -= 1; } while (0)
#endif

/* temporary debugging macros
 */
#if DEBUG_LEVEL
# define enter() elog(NOTICE, "Enter(%d): %s", func_enter_calls++,__FUNCTION__)
# define leave() elog(NOTICE, "Leave(%d): %s", func_leave_calls++,__FUNCTION__)
# define mark() elog(NOTICE, "Mark: %s:%d", __FUNCTION__, __LINE__);
# define refc(O) elog(NOTICE, "Ref<%p>:<%d>:%s:%d", (O), (((O) == NULL) ? -1 : (O)->ob_refcnt), __FUNCTION__, __LINE__)
#else
# define enter()
# define leave()
# define mark()
# define refc(O)
#endif

#endif
