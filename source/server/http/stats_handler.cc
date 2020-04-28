#include "server/http/stats_handler.h"

#include "common/common/empty_string.h"
#include "common/html/utility.h"
#include "common/http/headers.h"
#include "common/http/utility.h"

#include "server/http/prometheus_stats.h"
#include "server/http/utils.h"

namespace Envoy {
namespace Server {

const uint64_t RecentLookupsCapacity = 100;

Http::Code StatsHandler::handlerResetCounters(absl::string_view, Http::ResponseHeaderMap&,
                                              Buffer::Instance& response, AdminStream&,
                                              Server::Instance& server) {
  for (const Stats::CounterSharedPtr& counter : server.stats().counters()) {
    counter->reset();
  }
  server.stats().symbolTable().clearRecentLookups();
  response.add("OK\n");
  return Http::Code::OK;
}

Http::Code StatsHandler::handlerStatsRecentLookups(absl::string_view, Http::ResponseHeaderMap&,
                                                   Buffer::Instance& response, AdminStream&,
                                                   Server::Instance& server) {
  Stats::SymbolTable& symbol_table = server.stats().symbolTable();
  std::string table;
  const uint64_t total =
      symbol_table.getRecentLookups([&table](absl::string_view name, uint64_t count) {
        table += fmt::format("{:8d} {}\n", count, name);
      });
  if (table.empty() && symbol_table.recentLookupCapacity() == 0) {
    table = "Lookup tracking is not enabled. Use /stats/recentlookups/enable to enable.\n";
  } else {
    response.add("   Count Lookup\n");
  }
  response.add(absl::StrCat(table, "\ntotal: ", total, "\n"));
  return Http::Code::OK;
}

Http::Code StatsHandler::handlerStatsRecentLookupsClear(absl::string_view, Http::ResponseHeaderMap&,
                                                        Buffer::Instance& response, AdminStream&,
                                                        Server::Instance& server) {
  server.stats().symbolTable().clearRecentLookups();
  response.add("OK\n");
  return Http::Code::OK;
}

Http::Code StatsHandler::handlerStatsRecentLookupsDisable(absl::string_view,
                                                          Http::ResponseHeaderMap&,
                                                          Buffer::Instance& response, AdminStream&,
                                                          Server::Instance& server) {
  server.stats().symbolTable().setRecentLookupCapacity(0);
  response.add("OK\n");
  return Http::Code::OK;
}

Http::Code StatsHandler::handlerStatsRecentLookupsEnable(absl::string_view,
                                                         Http::ResponseHeaderMap&,
                                                         Buffer::Instance& response, AdminStream&,
                                                         Server::Instance& server) {
  server.stats().symbolTable().setRecentLookupCapacity(RecentLookupsCapacity);
  response.add("OK\n");
  return Http::Code::OK;
}

Http::Code StatsHandler::handlerStats(absl::string_view url,
                                      Http::ResponseHeaderMap& response_headers,
                                      Buffer::Instance& response, AdminStream& admin_stream,
                                      Server::Instance& server) {
  Http::Code rc = Http::Code::OK;
  const Http::Utility::QueryParams params = Http::Utility::parseQueryString(url);

  const bool used_only = params.find("usedonly") != params.end();
  absl::optional<std::regex> regex;
  if (!Utility::filterParam(params, response, regex)) {
    return Http::Code::BadRequest;
  }

  std::map<std::string, uint64_t> all_stats;
  for (const Stats::CounterSharedPtr& counter : server.stats().counters()) {
    if (shouldShowMetric(*counter, used_only, regex)) {
      all_stats.emplace(counter->name(), counter->value());
    }
  }

  for (const Stats::GaugeSharedPtr& gauge : server.stats().gauges()) {
    if (shouldShowMetric(*gauge, used_only, regex)) {
      ASSERT(gauge->importMode() != Stats::Gauge::ImportMode::Uninitialized);
      all_stats.emplace(gauge->name(), gauge->value());
    }
  }

  std::map<std::string, std::string> text_readouts;
  for (const auto& text_readout : server.stats().textReadouts()) {
    if (shouldShowMetric(*text_readout, used_only, regex)) {
      text_readouts.emplace(text_readout->name(), text_readout->value());
    }
  }

  if (const auto format_value = Utility::formatParam(params)) {
    if (format_value.value() == "json") {
      response_headers.setReferenceContentType(Http::Headers::get().ContentTypeValues.Json);
      response.add(
          statsAsJson(all_stats, text_readouts, server.stats().histograms(), used_only, regex));
    } else if (format_value.value() == "prometheus") {
      return handlerPrometheusStats(url, response_headers, response, admin_stream, server);
    } else {
      response.add("usage: /stats?format=json  or /stats?format=prometheus \n");
      response.add("\n");
      rc = Http::Code::NotFound;
    }
  } else { // Display plain stats if format query param is not there.
    for (const auto& text_readout : text_readouts) {
      response.add(fmt::format("{}: \"{}\"\n", text_readout.first,
                               Html::Utility::sanitize(text_readout.second)));
    }
    for (const auto& stat : all_stats) {
      response.add(fmt::format("{}: {}\n", stat.first, stat.second));
    }
    // TODO(ramaraochavali): See the comment in ThreadLocalStoreImpl::histograms() for why we use a
    // multimap here. This makes sure that duplicate histograms get output. When shared storage is
    // implemented this can be switched back to a normal map.
    std::multimap<std::string, std::string> all_histograms;
    for (const Stats::ParentHistogramSharedPtr& histogram : server.stats().histograms()) {
      if (shouldShowMetric(*histogram, used_only, regex)) {
        all_histograms.emplace(histogram->name(), histogram->quantileSummary());
      }
    }
    for (const auto& histogram : all_histograms) {
      response.add(fmt::format("{}: {}\n", histogram.first, histogram.second));
    }
  }
  return rc;
}

Http::Code StatsHandler::handlerPrometheusStats(absl::string_view path_and_query,
                                                Http::ResponseHeaderMap&,
                                                Buffer::Instance& response, AdminStream&,
                                                Server::Instance& server) {
  const Http::Utility::QueryParams params = Http::Utility::parseQueryString(path_and_query);
  const bool used_only = params.find("usedonly") != params.end();
  absl::optional<std::regex> regex;
  if (!Utility::filterParam(params, response, regex)) {
    return Http::Code::BadRequest;
  }
  PrometheusStatsFormatter::statsAsPrometheus(server.stats().counters(), server.stats().gauges(),
                                              server.stats().histograms(), response, used_only,
                                              regex);
  return Http::Code::OK;
}

std::string
StatsHandler::statsAsJson(const std::map<std::string, uint64_t>& all_stats,
                          const std::map<std::string, std::string>& text_readouts,
                          const std::vector<Stats::ParentHistogramSharedPtr>& all_histograms,
                          const bool used_only, const absl::optional<std::regex> regex,
                          const bool pretty_print) {

  ProtobufWkt::Struct document;
  std::vector<ProtobufWkt::Value> stats_array;
  for (const auto& text_readout : text_readouts) {
    ProtobufWkt::Struct stat_obj;
    auto* stat_obj_fields = stat_obj.mutable_fields();
    (*stat_obj_fields)["name"] = ValueUtil::stringValue(text_readout.first);
    (*stat_obj_fields)["value"] = ValueUtil::stringValue(text_readout.second);
    stats_array.push_back(ValueUtil::structValue(stat_obj));
  }
  for (const auto& stat : all_stats) {
    ProtobufWkt::Struct stat_obj;
    auto* stat_obj_fields = stat_obj.mutable_fields();
    (*stat_obj_fields)["name"] = ValueUtil::stringValue(stat.first);
    (*stat_obj_fields)["value"] = ValueUtil::numberValue(stat.second);
    stats_array.push_back(ValueUtil::structValue(stat_obj));
  }

  ProtobufWkt::Struct histograms_obj;
  auto* histograms_obj_fields = histograms_obj.mutable_fields();

  ProtobufWkt::Struct histograms_obj_container;
  auto* histograms_obj_container_fields = histograms_obj_container.mutable_fields();
  std::vector<ProtobufWkt::Value> computed_quantile_array;

  bool found_used_histogram = false;
  for (const Stats::ParentHistogramSharedPtr& histogram : all_histograms) {
    if (shouldShowMetric(*histogram, used_only, regex)) {
      if (!found_used_histogram) {
        // It is not possible for the supported quantiles to differ across histograms, so it is ok
        // to send them once.
        Stats::HistogramStatisticsImpl empty_statistics;
        std::vector<ProtobufWkt::Value> supported_quantile_array;
        for (double quantile : empty_statistics.supportedQuantiles()) {
          supported_quantile_array.push_back(ValueUtil::numberValue(quantile * 100));
        }
        (*histograms_obj_fields)["supported_quantiles"] =
            ValueUtil::listValue(supported_quantile_array);
        found_used_histogram = true;
      }

      ProtobufWkt::Struct computed_quantile;
      auto* computed_quantile_fields = computed_quantile.mutable_fields();
      (*computed_quantile_fields)["name"] = ValueUtil::stringValue(histogram->name());

      std::vector<ProtobufWkt::Value> computed_quantile_value_array;
      for (size_t i = 0; i < histogram->intervalStatistics().supportedQuantiles().size(); ++i) {
        ProtobufWkt::Struct computed_quantile_value;
        auto* computed_quantile_value_fields = computed_quantile_value.mutable_fields();
        const auto& interval = histogram->intervalStatistics().computedQuantiles()[i];
        const auto& cumulative = histogram->cumulativeStatistics().computedQuantiles()[i];
        (*computed_quantile_value_fields)["interval"] =
            std::isnan(interval) ? ValueUtil::nullValue() : ValueUtil::numberValue(interval);
        (*computed_quantile_value_fields)["cumulative"] =
            std::isnan(cumulative) ? ValueUtil::nullValue() : ValueUtil::numberValue(cumulative);

        computed_quantile_value_array.push_back(ValueUtil::structValue(computed_quantile_value));
      }
      (*computed_quantile_fields)["values"] = ValueUtil::listValue(computed_quantile_value_array);
      computed_quantile_array.push_back(ValueUtil::structValue(computed_quantile));
    }
  }

  if (found_used_histogram) {
    (*histograms_obj_fields)["computed_quantiles"] = ValueUtil::listValue(computed_quantile_array);
    (*histograms_obj_container_fields)["histograms"] = ValueUtil::structValue(histograms_obj);
    stats_array.push_back(ValueUtil::structValue(histograms_obj_container));
  }

  auto* document_fields = document.mutable_fields();
  (*document_fields)["stats"] = ValueUtil::listValue(stats_array);

  return MessageUtil::getJsonStringFromMessage(document, pretty_print, true);
}

} // namespace Server
} // namespace Envoy