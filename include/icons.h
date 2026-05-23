/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_ICONS_H
#define LABWC_ICONS_H

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

void icons_update(void);
void process_icon_release(float sx, float sy);;
void process_icon_drag(float sx, float sy);
void process_icon_press(float sx, float sy, struct view *found_view);
void process_icon_move(float sx, float sy, struct view *found_view);
extern struct view *active_drag_icon;

#endif /* LABWC_ICONS_H */
