#include <libstemmer.h>
#include <sqlite3ext.h>
#include <string.h>

#define MIN_TOKEN_LEN (3)
#define MAX_TOKEN_LEN (64)
#define DEFAULT_LANGUAGE "english"

SQLITE_EXTENSION_INIT1;
#if defined(_WIN32)
#define _USE_MATH_DEFINES
#endif /* _WIN32 */

static const char **languagesList;

struct SnowTokenizer {
    void *pCtx;

    struct {
        fts5_tokenizer module;
        Fts5Tokenizer *instance;
    } next_tokenizer;

    struct sb_stemmer **stemmers;

    int (*xToken)(void *, int, const char *, int, int, int);
};

static fts5_api *fts5_api_from_db(sqlite3 *db) {
    fts5_api *pRet = 0;
    sqlite3_stmt *pStmt = 0;

    int version = sqlite3_libversion_number();
    if (version >= 3020000) {  // current api
        if (SQLITE_OK == sqlite3_prepare(db, "SELECT fts5(?1)", -1, &pStmt, 0)) {
            sqlite3_bind_pointer(pStmt, 1, (void *)&pRet, "fts5_api_ptr", NULL);
            sqlite3_step(pStmt);
        }
        sqlite3_finalize(pStmt);
    } else {  // before 3.20
        int rc = sqlite3_prepare(db, "SELECT fts5()", -1, &pStmt, 0);
        if (rc == SQLITE_OK) {
            if (SQLITE_ROW == sqlite3_step(pStmt) && sizeof(fts5_api *) == sqlite3_column_bytes(pStmt, 0)) {
                memcpy(&pRet, sqlite3_column_blob(pStmt, 0), sizeof(fts5_api *));
            }
            sqlite3_finalize(pStmt);
        }
    }
    return pRet;
}

static int isValidLanguage(const char *name) {
    const char **languages;

    languages = languagesList;
    while (*languages != NULL) {
        if (strcasecmp(*languages, name) == 0) return 1;
        languages++;
    }
    return 0;
}

static int processListLanguages(const char **azArg, int nArg, int *nextArg, struct SnowTokenizer *snow) {
    int i;

    // find the position of the last language in the list
    for (i = 0; i < nArg; i++) {
        if (!isValidLanguage(azArg[i])) break;
    }

    *nextArg = i;
    int languages = i;

    if (!languages) {
        azArg = (const char *[]){DEFAULT_LANGUAGE};
        languages = 1;
    }

    snow->stemmers = (struct sb_stemmer **)sqlite3_malloc((languages + 1) * sizeof(struct sb_stemmer *));
    if (!snow->stemmers) return SQLITE_NOMEM;

    snow->stemmers[languages] = NULL;  // terminate the list

    for (i = 0; i < languages; i++) {
        snow->stemmers[i] = sb_stemmer_new(azArg[i], NULL);
        if (!snow->stemmers[i]) return SQLITE_ERROR;
    }

    return SQLITE_OK;
}

static void ftsSnowballDelete(Fts5Tokenizer *pTok) {
    if (pTok) {
        struct SnowTokenizer *p = (struct SnowTokenizer *)pTok;

        if (p->stemmers) {
            for (int i = 0; p->stemmers[i] != NULL; i++) {
                sb_stemmer_delete(p->stemmers[i]);
            }

            sqlite3_free(p->stemmers);
        }

        if (p->next_tokenizer.instance) {
            p->next_tokenizer.module.xDelete(p->next_tokenizer.instance);
        }

        sqlite3_free(p);
    }
}

static int ftsSnowballCreate(void *pCtx, const char **azArg, int nArg, Fts5Tokenizer **ppOut) {
    struct SnowTokenizer *result;
    fts5_api *pApi = (fts5_api *)pCtx;
    void *pUserdata = 0;
    int rc = SQLITE_OK;
    int nextArg;
    const char *zBase = "unicode61";

    result = (struct SnowTokenizer *)sqlite3_malloc(sizeof(struct SnowTokenizer));

    if (result) {
        memset(result, 0, sizeof(struct SnowTokenizer));
        rc = processListLanguages(azArg, nArg, &nextArg, result);
    } else {
        rc = SQLITE_ERROR;
    }

    if (rc == SQLITE_OK) {
        if (nArg > nextArg) {
            zBase = azArg[nextArg];
        }
        rc = pApi->xFindTokenizer(pApi, zBase, &pUserdata, &result->next_tokenizer.module);
    }

    if (rc == SQLITE_OK) {
        int nArg2 = (nArg > nextArg + 1 ? nArg - nextArg - 1 : 0);
        const char **azArg2 = (nArg2 ? &azArg[nextArg + 1] : 0);
        rc = result->next_tokenizer.module.xCreate(pUserdata, azArg2, nArg2, &result->next_tokenizer.instance);
    }

    if (rc != SQLITE_OK) {
        ftsSnowballDelete((Fts5Tokenizer *)result);
        result = NULL;
    }

    *ppOut = (Fts5Tokenizer *)result;
    return rc;
}

static int fts5SnowballCb(void *pCtx, int tflags, const char *pToken, int nToken, int iStart, int iEnd) {
    struct SnowTokenizer *p = (struct SnowTokenizer *)pCtx;

    if (nToken > MAX_TOKEN_LEN || nToken <= MIN_TOKEN_LEN) {
        return p->xToken(p->pCtx, tflags, pToken, nToken, iStart, iEnd);
    } else {
        int nBuf = nToken;
        sb_symbol *stemmed = NULL;
        struct sb_stemmer **stemmer;
        char tokenBuf[MAX_TOKEN_LEN];

        memcpy(tokenBuf, pToken, nToken);
        stemmer = p->stemmers;

        while (*stemmer) {
            stemmed = (sb_symbol *)sb_stemmer_stem(*stemmer, (unsigned char *)tokenBuf, nBuf);
            nBuf = sb_stemmer_length(*stemmer);
            if (nBuf != nToken) break;
            stemmer++;
        }

        return p->xToken(p->pCtx, tflags, (char *)stemmed, nBuf, iStart, iEnd);
    }
}

static int ftsSnowballTokenize(Fts5Tokenizer *pTokenizer, void *pCtx, int flags, const char *pText, int nText,
                               int (*xToken)(void *, int, const char *, int nToken, int iStart, int iEnd)) {
    struct SnowTokenizer *p = (struct SnowTokenizer *)pTokenizer;
    p->xToken = xToken;
    p->pCtx = pCtx;

    return p->next_tokenizer.module.xTokenize(p->next_tokenizer.instance, (void *)p, flags, pText, nText,
                                              fts5SnowballCb);
}

#ifdef _WIN32
__declspec(dllexport)
#endif
    int sqlite3_extension_init(sqlite3 *db, char **error, const sqlite3_api_routines *api) {
    fts5_api *ftsApi;

    fts5_tokenizer tokenizer = {ftsSnowballCreate, ftsSnowballDelete, ftsSnowballTokenize};

    SQLITE_EXTENSION_INIT2(api);

    languagesList = sb_stemmer_list();

    ftsApi = fts5_api_from_db(db);

    if (ftsApi) {
        ftsApi->xCreateTokenizer(ftsApi, "snowball", (void *)ftsApi, &tokenizer, NULL);
        return SQLITE_OK;
    } else {
        *error = sqlite3_mprintf("Can't find fts5 extension");
        return SQLITE_ERROR;
    }
}
