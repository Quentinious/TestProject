# Synthetic dataset batch (Editor, UE 5.7)

Open the intended map in Editor, wait for any current PCG generation to finish,
then enter this in the Output Log command field:

```text
TestProject.GenerateSyntheticDataset DIPLOMA
```

This generates **66, 70, 82**, in that order. Alternatively, select one PCG actor
and run `TestProject.GenerateSyntheticDataset` without arguments. Selection must
resolve to exactly one original component whose graph contains the existing
`Export Synthetic Point Cloud` node. Other PCG tool components on the actor are
not mistaken for the dataset component. Multiple matching components are an error.

The first milestone supports active, non-partitioned components in an Editor
world. It rejects busy components, PIE, partition-local/partitioned generation,
and concurrent batches rather than guessing which scene or inspection stack to use.

The default output is:

```text
Saved/SyntheticDataset/Runs/
  seed_66/summary.json
  seed_66/instances.csv
  seed_66/construction_poc_001_points.ply
  seed_66/construction_poc_001_points.txt
  seed_70/...
  seed_82/...
```

Point filenames inherit the existing exporter's `BaseFileName` and suffixes;
TXT/PLY formats, sample count and sampling seed are unchanged. Both writers are
enabled for the batch. There is no separate exporter implementation.

Existing `seed_<n>` directories are rejected before changing the component,
so stale files cannot satisfy validation and previous runs are not overwritten.
For another batch, supply a fresh root relative to the project's Saved directory:

```text
TestProject.GenerateSyntheticDataset DIPLOMA SyntheticDataset/Runs/second_batch
```

Cancel using `TestProject.CancelSyntheticDatasetBatch`. Cancellation stops new
dataset runs and restores the original scene; wait for the completion/failure
message in `LogSyntheticDatasetBatch` before editing the component again.

## Execution order and UE APIs

`FSyntheticDatasetBatch` is Editor-only C++ orchestration, registered by the
project module. It uses the selected **original** `UPCGComponent` throughout.
It duplicates the existing graph into the transient package, gives its exporter
node private settings, and preserves the component's graph parameter overrides.
No placement nodes, edges, mesh assets or generation parameters are changed.

For each requested seed:

1. Set `UPCGComponent::Seed` and call `DirtyGenerated()`, matching the invalidation
   performed when editing Seed in Details.
2. Call `GenerateLocalGetTaskId(true)` and reject an invalid task ID.
3. Wait for `OnPCGGraphGeneratedDelegate`; cancellation is detected through
   `OnPCGGraphCancelledDelegate`. The callback only records completion.
4. On a subsequent Editor ticker iteration, require generation and refresh to be
   inactive. This avoids executing inside UE's `PostProcessGraph` callback stack.
5. Read the actual final object points at the exporter node's `In` pin from
   `FPCGGraphExecutionInspection`. Require exactly one executed exporter stack.
6. Execute the existing exporter's element with those object points, copied
   export settings and the same generating component as `ExecutionSource`.
7. Validate output and only then request the next seed.

The graph's exporter is temporarily prevented from writing during generation.
Otherwise it would export **before** the graph completion callback. Editor
inspection retains its actual final input, including assigned InstanceIds, so the
post-completion call reuses the existing sampling/writing code without replaying
placement or constructing replacement object points. No sleep or arbitrary
delay determines completion; a 240-second per-phase watchdog only stops a stalled run.

Each run requires:

- `summary.scene_seed == requested seed`;
- brick `class_id == 3`;
- `instance_count == unique_instance_count == actual final brick object count`;
- `instance_ids_unique == true`;
- non-empty TXT and PLY outputs.
- `instances.csv` from the same exporter transaction (one row per brick).

Captured ClassId 3 transforms must also differ from every previous run. The
55/51/56 baseline counts are asserted by the integration test, not hardcoded
into production orchestration. Any failure stops the batch with its requested
seed and reason. Already validated runs are retained; no later seed is exported.

## Restoration and validation

Editor auto-refresh is temporarily disabled on the selected component. Normal
completion, cancellation and validation failure regenerate the original seed
with export disabled, then restore the original graph parent, parameter overrides
and auto-refresh flag. This keeps the visible generated scene consistent with
the restored Seed. Restoration itself creates no dataset output.

The batch never saves the graph or map. UE generation can mark the level dirty,
just as manual Generate does; the tool does not clear unrelated dirty state or
save it automatically. On forced Editor/module shutdown the references and seed
are restored immediately, but an asynchronous scene regeneration cannot be
guaranteed after shutdown or component destruction.

`TestProject.GroundTruth.Batch.ThreeSeeds` dispatches the same public console
command and runs the real `/Game/DIPLOMA` graph through all three seeds. It checks
55/51/56 objects, the controller's per-run summary/identity/file checks and
restoration. Its outputs use a fresh
`Saved/SyntheticDataset/Runs/validation_<run id>/seed_<n>` root.
It also verifies that CSV brick rows equal summary instance/unique counts, each
dimension and box volume is finite and positive, each volume equals X * Y * Z,
and summary total/mean/min/max match CSV (relative tolerance 1e-9). Bounds use
the final transformed LOD 0 geometry's world AABB; see
[geometric baseline semantics](GROUND_TRUTH_EXPORT.md#geometric-bounding-volume-baseline-brick-class-3).
`ExternalSeedEditStopsBatch` changes Seed after generation and confirms failure
before writing any run or proceeding to later seeds. Additional checks cover
concurrent starts, existing output rejection, missing component and command registration.

Run these together with existing tests using `Automation RunTests
TestProject.GroundTruth` in a separate Editor process. The real graph needs GPU
PCG execution. Local launches use `-DDC-ForceMemoryCache`; the existing test setup
also uses `-ddc=InstalledNoZenLocalFallback -DisablePlugins=Fab -RenderOffscreen`.
These are process arguments, not project/DDC configuration changes.

Local UE 5.7 validation passed: Win64 Development Editor build and all nine
ground-truth cases, including the console-command integration and interrupted
batch case. Each successful run contains 200,000 TXT/PLY records and summary
seed/count/unique values `66/55/55`, `70/51/51`, `82/56/56`, plus instances.csv
with matching brick rows and verified bounding volumes. TXT/PLY SHA-256 hashes
match the previous pre-volume batch for each seed. Point positions differ
between every pair of runs. Existing missing environment asset/material warnings
appear during map loading; no test errors remain.
The report is in `Saved/Automation/BoundingVolumesReport`. The most recent local
three-run output is
`Saved/SyntheticDataset/Runs/validation_D754411D4557FB1A38A9A2BF07B63FCF`.
