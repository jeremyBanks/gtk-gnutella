/*
 * Copyright (c) 2001-2003, Richard Eckart
 *
 * THIS FILE IS AUTOGENERATED! DO NOT EDIT!
 * This file is generated from gnet_props.ag using autogen.
 * Autogen is available at http://autogen.sourceforge.net/.
 *
 *----------------------------------------------------------------------
 * This file is part of gtk-gnutella.
 *
 *  gtk-gnutella is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  gtk-gnutella is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with gtk-gnutella; if not, write to the Free Software
 *  Foundation, Inc.:
 *      59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *----------------------------------------------------------------------
 */

#ifndef _gnet_property_h_
#define _gnet_property_h_

#include "lib/prop.h"

#define GNET_PROPERTY_MIN ((NO_PROP+1))
#define GNET_PROPERTY_MAX ((NO_PROP+1)+GNET_PROPERTY_END-1)
#define GNET_PROPERTY_NUM (GNET_PROPERTY_END-(NO_PROP+1))

typedef enum {
    PROP_READING_HOSTFILE=(NO_PROP+1),
    PROP_READING_ULTRAFILE,
    PROP_ANCIENT_VERSION,
    PROP_NEW_VERSION_STR,
    PROP_UP_CONNECTIONS,
    PROP_NORMAL_CONNECTIONS,
    PROP_MAX_CONNECTIONS,
    PROP_NODE_LEAF_COUNT,
    PROP_NODE_NORMAL_COUNT,
    PROP_NODE_ULTRA_COUNT,
    PROP_MAX_DOWNLOADS,
    PROP_MAX_HOST_DOWNLOADS,
    PROP_MAX_UPLOADS,
    PROP_MAX_UPLOADS_IP,
    PROP_LOCAL_IP,
    PROP_CURRENT_IP_STAMP,
    PROP_AVERAGE_IP_UPTIME,
    PROP_START_STAMP,
    PROP_AVERAGE_SERVENT_UPTIME,
    PROP_LISTEN_PORT,
    PROP_FORCED_LOCAL_IP,
    PROP_CONNECTION_SPEED,
    PROP_COMPUTE_CONNECTION_SPEED,
    PROP_QUERY_RESPONSE_MAX_ITEMS,
    PROP_UL_USAGE_MIN_PERCENTAGE,
    PROP_DOWNLOAD_CONNECTING_TIMEOUT,
    PROP_DOWNLOAD_PUSH_SENT_TIMEOUT,
    PROP_DOWNLOAD_CONNECTED_TIMEOUT,
    PROP_DOWNLOAD_RETRY_TIMEOUT_MIN,
    PROP_DOWNLOAD_RETRY_TIMEOUT_MAX,
    PROP_DOWNLOAD_MAX_RETRIES,
    PROP_DOWNLOAD_RETRY_TIMEOUT_DELAY,
    PROP_DOWNLOAD_RETRY_BUSY_DELAY,
    PROP_DOWNLOAD_RETRY_REFUSED_DELAY,
    PROP_DOWNLOAD_RETRY_STOPPED_DELAY,
    PROP_DOWNLOAD_OVERLAP_RANGE,
    PROP_UPLOAD_CONNECTING_TIMEOUT,
    PROP_UPLOAD_CONNECTED_TIMEOUT,
    PROP_SEARCH_REISSUE_TIMEOUT,
    PROP_BAN_RATIO_FDS,
    PROP_BAN_MAX_FDS,
    PROP_BANNED_COUNT,
    PROP_MAX_BANNED_FD,
    PROP_INCOMING_CONNECTING_TIMEOUT,
    PROP_NODE_CONNECTING_TIMEOUT,
    PROP_NODE_CONNECTED_TIMEOUT,
    PROP_NODE_SENDQUEUE_SIZE,
    PROP_NODE_TX_FLOWC_TIMEOUT,
    PROP_NODE_RX_FLOWC_RATIO,
    PROP_MAX_TTL,
    PROP_MY_TTL,
    PROP_HARD_TTL_LIMIT,
    PROP_DBG,
    PROP_LIB_DEBUG,
    PROP_BITZI_DEBUG,
    PROP_GWC_DEBUG,
    PROP_URL_DEBUG,
    PROP_DH_DEBUG,
    PROP_DQ_DEBUG,
    PROP_VMSG_DEBUG,
    PROP_QUERY_DEBUG,
    PROP_SEARCH_DEBUG,
    PROP_UDP_DEBUG,
    PROP_QRP_DEBUG,
    PROP_ROUTING_DEBUG,
    PROP_GGEP_DEBUG,
    PROP_PCACHE_DEBUG,
    PROP_HSEP_DEBUG,
    PROP_TRACK_PROPS,
    PROP_STOP_HOST_GET,
    PROP_BW_HTTP_IN_ENABLED,
    PROP_BW_HTTP_OUT_ENABLED,
    PROP_BW_GNET_IN_ENABLED,
    PROP_BW_GNET_LEAF_IN_ENABLED,
    PROP_BW_GNET_OUT_ENABLED,
    PROP_BW_GNET_LEAF_OUT_ENABLED,
    PROP_BW_UL_USAGE_ENABLED,
    PROP_BW_ALLOW_STEALING,
    PROP_AUTOCLEAR_COMPLETED_DOWNLOADS,
    PROP_AUTOCLEAR_FAILED_DOWNLOADS,
    PROP_AUTOCLEAR_UNAVAILABLE_DOWNLOADS,
    PROP_SEARCH_REMOVE_DOWNLOADED,
    PROP_FORCE_LOCAL_IP,
    PROP_USE_NETMASKS,
    PROP_ALLOW_PRIVATE_NETWORK_CONNECTION,
    PROP_USE_IP_TOS,
    PROP_DOWNLOAD_DELETE_ABORTED,
    PROP_PROXY_AUTH,
    PROP_SOCKS_USER,
    PROP_SOCKS_PASS,
    PROP_PROXY_IP,
    PROP_PROXY_HOSTNAME,
    PROP_PROXY_PORT,
    PROP_PROXY_PROTOCOL,
    PROP_HOSTS_IN_CATCHER,
    PROP_HOSTS_IN_ULTRA_CATCHER,
    PROP_HOSTS_IN_BAD_CATCHER,
    PROP_MAX_HOSTS_CACHED,
    PROP_MAX_ULTRA_HOSTS_CACHED,
    PROP_MAX_BAD_HOSTS_CACHED,
    PROP_MAX_HIGH_TTL_MSG,
    PROP_MAX_HIGH_TTL_RADIUS,
    PROP_BW_HTTP_IN,
    PROP_BW_HTTP_OUT,
    PROP_BW_GNET_IN,
    PROP_BW_GNET_OUT,
    PROP_BW_GNET_LIN,
    PROP_BW_GNET_LOUT,
    PROP_SEARCH_QUERIES_FORWARD_SIZE,
    PROP_SEARCH_QUERIES_KICK_SIZE,
    PROP_SEARCH_ANSWERS_FORWARD_SIZE,
    PROP_SEARCH_ANSWERS_KICK_SIZE,
    PROP_OTHER_MESSAGES_KICK_SIZE,
    PROP_HOPS_RANDOM_FACTOR,
    PROP_SEND_PUSHES,
    PROP_MIN_DUP_MSG,
    PROP_MIN_DUP_RATIO,
    PROP_SCAN_EXTENSIONS,
    PROP_SCAN_IGNORE_SYMLINK_DIRS,
    PROP_SCAN_IGNORE_SYMLINK_REGFILES,
    PROP_SAVE_FILE_PATH,
    PROP_MOVE_FILE_PATH,
    PROP_BAD_FILE_PATH,
    PROP_SHARED_DIRS_PATHS,
    PROP_LOCAL_NETMASKS_STRING,
    PROP_TOTAL_DOWNLOADS,
    PROP_UL_RUNNING,
    PROP_UL_REGISTERED,
    PROP_TOTAL_UPLOADS,
    PROP_SERVENT_GUID,
    PROP_USE_SWARMING,
    PROP_USE_AGGRESSIVE_SWARMING,
    PROP_DL_MINCHUNKSIZE,
    PROP_DL_MAXCHUNKSIZE,
    PROP_AUTO_DOWNLOAD_IDENTICAL,
    PROP_AUTO_FEED_DOWNLOAD_MESH,
    PROP_STRICT_SHA1_MATCHING,
    PROP_USE_FUZZY_MATCHING,
    PROP_FUZZY_THRESHOLD,
    PROP_IS_FIREWALLED,
    PROP_IS_INET_CONNECTED,
    PROP_IS_UDP_FIREWALLED,
    PROP_RECV_SOLICITED_UDP,
    PROP_GNET_COMPACT_QUERY,
    PROP_DOWNLOAD_OPTIMISTIC_START,
    PROP_LIBRARY_REBUILDING,
    PROP_SHA1_REBUILDING,
    PROP_SHA1_VERIFYING,
    PROP_FILE_MOVING,
    PROP_PREFER_COMPRESSED_GNET,
    PROP_ONLINE_MODE,
    PROP_DOWNLOAD_REQUIRE_URN,
    PROP_DOWNLOAD_REQUIRE_SERVER_NAME,
    PROP_MAX_ULTRAPEERS,
    PROP_QUICK_CONNECT_POOL_SIZE,
    PROP_MAX_LEAVES,
    PROP_SEARCH_HANDLE_IGNORED_FILES,
    PROP_CONFIGURED_PEERMODE,
    PROP_CURRENT_PEERMODE,
    PROP_SYS_NOFILE,
    PROP_SYS_PHYSMEM,
    PROP_DL_QUEUE_COUNT,
    PROP_DL_RUNNING_COUNT,
    PROP_DL_ACTIVE_COUNT,
    PROP_DL_AQUEUED_COUNT,
    PROP_DL_PQUEUED_COUNT,
    PROP_FI_ALL_COUNT,
    PROP_FI_WITH_SOURCE_COUNT,
    PROP_DL_QALIVE_COUNT,
    PROP_DL_BYTE_COUNT,
    PROP_UL_BYTE_COUNT,
    PROP_PFSP_SERVER,
    PROP_PFSP_FIRST_CHUNK,
    PROP_FUZZY_FILTER_DMESH,
    PROP_CRAWLER_VISIT_COUNT,
    PROP_UDP_CRAWLER_VISIT_COUNT,
    PROP_HOST_RUNS_NTP,
    PROP_NTP_DETECTED,
    PROP_CLOCK_SKEW,
    PROP_NODE_MONITOR_UNSTABLE_IP,
    PROP_NODE_MONITOR_UNSTABLE_SERVENTS,
    PROP_DL_REMOVE_FILE_ON_MISMATCH,
    PROP_DL_MISMATCH_BACKOUT,
    PROP_SERVER_HOSTNAME,
    PROP_GIVE_SERVER_HOSTNAME,
    PROP_RESERVE_GTKG_NODES,
    PROP_UNIQUE_NODES,
    PROP_DOWNLOAD_RX_SIZE,
    PROP_NODE_RX_SIZE,
    PROP_DL_HTTP_LATENCY,
    PROP_NODE_LAST_ULTRA_CHECK,
    PROP_NODE_LAST_ULTRA_LEAF_SWITCH,
    PROP_UP_REQ_AVG_SERVENT_UPTIME,
    PROP_UP_REQ_AVG_IP_UPTIME,
    PROP_UP_REQ_NODE_UPTIME,
    PROP_UP_REQ_NOT_FIREWALLED,
    PROP_UP_REQ_ENOUGH_CONN,
    PROP_UP_REQ_ENOUGH_FD,
    PROP_UP_REQ_ENOUGH_MEM,
    PROP_UP_REQ_ENOUGH_BW,
    PROP_SEARCH_QUEUE_SIZE,
    PROP_SEARCH_QUEUE_SPACING,
    PROP_ENABLE_SHELL,
    PROP_ENTRY_REMOVAL_TIMEOUT,
    PROP_NODE_WATCH_SIMILAR_QUERIES,
    PROP_NODE_QUERIES_HALF_LIFE,
    PROP_NODE_REQUERY_THRESHOLD,
    PROP_LIBRARY_RESCAN_TIMESTAMP,
    PROP_LIBRARY_RESCAN_TIME,
    PROP_QRP_INDEXING_TIMESTAMP,
    PROP_QRP_INDEXING_TIME,
    PROP_QRP_TIMESTAMP,
    PROP_QRP_COMPUTATION_TIME,
    PROP_QRP_PATCH_TIMESTAMP,
    PROP_QRP_PATCH_COMPUTATION_TIME,
    PROP_QRP_GENERATION,
    PROP_QRP_SLOTS,
    PROP_QRP_SLOTS_FILLED,
    PROP_QRP_FILL_RATIO,
    PROP_QRP_CONFLICT_RATIO,
    PROP_QRP_HASHED_KEYWORDS,
    PROP_QRP_PATCH_RAW_LENGTH,
    PROP_QRP_PATCH_LENGTH,
    PROP_QRP_PATCH_COMP_RATIO,
    PROP_ANCIENT_VERSION_FORCE,
    PROP_ANCIENT_VERSION_LEFT_DAYS,
    PROP_FILE_DESCRIPTOR_SHORTAGE,
    PROP_FILE_DESCRIPTOR_RUNOUT,
    PROP_ENABLE_G2_SUPPORT,
    PROP_CONVERT_SPACES,
    PROP_CONVERT_EVIL_CHARS,
    PROP_CONVERT_OLD_FILENAMES,
    PROP_TLS_ENFORCE,
    PROP_GNET_DEFLATE_ENABLED,
    PROP_ENABLE_UDP,
    PROP_PROCESS_OOB_QUERIES,
    PROP_SEND_OOB_QUERIES,
    PROP_PROXY_OOB_QUERIES,
    PROP_UPLOADS_STALLING,
    PROP_ALLOW_AUTO_REQUERIES,
    PROP_USE_GLOBAL_HOSTILES_TXT,
    PROP_USE_SO_LINGER,
    PROP_BROWSE_HOST_ENABLED,
    GNET_PROPERTY_END
} gnet_property_t;

/*
 * Property set stub
 */
prop_set_stub_t *gnet_prop_get_stub(void);

/*
 * Property definition
 */
prop_def_t *gnet_prop_get_def(property_t);
property_t gnet_prop_get_by_name(const gchar *);
GSList *gnet_prop_get_by_regex(const gchar *, gint *);
const gchar *gnet_prop_name(property_t);
const gchar *gnet_prop_description(property_t);
void gnet_prop_set_from_string(property_t, const gchar *);

/*
 * Property-change listeners
 */
void gnet_prop_add_prop_changed_listener(
    property_t, prop_changed_listener_t, gboolean);
void gnet_prop_remove_prop_changed_listener(
    property_t, prop_changed_listener_t);

/*
 * get/set functions
 *
 * The *_val macros are shortcuts for single scalar properties.
 */
void gnet_prop_set_boolean(
    property_t, const gboolean *, size_t, size_t);
gboolean *gnet_prop_get_boolean(
    property_t, gboolean *, size_t, size_t);

#define gnet_prop_set_boolean_val(p, v) do { \
	gboolean value = v; \
	gnet_prop_set_boolean(p, &value, 0, 1); \
} while (0)

#define gnet_prop_get_boolean_val(p, v) do { \
	gnet_prop_get_boolean(p, v, 0, 1); \
} while (0)


void gnet_prop_set_string(property_t, const gchar *);
gchar *gnet_prop_get_string(property_t, gchar *, size_t);

void gnet_prop_set_guint32(
    property_t, const guint32 *, size_t, size_t);
guint32 *gnet_prop_get_guint32(
    property_t, guint32 *, size_t, size_t);

#define gnet_prop_set_guint32_val(p, v) do { \
	guint32 value = v; \
	gnet_prop_set_guint32(p, &value, 0, 1); \
} while (0)

#define gnet_prop_get_guint32_val(p, v) do { \
	gnet_prop_get_guint32(p, v, 0, 1); \
} while (0)

void gnet_prop_set_guint64(
    property_t, const guint64 *, size_t, size_t);
guint64 *gnet_prop_get_guint64(
    property_t, guint64 *, size_t, size_t);

#define gnet_prop_set_guint64_val(p, v) do { \
	guint64 value = v; \
	gnet_prop_set_guint64(p, &value, 0, 1); \
} while (0)

#define gnet_prop_get_guint64_val(p, v) do { \
	gnet_prop_get_guint64(p, v, 0, 1); \
} while (0)

void gnet_prop_set_storage(property_t, const gchar *, size_t);
gchar *gnet_prop_get_storage(property_t, gchar *, size_t);

const gchar *gnet_prop_to_string(property_t prop);

#endif /* _gnet_property_h_ */

