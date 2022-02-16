/*
 * Copyright 2006 - 2018, Werner Dittmann
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Authors: Werner Dittmann <Werner.Dittmann@t-online.de>
 */

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <ctime>

#include <sqlite3.h>        // Note: set correct include path for compiler

#include "crypto/zrtpDH.h"
#include "botan_all.h"
#include "libzrtpcpp/zrtpCacheDbBackend.h"

#define SQLITE_PREPARE sqlite3_prepare_v2

#if defined(_WIN32) || defined(_WIN64)
# define snprintf _snprintf
#endif

#ifdef TRANSACTIONS
constexpr char beginTransactionSql[]  = "BEGIN TRANSACTION;";
constexpr char commitTransactionSql[] = "COMMIT;";
#endif

/*
 * The database backend uses the following definitions if it implements the localZid storage.
 */

/* The type field in zrtpIdOwn stores the following values */
static const int32_t localZidStandard         = 1; /* this local ZID is not tied to a specific account */
static const int32_t localZidWithAccount      = 2;

/* Default data for account info if none specified */
constexpr char defaultAccountString[] = "_STANDARD_";


/* *****************************************************************************
 * The SQLite master table.
 *
 * Used to check if we have valid ZRTP cache tables.
 */
constexpr char lookupTables[] = "SELECT name FROM sqlite_master WHERE type='table' AND name='zrtpIdOwn';";


/* *****************************************************************************
 * SQL statements to process the zrtpIdOwn table.
 */
constexpr char dropZrtpIdOwn[] =      "DROP TABLE zrtpIdOwn;";

/* SQLite doesn't care about the VARCHAR length. */
constexpr char createZrtpIdOwn[] = "CREATE TABLE zrtpIdOwn(localZid CHAR(18), type INTEGER, accountInfo VARCHAR(1000));";

constexpr char selectZrtpIdOwn[] = "SELECT localZid FROM zrtpIdOwn WHERE type = ?1 AND accountInfo = ?2;";
constexpr char insertZrtpIdOwn[] = "INSERT INTO zrtpIdOwn (localZid, type, accountInfo) VALUES (?1, ?2, ?3);";


/* *****************************************************************************
 * SQL statements to process the remoteId table.
 */
constexpr char dropZrtpIdRemote[] =      "DROP TABLE zrtpIdRemote;";

constexpr char createZrtpIdRemote[] =
    "CREATE TABLE zrtpIdRemote "
    "(remoteZid CHAR(16),  localZid CHAR(16), flags INTEGER,"
    "rs1 BLOB(32), rs1LastUsed TIMESTAMP, rs1TimeToLive TIMESTAMP," 
    "rs2 BLOB(32), rs2LastUsed TIMESTAMP, rs2TimeToLive TIMESTAMP,"
    "mitmKey BLOB(32), mitmLastUsed TIMESTAMP, secureSince TIMESTAMP, preshCounter INTEGER);";

constexpr char selectZrtpIdRemoteAll[] =
    "SELECT flags,"
    "rs1, strftime('%s', rs1LastUsed, 'unixepoch'), strftime('%s', rs1TimeToLive, 'unixepoch'),"
    "rs2, strftime('%s', rs2LastUsed, 'unixepoch'), strftime('%s', rs2TimeToLive, 'unixepoch'),"
    "mitmKey, strftime('%s', mitmLastUsed, 'unixepoch'), strftime('%s', secureSince, 'unixepoch'),"
    "preshCounter "
    "FROM zrtpIdRemote WHERE remoteZid=?1 AND localZid=?2;";

constexpr char insertZrtpIdRemote[] =
    "INSERT INTO zrtpIdRemote "
        "(remoteZid, localZid, flags,"
        "rs1, rs1LastUsed, rs1TimeToLive,"
        "rs2, rs2LastUsed, rs2TimeToLive,"
        "mitmKey, mitmLastUsed, secureSince, preshCounter)"
      "VALUES"
        "(?1, ?12, ?2,"
        "?3, strftime('%s', ?4, 'unixepoch'), strftime('%s', ?5, 'unixepoch'),"
        "?6, strftime('%s', ?7, 'unixepoch'), strftime('%s', ?8, 'unixepoch'),"
        "?9, strftime('%s', ?10, 'unixepoch'), strftime('%s', ?11, 'unixepoch'), ?13);";

constexpr char updateZrtpIdRemote[] =
    "UPDATE zrtpIdRemote SET "
    "flags=?2,"
    "rs1=?3, rs1LastUsed=strftime('%s', ?4, 'unixepoch'), rs1TimeToLive=strftime('%s', ?5, 'unixepoch'),"
    "rs2=?6, rs2LastUsed=strftime('%s', ?7, 'unixepoch'), rs2TimeToLive=strftime('%s', ?8, 'unixepoch'),"
    "mitmKey=?9, mitmLastUsed=strftime('%s', ?10, 'unixepoch'),"
    "secureSince=strftime('%s', ?11, 'unixepoch'), preshCounter=?13 "
    "WHERE remoteZid=?1 AND localZid=?12;";

constexpr char selectZrtpIdRemoteAllNoCondition[] =
    "SELECT flags,"
    "rs1, strftime('%s', rs1LastUsed, 'unixepoch'), strftime('%s', rs1TimeToLive, 'unixepoch'),"
    "rs2, strftime('%s', rs2LastUsed, 'unixepoch'), strftime('%s', rs2TimeToLive, 'unixepoch'),"
    "mitmKey, strftime('%s', mitmLastUsed, 'unixepoch'), strftime('%s', secureSince, 'unixepoch'),"
    "preshCounter, remoteZid "
    "FROM zrtpIdRemote ORDER BY secureSince DESC;";


/* *****************************************************************************
 * SQL statements to process the name table.
 *
 * The name tables holds free format information and binds it to the combination
 * of local, remote ZIDs and an optional account information.
 */
constexpr char dropZrtpNames[] = "DROP TABLE zrtpNames;";

constexpr char createZrtpNames[] =
    "CREATE TABLE zrtpNames "
    "(remoteZid CHAR(16), localZid CHAR(16), flags INTEGER, "
    "lastUpdate TIMESTAMP, accountInfo VARCHAR(1000), name VARCHAR(1000));";

constexpr char selectZrtpNames[] =
    "SELECT flags, strftime('%s', lastUpdate, 'unixepoch'), name "
    "FROM zrtpNames "
    "WHERE remoteZid=?1 AND localZid=?2 AND accountInfo=?3;";

constexpr char insertZrtpNames[] =
    "INSERT INTO zrtpNames "
        "(remoteZid, localZid, flags, lastUpdate, accountInfo, name)"
      "VALUES"
        "(?1, ?2, ?4, strftime('%s', ?5, 'unixepoch'), ?3, ?6);";

constexpr char updateZrtpNames[] =
    "UPDATE zrtpNames SET "
    "flags=?4,"
    "lastUpdate=strftime('%s', ?5, 'unixepoch'), name=?6 "
    "WHERE remoteZid=?1 AND localZid=?2 AND accountInfo=?3;";


/* *****************************************************************************
 * A few helping macros. 
 * These macros require some names/patterns in the methods that use these 
 * macros:
 * 
 * ERRMSG requires:
 * - a variable with name "db" is the pointer to sqlite3
 * - a char* with name "errString" points to a buffer of at least SQL_CACHE_ERR_BUFF_SIZE chars
 *
 * SQLITE_CHK requires:
 * - a cleanup label, the macro goes to that label in case of error
 * - an integer (int) variable with name "rc" that stores return codes from sqlite
 * - ERRMSG
 */
#define ERRMSG  {if (errString) snprintf(errString, (size_t)DB_CACHE_ERR_BUFF_SIZE, \
                                         "SQLite3 error: %s, line: %d, error message: %s\n", __FILE__, __LINE__, sqlite3_errmsg(db));}
#define SQLITE_CHK(func) {                                              \
        rc = (func);                                                    \
        if(rc != SQLITE_OK) {                                           \
            ERRMSG;                                                     \
            goto cleanup;                                               \
        }                                                               \
    }

static int b64Encode(const uint8_t *binData, size_t binLength, char *b64Data, size_t b64Length)
{
    (void) b64Length;
    size_t consumed = 0;
    return static_cast<int>(Botan::base64_encode(b64Data, binData, binLength, consumed, true));
}

static int b64Decode(const char *b64Data, size_t b64length, uint8_t *binData, size_t binLength)
{
    (void) binLength;
    return static_cast<int>(Botan::base64_decode(binData, b64Data, b64length));
}

#ifdef TRANSACTIONS
static int beginTransaction(sqlite3 *db, char* errString)
{
    sqlite3_stmt *stmt;
    int rc;

    SQLITE_CHK(SQLITE_PREPARE(db, beginTransactionSql, sizeof(beginTransactionSql), &stmt, NULL));

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        ERRMSG;
        return rc;
    }
    return SQLITE_OK;

 cleanup:
    sqlite3_finalize(stmt);
    return rc;
}

static int commitTransaction(sqlite3 *db, char* errString)
{
    sqlite3_stmt *stmt;
    int rc;

    SQLITE_CHK(SQLITE_PREPARE(db, commitTransactionSql, sizeof(commitTransactionSql), &stmt, NULL));

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        ERRMSG;
        return rc;
    }
    return SQLITE_OK;

 cleanup:
    sqlite3_finalize(stmt);
    return rc;
}
#endif

/**
 * Initialize remote ZID and remote name tables.
 *
 * First drop the remote ZID and remote name tables and create them again.
 * All information regarding remote peers is lost.
 */
static int initializeRemoteTables(sqlite3 *db, char* errString)
{
    sqlite3_stmt * stmt;
    int rc;

    /* First drop them, just to be on the save side
     * Ignore errors, there is nothing to drop on empty DB. If ZrtpIdOwn was
     * deleted using DB admin command then we need to drop the remote id table
     * and names also to have a clean state.
     */
    SQLITE_PREPARE(db, dropZrtpIdRemote, sizeof(dropZrtpIdRemote), &stmt, nullptr);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    SQLITE_PREPARE(db, dropZrtpNames, sizeof(dropZrtpNames), &stmt, nullptr);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    SQLITE_CHK(SQLITE_PREPARE(db, createZrtpIdRemote, sizeof(createZrtpIdRemote), &stmt, nullptr))
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        ERRMSG
        return rc;
    }
    SQLITE_CHK(SQLITE_PREPARE(db, createZrtpNames, sizeof(createZrtpNames), &stmt, nullptr))
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        ERRMSG
        return rc;
    }
    return 0;

 cleanup:
    sqlite3_finalize(stmt);
    return rc;

}
/**
 * Create ZRTP cache tables in database.
 *
 * openCache calls this function if it cannot find the table zrtpId_own. This indicates
 * that no ZRTP cache tables are available in the database.
 */
static int createTables(sqlite3 *db, char* errString)
{
    sqlite3_stmt * stmt;
    int rc;

    /* no ZRTP cache tables were found - create them, first the OwnId table */
    SQLITE_CHK(SQLITE_PREPARE(db, createZrtpIdOwn, sizeof(createZrtpIdOwn), &stmt, nullptr))

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        ERRMSG
        return rc;
    }
    return initializeRemoteTables(db, errString);

 cleanup:
    sqlite3_finalize(stmt);
    return rc;
}

static int insertRemoteZidRecord(void *vdb, const uint8_t *remoteZid, const uint8_t *localZid, 
                                 const remoteZidRecord_t *remZid, char* errString)
{
    auto *db = (sqlite3*)vdb;
    sqlite3_stmt *stmt;
    int rc;

    char b64RemoteZid[IDENTIFIER_LEN*2] = {0};
    char b64LocalZid[IDENTIFIER_LEN*2] = {0};

    /* Get B64 code for remoteZid first */
    b64Encode(remoteZid, IDENTIFIER_LEN, b64RemoteZid, IDENTIFIER_LEN*2);

    /* Get B64 code for localZid now */
    b64Encode(localZid, IDENTIFIER_LEN, b64LocalZid, IDENTIFIER_LEN*2);

    SQLITE_CHK(SQLITE_PREPARE(db, insertZrtpIdRemote, sizeof(insertZrtpIdRemote), &stmt, nullptr))

    /* For *_bind_* methods: column index starts with 1 (one), not zero */
    SQLITE_CHK(sqlite3_bind_text(stmt,   1, b64RemoteZid, strlen(b64RemoteZid), SQLITE_STATIC))
    SQLITE_CHK(sqlite3_bind_text(stmt,  12, b64LocalZid, strlen(b64LocalZid), SQLITE_STATIC))
    SQLITE_CHK(sqlite3_bind_int(stmt,    2, remZid->flags))
    SQLITE_CHK(sqlite3_bind_blob(stmt,   3, remZid->rs1, RS_LENGTH, SQLITE_STATIC))
    SQLITE_CHK(sqlite3_bind_int64(stmt,  4, remZid->rs1LastUse))
    SQLITE_CHK(sqlite3_bind_int64(stmt,  5, remZid->rs1Ttl))
    SQLITE_CHK(sqlite3_bind_blob(stmt,   6, remZid->rs2, RS_LENGTH, SQLITE_STATIC))
    SQLITE_CHK(sqlite3_bind_int64(stmt,  7, remZid->rs2LastUse))
    SQLITE_CHK(sqlite3_bind_int64(stmt,  8, remZid->rs2Ttl))
    SQLITE_CHK(sqlite3_bind_blob(stmt,   9, remZid->mitmKey, RS_LENGTH, SQLITE_STATIC))
    SQLITE_CHK(sqlite3_bind_int64(stmt, 10, remZid->mitmLastUse))
    SQLITE_CHK(sqlite3_bind_int64(stmt, 11, remZid->secureSince))
    SQLITE_CHK(sqlite3_bind_int(stmt,   13, remZid->preshCounter))

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        ERRMSG
        return rc;
    }
    return SQLITE_OK;

 cleanup:
    sqlite3_finalize(stmt);
    return rc;

}

static int updateRemoteZidRecord(void *vdb, const uint8_t *remoteZid, const uint8_t *localZid, 
                                 const remoteZidRecord_t *remZid, char* errString)
{
    auto *db = (sqlite3*)vdb;
    sqlite3_stmt *stmt;
    int rc;

    char b64RemoteZid[IDENTIFIER_LEN*2] = {0};
    char b64LocalZid[IDENTIFIER_LEN*2] = {0};

    /* Get B64 code for remoteZid first */
    b64Encode(remoteZid, IDENTIFIER_LEN, b64RemoteZid, IDENTIFIER_LEN*2);

    /* Get B64 code for localZid now */
    b64Encode(localZid, IDENTIFIER_LEN, b64LocalZid, IDENTIFIER_LEN*2);

    SQLITE_CHK(SQLITE_PREPARE(db, updateZrtpIdRemote, sizeof(updateZrtpIdRemote), &stmt, nullptr))

    /* For *_bind_* methods: column index starts with 1 (one), not zero */
    /* Select for update with the following keys */
    SQLITE_CHK(sqlite3_bind_text(stmt,   1, b64RemoteZid, strlen(b64RemoteZid), SQLITE_STATIC))
    SQLITE_CHK(sqlite3_bind_text(stmt,  12, b64LocalZid, strlen(b64LocalZid), SQLITE_STATIC))

    /* Update the following values */
    SQLITE_CHK(sqlite3_bind_int(stmt,    2, remZid->flags))
    SQLITE_CHK(sqlite3_bind_blob(stmt,   3, remZid->rs1, RS_LENGTH, SQLITE_STATIC))
    SQLITE_CHK(sqlite3_bind_int64(stmt,  4, remZid->rs1LastUse))
    SQLITE_CHK(sqlite3_bind_int64(stmt,  5, remZid->rs1Ttl))
    SQLITE_CHK(sqlite3_bind_blob(stmt,   6, remZid->rs2, RS_LENGTH, SQLITE_STATIC))
    SQLITE_CHK(sqlite3_bind_int64(stmt,  7, remZid->rs2LastUse))
    SQLITE_CHK(sqlite3_bind_int64(stmt,  8, remZid->rs2Ttl))
    SQLITE_CHK(sqlite3_bind_blob(stmt,   9, remZid->mitmKey, RS_LENGTH, SQLITE_STATIC))
    SQLITE_CHK(sqlite3_bind_int64(stmt, 10, remZid->mitmLastUse))
    SQLITE_CHK(sqlite3_bind_int64(stmt, 11, remZid->secureSince))
    SQLITE_CHK(sqlite3_bind_int(stmt,   13, remZid->preshCounter))

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        ERRMSG
        return rc;
    }
    return SQLITE_OK;

 cleanup:
    sqlite3_finalize(stmt);
    return rc;
}

static int readRemoteZidRecord(void *vdb, const uint8_t *remoteZid, const uint8_t *localZid, 
                               remoteZidRecord_t *remZid, char* errString)
{
    auto *db = (sqlite3*)vdb;
    sqlite3_stmt *stmt;
    int rc;
    int found = 0;

    char b64RemoteZid[IDENTIFIER_LEN*2] = {0};
    char b64LocalZid[IDENTIFIER_LEN*2] = {0};

    /* Get B64 code for remoteZid */
    b64Encode(remoteZid, IDENTIFIER_LEN, b64RemoteZid, IDENTIFIER_LEN*2);

    /* Get B64 code for localZid */
    b64Encode(localZid, IDENTIFIER_LEN, b64LocalZid, IDENTIFIER_LEN*2);

    SQLITE_CHK(SQLITE_PREPARE(db, selectZrtpIdRemoteAll, sizeof(selectZrtpIdRemoteAll), &stmt, nullptr))
    SQLITE_CHK(sqlite3_bind_text(stmt, 1, b64RemoteZid, strlen(b64RemoteZid), SQLITE_STATIC))
    SQLITE_CHK(sqlite3_bind_text(stmt, 2, b64LocalZid, strlen(b64LocalZid), SQLITE_STATIC))

    /* Getting data from result set: column index starts with 0 (zero), not one */
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        remZid->flags =         sqlite3_column_int(stmt,    0);
        memcpy(remZid->rs1,     sqlite3_column_blob(stmt,   1), RS_LENGTH);
        remZid->rs1LastUse =    sqlite3_column_int64(stmt,  2);
        remZid->rs1Ttl =        sqlite3_column_int64(stmt,  3);
        memcpy(remZid->rs2,     sqlite3_column_blob(stmt,   4), RS_LENGTH);
        remZid->rs2LastUse =    sqlite3_column_int64(stmt,  5);
        remZid->rs2Ttl =        sqlite3_column_int64(stmt,  6);
        memcpy(remZid->mitmKey, sqlite3_column_blob(stmt,   7), RS_LENGTH);
        remZid->mitmLastUse =   sqlite3_column_int64(stmt,  8);
        remZid->secureSince =   sqlite3_column_int64(stmt,  9);
        remZid->preshCounter =  sqlite3_column_int(stmt,   10);
        found++;
    }
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        ERRMSG
        return rc;
    }
    if (found == 0) {
        remZid->flags = 0;
    }
    else if (found > 1) {
        if (errString) 
            snprintf(errString, DB_CACHE_ERR_BUFF_SIZE, "ZRTP cache inconsistent. More than one remote ZID found: %d\n", found);
        return 1;
    }
    return SQLITE_OK;

 cleanup:
    sqlite3_finalize(stmt);
    return rc;
}


static int readLocalZid(void *vdb, uint8_t *localZid, const char *accountInfo, char *errString)
{
    auto *db = (sqlite3*)vdb;
    sqlite3_stmt *stmt;
    char *zidBase64Text;
    int rc;
    int found = 0;
    int type = localZidWithAccount;

    if (accountInfo == nullptr || !strcmp(accountInfo, defaultAccountString)) {
        accountInfo = defaultAccountString;
        type = localZidStandard;
    }

    /* Find a localZid record for this combination */
    SQLITE_CHK(SQLITE_PREPARE(db, selectZrtpIdOwn, sizeof(selectZrtpIdOwn), &stmt, nullptr))

    SQLITE_CHK(sqlite3_bind_int(stmt,  1, type))
    SQLITE_CHK(sqlite3_bind_text(stmt, 2, accountInfo, strlen(accountInfo), SQLITE_STATIC))

    /* Loop over result set and count it. However, use only the localZid of first row */
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (found == 0) {
            zidBase64Text = (char *)sqlite3_column_text(stmt, 0);
            b64Decode(zidBase64Text, strlen(zidBase64Text), localZid, IDENTIFIER_LEN);
        }
        found++;
    }
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        ERRMSG
        return rc;
    }
    /* No matching record found, create new local ZID for this combination and store in DB */
    if (found == 0) {
        char b64zid[IDENTIFIER_LEN+IDENTIFIER_LEN] = {0};
        int b64len;

        /* create a 12 byte random value, convert to base 64, insert in zrtpIdOwn table */
        randomZRTP(localZid, IDENTIFIER_LEN);
        b64len = b64Encode(localZid, IDENTIFIER_LEN, b64zid, IDENTIFIER_LEN+IDENTIFIER_LEN);

        SQLITE_CHK(SQLITE_PREPARE(db, insertZrtpIdOwn, sizeof(insertZrtpIdOwn), &stmt, nullptr))

        SQLITE_CHK(sqlite3_bind_text(stmt, 1, b64zid, b64len, SQLITE_STATIC))
        SQLITE_CHK(sqlite3_bind_int(stmt,  2, type))
        SQLITE_CHK(sqlite3_bind_text(stmt, 3, accountInfo, strlen(accountInfo), SQLITE_STATIC))

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            ERRMSG
            return rc;
        }
    }
    else if (found > 1) {
        if (errString) 
            snprintf(errString, DB_CACHE_ERR_BUFF_SIZE,
                     "ZRTP cache inconsistent. Found %d matching local ZID for account: %s\n", found, accountInfo);
        return 1;
    }
    return SQLITE_OK;

 cleanup:
    sqlite3_finalize(stmt);
    return rc;
}

/*
 * SQLite use the following table structure to manage some internal data
 *
 * CREATE TABLE sqlite_master (
 *   type TEXT,
 *   name TEXT,
 *   tbl_name TEXT,
 *   rootpage INTEGER,
 *   sql TEXT
 * );
 */

static int openCache(const char* name, void **vpdb, char *errString)
{
    sqlite3_stmt *stmt;
    int found = 0;
    auto **pdb = (sqlite3**)vpdb;
    sqlite3 *db;

    int rc = sqlite3_open_v2(name, pdb, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    db = *pdb;
    if (rc) {
        ERRMSG
        return(rc);
    }

    /* check if ZRTP cache tables are already available, look if zrtpIdOwn is available */
    SQLITE_CHK(SQLITE_PREPARE(db, lookupTables, sizeof(lookupTables), &stmt, nullptr))
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc == SQLITE_ROW) {
        found++;
    }
    else if (rc != SQLITE_DONE) {
        ERRMSG
        return rc;
    }
    /* If table zrtpOwnId not found then we have an empty cache DB */
    if (found == 0) {
        rc = createTables(db, errString);
        if (rc)
            return rc;
    }
    return SQLITE_OK;

 cleanup:
    sqlite3_finalize(stmt);
    return rc;
}

static int closeCache(void *vdb)
{

    auto *db = (sqlite3*)vdb;
    sqlite3_close(db);
    return SQLITE_OK;
}

static int clearCache(void *vdb, char *errString)
{

    auto *db = (sqlite3*)vdb;
    sqlite3_stmt * stmt;
    int rc;

    SQLITE_PREPARE(db, dropZrtpIdOwn, sizeof(dropZrtpIdOwn), &stmt, nullptr);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    rc = createTables(db, errString);
    if (rc)
        return rc;
    return SQLITE_OK;
}

static int insertZidNameRecord(void *vdb, const uint8_t *remoteZid, const uint8_t *localZid,
                               const char *accountInfo, zidNameRecord_t *zidName, char* errString)
{
    auto *db = (sqlite3*)vdb;
    sqlite3_stmt *stmt;
    int rc;
    char b64RemoteZid[IDENTIFIER_LEN*2] = {0};
    char b64LocalZid[IDENTIFIER_LEN*2] = {0};

    if (accountInfo == nullptr) {
        accountInfo = defaultAccountString;
    }

    /* Get B64 code for remoteZid */
    b64Encode(remoteZid, IDENTIFIER_LEN, b64RemoteZid, IDENTIFIER_LEN*2);

    /* Get B64 code for localZid */
    b64Encode(localZid, IDENTIFIER_LEN, b64LocalZid, IDENTIFIER_LEN*2);

    SQLITE_CHK(SQLITE_PREPARE(db, insertZrtpNames, sizeof(insertZrtpNames), &stmt, nullptr))

    /* For *_bind_* methods: column index starts with 1 (one), not zero */
    SQLITE_CHK(sqlite3_bind_text(stmt,  1, b64RemoteZid, strlen(b64RemoteZid), SQLITE_STATIC))
    SQLITE_CHK(sqlite3_bind_text(stmt,  2, b64LocalZid, strlen(b64LocalZid), SQLITE_STATIC))
    SQLITE_CHK(sqlite3_bind_text(stmt,  3, accountInfo, strlen(accountInfo), SQLITE_STATIC))
    SQLITE_CHK(sqlite3_bind_int(stmt,   4, zidName->flags))
    SQLITE_CHK(sqlite3_bind_int64(stmt, 5, (int64_t)time(nullptr)))
    if (zidName->name != nullptr) {
        SQLITE_CHK(sqlite3_bind_text(stmt,   6, zidName->name, strlen(zidName->name), SQLITE_STATIC))
    }
    else {
        SQLITE_CHK(sqlite3_bind_text(stmt,   6, "_NO_NAME_", 9, SQLITE_STATIC))
    }
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        ERRMSG
        return rc;
    }
    return SQLITE_OK;

 cleanup:
    sqlite3_finalize(stmt);
    return rc;

}


static int updateZidNameRecord(void *vdb, const uint8_t *remoteZid, const uint8_t *localZid,
                               const char *accountInfo, zidNameRecord_t *zidName, char* errString)
{
    auto *db = (sqlite3*)vdb;
    sqlite3_stmt *stmt;
    int rc;
    char b64RemoteZid[IDENTIFIER_LEN*2] = {0};
    char b64LocalZid[IDENTIFIER_LEN*2] = {0};

    if (accountInfo == nullptr) {
        accountInfo = defaultAccountString;
    }

    /* Get B64 code for remoteZid */
    b64Encode(remoteZid, IDENTIFIER_LEN, b64RemoteZid, IDENTIFIER_LEN*2);

    /* Get B64 code for localZid */
    b64Encode(localZid, IDENTIFIER_LEN, b64LocalZid, IDENTIFIER_LEN*2);

    SQLITE_CHK(SQLITE_PREPARE(db, updateZrtpNames, sizeof(updateZrtpNames), &stmt, nullptr))

    /* For *_bind_* methods: column index starts with 1 (one), not zero */
    /* Select for update with the following values */
    SQLITE_CHK(sqlite3_bind_text(stmt,   1, b64RemoteZid, strlen(b64RemoteZid), SQLITE_STATIC))
    SQLITE_CHK(sqlite3_bind_text(stmt,   2, b64LocalZid, strlen(b64LocalZid), SQLITE_STATIC))
    SQLITE_CHK(sqlite3_bind_text(stmt,   3, accountInfo, strlen(accountInfo), SQLITE_STATIC))

    /* Update the following vaulues */
    SQLITE_CHK(sqlite3_bind_int(stmt,    4, zidName->flags))
    SQLITE_CHK(sqlite3_bind_int64(stmt,  5, (int64_t)time(nullptr)))
    if (zidName->name != nullptr) {
        SQLITE_CHK(sqlite3_bind_text(stmt,   6, zidName->name, strlen(zidName->name), SQLITE_STATIC))
    }
    else {
        SQLITE_CHK(sqlite3_bind_text(stmt,   6, "_NO_NAME_", 9, SQLITE_STATIC))
    }
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        ERRMSG
        return rc;
    }
    return SQLITE_OK;

 cleanup:
    sqlite3_finalize(stmt);
    return rc;

}

static int readZidNameRecord(void *vdb, const uint8_t *remoteZid, const uint8_t *localZid,
                             const char *accountInfo, zidNameRecord_t *zidName, char* errString)
{
    auto *db = (sqlite3*)vdb;
    sqlite3_stmt *stmt;
    int rc;
    int found = 0;

    char b64RemoteZid[IDENTIFIER_LEN*2] = {0};
    char b64LocalZid[IDENTIFIER_LEN*2] = {0};

    if (accountInfo == nullptr) {
        accountInfo = defaultAccountString;
    }
    /* Get B64 code for remoteZid */
    b64Encode(remoteZid, IDENTIFIER_LEN, b64RemoteZid, IDENTIFIER_LEN*2);

    /* Get B64 code for localZid */
    b64Encode(localZid, IDENTIFIER_LEN, b64LocalZid, IDENTIFIER_LEN*2);

    SQLITE_CHK(SQLITE_PREPARE(db, selectZrtpNames, sizeof(selectZrtpNames), &stmt, nullptr))

    SQLITE_CHK(sqlite3_bind_text(stmt, 1, b64RemoteZid, strlen(b64RemoteZid), SQLITE_STATIC))
    SQLITE_CHK(sqlite3_bind_text(stmt, 2, b64LocalZid, strlen(b64LocalZid), SQLITE_STATIC))
    SQLITE_CHK(sqlite3_bind_text(stmt, 3, accountInfo, strlen(accountInfo), SQLITE_STATIC))

    /* Getting data from result set: column index starts with 0 (zero), not one */
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        zidName->flags = sqlite3_column_int(stmt,        0);
        strncpy(zidName->name, (const char*)sqlite3_column_text(stmt, 2), zidName->nameLength);
        zidName->nameLength = sqlite3_column_bytes(stmt, 2);    /* Return number of bytes in string */
        found++;
    }
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        ERRMSG
        return rc;
    }
    if (found == 0)
        zidName->flags = 0;
    else if (found > 1) {
        if (errString)
            snprintf(errString, DB_CACHE_ERR_BUFF_SIZE, "ZRTP name cache inconsistent. More than one ZID name found: %d\n", found);
        return 1;
    }
    return SQLITE_OK;

 cleanup:
    sqlite3_finalize(stmt);
    return rc;
}

static void *prepareReadAllZid(void *vdb, char *errString)
{
    auto *db = (sqlite3*)vdb;
    sqlite3_stmt *stmt;
    int rc;

    SQLITE_CHK(SQLITE_PREPARE(db, selectZrtpIdRemoteAllNoCondition, sizeof(selectZrtpIdRemoteAllNoCondition), &stmt, nullptr))
    return stmt;

  cleanup:
    sqlite3_finalize(stmt);
    return nullptr;
}

static void *readNextZidRecord(void *vdb, void *vstmt, remoteZidRecord_t *remZid, char* errString)
{
    auto *db = (sqlite3*)vdb;
    sqlite3_stmt *stmt;
    char *zidBase64Text;
    int rc;

    if (vstmt == nullptr)
        return nullptr;
    stmt = (sqlite3_stmt*)vstmt;

    /* Getting data from result set: column index starts with 0 (zero), not one */
    if ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        remZid->flags =         sqlite3_column_int(stmt,    0);
        memcpy(remZid->rs1,     sqlite3_column_blob(stmt,   1), RS_LENGTH);
        remZid->rs1LastUse =    sqlite3_column_int64(stmt,  2);
        remZid->rs1Ttl =        sqlite3_column_int64(stmt,  3);
        memcpy(remZid->rs2,     sqlite3_column_blob(stmt,   4), RS_LENGTH);
        remZid->rs2LastUse =    sqlite3_column_int64(stmt,  5);
        remZid->rs2Ttl =        sqlite3_column_int64(stmt,  6);
        memcpy(remZid->mitmKey, sqlite3_column_blob(stmt,   7), RS_LENGTH);
        remZid->mitmLastUse =   sqlite3_column_int64(stmt,  8);
        remZid->secureSince =   sqlite3_column_int64(stmt,  9);
        remZid->preshCounter =  sqlite3_column_int(stmt,   10);
        zidBase64Text = (char *)sqlite3_column_text(stmt,  11);
        b64Decode(zidBase64Text, strlen(zidBase64Text), remZid->identifier, IDENTIFIER_LEN);
        return stmt;
    }
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE)
        ERRMSG
    return nullptr;
}

static void closeStatement(void *vstmt)
{
    sqlite3_stmt *stmt;

    if (vstmt == nullptr)
        return;
    stmt = (sqlite3_stmt*)vstmt;
    sqlite3_finalize(stmt);
}

void getDbCacheOps(dbCacheOps_t *ops)
{
    ops->openCache = openCache;
    ops->closeCache = closeCache;
    ops->cleanCache = clearCache;

    ops->readLocalZid = readLocalZid;

    ops->readRemoteZidRecord =   readRemoteZidRecord;
    ops->updateRemoteZidRecord = updateRemoteZidRecord;
    ops->insertRemoteZidRecord = insertRemoteZidRecord;

    ops->readZidNameRecord =   readZidNameRecord;
    ops->updateZidNameRecord = updateZidNameRecord;
    ops->insertZidNameRecord = insertZidNameRecord;

    ops->prepareReadAllZid = prepareReadAllZid;
    ops->readNextZidRecord = readNextZidRecord;
    ops->closeStatement = closeStatement;
}
