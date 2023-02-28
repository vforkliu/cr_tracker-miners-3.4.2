/*
 * Copyright (C) 2006, Jamie McCracken <jamiemcc@gnome.org>
 * Copyright (C) 2008-2011, Nokia <ivan.frade@nokia.com>
 * Copyright (C) 2010, Amit Aggarwal <amitcs06@gmail.com>
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

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <poppler.h>

#include <gio/gunixoutputstream.h>
#include <gio/gunixinputstream.h>

#include <libtracker-miners-common/tracker-date-time.h>
#include <libtracker-miners-common/tracker-utils.h>
#include <libtracker-miners-common/tracker-file-utils.h>

#include <libtracker-extract/tracker-extract.h>

#include "tracker-main.h"

/* Time in seconds before we stop processing content */
#define EXTRACTION_PROCESS_TIMEOUT 10

typedef struct {
	gchar *title;
	gchar *subject;
	gchar *creation_date;
	gchar *author;
	gchar *date;
	gchar *keywords;
} PDFData;

static void
read_toc (PopplerIndexIter  *index,
          GString          **toc)
{
	if (!index) {
		return;
	}

	if (!*toc) {
		*toc = g_string_new ("");
	}

	do {
		PopplerAction *action;
		PopplerIndexIter *iter;

		action = poppler_index_iter_get_action (index);

		if (!action) {
			continue;
		}

		switch (action->type) {
			case POPPLER_ACTION_GOTO_DEST: {
				PopplerActionGotoDest *ag = (PopplerActionGotoDest *)action;

				if (!tracker_is_empty_string (ag->title)) {
					g_string_append_printf (*toc, "%s ", ag->title);
				}

				break;
			}

			case POPPLER_ACTION_LAUNCH: {
				PopplerActionLaunch *al = (PopplerActionLaunch *)action;

				if (!tracker_is_empty_string (al->title)) {
					g_string_append_printf (*toc, "%s ", al->title);
				}

				if (!tracker_is_empty_string (al->file_name)) {
					g_string_append_printf (*toc, "%s ", al->file_name);
				}

				if (!tracker_is_empty_string (al->params)) {
					g_string_append_printf (*toc, "%s ", al->params);
				}

				break;
			}

			case POPPLER_ACTION_URI: {
				PopplerActionUri *au = (PopplerActionUri *)action;

				if (!tracker_is_empty_string (au->uri)) {
					g_string_append_printf (*toc, "%s ", au->uri);
				}

				break;
			}

			case POPPLER_ACTION_NAMED: {
				PopplerActionNamed *an = (PopplerActionNamed *)action;

				if (!tracker_is_empty_string (an->title)) {
					g_string_append_printf (*toc, "%s, ", an->title);
				}

				if (!tracker_is_empty_string (an->named_dest)) {
					g_string_append_printf (*toc, "%s ", an->named_dest);
				}

				break;
			}

			case POPPLER_ACTION_MOVIE: {
				PopplerActionMovie *am = (PopplerActionMovie *)action;

				if (!tracker_is_empty_string (am->title)) {
					g_string_append_printf (*toc, "%s ", am->title);
				}

				break;
			}

			case POPPLER_ACTION_NONE:
			case POPPLER_ACTION_UNKNOWN:
			case POPPLER_ACTION_GOTO_REMOTE:
			case POPPLER_ACTION_RENDITION:
			case POPPLER_ACTION_OCG_STATE:
			case POPPLER_ACTION_JAVASCRIPT:
			#if POPPLER_CHECK_VERSION (0, 90, 0)
  			case POPPLER_ACTION_RESET_FORM:
			#endif

				/* Do nothing */
				break;
		}

		poppler_action_free (action);
		iter = poppler_index_iter_get_child (index);
		read_toc (iter, toc);
	} while (poppler_index_iter_next (index));

	poppler_index_iter_free (index);
}

static void
read_outline (PopplerDocument *document,
              TrackerResource *metadata)
{
	PopplerIndexIter *index;
	GString *toc = NULL;

	index = poppler_index_iter_new (document);

	if (!index) {
		return;
	}

	read_toc (index, &toc);

	if (toc) {
		if (toc->len > 0) {
			tracker_resource_set_string (metadata, "nfo:tableOfContents", toc->str);
		}

		g_string_free (toc, TRUE);
	}
}

static gchar *
extract_content_text (PopplerDocument *document,
                      gsize            n_bytes)
{
	GString *string;
	GTimer *timer;
	gsize remaining_bytes;
	gint n_pages, i;
	gdouble elapsed;

	n_pages = poppler_document_get_n_pages (document);
	string = g_string_new ("");
	timer = g_timer_new ();

	for (i = 0, remaining_bytes = n_bytes, elapsed = g_timer_elapsed (timer, NULL);
	     i < n_pages && remaining_bytes > 0 && elapsed < EXTRACTION_PROCESS_TIMEOUT;
	     i++, elapsed = g_timer_elapsed (timer, NULL)) {
		PopplerPage *page;
		gsize written_bytes = 0;
		gchar *text;

		page = poppler_document_get_page (document, i);
		text = poppler_page_get_text (page);

		if (!text) {
			g_object_unref (page);
			continue;
		}

		if (tracker_text_validate_utf8 (text,
		                                MIN (strlen (text), remaining_bytes),
		                                &string,
		                                &written_bytes)) {
			g_string_append_c (string, ' ');
		}

		remaining_bytes -= written_bytes;

		g_debug ("Extracted %" G_GSIZE_FORMAT " bytes from page %d, "
		         "%" G_GSIZE_FORMAT " bytes remaining",
		         written_bytes, i, remaining_bytes);

		g_free (text);
		g_object_unref (page);
	}

	if (elapsed >= EXTRACTION_PROCESS_TIMEOUT) {
		g_debug ("Extraction timed out, %d seconds reached", EXTRACTION_PROCESS_TIMEOUT);
	}

	g_debug ("Content extraction finished: %d/%d pages indexed in %2.2f seconds, "
	         "%" G_GSIZE_FORMAT " bytes extracted",
	         i,
	         n_pages,
	         g_timer_elapsed (timer, NULL),
	         (n_bytes - remaining_bytes));

	g_timer_destroy (timer);

	return g_string_free (string, FALSE);
}

static void
write_pdf_data (PDFData          data,
                TrackerResource *metadata,
                GPtrArray       *keywords)
{
	if (!tracker_is_empty_string (data.title)) {
		tracker_resource_set_string (metadata, "nie:title", data.title);
	}

	if (!tracker_is_empty_string (data.subject)) {
		tracker_resource_set_string (metadata, "nie:subject", data.subject);
	}

	if (!tracker_is_empty_string (data.author)) {
		TrackerResource *author = tracker_extract_new_contact (data.author);
		tracker_resource_set_relation (metadata, "nco:creator", author);
		g_object_unref (author);
	}

	if (!tracker_is_empty_string (data.date)) {
		tracker_resource_set_string (metadata, "nie:contentCreated", data.date);
	}

	if (!tracker_is_empty_string (data.keywords)) {
		tracker_keywords_parse (keywords, data.keywords);
	}
}

G_MODULE_EXPORT gboolean
tracker_extract_get_metadata (TrackerExtractInfo  *info,
                              GError             **error)
{
	TrackerConfig *config;
	time_t creation_date;
	GError *inner_error = NULL;
	TrackerResource *metadata;
	TrackerXmpData *xd = NULL;
	PDFData pd = { 0 }; /* actual data */
	PDFData md = { 0 }; /* for merging */
	G_GNUC_UNUSED GBytes *bytes;
	PopplerDocument *document;
	gchar *xml = NULL;
	gchar *content, *uri;
	guint n_bytes;
	GPtrArray *keywords;
	guint i;
	GFile *file;
	gchar *filename, *resource_uri;
	int fd;
	gchar *contents = NULL;
	gsize len;
	struct stat st;

	file = tracker_extract_info_get_file (info);
	filename = g_file_get_path (file);

	fd = tracker_file_open_fd (filename);

	if (fd == -1) {
		g_warning ("Could not open pdf file '%s': %s\n",
		           filename,
		           g_strerror (errno));
		g_free (filename);
		return FALSE;
	}

	if (fstat (fd, &st) == -1) {
		g_warning ("Could not fstat pdf file '%s': %s\n",
		           filename,
		           g_strerror (errno));
		close (fd);
		g_free (filename);
		return FALSE;
	}

	if (st.st_size == 0) {
		contents = NULL;
		len = 0;
	} else {
		contents = (gchar *) mmap (NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
		if (contents == NULL || contents == MAP_FAILED) {
			g_warning ("Could not mmap pdf file '%s': %s\n",
			           filename,
			           g_strerror (errno));
			close (fd);
			g_free (filename);
			return FALSE;
		}
		len = st.st_size;
	}

	g_free (filename);
	uri = g_file_get_uri (file);

#if POPPLER_CHECK_VERSION (0, 82, 0)
	bytes = g_bytes_new (contents, len);
	document = poppler_document_new_from_bytes (bytes, NULL, &inner_error);
	g_bytes_unref (bytes);
#else
	document = poppler_document_new_from_data (contents, len, NULL, &inner_error);
#endif

	if (inner_error) {
		if (inner_error->code == POPPLER_ERROR_ENCRYPTED) {
			resource_uri = tracker_file_get_content_identifier (file, NULL, NULL);
			metadata = tracker_resource_new (resource_uri);
			g_free (resource_uri);

			tracker_resource_add_uri (metadata, "rdf:type", "nfo:PaginatedTextDocument");
			tracker_resource_set_boolean (metadata, "nfo:isContentEncrypted", TRUE);

			tracker_extract_info_set_resource (info, metadata);
			g_object_unref (metadata);

			g_error_free (inner_error);
			g_free (uri);
			close (fd);

			return TRUE;
		} else {
			g_propagate_prefixed_error (error, inner_error, "Couldn't open PopplerDocument:");
			g_free (uri);
			close (fd);

			return FALSE;
		}
	}

	if (!document) {
		g_warning ("Could not create PopplerDocument from uri:'%s', "
		           "NULL returned without an error",
		           uri);
		g_free (uri);
		close (fd);
		return FALSE;
	}

	resource_uri = tracker_file_get_content_identifier (file, NULL, NULL);
	metadata = tracker_resource_new (resource_uri);
	tracker_resource_add_uri (metadata, "rdf:type", "nfo:PaginatedTextDocument");
	g_free (resource_uri);

	g_object_get (document,
	              "title", &pd.title,
	              "author", &pd.author,
	              "subject", &pd.subject,
	              "keywords", &pd.keywords,
	              "metadata", &xml,
	              NULL);

	creation_date = poppler_document_get_creation_date (document);

	if (creation_date > 0) {
		pd.creation_date = tracker_date_to_string (creation_date);
	}

	keywords = g_ptr_array_new_with_free_func ((GDestroyNotify) g_free);

	if (xml && *xml) {
		xd = tracker_xmp_new (xml, strlen (xml), uri);
	} else {
		gchar *sidecar = NULL;

		xd = tracker_xmp_new_from_sidecar (file, &sidecar);

		if (sidecar) {
			TrackerResource *sidecar_resource;

			sidecar_resource = tracker_resource_new (sidecar);
			tracker_resource_add_uri (sidecar_resource, "rdf:type", "nfo:FileDataObject");
			tracker_resource_add_relation (sidecar_resource, "nie:interpretedAs", metadata);

			tracker_resource_add_take_relation (metadata, "nie:isStoredAs", sidecar_resource);
		}
	}

	if (xd) {
		/* The casts here are well understood and known */
		md.title = (gchar *) tracker_coalesce_strip (4, pd.title, xd->title, xd->title2, xd->pdf_title);
		md.subject = (gchar *) tracker_coalesce_strip (2, pd.subject, xd->subject);
		md.date = (gchar *) tracker_coalesce_strip (3, pd.creation_date, xd->date, xd->time_original);
		md.author = (gchar *) tracker_coalesce_strip (2, pd.author, xd->creator);

		write_pdf_data (md, metadata, keywords);

		if (xd->keywords) {
			tracker_keywords_parse (keywords, xd->keywords);
		}

		if (xd->pdf_keywords) {
			tracker_keywords_parse (keywords, xd->pdf_keywords);
		}

		if (xd->publisher) {
			TrackerResource *publisher = tracker_extract_new_contact (xd->publisher);
			tracker_resource_set_relation (metadata, "nco:publisher", publisher);
			g_object_unref (publisher);
		}

		if (xd->type) {
			tracker_resource_set_string (metadata, "dc:type", xd->type);
		}

		if (xd->format) {
			tracker_resource_set_string (metadata, "dc:format", xd->format);
		}

		if (xd->identifier) {
			tracker_resource_set_string (metadata, "dc:identifier", xd->identifier);
		}

		if (xd->source) {
			tracker_resource_set_string (metadata, "dc:source", xd->source);
		}

		if (xd->language) {
			tracker_resource_set_string (metadata, "dc:language", xd->language);
		}

		if (xd->relation) {
			tracker_resource_set_string (metadata, "dc:relation", xd->relation);
		}

		if (xd->coverage) {
			tracker_resource_set_string (metadata, "dc:coverage", xd->coverage);
		}

		if (xd->license) {
			tracker_resource_set_string (metadata, "nie:license", xd->license);
		}

		if (xd->make || xd->model) {
			TrackerResource *equipment = tracker_extract_new_equipment (xd->make, xd->model);
			tracker_resource_set_relation (metadata, "nfo:equipment", equipment);
			g_object_unref (equipment);
		}

		if (xd->rights) {
			tracker_resource_set_string (metadata, "nie:copyright", xd->rights);
		}

		/* Question: Shouldn't xd->Artist be merged with md.author instead? */

		if (xd->artist || xd->contributor) {
			TrackerResource *artist;
			const gchar *artist_name;

			artist_name = tracker_coalesce_strip (2, xd->artist, xd->contributor);

			artist = tracker_extract_new_contact (artist_name);

			tracker_resource_set_relation (metadata, "nco:contributor", artist);

			g_object_unref (artist);
		}

		if (xd->description) {
			tracker_resource_set_string (metadata, "nie:description", xd->description);
		}

		if (xd->address || xd->state || xd->country || xd->city ||
		    xd->gps_altitude || xd->gps_latitude || xd-> gps_longitude) {

			TrackerResource *location = tracker_extract_new_location (xd->address,
			        xd->state, xd->city, xd->country, xd->gps_altitude,
			        xd->gps_latitude, xd->gps_longitude);

			tracker_resource_set_relation (metadata, "slo:location", location);

			g_object_unref (location);
		}

		if (xd->regions) {
			tracker_xmp_apply_regions_to_resource (metadata, xd);
		}

		tracker_xmp_free (xd);
	} else {
		/* So if we are here we have NO XMP data and we just
		 * write what we know from Poppler.
		 */
		write_pdf_data (pd, metadata, keywords);
	}

	for (i = 0; i < keywords->len; i++) {
		TrackerResource *tag;
		const gchar *p;

		p = g_ptr_array_index (keywords, i);
		tag = tracker_extract_new_tag (p);

		tracker_resource_add_relation (metadata, "nao:hasTag", tag);

		g_object_unref (tag);
	}
	g_ptr_array_free (keywords, TRUE);

	tracker_resource_set_int64 (metadata, "nfo:pageCount", poppler_document_get_n_pages(document));

	config = tracker_main_get_config ();
	n_bytes = tracker_config_get_max_bytes (config);
	content = extract_content_text (document, n_bytes);

	if (content) {
		tracker_resource_set_string (metadata, "nie:plainTextContent", content);
		g_free (content);
	}

	read_outline (document, metadata);

	g_free (xml);
	g_free (pd.keywords);
	g_free (pd.title);
	g_free (pd.subject);
	g_free (pd.creation_date);
	g_free (pd.author);
	g_free (pd.date);
	g_free (uri);

	g_object_unref (document);

	if (contents) {
		munmap (contents, len);
	}

	close (fd);

	tracker_extract_info_set_resource (info, metadata);
	g_object_unref (metadata);

	return TRUE;
}
