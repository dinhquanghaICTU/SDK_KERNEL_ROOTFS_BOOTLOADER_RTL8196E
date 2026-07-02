
                    HƯỚNG DẪN BUILD VÀ CÀI MÔI TRƯỜNG CHO APP


I.CÀI MÔI TRƯỜNG

    cd ~/SDK_KERNEL_ROOTFS_BOOTLOADER_RTL8196E/1-Build-Environment
    sudo ./install_deps.sh

lưu ý: cài đoạn này hơi lâu em cài mất tầm 30 phút (tùy vào mạng nó kéo src về nữa) để nó cài toolchain và môi trường cross compile

II. THỬ BIÊN DỊCH APP

    Ví dụ này em đã viết 1 cái app blink led màu đỏ trên board có thể xem device tree của nó ở đây.
    Còn led màu xanh lá cây là led của ethernet mặc định cắm ethernet vào là có.

    SDK_KERNEL_ROOTFS_BOOTLOADER_RTL8196E/3-Main-SoC-Realtek-RTL8196E/32-Kernel/files-6.18/arch/mips/boot/dts/realtek/rtl8196e.dts



    App em viết để blink led nằm ở đây 
    SDK_KERNEL_ROOTFS_BOOTLOADER_RTL8196E/APP_EXAMPLE







