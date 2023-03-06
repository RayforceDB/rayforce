#ifndef VM_H
#define VM_H

#include "storm.h"

#define VM_STACK_SIZE 4096

typedef enum vm_opcode_t
{
    VM_HALT = 0, // Halt the VM
    VM_PUSH,     // Push a value to the stack
    VM_POP,      // Pop a value from the stack
    VM_ADD,      // Add two values from the stack
    VM_SUB,      // Subtract two values from the stack
    VM_MUL,      // Multiply two values from the stack
} vm_opcode_t;

typedef struct vm_t
{
    i32_t ip;    // Instruction pointer
    i32_t sp;    // Stack pointer
    i8_t halted; // Halt flag
    i64_t regs[16];
    struct value_t stack[VM_STACK_SIZE];
} *vm_t;

vm_t vm_create();
nil_t vm_exec(vm_t vm, u8_t *code);
nil_t vm_free(vm_t vm);

// void vm_init(VM *vm, int *code, int code_size, int nglobals);
// void vm_print_instr(i16_t *code, int ip);
// void vm_print_stack(int *stack, int count);
// void vm_print_data(int *globals, int count);

#endif
