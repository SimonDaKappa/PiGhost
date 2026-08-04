// fb_sink_file.c - see fb_sink_file.h.

#include "fb_sink_file.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

typedef struct {
  char out_dir[512];
  uint32_t snapshot_interval;
  uint32_t max_snapshots;
  uint64_t blit_count;
  char **snapshot_paths; /* ring buffer of malloc'd paths, len max_snapshots */
  uint32_t snapshot_head;
  uint32_t snapshot_count;
} pgdps_fb_sink_file_ctx_t;

/**
 * file_open() - ensure the output directory exists
 * @ctxv:   pgdps_fb_sink_file_ctx_t*, file sink state
 * @width:  negotiated frame width
 * @height: negotiated frame height
 *
 * idempotent; safe to call again on a mode change.
 */
static int file_open(void *ctxv, uint32_t width, uint32_t height);

/**
 * file_blit() - push one frame to the sink
 * @ctxv:     pgdps_fb_sink_file_ctx_t*, file sink stae
 * @pixels:   tightly packed XRGB8888 pixel data, @height rows of @width pixels
 * @width:    negotiated frame width
 * @height:   negotiated frame height
 * @frame_id: producer-assigned frame id, forwarded for logging/diagnostics only
 * 
 * Write latest.ppm and possibly a numbered snapshot.
 */
static int file_blit(void *ctxv, const unsigned char *pixels, uint32_t width,
                     uint32_t height, uint64_t frame_id);

/**
 * file_close() - release all resources
 * @ctxv: pgdps_fb_sink_file_ctx_t*, file sink state
 */
static void file_close(void *ctxv);

static const pgdps_fb_sink_ops_t pgdps_fb_sink_file_ops = {
    .open = file_open,
    .blit = file_blit,
    .close = file_close,
};

/**
 * write_ppm() - dump one XRGB8888 frame as a binary PPM (P6) file.
 * @path:    output file path
 * @pixels:  tightly packed XRGB8888 pixel data, @height rows of @width pixels
 * @width:   frame width
 * @height:  frame height
 *
 * Pixel format note: frames arrive as PGDP_FORMAT_XRGB8888 (libpgdp.h), which per DRM
 * convention is a 32-bit little-endian word laid out 0xXXRRGGBB (i.e. in memory byte
 * order B, G, R, X). This converts that to PPM's byte order (R, G, B) once per pixel.
 */
static int write_ppm(const char *path, const unsigned char *pixels, uint32_t width,
                     uint32_t height);



/**
 * file_snapshot_record() - track a newly-written numbered snapshot
 * @ctx:  file sink state
 * @path: path of the newly-written snapshot
 *
 * Evicts the oldest one if the ring is full. No-op if snapshots are unbounded
 * (max_snapshots == 0).
 */
static void file_snapshot_record(pgdps_fb_sink_file_ctx_t *ctx, const char *path);

static int file_blit(void *ctxv, const unsigned char *pixels, uint32_t width,
                     uint32_t height, uint64_t frame_id) {
  pgdps_fb_sink_file_ctx_t *ctx = (pgdps_fb_sink_file_ctx_t *)ctxv;

  char latest_path[600];
  snprintf(latest_path, sizeof(latest_path), "%s/latest.ppm", ctx->out_dir);
  if (write_ppm(latest_path, pixels, width, height) != 0)
    return -1;

  ctx->blit_count++;
  if (ctx->snapshot_interval > 0 && (ctx->blit_count % ctx->snapshot_interval) == 0) {
    char path[600];
    snprintf(path, sizeof(path), "%s/frame_%08llu.ppm", ctx->out_dir,
             (unsigned long long)frame_id);
    if (write_ppm(path, pixels, width, height) == 0)
      file_snapshot_record(ctx, path);
  }

  return 0;
}

static void file_close(void *ctxv) {
  pgdps_fb_sink_file_ctx_t *ctx = (pgdps_fb_sink_file_ctx_t *)ctxv;
  if (!ctx)
    return;

  if (ctx->snapshot_paths) {
    for (uint32_t i = 0; i < ctx->snapshot_count; i++)
      free(ctx->snapshot_paths[i]);
    free(ctx->snapshot_paths);
  }
  free(ctx);
}

int pgdps_fb_sink_file_create(pgdps_fb_sink_t *sink,
                              const pgdps_fb_sink_file_opts_t *opts) {
  pgdps_fb_sink_file_ctx_t *ctx = (pgdps_fb_sink_file_ctx_t *)calloc(1, sizeof(*ctx));
  if (!ctx)
    return -1;

  strncpy(ctx->out_dir, opts->out_dir, sizeof(ctx->out_dir) - 1);
  ctx->snapshot_interval = opts->snapshot_interval;
  ctx->max_snapshots = opts->max_snapshots;

  if (ctx->max_snapshots > 0) {
    ctx->snapshot_paths = (char **)calloc(ctx->max_snapshots, sizeof(char *));
    if (!ctx->snapshot_paths) {
      free(ctx);
      return -1;
    }
  }

  sink->ops = &pgdps_fb_sink_file_ops;
  sink->ctx = ctx;
  return 0;
}

static int write_ppm(const char *path, const unsigned char *pixels, uint32_t width,
                     uint32_t height) {
  FILE *f = fopen(path, "wb");
  if (!f) {
    perror("fopen (ppm)");
    return -1;
  }

  fprintf(f, "P6\n%u %u\n255\n", width, height);

  unsigned char *row = (unsigned char *)malloc((size_t)width * 3);
  if (!row) {
    fclose(f);
    return -1;
  }

  for (uint32_t y = 0; y < height; y++) {
    const unsigned char *src = pixels + (size_t)y * width * 4;
    for (uint32_t x = 0; x < width; x++) {
      unsigned char b = src[x * 4 + 0];
      unsigned char g = src[x * 4 + 1];
      unsigned char r = src[x * 4 + 2];
      row[x * 3 + 0] = r;
      row[x * 3 + 1] = g;
      row[x * 3 + 2] = b;
    }
    if (fwrite(row, 1, (size_t)width * 3, f) != (size_t)width * 3) {
      free(row);
      fclose(f);
      return -1;
    }
  }

  free(row);
  fclose(f);
  return 0;
}

static int file_open(void *ctxv, uint32_t width, uint32_t height) {
  pgdps_fb_sink_file_ctx_t *ctx = (pgdps_fb_sink_file_ctx_t *)ctxv;
  (void)width;
  (void)height;

  struct stat st;
  if (stat(ctx->out_dir, &st) != 0) {
    if (mkdir(ctx->out_dir, 0755) != 0 && errno != EEXIST) {
      perror("mkdir (fb_sink_file out_dir)");
      return -1;
    }
  }
  return 0;
}

static void file_snapshot_record(pgdps_fb_sink_file_ctx_t *ctx, const char *path) {
  if (ctx->max_snapshots == 0)
    return;

  if (ctx->snapshot_count == ctx->max_snapshots) {
    remove(ctx->snapshot_paths[ctx->snapshot_head]);
    free(ctx->snapshot_paths[ctx->snapshot_head]);
    ctx->snapshot_paths[ctx->snapshot_head] = strdup(path);
    ctx->snapshot_head = (ctx->snapshot_head + 1) % ctx->max_snapshots;
  } else {
    ctx->snapshot_paths[ctx->snapshot_count] = strdup(path);
    ctx->snapshot_count++;
  }
}