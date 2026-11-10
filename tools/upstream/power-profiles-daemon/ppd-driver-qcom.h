// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (c) 2026 CoWave Fall
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * Qualcomm qcom-cpufreq-hw CPU driver declarations.
 */

#pragma once

#include "ppd-driver-cpu.h"

#define PPD_TYPE_DRIVER_QCOM (ppd_driver_qcom_get_type ())
G_DECLARE_FINAL_TYPE (PpdDriverQcom, ppd_driver_qcom, PPD, DRIVER_QCOM, PpdDriverCpu)
