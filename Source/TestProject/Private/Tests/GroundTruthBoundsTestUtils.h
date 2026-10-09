#pragma once

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"

namespace GroundTruthBoundsTests
{
	struct FRow
	{
		int32 InstanceId = 0;
		FVector SizeCm = FVector::ZeroVector;
		double VolumeCm3 = 0.0;
	};

	inline bool Near(double A, double B)
	{
		return FMath::IsFinite(A) && FMath::IsFinite(B) && FMath::Abs(A - B) <= 1.e-9 * FMath::Max(1.0, FMath::Max(FMath::Abs(A), FMath::Abs(B)));
	}

	// Validate persisted object metadata independently of the exporter's in-memory statistics.
	inline bool Validate(FAutomationTestBase& Test, const FString& Directory, TOptional<int32> Seed,
		int32 ExpectedCount, int32 ExpectedUniqueCount, TArray<FRow>& Rows)
	{
		Rows.Reset();
		FString Csv, Json;
		TSharedPtr<FJsonObject> Summary;
		if (!Test.TestTrue(TEXT("Instance CSV readable"), FFileHelper::LoadFileToString(Csv, *FPaths::Combine(Directory, TEXT("instances.csv")))) ||
			!Test.TestTrue(TEXT("Volume summary readable"), FFileHelper::LoadFileToString(Json, *FPaths::Combine(Directory, TEXT("summary.json"))) &&
				FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Summary) && Summary.IsValid())) { return false; }
		TArray<FString> Lines;
		Csv.ParseIntoArrayLines(Lines);
		if (!Test.TestTrue(TEXT("CSV contains header"), !Lines.IsEmpty())) { return false; }
		Test.TestEqual(TEXT("Object CSV schema"), Lines[0], FString(TEXT("SceneSeed,ClassId,InstanceId,BoundsXcm,BoundsYcm,BoundsZcm,BBoxVolumeCm3")));
		Test.TestEqual(TEXT("Exactly one row per final brick, independent of surface samples"), Lines.Num() - 1, ExpectedCount);
		TSet<int32> Ids;
		double Total = 0.0, Min = TNumericLimits<double>::Max(), Max = 0.0;
		for (int32 Index = 1; Index < Lines.Num(); ++Index)
		{
			TArray<FString> Fields;
			Lines[Index].ParseIntoArray(Fields, TEXT(","), false);
			if (!Test.TestEqual(TEXT("Seven CSV fields"), Fields.Num(), 7)) { return false; }
			Test.TestEqual(TEXT("CSV scene seed from generating context, empty if unknown"), Fields[0], Seed.IsSet() ? FString::FromInt(Seed.GetValue()) : FString());
			Test.TestEqual(TEXT("Only brick class exported in object CSV"), Fields[1], FString(TEXT("3")));
			FRow Row;
			if (!Test.TestTrue(TEXT("Numeric instance metadata"), LexTryParseString(Row.InstanceId, *Fields[2]) &&
				LexTryParseString(Row.SizeCm.X, *Fields[3]) && LexTryParseString(Row.SizeCm.Y, *Fields[4]) &&
				LexTryParseString(Row.SizeCm.Z, *Fields[5]) && LexTryParseString(Row.VolumeCm3, *Fields[6]))) { return false; }
			Test.TestTrue(TEXT("Dimensions and volume finite and positive"), FMath::IsFinite(Row.SizeCm.X) && FMath::IsFinite(Row.SizeCm.Y) &&
				FMath::IsFinite(Row.SizeCm.Z) && Row.SizeCm.GetMin() > 0.0 && FMath::IsFinite(Row.VolumeCm3) && Row.VolumeCm3 > 0.0);
			Test.TestTrue(TEXT("Persisted volume equals X * Y * Z"), Near(Row.VolumeCm3, Row.SizeCm.X * Row.SizeCm.Y * Row.SizeCm.Z));
			Ids.Add(Row.InstanceId);
			Total += Row.VolumeCm3; Min = FMath::Min(Min, Row.VolumeCm3); Max = FMath::Max(Max, Row.VolumeCm3);
			Rows.Add(Row);
		}
		const auto Brick = Summary->GetObjectField(TEXT("classes"))->GetObjectField(TEXT("brick"));
		Test.TestEqual(TEXT("CSV row count equals summary brick count"), Rows.Num(), Brick->GetIntegerField(TEXT("instance_count")));
		Test.TestEqual(TEXT("CSV unique ID count"), Ids.Num(), ExpectedUniqueCount);
		Test.TestEqual(TEXT("CSV unique IDs equal summary unique count"), Ids.Num(), Brick->GetIntegerField(TEXT("unique_instance_count")));
		Test.TestEqual(TEXT("Explicit bounds definition"), Brick->GetStringField(TEXT("bbox_definition")), FString(TEXT("world_aabb_lod0_triangle_vertices")));
		Test.TestTrue(TEXT("Summary total equals CSV sum"), Near(Brick->GetNumberField(TEXT("total_bbox_volume_cm3")), Total));
		if (!Rows.IsEmpty())
		{
			Test.TestTrue(TEXT("Summary mean equals CSV mean"), Near(Brick->GetNumberField(TEXT("mean_bbox_volume_cm3")), Total / Rows.Num()));
			Test.TestTrue(TEXT("Summary min equals CSV min"), Near(Brick->GetNumberField(TEXT("min_bbox_volume_cm3")), Min));
			Test.TestTrue(TEXT("Summary max equals CSV max"), Near(Brick->GetNumberField(TEXT("max_bbox_volume_cm3")), Max));
		}
		else
		{
			for (const TCHAR* Field : {TEXT("mean_bbox_volume_cm3"), TEXT("min_bbox_volume_cm3"), TEXT("max_bbox_volume_cm3")})
			{
				Test.TestTrue(TEXT("Empty-class statistics are undefined/null"), Brick->Values[Field]->IsNull());
			}
		}
		return true;
	}
}
