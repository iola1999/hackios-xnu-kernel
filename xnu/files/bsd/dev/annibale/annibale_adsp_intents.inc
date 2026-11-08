/* Remote GLINK intents retain their advertised capacity until RX_DONE. */

static void glink_add_their_intent(struct glink_intent_pool *pool, uint32_t id, uint32_t size)
{
	if (size == 0) return;
	lck_mtx_lock(&glink_intent_lock);
	struct glink_remote_intent **slot = &pool->head;
	while (*slot != NULL) {
		if ((*slot)->id == id) {
			lck_mtx_unlock(&glink_intent_lock);
			return;
		}
		slot = &(*slot)->next;
	}
	/* Firmware advertises multiple size classes; all remain eligible for TX. */
	struct glink_remote_intent *intent = kalloc_type(struct glink_remote_intent, Z_WAITOK | Z_ZERO);
	if (intent != NULL) {
		intent->id = id;
		intent->size = size;
		intent->free = true;
		*slot = intent;
	} else {
		say("cannot record GLINK intent %u size %u\n", id, size);
	}
	lck_mtx_unlock(&glink_intent_lock);
}

static void glink_complete_their_intent(struct glink_intent_pool *pool, uint32_t id, bool reuse)
{
	lck_mtx_lock(&glink_intent_lock);
	for (struct glink_remote_intent **slot = &pool->head; *slot != NULL; slot = &(*slot)->next) {
		struct glink_remote_intent *intent = *slot;
		if (intent->id == id) {
			if (reuse) {
				intent->free = true;
			} else {
				*slot = intent->next;
				kfree_type(struct glink_remote_intent, intent);
			}
			break;
		}
	}
	lck_mtx_unlock(&glink_intent_lock);
}

static bool glink_take_their_intent(struct glink_intent_pool *pool, uint16_t our_cid, uint32_t need, uint32_t *id_out)
{
	uint64_t deadline = 0;
	for (;;) {
		lck_mtx_lock(&glink_intent_lock);
		for (struct glink_remote_intent *intent = pool->head; intent != NULL; intent = intent->next) {
			if (intent->free && intent->size >= need) {
				intent->free = false;
				*id_out = intent->id;
				lck_mtx_unlock(&glink_intent_lock);
				return true;
			}
		}
		lck_mtx_unlock(&glink_intent_lock);
		if (our_cid == 0) return false;
		if (deadline == 0) {
			glink_send_cmd2(GLINK_RX_INTENT_REQ, our_cid, need);
			deadline = now_us() + 50000ULL;
		} else if (now_us() >= deadline) {
			return false;
		}
		/* Receive handling acquires the intent lock while registering replies. */
		glink_poll_rx();
		sleep_us(200);
	}
}
