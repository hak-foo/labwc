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
#include <drm_fourcc.h>
#include <wlr/types/wlr_ext_workspace_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/render/allocator.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_scene.h>
#include "buffer.h"
#include "common/font.h"
#include "common/graphic-helpers.h"
#include "scaled-buffer/scaled-icon-buffer.h"
#include "scaled-buffer/scaled-buffer.h"
#include "common/list.h"
#include "common/mem.h"
#include "common/scene-helpers.h"
#include "config/rcxml.h"
#include "input/keyboard.h"
#include "common/borderset.h"
#include "labwc.h"
#include "output.h"
#include "theme.h"
#include "view.h"
#include "regions.h"
#include "node.h"
#include "cycle.h"
#include "pager.h"

float pager_drag_start_x, pager_drag_start_y;

struct view *active_drag_view;

unsigned char *pixel_data;

struct thumbnail_cache *thumb_cache;

struct wlr_fbox thumbnail_size(struct view *view, int wscount)
{
	struct wlr_box overall_box = { 0 };
	wlr_output_layout_get_box(server.output_layout,
		NULL, &overall_box);
	int pagerwidth = rc.pager_width - 2* rc.theme->pager_border_width;
	int total_pagerheight = rc.pager_height - 2 * rc.theme->pager_border_width;
	int pagerheight = ceil((float)total_pagerheight /
		wl_list_length(&rc.workspace_config.workspaces));

	int screenwidth = overall_box.width - overall_box.x;
	int screenheight = overall_box.height - overall_box.y;

	struct theme *theme = rc.theme;
	int wx = view->current.x * pagerwidth / screenwidth;
	int wy = view->current.y * pagerheight / screenheight +
		pagerheight * wscount;
	int width = view->current.width * pagerwidth / screenwidth;
	int height = view->current.height *
		pagerheight / screenheight;
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

	// Shaded windows are shown as 1px high
	if (view->shaded) {
		if (rc.rotated_title) {
			width = 1;
		} else {
			height = 1;
		}
	}

	// Crop to current frame
	height = MIN(height, (wscount+1) * pagerheight - wy);
	// Crop to bottom edge for odd sizes where the
	// last frame is marginally smaller
	height = MIN(height, (total_pagerheight-1 - wy));

	if (height < 1 + 2 * (view->minimized ?
			theme->pager_minimized_window_border_width :
			theme->pager_window_border_width)) {
		height = 1 + 2 * (view->minimized ?
			theme->pager_minimized_window_border_width :
			theme->pager_window_border_width);
		wy = MIN(wy, (wscount+1) * pagerheight-height);
	}

	if (rc.rotated_title) {
		if (width < 1 + 2 * (view->minimized ?
				theme->pager_minimized_window_border_width :
				theme->pager_window_border_width)) {
			width = 1 + 2 * (view->minimized ?
				theme->pager_minimized_window_border_width :
				theme->pager_window_border_width);
			wx = MIN(wx, pagerwidth - width);
		}
	}

	struct wlr_fbox border_fbox = {
		.x = wx,
		.y = wy,
		.width = width,
		.height = height,
		};
	return border_fbox;
}

void process_pager_move(float sx, float sy, struct view *found_view)
{
	
	int pagerwidth = rc.pager_width - 2* rc.theme->pager_border_width;
	int total_pagerheight = rc.pager_height - 2 * rc.theme->pager_border_width;
	int pagerheight = ceil((float)total_pagerheight /
		wl_list_length(&rc.workspace_config.workspaces));

	struct wlr_box overall_box = { 0 };
	wlr_output_layout_get_box(server.output_layout,
		NULL, &overall_box);

	int screenwidth = overall_box.width - overall_box.x;
	int screenheight = overall_box.height - overall_box.y;

	// sx/sy and pager_drag_start_x/y are seat-wide coordinates
	// so if we need to find the right "workspace" we need to offset it
	// by the position the pager lives in and its border.
	// The other calculations are relative and don't care.
	float adj_pager_drag_start_y = pager_drag_start_y - rc.pager_y - rc.theme->pager_border_width;
	sy -= rc.pager_y;
	sy -= rc.theme->pager_border_width;

	if (found_view) {
		int old_workspace = adj_pager_drag_start_y / pagerheight;
		int new_workspace = sy / pagerheight;
		
		if (old_workspace == new_workspace) {
			int delta_x = (sx - pager_drag_start_x) * ((float)screenwidth / (float)pagerwidth);
			int delta_y = (sy - adj_pager_drag_start_y) * ((float)screenheight / (float)pagerheight);
			view_move_relative(found_view, delta_x, delta_y);
		} else {
			int delta_x = (sx - pager_drag_start_x) * screenwidth / pagerwidth;
			int delta_y = (((int)sy % pagerheight) -
				((int)adj_pager_drag_start_y % pagerheight)) *
				screenheight / pagerheight;
			int target_workspace = sy/pagerheight;
			int wscount = 0;
			struct workspace *workspace;
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

void process_pager_release(void)
{
	active_drag_view = NULL;
	pager_update();
}

void process_pager_drag(float sx, float sy)
{
	if (sx < rc.pager_x ||
		sy < rc.pager_y ||
		sx > rc.pager_x + rc.pager_width ||
		sy > rc.pager_y + rc.pager_height) {
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

// Pressing an icon in the pager will tag it for drag actions.
void 
process_pager_window_press(float sx, float sy, struct view *found_view)
{
	if (found_view) {
		pager_drag_start_x = sx;
		pager_drag_start_y = sy;
		pager_update();
	}
	
	active_drag_view = found_view;
}	

// A click on the pager itself will focus the selected workspace
void
process_pager_press(float sx, float sy)
{
	if (sx < rc.theme->pager_border_width ||
		sy < rc.theme->pager_border_width ||
		sx > rc.pager_width - rc.theme->pager_border_width ||
		sy > rc.pager_height - rc.theme->pager_border_width) {
		return;
	}
	sx -= rc.theme->pager_border_width;
	sy -= rc.theme->pager_border_width;

	int total_pagerheight = rc.pager_height - 2 * rc.theme->pager_border_width;
	int pagerheight = ceil((float)total_pagerheight /
		wl_list_length(&rc.workspace_config.workspaces));
	int wscount = 0;
	int target_workspace = sy/pagerheight;

	struct workspace *workspace;
	wl_list_for_each(workspace, &server.workspaces.all, link) {
		if (wscount == target_workspace) {
			workspaces_switch_to(workspace, /* update_focus */ true);
			pager_update();
			break;
		}
		wscount++;
	}	
}

static void
render_node_sized(struct wlr_render_pass *pass,
		struct wlr_scene_node *node, int x, int y, float sx, float sy)
{
	switch (node->type) {
	case WLR_SCENE_NODE_TREE: {
		struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &tree->children, link) {
			render_node_sized(pass, child, sx*(x + node->x), sy*(y + node->y), sx, sy);
		}
		break;
	}
	case WLR_SCENE_NODE_BUFFER: {
		struct wlr_scene_buffer *scene_buffer =
			wlr_scene_buffer_from_node(node);
		if (!scene_buffer->buffer) {
			break;
		}
		struct wlr_texture *texture = wlr_texture_from_buffer(
			server.renderer, scene_buffer->buffer);
		if (!texture) {
			break;
		}
		wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
			.texture = texture,
			.src_box = scene_buffer->src_box,
			.dst_box = {
				.x = x * sx,
				.y = y * sy,
				.width = scene_buffer->dst_width*sx,
				.height = scene_buffer->dst_height*sy
			},
			.transform = scene_buffer->transform,
		});
		wlr_texture_destroy(texture);
		break;
	}
	case WLR_SCENE_NODE_RECT:
		/* should be unreached */
		wlr_log(WLR_ERROR, "ignoring rect");
		break;
	}
}

static struct wlr_buffer *
render_thumb_sized(struct output *output, struct view *view, float sx, float sy)
{
	if (!view->content_tree) {
		/*
		 * Defensive. Could possibly occur if view was unmapped
		 * with OSD already displayed.
		 */
		return NULL;
	}
	struct wlr_buffer *buffer = wlr_allocator_create_buffer(server.allocator,
		sx*view->current.width, sy*view->current.height,
		&output->wlr_output->swapchain->format);
		
	if(!buffer) {
		return NULL;
	}

	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		server.renderer, buffer, NULL);
	render_node_sized(pass, &view->content_tree->node, 0, 0, sx, sy);
	if (!wlr_render_pass_submit(pass)) {
		wlr_log(WLR_ERROR, "failed to submit render pass");
		wlr_buffer_drop(buffer);
		return NULL;
	}
	return buffer;
}

// Flush a specific view from the cache.  This allows actual window
// changes to reflect quickly, and a long-running cache so eventually
// maybe we'll refresh slow windows not being interacted with
void pager_flush(struct view *view)
{
	// Sometimes, seems like after Focus events on
	// tray icons, view is null, so bail before trying
	// to access it and segfaulting
	if (!view) {
		return;
	}
	struct thumbnail_cache *pointer = thumb_cache;
	struct thumbnail_cache *old = NULL, *next = NULL;
	while (pointer) {
		next = pointer->next;
		// We'll drop a cached thumbnail after this many re-renderings of the pager.
		// We could also flush after a change occured to a specific window.
		if (pointer->creation_id == view->creation_id) {
			// If we're expiring the first entry
			// update the start of the cache list for everyone
			if (pointer == thumb_cache) {
				thumb_cache = next;
			}
			// Clear old cache
			free(pointer->thumbnail);
			free(pointer);
			// Stitch the list over the removed item.
			if (old) {
				old->next = next;
			}
			// Skip for this use only since we just freed it
			pointer = next;
			continue;
		}
		old = pointer;
		pointer = next;
	}
}

unsigned char *get_thumbnail_cache(
	struct output *output,
	struct view *view,
	struct wlr_fbox border_fbox)
{
	struct thumbnail_cache *pointer = thumb_cache;
	struct thumbnail_cache *old = NULL, *next = NULL;
	while (pointer) {
		next = pointer->next;
		pointer->age++;
		// We'll drop a cached thumbnail after this many re-renderings of the pager.
		// We could also flush after a change occured to a specific window.
		if (pointer->age > 10000) {
			// If we're expiring the first entry
			// update the start of the cache list for everyone
			if (pointer == thumb_cache) {
				thumb_cache = next;
			}
			// Clear old cache
			free(pointer->thumbnail);
			free(pointer);
			// Stitch the list over the removed item.
			if (old) {
				old->next = next;
			}
			// Skip for this use only since we just freed it
			pointer = next;
			continue;
		}
		if (pointer
			&& pointer->creation_id == view->creation_id
			&& pointer->width == border_fbox.width
			&& pointer->height == border_fbox.height) {
			return pointer->thumbnail;
		}
		old = pointer;
		pointer = next;
	}
	struct thumbnail_cache *new_entry = malloc(sizeof(struct thumbnail_cache));
	new_entry->creation_id = view->creation_id;
	new_entry->age = 0;
	new_entry->next = NULL;
	new_entry->width = border_fbox.width;
	new_entry->height = border_fbox.height;

	struct wlr_buffer *thumb_buffer =
		render_thumb_sized(output, view,
			border_fbox.width /
				view->current.width,
			border_fbox.height /
				view->current.height);
	if (!thumb_buffer) {
		return NULL;
	}
	struct wlr_texture *thumb_texture =
		wlr_texture_from_buffer(server.renderer,
			thumb_buffer);
	new_entry->thumbnail = malloc(4*border_fbox.width*border_fbox.height);
	struct wlr_texture_read_pixels_options options = {
		.data = new_entry->thumbnail,
		.stride = 4*border_fbox.width,
		.dst_x = 0,
		.dst_y = 0,
		.format = DRM_FORMAT_ARGB8888
	};
	wlr_texture_read_pixels(thumb_texture,
		&options);
	wlr_texture_destroy(thumb_texture);
	wlr_buffer_drop(thumb_buffer);
	if (old) {
		old->next = new_entry;
	} else {
		thumb_cache = new_entry;
	}
	return new_entry->thumbnail;
}
	
void pager_update(void)
{
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
	int total_pagerheight = rc.pager_height - theme->pager_border_width * 2;
	int x = rc.pager_x;
	int y = rc.pager_y;

	struct workspace *workspace;
	struct view *view;
	struct wlr_box overall_box = { 0 };
	wlr_output_layout_get_box(server.output_layout,
		NULL, &overall_box);

	// Divide pager among workspaces vertically
	int pagerheight = ceil((float)total_pagerheight /
		wl_list_length(&rc.workspace_config.workspaces));

	int font_h = font_height(&rc.font_pager);

	wl_list_for_each(output, &server.outputs, link) {
		if (!output_is_usable(output)) {
			continue;
		}
		if (output->pager_osd) {
			// Kill off all the icon images
			struct wlr_scene_node *node, *tmpnode;
			wl_list_for_each_safe(node, tmpnode, &output->pager_osd->children, link) {	
				wlr_scene_node_destroy(node);
			}
		}
		if (!output->pager_osd) {
			output->pager_osd = lab_wlr_scene_tree_create(
				&server.scene->tree);
			node_descriptor_create(&output->pager_osd->node, LAB_NODE_PAGER, NULL, 0);
		}

		struct lab_data_buffer *buffer = buffer_create_cairo(rc.pager_width,
			rc.pager_height, output->wlr_output->scale);
		if (!buffer) {
			wlr_log(WLR_ERROR, "Failed to allocate buffer for pager");
			continue;
		}
		
		struct wlr_scene_buffer	*pager_backdrop = 
			lab_wlr_scene_buffer_create(output->pager_osd, &buffer->base);
		wlr_scene_buffer_set_dest_size(pager_backdrop,
			rc.pager_width, rc.pager_height);

		cairo_t *cairo;
		cairo_surface_t *surface;
		cairo = cairo_create(buffer->surface);
		int wscount = 0;
		wl_list_for_each(workspace, &server.workspaces.all, link) {
			/* Background */
			set_cairo_color(cairo, server.workspaces.current == workspace ?
				theme->pager_color_active : theme->pager_color_inactive);
			cairo_rectangle(cairo, theme->pager_border_width,
				theme->pager_border_width +
				pagerheight * wscount, pagerwidth, pagerheight);
			cairo_fill(cairo);


			for_each_view_reverse(view, &server.views, LAB_VIEW_CRITERIA_NONE) {
				if (view->workspace == workspace) {
					// Determine dimensions for mini-window (common)
					struct wlr_fbox border_fbox =
						thumbnail_size(view, wscount);
					// Prepare border/caption styles that differ
					// for minimized/normal windows
					float *bc =
						view->minimized ?
						theme->pager_color_minimized_window :
						theme->pager_color_window;

					int bw =
						view->minimized ?
						theme->pager_minimized_window_border_width :
						theme->pager_window_border_width;

					int highlight =
						view->minimized ?
						theme->pager_minimized_window_highlight :
						theme->pager_window_highlight;

					int shadow =
						view->minimized ?
						theme->pager_minimized_window_shadow :
						theme->pager_window_shadow;

					enum border_type bt =
						view->minimized ?
						theme->pager_minimized_window_border_type :
						theme->pager_window_border_type;

					int bvw =
						view->minimized ?
						theme->pager_minimized_window_bevel_width :
						theme->pager_window_bevel_width;

					float *tc =
						view->minimized ?
						theme->pager_color_minimized_window_title :
						theme->pager_color_window_title;

					struct lab_data_buffer *thumb_buffer = buffer_create_cairo(
					border_fbox.width, border_fbox.height,
						output->wlr_output->scale);
					struct wlr_scene_buffer	*base_scene = 
						lab_wlr_scene_buffer_create(output->pager_osd, &thumb_buffer->base);
						
					cairo_t *thumb_cairo;
					thumb_cairo = cairo_create(thumb_buffer->surface);

					// Only generate a thumbnail if we are configured for
					// them and it will produce a positive size
					
					if (rc.pager_thumbnail &&
						view->current.width > 0 &&
						view->current.height > 0 &&
						border_fbox.width > 0 &&
						border_fbox.height > 0) {
						pixel_data = get_thumbnail_cache(output, view,
							border_fbox);
					} else {
						pixel_data = NULL;
					}

					// If we have a thumbnail, render it.
					if (pixel_data) {
						cairo_surface_t *snapshot_surface =
							cairo_image_surface_create_for_data(
								pixel_data, CAIRO_FORMAT_ARGB32,
								border_fbox.width,
								border_fbox.height,
								4*border_fbox.width);
						cairo_set_source_surface(thumb_cairo,
							snapshot_surface,
							0, 0);
						cairo_rectangle(thumb_cairo,
							0, 0,
							border_fbox.width,
							border_fbox.height);
						cairo_fill(thumb_cairo);
						cairo_surface_destroy(snapshot_surface);
					} else {
						// If we're not rendering a thumbnail, use rules for
						// box-based windows:
						set_cairo_color(thumb_cairo, bc);
						cairo_rectangle(thumb_cairo,
							0, 0,
							border_fbox.width,
							border_fbox.height);
						cairo_fill(thumb_cairo);
					}

					// Experiment:
					// Add borders on both highlighted and normal windows
					// so they're easier to identify the edges and identifies
					// minimized windows
					cairo_borders(thumb_cairo,
						0, 0,
						border_fbox.width,
						border_fbox.height,
						bw, highlight, shadow,
						bt, bvw, bc);

					// Only draw a title if the window
					// is big enough for it and isn't a thumbnail
					if (!pixel_data &&
						border_fbox.height >= font_h + 6) {
						PangoLayout *layout =
							pango_cairo_create_layout(thumb_cairo);
						pango_context_set_round_glyph_positions(
							pango_layout_get_context(layout),
							false);
						pango_layout_set_ellipsize(
							layout,
							PANGO_ELLIPSIZE_END);
						int req_width = font_width(
							&rc.font_pager,
							view->title);
						PangoFontDescription *desc =
							font_to_pango_desc(
								&rc.font_pager);

						set_cairo_color(thumb_cairo, tc);

						req_width = MIN(req_width,
							border_fbox.width -
							2 * bw -2
						);

						cairo_move_to(thumb_cairo,
							(border_fbox.width
									- req_width) / 2,
							(border_fbox.height
									- font_h) / 2);
							pango_layout_set_font_description(
								layout,
								desc);
							pango_layout_set_width(layout,
								req_width * PANGO_SCALE);
							pango_font_description_free(desc);
							pango_layout_set_text(layout,
								view->title, -1);
							pango_cairo_show_layout(thumb_cairo,
								layout);
							g_object_unref(layout);
					}
					cairo_destroy(thumb_cairo);
					wlr_scene_buffer_set_dest_size(base_scene,
						border_fbox.width, border_fbox.height);
					wlr_scene_node_set_position(&base_scene->node,
						x + theme->pager_border_width
							+ border_fbox.x,
						y + theme->pager_border_width
							+border_fbox.y);
					node_descriptor_create(&base_scene->node, LAB_NODE_PAGER_WINDOW, view, 0);
					wlr_buffer_drop(&thumb_buffer->base);
				}
				
			}
			wscount++;
		}
		cairo_borders(cairo, 0, 0, rc.pager_width,
					rc.pager_height, theme->pager_border_width,
					theme->pager_highlight, theme->pager_shadow,
					theme->pager_border_type, theme->pager_bevel_width,
					theme->pager_color_border);
		surface = cairo_get_target(cairo);
		cairo_surface_flush(surface);
		cairo_destroy(cairo);

		
		
		wlr_scene_node_set_enabled(&output->pager_osd->node, true);

		wlr_scene_node_set_position(&pager_backdrop->node, x, y);
		wlr_scene_buffer_set_buffer(pager_backdrop, &buffer->base);
		wlr_scene_buffer_set_dest_size(pager_backdrop,
			buffer->logical_width, buffer->logical_height);
		wlr_scene_node_place_below(&output->pager_osd->node, &output->layer_tree[1]->node);
		wlr_buffer_drop(&buffer->base);
	}
}
