#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "parse_mem.h"

int parse_meminfo(const char *input, meminfo_entry_t *entries, int max_entries) {
    if (!input || !entries || max_entries <= 0)
        return -1;

    char *copy = strdup(input);
    if (!copy)
        return -1;

    int count = 0;
    char *line = strtok(copy, "\n");

    while (line && count < max_entries) {
        if (line[0] == '\0') {
            line = strtok(NULL, "\n");
            continue;
        }

        const char *colon = strchr(line, ':');
        if (!colon) {                          /* skip malformed lines */
            line = strtok(NULL, "\n");
            continue;
        }

        size_t key_len = (size_t)(colon - line);
        if (key_len >= MEMINFO_MAX_KEY) {      /* skip oversized keys */
            line = strtok(NULL, "\n");
            continue;
        }

        memcpy(entries[count].key, line, key_len);
        entries[count].key[key_len] = '\0';

        const char *val_start = colon + 1;
        while (*val_start == ' ' || *val_start == '\t')
            val_start++;

        entries[count].value_kb = (uint64_t)strtoull(val_start, NULL, 10);

        count++;
        line = strtok(NULL, "\n");             /* advance to next line */
    }

    free(copy);
    return count;
}

int64_t lookup_meminfo(meminfo_entry_t *entries, int count, const char *key) {
    if (!entries || !key)
        return -1;

    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].key, key) == 0)
            return (int64_t)entries[i].value_kb;
    }

    return -1;
}

void print_memory_summary(const char *raw_meminfo) {
    meminfo_entry_t entries[64];
    int count = parse_meminfo(raw_meminfo, entries, 64);
    if (count < 0)
        return;

    int64_t total     = lookup_meminfo(entries, count, "MemTotal");
    int64_t free_mem  = lookup_meminfo(entries, count, "MemFree");
    int64_t available = lookup_meminfo(entries, count, "MemAvailable");

    printf("Summary:\n");
    printf("Total:     %" PRId64 " MB\n", total     >= 0 ? total     / 1024 : -1);
    printf("Free:      %" PRId64 " MB\n", free_mem  >= 0 ? free_mem  / 1024 : -1);
    printf("Available: %" PRId64 " MB\n", available >= 0 ? available / 1024 : -1);

    if (total >= 0 && free_mem >= 0)
        printf("Used:      %" PRId64 " MB\n", (total - free_mem) / 1024);
    else
        printf("Used:      N/A\n");
}
