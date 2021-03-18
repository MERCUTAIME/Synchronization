/*
 * This code is provided solely for the personal and private use of students
 * taking the CSC369H course at the University of Toronto. Copying for purposes
 * other than this use is expressly prohibited. All forms of distribution of
 * this code, including but not limited to public repositories on GitHub,
 * GitLab, Bitbucket, or any other online platform, whether as given or with
 * any changes, are expressly prohibited.
 *
 * Authors: Alexey Khrabrov, Andrew Pelegris, Karen Reid
 *
 * All of the files in this directory and all subdirectories are:
 * Copyright (c) 2019, 2020 Karen Reid
 */

/**
 * CSC369 Assignment 2 - Message queue implementation.
 *
 * You may not use the pthread library directly. Instead you must use the
 * functions and types available in sync.h.
 */

#include <assert.h>
#include <errno.h>
#include <stdlib.h>

#include "errors.h"
#include "list.h"
#include "msg_queue.h"
#include "ring_buffer.h"

typedef struct event_thread
{
	list_entry ent;
	mutex_t *poll_mutex; //poll_mutex pointer
	cond_t *evt_wait;	 // condition variable for poll

	int *event_flag;

	int evt;

	msg_queue_t queue;

} event_thread;

// Message queue implementation backend
typedef struct mq_backend
{
	// Ring buffer for storing the messages
	ring_buffer buffer;

	// Reference count
	size_t refs;

	// Number of handles open for reads
	size_t readers;
	// Number of handles open for writes
	size_t writers;

	// Set to true when all the reader handles have been closed. Starts false
	// when they haven't been opened yet.
	bool no_readers;
	// Set to true when all the writer handles have been closed. Starts false
	// when they haven't been opened yet.
	bool no_writers;

	//TODO: add necessary synchronization primitives, as well as data structures
	//      needed to implement the msg_queue_poll() functionality
	mutex_t hz_mutex;

	list_head hz_node;

	cond_t hz_write_occupied;

	cond_t hz_read_occupied;
	int *stg_three_trigger;

} mq_backend;

static int mq_init(mq_backend *mq, size_t capacity)
{
	if (ring_buffer_init(&mq->buffer, capacity) < 0)
	{
		return -1;
	}

	mq->refs = 0;

	mq->readers = 0;
	mq->writers = 0;

	mq->no_readers = false;
	mq->no_writers = false;

	//Initialize remaining fields (synchronization primitives, etc.)
	cond_init(&(mq->hz_read_occupied));
	cond_init(&(mq->hz_write_occupied));
	list_init(&(mq->hz_node));
	mutex_init(&(mq->hz_mutex));

	return 0;
}

static void mq_destroy(mq_backend *mq)
{
	assert(mq->refs == 0);
	assert(mq->readers == 0);
	assert(mq->writers == 0);

	//Cleanup remaining fields (synchronization primitives, etc.)
	mutex_destroy(&(mq->hz_mutex));
	cond_destroy(&(mq->hz_write_occupied));
	cond_destroy(&(mq->hz_read_occupied));

	list_destroy(&mq->hz_node);
	ring_buffer_destroy(&mq->buffer);
}

#define ALL_FLAGS (MSG_QUEUE_READER | MSG_QUEUE_WRITER | MSG_QUEUE_NONBLOCK)

// Message queue handle is a combination of the pointer to the queue backend and
// the handle flags. The pointer is always aligned on 8 bytes - its 3 least
// significant bits are always 0. This allows us to store the flags within the
// same word-sized value as the pointer by ORing the pointer with the flag bits.

// Get queue backend pointer from the queue handle
static mq_backend *get_backend(msg_queue_t queue)
{
	mq_backend *mq = (mq_backend *)(queue & ~ALL_FLAGS);
	assert(mq);
	return mq;
}

// Get handle flags from the queue handle
static int get_flags(msg_queue_t queue)
{
	return (int)(queue & ALL_FLAGS);
}

// Create a queue handle for given backend pointer and handle flags
static msg_queue_t make_handle(mq_backend *mq, int flags)
{
	assert(((uintptr_t)mq & ALL_FLAGS) == 0);
	assert((flags & ~ALL_FLAGS) == 0);
	return (uintptr_t)mq | flags;
}

static msg_queue_t mq_open(mq_backend *mq, int flags)
{
	++mq->refs;

	if (flags & MSG_QUEUE_READER)
	{
		++mq->readers;
		mq->no_readers = false;
	}
	if (flags & MSG_QUEUE_WRITER)
	{
		++mq->writers;
		mq->no_writers = false;
	}

	return make_handle(mq, flags);
}

// Returns true if this was the last handle
static bool mq_close(mq_backend *mq, int flags)
{
	assert(mq->refs != 0);
	assert(mq->refs >= mq->readers);
	assert(mq->refs >= mq->writers);

	if ((flags & MSG_QUEUE_READER) && (--mq->readers == 0))
	{
		mq->no_readers = true;
	}
	if ((flags & MSG_QUEUE_WRITER) && (--mq->writers == 0))
	{
		mq->no_writers = true;
	}

	if (--mq->refs == 0)
	{
		assert(mq->readers == 0);
		assert(mq->writers == 0);
		return true;
	}
	return false;
}

msg_queue_t msg_queue_create(size_t capacity, int flags)
{
	if (flags & ~ALL_FLAGS)
	{
		errno = EINVAL;
		report_error("msg_queue_create");
		return MSG_QUEUE_NULL;
	}

	// Refuse to create a message queue without capacity for
	// at least one message (length + 1 byte of message data).
	if (capacity < (sizeof(size_t) + 1))
	{
		errno = EINVAL;
		report_error("msg_queue_create");
		return MSG_QUEUE_NULL;
	}

	mq_backend *mq = (mq_backend *)malloc(sizeof(mq_backend));
	if (!mq)
	{
		report_error("malloc");
		return MSG_QUEUE_NULL;
	}
	// Result of malloc() is always aligned on 8 bytes, allowing us to use the
	// 3 least significant bits of the handle to store the 3 bits of flags
	assert(((uintptr_t)mq & ALL_FLAGS) == 0);

	if (mq_init(mq, capacity) < 0)
	{
		// Preserve errno value that can be changed by free()
		int e = errno;
		free(mq);
		errno = e;
		return MSG_QUEUE_NULL;
	}

	return mq_open(mq, flags);
}

msg_queue_t msg_queue_open(msg_queue_t queue, int flags)
{
	if (!queue)
	{
		errno = EBADF;
		report_error("msg_queue_open");
		return MSG_QUEUE_NULL;
	}

	if (flags & ~ALL_FLAGS)
	{
		errno = EINVAL;
		report_error("msg_queue_open");
		return MSG_QUEUE_NULL;
	}

	mq_backend *mq = get_backend(queue);

	//Add necessary synchronization
	mutex_lock(&(mq->hz_mutex));
	msg_queue_t new_handle = mq_open(mq, flags);
	mutex_unlock(&(mq->hz_mutex));
	return new_handle;
}

int msg_queue_close(msg_queue_t *queue)
{
	if (!queue || !*queue)
	{
		errno = EBADF;
		report_error("msg_queue_close");
		return -1;
	}

	mq_backend *mq = get_backend(*queue);

	//TODO: add necessary synchronization

	mutex_lock(&(mq->hz_mutex));
	if (mq_close(mq, get_flags(*queue)))
	{
		// Closed last handle; destroy the queue
		mutex_unlock(&(mq->hz_mutex));
		mq_destroy(mq);
		free(mq);
		*queue = MSG_QUEUE_NULL;
		return 0;
	}

	//TODO: if this is the last reader (or writer) handle, notify all the writer
	//      (or reader) threads currently blocked in msg_queue_write() (or
	//      msg_queue_read()) and msg_queue_poll() calls for this queue.
	if (mq->no_readers)
	{
		cond_broadcast(&(mq->hz_read_occupied));
	}
	if (mq->no_writers)
	{
		cond_broadcast(&(mq->hz_write_occupied));
	}
	*queue = MSG_QUEUE_NULL;
	mutex_unlock(&(mq->hz_mutex));
	return 0;
}
void subscribe_event(event_thread *thread, size_t (*ptr)(ring_buffer *), int evt, int evt_flg, void *buffer, bool is_read, mq_backend *mq)
{
	if (((ptr(buffer) && !is_read) || (is_read && ptr(buffer) > sizeof(size_t))) && (evt & evt_flg))
	{
		*(thread->event_flag) = 1;
		*(mq->stg_three_trigger) = 1;
		cond_signal(thread->evt_wait);
	}
}
void has_entry(bool is_read, list_entry *head, list_entry *next, mq_backend *mq)
{
	int handled_symbol = MQPOLL_WRITABLE;
	if (!is_read)
	{
		handled_symbol = MQPOLL_READABLE;
	}
	if (next == head)
	{
		list_entry *root;
		list_for_each(root, &(mq->hz_node))
		{
			event_thread *data = container_of(root, event_thread, ent);

			mutex_lock(data->poll_mutex);
			if (data->queue != MSG_QUEUE_NULL && data)
			{
				subscribe_event(data, ring_buffer_free, data->evt, handled_symbol, &(mq->buffer), is_read, mq);
			}
		}
	}

	mutex_unlock(&(mq->hz_mutex));
}

/*Helper function for error checking when performing read and write action
Return 0 if no error, return -1 if error occur.
*/
int check_empty_queue(msg_queue_t queue, int msg_q_rw)
{
	if (!queue || !(get_flags(queue) & msg_q_rw))
	{
		errno = EBADF;
		if (msg_q_rw == MSG_QUEUE_READER)
		{
			report_error("msg_queue_read");
		}
		else
		{
			report_error("msg_queue_write");
		}
		return -1;
	}
	return 0;
}
/*
Error checking for read & write operation
Return true if error occurs
*/
bool err_checking_rw(bool is_read, int err_code, mutex_t *mutex_lk, bool condition)
{
	if (condition)
	{
		errno = err_code;
		if (is_read)
		{
			report_error("msg_queue_read");
		}
		else
		{
			report_error("msg_queue_write");
		}
		mutex_unlock(mutex_lk);
		return true;
	}
	return false;
}

bool check_buffer_availability(size_t (*ptr)(ring_buffer *), bool is_read, msg_queue_t queue, size_t msg_size, ring_buffer *buffer, mutex_t *mutex_lk, cond_t *occupied)
{
	bool err = false;
	// Check if there's available space in the buffer
	// If no space then wait
	while ((is_read && ptr(buffer) == 0) || (!is_read && msg_size > ptr(buffer)))
	{

		if (get_flags(queue) & MSG_QUEUE_NONBLOCK)
		{
			err_checking_rw(true, EAGAIN, mutex_lk, true);
			err = true;
			break;
		}
		//Available space exist for read/write
		cond_wait(occupied, mutex_lk);
	}
	return err;
}

/*	This function is triggered when an rb operation inolves unlocking
Return true if unlock;
*/
bool unlock_op(mutex_t *mutex_lk, bool (*ptr)(ring_buffer *, void *, size_t), ring_buffer *rb, void *buffer, size_t length)
{
	if (!ptr(rb, buffer, length))
	{
		mutex_unlock(mutex_lk);
		return true;
	}
	return false;
}

/*Get size of size_t 
Common function for read & write
*/
size_t get_size(mutex_t *mutex_lk)
{
	mutex_lock(mutex_lk);
	return sizeof(size_t);
}

/*
Perform Read & write operation to the message and its length
Brodcast the correponding cond_t to wake up all the threads
that blocked on it
*/
int rw_msg(mq_backend *mq, bool is_read, size_t size, size_t length, void *buf, cond_t *occupied)
{
	size_t sizes[2] = {size, length};
	void *buffers[2] = {&length, buf};
	const void *buffers_const[2] = {(const void *)&length, (const void *)buf};
	//Read the message and its length and unlock the lock correspondingly
	for (int i = 0; i < 2; i++)
	{
		//If read/write fail, return -1;
		if (is_read)
		{
			//Read the message and its length and unlock the lock correspondingly
			if (unlock_op(&(mq->hz_mutex), ring_buffer_read, &(mq->buffer), buffers[i], sizes[i]))
			{
				return -1;
			}
		}
		else
		{
			//Write the message and its length and unlock the lock correspondingly
			if (!ring_buffer_write(&(mq->buffer), buffers_const[i], sizes[i]))
			{
				mutex_unlock(&(mq->hz_mutex));
				return -1;
			}
		}
	}
	cond_broadcast(occupied);
	return 0;
}
ssize_t msg_queue_read(msg_queue_t queue, void *buffer, size_t length)
{
	int err_code = check_empty_queue(queue, MSG_QUEUE_READER);
	if (err_code != -1)
	{
		size_t size_msg;
		mq_backend *mq = get_backend(queue);
		size_t size = get_size(&(mq->hz_mutex));

		//If there's no writer and the ring buffer is empty
		//Return 0
		if (mq->no_writers && ring_buffer_used(&(mq->buffer)) == 0)
		{
			mutex_unlock(&(mq->hz_mutex));
			return 0;
		}

		//Update error code and check buffer availability
		bool err = check_buffer_availability(ring_buffer_used, true, queue, 0, &(mq->buffer), &(mq->hz_mutex), &(mq->hz_write_occupied));
		//Load the message length & Check whether that message is too long
		if (err || unlock_op(&(mq->hz_mutex), ring_buffer_peek, &(mq->buffer), &size_msg, size) || err_checking_rw(true, EMSGSIZE, &(mq->hz_mutex), size_msg > length))
		{
			return -1;
		}
		//Read msg and corresponding length
		if (rw_msg(mq, true, size, size_msg, buffer, &(mq->hz_read_occupied)) == -1)
		{
			return -1;
		}
		has_entry(true, &(mq->hz_node.head), mq->hz_node.head.next, mq);

		return size_msg;
	}
	return err_code;
}

int msg_queue_write(msg_queue_t queue, const void *buffer, size_t length)
{
	int err = check_empty_queue(queue, MSG_QUEUE_WRITER);
	if (err != -1)
	{

		mq_backend *mq = get_backend(queue);
		size_t buf_size = (mq->buffer).size;
		size_t msg_size = length + sizeof(size_t);
		int size = buf_size - length - get_size(&(mq->hz_mutex));
		//There are 3 error checking that need to perform
		//1. Write is valid or not 2. Message size > buffer size or not
		//3. Whether all write handler are closed
		int err_code[3] = {EINVAL, EMSGSIZE, EPIPE};
		bool conditions[3] = {length == 0, size < 0, mq->no_readers};
		for (int i = 0; i < 3; i++)
		{
			if (err_checking_rw(false, err_code[i], &(mq->hz_mutex), conditions[i]))
			{
				return -1;
			}
		}
		//Update error code and check buffer availability
		bool err = check_buffer_availability(ring_buffer_free, false, queue, msg_size, &(mq->buffer), &(mq->hz_mutex), &(mq->hz_read_occupied));
		if (err)
		{
			return -1;
		}
		if (rw_msg(mq, false, sizeof(size_t), length, (void *)buffer, &(mq->hz_write_occupied)) == -1)
		{
			return -1;
		}

		has_entry(false, &(mq->hz_node.head), mq->hz_node.head.next, mq);
		err = 0;
	}
	return err;
}

int msg_queue_poll(msg_queue_pollfd *fds, size_t nfds)
{
	//TODO
	(void)fds;
	(void)nfds;
	errno = ENOSYS;
	return -1;
}
