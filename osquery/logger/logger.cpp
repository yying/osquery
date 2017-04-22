/*
 *  Copyright (c) 2014-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#ifndef WIN32
#include <syslog.h>
#endif

#include <algorithm>
#include <future>
#include <queue>
#include <thread>

#include <boost/noncopyable.hpp>

#include <osquery/events.h>
#include <osquery/extensions.h>
#include <osquery/filesystem.h>
#include <osquery/flags.h>
#include <osquery/logger.h>
#include <osquery/system.h>

#include "osquery/core/conversions.h"
#include "osquery/core/json.h"

namespace pt = boost::property_tree;

namespace osquery {

FLAG(bool, verbose, false, "Enable verbose informational messages");
FLAG_ALIAS(bool, verbose_debug, verbose);
FLAG_ALIAS(bool, debug, verbose);

/// Despite being a configurable option, this is only read/used at load.
FLAG(bool, disable_logging, false, "Disable ERROR/INFO logging");

FLAG(string, logger_plugin, "filesystem", "Logger plugin name");

FLAG(bool, logger_event_type, true, "Log scheduled results as events");
FLAG_ALIAS(bool, log_result_events, logger_event_type);

/// Alias for the minloglevel used internally by GLOG.
FLAG(int32, logger_min_status, 0, "Minimum level for status log recording");

FLAG(bool,
     logger_secondary_status_only,
     false,
     "Only send status logs to secondary logger plugins");

/**
 * @brief This hidden flag is for testing status logging.
 *
 * When enabled, logs are pushed directly to logger plugin from Glog.
 * Otherwise they are buffered and an async request for draining is sent
 * for each log.
 *
 * Within the daemon, logs are drained every 3 seconds.
 */
HIDDEN_FLAG(bool,
            logger_status_sync,
            false,
            "Always send status logs synchronously");

/**
 * @brief Logger plugin registry.
 *
 * This creates an osquery registry for "logger" which may implement
 * LoggerPlugin. Only strings are logged in practice, and LoggerPlugin provides
 * a helper member for transforming PluginRequest%s to strings.
 */
CREATE_REGISTRY(LoggerPlugin, "logger");

class LoggerDisabler;

/**
 * @brief A custom Glog log sink for forwarding or buffering status logs.
 *
 * This log sink has two modes, it can buffer Glog status logs until an osquery
 * logger is initialized or forward Glog status logs to an initialized and
 * appropriate logger. The appropriateness is determined by the logger when its
 * LoggerPlugin::init method is called. If the `init` method returns success
 * then a BufferedLogSink will start forwarding status logs to
 * LoggerPlugin::logStatus.
 *
 * This facility will start buffering when first used and stop buffering
 * (aka remove itself as a Glog sink) using the exposed APIs. It will live
 * throughout the life of the process for two reasons: (1) It makes sense when
 * the active logger plugin is handling Glog status logs and (2) it must remove
 * itself as a Glog target.
 */
class BufferedLogSink : public google::LogSink, private boost::noncopyable {
 public:
  /// We create this as a Singleton for proper disable/shutdown.
  static BufferedLogSink& instance() {
    static BufferedLogSink sink;
    return sink;
  }

  /// The Glog-API LogSink call-in method.
  void send(google::LogSeverity severity,
            const char* full_filename,
            const char* base_filename,
            int line,
            const struct ::tm* tm_time,
            const char* message,
            size_t message_len) override;

  /// Pop from the aync sender queue and wait for one send to complete.
  void WaitTillSent() override;

 public:
  /// Accessor/mutator to dump all of the buffered logs.
  static std::vector<StatusLogLine>& dump() {
    return instance().logs_;
  }

  /// Remove the buffered log sink from Glog.
  static void disable();

  /// Add the buffered log sink to Glog.
  static void enable();

  /// Start the Buffered Sink, without enabling forwarding to loggers.
  static void setUp();

  /**
   * @brief Add a logger plugin that should receive status updates.
   *
   * Since the logger may support multiple active logger plugins the sink
   * will keep track of those plugins that returned success after ::init.
   * This list of plugins will received forwarded messages from the sink.
   *
   * This list is important because sending logs to plugins that also use
   * and active Glog Sink (supports multiple) will create a logging loop.
   */
  static void addPlugin(const std::string& name) {
    instance().sinks_.push_back(name);
  }

  static void resetPlugins() {
    instance().sinks_.clear();
  }

  /// Retrieve the list of enabled plugins that should have logs forwarded.
  static const std::vector<std::string>& enabledPlugins() {
    return instance().sinks_;
  }

  /**
   * @brief Check if a given logger plugin was the first or 'primary'.
   *
   * Within the osquery core the BufferedLogSink acts as a router for status
   * logs. While initializing it inspects the set of logger plugins and saves
   * the first as the 'primary'.
   *
   * Checks within the core may act on this state. Checks within extensions
   * cannot, and thus any check for primary logger plugins is true.
   * While this is a limitation, in practice if a remote logger plugin is called
   * it is intended to receive all logging data.
   *
   * @param plugin Check if this target plugin is primary.
   * @return true of the provided plugin was the first specified.
   */
  static bool isPrimaryLogger(const std::string& plugin) {
    auto& self = instance();
    WriteLock lock(self.primary_mutex_);
    return (self.primary_.empty() || plugin == self.primary_);
  }

  /// Set the primary logger plugin is none has been previously specified.
  static void setPrimary(const std::string& plugin) {
    auto& self = instance();
    WriteLock lock(self.primary_mutex_);
    if (self.primary_.empty()) {
      self.primary_ = plugin;
    }
  }

 public:
  std::queue<std::future<void>> senders;

 public:
  BufferedLogSink(BufferedLogSink const&) = delete;
  void operator=(BufferedLogSink const&) = delete;

 private:
  /// Create the log sink as buffering or forwarding.
  BufferedLogSink() : enabled_(false) {}

  /// Remove the log sink.
  ~BufferedLogSink() {
    disable();
  }

 private:
  /// Intermediate log storage until an osquery logger is initialized.
  std::vector<StatusLogLine> logs_;

  /// Is the logger temporarily disabled.
  std::atomic<bool> enabled_{false};

  bool active_{false};

  /// Track multiple loggers that should receive sinks from the send forwarder.
  std::vector<std::string> sinks_;

  /// Keep track of the first, or 'primary' logger.
  std::string primary_;

  /// Mutex for checking primary status.
  Mutex primary_mutex_;

  /// Mutex protecting activation and enabling of the buffered status logger.
  Mutex enable_mutex_;

 private:
  friend class LoggerDisabler;
};

/// Mutex protecting accesses to buffered status logs.
Mutex kBufferedLogSinkLogs;

/// Mutex protecting queued status log futures.
Mutex kBufferedLogSinkSenders;

/// Scoped helper to perform logging actions without races.
class LoggerDisabler : private boost::noncopyable {
 public:
  LoggerDisabler()
      : stderr_status_(FLAGS_logtostderr),
        enabled_(BufferedLogSink::instance().enabled_) {
    BufferedLogSink::disable();
    FLAGS_logtostderr = true;
  }

  ~LoggerDisabler() {
    // Only enable if the sink was enabled when the disabler was requested.
    if (enabled_) {
      BufferedLogSink::enable();
    }
    FLAGS_logtostderr = stderr_status_;
  }

 private:
  /// Value of the 'logtostderr' Glog status when constructed.
  bool stderr_status_;

  /// Value of the BufferedLogSink's enabled status when constructed.
  bool enabled_;
};

static void serializeIntermediateLog(const std::vector<StatusLogLine>& log,
                                     PluginRequest& request) {
  pt::ptree tree;
  for (const auto& log_item : log) {
    pt::ptree child;
    child.put("s", log_item.severity);
    child.put("f", log_item.filename);
    child.put("i", log_item.line);
    child.put("m", log_item.message);
    child.put("c", log_item.calendar_time);
    child.put("u", log_item.time);
    tree.push_back(std::make_pair("", child));
  }

  // Save the log as a request JSON string.
  std::ostringstream output;
  pt::write_json(output, tree, false);
  request["log"] = output.str();
}

static void deserializeIntermediateLog(const PluginRequest& request,
                                       std::vector<StatusLogLine>& log) {
  if (request.count("log") == 0) {
    return;
  }

  // Read the plugin request string into a JSON tree and enumerate.
  pt::ptree tree;
  try {
    std::stringstream input;
    input << request.at("log");
    pt::read_json(input, tree);
  } catch (const pt::json_parser::json_parser_error& /* e */) {
    return;
  }

  for (const auto& item : tree.get_child("")) {
    log.push_back({
        (StatusLogSeverity)item.second.get<int>("s", O_INFO),
        item.second.get<std::string>("f", "<unknown>"),
        item.second.get<int>("i", 0),
        item.second.get<std::string>("m", ""),
        item.second.get<std::string>("c", ""),
        item.second.get<size_t>("u", 0),
    });
  }
}

void setVerboseLevel() {
  if (Flag::getValue("verbose") == "true") {
    // Turn verbosity up to 1.
    // Do log DEBUG, INFO, WARNING, ERROR to their log files.
    // Do log the above and verbose=1 to stderr.
    FLAGS_minloglevel = google::GLOG_INFO;
    if (FLAGS_logger_plugin != "stdout") {
      // Special case for the stdout plugin.
      FLAGS_stderrthreshold = google::GLOG_INFO;
    }
    FLAGS_v = 1;
  } else {
    // Do NOT log INFO, WARNING, ERROR to stderr.
    // Do log only WARNING, ERROR to log sinks.
    auto default_level = google::GLOG_INFO;
    if (kToolType == ToolType::SHELL) {
      default_level = google::GLOG_WARNING;
    }

    if (Flag::isDefault("minloglevel")) {
      FLAGS_minloglevel = default_level;
    }

    if (Flag::isDefault("stderrthreshold")) {
      FLAGS_stderrthreshold = default_level;
    }
  }

  if (!Flag::isDefault("logger_min_status")) {
    long int i = 0;
    safeStrtol(Flag::getValue("logger_min_status"), 10, i);
    FLAGS_minloglevel = static_cast<decltype(FLAGS_minloglevel)>(i);
  }

  if (FLAGS_disable_logging) {
    // Do log ERROR to stderr.
    // Do NOT log INFO, WARNING, ERROR to their log files.
    FLAGS_logtostderr = true;
  }
}

void initStatusLogger(const std::string& name) {
  FLAGS_alsologtostderr = false;
  FLAGS_colorlogtostderr = true;
  FLAGS_logbufsecs = 0; // flush the log buffer immediately
  FLAGS_stop_logging_if_full_disk = true;
  FLAGS_max_log_size = 10; // max size for individual log file is 10MB
  FLAGS_logtostderr = true;

  setVerboseLevel();
  // Start the logging, and announce the daemon is starting.
  google::InitGoogleLogging(name.c_str());
  BufferedLogSink::setUp();
}

void initLogger(const std::string& name) {
  // Check if logging is disabled, if so then no need to shuttle intermediates.
  if (FLAGS_disable_logging) {
    return;
  }

  // Stop the buffering sink and store the intermediate logs.
  BufferedLogSink::disable();
  BufferedLogSink::resetPlugins();

  bool forward = false;
  PluginRequest init_request = {{"init", name}};
  PluginRequest features_request = {{"action", "features"}};
  auto logger_plugin = RegistryFactory::get().getActive("logger");
  // Allow multiple loggers, make sure each is accessible.
  for (const auto& logger : osquery::split(logger_plugin, ",")) {
    BufferedLogSink::setPrimary(logger);
    if (!RegistryFactory::get().exists("logger", logger)) {
      continue;
    }

    Registry::call("logger", logger, init_request);
    auto status = Registry::call("logger", logger, features_request);
    if ((status.getCode() & LOGGER_FEATURE_LOGSTATUS) > 0) {
      // Glog status logs are forwarded to logStatus.
      forward = true;
      // To support multiple plugins we only add the names of plugins that
      // return a success status after initialization.
      BufferedLogSink::addPlugin(logger);
    }

    if ((status.getCode() & LOGGER_FEATURE_LOGEVENT) > 0) {
      EventFactory::addForwarder(logger);
    }
  }

  if (forward) {
    // Begin forwarding after all plugins have been set up.
    BufferedLogSink::enable();
    relayStatusLogs(true);
  }
}

void BufferedLogSink::setUp() {
  auto& self = instance();
  WriteLock lock(self.enable_mutex_);

  if (!self.active_) {
    self.active_ = true;
    google::AddLogSink(&self);
  }
}

void BufferedLogSink::disable() {
  auto& self = instance();
  WriteLock lock(self.enable_mutex_);

  if (self.enabled_) {
    self.enabled_ = false;
    if (self.active_) {
      self.active_ = false;
      google::RemoveLogSink(&self);
    }
  }
}

void BufferedLogSink::enable() {
  auto& self = instance();
  WriteLock lock(self.enable_mutex_);

  if (!self.enabled_) {
    self.enabled_ = true;
    if (!self.active_) {
      self.active_ = true;
      google::AddLogSink(&self);
    }
  }
}

// NOTE: This function can be called prior to the initialization of database
// plugins.
void BufferedLogSink::send(google::LogSeverity severity,
                           const char* full_filename,
                           const char* base_filename,
                           int line,
                           const struct ::tm* tm_time,
                           const char* message,
                           size_t message_len) {
  {
    WriteLock lock(kBufferedLogSinkLogs);
    logs_.push_back({(StatusLogSeverity)severity,
                     std::string(base_filename),
                     line,
                     std::string(message, message_len),
                     toAsciiTimeUTC(tm_time),
                     toUnixTime(tm_time)});
  }

  // The daemon will relay according to the schedule.
  if (enabled_ && kToolType != ToolType::DAEMON) {
    relayStatusLogs(FLAGS_logger_status_sync);
  }
}

void BufferedLogSink::WaitTillSent() {
  auto& self = instance();
  std::future<void> first;

  {
    WriteLock lock(kBufferedLogSinkSenders);
    if (self.senders.empty()) {
      return;
    }
    first = std::move(self.senders.back());
    self.senders.pop();
  }

  if (!isPlatform(PlatformType::TYPE_WINDOWS)) {
    first.wait();
  } else {
    // Windows is locking by scheduling an async on the main thread.
    first.wait_for(std::chrono::microseconds(100));
  }
}

Status LoggerPlugin::call(const PluginRequest& request,
                          PluginResponse& response) {
  if (FLAGS_logger_secondary_status_only &&
      !BufferedLogSink::isPrimaryLogger(getName()) &&
      (request.count("string") || request.count("snapshot"))) {
    return Status(0, "Logging disabled to secondary plugins");
  }

  QueryLogItem item;
  std::vector<StatusLogLine> intermediate_logs;
  if (request.count("string") > 0) {
    return this->logString(request.at("string"));
  } else if (request.count("snapshot") > 0) {
    return this->logSnapshot(request.at("snapshot"));
  } else if (request.count("init") > 0) {
    deserializeIntermediateLog(request, intermediate_logs);
    this->setProcessName(request.at("init"));
    this->init(this->name(), intermediate_logs);
    return Status(0);
  } else if (request.count("status") > 0) {
    deserializeIntermediateLog(request, intermediate_logs);
    return this->logStatus(intermediate_logs);
  } else if (request.count("event") > 0) {
    return this->logEvent(request.at("event"));
  } else if (request.count("action") && request.at("action") == "features") {
    size_t features = 0;
    features |= (usesLogStatus()) ? LOGGER_FEATURE_LOGSTATUS : 0;
    features |= (usesLogEvent()) ? LOGGER_FEATURE_LOGEVENT : 0;
    return Status(static_cast<int>(features));
  } else {
    return Status(1, "Unsupported call to logger plugin");
  }
}

Status logString(const std::string& message, const std::string& category) {
  return logString(
      message, category, RegistryFactory::get().getActive("logger"));
}

Status logString(const std::string& message,
                 const std::string& category,
                 const std::string& receiver) {
  if (FLAGS_disable_logging) {
    return Status(0, "Logging disabled");
  }

  return Registry::call(
      "logger", receiver, {{"string", message}, {"category", category}});
}

Status logQueryLogItem(const QueryLogItem& results) {
  return logQueryLogItem(results, RegistryFactory::get().getActive("logger"));
}

Status logQueryLogItem(const QueryLogItem& results,
                       const std::string& receiver) {
  if (FLAGS_disable_logging) {
    return Status(0, "Logging disabled");
  }

  std::vector<std::string> json_items;
  Status status;
  if (FLAGS_log_result_events) {
    status = serializeQueryLogItemAsEventsJSON(results, json_items);
  } else {
    std::string json;
    status = serializeQueryLogItemJSON(results, json);
    json_items.push_back(json);
  }
  if (!status.ok()) {
    return status;
  }

  for (auto& json : json_items) {
    if (!json.empty() && json.back() == '\n') {
      json.pop_back();
      status = logString(json, "event", receiver);
    }
  }
  return status;
}

Status logSnapshotQuery(const QueryLogItem& item) {
  if (FLAGS_disable_logging) {
    return Status(0, "Logging disabled");
  }

  std::string json;
  if (!serializeQueryLogItemJSON(item, json)) {
    return Status(1, "Could not serialize snapshot");
  }
  if (!json.empty() && json.back() == '\n') {
    json.pop_back();
  }
  return Registry::call("logger", {{"snapshot", json}});
}

size_t queuedStatuses() {
  ReadLock lock(kBufferedLogSinkLogs);
  return BufferedLogSink::dump().size();
}

size_t queuedSenders() {
  ReadLock lock(kBufferedLogSinkSenders);
  return BufferedLogSink::instance().senders.size();
}

void relayStatusLogs(bool async) {
  if (FLAGS_disable_logging) {
    return;
  }

  {
    ReadLock lock(kBufferedLogSinkLogs);
    if (BufferedLogSink::dump().size() == 0) {
      return;
    }
  }

  auto sender = ([]() {
    // Construct a status log plugin request.
    PluginRequest request = {{"status", "true"}};

    {
      WriteLock lock(kBufferedLogSinkLogs);
      auto& status_logs = BufferedLogSink::dump();
      serializeIntermediateLog(status_logs, request);
      if (!request["log"].empty()) {
        request["log"].pop_back();
      }

      // Flush the buffered status logs.
      status_logs.clear();
    }

    auto logger_plugin = RegistryFactory::get().getActive("logger");
    for (const auto& logger : osquery::split(logger_plugin, ",")) {
      auto& enabled = BufferedLogSink::enabledPlugins();
      if (std::find(enabled.begin(), enabled.end(), logger) != enabled.end()) {
        // Skip the registry's logic, and send directly to the core's logger.
        PluginResponse response;
        Registry::call("logger", request, response);
      }
    }
  });

  if (async) {
    sender();
  } else {
    std::packaged_task<void()> task(std::move(sender));
    auto result = task.get_future();
    std::thread(std::move(task)).detach();

    // Lock accesses to the sender queue.
    WriteLock lock(kBufferedLogSinkSenders);
    BufferedLogSink::instance().senders.push(std::move(result));
  }
}

void systemLog(const std::string& line) {
#ifndef WIN32
  syslog(LOG_NOTICE, "%s", line.c_str());
#endif
}
}
