/* Remote GLINK intents retain their advertised capacity until RX_DONE. */

static void glink_add_their_intent(struct glink_intent_pool *pool, uint32_t id, uint32_t size)
{
	if (size == 0) return;
	lck_mtx_lock(&glink_intent_lock);
	for (unsigned i = 0; i < GLINK_MAX_INTENTS; i++) {
		if (pool->list[i].size != 0 && pool->list[i].id == id) {
			lck_mtx_unlock(&glink_intent_lock);
			return;
		}
	}
	for (unsigned i = 0; i < GLINK_MAX_INTENTS; i++) {
		if (pool->list[i].size == 0) {
			pool->list[i].id = id;
			pool->list[i].size = size;
			pool->list[i].free = true;
			break;
		}
	}
	lck_mtx_unlock(&glink_intent_lock);
}

static void glink_complete_their_intent(struct glink_intent_pool *pool, uint32_t id, bool reuse)
{
	lck_mtx_lock(&glink_intent_lock);
	for (unsigned i = 0; i < GLINK_MAX_INTENTS; i++) {
		if (pool->list[i].size != 0 && pool->list[i].id == id) {
			pool->list[i].free = reuse;
			if (!reuse) pool->list[i].size = 0;
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
		for (unsigned i = 0; i < GLINK_MAX_INTENTS; i++) {
			if (pool->list[i].free && pool->list[i].size >= need) {
				pool->list[i].free = false;
				*id_out = pool->list[i].id;
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
