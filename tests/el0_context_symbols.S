// Synthetic link-time addresses for the native context fixture. Keep these
// in a separate object so native PC-relative relocations are resolved by
// the linker, as they are for the real kernel's linker-defined symbols.
.global __el0_stack_bottom
.set __el0_stack_bottom, 0x101000
.global __el0_stack_top
.set __el0_stack_top, 0x102000
.global __el0_stack1_bottom
.set __el0_stack1_bottom, 0x102000
.global __el0_stack1_top
.set __el0_stack1_top, 0x103000
.global el0_context_program
.set el0_context_program, 0x100000
.global el0_context_fault_program
.set el0_context_fault_program, 0x100010
.global el0_context_stack_fault_program
.set el0_context_stack_fault_program, 0x100020
.global el0_context_stack_read
.set el0_context_stack_read, 0x100098
.global el0_context_fault_read
.set el0_context_fault_read, 0x100090
.global el0_context_program_resume
.set el0_context_program_resume, 0x100080
.global el0_context_exit_resume
.set el0_context_exit_resume, 0x100100
.global el0_context_program_end
.set el0_context_program_end, 0x100104
.section .note.GNU-stack,"",%progbits
