/* bson.c */

/*    Copyright 2009, 2010 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <limits.h>
#include <assert.h>

#include "bson.h"
#include "encoding.h"
#include "myconf.h"

const int initialBufferSize = 128;

#define        MIN(a,b) (((a)<(b))?(a):(b))
#define        MAX(a,b) (((a)>(b))?(a):(b))

/* only need one of these */
static const int zero = 0;

/* Custom standard function pointers. */
void *(*bson_malloc_func)(size_t) = MYMALLOC;
void *(*bson_realloc_func)(void *, size_t) = MYREALLOC;
void ( *bson_free_func)(void *) = MYFREE;
#ifdef R_SAFETY_NET
bson_printf_func bson_printf;
#else
bson_printf_func bson_printf = printf;
#endif
bson_fprintf_func bson_fprintf = fprintf;
bson_sprintf_func bson_sprintf = sprintf;

static int _bson_errprintf(const char *, ...);
bson_printf_func bson_errprintf = _bson_errprintf;

/* ObjectId fuzz functions. */
static int ( *oid_fuzz_func)(void) = NULL;
static int ( *oid_inc_func)(void) = NULL;

static void _bson_reset(bson *b) {
    b->finished = 0;
    b->stackPos = 0;
    b->err = 0;
    b->errstr = NULL;
    b->flags = 0;
}

/* ----------------------------
   READING
   ------------------------------ */

EJDB_EXPORT bson* bson_create(void) {
    return (bson*) bson_malloc(sizeof (bson));
}

EJDB_EXPORT void bson_dispose(bson* b) {
    bson_free(b);
}

EJDB_EXPORT bson *bson_empty(bson *obj) {
    static char *data = "\005\0\0\0\0";
    bson_init_data(obj, data);
    obj->finished = 1;
    obj->err = 0;
    obj->errstr = NULL;
    obj->stackPos = 0;
    obj->flags = 0;
    return obj;
}

EJDB_EXPORT int bson_copy(bson *out, const bson *in) {
    if (!out || !in) return BSON_ERROR;
    if (!in->finished) return BSON_ERROR;
    bson_init_size(out, bson_size(in));
    memcpy(out->data, in->data, bson_size(in));
    out->finished = 1;
    return BSON_OK;
}

int bson_init_data(bson *b, char *data) {
    b->data = data;
    return BSON_OK;
}

EJDB_EXPORT int bson_init_finished_data(bson *b, char *data) {
    bson_init_data(b, data);
    _bson_reset(b);
    b->finished = 1;
    return BSON_OK;
}

EJDB_EXPORT int bson_size(const bson *b) {
    int i;
    if (!b || !b->data)
        return 0;
    bson_little_endian32(&i, b->data);
    return i;
}

EJDB_EXPORT int bson_buffer_size(const bson *b) {
    return (b->cur - b->data + 1);
}

EJDB_EXPORT const char *bson_data(const bson *b) {
    return (const char *) b->data;
}

static char hexbyte(char hex) {
    if (hex >= '0' && hex <= '9')
        return (hex - '0');
    else if (hex >= 'A' && hex <= 'F')
        return (hex - 'A' + 10);
    else if (hex >= 'a' && hex <= 'f')
        return (hex - 'a' + 10);
    else
        return 0x0;
}

EJDB_EXPORT void bson_oid_from_string(bson_oid_t *oid, const char *str) {
    int i;
    for (i = 0; i < 12; i++) {
        oid->bytes[i] = (hexbyte(str[2 * i]) << 4) | hexbyte(str[2 * i + 1]);
    }
}

EJDB_EXPORT void bson_oid_to_string(const bson_oid_t *oid, char *str) {
    static const char hex[16] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    int i;
    for (i = 0; i < 12; i++) {
        str[2 * i] = hex[(oid->bytes[i] & 0xf0) >> 4];
        str[2 * i + 1] = hex[ oid->bytes[i] & 0x0f ];
    }
    str[24] = '\0';
}

EJDB_EXPORT void bson_set_oid_fuzz(int ( *func)(void)) {
    oid_fuzz_func = func;
}

EJDB_EXPORT void bson_set_oid_inc(int ( *func)(void)) {
    oid_inc_func = func;
}

EJDB_EXPORT void bson_oid_gen(bson_oid_t *oid) {
    static int incr = 0;
    static int fuzz = 0;
    int i;
    int t = time(NULL);

    if (oid_inc_func)
        i = oid_inc_func();
    else
        i = incr++;

    if (!fuzz) {
        if (oid_fuzz_func)
            fuzz = oid_fuzz_func();
        else {
            srand(t);
            fuzz = rand();
        }
    }

    bson_big_endian32(&oid->ints[0], &t);
    oid->ints[1] = fuzz;
    bson_big_endian32(&oid->ints[2], &i);
}

EJDB_EXPORT time_t bson_oid_generated_time(bson_oid_t *oid) {
    time_t out;
    bson_big_endian32(&out, &oid->ints[0]);

    return out;
}

EJDB_EXPORT void bson_print(FILE *f, const bson *b) {
    bson_print_raw(f, b->data, 0);
}

EJDB_EXPORT void bson_print_raw(FILE *f, const char *data, int depth) {
    bson_iterator i;
    const char *key;
    int temp;
    bson_timestamp_t ts;
    char oidhex[25];
    bson scope;
    bson_iterator_from_buffer(&i, data);

    while (bson_iterator_next(&i)) {
        bson_type t = bson_iterator_type(&i);
        if (t == 0)
            break;
        key = bson_iterator_key(&i);

        for (temp = 0; temp <= depth; temp++)
            bson_fprintf(f, "\t");
        bson_fprintf(f, "%s : %d \t ", key, t);
        switch (t) {
            case BSON_DOUBLE:
                bson_fprintf(f, "%f", bson_iterator_double(&i));
                break;
            case BSON_STRING:
                bson_fprintf(f, "%s", bson_iterator_string(&i));
                break;
            case BSON_SYMBOL:
                bson_fprintf(f, "SYMBOL: %s", bson_iterator_string(&i));
                break;
            case BSON_OID:
                bson_oid_to_string(bson_iterator_oid(&i), oidhex);
                bson_fprintf(f, "%s", oidhex);
                break;
            case BSON_BOOL:
                bson_fprintf(f, "%s", bson_iterator_bool(&i) ? "true" : "false");
                break;
            case BSON_DATE:
                bson_fprintf(f, "%ld", (long int) bson_iterator_date(&i));
                break;
            case BSON_BINDATA:
                bson_fprintf(f, "BSON_BINDATA");
                break;
            case BSON_UNDEFINED:
                bson_fprintf(f, "BSON_UNDEFINED");
                break;
            case BSON_NULL:
                bson_fprintf(f, "BSON_NULL");
                break;
            case BSON_REGEX:
                bson_fprintf(f, "BSON_REGEX: %s", bson_iterator_regex(&i));
                break;
            case BSON_CODE:
                bson_fprintf(f, "BSON_CODE: %s", bson_iterator_code(&i));
                break;
            case BSON_CODEWSCOPE:
                bson_fprintf(f, "BSON_CODE_W_SCOPE: %s", bson_iterator_code(&i));
                /* bson_init( &scope ); */ /* review - stepped on by bson_iterator_code_scope? */
                bson_iterator_code_scope(&i, &scope);
                bson_fprintf(f, "\n\t SCOPE: ");
                bson_print(f, &scope);
                /* bson_destroy( &scope ); */ /* review - causes free error */
                break;
            case BSON_INT:
                bson_fprintf(f, "%d", bson_iterator_int(&i));
                break;
            case BSON_LONG:
                bson_fprintf(f, "%lld", (uint64_t) bson_iterator_long(&i));
                break;
            case BSON_TIMESTAMP:
                ts = bson_iterator_timestamp(&i);
                bson_fprintf(f, "i: %d, t: %d", ts.i, ts.t);
                break;
            case BSON_OBJECT:
            case BSON_ARRAY:
                bson_fprintf(f, "\n");
                bson_print_raw(f, bson_iterator_value(&i), depth + 1);
                break;
            default:
                bson_errprintf("can't print type : %d\n", t);
        }
        bson_fprintf(f, "\n");
    }
}

/* ----------------------------
   ITERATOR
   ------------------------------ */

EJDB_EXPORT bson_iterator* bson_iterator_create(void) {
    return (bson_iterator*) malloc(sizeof ( bson_iterator));
}

EJDB_EXPORT void bson_iterator_dispose(bson_iterator* i) {
    free(i);
}

EJDB_EXPORT void bson_iterator_init(bson_iterator *i, const bson *b) {
    i->cur = b->data + 4;
    i->first = 1;
}

EJDB_EXPORT void bson_iterator_from_buffer(bson_iterator *i, const char *buffer) {
    i->cur = buffer + 4;
    i->first = 1;
}

EJDB_EXPORT bson_type bson_find(bson_iterator *it, const bson *obj, const char *name) {
    bson_iterator_init(it, (bson *) obj);
    while (bson_iterator_next(it)) {
        if (strcmp(name, bson_iterator_key(it)) == 0)
            break;
    }
    return bson_iterator_type(it);
}

EJDB_EXPORT bson_type bson_find_from_buffer(bson_iterator *it, const char *buffer, const char *name) {
    bson_iterator_from_buffer(it, buffer);
    while (bson_iterator_next(it)) {
        if (strcmp(name, bson_iterator_key(it)) == 0)
            break;
    }
    return bson_iterator_type(it);
}

static bson_type bson_find_fieldpath_value_impl(char* pstack, int curr, const char *fpath, int fplen, bson_iterator *it) {
    int i;
    int klen = 0;
    bson_type t;
    while ((t = bson_iterator_next(it)) != BSON_EOO) {
        const char* key = bson_iterator_key(it);
        klen = strlen(key);
        if (curr + klen > fplen || curr + klen + 1 >= BSON_MAX_FPATH_LEN) {
            continue;
        }
        //PUSH
        if (curr > 0) { //add leading dot
            memset(pstack + curr, '.', 1);
            curr++;
        }
        memcpy(pstack + curr, key, klen);
        curr += klen;
        for (i = 0; i < curr && i < fplen && pstack[i] == fpath[i]; ++i);
        if (i == curr && i == fplen) { //Position matched with field path
            return t;
        }
        if (i == curr && i < fplen && (t == BSON_OBJECT || t == BSON_ARRAY)) { //Only prefix and we can go into nested objects
            bson_iterator sit;
            bson_iterator_subiterator(it, &sit);
            bson_type st = bson_find_fieldpath_value_impl(pstack, curr, fpath, fplen, &sit);
            if (st != BSON_EOO) { //Found in nested
                *it = sit;
                return st;
            }
        }
        //POP
        curr -= klen;
        if (curr > 0) {
            curr--; //remove leading dot
        }
    }
    return BSON_EOO;
}

EJDB_EXPORT bson_type bson_find_fieldpath_value(const char *fpath, bson_iterator *it) {
    return bson_find_fieldpath_value2(fpath, strlen(fpath), it);
}

EJDB_EXPORT bson_type bson_find_fieldpath_value2(const char *fpath, int fplen, bson_iterator *it) {
    if (fplen >= BSON_MAX_FPATH_LEN) {
        return BSON_EOO; //give up
    }
    char pstack[BSON_MAX_FPATH_LEN];
    return bson_find_fieldpath_value_impl(pstack, 0, fpath, fplen, it);
}

EJDB_EXPORT bson_bool_t bson_iterator_more(const bson_iterator *i) {
    return *(i->cur);
}

EJDB_EXPORT bson_type bson_iterator_next(bson_iterator *i) {
    int ds;

    if (i->first) {
        i->first = 0;
        return (bson_type) (*i->cur);
    }

    switch (bson_iterator_type(i)) {
        case BSON_EOO:
            return BSON_EOO; /* don't advance */
        case BSON_UNDEFINED:
        case BSON_NULL:
            ds = 0;
            break;
        case BSON_BOOL:
            ds = 1;
            break;
        case BSON_INT:
            ds = 4;
            break;
        case BSON_LONG:
        case BSON_DOUBLE:
        case BSON_TIMESTAMP:
        case BSON_DATE:
            ds = 8;
            break;
        case BSON_OID:
            ds = 12;
            break;
        case BSON_STRING:
        case BSON_SYMBOL:
        case BSON_CODE:
            ds = 4 + bson_iterator_int_raw(i);
            break;
        case BSON_BINDATA:
            ds = 5 + bson_iterator_int_raw(i);
            break;
        case BSON_OBJECT:
        case BSON_ARRAY:
        case BSON_CODEWSCOPE:
            ds = bson_iterator_int_raw(i);
            break;
        case BSON_DBREF:
            ds = 4 + 12 + bson_iterator_int_raw(i);
            break;
        case BSON_REGEX:
        {
            const char *s = bson_iterator_value(i);
            const char *p = s;
            p += strlen(p) + 1;
            p += strlen(p) + 1;
            ds = p - s;
            break;
        }

        default:
        {
            char msg[] = "unknown type: 000000000000";
            bson_numstr(msg + 14, (unsigned) (i->cur[0]));
            bson_fatal_msg(0, msg);
            return 0;
        }
    }

    i->cur += 1 + strlen(i->cur + 1) + 1 + ds;

    return (bson_type) (*i->cur);
}

EJDB_EXPORT bson_type bson_iterator_type(const bson_iterator *i) {
    return (bson_type) i->cur[0];
}

EJDB_EXPORT const char *bson_iterator_key(const bson_iterator *i) {
    return i->cur + 1;
}

EJDB_EXPORT const char *bson_iterator_value(const bson_iterator *i) {
    const char *t = i->cur + 1;
    t += strlen(t) + 1;
    return t;
}

/* types */

EJDB_EXPORT int bson_iterator_int_raw(const bson_iterator *i) {
    int out;
    bson_little_endian32(&out, bson_iterator_value(i));
    return out;
}

EJDB_EXPORT double bson_iterator_double_raw(const bson_iterator *i) {
    double out;
    bson_little_endian64(&out, bson_iterator_value(i));
    return out;
}

EJDB_EXPORT int64_t bson_iterator_long_raw(const bson_iterator *i) {
    int64_t out;
    bson_little_endian64(&out, bson_iterator_value(i));
    return out;
}

EJDB_EXPORT bson_bool_t bson_iterator_bool_raw(const bson_iterator *i) {
    return bson_iterator_value(i)[0];
}

EJDB_EXPORT bson_oid_t *bson_iterator_oid(const bson_iterator *i) {
    return (bson_oid_t *) bson_iterator_value(i);
}

EJDB_EXPORT int bson_iterator_int(const bson_iterator *i) {
    switch (bson_iterator_type(i)) {
        case BSON_INT:
            return bson_iterator_int_raw(i);
        case BSON_LONG:
            return bson_iterator_long_raw(i);
        case BSON_DOUBLE:
            return bson_iterator_double_raw(i);
        default:
            return 0;
    }
}

EJDB_EXPORT double bson_iterator_double(const bson_iterator *i) {
    switch (bson_iterator_type(i)) {
        case BSON_INT:
            return bson_iterator_int_raw(i);
        case BSON_LONG:
            return bson_iterator_long_raw(i);
        case BSON_DOUBLE:
            return bson_iterator_double_raw(i);
        default:
            return 0;
    }
}

EJDB_EXPORT int64_t bson_iterator_long(const bson_iterator *i) {
    switch (bson_iterator_type(i)) {
        case BSON_INT:
            return bson_iterator_int_raw(i);
        case BSON_LONG:
            return bson_iterator_long_raw(i);
        case BSON_DOUBLE:
            return bson_iterator_double_raw(i);
        default:
            return 0;
    }
}

static int64_t bson_iterator_long_ext(const bson_iterator *i) {
    switch (bson_iterator_type(i)) {
        case BSON_INT:
            return bson_iterator_int_raw(i);
        case BSON_LONG:
        case BSON_DATE:
        case BSON_TIMESTAMP:
            return bson_iterator_long_raw(i);
        case BSON_DOUBLE:
            return bson_iterator_double_raw(i);
        default:
            return 0;
    }
}

EJDB_EXPORT bson_timestamp_t bson_iterator_timestamp(const bson_iterator *i) {
    bson_timestamp_t ts;
    bson_little_endian32(&(ts.i), bson_iterator_value(i));
    bson_little_endian32(&(ts.t), bson_iterator_value(i) + 4);
    return ts;
}

EJDB_EXPORT int bson_iterator_timestamp_time(const bson_iterator *i) {
    int time;
    bson_little_endian32(&time, bson_iterator_value(i) + 4);
    return time;
}

EJDB_EXPORT int bson_iterator_timestamp_increment(const bson_iterator *i) {
    int increment;
    bson_little_endian32(&increment, bson_iterator_value(i));
    return increment;
}

EJDB_EXPORT bson_bool_t bson_iterator_bool(const bson_iterator *i) {
    switch (bson_iterator_type(i)) {
        case BSON_BOOL:
            return bson_iterator_bool_raw(i);
        case BSON_INT:
            return bson_iterator_int_raw(i) != 0;
        case BSON_LONG:
            return bson_iterator_long_raw(i) != 0;
        case BSON_DOUBLE:
            return bson_iterator_double_raw(i) != 0;
        case BSON_EOO:
        case BSON_NULL:
        case BSON_UNDEFINED:
            return 0;
        default:
            return 1;
    }
}

EJDB_EXPORT const char *bson_iterator_string(const bson_iterator *i) {
    switch (bson_iterator_type(i)) {
        case BSON_STRING:
        case BSON_SYMBOL:
            return bson_iterator_value(i) + 4;
        default:
            return "";
    }
}

int bson_iterator_string_len(const bson_iterator *i) {
    return bson_iterator_int_raw(i);
}

EJDB_EXPORT const char *bson_iterator_code(const bson_iterator *i) {
    switch (bson_iterator_type(i)) {
        case BSON_STRING:
        case BSON_CODE:
            return bson_iterator_value(i) + 4;
        case BSON_CODEWSCOPE:
            return bson_iterator_value(i) + 8;
        default:
            return NULL;
    }
}

EJDB_EXPORT void bson_iterator_code_scope(const bson_iterator *i, bson *scope) {
    if (bson_iterator_type(i) == BSON_CODEWSCOPE) {
        int code_len;
        bson_little_endian32(&code_len, bson_iterator_value(i) + 4);
        bson_init_data(scope, (void *) (bson_iterator_value(i) + 8 + code_len));
        _bson_reset(scope);
        scope->finished = 1;
    } else {
        bson_empty(scope);
    }
}

EJDB_EXPORT bson_date_t bson_iterator_date(const bson_iterator *i) {
    return bson_iterator_long_raw(i);
}

EJDB_EXPORT time_t bson_iterator_time_t(const bson_iterator *i) {
    return bson_iterator_date(i) / 1000;
}

EJDB_EXPORT int bson_iterator_bin_len(const bson_iterator *i) {
    return ( bson_iterator_bin_type(i) == BSON_BIN_BINARY_OLD)
            ? bson_iterator_int_raw(i) - 4
            : bson_iterator_int_raw(i);
}

EJDB_EXPORT char bson_iterator_bin_type(const bson_iterator *i) {
    return bson_iterator_value(i)[4];
}

EJDB_EXPORT const char *bson_iterator_bin_data(const bson_iterator *i) {
    return ( bson_iterator_bin_type(i) == BSON_BIN_BINARY_OLD)
            ? bson_iterator_value(i) + 9
            : bson_iterator_value(i) + 5;
}

EJDB_EXPORT const char *bson_iterator_regex(const bson_iterator *i) {
    return bson_iterator_value(i);
}

EJDB_EXPORT const char *bson_iterator_regex_opts(const bson_iterator *i) {
    const char *p = bson_iterator_value(i);
    return p + strlen(p) + 1;

}

EJDB_EXPORT void bson_iterator_subobject(const bson_iterator *i, bson *sub) {
    bson_init_data(sub, (char *) bson_iterator_value(i));
    _bson_reset(sub);
    sub->finished = 1;
}

EJDB_EXPORT void bson_iterator_subiterator(const bson_iterator *i, bson_iterator *sub) {
    bson_iterator_from_buffer(sub, bson_iterator_value(i));
}

/* ----------------------------
   BUILDING
   ------------------------------ */

static void _bson_init_size(bson *b, int size) {
    if (size == 0)
        b->data = NULL;
    else
        b->data = (char *) bson_malloc(size);
    b->dataSize = size;
    b->cur = b->data + 4;
    _bson_reset(b);
}

EJDB_EXPORT void bson_init(bson *b) {
    _bson_init_size(b, initialBufferSize);
}

EJDB_EXPORT void bson_init_as_query(bson *b) {
    bson_init(b);
    b->flags |= BSON_FLAG_QUERY_MODE;
}

void bson_init_size(bson *b, int size) {
    _bson_init_size(b, size);
}

void bson_append_byte(bson *b, char c) {
    b->cur[0] = c;
    b->cur++;
}

EJDB_EXPORT void bson_append(bson *b, const void *data, int len) {
    memcpy(b->cur, data, len);
    b->cur += len;
}

void bson_append32(bson *b, const void *data) {
    bson_little_endian32(b->cur, data);
    b->cur += 4;
}

void bson_append64(bson *b, const void *data) {
    bson_little_endian64(b->cur, data);
    b->cur += 8;
}

int bson_ensure_space(bson *b, const int bytesNeeded) {
    int pos = b->cur - b->data;
    char *orig = b->data;
    int new_size;

    if (pos + bytesNeeded <= b->dataSize)
        return BSON_OK;

    new_size = 1.5 * (b->dataSize + bytesNeeded);

    if (new_size < b->dataSize) {
        if ((b->dataSize + bytesNeeded) < INT_MAX)
            new_size = INT_MAX;
        else {
            b->err = BSON_SIZE_OVERFLOW;
            return BSON_ERROR;
        }
    }

    b->data = bson_realloc(b->data, new_size);
    if (!b->data)
        bson_fatal_msg(!!b->data, "realloc() failed");

    b->dataSize = new_size;
    b->cur += b->data - orig;

    return BSON_OK;
}

EJDB_EXPORT int bson_finish(bson *b) {
    int i;

    if (b->err & BSON_NOT_UTF8)
        return BSON_ERROR;

    if (!b->finished) {
        if (bson_ensure_space(b, 1) == BSON_ERROR) return BSON_ERROR;
        bson_append_byte(b, 0);
        i = b->cur - b->data;
        bson_little_endian32(b->data, &i);
        b->finished = 1;
    }

    return BSON_OK;
}

EJDB_EXPORT void bson_destroy(bson *b) {
    if (b) {
        bson_free(b->data);
        b->err = 0;
        b->data = 0;
        b->cur = 0;
        b->finished = 1;
        if (b->errstr) {
            bson_free_func(b->errstr);
            b->errstr = NULL;
        }
    }
}

EJDB_EXPORT void bson_del(bson *b) {
    if (b) {
        bson_destroy(b);
        bson_free(b);
    }
}

static int bson_append_estart(bson *b, int type, const char *name, const int dataSize) {
    const int len = strlen(name) + 1;

    if (b->finished) {
        b->err |= BSON_ALREADY_FINISHED;
        return BSON_ERROR;
    }

    if (bson_ensure_space(b, 1 + len + dataSize) == BSON_ERROR) {
        return BSON_ERROR;
    }

    if (bson_check_field_name(b, (const char *) name, len - 1,
            !(b->flags & BSON_FLAG_QUERY_MODE), !(b->flags & BSON_FLAG_QUERY_MODE)) == BSON_ERROR) {
        bson_builder_error(b);
        return BSON_ERROR;
    }

    bson_append_byte(b, (char) type);
    bson_append(b, name, len);
    return BSON_OK;
}

/* ----------------------------
   BUILDING TYPES
   ------------------------------ */

EJDB_EXPORT int bson_append_int(bson *b, const char *name, const int i) {
    if (bson_append_estart(b, BSON_INT, name, 4) == BSON_ERROR)
        return BSON_ERROR;
    bson_append32(b, &i);
    return BSON_OK;
}

EJDB_EXPORT int bson_append_long(bson *b, const char *name, const int64_t i) {
    if (bson_append_estart(b, BSON_LONG, name, 8) == BSON_ERROR)
        return BSON_ERROR;
    bson_append64(b, &i);
    return BSON_OK;
}

EJDB_EXPORT int bson_append_double(bson *b, const char *name, const double d) {
    if (bson_append_estart(b, BSON_DOUBLE, name, 8) == BSON_ERROR)
        return BSON_ERROR;
    bson_append64(b, &d);
    return BSON_OK;
}

EJDB_EXPORT int bson_append_bool(bson *b, const char *name, const bson_bool_t i) {
    if (bson_append_estart(b, BSON_BOOL, name, 1) == BSON_ERROR)
        return BSON_ERROR;
    bson_append_byte(b, i != 0);
    return BSON_OK;
}

EJDB_EXPORT int bson_append_null(bson *b, const char *name) {
    if (bson_append_estart(b, BSON_NULL, name, 0) == BSON_ERROR)
        return BSON_ERROR;
    return BSON_OK;
}

EJDB_EXPORT int bson_append_undefined(bson *b, const char *name) {
    if (bson_append_estart(b, BSON_UNDEFINED, name, 0) == BSON_ERROR)
        return BSON_ERROR;
    return BSON_OK;
}

int bson_append_string_base(bson *b, const char *name,
        const char *value, int len, bson_type type) {

    int sl = len + 1;
    if (bson_check_string(b, (const char *) value, sl - 1) == BSON_ERROR)
        return BSON_ERROR;
    if (bson_append_estart(b, type, name, 4 + sl) == BSON_ERROR) {
        return BSON_ERROR;
    }
    bson_append32(b, &sl);
    bson_append(b, value, sl - 1);
    bson_append(b, "\0", 1);
    return BSON_OK;
}

EJDB_EXPORT int bson_append_string(bson *b, const char *name, const char *value) {
    return bson_append_string_base(b, name, value, strlen(value), BSON_STRING);
}

EJDB_EXPORT int bson_append_symbol(bson *b, const char *name, const char *value) {
    return bson_append_string_base(b, name, value, strlen(value), BSON_SYMBOL);
}

EJDB_EXPORT int bson_append_code(bson *b, const char *name, const char *value) {
    return bson_append_string_base(b, name, value, strlen(value), BSON_CODE);
}

EJDB_EXPORT int bson_append_string_n(bson *b, const char *name, const char *value, int len) {
    return bson_append_string_base(b, name, value, len, BSON_STRING);
}

EJDB_EXPORT int bson_append_symbol_n(bson *b, const char *name, const char *value, int len) {
    return bson_append_string_base(b, name, value, len, BSON_SYMBOL);
}

EJDB_EXPORT int bson_append_code_n(bson *b, const char *name, const char *value, int len) {
    return bson_append_string_base(b, name, value, len, BSON_CODE);
}

EJDB_EXPORT int bson_append_code_w_scope_n(bson *b, const char *name,
        const char *code, int len, const bson *scope) {

    int sl, size;
    if (!scope) return BSON_ERROR;
    sl = len + 1;
    size = 4 + 4 + sl + bson_size(scope);
    if (bson_append_estart(b, BSON_CODEWSCOPE, name, size) == BSON_ERROR)
        return BSON_ERROR;
    bson_append32(b, &size);
    bson_append32(b, &sl);
    bson_append(b, code, sl);
    bson_append(b, scope->data, bson_size(scope));
    return BSON_OK;
}

EJDB_EXPORT int bson_append_code_w_scope(bson *b, const char *name, const char *code, const bson *scope) {
    return bson_append_code_w_scope_n(b, name, code, strlen(code), scope);
}

EJDB_EXPORT int bson_append_binary(bson *b, const char *name, char type, const char *str, int len) {
    if (type == BSON_BIN_BINARY_OLD) {
        int subtwolen = len + 4;
        if (bson_append_estart(b, BSON_BINDATA, name, 4 + 1 + 4 + len) == BSON_ERROR)
            return BSON_ERROR;
        bson_append32(b, &subtwolen);
        bson_append_byte(b, type);
        bson_append32(b, &len);
        bson_append(b, str, len);
    } else {
        if (bson_append_estart(b, BSON_BINDATA, name, 4 + 1 + len) == BSON_ERROR)
            return BSON_ERROR;
        bson_append32(b, &len);
        bson_append_byte(b, type);
        bson_append(b, str, len);
    }
    return BSON_OK;
}

EJDB_EXPORT int bson_append_oid(bson *b, const char *name, const bson_oid_t *oid) {
    if (bson_append_estart(b, BSON_OID, name, 12) == BSON_ERROR)
        return BSON_ERROR;
    bson_append(b, oid, 12);
    return BSON_OK;
}

EJDB_EXPORT int bson_append_new_oid(bson *b, const char *name) {
    bson_oid_t oid;
    bson_oid_gen(&oid);
    return bson_append_oid(b, name, &oid);
}

EJDB_EXPORT int bson_append_regex(bson *b, const char *name, const char *pattern, const char *opts) {
    const int plen = strlen(pattern) + 1;
    const int olen = strlen(opts) + 1;
    if (bson_append_estart(b, BSON_REGEX, name, plen + olen) == BSON_ERROR)
        return BSON_ERROR;
    if (bson_check_string(b, pattern, plen - 1) == BSON_ERROR)
        return BSON_ERROR;
    bson_append(b, pattern, plen);
    bson_append(b, opts, olen);
    return BSON_OK;
}

EJDB_EXPORT int bson_append_bson(bson *b, const char *name, const bson *bson) {
    if (!bson) return BSON_ERROR;
    if (bson_append_estart(b, BSON_OBJECT, name, bson_size(bson)) == BSON_ERROR)
        return BSON_ERROR;
    bson_append(b, bson->data, bson_size(bson));
    return BSON_OK;
}

EJDB_EXPORT int bson_append_element(bson *b, const char *name_or_null, const bson_iterator *elem) {
    bson_iterator next = *elem;
    int size;

    bson_iterator_next(&next);
    size = next.cur - elem->cur;

    if (name_or_null == NULL) {
        if (bson_ensure_space(b, size) == BSON_ERROR)
            return BSON_ERROR;
        bson_append(b, elem->cur, size);
    } else {
        int data_size = size - 2 - strlen(bson_iterator_key(elem));
        bson_append_estart(b, elem->cur[0], name_or_null, data_size);
        bson_append(b, bson_iterator_value(elem), data_size);
    }

    return BSON_OK;
}

EJDB_EXPORT int bson_append_timestamp(bson *b, const char *name, bson_timestamp_t *ts) {
    if (bson_append_estart(b, BSON_TIMESTAMP, name, 8) == BSON_ERROR) return BSON_ERROR;

    bson_append32(b, &(ts->i));
    bson_append32(b, &(ts->t));

    return BSON_OK;
}

EJDB_EXPORT int bson_append_timestamp2(bson *b, const char *name, int time, int increment) {
    if (bson_append_estart(b, BSON_TIMESTAMP, name, 8) == BSON_ERROR) return BSON_ERROR;

    bson_append32(b, &increment);
    bson_append32(b, &time);
    return BSON_OK;
}

EJDB_EXPORT int bson_append_date(bson *b, const char *name, bson_date_t millis) {
    if (bson_append_estart(b, BSON_DATE, name, 8) == BSON_ERROR) return BSON_ERROR;
    bson_append64(b, &millis);
    return BSON_OK;
}

EJDB_EXPORT int bson_append_time_t(bson *b, const char *name, time_t secs) {
    return bson_append_date(b, name, (bson_date_t) secs * 1000);
}

EJDB_EXPORT int bson_append_start_object(bson *b, const char *name) {
    if (bson_append_estart(b, BSON_OBJECT, name, 5) == BSON_ERROR) return BSON_ERROR;
    b->stack[ b->stackPos++ ] = b->cur - b->data;
    bson_append32(b, &zero);
    return BSON_OK;
}

EJDB_EXPORT int bson_append_start_array(bson *b, const char *name) {
    if (bson_append_estart(b, BSON_ARRAY, name, 5) == BSON_ERROR) return BSON_ERROR;
    b->stack[ b->stackPos++ ] = b->cur - b->data;
    bson_append32(b, &zero);
    return BSON_OK;
}

EJDB_EXPORT int bson_append_finish_object(bson *b) {
    char *start;
    int i;
    if (bson_ensure_space(b, 1) == BSON_ERROR) return BSON_ERROR;
    bson_append_byte(b, 0);

    start = b->data + b->stack[ --b->stackPos ];
    i = b->cur - start;
    bson_little_endian32(start, &i);

    return BSON_OK;
}

EJDB_EXPORT double bson_int64_to_double(int64_t i64) {
    return (double) i64;
}

EJDB_EXPORT int bson_append_finish_array(bson *b) {
    return bson_append_finish_object(b);
}

/* Error handling and allocators. */

static bson_err_handler err_handler = NULL;

EJDB_EXPORT bson_err_handler set_bson_err_handler(bson_err_handler func) {
    bson_err_handler old = err_handler;
    err_handler = func;
    return old;
}

EJDB_EXPORT void bson_free(void *ptr) {
    bson_free_func(ptr);
}

EJDB_EXPORT void *bson_malloc(int size) {
    void *p;
    p = bson_malloc_func(size);
    bson_fatal_msg(!!p, "malloc() failed");
    return p;
}

void *bson_realloc(void *ptr, int size) {
    void *p;
    p = bson_realloc_func(ptr, size);
    bson_fatal_msg(!!p, "realloc() failed");
    return p;
}

int _bson_errprintf(const char *format, ...) {
    va_list ap;
    int ret;
    va_start(ap, format);
#ifndef R_SAFETY_NET
    ret = vfprintf(stderr, format, ap);
#endif
    va_end(ap);

    return ret;
}

/**
 * This method is invoked when a non-fatal bson error is encountered.
 * Calls the error handler if available.
 *
 *  @param
 */
void bson_builder_error(bson *b) {
    if (err_handler)
        err_handler("BSON error.");
}

void bson_fatal(int ok) {
    bson_fatal_msg(ok, "");
}

void bson_fatal_msg(int ok, const char *msg) {
    if (ok)
        return;

    if (err_handler) {
        err_handler(msg);
    }
#ifndef R_SAFETY_NET
    bson_errprintf("error: %s\n", msg);
    exit(-5);
#endif
}


/* Efficiently copy an integer to a string. */
extern const char bson_numstrs[1000][4];

EJDB_EXPORT void bson_numstr(char *str, int64_t i) {
    if (i < 1000)
        memcpy(str, bson_numstrs[i], 4);
    else
        bson_sprintf(str, "%lld", (long long int) i);
}

EJDB_EXPORT int bson_numstrn(char *str, int maxbuf, int64_t i) {
    if (i < 1000 && maxbuf > 4) {
        memcpy(str, bson_numstrs[i], 4);
        return strlen(bson_numstrs[i]);
    } else {
        return snprintf(str, maxbuf, "%lld", (long long int) i);
    }
}

EJDB_EXPORT void bson_swap_endian64(void *outp, const void *inp) {
    const char *in = (const char *) inp;
    char *out = (char *) outp;

    out[0] = in[7];
    out[1] = in[6];
    out[2] = in[5];
    out[3] = in[4];
    out[4] = in[3];
    out[5] = in[2];
    out[6] = in[1];
    out[7] = in[0];

}

EJDB_EXPORT void bson_swap_endian32(void *outp, const void *inp) {
    const char *in = (const char *) inp;
    char *out = (char *) outp;

    out[0] = in[3];
    out[1] = in[2];
    out[2] = in[1];
    out[3] = in[0];
}

EJDB_EXPORT void bson_append_field_from_iterator(bson_iterator *from, bson *into) {
    bson_type t = bson_iterator_type(from);
    if (t == BSON_EOO) {
        return;
    }
    const char* key = bson_iterator_key(from);
    switch (t) {
        case BSON_STRING:
        case BSON_SYMBOL:
            bson_append_string(into, key, bson_iterator_string(from));
            break;
        case BSON_CODE:
            bson_append_code(into, key, bson_iterator_code(from));
            break;
        case BSON_INT:
            bson_append_int(into, key, bson_iterator_int_raw(from));
            break;
        case BSON_DOUBLE:
            bson_append_double(into, key, bson_iterator_double_raw(from));
            break;
        case BSON_LONG:
            bson_append_long(into, key, bson_iterator_long_raw(from));
            break;
        case BSON_UNDEFINED:
            bson_append_undefined(into, key);
            break;
        case BSON_NULL:
            bson_append_null(into, key);
            break;
        case BSON_BOOL:
            bson_append_bool(into, key, bson_iterator_bool_raw(from));
            break;
        case BSON_TIMESTAMP:
        {
            bson_timestamp_t ts = bson_iterator_timestamp(from);
            bson_append_timestamp(into, key, &ts);
            break;
        }
        case BSON_DATE:
            bson_append_date(into, key, bson_iterator_date(from));
            break;
        case BSON_REGEX:
            bson_append_regex(into, key, bson_iterator_regex(from), bson_iterator_regex_opts(from));
            break;
        case BSON_OID:
            bson_append_oid(into, key, bson_iterator_oid(from));
            break;
        case BSON_OBJECT:
        {
            bson_iterator sit;
            bson_iterator_subiterator(from, &sit);
            bson_append_start_object(into, key);
            while (bson_iterator_next(&sit) != BSON_EOO) {
                bson_append_field_from_iterator(&sit, into);
            }
            bson_append_finish_object(into);
            break;
        }
        case BSON_ARRAY:
        {
            bson_iterator sit;
            bson_iterator_subiterator(from, &sit);
            bson_append_start_array(into, key);
            while (bson_iterator_next(&sit) != BSON_EOO) {
                bson_append_field_from_iterator(&sit, into);
            }
            bson_append_finish_array(into);
            break;
        }
        case BSON_DBREF:
        case BSON_CODEWSCOPE:
            break;
        default:
            break;
    }

}

EJDB_EXPORT int bson_merge(bson *b1, bson *b2, bson_bool_t overwrite, bson *out) {
    assert(b1 && b2 && out);
    if (!b1->finished || !b2->finished || out->finished) {
        return BSON_ERROR;
    }

    bson_iterator it1, it2;
    bson_type bt1, bt2;

    bson_iterator_init(&it1, b1);
    bson_iterator_init(&it2, b2);

    //Append all fields in B1 overwrited by B2
    while ((bt1 = bson_iterator_next(&it1)) != BSON_EOO) {
        const char* k1 = bson_iterator_key(&it1);
        if (overwrite && (bt2 = bson_find(&it2, b2, k1)) != BSON_EOO) {
            bson_append_field_from_iterator(&it2, out);
        } else {
            bson_append_field_from_iterator(&it1, out);
        }
    }

    bson_iterator_init(&it1, b1);
    bson_iterator_init(&it2, b2);

    //Append all fields from B2 missing in B1
    while ((bt2 = bson_iterator_next(&it2)) != BSON_EOO) {
        const char* k2 = bson_iterator_key(&it2);
        if ((bt1 = bson_find(&it1, b1, k2)) == BSON_EOO) {
            bson_append_field_from_iterator(&it2, out);
        }
    }

    return BSON_OK;
}

EJDB_EXPORT int bson_compare_fpaths(const void *bsdata1, const void *bsdata2, const char *fpath1, int fplen1, const char *fpath2, int fplen2) {
    assert(bsdata1 && bsdata2 && fpath1 && fpath2);
    bson_iterator it1, it2;
    bson_type t1, t2;
    bson_iterator_from_buffer(&it1, bsdata1);
    bson_iterator_from_buffer(&it2, bsdata2);
    t1 = bson_find_fieldpath_value2(fpath1, fplen1, &it1);
    t2 = bson_find_fieldpath_value2(fpath2, fplen2, &it2);
    if (t1 == BSON_BOOL || t1 == BSON_EOO || t1 == BSON_NULL || t1 == BSON_UNDEFINED) {
        int v1 = bson_iterator_bool(&it1);
        int v2 = bson_iterator_bool(&it2);
        return (v1 > v2) ? 1 : ((v1 < v2) ? -1 : 0);
    } else if (t1 == BSON_INT || t1 == BSON_LONG || t1 == BSON_DATE || t1 == BSON_TIMESTAMP) {
        int64_t v1 = bson_iterator_long_ext(&it1);
        int64_t v2 = bson_iterator_long_ext(&it2);
        return (v1 > v2) ? 1 : ((v1 < v2) ? -1 : 0);
    } else if (t1 == BSON_DOUBLE) {
        double v1 = bson_iterator_double_raw(&it1);
        double v2 = bson_iterator_double(&it2);
        return (v1 > v2) ? 1 : ((v1 < v2) ? -1 : 0);
    } else if (t1 == BSON_STRING || t1 == BSON_SYMBOL) {
        const char* v1 = bson_iterator_string(&it1);
        int l1 = bson_iterator_string_len(&it1);
        const char* v2 = bson_iterator_string(&it2);
        int l2 = (t2 == BSON_STRING || t2 == BSON_SYMBOL) ? bson_iterator_string_len(&it2) : strlen(v2);
        int rv;
        TCCMPLEXICAL(rv, v1, l1, v2, l2);
        return rv;
    } else if (t1 == BSON_BINDATA && t2 == BSON_BINDATA) {
        int l1 = bson_iterator_bin_len(&it1);
        int l2 = bson_iterator_bin_len(&it2);
        return memcmp(bson_iterator_bin_data(&it1), bson_iterator_bin_data(&it2), MIN(l1, l2));
    }
    return 0;
}

EJDB_EXPORT int bson_compare(const void *bsdata1, const void *bsdata2, const char* fpath, int fplen) {
    return bson_compare_fpaths(bsdata1, bsdata2, fpath, fplen, fpath, fplen);
}

EJDB_EXPORT int bson_compare_string(const char *cv, const void *bsdata, const char *fpath) {
    assert(cv && bsdata && fpath);
    bson *bs1 = bson_create();
    bson_init(bs1);
    bson_append_string(bs1, "$", cv);
    bson_finish(bs1);
    int res = bson_compare_fpaths(bson_data(bs1), bsdata, "$", 1, fpath, strlen(fpath));
    bson_del(bs1);
    return res;
}

EJDB_EXPORT int bson_compare_long(long cv, const void *bsdata, const char *fpath) {
    bson *bs1 = bson_create();
    bson_init(bs1);
    bson_append_long(bs1, "$", cv);
    bson_finish(bs1);
    int res = bson_compare_fpaths(bson_data(bs1), bsdata, "$", 1, fpath, strlen(fpath));
    bson_del(bs1);
    return res;
}

EJDB_EXPORT int bson_compare_double(double cv, const void *bsdata, const char *fpath) {
    bson *bs1 = bson_create();
    bson_init(bs1);
    bson_append_double(bs1, "$", cv);
    bson_finish(bs1);
    int res = bson_compare_fpaths(bson_data(bs1), bsdata, "$", 1, fpath, strlen(fpath));
    bson_del(bs1);
    return res;
}

EJDB_EXPORT int bson_compare_bool(bson_bool_t cv, const void *bsdata, const char *fpath) {
    bson *bs1 = bson_create();
    bson_init(bs1);
    bson_append_bool(bs1, "$", cv);
    bson_finish(bs1);
    int res = bson_compare_fpaths(bson_data(bs1), bsdata, "$", 1, fpath, strlen(fpath));
    bson_del(bs1);
    return res;
}

EJDB_EXPORT bson* bson_dup(const bson* src) {
    assert(src);
    bson *rv = bson_create();
    const char *raw = bson_data(src);
    int sz = bson_size(src);
    bson_init_size(rv, sz);
    bson_ensure_space(rv, sz - 4);
    bson_append(rv, raw + 4, sz - (4 + 1/*BSON_EOO*/));
    bson_finish(rv);
    return rv;
}

EJDB_EXPORT bson* bson_create_from_buffer(const void* buf, int bufsz) {
    assert(buf);
    assert(bufsz - 4 > 0);
    bson *rv = bson_create();
    bson_init_size(rv, bufsz);
    bson_ensure_space(rv, bufsz - 4);
    bson_append(rv, (char*) buf + 4, bufsz - (4 + 1/*BSON_EOO*/));
    bson_finish(rv);
    return rv;
}
