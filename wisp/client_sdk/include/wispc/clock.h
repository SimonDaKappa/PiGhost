#pragma once

typedef enum {
  WISPC_CLOCK_KIND_HEADLESS = 0,
  WISPC_CLOCK_KIND_PACED = 1,
  WISPC_CLOCK_KIND_TICKED = 2,
} wispc_clock_kind_t;

typedef struct {
  wispc_clock_kind_t clock_kind;
} wispc_clock_cfg_t;