/* gqlb.h - binary schema file (.gqlb): the flat model dumped as is. */
#ifndef GQL_GQLB_H
#define GQL_GQLB_H

#include "schema.h"

#define GQLB_MAGIC "GQLB"
#define GQLB_VERSION 1u
#define GQLB_ENDIAN 0x01020304u

enum { SEC_TYPES, SEC_FIELDS, SEC_IVALS, SEC_EVALS, SEC_TREFS, SEC_IDX, SEC_DIRS, SEC_STR, SEC_N };

typedef struct {
    char magic[4];
    uint32_t version, endian, header_size;
    uint32_t hash_lo, hash_hi;
    uint32_t query_type, mutation_type, subscription_type;
    uint32_t f_typename, f_schema, f_type;
    gql_sref desc;
    struct {
        uint32_t off, count, recsize;
    } sec[SEC_N];
    uint32_t file_size, reserved;
} gqlb_header;

/* maps and validates a .gqlb file; NULL on error (message in err) */
gql_schema *gqlb_open(const char *path, char *err, size_t errn);
/* loads from memory (copied); used by tests and fuzzers */
gql_schema *gqlb_load(const void *data, size_t n, char *err, size_t errn);
void gqlb_close(gql_schema *s);
/* serializes a schema into a buffer */
int gqlb_serialize(const gql_schema *s, gql_buf *out);

#endif
