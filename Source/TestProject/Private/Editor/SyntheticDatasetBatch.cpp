#include "Editor/SyntheticDatasetBatch.h"

#if WITH_EDITOR

#include "PCG/PCGExportSyntheticPointCloudSettings.h"
#include "PCGComponent.h"
#include "PCGContext.h"
#include "PCGGraphExecutionInspection.h"
#include "PCGPin.h"
#include "Data/PCGBasePointData.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Metadata/Accessors/PCGAttributeAccessorHelpers.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogSyntheticDatasetBatch, Log, All);

namespace
{
	TWeakPtr<FSyntheticDatasetBatch> RunningBatch;
	TSharedPtr<FSyntheticDatasetBatch> ConsoleBatch;
	TUniquePtr<FAutoConsoleCommand> GenerateCommand;
	TUniquePtr<FAutoConsoleCommand> CancelCommand;
	constexpr double PhaseTimeoutSeconds = 240.0;

	bool SameTransforms(const TArray<FTransform>& A, const TArray<FTransform>& B)
	{
		if (A.Num() != B.Num()) { return false; }
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			if (!A[Index].Equals(B[Index], 0.0)) { return false; }
		}
		return true;
	}

	bool ValidRoot(const FString& Root)
	{
		FString Normalized = Root;
		FPaths::NormalizeDirectoryName(Normalized);
		TArray<FString> Parts;
		Normalized.ParseIntoArray(Parts, TEXT("/"), false);
		return !Normalized.IsEmpty() && FPaths::IsRelative(Normalized) &&
			!Parts.Contains(TEXT("..")) && !Parts.Contains(TEXT(".")) && !Normalized.Contains(TEXT(":"));
	}
}

TSharedPtr<FSyntheticDatasetBatch> FSyntheticDatasetBatch::Start(UPCGComponent* InComponent, const TArray<int32>& InSeeds, const FString& InRoot, FString& Error)
{
	Error.Reset();
	auto Reject = [&Error](const TCHAR* Reason) -> TSharedPtr<FSyntheticDatasetBatch> { Error = Reason; return nullptr; };
	if (const auto Existing = RunningBatch.Pin(); Existing && !Existing->IsFinished()) { return Reject(TEXT("A synthetic dataset batch is already running.")); }
	if (!IsValid(InComponent) || !InComponent->GetWorld() || InComponent->GetWorld()->WorldType != EWorldType::Editor)
	{
		return Reject(TEXT("Select a PCG component in an Editor world (PIE is not supported)."));
	}
	if (InComponent->IsLocalComponent() || InComponent->IsPartitioned()) { return Reject(TEXT("This milestone requires an original, non-partitioned PCG component.")); }
	if (!InComponent->bActivated || !InComponent->GetGraph()) { return Reject(TEXT("The PCG component must be active and have a graph.")); }
	if (InComponent->IsGenerating() || InComponent->IsRefreshInProgress() || InComponent->IsCleaningUp()) { return Reject(TEXT("The PCG component is busy; wait for its current generation/refresh/cleanup to finish.")); }
	if (InSeeds.IsEmpty() || InSeeds.Num() > 3 || TSet<int32>(InSeeds).Num() != InSeeds.Num()) { return Reject(TEXT("Supply one to three distinct seeds; ranges are not supported.")); }
	if (!ValidRoot(InRoot)) { return Reject(TEXT("Output root must be a relative path under Saved without '.' or '..'.")); }
	for (int32 Seed : InSeeds)
	{
		const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), InRoot, FString::Printf(TEXT("seed_%d"), Seed));
		if (IFileManager::Get().DirectoryExists(*Directory) || IFileManager::Get().FileExists(*Directory))
		{
			Error = FString::Printf(TEXT("Refusing to reuse existing run directory '%s'; choose a fresh output root."), *Directory);
			return nullptr;
		}
	}

	TSharedPtr<FSyntheticDatasetBatch> Batch = MakeShared<FSyntheticDatasetBatch>();
	Batch->BatchGraph.Reset(DuplicateObject<UPCGGraph>(InComponent->GetGraph(), GetTransientPackage()));
	for (UPCGNode* Node : Batch->BatchGraph->GetNodes())
	{
		if (Cast<UPCGExportSyntheticPointCloudSettings>(Node->GetSettings()))
		{
			if (Batch->ExportNode) { return Reject(TEXT("The graph must contain exactly one Export Synthetic Point Cloud node.")); }
			Batch->ExportNode = Node;
		}
	}
	if (!Batch->ExportNode) { return Reject(TEXT("The graph needs one Export Synthetic Point Cloud node.")); }
	// Never change the source asset. Preserve all placement settings and graph parameters.
	// Settings instances may reference a shared settings asset outside the graph.
	// Give the transient exporter node its own settings before changing any flags.
	auto* NodeSettings = DuplicateObject<UPCGExportSyntheticPointCloudSettings>(CastChecked<UPCGExportSyntheticPointCloudSettings>(Batch->ExportNode->GetSettings()), Batch->ExportNode);
	NodeSettings->bEnabled = true;
	NodeSettings->bExportEnabled = false;
	Batch->ExportNode->SetSettingsInterface(NodeSettings);
	Batch->Component = InComponent;
	Batch->OriginalParent.Reset(InComponent->GetGraphInstance()->Graph);
	Batch->OriginalInstanceSnapshot.Reset(DuplicateObject<UPCGGraphInstance>(InComponent->GetGraphInstance(), GetTransientPackage()));
	Batch->OriginalOverrides = InComponent->GetGraphInstance()->ParametersOverrides;
	Batch->OriginalSeed = InComponent->Seed;
	Batch->bOriginalRegenerate = InComponent->bRegenerateInEditor;
	Batch->Seeds = InSeeds;
	Batch->OutputRoot = InRoot;
	InComponent->bRegenerateInEditor = false;
	InComponent->SetGraphLocal(Batch->BatchGraph.Get());
	InComponent->GetGraphInstance()->ParametersOverrides = Batch->OriginalOverrides;
	InComponent->GetExecutionState().GetInspection().EnableInspection();
	Batch->bInspectionEnabled = true;
	Batch->GeneratedHandle = InComponent->OnPCGGraphGeneratedDelegate.AddSP(Batch.ToSharedRef(), &FSyntheticDatasetBatch::OnGenerated);
	Batch->CancelledHandle = InComponent->OnPCGGraphCancelledDelegate.AddSP(Batch.ToSharedRef(), &FSyntheticDatasetBatch::OnCancelled);
	Batch->PhaseStarted = FPlatformTime::Seconds();
	Batch->Phase = EPhase::NextSeed;
	Batch->TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSP(Batch.ToSharedRef(), &FSyntheticDatasetBatch::Tick));
	RunningBatch = Batch;
	UE_LOG(LogSyntheticDatasetBatch, Display, TEXT("Batch started for %s; %d seeds; output Saved/%s"), *InComponent->GetOwner()->GetActorLabel(), InSeeds.Num(), *InRoot);
	return Batch;
}

TSharedPtr<FSyntheticDatasetBatch> FSyntheticDatasetBatch::GetActive() { return RunningBatch.Pin(); }

FSyntheticDatasetBatch::~FSyntheticDatasetBatch() { Shutdown(); }

void FSyntheticDatasetBatch::OnGenerated(UPCGComponent* Source)
{
	if (Source == Component.Get() && (Phase == EPhase::Generating || Phase == EPhase::Restoring)) { bGenerated = true; }
}

void FSyntheticDatasetBatch::OnCancelled(UPCGComponent* Source)
{
	if (!bIgnoreCancellation && Source == Component.Get()) { bCancelled = true; }
}

void FSyntheticDatasetBatch::Generate(int32 Seed, EPhase NewPhase)
{
	Phase = NewPhase;
	PhaseStarted = FPlatformTime::Seconds();
	bGenerated = bCancelled = false;
	Component->Seed = Seed; // Always the original component, never a generated/local copy.
	Component->DirtyGenerated(); // Same invalidation as editing Seed in Details.
	Component->GetExecutionState().GetInspection().ClearInspectionData();
	const FPCGTaskId Task = Component->GenerateLocalGetTaskId(true);
	if (Task == InvalidPCGTaskId)
	{
		if (NewPhase == EPhase::Restoring)
		{
			Failure += TEXT(" Could not regenerate the original scene during restoration.");
			Finish();
		}
		else { Fail(TEXT("PCG refused the forced generation request.")); }
	}
}

bool FSyntheticDatasetBatch::Tick(float)
{
	if (IsFinished()) { return false; }
	if (!Component.IsValid()) { Failure += TEXT(" PCG component was destroyed."); Finish(); return false; }
	if (FPlatformTime::Seconds() - PhaseStarted > PhaseTimeoutSeconds)
	{
		if (Phase == EPhase::Restoring) { Failure += TEXT(" Restoration generation timed out."); Finish(); }
		else { Fail(TEXT("Timed out waiting for PCG generation/export completion (240 seconds).")); }
		return !IsFinished();
	}
	if (bCancelled)
	{
		if (Phase == EPhase::Restoring) { Failure += TEXT(" Restoration generation was cancelled."); Finish(); }
		else { Fail(TEXT("PCG generation was cancelled.")); }
		return !IsFinished();
	}
	if (Component->GetGraph() != BatchGraph.Get()) { Failure += TEXT(" Component graph changed during batch."); Finish(); return false; }
	if (Phase == EPhase::NextSeed)
	{
		Generate(Seeds[SeedIndex], EPhase::Generating);
	}
	else if ((Phase == EPhase::Generating || Phase == EPhase::Restoring) && bGenerated &&
		!Component->IsGenerating() && !Component->IsGenerationInProgress() && !Component->IsRefreshInProgress())
	{
		// The delegate runs inside PostProcessGraph. Defer work until it has unwound;
		// never change seeds or export reentrantly from the callback itself.
		if (Phase == EPhase::Restoring) { Finish(); return false; }
		FString Error;
		if (!BeginExport(Error)) { Fail(Error); }
	}
	else if (Phase == EPhase::Exporting)
	{
		if (Component->Seed != Seeds[SeedIndex]) { Fail(TEXT("component seed changed before export completed.")); }
		else if (ExportElement->Execute(ExportContext.Get()))
		{
			FString Error;
			if (!ValidateExport(Error)) { Fail(Error); }
			else
			{
				UE_LOG(LogSyntheticDatasetBatch, Display, TEXT("Seed %d verified: brick count=%d, unique=%d; %s"), PendingRun.Seed, PendingRun.BrickCount, PendingRun.BrickCount, *PendingRun.Directory);
				Runs.Add(MoveTemp(PendingRun));
				ExportContext.Reset();
				ExportElement.Reset();
				ExportSettings.Reset();
				if (++SeedIndex == Seeds.Num()) { BeginRestore(); }
				else { Phase = EPhase::NextSeed; PhaseStarted = FPlatformTime::Seconds(); }
			}
		}
	}
	return !IsFinished();
}

bool FSyntheticDatasetBatch::BeginExport(FString& Error)
{
	if (Component->Seed != Seeds[SeedIndex]) { Error = TEXT("component seed changed after generation."); return false; }
	auto& Inspection = Component->GetExecutionState().GetInspection();
	const auto Stacks = Inspection.GetExecutedNodeStacks();
	const auto* Executions = Stacks.Find(ExportNode);
	if (!Executions || Executions->Num() != 1) { Error = TEXT("Expected exactly one exporter execution with captured final object inputs."); return false; }
	FPCGStack Stack = Executions->begin()->Stack;
	Stack.GetStackFramesMutable().Emplace(ExportNode);
	Stack.GetStackFramesMutable().Emplace(ExportNode->GetInputPin(TEXT("In")));
	const FPCGDataCollection* Data = Inspection.GetInspectionData(Stack);
	if (!Data || Data->TaggedData.IsEmpty()) { Error = TEXT("Final exporter input inspection data is empty or unavailable."); return false; }

	PendingRun = {};
	PendingRun.Seed = Seeds[SeedIndex];
	TSet<int32> BrickIds;
	for (const FPCGTaggedData& Item : Data->TaggedData)
	{
		const auto* Points = Cast<UPCGBasePointData>(Item.Data);
		if (!Points) { Error = TEXT("Final object input contains non-point data."); return false; }
		auto ReadInts = [Points](const TCHAR* Name, TArray<int32>& Values)
		{
			const auto Selector = FPCGAttributePropertySelector::CreateAttributeSelector(Name);
			const auto Accessor = PCGAttributeAccessorHelpers::CreateConstAccessor(Points, Selector);
			const auto Keys = PCGAttributeAccessorHelpers::CreateConstKeys(Points, Selector);
			return Accessor && Keys && PCGAttributeAccessorHelpers::ExtractAllValues(Accessor.Get(), Keys.Get(), Values, EPCGAttributeAccessorFlags::StrictType);
		};
		TArray<int32> Classes, Ids;
		if (!ReadInts(TEXT("ClassId"), Classes) || !ReadInts(TEXT("InstanceId"), Ids) || Classes.Num() != Points->GetNumPoints() || Ids.Num() != Classes.Num())
		{
			Error = TEXT("Final object input has invalid ClassId/InstanceId metadata."); return false;
		}
		const auto Transforms = Points->GetTransformsCopy();
		for (int32 Index = 0; Index < Classes.Num(); ++Index)
		{
			if (Classes[Index] == 3)
			{
				PendingRun.BrickTransforms.Add(Transforms[Index]);
				BrickIds.Add(Ids[Index]);
			}
		}
	}
	PendingRun.BrickCount = PendingRun.BrickTransforms.Num();
	if (BrickIds.Num() != PendingRun.BrickCount) { Error = TEXT("Final brick instances have duplicate InstanceId values."); return false; }
	for (const auto& Previous : Runs)
	{
		if (SameTransforms(Previous.BrickTransforms, PendingRun.BrickTransforms))
		{
			Error = FString::Printf(TEXT("Brick transforms are identical to seed %d; scene variation was not verified."), Previous.Seed); return false;
		}
	}
	ExportSettings.Reset(DuplicateObject<UPCGExportSyntheticPointCloudSettings>(CastChecked<UPCGExportSyntheticPointCloudSettings>(ExportNode->GetSettings()), GetTransientPackage()));
	ExportSettings->bExportEnabled = true;
	ExportSettings->bEnabled = true;
	ExportSettings->bWritePly = ExportSettings->bWriteTxt = true;
	ExportSettings->OutputSubdirectory = FPaths::Combine(OutputRoot, FString::Printf(TEXT("seed_%d"), PendingRun.Seed));
	PendingRun.Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), ExportSettings->OutputSubdirectory));
	FPCGDataCollection Input = *Data;
	auto& SettingsInput = Input.TaggedData.Emplace_GetRef();
	SettingsInput.Pin = TEXT("Settings");
	SettingsInput.Data = ExportSettings.Get();
	Input.ComputeCrcs(false);
	ExportElement = ExportSettings->GetElement();
	// Actual final object data and the same generating component; no invented seed.
	ExportContext.Reset(ExportElement->Initialize(FPCGInitializeElementParams(&Input, Component.Get(), nullptr)));
	ExportContext->InitializeSettings();
	ExportContext->AsyncState.NumAvailableTasks = 1;
	Phase = EPhase::Exporting;
	PhaseStarted = FPlatformTime::Seconds();
	return true;
}

bool FSyntheticDatasetBatch::ValidateExport(FString& Error)
{
	FString Json;
	TSharedPtr<FJsonObject> Summary;
	if (!FFileHelper::LoadFileToString(Json, *FPaths::Combine(PendingRun.Directory, TEXT("summary.json"))) ||
		!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Summary) || !Summary.IsValid())
	{
		Error = TEXT("Exporter did not produce a readable summary.json."); return false;
	}
	double Seed = 0, Count = 0, Unique = 0, Class = 0;
	bool bUnique = false;
	const TSharedPtr<FJsonObject>* Classes = nullptr;
	const TSharedPtr<FJsonObject>* Brick = nullptr;
	if (!Summary->TryGetNumberField(TEXT("scene_seed"), Seed) || Seed != PendingRun.Seed ||
		!Summary->TryGetObjectField(TEXT("classes"), Classes) || !(*Classes)->TryGetObjectField(TEXT("brick"), Brick) ||
		!(*Brick)->TryGetNumberField(TEXT("class_id"), Class) || Class != 3 ||
		!(*Brick)->TryGetNumberField(TEXT("instance_count"), Count) || Count != PendingRun.BrickCount ||
		!(*Brick)->TryGetNumberField(TEXT("unique_instance_count"), Unique) || Unique != Count ||
		!(*Brick)->TryGetBoolField(TEXT("instance_ids_unique"), bUnique) || !bUnique)
	{
		Error = TEXT("Summary seed/ClassId/brick count/unique InstanceId validation failed."); return false;
	}
	for (const TCHAR* Suffix : {TEXT("_points.ply"), TEXT("_points.txt")})
	{
		if (IFileManager::Get().FileSize(*FPaths::Combine(PendingRun.Directory, ExportSettings->BaseFileName + Suffix)) <= 0)
		{
			Error = FString::Printf(TEXT("Exporter did not produce %s%s."), *ExportSettings->BaseFileName, Suffix); return false;
		}
	}
	if (IFileManager::Get().FileSize(*FPaths::Combine(PendingRun.Directory, TEXT("instances.csv"))) <= 0)
	{
		Error = TEXT("Exporter did not produce instances.csv."); return false;
	}
	return true;
}

void FSyntheticDatasetBatch::Fail(const FString& Error)
{
	if (Failure.IsEmpty())
	{
		Failure = FString::Printf(TEXT("Seed %d: %s"), Seeds.IsValidIndex(SeedIndex) ? Seeds[SeedIndex] : OriginalSeed, *Error);
		UE_LOG(LogSyntheticDatasetBatch, Error, TEXT("Batch stopped. %s"), *Failure);
	}
	BeginRestore();
}

void FSyntheticDatasetBatch::BeginRestore()
{
	ExportContext.Reset(); ExportElement.Reset(); ExportSettings.Reset();
	if (!Component.IsValid()) { Finish(); return; }
	bIgnoreCancellation = true;
	if (Component->IsGenerating()) { Component->CancelGeneration(); }
	bIgnoreCancellation = false;
	// Regenerate the original seed using identical placement settings and disabled
	// export, so the restored component seed and visible scene remain consistent.
	Generate(OriginalSeed, EPhase::Restoring);
}

void FSyntheticDatasetBatch::RestoreReferences()
{
	if (bReferencesRestored) { return; }
	bReferencesRestored = true;
	if (Component.IsValid())
	{
		Component->OnPCGGraphGeneratedDelegate.Remove(GeneratedHandle);
		Component->OnPCGGraphCancelledDelegate.Remove(CancelledHandle);
		if (bInspectionEnabled) { Component->GetExecutionState().GetInspection().DisableInspection(); }
		Component->Seed = OriginalSeed;
		Component->SetGraphLocal(OriginalParent.Get());
		Component->GetGraphInstance()->ParametersOverrides = OriginalOverrides;
		Component->bRegenerateInEditor = bOriginalRegenerate;
	}
}

void FSyntheticDatasetBatch::Finish()
{
	// Remove callbacks before cancelling to avoid re-entrant failure callbacks.
	RestoreReferences();
	if (Component.IsValid() && Component->IsGenerating()) { Component->CancelGeneration(); }
	Phase = EPhase::Finished;
	UE_LOG(LogSyntheticDatasetBatch, Display, TEXT("Batch %s; %d verified runs. %s"), Failure.IsEmpty() ? TEXT("completed") : TEXT("failed"), Runs.Num(), *Failure);
}

void FSyntheticDatasetBatch::Cancel()
{
	if (!IsFinished()) { Fail(TEXT("Batch cancelled by user.")); }
}

void FSyntheticDatasetBatch::Shutdown()
{
	FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
	if (!IsFinished()) { Failure += TEXT(" Batch owner/editor shut down."); Finish(); }
}

void RegisterSyntheticDatasetBatchCommands()
{
	GenerateCommand = MakeUnique<FAutoConsoleCommand>(TEXT("TestProject.GenerateSyntheticDataset"),
		TEXT("Generate seeds 66,70,82. Optional: ActorLabel RelativeOutputRoot. Otherwise select exactly one PCG actor. Default root: SyntheticDataset/Runs."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (!GEditor || GEditor->PlayWorld || Args.Num() > 2) { UE_LOG(LogSyntheticDatasetBatch, Error, TEXT("Use in Editor: TestProject.GenerateSyntheticDataset [ActorLabel] [RelativeOutputRoot]")); return; }
			TArray<UPCGComponent*> Candidates;
			UWorld* World = GEditor->GetEditorWorldContext().World();
			for (TActorIterator<AActor> Actor(World); Actor; ++Actor)
			{
				if (Args.IsEmpty() ? !Actor->IsSelected() : Actor->GetActorLabel() != Args[0]) { continue; }
				TArray<UPCGComponent*> Components;
				Actor->GetComponents(Components);
				for (UPCGComponent* Item : Components)
				{
					if (Item->IsLocalComponent() || !Item->GetGraph()) { continue; }
					bool bHasExporter = false;
					for (UPCGNode* Node : Item->GetGraph()->GetNodes())
					{
						bHasExporter |= Cast<UPCGExportSyntheticPointCloudSettings>(Node->GetSettings()) != nullptr;
					}
					if (bHasExporter) { Candidates.Add(Item); }
				}
			}
			if (Candidates.Num() != 1) { UE_LOG(LogSyntheticDatasetBatch, Error, TEXT("Expected exactly one PCG component with an exporter; found %d. Select one actor or specify its unique label."), Candidates.Num()); return; }
			FString Error;
			auto NewBatch = FSyntheticDatasetBatch::Start(Candidates[0], {66, 70, 82}, Args.Num() == 2 ? Args[1] : TEXT("SyntheticDataset/Runs"), Error);
			if (NewBatch) { ConsoleBatch = NewBatch; }
			else { UE_LOG(LogSyntheticDatasetBatch, Error, TEXT("Cannot start batch: %s"), *Error); }
		}));
	CancelCommand = MakeUnique<FAutoConsoleCommand>(TEXT("TestProject.CancelSyntheticDatasetBatch"), TEXT("Stop the active console batch and restore the original scene."),
		FConsoleCommandDelegate::CreateLambda([] { if (ConsoleBatch) { ConsoleBatch->Cancel(); } }));
}

void UnregisterSyntheticDatasetBatchCommands()
{
	if (ConsoleBatch) { ConsoleBatch->Shutdown(); ConsoleBatch.Reset(); }
	GenerateCommand.Reset(); CancelCommand.Reset();
}

#endif
