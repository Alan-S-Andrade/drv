        .global _start
_start:
        li      x1, -1
        sw      x1, 0(x1)
        lwu     x1, 0(x1)
        addi    x2, x1, 0
        addiw   x3, x1, 0
        li      x4, 1
        slli   x5, x4, 63
        srai   x6, x5, 63
        sraiw  x7, x1, 1
        srliw  x8, x1, 1
        li     x9, 2
        slliw x10, x9, 31
done:
        j done
