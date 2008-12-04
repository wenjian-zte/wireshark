/* follow_ssl.c
 * SSL specific routines for following traffic streams
 *
 * $Id$
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */

#include "config.h"

#include <gtk/gtk.h>

#include <stdio.h>
#include <string.h>


#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <ctype.h>

#include <color.h>
#include <gtk/colors.h>
#include <gtk/main.h>
#include <epan/follow.h>
#include <gtk/dlg_utils.h>
#include <gtk/file_dlg.h>
#include <gtk/keys.h>
#include <globals.h>
#include <alert_box.h>
#include <simple_dialog.h>
#include <epan/dissectors/packet-ipv6.h>
#include <epan/prefs.h>
#include <epan/addr_resolv.h>
#include <util.h>
#include <gtk/gui_utils.h>
#include <epan/epan_dissect.h>
#include <epan/filesystem.h>
#include <gtk/compat_macros.h>
#include <epan/ipproto.h>
#include <gtk/font_utils.h>
#include <wiretap/file_util.h>
#include <epan/tap.h>

#ifdef SSL_PLUGIN
#include "packet-ssl-utils.h"
#else
#include <epan/dissectors/packet-ssl-utils.h>
#endif

#include "follow_ssl.h"

#include "follow_stream.h"

typedef struct {
    gboolean is_server;
    StringInfo data;
} SslDecryptedRecord;

static int
ssl_queue_packet_data(void *tapdata, packet_info *pinfo, epan_dissect_t *edt _U_, const void *ssl)
{
    follow_info_t* follow_info = tapdata;
    SslDecryptedRecord* rec;
    SslDataInfo* appl_data;
    gint total_len;
    guchar *p;
    int proto_ssl = (long) ssl;
    SslPacketInfo* pi = p_get_proto_data(pinfo->fd, proto_ssl);

    /* skip packet without decrypted data payload*/    
    if (!pi || !pi->appl_data)
        return 0;

    /* compute total length */
    total_len = 0;
    appl_data = pi->appl_data;
    do {
      total_len += appl_data->plain_data.data_len; 
      appl_data = appl_data->next;
    } while (appl_data);
    
    /* compute packet direction */
    rec = g_malloc(sizeof(SslDecryptedRecord) + total_len);

    if (follow_info->client_port == 0) {
        follow_info->client_port = pinfo->srcport;
        COPY_ADDRESS(&follow_info->client_ip, &pinfo->src);
    }
    if (ADDRESSES_EQUAL(&follow_info->client_ip, &pinfo->src) &&
        follow_info->client_port == pinfo->srcport)
        rec->is_server = 0;
    else 
        rec->is_server = 1;

    /* update stream counter */
    follow_info->bytes_written[rec->is_server] += total_len;
    
    /* extract decrypted data and queue it locally */    
    rec->data.data = (guchar*)(rec + 1);
    rec->data.data_len = total_len;
    appl_data = pi->appl_data;
    p = rec->data.data;
    do {
      memcpy(p, appl_data->plain_data.data, appl_data->plain_data.data_len);
      p += appl_data->plain_data.data_len; 
      appl_data = appl_data->next;
    } while (appl_data);
    follow_info->payload = g_list_append(
        follow_info->payload,rec);

    return 0;
}

extern gboolean 
packet_is_ssl(epan_dissect_t* edt);


/* Follow the SSL stream, if any, to which the last packet that we called
   a dissection routine on belongs (this might be the most recently
   selected packet, or it might be the last packet in the file). */
void
follow_ssl_stream_cb(GtkWidget * w, gpointer data _U_)
{
    GtkWidget	*filter_te;
    gchar	*follow_filter;
    const gchar	*previous_filter;
    int		filter_out_filter_len, previous_filter_len;
    const char	*hostname0, *hostname1;
    char	*port0, *port1;
    gchar	*server_to_client_string = NULL;
    gchar       *client_to_server_string = NULL;
    gchar	*both_directions_string = NULL;
    follow_stats_t stats;
    follow_info_t *follow_info;
    GString*    msg;

    /* we got ssl so we can follow */
    if (!packet_is_ssl(cfile.edt)) {
        simple_dialog(ESD_TYPE_ERROR, ESD_BTN_OK,
                      "Error following stream.  Please make\n"
                      "sure you have an SSL packet selected.");
        return;
    }

    follow_info = g_new0(follow_info_t, 1);
    follow_info->follow_type = FOLLOW_SSL;

    /* Create a new filter that matches all packets in the SSL stream,
       and set the display filter entry accordingly */
    reset_tcp_reassembly();
    follow_filter = build_follow_filter(&cfile.edt->pi);
    if (!follow_filter)
    {
        simple_dialog(ESD_TYPE_ERROR, ESD_BTN_OK,
                      "Error creating filter for this stream.\n"
                      "A network layer header is needed");
	g_free(follow_info);
        return;
    }

    /* Set the display filter entry accordingly */
    filter_te = OBJECT_GET_DATA(w, E_DFILTER_TE_KEY);

    /* needed in follow_filter_out_stream(), is there a better way? */
    follow_info->filter_te = filter_te;

    /* save previous filter, const since we're not supposed to alter */
    previous_filter =
        (const gchar *)gtk_entry_get_text(GTK_ENTRY(filter_te));

    /* allocate our new filter. API claims g_malloc terminates program on failure */
    /* my calc for max alloc needed is really +10 but when did a few extra bytes hurt ? */
    previous_filter_len = previous_filter?strlen(previous_filter):0;
    filter_out_filter_len = strlen(follow_filter) + previous_filter_len + 16;
    follow_info->filter_out_filter = (gchar *)g_malloc(filter_out_filter_len);

    /* append the negation */
    if(previous_filter_len) {
        g_snprintf(follow_info->filter_out_filter, filter_out_filter_len,
        "%s and !(%s)", previous_filter, follow_filter);
    } else {
        g_snprintf(follow_info->filter_out_filter, filter_out_filter_len,
        "!(%s)", follow_filter);
    }

    /* data will be passed via tap callback*/
    msg = register_tap_listener("ssl", follow_info, follow_filter,
	NULL, ssl_queue_packet_data, NULL);
    if (msg)
    {
        simple_dialog(ESD_TYPE_ERROR, ESD_BTN_OK,
            "Can't register ssl tap: %s\n",msg->str);
	g_free(follow_info);
        return;
    }
    gtk_entry_set_text(GTK_ENTRY(filter_te), follow_filter);

    /* Run the display filter so it goes in effect - even if it's the
       same as the previous display filter. */
    main_filter_packets(&cfile, follow_filter, TRUE);

    /* Free the filter string, as we're done with it. */
    g_free(follow_filter);

    remove_tap_listener(follow_info);

    /* Stream to show */
    follow_stats(&stats);

    if (stats.is_ipv6) {
	    struct e_in6_addr ipaddr;
	    memcpy(&ipaddr, stats.ip_address[0], 16);
	    hostname0 = get_hostname6(&ipaddr);
	    memcpy(&ipaddr, stats.ip_address[0], 16);
	    hostname1 = get_hostname6(&ipaddr);
    } else {
	    guint32 ipaddr;
	    memcpy(&ipaddr, stats.ip_address[0], 4);
	    hostname0 = get_hostname(ipaddr);
	    memcpy(&ipaddr, stats.ip_address[1], 4);
	    hostname1 = get_hostname(ipaddr);
    }
    
    port0 = get_tcp_port(stats.port[0]);
    port1 = get_tcp_port(stats.port[1]);
    
    follow_info->is_ipv6 = stats.is_ipv6;

   /* Both Stream Directions */
    both_directions_string = g_strdup_printf("Entire conversation (%u bytes)", follow_info->bytes_written[0] + follow_info->bytes_written[1]);
    
    if(follow_info->client_port == stats.port[0]) {
	    server_to_client_string =
		    g_strdup_printf("%s:%s --> %s:%s (%u bytes)",
				    hostname0, port0,
				    hostname1, port1,
				    follow_info->bytes_written[0]);
	    
	    client_to_server_string =
		    g_strdup_printf("%s:%s --> %s:%s (%u bytes)",
				    hostname1, port1,
				    hostname0, port0,
				    follow_info->bytes_written[1]);
    } else {
	    server_to_client_string =
		    g_strdup_printf("%s:%s --> %s:%s (%u bytes)",
				    hostname1, port1,
				    hostname0, port0,
				    follow_info->bytes_written[0]);
	    
	    client_to_server_string =
		    g_strdup_printf("%s:%s --> %s:%s (%u bytes)",
				    hostname0, port0,
				    hostname1, port1,
				    follow_info->bytes_written[1]);
    }
    
    follow_stream("Follow SSL Stream", follow_info, both_directions_string,
		  server_to_client_string, client_to_server_string);

    g_free(both_directions_string);
    g_free(server_to_client_string);
    g_free(client_to_server_string);
}

#define FLT_BUF_SIZE 1024

/*
 * XXX - the routine pointed to by "print_line" doesn't get handed lines,
 * it gets handed bufferfuls.  That's fine for "follow_write_raw()"
 * and "follow_add_to_gtk_text()", but, as "follow_print_text()" calls
 * the "print_line()" routine from "print.c", and as that routine might
 * genuinely expect to be handed a line (if, for example, it's using
 * some OS or desktop environment's printing API, and that API expects
 * to be handed lines), "follow_print_text()" should probably accumulate
 * lines in a buffer and hand them "print_line()".  (If there's a
 * complete line in a buffer - i.e., there's nothing of the line in
 * the previous buffer or the next buffer - it can just hand that to
 * "print_line()" after filtering out non-printables, as an
 * optimization.)
 *
 * This might or might not be the reason why C arrays display
 * correctly but get extra blank lines very other line when printed.
 */
frs_return_t
follow_read_ssl_stream(follow_info_t *follow_info,
		       gboolean (*print_line)(char *, size_t, gboolean, void *),
		       void *arg)
{
    int			iplen;
    guint32		global_client_pos = 0, global_server_pos = 0;
    guint32		server_packet_count = 0;
    guint32		client_packet_count = 0;
    guint32		*global_pos;
    gboolean		skip;
    GList* cur;
    frs_return_t        frs_return;

    iplen = (follow_info->is_ipv6) ? 16 : 4;
    
    for (cur = follow_info->payload; cur; cur = g_list_next(cur)) {
        SslDecryptedRecord* rec = cur->data;
	skip = FALSE;
	if (!rec->is_server) {
	    global_pos = &global_client_pos;
	    if (follow_info->show_stream == FROM_SERVER) {
		skip = TRUE;
	    }
	} else {
	    global_pos = &global_server_pos;
	    if (follow_info->show_stream == FROM_CLIENT) {
		skip = TRUE;
	    }
	}

        if (!skip) {
            size_t nchars = rec->data.data_len;
            gchar *buffer = g_memdup(rec->data.data, nchars);
            
	    frs_return = follow_show(follow_info, print_line, buffer, nchars,
				     rec->is_server, arg, global_pos,
				     &server_packet_count, &client_packet_count);
	    g_free(buffer);
	    if(frs_return == FRS_PRINT_ERROR)
		    return frs_return;
	}
    }

    return FRS_OK;
}
