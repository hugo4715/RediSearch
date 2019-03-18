#include "spec.h"
#include "field_spec.h"
#include "redisearch_api.h"
#include "document.h"
#include <assert.h>
#include "util/dict.h"
#include "query_node.h"
#include "search_options.h"
#include "query_internal.h"
#include "numeric_filter.h"
#include "query.h"

int RS_GetCApiVersion() {
  return REDISEARCH_CAPI_VERSION;
}

static dictType invidxDictType = {0};

static void valFreeCb(void* unused, void* p) {
  KeysDictValue* kdv = p;
  if (kdv->dtor) {
    kdv->dtor(kdv->p);
  }
  free(kdv);
}

static IndexSpec* RS_CreateIndex(const char* name, RSGetValueCallback getValue, void* getValueCtx) {
  IndexSpec* spec = NewIndexSpec(name);
  spec->flags |= Index_Temporary;  // temporary is so that we will not use threads!!

  // Initialize only once:
  if (!invidxDictType.valDestructor) {
    invidxDictType = dictTypeHeapRedisStrings;
    invidxDictType.valDestructor = valFreeCb;
  }

  spec->keysDict = dictCreate(&invidxDictType, NULL);
  spec->minPrefix = 0;
  spec->maxPrefixExpansions = -1;
  spec->getValue = getValue;
  spec->getValueCtx = getValueCtx;
  DocTable_EnableIdArray(&spec->docs);
  return spec;
}

static void RS_DropIndex(IndexSpec* sp) {
  dict* d = sp->keysDict;
  dictRelease(d);
  sp->keysDict = NULL;
  IndexSpec_FreeSync(sp);
}

static RSField* RS_CreateField(IndexSpec* sp, const char* name, unsigned types, unsigned options) {
  assert(types);
  RSField* fs = IndexSpec_CreateField(sp, name);
  int numTypes = 0;

  if (types & RSFLDTYPE_FULLTEXT) {
    numTypes++;
    int txtId = IndexSpec_CreateTextId(sp);
    if (txtId < 0) {
      return NULL;
    }
    fs->ftId = txtId;
    fs->types |= INDEXFLD_T_FULLTEXT;
  }

  if (types & RSFLDTYPE_NUMERIC) {
    numTypes++;
    fs->types |= INDEXFLD_T_NUMERIC;
  }
  if (types & RSFLDTYPE_GEO) {
    fs->types |= INDEXFLD_T_GEO;
    numTypes++;
  }
  if (types & RSFLDTYPE_TAG) {
    fs->types |= INDEXFLD_T_TAG;
    numTypes++;
  }

  if (numTypes > 1) {
    fs->options |= FieldSpec_Dynamic;
  }

  if (options & RSFLDOPT_NOINDEX) {
    fs->options |= FieldSpec_NotIndexable;
  }
  if (options & RSFLDOPT_SORTABLE) {
    fs->options |= FieldSpec_Sortable;
    fs->sortIdx = RSSortingTable_Add(sp->sortables, fs->name, fieldTypeToValueType(fs->types));
  }
  if (options & RSFLDOPT_TXTNOSTEM) {
    fs->options |= FieldSpec_NoStemming;
  }
  if (options & RSFLDOPT_TXTPHONETIC) {
    fs->options |= FieldSpec_Phonetics;
    sp->flags |= Index_HasPhonetic;
  }

  return fs;
}

static void RS_TextFieldSetWeight(IndexSpec* sp, FieldSpec* fs, double w) {
  assert(FIELD_IS(fs, INDEXFLD_T_FULLTEXT));
  fs->ftWeight = w;
}

static void RS_TagSetSeparator(FieldSpec* fs, char sep) {
  assert(FIELD_IS(fs, INDEXFLD_T_TAG));
  fs->tagSep = sep;
}

static Document* RS_CreateDocument(const void* docKey, size_t len, double score, const char* lang) {
  RedisModuleString* docKeyStr = RedisModule_CreateString(NULL, docKey, len);
  const char* language = lang ? lang : "english";
  Document* ret = rm_calloc(1, sizeof(*ret));
  Document_Init(ret, docKeyStr, score, 0, language, NULL, 0);
  ret->language = strdup(ret->language);
  return ret;
}

static int RS_DropDocument(IndexSpec* sp, const void* docKey, size_t len) {
  RedisModuleString* docId = RedisModule_CreateString(NULL, docKey, len);
  t_docId id = DocTable_GetIdR(&sp->docs, docId);
  if (id == 0) {
    RedisModule_FreeString(NULL, docId);
    return 0;
  }
  int rc = DocTable_DeleteR(&sp->docs, docId);
  if (rc) {
    sp->stats.numDocuments--;
    return 1;
  }
  return 0;
}

static void RS_DocumentAddField(Document* d, const char* fieldName, RedisModuleString* value,
                                unsigned as) {
  Document_AddField(d, fieldName, value, as);
  RedisModule_RetainString(NULL, value);
}

static void RS_DocumentAddFieldString(Document* d, const char* fieldname, const char* s, size_t n,
                                      unsigned as) {
  RedisModuleString* r = RedisModule_CreateString(NULL, s, n);
  Document_AddField(d, fieldname, r, as);
}

static void RS_DocumentAddFieldNumber(Document* d, const char* fieldname, double n, unsigned as) {
  RedisModuleString* r = RedisModule_CreateStringPrintf(NULL, "%lf", n);
  Document_AddField(d, fieldname, r, as);
}

static void RS_AddDocDone(RSAddDocumentCtx* aCtx, RedisModuleCtx* ctx, void* unused) {
}

void RS_SpecAddDocument(IndexSpec* sp, Document* d) {
  uint32_t options = 0;
  QueryError status = {0};
  RSAddDocumentCtx* aCtx = NewAddDocumentCtx(sp, d, &status);
  aCtx->donecb = RS_AddDocDone;
  RedisSearchCtx sctx = {.redisCtx = NULL, .spec = sp};
  int exists = !!DocTable_GetIdR(&sp->docs, d->docKey);
  if (exists) {
    options |= DOCUMENT_ADD_REPLACE;
  }
  options |= DOCUMENT_ADD_NOSAVE;
  aCtx->stateFlags |= ACTX_F_NOBLOCK;
  AddDocumentCtx_Submit(aCtx, &sctx, options);
  rm_free(d);
}

QueryNode* RS_CreateTokenNode(IndexSpec* sp, const char* fieldName, const char* token) {
  QueryNode* ret = NewQueryNode(QN_TOKEN);

  ret->tn = (QueryTokenNode){
      .str = (char*)strdup(token), .len = strlen(token), .expanded = 0, .flags = 0};
  if (fieldName) {
    ret->opts.fieldMask = IndexSpec_GetFieldBit(sp, fieldName, strlen(fieldName));
  }
  return ret;
}

QueryNode* RS_CreateNumericNode(IndexSpec* sp, const char* field, double max, double min,
                                int includeMax, int includeMin) {
  QueryNode* ret = NewQueryNode(QN_NUMERIC);
  ret->nn.nf = NewNumericFilter(min, max, includeMin, includeMax);
  ret->nn.nf->fieldName = strdup(field);
  ret->opts.fieldMask = IndexSpec_GetFieldBit(sp, field, strlen(field));
  return ret;
}

QueryNode* RS_CreatePrefixNode(IndexSpec* sp, const char* fieldName, const char* s) {
  QueryNode* ret = NewQueryNode(QN_PREFX);
  ret->pfx =
      (QueryPrefixNode){.str = (char*)strdup(s), .len = strlen(s), .expanded = 0, .flags = 0};
  if (fieldName) {
    ret->opts.fieldMask = IndexSpec_GetFieldBit(sp, fieldName, strlen(fieldName));
  }
  return ret;
}

QueryNode* RS_CreateLexRangeNode(IndexSpec* sp, const char* fieldName, const char* begin,
                                 const char* end) {
  QueryNode* ret = NewQueryNode(QN_LEXRANGE);
  if (begin) {
    ret->lxrng.begin = begin;
  }
  if (end) {
    ret->lxrng.end = end;
  }
  if (fieldName) {
    ret->opts.fieldMask = IndexSpec_GetFieldBit(sp, fieldName, strlen(fieldName));
  }
  return ret;
}

QueryNode* RS_CreateTagNode(IndexSpec* sp, const char* field) {
  QueryNode* ret = NewQueryNode(QN_TAG);
  ret->tag.fieldName = strdup(field);
  ret->tag.len = strlen(field);
  ret->tag.numChildren = 0;
  ret->tag.children = NULL;
  ret->opts.fieldMask = IndexSpec_GetFieldBit(sp, field, strlen(field));
  return ret;
}

void RS_TagNodeAddChild(QueryNode* qn, QueryNode* child) {
  QueryTagNode_AddChildren(qn, &child, 1);
}

QueryNode* RS_CreateIntersectNode(IndexSpec* sp, int exact) {
  QueryNode* ret = NewQueryNode(QN_PHRASE);
  ret->pn = (QueryPhraseNode){.children = NULL, .numChildren = 0, .exact = exact};
  return ret;
}

void RS_IntersectNodeAddChild(QueryNode* qn, QueryNode* child) {
  QueryPhraseNode_AddChild(qn, child);
}

void RS_IntersectNodeClearChildren(QueryNode* qn) {
  assert(qn->type == QN_PHRASE);
  qn->pn.numChildren = 0;
}

size_t RS_IntersectNodeGetNumChildren(QueryNode* qn) {
  assert(qn->type == QN_PHRASE);
  return qn->pn.numChildren;
}

QueryNode* RS_IntersectNodeGetChild(QueryNode* qn, size_t index) {
  assert(qn->type == QN_PHRASE);
  assert(index >= 0 && index < qn->pn.numChildren);
  return qn->pn.children[index];
}

QueryNode* RS_CreateUnionNode(IndexSpec* sp) {
  QueryNode* ret = NewQueryNode(QN_UNION);
  ret->un = (QueryUnionNode){.children = NULL, .numChildren = 0};
  return ret;
}

void RS_UnionNodeAddChild(QueryNode* qn, QueryNode* child) {
  assert(qn->type == QN_UNION);
  QueryUnionNode_AddChild(qn, child);
}

void RS_UnionNodeClearChildren(QueryNode* qn) {
  assert(qn->type == QN_UNION);
  qn->un.numChildren = 0;
}

size_t RS_UnionNodeGetNumChildren(QueryNode* qn) {
  assert(qn->type == QN_UNION);
  return qn->un.numChildren;
}

QueryNode* RS_UnionNodeGetChild(QueryNode* qn, size_t index) {
  assert(qn->type == QN_UNION);
  assert(index >= 0 && index < qn->un.numChildren);
  return qn->un.children[index];
}

int RS_QueryNodeGetFieldMask(QueryNode* qn) {
  return qn->opts.fieldMask;
}

IndexIterator* RS_GetResultsIterator(QueryNode* qn, IndexSpec* sp) {
  RedisSearchCtx sctx = {.redisCtx = NULL, .spec = sp};
  RSSearchOptions searchOpts = {0};
  searchOpts.fieldmask = RS_FIELDMASK_ALL;
  searchOpts.slop = -1;

  QueryAST ast = {.root = qn};

  QueryError status = {0};
  QAST_Expand(&ast, NULL, &searchOpts, &sctx, &status);

  QueryEvalCtx qectx = {
      .conc = NULL,
      .opts = &searchOpts,
      .numTokens = 0,
      .docTable = &sp->docs,
      .sctx = &sctx,
  };
  IndexIterator* ret = Query_EvalNode(&qectx, ast.root);
  QueryNode_Free(qn);
  return ret;
}

void RS_QueryNodeFree(QueryNode* qn) {
  QueryNode_Free(qn);
}

int RS_QueryNodeType(QueryNode* qn) {
  return qn->type;
}

const void* RS_ResultsIteratorNext(IndexIterator* iter, IndexSpec* sp, size_t* len) {
  RSIndexResult* e = NULL;
  while (iter->Read(iter->ctx, &e) != INDEXREAD_EOF) {
    const char* docId = DocTable_GetKey(&sp->docs, e->docId, len);
    if (docId) {
      return docId;
    }
  }
  return NULL;
}

void RS_ResultsIteratorFree(IndexIterator* iter) {
  iter->Free(iter);
}

void RS_ResultsIteratorReset(RSResultsIterator* iter) {
  iter->Rewind(iter->ctx);
}

#define REGISTER_API(name, registerApiCallback)                                \
  if (registerApiCallback("RediSearch_" #name, RS_##name) != REDISMODULE_OK) { \
    printf("could not register RediSearch_" #name "\r\n");                     \
    return REDISMODULE_ERR;                                                    \
  }

int moduleRegisterApi(const char* funcname, void* funcptr);

int RS_InitializeLibrary(RedisModuleCtx* ctx) {
  REGISTER_API(GetCApiVersion, moduleRegisterApi);
  REGISTER_API(CreateIndex, moduleRegisterApi);
  REGISTER_API(DropIndex, moduleRegisterApi);
  REGISTER_API(CreateField, moduleRegisterApi);
  REGISTER_API(TextFieldSetWeight, moduleRegisterApi);
  REGISTER_API(TagSetSeparator, moduleRegisterApi);

  REGISTER_API(CreateDocument, moduleRegisterApi);
  REGISTER_API(DropDocument, moduleRegisterApi);
  REGISTER_API(DocumentAddField, moduleRegisterApi);
  REGISTER_API(DocumentAddFieldString, moduleRegisterApi);
  REGISTER_API(DocumentAddFieldNumber, moduleRegisterApi);

  REGISTER_API(SpecAddDocument, moduleRegisterApi);

  REGISTER_API(CreateTokenNode, moduleRegisterApi);
  REGISTER_API(CreateNumericNode, moduleRegisterApi);
  REGISTER_API(CreatePrefixNode, moduleRegisterApi);
  REGISTER_API(CreateLexRangeNode, moduleRegisterApi);
  REGISTER_API(CreateTagNode, moduleRegisterApi);
  REGISTER_API(TagNodeAddChild, moduleRegisterApi);
  REGISTER_API(CreateIntersectNode, moduleRegisterApi);
  REGISTER_API(IntersectNodeAddChild, moduleRegisterApi);
  REGISTER_API(CreateUnionNode, moduleRegisterApi);
  REGISTER_API(UnionNodeAddChild, moduleRegisterApi);
  REGISTER_API(QueryNodeFree, moduleRegisterApi);
  REGISTER_API(UnionNodeClearChildren, moduleRegisterApi);
  REGISTER_API(IntersectNodeClearChildren, moduleRegisterApi);
  REGISTER_API(QueryNodeType, moduleRegisterApi);
  REGISTER_API(UnionNodeGetNumChildren, moduleRegisterApi);
  REGISTER_API(UnionNodeGetChild, moduleRegisterApi);
  REGISTER_API(IntersectNodeGetNumChildren, moduleRegisterApi);
  REGISTER_API(IntersectNodeGetChild, moduleRegisterApi);
  REGISTER_API(QueryNodeGetFieldMask, moduleRegisterApi);

  REGISTER_API(GetResultsIterator, moduleRegisterApi);
  REGISTER_API(ResultsIteratorNext, moduleRegisterApi);
  REGISTER_API(ResultsIteratorFree, moduleRegisterApi);
  REGISTER_API(ResultsIteratorReset, moduleRegisterApi);

  return REDISMODULE_OK;
}
