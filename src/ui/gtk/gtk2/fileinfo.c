/*
 * $Id$
 *
 * Copyright (c) 2003, Richard Eckart
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

/**
 * @ingroup gtk
 * @file
 *
 * Displaying of file information in the GUI.
 *
 * @author Richard Eckart
 * @date 2003
 */

#include "gtk/gui.h"

RCSID("$Id$")

#include "downloads_cb.h"

#include "gtk/columns.h"
#include "gtk/downloads_common.h"
#include "gtk/drag.h"
#include "gtk/filter.h"
#include "gtk/gtk-missing.h"
#include "gtk/gtkcolumnchooser.h"
#include "gtk/misc.h"
#include "gtk/settings.h"
#include "gtk/statusbar.h"
#include "gtk/visual_progress.h"

#include "if/gui_property.h"
#include "if/gui_property_priv.h"
#include "if/bridge/ui2c.h"

#include "lib/atoms.h"
#include "lib/utf8.h"
#include "lib/url.h"
#include "lib/walloc.h"
#include "lib/hashlist.h"
#include "lib/glib-missing.h"

#include "lib/override.h"		/* Must be the last header included */

static GHashTable *fi_sources;

static GtkTreeView *treeview_download_aliases;
static GtkTreeView *treeview_download_details;
static GtkTreeView *treeview_download_files;
static GtkTreeView *treeview_download_sources;

static GtkListStore *store_aliases;
static GtkListStore *store_files;
static GtkListStore *store_sources;

static inline void
fileinfo_data_set_iter(struct fileinfo_data *file, GtkTreeIter *iter)
{
	fi_gui_file_set_user_data(file, iter);
}

static inline GtkTreeIter * 
fileinfo_data_get_iter(const struct fileinfo_data *file)
{
	return fi_gui_file_get_user_data(file);
}

void
fi_gui_file_invalidate(struct fileinfo_data *file)
{
	GtkTreeIter *iter = fileinfo_data_get_iter(file);
	if (iter) {
		fileinfo_data_set_iter(file, NULL);
		WFREE_NULL(iter, sizeof *iter);
	}
}

void
fi_gui_file_show(struct fileinfo_data *file)
{
	static const GValue zero_value;
	GValue value = zero_value;
	GtkTreeIter *iter;

	g_return_if_fail(store_files);
	g_assert(file);

	iter = fileinfo_data_get_iter(file);
	if (!iter) {
		iter = walloc(sizeof *iter);
		fileinfo_data_set_iter(file, iter);
		gtk_list_store_append(store_files, iter);
	}
	g_value_init(&value, G_TYPE_POINTER);
	g_value_set_pointer(&value, file);
	gtk_list_store_set_value(store_files, iter, 0, &value);
}

void
fi_gui_file_hide(struct fileinfo_data *file)
{
	GtkTreeIter *iter;

	iter = fileinfo_data_get_iter(file);
	if (iter) {
		if (store_files) {
			gtk_list_store_remove(store_files, iter);
		}
		fi_gui_file_invalidate(file);
	}
}

static inline void *
get_row_data(GtkTreeModel *model, GtkTreeIter *iter)
{
	static const GValue zero_value;
	GValue value = zero_value;

	gtk_tree_model_get_value(model, iter, 0, &value);
	return g_value_get_pointer(&value);
}

static inline struct fileinfo_data *
get_fileinfo_data(GtkTreeIter *iter)
{
	return get_row_data(GTK_TREE_MODEL(store_files), iter);
}

static inline struct download *
get_source(GtkTreeIter *iter)
{
	return get_row_data(GTK_TREE_MODEL(store_sources), iter);
}

static void
render_files(GtkTreeViewColumn *column, GtkCellRenderer *cell, 
	GtkTreeModel *unused_model, GtkTreeIter *iter, void *udata)
{
	const struct fileinfo_data *file;
	enum c_fi idx;

	(void) unused_model;

	if (!gtk_tree_view_column_get_visible(column))
		return;

	file = get_fileinfo_data(iter);
	g_return_if_fail(file);

	idx = GPOINTER_TO_UINT(udata);
	if (c_fi_progress == idx) {
		unsigned value = fi_gui_file_get_progress(file);
		g_object_set(cell, "value", value, (void *) 0);
	} else {
		const char *text = fi_gui_file_column_text(file, idx);
		g_object_set(cell, "text", text, (void *) 0);
	}
}

static void
render_sources(GtkTreeViewColumn *column, GtkCellRenderer *cell, 
	GtkTreeModel *unused_model, GtkTreeIter *iter, void *udata)
{
	struct download *d;
	enum c_src idx;

	(void) unused_model;

	if (!gtk_tree_view_column_get_visible(column))
		return;

	d = get_source(iter);
	g_return_if_fail(d);

	idx = GPOINTER_TO_UINT(udata);
	if (c_src_progress == idx) {
		int value;

		value = 100.0 * guc_download_source_progress(d);
		value = CLAMP(value, 0, 100);
		g_object_set(cell, "value", value, (void *) 0);
	} else {
		const char *text = fi_gui_source_column_text(d, idx);
		g_object_set(cell, "text", text, (void *) 0);
	}
}

static GtkCellRenderer *
create_text_cell_renderer(gfloat xalign)
{
	GtkCellRenderer *renderer;
	
	renderer = gtk_cell_renderer_text_new();
	gtk_cell_renderer_text_set_fixed_height_from_font(
		GTK_CELL_RENDERER_TEXT(renderer), 1);
	g_object_set(G_OBJECT(renderer),
		"mode",		GTK_CELL_RENDERER_MODE_INERT,
		"xalign",	xalign,
		"ypad",		(unsigned) GUI_CELL_RENDERER_YPAD,
		(void *) 0);

	return renderer;
}

static gboolean
fi_sources_remove(void *unused_key, void *value, void *unused_udata)
{
	GtkTreeIter *iter;

	g_assert(value);
	(void) unused_key;
	(void) unused_udata;

	iter = value;
	wfree(iter, sizeof *iter);
	return TRUE; /* Remove the handle from the hashtable */
}

void
fi_gui_clear_aliases(void)
{
    gtk_list_store_clear(store_aliases);
}

void
fi_gui_clear_sources(void)
{
    gtk_list_store_clear(store_sources);
	g_hash_table_foreach_remove(fi_sources, fi_sources_remove, NULL);
}

void
fi_gui_show_aliases(const char * const *aliases)
{
	size_t i;

	g_return_if_fail(store_aliases);
    gtk_list_store_clear(store_aliases);

	for (i = 0; NULL != aliases[i]; i++) {
		GtkTreeIter iter;
		const char *filename;

		filename = lazy_filename_to_ui_string(aliases[i]);
		gtk_list_store_append(store_aliases, &iter);
		gtk_list_store_set(store_aliases, &iter, 0, filename, (-1));
	}
}

void
fi_gui_source_add(struct download *d)
{
	GtkTreeIter *iter;

	g_return_if_fail(store_sources);
	g_return_if_fail(NULL == g_hash_table_lookup(fi_sources, d));

	iter = walloc(sizeof *iter);
	g_hash_table_insert(fi_sources, d, iter);
	gtk_list_store_append(store_sources, iter);
	gtk_list_store_set(store_sources, iter, 0, d, (-1));
}

static GSList *
fi_gui_collect_selected(GtkTreeView *tv,
	GtkTreeSelectionForeachFunc func, gboolean unselect)
{
	GtkTreeSelection *selection;
	GSList *list;

	g_return_val_if_fail(tv, NULL);
	g_return_val_if_fail(func, NULL);

	list = NULL;
	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tv));
	gtk_tree_selection_selected_foreach(selection, func, &list);
	if (unselect) {
		gtk_tree_selection_unselect_all(selection);
	}
	return list;
}


static void
fi_gui_sources_select_helper(GtkTreeModel *unused_model,
	GtkTreePath *unused_path, GtkTreeIter *iter, void *user_data)
{
	GSList **sources_ptr = user_data;

	(void) unused_model;
	(void) unused_path;
	*sources_ptr = g_slist_prepend(*sources_ptr, get_source(iter));
}

static void
fi_gui_files_select_helper(GtkTreeModel *unused_model,
	GtkTreePath *unused_path, GtkTreeIter *iter, void *user_data)
{
	GSList **files_ptr = user_data;
	struct fileinfo_data *file;

	(void) unused_model;
	(void) unused_path;
	file = get_fileinfo_data(iter);
	*files_ptr = g_slist_prepend(*files_ptr, GUINT_TO_POINTER(file));
}

static void
fi_gui_sources_of_selected_files_helper(GtkTreeModel *unused_model,
	GtkTreePath *unused_path, GtkTreeIter *iter, void *user_data)
{
	GSList **files_ptr = user_data;
	struct fileinfo_data *file;

	(void) unused_model;
	(void) unused_path;
	file = get_fileinfo_data(iter);
	*files_ptr = g_slist_concat(fi_gui_file_get_sources(file), *files_ptr);
}

GSList *
fi_gui_sources_select(gboolean unselect)
{
	return fi_gui_collect_selected(treeview_download_sources,
			fi_gui_sources_select_helper,
			unselect);
}

GSList *
fi_gui_files_select(gboolean unselect)
{
	return fi_gui_collect_selected(treeview_download_files,
			fi_gui_files_select_helper,
			unselect);
}

GSList *
fi_gui_sources_of_selected_files(gboolean unselect)
{
	return fi_gui_collect_selected(treeview_download_files,
			fi_gui_sources_of_selected_files_helper,
			unselect);
}

static char * 
download_details_get_text(GtkWidget *widget)
{
	GtkTreeModel *model;
	GtkTreeIter iter;

	g_return_val_if_fail(widget, NULL);

	if (drag_get_iter(GTK_TREE_VIEW(widget), &model, &iter)) {
		static const GValue zero_value;
		GValue value;

		value = zero_value;
		gtk_tree_model_get_value(model, &iter, 1, &value);
		return g_strdup(g_value_get_string(&value));
	} else {
		return NULL;
	}
}


static void *
get_row_data_at_cursor(GtkTreeView *tv)
{
	GtkTreePath *path;
	void *data = NULL;

	g_return_val_if_fail(tv, NULL);

	gtk_tree_view_get_cursor(tv, &path, NULL);
	if (path) {
		GtkTreeModel *model;
		GtkTreeIter iter;

		model = gtk_tree_view_get_model(tv);
		if (gtk_tree_model_get_iter(model, &iter, path)) {
			data = get_row_data(model, &iter);
		}
		gtk_tree_path_free(path);
	}
	return data;
}

char *
fi_gui_get_detail_at_cursor(void)
{
	return download_details_get_text(GTK_WIDGET(treeview_download_details));
}

struct fileinfo_data *
fi_gui_get_file_at_cursor(void)
{
	return get_row_data_at_cursor(treeview_download_files);
}

struct download *
fi_gui_get_source_at_cursor(void)
{
	return get_row_data_at_cursor(treeview_download_sources);
}

static void
on_treeview_download_files_cursor_changed(GtkTreeView *unused_tv,
	void *unused_udata)
{
	struct fileinfo_data *file;

	(void) unused_tv;
	(void) unused_udata;

	fi_gui_clear_details();
	file = fi_gui_get_file_at_cursor();
	if (file) {
		fi_gui_set_details(file);
	}
}

void
fi_gui_purge_selected_files(void)
{
	GtkTreeView *tv = treeview_download_files;
	
	g_return_if_fail(tv);

	g_object_freeze_notify(G_OBJECT(tv));
	fi_gui_purge_selected_fileinfo();
	g_object_thaw_notify(G_OBJECT(tv));
}

void
fi_gui_source_update(struct download *d)
{
	GtkTreeIter *iter;

	download_check(d);

	iter = g_hash_table_lookup(fi_sources, d);
	if (iter) {
		tree_model_iter_changed(GTK_TREE_MODEL(store_sources), iter);
	}
}

static int
fileinfo_data_cmp_func(GtkTreeModel *unused_model,
	GtkTreeIter *a, GtkTreeIter *b, void *user_data)
{
	(void) unused_model;
	return fileinfo_data_cmp(get_fileinfo_data(a), get_fileinfo_data(b),
				GPOINTER_TO_UINT(user_data));
}

static GtkTreeViewColumn *
create_column(int column_id, const char *title, gfloat xalign,
	GtkCellRenderer *renderer, GtkTreeCellDataFunc cell_data_func)
{
    GtkTreeViewColumn *column;

	if (!renderer) {
		renderer = create_text_cell_renderer(xalign);
	}

	column = gtk_tree_view_column_new_with_attributes(title,
				renderer, (void *) 0);
	gtk_tree_view_column_set_cell_data_func(column, renderer,
		cell_data_func, GUINT_TO_POINTER(column_id), NULL);
	return column;
}

void
configure_column(GtkTreeViewColumn *column)
{
	g_object_set(G_OBJECT(column),
		"fixed-width", 100,
		"min-width", 1,
		"reorderable", FALSE,
		"resizable", TRUE,
		"sizing", GTK_TREE_VIEW_COLUMN_FIXED,
		(void *) 0);
}

static GtkTreeViewColumn *
add_column(GtkTreeView *tv, int column_id, const char *title, gfloat xalign,
	GtkCellRenderer *renderer, GtkTreeCellDataFunc cell_data_func)
{
	GtkTreeViewColumn *column;

	column = create_column(column_id, title, xalign, renderer, cell_data_func);
	configure_column(column);
	gtk_tree_view_column_set_sort_column_id(column, column_id);
    gtk_tree_view_append_column(tv, column);
	return column;
}

static char *
download_files_get_file_url(GtkWidget *widget)
{
	GtkTreeModel *model;
	GtkTreeIter iter;

	g_return_val_if_fail(widget, NULL);

	if (drag_get_iter(GTK_TREE_VIEW(widget), &model, &iter)) {
		return fi_gui_file_get_file_url(get_fileinfo_data(&iter));
	} else {
		return NULL;
	}
}

static char *
fi_gui_get_alias(GtkWidget *widget)
{
	GtkTreeModel *model;
	GtkTreeIter iter;

	g_return_val_if_fail(widget, NULL);

	if (drag_get_iter(GTK_TREE_VIEW(widget), &model, &iter)) {
		static const GValue zero_value;
		GValue value;

		value = zero_value;
		gtk_tree_model_get_value(model, &iter, 0, &value);
		return g_strdup(g_value_get_string(&value));
	} else {
		return NULL;
	}
}

void
fi_gui_update_display(time_t unused_now)
{
	(void) unused_now;

	if (!main_gui_window_visible())
		return;

	g_return_if_fail(treeview_download_files);
	if (!GTK_WIDGET_DRAWABLE(GTK_WIDGET(treeview_download_files)))
		return;

	g_object_freeze_notify(G_OBJECT(treeview_download_files));
	fi_gui_file_process_updates();
	g_object_thaw_notify(G_OBJECT(treeview_download_files));
}


static void
fi_gui_details_treeview_init(void)
{
	static const struct {
		const char *title;
		gfloat xalign;
		gboolean editable;
	} tab[] = {
		{ "Item",	1.0, FALSE },
		{ "Value",	0.0, TRUE },
	};
	GtkTreeView *tv;
	GtkTreeModel *model;
	unsigned i;

	tv = GTK_TREE_VIEW(gui_main_window_lookup("treeview_download_details"));
	g_return_if_fail(tv);
	treeview_download_details = tv;

	model = GTK_TREE_MODEL(
		gtk_list_store_new(G_N_ELEMENTS(tab), G_TYPE_STRING, G_TYPE_STRING));

	gtk_tree_view_set_model(tv, model);
	g_object_unref(model);

	for (i = 0; i < G_N_ELEMENTS(tab); i++) {
    	GtkTreeViewColumn *column;
		GtkCellRenderer *renderer;
		
		renderer = create_text_cell_renderer(tab[i].xalign);
		g_object_set(G_OBJECT(renderer),
			"editable", tab[i].editable,
			(void *) 0);
		column = gtk_tree_view_column_new_with_attributes(tab[i].title,
					renderer, "text", i, (void *) 0);
		g_object_set(column,
			"min-width", 1,
			"resizable", TRUE,
			"sizing", (0 == i)
						? GTK_TREE_VIEW_COLUMN_AUTOSIZE
						: GTK_TREE_VIEW_COLUMN_FIXED,
			(void *) 0);
    	gtk_tree_view_append_column(tv, column);
	}

	drag_attach(GTK_WIDGET(tv), download_details_get_text);
}

static void
store_files_init(void)
{
	unsigned i;

	if (store_files) {
		g_object_unref(store_files);
	}
	store_files = gtk_list_store_new(1, G_TYPE_POINTER);

	for (i = 0; i < c_fi_num; i++) {
		gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(store_files),
			i, fileinfo_data_cmp_func, GUINT_TO_POINTER(i), NULL);
	}

	g_object_freeze_notify(G_OBJECT(store_files));
	fi_gui_files_visualize();
	g_object_thaw_notify(G_OBJECT(store_files));

}

static void
treeview_download_files_init(void)
{
	static const struct {
		const int id;
		const char * const title;
		gboolean justify_right;
	} columns[] = {
		{ c_fi_filename, N_("Filename"), 	FALSE },
    	{ c_fi_size,	 N_("Size"),	 	TRUE },
    	{ c_fi_progress, N_("Progress"), 	FALSE },
    	{ c_fi_rx, 		 N_("RX"), 			TRUE },
    	{ c_fi_done,	 N_("Downloaded"), 	TRUE },
    	{ c_fi_uploaded, N_("Uploaded"), 	TRUE },
    	{ c_fi_sources,  N_("Sources"),  	FALSE },
    	{ c_fi_status,   N_("Status"),	 	FALSE }
	};
	GtkTreeView *tv;
	unsigned i;

	STATIC_ASSERT(FILEINFO_VISIBLE_COLUMNS == G_N_ELEMENTS(columns));

	tv = GTK_TREE_VIEW(gtk_tree_view_new());
	treeview_download_files = tv;

	for (i = 0; i < G_N_ELEMENTS(columns); i++) {
		GtkCellRenderer *renderer;

		renderer = columns[i].id == c_fi_progress
					? gtk_cell_renderer_progress_new()
					: NULL;
		add_column(tv, columns[i].id, _(columns[i].title),
			columns[i].justify_right ? 1.0 : 0.0,
			renderer, render_files);
	}
	gtk_tree_selection_set_mode(gtk_tree_view_get_selection(tv),
		GTK_SELECTION_MULTIPLE);
	gtk_tree_view_set_headers_visible(tv, TRUE);
	gtk_tree_view_set_headers_clickable(tv, TRUE);
	gtk_tree_view_set_enable_search(tv, FALSE);
	gtk_tree_view_set_rules_hint(tv, TRUE);
	tree_view_set_fixed_height_mode(tv, TRUE);

	gtk_tree_view_set_model(tv, GTK_TREE_MODEL(store_files));
	tree_view_restore_visibility(tv, PROP_FILE_INFO_COL_VISIBLE);
	tree_view_restore_widths(tv, PROP_FILE_INFO_COL_WIDTHS);

	gui_signal_connect(tv, "cursor-changed",
		on_treeview_download_files_cursor_changed, NULL);

	gui_signal_connect(tv, "key-press-event",
		on_files_key_press_event, NULL);
	gui_signal_connect(tv, "button-press-event",
		on_files_button_press_event, NULL);

	drag_attach(GTK_WIDGET(tv), download_files_get_file_url);
}

GtkWidget *
fi_gui_files_widget_new(void)
{
	store_files_init();
	treeview_download_files_init();
	return GTK_WIDGET(treeview_download_files);
}

void
fi_gui_files_widget_destroy(void)
{
	if (treeview_download_files) {
		tree_view_save_visibility(treeview_download_files,
			PROP_FILE_INFO_COL_VISIBLE);
		tree_view_save_widths(treeview_download_files,
			PROP_FILE_INFO_COL_WIDTHS);
		gtk_widget_destroy(GTK_WIDGET(treeview_download_files));
		treeview_download_files = NULL;
	}
}

void
fi_gui_init(void)
{
	fi_sources = g_hash_table_new(NULL, NULL);
	
	fi_gui_common_init();

	{
		GtkTreeViewColumn *column;
		GtkTreeView *tv;

		tv = GTK_TREE_VIEW(gui_main_window_lookup("treeview_download_aliases"));
		treeview_download_aliases = tv;

		store_aliases = gtk_list_store_new(1, G_TYPE_STRING);
		gtk_tree_view_set_model(tv, GTK_TREE_MODEL(store_aliases));

		column = gtk_tree_view_column_new_with_attributes(_("Aliases"),
					create_text_cell_renderer(0.0),
					"text", 0,
					(void *) 0);
		configure_column(column);
		gtk_tree_view_column_set_sort_column_id(column, 0);
    	gtk_tree_view_append_column(tv, column);

		tree_view_set_fixed_height_mode(tv, TRUE);
		drag_attach(GTK_WIDGET(tv), fi_gui_get_alias);
	}

	{
		static const struct {
			enum c_src id;
			const char *title;
		} tab[] = {
   			{ c_src_host, 	 	N_("Host"), },
   			{ c_src_country, 	N_("Country"), },
   			{ c_src_server,  	N_("Server"), },
   			{ c_src_range, 	 	N_("Range"), },
   			{ c_src_progress,	N_("Progress"), },
   			{ c_src_status,	 	N_("Status"), },
		};
		GtkTreeView *tv;
		unsigned i;

		STATIC_ASSERT(c_src_num == G_N_ELEMENTS(tab));
		
		tv = GTK_TREE_VIEW(gui_main_window_lookup("treeview_download_sources"));
		treeview_download_sources = tv;

		store_sources = gtk_list_store_new(1, G_TYPE_POINTER);
		gtk_tree_view_set_model(tv, GTK_TREE_MODEL(store_sources));

		for (i = 0; i < G_N_ELEMENTS(tab); i++) {
			GtkCellRenderer *renderer;

			renderer = tab[i].id == c_src_progress
						? gtk_cell_renderer_progress_new()
						: NULL;
    		add_column(tv, tab[i].id, tab[i].title, 0.0,
				renderer, render_sources);
		}

		gtk_tree_view_set_headers_clickable(tv, FALSE);
		gtk_tree_selection_set_mode(gtk_tree_view_get_selection(tv),
			GTK_SELECTION_MULTIPLE);
		tree_view_restore_widths(tv, PROP_SOURCES_COL_WIDTHS);
		tree_view_set_fixed_height_mode(tv, TRUE);

		gui_signal_connect(tv, "button-press-event",
			on_sources_button_press_event, NULL);
	}

	fi_gui_details_treeview_init();
}

void
fi_gui_shutdown(void)
{
	tree_view_save_visibility(treeview_download_files,
		PROP_FILE_INFO_COL_VISIBLE);
	tree_view_save_widths(treeview_download_files,
		PROP_FILE_INFO_COL_WIDTHS);
	tree_view_save_widths(treeview_download_sources,
		PROP_SOURCES_COL_WIDTHS);

	fi_gui_common_shutdown();

	if (treeview_download_files) {
		gtk_widget_destroy(GTK_WIDGET(treeview_download_files));
		treeview_download_files = NULL;
	}
	if (store_files) {
		g_object_unref(store_files);
		store_files = NULL;
	}
	if (treeview_download_aliases) {
		gtk_widget_destroy(GTK_WIDGET(treeview_download_aliases));
		treeview_download_aliases = NULL;
	}
	if (store_aliases) {
		g_object_unref(store_aliases);
		store_aliases = NULL;
	}
	if (treeview_download_sources) {
		gtk_widget_destroy(GTK_WIDGET(treeview_download_sources));
		treeview_download_sources = NULL;
	}
	if (store_sources) {
		g_object_unref(store_sources);
		store_sources = NULL;
	}

	g_hash_table_destroy(fi_sources);
	fi_sources = NULL;
}

void
fi_gui_source_remove(struct download *d)
{
	GtkTreeIter *iter;

	iter = g_hash_table_lookup(fi_sources, d);
	if (iter) {
		if (store_sources) {
			gtk_list_store_remove(store_sources, iter);
		}
		g_hash_table_remove(fi_sources, d);
		wfree(iter, sizeof *iter);
	}
}

void
fi_gui_files_unselect_all(void)
{
	GtkTreeView *tv = treeview_download_files;

	g_return_if_fail(tv);
	gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(tv));
}

void
fi_gui_file_select(struct fileinfo_data *file)
{
	GtkTreeIter *iter;

	g_return_if_fail(file);
	iter = fileinfo_data_get_iter(file);
	if (iter) {
		GtkTreeView *tv = treeview_download_files;

		g_return_if_fail(tv);
		gtk_tree_selection_select_iter(gtk_tree_view_get_selection(tv), iter);
	}
}

struct fi_gui_files_foreach {
	fi_gui_files_foreach_cb func;
	void *user_data;
};

static int
fi_gui_files_foreach_helper(GtkTreeModel *unused_model,
	GtkTreePath *unused_path, GtkTreeIter *iter, void *user_data)
{
	struct fi_gui_files_foreach *ctx;

	(void) unused_model;
	(void) unused_path;

	ctx = user_data;
	return ctx->func(get_fileinfo_data(iter), ctx->user_data);
}

void
fi_gui_files_foreach(fi_gui_files_foreach_cb func, void *user_data)
{
	struct fi_gui_files_foreach ctx;
	GtkTreeView *tv;

	g_return_if_fail(func);

	tv = treeview_download_files;
	g_object_freeze_notify(G_OBJECT(tv));

	ctx.func = func;
	ctx.user_data = user_data;
	gtk_tree_model_foreach(GTK_TREE_MODEL(store_files),
		fi_gui_files_foreach_helper, &ctx);

	g_object_thaw_notify(G_OBJECT(tv));
}

void
fi_gui_files_configure_columns(void)
{
    GtkWidget *cc;

	g_return_if_fail(treeview_download_files);

    cc = gtk_column_chooser_new(GTK_WIDGET(treeview_download_files));
    gtk_menu_popup(GTK_MENU(cc), NULL, NULL, NULL, NULL, 1,
		gtk_get_current_event_time());
}

/* vi: set ts=4 sw=4 cindent: */
