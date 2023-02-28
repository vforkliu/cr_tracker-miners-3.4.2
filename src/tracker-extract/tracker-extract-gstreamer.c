/*
 * Copyright (C) 2006, Laurent Aguerreche <laurent.aguerreche@free.fr>
 * Copyright (C) 2007, Jamie McCracken <jamiemcc@gnome.org>
 * Copyright (C) 2008, Nokia <ivan.frade@nokia.com>
 * Copyright (C) 2016, Sam Thursfield <sam@afuera.me.uk>
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

/* Ensure we have a valid backend enabled */
#if !defined(GSTREAMER_BACKEND_DISCOVERER) && \
    !defined(GSTREAMER_BACKEND_GUPNP_DLNA)
#error Not a valid GStreamer backend defined
#endif

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include <glib.h>
#include <glib/gstdio.h>

#if defined(GSTREAMER_BACKEND_DISCOVERER) || \
    defined(GSTREAMER_BACKEND_GUPNP_DLNA)
#define GST_USE_UNSTABLE_API
#include <gst/pbutils/pbutils.h>
#endif

#if defined(GSTREAMER_BACKEND_GUPNP_DLNA)
#include <libgupnp-dlna/gupnp-dlna.h>
#include <libgupnp-dlna/gupnp-dlna-gst-utils.h>
#endif

#include <gst/gst.h>
#include <gst/tag/tag.h>

#include <libtracker-miners-common/tracker-common.h>
#include <libtracker-extract/tracker-extract.h>

#include "tracker-cue-sheet.h"
#include "tracker-main.h"

typedef enum {
	EXTRACT_MIME_AUDIO,
	EXTRACT_MIME_VIDEO,
	EXTRACT_MIME_IMAGE,
	EXTRACT_MIME_GUESS
} ExtractMime;

typedef struct {
	ExtractMime     mime;
	GstTagList     *tagcache;
	GstToc         *gst_toc;
	TrackerToc     *toc;
	gboolean        is_content_encrypted;

	GSList         *artist_list;

	GstSample      *sample;
	GstMapInfo      info;

#if defined(GSTREAMER_BACKEND_DISCOVERER) || \
    defined(GSTREAMER_BACKEND_GUPNP_DLNA)
	gboolean        has_image;
	gboolean        has_audio;
	gboolean        has_video;
	GList          *streams;
#endif

#if defined(GSTREAMER_BACKEND_DISCOVERER) || \
    defined(GSTREAMER_BACKEND_GUPNP_DLNA)
	GstDiscoverer  *discoverer;
#endif

#if defined(GSTREAMER_BACKEND_GUPNP_DLNA)
	const gchar          *dlna_profile;
	const gchar          *dlna_mime;
#endif

#if defined(GSTREAMER_BACKEND_DISCOVERER) || \
    defined(GSTREAMER_BACKEND_GUPNP_DLNA)
	gint64          duration;
	gint            audio_channels;
	gint            audio_samplerate;
	gint            height;
	gint            width;
	gfloat          aspect_ratio;
	gfloat          video_fps;
#endif
} MetadataExtractor;

static TrackerSparqlConnection *local_conn = NULL;

static void common_extract_stream_metadata (MetadataExtractor    *extractor,
                                            const gchar          *uri,
                                            TrackerResource      *resource);

static TrackerResource *
intern_artist (MetadataExtractor     *extractor,
               const gchar           *artist_name)
{
	GSList *node;
	TrackerResource *artist;
	gchar *artist_uri;

	if (artist_name == NULL)
		return NULL;

	artist_uri = tracker_sparql_escape_uri_printf ("urn:artist:%s", artist_name);

	node = g_slist_find_custom (extractor->artist_list, artist_uri,
	                            (GCompareFunc) tracker_resource_identifier_compare_func);
	if (node) {
		g_free (artist_uri);
		return node->data;
	}

	artist = tracker_extract_new_artist (artist_name);
	g_free (artist_uri);

	extractor->artist_list = g_slist_prepend (extractor->artist_list, artist);

	return artist;
}

static void
set_property_from_gst_tag (TrackerResource *resource,
                           const gchar     *property_uri,
                           GstTagList      *tag_list,
                           const gchar     *tag)
{
	GValue value = G_VALUE_INIT;

	if (gst_tag_list_copy_value (&value, tag_list, tag)) {
		tracker_resource_set_gvalue (resource, property_uri, &value);
		g_value_unset (&value);
	}
}

static inline gboolean
get_gst_date_time_to_buf (GstDateTime *date_time,
                          gchar       *buf,
                          size_t       size)
{
	const gchar *offset_str;
	gint year, month, day, hour, minute, second;
	gfloat offset;
	gboolean complete;

	offset_str = "+";
	year = hour = minute = second = 0;
	month = day = 1;
	offset = 0.0;
	complete = TRUE;

	if (gst_date_time_has_year (date_time)) {
		year = gst_date_time_get_year (date_time);
	} else {
		complete = FALSE;
	}

	if (gst_date_time_has_month (date_time)) {
		month = gst_date_time_get_month (date_time);
	} else {
		complete = FALSE;
	}

	if (gst_date_time_has_day (date_time)) {
		day = gst_date_time_get_day (date_time);
	} else {
		complete = FALSE;
	}

	/* Hour and Minute data is retrieved by first checking the
	 * _has_time() API.
	 */

	if (gst_date_time_has_second (date_time)) {
		second = gst_date_time_get_second (date_time);
	} else {
		complete = FALSE;
	}

	if (gst_date_time_has_time (date_time)) {
		hour = gst_date_time_get_hour (date_time);
		minute = gst_date_time_get_minute (date_time);
		offset_str = gst_date_time_get_time_zone_offset (date_time) >= 0 ? "+" : "-";
		offset = gst_date_time_get_time_zone_offset (date_time);
	} else {
		offset_str = "+";
		complete = FALSE;
	}

	snprintf (buf, size, "%04d-%02d-%02dT%02d:%02d:%02d%s%02d:00",
	          year,
	          month,
	          day,
	          hour,
	          minute,
	          second,
	          offset_str,
	          (gint) ABS (offset));

	return complete;
}

static gboolean
extract_gst_date_time (gchar       *buf,
                       size_t       size,
                       GstTagList  *tag_list,
                       const gchar *tag_date_time,
                       const gchar *tag_date)
{
	GstDateTime *date_time = NULL;
	GDate *date = NULL;
	gboolean ret = FALSE;

	buf[0] = '\0';

	if (gst_tag_list_get_date_time (tag_list, tag_date_time, &date_time)) {
		gboolean complete;

		ret = gst_date_time_has_year (date_time);
		complete = get_gst_date_time_to_buf (date_time, buf, size);
		gst_date_time_unref (date_time);

		if (!complete) {
			g_debug ("GstDateTime was not complete, parts of the date/time were missing (e.g. hours, minutes, seconds)");
		}
	} else if (gst_tag_list_get_date (tag_list, tag_date, &date)) {
		if (date && g_date_valid (date)) {
			if (date->julian)
				ret = g_date_valid_julian (date->julian_days);
			if (date->dmy)
				ret = g_date_valid_dmy (date->day, date->month, date->year);
		}

		if (ret) {
			/* GDate does not carry time zone information, assume UTC */
			g_date_strftime (buf, size, "%Y-%m-%dT%H:%M:%SZ", date);
		}
	}

	if (date) {
		g_date_free (date);
	}

	return ret;
}

static void
add_date_time_gst_tag_with_mtime_fallback (TrackerResource *resource,
                                           const gchar     *uri,
                                           const gchar     *key,
                                           GstTagList      *tag_list,
                                           const gchar     *tag_date_time,
                                           const gchar     *tag_date)
{
	gchar buf[26];

	extract_gst_date_time (buf, sizeof (buf), tag_list, tag_date_time, tag_date);

	tracker_guarantee_resource_date_from_file_mtime (resource, key, buf, uri);
}

static void
set_keywords_from_gst_tag (TrackerResource *resource,
                           GstTagList      *tag_list)
{
	gboolean ret;
	gchar *str;

	ret = gst_tag_list_get_string (tag_list, GST_TAG_KEYWORDS, &str);

	if (ret) {
		GStrv keywords;
		gint i = 0;

		keywords = g_strsplit_set (str, " ,", -1);

		while (keywords[i]) {
			tracker_resource_add_string (resource, "nie:keyword", g_strstrip (keywords[i]));
			i++;
		}

		g_strfreev (keywords);
		g_free (str);
	}
}

static gchar *
get_embedded_cue_sheet_data (GstTagList *tag_list)
{
	gint i, count;
	gchar *buffer = NULL;

	count = gst_tag_list_get_tag_size (tag_list, GST_TAG_EXTENDED_COMMENT);
	for (i = 0; i < count; i++) {
		gst_tag_list_get_string_index (tag_list, GST_TAG_EXTENDED_COMMENT, i, &buffer);

		if (g_ascii_strncasecmp (buffer, "cuesheet=", 9) == 0) {
			/* Use same functionality as g_strchug() here
			 * for cuesheet, to avoid allocating new
			 * memory but also to return the string and
			 * not have to jump past cuesheet= on the
			 * returned value.
			 */
			memmove (buffer, buffer + 9, strlen ((gchar *) buffer + 9) + 1);

			return buffer;
		}

		g_free (buffer);
	}

	return NULL;
}

/* Utility function to convert from GstToc, as returned by
 * gst_discoverer_info_get_toc(), to TrackerToc.
 */
static TrackerToc *
translate_discoverer_toc (GstToc *gst_toc)
{
	const GList *entries, *l;
	TrackerToc *toc;
	gint i = 0;

	entries = gst_toc_get_entries (gst_toc);
	if (!entries)
		return NULL;

	toc = tracker_toc_new ();

	for (l = entries; l; l = l->next) {
		GstTocEntry *entry = l->data;
		GstTagList *tags, *copy = NULL;
		gint64 start, stop;

		tags = gst_toc_entry_get_tags (entry);

		if (tags) {
			copy = gst_tag_list_copy (tags);
		} else {
			copy = gst_tag_list_new_empty ();
		}

		if (gst_tag_list_get_tag_size (copy, GST_TAG_TRACK_NUMBER) == 0) {
			gst_tag_list_add (copy, GST_TAG_MERGE_REPLACE,
			                  GST_TAG_TRACK_NUMBER, i + 1,
			                  NULL);
		}

		gst_toc_entry_get_start_stop_times (entry, &start, &stop);
		tracker_toc_add_entry (toc, copy, (gdouble) start / GST_SECOND,
		                       (gdouble) (stop - start) / GST_SECOND);
		gst_tag_list_unref (copy);
		i++;
	}

	return toc;
}

static TrackerResource *
extractor_get_geolocation (MetadataExtractor     *extractor,
                           GstTagList            *tag_list)
{
	TrackerResource *location = NULL;
	gdouble lat, lon, alt;
	gboolean has_coords;

	g_debug ("Retrieving geolocation metadata...");

	has_coords = (gst_tag_list_get_double (tag_list, GST_TAG_GEO_LOCATION_LATITUDE, &lat) &&
	              gst_tag_list_get_double (tag_list, GST_TAG_GEO_LOCATION_LONGITUDE, &lon) &&
	              gst_tag_list_get_double (tag_list, GST_TAG_GEO_LOCATION_ELEVATION, &alt));

	if (has_coords) {
		location = tracker_resource_new (NULL);
		tracker_resource_set_uri (location, "rdf:type", "slo:GeoLocation");

		tracker_resource_set_double (location, "slo:latitude", lat);
		tracker_resource_set_double (location, "slo:longitude", lon);
		tracker_resource_set_double (location, "slo:altitude", alt);
	}

	return location;
}

static TrackerResource *
extractor_get_address (MetadataExtractor     *extractor,
                       GstTagList            *tag_list)
{
	TrackerResource *address = NULL;
	gchar *country = NULL, *city = NULL, *sublocation = NULL;

	g_debug ("Retrieving address metadata...");

	gst_tag_list_get_string (tag_list, GST_TAG_GEO_LOCATION_CITY, &city);
	gst_tag_list_get_string (tag_list, GST_TAG_GEO_LOCATION_COUNTRY, &country);
	gst_tag_list_get_string (tag_list, GST_TAG_GEO_LOCATION_SUBLOCATION, &sublocation);

	if (city || country || sublocation) {
		gchar *address_uri = NULL;

		address_uri = tracker_sparql_get_uuid_urn ();
		address = tracker_resource_new (address_uri);

		tracker_resource_set_uri (address, "rdf:type", "nco:PostalAddress");

		if (sublocation) {
			tracker_resource_set_string (address, "nco:region", sublocation);
		}

		if (city) {
			tracker_resource_set_string (address, "nco:locality", city);
		}

		if (country) {
			tracker_resource_set_string (address, "nco:country", country);
		}
	}

	return address;
}

static void
extractor_guess_content_type (MetadataExtractor *extractor)
{
	if (extractor->has_video) {
		extractor->mime = EXTRACT_MIME_VIDEO;
	} else if (extractor->has_audio) {
		extractor->mime = EXTRACT_MIME_AUDIO;
	} else if (extractor->has_image) {
		extractor->mime = EXTRACT_MIME_IMAGE;
	} else {
		/* default to video */
		extractor->mime = EXTRACT_MIME_VIDEO;
	}
}

static void
extractor_apply_general_metadata (MetadataExtractor     *extractor,
                                  GstTagList            *tag_list,
                                  const gchar           *file_url,
                                  TrackerResource       *resource,
                                  TrackerResource      **p_artist,
                                  TrackerResource      **p_performer,
                                  TrackerResource      **p_composer)
{
	gchar *artist_name = NULL;
	gchar *performer_name = NULL;
	gchar *composer_name = NULL;
	gchar *genre = NULL;
	gchar *title = NULL;
	gchar *title_guaranteed = NULL;

	*p_artist = NULL;
	*p_composer = NULL;
	*p_performer = NULL;

	gst_tag_list_get_string (tag_list, GST_TAG_PERFORMER, &performer_name);
	gst_tag_list_get_string (tag_list, GST_TAG_ARTIST, &artist_name);
	gst_tag_list_get_string (tag_list, GST_TAG_COMPOSER, &composer_name);

	if (performer_name != NULL) {
		*p_performer = intern_artist (extractor, performer_name);
	}

	if (artist_name != NULL) {
		*p_artist = intern_artist (extractor, artist_name);
	}

	if (composer_name != NULL) {
		*p_composer = intern_artist (extractor, composer_name);
	}

	gst_tag_list_get_string (tag_list, GST_TAG_GENRE, &genre);
	gst_tag_list_get_string (tag_list, GST_TAG_TITLE, &title);

	if (genre && g_strcmp0 (genre, "Unknown") != 0) {
		tracker_resource_set_string (resource, "nfo:genre", genre);
	}

	tracker_guarantee_resource_title_from_file (resource,
	                                           "nie:title",
	                                            title,
	                                            file_url,
	                                            &title_guaranteed);

	add_date_time_gst_tag_with_mtime_fallback (resource,
	                                           file_url,
	                                           "nie:contentCreated",
	                                           tag_list,
	                                           GST_TAG_DATE_TIME,
	                                           GST_TAG_DATE);

	set_property_from_gst_tag (resource, "nie:copyright", tag_list, GST_TAG_COPYRIGHT);
	set_property_from_gst_tag (resource, "nie:license", tag_list, GST_TAG_LICENSE);
	set_property_from_gst_tag (resource, "dc:coverage", tag_list, GST_TAG_LOCATION);
	set_property_from_gst_tag (resource, "nie:comment", tag_list, GST_TAG_COMMENT);
	set_property_from_gst_tag (resource, "nie:generator", tag_list, GST_TAG_ENCODER);

	g_free (title_guaranteed);
	g_free (performer_name);
	g_free (artist_name);
	g_free (composer_name);
	g_free (genre);
	g_free (title);
}

static TrackerResource *
extractor_maybe_get_album_disc (MetadataExtractor *extractor,
                                GstTagList        *tag_list)
{
	TrackerResource *album = NULL, *album_artist = NULL, *album_disc = NULL;
	gchar *album_artist_name = NULL;
	gchar *album_title = NULL;
	gchar *track_artist_temp = NULL;
	gboolean has_it, has_date;
	guint volume_number;
	gchar album_datetime[26];
	gchar *mb_release_id = NULL;

	#if defined GST_TAG_MUSICBRAINZ_RELEASEGROUPID
		gchar *mb_release_group_id = NULL;
	#endif

	gst_tag_list_get_string (tag_list, GST_TAG_ALBUM, &album_title);

	if (!album_title)
		return NULL;

	gst_tag_list_get_string (tag_list, GST_TAG_ALBUM_ARTIST, &album_artist_name);
	gst_tag_list_get_string (tag_list, GST_TAG_ARTIST, &track_artist_temp);

	has_date = extract_gst_date_time (album_datetime, sizeof (album_datetime),
					  tag_list, GST_TAG_DATE_TIME, GST_TAG_DATE);

	album_artist = intern_artist (extractor, album_artist_name);
	has_it = gst_tag_list_get_uint (tag_list, GST_TAG_ALBUM_VOLUME_NUMBER, &volume_number);

	album_disc = tracker_extract_new_music_album_disc (album_title,
	                                                   album_artist,
	                                                   has_it ? volume_number : 1,
	                                                   has_date ? album_datetime : NULL);

	album = tracker_resource_get_first_relation (album_disc, "nmm:albumDiscAlbum");
	set_property_from_gst_tag (album, "nmm:albumTrackCount", tag_list, GST_TAG_TRACK_COUNT);
	set_property_from_gst_tag (album, "nmm:albumGain", extractor->tagcache, GST_TAG_ALBUM_GAIN);
	set_property_from_gst_tag (album, "nmm:albumPeakGain", extractor->tagcache, GST_TAG_ALBUM_PEAK);

	gst_tag_list_get_string (tag_list, GST_TAG_MUSICBRAINZ_ALBUMID, &mb_release_id);
	if (mb_release_id) {
		g_autofree char *mb_release_uri = g_strdup_printf("https://musicbrainz.org/release/%s", mb_release_id);
		TrackerResource *mb_release = tracker_extract_new_external_reference("https://musicbrainz.org/doc/Release",
		                                                                     mb_release_id,
		                                                                     mb_release_uri);

		tracker_resource_add_take_relation (album, "tracker:hasExternalReference", mb_release);
		g_free (mb_release_id);
	}

	#if defined GST_TAG_MUSICBRAINZ_RELEASEGROUPID
		gst_tag_list_get_string (tag_list, GST_TAG_MUSICBRAINZ_RELEASEGROUPID, &mb_release_group_id);
		if (mb_release_group_id) {
			g_autofree char *mb_release_group_uri = g_strdup_printf("https://musicbrainz.org/release-group/%s", mb_release_group_id);
			TrackerResource *mb_release_group = tracker_extract_new_external_reference("https://musicbrainz.org/doc/Release_Group",
			                                                                           mb_release_group_id,
			                                                                           mb_release_group_uri);

			tracker_resource_add_take_relation (album, "tracker:hasExternalReference", mb_release_group);
			g_free (mb_release_group_id);
		}
	#endif

	g_free (album_artist_name);
	g_free (track_artist_temp);
	g_free (album_title);

	return album_disc;
}

static TrackerResource *
extractor_get_equipment (MetadataExtractor    *extractor,
                         GstTagList           *tag_list)
{
	TrackerResource *equipment;
	gchar *model = NULL, *manuf = NULL;

	gst_tag_list_get_string (tag_list, GST_TAG_DEVICE_MODEL, &model);
	gst_tag_list_get_string (tag_list, GST_TAG_DEVICE_MANUFACTURER, &manuf);

	if (model == NULL && manuf == NULL)
		return NULL;

	equipment = tracker_extract_new_equipment (manuf, model);

	g_free (model);
	g_free (manuf);

	return equipment;
}

static TrackerResource *
ensure_file_resource (TrackerResource *resource,
                      const gchar     *file_url)
{
	TrackerResource *file_resource;

	file_resource = tracker_resource_get_first_relation (resource, "nie:isStoredAs");
	if (!file_resource) {
		file_resource = tracker_resource_new (file_url);
		tracker_resource_set_take_relation (resource, "nie:isStoredAs", file_resource);
	}

	return file_resource;
}

static void
extractor_apply_audio_metadata (MetadataExtractor     *extractor,
                                GstTagList            *tag_list,
                                const gchar           *file_url,
                                TrackerResource       *audio,
                                TrackerResource       *artist,
                                TrackerResource       *performer,
                                TrackerResource       *composer,
                                TrackerResource       *album_disc)
{
	gchar *mb_recording_id = NULL;

	#if defined GST_TAG_MUSICBRAINZ_RELEASETRACKID
		gchar *mb_track_id = NULL;
	#endif

	#if defined GST_TAG_ACOUSTID_FINGERPRINT
		GValue acoustid_fingerprint = G_VALUE_INIT;
		gboolean have_acoustid_fingerprint;
	#endif

	set_property_from_gst_tag (audio, "nmm:trackNumber", tag_list, GST_TAG_TRACK_NUMBER);
	set_property_from_gst_tag (audio, "nfo:codec", tag_list, GST_TAG_AUDIO_CODEC);
	set_property_from_gst_tag (audio, "nfo:gain", tag_list, GST_TAG_TRACK_GAIN);
	set_property_from_gst_tag (audio, "nfo:peakGain", tag_list, GST_TAG_TRACK_PEAK);

	gst_tag_list_get_string (tag_list, GST_TAG_MUSICBRAINZ_TRACKID, &mb_recording_id);
	if (mb_recording_id) {
		g_autofree char *mb_recording_uri = g_strdup_printf("https://musicbrainz.org/recording/%s", mb_recording_id);
		TrackerResource *mb_recording = tracker_extract_new_external_reference("https://musicbrainz.org/doc/Recording",
		                                                                       mb_recording_id,
		                                                                       mb_recording_uri);

		tracker_resource_add_take_relation (audio, "tracker:hasExternalReference", mb_recording);
		g_free (mb_recording_id);
	}

	#if defined GST_TAG_MUSICBRAINZ_RELEASETRACKID
		gst_tag_list_get_string (tag_list, GST_TAG_MUSICBRAINZ_RELEASETRACKID, &mb_track_id);
		if (mb_track_id) {
			g_autofree char *mb_track_uri = g_strdup_printf("https://musicbrainz.org/track/%s", mb_track_id);
			TrackerResource *mb_track = tracker_extract_new_external_reference("https://musicbrainz.org/doc/Track",
			                                                                   mb_track_id,
			                                                                   mb_track_uri);

			tracker_resource_add_take_relation (audio, "tracker:hasExternalReference", mb_track);
			g_free (mb_track_id);
		}
	#endif

	#if defined GST_TAG_ACOUSTID_FINGERPRINT
		have_acoustid_fingerprint = gst_tag_list_copy_value (&acoustid_fingerprint, tag_list, GST_TAG_ACOUSTID_FINGERPRINT);
		if (have_acoustid_fingerprint) {
			TrackerResource *hash_resource = tracker_resource_new (NULL);
			TrackerResource *file_resource = ensure_file_resource (audio, file_url);

			tracker_resource_set_uri (hash_resource, "rdf:type", "nfo:FileHash");
			tracker_resource_set_gvalue (hash_resource, "nfo:hashValue", &acoustid_fingerprint);
			tracker_resource_set_string (hash_resource, "nfo:hashAlgorithm", "chromaprint");

			tracker_resource_add_take_relation (file_resource, "nfo:hasHash", hash_resource);
			g_value_unset (&acoustid_fingerprint);
		}
	#endif

	if (artist) {
		gchar *mb_artist_id = NULL;

		tracker_resource_set_relation (audio, "nmm:artist", artist);

		gst_tag_list_get_string (tag_list, GST_TAG_MUSICBRAINZ_ARTISTID, &mb_artist_id);
		if (mb_artist_id) {
			g_autofree char *mb_artist_uri = g_strdup_printf("https://musicbrainz.org/artist/%s", mb_artist_id);
			TrackerResource *mb_artist = tracker_extract_new_external_reference("https://musicbrainz.org/doc/Artist",
			                                                                    mb_artist_id,
			                                                                    mb_artist_uri);

			tracker_resource_add_take_relation (artist, "tracker:hasExternalReference", mb_artist);
			g_free (mb_artist_id);
		}
	}

	if (performer) {
		tracker_resource_set_relation (audio, "nmm:performer", performer);
	}

	if (composer) {
		tracker_resource_set_relation (audio, "nmm:composer", composer);
	}

	if (album_disc) {
		TrackerResource *album;
		album = tracker_resource_get_first_relation (album_disc, "nmm:albumDiscAlbum");

		tracker_resource_set_relation (audio, "nmm:musicAlbumDisc", album_disc);
		tracker_resource_set_relation (audio, "nmm:musicAlbum", album);
	}
}

static void
extractor_apply_video_metadata (MetadataExtractor *extractor,
                                GstTagList        *tag_list,
                                TrackerResource   *video,
                                TrackerResource   *performer,
                                TrackerResource   *composer)
{
	if (performer) {
		tracker_resource_set_relation (video, "nmm:leadActor", performer);
	}

	if (composer) {
		tracker_resource_set_relation (video, "nmm:director", composer);
	}

	set_keywords_from_gst_tag (video, tag_list);
}

static void
extract_track (TrackerResource      *track,
               MetadataExtractor    *extractor,
               TrackerTocEntry      *toc_entry,
               const gchar          *file_url,
               TrackerResource      *album_disc)
{
	TrackerResource *track_artist = NULL;
	TrackerResource *track_performer = NULL;
	TrackerResource *track_composer = NULL;

	tracker_resource_add_uri (track, "rdf:type", "nmm:MusicPiece");
	tracker_resource_add_uri (track, "rdf:type", "nfo:Audio");

	extractor_apply_general_metadata (extractor,
	                                  toc_entry->tag_list,
	                                  file_url,
	                                  track,
	                                  &track_artist,
	                                  &track_performer,
	                                  &track_composer);

	extractor_apply_audio_metadata (extractor,
	                                toc_entry->tag_list,
	                                file_url,
	                                track,
	                                track_artist,
	                                track_performer,
	                                track_composer,
	                                album_disc);

	if (toc_entry->duration > 0) {
		tracker_resource_set_int64 (track, "nfo:duration", (gint64)toc_entry->duration);
	} else if (extractor->toc->entry_list &&
	           toc_entry == g_list_last (extractor->toc->entry_list)->data) {
		/* The last element may not have a duration, because it depends
		 * on the duration of the media file rather than info from the
		 * cue sheet. In this case figure the data out from the total
		 * duration.
		 */
		tracker_resource_set_int64 (track, "nfo:duration", (gint64)extractor->duration - toc_entry->start);
	}

	tracker_resource_set_double (track, "nfo:audioOffset", toc_entry->start);
}

#define CHUNK_N_BYTES (2 << 15)

static guint64
extract_gibest_hash (GFile *file)
{
	guint64 buffer[2][CHUNK_N_BYTES/8];
	GInputStream *stream = NULL;
	gssize n_bytes, file_size;
	GError *error = NULL;
	guint64 hash = 0;
	gint i;

	stream = G_INPUT_STREAM (g_file_read (file, NULL, &error));
	if (stream == NULL)
		goto fail;

	/* Extract start/end chunks of the file */
	n_bytes = g_input_stream_read (stream, buffer[0], CHUNK_N_BYTES, NULL, &error);
	if (n_bytes == -1)
		goto fail;

	if (!g_seekable_seek (G_SEEKABLE (stream), -CHUNK_N_BYTES, G_SEEK_END, NULL, &error))
		goto fail;

	n_bytes = g_input_stream_read (stream, buffer[1], CHUNK_N_BYTES, NULL, &error);
	if (n_bytes == -1)
		goto fail;

	for (i = 0; i < G_N_ELEMENTS (buffer[0]); i++)
		hash += buffer[0][i] + buffer[1][i];

	file_size = g_seekable_tell (G_SEEKABLE (stream));

	if (file_size < CHUNK_N_BYTES)
		goto end;

	/* Include file size */
	hash += file_size;
	g_object_unref (stream);

	return hash;

fail:
	g_warning ("Could not get file hash: %s\n", error ? error->message : "Unknown error");
	g_clear_error (&error);

end:
	g_clear_object (&stream);
	return 0;
}

static TrackerResource *
extract_metadata (MetadataExtractor      *extractor,
                  const gchar            *file_url)
{
	TrackerResource *resource;
	gchar *resource_uri;
	GFile *file;

	g_return_val_if_fail (extractor != NULL, NULL);

	file = g_file_new_for_uri (file_url);
	resource_uri = tracker_file_get_content_identifier (file, NULL, "1");
	resource = tracker_resource_new (resource_uri);
	g_free (resource_uri);

	if (extractor->toc) {
		gst_tag_list_insert (extractor->tagcache,
		                     extractor->toc->tag_list,
		                     GST_TAG_MERGE_KEEP);

		if (g_list_length (extractor->toc->entry_list) == 1) {
			/* If we only got one track, stick all the info together and
			 * forget about the table of contents
			 */
			TrackerTocEntry *toc_entry;

			toc_entry = extractor->toc->entry_list->data;
			gst_tag_list_insert (extractor->tagcache,
			                     toc_entry->tag_list,
			                     GST_TAG_MERGE_KEEP);

			tracker_toc_free (extractor->toc);
			extractor->toc = NULL;
		}
	}

	if (extractor->mime == EXTRACT_MIME_GUESS && !gst_tag_list_is_empty (extractor->tagcache)) {
		extractor_guess_content_type (extractor);
	} else {
		/* Rely on the information from the discoverer rather than the
		 * mimetype, this is a safety net for those formats that fool
		 * mimetype sniffing (eg. .ogg suffixed OGG videos being detected
		 * as audio/ogg.
		 */
		if (extractor->mime == EXTRACT_MIME_AUDIO && extractor->has_video) {
			g_debug ("mimetype says its audio, but has video frames. Falling back to video extraction.");
			extractor->mime = EXTRACT_MIME_VIDEO;
		} else if (extractor->mime == EXTRACT_MIME_VIDEO &&
			   !extractor->has_video && extractor->has_audio) {
			g_debug ("mimetype says its video, but has only audio. Falling back to audio extraction.");
			extractor->mime = EXTRACT_MIME_AUDIO;
		}
	}

	if (extractor->mime == EXTRACT_MIME_GUESS) {
		g_warning ("Cannot guess real stream type if no tags were read! "
		           "Defaulting to Video.");
		tracker_resource_add_uri (resource, "rdf:type", "nmm:Video");
	} else {
		if (extractor->mime == EXTRACT_MIME_AUDIO) {
			/* Audio: don't make an nmm:MusicPiece for the file resource if it's
			 * actually a container for an entire album - we will make a
			 * nmm:MusicPiece for each of the tracks inside instead.
			 */
			tracker_resource_add_uri (resource, "rdf:type", "nfo:Audio");

			if (extractor->toc == NULL || extractor->toc->entry_list == NULL)
				tracker_resource_add_uri (resource, "rdf:type", "nmm:MusicPiece");
		} else if (extractor->mime == EXTRACT_MIME_VIDEO) {
			tracker_resource_add_uri (resource, "rdf:type", "nmm:Video");
		} else {
			tracker_resource_add_uri (resource, "rdf:type", "nfo:Image");
			tracker_resource_add_uri (resource, "rdf:type", "nmm:Photo");
		}
	}

	if (!gst_tag_list_is_empty (extractor->tagcache)) {
		GList *node;
		TrackerResource *equipment;
		TrackerResource *geolocation, *address;
		TrackerResource *artist = NULL, *performer = NULL, *composer = NULL;
		TrackerResource *album_disc;

		extractor_apply_general_metadata (extractor,
		                                  extractor->tagcache,
		                                  file_url,
		                                  resource,
		                                  &artist,
		                                  &performer,
		                                  &composer);

		equipment = extractor_get_equipment (extractor, extractor->tagcache);
		if (equipment) {
			tracker_resource_set_relation (resource, "nfo:equipment", equipment);
			g_object_unref (equipment);
		}

		geolocation = extractor_get_geolocation (extractor, extractor->tagcache);
		if (geolocation) {
			address = extractor_get_address (extractor, extractor->tagcache);
			if (address) {
				tracker_resource_set_relation (geolocation, "slo:postalAddress", address);
				g_object_unref (address);
			}

			tracker_resource_set_relation (resource, "slo:location", geolocation);
			g_object_unref (geolocation);
		}

		if (extractor->mime == EXTRACT_MIME_VIDEO) {
			extractor_apply_video_metadata (extractor,
			                                extractor->tagcache,
			                                resource,
			                                performer,
			                                composer);
		}

		if (extractor->mime == EXTRACT_MIME_AUDIO) {
			album_disc = extractor_maybe_get_album_disc (extractor, extractor->tagcache);

			/* If the audio file contains multiple tracks, we create the tracks
			 * as abstract information element types and relate them to the
			 * concrete nfo:FileDataObject using nie:isStoredAs.
			 */
			if (extractor->toc && g_list_length (extractor->toc->entry_list) > 1) {
				TrackerResource *file_resource;

				file_resource = ensure_file_resource (resource, file_url);

				for (node = extractor->toc->entry_list; node; node = node->next) {
					TrackerResource *track;

					/* Reuse the "root" InformationElement resource for the first track,
					 * so there's no spare ones.
					 */
					if (node == extractor->toc->entry_list) {
						track = resource;
					} else {
						gchar *suffix, *resource_uri;

						suffix = g_strdup_printf ("%d", g_list_position (extractor->toc->entry_list,
						                                                 node) + 1);
						resource_uri = tracker_file_get_content_identifier (file, NULL, suffix);
						track = tracker_resource_new (resource_uri);
						g_free (resource_uri);
						g_free (suffix);
					}

					extract_track (track, extractor, node->data, file_url, album_disc);
					tracker_resource_set_relation (track, "nie:isStoredAs", file_resource);
					tracker_resource_add_take_relation (file_resource, "nie:interpretedAs", track);
				}
			} else {
				extractor_apply_audio_metadata (extractor,
				                                extractor->tagcache,
				                                file_url,
				                                resource,
				                                artist,
				                                performer,
				                                composer,
				                                album_disc);
			}

			if (album_disc)
				g_object_unref (album_disc);
		}
	}

	/* OpenSubtitles compatible hash */
	if (extractor->mime == EXTRACT_MIME_VIDEO) {
		guint64 hash;
		GFile *file;

		file = g_file_new_for_uri (file_url);
		hash = extract_gibest_hash (file);
		g_object_unref (file);

		if (hash) {
			TrackerResource *file_resource, *hash_resource;
			char *hash_str;

			hash_resource = tracker_resource_new (NULL);
			tracker_resource_set_uri (hash_resource, "rdf:type", "nfo:FileHash");

			hash_str = g_strdup_printf ("%" G_GINT64_MODIFIER "x", hash);
			tracker_resource_set_string (hash_resource, "nfo:hashValue", hash_str);
			g_free (hash_str);

			tracker_resource_set_string (hash_resource, "nfo:hashAlgorithm", "gibest");

			file_resource = ensure_file_resource (resource, file_url);

			tracker_resource_set_relation (file_resource, "nfo:hasHash", hash_resource);

			g_object_unref (hash_resource);
		}
	}

	/* If content was encrypted, set it. */
/* #warning TODO: handle encrypted content with the Discoverer/GUPnP-DLNA backends */

	common_extract_stream_metadata (extractor, file_url, resource);

	g_object_unref (file);

	return resource;
}

#if defined(GSTREAMER_BACKEND_DISCOVERER) || \
    defined(GSTREAMER_BACKEND_GUPNP_DLNA)
static void
common_extract_stream_metadata (MetadataExtractor    *extractor,
                                const gchar          *uri,
                                TrackerResource      *resource)
{
	if (extractor->mime == EXTRACT_MIME_AUDIO ||
	    extractor->mime == EXTRACT_MIME_VIDEO) {
		if (extractor->audio_channels >= 0) {
			tracker_resource_set_int64 (resource, "nfo:channels", extractor->audio_channels);
		}

		if (extractor->audio_samplerate >= 0) {
			tracker_resource_set_int64 (resource, "nfo:sampleRate", extractor->audio_samplerate);
		}

		if (extractor->duration >= 0) {
			tracker_resource_set_int64 (resource, "nfo:duration", extractor->duration);
		}
	}

	if (extractor->mime == EXTRACT_MIME_VIDEO) {
		if (extractor->video_fps >= 0) {
			tracker_resource_set_double (resource, "nfo:frameRate", (gdouble)extractor->video_fps);
		}
	}

	if (extractor->mime == EXTRACT_MIME_IMAGE ||
	    extractor->mime == EXTRACT_MIME_VIDEO) {

		if (extractor->width >= 0) {
			tracker_resource_set_int64 (resource, "nfo:width", extractor->width);
		}

		if (extractor->height >= 0) {
			tracker_resource_set_int64 (resource, "nfo:height", extractor->height);
		}

		if (extractor->aspect_ratio >= 0) {
			tracker_resource_set_double (resource, "nfo:aspectRatio", (gdouble)extractor->aspect_ratio);
		}
	}

#if defined(GSTREAMER_BACKEND_GUPNP_DLNA)
	if (extractor->dlna_profile) {
		tracker_resource_set_string (resource, "nmm:dlnaProfile", extractor->dlna_profile);
	} else {
		g_debug ("No DLNA profile found");
	}

	if (extractor->dlna_mime) {
		tracker_resource_set_string (resource, "nmm:dlnaMime", extractor->dlna_mime);
	} else {
		g_debug ("No DLNA mime found");
	}
#endif /* GSTREAMER_BACKEND_GUPNP_DLNA */
}

#endif

/* ----------------------- Discoverer/GUPnP-DLNA specific implementation --------------- */

#if defined(GSTREAMER_BACKEND_DISCOVERER) || \
    defined(GSTREAMER_BACKEND_GUPNP_DLNA)

static void
discoverer_shutdown (MetadataExtractor *extractor)
{
	if (extractor->streams)
		gst_discoverer_stream_info_list_free (extractor->streams);
	if (extractor->discoverer)
		g_object_unref (extractor->discoverer);
}

static gchar *
get_discoverer_required_plugins_message (GstDiscovererInfo *info)
{
	GString *str;
	gchar **plugins;
	gchar *plugins_str;

	plugins = (gchar **)
	        gst_discoverer_info_get_missing_elements_installer_details (info);

	if (g_strv_length((gchar **)plugins) == 0) {
		str = g_string_new ("No information available on which plugin is required.");
	} else {
		str = g_string_new("Required plugins: ");
		plugins_str = g_strjoinv (", ", (gchar **)plugins);
		g_string_append (str, plugins_str);
		g_free (plugins_str);
	}

	return g_string_free (str, FALSE);
}

static gboolean
discoverer_init_and_run (MetadataExtractor *extractor,
                         const gchar       *uri)
{
	GstDiscovererInfo *info;
	const GstTagList *discoverer_tags;
	const GstToc *gst_toc;
	GError *error = NULL;
	GList *l;
	gchar *required_plugins_message;

	extractor->duration = -1;
	extractor->audio_channels = -1;
	extractor->audio_samplerate = -1;
	extractor->height = -1;
	extractor->width = -1;
	extractor->video_fps = -1.0;
	extractor->aspect_ratio = -1.0;

	extractor->has_image = FALSE;
	extractor->has_video = FALSE;
	extractor->has_audio = FALSE;

	extractor->discoverer = gst_discoverer_new (5 * GST_SECOND, &error);
	if (!extractor->discoverer) {
		g_warning ("Couldn't create discoverer: %s",
		           error ? error->message : "unknown error");
		g_clear_error (&error);
		return FALSE;
	}

#if defined(GST_TYPE_DISCOVERER_FLAGS)
	/* Tell the discoverer to use *only* Tagreadbin backend.
	 *  See https://bugzilla.gnome.org/show_bug.cgi?id=656345
	 */
	g_debug ("Using Tagreadbin backend in the GStreamer discoverer...");
	g_object_set (extractor->discoverer,
	              "flags", GST_DISCOVERER_FLAGS_EXTRACT_LIGHTWEIGHT,
	              NULL);
#endif

	info = gst_discoverer_discover_uri (extractor->discoverer,
	                                    uri,
	                                    &error);

	if (!info) {
		g_warning ("Nothing discovered, bailing out");
		return TRUE;
	}

	if (error) {
		if (gst_discoverer_info_get_result(info) == GST_DISCOVERER_MISSING_PLUGINS) {
			required_plugins_message = get_discoverer_required_plugins_message (info);
			g_debug ("Missing a GStreamer plugin for %s. %s", uri,
			         required_plugins_message);
			g_free (required_plugins_message);
		} else if (error->domain != GST_STREAM_ERROR ||
		           (error->code != GST_STREAM_ERROR_TYPE_NOT_FOUND &&
		            error->code != GST_STREAM_ERROR_WRONG_TYPE &&
		            error->code != GST_STREAM_ERROR_DECODE)) {
			g_warning ("Call to gst_discoverer_discover_uri(%s) failed: %s",
			           uri, error->message);
		}
		gst_discoverer_info_unref (info);
		g_error_free (error);
		return FALSE;
	}

#if defined(GSTREAMER_BACKEND_GUPNP_DLNA)
	{
		GUPnPDLNAProfile *profile;
		GUPnPDLNAInformation *dlna_info;
		GUPnPDLNAProfileGuesser *guesser;

		dlna_info = gupnp_dlna_gst_utils_information_from_discoverer_info (info);
		guesser = gupnp_dlna_profile_guesser_new (TRUE, FALSE);
		profile = gupnp_dlna_profile_guesser_guess_profile_from_info (guesser, dlna_info);

		if (profile) {
			extractor->dlna_profile = gupnp_dlna_profile_get_name (profile);
			extractor->dlna_mime = gupnp_dlna_profile_get_mime (profile);
		}

		g_object_unref (guesser);
		g_object_unref (dlna_info);
	}
#endif

	gst_toc = gst_discoverer_info_get_toc (info);
	if (gst_toc)
		extractor->gst_toc = gst_toc_copy (gst_toc);

	extractor->duration = gst_discoverer_info_get_duration (info) / GST_SECOND;

	/* Retrieve global tags */
#if defined(HAVE_GSTREAMER_1_20)
	GstDiscovererStreamInfo *sinfo = gst_discoverer_info_get_stream_info (info);

	if (GST_IS_DISCOVERER_CONTAINER_INFO (sinfo))
		discoverer_tags = gst_discoverer_container_info_get_tags (GST_DISCOVERER_CONTAINER_INFO (sinfo));
	else if (GST_IS_DISCOVERER_STREAM_INFO (sinfo))
		discoverer_tags = gst_discoverer_stream_info_get_tags (sinfo);
	else
		discoverer_tags = NULL;
#else
	discoverer_tags = gst_discoverer_info_get_tags (info);
#endif

	if (discoverer_tags) {
		gst_tag_list_insert (extractor->tagcache,
				     discoverer_tags,
				     GST_TAG_MERGE_APPEND);
	}

	/* Get list of Streams to iterate */
	extractor->streams = gst_discoverer_info_get_stream_list (info);
	for (l = extractor->streams; l; l = g_list_next (l)) {
		GstDiscovererStreamInfo *stream = l->data;

		if (G_TYPE_CHECK_INSTANCE_TYPE (stream, GST_TYPE_DISCOVERER_AUDIO_INFO)) {
			GstDiscovererAudioInfo *audio = (GstDiscovererAudioInfo*)stream;

			extractor->has_audio = TRUE;
			extractor->audio_samplerate = gst_discoverer_audio_info_get_sample_rate (audio);
			extractor->audio_channels = gst_discoverer_audio_info_get_channels (audio);
		} else if (G_TYPE_CHECK_INSTANCE_TYPE (stream, GST_TYPE_DISCOVERER_VIDEO_INFO)) {
			GstDiscovererVideoInfo *video = (GstDiscovererVideoInfo*)stream;

			if (gst_discoverer_video_info_is_image (video)) {
				extractor->has_image = TRUE;
			} else {
				extractor->has_video = TRUE;
				if (gst_discoverer_video_info_get_framerate_denom (video) > 0) {
					extractor->video_fps = (gfloat)(gst_discoverer_video_info_get_framerate_num (video) /
						                        gst_discoverer_video_info_get_framerate_denom (video));
				}
				extractor->width = gst_discoverer_video_info_get_width (video);
				extractor->height = gst_discoverer_video_info_get_height (video);
				if (gst_discoverer_video_info_get_par_denom (video) > 0) {
					extractor->aspect_ratio = (gfloat)(gst_discoverer_video_info_get_par_num (video) /
						                           gst_discoverer_video_info_get_par_denom (video));
				}
			}
		} else {
			/* Unknown type - do nothing */
		}
	}

	for (l = extractor->streams; l; l = g_list_next (l)) {
		GstDiscovererStreamInfo *stream = l->data;
		const GstTagList *stream_tags;
		GstTagList *copy = NULL;

		stream_tags = gst_discoverer_stream_info_get_tags (stream);

		if (stream_tags)
			copy = gst_tag_list_copy (stream_tags);

		if (extractor->has_video &&
		    copy &&
		    gst_tag_list_get_tag_size (extractor->tagcache, "title") > 0)
			gst_tag_list_remove_tag (copy, "title");

		if (copy) {
			gst_tag_list_insert (extractor->tagcache,
					     copy,
					     GST_TAG_MERGE_APPEND);
		}

		g_clear_pointer (&copy, gst_tag_list_unref);
	}

	gst_discoverer_info_unref (info);
#if defined(HAVE_GSTREAMER_1_20)
	gst_discoverer_stream_info_unref (sinfo);
#endif

	return TRUE;
}

#endif /* defined(GSTREAMER_BACKEND_DISCOVERER) || \
          defined(GSTREAMER_BACKEND_GUPNP_DLNA) */

static TrackerResource *
tracker_extract_gstreamer (const gchar          *uri,
                           TrackerExtractInfo   *info,
                           ExtractMime           type)
{
	TrackerResource *main_resource = NULL;
	MetadataExtractor *extractor;
	GstBuffer *buffer;
	gchar *cue_sheet;
	gboolean success;

	g_return_val_if_fail (uri, NULL);

	extractor = g_slice_new0 (MetadataExtractor);
	extractor->mime = type;
	extractor->tagcache = gst_tag_list_new_empty ();

	g_debug ("GStreamer backend in use:");
	g_debug ("  Discoverer/GUPnP-DLNA");
	success = discoverer_init_and_run (extractor, uri);

	if (success) {
		cue_sheet = get_embedded_cue_sheet_data (extractor->tagcache);

		if (cue_sheet) {
			g_debug ("Using embedded CUE sheet.");
			extractor->toc = tracker_cue_sheet_parse (cue_sheet);
			g_free (cue_sheet);
		}

		if (extractor->toc == NULL) {
			if (!local_conn)
				local_conn = tracker_main_get_readonly_connection (NULL);

			extractor->toc = tracker_cue_sheet_guess_from_uri (local_conn, uri);
		}

		if (extractor->toc == NULL &&
		    extractor->gst_toc != NULL) {
			extractor->toc = translate_discoverer_toc (extractor->gst_toc);
		}

		main_resource = extract_metadata (extractor, uri);
	}

	/* Clean up */
	if (extractor->sample) {
		buffer = gst_sample_get_buffer (extractor->sample);
		gst_buffer_unmap (buffer, &extractor->info);
		gst_sample_unref (extractor->sample);
	}

	gst_tag_list_free (extractor->tagcache);

	tracker_toc_free (extractor->toc);

	if (extractor->gst_toc)
		gst_toc_unref (extractor->gst_toc);

	g_slist_foreach (extractor->artist_list, (GFunc)g_object_unref, NULL);
	g_slist_free (extractor->artist_list);

	discoverer_shutdown (extractor);

	g_slice_free (MetadataExtractor, extractor);

	return main_resource;
}

G_MODULE_EXPORT gboolean
tracker_extract_get_metadata (TrackerExtractInfo  *info,
                              GError             **error)
{
	GFile *file;
	gchar *uri;
	const gchar *mimetype;
	TrackerResource *main_resource;

	file = tracker_extract_info_get_file (info);
	uri = g_file_get_uri (file);
	mimetype = tracker_extract_info_get_mimetype (info);

#if defined(GSTREAMER_BACKEND_GUPNP_DLNA)
	if (g_str_has_prefix (mimetype, "dlna/")) {
		main_resource = tracker_extract_gstreamer (uri, info, EXTRACT_MIME_GUESS);
	} else
#endif /* GSTREAMER_BACKEND_GUPNP_DLNA */

	if (strcmp (mimetype, "video/3gpp") == 0 ||
	           strcmp (mimetype, "video/mp4") == 0 ||
                   strcmp (mimetype, "video/x-ms-asf") == 0 ||
                   strcmp (mimetype, "application/vnd.ms-asf") == 0 ||
	           strcmp (mimetype, "application/vnd.rn-realmedia") == 0) {
		main_resource = tracker_extract_gstreamer (uri, info, EXTRACT_MIME_GUESS);
	} else if (g_str_has_prefix (mimetype, "audio/")) {
		main_resource = tracker_extract_gstreamer (uri, info, EXTRACT_MIME_AUDIO);
	} else if (g_str_has_prefix (mimetype, "video/")) {
		main_resource = tracker_extract_gstreamer (uri, info, EXTRACT_MIME_VIDEO);
	} else if (g_str_has_prefix (mimetype, "image/")) {
		main_resource = tracker_extract_gstreamer (uri, info, EXTRACT_MIME_IMAGE);
	} else {
		g_set_error (error,
		             G_IO_ERROR,
		             G_IO_ERROR_INVALID_ARGUMENT,
		             "Mimetype '%s is not supported",
		             mimetype);
		g_free (uri);
		return FALSE;
	}

	if (main_resource) {
		tracker_extract_info_set_resource (info, main_resource);
		g_object_unref (main_resource);
	}

	g_free (uri);
	return TRUE;
}

G_MODULE_EXPORT gboolean
tracker_extract_module_init (GError **error)
{
	/* Lifted from totem-video-thumbnailer */
	const gchar *blocklisted[] = {
		"bcmdec",
		"fluiddec",
		"vaapi",
		"video4linux2",
		"nvcodec",
		"ges",
	};
	GstRegistry *registry;
	guint i;

	gst_init (NULL, NULL);
	registry = gst_registry_get ();

	for (i = 0; i < G_N_ELEMENTS (blocklisted); i++) {
		GstPlugin *plugin =
			gst_registry_find_plugin (registry,
						  blocklisted[i]);
		if (plugin) {
			g_debug ("Removing GStreamer plugin '%s' from registry",
			         blocklisted[i]);
			gst_registry_remove_plugin (registry, plugin);
		}
	}

	return TRUE;
}

G_MODULE_EXPORT gboolean
tracker_extract_module_shutdown (void)
{
	g_clear_object (&local_conn);
	return TRUE;
}
