/*
 *  gensio - A library for abstracting stream I/O
 *  Copyright (C) 2018  Corey Minyard <minyard@acm.org>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 */

#include "config.h"
#include <errno.h>

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <limits.h>
#include <stdio.h>
#include <assert.h>

#include <arpa/inet.h>
#ifdef HAVE_LIBSCTP
#include <netinet/sctp.h>
#endif

#include <gensio/gensio.h>
#include <gensio/gensio_builtins.h>
#include <gensio/gensio_class.h>

#include "utils.h"

static unsigned int gensio_log_mask =
    (1 << GENSIO_LOG_FATAL) | (1 << GENSIO_LOG_ERR);

struct gensio_classobj {
    const char *name;
    void *classdata;
    struct gensio_classobj *next;
};

static int
gen_addclass(struct gensio_os_funcs *o,
	     struct gensio_classobj **classes,
	     const char *name, void *classdata)
{
    struct gensio_classobj *c;

    c = o->zalloc(o, sizeof(*c));
    if (!c)
	return GE_NOMEM;
    c->name = name;
    c->classdata = classdata;
    c->next = *classes;
    *classes = c;
    return 0;
}

static void *
gen_getclass(struct gensio_classobj *classes, const char *name)
{
    struct gensio_classobj *c;

    for (c = classes; c; c = c->next) {
	if (strcmp(c->name, name) == 0)
	    return c->classdata;
    }
    return NULL;
}

struct gensio_nocbwait {
    bool queued;
    struct gensio_waiter *waiter;
    struct gensio_link link;
};

struct gensio_sync_io;

struct gensio {
    struct gensio_os_funcs *o;
    void *user_data;
    gensio_event cb;
    unsigned int cb_count;
    struct gensio_list waiters;
    struct gensio_lock *lock;

    struct gensio_classobj *classes;

    gensio_func func;
    void *gensio_data;

    const char *typename;

    struct gensio *child;

    bool is_client;
    bool is_packet;
    bool is_reliable;
    bool is_authenticated;
    bool is_encrypted;
    bool is_message;

    struct gensio_sync_io *sync_io;

    struct gensio_link pending_link;
};

struct gensio *
gensio_data_alloc(struct gensio_os_funcs *o,
		  gensio_event cb, void *user_data,
		  gensio_func func, struct gensio *child,
		  const char *typename, void *gensio_data)
{
    struct gensio *io = o->zalloc(o, sizeof(*io));

    if (!io)
	return NULL;

    io->lock = o->alloc_lock(o);
    if (!io->lock) {
	o->free(o, io);
	return NULL;
    }
    gensio_list_init(&io->waiters);
    io->o = o;
    io->cb = cb;
    io->user_data = user_data;
    io->func = func;
    io->typename = typename;
    io->gensio_data = gensio_data;
    io->child = child;

    return io;
}

void
gensio_data_free(struct gensio *io)
{
    assert(gensio_list_empty(&io->waiters));

    while (io->classes) {
	struct gensio_classobj *c = io->classes;

	io->classes = c->next;
	io->o->free(io->o, c);
    }
    io->o->free_lock(io->lock);
    io->o->free(io->o, io);
}

void *
gensio_get_gensio_data(struct gensio *io)
{
    return io->gensio_data;
}

gensio_event
gensio_get_cb(struct gensio *io)
{
    return io->cb;
}

void gensio_set_cb(struct gensio *io, gensio_event cb, void *user_data)
{
    io->cb = cb;
    io->user_data = user_data;
}

int
gensio_cb(struct gensio *io, int event, int err,
	  unsigned char *buf, gensiods *buflen, const char *const *auxdata)
{
    struct gensio_os_funcs *o = io->o;
    int rv;

    if (!io->cb)
	return GE_NOTSUP;
    o->lock(io->lock);
    io->cb_count++;
    o->unlock(io->lock);
    rv = io->cb(io, io->user_data, event, err, buf, buflen, auxdata);
    o->lock(io->lock);
    assert(io->cb_count > 0);
    io->cb_count--;
    if (io->cb_count == 0) {
	struct gensio_link *l, *l2;

	gensio_list_for_each_safe(&io->waiters, l, l2) {
	    struct gensio_nocbwait *w = gensio_container_of(l,
							struct gensio_nocbwait,
							link);

	    gensio_list_rm(&io->waiters, l);
	    w->queued = false;
	    o->wake(w->waiter);
	}
    }
    o->unlock(io->lock);

    return rv;
}

int
gensio_addclass(struct gensio *io, const char *name, void *classdata)
{
    return gen_addclass(io->o, &io->classes, name, classdata);
}

void *
gensio_getclass(struct gensio *io, const char *name)
{
    return gen_getclass(io->classes, name);
}

struct gensio_accepter {
    struct gensio_os_funcs *o;

    void *user_data;
    gensio_accepter_event cb;

    struct gensio_classobj *classes;

    const struct gensio_accepter_functions *funcs;
    gensio_acc_func func;
    void *gensio_acc_data;

    const char *typename;

    struct gensio_accepter *child;

    bool is_packet;
    bool is_reliable;
    bool is_message;

    struct gensio_list pending_ios;
};

struct gensio_accepter *
gensio_acc_data_alloc(struct gensio_os_funcs *o,
		      gensio_accepter_event cb, void *user_data,
		      gensio_acc_func func, struct gensio_accepter *child,
		      const char *typename, void *gensio_acc_data)
{
    struct gensio_accepter *acc = o->zalloc(o, sizeof(*acc));

    if (!acc)
	return NULL;

    acc->o = o;
    acc->cb = cb;
    acc->user_data = user_data;
    acc->func = func;
    acc->typename = typename;
    acc->child = child;
    acc->gensio_acc_data = gensio_acc_data;
    gensio_list_init(&acc->pending_ios);

    return acc;
}

void
gensio_acc_data_free(struct gensio_accepter *acc)
{
    while (acc->classes) {
	struct gensio_classobj *c = acc->classes;

	acc->classes = c->next;
	acc->o->free(acc->o, c);
    }
    acc->o->free(acc->o, acc);
}

void *
gensio_acc_get_gensio_data(struct gensio_accepter *acc)
{
    return acc->gensio_acc_data;
}

int
gensio_acc_cb(struct gensio_accepter *acc, int event, void *data)
{
    return acc->cb(acc, acc->user_data, event, data);
}

int
gensio_acc_addclass(struct gensio_accepter *acc,
		    const char *name, void *classdata)
{
    return gen_addclass(acc->o, &acc->classes, name, classdata);
}

void *
gensio_acc_getclass(struct gensio_accepter *acc, const char *name)
{
    return gen_getclass(acc->classes, name);
}

const char *
gensio_acc_get_type(struct gensio_accepter *acc, unsigned int depth)
{
    struct gensio_accepter *c = acc;

    while (depth > 0) {
	if (!c->child)
	    return NULL;
	depth--;
	c = c->child;
    }
    return c->typename;
}

void
gensio_acc_add_pending_gensio(struct gensio_accepter *acc,
			      struct gensio *io)
{
    gensio_list_add_tail(&acc->pending_ios, &io->pending_link);
}

void
gensio_acc_remove_pending_gensio(struct gensio_accepter *acc,
				 struct gensio *io)
{
    gensio_list_rm(&acc->pending_ios, &io->pending_link);
}

int
gensio_scan_args(struct gensio_os_funcs *o,
		 const char **rstr, int *argc, const char ***args)
{
    const char *str = *rstr;
    int err = 0;

    if (*str == '(') {
	err = gensio_str_to_argv_endchar(o, str + 1, argc, args,
					 " \f\n\r\t\v,", ")", &str);
	if (!err && (!str || (*str != ',' && *str)))
	    err = GE_INVAL; /* Not a ',' or end of string after */
	else
	    str++;
    } else {
	if (*str)
	    str += 1; /* skip the comma */
	err = gensio_str_to_argv(o, "", argc, args, ")");
    }

    if (!err)
	*rstr = str;

    return err;
}

static int
strisallzero(const char *str)
{
    if (*str == '\0')
	return 0;

    while (*str == '0')
	str++;
    return *str == '\0';
}

static int
scan_ips(struct gensio_os_funcs *o, const char *str, bool listen, int ifamily,
	 int socktype, int protocol, bool *is_port_set, struct addrinfo **rai)
{
    char *strtok_data, *strtok_buffer;
    struct addrinfo hints, *ai = NULL, *ai2 = NULL, *ai3, *ai4;
    char *ip;
    char *port;
    int portnum;
    bool first = true, portset = false;
    int rv = 0;
    int bflags = AI_ADDRCONFIG;

    if (listen)
	bflags |= AI_PASSIVE;

    strtok_buffer = gensio_strdup(o, str);
    if (!strtok_buffer)
	return GE_NOMEM;

    ip = strtok_r(strtok_buffer, ",", &strtok_data);
    while (ip) {
	int family = ifamily, rflags = 0;

	if (strcmp(ip, "ipv4") == 0) {
	    family = AF_INET;
	    ip = strtok_r(NULL, ",", &strtok_data);
	} else if (strcmp(ip, "ipv6") == 0) {
	    family = AF_INET6;
	    ip = strtok_r(NULL, ",", &strtok_data);
	} else if (strcmp(ip, "ipv6n4") == 0) {
	    family = AF_INET6;
	    rflags |= AI_V4MAPPED;
	    ip = strtok_r(NULL, ",", &strtok_data);
	}

	if (ip == NULL) {
	    rv = GE_INVAL;
	    goto out_err;
	}

	port = strtok_r(NULL, ",", &strtok_data);
	if (port == NULL) {
	    port = ip;
	    ip = NULL;
	}

	if (ip && *ip == '\0')
	    ip = NULL;

	memset(&hints, 0, sizeof(hints));
	hints.ai_flags = bflags | rflags;
	hints.ai_family = family;
	hints.ai_socktype = socktype;
	hints.ai_protocol = protocol;
	if (getaddrinfo(ip, port, &hints, &ai)) {
	    rv = GE_INVAL;
	    goto out_err;
	}

	/*
	 * If a port was/was not set, this must be consistent for all
	 * addresses.
	 */
	portnum = gensio_sockaddr_get_port(ai->ai_addr);
	if (portnum == -1) {
	    /* Not AF_INET or AF_INET6. */
	    rv = GE_INVAL;
	    goto out_err;
	}
	if (first) {
	    portset = portnum != 0;
	} else {
	    if ((portnum != 0) != portset) {
		/* One port was set and the other wasn't. */
		rv = GE_INCONSISTENT;
		goto out_err;
	    }
	}

	ai3 = gensio_dup_addrinfo(o, ai);
	if (!ai3) {
	    rv = GE_NOMEM;
	    goto out_err;
	}

	for (ai4 = ai3; ai4; ai4 = ai4->ai_next)
	    ai4->ai_flags = rflags;

	if (ai2)
	    ai2 = gensio_cat_addrinfo(o, ai2, ai3);
	else
	    ai2 = ai3;
	ip = strtok_r(NULL, ",", &strtok_data);
	first = false;
    }

    if (!ai2) {
	rv = GE_NOTFOUND;
	goto out_err;
    }

    if (is_port_set)
	*is_port_set = portset;

    *rai = ai2;

 out_err:
    if (ai)
	freeaddrinfo(ai);
    o->free(o, strtok_buffer);
    if (rv && ai2)
	gensio_free_addrinfo(o, ai2);

    return rv;
}

int
gensio_scan_network_port(struct gensio_os_funcs *o, const char *str,
			 bool listen, struct addrinfo **rai,
			 int *socktype, int *protocol,
			 bool *is_port_set,
			 int *rargc, const char ***rargs)
{
    int err = 0, family = AF_UNSPEC, argc = 0;
    const char **args = NULL;
    bool doskip = true;

    if (strncmp(str, "ipv4,", 5) == 0) {
	family = AF_INET;
	str += 5;
    } else if (strncmp(str, "ipv6,", 5) == 0) {
	family = AF_INET6;
	str += 5;
    }

    if (strncmp(str, "tcp,", 4) == 0 ||
		(rargs && strncmp(str, "tcp(", 4) == 0)) {
	str += 3;
	*socktype = SOCK_STREAM;
	*protocol = IPPROTO_TCP;
    } else if (strncmp(str, "udp,", 4) == 0 ||
	       (rargs && strncmp(str, "udp(", 4) == 0)) {
	str += 3;
	*socktype = SOCK_DGRAM;
	*protocol = IPPROTO_UDP;
    } else if (strncmp(str, "sctp,", 5) == 0 ||
	       (rargs && strncmp(str, "sctp(", 5) == 0)) {
	str += 4;
	*socktype = SOCK_SEQPACKET;
	*protocol = IPPROTO_SCTP;
    } else {
	doskip = false;
	*socktype = SOCK_STREAM;
	*protocol = IPPROTO_TCP;
    }

    if (doskip) {
	if (*str == '(') {
	    if (!rargs)
		return GE_INVAL;
	    err = gensio_scan_args(o, &str, &argc, &args);
	    if (err)
		return err;
	} else {
	    str++; /* Skip the ',' */
	}
    }

    err = scan_ips(o, str, listen, family, *socktype, *protocol,
		   is_port_set, rai);
    if (err) {
	if (args)
	    gensio_argv_free(o, args);
	return err;
    }

    if (rargc)
	*rargc = argc;
    if (rargs)
	*rargs = args;

    return 0;
}

int
gensio_scan_netaddr(struct gensio_os_funcs *o, const char *str, bool listen,
		    int socktype, int protocol, struct addrinfo **rai)
{
    int family = AF_UNSPEC;

    if (strncmp(str, "ipv4,", 5) == 0) {
	family = AF_INET;
	str += 5;
    } else if (strncmp(str, "ipv6,", 5) == 0) {
	family = AF_INET6;
	str += 5;
    }

    return scan_ips(o, str, listen, family, socktype, protocol, NULL, rai);
}

bool
gensio_sockaddr_equal(const struct sockaddr *a1, socklen_t l1,
		      const struct sockaddr *a2, socklen_t l2,
		      bool compare_ports)
{
    if (l1 != l2)
	return false;
    if (a1->sa_family != a2->sa_family)
	return false;
    switch (a1->sa_family) {
    case AF_INET:
	{
	    struct sockaddr_in *s1 = (struct sockaddr_in *) a1;
	    struct sockaddr_in *s2 = (struct sockaddr_in *) a2;
	    if (compare_ports && s1->sin_port != s2->sin_port)
		return false;
	    if (s1->sin_addr.s_addr != s2->sin_addr.s_addr)
		return false;
	}
	break;

    case AF_INET6:
	{
	    struct sockaddr_in6 *s1 = (struct sockaddr_in6 *) a1;
	    struct sockaddr_in6 *s2 = (struct sockaddr_in6 *) a2;
	    if (compare_ports && s1->sin6_port != s2->sin6_port)
		return false;
	    if (memcmp(s1->sin6_addr.s6_addr, s2->sin6_addr.s6_addr,
		       sizeof(s1->sin6_addr.s6_addr)) != 0)
		return false;
	}
	break;

    default:
	/* Unknown family. */
	return false;
    }

    return true;
}

int
gensio_sockaddr_get_port(const struct sockaddr *s)
{
    switch (s->sa_family) {
    case AF_INET:
	return ntohs(((struct sockaddr_in *) s)->sin_port);

    case AF_INET6:
	return ntohs(((struct sockaddr_in6 *) s)->sin6_port);
    }
    return -1;
}

void
gensio_set_callback(struct gensio *io, gensio_event cb, void *user_data)
{
    io->cb = cb;
    io->user_data = user_data;
}

void *
gensio_get_user_data(struct gensio *io)
{
    return io->user_data;
}

void
gensio_set_user_data(struct gensio *io, void *user_data)
{
    io->user_data = user_data;
}

int
gensio_write(struct gensio *io, gensiods *count,
	     const void *buf, gensiods buflen,
	     const char *const *auxdata)
{
    struct gensio_sg sg;

    if (buflen == 0) {
	*count = 0;
	return 0;
    }
    sg.buf = buf;
    sg.buflen = buflen;
    return io->func(io, GENSIO_FUNC_WRITE_SG, count, &sg, 1, NULL, auxdata);
}

int
gensio_write_sg(struct gensio *io, gensiods *count,
		const struct gensio_sg *sg, gensiods sglen,
		const char *const *auxdata)
{
    if (sglen == 0) {
	*count = 0;
	return 0;
    }
    return io->func(io, GENSIO_FUNC_WRITE_SG, count, sg, sglen, NULL, auxdata);
}

int
gensio_raddr_to_str(struct gensio *io, gensiods *pos,
		    char *buf, gensiods buflen)
{
    return io->func(io, GENSIO_FUNC_RADDR_TO_STR, pos, NULL, buflen, buf, NULL);
}

int
gensio_get_raddr(struct gensio *io, void *addr, gensiods *addrlen)
{
    return io->func(io, GENSIO_FUNC_GET_RADDR, addrlen, NULL, 0, addr, NULL);
}

int
gensio_remote_id(struct gensio *io, int *id)
{
    return io->func(io, GENSIO_FUNC_REMOTE_ID, NULL, NULL, 0, id, NULL);
}

int
gensio_open(struct gensio *io, gensio_done_err open_done, void *open_data)
{
    return io->func(io, GENSIO_FUNC_OPEN, NULL, open_done, 0, open_data, NULL);
}

int
gensio_open_nochild(struct gensio *io, gensio_done_err open_done,
		    void *open_data)
{
    return io->func(io, GENSIO_FUNC_OPEN_NOCHILD, NULL, open_done, 0,
		    open_data, NULL);
}

struct gensio_open_s_data {
    struct gensio_os_funcs *o;
    int err;
    struct gensio_waiter *waiter;
};

static void
gensio_open_s_done(struct gensio *io, int err, void *cb_data)
{
    struct gensio_open_s_data *data = cb_data;

    data->err = err;
    data->o->wake(data->waiter);
}

static int
i_gensio_open_s(struct gensio *io,
		int (*func)(struct gensio *io, gensio_done_err open_done,
			    void *open_data))
{
    struct gensio_os_funcs *o = io->o;
    struct gensio_open_s_data data;
    int err;

    data.o = o;
    data.err = 0;
    data.waiter = o->alloc_waiter(o);
    if (!data.waiter)
	return GE_NOMEM;
    err = func(io, gensio_open_s_done, &data);
    if (!err) {
	o->wait(data.waiter, 1, NULL);
	err = data.err;
    }
    o->free_waiter(data.waiter);
    return err;
}

int
gensio_open_s(struct gensio *io)
{
    return i_gensio_open_s(io, gensio_open);
}

int
gensio_open_nochild_s(struct gensio *io)
{
    return i_gensio_open_s(io, gensio_open_nochild);
}

int
gensio_open_channel(struct gensio *io, const char * const args[],
		    gensio_event cb, void *user_data,
		    gensio_done_err open_done, void *open_data,
		    struct gensio **new_io)
{
    int rv;
    struct gensio_func_open_channel_data d;

    d.args = args;
    d.cb = cb;
    d.user_data = user_data;
    d.open_done = open_done;
    d.open_data = open_data;
    rv = io->func(io, GENSIO_FUNC_OPEN_CHANNEL, NULL, NULL, 0, &d, NULL);
    if (!rv)
	*new_io = d.new_io;

    return rv;
}

int
gensio_open_channel_s(struct gensio *io, const char * const args[],
		      gensio_event cb, void *user_data,
		      struct gensio **new_io)
{
    struct gensio_os_funcs *o = io->o;
    struct gensio_open_s_data data;
    int err;

    data.o = o;
    data.err = 0;
    data.waiter = o->alloc_waiter(o);
    if (!data.waiter)
	return GE_NOMEM;
    err = gensio_open_channel(io, args, cb, user_data,
			      gensio_open_s_done, &data, new_io);
    if (!err) {
	o->wait(data.waiter, 1, NULL);
	err = data.err;
    }
    o->free_waiter(data.waiter);
    return err;
}

int
gensio_control(struct gensio *io, int depth, bool get,
	       unsigned int option, char *data, gensiods *datalen)
{
    struct gensio *c = io;

    if (depth == GENSIO_CONTROL_DEPTH_ALL) {
	if (get)
	    return GE_INVAL;
	while (c) {
	    int rv = c->func(c, GENSIO_FUNC_CONTROL, datalen, &get, option,
			     data, NULL);

	    if (rv && rv != GE_NOTSUP)
		return rv;
	    c = c->child;
	}
	return 0;
    }

    if (depth == GENSIO_CONTROL_DEPTH_FIRST) {
	while (c) {
	    int rv = c->func(c, GENSIO_FUNC_CONTROL, datalen, &get, option,
			     data, NULL);

	    if (rv != GE_NOTSUP)
		return rv;
	    c = c->child;
	}
	return GE_NOTSUP;
    }

    if (depth < 0)
	return GE_INVAL;

    while (depth > 0) {
	if (!c->child)
	    return GE_NOTFOUND;
	depth--;
	c = c->child;
    }

    return c->func(c, GENSIO_FUNC_CONTROL, datalen, &get, option, data, NULL);
}

const char *
gensio_get_type(struct gensio *io, unsigned int depth)
{
    struct gensio *c = io;

    while (depth > 0) {
	if (!c->child)
	    return NULL;
	depth--;
	c = c->child;
    }
    return c->typename;
}

struct gensio *
gensio_get_child(struct gensio *io, unsigned int depth)
{
    struct gensio *c = io;

    while (depth > 0) {
	if (!c->child)
	    return NULL;
	depth--;
	c = c->child;
    }
    return c;
}

int
gensio_close(struct gensio *io, gensio_done close_done, void *close_data)
{
    return io->func(io, GENSIO_FUNC_CLOSE, NULL, close_done, 0, close_data,
		    NULL);
}

struct gensio_close_s_data {
    struct gensio_os_funcs *o;
    struct gensio_waiter *waiter;
};

static void
gensio_close_s_done(struct gensio *io, void *cb_data)
{
    struct gensio_close_s_data *data = cb_data;

    data->o->wake(data->waiter);
}

int
gensio_close_s(struct gensio *io)
{
    struct gensio_os_funcs *o = io->o;
    struct gensio_close_s_data data;
    int err;

    data.o = o;
    data.waiter = o->alloc_waiter(o);
    if (!data.waiter)
	return GE_NOMEM;
    err = gensio_close(io, gensio_close_s_done, &data);
    if (!err)
	o->wait(data.waiter, 1, NULL);
    o->free_waiter(data.waiter);
    return err;
}

void
gensio_disable(struct gensio *io)
{
    struct gensio *c = io;

    while (c) {
	io->func(c, GENSIO_FUNC_DISABLE, NULL, NULL, 0, NULL, NULL);
	c = c->child;
    }
}

void
gensio_free(struct gensio *io)
{
    io->func(io, GENSIO_FUNC_FREE, NULL, NULL, 0, NULL, NULL);
}

void
gensio_set_read_callback_enable(struct gensio *io, bool enabled)
{
    io->func(io, GENSIO_FUNC_SET_READ_CALLBACK, NULL, NULL, enabled, NULL,
	     NULL);
}

void
gensio_set_write_callback_enable(struct gensio *io, bool enabled)
{
    io->func(io, GENSIO_FUNC_SET_WRITE_CALLBACK, NULL, NULL, enabled, NULL,
	     NULL);
}

void
gensio_ref(struct gensio *io)
{
    io->func(io, GENSIO_FUNC_REF, NULL, NULL, 0, NULL, NULL);
}

bool
gensio_is_client(struct gensio *io)
{
    return io->is_client;
}

bool
gensio_is_reliable(struct gensio *io)
{
    return io->is_reliable;
}

bool
gensio_is_packet(struct gensio *io)
{
    return io->is_packet;
}

bool
gensio_is_message(struct gensio *io)
{
    return io->is_message;
}

bool
gensio_is_authenticated(struct gensio *io)
{
    return io->is_authenticated;
}

bool
gensio_is_encrypted(struct gensio *io)
{
    return io->is_encrypted;
}

void
gensio_set_is_client(struct gensio *io, bool is_client)
{
    io->is_client = is_client;
}

void
gensio_set_is_reliable(struct gensio *io, bool is_reliable)
{
    io->is_reliable = is_reliable;
}

void
gensio_set_is_packet(struct gensio *io, bool is_packet)
{
    io->is_packet = is_packet;
}

void
gensio_set_is_message(struct gensio *io, bool is_message)
{
    io->is_message = is_message;
}

void
gensio_set_is_authenticated(struct gensio *io, bool is_authenticated)
{
    io->is_authenticated = is_authenticated;
}

void
gensio_set_is_encrypted(struct gensio *io, bool is_encrypted)
{
    io->is_encrypted = is_encrypted;
}

void *
gensio_acc_get_user_data(struct gensio_accepter *accepter)
{
    return accepter->user_data;
}

void
gensio_acc_set_user_data(struct gensio_accepter *accepter,
			 void *user_data)
{
    accepter->user_data = user_data;
}

void
gensio_acc_set_callback(struct gensio_accepter *accepter,
			gensio_accepter_event cb,
			void *user_data)
{
    accepter->cb = cb;
    accepter->user_data = user_data;
}

int
gensio_acc_startup(struct gensio_accepter *accepter)
{
    return accepter->func(accepter, GENSIO_ACC_FUNC_STARTUP, 0,
			  NULL, NULL, NULL, NULL, NULL);
}

int
gensio_acc_shutdown(struct gensio_accepter *accepter,
		    gensio_acc_done shutdown_done, void *shutdown_data)
{
    return accepter->func(accepter, GENSIO_ACC_FUNC_SHUTDOWN, 0,
			  0, shutdown_done, shutdown_data, NULL, NULL);
}

static void
gensio_acc_shutdown_s_done(struct gensio_accepter *acc, void *cb_data)
{
    struct gensio_close_s_data *data = cb_data;

    data->o->wake(data->waiter);
}

int
gensio_acc_shutdown_s(struct gensio_accepter *acc)
{
    struct gensio_os_funcs *o = acc->o;
    struct gensio_close_s_data data;
    int err;

    data.o = o;
    data.waiter = o->alloc_waiter(o);
    if (!data.waiter)
	return GE_NOMEM;
    err = gensio_acc_shutdown(acc, gensio_acc_shutdown_s_done, &data);
    if (!err)
	o->wait(data.waiter, 1, NULL);
    o->free_waiter(data.waiter);
    return err;
}

void
gensio_acc_disable(struct gensio_accepter *acc)
{
    struct gensio_accepter *c = acc;

    while (c) {
	struct gensio_link *l, *l2;

	gensio_list_for_each_safe(&acc->pending_ios, l, l2) {
	    struct gensio *io = gensio_container_of(l, struct gensio,
						    pending_link);
	    gensio_acc_remove_pending_gensio(acc, io);
	    gensio_disable(io);
	    gensio_free(io);
	}
	c->func(c, GENSIO_ACC_FUNC_DISABLE, 0, NULL, NULL, NULL, NULL, NULL);
	c = c->child;
    }
}

int
gensio_acc_control(struct gensio_accepter *acc, int depth, bool get,
		   unsigned int option, char *data, gensiods *datalen)
{
    struct gensio_accepter *c = acc;

    if (depth == GENSIO_CONTROL_DEPTH_ALL) {
	if (get)
	    return GE_INVAL;
	while (c) {
	    int rv = c->func(c, GENSIO_ACC_FUNC_CONTROL, get, NULL, NULL,
			     data, NULL, datalen);

	    if (rv && rv != GE_NOTSUP)
		return rv;
	    c = c->child;
	}
	return 0;
    }

    if (depth == GENSIO_CONTROL_DEPTH_FIRST) {
	while (c) {
	    int rv = c->func(c, GENSIO_ACC_FUNC_CONTROL, get, NULL, NULL,
			     data, NULL, datalen);

	    if (rv != GE_NOTSUP)
		return rv;
	    c = c->child;
	}
	return GE_NOTSUP;
    }

    if (depth < 0)
	return GE_INVAL;

    while (depth > 0) {
	if (!c->child)
	    return GE_NOTFOUND;
	depth--;
	c = c->child;
    }

    return c->func(c, GENSIO_ACC_FUNC_CONTROL, get, NULL, NULL,
		   data, NULL, datalen);
}

void
gensio_acc_set_accept_callback_enable(struct gensio_accepter *accepter,
				      bool enabled)
{
    accepter->func(accepter, GENSIO_ACC_FUNC_SET_ACCEPT_CALLBACK, enabled,
		   NULL, NULL, NULL, NULL, NULL);
}

int
gensio_acc_set_accept_callback_enable_cb(struct gensio_accepter *accepter,
					 bool enabled,
					 gensio_acc_done done,
					 void *done_data)
{
    return accepter->func(accepter, GENSIO_ACC_FUNC_SET_ACCEPT_CALLBACK,
			  enabled, NULL, done, done_data, NULL, NULL);
}

struct acc_cb_enable_data {
    struct gensio_os_funcs *o;
    struct gensio_waiter *waiter;
};

static void
acc_cb_enable_done(struct gensio_accepter *acc, void *done_data)
{
    struct acc_cb_enable_data *data = done_data;

    data->o->wake(data->waiter);
}

int
gensio_acc_set_accept_callback_enable_s(struct gensio_accepter *accepter,
					bool enabled)
{
    struct acc_cb_enable_data data;
    int err;

    data.o = accepter->o;
    data.waiter = data.o->alloc_waiter(data.o);
    if (!data.waiter)
	return GE_NOMEM;
    err = gensio_acc_set_accept_callback_enable_cb(accepter, enabled,
						   acc_cb_enable_done, &data);
    if (err) {
	data.o->free_waiter(data.waiter);
	return err;
    }
    data.o->wait(data.waiter, 1, NULL);
    data.o->free_waiter(data.waiter);

    return 0;
}

void
gensio_acc_free(struct gensio_accepter *accepter)
{
    accepter->func(accepter, GENSIO_ACC_FUNC_FREE, 0, NULL, NULL, NULL, NULL,
		   NULL);
}

int
gensio_acc_str_to_gensio(struct gensio_accepter *accepter, const char *addr,
			 gensio_event cb, void *user_data,
			 struct gensio **new_io)
{
    return accepter->func(accepter, GENSIO_ACC_FUNC_STR_TO_GENSIO, 0,
			  addr, cb, user_data, NULL, new_io);
}

/* FIXME - this is a cheap hack and needs to be fixed. */
bool
gensio_acc_exit_on_close(struct gensio_accepter *accepter)
{
    return strcmp(accepter->typename, "stdio") == 0;
}

bool
gensio_acc_is_reliable(struct gensio_accepter *accepter)
{
    return accepter->is_reliable;
}

bool
gensio_acc_is_packet(struct gensio_accepter *accepter)
{
    return accepter->is_packet;
}

bool
gensio_acc_is_message(struct gensio_accepter *accepter)
{
    return accepter->is_message;
}

void
gensio_acc_set_is_reliable(struct gensio_accepter *accepter, bool is_reliable)
{
     accepter->is_reliable = is_reliable;
}

void
gensio_acc_set_is_packet(struct gensio_accepter *accepter, bool is_packet)
{
    accepter->is_packet = is_packet;
}

void
gensio_acc_set_is_message(struct gensio_accepter *accepter, bool is_message)
{
    accepter->is_message = is_message;
}

struct registered_gensio_accepter {
    const char *name;
    str_to_gensio_acc_handler handler;
    str_to_gensio_acc_child_handler chandler;
    struct registered_gensio_accepter *next;
};

struct registered_gensio_accepter *reg_gensio_accs;
struct gensio_lock *reg_gensio_acc_lock;


struct gensio_once gensio_acc_str_initialized;

static void
add_default_gensio_accepters(void *cb_data)
{
    struct gensio_os_funcs *o = cb_data;

    reg_gensio_acc_lock = o->alloc_lock(o);
    register_gensio_accepter(o, "tcp", str_to_tcp_gensio_accepter);
    register_gensio_accepter(o, "udp", str_to_udp_gensio_accepter);
    register_gensio_accepter(o, "sctp", str_to_sctp_gensio_accepter);
    register_gensio_accepter(o, "stdio", str_to_stdio_gensio_accepter);
    register_filter_gensio_accepter(o, "ssl", str_to_ssl_gensio_accepter,
				    ssl_gensio_accepter_alloc);
    register_filter_gensio_accepter(o, "certauth",
				    str_to_certauth_gensio_accepter,
				    certauth_gensio_accepter_alloc);
    register_filter_gensio_accepter(o, "telnet", str_to_telnet_gensio_accepter,
				    telnet_gensio_accepter_alloc);
    register_gensio_accepter(o, "dummy", str_to_dummy_gensio_accepter);
}

int
register_filter_gensio_accepter(struct gensio_os_funcs *o,
				const char *name,
				str_to_gensio_acc_handler handler,
				str_to_gensio_acc_child_handler chandler)
{
    struct registered_gensio_accepter *n;

    o->call_once(o, &gensio_acc_str_initialized,
		 add_default_gensio_accepters, o);

    n = o->zalloc(o, sizeof(*n));
    if (!n)
	return GE_NOMEM;

    n->name = name;
    n->handler = handler;
    n->chandler = chandler;
    o->lock(reg_gensio_acc_lock);
    n->next = reg_gensio_accs;
    reg_gensio_accs = n;
    o->unlock(reg_gensio_acc_lock);
    return 0;
}

int
register_gensio_accepter(struct gensio_os_funcs *o,
			 const char *name,
			 str_to_gensio_acc_handler handler)
{
    return register_filter_gensio_accepter(o, name, handler, NULL);
}

int
str_to_gensio_accepter(const char *str,
		       struct gensio_os_funcs *o,
		       gensio_accepter_event cb, void *user_data,
		       struct gensio_accepter **accepter)
{
    int err;
    struct addrinfo *ai = NULL;
    bool is_port_set;
    int socktype, protocol;
    const char **args = NULL;
    struct registered_gensio_accepter *r;
    unsigned int len;

    o->call_once(o, &gensio_acc_str_initialized,
		 add_default_gensio_accepters, o);

    while (isspace(*str))
	str++;
    for (r = reg_gensio_accs; r; r = r->next) {
	len = strlen(r->name);
	if (strncmp(r->name, str, len) != 0 ||
			(str[len] != ',' && str[len] != '(' && str[len]))
	    continue;

	str += len;
	err = gensio_scan_args(o, &str, NULL, &args);
	if (!err)
	    err = r->handler(str, args, o, cb, user_data, accepter);
	if (args)
	    gensio_argv_free(o, args);
	return err;
    }

    if (strisallzero(str)) {
	err = stdio_gensio_accepter_alloc(NULL, o, cb, user_data,
					  accepter);
    } else {
	err = gensio_scan_network_port(o, str, true, &ai, &socktype, &protocol,
				       &is_port_set, NULL, &args);
	if (!err) {
	    if (!is_port_set) {
		err = GE_INVAL;
	    } else if (protocol == IPPROTO_UDP) {
		err = udp_gensio_accepter_alloc(ai, args, o, cb,
						user_data, accepter);
	    } else if (protocol == IPPROTO_TCP) {
		err = tcp_gensio_accepter_alloc(ai, args, o, cb,
						user_data, accepter);
	    } else if (protocol == IPPROTO_SCTP) {
		err = sctp_gensio_accepter_alloc(ai, args, o, cb,
						 user_data, accepter);
	    } else {
		err = GE_INVAL;
	    }

	    gensio_free_addrinfo(o, ai);
	}
    }

    if (args)
	gensio_argv_free(o, args);

    return err;
}

int
str_to_gensio_accepter_child(struct gensio_accepter *child,
			     const char *str,
			     struct gensio_os_funcs *o,
			     gensio_accepter_event cb, void *user_data,
			     struct gensio_accepter **accepter)
{
    int err = GE_INVAL;
    struct registered_gensio_accepter *r;
    unsigned int len;

    o->call_once(o, &gensio_acc_str_initialized,
		 add_default_gensio_accepters, o);

    while (isspace(*str))
	str++;
    for (r = reg_gensio_accs; r; r = r->next) {
	const char **args = NULL;

	len = strlen(r->name);
	if (strncmp(r->name, str, len) != 0 ||
			(str[len] != ',' && str[len] != '(' && str[len]))
	    continue;

	str += len;
	err = gensio_scan_args(o, &str, NULL, &args);
	if (!err)
	    err = r->chandler(child, args, o, cb, user_data, accepter);
	if (args)
	    gensio_argv_free(o, args);
	return err;
    }

    return err;
}

struct registered_gensio {
    const char *name;
    str_to_gensio_handler handler;
    str_to_gensio_child_handler chandler;
    struct registered_gensio *next;
};

struct registered_gensio *reg_gensios;
struct gensio_lock *reg_gensio_lock;


struct gensio_once gensio_str_initialized;

static void
add_default_gensios(void *cb_data)
{
    struct gensio_os_funcs *o = cb_data;

    reg_gensio_lock = o->alloc_lock(o);
    register_gensio(o, "tcp", str_to_tcp_gensio);
    register_gensio(o, "udp", str_to_udp_gensio);
    register_gensio(o, "sctp", str_to_sctp_gensio);
    register_gensio(o, "stdio", str_to_stdio_gensio);
    register_gensio(o, "pty", str_to_pty_gensio);
    register_filter_gensio(o, "ssl", str_to_ssl_gensio, ssl_gensio_alloc);
    register_filter_gensio(o, "certauth", str_to_certauth_gensio,
			   certauth_gensio_alloc);
    register_filter_gensio(o, "telnet", str_to_telnet_gensio,
			   telnet_gensio_alloc);
    register_gensio(o, "serialdev", str_to_serialdev_gensio);
    register_gensio(o, "echo", str_to_echo_gensio);
#ifdef HAVE_OPENIPMI
    register_gensio(o, "ipmisol", str_to_ipmisol_gensio);
#endif
}

int
register_filter_gensio(struct gensio_os_funcs *o,
		       const char *name, str_to_gensio_handler handler,
		       str_to_gensio_child_handler chandler)
{
    struct registered_gensio *n;

    o->call_once(o, &gensio_str_initialized, add_default_gensios, o);

    n = o->zalloc(o, sizeof(*n));
    if (!n)
	return GE_NOMEM;

    n->name = name;
    n->handler = handler;
    n->chandler = chandler;
    o->lock(reg_gensio_lock);
    n->next = reg_gensios;
    reg_gensios = n;
    o->unlock(reg_gensio_lock);
    return 0;
}

int
register_gensio(struct gensio_os_funcs *o,
		const char *name, str_to_gensio_handler handler)
{
    return register_filter_gensio(o, name, handler, NULL);
}

int
str_to_gensio(const char *str,
	      struct gensio_os_funcs *o,
	      gensio_event cb, void *user_data,
	      struct gensio **gensio)
{
    int err = 0;
    struct addrinfo *ai = NULL;
    bool is_port_set;
    int socktype, protocol;
    const char **args = NULL;
    struct registered_gensio *r;
    unsigned int len;

    o->call_once(o, &gensio_str_initialized, add_default_gensios, o);

    while (isspace(*str))
	str++;
    for (r = reg_gensios; r; r = r->next) {
	len = strlen(r->name);
	if (strncmp(r->name, str, len) != 0 ||
			(str[len] != ',' && str[len] != '(' && str[len]))
	    continue;

	str += len;
	err = gensio_scan_args(o, &str, NULL, &args);
	if (!err)
	    err = r->handler(str, args, o, cb, user_data, gensio);
	if (args)
	    gensio_argv_free(o, args);
	return err;
    }

    if (*str == '/') {
	err = str_to_serialdev_gensio(str, NULL, o, cb, user_data,
				      gensio);
	goto out;
    }

    err = gensio_scan_network_port(o, str, false, &ai, &socktype, &protocol,
				   &is_port_set, NULL, &args);
    if (!err) {
	if (!is_port_set) {
	    err = GE_INVAL;
	} else if (protocol == IPPROTO_UDP) {
	    err = udp_gensio_alloc(ai, args, o, cb, user_data, gensio);
	} else if (protocol == IPPROTO_TCP) {
	    err = tcp_gensio_alloc(ai, args, o, cb, user_data, gensio);
	} else if (protocol == IPPROTO_SCTP) {
	    err = sctp_gensio_alloc(ai, args, o, cb, user_data, gensio);
	} else {
	    err = GE_INVAL;
	}

	gensio_free_addrinfo(o, ai);
    }

 out:
    if (args)
	gensio_argv_free(o, args);

    return err;
}

int
str_to_gensio_child(struct gensio *child,
		    const char *str,
		    struct gensio_os_funcs *o,
		    gensio_event cb, void *user_data,
		    struct gensio **gensio)
{
    int err = 0;
    const char **args = NULL;
    struct registered_gensio *r;
    unsigned int len;

    while (isspace(*str))
	str++;
    for (r = reg_gensios; r; r = r->next) {
	len = strlen(r->name);
	if (strncmp(r->name, str, len) != 0 ||
			(str[len] != '(' && str[len]))
	    continue;

	if (!r->chandler)
	    return GE_INVAL;

	str += len;
	err = gensio_scan_args(o, &str, NULL, &args);
	if (!err)
	    err = r->chandler(child, args, o, cb, user_data, gensio);
	if (args)
	    gensio_argv_free(o, args);
	return err;
    }

    return GE_INVAL;
}

struct addrinfo *
gensio_dup_addrinfo(struct gensio_os_funcs *o, struct addrinfo *iai)
{
    struct addrinfo *ai = NULL, *aic, *aip = NULL;

    while (iai) {
	aic = o->zalloc(o, sizeof(*aic));
	if (!aic)
	    goto out_nomem;
	memcpy(aic, iai, sizeof(*aic));
	aic->ai_next = NULL;
	aic->ai_addr = o->zalloc(o, iai->ai_addrlen);
	if (!aic->ai_addr) {
	    o->free(o, aic);
	    goto out_nomem;
	}
	memcpy(aic->ai_addr, iai->ai_addr, iai->ai_addrlen);
	if (iai->ai_canonname) {
	    aic->ai_canonname = gensio_strdup(o, iai->ai_canonname);
	    if (!aic->ai_canonname) {
		o->free(o, aic->ai_addr);
		o->free(o, aic);
		goto out_nomem;
	    }
	}
	if (aip) {
	    aip->ai_next = aic;
	    aip = aic;
	} else {
	    ai = aic;
	    aip = aic;
	}
	iai = iai->ai_next;
    }

    return ai;

 out_nomem:
    gensio_free_addrinfo(o, ai);
    return NULL;
}

struct addrinfo *gensio_cat_addrinfo(struct gensio_os_funcs *o,
				     struct addrinfo *ai1,
				     struct addrinfo *ai2)
{
    struct addrinfo *rai = ai1;

    while (ai1->ai_next)
	ai1 = ai1->ai_next;
    ai1->ai_next = ai2;

    return rai;
}

void
gensio_free_addrinfo(struct gensio_os_funcs *o, struct addrinfo *ai)
{
    while (ai) {
	struct addrinfo *aic = ai;

	ai = ai->ai_next;
	o->free(o, aic->ai_addr);
	if (aic->ai_canonname)
	    o->free(o, aic->ai_canonname);
	o->free(o, aic);
    }
}

int
gensio_sockaddr_to_str(const struct sockaddr *addr, socklen_t *addrlen,
		       char *buf, gensiods *epos, gensiods buflen)
{
    gensiods pos = 0;
    gensiods left;

    if (epos)
	pos = *epos;

    if (pos >= buflen)
	left = 0;
    else
	left = buflen - pos;

    if (addr->sa_family == AF_INET) {
	struct sockaddr_in *a4 = (struct sockaddr_in *) addr;
	char ibuf[INET_ADDRSTRLEN];

	if (addrlen && *addrlen && *addrlen != sizeof(struct sockaddr_in))
	    goto out_err;
	pos += snprintf(buf + pos, left, "%s,%d",
			inet_ntop(AF_INET, &a4->sin_addr, ibuf, sizeof(ibuf)),
			ntohs(a4->sin_port));
	if (addrlen)
	    *addrlen = sizeof(struct sockaddr_in);
    } else if (addr->sa_family == AF_INET6) {
	struct sockaddr_in6 *a6 = (struct sockaddr_in6 *) addr;
	char ibuf[INET6_ADDRSTRLEN];

	if (addrlen && *addrlen && *addrlen != sizeof(struct sockaddr_in6))
	    goto out_err;
	pos += snprintf(buf + pos, left, "%s,%d",
			inet_ntop(AF_INET6, &a6->sin6_addr, ibuf, sizeof(ibuf)),
			ntohs(a6->sin6_port));
	if (addrlen)
	    *addrlen = sizeof(struct sockaddr_in6);
    } else {
    out_err:
	if (left)
	    buf[pos] = '\0';
	return GE_INVAL;
    }

    if (epos)
	*epos = pos;

    return 0;
}

int
gensio_check_keyvalue(const char *str, const char *key, const char **value)
{
    unsigned int keylen = strlen(key);

    if (strncasecmp(str, key, keylen) != 0)
	return 0;
    if (str[keylen] != '=')
	return 0;
    *value = str + keylen + 1;
    return 1;
}

int
gensio_check_keyds(const char *str, const char *key, gensiods *rvalue)
{
    const char *sval;
    char *end;
    int rv = gensio_check_keyvalue(str, key, &sval);
    gensiods value;

    if (!rv)
	return 0;

    if (!*sval)
	return -1;

    value = strtoul(sval, &end, 0);
    if (*end != '\0')
	return -1;

    *rvalue = value;
    return 1;
}

int
gensio_check_keyuint(const char *str, const char *key, unsigned int *rvalue)
{
    const char *sval;
    char *end;
    int rv = gensio_check_keyvalue(str, key, &sval);
    unsigned long value;

    if (!rv)
	return 0;

    if (!*sval)
	return -1;

    value = strtoul(sval, &end, 0);
    if (*end != '\0')
	return -1;

    if (value > UINT_MAX)
	return -1;

    *rvalue = value;
    return 1;
}

int
gensio_check_keybool(const char *str, const char *key, bool *rvalue)
{
    const char *sval;
    int rv;

    if (strcasecmp(str, key) == 0) {
	*rvalue = true;
	return 1;
    }

    rv = gensio_check_keyvalue(str, key, &sval);
    if (!rv)
	return 0;

    if (!*sval)
	return -1;

    if (strcmp(sval, "true") == 0 || strcmp(sval, "1") == 0)
	*rvalue = true;
    else if (strcmp(sval, "false") == 0 || strcmp(sval, "0") == 0)
	*rvalue = false;
    else
	return -1;

    return 1;
}

int
gensio_check_keyboolv(const char *str, const char *key, const char *trueval,
		      const char *falseval, bool *rvalue)
{
    const char *sval;
    int rv;

    rv = gensio_check_keyvalue(str, key, &sval);
    if (!rv)
	return 0;

    if (!*sval)
	return -1;

    if (strcmp(sval, trueval) == 0)
	*rvalue = true;
    else if (strcmp(sval, falseval) == 0)
	*rvalue = false;
    else
	return -1;

    return 1;
}

int
gensio_check_keyenum(const char *str, const char *key,
		     struct gensio_enum_val *enums, int *rval)
{
    const char *sval;
    int rv;
    unsigned int i;

    rv = gensio_check_keyvalue(str, key, &sval);
    if (!rv)
	return 0;

    for (i = 0; enums[i].name; i++) {
	if (strcasecmp(sval, enums[i].name) == 0) {
	    *rval = enums[i].val;
	    return 1;
	}
    }

    return -1;
}

int
gensio_check_keyaddrs(struct gensio_os_funcs *o,
		      const char *str, const char *key, int iprotocol,
		      bool listen, bool require_port, struct addrinfo **rai)
{
    const char *sval;
    int rv;
    struct addrinfo *ai;
    int socktype, protocol;
    bool is_port_set;

    rv = gensio_check_keyvalue(str, key, &sval);
    if (!rv)
	return 0;

    if (!*sval)
	return -1;

    rv = gensio_scan_network_port(o, sval, listen, &ai, &socktype,
				  &protocol, &is_port_set, NULL, NULL);
    if (rv)
	return -1;

    if ((require_port && !is_port_set) || protocol != iprotocol) {
	gensio_free_addrinfo(o, ai);
	return -1;
    }

    if (*rai)
	gensio_free_addrinfo(o, *rai);

    *rai = ai;

    return 1;
}

void
gensio_set_log_mask(unsigned int mask)
{
    gensio_log_mask = mask;
}

unsigned int
gensio_get_log_mask(void)
{
    return gensio_log_mask;
}

void
gensio_vlog(struct gensio_os_funcs *o, enum gensio_log_levels level,
	    const char *str, va_list args)
{
    if (!(gensio_log_mask & (1 << level)))
	return;

    o->vlog(o, level, str, args);
}

void
gensio_log(struct gensio_os_funcs *o, enum gensio_log_levels level,
	   const char *str, ...)
{
    va_list args;

    va_start(args, str);
    gensio_vlog(o, level, str, args);
    va_end(args);
}

void
gensio_acc_vlog(struct gensio_accepter *acc, enum gensio_log_levels level,
		char *str, va_list args)
{
    struct gensio_loginfo info;

    if (!(gensio_log_mask & (1 << level)))
	return;

    info.level = level;
    info.str = str;
    va_copy(info.args, args);
    acc->cb(acc, acc->user_data, GENSIO_ACC_EVENT_LOG, &info);
    va_end(info.args);
}

void
gensio_acc_log(struct gensio_accepter *acc, enum gensio_log_levels level,
	       char *str, ...)
{
    va_list args;

    va_start(args, str);
    gensio_acc_vlog(acc, level, str, args);
    va_end(args);
}

static struct gensio_once gensio_default_initialized;

static struct gensio_lock *deflock;

union gensio_def_val {
	char *strval;
	int intval;
};

struct gensio_class_def {
    char *class;
    union gensio_def_val val;
    struct gensio_class_def *next;
};

struct gensio_def_entry {
    char *name;
    enum gensio_default_type type;
    int min;
    int max;
    union gensio_def_val val;
    bool val_set;
    union gensio_def_val def;
    const struct gensio_enum_val *enums;
    struct gensio_class_def *classvals;
    struct gensio_def_entry *next;
};

#ifdef HAVE_OPENIPMI
#include <OpenIPMI/ipmi_conn.h>
#include <OpenIPMI/ipmi_sol.h>
struct gensio_enum_val shared_serial_alert_enums[] = {
    { "fail",		ipmi_sol_serial_alerts_fail },
    { "deferred", 	ipmi_sol_serial_alerts_deferred },
    { "succeed", 	ipmi_sol_serial_alerts_succeed },
    { NULL }
};
#endif

struct gensio_def_entry builtin_defaults[] = {
    /* Defaults for TCP, UDP, and SCTP. */
    { "nodelay",	GENSIO_DEFAULT_BOOL,	.def.intval = 0 },
    { "laddr",		GENSIO_DEFAULT_STR,	.def.strval = NULL },
    /* sctp */
    { "instreams",	GENSIO_DEFAULT_INT,	.min = 1, .max = INT_MAX,
						.def.intval = 1 },
    { "ostreams",	GENSIO_DEFAULT_INT,	.min = 1, .max = INT_MAX,
						.def.intval = 1 },
    /* serialdev */
    { "rtscts",		GENSIO_DEFAULT_BOOL,	.def.intval = 0 },
    { "local",		GENSIO_DEFAULT_BOOL,	.def.intval = 0 },
    { "hangup_when_done", GENSIO_DEFAULT_BOOL,	.def.intval = 0 },
    { "rs485",		GENSIO_DEFAULT_STR,	.def.strval = NULL },
    /* serialdev and SOL */
    { "speed",		GENSIO_DEFAULT_STR,	.def.strval = "9600N81" },
    { "nobreak",	GENSIO_DEFAULT_BOOL,	.def.intval = 0 },
#ifdef HAVE_OPENIPMI
    /* SOL only */
    { "authenticated",	GENSIO_DEFAULT_BOOL,	.def.intval = 1 },
    { "encrypted",	GENSIO_DEFAULT_BOOL,	.def.intval = 1 },
    { "ack-timeout",	GENSIO_DEFAULT_INT,	.min = 1, .max = INT_MAX,
						.def.intval = 1000000 },
    { "ack-retries",	GENSIO_DEFAULT_INT,	.min = 1, .max = INT_MAX,
						.def.intval = 10 },
    { "shared-serial-alert", GENSIO_DEFAULT_ENUM,
				.enums = shared_serial_alert_enums,
				.def.intval = ipmi_sol_serial_alerts_fail },
    { "deassert_CTS_DCD_DSR_on_connect", GENSIO_DEFAULT_BOOL, .def.intval = 0 },
#endif
    /* For client/server protocols. */
    { "mode",		GENSIO_DEFAULT_STR,	.def.strval = NULL },
    /* For telnet */
    { "rfc2217",	GENSIO_DEFAULT_BOOL,	.def.intval = false },
    /* For SSL or other key authentication. */
    { "CA",		GENSIO_DEFAULT_STR,	.def.strval = NULL },
    { "cert",		GENSIO_DEFAULT_STR,	.def.strval = NULL },
    { "key",		GENSIO_DEFAULT_STR,	.def.strval = NULL },
    { "clientauth",	GENSIO_DEFAULT_BOOL,	.def.intval = false },
    /* General authentication flags. */
    { "allow-authfail",	GENSIO_DEFAULT_BOOL,	.def.intval = false },
    { "username",	GENSIO_DEFAULT_STR,	.def.strval = NULL },
    { "password",	GENSIO_DEFAULT_STR,	.def.strval = NULL },
    { "service",	GENSIO_DEFAULT_STR,	.def.strval = NULL },
    { "use-child-auth",	GENSIO_DEFAULT_BOOL,	.def.intval = false, },
    { "enable-password",GENSIO_DEFAULT_BOOL,	.def.intval = false, },
    {}
};

static struct gensio_def_entry *defaults;

static void
gensio_default_init(void *cb_data)
{
    struct gensio_os_funcs *o = cb_data;

    deflock = o->alloc_lock(o);
    if (!deflock)
	gensio_log(o, GENSIO_LOG_FATAL,
		   "Unable to allocate gensio default lock");
}

static void
gensio_reset_default(struct gensio_os_funcs *o, struct gensio_def_entry *d)
{
    struct gensio_class_def *n, *c = d->classvals;

    for (; c; c = n) {
	n = c->next;
	o->free(o, c->class);
	if (d->type == GENSIO_DEFAULT_STR && c->val.strval)
	    o->free(o, c->val.strval);
	o->free(o, c);
    }
    d->classvals = NULL;

    if (d->type == GENSIO_DEFAULT_STR && d->val.strval) {
	o->free(o, d->val.strval);
	d->val.strval = NULL;
    }
    d->val_set = false;
}

void
gensio_reset_defaults(struct gensio_os_funcs *o)
{
    struct gensio_def_entry *d;
    unsigned int i;

    o->call_once(o, &gensio_default_initialized, gensio_default_init, o);

    o->lock(deflock);
    for (i = 0; builtin_defaults[i].name; i++)
	gensio_reset_default(o, &builtin_defaults[i]);
    for (d = defaults; d; d = d->next)
	gensio_reset_default(o, d);
    o->unlock(deflock);
}

static struct gensio_def_entry *
gensio_lookup_default(const char *name, struct gensio_def_entry **prev,
		      bool *isdefault)
{
    struct gensio_def_entry *d, *p = NULL;
    unsigned int i;

    for (i = 0; builtin_defaults[i].name; i++) {
	if (strcmp(builtin_defaults[i].name, name) == 0) {
	    if (prev)
		*prev = NULL;
	    if (isdefault)
		*isdefault = true;
	    return &builtin_defaults[i];
	}
    }
    for (d = defaults; d; d = d->next) {
	if (strcmp(d->name, name) == 0) {
	    if (prev)
		*prev = p;
	    if (isdefault)
		*isdefault = false;
	    return d;
	}
	p = d;
    }
    return NULL;
}

static struct gensio_class_def *
gensio_lookup_default_class(struct gensio_def_entry *d, const char *class,
			    struct gensio_class_def **prev)
{
    struct gensio_class_def *c = d->classvals;
    struct gensio_class_def *p = NULL;

    for (; c; c = c->next) {
	if (strcmp(c->class, class) == 0) {
	    if (prev)
		*prev = p;
	    return c;
	}
	p = c;
    }
    return NULL;
}

int
gensio_add_default(struct gensio_os_funcs *o,
		   const char *name,
		   enum gensio_default_type type,
		   const char *strval, int intval,
		   int minval, int maxval,
		   const struct gensio_enum_val *enums)
{
    int err = 0;
    struct gensio_def_entry *d;

    o->call_once(o, &gensio_default_initialized, gensio_default_init, o);

    o->lock(deflock);
    d = gensio_lookup_default(name, NULL, NULL);
    if (d) {
	err = GE_EXISTS;
	goto out_unlock;
    }

    d = o->zalloc(o, sizeof(*d));
    if (!d) {
	err = GE_NOMEM;
	goto out_unlock;
    }

    d->name = strdup(name);
    if (!d->name) {
	o->free(o, d);
	err = GE_NOMEM;
	goto out_unlock;
    }
    d->type = type;
    d->min = minval;
    d->max = maxval;
    d->enums = enums;
    d->def.intval = intval;
    if (strval) {
	d->def.strval = strdup(strval);
	if (!d->def.strval) {
	    o->free(o, d->name);
	    o->free(o, d);
	    err = GE_NOMEM;
	    goto out_unlock;
	}
    }

    d->next = defaults;
    defaults = d;

 out_unlock:
    o->unlock(deflock);
    return err;
}

int
gensio_set_default(struct gensio_os_funcs *o,
		   const char *class, const char *name,
		   const char *strval, int intval)
{
    int err = 0;
    struct gensio_def_entry *d;
    char *new_strval = NULL, *end;
    unsigned int i;

    o->call_once(o, &gensio_default_initialized, gensio_default_init, o);

    o->lock(deflock);
    d = gensio_lookup_default(name, NULL, NULL);
    if (!d) {
	err = GE_NOTFOUND;
	goto out_unlock;
    }

    switch (d->type) {
    case GENSIO_DEFAULT_ENUM:
	if (!strval) {
	    err = GE_INVAL;
	    goto out_unlock;
	}
	for (i = 0; d->enums[i].name; i++) {
	    if (strcmp(d->enums[i].name, strval) == 0)
		break;
	}
	if (!d->enums[i].name) {
	    err = GE_INVAL;
	    goto out_unlock;
	}
	intval = d->enums[i].val;
	break;

    case GENSIO_DEFAULT_BOOL:
	if (strval) {
	    if (strcmp(strval, "true") == 0 ||
			strcmp(strval, "TRUE") == 0) {
		intval = 1;
	    } else if (strcmp(strval, "false") == 0 ||
		       strcmp(strval, "FALSE") == 0) {
		intval = 0;
	    } else {
		intval = strtoul(strval, &end, 10);
		if (end == strval || *end) {
		    err = GE_INVAL;
		    goto out_unlock;
		}
	    }
	} else {
	    intval = !!intval;
	}
	break;

    case GENSIO_DEFAULT_INT:
	if (strval) {
	    intval = strtoul(strval, &end, 10);
	    if (end == strval || *end) {
		err = GE_INVAL;
		goto out_unlock;
	    }
	    if (intval < d->min || intval > d->max) {
		err = GE_OUTOFRANGE;
		goto out_unlock;
	    }
	}
	break;

    case GENSIO_DEFAULT_STR:
	if (strval) {
	    new_strval = gensio_strdup(o, strval);
	    if (!new_strval) {
		err = GE_NOMEM;
		goto out_unlock;
	    }
	}
	break;

    default:
	err = GE_INVAL;
	goto out_unlock;
    }

    if (class) {
	struct gensio_class_def *c = gensio_lookup_default_class(d, class,
								 NULL);

	if (!c) {
	    c = o->zalloc(o, sizeof(*c));
	    if (!c) {
		err = GE_NOMEM;
		goto out_unlock;
	    }
	    c->class = gensio_strdup(o, class);
	    if (!c->class) {
		o->free(o, c);
		err = GE_NOMEM;
		goto out_unlock;
	    }
	    c->next = d->classvals;
	    d->classvals = c;
	}
	if (d->type == GENSIO_DEFAULT_STR) {
	    if (c->val.strval)
		o->free(o, c->val.strval);
	    c->val.strval = new_strval;
	    new_strval = NULL;
	} else {
	    c->val.intval = intval;
	}
    } else {
	if (d->type == GENSIO_DEFAULT_STR) {
	    if (d->val.strval)
		o->free(o, d->val.strval);
	    d->val.strval = new_strval;
	    new_strval = NULL;
	} else {
	    d->val.intval = intval;
	}
	d->val_set = true;
    }

 out_unlock:
    if (new_strval)
	o->free(o, new_strval);
    o->unlock(deflock);
    return err;
}

int
gensio_get_default(struct gensio_os_funcs *o,
		   const char *class, const char *name, bool classonly,
		   enum gensio_default_type type,
		   char **strval, int *intval)
{
    struct gensio_def_entry *d;
    struct gensio_class_def *c = NULL;
    union gensio_def_val *val;
    int err = 0;
    char *str;

    o->call_once(o, &gensio_default_initialized, gensio_default_init, o);

    o->lock(deflock);
    d = gensio_lookup_default(name, NULL, NULL);
    if (!d) {
	err = GE_NOTFOUND;
	goto out_unlock;
    }

    if (d->type != type &&
	    !(d->type == GENSIO_DEFAULT_ENUM && type == GENSIO_DEFAULT_INT) &&
	    !(d->type == GENSIO_DEFAULT_BOOL && type == GENSIO_DEFAULT_INT)) {
	err = GE_INVAL;
	goto out_unlock;
    }

    if (class)
	c = gensio_lookup_default_class(d, class, NULL);

    if (c)
	val = &c->val;
    else if (classonly) {
	err = GE_NOTFOUND;
	goto out_unlock;
    } else if (d->val_set)
	val = &d->val;
    else
	val = &d->def;

    switch (type) {
    case GENSIO_DEFAULT_BOOL:
    case GENSIO_DEFAULT_ENUM:
    case GENSIO_DEFAULT_INT:
	*intval = val->intval;
	break;

    case GENSIO_DEFAULT_STR:
	if (val->strval) {
	    str = gensio_strdup(o, val->strval);
	    if (!str) {
		err = GE_NOMEM;
		goto out_unlock;
	    }
	    *strval = str;
	} else {
	    *strval = NULL;
	}
	break;

    default:
	abort(); /* Shouldn't happen. */
    }

 out_unlock:
    o->unlock(deflock);

    return err;
}

int
gensio_del_default(struct gensio_os_funcs *o,
		   const char *class, const char *name, bool delclasses)
{
    struct gensio_def_entry *d, *prev;
    struct gensio_class_def *c = NULL, *prevc;
    bool isdefault;
    int err = 0;

    o->call_once(o, &gensio_default_initialized, gensio_default_init, o);

    o->lock(deflock);
    d = gensio_lookup_default(name, &prev, &isdefault);
    if (!d) {
	err = GE_NOTFOUND;
	goto out_unlock;
    }

    if (class) {
	c = gensio_lookup_default_class(d, class, &prevc);
	if (!c) {
	    err = GE_NOTFOUND;
	    goto out_unlock;
	}
	if (prevc)
	    prevc->next = c->next;
	else
	    d->classvals = c->next;
	
	if (d->type == GENSIO_DEFAULT_STR && c->val.strval)
	    o->free(o, c->val.strval);
	o->free(o, c->class);
	o->free(o, c);
	goto out_unlock;
    }

    if (isdefault) {
	err = GE_NOTSUP;
	goto out_unlock;
    }

    if (d->classvals && !delclasses) {
	err = GE_INUSE;
	goto out_unlock;
    }

    if (prev)
	prev->next = d->next;
    else
	defaults = d->next;

    while (d->classvals) {
	c = d->classvals;
	d->classvals = c->next;
	if (d->type == GENSIO_DEFAULT_STR && c->val.strval)
	    o->free(o, c->val.strval);
	o->free(o, c->class);
	o->free(o, c);
    }

    if (d->type == GENSIO_DEFAULT_STR && d->val.strval)
	o->free(o, d->val.strval);
    o->free(o, d->name);
    o->free(o, d);

 out_unlock:
    o->unlock(deflock);

    return err;
}

int
gensio_get_defaultaddr(struct gensio_os_funcs *o,
		       const char *class, const char *name, bool classonly,
		       int iprotocol, bool listen, bool require_port,
		       struct addrinfo **rai)
{
    int err;
    int socktype, protocol;
    struct addrinfo *ai;
    bool is_port_set;
    char *str;

    err = gensio_get_default(o, class, name, classonly, GENSIO_DEFAULT_STR,
			     &str, NULL);
    if (err)
	return err;

    if (!str)
	return GE_NOTSUP;

    err = gensio_scan_network_port(o, str, listen, &ai, &socktype,
				   &protocol, &is_port_set, NULL, NULL);
    o->free(o, str);
    if (err)
	return err;

    if ((require_port && !is_port_set) || protocol != iprotocol) {
	gensio_free_addrinfo(o, ai);
	return GE_INCONSISTENT;
    }

    if (*rai)
	gensio_free_addrinfo(o, *rai);

    *rai = ai;

    return 1;
}

void
gensio_list_rm(struct gensio_list *list, struct gensio_link *link)
{
    link->next->prev = link->prev;
    link->prev->next = link->next;
}

void
gensio_list_add_tail(struct gensio_list *list, struct gensio_link *link)
{
    link->prev = list->link.prev;
    link->next = &list->link;
    list->link.prev->next = link;
    list->link.prev = link;
}

void
gensio_list_add_next(struct gensio_list *list, struct gensio_link *curr,
		     struct gensio_link *link)
{
    link->next = curr->next;
    link->prev = curr;
    curr->next->prev = link;
    curr->next = link;
}

void
gensio_list_add_prev(struct gensio_list *list, struct gensio_link *curr,
		     struct gensio_link *link)
{
    link->prev = curr->prev;
    link->next = curr;
    curr->prev->next = link;
    curr->prev = link;
}

void
gensio_list_init(struct gensio_list *list)
{
    list->link.next = &list->link;
    list->link.prev = &list->link;
}

bool
gensio_list_empty(struct gensio_list *list)
{
    return list->link.next == &list->link;
}

const char *
gensio_log_level_to_str(enum gensio_log_levels level)
{
    switch (level) {
    case GENSIO_LOG_FATAL: return "fatal"; break;
    case GENSIO_LOG_ERR: return "err"; break;
    case GENSIO_LOG_WARNING: return "warning"; break;
    case GENSIO_LOG_INFO: return "info"; break;
    case GENSIO_LOG_DEBUG: return "debug"; break;
    default: return "invalid";
    }
}

static int
gensio_wait_no_cb(struct gensio *io, struct gensio_waiter *waiter,
		  struct timeval *timeout)
{
    struct gensio_os_funcs *o = io->o;
    struct gensio_nocbwait wait;
    int rv = 0;

    wait.waiter = waiter;
    o->lock(io->lock);
    if (io->cb_count != 0) {
	wait.queued = true;
	gensio_list_add_tail(&io->waiters, &wait.link);
	o->unlock(io->lock);
	rv = o->wait(waiter, 1, timeout);
	o->lock(io->lock);
	if (wait.queued) {
	    rv = GE_TIMEDOUT;
	    gensio_list_rm(&io->waiters, &wait.link);
	}
    }
    o->unlock(io->lock);
    return rv;
}

struct gensio_sync_op {
    bool queued;
    unsigned char *buf;
    gensiods len;
    int err;
    struct gensio_waiter *waiter;
    struct gensio_link link;
};

struct gensio_sync_io {
    gensio_event old_cb;

    struct gensio_list readops;
    struct gensio_list writeops;
    int err;

    struct gensio_lock *lock;
    struct gensio_waiter *close_waiter;
};

static void
gensio_sync_flush_waiters(struct gensio_sync_io *sync_io,
			  struct gensio_os_funcs *o)
{
    struct gensio_link *l, *l2;

    gensio_list_for_each_safe(&sync_io->readops, l, l2) {
	struct gensio_sync_op *op = gensio_container_of(l,
							struct gensio_sync_op,
							link);

	op->err = sync_io->err;
	op->queued = false;
	o->wake(op->waiter);
	gensio_list_rm(&sync_io->readops, l);
    }

    gensio_list_for_each_safe(&sync_io->writeops, l, l2) {
	struct gensio_sync_op *op = gensio_container_of(l,
							struct gensio_sync_op,
							link);

	op->err = sync_io->err;
	op->queued = false;
	o->wake(op->waiter);
	gensio_list_rm(&sync_io->writeops, l);
    }
}

static int
gensio_syncio_event(struct gensio *io, void *user_data,
		    int event, int err,
		    unsigned char *buf, gensiods *buflen,
		    const char *const *auxdata)
{
    struct gensio_os_funcs *o = io->o;
    struct gensio_sync_io *sync_io = io->sync_io;
    gensiods done_len;

    switch (event) {
    case GENSIO_EVENT_READ:
	o->lock(sync_io->lock);
	if (err) {
	    if (!sync_io->err)
		sync_io->err = err;
	    gensio_sync_flush_waiters(sync_io, o);
	    goto read_unlock;
	}
	if (gensio_list_empty(&sync_io->readops)) {
	    *buflen = 0;
	    gensio_set_read_callback_enable(io, false);
	    goto read_unlock;
	}
	done_len = *buflen;
	while (*buflen && !gensio_list_empty(&sync_io->readops)) {
	    struct gensio_link *l = gensio_list_first(&sync_io->readops);
	    struct gensio_sync_op *op = gensio_container_of(l,
							struct gensio_sync_op,
							link);
	    gensiods len = done_len;

	    if (len > op->len)
		len = op->len;
	    memcpy(op->buf, buf, len);
	    op->len = len;
	    gensio_list_rm(&sync_io->readops, l);
	    op->queued = false;
	    o->wake(op->waiter);
	    done_len -= len;
	}
	*buflen -= done_len;
	if (done_len > 0)
	    gensio_set_read_callback_enable(io, false);
    read_unlock:
	o->unlock(sync_io->lock);
	return 0;

    case GENSIO_EVENT_WRITE_READY:
	o->lock(sync_io->lock);
	if (gensio_list_empty(&sync_io->writeops)) {
	    gensio_set_write_callback_enable(io, false);
	    goto write_unlock;
	}
	while (!gensio_list_empty(&sync_io->writeops)) {
	    struct gensio_link *l = gensio_list_first(&sync_io->writeops);
	    struct gensio_sync_op *op = gensio_container_of(l,
							struct gensio_sync_op,
							link);
	    gensiods len = 0;

	    err = gensio_write(io, &len, op->buf, op->len, NULL);
	    if (err) {
		if (!sync_io->err)
		    sync_io->err = err;
		gensio_sync_flush_waiters(sync_io, o);
	    } else {
		op->buf += len;
		op->len -= len;
		if (op->len == 0) {
		    gensio_list_rm(&sync_io->readops, l);
		    op->queued = false;
		    o->wake(op->waiter);
		} else {
		    break;
		}
	    }
	}
	if (gensio_list_empty(&sync_io->writeops))
	    gensio_set_write_callback_enable(io, false);
    write_unlock:
	o->unlock(sync_io->lock);
	return 0;

    default:
	if (sync_io->old_cb)
	    return sync_io->old_cb(io, io->user_data,
				   event, err, buf, buflen, auxdata);
	return GE_NOTSUP;
    }
}

int
gensio_set_sync(struct gensio *io)
{
    struct gensio_os_funcs *o = io->o;
    struct gensio_sync_io *sync_io = o->zalloc(o, sizeof(*sync_io));

    if (!sync_io)
	return GE_NOMEM;

    sync_io->lock = o->alloc_lock(o);
    if (!sync_io->lock) {
	o->free(o, sync_io);
	return GE_NOMEM;
    }

    sync_io->close_waiter = o->alloc_waiter(o);
    if (!sync_io->close_waiter) {
	o->free_lock(sync_io->lock);
	o->free(o, sync_io);
	return GE_NOMEM;
    }

    gensio_list_init(&sync_io->readops);
    gensio_list_init(&sync_io->writeops);

    gensio_set_read_callback_enable(io, false);
    gensio_set_write_callback_enable(io, false);
    gensio_wait_no_cb(io, sync_io->close_waiter, NULL);

    io->sync_io = sync_io;
    sync_io->old_cb = io->cb;
    io->cb = gensio_syncio_event;
    return 0;
}

int
gensio_clear_sync(struct gensio *io)
{
    struct gensio_os_funcs *o = io->o;
    struct gensio_sync_io *sync_io = io->sync_io;

    if (!sync_io)
	return GE_NOTREADY;

    gensio_set_read_callback_enable(io, false);
    gensio_set_write_callback_enable(io, false);
    gensio_wait_no_cb(io, sync_io->close_waiter, NULL);

    io->cb = sync_io->old_cb;

    o->free_waiter(sync_io->close_waiter);
    o->free_lock(sync_io->lock);
    o->free(o, sync_io);
    io->sync_io = NULL;

    return 0;
}

int
gensio_read_s(struct gensio *io, gensiods *count, void *data, gensiods datalen,
	      struct timeval *timeout)
{
    struct gensio_os_funcs *o = io->o;
    struct gensio_sync_io *sync_io = io->sync_io;
    struct gensio_sync_op op;
    int rv = 0;

    if (!sync_io)
	return GE_NOTREADY;

    if (datalen == 0) {
	if (count)
	    *count = 0;
	return 0;
    }

    op.queued = true;
    op.buf = data;
    op.len = datalen;
    op.err = 0;
    op.waiter = o->alloc_waiter(o);
    if (!op.waiter)
	return GE_NOMEM;
    o->lock(sync_io->lock);
    if (sync_io->err) {
	rv = sync_io->err;
	goto out_unlock;
    }
    gensio_set_read_callback_enable(io, true);
    gensio_list_add_tail(&sync_io->readops, &op.link);

    o->unlock(sync_io->lock);
    o->wait_intr(op.waiter, 1, timeout);
    o->lock(sync_io->lock);
    if (op.err) {
	rv = op.err;
    } else if (op.queued) {
	if (count)
	    *count = 0;
	gensio_list_rm(&sync_io->readops, &op.link);
    } else if (count) {
	*count = op.len;
    }
    if (gensio_list_empty(&sync_io->readops))
	gensio_set_read_callback_enable(io, false);
 out_unlock:
    o->unlock(sync_io->lock);
    o->free_waiter(op.waiter);

    return rv;
}

int
gensio_write_s(struct gensio *io, gensiods *count,
	       const void *data, gensiods datalen,
	       struct timeval *timeout)
{
    struct gensio_os_funcs *o = io->o;
    struct gensio_sync_io *sync_io = io->sync_io;
    struct gensio_sync_op op;
    int rv = 0;
    gensiods origlen;

    if (!sync_io)
	return GE_NOTREADY;

    if (datalen == 0) {
	if (count)
	    *count = 0;
	return 0;
    }

    origlen = datalen;
    op.queued = true;
    op.buf = (void *) data;
    op.len = datalen;
    op.err = 0;
    op.waiter = o->alloc_waiter(o);
    if (!op.waiter)
	return GE_NOMEM;
    o->lock(sync_io->lock);
    if (sync_io->err) {
	rv = sync_io->err;
	goto out_unlock;
    }
    gensio_set_write_callback_enable(io, true);
    gensio_list_add_tail(&sync_io->writeops, &op.link);

    o->unlock(sync_io->lock);
    o->wait_intr(op.waiter, 1, timeout);
    o->lock(sync_io->lock);
    if (op.queued)
	gensio_list_rm(&sync_io->readops, &op.link);
    if (op.err)
	rv = op.err;
    else if (count)
	*count = origlen - op.len;
    if (gensio_list_empty(&sync_io->writeops))
	gensio_set_write_callback_enable(io, false);
 out_unlock:
    o->unlock(sync_io->lock);
    o->free_waiter(op.waiter);

    return rv;
}
