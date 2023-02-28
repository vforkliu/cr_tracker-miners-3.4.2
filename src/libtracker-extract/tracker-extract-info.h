/*
 * Copyright (C) 2011, Nokia <ivan.frade@nokia.com>
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
 * Author: Carlos Garnacho <carlos@lanedo.com>
 */

#ifndef __LIBTRACKER_EXTRACT_INFO_H__
#define __LIBTRACKER_EXTRACT_INFO_H__

#if !defined (__LIBTRACKER_EXTRACT_INSIDE__) && !defined (TRACKER_COMPILATION)
#error "only <libtracker-extract/tracker-extract.h> must be included directly."
#endif

#include <libtracker-sparql/tracker-sparql.h>
#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _TrackerExtractInfo TrackerExtractInfo;

GType                 tracker_extract_info_get_type               (void) G_GNUC_CONST;

TrackerExtractInfo *  tracker_extract_info_new                    (GFile              *file,
                                                                   const gchar        *mimetype,
                                                                   const gchar        *graph);
TrackerExtractInfo *  tracker_extract_info_ref                    (TrackerExtractInfo *info);
void                  tracker_extract_info_unref                  (TrackerExtractInfo *info);
GFile *               tracker_extract_info_get_file               (TrackerExtractInfo *info);
const gchar *         tracker_extract_info_get_mimetype           (TrackerExtractInfo *info);
const gchar *         tracker_extract_info_get_graph              (TrackerExtractInfo *info);

TrackerResource *     tracker_extract_info_get_resource           (TrackerExtractInfo *info);
void                  tracker_extract_info_set_resource           (TrackerExtractInfo *info,
                                                                   TrackerResource    *resource);

G_END_DECLS

#endif /* __LIBTRACKER_EXTRACT_INFO_H__ */
