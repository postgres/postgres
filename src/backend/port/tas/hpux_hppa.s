
        .SPACE  $TEXT$,SORT=8
        .SUBSPA $CODE$,QUAD=0,ALIGN=4,ACCESS=44,CODE_ONLY,SORT=24
tas
        .PROC
        .CALLINFO CALLER,FRAME=0,ENTRY_SR=3
        .ENTRY
        LDO     15(%r26),%r31   ;offset 0x0
        DEPI    0,31,4,%r31     ;offset 0x4
        LDCWX   0(0,%r31),%r23  ;offset 0x8
        COMICLR,=       0,%r23,%r0      ;offset 0xc
        DEP,TR  %r0,31,32,%r28  ;offset 0x10
$00000001
        LDI     1,%r28  ;offset 0x14
$L0
        .EXIT
        BV,N    %r0(%r2)        ;offset 0x18
        .PROCEND ;in=26;out=28;


        .SPACE  $TEXT$
        .SUBSPA $CODE$
        .SPACE  $PRIVATE$,SORT=16
        .SUBSPA $DATA$,QUAD=1,ALIGN=8,ACCESS=31,SORT=16
        .SPACE  $TEXT$
        .SUBSPA $CODE$
        .EXPORT tas,ENTRY,PRIV_LEV=3,ARGW0=GR,RTNVAL=GR
        .END
