# ---- Aegis async runtime (stackful cooperative tasks, single thread) ----
# Included by the compiler when a program uses async/await/yield_now. Every task runs on its own
# 256 KiB mmap'd stack with a guard page. Calling an async fn starts the task immediately; it runs
# until it awaits something unfinished, calls yield_now(), or returns. `await` and dropping a Task
# wait for it; the main program is itself a task (ag__rt_main).
# Task layout: 0 saved rsp | 8 stack | 16 done | 24 waiter | 32 next (ready queue) | 40 entry
#              48..88 argument registers | 96 result size | 104 result is aggregate | 112 result
	.text
ag__rt_switch:                          # rdi = &from.rsp, rsi = &to.rsp
	push rbp
	push rbx
	push r12
	push r13
	push r14
	push r15
	mov QWORD PTR [rdi], rsp
	mov rsp, QWORD PTR [rsi]
	pop r15
	pop r14
	pop r13
	pop r12
	pop rbx
	pop rbp
	ret
ag__rt_enqueue:                         # rdi = task, appended to the ready queue
	mov QWORD PTR [rdi+32], 0
	mov rax, QWORD PTR [rip+ag__rt_tail]
	test rax, rax
	jz .Lrt_eq_empty
	mov QWORD PTR [rax+32], rdi
	jmp .Lrt_eq_tail
.Lrt_eq_empty:
	mov QWORD PTR [rip+ag__rt_head], rdi
.Lrt_eq_tail:
	mov QWORD PTR [rip+ag__rt_tail], rdi
	ret
ag__rt_push_front:                      # rdi = task, runs next
	mov rax, QWORD PTR [rip+ag__rt_head]
	mov QWORD PTR [rdi+32], rax
	mov QWORD PTR [rip+ag__rt_head], rdi
	cmp QWORD PTR [rip+ag__rt_tail], 0
	jne .Lrt_pf_ret
	mov QWORD PTR [rip+ag__rt_tail], rdi
.Lrt_pf_ret:
	ret
ag__rt_dequeue:                         # rax = next ready task or 0
	mov rax, QWORD PTR [rip+ag__rt_head]
	test rax, rax
	jz .Lrt_dq_ret
	mov rcx, QWORD PTR [rax+32]
	mov QWORD PTR [rip+ag__rt_head], rcx
	test rcx, rcx
	jnz .Lrt_dq_ret
	mov QWORD PTR [rip+ag__rt_tail], 0
.Lrt_dq_ret:
	ret
ag__rt_run_next:                        # give the CPU to the next ready task
	push rbp
	mov rbp, rsp
	call ag__rt_dequeue
	test rax, rax
	jz .Lrt_deadlock
	mov rdi, QWORD PTR [rip+ag__rt_cur]
	cmp rax, rdi
	je .Lrt_rn_ret
	mov QWORD PTR [rip+ag__rt_cur], rax
	mov rsi, rax
	call ag__rt_switch
.Lrt_rn_ret:
	pop rbp
	ret
.Lrt_deadlock:
	mov eax, 1
	mov edi, 2
	lea rsi, [rip+.Lrt_dlmsg]
	mov edx, 39
	syscall
	mov eax, 231
	mov edi, 1
	syscall
ag__rt_spawn:                           # rdi = entry, rsi = saved argument registers, rdx = result size, rcx = aggregate?
	push rbp
	mov rbp, rsp
	push rbx
	push r12
	push r13
	push r14
	mov r12, rdi
	mov r13, rsi
	mov r14, rdx
	mov rbx, rcx
	lea rsi, [r14+120]
	mov edi, 1
	call calloc
	mov QWORD PTR [rax+40], r12
	mov QWORD PTR [rax+96], r14
	mov QWORD PTR [rax+104], rbx
	mov rbx, rax
	mov rax, QWORD PTR [r13]
	mov QWORD PTR [rbx+48], rax
	mov rax, QWORD PTR [r13+8]
	mov QWORD PTR [rbx+56], rax
	mov rax, QWORD PTR [r13+16]
	mov QWORD PTR [rbx+64], rax
	mov rax, QWORD PTR [r13+24]
	mov QWORD PTR [rbx+72], rax
	mov rax, QWORD PTR [r13+32]
	mov QWORD PTR [rbx+80], rax
	mov rax, QWORD PTR [r13+40]
	mov QWORD PTR [rbx+88], rax
	mov eax, 9                          # mmap(0, 256K, RW, PRIVATE|ANONYMOUS, -1, 0)
	xor edi, edi
	mov esi, 262144
	mov edx, 3
	mov r10d, 34
	mov r8, -1
	xor r9d, r9d
	syscall
	mov QWORD PTR [rbx+8], rax
	mov r12, rax
	mov eax, 10                         # mprotect(stack, 4096, NONE): guard page
	mov rdi, r12
	mov esi, 4096
	xor edx, edx
	syscall
	lea rax, [r12+262080]               # initial frame: 6 zero registers, then the trampoline as return address
	lea rcx, [rip+ag__rt_trampoline]
	mov QWORD PTR [rax+48], rcx
	mov QWORD PTR [rbx], rax
	mov rdi, QWORD PTR [rip+ag__rt_cur] # the spawner continues as soon as the task suspends or ends
	call ag__rt_push_front
	mov rdi, QWORD PTR [rip+ag__rt_cur]
	mov QWORD PTR [rip+ag__rt_cur], rbx
	mov rsi, rbx
	call ag__rt_switch
	mov rax, rbx
	pop r14
	pop r13
	pop r12
	pop rbx
	pop rbp
	ret
ag__rt_trampoline:                      # first code a task runs, on its own stack
	sub rsp, 8
	mov rbx, QWORD PTR [rip+ag__rt_cur]
	cmp QWORD PTR [rbx+104], 0
	jne .Lrt_tr_agg
	mov rdi, QWORD PTR [rbx+48]
	mov rsi, QWORD PTR [rbx+56]
	mov rdx, QWORD PTR [rbx+64]
	mov rcx, QWORD PTR [rbx+72]
	mov r8, QWORD PTR [rbx+80]
	mov r9, QWORD PTR [rbx+88]
	call QWORD PTR [rbx+40]
	mov rbx, QWORD PTR [rip+ag__rt_cur]
	mov QWORD PTR [rbx+112], rax
	jmp .Lrt_tr_done
.Lrt_tr_agg:
	lea rdi, [rbx+112]
	mov rsi, QWORD PTR [rbx+48]
	mov rdx, QWORD PTR [rbx+56]
	mov rcx, QWORD PTR [rbx+64]
	mov r8, QWORD PTR [rbx+72]
	mov r9, QWORD PTR [rbx+80]
	call QWORD PTR [rbx+40]
	mov rbx, QWORD PTR [rip+ag__rt_cur]
.Lrt_tr_done:
	mov QWORD PTR [rbx+16], 1
	mov rdi, QWORD PTR [rbx+24]
	test rdi, rdi
	jz .Lrt_tr_next
	call ag__rt_enqueue
.Lrt_tr_next:
	call ag__rt_run_next                # never comes back: this task is finished
	ud2
ag__rt_join:                            # rdi = task: wait until it has finished
	push rbp
	mov rbp, rsp
	push rbx
	sub rsp, 8
	mov rbx, rdi
.Lrt_jn_loop:
	cmp QWORD PTR [rbx+16], 0
	jne .Lrt_jn_done
	cmp QWORD PTR [rbx+24], 0
	jne .Lrt_twice
	mov rax, QWORD PTR [rip+ag__rt_cur]
	mov QWORD PTR [rbx+24], rax
	call ag__rt_run_next
	jmp .Lrt_jn_loop
.Lrt_jn_done:
	add rsp, 8
	pop rbx
	pop rbp
	ret
.Lrt_twice:
	mov eax, 1
	mov edi, 2
	lea rsi, [rip+.Lrt_twmsg]
	mov edx, 38
	syscall
	mov eax, 231
	mov edi, 1
	syscall
ag__rt_free:                            # rdi = finished task: release its stack and record
	push rbp
	mov rbp, rsp
	push rbx
	sub rsp, 8
	mov rbx, rdi
	mov eax, 11
	mov rdi, QWORD PTR [rbx+8]
	mov esi, 262144
	syscall
	mov rdi, rbx
	call free
	add rsp, 8
	pop rbx
	pop rbp
	ret
ag__rt_await:                           # rdi = task, rsi = destination, rdx = size: wait, move the result out, free
	push rbp
	mov rbp, rsp
	push rbx
	push r12
	push r13
	push r14
	mov rbx, rdi
	mov r12, rsi
	mov r13, rdx
	mov rdi, rbx
	call ag__rt_join
	mov rdi, r12
	lea rsi, [rbx+112]
	mov rdx, r13
	call memcpy
	mov rdi, rbx
	call ag__rt_free
	pop r14
	pop r13
	pop r12
	pop rbx
	pop rbp
	ret
ag__rt_yield:                           # let the other ready tasks run first
	push rbp
	mov rbp, rsp
	mov rdi, QWORD PTR [rip+ag__rt_cur]
	call ag__rt_enqueue
	call ag__rt_run_next
	pop rbp
	ret
	.data
	.balign 8
ag__rt_cur:
	.quad ag__rt_main
ag__rt_head:
	.quad 0
ag__rt_tail:
	.quad 0
ag__rt_main:
	.zero 128
	.section .rodata
.Lrt_dlmsg:
	.ascii "aegis: deadlock: all tasks are waiting\n"
.Lrt_twmsg:
	.ascii "aegis: a task was awaited twice (bug)\n"
	.text
