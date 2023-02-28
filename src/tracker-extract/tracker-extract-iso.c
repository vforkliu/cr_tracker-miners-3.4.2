/*
 * Copyright (C) 2011, 2014 Red Hat, Inc
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
 * Authors: Christophe Fergeau <cfergeau@redhat.com>
 *          Zeeshan Ali (Khattak) <zeeshanak@gnome.org>
 */

#include "config-miners.h"

#include <stdio.h>

#include <osinfo/osinfo.h>

#include <gio/gio.h>

#include <libtracker-extract/tracker-extract.h>
#include <libtracker-miners-common/tracker-file-utils.h>
#include <libtracker-sparql/tracker-sparql.h>

G_MODULE_EXPORT gboolean
tracker_extract_get_metadata (TrackerExtractInfo  *info_,
                              GError             **error)
{
	/* NOTE: This function has to exist, tracker-extract checks
	 * the symbol table for this function and if it doesn't
	 * exist, the module is not loaded to be used as an extractor.
	 */

	/* File information */
	GFile *file;
	GError *inner_error = NULL;
	gchar *filename, *resource_uri;
	OsinfoLoader *loader = NULL;
	OsinfoMedia *media;
	OsinfoDb *db;
	OsinfoOs *os;
	OsinfoOsVariantList *variants;

	/* Data input */
	gboolean bootable;
	const gchar *id;
	const gchar *name;
	GList *languages, *l;
	TrackerResource *metadata;

	file = tracker_extract_info_get_file (info_);
	filename = g_file_get_path (file);

	resource_uri = tracker_file_get_content_identifier (file, NULL, NULL);
	metadata = tracker_resource_new (resource_uri);
	g_free (resource_uri);

	media = osinfo_media_create_from_location (filename, NULL, &inner_error);
	if (inner_error != NULL) {
		if (inner_error->code != OSINFO_MEDIA_ERROR_NOT_BOOTABLE) {
			g_object_unref (metadata);
			g_free (filename);
			g_propagate_prefixed_error (error, inner_error, "Could not extract ISO info:");
			return FALSE;
		}
		bootable = FALSE;
		goto no_os;
	} else {
		bootable = TRUE;
	}
	g_free (filename);

	loader = osinfo_loader_new ();
	osinfo_loader_process_default_path (loader, &inner_error);
	if (inner_error != NULL) {
		g_message ("Error loading libosinfo OS data: %s",
			   inner_error->message);
		g_error_free (inner_error);
		goto no_os;
	}
	g_warn_if_fail (media != NULL);
	g_warn_if_fail (loader != NULL);

	db = osinfo_loader_get_db (loader);
	osinfo_db_identify_media (db, media);
	os = osinfo_media_get_os (media);

	if (os == NULL)
		goto unknown_os;

	tracker_resource_add_uri (metadata, "rdf:type", "nfo:FilesystemImage");

	variants = osinfo_media_get_os_variants (media);
	if (osinfo_list_get_length (OSINFO_LIST (variants)) > 0) {
		OsinfoEntity *variant;

		/* FIXME: Assuming first variant from multivariant medias. */
		variant = osinfo_list_get_nth (OSINFO_LIST (variants), 0);
		name = osinfo_os_variant_get_name (OSINFO_OS_VARIANT (variant));
	} else {
		name = osinfo_product_get_name (OSINFO_PRODUCT (os));
	}

	g_object_unref (variants);

	if (name != NULL) {
		tracker_resource_set_string (metadata, "nie:title", name);
	}

	if (osinfo_media_get_live (media)) {
		tracker_resource_add_uri (metadata, "rdf:type", "nfo:OperatingSystem");
	}

	if (osinfo_media_get_installer (media)) {
		tracker_resource_add_uri (metadata, "rdf:type", "osinfo:Installer");
	}

	tracker_resource_set_boolean (metadata, "nfo:isBootable", bootable);

	id = osinfo_entity_get_id (OSINFO_ENTITY (os));
	if (id != NULL) {
		tracker_resource_set_string (metadata, "osinfo:id", id);
	}

        id = osinfo_entity_get_id (OSINFO_ENTITY (media));
	if (id != NULL) {
		tracker_resource_set_string (metadata, "osinfo:mediaId", id);
	}

        languages = osinfo_media_get_languages (media);
        for (l = languages; l != NULL; l = l->next) {
		tracker_resource_add_string (metadata, "osinfo:language", (char *)l->data);
        }
        g_list_free (languages);

	g_object_unref (media);
	g_object_unref (loader);
	g_object_unref (os);

	tracker_extract_info_set_resource (info_, metadata);
	g_object_unref (metadata);

	return TRUE;

unknown_os:
        name = osinfo_media_get_volume_id (media);
	if (name != NULL) {
                gchar *stripped = g_strdup (name);

                g_strstrip (stripped);
		tracker_resource_set_string (metadata, "nie:title", stripped);
                g_free (stripped);
	}

no_os:
	if (media != NULL) {
		g_object_unref (G_OBJECT (media));
	}
	if (loader != NULL) {
		g_object_unref (G_OBJECT (loader));
	}

	tracker_resource_add_uri (metadata, "rdf:type", "nfo:FilesystemImage");

	tracker_resource_set_boolean (metadata, "nfo:isBootable", bootable);

	tracker_extract_info_set_resource (info_, metadata);
	g_object_unref (metadata);

	return TRUE;
}
