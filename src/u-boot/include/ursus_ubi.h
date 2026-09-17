/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __URSUS_UBI_H
#define __URSUS_UBI_H

#include <linux/types.h>

#define URSUS_UBI_PRELOADER_ADDR 0x8f000000UL
#define URSUS_UBI_PRELOADER_MAX  0x00020000UL

struct ursus_ubi_volume_diag {
    bool present;
    bool valid;
    int id;
    int type;
    unsigned long long used_bytes;
    unsigned int reserved_pebs;
};

struct ursus_ubi_diag {
    bool attached;
    unsigned int peb_count;
    unsigned int good_pebs;
    unsigned int bad_pebs;
    unsigned int corrupt_pebs;
    unsigned int free_pebs;
    unsigned int leb_size;
    struct ursus_ubi_volume_diag fip;
    struct ursus_ubi_volume_diag fit;
    struct ursus_ubi_volume_diag fit_old;
};

int ursus_ubi_boot(void);
const char *ursus_ubi_last_boot_reason(void);
int ursus_ubi_probe(bool *fip_ok, bool *fit_ok);
int ursus_ubi_probe_diag(struct ursus_ubi_diag *diag);

int ursus_ubi_update_fit(ulong fit_addr, size_t fit_len);
int ursus_ubi_update_start(ulong fit_addr, size_t fit_len, bool keep_settings);
int ursus_ubi_reset_settings(void);
int ursus_ubi_update_step(void);
bool ursus_ubi_update_active(void);
bool ursus_ubi_update_complete(void);
bool ursus_ubi_update_failed(void);
const char *ursus_ubi_update_stage(void);
const char *ursus_ubi_update_detail(void);
unsigned int ursus_ubi_update_percent(void);
int ursus_ubi_update_error(void);
const char *ursus_ubi_update_failed_stage(void);
const char *ursus_ubi_update_last_success_stage(void);
const char *ursus_ubi_update_error_code(void);
const char *ursus_ubi_update_transaction_state(void);

int ursus_ubi_migrate_from_stock(ulong fit_addr, size_t fit_len,
                                 ulong preloader_addr, size_t preloader_len);
int ursus_ubi_migration_start(ulong fit_addr, size_t fit_len,
                              ulong preloader_addr, size_t preloader_len);
int ursus_ubi_migration_step(void);
bool ursus_ubi_migration_active(void);
bool ursus_ubi_migration_complete(void);
bool ursus_ubi_migration_failed(void);
const char *ursus_ubi_migration_stage(void);
const char *ursus_ubi_migration_detail(void);
unsigned int ursus_ubi_migration_percent(void);
int ursus_ubi_migration_error(void);
const char *ursus_ubi_migration_failed_stage(void);
const char *ursus_ubi_migration_last_success_stage(void);
const char *ursus_ubi_migration_error_code(void);
const char *ursus_ubi_migration_transaction_state(void);
int ursus_ubi_validate_preloader(ulong addr, size_t len);
int ursus_ubi_prepare_bl2_candidate(ulong preloader_addr, size_t preloader_len);
bool ursus_ubi_bl2_candidate_valid(void);
ulong ursus_ubi_bl2_candidate_addr(void);

#endif
