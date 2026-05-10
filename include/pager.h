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

void pager_create(void);
void pager_update(void);
void process_pager_release(float sx, float sy);
void process_pager_drag(float sx, float sy);
void process_pager_press(float sx, float sy);
void process_pager_move(float sx, float sy, struct view *found_view);
struct view *find_pager_window(float sx, float sy);

extern struct view *active_drag_view;

#endif /* LABWC_PAGER_H */
