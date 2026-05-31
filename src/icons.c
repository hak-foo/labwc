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
#include "icons.h"

float icon_drag_start_x, icon_drag_start_y;

struct view *active_drag_icon;


void process_icon_release(void)
{
	active_drag_icon = NULL;
	icons_update();
}

void process_icon_move(float sx, float sy, struct view *found_view)
{
	if (found_view) {
		float delta_x = (sx - icon_drag_start_x);
		float delta_y = (sy - icon_drag_start_y);
		found_view->icon_x = delta_x;
		found_view->icon_y = delta_y;
		icons_update();
	}
}

void process_icon_drag(float sx, float sy)
{
	if (active_drag_icon)  {
		process_icon_move(sx, sy, active_drag_icon);		
	}
}

void process_icon_press(float sx, float sy, struct view *found_view)
{
	icon_drag_start_x = sx - found_view->icon_x;
	icon_drag_start_y = sy - found_view->icon_y;
	active_drag_icon = found_view;
}


void icons_update(void)
{
	struct output *output;
	if (!rc.icons_enabled) {
		wl_list_for_each(output, &server.outputs, link) {
			if (!output_is_usable(output)) {
				continue;
			}
			if (output->icons_osd) {
				wlr_scene_node_set_enabled(&output->icons_osd->node, false);
			}
		}
		return;
	}
	
	if (wl_list_empty(&rc.workspace_config.workspaces)) {
		return;
	}
	struct theme *theme = rc.theme;
	

	int x_spacing = rc.icon_x_spacing;
	int y_spacing = rc.icon_y_spacing;
	int left_offset = rc.icon_left_offset;
	int bottom_offset = rc.icon_bottom_offset;
	int icon_width = rc.icon_width;
	int icon_height = rc.icon_height;
	int graphic_size = rc.icon_graphic_size;
	
	
	
	
	struct workspace *workspace;
	struct view *view;
	struct wlr_box overall_box = { 0 };
	wlr_output_layout_get_box(server.output_layout,
		NULL, &overall_box);
	int screenwidth = overall_box.width - overall_box.x - left_offset;
	// Trim the bottom row off the screen height to ensure we always stay on screen
	int screenheight = overall_box.height - overall_box.y - bottom_offset - y_spacing;
	
	int rows = screenheight / y_spacing;
	int cols = screenwidth / x_spacing;

	wl_list_for_each(output, &server.outputs, link) {		
		if (!output_is_usable(output)) {
			continue;
		}

		if (output->icons_osd) {
			// Kill off all the icon images
			struct wlr_scene_node *node, *tmpnode;
			wl_list_for_each_safe(node, tmpnode, &output->icons_osd->children, link) {	
				wlr_scene_node_destroy(node);
			}
		}
		if (!output->icons_osd) {
			output->icons_osd = lab_wlr_scene_tree_create(
				&server.scene->tree);
		}
			
		int wscount = 0;	
		
		wl_list_for_each(workspace, &server.workspaces.all, link) {
			
			// Note that the icon map is "bottom to top" - row 0 is the
			// lowest row on the screen
			unsigned char *icon_map = malloc(rows * cols);
			memset(icon_map, 0, rows*cols);
			for_each_view(view, &server.views, LAB_VIEW_CRITERIA_NONE) {
				if (view->workspace == server.workspaces.current && view->minimized) {
					if (view->icon_mapped) {
						int icon_x = (view->icon_x - left_offset) / x_spacing;
						int icon_y = (screenheight - view->icon_y) / y_spacing;
						if (icon_x >=0 && icon_x < cols && icon_y >=0 && icon_y < rows) {
							icon_map[icon_y * cols + icon_x]++;
						}
						// To consider:  We could block out nearby map squares if the icon is off grid
						
					}
				}
			}
			

			for_each_view(view, &server.views, LAB_VIEW_CRITERIA_NONE) {
				if (view->workspace == server.workspaces.current && view->minimized) {
					int retries = 0;
					while (!view->icon_mapped && retries < 30) {
						for (int i = 0; i < rows && view->icon_mapped == FALSE; i++) {
							for (int j = 0; j < cols  && view->icon_mapped == FALSE; j++) {
								if (icon_map[i * cols + j] <= retries) {
									view->icon_x = j * x_spacing + left_offset + 3 * retries;
									view->icon_y = screenheight - (i * y_spacing) + 3 * retries;
									view->icon_mapped = TRUE;
									icon_map[i * cols + j]++;
								}
							}
						}
						// Try the next layer of icons over.
						retries++;
					}
					// Last chance effort, just place it anywhere.
					if (!view->icon_mapped) {
						view->icon_x = left_offset;
						view->icon_y = screenheight - y_spacing;
						view->icon_mapped = TRUE;
					}
						
							
					// Determine dimensions for mini-window (common)
					struct wlr_fbox border_fbox = {
						.width = icon_width,
						.height = icon_height,
						.x = view->icon_x,
						.y = view->icon_y
					};

					
					struct lab_data_buffer *cbuffer = buffer_create_cairo(
					border_fbox.width, border_fbox.height,
						output->wlr_output->scale);
					struct wlr_scene_buffer	*scene_buffer = 
						lab_wlr_scene_buffer_create(output->icons_osd, &cbuffer->base);
					wlr_scene_buffer_set_dest_size(scene_buffer,
						border_fbox.width, border_fbox.height);
						
					cairo_t *cairo;
					cairo_surface_t *surface;
					cairo = cairo_create(cbuffer->surface);

					// Prepare border/caption styles that differ
					// for minimized/normal windows
					float *bc =
						theme->icon_color;

					int bw =
						theme->icon_border_width;

					int highlight =
						theme->icon_highlight;

					int shadow =
						theme->icon_shadow;

					enum border_type bt =
						theme->icon_border_type;

					int bvw =
						theme->icon_bevel_width;

					float *tc =
						theme->icon_title_color;

					
					set_cairo_color(cairo, bc);

					cairo_rectangle(cairo,
						0,
						0,
						border_fbox.width,
						border_fbox.height);
					cairo_fill(cairo);

					PangoLayout *layout =
						pango_cairo_create_layout(cairo);
					pango_context_set_round_glyph_positions(
						pango_layout_get_context(layout),
						false);
					pango_layout_set_ellipsize(
						layout,
						PANGO_ELLIPSIZE_END);
					pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
					PangoFontDescription *desc =
						font_to_pango_desc(
							&rc.font_icon);

					set_cairo_color(cairo, tc);

					int req_width = border_fbox.width - 2 * bw -2;

				
					pango_layout_set_font_description(
						layout,
						desc);
					
					pango_layout_set_width(layout,
						req_width * PANGO_SCALE);
					pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
					pango_layout_set_height(layout,
						(icon_height - 2 * bw - graphic_size - 6)
						* PANGO_SCALE);
					pango_font_description_free(desc);
					pango_layout_set_text(layout,
						view->title, -1);
					
					PangoRectangle prect = {0};
					pango_layout_get_extents(layout, NULL, &prect);
					

					cairo_borders(cairo,
						0,
						0,
						border_fbox.width,
						(border_fbox.height
							- prect.height/ PANGO_SCALE)
							- 2* bw - 4,
						bw, highlight, shadow,
						bt, bvw, bc);

					cairo_borders(cairo,
						0,
						(border_fbox.height
							- prect.height/ PANGO_SCALE)
							- 2* bw - 4,
						border_fbox.width,
						(prect.height/ PANGO_SCALE)
							+ 2* bw + 4,
						bw, highlight, shadow,
						bt, bvw, bc);
						
					cairo_move_to(cairo,
						(border_fbox.width
							- req_width) / 2,
						(border_fbox.height
							- prect.height/ PANGO_SCALE)
							- bw - 2);
							
					set_cairo_color(cairo, tc);
					pango_cairo_show_layout(cairo,
						layout);
					g_object_unref(layout);
					
					surface = cairo_get_target(cairo);
					cairo_surface_flush(surface);
					cairo_destroy(cairo);
					
					struct scaled_icon_buffer *icon_buffer =
					scaled_icon_buffer_create(output->icons_osd, graphic_size, graphic_size);
					scaled_icon_buffer_set_view(icon_buffer, view);

					wlr_scene_node_set_position(
						&icon_buffer->scene_buffer->node,
						border_fbox.x+(border_fbox.width-graphic_size)/2,
						border_fbox.y+bw+4);
					
		
					wlr_scene_node_set_position(&scene_buffer->node, border_fbox.x, border_fbox.y);
					if (view == active_drag_icon) {
						wlr_scene_node_raise_to_top(&scene_buffer->node);
						wlr_scene_node_raise_to_top(&icon_buffer->scene_buffer->node);
					} else {
						wlr_scene_node_lower_to_bottom(&icon_buffer->scene_buffer->node);
						wlr_scene_node_lower_to_bottom(&scene_buffer->node);
					}
						
					node_descriptor_create(&icon_buffer->scene_buffer->node, LAB_NODE_DESKTOP_ICON, view, 0);
					node_descriptor_create(&scene_buffer->node, LAB_NODE_DESKTOP_ICON, view, 0);
					wlr_buffer_drop(&cbuffer->base);
				
		
					

				
				}
			}
			free(icon_map);
			wscount++;
		}
		

	
		if (output->icons_osd) {
			wlr_scene_node_set_enabled(&output->icons_osd->node, true);

			wlr_scene_node_set_position(&output->icons_osd->node, 0, 0);
			wlr_scene_node_place_below(&output->icons_osd->node, &output->layer_tree[1]->node);
		}
	}
}
