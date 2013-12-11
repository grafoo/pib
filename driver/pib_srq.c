/*
 * Copyright (c) 2013 Minoru NAKAMURA <nminoru@nminoru.jp>
 *
 * This code is licenced under the GPL version 2 or BSD license.
 */
#include <linux/module.h>
#include <linux/init.h>

#include "pib.h"


static volatile int post_srq_recv_counter; /* これは厳密でなくてもよい */


static int pib_ib_srq_attr_is_ok(const struct pib_ib_dev *dev, const struct ib_srq_attr *attr)
{
	if ((attr->max_wr < 1)  || (dev->ib_dev_attr.max_srq_wr  < attr->max_wr))
		return 0;

	if ((attr->max_sge < 1) || (dev->ib_dev_attr.max_srq_sge < attr->max_sge))
		return 0;

	return 1;
}


struct ib_srq *pib_ib_create_srq(struct ib_pd *ibpd,
				 struct ib_srq_init_attr *init_attr,
				 struct ib_udata *udata)
{
	struct pib_ib_srq *srq;

	debug_printk("pib_ib_create_srq\n");

	if (!ibpd || !init_attr)
		return ERR_PTR(-EINVAL);

	if (!pib_ib_srq_attr_is_ok(to_pdev(ibpd->device), &init_attr->attr))
		return ERR_PTR(-EINVAL);

	srq = kmem_cache_zalloc(pib_ib_srq_cachep, GFP_KERNEL);
	if (!srq)
		return ERR_PTR(-ENOMEM);

	srq->ib_srq_attr = init_attr->attr;
	srq->ib_srq_attr.srq_limit = 0; /* srq_limit isn't set when ibv_craete_srq */

	sema_init(&srq->sem, 1);
	INIT_LIST_HEAD(&srq->recv_wqe_head);

	return &srq->ib_srq;
}


int pib_ib_destroy_srq(struct ib_srq *ibsrq)
{
	struct pib_ib_srq *srq;
	struct pib_ib_recv_wqe *recv_wqe, *next;

	debug_printk("pib_ib_destroy_srq\n");

	if (!ibsrq)
		return 0;

	srq = to_psrq(ibsrq);

	down(&srq->sem);
	list_for_each_entry_safe(recv_wqe, next, &srq->recv_wqe_head, list) {
		list_del_init(&recv_wqe->list);
		kmem_cache_free(pib_ib_recv_wqe_cachep, recv_wqe);
	}
	srq->nr_recv_wqe = 0;
	up(&srq->sem);

	kmem_cache_free(pib_ib_srq_cachep, to_psrq(ibsrq));

	return 0;
}


int pib_ib_modify_srq(struct ib_srq *ibsrq, struct ib_srq_attr *attr,
		      enum ib_srq_attr_mask attr_mask, struct ib_udata *udata)
{
	struct pib_ib_srq *srq;

	if (!ibsrq || !attr)
		return -EINVAL;

	srq = to_psrq(ibsrq);

	if (attr_mask & IB_SRQ_MAX_WR)
		return -EINVAL; /* @todo not yet implemented. */

	if (attr_mask & IB_SRQ_LIMIT) {
		srq->ib_srq_attr.srq_limit = attr->srq_limit;
		srq->issue_srq_limit = 0;
	}

	return 0;
}


int pib_ib_query_srq(struct ib_srq *ibsrq, struct ib_srq_attr *attr)
{
	struct pib_ib_srq *srq;

	if (!ibsrq || !attr)
		return -EINVAL;
	
	srq = to_psrq(ibsrq);

	*attr = srq->ib_srq_attr;

	return 0;
}


int pib_ib_post_srq_recv(struct ib_srq *ibsrq, struct ib_recv_wr *ibwr,
			 struct ib_recv_wr **bad_wr)
{
	int i, ret;
	struct pib_ib_dev *dev;
	struct pib_ib_recv_wqe *recv_wqe;
	struct pib_ib_srq *srq;
	u64 total_length = 0;

	if (!ibsrq || !ibwr)
		return -EINVAL;

	dev = to_pdev(ibsrq->device);

	srq = to_psrq(ibsrq);

next_wr:
	if ((ibwr->num_sge < 1) || (srq->ib_srq_attr.max_sge < ibwr->num_sge)) {
		ret = -EINVAL;
		goto err;
	}

	recv_wqe = kmem_cache_zalloc(pib_ib_recv_wqe_cachep, GFP_KERNEL);
	if (!recv_wqe) {
		ret = -ENOMEM;
		goto err;
	}

	INIT_LIST_HEAD(&recv_wqe->list);

	recv_wqe->wr_id   = ibwr->wr_id;
	recv_wqe->num_sge = ibwr->num_sge;

	for (i=0 ; i<ibwr->num_sge ; i++) {
		recv_wqe->sge_array[i] = ibwr->sg_list[i];

		if (pib_ib_get_behavior(dev, PIB_BEHAVIOR_ZERO_LEN_SGE_CONSIDER_AS_MAX_LEN))
			if (ibwr->sg_list[i].length == 0)
				ibwr->sg_list[i].length = PIB_IB_MAX_PAYLOAD_LEN;

		total_length += ibwr->sg_list[i].length;
	}

	if (PIB_IB_MAX_PAYLOAD_LEN < total_length) 
		; /* @todo */

	recv_wqe->total_length = (u32)total_length;

	down(&srq->sem);

	if (srq->ib_srq_attr.max_wr < srq->nr_recv_wqe + 1) {
		up(&srq->sem);
		kmem_cache_free(pib_ib_recv_wqe_cachep, recv_wqe);
		ret = -ENOMEM; /* @todo */
		goto err;
	}

	if (pib_ib_get_behavior(dev, PIB_BEHAVIOR_SRQ_SHUFFLE)) {
		/* shuffle WRs */
		if ((post_srq_recv_counter++ % 2) == 0)
			list_add_tail(&recv_wqe->list, &srq->recv_wqe_head);
		else
			list_add(&recv_wqe->list, &srq->recv_wqe_head);
	} else {
		/* in order */
		list_add_tail(&recv_wqe->list, &srq->recv_wqe_head);
	}

	srq->nr_recv_wqe++;
	up(&srq->sem);

	ibwr = ibwr->next;
	if (ibwr)
		goto next_wr;

	return 0;

err:
	if (bad_wr)
		*bad_wr = ibwr;

	return ret;
}


struct pib_ib_recv_wqe *
pib_util_get_srq(struct pib_ib_srq *srq)
{
	struct pib_ib_recv_wqe *recv_wqe = NULL;

	down(&srq->sem);

	if (list_empty(&srq->recv_wqe_head))
		goto skip;

	recv_wqe = list_first_entry(&srq->recv_wqe_head, struct pib_ib_recv_wqe, list);
	list_del_init(&recv_wqe->list);
	srq->nr_recv_wqe--;

	if ((srq->ib_srq_attr.srq_limit != 0) &&
	    (srq->issue_srq_limit == 0) &&
	    (srq->nr_recv_wqe < srq->ib_srq_attr.srq_limit)) {
		struct ib_event ev;

		srq->issue_srq_limit = 1;

		ev.event       = IB_EVENT_SRQ_LIMIT_REACHED;
		ev.device      = srq->ib_srq.device;
		ev.element.srq = &srq->ib_srq;

		local_bh_disable();		
		srq->ib_srq.event_handler(&ev, srq->ib_srq.srq_context);
		local_bh_enable();
	}

skip:
	up(&srq->sem);

	return recv_wqe;
}