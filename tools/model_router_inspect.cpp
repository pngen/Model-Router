// Model Router - command line inspection tool.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0
//
// The tool reads only: it loads durable state through the public persistence
// API and renders what the router knows. It never mutates durable state, never
// prompts for input, and never waits for anything.

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "model_router/model_router.hpp"

namespace {

constexpr int kExitSuccess = 0;
constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;

constexpr const char* kStateFileLabel = "(none)";

struct Options {
  bool has_state{false};
  std::string state_path;
  bool validate{false};
  bool summary{false};
  bool models{false};
  bool backends{false};
  bool routes{false};
  bool fenced{false};
  bool json{false};
  bool explain{false};
  std::uint64_t explain_id{0};
  bool version{false};
  bool help{false};

  [[nodiscard]] bool any_section() const {
    return validate || summary || models || backends || routes || fenced || explain;
  }
};

void print_usage(std::ostream& out) {
  out << "Model Router inspection tool " << model_router::version_string() << "\n";
  out << "\n";
  out << "usage: model_router_inspect [options]\n";
  out << "\n";
  out << "options:\n";
  out << "  --state <file>        load a persisted router state file\n";
  out << "  --validate            validate the state file integrity and print the result\n";
  out << "  --summary             print the router summary\n";
  out << "  --models              print registered models\n";
  out << "  --backends            print registered backends\n";
  out << "  --routes              print retained route decisions\n";
  out << "  --fenced              print fenced backend incarnations\n";
  out << "  --explain-route <id>  print the canonical explanation of one retained decision\n";
  out << "  --json                emit JSON instead of text where a JSON form exists\n";
  out << "  --version             print the product version string\n";
  out << "  --help                print this usage\n";
  out << "\n";
  out << "Without --state the tool inspects a fresh, unloaded router.\n";
  out << "Exit codes: " << kExitSuccess << " success, " << kExitFailure
      << " validation or load failure, " << kExitUsage << " usage error.\n";
}

[[nodiscard]] const char* bool_text(bool value) { return value ? "true" : "false"; }

[[nodiscard]] std::string escape_json(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (const char raw : text) {
    const auto ch = static_cast<unsigned char>(raw);
    switch (ch) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (ch < 0x20u) {
          static constexpr char kHex[] = "0123456789abcdef";
          out += "\\u00";
          out.push_back(kHex[ch >> 4]);
          out.push_back(kHex[ch & 0x0Fu]);
        } else {
          out.push_back(raw);
        }
        break;
    }
  }
  return out;
}

/// Strict decimal parse: no signs, no whitespace, no exceptions.
[[nodiscard]] bool parse_u64(std::string_view text, std::uint64_t* out) {
  if (text.empty()) {
    return false;
  }
  std::uint64_t value = 0;
  for (const char ch : text) {
    if (ch < '0' || ch > '9') {
      return false;
    }
    value = (value * 10u) + static_cast<std::uint64_t>(ch - '0');
  }
  *out = value;
  return true;
}

void split_option(const std::string& argument, std::string* name, std::string* value,
                  bool* has_value) {
  const std::size_t position = argument.find('=');
  if (position == std::string::npos) {
    *name = argument;
    value->clear();
    *has_value = false;
    return;
  }
  *name = argument.substr(0, position);
  *value = argument.substr(position + 1);
  *has_value = true;
}

void print_load_report(const Options& options, const model_router::PersistenceLoad& loaded) {
  if (options.json) {
    std::cout << "{\"state\":{\"path\":\"" << escape_json(options.state_path) << "\",\"code\":\""
              << model_router::to_string(loaded.code) << "\",\"format_version\":"
              << loaded.format_version << ",\"records\":" << loaded.record_count << ",\"detail\":\""
              << escape_json(loaded.detail) << "\"}}\n";
    return;
  }
  std::cout << "state path=" << options.state_path
            << " code=" << model_router::to_string(loaded.code)
            << " format=" << loaded.format_version << " records=" << loaded.record_count
            << " detail=" << loaded.detail << "\n";
}

void print_summary(const Options& options, const model_router::ModelRouter& router) {
  if (options.json) {
    std::cout << router.summary().to_json() << "\n";
    return;
  }
  std::cout << router.summary().to_text();
}

void print_models(const Options& options, const model_router::ModelRouter& router,
                  const model_router::PersistenceLoad& loaded, bool loaded_ok) {
  std::vector<model_router::ModelDescriptor> models;
  if (loaded_ok) {
    models.reserve(loaded.state.models.size());
    for (const model_router::PersistedModelRecord& record : loaded.state.models) {
      models.push_back(record.model);
    }
  } else {
    // A fresh router exposes no model enumeration, so the model set is derived
    // from the bindings of every registered backend.
    std::set<model_router::ModelId> model_ids;
    const model_router::RouterSnapshot snapshot = router.snapshot();
    for (const model_router::BackendSummary& backend : snapshot.backends) {
      model_router::BackendDescriptor descriptor;
      if (!router.find_backend(backend.backend_id, &descriptor)) {
        continue;
      }
      for (const model_router::ModelBinding& binding : descriptor.model_bindings) {
        model_ids.insert(binding.model_id);
      }
    }
    for (const model_router::ModelId model_id : model_ids) {
      model_router::ModelDescriptor descriptor;
      if (router.find_model(model_id, &descriptor)) {
        models.push_back(std::move(descriptor));
      }
    }
  }

  if (options.json) {
    std::cout << "{\"models\":[";
    bool first = true;
    for (const model_router::ModelDescriptor& model : models) {
      if (!first) {
        std::cout << ",";
      }
      first = false;
      std::cout << "{\"model_id\":" << model.model_id.value() << ",\"model_generation\":"
                << model.model_generation.value() << ",\"artifact_generation\":"
                << model.artifact_generation.value() << ",\"family_id\":"
                << model.family_id.value() << ",\"context_limit_tokens\":"
                << model.context_limit_tokens << ",\"max_output_tokens\":"
                << model.max_output_tokens << ",\"quality_class\":" << model.quality_class
                << ",\"lifecycle\":\"" << model_router::to_string(model.lifecycle)
                << "\",\"provenance\":\"" << model_router::to_string(model.provenance)
                << "\",\"capabilities\":" << model.capabilities.size() << ",\"display_name\":\""
                << escape_json(model.display_name) << "\"}";
    }
    std::cout << "]}\n";
    return;
  }

  std::cout << "models count=" << models.size() << "\n";
  for (const model_router::ModelDescriptor& model : models) {
    std::cout << "  model id=" << model.model_id.value()
              << " generation=" << model.model_generation.value()
              << " artifact=" << model.artifact_generation.value()
              << " family=" << model.family_id.value()
              << " context=" << model.context_limit_tokens
              << " max_output=" << model.max_output_tokens
              << " quality=" << model.quality_class
              << " lifecycle=" << model_router::to_string(model.lifecycle)
              << " provenance=" << model_router::to_string(model.provenance)
              << " capabilities=" << model.capabilities.size()
              << " name=" << (model.display_name.empty() ? kStateFileLabel
                                                         : model.display_name.c_str())
              << "\n";
  }
}

void print_backends(const Options& options, const model_router::RouterSnapshot& snapshot) {
  if (options.json) {
    std::cout << "{\"backends\":[";
    bool first = true;
    for (const model_router::BackendSummary& backend : snapshot.backends) {
      if (!first) {
        std::cout << ",";
      }
      first = false;
      std::cout << "{\"backend_id\":" << backend.backend_id.value()
                << ",\"backend_generation\":" << backend.backend_generation.value()
                << ",\"backend_boot\":" << backend.backend_boot.value() << ",\"provider_id\":"
                << backend.provider_id.value() << ",\"endpoint_id\":" << backend.endpoint_id.value()
                << ",\"endpoint_generation\":" << backend.endpoint_generation.value()
                << ",\"trust_domain\":\"" << model_router::to_string(backend.trust_domain)
                << "\",\"locality\":\"" << escape_json(backend.locality.value())
                << "\",\"health\":\"" << model_router::to_string(backend.health)
                << "\",\"availability\":\"" << model_router::to_string(backend.availability)
                << "\",\"readiness\":\"" << model_router::to_string(backend.readiness)
                << "\",\"residency\":\"" << model_router::to_string(backend.residency)
                << "\",\"capacity\":\"" << model_router::to_string(backend.capacity)
                << "\",\"bound_models\":" << backend.bound_model_count << ",\"fenced\":"
                << bool_text(backend.fenced) << ",\"evidence_current\":"
                << bool_text(backend.evidence_current) << ",\"provenance\":\""
                << model_router::to_string(backend.provenance) << "\"}";
    }
    std::cout << "]}\n";
    return;
  }

  std::cout << "backends count=" << snapshot.backends.size() << "\n";
  for (const model_router::BackendSummary& backend : snapshot.backends) {
    std::cout << "  backend id=" << backend.backend_id.value()
              << " generation=" << backend.backend_generation.value()
              << " boot=" << backend.backend_boot.value()
              << " provider=" << backend.provider_id.value()
              << " endpoint=" << backend.endpoint_id.value() << "@"
              << backend.endpoint_generation.value()
              << " trust=" << model_router::to_string(backend.trust_domain)
              << " locality=" << (backend.locality.empty() ? kStateFileLabel
                                                           : backend.locality.value().c_str())
              << " health=" << model_router::to_string(backend.health)
              << " availability=" << model_router::to_string(backend.availability)
              << " readiness=" << model_router::to_string(backend.readiness)
              << " residency=" << model_router::to_string(backend.residency)
              << " capacity=" << model_router::to_string(backend.capacity)
              << " models=" << backend.bound_model_count << " fenced="
              << (backend.fenced ? 1 : 0) << " evidence_current="
              << (backend.evidence_current ? 1 : 0) << " provenance="
              << model_router::to_string(backend.provenance) << "\n";
  }
}

void print_routes(const Options& options, const model_router::ModelRouter& router) {
  const std::vector<model_router::RouteDecision> history = router.route_history();
  if (options.json) {
    std::cout << "{\"routes\":[";
    bool first = true;
    for (const model_router::RouteDecision& decision : history) {
      if (!first) {
        std::cout << ",";
      }
      first = false;
      std::cout << "{\"decision_id\":" << decision.decision_id.value()
                << ",\"decision_generation\":" << decision.decision_generation.value()
                << ",\"request_id\":" << decision.request_id.value()
                << ",\"request_generation\":" << decision.request_generation.value()
                << ",\"status\":\"" << model_router::to_string(decision.status) << "\",\"code\":\""
                << model_router::to_string(decision.code) << "\",\"currentness\":\""
                << model_router::to_string(decision.explanation.currentness)
                << "\",\"fallbacks\":" << decision.fallbacks.size() << ",\"created_at\":"
                << decision.created_at_unix_millis << ",\"winner\":\""
                << escape_json(decision.explanation.winner.to_string())
                << "\",\"semantic_digest\":\"" << decision.explanation.semantic_digest
                << "\",\"explanation\":" << decision.explanation.to_json() << "}";
    }
    std::cout << "]}\n";
    return;
  }

  std::cout << "routes count=" << history.size() << "\n";
  for (const model_router::RouteDecision& decision : history) {
    std::cout << "  decision id=" << decision.decision_id.value()
              << " generation=" << decision.decision_generation.value()
              << " request=" << decision.request_id.value()
              << " request_generation=" << decision.request_generation.value()
              << " status=" << model_router::to_string(decision.status)
              << " code=" << model_router::to_string(decision.code)
              << " currentness=" << model_router::to_string(decision.explanation.currentness)
              << " fallbacks=" << decision.fallbacks.size()
              << " created_at=" << decision.created_at_unix_millis
              << " winner=" << decision.explanation.winner.to_string()
              << " digest=" << decision.explanation.semantic_digest << "\n";
  }
}

void print_fenced(const Options& options, const model_router::ModelRouter& router) {
  const std::vector<model_router::FencedBootRecord> fenced = router.fenced_boots();
  if (options.json) {
    std::cout << "{\"fenced\":[";
    bool first = true;
    for (const model_router::FencedBootRecord& record : fenced) {
      if (!first) {
        std::cout << ",";
      }
      first = false;
      std::cout << "{\"backend_id\":" << record.backend_id.value() << ",\"backend_generation\":"
                << record.backend_generation.value() << ",\"backend_boot\":"
                << record.backend_boot.value() << ",\"reason\":\""
                << model_router::to_string(record.reason) << "\",\"fenced_at\":"
                << record.fenced_at_unix_millis << "}";
    }
    std::cout << "]}\n";
    return;
  }

  std::cout << "fenced count=" << fenced.size() << "\n";
  for (const model_router::FencedBootRecord& record : fenced) {
    std::cout << "  fenced backend=" << record.backend_id.value()
              << " generation=" << record.backend_generation.value()
              << " boot=" << record.backend_boot.value()
              << " reason=" << model_router::to_string(record.reason)
              << " at=" << record.fenced_at_unix_millis << "\n";
  }
}

/// Returns false when the decision is not retained.
bool print_explain(const Options& options, const model_router::ModelRouter& router) {
  const model_router::RouteDecisionId decision_id(options.explain_id);
  model_router::RouteDecision decision;
  if (!router.find_decision(decision_id, &decision)) {
    std::cerr << "error: decision " << options.explain_id << " is not retained\n";
    return false;
  }
  if (options.json) {
    std::cout << "{\"explain\":{\"decision_id\":" << decision.decision_id.value()
              << ",\"decision_generation\":" << decision.decision_generation.value()
              << ",\"status\":\"" << model_router::to_string(decision.status) << "\",\"code\":\""
              << model_router::to_string(decision.code) << "\",\"semantic_digest\":\""
              << decision.explanation.semantic_digest
              << "\",\"explanation\":" << decision.explanation.to_json() << "}}\n";
    return true;
  }
  std::cout << "explain decision=" << options.explain_id << "\n";
  std::cout << router.explain_route(decision_id) << "\n";
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = (argv[index] != nullptr) ? std::string(argv[index]) : std::string();
    std::string name;
    std::string value;
    bool has_value = false;
    split_option(argument, &name, &value, &has_value);

    if (name == "--help") {
      options.help = true;
    } else if (name == "--version") {
      options.version = true;
    } else if (name == "--json") {
      options.json = true;
    } else if (name == "--validate") {
      options.validate = true;
    } else if (name == "--summary") {
      options.summary = true;
    } else if (name == "--models") {
      options.models = true;
    } else if (name == "--backends") {
      options.backends = true;
    } else if (name == "--routes") {
      options.routes = true;
    } else if (name == "--fenced") {
      options.fenced = true;
    } else if (name == "--state") {
      if (!has_value) {
        if (index + 1 >= argc || argv[index + 1] == nullptr) {
          std::cerr << "error: --state requires a file path\n";
          return kExitUsage;
        }
        ++index;
        value = argv[index];
      }
      if (value.empty()) {
        std::cerr << "error: --state requires a non-empty file path\n";
        return kExitUsage;
      }
      options.has_state = true;
      options.state_path = value;
    } else if (name == "--explain-route") {
      if (!has_value) {
        if (index + 1 >= argc || argv[index + 1] == nullptr) {
          std::cerr << "error: --explain-route requires a decision identity\n";
          return kExitUsage;
        }
        ++index;
        value = argv[index];
      }
      if (!parse_u64(value, &options.explain_id)) {
        std::cerr << "error: --explain-route requires a decimal decision identity\n";
        return kExitUsage;
      }
      options.explain = true;
    } else {
      std::cerr << "error: unknown option " << argument << "\n";
      print_usage(std::cerr);
      return kExitUsage;
    }
  }

  if (options.help) {
    print_usage(std::cout);
    return kExitSuccess;
  }
  if (options.version) {
    std::cout << model_router::product_string() << "\n";
    return kExitSuccess;
  }
  if (!options.has_state && !options.any_section()) {
    std::cerr << "error: no inspection option was given\n";
    print_usage(std::cerr);
    return kExitUsage;
  }

  const model_router::ResourceLimits limits = model_router::default_resource_limits();
  model_router::ModelRouterOptions router_options;
  router_options.clock = std::make_shared<model_router::SystemClock>();
  model_router::ModelRouter router(std::move(router_options));

  model_router::PersistenceLoad loaded;
  bool loaded_ok = false;

  if (options.has_state) {
    loaded = model_router::RouterStateStore::load(options.state_path, limits);
    loaded_ok = loaded.ok();
    print_load_report(options, loaded);
    if (!loaded_ok) {
      return kExitFailure;
    }
    if (options.any_section()) {
      const model_router::PersistenceResult imported = router.load(options.state_path);
      if (!imported.ok()) {
        std::cerr << "error: durable state could not be imported: "
                  << model_router::to_string(imported.code) << " " << imported.detail << "\n";
        return kExitFailure;
      }
    }
  }

  int exit_code = kExitSuccess;

  if (options.validate) {
    const model_router::PersistenceResult file_result =
        options.has_state
            ? model_router::RouterStateStore::validate_file(options.state_path, limits)
            : model_router::PersistenceResult{model_router::OutcomeCode::ACCEPTED, "no state file"};
    const model_router::InvariantReport invariants = router.check_invariants();
    if (options.json) {
      std::cout << "{\"validate\":{\"path\":\""
                << escape_json(options.has_state ? options.state_path : std::string())
                << "\",\"file_code\":\"" << model_router::to_string(file_result.code)
                << "\",\"file_detail\":\"" << escape_json(file_result.detail)
                << "\",\"invariants\":" << invariants.to_json() << "}}\n";
    } else {
      std::cout << "validate file=" << (options.has_state ? options.state_path.c_str()
                                                         : kStateFileLabel)
                << " code=" << model_router::to_string(file_result.code)
                << " detail=" << file_result.detail << "\n";
      std::cout << invariants.to_text();
    }
    if (!file_result.ok() || !invariants.ok()) {
      exit_code = kExitFailure;
    }
  }

  if (options.summary) {
    print_summary(options, router);
  }
  if (options.models) {
    print_models(options, router, loaded, loaded_ok);
  }
  if (options.backends) {
    print_backends(options, router.snapshot());
  }
  if (options.routes) {
    print_routes(options, router);
  }
  if (options.fenced) {
    print_fenced(options, router);
  }
  if (options.explain && !print_explain(options, router)) {
    exit_code = kExitFailure;
  }

  return exit_code;
}
