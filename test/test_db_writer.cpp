#include <boost/test/unit_test.hpp>
#include <boost/test/data/test_case.hpp>

#include <set>
#include <limits>
#include <algorithm>
#include <iostream>

#include <db_writer.h>

using namespace reven::backend::memaccess::db;
using Db = reven::sqlite::Database;
using Stmt = reven::sqlite::Statement;

constexpr const char* test_tool_name = "TestDbWriter";
constexpr const char* test_tool_version = "1.0.0";
constexpr const char* test_tool_info = "TestDbWriter info";

std::vector<std::uint64_t> sqlite_results(Db& db, const char* query)
{
	Stmt stmt(db, query);
	std::vector<std::uint64_t> results;

	Stmt::StepResult result;
	while ((result = stmt.step()) == Stmt::StepResult::Row) {
		results.push_back(stmt.column_i64(0));
	}

	return results;
}

std::vector<bool> sqlite_results_is_null(Db& db, const char* query)
{
	Stmt stmt(db, query);
	std::vector<bool> results;

	Stmt::StepResult result;
	while ((result = stmt.step()) == Stmt::StepResult::Row) {
		results.push_back(stmt.column_type(0) == Stmt::Type::Null);
	}

	return results;
}

std::uint64_t sqlite_result(Db& db, const char* query)
{
	return sqlite_results(db, query).at(0);
}

std::uint64_t slice_count(Db& db)
{
	return sqlite_result(db, "select count(*) from slices;");
}

std::uint64_t chunk_count(Db& db)
{
	return sqlite_result(db, "select count(*) from chunks;");
}

std::uint64_t access_count(Db& db)
{
	return sqlite_result(db, "select count(*) from accesses;");
}

bool is_access_present(Db& db, MemoryAccess access)
{
	std::stringstream ss;
	ss << "select rowid from accesses where "
	   << "transition = " << access.transition_id << " and "
	   << "phy_first = " << access.physical_address << " and "
	   << "linear = " << access.virtual_address << " and "
	   << "size = " << access.size << " and "
	   << "operation = " << +static_cast<std::uint8_t>(access.operation)
	   << ";";
	auto result = sqlite_results(db, ss.str().c_str());
	BOOST_CHECK(result.size() < 2);
	return result.size();
}

bool is_non_empty_and_ordered(Db& db, const char* query)
{
	auto result = sqlite_results(db, query);
	auto sorted = result;
	std::sort(sorted.begin(), sorted.end());

	return result.size() and sorted == result;
}

std::array<MemoryAccess, 8> accesses {{
	MemoryAccess{ 0, 10, 6666, 10, true, Operation::Write },
	MemoryAccess{ 1, 100, 6666, 10, true, Operation::Write },
	MemoryAccess{ 2, 1000, 6666, 10, true, Operation::Write },
	MemoryAccess{ 3, 1005, 6666, 10, true, Operation::Write },
	MemoryAccess{ 4, 10, 6666, 10, true, Operation::Read },
	MemoryAccess{ 5, 100, 6666, 10, true, Operation::Read },
	MemoryAccess{ 6, 1000, 6666, 10, true, Operation::Read },
	MemoryAccess{ 7, 1005, 6666, 10, true, Operation::Read },
}};

BOOST_AUTO_TEST_CASE(test_db_writer_nominal)
{
	auto writer = DbWriter::from_memory(test_tool_name, test_tool_version, test_tool_info);
	for (const auto& a : accesses)
		writer.push(a);

	auto db = std::move(writer).take();
	BOOST_CHECK_EQUAL(slice_count(db), 1);
	BOOST_CHECK_EQUAL(chunk_count(db), 6);
	BOOST_CHECK_EQUAL(access_count(db), accesses.size());
	BOOST_CHECK_EQUAL(sqlite_result(db, "select min(transition_first) from slices;"), 0);
	BOOST_CHECK_EQUAL(sqlite_result(db, "select max(transition_last) from slices;"), 7);

	for (const auto& a : accesses)
		BOOST_CHECK(is_access_present(db, a));
}

BOOST_AUTO_TEST_CASE(test_db_writer_no_virtual)
{
	auto writer = DbWriter::from_memory(test_tool_name, test_tool_version, test_tool_info);
	writer.push(MemoryAccess{ 0, 10, 6666, 10, true, Operation::Write });
	writer.push(MemoryAccess{ 1, 100, 156, 10, false, Operation::Write });

	auto db = std::move(writer).take();
	BOOST_CHECK(not sqlite_results_is_null(db, "select linear from accesses where transition = 0;").at(0));
	BOOST_CHECK(sqlite_results_is_null(db, "select linear from accesses where transition = 1;").at(0));
}

BOOST_AUTO_TEST_CASE(test_db_writer_remove_last)
{
	auto writer = DbWriter::from_memory(test_tool_name, test_tool_version, test_tool_info);
	for (const auto& a : accesses)
		writer.push(a);
	writer.push(MemoryAccess{ 7, 200, 6666, 10, true, Operation::Write });
	writer.push(MemoryAccess{ 7, 200, 6666, 10, true, Operation::Read });
	writer.discard_after(7);

	auto db = std::move(writer).take();
	BOOST_CHECK_EQUAL(access_count(db), accesses.size() - 1);
}

BOOST_AUTO_TEST_CASE(test_db_writer_slices_ordering)
{
	auto writer = DbWriter::from_memory(test_tool_name, test_tool_version, test_tool_info);
	std::array<MemoryAccess, 9> accesses {{
		MemoryAccess{ 0, 10, 6666, 10, true, Operation::Write },
		MemoryAccess{ 1, 1000, 6666, 10, true, Operation::Write },
		MemoryAccess{ 2, 1, 6666, 10, true, Operation::Read },
		MemoryAccess{ 2, 100, 6666, 10, true, Operation::Read },
		MemoryAccess{ 4, 10, 6666, 10, true, Operation::Read },
		MemoryAccess{ 4, 1005, 6666, 10, true, Operation::Read },
		MemoryAccess{ 6, 100, 6666, 10, true, Operation::Write },
		MemoryAccess{ 7, 1005, 6666, 10, true, Operation::Read },
		MemoryAccess{ 12, 100, 6666, 10, true, Operation::Read },
	}};
	for (const auto& a : accesses)
		writer.push(a);

	auto db = std::move(writer).take();

	// Assume only one slice
	std::stringstream ss;
	std::vector<std::uint64_t> result;

	// Ensure basic request on indices do return sorted result by default.
	ss << "select phy_first from chunks where operation = " << +static_cast<std::uint8_t>(Operation::Read)
	   << " and slice_id = 1;";
	BOOST_CHECK(is_non_empty_and_ordered(db, ss.str().c_str()));
	ss.clear();

	ss << "select phy_first from chunks where operation = " << +static_cast<std::uint8_t>(Operation::Write)
	   << " and slice_id = 1;";
	BOOST_CHECK(is_non_empty_and_ordered(db, ss.str().c_str()));
	ss.clear();

	auto chunk_count = sqlite_result(db, "select max(rowid) from chunks;");
	for (std::uint64_t i = 1; i < chunk_count; ++i) {
		ss << "select transition from accesses where chunk_id = " << i << " and transition >= 0;";
		BOOST_CHECK(is_non_empty_and_ordered(db, ss.str().c_str()));
		ss.clear();
	}
	for (std::uint64_t i = 1; i < chunk_count; ++i) {
		ss << "select transition from accesses where chunk_id = " << i << " and transition <= 4;";
		BOOST_CHECK(is_non_empty_and_ordered(db, ss.str().c_str()));
		ss.clear();
	}
}
