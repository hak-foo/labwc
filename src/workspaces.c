// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L
#include "workspaces.h"
#include <assert.h>
#include <cairo.h>
#include <pango/pangocairo.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wlr/types/wlr_ext_workspace_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include "buffer.h"
#include "common/font.h"
#include "common/graphic-helpers.h"
#include "common/list.h"
#include "common/mem.h"
#include "common/scene-helpers.h"
#include "config/rcxml.h"
#include "input/keyboard.h"
#include "common/borderset.h"
#include "labwc.h"
#include "output.h"
#include "show-desktop.h"
#include "theme.h"
#include "view.h"
#include "regions.h"
#include "node.h"

#define EXT_WORKSPACES_VERSION 1

float pager_drag_start_x, pager_drag_start_y;
struct view *active_drag_view;

/* Internal helpers */
static size_t
parse_workspace_index(const char *name)
{
	/*
	 * We only want to get positive numbers which span the whole string.
	 *
	 * More detailed requirement:
	 *  .---------------.--------------.
	 *  |     Input     | Return value |
	 *  |---------------+--------------|
	 *  | "2nd desktop" |      0       |
	 *  |    "-50"      |      0       |
	 *  |     "0"       |      0       |
	 *  |    "124"      |     124      |
	 *  |    "1.24"     |      0       |
	 *  `------------------------------´
	 *
	 * As atoi() happily parses any numbers until it hits a non-number we
	 * can't really use it for this case. Instead, we use strtol() combined
	 * with further checks for the endptr (remaining non-number characters)
	 * and returned negative numbers.
	 */
	long index;
	char *endptr;
	errno = 0;
	index = strtol(name, &endptr, 10);
	if (errno || *endptr != '\0' || index < 0) {
		return 0;
	}
	return index;
}


struct view * find_pager_window(float sx, float sy)
{
	int pagerwidth = rc.pager_width - 2* rc.theme->pager_border_width;
	int totalPagerheight = rc.pager_height - 2 * rc.theme->pager_border_width;
	int pagerheight = ceil((float)totalPagerheight / wl_list_length(&rc.workspace_config.workspaces));

	struct wlr_box overallBox = { 0 };
	wlr_output_layout_get_box(server.output_layout,
		NULL, &overallBox);

	int screenwidth = overallBox.width - overallBox.x;
	int screenheight = overallBox.height - overallBox.y;
	

	struct workspace *workspace;
	struct view *view;
	
	int wscount = 0;
		wl_list_for_each(workspace, &server.workspaces.all, link) {
			// Start at the top down
			for_each_view(view, &server.views, LAB_VIEW_CRITERIA_NONE) {
				if (view->workspace == workspace) {
					int wx = view->current.x * pagerwidth / screenwidth;
					int wy = view->current.y * pagerheight / screenheight + pagerheight * wscount;
					int width = view->current.width * pagerwidth / screenwidth;
					int height = view->current.height * pagerheight / screenheight;
					if (sx >= wx && sy >= wy && sx <= wx+width && sy <= wy+height) {
						return view;
					}
				}
			}
			wscount++;
		}
	
	return NULL;
}

void process_pager_move(float sx, float sy, struct view *found_view) {
	int pagerwidth = rc.pager_width - 2* rc.theme->pager_border_width;
	int totalPagerheight = rc.pager_height - 2 * rc.theme->pager_border_width;
	int pagerheight = ceil((float)totalPagerheight / wl_list_length(&rc.workspace_config.workspaces));
	
	struct wlr_box overallBox = { 0 };
	wlr_output_layout_get_box(server.output_layout,
		NULL, &overallBox);

	int screenwidth = overallBox.width - overallBox.x;
	int screenheight = overallBox.height - overallBox.y;


	if (found_view) {
		int old_workspace = pager_drag_start_y / pagerheight;
		int new_workspace = sy / pagerheight;
		if (old_workspace == new_workspace) {
			int delta_x = (sx - pager_drag_start_x) * screenwidth / pagerwidth;	
			int delta_y = (sy - pager_drag_start_y) * screenheight / pagerheight;	
			view_move_relative(found_view, delta_x, delta_y);
		} else {
			int delta_x = (sx - pager_drag_start_x) * screenwidth / pagerwidth;	
			int delta_y = (((int)sy % pagerheight) - ((int)pager_drag_start_y % pagerheight)) * screenheight / pagerheight;	
			int target_workspace = sy/pagerheight;
			int wscount = 0;
			struct workspace * workspace;
			wl_list_for_each(workspace, &server.workspaces.all, link) {
				if (wscount == target_workspace) {
					view_move_to_workspace(found_view, workspace);
					break;
				}
				wscount++;
			}
			view_move_relative(found_view, delta_x, delta_y);
		}
		pager_update();
	}
}

void process_pager_release(float sx, float sy)
{
	if (sx < rc.theme->pager_border_width || sy < rc.theme->pager_border_width || sx > rc.pager_width - rc.theme->pager_border_width || sy > rc.pager_height - rc.theme->pager_border_width) {
		return;
	}
	sx -= rc.theme->pager_border_width;
	sy -= rc.theme->pager_border_width;
	if (!active_drag_view) return;
	
	struct view * found_view = find_pager_window(pager_drag_start_x, pager_drag_start_y);
	if (found_view) {
		process_pager_move(sx, sy, found_view);
		desktop_focus_view(found_view, true);
	}
	active_drag_view = NULL;
	pager_update();
}

void process_pager_drag(float sx, float sy) {
	if (sx < rc.theme->pager_border_width || sy < rc.theme->pager_border_width || sx > rc.pager_width - rc.theme->pager_border_width || sy > rc.pager_height - rc.theme->pager_border_width) {
		return;
	}
	sx -= rc.theme->pager_border_width;
	sy -= rc.theme->pager_border_width;
	if (active_drag_view) {
		process_pager_move(sx, sy, active_drag_view);
		pager_drag_start_x = sx;
		pager_drag_start_y = sy;
	}
}

void process_pager_press(float sx, float sy)
{
	if (sx < rc.theme->pager_border_width || sy < rc.theme->pager_border_width || sx > rc.pager_width - rc.theme->pager_border_width || sy > rc.pager_height - rc.theme->pager_border_width) {
		return;
	}
	sx -= rc.theme->pager_border_width;
	sy -= rc.theme->pager_border_width;
	struct view * found_view = find_pager_window(sx, sy);
	if (found_view) {
		pager_drag_start_x = sx;
		pager_drag_start_y = sy;
		pager_update();
	} else {
		int totalPagerheight = rc.pager_height - 2 * rc.theme->pager_border_width;
		int pagerheight = ceil((float)totalPagerheight / wl_list_length(&rc.workspace_config.workspaces));
		int wscount = 0;
		int target_workspace = sy/pagerheight;
		
		struct workspace * workspace;
		wl_list_for_each(workspace, &server.workspaces.all, link) {
			if (wscount == target_workspace) {
				workspaces_switch_to(workspace, /* update_focus */ true);
				pager_update();
				break;
			}
			wscount++;
		}
	}
	active_drag_view = found_view;
}

void pager_update(void) {
	struct output *output;
	if (!rc.pager_enabled) {
		wl_list_for_each(output, &server.outputs, link) {
			if (!output_is_usable(output)) {
				continue;
			}
			if (output->pager_osd) {
				wlr_scene_node_set_enabled(&output->pager_osd->node, false);
			}
		}
		return;
	}
	if (wl_list_empty(&rc.workspace_config.workspaces)) {
		return;
	}
	
	struct theme *theme = rc.theme;
	int pagerwidth = rc.pager_width - theme->pager_border_width * 2;
	int totalPagerheight = rc.pager_height - theme->pager_border_width * 2;
	int x = rc.pager_x;
	int y = rc.pager_y;
	

	struct workspace *workspace;
	struct view *view;
	struct wlr_box overallBox = { 0 };
	wlr_output_layout_get_box(server.output_layout,
		NULL, &overallBox);

	int screenwidth = overallBox.width - overallBox.x;
	int screenheight = overallBox.height - overallBox.y;
	
	// Divide pager among workspaces vertically
	int pagerheight = ceil((float)totalPagerheight / wl_list_length(&rc.workspace_config.workspaces));
	
	int font_h = font_height(&rc.font_pager);
	
	wl_list_for_each(output, &server.outputs, link) {
		if (!output_is_usable(output)) {
			continue;
		}

		struct lab_data_buffer *buffer = buffer_create_cairo(rc.pager_width,
			rc.pager_height, output->wlr_output->scale);
		if (!buffer) {
			wlr_log(WLR_ERROR, "Failed to allocate buffer for pager");
			continue;
		}

		cairo_t *cairo;
		cairo_surface_t *surface;
		cairo = cairo_create(buffer->surface);
		int wscount = 0;
		wl_list_for_each(workspace, &server.workspaces.all, link) {
			/* Background */
			set_cairo_color(cairo, server.workspaces.current == workspace ? theme->pager_color_active : theme->pager_color_inactive);
			cairo_rectangle(cairo, theme->pager_border_width, theme->pager_border_width + pagerheight * wscount, pagerwidth, pagerheight);
			cairo_fill(cairo);
			
			for_each_view_reverse(view, &server.views, LAB_VIEW_CRITERIA_NONE) {
				if (view->workspace == workspace) {
					int wx = view->current.x * pagerwidth / screenwidth;
					int wy = view->current.y * pagerheight / screenheight + pagerheight * wscount;
					int width = view->current.width * pagerwidth / screenwidth;
					int height = view->current.height * pagerheight / screenheight;
					// Bound the outlines to the current pager frame
					if (wy < pagerheight * wscount) {
						height += (wy - pagerheight * wscount);
					}
					wy = MAX(pagerheight*wscount, wy);
					
					width = MIN(width, pagerwidth - wx);
					if (wx < 0) {
						width += wx;
					}
					wx = MAX(0, wx);
					// Crop to current frame
					height = MIN(height, (wscount+1) * pagerheight - wy);
					// Crop to bottom edge for odd sizes where the last frame is marginally smaller
					height = MIN(height, (totalPagerheight-1 - wy));
					
					struct wlr_fbox border_fbox = {
						.x = wx,
						.y = wy,
						.width = width,
						.height = height,
						};

					if (view->shaded) {
						border_fbox.height = 1;
					}
					if (view->minimized) {
						set_cairo_color(cairo, theme->pager_color_minimized_window);
					} else {
						set_cairo_color(cairo, theme->pager_color_window);
					}
					cairo_rectangle(cairo, theme->pager_border_width+border_fbox.x, theme->pager_border_width+border_fbox.y, border_fbox.width, border_fbox.height);
					cairo_fill(cairo);
					if (view->minimized) {
						cairo_borders(cairo, theme->pager_border_width+ border_fbox.x,
							theme->pager_border_width+border_fbox.y, border_fbox.width, border_fbox.height,
							theme->pager_minimized_window_border_width, theme->pager_minimized_window_highlight,
							theme->pager_minimized_window_shadow, theme->pager_minimized_window_border_type,
							theme->pager_minimized_window_bevel_width, theme->pager_color_minimized_window);
					} else {
						cairo_borders(cairo, theme->pager_border_width+ border_fbox.x,
							theme->pager_border_width+border_fbox.y, border_fbox.width, border_fbox.height,
							theme->pager_window_border_width, theme->pager_window_highlight,
							theme->pager_window_shadow, theme->pager_window_border_type,
							theme->pager_window_bevel_width, theme->pager_color_window);
					}

					
					if (border_fbox.height >= font_h + 6) {
						PangoLayout *layout = pango_cairo_create_layout(cairo);
						pango_context_set_round_glyph_positions(pango_layout_get_context(layout), false);
						pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
						int req_width = font_width(&rc.font_pager, view->title);
						PangoFontDescription *desc = font_to_pango_desc(&rc.font_pager);
						if (view->minimized) {
							set_cairo_color(cairo, theme->pager_color_minimized_window_title);
					
							req_width = MIN(req_width,
								border_fbox.width-2*theme->pager_minimized_window_border_width -2);
						} else {
							set_cairo_color(cairo, theme->pager_color_window_title);
					
							req_width = MIN(req_width, border_fbox.width-2*theme->pager_window_border_width -2);
						}
						cairo_move_to(cairo,
						theme->pager_border_width+border_fbox.x+ (border_fbox.width - req_width) / 2,
						theme->pager_border_width+border_fbox.y+(border_fbox.height - font_h) / 2);
						pango_layout_set_font_description(layout, desc);
						pango_layout_set_width(layout, req_width * PANGO_SCALE);
						pango_font_description_free(desc);
						pango_layout_set_text(layout, view->title, -1);
						pango_cairo_show_layout(cairo, layout);

						g_object_unref(layout);
					}
				}
			}
			wscount++;
		}
		cairo_borders(cairo, 0, 0, rc.pager_width, rc.pager_height, theme->pager_border_width,
					theme->pager_highlight, theme->pager_shadow,
					theme->pager_border_type, theme->pager_bevel_width,
					theme->pager_color_border);
		surface = cairo_get_target(cairo);
		cairo_surface_flush(surface);
		cairo_destroy(cairo);

		if (!output->pager_osd) {
			output->pager_osd = lab_wlr_scene_buffer_create(
				&server.scene->tree, NULL);
			node_descriptor_create(&output->pager_osd->node, LAB_NODE_PAGER, NULL, 0);
		}
		wlr_scene_node_set_enabled(&output->pager_osd->node, true);

		
		wlr_scene_node_set_position(&output->pager_osd->node, x, y);
		wlr_scene_buffer_set_buffer(output->pager_osd, &buffer->base);
		wlr_scene_buffer_set_dest_size(output->pager_osd,
			buffer->logical_width, buffer->logical_height);
		wlr_scene_node_place_below(&output->pager_osd->node, &output->layer_tree[1]->node);
		wlr_buffer_drop(&buffer->base);
	}
}


void
cairo_borders(cairo_t *cairo, int x, int y, int width, int height, int bw, int highlight,
	int shadow, enum border_type border_type, int bevel_width, float color[])
{
	float r = color[0];
	float g = color[1];
	float b = color[2];
	float a = color[3];

	uint32_t colour32 = (uint32_t)(a*255) << 24 |
		(uint32_t)(r*255) << 16 |
		(uint32_t)(g*255) << 8 |
		(uint32_t)(b*255);
	struct borderset *renderedborders = get_borders(colour32, bw,
		border_type, bevel_width,
		highlight, shadow);

	cairo_set_source_surface(cairo, renderedborders->top->surface, x+bw, y);
	cairo_pattern_set_extend(cairo_get_source(cairo), CAIRO_EXTEND_REPEAT);
	cairo_rectangle(cairo, x+bw, y, width-bw*2, bw);
	cairo_fill(cairo);

	cairo_set_source_surface(cairo, renderedborders->bottom->surface,  x+bw, y+height-bw);
	cairo_pattern_set_extend(cairo_get_source(cairo), CAIRO_EXTEND_REPEAT);
	cairo_rectangle(cairo, x+bw, y+height-bw, width-bw*2, bw);
	cairo_fill(cairo);

	cairo_set_source_surface(cairo, renderedborders->left->surface, x, y+bw);
	cairo_pattern_set_extend(cairo_get_source(cairo), CAIRO_EXTEND_REPEAT);
	cairo_rectangle(cairo, x, y + bw, bw, height-bw*2);
	cairo_fill(cairo);

	cairo_set_source_surface(cairo, renderedborders->right->surface, x+width-bw, y+bw);
	cairo_pattern_set_extend(cairo_get_source(cairo), CAIRO_EXTEND_REPEAT);
	cairo_rectangle(cairo, x + width - bw, y + bw, bw, height-bw*2);
	cairo_fill(cairo);

	cairo_set_source_surface(cairo, renderedborders->tl->surface, x, y);
	cairo_rectangle(cairo, x, y, bw, bw);
	cairo_fill(cairo);

	cairo_set_source_surface(cairo, renderedborders->tr->surface, x+width-bw, y);
	cairo_rectangle(cairo, x+width - bw, y, bw, bw);
	cairo_fill(cairo);

	cairo_set_source_surface(cairo, renderedborders->bl->surface,
		x, y+height - bw);
	cairo_rectangle(cairo, x, y + height - bw, bw, bw);
	cairo_fill(cairo);

	cairo_set_source_surface(cairo, renderedborders->br->surface,
		x+width - bw, y+height -bw);
	cairo_rectangle(cairo, x + width - bw, y + height - bw, bw, bw);
	cairo_fill(cairo);
}

static void
_osd_update(void)
{
	struct theme *theme = rc.theme;

	/* Settings */
	uint16_t margin = 10;
	uint16_t padding = 2;
	uint16_t rect_height = theme->osd_workspace_switcher_boxes_height;
	uint16_t rect_width = theme->osd_workspace_switcher_boxes_width;
	bool hide_boxes = theme->osd_workspace_switcher_boxes_width == 0 ||
		theme->osd_workspace_switcher_boxes_height == 0;

	/* Dimensions */
	size_t workspace_count = wl_list_length(&server.workspaces.all);
	uint16_t marker_width = workspace_count * (rect_width + padding) - padding;
	uint16_t width = margin * 2 + (marker_width < 200 ? 200 : marker_width);
	uint16_t height = margin * (hide_boxes ? 2 : 3) + rect_height + font_height(&rc.font_osd);

	cairo_t *cairo;
	cairo_surface_t *surface;
	struct workspace *workspace;

	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		if (!output_is_usable(output)) {
			continue;
		}
		struct lab_data_buffer *buffer = buffer_create_cairo(width, height,
			output->wlr_output->scale);
		if (!buffer) {
			wlr_log(WLR_ERROR, "Failed to allocate buffer for workspace OSD");
			continue;
		}

		cairo = cairo_create(buffer->surface);

		int bw = theme->osd_border_width;
		/* Background */
		set_cairo_color(cairo, theme->osd_bg_color);
		cairo_rectangle(cairo, bw, bw, width-bw*2, height-bw*2);
		cairo_fill(cairo);

		/* Border */
		if (theme->osd_border_type) {
			cairo_borders(cairo, 0, 0, width, height, bw,
				theme->osd_highlight, theme->osd_shadow, theme->osd_border_type,
				theme->osd_border_bevel_width, theme->osd_border_color);
			set_cairo_color(cairo, theme->osd_border_color);
		} else {
			set_cairo_color(cairo, theme->osd_border_color);
			struct wlr_fbox border_fbox = {
				.width = width,
				.height = height,
				};
			draw_cairo_border(cairo, border_fbox, theme->osd_border_width);
		}

		/* Boxes */
		uint16_t x;
		if (!hide_boxes) {
			x = (width - marker_width) / 2;
			wl_list_for_each(workspace, &server.workspaces.all, link) {
				bool active =  workspace == server.workspaces.current;
				set_cairo_color(cairo, rc.theme->osd_label_text_color);
				struct wlr_fbox fbox = {
					.x = x,
					.y = margin,
					.width = rect_width,
					.height = rect_height,
				};
				draw_cairo_border(cairo, fbox,
					theme->osd_workspace_switcher_boxes_border_width);
				if (active) {
					cairo_rectangle(cairo, x, margin,
						rect_width, rect_height);
					cairo_fill(cairo);
				}
				x += rect_width + padding;
			}
		}

		/* Text */
		set_cairo_color(cairo, rc.theme->osd_label_text_color);
		PangoLayout *layout = pango_cairo_create_layout(cairo);
		pango_context_set_round_glyph_positions(pango_layout_get_context(layout), false);
		pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

		/* Center workspace indicator on the x axis */
		int req_width = font_width(&rc.font_osd, server.workspaces.current->name);
		req_width = MIN(req_width, width - 2 * margin);
		x = (width - req_width) / 2;
		if (!hide_boxes) {
			cairo_move_to(cairo, x, margin * 2 + rect_height);
		} else {
			cairo_move_to(cairo, x, (height - font_height(&rc.font_osd)) / 2.0);
		}
		PangoFontDescription *desc = font_to_pango_desc(&rc.font_osd);
		//pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
		pango_layout_set_font_description(layout, desc);
		pango_layout_set_width(layout, req_width * PANGO_SCALE);
		pango_font_description_free(desc);
		pango_layout_set_text(layout, server.workspaces.current->name, -1);
		pango_cairo_show_layout(cairo, layout);

		g_object_unref(layout);
		surface = cairo_get_target(cairo);
		cairo_surface_flush(surface);
		cairo_destroy(cairo);

		if (!output->workspace_osd) {
			output->workspace_osd = lab_wlr_scene_buffer_create(
				&server.scene->tree, NULL);
		}
		/* Position the whole thing */
		struct wlr_box output_box;
		wlr_output_layout_get_box(server.output_layout,
			output->wlr_output, &output_box);
		int lx = output_box.x + (output_box.width - width) / 2;
		int ly = output_box.y + (output_box.height - height) / 2;
		wlr_scene_node_set_position(&output->workspace_osd->node, lx, ly);
		wlr_scene_buffer_set_buffer(output->workspace_osd, &buffer->base);
		wlr_scene_buffer_set_dest_size(output->workspace_osd,
			buffer->logical_width, buffer->logical_height);

		/* And finally drop the buffer so it will get destroyed on OSD hide */
		wlr_buffer_drop(&buffer->base);
	}
}

static struct workspace *
workspace_find_by_name(const char *name)
{
	struct workspace *workspace;

	/* by index */
	size_t parsed_index = parse_workspace_index(name);
	if (parsed_index) {
		size_t index = 0;
		wl_list_for_each(workspace, &server.workspaces.all, link) {
			if (parsed_index == ++index) {
				return workspace;
			}
		}
	}

	/* by name */
	wl_list_for_each(workspace, &server.workspaces.all, link) {
		if (!strcmp(workspace->name, name)) {
			return workspace;
		}
	}

	wlr_log(WLR_ERROR, "Workspace '%s' not found", name);
	return NULL;
}

static void
handle_ext_workspace_commit(struct wl_listener *listener, void *data)
{
	struct wlr_ext_workspace_v1_commit_event *event = data;

	struct wlr_ext_workspace_v1_request *req;
	wl_list_for_each(req, event->requests, link) {
		if (req->type == WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE) {
			struct workspace *workspace = req->activate.workspace->data;
			workspaces_switch_to(workspace, /* update_focus */ true);
			wlr_log(WLR_INFO, "activating workspace %s", workspace->name);
		}
	}
}

/* Internal API */
static void
add_workspace(const char *name)
{
	struct workspace *workspace = znew(*workspace);
	workspace->name = xstrdup(name);
	workspace->tree = lab_wlr_scene_tree_create(server.workspace_tree);
	workspace->view_trees[VIEW_LAYER_ALWAYS_ON_BOTTOM] =
		lab_wlr_scene_tree_create(workspace->tree);
	workspace->view_trees[VIEW_LAYER_NORMAL] =
		lab_wlr_scene_tree_create(workspace->tree);
	workspace->view_trees[VIEW_LAYER_ALWAYS_ON_TOP] =
		lab_wlr_scene_tree_create(workspace->tree);
	wl_list_append(&server.workspaces.all, &workspace->link);
	wlr_scene_node_set_enabled(&workspace->tree->node, false);

	workspace->ext_workspace = wlr_ext_workspace_handle_v1_create(
		server.workspaces.ext_manager, /*id*/ NULL,
		EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE);
	workspace->ext_workspace->data = workspace;
	wlr_ext_workspace_handle_v1_set_group(
		workspace->ext_workspace, server.workspaces.ext_group);
	wlr_ext_workspace_handle_v1_set_name(workspace->ext_workspace, name);
}

static struct workspace *
get_prev(struct workspace *current, struct wl_list *workspaces, bool wrap)
{
	struct wl_list *target_link = current->link.prev;
	if (target_link == workspaces) {
		/* Current workspace is the first one */
		if (!wrap) {
			return NULL;
		}
		/* Roll over */
		target_link = target_link->prev;
	}
	return wl_container_of(target_link, current, link);
}

static struct workspace *
get_next(struct workspace *current, struct wl_list *workspaces, bool wrap)
{
	struct wl_list *target_link = current->link.next;
	if (target_link == workspaces) {
		/* Current workspace is the last one */
		if (!wrap) {
			return NULL;
		}
		/* Roll over */
		target_link = target_link->next;
	}
	return wl_container_of(target_link, current, link);
}

static bool
workspace_has_views(struct workspace *workspace)
{
	struct view *view;

	for_each_view(view, &server.views, LAB_VIEW_CRITERIA_NO_OMNIPRESENT) {
		if (view->workspace == workspace) {
			return true;
		}
	}
	return false;
}

static struct workspace *
get_adjacent_occupied(struct workspace *current, struct wl_list *workspaces,
		bool wrap, bool reverse)
{
	struct wl_list *start = &current->link;
	struct wl_list *link = reverse ? start->prev : start->next;
	bool has_wrapped = false;

	while (true) {
		/* Handle list boundaries */
		if (link == workspaces) {
			if (!wrap) {
				break;  /* No wrapping allowed - stop searching */
			}
			if (has_wrapped) {
				break;  /* Already wrapped once - stop to prevent infinite loop */
			}
			/* Wrap around */
			link = reverse ? workspaces->prev : workspaces->next;
			has_wrapped = true;
			continue;
		}

		/* Get the workspace */
		struct workspace *target = wl_container_of(link, target, link);

		/* Check if we've come full circle */
		if (link == start) {
			break;
		}

		/* Check if it's occupied (and not current) */
		if (target != current && workspace_has_views(target)) {
			return target;
		}

		/* Move to next/prev */
		link = reverse ? link->prev : link->next;
	}

	return NULL;  /* No occupied workspace found */
}

static struct workspace *
get_prev_occupied(struct workspace *current, struct wl_list *workspaces, bool wrap)
{
	return get_adjacent_occupied(current, workspaces, wrap, true);
}

static struct workspace *
get_next_occupied(struct workspace *current, struct wl_list *workspaces, bool wrap)
{
	return get_adjacent_occupied(current, workspaces, wrap, false);
}

static int
_osd_handle_timeout(void *data)
{
	struct seat *seat = data;
	workspaces_osd_hide(seat);
	/* Don't re-check */
	return 0;
}

static void
_osd_show(void)
{
	if (!rc.workspace_config.popuptime) {
		return;
	}

	_osd_update();
	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		if (output_is_usable(output) && output->workspace_osd) {
			wlr_scene_node_set_enabled(&output->workspace_osd->node, true);
		}
	}
	if (keyboard_get_all_modifiers(&server.seat)) {
		/* Hidden by release of all modifiers */
		server.seat.workspace_osd_shown_by_modifier = true;
	} else {
		/* Hidden by timer */
		if (!server.seat.workspace_osd_timer) {
			server.seat.workspace_osd_timer = wl_event_loop_add_timer(
				server.wl_event_loop, _osd_handle_timeout, &server.seat);
		}
		wl_event_source_timer_update(server.seat.workspace_osd_timer,
			rc.workspace_config.popuptime);
	}
}

/* Public API */
void
workspaces_init(void)
{
	server.workspaces.ext_manager = wlr_ext_workspace_manager_v1_create(
		server.wl_display, EXT_WORKSPACES_VERSION);

	server.workspaces.ext_group = wlr_ext_workspace_group_handle_v1_create(
		server.workspaces.ext_manager, /*caps*/ 0);

	server.workspaces.on_ext_manager.commit.notify = handle_ext_workspace_commit;
	wl_signal_add(&server.workspaces.ext_manager->events.commit,
		&server.workspaces.on_ext_manager.commit);

	wl_list_init(&server.workspaces.all);

	struct workspace_config *conf;
	wl_list_for_each(conf, &rc.workspace_config.workspaces, link) {
		add_workspace(conf->name);
	}

	/*
	 * After adding workspaces, check if there is an initial workspace
	 * selected and set that as the initial workspace.
	 */
	char *initial_name = rc.workspace_config.initial_workspace_name;
	struct workspace *initial = NULL;
	struct workspace *first = wl_container_of(
		server.workspaces.all.next, first, link);

	if (initial_name) {
		initial = workspace_find_by_name(initial_name);
	}
	if (!initial) {
		initial = first;
	}

	server.workspaces.current = initial;
	wlr_scene_node_set_enabled(&initial->tree->node, true);
	wlr_ext_workspace_handle_v1_set_active(initial->ext_workspace, true);
}

/*
 * update_focus should normally be set to true. It is set to false only
 * when this function is called from desktop_focus_view(), in order to
 * avoid unnecessary extra focus changes and possible recursion.
 */
void
workspaces_switch_to(struct workspace *target, bool update_focus)
{
	assert(target);
	if (target == server.workspaces.current) {
		return;
	}

	/* Disable the old workspace */
	wlr_scene_node_set_enabled(
		&server.workspaces.current->tree->node, false);

	wlr_ext_workspace_handle_v1_set_active(
		server.workspaces.current->ext_workspace, false);

	/*
	 * Move Omnipresent views to new workspace.
	 * Not using for_each_view() since it skips views that
	 * view_is_focusable() returns false (e.g. Conky).
	 */
	struct view *view;
	wl_list_for_each_reverse(view, &server.views, link) {
		if (view->visible_on_all_workspaces) {
			view_move_to_workspace(view, target);
		}
	}

	/* Enable the new workspace */
	wlr_scene_node_set_enabled(&target->tree->node, true);

	/* Save the last visited workspace */
	server.workspaces.last = server.workspaces.current;

	/* Make sure new views will spawn on the new workspace */
	server.workspaces.current = target;

	struct view *grabbed_view = server.grabbed_view;
	if (grabbed_view) {
		view_move_to_workspace(grabbed_view, target);
	}

	/*
	 * Make sure we are focusing what the user sees. Only refocus if
	 * the focus is not already on an omnipresent view.
	 */
	if (update_focus) {
		struct view *active_view = server.active_view;
		if (!(active_view && active_view->visible_on_all_workspaces)) {
			desktop_focus_topmost_view();
		}
	}

	/* And finally show the OSD */
	_osd_show();

	/*
	 * Make sure we are not carrying around a
	 * cursor image from the previous desktop
	 */
	cursor_update_focus();

	/* Ensure that only currently visible fullscreen windows hide the top layer */
	desktop_update_top_layer_visibility();

	wlr_ext_workspace_handle_v1_set_active(target->ext_workspace, true);

	show_desktop_reset();
}

void
workspaces_osd_hide(struct seat *seat)
{
	assert(seat);
	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		if (!output->workspace_osd) {
			continue;
		}
		wlr_scene_node_set_enabled(&output->workspace_osd->node, false);
		wlr_scene_buffer_set_buffer(output->workspace_osd, NULL);
	}
	seat->workspace_osd_shown_by_modifier = false;

	/* Update the cursor focus in case it was on top of the OSD before */
	cursor_update_focus();
}

struct workspace *
workspaces_find(struct workspace *anchor, const char *name, bool wrap)
{
	assert(anchor);
	if (!name) {
		return NULL;
	}
	struct wl_list *workspaces = &server.workspaces.all;

	if (!strcasecmp(name, "current")) {
		return anchor;
	} else if (!strcasecmp(name, "last")) {
		return server.workspaces.last;
	} else if (!strcasecmp(name, "left")) {
		return get_prev(anchor, workspaces, wrap);
	} else if (!strcasecmp(name, "right")) {
		return get_next(anchor, workspaces, wrap);
	} else if (!strcasecmp(name, "left-occupied")) {
		return get_prev_occupied(anchor, workspaces, wrap);
	} else if (!strcasecmp(name, "right-occupied")) {
		return get_next_occupied(anchor, workspaces, wrap);
	}
	return workspace_find_by_name(name);
}

static void
destroy_workspace(struct workspace *workspace)
{
	wlr_scene_node_destroy(&workspace->tree->node);
	zfree(workspace->name);
	wl_list_remove(&workspace->link);

	wlr_ext_workspace_handle_v1_destroy(workspace->ext_workspace);
	free(workspace);
}

void
workspaces_reconfigure(void)
{
	/*
	 * Compare actual workspace list with the new desired configuration to:
	 *   - Update names
	 *   - Add workspaces if more workspaces are desired
	 *   - Destroy workspaces if fewer workspace are desired
	 */

	struct wl_list *workspace_link = server.workspaces.all.next;

	struct workspace_config *conf;
	wl_list_for_each(conf, &rc.workspace_config.workspaces, link) {
		struct workspace *workspace = wl_container_of(
			workspace_link, workspace, link);

		if (workspace_link == &server.workspaces.all) {
			/* # of configured workspaces increased */
			wlr_log(WLR_DEBUG, "Adding workspace \"%s\"",
				conf->name);
			add_workspace(conf->name);
			continue;
		}
		if (strcmp(workspace->name, conf->name)) {
			/* Workspace is renamed */
			wlr_log(WLR_DEBUG, "Renaming workspace \"%s\" to \"%s\"",
				workspace->name, conf->name);
			xstrdup_replace(workspace->name, conf->name);
			wlr_ext_workspace_handle_v1_set_name(
				workspace->ext_workspace, workspace->name);
		}
		workspace_link = workspace_link->next;
	}

	if (workspace_link == &server.workspaces.all) {
		return;
	}

	/* # of configured workspaces decreased */
	overlay_finish(&server.seat);
	struct workspace *first_workspace =
		wl_container_of(server.workspaces.all.next, first_workspace, link);

	while (workspace_link != &server.workspaces.all) {
		struct workspace *workspace = wl_container_of(
			workspace_link, workspace, link);

		wlr_log(WLR_DEBUG, "Destroying workspace \"%s\"",
			workspace->name);

		struct view *view;
		wl_list_for_each(view, &server.views, link) {
			if (view->workspace == workspace) {
				view_move_to_workspace(view, first_workspace);
			}
		}

		if (server.workspaces.current == workspace) {
			workspaces_switch_to(first_workspace,
				/* update_focus */ true);
		}
		if (server.workspaces.last == workspace) {
			server.workspaces.last = first_workspace;
		}

		workspace_link = workspace_link->next;
		destroy_workspace(workspace);
	}
}

void
workspaces_destroy(void)
{
	struct workspace *workspace, *tmp;
	wl_list_for_each_safe(workspace, tmp, &server.workspaces.all, link) {
		destroy_workspace(workspace);
	}
	assert(wl_list_empty(&server.workspaces.all));
	wl_list_remove(&server.workspaces.on_ext_manager.commit.link);
}
