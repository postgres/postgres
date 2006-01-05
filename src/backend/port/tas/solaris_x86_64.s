/=============================================================================
/ tas.s -- test and set lock for solaris_i386
/ based on i386 ASM with modifications outlined in:
/   http://www.x86-64.org/documentation/assembly.
/ This might require flags:  -xtarget=opteron -xarch=amd64
/ DB optimization documenation at:
/   http://developers.sun.com/solaris/articles/mysql_perf_tune.html
/=============================================================================

        .file   "tas.s"
        .text
        .align  16
.L1.text:

        .globl  tas
tas:
        pushq   %rbp            /save prev base pointer
        movq    %rsp,%rbp       /new base pointer
        pushq   %rbx            /save prev bx
        movq    8(%rbp),%rbx    /load bx with address of lock
        movq    $255,%rax       /put something in ax
        xchgb   %al,(%rbx)      /swap lock value with "0"
        cmpb    $0,%al          /did we get the lock?
        jne     .Locked
        subq    %rax,%rax       /yes, we got it -- return 0
        jmp     .Finish
        .align  8
.Locked:
        movq    $1,%rax         /no, we didn't get it - return 1
.Finish:
        popq    %rbx            /restore prev bx
        movq    %rbp,%rsp       /restore stack state
        popq    %rbp
        ret                     /return
        .align  8
        .type   tas,@function
        .size   tas,.-tas

