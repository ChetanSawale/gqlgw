/* toml-test decoder: TOML on stdin (or in argv[1]) -> tagged JSON on stdout.
 * Exit status 1 and a message on stderr when the document is invalid. */
#include <stdio.h>
#include <stdlib.h>

#include "toml.h"

int main(int argc, char **argv)
{
    FILE *f = argc > 1 ? fopen(argv[1], "rb") : stdin;
    gql_buf in, out;
    toml_doc doc;
    char chunk[4096];
    size_t n;
    int rc;
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    buf_init(&in);
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
        buf_put(&in, chunk, n);
    if (f != stdin)
        fclose(f);
    buf_init(&out);
    if (toml_parse(&doc, in.p ? in.p : "", in.len) == 0) {
        toml_to_tagged_json(doc.root, &out);
        printf("%s\n", buf_cstr(&out));
        rc = 0;
    } else {
        fprintf(stderr, "%u:%u: %s\n", doc.err_loc.line, doc.err_loc.col, doc.err);
        rc = 1;
    }
    toml_free(&doc);
    buf_free(&in);
    buf_free(&out);
    return rc;
}
