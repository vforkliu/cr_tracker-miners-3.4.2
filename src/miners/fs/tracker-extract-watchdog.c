/*
 * Copyright (C) 2016, Red Hat Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config-miners.h"

#include "tracker-extract-watchdog.h"

#include <libtracker-miners-common/tracker-common.h>
#include <libtracker-miner/tracker-miner.h>

enum {
	STATUS,
	LOST,
	N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0, };

struct _TrackerExtractWatchdog {
	GObject parent_class;
	GDBusConnection *conn;
	gchar *domain;
	guint extractor_watchdog_id;
	guint progress_signal_id;
	gboolean initializing;
};

static void extract_watchdog_start (TrackerExtractWatchdog *watchdog,
				    gboolean                autostart);

G_DEFINE_TYPE (TrackerExtractWatchdog, tracker_extract_watchdog, G_TYPE_OBJECT)

static void
extract_watchdog_stop (TrackerExtractWatchdog *watchdog)
{
	if (watchdog->conn && watchdog->progress_signal_id) {
		g_dbus_connection_signal_unsubscribe (watchdog->conn,
		                                      watchdog->progress_signal_id);
		watchdog->progress_signal_id = 0;
		watchdog->conn = NULL;
	}

	if (watchdog->extractor_watchdog_id) {
		g_bus_unwatch_name (watchdog->extractor_watchdog_id);
		watchdog->extractor_watchdog_id = 0;
	}
}

static void
on_extract_progress_cb (GDBusConnection *conn,
                        const gchar     *sender_name,
                        const gchar     *object_path,
                        const gchar     *interface_name,
                        const gchar     *signal_name,
                        GVariant        *parameters,
                        gpointer         user_data)
{
	TrackerExtractWatchdog *watchdog = user_data;
	const gchar *status;
	gdouble progress;
	gint32 remaining;

	g_variant_get (parameters, "(&sdi)",
	               &status, &progress, &remaining);
	g_signal_emit (watchdog, signals[STATUS], 0,
	               status, progress, (gint) remaining);
}

static void
extract_watchdog_name_appeared (GDBusConnection *conn,
				const gchar     *name,
				const gchar     *name_owner,
				gpointer         user_data)
{
	TrackerExtractWatchdog *watchdog = user_data;

	if (watchdog->initializing)
		watchdog->initializing = FALSE;

	watchdog->conn = conn;
	watchdog->progress_signal_id =
		g_dbus_connection_signal_subscribe (watchdog->conn,
		                                    "org.freedesktop.Tracker3.Miner.Extract",
		                                    "org.freedesktop.Tracker3.Miner",
		                                    "Progress",
		                                    "/org/freedesktop/Tracker3/Miner/Extract",
		                                    NULL,
		                                    G_DBUS_SIGNAL_FLAGS_NONE,
		                                    on_extract_progress_cb,
		                                    watchdog,
		                                    NULL);
}

static void
extract_watchdog_name_vanished (GDBusConnection *conn,
				const gchar     *name,
				gpointer         user_data)
{
	TrackerExtractWatchdog *watchdog = user_data;

	/* If connection is lost, there's not much we can startup */
	if (conn == NULL)
		return;

	/* Close the name watch, so we'll create another one that will
	 * autostart the service if it not already running.
	 */
	extract_watchdog_stop (watchdog);

	/* We will ignore the first call after initialization, as we
	 * don't want to autostart tracker-extract in this case (useful
	 * for debugging purposes).
	 */
	if (watchdog->initializing) {
		watchdog->initializing = FALSE;
		return;
	}

	g_signal_emit (watchdog, signals[STATUS], 0, "Idle", 1.0, 0);
	g_signal_emit (watchdog, signals[LOST], 0);
}

static void
extract_watchdog_start (TrackerExtractWatchdog *watchdog,
			gboolean                autostart)
{
	const gchar *domain_name = watchdog->domain;
	gchar *tracker_extract_dbus_name;

	if (domain_name == NULL) {
		tracker_extract_dbus_name = g_strdup (TRACKER_MINER_DBUS_NAME_PREFIX "Extract");
	} else {
		tracker_extract_dbus_name = g_strconcat (domain_name, ".Tracker3.Miner.Extract", NULL);
	}

	g_debug ("Setting up watch on tracker-extract at %s (autostart: %s)",
		 tracker_extract_dbus_name, autostart ? "yes" : "no");

	watchdog->extractor_watchdog_id =
		g_bus_watch_name (TRACKER_IPC_BUS,
				  tracker_extract_dbus_name,
				  (autostart ?
				   G_BUS_NAME_WATCHER_FLAGS_AUTO_START :
				   G_BUS_NAME_WATCHER_FLAGS_NONE),
				  extract_watchdog_name_appeared,
				  extract_watchdog_name_vanished,
				  watchdog, NULL);

	g_free (tracker_extract_dbus_name);
}

static void
tracker_extract_watchdog_finalize (GObject *object)
{
	TrackerExtractWatchdog *watchdog = TRACKER_EXTRACT_WATCHDOG (object);

	extract_watchdog_stop (watchdog);
	g_free (watchdog->domain);

	G_OBJECT_CLASS (tracker_extract_watchdog_parent_class)->finalize (object);
}

static void
tracker_extract_watchdog_class_init (TrackerExtractWatchdogClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = tracker_extract_watchdog_finalize;

	signals[STATUS] = g_signal_new ("status",
	                                G_OBJECT_CLASS_TYPE (object_class),
	                                G_SIGNAL_RUN_LAST,
	                                0, NULL, NULL, NULL,
	                                G_TYPE_NONE, 3,
	                                G_TYPE_STRING,
	                                G_TYPE_DOUBLE,
	                                G_TYPE_INT);
	signals[LOST] = g_signal_new ("lost",
	                              G_OBJECT_CLASS_TYPE (object_class),
	                              G_SIGNAL_RUN_LAST,
	                              0, NULL, NULL, NULL,
	                              G_TYPE_NONE, 0);
}

static void
tracker_extract_watchdog_init (TrackerExtractWatchdog *watchdog)
{
}

TrackerExtractWatchdog *
tracker_extract_watchdog_new (const gchar *domain)
{
	TrackerExtractWatchdog *watchdog;

	watchdog = g_object_new (TRACKER_TYPE_EXTRACT_WATCHDOG,
	                         NULL);

	watchdog->initializing = TRUE;
	watchdog->domain = g_strdup (domain);
	extract_watchdog_start (watchdog, FALSE);

	return watchdog;
}

void
tracker_extract_watchdog_ensure_started (TrackerExtractWatchdog *watchdog)
{
	if (!watchdog->extractor_watchdog_id)
		extract_watchdog_start (watchdog, TRUE);
}
