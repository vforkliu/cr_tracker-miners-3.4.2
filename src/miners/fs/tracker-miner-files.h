/*
 * Copyright (C) 2008, Nokia <ivan.frade@nokia.com>
 *
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

#ifndef __TRACKER_MINER_FS_FILES_H__
#define __TRACKER_MINER_FS_FILES_H__

#include <libtracker-miner/tracker-miner.h>

#include "tracker-config.h"

#include "tracker-storage.h"

G_BEGIN_DECLS

#define TRACKER_TYPE_MINER_FILES         (tracker_miner_files_get_type())
#define TRACKER_MINER_FILES(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), TRACKER_TYPE_MINER_FILES, TrackerMinerFiles))
#define TRACKER_MINER_FILES_CLASS(c)     (G_TYPE_CHECK_CLASS_CAST ((c), TRACKER_TYPE_MINER_FILES, TrackerMinerFilesClass))
#define TRACKER_IS_MINER_FILES(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), TRACKER_TYPE_MINER_FILES))
#define TRACKER_IS_MINER_FILES_CLASS(c)  (G_TYPE_CHECK_CLASS_TYPE ((c),  TRACKER_TYPE_MINER_FILES))
#define TRACKER_MINER_FILES_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), TRACKER_TYPE_MINER_FILES, TrackerMinerFilesClass))

typedef struct TrackerMinerFiles TrackerMinerFiles;
typedef struct TrackerMinerFilesClass TrackerMinerFilesClass;
typedef struct TrackerMinerFilesPrivate TrackerMinerFilesPrivate;

struct TrackerMinerFiles {
	TrackerMinerFS parent_instance;
	TrackerMinerFilesPrivate *private;
};

struct TrackerMinerFilesClass {
	TrackerMinerFSClass parent_class;
};

GType         tracker_miner_files_get_type                 (void) G_GNUC_CONST;

TrackerMiner *tracker_miner_files_new                      (TrackerSparqlConnection  *connection,
                                                            TrackerConfig            *config,
                                                            const gchar              *domain,
                                                            GError                  **error);

/* Global functions to handle timestamp files */
gboolean tracker_miner_files_get_first_index_done (TrackerMinerFiles *mf);
void     tracker_miner_files_set_first_index_done (TrackerMinerFiles *mf,
                                                   gboolean           done);

guint64  tracker_miner_files_get_last_crawl_done  (TrackerMinerFiles *mf);
void     tracker_miner_files_set_last_crawl_done  (TrackerMinerFiles *mf,
                                                   gboolean           done);

gboolean tracker_miner_files_get_need_mtime_check (TrackerMinerFiles *mf);
void     tracker_miner_files_set_need_mtime_check (TrackerMinerFiles *mf,
                                                   gboolean           needed);

void     tracker_miner_files_set_mtime_checking   (TrackerMinerFiles *miner,
                                                   gboolean           mtime_checking);

void     tracker_miner_files_writeback_file       (TrackerMinerFiles *mf,
                                                   GFile             *file,
                                                   GStrv              rdf_types,
                                                   GPtrArray         *results);
void     tracker_miner_files_writeback_notify     (TrackerMinerFiles *mf,
                                                   GFile             *file,
                                                   const GError      *error);
TrackerStorage * tracker_miner_files_get_storage (TrackerMinerFiles *mf);

G_END_DECLS

#endif /* __TRACKER_MINER_FS_FILES_H__ */
