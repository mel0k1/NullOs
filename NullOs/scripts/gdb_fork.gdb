set pagination off
set confirm off
target remote :1234

printf "\n=== CONNECTED, setting breakpoints ===\n"
break fork_resume_child
continue

printf "\n=== HIT fork_resume_child ===\n"
printf "rsp (points at cont[7]) = %p\n", $rsp
x/2gx $rsp
delete

# Step into the restore path
si
printf "\n=== after popq rsp: rsp=%p ===\n", $rsp
x/6gx $rsp

# Walk to iretq: single-step up to 60 instructions, print each
set $i = 0
while $i < 60
  printf "step %d: ", $i
  x/i $pc
  # stop stepping once we reach ring 3 past the iretq
  si
  set $i = $i + 1
end

printf "\n=== POST-IRETQ STATE ===\n"
info registers rip rsp rax rbx rcx rdx rsi rdi rbp r8 r9 r10 r11 r12 r13 r14 r15 eflags
printf "\n=== child-visible memory ===\n"
printf "code @0x400150:\n"
x/8gx 0x400150
printf "code bytes @0x400158:\n"
x/8bx 0x400158
printf "stack @rsp:\n"
x/4gx $rsp

printf "\n=== arming page_fault_handler breakpoint and continuing ===\n"
break page_fault_handler
continue

printf "\n=== FAULT HANDLER ENTERED ===\n"
info registers rip rsp
# cr2 is pushed in our frame; handler reads it itself — check serial log
x/6i $pc

kill
detach
