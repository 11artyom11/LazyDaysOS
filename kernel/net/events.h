#ifndef __EVENTS_H__
#define __EVENTS_H__

#include <stdint.h>
#include <strbuf.h>

/* Ring buffer of recent router events for the dashboard (DHCP leases, blocks, ...) */

#define EVENTS_MAX          64
#define EVENTS_TEXT_LEN     96

/* category: short tag such as "dhcp", "wan", "firewall", "client", "system" */
void events_add(const char* category, const char* text);

/* JSON array of events with id > since, oldest first */
void events_json(struct strbuf* sb, uint32_t since);

#endif /* __EVENTS_H__ */
