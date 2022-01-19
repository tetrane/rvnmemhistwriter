#pragma once

#include <vector>
#include <memory>
#include <rvnsqlite/resource_database.h>

namespace reven {
namespace backend {
namespace memaccess {
namespace db {

constexpr const char* format_version = "1.0.0";
constexpr const char* writer_version = "1.1.0";

enum class Operation : std::uint8_t
{
	Execute = 0b001,
	Write = 0b010,
	Read = 0b100
};

struct MemoryAccess {
	std::uint64_t transition_id;
	std::uint64_t physical_address;
	std::uint64_t virtual_address;
	std::uint32_t size;
	bool has_virtual_address;
	Operation operation;
};

class SliceBuilder;
struct ChunkAccess;
struct AccessInfo;
struct ChunkWithDescription;

class DbWriter {
public:
	explicit DbWriter(const char* filename, const char* tool_name, const char* tool_version, const char* tool_info);

	// Build a DbWriter that writes a non-persistent database into memory
	static DbWriter from_memory(const char* tool_name, const char* tool_version, const char* tool_info);
	~DbWriter();
	// Due to having a dtor, we MUST explicitly declare the following ctors/operators.
	DbWriter(DbWriter&&);
	DbWriter(const DbWriter&) = delete;
	DbWriter& operator=(DbWriter&&);
	DbWriter& operator=(const DbWriter&) = delete;

	// Add a memory access to the database
	void push(const MemoryAccess& access);

	// Remove all accesses that were pushed with a transition >= transition_count
	// This method allows to cap the number of allowed transitions in a database after the fact.
	// It is in particular meant to help with the case of the final transition, which may be incomplete (as in, it
	// doesn't produce the final state).
	// Note that calling `push` after calling this method is not defined.
	void discard_after(std::uint64_t transition_count);

	sqlite::ResourceDatabase take() &&;

private:
	// Instanciate the slice builders.
	// Note: `read_slice_builder_` and `write_slice_builder_` pointers are valid after calling this method.
	void create_slices();

	// Will push the slices being built into database.
	// Note: `read_slice_builder_` and `write_slice_builder_` pointers are not valid after calling this method.
	void insert_slices();

	sqlite::ResourceDatabase db_;
	sqlite::Statement insert_slice_stmt_;
	sqlite::Statement insert_chunk_stmt_;
	sqlite::Statement insert_access_stmt_;

	// Both builders are pimpl, since Slice objects are implementation details.
	std::unique_ptr<SliceBuilder> read_slice_builder_;
	std::unique_ptr<SliceBuilder> write_slice_builder_;

	std::vector<AccessInfo> current_access_list_;
	// scratch-space for reuse without allocation during chunk insertion
	std::vector<ChunkWithDescription> chunk_list_;
};

}}}} // namespace reven::backend::memaccess::db
