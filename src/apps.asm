; Built-in AXIOM programs, embedded into the kernel image.
section .rodata
global app_images, app_count

%macro APP 3
    dq id_%1, title_%1, bin_%1, bin_%1_end
%endmacro

align 8
app_images:
    APP files, "Files", 0
    APP terminal, "Terminal", 0
    APP monitor, "System Monitor", 0
    APP bluetooth, "Bluetooth", 0
    APP network, "Network", 0
    APP calc, "Calculator", 0
    APP sandbox, "Protection Test", 0
    APP settings, "Settings", 0
    APP viewer, "Viewer", 0
    APP web, "Web", 0
    APP about, "About AXIOM", 0
app_count: dd 11

id_files: db "files", 0
title_files: db "Files", 0
id_about: db "about", 0
title_about: db "About AXIOM", 0
id_terminal: db "terminal", 0
title_terminal: db "Terminal", 0
id_monitor: db "monitor", 0
title_monitor: db "System Monitor", 0
id_bluetooth: db "bluetooth", 0
title_bluetooth: db "Bluetooth", 0
id_network: db "network", 0
title_network: db "Network", 0
id_calc: db "calc", 0
title_calc: db "Calculator", 0
id_sandbox: db "sandbox", 0
title_sandbox: db "Protection Test", 0
id_settings: db "settings", 0
title_settings: db "Settings", 0
id_viewer: db "viewer", 0
title_viewer: db "Viewer", 0
id_web: db "web", 0
title_web: db "Web", 0

align 16
bin_files: incbin "build/user/files.elf"
bin_files_end:
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
bin_network: incbin "build/user/network.elf"
bin_network_end:
align 16
bin_calc: incbin "build/user/calc.elf"
bin_calc_end:
align 16
bin_sandbox: incbin "build/user/sandbox.elf"
bin_sandbox_end:
align 16
bin_settings: incbin "build/user/settings.elf"
bin_settings_end:
align 16
bin_viewer: incbin "build/user/viewer.elf"
bin_viewer_end:
align 16
bin_web: incbin "build/user/web.elf"
bin_web_end:
