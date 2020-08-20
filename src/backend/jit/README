What is Just-in-Time Compilation?
=================================

Just-in-Time compilation (JIT) is the process of turning some form of
interpreted program evaluation into a native program, and doing so at
runtime.

For example, instead of using a facility that can evaluate arbitrary
SQL expressions to evaluate an SQL predicate like WHERE a.col = 3, it
is possible to generate a function than can be natively executed by
the CPU that just handles that expression, yielding a speedup.

This is JIT, rather than ahead-of-time (AOT) compilation, because it
is done at query execution time, and perhaps only in cases where the
relevant task is repeated a number of times. Given the way JIT
compilation is used in PostgreSQL, the lines between interpretation,
AOT and JIT are somewhat blurry.

Note that the interpreted program turned into a native program does
not necessarily have to be a program in the classical sense. E.g. it
is highly beneficial to JIT compile tuple deforming into a native
function just handling a specific type of table, despite tuple
deforming not commonly being understood as a "program".


Why JIT?
========

Parts of PostgreSQL are commonly bottlenecked by comparatively small
pieces of CPU intensive code. In a number of cases that is because the
relevant code has to be very generic (e.g. handling arbitrary SQL
level expressions, over arbitrary tables, with arbitrary extensions
installed). This often leads to a large number of indirect jumps and
unpredictable branches, and generally a high number of instructions
for a given task. E.g. just evaluating an expression comparing a
column in a database to an integer ends up needing several hundred
cycles.

By generating native code large numbers of indirect jumps can be
removed by either making them into direct branches (e.g. replacing the
indirect call to an SQL operator's implementation with a direct call
to that function), or by removing it entirely (e.g. by evaluating the
branch at compile time because the input is constant). Similarly a lot
of branches can be entirely removed (e.g. by again evaluating the
branch at compile time because the input is constant). The latter is
particularly beneficial for removing branches during tuple deforming.


How to JIT
==========

PostgreSQL, by default, uses LLVM to perform JIT. LLVM was chosen
because it is developed by several large corporations and therefore
unlikely to be discontinued, because it has a license compatible with
PostgreSQL, and because its IR can be generated from C using the Clang
compiler.


Shared Library Separation
-------------------------

To avoid the main PostgreSQL binary directly depending on LLVM, which
would prevent LLVM support being independently installed by OS package
managers, the LLVM dependent code is located in a shared library that
is loaded on-demand.

An additional benefit of doing so is that it is relatively easy to
evaluate JIT compilation that does not use LLVM, by changing out the
shared library used to provide JIT compilation.

To achieve this, code intending to perform JIT (e.g. expression evaluation)
calls an LLVM independent wrapper located in jit.c to do so. If the
shared library providing JIT support can be loaded (i.e. PostgreSQL was
compiled with LLVM support and the shared library is installed), the task
of JIT compiling an expression gets handed off to the shared library. This
obviously requires that the function in jit.c is allowed to fail in case
no JIT provider can be loaded.

Which shared library is loaded is determined by the jit_provider GUC,
defaulting to "llvmjit".

Cloistering code performing JIT into a shared library unfortunately
also means that code doing JIT compilation for various parts of code
has to be located separately from the code doing so without
JIT. E.g. the JIT version of execExprInterp.c is located in jit/llvm/
rather than executor/.


JIT Context
-----------

For performance and convenience reasons it is useful to allow JITed
functions to be emitted and deallocated together. It is e.g. very
common to create a number of functions at query initialization time,
use them during query execution, and then deallocate all of them
together at the end of the query.

Lifetimes of JITed functions are managed via JITContext. Exactly one
such context should be created for work in which all created JITed
function should have the same lifetime. E.g. there's exactly one
JITContext for each query executed, in the query's EState.  Only the
release of a JITContext is exposed to the provider independent
facility, as the creation of one is done on-demand by the JIT
implementations.

Emitting individual functions separately is more expensive than
emitting several functions at once, and emitting them together can
provide additional optimization opportunities. To facilitate that, the
LLVM provider separates defining functions from optimizing and
emitting functions in an executable manner.

Creating functions into the current mutable module (a module
essentially is LLVM's equivalent of a translation unit in C) is done
using
  extern LLVMModuleRef llvm_mutable_module(LLVMJitContext *context);
in which it then can emit as much code using the LLVM APIs as it
wants. Whenever a function actually needs to be called
  extern void *llvm_get_function(LLVMJitContext *context, const char *funcname);
returns a pointer to it.

E.g. in the expression evaluation case this setup allows most
functions in a query to be emitted during ExecInitNode(), delaying the
function emission to the time the first time a function is actually
used.


Error Handling
--------------

There are two aspects of error handling.  Firstly, generated (LLVM IR)
and emitted functions (mmap()ed segments) need to be cleaned up both
after a successful query execution and after an error. This is done by
registering each created JITContext with the current resource owner,
and cleaning it up on error / end of transaction. If it is desirable
to release resources earlier, jit_release_context() can be used.

The second, less pretty, aspect of error handling is OOM handling
inside LLVM itself. The above resowner based mechanism takes care of
cleaning up emitted code upon ERROR, but there's also the chance that
LLVM itself runs out of memory. LLVM by default does *not* use any C++
exceptions. Its allocations are primarily funneled through the
standard "new" handlers, and some direct use of malloc() and
mmap(). For the former a 'new handler' exists:
http://en.cppreference.com/w/cpp/memory/new/set_new_handler
For the latter LLVM provides callbacks that get called upon failure
(unfortunately mmap() failures are treated as fatal rather than OOM errors).
What we've chosen to do for now is have two functions that LLVM using code
must use:
extern void llvm_enter_fatal_on_oom(void);
extern void llvm_leave_fatal_on_oom(void);
before interacting with LLVM code.

When a libstdc++ new or LLVM error occurs, the handlers set up by the
above functions trigger a FATAL error. We have to use FATAL rather
than ERROR, as we *cannot* reliably throw ERROR inside a foreign
library without risking corrupting its internal state.

Users of the above sections do *not* have to use PG_TRY/CATCH blocks,
the handlers instead are reset on toplevel sigsetjmp() level.

Using a relatively small enter/leave protected section of code, rather
than setting up these handlers globally, avoids negative interactions
with extensions that might use C++ such as PostGIS. As LLVM code
generation should never execute arbitrary code, just setting these
handlers temporarily ought to suffice.


Type Synchronization
--------------------

To be able to generate code that can perform tasks done by "interpreted"
PostgreSQL, it obviously is required that code generation knows about at
least a few PostgreSQL types.  While it is possible to inform LLVM about
type definitions by recreating them manually in C code, that is failure
prone and labor intensive.

Instead there is one small file (llvmjit_types.c) which references each of
the types required for JITing. That file is translated to bitcode at
compile time, and loaded when LLVM is initialized in a backend.

That works very well to synchronize the type definition, but unfortunately
it does *not* synchronize offsets as the IR level representation doesn't
know field names.  Instead, required offsets are maintained as defines in
the original struct definition, like so:
#define FIELDNO_TUPLETABLESLOT_NVALID 9
        int                     tts_nvalid;             /* # of valid values in tts_values */
While that still needs to be defined, it's only required for a
relatively small number of fields, and it's bunched together with the
struct definition, so it's easily kept synchronized.


Inlining
--------

One big advantage of JITing expressions is that it can significantly
reduce the overhead of PostgreSQL's extensible function/operator
mechanism, by inlining the body of called functions/operators.

It obviously is undesirable to maintain a second implementation of
commonly used functions, just for inlining purposes. Instead we take
advantage of the fact that the Clang compiler can emit LLVM IR.

The ability to do so allows us to get the LLVM IR for all operators
(e.g. int8eq, float8pl etc), without maintaining two copies.  These
bitcode files get installed into the server's
  $pkglibdir/bitcode/postgres/
Using existing LLVM functionality (for parallel LTO compilation),
additionally an index is over these is stored to
$pkglibdir/bitcode/postgres.index.bc

Similarly extensions can install code into
  $pkglibdir/bitcode/[extension]/
accompanied by
  $pkglibdir/bitcode/[extension].index.bc

just alongside the actual library.  An extension's index will be used
to look up symbols when located in the corresponding shared
library. Symbols that are used inside the extension, when inlined,
will be first looked up in the main binary and then the extension's.


Caching
-------

Currently it is not yet possible to cache generated functions, even
though that'd be desirable from a performance point of view. The
problem is that the generated functions commonly contain pointers into
per-execution memory. The expression evaluation machinery needs to
be redesigned a bit to avoid that. Basically all per-execution memory
needs to be referenced as an offset to one block of memory stored in
an ExprState, rather than absolute pointers into memory.

Once that is addressed, adding an LRU cache that's keyed by the
generated LLVM IR will allow the usage of optimized functions even for
faster queries.

A longer term project is to move expression compilation to the planner
stage, allowing e.g. to tie compiled expressions to prepared
statements.

An even more advanced approach would be to use JIT with few
optimizations initially, and build an optimized version in the
background. But that's even further off.


What to JIT
===========

Currently expression evaluation and tuple deforming are JITed. Those
were chosen because they commonly are major CPU bottlenecks in
analytics queries, but are by no means the only potentially beneficial cases.

For JITing to be beneficial a piece of code first and foremost has to
be a CPU bottleneck. But also importantly, JITing can only be
beneficial if overhead can be removed by doing so. E.g. in the tuple
deforming case the knowledge about the number of columns and their
types can remove a significant number of branches, and in the
expression evaluation case a lot of indirect jumps/calls can be
removed.  If neither of these is the case, JITing is a waste of
resources.

Future avenues for JITing are tuple sorting, COPY parsing/output
generation, and later compiling larger parts of queries.


When to JIT
===========

Currently there are a number of GUCs that influence JITing:

- jit_above_cost = -1, 0-DBL_MAX - all queries with a higher total cost
  get JITed, *without* optimization (expensive part), corresponding to
  -O0. This commonly already results in significant speedups if
  expression/deforming is a bottleneck (removing dynamic branches
  mostly).
- jit_optimize_above_cost = -1, 0-DBL_MAX - all queries with a higher total cost
  get JITed, *with* optimization (expensive part).
- jit_inline_above_cost = -1, 0-DBL_MAX - inlining is tried if query has
  higher cost.

Whenever a query's total cost is above these limits, JITing is
performed.

Alternative costing models, e.g. by generating separate paths for
parts of a query with lower cpu_* costs, are also a possibility, but
it's doubtful the overhead of doing so is sufficient.  Another
alternative would be to count the number of times individual
expressions are estimated to be evaluated, and perform JITing of these
individual expressions.

The obvious seeming approach of JITing expressions individually after
a number of execution turns out not to work too well. Primarily
because emitting many small functions individually has significant
overhead. Secondarily because the time until JITing occurs causes
relative slowdowns that eat into the gain of JIT compilation.
