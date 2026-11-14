; the other cores' start-up code (ap_tramp.asm, assembled as a flat binary)
section .rodata
global ap_tramp_start, ap_tramp_end
ap_tramp_start:
    incbin "build/k/ap_tramp.bin"
ap_tramp_end:
