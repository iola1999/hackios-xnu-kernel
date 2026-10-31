/*
 * Two tables the driver keeps its things in, split from lx_base.c: numbered things (Linux's "idr")
 * and things found by a key inside them (Linux's "rhashtable"). Both work under the one lock of
 * lx_os.h, like the rest of lx_base.c.
 */
#include <sys/param.h>
#include <sys/systm.h>

#include <linux/idr.h>
#include <linux/rhashtable.h>

#include <lx_os.h>
#include "lx.h"

/*
 * Numbered things (Linux's "idr": the driver numbers the frames it has handed to the firmware
 * and finds them again by number). Here a table that grows, hung where FreeBSD's keeps its tree.
 */
struct lx_ids {
	int size;
	void *things[];
};

void
idr_init(struct idr *idr)
{
	bzero(idr, sizeof(*idr));
}

void
idr_destroy(struct idr *idr)
{
	struct lx_ids *ids = (struct lx_ids *)idr->top;

	if (ids != NULL) {
		lx_os_free(ids, sizeof(*ids) + ids->size * sizeof(void *));
	}
	idr->top = NULL;
}

/* The lowest free number from `start` up to but not `end` (0 or less: no limit), or -ENOSPC. */
int
idr_alloc(struct idr *idr, void *thing, int start, int end, gfp_t gfp)
{
	struct lx_ids *ids, *more;
	int id, size;

	lx_os_lock();
	ids = (struct lx_ids *)idr->top;
	for (id = start < 0 ? 0 : start; ids != NULL && id < ids->size && ids->things[id] != NULL; id++) {
	}
	if (end > 0 && id >= end) {
		lx_os_unlock();
		return (-ENOSPC);
	}
	if (ids == NULL || id >= ids->size) {
		size = id + 64;
		lx_os_unlock();
		more = lx_os_alloc(sizeof(*more) + size * sizeof(void *));
		if (more == NULL) {
			return (-ENOMEM);
		}
		lx_os_lock();
		ids = (struct lx_ids *)idr->top;
		if (ids != NULL && ids->size >= size) {		/* (somebody else has made it grow meanwhile) */
			lx_os_unlock();
			lx_os_free(more, sizeof(*more) + size * sizeof(void *));
			return (idr_alloc(idr, thing, start, end, gfp));
		}
		more->size = size;
		if (ids != NULL) {
			memcpy(more->things, ids->things, ids->size * sizeof(void *));
		}
		idr->top = (struct idr_layer *)more;
		lx_os_unlock();
		if (ids != NULL) {
			lx_os_free(ids, sizeof(*ids) + ids->size * sizeof(void *));
		}
		return (idr_alloc(idr, thing, start, end, gfp));
	}
	ids->things[id] = thing;
	lx_os_unlock();
	return (id);
}

void *
idr_find(struct idr *idr, int id)
{
	struct lx_ids *ids;
	void *thing;

	lx_os_lock();
	ids = (struct lx_ids *)idr->top;
	thing = ids != NULL && id >= 0 && id < ids->size ? ids->things[id] : NULL;
	lx_os_unlock();
	return (thing);
}

void *
idr_remove(struct idr *idr, int id)
{
	struct lx_ids *ids;
	void *thing = NULL;

	lx_os_lock();
	ids = (struct lx_ids *)idr->top;
	if (ids != NULL && id >= 0 && id < ids->size) {
		thing = ids->things[id];
		ids->things[id] = NULL;
	}
	lx_os_unlock();
	return (thing);
}

int
idr_for_each(struct idr *idr, int (*function)(int id, void *p, void *data), void *data)
{
	int answer = 0;

	for (int id = 0; answer == 0; id++) {
		struct lx_ids *ids;
		void *thing;

		lx_os_lock();
		ids = (struct lx_ids *)idr->top;
		if (ids == NULL || id >= ids->size) {
			lx_os_unlock();
			break;
		}
		thing = ids->things[id];
		lx_os_unlock();
		if (thing != NULL) {
			answer = function(id, thing, data);
		}
	}
	return (answer);
}

/*
 * Things found by a key inside them (linux/rhashtable.h, ours). The chains are changed and walked
 * under the layer's lock; that a thing found stays there while it is used is the caller's
 * business, as it is under Linux.
 */
static struct rhash_head **
rht_chain(struct rhashtable *table, const void *key, unsigned length)
{
	uint32_t hash = 2166136261u;

	for (unsigned i = 0; i < length; i++) {
		hash = (hash ^ ((const uint8_t *)key)[i]) * 16777619u;
	}
	return (&table->chains[hash % nitems(table->chains)]);
}

int
rhashtable_init(struct rhashtable *table, const struct rhashtable_params *params)
{
	bzero(table, sizeof(*table));
	return (0);
}

void
rhashtable_destroy(struct rhashtable *table)
{
}

static void *
rht_find(struct rhashtable *table, const void *key, const struct rhashtable_params *params)
{
	for (struct rhash_head *head = *rht_chain(table, key, params->key_len); head != NULL; head = head->next) {
		uint8_t *thing = (uint8_t *)head - params->head_offset;

		if (memcmp(thing + params->key_offset, key, params->key_len) == 0) {
			return (thing);
		}
	}
	return (NULL);
}

void *
rhashtable_lookup_fast(struct rhashtable *table, const void *key, const struct rhashtable_params params)
{
	void *thing;

	lx_os_lock();
	thing = rht_find(table, key, &params);
	lx_os_unlock();
	return (thing);
}

void *
rhashtable_lookup_get_insert_fast(struct rhashtable *table, struct rhash_head *head, const struct rhashtable_params params)
{
	const uint8_t *key = (uint8_t *)head - params.head_offset + params.key_offset;
	struct rhash_head **chain;
	void *there;

	lx_os_lock();
	there = rht_find(table, key, &params);
	if (there == NULL) {
		chain = rht_chain(table, key, params.key_len);
		head->next = *chain;
		*chain = head;
	}
	lx_os_unlock();
	return (there);
}

int
rhashtable_remove_fast(struct rhashtable *table, struct rhash_head *head, const struct rhashtable_params params)
{
	const uint8_t *key = (uint8_t *)head - params.head_offset + params.key_offset;
	int error = -ENOENT;

	lx_os_lock();
	for (struct rhash_head **link = rht_chain(table, key, params.key_len); *link != NULL; link = &(*link)->next) {
		if (*link == head) {
			*link = head->next;
			error = 0;
			break;
		}
	}
	lx_os_unlock();
	return (error);
}

/* The numbered thing with the lowest number from *id on, and that number; or NULL. */
void *
idr_get_next(struct idr *idr, int *id)
{
	struct lx_ids *ids;
	void *thing = NULL;

	lx_os_lock();
	ids = (struct lx_ids *)idr->top;
	for (int i = *id < 0 ? 0 : *id; ids != NULL && i < ids->size; i++) {
		if (ids->things[i] != NULL) {
			thing = ids->things[i];
			*id = i;
			break;
		}
	}
	lx_os_unlock();
	return (thing);
}
