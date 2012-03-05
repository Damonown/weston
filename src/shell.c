/*
 * Copyright © 2010-2012 Intel Corporation
 * Copyright © 2011-2012 Collabora, Ltd.
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <linux/input.h>
#include <assert.h>
#include <signal.h>
#include <math.h>

#include <wayland-server.h>
#include "compositor.h"
#include "desktop-shell-server-protocol.h"
#include "../shared/config-parser.h"

struct shell_surface;

struct wl_shell {
	struct weston_compositor *compositor;
	struct weston_shell shell;

	struct weston_layer fullscreen_layer;
	struct weston_layer panel_layer;
	struct weston_layer toplevel_layer;
	struct weston_layer background_layer;
	struct weston_layer lock_layer;

	struct {
		struct weston_process process;
		struct wl_client *client;
		struct wl_resource *desktop_shell;

		unsigned deathcount;
		uint32_t deathstamp;
	} child;

	bool locked;
	bool prepare_event_sent;

	struct shell_surface *lock_surface;
	struct wl_listener lock_surface_listener;

	struct wl_list backgrounds;
	struct wl_list panels;

	struct {
		char *path;
		int duration;
		struct wl_resource *binding;
		struct wl_list surfaces;
		struct weston_process process;
	} screensaver;

	struct weston_surface *debug_repaint_surface;
};

enum shell_surface_type {
	SHELL_SURFACE_NONE,

	SHELL_SURFACE_PANEL,
	SHELL_SURFACE_BACKGROUND,
	SHELL_SURFACE_LOCK,
	SHELL_SURFACE_SCREENSAVER,

	SHELL_SURFACE_TOPLEVEL,
	SHELL_SURFACE_TRANSIENT,
	SHELL_SURFACE_FULLSCREEN,
	SHELL_SURFACE_MAXIMIZED,
	SHELL_SURFACE_POPUP
};

struct shell_surface {
	struct wl_resource resource;

	struct weston_surface *surface;
	struct wl_listener surface_destroy_listener;
	struct shell_surface *parent;

	enum shell_surface_type type;
	int32_t saved_x, saved_y;
	bool saved_position_valid;

	struct {
		struct weston_transform transform;
		struct weston_matrix rotation;
	} rotation;

	struct {
		struct wl_pointer_grab grab;
		uint32_t time;
		int32_t x, y;
		struct weston_transform parent_transform;
		int32_t initial_up;
	} popup;

	struct {
		enum wl_shell_surface_fullscreen_method type;
		struct weston_transform transform; /* matrix from x, y */
		uint32_t framerate;
		struct weston_surface *black_surface;
	} fullscreen;

	struct weston_output *fullscreen_output;
	struct weston_output *output;
	struct wl_list link;
};

struct weston_move_grab {
	struct wl_pointer_grab grab;
	struct weston_surface *surface;
	int32_t dx, dy;
};

struct rotate_grab {
	struct wl_pointer_grab grab;
	struct shell_surface *surface;
	struct weston_matrix rotation;
	struct {
		int32_t x;
		int32_t y;
	} center;
};

static void
center_on_output(struct weston_surface *surface,
		 struct weston_output *output);

static void
shell_configuration(struct wl_shell *shell)
{
	char *config_file;
	char *path = NULL;
	int duration = 60;

	struct config_key saver_keys[] = {
		{ "path",       CONFIG_KEY_STRING,  &path },
		{ "duration",   CONFIG_KEY_INTEGER, &duration },
	};

	struct config_section cs[] = {
		{ "screensaver", saver_keys, ARRAY_LENGTH(saver_keys), NULL },
	};

	config_file = config_file_path("weston-desktop-shell.ini");
	parse_config_file(config_file, cs, ARRAY_LENGTH(cs), shell);
	free(config_file);

	shell->screensaver.path = path;
	shell->screensaver.duration = duration;
}

static void
noop_grab_focus(struct wl_pointer_grab *grab, uint32_t time,
		struct wl_surface *surface, int32_t x, int32_t y)
{
	grab->focus = NULL;
}

static void
move_grab_motion(struct wl_pointer_grab *grab,
		 uint32_t time, int32_t x, int32_t y)
{
	struct weston_move_grab *move = (struct weston_move_grab *) grab;
	struct wl_input_device *device = grab->input_device;
	struct weston_surface *es = move->surface;

	weston_surface_configure(es,
				 device->x + move->dx,
				 device->y + move->dy,
				 es->geometry.width, es->geometry.height);
}

static void
move_grab_button(struct wl_pointer_grab *grab,
		 uint32_t time, int32_t button, int32_t state)
{
	struct wl_input_device *device = grab->input_device;

	if (device->button_count == 0 && state == 0) {
		wl_input_device_end_pointer_grab(device, time);
		free(grab);
	}
}

static const struct wl_pointer_grab_interface move_grab_interface = {
	noop_grab_focus,
	move_grab_motion,
	move_grab_button,
};

static int
weston_surface_move(struct weston_surface *es,
		    struct weston_input_device *wd, uint32_t time)
{
	struct weston_move_grab *move;

	move = malloc(sizeof *move);
	if (!move)
		return -1;

	move->grab.interface = &move_grab_interface;
	move->dx = es->geometry.x - wd->input_device.grab_x;
	move->dy = es->geometry.y - wd->input_device.grab_y;
	move->surface = es;

	wl_input_device_start_pointer_grab(&wd->input_device, &move->grab, time);

	wl_input_device_set_pointer_focus(&wd->input_device, NULL, time, 0, 0);

	return 0;
}

static void
shell_surface_move(struct wl_client *client, struct wl_resource *resource,
		   struct wl_resource *input_resource, uint32_t time)
{
	struct weston_input_device *wd = input_resource->data;
	struct shell_surface *shsurf = resource->data;

	if (wd->input_device.button_count == 0 ||
	    wd->input_device.grab_time != time ||
	    wd->input_device.pointer_focus != &shsurf->surface->surface)
		return;

	if (weston_surface_move(shsurf->surface, wd, time) < 0)
		wl_resource_post_no_memory(resource);
}

struct weston_resize_grab {
	struct wl_pointer_grab grab;
	uint32_t edges;
	int32_t width, height;
	struct shell_surface *shsurf;
};

static void
resize_grab_motion(struct wl_pointer_grab *grab,
		   uint32_t time, int32_t x, int32_t y)
{
	struct weston_resize_grab *resize = (struct weston_resize_grab *) grab;
	struct wl_input_device *device = grab->input_device;
	int32_t width, height;
	int32_t from_x, from_y;
	int32_t to_x, to_y;

	weston_surface_from_global(resize->shsurf->surface,
				   device->grab_x, device->grab_y,
				   &from_x, &from_y);
	weston_surface_from_global(resize->shsurf->surface,
				   device->x, device->y, &to_x, &to_y);

	if (resize->edges & WL_SHELL_SURFACE_RESIZE_LEFT) {
		width = resize->width + from_x - to_x;
	} else if (resize->edges & WL_SHELL_SURFACE_RESIZE_RIGHT) {
		width = resize->width + to_x - from_x;
	} else {
		width = resize->width;
	}

	if (resize->edges & WL_SHELL_SURFACE_RESIZE_TOP) {
		height = resize->height + from_y - to_y;
	} else if (resize->edges & WL_SHELL_SURFACE_RESIZE_BOTTOM) {
		height = resize->height + to_y - from_y;
	} else {
		height = resize->height;
	}

	wl_shell_surface_send_configure(&resize->shsurf->resource,
					time, resize->edges, width, height);
}

static void
resize_grab_button(struct wl_pointer_grab *grab,
		   uint32_t time, int32_t button, int32_t state)
{
	struct wl_input_device *device = grab->input_device;

	if (device->button_count == 0 && state == 0) {
		wl_input_device_end_pointer_grab(device, time);
		free(grab);
	}
}

static const struct wl_pointer_grab_interface resize_grab_interface = {
	noop_grab_focus,
	resize_grab_motion,
	resize_grab_button,
};

static int
weston_surface_resize(struct shell_surface *shsurf,
		    struct weston_input_device *wd,
		    uint32_t time, uint32_t edges)
{
	struct weston_resize_grab *resize;

	if (shsurf->type == SHELL_SURFACE_FULLSCREEN)
		return 0;

	if (edges == 0 || edges > 15 ||
	    (edges & 3) == 3 || (edges & 12) == 12)
		return 0;

	resize = malloc(sizeof *resize);
	if (!resize)
		return -1;

	resize->grab.interface = &resize_grab_interface;
	resize->edges = edges;
	resize->width = shsurf->surface->geometry.width;
	resize->height = shsurf->surface->geometry.height;
	resize->shsurf = shsurf;

	wl_input_device_start_pointer_grab(&wd->input_device, &resize->grab, time);

	wl_input_device_set_pointer_focus(&wd->input_device, NULL, time, 0, 0);

	return 0;
}

static void
shell_surface_resize(struct wl_client *client, struct wl_resource *resource,
		     struct wl_resource *input_resource, uint32_t time,
		     uint32_t edges)
{
	struct weston_input_device *wd = input_resource->data;
	struct shell_surface *shsurf = resource->data;

	if (shsurf->type == SHELL_SURFACE_FULLSCREEN)
		return;

	if (wd->input_device.button_count == 0 ||
	    wd->input_device.grab_time != time ||
	    wd->input_device.pointer_focus != &shsurf->surface->surface)
		return;

	if (weston_surface_resize(shsurf, wd, time, edges) < 0)
		wl_resource_post_no_memory(resource);
}

static struct weston_output *
get_default_output(struct weston_compositor *compositor)
{
	return container_of(compositor->output_list.next,
			    struct weston_output, link);
}

static void
shell_unset_fullscreen(struct shell_surface *shsurf)
{
	/* undo all fullscreen things here */
	shsurf->fullscreen.type = WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT;
	shsurf->fullscreen.framerate = 0;
	wl_list_remove(&shsurf->fullscreen.transform.link);
	wl_list_init(&shsurf->fullscreen.transform.link);
	weston_surface_destroy(shsurf->fullscreen.black_surface);
	shsurf->fullscreen.black_surface = NULL;
	shsurf->fullscreen_output = NULL;
	shsurf->surface->force_configure = 1;
	weston_surface_set_position(shsurf->surface,
				    shsurf->saved_x, shsurf->saved_y);
}

static int
reset_shell_surface_type(struct shell_surface *surface)
{
	switch (surface->type) {
	case SHELL_SURFACE_FULLSCREEN:
		shell_unset_fullscreen(surface);
		break;
	case SHELL_SURFACE_MAXIMIZED:
		surface->output = get_default_output(surface->surface->compositor);
		weston_surface_set_position(surface->surface,
					    surface->saved_x,
					    surface->saved_y);
		break;
	case SHELL_SURFACE_PANEL:
	case SHELL_SURFACE_BACKGROUND:
		wl_list_remove(&surface->link);
		wl_list_init(&surface->link);
		break;
	case SHELL_SURFACE_SCREENSAVER:
	case SHELL_SURFACE_LOCK:
		wl_resource_post_error(&surface->resource,
				       WL_DISPLAY_ERROR_INVALID_METHOD,
				       "cannot reassign surface type");
		return -1;
	case SHELL_SURFACE_NONE:
	case SHELL_SURFACE_TOPLEVEL:
	case SHELL_SURFACE_TRANSIENT:
	case SHELL_SURFACE_POPUP:
		break;
	}

	surface->type = SHELL_SURFACE_NONE;
	return 0;
}

static void
shell_surface_set_toplevel(struct wl_client *client,
			   struct wl_resource *resource)

{
	struct shell_surface *surface = resource->data;

	if (reset_shell_surface_type(surface))
		return;

	surface->type = SHELL_SURFACE_TOPLEVEL;
}

static void
shell_surface_set_transient(struct wl_client *client,
			    struct wl_resource *resource,
			    struct wl_resource *parent_resource,
			    int x, int y, uint32_t flags)
{
	struct shell_surface *shsurf = resource->data;
	struct weston_surface *es = shsurf->surface;
	struct shell_surface *pshsurf = parent_resource->data;
	struct weston_surface *pes = pshsurf->surface;

	if (reset_shell_surface_type(shsurf))
		return;

	/* assign to parents output */
	shsurf->output = pes->output;
 	weston_surface_set_position(es, pes->geometry.x + x,
					pes->geometry.y + y);

	shsurf->type = SHELL_SURFACE_TRANSIENT;
}

static struct wl_shell *
shell_surface_get_shell(struct shell_surface *shsurf)
{
	struct weston_surface *es = shsurf->surface;
	struct weston_shell *shell = es->compositor->shell;

	return (struct wl_shell *)container_of(shell, struct wl_shell, shell);
}

static int
get_output_panel_height(struct wl_shell *wlshell,struct weston_output *output)
{
	struct shell_surface *priv;
	int panel_height = 0;

	if (!output)
		return 0;

	wl_list_for_each(priv, &wlshell->panels, link) {
		if (priv->output == output) {
			panel_height = priv->surface->geometry.height;
			break;
		}
	}
	return panel_height;
}

static void
shell_surface_set_maximized(struct wl_client *client,
			    struct wl_resource *resource,
			    struct wl_resource *output_resource )
{
	struct shell_surface *shsurf = resource->data;
	struct weston_surface *es = shsurf->surface;
	struct wl_shell *wlshell = NULL;
	uint32_t edges = 0, panel_height = 0;

	/* get the default output, if the client set it as NULL
	   check whether the ouput is available */
	if (output_resource)
		shsurf->output = output_resource->data;
	else
		shsurf->output = get_default_output(es->compositor);

	if (reset_shell_surface_type(shsurf))
		return;

	shsurf->saved_x = es->geometry.x;
	shsurf->saved_y = es->geometry.y;
	shsurf->saved_position_valid = true;

	wlshell = shell_surface_get_shell(shsurf);
	panel_height = get_output_panel_height(wlshell, es->output);
	edges = WL_SHELL_SURFACE_RESIZE_TOP|WL_SHELL_SURFACE_RESIZE_LEFT;

	wl_shell_surface_send_configure(&shsurf->resource,
					weston_compositor_get_time(), edges,
					es->output->current->width,
					es->output->current->height - panel_height);

	shsurf->type = SHELL_SURFACE_MAXIMIZED;
}

static struct weston_surface *
create_black_surface(struct weston_compositor *ec,
		     GLfloat x, GLfloat y, int w, int h)
{
	struct weston_surface *surface = NULL;

	surface = weston_surface_create(ec);
	if (surface == NULL) {
		fprintf(stderr, "no memory\n");
		return NULL;
	}

	weston_surface_configure(surface, x, y, w, h);
	weston_surface_set_color(surface, 0.0, 0.0, 0.0, 1);
	return surface;
}

/* Create black surface and append it to the associated fullscreen surface.
 * Handle size dismatch and positioning according to the method. */
static void
shell_configure_fullscreen(struct shell_surface *shsurf)
{
	struct weston_output *output = shsurf->fullscreen_output;
	struct weston_surface *surface = shsurf->surface;
	struct weston_matrix *matrix;
	float scale;

	center_on_output(surface, output);

	if (!shsurf->fullscreen.black_surface)
		shsurf->fullscreen.black_surface =
			create_black_surface(surface->compositor,
					     output->x, output->y,
					     output->current->width,
					     output->current->height);

	wl_list_remove(&shsurf->fullscreen.black_surface->layer_link);
	wl_list_insert(&surface->layer_link,
		       &shsurf->fullscreen.black_surface->layer_link);
	shsurf->fullscreen.black_surface->output = output;

	switch (shsurf->fullscreen.type) {
	case WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT:
		break;
	case WL_SHELL_SURFACE_FULLSCREEN_METHOD_SCALE:
		matrix = &shsurf->fullscreen.transform.matrix;
		weston_matrix_init(matrix);
		scale = (float)output->current->width/(float)surface->geometry.width;
		weston_matrix_scale(matrix, scale, scale, 1);
		wl_list_remove(&shsurf->fullscreen.transform.link);
		wl_list_insert(surface->geometry.transformation_list.prev,
			       &shsurf->fullscreen.transform.link);
		weston_surface_set_position(surface, output->x, output->y);
		break;
	case WL_SHELL_SURFACE_FULLSCREEN_METHOD_DRIVER:
		break;
	case WL_SHELL_SURFACE_FULLSCREEN_METHOD_FILL:
		break;
	default:
		break;
	}
}

/* make the fullscreen and black surface at the top */
static void
shell_stack_fullscreen(struct shell_surface *shsurf)
{
	struct weston_surface *surface = shsurf->surface;
	struct wl_shell *shell = shell_surface_get_shell(shsurf);

	wl_list_remove(&surface->layer_link);
	wl_list_remove(&shsurf->fullscreen.black_surface->layer_link);

	wl_list_insert(&shell->fullscreen_layer.surface_list,
		       &surface->layer_link);
	wl_list_insert(&surface->layer_link,
		       &shsurf->fullscreen.black_surface->layer_link);

	weston_surface_damage(surface);
	weston_surface_damage(shsurf->fullscreen.black_surface);
}

static void
shell_map_fullscreen(struct shell_surface *shsurf)
{
	shell_configure_fullscreen(shsurf);
	shell_stack_fullscreen(shsurf);
}

static void
shell_surface_set_fullscreen(struct wl_client *client,
			     struct wl_resource *resource,
			     uint32_t method,
			     uint32_t framerate,
			     struct wl_resource *output_resource)
{
	struct shell_surface *shsurf = resource->data;
	struct weston_surface *es = shsurf->surface;

	if (output_resource)
		shsurf->output = output_resource->data;
	else
		shsurf->output = get_default_output(es->compositor);

	if (reset_shell_surface_type(shsurf))
		return;

	shsurf->fullscreen_output = shsurf->output;
	shsurf->fullscreen.type = method;
	shsurf->fullscreen.framerate = framerate;
	shsurf->type = SHELL_SURFACE_FULLSCREEN;

	shsurf->saved_x = es->geometry.x;
	shsurf->saved_y = es->geometry.y;
	shsurf->saved_position_valid = true;

	if (es->output)
		shsurf->surface->force_configure = 1;

	wl_shell_surface_send_configure(&shsurf->resource,
					weston_compositor_get_time(), 0,
					shsurf->output->current->width,
					shsurf->output->current->height);
}

static void
popup_grab_focus(struct wl_pointer_grab *grab, uint32_t time,
		 struct wl_surface *surface, int32_t x, int32_t y)
{
	struct wl_input_device *device = grab->input_device;
	struct shell_surface *priv =
		container_of(grab, struct shell_surface, popup.grab);
	struct wl_client *client = priv->surface->surface.resource.client;

	if (surface && surface->resource.client == client) {
		wl_input_device_set_pointer_focus(device, surface, time, x, y);
		grab->focus = surface;
	} else {
		wl_input_device_set_pointer_focus(device, NULL, time, 0, 0);
		grab->focus = NULL;
	}
}

static void
popup_grab_motion(struct wl_pointer_grab *grab,
		  uint32_t time, int32_t sx, int32_t sy)
{
	struct wl_resource *resource;

	resource = grab->input_device->pointer_focus_resource;
	if (resource)
		wl_input_device_send_motion(resource, time, sx, sy);
}

static void
popup_grab_button(struct wl_pointer_grab *grab,
		  uint32_t time, int32_t button, int32_t state)
{
	struct wl_resource *resource;
	struct shell_surface *shsurf =
		container_of(grab, struct shell_surface, popup.grab);

	resource = grab->input_device->pointer_focus_resource;
	if (resource) {
		wl_input_device_send_button(resource, time, button, state);
	} else if (state == 0 &&
		   (shsurf->popup.initial_up ||
		    time - shsurf->popup.time > 500)) {
		wl_shell_surface_send_popup_done(&shsurf->resource);
		wl_input_device_end_pointer_grab(grab->input_device, time);
		shsurf->popup.grab.input_device = NULL;
	}

	if (state == 0)
		shsurf->popup.initial_up = 1;
}

static const struct wl_pointer_grab_interface popup_grab_interface = {
	popup_grab_focus,
	popup_grab_motion,
	popup_grab_button,
};

static void
shell_map_popup(struct shell_surface *shsurf, uint32_t time)
{
	struct wl_input_device *device;
	struct weston_surface *es = shsurf->surface;
	struct weston_surface *parent = shsurf->parent->surface;

	es->output = parent->output;

	shsurf->popup.grab.interface = &popup_grab_interface;
	device = es->compositor->input_device;

	weston_surface_update_transform(parent);
	if (parent->transform.enabled) {
		shsurf->popup.parent_transform.matrix =
			parent->transform.matrix;
	} else {
		/* construct x, y translation matrix */
		weston_matrix_init(&shsurf->popup.parent_transform.matrix);
		shsurf->popup.parent_transform.matrix.d[12] =
			parent->geometry.x;
		shsurf->popup.parent_transform.matrix.d[13] =
			parent->geometry.y;
	}
	wl_list_insert(es->geometry.transformation_list.prev,
		       &shsurf->popup.parent_transform.link);
	weston_surface_set_position(es, shsurf->popup.x, shsurf->popup.y);

	shsurf->popup.grab.input_device = device;
	shsurf->popup.time = device->grab_time;
	shsurf->popup.initial_up = 0;

	wl_input_device_start_pointer_grab(shsurf->popup.grab.input_device,
				   &shsurf->popup.grab, shsurf->popup.time);
}

static void
shell_surface_set_popup(struct wl_client *client,
			struct wl_resource *resource,
			struct wl_resource *input_device_resource,
			uint32_t time,
			struct wl_resource *parent_resource,
			int32_t x, int32_t y, uint32_t flags)
{
	struct shell_surface *shsurf = resource->data;

	shsurf->type = SHELL_SURFACE_POPUP;
	shsurf->parent = parent_resource->data;
	shsurf->popup.x = x;
	shsurf->popup.y = y;
}

static const struct wl_shell_surface_interface shell_surface_implementation = {
	shell_surface_move,
	shell_surface_resize,
	shell_surface_set_toplevel,
	shell_surface_set_transient,
	shell_surface_set_fullscreen,
	shell_surface_set_popup,
	shell_surface_set_maximized
};

static void
destroy_shell_surface(struct wl_resource *resource)
{
	struct shell_surface *shsurf = resource->data;

	if (shsurf->popup.grab.input_device)
		wl_input_device_end_pointer_grab(shsurf->popup.grab.input_device, 0);

	/* in case cleaning up a dead client destroys shell_surface first */
	if (shsurf->surface)
		wl_list_remove(&shsurf->surface_destroy_listener.link);

	if (shsurf->fullscreen.black_surface)
		weston_surface_destroy(shsurf->fullscreen.black_surface);

	wl_list_remove(&shsurf->link);
	free(shsurf);
}

static void
shell_handle_surface_destroy(struct wl_listener *listener,
			     struct wl_resource *resource, uint32_t time)
{
	struct shell_surface *shsurf = container_of(listener,
						    struct shell_surface,
						    surface_destroy_listener);

	shsurf->surface = NULL;
	wl_resource_destroy(&shsurf->resource, time);
}

static struct shell_surface *
get_shell_surface(struct weston_surface *surface)
{
	struct wl_list *lst = &surface->surface.resource.destroy_listener_list;
	struct wl_listener *listener;

	/* search the destroy listener list for our callback */
	wl_list_for_each(listener, lst, link) {
		if (listener->func == shell_handle_surface_destroy) {
			return container_of(listener, struct shell_surface,
					    surface_destroy_listener);
		}
	}

	return NULL;
}

static void
shell_get_shell_surface(struct wl_client *client,
			struct wl_resource *resource,
			uint32_t id,
			struct wl_resource *surface_resource)
{
	struct weston_surface *surface = surface_resource->data;
	struct shell_surface *shsurf;

	if (get_shell_surface(surface)) {
		wl_resource_post_error(surface_resource,
			WL_DISPLAY_ERROR_INVALID_OBJECT,
			"wl_shell::get_shell_surface already requested");
		return;
	}

	shsurf = calloc(1, sizeof *shsurf);
	if (!shsurf) {
		wl_resource_post_no_memory(resource);
		return;
	}

	shsurf->resource.destroy = destroy_shell_surface;
	shsurf->resource.object.id = id;
	shsurf->resource.object.interface = &wl_shell_surface_interface;
	shsurf->resource.object.implementation =
		(void (**)(void)) &shell_surface_implementation;
	shsurf->resource.data = shsurf;

	shsurf->saved_position_valid = false;
	shsurf->surface = surface;
	shsurf->fullscreen.type = WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT;
	shsurf->fullscreen.framerate = 0;
	shsurf->fullscreen.black_surface = NULL;
	wl_list_init(&shsurf->fullscreen.transform.link);

	shsurf->surface_destroy_listener.func = shell_handle_surface_destroy;
	wl_list_insert(surface->surface.resource.destroy_listener_list.prev,
		       &shsurf->surface_destroy_listener.link);

	/* init link so its safe to always remove it in destroy_shell_surface */
	wl_list_init(&shsurf->link);

	/* empty when not in use */
	wl_list_init(&shsurf->rotation.transform.link);
	weston_matrix_init(&shsurf->rotation.rotation);

	shsurf->type = SHELL_SURFACE_NONE;

	wl_client_add_resource(client, &shsurf->resource);
}

static const struct wl_shell_interface shell_implementation = {
	shell_get_shell_surface
};

static void
handle_screensaver_sigchld(struct weston_process *proc, int status)
{
	proc->pid = 0;
}

static void
launch_screensaver(struct wl_shell *shell)
{
	if (shell->screensaver.binding)
		return;

	if (!shell->screensaver.path)
		return;

	if (shell->screensaver.process.pid != 0) {
		fprintf(stderr, "old screensaver still running\n");
		return;
	}

	weston_client_launch(shell->compositor,
			   &shell->screensaver.process,
			   shell->screensaver.path,
			   handle_screensaver_sigchld);
}

static void
terminate_screensaver(struct wl_shell *shell)
{
	if (shell->screensaver.process.pid == 0)
		return;

	kill(shell->screensaver.process.pid, SIGTERM);
}

static void
show_screensaver(struct wl_shell *shell, struct shell_surface *surface)
{
	struct wl_list *list;

	if (shell->lock_surface)
		list = &shell->lock_surface->surface->layer_link;
	else
		list = &shell->lock_layer.surface_list;

	wl_list_remove(&surface->surface->layer_link);
	wl_list_insert(list, &surface->surface->layer_link);
	surface->surface->output = surface->output;
	weston_surface_damage(surface->surface);
}

static void
hide_screensaver(struct wl_shell *shell, struct shell_surface *surface)
{
	wl_list_remove(&surface->surface->layer_link);
	wl_list_init(&surface->surface->layer_link);
	surface->surface->output = NULL;
}

static void
desktop_shell_set_background(struct wl_client *client,
			     struct wl_resource *resource,
			     struct wl_resource *output_resource,
			     struct wl_resource *surface_resource)
{
	struct wl_shell *shell = resource->data;
	struct shell_surface *shsurf = surface_resource->data;
	struct weston_surface *surface = shsurf->surface;
	struct shell_surface *priv;

	if (reset_shell_surface_type(shsurf))
		return;

	wl_list_for_each(priv, &shell->backgrounds, link) {
		if (priv->output == output_resource->data) {
			priv->surface->output = NULL;
			wl_list_remove(&priv->surface->layer_link);
			wl_list_remove(&priv->link);
			break;
		}
	}

	shsurf->type = SHELL_SURFACE_BACKGROUND;
	shsurf->output = output_resource->data;

	wl_list_insert(&shell->backgrounds, &shsurf->link);

	weston_surface_set_position(surface, shsurf->output->x,
				    shsurf->output->y);

	desktop_shell_send_configure(resource,
				     weston_compositor_get_time(), 0,
				     surface_resource,
				     shsurf->output->current->width,
				     shsurf->output->current->height);
}

static void
desktop_shell_set_panel(struct wl_client *client,
			struct wl_resource *resource,
			struct wl_resource *output_resource,
			struct wl_resource *surface_resource)
{
	struct wl_shell *shell = resource->data;
	struct shell_surface *shsurf = surface_resource->data;
	struct weston_surface *surface = shsurf->surface;
	struct shell_surface *priv;

	if (reset_shell_surface_type(shsurf))
		return;

	wl_list_for_each(priv, &shell->panels, link) {
		if (priv->output == output_resource->data) {
			priv->surface->output = NULL;
			wl_list_remove(&priv->surface->layer_link);
			wl_list_remove(&priv->link);
			break;
		}
	}

	shsurf->type = SHELL_SURFACE_PANEL;
	shsurf->output = output_resource->data;

	wl_list_insert(&shell->panels, &shsurf->link);

	weston_surface_set_position(surface, shsurf->output->x,
				    shsurf->output->y);

	desktop_shell_send_configure(resource,
				     weston_compositor_get_time(), 0,
				     surface_resource,
				     shsurf->output->current->width,
				     shsurf->output->current->height);
}

static void
handle_lock_surface_destroy(struct wl_listener *listener,
			    struct wl_resource *resource, uint32_t time)
{
	struct wl_shell *shell =
		container_of(listener, struct wl_shell, lock_surface_listener);

	fprintf(stderr, "lock surface gone\n");
	shell->lock_surface = NULL;
}

static void
desktop_shell_set_lock_surface(struct wl_client *client,
			       struct wl_resource *resource,
			       struct wl_resource *surface_resource)
{
	struct wl_shell *shell = resource->data;
	struct shell_surface *surface = surface_resource->data;

	if (reset_shell_surface_type(surface))
		return;

	shell->prepare_event_sent = false;

	if (!shell->locked)
		return;

	shell->lock_surface = surface;

	shell->lock_surface_listener.func = handle_lock_surface_destroy;
	wl_list_insert(&surface_resource->destroy_listener_list,
		       &shell->lock_surface_listener.link);

	shell->lock_surface->type = SHELL_SURFACE_LOCK;
}

static void
resume_desktop(struct wl_shell *shell)
{
	struct shell_surface *tmp;

	wl_list_for_each(tmp, &shell->screensaver.surfaces, link)
		hide_screensaver(shell, tmp);

	terminate_screensaver(shell);

	wl_list_remove(&shell->lock_layer.link);
	wl_list_insert(&shell->compositor->cursor_layer.link,
		       &shell->fullscreen_layer.link);
	wl_list_insert(&shell->fullscreen_layer.link,
		       &shell->panel_layer.link);
	wl_list_insert(&shell->panel_layer.link, &shell->toplevel_layer.link);

	shell->locked = false;
	weston_compositor_repick(shell->compositor);
	shell->compositor->idle_time = shell->compositor->option_idle_time;
	weston_compositor_wake(shell->compositor);
	weston_compositor_damage_all(shell->compositor);
}

static void
desktop_shell_unlock(struct wl_client *client,
		     struct wl_resource *resource)
{
	struct wl_shell *shell = resource->data;

	shell->prepare_event_sent = false;

	if (shell->locked)
		resume_desktop(shell);
}

static const struct desktop_shell_interface desktop_shell_implementation = {
	desktop_shell_set_background,
	desktop_shell_set_panel,
	desktop_shell_set_lock_surface,
	desktop_shell_unlock
};

static enum shell_surface_type
get_shell_surface_type(struct weston_surface *surface)
{
	struct shell_surface *shsurf;

	shsurf = get_shell_surface(surface);
	if (!shsurf)
		return SHELL_SURFACE_NONE;
	return shsurf->type;
}

static void
move_binding(struct wl_input_device *device, uint32_t time,
	     uint32_t key, uint32_t button, uint32_t state, void *data)
{
	struct weston_surface *surface =
		(struct weston_surface *) device->pointer_focus;

	if (surface == NULL)
		return;

	switch (get_shell_surface_type(surface)) {
		case SHELL_SURFACE_PANEL:
		case SHELL_SURFACE_BACKGROUND:
		case SHELL_SURFACE_FULLSCREEN:
		case SHELL_SURFACE_SCREENSAVER:
			return;
		default:
			break;
	}

	weston_surface_move(surface, (struct weston_input_device *) device, time);
}

static void
resize_binding(struct wl_input_device *device, uint32_t time,
	       uint32_t key, uint32_t button, uint32_t state, void *data)
{
	struct weston_surface *surface =
		(struct weston_surface *) device->pointer_focus;
	uint32_t edges = 0;
	int32_t x, y;
	struct shell_surface *shsurf;

	if (surface == NULL)
		return;

	shsurf = get_shell_surface(surface);
	if (!shsurf)
		return;

	switch (shsurf->type) {
		case SHELL_SURFACE_PANEL:
		case SHELL_SURFACE_BACKGROUND:
		case SHELL_SURFACE_FULLSCREEN:
		case SHELL_SURFACE_SCREENSAVER:
			return;
		default:
			break;
	}

	weston_surface_from_global(surface,
				   device->grab_x, device->grab_y, &x, &y);

	if (x < surface->geometry.width / 3)
		edges |= WL_SHELL_SURFACE_RESIZE_LEFT;
	else if (x < 2 * surface->geometry.width / 3)
		edges |= 0;
	else
		edges |= WL_SHELL_SURFACE_RESIZE_RIGHT;

	if (y < surface->geometry.height / 3)
		edges |= WL_SHELL_SURFACE_RESIZE_TOP;
	else if (y < 2 * surface->geometry.height / 3)
		edges |= 0;
	else
		edges |= WL_SHELL_SURFACE_RESIZE_BOTTOM;

	weston_surface_resize(shsurf, (struct weston_input_device *) device,
			    time, edges);
}

static void
zoom_binding(struct wl_input_device *device, uint32_t time,
	       uint32_t key, uint32_t button, uint32_t state, void *data)
{
	struct weston_input_device *wd = (struct weston_input_device *) device;
	struct weston_compositor *compositor = wd->compositor;
	struct weston_output *output;

	wl_list_for_each(output, &compositor->output_list, link) {
		if (pixman_region32_contains_point(&output->region,
						device->x, device->y, NULL)) {
			if (state && key == KEY_UP) {
				output->zoom.active = 1;
				output->zoom.level -= output->zoom.increment;
			}
			if (state && key == KEY_DOWN)
				output->zoom.level += output->zoom.increment;

			if (output->zoom.level >= 1.0) {
				output->zoom.active = 0;
				output->zoom.level = 1.0;
			}

			if (output->zoom.level < output->zoom.increment)
				output->zoom.level = output->zoom.increment;

			weston_output_update_zoom(output, device->x, device->y);
		}
	}
}

static void
terminate_binding(struct wl_input_device *device, uint32_t time,
		  uint32_t key, uint32_t button, uint32_t state, void *data)
{
	struct weston_compositor *compositor = data;

	if (state)
		wl_display_terminate(compositor->wl_display);
}

static void
rotate_grab_motion(struct wl_pointer_grab *grab,
		 uint32_t time, int32_t x, int32_t y)
{
	struct rotate_grab *rotate =
		container_of(grab, struct rotate_grab, grab);
	struct wl_input_device *device = grab->input_device;
	struct shell_surface *surface = rotate->surface;
	GLfloat cx = 0.5f * surface->surface->geometry.width;
	GLfloat cy = 0.5f * surface->surface->geometry.height;
	GLfloat dx, dy;
	GLfloat r;

	dx = device->x - rotate->center.x;
	dy = device->y - rotate->center.y;
	r = sqrtf(dx * dx + dy * dy);

	wl_list_remove(&surface->rotation.transform.link);
	surface->surface->geometry.dirty = 1;

	if (r > 20.0f) {
		struct weston_matrix *matrix =
			&surface->rotation.transform.matrix;

		weston_matrix_init(&rotate->rotation);
		rotate->rotation.d[0] = dx / r;
		rotate->rotation.d[4] = -dy / r;
		rotate->rotation.d[1] = -rotate->rotation.d[4];
		rotate->rotation.d[5] = rotate->rotation.d[0];

		weston_matrix_init(matrix);
		weston_matrix_translate(matrix, -cx, -cy, 0.0f);
		weston_matrix_multiply(matrix, &surface->rotation.rotation);
		weston_matrix_multiply(matrix, &rotate->rotation);
		weston_matrix_translate(matrix, cx, cy, 0.0f);

		wl_list_insert(
			&surface->surface->geometry.transformation_list,
			&surface->rotation.transform.link);
	} else {
		wl_list_init(&surface->rotation.transform.link);
		weston_matrix_init(&surface->rotation.rotation);
		weston_matrix_init(&rotate->rotation);
	}

	/* Repaint implies weston_surface_update_transform(), which
	 * lazily applies the damage due to rotation update.
	 */
	weston_compositor_schedule_repaint(surface->surface->compositor);
}

static void
rotate_grab_button(struct wl_pointer_grab *grab,
		 uint32_t time, int32_t button, int32_t state)
{
	struct rotate_grab *rotate =
		container_of(grab, struct rotate_grab, grab);
	struct wl_input_device *device = grab->input_device;
	struct shell_surface *surface = rotate->surface;

	if (device->button_count == 0 && state == 0) {
		weston_matrix_multiply(&surface->rotation.rotation,
				       &rotate->rotation);
		wl_input_device_end_pointer_grab(device, time);
		free(rotate);
	}
}

static const struct wl_pointer_grab_interface rotate_grab_interface = {
	noop_grab_focus,
	rotate_grab_motion,
	rotate_grab_button,
};

static void
rotate_binding(struct wl_input_device *device, uint32_t time,
	       uint32_t key, uint32_t button, uint32_t state, void *data)
{
	struct weston_surface *base_surface =
		(struct weston_surface *) device->pointer_focus;
	struct shell_surface *surface;
	struct rotate_grab *rotate;
	GLfloat dx, dy;
	GLfloat r;

	if (base_surface == NULL)
		return;

	surface = get_shell_surface(base_surface);
	if (!surface)
		return;

	switch (surface->type) {
		case SHELL_SURFACE_PANEL:
		case SHELL_SURFACE_BACKGROUND:
		case SHELL_SURFACE_FULLSCREEN:
		case SHELL_SURFACE_SCREENSAVER:
			return;
		default:
			break;
	}

	rotate = malloc(sizeof *rotate);
	if (!rotate)
		return;

	rotate->grab.interface = &rotate_grab_interface;
	rotate->surface = surface;

	weston_surface_to_global(surface->surface,
				 surface->surface->geometry.width / 2,
				 surface->surface->geometry.height / 2,
				 &rotate->center.x, &rotate->center.y);

	wl_input_device_start_pointer_grab(device, &rotate->grab, time);

	dx = device->x - rotate->center.x;
	dy = device->y - rotate->center.y;
	r = sqrtf(dx * dx + dy * dy);
	if (r > 20.0f) {
		struct weston_matrix inverse;

		weston_matrix_init(&inverse);
		inverse.d[0] = dx / r;
		inverse.d[4] = dy / r;
		inverse.d[1] = -inverse.d[4];
		inverse.d[5] = inverse.d[0];
		weston_matrix_multiply(&surface->rotation.rotation, &inverse);
	} else {
		weston_matrix_init(&surface->rotation.rotation);
		weston_matrix_init(&rotate->rotation);
	}

	wl_input_device_set_pointer_focus(device, NULL, time, 0, 0);
}

static void
activate(struct weston_shell *base, struct weston_surface *es,
	 struct weston_input_device *device, uint32_t time)
{
	struct wl_shell *shell = container_of(base, struct wl_shell, shell);
	struct weston_compositor *compositor = shell->compositor;

	weston_surface_activate(es, device, time);

	if (compositor->wxs)
		weston_xserver_surface_activate(es);

	switch (get_shell_surface_type(es)) {
	case SHELL_SURFACE_BACKGROUND:
	case SHELL_SURFACE_PANEL:
	case SHELL_SURFACE_LOCK:
		break;

	case SHELL_SURFACE_SCREENSAVER:
		/* always below lock surface */
		if (shell->lock_surface)
			weston_surface_restack(es,
					       &shell->lock_surface->surface->layer_link);
		break;
	case SHELL_SURFACE_FULLSCREEN:
		/* should on top of panels */
		break;
	default:
		weston_surface_restack(es,
				       &shell->toplevel_layer.surface_list);
		break;
	}
}

static void
click_to_activate_binding(struct wl_input_device *device,
			  uint32_t time, uint32_t key,
			  uint32_t button, uint32_t state, void *data)
{
	struct weston_input_device *wd = (struct weston_input_device *) device;
	struct weston_compositor *compositor = data;
	struct weston_surface *focus;
	struct weston_surface *upper;

	focus = (struct weston_surface *) device->pointer_focus;
	upper = container_of(focus->link.prev, struct weston_surface, link);
	if (focus->link.prev != &compositor->surface_list &&
	    get_shell_surface_type(upper) == SHELL_SURFACE_FULLSCREEN) {
		printf("%s: focus is black surface, raise its fullscreen surface\n", __func__);
		shell_stack_fullscreen(get_shell_surface(upper));
		focus = upper;
	}

	if (state && focus && device->pointer_grab == &device->default_pointer_grab)
		activate(compositor->shell, focus, wd, time);
}

static void
lock(struct weston_shell *base)
{
	struct wl_shell *shell = container_of(base, struct wl_shell, shell);
	struct weston_input_device *device;
	struct shell_surface *shsurf;
	struct weston_output *output;
	uint32_t time;

	if (shell->locked) {
		wl_list_for_each(output, &shell->compositor->output_list, link)
			/* TODO: find a way to jump to other DPMS levels */
			if (output->set_dpms)
				output->set_dpms(output, WESTON_DPMS_STANDBY);
		return;
	}

	shell->locked = true;

	/* Hide all surfaces by removing the fullscreen, panel and
	 * toplevel layers.  This way nothing else can show or receive
	 * input events while we are locked. */

	wl_list_remove(&shell->panel_layer.link);
	wl_list_remove(&shell->toplevel_layer.link);
	wl_list_remove(&shell->fullscreen_layer.link);
	wl_list_insert(&shell->compositor->cursor_layer.link,
		       &shell->lock_layer.link);

	launch_screensaver(shell);

	wl_list_for_each(shsurf, &shell->screensaver.surfaces, link)
		show_screensaver(shell, shsurf);

	if (!wl_list_empty(&shell->screensaver.surfaces)) {
		shell->compositor->idle_time = shell->screensaver.duration;
		weston_compositor_wake(shell->compositor);
		shell->compositor->state = WESTON_COMPOSITOR_IDLE;
	}

	/* reset pointer foci */
	weston_compositor_repick(shell->compositor);

	/* reset keyboard foci */
	time = weston_compositor_get_time();
	wl_list_for_each(device, &shell->compositor->input_device_list, link) {
		wl_input_device_set_keyboard_focus(&device->input_device,
						   NULL, time);
	}

	/* TODO: disable bindings that should not work while locked. */

	/* All this must be undone in resume_desktop(). */
}

static void
unlock(struct weston_shell *base)
{
	struct wl_shell *shell = container_of(base, struct wl_shell, shell);

	if (!shell->locked || shell->lock_surface) {
		weston_compositor_wake(shell->compositor);
		return;
	}

	/* If desktop-shell client has gone away, unlock immediately. */
	if (!shell->child.desktop_shell) {
		resume_desktop(shell);
		return;
	}

	if (shell->prepare_event_sent)
		return;

	desktop_shell_send_prepare_lock_surface(shell->child.desktop_shell);
	shell->prepare_event_sent = true;
}

static void
center_on_output(struct weston_surface *surface, struct weston_output *output)
{
	struct weston_mode *mode = output->current;
	GLfloat x = (mode->width - surface->geometry.width) / 2;
	GLfloat y = (mode->height - surface->geometry.height) / 2;

	weston_surface_set_position(surface, output->x + x, output->y + y);
}

static void
map(struct weston_shell *base, struct weston_surface *surface,
    int32_t width, int32_t height, int32_t sx, int32_t sy)
{
	struct wl_shell *shell = container_of(base, struct wl_shell, shell);
	struct weston_compositor *compositor = shell->compositor;
	struct shell_surface *shsurf;
	enum shell_surface_type surface_type = SHELL_SURFACE_NONE;
	struct weston_surface *parent;
	int panel_height = 0;

	shsurf = get_shell_surface(surface);
	if (shsurf)
		surface_type = shsurf->type;

	surface->geometry.width = width;
	surface->geometry.height = height;
	surface->geometry.dirty = 1;

	weston_compositor_update_drag_surfaces(compositor);

	/* initial positioning, see also configure() */
	switch (surface_type) {
	case SHELL_SURFACE_TOPLEVEL:
		weston_surface_set_position(surface, 10 + random() % 400,
					    10 + random() % 400);
		break;
	case SHELL_SURFACE_SCREENSAVER:
		center_on_output(surface, shsurf->fullscreen_output);
		break;
	case SHELL_SURFACE_FULLSCREEN:
		shell_map_fullscreen(shsurf);
		break;
	case SHELL_SURFACE_MAXIMIZED:
		/* use surface configure to set the geometry */
		panel_height = get_output_panel_height(shell,surface->output);
		weston_surface_set_position(surface, surface->output->x,
					    surface->output->y + panel_height);
		break;
	case SHELL_SURFACE_LOCK:
		center_on_output(surface, get_default_output(compositor));
		break;
	case SHELL_SURFACE_POPUP:
		shell_map_popup(shsurf, shsurf->popup.time);
	case SHELL_SURFACE_NONE:
		weston_surface_set_position(surface,
					    surface->geometry.x + sx,
					    surface->geometry.y + sy);
		break;
	default:
		;
	}

	/* surface stacking order, see also activate() */
	switch (surface_type) {
	case SHELL_SURFACE_BACKGROUND:
		/* background always visible, at the bottom */
		wl_list_insert(&shell->background_layer.surface_list,
			       &surface->layer_link);
		break;
	case SHELL_SURFACE_PANEL:
		/* panel always on top, hidden while locked */
		wl_list_insert(&shell->panel_layer.surface_list,
			       &surface->layer_link);
		break;
	case SHELL_SURFACE_LOCK:
		/* lock surface always visible, on top */
		wl_list_insert(&shell->lock_layer.surface_list,
			       &surface->layer_link);
		weston_compositor_wake(compositor);
		break;
	case SHELL_SURFACE_SCREENSAVER:
		/* If locked, show it. */
		if (shell->locked) {
			show_screensaver(shell, shsurf);
			compositor->idle_time = shell->screensaver.duration;
			weston_compositor_wake(compositor);
			if (!shell->lock_surface)
				compositor->state = WESTON_COMPOSITOR_IDLE;
		}
		break;
	case SHELL_SURFACE_POPUP:
	case SHELL_SURFACE_TRANSIENT:
		parent = shsurf->parent->surface;
		wl_list_insert(parent->layer_link.prev, &surface->layer_link);
		break;
	case SHELL_SURFACE_FULLSCREEN:
	case SHELL_SURFACE_NONE:
		break;
	default:
		wl_list_insert(&shell->toplevel_layer.surface_list,
			       &surface->layer_link); 
		break;
	}

	weston_surface_assign_output(surface);
	weston_compositor_repick(compositor);
	if (surface_type == SHELL_SURFACE_MAXIMIZED)
		surface->output = shsurf->output;

	switch (surface_type) {
	case SHELL_SURFACE_TOPLEVEL:
	case SHELL_SURFACE_TRANSIENT:
	case SHELL_SURFACE_FULLSCREEN:
	case SHELL_SURFACE_MAXIMIZED:
		if (!shell->locked)
			activate(base, surface,
				 (struct weston_input_device *)
				 compositor->input_device,
				 weston_compositor_get_time());
		break;
	default:
		break;
	}

	if (surface_type == SHELL_SURFACE_TOPLEVEL)
		weston_zoom_run(surface, 0.8, 1.0, NULL, NULL);
}

static void
configure(struct weston_shell *base, struct weston_surface *surface,
	  GLfloat x, GLfloat y, int32_t width, int32_t height)
{
	struct wl_shell *shell = container_of(base, struct wl_shell, shell);
	enum shell_surface_type surface_type = SHELL_SURFACE_NONE;
	enum shell_surface_type prev_surface_type = SHELL_SURFACE_NONE;
	struct shell_surface *shsurf;

	shsurf = get_shell_surface(surface);
	if (shsurf)
		surface_type = shsurf->type;

	surface->geometry.x = x;
	surface->geometry.y = y;
	surface->geometry.width = width;
	surface->geometry.height = height;
	surface->geometry.dirty = 1;

	switch (surface_type) {
	case SHELL_SURFACE_SCREENSAVER:
		center_on_output(surface, shsurf->fullscreen_output);
		break;
	case SHELL_SURFACE_FULLSCREEN:
		shell_configure_fullscreen(shsurf);
		if (prev_surface_type != SHELL_SURFACE_FULLSCREEN)
			shell_stack_fullscreen(shsurf);
		break;
	case SHELL_SURFACE_MAXIMIZED:
		/* setting x, y and using configure to change that geometry */
		surface->geometry.x = surface->output->x;
		surface->geometry.y = surface->output->y +
			get_output_panel_height(shell,surface->output);
		break;
	case SHELL_SURFACE_TOPLEVEL:
		break;
	default:
		break;
	}

	/* XXX: would a fullscreen surface need the same handling? */
	if (surface->output) {
		weston_surface_assign_output(surface);
		weston_compositor_repick(surface->compositor);

		if (surface_type == SHELL_SURFACE_SCREENSAVER)
			surface->output = shsurf->output;
		else if (surface_type == SHELL_SURFACE_MAXIMIZED)
			surface->output = shsurf->output;
	}
}

static int launch_desktop_shell_process(struct wl_shell *shell);

static void
desktop_shell_sigchld(struct weston_process *process, int status)
{
	uint32_t time;
	struct wl_shell *shell =
		container_of(process, struct wl_shell, child.process);

	shell->child.process.pid = 0;
	shell->child.client = NULL; /* already destroyed by wayland */

	/* if desktop-shell dies more than 5 times in 30 seconds, give up */
	time = weston_compositor_get_time();
	if (time - shell->child.deathstamp > 30000) {
		shell->child.deathstamp = time;
		shell->child.deathcount = 0;
	}

	shell->child.deathcount++;
	if (shell->child.deathcount > 5) {
		fprintf(stderr, "weston-desktop-shell died, giving up.\n");
		return;
	}

	fprintf(stderr, "weston-desktop-shell died, respawning...\n");
	launch_desktop_shell_process(shell);
}

static int
launch_desktop_shell_process(struct wl_shell *shell)
{
	const char *shell_exe = LIBEXECDIR "/weston-desktop-shell";

	shell->child.client = weston_client_launch(shell->compositor,
						 &shell->child.process,
						 shell_exe,
						 desktop_shell_sigchld);

	if (!shell->child.client)
		return -1;
	return 0;
}

static void
bind_shell(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_shell *shell = data;

	wl_client_add_object(client, &wl_shell_interface,
			     &shell_implementation, id, shell);
}

static void
unbind_desktop_shell(struct wl_resource *resource)
{
	struct wl_shell *shell = resource->data;

	if (shell->locked)
		resume_desktop(shell);

	shell->child.desktop_shell = NULL;
	shell->prepare_event_sent = false;
	free(resource);
}

static void
bind_desktop_shell(struct wl_client *client,
		   void *data, uint32_t version, uint32_t id)
{
	struct wl_shell *shell = data;
	struct wl_resource *resource;

	resource = wl_client_add_object(client, &desktop_shell_interface,
					&desktop_shell_implementation,
					id, shell);

	if (client == shell->child.client) {
		resource->destroy = unbind_desktop_shell;
		shell->child.desktop_shell = resource;
		return;
	}

	wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
			       "permission to bind desktop_shell denied");
	wl_resource_destroy(resource, 0);
}

static void
screensaver_set_surface(struct wl_client *client,
			struct wl_resource *resource,
			struct wl_resource *shell_surface_resource,
			struct wl_resource *output_resource)
{
	struct wl_shell *shell = resource->data;
	struct shell_surface *surface = shell_surface_resource->data;
	struct weston_output *output = output_resource->data;

	if (reset_shell_surface_type(surface))
		return;

	surface->type = SHELL_SURFACE_SCREENSAVER;

	surface->fullscreen_output = output;
	surface->output = output;
	wl_list_insert(shell->screensaver.surfaces.prev, &surface->link);
}

static const struct screensaver_interface screensaver_implementation = {
	screensaver_set_surface
};

static void
unbind_screensaver(struct wl_resource *resource)
{
	struct wl_shell *shell = resource->data;

	shell->screensaver.binding = NULL;
	free(resource);
}

static void
bind_screensaver(struct wl_client *client,
		 void *data, uint32_t version, uint32_t id)
{
	struct wl_shell *shell = data;
	struct wl_resource *resource;

	resource = wl_client_add_object(client, &screensaver_interface,
					&screensaver_implementation,
					id, shell);

	if (shell->screensaver.binding == NULL) {
		resource->destroy = unbind_screensaver;
		shell->screensaver.binding = resource;
		return;
	}

	wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
			       "interface object already bound");
	wl_resource_destroy(resource, 0);
}

struct switcher {
	struct weston_compositor *compositor;
	struct weston_surface *current;
	struct wl_listener listener;
	struct wl_keyboard_grab grab;
};

static void
switcher_next(struct switcher *switcher)
{
	struct weston_compositor *compositor = switcher->compositor;
	struct weston_surface *surface;
	struct weston_surface *first = NULL, *prev = NULL, *next = NULL;

	wl_list_for_each(surface, &compositor->surface_list, link) {
		/* Workaround for cursor surfaces. */
		if (surface->surface.resource.destroy_listener_list.next == NULL)
			continue;

		switch (get_shell_surface_type(surface)) {
		case SHELL_SURFACE_TOPLEVEL:
		case SHELL_SURFACE_FULLSCREEN:
		case SHELL_SURFACE_MAXIMIZED:
			if (first == NULL)
				first = surface;
			if (prev == switcher->current)
				next = surface;
			prev = surface;
			surface->alpha = 64;
			surface->geometry.dirty = 1;
			weston_surface_damage(surface);
			break;
		default:
			break;
		}
	}

	if (next == NULL)
		next = first;

	wl_list_remove(&switcher->listener.link);
	wl_list_insert(next->surface.resource.destroy_listener_list.prev,
		       &switcher->listener.link);

	switcher->current = next;
	next->alpha = 255;
}

static void
switcher_handle_surface_destroy(struct wl_listener *listener,
				struct wl_resource *resource, uint32_t time)
{
	struct switcher *switcher =
		container_of(listener, struct switcher, listener);

	switcher_next(switcher);
}

static void
switcher_destroy(struct switcher *switcher, uint32_t time)
{
	struct weston_compositor *compositor = switcher->compositor;
	struct weston_surface *surface;
	struct weston_input_device *device =
		(struct weston_input_device *) switcher->grab.input_device;

	wl_list_for_each(surface, &compositor->surface_list, link) {
		surface->alpha = 255;
		weston_surface_damage(surface);
	}

	activate(compositor->shell, switcher->current, device, time);
	wl_list_remove(&switcher->listener.link);
	wl_input_device_end_keyboard_grab(&device->input_device, time);
	free(switcher);
}

static void
switcher_key(struct wl_keyboard_grab *grab,
	     uint32_t time, uint32_t key, int32_t state)
{
	struct switcher *switcher = container_of(grab, struct switcher, grab);
	struct weston_input_device *device =
		(struct weston_input_device *) grab->input_device;

	if ((device->modifier_state & MODIFIER_SUPER) == 0) {
		switcher_destroy(switcher, time);
	} else if (key == KEY_TAB && state) {
		switcher_next(switcher);
	}
};

static const struct wl_keyboard_grab_interface switcher_grab = {
	switcher_key
};

static void
switcher_binding(struct wl_input_device *device, uint32_t time,
		 uint32_t key, uint32_t button,
		 uint32_t state, void *data)
{
	struct weston_compositor *compositor = data;
	struct switcher *switcher;

	switcher = malloc(sizeof *switcher);
	switcher->compositor = compositor;
	switcher->current = NULL;
	switcher->listener.func = switcher_handle_surface_destroy;
	wl_list_init(&switcher->listener.link);

	switcher->grab.interface = &switcher_grab;
	wl_input_device_start_keyboard_grab(device, &switcher->grab, time);
	wl_input_device_set_keyboard_focus(device, NULL,
					   weston_compositor_get_time());
	switcher_next(switcher);
}

static void
backlight_binding(struct wl_input_device *device, uint32_t time,
		  uint32_t key, uint32_t button, uint32_t state, void *data)
{
	struct weston_compositor *compositor = data;
	struct weston_output *output;

	/* TODO: we're limiting to simple use cases, where we assume just
	 * control on the primary display. We'd have to extend later if we
	 * ever get support for setting backlights on random desktop LCD
	 * panels though */
	output = get_default_output(compositor);
	if (!output)
		return;

	if (!output->set_backlight)
		return;

	if ((key == KEY_F9 || key == KEY_BRIGHTNESSDOWN) &&
	    output->backlight_current > 1)
		output->backlight_current--;
	else if ((key == KEY_F10 || key == KEY_BRIGHTNESSUP) &&
	    output->backlight_current < 10)
		output->backlight_current++;

	output->set_backlight(output, output->backlight_current);
}

static void
debug_repaint_binding(struct wl_input_device *device, uint32_t time,
		      uint32_t key, uint32_t button, uint32_t state, void *data)
{
	struct weston_compositor *compositor = data;
	struct wl_shell *shell =
		container_of(compositor->shell, struct wl_shell, shell);
	struct weston_surface *surface;

	if (shell->debug_repaint_surface) {
		weston_surface_destroy(shell->debug_repaint_surface);
		shell->debug_repaint_surface = NULL;
	} else {
		surface = weston_surface_create(compositor);
		weston_surface_set_color(surface, 1.0, 0.0, 0.0, 0.2);
		weston_surface_configure(surface, 0, 0, 8192, 8192);
		wl_list_insert(&compositor->fade_layer.surface_list,
			       &surface->layer_link);
		weston_surface_assign_output(surface);
		pixman_region32_init(&surface->input);

		/* Here's the dirty little trick that makes the
		 * repaint debugging work: we force an
		 * update_transform first to update dependent state
		 * and clear the geometry.dirty bit.  Then we clear
		 * the surface damage so it only gets repainted
		 * piecewise as we repaint other things.  */

		weston_surface_update_transform(surface);
		pixman_region32_fini(&surface->damage);
		pixman_region32_init(&surface->damage);
		shell->debug_repaint_surface = surface;
	}
}

static void
shell_destroy(struct weston_shell *base)
{
	struct wl_shell *shell = container_of(base, struct wl_shell, shell);

	if (shell->child.client)
		wl_client_destroy(shell->child.client);

	free(shell->screensaver.path);
	free(shell);
}

int
shell_init(struct weston_compositor *ec);

WL_EXPORT int
shell_init(struct weston_compositor *ec)
{
	struct wl_shell *shell;

	shell = malloc(sizeof *shell);
	if (shell == NULL)
		return -1;

	memset(shell, 0, sizeof *shell);
	shell->compositor = ec;
	shell->shell.lock = lock;
	shell->shell.unlock = unlock;
	shell->shell.map = map;
	shell->shell.configure = configure;
	shell->shell.destroy = shell_destroy;

	wl_list_init(&shell->backgrounds);
	wl_list_init(&shell->panels);
	wl_list_init(&shell->screensaver.surfaces);

	weston_layer_init(&shell->fullscreen_layer, &ec->cursor_layer.link);
	weston_layer_init(&shell->panel_layer, &shell->fullscreen_layer.link);
	weston_layer_init(&shell->toplevel_layer, &shell->panel_layer.link);
	weston_layer_init(&shell->background_layer,
			  &shell->toplevel_layer.link);
	wl_list_init(&shell->lock_layer.surface_list);

	shell_configuration(shell);

	if (wl_display_add_global(ec->wl_display, &wl_shell_interface,
				  shell, bind_shell) == NULL)
		return -1;

	if (wl_display_add_global(ec->wl_display,
				  &desktop_shell_interface,
				  shell, bind_desktop_shell) == NULL)
		return -1;

	if (wl_display_add_global(ec->wl_display, &screensaver_interface,
				  shell, bind_screensaver) == NULL)
		return -1;

	shell->child.deathstamp = weston_compositor_get_time();
	if (launch_desktop_shell_process(shell) != 0)
		return -1;

	weston_compositor_add_binding(ec, 0, BTN_LEFT, MODIFIER_SUPER,
				    move_binding, shell);
	weston_compositor_add_binding(ec, 0, BTN_MIDDLE, MODIFIER_SUPER,
				    resize_binding, shell);
	weston_compositor_add_binding(ec, KEY_BACKSPACE, 0,
				    MODIFIER_CTRL | MODIFIER_ALT,
				    terminate_binding, ec);
	weston_compositor_add_binding(ec, 0, BTN_LEFT, 0,
				    click_to_activate_binding, ec);
	weston_compositor_add_binding(ec, KEY_UP, 0, MODIFIER_SUPER,
				    zoom_binding, shell);
	weston_compositor_add_binding(ec, KEY_DOWN, 0, MODIFIER_SUPER,
				    zoom_binding, shell);
	weston_compositor_add_binding(ec, 0, BTN_LEFT,
				      MODIFIER_SUPER | MODIFIER_ALT,
				      rotate_binding, NULL);
	weston_compositor_add_binding(ec, KEY_TAB, 0, MODIFIER_SUPER,
				      switcher_binding, ec);

	/* brightness */
	weston_compositor_add_binding(ec, KEY_F9, 0, MODIFIER_CTRL,
				      backlight_binding, ec);
	weston_compositor_add_binding(ec, KEY_BRIGHTNESSDOWN, 0, 0,
				      backlight_binding, ec);
	weston_compositor_add_binding(ec, KEY_F10, 0, MODIFIER_CTRL,
				      backlight_binding, ec);
	weston_compositor_add_binding(ec, KEY_BRIGHTNESSUP, 0, 0,
				      backlight_binding, ec);

	weston_compositor_add_binding(ec, KEY_SPACE, 0, MODIFIER_SUPER,
				    debug_repaint_binding, ec);

	ec->shell = &shell->shell;

	return 0;
}
