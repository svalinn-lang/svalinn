# Aegis runtime: clear messages for runtime errors (appended to every program by the compiler).
# Every runtime error flushes the program's buffered output, prints one line to stderr and exits with code 101:
#   array or String index out of bounds (bounds checks jump to ag__rt_trap)
#   integer division by zero / overflow (SIGFPE), stack overflow / invalid memory access (SIGSEGV, SIGBUS);
#   the signal handlers run on their own stack, so a stack overflow can still be reported.
	.text
ag__rt_init:
	push rbp
	push rdi
	push rsi
	lea rax, [rip+ag__rt_altstack]
	mov QWORD PTR [rip+ag__rt_ss], rax
	mov QWORD PTR [rip+ag__rt_ss+16], 65536
	lea rdi, [rip+ag__rt_ss]
	xor esi, esi
	call sigaltstack
	lea rax, [rip+ag__rt_fault]
	mov QWORD PTR [rip+ag__rt_sa], rax
	mov DWORD PTR [rip+ag__rt_sa+136], 0x88000000
	mov edi, 8
	lea rsi, [rip+ag__rt_sa]
	xor edx, edx
	call sigaction
	mov edi, 11
	lea rsi, [rip+ag__rt_sa]
	xor edx, edx
	call sigaction
	mov edi, 7
	lea rsi, [rip+ag__rt_sa]
	xor edx, edx
	call sigaction
	pop rsi
	pop rdi
	pop rbp
	ret
ag__rt_fault:
	mov ebx, edi
	and rsp, -16
	xor edi, edi
	call fflush
	lea rsi, [rip+.Lrt_segv]
	mov edx, 49
	cmp ebx, 8
	jne .Lrt_say
	lea rsi, [rip+.Lrt_fpe]
	mov edx, 46
	jmp .Lrt_say
ag__rt_trap:
	and rsp, -16
	xor edi, edi
	call fflush
	lea rsi, [rip+.Lrt_oob]
	mov edx, 27
.Lrt_say:
	mov edi, 2
	mov eax, 1
	syscall
	mov edi, 101
	call _exit
	.section .rodata
.Lrt_oob:
	.ascii "aegis: index out of bounds\n"
.Lrt_fpe:
	.ascii "aegis: integer division by zero (or overflow)\n"
.Lrt_segv:
	.ascii "aegis: stack overflow (or invalid memory access)\n"
	.bss
	.balign 16
ag__rt_altstack:
	.zero 65536
ag__rt_ss:
	.zero 24
ag__rt_sa:
	.zero 152
	.text
