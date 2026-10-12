/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * android.database.sqlite over the host's SQLite (see dvm_sqlite.c).  The
 * tables below have the layout of the runtime's rt_method / rt_field rows and
 * are NULL-terminated; dvm_runtime.c wires them to the framework classes.
 */

#pragma once

#include "dvm/dvm.h"
#include "dvm/dvm_internal.h"

struct dvm_sqlite_method {
   const char *name, *sig;
   dvm_builtin_fn fn;
   uint32_t access;
};

struct dvm_sqlite_field {
   const char *name, *type;
};

extern const struct dvm_sqlite_method dvm_sqlite_database_methods[];
extern const struct dvm_sqlite_field  dvm_sqlite_database_fields[];
extern const struct dvm_sqlite_method dvm_sqlite_cursor_methods[];
extern const struct dvm_sqlite_field  dvm_sqlite_cursor_fields[];
extern const struct dvm_sqlite_method dvm_sqlite_program_methods[];
extern const struct dvm_sqlite_method dvm_sqlite_statement_methods[];
extern const struct dvm_sqlite_field  dvm_sqlite_program_fields[];
extern const struct dvm_sqlite_method dvm_sqlite_helper_methods[];
extern const struct dvm_sqlite_field  dvm_sqlite_helper_fields[];

/* Opens (creating when `flags` says so) a database and returns the
 * SQLiteDatabase object, or 0 with an SQLiteException pending.  `flags` are
 * SQLiteDatabase.OPEN_* / CREATE_IF_NECESSARY / ENABLE_WRITE_AHEAD_LOGGING. */
dvm_ref dvm_sqlite_open(struct dvm *vm, const char *path, int flags);

/* Removes a database file and its -journal/-wal/-shm companions. */
bool dvm_sqlite_delete_files(const char *path);

/* A Cursor with no columns and no rows. */
dvm_ref dvm_sqlite_empty_cursor(struct dvm *vm);

/* The file a database name refers to (Context.getDatabasePath). */
void dvm_sqlite_database_path(const char *name, char *path, size_t cap);
