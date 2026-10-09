#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Editor/SyntheticDatasetBatch.h"
#include "PCG/PCGExportSyntheticPointCloudSettings.h"
#include "PCGComponent.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Tests/AutomationEditorCommon.h"

namespace SyntheticDatasetBatchTests
{
	class FRunBatch : public IAutomationLatentCommand
	{
	public:
		FRunBatch(FAutomationTestBase* InTest, bool bInInterrupt, UPCGExportSyntheticPointCloudSettings* InSettings, bool bInEnabled)
			: Test(InTest), bInterrupt(bInInterrupt), AssetSettings(InSettings), bOriginalEnabled(bInEnabled) {}
		virtual ~FRunBatch() override
		{
			if (Batch && !Batch->IsFinished()) { Batch->Shutdown(); }
			if (Component.IsValid()) { Component->OnPCGGraphGeneratedDelegate.Remove(InterruptHandle); }
			AssetSettings->bExportEnabled = bOriginalEnabled;
		}
		virtual bool Update() override
		{
			if (FPlatformTime::Seconds() - Started > 1000.0)
			{
				Test->AddError(TEXT("Batch test did not finish within its overall timeout.")); return true;
			}
			if (!Component.IsValid())
			{
				UPCGGraph* SceneGraph = LoadObject<UPCGGraph>(nullptr, TEXT("/Game/DIPLOMA.DIPLOMA"));
				for (TActorIterator<AActor> Actor(GEditor->GetEditorWorldContext().World()); Actor; ++Actor)
				{
					if (Actor->GetActorLabel() != TEXT("DIPLOMA")) { continue; }
					TArray<UPCGComponent*> Components;
					Actor->GetComponents(Components);
					for (UPCGComponent* Candidate : Components)
					{
						Test->AddInfo(FString::Printf(TEXT("Scene candidate: %s; graph=%s"), *Candidate->GetName(), *GetPathNameSafe(Candidate->GetGraph())));
						if (!Candidate->IsLocalComponent() && Candidate->GetGraph() == SceneGraph) { Component = Candidate; }
					}
				}
				if (!Test->TestTrue(TEXT("Main scene PCG component found"), Component.IsValid())) { return true; }
			}
			if (!Batch)
			{
				if (Component->IsGenerating() || Component->IsRefreshInProgress()) { return false; }
				OriginalSeed = Component->Seed;
				OriginalParent = Component->GetGraphInstance()->Graph;
				OriginalRegenerate = Component->bRegenerateInEditor;
				OriginalParameters = Component->GetGraphInstance()->ParametersOverrides.Parameters;
				Root = TEXT("SyntheticDataset/Runs/validation_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
				if (bInterrupt)
				{
					// Simulate an external edit after generation but before export.
					// The controller must stop, not label the previous scene with a new seed.
					InterruptHandle = Component->OnPCGGraphGeneratedDelegate.AddLambda([](UPCGComponent* Source)
					{
						if (Source->Seed == 66) { Source->Seed = 67; }
					});
					Test->AddExpectedError(TEXT("Seed 66: component seed changed after generation."), EAutomationExpectedErrorFlags::Contains, 1);
				}
				FString Error;
				if (bInterrupt) { Batch = FSyntheticDatasetBatch::Start(Component.Get(), {66, 70, 82}, Root, Error); }
				else
				{
					const FString Command = TEXT("TestProject.GenerateSyntheticDataset DIPLOMA ") + Root;
					Test->TestTrue(TEXT("Editor console command dispatched"), IConsoleManager::Get().ProcessUserConsoleInput(*Command, *GLog, Component->GetWorld()));
					Batch = FSyntheticDatasetBatch::GetActive();
				}
				if (!Test->TestTrue(TEXT("Batch starts: ") + Error, Batch.IsValid())) { return true; }
				FString ConcurrentError;
				Test->TestFalse(TEXT("Concurrent batch rejected"), FSyntheticDatasetBatch::Start(Component.Get(), {66}, Root + TEXT("_other"), ConcurrentError).IsValid());
				return false;
			}
			if (!Batch->IsFinished()) { return false; }
			Test->TestEqual(TEXT("Original seed restored"), Component->Seed, OriginalSeed);
			Test->TestTrue(TEXT("Original graph reference restored without a transient parent"), Component->GetGraphInstance()->Graph == OriginalParent.Get());
			Test->TestEqual(TEXT("Editor regeneration flag restored"), Component->bRegenerateInEditor, OriginalRegenerate);
			Test->TestTrue(TEXT("Original parameter values restored"), Component->GetGraphInstance()->ParametersOverrides.Parameters.Identical(&OriginalParameters, 0));
			if (bInterrupt)
			{
				Test->TestFalse(TEXT("External seed edit fails the batch"), Batch->Succeeded());
				Test->TestTrue(TEXT("Failure identifies requested seed"), Batch->GetFailure().Contains(TEXT("Seed 66")));
				Test->TestEqual(TEXT("No incorrect run accepted"), Batch->GetRuns().Num(), 0);
				for (int32 Seed : {66, 70, 82})
				{
					Test->TestFalse(TEXT("No output exported or later seed attempted"), IFileManager::Get().DirectoryExists(*FPaths::Combine(FPaths::ProjectSavedDir(), Root, FString::Printf(TEXT("seed_%d"), Seed))));
				}
			}
			else
			{
				if (!Test->TestTrue(TEXT("Batch succeeds: ") + Batch->GetFailure(), Batch->Succeeded())) { return true; }
				if (!Test->TestEqual(TEXT("Exactly three sequential runs"), Batch->GetRuns().Num(), 3)) { return true; }
				const TArray<int32> Seeds = {66, 70, 82};
				const TArray<int32> Counts = {55, 51, 56};
				for (int32 Index = 0; Index < 3; ++Index)
				{
					const auto& Run = Batch->GetRuns()[Index];
					Test->TestEqual(TEXT("Requested seed order"), Run.Seed, Seeds[Index]);
					Test->TestEqual(TEXT("Confirmed brick_stack baseline"), Run.BrickCount, Counts[Index]);
					Test->TestEqual(TEXT("Actual brick transforms captured"), Run.BrickTransforms.Num(), Counts[Index]);
					Test->AddInfo(FString::Printf(TEXT("Seed %d: brick=%d, unique=%d, verified scene_seed and TXT/PLY; %s"), Run.Seed, Run.BrickCount, Run.BrickCount, *Run.Directory));
				}
				FString Error;
				Test->TestFalse(TEXT("Existing runs cannot satisfy a new batch through stale files"), FSyntheticDatasetBatch::Start(Component.Get(), {66, 70, 82}, Root, Error).IsValid());
				Test->TestTrue(TEXT("Existing output rejection explained"), Error.Contains(TEXT("existing run directory")));
				Test->AddInfo(TEXT("Batch output root: ") + FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), Root)));
			}
			return true;
		}
	private:
		FAutomationTestBase* Test;
		bool bInterrupt;
		TStrongObjectPtr<UPCGExportSyntheticPointCloudSettings> AssetSettings;
		bool bOriginalEnabled;
		TWeakObjectPtr<UPCGComponent> Component;
		TSharedPtr<FSyntheticDatasetBatch> Batch;
		TWeakObjectPtr<UPCGGraphInterface> OriginalParent;
		FInstancedPropertyBag OriginalParameters;
		FDelegateHandle InterruptHandle;
		FString Root;
		int32 OriginalSeed = 0;
		bool OriginalRegenerate = true;
		double Started = FPlatformTime::Seconds();
	};
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FSyntheticDatasetBatchTest, "TestProject.GroundTruth.Batch", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

void FSyntheticDatasetBatchTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	Names = {TEXT("ThreeSeeds"), TEXT("ExternalSeedEditStopsBatch")};
	Commands = Names;
}

bool FSyntheticDatasetBatchTest::RunTest(const FString& Parameters)
{
	UPCGGraph* Graph = LoadObject<UPCGGraph>(nullptr, TEXT("/Game/DIPLOMA.DIPLOMA"));
	if (!TestNotNull(TEXT("DIPLOMA graph exists"), Graph)) { return false; }
	UPCGExportSyntheticPointCloudSettings* Settings = nullptr;
	for (UPCGNode* Node : Graph->GetNodes())
	{
		if (auto* Candidate = Cast<UPCGExportSyntheticPointCloudSettings>(Node->GetSettings())) { Settings = Candidate; }
	}
	if (!TestNotNull(TEXT("Graph exporter exists"), Settings)) { return false; }
	const bool bOriginalEnabled = Settings->bExportEnabled;
	Settings->bExportEnabled = false; // Avoid unrelated map-load export to the manual directory.
	FAutomationEditorCommonUtils::LoadMap(TEXT("/Game/DIPLOMAS"));
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<SyntheticDatasetBatchTests::FRunBatch>(this, Parameters == TEXT("ExternalSeedEditStopsBatch"), Settings, bOriginalEnabled));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSyntheticDatasetBatchInvalidSourceTest, "TestProject.GroundTruth.Export.BatchRejectsMissingComponent", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSyntheticDatasetBatchInvalidSourceTest::RunTest(const FString& Parameters)
{
	FString Error;
	TestNotNull(TEXT("Editor generate command registered"), IConsoleManager::Get().FindConsoleObject(TEXT("TestProject.GenerateSyntheticDataset")));
	TestNotNull(TEXT("Editor cancel command registered"), IConsoleManager::Get().FindConsoleObject(TEXT("TestProject.CancelSyntheticDatasetBatch")));
	TestFalse(TEXT("No arbitrary component selected for a missing source"), FSyntheticDatasetBatch::Start(nullptr, {66, 70, 82}, TEXT("SyntheticDataset/Runs"), Error).IsValid());
	TestTrue(TEXT("Missing component explained"), Error.Contains(TEXT("PCG component")));
	return true;
}

#endif
