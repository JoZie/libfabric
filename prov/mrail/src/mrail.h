/*
 * Copyright (c) 2018 Intel Corporation, Inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#if HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <string.h>

#include <rdma/fabric.h>
#include <rdma/fi_atomic.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_eq.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_tagged.h>
#include "rdma/providers/fi_log.h"

#include <ofi.h>
#include <ofi_util.h>
#include <ofi_proto.h>
#include <ofi_prov.h>
#include <ofi_enosys.h>

#define MRAIL_MAJOR_VERSION 1
#define MRAIL_MINOR_VERSION 0

#define MRAIL_MAX_INFO 100

#define MRAIL_PASSTHROUGH_MODES		0
#define MRAIL_PASSTHROUGH_MR_MODES	(FI_MR_LOCAL | OFI_MR_BASIC_MAP)

#define MRAIL_RAIL_CQ_FORMAT	FI_CQ_FORMAT_TAGGED
typedef struct fi_cq_tagged_entry mrail_cq_entry_t;

extern struct fi_info mrail_info;
extern struct fi_provider mrail_prov;
extern struct util_prov mrail_util_prov;
extern struct fi_fabric_attr mrail_fabric_attr;

extern struct fi_info *mrail_info_vec[MRAIL_MAX_INFO];
extern size_t mrail_num_info;

struct mrail_match_attr {
	fi_addr_t addr;
	uint64_t tag;
};

struct mrail_unexp_msg_entry {
	struct dlist_entry 	entry;
	fi_addr_t 		addr;
	uint64_t 		tag;
	void			*context;
	char			data[];		/* completion entry */
};

struct mrail_recv_queue;

typedef struct mrail_unexp_msg_entry *
(*mrail_get_unexp_msg_entry_func)(struct mrail_recv_queue *recv_queue, void *context);

struct mrail_recv_queue {
	struct fi_provider 		*prov;
	struct dlist_entry 		recv_list;
	struct dlist_entry 		unexp_msg_list;
	dlist_func_t 			*match_recv;
	dlist_func_t 			*match_unexp;
	mrail_get_unexp_msg_entry_func	get_unexp_msg_entry;
};

struct mrail_recv *
mrail_match_recv_handle_unexp(struct mrail_recv_queue *recv_queue, uint64_t tag,
			      uint64_t addr, char *data, size_t len, void *context);

/* mrail protocol */
#define MRAIL_HDR_VERSION 1

struct mrail_hdr {
	uint8_t		version;
	uint8_t		op;
	uint8_t		padding[2];
	uint32_t	seq;
	uint64_t 	tag;
};

struct mrail_pkt {
	struct mrail_hdr	hdr;
	char 			data[];
};

/* TX & RX processing */

#define MRAIL_IOV_LIMIT	5

struct mrail_rx_buf {
	struct fid_ep		*rail_ep;
	struct mrail_pkt	pkt;
};

struct mrail_recv {
	struct iovec 		iov[MRAIL_IOV_LIMIT];
	void 			*desc[MRAIL_IOV_LIMIT];
	uint8_t 		count;
	void 			*context;
	uint64_t 		flags;
	uint64_t 		comp_flags;
	struct dlist_entry 	entry;
	fi_addr_t 		addr;
	uint64_t 		tag;
	uint64_t 		ignore;
};
DECLARE_FREESTACK(struct mrail_recv, mrail_recv_fs);

int mrail_cq_process_buf_recv(mrail_cq_entry_t *comp,
			      struct mrail_recv *recv);

struct mrail_fabric {
	struct util_fabric util_fabric;
	struct fi_info *info;
	struct fid_fabric **fabrics;
	size_t num_fabrics;
};

struct mrail_domain {
	struct util_domain util_domain;
	struct fi_info *info;
	struct fid_domain **domains;
	size_t num_domains;
	size_t addrlen;
};

struct mrail_av {
	struct util_av util_av;
	struct fid_av **avs;
	size_t *rail_addrlen;
	size_t num_avs;
	ofi_atomic32_t index;
};

typedef int (*mrail_cq_process_comp_func_t)(struct util_cq *cq,
					    mrail_cq_entry_t *comp);
struct mrail_cq {
	struct util_cq 			util_cq;
	struct fid_cq 			**cqs;
	size_t 				num_cqs;
	mrail_cq_process_comp_func_t	process_comp;
};

struct mrail_ep {
	struct util_ep 		util_ep;
	struct fi_info 		*info;
	struct {
		struct fid_ep 		*ep;
		struct fi_info		*info;
	} 			*rails;
	size_t 			num_eps;
	ofi_atomic32_t 		tx_rail;
	ofi_atomic32_t 		rx_rail;

	struct mrail_recv_fs	*recv_fs;
	struct mrail_recv_queue recv_queue;
	struct mrail_recv_queue trecv_queue;
};

struct mrail_addr_key {
	uint64_t base_addr;
	uint64_t key;
};

struct mrail_mr {
	struct fid_mr mr_fid;
	size_t num_mrs;
	struct {
		uint64_t base_addr;
		struct fid_mr *mr;
	} rails[];
};

int mrail_get_core_info(uint32_t version, const char *node, const char *service,
			uint64_t flags, const struct fi_info *hints,
			struct fi_info **core_info);
int mrail_fabric_open(struct fi_fabric_attr *attr, struct fid_fabric **fabric,
		       void *context);
int mrail_domain_open(struct fid_fabric *fabric, struct fi_info *info,
		       struct fid_domain **domain, void *context);
int mrail_cq_open(struct fid_domain *domain, struct fi_cq_attr *attr,
		   struct fid_cq **cq_fid, void *context);
int mrail_av_open(struct fid_domain *domain_fid, struct fi_av_attr *attr,
		   struct fid_av **av_fid, void *context);
int mrail_ep_open(struct fid_domain *domain, struct fi_info *info,
		   struct fid_ep **ep_fid, void *context);

static inline struct mrail_recv *
mrail_pop_recv(struct mrail_ep *mrail_ep)
{
	struct mrail_recv *recv;
	fastlock_acquire(&mrail_ep->util_ep.lock);
	recv = freestack_isempty(mrail_ep->recv_fs) ? NULL :
		freestack_pop(mrail_ep->recv_fs);
	fastlock_release(&mrail_ep->util_ep.lock);
	return recv;
}

static inline void
mrail_push_recv(struct mrail_ep *mrail_ep, struct mrail_recv *recv)
{
	fastlock_acquire(&mrail_ep->util_ep.lock);
	freestack_push(mrail_ep->recv_fs, recv);
	fastlock_release(&mrail_ep->util_ep.lock);
}

static inline struct fi_info *mrail_get_info_cached(char *name)
{
	struct fi_info *info;
	size_t i;

	for (i = 0; i < mrail_num_info; i++) {
		info = mrail_info_vec[i];
		if (!strcmp(info->fabric_attr->name, name))
			return info;
	}

	FI_WARN(&mrail_prov, FI_LOG_CORE, "Unable to find matching "
		"fi_info in mrail_info_vec for given fabric name\n");
	return NULL;
}

static inline int mrail_close_fids(struct fid **fids, size_t count)
{
	int ret, retv = 0;
	size_t i;

	for (i = 0; i < count; i++) {
		if (fids[i]) {
			ret = fi_close(fids[i]);
			if (ret)
				retv = ret;
		}
	}
	return retv;
}
