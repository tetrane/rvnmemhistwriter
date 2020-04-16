#define BOOST_TEST_MODULE RVN_DB_WRITER
#include <boost/test/unit_test.hpp>
#include <boost/test/data/test_case.hpp>

#include <set>

#include "chunk.h"

using namespace reven::backend::memaccess::db;

static std::set<const ChunkAccess*> get_accesses(const Chunk& chunk, std::set<const ChunkAccess*> accesses = {})
{
	for (const auto* access = chunk.accesses(); access; access = access->next())
		BOOST_CHECK(accesses.insert(access).second); // Ensure unicity
	return accesses;
}

static void assert_merge(Chunk& a, Chunk&& b)
{
	auto size = a.size() + b.size();
	auto address_first = std::min(a.address_first(), b.address_first());
	auto address_last = std::max(a.address_last(), b.address_last());
	auto accesses = get_accesses(b, get_accesses(a));
	a.merge_in(std::move(b));
	BOOST_CHECK_EQUAL(a.size(), size);
	BOOST_CHECK_EQUAL(a.address_first(), address_first);
	BOOST_CHECK_EQUAL(a.address_last(), address_last);
	BOOST_CHECK(accesses == get_accesses(a));
}

static void assert_overlap(Chunk a, Chunk b)
{
	BOOST_CHECK(a.overlaps(b));
	assert_merge(a, std::move(b));
}

static void assert_touch(Chunk a, Chunk b)
{
	BOOST_CHECK(a.is_contiguous(b));
	assert_merge(a, std::move(b));
}

BOOST_AUTO_TEST_CASE(test_db_writer_chunk_creation)
{
	Chunk chunk(0x42, 10, 100);
	BOOST_CHECK_EQUAL(chunk.size(), 1);
	BOOST_CHECK(chunk.accesses() != nullptr);
	BOOST_CHECK(chunk.accesses()->next() == nullptr);
	BOOST_CHECK_EQUAL(chunk.accesses()->transition, 0x42);
	BOOST_CHECK_EQUAL(chunk.accesses()->address, 10);
	BOOST_CHECK_EQUAL(chunk.accesses()->size, 100);
}

BOOST_AUTO_TEST_CASE(test_db_writer_chunk_merging)
{
	assert_overlap(Chunk(0, 10, 10), Chunk(2, 10, 10)); // Cover
	assert_overlap(Chunk(0, 10, 10), Chunk(2, 4, 20));  // Over
	assert_overlap(Chunk(0, 10, 10), Chunk(2, 15, 2));  // Inside
	assert_overlap(Chunk(0, 10, 10), Chunk(2, 12, 10)); // Up
	assert_overlap(Chunk(0, 10, 10), Chunk(2, 8, 10));  // Down

	assert_touch(Chunk(0, 10, 10), Chunk(0, 20, 10)); // Up
	assert_touch(Chunk(0, 10, 10), Chunk(0, 0, 10)); // Down
}
