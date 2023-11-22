#include "nng/supplemental/nanolib/ringbuffer.h"
#include "core/nng_impl.h"

int ringBuffer_init(struct ringBuffer **rb,
					unsigned int cap,
					unsigned int overWrite,
					unsigned long long expiredAt)
{
	struct ringBuffer *newRB;

	if (cap >= RINGBUFFER_MAX_SIZE) {
		log_error("Want to init a ring buffer which is greater than MAX_SIZE: %u\n", RINGBUFFER_MAX_SIZE);
		return -1;
	}

	newRB = (struct ringBuffer *)nng_alloc(sizeof(struct ringBuffer));
	if (newRB == NULL) {
		log_error("New ring buffer alloc failed\n");
		return -1;
	}

	newRB->msgs = (struct ringBufferMsg *)nng_alloc(sizeof(struct ringBufferMsg) * cap);
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
	newRB->overWrite = overWrite;

	*rb = newRB;

	return 0;
}

int ringBuffer_enqueue(struct ringBuffer *rb,
					   void *data,
					   unsigned long long expiredAt)
{
	if (rb->size == rb->cap) {
		if (rb->overWrite) {
			nng_free(rb->msgs[rb->head].data, sizeof(*rb->msgs[rb->head].data));
			rb->msgs[rb->head].data = data;
			rb->msgs[rb->head].expiredAt = expiredAt;
			rb->head = (rb->head + 1) % rb->cap;
			rb->tail = (rb->tail + 1) % rb->cap;
			log_error("Ring buffer is full but overwrite the old data\n");
			return 0;
		} else {
			log_error("Ring buffer is full enqueue failed!!!\n");
			return -1;
		}
	}

	struct ringBufferMsg *msg = &rb->msgs[rb->tail];

	msg->data = data;
	msg->expiredAt = expiredAt;

	rb->tail = (rb->tail + 1) % rb->cap;
	rb->size++;

	return 0;
}

int ringBuffer_dequeue(struct ringBuffer *rb, void **data)
{
	if (rb->size == 0) {
		log_error("Ring buffer is NULL dequeue failed\n");
		return -1;
	}

	*data = rb->msgs[rb->head].data;
	rb->head = (rb->head + 1) % rb->cap;
	rb->size = rb->size - 1;

	return 0;
}

int ringBuffer_release(struct ringBuffer *rb)
{
	unsigned int i = 0;
	unsigned int count = 0;

	if (rb == NULL) {
		return -1;
	}

	if (rb->msgs != NULL) {
		if (rb->size != 0) {
			i = rb->head;
			count = 0;
			while (count < rb->size) {
				nng_free(rb->msgs[i].data, sizeof(*(rb->msgs[i].data)));
				i = (i + 1) % rb->cap;
				count++;
			}
		}
		nng_free(rb->msgs, sizeof(*rb->msgs));
	}
	nng_free(rb, sizeof(*rb));

	return 0;
}
