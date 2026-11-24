#define	SYSREG(op0,op1,Cn,Cm,op2)	SPR(((op0)<<19|(op1)<<16|(Cn)<<12|(Cm)<<8|(op2)<<5))
#define	SCTLR_EL1			SYSREG(3,0,1,0,0)

#define CTR_EL0 SYSREG(3,3,0,0,1)

#define NSH	(1<<2 | 3)
#define NSHST	(1<<2 | 2)
#define	SY	(3<<2 | 3)

TEXT start(SB), 1, $-4
_base:
	MOV	R0, R3
	MOV	R1, R4

	MOV	$setSB(SB), R0
	BL	rebase(SB)
	MOV	R0, R28

	MOV	R3, R0
	MOV	R4, 0x08(FP)
	B	efimain(SB)

TEXT rebase(SB), 1, $-4
	ADR	_base, R1
	SUB	$0x8200, R0
	ADD	R1, R0
	RETURN

TEXT eficall(SB), 1, $-4
	MOV	R0, R8
	MOV	0x08(FP), R0
	MOV	0x10(FP), R1
	MOV	0x18(FP), R2
	MOV	0x20(FP), R3
	MOV	0x28(FP), R4
	MOV	0x30(FP), R5
	MOV	0x38(FP), R6
	MOV	0x40(FP), R7
	B	(R8)

/*
 * Enter the kernel after cleaning firmware cache writes, Plan2001 Boot
 * ABI v1, ARM64 entry (sys/include/bootinfo.h): X0 = BootInfo, X1 = 0.
 * jump(entry, bootinfo), called by archjump() in archaa64.c.
 */
TEXT jump(SB), 1, $-4
	MSR	$0xF, DAIFSet
	MOV	R0, R3
	MOV	0x08(FP), R4
	BL	cachehandoff<>(SB)
	MOV	R4, R0
	MOV	$0, R1
	B	(R3)

/* Clean QEMU L1/L2 data caches before the kernel disables its MMU.
 * The frozen cache.v8.s supplies the set/way implementation. */
TEXT cachehandoff<>(SB), 1, $-4
	MOV	LR, R20
	MOV	R3, R21
	MOV	R4, R22
	BL	cachedwbinv(SB)
	BL	l2cacheuwbinv(SB)
	BL	cachesync(SB)
	MOV	R21, R3
	MOV	R22, R4
	RET	R20

/* Clean an image range to PoC; CTR_EL0 gives the minimum D-cache line. */
TEXT cacheclean(SB), 1, $-4
	MOV	8(FP), R1
	ADD	R0, R1
	MRS	CTR_EL0, R2
	LSR	$16, R2
	AND	$15, R2
	MOV	$4, R5
	LSL	R2, R5
	SUB	$1, R5, R6
	BIC	R6, R0
_clean:
	DC	R0, 3,7,10,1
	ADD	R5, R0
	CMP	R1, R0
	BLT	_clean
	DSB	$SY
	RETURN

TEXT cachesync(SB), 1, $-4
	DSB	$SY
	IC	R0, 0,7,5,0
	DSB	$SY
	ISB	$SY
	RETURN
