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

USB and Type-C preparation
--------------------------

The USB nodes in ``sm6150-usb.dtsi`` are available by default. The Surya
DTS explicitly disables both controller wrappers until role detection is
ready. The fragment selects the downstream DWC3 MSM and QUSB2 PHY drivers;
GKI already selects the ConfigFS FunctionFS gadget support needed by ADB.

The 4.14 ``sdmmagpie.dtsi`` routes primary USB events through the PM6150
PD PHY, charger and EUD extcon providers. The generic 6.1 SM6150 base does
not supply the same board-specific PMIC policy nodes. Do not copy Violet's
charger configuration or force peripheral mode to bypass role detection.
Port the Surya PMIC/Type-C event path first, then verify VBUS detection,
USB role changes and gadget enumeration on hardware before testing ADB.

``surya-pm6150.dtsi`` stages the PM6150 PD PHY and the SMB5 register and
IRQ layout from 4.14, with single-cell charger peripheral addresses for
6.1. It connects the PHY to the SMB5 VBUS/VCONN regulators and the 6.1
policy engine's IIO channels, and describes the primary DWC3 extcon
providers. The charger, PHY and USB wrappers all remain disabled.

The charger still needs verified ADC inputs, battery data and thermal
limits. Do not use default driver limits or copy Violet's battery policy.
The smoke test verifies that the staging nodes remain disabled.

The disabled PM6150 ADC5 node maps the six on-chip SMB5 measurements
used by Surya's 4.14 charger: USB input voltage/current, charger and die
temperature, SBU resistance and VPH voltage. The disabled PM6150L ADC5
node supplies the connector temperature channel. The 4.14 AMUX THM1
PU2 channel ID (0x4d) maps to the 6.1 100 kOhm pull-up channel; the
ratiometric setting and 200 us settle time are preserved. Compare the
ADC readings, especially the connector thermistor curve, against stock
hardware before enabling charging. The DTB test checks the description,
not the readings.

The disabled SMB5 node also carries the six Surya thermal mitigation
tables supported by the 6.1 driver, copied from 4.14
``sdmmagpie-idp.dtsi``. The smoke test verifies that each table has 16
non-increasing entries. Charge-pump-specific tables, battery profile,
and fast-charge policy are not ported here. Table parity does not
validate temperature calibration, battery limits, or charger behavior.

The disabled PM6150 QGauge node describes the stock gauge and SDAM
registers and the 100 kOhm pull-up battery thermistor and ID channels.
The 4.14 board DTS includes both M703 ATL 5160 mAh and 6000 mAh profiles.
Do not choose a profile by capacity alone: compare the battery ID
resistance and pack markings with stock, then validate profile selection,
temperature, and state of charge before enabling the gauge or SMB5.

SMB5 now references the 6.1 QGauge IIO outputs for battery telemetry.
Both nodes remain disabled and neither selects a profile. Both stock
profiles declare the same 100 kOhm battery ID and the board uses a 15%
match range, so resistance alone cannot distinguish them. Identify the
pack and firmware variant before linking a profile. QGauge also references
SMB5 for input-current limiting, recharge threshold, forced recharge and
charge completion. The smoke test checks the provider references in both
directions. Before enabling the nodes, verify that both drivers bind
without a persistent probe dependency and that the reported charger
state matches stock.

Driver compile checks
---------------------

A DTB build does not compile kernel drivers. The CI smoke test also
compiles the selected SMB5, QGauge, USB-PD, DWC3, ADC5 and UFS code using
the resolved Surya configuration. This is an object-level check, not a
full kernel link or an on-device probe test. The workflow does not
produce a flashable artifact.

NT36672C panels
---------------

The Huaxing and Tianma timing and DSI command data are staged from the
4.14 board tree under the disabled SM6150 MDP node. No default panel
is selected. The smoke test checks
that both 1080x2400 at 120 Hz definitions are present and disabled.

Before attaching them to the 6.1 display host, validate the DSI PHY
settings and topology, ABL panel selection, TE on TLMM GPIO 10, reset on
PM6150L GPIO 9, and LAB/IBB and WLED supplies. Test the actual panel
variant and bring up a stable single mode before testing dynamic refresh.

Both staged panel nodes now describe the stock TE and reset wiring.
The PM6150L GPIO controller used for reset is present but disabled, so
this change does not toggle a pin. Host-side pinctrl, power rails,
backlight, and reset sequencing remain to be validated before probing.

The PM6150L SID 5 nodes describe the LCDB bias regulators and WLED
backlight using 6.1 bindings and the stock Surya WLED string mask.
Both providers are disabled. The WLED flash children and automatic
string calibration are not enabled. Validate the string wiring, current
limit, and panel rail enable/disable sequence before enabling the SDE/DSI host.

Both panels now reference the stock Surya supply table from 4.14
``sdmmagpie-sde-display.dtsi``. It describes VDDIO, LAB and IBB votes
and the 20 ms delay after turning on IBB. The panel nodes also carry
the DSI0/PHY0 and clock names in the locations the 6.1 parser expects.
The disabled DSI host maps ``vddio`` to PM6150 L18 and ``lab``/``ibb``
to LCDB LDO/NCP. SoC-level SDE, DSI and PHY definitions are shared with
Violet; its panel selection, supplies and pinctrl stay in its board
include. Surya still needs its own host pinctrl, PHY mode timing, and
on-device voltage and reset validation before enabling the display.

The stock DSC topology (one mixer, one compression encoder and one
interface) and DSI clock pre/post values are carried over for both panels.
The stock 12-byte PHY timing table is not passed to the 6.1 14 nm PHY,
whose known board configuration uses 40 per-lane timing bytes. Do not
pad the stock table or copy Violet's values. Also, the current SM6150
pinctrl driver does not expose the stock ``mdp_vsync`` mux on GPIO 10;
verify the SoC pinmux selection before adding a host pinctrl state.

The Surya fragment selects the downstream SM6150 display stack and the
matching display clock controller, which CI compiles separately. The
6.1 DSI driver can calculate PHY timing on enable when the DT has no
explicit timing bytes. For a first single-mode hardware test, validate
that calculated timing against the real lane rate rather than porting
incompatible bytes. The stock 4.14 sdmmagpie pinctrl table also puts
GPIO 10 in the NORTH tile and selects ``mdp_vsync`` at mux slot 1; the
6.1 table puts it in EAST and uses slot 1 for ``qup13``. Leave the shared
mux table unchanged until the Surya pinctrl variant is established.

Novatek SPI touch
-----------------

The stock J20 touch device is staged at chip select 0 on QUPv3 SE0,
with its 19.2 MHz SPI limit, reset/IRQ GPIOs and L18/LCDB supplies. The
SPI node and touch client are disabled. SE0 overlaps the base debug
UART at 0x880000 (including its IRQ); do not enable SPI until the UART
is relocated or disabled and the Surya SPI pinmux is verified. The
legacy Novatek SPI driver and firmware selection still need a 6.1 port;
this change does not import factory test tables.

The stock SPI active state uses ``qup00`` on GPIOs 49–52 (WEST tile,
mux slot 2). The 6.1 SM6150 pinctrl table only advertises ``qup00`` on
GPIOs 16–17 and places GPIOs 49–52 in EAST/SOUTH. Do not attach the
stock pinctrl state to the current driver or change the shared table
without a verified Surya-specific controller variant.

Haptics
-------

The 4.14 haptic wiring is staged on QUPv3 SE1 I2C at address 0x5a,
with reset on TLMM GPIO 63 and IRQ on GPIO 26. Both the host and the
client remain disabled. The stock DTS lists AW8624 and DRV2624; identify
the populated chip before choosing a driver. Waveforms, calibration
and actuator voltage settings are deliberately not imported here.

Full kernel link check
----------------------

After the smoke test passes, CI builds ``Image.gz`` with the same Surya
fragment and tests the compressed image. This catches kernel-wide
compile and link failures that the individual driver checks cannot see.
To reproduce locally, configure the tree as above, install the usual
kernel build dependencies (including ``pahole`` for BTF), then run::

    make -j"$(nproc)" O=out/surya ARCH=arm64 LLVM=1 Image.gz
    gzip -t out/surya/arch/arm64/boot/Image.gz

The job does not package a boot image, publish a flashable artifact, or
assert that the resulting kernel can boot on Surya.

NFC
---

The stock NQ-NCI wiring is staged at I2C address 0x28 on QUPv3 SE2.
The eSE power signal uses GPIO 22, not GPIO 94 (repurposed for charging
on the stock board). The bus and NFC client stay disabled. The stock
I2C pinctrl uses GPIOs 34/35; the 6.1 base uses GPIOs 0/1. The inherited
pinctrl was removed from this disabled bus to avoid treating it as the
Surya wiring. A matching pinctrl variant and a compatible NQ-NCI driver
are required before enabling it.

SDMMagpie pinctrl driver
------------------------

The separate ``qcom,sdmmagpie-pinctrl`` driver ports the stock 4.14 mux
and tile tables to the 6.1 MSM pinctrl core without changing SM6150 or
Violet mappings. It supports the stock summary IRQ only; direct-connect
IRQs and wake routing are not ported. CI compiles the driver, but the
Surya DTS still selects the existing SM6150 controller. Switching to
SDMMagpie requires auditing the stock TLMM MMIO base (0x03400000 versus
the inherited 0x03000000), its IRQ, and every inherited pinctrl consumer
as a group. Changing only the compatible string would be incorrect.

``surya-pinctrl.dtsi`` records the stock TE, touch SPI and NFC I2C pin
states for that switch. The states are intentionally unreferenced. A
source comparison places GPIO 10 in NORTH (``mdp_vsync``), GPIOs 49–52
in WEST (``qup00``) and GPIOs 34–35 in SOUTH (``qup02``) in the ported
SDMMagpie table. CI checks these DTS mappings while also checking that
the active controller compatible has not changed.

The explicit ``vendor/qcom/surya-sm6150-tlmm-audit.dtb`` target encodes
stock MMIO and the summary IRQ for offline comparison. It uses the
6.1 driver's level-high summary configuration for GIC SPI 208 and
omits the inherited direct-connect IRQs. Both TLMM and the conflicting
SE0 debug UART are disabled in this audit DTB. Do not boot or flash it.
The primary Surya DTB and the Violet controller are unchanged. Before
any switch, audit inherited consumers (UART, SD, USB and other GPIO
users) against the SDMMagpie pin table and the running stock FDT.

To inventory consumers before the switch, install ``python3-libfdt`` and
run the following on each compiled or stock DTB, supplying its TLMM
node path::

    /usr/bin/python3 scripts/dtc/surya-tlmm-users.py surya-sm6150-tlmm-audit.dtb /soc/pinctrl@3400000

The report lists standard pinctrl, GPIO and inherited interrupt-parent
references and accounts for disabled ancestors. It is a starting point:
legacy vendor properties and ``interrupts-extended`` need manual review.
CI prints the audit report and checks that it includes the disabled SE0
UART pinctrl reference.

Gates for the first boot test
-----------------------------

Use an exact stock firmware build for the target device. Collect the
original ``boot.img``, ``dtbo.img``, and any separate ``dtb.img`` or
``vendor_boot.img`` provided by that release. Record the firmware build
and Android version. Do not publish device-unique partitions or logs
containing identifiers. If the running stock kernel exposes it, collect
its live FDT (``/sys/firmware/fdt``) and early boot log as well.

1. Confirm the board and DTBO selection, boot image header, kernel
   compression, ramdisk, and DTB placement from the original images.
2. Compare the live FDT's reserved ranges and TLMM resources with the
   6.1 DTS. Validate the regulator and UFS configuration before enabling
   storage, and establish a working early log path before changing UART.
3. Require both the DTB smoke test and the full kernel link to pass.
   Repack a test image using the stock layout and preserve an untouched
   stock image for recovery. Do not use the offline TLMM audit DTB.
4. If the unlocked bootloader supports temporary boot, use that for
   the first test instead of writing partitions. Capture boot logs and
   confirm that storage and boot userspace work. Do not flash until a
   temporary boot has succeeded and a stock recovery path is verified.

Stock dump reference (MIUI V14.0.1.0.SJGMIXM)
--------------------------------------------

The `Android Dumps Surya snapshot <https://gitlab.com/Android-Dumps/poco/surya/-/tree/47617823f73a3288ee5d42c611e0e22a339512ab>`_
identifies Android 12 (SKQ1.211019.001). Its ``bootimg`` directory
lists an extracted kernel and multiple base DTBs; the SDMMagpie base
candidate is ``12_dtbdump_Qualcomm_Technologies,_Inc._SDMMAGPIE_SoC.dtb``.
The decompiled ``dtbodts/16_dtbdump_qcom,sdmmagpie-idp.dts`` has
``qcom,msm-id = <0x16d 0>`` and ``qcom,board-id = <0x22 0>``, matching
the 6.1 stub. Its ``__fixups__`` show that it is an overlay, not a live
FDT. Match it with the base DTB before comparing the effective board
configuration. The prefix ``16`` is a dump filename, not proof of the
bootloader-selected DTBO index.

The extracted components are useful for DT comparison but do not
establish the original boot header, image placement, or AVB metadata.
Use the original firmware images for that part of the boot audit.

Use this snapshot as the offline comparison baseline even when the
phone runs V14.0.2.0.SJGMIXM. In the snapshot the base DTS reports
``qcom,sdmmagpie``, SoC ID ``<0x16d 0>``, and board ID ``<0 0>``; the
IDP overlay supplies board ID ``<0x22 0>``. The base also retains
``__symbols__``, which the overlay's ``__fixups__`` require.

To compare this pair offline, download the base binary named above and
extract the matching binary overlay from the dump. If only the dumped
text overlay is available, recompile it for comparison only::

    dtc -I dts -O dtb -o stock-idp.dtbo '16_dtbdump_qcom,sdmmagpie-idp.dts'
    fdtoverlay -i '12_dtbdump_Qualcomm_Technologies,_Inc._SDMMAGPIE_SoC.dtb' \
        -o stock-surya-merged.dtb stock-idp.dtbo
    fdtget -t x stock-surya-merged.dtb / qcom,board-id
    /usr/bin/python3 scripts/dtc/surya-tlmm-users.py \
        stock-surya-merged.dtb /soc/pinctrl@3400000

The overlay input must have board ID ``22 0``. The merged root may retain
``0 0`` because the overlay header selects a DTBO but is not itself an
overlay fragment. The merged tree is not a runtime FDT:
ABL fixups, selected overlay index, and firmware differences are not
validated by a successful offline merge. Do not use it as a boot image
or substitute it for the phone's V14.0.2.0 stock boot components.

The same offline merge, with base/overlay ID checks and a TLMM and
fixed reserved-memory report, can be run with::

    scripts/dtc/surya-stock-audit.sh BASE.dtb IDP.dtbo out/surya-stock

The second input may also be the decompiled IDP ``.dts`` from the dump.
The tool never fetches firmware or writes device partitions.

The CI stock audit fetches only the two device-tree inputs from the
pinned dump commit. Its job summary reports the merged board ID, fixed
reserved-memory ranges and TLMM consumers. It does not retain the dump
binaries, produce a flashable artifact, or test V14.0.2.0 firmware.

After obtaining both compiled trees, list different fixed reservations
and partially overlapping ranges (even when node names changed)::

    /usr/bin/python3 scripts/dtc/surya-reserved-memory-diff.py \
        out/surya-stock/stock-surya-merged.dtb \
        out/surya/arch/arm64/boot/dts/vendor/qcom/surya-sm6150.dtb

This comparison excludes dynamic reservations and ABL fixups. An overlap
is a review item, not an instruction to change either address.

CI compares the merged stock ranges with the compiled 6.1 DTB and
records the differences in the job summary. The short-lived CI DTB
artifact is only an input to this audit, not a flashable deliverable.
The full Image.gz job also caches Clang compilation results with ccache;
the first cache fill and every final link/BTF step still take time.

Read-only inspection of the target firmware
-------------------------------------------

Use the original ``boot.img`` and ``dtbo.img`` from the Surya Global
V14.0.2.0.SJGMIXM package, not images reconstructed from the V14.0.1.0
dump. With AOSP's ``unpack_bootimg`` and ``mkdtimg`` on PATH, record
hashes and the boot and DTBO layouts without modifying the inputs::

    mkdir -p out/surya-stock-v14.0.2/boot
    sha256sum boot.img dtbo.img > out/surya-stock-v14.0.2/sha256.txt
    unpack_bootimg --boot_img boot.img --out out/surya-stock-v14.0.2/boot \
        --format mkbootimg > out/surya-stock-v14.0.2/boot-layout.txt
    mkdtimg dump dtbo.img > out/surya-stock-v14.0.2/dtbo-layout.txt
    file out/surya-stock-v14.0.2/boot/*

Check the header version and ramdisk, DTB presence and placement, and
kernel compression separately. DTBO table order alone does not prove
which entry ABL loaded. If available, compare the live FDT from the
same firmware. Do not replace the stock kernel payload based only on
these reports; reserved memory, TLMM, and UFS remain bring-up gates.
