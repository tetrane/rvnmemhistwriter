#include "db_writer.h"

#include <unordered_map>
#include <algorithm>
#include <sstream>
#include <iostream>

#include <rvnmetadata/metadata-common.h>
#include <rvnmetadata/metadata-sql.h>

#include "slice.h"

namespace reven {
namespace backend {
namespace memaccess {
namespace db {

/**
 * This structure is there to keep track of information related to the inserted accesses that is not stored in the
 * underlying Slice objects.
 *
 * Note that they are stored in DbWriter::current_access_list_ in their order of appearance, which is lost by the Slice
 * object but required when inserting them in the database.
 */
struct AccessInfo {
	const ChunkAccess* chunk_access; // Inserted access. Invalidated when builder and built slices are destroyed.
	bool has_virtual_address;
	std::uint64_t virtual_address;
	std::uint8_t operation; // Operation as is supposed to be inserted in the database.
};

namespace {

using ChunkAccessToRowId = std::unordered_map<const ChunkAccess*, std::uint64_t>;
using Db = sqlite::Database;
using RDb = sqlite::ResourceDatabase;
using Stmt = sqlite::Statement;

using Meta = ::reven::metadata::Metadata;
using MetaType = ::reven::metadata::ResourceType;
using MetaVersion = ::reven::metadata::Version;

void create_sqlite_db(Db& db)
{
	db.exec("create table slices(transition_first int8 not null, transition_last int8 not null);",
	        "Can't create table slices");
	db.exec("create table chunks(slice_id int8 not null, phy_first int8 not null, phy_last int8 not null,"
	        "operation int not null);",
	        "Can't create table chunks");
	db.exec("create table accesses(chunk_id int8 not null, transition int8 not null, linear int8,"
	        "phy_first int8 not null, size int not null, operation int not null);",
	        "Can't create table accesses");

	db.exec("create index idx_slices_1 on slices(transition_last);", "Can't create idx_slices_1");
	db.exec("create index idx_chunks_1 on chunks(operation, slice_id, phy_last);",
	        "Can't create idx_chunks_1");
	db.exec("create index idx_accesses_1 on accesses(chunk_id, transition);",
	        "Can't create idx_accesses_1");
	db.exec("create index idx_accesses_2 on accesses(transition);",
	        "Can't create idx_accesses_2");

	db.exec("pragma synchronous=off", "Pragma error");
	db.exec("pragma count_changes=off", "Pragma error");
	db.exec("pragma journal_mode=memory", "Pragma error");
	db.exec("pragma temp_store=memory", "Pragma error");
}

// Will insert a slice in the database and return the inserted rowid
std::uint64_t insert_slice(Db& db, Stmt& stmt, const Slice& read_slice, const Slice& write_slice)
{
	// Compute slice bounding box
	auto transition_first = std::min(read_slice.transition_first(), write_slice.transition_first());
	auto transition_last = std::max(read_slice.transition_last(), write_slice.transition_last());

	if (read_slice.empty() and write_slice.empty()) {
		throw std::logic_error("You should not be writing empty slices into database");
	}

	if (read_slice.empty()) {
		transition_first = write_slice.transition_first();
		transition_last = write_slice.transition_last();
	} else if (write_slice.empty()) {
		transition_first = read_slice.transition_first();
		transition_last = read_slice.transition_last();
	}

	stmt.bind_arg_throw(1, transition_first, "transition_first");
	stmt.bind_arg_throw(2, transition_last, "transition_last");
	stmt.step();

	stmt.reset();

	return static_cast<std::uint64_t>(db.last_insert_rowid());
}

// Will insert chunks from both slices in the database and return a map of ChunkAccess -> corresponding chunk rowid
ChunkAccessToRowId insert_chunks(Db& db, Stmt& stmt, const Slice& read_slice, const Slice& write_slice,
	                             std::uint64_t slice_id)
{
	struct ChunkWithDescription {
		std::uint8_t operation;
		const Chunk* chunk;
	};

	std::vector<ChunkWithDescription> chunk_list;
	ChunkAccessToRowId access_to_chunk_id;

	for (const auto& it : read_slice) {
		chunk_list.emplace_back(ChunkWithDescription{ static_cast<std::uint8_t>(Operation::Read), &it.second });
	}
	for (const auto& it : write_slice) {
		chunk_list.emplace_back(ChunkWithDescription{ static_cast<std::uint8_t>(Operation::Write), &it.second });
	}

	// Let's ease sqlite's job and ensure chunks are naturally sorted by ascending address
	std::sort(chunk_list.begin(), chunk_list.end(), [](const ChunkWithDescription& a, const ChunkWithDescription& b) {
		return a.chunk->address_first() > b.chunk->address_first();
	});

	for (const auto& it : chunk_list) {
		stmt.bind_arg_throw(1, slice_id, "slice_id");
		stmt.bind_arg_throw(2, it.chunk->address_first(), "phy_first");
		stmt.bind_arg_throw(3, it.chunk->address_last(), "phy_last");
		stmt.bind_arg_extend(4, it.operation, "operation");
		stmt.step();
		stmt.reset();

		std::uint64_t chunk_id = static_cast<std::uint64_t>(db.last_insert_rowid());
		for (auto a = it.chunk->accesses(); a; a = a->next()) {
			access_to_chunk_id.emplace(a, chunk_id);
		}
	}

	return access_to_chunk_id;
}

// Will insert chunks from both slices in the database and return a map of ChunkAccess -> corresponding chunk rowid
void insert_accesses(Stmt& stmt, const std::vector<AccessInfo>& current_access_list,
	            const ChunkAccessToRowId& access_to_chunk_id)
{
	for (const auto& access: current_access_list) {
		auto chunk_id_it = access_to_chunk_id.find(access.chunk_access);
		if (chunk_id_it == access_to_chunk_id.end()) {
			throw std::logic_error("access_to_chunk_id object should contain all accesses, but one is missing");
		}

		stmt.bind_arg_throw(1, chunk_id_it->second, "chunk_id");
		stmt.bind_arg_throw(2, access.chunk_access->transition, "transition");
		if (access.has_virtual_address)
			stmt.bind_arg_cast(3, access.virtual_address, "linear");
		else
			stmt.bind_null(3, "linear");
		stmt.bind_arg_throw(4, access.chunk_access->address, "phy_first");
		stmt.bind_arg_throw(5, access.chunk_access->size, "size");
		stmt.bind_arg_extend(6, access.operation, "operation");
		stmt.step();
		stmt.reset();
	}
}

} // anonymous namespace

DbWriter::DbWriter(const char* filename, const char* tool_name, const char* tool_version, const char* tool_info) :
	db_([filename, tool_name, tool_version, tool_info]() {
	auto md = Meta(
		MetaType::MemHist,
		MetaVersion::from_string(format_version),
		tool_name,
		MetaVersion::from_string(tool_version),
		tool_info + std::string(" - using rvnmemhistwriter ") + writer_version
	);

	auto rdb = RDb::create(filename, metadata::to_sqlite_raw_metadata(md));
	create_sqlite_db(rdb);
	return rdb;
}()),
	insert_slice_stmt_(db_, "insert into slices values (?,?);"),
	insert_chunk_stmt_(db_, "insert into chunks values (?,?,?,?);"),
	insert_access_stmt_(db_, "insert into accesses values (?,?,?,?,?,?);")
{
	create_slices();
}

DbWriter DbWriter::from_memory(const char* tool_name, const char* tool_version, const char* tool_info)
{
	return DbWriter(":memory:", tool_name, tool_version, tool_info);
}

void DbWriter::push(const MemoryAccess& access)
{
	auto& builder = [&access, this]() -> std::unique_ptr<SliceBuilder>& {
		switch(access.operation) {
			case Operation::Read: return read_slice_builder_;
			case Operation::Write: return write_slice_builder_;
			case Operation::Execute: throw std::runtime_error("Execute access is not supported");
		}
		throw std::logic_error("Unknown access type");
	}();

	const auto* inserted_access = builder->insert(access.transition_id, access.physical_address, access.size);
	if (not inserted_access) {
		insert_slices();
		create_slices();

		// Note that SliceBuilder pointers will have changed since last `insert` call

		inserted_access = builder->insert(access.transition_id, access.physical_address, access.size);
		if (not inserted_access) {
			throw std::logic_error("Insertion must be possible on empty slices");
		}
	}
	current_access_list_.push_back(
	  { inserted_access, access.has_virtual_address, access.virtual_address, static_cast<std::uint8_t>(access.operation) });
}

void DbWriter::create_slices()
{
	// These capping values are found empirically
	std::size_t overlap_limit = 100000; // reasonable access time
	std::size_t touch_limit = 1000;
	std::size_t access_count_limit = 10000000; // ~3Go ram while building

	read_slice_builder_ = std::make_unique<SliceBuilder>();
	read_slice_builder_->chunk_size_overlap_limit(overlap_limit)
	  .chunk_size_touch_limit(touch_limit)
	  .access_count_limit(access_count_limit);
	write_slice_builder_ = std::make_unique<SliceBuilder>();
	write_slice_builder_->chunk_size_overlap_limit(overlap_limit)
	  .chunk_size_touch_limit(touch_limit)
	  .access_count_limit(access_count_limit);
}

void DbWriter::insert_slices()
{
	if (current_access_list_.empty())
		return;

	auto read_slice = std::move(*read_slice_builder_).build();
	read_slice_builder_.reset();
	auto write_slice = std::move(*write_slice_builder_).build();
	write_slice_builder_.reset();

	db_.exec("begin", "Cannot start transaction");

	std::uint64_t slice_id = insert_slice(db_, insert_slice_stmt_, read_slice, write_slice);

	auto access_to_chunk_id = insert_chunks(db_, insert_chunk_stmt_, read_slice, write_slice, slice_id);

	insert_accesses(insert_access_stmt_, current_access_list_, access_to_chunk_id);

	db_.exec("commit", "Can't commit transaction");

	// Slices will be deleted when this function exits, invalidating these pointers. We couldn't keep them even if we
	// needed to.
	current_access_list_.clear();
}

void DbWriter::discard_after(uint64_t transition_count)
{
	// We have no choice but to dump the current open slices, regardless of constraints. This is OK, because we call
	// this at the end of the recording. This is why push after this method should not happen.
	insert_slices();

	std::stringstream ss;
	ss << "delete from accesses where "
	   << "chunk_id >= (select min(rowid) from chunks where "
	   <<              "slice_id = (select rowid from slices where transition_last >= " << transition_count
	   <<                         " limit 1) limit 1) and "
	   << "transition >= " << transition_count << ";";
	db_.exec(ss.str().c_str(), "Can't discard accesses");

	// Note we do not shrink chunks or slices to reflect this change, which may result in slight inconsistencies where
	// chunks are empty or not tight.
	// This should not be a problem because the user should always end up requesting the accesses for a given chunk,
	// and in this case accesses will simply not be there.
}

DbWriter::~DbWriter()
{
	if (db_.get()) {
		insert_slices();
	}
}

RDb DbWriter::take() &&
{
	insert_slices();
	return std::move(db_);
}

// Move ctor/op can be default because they the dtor does nothing after the move db_
DbWriter::DbWriter(DbWriter&&) = default;
DbWriter& DbWriter::operator=(DbWriter&&) = default;

}}}} // namespace reven::backend::memaccess::db
