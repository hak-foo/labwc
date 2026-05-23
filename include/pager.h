/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_PAGER_H
#define LABWC_PAGER_H

#include <stdbool.h>
#include <wayland-util.h>
#include <wayland-server-core.h>
#include <cairo.h>
#include "workspaces.h"

struct seat;
struct server;
struct wlr_scene_tree;
struct view;
enum border_type;

struct thumbnail_cache {
	uint64_t creation_id;
	uint32_t age;
	unsigned char *thumbnail;
	int width;
	int height;
	struct thumbnail_cache *next;
};

void pager_flush(struct view *view);
void pager_create(void);
void pager_update(void);
void icons_update(void);
void process_pager_release(float sx, float sy);
void process_icon_release(float sx, float sy);
void process_pager_drag(float sx, float sy);
void process_icon_drag(float sx, float sy);
void process_pager_press(float sx, float sy);
void process_icon_press(float sx, float sy, struct view *found_view);
void process_pager_move(float sx, float sy, struct view *found_view);
void process_icon_move(float sx, float sy, struct view *found_view);
struct view *find_pager_window(float sx, float sy);
unsigned char *get_thumbnail_cache(struct output *output, struct view *view,
	struct wlr_fbox border_fbox);
struct wlr_fbox thumbnail_size(struct view *view, int wscount);
extern struct view *active_drag_view;
extern struct view *active_drag_icon;

#endif /* LABWC_PAGER_H */
