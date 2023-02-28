/*
 * Copyright (C) 2008, Nokia <ivan.frade@nokia.com>

 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config-miners.h"

#include <string.h>
#include <stdlib.h>
#include <locale.h>
#include <errno.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef HAVE_MALLOC_TRIM
#include <malloc.h>
#endif

#include <glib.h>
#include <glib-unix.h>
#include <glib-object.h>
#include <glib/gi18n.h>

#include <libtracker-miners-common/tracker-common.h>
#include <libtracker-sparql/tracker-sparql.h>
#include <libtracker-miner/tracker-miner.h>

#include "tracker-config.h"
#include "tracker-miner-files.h"

#define ABOUT	  \
	"Tracker " PACKAGE_VERSION "\n"

#define LICENSE	  \
	"This program is free software and comes without any warranty.\n" \
	"It is licensed under version 2 or later of the General Public " \
	"License which can be viewed at:\n" \
	"\n" \
	"  http://www.gnu.org/licenses/gpl.txt\n"

#define SECONDS_PER_DAY 60 * 60 * 24

#define DBUS_NAME_SUFFIX "Tracker3.Miner.Files"
#define DBUS_PATH "/org/freedesktop/Tracker3/Miner/Files"
#define LOCALE_FILENAME "locale-for-miner-apps.txt"

static GMainLoop *main_loop;
static GDBusProxy *index_proxy;
static GPtrArray *proxy_folders;
static guint cleanup_id;

static gint initial_sleep = -1;
static gboolean no_daemon;
static gchar *eligible;
static gboolean version;
static guint miners_timeout_id = 0;
static gboolean do_crawling = FALSE;
static gchar *domain_ontology_name = NULL;
static gboolean dry_run = FALSE;

static gboolean slept = TRUE;
static gboolean graphs_ready = FALSE;

static GOptionEntry entries[] = {
	{ "initial-sleep", 's', 0,
	  G_OPTION_ARG_INT, &initial_sleep,
	  N_("Initial sleep time in seconds, "
	     "0->1000 (default=15)"),
	  NULL },
	{ "no-daemon", 'n', 0,
	  G_OPTION_ARG_NONE, &no_daemon,
	  N_("Runs until all configured locations are indexed and then exits"),
	  NULL },
	{ "eligible", 'e', 0,
	  G_OPTION_ARG_FILENAME, &eligible,
	  N_("Checks if FILE is eligible for being mined based on configuration"),
	  N_("FILE") },
	{ "domain-ontology", 'd', 0,
	  G_OPTION_ARG_STRING, &domain_ontology_name,
	  N_("Runs for a specific domain ontology"),
	  NULL },
	{ "dry-run", 'r', 0,
	  G_OPTION_ARG_NONE, &dry_run,
	  N_("Avoids changes in the filesystem"),
	  NULL },
	{ "version", 'V', 0,
	  G_OPTION_ARG_NONE, &version,
	  N_("Displays version information"),
	  NULL },
	{ NULL }
};

static void
log_option_values (TrackerConfig *config)
{
#ifdef G_ENABLE_DEBUG
	if (TRACKER_DEBUG_CHECK (CONFIG)) {
		g_message ("General options:");
		g_message ("  Initial Sleep  ........................  %d",
		           tracker_config_get_initial_sleep (config));

		g_message ("Indexer options:");
		g_message ("  Throttle level  .......................  %d",
		           tracker_config_get_throttle (config));
		g_message ("  Indexing while on battery  ............  %s (first time only = %s)",
		           tracker_config_get_index_on_battery (config) ? "yes" : "no",
		           tracker_config_get_index_on_battery_first_time (config) ? "yes" : "no");

		if (tracker_config_get_low_disk_space_limit (config) == -1) {
			g_message ("  Low disk space limit  .................  Disabled");
		} else {
			g_message ("  Low disk space limit  .................  %d%%",
			           tracker_config_get_low_disk_space_limit (config));
		}
	}
#endif
}

static void
miner_reset_applications (TrackerMiner *miner)
{
	const gchar *sparql;
	GError *error = NULL;

	sparql =
		"DELETE { ?icon a rdfs:Resource } "
		"WHERE { ?app a nfo:SoftwareApplication; nfo:softwareIcon ?icon }; "
		"DELETE { ?app tracker:extractorHash ?h } "
		"WHERE { ?app a nfo:SoftwareApplication ; "
		"             tracker:extractorHash ?h } ";

	/* Execute a sync update, we don't want the apps miner to start before
	 * we finish this. */
	tracker_sparql_connection_update (tracker_miner_get_connection (miner),
	                                  sparql,
	                                  NULL, &error);

	if (error) {
		/* Some error happened performing the query, not good */
		g_critical ("Couldn't reset indexed applications: %s",
		            error ? error->message : "unknown error");
		g_error_free (error);
	}
}

static GFile *
get_cache_dir (TrackerDomainOntology *domain_ontology)
{
	GFile *cache;

	cache = tracker_domain_ontology_get_cache (domain_ontology);
	return g_file_get_child (cache, "files");
}

static void
save_current_locale (TrackerDomainOntology *domain_ontology)
{
	GError *error = NULL;
	gchar *locale = tracker_locale_get (TRACKER_LOCALE_LANGUAGE);
	GFile *cache = get_cache_dir (domain_ontology);
	gchar *cache_path = g_file_get_path (cache);
	gchar *locale_file;

	locale_file = g_build_filename (cache_path, LOCALE_FILENAME, NULL);
	g_free (cache_path);

	TRACKER_NOTE (CONFIG, g_message ("Saving locale used to index applications"));
	TRACKER_NOTE (CONFIG, g_message ("  Creating locale file '%s'", locale_file));

	if (locale == NULL) {
		locale = g_strdup ("");
	}

	if (!g_file_set_contents (locale_file, locale, -1, &error)) {
		g_message ("  Could not set file contents, %s",
		           error ? error->message : "no error given");
		g_clear_error (&error);
	}

	g_object_unref (cache);
	g_free (locale);
	g_free (locale_file);
}

static gboolean
detect_locale_changed (TrackerMiner          *miner,
		       TrackerDomainOntology *domain_ontology)
{
	GFile *cache;
	gchar *cache_path;
	gchar *locale_file;
	gchar *previous_locale = NULL;
	gchar *current_locale;
	gboolean changed;

	cache = get_cache_dir (domain_ontology);
	cache_path = g_file_get_path (cache);
	locale_file = g_build_filename (cache_path, LOCALE_FILENAME, NULL);
	g_object_unref (cache);
	g_free (cache_path);

	if (G_LIKELY (g_file_test (locale_file, G_FILE_TEST_EXISTS))) {
		gchar *contents;

		/* Check locale is correct */
		if (G_LIKELY (g_file_get_contents (locale_file, &contents, NULL, NULL))) {
			if (contents &&
			    contents[0] == '\0') {
				g_critical ("  Empty locale file found at '%s'", locale_file);
				g_free (contents);
			} else {
				/* Re-use contents */
				previous_locale = contents;
			}
		} else {
			g_critical ("  Could not get content of file '%s'", locale_file);
		}
	} else {
		TRACKER_NOTE (CONFIG, g_message ("  Could not find locale file:'%s'", locale_file));
	}

	g_free (locale_file);

	current_locale = tracker_locale_get (TRACKER_LOCALE_LANGUAGE);

	/* Note that having both to NULL is actually valid, they would default
	 * to the unicode collation without locale-specific stuff. */
	if (g_strcmp0 (previous_locale, current_locale) != 0) {
		TRACKER_NOTE (CONFIG, g_message ("Locale change detected from '%s' to '%s'...",
		                      previous_locale, current_locale));
		changed = TRUE;
	} else {
		TRACKER_NOTE (CONFIG, g_message ("Current and previous locales match: '%s'", previous_locale));
		changed = FALSE;
	}

	g_free (current_locale);
	g_free (previous_locale);

	if (changed) {
		TRACKER_NOTE (CONFIG, g_message ("Resetting nfo:Software due to locale change..."));
		miner_reset_applications (miner);
	}

	return changed;
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

	errno = 0;
	if (nice (19) == -1 && errno != 0) {
		const gchar *str = g_strerror (errno);

		g_message ("Couldn't set nice value to 19, %s",
		           str ? str : "no error given");
	}
}

static gboolean
should_crawl (TrackerMinerFiles *miner_files,
              TrackerConfig     *config,
              gboolean          *forced)
{
	gint crawling_interval;

	crawling_interval = tracker_config_get_crawling_interval (config);

	TRACKER_NOTE (CONFIG, g_message ("Checking whether to crawl file system based on configured crawling interval:"));

	if (crawling_interval == -2) {
		TRACKER_NOTE (CONFIG, g_message ("  Disabled"));
		return FALSE;
	} else if (crawling_interval == -1) {
		TRACKER_NOTE (CONFIG, g_message ("  Maybe (depends on a clean last shutdown)"));
		return TRUE;
	} else if (crawling_interval == 0) {
		TRACKER_NOTE (CONFIG, g_message ("  Forced"));

		if (forced) {
			*forced = TRUE;
		}

		return TRUE;
	} else {
		guint64 then, now;

		then = tracker_miner_files_get_last_crawl_done (miner_files);

		if (then < 1) {
			return TRUE;
		}

		now = (guint64) time (NULL);

		if (now < then + (crawling_interval * SECONDS_PER_DAY)) {
			TRACKER_NOTE (CONFIG, g_message ("  Postponed"));
			return FALSE;
		} else {
			TRACKER_NOTE (CONFIG, g_message ("  (More than) %d days after last crawling, enabled", crawling_interval));
			return TRUE;
		}
	}
}

static void
miner_do_start (TrackerMiner *miner)
{
	if (!tracker_miner_is_started (miner)) {
		g_debug ("Starting filesystem miner...");
		tracker_miner_start (miner);
	}
}

static void
miner_maybe_start (TrackerMiner *miner)
{
	if (!slept || !graphs_ready)
		return;

	miner_do_start (miner);
}

static gboolean
miner_start_idle_cb (gpointer data)
{
	TrackerMiner *miner = data;

	miners_timeout_id = 0;
	slept = TRUE;
	miner_maybe_start (miner);
	return G_SOURCE_REMOVE;
}

static void
miner_start (TrackerMiner  *miner,
	     TrackerConfig *config,
	     gboolean       do_mtime_checking)
{
	gint initial_sleep;

	if (!do_mtime_checking) {
		g_debug ("Avoiding initial sleep, no mtime check needed");
		miner_maybe_start (miner);
		return;
	}

	/* If requesting to run as no-daemon, start right away */
	if (no_daemon) {
		miner_maybe_start (miner);
		return;
	}

	/* If no need to initially sleep, start right away */
	initial_sleep = tracker_config_get_initial_sleep (config);

	if (initial_sleep <= 0) {
		miner_maybe_start (miner);
		return;
	}

	slept = FALSE;
	g_debug ("Performing initial sleep of %d seconds",
	         initial_sleep);
	miners_timeout_id = g_timeout_add_seconds (initial_sleep,
	                                           miner_start_idle_cb,
	                                           miner);
}

#ifdef HAVE_MALLOC_TRIM

static void
release_heap_memory (void)
{
	malloc_trim (0);
}

#else

static void
release_heap_memory (void)
{
	g_debug ("release_heap_memory(): Doing nothing as malloc_trim() is not available on this platform.");
}

#endif

static gboolean
cleanup_cb (gpointer user_data)
{
	release_heap_memory ();

	cleanup_id = 0;

	return G_SOURCE_REMOVE;
}

#if GLIB_CHECK_VERSION (2, 64, 0)
static void
on_low_memory (GMemoryMonitor            *monitor,
               GMemoryMonitorWarningLevel level,
               gpointer                   user_data)
{
	if (level > G_MEMORY_MONITOR_WARNING_LEVEL_LOW)
		release_heap_memory ();
}
#endif

static void
miner_started_cb (TrackerMinerFS *fs)
{
	if (cleanup_id) {
		g_source_remove (cleanup_id);
		cleanup_id = 0;
	}
}

static void
miner_finished_cb (TrackerMinerFS *fs,
                   gdouble         seconds_elapsed,
                   guint           total_directories_found,
                   guint           total_directories_ignored,
                   guint           total_files_found,
                   guint           total_files_ignored,
                   guint           changes,
                   gpointer        user_data)
{
	g_info ("Finished mining in %f seconds. Changes processed: %d, inspected directories:%d, inspected files:%d",
	        seconds_elapsed,
	        changes,
	        total_directories_found,
	        total_files_found);

	if (do_crawling && !dry_run) {
		tracker_miner_files_set_last_crawl_done (TRACKER_MINER_FILES (fs),
							 TRUE);
	}

	cleanup_id = g_timeout_add_seconds (30, cleanup_cb, NULL);

	/* We're not sticking around for file updates, so stop
	 * the mainloop and exit.
	 */
	if (no_daemon && main_loop) {
		/* FIXME: wait for extractor to finish */
		g_main_loop_quit (main_loop);
	}
}

static GList *
get_dir_children_as_gfiles (const gchar *path)
{
	GList *children = NULL;
	GDir *dir;

	dir = g_dir_open (path, 0, NULL);

	if (dir) {
		const gchar *basename;

		while ((basename = g_dir_read_name (dir)) != NULL) {
			GFile *child;
			gchar *str;

			str = g_build_filename (path, basename, NULL);
			child = g_file_new_for_path (str);
			g_free (str);

			children = g_list_prepend (children, child);
		}

		g_dir_close (dir);
	}

	return children;
}

static void
dummy_log_handler (const gchar    *domain,
                   GLogLevelFlags  log_level,
                   const gchar    *message,
                   gpointer        user_data)
{
	return;
}

static void
graphs_created_cb (GObject      *source,
                   GAsyncResult *res,
                   gpointer      user_data)
{
	TrackerMiner *miner = user_data;

	tracker_sparql_connection_update_finish (TRACKER_SPARQL_CONNECTION (source),
	                                         res, NULL);
	graphs_ready = TRUE;
	miner_maybe_start (miner);
}

static gint
check_eligible (void)
{
	TrackerSparqlConnection *sparql_conn;
	TrackerMiner *miner_files;
	TrackerIndexingTree *indexing_tree;
	TrackerDomainOntology *domain_ontology;
	TrackerConfig *config;
	GFile *ontology;
	GFile *file;
	GFileInfo *info;
	GError *error = NULL;
	gchar *path;
	guint log_handler_id;
	gboolean exists = TRUE;
	gboolean indexable;
	gboolean parents_indexable = TRUE;
	gboolean is_dir;

	/* Set log handler for library messages */
	log_handler_id = g_log_set_handler (NULL,
	                                    G_LOG_LEVEL_MASK | G_LOG_FLAG_FATAL,
	                                    dummy_log_handler,
	                                    NULL);

	g_log_set_default_handler (dummy_log_handler, NULL);


	/* Start check */
	file = g_file_new_for_commandline_arg (eligible);
	info = g_file_query_info (file,
	                          G_FILE_ATTRIBUTE_STANDARD_TYPE ","
	                          G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN,
	                          G_FILE_QUERY_INFO_NONE,
	                          NULL,
	                          &error);

	if (error) {
		if (error->code == G_IO_ERROR_NOT_FOUND) {
			exists = FALSE;
		}

		g_error_free (error);
	}

	if (info) {
		is_dir = g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY;
	} else {
		/* Assume not a dir */
		is_dir = FALSE;
	}

	config = tracker_config_new ();
	path = g_file_get_path (file);

	g_print (exists ?
	         _("Data object “%s” currently exists") :
	         _("Data object “%s” currently does not exist"),
	         path);

	g_print ("\n");

	domain_ontology = tracker_domain_ontology_new (domain_ontology_name, NULL, &error);
	ontology = tracker_domain_ontology_get_ontology (domain_ontology);
	tracker_domain_ontology_unref (domain_ontology);

	sparql_conn = tracker_sparql_connection_new (0,
	                                              NULL,
	                                              ontology,
	                                              NULL,
	                                              NULL);
	if (!sparql_conn) {
		g_object_unref (info);
		return EXIT_FAILURE;
	}

	/* Create new TrackerMinerFiles object */
	config = tracker_config_new ();
	miner_files = tracker_miner_files_new (sparql_conn, config,
	                                       domain_ontology_name, NULL);
	g_object_unref (config);

	if (!miner_files) {
		g_object_unref (info);
		g_object_unref (sparql_conn);
		return EXIT_FAILURE;
	}

	indexing_tree = tracker_miner_fs_get_indexing_tree (TRACKER_MINER_FS (miner_files));

	indexable = tracker_indexing_tree_file_is_indexable (indexing_tree, file, info);

	if (!indexable) {
		if (is_dir &&
		    tracker_indexing_tree_file_matches_filter (indexing_tree, TRACKER_FILTER_DIRECTORY, file)) {
			g_print ("  %s\n", _("Directory is NOT eligible to be indexed (based on filters)"));

		} else if (!is_dir &&
		           tracker_indexing_tree_file_matches_filter (indexing_tree, TRACKER_FILTER_FILE, file)) {
			g_print ("  %s\n", _("File is NOT eligible to be indexed (based on filters)"));
		} else if (tracker_file_is_hidden (file) &&
		           tracker_indexing_tree_get_filter_hidden (indexing_tree)) {
			g_print ("  %s\n", _("File is NOT eligible to be indexed (hidden file)"));
		} else {
			g_print ("  %s\n", _("File is NOT eligible to be indexed (not an indexed folder)"));
		}
	}

	if (indexable) {
		GFile *root, *parent;
		GList *files = NULL, *l;

		root = tracker_indexing_tree_get_root (indexing_tree, file, NULL);
		parent = file;

		/* Still, a parent folder might be filtered out, figure it out */
		while (parent && !g_file_equal (parent, root)) {
			parent = g_file_get_parent (parent);
			files = g_list_prepend (files, parent);
		}

		for (l = files; l; l = l->next) {
			gchar *dir_path;

			dir_path = g_file_get_path (l->data);

			if (is_dir &&
			    tracker_indexing_tree_file_matches_filter (indexing_tree, TRACKER_FILTER_DIRECTORY, l->data)) {
				g_print (_("Parent directory “%s” is NOT eligible to be indexed (based on filters)"),
				         dir_path);
				g_print ("\n");
				parents_indexable = FALSE;
			} else if (tracker_file_is_hidden (l->data) &&
			           tracker_indexing_tree_get_filter_hidden (indexing_tree)) {
				g_print (_("Parent directory “%s” is NOT eligible to be indexed (hidden file)"),
				         dir_path);
				g_print ("\n");
				parents_indexable = FALSE;
			} else {
				GList *children = get_dir_children_as_gfiles (dir_path);

				if (!tracker_indexing_tree_parent_is_indexable (indexing_tree, l->data, children)) {
					g_print (_("Parent directory “%s” is NOT eligible to be indexed (based on content filters)"),
					         dir_path);
					g_print ("\n");
					parents_indexable = FALSE;
				}

				g_list_free_full (children, g_object_unref);
			}

			g_free (dir_path);

			if (!parents_indexable)
				break;
		}

		g_list_free_full (files, g_object_unref);
	}

	if (indexable && parents_indexable) {
		g_print ("  %s\n",
		         is_dir ?
		         _("Directory is eligible to be indexed") :
		         _("File is eligible to be indexed"));
	}

	if (log_handler_id != 0) {
		/* Unset log handler */
		g_log_remove_handler (NULL, log_handler_id);
	}

	g_free (path);
	g_object_unref (file);
	g_object_unref (miner_files);
	g_object_unref (info);

	return (indexable && parents_indexable) ? EXIT_SUCCESS : EXIT_FAILURE;
}

static gboolean
miner_needs_check (TrackerMiner *miner)
{
	/* Reasons to not mark ourselves as cleanly shutdown include:
	 *
	 * 1. Still crawling or with files to process in our queues.
	 * 2. We crash (out of our control usually anyway).
	 * 3. At least one of the miners is PAUSED, we have
	 *    to exclude the situations where the miner is actually done.
	 */
	if (!tracker_miner_is_paused (miner)) {
		if (tracker_miner_fs_has_items_to_process (TRACKER_MINER_FS (miner))) {
			/* There are items left to process */
			return TRUE;
		}

		/* FIXME: We currently don't check the applications
		 *  miner if we are finished before returning TRUE/FALSE here, should
		 *  we?
		 */

		/* We consider the miner finished */
		return FALSE;
	} else {
		/* Paused for other reasons, so probably not done */
		return TRUE;
	}
}

static void
on_domain_vanished (GDBusConnection *connection,
                    const gchar     *name,
                    gpointer         user_data)
{
	GMainLoop *loop = user_data;
	g_message ("Domain %s vanished: quitting now.", name);
	g_main_loop_quit (loop);
}

TrackerSparqlConnectionFlags
get_fts_connection_flags (void)
{
	TrackerSparqlConnectionFlags flags = 0;
	TrackerFTSConfig *fts_config;

	fts_config = tracker_fts_config_new ();

	if (tracker_fts_config_get_enable_stemmer (fts_config))
		flags |= TRACKER_SPARQL_CONNECTION_FLAGS_FTS_ENABLE_STEMMER;
	if (tracker_fts_config_get_enable_unaccent (fts_config))
		flags |= TRACKER_SPARQL_CONNECTION_FLAGS_FTS_ENABLE_UNACCENT;
	if (tracker_fts_config_get_ignore_numbers (fts_config))
		flags |= TRACKER_SPARQL_CONNECTION_FLAGS_FTS_IGNORE_NUMBERS;
	if (tracker_fts_config_get_ignore_stop_words (fts_config))
		flags |= TRACKER_SPARQL_CONNECTION_FLAGS_FTS_ENABLE_STOP_WORDS;

	g_object_unref (fts_config);

	return flags;
}

static gboolean
setup_connection_and_endpoint (TrackerDomainOntology    *domain,
                               GDBusConnection          *connection,
                               TrackerSparqlConnection **sparql_conn,
                               TrackerEndpointDBus     **endpoint,
                               GError                  **error)
{
	GFile *store = NULL, *ontology;

	if (!dry_run)
		store = get_cache_dir (domain);
	ontology = tracker_domain_ontology_get_ontology (domain);
	*sparql_conn = tracker_sparql_connection_new (get_fts_connection_flags (),
	                                              store,
	                                              ontology,
	                                              NULL,
	                                              error);
	g_clear_object (&store);

	if (!*sparql_conn)
		return FALSE;

	*endpoint = tracker_endpoint_dbus_new (*sparql_conn,
	                                       connection,
	                                       NULL,
	                                       NULL,
	                                       error);
	if (!*endpoint)
		return FALSE;

	return TRUE;
}

static void
update_indexed_files_from_proxy (TrackerMinerFiles *miner,
                                 GDBusProxy        *proxy)
{
	TrackerIndexingTree *indexing_tree;
	const gchar **indexed_uris = NULL;
	GVariant *v;
	gint i;

	v = g_dbus_proxy_get_cached_property (proxy, "IndexedLocations");
	if (v)
		indexed_uris = g_variant_get_strv (v, NULL);

	indexing_tree = tracker_miner_fs_get_indexing_tree (TRACKER_MINER_FS (miner));

	/* Remove folders no longer there */
	for (i = 0; i < proxy_folders->len; i++) {
		GFile *file;
		gchar *uri;

		file = g_ptr_array_index (proxy_folders, i);
		uri = g_file_get_uri (file);

		if (!indexed_uris || !g_strv_contains (indexed_uris, uri)) {
			tracker_indexing_tree_remove (indexing_tree,
			                              file);
		}

		g_free (uri);
	}

	for (i = 0; indexed_uris && indexed_uris[i]; i++) {
		GFileInfo *file_info;
		GFile *file;

		file = g_file_new_for_uri (indexed_uris[i]);
		if (g_ptr_array_find_with_equal_func (proxy_folders,
		                                      file,
		                                      (GEqualFunc) g_file_equal,
		                                      NULL)) {
			g_object_unref (file);
			continue;
		}

		file_info = g_file_query_info (file,
		                               G_FILE_ATTRIBUTE_STANDARD_TYPE ","
		                               G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN,
		                               G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
		                               NULL, NULL);

		if (!file_info) {
			g_object_unref (file);
			continue;
		}

		if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY) {
			if (!tracker_indexing_tree_file_is_indexable (indexing_tree,
			                                              file, file_info)) {
				tracker_indexing_tree_add (indexing_tree,
				                           file,
				                           TRACKER_DIRECTORY_FLAG_RECURSE |
				                           TRACKER_DIRECTORY_FLAG_CHECK_MTIME |
				                           TRACKER_DIRECTORY_FLAG_MONITOR);
				g_ptr_array_add (proxy_folders, g_object_ref (file));
			} else {
				tracker_indexing_tree_notify_update (indexing_tree,
				                                     file, TRUE);
			}
		} else {
			tracker_miner_fs_check_file (TRACKER_MINER_FS (miner),
			                             file, G_PRIORITY_HIGH, TRUE);
		}

		g_object_unref (file_info);
		g_object_unref (file);
	}

	g_free (indexed_uris);
}

static void
proxy_properties_changed_cb (GDBusProxy *proxy,
                             GVariant   *changed_properties,
                             GStrv       invalidated_properties,
                             gpointer    user_data)
{
	update_indexed_files_from_proxy (user_data, proxy);
}

static void
on_tracker_index_proxy_ready (GObject      *source,
                              GAsyncResult *res,
                              gpointer      user_data)
{
	TrackerMinerFiles *miner = user_data;
	GError *error = NULL;

	index_proxy = g_dbus_proxy_new_finish (res, &error);
	if (error) {
		g_critical ("Could not set up proxy: %s", error->message);
		g_error_free (error);
		return;
	}

	g_signal_connect (index_proxy, "g-properties-changed",
	                  G_CALLBACK (proxy_properties_changed_cb), miner);
	update_indexed_files_from_proxy (miner, index_proxy);
}

int
main (gint argc, gchar *argv[])
{
	TrackerConfig *config;
	TrackerMiner *miner_files;
	GOptionContext *context;
	GError *error = NULL;
	gboolean do_mtime_checking;
	gboolean force_mtime_checking = FALSE;
	TrackerMinerProxy *proxy;
	GDBusConnection *connection;
	TrackerSparqlConnection *sparql_conn;
	TrackerEndpointDBus *endpoint;
	TrackerDomainOntology *domain_ontology;
	GCancellable *cancellable;
#if GLIB_CHECK_VERSION (2, 64, 0)
	GMemoryMonitor *memory_monitor;
#endif
	gchar *domain_name, *dbus_name;

	main_loop = NULL;

	setlocale (LC_ALL, "");

	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	/* Set timezone info */
	tzset ();

	/* This makes sure we don't steal all the system's resources */
	initialize_priority_and_scheduling ();

	/* Translators: this messagge will apper immediately after the
	 * usage string - Usage: COMMAND <THIS_MESSAGE>
	 */
	context = g_option_context_new (_("— start the tracker indexer"));

	g_option_context_add_main_entries (context, entries, NULL);
	g_option_context_parse (context, &argc, &argv, &error);
	g_option_context_free (context);

	if (error) {
		g_printerr ("%s\n", error->message);
		g_error_free (error);
		return EXIT_FAILURE;
	}

	if (version) {
		g_print ("\n" ABOUT "\n" LICENSE "\n");
		return EXIT_SUCCESS;
	}

	if (eligible) {
		return check_eligible ();
	}

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

	/* Initialize logging */
	config = tracker_config_new ();

	if (initial_sleep > -1) {
		tracker_config_set_initial_sleep (config, initial_sleep);
	}

	log_option_values (config);

	main_loop = g_main_loop_new (NULL, FALSE);

	if (no_daemon) {
		g_debug ("tracker-miner-fs-3 running in --no-daemon mode.");
	} else if (domain_ontology_name) {
		domain_name = tracker_domain_ontology_get_domain (domain_ontology, NULL);

		g_debug ("tracker-miner-fs-3 running in --domain-ontology mode as "
		         "%s." DBUS_NAME_SUFFIX ". The service will exit when %s "
		         "disappears from the bus.", domain_name, domain_name);

		g_bus_watch_name_on_connection (connection, domain_name,
		                                G_BUS_NAME_WATCHER_FLAGS_NONE,
		                                NULL, on_domain_vanished,
		                                main_loop, NULL);
		g_free (domain_name);
	} else {
		g_debug ("tracker-miner-fs-3 running as org.freedesktop." DBUS_NAME_SUFFIX);
	}

	if (!dry_run) {
		GFile *store = get_cache_dir (domain_ontology);
		tracker_error_report_init (store);
		g_object_unref (store);
	}

	if (!setup_connection_and_endpoint (domain_ontology,
	                                    connection,
	                                    &sparql_conn,
	                                    &endpoint,
	                                    &error)) {

		g_critical ("Could not create store/endpoint: %s",
		            error->message);
		g_error_free (error);

		return EXIT_FAILURE;
	}

	/* Create new TrackerMinerFiles object */
	miner_files = tracker_miner_files_new (sparql_conn, config,
	                                       domain_ontology_name, &error);
	if (!miner_files) {
		g_critical ("Couldn't create new Files miner: '%s'",
		            error ? error->message : "unknown error");
		g_object_unref (config);
		return EXIT_FAILURE;
	}

	proxy_folders = g_ptr_array_new_with_free_func (g_object_unref);
	cancellable = g_cancellable_new ();
	g_dbus_proxy_new (connection,
	                  G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
	                  NULL,
	                  "org.freedesktop.Tracker3.Miner.Files.Control",
	                  "/org/freedesktop/Tracker3/Miner/Files/Proxy",
	                  "org.freedesktop.Tracker3.Miner.Files.Proxy",
	                  cancellable,
	                  on_tracker_index_proxy_ready,
	                  miner_files);

	/* If the locales changed, we need to reset some things first */
	detect_locale_changed (TRACKER_MINER (miner_files), domain_ontology);

	proxy = tracker_miner_proxy_new (miner_files, connection, DBUS_PATH, NULL, &error);
	if (error) {
		g_critical ("Couldn't create miner proxy: %s", error->message);
		g_error_free (error);
		g_object_unref (config);
		g_object_unref (miner_files);
		return EXIT_FAILURE;
	}

	/* Request DBus name */
	dbus_name = tracker_domain_ontology_get_domain (domain_ontology, DBUS_NAME_SUFFIX);

	if (!tracker_dbus_request_name (connection, dbus_name, &error)) {
		g_critical ("Could not request DBus name '%s': %s",
		            dbus_name, error->message);
		g_error_free (error);
		g_free (dbus_name);
		return EXIT_FAILURE;
	}

	g_free (dbus_name);

	/* Check if we should crawl and if we should force mtime
	 * checking based on the config.
	 */
	do_crawling = should_crawl (TRACKER_MINER_FILES (miner_files),
	                            config, &force_mtime_checking);

	/* Get the last shutdown state to see if we need to perform a
	 * full mtime check against the db or not.
	 *
	 * Set to TRUE here in case we crash and miss file system
	 * events.
	 */
	TRACKER_NOTE (CONFIG, g_message ("Checking whether to force mtime checking during crawling (based on last clean shutdown):"));

	/* Override the shutdown state decision based on the config */
	if (force_mtime_checking) {
		do_mtime_checking = TRUE;
	} else {
		do_mtime_checking = tracker_miner_files_get_need_mtime_check (TRACKER_MINER_FILES (miner_files));
	}

	TRACKER_NOTE (CONFIG, g_message ("  %s %s",
	                      do_mtime_checking ? "Yes" : "No",
	                      force_mtime_checking ? "(forced from config)" : ""));

	/* Set the need for an mtime check to TRUE so we check in the
	 * event of a crash, this is changed back on shutdown if
	 * everything appears to be fine.
	 */
	if (!dry_run) {
		tracker_miner_files_set_need_mtime_check (TRACKER_MINER_FILES (miner_files), TRUE);
		tracker_miner_files_set_mtime_checking (TRACKER_MINER_FILES (miner_files), do_mtime_checking);
	}

	g_signal_connect (miner_files, "started",
			  G_CALLBACK (miner_started_cb),
			  NULL);
	g_signal_connect (miner_files, "finished",
			  G_CALLBACK (miner_finished_cb),
			  NULL);

#if GLIB_CHECK_VERSION (2, 64, 0)
	memory_monitor = g_memory_monitor_dup_default ();
	g_signal_connect (memory_monitor, "low-memory-warning", G_CALLBACK (on_low_memory), NULL);
#endif

	/* Preempt creation of graphs */
	tracker_sparql_connection_update_async (tracker_miner_get_connection (miner_files),
	                                        "CREATE SILENT GRAPH tracker:FileSystem; "
	                                        "CREATE SILENT GRAPH tracker:Software; "
	                                        "CREATE SILENT GRAPH tracker:Documents; "
	                                        "CREATE SILENT GRAPH tracker:Pictures; "
	                                        "CREATE SILENT GRAPH tracker:Audio; "
	                                        "CREATE SILENT GRAPH tracker:Video ",
	                                        NULL, graphs_created_cb, miner_files);

	if (do_crawling)
		miner_start (miner_files, config, do_mtime_checking);

	initialize_signal_handler ();

	/* Go, go, go! */
	g_main_loop_run (main_loop);

	g_debug ("Shutdown started");

	if (!dry_run && miners_timeout_id == 0 && !miner_needs_check (miner_files)) {
		tracker_miner_files_set_need_mtime_check (TRACKER_MINER_FILES (miner_files), FALSE);
		save_current_locale (domain_ontology);
	}

	g_main_loop_unref (main_loop);
	g_object_unref (config);

	g_cancellable_cancel (cancellable);
	g_object_unref (cancellable);
	g_clear_object (&index_proxy);
	g_clear_pointer (&proxy_folders, g_ptr_array_unref);

	g_object_unref (miner_files);

	g_object_unref (proxy);
	g_object_unref (connection);
	tracker_domain_ontology_unref (domain_ontology);

	tracker_sparql_connection_close (sparql_conn);
	g_object_unref (sparql_conn);

#if GLIB_CHECK_VERSION (2, 64, 0)
	g_signal_handlers_disconnect_by_func (memory_monitor, on_low_memory, NULL);
	g_object_unref (memory_monitor);
#endif

	g_print ("\nOK\n\n");

	return EXIT_SUCCESS;
}
