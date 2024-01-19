//
// Copyright 2023 NanoMQ Team, Inc.
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//
#include "nng/supplemental/nanolib/ringbuffer.h"
#include "core/nng_impl.h"

static inline int ringBuffer_get_msgs(ringBuffer_t *rb, int count, nng_msg ***list)
{
	unsigned int i = 0;
	int j = 0;

	nng_msg **newList = nng_alloc(count * sizeof(nng_msg *));
	if (newList == NULL) {
		return -1;
	}

	for (i = rb->head; i < rb->size; i++) {
		i = i % rb->cap;
		nng_msg *msg = rb->msgs[i].data;
		nng_msg_set_proto_data(msg, NULL, (void *)(uintptr_t)rb->msgs[i].key);

		newList[j] = msg;

		j++;
		if (j == count) {
			*list = newList;
			return 0;
		}
	}

	nng_free(newList, sizeof(*newList));
	return -1;
}

static inline void ringBuffer_clean_msgs(ringBuffer_t *rb, int needFree)
{
	unsigned int i = 0;
	unsigned int count = 0;

	if (rb->msgs != NULL) {
		if (rb->size != 0) {
			i = rb->head;
			count = 0;
			while (count < rb->size) {
				if (rb->msgs[i].data != NULL) {
					if (needFree) {
						/* For nni_msg now */
						nng_msg_free(rb->msgs[i].data);
					}
					rb->msgs[i].data = NULL;
				}

				i = (i + 1) % rb->cap;
				count++;
			}
		}
	}

	rb->head = 0;
	rb->tail = 0;
	rb->size = 0;

	return;
}

static inline int ringBuffer_get_and_clean_msgs(ringBuffer_t *rb, unsigned int count, nng_msg ***list)
{
	int ret;

	if (rb == NULL || count <= 0 || list == NULL) {
		return -1;
	}

	if (count > rb->size) {
		return -1;
	}

	ret = ringBuffer_get_msgs(rb, count, list);
	if (ret != 0) {
		return -1;
	}

	ringBuffer_clean_msgs(rb, 0);

	return 0;
}

int ringBuffer_init(ringBuffer_t **rb,
					unsigned int cap,
					enum fullOption fullOp,
					unsigned long long expiredAt)
{
	ringBuffer_t *newRB;

	if (cap >= RINGBUFFER_MAX_SIZE) {
		log_error("Want to init a ring buffer which is greater than MAX_SIZE: %u\n", RINGBUFFER_MAX_SIZE);
		return -1;
	}

	if (fullOp >= RB_FULL_MAX) {
		log_error("fullOp is not valid: %d\n", fullOp);
		return -1;
	}

	newRB = (ringBuffer_t *)nng_alloc(sizeof(ringBuffer_t));
	if (newRB == NULL) {
		log_error("New ring buffer alloc failed\n");
		return -1;
	}

	newRB->msgs = (ringBufferMsg_t *)nng_alloc(sizeof(ringBufferMsg_t) * cap);
	if (newRB->msgs == NULL) {
		log_error("New ringbuffer messages alloc failed\n");
		nng_free(newRB, sizeof(*newRB));
		return -1;
	}

	newRB->head = 0;
	newRB->tail = 0;
	newRB->size = 0;
	newRB->cap = cap;

	newRB->expiredAt = expiredAt;
	newRB->fullOp = fullOp;
	newRB->files = NULL;

	newRB->enqinRuleList[0] = NULL;
	newRB->enqoutRuleList[0] = NULL;
	newRB->deqinRuleList[0] = NULL;
	newRB->deqoutRuleList[0] = NULL;

	newRB->enqinRuleListLen = 0;
	newRB->enqoutRuleListLen = 0;
	newRB->deqinRuleListLen = 0;
	newRB->deqoutRuleListLen = 0;

	nng_mtx_alloc(&newRB->ring_lock);

	*rb = newRB;

	return 0;
}

static inline int ringBufferRule_check(ringBuffer_t *rb,
									   ringBufferRule_t **list,
									   unsigned int len,
									   void *data,
									   int flag)
{
	int ret = 0;
	unsigned int i = 0;

	for (i = 0; i < len; i++) {
		ret = list[i]->match(rb, data, flag);
		if (ret != 0) {
			continue;
		}
		ret = list[i]->target(rb, data, flag);
		if (ret != 0) {
			return -1;
		}
	}

	return 0;
}

static inline int ringBuffer_rule_check(ringBuffer_t *rb, void *data, int flag)
{
	int ret;

	if (flag & ENQUEUE_IN_HOOK) {
		ret = ringBufferRule_check(rb, rb->enqinRuleList, rb->enqinRuleListLen, data, ENQUEUE_IN_HOOK);
		if (ret != 0) {
			return -1;
		}
	}
	if (flag & ENQUEUE_OUT_HOOK) {
		ret = ringBufferRule_check(rb, rb->enqoutRuleList, rb->enqoutRuleListLen, data, ENQUEUE_OUT_HOOK);
		if (ret != 0) {
			return -1;
		}
	}
	if (flag & DEQUEUE_IN_HOOK) {
		ret = ringBufferRule_check(rb, rb->deqinRuleList, rb->deqinRuleListLen, data, DEQUEUE_IN_HOOK);
		if (ret != 0) {
			return -1;
		}
	}
	if (flag & DEQUEUE_OUT_HOOK) {
		ret = ringBufferRule_check(rb, rb->deqoutRuleList, rb->deqoutRuleListLen, data, DEQUEUE_OUT_HOOK);
		if (ret != 0) {
			return -1;
		}
	}

	return 0;
}

static int write_msgs_to_file(ringBuffer_t *rb)
{
	int ret = 0;
	/* TODO: use parquet to write file */

	if (ret != 0) {
		log_error("write msgs to file failed! msg will be freed\n");
		ringBuffer_clean_msgs(rb, 1);
		return -1;
	}

	ringBufferFile_t *file = nng_alloc(sizeof(ringBufferFile_t));
	if (file == NULL) {
		log_error("alloc new file failed! no memory! msg will be freed\n");
		ringBuffer_clean_msgs(rb, 1);
		return -1;
	}

	file->keys = nng_alloc(sizeof(uint64_t) * rb->cap);
	if (file->keys == NULL) {
		log_error("alloc new file keys failed! no memory! msg will be freed\n");
		ringBuffer_clean_msgs(rb, 1);
		nng_free(file, sizeof(ringBufferFile_t));
		return -1;
	}
	for (int i = 0; i < rb->cap; i++) {
		file->keys[i] = rb->msgs[i].key;
	}

	file->path = nng_alloc(sizeof(char) * 256);
	if (file->path == NULL) {
		log_error("alloc new file path failed! no memory! msg will be freed\n");
		ringBuffer_clean_msgs(rb, 1);
		nng_free(file->keys, sizeof(uint64_t) * rb->cap);
		nng_free(file, sizeof(ringBufferFile_t));
		return -1;
	}

	/* TODO: Path will return by parquet */
	sprintf(file->path, "/tmp/nanomq/RB%d", nng_timestamp());

	cvector_push_back(rb->files, file);

	log_info("RB msgs write to : %s\n", file->path);

	ringBuffer_clean_msgs(rb, 1);

	return 0;
}

int ringBuffer_enqueue(ringBuffer_t *rb,
					   uint64_t key,
					   void *data,
					   unsigned long long expiredAt,
					   nng_aio *aio)
{
	int ret;
	int *list_len;

	nng_mtx_lock(rb->ring_lock);
	ret = ringBuffer_rule_check(rb, data, ENQUEUE_IN_HOOK);
	if (ret != 0) {
		nng_mtx_unlock(rb->ring_lock);
		return -1;
	}

	if (rb->size == rb->cap) {
		/* ringbuffer not allowed to overwrite */
		if (rb->overWrite == 0) {
			nng_mtx_unlock(rb->ring_lock);
			log_error("Ring buffer is full enqueue failed!!!\n");
			return -1;
		}
		/* get all msgs and clean ringbuffer */
		nni_msg **list = NULL;
		ret = ringBuffer_get_and_clean_msgs(rb, rb->cap, &list);
		if (ret != 0 || list == NULL) {
			log_error("Ring buffer is full and clean ringbuffer failed!\n");
			nng_mtx_unlock(rb->ring_lock);
			return -1;
		}
		/* Put list len in msg proto data */
		list_len = nng_alloc(sizeof(int));
		if (list_len == NULL) {
			nng_mtx_unlock(rb->ring_lock);
			log_error("alloc new list_len failed! no memory!\n");
			return -1;
		}
		*list_len = rb->cap;

		nng_msg *tmsg;
		ret = nng_msg_alloc(&tmsg, 0);
		if (ret != 0 || tmsg == NULL) {
			nng_mtx_unlock(rb->ring_lock);
			log_error("alloc new msg failed! no memory!\n");
			return -1;
		}
		// just use aio count : nni_aio_count(nni_aio *aio)
		nng_msg_set_proto_data(tmsg, NULL, (void *)list_len);
		nng_aio_set_msg(aio, tmsg);
		nng_aio_set_prov_data(aio, (void *)list);
//		if (rb->overWrite) {
//			/*
//			 * sizeof(*(void *)) can not compile on windows,
//			 * and sz of nng_free is unused.
//			 */
//			/* For nng_msg now */
//			nng_msg *msg = rb->msgs[rb->head].data;
//			/* Put older msg to aio, send to user */
//			/* keyp will free by user */
//			keyp = nng_alloc(sizeof(int));
//			if (keyp == NULL) {
//				nng_mtx_unlock(rb->ring_lock);
//				log_error("alloc new key failed! no memory!\n");
//				return -1;
//			}
//			nng_msg_set_proto_data(msg, NULL, keyp);
//			nng_aio_set_msg(aio, msg);
//
//			rb->msgs[rb->head].key = key;
//			rb->msgs[rb->head].data = data;
//			rb->msgs[rb->head].expiredAt = expiredAt;
//			rb->head = (rb->head + 1) % rb->cap;
//			rb->tail = (rb->tail + 1) % rb->cap;
//			nng_mtx_unlock(rb->ring_lock);
//			log_error("Ring buffer is full but overwrite the old data\n");
//			return 0;
//		} else {
//			nng_mtx_unlock(rb->ring_lock);
//			log_error("Ring buffer is full enqueue failed!!!\n");
//			return -1;
//		}
	}
	ringBufferMsg_t *msg = &rb->msgs[rb->tail];

	msg->key = key;
	msg->data = data;
	msg->expiredAt = expiredAt;

	rb->tail = (rb->tail + 1) % rb->cap;
	rb->size++;

	(void)ringBuffer_rule_check(rb, data, ENQUEUE_OUT_HOOK);

	nng_mtx_unlock(rb->ring_lock);
	return 0;
}

int ringBuffer_dequeue(ringBuffer_t *rb, void **data)
{
	int ret;
	nng_mtx_lock(rb->ring_lock);
	ret = ringBuffer_rule_check(rb, NULL, DEQUEUE_IN_HOOK);
	if (ret != 0) {
		nng_mtx_unlock(rb->ring_lock);
		return -1;
	}

	if (rb->size == 0) {
		log_error("Ring buffer is NULL dequeue failed\n");
		nng_mtx_unlock(rb->ring_lock);
		return -1;
	}

	*data = rb->msgs[rb->head].data;
	rb->head = (rb->head + 1) % rb->cap;
	rb->size = rb->size - 1;

	(void)ringBuffer_rule_check(rb, *data, DEQUEUE_OUT_HOOK);

	nng_mtx_unlock(rb->ring_lock);
	return 0;
}

static inline void ringBufferRuleList_release(ringBufferRule_t **list, unsigned int len)
{
	unsigned int i = 0;

	if (list == NULL) {
		return;
	}

	for (i = 0; i < len; i++) {
		if (list[i] != NULL) {
			nng_free(list[i], sizeof(sizeof(ringBufferRule_t)));
			list[i] = NULL;
		}
	}

	return;
}

int ringBuffer_release(ringBuffer_t *rb)
{
	unsigned int i = 0;
	unsigned int count = 0;

	if (rb == NULL) {
		return -1;
	}

	nng_mtx_lock(rb->ring_lock);
	if (rb->msgs != NULL) {
		if (rb->size != 0) {
			i = rb->head;
			count = 0;
			while (count < rb->size) {
				if (rb->msgs[i].data != NULL) {
					/* For nni_msg now */
					nng_msg_free(rb->msgs[i].data);
					rb->msgs[i].data = NULL;
				}
				i = (i + 1) % rb->cap;
				count++;
			}
		}
		nng_free(rb->msgs, sizeof(*rb->msgs));
	}

	if (rb->files != NULL) {
		for (int i = 0; i < cvector_size(rb->files); i++) {
			nng_free(rb->files[i]->keys, sizeof(uint64_t) * rb->cap);
			nng_free(rb->files[i]->path, sizeof(char) * 256);
			nng_free(rb->files[i], sizeof(ringBufferFile_t));
		}
		cvector_free(rb->files);
	}

	ringBufferRuleList_release(rb->enqinRuleList, rb->enqinRuleListLen);
	ringBufferRuleList_release(rb->deqinRuleList, rb->deqinRuleListLen);
	ringBufferRuleList_release(rb->enqoutRuleList, rb->enqoutRuleListLen);
	ringBufferRuleList_release(rb->deqoutRuleList, rb->deqoutRuleListLen);

	nng_mtx_unlock(rb->ring_lock);
	nng_mtx_free(rb->ring_lock);
	nng_free(rb, sizeof(*rb));

	return 0;
}

static inline int ringBufferRuleList_add(ringBufferRule_t **list, unsigned int *len,
										 int (*match)(ringBuffer_t *rb, void *data, int flag),
										 int (*target)(ringBuffer_t *rb, void *data, int flag))
{
	ringBufferRule_t *newRule = NULL;
	if (*len == RBRULELIST_MAX_SIZE) {
		log_error("Rule Buffer enqueue rule list is full!\n");
		return -1;
	}

	newRule = nng_alloc(sizeof(ringBufferRule_t));
	if (newRule == NULL) {
		log_error("alloc new rule failed! no memory!\n");
		return -1;
	}

	newRule->match = match;
	newRule->target = target;
	list[*len] = newRule;
	*len = *len + 1;
	return 0;
}

int ringBuffer_add_rule(ringBuffer_t *rb,
						int (*match)(ringBuffer_t *rb, void *data, int flag),
						int (*target)(ringBuffer_t *rb, void *data, int flag),
						int flag)
{
	int ret;

	if (rb == NULL || match == NULL || target == NULL || (flag & HOOK_MASK) == 0) {
		return -1;
	}

	nng_mtx_lock(rb->ring_lock);
	if (flag & ENQUEUE_IN_HOOK) {
		ret = ringBufferRuleList_add(rb->enqinRuleList, &rb->enqinRuleListLen, match, target);
		if (ret != 0) {
			nng_mtx_unlock(rb->ring_lock);
			return -1;
		}
	}

	if (flag & ENQUEUE_OUT_HOOK) {
		ret = ringBufferRuleList_add(rb->enqoutRuleList, &rb->enqoutRuleListLen, match, target);
		if (ret != 0) {
			nng_mtx_unlock(rb->ring_lock);
			return -1;
		}
	}

	if (flag & DEQUEUE_IN_HOOK) {
		ret = ringBufferRuleList_add(rb->deqinRuleList, &rb->deqinRuleListLen, match, target);
		if (ret != 0) {
			nng_mtx_unlock(rb->ring_lock);
			return -1;
		}
	}

	if (flag & DEQUEUE_OUT_HOOK) {
		ret = ringBufferRuleList_add(rb->deqoutRuleList, &rb->deqoutRuleListLen, match, target);
		if (ret != 0) {
			nng_mtx_unlock(rb->ring_lock);
			return -1;
		}
	}

	nng_mtx_unlock(rb->ring_lock);
	return 0;
}

int ringBuffer_search_msg_by_key(ringBuffer_t *rb, uint64_t key, nng_msg **msg)
{
	unsigned int i = 0;

	if (rb == NULL || msg == NULL) {
		return -1;
	}

	nng_mtx_lock(rb->ring_lock);
	for (i = rb->head; i < rb->size; i++) {
		i = i % rb->cap;
		if (rb->msgs[i].key == key) {
			*msg = rb->msgs[i].data;
			nng_mtx_unlock(rb->ring_lock);
			return 0;
		}
	}

	nng_mtx_unlock(rb->ring_lock);
	return -1;
}

/*
 * binary search, only for ringbuffer head is 0
 */
int ringBuffer_search_msgs_fuzz(ringBuffer_t *rb,
								uint64_t start,
								uint64_t end,
								uint32_t *count,
								nng_msg ***list)
{
	uint32_t low = 0;
	uint32_t high = 0;
	uint32_t mid = 0;
	uint32_t start_index = 0;
	uint32_t end_index = 0;

	nng_mtx_lock(rb->ring_lock);
	if (rb == NULL || rb->size == 0 || count == NULL || list == NULL) {
		nng_mtx_unlock(rb->ring_lock);
		log_error("ringbuffer is NULL or count is NULL or list is NULL\n");
		return -1;
	}

	if (start > end ||
		start > rb->msgs[rb->head + rb->size - 1].key ||
		end < rb->msgs[rb->head].key) {
		nng_mtx_unlock(rb->ring_lock);
		return -1;
	}

	if (rb->head != 0) {
		log_error("Ringbuffer head is not 0, can not use binary search\n");
		nng_mtx_unlock(rb->ring_lock);
		return -1;
	}

	low	= rb->head;
	high = rb->head + rb->size - 1;
	while (low < high) {
		mid = (low + high) / 2;
		if (rb->msgs[mid].key < start) {
			low = mid + 1;
		} else {
			high = mid;
		}
	}

	if (rb->msgs[high].key >= start) {
		start_index = high;
	} else {
		nng_mtx_unlock(rb->ring_lock);
		return -1;
	}

	low	= rb->head;
	high = rb->head + rb->size - 1;
	while (low < high) {
		mid = (low + high + 1) / 2;
		if (rb->msgs[mid].key <= end) {
			low = mid;
		} else {
			high = mid - 1;
		}
	}

	if (rb->msgs[low].key > end) {
		nng_mtx_unlock(rb->ring_lock);
		return -1;
	} else {
		end_index = low;
	}

	*count = end_index - start_index + 1;
	nng_msg **newList = nng_alloc((*count) * sizeof(nng_msg *));
	if (newList == NULL) {
		nng_mtx_unlock(rb->ring_lock);
		return -1;
	}

	for (uint32_t i = start_index; i <= end_index; i++) {
		nng_msg *msg = rb->msgs[i].data;
		if (msg == NULL) {
			nng_free(newList, sizeof(*newList));
			nng_mtx_unlock(rb->ring_lock);
			log_error("msg is NULL and some error occured\n");
			return -1;
		}
		nng_msg_set_proto_data(msg, NULL, (void *)(uintptr_t)rb->msgs[i].key);
		newList[i - start_index] = msg;
	}

	*list = newList;
	nng_mtx_unlock(rb->ring_lock);
	return 0;
}

int ringBuffer_search_msgs_by_key(ringBuffer_t *rb, uint64_t key, uint32_t count, nng_msg ***list)
{
	unsigned int i = 0;
	unsigned int j = 0;

	if (rb == NULL || count <= 0 || list == NULL) {
		return -1;
	}

	nng_mtx_lock(rb->ring_lock);
	if (count > rb->size) {
		nng_mtx_unlock(rb->ring_lock);
		return -1;
	}

	nng_msg **newList = nng_alloc(count * sizeof(nng_msg *));
	if (newList == NULL) {
		nng_mtx_unlock(rb->ring_lock);
		return -1;
	}

	for (i = rb->head; i < rb->size; i++) {
		i = i % rb->cap;
		if (rb->msgs[i].key == key) {
			for (j = 0; j < count; j++) {
				nng_msg *msg = rb->msgs[i].data;

				nng_msg_set_proto_data(msg, NULL, (void *)(uintptr_t)rb->msgs[i].key);

				newList[j] = msg;

				i = (i + 1) % rb->cap;
			}
			*list = newList;
			nng_mtx_unlock(rb->ring_lock);
			return 0;
		}
	}

	nng_mtx_unlock(rb->ring_lock);
	nng_free(newList, sizeof(*newList));
	return -1;
}
