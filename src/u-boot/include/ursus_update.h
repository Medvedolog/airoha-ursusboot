/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __URSUS_UPDATE_H
#define __URSUS_UPDATE_H

#include <linux/types.h>

#define URSUS_FIP_STAGE_ADDR 0x8e000000UL
#define URSUS_FIP_STAGE_MAX  0x00100000UL

enum ursus_fip_kind {
    URSUS_FIP_KIND_URSUS = 0,   /* UrsusBoot -> UrsusBoot self-update */
    URSUS_FIP_KIND_VANILLA,     /* one-way UrsusBoot -> pinned Vanilla OpenWrt U-Boot */
};

int ursus_fip_validate(ulong addr, size_t len, bool stock_limit);
int ursus_fip_update_start(ulong addr, size_t len);
int ursus_vanilla_fip_validate(ulong addr, size_t len, bool stock_limit);
int ursus_vanilla_fip_update_start(ulong addr, size_t len);
bool ursus_vanilla_fip_pinned(void);
const char *ursus_fip_update_kind(void);
int ursus_fip_update_step(void);
bool ursus_fip_update_active(void);
bool ursus_fip_update_complete(void);
bool ursus_fip_update_failed(void);
const char *ursus_fip_update_stage(void);
const char *ursus_fip_update_detail(void);
const char *ursus_fip_update_layout(void);
unsigned int ursus_fip_update_percent(void);
int ursus_fip_update_error(void);
const char *ursus_fip_update_failed_stage(void);
const char *ursus_fip_update_last_success_stage(void);
const char *ursus_fip_update_error_code(void);
const char *ursus_fip_update_transaction_state(void);

#endif
