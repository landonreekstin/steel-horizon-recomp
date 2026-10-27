/* Minimal JSON reader: parses a whole document into a tree of nodes. */
#ifndef JSON_H
#define JSON_H

#include <stddef.h>

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype;

typedef struct jnode {
    jtype type;
    double num;           /* J_NUM, J_BOOL (0/1) */
    char *str;            /* J_STR */
    char *key;            /* set when the node is an object member */
    struct jnode *child;  /* first element/member (J_ARR, J_OBJ) */
    struct jnode *next;   /* next sibling */
} jnode;

/* Returns NULL on a parse error and writes a message to stderr. */
jnode *json_parse(const char *text);
jnode *json_load_file(const char *path);
void json_free(jnode *n);

jnode *json_get(const jnode *obj, const char *key);          /* member or NULL */
double json_num(const jnode *obj, const char *key, double fallback);
const char *json_str(const jnode *obj, const char *key);     /* or NULL */
size_t json_len(const jnode *arr);

#endif
