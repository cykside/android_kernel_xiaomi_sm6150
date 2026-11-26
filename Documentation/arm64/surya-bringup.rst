Surya (POCO X3 NFC) bring-up
==========================

This is a Linux 6.1 bring-up stub, not a bootable port. Do not flash the
result. The board IDs come from the 4.14 ``sdmmagpie-idp-overlay-miui.dts``
on the ``cykside/msm-4.14`` branch ``arena/6ccacdbd-msm-4-14`` at commit
``290a8a5e9f7afc5f3b534762d6e31b6d74e251af``. The DTS uses this tree's
vendor SM6150 base without copying Violet peripherals or assuming that
Violet's bootloader and memory layout also apply to Surya.

Build the experimental DTB explicitly with the kernel's LLVM toolchain
on PATH::

    make O=out/surya ARCH=arm64 LLVM=1 gki_defconfig vendor/surya.config
    make O=out/surya ARCH=arm64 LLVM=1 vendor/qcom/surya-sm6150.dtb

The separate output directory keeps the Surya config isolated. The DTB
is deliberately omitted from the default ``dtbs`` build; the commands
do not create a boot image or DTBO.

Before an on-device test, validate ABL DTB/DTBO selection, reserved memory,
and PMIC/regulator and storage configuration against Surya firmware. Then
bring up USB, display (NT36672C Huaxing/Tianma), Novatek SPI touch, haptics,
battery authentication and charging, connectivity/modem, audio, and camera.
Android vendor userspace compatibility needs separate validation.

The Surya DTB smoke test runs on pull requests targeting ``seventeen-6.1``.
It checks that Kconfig retains the requested settings and that the compiled
DTB has the expected compatible strings and board IDs. Passing this check
does not validate the hardware description or make the DTB safe to flash.

Reserved memory comparison
--------------------------

A source comparison of 4.14 ``sdmmagpie.dtsi`` and the 6.1 vendor
``sm6150.dtsi`` found the following differences. Values are hexadecimal
and use start address / size. These are source values, not an ABL or
runtime memory dump.

================== ====================== ======================
Region             4.14 sdmmagpie         6.1 SM6150 base
================== ====================== ======================
XBL/AOP            85e00000 / 1ff000      85e00000 / 120000
AOP command DB     inside XBL/AOP         85f20000 / 20000
CDSP firmware      93900000 / 1e00000     93b00000 / 1e00000
ADSP firmware      95700000 / 1e00000     95900000 / 1e00000
WLAN MSA           97500000 / 180000      93900000 / 200000
Continuous splash  9c000000 / 1700000     9c000000 / f00000
Display FPS data   9d700000 / 100000      9cf00000 / 100000
Ramoops            9d800000 / 400000      not in base DTSI
================== ====================== ======================

The Surya DTS adds the 4.14 ramoops region and its record geometry for
boot diagnostics. It does not overlap a fixed reservation in the 6.1
source. Confirm the region against the firmware-provided memory map before
using it. The smoke test also checks the compiled region and record sizes.

Do not copy the other ranges individually: extending XBL/AOP as in 4.14
would overlap the separate 6.1 command DB, and shifting CDSP alone would
overlap 6.1 WLAN MSA. Confirm these as groups using the running 4.14 device's
live FDT and boot logs, and compare the stock DTB/DTBO from the exact firmware
version under test. Keep the board DTS out of shipping builds until that
comparison and an on-device boot test are complete.

UFS preparation
---------------

The Surya DTS maps the UFS controller and QMP V3 PHY supplies to the
PM6150/PM6150L rails named in 4.14 ``sdmmagpie-idp.dtsi``. Both nodes
remain disabled. Before enabling them, confirm the firmware rail votes,
particularly the L3C PLL/reference-clock rail (described as 1.2 V in
4.14; the 6.1 base permits 1.232–1.260 V), and verify the reset GPIO,
boot-device NVMEM gate, and UFS link on hardware. The DTB smoke test
checks that neither node can probe by default.
