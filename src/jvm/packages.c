/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * The installed-package ledger of the Android device Lunaria presents.
 *
 * PackageManager (getInstalledPackages, getPackageInfo, queryIntentActivities)
 * and the device shell's `pm list packages` all answer from this one table.
 * Two of them disagreeing is itself something an app can measure, so there is
 * exactly one of it and every reader goes through this interface.
 *
 * Why it has to hold more than the running app
 * --------------------------------------------
 * A device has never had an empty package list.  An app that targets API 30 or
 * later sees, without any permission at all:
 *
 *   - itself,
 *   - every package its manifest <queries> names or whose components match a
 *     <queries><intent>, and
 *   - the force-queryable set, which on a Google device includes the Play
 *     Store and Play services because they declare android:forceQueryable.
 *
 * Reporting one package -- the caller, which every enumerating loop skips --
 * therefore models no device that exists.  Cross Worlds' security module reads
 * the list through both PackageManager and `pm list packages -f`, finds it
 * empty in both, and reports the environment as unsupported; Play Core reads
 * it and reports PLAY_STORE_NOT_FOUND for an asset pack that is on disk.
 *
 * What is in the ledger is an installation's property, not a run's, so it is
 * configured in lunaria.conf (see LUNARIA_PACKAGES / LUNARIA_PACKAGES_EXTRA
 * there) with a built-in default that describes the device profile Lunaria is
 * reporting itself as.
 *
 * Every entry's sourceDir is materialised on the guest filesystem when the
 * ledger is built -- see ledger_materialise().  `pm list packages -f` prints a
 * path, and an app is free to open what it printed; a ledger that advertises
 * an APK open(2) cannot find describes a broken device rather than a stock
 * one, which is worse than describing none.
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "jvm.h"
#include "lunaria_os.h"
#include "arm_exec.h"

/* ApplicationInfo.flags, as the framework defines them.  The ledger stores the
 * value the guest is handed, so the names are Android's and not ours. */
#define AI_FLAG_SYSTEM               0x00000001
#define AI_FLAG_HAS_CODE             0x00000004
#define AI_FLAG_ALLOW_CLEAR_USER_DATA 0x00000040
#define AI_FLAG_UPDATED_SYSTEM_APP   0x00000080
#define AI_FLAG_ALLOW_BACKUP         0x00008000
#define AI_FLAG_INSTALLED            0x00800000

#define AI_FLAGS_BASE \
   (AI_FLAG_HAS_CODE | AI_FLAG_ALLOW_CLEAR_USER_DATA | \
    AI_FLAG_ALLOW_BACKUP | AI_FLAG_INSTALLED)

/* One row of the ledger.  `record` is what callers see; the rest is how this
 * file answers questions about the row that the guest-visible structure has no
 * field for. */
struct ledger_row {
  struct lunaria_android_package record;
  char *name;
  char *source_dir;
  char *data_dir;
  char *lib_dir;
  char *version_name;
  int force_queryable;   /* <application android:forceQueryable="true"> */
};

static struct ledger_row *g_rows;
static size_t g_nrows, g_cap;
static int g_built;

/* ------------------------------------------------------------------ *
 * The running application's own row
 * ------------------------------------------------------------------ */

static int current_package(struct lunaria_android_package *out)
{
  const char *name = getenv("ANDROID_PACKAGE_NAME");

  if (!out || !name || !*name) return 0;
  out->name = name;
  out->version_name = NULL;       /* the running app reads its manifest */
  out->version_code = 0;
  out->source_dir = lunaria_android_apk_path();
  out->data_dir = lunaria_android_data_path();
  out->lib_dir = lunaria_android_native_lib_path();
  out->uid = arm_exec_guest_uid();
  /* An installed app is FLAG_INSTALLED with code, backup and clearable data.
   * Zero said "not installed, no code" about the package that is running. */
  out->flags = AI_FLAGS_BASE;
  out->target_sdk = lunaria_app_target_sdk();
  return 1;
}

/* ------------------------------------------------------------------ *
 * The built-in default ledger
 * ------------------------------------------------------------------ *
 *
 * A stock Google device carries a few hundred packages.  Listing all of them
 * would be a transcription with no reader: what an app can actually observe
 * without QUERY_ALL_PACKAGES is the force-queryable set plus whatever its own
 * <queries> names, and what the shell prints is only ever read by a grep for a
 * particular name.  So the table holds the packages an application has a
 * reason to look for -- the Play Store, the platform itself, the browser and
 * the shell -- at the paths a Pixel-class device keeps them.
 *
 * Google Play services (com.google.android.gms, com.google.android.gsf) is in
 * it, as on a Pixel.  The emulator ships no Google-signed APK: the file at the
 * ledger path is an empty archive until the installation puts a genuine one
 * there, and PackageInfo carries signatures only from a real signed APK.  An
 * installation that wants a device without it lists `-com.google.android.gms`
 * in LUNARIA_PACKAGES.
 */
static const char kDefaultLedger[] =
  "# <package>                        <uid>  <flags>            <sourceDir> [<versionName>/<versionCode>]\n"
  "android                            1000   system,queryable   /system/framework/framework-res.apk\n"
  "com.android.shell                  2000   system             /system/priv-app/Shell/Shell.apk\n"
  "com.android.settings               1000   system             /system/priv-app/Settings/Settings.apk\n"
  "com.android.systemui               10035  system             /system_ext/priv-app/SystemUI/SystemUI.apk\n"
  "com.android.providers.settings     1000   system             /system/priv-app/SettingsProvider/SettingsProvider.apk\n"
  "com.android.providers.media.module 10062  system             /apex/com.android.mediaprovider/priv-app/MediaProvider/MediaProvider.apk\n"
  "com.android.vending                10108  system,queryable,updated /product/priv-app/Phonesky/Phonesky.apk 38.5.21-31/83852130\n"
  "com.google.android.webview         10088  system,queryable   /product/app/WebViewGoogle/WebViewGoogle.apk 120.0.6099.230/609923033\n"
  "com.google.android.gms             10109  system,queryable,updated /product/priv-app/PrebuiltGmsCore/PrebuiltGmsCore.apk 23.50.13/235013000\n"
  "com.google.android.gsf             10109  system,queryable   /product/priv-app/GoogleServicesFramework/GoogleServicesFramework.apk\n"
  "com.android.chrome                 10091  system,updated     /product/app/Chrome/Chrome.apk 120.0.6099.230/609923033\n"
  "com.google.android.googlequicksearchbox 10092 system         /product/priv-app/Velvet/Velvet.apk\n"
  "com.google.android.inputmethod.latin    10093 system         /product/app/LatinIMEGooglePrebuilt/LatinIMEGooglePrebuilt.apk\n"
  "com.google.android.apps.photos     10094  system             /product/priv-app/Photos/Photos.apk\n"
  "com.google.android.youtube         10095  system             /product/app/YouTube/YouTube.apk\n"
  "com.google.android.gm              10096  system             /product/app/PrebuiltGmail/PrebuiltGmail.apk\n";

/* ------------------------------------------------------------------ *
 * Materialising a row's APK on the guest filesystem
 * ------------------------------------------------------------------ */

/* Where an absolute guest path that is not one of the staged Android roots
 * lands.  This is the same choice arm_exec.cpp's guest_root_dir() makes, and
 * it has to stay the same one: the guest opens the path the ledger printed. */
static const char *guest_root(void)
{
  static char root[PATH_MAX];
  if (!root[0]) {
    const char *r = getenv("LUNARIA_GUEST_ROOT");
    if (!r || !*r) r = "/tmp/lunaria-guest-root";
    snprintf(root, sizeof root, "%s", r);
    (void)luna_file_mkdir(root, 0755);
  }
  return root;
}

static void mkdir_p(char *path)
{
  for (char *p = path + 1; *p; ++p) {
    if (*p != '/') continue;
    *p = '\0';
    (void)luna_file_mkdir(path, 0755);
    *p = '/';
  }
  (void)luna_file_mkdir(path, 0755);
}

/* The device's system image is shared by every app of this data folder:
 * <data root>/system/<guest path> is the real, signed APK of a system package
 * (Play services, the Play Store).  Puts it where this app's guest root expects
 * the package, replacing a stub.  False when no such file is installed. */
static bool shared_system_apk(const char *guest_path, const char *host)
{
  const char *root = getenv("LUNARIA_DATA_ROOT");
  char shared[PATH_MAX];
  struct stat st;
  if (!root || !*root ||
      (size_t)snprintf(shared, sizeof shared, "%s/system%s", root, guest_path) >= sizeof shared ||
      stat(shared, &st) != 0 || st.st_size <= 22)
    return false;
  (void)unlink(host);
  return luna_file_symlink(shared, host) == 0;
}

/* A package's APK, as far as anything outside its own installer can tell, is a
 * zip archive at the path PackageManager reports.  The emulator has no copy of
 * a Pixel's system image, so what it can honestly provide is an archive that
 * opens and holds nothing -- 22 bytes of end-of-central-directory record, the
 * shortest well-formed zip there is.  That keeps open(2), stat(2) and any zip
 * reader in agreement with `pm list packages -f`, which is the disagreement
 * this exists to avoid.  It is deliberately not a fabricated manifest: a file
 * that claims to be a signed Google APK and is not would be a worse lie than
 * an empty one. */
static void ledger_materialise(const char *guest_path)
{
  char host[PATH_MAX];
  char dir[PATH_MAX];
  struct stat st;
  char *slash;
  FILE *f;
  static const unsigned char eocd[22] = {
    'P', 'K', 0x05, 0x06, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0
  };

  if (!guest_path || guest_path[0] != '/') return;
  if ((size_t)snprintf(host, sizeof host, "%s%s", guest_root(), guest_path)
      >= sizeof host)
    return;
  if (stat(host, &st) == 0 && st.st_size > 0) {
    /* A stub left by an earlier run gives way to the real APK once it is
     * installed in the shared system image. */
    if (st.st_size > 22 || !shared_system_apk(guest_path, host)) return;
    return;
  }

  snprintf(dir, sizeof dir, "%s", host);
  slash = strrchr(dir, '/');
  if (!slash || slash == dir) return;
  *slash = '\0';
  mkdir_p(dir);

  if (shared_system_apk(guest_path, host)) return;
  f = fopen(host, "wb");
  if (!f) return;
  (void)fwrite(eocd, 1, sizeof eocd, f);
  (void)fclose(f);
}

/* /system/build.prop: the device's build properties as a file, from the same
 * table android.os.Build and __system_property_get answer from -- Android has
 * one set of properties, and apps (and `getprop`-style readers) read this
 * file as well. */
static void build_prop_materialise(void)
{
  char host[PATH_MAX], dir[PATH_MAX], *slash;
  FILE *f;
  if ((size_t)snprintf(host, sizeof host, "%s/system/build.prop", guest_root())
      >= sizeof host)
    return;
  snprintf(dir, sizeof dir, "%s", host);
  slash = strrchr(dir, '/');
  if (!slash || slash == dir) return;
  *slash = '\0';
  mkdir_p(dir);
  f = fopen(host, "wb");
  if (!f) return;
  fputs("# begin build properties\n", f);
  for (size_t i = 0; ; ++i) {
    const char *name = lunaria_android_property_name(i);
    if (!name) break;
    fprintf(f, "%s=%s\n", name, lunaria_android_property(name));
  }
  (void)fclose(f);
}

/* /system/etc/security/otacerts.zip: the certificates an OTA package must be
 * signed with.  This device installs no OTA from a vendor, so the archive holds
 * none -- an empty zip, not a missing file: Play services reads the file at every
 * check-in, and a device image always carries it. */
static void ota_certs_materialise(void)
{
  static const unsigned char empty_zip[22] = {
    'P', 'K', 5, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
  };
  char host[PATH_MAX], dir[PATH_MAX], *slash;
  FILE *f;
  if ((size_t)snprintf(host, sizeof host, "%s/system/etc/security/otacerts.zip", guest_root())
      >= sizeof host)
    return;
  snprintf(dir, sizeof dir, "%s", host);
  slash = strrchr(dir, '/');
  if (!slash || slash == dir) return;
  *slash = '\0';
  mkdir_p(dir);
  f = fopen(host, "wb");
  if (!f) return;
  (void)fwrite(empty_zip, 1, sizeof empty_zip, f);
  (void)fclose(f);
}

/* ------------------------------------------------------------------ *
 * Building the ledger
 * ------------------------------------------------------------------ */

static char *dup_range(const char *s, size_t n)
{
  char *p = malloc(n + 1);
  if (!p) return NULL;
  memcpy(p, s, n);
  p[n] = '\0';
  return p;
}

static int row_reserve(void)
{
  if (g_nrows < g_cap) return 1;
  size_t cap = g_cap ? g_cap * 2u : 32u;
  struct ledger_row *rows = realloc(g_rows, cap * sizeof *rows);
  if (!rows) return 0;
  g_rows = rows;
  g_cap = cap;
  return 1;
}

static int row_exists(const char *name)
{
  for (size_t i = 0; i < g_nrows; ++i)
    if (!strcmp(g_rows[i].name, name)) return 1;
  return 0;
}

/* One ledger line:  <package> [uid] [flags] [sourceDir]
 *
 * flags is a comma-separated subset of {system, queryable, updated} or "-".
 * Everything after the package name may be left off; the defaults describe an
 * ordinary user-installed app, which is what a bare name in
 * LUNARIA_PACKAGES_EXTRA means. */
static void ledger_add_line(const char *line, size_t len)
{
  const char *tok[5] = { NULL, NULL, NULL, NULL, NULL };
  size_t toklen[5] = { 0, 0, 0, 0, 0 };
  int ntok = 0;
  const char *p = line, *end = line + len;

  while (p < end && ntok < 5) {
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    if (p >= end) break;
    const char *s = p;
    while (p < end && *p != ' ' && *p != '\t') ++p;
    tok[ntok] = s;
    toklen[ntok] = (size_t)(p - s);
    ++ntok;
  }
  if (ntok < 1 || !toklen[0]) return;

  char name[256];
  if (toklen[0] >= sizeof name) return;
  memcpy(name, tok[0], toklen[0]);
  name[toklen[0]] = '\0';
  if (row_exists(name)) return;

  /* An app uid that no other row claims, for entries that do not state one.
   * FIRST_APPLICATION_UID is 10000; the running app already holds one. */
  static int next_uid = 10200;
  int uid = (ntok > 1) ? (int)strtol(tok[1], NULL, 10) : 0;
  if (uid <= 0) uid = next_uid++;

  int flags = AI_FLAGS_BASE;
  int queryable = 0;
  if (ntok > 2) {
    const char *f = tok[2], *fend = tok[2] + toklen[2];
    while (f < fend) {
      const char *comma = memchr(f, ',', (size_t)(fend - f));
      size_t n = comma ? (size_t)(comma - f) : (size_t)(fend - f);
      if (n == 6 && !memcmp(f, "system", 6)) flags |= AI_FLAG_SYSTEM;
      else if (n == 9 && !memcmp(f, "queryable", 9)) queryable = 1;
      else if (n == 7 && !memcmp(f, "updated", 7))
        flags |= AI_FLAG_UPDATED_SYSTEM_APP;
      f = comma ? comma + 1 : fend;
    }
  }

  char apk[PATH_MAX];
  if (ntok > 3 && toklen[3] < sizeof apk) {
    memcpy(apk, tok[3], toklen[3]);
    apk[toklen[3]] = '\0';
  } else if (flags & AI_FLAG_SYSTEM) {
    snprintf(apk, sizeof apk, "/system/app/%s/%s.apk", name, name);
  } else {
    snprintf(apk, sizeof apk, "/data/app/%s-1/base.apk", name);
  }

  /* The fifth column is "<versionName>/<versionCode>".  A package on a device
   * always has a version name, so an entry that gives none gets 1.0. */
  char version[64] = "1.0";
  int version_code = 1;
  if (ntok > 4 && toklen[4] < sizeof version) {
    memcpy(version, tok[4], toklen[4]);
    version[toklen[4]] = '\0';
    char *slash = strchr(version, '/');
    if (slash) {
      *slash = '\0';
      version_code = (int)strtol(slash + 1, NULL, 10);
    }
  }

  char data[PATH_MAX], lib[PATH_MAX];
  snprintf(data, sizeof data, "/data/user/0/%s", name);
  /* Never empty: an app that builds nativeLibraryDir + "/lib<x>.so" out of
   * this gets a path, the way it does on a device.  A system package's
   * libraries are the platform's, an installed one's are its own. */
  if (flags & AI_FLAG_SYSTEM)
    snprintf(lib, sizeof lib, "/system/lib64");
  else
    snprintf(lib, sizeof lib, "/data/app/%s-1/lib/arm64", name);

  if (!row_reserve()) return;
  struct ledger_row *row = &g_rows[g_nrows];
  memset(row, 0, sizeof *row);
  row->name = dup_range(name, strlen(name));
  row->source_dir = dup_range(apk, strlen(apk));
  row->data_dir = dup_range(data, strlen(data));
  row->lib_dir = dup_range(lib, strlen(lib));
  row->version_name = dup_range(version, strlen(version));
  if (!row->name || !row->source_dir || !row->data_dir || !row->lib_dir ||
      !row->version_name) {
    free(row->name);
    free(row->source_dir);
    free(row->data_dir);
    free(row->lib_dir);
    free(row->version_name);
    return;
  }
  row->force_queryable = queryable;
  row->record.name = row->name;
  row->record.source_dir = row->source_dir;
  row->record.data_dir = row->data_dir;
  row->record.lib_dir = row->lib_dir;
  row->record.uid = uid;
  row->record.flags = flags;
  row->record.version_name = row->version_name;
  row->record.version_code = version_code;
  /* A platform package targets the platform it shipped with. */
  row->record.target_sdk = lunaria_sdk_int();
  ++g_nrows;

  ledger_materialise(row->source_dir);
}

static void ledger_add_text(const char *text)
{
  const char *p = text;
  while (*p) {
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    const char *hash = memchr(p, '#', len);
    if (hash) len = (size_t)(hash - p);
    while (len && (p[len - 1] == ' ' || p[len - 1] == '\t' ||
                   p[len - 1] == '\r'))
      --len;
    if (len) ledger_add_line(p, len);
    if (!nl) break;
    p = nl + 1;
  }
}

static char *read_file(const char *path)
{
  FILE *f = fopen(path, "rb");
  long n;
  char *buf;
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 ||
      fseek(f, 0, SEEK_SET) != 0 || n > (1L << 20)) {
    (void)fclose(f);
    return NULL;
  }
  buf = malloc((size_t)n + 1u);
  if (!buf) {
    (void)fclose(f);
    return NULL;
  }
  if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
    free(buf);
    (void)fclose(f);
    return NULL;
  }
  buf[n] = '\0';
  (void)fclose(f);
  return buf;
}

/* Takes a package out of the table (never row 0, the running app). */
static void ledger_remove(const char *name)
{
  for (size_t i = 1; i < g_nrows; ++i) {
    if (strcmp(g_rows[i].name, name)) continue;
    free(g_rows[i].name);
    free(g_rows[i].source_dir);
    free(g_rows[i].data_dir);
    free(g_rows[i].lib_dir);
    free(g_rows[i].version_name);
    memmove(&g_rows[i], &g_rows[i + 1], (g_nrows - i - 1) * sizeof *g_rows);
    --g_nrows;
    return;
  }
}

/* One item of an inline list: `default` (the built-in image), `-name` (drop a
 * package), or a ledger line / bare package name. */
static void ledger_add_item(char *item)
{
  while (*item == ' ' || *item == '\t') ++item;
  size_t n = strlen(item);
  while (n && (item[n - 1] == ' ' || item[n - 1] == '\t')) item[--n] = '\0';
  if (!n) return;
  if (!strcmp(item, "default")) ledger_add_text(kDefaultLedger);
  else if (item[0] == '-') ledger_remove(item + 1);
  else ledger_add_line(item, n);
}

/* LUNARIA_PACKAGES: `default`, `none`, the path of a ledger file, or an inline
 * list separated by ';' -- e.g.
 *   default;-com.google.android.googlequicksearchbox;com.google.android.gms 10109 system,queryable /product/priv-app/PrebuiltGmsCore/PrebuiltGmsCore.apk
 * A value with no ';' that is not a readable file is a one-item list. */
static void ledger_add_spec(const char *spec)
{
  if (!strcmp(spec, "none")) return;   /* no device packages, only the app */
  if (!strchr(spec, ';')) {
    char *text = read_file(spec);
    if (text) {
      ledger_add_text(text);
      free(text);
      return;
    }
    if (strchr(spec, '/') || strchr(spec, '\\')) {
      fprintf(stderr, "[pm] LUNARIA_PACKAGES=%s cannot be read (%s) -- "
                      "using the built-in ledger\n", spec, strerror(errno));
      ledger_add_text(kDefaultLedger);
      return;
    }
  }
  char *copy = dup_range(spec, strlen(spec));
  if (!copy) return;
  for (char *e = copy; *e; ) {
    char *sep = strchr(e, ';');
    if (sep) *sep = '\0';
    ledger_add_item(e);
    if (!sep) break;
    e = sep + 1;
  }
  free(copy);
}

static void ledger_build(void)
{
  struct lunaria_android_package self;
  const char *spec, *extra;

  g_built = 1;
  build_prop_materialise();
  ota_certs_materialise();

  /* The caller is row 0.  Every other reader of this table expects to find
   * the running package in it, and a lookup that misses it would answer
   * "not installed" about the process asking. */
  if (row_reserve() && current_package(&self)) {
    struct ledger_row *row = &g_rows[g_nrows];
    memset(row, 0, sizeof *row);
    row->record = self;
    row->name = dup_range(self.name, strlen(self.name));
    row->record.name = row->name ? row->name : self.name;
    ++g_nrows;
  }

  spec = getenv("LUNARIA_PACKAGES");
  if (!spec || !*spec) spec = "default";
  ledger_add_spec(spec);

  /* Packages this installation adds on top, as bare names or as full ledger
   * lines separated by ';'. */
  extra = getenv("LUNARIA_PACKAGES_EXTRA");
  if (extra && *extra) {
    char *copy = dup_range(extra, strlen(extra));
    if (copy) {
      for (char *e = copy; *e; ) {
        char *sep = strpbrk(e, ",;");
        if (sep) *sep = '\0';
        ledger_add_line(e, strlen(e));
        if (!sep) break;
        e = sep + 1;
      }
      free(copy);
    }
  }

  if (getenv("LUNARIA_TRACE_PM")) {
    for (size_t i = 0; i < g_nrows; ++i)
      fprintf(stderr, "[pm] ledger %zu: %s uid=%d flags=0x%x%s %s\n",
              i, g_rows[i].record.name, g_rows[i].record.uid,
              g_rows[i].record.flags,
              g_rows[i].force_queryable ? " queryable" : "",
              g_rows[i].record.source_dir);
  }
}

static void ledger_ready(void)
{
  if (!g_built) ledger_build();
}

/* ------------------------------------------------------------------ *
 * The interface the rest of the emulator uses
 * ------------------------------------------------------------------ */

size_t lunaria_android_package_count(void)
{
  ledger_ready();
  return g_nrows;
}

int lunaria_android_package_at(size_t index,
                               struct lunaria_android_package *out)
{
  ledger_ready();
  if (!out || index >= g_nrows) return 0;
  *out = g_rows[index].record;
  return 1;
}

int lunaria_android_package_find(const char *name,
                                 struct lunaria_android_package *out)
{
  ledger_ready();
  if (!name || !*name || !out) return 0;
  for (size_t i = 0; i < g_nrows; ++i) {
    if (!strcmp(name, g_rows[i].record.name)) {
      *out = g_rows[i].record;
      return 1;
    }
  }
  return 0;
}

/* The device's own filesystem is laid out by the system, not by the app that
 * happens to be running, so this does not go through the guest's credentials. */
static void host_mkdir_p(char *path)
{
  for (char *p = path + 1; *p; ++p) {
    if (*p != '/') continue;
    *p = '\0';
    (void)luna_file_mkdir(path, 0755);
    *p = '/';
  }
  (void)luna_file_mkdir(path, 0755);
}

/* A package learned at run time (the caller of a Binder transaction is an
 * installed package, and the Play Store looks it up by uid and reads its
 * signing certificate from its APK).  `host_apk` is where that package's APK
 * really is; it is made visible at the package's sourceDir.  A package the
 * ledger already has is left as it is. */
int lunaria_android_package_install(const char *name, uint32_t uid, const char *host_apk)
{
  char line[1024], guest[512], host[PATH_MAX], dir[PATH_MAX], *slash;
  ledger_ready();
  if (!name || !*name || strlen(name) > 200 || row_exists(name)) return 0;
  char version[256];
  int version_code = 0, target_sdk = 0;
  bool have_manifest = host_apk && host_apk[0] == '/';
  if (have_manifest && luna_apk_package_version(host_apk,name,&version_code,version,sizeof version,&target_sdk) < 1)
    return -1;
  snprintf(guest, sizeof guest, "/data/app/%s/base.apk", name);
  snprintf(line, sizeof line, "%s %u - %s", name, uid, guest);
  size_t before = g_nrows;
  ledger_add_line(line, strlen(line));
  if (g_nrows == before) return -1;
  if (have_manifest) {
    struct ledger_row *row = &g_rows[g_nrows-1];
    char *copy = strdup(version);
    if (!copy) {
      ledger_remove(name);
      return -1;
    }
    free(row->version_name);
    row->version_name = copy;
    row->record.version_name = *copy ? copy : NULL;
    row->record.version_code = version_code;
    row->record.target_sdk = target_sdk;
  }
  if (!host_apk || host_apk[0] != '/') return 1;
  /* ledger_add_line materialised an empty archive there: put the real one in
   * its place. */
  if ((size_t)snprintf(host, sizeof host, "%s%s", guest_root(), guest) >= sizeof host) return 1;
  snprintf(dir, sizeof dir, "%s", host);
  slash = strrchr(dir, '/');
  if (!slash || slash == dir) return 1;
  *slash = '\0';
  host_mkdir_p(dir);
  (void)unlink(host);
  if (luna_file_symlink(host_apk, host) != 0)
    fprintf(stderr, "[pm] cannot show %s at %s: %s\n", host_apk, host, strerror(errno));
  else
    fprintf(stderr, "[pm] installed %s (uid %u): %s -> %s\n", name, uid, host, host_apk);
  return 1;
}

static int holds_permission(const char *want)
{
  const char *list = getenv("ANDROID_REQUESTED_PERMISSIONS");
  size_t n = strlen(want);
  if (!list) return 0;
  for (const char *p = list; *p;) {
    const char *end = strchr(p, ',');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len == n && !memcmp(p, want, n)) return 1;
    if (!end) break;
    p = end + 1;
  }
  return 0;
}

static int csv_has(const char *list, const char *want)
{
  size_t n;
  if (!list || !want) return 0;
  n = strlen(want);
  while (*list) {
    const char *end = strchr(list, ',');
    size_t len = end ? (size_t)(end - list) : strlen(list);
    if (len == n && !memcmp(list, want, n)) return 1;
    if (!end) break;
    list = end + 1;
  }
  return 0;
}

int lunaria_android_package_visible(const struct lunaria_android_package *record)
{
  const char *self = getenv("ANDROID_PACKAGE_NAME");

  if (!record || !record->name) return 0;
  /* A package always sees itself. */
  if (self && !strcmp(self, record->name)) return 1;
  /* Filtering starts at target API 30; older apps see the whole device. */
  if (lunaria_app_target_sdk() < 30) return 1;
  if (holds_permission("android.permission.QUERY_ALL_PACKAGES")) return 1;
  /* The force-queryable set: packages that declare android:forceQueryable,
   * which every caller sees with no <queries> entry and no permission.  This
   * is why an app on a Google device can always find the Play Store. */
  ledger_ready();
  for (size_t i = 0; i < g_nrows; ++i) {
    if (strcmp(g_rows[i].record.name, record->name)) continue;
    if (g_rows[i].force_queryable) return 1;
    break;
  }
  /* Android 11 package visibility includes packages matching an <intent>
   * under <queries>.  The launcher records the manifest declarations, while
   * this device ledger records which modeled packages actually provide such
   * an activity.  Settings has a MAIN activity on Android; the framework and
   * provider packages do not become visible merely because MAIN was queried. */
  if (!strcmp(record->name, "com.android.settings") &&
      csv_has(getenv("ANDROID_QUERY_ACTIONS"), "android.intent.action.MAIN"))
    return 1;
  if (csv_has(getenv("ANDROID_QUERY_PACKAGES"), record->name)) return 1;
  return 0;
}

/* Android's external-source policy is per installed package. Keep it under
 * the device data root rather than an app's preferences. A declaration of
 * REQUEST_INSTALL_PACKAGES is not a user grant. */
static int install_source_path(const char *package_name, char *path, size_t cap)
{
  const char *root = getenv("LUNARIA_DATA_ROOT");
  if (!root || !*root || !package_name || !*package_name) return 0;
  unsigned char first = (unsigned char)*package_name;
  if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '_')) return 0;
  for (const unsigned char *p = (const unsigned char *)package_name; *p; ++p)
    if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
          (*p >= '0' && *p <= '9') || *p == '_' || *p == '.')) return 0;
  int n = snprintf(path, cap, "%s/system/install-sources/%s", root, package_name);
  return n >= 0 && (size_t)n < cap;
}

int lunaria_android_install_source_allowed(const char *package_name)
{
  char path[4096];
  if (!install_source_path(package_name, path, sizeof path)) return 0;
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  int value = fgetc(f), end = fgetc(f);
  int valid = !ferror(f);
  fclose(f);
  return valid && value == '1' && end == EOF;
}

int lunaria_android_install_source_set_allowed(const char *package_name, int allowed)
{
  char path[4096];
  if (!install_source_path(package_name, path, sizeof path)) {
    errno = EINVAL;
    return 0;
  }
  const char *root = getenv("LUNARIA_DATA_ROOT");
  char directory[4096];
  const char *suffixes[] = { "system", "system/install-sources" };
  for (size_t i = 0; i < sizeof suffixes / sizeof suffixes[0]; ++i) {
    int n = snprintf(directory, sizeof directory, "%s/%s", root, suffixes[i]);
    if (n < 0 || (size_t)n >= sizeof directory) {
      errno = ENAMETOOLONG;
      return 0;
    }
    if (luna_file_mkdir(directory, 0700) && errno != EEXIST) return 0;
  }
  FILE *f = fopen(path, "wb");
  if (!f) return 0;
  int ok = fputc(allowed ? '1' : '0', f) != EOF;
  if (fclose(f)) ok = 0;
  return ok;
}

/* Process-local provider selection and initialization order. */
void luna_providers_close(struct luna_providers *p)
{
  free(p->items);
  free(p->storage);
  memset(p,0,sizeof *p);
}
static int luna_provider_integer(const char *s,int *out)
{
  char *end;
  long n;
  errno=0;
  n=strtol(s,&end,10);
  if(!*s || *end || errno || n<INT_MIN || n>INT_MAX)return -EINVAL;
  *out=(int)n;
  return 0;
}
static int luna_provider_compare(const void *a,const void *b)
{
  const struct luna_provider *x=a,*y=b;
  if(x->init_order!=y->init_order)return x->init_order>y->init_order ? -1 : 1;
  return x->ordinal<y->ordinal ? -1 : x->ordinal!=y->ordinal;
}
/* v2 uses newline records so an authority list can contain semicolons.
 * Legacy name|authority|metadata records retain their original separator. */
int luna_providers_open(struct luna_providers *p,const char *text,
                        const char *package)
{
  memset(p,0,sizeof *p);
  if(!text || !*text)return 0;
  int version2=!strncmp(text,"!2\n",3);
  if(version2)text+=3;
  const char separator=version2 ? '\n' : ';';
  size_t capacity=1;
  for(const char *s=text; *s; ++s)if(*s==separator)++capacity;
  p->storage=strdup(text);
  p->items=calloc(capacity,sizeof *p->items);
  if(!p->storage || !p->items) {
    luna_providers_close(p);
    return -ENOMEM;
  }
  char *entry=p->storage;
  size_t count_dbg=0;
  char *fields_dbg[8]= {0};
  while(entry && *entry) {
    char *next=strchr(entry,separator);
    if(next)*next++=0;
    char *fields[8]= {0};
    fields[0]=entry;
    size_t count=1;
    count_dbg=0;
    for(char *s=entry; *s; ++s)if(*s=='|') {
        if(count==8)goto invalid;
        *s=0;
        fields[count++]=s+1;
      }
    count_dbg=count;
    for(size_t i=0; i<8; ++i)fields_dbg[i]=fields[i];
    if(count!=(version2 ? 8u : 3u) || !*fields[0] || !*fields[1])goto invalid;
    struct luna_provider *r=&p->items[p->count];
    r->name=fields[0];
    r->authority=fields[1];
    r->metadata=fields[2];
    r->process=version2 ? fields[3] : (char *)(package ? package : "");
    r->enabled=1;
    r->ordinal=p->count;
    if(version2 && (!*r->process || luna_provider_integer(fields[4],&r->enabled) ||
                    luna_provider_integer(fields[5],&r->init_order) ||
                    luna_provider_integer(fields[6],&r->grant_uri) ||
                    luna_provider_integer(fields[7],&r->multiprocess) ||
                    (r->enabled!=0 && r->enabled!=1) || (r->grant_uri!=0 && r->grant_uri!=1) ||
                    (r->multiprocess!=0 && r->multiprocess!=1)))goto invalid;
    ++p->count;
    entry=next;
  }
  qsort(p->items,p->count,sizeof *p->items,luna_provider_compare);
  return 0;
invalid:
  fprintf(stderr,"[providers] malformed manifest provider entry (%zu fields): name=%.120s authority=%.120s process=%.80s enabled=%.8s init=%.8s grant=%.8s multi=%.8s\n",
          count_dbg,entry,count_dbg>1&&fields_dbg[1]?fields_dbg[1]:"",
          count_dbg>3&&fields_dbg[3]?fields_dbg[3]:"",count_dbg>4&&fields_dbg[4]?fields_dbg[4]:"",
          count_dbg>5&&fields_dbg[5]?fields_dbg[5]:"",count_dbg>6&&fields_dbg[6]?fields_dbg[6]:"",
          count_dbg>7&&fields_dbg[7]?fields_dbg[7]:"");
  luna_providers_close(p);
  return -EINVAL;
}
int luna_provider_local(const struct luna_provider *p,const char *process)
{
  return p->enabled && process && (!strcmp(p->process,process) || p->multiprocess);
}
int luna_provider_authority(const struct luna_provider *p,const char *wanted)
{
  if(!wanted || !*wanted)return 0;
  size_t n=strlen(wanted);
  const char *s=p->authority;
  while(s && *s) {
    const char *end=strchr(s,';');
    size_t len=end ? (size_t)(end-s) : strlen(s);
    if(len==n && !memcmp(s,wanted,n))return 1;
    s=end ? end+1 : NULL;
  }
  return 0;
}
