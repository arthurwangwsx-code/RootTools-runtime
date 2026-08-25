#include "principal_store.h"

#include <CommonCrypto/CommonDigest.h>
#include <ctype.h>
#include <errno.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static const char *principal_db_path(void) {
    const char *override = getenv("ROOTTOOLS_PRINCIPAL_DB");
    return override && override[0] ? override : "/var/mobile/Library/RootTools/principals.sqlite3";
}

static int safe_id(const char *value, size_t max) {
    size_t n = value ? strlen(value) : 0;
    if (!n || n > max) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)value[i];
        if (!(isalnum(c) || c == '.' || c == '-' || c == '_' || c == ':')) return 0;
    }
    return strstr(value, "..") == NULL;
}

static int safe_display_name(const char *value) {
    size_t n = value ? strlen(value) : 0;
    if (!n || n >= RT_PRINCIPAL_NAME_CAP) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x20 || c == 0x7f) return 0;
    }
    return 1;
}

static int allowed_kind(const char *kind) {
    return kind && (
        !strcmp(kind, "host") ||
        !strcmp(kind, "app") ||
        !strcmp(kind, "skill") ||
        !strcmp(kind, "automation")
    );
}

static int db_open(sqlite3 **out) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(
        principal_db_path(),
        &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        NULL
    );
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return 0;
    }
    sqlite3_busy_timeout(db, 1500);
    const char *schema =
        "PRAGMA journal_mode=DELETE;PRAGMA synchronous=FULL;"
        "CREATE TABLE IF NOT EXISTS principals("
        "principal_id TEXT PRIMARY KEY,kind TEXT NOT NULL,display_name TEXT NOT NULL,"
        "token_hash TEXT NOT NULL UNIQUE,state TEXT NOT NULL,created_at INTEGER NOT NULL,"
        "last_used_at INTEGER,revoked_at INTEGER);"
        "CREATE INDEX IF NOT EXISTS principals_state_idx ON principals(state,kind);";
    char *error = NULL;
    rc = sqlite3_exec(db, schema, NULL, NULL, &error);
    sqlite3_free(error);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return 0;
    }
    *out = db;
    return 1;
}

static void token_hash(const char *token, char out[65]) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(token, (CC_LONG)strlen(token), digest);
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) snprintf(out + i * 2, 3, "%02x", digest[i]);
    out[64] = 0;
}

static void make_token(char out[49]) {
    unsigned char bytes[24];
    arc4random_buf(bytes, sizeof(bytes));
    for (size_t i = 0; i < sizeof(bytes); i++) snprintf(out + i * 2, 3, "%02x", bytes[i]);
    out[48] = 0;
}

static void set_error(char *out, size_t cap, const char *message) {
    if (out && cap) snprintf(out, cap, "%s", message ? message : "principal store error");
}

int rt_principal_create(
    const char *principal_id,
    const char *kind,
    const char *display_name,
    char *token_out,
    size_t token_cap,
    char *error_out,
    size_t error_cap
) {
    if (!safe_id(principal_id, RT_PRINCIPAL_ID_CAP - 1) || !allowed_kind(kind) || !safe_display_name(display_name)) {
        set_error(error_out, error_cap, "invalid principal identity");
        return 0;
    }
    if (!token_out || token_cap < 49) {
        set_error(error_out, error_cap, "token output buffer unavailable");
        return 0;
    }
    sqlite3 *db = NULL;
    if (!db_open(&db)) {
        set_error(error_out, error_cap, "principal store unavailable");
        return 0;
    }
    char token[49] = {0}, hash[65] = {0};
    make_token(token);
    token_hash(token, hash);
    sqlite3_stmt *statement = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "INSERT INTO principals(principal_id,kind,display_name,token_hash,state,created_at) "
        "VALUES(?1,?2,?3,?4,'active',?5)",
        -1,
        &statement,
        NULL
    );
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(statement, 1, principal_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, kind, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 3, display_name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 4, hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement, 5, time(NULL));
        rc = sqlite3_step(statement);
    }
    sqlite3_finalize(statement);
    sqlite3_close(db);
    if (rc != SQLITE_DONE) {
        set_error(error_out, error_cap, rc == SQLITE_CONSTRAINT ? "principal id already exists" : "principal could not be persisted");
        return 0;
    }
    snprintf(token_out, token_cap, "%s", token);
    return 1;
}

int rt_principal_revoke(const char *principal_id, char *error_out, size_t error_cap) {
    if (!safe_id(principal_id, RT_PRINCIPAL_ID_CAP - 1)) {
        set_error(error_out, error_cap, "invalid principal id");
        return 0;
    }
    sqlite3 *db = NULL;
    if (!db_open(&db)) {
        set_error(error_out, error_cap, "principal store unavailable");
        return 0;
    }
    sqlite3_stmt *statement = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "UPDATE principals SET state='revoked',revoked_at=?1 WHERE principal_id=?2 AND state='active'",
        -1,
        &statement,
        NULL
    );
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(statement, 1, time(NULL));
        sqlite3_bind_text(statement, 2, principal_id, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(statement);
    }
    int changed = sqlite3_changes(db);
    sqlite3_finalize(statement);
    sqlite3_close(db);
    if (rc != SQLITE_DONE || changed != 1) {
        set_error(error_out, error_cap, "active principal not found");
        return 0;
    }
    return 1;
}

int rt_principal_authenticate(
    const char *token,
    char *principal_id_out,
    size_t principal_id_cap,
    char *kind_out,
    size_t kind_cap
) {
    if (!token || strlen(token) < 16 || strlen(token) >= RT_PRINCIPAL_TOKEN_CAP) return 0;
    char hash[65] = {0};
    token_hash(token, hash);
    sqlite3 *db = NULL;
    if (!db_open(&db)) return 0;
    sqlite3_stmt *statement = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "SELECT principal_id,kind FROM principals WHERE token_hash=?1 AND state='active' LIMIT 1",
        -1,
        &statement,
        NULL
    );
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(statement, 1, hash, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(statement);
    }
    int ok = 0;
    if (rc == SQLITE_ROW) {
        const char *principal_id = (const char *)sqlite3_column_text(statement, 0);
        const char *kind = (const char *)sqlite3_column_text(statement, 1);
        if (principal_id && kind && strlen(principal_id) < principal_id_cap && strlen(kind) < kind_cap) {
            snprintf(principal_id_out, principal_id_cap, "%s", principal_id);
            snprintf(kind_out, kind_cap, "%s", kind);
            ok = 1;
        }
    }
    sqlite3_finalize(statement);
    if (ok) {
        time_t now = time(NULL);
        sqlite3_stmt *touch = NULL;
        rc = sqlite3_prepare_v2(
            db,
            "UPDATE principals SET last_used_at=?1 WHERE principal_id=?2 AND (last_used_at IS NULL OR last_used_at<?3)",
            -1,
            &touch,
            NULL
        );
        if (rc == SQLITE_OK) {
            sqlite3_bind_int64(touch, 1, now);
            sqlite3_bind_text(touch, 2, principal_id_out, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(touch, 3, now - 60);
            (void)sqlite3_step(touch);
        }
        sqlite3_finalize(touch);
    }
    sqlite3_close(db);
    return ok;
}

static void json_escape(const char *input, char *out, size_t cap) {
    size_t j = 0;
    for (size_t i = 0; input && input[i] && j + 2 < cap; i++) {
        unsigned char c = (unsigned char)input[i];
        if (c == '\\' || c == '"') {
            out[j++] = '\\';
            out[j++] = (char)c;
        } else if (c == '\n' || c == '\r' || c == '\t') {
            out[j++] = ' ';
        } else if (c >= 0x20) {
            out[j++] = (char)c;
        }
    }
    out[j] = 0;
}

char *rt_principals_json(void) {
    sqlite3 *db = NULL;
    if (!db_open(&db)) return NULL;
    sqlite3_stmt *statement = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "SELECT principal_id,kind,display_name,state,created_at,last_used_at,revoked_at "
        "FROM principals ORDER BY created_at DESC,principal_id",
        -1,
        &statement,
        NULL
    );
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }
    char *out = calloc(1, 32768);
    if (!out) {
        sqlite3_finalize(statement);
        sqlite3_close(db);
        return NULL;
    }
    size_t used = 0;
    int count = 0;
    int n = snprintf(out, 32768, "{\"schemaVersion\":1,\"principals\":[");
    if (n < 0) {
        free(out); sqlite3_finalize(statement); sqlite3_close(db); return NULL;
    }
    used = (size_t)n;
    while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
        const char *principal_id = (const char *)sqlite3_column_text(statement, 0);
        const char *kind = (const char *)sqlite3_column_text(statement, 1);
        const char *display_name = (const char *)sqlite3_column_text(statement, 2);
        const char *state = (const char *)sqlite3_column_text(statement, 3);
        sqlite3_int64 created_at = sqlite3_column_int64(statement, 4);
        int last_used_null = sqlite3_column_type(statement, 5) == SQLITE_NULL;
        sqlite3_int64 last_used_at = sqlite3_column_int64(statement, 5);
        int revoked_null = sqlite3_column_type(statement, 6) == SQLITE_NULL;
        sqlite3_int64 revoked_at = sqlite3_column_int64(statement, 6);
        char eid[256] = {0}, ekind[128] = {0}, ename[512] = {0}, estate[64] = {0};
        json_escape(principal_id ? principal_id : "", eid, sizeof(eid));
        json_escape(kind ? kind : "", ekind, sizeof(ekind));
        json_escape(display_name ? display_name : "", ename, sizeof(ename));
        json_escape(state ? state : "", estate, sizeof(estate));
        char last_used_text[64] = {0}, revoked_text[64] = {0};
        if (last_used_null) snprintf(last_used_text, sizeof(last_used_text), "null");
        else snprintf(last_used_text, sizeof(last_used_text), "%lld", (long long)last_used_at);
        if (revoked_null) snprintf(revoked_text, sizeof(revoked_text), "null");
        else snprintf(revoked_text, sizeof(revoked_text), "%lld", (long long)revoked_at);
        n = snprintf(
            out + used,
            32768 - used,
            "%s{\"principalId\":\"%s\",\"kind\":\"%s\",\"displayName\":\"%s\",\"state\":\"%s\","
            "\"createdAt\":%lld,\"lastUsedAt\":%s,\"revokedAt\":%s}",
            count ? "," : "",
            eid, ekind, ename, estate,
            (long long)created_at,
            last_used_text,
            revoked_text
        );
        if (n < 0 || (size_t)n >= 32768 - used) break;
        used += (size_t)n;
        count++;
    }
    sqlite3_finalize(statement);
    sqlite3_close(db);
    n = snprintf(out + used, 32768 - used, "],\"count\":%d}", count);
    if (n < 0 || (size_t)n >= 32768 - used) {
        free(out);
        return NULL;
    }
    return out;
}
