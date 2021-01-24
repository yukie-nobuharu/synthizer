#pragma once

#include <atomic>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <tuple>
#include <thread>

#include "autoresetevent.h"

#include "config.hpp"
#include "memory.hpp"
#include "types.hpp"

namespace synthizer {
static_assert(std::atomic<std::size_t>::is_always_lock_free, "Unable to use mutex in size_t atomic inside AudioRing due to kernel calls.");

/*
 * An audio ring is a ringbuffer modelled after the DirectSound API. You tell it how many samples you want,
 * and it gives you a pair of pointers and lengths. You write to both lengths, then tell it you're done.
 * 
 * This is SPSC, and used for things like the streaming generator which need to run part of their logic in a background thread.
 * */

template<typename ELEM_T, typename data_provider_t>
class AudioRingBase {
	public:

	/*
	 * This is modelled after DirectSound.
	 * 
	 * The first pointer is always set to a non-null value.  The second pointer might be non-null, if the request wraps around the end of the buffer.
	 * 
	 * Note that if the user of this function always requests the same size on every call, that size is a factor of the ring size, and the user always fully commits, the second pointer is never non-NULL and the first pointer is the entire block.
	 * 
	 * If maxAvailable is true, return at least the size requested, but if more space is available then return all the space.
	 **/
	std::tuple<std::size_t, ELEM_T *, std::size_t, ELEM_T *>
	beginWrite(std::size_t requested, bool maxAvailable = false) {
		assert(maxAvailable == true || requested != 0);
		/* What if we requested a size that's bigger than the buffer? */
		assert(requested <= this->size());

		/*
		 * Explanation: the write pointer is always "behind" the read pointer, such that if write == read, then there is no data in the buffer.
		 * */
		std::size_t available;
		do {
			/* Get the number of bytes left, subtract from the size of the ring. */
			available = this->size() - this->samples_in_buffer.load(std::memory_order_relaxed);
			if (available < requested)
				this->read_end_event.wait();
		} while ( available < requested);

		/* Work out the sizes of the segments. */
		std::size_t size1 = 0, size2 = 0;
		std::size_t allocating = maxAvailable ? available : requested;
		this->pending_write_size = allocating;

		size1 = std::min(this->size() - write_pointer, allocating);
		if (size1 == allocating)
			return { size1, &this->data_provider[0] + write_pointer, 0, nullptr };

		size2 = allocating - size1;
		return { size1, &this->data_provider[0] + write_pointer, size2, &this->data_provider[0] };
	}

	/*
	 * It is possible to commit writes in chunks. To do so, specify amount here as a nonzero value.
	 *  */
	void endWrite(std::size_t amount) {
		assert(amount <= this->pending_write_size);
		this->write_pointer = (this->write_pointer + amount) % this->size();
		this->pending_write_size -= amount;
		this->samples_in_buffer.fetch_add(amount, std::memory_order_release);
	}

	/* Commit the entire write. */
	void endWrite() {
		endWrite(this->pending_write_size);
	}

	/*
	 * The read side. Like the write side, but never blocks.
	 * If maxAvailable = false and there isn't enough data in the buffer, returns null pointers and 0 sizes.
	 * Otherwise returns what's available even if it's less than the amount requested.
	 * */
	std::tuple<std::size_t, ELEM_T *, std::size_t, ELEM_T *>
	beginRead(std::size_t requested, bool maxAvailable = false) {
		assert(maxAvailable == true || requested != 0);
		/* What if we requested a size that's bigger than the buffer? */
		assert(requested <= this->size());

		std::size_t available = this->samples_in_buffer.load(std::memory_order_acquire);
		if (available == 0 || (available < requested && maxAvailable == false))
			return {0, nullptr, 0, nullptr};

		std::size_t allocating = maxAvailable ? available : requested;
		this->pending_read_size = allocating;
		std::size_t size1 = std::min(allocating, this->size() - read_pointer);
		ELEM_T *ptr1 = &this->data_provider[0] + read_pointer;
		if (size1 == allocating)
			return {size1, ptr1, 0, nullptr};

		std::size_t size2 = allocating - size1;
		ELEM_T *ptr2 = &this->data_provider[0];
		return {size1, ptr1, size2, ptr2};
	}

	void endRead(std::size_t amount) {
		assert(amount <= this->pending_read_size);
		this->read_pointer = (this->read_pointer + amount) % this->size();
		
		this->pending_read_size -= amount;
		this->samples_in_buffer.fetch_sub(amount, std::memory_order_release);
		this->read_end_event.signal();
	}

	void endRead() {
		endRead(this->pending_read_size);
	}

	std::size_t size() {
		return this->data_provider.size();
	}

	protected:
	AudioRingBase() = default;
	data_provider_t data_provider;

	private:
	std::size_t write_pointer = 0, read_pointer = 0;
	std::atomic<std::size_t> samples_in_buffer = 0;
	std::size_t pending_write_size = 0, pending_read_size = 0;
	AutoResetEvent read_end_event;
};

template<typename ELEM_T, std::size_t n>
class InlineAudioRingProvider {
	public:
	InlineAudioRingProvider(): data() {}

	constexpr std::size_t size() const {
		return this->data.size();
	}

	ELEM_T &
	operator[](std::size_t x) {
		return this->data[x];
	}

	private:
	std::array<ELEM_T, n> data;
};

/**
 * An inline allocated audio ring.
 * 
 * ELEM_T is second so it can be defaulted.
 * */
template<std::size_t size, typename ELEM_T = float>
class InlineAudioRing: public AudioRingBase<ELEM_T, InlineAudioRingProvider<ELEM_T, size>> {
	public:

	InlineAudioRing() {
	}
};

template<typename ELEM_T>
void audioRingDeleteArray(void *e) {
	ELEM_T *ptr = (ELEM_T *)e;
	delete[] ptr;
}

template<typename ELEM_T>
class AllocatedRingProvider {
	public:

	~AllocatedRingProvider() {
		deferredFreeCallback(audioRingDeleteArray<ELEM_T>, this->data);
	}
	
	std::size_t size() const {
		return this->_size;
	}

	void allocate(std::size_t n) {
		this->data  = new ELEM_T[n]();
		this->_size = n;
	}

	ELEM_T&
	operator[](std::size_t x) {
		return *(this->data+x);
	}

	private:
	std::size_t _size = 0;
	ELEM_T *data = nullptr;
};

/* An allocated (heap) ring. */
template<typename ELEM_T = float>
class AllocatedAudioRing: public AudioRingBase<ELEM_T, AllocatedRingProvider<ELEM_T>> {
	public:
	AllocatedAudioRing(std::size_t n) {
		assert(n > 0);
		this->data_provider.allocate(n);
	}
};

}
