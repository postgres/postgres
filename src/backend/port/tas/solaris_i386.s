/=============================================================================
/ tas.s -- test and set lock for solaris_i386
/=============================================================================

        .file   "tas.s"
        .text
        .align  16
.L1.text:

        .globl  tas
tas:
        pushl   %ebp            /save prev base pointer
        movl    %esp,%ebp       /new base pointer
        pushl   %ebx            /save prev bx
        movl    8(%ebp),%ebx    /load bx with address of lock
        movl    $255,%eax       /put something in ax
        xchgb   %al,(%ebx)      /swap lock value with "0"
        cmpb    $0,%al          /did we get the lock?
        jne     .Locked
        subl    %eax,%eax       /yes, we got it -- return 0
        jmp     .Finish
        .align  4
.Locked:
        movl    $1,%eax         /no, we didn't get it - return 1
.Finish:
        popl    %ebx            /restore prev bx
        movl    %ebp,%esp       /restore stack state
        popl    %ebp
        ret                     /return
        .align  4
        .type   tas,@function
        .size   tas,.-tas

