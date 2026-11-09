/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* Google service environments are independent of each application's profile.
 * The default environment retains the installation's existing account store. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline bool luna_google_profile_valid(const char *name)
{
  if (!name || !*name || strlen(name) > 64) return false;
  for (size_t i = 0; name[i]; ++i) {
    unsigned char c = (unsigned char)name[i];
    bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    if (!alnum && c != '_' && !(i && (c == '-' || c == '.'))) return false;
  }
  return true;
}
static inline const char *luna_google_profile_current(void)
{
  const char *profile = getenv("LUNARIA_GMS_PROFILE");
  return profile && *profile ? profile : "default";
}
static inline int luna_google_environment_root(const char *root, const char *profile,
    char *out, size_t cap)
{
  if (!root || !*root || !luna_google_profile_valid(profile)) return -1;
  int n = !strcmp(profile, "default") ? snprintf(out, cap, "%s", root)
          : snprintf(out, cap, "%s/system/gms/%s", root, profile);
  return n < 0 || (size_t)n >= cap ? -1 : 0;
}
static inline int luna_google_accounts_directory(const char *root, const char *profile,
    char *out, size_t cap)
{
  if (!root || !*root || !luna_google_profile_valid(profile)) return -1;
  int n = !strcmp(profile, "default") ? snprintf(out, cap, "%s/system", root)
          : snprintf(out, cap, "%s/system/gms/%s/system", root, profile);
  return n < 0 || (size_t)n >= cap ? -1 : 0;
}
