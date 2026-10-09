// Copyright Epic Games, Inc. All Rights Reserved.

#include "TestProject.h"
#include "Modules/ModuleManager.h"

#if WITH_EDITOR
#include "Editor/SyntheticDatasetBatch.h"
#endif

class FTestProjectModule : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override
	{
#if WITH_EDITOR
		RegisterSyntheticDatasetBatchCommands();
#endif
	}
	virtual void ShutdownModule() override
	{
#if WITH_EDITOR
		UnregisterSyntheticDatasetBatchCommands();
#endif
	}
};

IMPLEMENT_PRIMARY_GAME_MODULE(FTestProjectModule, TestProject, "TestProject");
