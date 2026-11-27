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

The active boot partition has a terminal ``AVBf`` footer at
``0x07ffffc0``. It reports version 1.0, original image size
``0xe44000``, vbmeta offset ``0xe44000``, and vbmeta size ``0x380``.
The original size equals the page-aligned end of the observed v2 DTB
section. This establishes footer presence, not signature validity or
bootloader acceptance. Do not copy the footer or vbmeta unchanged to
an image with a replacement payload.

``surya-boot-v2-repack-offline.py`` creates an unsigned local fixture
from a reference image, replacement gzip kernel and one DTB. It
preserves the reference ramdisk, recalculates v2 sizes and the legacy
boot image ID, then checks the resulting section offsets. It neither
copies nor generates AVB metadata, and refuses an existing output.
Do not boot, flash, or distribute this fixture as an installer.

To investigate whether the working boot flow supplies overlay content,
compare markers from the pinned FSociety overlay (``/soc/onewire_gpio``
and ``/soc/maxim_ds28e16``) between the extracted boot DTB and the live
OF tree. A marker present only in the live tree demonstrates a
post-base addition, not necessarily application of the entire active
DTBO. A marker already present in the base is not discriminatory.
Do not change the DTBO partition to run this comparison.

On the test device, both markers were absent from the extracted
FSociety boot DTB and present in ``/proc/device-tree``. The live tree
therefore includes content absent from that base dump, consistent
with the FSociety overlay. This does not establish which boot stage
added the nodes, which payload supplied them, or whether broken fixups
were ignored. Treat unchanged-DTBO boot of a replacement 6.1 base
as unvalidated; a successful offline repack is not a flash gate.

The offline ``surya-sm6150-overlay-test.dts`` enables the UFS host
and PHY on its matching ``surya-sm6150-v2-test.dts`` base. The base
contains the 6.1 wiring and supplies, but leaves both nodes disabled
until the overlay is applied. Only the ``soc`` external symbol is
required, following Violet's local convention. The fast DTB job tests
this transition and checks the retained PHY and reset-state references.
The pair is not a drop-in replacement for the active DTBO: it does not
validate ABL selection, runtime access to UFS, or other board features.

Fast iteration CI
~~~~~~~~~~~~~~~~~

Ordinary PR updates run the Surya DTB and staged driver smoke tests,
including UFS PHY compilation. The expensive jobs are opt-in:
``surya-image-ci`` builds the full image and offline bundle,
``surya-stock-ci`` runs the stock dump audit, and ``surya-fsociety-ci``
rebuilds the FSociety reference. ``surya-full-ci`` enables all three.
These are PR labels; adding a label triggers a new run and the label
remains effective for future pushes until removed. Run full CI before
packaging a device test. A skipped job is not evidence that it passed.

First device test
~~~~~~~~~~~~~~~~~

The owner reported that the experimental TWRP installer completed,
but the phone rebooted at the Xiaomi logo without reaching Android.
Restoring the pre-test Boot and DTBO pair in TWRP returned the phone
to a working FSociety system. After recovery, pstore had no entries
and reading the tail of ``/proc/last_kmsg`` produced no output.
The failure stage is unknown: this is not proof of an ABL rejection,
a kernel panic, or a UFS failure. Do not repeat the same flash solely
to recover logs. Before another device test, review the actual
recovery repack path and establish a diagnostic capture method.

The no-write TWRP report from the test device confirmed an unlocked,
orange boot state, unchanged ramdisk bytes, and a byte-identical
replacement DTB. The repacked image retained the reference
``earlycon=msm_geni_serial,0x880000`` even though the integration tree
disables the corresponding SE0 UART. This is a diagnostic mismatch,
not proof of the cause of the reboot. The next no-write diagnostic
replaces that single argument with the built-in ramoops early console.
The address ``0x9da00000`` is the start of pmsg (1 MiB of crash records
plus 1 MiB of console after ``0x9d800000``); the early mapping is limited
to 256 KiB. Persistence through an actual reset is not yet verified.

Violet commit ``1c82ff81f`` keeps the early ramoops console and
``reboot=warm panic=5`` while removing unrelated debug flags. The
Surya diagnostic likewise requests warm reboot and a five-second panic
reboot, without inheriting Violet's address or driver policy flags.
Before a write-enabled logging trial, verify that the recovery or
restored kernel can read the same pmsg zone (address, geometry and
ECC). A warm reboot request cannot ensure DDR preservation across
watchdog, firmware resets or power loss.

The first captured 6.1 console log is partially corrupted, but the
panic trace identifies an asynchronous SError around 0.34 seconds. It
shows ``readl`` from ``msm_gpio_get_direction`` during GPIO chip
registration under ``sdmmagpie_pinctrl_probe``. Since SError is
asynchronous, the exact offending pad or transaction is not proven.
The follow-up skips only the registration-time GPIO direction sweep
on SDMMagpie. Explicit direction queries still use the driver callback.
This is not yet verified on hardware and does not establish UFS status.

The recovered FSociety boot registered ramoops at 0x9d800000 (4 MiB,
16-byte ECC). Its boot reason was recovery and its PMIC reported a hard
reset/cold boot; those observations do not identify the failed 6.1 reset.
On the 6.1 PSCI restart path, reboot=warm selects SYSTEM_RESET2 only if
firmware reported support. Otherwise Linux requests SYSTEM_RESET. A
watchdog/firmware reset can bypass that path entirely. Log the probed
SYSTEM_RESET2 capability at notice level in the next trial; a positive
result is not proof that RAM survived the specific failed boot.

The next captured console log reached an interconnect QoS register
read in deferred probe and ended in a synchronous external abort
(about 0.66 seconds). The exact provider is not established by the
corrupted trace. Apply the existing Violet-style skip-qos workaround
only to the Surya v2 test DTB's six physical NoC providers and honor
it on SM6150 restore too. This leaves the interconnect topology and
RPMh bandwidth votes in place; it is not yet a validated QoS port.
The same captured log reports that firmware does not support the PSCI
SYSTEM_RESET2 warm-reset call, but this particular panic log survived.

With QoS programming skipped, the device reached Android first-stage
init. It timed out waiting for metadata, super and vbmeta_system, and
panicked after init exited. The captured console also reports TBU
probe failures and missing RPMh resources. Neither these warnings nor
the absence of UFS messages proves why storage was not discovered.
As a controlled next trial, merge the existing UFS enable overlay into
the DTB embedded by the flashable package, so UFS status does not depend
on ABL applying the external DTBO. Keep the matching DTBO alongside it.

The FSociety SDMMagpie UFS node and the matching MIUI overlay do not
specify an IOMMU, whereas the 6.1 trial inherits an apps_smmu stream
(0x300) and fastmap from sm6150.dtsi. The captured trial reports failed
QSMMUV500 TBU probes before the Android storage timeout. Match the
FSociety DT dependency graph for this one host by dropping the inherited
IOMMU and fastmap properties from the Surya v2 trial. Other SMMU users,
DMA coherency and UFS clock/power settings are unchanged. Whether the
native firmware allows this DMA path still needs hardware validation.

The next device snapshot confirms both UFS DT nodes are enabled and
identifies the host's pending supplier as 1700000.interconnect
(aggre1_noc), not the inherited UFS IOMMU. Its three provider bulk
clocks are only used by static QoS setup, which this trial already
skips. Drop that unused DT clock dependency on the Surya v2 provider;
leave the host's own clocks, both interconnect paths and RPMh votes in
place. The snapshot does not prove which aggre1_noc dependency failed;
record BCM voter lookup failures with the deferred-probe reason too.

The owner reports that trial ``12c954015`` still did not boot and did
not leave a recoverable log. This cannot establish whether removing
the unused QoS clocks changed the provider's probe outcome. The last
observable UFS blocker remains the earlier snapshot's dependency on
``1700000.interconnect``; the PHY was unbound in that snapshot too.
Do not repeat the same trial solely to recover a log.

The FSociety SDMMagpie PHY does not request reset from the UFS host.
The 6.1 inherited PHY reset phandle points back to the host, which
itself consumes the PHY. Remove this reverse dependency in the Surya
trial to match the reference. Keep the host's GCC reset and dedicated
UFS device-reset pin states. The QMP v3 driver already allows the PHY
reset to be absent; this does not prove why the PHY was unbound.

Trial ``ff9621fa8`` also did not boot and left no readable panic log.
It does not establish whether the PHY reset change affected probe.
The installer assembly now explicitly checks both trial dependency
removals and the presence of the host reset, reset pin states and
bandwidth path names in the merged DTB. These are packaging checks,
not hardware validation. Violet's PS_HOLD option is implemented by
qcom-pon; adding the property to a vendor qpnp-power-on node would
not use that implementation. Do not change Surya PON ownership or
restart-reason programming based on the Violet DT alone.

FSociety's SDMMagpie bus DTS at ``8d46546a`` declares CE0, QUP0,
SN14 and IP0 from the inherited 6.1 aggre1_noc BCM list, but not CN1
or SN3. It declares neither SN3 nor SN5 from the failing System NoC
lookups. Its UFS CPU/config endpoint uses CN0 and the DDR endpoint
uses MC0 and ACV. These are source declarations, not a dump of the
firmware Command DB. This discrepancy does not explain provider
registration by itself: 6.1's RPMh provider probe does not return the
individual BCM lookup error. Do not globally delete or rename BCMs
based on this comparison.

The FSociety bus DT maps ``aggre1_noc-base`` to ``0x16e0000/0x11080``
and ``aggre2_noc-base`` to ``0x1700000/0x1f080``. The inherited SM6150
aggre1 node used the latter base address. Match the SDMMagpie aggre1
resource in the Surya v2 DTB only, preserving its phandle consumers,
RPMh votes and skip-qos. This corrects a reference mismatch; mapping the
old region alone has not been established as the cause of deferred probe.

FSociety's gcc-sdmmagpie driver and the inherited gcc-sm6150 driver
use different offsets for aggre UFS AXI, UFS AXI, ICE, UniPro and
PHY AUX branches (including their HW_CTL variants), and the AUX
RCG. The Surya v2 GCC compatible selects the SDMMagpie offsets for
these clocks only. Clock IDs, GCC base, UFS MEM CLKREF and the UFS
BCR remain unchanged. The rest of the GCC controller is not audited
by this comparison. This does not diagnose the earlier NoC defer.

Violet config comparison: QCOM_CLK_RPMH is the CXO clock provider,
separate from QCOM_RPMH transport; QCOM_GDSC_REGULATOR provides vdd-hba.
Both now build in for Surya. The legacy REGULATOR_RPMH driver matches
Surya's rpmh-vrm-regulator and rpmh-arc-regulator nodes. It calls the
proxy-consumer helper, and the inherited PM6150 L4 UFS PHY rail declares
an enable/current proxy vote. Enable REGULATOR_PROXY_CONSUMER as in
Violet so those DT votes are honored without changing their values.
Do not add Violet's separate mainline RPMh regulator or power-domain
options solely because they appear in its fragment; verify the live
consumer and provider binding first.

FSociety's qcom,rpmh-clk-sdmmagpie match uses its SM6150 descriptor.
The shared CXO, LN_BB2/3 and RF1/2 entries map to the same firmware
resource names as the 6.1 SM6150 clock descriptor. The latter also
looks up IP0 eagerly. IP0 is declared by the legacy bus DT, but only
firmware lookup can confirm its presence. Do not mark it optional
or override the clock provider without evidence of a failed lookup.

Violet also builds QCOM_PDC in. The inherited Surya SPMI PMIC arbiter
routes its mandatory periph_irq through the qcom,pdc interrupt domain.
Build that provider in on Surya rather than relying on the SPMI config
alone. This supplies the declared interrupt path; it is not evidence
that the earlier NoC defer was caused by the PDC.

The PMIC interrupt routes agree at the GIC: FSociety uses SPI 481
directly, while the trial routes PDC pin 1 through the range
``<0 480 94>``, which resolves to the same SPI. The 6.1 PMIC arbiter
returns an error if it cannot acquire the named periph_irq. This is
a required supplier, not merely a suspend/wakeup optimization.

Hardware result for the combined supplier trial
-----------------------------------------------

The tester reported that ``4e96dd2bf`` did not boot, despite successful
build, DT validation and offline boot-v2 repack in run ``45897126700``.
No matching kernel log was supplied. This result does not establish that
UFS still defers on the interconnect or that its probe reaches the PHY.
Keep the last logged failure separate from this unobserved boot path.
Do not distribute further test ZIPs based on offline validation alone.

Raghav's Violet changes and Surya applicability
-----------------------------------------------

* ``7536e8f9c``: the PMIC arbiter stale-IRQ cleanup is already present
  in the shared SPMI driver, including the valid APID and ownership
  checks. Do not duplicate it or broaden the register walk. The failure
  it addressed was observed after kexec from 4.14 on Violet.
* ``9c2015eb2``: retain the Surya NoC skip-qos workaround with RPMh
  bandwidth votes intact. Violet's apps-only voter changes addressed
  a display RSC that read zero after kexec; its SDHCI runtime-PM change
  does not address UFS.
* ``10ad815b6``: adopt the handoff investigation method, not the
  display stream ID or context bank. Violet's stream 0x800 and context
  bank 2 were observed on Violet. Reusing a bootloader mapping on Surya
  requires proof of the active mapping; this is not a UFS supplier fix.
* ``d2552efbb`` removes six Violet-only TBU disable overrides; it does
  not add a UFS IOMMU mapping. Surya already inherits those enabled
  TBUs. The common UFS SID 0x300 is within ANOC1's 0x000--0x3ff
  range, but this range only describes the TBU; it does not establish
  that UFS requires translation. The Surya trial removes only the UFS
  consumer's iommus property to match the FSociety reference. This
  leaves the TBUs available to other clients and does not justify
  globally disabling or bypassing the SMMU.

HLOS voter registration before UFS
----------------------------------

Surya and Violet inherit the apps_rsc cluster power-domain dependency.
In rpmh_rsc_probe(), a failure attaching that domain or registering its
notifier returns before the DRV children are populated. Consequently,
matching the hlos voter phandle is necessary but not enough: a missing
PSCI cluster-genpd provider can block both the voter and the RPMh clock
controller. ARM_PSCI_CPUIDLE_DOMAIN defaults to y when ARM_PSCI_CPUIDLE
and PM_GENERIC_DOMAINS_OF resolve to y. Verify the resolved symbols and
provider registration rather than dropping the power-domain reference:
that would also change RPMh sleep/wake handling. No corresponding
attach failure has been established for the last hardware trial.

The packaged v2 trial enables the SDMMagpie discovery node; the default
Surya DT does not. If discovery fails before publishing cmd_db_header,
cmd_db_ready() returns -EPROBE_DEFER to RPMh. This happens before DRV
initialization and is distinct from failure attaching cluster-pd or
looking up an individual BCM. Keep the Command DB deferral reason in
the device core so those paths can be distinguished. This does not
change the reserved window, permit out-of-range mappings, or fix a
firmware rejection by itself.

The legacy FSociety source does not attach a BCM to either
slv-qns-cnoc (the SNOC-to-CNOC bridge) or slv-srvc-aggre2-noc.
The 6.1 table puts both nodes in SN3 and references that shared
BCM from both aggre1_noc and system_noc. Removing it from only
one descriptor would neither remove the bridge membership nor
prevent the other provider from attempting initialization. Both
trees associate the GEM-to-SNOC master with SN15 and the UFS
configuration endpoint with CN0. This comparison concerns Linux
vote descriptions, not the contents of the firmware Command DB.

A breadth-first traversal of the SM6150 C node/link tables confirms
that ufs-ddr needs Aggre1, System NoC, GEM NoC and MC Virtual, while
cpu-ufs needs GEM NoC, System NoC and Config NoC. Removing any one
of the listed providers from its corresponding graph makes the route
unreachable. This is a static topology check only: it does not show
that any of those providers registered or committed a vote on hardware.

GEM NoC and MC Virtual also request the display BCM voter before
registering, even when the consumer is UFS and its path uses HLOS
nodes. Disabling Surya display consumers does not by itself eliminate
that provider-level dependency. Violet's remap therefore addresses a
structurally relevant dependency, but its evidence was a failed
Display RSC probe after kexec. Confirm the Display RSC failure on
Surya before redirecting display votes onto the APPS RSC.

Hardware result: e9238fb89
-------------------------

The TWRP boot-test artifact from run 45984809577 reached the stock logo
and repeatedly rebooted, according to the tester. The log collector
returned no records. Offline boot-v2 repack and DT validation passed,
but this observation does not distinguish an ABL failure from an early
kernel failure or loss of the ramoops record at reset. Do not attribute
this run to the earlier Aggre1 probe deferral without new evidence.

The boot-test installer script and the workflow commands that assemble
its Image.gz, embed the UFS overlay and stage dtbo.img did not change
between da96d95cb and e9238fb89. That checks source-level packaging
continuity, not the bytes on the tested device. The test DTB and kernel
configuration did change: Aggre1 resources, ramoops no-map, GCC
selection, built-in power/IRQ suppliers and Display RSC voter parent
all differ. The latest QMP v3 reset phandle matches the older DTB
again, so it is not a newly introduced DT dependency relative to that
logged baseline.

The first reported unlogged boot failure, 12c954015, immediately
followed da96d95cb. Its functional DT delta removed the Aggre1 bulk
clock references; the other driver delta only changed error reporting
for the BCM voter lookup. There was no installer delta. Removing
these references can let Aggre1 reach BCM initialization earlier even
with skip-qos: QUP0 is keepalive and can send an RPMh request before
icc_provider_register(). This identifies an altered execution path,
not evidence that the QUP0 request failed on the device. The Aggre1
base relocation, GCC selector and Display voter fix all came later.

Violet pstore config parity and Surya reader compatibility
---------------------------------------------------------

The Surya and Violet fragments request the same built-in pstore,
ramoops, console, pmsg and ramoops early console. Surya now also
explicitly disables the two Violet panic reset overrides (Qualcomm
download mode and forced panic watchdog bite). Keep the Surya boot
arguments installed by AnyKernel rather than importing Violet's
board-specific command line.

The memory formats are not interchangeable: Violet's reviewed DT has
8 MiB at 0xa1600000, with a 4 MiB console and 4 MiB pmsg and no ECC
property. Surya/FSociety use 4 MiB at 0x9d800000 with two 512 KiB dump
slots, a 1 MiB console, a 2 MiB pmsg and 16 ECC bytes. The Surya early
console starts at the pmsg base (0x9da00000) and writes a raw
persistent RAM header and data, not ECC parity. A successful pstore
mount does not establish that the recovery kernel uses this layout
or that its ECC reader accepted the early record. Do not remove ECC
or relocate the zone on just the writer side: that does not make the
existing recovery reader compatible. Inspect the recovery dmesg for
ramoops registration, mapping errors and ECC diagnostics first.

The df0c8bf test report's paraphrase of an RPMh address error is not
sufficient to diagnose an invalid APPS RSC MMIO base. At da96d95cb,
the exact ``could not find RPMh address`` string in the interconnect
path is emitted by the BCM Command DB lookup. Its return value is
not used to abort provider registration at that revision. Keep the
observed UFS supplier defer distinct from the report's attribution
of an RSC probe failure; do not change the APPS RSC base on that basis.

A host Reed-Solomon reproduction (16 parity bytes, first root 0,
polynomial 0x11d) demonstrated a specific failure mode: an earlycon
header with size and cursor 8192, paired with stale parity from an
empty pmsg header, decodes back to size and cursor zero. In the kernel,
``persistent_ram_init_ecc()`` decodes the header before
``persistent_ram_post_init()`` checks whether the zone is empty.
This can hide an earlycon record even when DDR survived. It does not
establish what parity or header was present on the failed Surya boot.

Zap recovery (anchor block)
---------------------------

Any later kernel that probes ramoops on the same zones resets the 12-byte
record header (start, size -> 0) before the log can be read, and 4.14
readers additionally recompute the header parity, so pstore in recovery
legitimately shows nothing even when the ring data survived in DRAM.
The earlycon therefore mirrors its header into an anchor block at the
end of the 256 KiB window ("2LCE" magic, cursor, length, generation and
xor checksum).  The collector also raw-dumps the window through
/dev/mem when it is readable, and scripts/dtc/surya-decode-early-log.py
rebuilds the record from the anchor when the base header was zapped.
An anchor that exists with a zero length still proves the kernel reached
parse_earlycon, which separates a dead kernel from a dead log.

Channel switch (no /dev/mem in TWRP)
-----------------------------------

The official recovery exposes no readable ``/dev/mem``, so a raw window
dump is not an option there.  The pmsg prz, in contrast, is not declared
by the recovery kernel at all: pmsg-ramoops files can never appear even
when the ring is intact.  The only zone the TWRP ramoops driver both
maps and exports is the console prz, and 4.14-era captures prove its
records survive into a recovery mount.  The earlycon therefore targets
``0x9d900000`` with the full 1 MiB console zone size so block and header
parity land exactly where the 4.14 reader computes them
(``0x9d9e38f0`` and ``0x9d9ffff0``), and the driver resumes its ring
from the anchor when its own ramoops probe zaps the header mid-boot.

Ownership handover (final)
--------------------------

Official recovery exposes no usable ``/dev/mem`` (the node does not even
exist; after ``mknod`` the kernel still refuses reads), so raw window
capture is dead there.  The test kernel therefore declares every ramoops
prz size as zero: the 6.1 ramoops driver then fails to bind at all and
cannot zap or re-ECC the ring.  The earlycon is the only writer for the
whole boot (it is also the only console, so panics flow into the ring),
and the recovery kernel's own 4.14-era DT layout exports the ring as
``console-ramoops-0`` without touching our header.  The collector's
``dd`` uses MiB units because 32-bit recovery shells overflow on the
raw 0x9d800000 constant.

4.14 header compatibility (the actual root cause)
--------------------------------------------------

The recovery dmesg settled the long-standing empty-pstore mystery.  Its
ramoops reports ``ecc: 0/0`` (parity is never validated by this TWRP)
and ``found existing invalid buffer, size 52101, start 52133``: the
openela 4.14 reader rejects any header where ``start > size`` and
zaps it.  The earlycon updated ``start`` before ``size``, so a warm
reset landing between those two 32-bit writes left exactly that
torn state, and every loop iteration had its header destroyed at the
next TWRP boot.  The writer now publishes ``size`` first, so the
header is always in the ``start <= size`` form both reader generations
accept; the anchor-resume guard tightened to match.  Recovery also
relocates pstore records into ``*/recovery/last_log`` at boot and
erases the mount, which is why ``/sys/fs/pstore`` can legitimately
read empty even after a good record; the collector now copies those
destinations and traces recovery logs for ramoops mentions.

dprz smuggling (capture v3)
---------------------------

TWRP's own boot still zaps or consumes whatever lives in the console
and pmsg prz before the collector can mount pstore (proven by the
empty mount with no "invalid buffer" report, and by ``last_log.gz``
containing only recovery chatter).  The one prz family the 4.14
recovery kernel reads without a probe-time zap is the dump zone set:
``ramoops_init_przs()`` passes no ZAP flag there.  The earlycon
therefore targets the first legacy dump prz (``0x9d800000``, 256 KiB
window inside a 512 KiB dprz) and seeds its ring with the exact
``====<sec>.<nsec>-P`` kmsg header that ``ramoops_read_kmsg_hdr()``
requires, so recovery exports the whole boot log as
``dmesg-ramoops-0`` through its normal move-to-last-log path.  The
ioremap-leaking remap initcall was dropped (it was the source of the
``ioremap_allowed`` and "early ioremap leak" warnings visible in the
log3-era capture, which also confirmed the channel: that 11,525-byte
/sdcard/console-ramoops-0 is the authentic era of build-time ICC QoS
aborts, ``qcom_icc_rpmh_config_qos`` -> regmap read -> synchronous
external abort -> panic+5s reboot; log3's era predates skip-qos and
is not the current failure).

Pre-earlycon triage breadcrumbs
-------------------------------

With the dprz channel live (no "invalid buffer" report and no export at
all proved the current images die before parse\_earlycon while still
dying through a PS_HOLD-class panic reboot), ``start_kernel`` itself
now writes complete persistent_ram records into the second legacy dump
prz (``0x9d880000``, outside the ring window): ``SURYA-CRUMB
BOOT-ENTRY`` before ``setup_arch`` and ``ARCH-DONE`` after it.  The
recovery exports them as ``dmesg-ramoops-1`` even when the boot console
never registers, splitting the death window into decompressor,
setup\_arch, or post-setup.  Remove the two ``surya_boot_canary()``
call sites in init/main.c once booting works.
