#ifndef __STRBUF_H__
#define __STRBUF_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
    Bounded string builder over a caller-supplied buffer. Appends never write past
    the buffer; if something doesn't fit, `overflow` is set and the text stays
    NUL-terminated at the last complete character.
*/
struct strbuf
{
    char* data;
    size_t cap;
    size_t len;
    bool overflow;
};

void sb_init(struct strbuf* sb, char* buffer, size_t capacity);
void sb_char(struct strbuf* sb, char c);
void sb_str(struct strbuf* sb, const char* s);
void sb_uint(struct strbuf* sb, uint32_t value);
void sb_int(struct strbuf* sb, int32_t value);
void sb_hex2(struct strbuf* sb, uint8_t value);         /* two lowercase hex digits */
void sb_ip(struct strbuf* sb, uint32_t ip);             /* dotted quad, ip in host order */
void sb_mac(struct strbuf* sb, const uint8_t mac[6]);   /* aa:bb:cc:dd:ee:ff */

/* JSON string literal with quotes; escapes ", \ and control characters */
void sb_json_str(struct strbuf* sb, const char* s);

/* "name": prefix for JSON objects, with a comma unless first */
void sb_json_key(struct strbuf* sb, const char* name, bool first);

#endif /* __STRBUF_H__ */
