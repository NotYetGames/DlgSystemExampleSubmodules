// Copyright 2017-2018 Csaba Molnar, Daniel Butum
#include "IO/DlgJsonParser.h"
#include "LogMacros.h"
#include "Object.h"
#include "FileHelper.h"
#include "Paths.h"
#include "UnrealType.h"
#include "EnumProperty.h"
#include "UObjectIterator.h"
#include "JsonUtilities.h"
#include "TextProperty.h"
#include "PropertyPortFlags.h"

DEFINE_LOG_CATEGORY(LogDlgJsonParser);

bool GetTextFromObject(const TSharedRef<FJsonObject>& Obj, FText& TextOut)
{
	// get the prioritized culture name list
	FCultureRef CurrentCulture = FInternationalization::Get().GetCurrentCulture();
	TArray<FString> CultureList = CurrentCulture->GetPrioritizedParentCultureNames();

	// try to follow the fall back chain that the engine uses
	FString TextString;
	for (const FString& CultureCode : CultureList)
	{
		if (Obj->TryGetStringField(CultureCode, TextString))
		{
			TextOut = FText::FromString(TextString);
			return true;
		}
	}

	// no luck, is this possibly an unrelated json object?
	return false;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void DlgJsonParser::InitializeParser(const FString& FilePath)
{
	if (!FFileHelper::LoadFileToString(JsonString, *FilePath))
	{
		UE_LOG(LogDlgJsonParser, Error, TEXT("Failed to load config file %s"), *FilePath);
		bIsValidFile = false;
	}
	else
	{
		FileName = FPaths::GetBaseFilename(FilePath, true);
		bIsValidFile = true;
	}

	// TODO check here if the JSON file is valid.
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void DlgJsonParser::ReadAllProperty( const UStruct* ReferenceClass, void* TargetObject, UObject* InDefaultObjectOuter)
{
	if (!IsValidFile())
	{
		return;
	}

	// TODO use DefaultObjectOuter;
	DefaultObjectOuter = InDefaultObjectOuter;
	bIsValidFile = JsonObjectStringToUStruct(ReferenceClass, TargetObject);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool DlgJsonParser::ConvertScalarJsonValueToUProperty(TSharedPtr<FJsonValue> JsonValue, UProperty* Property, void* OutValue)
{
	check(Property);
	check(OutValue);

	// Enum
	if (UEnumProperty* EnumProperty = Cast<UEnumProperty>(Property))
	{
		if (JsonValue->Type == EJson::String)
		{
			// see if we were passed a string for the enum
			const UEnum* Enum = EnumProperty->GetEnum();
			check(Enum);
			const FString StrValue = JsonValue->AsString();
			const int64 IntValue = Enum->GetValueByName(FName(*StrValue));
			if (IntValue == INDEX_NONE)
			{
				UE_LOG(LogDlgJsonParser,
					   Warning,
					   TEXT("JsonValueToUProperty - Unable import enum %s from string value %s for property %s"),
					   *Enum->CppType, *StrValue, *Property->GetNameCPP());
				return false;
			}
			EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(OutValue, IntValue);
		}
		else
		{
			// Numeric enum
			// AsNumber will log an error for completely inappropriate types (then give us a default)
			EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(OutValue, static_cast<int64>(JsonValue->AsNumber()));
		}

		return true;
	}

	// Numeric, int, float, possible enum
	if (UNumericProperty* NumericProperty = Cast<UNumericProperty>(Property))
	{
		if (NumericProperty->IsEnum() && JsonValue->Type == EJson::String)
		{
			// see if we were passed a string for the enum
			const UEnum* Enum = NumericProperty->GetIntPropertyEnum();
			check(Enum); // should be assured by IsEnum()
			const FString StrValue = JsonValue->AsString();
			const int64 IntValue = Enum->GetValueByName(FName(*StrValue));
			if (IntValue == INDEX_NONE)
			{
				UE_LOG(LogDlgJsonParser,
					   Warning,
					   TEXT("JsonValueToUProperty - Unable import enum %s from string value %s for property %s"),
					   *Enum->CppType, *StrValue, *Property->GetNameCPP());
				return false;
			}
			NumericProperty->SetIntPropertyValue(OutValue, IntValue);
		}
		else if (NumericProperty->IsFloatingPoint())
		{
			// AsNumber will log an error for completely inappropriate types (then give us a default)
			NumericProperty->SetFloatingPointPropertyValue(OutValue, JsonValue->AsNumber());
		}
		else if (NumericProperty->IsInteger())
		{
			if (JsonValue->Type == EJson::String)
			{
				// parse string -> int64 ourselves so we don't lose any precision going through AsNumber (aka double)
				NumericProperty->SetIntPropertyValue(OutValue, FCString::Atoi64(*JsonValue->AsString()));
			}
			else
			{
				// AsNumber will log an error for completely inappropriate types (then give us a default)
				NumericProperty->SetIntPropertyValue(OutValue, static_cast<int64>(JsonValue->AsNumber()));
			}
		}
		else
		{
			UE_LOG(LogDlgJsonParser,
				   Warning,
				   TEXT("JsonValueToUProperty - Unable to set numeric property type %s for property %s"),
				   *Property->GetClass()->GetName(), *Property->GetNameCPP());
			return false;
		}

		return true;
	}

	// Bool
	if (UBoolProperty* BoolProperty = Cast<UBoolProperty>(Property))
	{
		// AsBool will log an error for completely inappropriate types (then give us a default)
		BoolProperty->SetPropertyValue(OutValue, JsonValue->AsBool());
		return true;
	}

	// FString
	if (UStrProperty* StringProperty = Cast<UStrProperty>(Property))
	{
		// AsString will log an error for completely inappropriate types (then give us a default)
		StringProperty->SetPropertyValue(OutValue, JsonValue->AsString());
		return true;
	}

	// FText
	if (UTextProperty* TextProperty = Cast<UTextProperty>(Property))
	{
		if (JsonValue->Type == EJson::String)
		{
			// assume this string is already localized, so import as invariant
			TextProperty->SetPropertyValue(OutValue, FText::FromString(JsonValue->AsString()));
		}
		else if (JsonValue->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> Obj = JsonValue->AsObject();
			check(Obj.IsValid()); // should not fail if Type == EJson::Object

			// import the subvalue as a culture invariant string
			FText Text;
			if (!GetTextFromObject(Obj.ToSharedRef(), Text))
			{
				UE_LOG(LogDlgJsonParser,
					   Warning,
					   TEXT("JsonValueToUProperty - Attempted to import FText from JSON object with invalid keys for property %s"),
					   *Property->GetNameCPP());
				return false;
			}
			TextProperty->SetPropertyValue(OutValue, Text);
		}
		else
		{
			UE_LOG(LogDlgJsonParser,
				   Warning,
				   TEXT("JsonValueToUProperty - Attempted to import FText from JSON that was neither string nor object for property %s"),
				   *Property->GetNameCPP());
			return false;
		}

		return true;
	}

	// TArray
	if (UArrayProperty* ArrayProperty = Cast<UArrayProperty>(Property))
	{
		if (JsonValue->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>> ArrayValue = JsonValue->AsArray();
			const int32 ArrayNum = ArrayValue.Num();

			// make the output array size match
			FScriptArrayHelper Helper(ArrayProperty, OutValue);
			Helper.Resize(ArrayNum);

			// set the property values
			for (int32 Index = 0; Index < ArrayNum; Index++)
			{
				const TSharedPtr<FJsonValue>& ArrayValueItem = ArrayValue[Index];
				if (ArrayValueItem.IsValid() && !ArrayValueItem->IsNull())
				{
					if (!JsonValueToUProperty(ArrayValueItem, ArrayProperty->Inner, Helper.GetRawPtr(Index)))
					{
						UE_LOG(LogDlgJsonParser,
							   Warning,
							   TEXT("JsonValueToUProperty - Unable to deserialize array element [%d] for property %s"),
							   Index, *Property->GetNameCPP());
						return false;
					}
				}
			}

			return true;
		}
		else
		{
			UE_LOG(LogDlgJsonParser,
				   Warning,
				   TEXT("JsonValueToUProperty - Attempted to import TArray from non-array JSON key for property %s"),
				   *Property->GetNameCPP());
			return false;
		}
	}

	// Set
	if (USetProperty* SetProperty = Cast<USetProperty>(Property))
	{
		if (JsonValue->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>> ArrayValue = JsonValue->AsArray();
			const int32 ArrayNum = ArrayValue.Num();

			FScriptSetHelper Helper(SetProperty, OutValue);

			// set the property values
			for (int32 Index = 0; Index < ArrayNum; ++Index)
			{
				const TSharedPtr<FJsonValue>& ArrayValueItem = ArrayValue[Index];
				if (ArrayValueItem.IsValid() && !ArrayValueItem->IsNull())
				{
					const int32 NewIndex = Helper.AddDefaultValue_Invalid_NeedsRehash();
					if (!JsonValueToUProperty(ArrayValueItem, SetProperty->ElementProp, Helper.GetElementPtr(NewIndex)))
					{
						UE_LOG(LogDlgJsonParser,
							   Error,
							   TEXT("JsonValueToUProperty - Unable to deserialize set element [%d] for property %s"),
							   Index,
							   *Property->GetNameCPP());
						return false;
					}
				}
			}

			Helper.Rehash();
			return true;
		}
		else
		{
			UE_LOG(LogDlgJsonParser,
				   Error,
				   TEXT("JsonValueToUProperty - Attempted to import TSet from non-array JSON key for property %s"),
				   *Property->GetNameCPP());
			return false;
		}
	}

	// TMap
	if (UMapProperty* MapProperty = Cast<UMapProperty>(Property))
	{
		if (JsonValue->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> ObjectValue = JsonValue->AsObject();
			FScriptMapHelper Helper(MapProperty, OutValue);

			// set the property values
			for (const auto& Entry : ObjectValue->Values)
			{
				if (Entry.Value.IsValid() && !Entry.Value->IsNull())
				{
					const int32 NewIndex = Helper.AddDefaultValue_Invalid_NeedsRehash();
					const TSharedPtr<FJsonValueString> TempKeyValue = MakeShareable(new FJsonValueString(Entry.Key));

					// Add Key and Value
					const bool bKeySuccess = JsonValueToUProperty(TempKeyValue, MapProperty->KeyProp, Helper.GetKeyPtr(NewIndex));
					const bool bValueSuccess = JsonValueToUProperty(Entry.Value, MapProperty->ValueProp, Helper.GetValuePtr(NewIndex));

					if (!bKeySuccess || !bValueSuccess)
					{
						UE_LOG(LogDlgJsonParser,
							   Error,
							   TEXT("JsonValueToUProperty - Unable to deserialize map element [key: %s] for property %s"),
							   *Entry.Key, *Property->GetNameCPP());
						return false;
					}
				}
			}

			Helper.Rehash();
			return true;
		}
		else
		{
			UE_LOG(LogDlgJsonParser,
				   Error,
				   TEXT("JsonValueToUProperty - Attempted to import TMap from non-object JSON key for property %s"),
				   *Property->GetNameCPP());
			return false;
		}
	}

	// UStruct
	if (UStructProperty* StructProperty = Cast<UStructProperty>(Property))
	{
		static const FName NAME_DateTime(TEXT("DateTime"));
		static const FName NAME_Color(TEXT("Color"));
		static const FName NAME_LinearColor(TEXT("LinearColor"));

		// Default struct export
		if (JsonValue->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> Obj = JsonValue->AsObject();
			check(Obj.IsValid()); // should not fail if Type == EJson::Object
			if (!JsonObjectToUStruct(Obj.ToSharedRef(), StructProperty->Struct, OutValue))
			{
				UE_LOG(LogDlgJsonParser,
					   Error,
					   TEXT("JsonValueToUProperty - JsonObjectToUStruct failed for property %s"),
					   *Property->GetNameCPP());
				return false;
			}
		}

		// Handle some structs that are exported to string in a special way
		else if (JsonValue->Type == EJson::String && StructProperty->Struct->GetFName() == NAME_LinearColor)
		{
			const FString ColorString = JsonValue->AsString();
			const FColor IntermediateColor = FColor::FromHex(ColorString);
			FLinearColor& ColorOut = *static_cast<FLinearColor*>(OutValue);
			ColorOut = IntermediateColor;
		}
		else if (JsonValue->Type == EJson::String && StructProperty->Struct->GetFName() == NAME_Color)
		{
			const FString ColorString = JsonValue->AsString();
			FColor& ColorOut = *static_cast<FColor*>(OutValue);
			ColorOut = FColor::FromHex(ColorString);
		}
		else if (JsonValue->Type == EJson::String && StructProperty->Struct->GetFName() == NAME_DateTime)
		{
			const FString DateString = JsonValue->AsString();
			FDateTime& DateTimeOut = *static_cast<FDateTime*>(OutValue);
			if (DateString == TEXT("min"))
			{
				// min representable value for our date struct. Actual date may vary by platform (this is used for sorting)
				DateTimeOut = FDateTime::MinValue();
			}
			else if (DateString == TEXT("max"))
			{
				// max representable value for our date struct. Actual date may vary by platform (this is used for sorting)
				DateTimeOut = FDateTime::MaxValue();
			}
			else if (DateString == TEXT("now"))
			{
				// this value's not really meaningful from json serialization (since we don't know timezone) but handle it anyway since we're handling the other keywords
				DateTimeOut = FDateTime::UtcNow();
			}
			else if (FDateTime::ParseIso8601(*DateString, DateTimeOut))
			{
				// ok
			}
			else if (FDateTime::Parse(DateString, DateTimeOut))
			{
				// ok
			}
			else
			{
				UE_LOG(LogDlgJsonParser,
					   Error,
					   TEXT("JsonValueToUProperty - Unable to import FDateTime for property %s"),
					   *Property->GetNameCPP());
				return false;
			}
		}
		else if (JsonValue->Type == EJson::String &&
				 StructProperty->Struct->GetCppStructOps() &&
				 StructProperty->Struct->GetCppStructOps()->HasImportTextItem())
		{
			UScriptStruct::ICppStructOps* TheCppStructOps = StructProperty->Struct->GetCppStructOps();

			const FString ImportTextString = JsonValue->AsString();
			const TCHAR* ImportTextPtr = *ImportTextString;
			if (!TheCppStructOps->ImportTextItem(ImportTextPtr, OutValue, PPF_None, nullptr, static_cast<FOutputDevice*>(GWarn)))
			{
				// Fall back to trying the tagged property approach if custom ImportTextItem couldn't get it done
				Property->ImportText(ImportTextPtr, OutValue, PPF_None, nullptr);
			}
		}
		else if (JsonValue->Type == EJson::String)
		{
			const FString ImportTextString = JsonValue->AsString();
			const TCHAR* ImportTextPtr = *ImportTextString;
			Property->ImportText(ImportTextPtr, OutValue, PPF_None, nullptr);
		}
		else
		{
			UE_LOG(LogDlgJsonParser,
				   Error,
				   TEXT("JsonValueToUProperty - Attempted to import UStruct from non-object JSON key for property %s"),
				   *Property->GetNameCPP());
			return false;
		}

		return true;
	}

	// UObject
	if (UObjectProperty* ObjectProperty = Cast<UObjectProperty>(Property))
	{
		// NOTE: The Value here should be a pointer to a pointer
		if (static_cast<UObject*>(OutValue) == nullptr)
		{
			UE_LOG(LogDlgJsonParser,
				   Warning,
				   TEXT("PropertyName = `%s` Is a UObjectProperty but can't convert Value to an UObject..."),
				   *Property->GetName());
			return false;
		}

		// Because the UObjects are pointers, we must deference it. So instead of it being a void** we want it to be a void*
		auto* ObjectPtrPtr = static_cast<UObject**>(ObjectProperty->ContainerPtrToValuePtr<void>(OutValue, 0));
		if (ObjectPtrPtr == nullptr)
		{
			UE_LOG(LogDlgJsonParser,
				   Warning,
				   TEXT("PropertyName = `%s` Is a UObjectProperty but can't get non null ContainerPtrToValuePtr from it's StructObject"),
				   *Property->GetName());
			return false;
		}
		const UClass* ObjectClass = ObjectProperty->PropertyClass;

		// Special case, load by reference, See CanSaveAsReference
		// Handle some objects that are exported to string in a special way. Similar to the UStruct above.
		if (JsonValue->Type == EJson::String)
		{
			const FString ObjectReferenceName = JsonValue->AsString();
			*ObjectPtrPtr = StaticLoadObject(UObject::StaticClass(), DefaultObjectOuter, *ObjectReferenceName);
			return true;
		}

		// Load the Normal JSON object
		// Must have the type inside the Json Object
		check(JsonValue->Type == EJson::Object);
		const TSharedPtr<FJsonObject> JsonObject = JsonValue->AsObject();
		check(JsonObject.IsValid()); // should not fail if Type == EJson::Object

		const FString SpecialKeyType = TEXT("__type__");
		if (!JsonObject->HasField(SpecialKeyType))
		{
			UE_LOG(LogDlgJsonParser,
				   Warning,
				   TEXT("PropertyName = `%s` JSON does not have the __type__ special property."),
				   *Property->GetName());
			return false;
		}

		// Create the new Object
		if (*ObjectPtrPtr == nullptr)
		{
			FString JsonObjectType;
			check(JsonObject->TryGetStringField(SpecialKeyType, JsonObjectType));

			const UClass* ChildClass = GetChildClassFromName(ObjectClass, JsonObjectType);
			if (ChildClass == nullptr)
			{
				UE_LOG(LogDlgJsonParser,
					   Warning,
					   TEXT("Could not find class `%s` for UObjectProperty = `%s`. Ignored."),
					   *JsonObjectType, *Property->GetName());
				return false;
			}

			*ObjectPtrPtr = NewObject<UObject>(DefaultObjectOuter, const_cast<UClass*>(ChildClass), NAME_None, RF_Transactional);
			check(*ObjectPtrPtr);
		}

		// Write the json object
		if (!JsonObjectToUStruct(JsonObject.ToSharedRef(), ObjectClass, *ObjectPtrPtr))
		{
			UE_LOG(LogDlgJsonParser,
				   Warning,
				   TEXT("JsonValueToUProperty - JsonObjectToUStruct failed for property %s"),
				   *Property->GetNameCPP());
			return false;
		}

		return true;
	}

	// Default to expect a string for everything else
	check(JsonValue->Type != EJson::Object);
	if (Property->ImportText(*JsonValue->AsString(), OutValue, 0, nullptr) == nullptr)
	{
		UE_LOG(LogDlgJsonParser,
			   Warning,
			   TEXT("JsonValueToUProperty - Unable import property type %s from string value for property %s"),
			   *Property->GetClass()->GetName(), *Property->GetNameCPP());
		return false;
	}
	return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool DlgJsonParser::JsonValueToUProperty(const TSharedPtr<FJsonValue> JsonValue, UProperty* Property, void* OutValue)
{
	check(Property);
	UE_LOG(LogDlgJsonParser, Verbose, TEXT("JsonValueToUProperty, PropertyName = `%s`"), *Property->GetName());

	if (!JsonValue.IsValid())
	{
		UE_LOG(LogDlgJsonParser, Error, TEXT("JsonValueToUProperty - Invalid value JSON key"));
		return false;
	}

	const bool bArrayProperty = Property->IsA<UArrayProperty>();
	const bool bJsonArray = JsonValue->Type == EJson::Array;

	// Scalar only one property
	if (!bJsonArray)
	{
		if (bArrayProperty)
		{
			UE_LOG(LogDlgJsonParser, Error, TEXT("JsonValueToUProperty - Attempted to import TArray from non-array JSON key"));
			return false;
		}

		if (Property->ArrayDim != 1)
		{
			UE_LOG(LogDlgJsonParser, Warning, TEXT("Ignoring excess properties when deserializing %s"), *Property->GetName());
		}

		return ConvertScalarJsonValueToUProperty(JsonValue, Property, OutValue);
	}

	// In practice, the ArrayDim == 1 check ought to be redundant, since nested arrays of UPropertys are not supported
	if (bArrayProperty && Property->ArrayDim == 1)
	{
		// Read into TArray
		return ConvertScalarJsonValueToUProperty(JsonValue, Property, OutValue);
	}

	// Array
	// We're deserializing a JSON array
	const auto& ArrayValue = JsonValue->AsArray();
	if (Property->ArrayDim < ArrayValue.Num())
	{
		UE_LOG(LogDlgJsonParser, Warning, TEXT("Ignoring excess properties when deserializing %s"), *Property->GetName());
	}

	// Read into native array
	const int32 ItemsToRead = FMath::Clamp(ArrayValue.Num(), 0, Property->ArrayDim);
	auto* ValuePtr = static_cast<uint8*>(OutValue);
	check(ValuePtr);
	for (int32 Index = 0; Index < ItemsToRead; ++Index)
	{
		// ValuePtr + Index * Property->ElementSize is literally FScriptArrayHelper::GetRawPtr
		if (!ConvertScalarJsonValueToUProperty(ArrayValue[Index], Property, ValuePtr + Index * Property->ElementSize))
		{
			return false;
		}
	}
	return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool DlgJsonParser::JsonAttributesToUStruct(const TMap<FString, TSharedPtr<FJsonValue>>& JsonAttributes,
											const UStruct* StructDefinition, void* OutStruct)
{
	check(StructDefinition);
	check(OutStruct);
	UE_LOG(LogDlgJsonParser, Verbose, TEXT("JsonAttributesToUStruct, StructDefinition = `%s`"), *StructDefinition->GetName());

	// Json Wrapper, already have an Object
	if (StructDefinition == FJsonObjectWrapper::StaticStruct())
	{
		// Just copy it into the object
		FJsonObjectWrapper* ProxyObject = (FJsonObjectWrapper *)OutStruct;
		ProxyObject->JsonObject = MakeShareable(new FJsonObject());
		ProxyObject->JsonObject->Values = JsonAttributes;
		return true;
	}

	// Handle UObject inheritance (children of class)
	if (StructDefinition->IsA<UClass>())
	{
		// Structure points to the child
		StructDefinition = static_cast<const UObject*>(OutStruct)->GetClass();
		check(StructDefinition);
	}

	// iterate over the struct properties
	for(TFieldIterator<UProperty> PropIt(StructDefinition); PropIt; ++PropIt)
	{
		UProperty* Property = *PropIt;
		const FString PropertyName = Property->GetName();

		// Check to see if we should ignore this property
		if (CheckFlags != 0 && !Property->HasAnyPropertyFlags(CheckFlags))
		{
			continue;
		}
		// TODO skip property

		// Find a JSON value matching this property name
		TSharedPtr<FJsonValue> JsonValue;
		for (auto& Elem : JsonAttributes)
		{
			// use case insensitive search since FName may change caseing strangely on us
			// TODO does this break on struct/classe with properties of similar name?
			if (PropertyName.Equals(Elem.Key, ESearchCase::IgnoreCase))
			{
				JsonValue = Elem.Value;
				break;
			}
		}
		if (!JsonValue.IsValid() || JsonValue->IsNull())
		{
			// we allow values to not be found since this mirrors the typical UObject mantra that all the fields are optional when deserializing
			continue;
		}

		void* ValuePtr = nullptr;
		if (Property->IsA<UObjectProperty>())
		{
			// Handle pointers, only allowed to be UObjects (are already pointers to the Value)
			check(static_cast<const UObject*>(OutStruct) != nullptr);
			ValuePtr = OutStruct;
		}
		else
		{
			// Normal non pointer property
			ValuePtr = Property->ContainerPtrToValuePtr<void>(OutStruct, 0);
		}

		// Convert the JsonValue to the Property
		if (!JsonValueToUProperty(JsonValue, Property, ValuePtr))
		{
			UE_LOG(LogDlgJsonParser,
				   Warning,
				   TEXT("JsonObjectToUStruct - Unable to parse %s.%s from JSON"),
				   *StructDefinition->GetName(), *PropertyName);
			continue;
		}
	}

	return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool DlgJsonParser::JsonObjectStringToUStruct(const UStruct* StructDefinition, void* TargetPtr)
{
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(JsonReader, JsonObject) || !JsonObject.IsValid())
	{
		UE_LOG(LogDlgJsonParser, Error, TEXT("JsonObjectStringToUStruct - Unable to parse json=[%s]"), *JsonString);
		return false;
	}
	if (!JsonObjectToUStruct(JsonObject.ToSharedRef(), StructDefinition, TargetPtr))
	{
		UE_LOG(LogDlgJsonParser, Error, TEXT("JsonObjectStringToUStruct - Unable to deserialize. json=[%s]"), *JsonString);
		return false;
	}
	return true;
}
