# void rw_call_on_stack(void (*fn)(void), void *stack_top)
# Runs fn on another stack and returns to the caller's stack afterwards (OSSwitchFiber is not
# emulated by Cemu). The new stack's first frame back-chains to the caller's frame.
    .section .text
    .globl rw_call_on_stack
    .type rw_call_on_stack, @function
rw_call_on_stack:
    mflr    %r0
    stw     %r0, 4(%r1)          # LR in the caller's frame (SysV ABI slot)
    stwu    %r1, -16(%r1)        # our frame on the old stack
    clrrwi  %r4, %r4, 4          # 16-byte align the new top
    stwu    %r1, -16(%r4)        # new frame: back chain = our old frame; r4 = new sp
    mr      %r1, %r4
    mtctr   %r3
    bctrl
    lwz     %r1, 0(%r1)          # back to our frame on the old stack
    addi    %r1, %r1, 16
    lwz     %r0, 4(%r1)
    mtlr    %r0
    blr
    .size rw_call_on_stack, .-rw_call_on_stack
