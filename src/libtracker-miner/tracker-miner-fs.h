/*
 * Copyright (C) 2009, Nokia <ivan.frade@nokia.com>
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

#ifndef __LIBTRACKER_MINER_MINER_FS_H__
#define __LIBTRACKER_MINER_MINER_FS_H__

#if !defined (__LIBTRACKER_MINER_H_INSIDE__) && !defined (TRACKER_COMPILATION)
#error "Only <libtracker-miner/tracker-miner.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>

#include <libtracker-sparql/tracker-sparql.h>

#include "tracker-miner-object.h"
#include "tracker-data-provider.h"
#include "tracker-indexing-tree.h"
#include "tracker-sparql-buffer.h"

G_BEGIN_DECLS

#define TRACKER_PREFIX_DATASOURCE_URN \
       "urn:nepomuk:datasource:"

#define TRACKER_DATASOURCE_URN_NON_REMOVABLE_MEDIA \
        TRACKER_PREFIX_DATASOURCE_URN "9291a450-1d49-11de-8c30-0800200c9a66"

#define TRACKER_OWN_GRAPH_URN "urn:uuid:472ed0cc-40ff-4e37-9c0c-062d78656540"

#define TRACKER_TYPE_MINER_FS         (tracker_miner_fs_get_type())
#define TRACKER_MINER_FS(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), TRACKER_TYPE_MINER_FS, TrackerMinerFS))
#define TRACKER_MINER_FS_CLASS(c)     (G_TYPE_CHECK_CLASS_CAST ((c), TRACKER_TYPE_MINER_FS, TrackerMinerFSClass))
#define TRACKER_IS_MINER_FS(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), TRACKER_TYPE_MINER_FS))
#define TRACKER_IS_MINER_FS_CLASS(c)  (G_TYPE_CHECK_CLASS_TYPE ((c),  TRACKER_TYPE_MINER_FS))
#define TRACKER_MINER_FS_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), TRACKER_TYPE_MINER_FS, TrackerMinerFSClass))

typedef enum {
	TRACKER_MINER_FS_EVENT_CREATED,
	TRACKER_MINER_FS_EVENT_UPDATED,
	TRACKER_MINER_FS_EVENT_DELETED,
	TRACKER_MINER_FS_EVENT_MOVED,
} TrackerMinerFSEventType;

typedef struct _TrackerMinerFS        TrackerMinerFS;
typedef struct _TrackerMinerFSPrivate TrackerMinerFSPrivate;

/**
 * TrackerMinerFS:
 *
 * Abstract miner implementation to get data from the filesystem.
 **/
struct _TrackerMinerFS {
	TrackerMiner parent;
	TrackerMinerFSPrivate *priv;
};

/**
 * TrackerMinerFSClass:
 * @parent: parent object class
 * @process_file: Called when the metadata associated to a file is
 * requested.
 * @finished: Called when all processing has been performed.
 * @process_file_attributes: Called when the metadata associated with
 * a file's attributes changes, for example, the mtime.
 * @finished_root: Called when all resources on a particular root URI
 * have been processed.
 * @remove_file: Called when a file is removed.
 * @remove_children: Called when children have been removed.
 * @move_file: Called when a file has moved.
 * @padding: Reserved for future API improvements.
 *
 * Prototype for the abstract class, @process_file must be implemented
 * in the deriving class in order to actually extract data.
 **/
typedef struct {
	TrackerMinerClass parent;

	void     (* process_file)             (TrackerMinerFS       *fs,
	                                       GFile                *file,
	                                       GFileInfo            *info,
	                                       TrackerSparqlBuffer  *buffer,
	                                       gboolean              created);
	void     (* finished)                 (TrackerMinerFS       *fs,
	                                       gdouble               elapsed,
	                                       gint                  directories_found,
	                                       gint                  directories_ignored,
	                                       gint                  files_found,
	                                       gint                  files_ignored);
	void     (* process_file_attributes)  (TrackerMinerFS       *fs,
	                                       GFile                *file,
	                                       GFileInfo            *info,
	                                       TrackerSparqlBuffer  *buffer);
	void     (* finished_root)            (TrackerMinerFS       *fs,
	                                       GFile                *root,
	                                       gint                  directories_found,
	                                       gint                  directories_ignored,
	                                       gint                  files_found,
	                                       gint                  files_ignored);
	void     (* remove_file)              (TrackerMinerFS       *fs,
	                                       GFile                *file,
	                                       TrackerSparqlBuffer  *buffer,
	                                       gboolean              is_dir);
	void     (* remove_children)          (TrackerMinerFS       *fs,
	                                       GFile                *file,
	                                       TrackerSparqlBuffer  *buffer);
	void     (* move_file)                (TrackerMinerFS       *fs,
	                                       GFile                *dest,
	                                       GFile                *source,
	                                       TrackerSparqlBuffer  *buffer,
	                                       gboolean              recursive);
} TrackerMinerFSClass;

/**
 * TrackerMinerFSError:
 * @TRACKER_MINER_FS_ERROR_INIT: There was an error during
 * initialization of the object. The specific details are in the
 * message.
 *
 * Possible errors returned when calling creating new objects based on
 * the #TrackerMinerFS type and other APIs available with this class.
 *
 * Since: 1.2
 **/
typedef enum {
	TRACKER_MINER_FS_ERROR_INIT,
} TrackerMinerFSError;

GType                 tracker_miner_fs_get_type              (void) G_GNUC_CONST;
GQuark                tracker_miner_fs_error_quark           (void);

/* Properties */
TrackerIndexingTree * tracker_miner_fs_get_indexing_tree     (TrackerMinerFS  *fs);
TrackerDataProvider * tracker_miner_fs_get_data_provider     (TrackerMinerFS  *fs);
gdouble               tracker_miner_fs_get_throttle          (TrackerMinerFS  *fs);
void                  tracker_miner_fs_set_throttle          (TrackerMinerFS  *fs,
                                                              gdouble          throttle);

/* Queueing files to be processed AFTER checking rules in IndexingTree */
void                  tracker_miner_fs_check_file            (TrackerMinerFS  *fs,
                                                              GFile           *file,
                                                              gint             priority,
                                                              gboolean         check_parents);

/* Continuation for async vmethods */
void                  tracker_miner_fs_notify_finish         (TrackerMinerFS  *fs,
							      GTask           *task,
							      const gchar     *sparql,
							      GError          *error);

/* URNs */
const gchar * tracker_miner_fs_get_identifier (TrackerMinerFS *miner,
                                               GFile          *file);

/* Progress */
gboolean              tracker_miner_fs_has_items_to_process  (TrackerMinerFS  *fs);

G_END_DECLS

#endif /* __LIBTRACKER_MINER_MINER_FS_H__ */
