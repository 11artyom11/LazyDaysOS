#include "strbuf.h"

void sb_init(struct strbuf* sb, char* buffer, size_t capacity)
{
    sb->data = buffer;
    sb->cap = capacity;
    sb->len = 0;
    sb->overflow = false;
    if (capacity) {
        buffer[0] = '\0';
    }
}

void sb_char(struct strbuf* sb, char c)
{
    if (sb->len + 1 >= sb->cap) {
        sb->overflow = true;
        return;
    }
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
}

void sb_str(struct strbuf* sb, const char* s)
{
    while (*s) {
        sb_char(sb, *s++);
    }
}

void sb_uint(struct strbuf* sb, uint32_t value)
{
    char digits[10];
    int n = 0;
    do {
        digits[n++] = '0' + value % 10;
        value /= 10;
    } while (value);
    while (n) {
        sb_char(sb, digits[--n]);
    }
}

void sb_int(struct strbuf* sb, int32_t value)
{
    if (value < 0) {
        sb_char(sb, '-');
        sb_uint(sb, (uint32_t)(~value) + 1);
    } else {
        sb_uint(sb, (uint32_t)value);
    }
}

void sb_hex2(struct strbuf* sb, uint8_t value)
{
    static const char hex[] = "0123456789abcdef";
    sb_char(sb, hex[value >> 4]);
    sb_char(sb, hex[value & 0xF]);
}

void sb_ip(struct strbuf* sb, uint32_t ip)
{
    for (int shift = 24; shift >= 0; shift -= 8) {
        sb_uint(sb, (ip >> shift) & 0xFF);
        if (shift) {
            sb_char(sb, '.');
        }
    }
}

void sb_mac(struct strbuf* sb, const uint8_t mac[6])
{
    for (int i = 0; i < 6; i++) {
        sb_hex2(sb, mac[i]);
        if (i < 5) {
            sb_char(sb, ':');
        }
    }
}

void sb_json_str(struct strbuf* sb, const char* s)
{
    sb_char(sb, '"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            sb_char(sb, '\\');
            sb_char(sb, (char)c);
        } else if (c < 0x20) {
            sb_str(sb, "\\u00");
            sb_hex2(sb, c);
        } else {
            sb_char(sb, (char)c);
        }
    }
    sb_char(sb, '"');
}

void sb_json_key(struct strbuf* sb, const char* name, bool first)
{
    if (!first) {
        sb_char(sb, ',');
    }
    sb_json_str(sb, name);
    sb_char(sb, ':');
}
