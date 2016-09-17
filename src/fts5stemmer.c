#include <sqlite3ext.h>
#include <libstemmer.h>
#include <string.h>

#define MIN_TOKEN_LEN (3)
#define MAX_TOKEN_LEN (64)
#define DEFAULT_LANGUAGE "english"

SQLITE_EXTENSION_INIT1;
#if defined( _WIN32 )
#define _USE_MATH_DEFINES
#endif /* _WIN32 */

struct SnowTokenizer {
	fts5_tokenizer tokenizer;       /* Parent tokenizer module */
	Fts5Tokenizer *pTokenizer;      /* Parent tokenizer instance */
	struct sb_stemmer *stemmer;
	char aBuf[MAX_TOKEN_LEN];
};

struct SnowContext {
	void *pCtx;
	int (*xToken)(void*, int, const char*, int, int, int);
	struct sb_stemmer *stemmer;
	char *aBuf;
};

struct sb_stemmer *createStemmer(const char *language) {
	return sb_stemmer_new(language, NULL);
}

static void destroyStemmer(void *p) {
	sb_stemmer_delete(p);
}

static fts5_api *fts5_api_from_db(sqlite3 *db) {
	fts5_api *pRet = 0;
	sqlite3_stmt *pStmt = 0;

	if (SQLITE_OK==sqlite3_prepare(db, "SELECT fts5()", -1, &pStmt, 0) &&
		SQLITE_ROW==sqlite3_step(pStmt) &&
		sizeof(pRet)==sqlite3_column_bytes(pStmt, 0))
	{
		memcpy(&pRet, sqlite3_column_blob(pStmt, 0), sizeof(pRet));
	}
	sqlite3_finalize(pStmt);
	return pRet;
}

static void destroySnowball(void *p) {
}

static void ftsSnowballDelete(Fts5Tokenizer *pTok) {
	if (pTok) {
		struct SnowTokenizer *p = (struct SnowTokenizer*)pTok;
		if (p->pTokenizer) {
			p->tokenizer.xDelete(p->pTokenizer);
		}
		if (p->stemmer) {
			destroyStemmer(p->stemmer);
		}
		sqlite3_free(p);
	}
}

static int ftsSnowballCreate(
	void *pCtx,
	const char **azArg, int nArg,
	Fts5Tokenizer **ppOut
){
	struct SnowTokenizer *result;
	const char *language;
	fts5_api *pApi = (fts5_api*)pCtx;
	void *pUserdata = 0;
	int rc = SQLITE_OK;
	const char *zBase = "unicode61";

	if (nArg == 0) language = DEFAULT_LANGUAGE;
	if (nArg > 0) {
		language  = azArg[0];
	}

	if (nArg > 1) {
		zBase = azArg[1];
	}

	result = (struct SnowTokenizer*)sqlite3_malloc(sizeof(struct SnowTokenizer));

	if (result) {
		memset(result, 0, sizeof(struct SnowTokenizer));
		rc = pApi->xFindTokenizer(pApi, zBase, &pUserdata, &result->tokenizer);
	} else {
		rc = SQLITE_NOMEM;
	}

	if (rc == SQLITE_OK) {
		int nArg2 = (nArg > 1 ? nArg-2 : 0);
		const char **azArg2 = (nArg2 ? &azArg[2] : 0);
		rc = result->tokenizer.xCreate(pUserdata, azArg2, nArg2, &result->pTokenizer);
	}

	if (rc == SQLITE_OK) {
		result->stemmer = createStemmer(language);
		if (!result->stemmer) rc = SQLITE_ERROR;
	}

	if (rc != SQLITE_OK) {
		ftsSnowballDelete((Fts5Tokenizer*) result);
		result = 0;
	}

	*ppOut = (Fts5Tokenizer*) result;
	return rc;
}

static int fts5SnowballCb(
	void *pCtx,
	int tflags,
	const char *pToken,
	int nToken,
	int iStart,
	int iEnd
){
	struct SnowContext *p = (struct SnowContext*) pCtx;

	if (nToken > MAX_TOKEN_LEN || nToken <= MIN_TOKEN_LEN) {
		return p->xToken(p->pCtx, tflags, pToken, nToken, iStart, iEnd);
	} else {
		char *aBuf;
		int nBuf;
		sb_symbol *stemmed;

		aBuf = p->aBuf;
		nBuf = nToken;
		memcpy(aBuf, pToken, nBuf);

		stemmed = (sb_symbol *) sb_stemmer_stem(p->stemmer, (unsigned char*) aBuf, nBuf);
		nBuf = sb_stemmer_length(p->stemmer);
		return p->xToken(p->pCtx, tflags, (char *) stemmed, nBuf, iStart, iEnd);
	}
}

static int ftsSnowballTokenize(
	Fts5Tokenizer *pTokenizer,
	void *pCtx,
	int flags,
	const char *pText, int nText,
	int (*xToken)(void*, int, const char*, int nToken, int iStart, int iEnd)
){
	struct SnowTokenizer *p = (struct SnowTokenizer*)pTokenizer;
	struct SnowContext sCtx;
	sCtx.xToken = xToken;
	sCtx.pCtx = pCtx;
	sCtx.stemmer = p->stemmer;
	sCtx.aBuf = p->aBuf;
	return p->tokenizer.xTokenize(
		p->pTokenizer, (void*)&sCtx, flags, pText, nText, fts5SnowballCb
	);
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_extension_init(sqlite3 *db, char **error, const sqlite3_api_routines *api) {
	fts5_api *ftsApi;

	fts5_tokenizer tokenizer = {ftsSnowballCreate, ftsSnowballDelete, ftsSnowballTokenize};

	SQLITE_EXTENSION_INIT2(api);

	ftsApi = fts5_api_from_db(db);
	ftsApi->xCreateTokenizer(ftsApi, "snowball", (void *) ftsApi, &tokenizer, destroySnowball);

	return SQLITE_OK;
}
