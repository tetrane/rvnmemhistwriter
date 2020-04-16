# rvnmemhistwriter: a library for writing the memory history to disk

## What's this

This library is meant to be used by trace providers to write a detailed of memory accesses to disk.

The data is stored as an sqlite database. The storage model is as follows:

```
@ ^
 |         slice         slice
 |    |             |             |  |    |
 |    |             |             |  +----+
 |    +-------------+             |  ||   |
 |    |    | ||     +-------------+  ||  ||
 |    |     |   |   |        |  | |  +----+
 |    +-------------+    |   |    |  |    |
 |    |    chunk-^  |   |     |<----access with size
 |    |             +-------------+  |    |
 |    |             |             |  |    |
 |    |             |             |  |    |
 |    |             +-------------+  |    |
 |    |             |  |       |  |  |    |
 |    |             +-------------+  |    |
 |    |             |             |  |    |
 |    |             |             | ^------ empty gap: no slice there
 +----+-------------+-------------+--+----+->
```

## Database description and properties

The database schema looks like this:

```
create table slices(transition_first int8 not null, transition_last int8 not null);
create table chunks(slice_id int8 not null, phy_first int8 not null, phy_last int8 not null, operation int not null);
create table accesses(chunk_id int8 not null, transition int8 not null, linear int8 not null, phy_first int8 not null, size int not null, operation int not null);

create index if not exists idx_slices_1 on slices(transition_last);
create index if not exists idx_chunks_1 on chunks(operation, slice_id, phy_last);
create index if not exists idx_accesses_1 on accesses(chunk_id, transition);
create index if not exists idx_accesses_1 on accesses(chunk_id, rowid);
```

See the `db_writer.cpp` file for the up-to-date detailed database schema.

This model also guarantees the following properties:
- Slices do not overlap at all.
- Slices are stored in order of transition but are not consecutive (there may be gaps)
- Slices are shared by operations
- Chunks are stored in order of addresses but are not consecutive (there may be gaps)
- Chunks are not shared by operations: each chunks stores access for one operation type only. This is necessary because of the next property:
- Chunks will always contain at least one access of the chunk's type for each address of their range, except on the very last (few) transition (because of `discard_after`).
- Chunks for the same operation do not overlap but chunks of different operation may.
- The amount of accesses a chunk can contain is capped.
- Each access belongs to exactly one chunk.
- Accesses are stored in the order of the trace, so their row id order is their order of appearance.

These properties are what make the model fast to query, because:
- no overlapping means quering ranges is fast (we'll see why below)
- there are few slices (500) so querying them all is acceptable in the worst case where no access exists for a query
- since a chunk is guaranteed to contain accesses, there is no worst case to watch for when very few accesses exist on a range
- the "natural" order of query results implied by the indexes allow us to avoid using slow `order by` clauses most of the time.

Some of these properties cannot be expressed in SQL and must leveraged by hand: therefore, this schema is meant to be used in a certain way, so we can ease SQlite's work where possible.

## Half-axis query

The basic query (and pretty much the only query we support for now) is the "half axis" query: "Give me the first X accesses starting at transition Y going forward / backward, between address A and B".

### Overview

The forward query implementation would look like this pseudo-code:

```
def next(transition, type, address_first, address_last, max_results):
  slice_id = find_slice(transition, forward=True)

  results = []

  # Iterate on slices
  while (len(results) < max_results)
    slice_result = []

    required_results = max_results - len(results)

    # Select chunks at or upwards our range.
    # Note that if type contains two different type (Read Or Write), we must query chunks twice, once for each operation
    # See below for an explanation why.
    for c in find_chunks_between(slice_id, type, address_first, address_last):

      # We want at most `required_results` results for this chunk.
      for a in find_accesses(c.chunk_id, address_first, address_last, transition, forward=True, max_result=required_results):
        slice_result.append(a)

      # In this loop, we do not yet know if we have the actual first `required_results` of the entire slice, only for
      # the chunks we parsed! We need to access all chunks before we know for sure. This is a drawback of this data
      # model, but it is acceptable since we can still limit our query with `required_results` and chunks sizes are
      # capped anyway.

    # Now we do have all accesses we need. Sort them and keep only what we need.
    results += sorted(slice_result)[0:max_results]

    slice_id += 1 # Go forward
  return results
```

This is easy to adapt for backward queries.

### How to quickly query sorted, non-overlapping ranges: slices.

Slices are non overlapping, not necessarily consecutive ranges of transition stored in order of transition. That means one transition will be part of at most one slice, sometimes none.

To find which slice a transition is in, you would NOT do this:

```
select rowid from slices where transition_first <= $transition and transition_last >= $transition;
```

Doing this would force sqlite to browse all slices because of the range query on two different parameters. Instead, it is valid to do this:

```
"""
Find the slice corresponding to transition. If transition is on a gap, what is returned depends on the forward
parameter: if forward, will return the next possible slice, else will return the previous possible slice.

If no possible slice exists, will return None.
"""
def find_slice(transition, forward):
	slice = sql("select rowid, transition_first from slices where transition_last >= $transition limit 1;"):

	# Slices will be returned in order, so the first slice where `transition_last >= transition` is either the
	# slice we want if `transition_first <= transition`, or the slice immediately to the right if not (which means the
	# requested transition is in an empty gap)

	if not slice:
		# `transition` is past any existing slice, so:
		return None if forward else last_slice_id

	if slice.transition_first <= transition:
		# This is a match, we're on this slice.
		return slice.rowid

	# Not exact match, so `slice` is the slice to the right of transition.
	if forward:
		# We want the slice to the right
		return slice.rowid
	else
		# We want the slice to the left, if there is one.
		return None if slice.rowid == 0 else slice.rowid - 1
```

NOTE: Again slices may not be consecutive! There can be empty gaps between them.

### How to quickly query sorted, non-overlapping ranges: chunks.

Once you know which slice you're querying, you can do something similar for chunks:

```
"""
Return chunks on a specifed slice, for a specified type, that overlap with the given address range.
Note: you cannot request multiple types at once, see below.
"""
def find_chunks_between(slice_id, type, address_first, address_last)::
  chunks = []

  # We pre-filter out all chunks that are before the requested range thanks to sqlite's index
  for chunk in sql("select rowid, phy_first from chunks where operation = $type and slice_id = $slice_id and phy_last >= $address_first;"):
    # Now all chunks are on or after the requested range
    if slice.phy_first <= address_last:
      # This is a match, keep this chunk.
      chunks.append(slice.rowid)
    else:
      # If this chunk is not a match, no other following chunk will be, we can stop there.
      break

  return chunks
```

Sqlite would not be able to do something similar to our `break`, which here helps immensily in reducing the amount of chunks we check.

IMPORTANT: Note that since this query relies on chunks never overlapping, you **must** make one query per operation.
Do not mix chunks from different operation here, because then the assumption which allows the `break` in the above code
does not work. This is deemed acceptable because this query is very fast anyway, it is not the bottleneck.

### How to quickly query accesses.

Finally, we can get accesses that way:

```
def find_accesses(chunk_id, address_first, address_last, transition, forward, max_result):
  if not forward:
    transition_query = "transition <= $transition"
    order = "order by transition desc"
  else:
    transition_query = "transition >= $transition"
    # Order by transition is natural because of the index
    order = ""

  # Query all accesses. Note the index will be used on chunk_id & transition, but then all resulting accesses will need
  # to be parsed for the address range query because there is no order there, so above optimisations cannot apply.
  # That means we can make a complex query because we will pay the full price even if we don't.
  # Again, this is acceptable because the amount of accesses per chunk is capped to acceptable values.
  for a in sql("select transition from accesses where chunk_id = $chunk_id and " + transition_query + " and phy_first <= $address_last and phy_first + size >= $address_first " + order + " limit $max_result;"):

```

### Resuming the half-axis query

Since the access' row ids are sorted in order of appearance (which is the same axis as transition but even finer), you can substitue the transition in the previous code with the access row id easily, which makes it easy to resume a half-axis query.
