// SPDX-FileCopyrightText: 2026 Froststrap
// SPDX-FileCopyrightText: 2026 Voidstrap
//
// SPDX-License-Identifier: MPL-2.0

#ifndef VIRTUALDISPLAY_H
#define VIRTUALDISPLAY_H

#include <stddef.h>
#include <stdint.h>

int vd_configure(const char *name, uint32_t width, uint32_t height, const double *rates, size_t count);
int vd_start(void);
int vd_running(void);
void vd_stop(void);

#endif
