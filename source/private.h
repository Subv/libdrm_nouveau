#ifndef __NOUVEAU_LIBDRM_PRIVATE_H__
#define __NOUVEAU_LIBDRM_PRIVATE_H__

#include "libdrm_atomics.h"
#include "nouveau_drm.h"

#include "nouveau.h"

#include <switch.h>

struct nouveau_client_kref {
	struct drm_nouveau_gem_pushbuf_bo *kref;
	struct nouveau_pushbuf *push;
};

struct nouveau_client_priv {
	struct nouveau_client base;
	struct nouveau_client_kref *kref;
	unsigned kref_nr;
};

static inline struct nouveau_client_priv *
nouveau_client(struct nouveau_client *client)
{
	return (struct nouveau_client_priv *)client;
}

static inline struct drm_nouveau_gem_pushbuf_bo *
cli_kref_get(struct nouveau_client *client, struct nouveau_bo *bo)
{
	struct nouveau_client_priv *pcli = nouveau_client(client);
	struct drm_nouveau_gem_pushbuf_bo *kref = NULL;
	if (pcli->kref_nr > bo->handle)
		kref = pcli->kref[bo->handle].kref;
	return kref;
}

static inline struct nouveau_pushbuf *
cli_push_get(struct nouveau_client *client, struct nouveau_bo *bo)
{
	struct nouveau_client_priv *pcli = nouveau_client(client);
	struct nouveau_pushbuf *push = NULL;
	if (pcli->kref_nr > bo->handle)
		push = pcli->kref[bo->handle].push;
	return push;
}

static inline void
cli_kref_set(struct nouveau_client *client, struct nouveau_bo *bo,
	     struct drm_nouveau_gem_pushbuf_bo *kref,
	     struct nouveau_pushbuf *push)
{
	struct nouveau_client_priv *pcli = nouveau_client(client);
	if (pcli->kref_nr <= bo->handle) {
		pcli->kref = realloc(pcli->kref,
				     sizeof(*pcli->kref) * bo->handle * 2);
		while (pcli->kref_nr < bo->handle * 2) {
			pcli->kref[pcli->kref_nr].kref = NULL;
			pcli->kref[pcli->kref_nr].push = NULL;
			pcli->kref_nr++;
		}
	}
	pcli->kref[bo->handle].kref = kref;
	pcli->kref[bo->handle].push = push;
}

struct nouveau_bo_priv {
	struct nouveau_bo base;
	struct nouveau_list head;
	atomic_t refcnt;
	uint64_t map_handle;
	uint32_t name;
	uint32_t access;
	NvBuffer buffer;
};

static inline struct nouveau_bo_priv *
nouveau_bo(struct nouveau_bo *bo)
{
	return (struct nouveau_bo_priv *)bo;
}

struct nouveau_device_priv {
	struct nouveau_device base;
	int close;
	struct nouveau_list bo_list;
	uint32_t *client;
	int nr_client;
	bool have_bo_usage;
	int gart_limit_percent, vram_limit_percent;
	uint64_t allocspace_offset;
	Mutex lock;
	NvGpu gpu;
};

static inline struct nouveau_device_priv *
nouveau_device(struct nouveau_device *dev)
{
	return (struct nouveau_device_priv *)dev;
}

#endif
