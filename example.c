#define _POSIX_C_SOURCE 200112L
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <linux/input.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include "xdg-shell-client-protocol.h"

#define BARF(fmt, ...)		do { fprintf(stderr, fmt "\n", ##__VA_ARGS__); exit(EXIT_FAILURE); } while (0)
#define MIN(a, b)		((a) < (b) ? (a) : (b))
#define MAX(a, b)		((a) > (b) ? (a) : (b))

#define MAX_LINE_LEN 8192

/* Shared memory support code */
static void
randname(char *buf)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	long r = ts.tv_nsec;
	for (int i = 0; i < 6; ++i) {
		buf[i] = 'A'+(r&15)+(r&16)*2;
		r >>= 5;
	}
}

static int
create_shm_file(void)
{
	int retries = 100;
	do {
		char name[] = "/wl_shm-XXXXXX";
		randname(name + sizeof(name) - 7);
		--retries;
		int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd >= 0) {
			shm_unlink(name);
			return fd;
		}
	} while (retries > 0 && errno == EEXIST);
	return -1;
}

static int
allocate_shm_file(size_t size)
{
	int fd = create_shm_file();
	if (fd < 0)
		return -1;
	int ret;
	do {
		ret = ftruncate(fd, size);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

enum pointer_event_mask {
	POINTER_EVENT_ENTER = 1 << 0,
	POINTER_EVENT_LEAVE = 1 << 1,
	POINTER_EVENT_MOTION = 1 << 2,
	POINTER_EVENT_BUTTON = 1 << 3,
	POINTER_EVENT_AXIS = 1 << 4,
	POINTER_EVENT_AXIS_SOURCE = 1 << 5,
	POINTER_EVENT_AXIS_STOP = 1 << 6,
	POINTER_EVENT_AXIS_DISCRETE = 1 << 7,
};

struct pointer_event {
	uint32_t event_mask;
	wl_fixed_t surface_x, surface_y;
	uint32_t button, state;
	uint32_t time;
	uint32_t serial;
	struct wl_surface *surface;
	struct {
		bool valid;
		wl_fixed_t value;
		int32_t discrete;
	} axes[2];
	uint32_t axis_source;
};

// could definitely do this much better
struct area {
	uint32_t x1, y1, x2, y2;
};

/* Wayland code */
struct client_state {
	/* Globals */
	struct wl_display *wl_display;
	struct wl_registry *wl_registry;
	struct wl_region *wl_region;
	struct wl_shm *wl_shm;
	struct wl_compositor *wl_compositor;
	struct wl_subcompositor *wl_subcompositor;
	struct xdg_wm_base *xdg_wm_base;
	struct wl_seat *wl_seat;
	/* Objects */
	struct wl_surface *wl_surface;
	struct xdg_surface *xdg_surface;
	struct wl_pointer *wl_pointer;
	struct wl_touch *wl_touch;
	struct xdg_toplevel *xdg_toplevel;
	/* State */
	float offset;
	uint32_t last_frame;
	int width, height;
	bool closed;
	struct pointer_event pointer_event;
	uint32_t pointer_x;
	uint32_t pointer_y;
	/* what */
	struct area a;
};

/* taken from dwl */
typedef union {
        int i;
        unsigned int ui; 
        float f;
        const void *v;
} Arg;

/* spawns commands, taken from dwl */
void
spawn(const Arg *arg)
{
        if (fork() == 0) {
                dup2(STDERR_FILENO, STDOUT_FILENO);
                setsid();
                execvp(((char **)arg->v)[0], (char **)arg->v);
                BARF("execvp %s failed:", ((char **)arg->v)[0]);
        }
}

static void
wl_buffer_release(void *data, struct wl_buffer *wl_buffer)
{
	/* Sent by the compositor when it's no longer using this buffer */
	wl_buffer_destroy(wl_buffer);
}

static const struct wl_buffer_listener wl_buffer_listener = {
	.release = wl_buffer_release,
};

static struct wl_buffer *
draw_frame(struct client_state *state)
{
	int width = state->width, height = state->height;
	int stride = width * 4;
	int size = stride * height;

	int fd = allocate_shm_file(size);
	if (fd == -1) {
		return NULL;
	}

	uint32_t *data = mmap(NULL, size,
			PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		return NULL;
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(state->wl_shm, fd, size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0,
			width, height, stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);

	/* Draw checkerboxed background */
	int offset = (int)state->offset % 16;
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			/*if (x > MAX(0,state->pointer_x-10) && x < state->pointer_x+10 &&
				 y > MAX(0, state->pointer_y-10) && y < state->pointer_y+10) {
					data[y * width + x] = 0xFF0000FF;*/
			int dx = x - state->pointer_x;
			int dy = y - state->pointer_y;
			if (dx * dx + dy * dy < 256) {
					data[y * width + x] = 0x000000;
			} else
				data[y * width + x] = (((x + offset) + (y + offset)) % 32 < 16) ?
					0x00000000 : 0xFF000000;
				
			
		}
	}

	munmap(data, size);
	wl_buffer_add_listener(buffer, &wl_buffer_listener, NULL);
	return buffer;
}

static void
xdg_toplevel_configure(void *data,
		struct xdg_toplevel *xdg_toplevel, int32_t width, int32_t height,
		struct wl_array *states)
{
	struct client_state *state = data;
	if (width == 0 || height == 0) {
		/* Compositor is deferring to us */
		return;
	}
	state->width = width;
	state->height = height;
}

static void
xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
	struct client_state *state = data;
	state->closed = true;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	.configure = xdg_toplevel_configure,
	.close = xdg_toplevel_close,
};


static void
xdg_surface_configure(void *data,
		struct xdg_surface *xdg_surface, uint32_t serial)
{
	struct client_state *state = data;
	xdg_surface_ack_configure(xdg_surface, serial);

	struct wl_buffer *buffer = draw_frame(state);
	wl_surface_attach(state->wl_surface, buffer, 0, 0);
	wl_surface_commit(state->wl_surface);
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void
xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial)
{
	xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
	.ping = xdg_wm_base_ping,
};

static const struct wl_callback_listener wl_surface_frame_listener;

static void
wl_surface_frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
	/* Destroy this callback */
	wl_callback_destroy(cb);

	/* Request another frame */
	struct client_state *state = data;
	cb = wl_surface_frame(state->wl_surface);
	wl_callback_add_listener(cb, &wl_surface_frame_listener, state);

	/* Update scroll amount at 24 pixels per second */
	if (state->last_frame != 0) {
		int elapsed = time - state->last_frame;
		state->offset += elapsed / 1000.0 * 16;
	}

	/* Submit a frame for this event */
	struct wl_buffer *buffer = draw_frame(state);
	wl_surface_attach(state->wl_surface, buffer, 0, 0);
	wl_surface_damage_buffer(state->wl_surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(state->wl_surface);

	state->last_frame = time;
}

static const struct wl_callback_listener wl_surface_frame_listener = {
	.done = wl_surface_frame_done,
};


struct wl_surface *cursor_surface;
struct wl_cursor_image *cursor_image;

static void
wl_pointer_enter(void *data, struct wl_pointer *wl_pointer,
			uint32_t serial, struct wl_surface *surface,
			wl_fixed_t surface_x, wl_fixed_t surface_y)
{
	struct client_state *client_state = data;
	client_state->pointer_event.event_mask |= POINTER_EVENT_ENTER;
	client_state->pointer_event.serial = serial;
	client_state->pointer_event.surface = surface;
	client_state->pointer_event.surface_x = surface_x,
			client_state->pointer_event.surface_y = surface_y;
			

	// Set our pointer	
	//wl_pointer_set_cursor(wl_pointer, serial, cursor_surface,
	//	cursor_image->hotspot_x, cursor_image->hotspot_y);
}

static void
wl_pointer_leave(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, struct wl_surface *surface)
{
	struct client_state *client_state = data;
	client_state->pointer_event.event_mask |= POINTER_EVENT_LEAVE;
	client_state->pointer_event.serial = serial;
	client_state->pointer_event.surface = surface;
}


enum xdg_toplevel_resize_edge
component_edge(const int width, const int height,
		const int pointer_x,
		const int pointer_y,
		const int margin)
{
	const bool top = pointer_y < margin;
	const bool bottom = pointer_y > (height - margin);
	const bool left = pointer_x < margin;
	const bool right = pointer_x > (width - margin);

	if (top)
		if (left)
			return XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT;
		else if (right)
			return XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT;
		else
			return XDG_TOPLEVEL_RESIZE_EDGE_TOP;
	else if (bottom)
		if (left)
			return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT;
		else if (right)
			return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT;
		else
			return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
	else if (left)
		return XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
	else if (right)
		return XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
	else
		return XDG_TOPLEVEL_RESIZE_EDGE_NONE;
}

static void
wl_pointer_motion(void *data, struct wl_pointer *wl_pointer, uint32_t time,
		wl_fixed_t surface_x, wl_fixed_t surface_y)
{
	struct client_state *client_state = data;
	client_state->pointer_event.event_mask |= POINTER_EVENT_MOTION;
	client_state->pointer_event.time = time;
	client_state->pointer_event.surface_x = surface_x,
			client_state->pointer_event.surface_y = surface_y;
}

static void
wl_pointer_button(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
		uint32_t time, uint32_t button, uint32_t state)
{
	struct client_state *client_state = data;
	client_state->pointer_event.event_mask |= POINTER_EVENT_BUTTON;
	client_state->pointer_event.time = time;
	client_state->pointer_event.serial = serial;
	client_state->pointer_event.button = button,
	client_state->pointer_event.state = state;
}

static void
wl_pointer_axis(void *data, struct wl_pointer *wl_pointer, uint32_t time,
			uint32_t axis, wl_fixed_t value)
{
	struct client_state *client_state = data;
	client_state->pointer_event.event_mask |= POINTER_EVENT_AXIS;
	client_state->pointer_event.time = time;
	client_state->pointer_event.axes[axis].valid = true;
	client_state->pointer_event.axes[axis].value = value;
}

static void
wl_pointer_axis_source(void *data, struct wl_pointer *wl_pointer,
			uint32_t axis_source)
{
	struct client_state *client_state = data;
	client_state->pointer_event.event_mask |= POINTER_EVENT_AXIS_SOURCE;
	client_state->pointer_event.axis_source = axis_source;
}

static void
wl_pointer_axis_stop(void *data, struct wl_pointer *wl_pointer,
			uint32_t time, uint32_t axis)
{
	struct client_state *client_state = data;
	client_state->pointer_event.time = time;
	client_state->pointer_event.event_mask |= POINTER_EVENT_AXIS_STOP;
	client_state->pointer_event.axes[axis].valid = true;
}

static void
wl_pointer_axis_discrete(void *data, struct wl_pointer *wl_pointer,
			uint32_t axis, int32_t discrete)
{
	struct client_state *client_state = data;
	client_state->pointer_event.event_mask |= POINTER_EVENT_AXIS_DISCRETE;
	client_state->pointer_event.axes[axis].valid = true;
	client_state->pointer_event.axes[axis].discrete = discrete;
}

static void
wl_pointer_frame(void *data, struct wl_pointer *wl_pointer)
{
	struct client_state *client_state = data;
	struct pointer_event *event = &client_state->pointer_event;
	//fprintf(stderr, "pointer frame @ %d: ", event->time);

	if (event->event_mask & POINTER_EVENT_ENTER) {
		fprintf(stderr, "enter %p at %f, %f \n",
				(void *) event->surface,
				wl_fixed_to_double(event->surface_x),
				wl_fixed_to_double(event->surface_y));
	}

	if (event->event_mask & POINTER_EVENT_LEAVE) {
		fprintf(stderr, "leave %p \n", (void *) *(&client_state->pointer_event.surface));
	}

	if (event->event_mask & POINTER_EVENT_BUTTON) {
		char *state = event->state == WL_POINTER_BUTTON_STATE_RELEASED ?
				"released" : "pressed";
		fprintf(stderr, "button %d %s in %p \n", event->button, state, &client_state->pointer_event.surface);
		struct area *a = &client_state->a;
		if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
			// upper-left corner of new region
			a->x1 = client_state->pointer_x;
			a->y1 = client_state->pointer_y;
			fprintf(stderr, "%d %d %p\n", a->x1, a->y1, a);
		} else {
			// bottom-right
			if (!(a->x1 || a->y1))
				BARF("bad x1 y1");

			a->x2 = client_state->pointer_x;
			a->y2 = client_state->pointer_y;

			fprintf(stderr, "%d,%d %p\n", a->x2, a->y2, a);

			struct wl_surface *surface = wl_compositor_create_surface(client_state->wl_compositor);
			struct wl_region *region = wl_compositor_create_region(client_state->wl_compositor);

			fprintf(stderr, "%d,%d - %d,%d %p\n", a->x1, a->y1, a->x2, a->y2, a);

			wl_region_add(region, a->x1, a->y1, a->x2-a->x1, a->y2-a->y1);
			wl_surface_set_opaque_region(client_state->wl_surface, region);
			wl_surface_set_input_region(surface, region);

			struct wl_subsurface *subsurface = wl_subcompositor_get_subsurface(client_state->wl_subcompositor, surface, client_state->wl_surface);

			wl_subsurface_place_above(subsurface, client_state->wl_surface);
			// if it isn't evident enough, I know almost nothing about pointers
			*a = (struct area) { 0 };
		}
	}

	if (event->event_mask & POINTER_EVENT_MOTION) {
		/*fprintf(stderr, "motion %f, %f",
				wl_fixed_to_double(event->surface_x),
				wl_fixed_to_double(event->surface_y));*/
		client_state->pointer_x = wl_fixed_to_int(event->surface_x);
		client_state->pointer_y = wl_fixed_to_int(event->surface_y);

	}

	/*uint32_t axis_events = POINTER_EVENT_AXIS
		| POINTER_EVENT_AXIS_SOURCE
		| POINTER_EVENT_AXIS_STOP
		| POINTER_EVENT_AXIS_DISCRETE;
	char *axis_name[2] = {
		[WL_POINTER_AXIS_VERTICAL_SCROLL] = "vertical",
		[WL_POINTER_AXIS_HORIZONTAL_SCROLL] = "horizontal",
	};
	char *axis_source[4] = {
		[WL_POINTER_AXIS_SOURCE_WHEEL] = "wheel",
		[WL_POINTER_AXIS_SOURCE_FINGER] = "finger",
		[WL_POINTER_AXIS_SOURCE_CONTINUOUS] = "continuous",
		[WL_POINTER_AXIS_SOURCE_WHEEL_TILT] = "wheel tilt",
	};
	if (event->event_mask & axis_events) {
		for (size_t i = 0; i < 2; ++i) {
			if (!event->axes[i].valid) {
				continue;
			}
			fprintf(stderr, "%s axis ", axis_name[i]);
			if (event->event_mask & POINTER_EVENT_AXIS) {
				fprintf(stderr, "value %f ", wl_fixed_to_double(
						event->axes[i].value));
			}
			if (event->event_mask & POINTER_EVENT_AXIS_DISCRETE) {
				fprintf(stderr, "discrete %d ",
						event->axes[i].discrete);
			}
			if (event->event_mask & POINTER_EVENT_AXIS_SOURCE) {
				fprintf(stderr, "via %s ",
						axis_source[event->axis_source]);
			}
			if (event->event_mask & POINTER_EVENT_AXIS_STOP) {
				fprintf(stderr, "(stopped) ");
			}
		}
	}

	fprintf(stderr, "\n"); */
	memset(event, 0, sizeof(*event));
}

static const struct wl_pointer_listener wl_pointer_listener = {
	.enter = wl_pointer_enter,
	.leave = wl_pointer_leave,
	.motion = wl_pointer_motion,
	.button = wl_pointer_button,
	.axis = wl_pointer_axis,
	.frame = wl_pointer_frame,
	.axis_source = wl_pointer_axis_source,
	.axis_stop = wl_pointer_axis_stop,
	.axis_discrete = wl_pointer_axis_discrete,
};

static void
wl_seat_capabilities(void *data, struct wl_seat *wl_seat, uint32_t capabilities)
{
	struct client_state *state = data;

	bool have_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;

	if (have_pointer && state->wl_pointer == NULL) {
		state->wl_pointer = wl_seat_get_pointer(state->wl_seat);
		wl_pointer_add_listener(state->wl_pointer,
				&wl_pointer_listener, state);
	} else if (!have_pointer && state->wl_pointer != NULL) {
		wl_pointer_release(state->wl_pointer);
		state->wl_pointer = NULL;
	}
}

static void
wl_seat_name(void *data, struct wl_seat *wl_seat, const char *name)
{
	fprintf(stderr, "seat name: %s\n", name);
}

static const struct wl_seat_listener wl_seat_listener = {
	.capabilities = wl_seat_capabilities,
	.name = wl_seat_name,
};

static void
registry_global(void *data, struct wl_registry *wl_registry,
		uint32_t name, const char *interface, uint32_t version)
{
	struct client_state *state = data;
	if (strcmp(interface, wl_shm_interface.name) == 0) {
		state->wl_shm = wl_registry_bind(
				wl_registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_compositor_interface.name) == 0) {
		state->wl_compositor = wl_registry_bind(
				wl_registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
		state->wl_subcompositor = wl_registry_bind(
				wl_registry, name, &wl_subcompositor_interface, 1);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		state->xdg_wm_base = wl_registry_bind(
				wl_registry, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(state->xdg_wm_base,
				&xdg_wm_base_listener, state);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		state->wl_seat = wl_registry_bind(
						wl_registry, name, &wl_seat_interface, 5);
		wl_seat_add_listener(state->wl_seat,
						&wl_seat_listener, state);
	}
}

static void
registry_global_remove(void *data,
		struct wl_registry *wl_registry, uint32_t name)
{
	/* This space deliberately left blank */
}

static const struct wl_registry_listener wl_registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

int
main(int argc, char *argv[])
{
	struct client_state state = { 0 };
	state.width = 640;
	state.height = 480;
	state.wl_display = wl_display_connect(NULL);
	state.wl_registry = wl_display_get_registry(state.wl_display);
	wl_registry_add_listener(state.wl_registry, &wl_registry_listener, &state);
	wl_display_roundtrip(state.wl_display);

	//cursor_surface = wl_compositor_create_surface(state.wl_compositor);
	//wl_surface_commit(cursor_surface);


	state.wl_surface = wl_compositor_create_surface(state.wl_compositor);

	//struct wl_region *empty = wl_compositor_create_region(state.wl_compositor);
	//wl_surface_set_input_region(state.wl_surface, empty);
	//wl_region_destroy(empty);
	//wl_surface_set_opaque_region(state.wl_surface, state.wl_region);

	state.xdg_surface = xdg_wm_base_get_xdg_surface(
			state.xdg_wm_base, state.wl_surface);
	xdg_surface_add_listener(state.xdg_surface, &xdg_surface_listener, &state);
	state.xdg_toplevel = xdg_surface_get_toplevel(state.xdg_surface);
	xdg_toplevel_add_listener(state.xdg_toplevel,
			&xdg_toplevel_listener, &state);
	xdg_toplevel_set_title(state.xdg_toplevel, "Example client");
	wl_surface_commit(state.wl_surface);

	struct wl_callback *cb = wl_surface_frame(state.wl_surface);
	wl_callback_add_listener(cb, &wl_surface_frame_listener, &state);


	while (wl_display_dispatch(state.wl_display) && state.closed != true) {
		/* This space deliberately left blank */
	}

	return 0;
}
