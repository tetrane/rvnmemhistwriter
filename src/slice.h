#pragma once

#include <map>
#include <cstdint>
#include <experimental/optional>

#include "chunk.h"

namespace reven {
namespace backend {
namespace memaccess {
namespace db {

class SliceBuilder;

/**
 * This is the representation of a slice. Chunks are accessible through begin() and end(), which are map's iterator and
 * so chunks are stored sorted by addresses.
 *
 * There cannot be two overlapping chunks in a slice, though some might be side by side.
 */
class Slice
{
public:
	using StorageType = std::map<std::uint64_t, Chunk>;
	using Iterator = StorageType::iterator;
	using ConstIterator = StorageType::const_iterator;

	std::uint64_t transition_first() const { return transition_first_; }
	std::uint64_t transition_last() const { return transition_last_; }

	// Iterators to chunks
	Iterator begin() { return access_chunks_.begin(); }
	Iterator end() { return access_chunks_.end(); }

	ConstIterator begin() const { return access_chunks_.cbegin(); }
	ConstIterator end() const { return access_chunks_.cend(); }

	bool empty() const { return begin() == end(); }

	std::size_t chunk_count() const {
		return access_chunks_.size();
	}

	/**
	 * Warning: will actually count accesses, so it is fairly slow.
	 */
	std::size_t access_count() const {
		std::size_t count = 0;
		for(const auto& c: access_chunks_) {
			count += c.second.size();
		}
		return count;
	}

private:
	friend SliceBuilder;
	StorageType access_chunks_;
	std::uint64_t transition_first_ = 0;
	std::uint64_t transition_last_ = 0;
};

/**
 * This object's role is to help create a slice from separate accesses, creating and merging chunks as necessary.
 */
class SliceBuilder
{
public:
	/**
	 * Will impose a soft limit on the amount of accesses in a chunk. Note this limit will temporarily be ignored if the
	 * access being added is on a transition that is already part of the slice, to enforce the non-overlapping property
	 * of slices.
	 */
	SliceBuilder& chunk_size_overlap_limit(std::uint64_t chunk_size_overlap_limit) {
		chunk_size_overlap_limit_ = chunk_size_overlap_limit;
		return *this;
	}

	/**
	 * Will impose a soft limit on the amount of accesses in a chunk that will not be crossed when merging touching
	 * chunks.
	 * Note this will not impact `insert`, only the post-processing merge phase.
	 */
	SliceBuilder& chunk_size_touch_limit(std::uint64_t chunk_size_touch_limit) {
		chunk_size_touch_limit_ = chunk_size_touch_limit;
		return *this;
	}

	/**
	 * Will impose a hard limit on the transitions a slice can represent.
	 */
	SliceBuilder& transition_limit(std::uint64_t transition_limit) {
		transition_limit_ = transition_limit;
		return *this;
	}

	/**
	 * Will impose a soft limit on the amount of accesses a slice can contain.
	 */
	SliceBuilder& access_count_limit(std::uint64_t access_count) {
		access_count_limit_ = access_count;
		return *this;
	}

	/**
	 * Insert an access in the slice being built.
	 *
	 * If `transition_limit` is hit, the insertion will not happen and this function will return nullptr.
	 * If `chunk_size_overlap_limit` is hit, the insertion will not happen unless the transition is already part of the
	 * slice, in which case the next access on a new transition will be refused.
	 *
	 * Otherwise, it will return a pointer to the inserted access. This pointer is valid until after the slice being
	 * returned by `build` is destroyed, or until this object is destroyed if `build` is never called.
	 */
	const ChunkAccess* insert(std::uint64_t icount, std::uint64_t address, std::uint64_t size)
	{
		if (size == 0) {
			throw std::invalid_argument("SliceBuilder insertion: attempted to insert access with size 0");
		}

		if (icount > slice_.transition_last_ and stop_at_next_transition_)
			return nullptr;

		if (access_count_limit_ and access_count_ >= access_count_limit_) {
			if (icount > slice_.transition_last_) {
				return nullptr;
			} else {
				// Do not refuse accesses if this transition is already part of the slice.
				stop_at_next_transition_ = true;
			}
		}

		if (static_cast<std::uint64_t>(address - 1 + size) < address) {
			// Wrap around
			throw std::invalid_argument("SliceBuilder insertion: address + size wraps around std::uint64_t");
		}

		if (slice_.access_chunks_.size() and icount < slice_.transition_last_) {
			throw std::invalid_argument("SliceBuilder insertion: icount going backward");
		}

		if (transition_limit_ and not slice_.access_chunks_.empty() and
		    (icount - slice_.transition_first_ + 1) > *transition_limit_)
			return nullptr;

		Chunk access_chunk(icount, address, size);
		const auto* access = access_chunk.accesses();
		std::vector<Slice::Iterator> overlaps;
		auto total_count = access_chunk.size();

		// Look for existing chunks that we might have to merge in.
		if (slice_.access_chunks_.empty()) {
			slice_.transition_first_ = icount;
		} else {
			auto next = slice_.access_chunks_.upper_bound(address);
			if (next != slice_.access_chunks_.begin()) {
				auto previous = std::prev(next);
				if (previous->second.overlaps(access_chunk)) {
					overlaps.push_back(previous);
					total_count += previous->second.size();
				}
			}
			while (next != slice_.access_chunks_.end()) {
				if (next->second.overlaps(access_chunk)) {
					overlaps.push_back(next);
					total_count += next->second.size();
					next++;
				} else {
					break;
				}
			}
		}

		if (chunk_size_overlap_limit_ and total_count > *chunk_size_overlap_limit_) {
			if (icount > slice_.transition_last_) {
				return nullptr;
			} else {
				// Do not refuse accesses if this transition is already part of the slice.
				stop_at_next_transition_ = true;
			}
		}

		if (slice_.access_chunks_.empty())
			slice_.transition_first_ = icount;

		for (auto& it : overlaps) {
			access_chunk.merge_in(std::move(it->second));
			slice_.access_chunks_.erase(it);
		}

		slice_.transition_last_ = icount;
		slice_.access_chunks_.insert(std::pair<std::uint64_t, Chunk>(access_chunk.address_first(), std::move(access_chunk)));

		access_count_ += 1;
		return access;
	}

	/**
	 * Finish building the slice and return it. The current object is left in an undefined state.
	 */
	Slice build() &&
	{
		merge();
		access_count_ = 0;
		return std::move(slice_);
	}

	/**
	 * Count the accesses that have been inserted so far.
	 */
	std::size_t access_count() const { return access_count_; }

	/**
	 * Count the chunks that have been created so far.
	 */
	std::size_t chunk_count() const { return slice_.chunk_count(); }

private:
	/**
	 * During normal insertion, chunks are merged only if absolutely necessary, ie when an access overlaps an existing
	 * chunk. This method will then try to merge chunks that are next to each other: this step drastically reduces the
	 * amount of chunks, while allowing for a separate, smaller "soft" limit (see @ref chunk_size_touch_limit)
	 */
	void merge()
	{
		if (slice_.access_chunks_.empty())
			return;

		auto current = slice_.access_chunks_.begin();
		for (auto next = std::next(current); next != slice_.access_chunks_.end();) {
			if (current->second.is_contiguous(next->second) and
			    (not chunk_size_touch_limit_ or
			     current->second.size() + next->second.size() <= *chunk_size_touch_limit_)) {
				current->second.merge_in(std::move(next->second));
				slice_.access_chunks_.erase(next++);
			} else {
				current = next;
				next++;
			}
		}
	}

	Slice slice_;
	std::experimental::optional<std::size_t> chunk_size_touch_limit_;
	std::experimental::optional<std::size_t> chunk_size_overlap_limit_;
	std::experimental::optional<std::size_t> transition_limit_;
	std::experimental::optional<std::size_t> access_count_limit_;
	bool stop_at_next_transition_ = false;
	std::size_t access_count_ = 0;
};

}}}}
