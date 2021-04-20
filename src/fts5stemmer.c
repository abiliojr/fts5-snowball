#include <sqlite3ext.h>
#include <libstemmer.h>
#include <string.h>
#include <assert.h>

#define MIN_TOKEN_LEN (3)
#define MAX_TOKEN_LEN (64)
#define DEFAULT_LANGUAGE "english"

SQLITE_EXTENSION_INIT1;
#if defined( _WIN32 )
#define _USE_MATH_DEFINES
#endif /* _WIN32 */

struct StemmerListItem {
	struct sb_stemmer *stemmer;
	char *language;
};

// global stemmers cache on ~db connection level.
// other variants like a simple global cache, global thread local cache, etc
// failed in a multithreaded environment with ability of garbage collector
// to close connection in a different thread (not where conn have been created)
struct GlobalSnowballContext {
	fts5_api* fts5;
	struct StemmerListItem *stemmers;
	int numberStemmers;
};

struct SnowTokenizer {
	fts5_tokenizer tokenizer;           /* Parent tokenizer module */
	Fts5Tokenizer *pTokenizer;          /* Parent tokenizer instance */
	int *stemmers;
	char aBuf[MAX_TOKEN_LEN];
	struct GlobalSnowballContext* gCtx; /* stemmers cache */
};

struct SnowContext {
	void *pCtx;                         /* internal context of fts5 */
	struct GlobalSnowballContext* gCtx; /* stemmers cache */
	int (*xToken)(void*, int, const char*, int, int, int);
	int *stemmers;
	char *aBuf;
};

static void *realloc_or_free(void *mem, int size) {
	void *new_mem = sqlite3_realloc(mem, size);
	if (new_mem == NULL) sqlite3_free(mem);
	return new_mem;
}

static void destroyStemmer(void *p) {
	sb_stemmer_delete(p);
}

static fts5_api *fts5_api_from_db(sqlite3 *db) {
	fts5_api *pRet=0;
	sqlite3_stmt *pStmt = 0;

	int version=sqlite3_libversion_number();
	if (version >=3020000) { // current api
		if( SQLITE_OK==sqlite3_prepare(db, "SELECT fts5(?1)", -1, &pStmt, 0) ) {
			sqlite3_bind_pointer(pStmt, 1, (void*)&pRet, "fts5_api_ptr", NULL);
			sqlite3_step(pStmt);
		}
		sqlite3_finalize(pStmt);
	} else { // before 3.20
		int rc = sqlite3_prepare(db, "SELECT fts5()", -1, &pStmt, 0);
		if( rc==SQLITE_OK ) {
			if(SQLITE_ROW==sqlite3_step(pStmt) && sizeof(fts5_api*)==sqlite3_column_bytes(pStmt, 0)) {
				memcpy(&pRet, sqlite3_column_blob(pStmt, 0), sizeof(fts5_api*));
			}
			sqlite3_finalize(pStmt);
		}
	}
	return pRet;
}

static void destroySnowball(void *p) {
	struct GlobalSnowballContext* ctx = (struct GlobalSnowballContext*)p;
	struct StemmerListItem* availableStemmers = ctx->stemmers;

	assert(ctx->numberStemmers == 0 || availableStemmers != NULL);

	int i;
	for (i = 0; i < ctx->numberStemmers; i++) {
		destroyStemmer(availableStemmers[i].stemmer);
		sqlite3_free(availableStemmers[i].language);
	}

	if (ctx->stemmers) sqlite3_free(ctx->stemmers);
	sqlite3_free(ctx);  // destroy global stemmers cache
}

static int isValidLanguage(char *name) {
	const char **languages;

	languages = sb_stemmer_list();
	while (*languages != NULL) {
		if (strcasecmp(*languages, name) == 0) return 1;
		languages++;
	}
	return 0;
}

static int findStemmerOrLoad(char *language, int *result, struct GlobalSnowballContext* cxt) {
	int i;
	struct sb_stemmer *newStemmer;
	char *stemmerLanguage;
	struct StemmerListItem* availableStemmers = cxt->stemmers;
	int numberAvailableStemmers = cxt->numberStemmers;

	assert(numberAvailableStemmers == 0 || availableStemmers != NULL);

	if (!isValidLanguage(language)) {
		*result = -1;
		return SQLITE_OK;
	}

	for (i = 0; i < numberAvailableStemmers; i++) {
		if (sqlite3_stricmp(availableStemmers[i].language, language) == 0) {
			*result = i;
			return SQLITE_OK;
		}
	}

	/* no stemmer was already instanciated for that language, do so now */
	newStemmer = sb_stemmer_new(language, NULL);

	if (!newStemmer) {
		return SQLITE_ERROR;
	}

	availableStemmers = realloc_or_free(availableStemmers, (numberAvailableStemmers + 1) * sizeof(struct StemmerListItem));
	stemmerLanguage = sqlite3_malloc(strlen(language) + 1);
	if (!availableStemmers || !stemmerLanguage) {
		destroyStemmer(newStemmer);
		return SQLITE_ERROR;
	}

	strcpy(stemmerLanguage, language);

	availableStemmers[numberAvailableStemmers].stemmer = newStemmer;
	availableStemmers[numberAvailableStemmers].language = stemmerLanguage;


	*result = numberAvailableStemmers;
	numberAvailableStemmers += 1;

	cxt->stemmers = availableStemmers;
	cxt->numberStemmers = numberAvailableStemmers;
	return SQLITE_OK;
}

static int processListLanguages(const char **azArg, int nArg, int **stemmers_ret, int *nextArg, struct GlobalSnowballContext* cxt) {
	int i, j;
	int *stemmer_indexes = NULL;

	for (i = 0; i < nArg; i++) {
		if (findStemmerOrLoad((char *) azArg[i], &j, cxt) == SQLITE_ERROR) return SQLITE_ERROR;
		if (j == -1) break;

		stemmer_indexes = realloc_or_free(stemmer_indexes, (i + 1) * sizeof(int));
		if (stemmer_indexes == NULL) return SQLITE_ERROR;

		stemmer_indexes[i] = j;
	}

	*nextArg = i;

	if (i == 0) {
		if (findStemmerOrLoad(DEFAULT_LANGUAGE, &j, cxt) == SQLITE_ERROR) return SQLITE_ERROR;
		stemmer_indexes = realloc_or_free(stemmer_indexes, sizeof(int));
		if (stemmer_indexes == NULL) return SQLITE_ERROR;
		stemmer_indexes[0] = j;
		i++;
	}

	stemmer_indexes = realloc_or_free(stemmer_indexes, (i + 1) * sizeof(int));
	if (stemmer_indexes == NULL) return SQLITE_ERROR;
	stemmer_indexes[i] = -1;
	*stemmers_ret = stemmer_indexes;
	return SQLITE_OK;
}

static void ftsSnowballDelete(Fts5Tokenizer *pTok) {
	if (pTok) {
		struct SnowTokenizer *p = (struct SnowTokenizer*)pTok;
		if (p->pTokenizer) {
			p->tokenizer.xDelete(p->pTokenizer);
		}
		if (p->stemmers) sqlite3_free(p->stemmers);

		sqlite3_free(p);
	}
}

static int ftsSnowballCreate(
	void *pCtx,
	const char **azArg, int nArg,
	Fts5Tokenizer **ppOut
){
	struct SnowTokenizer *result;
	struct GlobalSnowballContext* cxt = (struct GlobalSnowballContext*)pCtx;
	fts5_api *pApi = cxt->fts5;
	void *pUserdata = 0;
	int rc = SQLITE_OK;
	int nextArg;
	int *stemmers = NULL;
	const char *zBase = "unicode61";

	result = (struct SnowTokenizer*) sqlite3_malloc(sizeof(struct SnowTokenizer));

	if (result) {
		memset(result, 0, sizeof(struct SnowTokenizer));
		result->gCtx = cxt;
		rc = processListLanguages(azArg, nArg, &stemmers, &nextArg, cxt);
		result->stemmers = stemmers;
	} else {
		rc = SQLITE_ERROR;
	}

	if (rc == SQLITE_OK) {
		if (nArg > nextArg) {
			zBase = azArg[nextArg];
		}
		rc = pApi->xFindTokenizer(pApi, zBase, &pUserdata, &result->tokenizer);
	}

	if (rc == SQLITE_OK) {
		int nArg2 = (nArg > nextArg + 1 ? nArg-nextArg-1 : 0);
		const char **azArg2 = (nArg2 ? &azArg[nextArg + 1] : 0);
		rc = result->tokenizer.xCreate(pUserdata, azArg2, nArg2, &result->pTokenizer);
	}

	if (rc != SQLITE_OK) {
		ftsSnowballDelete((Fts5Tokenizer*) result);
		// ftsSnowballDelete() normally releases "stemmers", because result->stemmers = stemmers
		if (!result && stemmers != NULL) sqlite3_free(stemmers);
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
		int nBuf, originalNBuf;
		sb_symbol *stemmed = NULL;
		int *stemmers;

		aBuf = p->aBuf;
		nBuf = nToken;
		memcpy(aBuf, pToken, nBuf);
		stemmers = p->stemmers;
		originalNBuf = nBuf;
		struct StemmerListItem* availableStemmers = p->gCtx->stemmers;
		assert(availableStemmers != NULL);
		while (*stemmers != -1) {
			stemmed = (sb_symbol *) sb_stemmer_stem(availableStemmers[*stemmers].stemmer, (unsigned char*) aBuf, originalNBuf);
			nBuf = sb_stemmer_length(availableStemmers[*stemmers].stemmer);
			if (nBuf != originalNBuf) break;
			stemmers++;
		}
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
	sCtx.pCtx = pCtx; // internal fts5 context
	sCtx.gCtx = p->gCtx; // our global cache
	sCtx.stemmers = p->stemmers;
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

	if (ftsApi) {
		// stemmers cache on ~db connection level
		struct GlobalSnowballContext *cxt = sqlite3_malloc(sizeof(struct GlobalSnowballContext));
		if (cxt == NULL) {
			*error = sqlite3_mprintf("Can't create extension context");
			return SQLITE_ERROR;
		}
		cxt->fts5 = ftsApi;
		cxt->stemmers = NULL;
		cxt->numberStemmers = 0;
		ftsApi->xCreateTokenizer(ftsApi, "snowball", (void *) cxt, &tokenizer, destroySnowball);
		return SQLITE_OK;
	} else {
		*error = sqlite3_mprintf("Can't find fts5 extension");
		return SQLITE_ERROR;
	}
}

