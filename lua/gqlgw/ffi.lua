-- gqlgw.ffi: LuaJIT binding to libgql (src/gql.h). Keep in sync with GQL_ABI_VERSION.
local ffi = require "ffi"

local ABI_VERSION = 1

ffi.cdef [[
typedef struct { uint32_t off, len; } gql_sref;

typedef struct {
    uint32_t kind, flags, scalar;
    gql_sref name, desc, specified_by;
    uint32_t fields_first, fields_count;
    uint32_t ifaces_first, ifaces_count;
    uint32_t possible_first, possible_count;
    uint32_t evals_first, evals_count;
    uint32_t pad;
} gql_type_rec;

typedef struct {
    gql_sref name, desc, deprecation;
    uint32_t flags, owner, type;
    uint32_t args_first, args_count;
    uint32_t pad;
} gql_field_rec;

typedef struct {
    gql_sref name, desc, deprecation, defval, defjson;
    uint32_t flags, type;
} gql_ival_rec;

typedef struct {
    gql_sref name, desc, deprecation;
    uint32_t flags, owner;
} gql_eval_rec;

typedef struct { uint32_t kind, of; } gql_tref_rec;

typedef struct gql_schema gql_schema;
typedef struct gql_doc gql_doc;
typedef struct gql_plan gql_plan;

typedef struct {
    uint32_t max_bytes, max_tokens, max_depth, max_errors, max_fields, max_memory, introspection, reserved;
} gql_limits;

typedef struct {
    uint32_t key_off, key_len;
    uint32_t field;
    uint32_t kind;
    uint32_t child_first;
    uint32_t child_count;
    uint32_t args_off, args_len;
    uint32_t line, col;
} gql_plan_field;

typedef struct { uint32_t type, fields_first, fields_count, pad; } gql_plan_sel;
typedef struct { uint32_t type, sel; } gql_plan_case;

typedef struct {
    uint32_t op;
    uint32_t root_sel;
    const gql_plan_sel *sels;       uint32_t nsels;
    const gql_plan_field *fields;   uint32_t nfields;
    const gql_plan_case *cases;     uint32_t ncases;
    const char *str;                uint32_t nstr;
} gql_plan_view;

typedef struct {
    const gql_type_rec *types;   uint32_t ntypes;
    const gql_field_rec *fields; uint32_t nfields;
    const gql_ival_rec *ivals;   uint32_t nivals;
    const gql_eval_rec *evals;   uint32_t nevals;
    const gql_tref_rec *trefs;   uint32_t ntrefs;
    const uint32_t *idx;         uint32_t nidx;
    const char *str;             uint32_t nstr;
    uint32_t query_type, mutation_type;
    uint32_t f_typename, f_schema, f_type;
} gql_schema_view;

int gql_abi_version(void);
gql_schema *gql_schema_open(const char *path, char *err, size_t errn);
void gql_schema_close(gql_schema *s);
void gql_schema_hash_hex(const gql_schema *s, char out[17]);
void gql_schema_get_view(const gql_schema *s, gql_schema_view *out);

gql_doc *gql_doc_new(const gql_schema *s, const char *q, size_t n, const gql_limits *lim);
int gql_doc_ok(const gql_doc *d);
const char *gql_doc_errors(const gql_doc *d, size_t *len);
void gql_doc_free(gql_doc *d);

gql_plan *gql_plan_new(const gql_schema *s, const gql_doc *d, const char *op, size_t opn,
                       const char *vars, size_t varsn, const gql_limits *lim);
int gql_plan_ok(const gql_plan *p);
const char *gql_plan_errors(const gql_plan *p, size_t *len);
const gql_plan_view *gql_plan_get_view(const gql_plan *p);
void gql_plan_free(gql_plan *p);
]]

local _M = {
    PF_LEAF = 0, PF_OBJECT = 1, PF_ABSTRACT = 2, PF_RAW = 3,
    GK_SCALAR = 0, GK_OBJECT = 1, GK_INTERFACE = 2, GK_UNION = 3, GK_ENUM = 4, GK_INPUT = 5,
    SK_CUSTOM = 0, SK_INT = 1, SK_FLOAT = 2, SK_STRING = 3, SK_BOOLEAN = 4, SK_ID = 5,
    TR_NAMED = 0, TR_LIST = 1, TR_NONNULL = 2,
    NONE = 0xffffffff,
}

-- loads libgql; path may be nil (then the loader search path is used)
function _M.load(path)
    local lib = ffi.load(path or "gql")
    local v = lib.gql_abi_version()
    if v ~= ABI_VERSION then
        error(("libgql ABI version %d, gqlgw.ffi expects %d"):format(v, ABI_VERSION))
    end
    _M.C = lib
    return lib
end

return _M
