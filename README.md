# Model Router

Model Router is a policy-, capability-, cost-, availability-, latency-, locality-,
compatibility-, health-, and authority-aware routing runtime across heterogeneous AI models
and inference backends. It answers one systems question:

> Which model/backend is the deterministic legal target for this request now, under the current
> request requirements, model capabilities, provider/backend health, policy, cost, budget,
> latency/SLO, locality, compatibility, availability, residency/readiness, and generation-bound
> authority — and when must that route be rejected, deferred, invalidated, rerouted, or
> superseded?

Model Router owns the **route decision** and the **route authority**. It does not own model
execution.

- Version: 1.0.0
- Language: C++20
- Namespace: `model_router`
- License: Apache License 2.0
- Installed target: `model_router::model_router`

## Core doctrine

Hard validity before ranking. `UNKNOWN` is first-class and must never silently become
`ELIGIBLE`, `AVAILABLE`, `SUPPORTED`, `COMPATIBLE`, `HEALTHY`, `READY`, `CHEAP`, `FAST`,
`LOCAL`, `TRUSTED`, `POLICY_ALLOWED`, `BUDGET_ALLOWED`, `SLO_SAFE`, `CURRENT`, or
`AUTHORITATIVE`.

- A favorable score never rescues a hard-invalid candidate.
- A model/backend that once worked is not automatically current.
- A backend process that remains alive is not automatically healthy.
- A cheap model is not automatically capable; a capable model is not automatically policy-allowed.
- A route that was valid when constructed is not automatically valid at dispatch time.
- A response arriving from a backend does not prove the route was still authoritative when executed.

The pipeline is:

```
request admission
  -> request requirement normalization
  -> candidate discovery
  -> hard eligibility filtering
  -> external policy/cost/SLO/availability validation
  -> deterministic factor construction
  -> deterministic candidate ranking
  -> route-plan creation
  -> route authority binding
  -> optional backend reservation/lease binding
  -> pre-dispatch revalidation
  -> dispatch handoff
  -> completion observation
  -> route outcome recording
  -> retry/reroute/fallback decision where authorized
```

## Systems boundary

Model Router is **not** an inference server, inference scheduler, agent scheduler, agent
runtime, ensemble orchestrator, critic, semantic-blind load balancer, model registry, model
lifecycle manager, cost accounting engine, benchmark harness, or generic workflow engine.

| Adjacent system | What it owns | What Model Router does |
| --- | --- | --- |
| Agent Runtime | Execution lifecycle of a long-running autonomous agent: actions, attempts, checkpoints, tools, side effects, agent retries, cancellation, durable progress | Answers "route this model action under these requirements and this current authority" with a deterministic route decision or an explicit rejection/defer outcome. It never reproduces agent action lifecycle, checkpoint semantics, tool lifecycle, side-effect semantics, agent retries, or agent cancellation. |
| Agent Scheduler | Which persistent autonomous agent owns work: agent ranking, worker assignment, fairness, placement | Chooses which model/backend satisfies an admitted model request. It never ranks agents or assigns work. |
| Ensemble Fabric | Coordinating multiple models/agents as one logical runtime: parallel attempts, specialists, judges, voting, consensus, arbitration, aggregation | Chooses exactly one authoritative route, or an explicitly ordered fallback plan when policy allows. A fallback chain is an ordered set of alternative **singular** routes; it is not an ensemble and implies no simultaneous execution. A ranked candidate list is not an ensemble. |
| Model Registry / Model Lifecycle Fabric | Promotion, rollout, canarying, rollback, retirement, fleet-wide model version authority | Consumes current eligible model generations through narrow adapters. It never invents lifecycle truth. |
| Cost Governor | Execution economics and budget feasibility: accounting model, price authority, budget verdicts | Consumes price/budget evidence as a routing factor or hard constraint. `UNKNOWN` cost is never treated as free. It is not a billing engine. |
| SLO Fabric | Service-level contracts and evidence | Consumes latency targets, deadlines, tail-latency constraints, availability classes, quality floors and throughput classes. It does not implement a second SLO control plane. |
| Model Residency / Warmth / Capacity / Reservation / Resource Broker / Inference Scheduler | Their resource state | Considers their evidence (residency, readiness, capacity headroom, queue pressure, reservation feasibility) but never owns it. |

Model Router is standalone: it builds and functions without any other repository.

## Identity model

Authority semantics are carried by strong types, never by raw strings. `EntityId<Tag>` and
`Generation<Tag>` produce distinct, non-interchangeable types per identity kind.

| Kind | Types |
| --- | --- |
| Router | `RouterId`, `RouterGeneration`, `RouterBootId`, `RouterEpoch`, `CoordinatorEpoch` |
| Routing | `RouteRequestId/Generation`, `RouteDecisionId/Generation`, `RoutePlanId/Generation`, `DispatchId/Generation` |
| Model | `ModelId`, `ModelGeneration`, `ModelVersionId`, `ArtifactGeneration`, `ModelFamilyId` |
| Serving | `ProviderId/Generation`, `BackendId/Generation`, `BackendBootId`, `BackendRegistrationGeneration`, `EndpointId/Generation` |
| Evidence | `CapabilityProfileId`, `CapabilityGeneration`, `HealthGeneration`, `AvailabilityGeneration`, `ReadinessGeneration`, `ResidencyGeneration`, `CapacityGeneration`, `ReservationId/Generation` |
| External | `PolicyId/Generation`, `BudgetId/Generation`, `CostEvidenceId`, `PriceGeneration`, `SLOId/Generation`, `CompatibilityProfileId/Generation`, `TrustProfileId/Generation` |
| Tenancy | `TenantId`, `NamespaceId` |

Model identity is deliberately separate from serving identity:

- `ModelId` identifies the model family/artifact; `ModelGeneration` the current generation;
  `ArtifactGeneration` the exact artifact revision.
- `BackendId` identifies the serving runtime; `BackendGeneration` its configuration generation;
  `BackendBootId` the process/service incarnation; `EndpointId/Generation` the concrete
  dispatch destination.
- One model may have many backends; one backend may host multiple models. None of these are
  conflated.

A `CandidateKey` is the canonical tuple
`(ModelId, ModelGeneration, ArtifactGeneration, BackendId, BackendGeneration, BackendBootId,
EndpointId, EndpointGeneration)` with total content-derived ordering and a content-derived
digest.

## Request requirements

`RouteRequirements` is normalized and validated before any candidate is considered. It can
express required capabilities with explicit minimum evidence states, preferred capabilities,
minimum context length, maximum expected output length, required input/output modalities,
structured-output/JSON-schema/tool-calling/streaming/logprobs/deterministic-seed requirements,
quality floor, model/family/provider/backend allowlists and denylists, local-only,
offline-only, remote-allowed, required and denied localities, minimum trust domain,
known-cost requirement and cost ceiling, latency and deadline constraints, mandatory SLO,
availability/health/readiness/residency/capacity/reservation requirements, protocol
compatibility requirements, affinity, anti-affinity, stickiness, fallback policy, and
retry/reroute policy hints.

Self-contradictory combinations are rejected deterministically with a typed outcome, and
`RouteRequirements::digest()` gives a stable content digest of the normalized set.

## Capability model

Capabilities are namespaced, canonical keys such as `text/generation`,
`modality/vision-input`, `tool/calling`, `output/structured`, `task/code`,
`execution/cuda`, or `execution/offline`. Keys are validated: lower-case ASCII, bounded
length, no empty segments, no control characters.

Every claim carries an explicit state, generation, profile identity, model generation,
backend generation and backend incarnation, observation time, expiry, and provenance:

`UNKNOWN` < `DECLARED` < `OBSERVED` < `VERIFIED`, plus the non-satisfying states
`REVOKED`, `STALE`, and `UNSUPPORTED`.

A requirement states the minimum evidence state that satisfies it. `UNKNOWN` never satisfies a
required capability, and evidence from a different or fenced `BackendBootId` never authorizes
the current incarnation.

## Hard eligibility

Hard predicates run before ranking, in a fixed order, and return typed rejection reasons. They
cover: model lifecycle and generation, artifact generation, backend generation and
incarnation, endpoint generation, availability, health, readiness, draining/retirement,
residency, capacity, reservation, required capabilities and their evidence strength, context
limit, output limit, input/output modality, structured output, JSON schema, tool calling,
streaming, logprobs, deterministic seeding, protocol compatibility, quality floor, identity
allow/deny lists, local/offline/remote placement, required and denied localities, trust floor,
policy, budget, cost knownness and ceiling, SLO feasibility, latency ceiling, and request
validity.

`UNKNOWN` evidence fails every predicate that requires affirmative proof.

## Deterministic ranking

Ranking uses named integer factors with no floating-point arithmetic anywhere in the ranking
path. Each factor is normalized into `[0, 1000000]` against an absolute reference scale and
multiplied by an integer weight in parts per 10000; the score is a saturating `int64_t` sum.

Factors: capability fit, preferred capability coverage, quality class, cost total, latency,
tail latency, queue delay, warmth, residency, locality, network distance, provider
availability, backend health, backend readiness, capacity headroom, reservation confidence,
SLO headroom, context headroom, trust preference, policy preference, model affinity, cache
affinity, historical reliability, failure-domain diversity, data movement cost, backend
startup cost, route switch penalty, continuity stickiness, and explicit caller preference.

A factor whose evidence is absent is `UNKNOWN`: it contributes exactly zero and is marked
visible in the explanation. Missing evidence never becomes a favorable score. Hard-invalid
factors never enter the score.

Ties break deterministically: by the canonical factor sequence, then by the canonical candidate
identity. Identical canonical state therefore produces the same eligible set, the same ranked
order, the same winner, the same route-plan identity, and the same explanation, regardless of
insertion order or container layout.

## Route authority and pre-dispatch revalidation

A `RouteDecision` binds the router and coordinator epochs, the request identity and
generation, the decision identity and generation, the model, artifact, provider, backend,
backend incarnation, backend registration generation, endpoint, capability profile, policy,
budget, price, SLO, compatibility, trust, health, availability, readiness, residency, capacity,
reservation, tenancy, and dispatch generations.

Before dispatch, the router revalidates every required component against current authority and
re-resolves the target candidate from current canonical state instead of reusing the stored
verdict. A stale, fenced, policy-denied, budget-denied, retired, or expired route is never
dispatched. Typed outcomes distinguish `REVALIDATION_REQUIRED`, `REROUTE_REQUIRED`, and the
specific `REJECT_STALE_*` reasons.

Every authority-changing mutation marks retained executable decisions whose bound authority no
longer matches current state as `STALE`, so a decision reported as `CURRENT` is dispatchable.

## Fallback and reroute semantics

Fallback behavior is policy-bound. Failures are classified as `TRANSIENT_BACKEND_FAILURE`,
`BACKEND_UNAVAILABLE`, `BACKEND_RESTARTED`, `MODEL_UNAVAILABLE`, `STALE_ROUTE`,
`POLICY_CHANGED`, `BUDGET_CHANGED`, `PRICE_CHANGED`, `SLO_CHANGED`, `CAPACITY_CHANGED`,
`COMPATIBILITY_CHANGED`, `PERMANENT_REJECTION`, `CALLER_CANCELLED`, or `UNKNOWN`.

A fallback may be used only when the request is still current, caller authority is still
current, fallback policy permits that failure class, the fallback candidate passes its own
fresh revalidation, and budget/policy/SLO remain valid. Permanent failures are never rerouted.
If no fallback survives, a fresh full routing pass is the only legal option. A precomputed
fallback list is never treated as timeless authority. The caller remains responsible for
higher-level agent/tool side-effect semantics.

## Policy, cost, SLO, trust, and tenancy integration

Policy is a typed declarative structure, not a scripting engine: allowed/denied models,
providers, backends, families and localities; trust floor; minimum capability evidence state;
quality floor; maximum request cost; maximum latency; local-only and deny-public-remote;
fallback policy with permitted failure classes and maximum depth; maximum provider
concentration; and tenant binding. Policy is generation-bound and may also be supplied by a
narrow `PolicyProvider` adapter.

Cost is evidence, never an accounting engine: input/output unit prices, request minimum,
cache read/write prices, currency and billing-basis identity, price generation, effective
time, knownness, and an estimated total. Numerically incompatible cost units are never compared
without explicit identity. `UNKNOWN` price never becomes zero.

SLO, budget, compatibility, and trust evidence each arrive as generation-bound snapshots with
explicit `UNKNOWN` states. Trust domains are `LOCAL`, `PRIVATE_NETWORK`, `TRUSTED_REMOTE`,
`PUBLIC_REMOTE`, and `UNKNOWN`; trust is never inferred from a hostname, address, or provider
name.

Tenancy is part of the bound authority: `revalidate` and `dispatch` accept an optional caller
tenant/namespace and reject a decision that belongs to a different one.

## Backend reincarnation and fencing

Availability, health, readiness, residency, and capacity are five distinct facts, each
`UNKNOWN` until current evidence proves otherwise. `AVAILABLE` is not `HEALTHY`; `HEALTHY` is
not `READY`; `READY` is not `CURRENT`; `CURRENT` is not `ELIGIBLE`.

Backend incarnation authority is independent per `BackendId`. Restarting one backend never
invalidates another. A fenced `(BackendId, BackendBootId)` is durable history and can never
become authoritative again, in any process, at any epoch; registration, capability publication,
and dynamic evidence from a fenced or replaced incarnation are rejected before any canonical
mutation.

## Persistence and conservative recovery

Durable state holds router identity, generation and epochs, configuration and ranking weights,
model descriptors, backend **registration identity only**, fenced incarnation history with its
real generation and fence time, retained route decisions, and deterministic counters. Dynamic
evidence is deliberately absent.

Format: a fixed 72-byte header (magic `MRSTATE\0`, format version, header size, flags, record
count, payload length, payload CRC-32C, header CRC-32C over the bytes preceding the checksum
field, and a SHA-256 semantic digest) followed by a deterministic record payload. The file must
be exactly `header_bytes + payload_bytes` long, so truncation and trailing garbage are both
rejected; an unsupported format version is rejected before any ambiguous parsing. Saves are
atomic: the image is written to a sibling temporary file, flushed to the device, and moved over
the target. Corruption, truncation, trailing garbage, unsupported versions, and record-count
mismatches are all rejected with typed results.

Recovery is conservative. After a restart the router advances `RouterEpoch` and
`CoordinatorEpoch`, restores durable identity and history, restores fenced incarnations,
invalidates every dynamic backend evidence generation (health, availability, readiness,
residency, capacity and latency become `UNKNOWN` and their generations unset), invalidates old
dispatch authority, marks every loaded route `STALE`/`RECONSTRUCTED`, and requires fresh
backend registration and fresh external evidence. Persisted historical decisions remain
history and never become dispatchable merely because they were once valid.

## Transactional mutation and invariants

Multi-step changes follow: validate -> construct candidate state -> check invariants -> persist
where required -> commit canonical mutation -> publish outcome. On failure, canonical state is
unchanged.

`ModelRouter::check_invariants()` is a callable full scan over canonical state. It proves, among
others: one current router epoch and coordinator epoch; backend incarnations unique by
`(BackendId, BackendBootId)`; fenced boots never authoritative again; monotonic model and
backend generations; valid route generations; bound generations matching the decision snapshot;
the plan primary equal to the top-ranked candidate; stable fallback order; no duplicate ranked
candidate; canonical ranking and rejection order; pre-dispatch revalidation always required;
and, for every `CURRENT` decision, that the winner exists, is not retired, is not draining, is
not a fenced or older incarnation, is bound to the current policy, router epoch, coordinator
epoch, and has not expired — plus that derived indexes equal a canonical scan and that retained
history stays bounded.

## Reference multiprocess architecture

The reference system is **REAL multiprocess execution on one physical host**: a coordinator
process and three backend worker processes, all independent operating-system processes,
communicating over framed loopback TCP (Winsock on Windows, `poll` on POSIX).

```
            router_coordinator (hosts one ModelRouter)
                     |
        framed TCP over 127.0.0.1
        /            |            \
 backend_worker  backend_worker  backend_worker
   (Backend A)     (Backend B)     (Backend C)
```

- Session termination is event-driven. On Windows the socket is registered with
  `WSAEventSelect` and the session loop waits on the socket event, a wake event used to signal
  queued output, and a stop event, so a blocked receive is interrupted by an explicit stop event
  rather than by `shutdown()` alone.
- A closed backend session fences that incarnation through the real path.
- The harness spawns real child processes with `CREATE_NO_WINDOW`, kills them with real
  operating-system termination, and observes the loss through the live socket.
- The reference control plane (`ADMIN_REQUEST`/`ADMIN_RESPONSE`) lets the harness drive current
  policy, price, budget, capability, and readiness/availability changes through the coordinator
  process. It is a reference-system facility, not part of the routing contract.

The core library does not depend on this transport and is fully usable in-process.

### REAL / SYNTHETIC / UNSUPPORTED

| Item | Status |
| --- | --- |
| Multiprocess execution on one host, framed loopback TCP, real process kill, real session loss detection | **REAL** (executed) |
| CUDA device probe: enumerate RTX 5090, allocate device memory, host-to-device copy, deterministic kernel launch, synchronize, device-to-host copy, exact CPU parity, free device memory | **REAL** (executed; `sm_120` cubin embedded) |
| Reference model profiles (small / general / specialist) | **SYNTHETIC** — they prove routing behaviour and claim no benchmark quality |
| Backend worker model step | **SYNTHETIC** — a deterministic integer transform, not inference |
| Real LLM inference, paid provider APIs, multi-GPU, MIG, NVLink, NVSwitch, RDMA, GPUDirect, DPU, remote GPU, physical multi-node routing | **UNSUPPORTED / not claimed** |

## Build

Requirements: CMake 3.20+, a C++20 compiler, and on Windows MSVC (validated with 14.44) or
another C++20 toolchain. CUDA is optional and never required.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Options (all default to ON at top level, except the CUDA proof which is auto-detected):

| Option | Effect |
| --- | --- |
| `MODEL_ROUTER_BUILD_TESTS` | builds `model_router_tests` (an `EXCLUDE_FROM_ALL` target) |
| `MODEL_ROUTER_BUILD_EXAMPLES` | builds the runnable examples |
| `MODEL_ROUTER_BUILD_TOOLS` | builds `model_router_inspect` |
| `MODEL_ROUTER_BUILD_BENCHMARKS` | builds `model_router_benchmarks` |
| `MODEL_ROUTER_BUILD_REFERENCE` | builds `router_coordinator`, `backend_worker`, `reference_harness` |
| `MODEL_ROUTER_BUILD_CUDA_PROOF` | builds the optional real CUDA device probe when CUDA is found |
| `MODEL_ROUTER_ENABLE_ASAN` | builds with AddressSanitizer |
| `MODEL_ROUTER_ENABLE_SHARED` | builds a shared library instead of a static one |

MSVC builds use `/W4 /WX /permissive- /utf-8 /EHsc /Zc:__cplusplus /Zc:preprocessor`; GCC and
Clang builds use the equivalent strict set with `-Werror`. No warning is suppressed.

## Install and downstream use

```
cmake --install build --prefix <prefix>
```

Installed layout: headers under `include/model_router` (implementation-detail headers are not
installed), `model_router.lib`/`libmodel_router.a`, and CMake package files under
`lib/cmake/model_router`. The installed package does not depend on the source tree.

Downstream usage:

```cmake
find_package(model_router 1.0.0 REQUIRED)
target_link_libraries(my_target PRIVATE model_router::model_router)
```

```cpp
#include <model_router/model_router.hpp>

model_router::ModelRouterOptions options;
options.clock = std::make_shared<model_router::LogicalClock>();
model_router::ModelRouter router(std::move(options));

router.start();
router.register_model(model);          // semantic model identity
router.register_backend(backend);      // serving identity + incarnation
router.update_health(health);          // boot-bound dynamic evidence
router.set_policy(policy);

const model_router::RouteOutcome outcome = router.route(request);
if (outcome.code == model_router::OutcomeCode::ROUTED) {
  const model_router::RouteDecision& decision = outcome.decision;
  // decision.authority binds every generation the route depends on.
}
```

## CLI

`model_router_inspect` inspects a fresh router or a persisted state file:

```
--state <file>        load a persisted router state file
--validate            validate state file integrity
--summary             print the router summary
--models              print registered models
--backends            print registered backends
--routes              print retained route decisions
--fenced              print fenced backend incarnations
--explain-route <id>  print the canonical explanation of one retained decision
--json                emit JSON where a JSON form exists
--version             print the product version string
--help                print usage
```

Exit codes: 0 success, 1 validation or load failure, 2 usage error.

## Examples

Six runnable examples use only the public API:

| Target | Demonstrates |
| --- | --- |
| `model_router_example_capability` | a required capability selects the only capable model |
| `model_router_example_cost` | two capable backends, cheaper current legal route wins |
| `model_router_example_policy` | a policy generation denies a backend, the alternate wins |
| `model_router_example_fallback` | the primary is invalidated, the fallback is freshly revalidated |
| `model_router_example_recovery` | save, restart, evidence invalidation, fresh evidence, route resumes |
| `model_router_example_distributed` | in-process demonstration of the coordinator-side plan contract |

## Tests

`model_router_tests` is an `EXCLUDE_FROM_ALL` target so a normal build stays fast:

```
cmake --build build --target model_router_tests
ctest --test-dir build --output-on-failure
```

The suite covers core routing, hard eligibility per predicate, ranking factors and determinism,
authority comparison and revalidation, capability evidence, persistence integrity and recovery,
the wire protocol and transport, deterministic concurrency races, seeded property testing,
adversarial hardening, C++ lifetime hazards, shutdown behaviour, and the optional CUDA probe.

`reference_harness` runs the multiprocess proof matrix against real processes.

## Benchmarks

`model_router_benchmarks` measures **completed routing operations**, not request submission and
not model inference. It reports candidate discovery, hard eligibility filtering, factor
construction and ranking, route-plan construction, pre-dispatch revalidation, explanation
generation, snapshots, invariant scans, and persistence save/load, at candidate-set scales of
1, 10, 100, 1 000, and 10 000, at catalog scales of 10 models/100 backends, 100/1 000, and
1 000/10 000, and across 1 000, 10 000, and 100 000 completed route decisions. Every phase is
guarded by an invariant check and a winner assertion.

## Limitations

- The reference architecture is real multiprocess execution **on one physical host**. Physical
  multi-node routing is not implemented or claimed.
- Reference model profiles are synthetic and make no quality claim. Model Router performs no
  inference; there is no bundled provider integration.
- Route-decision caching is deliberately not implemented. Correct caching would have to key on
  every semantically relevant authority generation and would still require revalidation, so no
  cache is offered rather than an unproven one. Continuity is expressed only as a ranking
  preference.
- The reference control plane exists for proof and inspection; it is not a production
  management API.
- The POSIX transport path uses `poll` and a self-pipe wakeup. It is implemented but is
  validated on Windows/Winsock in this release.
- AddressSanitizer on Windows does not support leak detection, so ASan runs prove memory-error
  freedom, not leak freedom.

## Project layout

```
include/model_router/     public API (ids, limits, clock, outcome, authority, capability,
                          evidence, catalog, request, candidate, decision, policy, providers,
                          persistence, router, reference profiles, distributed protocol and
                          transport, optional CUDA probe)
include/model_router/detail/  implementation-detail helpers (not installed)
src/                      implementation
reference/                reference coordinator, backend worker, and proof harness
tests/                    test suite and deterministic fixtures
examples/                 runnable public-API examples
tools/                    model_router_inspect
benchmarks/               routing-throughput benchmark suite
cuda/                     optional real CUDA device probe
cmake/                    package configuration
```

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
