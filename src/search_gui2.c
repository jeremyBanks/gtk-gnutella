/*
 * $Id$
 *
 * Copyright (c) 2001-2003, Raphael Manfredi, Richard Eckart
 *
 * GUI filtering functions.
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

#include "gui.h"

#include "fileinfo.h"	/* FIXME: remove this dependency */

/* GUI includes  */
#include "search_gui_common.h"
#include "search_gui.h"
#include "search_cb2.h"
#include "gtk-missing.h"
#include "gui_property.h"
#include "settings_gui.h"

#ifdef USE_SEARCH_XML
# include "search_xml.h"
# include <libxml/parser.h>
#endif

/* System includes */
#include <ctype.h>
#include <gtk/gtk.h>

RCSID("$Id$");

#define MAX_TAG_SHOWN	60		/* Show only first chars of tag */

static gchar tmpstr[4096];

GList *searches = NULL;		/* List of search structs */

/* Need to remove this dependency on GUI further --RAM */
extern GtkWidget *default_scrolled_window;

static search_t *current_search  = NULL; /*	The search currently displayed */
search_t *search_selected = NULL;

static gchar *search_file = "searches";	/* File where searches are saved */

time_t tab_update_time = 5;

static GList *sl_search_history = NULL;
static gboolean search_gui_shutting_down = FALSE;

/*
 * Private function prototypes
 */
static GtkTreeViewColumn *add_column(GtkTreeView *treeview, gchar *name,
	gint id, gint width, gfloat xalign, gint fg_column, gint bg_column);
#ifndef USE_SEARCH_XML
    static void search_store_old(void);
#endif /* USE_SEARCH_XML */

/*
 * Human readable translation of servent trailer open flags.
 * Decompiled flags are listed in the order of the table.
 */
static struct {
	guint32 flag;
	gchar *status;
} open_flags[] = {
	{ ST_BUSY,		"busy" },
	{ ST_UPLOADED,	"stable" },		/* Allows uploads -> stable */
	{ ST_FIREWALL,	"push" },
};

/*
 * If no search are currently allocated 
 */
GtkWidget *default_search_tree_view = NULL;
GtkWidget *default_scrolled_window = NULL;


/* ----------------------------------------- */

static void do_atom_sha1_free(gpointer key)
{
	atom_sha1_free(key);
}

static void add_parent_with_sha1(
	GHashTable *ht, gpointer key, GtkTreeIter *iter)
{
	g_hash_table_insert(ht, key, w_tree_iter_copy(iter));
}

static void remove_parent_with_sha1(GHashTable *ht, const gchar *sha1)
{
	gpointer key;
 
	key = atom_sha1_get(sha1);
	g_hash_table_remove(ht, key);
	atom_sha1_free(key);
}

static GtkTreeIter *find_parent_with_sha1(GHashTable *ht, gpointer key)
{
	GtkTreeIter *iter = NULL;
	gpointer *orig_key;

	if (g_hash_table_lookup_extended(ht, key,
			(gpointer) &orig_key, (gpointer) &iter))
		return iter;
		
	return NULL;
}

void search_gui_restart_search(search_t *sch)
{
	gtk_tree_store_clear(GTK_TREE_STORE(
			gtk_tree_view_get_model(GTK_TREE_VIEW(sch->tree_view))));
	search_gui_clear_search(sch);
	sch->items = sch->unseen_items = 0;
	gui_search_update_items(sch);
	search_reissue(sch->search_handle);
}

static gboolean always_true(gpointer key, gpointer value, gpointer x)
{
	return TRUE;
}

/*
 * dec_records_refcount
 *
 * Decrement refcount of hash table key entry.
 */
static gboolean dec_records_refcount(gpointer key, gpointer value, gpointer x)
{
	struct record *rc = (struct record *) key;

	g_assert(rc->refcount > 0);

	rc->refcount--;
	return TRUE;
}

/*
 * search_clear
 *
 * Clear all results from search.
 */
void search_gui_clear_search(search_t *sch)
{
	g_assert(sch);
	g_assert(sch->dups);

	/*
	 * Before invoking search_free_r_sets(), we must iterate on the
	 * hash table where we store records and decrement the refcount of
	 * each record, and remove them from the hash table.
	 *
	 * Otherwise, we will violate the pre-condition of search_free_record(),
	 * which is there precisely for that reason!
	 */
	g_hash_table_foreach_remove(sch->dups, dec_records_refcount, NULL);
	search_gui_free_r_sets(sch);
	g_hash_table_foreach_remove(sch->parents, always_true, NULL);

	sch->items = sch->unseen_items = 0;
}

/* 
 * search_gui_close_search:
 *
 * Remove the search from the list of searches and free all 
 * associated ressources (including filter and gui stuff).
 */
void search_gui_close_search(search_t *sch)
{
    g_assert(sch != NULL);

    /*
     * We remove the search immeditaly from the list of searches,
     * because some of the following calls (may) depend on 
     * "searches" holding only the remaining searches. 
     * We may not free any ressources of "sch" yet, because 
     * the same calls may still need them!.
     *      --BLUE 26/05/2002
     */
 	searches = g_list_remove(searches, (gpointer) sch);

    search_gui_remove_search(sch);
	filter_close_search(sch);
	search_gui_clear_search(sch);
	g_hash_table_destroy(sch->dups);
	sch->dups = NULL;
	g_hash_table_destroy(sch->parents);
	sch->parents = NULL;

    search_close(sch->search_handle);
	atom_str_free(sch->query);

	G_FREE_NULL(sch);
}

/*
 * search_gui_new_search:
 * 
 * Create a new search and start it. Use default reissue timeout.
 */
gboolean search_gui_new_search(
	const gchar *query, flag_t flags, search_t **search)
{
    guint32 timeout;
    guint32 speed;
    
    gnet_prop_get_guint32_val(PROP_SEARCH_REISSUE_TIMEOUT, &timeout);
    gui_prop_get_guint32_val(PROP_DEFAULT_MINIMUM_SPEED, &speed);
	return search_gui_new_search_full(
        query, speed, timeout, SORT_NO_COL, SORT_NONE,
		flags | SEARCH_ENABLED, search);
}

/* 
 * search_gui_new_search_full:
 *
 * Create a new search and start it.
 * Returns TRUE if search was sucessfully created and FALSE if an error
 * happened. If the "search" argument is not NULL a pointer to the new
 * search is stored there.
 */
gboolean search_gui_new_search_full(
	const gchar *querystr, guint16 speed,
	guint32 reissue_timeout,
	gint sort_col, gint sort_order, flag_t flags, search_t **search)
{
	search_t *sch;
	GList *glist;
	GtkTreeStore *model;
	GtkTreeIter iter;
	static gchar query[512];

    GtkWidget *combo_searches = lookup_widget(main_window, "combo_searches");
    GtkWidget *tree_view_search = lookup_widget(main_window, "tree_view_search");
    GtkWidget *notebook_search_results = 
        lookup_widget(main_window, "notebook_search_results");
    GtkWidget *button_search_close = 
        lookup_widget(main_window, "button_search_close");
    GtkWidget *entry_search = lookup_widget(main_window, "entry_search");


	/*
	 * If the text is a magnet link we extract the SHA1 urn
	 * and put it back into the search field string so that the
	 * code for urn searches below can handle it.
	 *		--DBelius   11/11/2002
	 */

	g_strlcpy(query, querystr, sizeof(query));

	if (0 == strncasecmp(query, "magnet:", 7)) {
		guchar raw[SHA1_RAW_SIZE];

		if (huge_extract_sha1(query, raw)) {
			size_t len;

			len = g_strlcpy(query, "urn:sha1:", sizeof(query));
			if (len < sizeof(query))
				g_strlcpy(query + len, sha1_base32(raw), sizeof(query) - len);
		} else {
			return FALSE;		/* Entry refused */
		}
	}

	/*
	 * If string begins with "urn:sha1:", then it's an URN search.
	 * Validate the base32 representation, and if not valid, beep
	 * and refuse the entry.
	 *		--RAM, 28/06/2002
	 */

	if (0 == strncasecmp(query, "urn:sha1:", 9)) {
		guchar raw[SHA1_RAW_SIZE];
		gchar *b = query + 9;

		if (strlen(b) < SHA1_BASE32_SIZE)
			goto refused;

		if (base32_decode_into(b, SHA1_BASE32_SIZE, raw, sizeof(raw)))
			goto validated;

		/*
		 * If they gave us an old base32 representation, convert it to
		 * the new one on the fly.
		 */
		if (base32_decode_old_into(b, SHA1_BASE32_SIZE, raw, sizeof(raw))) {
			guchar b32[SHA1_BASE32_SIZE];
			base32_encode_into(raw, sizeof(raw), b32, sizeof(b32));
			memcpy(b, b32, SHA1_BASE32_SIZE);
			goto validated;
		}

		/*
		 * Entry refused.
		 */
	refused:
		return FALSE;

	validated:
		b[SHA1_BASE32_SIZE] = '\0';		/* Truncate to end of URN */

		/* FALL THROUGH */
	}

	sch = g_new0(search_t, 1);

	sch->query = atom_str_get(query);
	sch->enabled = (flags & SEARCH_ENABLED) ? TRUE : FALSE;
	sch->search_handle = search_new(query, speed, reissue_timeout, flags);
	sch->passive = (flags & SEARCH_PASSIVE) ? TRUE : FALSE;
	sch->dups = g_hash_table_new(
		search_gui_hash_func, search_gui_hash_key_compare);
	if (!sch->dups)
		g_error("new_search: unable to allocate hash table.");
	sch->parents = g_hash_table_new_full(NULL, NULL,
		do_atom_sha1_free, (gpointer) w_tree_iter_free);
    
  	filter_new_for_search(sch);

	/* Create the list item */

	sch->list_item = gtk_list_item_new_with_label(sch->query);
	gtk_widget_show(sch->list_item);
	glist = g_list_prepend(NULL, (gpointer) sch->list_item);
	gtk_list_prepend_items(GTK_LIST(GTK_COMBO(combo_searches)->list), glist);

	/* Create a new TreeView if needed, or use the default TreeView */

	if (searches) {
		/* We have to create a new clist for this search */
		gui_search_create_tree_view(&sch->scrolled_window, &sch->tree_view);

		gtk_object_set_user_data((GtkObject *) sch->scrolled_window,
								 (gpointer) sch);

		gtk_notebook_append_page(GTK_NOTEBOOK(notebook_search_results),
								 sch->scrolled_window, NULL);
	} else {
		/* There are no searches currently, we can use the default clist */

		if (default_scrolled_window && default_search_tree_view) {
			sch->scrolled_window = default_scrolled_window;
			sch->tree_view = default_search_tree_view;

			default_search_tree_view = default_scrolled_window = NULL;
		} else
			g_warning("new_search():"
				" No current search but no default tree_view !?");

		gtk_object_set_user_data((GtkObject *) sch->scrolled_window,
								 (gpointer) sch);
	}


	model = (GtkTreeStore *) gtk_tree_view_get_model(
		(GtkTreeView *) tree_view_search);
	gtk_tree_store_append(model, &iter, NULL);
	gtk_tree_store_set(model, &iter,
		c_sl_name, sch->query,
		c_sl_hit, 0,
		c_sl_new, 0, 
		c_sl_sch, sch,
		c_sl_fg, NULL,
		c_sl_bg, NULL,
		(-1));

	gui_search_update_tab_label(sch);
	sch->tab_updating = gtk_timeout_add(tab_update_time * 1000,
        (GtkFunction) gui_search_update_tab_label, sch);

    if (!searches) {
        GtkWidget * w = gtk_notebook_get_nth_page( 
            GTK_NOTEBOOK(notebook_search_results), 0);
    
		gtk_notebook_set_tab_label_text(
            GTK_NOTEBOOK(notebook_search_results),
            w, "(no search)");
    }

	g_signal_connect(GTK_OBJECT(sch->list_item), "select",
		G_CALLBACK(on_search_selected), (gpointer) sch);
	search_gui_set_current_search(sch);
	gtk_widget_set_sensitive(combo_searches, TRUE);
	gtk_widget_set_sensitive(button_search_close, TRUE);
    gtk_entry_set_text(GTK_ENTRY(entry_search), "");
	searches = g_list_append(searches, (gpointer) sch);

	if (sch->enabled)
		search_start(sch->search_handle);

	if (NULL != search)
		*search = sch;
	return TRUE;
}

/* Searches results */
static gint search_gui_compare_size_func(
    GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer user_data)
{
    record_t *rec_a = NULL;
	record_t *rec_b = NULL;

    gtk_tree_model_get(model, a, c_sr_record, &rec_a, (-1));
    gtk_tree_model_get(model, b, c_sr_record, &rec_b, (-1));
	return SIGN(rec_a->size, rec_b->size);
}

static gint search_gui_compare_count_func(
    GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer user_data)
{
	guint m = (guint) gtk_tree_model_iter_n_children(model, a);
	guint n = (guint) gtk_tree_model_iter_n_children(model, b);
	return SIGN(m, n);
}

#if 0
/* Who wants to see the IP addresses in the GtkTreeView, anyway? */
static gint search_gui_compare_host_func(
    GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer user_data)
{
    record_t *rec_a = NULL;
	record_t *rec_b = NULL;
	gint d;

    gtk_tree_model_get(model, a, c_sr_record, &rec_a, (-1));
    gtk_tree_model_get(model, b, c_sr_record, &rec_b, (-1));
	d = SIGN(rec_a->results_set->ip, rec_b->results_set->ip);

	return d != 0
		? d : SIGN(rec_b->results_set->port, rec_a->results_set->port);
}
#endif

static void search_gui_add_record(
	search_t *sch,
	record_t *rc,
	GString *vinfo,
	GdkColor *fg,
	GdkColor *bg,
	const gchar *status)
{
  	GString *info = g_string_sized_new(80);
  	gchar *info_utf8;
	GtkTreeIter *parent;
	GtkTreeIter iter;
	GtkTreeView *tree_view = GTK_TREE_VIEW(sch->tree_view);
	GtkTreeStore *model = (GtkTreeStore *) gtk_tree_view_get_model(tree_view);

	if (rc->tag) {
		guint len = strlen(rc->tag);

		/*
		 * We want to limit the length of the tag shown, but we don't
		 * want to loose that information.	I imagine to have a popup
		 * "show file info" one day that will give out all the
		 * information.
		 *				--RAM, 09/09/2001
		 */

		if (len > MAX_TAG_SHOWN) {
            gchar saved = rc->tag[MAX_TAG_SHOWN];
			rc->tag[MAX_TAG_SHOWN] = '\0';
			g_string_append(info, rc->tag);
			rc->tag[MAX_TAG_SHOWN] = saved;
		} else
			g_string_append(info, rc->tag);
	}
	if (vinfo->len) {
		if (info->len)
			g_string_append(info, "; ");
		g_string_append(info, vinfo->str);
	}
	/* strdup() info because it's normally shorter than filename */
	info_utf8 = g_strdup(locale_to_utf8(info->str, info->len));

	if (NULL != rc->sha1) {
		gpointer key;

		key = atom_sha1_get(rc->sha1);
		parent = find_parent_with_sha1(sch->parents, key);
		gtk_tree_store_append(model, &iter, parent);
		if (NULL != parent) {
			guint n;

			n = (guint) gtk_tree_model_iter_n_children(
                                (GtkTreeModel *) model, parent) + 1;
			/* Use a string to suppress showing 0 in the # column */
			gm_snprintf(tmpstr, sizeof(tmpstr), "%u", n);
			gtk_tree_store_set(model, parent, c_sr_count, tmpstr, (-1));
 			/* we need only the reference for the parent */
			atom_sha1_free(key);
		} else
			add_parent_with_sha1(sch->parents, key, &iter);
	} else
		gtk_tree_store_append(model, &iter, (parent = NULL));

	gtk_tree_store_set(model, &iter,
		      c_sr_filename, locale_to_utf8(rc->name, 0),
		      c_sr_size, NULL != parent ? NULL : short_size(rc->size),
		      c_sr_info, info_utf8,
		      c_sr_fg, fg,
		      c_sr_bg, bg,
		      c_sr_record, rc,
		      (-1));
	G_FREE_NULL(info_utf8);
	g_string_free(info, TRUE);
}

static void search_matched(search_t *sch, results_set_t *rs)
{
	guint32 old_items = sch->items;
   	gboolean need_push;			/* Would need a push to get this file? */
	gboolean skip_records;		/* Shall we skip those records? */
	GString *vinfo = g_string_sized_new(40);
	gchar *vendor;
    GdkColor *download_color;
    GdkColor *ignore_color;
    GdkColor *mark_color;
    GSList *l;
    gboolean send_pushes;
    gboolean is_firewalled;
	gint i;
	guint32 max_results;

    g_assert(sch != NULL);
    g_assert(rs != NULL);

    mark_color = &(gtk_widget_get_style(GTK_WIDGET(sch->tree_view))
        ->bg[GTK_STATE_INSENSITIVE]);

    ignore_color = &(gtk_widget_get_style(GTK_WIDGET(sch->tree_view))
        ->fg[GTK_STATE_INSENSITIVE]);

    download_color =  &(gtk_widget_get_style(GTK_WIDGET(sch->tree_view))
        ->fg[GTK_STATE_ACTIVE]);

    vendor = lookup_vendor_name(rs->vendor);

   	if (vendor) {
		g_string_append(vinfo, vendor);
		if (rs->version) {
			g_string_append(vinfo, "/");
			g_string_append(vinfo, rs->version);
		}
	}

	for (i = 0; i < G_N_ELEMENTS(open_flags); i++) {
		if (rs->status & open_flags[i].flag) {
			if (vinfo->len)
				g_string_append(vinfo, ", ");
			g_string_append(vinfo, open_flags[i].status);
		}
	}

	if (vendor && !(rs->status & ST_PARSED_TRAILER)) {
		if (vinfo->len)
			g_string_append(vinfo, ", ");
		g_string_append(vinfo, "<unparsed>");
	}

	/*
	 * If we're firewalled, or they don't want to send pushes, then don't
	 * bother displaying results if they need a push request to succeed.
	 *		--RAM, 10/03/2002
	 */
    gnet_prop_get_boolean(PROP_SEND_PUSHES, &send_pushes, 0, 1);
    gnet_prop_get_boolean(PROP_IS_FIREWALLED, &is_firewalled, 0, 1);

	need_push = (rs->status & ST_FIREWALL) || !host_is_valid(rs->ip, rs->port);
	skip_records = (!send_pushes || is_firewalled) && need_push;

	if (gui_debug > 6)
		g_warning("search_matched: [%s] got hit with %d record%s (from %s) "
			"need_push=%d, skipping=%d\n",
			sch->query, rs->num_recs, rs->num_recs == 1 ? "" : "s",
			ip_port_to_gchar(rs->ip, rs->port), need_push, skip_records);

  	for (l = rs->records; l && !skip_records; l = l->next) {
		record_t *rc = (record_t *) l->data;
        filter_result_t *flt_result;
        gboolean downloaded = FALSE;

        if (gui_debug > 7)
            g_warning("search_matched: [%s] considering %s (%s)\n",
				sch->query, rc->name, vinfo->str);

        /*
	     * If the size is zero bytes,
		 * or we don't send pushes and it's a private IP,
		 * or if this is a duplicate search result,
		 *
		 * Note that we pass ALL records through search_gui_result_is_dup(),
		 * to be able to update the index/GUID of our records correctly, when
		 * we detect a change.
		 */

       	if (
			search_gui_result_is_dup(sch, rc)    ||
			skip_records                    	 ||
			rc->size == 0
		)
			continue;

        flt_result = filter_record(sch, rc);

        /*
         * Check whether this record was already scheduled for
         * download by the backend.
         */
        downloaded = rc->flags & SR_DOWNLOADED;
        
        /*
         * Now we check for the different filter result properties.
         */

        /*
         * Check for FILTER_PROP_DOWNLOAD:
         */
        if (!downloaded &&
            (flt_result->props[FILTER_PROP_DOWNLOAD].state ==
            FILTER_PROP_STATE_DO)) {
            download_auto_new(rc->name, rc->size, rc->index, rs->ip, rs->port,
                rs->guid, rc->sha1, rs->stamp, need_push, NULL);
            downloaded = TRUE;
        }
    
        /*
         * We start with FILTER_PROP_DISPLAY:
         */
		gui_prop_get_guint32_val(PROP_SEARCH_MAX_RESULTS, &max_results);
        if (!((flt_result->props[FILTER_PROP_DISPLAY].state == 
                FILTER_PROP_STATE_DONT) &&
            (flt_result->props[FILTER_PROP_DISPLAY].user_data == 0)) &&
            (sch->items < max_results))
        {
            GdkColor *fg_color = NULL;
			const gchar *status = NULL;
            gboolean mark;
            sch->items++;
            g_hash_table_insert(sch->dups, rc, GINT_TO_POINTER(1));
            rc->refcount++;

            mark = 
                (flt_result->props[FILTER_PROP_DISPLAY].state == 
                    FILTER_PROP_STATE_DONT) &&
                (flt_result->props[FILTER_PROP_DISPLAY].user_data == 
                    GINT_TO_POINTER(1));

            if (rc->flags & SR_IGNORED) {
                /*
                 * Check whether this record will be ignored by the backend.
                 */
                fg_color = ignore_color;
				status = "Ignored";
            } else if (downloaded) {
                fg_color = download_color;
				status = "Downloaded";
            } else {
                fg_color = NULL;
            }
			
			if (NULL == status && mark)
				status = "Filtered";

            search_gui_add_record(sch, rc, vinfo, fg_color,
                mark ? mark_color : NULL, status);
        }

        filter_free_result(flt_result);
    }

    /*
     * A result set may not be added more then once to a search!
     */
    /* FIXME: expensive assert */
    g_assert(g_slist_find(sch->r_sets, rs) == NULL);

	/* Adds the set to the list */
	sch->r_sets = g_slist_prepend(sch->r_sets, (gpointer) rs);
	rs->refcount++;

	if (old_items == 0 && sch == current_search && sch->items > 0) {
		GtkWidget *button_search_clear =
		 	lookup_widget(main_window, "button_search_clear");
		gtk_widget_set_sensitive(button_search_clear, TRUE);
	}

	/*
	 *	FIXME:	unseen_items is not for current_search increased even if
	 *			we're not at the search pane.
	 */
		
	if (sch == current_search)
		gui_search_update_items(sch);
	else
		sch->unseen_items += sch->items - old_items;


	if (time(NULL) - sch->last_update_time < tab_update_time)
		gui_search_update_tab_label(sch);

  	g_string_free(vinfo, TRUE);
}

#ifndef USE_SEARCH_XML
/*
 * search_store_old
 *
 * Store pending non-passive searches.
 */
static void search_store_old(void)
{
	GList *l;
	FILE *out;
	time_t now = time((time_t *) NULL);
    char *path;

    path = g_strdup_printf("%s/%s", settings_gui_config_dir(), search_file);
    g_return_if_fail(NULL != path);

	out = fopen(path, "w");
	if (!out) {
		g_warning("Unable to create \"%s\" to persist search: %s",
			path, g_strerror(errno));
        G_FREE_NULL(path);
		return;
	}

	fputs("# THIS FILE IS AUTOMATICALLY GENERATED -- DO NOT EDIT\n", out);
	fprintf(out, "#\n# Searches saved on %s#\n\n", ctime(&now));

	for (l = searches; l; l = g_list_next(l)) {
		const struct search *sch = (const struct search *) l->data;
		if (!sch->passive)
			fprintf(out, "%s\n", sch->query);
	}

	if (0 != fclose(out))
		g_warning("Could not flush \"%s\": %s", path, g_strerror(errno));
    G_FREE_NULL(path);
}
#endif /* USE_SEARCH_XML */

/*
 * search_store
 *
 * Persist searches to disk.
 */
void search_gui_store_searches(void)
{
#ifdef USE_SEARCH_XML
  	char *path;

	search_store_xml();
    path = g_strdup_printf("%s/%s", settings_gui_config_dir(), search_file);
    g_return_if_fail(NULL != path);
    
    if (file_exists(path)) {
        char *path_backup;

        path_backup = g_strdup_printf("%s.old", path);
        if (NULL != path_backup) {
            g_warning(
                "Found old searches file. The search information has been\n"
                "stored in the new XML format and the old file is renamed to\n"
                "%s", path_backup);
            if (-1 == rename(path, path_backup))
          	    g_warning("could not rename %s as %s: %s\n"
                    "The XML file will not be used unless this problem is "
                    "resolved", path, path_backup, g_strerror(errno));
            G_FREE_NULL(path_backup);
        }
    }
    G_FREE_NULL(path);
#else
    search_store_old();
#endif
}

/* ----------------------------------------- */



static void download_selected_file(
	GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
	struct results_set *rs;
	struct record *rc;
	gboolean need_push;

	gtk_tree_model_get(model, iter, c_sr_record, &rc, (-1));

	rs = rc->results_set;
	need_push = (rs->status & ST_FIREWALL) || !host_is_valid(rs->ip, rs->port);

	download_new(rc->name, rc->size, rc->index, rs->ip, rs->port,
		rs->guid, rc->sha1, rs->stamp, need_push, NULL);

	search_gui_check_alt_locs(rc, rs->stamp);

	if (data != NULL) {
		GSList	**iter_list = (GSList **)data;

		*iter_list = g_slist_prepend(*iter_list, w_tree_iter_copy(iter));
	}
}

static void remove_selected_file(
	gpointer data, gpointer user_data)
{
	GtkTreeModel *model = (GtkTreeModel *) user_data;
	GtkTreeIter *iter = data;
	GtkTreeIter child;
	
	current_search->items--;

/* FIXME: Is there a memory leak or not? The records cannot be unref'ed here
 *        but where's the right place? */
/* NOTE: I confirm there is a huge memory leak if you don't unref. --RAM */
#if 0
	/* Remove one reference to this record. */
	search_gui_unref_record(rc);
	g_hash_table_remove(current_search->dups, rc);
#endif
	if (gtk_tree_model_iter_nth_child(model, &child, iter, 0)) {
		gchar *filename;
		gchar *info;
		gpointer fg;
		gpointer bg;
		gpointer rc;
		
    	gtk_tree_model_get(model, &child,
              c_sr_filename, &filename,
              c_sr_info, &info,
              c_sr_fg, &fg,
              c_sr_bg, &bg,
              c_sr_record, &rc,
              (-1));

		gtk_tree_store_set((GtkTreeStore *) model, iter,
              c_sr_filename, filename,
              c_sr_info, info,
              c_sr_fg, fg,
              c_sr_bg, bg,
              c_sr_record, rc,
              (-1));
		gtk_tree_store_remove((GtkTreeStore *) model, &child);
	} else {
		record_t *rc;

    	gtk_tree_model_get(model, iter, c_sr_record, &rc, (-1));
		if (NULL != rc->sha1)
			remove_parent_with_sha1(current_search->parents, rc->sha1);
		gtk_tree_store_remove((GtkTreeStore *) model, iter);
	}
	w_tree_iter_free(iter);
}

static void download_selection_of_tree_view(GtkTreeView * tree_view)
{
    gboolean search_remove_downloaded;
	GtkTreeSelection *selection;
	GtkTreeModel *model;
	GSList	*iter_list = NULL;

	selection = gtk_tree_view_get_selection(tree_view);
	model = gtk_tree_view_get_model(tree_view);

    gnet_prop_get_boolean_val(PROP_SEARCH_REMOVE_DOWNLOADED,
		&search_remove_downloaded);

	gtk_tree_selection_selected_foreach(
		selection, 
		download_selected_file, 
		search_remove_downloaded ? &iter_list : NULL);

	if (search_remove_downloaded) {
		g_slist_foreach(iter_list, remove_selected_file, model);
		g_slist_free(iter_list);
	}

    gui_search_force_update_tab_label(current_search, time(NULL));
    gui_search_update_items(current_search);
}

struct menu_helper {
	gint page;
	GtkTreeIter iter;
};

static gboolean search_gui_menu_select_helper(
	GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
	gint page = -1;
	struct menu_helper *mh = data;

	gtk_tree_model_get(model, iter, 1, &page, (-1));
	if (page == mh->page) {
		mh->iter = *iter;
		return TRUE;
	}
	return FALSE;
}

static void search_gui_menu_select(gint page)
{
	GtkTreeView *treeview;
	GtkTreeModel *model;
	GtkTreeSelection *selection;
	struct menu_helper mh = {.page = page};

	treeview = GTK_TREE_VIEW(
		lookup_widget(main_window, "treeview_menu"));
	model = GTK_TREE_MODEL(gtk_tree_view_get_model(treeview));
	gtk_tree_model_foreach(
		model, search_gui_menu_select_helper, &mh);
	selection = gtk_tree_view_get_selection(treeview);
	gtk_tree_selection_select_iter(selection, &mh.iter);
}

void search_gui_download_files(void)
{
    GtkTreeSelection *selection;

	selection = gtk_tree_view_get_selection(
		GTK_TREE_VIEW(lookup_widget(main_window, "treeview_menu")));

	/* Download the selected files */

	if (jump_to_downloads) {
		gtk_notebook_set_page(
			GTK_NOTEBOOK(lookup_widget(main_window, "notebook_main")),
			nb_main_page_downloads);
		search_gui_menu_select(nb_main_page_downloads);
	}

	if (current_search) {
		download_selection_of_tree_view(
			GTK_TREE_VIEW(current_search->tree_view));
		gtk_tree_selection_unselect_all(
			GTK_TREE_SELECTION(gtk_tree_view_get_selection(
				GTK_TREE_VIEW(current_search->tree_view))));
	} else {
		g_warning("search_download_files(): no possible search!");
	}
}



/***
 *** Callbacks
 ***/

static void search_gui_got_results(
	GSList *schl, const gnet_results_set_t *r_set)
{
    GSList *l;
    results_set_t *rs;

    /*
     * Copy the data we got from the backend.
     */
    rs = search_gui_create_results_set(r_set);

    if (gui_debug >= 12)
        g_warning("got incoming results...\n");

    for (l = schl; l != NULL; l = g_slist_next(l))
        search_matched(search_gui_find((gnet_search_t)l->data), rs);

   	/*
	 * Some of the records might have not been used by searches, and need
	 * to be freed.  If no more records remain, we request that the
	 * result set be removed from all the dispatched searches, the last one
	 * removing it will cause its destruction.
	 */
    if (gui_debug >= 15)
        g_warning("cleaning phase\n");

    if (rs->refcount == 0) {
        search_gui_free_r_set(rs);
        return;
    }

    search_gui_clean_r_set(rs);

    if (gui_debug >= 15)
        g_warning("trash phase\n");
    /*
     * If the record set does not contain any records after the cleansing,
     * we have only an empty shell left which we can safely remove from 
     * all the searches.
     */
	if (rs->num_recs == 0) {
		for (l = schl; l; l = l->next) {
			search_t *sch = search_gui_find((gnet_search_t) l->data);
			search_gui_remove_r_set(sch, rs);
		}
	}

}

/*
 * search_gui_search_results_col_widths_changed:
 *
 * known outside this file.
 */
static void sync_column_widths(GtkTreeView *treeview)
{
    guint32 *width;
	gint i;

	g_assert(NULL != treeview);
    width = gui_prop_get_guint32(PROP_SEARCH_RESULTS_COL_WIDTHS, NULL, 0, 0);

    for (i = 0; i <= c_sr_info; i ++)
		gtk_tree_view_column_set_fixed_width(
			gtk_tree_view_get_column(treeview, i), MAX(1, (gint) width[i]));

    G_FREE_NULL(width);
}

/*
 * search_gui_search_results_col_visible_changed:
 *
 * Callback to update the columns withs in the currently visible search.
 * This is not in settings_gui because the current search should not be
 * known outside this file.
 */
gboolean search_gui_search_results_col_visible_changed(property_t prop)
{
    guint32 *val;
    GtkTreeView *treeview;

    if (current_search == NULL && default_search_tree_view == NULL)
        return FALSE;

    val = gui_prop_get_guint32(PROP_SEARCH_RESULTS_COL_VISIBLE, NULL, 0, 0);

    treeview = GTK_TREE_VIEW((current_search != NULL) ? 
        current_search->tree_view : default_search_tree_view);
    if (NULL != treeview) {
		GtkTreeViewColumn *c;
        gint i = 0;
  
		for (i = 0; NULL != (c = gtk_tree_view_get_column(treeview, i)); i++) 
            gtk_tree_view_column_set_visible(c, val[i]);
    }
    G_FREE_NULL(val);
    return FALSE;
}

/***
 *** Private functions
 ***/

/*
 * search_retrieve_old
 *
 * Retrieve search list and restart searches.
 * The searches are normally retrieved from ~/.gtk-gnutella/searches.
 */
static gboolean search_retrieve_old(void)
{
	FILE *in;
	gint line;				/* File line number */
    const file_path_t fp = { settings_gui_config_dir(), search_file };

    in = file_config_open_read("old searches (pre v0.90)", &fp, 1);
    if (!in)
        return FALSE;

	/*
	 * Retrieval of each searches.
	 */

	line = 0;

	while (fgets(tmpstr, sizeof(tmpstr) - 1, in)) {	/* Room for trailing NUL */
		line++;

		if (tmpstr[0] == '#')
			continue;				/* Skip comments */

		if (tmpstr[0] == '\n')
			continue;				/* Allow arbitrary blank lines */

		(void) str_chomp(tmpstr, 0);	/* The search string */

		search_gui_new_search(tmpstr, 0, NULL);
		tmpstr[0] = '\0';
	}

	fclose(in);

    return TRUE;
}

static void search_gui_column_resized(
    GtkTreeViewColumn *column,
	property_t prop,
	gint id,
	gint min_id,
	gint max_id)
{
    guint32 width;
    static GStaticMutex mutex = G_STATIC_MUTEX_INIT;

    g_assert(id >= min_id && id <= max_id);
    g_static_mutex_lock(&mutex);
    width = gtk_tree_view_column_get_width(column);
	if ((gint) width < 1)
		width = 1;
	if (!search_gui_shutting_down)
		gui_prop_set_guint32(prop, &width, id, 1);
    g_static_mutex_unlock(&mutex);
}

static void on_search_gui_list_column_resized(
    GtkTreeViewColumn *column, GParamSpec *param, gpointer data)
{
    search_gui_column_resized(column, PROP_SEARCH_LIST_COL_WIDTHS,
		GPOINTER_TO_INT(data), 0, 2);
}

static void on_search_gui_results_column_resized(
    GtkTreeViewColumn *column, GParamSpec *param, gpointer data)
{
    search_gui_column_resized(column, PROP_SEARCH_RESULTS_COL_WIDTHS,
		GPOINTER_TO_INT(data), 0, 7);
}

static void add_list_column(
	GtkTreeView *treeview,
	gchar *name,
	gint id,
	gint width,
	gfloat xalign) 
{
    GtkTreeViewColumn *column;

	column = add_column(treeview, name, id, width, xalign, c_sl_fg, c_sl_bg);
	g_signal_connect(G_OBJECT(column), "notify::width",
        G_CALLBACK(on_search_gui_list_column_resized), GINT_TO_POINTER(id));
}

static void add_list_columns(GtkTreeView *treeview)
{
	guint32 *width;

    width = gui_prop_get_guint32(PROP_SEARCH_LIST_COL_WIDTHS, NULL, 0, 0);
	add_list_column(treeview, "Search", c_sl_name, width[c_sl_name],
		(gfloat) 0.0);
	add_list_column(treeview, "Hits", c_sl_hit, width[c_sl_hit], (gfloat) 1.0);
	add_list_column(treeview, "New", c_sl_new, width[c_sl_new], (gfloat) 1.0);
	G_FREE_NULL(width);
}

static void add_results_column(
	GtkTreeView *treeview,
	gchar *name,
	gint id,
	gint width,
	gfloat xalign,
	gint (*sortfunc)(GtkTreeModel *, GtkTreeIter *, GtkTreeIter *, gpointer))
{
    GtkTreeViewColumn *column;
	GtkTreeModel *model;

	model = gtk_tree_view_get_model(treeview);
	column = add_column(treeview, name, id, width, xalign, c_sr_fg, c_sr_bg);
	if (NULL != sortfunc)
		gtk_tree_sortable_set_sort_func(
			GTK_TREE_SORTABLE(model), id, sortfunc, NULL, NULL);
	g_signal_connect(G_OBJECT(column), "notify::width",
        G_CALLBACK(on_search_gui_results_column_resized), GINT_TO_POINTER(id));
	g_signal_connect(G_OBJECT(column), "clicked",
		G_CALLBACK(on_tree_view_search_results_click_column),
		GINT_TO_POINTER(id));
}


/***
 *** Public functions
 ***/

void search_gui_init(void)
{
    GtkNotebook *notebook_search_results = GTK_NOTEBOOK
        (lookup_widget(main_window, "notebook_search_results"));
    GtkCombo *combo_searches = GTK_COMBO
        (lookup_widget(main_window, "combo_searches"));
    GtkTreeView *tree_view_search = GTK_TREE_VIEW
        (lookup_widget(main_window, "tree_view_search"));
    GtkTreeStore *store;

	search_gui_common_init();

	store = gtk_tree_store_new(
		c_sl_num,
		G_TYPE_STRING,
		G_TYPE_INT,
		G_TYPE_INT,
		GDK_TYPE_COLOR,
		GDK_TYPE_COLOR,
		G_TYPE_POINTER);
	gtk_tree_view_set_model(tree_view_search, (GtkTreeModel *) store);
	add_list_columns(tree_view_search);
	g_signal_connect(G_OBJECT(tree_view_search), 
		"cursor-changed",
		G_CALLBACK(on_tree_view_search_select_row),
		NULL);

	gui_search_create_tree_view(&default_scrolled_window, 
		&default_search_tree_view);
    gtk_notebook_remove_page(notebook_search_results, 0);
	gtk_notebook_set_scrollable(notebook_search_results, TRUE);
	gtk_notebook_append_page
        (notebook_search_results, default_scrolled_window, NULL);
  	gtk_notebook_set_tab_label_text
        (notebook_search_results, default_scrolled_window, "(no search)");
    
	g_signal_connect(GTK_OBJECT(combo_searches->popwin), "hide", 
		G_CALLBACK(on_search_popdown_switch), NULL);
	g_signal_connect(GTK_OBJECT(notebook_search_results), "switch_page",
		G_CALLBACK(on_search_notebook_switch), NULL);
	g_signal_connect(GTK_OBJECT(notebook_search_results), "focus_tab",
		G_CALLBACK(on_search_notebook_focus_tab), NULL);

    /*
     * Now we restore the column visibility
     */
    {
        gint i;
        GtkTreeView *treeview;
        GtkTreeViewColumn *c;

        treeview = (current_search != NULL) ? 
                GTK_TREE_VIEW(current_search->tree_view) : 
                GTK_TREE_VIEW(default_search_tree_view);
      
		for (i = 0; NULL != (c = gtk_tree_view_get_column(treeview, i)); i++) 
            gtk_tree_view_column_set_visible(c,
				 (gboolean) search_results_col_visible[i]);
    }

#ifdef USE_SEARCH_XML
    LIBXML_TEST_VERSION
	if (search_retrieve_old()) {
        g_warning(
            "Found old searches file. Loaded it.\n"
            "On exit the searches will be saved in the new XML format\n"
            "and the old file will be renamed.");
    } else {
        search_retrieve_xml();
    }
#else
    search_retrieve_old();
#endif /* USE_SEARCH_XML */

    search_add_got_results_listener(search_gui_got_results);
}

void search_gui_shutdown(void)
{
	search_gui_shutting_down = TRUE;
	search_callbacks_shutdown();
    search_remove_got_results_listener(search_gui_got_results);

	search_gui_store_searches();

    while (searches != NULL)
        search_gui_close_search((search_t *) searches->data);

	search_gui_common_shutdown();
}

static void selection_counter_helper(
	GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
	gint *counter = data;
	*counter += 1;
}

static gint selection_counter(GtkTreeView *tree_view)
{
	GtkTreeSelection *selection = NULL;
	gint rows = 0;

	if (tree_view != NULL)
 		selection = gtk_tree_view_get_selection(tree_view);
	if (selection != NULL)
		gtk_tree_selection_selected_foreach(
				selection, 
				selection_counter_helper,
				&rows);

	return rows;
}

static gboolean tree_view_search_remove(
	GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
	gpointer sch;

    gtk_tree_model_get(model, iter, c_sl_sch, &sch, (-1));
 	if (sch == data) {
    	gtk_tree_store_remove((GtkTreeStore *) model, iter);
		return TRUE;
	}

	return FALSE;
}

/*
 * search_gui_remove_search:
 *
 * Remove the search from the gui and update all widget accordingly.
 */
void search_gui_remove_search(search_t * sch)
{
    GList *glist;
    gboolean sensitive;
	GtkTreeModel *model;
    GtkTreeView *tree_view_search = GTK_TREE_VIEW
        (lookup_widget(main_window, "tree_view_search"));
    GtkNotebook *notebook_search_results = GTK_NOTEBOOK
        (lookup_widget(main_window, "notebook_search_results"));
    GtkCombo *combo_searches = GTK_COMBO
         (lookup_widget(main_window, "combo_searches"));

    g_assert(sch != NULL);

   	glist = g_list_prepend(NULL, (gpointer) sch->list_item);
	gtk_list_remove_items(GTK_LIST(combo_searches->list), glist);

	gtk_tree_store_clear(
		(GtkTreeStore *) gtk_tree_view_get_model(
			(GtkTreeView *) sch->tree_view));

	model = gtk_tree_view_get_model(tree_view_search);
    gtk_tree_model_foreach(model, tree_view_search_remove, sch);

    gtk_timeout_remove(sch->tab_updating);

    if (searches) {				/* Some other searches remain. */
		gtk_notebook_remove_page(notebook_search_results,
			gtk_notebook_page_num(notebook_search_results, 
				sch->scrolled_window));
	} else {
		/*
		 * Keep the GtkTreeView of this search, clear it and make it the
		 * default GtkTreeView 
		 */
        GtkWidget *spinbutton_minimum_speed =
            lookup_widget(main_window, "spinbutton_minimum_speed");

		gtk_tree_store_clear((GtkTreeStore *)
			gtk_tree_view_get_model((GtkTreeView *) sch->tree_view));

		default_search_tree_view = sch->tree_view;
		default_scrolled_window = sch->scrolled_window;

        search_selected = current_search = NULL;

		gui_search_update_items(NULL);

		gtk_entry_set_text
            (GTK_ENTRY(lookup_widget(main_window, "combo_entry_searches")), "");

        gtk_notebook_set_tab_label_text
            (notebook_search_results, default_scrolled_window, "(no search)");

		gtk_widget_set_sensitive
            (lookup_widget(main_window, "button_search_clear"), FALSE);
        gtk_widget_set_sensitive(spinbutton_minimum_speed, FALSE);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(spinbutton_minimum_speed), 0);
	}
    
    sensitive = searches != NULL;
	gtk_widget_set_sensitive(GTK_WIDGET(combo_searches), sensitive);
	gtk_widget_set_sensitive(
        lookup_widget(main_window, "button_search_close"), sensitive);

	if (current_search != NULL)
		sensitive = sensitive && 
			selection_counter(GTK_TREE_VIEW(current_search->tree_view)) > 0;

    gtk_widget_set_sensitive(
		lookup_widget(popup_search, "popup_search_download"), sensitive);
}

void search_gui_set_current_search(search_t *sch) 
{
	search_t *old_sch = current_search;
    GtkWidget *spinbutton_minimum_speed;
    GtkWidget *spinbutton_reissue_timeout;
    GtkTreeView *tree_view_search;
    static gboolean locked = FALSE;
	static GStaticMutex mutex = G_STATIC_MUTEX_INIT;
    gboolean passive;
    gboolean frozen;
    guint32 reissue_timeout;
    guint16 minimum_speed;

	g_assert(sch != NULL);

	g_static_mutex_lock(&mutex);
    if (locked) {
		g_static_mutex_unlock(&mutex);
		return;
	}
    locked = TRUE;
	g_static_mutex_unlock(&mutex);


	if (old_sch)
		gui_search_force_update_tab_label(old_sch, time(NULL));

    passive = search_is_passive(sch->search_handle);
    frozen = search_is_frozen(sch->search_handle);
    reissue_timeout = search_get_reissue_timeout(sch->search_handle);
    minimum_speed = search_get_minimum_speed(sch->search_handle);

    /*
     * We now propagate the column visibility from the current_search
     * to the new current_search.
     */
    if (current_search != NULL) {
        gint i;
        GtkTreeView *treeview = GTK_TREE_VIEW(sch->tree_view);
        GtkTreeView *treeview_old = GTK_TREE_VIEW(current_search->tree_view);
		GtkTreeViewColumn *c;
		GtkTreeViewColumn *old_c;
        
		for (
			i = 0;
			NULL != (c = gtk_tree_view_get_column(treeview, i)) &&
			NULL != (old_c = gtk_tree_view_get_column(treeview_old, i));
			i++
		) {
            gtk_tree_view_column_set_visible(c,
				 gtk_tree_view_column_get_visible(old_c));
        }
    }

	current_search = sch;
	sch->unseen_items = 0;

    spinbutton_minimum_speed = lookup_widget
        (main_window, "spinbutton_minimum_speed");
    spinbutton_reissue_timeout= lookup_widget
        (main_window, "spinbutton_search_reissue_timeout");
    tree_view_search = GTK_TREE_VIEW
            (lookup_widget(main_window, "tree_view_search"));

    if (sch != NULL) {
        gui_search_force_update_tab_label(sch, time(NULL));
        gui_search_update_items(sch);

        gtk_spin_button_set_value
            (GTK_SPIN_BUTTON(spinbutton_minimum_speed), minimum_speed);
        gtk_widget_set_sensitive(spinbutton_minimum_speed, TRUE);
        gtk_spin_button_set_value
            (GTK_SPIN_BUTTON(spinbutton_reissue_timeout), reissue_timeout);
        gtk_widget_set_sensitive(spinbutton_reissue_timeout, !passive);
        gtk_widget_set_sensitive(
            lookup_widget(popup_search, "popup_search_download"), 
			selection_counter(GTK_TREE_VIEW(sch->tree_view)) > 0);
        gtk_widget_set_sensitive(
            lookup_widget(main_window, "button_search_clear"), 
            sch->items != 0);
        gtk_widget_set_sensitive(
            lookup_widget(popup_search, "popup_search_restart"), !passive);
        gtk_widget_set_sensitive(
            lookup_widget(popup_search, "popup_search_duplicate"), !passive);
        gtk_widget_set_sensitive(
            lookup_widget(popup_search, "popup_search_stop"), !frozen);
        gtk_widget_set_sensitive(
            lookup_widget(popup_search, "popup_search_resume"),frozen);

        /*
         * Combo "Active searches"
         */
        gtk_list_item_select(GTK_LIST_ITEM(sch->list_item));
    } else {
        gtk_tree_selection_unselect_all(
			gtk_tree_view_get_selection(tree_view_search));
        gtk_widget_set_sensitive(spinbutton_minimum_speed, FALSE);
        gtk_widget_set_sensitive(spinbutton_reissue_timeout, FALSE);
        gtk_widget_set_sensitive(
            lookup_widget(popup_search, "popup_search_download"), FALSE);
        gtk_widget_set_sensitive(
            lookup_widget(main_window, "button_search_clear"), FALSE);
        gtk_widget_set_sensitive(
            lookup_widget(popup_search, "popup_search_restart"), FALSE);
        gtk_widget_set_sensitive(
            lookup_widget(popup_search, "popup_search_duplicate"), FALSE);
        gtk_widget_set_sensitive(
            lookup_widget(popup_search, "popup_search_stop"), FALSE);
        gtk_widget_set_sensitive(
            lookup_widget(popup_search, "popup_search_resume"), FALSE);
    }

	sync_column_widths(GTK_TREE_VIEW(sch->tree_view));

    /*
     * Search results notebook
     */
    {
        GtkNotebook *notebook_search_results = GTK_NOTEBOOK
            (lookup_widget(main_window, "notebook_search_results"));

        gtk_notebook_set_current_page(notebook_search_results,
  			  gtk_notebook_page_num(notebook_search_results,
                  sch->scrolled_window));
    }

	search_gui_menu_select(nb_main_page_search);
    locked = FALSE;
}

search_t *search_gui_get_current_search(void)
{
    return current_search;
}

static GtkTreeModel *create_model(void)
{
  GtkTreeModel *model;

  /* create tree store */
  model = (GtkTreeModel *) gtk_tree_store_new(c_sr_num,
	G_TYPE_STRING,		/* File */
	G_TYPE_STRING,		/* Size */
	G_TYPE_STRING,		/* Source counter */
	G_TYPE_STRING,		/* Info */
	GDK_TYPE_COLOR,		/* Foreground */
	GDK_TYPE_COLOR,		/* Background */
	G_TYPE_POINTER);	/* (record_t *) */

  return model;
}


static GtkTreeViewColumn *add_column(
	GtkTreeView *treeview,
	gchar *name,
	gint id,
	gint width,
	gfloat xalign,
	gint fg_column,
	gint bg_column)
{
    GtkTreeViewColumn *column;
	GtkCellRenderer *renderer;

	renderer = gtk_cell_renderer_text_new();
	gtk_cell_renderer_text_set_fixed_height_from_font(
		GTK_CELL_RENDERER_TEXT(renderer), 1);
	column = gtk_tree_view_column_new_with_attributes(name, renderer,
		"background-gdk", bg_column,
		"foreground-gdk", fg_column,
		"text", id,
		NULL);
	g_object_set(G_OBJECT(renderer),
		"background-set", TRUE,
		"foreground-set", TRUE,
		"mode", GTK_CELL_RENDERER_MODE_INERT,
		"xalign", xalign,
		"ypad", (guint) GUI_CELL_RENDERER_YPAD,
		NULL);
	g_object_set(G_OBJECT(column),
		"fixed-width", MAX(1, width),
		"min-width", 1,
		"reorderable", TRUE,
		"resizable", TRUE,
		"sizing", GTK_TREE_VIEW_COLUMN_FIXED,
		NULL);
    gtk_tree_view_column_set_sort_column_id(column, id);
    gtk_tree_view_append_column(treeview, column);
	g_object_notify(G_OBJECT(column), "width");

	return column;
}

static void add_results_columns (GtkTreeView *treeview)
{
	guint32 *width;

    width = gui_prop_get_guint32(PROP_SEARCH_RESULTS_COL_WIDTHS, NULL, 0, 0);

	add_results_column(treeview, "File", c_sr_filename, width[c_sr_filename],
		(gfloat) 0.0, NULL);
	add_results_column(treeview, "Size", c_sr_size, width[c_sr_size],
		(gfloat) 1.0, search_gui_compare_size_func);
	add_results_column(treeview, "#", c_sr_count, width[c_sr_count],
		(gfloat) 1.0, search_gui_compare_count_func);
	add_results_column(treeview, "Info", c_sr_info, width[c_sr_info],
		(gfloat) 0.0, NULL);

	G_FREE_NULL(width);
}

/* Create a new GtkTreeView for search results */

void gui_search_create_tree_view(GtkWidget ** sw, GtkWidget ** tv)
{
	GtkTreeModel *model = create_model();
	GtkTreeSelection *selection;
	GtkTreeView	*treeview;

	*sw = gtk_scrolled_window_new(NULL, NULL);

	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(*sw),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);

	treeview = GTK_TREE_VIEW(gtk_tree_view_new_with_model(model));
	*tv = GTK_WIDGET(treeview);
	g_object_unref(model);

	selection = gtk_tree_view_get_selection(treeview);
	gtk_tree_selection_set_mode(selection, GTK_SELECTION_MULTIPLE);
	gtk_tree_view_set_headers_visible(treeview, TRUE);
	gtk_tree_view_set_headers_clickable(treeview, TRUE);
	gtk_tree_view_set_enable_search(treeview, TRUE);
	gtk_tree_view_set_rules_hint(treeview, TRUE);

      /* add columns to the tree view */
	add_results_columns(treeview);

	gtk_container_add(GTK_CONTAINER(*sw), *tv);

	if (!GTK_WIDGET_VISIBLE (*sw))
		gtk_widget_show_all(*sw);

	g_signal_connect(GTK_OBJECT(treeview), "cursor-changed",
		G_CALLBACK(on_tree_view_search_results_select_row), treeview);
	g_signal_connect(GTK_OBJECT(treeview), "button_press_event",
		G_CALLBACK(on_tree_view_search_results_button_press_event), NULL);
    g_signal_connect(GTK_OBJECT(treeview), "key_press_event",
		G_CALLBACK(on_tree_view_search_results_key_press_event), NULL);
}

void gui_search_update_items(struct search *sch)
{
    if (sch) {
        gchar *str = sch->passive ? "(passive search) " : "";
    
        if (sch->items)
            gm_snprintf(tmpstr, sizeof(tmpstr), "%s%u item%s found", 
                str, sch->items, (sch->items > 1) ? "s" : "");
        else
            gm_snprintf(tmpstr, sizeof(tmpstr), "%sNo items found", str);
    } else
        g_strlcpy(tmpstr, "No search", sizeof(tmpstr));

	gtk_label_set(GTK_LABEL(lookup_widget(main_window, "label_items_found")), 
        tmpstr);
}


static gboolean tree_view_search_update(
	GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
	search_t *sch;

    gtk_tree_model_get(model, iter, c_sl_sch, &sch, -1);
 	if ((gpointer)sch == data) {
		GdkColor *fg;
		GdkColor *bg;

		if (sch->unseen_items > 0) {
    		GtkWidget *widget;

			widget = lookup_widget(main_window, "tree_view_search");
    		fg = &(gtk_widget_get_style(widget)->fg[GTK_STATE_PRELIGHT]);
    		bg = &(gtk_widget_get_style(widget)->bg[GTK_STATE_PRELIGHT]);
		} else {
			fg = NULL;
			bg = NULL;
		}
	
		gtk_tree_store_set((GtkTreeStore *) model, iter, 
			c_sl_hit, sch->items, 
			c_sl_new, sch->unseen_items,
			c_sl_fg, fg,
			c_sl_bg, bg,
			(-1));
		return TRUE;
	}

	return FALSE;
}

/* Like search_update_tab_label but always update the label */
void gui_search_force_update_tab_label(struct search *sch, time_t now)
{
    GtkNotebook *notebook_search_results = GTK_NOTEBOOK
        (lookup_widget(main_window, "notebook_search_results"));
    GtkTreeView *tree_view_search = GTK_TREE_VIEW
        (lookup_widget(main_window, "tree_view_search"));
    search_t *search;
	GtkTreeModel *model;

    search = search_gui_get_current_search();

	if (sch == search || sch->unseen_items == 0)
		gm_snprintf(tmpstr, sizeof(tmpstr), "%s\n(%d)", sch->query,
				   sch->items);
	else
		gm_snprintf(tmpstr, sizeof(tmpstr), "%s\n(%d, %d)", sch->query,
				   sch->items, sch->unseen_items);
	sch->last_update_items = sch->items;
	gtk_notebook_set_tab_label_text
        (notebook_search_results, sch->scrolled_window, tmpstr);
	model = gtk_tree_view_get_model(tree_view_search);
	gtk_tree_model_foreach(model, tree_view_search_update, sch);
	sch->last_update_time = now;
}

/* Doesn't update the label if nothing's changed or if the last update was
   recent. */
gboolean gui_search_update_tab_label(struct search *sch)
{
	static time_t now = 0;
	if (sch->items != sch->last_update_items &&
		((now = time(NULL)) - sch->last_update_time >= tab_update_time))
			gui_search_force_update_tab_label(sch, now);

	return TRUE;
}

void gui_search_clear_results(void)
{
	search_t *search;

	search = search_gui_get_current_search();
	gtk_tree_store_clear((GtkTreeStore *) gtk_tree_view_get_model(
			(GtkTreeView *) search->tree_view));
	search_gui_clear_search(search);
	gui_search_force_update_tab_label(search, time(NULL));
	gui_search_update_items(search);
}

/*
 * gui_search_history_add:
 *
 * Adds a search string to the search history combo. Makes
 * sure we do not get more than 10 entries in the history.
 * Also makes sure we don't get duplicate history entries.
 * If a string is already in history and it's added again,
 * it's moved to the beginning of the history list.
 */
void gui_search_history_add(gchar * s)
{
    GList *new_hist = NULL;
    GList *cur_hist = sl_search_history;
    guint n = 0;

    g_return_if_fail(s);

    while (cur_hist != NULL) {
        if ((n < 9) && (g_ascii_strcasecmp(s, cur_hist->data) != 0)) {
            /* copy up to the first 9 items */
            new_hist = g_list_append(new_hist, cur_hist->data);
            n ++;
        } else {
            /* and free the rest */
            G_FREE_NULL(cur_hist->data);
        }
        cur_hist = cur_hist->next;
    }
    /* put the new item on top */
    new_hist = g_list_prepend(new_hist, g_strdup(s));

    /* set new history */
    gtk_combo_set_popdown_strings(
        GTK_COMBO(lookup_widget(main_window, "combo_search")),
        new_hist);

    /* free old list structure */
    g_list_free(sl_search_history);
    
    sl_search_history = new_hist;
}


