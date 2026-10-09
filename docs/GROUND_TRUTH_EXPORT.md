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
  "scene_seed": null,
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

The summary currently emits `scene_seed: null`. The existing C++ pipeline does
not establish which scene-level seed was recorded in the historical baseline.
`SamplingSeed` controls only surface sampling; the PCG execution seed can combine
node and component seeds. Neither is substituted for the real scene seed.
To fill this field, explicitly pass the confirmed scene-level generation seed
through the dataset pipeline (for example, a named scene parameter/metadata with
a defined producer), and validate that value before serialization. The source
contains `TODO(SceneSeed)` at the serialization site.

`GROUND_TRUTH_BASELINE.md` records 66 -> 55, 70 -> 51 and 82 -> 56, but does not
name `StaticMeshSpawner_12` or otherwise establish that those measurements came
from `brick_stack`. They are retained as historical reference, not used as
acceptance assertions or hardcoded into the exporter.

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
On Windows, they also lock an existing summary and verify that a failed
replacement preserves all three previous output files.

The opt-in `TestProject.GroundTruth.Scene.PreservePlacement` test loads the current
`/Game/DIPLOMAS` map and compares transient pre-change and post-change graph copies
for each of its two active volumes, `DIPLOMA` (currently seed 82) and `DIPLOMA2`
(currently seed 42). It reads `AddAttribute_16.Out`, the immediate final stream
consumed by `StaticMeshSpawner_12`, compares brick transforms and counts, checks
other class counts, and exports the real merged object points to a fresh
`Saved/Automation/GroundTruthScene/<actor label>/<run id>` directory. This prevents
stale output from satisfying a failed export. An empty stream is accepted only
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
all four automation cases passed. The duplicate-ID case emits the expected
existing warning; scene loading warns about an unavailable Fab material while
Fab is disabled for the unattended launch.

| Current volume | Component seed | Final brick objects before / after | Exported brick instances / unique IDs |
| --- | ---: | ---: | ---: |
| DIPLOMA | 82 | 56 / 56 | 56 / 56 |
| DIPLOMA2 | 42 | 0 / 0 | Empty stream; no export requested |

For DIPLOMA, brick transforms matched exactly and the other exported class
(ClassId 0, 424 objects) was unchanged. TXT and PLY were generated successfully
from the real scene. The local automation report is under
`Saved/Automation/GroundTruthFinalReport`. Seeds 66 and 70 were not changed or
tested; the historical baseline's branch provenance remains unconfirmed.
