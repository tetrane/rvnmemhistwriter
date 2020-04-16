#pragma once

#include <vector>
#include <cstdint>

namespace reven {
namespace backend {
namespace memaccess {
namespace db {

class Chunk;

/**
 * This is the representation for an access. It is intrusively linked because the author could not make a performant use
 * of the std lists objects.
 */
struct ChunkAccess {
	ChunkAccess(std::uint64_t transition, std::uint64_t address, std::uint32_t size)
	  : transition(transition)
	  , address(address)
	  , size(size)
	{
	}

	std::uint64_t transition;
	std::uint64_t address;
	std::uint32_t size;

	/**
	 * Return a valid pointer to the next item in the list, or `nullptr` if this is the last element.
	 */
	const ChunkAccess* next() const { return next_.get(); }

	/**
	 * Return a valid pointer to the next item in the list, or `nullptr` if this is the last element.
	 */
	ChunkAccess* next() { return next_.get(); }

private:
	std::unique_ptr<ChunkAccess> next_;
	friend Chunk;
};

/**
 * This is a chunk. Note that it is supposed to be part of a slice, so it does not store the bounding transitions.
 */
class Chunk
{
public:
	/**
	 * Spawn a new chunk from a single access.
	 */
	Chunk(std::uint64_t transition, std::uint64_t address, std::uint32_t size)
		: address_first_(address), address_last_(address + size - 1)
	{
		accesses_ = std::make_unique<ChunkAccess>(transition, address, size);
		last_access_ = accesses_.get();
		size_ = 1;
	}

	Chunk(Chunk&& other) = default;

	~Chunk()
	{
		// Manually unwind nested ptr to avoid stack overflow while browsing destructors
		while (accesses_) {
			accesses_ = std::move(accesses_->next_);
		}
	}

	std::uint64_t address_first() const { return address_first_; }
	std::uint64_t address_last() const { return address_last_; }
	std::uint64_t address_size() const { return address_last_ - address_first_ + 1; }

	/**
	 * Return the first access known. You can iterate on accesses by calling ChunkAccess::next(), until it returns
	 * nullptr. The returned pointer and its siblings are valid as long as the Chunk is valid.
	 */
	const ChunkAccess* accesses() const { return accesses_.get(); }

	/**
	 * Return the number of accesses stored
	 */
	std::size_t size() const { return size_; }

	/**
	 * Does `other` overlaps with this chunk? Overlapping means there is at least one address in common.
	 */
	bool overlaps(const Chunk& other) const
	{
		if ((address_last() + 1) <= other.address_first() or (other.address_last() + 1) <= address_first())
			return false;
		else
			return true;
	}

	/**
	 * Does `other` touch with this chunk? Touching means `a.last + 1 == b.first`, regardless of which is a and b.
	 * Note this will return false on overlapping chunks.
	 */
	bool is_contiguous(const Chunk& other) const
	{
		return ((address_last() + 1) == other.address_first() or (other.address_last() + 1) == address_first());
	}

	/**
	 * Merge a chunk in. The other chunk's accesses will be moved in 0(1), and their pointers will remain valid.
	 */
	void merge_in(Chunk&& other)
	{
		address_first_ = std::min(address_first(), other.address_first());
		address_last_ = std::max(address_last(), other.address_last());
		if (last_access_->next_)
			throw std::logic_error("Current next is not null");
		if (other.last_access_->next_)
			throw std::logic_error("Other next is not null");
		last_access_->next_ = std::move(other.accesses_);
		last_access_ = other.last_access_;
		size_ += other.size();
	}

private:
	std::uint64_t address_first_, address_last_;
	std::unique_ptr<ChunkAccess> accesses_;
	ChunkAccess* last_access_;
	std::size_t size_;
};

}}}}
