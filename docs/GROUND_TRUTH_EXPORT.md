# Ground-truth instance summary

The existing `Export Synthetic Point Cloud` node writes `summary.json` alongside
`<BaseFileName>_points.txt` and `<BaseFileName>_points.ply` under
`Saved/<OutputSubdirectory>`. The existing export enable switch and point-cloud
formats are unchanged. All enabled outputs, including the summary, participate
in the existing temporary-file / backup / rollback transaction.

`summary.json` describes the latest successful export in that directory. Use a
separate `OutputSubdirectory` for each scene when keeping multiple scenes.

## Object and class semantics

In `/Game/DIPLOMA`, object points pass through the static mesh spawners, then
`MergePoints_0`, `AssignDatasetInstanceIds_0` and `ExportSyntheticPointCloud_0`.
One input point supplies one mesh path and transform to the exporter. The
exporter counts these object points before it constructs triangles or samples
surfaces. Neither mesh vertex counts nor `TargetPointCount` affect instance counts.

The graph inspection found the following assignments before the authorized fix:

| ClassId | Mesh/type |
| ---: | --- |
| 0 | `wood_pallet`, `modelA2`, `brick_stack` |
| 1 | `dirt_brick_pile` |
| 2 | `modelB1` |

The authorized graph change assigns the previously unused **ClassId 3** to
`brick_stack` in `AddAttribute_15` and adds
`StaticMeshSpawner_12.Out -> MergePoints_0.In`. No other class assignment,
placement setting or existing edge is changed. Other class IDs retain their
existing semantics, including the existing mixed class 0.

The exporter identifies bricks by the confirmed class mapping `ClassId == 3`,
not by assuming every input object is a brick. It also reports other classes
under `class_<id>` without inventing semantic names. The brick entry is included
with zero counts when other classes are present but bricks are absent. The
existing exporter still aborts on entirely empty input with no surface geometry.

Example schema (counts are illustrative, not a measured scene baseline):

```json
{
  "scene_seed": 66,
  "classes": {
    "brick": {
      "class_id": 3,
      "instance_count": 3,
      "unique_instance_count": 3,
      "instance_ids_unique": true
    }
  }
}
```

`Assign Dataset Instance IDs` assigns IDs `0..N-1` per input point-data batch.
The current graph merges object streams before this node, so the intended
single-batch flow provides a unique ID per exported object. The exporter checks
uniqueness over all batches and counts unique IDs separately for each class.
Duplicate IDs retain the existing warning and export behavior; object counts are
not silently deduplicated. In that case, `instance_ids_unique` is false for the
affected class, and the object/unique-ID invariant has failed.

## Scene seed and historical counts

`scene_seed` is the editable `Seed` of the original generating `UPCGComponent`.
The exporter casts `FPCGContext::ExecutionSource` to `UPCGComponent`, resolves
`GetOriginalComponent()` for partition-local execution, and reads `Seed`.
In UE 5.7 this property is `EditAnywhere` / `BlueprintReadWrite` in Settings;
`FPCGComponentExecutionState::GetSeed()` returns that same property. Ordinary
Generate/Regenerate schedules the component without replacing its seed.
No extra seed metadata or graph wiring is required.

`Context->GetSeed()` is deliberately not used: it passes through node settings,
which can combine seeds or return a default for nodes with `UseSeed() == false`.
`SamplingSeed` controls only surface sampling. Neither is the scene seed.
Standalone exporter calls without a component execution source still emit
`scene_seed: null` rather than inventing a value. Zero and negative component
seeds are valid integers and are preserved.

The user subsequently confirmed manual end-to-end results for the `brick_stack`
branch: 66 -> 55, 70 -> 51 and 82 -> 56, with unique brick InstanceId counts
matching each object count. These counts are not hardcoded into the exporter.

## Validation and manual graph review

The graph was modified through Unreal's Python Editor API, not binary patching.
The local `Saved/GroundTruthGraphReview/change.json` records the class assignments
and exact new edge. Text exports of each node's settings before and after verify
that every other node setting is identical and every existing edge is retained.
The pre-change asset is backed up there as `DIPLOMA.before.uasset` for review.
These local review artifacts are not committed.

Before committing any `Content/` changes, open `/Game/DIPLOMA` in UE 5.7 and review
`AddAttribute_15` (ClassId 3) and the new spawner-to-Merge edge. The exporter remains
disabled in the saved graph. Enable it for a deliberate export only after review.
Only the authorized graph asset was saved by the change script; the map and
other existing Content changes are not part of the C++ commit.

Build from the repository using:

```powershell
& "A:\UE_5.7\Engine\Build\BatchFiles\Build.bat" `
  TestProjectEditor Win64 Development `
  "-Project=A:\UEProjects\TestProject_clean\TestProject.uproject" `
  -WaitMutex -NoHotReloadFromIDE
```

Automation tests under `TestProject.GroundTruth.Export` execute the actual exporter
on multiple input batches and classes, varying surface sample counts. They verify
object counts, unique IDs, duplicate diagnostics, zero bricks, disabled export,
TXT/PLY schemas and record counts, and deterministic repeated PLY output.
`TestProject.GroundTruth.Export.ComponentSeed` checks component-context seed
serialization, including zero and negative seeds, with different node and
sampling seeds. Existing standalone cases also verify the null-source fallback.
On Windows, they also lock an existing summary and verify that a failed
replacement preserves all three previous output files.

The opt-in `TestProject.GroundTruth.Scene.PreservePlacement` test loads the current
`/Game/DIPLOMAS` map and compares transient pre-change and post-change graph copies
for each of its two active volumes, `DIPLOMA` (currently seed 82) and `DIPLOMA2`
(currently seed 42). It reads `AddAttribute_16.Out`, the immediate final stream
consumed by `StaticMeshSpawner_12`, compares brick transforms and counts, checks
other class counts, and enables the actual exporter node in the transient
post-change graph. The normal PCG executor initializes its context from the
generating component and writes to a fresh
`Saved/Automation/GroundTruthScene/<actor label>/<run id>` directory. This prevents
stale output from satisfying a failed export. The summary seed must match the
same component that generated those object points; the test no longer calls
the exporter separately. An empty stream is accepted only
when inspection confirms that the nodes executed and produced no output data;
fully empty scenes retain the existing no-export behavior. The test temporarily
disables editor auto-refresh to avoid racing explicit generations and restores
the flag afterward. It never saves the map or graph. Run it in a separate editor
process; it requires the scene's GPU PCG execution to be available.

For local automated launches, use `-DDC-ForceMemoryCache` (and, if Zen readiness
fails, `-ddc=InstalledNoZenLocalFallback`). `-DisablePlugins=Fab` avoids a local
restored Fab-tab startup assertion during headless tests. These are temporary
launch arguments, not project configuration changes.

Local verification on UE 5.7 succeeded: the Win64 Development Editor build and
all five automation cases passed, including real PCG graph execution with
`scene_seed = 82` and brick instance/unique counts `56 / 56`.
The duplicate-ID case emits the expected
existing warning; scene loading warns about an unavailable Fab material while
Fab is disabled for the unattended launch.

| Current volume | Component seed | Final brick objects before / after | Exported brick instances / unique IDs |
| --- | ---: | ---: | ---: |
| DIPLOMA | 82 | 56 / 56 | 56 / 56 |
| DIPLOMA2 | 42 | 0 / 0 | Empty stream; no export requested |

For DIPLOMA, brick transforms matched exactly and the other exported class
(ClassId 0, 424 objects) was unchanged. TXT and PLY were generated successfully
from the real scene. The local automation report is under
`Saved/Automation/GroundTruthSeedReport`. The automated scene test did not change
the scene's component seed; the 66/70/82 count baselines above were confirmed
separately by the user's manual end-to-end checks before this seed change.
The test suppresses map-load exports in memory and restores the asset's export
flag afterward, so existing files in `Saved/SyntheticDataset` are not overwritten.
