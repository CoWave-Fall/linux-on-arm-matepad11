// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (c) 2026 CoWave Fall
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Qualcomm qcom-cpufreq-hw CPU driver, with matching Adreno devfreq limits.
 *
 * qcom-cpufreq-hw 没有 energy performance preference 接口，也没有
 * intel_pstate/amd_pstate 那样的内部调速逻辑，因此按 profile 直接调整
 * scaling_governor 与 scaling_max_freq。GPU 使用 devfreq 的 min_freq 与
 * max_freq 限制频率范围：performance 固定最高档，power-saver 限制在
 * 400 MHz 以内。
 */

#define G_LOG_DOMAIN "QcomDriver"

#include <errno.h>
#include <glib/gstdio.h>
#include <upower.h>

#include "ppd-utils.h"
#include "ppd-driver-qcom.h"

#define CPUFREQ_POLICY_DIR "/sys/devices/system/cpu/cpufreq"
#define GPU_DEVFREQ_DIR   "/sys/class/devfreq/3d00000.gpu"

#define CPUFREQ_DRIVER_NAME "qcom-cpufreq-hw"

struct _PpdDriverQcom
{
  PpdDriverCpu  parent_instance;

  PpdProfile activated_profile;
  gboolean has_gpu;
};

G_DEFINE_TYPE (PpdDriverQcom, ppd_driver_qcom, PPD_TYPE_DRIVER_CPU)

static GObject*
ppd_driver_qcom_constructor (GType                  type,
                             guint                  n_construct_params,
                             GObjectConstructParam *construct_params)
{
  GObject *object;

  object = G_OBJECT_CLASS (ppd_driver_qcom_parent_class)->constructor (type,
                                                                      n_construct_params,
                                                                      construct_params);
  g_object_set (object,
                "driver-name", "qcom",
                "profiles", PPD_PROFILE_PERFORMANCE | PPD_PROFILE_BALANCED | PPD_PROFILE_POWER_SAVER,
                NULL);

  return object;
}

static char *
read_file_string (const char *path)
{
  char *contents = NULL;

  if (!g_file_get_contents (path, &contents, NULL, NULL))
    return NULL;

  return g_strstrip (contents);
}

static gboolean
write_file_string (const char *path,
                   const char *value,
                   GError    **error)
{
  return ppd_utils_write (path, value, error);
}

static char *
policy_path (const char *policy, const char *attribute)
{
  g_autofree char *dir = NULL;

  dir = ppd_utils_get_sysfs_path (CPUFREQ_POLICY_DIR);
  return g_build_filename (dir, policy, attribute, NULL);
}

static gboolean
read_uint64 (const char *path, guint64 *value)
{
  g_autofree char *contents = NULL;

  contents = read_file_string (path);
  if (!contents || *contents == '\0')
    return FALSE;

  *value = g_ascii_strtoull (contents, NULL, 10);
  return TRUE;
}

static GArray *
read_frequency_array (const char *path)
{
  g_autofree char *contents = NULL;
  g_autofree char **tokens = NULL;
  GArray *freqs;
  guint i;

  contents = read_file_string (path);
  freqs = g_array_new (FALSE, FALSE, sizeof (guint64));
  if (!contents)
    return freqs;

  tokens = g_strsplit_set (contents, " \t\n", -1);
  for (i = 0; tokens[i]; i++) {
    guint64 freq;

    if (*tokens[i] == '\0')
      continue;
    freq = g_ascii_strtoull (tokens[i], NULL, 10);
    if (freq > 0)
      g_array_append_val (freqs, freq);
  }

  return freqs;
}

static guint64
pick_capped_frequency (guint64 max_freq,
                       GArray *freqs,
                       guint   percent)
{
  guint64 target, best = 0;
  guint i;

  if (freqs->len == 0)
    return max_freq;

  target = max_freq * percent / 100;

  /*
   * 选择不超过目标的最大有效档位。频率表通常升序，但不能依赖顺序；
   * 没有档位落在目标以下时回退到最小档位。
   */
  for (i = 0; i < freqs->len; i++) {
    guint64 freq = g_array_index (freqs, guint64, i);

    if (freq <= target && freq > best)
      best = freq;
  }

  if (best != 0)
    return best;

  best = g_array_index (freqs, guint64, 0);
  for (i = 1; i < freqs->len; i++)
    best = MIN (best, g_array_index (freqs, guint64, i));

  return best;
}

static gboolean
write_frequency (const char     *policy,
                 const char     *attribute,
                 guint64         freq,
                 GError        **error)
{
  g_autofree char *path = NULL;
  g_autofree char *value = NULL;

  path = policy_path (policy, attribute);
  /* sysfs 只接受十进制数字，不能带 locale 的千位分隔符。 */
  value = g_strdup_printf ("%" G_GUINT64_FORMAT, freq);
  return write_file_string (path, value, error);
}

static gboolean
apply_policy (const char   *policy,
              PpdProfile    profile,
              GError      **error)
{
  g_autofree char *driver = NULL;
  g_autofree char *governor_path = NULL;
  g_autofree char *min_path = NULL;
  g_autofree char *max_path = NULL;
  g_autofree char *freqs_path = NULL;
  g_autoptr(GArray) freqs = NULL;
  guint64 min_freq, max_freq, current_max, target_max, target_min;
  const char *governor;

  driver = read_file_string (policy_path (policy, "scaling_driver"));
  if (g_strcmp0 (driver, CPUFREQ_DRIVER_NAME) != 0) {
    g_debug ("%s 使用 %s，跳过", policy, driver ? driver : "(未知)");
    return TRUE;
  }

  min_path = policy_path (policy, "cpuinfo_min_freq");
  max_path = policy_path (policy, "cpuinfo_max_freq");
  freqs_path = policy_path (policy, "scaling_available_frequencies");
  governor_path = policy_path (policy, "scaling_governor");

  if (!read_uint64 (min_path, &min_freq) || !read_uint64 (max_path, &max_freq))
    return FALSE;

  freqs = read_frequency_array (freqs_path);
  {
    g_autofree char *current_path = policy_path (policy, "scaling_max_freq");

    /* current_max 决定两条限制的写入顺序，读取失败时不能继续 */
    if (!read_uint64 (current_path, &current_max))
      return FALSE;
  }

  switch (profile) {
  case PPD_PROFILE_PERFORMANCE:
    governor = "performance";
    target_min = max_freq;
    target_max = max_freq;
    break;
  case PPD_PROFILE_POWER_SAVER:
    governor = "schedutil";
    target_min = min_freq;
    target_max = pick_capped_frequency (max_freq, freqs, 60);
    break;
  case PPD_PROFILE_BALANCED:
  default:
    governor = "schedutil";
    target_min = min_freq;
    target_max = max_freq;
    break;
  }

  if (!write_file_string (governor_path, governor, error))
    return FALSE;

  /*
   * 内核要求 scaling_min_freq <= scaling_max_freq。提高下限时先写上限，
   * 其余情况先写下限，避免中间状态越过另一端的限制。
   */
  if (target_min >= current_max) {
    if (!write_frequency (policy, "scaling_max_freq", target_max, error))
      return FALSE;
    return write_frequency (policy, "scaling_min_freq", target_min, error);
  }

  if (!write_frequency (policy, "scaling_min_freq", target_min, error))
    return FALSE;
  return write_frequency (policy, "scaling_max_freq", target_max, error);
}

static gboolean
apply_gpu_profile (PpdProfile profile,
                   GError   **error)
{
  g_autofree char *dir = NULL;
  g_autofree char *min_path = NULL;
  g_autofree char *max_path = NULL;
  g_autofree char *freqs_path = NULL;
  g_autoptr(GArray) freqs = NULL;
  guint64 hw_min, hw_max, current_max, target_min, target_max;
  guint i;

  dir = ppd_utils_get_sysfs_path (GPU_DEVFREQ_DIR);
  if (!g_file_test (dir, G_FILE_TEST_IS_DIR))
    return TRUE;

  min_path = g_build_filename (dir, "min_freq", NULL);
  max_path = g_build_filename (dir, "max_freq", NULL);
  freqs_path = g_build_filename (dir, "available_frequencies", NULL);

  freqs = read_frequency_array (freqs_path);
  if (freqs->len == 0)
    return FALSE;

  /* devfreq 的 min_freq/max_freq 会被当前档位限制，硬件范围取自可用频率表。 */
  hw_min = hw_max = g_array_index (freqs, guint64, 0);
  for (i = 1; i < freqs->len; i++) {
    guint64 freq = g_array_index (freqs, guint64, i);

    hw_min = MIN (hw_min, freq);
    hw_max = MAX (hw_max, freq);
  }
  if (!read_uint64 (max_path, &current_max))
    return FALSE;

  switch (profile) {
  case PPD_PROFILE_PERFORMANCE:
    target_min = hw_max;
    target_max = hw_max;
    break;
  case PPD_PROFILE_POWER_SAVER:
    target_min = hw_min;
    target_max = pick_capped_frequency (hw_max, freqs, 70);
    break;
  case PPD_PROFILE_BALANCED:
  default:
    target_min = hw_min;
    target_max = hw_max;
    break;
  }

  if (target_min >= current_max) {
    g_autofree char *max_value = g_strdup_printf ("%" G_GUINT64_FORMAT, target_max);
    g_autofree char *min_value = g_strdup_printf ("%" G_GUINT64_FORMAT, target_min);

    if (!write_file_string (max_path, max_value, error))
      return FALSE;
    return write_file_string (min_path, min_value, error);
  }

  {
    g_autofree char *max_value = g_strdup_printf ("%" G_GUINT64_FORMAT, target_max);
    g_autofree char *min_value = g_strdup_printf ("%" G_GUINT64_FORMAT, target_min);

    if (!write_file_string (min_path, min_value, error))
      return FALSE;
    return write_file_string (max_path, max_value, error);
  }
}

static PpdProbeResult
ppd_driver_qcom_probe (PpdDriver *driver)
{
  PpdDriverQcom *qcom = PPD_DRIVER_QCOM (driver);
  g_autofree char *dir = NULL;
  g_autoptr(GDir) gdir = NULL;
  const char *name;
  guint policies = 0;

  dir = ppd_utils_get_sysfs_path (CPUFREQ_POLICY_DIR);
  gdir = g_dir_open (dir, 0, NULL);
  if (!gdir) {
    g_debug ("无法打开 %s", dir);
    return PPD_PROBE_RESULT_FAIL;
  }

  while ((name = g_dir_read_name (gdir)) != NULL) {
    g_autofree char *driver_path = NULL;
    g_autofree char *scaling_driver = NULL;

    if (!g_str_has_prefix (name, "policy"))
      continue;

    driver_path = policy_path (name, "scaling_driver");
    scaling_driver = read_file_string (driver_path);
    if (g_strcmp0 (scaling_driver, CPUFREQ_DRIVER_NAME) != 0) {
      g_debug ("%s 由 %s 驱动，本驱动不接管",
               name, scaling_driver ? scaling_driver : "(未知)");
      return PPD_PROBE_RESULT_FAIL;
    }

    policies++;
  }

  if (policies == 0)
    return PPD_PROBE_RESULT_FAIL;

  {
    g_autofree char *gpu_dir = ppd_utils_get_sysfs_path (GPU_DEVFREQ_DIR);

    qcom->has_gpu = gpu_dir && g_file_test (gpu_dir, G_FILE_TEST_IS_DIR);
  }

  g_debug ("找到 %u 个 qcom-cpufreq-hw policy，GPU devfreq %s",
           policies, qcom->has_gpu ? "可用" : "缺失");
  return PPD_PROBE_RESULT_SUCCESS;
}

static gboolean
ppd_driver_qcom_activate_profile (PpdDriver                 *driver,
                                  PpdProfile                 profile,
                                  PpdProfileActivationReason reason,
                                  GError                   **error)
{
  PpdDriverQcom *qcom = PPD_DRIVER_QCOM (driver);
  g_autofree char *dir = NULL;
  g_autoptr(GDir) gdir = NULL;
  const char *name;
  gboolean ret = TRUE;

  if (profile == PPD_PROFILE_UNSET)
    return TRUE;

  dir = ppd_utils_get_sysfs_path (CPUFREQ_POLICY_DIR);
  gdir = g_dir_open (dir, 0, NULL);
  if (!gdir) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                 "无法打开 %s", dir);
    return FALSE;
  }

  while ((name = g_dir_read_name (gdir)) != NULL) {
    if (!g_str_has_prefix (name, "policy"))
      continue;

    if (!apply_policy (name, profile, error)) {
      ret = FALSE;
      break;
    }
  }

  if (ret && qcom->has_gpu && !apply_gpu_profile (profile, error))
    ret = FALSE;

  if (ret) {
    qcom->activated_profile = profile;
    g_debug ("已应用 %s profile",
             ppd_profile_to_str (profile));
  }

  return ret;
}

static void
ppd_driver_qcom_class_init (PpdDriverQcomClass *klass)
{
  GObjectClass *object_class;
  PpdDriverClass *driver_class;

  object_class = G_OBJECT_CLASS (klass);
  object_class->constructor = ppd_driver_qcom_constructor;

  driver_class = PPD_DRIVER_CLASS (klass);
  driver_class->probe = ppd_driver_qcom_probe;
  driver_class->activate_profile = ppd_driver_qcom_activate_profile;
}

static void
ppd_driver_qcom_init (PpdDriverQcom *qcom)
{
}
