/*
 * Copyright (c) 2003, Jeroen Asselman & Raphael Manfredi
 *
 * Passive/Active Remote Queuing.
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

#include "common.h"		/* For -DUSE_DMALLOC */

#include <ctype.h>
#include <glib.h>
#include "parq.h"
#include "ioheader.h"
#include "sockets.h"
#include "gnutella.h"
#include "settings.h"
#include "downloads.h"
#include "guid.h"
#include "gnet_property.h"

RCSID("$Id$");

#define PARQ_VERSION_MAJOR	1
#define PARQ_VERSION_MINOR	0

#define AGRESSIVE 0

#define PARQ_RETRY_SAFETY	40		/* 40 seconds before lifetime */
#define PARQ_TIMER_BY_POS	30		/* 30 seconds for each queue position */
#define PARQ_MAX_UL_RETRY_DELAY 1200	/* 20 minutes retry rate max. */
#define MIN_LIFE_TIME		90
#define EXPIRE_GRACE_TIME	90
#define QUEUE_PERIOD		600		/* Try to resend a queue every 10 minutes */
#define MAX_QUEUE			144		/* Max amount of QUEUE we can send */
#define MAX_QUEUE_REFUSED	2		/* Max QUEUE they can refuse in a row */

#define MAX_UPLOADS			100		/* Avoid more than that many uploads */
#define MAX_UPLOAD_QSIZE	4000	/* Size of the PARQ queue */

#define MIBI (1024 * 1024)
/*
 * Queues:
 *
 * 1 ul: 0 < q1
 * 2 ul: 0 < q1 < 300, 300 < q2 < oo
 * 3 ul: 0 < q1 < 150, 150 < q2 < 300, 300 < q2 < oo
 */
#define PARQ_UL_LARGE_SIZE (300 * MIBI)

#define PARQ_UL_MAGIC	0x6a3900a1

GHashTable *dl_all_parq_by_id = NULL;

guint parq_max_upload_size = MAX_UPLOAD_QSIZE;
guint parq_upload_active_size = 20; /* Number of active upload slots per queue*/
guint parq_upload_ban_window = 600;
static const gchar *file_parq_file = "parq";

GList *ul_parqs = NULL;			/* List of all queued uploads */
GList *ul_parq_queue = NULL;	/* To whom we need to send a QUEUE */
GHashTable *ul_all_parq_by_ip_and_name = NULL;
GHashTable *ul_all_parq_by_ip = NULL;
GHashTable *ul_all_parq_by_id = NULL;
gboolean enable_real_passive = TRUE;	/* If TRUE, a dead upload is only marked
										 * dead, if FALSE, a dead upload is 
										 * really removed and cannot reclaim
										 * its position */

 
/*
 * Holds status of current queue.
 */
struct parq_ul_queue {
	GList *by_position;		/* Queued items sorted on position. Newest is 
							   added to the end. */
	GList *by_rel_pos;		/* Queued items sorted by relative position */
	GList *by_date_dead;	/* Queued items sorted on last update and 
							   not alive */
	gint size;				/* Number of entries in current list */
	
	gboolean active;		/* Set to false when the number of upload slots
							   was decreased but the queue still contained
							   queued items. This queue shall be removed when
							   all queued items are finished / removed. */
	gint active_uploads;
	gint alive;
};

struct parq_ul_queued_by_ip {
	gint	uploading;		/* Number of uploads uploading */
	gint	total;			/* Total queued items for this ip */
	guint32 ip;
	GList	*list;			/* List or queued items for this ip */
};

/* Contains the queued upload */
struct parq_ul_queued {	
	guint32 magic;			/* Magic number */
	guint32 flags;			/* Operating flags */
	guint position;			/* Current position in the queue */
	guint relative_position; /* Relative position in the queue, if 'not alive' 
							  uploads are taken into account */
	gboolean has_slot;		/* Whether the items is currently uploading */
	gboolean had_slot;		/* If an upload had an upload slot it is not allowed
							   to reuse the id for another upload	*/
	guint eta;				/* Expected time in seconds till an upload slot is
							   reached, this is a relative timestamp */

	time_t expire;			/* Time at which the queue position will be lost */
	time_t retry;			/* Time at which the first retry-after is expected*/
	time_t enter;			/* Time upload entered parq */
	time_t updated;			/* Time last upload request was sent */
	time_t ban_timeout;		/* Time after which we won't kick out the upload out
							   of the queue when retry isn't obeyed */
	time_t disc_timeout;	/* Time after which we allow the upload to be
							   disconnected again. */

	time_t last_queue_sent;	/* When we last sent the QUEUE */

	guint32 queue_sent;		/* Amount of QUEUE messages we tried to send */
	guint32 queue_refused;	/* Amount of QUEUE messages refused remotely */
	 
	gboolean is_alive;		/* Whether client is still requesting this file */

	gchar *id;				/* PARQ identifier */
	 
	gchar *ip_and_name;		/* "IP name", used as key in hash table */
	gchar *name;			/* NB: points directly into `ip_and_name' */
	guint32 remote_ip;		/* IP address of the socket endpoint */
	
	guint32 file_size;		/* Needed to recalculate ETA */
	guint32 chunk_size;		/* Requested chunk size */
	guint32 ip;				/* Contact IP:port, as read from X-Node: */
	guint16 port;

	gint major;
	gint minor;
	
	struct parq_ul_queue *queue;	/* In which queue this entry is listed */
	struct parq_ul_queued_by_ip *by_ip;
};

/*
 * Flags for parq_ul_queued
 */

#define PARQ_UL_QUEUE		0x00000001	/* Scheduled for QUEUE sending */
#define PARQ_UL_NOQUEUE		0x00000002	/* No IP:port, don't send QUEUE */
#define PARQ_UL_QUEUE_SENT	0x00000004	/* QUEUE message sent */
#define PARQ_UL_ID_SENT		0x00000008	/* We already sent an ID */

/* Contains the queued download status */
struct parq_dl_queued {	
	guint position;			/* Current position in the queue */
	guint length;			/* Current queue length */
	time_t eta;				/* Estimated time till upload slot retrieved */
	guint lifetime;			/* Max interval before loosing queue position */
	guint retry_delay;		/* Interval between new attempt */
	gchar *id;				/* PARQ Queue ID, +1 for trailing NUL */
};


void parq_dl_del_id(struct download *d);

static void parq_upload_free(struct parq_ul_queued *parq_ul);
static struct parq_ul_queued *parq_upload_create(gnutella_upload_t *u);
static struct parq_ul_queue *parq_upload_which_queue(gnutella_upload_t *u);
static struct parq_ul_queue *parq_upload_new_queue();
static void parq_upload_free_queue(struct parq_ul_queue *queue);
static void parq_upload_update_eta(struct parq_ul_queue *which_ul_queue);
static struct parq_ul_queued *parq_upload_find(gnutella_upload_t *u);
static gint parq_ul_rel_pos_cmp(gconstpointer a, gconstpointer b);
static gboolean parq_upload_continue(
		struct parq_ul_queued *uq, gint free_slots);
static void parq_upload_decrease_all_after(struct parq_ul_queued *cur_parq_ul);
static void parq_store(gpointer data, gpointer x);
static void parq_upload_load_queue();
static void parq_upload_update_relative_position(
		struct parq_ul_queued *parq_ul);
static void parq_upload_update_ip_and_name(struct parq_ul_queued *parq_ul, 
	gnutella_upload_t *u);

static void parq_upload_send_queue(struct parq_ul_queued *parq_ul);
static void parq_upload_do_send_queue(struct parq_ul_queued *parq_ul);

/***
 ***  Generic non PARQ specific functions
 ***/

/*
 * get_header_version
 * 
 * Extract the version from a given header. EG:
 * X-Queue: 1.0
 * major=1 minor=0
 */
static gboolean get_header_version(gchar const *const header, 
								gint *major, gint *minor)
{
	return sscanf(header, "%d.%d", major, minor) == 2;
}

/* 
 * get_header_value
 *
 * Retrieves a value from a header line. If possible the length (in gchars)
 * is returned for that value.
 */
static gchar *get_header_value(
	gchar *const s, gchar const *const attribute, gint *length)
{
	gchar *header = s;
	gchar *end;
	gboolean found_right_attribute = FALSE;
	gboolean found_equal_sign = FALSE;
	
	size_t attrlen;
	
	g_assert(s != NULL);
	g_assert(attribute != NULL);

	attrlen = strlen(attribute);

	/*
	 * When we are looking for "foo", make sure we aren't actually
	 * parsing "barfoobar". There should be at least a space, or a
	 * delimiter at the end and at the beginning.
	 */

	do {
		gchar e;
		gchar b;
		gchar es;

		header = strcasestr(header, attribute);
		
		if (header == NULL)
			return NULL;

		e = header[attrlen];		/* End char after attribute */
		
		if (header == s) {
			/*
			 * This is actually the first value of the header. And it
			 * started at position '0'. Which is the same as were
			 * s pointed too. Only check to see if the end is correct
			 */

			found_right_attribute = e == ' ' || e == '=' || e == '\0';
		} else {
			b = *(header - 1);	/* Character before attribute */
			found_right_attribute = (
					b == ';' || b == ',' || b == ':' || b == ' '
				) && (
					e == ' ' || e == '=' || e == '\0'
				);
		}

		/* 
		 * If we weren't looking at the right value. Move on to the next.
		 * If there are no valid values, the while loop will abort with 
		 * lowercase_header == NULL
		 * If we did find a valid position we want to make sure the next
		 * char is an '='. So we need to move ahead anyway.
		 */
		
		header += attrlen;
		
		if (found_right_attribute) {
			
			/*
			 * OK, so we found a possible valid attribute. Now make sure the
			 * first character is an '=', ignoring white spaces.
			 * If we don't, we didn't find a valid attribute.
			 */
			
			es = *header;
			
			do {
				found_right_attribute = es == '=' || es == ' ' || es == '\0';
				found_equal_sign = es == '=';
								
				if (!found_equal_sign)
					es = *(++header);		/* Skip spaces */
				
			} while (!found_equal_sign && found_right_attribute && es != '\0');

			/*
			 * If we did not find the right attribute, it means we did not
			 * encounter an '=' sign before the start of the next attribute
			 * or the end of the string.
			 *
			 * For instance, we stumbled on `var2' in:
			 *
			 *   var1 = foo; var2 ; var3=bar
			 *
			 * Clearly, this is incorrect for our purposes, as all attributes
			 * are expected to have a value.
			 */

			g_assert(!found_equal_sign || found_right_attribute);

			if (!found_right_attribute) {
				g_assert(!found_equal_sign);
				g_warning(__FILE__ ": attribute '%s' has no value in string: %s",
					attribute, s);
			}
		}		
	} while (!found_right_attribute);	
	
	g_assert(header != NULL);
	g_assert(found_equal_sign);
	g_assert(*header == '=');
	
	header++;			/* Skip the '=' sign */

	/*
	 * If we need to compute the length of the attribute's value, look for
	 * the next trailing delimiter (';' or ',').
	 */
	
	if (length != NULL) {
		*length = 0;

		end = strchr(header, ';');		/* PARQ style */
		if (end == NULL)
			end = strchr(header, ',');	/* Active queuing style */

		/* 
		 * If we couldn't find a delimiter, then this value is the last one.
		 */

		*length = (end == NULL) ?
			strlen(header) : end - header;
	}

	return header;
}


/*
 * parq_upload_queue_init
 *
 * Initialises the upload queue for PARQ.
 */
void parq_init(void)
{
	ul_all_parq_by_ip_and_name = g_hash_table_new(g_str_hash, g_str_equal);
	ul_all_parq_by_ip = g_hash_table_new(g_int_hash, g_int_equal);
	ul_all_parq_by_id = g_hash_table_new(g_str_hash, g_str_equal);
	dl_all_parq_by_id = g_hash_table_new(g_str_hash, g_str_equal);

	(void) parq_upload_new_queue();
	
	g_assert(ul_all_parq_by_ip_and_name != NULL);
	g_assert(ul_all_parq_by_id != NULL);
	g_assert(ul_all_parq_by_ip != NULL);
	g_assert(dl_all_parq_by_id != NULL);
	
	parq_upload_load_queue();
}

/*
 * parq_close
 *
 * Saves any queueing information and frees all memory used by PARQ
 */

void parq_close(void)
{
	GList *queues;
	GList *dl;
	GSList *sl;
	GSList *remove = NULL;
	GSList *removeq = NULL;

	parq_upload_save_queue();
	
	/* 
	 * First locate all queued items (dead or alive). And place them in the
	 * 'to be removed' list.
	 */
	for (queues = ul_parqs ; queues != NULL; queues = queues->next) {
		struct parq_ul_queue *queue = (struct parq_ul_queue *) queues->data;

		for (dl = queue->by_position; dl != NULL; dl = dl->next) {	
			struct parq_ul_queued *parq_ul = (struct parq_ul_queued *) dl->data;

			if (parq_ul == NULL)
				break;
			
			parq_ul->by_ip->uploading = 0;
			
			remove = g_slist_prepend(remove, parq_ul);		
		}
		
		removeq = g_slist_prepend(removeq, queue);
	}

	/* Free all memory used by queued items */
	for (sl = remove; sl != NULL; sl = g_slist_next(sl))
		parq_upload_free((struct parq_ul_queued *) sl->data);
	
	g_slist_free(remove);

	for (sl = removeq; sl != NULL; sl = g_slist_next(sl)) {
		struct parq_ul_queue *queue = (struct parq_ul_queue *)sl->data;
			
		/* 
		 * We didn't decrease the active_uploads counters when we were freeing
		 * we don't care about this information anymore anyway.
		 * Set the queue inactive to avoid an assertion
		 */
		queue->active_uploads = 0;
		queue->active = FALSE;
		parq_upload_free_queue(queue);
	}
		
	g_slist_free(removeq);

}

/***
 ***  The following section contains download PARQ functions
 ***/

/*
 * get_parq_dl_id
 *
 * Retreives the PARQ ID associated with an download.
 * Returns a gchar pointer to the ID, or NULL if no ID is available.
 */
gchar *get_parq_dl_id(const struct download *d)
{
	g_assert(d != NULL);

	if (d->queue_status == NULL)
		return NULL;
		
	return ((struct parq_dl_queued *) d->queue_status)->id;
}

/*
 * get_parq_dl_position
 *
 * Retreives the remote queued position associated with an download.
 * Returns the remote queued position or 0 if download is not queued or queuing
 *     status is unknown
 */
gint get_parq_dl_position(const struct download *d)
{
	g_assert(d != NULL);
	
	if (d->queue_status == NULL)
		return 0;
	
	return ((struct parq_dl_queued *) d->queue_status)->position;
}

/*
 * get_parq_dl_queue_length
 *
 * Retreives the remote queue size associated with an download.
 * Returns the remote queue size or 0 if download is not queued or queueing 
 *     status is unknown.
 */
gint get_parq_dl_queue_length(const struct download *d)
{
	g_assert(d != NULL);
	
	if (d->queue_status == NULL)
		return 0;
	
	return ((struct parq_dl_queued *) d->queue_status)->length;
}

/*
 * get_parq_dl_eta
 *
 * Retreives the estimated time of arival for a queued download.
 * Returns the relative eta or 0 if download is not queued or queuing status is
 *     unknown.
 */
gint get_parq_dl_eta(const struct download *d)
{
	g_assert(d != NULL);
	
	if (d->queue_status == NULL)
		return 0;
	
	return ((struct parq_dl_queued *) d->queue_status)->eta;
}

/*
 * get_parq_dl_retry_delay
 * 
 * Retreives the retry rate at which a queued download should retry.
 * Returns the retry rate or 0 if download is not queued or queueing status is
 *     unknown.
 */
gint get_parq_dl_retry_delay(const struct download *d)
{
	g_assert(d != NULL);
	
	if (d->queue_status == NULL)
		return 0;

	return ((struct parq_dl_queued *) d->queue_status)->retry_delay;
}

/*
 * parq_download_retry_active_queued
 *
 * Active queued means we didn't close the http connection on a HTTP 503 busy
 * when the server supports queueing. So prepare the download structure
 * for a 'valid' segment. And re-request the segment.
 */
void parq_download_retry_active_queued(struct download *d)
{
	g_assert(d != NULL);
	g_assert(d->socket != NULL);
	g_assert(d->status == GTA_DL_ACTIVE_QUEUED);
	g_assert(d->queue_status != NULL);
	g_assert(parq_download_is_active_queued(d));
	
	if (download_start_prepare_running(d)) {
		struct gnutella_socket *s = d->socket;
		d->keep_alive = TRUE;			/* was reset in start_prepare_running */
		
 		/* Will be re initialised in download_send_request */
		io_free(d->io_opaque);
		d->io_opaque = NULL;
		getline_free(s->getline);		/* No longer need this */
		s->getline = NULL;

		/* Resend request for download */
		download_send_request(d);
	}
}
	
/*
 * get_integer
 *
 * Convenience wrapper on top of strtoul().
 * Returns parsed integer (base 10), or 0 if none could be found.
 */
static gint get_integer(const gchar *buf)
{
	gulong val;
	gchar *end;

	/* XXX This needs to get more parameters, so that we can log the
	 * XXX problem if we cannot parse, or if the value does not fit.
	 * XXX We probably need the download structure, and the name of
	 * XXX the field being parsed, with the header line as well.
	 * XXX	--RAM, 02/02/2003.
	 */

	val = strtoul(buf, &end, 10);
	if (end == buf)
		return 0;

	if (val > INT_MAX)
		val = INT_MAX;

	return (gint) val;
}

/*
 * parq_dl_remove
 *
 * Tells the parq logic that a download has been removed. If parq has
 * associated a queue structure with this download it will be freed.
 */
void parq_dl_remove(struct download *d)
{
	if (d->queue_status != NULL)
		parq_dl_free(d);
}

/*
 * parq_dl_free
 * 
 * Removes the queue information for a download from memory.
 */
void parq_dl_free(struct download *d)
{
	struct parq_dl_queued* parq_dl = NULL;
		
	parq_dl = (struct parq_dl_queued *) d->queue_status;
	
	if (parq_dl->id != NULL)
		parq_dl_del_id(d);

	g_assert(parq_dl->id == NULL);

	wfree(parq_dl, sizeof(struct parq_dl_queued));
	
	parq_dl = NULL;
	d->queue_status = NULL;
	
	g_assert(d->queue_status == NULL);
}

/*
 * parq_dl_create
 *
 * Creates a queue structure for a download.
 * Returns a parq_dl_queued pointer to the newly created structure.
 */
gpointer parq_dl_create(struct download *d)
{
	struct parq_dl_queued* parq_dl = NULL;
	
	g_assert(d->queue_status == NULL);
	
	parq_dl = walloc(sizeof(*parq_dl));
	
	parq_dl->id = NULL;		/* Can't allocate yet, ID size isn't fixed */
	parq_dl->position = 0;
	
	return parq_dl;
}

/*
 * parq_dl_add_id
 * 
 * Assigns an parq ID to a download, and places them in various lists for lookup
 */
void parq_dl_add_id(struct download *d, const gchar *new_id)
{
	struct parq_dl_queued *parq_dl = NULL;

	g_assert(d != NULL);
	g_assert(new_id != NULL);
	g_assert(d->queue_status != NULL);

	parq_dl = (struct parq_dl_queued *) d->queue_status;

	g_assert(parq_dl != NULL);
	g_assert(parq_dl->id == NULL);	/* We don't expect an id here */

	parq_dl->id = g_strdup(new_id);
	g_hash_table_insert(dl_all_parq_by_id, parq_dl->id, d);

	g_assert(parq_dl->id != NULL);
}

/*
 * parq_dl_del_id
 *
 * Remove the memory used by the ID string, and removes it from 
 * various lists
 */
void parq_dl_del_id(struct download *d)
{
	struct parq_dl_queued *parq_dl = NULL;

	g_assert(d != NULL);

	parq_dl = (struct parq_dl_queued *) d->queue_status;

	g_assert(parq_dl != NULL);
	g_assert(parq_dl->id != NULL);

	g_hash_table_remove(dl_all_parq_by_id, parq_dl->id);
	G_FREE_NULL(parq_dl->id);

	g_assert(parq_dl->id == NULL);	/* We don't expect an id here */
}

/*
 * parq_dl_reparent_id
 *
 * Called from download_clone() to reparent the PARQ ID from the parent `d'
 * to the cloned `cd'.
 */
void parq_dl_reparent_id(struct download *d, struct download *cd)
{
	struct parq_dl_queued *parq_dl = NULL;

	g_assert(d != NULL);
	g_assert(cd != NULL);

	parq_dl = (struct parq_dl_queued *) d->queue_status;

	g_assert(parq_dl != NULL);
	g_assert(d->queue_status == cd->queue_status);	/* Cloned */

	/*
	 * Legacy queueing might not provide any ID.
	 */

	if (parq_dl->id != NULL) {
		g_hash_table_remove(dl_all_parq_by_id, parq_dl->id);
		g_hash_table_insert(dl_all_parq_by_id, parq_dl->id, cd);
	}

	d->queue_status = NULL;			/* No longer associated to `d' */
}

/*
 * parq_dl_update_id
 *
 * Updates a parq id if needed.
 */
static void parq_dl_update_id(struct download *d, const gchar *temp)
{
	struct parq_dl_queued *parq_dl = NULL;

	g_assert(d != NULL);
	g_assert(temp != NULL);

	parq_dl = (struct parq_dl_queued *) d->queue_status;	
	if (parq_dl->id != NULL) {
		if (0 == strcmp(temp, parq_dl->id))
			return;
	
		parq_dl_del_id(d);
	}
	
	parq_dl_add_id(d, temp);
}

/*
 * parq_download_parse_queue_status
 *
 * Retrieve and parse queueing information.
 * Returns TRUE if we parsed it OK, FALSE on error.
 */
gboolean parq_download_parse_queue_status(struct download *d, header_t *header)
{	
	struct parq_dl_queued *parq_dl = NULL;
	gchar *buf = NULL;
	gchar *temp = NULL;
	gchar *value = NULL;
	gint major, minor;
	gint header_value_length;
	gint retry;

	g_assert(d != NULL);
	g_assert(header != NULL);

	buf = header_get(header, "X-Queue");
	
	if (buf == NULL)			/* Remote server does not support queues */
		return FALSE;

	if (!get_header_version(buf, &major, &minor)) {
		/*
	 	* Could not retreive queueing version. It could be 0.1 but there is 
		* no way to tell for certain
	 	*/
		major = 0;
		minor = 1;
	}
	
	d->server->parq_version.major = major;
	d->server->parq_version.minor = minor;
	
	if (d->queue_status == NULL) {
		/* So this download has no parq structure yet, well create one! */
		d->queue_status = parq_dl_create(d);
	}
	
	parq_dl = (struct parq_dl_queued *) d->queue_status;
	
	g_assert(parq_dl != NULL);
	
	switch (major) {
	case 0:				/* Active queueing */
		g_assert(buf != NULL);		
		value = get_header_value(buf, "pollMin", NULL);
		parq_dl->retry_delay  = value == NULL ? 0 : get_integer(value);
		
		value = get_header_value(buf, "pollMax", NULL);
		parq_dl->lifetime  = value == NULL ? 0 : get_integer(value);
		break;
	case 1:				/* PARQ */
		buf = header_get(header, "X-Queued");

		if (buf == NULL) {
			g_warning("[PARQ DL] host %s advertised PARQ %d.%d but did not"
				" send X-Queued",
				ip_port_to_gchar(download_ip(d), download_port(d)),
				major, minor);
			if (dbg) {
				g_warning("[PARQ DL]: header dump:");
				header_dump(header, stderr);
			}
			return FALSE;
		}
		
		value = get_header_value(buf, "lifetime", NULL);
		parq_dl->lifetime = value == NULL ? 0 : get_integer(value);

		parq_dl->retry_delay = extract_retry_after(header);

		/* Someone might not be playing nicely. */
		if (parq_dl->lifetime < parq_dl->retry_delay) {
			parq_dl->lifetime = MAX(300, parq_dl->retry_delay );
			g_warning("[PARQ DL] Invalid lifetime, using: %d",
				  parq_dl->lifetime);
		}
		
		value = get_header_value(buf, "ID", &header_value_length);
		if (header_value_length > 0) {
			temp = g_strndup(value, header_value_length);

			parq_dl_update_id(d, temp);
		
			g_free(temp);
		}
		break;
	default:
		g_warning("[PARQ DL] unhandled queuing version %d.%d from %s <%s>",
			major, minor, ip_port_to_gchar(download_ip(d), download_port(d)),
			download_vendor_str(d));
		return FALSE;
	}

	value = get_header_value(buf, "position", NULL);
	parq_dl->position = value == NULL ? 0 : get_integer(value);
	
	value = get_header_value(buf, "length", NULL);
	parq_dl->length   = value == NULL ? 0 : get_integer(value);
				
	value = get_header_value(buf, "ETA", NULL);
	parq_dl->eta  = value == NULL ? 0 : get_integer(value);

	/*
	 * If we're not in the first position, lower our retry rate.
	 * We try to retry every 60 seconds when in position 2, every 90 in
	 * position 3, and so on.  If we fall out of range, adjust: we must not
	 * poll before the minimum specified by `retry_delay', and we try to
	 * poll again at least 40 seconds before `lifetime' to avoid being
	 * kicked out.
	 *		--RAM, 22/02/2003
	 */

	retry = parq_dl->position * PARQ_TIMER_BY_POS;

	if (retry > (parq_dl->lifetime - PARQ_RETRY_SAFETY))
		retry = parq_dl->lifetime - PARQ_RETRY_SAFETY;
	if (retry < parq_dl->retry_delay)
		retry = parq_dl->retry_delay;

	if (dbg > 2)
		printf("Queue version: %d.%d, position %d out of %d,"
			" retry in %ds within [%d, %d]\n",
			major, minor, parq_dl->position, parq_dl->length,
			retry, parq_dl->retry_delay, parq_dl->lifetime);
	
	if (parq_download_is_active_queued(d)) {
		/*
		 * Don't keep a chunk busy if we are queued, perhaps another servent
		 * can complete it for us.
		 */

		file_info_clear_download(d, TRUE);
		d->status = GTA_DL_ACTIVE_QUEUED;
	}
	
	d->timeout_delay = retry;

	return TRUE;		/* OK */
}

/*
 * parq_download_is_active_queued
 *
 * Whether the download is queued remotely or not.
 */
gboolean parq_download_is_active_queued(struct download *d)
{
	struct parq_dl_queued *parq_dl = NULL;
		
	g_assert(d != NULL);
	parq_dl = (struct parq_dl_queued *) d->queue_status;
	if (parq_dl == NULL)
		return FALSE;
		
	return parq_dl->position > 0 && d->keep_alive;
}

/*
 * parq_download_is_active_queued
 *
 * Whether the download is queued remotely without keeping the connection or not
 */
gboolean parq_download_is_passive_queued(struct download *d)
{
	struct parq_dl_queued *parq_dl = NULL;
		
	g_assert(d != NULL);

	parq_dl = (struct parq_dl_queued *) d->queue_status;
	
	if (parq_dl == NULL)
		return FALSE;
	
	return parq_dl->position > 0 && !d->keep_alive;
}


/*
 * parq_download_add_header
 *
 * Adds an:
 *
 *    X-Queue: 1.0
 *    X-Queued: position=x; ID=xxxxx
 *
 * to the HTTP GET request
 */
void parq_download_add_header(
	gchar *buf, gint len, gint *rw, struct download *d)
{
	g_assert(d != NULL);
	g_assert(len >= 0 && *rw >= 0 && len >= *rw);	

	*rw += gm_snprintf(&buf[*rw], len - *rw,
		"X-Queue: %d.%d\r\n", PARQ_VERSION_MAJOR, PARQ_VERSION_MINOR);

	/*
	 * Only add X-Queued header if server really supports X-Queue: 1.x. Don't
	 * add X-Queued if there is no ID available. This could be because it is
	 * a first request.
	 */

	if (d->server->parq_version.major == 1) {
		if (get_parq_dl_id(d) != NULL)
			*rw += gm_snprintf(&buf[*rw], len - *rw,
				  "X-Queued: position=%d; ID=%s\r\n",
				  get_parq_dl_position(d), get_parq_dl_id(d));
	}

	/*
	 * Only send X-Node if not firewalled and the listen IP/port combination
	 * we're claiming is "valid".
	 */

	if (!is_firewalled && host_is_valid(listen_ip(), listen_port))
		*rw += gm_snprintf(&buf[*rw], len - *rw,
		  	  "X-Node: %s\r\n", 
			  ip_port_to_gchar(listen_ip(), listen_port));
}

/*
 * parq_download_queue_ack
 *
 * PARQ enabled servers send a 'QUEUE' command when the lifetime of the download
 * (upload from the servers point of view) is about to expire, or if the 
 * download has retreived an download slot (upload slot from the servers point
 * of view). This function looksup the ID associated with the QUEUE command
 * and prepares the download to continue.
 */
void parq_download_queue_ack(struct gnutella_socket *s)
{
	gchar *queue;
	gchar *id;
	gchar *ip_str;
	struct download *dl;
	guint32 ip = 0;
	guint16 port = 0;
	gboolean has_ip_port = TRUE;

	socket_tos_default(s);	/* Set proper Type of Service */

	g_assert(s != NULL);
	g_assert(s->getline);
	
	queue = getline_str(s->getline);
	
	if (dbg) {
		printf("--- Got QUEUE from %s:\n", ip_to_gchar(s->ip));
		printf("%s\n", queue);
		printf("---\n");
		fflush(stdout);
	}

 	/* ensured by socket_read() */
	g_assert(0 == strncmp(queue, "QUEUE ", sizeof("QUEUE ") - 1));

	id = queue + sizeof("QUEUE ") - 1;
	while (isspace((guchar) *id))
		id++;

	/*
	 * Fetch the IP port at the end of the QUEUE string.
	 */

	ip_str = strchr(id, ' ');

	if (ip_str == NULL || !gchar_to_ip_port(ip_str + 1, &ip, &port)) {
		g_warning("[PARQ DL] missing IP:port in \"%s\" from %s",
			queue, ip_to_gchar(s->ip));
		has_ip_port = FALSE;
	}

	/*
	 * Terminate the ID part from the QUEUE message.
	 */

	if (ip_str != NULL)
		*ip_str = '\0';

	dl = (struct download *) g_hash_table_lookup(dl_all_parq_by_id, id);

	/*
	 * If we were unable to locate a download by this ID, try to elect
	 * another download from this host for which we don't have any PARQ
	 * information yet.
	 */

	if (dl == NULL) {
		g_warning("[PARQ DL] could not locate QUEUE id '%s' from %s",
			id, ip_port_to_gchar(ip, port));

		if (has_ip_port) {
			dl = download_find_waiting_unparq(ip, port);

			if (dl != NULL) {
				g_warning("[PARQ DL] elected '%s' from %s for QUEUE id '%s'",
					dl->file_name, ip_port_to_gchar(ip, port), id);

				g_assert(dl->queue_status == NULL);		/* unparq'ed */

				dl->queue_status = parq_dl_create(dl);
				parq_dl_add_id(dl, id);

				/* All set for request now */
			}
		}
	}

	if (dl == NULL) {
		g_assert(s->resource.download == NULL);	/* Hence socket_free() */
		socket_free(s);
		return;
	}

	if (dl->list_idx == DL_LIST_RUNNING) {
		g_warning("[PARQ DL] Watch it! Download already running.");
		g_assert(s->resource.download == NULL); /* Hence socket_free() */
		socket_free(s);
		return;
	}

	if (has_ip_port)
		download_redirect_to_server(dl, ip, port);	/* Might have changed */

	dl->server->parq_version.major = 1;				/* At least */
	dl->server->parq_version.minor = 0;

	/*
	 * Revitalize download, if stopped (aborted, error).
	 */

	if (dl->list_idx == DL_LIST_STOPPED)
		dl->file_info->lifecount++;

	/*
	 * Send the request on the connection the server opened.
	 *
	 * NB: if this is the initial QUEUE request we get after being relaunched,
	 * we won't have a valid queue position to send back, and 0 will be used.
	 */

	if (download_start_prepare(dl)) {
		struct gnutella_socket *ds = dl->socket;
		dl->socket = s;
		ds = s;
#if 0
		d->keep_alive = TRUE;			/* was reset in start_prepare_running */
#endif
		
		getline_free(ds->getline);		/* No longer need this */
		ds->getline = NULL;

	
  		g_assert(dl->socket != NULL);
		dl->last_update = time((time_t *) NULL);
		s->resource.download = dl;
																			     
		/* Resend request for download */
		download_send_request(dl);
	}
}

/***
 ***  The following section contains upload queueing
 ***/

/*
 * handle_to_queued
 *
 * Convert an handle to a `parq_ul_queued' structure.
 */
inline struct parq_ul_queued *handle_to_queued(gpointer handle)
{
	struct parq_ul_queued *uq = (struct parq_ul_queued *) handle;

	g_assert(handle != NULL);
	g_assert(uq->magic == PARQ_UL_MAGIC);

	return uq;
}

/*
 * parq_upload_free
 *
 * removes an parq_ul from the parq list and frees all its memory
 */
static void parq_upload_free(struct parq_ul_queued *parq_ul)
{		
	g_assert(parq_ul != NULL);
	g_assert(parq_ul->ip_and_name != NULL);
	g_assert(parq_ul->queue != NULL);
	g_assert(parq_ul->queue->size > 0);
	g_assert(parq_ul->queue->by_position != NULL);
	g_assert(parq_ul->by_ip != NULL);
	g_assert(parq_ul->by_ip->total > 0);
	g_assert(parq_ul->by_ip->uploading <= parq_ul->by_ip->total);
	
	parq_upload_decrease_all_after(parq_ul);	

	if (parq_ul->flags & PARQ_UL_QUEUE)
		ul_parq_queue = g_list_remove(ul_parq_queue, parq_ul);
		
	parq_ul->by_ip->list = g_list_remove(parq_ul->by_ip->list, parq_ul);
	parq_ul->by_ip->total--;

	if (parq_ul->by_ip->total == 0) {
		g_assert(parq_ul->remote_ip == parq_ul->by_ip->ip);
		g_assert(parq_ul->by_ip->list == NULL);

		/* No more uploads from this ip, cleaning up */
		g_hash_table_remove(ul_all_parq_by_ip, &parq_ul->by_ip->ip);
		wfree(parq_ul->by_ip, sizeof(*parq_ul->by_ip));
		
		g_assert(g_hash_table_lookup(ul_all_parq_by_ip,
			  &parq_ul->remote_ip) == NULL);
	}
	
	parq_ul->by_ip = NULL;

	/*
	 * Tell parq_upload_update_relative_position not to take this
	 * upload into account when updating the relative position
	 */
	if (parq_ul->is_alive) {
		parq_ul->queue->alive--;
		parq_ul->is_alive = FALSE;
		parq_ul->queue->by_rel_pos = 
			  g_list_remove(parq_ul->queue->by_rel_pos, parq_ul);
		
		parq_upload_update_relative_position(parq_ul);
		parq_upload_update_eta(parq_ul->queue);
	} else {
		parq_ul->queue->by_date_dead = g_list_remove(
			  parq_ul->queue->by_date_dead, parq_ul);
	}

	/* Remove the current queued item from all lists */
	parq_ul->queue->by_position = 
		g_list_remove(parq_ul->queue->by_position, parq_ul);

	g_hash_table_remove(ul_all_parq_by_ip_and_name, parq_ul->ip_and_name);
	g_hash_table_remove(ul_all_parq_by_id, parq_ul->id);

	g_assert(g_list_find(parq_ul->queue->by_date_dead, parq_ul) == NULL);
	g_assert(g_list_find(parq_ul->queue->by_rel_pos, parq_ul) == NULL);

	/* 
	 * Queued upload is now removed from all lists. So queue size can be
	 * safely decreased and new ETAs can be calculate.
	 */
	parq_ul->queue->size--;

	parq_upload_update_eta(parq_ul->queue);


	/* Free the memory used by the current queued item */
	G_FREE_NULL(parq_ul->ip_and_name);
	G_FREE_NULL(parq_ul->id);
	parq_ul->name = NULL;

	wfree(parq_ul, sizeof(*parq_ul));
	parq_ul = NULL;
	
	if (dbg > 3)
		printf("PARQ UL: Entry freed from memory\n");
}

/*
 * parq_ul_calc_retry
 *
 * Calculates the retry delay for an upload
 * Returns the recommended retry delay.
 */
guint32 parq_ul_calc_retry(struct parq_ul_queued *parq_ul)
{
	int result = 60 + 45 * (parq_ul->relative_position - 1);
#if AGRESSIVE
	int fast_result;
	struct parq_ul_queued *parq_ul_prev = NULL;
	GList *l = NULL;
	
	l = g_list_find(parq_ul->queue->by_rel_pos, parq_ul);
	
	if (l == NULL)
		l = g_list_last(parq_ul->queue->by_position);
	
	if (l == NULL)
		return MIN(PARQ_MAX_UL_RETRY_DELAY, result);
	
	if (l->prev != NULL) {
		parq_ul_prev = (struct parq_ul_queued *) l->prev->data;
	
		g_assert(parq_ul_prev != NULL);
		
		fast_result = parq_ul_prev->chunk_size / bw_http_out;
	
		result = MIN(result, fast_result);
	}
#endif
	
	return MIN(PARQ_MAX_UL_RETRY_DELAY, result);
}

/*
 * parq_upload_create
 *
 * Creates a new upload structure and prefills some values. Returns a pointer to
 * the newly created ul_queued structure.
 */
static struct parq_ul_queued *parq_upload_create(gnutella_upload_t *u)
{
	time_t now = time((time_t *) NULL);
	struct parq_ul_queued *parq_ul = NULL;
	struct parq_ul_queued *parq_ul_prev = NULL;
	struct parq_ul_queue *parq_ul_queue = NULL;
	guchar parq_id[16];

	guint eta = 0;
	guint rel_pos = 1;
	GList *l;
	
	g_assert(u != NULL);
	g_assert(ul_all_parq_by_ip_and_name	!= NULL);
	g_assert(ul_all_parq_by_id != NULL);
	
	parq_ul_queue = parq_upload_which_queue(u);
	g_assert(parq_ul_queue != NULL);

	/* Locate the previous queued item so we can calculate the ETA */
	l = g_list_last(parq_ul_queue->by_position);
	if (l != NULL)
		parq_ul_prev = (struct parq_ul_queued *) l->data;

	if (parq_ul_prev != NULL) {
		rel_pos = parq_ul_prev->relative_position;
		if (parq_ul_prev->is_alive)
			rel_pos++;
		
		eta = parq_ul_prev->eta;
		
		if (max_uploads <= 0) {
			eta = (guint) -1;
		} else if (parq_ul_prev->is_alive) {
			if (bw_http_out != 0 && bws_out_enabled) {
				eta += parq_ul_prev->file_size / bw_http_out;
			} else {
				if (dbg > 2) {
					printf("PARQ UL Q %d/%d: Could not calculate ETA\n",
					g_list_position(ul_parqs, 
						g_list_find(ul_parqs, parq_ul_prev->queue)),
						g_list_length(ul_parqs) - 1);
				}
				
				/*
				 * According to the PARQ specification the ETA should be
				 * calculated using the maximum upload rate. However the
				 * maximum upload rate is unknown.
				 * Pessimistic: 1 bytes / sec
				 */
				eta += parq_ul_prev->file_size / max_uploads;
			}
		}
	}
	
	/* Create new parq_upload item */
	parq_ul = walloc0(sizeof(*parq_ul));
	g_assert(parq_ul != NULL);

	/* Create identifier to find upload again later. IP + Filename */
	parq_ul->remote_ip = u->ip;
	parq_upload_update_ip_and_name(parq_ul, u);
	
	/* Create an ID */
	guid_random_muid(parq_id);
	parq_ul->id = g_strdup(guid_hex_str(parq_id));

	g_assert(parq_ul->ip_and_name != NULL);
	g_assert(parq_ul->id != NULL);
	
	/* Fill parq_ul structure */
	parq_ul->magic = PARQ_UL_MAGIC;
	parq_ul->position = ++parq_ul_queue->size;
	parq_ul->relative_position = rel_pos;
	parq_ul->eta = eta;
	parq_ul->enter = now;
	parq_ul->updated = now;
	parq_ul->file_size = u->file_size;
	parq_ul->queue = parq_ul_queue;
	parq_ul->has_slot = FALSE;
	parq_ul->ip = 0;
	parq_ul->port = 0;
	parq_ul->major = 0;
	parq_ul->minor = 0;
	parq_ul->is_alive = TRUE;
	parq_ul->had_slot =  FALSE;
	parq_ul->queue->alive++;
	parq_ul->retry = now + parq_ul_calc_retry(parq_ul);
	parq_ul->expire = parq_ul->retry + MIN_LIFE_TIME;
	parq_ul->ban_timeout = 0;
	parq_ul->disc_timeout = 0;
	
	/* Save into hash table so we can find the current parq ul later */
	g_hash_table_insert(ul_all_parq_by_id, parq_ul->id, parq_ul);
	
	parq_ul_queue->by_position = 
		g_list_append(parq_ul_queue->by_position, parq_ul);	

	parq_ul_queue->by_rel_pos = 
		g_list_append(parq_ul_queue->by_rel_pos, parq_ul);	
	
	if (dbg > 3) {
		printf("PARQ UL Q %d/%d (%3d[%3d]/%3d): New: %s \"%s\"; ID=\"%s\"\n",
			g_list_position(ul_parqs, 
				g_list_find(ul_parqs, parq_ul->queue)) + 1,
			g_list_length(ul_parqs), 
			parq_ul->position,
			parq_ul->relative_position,			
			parq_ul->queue->size,
			ip_to_gchar(parq_ul->remote_ip),
			parq_ul->name,
			parq_ul->id);
	}	
	
	/* Check if the requesting client has already other PARQ entries */
	parq_ul->by_ip = (struct parq_ul_queued_by_ip *)
		g_hash_table_lookup(ul_all_parq_by_ip, &parq_ul->remote_ip);
	
	if (parq_ul->by_ip == NULL) {
		/* The requesting client has no other PARQ entries yet, create an ip
		 * reference structure */
		parq_ul->by_ip = walloc0(sizeof(*parq_ul->by_ip));
		parq_ul->by_ip->ip = parq_ul->remote_ip;
		g_hash_table_insert(
			ul_all_parq_by_ip, &parq_ul->by_ip->ip, parq_ul->by_ip);
		parq_ul->by_ip->uploading = 0;
		parq_ul->by_ip->total = 0;
		parq_ul->by_ip->list = NULL;
	}
	
	g_assert(parq_ul->by_ip->ip == parq_ul->remote_ip);
	
	parq_ul->by_ip->total++;
	parq_ul->by_ip->list = g_list_prepend(parq_ul->by_ip->list, parq_ul);
	
	g_assert(parq_ul != NULL);
	g_assert(parq_ul->position > 0);
	g_assert(parq_ul->id != NULL);
	g_assert(parq_ul->ip_and_name != NULL);
	g_assert(parq_ul->name != NULL);
	g_assert(parq_ul->queue != NULL);
	g_assert(parq_ul->queue->by_position != NULL);
	g_assert(parq_ul->queue->by_rel_pos != NULL);
	g_assert(parq_ul->queue->by_position->data != NULL);
	g_assert(parq_ul->relative_position > 0);
	g_assert(parq_ul->relative_position <= parq_ul->queue->size);
	g_assert(parq_ul->by_ip != NULL);
	g_assert(parq_ul->by_ip->uploading <= parq_ul->by_ip->total);

	return parq_ul;
}

/*
 * parq_upload_which_queue
 * 
 * Looks up in which queue the current upload should be placed and if the queue
 * doesn't exist yet it will be created.
 * Returns a pointer to the queue in which the upload should be queued.
 */
static struct parq_ul_queue *parq_upload_which_queue(gnutella_upload_t *u)
{
	struct parq_ul_queue *queue;
	guint size = 0;
	guint slot = 0;
	
	size = PARQ_UL_LARGE_SIZE;
	
	/* 
	 * Determine in which queue the upload should be placed. Upload queues:
	 * 300 < size < oo
	 * 150 < size < 300
	 *  75 < size < 150
	 *   0 < size < 75
	 * Smallest: PARQ_UL_LARGE_SIZE / 2^(parq_upload_slots-1)
	 */
	
	for (slot = 1 ; slot <= max_uploads; slot++) {
		if (u->file_size > size || slot >= max_uploads)
			break;
		size = size / 2;
	}
	
	while (g_list_length(ul_parqs) < max_uploads) {
		queue = parq_upload_new_queue();
	}
	
	queue = (struct parq_ul_queue *) g_list_nth_data(ul_parqs, slot - 1);

	/* We might need to reactivate the queue */
	queue->active = TRUE;
	
	g_assert(queue != NULL);
	g_assert(queue->active == TRUE);
	
	return queue;
}

/*
 * parq_upload_new_queue
 *
 * Creates a new parq_ul_queue structure and places it in the ul_parqs
 * linked list.
 */
static struct parq_ul_queue *parq_upload_new_queue()
{
	struct parq_ul_queue *queue = NULL;

	queue = walloc(sizeof(*queue));
	g_assert(queue != NULL);

	queue->size = 0;
	queue->active = TRUE;
	queue->by_position = NULL;
	queue->by_rel_pos = NULL;
	queue->by_date_dead = NULL;
	queue->active_uploads = 0;
	queue->alive = 0;
	
	ul_parqs = g_list_append(ul_parqs, queue);

	if (dbg)
		printf("PARQ UL: Created new queue %d\n", 
				g_list_position(ul_parqs, g_list_find(ul_parqs, queue)) + 1);
		
	g_assert(ul_parqs != NULL);
	g_assert(ul_parqs->data != NULL);
	g_assert(queue != NULL);
	
	return queue;
}

/*
 * parq_upload_free_queue
 *
 * Frees the queue from memory and the ul_parqs linked list
 */
static void parq_upload_free_queue(struct parq_ul_queue *queue)
{
	g_assert(queue != NULL);
	g_assert(ul_parqs != NULL);

	/* Never ever remove a queue which is in use and/or marked as active */
	g_assert(queue->size == 0);
	g_assert(queue->active_uploads == 0);
	g_assert(queue->active == FALSE);
	
	if (dbg)
		printf("PARQ UL: Removing inactive queue %d\r\b", 
				g_list_position(ul_parqs, g_list_find(ul_parqs, queue)) + 1);
		
	/* Remove queue from all lists */
	ul_parqs = g_list_remove(ul_parqs, queue);
	
	/* Free memory */
	wfree(queue, sizeof(*queue));
	queue = NULL;
}

/*
 * parq_upload_update_eta
 *
 * Updates the ETA of all queued items in the given queue
 */
static void parq_upload_update_eta(struct parq_ul_queue *which_ul_queue)
{
	extern gint running_uploads;
	GList *l;
	guint eta = 0;
	
	if (which_ul_queue->active_uploads) {
		/*
		 * Current queue as an upload slot. Use this one for a start ETA.
		 * Locate the first active upload in this queue.
		 */
		
		for (l = which_ul_queue->by_position; l; l = g_list_next(l)) {	
			struct parq_ul_queued *parq_ul = (struct parq_ul_queued *) l->data;
			
			if (parq_ul->has_slot) {
				if (max_uploads > 0) {
					/* Recalculate ETA */
					if (bw_http_out != 0 && bws_out_enabled) {
						eta += parq_ul->file_size / (bw_http_out / max_uploads);
					} else {
						/* FIXME, should use average bandwidth here */
						/* Pessimistic: 1 bytes / sec */
						eta += parq_ul->file_size;
					}
				} else
					eta = (guint) -1;

				break;
			}
		}
	}
	
	if (eta == 0 && running_uploads > max_uploads) {
		/* We don't have an upload slot available, so an start ETA (for position
		 * 1) is necessary.
		 * Use the eta of another queue. First by the queue which uses more than
		 * one upload slot. If that result is still 0, we have a small problem
		 * as the ETA can't be calculated correctly anymore.
		 */
		
		for (l = ul_parqs; l; l = g_list_next(l)) {
			struct parq_ul_queue *q = (struct parq_ul_queue *) l->data;
			
			if (q->active_uploads > 1) {
				struct parq_ul_queued *parq_ul = 
					(struct parq_ul_queued *) q->by_rel_pos->data;
				
				eta = parq_ul->eta;
				break;
			}
		}
		
		if (eta == 0)
			g_warning("[PARQ UL] Was unable to calculate an accurate ETA");

	}
	
	for (l = which_ul_queue->by_rel_pos; l; l = g_list_next(l)) {	
		struct parq_ul_queued *parq_ul = (struct parq_ul_queued *) l->data;
		
		g_assert(parq_ul->is_alive);
		
		parq_ul->eta = eta;
		
		if (parq_ul->has_slot)
			/* Skip already uploading uploads */
			continue;
		
		if (max_uploads > 0) {
			/* Recalculate ETA */
			if (bw_http_out != 0 && bws_out_enabled) {
				eta += parq_ul->file_size / (bw_http_out / max_uploads);
			} else {
				/* FIXME, should use average bandwidth here */
				/* Pessimistic: 1 bytes / sec */
				eta += parq_ul->file_size;
			}
		} else
			eta = (guint) -1;
	}
}

static struct parq_ul_queued *parq_upload_find_id(gnutella_upload_t *u, 
												  header_t *header)
{
	gchar *buf;
	struct parq_ul_queued *parq_ul = NULL;
	
	buf = header_get(header, "X-Queued");
	
	if (buf != NULL) {
		gint length;
		gchar *id = get_header_value(buf, "ID", &length);

		if (id == NULL) {
			g_warning("[PARQ UL] missing ID in PARQ request");
			if (dbg) {
				g_warning("[PARQ UL] header dump:");
				header_dump(header, stderr);
			}
			return NULL;
		}	

		parq_ul = g_hash_table_lookup(ul_all_parq_by_id, id);
	}
	
	return parq_ul;
}
	
/*
 * parq_upload_find
 *
 * Finds an upload if available in the upload queue.
 * returns NULL if upload could not be found.
 */
static struct parq_ul_queued *parq_upload_find(gnutella_upload_t *u)
{
	gchar buf[1024];
	
	g_assert(u != NULL);
	g_assert(ul_all_parq_by_ip_and_name != NULL);
	g_assert(ul_all_parq_by_id != NULL);
	
	gm_snprintf(buf, sizeof(buf), "%u %s", u->ip, u->name);
	
	return g_hash_table_lookup(ul_all_parq_by_ip_and_name, buf);
}

/*
 * parq_upload_timer
 *
 * Removes any PARQ uploads which show no activity.
 */
void parq_upload_timer(time_t now)
{
	GList *queues;
	GList *dl;
	GSList *sl;
	GSList *remove = NULL;
	static guint print_q_size = 0;
	static guint startup_delay = 0;
	guint	queue_selected = 0;
	gboolean rebuilding = FALSE;
	
	
	/*
	 * Don't do anything with parq during the first 10 seconds. Looks like
	 * PROP_LIBRARY_REBUILDING is not set yet immediatly at the first time, so
	 * there may be some other things not set properly yet neither.
	 */
	if (startup_delay < 10) {
		startup_delay++;
		return;
	}

	for (queues = ul_parqs ; queues != NULL; queues = queues->next) {
		struct parq_ul_queue *queue = (struct parq_ul_queue *) queues->data;

		queue_selected++;

		/*
		 * Infrequently scan the dead uploads as well to send QUEUE.
		 */

		if ((now % 60) == 0) {
			for (dl = queue->by_date_dead; dl != NULL; dl = dl->next) {	
				struct parq_ul_queued *parq_ul =
					(struct parq_ul_queued *) dl->data;

				g_assert(parq_ul != NULL);

				/* Entry can't have a slot, and we know it expired! */

				if (
					!(parq_ul->flags & (PARQ_UL_QUEUE|PARQ_UL_NOQUEUE)) &&
					now - parq_ul->last_queue_sent > QUEUE_PERIOD &&
					parq_ul->queue_sent < MAX_QUEUE &&
					parq_ul->queue_refused < MAX_QUEUE_REFUSED
				)
					parq_upload_send_queue(parq_ul);
			}
		}
			
		for (dl = queue->by_rel_pos; dl != NULL; dl = dl->next) {	
			struct parq_ul_queued *parq_ul = (struct parq_ul_queued *) dl->data;

			g_assert(parq_ul != NULL);
			
			if (
				parq_ul->expire <= now &&
				!parq_ul->has_slot &&
				!(parq_ul->flags & (PARQ_UL_QUEUE|PARQ_UL_NOQUEUE)) &&
				now - parq_ul->last_queue_sent > QUEUE_PERIOD &&
				parq_ul->queue_sent < MAX_QUEUE &&
				parq_ul->queue_refused < MAX_QUEUE_REFUSED
			)
				parq_upload_send_queue(parq_ul);
			
			if (
				parq_ul->is_alive &&
				parq_ul->expire + EXPIRE_GRACE_TIME < now &&
				!parq_ul->has_slot &&
				!(parq_ul->flags & PARQ_UL_QUEUE)	/* No timeout if pending */
			) {
				if (dbg > 3) 
					printf("PARQ UL Q %d/%d (%3d[%3d]/%3d): Timeout: %s '%s'\n",
						g_list_position(ul_parqs, 
							g_list_find(ul_parqs, parq_ul->queue)) + 1,
						g_list_length(ul_parqs), 
						parq_ul->position, 
						parq_ul->relative_position,
						parq_ul->queue->size,
						ip_to_gchar(parq_ul->remote_ip),
						parq_ul->name);
				
				
				/*
			 	 * Mark for removal. Can't remove now as we are still using the
			 	 * ul_parq_by_position linked list. (prepend is probably the 
				 * fastest function
			 	 */
				remove = g_slist_prepend(remove, parq_ul);		
			}
		}

		/*
		 * Mark queue as inactive when there are less uploads slots available.
		 */
		if (queue_selected > max_uploads)
			queue->active = FALSE;
		else
			queue->active = TRUE;
	}
	

	for (sl = remove; sl != NULL; sl = g_slist_next(sl)) {
		struct parq_ul_queued *parq_ul = (struct parq_ul_queued *) sl->data;
			
		parq_ul->is_alive = FALSE;
		parq_ul->queue->alive--;
				
		parq_ul->queue->by_rel_pos = 
			  g_list_remove(parq_ul->queue->by_rel_pos, parq_ul);

		parq_upload_update_relative_position(parq_ul);
		parq_upload_update_eta(parq_ul->queue);
		
		g_assert(parq_ul->queue->alive >= 0);					

		if (!enable_real_passive)
			parq_upload_free((struct parq_ul_queued *) sl->data);
		else
			parq_ul->queue->by_date_dead = 
				  g_list_append(parq_ul->queue->by_date_dead, parq_ul);
	}
	
	g_slist_free(remove);

	/* Save queue info every 60 seconds */
	if (print_q_size++ >= 60) {
		print_q_size = 0;

		if (dbg) {
			printf("\n");

			for (queues = ul_parqs ; queues != NULL; queues = queues->next) {
   	    		struct parq_ul_queue *queue = 
				  	  (struct parq_ul_queue *) queues->data;
			
 				printf("PARQ UL: Queue %d/%d contains %d items, "
					  "%d uploading, %d alive, queue is marked %s \n",
					  g_list_position(ul_parqs, g_list_find(ul_parqs, queue))
						  + 1,
					  g_list_length(ul_parqs),
					  queue->size,
					  queue->active_uploads,
					  queue->alive,
					  queue->active ? "active" : "inactive");
			}
		}
			
		parq_upload_save_queue();
		
	}
	
	/*
	 * If the last queue is not active anymore (ie it should be removed
	 * as soon as the queue is empty) and there are no more queued items
	 * in the queue, remove the queue.
	 */
	queues = g_list_last(ul_parqs);
	
	if (queues != NULL) {
		struct parq_ul_queue *queue = (struct parq_ul_queue *) queues->data;
		if (!queue->active && queue->size == 0) {
			parq_upload_free_queue(queue);
		}
	}
	
	/* Send one QUEUE command at a time, until we have MAX_UPLOADS uploads */

	gnet_prop_get_boolean_val(PROP_LIBRARY_REBUILDING, &rebuilding);
	
	if (!rebuilding) {
		extern gint registered_uploads;			/* From uploads.c */

		while (
			registered_uploads < MAX_UPLOADS && 
			g_list_first(ul_parq_queue) != NULL
		) {
			struct parq_ul_queued *parq_ul = 
				(struct parq_ul_queued *) g_list_first(ul_parq_queue)->data;
				
			parq_upload_do_send_queue(parq_ul);
	
			ul_parq_queue = g_list_remove(ul_parq_queue, parq_ul);
			parq_ul->flags &= ~PARQ_UL_QUEUE;
		}
	}
}

/*
 * parq_upload_queue_full
 *
 * Returns true if parq cannot hold any more uploads
 */
gboolean parq_upload_queue_full(gnutella_upload_t *u)
{
	struct parq_ul_queue *q_ul = NULL;	
	struct parq_ul_queued *parq_ul = NULL;
		
	q_ul = parq_upload_which_queue(u);
	g_assert(q_ul->size >= q_ul->alive);
	
	if (q_ul->size < parq_max_upload_size)
		return FALSE;
	
	if (q_ul->by_date_dead == NULL || 
		  g_list_first(q_ul->by_date_dead) == NULL) {
		return TRUE;
	}
	
	g_assert(q_ul->by_date_dead != NULL);
	
	if (dbg > 2)
		printf("PARQ UL: Removing a 'dead' upload\n");
	
	parq_ul = (struct parq_ul_queued *) g_list_first(q_ul->by_date_dead)->data;
	parq_upload_free(parq_ul);

	return FALSE;
}

/*
 * parq_upload_queued
 *
 * Whether the current upload is already queued.
 */
gboolean parq_upload_queued(gnutella_upload_t *u)
{
	return parq_upload_lookup_position(u) != (guint) -1;
}

/*
 * parq_upload_get_at
 *
 * Get parq structure at specified position.
 */
struct parq_ul_queued *parq_upload_get_at(struct parq_ul_queue *queue,
		int position)
{
	return (struct parq_ul_queued *) g_list_nth_data(queue->by_rel_pos,
			  position - 1);
}

/*
 * parq_upload_continue
 * 
 * Returns true if the current upload is allowed to get an upload slot.
 */
static gboolean parq_upload_continue(struct parq_ul_queued *uq, gint free_slots)
{
	GList *l = NULL;
	extern guint max_uploads;
	gint slots_free = max_uploads;
	
	/*
	 * max_uploads holds the number of upload slots an queue may currently
	 * use. This is the lowest number of upload slots used by a queue + 1.
	 */
	gint allowed_max_uploads = -1;

	g_assert(uq != NULL);

	/*
	 * If there are no free upload slots the queued upload isn't allowed an
	 * upload slot anyway. So we might just as well abort here
	 */

	if (free_slots <= 0)
		return FALSE;

	/*
	 * Don't allow more than max_uploads_ip per single host (IP)
	 */
	if (uq->by_ip->uploading >= max_uploads_ip)
		return FALSE;
	
	/*
	 * If the number of upload slots have been decreased, an old queue
	 * may still exist. What to do with those uploads? Should we make
	 * sure those uploads are served first? Those uploads should take
	 * less time to upload anyway, as they _must_ be smaller.
	 */

	l = g_list_last(ul_parqs);
	{
		struct parq_ul_queue *queue = (struct parq_ul_queue *) l->data;
		if (!queue->active && queue->alive > 0) {
			if (uq->queue->active) {
				g_warning("[PARQ UL] Upload in inactive queue first");
				return FALSE;
			}
		}
	}
	
	/*
	 * 1) First check if another queue 'needs' an upload slot.
	 * 2) Avoid  one queue getting almost all upload slots.
	 * 3) Then, check if the current upload is allowed this upload slot.
	 */
	
	/*
	 * Step 1. Check if another queues must have an upload.
	 *         That is when the current queue has no active uploads while there
	 *         are uploads alive.
	 * Step 2. Avoid one queue getting almost all upload slots.
	 *
	 * This is done by determining how many upload slots every queue is using,
	 * and if the queue would like to have another upload slot.
	 */
	
	for (l = g_list_last(ul_parqs); l; l = l->prev) {
		struct parq_ul_queue *queue = (struct parq_ul_queue *) l->data;
		if (queue->alive > queue->active_uploads) {
			/* Queue would like to get another upload slot */
			if ((guint) allowed_max_uploads > queue->active_uploads) {
				/*
				 * Determine the current maximum of upload
				 * slots allowed compared to other queus.
				 */
				allowed_max_uploads = queue->active_uploads + 1;
			}
		}
		if (queue->alive > 0)
			slots_free--;
	}
	
	/* This is to ensure dynamic slot allocation */
	if (slots_free < 0)
		slots_free = 0;

#if AGRESSIVE
#else
	slots_free = 0;
#endif
	
	if (allowed_max_uploads <= uq->queue->active_uploads - slots_free) {
		return FALSE;
	}
	
	/*
	 * Step 3. Check if current upload may have this slot
	 *         That is when the current upload is the first upload in its
	 *         queue which has no upload slot. Or if a earlier queued item is
	 *		   allready downloading something in another queue.
	 */
	
	for (l = g_list_first(uq->queue->by_rel_pos); l; l = l->next) {
		struct parq_ul_queued *parq_ul = (struct parq_ul_queued*) l->data;
	
		if (
			  !parq_ul->has_slot && parq_ul != uq && 
			  parq_ul->by_ip->ip != uq->by_ip->ip && !parq_ul->by_ip->uploading
			) {
			/* Another upload in the current queue is allowed first */
			if (slots_free < 0) {
				return FALSE;
			}
			slots_free--;
		} else
		if (parq_ul == uq || parq_ul->by_ip->ip == uq->by_ip->ip)
			/*
			 * So the current upload is the first in line (we would have
			 * returned FALSE otherwise by now).
			 * We also check on ip slot (instead of only the requested file-
			 * name). This is allowed as PARQ is a slot reservation system. So
			 * we check if the requesting host has another queued item which
			 * is allowed to continue. We will just use that position here then.
			 */
			return TRUE;
	}
	
	/* We should never make it here */
	g_warning("PARQ UL: "
		"Error while determining wether an upload should continue");
	
	return FALSE;
}


/*
 * parq_upload_update_ip_and_name
 *
 * Updates the IP and name entry in the queued structure and makes sure the hash
 * table remains in sync
 */
static void parq_upload_update_ip_and_name(struct parq_ul_queued *parq_ul, 
	gnutella_upload_t *u)
{
	g_assert(parq_ul != NULL);
	
	if (parq_ul->ip_and_name != NULL) {
		g_hash_table_remove(ul_all_parq_by_ip_and_name, parq_ul->ip_and_name);
		G_FREE_NULL(parq_ul->ip_and_name);
		parq_ul->name = NULL;
	}
	
	parq_ul->ip_and_name = g_strdup_printf("%u %s", u->ip, u->name);
	parq_ul->name = strchr(parq_ul->ip_and_name, ' ') + 1;
	
	g_hash_table_insert(ul_all_parq_by_ip_and_name, parq_ul->ip_and_name, 
		parq_ul);
}

/*
 * parq_ul_rel_pos_cmp
 * 
 * Function used to keep the relative position list sorted by relative position.
 * It should never be possible for 2 downloads to have the same relative
 * position in the same queue.
 */
static gint parq_ul_rel_pos_cmp(gconstpointer a, gconstpointer b)
{
	const struct parq_ul_queued *as = (const struct parq_ul_queued *) a;
	const struct parq_ul_queued *bs = (const struct parq_ul_queued *) b;

	g_assert(as->relative_position != bs->relative_position);
		
	return as->relative_position - bs->relative_position;
}
/*
 * parq_upload_get
 *
 * Get a queue slot, either existing or new.
 * Return slot as an opaque handle, NULL if slot cannot be created.
 */
gpointer parq_upload_get(gnutella_upload_t *u, header_t *header)
{
	struct parq_ul_queued *parq_ul = NULL;
	gchar *buf;

	g_assert(u != NULL);
	g_assert(header != NULL);

	/*
	 * Try to locate by ID first. If this fails, try to locate by IP and file
	 * name. We want to locate on ID first as a client may reuse an ID.
	 * Avoid abusing an PARQ entry by reusing an ID which allready finished
	 * uploading.
	 */
	
	parq_ul = parq_upload_find_id(u, header);
	
	if (parq_ul != NULL) {
		if (!parq_ul->had_slot)
			goto cleanup;
	}

	parq_ul = parq_upload_find(u);

	if (parq_ul == NULL) {
		/*
		 * Current upload is not queued yet. If the queue isn't full yet,
		 * always add the upload in the queue. 
		 */
		
		if (parq_upload_queue_full(u))
			return NULL;

		parq_ul = parq_upload_create(u);

		g_assert(parq_ul != NULL);

		if (dbg > 3)
			printf("PARQ UL Q %d/%d (%3d[%3d]/%3d) ETA: %s Added: %s '%s'\n",
				g_list_position(ul_parqs,
					g_list_find(ul_parqs, parq_ul->queue)) + 1,
				g_list_length(ul_parqs),
				parq_ul->position, 
				parq_ul->relative_position,
				parq_ul->queue->size,
				short_time(parq_upload_lookup_eta(u)),
				ip_to_gchar(parq_ul->remote_ip),
				parq_ul->name);
	}

cleanup:
	g_assert(parq_ul != NULL);

	if (parq_ul->queue->by_date_dead != NULL &&
		  g_list_find(parq_ul->queue->by_date_dead, parq_ul) != NULL)
		parq_ul->queue->by_date_dead = 
			  g_list_remove(parq_ul->queue->by_date_dead, parq_ul);
	
	/*
     * It is possible the client reused its ID for another file name, which is
	 * a valid thing to do. So make sure we have still got the IP and name
	 * in sync
	 */

	parq_upload_update_ip_and_name(parq_ul, u);
	
	if (!parq_ul->is_alive) {
		parq_ul->queue->alive++;
		parq_ul->is_alive = TRUE;
		parq_upload_update_relative_position(parq_ul);
		g_assert(parq_ul->queue->alive > 0);

		/* Insert again, in the relative position list. */
		parq_ul->queue->by_rel_pos = 
			g_list_insert_sorted(parq_ul->queue->by_rel_pos, parq_ul, 
				  parq_ul_rel_pos_cmp);	
		parq_upload_update_eta(parq_ul->queue);
	}
		
	buf = header_get(header, "X-Queue");
	
	if (buf != NULL)			/* Remote server does support queues */
		get_header_version(buf, &parq_ul->major, &parq_ul->minor);
	
	/*
	 * Update listening IP and port information
	 *
	 * Sepcs 1.0 defined X-Listen-IP, but 1.0.a corrected to X-Node.
	 * Parse both, but only emit X-Node from now on.
	 *		--RAM, 11/05/2003
	 */

	if (parq_ul->major >= 1) {					/* Only if PARQ advertised */
		buf = header_get(header, "X-Node");
		if (buf == NULL)
			buf = header_get(header, "X-Listen-Ip");	/* Case normalized */

		if (buf != NULL && gchar_to_ip_port(buf, &parq_ul->ip, &parq_ul->port))
			parq_ul->flags &= ~PARQ_UL_NOQUEUE;
	}
	
	return parq_ul;
}

/*
 * parq_upload_request_force
 *
 * If the download may continue, true is returned. False otherwise (which 
 * probably means the upload is queued).
 * Where parq_upload_request honours the number of upload slots, this one
 * is used for dynamic slot allocation.
 * This function expects that the upload was checked with parq_upload_request
 * first.
 */
gboolean parq_upload_request_force(gnutella_upload_t *u, gpointer handle, 
	  guint used_slots)
{
	struct parq_ul_queued *parq_ul = handle_to_queued(handle);
	
	/*
	 * Check whether the current upload is allowed to get an upload slot. If so
	 * move other queued items after the current item up one position in the
	 * queue
	 */
	if (max_uploads - used_slots > 0)
		/* Again no!. We are not out of upload slots yet. So there is no reason
		 * to let it continue now */
		return FALSE;
	
	if (parq_upload_continue(parq_ul, 1)) {
		if (u->status == GTA_UL_QUEUED)
			u->status = GTA_UL_SENDING;
		
		return TRUE;
	} else {
		return FALSE;
	}
}

/*
 * parq_upload_request
 *
 * If the download may continue, true is returned. False otherwise (which 
 * probably means the upload is queued).
 */
gboolean parq_upload_request(gnutella_upload_t *u, gpointer handle, 
	  guint used_slots)
{
	struct parq_ul_queued *parq_ul = handle_to_queued(handle);
	time_t now = time((time_t *) NULL);
	time_t org_retry = parq_ul->retry; 
	
	parq_ul->chunk_size = abs(u->skip - u->end);
	parq_ul->updated = now;
	parq_ul->retry = now + parq_ul_calc_retry(parq_ul);
		
#if AGRESSIVE
	/* If the chunk sizes are really small, expire them sooner */
	parq_ul->expire = parq_ul->retry + parq_ul->chunk_size / bw_http_out;
	parq_ul->expire = MIN(MIN_LIFE_TIME + parq_ul->retry, parq_ul->expire);
#else
	parq_ul->expire = MIN_LIFE_TIME + parq_ul->retry;
#endif
	
	if (
		org_retry > now && 
		!(
			(parq_ul->flags & PARQ_UL_QUEUE_SENT) || 
			u->status == GTA_UL_QUEUE_WAITING
		)
	) {
		/*
		 * Bad bad client, re-requested within the Retry-After interval.
		 * we are not going to allow this download. Wether it could get an
		 * upload slot or not. Neither are we going to active queue it.
		 */
		g_warning("[PARQ UL] "
			"host %s (%s) re-requested \"%s\" too soon (%d secs early)", 
			ip_port_to_gchar(u->socket->ip, u->socket->port), 
			upload_vendor_str(u),
			u->name, (gint) (org_retry - now));

		if (parq_ul->ban_timeout > now) {
			/*
			 * Bye bye, the client did it again, and is removed from the PARQ
		 	 * queue now.
			 */

			g_warning("[PARQ UL] "
				"punishing %s (%s) for re-requesting \"%s\" %d secs early", 
				ip_port_to_gchar(u->socket->ip, u->socket->port), 
				upload_vendor_str(u),
				u->name, (gint) (org_retry - now));

			parq_upload_force_remove(u);
			return FALSE;
		}
		
		parq_ul->ban_timeout = now + parq_upload_ban_window;
		return FALSE;
	}

	/*
	 * If we sent a QUEUE message and we're getting a reply, reset the
	 * amount of QUEUE messages sent amd clear the flag.
	 */

	if (parq_ul->flags & PARQ_UL_QUEUE_SENT) {
		parq_ul->queue_sent = 0;
		parq_ul->flags &= ~PARQ_UL_QUEUE_SENT;
	}

	/*
	 * Client was already downloading a segment, segment was finished and 
	 * just did a follow up request.
	 */

	if (parq_ul->has_slot)
		return TRUE;

	/*
	 * Check whether the current upload is allowed to get an upload slot. If so
	 * move other queued items after the current item up one position in the
	 * queue
	 */

	if (parq_upload_continue(parq_ul, max_uploads - used_slots))
		return TRUE;
	else {
		if (parq_ul->relative_position <= parq_upload_active_size) {
			if (parq_ul->minor > 0 || parq_ul->major > 0)
				u->status = GTA_UL_QUEUED;
		}
		u->parq_status = TRUE;		/* XXX would violate encapsulation */
		return FALSE;
	}
}

/*
 * parq_upload_busy
 *
 * Mark an upload as really being active instead of just being queued.
 */
void parq_upload_busy(gnutella_upload_t *u, gpointer handle)
{
	struct parq_ul_queued *parq_ul = handle_to_queued(handle);
	
	g_assert(parq_ul != NULL);
	
	if (dbg > 2) {
		printf("PARQ UL: Upload %d[%d] is busy\n",
		  	  parq_ul->position, parq_ul->relative_position);
	}
	
	u->parq_status = FALSE;			/* XXX -- get rid of `parq_status'? */
	
	if (parq_ul->has_slot)
		return;

	/* XXX Perhaps it is wise to update the parq_ul->remote_ip here.
	 * XXX However, we should also update the parq_by_ip and all related
	 * XXX uploads.
	 */
	
	g_assert(parq_ul->by_ip != NULL);
	g_assert(parq_ul->by_ip->ip == parq_ul->remote_ip);
	
	
	parq_ul->has_slot = TRUE;
	parq_ul->had_slot = TRUE;
	parq_ul->queue->active_uploads++;
	parq_ul->by_ip->uploading++;
}

void parq_upload_add(gnutella_upload_t *u)
{
	/*
	 * Cosmetic. Not used at the moment. gnutella_upload_t structure probably
	 * isn't complete yet at this moment
	 */	
}

void parq_upload_force_remove(gnutella_upload_t *u)
{
	struct parq_ul_queued *parq_ul = parq_upload_find(u);
		
	if (parq_ul != NULL) {
		if (!parq_upload_remove(u))
			parq_upload_free(parq_ul);
	}
}

/*
 * parq_upload_remove
 *
 * When an upload is removed this function should be called so parq
 * knows the current upload status of an upload.
 * 
 * @return TRUE if the download was totally removed. And the associated memory
 * was cleared. FALSE if the parq structure still exists.
 */
gboolean parq_upload_remove(gnutella_upload_t *u)
{
	gboolean return_result = FALSE; /* True if the upload was really removed
									   ie: Removed from memory */
	time_t now = time((time_t *) NULL);
	struct parq_ul_queued *parq_ul = NULL;

	g_assert(u != NULL);

	/*
	 * Avoid removing an upload which is being removed because we are returning
	 * a busy (503), in which case the upload got queued
	 */
		
	if (u->parq_status) {
		u->parq_status = FALSE;
		return FALSE;
	}
	
	parq_ul = parq_upload_find(u);

	/* If parq_ul = NULL, than the upload didn't get a slot in the PARQ. */
	if (parq_ul == NULL)
		return FALSE;

	/*
	 * If we're still in the GTA_UL_QUEUE_WAITING state, we did not get any
	 * HTTP requesst after sending the QUEUE callback.  However, if we sent
	 * a QUEUE request and went further, reset the amount of refused QUEUE.
	 *		--RAM, 17/05/2003
	 */

	if (dbg > 2 && (parq_ul->flags & PARQ_UL_QUEUE_SENT))
		printf("PARQ UL Q %d/%d: QUEUE #%d sent [refused=%d], u->status = %d\n",
			g_list_position(ul_parqs, 
				g_list_find(ul_parqs, parq_ul->queue)) + 1,
			g_list_length(ul_parqs),
			parq_ul->queue_sent, parq_ul->queue_refused, u->status);

	if (u->status == GTA_UL_QUEUE_WAITING)
		parq_ul->queue_refused++;
	else if (parq_ul->flags & PARQ_UL_QUEUE_SENT)
		parq_ul->queue_refused = 0;

	parq_ul->flags &= ~PARQ_UL_QUEUE_SENT;

	if (parq_ul->has_slot && u->keep_alive && u->status == GTA_UL_WAITING) {
		printf("**** PARQ UL Q %d/%d: Not removed, waiting for new request\n",
			g_list_position(ul_parqs, 
				g_list_find(ul_parqs, parq_ul->queue)) + 1,
			g_list_length(ul_parqs));
		return FALSE;
	}
	
	/*
	 * When the upload was actively queued, the last_update timestamp was
	 * set to somewhere in the feature to avoid early removal. However, now we
	 * do want to remove the upload.
	 */
	if (u->status == GTA_UL_QUEUED && u->last_update > now) {
		u->last_update = parq_ul->updated;
	}
	
	if (dbg > 3)
		printf("PARQ UL Q %d/%d: Upload removed\n",
			g_list_position(ul_parqs, 
				g_list_find(ul_parqs, parq_ul->queue)) + 1,
			g_list_length(ul_parqs));

	if (parq_ul->has_slot) {
		GList *lnext = NULL;
		
		if (dbg > 2)
			printf("PARQ UL: Freed an upload slot\n");

		g_assert(parq_ul->by_ip != NULL);
		g_assert(parq_ul->by_ip->uploading > 0);
		g_assert(parq_ul->by_ip->ip == parq_ul->remote_ip);

		parq_ul->by_ip->uploading--;
		parq_ul->queue->active_uploads--;
		
		/* Tell next waiting upload that a slot is available, using QUEUE */
		for (lnext = g_list_first(parq_ul->queue->by_rel_pos); lnext != NULL;
			  lnext = g_list_next(lnext)) {
				struct parq_ul_queued *parq_ul_next =
					  (struct parq_ul_queued *) lnext->data;
				
			if (!parq_ul_next->has_slot) {
				g_assert(parq_ul_next->queue->active <= 1);
				if (!(parq_ul_next->flags & (PARQ_UL_QUEUE|PARQ_UL_NOQUEUE)))
					parq_upload_send_queue(parq_ul_next);
				break;
			}
		}
	}
	
	g_assert(parq_ul->queue->active_uploads >= 0);	
	
	if (parq_ul->disc_timeout > now && parq_ul->has_slot) {
		/* Client disconnects to often. This could block our upload
		 * slots. Sorry, but we are going to remove this upload */
		g_warning("[PARQ UL] "
			"Removing %s (%s) for too many disconnections \"%s\" %d secs early",
			ip_port_to_gchar(u->socket->ip, u->socket->port), 
			upload_vendor_str(u),
			u->name, (gint) (parq_ul->disc_timeout - now));
		parq_upload_free(parq_ul);
		return_result = TRUE;
	} else {
		/*
		 * A client is not allowed to disconnect over and over again
		 * (ie data write error). Set the time for which a client
		 * should not disconnect
		 */
		if (parq_ul->has_slot)
			parq_ul->disc_timeout = now + parq_upload_ban_window;

		/* Disconnected upload is allowed to reconnect immediatly */
		parq_ul->has_slot = FALSE;
		parq_ul->retry = now;
		
		/*
		 * The upload slot expires rather soon to speed up uploading. This 
		 * doesn't prevent a broken connection from reconnecting though, it is
		 * just not garanteed anymore that it will regain its upload slot
		 * immediatly
		 */
#if 1
		parq_ul->expire = now + 1;
#else
		parq_ul->expire = now + MIN_LIFE_TIME;
#endif
	}
	
	return return_result;
}

/*
 * parq_upload_add_header
 * 
 * Adds X-Queued status in the HTTP reply header for a queued upload.
 *
 * `buf' is the start of the buffer where the headers are to be added.
 * `retval' contains the length of the buffer initially, and is filled
 * with the amount of data written.
 *
 * NB: Adds a Retry-After field for servents that will not understand PARQ,
 * to make sure they do not re-request too soon.
 */
void parq_upload_add_header(gchar *buf, gint *retval, gpointer arg)
{	
	gint rw = 0;
	gint length = *retval;
	time_t now = time((time_t *) NULL);
	struct upload_http_cb *a = (struct upload_http_cb *) arg;

	g_assert(buf != NULL);
	g_assert(retval != NULL);
	g_assert(a->u != NULL);
	
	if (parq_upload_queued(a->u)) {
		struct parq_ul_queued *parq_ul = 
			(struct parq_ul_queued *) parq_upload_find(a->u);
				
		if (parq_ul->major == 0 && parq_ul->minor == 1 && 
				  a->u->status == GTA_UL_QUEUED) {
			g_assert(length > 0);
			rw = gm_snprintf(buf, length,
				"X-Queue: "
				"position=%d, length=%d, limit=%d, pollMin=%d, pollMax=%d\r\n",
				parq_ul->relative_position,
				parq_ul->queue->size,
				1,
				MAX((gint32) (parq_ul->retry - now), 0), 
				MAX((gint32) (parq_ul->expire - now), 0));
		} else {
			g_assert(length > 0);

			rw = gm_snprintf(buf, length,
				"X-Queue: %d.%d\r\n"
				"X-Queued: position=%d; ID=%s; length=%d; ETA=%d; lifetime=%d"
					  "\r\n"
				"Retry-After: %d\r\n",
				PARQ_VERSION_MAJOR, PARQ_VERSION_MINOR,
				parq_ul->relative_position,
				parq_ul->id,
				parq_ul->queue->size,
				parq_ul->eta,
				MAX((gint32) (parq_ul->expire - now), 0),
				MAX((gint32)  (parq_ul->retry - now ), 0));
		
			/*
			 * If we filled all the buffer, try with a shorter string, bearing
			 * only the minimal amount of information.
			 */
			g_assert(length > 0);	
			if (rw == length - 1 && buf[rw - 1] != '\n')
				rw = gm_snprintf(buf, length,
					"X-Queue: %d.%d\r\n"
					"X-Queued: ID=%s; lifetime=%d\r\n",
					PARQ_VERSION_MAJOR, PARQ_VERSION_MINOR,
					parq_upload_lookup_id(a->u),
					MAX((gint32)  (parq_ul->retry - now ), 0));

			parq_ul->flags |= PARQ_UL_ID_SENT;
		}
	}

	g_assert(rw < length);
	
	*retval = rw;
}

/*
 * parq_upload_add_header_id
 * 
 * Adds X-Queued status in the HTTP reply header showing the queue ID
 * for an upload getting a slot.
 *
 * `buf' is the start of the buffer where the headers are to be added.
 * `retval' contains the length of the buffer initially, and is filled
 * with the amount of data written.
 */
void parq_upload_add_header_id(gchar *buf, gint *retval, gpointer arg)
{	
	gint rw = 0;
	gint length = *retval;
	struct upload_http_cb *a = (struct upload_http_cb *) arg;
	struct parq_ul_queued *parq_ul;

	g_assert(buf != NULL);
	g_assert(retval != NULL);
	g_assert(a->u != NULL);
	
	parq_ul = (struct parq_ul_queued *) parq_upload_find(a->u);

	g_assert(a->u->status == GTA_UL_SENDING);
	g_assert(parq_ul != NULL);

	/*
	 * If they understand PARQ, we also give them a queue ID even
	 * when they get an upload slot.  This will allow safe resuming
	 * should the connection be broken while the upload is active.
	 *		--RAM, 17/05/2003
	 */

	if (parq_ul->major >= 1) {
		rw = gm_snprintf(buf, length,
			"X-Queue: %d.%d\r\n"
			"X-Queued: ID=%s\r\n",
			PARQ_VERSION_MAJOR, PARQ_VERSION_MINOR,
			parq_upload_lookup_id(a->u));

		parq_ul->flags |= PARQ_UL_ID_SENT;
	}

	g_assert(rw < length);
	
	*retval = rw;
}

/*
 * parq_ul_id_sent
 *
 * Determines whether the PARQ ID was already sent for an upload.
 */
gboolean parq_ul_id_sent(gnutella_upload_t *u)
{
	struct parq_ul_queued *parq_ul = parq_upload_find(u);

	return parq_ul != NULL && (parq_ul->flags & PARQ_UL_ID_SENT);
}

/*
 * parq_upload_lookup_position
 *
 * Returns the current queueing position of an upload. Returns a value of 
 * (guint) -1 if not found.
 */
guint parq_upload_lookup_position(gnutella_upload_t *u)
{
	struct parq_ul_queued *parq_ul = NULL;
	
	g_assert(u != NULL);
	
	parq_ul = parq_upload_find(u);

	if (parq_ul != NULL) {
		return parq_ul->relative_position;
	} else {
		return (guint) -1;	
	}
}

/*
 * parq_upload_lookup_id
 * 
 * Returns the current ID of the upload.
 */
gchar* parq_upload_lookup_id(gnutella_upload_t *u)
{
	struct parq_ul_queued *parq_ul = NULL;
	
	g_assert(u != NULL);
	
	parq_ul = parq_upload_find(u);

	if ( parq_ul != NULL)
		return parq_ul->id;
	else		
		return NULL;
}

/*
 * parq_upload_lookup_ETA
 *
 * Returns the Estimated Time of Arrival for an upload slot for a given upload.
 */
guint parq_upload_lookup_eta(gnutella_upload_t *u)
{
	struct parq_ul_queued *parq_ul;
	
	parq_ul = parq_upload_find(u);
	
	/* If parq_ul == NULL the current upload isn't queued and ETA is unknown */
	if (parq_ul != NULL)
		return parq_ul->eta;
	else
		return (guint) -1;
}

/*
 * parq_upload_lookup_size
 *
 * Returns the current upload queue size of alive uploads.
 */
guint parq_upload_lookup_size(gnutella_upload_t *u)
{
	struct parq_ul_queued *parq_ul = NULL;
	
	/*
	 * There can be multiple queues. Find the queue in which the upload is
	 * queued.
	 */
	
	parq_ul = parq_upload_find(u);
	
	if (parq_ul != NULL) {
		g_assert(parq_ul->queue != NULL);
		
		return parq_ul->queue->alive;
	} else {
		/* No queue created yet */
		return 0;
	}
}

/*
 * parq_upload_lookup_lifetime
 *
 * Returns the lifetime of a queued upload
 */
time_t parq_upload_lookup_lifetime(gnutella_upload_t *u)
{
	struct parq_ul_queued *parq_ul = NULL;
	
	/*
	 * There can be multiple queues. Find the queue in which the upload is
	 * queued.
	 */
	
	parq_ul = parq_upload_find(u);
	
	if (parq_ul != NULL) {
		return parq_ul->expire;
	} else {
		/* No queue created yet */
		return 0;
	}	
}

/*
 * parq_upload_lookup_retry
 *
 * Returns the time_t at which the next retry is expected
 */
time_t parq_upload_lookup_retry(gnutella_upload_t *u)
{
	struct parq_ul_queued *parq_ul = NULL;
	
	/*
	 * There can be multiple queues. Find the queue in which the upload is
	 * queued.
	 */
	
	parq_ul = parq_upload_find(u);
	
	if (parq_ul != NULL) {
		return parq_ul->retry;
	} else {
		/* No queue created yet */
		return 0;
	}	
}

/*
 * parq_upload_lookup_queue_no
 *
 * Returns the queue number the current upload is queued in.
 */
guint parq_upload_lookup_queue_no(gnutella_upload_t *u)
{
	struct parq_ul_queued *parq_ul = NULL;
	
	parq_ul = parq_upload_find(u);
	
	if (parq_ul != NULL) {
		return g_list_position(ul_parqs, 
			  g_list_find(ul_parqs, parq_ul->queue)) + 1;
	} else {
		/* No queue created yet */
		return 0;
	}	
}


/*
 * parq_upload_update_relative_position
 *
 * Updates the relative position of all queued after the given queued
 * item
 */

static void parq_upload_update_relative_position(
	  struct parq_ul_queued *cur_parq_ul)
{
	GList *l = NULL;
	guint rel_pos = cur_parq_ul->relative_position;
	
	g_assert(cur_parq_ul != NULL);
	g_assert(cur_parq_ul->queue != NULL);
	g_assert(cur_parq_ul->queue->by_position != NULL);
	g_assert(cur_parq_ul->queue->size > 0);
	g_assert(rel_pos > 0);

	l = g_list_find(cur_parq_ul->queue->by_position, cur_parq_ul);
	
	if (cur_parq_ul->is_alive)
		rel_pos++;
	
	for (l = g_list_next(l); l; l = g_list_next(l)) {
		struct parq_ul_queued *parq_ul = (struct parq_ul_queued *) l->data;
		
		g_assert(parq_ul != NULL);

		parq_ul->relative_position = rel_pos;

		if (parq_ul->is_alive) 
			rel_pos++;

		g_assert(parq_ul->relative_position > 0);
	}
}

/* 
 * parq_upload_decrease_all_after
 *
 * Decreases the position of all queued items after the given queued item.
 */
static void parq_upload_decrease_all_after(struct parq_ul_queued *cur_parq_ul)
{
	GList *l;
	gint pos_cnt = 0;	/* Used for assertion */
	
	g_assert(cur_parq_ul != NULL);
	g_assert(cur_parq_ul->queue != NULL);
	g_assert(cur_parq_ul->queue->by_position != NULL);
	g_assert(cur_parq_ul->queue->size > 0);
	
	l = g_list_find(cur_parq_ul->queue->by_position, cur_parq_ul);
	pos_cnt = ((struct parq_ul_queued *) l->data)->position;

	l = g_list_next(l);	/* Decrease _after_ current parq */
	
	/*
	 * Cycle through list and decrease all positions by one. Position should
	 * never reach 0 which would mean the queued item is currently uploading
	 */
	for (;	l; l = g_list_next(l)) {
		struct parq_ul_queued *parq_ul = (struct parq_ul_queued *) l->data;

		g_assert(parq_ul != NULL);
		parq_ul->position--;
		
		g_assert(parq_ul->position == pos_cnt);

		pos_cnt++;
		g_assert(parq_ul->position > 0);
	}
}

void parq_upload_send_queue(struct parq_ul_queued *parq_ul)
{
	g_assert(!(parq_ul->flags & PARQ_UL_QUEUE));

	/* No known connect back port / ip */
	if (parq_ul->port == 0 || parq_ul->ip == 0) {
		if (dbg > 2) {
			printf("PARQ UL Q %d/%d (%3d[%3d]/%3d): "
				"No port to send QUEUE: %s '%s'\n\r",
				  g_list_position(ul_parqs, 
				  g_list_find(ul_parqs, parq_ul->queue)) + 1,
				  g_list_length(ul_parqs), 
				  parq_ul->position, 
				  parq_ul->relative_position,
				  parq_ul->queue->size,
				  ip_to_gchar(parq_ul->remote_ip),
				  parq_ul->name
			);
		}
		parq_ul->flags |= PARQ_UL_NOQUEUE;
		return;
	}

	ul_parq_queue = g_list_append(ul_parq_queue, parq_ul);
	parq_ul->flags |= PARQ_UL_QUEUE;
}

/*
 * parq_upload_do_send_queue
 *
 * Sends a QUEUE to a parq enabled client
 */
void parq_upload_do_send_queue(struct parq_ul_queued *parq_ul)
{
	struct gnutella_socket *s;
	gnutella_upload_t *u;

	g_assert(parq_ul->flags & PARQ_UL_QUEUE);

	parq_ul->last_queue_sent = time(NULL);		/* We tried... */
	parq_ul->queue_sent++;

	if (dbg)
		printf("PARQ UL Q %d/%d (%3d[%3d]/%3d): "
			"Sending QUEUE #%d to %s: '%s'\n\r",
			  g_list_position(ul_parqs, 
			  g_list_find(ul_parqs, parq_ul->queue)) + 1,
			  g_list_length(ul_parqs), 
			  parq_ul->position, 
			  parq_ul->relative_position,
			  parq_ul->queue->size,
			  parq_ul->queue_sent,
			  ip_port_to_gchar(parq_ul->ip, parq_ul->port),
			  parq_ul->name);

	s = socket_connect(parq_ul->ip, parq_ul->port, SOCK_TYPE_UPLOAD);
	
	if (!s) {
		g_warning("[PARQ UL] could not send QUEUE #%d to %s (can't connect)",
			parq_ul->queue_sent, ip_port_to_gchar(parq_ul->ip, parq_ul->port));
		return;
	}

	u = upload_create(s, TRUE);
	
	u->status = GTA_UL_QUEUE;
	u->name = atom_str_get(parq_ul->name);
	parq_upload_update_ip_and_name(parq_ul, u);
	upload_fire_upload_info_changed(u);
	
	/* Verify created upload entry */
	g_assert(parq_upload_find(u) != NULL);
}
	
/*
 * parq_upload_send_queue_conf
 *
 * 'Call back' connection was succesfull. So prepare to send headers
 */
void parq_upload_send_queue_conf(gnutella_upload_t *u)
{
	gchar queue[MAX_LINE_SIZE];
	struct parq_ul_queued *parq_ul = NULL;
	struct gnutella_socket *s;
	gint rw;
	gint sent;

	g_assert(u);
	g_assert(u->status == GTA_UL_QUEUE);
	g_assert(u->name);
	
	parq_ul = parq_upload_find(u);
	
	g_assert(parq_ul != NULL);

	/*
	 * Send the QUEUE header.
	 */

	rw = gm_snprintf(queue, sizeof(queue), "QUEUE %s %s\r\n",
		parq_ul->id, ip_port_to_gchar(listen_ip(), listen_port));
	
	s = u->socket;

	if (-1 == (sent = bws_write(bws.out, s->file_desc, queue, rw))) {
		g_warning("[PARQ UL] "
			"Unable to send back QUEUE for \"%s\" to %s: %s",
			  u->name, ip_port_to_gchar(s->ip, s->port), g_strerror(errno));
	} else if (sent < rw) {
		g_warning("[PARQ UL] "
			"Only sent %d out of %d bytes of QUEUE for \"%s\" to %s: %s",
			  sent, rw, u->name, ip_port_to_gchar(s->ip, s->port),
			  g_strerror(errno));
	} else if (dbg > 2) {
		printf("PARQ UL: Sent #%d to %s: %s",
			  parq_ul->queue_sent, ip_port_to_gchar(s->ip, s->port), queue);
		fflush(stdout);
	}

	if (sent != rw) {
		upload_remove(u, "Unable to send QUEUE #%d", parq_ul->queue_sent);
		return;
	}

	parq_ul->flags |= PARQ_UL_QUEUE_SENT;		/* We sent the QUEUE message */

	/*
	 * We're now expecting HTTP headers on the connection we've made.
	 */
	expect_http_header(u, GTA_UL_QUEUE_WAITING);
}

/*
 * parq_upload_save_queue
 *
 * Saves all the current queues and there items so it can be restored when the
 * client starts up again.
 */
void parq_upload_save_queue()
{
	gchar *file;
	FILE *f;
	time_t now = time((time_t *)NULL);
	GList *queues;

	if (dbg > 3)
		printf("PARQ UL: Trying to save all queue info\n");
	
	// XXX need to use te file routines -- RAM
	file = g_strdup_printf("%s/%s", settings_config_dir(), file_parq_file);
	f = fopen(file, "w");	

	if (!f) {
		g_warning("[PARQ UL] parq_upload_save_queue(): "
			  "unable to open file \"%s\" for writing: %s",
			  file, g_strerror(errno));
		g_free(file);
		return;
	}

	fputs("# THIS FILE IS AUTOMATICALLY GENERATED -- DO NOT EDIT\n#\n", f);
	fprintf(f, "# Saved on %s\n", ctime(&now));

	for (
		queues = g_list_last(ul_parqs) ; queues != NULL; queues = queues->prev
	) {
		struct parq_ul_queue *queue = (struct parq_ul_queue *) queues->data;

		g_list_foreach(queue->by_position, parq_store, f);
	}

	fclose(f);
	g_free(file);

	if (dbg > 3)
		printf("PARQ UL: All saved\n");

}

/*
 * parq_store
 *
 * Saves an individual queued upload to disc. This is the callback function
 * used by g_list_foreach in function parq_upload_save_queue
 */
static void parq_store(gpointer data, gpointer x)
{
	FILE *f = (FILE *)x;
	time_t now = time((time_t *) NULL);
	struct parq_ul_queued *parq_ul = (struct parq_ul_queued *) data;
	gchar xip[32];
	gchar ip[32];
	
	if (parq_ul->had_slot && !parq_ul->has_slot)
		/* We are not saving uploads which allready finished an upload */
		return;
	
	g_assert(NULL != f);
	if (dbg > 5)
		printf("PARQ UL Q %d/%d (%3d[%3d]/%3d): Saving ID: '%s' - %s '%s'\n",
			  g_list_position(ul_parqs, 
				  g_list_find(ul_parqs, parq_ul->queue)) + 1,
			  g_list_length(ul_parqs), 
			  parq_ul->position, 
			  parq_ul->relative_position,
			  parq_ul->queue->size,
			  parq_ul->id,
			  ip_to_gchar(parq_ul->remote_ip),
			  parq_ul->name);
	
	strncpy(ip, ip_to_gchar(parq_ul->remote_ip), sizeof(ip));
	strncpy(xip, ip_to_gchar(parq_ul->ip), sizeof(xip));
	/*
	 * Save all needed parq information. The ip and port information gathered
	 * from X-Node is saved as XIP and XPORT 
	 * The lifetime is saved as a relative value.
	 */
	fprintf(f, "QUEUE: %d\n"
		  "POS: %d\n"
		  "ENTERED: %d\n"
		  "EXPIRE: %d\n"
		  "ID: %s\n"
		  "SIZE: %d\n"
		  "XIP: %s\n"
		  "XPORT: %d\n"
		  "IP: %s\n"
		  "NAME: %s\n\n", 
		  g_list_position(ul_parqs, g_list_find(ul_parqs, parq_ul->queue)) + 1,
		  parq_ul->position,
		  (gint) parq_ul->enter,
		  (gint) parq_ul->expire - (gint) now,
		  parq_ul->id,
		  parq_ul->file_size,
		  xip,
		  parq_ul->port,
		  ip,
		  parq_ul->name);
}

/*
 * parq_upload_load_queue
 *
 * Loads the saved queue status back into memory
 */
static void parq_upload_load_queue(void)
{
	gchar *file;
	FILE *f;
	gchar line[1024];
	gboolean next = FALSE;
	gnutella_upload_t *u;
	struct parq_ul_queued *parq_ul;
	time_t now = time((time_t *) NULL);

	int queue = 0;
	int position = 0;
	int enter = 0;
	int expire = 0;
	int filesize = 0;
	guint32 ip = 0;
	guint32 xip = 0;
	int xport = 0;
	gint32 signed_ip = 0;
	char name[1024];
	char id[1024];
	gchar ip_copy[32];
	int ip1, ip2, ip3, ip4;
	
	// XXX need to use the file routines -- RAM
	file = g_strdup_printf("%s/%s", settings_config_dir(), file_parq_file);
	g_return_if_fail(NULL != file);

	f = fopen(file, "r");
	if (!f) {
		g_warning("[PARQ UL] parq_upload_load_queue(): "
			"unable to open file \"%s\" for reading: %s",
			file, g_strerror(errno));
		G_FREE_NULL(file);
		return;
	}
	G_FREE_NULL(file);
	
	u = walloc(sizeof(gnutella_upload_t));
	
	if (dbg)
		g_warning("[PARQ UL] Loading queue information");

	line[sizeof(line)-1] = '\0';

	while (fgets(line, sizeof(line), f)) {
		/* Skip comments */
		if (*line == '#') continue;
	
		if (!strncmp(line, "IP: ", 4)) {
			// XXX legacy -- remove after 0.92 is out -- RAM, 11/05/2003
			if (sscanf(line, "IP: -%u\n", &ip)) {
				sscanf(line, "IP: %d\n", &signed_ip);
				ip = (guint32) signed_ip;
				g_warning("[PARQ UL] Legacy import of IP: %d", signed_ip);
			} else {
				strncpy(ip_copy, line + sizeof("IP:"), sizeof(ip_copy));
				if (strchr(ip_copy, '\n') != NULL) {
					*strchr(ip_copy, '\n') = '\0';
					ip_copy[31] = '\0';
					if (sscanf(ip_copy, "%d.%d.%d.%d\n",
						  &ip1, &ip2, &ip3, &ip4) == 4) {
						ip = gchar_to_ip(ip_copy);
					} else {
						sscanf(line, "IP: %u\n", &ip);
						g_warning("[PARQ UL] "
							"Pre 0.92 (cvs only) import of IP: %u", ip);
					}
				}
			}
		}
		
		if (!strncmp(line, "XIP: ", 5)) {
			// XXX legacy -- remove after 0.92 is out -- RAM, 11/05/2003
			if (sscanf(line, "XIP: -%u\n", &xip)) {
				sscanf(line, "XIP: %d\n", &signed_ip);
				xip = (guint32) signed_ip;
				g_warning("[PARQ UL] Legacy import of XIP: %d", signed_ip);
			} else {
				strncpy(ip_copy, line + sizeof("XIP:"), sizeof(ip_copy));
				if (strchr(ip_copy, '\n') != NULL) {
					*strchr(ip_copy, '\n') = '\0';
					ip_copy[31] = '\0';
					if (sscanf(ip_copy, "%d.%d.%d.%d",
						  &ip1, &ip2, &ip3, &ip4) == 4) {
						xip = gchar_to_ip(ip_copy);
					} else {
						sscanf(line, "XIP: %u\n", &xip);
						g_warning("[PARQ UL] "
							"Pre 0.92 (cvs only) import of XIP: %u", xip);
					}
				}
			}
		}

		sscanf(line, "QUEUE: %d", &queue);
		sscanf(line, "POS: %d\n", &position);
		sscanf(line, "ENTERED: %d\n", &enter);
		sscanf(line, "EXPIRE: %d\n", &expire);
		sscanf(line, "SIZE: %d\n", &filesize);
		sscanf(line, "XPORT: %d\n", &xport);
		
		if (!strncmp(line, "ID: ", 4)) {
			gchar *newline;
			g_strlcpy(id, (line + sizeof("ID:")), sizeof(id));
			newline = strchr(id, '\n');
			if (newline)
				*newline = '\0';
		}		
		if (!strncmp(line, "NAME: ", 6)) {
			char *newline;

			g_strlcpy(name, (line + sizeof("NAME:")), sizeof(name));
			newline = strchr(name, '\n');
			
			if (newline)
				*newline = '\0';
			
			/* Expect next parq entry */
			next = TRUE;
		}
		
		if (next) {
			next = FALSE;
			
			/* Fill a fake upload structure */
			u->file_size = filesize;
			u->name = name;
			u->ip = ip;

			g_assert(u->name != NULL);
			
			parq_ul = parq_upload_create(u);
			
			g_assert(parq_ul != NULL);
	
			parq_ul->enter = enter;
			parq_ul->expire = now + expire;
			parq_ul->ip = xip;
			parq_ul->port = xport;
			
			/* During parq_upload_create already created an ID for us */
			g_hash_table_remove(ul_all_parq_by_id, parq_ul->id);
			g_free(parq_ul->id);
			parq_ul->id = g_strdup(id);
			g_hash_table_insert(ul_all_parq_by_id, parq_ul->id, parq_ul);
			
			if (dbg > 2)
				printf(
					"PARQ UL Q %d/%d (%3d[%3d]/%3d) ETA: %s Restored: %s '%s'\n",
					g_list_position(ul_parqs,
						g_list_find(ul_parqs, parq_ul->queue)) + 1,
					g_list_length(ul_parqs),
					parq_ul->position, 
				 	parq_ul->relative_position,
					parq_ul->queue->size,
					short_time(parq_upload_lookup_eta(u)),
					ip_to_gchar(parq_ul->remote_ip),
					parq_ul->name);						
			
			parq_upload_send_queue(parq_ul);
		}
	}
	
	wfree(u, sizeof(gnutella_upload_t));
}

/*
# vim:ts=4:sw=4
*/
