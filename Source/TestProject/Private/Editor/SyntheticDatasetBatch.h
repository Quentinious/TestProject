#pragma once

#if WITH_EDITOR

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "PCGGraph.h"
#include "UObject/StrongObjectPtr.h"

class UPCGComponent;
class UPCGExportSyntheticPointCloudSettings;
struct FPCGContext;

struct FSyntheticDatasetRun
{
	int32 Seed = 0;
	int32 BrickCount = 0;
	FString Directory;
	TArray<FTransform> BrickTransforms;
};

/** Editor orchestration only; all point sampling and writing stay in the existing exporter. */
class FSyntheticDatasetBatch : public TSharedFromThis<FSyntheticDatasetBatch>
{
public:
	static TSharedPtr<FSyntheticDatasetBatch> Start(UPCGComponent* Component, const TArray<int32>& Seeds, const FString& OutputRoot, FString& Error);
	static TSharedPtr<FSyntheticDatasetBatch> GetActive();
	~FSyntheticDatasetBatch();
	void Cancel();
	void Shutdown();
	bool IsFinished() const { return Phase == EPhase::Finished; }
	bool Succeeded() const { return IsFinished() && Failure.IsEmpty(); }
	const FString& GetFailure() const { return Failure; }
	const TArray<FSyntheticDatasetRun>& GetRuns() const { return Runs; }

private:
	enum class EPhase { NextSeed, Generating, Exporting, Restoring, Finished };
	bool Tick(float DeltaTime);
	void OnGenerated(UPCGComponent* Source);
	void OnCancelled(UPCGComponent* Source);
	void Generate(int32 Seed, EPhase NewPhase);
	bool BeginExport(FString& Error);
	bool ValidateExport(FString& Error);
	void Fail(const FString& Error);
	void BeginRestore();
	void Finish();
	void RestoreReferences();

	TWeakObjectPtr<UPCGComponent> Component;
	TStrongObjectPtr<UPCGGraphInterface> OriginalParent;
	TStrongObjectPtr<UPCGGraphInstance> OriginalInstanceSnapshot; // Keeps UObject parameter overrides alive through GC.
	FPCGOverrideInstancedPropertyBag OriginalOverrides;
	TStrongObjectPtr<UPCGGraph> BatchGraph;
	TStrongObjectPtr<UPCGExportSyntheticPointCloudSettings> ExportSettings;
	UPCGNode* ExportNode = nullptr; // Owned by BatchGraph.
	TUniquePtr<FPCGContext> ExportContext;
	TSharedPtr<class IPCGElement, ESPMode::ThreadSafe> ExportElement;
	TArray<int32> Seeds;
	TArray<FSyntheticDatasetRun> Runs;
	FSyntheticDatasetRun PendingRun;
	FString OutputRoot;
	FString Failure;
	int32 OriginalSeed = 0;
	int32 SeedIndex = 0;
	bool bOriginalRegenerate = true;
	bool bGenerated = false;
	bool bCancelled = false;
	bool bIgnoreCancellation = false;
	bool bInspectionEnabled = false;
	bool bReferencesRestored = false;
	EPhase Phase = EPhase::Finished;
	double PhaseStarted = 0;
	FDelegateHandle GeneratedHandle;
	FDelegateHandle CancelledHandle;
	FTSTicker::FDelegateHandle TickerHandle;
};

void RegisterSyntheticDatasetBatchCommands();
void UnregisterSyntheticDatasetBatchCommands();

#endif
