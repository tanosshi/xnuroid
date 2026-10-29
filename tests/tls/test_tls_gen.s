	.text
	.file	"test_tls_gen.c"
	.globl	get_apple_tsd                   // -- Begin function get_apple_tsd
	.p2align	2
	.type	get_apple_tsd,@function
get_apple_tsd:                          // @get_apple_tsd
	.cfi_startproc
// %bb.0:
	mrs	x8, TPIDR_EL0
	add	x8, x8, :tprel_hi12:g_apple_tsd
	add	x0, x8, :tprel_lo12_nc:g_apple_tsd
	ret
.Lfunc_end0:
	.size	get_apple_tsd, .Lfunc_end0-get_apple_tsd
	.cfi_endproc
                                        // -- End function
	.globl	main                            // -- Begin function main
	.p2align	2
	.type	main,@function
main:                                   // @main
	.cfi_startproc
// %bb.0:
	stp	x29, x30, [sp, #-16]!           // 16-byte Folded Spill
	.cfi_def_cfa_offset 16
	mov	x29, sp
	.cfi_def_cfa w29, 16
	.cfi_offset w30, -8
	.cfi_offset w29, -16
	mrs	x8, TPIDR_EL0
	adrp	x0, .L.str
	add	x0, x0, :lo12:.L.str
	add	x8, x8, :tprel_hi12:g_apple_tsd
	add	x1, x8, :tprel_lo12_nc:g_apple_tsd
	bl	printf
	mov	w0, wzr
	.cfi_def_cfa wsp, 16
	ldp	x29, x30, [sp], #16             // 16-byte Folded Reload
	.cfi_def_cfa_offset 0
	.cfi_restore w30
	.cfi_restore w29
	ret
.Lfunc_end1:
	.size	main, .Lfunc_end1-main
	.cfi_endproc
                                        // -- End function
	.type	g_apple_tsd,@object             // @g_apple_tsd
	.section	.tbss,"awT",@nobits
	.globl	g_apple_tsd
	.p2align	3, 0x0
g_apple_tsd:
	.zero	4096
	.size	g_apple_tsd, 4096

	.type	.L.str,@object                  // @.str
	.section	.rodata.str1.1,"aMS",@progbits,1
.L.str:
	.asciz	"get_apple_tsd: %p\n"
	.size	.L.str, 19

	.ident	"Android (13624864, based on r530567e) clang version 19.0.1 (https://android.googlesource.com/toolchain/llvm-project 97a699bf4812a18fb657c2779f5296a4ab2694d2)"
	.section	".note.GNU-stack","",@progbits
