/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_WORKSPACES_H
#define LABWC_WORKSPACES_H

#include <stdbool.h>
#include <wayland-util.h>
#include <wayland-server-core.h>
#include <cairo.h>

struct seat;
struct server;
struct wlr_scene_tree;
struct view;
enum border_type;
struct workspace {
	struct wl_list link; /* struct server.workspaces */

	char *name;
	struct wlr_scene_tree *tree;
	struct wlr_scene_tree *view_trees[3];

	struct wlr_ext_workspace_handle_v1 *ext_workspace;
};

void workspaces_init(void);
void workspaces_switch_to(struct workspace *target, bool update_focus);
void workspaces_destroy(void);
void workspaces_osd_hide(struct seat *seat);
struct workspace *workspaces_find(struct workspace *anchor, const char *name,
	bool wrap);
void workspaces_reconfigure(void);
void pager_create(void);
void pager_update(void);
void process_pager_release(float sx, float sy);
void process_pager_drag(float sx, float sy);
void process_pager_press(float sx, float sy);
void process_pager_move(float sx, float sy, struct view *found_view);
struct view *find_pager_window(float sx, float sy);

void
cairo_borders(cairo_t *cairo, int x, int y, int width, int height, int bw, int highlight,
			int shadow, enum border_type border_type, int bevel_width, float color[]);

extern struct view *active_drag_view;


#endif /* LABWC_WORKSPACES_H */
