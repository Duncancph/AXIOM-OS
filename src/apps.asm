; Built-in DuncOS programs, embedded into the kernel image.
section .rodata
global app_images, app_count

%macro APP 3
    dq id_%1, title_%1, bin_%1, bin_%1_end
%endmacro

align 8
app_images:
    APP about, "About DuncOS", 0
    APP terminal, "Terminal", 0
    APP monitor, "System Monitor", 0
    APP bluetooth, "Bluetooth", 0
    APP calc, "Calculator", 0
    APP sandbox, "Protection Test", 0
app_count: dd 6

id_about: db "about", 0
title_about: db "About DuncOS", 0
id_terminal: db "terminal", 0
title_terminal: db "Terminal", 0
id_monitor: db "monitor", 0
title_monitor: db "System Monitor", 0
id_bluetooth: db "bluetooth", 0
title_bluetooth: db "Bluetooth", 0
id_calc: db "calc", 0
title_calc: db "Calculator", 0
id_sandbox: db "sandbox", 0
title_sandbox: db "Protection Test", 0

align 16
bin_about: incbin "build/user/about.elf"
bin_about_end:
align 16
bin_terminal: incbin "build/user/terminal.elf"
bin_terminal_end:
align 16
bin_monitor: incbin "build/user/monitor.elf"
bin_monitor_end:
align 16
bin_bluetooth: incbin "build/user/bluetooth.elf"
bin_bluetooth_end:
align 16
bin_calc: incbin "build/user/calc.elf"
bin_calc_end:
align 16
bin_sandbox: incbin "build/user/sandbox.elf"
bin_sandbox_end:
