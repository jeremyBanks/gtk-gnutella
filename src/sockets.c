
/* Socket management */

#include "gnutella.h"

#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <string.h>
#include <dlfcn.h>
#include <pwd.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include "sockets.h"
#include "downloads.h"
#include "uploads.h"
#include "nodes.h"
#include "misc.h"
#include "getline.h"

guint32 local_ip = 0;
gboolean is_firewalled = TRUE;		/* Assume the worst --RAM, 20/12/2001 */

static GSList *sl_incoming = (GSList *) NULL;	/* Track incoming sockets */

void socket_monitor_incoming(void)
{
	/* Did an incoming connection timout? */

	GSList *l;
	time_t now = time((time_t *) 0);

  retry:
	for (l = sl_incoming; l; l = l->next) {
		struct gnutella_socket *s = (struct gnutella_socket *) l->data;
		g_assert(s->last_update);
		/* We reuse the `node_connecting_timeout' parameter, need a new one? */
		if (now - s->last_update > node_connecting_timeout) {
			g_warning("connection from %s timed out (%d bytes read)",
					  ip_to_gchar(s->ip), s->pos);
			if (s->pos > 0)
				dump_hex(stderr, "Connection Header",
					s->buffer, MIN(s->pos, 80));
			socket_destroy(s);
			goto retry;			/* Don't know the internals of lists, retry */
		}
	}
}

void socket_shutdown(void)
{
	while (sl_incoming)
		socket_destroy((struct gnutella_socket *) sl_incoming->data);
}

/* ----------------------------------------- */

/* Destroy a socket, and free its resource if needed */

void socket_destroy(struct gnutella_socket *s)
{
	g_assert(s);

	if (s->type == GTA_TYPE_CONTROL && s->resource.node) {
		node_remove(s->resource.node, NULL);
		return;
	} else if (s->type == GTA_TYPE_DOWNLOAD && s->resource.download) {
		download_stop(s->resource.download, GTA_DL_ERROR, NULL);
		return;

	} else if (s->type == GTA_TYPE_UPLOAD && s->resource.upload) {
		upload_remove(s->resource.upload, NULL);
		return;
	}

	socket_free(s);
}

void socket_free(struct gnutella_socket *s)
{
	g_assert(s);
	if (s->last_update) {
		g_assert(sl_incoming);
		sl_incoming = g_slist_remove(sl_incoming, s);
	}
	if (s->gdk_tag)
		gdk_input_remove(s->gdk_tag);
	if (s->getline)
		getline_free(s->getline);
	if (s->file_desc != -1)
		close(s->file_desc);
	g_free(s);
}

/* ----------------------------------------- */

/* Read bytes on an unknown incoming socket */

static void socket_read(gpointer data, gint source, GdkInputCondition cond)
{
	gint r;
	struct gnutella_socket *s = (struct gnutella_socket *) data;
	guint count;
	guint parsed;
	guchar *first;

	//s->type = 0;

	if (cond & GDK_INPUT_EXCEPTION) {
		socket_destroy(s);
		return;
	}

	g_assert(s->pos == 0);		/* We read a line, then leave this callback */

	count = sizeof(s->buffer) - s->pos - 1;		/* -1 to allow trailing NUL */
	if (count <= 0) {
		g_warning
			("socket_read(): incoming buffer full, disconnecting from %s",
			 ip_to_gchar(s->ip));
		dump_hex(stderr, "Leading Data", s->buffer, MIN(s->pos, 256));
		socket_destroy(s);
		return;
	}

	r = read(s->file_desc, s->buffer + s->pos, count);

	if (r == 0) {
		socket_destroy(s);
		return;
	} else if (r < 0 && errno == EAGAIN)
		return;
	else if (r < 0) {
		socket_destroy(s);
		return;
	}

	s->last_update = time((time_t *) 0);
	s->pos += r;

	/*
	 * Get first line.
	 */

	switch (getline_read(s->getline, s->buffer, s->pos, &parsed)) {
	case READ_OVERFLOW:
		g_warning("socket_read(): first line too long, disconnecting from %s",
			 ip_to_gchar(s->ip));
		dump_hex(stderr, "Leading Data", s->buffer, MIN(s->pos, 256));
		socket_destroy(s);
		return;
	case READ_DONE:
		if (s->pos != parsed)
			memmove(s->buffer, s->buffer + parsed, s->pos - parsed);
		s->pos -= parsed;
		break;
	case READ_MORE:		/* ok, but needs more data */
	default:
		g_assert(parsed == s->pos);
		s->pos = 0;
		return;
	}

	/*
	 * We come here only when we got the first line of data.
	 *
	 * Whatever happens now, we're not going to use the existing read
	 * callback, and we'll no longer monitor the socket via the `sl_incoming'
	 * list: if it's a node connection, we'll monitor the node, if it's
	 * an upload, we'll monitor the uplaod.
	 */

	gdk_input_remove(s->gdk_tag);
	s->gdk_tag = 0;
	sl_incoming = g_slist_remove(sl_incoming, s);
	s->last_update = 0;

	/*
	 * Dispatch request.
	 */

	first = getline_str(s->getline);

	if (0 == strncmp(first, gnutella_hello, gnutella_hello_length))
		node_add(s, s->ip, s->port);	/* Incoming control connection */
	else if (0 == strncmp(first, "GET ", 4))
		upload_add(s);
	else if (0 == strncmp(first, "HEAD ", 5))
		upload_add(s);
	else if (0 == strncmp(first, "GIV ", 4))
		download_push_ack(s);
	else {
		gint len = getline_length(s->getline);
		g_warning("socket_read(): Got an unknown incoming connection, "
			"dropping it.");
		dump_hex(stderr, "First Line", first, MIN(len, 160));
		socket_destroy(s);
	}
}

/*
 * Sockets connection
 */

void socket_connected(gpointer data, gint source, GdkInputCondition cond)
{
	/* We are connected to somebody */

	struct gnutella_socket *s = (struct gnutella_socket *) data;

	if (cond & GDK_INPUT_EXCEPTION) {	/* Error while connecting */
		if (s->type == GTA_TYPE_CONTROL && s->resource.node)
			node_remove(s->resource.node, "Connection failed");
		else if (s->type == GTA_TYPE_DOWNLOAD && s->resource.download)
			download_fallback_to_push(s->resource.download, FALSE);
		else if (s->type == GTA_TYPE_UPLOAD && s->resource.upload)
			upload_remove(s->resource.upload, "Connection failed");
		else
			socket_destroy(s);
		return;
	}

	if (cond & GDK_INPUT_READ) {
		if (
			proxy_connections
			&& s->direction == GTA_CONNECTION_PROXY_OUTGOING
		) {
			gdk_input_remove(s->gdk_tag);
			s->gdk_tag = 0;

			if (socks_protocol == 4) {
				if (recv_socks(s) != 0) {
					socket_destroy(s);
					return;
				}

				s->direction = GTA_CONNECTION_OUTGOING;

				s->gdk_tag =
					gdk_input_add(s->file_desc,
								  GDK_INPUT_READ | GDK_INPUT_WRITE |
								  GDK_INPUT_EXCEPTION, socket_connected,
								  (gpointer) s);
				return;
			} else if (socks_protocol == 5) {
				if (connect_socksv5(s) != 0) {
					socket_destroy(s);
					return;
				}

				if (s->pos > 5) {
					s->direction = GTA_CONNECTION_OUTGOING;

					s->gdk_tag =
						gdk_input_add(s->file_desc,
									  GDK_INPUT_READ | GDK_INPUT_WRITE |
									  GDK_INPUT_EXCEPTION,
									  socket_connected, (gpointer) s);

					return;
				} else
					s->gdk_tag =
						gdk_input_add(s->file_desc,
									  GDK_INPUT_WRITE |
									  GDK_INPUT_EXCEPTION,
									  socket_connected, (gpointer) s);

				return;

			}
		}
	}

	if (cond & GDK_INPUT_WRITE) {
		/* We are just connected to our partner */
		gint res, option, size = sizeof(gint);

		gdk_input_remove(s->gdk_tag);
		s->gdk_tag = 0;

		/* Check wether the socket is really connected */

		res = getsockopt(s->file_desc, SOL_SOCKET, SO_ERROR,
					   (void *) &option, &size);

		if (res == -1 || option) {
			if (s->type == GTA_TYPE_CONTROL && s->resource.node)
				node_remove(s->resource.node, "Connection failed");
			else if (s->type == GTA_TYPE_DOWNLOAD && s->resource.download)
				download_fallback_to_push(s->resource.download, FALSE);
			else
				socket_destroy(s);
			return;
		}

		if (proxy_connections
			&& s->direction == GTA_CONNECTION_PROXY_OUTGOING) {
			if (socks_protocol == 4) {

				if (send_socks(s) != 0) {
					socket_destroy(s);
					return;
				}
			} else if (socks_protocol == 5) {
				if (connect_socksv5(s) != 0) {
					socket_destroy(s);
					return;
				}

			}

			s->gdk_tag =
				gdk_input_add(s->file_desc,
							  GDK_INPUT_READ | GDK_INPUT_EXCEPTION,
							  socket_connected, (gpointer) s);
			return;
		}

		s->pos = 0;
		memset(s->buffer, 0, sizeof(s->buffer));

		g_assert(s->gdk_tag == 0);

		switch (s->type) {
		case GTA_TYPE_CONTROL:
			{
				struct gnutella_node *n = s->resource.node;

				g_assert(n->socket == s);
				node_init_outgoing(n);
			}
			break;

		case GTA_TYPE_DOWNLOAD:
			{
				struct download *d = s->resource.download;

				g_assert(d->socket == s);
				download_send_request(d);
			}
			break;

		case GTA_TYPE_UPLOAD:
			{
				struct upload *u = s->resource.upload;

				g_assert(u->socket == s);
				upload_push_conf(u);
			}
			break;

		default:
			g_warning("socket_connected(): Unknown socket type %d !", s->type);
			socket_destroy(s);		/* ? */
			break;
		}
	}
}

int guess_local_ip(int sd, guint32 * ip_addr, guint16 * ip_port)
{
	struct sockaddr_in addr;
	gint len = sizeof(struct sockaddr_in);

	if (getsockname(sd, (struct sockaddr *) &addr, &len) == -1) {
		return -1;
	} else {
		if (ip_addr)
			*ip_addr = g_ntohl(addr.sin_addr.s_addr);
		if (ip_port)
			*ip_port = g_ntohs(addr.sin_port);
		return 0;
	}
}


static void socket_accept(gpointer data, gint source,
						  GdkInputCondition cond)
{
	/* Someone is connecting to us */

	struct sockaddr_in addr;
	gint sd, len = sizeof(struct sockaddr_in);
	struct gnutella_socket *s = (struct gnutella_socket *) data;
	struct gnutella_socket *t = NULL;

	if (cond & GDK_INPUT_EXCEPTION) {
		g_warning("Input Exception for listening socket #%d !!!!\n",
				  s->file_desc);
		gtk_gnutella_exit(2);
		return;
	}

	switch (s->type) {
	case GTA_TYPE_CONTROL:
	case GTA_TYPE_DOWNLOAD:
		/* No listening socket ever created for uploads --RAM */
		break;

	default:
		g_warning("socket_accept(): Unknown listning socket type %d !\n",
				  s->type);
		socket_destroy(s);
		return;
	}

	sd = accept(s->file_desc, (struct sockaddr *) &addr, &len);

	if (sd == -1) {
		g_warning("accept() failed (%s)", g_strerror(errno));
		return;
	}

	if (!local_ip)
		guess_local_ip(sd, &local_ip, NULL);

	/* Create a new struct socket for this incoming connection */

	fcntl(sd, F_SETFL, O_NONBLOCK);	/* Set the file descriptor non blocking */

	t = (struct gnutella_socket *) g_malloc0(sizeof(struct gnutella_socket));

	t->file_desc = sd;
	t->ip = g_ntohl(addr.sin_addr.s_addr);
	t->port = g_ntohs(addr.sin_port);
	t->direction = GTA_CONNECTION_INCOMING;
	t->type = s->type;
	t->local_port = s->local_port;
	t->getline = getline_make();

	switch (s->type) {
	case GTA_TYPE_CONTROL:
		t->gdk_tag =
			gdk_input_add(sd, GDK_INPUT_READ | GDK_INPUT_EXCEPTION,
						  socket_read, t);
		/*
		 * Whilst the socket is attached to that callback, it has been
		 * freshly accepted and we don't know what we're going to do with
		 * it.	Is it an incoming node connection or an upload request?
		 * Can't tell until we have read enough bytes.
		 *
		 * However, we must guard against a subtle DOS attack whereby
		 * someone would connect to us and then send only one byte (say),
		 * then nothing.  The socket would remain connected, without
		 * being monitored for timeout by the node/upload code.
		 *
		 * Insert the socket to the `sl_incoming' list, and have it
		 * monitored periodically.	We know the socket is on the list
		 * as soon as it has a non-zero last_update field.
		 *				--RAM, 07/09/2001
		 */

		sl_incoming = g_slist_prepend(sl_incoming, t);
		t->last_update = time((time_t *) 0);
		break;

	default:
		g_assert(0);			/* Can't happen */
		break;
	}

	/*
	 * Since someone connected to us, we're not completely firewalled.
	 * The Gnutella port has at least been opened.
	 *		--RAM, 20/12/2001
	 */

	if (is_firewalled) {
		is_firewalled = FALSE;
		if (dbg) printf("Got evidence that we're not fully firewalled\n");
	}
}

/*
 * Sockets creation
 */

struct gnutella_socket *socket_connect(guint32 ip, guint16 port, gint type)
{
	/* Create a socket and try to connect it to ip:port */

	gint sd, option = 1, res = 0;
	struct sockaddr_in addr, lcladdr;
	struct gnutella_socket *s;

	sd = socket(AF_INET, SOCK_STREAM, 0);

	if (sd == -1) {
		g_warning("Unable to create a socket (%s)", g_strerror(errno));
		return NULL;
	}

	s = (struct gnutella_socket *) g_malloc0(sizeof(struct gnutella_socket));

	s->type = type;
	s->direction = GTA_CONNECTION_OUTGOING;
	s->file_desc = sd;
	s->ip = ip;
	s->port = port;

	setsockopt(sd, SOL_SOCKET, SO_KEEPALIVE, (void *) &option,
			   sizeof(option));
	setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, (void *) &option,
			   sizeof(option));

	fcntl(sd, F_SETFL, O_NONBLOCK);	/* Set the file descriptor non blocking */

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = g_htonl(ip);
	addr.sin_port = g_htons(port);

	/*
	 * Now we check if we're forcing a local IP, and make it happen if so.
	 *   --JSL
	 */
	if (force_local_ip) {
		lcladdr.sin_family = AF_INET;
		lcladdr.sin_addr.s_addr = g_htonl(forced_local_ip);
		lcladdr.sin_port = g_htons(0);

		/*
		 * Note: we ignore failures: it will be automatic at connect()
		 * It's useful only for people forcing the IP without being
		 * behind a masquerading firewall --RAM.
		 */
		(void) bind(sd, (struct sockaddr *) &lcladdr,
			sizeof(struct sockaddr_in));
	}

	if (proxy_connections) {
		lcladdr.sin_family = AF_INET;
		lcladdr.sin_port = INADDR_ANY;

		(void) bind(sd, (struct sockaddr *) &lcladdr,
			sizeof(struct sockaddr_in));

		res = proxy_connect(sd, (struct sockaddr *) &addr,
			sizeof(struct sockaddr_in));

		s->direction = GTA_CONNECTION_PROXY_OUTGOING;
	} else
		res = connect(sd, (struct sockaddr *) &addr,
			sizeof(struct sockaddr_in));

	if (res == -1 && errno != EINPROGRESS) {
		g_warning("Unable to connect to %s: (%s)",
			ip_port_to_gchar(s->ip, s->port), g_strerror(errno));
		socket_destroy(s);
		return NULL;
	}

	fcntl(sd, F_SETFL, O_NONBLOCK);	/* Set the file descriptor non blocking */

	/* Always keep our IP current, in case of dynamic address */
	if (guess_local_ip(sd, &local_ip, &s->local_port) == -1) {
		g_warning("Unable to guess our IP ! (%s)", g_strerror(errno));
		socket_destroy(s);
		return NULL;
	}

	g_assert(s->gdk_tag == 0);

	if (proxy_connections)
		s->gdk_tag = gdk_input_add(sd,
			GDK_INPUT_READ | GDK_INPUT_WRITE | GDK_INPUT_EXCEPTION,
			socket_connected, s);
	else
		s->gdk_tag = gdk_input_add(sd,
			GDK_INPUT_WRITE | GDK_INPUT_EXCEPTION,
			socket_connected, s);

	return s;
}

struct gnutella_socket *socket_listen(guint32 ip, guint16 port, gint type)
{
	/* Create a socket, then bind() and listen() it */

	int sd, option = 1;
	unsigned int l = sizeof(struct sockaddr_in);
	struct sockaddr_in addr;
	struct gnutella_socket *s;

	sd = socket(AF_INET, SOCK_STREAM, 0);

	if (sd == -1) {
		g_warning("Unable to create a socket (%s)", g_strerror(errno));
		return NULL;
	}

	s = (struct gnutella_socket *) g_malloc0(sizeof(struct gnutella_socket));

	s->type = type;
	s->direction = GTA_CONNECTION_LISTENING;
	s->file_desc = sd;
	s->pos = 0;

	setsockopt(sd, SOL_SOCKET, SO_KEEPALIVE, (void *) &option,
			   sizeof(option));
	setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, (void *) &option,
			   sizeof(option));

	fcntl(sd, F_SETFL, O_NONBLOCK);	/* Set the file descriptor non blocking */

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = g_htonl((ip) ? ip : INADDR_ANY);
	addr.sin_port = g_htons(port);

	/* bind() the socket */

	if (bind(sd, (struct sockaddr *) &addr, l) == -1) {
		g_warning("Unable to bind() the socket on port %u (%s)",
				  port, g_strerror(errno));
		socket_destroy(s);
		return NULL;
	}

	/* listen() the socket */

	if (listen(sd, 5) == -1) {
		g_warning("Unable to listen() the socket (%s)", g_strerror(errno));
		socket_destroy(s);
		return NULL;
	}

	/* Get the port of the socket, if needed */

	if (!port) {
		option = sizeof(struct sockaddr_in);

		if (getsockname(sd, (struct sockaddr *) &addr, &option) == -1) {
			g_warning("Unable to get the port of the socket: "
				"getsockname() failed (%s)", g_strerror(errno));
			socket_destroy(s);
			return NULL;
		}

		s->local_port = g_ntohs(addr.sin_port);
	} else
		s->local_port = port;

	s->gdk_tag =
		gdk_input_add(sd, GDK_INPUT_READ | GDK_INPUT_EXCEPTION,
					  socket_accept, s);

	return s;
}



static void show_error(char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
}

/*
 * The socks 4/5 code was taken from tsocks 1.16 Copyright (C) 2000 Shaun Clowes
 * It was modified to work with gtk_gnutella and non-blocking sockets. --DW
 */

int proxy_connect(int __fd, const struct sockaddr *__addr, socklen_t __len)
{
	struct sockaddr_in *connaddr;
	void **kludge;
	struct sockaddr_in server;

	int rc = 0;


	if (!(inet_aton(proxy_ip, &server.sin_addr))) {
		show_error("The SOCKS server (%s) in configuration "
				   "file is invalid\n", proxy_ip);
	} else {
		/* Construct the addr for the socks server */
		server.sin_family = AF_INET;	/* host byte order */
		server.sin_port = htons(proxy_port);
		bzero(&(server.sin_zero), 8);	/* zero the rest of the struct */
	}


	/* Ok, so this method sucks, but it's all I can think of */

	kludge = (void *) &__addr;
	connaddr = (struct sockaddr_in *) *kludge;

	rc = connect(__fd, (struct sockaddr *) &server,
				 sizeof(struct sockaddr));

	return rc;

}

struct socksent {
	struct in_addr localip;
	struct in_addr localnet;
	struct socksent *next;
};

struct sockreq {
	int8_t version;
	int8_t command;
	int16_t dstport;
	int32_t dstip;
	/* A null terminated username goes here */
};

struct sockrep {
	int8_t version;
	int8_t result;
	int16_t ignore1;
	int32_t ignore2;
};

int send_socks(struct gnutella_socket *s)
{
	int rc = 0;
	int length = 0;
	char *realreq;
	struct passwd *user;
	struct sockreq *thisreq;


	/* Determine the current username */
	user = getpwuid(getuid());

	/* Allocate enough space for the request and the null */
	/* terminated username */
	length = sizeof(struct sockreq) +
		(user == NULL ? 1 : strlen(user->pw_name) + 1);
	if ((realreq = malloc(length)) == NULL) {
		/* Could not malloc, bail */
		exit(1);
	}
	thisreq = (struct sockreq *) realreq;

	/* Create the request */
	thisreq->version = 4;
	thisreq->command = 1;
	thisreq->dstport = htons(s->port);
	thisreq->dstip = htonl(s->ip);

	/* Copy the username */
	strcpy(realreq + sizeof(struct sockreq),
		   (user == NULL ? "" : user->pw_name));

	/* Send the socks header info */
	if ((rc = send(s->file_desc, (void *) thisreq, length, 0)) < 0) {
		show_error("Error attempting to send SOCKS request (%s)\n",
				   strerror(errno));
		rc = rc;
		return -1;
	}

	free(thisreq);

	return 0;

}

int recv_socks(struct gnutella_socket *s)
{
	int rc = 0;
	struct sockrep thisrep;

	if ((rc =
		 recv(s->file_desc, (void *) &thisrep, sizeof(struct sockrep),
			  0)) < 0) {
		show_error("Error attempting to receive SOCKS " "reply (%s)\n",
				   g_strerror(errno));
		rc = ECONNREFUSED;
	} else if (rc < sizeof(struct sockrep)) {
		show_error("Short reply from SOCKS server\n");
		/* Let the application try and see how they */
		/* go										*/
		rc = 0;
	} else if (thisrep.result == 91) {
		show_error("SOCKS server refused connection\n");
		rc = ECONNREFUSED;
	} else if (thisrep.result == 92) {
		show_error("SOCKS server refused connection "
				   "because of failed connect to identd "
				   "on this machine\n");
		rc = ECONNREFUSED;
	} else if (thisrep.result == 93) {
		show_error("SOCKS server refused connection "
				   "because identd and this library "
				   "reported different user-ids\n");
		rc = ECONNREFUSED;
	} else {
		rc = 0;
	}

	if (rc != 0) {
		errno = rc;
		return -1;
	}

	return 0;

}

/*
0: Send
1: Recv
.. 
4: Send
5: Recv

6: Done
*/

int connect_socksv5(struct gnutella_socket *s)
{
	int rc = 0;
	int offset = 0;
	char *verstring = "\x05\x02\x02\x00";
	char *uname, *upass;
	struct passwd *nixuser;
	char *buf;
	int sockid;

	sockid = s->file_desc;

	buf = (char *) s->buffer;

	switch (s->pos) {

	case 0:
		/* Now send the method negotiation */
		if ((rc = send(sockid, (void *) verstring, 4, 0)) < 0) {
			show_error("Error %d attempting to send SOCKS "
					   "method negotiation\n", errno);
			return (-1);
		}
		s->pos++;
		break;

	case 1:
		/* Now receive the reply as to which method we're using */
		if ((rc = recv(sockid, (void *) buf, 2, 0)) < 0) {
			show_error("Error %d attempting to receive SOCKS "
					   "method negotiation reply\n", errno);
			rc = ECONNREFUSED;
			return (rc);
		}

		if (rc < 2) {
			show_error("Short reply from SOCKS server\n");
			rc = ECONNREFUSED;
			return (rc);
		}

		/* See if we offered an acceptable method */
		if (buf[1] == '\xff') {
			show_error("SOCKS server refused authentication methods\n");
			rc = ECONNREFUSED;
			return (rc);
		}

		if (
			(unsigned short int) buf[1] == 2 &&
			socksv5_user && socksv5_user[0]		/* has provided user info */
		)
			s->pos++;
		else
			s->pos += 3;
		break;
	case 2:
		/* If the socks server chose username/password authentication */
		/* (method 2) then do that */


		/* Determine the current *nix username */
		nixuser = getpwuid(getuid());

		if (((uname = socksv5_user) == NULL) &&
			((uname =
			  (nixuser == NULL ? NULL : nixuser->pw_name)) == NULL)) {
			show_error("No Username to authenticate with.");
			rc = ECONNREFUSED;
			return (rc);
		}

		if (((upass = socksv5_pass) == NULL)) {
			show_error("No Password to authenticate with.");
			rc = ECONNREFUSED;
			return (rc);
		}

		offset = 0;
		buf[offset] = '\x01';
		offset++;
		buf[offset] = (int8_t) strlen(uname);
		offset++;
		memcpy(&buf[offset], uname, strlen(uname));
		offset = offset + strlen(uname);
		buf[offset] = (int8_t) strlen(upass);
		offset++;
		memcpy(&buf[offset], upass, strlen(upass));
		offset = offset + strlen(upass);

		/* Send out the authentication */
		if ((rc = send(sockid, (void *) buf, offset, 0)) < 0) {
			show_error("Error %d attempting to send SOCKS "
					   "authentication\n", errno);
			return (-1);
		}

		s->pos++;

		break;
	case 3:
		/* Receive the authentication response */
		if ((rc = recv(sockid, (void *) buf, 2, 0)) < 0) {
			show_error("Error %d attempting to receive SOCKS "
					   "authentication reply\n", errno);
			rc = ECONNREFUSED;
			return (rc);
		}

		if (rc < 2) {
			show_error("Short reply from SOCKS server\n");
			rc = ECONNREFUSED;
			return (rc);
		}

		if (buf[1] != '\x00') {
			show_error("SOCKS authentication failed, "
					   "check username and password\n");
			rc = ECONNREFUSED;
			return (rc);
		}
		s->pos++;
		break;
	case 4:
		/* Now send the connect */
		buf[0] = '\x05';		/* Version 5 SOCKS */
		buf[1] = '\x01';		/* Connect request */
		buf[2] = '\x00';		/* Reserved		*/
		buf[3] = '\x01';		/* IP version 4	*/
		*(uint32_t *)(buf + 4) = htonl(s->ip);
		*(uint16_t *)(buf + 8) = htons(s->port);

		/* Now send the connection */
		if ((rc = send(sockid, (void *) buf, 10, 0)) <= 0) {
			show_error("Error %d attempting to send SOCKS "
					   "connect command\n", errno);
			return (-1);
		}

		s->pos++;
		break;
	case 5:
		/* Now receive the reply to see if we connected */
		if ((rc = recv(sockid, (void *) buf, 10, 0)) < 0) {
			show_error("Error %d attempting to receive SOCKS "
					   "connection reply\n", errno);
			rc = ECONNREFUSED;
			return (rc);
		}
		if (dbg) printf("connect_socksv5: Step 5, bytes recv'd %i\n", rc);
		if (rc < 10) {
			show_error("Short reply from SOCKS server\n");
			rc = ECONNREFUSED;
			return (rc);
		}

		/* See the connection succeeded */
		if (buf[1] != '\x00') {
			show_error("SOCKS connect failed: ");
			switch ((int8_t) buf[1]) {
			case 1:
				show_error("General SOCKS server failure\n");
				return (ECONNABORTED);
			case 2:
				show_error("Connection denied by rule\n");
				return (ECONNABORTED);
			case 3:
				show_error("Network unreachable\n");
				return (ENETUNREACH);
			case 4:
				show_error("Host unreachable\n");
				return (EHOSTUNREACH);
			case 5:
				show_error("Connection refused\n");
				return (ECONNREFUSED);
			case 6:
				show_error("TTL Expired\n");
				return (ETIMEDOUT);
			case 7:
				show_error("Command not supported\n");
				return (ECONNABORTED);
			case 8:
				show_error("Address type not supported\n");
				return (ECONNABORTED);
			default:
				show_error("Unknown error\n");
				return (ECONNABORTED);
			}
		}

		s->pos++;

		break;
	}

	return (0);

}

/* vi: set ts=4: */
