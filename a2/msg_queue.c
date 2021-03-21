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
	
	// read write poll mutex
	mutex_t hz_mutex;

	// pointer of mutex for poll
	mutex_t *poll_mutex; 
	
	// head of linked list 
	list_head hz_node;

	// pointer of predicate for poll
	int *stg_three_trigger;

	// subscribed events
	int evt;

	// queue handle
	msg_queue_t queue;

	// conditional variable to check if buffer is occupied,when performing write
	cond_t hz_write_occupied;

	// conditional variable to check if buffer is occupied,when performing read
	cond_t hz_read_occupied;
	
	// list entry to traverse the linked list that used to track the subscribed events
	list_entry ent;
	

	// pointer of condition variable for poll
	cond_t *evt_wait;


} mq_backend;


// Init the message queue backend
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
	
	ring_buffer_destroy(&mq->buffer);
	//Cleanup remaining fields (synchronization primitives, etc.)
	mutex_destroy(&(mq->hz_mutex));
	cond_destroy(&(mq->hz_write_occupied));
	cond_destroy(&(mq->hz_read_occupied));
	list_destroy(&mq->hz_node);

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

// inform the predicate and the conditional variable to subscribe events
void subscribe_event(mq_backend *thread, size_t (*ptr)(ring_buffer *), int evt, int evt_flg, void *buffer, bool is_read, mq_backend *mq, bool is_update)
{
	if (((is_update && is_read && mq->no_readers) || (is_update && !is_read && mq->no_writers) || (!is_update && ptr(buffer) && !is_read) || (!is_update && is_read && ptr(buffer) > sizeof(size_t))) && (evt & evt_flg))
	{
		*(thread->stg_three_trigger) = 1;
		cond_signal(thread->evt_wait);
	}
}

// When performing msg_queue_close, if the linked list is not empty, 
// We need to update the subscribed the event through all entries in the linked
void update_ent(bool is_read, list_entry *head, list_entry *next, mq_backend *mq)
{
	if (next != head)
	{
		list_entry *root;
		list_for_each(root, &(mq->hz_node))
		{
			mq_backend *new_mq = container_of(root, mq_backend, ent);

			mutex_lock(new_mq->poll_mutex);
			// Make sure that Msg queue is not null
			if (new_mq->queue != MSG_QUEUE_NULL && new_mq)
			{
				size_t (*ptr)(ring_buffer *) = ring_buffer_free;
				int flags[4] = {MQPOLL_WRITABLE, MQPOLL_NOREADERS, MQPOLL_READABLE, MQPOLL_NOWRITERS};
				for (int i = 0; i < 4; i++)
				{
					is_read = i < 2;
					subscribe_event(new_mq, ptr, new_mq->evt, flags[i], &(mq->buffer), is_read, mq, true);
				}
			}
			mutex_unlock(new_mq->poll_mutex);
		}
	}
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
		update_ent(false, mq->hz_node.head.next, &(mq->hz_node.head), mq);
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

	update_ent(false, mq->hz_node.head.next, &(mq->hz_node.head), mq);
	*queue = MSG_QUEUE_NULL;
	mutex_unlock(&(mq->hz_mutex));
	return 0;
}

// When performing read or write, if the linked list is not empty, 
// We need to update the subscribed the event through all entries in the linked
void has_entry(bool is_read, list_entry *head, list_entry *next, mq_backend *mq)
{
	int handled_symbol = MQPOLL_WRITABLE;
	if (!is_read)
	{
		handled_symbol = MQPOLL_READABLE;
	}
	if (next != head)
	{
		list_entry *root;
		list_for_each(root, &(mq->hz_node))
		{
			mq_backend *new_mq = container_of(root, mq_backend, ent);
			// Make sure that Msg queue is not null
			mutex_lock(new_mq->poll_mutex);
			if (new_mq->queue != MSG_QUEUE_NULL && new_mq)
			{
				subscribe_event(new_mq, ring_buffer_free, new_mq->evt, handled_symbol, &(mq->buffer), is_read, mq, false);
			}
			mutex_unlock(new_mq->poll_mutex);
		}
	}
	mutex_unlock(&(mq->hz_mutex));
}

// Helper function for error checking when performing read and write action
// Return 0 if no error, return -1 if error occur.
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

// Error checking for read & write operation
// Return true if error occurs
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

// Helper function for read
// Update error code and check buffer availability
// Return true if error occurs
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

// This function is triggered when an rb operation inolves unlocking
// Return true if unlock;
bool unlock_op(mutex_t *mutex_lk, bool (*ptr)(ring_buffer *, void *, size_t), ring_buffer *rb, void *buffer, size_t length)
{
	if (!ptr(rb, buffer, length))
	{
		mutex_unlock(mutex_lk);
		return true;
	}
	return false;
}

// Get size of size_t 
// Common function for read & write
size_t get_size(mutex_t *mutex_lk)
{
	mutex_lock(mutex_lk);
	return sizeof(size_t);
}

// Perform Read & write operation to the message and its length
// Brodcast the correponding cond_t to wake up all the threads
// that blocked on it
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
		//Subscribe Event in all entry
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
		// Perform write message and update msg length
		if (rw_msg(mq, false, sizeof(size_t), length, (void *)buffer, &(mq->hz_write_occupied)) == -1)
		{
			return -1;
		}
		//Subscribe Event in all entry
		has_entry(false, &(mq->hz_node.head), mq->hz_node.head.next, mq);
		err = 0;
	}
	return err;
}


// Report Error if for a non-reader/non-writer handle, 
// non-corresponding event flag is triggerd
bool check_EINVAL(mq_backend *mq, size_t i, msg_queue_pollfd *fds, int mq_flag, mutex_t *wait_mutex)
{
	if ((fds[i].events & !(MQPOLL_READABLE || MQPOLL_WRITABLE || MQPOLL_NOREADERS || MQPOLL_NOWRITERS)) || ((fds[i].events & MQPOLL_READABLE) && !((mq_flag & MSG_QUEUE_READER))) || ((fds[i].events & MQPOLL_WRITABLE) && !(mq_flag & MSG_QUEUE_WRITER)))
	{
		errno = EINVAL;
		report_error("msg_queue_poll");
		mutex_unlock(wait_mutex);
		mutex_unlock(&(mq->hz_mutex));
		return true;
	}
	return false;
}


// Init the event thread attribute
mq_backend init_evt_thread(mq_backend mq, cond_t *evt_wait, int events, msg_queue_t q, mutex_t *wait_mutex, int *stg_three_trigger)
{
	mq.queue = q;
	mq.evt = events;
	mq.poll_mutex = wait_mutex;
	mq.evt_wait = evt_wait;
	mq.stg_three_trigger = stg_three_trigger;
	return mq;
}


// Handle subscription event for reader and writer given handle symbol
// Handle symbol is either MSG_QUEUE_READER , MSG_QUEUE_WRITER 
// indicating the handle is reader or writer
// reader subscribe -> MQPOLL_NOWRITERS;
// writer subscribe->MQPOLL_NOREADERS;
mq_backend rw_subscribe(int flag, int handle_symbol, int subscription, mq_backend thread)
{
	if (handle_symbol & flag)
		thread.evt |= subscription;
	return thread;
}

// Update revent if corresponding hanldes(read/write) is closed,
// or event flag match message status(wait to be read/write) 
bool update_revent_rw_enable(size_t (*ptr)(ring_buffer *), int handle_symbol, int index, mq_backend *data, msg_queue_pollfd *mg_fd, ring_buffer *buffer, int closed_rw, bool is_read)
{
	if ((data[index].evt & handle_symbol) && ((ptr(buffer) && is_read) || (!is_read && ptr(buffer) > sizeof(size_t)) || closed_rw))
	{
		mg_fd[index].revents |= handle_symbol;
		return true;
	}

	return false;
}


// Set revent to corresponding handle symbol 
// if all read or write handles are closed 
bool update_revent_rw_closed(int handle_symbol, int index, mq_backend *data, msg_queue_pollfd *mg_fd, int closed_rw)
{
	if (closed_rw && (data[index].evt & handle_symbol))
	{
		mg_fd[index].revents |= handle_symbol;
		return true;
	}

	return false;
}

// Helper funciton for Stage 1 and 3 to check if any of the requested events on any of the 
// message queues have already been triggered. 
int check_triggered_events(void (*lst_op)(list_head *, list_entry *), cond_t *evt_wait, int *stg_three_trigger, mq_backend *data, size_t nfds, msg_queue_pollfd *mg_fd, mutex_t *wait_mutex)
{
	unsigned int k = 0;
	// Number of time for updating revent
	unsigned int num_updated_revent = 0;
	int triggered_events = 0;
	while (k < nfds)
	{
		// Ensure that queue is not NULL
		if (mg_fd[k].queue != MSG_QUEUE_NULL)
		{
			mq_backend *mq = get_backend(mg_fd[k].queue);
			//Put all mutex on wait
			mutex_lock(&(mq->hz_mutex));
			mutex_lock(wait_mutex);
			int evt_flag[2] = {MQPOLL_NOWRITERS, MQPOLL_NOREADERS};

			data[k] = init_evt_thread(data[k], evt_wait, mg_fd[k].events, mg_fd[k].queue, wait_mutex, stg_three_trigger);
			int flg = get_flags((mg_fd + k)->queue);
			int queue_flag[2] = {MSG_QUEUE_READER, MSG_QUEUE_WRITER};
			// Handle subscription event for reader and writer given handle symbol
			for (int c = 0; c < 2; c++)
			{
				data[k] = rw_subscribe(flg, queue_flag[c], evt_flag[c], data[k]);
			}
			
			//check EINVAL.
			if (check_EINVAL(mq, k, mg_fd, flg, wait_mutex))
				return -1;

			// Set revent based on the event flag, MQPOLL_READABLE,MQPOLL_WRITABLE,MQPOLL_NOREADERS and MQPOLL_NOWRITERS
			// Record the number of time that revent update
			num_updated_revent += (int)update_revent_rw_enable(ring_buffer_used, MQPOLL_READABLE, k, data, mg_fd, &(mq->buffer), mq->no_writers, true);
			num_updated_revent += (int)update_revent_rw_enable(ring_buffer_free, MQPOLL_WRITABLE, k, data, mg_fd, &(mq->buffer), mq->no_readers, false);
			mg_fd[k].revents = num_updated_revent > 0 ? mg_fd[k].revents : 0;
			num_updated_revent += (int)update_revent_rw_closed(evt_flag[1], k, data, mg_fd, mq->no_readers);
			num_updated_revent += (int)update_revent_rw_closed(evt_flag[0], k, data, mg_fd, mq->no_writers);
			triggered_events += num_updated_revent > 0 ? 1 : 0;
			lst_op(&(mq->hz_node), &(data[k].ent));
			//unlock mutex
			mutex_unlock(wait_mutex);
			mutex_unlock(&(mq->hz_mutex));
		}
		else
			mg_fd[k].revents = 0;
		++k;
	}

	return triggered_events;
}



// STAGE 1 Handler:
// Check if any of the requested events on any of the 
// message queues have already been triggered. 
int stage_one_handler(cond_t *evt_wait, int *stg_three_trigger, mq_backend *data, size_t nfds, msg_queue_pollfd *mg_fd, mutex_t *wait_mutex)
{
	int triggered_events = check_triggered_events(list_add_tail, evt_wait, stg_three_trigger, data, nfds, mg_fd, wait_mutex);
	if (triggered_events)
	{
		unsigned int k = 0;
		while (k < nfds)
		{
			if (mg_fd[k].queue == MSG_QUEUE_NULL)
			{

				mg_fd[k].revents = 0; //Set revent to 0
			}
			else
			{ // Only enter critical section when MSG Queue is not null
				mq_backend *mq = get_backend(mg_fd[k].queue);
				mutex_lock(&(mq->hz_mutex));
				mutex_lock(wait_mutex);
				list_del(&(mq->hz_node), &(data[k].ent));
				mutex_unlock(wait_mutex);
				mutex_unlock(&(mq->hz_mutex));
			}
			++k;
		}

		cond_destroy(evt_wait);
		mutex_destroy(wait_mutex);
	}

	return triggered_events;
}

//Entering Stage II
void stg_two_blocking(mutex_t *wait_mutex, cond_t *evt_wait, int *stg_three_trigger)
{
	mutex_lock(wait_mutex);
	//If none of the requested events are already triggered
	while (!(*stg_three_trigger))
	{
		//Block until another thread triggers an event
		cond_wait(evt_wait, wait_mutex);
	}
	mutex_unlock(wait_mutex);
}

// Handling Stage III
// Determine what events have been triggered, and 
// return the number of message queues with any triggered events. 
int stg_three_check_queue(void (*lst_op)(list_head *, list_entry *), mutex_t *wait_mutex, mq_backend *data, size_t nfds, msg_queue_pollfd *mg_fd)
{
	unsigned int k = 0;
	// Number of time for updating revent
	unsigned int num_updated_revent = 0;
	int msg_q_triggered = 0;
	while (k < nfds)
	{
		// Ensure that queue is not NULL
		if (mg_fd[k].queue != MSG_QUEUE_NULL)
		{
			mq_backend *mq = get_backend(mg_fd[k].queue);
			//Put all mutex on wait
			mutex_lock(&(mq->hz_mutex));
			mutex_lock(wait_mutex);
			int evt_flag[2] = {MQPOLL_NOWRITERS, MQPOLL_NOREADERS};
			// Set revent based on the event flag, MQPOLL_READABLE,MQPOLL_WRITABLE,MQPOLL_NOREADERS and MQPOLL_NOWRITERS
			// Record the number of time that revent update
			num_updated_revent += (int)update_revent_rw_enable(ring_buffer_used, MQPOLL_READABLE, k, data, mg_fd, &(mq->buffer), mq->no_writers, true);
			num_updated_revent += (int)update_revent_rw_enable(ring_buffer_free, MQPOLL_WRITABLE, k, data, mg_fd, &(mq->buffer), mq->no_readers, false);
			mg_fd[k].revents = num_updated_revent > 0 ? mg_fd[k].revents : 0;
			num_updated_revent += (int)update_revent_rw_closed(evt_flag[1], k, data, mg_fd, mq->no_readers);
			num_updated_revent += (int)update_revent_rw_closed(evt_flag[0], k, data, mg_fd, mq->no_writers);
			msg_q_triggered += num_updated_revent > 0 ? 1 : 0;
			lst_op(&(mq->hz_node), &(data[k].ent));
			//unlock mutex
			mutex_unlock(wait_mutex);
			mutex_unlock(&(mq->hz_mutex));
		}
		else
			mg_fd[k].revents = 0;
		++k;
	}

	return msg_q_triggered;
}

int msg_queue_poll(msg_queue_pollfd *fds, size_t nfds)
{

	//Define mutex & condition variable & mq_backend
	cond_t evt_wait;
	mutex_t wait_mutex;
	mq_backend data[nfds];
	int stg_three_trigger = 0;

	//if size is 0, report error
	if (nfds == 0)
	{
		errno = EINVAL;
		report_error("msg_queue_poll");
		return -1;
	}
	//If no error then init
	mutex_init(&wait_mutex);
	cond_init(&evt_wait);

	//Handling Stage 1
	int triggered_events = 0;
	triggered_events = stage_one_handler(&evt_wait, &stg_three_trigger, data, nfds, fds, &wait_mutex);
	//Check if STAGE 1 generate any error
	if (triggered_events <0)
	{
		return -1;
	}
	else if (triggered_events == 0)
	{

		//Handling Stage 2
		stg_two_blocking(&wait_mutex, &evt_wait, &stg_three_trigger);
		//Handling Stage 3
		triggered_events = stg_three_check_queue(list_del, &wait_mutex, data, nfds, fds);

		//Clear all mutex and conditional var
		cond_destroy(&evt_wait);
		mutex_destroy(&wait_mutex);
	}
	return triggered_events;
}
