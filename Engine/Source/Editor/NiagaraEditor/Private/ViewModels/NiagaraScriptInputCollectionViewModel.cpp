// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

#include "NiagaraScriptInputCollectionViewModel.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraTypes.h"
#include "NiagaraScriptParameterViewModel.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeInput.h"
#include "INiagaraEditorTypeUtilities.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraDataInterface.h"

#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "NiagaraScriptInputCollection"

FText DisplayNameFormat = NSLOCTEXT("ScriptInputCollection", "DisplayNameFormat", "{0} Inputs");

FNiagaraScriptInputCollectionViewModel::FNiagaraScriptInputCollectionViewModel(UNiagaraScript* InScript, FText InDisplayName, ENiagaraParameterEditMode InParameterEditMode)
	: FNiagaraParameterCollectionViewModel(InParameterEditMode)
	, Script(InScript)
	, DisplayName(FText::Format(DisplayNameFormat, InDisplayName))
{
	if (Script && Script->Source)
	{
		Graph = Cast<UNiagaraScriptSource>(Script->Source)->NodeGraph;
		bCanHaveNumericParameters = (Script->Usage == ENiagaraScriptUsage::Function || Script->Usage == ENiagaraScriptUsage::Module);
	}
	else
	{
		Graph = nullptr;
		bCanHaveNumericParameters = true;
	}

	RefreshParameterViewModels();

	if (Graph)
	{
		OnGraphChangedHandle = Graph->AddOnGraphChangedHandler(
			FOnGraphChanged::FDelegate::CreateRaw(this, &FNiagaraScriptInputCollectionViewModel::OnGraphChanged));
	}
}

FNiagaraScriptInputCollectionViewModel::~FNiagaraScriptInputCollectionViewModel()
{
	if (Graph)
	{
		Graph->RemoveOnGraphChangedHandler(OnGraphChangedHandle);
	}
}

void FNiagaraScriptInputCollectionViewModel::SetScript(UNiagaraScript* InScript)
{
	if (Graph)
	{
		Graph->RemoveOnGraphChangedHandler(OnGraphChangedHandle);
	}

	Script = InScript;

	if (Script && Script->Source)
	{
		Graph = Cast<UNiagaraScriptSource>(Script->Source)->NodeGraph;
		OnGraphChangedHandle = Graph->AddOnGraphChangedHandler(
			FOnGraphChanged::FDelegate::CreateRaw(this, &FNiagaraScriptInputCollectionViewModel::OnGraphChanged));
		bCanHaveNumericParameters = Script->Usage == ENiagaraScriptUsage::Function || Script->Usage == ENiagaraScriptUsage::Module;
	}
	else
	{
		Graph = nullptr;
		bCanHaveNumericParameters = true;
	}

	RefreshParameterViewModels();
}

FText FNiagaraScriptInputCollectionViewModel::GetDisplayName() const 
{
	return DisplayName;
}

FVector2D GetNewNodeLocation(UNiagaraGraph* Graph, UNiagaraNode* NewInputNode, float VerticalNodeOffset, float HorizontalNodeOffset)
{
	FVector2D PlacementLocation;
	TArray<UNiagaraNodeInput*> InputNodes;
	Graph->GetNodesOfClass(InputNodes);
	if (InputNodes.Num() > 1)
	{
		// If there are input nodes, try to put it under the lowest one.
		UNiagaraNodeInput* LowestNode = nullptr;
		for (UNiagaraNodeInput* InputNode : InputNodes)
		{
			if (InputNode->Usage == ENiagaraInputNodeUsage::Parameter && InputNode != NewInputNode && (LowestNode == nullptr || InputNode->NodePosY > LowestNode->NodePosY))
			{
				LowestNode = InputNode;
			}
		}
		
		if (LowestNode)
		{
			PlacementLocation = FVector2D(
				LowestNode->NodePosX,
				LowestNode->NodePosY + VerticalNodeOffset);
		}
		else
		{
			PlacementLocation = FVector2D(0.0f, 0.0f);
		}
	}
	else
	{
		TArray<UNiagaraNode*> Nodes;
		Graph->GetNodesOfClass(Nodes);
		if (Nodes.Num() > 0)
		{
			// If there are other nodes, try to put it to the left of the leftmost one.
			UNiagaraNode* LeftmostNode = nullptr;
			for (UNiagaraNode* Node : Nodes)
			{
				if (Node != NewInputNode && (LeftmostNode == nullptr || Node->NodePosX < LeftmostNode->NodePosX))
				{
					LeftmostNode = Node;
				}
			}
			check(LeftmostNode);
			PlacementLocation = FVector2D(
				LeftmostNode->NodePosX - HorizontalNodeOffset,
				LeftmostNode->NodePosY);
		}
	}
	return PlacementLocation;
}

void FNiagaraScriptInputCollectionViewModel::AddParameter(TSharedPtr<FNiagaraTypeDefinition> ParameterType)
{
	if (Graph)
	{
		FScopedTransaction ScopedTransaction(LOCTEXT("AddScriptInput", "Add script input"));
		Graph->Modify();
		FGraphNodeCreator<UNiagaraNodeInput> InputNodeCreator(*Graph);
		UNiagaraNodeInput* InputNode = InputNodeCreator.CreateNode();

		FNiagaraEditorUtilities::InitializeParameterInputNode(*InputNode, *ParameterType.Get(), GetParameterNames());

		FVector2D PlacementLocation = GetNewNodeLocation(Graph, InputNode, 100, 150);
		InputNode->NodePosX = PlacementLocation.X;
		InputNode->NodePosY = PlacementLocation.Y;

		InputNodeCreator.Finalize();

		// The CreateNode notified the graph has changed, but changing the name and type will also need to signal the graph changed event...
		// We need to do this because NiagaraEffectScriptView model is listening for these changes to update the bindings table. This 
		// will also cause the RefreshParameterViewModels in our own graph changed handler.
		Graph->NotifyGraphChanged();

		for (TSharedRef<INiagaraParameterViewModel> ParameterViewModel : ParameterViewModels)
		{
			if (ParameterViewModel->GetName() == InputNode->Input.GetName())
			{
				GetSelection().SetSelectedObject(ParameterViewModel);
				break;
			}
		}
	}
}

void FNiagaraScriptInputCollectionViewModel::DeleteSelectedParameters()
{
	if (GetSelection().GetSelectedObjects().Num() > 0)
	{
		TSet<FGuid> InputIdsToDelete;
		for (TSharedRef<INiagaraParameterViewModel> InputParameter : GetSelection().GetSelectedObjects())
		{
			InputIdsToDelete.Add(InputParameter->GetId());
		}
		GetSelection().ClearSelectedObjects();

		if (Graph)
		{
			FScopedTransaction ScopedTransaction(NSLOCTEXT("NiagaraEmitterInputEditor", "DeletedSelectedNodes", "Delete selected nodes"));
			Graph->Modify();

			TArray<UNiagaraNodeInput*> InputNodes;
			Graph->GetNodesOfClass(InputNodes);
			for (UNiagaraNodeInput* InputNode : InputNodes)
			{
				if (InputIdsToDelete.Contains(InputNode->Input.GetId()))
				{
					InputNode->Modify();
					InputNode->DestroyNode();
				}
			}
		}
	}
}

const TArray<TSharedRef<INiagaraParameterViewModel>>& FNiagaraScriptInputCollectionViewModel::GetParameters()
{
	return ParameterViewModels;
}

void FNiagaraScriptInputCollectionViewModel::RefreshParameterViewModels()
{
	ParameterViewModels.Empty();

	TArray<UNiagaraNodeInput*> InputNodes;

	if (Graph)
	{
		Graph->GetNodesOfClass(InputNodes);
	}

	TSet<FGuid> AddedInputIds;
	for (UNiagaraNodeInput* InputNode : InputNodes)
	{
		// We can have multiple input nodes in the graph for each unique input id so make sure we only add one of each.
		if (InputNode->Usage == ENiagaraInputNodeUsage::Parameter && AddedInputIds.Contains(InputNode->Input.GetId()) == false)
		{
			FNiagaraVariable& GraphVariable = InputNode->Input;
			TSharedPtr<FNiagaraScriptParameterViewModel> ParameterViewModel;
			if (GraphVariable.GetType().GetScriptStruct() != nullptr)
			{
				FNiagaraVariable* EmitterVariable = nullptr;
				for (FNiagaraVariable& EmitterVariableToCheck : Script->Parameters.Parameters)
				{

                    // @TODO We should check ID's here, but its possible that the 
					// EmitterVariable is invalid and we may not have a great way to 
					// recover without also changing the VM.
					if (EmitterVariableToCheck.GetName() == GraphVariable.GetName())
					{
						EmitterVariable = &EmitterVariableToCheck;
						break;
					}
				}
				ParameterViewModel = MakeShareable(new FNiagaraScriptParameterViewModel(GraphVariable, *InputNode, EmitterVariable, Script, ParameterEditMode));
			}
			else
			{
				ParameterViewModel = MakeShareable(new FNiagaraScriptParameterViewModel(GraphVariable, *InputNode, InputNode->DataInterface, ParameterEditMode));
			}

			ParameterViewModel->OnNameChanged().AddRaw(this, &FNiagaraScriptInputCollectionViewModel::OnParameterNameChanged, &GraphVariable);
			ParameterViewModel->OnTypeChanged().AddRaw(this, &FNiagaraScriptInputCollectionViewModel::OnParameterTypeChanged, &GraphVariable);
			ParameterViewModel->OnDefaultValueChanged().AddRaw(this, &FNiagaraScriptInputCollectionViewModel::OnParameterValueChangedInternal, ParameterViewModel.ToSharedRef());
			ParameterViewModels.Add(ParameterViewModel.ToSharedRef());
			AddedInputIds.Add(InputNode->Input.GetId());
		}
	}

	OnCollectionChangedDelegate.Broadcast();
}

bool FNiagaraScriptInputCollectionViewModel::SupportsType(const FNiagaraTypeDefinition& Type) const
{
	return bCanHaveNumericParameters || Type != FNiagaraTypeDefinition::GetGenericNumericDef();
}

void FNiagaraScriptInputCollectionViewModel::OnGraphChanged(const struct FEdGraphEditAction& InAction)
{
	RefreshParameterViewModels();
}

void FNiagaraScriptInputCollectionViewModel::OnParameterNameChanged(FNiagaraVariable* ParameterVariable)
{
	TSet<FName> CurrentNames;
	TArray<UNiagaraNodeInput*> InputNodes;
	TArray<UNiagaraNodeInput*> InputNodesToRename;

	// Check the existing input nodes and get a set of the current names, and find nodes with matching names to rename.
	if (Graph)
	{
		Graph->GetNodesOfClass(InputNodes);
	}
	
	for (UNiagaraNodeInput* InputNode : InputNodes)
	{
		if (InputNode->Usage == ENiagaraInputNodeUsage::Parameter && &InputNode->Input != ParameterVariable)
		{
			if (InputNode->Input.GetId() == ParameterVariable->GetId())
			{
				InputNodesToRename.Add(InputNode);
			}
			else
			{
				CurrentNames.Add(InputNode->Input.GetName());
			}
		}
	}

	TSet<FName> SystemConstantNames = FNiagaraEditorUtilities::GetSystemConstantNames();

	// Rename the nodes and notify the graph that they've changed.
	FName UniqueNewName = FNiagaraEditorUtilities::GetUniqueName(ParameterVariable->GetName(), CurrentNames.Union(SystemConstantNames));
	if (ParameterVariable->GetName() != UniqueNewName)
	{
		ParameterVariable->SetName(UniqueNewName);
	}
	for (UNiagaraNodeInput* InputNodeToRename : InputNodesToRename)
	{
		InputNodeToRename->Modify();
		InputNodeToRename->Input.SetName(UniqueNewName);
	}

	if (Graph)
	{
		Graph->NotifyGraphChanged();
	}
}

void FNiagaraScriptInputCollectionViewModel::OnParameterTypeChanged(FNiagaraVariable* ParameterVariable)
{
	TArray<UNiagaraNodeInput*> InputNodes;
	TArray<UNiagaraNodeInput*> InputNodesToUpdate;

	if (Graph)
	{
		Graph->GetNodesOfClass(InputNodes);
	}
	
	for (UNiagaraNodeInput* InputNode : InputNodes)
	{
		if (InputNode->Usage == ENiagaraInputNodeUsage::Parameter && InputNode->Input.GetId() == ParameterVariable->GetId())
		{
			InputNodesToUpdate.Add(InputNode);
		}
	}

	if (InputNodesToUpdate.Num() > 0)
	{
		// Reinitialize the first node found.
		UNiagaraNodeInput* FirstNodeToUpdate = InputNodesToUpdate[0];
		FirstNodeToUpdate->Modify();
		TSet<FName> EmptyNames;
		FNiagaraEditorUtilities::InitializeParameterInputNode(*FirstNodeToUpdate, ParameterVariable->GetType(), EmptyNames, FirstNodeToUpdate->Input.GetName());
		FirstNodeToUpdate->NotifyInputTypeChanged();

		// Then copy that nodes input variable to the others so that they all have the same id and data object pointers.
		for (int32 i = 1; i < InputNodesToUpdate.Num(); ++i)
		{
			UNiagaraNodeInput* InputNodeToUpdate = InputNodesToUpdate[i];
			InputNodeToUpdate->Modify();
			InputNodeToUpdate->Input = FirstNodeToUpdate->Input;
			InputNodeToUpdate->DataInterface = FirstNodeToUpdate->DataInterface;
			InputNodeToUpdate->NotifyInputTypeChanged();
		}
	}
}

void FNiagaraScriptInputCollectionViewModel::OnParameterValueChangedInternal(const FNiagaraVariable* ChangedVariable, TSharedRef<FNiagaraScriptParameterViewModel> ChangedParameter)
{
	// Since we potentially have multiple input nodes that point to the exact same underlying input variable, we need to make sure that 
	// all of them are synchronized in their values or else we might end up with confusion when we finally compile. 
	if (ChangedParameter->GetDefaultValueType() == INiagaraParameterViewModel::EDefaultValueType::Struct || 
		ChangedParameter->GetDefaultValueType() == INiagaraParameterViewModel::EDefaultValueType::Object)
	{
		TArray<UNiagaraNodeInput*> InputNodes;
		TArray<UNiagaraNodeInput*> InputNodesToUpdate;

		if (Graph)
		{
			Graph->GetNodesOfClass(InputNodes);
		}

		for (UNiagaraNodeInput* InputNode : InputNodes)
		{
			// Copy the value to all nodes with the matching name to cover both the case where we're updating a graph variable
			// and the case where we're updating a compiled variable.
			if (InputNode->Usage == ENiagaraInputNodeUsage::Parameter && InputNode->Input.GetId() == ChangedParameter->GetId())
			{
				InputNodesToUpdate.Add(InputNode);
			}
		}

		for (UNiagaraNodeInput* InputNodeToUpdate : InputNodesToUpdate)
		{
			checkf(InputNodeToUpdate->Input.GetType() == *ChangedParameter->GetType().Get(), TEXT("Can not propagate variable values when the types don't match."));
			
			if (ChangedParameter->GetDefaultValueType() == INiagaraParameterViewModel::EDefaultValueType::Struct)
			{
				InputNodeToUpdate->Modify();
				InputNodeToUpdate->Input.AllocateData();
				InputNodeToUpdate->Input.SetData(ChangedParameter->GetDefaultValueStruct()->GetStructMemory());
			}
			else if (ChangedParameter->GetDefaultValueType() == INiagaraParameterViewModel::EDefaultValueType::Object)
			{
				// All of the variables of the same id *should* reference the same DataInterface pointer. This check
				// is just to confirm that.
				ensure(ChangedParameter->GetDefaultValueObject() == InputNodeToUpdate->DataInterface);
			}
		}
	}
	
	OnParameterValueChanged().Broadcast(ChangedVariable);
}

#undef LOCTEXT_NAMESPACE // "NiagaraScriptInputCollection"
