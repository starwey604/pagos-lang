# Refresh the base digest and archive date together, with full regression.
FROM archlinux@sha256:8185e444e45ba166146b244b41f1cba2d7d91f3eddce533a839cc9591e0fa785
RUN printf 'Server = https://archive.archlinux.org/repos/2026/09/25/$repo/os/$arch\n' > /etc/pacman.d/mirrorlist \
    && pacman -Syyuu --noconfirm --needed \
       cmake ninja clang llvm lld compiler-rt gcc gtest python git time \
       qemu-system-riscv riscv64-elf-gcc \
    && pacman -Q
WORKDIR /work
