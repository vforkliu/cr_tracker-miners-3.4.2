/*
 * Copyright (C) 2011, ARQ Media <sam.thursfield@codethink.co.uk>
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
 *
 * Author: Sam Thursfield <sam.thursfield@codethink.co.uk>
 */

#include "config-miners.h"

#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <gio/gio.h>
#include <gst/gst.h>
#include <gst/tag/tag.h>

#if defined(HAVE_LIBCUE)
#include <libcue.h>
#endif

#include <libtracker-miners-common/tracker-file-utils.h>

#include "tracker-cue-sheet.h"

TrackerToc *
tracker_toc_new (void)
{
	TrackerToc *toc;

	toc = g_slice_new (TrackerToc);
	toc->tag_list = gst_tag_list_new_empty ();
	toc->entry_list = NULL;

	return toc;
}

void
tracker_toc_free (TrackerToc *toc)
{
	TrackerTocEntry *entry;
	GList *n;

	if (!toc) {
		return;
	}

	for (n = toc->entry_list; n != NULL; n = n->next) {
		entry = n->data;
		gst_tag_list_free (entry->tag_list);
		g_slice_free (TrackerTocEntry, entry);
	}

	gst_tag_list_free (toc->tag_list);
	g_list_free (toc->entry_list);

	g_slice_free (TrackerToc, toc);
}

void
tracker_toc_add_entry (TrackerToc *toc,
                       GstTagList *tags,
                       gdouble     start,
                       gdouble     duration)
{
	TrackerTocEntry *toc_entry;

	toc_entry = g_slice_new (TrackerTocEntry);
	toc_entry->tag_list = gst_tag_list_ref (tags);
	toc_entry->start = start;
	toc_entry->duration = duration;

	toc->entry_list = g_list_append (toc->entry_list, toc_entry);
}

#if defined(HAVE_LIBCUE)

static void
add_cdtext_string_tag (Cdtext      *cd_text,
                       enum Pti     index,
                       GstTagList  *tag_list,
                       const gchar *tag)
{
	const gchar *text;

	text = cdtext_get (index, cd_text);

	if (text != NULL) {
		gst_tag_list_add (tag_list, GST_TAG_MERGE_REPLACE, tag, text, NULL);
	}
}

static void
add_cdtext_comment_date_tag (Rem          *cd_comments,
                             enum RemType index,
                             GstTagList   *tag_list,
                             const gchar  *tag)
{
	const gchar *text;
	gint year;
	GDate *date;

	text = rem_get (index, cd_comments);

	if (text != NULL) {
		year = atoi (text);

		if (year >= 1860) {
			date = g_date_new_dmy (1, 1, year);
			gst_tag_list_add (tag_list, GST_TAG_MERGE_REPLACE, tag, date, NULL);
			g_date_free (date);
		}
	}
}

static void
add_cdtext_comment_double_tag (Rem          *cd_comments,
                               enum RemType  index,
                               GstTagList   *tag_list,
                               const gchar  *tag)
{
	const gchar *text;
	gdouble value;

	text = rem_get (index, cd_comments);

	if (text != NULL) {
		value = strtod (text, NULL);

		/* Shortcut: it just so happens that 0.0 is meaningless for the replay
		 * gain properties so we can get away with testing for errors this way.
		 */
		if (value != 0.0)
			gst_tag_list_add (tag_list, GST_TAG_MERGE_REPLACE, tag, value, NULL);
	}
}

static void
set_album_tags_from_cdtext (GstTagList *tag_list,
                            Cdtext     *cd_text,
                            Rem        *cd_comments)
{
	if (cd_text != NULL) {
		add_cdtext_string_tag (cd_text, PTI_TITLE, tag_list, GST_TAG_ALBUM);
		add_cdtext_string_tag (cd_text, PTI_PERFORMER, tag_list, GST_TAG_ALBUM_ARTIST);
	}

	if (cd_comments != NULL) {
		add_cdtext_comment_date_tag (cd_comments, REM_DATE, tag_list, GST_TAG_DATE);

		add_cdtext_comment_double_tag (cd_comments, REM_REPLAYGAIN_ALBUM_GAIN, tag_list, GST_TAG_ALBUM_GAIN);
		add_cdtext_comment_double_tag (cd_comments, REM_REPLAYGAIN_ALBUM_PEAK, tag_list, GST_TAG_ALBUM_PEAK);
	}
}

static void
set_track_tags_from_cdtext (GstTagList *tag_list,
                            Cdtext     *cd_text,
                            Rem        *cd_comments)
{
	if (cd_text != NULL) {
		add_cdtext_string_tag (cd_text, PTI_TITLE, tag_list, GST_TAG_TITLE);
		add_cdtext_string_tag (cd_text, PTI_PERFORMER, tag_list, GST_TAG_PERFORMER);
		add_cdtext_string_tag (cd_text, PTI_COMPOSER, tag_list, GST_TAG_COMPOSER);
	}

	if (cd_comments != NULL) {
		add_cdtext_comment_double_tag (cd_comments, REM_REPLAYGAIN_TRACK_GAIN, tag_list, GST_TAG_TRACK_GAIN);
		add_cdtext_comment_double_tag (cd_comments, REM_REPLAYGAIN_TRACK_PEAK, tag_list, GST_TAG_TRACK_PEAK);
	}
}

/* Some simple heuristics to fill in missing tag information. */
static void
process_toc_tags (TrackerToc *toc)
{
	gint track_count;

	if (gst_tag_list_get_tag_size (toc->tag_list, GST_TAG_TRACK_COUNT) == 0) {
		track_count = g_list_length (toc->entry_list);
		gst_tag_list_add (toc->tag_list,
		                  GST_TAG_MERGE_REPLACE,
		                  GST_TAG_TRACK_COUNT,
		                  track_count,
		                  NULL);
	}
}

/* This function runs in two modes: for external CUE sheets, it will check
 * the FILE field for each track and build a TrackerToc for all the tracks
 * contained in @file_name. If @file_name does not appear in the CUE sheet,
 * %NULL will be returned. For embedded CUE sheets, @file_name will be NULL
 * the whole TOC will be returned regardless of any FILE information.
 */
static TrackerToc *
parse_cue_sheet_for_file (const gchar *cue_sheet,
                          const gchar *file_name)
{
	TrackerToc *toc;
	TrackerTocEntry *toc_entry;
	Cd *cd;
	Track *track;
	gint i;

	toc = NULL;

	cd = cue_parse_string (cue_sheet);

	if (cd == NULL) {
		g_debug ("Unable to parse CUE sheet for %s.",
		         file_name ? file_name : "(embedded in FLAC)");
		return NULL;
	}

	for (i = 1; i <= cd_get_ntrack (cd); i++) {
		track = cd_get_track (cd, i);

		/* CUE sheets generally have the correct basename but wrong
		 * extension in the FILE field, so this is what we test for.
		 */
		if (file_name != NULL) {
			if (!tracker_filename_casecmp_without_extension (file_name,
			                                                 track_get_filename (track))) {
				continue;
			}
		}

		if (track_get_mode (track) != MODE_AUDIO)
			continue;

		if (toc == NULL) {
			toc = tracker_toc_new ();

			set_album_tags_from_cdtext (toc->tag_list,
			                            cd_get_cdtext (cd),
			                            cd_get_rem (cd));
		}

		toc_entry = g_slice_new (TrackerTocEntry);
		toc_entry->tag_list = gst_tag_list_new_empty ();
		toc_entry->start = track_get_start (track) / 75.0;
		toc_entry->duration = track_get_length (track) / 75.0;

		set_track_tags_from_cdtext (toc_entry->tag_list,
		                            track_get_cdtext (track),
		                            track_get_rem (track));

		gst_tag_list_add (toc_entry->tag_list,
		                  GST_TAG_MERGE_REPLACE,
		                  GST_TAG_TRACK_NUMBER,
		                  i,
		                  NULL);


		toc->entry_list = g_list_prepend (toc->entry_list, toc_entry);
	}

	cd_delete (cd);

	if (toc != NULL)
		toc->entry_list = g_list_reverse (toc->entry_list);

	return toc;
}

TrackerToc *
tracker_cue_sheet_parse (const gchar *cue_sheet)
{
	TrackerToc *result;

	result = parse_cue_sheet_for_file (cue_sheet, NULL);

	if (result)
		process_toc_tags (result);

	return result;
}

static GList *
find_local_cue_sheets (TrackerSparqlConnection *conn,
                       GFile                   *audio_file)
{
	g_autoptr (TrackerSparqlStatement) stmt = NULL;
	g_autoptr (TrackerSparqlCursor) cursor = NULL;
	g_autoptr (GFile) parent = NULL;
	g_autofree gchar *parent_uri = NULL;
	GList *result = NULL;

	stmt = tracker_sparql_connection_query_statement (conn,
	                                                  "SELECT ?u {"
	                                                  "  GRAPH tracker:FileSystem {"
	                                                  "    ?u a nfo:FileDataObject ;"
	                                                  "      nfo:fileName ?fn ;"
	                                                  "      nfo:belongsToContainer/nie:isStoredAs ?c ."
	                                                  "    FILTER (?c = ~parent) ."
	                                                  "    FILTER (STRENDS (?fn, \".cue\")) ."
	                                                  "  }"
	                                                  "}",
	                                                  NULL, NULL);
	if (!stmt)
		return NULL;

	parent = g_file_get_parent (audio_file);
	parent_uri = g_file_get_uri (parent);
	tracker_sparql_statement_bind_string (stmt, "parent", parent_uri);
	cursor = tracker_sparql_statement_execute (stmt, NULL, NULL);

	if (!cursor)
		return NULL;

	while (tracker_sparql_cursor_next (cursor, NULL, NULL)) {
		const gchar *str;

		str = tracker_sparql_cursor_get_string (cursor, 0, NULL);
		result = g_list_prepend (result, g_file_new_for_uri (str));
	}

	return result;
}

static GFile *
find_matching_cue_file (GFile *audio_file)
{
	const gchar *dot;
	g_autofree gchar *uri = NULL, *cue_uri = NULL;
	g_autoptr (GFile) file = NULL;

	uri = g_file_get_uri (audio_file);
	dot = strrchr (uri, '.');
	if (!dot)
		return NULL;

	cue_uri = g_strdup_printf ("%.*s.cue", (int) (dot - uri), uri);
	file = g_file_new_for_uri (cue_uri);

	if (g_file_query_exists (file, NULL))
		return g_steal_pointer (&file);

	return NULL;
}

TrackerToc *
tracker_cue_sheet_guess_from_uri (TrackerSparqlConnection *conn,
                                  const gchar             *uri)
{
	GFile *audio_file;
	GFile *cue_sheet_file;
	gchar *audio_file_name;
	GList *cue_sheet_list = NULL;
	TrackerToc *toc;
	GError *error = NULL;
	GList *n;

	audio_file = g_file_new_for_uri (uri);
	audio_file_name = g_file_get_basename (audio_file);

	cue_sheet_file = find_matching_cue_file (audio_file);

	if (cue_sheet_file)
		cue_sheet_list = g_list_prepend (cue_sheet_list, cue_sheet_file);
	else if (conn)
		cue_sheet_list = find_local_cue_sheets (conn, audio_file);

	toc = NULL;

	for (n = cue_sheet_list; n != NULL; n = n->next) {
		gchar *buffer;

		cue_sheet_file = n->data;

		if (!g_file_load_contents (cue_sheet_file, NULL, &buffer, NULL, NULL, &error)) {
			g_debug ("Unable to read cue sheet: %s", error->message);
			g_error_free (error);
			continue;
		}

		toc = parse_cue_sheet_for_file (buffer, audio_file_name);

		g_free (buffer);

		if (toc != NULL) {
			char *path = g_file_get_path (cue_sheet_file);
			g_debug ("Using external CUE sheet: %s", path);
			g_free (path);
			break;
		}
	}

	g_list_foreach (cue_sheet_list, (GFunc) g_object_unref, NULL);
	g_list_free (cue_sheet_list);

	g_object_unref (audio_file);
	g_free (audio_file_name);

	if (toc)
		process_toc_tags (toc);

	return toc;
}

#else  /* ! HAVE_LIBCUE */

TrackerToc *
tracker_cue_sheet_parse (const gchar *cue_sheet)
{
	return NULL;
}

TrackerToc *
tracker_cue_sheet_guess_from_uri (TrackerSparqlConnection *conn,
                                  const gchar             *uri)
{
	return NULL;
}

#endif /* ! HAVE_LIBCUE */
