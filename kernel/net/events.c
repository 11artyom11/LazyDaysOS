#include "events.h"
#include <kio.h>
#include <pit.h>

#define EVENTS$CATEGORY_LEN 12

struct events$entry
{
    uint32_t id;                        /* 0 = empty; ids start at 1 and only grow */
    uint32_t time_ms;
    char category[EVENTS$CATEGORY_LEN];
    char text[EVENTS_TEXT_LEN];
};

static struct events$entry events$ring[EVENTS_MAX];
static uint32_t events$next_id = 1;

static void events$$copy(char* dst, const char* src, size_t cap)
{
    size_t i = 0;
    for (; src[i] && i < cap - 1; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

void events_add(const char* category, const char* text)
{
    struct events$entry* e = &events$ring[events$next_id % EVENTS_MAX];
    e->id = events$next_id++;
    e->time_ms = pit_uptime_ms();
    events$$copy(e->category, category, sizeof(e->category));
    events$$copy(e->text, text, sizeof(e->text));
    k_print("[K_EVNT] %s: %s\n", e->category, e->text);
}

void events_json(struct strbuf* sb, uint32_t since)
{
    bool first = true;
    sb_char(sb, '[');
    /* The oldest surviving entry is the one after the newest */
    for (uint32_t id = events$next_id > EVENTS_MAX ? events$next_id - EVENTS_MAX : 1; id < events$next_id; id++) {
        const struct events$entry* e = &events$ring[id % EVENTS_MAX];
        if (e->id != id || id <= since) {
            continue;
        }
        if (!first) {
            sb_char(sb, ',');
        }
        first = false;
        sb_char(sb, '{');
        sb_json_key(sb, "id", true);
        sb_uint(sb, e->id);
        sb_json_key(sb, "time_ms", false);
        sb_uint(sb, e->time_ms);
        sb_json_key(sb, "category", false);
        sb_json_str(sb, e->category);
        sb_json_key(sb, "text", false);
        sb_json_str(sb, e->text);
        sb_char(sb, '}');
    }
    sb_char(sb, ']');
}
