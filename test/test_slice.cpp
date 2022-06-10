#include <boost/test/unit_test.hpp>
#include <boost/test/data/test_case.hpp>

#include <set>
#include <limits>
#include <iostream>

#include "slice.h"

using namespace reven::backend::memaccess::db;

BOOST_AUTO_TEST_CASE(test_db_writer_slice_builder_nominal)
{
	SliceBuilder b;
	BOOST_CHECK(b.insert(1, 10, 10));
	BOOST_CHECK(b.insert(2, 8, 10));    // merged
	BOOST_CHECK(b.insert(3, 12, 10));   // merged
	BOOST_CHECK(b.insert(3, 30, 10));   // new chunk, but...
	BOOST_CHECK(b.insert(3, 18, 15));   // merged now

	BOOST_CHECK(b.insert(4, 100, 10));
	BOOST_CHECK(b.insert(5, 100, 10));  // merged
	BOOST_CHECK(b.insert(6, 98, 10));   // merged
	BOOST_CHECK(b.insert(7, 108, 10));  // merged
	BOOST_CHECK(b.insert(8, 80, 10));   // new chunk, but...
	BOOST_CHECK(b.insert(9, 85, 20));   // merged now
	BOOST_CHECK(b.insert(10, 120, 10)); // new chunk, but...
	BOOST_CHECK(b.insert(11, 90, 40));  // merged now

	BOOST_CHECK(b.insert(12, 200, 10));
	BOOST_CHECK(b.insert(13, 210, 10)); // touches
	BOOST_CHECK(b.insert(14, 190, 10)); // touches

	BOOST_CHECK(b.insert(100, 0xfffffff0, 1));  // last one

	auto builder_count = b.access_count();
	auto slice = std::move(b).build();

	BOOST_CHECK_EQUAL(slice.access_count(), builder_count);
	BOOST_CHECK_EQUAL(slice.access_count(), 17);
	BOOST_CHECK_EQUAL(slice.chunk_count(), 4);
	BOOST_CHECK_EQUAL(slice.transition_first(), 1);
	BOOST_CHECK_EQUAL(slice.transition_last(), 100);
}

BOOST_AUTO_TEST_CASE(test_db_writer_slice_builder_wraparound)
{
	SliceBuilder b;
	BOOST_CHECK_NO_THROW(b.insert(0, std::numeric_limits<std::uint64_t>::max(), 1));
	BOOST_CHECK_NO_THROW(b.insert(0, std::numeric_limits<std::uint64_t>::max() - 2, 3));
	BOOST_CHECK_THROW(b.insert(0, std::numeric_limits<std::uint64_t>::max() - 2, 4), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(test_db_writer_slice_builder_backward)
{
	SliceBuilder b;
	BOOST_CHECK_NO_THROW(b.insert(0, 1, 1));
	BOOST_CHECK_NO_THROW(b.insert(1, 1, 1));
	BOOST_CHECK_THROW(b.insert(0, 1, 1), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(test_db_writer_slice_builder_limit_overlap)
{
	SliceBuilder b;
	b.chunk_size_overlap_limit(2);
	BOOST_CHECK(b.insert(1, 10, 10));
	BOOST_CHECK(b.insert(2, 18, 10));
	BOOST_CHECK(not b.insert(3, 25, 10));
}

BOOST_AUTO_TEST_CASE(test_db_writer_slice_builder_limit_overlap_ignored)
{
	SliceBuilder b;
	b.chunk_size_overlap_limit(2);
	BOOST_CHECK(b.insert(1, 10, 10));
	BOOST_CHECK(b.insert(2, 15, 10));
	BOOST_CHECK(b.insert(2, 20, 10)); // Will be inserted anyway because transition 2 is already part of the slice.
	BOOST_CHECK(b.insert(2, 25, 10)); // Idem
	BOOST_CHECK(b.insert(2, 50, 10)); // Idem even if not on previous chunk
	BOOST_CHECK(not b.insert(3, 250, 10)); // New transition: will finally be ignored even if not on previous chunk.
}

BOOST_AUTO_TEST_CASE(test_db_writer_slice_builder_limit_transition)
{
	SliceBuilder b;
	b.transition_limit(2);
	BOOST_CHECK(b.insert(0, 10, 10));
	BOOST_CHECK(b.insert(1, 10, 10));
	BOOST_CHECK(not b.insert(2, 10, 10));
}

BOOST_AUTO_TEST_CASE(test_db_writer_slice_builder_limit_touch)
{
	SliceBuilder b;
	b.chunk_size_touch_limit(2);
	BOOST_CHECK(b.insert(0, 0, 10));
	BOOST_CHECK(b.insert(1, 10, 10)); // will be merged
	BOOST_CHECK(b.insert(2, 20, 10)); // will be ignored
	BOOST_CHECK_EQUAL(b.chunk_count(), 3);
	auto slice = std::move(b).build();
	BOOST_CHECK_EQUAL(slice.chunk_count(), 2);
}

BOOST_AUTO_TEST_CASE(test_db_writer_slice_builder_limit_access_count_hard)
{
	SliceBuilder b;
	b.access_count_limit(2);
	BOOST_CHECK(b.insert(0, 0, 10));
	BOOST_CHECK(b.insert(1, 50, 10));
	BOOST_CHECK(b.insert(2, 200, 10) == nullptr); // will be ignored
	BOOST_CHECK_EQUAL(b.access_count(), 2);
}

BOOST_AUTO_TEST_CASE(test_db_writer_slice_builder_limit_access_count_soft)
{
	SliceBuilder b;
	b.access_count_limit(2);
	BOOST_CHECK(b.insert(0, 0, 10));
	BOOST_CHECK(b.insert(1, 50, 10));
	BOOST_CHECK(b.insert(1, 100, 10)); // will be kept even though > access count
	BOOST_CHECK(not b.insert(2, 200, 10)); // will be ignored
	BOOST_CHECK_EQUAL(b.access_count(), 3);
}

BOOST_AUTO_TEST_CASE(test_db_writer_slice_invalid_accesses)
{
	SliceBuilder b;
	BOOST_CHECK_THROW(b.insert(0, 1, 0), std::invalid_argument);
	BOOST_CHECK_THROW(b.insert(0, 0, 0), std::invalid_argument);
}
