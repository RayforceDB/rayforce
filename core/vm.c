#include <stdio.h>
#include <string.h>
#include "vm.h"
#include "storm.h"
#include "alloc.h"

vm_t vm_create()
{
    vm_t vm;

    vm = (vm_t)storm_malloc(sizeof(struct vm_t));
    memset(vm, 0, sizeof(struct vm_t));
    return vm;
}

nil_t vm_exec(vm_t vm, u8_t *code)
{
    // i8_t *stack = vm->stack;
    // i64_t *regs = vm->regs;
    i32_t *ip = &vm->ip;
    // i32_t *sp = &vm->sp;

    while (!vm->halted)
    {
        switch (code[*ip++])
        {
        case VM_HALT:
            printf("HALT");
            vm->halted = 1;
            break;
        // case VM_PUSH:
        //     stack[*sp++] = regs[code[*ip++]];
        //     break;
        // case VM_POP:
        //     regs[code[*ip++]] = stack[--*sp];
        //     break;
        // case VM_ADD:
        //     regs[code[*ip++]] = stack[--(*sp)] + stack[--*sp];
        //     break;
        // case VM_SUB:
        //     regs[code[*ip++]] = stack[--*sp] - stack[--*sp];
        //     break;
        // case VM_MUL:
        //     regs[code[*ip++]] = stack[--*sp] * stack[--*sp];
        //     break;
        default:
            return;
        }
    }
}

nil_t vm_free(vm_t vm)
{
    storm_free(vm);
}

// str_t vm_code_fmt(u8_t *code)
// {
//     // TODO
// }
