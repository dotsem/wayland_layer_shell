#include "include/wayland_layer_shell/wayland_layer_shell_plugin.h"

#include <flutter_linux/flutter_linux.h>
#include <gtk/gtk.h>
#include <sys/utsname.h>

#include <cstring>
#include <iostream>
#include <map>
#include <string>

#include "wayland_layer_shell_plugin_private.h"

#include <gtk-layer-shell/gtk-layer-shell.h>

#define WAYLAND_LAYER_SHELL_PLUGIN(obj)                                        \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), wayland_layer_shell_plugin_get_type(),    \
                              WaylandLayerShellPlugin))

// Global map to track initialized windows
static std::map<GtkWindow *, bool> initialized_windows;

struct _WaylandLayerShellPlugin {
  GObject parent_instance;
  FlPluginRegistrar *registrar;
  GtkWindow
      *target_window; // Store the specific window this plugin instance manages
};

G_DEFINE_TYPE(WaylandLayerShellPlugin, wayland_layer_shell_plugin,
              g_object_get_type())

GtkWindow *get_window(WaylandLayerShellPlugin *self) {
  // If we have a cached target window, use it
  if (self->target_window != nullptr) {
    return self->target_window;
  }

  // Otherwise get from the registrar (for backwards compatibility)
  FlView *view = fl_plugin_registrar_get_view(self->registrar);
  if (view == nullptr)
    return nullptr;

  GtkWindow *window = GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(view)));

  // Cache this window for this plugin instance
  self->target_window = window;

  return window;
}

static FlMethodResponse *is_supported(WaylandLayerShellPlugin *self) {
  g_autoptr(FlValue) result = fl_value_new_bool(gtk_layer_is_supported());
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
}

static FlMethodResponse *initialize(WaylandLayerShellPlugin *self,
                                    FlValue *args) {
  GtkWindow *gtk_window = get_window(self);

  if (gtk_window == nullptr) {
    std::cout << "ERROR: Could not get GTK window" << std::endl;
    g_autoptr(FlValue) result = fl_value_new_bool(false);
    return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  }

  if (gtk_layer_is_supported() == 0) {
    std::cout << "ERROR: Layer shell not supported" << std::endl;
    g_autoptr(FlValue) result = fl_value_new_bool(false);
    return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  }

  // Check if this window is already initialized
  if (initialized_windows.find(gtk_window) != initialized_windows.end() &&
      initialized_windows[gtk_window]) {
    std::cout << "Window already initialized for layer shell" << std::endl;
    g_autoptr(FlValue) result = fl_value_new_bool(true);
    return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  }

  // Hide the window first if it's already shown
  if (gtk_widget_get_mapped(GTK_WIDGET(gtk_window))) {
    std::cout << "Window already mapped, hiding before layer shell init"
              << std::endl;
    gtk_widget_hide(GTK_WIDGET(gtk_window));

    // Wait for the hide to take effect
    while (gtk_widget_get_mapped(GTK_WIDGET(gtk_window))) {
      gtk_main_iteration_do(FALSE);
    }
  }

  int width = fl_value_get_int(fl_value_lookup_string(args, "width"));
  int height = fl_value_get_int(fl_value_lookup_string(args, "height"));

  gtk_widget_set_size_request(GTK_WIDGET(gtk_window), width, height);

  // Remove decorations for layer shell
  gtk_window_set_decorated(gtk_window, FALSE);

  // Initialize layer shell for this specific window
  gtk_layer_init_for_window(gtk_window);

  // Set layer shell properties IMMEDIATELY after initialization
  gtk_layer_set_layer(gtk_window, GTK_LAYER_SHELL_LAYER_TOP);
  gtk_layer_set_anchor(gtk_window, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
  gtk_layer_set_anchor(gtk_window, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
  gtk_layer_set_anchor(gtk_window, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
  gtk_layer_set_anchor(gtk_window, GTK_LAYER_SHELL_EDGE_TOP, FALSE);

  // Set margins to 0
  gtk_layer_set_margin(gtk_window, GTK_LAYER_SHELL_EDGE_BOTTOM, 0);
  gtk_layer_set_margin(gtk_window, GTK_LAYER_SHELL_EDGE_LEFT, 0);
  gtk_layer_set_margin(gtk_window, GTK_LAYER_SHELL_EDGE_RIGHT, 0);

  // Enable auto exclusive zone
  gtk_layer_auto_exclusive_zone_enable(gtk_window);

  // Set keyboard mode
  gtk_layer_set_keyboard_mode(gtk_window,
                              GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);

  // FIXED: Try to set monitor from args if provided
  FlValue *monitor_value = fl_value_lookup_string(args, "monitor");
  if (monitor_value != nullptr) {
    // Check if it's a string (old format) or a Monitor object (new format)
    if (fl_value_get_type(monitor_value) == FL_VALUE_TYPE_STRING) {
      // Handle string format: "0:24G1WG4"
      const gchar *monitor_str = fl_value_get_string(monitor_value);
      std::cout << "Monitor string provided: " << monitor_str << std::endl;
      
      // Parse monitor_str (e.g., "0:24G1WG4") to extract index
      gchar **parts = g_strsplit(monitor_str, ":", 2);
      if (parts != nullptr && parts[0] != nullptr) {
        gint monitor_index = g_ascii_strtoll(parts[0], nullptr, 10);
        std::cout << "Parsed monitor index: " << monitor_index << std::endl;
        g_strfreev(parts);
        
        // Use monitor_index to set the monitor
        GdkDisplay *display = gdk_display_get_default();
        if (monitor_index >= 0 &&
            monitor_index < gdk_display_get_n_monitors(display)) {
          GdkMonitor *monitor = gdk_display_get_monitor(display, monitor_index);
          gtk_layer_set_monitor(gtk_window, monitor);
          std::cout << "Set monitor " << monitor_index << " during initialization" << std::endl;
        } else {
          std::cout << "Invalid monitor index: " << monitor_index << std::endl;
        }
      } else {
        std::cout << "Failed to parse monitor string: " << monitor_str << std::endl;
      }
    } else {
      // For non-string monitor values, try to handle them appropriately
      std::cout << "Monitor parameter is not a string, type: " << fl_value_get_type(monitor_value) << std::endl;
      
      // If it's an int, treat it as a monitor index
      if (fl_value_get_type(monitor_value) == FL_VALUE_TYPE_INT) {
        gint monitor_index = fl_value_get_int(monitor_value);
        std::cout << "Monitor index provided: " << monitor_index << std::endl;
        
        GdkDisplay *display = gdk_display_get_default();
        if (monitor_index >= 0 &&
            monitor_index < gdk_display_get_n_monitors(display)) {
          GdkMonitor *monitor = gdk_display_get_monitor(display, monitor_index);
          gtk_layer_set_monitor(gtk_window, monitor);
          std::cout << "Set monitor " << monitor_index << " during initialization" << std::endl;
        } else {
          std::cout << "Invalid monitor index: " << monitor_index << std::endl;
        }
      }
    }
  } else {
    std::cout << "No monitor parameter provided" << std::endl;
  }

  // Mark this window as initialized
  initialized_windows[gtk_window] = true;

  std::cout << "Initialized layer shell for window: " << gtk_window
            << std::endl;

  g_autoptr(FlValue) result = fl_value_new_bool(true);
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
}

static FlMethodResponse *show_window(WaylandLayerShellPlugin *self) {
  GtkWindow *gtk_window = get_window(self);
  if (gtk_window == nullptr) {
    g_autoptr(FlValue) result = fl_value_new_bool(false);
    return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  }

  gtk_widget_show(GTK_WIDGET(gtk_window));
  g_autoptr(FlValue) result = fl_value_new_bool(true);
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
}

static FlMethodResponse *set_layer(WaylandLayerShellPlugin *self,
                                   FlValue *args) {
  GtkWindow *window = get_window(self);
  if (window == nullptr) {
    g_autoptr(FlValue) result = fl_value_new_bool(false);
    return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  }

  int layer = fl_value_get_int(fl_value_lookup_string(args, "layer"));
  gtk_layer_set_layer(window, static_cast<GtkLayerShellLayer>(layer));
  g_autoptr(FlValue) result = fl_value_new_bool(true);
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
}

static FlMethodResponse *get_layer(WaylandLayerShellPlugin *self) {
  GtkWindow *window = get_window(self);
  if (window == nullptr) {
    g_autoptr(FlValue) result = fl_value_new_int(0);
    return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  }

  g_autoptr(FlValue) result = fl_value_new_int(gtk_layer_get_layer(window));
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
}

static FlMethodResponse *get_monitor_list(WaylandLayerShellPlugin *self) {
  GdkDisplay *display = gdk_display_get_default();
  g_autoptr(FlValue) result = fl_value_new_list();
  for (int i = 0; i < gdk_display_get_n_monitors(display); i++) {
    GdkMonitor *monitor = gdk_display_get_monitor(display, i);
    gchar *val = g_strdup_printf("%i:%s", i, gdk_monitor_get_model(monitor));
    fl_value_append_take(result, fl_value_new_string(val));
  }
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
}

static FlMethodResponse *set_monitor(WaylandLayerShellPlugin *self,
                                     FlValue *args) {
  GtkWindow *window = get_window(self);
  if (window == nullptr) {
    g_autoptr(FlValue) result = fl_value_new_bool(false);
    return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  }

  // FIXED: Handle different monitor parameter types
  FlValue *monitor_value = fl_value_lookup_string(args, "id");
  if (monitor_value == nullptr) {
    std::cout << "No monitor ID provided" << std::endl;
    g_autoptr(FlValue) result = fl_value_new_bool(false);
    return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  }

  gint monitor_index = -1;
  
  if (fl_value_get_type(monitor_value) == FL_VALUE_TYPE_STRING) {
    // Handle string format: "0:24G1WG4"
    const gchar *monitor_str = fl_value_get_string(monitor_value);
    std::cout << "Monitor string: " << monitor_str << std::endl;
    
    // Parse the monitor string (format: "index:name")
    gchar **parts = g_strsplit(monitor_str, ":", 2);
    if (parts != nullptr && parts[0] != nullptr) {
      monitor_index = g_ascii_strtoll(parts[0], nullptr, 10);
      g_strfreev(parts);
    }
  } else if (fl_value_get_type(monitor_value) == FL_VALUE_TYPE_INT) {
    // Handle integer format
    monitor_index = fl_value_get_int(monitor_value);
    std::cout << "Monitor index: " << monitor_index << std::endl;
  } else {
    std::cout << "ERROR: Monitor parameter type not supported: " << fl_value_get_type(monitor_value) << std::endl;
    g_autoptr(FlValue) result = fl_value_new_bool(false);
    return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  }

  GdkDisplay *display = gdk_display_get_default();

  if (monitor_index == -1) {
    gtk_layer_set_monitor(window, NULL);
    std::cout << "Set monitor to NULL for window: " << window << std::endl;
  } else if (monitor_index >= 0 &&
             monitor_index < gdk_display_get_n_monitors(display)) {
    GdkMonitor *monitor = gdk_display_get_monitor(display, monitor_index);
    gtk_layer_set_monitor(window, monitor);
    std::cout << "Set monitor " << monitor_index << " for window: " << window
              << std::endl;
  } else {
    std::cout << "Invalid monitor index: " << monitor_index << std::endl;
    g_autoptr(FlValue) result = fl_value_new_bool(false);
    return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  }

  g_autoptr(FlValue) result = fl_value_new_bool(true);
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
}

static FlMethodResponse *set_anchor(WaylandLayerShellPlugin *self,
                                    FlValue *args) {
  GtkWindow *window = get_window(self);
  if (window == nullptr) {
    g_autoptr(FlValue) result = fl_value_new_bool(false);
    return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  }

  int edge = fl_value_get_int(fl_value_lookup_string(args, "edge"));
  gboolean anchor_to_edge =
      fl_value_get_bool(fl_value_lookup_string(args, "anchor_to_edge"));

  gtk_layer_set_anchor(window, static_cast<GtkLayerShellEdge>(edge),
                       anchor_to_edge);
  g_autoptr(FlValue) result = fl_value_new_bool(true);
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
}

static FlMethodResponse *get_anchor(WaylandLayerShellPlugin *self,
                                    FlValue *args) {
  GtkWindow *window = get_window(self);
  if (window == nullptr) {
    g_autoptr(FlValue) result = fl_value_new_bool(false);
    return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  }

  int edge = fl_value_get_int(fl_value_lookup_string(args, "edge"));
  g_autoptr(FlValue) result = fl_value_new_bool(
      gtk_layer_get_anchor(window, static_cast<GtkLayerShellEdge>(edge)));
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
}

static FlMethodResponse *set_margin(WaylandLayerShellPlugin *self,
                                    FlValue *args) {
  GtkWindow *window = get_window(self);
  if (window == nullptr) {
    g_autoptr(FlValue) result = fl_value_new_bool(false);
    return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  }

  int edge = fl_value_get_int(fl_value_lookup_string(args, "edge"));
  int margin_size =
      fl_value_get_int(fl_value_lookup_string(args, "margin_size"));

  gtk_layer_set_margin(window, static_cast<GtkLayerShellEdge>(edge),
                       margin_size);
  g_autoptr(FlValue) result = fl_value_new_bool(true);
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
}

static FlMethodResponse *get_margin(WaylandLayerShellPlugin *self,
                                    FlValue *args) {
  GtkWindow *window = get_window(self);
  if (window == nullptr) {
    g_autoptr(FlValue) result = fl_value_new_int(0);
    return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  }

  int edge = fl_value_get_int(fl_value_lookup_string(args, "edge"));
  g_autoptr(FlValue) result = fl_value_new_int(
      gtk_layer_get_margin(window, static_cast<GtkLayerShellEdge>(edge)));
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
}

static FlMethodResponse *set_exclusive_zone(WaylandLayerShellPlugin *self,
                                            FlValue *args) {
  GtkWindow *window = get_window(self);
  if (window == nullptr) {
    g_autoptr(FlValue) result = fl_value_new_bool(false);
    return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  }

  int exclusive_zone =
      fl_value_get_int(fl_value_lookup_string(args, "exclusive_zone"));
  gtk_layer_set_exclusive_zone(window, exclusive_zone);
  g_autoptr(FlValue) result = fl_value_new_bool(true);
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
}

static FlMethodResponse *get_exclusive_zone(WaylandLayerShellPlugin *self) {
  GtkWindow *window = get_window(self);
  if (window == nullptr) {
    g_autoptr(FlValue) result = fl_value_new_int(0);
    return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  }

  g_autoptr(FlValue) result =
      fl_value_new_int(gtk_layer_get_exclusive_zone(window));
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
}

static FlMethodResponse *
enable_auto_exclusive_zone(WaylandLayerShellPlugin *self) {
  GtkWindow *window = get_window(self);
  if (window == nullptr) {
    g_autoptr(FlValue) result = fl_value_new_bool(false);
    return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  }

  gtk_layer_auto_exclusive_zone_enable(window);
  g_autoptr(FlValue) result = fl_value_new_bool(true);
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
}

static FlMethodResponse *
is_auto_exclusive_zone_enabled(WaylandLayerShellPlugin *self) {
  GtkWindow *window = get_window(self);
  if (window == nullptr) {
    g_autoptr(FlValue) result = fl_value_new_bool(false);
    return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  }

  g_autoptr(FlValue) result =
      fl_value_new_bool(gtk_layer_auto_exclusive_zone_is_enabled(window));
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
}

static FlMethodResponse *set_keyboard_mode(WaylandLayerShellPlugin *self,
                                           FlValue *args) {
  GtkWindow *window = get_window(self);
  if (window == nullptr) {
    g_autoptr(FlValue) result = fl_value_new_bool(false);
    return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  }

  int keyboard_mode =
      fl_value_get_int(fl_value_lookup_string(args, "keyboard_mode"));
  gtk_layer_set_keyboard_mode(
      window, static_cast<GtkLayerShellKeyboardMode>(keyboard_mode));
  g_autoptr(FlValue) result = fl_value_new_bool(true);
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
}

static FlMethodResponse *get_keyboard_mode(WaylandLayerShellPlugin *self) {
  GtkWindow *window = get_window(self);
  if (window == nullptr) {
    g_autoptr(FlValue) result = fl_value_new_int(0);
    return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  }

  g_autoptr(FlValue) result =
      fl_value_new_int(gtk_layer_get_keyboard_mode(window));
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
}

// Called when a method call is received from Flutter.
static void
wayland_layer_shell_plugin_handle_method_call(WaylandLayerShellPlugin *self,
                                              FlMethodCall *method_call) {
  g_autoptr(FlMethodResponse) response = nullptr;

  const gchar *method = fl_method_call_get_name(method_call);
  FlValue *args = fl_method_call_get_args(method_call);

  if (strcmp(method, "getPlatformVersion") == 0) {
    response = get_platform_version();
  } else if (strcmp(method, "isSupported") == 0) {
    response = is_supported(self);
  } else if (strcmp(method, "initialize") == 0) {
    response = initialize(self, args);
  } else if (strcmp(method, "showWindow") == 0) {
    response = show_window(self);
  } else if (strcmp(method, "setLayer") == 0) {
    response = set_layer(self, args);
  } else if (strcmp(method, "getLayer") == 0) {
    response = get_layer(self);
  } else if (strcmp(method, "getMonitorList") == 0) {
    response = get_monitor_list(self);
  } else if (strcmp(method, "setMonitor") == 0) {
    response = set_monitor(self, args);
  } else if (strcmp(method, "setAnchor") == 0) {
    response = set_anchor(self, args);
  } else if (strcmp(method, "getAnchor") == 0) {
    response = get_anchor(self, args);
  } else if (strcmp(method, "setMargin") == 0) {
    response = set_margin(self, args);
  } else if (strcmp(method, "getMargin") == 0) {
    response = get_margin(self, args);
  } else if (strcmp(method, "setExclusiveZone") == 0) {
    response = set_exclusive_zone(self, args);
  } else if (strcmp(method, "getExclusiveZone") == 0) {
    response = get_exclusive_zone(self);
  } else if (strcmp(method, "enableAutoExclusiveZone") == 0) {
    response = enable_auto_exclusive_zone(self);
  } else if (strcmp(method, "isAutoExclusiveZoneEnabled") == 0) {
    response = is_auto_exclusive_zone_enabled(self);
  } else if (strcmp(method, "setKeyboardMode") == 0) {
    response = set_keyboard_mode(self, args);
  } else if (strcmp(method, "getKeyboardMode") == 0) {
    response = get_keyboard_mode(self);
  } else {
    response = FL_METHOD_RESPONSE(fl_method_not_implemented_response_new());
  }

  fl_method_call_respond(method_call, response, nullptr);
}

FlMethodResponse *get_platform_version() {
  struct utsname uname_data = {};
  uname(&uname_data);
  g_autofree gchar *version = g_strdup_printf("Linux %s", uname_data.version);
  g_autoptr(FlValue) result = fl_value_new_string(version);
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
}

static void wayland_layer_shell_plugin_dispose(GObject *object) {
  WaylandLayerShellPlugin *self = WAYLAND_LAYER_SHELL_PLUGIN(object);

  // Clean up our window tracking
  if (self->target_window != nullptr) {
    initialized_windows.erase(self->target_window);
    self->target_window = nullptr;
  }

  G_OBJECT_CLASS(wayland_layer_shell_plugin_parent_class)->dispose(object);
}

static void
wayland_layer_shell_plugin_class_init(WaylandLayerShellPluginClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = wayland_layer_shell_plugin_dispose;
}

static void wayland_layer_shell_plugin_init(WaylandLayerShellPlugin *self) {
  self->target_window = nullptr;
}

static void method_call_cb(FlMethodChannel *channel, FlMethodCall *method_call,
                           gpointer user_data) {
  WaylandLayerShellPlugin *plugin = WAYLAND_LAYER_SHELL_PLUGIN(user_data);
  wayland_layer_shell_plugin_handle_method_call(plugin, method_call);
}

void wayland_layer_shell_plugin_register_with_registrar(
    FlPluginRegistrar *registrar) {
  WaylandLayerShellPlugin *plugin = WAYLAND_LAYER_SHELL_PLUGIN(
      g_object_new(wayland_layer_shell_plugin_get_type(), nullptr));

  plugin->registrar = FL_PLUGIN_REGISTRAR(g_object_ref(registrar));

  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  g_autoptr(FlMethodChannel) channel =
      fl_method_channel_new(fl_plugin_registrar_get_messenger(registrar),
                            "wayland_layer_shell", FL_METHOD_CODEC(codec));
  fl_method_channel_set_method_call_handler(
      channel, method_call_cb, g_object_ref(plugin), g_object_unref);

  g_object_unref(plugin);
}