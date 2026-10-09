// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "PCG/PCGExportSyntheticPointCloudSettings.h"
#include "PCGComponent.h"
#include "PCGContext.h"
#include "PCGGraph.h"
#include "PCGGraphExecutionInspection.h"
#include "PCGPin.h"
#include "Data/PCGBasePointData.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Elements/PCGCreateAttribute.h"
#include "EngineUtils.h"
#include "Metadata/Accessors/PCGAttributeAccessorHelpers.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/StrongObjectPtr.h"

namespace GroundTruthSceneTests
{
	UPCGNode* FindNode(UPCGGraph* Graph, const TCHAR* Name)
	{
		for (UPCGNode* Node : Graph->GetNodes())
		{
			if (Node->GetFName() == Name) { return Node; }
		}
		return nullptr;
	}

	struct FCapture
	{
		TArray<FTransform> BrickTransforms;
		TMap<int32, int32> ExportClasses;
		FPCGDataCollection ExportData;
	};

	class FValidateScene : public IAutomationLatentCommand
	{
	public:
		FValidateScene(FAutomationTestBase* InTest, const FString& InActorLabel) : Test(InTest), ActorLabel(InActorLabel) {}
		virtual ~FValidateScene() override
		{
			if (bInspecting && Component.IsValid())
			{
				if (Component->IsGenerating()) { Component->CancelGeneration(); }
				Component->GetExecutionState().GetInspection().DisableInspection();
				Component->SetGraphLocal(OriginalGraph.Get());
				Component->bRegenerateInEditor = bOriginalRegenerateInEditor;
			}
		}

		virtual bool Update() override
		{
			if (FPlatformTime::Seconds() - Started > 240.0)
			{
				Test->AddError(TEXT("Scene generation did not finish within 240 seconds; no runtime count was verified."));
				return true;
			}
			if (!Component.IsValid())
			{
				UPCGGraph* Asset = LoadObject<UPCGGraph>(nullptr, TEXT("/Game/DIPLOMA.DIPLOMA"));
				UWorld* World = GEditor->GetEditorWorldContext().World();
				for (TActorIterator<AActor> Actor(World); Actor; ++Actor)
				{
					if (Actor->GetActorLabel() != ActorLabel) { continue; }
					TArray<UPCGComponent*> Components;
					Actor->GetComponents(Components);
					for (UPCGComponent* Candidate : Components)
					{
						if (!Candidate->IsLocalComponent() && Candidate->GetGraph() == Asset)
						{
							if (Component.IsValid())
							{
								Test->AddError(TEXT("Multiple DIPLOMA components found; select the intended scene explicitly."));
								return true;
							}
							Component = Candidate;
						}
					}
				}
				if (!Component.IsValid())
				{
					Test->AddError(FString::Printf(TEXT("No DIPLOMA PCG component found on actor %s in /Game/DIPLOMAS."), *ActorLabel));
					return true;
				}
				// SetGraphLocal mutates the component's existing graph instance.
				// Keep a separate copy, including its parameter overrides, for restoration.
				OriginalGraph.Reset(DuplicateObject<UPCGGraphInstance>(Component->GetGraphInstance(), GetTransientPackage()));
				// Prevent editor auto-refresh from racing our explicit generations.
				// This flag is restored and the map is never saved.
				bOriginalRegenerateInEditor = Component->bRegenerateInEditor;
				Component->bRegenerateInEditor = false;
				ComponentSeed = Component->Seed;
				BeforeGraph.Reset(DuplicateObject<UPCGGraph>(Asset, GetTransientPackage()));
				AfterGraph.Reset(DuplicateObject<UPCGGraph>(Asset, GetTransientPackage()));
				CastChecked<UPCGAddAttributeSettings>(FindNode(BeforeGraph.Get(), TEXT("AddAttribute_15"))->GetSettings())->AttributeTypes.Int32Value = 0;
				BeforeGraph->RemoveEdge(FindNode(BeforeGraph.Get(), TEXT("StaticMeshSpawner_12")), TEXT("Out"), FindNode(BeforeGraph.Get(), TEXT("MergePoints_0")), TEXT("In"));
				Component->GetExecutionState().GetInspection().EnableInspection();
				bInspecting = true;
			}
			if (Component->IsGenerating() || Component->IsRefreshInProgress()) { return false; }
			if (Phase == 0)
			{
				Component->SetGraphLocal(BeforeGraph.Get());
				Component->GenerateLocal(true);
				Phase = 1;
				return false;
			}
			if (Phase == 1)
			{
				if (!Capture(BeforeGraph.Get(), Before)) { return true; }
				Component->SetGraphLocal(AfterGraph.Get());
				Component->GenerateLocal(true);
				Phase = 2;
				return false;
			}
			FCapture After;
			if (!Capture(AfterGraph.Get(), After)) { return true; }
			Test->TestEqual(TEXT("Final brick_stack count unchanged"), After.BrickTransforms.Num(), Before.BrickTransforms.Num());
			if (ActorLabel == TEXT("DIPLOMA"))
			{
				Test->TestTrue(TEXT("Primary scene contains bricks to validate"), !After.BrickTransforms.IsEmpty());
			}
			bool bSameTransforms = Before.BrickTransforms.Num() == After.BrickTransforms.Num();
			for (int32 Index = 0; bSameTransforms && Index < Before.BrickTransforms.Num(); ++Index)
			{
				bSameTransforms = Before.BrickTransforms[Index].Equals(After.BrickTransforms[Index], 0.0);
			}
			Test->TestTrue(TEXT("Brick placement transforms unchanged"), bSameTransforms);
			Test->TestEqual(TEXT("Exporter receives every final brick"), After.ExportClasses.FindRef(3), After.BrickTransforms.Num());
			Test->TestEqual(TEXT("Brick class absent from old export"), Before.ExportClasses.FindRef(3), 0);
			for (const auto& Pair : Before.ExportClasses)
			{
				Test->TestEqual(FString::Printf(TEXT("Other ClassId %d count unchanged"), Pair.Key), After.ExportClasses.FindRef(Pair.Key), Pair.Value);
			}
			Test->TestEqual(TEXT("Component seed unchanged"), Component->Seed, ComponentSeed);
			Test->AddInfo(FString::Printf(TEXT("Actor=%s; current component seed=%d; brick_stack before=%d, after=%d, exporter=%d. No seed parameters were changed and historical 55/51/56 values were not used."), *ActorLabel, ComponentSeed, Before.BrickTransforms.Num(), After.BrickTransforms.Num(), After.ExportClasses.FindRef(3)));
			if (After.ExportClasses.IsEmpty())
			{
				Test->AddInfo(TEXT("Verified empty object stream; no export requested, preserving the existing empty-scene behavior."));
				return true;
			}

			TStrongObjectPtr<UPCGExportSyntheticPointCloudSettings> Settings(NewObject<UPCGExportSyntheticPointCloudSettings>());
			Settings->bExportEnabled = true;
			Settings->TargetPointCount = 128;
			Settings->OutputSubdirectory = FPaths::Combine(TEXT("Automation/GroundTruthScene"), ActorLabel, FGuid::NewGuid().ToString(EGuidFormats::Digits));
			FPCGTaggedData& SettingsInput = After.ExportData.TaggedData.Emplace_GetRef();
			SettingsInput.Data = Settings.Get();
			SettingsInput.Pin = TEXT("Settings");
			After.ExportData.ComputeCrcs(false);
			const FPCGElementPtr Element = Settings->GetElement();
			TUniquePtr<FPCGContext> Context(Element->Initialize(FPCGInitializeElementParams(&After.ExportData, Component.Get(), nullptr)));
			Context->InitializeSettings();
			Context->AsyncState.NumAvailableTasks = 1;
			while (!Element->Execute(Context.Get())) {}
			FString Json;
			TSharedPtr<FJsonObject> Summary;
			const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), Settings->OutputSubdirectory);
			const bool bReadSummary = FFileHelper::LoadFileToString(Json, *FPaths::Combine(Directory, TEXT("summary.json"))) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Summary);
			if (Test->TestTrue(TEXT("Real scene export writes summary"), bReadSummary))
			{
				const auto Brick = Summary->GetObjectField(TEXT("classes"))->GetObjectField(TEXT("brick"));
				Test->TestEqual(TEXT("Real summary matches final stream"), Brick->GetIntegerField(TEXT("instance_count")), After.BrickTransforms.Num());
				Test->TestEqual(TEXT("Real unique IDs match final stream"), Brick->GetIntegerField(TEXT("unique_instance_count")), After.BrickTransforms.Num());
				Test->TestTrue(TEXT("Real PLY written"), FPaths::FileExists(FPaths::Combine(Directory, TEXT("scene_0001_points.ply"))));
				Test->TestTrue(TEXT("Real TXT written"), FPaths::FileExists(FPaths::Combine(Directory, TEXT("scene_0001_points.txt"))));
			}
			return true;
		}

	private:
		const FPCGDataCollection* PinData(UPCGGraph* Graph, const TCHAR* NodeName)
		{
			const UPCGNode* Node = FindNode(Graph, NodeName);
			const auto Stacks = Component->GetExecutionState().GetInspection().GetExecutedNodeStacks();
			const auto* Notifications = Stacks.Find(Node);
			if (Notifications && !Notifications->IsEmpty())
			{
				bool bProducedData = false;
				for (const auto& Notification : *Notifications)
				{
					bProducedData |= Component->GetExecutionState().GetInspection().HasNodeProducedData(Node, Notification.Stack);
					FPCGStack Stack = Notification.Stack;
					Stack.GetStackFramesMutable().Emplace(Node);
					Stack.GetStackFramesMutable().Emplace(Node->GetOutputPin(TEXT("Out")));
					if (const FPCGDataCollection* Data = Component->GetExecutionState().GetInspection().GetInspectionData(Stack)) { return Data; }
				}
				// UE omits empty collections from the inspection cache. Accept zero
				// only with positive execution evidence and no produced output data.
				if (!bProducedData)
				{
					static const FPCGDataCollection Empty;
					Test->AddInfo(FString::Printf(TEXT("%s executed with empty output."), NodeName));
					return &Empty;
				}
			}
			Test->AddError(FString::Printf(TEXT("No inspection data for %s; runtime count cannot be established."), NodeName));
			return nullptr;
		}

		bool Capture(UPCGGraph* Graph, FCapture& Out)
		{
			// The immediate upstream CPU pin is the exact final stream consumed by
			// StaticMeshSpawner_12, including when that spawner executes on the GPU.
			const FPCGDataCollection* Brick = PinData(Graph, TEXT("AddAttribute_16"));
			const FPCGDataCollection* Export = PinData(Graph, TEXT("AssignDatasetInstanceIds_0"));
			if (!Brick || !Export) { return false; }
			for (const FPCGTaggedData& Item : Brick->TaggedData)
			{
				if (const UPCGBasePointData* Points = Cast<UPCGBasePointData>(Item.Data)) { Out.BrickTransforms.Append(Points->GetTransformsCopy()); }
			}
			Out.ExportData = *Export;
			for (FPCGTaggedData& Item : Out.ExportData.TaggedData)
			{
				Item.Pin = TEXT("In");
				const UPCGBasePointData* Points = Cast<UPCGBasePointData>(Item.Data);
				if (!Points) { continue; }
				TArray<int32> Classes;
				const auto Selector = FPCGAttributePropertySelector::CreateAttributeSelector(TEXT("ClassId"));
				const auto Accessor = PCGAttributeAccessorHelpers::CreateConstAccessor(Points, Selector);
				const auto Keys = PCGAttributeAccessorHelpers::CreateConstKeys(Points, Selector);
				if (!Accessor || !Keys || !PCGAttributeAccessorHelpers::ExtractAllValues(Accessor.Get(), Keys.Get(), Classes, EPCGAttributeAccessorFlags::StrictType)) { return false; }
				for (int32 Class : Classes) { Out.ExportClasses.FindOrAdd(Class)++; }
			}
			return true;
		}

		FAutomationTestBase* Test;
		FString ActorLabel;
		double Started = FPlatformTime::Seconds();
		int32 Phase = 0;
		int32 ComponentSeed = 0;
		TWeakObjectPtr<UPCGComponent> Component;
		bool bInspecting = false;
		bool bOriginalRegenerateInEditor = true;
		TStrongObjectPtr<UPCGGraphInstance> OriginalGraph;
		TStrongObjectPtr<UPCGGraph> BeforeGraph;
		TStrongObjectPtr<UPCGGraph> AfterGraph;
		FCapture Before;
	};
}

// Opt-in: loads the current map, uses its current seed and transient graph copies,
// and never saves the map or changes persisted placement settings.
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FGroundTruthSceneTest, "TestProject.GroundTruth.Scene.PreservePlacement", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

void FGroundTruthSceneTest::GetTests(TArray<FString>& OutNames, TArray<FString>& OutCommands) const
{
	// The current map contains two active volumes using the same graph. Validate
	// both explicitly, without selecting a component or combining scenes silently.
	OutNames = {TEXT("DIPLOMA"), TEXT("DIPLOMA2")};
	OutCommands = OutNames;
}

bool FGroundTruthSceneTest::RunTest(const FString& Parameters)
{
	FAutomationEditorCommonUtils::LoadMap(TEXT("/Game/DIPLOMAS"));
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<GroundTruthSceneTests::FValidateScene>(this, Parameters));
	return true;
}

#endif
