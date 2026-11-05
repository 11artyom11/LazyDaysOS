#include "cmdline.h"

static char cmdline$buf[CMDLINE_MAX];

void cmdline_init(const char* cmdline)
{
	size_t i = 0;
	if (cmdline) {
		for (; cmdline[i] && i < CMDLINE_MAX - 1; i++) {
			cmdline$buf[i] = cmdline[i];
		}
	}
	cmdline$buf[i] = '\0';
}

const char* cmdline_raw(void)
{
	return cmdline$buf;
}

/* Advance to the next word; returns its start and stores its length, or NULL at the end */
static const char* cmdline$$next_word(const char** cursor, size_t* len)
{
	const char* p = *cursor;
	while (*p == ' ' || *p == '\t') {
		p++;
	}
	if (*p == '\0') {
		return NULL;
	}
	const char* start = p;
	while (*p && *p != ' ' && *p != '\t') {
		p++;
	}
	*len = p - start;
	*cursor = p;
	return start;
}

bool cmdline_has(const char* word)
{
	const char* cursor = cmdline$buf;
	const char* w;
	size_t len;
	while ((w = cmdline$$next_word(&cursor, &len)) != NULL) {
		size_t i = 0;
		while (i < len && word[i] && w[i] == word[i]) {
			i++;
		}
		if (i == len && word[i] == '\0') {
			return true;
		}
	}
	return false;
}

bool cmdline_get(const char* key, size_t nth, char* out, size_t out_len)
{
	const char* cursor = cmdline$buf;
	const char* w;
	size_t len;
	while ((w = cmdline$$next_word(&cursor, &len)) != NULL) {
		size_t i = 0;
		while (i < len && key[i] && w[i] == key[i]) {
			i++;
		}
		if (key[i] != '\0' || i >= len || w[i] != '=') {
			continue;
		}
		if (nth-- > 0) {
			continue;
		}
		const char* value = w + i + 1;
		size_t value_len = len - i - 1;
		if (value_len + 1 > out_len) {
			return false;
		}
		for (size_t v = 0; v < value_len; v++) {
			out[v] = value[v];
		}
		out[value_len] = '\0';
		return true;
	}
	return false;
}
