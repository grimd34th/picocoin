/* Copyright 2012 exMULTI, Inc.
 * Distributed under the MIT/X11 software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 */
#include "picocoin-config.h"

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef WIN32
#include <mingw.h>
#include "fakepoll.h"
#else
#include <sys/wait.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#endif
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <glib.h>
#include <event2/event.h>
#include <ccoin/util.h>
#include <ccoin/mbr.h>
#include <ccoin/core.h>
#include <ccoin/message.h>
#include "picocoin.h"
#include "peerman.h"
#include <ccoin/blkdb.h>

struct net_engine {
	bool		running;
	int		rx_pipefd[2];
	int		tx_pipefd[2];
	int		par_read;
	int		par_write;
	pid_t		child;
};

enum netcmds {
	NC_OK,
	NC_ERR,
	NC_TIMEOUT,
	NC_START,
	NC_STOP,
};

struct net_child_info {
	int			read_fd;
	int			write_fd;

	struct peer_manager	*peers;
	struct blkdb		*db;

	GPtrArray		*conns;
	struct event_base	*eb;
};

struct nc_conn {
	int			fd;
	struct bp_address	addr;
	bool			ipv4;
	bool			connected;
	struct event		*ev;
	struct net_child_info	*nci;

	struct event		*write_ev;
	GList			*write_q;	/* of struct buffer */
	unsigned int		write_partial;

	struct p2p_message	msg;

	void			*msg_p;
	unsigned int		expected;
	bool			reading_hdr;
	unsigned char		hdrbuf[P2P_HDR_SZ];

	bool			seen_version;
	bool			seen_verack;
	uint32_t		protover;
};


enum {
	NC_MAX_CONN		= 8,
};

static void nc_conn_free(struct nc_conn *conn);
static bool nc_conn_read_enable(struct nc_conn *conn);
static bool nc_conn_read_disable(struct nc_conn *conn);
static bool nc_conn_write_enable(struct nc_conn *conn);
static bool nc_conn_write_disable(struct nc_conn *conn);

static void pipwr(int fd, const void *buf, size_t len)
{
	while (len > 0) {
		ssize_t wrc;

		wrc = write(fd, buf, len);
		if (wrc < 0) {
			perror("pipe write");
			return;
		}

		buf += wrc;
		len -= wrc;
	}
}

static void sendcmd(int fd, enum netcmds nc)
{
	uint8_t v = nc;
	pipwr(fd, &v, 1);
}

static enum netcmds readcmd(int fd, int timeout_secs)
{
	struct pollfd pfd = { fd, POLLIN };
	int prc = poll(&pfd, 1, timeout_secs ? timeout_secs * 1000 : -1);
	if (prc < 0) {
		perror("pipe poll");
		return NC_ERR;
	}
	if (prc == 0)
		return NC_TIMEOUT;

	uint8_t v;
	ssize_t rrc = read(fd, &v, 1);
	if (rrc < 0) {
		perror("pipe read");
		return NC_ERR;
	}
	if (rrc != 1)
		return NC_ERR;

	return v;
}

static void nc_conn_build_iov(GList *write_q, unsigned int partial,
			      struct iovec **iov_, unsigned int *iov_len_)
{
	*iov_ = NULL;
	*iov_len_ = 0;

	unsigned int i, iov_len = g_list_length(write_q);
	struct iovec *iov = calloc(iov_len, sizeof(struct iovec));

	GList *tmp = write_q;

	i = 0;
	while (tmp) {
		struct buffer *buf = tmp->data;

		iov[i].iov_base = buf->p;
		iov[i].iov_len = buf->len;

		if (i == 0) {
			iov[0].iov_base += partial;
			iov[0].iov_len -= partial;
		}

		tmp = tmp->next;
		i++;
	}

	*iov_ = iov;
	*iov_len_ = iov_len;
}

static void nc_conn_written(struct nc_conn *conn, size_t bytes)
{
	while (bytes > 0) {
		GList *tmp;
		struct buffer *buf;
		unsigned int left;

		tmp = conn->write_q;
		buf = tmp->data;
		left = buf->len - conn->write_partial;

		/* buffer fully written; free */
		if (bytes >= left) {
			free(buf->p);
			free(buf);
			conn->write_partial = 0;
			conn->write_q = g_list_delete_link(tmp, tmp);

			bytes -= left;
		}

		/* buffer partially written; store state */
		else {
			conn->write_partial += bytes;
			break;
		}
	}
}

static void nc_conn_write_evt(int fd, short events, void *priv)
{
	struct nc_conn *conn = priv;
	struct iovec *iov = NULL;
	unsigned int iov_len = 0;

	/* build list of outgoing data buffers */
	nc_conn_build_iov(conn->write_q, conn->write_partial, &iov, &iov_len);

	/* send data to network */
	ssize_t wrc = writev(conn->fd, iov, iov_len);

	free(iov);

	if (wrc < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			nc_conn_free(conn);
		return;
	}

	/* handle partially and fully completed buffers */
	nc_conn_written(conn, wrc);

	/* thaw read, if write fully drained */
	if (!conn->write_q) {
		nc_conn_write_disable(conn);
		nc_conn_read_enable(conn);
	}
}

static bool nc_conn_send(struct nc_conn *conn, const char *command,
			 const void *data, size_t data_len)
{
	/* build wire message */
	GString *msg = message_str(chain->netmagic, command, data, data_len);
	if (!msg)
		return false;

	/* buffer now owns message data */
	struct buffer *buf = calloc(1, sizeof(struct buffer));
	buf->p = msg->str;
	buf->len = msg->len;

	g_string_free(msg, FALSE);

	/* if write q exists, write_evt will handle output */
	if (conn->write_q) {
		conn->write_q = g_list_append(conn->write_q, buf);
		return true;
	}

	/* attempt optimistic write */
	ssize_t wrc = write(conn->fd, buf->p, buf->len);
	if (wrc < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
			free(buf->p);
			free(buf);
			return false;
		}

		conn->write_q = g_list_append(conn->write_q, buf);
		goto out_wrstart;
	}

	/* message fully sent */
	if (wrc == buf->len) {
		free(buf->p);
		free(buf);
		return true;
	}

	/* message partially sent; pause read; poll for writable */
	conn->write_q = g_list_append(conn->write_q, buf);
	conn->write_partial = wrc;

out_wrstart:
	nc_conn_read_disable(conn);
	nc_conn_write_enable(conn);
	return true;
}

static bool nc_msg_version(struct nc_conn *conn)
{
	if (conn->seen_version)
		return false;
	conn->seen_version = true;

	struct const_buffer buf = { conn->msg.data, conn->msg.hdr.data_len };
	struct msg_version mv;
	bool rc = false;

	msg_version_init(&mv);

	if (!deser_msg_version(&mv, &buf))
		goto out;

	if (!(mv.nServices & NODE_NETWORK))	/* require NODE_NETWORK */
		goto out;
	if (mv.nonce == instance_nonce)		/* connected to ourselves? */
		goto out;

	conn->protover = MIN(mv.nVersion, PROTO_VERSION);

	/* acknowledge version receipt */
	if (!nc_conn_send(conn, "verack", NULL, 0))
		goto out;

	rc = true;

out:
	msg_version_free(&mv);
	return rc;
}

static bool nc_msg_addr(struct nc_conn *conn)
{
	struct const_buffer buf = { conn->msg.data, conn->msg.hdr.data_len };
	struct msg_addr ma;
	bool rc = false;

	msg_addr_init(&ma);

	if (!deser_msg_addr(conn->protover, &ma, &buf))
		goto out;

	/* ignore ancient addresses */
	if (conn->protover < CADDR_TIME_VERSION)
		goto out_ok;

	/* feed addresses to peer manager */
	unsigned int i;
	for (i = 0; i < ma.addrs->len; i++) {
		struct bp_address *addr = g_ptr_array_index(ma.addrs, i);
		peerman_add(conn->nci->peers, addr, false);
	}

out_ok:
	rc = true;

out:
	msg_addr_free(&ma);
	return rc;
}

static bool nc_msg_verack(struct nc_conn *conn)
{
	if (conn->seen_verack)
		return false;
	conn->seen_verack = true;

	/*
	 * When a connection attempt is made, the peer is deleted
	 * from the peer list.  When we successfully connect,
	 * the peer is re-added.  Thus, peers are immediately
	 * forgotten if they fail, on the first try.
	 */
	peerman_add(conn->nci->peers, &conn->addr, true);

	/* request peer addresses */
	if ((conn->protover >= CADDR_TIME_VERSION) &&
	    (!nc_conn_send(conn, "getaddr", NULL, 0)))
		return false;

	return true;
}

static bool nc_conn_message(struct nc_conn *conn)
{
	/* verify correct network */
	if (memcmp(conn->msg.hdr.netmagic, chain->netmagic, 4))
		return false;

	char *command = conn->msg.hdr.command;

	/* incoming message: version */
	if (!strncmp(command, "version", 12))
		return nc_msg_version(conn);

	/* "version" must be first message */
	if (!conn->seen_version)
		return false;

	/* incoming message: verack */
	if (!strncmp(command, "verack", 12))
		return nc_msg_verack(conn);

	/* "verack" must be second message */
	if (!conn->seen_verack)
		return false;

	/* incoming message: addr */
	if (!strncmp(command, "addr", 12))
		return nc_msg_addr(conn);

	/* ignore unknown messages */
	return true;
}

static bool nc_conn_ip_active(struct net_child_info *nci,
			      const unsigned char *ip)
{
	unsigned int i;
	for (i = 0; i < nci->conns->len; i++) {
		struct nc_conn *conn;

		conn = g_ptr_array_index(nci->conns, i);
		if (!memcmp(conn->addr.ip, ip, 16))
			return true;
	}

	return false;
}

static struct nc_conn *nc_conn_new(const struct bp_address *addr_in)
{
	struct nc_conn *conn;

	conn = calloc(1, sizeof(*conn));
	if (!conn)
		return NULL;

	conn->fd = -1;

	if (addr_in)
		memcpy(&conn->addr, addr_in, sizeof(*addr_in));

	return conn;
}

static void nc_conn_free(struct nc_conn *conn)
{
	if (!conn)
		return;

	if (conn->write_q) {
		GList *tmp = conn->write_q;

		while (tmp) {
			struct buffer *buf;

			buf = tmp->data;
			tmp = tmp->next;

			free(buf->p);
			free(buf);
		}

		g_list_free(conn->write_q);
	}

	if (conn->ev) {
		event_del(conn->ev);
		event_free(conn->ev);
	}
	if (conn->write_ev) {
		event_del(conn->write_ev);
		event_free(conn->write_ev);
	}

	if (conn->fd >= 0)
		close(conn->fd);

	free(conn->msg.data);

	free(conn);
}

static bool nc_conn_start(struct nc_conn *conn)
{
	/* create socket */
	conn->ipv4 = is_ipv4_mapped(conn->addr.ip);
	conn->fd = socket(conn->ipv4 ? AF_INET : AF_INET6,
			  SOCK_STREAM, IPPROTO_TCP);
	if (conn->fd < 0)
		return false;

	/* set non-blocking */
	int flags = fcntl(conn->fd, F_GETFL, 0);
	if ((flags < 0) ||
	    (fcntl(conn->fd, F_SETFL, flags | O_NONBLOCK) < 0))
		return false;

	struct sockaddr *saddr;
	struct sockaddr_in6 saddr6;
	struct sockaddr_in saddr4;
	socklen_t saddr_len;

	/* fill out connect(2) address */
	if (conn->ipv4) {
		memset(&saddr4, 0, sizeof(saddr4));
		saddr4.sin_family = AF_INET;
		memcpy(&saddr4.sin_addr.s_addr,
		       &conn->addr.ip[12], 4);
		saddr4.sin_port = htons(conn->addr.port);

		saddr = (struct sockaddr *) &saddr4;
		saddr_len = sizeof(saddr4);
	} else {
		memset(&saddr6, 0, sizeof(saddr6));
		saddr6.sin6_family = AF_INET6;
		memcpy(&saddr6.sin6_addr.s6_addr,
		       &conn->addr.ip[0], 16);
		saddr6.sin6_port = htons(conn->addr.port);

		saddr = (struct sockaddr *) &saddr6;
		saddr_len = sizeof(saddr6);
	}

	/* initiate TCP connection */
	if (connect(conn->fd, saddr, saddr_len) < 0)
		return false;

	return true;
}

static void nc_conn_got_header(struct nc_conn *conn)
{
	parse_message_hdr(&conn->msg.hdr, conn->hdrbuf);

	unsigned int data_len = conn->msg.hdr.data_len;
	if (data_len > (16 * 1024 * 1024))
		goto err_out;

	conn->msg.data = malloc(data_len);

	/* switch to read-body state */
	conn->msg_p = conn->msg.data;
	conn->expected = data_len;
	conn->reading_hdr = false;

	return;

err_out:
	nc_conn_free(conn);
}

static void nc_conn_got_msg(struct nc_conn *conn)
{
	if (!message_valid(&conn->msg))
		goto err_out;

	if (!nc_conn_message(conn))
		goto err_out;

	free(conn->msg.data);
	conn->msg.data = NULL;

	/* switch to read-header state */
	conn->msg_p = conn->hdrbuf;
	conn->expected = P2P_HDR_SZ;
	conn->reading_hdr = true;

	return;

err_out:
	nc_conn_free(conn);
}

static void nc_conn_read_evt(int fd, short events, void *priv)
{
	struct nc_conn *conn = priv;

	ssize_t rrc = read(fd, conn->msg_p, conn->expected);
	if (rrc <= 0) {
		nc_conn_free(conn);
		return;
	}

	conn->msg_p += rrc;
	conn->expected -= rrc;

	if (conn->expected == 0) {
		if (conn->reading_hdr)
			nc_conn_got_header(conn);
		else
			nc_conn_got_msg(conn);
	}
}

static GString *nc_version_build(struct nc_conn *conn)
{
	struct msg_version mv;

	msg_version_init(&mv);

	mv.nVersion = PROTO_VERSION;
	mv.nTime = (int64_t) time(NULL);
	mv.nonce = instance_nonce;
	sprintf(mv.strSubVer, "/picocoin:%s/", VERSION);
	mv.nStartingHeight = conn->nci->db->nBestHeight;

	GString *rs = ser_msg_version(&mv);

	msg_version_free(&mv);

	return rs;
}

static bool nc_conn_read_enable(struct nc_conn *conn)
{
	if (conn->ev)
		return true;

	conn->ev = event_new(conn->nci->eb, conn->fd, EV_READ | EV_PERSIST,
			     nc_conn_read_evt, conn);
	if (!conn->ev)
		return false;

	if (event_add(conn->ev, NULL) != 0) {
		event_free(conn->ev);
		conn->ev = NULL;
		return false;
	}

	return true;
}

static bool nc_conn_read_disable(struct nc_conn *conn)
{
	if (!conn->ev)
		return true;

	event_del(conn->ev);
	event_free(conn->ev);

	conn->ev = NULL;

	return true;
}

static bool nc_conn_write_enable(struct nc_conn *conn)
{
	if (conn->write_ev)
		return true;

	conn->write_ev = event_new(conn->nci->eb, conn->fd,
				   EV_WRITE | EV_PERSIST,
				   nc_conn_write_evt, conn);
	if (!conn->write_ev)
		return false;

	if (event_add(conn->write_ev, NULL) != 0) {
		event_free(conn->write_ev);
		conn->write_ev = NULL;
		return false;
	}

	return true;
}

static bool nc_conn_write_disable(struct nc_conn *conn)
{
	if (!conn->write_ev)
		return true;

	event_del(conn->write_ev);
	event_free(conn->write_ev);

	conn->write_ev = NULL;

	return true;
}

static void nc_conn_evt_connected(int fd, short events, void *priv)
{
	struct nc_conn *conn = priv;

	int err = 0;
	socklen_t len = sizeof(err);

	/* check success of connect(2) */
	if ((getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) ||
	    (err != 0))
		goto err_out;

	conn->connected = true;

	event_free(conn->ev);
	conn->ev = NULL;

	/* build and send "version" message */
	GString *msg_data = nc_version_build(conn);
	GString *msg = message_str(chain->netmagic, "version",
				   msg_data->str, msg_data->len);
	g_string_free(msg_data, TRUE);

	unsigned int msg_len = msg->len;
	ssize_t wrc = write(conn->fd, msg->str, msg_len);

	g_string_free(msg, TRUE);

	if (wrc != msg_len)
		goto err_out;

	/* switch to read-header state */
	conn->msg_p = conn->hdrbuf;
	conn->expected = P2P_HDR_SZ;
	conn->reading_hdr = true;

	if (!nc_conn_read_enable(conn))
		goto err_out;

	return;

err_out:
	nc_conn_free(conn);
}

static void nc_conns_open(struct net_child_info *nci)
{
	while ((g_hash_table_size(nci->peers->map_addr) > 0) &&
	       (nci->conns->len < NC_MAX_CONN)) {

		/* delete peer from front of address list.  it will be
		 * re-added before writing peer file, if successful
		 */
		struct bp_address *addr = peerman_pop(nci->peers);

		struct nc_conn *conn = nc_conn_new(addr);
		conn->nci = nci;
		free(addr);

		/* are we already connected to this IP? */
		if (nc_conn_ip_active(nci, conn->addr.ip))
			goto err_loop;

		/* initiate non-blocking connect(2) */
		if (!nc_conn_start(conn))
			goto err_loop;

		/* add to our list of monitored event sources */
		conn->ev = event_new(nci->eb, conn->fd, EV_WRITE,
				     nc_conn_evt_connected, conn);
		if (!conn->ev)
			goto err_loop;

		struct timeval timeout = { 60, };
		if (event_add(conn->ev, &timeout) != 0)
			goto err_loop;

		/* add to our list of active connections */
		g_ptr_array_add(nci->conns, conn);

		continue;

err_loop:
		nc_conn_free(conn);
	}
}

static void nc_pipe_evt(int fd, short events, void *priv)
{
	struct net_child_info *nci = priv;

	enum netcmds nc = readcmd(nci->read_fd, 0);
	switch (nc) {

	case NC_START:
		sendcmd(nci->write_fd, NC_OK);
		break;

	case NC_STOP:
		sendcmd(nci->write_fd, NC_OK);
		event_base_loopbreak(nci->eb);
		break;

	default:
		exit(1);
	}
}

static void network_child(int read_fd, int write_fd)
{
	/*
	 * read network peers
	 */
	struct peer_manager *peers;

	peers = peerman_read();
	if (!peers) {
		peers = peerman_seed();
		peerman_write(peers);
	}

	/*
	 * read block database
	 */
	struct blkdb db;
	if (!blkdb_init(&db, chain->netmagic, &chain_genesis))
		exit(1);

	char *blkdb_fn = setting("blkdb");
	if (!blkdb_fn)
		exit(1);
	if ((access(blkdb_fn, F_OK) == 0) &&
	    (!blkdb_read(&db, blkdb_fn)))
		exit(1);

	/*
	 * prep block database for new records
	 */
	db.fd = open(blkdb_fn, O_WRONLY | O_APPEND | O_CREAT, 0666);
	if (db.fd < 0)
		exit(1);
	
	/*
	 * set up libevent dispatch
	 */
	struct net_child_info nci = { read_fd, write_fd, peers, &db };
	nci.conns = g_ptr_array_sized_new(8);

	struct event *pipe_evt;

	nci.eb = event_base_new();
	pipe_evt = event_new(nci.eb, read_fd, EV_READ | EV_PERSIST,
			     nc_pipe_evt, &nci);
	event_add(pipe_evt, NULL);

	nc_conns_open(&nci);		/* start opening P2P connections */

	/* main loop */
	event_base_dispatch(nci.eb);

	/* cleanup: just the minimum for file I/O correctness */
	peerman_write(peers);
	blkdb_free(&db);
	exit(0);
}

struct net_engine *neteng_new(void)
{
	struct net_engine *neteng;

	neteng = calloc(1, sizeof(*neteng));

	neteng->rx_pipefd[0] = -1;
	neteng->rx_pipefd[1] = -1;
	neteng->tx_pipefd[0] = -1;
	neteng->tx_pipefd[1] = -1;

	return neteng;
}

static void neteng_child_kill(pid_t child)
{
	kill(child, SIGTERM);
	sleep(1);
	waitpid(child, NULL, WNOHANG);
}

static bool neteng_cmd_exec(pid_t child, int read_fd, int write_fd,
			    enum netcmds nc)
{
	sendcmd(write_fd, nc);

	enum netcmds ncr = readcmd(read_fd, 60);
	if (ncr != NC_OK)
		return false;

	return true;
}

bool neteng_start(struct net_engine *neteng)
{
	if (neteng->running)
		return false;

	if (pipe(neteng->rx_pipefd) < 0)
		return false;
	if (pipe(neteng->tx_pipefd) < 0)
		goto err_out_rxfd;

	#ifdef WIN32 
	neteng->child = createthread();
	#else 
	neteng->child = fork();
	#endif
	if (neteng->child == -1)
		goto err_out_txfd;

	/* child execution path continues here */
	if (neteng->child == 0) {
		network_child(neteng->tx_pipefd[0], neteng->rx_pipefd[1]);
		exit(0);
	}

	/* otherwise, we are the parent */

	int par_read = neteng->par_read = neteng->rx_pipefd[0];
	int par_write = neteng->par_write = neteng->tx_pipefd[1];

	if (!neteng_cmd_exec(neteng->child, par_read, par_write, NC_START))
		goto err_out_child;

	neteng->running = true;
	return true;

err_out_child:
	neteng_child_kill(neteng->child);
err_out_txfd:
	close(neteng->tx_pipefd[0]);
	close(neteng->tx_pipefd[1]);
err_out_rxfd:
	close(neteng->rx_pipefd[0]);
	close(neteng->rx_pipefd[1]);
	neteng->rx_pipefd[0] = -1;
	neteng->rx_pipefd[1] = -1;
	neteng->tx_pipefd[0] = -1;
	neteng->tx_pipefd[1] = -1;
	return false;
}

void neteng_stop(struct net_engine *neteng)
{
	if (!neteng->running)
		return;

	if (!neteng_cmd_exec(neteng->child, neteng->par_read,
			     neteng->par_write, NC_STOP))
		kill(neteng->child, SIGTERM);
	sleep(1);
	waitpid(neteng->child, NULL, WNOHANG);

	close(neteng->tx_pipefd[0]);
	close(neteng->tx_pipefd[1]);
	close(neteng->rx_pipefd[0]);
	close(neteng->rx_pipefd[1]);
	neteng->rx_pipefd[0] = -1;
	neteng->rx_pipefd[1] = -1;
	neteng->tx_pipefd[0] = -1;
	neteng->tx_pipefd[1] = -1;

	neteng->running = false;
}

void neteng_free(struct net_engine *neteng)
{
	neteng_stop(neteng);

	memset(neteng, 0, sizeof(*neteng));
	free(neteng);
}

static struct net_engine *neteng_new_start(void)
{
	struct net_engine *neteng;

	neteng = neteng_new();
	if (!neteng) {
		fprintf(stderr, "neteng new fail\n");
		exit(1);
	}

	if (!neteng_start(neteng)) {
		fprintf(stderr, "failed to start engine\n");
		exit(1);
	}

	return neteng;
}

void network_sync(void)
{
	struct net_engine *neteng = neteng_new_start();

	neteng_free(neteng);
}

