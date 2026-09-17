/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __URSUS_LED_H
#define __URSUS_LED_H

/* 0.1.0-alpha: experimental board LED mapping is enabled for hardware proof. */
#define URSUS_LED_PRODUCTION_ENABLED 1

void ursus_led_recovery_latched(void);
void ursus_led_poll(void);
void ursus_led_fatal_wait(const char *reason);
void ursus_lan_led_enable(void);

#endif
