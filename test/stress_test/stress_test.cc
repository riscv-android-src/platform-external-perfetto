/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include <signal.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <chrono>
#include <list>
#include <map>
#include <random>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/subprocess.h"
#include "perfetto/ext/base/temp_file.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/tracing.h"
#include "perfetto/tracing/core/forward_decls.h"
#include "perfetto/tracing/core/trace_config.h"
#include "src/base/test/utils.h"

#include "protos/perfetto/config/stress_test_config.gen.h"
#include "protos/perfetto/trace/test_event.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

// Generated by gen_configs_blob.py. It defines the kStressTestConfigs array,
// which contains a proto-encoded StressTestConfig message for each .cfg file
// listed in /test/stress_test/configs/BUILD.gn.
#include "test/stress_test/configs/stress_test_config_blobs.h"

namespace perfetto {
namespace {

using StressTestConfig = protos::gen::StressTestConfig;

struct SigHandlerCtx {
  std::atomic<bool> aborted{};
  std::vector<int> pids_to_kill;
};
SigHandlerCtx* g_sig;

struct TestResult {
  const char* cfg_name = nullptr;
  StressTestConfig cfg;
  uint32_t run_time_ms = 0;
  uint32_t trace_size_kb = 0;
  uint32_t num_packets = 0;
  uint32_t num_threads = 0;
  uint32_t num_errors = 0;
  base::Subprocess::ResourceUsage svc_rusage;
  base::Subprocess::ResourceUsage prod_rusage;
};

struct ParsedTraceStats {
  struct WriterThread {
    uint64_t packets_seen = 0;
    bool last_seen = false;
    uint32_t last_seq = 0;
    uint64_t seq_errors = 0;
    uint64_t counter_errors = 0;
    std::minstd_rand0 rnd_engine;
  };

  // One for each trusted_packet_sequence_id.
  std::map<uint32_t, WriterThread> threads;
};

class TestHarness {
 public:
  TestHarness();
  void RunConfig(const char* cfg_name, const StressTestConfig&, bool verbose);
  const std::list<TestResult>& test_results() const { return test_results_; }

 private:
  void ReadbackTrace(const std::string&, ParsedTraceStats*);
  void ParseTracePacket(const uint8_t*, size_t, ParsedTraceStats* ctx);
  void AddFailure(const char* fmt, ...) PERFETTO_PRINTF_FORMAT(2, 3);

  std::vector<std::string> env_;
  std::list<TestResult> test_results_;
  std::string results_dir_;
  base::ScopedFile error_log_;
};

TestHarness::TestHarness() {
  results_dir_ = base::GetSysTempDir() + "/perfetto-stress-test";
  system(("rm -r -- \"" + results_dir_ + "\"").c_str());
  PERFETTO_CHECK(mkdir(results_dir_.c_str(), 0755) == 0);
  PERFETTO_LOG("Saving test results in %s", results_dir_.c_str());
}

void TestHarness::AddFailure(const char* fmt, ...) {
  ++test_results_.back().num_errors;

  char log_msg[512];
  va_list args;
  va_start(args, fmt);
  int res = vsnprintf(log_msg, sizeof(log_msg), fmt, args);
  va_end(args);

  PERFETTO_ELOG("FAIL: %s", log_msg);

  if (res > 0 && static_cast<size_t>(res) < sizeof(log_msg) - 2) {
    log_msg[res++] = '\n';
    log_msg[res++] = '\0';
  }
  base::ignore_result(write(*error_log_, log_msg, static_cast<size_t>(res)));
}

void TestHarness::RunConfig(const char* cfg_name,
                            const StressTestConfig& cfg,
                            bool verbose) {
  test_results_.emplace_back();
  TestResult& test_result = test_results_.back();
  test_result.cfg_name = cfg_name;
  test_result.cfg = cfg;
  g_sig->pids_to_kill.clear();

  auto result_dir = results_dir_ + "/" + cfg_name;
  PERFETTO_CHECK(!mkdir(result_dir.c_str(), 0755));
  error_log_ = base::OpenFile(result_dir + "/errors.log",
                              O_RDWR | O_CREAT | O_TRUNC, 0644);

  PERFETTO_ILOG("Starting \"%s\" - %s", cfg_name, result_dir.c_str());

  env_.emplace_back("PERFETTO_PRODUCER_SOCK_NAME=" + result_dir +
                    "/producer.sock");
  env_.emplace_back("PERFETTO_CONSUMER_SOCK_NAME=" + result_dir +
                    "/consumer.sock");
  std::string bin_dir = base::GetCurExecutableDir();

  // Start the service.
  base::Subprocess traced({bin_dir + "/traced"});
  traced.args.env = env_;
  if (!verbose) {
    traced.args.out_fd = base::OpenFile(result_dir + "/traced.log",
                                        O_RDWR | O_CREAT | O_TRUNC, 0644);
    traced.args.stderr_mode = traced.args.stdout_mode = base::Subprocess::kFd;
  }
  traced.Start();
  g_sig->pids_to_kill.emplace_back(traced.pid());
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  PERFETTO_CHECK(traced.Poll() == base::Subprocess::kRunning);

  // Start the producer processes.
  std::list<base::Subprocess> producers;
  for (uint32_t i = 0; i < cfg.num_processes(); ++i) {
    producers.emplace_back(base::Subprocess({bin_dir + "/stress_producer"}));
    auto& producer = producers.back();
    producer.args.input = cfg.SerializeAsString();
    if (!verbose) {
      producer.args.out_fd =
          base::OpenFile(result_dir + "/producer." + std::to_string(i) + ".log",
                         O_RDWR | O_CREAT | O_TRUNC, 0644);
      producer.args.stderr_mode = producer.args.stdout_mode =
          base::Subprocess::kFd;
    }
    producer.args.env = env_;
    producer.Start();
    g_sig->pids_to_kill.emplace_back(producer.pid());
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  for (auto& producer : producers)
    PERFETTO_CHECK(producer.Poll() == base::Subprocess::kRunning);

  auto trace_file_path = result_dir + "/trace";
  base::Subprocess consumer(
      {bin_dir + "/perfetto", "-c", "-", "-o", trace_file_path.c_str()});
  consumer.args.env = env_;
  consumer.args.input = cfg.trace_config().SerializeAsString();
  if (!verbose) {
    consumer.args.out_fd = base::OpenFile(result_dir + "/perfetto.log",
                                          O_RDWR | O_CREAT | O_TRUNC, 0644);
    consumer.args.stderr_mode = consumer.args.stdout_mode =
        base::Subprocess::kFd;
  }
  unlink(trace_file_path.c_str());
  consumer.Start();
  int64_t t_start = base::GetBootTimeNs().count();
  g_sig->pids_to_kill.emplace_back(consumer.pid());

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  PERFETTO_CHECK(consumer.Poll() == base::Subprocess::kRunning);

  if (!consumer.Wait(
          static_cast<int>(cfg.trace_config().duration_ms() + 30000))) {
    AddFailure("Consumer didn't quit in time");
    consumer.KillAndWaitForTermination();
  }

  // Stop
  consumer.KillAndWaitForTermination(SIGTERM);
  int64_t t_end = base::GetBootTimeNs().count();

  for (auto& producer : producers) {
    producer.KillAndWaitForTermination();
    test_result.prod_rusage = producer.rusage();  // Only keep last one
  }
  producers.clear();
  traced.KillAndWaitForTermination();

  test_result.svc_rusage = traced.rusage();
  test_result.run_time_ms = static_cast<uint32_t>((t_end - t_start) / 1000000);

  // Verify
  // TODO(primiano): read back the TraceStats and check them as well.
  ParsedTraceStats ctx;
  ReadbackTrace(trace_file_path, &ctx);
  auto exp_thd = cfg.num_processes() * cfg.num_threads();
  if (ctx.threads.size() != exp_thd) {
    AddFailure("Trace threads mismatch. Expected %u threads, got %zu", exp_thd,
               ctx.threads.size());
  }
  for (const auto& it : ctx.threads) {
    uint32_t seq_id = it.first;
    const auto& thd = it.second;
    if (!thd.last_seen) {
      AddFailure("Last packet not seen for sequence %u", seq_id);
    }
    if (thd.seq_errors > 0) {
      AddFailure("Sequence %u had %" PRIu64 " packets out of sync", seq_id,
                 thd.seq_errors);
    }
    if (thd.counter_errors > 0) {
      AddFailure("Sequence %u had %" PRIu64 " packets counter errors", seq_id,
                 thd.counter_errors);
    }
  }

  error_log_.reset();
  PERFETTO_ILOG("Completed \"%s\"", cfg_name);
}

void TestHarness::ReadbackTrace(const std::string& trace_file_path,
                                ParsedTraceStats* ctx) {
  TestResult& test_result = test_results_.back();
  using namespace protozero::proto_utils;
  auto fd = base::OpenFile(trace_file_path.c_str(), O_RDONLY);
  if (!fd)
    return AddFailure("Trace file does not exist");
  const off_t file_size = lseek(*fd, 0, SEEK_END);
  if (file_size <= 0)
    return AddFailure("Trace file is empty");
  test_result.trace_size_kb = static_cast<uint32_t>(file_size / 1000);
  const uint8_t* const start = static_cast<const uint8_t*>(mmap(
      nullptr, static_cast<size_t>(file_size), PROT_READ, MAP_PRIVATE, *fd, 0));
  PERFETTO_CHECK(start != MAP_FAILED);
  const uint8_t* const end = start + file_size;

  constexpr uint8_t kTracePacketTag = MakeTagLengthDelimited(1);

  for (auto* ptr = start; (end - ptr) > 2;) {
    const uint8_t* tokenizer_start = ptr;
    if (*(ptr++) != kTracePacketTag) {
      return AddFailure("Tokenizer failure at offset %zd", ptr - start);
    }
    uint64_t packet_size = 0;
    ptr = ParseVarInt(ptr, end, &packet_size);
    const uint8_t* const packet_start = ptr;
    ptr += packet_size;
    if ((ptr - tokenizer_start) < 2 || ptr > end)
      return AddFailure("Got invalid packet size %" PRIu64 " at offset %zd",
                        packet_size,
                        static_cast<ssize_t>(packet_start - start));
    ParseTracePacket(packet_start, static_cast<size_t>(packet_size), ctx);
  }
  test_result.num_threads = static_cast<uint32_t>(ctx->threads.size());
}

void TestHarness::ParseTracePacket(const uint8_t* start,
                                   size_t size,
                                   ParsedTraceStats* ctx) {
  TestResult& test_result = test_results_.back();
  protos::pbzero::TracePacket::Decoder packet(start, size);
  if (!packet.has_for_testing())
    return;

  ++test_result.num_packets;
  const uint32_t seq_id = packet.trusted_packet_sequence_id();

  protos::pbzero::TestEvent::Decoder te(packet.for_testing());
  auto t_it = ctx->threads.find(seq_id);
  bool is_first_packet = false;
  if (t_it == ctx->threads.end()) {
    is_first_packet = true;
    t_it = ctx->threads.emplace(seq_id, ParsedTraceStats::WriterThread()).first;
  }
  ParsedTraceStats::WriterThread& thd = t_it->second;

  ++thd.packets_seen;
  if (te.is_last()) {
    if (thd.last_seen) {
      return AddFailure(
          "last_seen=true happened more than once for sequence %u", seq_id);
    } else {
      thd.last_seen = true;
    }
  }
  if (is_first_packet) {
    thd.rnd_engine = std::minstd_rand0(te.seq_value());
  } else {
    const uint32_t expected = static_cast<uint32_t>(thd.rnd_engine());
    if (te.seq_value() != expected) {
      thd.rnd_engine = std::minstd_rand0(te.seq_value());  // Resync the engine.
      ++thd.seq_errors;
      return AddFailure(
          "TestEvent seq mismatch for sequence %u. Expected %u got %u", seq_id,
          expected, te.seq_value());
    }
    if (te.counter() != thd.packets_seen) {
      return AddFailure(
          "TestEvent counter mismatch for sequence %u. Expected %" PRIu64
          " got %" PRIu64,
          seq_id, thd.packets_seen, te.counter());
    }
  }

  if (!te.has_payload()) {
    return AddFailure("TestEvent %u for sequence %u has no payload",
                      te.seq_value(), seq_id);
  }

  // Check the validity of the payload. The payload might be nested. If that is
  // the case, we need to check all levels.
  protozero::ConstBytes payload_bounds = te.payload();
  for (uint32_t depth = 0, last_depth = 0;; depth++) {
    if (depth > 100) {
      return AddFailure("Unexpectedly deep depth for event %u, sequence %u",
                        te.seq_value(), seq_id);
    }
    protos::pbzero::TestEvent::TestPayload::Decoder payload(payload_bounds);
    const uint32_t rem_depth = payload.remaining_nesting_depth();

    // The payload is a repeated field and must have exactly two instances.
    // The writer splits it always in two halves of identical size.
    int num_payload_pieces = 0;
    size_t last_size = 0;
    for (auto it = payload.str(); it; ++it, ++num_payload_pieces) {
      protozero::ConstChars payload_str = *it;
      last_size = last_size ? last_size : payload_str.size;
      if (payload_str.size != last_size) {
        return AddFailure(
            "Asymmetrical payload at depth %u, event id %u, sequence %u. "
            "%zu != %zu",
            depth, te.seq_value(), seq_id, last_size, payload_str.size);
      }
      // Check that the payload content matches the expected sequence.
      for (size_t i = 0; i < payload_str.size; i++) {
        char exp = static_cast<char>(33 + ((te.seq_value() + i) % 64));
        if (payload_str.data[i] != exp) {
          return AddFailure(
              "Payload mismatch at %zu, depth %u, event id %u, sequence %u. "
              "Expected: 0x%x, Actual: 0x%x",
              i, depth, te.seq_value(), seq_id, exp, payload_str.data[i]);
        }
      }
    }
    if (num_payload_pieces != 2) {
      return AddFailure(
          "Broken payload at depth %u, event id %u, sequence %u. "
          "Expecting 2 repeated str fields, got %d",
          depth, te.seq_value(), seq_id, num_payload_pieces);
    }

    if (depth > 0 && rem_depth != last_depth - 1) {
      return AddFailure(
          "Unexpected nesting level (expected: %u, actual: %u) at depth %u, "
          "event id %u, sequence %u",
          rem_depth, last_depth - 1, depth, te.seq_value(), seq_id);
    }

    last_depth = rem_depth;
    if (rem_depth == 0)
      break;
    if (payload.has_nested()) {
      payload_bounds = *payload.nested();
    } else {
      payload_bounds = {nullptr, 0};
    }
  }
}

void CtrlCHandler(int) {
  g_sig->aborted.store(true);
  for (auto it = g_sig->pids_to_kill.rbegin(); it != g_sig->pids_to_kill.rend();
       it++) {
    kill(*it, SIGKILL);
  }
}

void StressTestMain(int argc, char** argv) {
  TestHarness th;
  std::regex filter;
  bool has_filter = false;

  bool verbose = false;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "-v")) {
      verbose = true;
    } else {
      filter = std::regex(argv[i], std::regex::ECMAScript | std::regex::icase);
      has_filter = true;
    }
  }

  g_sig = new SigHandlerCtx();
  signal(SIGINT, CtrlCHandler);

  for (size_t i = 0; i < base::ArraySize(kStressTestConfigs) && !g_sig->aborted;
       ++i) {
    const auto& cfg_blob = kStressTestConfigs[i];
    StressTestConfig cfg;
    std::cmatch ignored;
    if (has_filter && !std::regex_search(cfg_blob.name, ignored, filter)) {
      continue;
    }
    PERFETTO_CHECK(cfg.ParseFromArray(cfg_blob.data, cfg_blob.size));
    th.RunConfig(cfg_blob.name, cfg, verbose);
  }

  for (const auto& tres : th.test_results()) {
    const auto& cfg = tres.cfg;
    printf("===============================================================\n");
    printf("Config: %s\n", tres.cfg_name);
    printf("===============================================================\n");
    printf("%-20s %-10s %-10s\n", "Metric", "Expected", "Actual");
    printf("%-20s %-10s %-10s\n", "------", "--------", "------");
    printf("%-20s %-10d %-10d\n", "#Errors", 0, tres.num_errors);
    printf("%-20s %-10d %-10d \n", "Duration [ms]",
           cfg.trace_config().duration_ms(), tres.run_time_ms);

    uint32_t exp_threads = cfg.num_processes() * cfg.num_threads();
    printf("%-20s %-10u %-10u\n", "Num threads", exp_threads, tres.num_threads);

    double dur_s = cfg.trace_config().duration_ms() / 1e3;
    double exp_per_thread = cfg.steady_state_timings().rate_mean() * dur_s;
    if (cfg.burst_period_ms()) {
      double burst_rate = 1.0 * cfg.burst_duration_ms() / cfg.burst_period_ms();
      exp_per_thread *= 1.0 - burst_rate;
      exp_per_thread += burst_rate * cfg.burst_timings().rate_mean() * dur_s;
    }
    if (cfg.max_events())
      exp_per_thread = std::min(exp_per_thread, 1.0 * cfg.max_events());
    double exp_packets = std::round(exp_per_thread * exp_threads);
    printf("%-20s %-10.0f %-10d\n", "Num packets", exp_packets,
           tres.num_packets);

    double exp_size_kb = exp_packets * (cfg.nesting() + 1) *
                         (cfg.steady_state_timings().payload_mean() + 40) /
                         1000;
    printf("%-20s ~%-9.0f %-10d\n", "Trace size [KB]", exp_size_kb,
           tres.trace_size_kb);

    double exp_rss_mb = cfg.trace_config().buffers()[0].size_kb() / 1000;
    printf("%-20s (max) %-4.0f %-10d\n", "Svc RSS [MB]", exp_rss_mb,
           tres.svc_rusage.max_rss_kb / 1000);
    printf("%-20s %-10s %-10d\n", "Svc CPU [ms]", "---",
           tres.svc_rusage.cpu_time_ms());
    printf("%-20s %-10s %d / %d\n", "Svc #ctxswitch", "---",
           tres.svc_rusage.invol_ctx_switch, tres.svc_rusage.vol_ctx_switch);

    printf("%-20s %-10s %-10d\n", "Prod RSS [MB]", "---",
           tres.prod_rusage.max_rss_kb / 1000);
    printf("%-20s %-10s %-10d\n", "Prod CPU [ms]", "---",
           tres.prod_rusage.cpu_time_ms());
    printf("%-20s %-10s %d / %d\n", "Prod #ctxswitch", "---",
           tres.prod_rusage.invol_ctx_switch, tres.prod_rusage.vol_ctx_switch);
    printf("\n");
  }
}

}  // namespace
}  // namespace perfetto

int main(int argc, char** argv) {
  perfetto::StressTestMain(argc, argv);
}
