
#include "ComputeShaderPrivatePCH.h"
#include "WeatherData.h"


#define NUM_THREADS_PER_GROUP_DIMENSION 4 // This has to be the same as in the compute shaders spec [X, X, 1]

FSimulationComputeShader::FSimulationComputeShader(ERHIFeatureLevel::Type ShaderFeatureLevel)
{
	FeatureLevel = ShaderFeatureLevel;

	IsComputeShaderExecuting = false;
	IsUnloading = false;
}

FSimulationComputeShader::~FSimulationComputeShader()
{
	IsUnloading = true;
}

void FSimulationComputeShader::Initialize(
	TResourceArray<FComputeShaderSimulationCell>& Cells, TResourceArray<FClimateData>& ClimateData, 
	float k_e, float k_m, float TMeltA, float TMeltB, float TSnowA, float TSnowB, 
	int32 TotalSimulationHours, int32 CellsDimension, int32 ClimateDataDimension)
{
	NumCells = Cells.Num();

	// Create output texture
	FRHIResourceCreateInfo CreateInfo;
	Texture = RHICreateTexture2D(CellsDimension, CellsDimension, PF_R32_UINT, 1, 1, TexCreate_ShaderResource | TexCreate_UAV, CreateInfo);
	TextureUAV = RHICreateUnorderedAccessView(Texture);

	// Create input data buffers
	SimulationCellsBuffer = new FRWStructuredBuffer();
	SimulationCellsBuffer->Initialize(sizeof(FComputeShaderSimulationCell), CellsDimension * CellsDimension, &Cells, 0, true, false);

	ClimateDataBuffer = new FRWStructuredBuffer();
	ClimateDataBuffer->Initialize(sizeof(FClimateData), TotalSimulationHours * ClimateDataDimension * ClimateDataDimension, &ClimateData, 0, true, false);

	TResourceArray<uint32> MaxSnowArray;
	MaxSnowArray.Add(0);
	MaxSnowBuffer = new FRWStructuredBuffer();
	MaxSnowBuffer->Initialize(sizeof(uint32), 1, &MaxSnowArray, 0, true, false);

	SnowOutputBuffer = new FRWStructuredBuffer();
	SnowOutputBuffer->Initialize(sizeof(float), CellsDimension * CellsDimension, nullptr, 0, true, false);

	// Fill constant parameters
	ConstantParameters.CellsDimension = CellsDimension;
	ConstantParameters.ClimateDataDimension = ClimateDataDimension;
	ConstantParameters.ThreadGroupCountX = Texture->GetSizeX() / NUM_THREADS_PER_GROUP_DIMENSION;
	ConstantParameters.ThreadGroupCountY = Texture->GetSizeY() / NUM_THREADS_PER_GROUP_DIMENSION;
	ConstantParameters.k_e = k_e;
	ConstantParameters.k_m = k_m;
	ConstantParameters.TMeltA = TMeltA;
	ConstantParameters.TMeltB = TMeltB;
	ConstantParameters.TSnowA = TSnowA;
	ConstantParameters.TSnowB = TSnowB;

	VariableParameters = FComputeShaderVariableParameters();
}

void FSimulationComputeShader::ExecuteComputeShader(int CurrentSimulationStep)
{
	// Skip this execution round if we are already executing
	if (IsUnloading || IsComputeShaderExecuting) return;

	IsComputeShaderExecuting = true;

	// Now set our runtime parameters
	VariableParameters.CurrentSimulationStep = CurrentSimulationStep;

	// This macro sends the function we declare inside to be run on the render thread. What we do is essentially just send this class and tell the render thread to run the internal render function as soon as it can.
	// I am still not 100% Certain on the thread safety of this, if you are getting crashes, depending on how advanced code you have in the start of the ExecutePixelShader function, you might have to use a lock :)
	ENQUEUE_UNIQUE_RENDER_COMMAND_ONEPARAMETER(
		FComputeShaderRunner,
		FSimulationComputeShader*, ComputeShader, this, 
		{
			ComputeShader->ExecuteComputeShaderInternal();
		}
	);
}

void FSimulationComputeShader::ExecuteComputeShaderInternal()
{
	check(IsInRenderingThread());

	// If we are about to unload, so just clean up the UAV
	if (IsUnloading)
	{
		if (NULL != TextureUAV)
		{
			TextureUAV.SafeRelease();
			TextureUAV = NULL;
		}
		if (NULL != SimulationCellsBuffer)
		{
			SimulationCellsBuffer->Release();
			delete SimulationCellsBuffer;
		}
		if (NULL != MaxSnowBuffer)
		{
			MaxSnowBuffer->Release();
			delete MaxSnowBuffer;
		}

		return;
	}
	// Get global RHI command list
	FRHICommandListImmediate& RHICmdList = GRHICommandList.GetImmediateCommandList();

	// Compute shader calculation
	TShaderMapRef<FComputeShaderDeclaration> ComputeShader(GetGlobalShaderMap(FeatureLevel));
	
	RHICmdList.SetComputeShader(ComputeShader->GetComputeShader());

	// Set inputs/outputs and dispatch compute shader
	ComputeShader->SetParameters(RHICmdList, TextureUAV, SimulationCellsBuffer->UAV, ClimateDataBuffer->UAV, SnowOutputBuffer->UAV, MaxSnowBuffer->UAV);
	ComputeShader->SetUniformBuffers(RHICmdList, ConstantParameters, VariableParameters);
	DispatchComputeShader(RHICmdList, *ComputeShader, Texture->GetSizeX() / NUM_THREADS_PER_GROUP_DIMENSION, Texture->GetSizeY() / NUM_THREADS_PER_GROUP_DIMENSION, 1);
	
	// Store max snow
	TArray<uint32> MaxSnowArray;
	MaxSnowArray.Reserve(1);
	MaxSnowArray.AddUninitialized(1);
	uint32* MaxBuffer = (uint32*)RHICmdList.LockStructuredBuffer(MaxSnowBuffer->Buffer, 0, MaxSnowBuffer->NumBytes, RLM_ReadOnly);
	FMemory::Memcpy(MaxSnowArray.GetData(), MaxBuffer, MaxSnowBuffer->NumBytes);
	RHICmdList.UnlockStructuredBuffer(MaxSnowBuffer->Buffer);

	MaxSnow = MaxSnowArray[0] / 100000.0f;
	UE_LOG(LogConsoleResponse, Display, TEXT("Max snow \"%f\""), MaxSnow);

	ComputeShader->UnbindBuffers(RHICmdList);
	IsComputeShaderExecuting = false;

	// Copy results from the GPU
	if (Debug)
	{
		TArray<FComputeShaderSimulationCell> SimulationCells;
		SimulationCells.Reserve(NumCells);
		SimulationCells.AddUninitialized(NumCells);
		uint32* CellsBuffer = (uint32*)RHICmdList.LockStructuredBuffer(SimulationCellsBuffer->Buffer, 0, SimulationCellsBuffer->NumBytes, RLM_ReadOnly);
		FMemory::Memcpy(SimulationCells.GetData(), CellsBuffer, SimulationCellsBuffer->NumBytes);
		RHICmdList.UnlockStructuredBuffer(SimulationCellsBuffer->Buffer);


		// Log max snow
		TArray<float> SnowArray;
		SnowArray.Reserve(NumCells);
		SnowArray.AddUninitialized(NumCells);
		uint32* SnowBuffer = (uint32*)RHICmdList.LockStructuredBuffer(SnowOutputBuffer->Buffer, 0, SnowOutputBuffer->NumBytes, RLM_ReadOnly);
		FMemory::Memcpy(SnowArray.GetData(), SnowBuffer, SnowOutputBuffer->NumBytes);
		RHICmdList.UnlockStructuredBuffer(SnowOutputBuffer->Buffer);
	}
}

float FSimulationComputeShader::GetMaxSnow()
{
	return MaxSnow;
}
