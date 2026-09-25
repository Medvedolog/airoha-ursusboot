# Credits and lineage

UrsusBoot builds on a large body of existing bootloader and OpenWrt work.

Particular thanks go to:

- **OpenWrt developers** for the platform, image formats, SDK/toolchains and U-Boot integration.
- **U-Boot developers** for the upstream bootloader.
- **Trusted Firmware-A developers** for the ARM trusted-firmware stack used by the platform.
- **Daniel Golle (dangowrt)** for OpenWrt-aware bootloader/UBI installation work and the broader recovery-oriented U-Boot ecosystem.
- **hanwckf** for the MediaTek `bl-mt798x` work, WebFailsafe and multi-layout/direct-install ideas that became an important reference in the router community.
- **Yuzhii0718** for continuing and extending that ecosystem, including modern recovery/WebFailsafe and Airoha/MediaTek bootloader work used as a practical reference.

Airoha / TF-A:

- **Ansuel (Christian Marangi)**, **mkshevetskiy (Mikhail Kshevetskiy)** and **Yuzhii0718** (`atf-airoha`) for the open Airoha TF-A work that the AN758x bootloader community builds on.

AN758x U-Boot / recovery references:

- **pbs05** — [`uboot-an758x`](https://github.com/pbs05/uboot-an758x): an independent AN7581/AN7583 U-Boot with a browser-based recovery interface, including Nokia XG-040G-MD/MF support; a useful reference for Airoha web recovery, NAND backup (physical main-area dumps with bad eraseblocks as `0xFF`) and board-data recovery workflows. UrsusBoot is not derived from it; the two were developed in parallel.
- **Yuzhii0718** — [`bl-mt798x-dhcpd`](https://github.com/Yuzhii0718/bl-mt798x-dhcpd): U-Boot with DHCP server and an advanced web UI, a reference for the current wave of web-recovery U-Boots.

UrsusBoot is not presented as an isolated invention. It is an Airoha-focused continuation and integration of those ideas, with its own board policy, image validation, migration, transaction safety and recovery work layered on top.

For exact source/binary lineage, see [PROVENANCE.md](PROVENANCE.md).
