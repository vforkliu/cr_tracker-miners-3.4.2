/*
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

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/mman.h>

#include <glib.h>
#include <glib/gstdio.h>

#include <gio/gio.h>

#include <libtracker-miners-common/tracker-file-utils.h>

#include <libtracker-extract/tracker-extract.h>

#include "tracker-main.h"
#include "tracker-extract.h"
#include "tracker-read.h"

static gboolean
allow_file (GSList      *text_allowlist_patterns,
            GFile       *file)
{
	GSList *l;
	g_autofree gchar *basename = NULL;

	basename = g_file_get_basename (file);

	for (l = text_allowlist_patterns; l; l = l->next) {
#if GLIB_CHECK_VERSION (2, 70, 0)
		if (g_pattern_spec_match_string (l->data, basename))
#else
		if (g_pattern_match_string (l->data, basename))
#endif
		{
			return TRUE;
		}
	}

	return FALSE;
}

static gchar *
get_file_content (GFile   *file,
                  gsize    n_bytes,
                  GError **error)
{
	gchar *text, *uri, *path;
	int fd;

	uri = g_file_get_uri (file);

	/* Get filename from URI */
	path = g_file_get_path (file);

	fd = tracker_file_open_fd (path);

	if (fd == -1) {
		g_set_error (error, TRACKER_EXTRACT_ERROR, TRACKER_EXTRACT_ERROR_IO_ERROR,
		             "Could not open file '%s': %s", uri, g_strerror (errno));
		g_free (uri);
		g_free (path);
		return NULL;
	}

	g_debug ("  Starting to read '%s' up to %" G_GSIZE_FORMAT " bytes...",
	         uri, n_bytes);

	/* Read up to n_bytes from stream. Output is always, always valid UTF-8,
	 * this function closes the FD.
	 */
	text = tracker_read_text_from_fd (fd, n_bytes, error);
	g_free (uri);
	g_free (path);

	return text;
}

G_MODULE_EXPORT gboolean
tracker_extract_get_metadata (TrackerExtractInfo  *info,
                              GError             **error)
{
	TrackerResource *metadata;
	TrackerConfig *config;
	GFile *file;
	GSList *text_allowlist_patterns;
	gchar *content = NULL, *resource_uri;
	GError *inner_error = NULL;

	config = tracker_main_get_config ();
	text_allowlist_patterns = tracker_config_get_text_allowlist_patterns (config);
	file = tracker_extract_info_get_file (info);

	resource_uri = tracker_file_get_content_identifier (file, NULL, NULL);
	metadata = tracker_resource_new (resource_uri);
	tracker_resource_add_uri (metadata, "rdf:type", "nfo:PlainTextDocument");
	g_free (resource_uri);

	if (allow_file (text_allowlist_patterns, file)) {
		content = get_file_content (tracker_extract_info_get_file (info),
		                            tracker_config_get_max_bytes (config),
		                            &inner_error);

		if (inner_error != NULL) {
			/* An error occurred, perhaps the file was deleted. */
			g_propagate_prefixed_error (error, inner_error, "Could not open:");
			return FALSE;
		}

		if (content) {
			tracker_resource_set_string (metadata, "nie:plainTextContent", content);
			g_free (content);
		} else {
			tracker_resource_set_string (metadata, "nie:plainTextContent", "");
		}
	}

	tracker_extract_info_set_resource (info, metadata);
	g_object_unref (metadata);

	return TRUE;
}
