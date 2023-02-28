/*
 * Copyright (C) 2009, Nokia <ivan.frade@nokia.com>
 * Copyright (C) 2014, SoftAtHome <contact@softathome.com>
 * Copyright (C) 2014, Lanedo <martyn@lanedo.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config-miners.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <locale.h>

#include <glib.h>
#include <glib/gi18n.h>

#include <libtracker-miners-common/tracker-common.h>
#include <libtracker-sparql/tracker-sparql.h>

#include "tracker-color.h"

static gint limit = -1;
static gint offset;
static gchar **terms;
static gboolean or_operator;
static gboolean detailed;
static gboolean all;
static gboolean disable_snippets;
static gboolean disable_fts;
static gboolean disable_color;
static gboolean files;
static gboolean folders;
static gboolean music_albums;
static gboolean music_artists;
static gboolean music_files;
static gboolean image_files;
static gboolean video_files;
static gboolean document_files;
static gboolean feeds;
static gboolean software;
static gboolean software_categories;

#define SEARCH_OPTIONS_ENABLED() \
	(music_albums || music_artists || music_files || \
	 feeds || \
	 software || \
	 software_categories || \
	 image_files || \
	 video_files || \
	 document_files || \
	 files || \
	 folders || \
	 (terms && g_strv_length (terms) > 0))

static GOptionEntry entries[] = {
	/* Search types */
	{ "files", 'f', 0, G_OPTION_ARG_NONE, &files,
	  N_("Search for files"),
	  NULL
	},
	{ "folders", 's', 0, G_OPTION_ARG_NONE, &folders,
	  N_("Search for folders"),
	  NULL
	},
	{ "music", 'm', 0, G_OPTION_ARG_NONE, &music_files,
	  N_("Search for music files"),
	  NULL
	},
	{ "music-albums", 0, 0, G_OPTION_ARG_NONE, &music_albums,
	  N_("Search for music albums (--all has no effect on this)"),
	  NULL
	},
	{ "music-artists", 0, 0, G_OPTION_ARG_NONE, &music_artists,
	  N_("Search for music artists (--all has no effect on this)"),
	  NULL
	},
	{ "images", 'i', 0, G_OPTION_ARG_NONE, &image_files,
	  N_("Search for image files"),
	  NULL
	},
	{ "videos", 'v', 0, G_OPTION_ARG_NONE, &video_files,
	  N_("Search for video files"),
	  NULL
	},
	{ "documents", 't', 0, G_OPTION_ARG_NONE, &document_files,
	  N_("Search for document files"),
	  NULL
	},
	{ "software", 0, 0, G_OPTION_ARG_NONE, &software,
	  N_("Search for software (--all has no effect on this)"),
	  NULL
	},
	{ "software-categories", 0, 0, G_OPTION_ARG_NONE, &software_categories,
	  N_("Search for software categories (--all has no effect on this)"),
	  NULL
	},
	{ "feeds", 0, 0, G_OPTION_ARG_NONE, &feeds,
	  N_("Search for feeds (--all has no effect on this)"),
	  NULL
	},

	/* Semantic options */
	{ "limit", 'l', 0, G_OPTION_ARG_INT, &limit,
	  N_("Limit the number of results shown"),
	  NULL
	},
	{ "offset", 'o', 0, G_OPTION_ARG_INT, &offset,
	  N_("Offset the results"),
	  "0"
	},
	{ "or-operator", 'r', 0, G_OPTION_ARG_NONE, &or_operator,
	  N_("Use OR for search terms instead of AND (the default)"),
	  NULL
	},
	{ "detailed", 'd', 0, G_OPTION_ARG_NONE, &detailed,
	  N_("Show URNs for results (doesn’t apply to --music-albums, --music-artists, --feeds, --software, --software-categories)"),
	  NULL
	},
	{ "all", 'a', 0, G_OPTION_ARG_NONE, &all,
	  N_("Return all non-existing matches too (i.e. include unmounted volumes)"),
	  NULL
	},
	{ "disable-snippets", 0, 0, G_OPTION_ARG_NONE, &disable_snippets,
	  N_("Disable showing snippets with results. This is only shown for some categories, e.g. Documents, Music…"),
	  NULL,
	},
	{ "disable-fts", 0, 0, G_OPTION_ARG_NONE, &disable_fts,
	  N_("Disable Full Text Search (FTS). Implies --disable-snippets"),
	  NULL,
	},
	{ "disable-color", 0, 0, G_OPTION_ARG_NONE, &disable_color,
	  N_("Disable color when printing snippets and results"),
	  NULL,
	},

	/* Main arguments, the search terms */
	{ G_OPTION_REMAINING, 0, 0,
	  G_OPTION_ARG_STRING_ARRAY, &terms,
	  N_("search terms"),
	  N_("EXPRESSION")
	},
	{ NULL }
};

static gchar *
get_fts_string (GStrv    search_words,
                gboolean use_or_operator)
{
	GString *fts;
	gint i, len;

	if (disable_fts) {
		return NULL;
	}

	if (!search_words) {
		return NULL;
	}

	fts = g_string_new ("");
	len = g_strv_length (search_words);

	for (i = 0; i < len; i++) {
		gchar *escaped;

		/* Properly escape the input string as it's going to be passed
		 * in a sparql query */
		escaped = tracker_sparql_escape_string (search_words[i]);

		g_string_append (fts, escaped);

		if (i < len - 1) {
			if (use_or_operator) {
				g_string_append (fts, " OR ");
			} else {
				g_string_append (fts, " ");
			}
		}

		g_free (escaped);
	}

	return g_string_free (fts, FALSE);
}

static inline void
print_snippet (const gchar *snippet)
{
	if (disable_snippets) {
		return;
	}

	if (!snippet || *snippet == '\0') {
		return;
	} else {
		gchar *compressed;
		gchar *p;

		compressed = g_strdup (snippet);

		for (p = compressed;
		     p && *p != '\0';
		     p = g_utf8_next_char (p)) {
			if (*p == '\r' || *p == '\n') {
				/* inline \n and \r */
				*p = ' ';
			}
		}

		g_print ("  %s\n", compressed);
		g_free (compressed);
	}

	g_print ("\n");
}

static gboolean
get_files_results (TrackerSparqlConnection *connection,
                   const gchar             *query,
                   gint                     search_limit,
                   gboolean                 details)
{
	GError *error = NULL;
	TrackerSparqlCursor *cursor;

	cursor = tracker_sparql_connection_query (connection, query, NULL, &error);

	if (error) {
		g_printerr ("%s, %s\n",
		            _("Could not get search results"),
		            error->message);
		g_error_free (error);

		return FALSE;
	}

	if (!cursor) {
		g_print ("%s\n",
		         _("No files were found"));
	} else {
		gint count = 0;

		g_print ("%s:\n", _("Files"));

		while (tracker_sparql_cursor_next (cursor, NULL, NULL)) {
			if (details) {
				g_print ("  %s%s%s (%s)\n",
		                         disable_color ? "" : TITLE_BEGIN,
				         tracker_sparql_cursor_get_string (cursor, 1, NULL),
		                         disable_color ? "" : TITLE_END,
				         tracker_sparql_cursor_get_string (cursor, 0, NULL));

				if (tracker_sparql_cursor_get_n_columns (cursor) > 2)
					print_snippet (tracker_sparql_cursor_get_string (cursor, 2, NULL));
			} else {
				g_print ("  %s%s%s\n",
		                         disable_color ? "" : TITLE_BEGIN,
				         tracker_sparql_cursor_get_string (cursor, 1, NULL),
				         disable_color ? "" : TITLE_END);

				if (tracker_sparql_cursor_get_n_columns (cursor) > 2)
					print_snippet (tracker_sparql_cursor_get_string (cursor, 2, NULL));
			}

			count++;
		}

		g_print ("\n");


		g_object_unref (cursor);
	}

	return TRUE;
}

static gboolean
get_document_files (TrackerSparqlConnection *connection,
                    GStrv                    search_terms,
                    gboolean                 show_all,
                    gint                     search_offset,
                    gint                     search_limit,
                    gboolean                 use_or_operator,
                    gboolean                 details)
{
	gchar *fts;
	gchar *query;
	const gchar *show_all_str;
	gboolean success;
	gchar *limit_str;

	if (search_limit != -1)
		limit_str = g_strdup_printf ("LIMIT %d", search_limit);
	else
		limit_str = g_strdup_printf (" ");

	show_all_str = show_all ? "" : "?u nie:dataSource/tracker:available true .";
	fts = get_fts_string (search_terms, use_or_operator);

	if (fts) {
		query = g_strdup_printf ("SELECT ?document ?u fts:snippet(?document, \"%s\", \"%s\") "
		                         "WHERE { "
					 "  GRAPH tracker:Documents {"
		                         "    ?document a nfo:Document ;"
		                         "      nie:isStoredAs ?u ;"
		                         "      fts:match \"%s\" ."
		                         "  }"
		                         "  GRAPH tracker:FileSystem {"
		                         "    %s"
		                         "  }"
		                         "} "
		                         "ORDER BY ASC(?u) "
		                         "OFFSET %d "
		                         "%s",
		                         disable_color ? "" : SNIPPET_BEGIN,
		                         disable_color ? "" : SNIPPET_END,
		                         fts,
		                         show_all_str,
		                         search_offset,
		                         limit_str);
	} else {
		query = g_strdup_printf ("SELECT ?document ?u "
		                         "WHERE { "
					 "  GRAPH tracker:Documents {"
		                         "    ?document a nfo:Document ;"
		                         "      nie:isStoredAs ?u ."
		                         "  }"
		                         "  GRAPH tracker:FileSystem {"
		                         "    %s"
		                         "  }"
		                         "} "
		                         "ORDER BY ASC(?u) "
		                         "OFFSET %d "
		                         "%s",
		                         show_all_str,
		                         search_offset,
		                         limit_str);
	}

	success = get_files_results (connection, query, search_limit, details);
	g_free (query);
	g_free (fts);
	g_free (limit_str);

	return success;
}

static gboolean
get_video_files (TrackerSparqlConnection *connection,
                 GStrv                    search_terms,
                 gboolean                 show_all,
                 gint                     search_offset,
                 gint                     search_limit,
                 gboolean                 use_or_operator,
                 gboolean                 details)
{
	gchar *fts;
	gchar *query;
	const gchar *show_all_str;
	gboolean success;
	gchar *limit_str;

	if (search_limit != -1)
		limit_str = g_strdup_printf ("LIMIT %d", search_limit);
	else
		limit_str = g_strdup_printf (" ");

	show_all_str = show_all ? "" : "?u nie:dataSource/tracker:available true . ";
	fts = get_fts_string (search_terms, use_or_operator);

	if (fts) {
		query = g_strdup_printf ("SELECT ?video ?u fts:snippet(?video, \"%s\", \"%s\") "
		                         "WHERE { "
					 "  GRAPH tracker:Video {"
		                         "    ?video a nfo:Video ;"
		                         "      nie:isStoredAs ?u ;"
		                         "      fts:match \"%s\" ."
		                         "  }"
		                         "  GRAPH tracker:FileSystem {"
		                         "    %s"
		                         "  }"
		                         "} "
		                         "ORDER BY ASC(?u) "
		                         "OFFSET %d "
		                         "%s",
		                         disable_color ? "" : SNIPPET_BEGIN,
		                         disable_color ? "" : SNIPPET_END,
		                         fts,
		                         show_all_str,
		                         search_offset,
		                         limit_str);
	} else {
		query = g_strdup_printf ("SELECT ?video ?u "
		                         "WHERE { "
					 "  GRAPH tracker:Video {"
		                         "    ?video a nfo:Video ;"
		                         "      nie:isStoredAs ?u ."
		                         "  }"
		                         "  GRAPH tracker:FileSystem {"
		                         "    %s"
		                         "  }"
		                         "} "
		                         "ORDER BY ASC(?u) "
		                         "OFFSET %d "
		                         "%s",
		                         show_all_str,
		                         search_offset,
		                         limit_str);
	}

	success = get_files_results (connection, query, search_limit, details);
	g_free (query);
	g_free (fts);
	g_free (limit_str);

	return success;
}

static gboolean
get_image_files (TrackerSparqlConnection *connection,
                 GStrv                    search_terms,
                 gboolean                 show_all,
                 gint                     search_offset,
                 gint                     search_limit,
                 gboolean                 use_or_operator,
                 gboolean                 details)
{
	gchar *fts;
	gchar *query;
	const gchar *show_all_str;
	gboolean success;
	gchar *limit_str;

	if (search_limit != -1)
		limit_str = g_strdup_printf ("LIMIT %d", search_limit);
	else
		limit_str = g_strdup_printf (" ");

	show_all_str = show_all ? "" : "?u nie:dataSource/tracker:available true . ";
	fts = get_fts_string (search_terms, use_or_operator);

	if (fts) {
		query = g_strdup_printf ("SELECT ?image ?u fts:snippet(?image, \"%s\", \"%s\") "
		                         "WHERE { "
					 "  GRAPH tracker:Pictures {"
		                         "    ?image a nfo:Image ;"
					 "      nie:isStoredAs ?u ;"
		                         "      fts:match \"%s\" ."
		                         "  }"
		                         "  GRAPH tracker:FileSystem {"
		                         "    %s"
		                         "  }"
		                         "}"
		                         "ORDER BY ASC(?u) "
		                         "OFFSET %d "
		                         "%s",
		                         disable_color ? "" : SNIPPET_BEGIN,
		                         disable_color ? "" : SNIPPET_END,
		                         fts,
		                         show_all_str,
		                         search_offset,
		                         limit_str);
	} else {
		query = g_strdup_printf ("SELECT ?image ?u "
		                         "WHERE { "
					 "  GRAPH tracker:Pictures {"
		                         "    ?image a nfo:Image ;"
		                         "      nie:isStoredAs ?u ."
		                         "  }"
		                         "  GRAPH tracker:FileSystem {"
		                         "    %s"
		                         "  }"
		                         "}"
		                         "ORDER BY ASC(?u) "
		                         "OFFSET %d "
		                         "%s",
		                         show_all_str,
		                         search_offset,
		                         limit_str);
	}

	success = get_files_results (connection, query, search_limit, details);
	g_free (query);
	g_free (fts);
	g_free (limit_str);

	return success;
}

static gboolean
get_music_files (TrackerSparqlConnection *connection,
                 GStrv                    search_terms,
                 gboolean                 show_all,
                 gint                     search_offset,
                 gint                     search_limit,
                 gboolean                 use_or_operator,
                 gboolean                 details)
{
	gchar *fts;
	gchar *query;
	const gchar *show_all_str;
	gboolean success;
	gchar *limit_str;

	if (search_limit != -1)
		limit_str = g_strdup_printf ("LIMIT %d", search_limit);
	else
		limit_str = g_strdup_printf (" ");

	show_all_str = show_all ? "" : "?u nie:dataSource/tracker:available true . ";
	fts = get_fts_string (search_terms, use_or_operator);

	if (fts) {
		query = g_strdup_printf ("SELECT ?song ?u fts:snippet(?song, \"%s\", \"%s\")"
		                         "WHERE { "
					 "  GRAPH tracker:Audio {"
		                         "    ?song a nmm:MusicPiece ;"
					 "      nie:isStoredAs ?u ;"
		                         "      fts:match \"%s\" ."
		                         "  }"
		                         "  GRAPH tracker:FileSystem {"
		                         "    %s"
					 "  }"
		                         "} "
		                         "ORDER BY ASC(?u) "
		                         "OFFSET %d "
		                         "%s",
		                         disable_color ? "" : SNIPPET_BEGIN,
		                         disable_color ? "" : SNIPPET_END,
		                         fts,
		                         show_all_str,
		                         search_offset,
		                         limit_str);
	} else {
		query = g_strdup_printf ("SELECT ?song ?u "
		                         "WHERE { "
					 "  GRAPH tracker:Audio {"
		                         "    ?song a nmm:MusicPiece ;"
					 "      nie:isStoredAs ?u ."
		                         "  }"
		                         "  GRAPH tracker:FileSystem {"
		                         "    %s"
					 "  }"
		                         "} "
		                         "ORDER BY ASC(?u) "
		                         "OFFSET %d "
		                         "%s",
		                         show_all_str,
		                         search_offset,
		                         limit_str);
	}

	success = get_files_results (connection, query, search_limit, details);
	g_free (query);
	g_free (fts);
	g_free (limit_str);

	return success;
}

static gboolean
get_music_artists (TrackerSparqlConnection *connection,
                   GStrv                    search_terms,
                   gint                     search_offset,
                   gint                     search_limit,
                   gboolean                 use_or_operator,
                   gboolean                 details)
{
	GError *error = NULL;
	TrackerSparqlCursor *cursor;
	gchar *fts;
	gchar *query;
	gchar *limit_str;

	if (search_limit != -1)
		limit_str = g_strdup_printf ("LIMIT %d", search_limit);
	else
		limit_str = g_strdup_printf (" ");

	fts = get_fts_string (search_terms, use_or_operator);

	if (fts) {
		query = g_strdup_printf ("SELECT ?artist ?title "
		                         "WHERE {"
		                         "  GRAPH tracker:Audio {"
		                         "    ?artist a nmm:Artist ;"
		                         "    nmm:artistName ?title ;"
		                         "    fts:match \"%s\" . "
		                         "  }"
		                         "} "
		                         "ORDER BY ASC(?title) "
		                         "OFFSET %d "
		                         "%s",
		                         fts,
		                         search_offset,
		                         limit_str);
	} else {
		query = g_strdup_printf ("SELECT ?artist ?title "
		                         "WHERE {"
		                         "  GRAPH tracker:Audio {"
		                         "    ?artist a nmm:Artist ;"
		                         "    nmm:artistName ?title . "
		                         "  }"
		                         "} "
		                         "ORDER BY ASC(?title) "
		                         "OFFSET %d "
		                         "%s",
		                         search_offset,
		                         limit_str);
	}

	g_free (fts);
	g_free (limit_str);

	cursor = tracker_sparql_connection_query (connection, query, NULL, &error);
	g_free (query);

	if (error) {
		g_printerr ("%s, %s\n",
		            _("Could not get search results"),
		            error->message);
		g_error_free (error);

		return FALSE;
	}

	if (!cursor) {
		g_print ("%s\n",
		         _("No artists were found"));
	} else {
		gint count = 0;

		g_print ("%s:\n", _("Artists"));

		while (tracker_sparql_cursor_next (cursor, NULL, NULL)) {
			if (details) {
				g_print ("  '%s%s%s' (%s)\n",
		                         disable_color ? "" : TITLE_BEGIN,
				         tracker_sparql_cursor_get_string (cursor, 1, NULL),
		                         disable_color ? "" : TITLE_END,
				         tracker_sparql_cursor_get_string (cursor, 0, NULL));
			} else {
				g_print ("  '%s%s%s'\n",
		                         disable_color ? "" : TITLE_BEGIN,
				         tracker_sparql_cursor_get_string (cursor, 1, NULL),
				         disable_color ? "" : TITLE_END);
			}
			count++;
		}

		g_print ("\n");

		g_object_unref (cursor);
	}

	return TRUE;
}

static gboolean
get_music_albums (TrackerSparqlConnection *connection,
                  GStrv                    search_words,
                  gint                     search_offset,
                  gint                     search_limit,
                  gboolean                 use_or_operator,
                  gboolean                 details)
{
	GError *error = NULL;
	TrackerSparqlCursor *cursor;
	gchar *fts;
	gchar *query;
	gchar *limit_str;

	if (search_limit != -1)
		limit_str = g_strdup_printf ("LIMIT %d", search_limit);
	else
		limit_str = g_strdup_printf (" ");

	fts = get_fts_string (search_words, use_or_operator);

	if (fts) {
		query = g_strdup_printf ("SELECT ?album nie:title(?album) "
		                         "WHERE {"
					 "  GRAPH tracker:Audio {"
		                         "    ?album a nmm:MusicAlbum ;"
		                         "    fts:match \"%s\" ."
		                         "  }"
		                         "} "
		                         "ORDER BY ASC(nie:title(?album)) "
		                         "OFFSET %d "
		                         "%s",
		                         fts,
		                         search_offset,
		                         limit_str);
	} else {
		query = g_strdup_printf ("SELECT ?album nie:title(?album) "
		                         "WHERE {"
					 "  GRAPH tracker:Audio {"
		                         "    ?album a nmm:MusicAlbum ."
		                         "  }"
		                         "} "
		                         "ORDER BY ASC(nie:title(?album)) "
		                         "OFFSET %d "
		                         "%s",
		                         search_offset,
		                         limit_str);
	}

	g_free (fts);
	g_free (limit_str);

	cursor = tracker_sparql_connection_query (connection, query, NULL, &error);
	g_free (query);

	if (error) {
		g_printerr ("%s, %s\n",
		            _("Could not get search results"),
		            error->message);
		g_error_free (error);

		return FALSE;
	}

	if (!cursor) {
		g_print ("%s\n",
		         _("No music was found"));
	} else {
		gint count = 0;

		g_print ("%s:\n", _("Albums"));

		while (tracker_sparql_cursor_next (cursor, NULL, NULL)) {
			if (details) {
				g_print ("  '%s%s%s' (%s)\n",
		                         disable_color ? "" : TITLE_BEGIN,
				         tracker_sparql_cursor_get_string (cursor, 1, NULL),
		                         disable_color ? "" : TITLE_END,
				         tracker_sparql_cursor_get_string (cursor, 0, NULL));
			} else {
				g_print ("  '%s%s%s'\n",
		                         disable_color ? "" : TITLE_BEGIN,
				         tracker_sparql_cursor_get_string (cursor, 1, NULL),
				         disable_color ? "" : TITLE_END);
			}
			count++;
		}

		g_print ("\n");


		g_object_unref (cursor);
	}

	return TRUE;
}

static gboolean
get_feeds (TrackerSparqlConnection *connection,
           GStrv                    search_terms,
           gint                     search_offset,
           gint                     search_limit,
           gboolean                 use_or_operator)
{
	GError *error = NULL;
	TrackerSparqlCursor *cursor;
	gchar *fts;
	gchar *query;
	gchar *limit_str;

	if (search_limit != -1)
		limit_str = g_strdup_printf ("LIMIT %d", search_limit);
	else
		limit_str = g_strdup_printf (" ");

	fts = get_fts_string (search_terms, use_or_operator);

	if (fts) {
		query = g_strdup_printf ("SELECT ?feed nie:title(?feed) "
		                         "WHERE {"
		                         "  ?feed a mfo:FeedMessage ;"
		                         "  fts:match \"%s\" . "
		                         "} "
		                         "ORDER BY ASC(nie:title(?feed)) "
		                         "OFFSET %d "
		                         "%s",
		                         fts,
		                         search_offset,
		                         limit_str);
	} else {
		query = g_strdup_printf ("SELECT ?feed nie:title(?feed) "
		                         "WHERE {"
		                         "  ?feed a mfo:FeedMessage ."
		                         "} "
		                         "ORDER BY ASC(nie:title(?feed)) "
		                         "OFFSET %d "
		                         "%s",
		                         search_offset,
		                         limit_str);
	}

	g_free (fts);
	g_free (limit_str);

	cursor = tracker_sparql_connection_query (connection, query, NULL, &error);
	g_free (query);

	if (error) {
		g_printerr ("%s, %s\n",
		            _("Could not get search results"),
		            error->message);
		g_error_free (error);

		return FALSE;
	}

	if (!cursor) {
		g_print ("%s\n",
		         _("No feeds were found"));
	} else {
		gint count = 0;

		g_print ("%s:\n", _("Feeds"));

		while (tracker_sparql_cursor_next (cursor, NULL, NULL)) {
			g_print ("  %s%s%s (%s)\n",
			         disable_color ? "" : TITLE_BEGIN,
			         tracker_sparql_cursor_get_string (cursor, 0, NULL),
			         disable_color ? "" : TITLE_END,
			         tracker_sparql_cursor_get_string (cursor, 1, NULL));

			count++;
		}

		g_print ("\n");

		g_object_unref (cursor);
	}

	return TRUE;
}

static gboolean
get_software (TrackerSparqlConnection *connection,
              GStrv                    search_terms,
              gint                     search_offset,
              gint                     search_limit,
              gboolean                 use_or_operator)
{
	GError *error = NULL;
	TrackerSparqlCursor *cursor;
	gchar *fts;
	gchar *query;
	gchar *limit_str;

	if (search_limit != -1)
		limit_str = g_strdup_printf ("LIMIT %d", search_limit);
	else
		limit_str = g_strdup_printf (" ");

	fts = get_fts_string (search_terms, use_or_operator);

	if (fts) {
		query = g_strdup_printf ("SELECT ?soft nie:title(?soft) fts:snippet(?soft, \"%s\", \"%s\") "
		                         "WHERE {"
					 "  GRAPH tracker:Software {"
		                         "    ?soft a nfo:Software ;"
		                         "    fts:match \"%s\" . "
		                         "  }"
		                         "} "
		                         "ORDER BY ASC(nie:title(?soft)) "
		                         "OFFSET %d "
		                         "%s",
		                         disable_color ? "" : SNIPPET_BEGIN,
		                         disable_color ? "" : SNIPPET_END,
		                         fts,
		                         search_offset,
		                         limit_str);
	} else {
		query = g_strdup_printf ("SELECT ?soft nie:title(?soft) "
		                         "WHERE {"
		                         "  ?soft a nfo:Software ."
		                         "} "
		                         "ORDER BY ASC(nie:title(?soft)) "
		                         "OFFSET %d "
		                         "%s",
		                         search_offset,
		                         limit_str);
	}

	g_free (fts);
	g_free (limit_str);

	cursor = tracker_sparql_connection_query (connection, query, NULL, &error);
	g_free (query);

	if (error) {
		g_printerr ("%s, %s\n",
		            _("Could not get search results"),
		            error->message);
		g_error_free (error);

		return FALSE;
	}

	if (!cursor) {
		g_print ("%s\n",
		         _("No software was found"));
	} else {
		gint count = 0;

		g_print ("%s:\n", _("Software"));

		while (tracker_sparql_cursor_next (cursor, NULL, NULL)) {
			g_print ("  %s%s%s (%s)\n",
			         disable_color ? "" : TITLE_BEGIN,
			         tracker_sparql_cursor_get_string (cursor, 0, NULL),
			         disable_color ? "" : TITLE_END,
			         tracker_sparql_cursor_get_string (cursor, 1, NULL));
			print_snippet (tracker_sparql_cursor_get_string (cursor, 2, NULL));
			count++;
		}

		g_print ("\n");

		g_object_unref (cursor);
	}

	return TRUE;
}

static gboolean
get_software_categories (TrackerSparqlConnection *connection,
                         GStrv                    search_terms,
                         gint                     search_offset,
                         gint                     search_limit,
                         gboolean                 use_or_operator)
{
	GError *error = NULL;
	TrackerSparqlCursor *cursor;
	gchar *fts;
	gchar *query;
	gchar *limit_str;

	if (search_limit != -1)
		limit_str = g_strdup_printf ("LIMIT %d", search_limit);
	else
		limit_str = g_strdup_printf (" ");

	fts = get_fts_string (search_terms, use_or_operator);

	if (fts) {
		query = g_strdup_printf ("SELECT ?cat nie:title(?cat) "
		                         "WHERE {"
					 "  GRAPH tracker:Software {"
		                         "    ?cat a nfo:SoftwareCategory ;"
		                         "    fts:match \"%s\" . "
		                         "  }"
		                         "} "
		                         "ORDER BY ASC(nie:title(?cat)) "
		                         "OFFSET %d "
		                         "%s",
		                         fts,
		                         search_offset,
		                         limit_str);
	} else {
		query = g_strdup_printf ("SELECT ?cat nie:title(?cat) "
		                         "WHERE {"
					 "  GRAPH tracker:Software {"
		                         "    ?cat a nfo:SoftwareCategory ."
		                         "  }"
		                         "} "
		                         "ORDER BY ASC(nie:title(?cat)) "
		                         "OFFSET %d "
		                         "%s",
		                         search_offset,
		                         limit_str);
	}

	g_free (fts);
	g_free (limit_str);

	cursor = tracker_sparql_connection_query (connection, query, NULL, &error);
	g_free (query);

	if (error) {
		g_printerr ("%s, %s\n",
		            _("Could not get search results"),
		            error->message);
		g_error_free (error);

		return FALSE;
	}

	if (!cursor) {
		g_print ("%s\n",
		         _("No software categories were found"));
	} else {
		gint count = 0;

		g_print ("%s:\n", _("Software Categories"));

		while (tracker_sparql_cursor_next (cursor, NULL, NULL)) {
			g_print ("  %s%s%s (%s)\n",
			         disable_color ? "" : TITLE_BEGIN,
			         tracker_sparql_cursor_get_string (cursor, 0, NULL),
			         disable_color ? "" : TITLE_END,
			         tracker_sparql_cursor_get_string (cursor, 1, NULL));

			count++;
		}

		g_print ("\n");

		g_object_unref (cursor);
	}

	return TRUE;
}

static gboolean
get_files (TrackerSparqlConnection *connection,
           GStrv                    search_terms,
           gboolean                 show_all,
           gint                     search_offset,
           gint                     search_limit,
           gboolean                 use_or_operator,
           gboolean                 details)
{
	gchar *fts;
	gchar *query;
	const gchar *show_all_str;
	gchar *limit_str;
	gboolean success;

	if (search_limit != -1)
		limit_str = g_strdup_printf ("LIMIT %d", search_limit);
	else
		limit_str = g_strdup_printf (" ");

	show_all_str = show_all ? "" : "?url nie:dataSource/tracker:available true . ";
	fts = get_fts_string (search_terms, use_or_operator);

	if (fts) {
		query = g_strdup_printf ("SELECT DISTINCT ?u ?url "
		                         "WHERE { "
		                         "  GRAPH ?g {"
		                         "    ?u a nie:InformationElement ;"
		                         "      nie:isStoredAs ?url ;"
		                         "      fts:match \"%s\" ."
		                         "  }"
		                         "  GRAPH tracker:FileSystem {"
		                         "    %s"
		                         "  }"
		                         "} "
		                         "ORDER BY ASC(?urn) "
		                         "OFFSET %d "
		                         "%s",
		                         fts,
		                         show_all_str,
		                         search_offset,
		                         limit_str);
	} else {
		query = g_strdup_printf ("SELECT DISTINCT ?u ?url "
		                         "WHERE { "
					 "  GRAPH ?g {"
		                         "    ?u a nie:InformationElement ;"
		                         "      nie:isStoredAs ?url ."
		                         "  }"
		                         "  GRAPH tracker:FileSystem {"
		                         "    %s"
		                         "  }"
		                         "} "
		                         "ORDER BY ASC(?urn) "
		                         "OFFSET %d "
		                         "%s",
		                         show_all_str,
		                         search_offset,
		                         limit_str);
	}

	success = get_files_results (connection, query, search_limit, details);
	g_free (query);
	g_free (fts);
	g_free (limit_str);

	return success;
}

static gboolean
get_folders (TrackerSparqlConnection *connection,
             GStrv                    search_terms,
             gboolean                 show_all,
             gint                     search_offset,
             gint                     search_limit,
             gboolean                 use_or_operator,
             gboolean                 details)
{
	gchar *fts;
	gchar *query;
	const gchar *show_all_str;
	gchar *limit_str;
	gboolean success;

	if (search_limit != -1)
		limit_str = g_strdup_printf ("LIMIT %d", search_limit);
	else
		limit_str = g_strdup_printf (" ");

	show_all_str = show_all ? "" : "?url nie:dataSource/tracker:available true . ";
	fts = get_fts_string (search_terms, use_or_operator);

	if (fts) {
		query = g_strdup_printf ("SELECT ?u ?url "
		                         "WHERE { "
					 "  GRAPH tracker:FileSystem {"
		                         "    ?u a nfo:Folder ;"
		                         "       nie:isStoredAs ?url ."
		                         "    ?url fts:match \"%s\" ."
		                         "    %s"
					 "  }"
		                         "} "
		                         "ORDER BY ASC(?url) "
		                         "OFFSET %d "
		                         "%s",
		                         fts,
		                         show_all_str,
		                         search_offset,
		                         limit_str);
	} else {
		query = g_strdup_printf ("SELECT ?u ?url "
		                         "WHERE { "
					 "  GRAPH tracker:FileSystem {"
		                         "    ?u a nfo:Folder ;"
					 "       nie:isStoredAs ?url ."
		                         "    %s"
					 "  }"
		                         "} "
		                         "ORDER BY ASC(?url) "
		                         "OFFSET %d "
		                         "%s",
		                         show_all_str,
		                         search_offset,
		                         limit_str);
	}

	success = get_files_results (connection, query, search_limit, details);
	g_free (query);
	g_free (fts);
	g_free (limit_str);

	return success;
}

static gboolean
get_all_by_search (TrackerSparqlConnection *connection,
                   GStrv                    search_words,
                   gboolean                 show_all,
                   gint                     search_offset,
                   gint                     search_limit,
                   gboolean                 use_or_operator,
                   gboolean                 details)
{
	GError *error = NULL;
	TrackerSparqlCursor *cursor;
	gchar *fts;
	gchar *query;

	fts = get_fts_string (search_words, use_or_operator);
	if (!fts) {
		return FALSE;
	}

	if (search_limit < 0)
		search_limit = G_MAXINT;

	if (details) {
		query = g_strdup_printf ("SELECT ?uri ?mimetype ?type ?snippet "
		                         "WHERE {"
					 "  {"
					 "    SELECT (?s AS ?uri) ?mimetype ?type (fts:snippet(?s, \"%s\", \"%s\") AS ?snippet) {"
					 "      GRAPH tracker:FileSystem {"
					 "        ?s a nfo:FileDataObject ;"
					 "           fts:match \"%s\" ;"
					 "           rdf:type ?type ;"
					 "           nie:dataSource ?ds ."
					 "         ?ie nie:isStoredAs ?s;"
					 "           nie:mimeType ?mimetype ."
					 "        OPTIONAL { ?ds tracker:available ?available } ."
					 "        FILTER (IF (%s, true, ?available)) ."
					 "      }"
					 "    }"
					 "  } UNION {"
					 "    SELECT ?uri ?mimetype ?type (fts:snippet(?s, \"%s\", \"%s\") AS ?snippet) {"
					 "      GRAPH ?g {"
					 "        ?s a nie:InformationElement ;"
					 "           fts:match \"%s\" ;"
					 "           rdf:type ?type ;"
					 "           nie:mimeType ?mimetype ;"
					 "           nie:isStoredAs ?uri ."
		                         "      }"
		                         "      GRAPH tracker:FileSystem {"
					 "        ?uri nie:dataSource ?ds ."
					 "        OPTIONAL { ?ds tracker:available ?available } ."
					 "        FILTER (IF (%s, true, ?available)) ."
					 "      }"
					 "    }"
					 "  }"
		                         "} "
		                         "GROUP BY ?uri "
		                         "ORDER BY ?uri "
		                         "OFFSET %d "
					 "LIMIT %d",
		                         disable_color ? "" : SNIPPET_BEGIN,
		                         disable_color ? "" : SNIPPET_END,
		                         fts,
		                         show_all ? "true" : "false",
		                         disable_color ? "" : SNIPPET_BEGIN,
		                         disable_color ? "" : SNIPPET_END,
		                         fts,
		                         show_all ? "true" : "false",
		                         search_offset,
		                         search_limit);
	} else {
		query = g_strdup_printf ("SELECT ?uri SAMPLE(?snippet) "
		                         "WHERE {"
					 "  {"
					 "    SELECT (?s AS ?uri) (fts:snippet(?s, \"%s\", \"%s\") AS ?snippet) {"
					 "      GRAPH tracker:FileSystem {"
					 "        ?s a nfo:FileDataObject ;"
					 "           fts:match \"%s\" ;"
					 "           nie:dataSource ?ds ."
					 "        OPTIONAL { ?ds tracker:available ?available } ."
					 "        FILTER (IF (%s, true, ?available))"
					 "      }"
					 "    }"
					 "  } UNION {"
					 "    SELECT ?uri (fts:snippet(?s, \"%s\", \"%s\") AS ?snippet) {"
					 "      GRAPH ?g {"
					 "        ?s a nie:InformationElement ;"
					 "           fts:match \"%s\" ;"
					 "           nie:isStoredAs ?uri ."
		                         "      }"
		                         "      GRAPH tracker:FileSystem {"
					 "        ?uri nie:dataSource ?ds ."
					 "        OPTIONAL { ?ds tracker:available ?available } ."
					 "        FILTER (IF (%s, true, ?available))"
					 "      }"
					 "    }"
					 "  }"
		                         "} "
		                         "GROUP BY ?uri "
		                         "ORDER BY ?uri "
		                         "OFFSET %d "
					 "LIMIT %d",
		                         disable_color ? "" : SNIPPET_BEGIN,
		                         disable_color ? "" : SNIPPET_END,
		                         fts,
		                         show_all ? "true" : "false",
		                         disable_color ? "" : SNIPPET_BEGIN,
		                         disable_color ? "" : SNIPPET_END,
		                         fts,
		                         show_all ? "true" : "false",
		                         search_offset,
		                         search_limit);
	}

	g_free (fts);

	cursor = tracker_sparql_connection_query (connection, query, NULL, &error);
	g_free (query);

	if (error) {
		g_printerr ("%s, %s\n",
		            _("Could not get search results"),
		            error->message);
		g_error_free (error);

		return FALSE;
	}

	if (!cursor) {
		g_print ("%s\n",
		         _("No results were found matching your query"));
	} else {
		gint count = 0;

		g_print ("%s:\n", _("Results"));

		while (tracker_sparql_cursor_next (cursor, NULL, NULL)) {
			if (details) {
				const gchar *urn;
				const gchar *mime_type;
				const gchar *class;

				g_print ("cols:%d\n", tracker_sparql_cursor_get_n_columns (cursor));

				urn = tracker_sparql_cursor_get_string (cursor, 0, NULL);
				mime_type = tracker_sparql_cursor_get_string (cursor, 1, NULL);
				class = tracker_sparql_cursor_get_string (cursor, 2, NULL);

				if (mime_type && mime_type[0] == '\0') {
					mime_type = NULL;
				}

				if (mime_type) {
					g_print ("  %s%s%s\n"
					         "    %s\n"
					         "    %s\n",
					         disable_color ? "" : TITLE_BEGIN,
					         urn,
					         disable_color ? "" : TITLE_END,
					         mime_type,
					         class);
				} else {
					g_print ("  %s%s%s\n"
					         "    %s\n",
					         disable_color ? "" : TITLE_BEGIN,
					         urn,
					         disable_color ? "" : TITLE_END,
					         class);
				}
				print_snippet (tracker_sparql_cursor_get_string (cursor, 3, NULL));
			} else {
				g_print ("  %s%s%s\n",
		                         disable_color ? "" : TITLE_BEGIN,
				         tracker_sparql_cursor_get_string (cursor, 0, NULL),
				         disable_color ? "" : TITLE_END);
				print_snippet (tracker_sparql_cursor_get_string (cursor, 1, NULL));
			}

			count++;
		}

		g_print ("\n");

		g_object_unref (cursor);
	}

	return TRUE;
}

static gint
search_run (void)
{
	TrackerSparqlConnection *connection;
	GError *error = NULL;

	if (disable_fts) {
		disable_snippets = TRUE;
	}

	connection = tracker_sparql_connection_bus_new ("org.freedesktop.Tracker3.Miner.Files",
							NULL, NULL, &error);

	if (!connection) {
		g_printerr ("%s: %s\n",
		            _("Could not establish a connection to Tracker"),
		            error ? error->message : _("No error given"));
		g_clear_error (&error);
		return EXIT_FAILURE;
	}

	tracker_term_pipe_to_pager ();

	if (files) {
		gboolean success;

		success = get_files (connection, terms, all, offset, limit, or_operator, detailed);
		g_object_unref (connection);
		tracker_term_pager_close ();

		return success ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	if (folders) {
		gboolean success;

		success = get_folders (connection, terms, all, offset, limit, or_operator, detailed);
		g_object_unref (connection);
		tracker_term_pager_close ();

		return success ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	if (music_albums) {
		gboolean success;

		success = get_music_albums (connection, terms, offset, limit, or_operator, detailed);
		g_object_unref (connection);
		tracker_term_pager_close ();

		return success ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	if (music_artists) {
		gboolean success;

		success = get_music_artists (connection, terms, offset, limit, or_operator, detailed);
		g_object_unref (connection);
		tracker_term_pager_close ();

		return success ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	if (music_files) {
		gboolean success;

		success = get_music_files (connection, terms, all, offset, limit, or_operator, detailed);
		g_object_unref (connection);
		tracker_term_pager_close ();

		return success ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	if (feeds) {
		gboolean success;

		success = get_feeds (connection, terms, offset, limit, or_operator);
		g_object_unref (connection);
		tracker_term_pager_close ();

		return success ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	if (image_files) {
		gboolean success;

		success = get_image_files (connection, terms, all, offset, limit, or_operator, detailed);
		g_object_unref (connection);
		tracker_term_pager_close ();

		return success ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	if (video_files) {
		gboolean success;

		success = get_video_files (connection, terms, all, offset, limit, or_operator, detailed);
		g_object_unref (connection);
		tracker_term_pager_close ();

		return success ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	if (document_files) {
		gboolean success;

		success = get_document_files (connection, terms, all, offset, limit, or_operator, detailed);
		g_object_unref (connection);
		tracker_term_pager_close ();

		return success ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	if (software) {
		gboolean success;

		success = get_software (connection, terms, offset, limit, or_operator);
		g_object_unref (connection);
		tracker_term_pager_close ();

		return success ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	if (software_categories) {
		gboolean success;

		success = get_software_categories (connection, terms, offset, limit, or_operator);
		g_object_unref (connection);
		tracker_term_pager_close ();

		return success ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	if (terms) {
		gboolean success;

		success = get_all_by_search (connection, terms, all, offset, limit, or_operator, detailed);
		g_object_unref (connection);
		tracker_term_pager_close ();

		return success ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	g_object_unref (connection);

	/* All known options have their own exit points */
	g_warn_if_reached ();

	return EXIT_FAILURE;
}

static int
search_run_default (void)
{
	GOptionContext *context;
	gchar *help;

	context = g_option_context_new (NULL);
	g_option_context_add_main_entries (context, entries, NULL);
	help = g_option_context_get_help (context, FALSE, NULL);
	g_option_context_free (context);
	g_printerr ("%s\n", help);
	g_free (help);

	return EXIT_FAILURE;
}

static gboolean
search_options_enabled (void)
{
	return SEARCH_OPTIONS_ENABLED ();
}

int
main (int argc, const char **argv)
{
	GOptionContext *context;
	GError *error = NULL;

	setlocale (LC_ALL, "");

	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	context = g_option_context_new (NULL);
	g_option_context_add_main_entries (context, entries, NULL);

	argv[0] = "tracker search";

	if (!g_option_context_parse (context, &argc, (char***) &argv, &error)) {
		g_printerr ("%s, %s\n", _("Unrecognized options"), error->message);
		g_error_free (error);
		g_option_context_free (context);
		return EXIT_FAILURE;
	}

	g_option_context_free (context);

	if (search_options_enabled ()) {
		return search_run ();
	}

	return search_run_default ();
}
