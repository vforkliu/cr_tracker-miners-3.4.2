/*
 * Copyright (C) 2009/2010, Roberto Guido <madbob@users.barberaware.org>
 *                          Michele Tameni <michele@amdplanet.it>
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

#include "config-miners.h"

#include <stdio.h>

#include <libgrss.h>

#include <libtracker-miners-common/tracker-dbus.h>
#include <libtracker-sparql/tracker-ontologies.h>
#include <libtracker-miners-common/tracker-common.h>

#include <glib/gi18n.h>

#include "tracker-miner-rss.h"

#define TRACKER_MINER_RSS_GET_PRIVATE(obj) (tracker_miner_rss_get_instance_private (TRACKER_MINER_RSS (obj)))

typedef struct _TrackerMinerRSSPrivate TrackerMinerRSSPrivate;

struct _TrackerMinerRSSPrivate {
	gboolean paused;
	gboolean stopped;
	gchar *last_status;

	GrssFeedsPool *pool;
	gint now_fetching;
	GDBusConnection *connection;
	guint graph_updated_id;

	GHashTable *channel_updates;
	GHashTable *channels;

	TrackerNotifier *notifier;
};

typedef struct {
	TrackerMinerRSS *miner;
	GrssFeedChannel *channel;
	gint timeout_id;
	GCancellable *cancellable;
} FeedChannelUpdateData;

typedef struct {
	TrackerMinerRSS *miner;
	GrssFeedItem *item;
	GCancellable *cancellable;
} FeedItemInsertData;

typedef struct {
	TrackerMinerRSS *miner;
	GrssFeedChannel *channel;
	GHashTable *items;
} FeedItemListInsertData;

static void         notifier_events_cb              (TrackerNotifier       *notifier,
                                                     const gchar           *service,
                                                     const gchar           *graph,
                                                     GPtrArray             *events,
                                                     gpointer               user_data);
static void         miner_started                   (TrackerMiner          *miner);
static void         miner_stopped                   (TrackerMiner          *miner);
static void         miner_paused                    (TrackerMiner          *miner);
static void         miner_resumed                   (TrackerMiner          *miner);
static void         retrieve_and_schedule_feeds     (TrackerMinerRSS       *miner,
                                                     GArray                *channel_ids);
static gboolean     feed_channel_changed_timeout_cb (gpointer               user_data);
static void         feed_channel_update_data_free   (FeedChannelUpdateData *fcud);
static void         feed_fetching_cb                (GrssFeedsPool             *pool,
                                                     GrssFeedChannel           *feed,
                                                     gpointer               user_data);
static void         feed_ready_cb                   (GrssFeedsPool             *pool,
                                                     GrssFeedChannel           *feed,
                                                     GList                 *items,
                                                     gpointer               user_data);
static const gchar *get_message_url                 (GrssFeedItem              *item);

G_DEFINE_TYPE_WITH_PRIVATE (TrackerMinerRSS, tracker_miner_rss, TRACKER_TYPE_MINER_ONLINE)

static void
parser_characters (void          *data,
                   const xmlChar *ch,
                   int            len)
{
	GString *string = data;
	const gchar *str, *end;

	str = (gchar *)ch;
	g_utf8_validate (str, len, &end);

	if (end > str) {
		g_string_append_len (string, str, end - str);
	}

	if (string->str[string->len - 1] != ' ')
		g_string_append_c (string, ' ');
}

static gchar *
parse_html_text (const gchar *html)
{
	GString *string;
	htmlDocPtr doc;
	xmlSAXHandler handler = {
		NULL, /* internalSubset */
		NULL, /* isStandalone */
		NULL, /* hasInternalSubset */
		NULL, /* hasExternalSubset */
		NULL, /* resolveEntity */
		NULL, /* getEntity */
		NULL, /* entityDecl */
		NULL, /* notationDecl */
		NULL, /* attributeDecl */
		NULL, /* elementDecl */
		NULL, /* unparsedEntityDecl */
		NULL, /* setDocumentLocator */
		NULL, /* startDocument */
		NULL, /* endDocument */
		NULL, /* startElement */
		NULL, /* endElement */
		NULL, /* reference */
		parser_characters, /* characters */
		NULL, /* ignorableWhitespace */
		NULL, /* processingInstruction */
		NULL, /* comment */
		NULL, /* xmlParserWarning */
		NULL, /* xmlParserError */
		NULL, /* xmlParserError */
		NULL, /* getParameterEntity */
		NULL, /* cdataBlock */
		NULL, /* externalSubset */
		1,    /* initialized */
		NULL, /* private */
		NULL, /* startElementNsSAX2Func */
		NULL, /* endElementNsSAX2Func */
		NULL  /* xmlStructuredErrorFunc */
	};

	string = g_string_new (NULL);
	doc = htmlSAXParseDoc ((xmlChar *) html, "UTF-8", &handler, string);

	if (doc) {
		xmlFreeDoc (doc);
	}

	return g_string_free (string, FALSE);
}

static void
tracker_miner_rss_constructed (GObject *object)
{
	TrackerMinerRSSPrivate *priv;
	TrackerSparqlConnection *connection;

	priv = TRACKER_MINER_RSS_GET_PRIVATE (object);
	connection = tracker_miner_get_connection (TRACKER_MINER (object));

	priv->notifier = tracker_sparql_connection_create_notifier (connection);
	g_signal_connect (priv->notifier, "events",
	                  G_CALLBACK (notifier_events_cb), object);
}

static void
tracker_miner_rss_finalize (GObject *object)
{
	TrackerMinerRSSPrivate *priv;

	priv = TRACKER_MINER_RSS_GET_PRIVATE (object);

	priv->stopped = TRUE;
	g_free (priv->last_status);
	g_object_unref (priv->pool);
	g_object_unref (priv->notifier);

	g_hash_table_unref (priv->channel_updates);
	g_hash_table_unref (priv->channels);

	G_OBJECT_CLASS (tracker_miner_rss_parent_class)->finalize (object);
}

static gboolean
miner_connected (TrackerMinerOnline *miner,
		 TrackerNetworkType  network)
{
	return (network == TRACKER_NETWORK_TYPE_LAN);
}

static void
tracker_miner_rss_class_init (TrackerMinerRSSClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	TrackerMinerClass *miner_class = TRACKER_MINER_CLASS (klass);
	TrackerMinerOnlineClass *miner_online_class = TRACKER_MINER_ONLINE_CLASS (klass);

	object_class->finalize = tracker_miner_rss_finalize;
	object_class->constructed = tracker_miner_rss_constructed;

	miner_class->started = miner_started;
	miner_class->stopped = miner_stopped;
	miner_class->paused  = miner_paused;
	miner_class->resumed = miner_resumed;

	miner_online_class->connected = miner_connected;
}

static void
tracker_miner_rss_init (TrackerMinerRSS *object)
{
	TrackerMinerRSSPrivate *priv;

	g_message ("Initializing...");

	priv = TRACKER_MINER_RSS_GET_PRIVATE (object);

	/* Key object reference is cleaned up in value destroy func */
	priv->channel_updates = g_hash_table_new_full (g_direct_hash,
	                                               g_direct_equal,
	                                               NULL,
	                                               (GDestroyNotify) feed_channel_update_data_free);
	priv->channels = g_hash_table_new_full (NULL, NULL, NULL,
	                                        (GDestroyNotify) g_object_unref);

	priv->pool = grss_feeds_pool_new ();
	g_signal_connect (priv->pool, "feed-fetching", G_CALLBACK (feed_fetching_cb), object);
	g_signal_connect (priv->pool, "feed-ready", G_CALLBACK (feed_ready_cb), object);
	priv->now_fetching = 0;
}

static void
delete_unbound_messages (TrackerMinerRSS *miner)
{
	tracker_sparql_connection_update_async (tracker_miner_get_connection (TRACKER_MINER (miner)),
	                                        "DELETE { ?msg a rdfs:Resource."
	                                        "         ?encl a rdfs:Resource. }"
	                                        "WHERE  { ?msg a mfo:FeedMessage ;"
	                                        "                mfo:enclosureList ?encl ."
	                                        "              FILTER(!BOUND(nmo:communicationChannel(?msg)))"
	                                        "}",
	                                        NULL, NULL, NULL);
}

static void
delete_message_channels_cb (GObject      *source_object,
                            GAsyncResult *res,
                            gpointer      user_data)
{
	TrackerMinerRSS *miner = user_data;
	TrackerSparqlConnection *connection;
	GError *error = NULL;

	connection = TRACKER_SPARQL_CONNECTION (source_object);
	tracker_sparql_connection_update_finish (connection, res, &error);

	if (error != NULL) {
		g_message ("Could not delete message channels: %s", error->message);
		g_error_free (error);
		return;
	}

	delete_unbound_messages (miner);
}

static void
delete_message_channels (TrackerMinerRSS *miner,
                         GArray          *channel_ids)
{
	TrackerMinerRSSPrivate *priv;
	GString *query, *ids_str;
	GrssFeedChannel *channel;
	gint i, id;

	priv = TRACKER_MINER_RSS_GET_PRIVATE (miner);
	ids_str = g_string_new (NULL);
	query = g_string_new (NULL);

	for (i = 0; i < channel_ids->len; i++) {
		id = g_array_index (channel_ids, gint, i);
		if (i != 0)
			g_string_append (ids_str, ",");
		g_string_append_printf (ids_str, "%d", id);

		channel = g_hash_table_lookup (priv->channels,
					       GINT_TO_POINTER (id));

		if (channel) {
			g_hash_table_remove (priv->channel_updates, channel);
			g_hash_table_remove (priv->channels, GINT_TO_POINTER (id));
		}
	}

	g_string_append_printf (query,
	                        "DELETE { ?msg nmo:communicationChannel ?chan }"
	                        "WHERE  { ?msg a mfo:FeedMessage;"
	                        "              nmo:communicationChannel ?chan ."
	                        "              FILTER (tracker:id(?chan) IN (%s))"
	                        "}", ids_str->str);

	tracker_sparql_connection_update_async (tracker_miner_get_connection (TRACKER_MINER (miner)),
	                                        query->str,
	                                        NULL,
	                                        delete_message_channels_cb,
	                                        miner);
	g_string_free (ids_str, TRUE);
	g_string_free (query, TRUE);
}

static void
notifier_events_cb (TrackerNotifier *notifier,
                    const gchar     *service,
                    const gchar     *graph,
                    GPtrArray       *events,
                    gpointer         user_data)
{
	TrackerMinerRSS *miner = user_data;
	GArray *inserted, *deleted;
	gint i;

	inserted = g_array_new (FALSE, FALSE, sizeof (gint));
	deleted = g_array_new (FALSE, FALSE, sizeof (gint));

	for (i = 0; i < events->len; i++) {
		TrackerNotifierEvent *event;
		TrackerNotifierEventType event_type;
		gint64 id;

		event = g_ptr_array_index (events, i);
		event_type = tracker_notifier_event_get_event_type (event);
		id = tracker_notifier_event_get_id (event);

		if (event_type == TRACKER_NOTIFIER_EVENT_CREATE) {
			g_array_append_val (inserted, id);
		} else if (event_type == TRACKER_NOTIFIER_EVENT_DELETE) {
			g_array_append_val (deleted, id);
		}
	}

	if (deleted->len > 0)
		delete_message_channels (miner, deleted);
	if (inserted->len > 0)
		retrieve_and_schedule_feeds (miner, inserted);

	g_array_unref (inserted);
	g_array_unref (deleted);
}

static FeedChannelUpdateData *
feed_channel_update_data_new (TrackerMinerRSS *miner,
                              GrssFeedChannel     *channel)
{
	FeedChannelUpdateData *fcud;

	fcud = g_slice_new0 (FeedChannelUpdateData);
	fcud->miner = g_object_ref (miner);
	fcud->channel = g_object_ref (channel);
	fcud->timeout_id = g_timeout_add_seconds (2, feed_channel_changed_timeout_cb, fcud);
	fcud->cancellable = g_cancellable_new ();

	return fcud;
}

static void
feed_channel_update_data_free (FeedChannelUpdateData *fcud)
{
	if (!fcud) {
		return;
	}

	if (fcud->cancellable) {
		g_cancellable_cancel (fcud->cancellable);
		g_object_unref (fcud->cancellable);
	}

	if (fcud->timeout_id) {
		g_source_remove (fcud->timeout_id);
	}

	if (fcud->channel) {
		g_object_unref (fcud->channel);
	}

	if (fcud->miner) {
		g_object_unref (fcud->miner);
	}

	g_slice_free (FeedChannelUpdateData, fcud);
}

static FeedItemListInsertData *
feed_item_list_insert_data_new (TrackerMinerRSS *miner,
                                GrssFeedChannel *channel,
                                GList           *items)
{
	FeedItemListInsertData *data;
	GrssFeedItem *prev;
	const gchar *url;
	GList *l;

	data = g_slice_new0 (FeedItemListInsertData);
	data->channel = channel;
	data->miner = miner;
	data->items = g_hash_table_new_full (g_str_hash, g_str_equal,
	                                     (GDestroyNotify) g_free,
	                                     (GDestroyNotify) g_object_unref);

	/* Make items unique, keep most recent */
	for (l = items; l; l = l->next) {
		url = get_message_url (l->data);
		prev = g_hash_table_lookup (data->items, url);

		if (prev) {
			/* Compare publish times */
			if (grss_feed_item_get_publish_time (l->data) <=
			    grss_feed_item_get_publish_time (prev))
				continue;
		}

		g_hash_table_insert (data->items, g_strdup (url),
		                     g_object_ref (l->data));
	}

	return data;
}

static void
feed_item_list_insert_data_free (FeedItemListInsertData *data)
{
	g_hash_table_destroy (data->items);
	g_slice_free (FeedItemListInsertData, data);
}

static void
feed_channel_change_updated_time_cb (GObject      *source,
                                     GAsyncResult *result,
                                     gpointer      user_data)
{
	TrackerMinerRSSPrivate *priv;
	FeedChannelUpdateData *fcud;
	GError *error = NULL;

	fcud = user_data;
	priv = TRACKER_MINER_RSS_GET_PRIVATE (fcud->miner);

	tracker_sparql_connection_update_finish (TRACKER_SPARQL_CONNECTION (source), result, &error);
	if (error != NULL) {
		g_critical ("Could not change feed channel updated time, %s", error->message);
		g_error_free (error);
	}

	/* This will clean up the fcud data too */
	g_hash_table_remove (priv->channel_updates, fcud->channel);
}

static gboolean
feed_channel_changed_timeout_cb (gpointer user_data)
{
	TrackerResource *resource;
	FeedChannelUpdateData *fcud;
	gchar *uri, *time_str;

	fcud = user_data;
	fcud->timeout_id = 0;

	uri = g_object_get_data (G_OBJECT (fcud->channel), "subject");

	g_message ("Updating mfo:updatedTime for channel '%s'", grss_feed_channel_get_title (fcud->channel));

	resource = tracker_resource_new (uri);
	time_str = tracker_date_to_string (time (NULL));
	tracker_resource_set_string (resource, "mfo:updatedTime", time_str);
	g_free (time_str);

	tracker_sparql_connection_update_async (tracker_miner_get_connection (TRACKER_MINER (fcud->miner)),
	                                        tracker_resource_print_sparql_update (resource, NULL, NULL),
	                                        fcud->cancellable,
	                                        feed_channel_change_updated_time_cb,
	                                        fcud);
	g_object_unref (resource);

	return FALSE;
}

static void
feed_channel_change_updated_time (TrackerMinerRSS *miner,
                                  GrssFeedChannel *channel)
{
	TrackerMinerRSSPrivate *priv;
	FeedChannelUpdateData *fcud;

	if (!channel)
		return;

	priv = TRACKER_MINER_RSS_GET_PRIVATE (miner);

	fcud = g_hash_table_lookup (priv->channel_updates, channel);
	if (fcud) {
		/* We already had an update for this channel in
		 * progress, so we just reset the timeout.
		 */
		g_source_remove (fcud->timeout_id);
		fcud->timeout_id = g_timeout_add_seconds (2,
		                                          feed_channel_changed_timeout_cb,
		                                          fcud);
	} else {
		/* This is a new update for this channel */
		fcud = feed_channel_update_data_new (miner, channel);
		g_hash_table_insert (priv->channel_updates,
		                     fcud->channel,
		                     fcud);
	}
}

static void
feed_fetching_cb (GrssFeedsPool   *pool,
                  GrssFeedChannel *channel,
                  gpointer        user_data)
{
	gint avail;
	gdouble prog;
	TrackerMinerRSS *miner;
	TrackerMinerRSSPrivate *priv;

	miner = TRACKER_MINER_RSS (user_data);
	priv = TRACKER_MINER_RSS_GET_PRIVATE (miner);
	avail = grss_feeds_pool_get_listened_num (priv->pool);

	priv->now_fetching++;

	if (priv->now_fetching > avail)
		priv->now_fetching = avail;

	g_message ("Fetching channel details, source:'%s' (in progress: %d/%d)",
	           grss_feed_channel_get_source (channel),
	           priv->now_fetching,
	           avail);

	prog = ((gdouble) priv->now_fetching) / ((gdouble) avail);
	g_object_set (miner, "progress", prog, "status", "Fetching…", NULL);
}

static gchar *
sparql_add_website (TrackerResource *resource,
                    const gchar     *uri)
{
	TrackerResource *website;
	gchar *website_urn;

	website_urn = tracker_sparql_escape_uri_printf ("urn:website:%s", uri);

	website = tracker_resource_new (website_urn);
	tracker_resource_add_uri (website, "rdf:type", "nie:DataObject");
	tracker_resource_add_uri (website, "rdf:type", "nfo:Website");
	tracker_resource_set_string (website, "nie:url", uri);

	tracker_resource_set_take_relation (resource, "nco:websiteUrl", website);

	return website_urn;
}

static TrackerResource *
sparql_add_contact (TrackerResource *resource,
                    const gchar     *property,
                    GrssPerson      *contact)
{
	const gchar *name = grss_person_get_name (contact);
	const gchar *email = grss_person_get_email (contact);
	const gchar *person_url = grss_person_get_uri (contact);
	TrackerResource *contact_resource;

	contact_resource = tracker_resource_new (NULL);
	tracker_resource_add_uri (contact_resource, "rdf:type", "nco:Contact");
	tracker_resource_set_string (contact_resource, "nco:fullname", name);

	if (email != NULL) {
		TrackerResource *email_resource;

		email_resource = tracker_resource_new (NULL);
		tracker_resource_add_uri (email_resource, "rdf:type", "nco:EmailAddress");
		tracker_resource_set_string (email_resource, "nco:emailAddress", email);
		tracker_resource_add_take_relation (contact_resource, "nco:hasEmailAddress", email_resource);
	}

	if (person_url != NULL)
		sparql_add_website (contact_resource, person_url);

	tracker_resource_add_take_relation (resource, property, contact_resource);

	return contact_resource;
}

static void
sparql_add_enclosure (TrackerResource   *resource,
                      GrssFeedEnclosure *enclosure)
{
	TrackerResource *child;
	const gchar *url;
	gint length;
	const gchar * format;

	child = tracker_resource_new (NULL);

	url = grss_feed_enclosure_get_url (enclosure);
	length = grss_feed_enclosure_get_length (enclosure);
	format = grss_feed_enclosure_get_format (enclosure);

	tracker_resource_add_uri (child, "rdf:type", "mfo:Enclosure");
	tracker_resource_add_uri (child, "rdf:type", "nfo:RemoteDataObject");

	tracker_resource_set_uri (child, "mfo:remoteLink", url);

	tracker_resource_set_int64 (child, "nfo:fileSize", length);

	if (format != NULL)
		tracker_resource_set_string (child, "nie:mimeType", format);

	tracker_resource_add_take_relation (resource, "mfo:enclosureList", child);
}

static gchar *
feed_message_create_update_channel_query (const gchar  *item_urn,
                                          GrssFeedItem *item)
{
	GrssFeedChannel *channel;
	const gchar *channel_urn;

	channel = grss_feed_item_get_parent (item);
	channel_urn = g_object_get_data (G_OBJECT (channel), "subject");

	return g_strdup_printf ("INSERT SILENT { <%s> nmo:communicationChannel <%s> }",
	                        item_urn, channel_urn);
}

static gchar *
feed_message_create_delete_properties_query (const gchar *item_urn)
{
	return g_strdup_printf ("DELETE { <%s> ?p ?o }"
	                        "WHERE  { <%s> a mfo:FeedMessage ;"
	                        "              ?p ?o ."
	                        "              FILTER (?p != rdf:type &&"
	                        "                      ?p != nmo:communicationChannel)"
	                        "}", item_urn, item_urn);
}

static TrackerResource *
feed_message_create_resource (TrackerMinerRSS *miner,
                              GrssFeedItem    *item,
                              const gchar     *item_urn)
{
	time_t t;
	gchar *uri, *time_str;
	const gchar *url;
	GrssPerson *author;
	gdouble latitude;
	gdouble longitude;
	const gchar *tmp_string;
	TrackerResource *resource;
	GrssFeedChannel *channel;
	const GList *contributors;
	const GList *enclosures;
	const GList *list, *l;
	GHashTable *enclosure_urls;

	url = get_message_url (item);
	g_message ("Inserting feed item for '%s'", url);

	resource = tracker_resource_new (item_urn);

	tracker_resource_add_uri (resource, "rdf:type", "mfo:FeedMessage");
	tracker_resource_add_uri (resource, "rdf:type", "nfo:RemoteDataObject");

	if (url != NULL)
		tracker_resource_set_string (resource, "nie:url", url);

	resource = tracker_resource_new (NULL);
	author = grss_feed_item_get_author (item);
	contributors = grss_feed_item_get_contributors (item);
	channel = grss_feed_item_get_parent (item);
	enclosures = grss_feed_item_get_enclosures (item);

	if (grss_feed_item_get_geo_point (item, &latitude, &longitude)) {
		TrackerResource *geolocation;

		g_message ("  Geolocation, using longitude:%f, latitude:%f",
		           longitude, latitude);

		geolocation = tracker_resource_new (NULL);
		tracker_resource_add_uri (geolocation, "rdf:type", "slo:GeoLocation");
		tracker_resource_set_double (geolocation, "slo:latitude", latitude);
		tracker_resource_set_double (geolocation, "slo:longitude", longitude);

		tracker_resource_set_take_relation (resource, "slo:location", geolocation);
	}

	if (author != NULL) {
		g_message ("  Author:'%s'", grss_person_get_name (author));
		sparql_add_contact (resource, "nco:creator", author);
	}

	for (l = contributors; l; l = l->next) {
		g_debug ("  Contributor:'%s'", grss_person_get_name (l->data));
		sparql_add_contact (resource, "nco:contributor", l->data);
	}

	enclosure_urls = g_hash_table_new (g_str_hash, g_str_equal);
	for (l = enclosures; l; l = l->next) {
		const gchar *url;

		url = grss_feed_enclosure_get_url (l->data);

		if (!g_hash_table_contains (enclosure_urls, url)) {
			g_debug ("  Enclosure:'%s'", url);
			g_hash_table_add (enclosure_urls, (gpointer) url);
			sparql_add_enclosure (resource, l->data);

		}
	}
	g_hash_table_destroy (enclosure_urls);

	tmp_string = grss_feed_item_get_title (item);
	if (tmp_string != NULL) {
		g_message ("  Title:'%s'", tmp_string);
		tracker_resource_set_string (resource, "nie:title", tmp_string);
	}

	tmp_string = grss_feed_item_get_description (item);
	if (tmp_string != NULL) {
		gchar *plain_text;

		plain_text = parse_html_text (tmp_string);
		tracker_resource_set_string (resource, "nie:plainTextContent", plain_text);
		g_free (plain_text);

		tracker_resource_set_string (resource, "nmo:htmlMessageContent", tmp_string);
	}

	time_str = tracker_date_to_string (time (NULL));
	tracker_resource_set_string (resource, "nmo:receivedDate", time_str);
	tracker_resource_set_string (resource, "mfo:downloadedTime", time_str);
	g_free (time_str);

	t = grss_feed_item_get_publish_time (item);
	time_str = tracker_date_to_string (t);
	tracker_resource_set_string (resource, "nie:contentCreated", time_str);
	g_free (time_str);

	tracker_resource_set_boolean (resource, "nmo:isRead", FALSE);

	uri = g_object_get_data (G_OBJECT (channel), "subject");
	tracker_resource_add_uri (resource, "nmo:communicationChannel", uri);

	tmp_string = grss_feed_item_get_copyright (item);
	if (tmp_string) {
		tracker_resource_set_string (resource, "nie:copyright", tmp_string);
	}

	list = grss_feed_item_get_categories (item);
	for (l = list; l; l = l->next) {
		tracker_resource_add_string (resource, "nie:keyword", l->data);
	}

	return resource;
}

static void
feed_channel_content_update_cb (GObject      *source,
                                GAsyncResult *result,
                                gpointer      user_data)
{
	TrackerSparqlConnection *connection;
	GPtrArray *array = user_data;
	GError *error = NULL;

	connection = TRACKER_SPARQL_CONNECTION (source);
	if (!tracker_sparql_connection_update_array_finish (connection,
	                                                    result, &error)) {
		g_warning ("Could not update feed items: %s",
		           error->message);
		g_error_free (error);
	}

	g_ptr_array_unref (array);
}

static void
check_feed_items_cb (GObject      *source_object,
                     GAsyncResult *res,
                     gpointer      user_data)
{
	TrackerSparqlConnection *connection;
	TrackerResource *resource;
	FeedItemListInsertData *data;
	TrackerSparqlCursor *cursor;
	GrssFeedItem *item;
	GError *error = NULL;
	GHashTableIter iter;
	GPtrArray *array;
	const gchar *str;

	data = user_data;
	connection = TRACKER_SPARQL_CONNECTION (source_object);
	cursor = tracker_sparql_connection_query_finish (connection, res, &error);
	array = g_ptr_array_new_with_free_func ((GDestroyNotify) g_free);

	while (!error && tracker_sparql_cursor_next (cursor, NULL, &error)) {
		const gchar *urn, *url, *date;
		time_t time;

		urn = tracker_sparql_cursor_get_string (cursor, 0, NULL);
		url = tracker_sparql_cursor_get_string (cursor, 1, NULL);
		date = tracker_sparql_cursor_get_string (cursor, 2, NULL);
		time = (time_t) tracker_string_to_date (date, NULL, NULL);

		item = g_hash_table_lookup (data->items, url);

		if (!item)
			continue;

		if (time <= grss_feed_item_get_publish_time (item)) {
			g_debug ("Item '%s' already up to date", url);
			g_ptr_array_add (array, feed_message_create_update_channel_query (urn, item));
		} else {
			g_debug ("Updating item '%s'", url);

			g_ptr_array_add (array, feed_message_create_delete_properties_query (urn));

			resource = feed_message_create_resource (data->miner,
			                                       item, urn);
			str = tracker_resource_print_sparql_update (resource, NULL, NULL);
			g_ptr_array_add (array, g_strdup (str));
			g_object_unref (resource);
		}

		g_hash_table_remove (data->items, url);
	}

	if (cursor) {
		g_object_unref (cursor);
	}

	if (error) {
		g_message ("Could check feed items, %s", error->message);
		g_error_free (error);
		feed_item_list_insert_data_free (data);
		g_ptr_array_unref (array);
		return;
	}

	g_hash_table_iter_init (&iter, data->items);

	/* Insert all remaining items as new */
	while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &item)) {
		resource = feed_message_create_resource (data->miner,
		                                       item, NULL);
		str = tracker_resource_print_sparql_update (resource, NULL, NULL);
		g_ptr_array_add (array, g_strdup (str));
		g_object_unref (resource);
	}

	if (array->len == 0) {
		feed_item_list_insert_data_free (data);
		g_ptr_array_unref (array);
		return;
	}

	tracker_sparql_connection_update_array_async (tracker_miner_get_connection (TRACKER_MINER (data->miner)),
	                                              (gchar **) array->pdata,
	                                              array->len,
	                                              NULL,
	                                              feed_channel_content_update_cb,
	                                              array);
	feed_channel_change_updated_time (data->miner, data->channel);
	feed_item_list_insert_data_free (data);
}

static void
check_feed_items (TrackerMinerRSS *miner,
                  GrssFeedChannel *channel,
                  GList           *items)
{
	FeedItemListInsertData *data;
	GHashTableIter iter;
	GrssFeedItem *item;
	gboolean first = TRUE;
	const gchar *url;
	GString *query;

	data = feed_item_list_insert_data_new (miner, channel, items);
	g_hash_table_iter_init (&iter, data->items);

	query = g_string_new ("SELECT ?msg nie:url(?msg)"
	                      "       nie:contentCreated(?msg) {"
	                      "  ?msg a rdfs:Resource ."
	                      "       FILTER (nie:url(?msg) IN (");

	while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &item)) {
		if (!first)
			g_string_append_c (query, ',');

		url = get_message_url (item);
		g_string_append_printf (query, "\"%s\"", url);
		first = FALSE;
	}

	g_string_append (query, "))}");

	tracker_sparql_connection_query_async (tracker_miner_get_connection (TRACKER_MINER (miner)),
	                                       query->str, NULL,
	                                       check_feed_items_cb,
	                                       data);
	g_string_free (query, TRUE);
}

static void
update_feed_channel_info (TrackerMinerRSS *miner,
                          GrssFeedChannel *channel)
{
	const gchar *subject, *str;
	GString *update;
	gchar *escaped;
	time_t time;

	g_debug ("Updating mfo:FeedChannel for '%s'",
	         grss_feed_channel_get_title (channel));

	subject = g_object_get_data (G_OBJECT (channel), "subject");
	update = g_string_new ("INSERT OR REPLACE { ");

	str = grss_feed_channel_get_title (channel);
	if (str) {
		escaped = tracker_sparql_escape_string (str);
		g_string_append_printf (update, "<%s> nie:title \"%s\".", subject, escaped);
		g_free (escaped);
	}

	str = grss_feed_channel_get_format (channel);
	if (str) {
		escaped = tracker_sparql_escape_string (str);
		g_string_append_printf (update,
		                        "<%s> mfo:type [ a mfo:FeedType ;"
		                        " mfo:name \"%s\"].",
		                        subject, escaped);
		g_free (escaped);
	}

	str = grss_feed_channel_get_description (channel);
	if (str) {
		escaped = tracker_sparql_escape_string (str);
		g_string_append_printf (update, "<%s> nie:description \"%s\".", subject, escaped);
		g_free (escaped);
	}

	str = grss_feed_channel_get_image (channel);
	if (str) {
		g_string_append_printf (update, "<%s> mfo:image \"%s\".", subject, str);
	}

	str = grss_feed_channel_get_copyright (channel);
	if (str) {
		escaped = tracker_sparql_escape_string (str);
		g_string_append_printf (update, "<%s> nie:copyright \"%s\".", subject, escaped);
		g_free (escaped);
	}

	time = grss_feed_channel_get_publish_time (channel);

	if (time != 0) {
		escaped = tracker_date_to_string (time);
		g_string_append_printf (update, "<%s> nmo:lastMessageDate \"%s\".", subject, escaped);
		g_free (escaped);
	}

	g_string_append (update, "}");

	tracker_sparql_connection_update_async (tracker_miner_get_connection (TRACKER_MINER (miner)),
	                                        update->str,
	                                        NULL, NULL, NULL);
	g_string_free (update, TRUE);
}

static void
feed_ready_cb (GrssFeedsPool   *pool,
               GrssFeedChannel *channel,
               GList           *items,
               gpointer         user_data)
{
	TrackerMinerRSS *miner;
	TrackerMinerRSSPrivate *priv;

	miner = TRACKER_MINER_RSS (user_data);
	priv = TRACKER_MINER_RSS_GET_PRIVATE (miner);

	priv->now_fetching--;

	g_debug ("Feed fetched, %d remaining", priv->now_fetching);

	if (priv->now_fetching <= 0) {
		priv->now_fetching = 0;
		g_object_set (miner, "progress", 1.0, "status", "Idle", NULL);
	}

	if (items == NULL) {
		return;
	}

	update_feed_channel_info (miner, channel);

	g_message ("Verifying channel:'%s' is up to date",
	           grss_feed_channel_get_title (channel));

	check_feed_items (miner, channel, items);
}

static void
feeds_retrieve_cb (GObject      *source_object,
                   GAsyncResult *res,
                   gpointer      user_data)
{
	GList *channels;
	TrackerSparqlCursor *cursor;
	GError *error = NULL;
	TrackerMinerRSSPrivate *priv;
	GrssFeedChannel *chan;

	priv = TRACKER_MINER_RSS_GET_PRIVATE (user_data);
	cursor = tracker_sparql_connection_query_finish (TRACKER_SPARQL_CONNECTION (source_object),
	                                                 res,
	                                                 &error);

	if (error != NULL) {
		g_message ("Could not retrieve feeds, %s", error->message);
		g_error_free (error);
		if (cursor) {
			g_object_unref (cursor);
		}
		return;
	}

	while (tracker_sparql_cursor_next (cursor, NULL, NULL)) {
		const gchar *source;
		const gchar *title;
		const gchar *interval;
		const gchar *subject;
		gint mins, id;

		source = tracker_sparql_cursor_get_string (cursor, 0, NULL);
		title = tracker_sparql_cursor_get_string (cursor, 1, NULL);
		interval = tracker_sparql_cursor_get_string (cursor, 2, NULL);
		subject = tracker_sparql_cursor_get_string (cursor, 3, NULL);
		id = tracker_sparql_cursor_get_integer (cursor, 4);

		g_debug ("Indexing channel '%s'", source);

		if (g_hash_table_lookup (priv->channels, GINT_TO_POINTER (id)))
			continue;

		chan = grss_feed_channel_new ();
		g_object_set_data_full (G_OBJECT (chan),
		                        "subject",
		                        g_strdup (subject),
		                        g_free);
		grss_feed_channel_set_source (chan, g_strdup (source));

		/* TODO How to manage feeds with an update mfo:updateInterval == 0 ?
		 * Here the interval is forced to be at least 1 minute, but perhaps those
		 * elements are to be considered "disabled"
		 */
		mins = strtoull (interval, NULL, 10);
		if (mins <= 0)
			mins = 1;
		grss_feed_channel_set_update_interval (chan, mins);

		g_message ("  '%s' (%s) - update interval of %s minutes",
		           title,
		           source,
		           interval);

		g_hash_table_insert (priv->channels, GINT_TO_POINTER (id), chan);
	}

	if (g_hash_table_size (priv->channels) == 0) {
		g_message ("No feeds set up, nothing more to do");
	}

	channels = g_hash_table_get_values (priv->channels);
	grss_feeds_pool_listen (priv->pool, channels);
	g_list_free (channels);

	g_object_unref (cursor);

	if (g_hash_table_size (priv->channels) == 0) {
		g_object_set (user_data, "progress", 1.0, "status", "Idle", NULL);
	}
}

static void
retrieve_and_schedule_feeds (TrackerMinerRSS *miner,
                             GArray          *channel_ids)
{
	GString *sparql;
	gint i, id;

	g_message ("Retrieving and scheduling feeds...");

	sparql = g_string_new ("SELECT ?url nie:title(?urn) ?interval ?urn tracker:id(?urn)"
	                       "WHERE {"
	                       "  ?urn a mfo:FeedChannel ; "
	                       "         mfo:feedSettings ?settings ; "
	                       "         nie:url ?url . "
	                       "  ?settings mfo:updateInterval ?interval ");

	if (channel_ids && channel_ids->len > 0) {
		g_string_append (sparql, ". FILTER (tracker:id(?urn) IN (");

		for (i = 0; i < channel_ids->len; i++) {
			id = g_array_index (channel_ids, gint, i);
			if (i != 0)
				g_string_append (sparql, ",");
			g_string_append_printf (sparql, "%d", id);
		}

		g_string_append (sparql, "))");
	}

	g_string_append_printf (sparql, "}");

	tracker_sparql_connection_query_async (tracker_miner_get_connection (TRACKER_MINER (miner)),
	                                       sparql->str,
	                                       NULL,
	                                       feeds_retrieve_cb,
	                                       miner);
	g_string_free (sparql, TRUE);
}

static const gchar *
get_message_url (GrssFeedItem *item)
{
	const gchar *url;

	grss_feed_item_get_real_source (item, &url, NULL);
	if (url == NULL)
		url = grss_feed_item_get_source (item);
	return url;
}

static void
miner_started (TrackerMiner *miner)
{
	TrackerMinerRSSPrivate *priv;

	g_object_set (miner, "progress", 0.0, "status", "Initializing", NULL);

	priv = TRACKER_MINER_RSS_GET_PRIVATE (miner);
	retrieve_and_schedule_feeds (TRACKER_MINER_RSS (miner), NULL);
	grss_feeds_pool_switch (priv->pool, TRUE);
}

static void
miner_stopped (TrackerMiner *miner)
{
	TrackerMinerRSSPrivate *priv;

	priv = TRACKER_MINER_RSS_GET_PRIVATE (miner);
	grss_feeds_pool_switch (priv->pool, FALSE);
	g_object_set (miner, "progress", 1.0, "status", "Idle", NULL);
}

static void
miner_paused (TrackerMiner *miner)
{
	TrackerMinerRSSPrivate *priv;

	priv = TRACKER_MINER_RSS_GET_PRIVATE (miner);
	grss_feeds_pool_switch (priv->pool, FALSE);

	/* Save last status */
	g_free (priv->last_status);
	g_object_get (miner, "status", &priv->last_status, NULL);

	/* Set paused */
	g_object_set (miner, "status", "Paused", NULL);
}

static void
miner_resumed (TrackerMiner *miner)
{
	TrackerMinerRSSPrivate *priv;

	priv = TRACKER_MINER_RSS_GET_PRIVATE (miner);
	grss_feeds_pool_switch (priv->pool, TRUE);

	/* Resume */
	g_object_set (miner, "status", priv->last_status ? priv->last_status : "Idle", NULL);
}

TrackerMinerRSS *
tracker_miner_rss_new (TrackerSparqlConnection  *connection,
                       GError                  **error)
{
	return g_initable_new (TRACKER_TYPE_MINER_RSS,
	                       NULL,
	                       error,
	                       "connection", connection,
	                       NULL);
}
