/*
 * $Id$
 *
 * Copyright (c) 2001-2003, Raphael Manfredi, Richard Eckart
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

#ifndef _gtk_gnet_stats_common_h_
#define _gtk_gnet_stats_common_h_

#include "gui.h"
#include "columns.h"
#include "if/core/hsep.h"
#include "if/ui/gtk/gnet_stats.h"

const gchar *msg_type_str(gint MSG_TYPE_COUNT);
const gchar *msg_drop_str(gint MSG_DROP_REASON_COUNT);
const gchar *general_type_str(gint GNR_TYPE_COUNT);
const gchar *horizon_stat_str(gint row,	c_horizon_t column);

gint msg_type_str_size(void);
void gnet_stats_gui_horizon_update(hsep_triple *table, guint32 triples);

#endif /* _gtk_gnet_stats_common_h_ */
