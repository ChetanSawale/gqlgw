/* libFuzzer target: TOML parser (and tagged JSON output of accepted documents). */
#include <stddef.h>
#include <stdint.h>

#include "toml.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    toml_doc doc;
    if (toml_parse(&doc, (const char *)data, size) == 0) {
        gql_buf b;
        buf_init(&b);
        toml_to_tagged_json(doc.root, &b);
        buf_free(&b);
    }
    toml_free(&doc);
    return 0;
}
