/*
 * Copyright (C) 2025 The Chromium Authors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"

#include "wayland/meta-wayland-ui-controls.h"

#include <glib.h>
#include <linux/input-event-codes.h>
#include <stdint.h>
#include <wayland-server-core.h>

#include "clutter/clutter.h"
#include "compositor/meta-window-actor-private.h"
#include "gio/gio.h"
#include "glib-object.h"
#include "meta/meta-window-actor.h"
#include "ui-controls-unstable-v1-server-protocol.h"
#include "wayland/meta-wayland-private.h"
#include "wayland/meta-wayland-types.h"
#include "wayland/meta-wayland-versions.h"
#include "wayland/meta-wayland-xdg-shell.h"

typedef struct
{
  struct wl_resource *resource;
  uint32_t id;
  ClutterEventType type;
  uint64_t time_us;
} MetaWaylandUiControlsRequest;

typedef struct _MetaWaylandUiControls
{
  MetaWaylandCompositor *compositor;
  struct wl_global *global;
  ClutterVirtualInputDevice *virtual_pointer;
  ClutterVirtualInputDevice *virtual_keyboard;
  MetaWaylandEventHandler *event_handler;
  GQueue *requests;
} MetaWaylandUiControls;

static void
notify_request_done (MetaWaylandUiControlsRequest *request)
{

  meta_topic (META_DEBUG_INPUT, "%s %u", __func__, request->id);
  zcr_ui_controls_v1_send_request_processed (request->resource, request->id);
  g_free (request);
}

static void
on_wayland_input_event_handled (MetaWaylandInput      *input,
                                const ClutterEvent    *event,
                                MetaWaylandUiControls *ui_controls)
{
  int n;
  MetaWaylandUiControlsRequest *request;

  meta_topic (META_DEBUG_INPUT, "%s %s", __func__,
              clutter_event_get_name (event));
  for (n = 0; n < g_queue_get_length (ui_controls->requests); n++)
    {
      request = g_queue_peek_nth (ui_controls->requests, n);
      if (request->type == clutter_event_type (event) &&
          request->time_us == clutter_event_get_time_us (event))
        {
          g_queue_remove (ui_controls->requests, request);
          notify_request_done (request);
          break;
        }
    }
}

/**
 * Stores the request until it is processed and `request_processed` is sent.
 **/
static void
store_request (MetaWaylandUiControls *ui_controls,
               struct wl_resource    *resource,
               uint32_t               request_id,
               ClutterEventType       type,
               uint64_t               time_us)
{
  MetaWaylandUiControlsRequest *request;
  request = g_new0 (MetaWaylandUiControlsRequest, 1);
  request->resource = resource;
  request->id = request_id;
  request->type = type;
  request->time_us = time_us;
  g_queue_push_tail (ui_controls->requests, request);
}

static void
notify_modifiers (MetaWaylandUiControls *ui_controls,
                  uint32_t               pressed_modifiers,
                  uint32_t               key_state)
{
  if (pressed_modifiers != 0)
    {
      if (pressed_modifiers & ZCR_UI_CONTROLS_V1_MODIFIER_SHIFT)
        {
          clutter_virtual_input_device_notify_key (
            ui_controls->virtual_keyboard, g_get_monotonic_time (),
            KEY_LEFTSHIFT, key_state);
        }
      if (pressed_modifiers & ZCR_UI_CONTROLS_V1_MODIFIER_CONTROL)
        {
          clutter_virtual_input_device_notify_key (
            ui_controls->virtual_keyboard, g_get_monotonic_time (),
            KEY_LEFTCTRL, key_state);
        }
      if (pressed_modifiers & ZCR_UI_CONTROLS_V1_MODIFIER_ALT)
        {
          clutter_virtual_input_device_notify_key (
            ui_controls->virtual_keyboard, g_get_monotonic_time (),
            KEY_LEFTALT, key_state);
        }
    }
}

/**
 * Creates clutter virtual pointer and keyboard devices.
 **/
static void
create_virtual_input_devices (MetaWaylandUiControls *ui_controls)
{

  MetaWaylandCompositor *compositor = ui_controls->compositor;
  MetaContext *context =
    meta_wayland_compositor_get_context (compositor);
  MetaBackend *backend = meta_context_get_backend (context);
  ClutterBackend *clutter_backend =
    meta_backend_get_clutter_backend (backend);
  ClutterSeat *seat = clutter_backend_get_default_seat (clutter_backend);

  ui_controls->virtual_pointer =
    clutter_seat_create_virtual_device (seat, CLUTTER_POINTER_DEVICE);
  ui_controls->virtual_keyboard =
    clutter_seat_create_virtual_device (seat, CLUTTER_KEYBOARD_DEVICE);
}

static void
ui_controls_send_key_events (struct wl_client   *client,
                             struct wl_resource *resource,
                             uint32_t            key,
                             uint32_t            key_state,
                             uint32_t            pressed_modifiers,
                             uint32_t            id)
{
  MetaWaylandUiControls *ui_controls = wl_resource_get_user_data (resource);
  ClutterEventType event_type;
  uint64_t time_us;

  meta_topic (META_DEBUG_INPUT, "%s id=%u key=%u state=%u has_modifiers=%d",
              __func__, id, key, key_state, pressed_modifiers != 0);

  if (key_state & ZCR_UI_CONTROLS_V1_KEY_STATE_PRESS)
    {
      event_type = CLUTTER_KEY_PRESS;
      time_us = g_get_monotonic_time ();
      notify_modifiers (ui_controls, pressed_modifiers,
                        CLUTTER_KEY_STATE_PRESSED);
      clutter_virtual_input_device_notify_key (ui_controls->virtual_keyboard,
                                               time_us, key,
                                               CLUTTER_KEY_STATE_PRESSED);
    }
  if (key_state & ZCR_UI_CONTROLS_V1_KEY_STATE_RELEASE)
    {
      event_type = CLUTTER_KEY_RELEASE;
      time_us = g_get_monotonic_time ();
      clutter_virtual_input_device_notify_key (ui_controls->virtual_keyboard,
                                               time_us, key,
                                               CLUTTER_KEY_STATE_RELEASED);
      notify_modifiers (ui_controls, pressed_modifiers,
                        CLUTTER_KEY_STATE_RELEASED);
    }

  store_request (ui_controls, resource, id, event_type, time_us);
}

static void
ui_controls_send_mouse_move (struct wl_client   *client,
                             struct wl_resource *resource,
                             int32_t             x,
                             int32_t             y,
                             struct wl_resource *surface_resource,
                             uint32_t            id)
{
  MetaWaylandXdgSurface *xdg_surface;
  MetaWaylandSurfaceRole *surface_role;
  MetaWaylandSurface *surface;
  MetaWindow *window;
  MetaWindowActor *window_actor;
  MetaWaylandUiControls *ui_controls = wl_resource_get_user_data (resource);
  double abs_x, abs_y;
  uint64_t time_us = g_get_monotonic_time ();

  meta_topic (META_DEBUG_INPUT, "%s id=%u x=%d y=%d has_surface=%d", __func__,
              id, x, y, surface_resource != 0);

  if (surface_resource)
  // A surface was provided. Use coordinates relative to it.
    {
      xdg_surface = wl_resource_get_user_data (surface_resource);
      surface_role = META_WAYLAND_SURFACE_ROLE (xdg_surface);
      surface = meta_wayland_surface_role_get_surface (surface_role);
      window = meta_wayland_surface_get_window (surface);
      window_actor = meta_window_actor_from_window (window);
      meta_window_actor_transform_relative_position (window_actor, x, y, &abs_x,
                                                     &abs_y);
    }
  else
  // No surface was provided. Use global coordinates.
    {
      abs_x = x;
      abs_y = y;
    }
  clutter_virtual_input_device_notify_absolute_motion (
    ui_controls->virtual_pointer, time_us, abs_x, abs_y);
  store_request (ui_controls, resource, id, CLUTTER_MOTION, time_us);
}

static void
ui_controls_send_mouse_button (struct wl_client   *client,
                               struct wl_resource *resource,
                               uint32_t            button,
                               uint32_t            button_state,
                               uint32_t            pressed_modifiers,
                               uint32_t            id)
{
  MetaWaylandUiControls *ui_controls = wl_resource_get_user_data (resource);
  uint32_t clutter_button;
  ClutterEventType event_type;
  uint64_t time_us;

  meta_topic (META_DEBUG_INPUT, "%s id=%u button=%u state=%u has_modifiers=%d",
              __func__, id, button, button_state, pressed_modifiers != 0);

  switch (button)
    {
    case ZCR_UI_CONTROLS_V1_MOUSE_BUTTON_LEFT:
      clutter_button = CLUTTER_BUTTON_PRIMARY;
      break;
    case ZCR_UI_CONTROLS_V1_MOUSE_BUTTON_RIGHT:
      clutter_button = CLUTTER_BUTTON_SECONDARY;
      break;
    case ZCR_UI_CONTROLS_V1_MOUSE_BUTTON_MIDDLE:
      clutter_button = CLUTTER_BUTTON_MIDDLE;
      break;
    default:
      g_assert_not_reached ();
    }

  if (button_state & ZCR_UI_CONTROLS_V1_MOUSE_BUTTON_STATE_DOWN)
    {
      event_type = CLUTTER_BUTTON_PRESS;
      time_us = g_get_monotonic_time ();
      notify_modifiers (ui_controls, pressed_modifiers,
                        CLUTTER_KEY_STATE_PRESSED);
      clutter_virtual_input_device_notify_button (ui_controls->virtual_pointer,
                                                  time_us, clutter_button,
                                                  CLUTTER_BUTTON_STATE_PRESSED);
    }

  if (button_state & ZCR_UI_CONTROLS_V1_MOUSE_BUTTON_STATE_UP)
    {
      event_type = CLUTTER_BUTTON_RELEASE;
      time_us = g_get_monotonic_time ();
      clutter_virtual_input_device_notify_button (
        ui_controls->virtual_pointer, time_us, clutter_button,
        CLUTTER_BUTTON_STATE_RELEASED);
      notify_modifiers (ui_controls, pressed_modifiers,
                        CLUTTER_KEY_STATE_RELEASED);
    }

  store_request (ui_controls, resource, id, event_type, time_us);
}

static void
ui_controls_send_touch (struct wl_client   *client,
                        struct wl_resource *resource,
                        uint32_t            action,
                        uint32_t            touch_id,
                        int32_t             x,
                        int32_t             y,
                        struct wl_resource *surface,
                        uint32_t            id)
{
  g_error ("%s: Unsupported", __func__);
}

static void
ui_controls_set_display_info_id (struct wl_client   *client,
                                 struct wl_resource *resource,
                                 uint32_t            display_id_hi,
                                 uint32_t            display_id_low)
{
  g_error ("%s: Unsupported", __func__);
}
static void
ui_controls_set_display_info_size (struct wl_client   *client,
                                   struct wl_resource *resource,
                                   uint32_t            width,
                                   uint32_t            height)
{
  g_error ("%s: Unsupported", __func__);
}

static void
ui_controls_set_display_info_device_scale_factor (
  struct wl_client   *client,
  struct wl_resource *resource,
  uint32_t            device_scale_factor_as_uint)
{
  g_error ("%s: Unsupported", __func__);
}
static void
ui_controls_display_info_done (struct wl_client   *client,
                               struct wl_resource *resource)
{
  g_error ("%s: Unsupported", __func__);
}

static void
ui_controls_display_info_list_done (struct wl_client   *client,
                                    struct wl_resource *resource,
                                    uint32_t            id)
{
  g_error ("%s: Unsupported", __func__);
}

static const struct zcr_ui_controls_v1_interface meta_ui_controls_interface = {
  ui_controls_send_key_events,
  ui_controls_send_mouse_move,
  ui_controls_send_mouse_button,
  ui_controls_send_touch,
  ui_controls_set_display_info_id,
  ui_controls_set_display_info_size,
  ui_controls_set_display_info_device_scale_factor,
  ui_controls_display_info_done,
  ui_controls_display_info_list_done,
};

static void
destroy_ui_controls (struct wl_resource *resource)
{
  MetaWaylandUiControls *ui_controls = wl_resource_get_user_data (resource);

  meta_topic (META_DEBUG_INPUT, "%s", __func__);

  // Ensure any pressed keys and buttons are released when a client resource is
  // destroyed.
  clutter_virtual_input_device_release_pressed (ui_controls->virtual_pointer);
  clutter_virtual_input_device_release_pressed (ui_controls->virtual_keyboard);
  g_queue_clear_full (ui_controls->requests, g_free);
}

static void
bind_ui_controls (struct wl_client *client,
                  void             *data,
                  uint32_t          version,
                  uint32_t          id)
{
  MetaWaylandUiControls *ui_controls = data;
  struct wl_resource *resource;

  meta_topic (META_DEBUG_INPUT, "%s id=%u requested_version=%u", __func__, id,
              version);

  resource = wl_resource_create (client, &zcr_ui_controls_v1_interface,
                                 META_UI_CONTROLS_V1_VERSION, id);
  wl_resource_set_implementation (resource, &meta_ui_controls_interface,
                                  ui_controls, &destroy_ui_controls);
}

static MetaWaylandUiControls *
meta_wayland_ui_controls_new (MetaWaylandCompositor *compositor)
{
  MetaWaylandUiControls *ui_controls;

  ui_controls = g_new0 (MetaWaylandUiControls, 1);
  ui_controls->compositor = compositor;

  return ui_controls;
}

void
meta_wayland_ui_controls_init (MetaWaylandCompositor *compositor)
{
  struct wl_display *wayland_display;

  compositor->ui_controls = meta_wayland_ui_controls_new (compositor);
  create_virtual_input_devices (compositor->ui_controls);

  if (!compositor->ui_controls->global)
    {
      wayland_display =
        meta_wayland_compositor_get_wayland_display (compositor);

      compositor->ui_controls->global =
        wl_global_create (wayland_display,
                          &zcr_ui_controls_v1_interface,
                          META_UI_CONTROLS_V1_VERSION,
                          compositor->ui_controls, bind_ui_controls);
      if (!compositor->ui_controls->global)
        g_error ("Could not create ui controls global");
    }
  compositor->ui_controls->requests = g_queue_new ();
  g_signal_connect (compositor->seat->input_handler, "event-handled",
                    G_CALLBACK (on_wayland_input_event_handled),
                    compositor->ui_controls);
}

void
meta_wayland_ui_controls_finalize (MetaWaylandCompositor *compositor)
{
  g_clear_pointer (&compositor->ui_controls, g_free);
}
