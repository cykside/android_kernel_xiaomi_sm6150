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
4.14; the 6.1 base permits 1.232–1.260 V), and verify dedicated-pad reset
and the UFS link on hardware. The DTB smoke test
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

The first compiled dump comparison (Actions run ``45774702701``) found
10 partially overlapping pairs. They group into three review items:

* Stock splash spans ``[0x9c000000, 0x9d700000)``; it overlaps both the
  6.1 splash and display ramdump reservations. Do not enable splash
  takeover until these ranges and their consumers are reconciled.
* The 6.1 ADSP/CDSP firmware and WLAN MSA layout differs as a group.
  Stock WLAN MSA and NPU occupy the end of the 6.1 ADSP range, while
  the 6.1 WLAN MSA occupies the start of the stock CDSP range.
* The stock XBL/AOP range encloses the 6.1 command DB range; do not
  widen the 6.1 XBL/AOP reservation without checking ABL and AOP data.

The audit now also displays the UFS host/PHY register, interrupt, lane,
boot-device and reset properties from both compiled trees. Raw GPIO
phandles are local to each DTB and must not be compared as pin numbers.
No memory ranges or UFS enablement are changed by these checks.

UFS reset is a concrete incompatibility: stock SDMMagpie uses
``dev-reset-assert`` and ``dev-reset-deassert`` pinctrl states on the
dedicated ``ufs_reset`` pad. The pad is group 126 in the SDMMagpie
table but is not exposed as a GPIO by that driver. The inherited
6.1 SM6150 ``reset-gpios`` cell 123 corresponds to an SD clock pad in
the stock table. Surya now drops that GPIO reference. Do not substitute
GPIO 126: the 6.1 UFS host currently uses a GPIO descriptor for device
reset; a verified dedicated-pad reset path is still needed. The UFS
host and PHY remain disabled.

The 6.1 Qualcomm UFS driver now accepts the stock pair of named reset
pinctrl states when no reset GPIO is present. The existing GPIO path
has priority and is unchanged for boards that use it. The state pair
is wired only in the offline TLMM audit DTB, where UFS and TLMM both
stay disabled. It is not wired into the primary Surya DTB until the
SDMMagpie controller switch is validated.

UFS boot selection and PLL supply
---------------------------------

Neither the pinned 4.14 sources nor the compiled MIUI 14.0.1 host
node declare the ``boot_conf`` NVMEM reference. The SM6150 base points
it at four bytes at QFPROM offset ``0x6070`` (base ``0x00780000``) and
expects value ``4`` in bits 5:1 of the first byte. A readable mismatch
skips host initialization. Surya drops that inherited gate so the
staged host does not depend on an unverified fuse mapping. The driver
uses its default when the cell is absent. The QFPROM provider and other
consumers are unchanged; this does not validate probing on hardware.

Stock SDMMagpie L3C allows 1.144–1.256 V and initializes at 1.144 V;
SM6150 6.1 inherits 1.232–1.260 V with a 1.232 V initial setting.
The common 6.1 UFS PHY votes 1.2–1.8 V for ``vdda-pll``, so its
request intersects both ranges. This does not establish that the
inherited voltage is suitable for Surya. Stock uses L3C for both the
PHY PLL and the host reference clock, so check both consumers and the
target DTB constraints before overriding the rail. The regulator
constraints are not changed.

The compiled MIUI 14.0.1 audit resolved the same supply roles, but
confirmed differing regulator constraints (stock vs. Surya 6.1):
VCC L19 2.944–3.304 V vs. 2.700–3.008 V; VCCQ2 L12
1.696–1.952 V vs. 1.800–1.950 V; PHY L4 0.824–0.920 V vs.
0.875–0.975 V. L3C matches the source comparison above. These results are for the offline dump, not the target
firmware's live tree. UFS remains disabled.

Active Android 12 boot image sample
-----------------------------------

Read-only header samples supplied from a running Surya with
``4.14.357-FSociety/8d46546a`` and KernelSU show Android boot header v2
(``1660`` bytes), 4096-byte pages, and gzip magic at kernel offset
``0x1000``. The header declares a kernel payload of ``0xd2bdf8`` bytes,
a ramdisk of ``0xc1915`` bytes at ``0xd2d000``, no second stage or
embedded recovery DTBO, and a separate DTB section of ``0x5475c``
bytes at calculated file offset ``0xdef000``. The DTB load address
in the header is ``0x01f00000``, not a file offset. A read at ``0xdef000`` confirmed FDT magic ``0xd00dfeed`` and
``totalsize = 0x5475c``, which fills the declared DTB section with one
FDT. DTBO selection remains unverified. The gzip prefix alone
cannot rule out additional data after the compressed stream.

This sample is from a working custom kernel, not a pristine MIUI boot
image. Its header is a repack reference, not proof of Android 12 vendor
compatibility with 6.1 or of KernelSU support in the new kernel.

The active ``dtbo`` partition starts with the Android DT table magic
``0xd7b7ab1e``. The table is version 0 and declares total size
``0x3f0ba``, 4096-byte pages, a 32-byte header and one 32-byte entry
at offset ``0x20``. This establishes that the partition contains one
entry, not that the bootloader applies it to the base FDT.
The entry declares payload size ``0x3f07a`` at offset ``0x40``; it
ends exactly at the declared table size. The entry ID, revision and
four custom words are zero. Those metadata fields do not establish
which board modifications the payload makes or whether it was applied.
The payload at ``0x40`` has FDT magic ``0xd00dfeed`` and an internal
size of ``0x3f07a``, matching the table entry. This checks the FDT
header only; structural validation and overlay application against the
6.1 base still require the payload itself.

The running kernel exposes model ``IDP`` and root compatibles
``qcom,sdmmagpie-idp``, ``qcom,sdmmagpie``, ``qcom,idp`` in that order.
Live ``qcom,msm-id`` and ``qcom,board-id`` decode as ``<365 0>`` and
``<34 0>`` respectively, matching the numeric IDs in the staged Surya
DTS. The compatible strings differ. This confirms live board identity,
not how the live tree was assembled or that the active overlay applies
to the 6.1 base.

Live reserved-memory sample
~~~~~~~~~~~~~~~~~~~~~~~~~~~

The FSociety device tree reports two address cells and two size cells.
The ranges below were read from ``reg`` on the running device; none of
these nodes had an explicit ``status``. Ends are exclusive.

====================  ============  ============
Reservation           Start         End
====================  ============  ============
XBL/AOP               ``85e00000``  ``85fff000``
CDSP                  ``93900000``  ``95700000``
ADSP                  ``95700000``  ``97500000``
WLAN MSA              ``97500000``  ``97680000``
NPU                   ``97680000``  ``97700000``
Continuous splash     ``9c000000``  ``9d700000``
DFPS                  ``9d700000``  ``9d800000``
Ramoops               ``9d800000``  ``9dc00000``
====================  ============  ============

The first seven ranges match the audited stock layout. Ramoops also
matches the staged Surya 6.1 address and size. The live XBL/AOP range
covers the port's command DB subrange, but that does not establish
whether firmware uses that subrange for command DB or exposes it through
a separate device-tree node. Keep the port layout unchanged until the
firmware consumers and the command DB mapping are reconciled.

The pinned SDMMagpie 4.14 source declares ``qcom,cmd-db@c3f000c``
with an eight-byte MMIO resource. Its driver reads the database
address and size from those registers. In contrast, the 6.1 driver
looks up a fixed reserved-memory node and validates the database
magic there. The 4.14 MMIO address is not the database address; do
not substitute it as the 6.1 reserved-memory base. Verify the live
node and look for existing kernel diagnostics before choosing a
mapping. Do not read raw physical memory from userspace.

The live FSociety tree confirms ``/soc/qcom,cmd-db@c3f000c`` with
``reg = <0x0c3f000c 8>``. The filtered kernel log returned no lines.
This confirms the discovery resource on the active device, but does
not identify the database's actual RAM address. A 6.1 discovery path
should validate that the firmware-reported address and length are
fully contained in reserved memory before mapping the database.

The offline audit DTB now models this path using
``qcom,sdmmagpie-cmd-db`` with an eight-byte ``reg`` and a
``memory-region`` phandle to the live-sized, no-map XBL/AOP reservation.
The driver checks containment before mapping, then checks magic and
entry bounds. The discovery node is disabled in the audit DTB, and
the primary Surya DTB retains its fixed mapping pending validation.

Actions run ``45792719883`` compared both compiled DTBs with the same
pinned stock dump. The primary DTB had 14 partial-overlap pairs; the
offline DTB had two. These are cross-tree comparisons, not a count of
internal overlaps or proof that firmware consumers are safe. Inspect
both remaining pairs before promoting the offline changes.

Actions run ``45794221831`` identified both remaining offline pairs:
stock ``disp_rdump_region@9c000000`` covers ``[0x9c000000, 0x9d800000)``
and overlaps the offline splash ``[0x9c000000, 0x9d700000)`` and DFPS
``[0x9d700000, 0x9d800000)``. The two offline ranges are adjacent and
exactly partition the stock ramdump span. There is no internal overlap
between these two offline nodes. The running FSociety sample exposed
splash and DFPS at those addresses without a separate display ramdump
node, so the offline variant follows the live layout. This does not
validate display takeover or justify dropping any firmware-owned
memory from the primary DTB.

``surya-sm6150-cmd-db-test.dts`` is a CI integration fixture that
selects the discovery node and SDMMagpie TLMM while inheriting the
audited reservations.
It is not exported as a flash or boot artifact: UFS and display
remain unavailable for normal boot. Its compile-time checks do not exercise MMIO or
validate the database contents on hardware.

CI run ``45797381714`` identified two enabled inherited TLMM clients
in the offline tree: the GENI UARTs at ``0xa80000`` and ``0xa8c000``.
Both are now disabled in the offline tree and integration fixture.
CI rejects any enabled consumer found by the inventory in the fixture.
This is a static check, not validation of TLMM hardware or untracked
consumer references. The main Surya DTB is unchanged.

The integration fixture also stages the four stock UFS rail ranges and
initial voltages in ``surya-ufs-audit.dtsi``. CI resolves the host and
PHY supply phandles before checking those initial voltages. The rails
can have other consumers; do not promote these settings to the main
DTB without validating the shared votes. The host and PHY remain off,
and this fixture is not a boot or flash candidate.

The legacy RPMh regulator DTS declares ``proxy-supply`` references to
L4 and L3C, independently of whether UFS probes. The UFS host's separate
reference-clock supply parser reads its DT current limit and obtains a
regulator handle; it does not set a fixed 1.2 V in that parser. Review
available proxy and other consumers before changing the provider's
initial voltage. An available DT node is not proof of an active vote.

CI run ``45802353483`` found five available references in the
integration fixture: two RPMh proxy references and three USB PHY
``vdd-supply`` references to L4. The USB controllers were disabled but
their PHYs were still independently available. The offline UFS fixture
now disables those PHYs too and checks for non-proxy references to its
UFS supplies. Proxy behavior still needs validation. The primary Surya
DTB retains its existing USB and regulator configuration.

``surya-sm6150-ufs-test.dts`` additionally enables the UFS host and PHY
for a compile-only integration check. CI checks the host-to-PHY link,
the PHY reset provider, the named device reset states, and the enabled
supply consumers. This fixture is not packaged or published as a boot
artifact. The primary DTB still disables UFS; hardware behavior,
active DTBO compatibility and recovery are not verified by this check.

CI attempts an offline merge of the pinned MIUI 14.0.1 overlay with
that UFS integration DTB and reports missing external fixup symbols.
An applied overlay is not a compatibility pass: it can still override
hardware properties, and the overlay from the active device has not
been tested. The merged output is not packaged or published.

With labels exported, Actions run ``45807383323`` still rejected the
pinned overlay: 77 of its 159 external symbols were absent. Audit
reports pair missing labels with their stock base node paths, so each
can be classified before considering any port. Do not add placeholder
aliases merely to make the merge succeed: a successful merge can
silently change pin, regulator or reserved-memory properties.

The annotations on run ``45808447785`` show that the missing references
span audio (APR, SLIMbus, SoundWire, codec and MI2S), camera (CCI, CAMCC
and sensor pin states), display (DSI PLL, secondary PHY and DP), PMIC
and thermal nodes, and peripheral pin states (SDC, UART, SPI and
charger). This is not a UFS-only overlay. Do not carry it over wholesale
or create dummy labels for these subsystems. Keep the UFS integration
fixture as a self-contained tree; before any repack, establish what ABL
will do with the active DTBO rather than assume that supplying a base
DTB suppresses overlay application.

A local image can be checked without uploading it using
``python3 scripts/dtc/surya-boot-v2-layout.py surya-boot.img``. The
checker verifies the observed v2 envelope, gzip prefix, and single FDT
section, but does not check gzip stream contents, AVB, or whether the
bootloader will apply the active overlay. It never repacks the image.

The same non-destructive overlay test is available locally with
``sh scripts/dtc/surya-overlay-check.sh BASE.dtb surya-active.dtbo``.
It needs ``fdtget`` and ``fdtoverlay``. The base for this test is the
symbol-enabled Surya UFS integration DTB, not the primary DTB. The
script reports missing labels and deletes its temporary merge. It
never writes a boot image or a partition.

The device owner ran the same checker locally against the active
``surya-active.dtbo``. It reported 77 missing labels out of 158 and
``FDT_ERR_BADOVERLAY``. The missing-label list matches the stock
baseline, but the total differs, so the two overlays must not be
treated as identical. Do not repack the 6.1 fixture expecting the
active overlay to merge. A useful control is to try that exact overlay
against the single FDT extracted from the working boot image; even a
successful control does not prove what ABL applies at boot.

The working FSociety boot image's extracted FDT provided all 158
external symbols for the active DTBO, but the local control still
returned ``FDT_ERR_BADOVERLAY``. Missing symbols therefore do not
explain the control failure. Inspect the active overlay's fragment
structure and fixups before drawing conclusions about ABL. The
working boot image alone does not prove that ABL applies this payload
with the same rules as libfdt, or that it applies it at all.

On-device inspection found all 158 base symbol paths readable, but
18 external fixup references could not resolve a node or property in
the overlay itself. All point to ``/fragment@24/__overlay__/qcom,dsi-display``
and cover reset/TE GPIOs, DSI PHY/controller, clocks, pinctrl, and
supplies. This is consistent with libfdt rejecting an overlay fixup
before applying fragments. Inspect the actual fragment children before
classifying these references as stale; do not repair them by aliasing
nodes in the 6.1 base or by modifying the active DTBO partition.

A direct DTS decompile disambiguated the display node names. The
unsuffixed ``qcom,dsi-display`` node contains only
``qcom,dsi-display-list``; ``qcom,dsi-display@0`` contains only
``qcom,dsi-panel``. The 18 fixups reference properties absent even
from the exact unsuffixed node. Prefer-exact lookup cannot fix them.
Any experiment that removes stale fixups should be limited to a local
copy and must not be treated as a deployable DTBO. The separate 6.1
missing-symbol issue and the active ABL overlay policy remain open.

FSociety source reference
~~~~~~~~~~~~~~~~~~~~~~~~

The supplied ``cykside/msm-4.14`` repository contains the running
kernel's short commit ID ``8d46546a`` (full commit
``8d46546a6674c65f317330b807dd0023b7d6305a``). The pinned port
reference ``290a8a5`` is three commits ahead with no DTS changes in
that comparison. At ``8d46546a``, ``surya_defconfig`` sets local
version ``-FSociety`` and enables DT overlay builds, while appended
DTB images are disabled. This corroborates the separate DTB section
observed in the active boot image. It does not establish which DTBO
was packaged or how the bootloader handles its stale fixups.

Use the pinned FSociety source as the primary 4.14 reference for the
Surya hardware port, checked against the live device where possible.
The MIUI dump remains an independent comparison, not the target ROM.
Do not import 4.14 driver ABIs, root features or flash scripts wholesale.

At ``8d46546a``, the Qualcomm DTS Makefile builds
``sdmmagpie-idp-overlay.dtbo`` and ``sdmmagpie-idp-overlay-miui.dtbo``
against ``sdmmagpie.dtb``. Both declare board ID ``<34 0>``. The MIUI
DTS additionally enables ``qupv3_se11_spi`` with a
``qcom,spi-msm-codec-slave`` child and a disabled IR LED child. The old
``build.sh`` stages ``Image.gz``, ``dtb.img``, ``dtbo.img`` and
``dtbo-miui.img`` into its AnyKernel3 packaging directory. This shows
that the build produces two overlay variants; it does not identify
which was installed or make either applicable to the 6.1 base.

The pinned FSociety ``sdmmagpie-sde-display.dtsi`` defines the
unsuffixed ``sde_dsi`` node with the exact GPIO, controller, PHY,
clock, pinctrl and supply properties missing from the active overlay.
Both overlay variants include this source through ``sdmmagpie-idp.dtsi``.
This is a source-to-artifact difference, not evidence that those
properties were absent from the pinned DTS. The 4.14 boot Makefile
uses separate table-packing targets (``dtbo.img`` passes an explicit
4096-byte page size, ``dtbo-miui.img`` uses the packer's default).
Neither rule identifies which payload is installed on the device.

CI now compiles the pinned FSociety base and both overlay source
variants with host ``dtc`` and tests them against that base without
building a 4.14 kernel. This is a source control experiment, not a
bit-for-bit recreation of the installed artifacts. No resulting DTB
or DTBO is published or written to a partition.

In run ``45815561402``, both rebuilt FSociety variants compiled and
both were rejected by the host overlay tool even though all 158
external symbols were present in the same-revision base. This
reproduces the rejection at the outcome level; it does not establish
that the rebuilt and installed overlays fail for the same reason.
The reference job reports the libfdt error and checks the decompiled
exact display node without relying on unit-address path matching.

Run ``45817414555`` isolated a host tool lookup issue in the rebuilt
source overlays: both fail with the unmodified control and both apply
with the same-version exact-name-first diagnostic. The active DTBO
still lacks the properties referenced by its 18 stale fixups; this
experiment does not fix that artifact or make the 6.1 base compatible.
Use the diagnostic merged source trees to cross-check register and
reservation data, not as boot images or substitutes for the live tree.

Both diagnostic merges in run ``45817917119`` reproduced the live
XBL/AOP reservation and the eight-byte Command DB discovery resource.
CI now checks those two values against the live observation when the
source merge succeeds and reports UFS register windows separately.
This corroborates the staged Command DB configuration, not the
firmware-reported address read at runtime.

Both FSociety source-derived DTBs in run ``45818373904`` locate the
UFS host and ICE at ``0x1d84000/0x3000`` and ``0x1d90000/0x8000``,
matching the 6.1 fixture. The PHY base is ``0x1d87000`` in both, but
FSociety describes ``0xddc`` bytes while 6.1 describes ``0xe00``.
The 6.1 mapping is deliberately not shrunk to the 4.14 size: the
6.1 driver maps the resource it is given and its register accesses
must determine the required span. Static agreement is not a link test.

To calculate replacement offsets without generating an image, add
``--plan Image.gz surya-sm6150-ufs-test.dtb`` to the boot layout checker.
The dry run retains the reference ramdisk size and DTB load address,
validates the replacement gzip prefix and single-FDT size, and reports
the padded payload end. It excludes AVB/footer data and does not
establish boot-partition capacity, bootability or overlay compatibility.

The test device's boot block device reports ``134217728`` bytes
(128 MiB) via ``blockdev --getsize64``. CI compares the aligned v2
payload with this measured capacity. The remaining space is reported
before AVB; it is not a validated final image size or boot permission.

Run ``45822625503`` measured the aligned 6.1 payload at ``17084416``
bytes, leaving ``117133312`` bytes before AVB against the measured
capacity. The read-only boot tool also accepts ``--avb`` to inspect an
existing image's terminal footer and its vbmeta location. A missing
footer does not rule out external vbmeta verification, and this check
does not verify signatures or change verified boot settings.
