/*
 * Copyright (C) 2006, Jamie McCracken <jamiemcc@gnome.org>
 * Copyright (C) 2008, Nokia <ivan.frade@nokia.com>
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

#define _XOPEN_SOURCE
#include <time.h>
#include <stdlib.h>
#include <locale.h>
#include <errno.h>
#include <sys/types.h>
#include <unistd.h>

#include <glib-object.h>
#include <glib-unix.h>
#include <glib/gi18n.h>
#include <glib/gprintf.h>
#include <gio/gio.h>

#ifndef G_OS_WIN32
#include <sys/resource.h>
#endif

#include <libtracker-miners-common/tracker-common.h>

#include "tracker-config.h"
#include "tracker-main.h"
#include "tracker-extract.h"
#include "tracker-extract-controller.h"
#include "tracker-extract-decorator.h"

#ifdef THREAD_ENABLE_TRACE
#warning Main thread traces enabled
#endif /* THREAD_ENABLE_TRACE */

#define ABOUT	  \
	"Tracker " PACKAGE_VERSION "\n"

#define LICENSE	  \
	"This program is free software and comes without any warranty.\n" \
	"It is licensed under version 2 or later of the General Public " \
	"License which can be viewed at:\n" \
	"\n" \
	"  http://www.gnu.org/licenses/gpl.txt\n"

#define DBUS_NAME_SUFFIX "Tracker3.Miner.Extract"
#define MINER_FS_NAME_SUFFIX "Tracker3.Miner.Files"
#define DBUS_PATH "/org/freedesktop/Tracker3/Miner/Extract"

static GMainLoop *main_loop;

static gchar *filename;
static gchar *mime_type;
static gchar *force_module;
static gchar *output_format_name;
static gboolean version;
static gchar *domain_ontology_name = NULL;
static guint shutdown_timeout_id = 0;

static TrackerConfig *config;

static GOptionEntry entries[] = {
	{ "file", 'f', 0,
	  G_OPTION_ARG_FILENAME, &filename,
	  N_("File to extract metadata for"),
	  N_("FILE") },
	{ "mime", 't', 0,
	  G_OPTION_ARG_STRING, &mime_type,
	  N_("MIME type for file (if not provided, this will be guessed)"),
	  N_("MIME") },
	{ "force-module", 'm', 0,
	  G_OPTION_ARG_STRING, &force_module,
	  N_("Force a module to be used for extraction (e.g. “foo” for “foo.so”)"),
	  N_("MODULE") },
	{ "output-format", 'o', 0, G_OPTION_ARG_STRING, &output_format_name,
	  N_("Output results format: “sparql”, “turtle” or “json-ld”"),
	  N_("FORMAT") },
	{ "domain-ontology", 'd', 0,
	  G_OPTION_ARG_STRING, &domain_ontology_name,
	  N_("Runs for a specific domain ontology"),
	  NULL },
	{ "version", 'V', 0,
	  G_OPTION_ARG_NONE, &version,
	  N_("Displays version information"),
	  NULL },
	{ NULL }
};

static void
initialize_priority_and_scheduling (void)
{
	/* Set CPU priority */
	tracker_sched_idle ();

	/* Set disk IO priority and scheduling */
	tracker_ioprio_init ();

	/* Set process priority:
	 * The nice() function uses attribute "warn_unused_result" and
	 * so complains if we do not check its returned value. But it
	 * seems that since glibc 2.2.4, nice() can return -1 on a
	 * successful call so we have to check value of errno too.
	 * Stupid...
	 */
	TRACKER_NOTE (CONFIG, g_message ("Setting priority nice level to 19"));

	if (nice (19) == -1) {
		const gchar *str = g_strerror (errno);

		TRACKER_NOTE (CONFIG, g_message ("Couldn't set nice value to 19, %s",
		                      str ? str : "no error given"));
	}
}

static gboolean
signal_handler (gpointer user_data)
{
	int signo = GPOINTER_TO_INT (user_data);

	static gboolean in_loop = FALSE;

	/* Die if we get re-entrant signals handler calls */
	if (in_loop) {
		_exit (EXIT_FAILURE);
	}

	switch (signo) {
	case SIGTERM:
	case SIGINT:
		in_loop = TRUE;
		g_main_loop_quit (main_loop);

		/* Fall through */
	default:
		if (g_strsignal (signo)) {
			g_debug ("Received signal:%d->'%s'",
			         signo,
			         g_strsignal (signo));
		}
		break;
	}

	return G_SOURCE_CONTINUE;
}

static void
initialize_signal_handler (void)
{
#ifndef G_OS_WIN32
	g_unix_signal_add (SIGTERM, signal_handler, GINT_TO_POINTER (SIGTERM));
	g_unix_signal_add (SIGINT, signal_handler, GINT_TO_POINTER (SIGINT));
#endif /* G_OS_WIN32 */
}

static void
log_option_values (TrackerConfig *config)
{
#ifdef G_ENABLE_DEBUG
	if (TRACKER_DEBUG_CHECK (CONFIG)) {
		g_message ("General options:");
		g_message ("  Max bytes (per file)  .................  %d",
		           tracker_config_get_max_bytes (config));
	}
#endif
}

TrackerConfig *
tracker_main_get_config (void)
{
	return config;
}

static int
run_standalone (TrackerConfig *config)
{
	TrackerExtract *object;
	GFile *file;
	gchar *uri;
	GEnumClass *enum_class;
	GEnumValue *enum_value;
	TrackerSerializationFormat output_format;

	if (!output_format_name) {
		output_format_name = "turtle";
	}

	/* This makes sure we don't steal all the system's resources */
	initialize_priority_and_scheduling ();

	/* Look up the output format by name */
	enum_class = g_type_class_ref (TRACKER_TYPE_SERIALIZATION_FORMAT);
	enum_value = g_enum_get_value_by_nick (enum_class, output_format_name);
	g_type_class_unref (enum_class);
	if (!enum_value) {
		g_printerr (N_("Unsupported serialization format “%s”\n"), output_format_name);
		return EXIT_FAILURE;
	}
	output_format = enum_value->value;

	tracker_locale_sanity_check ();

	file = g_file_new_for_commandline_arg (filename);
	uri = g_file_get_uri (file);

	object = tracker_extract_new (TRUE, force_module);

	if (!object) {
		g_object_unref (file);
		g_free (uri);
		return EXIT_FAILURE;
	}

	tracker_extract_get_metadata_by_cmdline (object, uri, mime_type, output_format);

	g_object_unref (object);
	g_object_unref (file);
	g_free (uri);

	return EXIT_SUCCESS;
}

static void
on_domain_vanished (GDBusConnection *connection,
                    const gchar     *name,
                    gpointer         user_data)
{
	GMainLoop *loop = user_data;
	g_main_loop_quit (loop);
}

static void
on_decorator_items_available (TrackerDecorator *decorator)
{
	if (shutdown_timeout_id) {
		g_source_remove (shutdown_timeout_id);
		shutdown_timeout_id = 0;
	}
}

static gboolean
shutdown_timeout_cb (gpointer user_data)
{
	GMainLoop *loop = user_data;

	g_debug ("Shutting down after 10 seconds inactivity");
	g_main_loop_quit (loop);
	shutdown_timeout_id = 0;
	return G_SOURCE_REMOVE;
}

static void
on_decorator_finished (TrackerDecorator *decorator,
                       GMainLoop        *loop)
{
	if (shutdown_timeout_id != 0)
		return;

	/* For debugging convenience, avoid the shutdown timeout if running
	 * on a terminal.
	 */
	if (tracker_term_is_tty ())
		return;

	shutdown_timeout_id = g_timeout_add_seconds (10, shutdown_timeout_cb,
	                                             main_loop);
}

static GFile *
get_cache_dir (TrackerDomainOntology *domain_ontology)
{
	GFile *cache;

	cache = tracker_domain_ontology_get_cache (domain_ontology);
	return g_file_get_child (cache, "files");
}

TrackerSparqlConnection *
tracker_main_get_readonly_connection (GError **error)
{
	TrackerDomainOntology *domain_ontology = NULL;
	g_autoptr (GFile) store = NULL;

	domain_ontology = tracker_domain_ontology_new (domain_ontology_name, NULL, error);
	if (!domain_ontology)
		return NULL;

	store = get_cache_dir (domain_ontology);
	tracker_domain_ontology_unref (domain_ontology);

	if (!g_file_query_exists (store, NULL))
		return NULL;

	return tracker_sparql_connection_new (TRACKER_SPARQL_CONNECTION_FLAGS_READONLY,
	                                      store,
	                                      NULL,
	                                      NULL,
	                                      error);
}

int
main (int argc, char *argv[])
{
	GOptionContext *context;
	GError *error = NULL;
	TrackerExtract *extract;
	TrackerDecorator *decorator;
	TrackerExtractController *controller;
	GMainLoop *my_main_loop;
	GDBusConnection *connection;
	TrackerMinerProxy *proxy;
	TrackerSparqlConnection *sparql_connection;
	TrackerDomainOntology *domain_ontology;
	gchar *dbus_name, *miner_dbus_name;
	GFile *cache_dir;

	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	/* This makes sure we don't steal all the system's resources */
	initialize_priority_and_scheduling ();

	/* Translators: this message will appear immediately after the  */
	/* usage string - Usage: COMMAND [OPTION]... <THIS_MESSAGE>     */
	context = g_option_context_new (_("— Extract file meta data"));

	g_option_context_add_main_entries (context, entries, NULL);
	g_option_context_parse (context, &argc, &argv, &error);

	if (error) {
		g_printerr ("%s\n", error->message);
		g_error_free (error);
		return EXIT_FAILURE;
	}

	if (!filename && mime_type) {
		gchar *help;

		g_printerr ("%s\n\n",
		            _("Filename and mime type must be provided together"));

		help = g_option_context_get_help (context, TRUE, NULL);
		g_option_context_free (context);
		g_printerr ("%s", help);
		g_free (help);

		return EXIT_FAILURE;
	}

	g_option_context_free (context);

	if (version) {
		g_print ("\n" ABOUT "\n" LICENSE "\n");
		return EXIT_SUCCESS;
	}

	g_set_application_name ("tracker-extract");

	setlocale (LC_ALL, "");

	domain_ontology = tracker_domain_ontology_new (domain_ontology_name, NULL, &error);
	if (error) {
		g_critical ("Could not load domain ontology '%s': %s",
		            domain_ontology_name, error->message);
		g_error_free (error);
		return EXIT_FAILURE;
	}

	connection = g_bus_get_sync (TRACKER_IPC_BUS, NULL, &error);
	if (error) {
		g_critical ("Could not create DBus connection: %s\n",
		            error->message);
		g_error_free (error);
		return EXIT_FAILURE;
	}

	tracker_content_identifier_cache_init ();

	cache_dir = get_cache_dir (domain_ontology);
	tracker_error_report_init (cache_dir);
	g_object_unref (cache_dir);

	config = tracker_config_new ();

	/* Extractor command line arguments */
	log_option_values (config);

	/* Set conditions when we use stand alone settings */
	if (filename) {
		return run_standalone (config);
	}

	extract = tracker_extract_new (TRUE, force_module);

	if (!extract) {
		g_object_unref (config);
		return EXIT_FAILURE;
	}

	tracker_module_manager_load_modules ();

	miner_dbus_name = tracker_domain_ontology_get_domain (domain_ontology,
	                                                      MINER_FS_NAME_SUFFIX);
	sparql_connection = tracker_sparql_connection_bus_new (miner_dbus_name,
	                                                       NULL, NULL, &error);

	if (error) {
		g_critical ("Could not connect to filesystem miner endpoint: %s",
		            error->message);
		g_error_free (error);
		return EXIT_FAILURE;
	}

	decorator = tracker_extract_decorator_new (sparql_connection, extract, NULL, &error);

	if (error) {
		g_critical ("Could not start decorator: %s\n", error->message);
		g_object_unref (config);
		return EXIT_FAILURE;
	}

	proxy = tracker_miner_proxy_new (TRACKER_MINER (decorator), connection, DBUS_PATH, NULL, &error);
	if (error) {
		g_critical ("Could not create miner DBus proxy: %s\n", error->message);
		g_error_free (error);
		g_object_unref (decorator);
		g_object_unref (config);
		return EXIT_FAILURE;
	}

#ifdef THREAD_ENABLE_TRACE
	g_debug ("Thread:%p (Main) --- Waiting for extract requests...",
	         g_thread_self ());
#endif /* THREAD_ENABLE_TRACE */

	tracker_locale_sanity_check ();

	controller = tracker_extract_controller_new (decorator, connection);

	/* Request DBus name */
	dbus_name = tracker_domain_ontology_get_domain (domain_ontology, DBUS_NAME_SUFFIX);

	if (tracker_term_is_tty ()) {
		g_debug ("tracker-extract-3 running as %s", dbus_name);
	} else {
		g_debug ("tracker-extract-3 running as %s. The service will exit when %s "
		         "disappears from the bus.", dbus_name, miner_dbus_name);
	}

	if (!tracker_dbus_request_name (connection, dbus_name, &error)) {
		g_critical ("Could not request DBus name '%s': %s",
		            dbus_name, error->message);
		g_error_free (error);
		g_free (dbus_name);
		return EXIT_FAILURE;
	}

	g_free (dbus_name);

	/* Main loop */
	main_loop = g_main_loop_new (NULL, FALSE);

	g_bus_watch_name_on_connection (connection, miner_dbus_name,
	                                G_BUS_NAME_WATCHER_FLAGS_NONE,
	                                NULL, on_domain_vanished,
	                                main_loop, NULL);
	g_free (miner_dbus_name);

	g_signal_connect (decorator, "finished",
	                  G_CALLBACK (on_decorator_finished),
	                  main_loop);
	g_signal_connect (decorator, "items-available",
	                  G_CALLBACK (on_decorator_items_available),
	                  main_loop);

	tracker_miner_start (TRACKER_MINER (decorator));

	initialize_signal_handler ();

	g_main_loop_run (main_loop);

	my_main_loop = main_loop;
	main_loop = NULL;
	g_main_loop_unref (my_main_loop);

	tracker_miner_stop (TRACKER_MINER (decorator));

	/* Shutdown subsystems */
	g_object_unref (extract);
	g_object_unref (decorator);
	g_object_unref (controller);
	g_object_unref (proxy);
	g_object_unref (connection);
	tracker_domain_ontology_unref (domain_ontology);
	tracker_sparql_connection_close (sparql_connection);
	g_object_unref (sparql_connection);

	g_object_unref (config);

	return EXIT_SUCCESS;
}
