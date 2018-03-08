//
// Copyright 2018 Staysail Systems, Inc. <info@staysail.tech>
// Copyright 2018 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include "core/nng_impl.h"

#ifdef NNG_PLATFORM_POSIX
#include "platform/posix/posix_aio.h"
#include "platform/posix/posix_pollq.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

// UDP support.

// If we can suppress SIGPIPE on send, please do so.
#ifdef MSG_NOSIGNAL
#define NNI_MSG_NOSIGNAL MSG_NOSIGNAL
#else
#define NNI_MSG_NOSIGNAL 0
#endif

struct nni_plat_udp {
	nni_posix_pollq_node udp_pitem;
	int                  udp_fd;
	nni_list             udp_recvq;
	nni_list             udp_sendq;
	nni_mtx              udp_mtx;
};

static void
nni_posix_udp_doclose(nni_plat_udp *udp)
{
	nni_aio *aio;

	while (((aio = nni_list_first(&udp->udp_recvq)) != NULL) ||
	    ((aio = nni_list_first(&udp->udp_sendq)) != NULL)) {
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, NNG_ECLOSED);
	}
	// Underlying socket left open until close API called.
}

static void
nni_posix_udp_dorecv(nni_plat_udp *udp)
{
	nni_aio * aio;
	nni_list *q = &udp->udp_recvq;
	// While we're able to recv, do so.
	while ((aio = nni_list_first(q)) != NULL) {
		struct iovec            iov[4];
		unsigned                niov;
		nni_iov *               aiov;
		struct sockaddr_storage ss;
		nng_sockaddr *          sa;
		struct msghdr           hdr;
		int                     rv  = 0;
		int                     cnt = 0;

		nni_aio_get_iov(aio, &niov, &aiov);

		for (unsigned i = 0; i < niov; i++) {
			iov[i].iov_base = aiov[i].iov_buf;
			iov[i].iov_len  = aiov[i].iov_len;
		}
		hdr.msg_iov        = iov;
		hdr.msg_iovlen     = niov;
		hdr.msg_name       = &ss;
		hdr.msg_namelen    = sizeof(ss);
		hdr.msg_flags      = 0;
		hdr.msg_control    = NULL;
		hdr.msg_controllen = 0;

		if ((cnt = recvmsg(udp->udp_fd, &hdr, 0)) < 0) {
			if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
				// No data available at socket.  Leave
				// the AIO at the head of the queue.
				return;
			}
			rv = nni_plat_errno(errno);
		} else if ((sa = nni_aio_get_input(aio, 0)) != NULL) {
			// We need to store the address information.
			// It is incumbent on the AIO submitter to supply
			// storage for the address.
			nni_posix_sockaddr2nn(sa, (void *) &ss);
		}
		nni_list_remove(q, aio);
		nni_aio_finish(aio, rv, cnt);
	}
}

static void
nni_posix_udp_dosend(nni_plat_udp *udp)
{
	nni_aio * aio;
	nni_list *q = &udp->udp_sendq;

	// While we're able to send, do so.
	while ((aio = nni_list_first(q)) != NULL) {
		struct sockaddr_storage ss;

		int len;
		int rv  = 0;
		int cnt = 0;

		len = nni_posix_nn2sockaddr(&ss, nni_aio_get_input(aio, 0));
		if (len < 1) {
			rv = NNG_EADDRINVAL;
		} else {
			struct msghdr hdr;
			unsigned      niov;
			nni_iov *     aiov;
#ifdef NNG_HAVE_ALLOCA
			struct iovec *iov;
#else
			struct iovec iov[16];
#endif

			nni_aio_get_iov(aio, &niov, &aiov);
#ifdef NNG_HAVE_ALLOCA
			if (niov > 64) {
				rv = NNG_EINVAL;
			} else {
				iov = alloca(niov * sizeof(*iov));
			}
#else
			if (niov > NNI_NUM_ELEMENTS(iov)) {
				rv = NNG_EINVAL;
			}
#endif

			for (unsigned i = 0; i < niov; i++) {
				iov[i].iov_base = aiov[i].iov_buf;
				iov[i].iov_len  = aiov[i].iov_len;
			}
			hdr.msg_iov        = iov;
			hdr.msg_iovlen     = niov;
			hdr.msg_name       = &ss;
			hdr.msg_namelen    = len;
			hdr.msg_flags      = NNI_MSG_NOSIGNAL;
			hdr.msg_control    = NULL;
			hdr.msg_controllen = 0;

			if ((cnt = sendmsg(udp->udp_fd, &hdr, 0)) < 0) {
				if ((errno == EAGAIN) ||
				    (errno == EWOULDBLOCK)) {
					// Cannot send now, leave at head.
					return;
				}
				rv = nni_plat_errno(errno);
			}
		}

		nni_list_remove(q, aio);
		nni_aio_finish(aio, rv, cnt);
	}
}

// This function is called by the poller on activity on the FD.
static void
nni_posix_udp_cb(void *arg)
{
	nni_plat_udp *udp = arg;
	int           revents;
	int           events = 0;

	nni_mtx_lock(&udp->udp_mtx);
	revents = udp->udp_pitem.revents;
	if (revents & POLLIN) {
		nni_posix_udp_dorecv(udp);
	}
	if (revents & POLLOUT) {
		nni_posix_udp_dosend(udp);
	}
	if (revents & (POLLHUP | POLLERR | POLLNVAL)) {
		nni_posix_udp_doclose(udp);
	} else {
		if (!nni_list_empty(&udp->udp_sendq)) {
			events |= POLLOUT;
		}
		if (!nni_list_empty(&udp->udp_recvq)) {
			events |= POLLIN;
		}
		if (events) {
			nni_posix_pollq_arm(&udp->udp_pitem, events);
		}
	}
	nni_mtx_unlock(&udp->udp_mtx);
}

int
nni_plat_udp_open(nni_plat_udp **upp, nni_sockaddr *bindaddr)
{
	nni_plat_udp *          udp;
	int                     salen;
	struct sockaddr_storage sa;
	int                     rv;

	if ((salen = nni_posix_nn2sockaddr(&sa, bindaddr)) < 1) {
		return (NNG_EADDRINVAL);
	}

	// UDP opens can actually run synchronously.
	if ((udp = NNI_ALLOC_STRUCT(udp)) == NULL) {
		return (NNG_ENOMEM);
	}
	nni_mtx_init(&udp->udp_mtx);

	udp->udp_fd = socket(sa.ss_family, SOCK_DGRAM, IPPROTO_UDP);
	if (udp->udp_fd < 0) {
		rv = nni_plat_errno(errno);
		nni_mtx_fini(&udp->udp_mtx);
		NNI_FREE_STRUCT(udp);
		return (rv);
	}

	if (bind(udp->udp_fd, (void *) &sa, salen) != 0) {
		rv = nni_plat_errno(errno);
		(void) close(udp->udp_fd);
		nni_mtx_fini(&udp->udp_mtx);
		NNI_FREE_STRUCT(udp);
		return (rv);
	}
	udp->udp_pitem.fd   = udp->udp_fd;
	udp->udp_pitem.cb   = nni_posix_udp_cb;
	udp->udp_pitem.data = udp;

	(void) fcntl(udp->udp_fd, F_SETFL, O_NONBLOCK);

	nni_aio_list_init(&udp->udp_recvq);
	nni_aio_list_init(&udp->udp_sendq);

	if (((rv = nni_posix_pollq_init(&udp->udp_pitem)) != 0) ||
	    ((rv = nni_posix_pollq_add(&udp->udp_pitem)) != 0)) {
		(void) close(udp->udp_fd);
		nni_mtx_fini(&udp->udp_mtx);
		NNI_FREE_STRUCT(udp);
		return (rv);
	}

	*upp = udp;
	return (0);
}

void
nni_plat_udp_close(nni_plat_udp *udp)
{
	// We're no longer interested in events.
	nni_posix_pollq_fini(&udp->udp_pitem);

	nni_mtx_lock(&udp->udp_mtx);
	nni_posix_udp_doclose(udp);
	nni_mtx_unlock(&udp->udp_mtx);

	(void) close(udp->udp_fd);
	nni_mtx_fini(&udp->udp_mtx);
	NNI_FREE_STRUCT(udp);
}

void
nni_plat_udp_cancel(nni_aio *aio, int rv)
{
	nni_plat_udp *udp = nni_aio_get_prov_data(aio);

	nni_mtx_lock(&udp->udp_mtx);
	if (nni_aio_list_active(aio)) {
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, rv);
	}
	nni_mtx_unlock(&udp->udp_mtx);
}

void
nni_plat_udp_recv(nni_plat_udp *udp, nni_aio *aio)
{
	nni_mtx_lock(&udp->udp_mtx);
	if (nni_aio_start(aio, nni_plat_udp_cancel, udp) == 0) {
		nni_list_append(&udp->udp_recvq, aio);
		nni_posix_pollq_arm(&udp->udp_pitem, POLLIN);
	}
	nni_mtx_unlock(&udp->udp_mtx);
}

void
nni_plat_udp_send(nni_plat_udp *udp, nni_aio *aio)
{
	nni_mtx_lock(&udp->udp_mtx);
	if (nni_aio_start(aio, nni_plat_udp_cancel, udp) == 0) {
		nni_list_append(&udp->udp_sendq, aio);
		nni_posix_pollq_arm(&udp->udp_pitem, POLLOUT);
	}
	nni_mtx_unlock(&udp->udp_mtx);
}

int
nni_plat_udp_sockname(nni_plat_udp *udp, nni_sockaddr *sa)
{
	struct sockaddr_storage ss;
	socklen_t               sz;
	int                     rv;

	sz = sizeof(ss);
	if ((rv = getsockname(udp->udp_fd, (struct sockaddr *) &ss, &sz)) <
	    0) {
		return (nni_plat_errno(errno));
	}
	return (nni_posix_sockaddr2nn(sa, &ss));
}

#endif // NNG_PLATFORM_POSIX
