/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RUNTIME_PSN_H
#define RUNTIME_PSN_H
#include <stdint.h>
int runtime_psn_selected(void);
int runtime_psn_start(void);
uintptr_t runtime_psn_resolve(const char *name);
int32_t runtime_psn_system_count(void);
int32_t runtime_psn_system_receive(void *event);
#endif
