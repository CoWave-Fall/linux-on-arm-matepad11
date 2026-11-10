// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * loa-alsd: 把 SDSP 环境光传感器的照度与色温通过会话 D-Bus 提供给桌面。
 *
 * DBY-W09 的 TCS3408 由 SDSP 固件驱动，ambient_light 报文的第一项是
 * lux，第二项是 CCT。上游 libssc 只暴露 lux，本服务使用带 CCT 补丁的
 * libssc，把两个值以 org.loa.AmbientLight1 属性发布给 KWin 效果。
 *
 * 传感器依赖 hexagonrpcd 与 SDSP 固件，启动顺序不固定，因此连接失败时
 * 按固定间隔重试；D-Bus 名称与属性在传感器就绪前保持 Valid=false。
 */

#include <gio/gio.h>
#include <glib-unix.h>
#include <glib.h>
#include <libssc.h>
#include <math.h>
#include <signal.h>

#define SERVICE_NAME "org.loa.AmbientLight"
#define OBJECT_PATH "/org/loa/AmbientLight"
#define INTERFACE_NAME "org.loa.AmbientLight1"

/* 传感器未就绪时每 5 秒重试一次。 */
#define SENSOR_RETRY_SECONDS 5
/* 测量值到达频率为 10 Hz，D-Bus 属性按 1 Hz 节流，避免反复唤醒合成器。 */
#define PROPERTY_UPDATE_INTERVAL_US (1 * G_TIME_SPAN_SECOND)
/* 色温变化超过该值时不等待节流周期，立即发布。 */
#define CCT_UPDATE_THRESHOLD 50.0

static const gchar introspection_xml[] =
	"<node>"
	"  <interface name='" INTERFACE_NAME "'>"
	"    <property name='Lux' type='d' access='read'/>"
	"    <property name='Cct' type='d' access='read'/>"
	"    <property name='Valid' type='b' access='read'/>"
	"  </interface>"
	"  <interface name='org.freedesktop.DBus.Properties'>"
	"    <method name='Get'>"
	"      <arg name='interface_name' type='s' direction='in'/>"
	"      <arg name='property_name' type='s' direction='in'/>"
	"      <arg name='value' type='v' direction='out'/>"
	"    </method>"
	"    <method name='GetAll'>"
	"      <arg name='interface_name' type='s' direction='in'/>"
	"      <arg name='properties' type='a{sv}' direction='out'/>"
	"    </method>"
	"    <method name='Set'>"
	"      <arg name='interface_name' type='s' direction='in'/>"
	"      <arg name='property_name' type='s' direction='in'/>"
	"      <arg name='value' type='v' direction='in'/>"
	"    </method>"
	"    <signal name='PropertiesChanged'>"
	"      <arg name='interface_name' type='s'/>"
	"      <arg name='changed_properties' type='a{sv}'/>"
	"      <arg name='invalidated_properties' type='as'/>"
	"    </signal>"
	"  </interface>"
	"</node>";

typedef struct {
	GMainLoop *loop;
	GDBusConnection *bus;
	SSCSensorLight *sensor;
	guint sensor_retry_id;
	gboolean sensor_failed;
	gboolean name_lost;
	gdouble lux;
	gdouble cct;
	gboolean valid;
	gint64 last_emit_us;
} Daemon;

static Daemon app;

static void maybe_emit_properties_changed (Daemon *d, gboolean force);

/*****************************************************************************/

static GVariant *
property_value (Daemon *d, const gchar *name)
{
	if (g_strcmp0 (name, "Lux") == 0)
		return g_variant_new_double (d->lux);
	if (g_strcmp0 (name, "Cct") == 0)
		return g_variant_new_double (d->cct);
	if (g_strcmp0 (name, "Valid") == 0)
		return g_variant_new_boolean (d->valid);

	return NULL;
}

static GVariant *
properties_dictionary (Daemon *d)
{
	GVariantBuilder builder;

	g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
	g_variant_builder_add (&builder, "{sv}", "Lux", g_variant_new_double (d->lux));
	g_variant_builder_add (&builder, "{sv}", "Cct", g_variant_new_double (d->cct));
	g_variant_builder_add (&builder, "{sv}", "Valid", g_variant_new_boolean (d->valid));

	return g_variant_builder_end (&builder);
}

static void
maybe_emit_properties_changed (Daemon *d, gboolean force)
{
	gint64 now = g_get_monotonic_time ();
	GVariantBuilder changed;
	GVariantBuilder invalidated;
	GVariant *changed_variant;
	GVariant *invalidated_variant;
	GError *error = NULL;

	if (!force && now - d->last_emit_us < PROPERTY_UPDATE_INTERVAL_US)
		return;

	d->last_emit_us = now;

	g_variant_builder_init (&changed, G_VARIANT_TYPE ("a{sv}"));
	g_variant_builder_add (&changed, "{sv}", "Lux", g_variant_new_double (d->lux));
	g_variant_builder_add (&changed, "{sv}", "Cct", g_variant_new_double (d->cct));
	g_variant_builder_add (&changed, "{sv}", "Valid", g_variant_new_boolean (d->valid));
	changed_variant = g_variant_builder_end (&changed);

	g_variant_builder_init (&invalidated, G_VARIANT_TYPE ("as"));
	invalidated_variant = g_variant_builder_end (&invalidated);

	if (!g_dbus_connection_emit_signal (d->bus, NULL, OBJECT_PATH,
					    "org.freedesktop.DBus.Properties",
					    "PropertiesChanged",
					    g_variant_new ("(s@a{sv}@as)", INTERFACE_NAME,
							   changed_variant, invalidated_variant),
					    &error)) {
		g_warning ("Failed to emit PropertiesChanged: %s", error->message);
		g_error_free (error);
	}
}

/*****************************************************************************/

static void
sensor_measurement (SSCSensorLight *sensor, gfloat intensity, gpointer user_data)
{
	Daemon *d = user_data;

	(void) sensor;
	d->lux = intensity;
	d->valid = d->cct > 0.0;
	maybe_emit_properties_changed (d, FALSE);
}

static void
sensor_cct_measurement (SSCSensorLight *sensor, gfloat cct, gpointer user_data)
{
	Daemon *d = user_data;
	gboolean significant;

	(void) sensor;
	significant = !d->valid || fabs (d->cct - cct) >= CCT_UPDATE_THRESHOLD;

	d->cct = cct;
	d->valid = TRUE;
	maybe_emit_properties_changed (d, significant);
}

/*****************************************************************************/

static void
sensor_disconnect (Daemon *d)
{
	if (!d->sensor)
		return;

	ssc_sensor_light_close_sync (d->sensor, NULL, NULL);
	g_clear_object (&d->sensor);
	d->valid = FALSE;
	d->cct = 0.0;
	maybe_emit_properties_changed (d, TRUE);
}

static void
sensor_connect (Daemon *d);

static gboolean
sensor_retry (gpointer user_data)
{
	Daemon *d = user_data;

	d->sensor_retry_id = 0;
	sensor_connect (d);

	return G_SOURCE_REMOVE;
}

static void
sensor_schedule_retry (Daemon *d)
{
	if (!d->sensor_retry_id)
		d->sensor_retry_id = g_timeout_add_seconds (SENSOR_RETRY_SECONDS, sensor_retry, d);
}

/*
 * 使用异步接口连接传感器。同步接口在 SSC 服务缺失时会自旋等待约 100 秒，
 * 期间主循环无法处理 D-Bus 请求；异步接口让属性服务始终保持响应。
 */
static void
sensor_open_ready (SSCSensorLight *sensor, GAsyncResult *result, gpointer user_data)
{
	Daemon *d = user_data;
	GError *error = NULL;

	if (!ssc_sensor_light_open_finish (sensor, result, &error)) {
		if (!d->sensor_failed)
			g_warning ("Failed to open ambient light sensor: %s", error->message);
		d->sensor_failed = TRUE;
		g_clear_error (&error);
		g_clear_object (&d->sensor);
		sensor_schedule_retry (d);
		return;
	}

	d->sensor_failed = FALSE;
	g_message ("Ambient light sensor connected");
}

static void
sensor_new_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
	Daemon *d = user_data;
	GError *error = NULL;

	(void) source;
	d->sensor = ssc_sensor_light_new_finish (result, &error);
	if (!d->sensor) {
		if (!d->sensor_failed)
			g_warning ("Ambient light sensor unavailable: %s", error->message);
		d->sensor_failed = TRUE;
		g_clear_error (&error);
		sensor_schedule_retry (d);
		return;
	}

	g_signal_connect (d->sensor, "measurement", G_CALLBACK (sensor_measurement), d);
	g_signal_connect (d->sensor, "cct-measurement", G_CALLBACK (sensor_cct_measurement), d);

	ssc_sensor_light_open (d->sensor, NULL, (GAsyncReadyCallback)sensor_open_ready, d);
}

static void
sensor_connect (Daemon *d)
{
	d->sensor_retry_id = 0;
	ssc_sensor_light_new (NULL, sensor_new_ready, d);
}

static gboolean
sensor_connect_idle (gpointer user_data)
{
	sensor_connect (user_data);

	return G_SOURCE_REMOVE;
}

/*****************************************************************************/

static void
handle_properties_method (GDBusConnection *connection, const gchar *sender,
			  const gchar *object_path, const gchar *interface_name,
			  const gchar *method_name, GVariant *parameters,
			  GDBusMethodInvocation *invocation, gpointer user_data)
{
	Daemon *d = user_data;

	(void) connection;
	(void) sender;
	(void) object_path;
	(void) interface_name;

	if (g_strcmp0 (method_name, "Get") == 0) {
		const gchar *requested_interface = NULL;
		const gchar *requested_property = NULL;
		GVariant *value = NULL;

		g_variant_get (parameters, "(&s&s)", &requested_interface, &requested_property);

		if (g_strcmp0 (requested_interface, INTERFACE_NAME) != 0) {
			g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
							       G_DBUS_ERROR_UNKNOWN_INTERFACE,
							       "Unknown interface %s", requested_interface);
			return;
		}

		value = property_value (d, requested_property);
		if (!value) {
			g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
							       G_DBUS_ERROR_UNKNOWN_PROPERTY,
							       "Unknown property %s", requested_property);
			return;
		}

		g_dbus_method_invocation_return_value (invocation, g_variant_new ("(v)", value));
		return;
	}

	if (g_strcmp0 (method_name, "GetAll") == 0) {
		const gchar *requested_interface = NULL;

		g_variant_get (parameters, "(&s)", &requested_interface);

		if (g_strcmp0 (requested_interface, INTERFACE_NAME) != 0) {
			g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
							       G_DBUS_ERROR_UNKNOWN_INTERFACE,
							       "Unknown interface %s", requested_interface);
			return;
		}

		g_dbus_method_invocation_return_value (invocation,
						       g_variant_new ("(@a{sv})",
								      properties_dictionary (d)));
		return;
	}

	if (g_strcmp0 (method_name, "Set") == 0) {
		g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
						       G_DBUS_ERROR_PROPERTY_READ_ONLY,
						       "Properties are read-only");
		return;
	}

	g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
					       G_DBUS_ERROR_UNKNOWN_METHOD,
					       "Unknown method %s", method_name);
}

static void
handle_sensor_method (GDBusConnection *connection, const gchar *sender,
		      const gchar *object_path, const gchar *interface_name,
		      const gchar *method_name, GVariant *parameters,
		      GDBusMethodInvocation *invocation, gpointer user_data)
{
	(void) connection;
	(void) sender;
	(void) object_path;
	(void) interface_name;
	(void) parameters;
	(void) user_data;

	g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
					       G_DBUS_ERROR_UNKNOWN_METHOD,
					       "Unknown method %s", method_name);
}

/*****************************************************************************/

/*
 * 名称被其他实例占用或连接断开时退出，让 systemd 决定是否重新拉起，
 * 避免两个进程同时向同一个对象路径发布属性。
 */
static void
on_name_lost (GDBusConnection *connection, const gchar *name, gpointer user_data)
{
	Daemon *d = user_data;

	(void) connection;
	(void) name;
	g_warning ("Failed to own %s", SERVICE_NAME);
	d->name_lost = TRUE;
	g_main_loop_quit (d->loop);
}

static gboolean
on_terminate (gpointer user_data)
{
	Daemon *d = user_data;

	g_message ("Shutting down");
	if (d->sensor_retry_id)
		g_source_remove (d->sensor_retry_id);
	sensor_disconnect (d);
	g_main_loop_quit (d->loop);

	return G_SOURCE_REMOVE;
}

int
main (void)
{
	GDBusNodeInfo *node_info = NULL;
	GDBusInterfaceInfo *sensor_interface = NULL;
	GDBusInterfaceInfo *properties_interface = NULL;
	GDBusInterfaceVTable sensor_vtable = { handle_sensor_method, NULL, NULL, { 0 } };
	GDBusInterfaceVTable properties_vtable = { handle_properties_method, NULL, NULL, { 0 } };
	GError *error = NULL;
	guint sensor_registration = 0;
	guint properties_registration = 0;
	guint name_owner_id;
	guint terminate_id;

	app.loop = g_main_loop_new (NULL, FALSE);
	app.bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
	if (!app.bus) {
		g_printerr ("Failed to connect to the session bus: %s\n", error->message);
		g_error_free (error);
		return EXIT_FAILURE;
	}

	node_info = g_dbus_node_info_new_for_xml (introspection_xml, &error);
	if (!node_info) {
		g_printerr ("Failed to parse D-Bus interface: %s\n", error->message);
		g_error_free (error);
		return EXIT_FAILURE;
	}

	sensor_interface = g_dbus_node_info_lookup_interface (node_info, INTERFACE_NAME);
	properties_interface = g_dbus_node_info_lookup_interface (node_info,
								  "org.freedesktop.DBus.Properties");

	sensor_registration = g_dbus_connection_register_object (app.bus, OBJECT_PATH,
								 sensor_interface, &sensor_vtable,
								 &app, NULL, &error);
	if (!sensor_registration) {
		g_printerr ("Failed to register %s: %s\n", INTERFACE_NAME, error->message);
		g_error_free (error);
		return EXIT_FAILURE;
	}

	properties_registration = g_dbus_connection_register_object (app.bus, OBJECT_PATH,
								     properties_interface, &properties_vtable,
								     &app, NULL, &error);
	if (!properties_registration) {
		g_printerr ("Failed to register Properties interface: %s\n", error->message);
		g_error_free (error);
		return EXIT_FAILURE;
	}

	name_owner_id = g_bus_own_name_on_connection (app.bus, SERVICE_NAME,
						      G_BUS_NAME_OWNER_FLAGS_NONE,
						      NULL, on_name_lost, NULL, NULL);

	/* 先完成名称注册，再在空闲回调里启动异步传感器连接。 */
	g_idle_add (sensor_connect_idle, &app);

	terminate_id = g_unix_signal_add (SIGTERM, on_terminate, &app);
	(void) terminate_id;
	g_unix_signal_add (SIGINT, on_terminate, &app);

	g_main_loop_run (app.loop);

	g_bus_unown_name (name_owner_id);
	g_dbus_connection_unregister_object (app.bus, properties_registration);
	g_dbus_connection_unregister_object (app.bus, sensor_registration);
	g_dbus_node_info_unref (node_info);
	g_object_unref (app.bus);
	g_main_loop_unref (app.loop);

	return app.name_lost ? EXIT_FAILURE : EXIT_SUCCESS;
}
