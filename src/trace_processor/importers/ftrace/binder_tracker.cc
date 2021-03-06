/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/trace_processor/importers/ftrace/binder_tracker.h"
#include "perfetto/base/compiler.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

namespace {
constexpr int kOneWay = 0x01;
constexpr int kRootObject = 0x04;
constexpr int kStatusCode = 0x08;
constexpr int kAcceptFds = 0x10;
constexpr int kNoFlags = 0;

std::string BinderFlagsToHuman(uint32_t flag) {
  std::string str;
  if (flag & kOneWay) {
    str += "this is a one-way call: async, no return; ";
  }
  if (flag & kRootObject) {
    str += "contents are the components root object; ";
  }
  if (flag & kStatusCode) {
    str += "contents are a 32-bit status code; ";
  }
  if (flag & kAcceptFds) {
    str += "allow replies with file descriptors; ";
  }
  if (flag == kNoFlags) {
    str += "No Flags Set";
  }
  return str;
}

}  // namespace

BinderTracker::BinderTracker(TraceProcessorContext* context)
    : context_(context),
      binder_category_id_(context->storage->InternString("binder")),
      lock_waiting_id_(context->storage->InternString("binder lock waiting")),
      lock_held_id_(context->storage->InternString("binder lock held")),
      transaction_slice_id_(
          context->storage->InternString("binder transaction")),
      transaction_async_id_(
          context->storage->InternString("binder transaction async")),
      reply_id_(context->storage->InternString("binder reply")),
      async_rcv_id_(context->storage->InternString("binder async rcv")),
      transaction_id_(context->storage->InternString("transaction id")),
      dest_node_(context->storage->InternString("destination node")),
      dest_process_(context->storage->InternString("destination process")),
      dest_thread_(context->storage->InternString("destination thread")),
      dest_name_(context->storage->InternString("destination name")),
      is_reply_(context->storage->InternString("reply transaction?")),
      flags_(context->storage->InternString("flags")),
      code_(context->storage->InternString("code")),
      calling_tid_(context->storage->InternString("calling tid")),
      dest_slice_id_(context->storage->InternString("destination slice id")),
      data_size_(context->storage->InternString("data size")),
      offsets_size_(context->storage->InternString("offsets size")) {}

BinderTracker::~BinderTracker() = default;

void BinderTracker::Transaction(int64_t ts,
                                uint32_t tid,
                                int32_t transaction_id,
                                int32_t dest_node,
                                int32_t dest_tgid,
                                int32_t dest_tid,
                                bool is_reply,
                                uint32_t flags,
                                StringId code) {
  UniqueTid src_utid = context_->process_tracker->GetOrCreateThread(tid);
  TrackId track_id = context_->track_tracker->InternThreadTrack(src_utid);

  auto args_inserter = [this, transaction_id, dest_node, dest_tgid, is_reply,
                        flags, code,
                        tid](ArgsTracker::BoundInserter* inserter) {
    inserter->AddArg(transaction_id_, Variadic::Integer(transaction_id));
    inserter->AddArg(dest_node_, Variadic::Integer(dest_node));
    inserter->AddArg(dest_process_, Variadic::Integer(dest_tgid));
    inserter->AddArg(is_reply_, Variadic::Boolean(is_reply));
    std::string flag_str =
        base::IntToHexString(flags) + " " + BinderFlagsToHuman(flags);
    inserter->AddArg(flags_, Variadic::String(context_->storage->InternString(
                                 base::StringView(flag_str))));
    inserter->AddArg(code_, Variadic::String(code));
    inserter->AddArg(calling_tid_, Variadic::UnsignedInteger(tid));
    // TODO(hjd): The legacy UI included the calling pid in the args,
    // is this necessary? It's complicated in our case because process
    // association might not happen until after the binder transaction slices
    // have been parsed. We would need to backfill the arg.
  };

  if (is_reply) {
    // Reply slices have accurate dest information, so we can add it.
    const auto& thread_table = context_->storage->thread_table();
    UniqueTid utid = context_->process_tracker->GetOrCreateThread(
        static_cast<uint32_t>(dest_tid));
    StringId dest_thread_name = thread_table.name()[utid];
    auto dest_args_inserter = [this, dest_tid, &dest_thread_name](
                                  ArgsTracker::BoundInserter* inserter) {
      inserter->AddArg(dest_thread_, Variadic::Integer(dest_tid));
      inserter->AddArg(dest_name_, Variadic::String(dest_thread_name));
    };
    context_->slice_tracker->AddArgs(track_id, binder_category_id_, reply_id_,
                                     dest_args_inserter);
    context_->slice_tracker->End(ts, track_id, kNullStringId, kNullStringId,
                                 args_inserter);
    awaiting_rcv_for_reply_.insert(transaction_id);
    return;
  }

  bool expects_reply = !is_reply && ((flags & kOneWay) == 0);

  if (expects_reply) {
    context_->slice_tracker->Begin(ts, track_id, binder_category_id_,
                                   transaction_slice_id_, args_inserter);
    transaction_await_rcv[transaction_id] = track_id;
  } else {
    context_->slice_tracker->Scoped(ts, track_id, binder_category_id_,
                                    transaction_async_id_, 0, args_inserter);
    awaiting_async_rcv_[transaction_id] = args_inserter;
  }
}

void BinderTracker::TransactionReceived(int64_t ts,
                                        uint32_t pid,
                                        int32_t transaction_id) {
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);
  const auto& thread_table = context_->storage->thread_table();
  StringId thread_name = thread_table.name()[utid];
  TrackId track_id = context_->track_tracker->InternThreadTrack(utid);
  if (awaiting_rcv_for_reply_.count(transaction_id) > 0) {
    context_->slice_tracker->End(ts, track_id);
    awaiting_rcv_for_reply_.erase(transaction_id);
    return;
  }

  TrackId* rcv_track_id = transaction_await_rcv.Find(transaction_id);
  if (rcv_track_id) {
    // First begin the reply slice to get its slice id.
    auto reply_slice_id = context_->slice_tracker->Begin(
        ts, track_id, binder_category_id_, reply_id_);
    // Add accurate dest info to the binder transaction slice.
    auto args_inserter = [this, pid, &thread_name, &reply_slice_id](
                             ArgsTracker::BoundInserter* inserter) {
      inserter->AddArg(dest_thread_, Variadic::UnsignedInteger(pid));
      inserter->AddArg(dest_name_, Variadic::String(thread_name));
      if (reply_slice_id.has_value())
        inserter->AddArg(dest_slice_id_,
                         Variadic::UnsignedInteger(reply_slice_id->value));
    };
    // Add the dest args to the current transaction slice and get the slice id.
    auto transaction_slice_id =
        context_->slice_tracker->AddArgs(*rcv_track_id, binder_category_id_,
                                         transaction_slice_id_, args_inserter);

    // Add the dest slice id to the reply slice that has just begun.
    auto reply_dest_inserter =
        [this, &transaction_slice_id](ArgsTracker::BoundInserter* inserter) {
          if (transaction_slice_id.has_value())
            inserter->AddArg(dest_slice_id_, Variadic::UnsignedInteger(
                                                 transaction_slice_id.value()));
        };
    context_->slice_tracker->AddArgs(track_id, binder_category_id_, reply_id_,
                                     reply_dest_inserter);
    transaction_await_rcv.Erase(transaction_id);
    return;
  }

  SetArgsCallback* args = awaiting_async_rcv_.Find(transaction_id);
  if (args) {
    context_->slice_tracker->Scoped(ts, track_id, binder_category_id_,
                                    async_rcv_id_, 0, *args);
    awaiting_async_rcv_.Erase(transaction_id);
    return;
  }
}

void BinderTracker::Lock(int64_t ts, uint32_t pid) {
  attempt_lock_[pid] = ts;

  UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);
  TrackId track_id = context_->track_tracker->InternThreadTrack(utid);
  context_->slice_tracker->Begin(ts, track_id, binder_category_id_,
                                 lock_waiting_id_);
}

void BinderTracker::Locked(int64_t ts, uint32_t pid) {
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);

  if (!attempt_lock_.Find(pid))
    return;

  TrackId track_id = context_->track_tracker->InternThreadTrack(utid);
  context_->slice_tracker->End(ts, track_id);
  context_->slice_tracker->Begin(ts, track_id, binder_category_id_,
                                 lock_held_id_);

  lock_acquired_[pid] = ts;
  attempt_lock_.Erase(pid);
}

void BinderTracker::Unlock(int64_t ts, uint32_t pid) {
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);

  if (!lock_acquired_.Find(pid))
    return;

  TrackId track_id = context_->track_tracker->InternThreadTrack(utid);
  context_->slice_tracker->End(ts, track_id, binder_category_id_,
                               lock_held_id_);
  lock_acquired_.Erase(pid);
}

void BinderTracker::TransactionAllocBuf(int64_t ts,
                                        uint32_t pid,
                                        uint64_t data_size,
                                        uint64_t offsets_size) {
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);
  TrackId track_id = context_->track_tracker->InternThreadTrack(utid);

  auto args_inserter = [this, &data_size,
                        offsets_size](ArgsTracker::BoundInserter* inserter) {
    inserter->AddArg(data_size_, Variadic::UnsignedInteger(data_size));
    inserter->AddArg(offsets_size_, Variadic::UnsignedInteger(offsets_size));
  };
  context_->slice_tracker->AddArgs(track_id, binder_category_id_,
                                   transaction_slice_id_, args_inserter);

  base::ignore_result(ts);
}

}  // namespace trace_processor
}  // namespace perfetto
