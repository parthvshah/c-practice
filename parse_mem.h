#ifndef PARSE_MEM_H
#define PARSE_MEM_H

#include <stdint.h>

#define MEMINFO_MAX_KEY 64

typedef struct {
    char key[MEMINFO_MAX_KEY];
    uint64_t value_kb;
} meminfo_entry_t;

int     parse_meminfo(const char *input, meminfo_entry_t *entries, int max_entries);
int64_t lookup_meminfo(meminfo_entry_t *entries, int count, const char *key);
void    print_memory_summary(const char *raw_meminfo);

#endif /* PARSE_MEM_H */
