/**
 * This file is part of the "libfnord" project
 *   Copyright (c) 2015 Paul Asmuth
 *
 * FnordMetric is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License v3.0. You should have received a
 * copy of the GNU General Public License along with this program. If not, see
 * <http://www.gnu.org/licenses/>.
 */
#include <fnord-tsdb/StreamChunk.h>
#include <fnord-base/io/fileutil.h>
#include <fnord-base/uri.h>
#include <fnord-base/util/Base64.h>
#include <fnord-base/util/binarymessagewriter.h>
#include <fnord-base/wallclock.h>
#include <fnord-msg/MessageEncoder.h>

namespace fnord {
namespace tsdb {

RefPtr<StreamChunk> StreamChunk::create(
    const String& streamchunk_key,
    const String& stream_key,
    RefPtr<StreamProperties> config,
    TSDBNodeRef* node) {
  return RefPtr<StreamChunk>(
      new StreamChunk(
          streamchunk_key,
          stream_key,
          config,
          node));
}

RefPtr<StreamChunk> StreamChunk::reopen(
    const String& stream_key,
    const StreamChunkState& state,
    RefPtr<StreamProperties> config,
    TSDBNodeRef* node) {
  return RefPtr<StreamChunk>(
      new StreamChunk(
          stream_key,
          state,
          config,
          node));
}

String StreamChunk::streamChunkKeyFor(
    const String& stream_key,
    DateTime time,
    const StreamProperties& properties) {
  util::BinaryMessageWriter buf(stream_key.size() + 32);

  auto cs = properties.chunk_size.microseconds();
  auto ts = (time.unixMicros() / cs) * cs / kMicrosPerSecond;

  buf.append(stream_key.data(), stream_key.size());
  buf.appendUInt8(27);
  buf.appendVarUInt(ts);

  return String((char *) buf.data(), buf.size());
}

Vector<String> StreamChunk::streamChunkKeysFor(
    const String& stream_key,
    DateTime from,
    DateTime until,
    const StreamProperties& properties) {
  auto cs = properties.chunk_size.microseconds();
  auto first_chunk = (from.unixMicros() / cs) * cs;
  auto last_chunk = (until.unixMicros() / cs) * cs;

  Vector<String> res;
  for (auto t = first_chunk; t <= last_chunk; t += cs) {
    res.emplace_back(streamChunkKeyFor(stream_key, t, properties));
  }

  return res;
}

StreamChunk::StreamChunk(
    const String& streamchunk_key,
    const String& stream_key,
    RefPtr<StreamProperties> config,
    TSDBNodeRef* node) :
    stream_key_(stream_key),
    key_(streamchunk_key),
    config_(config),
    node_(node),
    records_(
        config->schema,
        FileUtil::joinPaths(
            node->db_path,
            StringUtil::stripShell(stream_key) + ".")),
    replication_scheduled_(false),
    compaction_scheduled_(false),
    last_compaction_(0) {
  records_.setMaxDatafileSize(config_->max_datafile_size);
}

StreamChunk::StreamChunk(
    const String& streamchunk_key,
    const StreamChunkState& state,
    RefPtr<StreamProperties> config,
    TSDBNodeRef* node) :
    stream_key_(state.stream_key),
    key_(streamchunk_key),
    config_(config),
    node_(node),
    records_(
        config->schema,
        FileUtil::joinPaths(
            node->db_path,
            StringUtil::stripShell(state.stream_key) + "."),
        state.record_state),
    replication_scheduled_(false),
    compaction_scheduled_(false),
    last_compaction_(0) {
  if (records_.commitlogSize() > 0) {
    scheduleCompaction();
  }

  node_->replicationq.insert(this, WallClock::unixMicros());
  records_.setMaxDatafileSize(config_->max_datafile_size);
}

void StreamChunk::insertRecord(
    uint64_t record_id,
    const Buffer& record) {
  std::unique_lock<std::mutex> lk(mutex_);

  auto old_ver = records_.version();
  records_.addRecord(record_id, record);
  if (records_.version() != old_ver) {
    commitState();
  }

  scheduleCompaction();
}

void StreamChunk::scheduleCompaction() {
  if (compaction_scheduled_) {
    return;
  }

  auto now = WallClock::unixMicros();
  auto interval = config_->compaction_interval.microseconds();
  auto next = last_compaction_.unixMicros() + interval;

  auto compaction_delay = 0;
  if (next > now) {
    compaction_delay = next - now;
  }

  node_->compactionq.insert(this, now + compaction_delay);
  compaction_scheduled_ = true;
}

void StreamChunk::compact() {
  std::unique_lock<std::mutex> lk(mutex_);
  compaction_scheduled_ = false;
  last_compaction_ = DateTime::now();
  lk.unlock();

  Set<String> deleted_files;
  records_.compact(&deleted_files);

  lk.lock();
  commitState();
  lk.unlock();

  node_->replicationq.insert(this, WallClock::unixMicros());

  for (const auto& f : deleted_files) {
    FileUtil::rm(f);
  }
}

void StreamChunk::replicate() {
  std::unique_lock<std::mutex> lk(replication_mutex_);

  auto cur_offset = records_.numRecords();
  auto replicas = node_->replication_scheme->replicasFor(key_);
  bool dirty = false;
  bool needs_replication = false;
  bool has_error = false;

  for (const auto& r : replicas) {
    auto& off = replicated_offsets_[r.unique_id];

    if (off < cur_offset) {
      try {
        auto num_replicated = replicateTo(r.addr, off);
        dirty = true;
        off += num_replicated;
        if (off < cur_offset) {
          needs_replication = true;
        }
      } catch (const std::exception& e) {
        has_error = true;

        fnord::logError(
          "tsdb.replication",
          e,
          "Error while replicating stream '$0' to '$1'",
          stream_key_,
          r.addr);
      }
    }
  }

  if (dirty) {
    std::unique_lock<std::mutex> lk(mutex_);
    commitState();
  }

  if (needs_replication) {
    node_->replicationq.insert(this, WallClock::unixMicros());
  } else if (has_error) {
    node_->replicationq.insert(
        this,
        WallClock::unixMicros() + 30 * kMicrosPerSecond);
  }
}

uint64_t StreamChunk::replicateTo(const String& addr, uint64_t offset) {
  util::BinaryMessageWriter batch;
  size_t batch_size = 100;
  size_t n = 0;

  records_.fetchRecords(offset, batch_size, [this, &batch, &n] (
      uint64_t record_id,
      const msg::MessageObject& message) {
    ++n;

    Buffer msg_buf;
    msg::MessageEncoder::encode(message, *config_->schema, &msg_buf);

    batch.appendUInt64(record_id);
    batch.appendVarUInt(msg_buf.size());
    batch.append(msg_buf.data(), msg_buf.size());
  });

  String encoded_key;
  util::Base64::encode(key_, &encoded_key);
  URI uri(StringUtil::format(
      "http://$0/tsdb/replicate?stream=$1&chunk=$2",
      addr,
      URI::urlEncode(stream_key_),
      URI::urlEncode(encoded_key)));

  http::HTTPRequest req(http::HTTPMessage::M_POST, uri.pathAndQuery());
  req.addHeader("Host", uri.hostAndPort());
  req.addHeader("Content-Type", "application/fnord-msg");
  req.addBody(batch.data(), batch.size());

  auto res = node_->http->executeRequest(req);
  res.wait();

  const auto& r = res.get();
  if (r.statusCode() != 201) {
    RAISEF(kRuntimeError, "received non-201 response: $0", r.body().toString());
  }

  return n;
}

Vector<String> StreamChunk::listFiles() const {
  return records_.listDatafiles();
}

void StreamChunk::commitState() {
  StreamChunkState state;
  state.record_state = records_.getState();
  state.stream_key = stream_key_;

  util::BinaryMessageWriter buf;
  state.encode(&buf);

  auto txn = node_->db->startTransaction(false);
  txn->update(key_.data(), key_.size(), buf.data(), buf.size());
  txn->commit();
}

void StreamChunkState::encode(
    util::BinaryMessageWriter* writer) const {
  writer->appendLenencString(stream_key);
  record_state.encode(writer);
}

void StreamChunkState::decode(util::BinaryMessageReader* reader) {
  stream_key = reader->readLenencString();
  record_state.decode(reader);
}

}
}
