+ ld obj/kern/kernel
+ mk obj/kern/kernel.img
Could not open option rom 'sgabios.bin': No such file or directory
6828 decimal is 15254 octal!
Physical memory: 66556K available, base = 640K, extended = 65532K
Total Pages: 16639
npages = 16639, npages_basemem = 160
mp_code_beg = 7, mp_code_end = 8
Skipp 7
Oops = 647
EXTPHYSMEM = 100000
check_page_free_list(1) ok
check_page_alloc() succeeded!
check_page() succeeded!
check_kern_pgdir() succeeded!
check_page_free_list(0) ok
check_page_installed_pgdir() succeeded!
SMP: CPU 0 found 1 CPU(s)
enabled interrupts: 1 2 4
FS is running
TRAP frame at 0xf0268000 from CPU 0
  edi  0x00000000
  esi  0x00000000
  ebp  0xeebfdfd0
  oesp 0xf020cfdc
  ebx  0x00000000
  edx  0x00008a00
  ecx  0x0000000e
  eax  0xffff8a00
  es   0x----0023
  ds   0x----0023
  trap 0x0000000d General Protection
  err  0x00000000
  eip  0x00800b3f
  cs   0x----001b
  flag 0x00000292
  esp  0xeebfdfb8
  ss   0x----0023
I am the parent.  Forking the child...
I am the parent.  Running the child...
I am the child.  Spinning...
I am the parent.  Killing the child...
[00001001] destroying 00002000
[00001001] exiting gracefully
No runnable environments in the system!
Welcome to the JOS kernel monitor!
Type 'help' for a list of commands.

QEMU: Terminated via GDBstub
