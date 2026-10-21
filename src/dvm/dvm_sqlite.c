/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * android.database.sqlite on top of the host's own SQLite.
 *
 * Android's SQLiteDatabase is a thin Java layer over the platform SQLite, so
 * the honest model is the same: every database is a real sqlite3 handle, every
 * Cursor is the result set of a real statement, and SQL means what SQLite says
 * it means.  The library is the one the host already ships (libsqlite3 on
 * Linux, the system libsqlite3 on macOS, winsqlite3.dll on Windows) and is
 * opened at run time the way the Vulkan bridge opens libvulkan, so nothing is
 * added to the build and a host without it fails with an SQLiteException that
 * says so instead of answering from a database that is not there.
 *
 * Cursors are materialised when they are created, like Android's CursorWindow:
 * the statement is stepped to the end and each cell keeps its SQLite type, so
 * a Cursor outlives its statement and getInt()/getString() convert exactly as
 * the framework does.
 */

#include "dvm/dvm_sqlite.h"

#include "dvm/dex.h"
#include "dvm/dvm_internal.h"
#include "lunaria_os.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define LUNA_MKDIR(p) _mkdir(p)
#else
#define LUNA_MKDIR(p) mkdir((p), 0771)
#endif

#define ARG(i) (((i) < nargs) ? args[i] : (union dvm_value){ 0 })
#define RETI(v) do { out->i = (int32_t)(v); return true; } while (0)
#define RETJ(v) do { out->j = (int64_t)(v); return true; } while (0)
#define RETL(v) do { out->l = (dvm_ref)(v); return true; } while (0)
#define RETV()  do { return true; } while (0)

#define DB_DESC     "Landroid/database/sqlite/SQLiteDatabase;"
#define CURSOR_DESC "Landroid/database/Cursor;"
#define STMT_DESC   "Landroid/database/sqlite/SQLiteStatement;"

/* --- the host library ---------------------------------------------------- */

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;

enum {
   SQ_OK = 0, SQ_BUSY = 5, SQ_LOCKED = 6, SQ_NOMEM = 7, SQ_READONLY = 8,
   SQ_IOERR = 10, SQ_CORRUPT = 11, SQ_FULL = 13, SQ_CANTOPEN = 14,
   SQ_ABORT = 4, SQ_TOOBIG = 18, SQ_CONSTRAINT = 19, SQ_MISMATCH = 20,
   SQ_MISUSE = 21, SQ_RANGE = 25, SQ_NOTADB = 26, SQ_ROW = 100, SQ_DONE = 101,
   SQ_INTEGER = 1, SQ_FLOAT = 2, SQ_TEXT = 3, SQ_BLOB = 4, SQ_NULL = 5,
   SQ_OPEN_READONLY = 0x1, SQ_OPEN_READWRITE = 0x2, SQ_OPEN_CREATE = 0x4,
};
#define SQ_TRANSIENT ((void (*)(void *))(intptr_t)-1)

static struct sq_api {
   int (*open_v2)(const char *, sqlite3 **, int, const char *);
   int (*close_v2)(sqlite3 *);
   int (*busy_timeout)(sqlite3 *, int);
   int (*prepare_v2)(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
   int (*step)(sqlite3_stmt *);
   int (*finalize)(sqlite3_stmt *);
   int (*column_count)(sqlite3_stmt *);
   const char *(*column_name)(sqlite3_stmt *, int);
   int (*column_type)(sqlite3_stmt *, int);
   int64_t (*column_int64)(sqlite3_stmt *, int);
   double (*column_double)(sqlite3_stmt *, int);
   const unsigned char *(*column_text)(sqlite3_stmt *, int);
   const void *(*column_blob)(sqlite3_stmt *, int);
   int (*column_bytes)(sqlite3_stmt *, int);
   int (*bind_null)(sqlite3_stmt *, int);
   int (*bind_int64)(sqlite3_stmt *, int, int64_t);
   int (*bind_double)(sqlite3_stmt *, int, double);
   int (*bind_text)(sqlite3_stmt *, int, const char *, int, void (*)(void *));
   int (*bind_blob)(sqlite3_stmt *, int, const void *, int, void (*)(void *));
   int (*bind_parameter_count)(sqlite3_stmt *);
   const char *(*errmsg)(sqlite3 *);
   int (*extended_errcode)(sqlite3 *);
   int (*changes)(sqlite3 *);
   int64_t (*last_insert_rowid)(sqlite3 *);
} sq;

static bool g_sq_tried, g_sq_ready;

static bool sq_load(void)
{
   if (g_sq_tried) return g_sq_ready;
   g_sq_tried = true;
   void *lib = NULL;
#if defined(_WIN32)
   lib = luna_os_library_open_local("winsqlite3.dll");
#elif defined(__APPLE__)
   lib = luna_os_library_open_local("/usr/lib/libsqlite3.dylib");
#else
   lib = luna_os_library_open_local("libsqlite3.so.0");
   if (!lib) lib = luna_os_library_open_local("libsqlite3.so");
#endif
   if (!lib) {
      fprintf(stderr, "[sqlite] the host's SQLite cannot be opened (%s)\n",
              luna_os_library_error());
      return false;
   }
#define SQ_SYM(field, name) \
   do { *(void **)&sq.field = luna_os_library_symbol(lib, name); \
        if (!sq.field) { fprintf(stderr, "[sqlite] missing %s\n", name); return false; } } while (0)
   SQ_SYM(open_v2, "sqlite3_open_v2");
   SQ_SYM(close_v2, "sqlite3_close_v2");
   SQ_SYM(busy_timeout, "sqlite3_busy_timeout");
   SQ_SYM(prepare_v2, "sqlite3_prepare_v2");
   SQ_SYM(step, "sqlite3_step");
   SQ_SYM(finalize, "sqlite3_finalize");
   SQ_SYM(column_count, "sqlite3_column_count");
   SQ_SYM(column_name, "sqlite3_column_name");
   SQ_SYM(column_type, "sqlite3_column_type");
   SQ_SYM(column_int64, "sqlite3_column_int64");
   SQ_SYM(column_double, "sqlite3_column_double");
   SQ_SYM(column_text, "sqlite3_column_text");
   SQ_SYM(column_blob, "sqlite3_column_blob");
   SQ_SYM(column_bytes, "sqlite3_column_bytes");
   SQ_SYM(bind_null, "sqlite3_bind_null");
   SQ_SYM(bind_int64, "sqlite3_bind_int64");
   SQ_SYM(bind_double, "sqlite3_bind_double");
   SQ_SYM(bind_text, "sqlite3_bind_text");
   SQ_SYM(bind_blob, "sqlite3_bind_blob");
   SQ_SYM(bind_parameter_count, "sqlite3_bind_parameter_count");
   SQ_SYM(errmsg, "sqlite3_errmsg");
   SQ_SYM(extended_errcode, "sqlite3_extended_errcode");
   SQ_SYM(changes, "sqlite3_changes");
   SQ_SYM(last_insert_rowid, "sqlite3_last_insert_rowid");
#undef SQ_SYM
   g_sq_ready = true;
   return true;
}

/* --- errors -------------------------------------------------------------- */

static const char *sq_exception_class(int rc)
{
   switch (rc & 0xff) {
   case SQ_ABORT:      return "android/database/sqlite/SQLiteAbortException";
   case SQ_BUSY:       return "android/database/sqlite/SQLiteDatabaseLockedException";
   case SQ_LOCKED:     return "android/database/sqlite/SQLiteTableLockedException";
   case SQ_NOMEM:      return "android/database/sqlite/SQLiteOutOfMemoryException";
   case SQ_READONLY:   return "android/database/sqlite/SQLiteReadOnlyDatabaseException";
   case SQ_IOERR:      return "android/database/sqlite/SQLiteDiskIOException";
   case SQ_CORRUPT:
   case SQ_NOTADB:     return "android/database/sqlite/SQLiteDatabaseCorruptException";
   case SQ_FULL:       return "android/database/sqlite/SQLiteFullException";
   case SQ_CANTOPEN:   return "android/database/sqlite/SQLiteCantOpenDatabaseException";
   case SQ_TOOBIG:     return "android/database/sqlite/SQLiteBlobTooBigException";
   case SQ_CONSTRAINT: return "android/database/sqlite/SQLiteConstraintException";
   case SQ_MISMATCH:   return "android/database/sqlite/SQLiteDatatypeMismatchException";
   case SQ_MISUSE:     return "android/database/sqlite/SQLiteMisuseException";
   case SQ_RANGE:      return "android/database/sqlite/SQLiteBindOrColumnIndexOutOfRangeException";
   default:            return "android/database/sqlite/SQLiteException";
   }
}

/* The message the framework's SQLiteConnection builds:
 * "<sqlite message> (code <n>[ <ext>]): , while compiling: <sql>". */
static void sq_throw(struct dvm *vm, sqlite3 *db, int rc, const char *doing,
                     const char *sql)
{
   const char *msg = db ? sq.errmsg(db) : "unknown error";
   int ext = db ? sq.extended_errcode(db) : rc;
   char text[1024];
   if (sql && *sql)
      snprintf(text, sizeof text, "%s (code %d%s%.0d): , %s: %.600s", msg, rc & 0xff,
               ext != rc ? "/" : "", ext != rc ? ext : 0, doing, sql);
   else
      snprintf(text, sizeof text, "%s (code %d)", msg, rc & 0xff);
   dvm__throw(vm, sq_exception_class(rc), "%s", text);
}

static void sq_unavailable(struct dvm *vm)
{
   dvm__throw(vm, "android/database/sqlite/SQLiteCantOpenDatabaseException",
              "the host has no SQLite library to open a database with");
}

/* --- the database -------------------------------------------------------- */

struct sq_db {
   sqlite3 *db;
   int depth;            /* nested beginTransaction() calls */
   bool success;         /* setTransactionSuccessful() on the current level */
   bool child_failed;    /* an inner transaction ended without success */
   bool readonly;
   bool wal;
};

/* The host state of a database, cursor or statement lives in the object's own
 * `data` slot, so the VM frees it with the object (and at shutdown). */
static void *host_data(struct dvm *vm, dvm_ref self)
{
   struct dvm_object *o = dvm__obj(vm, self);
   return o ? o->data : NULL;
}

static void host_data_set(struct dvm *vm, dvm_ref self, void *data,
                          void (*destroy)(void *))
{
   struct dvm_object *o = dvm__obj(vm, self);
   if (!o) return;
   o->data = data;
   o->data_destroy = destroy;
}

static struct sq_db *db_of(struct dvm *vm, dvm_ref self)
{
   struct sq_db *d = host_data(vm, self);
   if (!d)
      dvm__throw(vm, "java/lang/IllegalStateException",
                 "attempt to re-open an already-closed object: SQLiteDatabase");
   return d;
}

static void db_destroy(void *p)
{
   struct sq_db *d = p;
   if (!d) return;
   if (d->db) sq.close_v2(d->db);
   free(d);
}

static bool sq_str(struct dvm *vm, dvm_ref s, const char **out)
{
   *out = s ? dvm_string_utf8(vm, s) : NULL;
   return *out != NULL;
}

/* Runs one statement that may return rows, discarding them. */
static bool sq_exec(struct dvm *vm, struct sq_db *d, const char *sql)
{
   sqlite3_stmt *st = NULL;
   int rc = sq.prepare_v2(d->db, sql, -1, &st, NULL);
   if (rc != SQ_OK) { sq_throw(vm, d->db, rc, "while compiling", sql); return false; }
   if (!st) return true;                      /* empty statement */
   while ((rc = sq.step(st)) == SQ_ROW) {}
   int fin = sq.finalize(st);
   if (rc != SQ_DONE) { sq_throw(vm, d->db, rc, "while executing", sql); return false; }
   (void)fin;
   return true;
}

/* A single integer: PRAGMA user_version and friends. */
static bool sq_query_int(struct dvm *vm, struct sq_db *d, const char *sql,
                         int64_t *value)
{
   sqlite3_stmt *st = NULL;
   int rc = sq.prepare_v2(d->db, sql, -1, &st, NULL);
   if (rc != SQ_OK) { sq_throw(vm, d->db, rc, "while compiling", sql); return false; }
   *value = 0;
   rc = st ? sq.step(st) : SQ_DONE;
   if (rc == SQ_ROW) *value = sq.column_int64(st, 0);
   if (st) sq.finalize(st);
   if (rc != SQ_ROW && rc != SQ_DONE) {
      sq_throw(vm, d->db, rc, "while executing", sql);
      return false;
   }
   return true;
}

static void sq_make_parent_dirs(const char *path)
{
   char dir[1024];
   snprintf(dir, sizeof dir, "%s", path);
   for (char *p = dir + 1; *p; ++p) {
      if (*p != '/') continue;
      *p = '\0';
      (void)LUNA_MKDIR(dir);
      *p = '/';
   }
}

dvm_ref dvm_sqlite_open(struct dvm *vm, const char *path, int flags)
{
   if (!sq_load()) { sq_unavailable(vm); return 0; }
   const bool memory = !path || !*path || !strcmp(path, ":memory:");
   if (!memory && !(flags & 1) && (flags & 0x10000000)) sq_make_parent_dirs(path);
   if (!memory && !(flags & 1)) sq_make_parent_dirs(path);
   const int mode = (flags & 1) ? SQ_OPEN_READONLY
                  : (flags & 0x10000000) ? SQ_OPEN_READWRITE | SQ_OPEN_CREATE
                                         : SQ_OPEN_READWRITE;
   struct sq_db *d = calloc(1, sizeof *d);
   if (!d) { dvm__throw(vm, "java/lang/OutOfMemoryError", "SQLiteDatabase"); return 0; }
   int rc = sq.open_v2(memory ? ":memory:" : path, &d->db, mode, NULL);
   if (rc != SQ_OK) {
      char text[1024];
      snprintf(text, sizeof text, "%s (code %d): , while opening the database file: %s",
               d->db ? sq.errmsg(d->db) : "unable to open database file", rc & 0xff,
               memory ? ":memory:" : path);
      dvm__throw(vm, sq_exception_class(rc), "%s", text);
      if (d->db) sq.close_v2(d->db);
      free(d);
      return 0;
   }
   d->readonly = (flags & 1) != 0;
   (void)sq.busy_timeout(d->db, 2500);
   struct dvm_class *c = dvm__class_by_desc(vm, DB_DESC);
   dvm_ref obj = c ? dvm_new_object(vm, c) : 0;
   if (!obj) { sq.close_v2(d->db); free(d); return 0; }
   host_data_set(vm, obj, d, db_destroy);
   (void)dvm_set_field(vm, obj, "path", "Ljava/lang/String;",
                       (union dvm_value){ .l = dvm_new_string(vm, memory ? ":memory:" : path) });
   dvm_pin(vm, obj);
   if (flags & 0x20000000) {
      if (!sq_exec(vm, d, "PRAGMA journal_mode=WAL")) return 0;
      d->wal = true;
   }
   return obj;
}

bool dvm_sqlite_delete_files(const char *path)
{
   if (!path || !*path) return false;
   char name[1100];
   bool any = remove(path) == 0;
   static const char *const suffix[] = { "-journal", "-wal", "-shm", "-shadow" };
   for (size_t i = 0; i < sizeof suffix / sizeof suffix[0]; ++i) {
      snprintf(name, sizeof name, "%s%s", path, suffix[i]);
      any |= remove(name) == 0;
   }
   return any;
}

/* --- binding ------------------------------------------------------------- */

struct sq_cell {
   uint8_t type;           /* SQ_NULL .. SQ_BLOB */
   int64_t i;
   double d;
   char *p;                /* text or blob bytes, owned */
   int n;
};

static void cell_clear(struct sq_cell *c)
{
   free(c->p);
   memset(c, 0, sizeof *c);
   c->type = SQ_NULL;
}

static bool cell_set_text(struct sq_cell *c, const char *s, size_t n)
{
   char *copy = malloc(n + 1);
   if (!copy) return false;
   memcpy(copy, s, n);
   copy[n] = '\0';
   cell_clear(c);
   c->type = SQ_TEXT;
   c->p = copy;
   c->n = (int)n;
   return true;
}

/* A Java value as a cell: boxed numbers, Boolean, String, byte[]; null is NULL. */
static bool cell_from_object(struct dvm *vm, dvm_ref v, struct sq_cell *c)
{
   cell_clear(c);
   if (!v) return true;
   struct dvm_object *o = dvm__obj(vm, v);
   if (!o) return true;
   if (o->kind == DVM_OBJ_STRING) {
      const char *s = dvm_string_utf8(vm, v);
      return cell_set_text(c, s ? s : "", s ? strlen(s) : 0);
   }
   if (o->kind == DVM_OBJ_ARRAY && o->elem_kind == 'B') {
      c->p = malloc(o->length ? o->length : 1);
      if (!c->p) return false;
      memcpy(c->p, o->data, o->length);
      c->n = (int)o->length;
      c->type = SQ_BLOB;
      return true;
   }
   const char *n = o->cls ? o->cls->name : "";
   if (!o->slots) return true;
   if (!strcmp(n, "java/lang/Long")) { c->type = SQ_INTEGER; c->i = o->slots[0].j; }
   else if (!strcmp(n, "java/lang/Integer") || !strcmp(n, "java/lang/Short") ||
            !strcmp(n, "java/lang/Byte") || !strcmp(n, "java/lang/Boolean")) {
      c->type = SQ_INTEGER; c->i = o->slots[0].i;
   }
   else if (!strcmp(n, "java/lang/Double")) { c->type = SQ_FLOAT; c->d = o->slots[0].d; }
   else if (!strcmp(n, "java/lang/Float")) { c->type = SQ_FLOAT; c->d = (double)o->slots[0].f; }
   else {
      dvm__throw(vm, "java/lang/IllegalArgumentException",
                 "Cannot bind arg of class %s, not allowed on SQLite", n);
      return false;
   }
   return true;
}

static int cell_bind(sqlite3_stmt *st, int index, const struct sq_cell *c)
{
   switch (c->type) {
   case SQ_INTEGER: return sq.bind_int64(st, index, c->i);
   case SQ_FLOAT:   return sq.bind_double(st, index, c->d);
   case SQ_TEXT:    return sq.bind_text(st, index, c->p, c->n, SQ_TRANSIENT);
   case SQ_BLOB:    return sq.bind_blob(st, index, c->p ? c->p : "", c->n, SQ_TRANSIENT);
   default:         return sq.bind_null(st, index);
   }
}

struct sq_binds { int n; struct sq_cell *v; };

static void binds_free(struct sq_binds *b)
{
   if (!b) return;
   for (int i = 0; i < b->n; ++i) cell_clear(&b->v[i]);
   free(b->v);
   free(b);
}

static struct sq_binds *binds_make(int n)
{
   struct sq_binds *b = calloc(1, sizeof *b);
   if (!b) return NULL;
   b->n = n;
   b->v = calloc((size_t)(n ? n : 1), sizeof *b->v);
   if (!b->v) { free(b); return NULL; }
   for (int i = 0; i < n; ++i) b->v[i].type = SQ_NULL;
   return b;
}

static bool binds_bind(struct dvm *vm, struct sq_db *d, sqlite3_stmt *st,
                       const struct sq_binds *b, const char *sql)
{
   const int params = sq.bind_parameter_count(st);
   for (int i = 0; b && i < b->n && i < params; ++i) {
      int rc = cell_bind(st, i + 1, &b->v[i]);
      if (rc != SQ_OK) { sq_throw(vm, d->db, rc, "while binding", sql); return false; }
   }
   return true;
}

/* A String[] / Object[] argument as binds: Strings bind as text, null as an
 * error for selection args and as NULL for execSQL's Object[]. */
static struct sq_binds *binds_from_array(struct dvm *vm, dvm_ref array,
                                         bool objects)
{
   struct dvm_object *o = array ? dvm__obj(vm, array) : NULL;
   const int n = o && o->kind == DVM_OBJ_ARRAY ? (int)o->length : 0;
   struct sq_binds *b = binds_make(n);
   if (!b) { dvm__throw(vm, "java/lang/OutOfMemoryError", "bind args"); return NULL; }
   for (int i = 0; i < n; ++i) {
      dvm_ref v = ((dvm_ref *)o->data)[i];
      if (!v && !objects) {
         dvm__throw(vm, "java/lang/IllegalArgumentException",
                    "the bind value at index %d is null", i);
         binds_free(b);
         return NULL;
      }
      if (!cell_from_object(vm, v, &b->v[i])) { binds_free(b); return NULL; }
   }
   return b;
}

/* --- cursors ------------------------------------------------------------- */

struct sq_cursor {
   int ncols, nrows;
   char **names;
   struct sq_cell *cells;      /* nrows * ncols */
};

static void cursor_data_free(struct sq_cursor *c)
{
   if (!c) return;
   for (int i = 0; i < c->ncols; ++i) free(c->names[i]);
   free(c->names);
   for (long i = 0; i < (long)c->nrows * c->ncols; ++i) free(c->cells[i].p);
   free(c->cells);
   free(c);
}

static struct sq_cursor *cursor_of(struct dvm *vm, dvm_ref self)
{
   return host_data(vm, self);
}

static void cursor_destroy(void *p) { cursor_data_free(p); }

static dvm_ref cursor_wrap(struct dvm *vm, struct sq_cursor *data)
{
   struct dvm_class *c = dvm__class_by_desc(vm, CURSOR_DESC);
   dvm_ref obj = c ? dvm_new_object(vm, c) : 0;
   if (!obj) { cursor_data_free(data); return 0; }
   host_data_set(vm, obj, data, cursor_destroy);
   (void)dvm_set_field(vm, obj, "position", "I", (union dvm_value){ .i = -1 });
   return obj;
}

/* Runs `sql` with `binds` to completion and keeps every row. */
static dvm_ref sq_run_query(struct dvm *vm, struct sq_db *d, const char *sql,
                            const struct sq_binds *binds)
{
   sqlite3_stmt *st = NULL;
   int rc = sq.prepare_v2(d->db, sql, -1, &st, NULL);
   if (rc != SQ_OK) { sq_throw(vm, d->db, rc, "while compiling", sql); return 0; }
   struct sq_cursor *c = calloc(1, sizeof *c);
   if (!c) { if (st) sq.finalize(st); dvm__throw(vm, "java/lang/OutOfMemoryError", "Cursor"); return 0; }
   if (!st) return cursor_wrap(vm, c);
   if (!binds_bind(vm, d, st, binds, sql)) { sq.finalize(st); cursor_data_free(c); return 0; }
   c->ncols = sq.column_count(st);
   c->names = calloc((size_t)(c->ncols ? c->ncols : 1), sizeof *c->names);
   for (int i = 0; c->names && i < c->ncols; ++i) {
      const char *n = sq.column_name(st, i);
      c->names[i] = strdup(n ? n : "");
   }
   int cap = 0;
   while ((rc = sq.step(st)) == SQ_ROW) {
      if (c->nrows == cap) {
         cap = cap ? cap * 2 : 16;
         struct sq_cell *grown = realloc(c->cells,
            (size_t)cap * (size_t)(c->ncols ? c->ncols : 1) * sizeof *c->cells);
         if (!grown) { dvm__throw(vm, "java/lang/OutOfMemoryError", "Cursor"); goto fail; }
         c->cells = grown;
      }
      struct sq_cell *row = c->cells + (size_t)c->nrows * (size_t)c->ncols;
      for (int i = 0; i < c->ncols; ++i) {
         struct sq_cell *cell = &row[i];
         memset(cell, 0, sizeof *cell);
         cell->type = (uint8_t)sq.column_type(st, i);
         if (cell->type == SQ_INTEGER) cell->i = sq.column_int64(st, i);
         else if (cell->type == SQ_FLOAT) cell->d = sq.column_double(st, i);
         else if (cell->type == SQ_TEXT || cell->type == SQ_BLOB) {
            const void *bytes = cell->type == SQ_TEXT ? (const void *)sq.column_text(st, i)
                                                      : sq.column_blob(st, i);
            const int n = sq.column_bytes(st, i);
            cell->p = malloc((size_t)n + 1);
            if (!cell->p) { cell->type = SQ_NULL; ++c->nrows; dvm__throw(vm, "java/lang/OutOfMemoryError", "Cursor"); goto fail; }
            if (n) memcpy(cell->p, bytes, (size_t)n);
            cell->p[n] = '\0';
            cell->n = n;
         }
      }
      ++c->nrows;
   }
   if (rc != SQ_DONE) { sq_throw(vm, d->db, rc, "while executing", sql); goto fail; }
   sq.finalize(st);
   return cursor_wrap(vm, c);
fail:
   sq.finalize(st);
   cursor_data_free(c);
   return 0;
}

/* --- Cursor methods ------------------------------------------------------ */

static struct sq_cell *cursor_cell(struct dvm *vm, dvm_ref self, int column)
{
   struct sq_cursor *c = cursor_of(vm, self);
   union dvm_value pos = { 0 };
   (void)dvm_get_field(vm, self, "position", "I", &pos);
   const int rows = c ? c->nrows : 0;
   if (pos.i < 0 || pos.i >= rows) {
      dvm__throw(vm, "android/database/CursorIndexOutOfBoundsException",
                 "Index %d requested, with a size of %d", pos.i, rows);
      return NULL;
   }
   if (column < 0 || column >= c->ncols) {
      dvm__throw(vm, "android/database/CursorIndexOutOfBoundsException",
                 "Index %d requested, with a size of %d", column, c->ncols);
      return NULL;
   }
   return c->cells + (size_t)pos.i * (size_t)c->ncols + (size_t)column;
}

static bool cur_count(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct sq_cursor *c = cursor_of(vm, self);
   RETI(c ? c->nrows : 0);
}

static bool cur_column_count(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct sq_cursor *c = cursor_of(vm, self);
   RETI(c ? c->ncols : 0);
}

static bool cur_column_name(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   struct sq_cursor *c = cursor_of(vm, self);
   const int i = ARG(0).i;
   if (!c || i < 0 || i >= c->ncols) {
      dvm__throw(vm, "java/lang/ArrayIndexOutOfBoundsException", "length=%d; index=%d",
                 c ? c->ncols : 0, i);
      return false;
   }
   RETL(dvm_new_string(vm, c->names[i]));
}

static bool cur_column_names(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct sq_cursor *c = cursor_of(vm, self);
   const int n = c ? c->ncols : 0;
   dvm_ref arr = dvm_new_array(vm, 'L', "Ljava/lang/String;", (uint32_t)n);
   dvm_ref *slots = arr ? dvm_array_data(vm, arr) : NULL;
   for (int i = 0; slots && i < n; ++i) slots[i] = dvm_new_string(vm, c->names[i]);
   RETL(arr);
}

static int cur_find(struct sq_cursor *c, const char *name)
{
   /* A column asked for as "table.column" matches the part after the dot, as
    * the framework does. */
   const char *dot = strrchr(name, '.');
   const char *want = dot ? dot + 1 : name;
   for (int i = 0; c && i < c->ncols; ++i)
      if (!strcasecmp(c->names[i], want)) return i;
   return -1;
}

static bool cur_column_index(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   const char *name;
   if (!sq_str(vm, ARG(0).l, &name)) {
      dvm__throw(vm, "java/lang/NullPointerException", "columnName");
      return false;
   }
   RETI(cur_find(cursor_of(vm, self), name));
}

static bool cur_column_index_or_throw(struct dvm *vm, dvm_ref self,
                                      const union dvm_value *args, int nargs,
                                      union dvm_value *out)
{
   const char *name;
   if (!sq_str(vm, ARG(0).l, &name)) {
      dvm__throw(vm, "java/lang/NullPointerException", "columnName");
      return false;
   }
   struct sq_cursor *c = cursor_of(vm, self);
   int i = cur_find(c, name);
   if (i < 0) {
      char list[512] = "";
      for (int k = 0; c && k < c->ncols; ++k)
         snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s",
                  k ? ", " : "", c->names[k]);
      dvm__throw(vm, "java/lang/IllegalArgumentException",
                 "column '%s' does not exist. Available columns: [%s]", name, list);
      return false;
   }
   RETI(i);
}

static bool cur_move_to(struct dvm *vm, dvm_ref self, int position, union dvm_value *out)
{
   struct sq_cursor *c = cursor_of(vm, self);
   const int rows = c ? c->nrows : 0;
   if (position < -1) position = -1;
   if (position > rows) position = rows;
   (void)dvm_set_field(vm, self, "position", "I", (union dvm_value){ .i = position });
   RETI(position >= 0 && position < rows);
}

static int cur_pos(struct dvm *vm, dvm_ref self)
{
   union dvm_value p = { 0 };
   (void)dvm_get_field(vm, self, "position", "I", &p);
   return p.i;
}

static bool cur_move_first(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{ (void)args; (void)nargs; return cur_move_to(vm, self, 0, out); }

static bool cur_move_last(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct sq_cursor *c = cursor_of(vm, self);
   return cur_move_to(vm, self, (c ? c->nrows : 0) - 1, out);
}

static bool cur_move_next(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{ (void)args; (void)nargs; return cur_move_to(vm, self, cur_pos(vm, self) + 1, out); }

static bool cur_move_prev(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{ (void)args; (void)nargs; return cur_move_to(vm, self, cur_pos(vm, self) - 1, out); }

static bool cur_move_position(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                              int nargs, union dvm_value *out)
{ return cur_move_to(vm, self, ARG(0).i, out); }

static bool cur_move_by(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{ return cur_move_to(vm, self, cur_pos(vm, self) + ARG(0).i, out); }

static bool cur_get_position(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{ (void)args; (void)nargs; RETI(cur_pos(vm, self)); }

static bool cur_is_first(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct sq_cursor *c = cursor_of(vm, self);
   RETI(c && c->nrows && cur_pos(vm, self) == 0);
}

static bool cur_is_last(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct sq_cursor *c = cursor_of(vm, self);
   RETI(c && c->nrows && cur_pos(vm, self) == c->nrows - 1);
}

static bool cur_is_before_first(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct sq_cursor *c = cursor_of(vm, self);
   RETI(!c || !c->nrows ? 1 : cur_pos(vm, self) == -1);
}

static bool cur_is_after_last(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                              int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct sq_cursor *c = cursor_of(vm, self);
   RETI(!c || !c->nrows ? 1 : cur_pos(vm, self) == c->nrows);
}

static const char *cell_text(const struct sq_cell *c, char *buf, size_t cap)
{
   switch (c->type) {
   case SQ_INTEGER: snprintf(buf, cap, "%" PRId64, c->i); return buf;
   case SQ_FLOAT: {
      /* The shortest decimal that reads back as the same double, in the form
       * Double.toString gives. */
      for (int prec = 15; prec <= 17; ++prec) {
         snprintf(buf, cap, "%.*g", prec, c->d);
         double back = strtod(buf, NULL);
         if (!(back < c->d) && !(back > c->d)) break;
      }
      if (!strpbrk(buf, ".eEn")) strncat(buf, ".0", cap - strlen(buf) - 1);
      return buf;
   }
   default: return c->p;
   }
}

static bool cur_get_string(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   struct sq_cell *c = cursor_cell(vm, self, ARG(0).i);
   if (!c) return false;
   if (c->type == SQ_NULL) RETL(0);
   if (c->type == SQ_BLOB) {
      dvm__throw(vm, "android/database/sqlite/SQLiteException",
                 "unable to convert BLOB to string");
      return false;
   }
   char buf[64];
   RETL(dvm_new_string(vm, cell_text(c, buf, sizeof buf)));
}

static bool cell_to_int64(struct dvm *vm, const struct sq_cell *c, int64_t *v)
{
   switch (c->type) {
   case SQ_INTEGER: *v = c->i; return true;
   case SQ_FLOAT:   *v = (int64_t)c->d; return true;
   case SQ_TEXT:    *v = strtoll(c->p, NULL, 10); return true;
   case SQ_BLOB:
      dvm__throw(vm, "android/database/sqlite/SQLiteException",
                 "unable to convert BLOB to long");
      return false;
   default:         *v = 0; return true;
   }
}

static bool cell_to_double(struct dvm *vm, const struct sq_cell *c, double *v)
{
   switch (c->type) {
   case SQ_INTEGER: *v = (double)c->i; return true;
   case SQ_FLOAT:   *v = c->d; return true;
   case SQ_TEXT:    *v = strtod(c->p, NULL); return true;
   case SQ_BLOB:
      dvm__throw(vm, "android/database/sqlite/SQLiteException",
                 "unable to convert BLOB to double");
      return false;
   default:         *v = 0; return true;
   }
}

static bool cur_get_long(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   struct sq_cell *c = cursor_cell(vm, self, ARG(0).i);
   int64_t v;
   if (!c || !cell_to_int64(vm, c, &v)) return false;
   RETJ(v);
}

static bool cur_get_int(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   struct sq_cell *c = cursor_cell(vm, self, ARG(0).i);
   int64_t v;
   if (!c || !cell_to_int64(vm, c, &v)) return false;
   RETI((int32_t)v);
}

static bool cur_get_short(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   struct sq_cell *c = cursor_cell(vm, self, ARG(0).i);
   int64_t v;
   if (!c || !cell_to_int64(vm, c, &v)) return false;
   RETI((int16_t)v);
}

static bool cur_get_double(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   struct sq_cell *c = cursor_cell(vm, self, ARG(0).i);
   double v;
   if (!c || !cell_to_double(vm, c, &v)) return false;
   out->d = v;
   return true;
}

static bool cur_get_float(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   struct sq_cell *c = cursor_cell(vm, self, ARG(0).i);
   double v;
   if (!c || !cell_to_double(vm, c, &v)) return false;
   out->f = (float)v;
   return true;
}

static bool cur_get_blob(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   struct sq_cell *c = cursor_cell(vm, self, ARG(0).i);
   if (!c) return false;
   if (c->type == SQ_NULL) RETL(0);
   if (c->type == SQ_INTEGER || c->type == SQ_FLOAT) {
      dvm__throw(vm, "android/database/sqlite/SQLiteException",
                 "Unable to convert %s to a blob", c->type == SQ_INTEGER ? "INTEGER" : "FLOAT");
      return false;
   }
   dvm_ref arr = dvm_new_array(vm, 'B', "B", (uint32_t)c->n);
   if (arr && c->n) memcpy(dvm_array_data(vm, arr), c->p, (size_t)c->n);
   RETL(arr);
}

static bool cur_get_type(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   struct sq_cell *c = cursor_cell(vm, self, ARG(0).i);
   if (!c) return false;
   /* Cursor.FIELD_TYPE_NULL/INTEGER/FLOAT/STRING/BLOB = 0..4 */
   static const int field_type[] = { 0, 1, 2, 3, 4, 0 };
   RETI(field_type[c->type]);
}

static bool cur_is_null(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   struct sq_cell *c = cursor_cell(vm, self, ARG(0).i);
   if (!c) return false;
   RETI(c->type == SQ_NULL);
}

static bool cur_close(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
   (void)args; (void)nargs; (void)out;
   cursor_data_free(cursor_of(vm, self));
   host_data_set(vm, self, NULL, NULL);
   (void)dvm_set_field(vm, self, "closed", "Z", (union dvm_value){ .i = 1 });
   (void)dvm_set_field(vm, self, "position", "I", (union dvm_value){ .i = -1 });
   RETV();
}

static bool cur_is_closed(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   (void)dvm_get_field(vm, self, "closed", "Z", &v);
   RETI(v.i);
}

static bool cur_nop(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                    int nargs, union dvm_value *out)
{ (void)vm; (void)self; (void)args; (void)nargs; (void)out; RETV(); }

static bool cur_false(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{ (void)vm; (void)self; (void)args; (void)nargs; RETI(0); }

static bool cur_null(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{ (void)vm; (void)self; (void)args; (void)nargs; RETL(0); }

const struct dvm_sqlite_method dvm_sqlite_cursor_methods[] = {
   { "getCount", "()I", cur_count, 0 },
   { "getColumnCount", "()I", cur_column_count, 0 },
   { "getColumnName", "(I)Ljava/lang/String;", cur_column_name, 0 },
   { "getColumnNames", "()[Ljava/lang/String;", cur_column_names, 0 },
   { "getColumnIndex", "(Ljava/lang/String;)I", cur_column_index, 0 },
   { "getColumnIndexOrThrow", "(Ljava/lang/String;)I", cur_column_index_or_throw, 0 },
   { "moveToFirst", "()Z", cur_move_first, 0 },
   { "moveToLast", "()Z", cur_move_last, 0 },
   { "moveToNext", "()Z", cur_move_next, 0 },
   { "moveToPrevious", "()Z", cur_move_prev, 0 },
   { "moveToPosition", "(I)Z", cur_move_position, 0 },
   { "move", "(I)Z", cur_move_by, 0 },
   { "getPosition", "()I", cur_get_position, 0 },
   { "isFirst", "()Z", cur_is_first, 0 },
   { "isLast", "()Z", cur_is_last, 0 },
   { "isBeforeFirst", "()Z", cur_is_before_first, 0 },
   { "isAfterLast", "()Z", cur_is_after_last, 0 },
   { "getString", "(I)Ljava/lang/String;", cur_get_string, 0 },
   { "getInt", "(I)I", cur_get_int, 0 },
   { "getLong", "(I)J", cur_get_long, 0 },
   { "getShort", "(I)S", cur_get_short, 0 },
   { "getFloat", "(I)F", cur_get_float, 0 },
   { "getDouble", "(I)D", cur_get_double, 0 },
   { "getBlob", "(I)[B", cur_get_blob, 0 },
   { "getType", "(I)I", cur_get_type, 0 },
   { "isNull", "(I)Z", cur_is_null, 0 },
   { "close", "()V", cur_close, 0 },
   { "isClosed", "()Z", cur_is_closed, 0 },
   { "deactivate", "()V", cur_nop, 0 },
   { "requery", "()Z", cur_false, 0 },
   { "setNotificationUri", "(Landroid/content/ContentResolver;Landroid/net/Uri;)V", cur_nop, 0 },
   { "getNotificationUri", "()Landroid/net/Uri;", cur_null, 0 },
   { "registerContentObserver", "(Landroid/database/ContentObserver;)V", cur_nop, 0 },
   { "unregisterContentObserver", "(Landroid/database/ContentObserver;)V", cur_nop, 0 },
   { "registerDataSetObserver", "(Landroid/database/DataSetObserver;)V", cur_nop, 0 },
   { "unregisterDataSetObserver", "(Landroid/database/DataSetObserver;)V", cur_nop, 0 },
   { "getWantsAllOnMoveCalls", "()Z", cur_false, 0 },
   { NULL, NULL, NULL, 0 }
};

const struct dvm_sqlite_field dvm_sqlite_cursor_fields[] = {
   { "position", "I" }, { "closed", "Z" }, { NULL, NULL }
};

/* --- SQL building -------------------------------------------------------- */

struct sq_text { char *p; size_t n, cap; bool failed; };

static void text_add(struct sq_text *t, const char *s)
{
   if (t->failed || !s) return;
   const size_t n = strlen(s);
   if (t->n + n + 1 > t->cap) {
      size_t cap = (t->n + n + 1) * 2;
      char *grown = realloc(t->p, cap);
      if (!grown) { t->failed = true; return; }
      t->p = grown;
      t->cap = cap;
   }
   memcpy(t->p + t->n, s, n + 1);
   t->n += n;
}

static void text_clause(struct sq_text *t, const char *keyword, const char *value)
{
   if (!value || !*value) return;
   text_add(t, keyword);
   text_add(t, value);
}

/* --- ContentValues ------------------------------------------------------- */

struct sq_values { int n; char **keys; struct sq_cell *cells; };

static void values_free(struct sq_values *v)
{
   for (int i = 0; i < v->n; ++i) { free(v->keys[i]); free(v->cells[i].p); }
   free(v->keys);
   free(v->cells);
   memset(v, 0, sizeof *v);
}

static bool call_ref(struct dvm *vm, dvm_ref obj, const char *name, const char *sig,
                     const union dvm_value *args, int nargs, union dvm_value *out)
{
   struct dvm_class *c = dvm_object_class(vm, obj);
   struct dvm_method *m = c ? dvm_find_method(vm, c, name, sig) : NULL;
   if (!m) {
      dvm__throw(vm, "java/lang/NoSuchMethodError", "%s%s", name, sig);
      return false;
   }
   return dvm_call(vm, m, obj, (union dvm_value *)args, nargs, out);
}

/* The entries of a ContentValues, through its own keySet()/get(). */
static bool values_read(struct dvm *vm, dvm_ref cv, struct sq_values *out)
{
   memset(out, 0, sizeof *out);
   if (!cv) return true;
   union dvm_value set = { 0 }, it = { 0 };
   if (!call_ref(vm, cv, "keySet", "()Ljava/util/Set;", NULL, 0, &set) || !set.l) return true;
   if (!call_ref(vm, set.l, "iterator", "()Ljava/util/Iterator;", NULL, 0, &it)) return false;
   int cap = 0;
   for (;;) {
      union dvm_value more = { 0 }, key = { 0 }, val = { 0 };
      if (!call_ref(vm, it.l, "hasNext", "()Z", NULL, 0, &more)) goto fail;
      if (!more.i) break;
      if (!call_ref(vm, it.l, "next", "()Ljava/lang/Object;", NULL, 0, &key)) goto fail;
      union dvm_value k = { .l = key.l };
      if (!call_ref(vm, cv, "get", "(Ljava/lang/String;)Ljava/lang/Object;", &k, 1, &val)) goto fail;
      if (out->n == cap) {
         cap = cap ? cap * 2 : 8;
         char **kk = realloc(out->keys, (size_t)cap * sizeof *kk);
         struct sq_cell *cc = realloc(out->cells, (size_t)cap * sizeof *cc);
         if (kk) out->keys = kk;
         if (cc) out->cells = cc;
         if (!kk || !cc) { dvm__throw(vm, "java/lang/OutOfMemoryError", "ContentValues"); goto fail; }
      }
      const char *name = key.l ? dvm_string_utf8(vm, key.l) : "";
      out->keys[out->n] = strdup(name ? name : "");
      memset(&out->cells[out->n], 0, sizeof out->cells[0]);
      out->cells[out->n].type = SQ_NULL;
      ++out->n;
      if (!cell_from_object(vm, val.l, &out->cells[out->n - 1])) goto fail;
   }
   return true;
fail:
   values_free(out);
   return false;
}

/* --- running prepared SQL ------------------------------------------------ */

/* Executes `sql` once with `binds`; `changes`/`rowid` report the result. */
static bool sq_run_update(struct dvm *vm, struct sq_db *d, const char *sql,
                          const struct sq_binds *binds, int *changes,
                          int64_t *rowid)
{
   sqlite3_stmt *st = NULL;
   int rc = sq.prepare_v2(d->db, sql, -1, &st, NULL);
   if (rc != SQ_OK) { sq_throw(vm, d->db, rc, "while compiling", sql); return false; }
   if (!st) { if (changes) *changes = 0; return true; }
   if (!binds_bind(vm, d, st, binds, sql)) { sq.finalize(st); return false; }
   while ((rc = sq.step(st)) == SQ_ROW) {}
   sq.finalize(st);
   if (rc != SQ_DONE) { sq_throw(vm, d->db, rc, "while executing", sql); return false; }
   if (changes) *changes = sq.changes(d->db);
   if (rowid) *rowid = sq.last_insert_rowid(d->db);
   return true;
}

static const char *const k_conflict[] = {
   "", " OR ROLLBACK", " OR ABORT", " OR FAIL", " OR IGNORE", " OR REPLACE"
};

/* --- SQLiteDatabase methods ---------------------------------------------- */

static bool db_exec_sql(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   (void)out;
   struct sq_db *d = db_of(vm, self);
   const char *sql;
   if (!d) return false;
   if (!sq_str(vm, ARG(0).l, &sql)) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "sql must not be null.");
      return false;
   }
   if (nargs > 1) {
      struct sq_binds *b = binds_from_array(vm, ARG(1).l, true);
      if (!b) return false;
      bool ok = sq_run_update(vm, d, sql, b, NULL, NULL);
      binds_free(b);
      return ok;
   }
   return sq_run_update(vm, d, sql, NULL, NULL, NULL);
}

static bool db_raw_query(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   struct sq_db *d = db_of(vm, self);
   const char *sql;
   if (!d) return false;
   /* rawQuery(sql, args) / rawQueryWithFactory(factory, sql, args, editTable) */
   const int a = nargs >= 4 ? 1 : 0;
   if (!sq_str(vm, ARG(a).l, &sql)) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "sql must not be null.");
      return false;
   }
   struct sq_binds *b = binds_from_array(vm, ARG(a + 1).l, false);
   if (!b) return false;
   dvm_ref c = sq_run_query(vm, d, sql, b);
   binds_free(b);
   if (!c) return false;
   RETL(c);
}

/* query(...): table, columns, selection, args, groupBy, having, orderBy[, limit],
 * optionally preceded by `distinct`. */
static bool db_query(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   struct sq_db *d = db_of(vm, self);
   if (!d) return false;
   const bool distinct = nargs == 9;
   const int a = distinct ? 1 : 0;
   const char *table;
   if (!sq_str(vm, ARG(a).l, &table)) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "Table name cannot be null");
      return false;
   }
   struct sq_text t = { 0 };
   text_add(&t, distinct && ARG(0).i ? "SELECT DISTINCT " : "SELECT ");
   struct dvm_object *cols = ARG(a + 1).l ? dvm__obj(vm, ARG(a + 1).l) : NULL;
   if (cols && cols->kind == DVM_OBJ_ARRAY && cols->length) {
      for (uint32_t i = 0; i < cols->length; ++i) {
         text_add(&t, i ? ", " : "");
         const char *c = ((dvm_ref *)cols->data)[i] ? dvm_string_utf8(vm, ((dvm_ref *)cols->data)[i]) : "NULL";
         text_add(&t, c);
      }
   } else text_add(&t, "*");
   text_add(&t, " FROM ");
   text_add(&t, table);
   const char *where = ARG(a + 2).l ? dvm_string_utf8(vm, ARG(a + 2).l) : NULL;
   const char *group = ARG(a + 4).l ? dvm_string_utf8(vm, ARG(a + 4).l) : NULL;
   const char *having = ARG(a + 5).l ? dvm_string_utf8(vm, ARG(a + 5).l) : NULL;
   const char *order = ARG(a + 6).l ? dvm_string_utf8(vm, ARG(a + 6).l) : NULL;
   const char *limit = nargs > a + 7 && ARG(a + 7).l ? dvm_string_utf8(vm, ARG(a + 7).l) : NULL;
   text_clause(&t, " WHERE ", where);
   text_clause(&t, " GROUP BY ", group);
   text_clause(&t, " HAVING ", having);
   text_clause(&t, " ORDER BY ", order);
   text_clause(&t, " LIMIT ", limit);
   if (t.failed) { free(t.p); dvm__throw(vm, "java/lang/OutOfMemoryError", "query"); return false; }
   struct sq_binds *b = binds_from_array(vm, ARG(a + 3).l, false);
   dvm_ref c = b ? sq_run_query(vm, d, t.p, b) : 0;
   binds_free(b);
   free(t.p);
   if (!c) return false;
   RETL(c);
}

static bool db_insert_impl(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, int conflict, bool or_throw, union dvm_value *out)
{
   struct sq_db *d = db_of(vm, self);
   if (!d) return false;
   const char *table, *hack = ARG(1).l ? dvm_string_utf8(vm, ARG(1).l) : NULL;
   if (!sq_str(vm, ARG(0).l, &table)) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "Table name cannot be null");
      return false;
   }
   struct sq_values v;
   if (!values_read(vm, ARG(2).l, &v)) return false;
   struct sq_text t = { 0 };
   text_add(&t, "INSERT");
   text_add(&t, k_conflict[conflict < 0 || conflict > 5 ? 0 : conflict]);
   text_add(&t, " INTO ");
   text_add(&t, table);
   text_add(&t, "(");
   struct sq_binds *b = NULL;
   if (v.n > 0) {
      for (int i = 0; i < v.n; ++i) { text_add(&t, i ? "," : ""); text_add(&t, v.keys[i]); }
      text_add(&t, ") VALUES (");
      for (int i = 0; i < v.n; ++i) text_add(&t, i ? ",?" : "?");
      b = binds_make(v.n);
      for (int i = 0; b && i < v.n; ++i) { b->v[i] = v.cells[i]; v.cells[i].p = NULL; }
   } else if (hack && *hack) {
      text_add(&t, hack);
      text_add(&t, ") VALUES (NULL");
   } else {
      free(t.p);
      values_free(&v);
      dvm__throw(vm, "java/lang/IllegalArgumentException",
                 "Empty values");
      return false;
   }
   text_add(&t, ")");
   values_free(&v);
   int64_t rowid = -1;
   int changes = 0;
   bool ok = !t.failed && sq_run_update(vm, d, t.p, b, &changes, &rowid);
   binds_free(b);
   free(t.p);
   if (!ok) {
      if (or_throw) return false;
      /* insert() logs the failure and answers -1. */
      char text[256];
      dvm_describe_exception(vm, dvm_exception(vm), text, sizeof text);
      dvm_clear_exception(vm);
      fprintf(stderr, "[sqlite] Error inserting into %s: %s\n", table, text);
      RETJ(-1);
   }
   RETJ(changes > 0 ? rowid : -1);
}

static bool db_insert(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{ return db_insert_impl(vm, self, args, nargs, 0, false, out); }

static bool db_insert_or_throw(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                               int nargs, union dvm_value *out)
{ return db_insert_impl(vm, self, args, nargs, 0, true, out); }

static bool db_insert_with_conflict(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                    int nargs, union dvm_value *out)
{ return db_insert_impl(vm, self, args, nargs, ARG(3).i, true, out); }

static bool db_replace(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{ return db_insert_impl(vm, self, args, nargs, 5, false, out); }

static bool db_replace_or_throw(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                int nargs, union dvm_value *out)
{ return db_insert_impl(vm, self, args, nargs, 5, true, out); }

static bool db_update_impl(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, int conflict, union dvm_value *out)
{
   struct sq_db *d = db_of(vm, self);
   if (!d) return false;
   const char *table;
   if (!sq_str(vm, ARG(0).l, &table)) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "Table name cannot be null");
      return false;
   }
   struct sq_values v;
   if (!values_read(vm, ARG(1).l, &v)) return false;
   if (!v.n) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "Empty values");
      return false;
   }
   const char *where = ARG(2).l ? dvm_string_utf8(vm, ARG(2).l) : NULL;
   struct dvm_object *wa = ARG(3).l ? dvm__obj(vm, ARG(3).l) : NULL;
   const int nwhere = wa && wa->kind == DVM_OBJ_ARRAY ? (int)wa->length : 0;
   struct sq_text t = { 0 };
   text_add(&t, "UPDATE");
   text_add(&t, k_conflict[conflict < 0 || conflict > 5 ? 0 : conflict]);
   text_add(&t, " ");
   text_add(&t, table);
   text_add(&t, " SET ");
   struct sq_binds *b = binds_make(v.n + nwhere);
   for (int i = 0; b && i < v.n; ++i) {
      text_add(&t, i ? "," : "");
      text_add(&t, v.keys[i]);
      text_add(&t, "=?");
      b->v[i] = v.cells[i];
      v.cells[i].p = NULL;
   }
   text_clause(&t, " WHERE ", where);
   for (int i = 0; b && i < nwhere; ++i) {
      dvm_ref s = ((dvm_ref *)wa->data)[i];
      if (!cell_from_object(vm, s, &b->v[v.n + i])) { binds_free(b); b = NULL; }
   }
   values_free(&v);
   int changes = 0;
   bool ok = b && !t.failed && sq_run_update(vm, d, t.p, b, &changes, NULL);
   binds_free(b);
   free(t.p);
   if (!ok) { if (!vm->exception) dvm__throw(vm, "java/lang/OutOfMemoryError", "update"); return false; }
   RETI(changes);
}

static bool db_update(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{ return db_update_impl(vm, self, args, nargs, 0, out); }

static bool db_update_with_conflict(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                    int nargs, union dvm_value *out)
{ return db_update_impl(vm, self, args, nargs, ARG(4).i, out); }

static bool db_delete(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
   struct sq_db *d = db_of(vm, self);
   if (!d) return false;
   const char *table;
   if (!sq_str(vm, ARG(0).l, &table)) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "Table name cannot be null");
      return false;
   }
   struct sq_text t = { 0 };
   text_add(&t, "DELETE FROM ");
   text_add(&t, table);
   text_clause(&t, " WHERE ", ARG(1).l ? dvm_string_utf8(vm, ARG(1).l) : NULL);
   struct sq_binds *b = binds_from_array(vm, ARG(2).l, false);
   int changes = 0;
   bool ok = b && !t.failed && sq_run_update(vm, d, t.p, b, &changes, NULL);
   binds_free(b);
   free(t.p);
   if (!ok) return false;
   RETI(changes);
}

/* Transactions follow SQLiteSession: only the outermost level talks to SQLite,
 * and a transaction commits only if every level asked for it. */
static bool db_begin(struct dvm *vm, dvm_ref self, bool exclusive, union dvm_value *out)
{
   (void)out;
   struct sq_db *d = db_of(vm, self);
   if (!d) return false;
   if (d->depth == 0) {
      if (!sq_exec(vm, d, exclusive ? "BEGIN EXCLUSIVE" : "BEGIN IMMEDIATE")) return false;
      d->success = false;
      d->child_failed = false;
   }
   ++d->depth;
   if (d->depth > 1) d->success = false;
   return true;
}

static bool db_begin_exclusive(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                               int nargs, union dvm_value *out)
{ (void)args; (void)nargs; return db_begin(vm, self, true, out); }

static bool db_begin_immediate(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                               int nargs, union dvm_value *out)
{ (void)args; (void)nargs; return db_begin(vm, self, false, out); }

static bool db_set_success(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   (void)args; (void)nargs; (void)out;
   struct sq_db *d = db_of(vm, self);
   if (!d) return false;
   if (d->depth == 0) {
      dvm__throw(vm, "java/lang/IllegalStateException",
                 "Cannot perform this operation because there is no current transaction.");
      return false;
   }
   if (d->success) {
      dvm__throw(vm, "java/lang/IllegalStateException",
                 "setTransactionSuccessful may only be called once per call to beginTransaction");
      return false;
   }
   d->success = true;
   return true;
}

static bool db_end(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                   int nargs, union dvm_value *out)
{
   (void)args; (void)nargs; (void)out;
   struct sq_db *d = db_of(vm, self);
   if (!d) return false;
   if (d->depth == 0) {
      dvm__throw(vm, "java/lang/IllegalStateException",
                 "Cannot perform this operation because there is no current transaction.");
      return false;
   }
   --d->depth;
   if (d->depth > 0) {
      if (!d->success) d->child_failed = true;
      d->success = false;
      return true;
   }
   const bool commit = d->success && !d->child_failed;
   d->success = d->child_failed = false;
   return sq_exec(vm, d, commit ? "COMMIT" : "ROLLBACK");
}

static bool db_in_transaction(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                              int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct sq_db *d = db_of(vm, self);
   if (!d) return false;
   RETI(d->depth > 0);
}

static bool db_is_open(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   RETI(host_data(vm, self) != NULL);
}

static bool db_is_read_only(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct sq_db *d = db_of(vm, self);
   if (!d) return false;
   RETI(d->readonly);
}

static bool db_close(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
   (void)args; (void)nargs; (void)out;
   struct sq_db *d = host_data(vm, self);
   if (!d) return true;                       /* close() twice is fine */
   host_data_set(vm, self, NULL, NULL);
   db_destroy(d);
   dvm_unpin(vm, self);
   return true;
}

static bool db_get_path(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value p = { 0 };
   (void)dvm_get_field(vm, self, "path", "Ljava/lang/String;", &p);
   RETL(p.l);
}

static bool db_get_version(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct sq_db *d = db_of(vm, self);
   int64_t v;
   if (!d || !sq_query_int(vm, d, "PRAGMA user_version", &v)) return false;
   RETI((int32_t)v);
}

static bool db_set_version(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   (void)out;
   struct sq_db *d = db_of(vm, self);
   if (!d) return false;
   char sql[64];
   snprintf(sql, sizeof sql, "PRAGMA user_version = %d", ARG(0).i);
   return sq_exec(vm, d, sql);
}

static bool db_need_upgrade(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   struct sq_db *d = db_of(vm, self);
   int64_t v;
   if (!d || !sq_query_int(vm, d, "PRAGMA user_version", &v)) return false;
   RETI(ARG(0).i > v);
}

static bool db_enable_wal(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct sq_db *d = db_of(vm, self);
   if (!d) return false;
   if (!d->wal && !sq_exec(vm, d, "PRAGMA journal_mode=WAL")) return false;
   d->wal = true;
   RETI(1);
}

static bool db_disable_wal(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   (void)args; (void)nargs; (void)out;
   struct sq_db *d = db_of(vm, self);
   if (!d) return false;
   if (d->wal && !sq_exec(vm, d, "PRAGMA journal_mode=DELETE")) return false;
   d->wal = false;
   return true;
}

static bool db_is_wal(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct sq_db *d = db_of(vm, self);
   if (!d) return false;
   RETI(d->wal);
}

static bool db_set_foreign_keys(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                int nargs, union dvm_value *out)
{
   (void)out;
   struct sq_db *d = db_of(vm, self);
   if (!d) return false;
   return sq_exec(vm, d, ARG(0).i ? "PRAGMA foreign_keys=ON" : "PRAGMA foreign_keys=OFF");
}

static bool db_page_size(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct sq_db *d = db_of(vm, self);
   int64_t v;
   if (!d || !sq_query_int(vm, d, "PRAGMA page_size", &v)) return false;
   RETJ(v);
}

/* setPageSize / setMaximumSize / getMaximumSize are PRAGMAs, as the framework
 * writes them.  The size comes back rounded to a whole number of pages. */
static bool db_set_page_size(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   struct sq_db *d = db_of(vm, self);
   char sql[64];
   snprintf(sql, sizeof sql, "PRAGMA page_size = %lld", (long long)args[0].j);
   if (!d || !sq_exec(vm, d, sql)) return false;
   RETV();
}

static bool db_maximum_size(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   struct sq_db *d = db_of(vm, self);
   int64_t pages, page;
   if (!d || !sq_query_int(vm, d, "PRAGMA page_size", &page)) return false;
   if (nargs) {
      char sql[64];
      const int64_t want = (args[0].j + page - 1) / page;
      snprintf(sql, sizeof sql, "PRAGMA max_page_count = %lld", (long long)want);
      if (!sq_query_int(vm, d, sql, &pages)) return false;
      RETJ(pages * page);
   }
   if (!sq_query_int(vm, d, "PRAGMA max_page_count", &pages)) return false;
   RETJ(pages * page);
}

/* getAttachedDbs(): the databases this connection can name — "main" on the
 * file it was opened on.  Room's corruption handler deletes every file listed,
 * so the answer has to be the real one. */
static bool db_attached(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct sq_db *d = db_of(vm, self);
   if (!d) return false;
   struct dvm_class *lc = dvm__class_by_desc(vm, "Ljava/util/ArrayList;");
   struct dvm_class *pc = dvm__class_by_desc(vm, "Landroid/util/Pair;");
   dvm_ref list = lc ? dvm_new_object(vm, lc) : 0;
   dvm_ref pair = pc ? dvm_new_object(vm, pc) : 0;
   if (!list || !pair) RETL(list);
   union dvm_value ignored = { 0 }, path = { 0 };
   (void)dvm_get_field(vm, self, "path", "Ljava/lang/String;", &path);
   struct dvm_method *init = dvm_find_method(vm, lc, "<init>", "()V");
   if (init && !dvm_call(vm, init, list, NULL, 0, &ignored)) return false;
   union dvm_value kv[2] = { { .l = dvm_new_string(vm, "main") }, { .l = path.l } };
   if (!call_ref(vm, pair, "<init>", "(Ljava/lang/Object;Ljava/lang/Object;)V", kv, 2, &ignored))
      return false;
   union dvm_value item = { .l = pair };
   if (!call_ref(vm, list, "add", "(Ljava/lang/Object;)Z", &item, 1, &ignored)) return false;
   RETL(list);
}

static bool db_compile(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   struct sq_db *d = db_of(vm, self);
   if (!d) return false;
   if (!ARG(0).l) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "sql must not be null.");
      return false;
   }
   struct dvm_class *c = dvm__class_by_desc(vm, STMT_DESC);
   dvm_ref st = c ? dvm_new_object(vm, c) : 0;
   if (!st) RETL(0);
   (void)dvm_set_field(vm, st, "database", "Landroid/database/sqlite/SQLiteDatabase;",
                       (union dvm_value){ .l = self });
   (void)dvm_set_field(vm, st, "sql", "Ljava/lang/String;", ARG(0));
   /* Compile once now so a bad statement fails here, as it does on a device. */
   sqlite3_stmt *probe = NULL;
   const char *sql = dvm_string_utf8(vm, ARG(0).l);
   int rc = sq.prepare_v2(d->db, sql, -1, &probe, NULL);
   if (rc != SQ_OK) { sq_throw(vm, d->db, rc, "while compiling", sql); return false; }
   if (probe) sq.finalize(probe);
   RETL(st);
}

static bool db_noop(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                    int nargs, union dvm_value *out)
{ (void)vm; (void)self; (void)args; (void)nargs; (void)out; RETV(); }

static bool db_true(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                    int nargs, union dvm_value *out)
{ (void)vm; (void)self; (void)args; (void)nargs; RETI(1); }

/* static SQLiteDatabase.openDatabase(path, factory, flags[, errorHandler]) */
static bool db_open_database(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   (void)self;
   const char *path;
   if (!sq_str(vm, ARG(0).l, &path)) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "path must not be null");
      return false;
   }
   dvm_ref db = dvm_sqlite_open(vm, path, ARG(2).i);
   if (!db) return false;
   RETL(db);
}

/* openOrCreateDatabase(String path | File, factory) */
static bool db_open_or_create(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                              int nargs, union dvm_value *out)
{
   (void)self;
   dvm_ref path = ARG(0).l;
   struct dvm_object *o = path ? dvm__obj(vm, path) : NULL;
   if (o && o->kind != DVM_OBJ_STRING) {        /* a File: its path field */
      union dvm_value p = { 0 };
      (void)dvm_get_field(vm, path, "path", "Ljava/lang/String;", &p);
      path = p.l;
   }
   const char *text;
   if (!sq_str(vm, path, &text)) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "path must not be null");
      return false;
   }
   dvm_ref db = dvm_sqlite_open(vm, text, 0x10000000);
   if (!db) return false;
   RETL(db);
}

static bool db_create(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
   (void)self; (void)args; (void)nargs;
   dvm_ref db = dvm_sqlite_open(vm, ":memory:", 0x10000000);
   if (!db) return false;
   RETL(db);
}

static bool db_delete_database(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                               int nargs, union dvm_value *out)
{
   (void)self;
   union dvm_value p = { 0 };
   if (ARG(0).l) (void)dvm_get_field(vm, ARG(0).l, "path", "Ljava/lang/String;", &p);
   RETI(dvm_sqlite_delete_files(p.l ? dvm_string_utf8(vm, p.l) : NULL));
}

#define SM DEX_ACC_STATIC
const struct dvm_sqlite_method dvm_sqlite_database_methods[] = {
   { "beginTransaction", "()V", db_begin_exclusive, 0 },
   { "beginTransactionNonExclusive", "()V", db_begin_immediate, 0 },
   { "beginTransactionWithListener", "(Landroid/database/sqlite/SQLiteTransactionListener;)V",
     db_begin_exclusive, 0 },
   { "beginTransactionWithListenerNonExclusive",
     "(Landroid/database/sqlite/SQLiteTransactionListener;)V", db_begin_immediate, 0 },
   { "setTransactionSuccessful", "()V", db_set_success, 0 },
   { "endTransaction", "()V", db_end, 0 },
   { "inTransaction", "()Z", db_in_transaction, 0 },
   { "isOpen", "()Z", db_is_open, 0 },
   { "isReadOnly", "()Z", db_is_read_only, 0 },
   { "isWriteAheadLoggingEnabled", "()Z", db_is_wal, 0 },
   { "enableWriteAheadLogging", "()Z", db_enable_wal, 0 },
   { "disableWriteAheadLogging", "()V", db_disable_wal, 0 },
   { "setForeignKeyConstraintsEnabled", "(Z)V", db_set_foreign_keys, 0 },
   { "getPath", "()Ljava/lang/String;", db_get_path, 0 },
   { "getAttachedDbs", "()Ljava/util/List;", db_attached, 0 },
   { "getVersion", "()I", db_get_version, 0 },
   { "setVersion", "(I)V", db_set_version, 0 },
   { "needUpgrade", "(I)Z", db_need_upgrade, 0 },
   { "getPageSize", "()J", db_page_size, 0 },
   { "setPageSize", "(J)V", db_set_page_size, 0 },
   { "getMaximumSize", "()J", db_maximum_size, 0 },
   { "setMaximumSize", "(J)J", db_maximum_size, 0 },
   { "setMaxSqlCacheSize", "(I)V", db_noop, 0 },
   { "setLocale", "(Ljava/util/Locale;)V", db_noop, 0 },
   { "setLockingEnabled", "(Z)V", db_noop, 0 },
   { "yieldIfContendedSafely", "()Z", db_true, 0 },
   { "execSQL", "(Ljava/lang/String;)V", db_exec_sql, 0 },
   { "execSQL", "(Ljava/lang/String;[Ljava/lang/Object;)V", db_exec_sql, 0 },
   { "rawQuery", "(Ljava/lang/String;[Ljava/lang/String;)Landroid/database/Cursor;", db_raw_query, 0 },
   { "rawQueryWithFactory",
     "(Landroid/database/sqlite/SQLiteDatabase$CursorFactory;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;)Landroid/database/Cursor;",
     db_raw_query, 0 },
   { "query",
     "(Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Landroid/database/Cursor;",
     db_query, 0 },
   { "query",
     "(Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Landroid/database/Cursor;",
     db_query, 0 },
   { "query",
     "(ZLjava/lang/String;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Landroid/database/Cursor;",
     db_query, 0 },
   { "compileStatement", "(Ljava/lang/String;)Landroid/database/sqlite/SQLiteStatement;", db_compile, 0 },
   { "delete", "(Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;)I", db_delete, 0 },
   { "insert", "(Ljava/lang/String;Ljava/lang/String;Landroid/content/ContentValues;)J", db_insert, 0 },
   { "insertOrThrow", "(Ljava/lang/String;Ljava/lang/String;Landroid/content/ContentValues;)J",
     db_insert_or_throw, 0 },
   { "insertWithOnConflict",
     "(Ljava/lang/String;Ljava/lang/String;Landroid/content/ContentValues;I)J",
     db_insert_with_conflict, 0 },
   { "replace", "(Ljava/lang/String;Ljava/lang/String;Landroid/content/ContentValues;)J", db_replace, 0 },
   { "replaceOrThrow", "(Ljava/lang/String;Ljava/lang/String;Landroid/content/ContentValues;)J",
     db_replace_or_throw, 0 },
   { "update",
     "(Ljava/lang/String;Landroid/content/ContentValues;Ljava/lang/String;[Ljava/lang/String;)I",
     db_update, 0 },
   { "updateWithOnConflict",
     "(Ljava/lang/String;Landroid/content/ContentValues;Ljava/lang/String;[Ljava/lang/String;I)I",
     db_update_with_conflict, 0 },
   { "close", "()V", db_close, 0 },
   { "openDatabase",
     "(Ljava/lang/String;Landroid/database/sqlite/SQLiteDatabase$CursorFactory;I)Landroid/database/sqlite/SQLiteDatabase;",
     db_open_database, SM },
   { "openDatabase",
     "(Ljava/lang/String;Landroid/database/sqlite/SQLiteDatabase$CursorFactory;ILandroid/database/DatabaseErrorHandler;)Landroid/database/sqlite/SQLiteDatabase;",
     db_open_database, SM },
   { "openOrCreateDatabase",
     "(Ljava/lang/String;Landroid/database/sqlite/SQLiteDatabase$CursorFactory;)Landroid/database/sqlite/SQLiteDatabase;",
     db_open_or_create, SM },
   { "openOrCreateDatabase",
     "(Ljava/io/File;Landroid/database/sqlite/SQLiteDatabase$CursorFactory;)Landroid/database/sqlite/SQLiteDatabase;",
     db_open_or_create, SM },
   { "create",
     "(Landroid/database/sqlite/SQLiteDatabase$CursorFactory;)Landroid/database/sqlite/SQLiteDatabase;",
     db_create, SM },
   { "deleteDatabase", "(Ljava/io/File;)Z", db_delete_database, SM },
   { NULL, NULL, NULL, 0 }
};

const struct dvm_sqlite_field dvm_sqlite_database_fields[] = {
   { "path", "Ljava/lang/String;" }, { NULL, NULL }
};

/* --- SQLiteProgram / SQLiteStatement ------------------------------------- */

static void binds_destroy(void *p) { binds_free(p); }

static struct sq_binds *stmt_binds(struct dvm *vm, dvm_ref self, int index)
{
   struct sq_binds *b = host_data(vm, self);
   if (index < 1) {
      dvm__throw(vm, "java/lang/IllegalArgumentException",
                 "Cannot bind argument at index %d because the index is out of range.  "
                 "The statement has no parameter at that index.", index);
      return NULL;
   }
   if (!b || b->n < index) {
      struct sq_binds *grown = binds_make(index);
      if (!grown) { dvm__throw(vm, "java/lang/OutOfMemoryError", "bind"); return NULL; }
      for (int i = 0; b && i < b->n; ++i) grown->v[i] = b->v[i];
      if (b) { free(b->v); free(b); }
      b = grown;
      host_data_set(vm, self, b, binds_destroy);
   }
   return b;
}

static bool prog_bind_null(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   (void)out;
   struct sq_binds *b = stmt_binds(vm, self, ARG(0).i);
   if (!b) return false;
   cell_clear(&b->v[ARG(0).i - 1]);
   return true;
}

static bool prog_bind_long(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   (void)out;
   struct sq_binds *b = stmt_binds(vm, self, ARG(0).i);
   if (!b) return false;
   struct sq_cell *c = &b->v[ARG(0).i - 1];
   cell_clear(c);
   c->type = SQ_INTEGER;
   c->i = ARG(1).j;
   return true;
}

static bool prog_bind_double(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   (void)out;
   struct sq_binds *b = stmt_binds(vm, self, ARG(0).i);
   if (!b) return false;
   struct sq_cell *c = &b->v[ARG(0).i - 1];
   cell_clear(c);
   c->type = SQ_FLOAT;
   c->d = ARG(1).d;
   return true;
}

static bool prog_bind_string(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   (void)out;
   if (!ARG(1).l) {
      dvm__throw(vm, "java/lang/IllegalArgumentException",
                 "the bind value at index %d is null", ARG(0).i);
      return false;
   }
   struct sq_binds *b = stmt_binds(vm, self, ARG(0).i);
   if (!b) return false;
   return cell_from_object(vm, ARG(1).l, &b->v[ARG(0).i - 1]);
}

static bool prog_bind_blob(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{ return prog_bind_string(vm, self, args, nargs, out); }

static bool prog_bind_all(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   (void)out;
   struct dvm_object *o = ARG(0).l ? dvm__obj(vm, ARG(0).l) : NULL;
   if (!o || o->kind != DVM_OBJ_ARRAY) return true;
   for (uint32_t i = 0; i < o->length; ++i) {
      union dvm_value a[2] = { { .i = (int32_t)i + 1 }, { .l = ((dvm_ref *)o->data)[i] } };
      if (!prog_bind_string(vm, self, a, 2, out)) return false;
   }
   return true;
}

static bool prog_clear(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
   (void)args; (void)nargs; (void)out;
   binds_free(host_data(vm, self));
   host_data_set(vm, self, NULL, NULL);
   return true;
}

/* A statement is compiled when it runs, with the arguments bound so far. */
static bool stmt_prepare(struct dvm *vm, dvm_ref self, struct sq_db **dbp,
                         sqlite3_stmt **stp, const char **sqlp)
{
   union dvm_value db = { 0 }, sql = { 0 };
   (void)dvm_get_field(vm, self, "database", "Landroid/database/sqlite/SQLiteDatabase;", &db);
   (void)dvm_get_field(vm, self, "sql", "Ljava/lang/String;", &sql);
   struct sq_db *d = db.l ? db_of(vm, db.l) : NULL;
   if (!d) return false;
   const char *text = sql.l ? dvm_string_utf8(vm, sql.l) : "";
   int rc = sq.prepare_v2(d->db, text, -1, stp, NULL);
   if (rc != SQ_OK) { sq_throw(vm, d->db, rc, "while compiling", text); return false; }
   if (*stp && !binds_bind(vm, d, *stp, (struct sq_binds *)host_data(vm, self), text)) {
      sq.finalize(*stp);
      return false;
   }
   *dbp = d;
   *sqlp = text;
   return true;
}

static bool stmt_execute(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   (void)args; (void)nargs; (void)out;
   struct sq_db *d; sqlite3_stmt *st; const char *sql;
   if (!stmt_prepare(vm, self, &d, &st, &sql)) return false;
   if (!st) return true;
   int rc;
   while ((rc = sq.step(st)) == SQ_ROW) {}
   sq.finalize(st);
   if (rc != SQ_DONE) { sq_throw(vm, d->db, rc, "while executing", sql); return false; }
   return true;
}

static bool stmt_execute_update(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct sq_db *d; sqlite3_stmt *st; const char *sql;
   if (!stmt_prepare(vm, self, &d, &st, &sql)) return false;
   if (!st) RETI(0);
   int rc;
   while ((rc = sq.step(st)) == SQ_ROW) {}
   sq.finalize(st);
   if (rc != SQ_DONE) { sq_throw(vm, d->db, rc, "while executing", sql); return false; }
   RETI(sq.changes(d->db));
}

static bool stmt_execute_insert(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct sq_db *d; sqlite3_stmt *st; const char *sql;
   if (!stmt_prepare(vm, self, &d, &st, &sql)) return false;
   if (!st) RETJ(-1);
   int rc;
   while ((rc = sq.step(st)) == SQ_ROW) {}
   sq.finalize(st);
   if (rc != SQ_DONE) { sq_throw(vm, d->db, rc, "while executing", sql); return false; }
   RETJ(sq.changes(d->db) > 0 ? sq.last_insert_rowid(d->db) : -1);
}

static bool stmt_first_row(struct dvm *vm, dvm_ref self, struct sq_db **dbp,
                           sqlite3_stmt **stp)
{
   const char *sql;
   if (!stmt_prepare(vm, self, dbp, stp, &sql)) return false;
   int rc = *stp ? sq.step(*stp) : SQ_DONE;
   if (rc == SQ_ROW) return true;
   if (*stp) sq.finalize(*stp);
   *stp = NULL;
   if (rc != SQ_DONE) { sq_throw(vm, (*dbp)->db, rc, "while executing", sql); return false; }
   dvm__throw(vm, "android/database/sqlite/SQLiteDoneException",
              "Query returned no rows");
   return false;
}

static bool stmt_query_long(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct sq_db *d; sqlite3_stmt *st;
   if (!stmt_first_row(vm, self, &d, &st)) return false;
   int64_t v = sq.column_int64(st, 0);
   sq.finalize(st);
   RETJ(v);
}

static bool stmt_query_string(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                              int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct sq_db *d; sqlite3_stmt *st;
   if (!stmt_first_row(vm, self, &d, &st)) return false;
   const unsigned char *t = sq.column_type(st, 0) == SQ_NULL ? NULL : sq.column_text(st, 0);
   dvm_ref s = t ? dvm_new_string(vm, (const char *)t) : 0;
   sq.finalize(st);
   RETL(s);
}

static bool stmt_close(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{ return prog_clear(vm, self, args, nargs, out); }

const struct dvm_sqlite_method dvm_sqlite_program_methods[] = {
   { "bindNull", "(I)V", prog_bind_null, 0 },
   { "bindLong", "(IJ)V", prog_bind_long, 0 },
   { "bindDouble", "(ID)V", prog_bind_double, 0 },
   { "bindString", "(ILjava/lang/String;)V", prog_bind_string, 0 },
   { "bindBlob", "(I[B)V", prog_bind_blob, 0 },
   { "bindAllArgsAsStrings", "([Ljava/lang/String;)V", prog_bind_all, 0 },
   { "clearBindings", "()V", prog_clear, 0 },
   { "close", "()V", stmt_close, 0 },
   { NULL, NULL, NULL, 0 }
};

const struct dvm_sqlite_method dvm_sqlite_statement_methods[] = {
   { "execute", "()V", stmt_execute, 0 },
   { "executeInsert", "()J", stmt_execute_insert, 0 },
   { "executeUpdateDelete", "()I", stmt_execute_update, 0 },
   { "simpleQueryForLong", "()J", stmt_query_long, 0 },
   { "simpleQueryForString", "()Ljava/lang/String;", stmt_query_string, 0 },
   { NULL, NULL, NULL, 0 }
};

const struct dvm_sqlite_field dvm_sqlite_program_fields[] = {
   { "database", "Landroid/database/sqlite/SQLiteDatabase;" },
   { "sql", "Ljava/lang/String;" }, { NULL, NULL }
};

/* Context.getDatabasePath(name): a bare name lives in the databases directory,
 * an absolute path is used as it is (WorkManager names its database by the
 * absolute path in no_backup), and no name is an in-memory database. */
void dvm_sqlite_database_path(const char *name, char *path, size_t cap)
{
   if (!name || !*name) {
      snprintf(path, cap, ":memory:");
   } else if (name[0] == '/') {
      snprintf(path, cap, "%s", name);
   } else {
      const char *base = getenv("ANDROID_DATABASES_DIR");
      snprintf(path, cap, "%s/%s", base && *base ? base : "/tmp/lunaria-databases", name);
   }
}

/* --- SQLiteOpenHelper ---------------------------------------------------- */

static bool helper_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   (void)out;
   /* (Context, name, factory, version[, errorHandler]) */
   (void)dvm_set_field(vm, self, "name", "Ljava/lang/String;", ARG(1));
   const int version = ARG(3).i;
   if (version < 1) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "Version must be >= 1, was %d", version);
      return false;
   }
   (void)dvm_set_field(vm, self, "version", "I", (union dvm_value){ .i = version });
   return true;
}

static bool helper_name(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value n = { 0 };
   (void)dvm_get_field(vm, self, "name", "Ljava/lang/String;", &n);
   RETL(n.l);
}

static bool helper_call(struct dvm *vm, dvm_ref self, const char *name,
                        const char *sig, union dvm_value *args, int nargs)
{
   struct dvm_class *c = dvm_object_class(vm, self);
   struct dvm_method *m = c ? dvm_find_method(vm, c, name, sig) : NULL;
   union dvm_value ignored = { 0 };
   return !m || dvm_call(vm, m, self, args, nargs, &ignored);
}

/* SQLiteOpenHelper.getDatabaseLocked(): open, configure, create or migrate
 * inside one transaction, then onOpen. */
static bool helper_database(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value cached = { 0 };
   (void)dvm_get_field(vm, self, "database", "Landroid/database/sqlite/SQLiteDatabase;", &cached);
   if (cached.l) {
      union dvm_value open = { 0 };
      if (db_is_open(vm, cached.l, NULL, 0, &open) && open.i) RETL(cached.l);
   }
   union dvm_value name = { 0 }, version = { 0 }, wal = { 0 };
   (void)dvm_get_field(vm, self, "name", "Ljava/lang/String;", &name);
   (void)dvm_get_field(vm, self, "version", "I", &version);
   (void)dvm_get_field(vm, self, "wal", "Z", &wal);
   const char *dbname = name.l ? dvm_string_utf8(vm, name.l) : NULL;
   char path[1024];
   dvm_sqlite_database_path(dbname, path, sizeof path);
   dvm_ref db = dvm_sqlite_open(vm, path, 0x10000000 | (wal.i ? 0x20000000 : 0));
   if (!db) return false;
   (void)dvm_set_field(vm, self, "database", "Landroid/database/sqlite/SQLiteDatabase;",
                       (union dvm_value){ .l = db });
   union dvm_value dba = { .l = db };

   if (!helper_call(vm, self, "onConfigure", "(Landroid/database/sqlite/SQLiteDatabase;)V", &dba, 1))
      goto fail;
   union dvm_value current = { 0 };
   if (!db_get_version(vm, db, NULL, 0, &current)) goto fail;
   if (current.i != version.i) {
      if (!db_begin_exclusive(vm, db, NULL, 0, out)) goto fail;
      union dvm_value v[3] = { { .l = db }, { .i = current.i }, { .i = version.i } };
      bool ok;
      if (current.i == 0) {
         ok = helper_call(vm, self, "onCreate", "(Landroid/database/sqlite/SQLiteDatabase;)V", &dba, 1);
      } else if (current.i > version.i) {
         ok = helper_call(vm, self, "onDowngrade", "(Landroid/database/sqlite/SQLiteDatabase;II)V", v, 3);
      } else {
         ok = helper_call(vm, self, "onUpgrade", "(Landroid/database/sqlite/SQLiteDatabase;II)V", v, 3);
      }
      if (ok) {
         union dvm_value nv = { .i = version.i };
         ok = db_set_version(vm, db, &nv, 1, out) && db_set_success(vm, db, NULL, 0, out);
      }
      /* The transaction ends either way; a failure keeps its exception. */
      dvm_ref pending = vm->exception;
      vm->exception = 0;
      bool ended = db_end(vm, db, NULL, 0, out);
      if (pending) { vm->exception = pending; ok = false; }
      if (!ok || !ended) goto fail;
   }
   if (!helper_call(vm, self, "onOpen", "(Landroid/database/sqlite/SQLiteDatabase;)V", &dba, 1))
      goto fail;
   RETL(db);
fail:
   {
      dvm_ref pending = vm->exception;
      vm->exception = 0;
      db_close(vm, db, NULL, 0, out);
      (void)dvm_set_field(vm, self, "database", "Landroid/database/sqlite/SQLiteDatabase;",
                          (union dvm_value){ .l = 0 });
      vm->exception = pending;
   }
   return false;
}

static bool helper_set_wal(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   (void)out;
   (void)dvm_set_field(vm, self, "wal", "Z", (union dvm_value){ .i = ARG(0).i });
   union dvm_value db = { 0 };
   (void)dvm_get_field(vm, self, "database", "Landroid/database/sqlite/SQLiteDatabase;", &db);
   union dvm_value open = { 0 };
   if (db.l && db_is_open(vm, db.l, NULL, 0, &open) && open.i)
      return ARG(0).i ? db_enable_wal(vm, db.l, NULL, 0, &open)
                      : db_disable_wal(vm, db.l, NULL, 0, &open);
   return true;
}

static bool helper_close(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value db = { 0 };
   (void)dvm_get_field(vm, self, "database", "Landroid/database/sqlite/SQLiteDatabase;", &db);
   (void)dvm_set_field(vm, self, "database", "Landroid/database/sqlite/SQLiteDatabase;",
                       (union dvm_value){ .l = 0 });
   return db.l ? db_close(vm, db.l, NULL, 0, out) : true;
}

static bool helper_downgrade(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   (void)self; (void)out;
   dvm__throw(vm, "android/database/sqlite/SQLiteException",
              "Can't downgrade database from version %d to %d", ARG(1).i, ARG(2).i);
   return false;
}

const struct dvm_sqlite_method dvm_sqlite_helper_methods[] = {
   { "<init>",
     "(Landroid/content/Context;Ljava/lang/String;Landroid/database/sqlite/SQLiteDatabase$CursorFactory;I)V",
     helper_init, 0 },
   { "<init>",
     "(Landroid/content/Context;Ljava/lang/String;Landroid/database/sqlite/SQLiteDatabase$CursorFactory;ILandroid/database/DatabaseErrorHandler;)V",
     helper_init, 0 },
   { "getDatabaseName", "()Ljava/lang/String;", helper_name, 0 },
   { "getWritableDatabase", "()Landroid/database/sqlite/SQLiteDatabase;", helper_database, 0 },
   { "getReadableDatabase", "()Landroid/database/sqlite/SQLiteDatabase;", helper_database, 0 },
   { "setWriteAheadLoggingEnabled", "(Z)V", helper_set_wal, 0 },
   { "close", "()V", helper_close, 0 },
   { "onConfigure", "(Landroid/database/sqlite/SQLiteDatabase;)V", db_noop, 0 },
   { "onOpen", "(Landroid/database/sqlite/SQLiteDatabase;)V", db_noop, 0 },
   { "onUpgrade", "(Landroid/database/sqlite/SQLiteDatabase;II)V", db_noop, 0 },
   { "onDowngrade", "(Landroid/database/sqlite/SQLiteDatabase;II)V", helper_downgrade, 0 },
   { NULL, NULL, NULL, 0 }
};

const struct dvm_sqlite_field dvm_sqlite_helper_fields[] = {
   { "name", "Ljava/lang/String;" }, { "database", "Landroid/database/sqlite/SQLiteDatabase;" },
   { "version", "I" }, { "wal", "Z" }, { NULL, NULL }
};

/* A Cursor with no columns and no rows: what a provider that has nothing to
 * say answers a query with (never null, so callers can move and close it). */
dvm_ref dvm_sqlite_empty_cursor(struct dvm *vm)
{
   struct sq_cursor *c = calloc(1, sizeof *c);
   return c ? cursor_wrap(vm, c) : 0;
}
