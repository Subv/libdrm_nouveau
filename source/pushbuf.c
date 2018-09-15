/*
 * Copyright 2012 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Ben Skeggs
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include "libdrm_lists.h"
#include "nouveau_drm.h"
#include "nouveau.h"
#include "private.h"

#include <switch.h>

#ifdef DEBUG
#	define TRACE(x...) printf("nouveau: " x)
#	define CALLED() TRACE("CALLED: %s\n", __PRETTY_FUNCTION__)
#else
#	define TRACE(x...)
# define CALLED()
#endif

struct nouveau_pushbuf_krec {
	struct nouveau_pushbuf_krec *next;
	struct drm_nouveau_gem_pushbuf_bo buffer[NOUVEAU_GEM_MAX_BUFFERS];
	struct drm_nouveau_gem_pushbuf_push push[NOUVEAU_GEM_MAX_PUSH];
	int nr_buffer;
	int nr_push;
};

struct nouveau_pushbuf_priv {
	struct nouveau_pushbuf base;
	struct nouveau_pushbuf_krec *list;
	struct nouveau_pushbuf_krec *krec;
	struct nouveau_list bctx_list;
	struct nouveau_bo *bo;
	uint32_t type;
	uint32_t *ptr;
	uint32_t *bgn;
	int bo_next;
	int bo_nr;
	struct nouveau_bo *bos[];
};

static inline struct nouveau_pushbuf_priv *
nouveau_pushbuf(struct nouveau_pushbuf *push)
{
	return (struct nouveau_pushbuf_priv *)push;
}

static int pushbuf_validate(struct nouveau_pushbuf *, bool);
static int pushbuf_flush(struct nouveau_pushbuf *);

static bool
pushbuf_kref_fits(struct nouveau_pushbuf *push, struct nouveau_bo *bo,
		  uint32_t *domains)
{
	CALLED();

	// Note: We assume we always have enough memory for the bo.
	return true;
}

static struct drm_nouveau_gem_pushbuf_bo *
pushbuf_kref(struct nouveau_pushbuf *push, struct nouveau_bo *bo,
	     uint32_t flags)
{
	CALLED();

	struct nouveau_pushbuf_priv *nvpb = nouveau_pushbuf(push);
	struct nouveau_pushbuf_krec *krec = nvpb->krec;
	struct nouveau_pushbuf *fpush;
	struct drm_nouveau_gem_pushbuf_bo *kref;
	uint32_t domains, domains_wr, domains_rd;

	domains = NOUVEAU_GEM_DOMAIN_GART;

	domains_wr = domains * !!(flags & NOUVEAU_BO_WR);
	domains_rd = domains * !!(flags & NOUVEAU_BO_RD);

	/* if buffer is referenced on another pushbuf that is owned by the
	 * same client, we need to flush the other pushbuf first to ensure
	 * the correct ordering of commands
	 */
	fpush = cli_push_get(push->client, bo);
	if (fpush && fpush != push)
		pushbuf_flush(fpush);

	kref = cli_kref_get(push->client, bo);
	if (kref) {
		/* possible conflict in memory types - flush and retry */
		if (!(kref->valid_domains & domains)) {
			return NULL;
		}

		kref->valid_domains &= domains;
		kref->write_domains |= domains_wr;
		kref->read_domains  |= domains_rd;
	} else {
		if (krec->nr_buffer == NOUVEAU_GEM_MAX_BUFFERS ||
		    !pushbuf_kref_fits(push, bo, &domains))
			return NULL;

		kref = &krec->buffer[krec->nr_buffer++];
		kref->bo = bo;
		kref->handle = bo->handle;
		kref->valid_domains = domains;
		kref->write_domains = domains_wr;
		kref->read_domains = domains_rd;
		kref->presumed.valid = 1;
		kref->presumed.offset = bo->offset;
		kref->presumed.domain = NOUVEAU_GEM_DOMAIN_GART;
		cli_kref_set(push->client, bo, kref, push);
		atomic_inc(&nouveau_bo(bo)->refcnt);
	}

	return kref;
}

#if 0
static uint32_t
pushbuf_krel(struct nouveau_pushbuf *push, struct nouveau_bo *bo,
	     uint32_t data, uint32_t flags, uint32_t vor, uint32_t tor)
{
	CALLED();
	// Unneeded
	return 0;
}
#endif

static void
pushbuf_dump(struct nouveau_pushbuf_krec *krec, int krec_id, int chid)
{
	struct drm_nouveau_gem_pushbuf_push *kpsh;
	struct drm_nouveau_gem_pushbuf_bo *kref;
	struct nouveau_bo *bo;
	uint32_t *bgn, *end;
	int i;

	TRACE("ch%d: krec %d pushes %d bufs %d\n", chid,
	    krec_id, krec->nr_push, krec->nr_buffer);

	kref = krec->buffer;
	for (i = 0; i < krec->nr_buffer; i++, kref++) {
		TRACE("ch%d: buf %08x %08x %08x %08x %08x\n", chid, i,
		    kref->handle, kref->valid_domains,
		    kref->read_domains, kref->write_domains);
	}

	kpsh = krec->push;
	for (i = 0; i < krec->nr_push; i++, kpsh++) {
		kref = krec->buffer + kpsh->bo_index;
		bo = kref->bo;
		bgn = (uint32_t *)((char *)bo->map + kpsh->offset);
		end = bgn + (kpsh->length /4);

		TRACE("ch%d: psh %08x %010llx %010llx\n", chid, kpsh->bo_index,
		    (unsigned long long)kpsh->offset,
		    (unsigned long long)(kpsh->offset + kpsh->length));
		while (bgn < end)
			TRACE("\t0x%08x\n", *bgn++);
	}

}

static int
pushbuf_submit(struct nouveau_pushbuf *push, struct nouveau_object *chan)
{
	CALLED();
	struct nouveau_pushbuf_priv *nvpb = nouveau_pushbuf(push);
	struct nouveau_pushbuf_krec *krec = nvpb->list;
	struct nouveau_device *dev = push->client->device;
	struct nouveau_device_priv *nvdev = nouveau_device(dev);
	struct drm_nouveau_gem_pushbuf_bo_presumed *info;
	struct drm_nouveau_gem_pushbuf_bo *kref;
	struct drm_nouveau_gem_pushbuf_push *kpsh;
	struct nouveau_fifo *fifo = chan->data;
	struct nouveau_bo *bo;
	struct nouveau_bo_priv *nvbo;
	int krec_id = 0;
	int ret = 0, i;
	Result rc;

	if (chan->oclass != NOUVEAU_FIFO_CHANNEL_CLASS)
		return -EINVAL;

	if (push->kick_notify)
		push->kick_notify(push);

	nouveau_pushbuf_data(push, NULL, 0, 0);

	while (krec && krec->nr_push) {
#if DEBUG
		pushbuf_dump(krec, krec_id++, fifo->channel);
#endif

		kpsh = krec->push;
		for (i = 0; i < krec->nr_push; i++, kpsh++) {
			kref = krec->buffer + kpsh->bo_index;
			bo = kref->bo;
			nvbo = nouveau_bo(bo);

			/* TODO: Do this in a better way when libnx 
			 * supports sending multiple cmdlists at the same time
			 */
			NvCmdList cmdlist;
			cmdlist.buffer = nvbo->buffer,
			cmdlist.offset = kpsh->offset / 4,
			cmdlist.num_cmds = kpsh->length / 4,
			cmdlist.max_cmds = kpsh->length / 4,
			cmdlist.parent = &nvdev->gpu;

			NvFence fence;
			rc = nvGpfifoSubmitCmdList(&nvdev->gpu.gpfifo, &cmdlist, 0, &fence);
			if (R_FAILED(rc)) {
				TRACE("nvGpfifo rejected pushbuf: %x\n", rc);
				pushbuf_dump(krec, krec_id++, fifo->channel);
				return -rc;
			}

			// TODO: Store the returned fence in the bo to wait on it later.
		}

		kref = krec->buffer;
		for (i = 0; i < krec->nr_buffer; i++, kref++) {
			bo = kref->bo;

			info = &kref->presumed;
			if (!info->valid) {
				bo->flags &= ~NOUVEAU_BO_APER;
				bo->flags |= NOUVEAU_BO_GART;
				bo->offset = info->offset;
			}

			if (kref->write_domains)
				nouveau_bo(bo)->access |= NOUVEAU_BO_WR;
			if (kref->read_domains)
				nouveau_bo(bo)->access |= NOUVEAU_BO_RD;
		}

		krec = krec->next;
	}

	return ret;
}

static int
pushbuf_flush(struct nouveau_pushbuf *push)
{
	CALLED();
	struct nouveau_pushbuf_priv *nvpb = nouveau_pushbuf(push);
	struct nouveau_pushbuf_krec *krec = nvpb->krec;
	struct drm_nouveau_gem_pushbuf_bo *kref;
	struct nouveau_bufctx *bctx, *btmp;
	struct nouveau_bo *bo;
	int ret = 0, i;

	ret = pushbuf_submit(push, push->channel);
	
	kref = krec->buffer;
	for (i = 0; i < krec->nr_buffer; i++, kref++) {
		bo = kref->bo;
		cli_kref_set(push->client, bo, NULL, NULL);
		if (push->channel)
			nouveau_bo_ref(NULL, &bo);
	}

	krec = nvpb->krec;
	krec->nr_buffer = 0;
	krec->nr_push = 0;

	DRMLISTFOREACHENTRYSAFE(bctx, btmp, &nvpb->bctx_list, head) {
		DRMLISTJOIN(&bctx->current, &bctx->pending);
		DRMINITLISTHEAD(&bctx->current);
		DRMLISTDELINIT(&bctx->head);
	}

	return ret;
}

static void
pushbuf_refn_fail(struct nouveau_pushbuf *push, int sref)
{
	CALLED();
	struct nouveau_pushbuf_priv *nvpb = nouveau_pushbuf(push);
	struct nouveau_pushbuf_krec *krec = nvpb->krec;
	struct drm_nouveau_gem_pushbuf_bo *kref;

	kref = krec->buffer + sref;
	while (krec->nr_buffer-- > sref) {
		struct nouveau_bo *bo = kref->bo;
		cli_kref_set(push->client, bo, NULL, NULL);
		nouveau_bo_ref(NULL, &bo);
		kref++;
	}
	krec->nr_buffer = sref;
}

static int
pushbuf_refn(struct nouveau_pushbuf *push, bool retry,
	     struct nouveau_pushbuf_refn *refs, int nr)
{
	CALLED();

	struct nouveau_pushbuf_priv *nvpb = nouveau_pushbuf(push);
	struct nouveau_pushbuf_krec *krec = nvpb->krec;
	struct drm_nouveau_gem_pushbuf_bo *kref;
	int sref = krec->nr_buffer;
	int ret = 0, i;

	for (i = 0; i < nr; i++) {
		kref = pushbuf_kref(push, refs[i].bo, refs[i].flags);
		if (!kref) {
			ret = -ENOSPC;
			break;
		}
	}

	if (ret) {
		pushbuf_refn_fail(push, sref);
		if (retry) {
			pushbuf_flush(push);
			nouveau_pushbuf_space(push, 0, 0, 0);
			return pushbuf_refn(push, false, refs, nr);
		}
	}

	return ret;
}

static int
pushbuf_validate(struct nouveau_pushbuf *push, bool retry)
{
	CALLED();
	struct nouveau_pushbuf_priv *nvpb = nouveau_pushbuf(push);
	struct nouveau_pushbuf_krec *krec = nvpb->krec;
	struct drm_nouveau_gem_pushbuf_bo *kref;
	struct nouveau_bufctx *bctx = push->bufctx;
	struct nouveau_bufref *bref;
	int relocs = bctx ? bctx->relocs * 2: 0;
	int sref, ret;

	ret = nouveau_pushbuf_space(push, relocs, relocs, 0);
	if (ret || bctx == NULL)
		return ret;

	sref = krec->nr_buffer;

	DRMLISTDEL(&bctx->head);
	DRMLISTADD(&bctx->head, &nvpb->bctx_list);

	DRMLISTFOREACHENTRY(bref, &bctx->pending, thead) {
		kref = pushbuf_kref(push, bref->bo, bref->flags);
		if (!kref) {
			ret = -ENOSPC;
			break;
		}
	}

	DRMLISTJOIN(&bctx->pending, &bctx->current);
	DRMINITLISTHEAD(&bctx->pending);

	if (ret) {
		pushbuf_refn_fail(push, sref);
		if (retry) {
			pushbuf_flush(push);
			return pushbuf_validate(push, false);
		}
	}

	return ret;
}

int
nouveau_pushbuf_new(struct nouveau_client *client, struct nouveau_object *chan,
		    int nr, uint32_t size, bool immediate,
		    struct nouveau_pushbuf **ppush)
{
	CALLED();
	struct nouveau_pushbuf_priv *nvpb;
	struct nouveau_pushbuf *push;
	int ret;

	nvpb = calloc(1, sizeof(*nvpb) + nr * sizeof(*nvpb->bos));
	if (!nvpb)
		return -ENOMEM;

	nvpb->krec = calloc(1, sizeof(*nvpb->krec));
	nvpb->list = nvpb->krec;
	if (!nvpb->krec) {
		free(nvpb);
		return -ENOMEM;
	}

	push = &nvpb->base;
	push->client = client;
	push->channel = immediate ? chan : NULL;
	push->flags = NOUVEAU_BO_RD | NOUVEAU_BO_GART | NOUVEAU_BO_MAP;
	nvpb->type = NOUVEAU_BO_GART;

	for (nvpb->bo_nr = 0; nvpb->bo_nr < nr; nvpb->bo_nr++) {
		ret = nouveau_bo_new(client->device, nvpb->type, 0, size,
				     NULL, &nvpb->bos[nvpb->bo_nr]);
		if (ret) {
			nouveau_pushbuf_del(&push);
			return ret;
		}
	}

	DRMINITLISTHEAD(&nvpb->bctx_list);
	*ppush = push;

	return 0;
}

void
nouveau_pushbuf_del(struct nouveau_pushbuf **ppush)
{
	CALLED();
	struct nouveau_pushbuf_priv *nvpb = nouveau_pushbuf(*ppush);
	if (nvpb) {
		struct drm_nouveau_gem_pushbuf_bo *kref;
		struct nouveau_pushbuf_krec *krec;
		while ((krec = nvpb->list)) {
			kref = krec->buffer;
			while (krec->nr_buffer--) {
				struct nouveau_bo *bo = kref++->bo;
				cli_kref_set(nvpb->base.client, bo, NULL, NULL);
				nouveau_bo_ref(NULL, &bo);
			}
			nvpb->list = krec->next;
			free(krec);
		}
		while (nvpb->bo_nr--)
			nouveau_bo_ref(NULL, &nvpb->bos[nvpb->bo_nr]);
		nouveau_bo_ref(NULL, &nvpb->bo);
		free(nvpb);
	}
	*ppush = NULL;
}

struct nouveau_bufctx *
nouveau_pushbuf_bufctx(struct nouveau_pushbuf *push, struct nouveau_bufctx *ctx)
{
	CALLED();

	struct nouveau_bufctx *prev = push->bufctx;
	push->bufctx = ctx;
	return prev;
}

int
nouveau_pushbuf_space(struct nouveau_pushbuf *push,
		      uint32_t dwords, uint32_t relocs, uint32_t pushes)
{
	CALLED();
	struct nouveau_pushbuf_priv *nvpb = nouveau_pushbuf(push);
	struct nouveau_pushbuf_krec *krec = nvpb->krec;
	struct nouveau_client *client = push->client;
	struct nouveau_bo *bo = NULL;
	bool flushed = false;
	int ret = 0;

	/* switch to next buffer if insufficient space in the current one */
	if (push->cur + dwords >= push->end) {
		if (nvpb->bo_next < nvpb->bo_nr) {
			nouveau_bo_ref(nvpb->bos[nvpb->bo_next++], &bo);
			if (nvpb->bo_next == nvpb->bo_nr && push->channel)
				nvpb->bo_next = 0;
		} else {
			ret = nouveau_bo_new(client->device, nvpb->type, 0,
					     nvpb->bos[0]->size, NULL, &bo);
			if (ret)
				return ret;
		}
	}

	/* make sure there's always enough space to queue up the pending
	 * data in the pushbuf proper
	 */
	pushes++;

	/* need to flush if we've run out of space on an immediate pushbuf,
	 * if the new buffer won't fit, or if the kernel push limits
	 * have been hit
	 */
	if ((bo && ( push->channel ||
		    !pushbuf_kref(push, bo, push->flags))) ||
	    krec->nr_push + pushes >= NOUVEAU_GEM_MAX_PUSH) {
		if (nvpb->bo && krec->nr_buffer)
			pushbuf_flush(push);
		flushed = true;
	}

	/* if necessary, switch to new buffer */
	if (bo) {
		ret = nouveau_bo_map(bo, NOUVEAU_BO_WR, push->client);
		if (ret)
			return ret;

		nouveau_pushbuf_data(push, NULL, 0, 0);
		nouveau_bo_ref(bo, &nvpb->bo);
		nouveau_bo_ref(NULL, &bo);

		nvpb->bgn = nvpb->bo->map;
		nvpb->ptr = nvpb->bgn;
		push->cur = nvpb->bgn;
		push->end = push->cur + (nvpb->bo->size / 4);
		push->end -= 2 + push->rsvd_kick; /* space for suffix */
	}

	pushbuf_kref(push, nvpb->bo, push->flags);
	return flushed ? pushbuf_validate(push, false) : 0;
}

void
nouveau_pushbuf_data(struct nouveau_pushbuf *push, struct nouveau_bo *bo,
		     uint64_t offset, uint64_t length)
{
	CALLED();

	struct nouveau_pushbuf_priv *nvpb = nouveau_pushbuf(push);
	struct nouveau_pushbuf_krec *krec = nvpb->krec;
	struct drm_nouveau_gem_pushbuf_push *kpsh;
	struct drm_nouveau_gem_pushbuf_bo *kref;

	if (bo != nvpb->bo && nvpb->bgn != push->cur) {
		nouveau_pushbuf_data(push, nvpb->bo,
				     (nvpb->bgn - nvpb->ptr) * 4,
				     (push->cur - nvpb->bgn) * 4);
		nvpb->bgn = push->cur;
	}

	if (bo) {
		kref = cli_kref_get(push->client, bo);
		assert(kref);
		kpsh = &krec->push[krec->nr_push++];
		kpsh->bo_index = kref - krec->buffer;
		kpsh->offset   = offset;
		kpsh->length   = length;
	}
}

int
nouveau_pushbuf_refn(struct nouveau_pushbuf *push,
		     struct nouveau_pushbuf_refn *refs, int nr)
{
	CALLED();
	return pushbuf_refn(push, true, refs, nr);
}

void
nouveau_pushbuf_reloc(struct nouveau_pushbuf *push, struct nouveau_bo *bo,
		      uint32_t data, uint32_t flags, uint32_t vor, uint32_t tor)
{
	CALLED();

	// Unimplemented
}

int
nouveau_pushbuf_validate(struct nouveau_pushbuf *push)
{
	CALLED();
	return pushbuf_validate(push, true);
}

uint32_t
nouveau_pushbuf_refd(struct nouveau_pushbuf *push, struct nouveau_bo *bo)
{
	CALLED();
	struct drm_nouveau_gem_pushbuf_bo *kref;
	uint32_t flags = 0;

	if (cli_push_get(push->client, bo) == push) {
		kref = cli_kref_get(push->client, bo);
		assert(kref);
		if (kref->read_domains)
			flags |= NOUVEAU_BO_RD;
		if (kref->write_domains)
			flags |= NOUVEAU_BO_WR;
	}

	return flags;
}

int
nouveau_pushbuf_kick(struct nouveau_pushbuf *push, struct nouveau_object *chan)
{
	CALLED();
	if (!push->channel)
		return pushbuf_submit(push, chan);
	pushbuf_flush(push);
	return pushbuf_validate(push, false);
}
