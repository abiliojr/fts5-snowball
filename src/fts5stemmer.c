#include <sqlite3ext.h>
#include <libstemmer.h>
#include <string.h>
#include <stdio.h>

#define MIN_TOKEN_LEN (3)
#define MAX_TOKEN_LEN (64)
#define DEFAULT_LANGUAGE "english"

SQLITE_EXTENSION_INIT1;
#if defined( _WIN32 )
#define _USE_MATH_DEFINES
#endif /* _WIN32 */

static const char **languagesList;

struct SnowTokenizer {
	fts5_tokenizer tokenizer;       /* Parent tokenizer module */
	Fts5Tokenizer *pTokenizer;      /* Parent tokenizer instance */
	struct sb_stemmer **stemmers;
	char aBuf[MAX_TOKEN_LEN];
};

struct SnowContext {
	void *pCtx;
	int (*xToken)(void*, int, const char*, int, int, int);
	struct sb_stemmer **stemmers;
	char *aBuf;
};

static void *realloc_or_free(void *mem, int size) {
	void *new_mem = sqlite3_realloc(mem, size);
	if (new_mem == NULL) sqlite3_free(mem);
	return new_mem;
}

static struct sb_stemmer *createStemmer(const char *language) {
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

static int isValidLanguage(char *name) {
	const char **languages;

	languages = languagesList;
	while (*languages != NULL) {
		if (strcasecmp(*languages, name) == 0) return 1;
		languages++;
	}
	return 0;
}

static int processListLanguages(const char **azArg, int nArg, struct sb_stemmer ***stemmers_ret, int *nextArg) {
	int i;
	struct sb_stemmer **stemmers = NULL;

	for (i = 0; i < nArg; i++) {
		if (!isValidLanguage((char *)azArg[i])) break;
		stemmers = realloc_or_free(stemmers, (i + 1) * sizeof(struct sb_stemmer *));
		if (stemmers == NULL) return SQLITE_ERROR;
		stemmers[i] = createStemmer(azArg[i]);
		if (!stemmers[i]) {
			return SQLITE_ERROR;
		}
	}

	*nextArg = i;

	if (i == 0) {
		stemmers = realloc_or_free(NULL, sizeof(struct sb_stemmer *));
		if (stemmers == NULL) return SQLITE_ERROR;
		stemmers[0] = createStemmer(DEFAULT_LANGUAGE);
		if (!stemmers[0]) return SQLITE_ERROR;
		i++;
	}

	stemmers = realloc_or_free(stemmers, (i + 1) * sizeof(struct sb_stemmer *));
	if (stemmers == NULL) return SQLITE_ERROR;
	stemmers[i] = NULL;
	*stemmers_ret = stemmers;
	return SQLITE_OK;
}

static void ftsSnowballDelete(Fts5Tokenizer *pTok) {
	void *stemmers;
	if (pTok) {
		struct SnowTokenizer *p = (struct SnowTokenizer*)pTok;
		if (p->pTokenizer) {
			p->tokenizer.xDelete(p->pTokenizer);
		}

		stemmers = p->stemmers;

		if (stemmers != NULL) {
			while (*p->stemmers) {
				destroyStemmer(*p->stemmers);
				p->stemmers++;
			}

			sqlite3_free(stemmers);
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
	fts5_api *pApi = (fts5_api*)pCtx;
	void *pUserdata = 0;
	int rc = SQLITE_OK;
	int nextArg;
	struct sb_stemmer **stemmers = NULL;
	const char *zBase = "unicode61";

	result = (struct SnowTokenizer*) sqlite3_malloc(sizeof(struct SnowTokenizer));

	if (result) {
		memset(result, 0, sizeof(struct SnowTokenizer));
		rc = processListLanguages(azArg, nArg, &stemmers, &nextArg);
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
		if (stemmers != NULL) sqlite3_free(stemmers);
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
		sb_symbol *stemmed;
		struct sb_stemmer **stemmers;

		aBuf = p->aBuf;
		nBuf = nToken;
		memcpy(aBuf, pToken, nBuf);
		stemmers = p->stemmers;

		originalNBuf = nBuf;
		while (*stemmers) {
			stemmed = (sb_symbol *) sb_stemmer_stem(*stemmers, (unsigned char*) aBuf, originalNBuf);
			nBuf = sb_stemmer_length(*stemmers);
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
	sCtx.pCtx = pCtx;
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

	languagesList = sb_stemmer_list();

	ftsApi = fts5_api_from_db(db);
	ftsApi->xCreateTokenizer(ftsApi, "snowball", (void *) ftsApi, &tokenizer, destroySnowball);

	return SQLITE_OK;
}
