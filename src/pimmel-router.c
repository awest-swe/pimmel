/*** pimmel-router.c -- pimmel router-dealer
 *
 * Copyright (C) 2013 Sebastian Freundt
 *
 * Author:  Sebastian Freundt <freundt@ga-group.nl>
 *
 * This file is part of pimmel.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the author nor the names of any contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ***/
#if defined HAVE_CONFIG_H
# include "config.h"
#endif	/* HAVE_CONFIG_H */
#include <stdio.h>
#include <string.h>
#include <time.h>
#if defined HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif	/* HAVE_SYS_TYPES_H */
#if defined HAVE_SYS_SOCKET_H
# include <sys/socket.h>
#endif	/* HAVE_SYS_SOCKET_H */
#if defined HAVE_NETINET_IN_H
# include <netinet/in.h>
#endif	/* HAVE_NETINET_IN_H */
#if defined HAVE_ARPA_INET_H
# include <arpa/inet.h>
#endif	/* HAVE_ARPA_INET_H */
#if defined HAVE_NETDB_H
# include <netdb.h>
#endif	/* HAVE_NETDB_H */
#if defined HAVE_EV_H
# include <ev.h>
# undef EV_P
# define EV_P  struct ev_loop *loop __attribute__((unused))
#endif	/* HAVE_EV_H */
#include <pimmel.h>
#include "nifty.h"
#include "ud-sock.h"
#include "daemonise.h"

#if defined DEBUG_FLAG && !defined BENCHMARK
# include <assert.h>
# define PMML_DEBUG(args...)	fprintf(stderr, args)
# define MAYBE_NOINLINE		__attribute__((noinline))
# define MAYBE_UNUSED		UNUSED
#else  /* !DEBUG_FLAG */
# define assert(...)
# define PMML_DEBUG(args...)
# define MAYBE_UNUSED		UNUSED
# define MAYBE_NOINLINE
#endif	/* DEBUG_FLAG */

typedef struct ctx_s *ctx_t;

struct ctx_s {
	/* socket used for forwarding */
	int dst;

	enum {
		PROTO_UDP,
		PROTO_TCP,
	} proto;
	const char *host;
	const char *port;

	/* router identity of the form RTR_xxx */
	uint32_t ident;
};


static void
ev_io_shut(EV_P_ ev_io *w)
{
	int fd = w->fd;

	ev_io_stop(EV_A_ w);

	shutdown(fd, SHUT_RDWR);
	close(fd);
	w->fd = -1;
	w->data = NULL;
	return;
}

static int
massage_conn(ctx_t ctx, const char *conn)
{
	char *port;

	ctx->proto = PROTO_UDP;
	for (char *p; (p = strstr(conn, "://")) != NULL;) {
		*p = '\0';

		if (!strcmp(conn, "udp")) {
			ctx->proto = PROTO_UDP;
		} else if (!strcmp(conn, "tcp")) {
			ctx->proto = PROTO_TCP;
		} else {
			fprintf(stderr, "cannot handle protocol %s\n", conn);
			return -1;
		}

		conn = p + 3;
		break;
	}

	if ((port = strrchr(conn, ':')) == NULL || strchr(port, ']') != NULL) {
		fprintf(stderr, "no port specified\n");
		return -1;
	}
	/* otherwise */
	*port++ = '\0';

	/* mash it all up */
	ctx->host = conn;
	ctx->port = port;
	return 0;
}

static uint32_t
make_router_id(void)
{
	uint32_t res = 0U;
	int s;

	/* mangle buf and put hex repr into ctx->ident */
	if ((s = open("/dev/urandom", O_RDONLY)) < 0) {

	} else if (read(s, &res, sizeof(res)) < (ssize_t)sizeof(res)) {

	} else {
		close(s);
	}
	return res;
}

static uint32_t
read_router_id(const char *s, size_t z)
{
	uint32_t res = 0U;

	if (z != 12U) {
		return 0U;
	} else if (*s++ != 'R' || *s++ != 'T' || *s++ != 'R' || *s++ != '_') {
		return 0U;
	}
	/* otherwise decipher the rest */
	for (size_t i = 0; i < 8U; i++, s++) {
		res <<= 4U;
		switch (*s) {
		case '0' ... '9':
			res += *s - '0';
			break;
		case 'a' ... 'f':
			res += *s - 'a' + 10;
			break;
		case 'A' ... 'F':
			res += *s - 'A' + 10;
			break;
		default:
			break;
		}
	}
	return res;
}

static int
bang_ident(char *restrict s, size_t z, uint32_t idn)
{
	if (z < 12U) {
		return -1;
	}

	*s++ = 'R';
	*s++ = 'T';
	*s++ = 'R';
	*s++ = '_';

	s += 8U;
	for (size_t i = 0; i < 8U; i++, idn >>= 4U) {
		static char ch[] = "0123456789abcdef";
		*--s = ch[idn & 0xf];
	}
	return 0;
}


static int
try_connect(struct addrinfo **aires)
{
	struct addrinfo *ai = *aires;

	for (int s; ai != NULL; ai = ai->ai_next, close(s), s = -1) {
		if ((s = socket(ai->ai_family, ai->ai_socktype, 0)) >= 0 &&
		    connect(s, ai->ai_addr, ai->ai_addrlen) >= 0) {
			*aires = ai;
			return s;
		}
	}
	return -1;
}

static int
pmml_router_socket(ctx_t ctx)
{
        struct addrinfo *aires;
        struct addrinfo hints = {0};
	int s = 0;

	/* set up hints for gai */
        hints.ai_family = AF_UNSPEC;
	switch (ctx->proto) {
	case PROTO_UDP:
		hints.ai_socktype = SOCK_DGRAM;
		break;
	case PROTO_TCP:
		hints.ai_socktype = SOCK_STREAM;
		break;
	default:
		abort();
	}
        hints.ai_flags = 0;
#if defined AI_ADDRCONFIG
        hints.ai_flags |= AI_ADDRCONFIG;
#endif  /* AI_ADDRCONFIG */
#if defined AI_V4MAPPED
        hints.ai_flags |= AI_V4MAPPED;
#endif  /* AI_V4MAPPED */
        hints.ai_protocol = 0;

	if (getaddrinfo(ctx->host, ctx->port, &hints, &aires) < 0) {
		goto out;
	} else if (UNLIKELY((s = try_connect(&aires)) < 0)) {
		goto out;
	}

out:
	freeaddrinfo(aires);
	return s;
}


static void
rtr_cb(EV_P_ ev_io *w, int UNUSED(revents))
{
	char buf[1280];
	ssize_t nrd;
	ctx_t ctx = w->data;

	PMML_DEBUG("rtr_cb\n");
	if ((nrd = recv(w->fd, buf, sizeof(buf), 0)) <= 0) {
		switch (ctx->proto) {
		case PROTO_UDP:
			break;
		case PROTO_TCP:
			ctx->dst = -1;
			ev_io_shut(EV_A_ w);
			break;
		default:
			abort();
		}
	}
	return;
}

static void
sub_cb(EV_P_ ev_io *w, int UNUSED(revents))
{
	char buf[1280];
	char pck[1280];
	struct pmml_chnmsg_idn_s msg[1];
	ctx_t ctx = w->data;
	/* source */
	ssize_t nrd;
	const char *bp;
	/* dest */
	ssize_t npp = sizeof(pck);
	char *pp = pck;

	PMML_DEBUG("sub_cb\n");
	if ((bp = buf, nrd = recv(w->fd, buf, sizeof(buf), 0)) <= 0) {
		/* don't even bother */
		return;
	}

	/* let pmml_chck() know that we are up for identity retrieval */
	msg->chnmsg.flags = PMML_CHNMSG_HAS_IDN;
	pp = pck;
	/* process them all */
	for (ssize_t nch, npk;
	     LIKELY(nrd > 0 && (nch = pmml_chck((void*)msg, bp, nrd)) > 0);
	     bp += nch, nrd -= nch, pp += npk, npp -= npk) {
		/* check identity */
		uint32_t idn = read_router_id(msg->idn, msg->idz);
		char ident[12];

		/* repack if it's not a message already routed by us */
		if ((ctx->ident & idn) == ctx->ident) {
			/* it's us */
			PMML_DEBUG("loop detected\n");
			npk = 0U;
		} else if ((bang_ident(ident, sizeof(ident), ctx->ident | idn),
			    msg->idn = ident, msg->idz = sizeof(ident),
			    npk = pmml_pack(pp, npp, (void*)msg)) < 0) {
			npk = 0U;
		}
	}
	if (LIKELY(pp > pck && ctx->dst > 0)) {
		size_t npck = pp - pck;

		(void)send(ctx->dst, pck, npck, 0);
	}
	return;
}

static void
chk_cb(EV_P_ ev_check *w, int rev)
{
	static ev_io rtr[1];
	static time_t last_reco = 0U;
	ctx_t ctx = w->data;
	time_t now;

	PMML_DEBUG("chk_cb\n");
	if (UNLIKELY(rev & EV_CUSTOM)) {
		/* we're going down :( */
		ev_io_shut(EV_A_ rtr);
		return;
	} else if (UNLIKELY(ctx->dst < 0 && (now = time(NULL)) > last_reco)) {
		/* just to make sure we don't reconnect too often */
		last_reco = now;

		if ((ctx->dst = pmml_router_socket(ctx)) < 0) {
			perror("cannot obtain pimmel router socket");
			return;
		}

		rtr->data = ctx;
		ev_io_init(rtr, rtr_cb, ctx->dst, EV_READ);
		ev_io_start(EV_A_ rtr);
	}
	return;
}

static void
sigall_cb(EV_P_ ev_signal *UNUSED(w), int UNUSED(revents))
{
	ev_unloop(EV_A_ EVUNLOOP_ALL);
	return;
}


#if defined __INTEL_COMPILER
# pragma warning (disable:593)
# pragma warning (disable:181)
#elif defined __GNUC__
# pragma GCC diagnostic ignored "-Wswitch"
# pragma GCC diagnostic ignored "-Wswitch-enum"
#endif /* __INTEL_COMPILER */
#include "pimmel-router-clo.h"
#include "pimmel-router-clo.c"
#if defined __INTEL_COMPILER
# pragma warning (default:593)
# pragma warning (default:181)
#elif defined __GNUC__
# pragma GCC diagnostic warning "-Wswitch"
# pragma GCC diagnostic warning "-Wswitch-enum"
#endif	/* __INTEL_COMPILER */

int
main(int argc, char *argv[])
{
	/* args */
	struct pimmel_args_info argi[1];
	/* use the default event loop unless you have special needs */
	struct ev_loop *loop;
	ev_signal sigint_watcher[1];
	ev_signal sigterm_watcher[1];
 	ev_io sub[1];
	ev_check chk[1];
	/* context we pass around */
	struct ctx_s ctx[1];
	/* business logic */
	int res = 0;
 
	/* parse the command line */
	if (pimmel_parser(argc, argv, argi)) {
		res = 1;
		goto out;
	} else if (argi->inputs_num < 1U) {
		pimmel_parser_print_help();
		res = 1;
		goto out;
	} else if (massage_conn(ctx, argi->inputs[argi->inputs_num - 1]) < 0) {
		res = 1;
		goto out;
	} else if (argi->daemonise_given && detach() < 0) {
		perror("daemonisation failed");
		res = 1;
		goto out;
	}

	/* fill in the rest of ctx */
	ctx->ident = make_router_id();

	/* initialise the main loop */
	loop = ev_default_loop(EVFLAG_AUTO);

	/* initialise a sig C-c handler */
	ev_signal_init(sigint_watcher, sigall_cb, SIGINT);
	ev_signal_start(EV_A_ sigint_watcher);
	ev_signal_init(sigterm_watcher, sigall_cb, SIGTERM);
	ev_signal_start(EV_A_ sigterm_watcher);

	/* set up the one end, sub to pmml network */
	{
		int s;

		if ((s = pmml_socket(PMML_SUB)) < 0) {
			perror("cannot initialise pimmel socket");
			res = 1;
			goto out;
		}

		sub->data = ctx;
		ev_io_init(sub, sub_cb, s, EV_READ);
		ev_io_start(EV_A_ sub);
	}

	/* subscribe to all interesting bits */
	for (unsigned int i = 0; i < argi->inputs_num - 1; i++) {
		(void)pmml_sub(sub->fd, argi->inputs[i]);
	}

	/* set up preparation */
	ctx->dst = -1;
	chk->data = ctx;
	ev_check_init(chk, chk_cb);
	ev_check_start(EV_A_ chk);

	/* now wait for events to arrive */
	ev_loop(EV_A_ 0);

	/* close the routed-to socket */
	chk_cb(EV_A_ chk, EV_CUSTOM);
	ev_check_stop(EV_A_ chk);

	/* unsubscribe from all channels */
	pmml_uns(sub->fd, NULL);

	/* and off */
	ev_io_stop(EV_A_ sub);
	pmml_close(sub->fd);

	/* destroy the default evloop */
	ev_default_destroy();

out:
	pimmel_parser_free(argi);
	return res;
}

/* pimmel-router.c ends here */
