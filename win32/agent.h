#pragma once

#ifdef __cplusplus
extern "C" {
#endif

extern uint32_t flags;

void print_debug(const char *fmt, ...);
void agent_query(void *buf);

#ifdef __cplusplus
}
#endif
