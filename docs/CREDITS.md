# Credits and lineage

UrsusBoot builds on a large body of existing bootloader and OpenWrt work.

Particular thanks go to:

- **OpenWrt developers** for the platform, image formats, SDK/toolchains and U-Boot integration.
- **U-Boot developers** for the upstream bootloader.
- **Trusted Firmware-A developers** for the ARM trusted-firmware stack used by the platform.
- **Daniel Golle (dangowrt)** for OpenWrt-aware bootloader/UBI installation work and the broader recovery-oriented U-Boot ecosystem.
- **hanwckf** for the MediaTek `bl-mt798x` work, WebFailsafe and multi-layout/direct-install ideas that became an important reference in the router community.
- **Yuzhii0718** for continuing and extending that ecosystem, including modern recovery/WebFailsafe and Airoha/MediaTek bootloader work used as a practical reference.

UrsusBoot is not presented as an isolated invention. It is an Airoha-focused continuation and integration of those ideas, with its own board policy, image validation, migration, transaction safety and recovery work layered on top.

For exact source/binary lineage, see [PROVENANCE.md](PROVENANCE.md).
