#include <Windows.h>
#include <tchar.h>
#include <wrl/client.h>
#include <stdexcept>
#include <dxgi1_3.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include "../_common/dxcommon.h"

#include <DirectXMath.h>
using DirectX::XMFLOAT3; // for WaveFrontReader
#include "../Mesh/WaveFrontReader.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3dcompiler.lib")

using namespace std;
using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace
{
	const int WINDOW_WIDTH = 400;
	const int WINDOW_HEIGHT = 240;
	const int BUFFER_COUNT = 2;
	HWND g_mainWindowHandle = 0;
};

void CHK(HRESULT hr)
{
	if (FAILED(hr))
		throw runtime_error("HRESULT is failed value.");
}

class D3D
{
	ComPtr<IDXGIFactory2> mDxgiFactory;
	ComPtr<IDXGISwapChain1> mSwapChain;
	ComPtr<ID3D12Resource> mD3DBuffer[BUFFER_COUNT];
	int mBufferWidth, mBufferHeight;
	UINT64 mFrameCount = 0;

	ID3D12Device* mDev;
	ComPtr<ID3D12CommandAllocator> mCmdAlloc;
	ComPtr<ID3D12CommandQueue> mCmdQueue;

	ComPtr<ID3D12GraphicsCommandList> mCmdList;
	ComPtr<ID3D12Fence> mFence;
	HANDLE mFenceEveneHandle = 0;

	ComPtr<ID3D12DescriptorHeap> mDescHeapRtv;
	ComPtr<ID3D12DescriptorHeap> mDescHeapDsv;
	ComPtr<ID3D12DescriptorHeap> mDescHeapCbvSrvUav;
	ComPtr<ID3D12DescriptorHeap> mDescHeapSampler;
	void* mCBUploadPtr = nullptr;

	ComPtr<ID3D12RootSignature> mRootSignature;
	ComPtr<ID3D12PipelineState> mPso;
	ComPtr<ID3D12Resource> mVB;
	D3D12_VERTEX_BUFFER_VIEW mVBView = {};
	D3D12_INDEX_BUFFER_VIEW mIBView = {};
	UINT mIndexCount = 0;
	UINT mVBIndexOffset = 0;
	ComPtr<ID3D12Resource> mDB;
	ComPtr<ID3D12Resource> mCB;
	ComPtr<ID3D12Resource> mTex;

public:
	D3D(int width, int height, HWND hWnd)
		: mBufferWidth(width), mBufferHeight(height), mDev(nullptr)
	{
		{
#if _DEBUG
			CHK(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(mDxgiFactory.ReleaseAndGetAddressOf())));
#else
			CHK(CreateDXGIFactory2(0, IID_PPV_ARGS(mDxgiFactory.ReleaseAndGetAddressOf())));
#endif /* _DEBUG */
		}

#if _DEBUG
		ID3D12Debug* debug = nullptr;
		D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
		if (debug)
		{
			debug->EnableDebugLayer();
			debug->Release();
			debug = nullptr;
		}
#endif /* _DEBUG */
		ID3D12Device* dev;
		CHK(D3D12CreateDevice(
			nullptr,
			D3D_FEATURE_LEVEL_11_0,
			IID_PPV_ARGS(&dev)));
		mDev = dev;

		CHK(mDev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(mCmdAlloc.ReleaseAndGetAddressOf())));

		D3D12_COMMAND_QUEUE_DESC queueDesc = {};
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		CHK(mDev->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(mCmdQueue.ReleaseAndGetAddressOf())));

		DXGI_SWAP_CHAIN_DESC1 scDesc = {};
		scDesc.Width = width;
		scDesc.Height = height;
		scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		scDesc.SampleDesc.Count = 1;
		scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		scDesc.BufferCount = BUFFER_COUNT;
		scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
		CHK(mDxgiFactory->CreateSwapChainForHwnd(mCmdQueue.Get(), hWnd, &scDesc, nullptr, nullptr, mSwapChain.ReleaseAndGetAddressOf()));

		CHK(mDev->CreateCommandList(
			0,
			D3D12_COMMAND_LIST_TYPE_DIRECT,
			mCmdAlloc.Get(),
			nullptr,
			IID_PPV_ARGS(mCmdList.ReleaseAndGetAddressOf())));

		CHK(mDev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(mFence.ReleaseAndGetAddressOf())));

		mFenceEveneHandle = CreateEvent(nullptr, FALSE, FALSE, nullptr);

		for (int i = 0; i < BUFFER_COUNT; i++)
		{
			CHK(mSwapChain->GetBuffer(i, IID_PPV_ARGS(mD3DBuffer[i].ReleaseAndGetAddressOf())));
			mD3DBuffer[i]->SetName(L"SwapChain_Buffer");
		}

		{
			D3D12_DESCRIPTOR_HEAP_DESC desc = {};
			desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			desc.NumDescriptors = 10;
			desc.NodeMask = 0;
			CHK(mDev->CreateDescriptorHeap(&desc, IID_PPV_ARGS(mDescHeapRtv.ReleaseAndGetAddressOf())));

			desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
			desc.NumDescriptors = 10;
			desc.NodeMask = 0;
			CHK(mDev->CreateDescriptorHeap(&desc, IID_PPV_ARGS(mDescHeapDsv.ReleaseAndGetAddressOf())));

			desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			desc.NumDescriptors = 100;
			desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			desc.NodeMask = 0;
			CHK(mDev->CreateDescriptorHeap(&desc, IID_PPV_ARGS(mDescHeapCbvSrvUav.ReleaseAndGetAddressOf())));

			desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
			desc.NumDescriptors = 100;
			desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			desc.NodeMask = 0;
			CHK(mDev->CreateDescriptorHeap(&desc, IID_PPV_ARGS(mDescHeapSampler.ReleaseAndGetAddressOf())));
		}

		auto rtvStep = mDev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		for (auto i = 0u; i < BUFFER_COUNT; i++)
		{
			auto d = mDescHeapRtv->GetCPUDescriptorHandleForHeapStart();
			d.ptr += i * rtvStep;
			mDev->CreateRenderTargetView(mD3DBuffer[i].Get(), nullptr, d);
		}

		{
			CD3DX12_DESCRIPTOR_RANGE descRange1[2];
			descRange1[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
			descRange1[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

			CD3DX12_DESCRIPTOR_RANGE descRange2[1];
			descRange2[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0);

			CD3DX12_ROOT_PARAMETER rootParam[2];
			rootParam[0].InitAsDescriptorTable(ARRAYSIZE(descRange1), descRange1);
			rootParam[1].InitAsDescriptorTable(ARRAYSIZE(descRange2), descRange2, D3D12_SHADER_VISIBILITY_PIXEL);

			ID3D10Blob *sig, *info;
			auto rootSigDesc = D3D12_ROOT_SIGNATURE_DESC();
			rootSigDesc.NumParameters = ARRAYSIZE(rootParam);
			rootSigDesc.NumStaticSamplers = 0;
			rootSigDesc.pParameters = rootParam;
			rootSigDesc.pStaticSamplers = nullptr;
			rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
			CHK(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &info));
			mDev->CreateRootSignature(
				0,
				sig->GetBufferPointer(),
				sig->GetBufferSize(),
				IID_PPV_ARGS(mRootSignature.ReleaseAndGetAddressOf()));
			sig->Release();
		}

		ID3D10Blob *vs, *ps;
		{
			ID3D10Blob *info;
			UINT flag = 0;
#if _DEBUG
			flag |= D3DCOMPILE_DEBUG;
#endif /* _DEBUG */
			CHK(D3DCompileFromFile(L"MeshTex.hlsl", nullptr, nullptr, "VSMain", "vs_5_0", flag, 0, &vs, &info));
			CHK(D3DCompileFromFile(L"MeshTex.hlsl", nullptr, nullptr, "PSMain", "ps_5_0", flag, 0, &ps, &info));
		}
		D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		};
		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDesc.InputLayout.NumElements = 3;
		psoDesc.InputLayout.pInputElementDescs = inputLayout;
		psoDesc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
		psoDesc.pRootSignature = mRootSignature.Get();
		psoDesc.VS.pShaderBytecode = vs->GetBufferPointer();
		psoDesc.VS.BytecodeLength = vs->GetBufferSize();
		psoDesc.PS.pShaderBytecode = ps->GetBufferPointer();
		psoDesc.PS.BytecodeLength = ps->GetBufferSize();
		psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(CCCD3DX12_DEFAULT());
		psoDesc.BlendState = CD3DX12_BLEND_DESC(CCCD3DX12_DEFAULT());
		psoDesc.DepthStencilState.DepthEnable = true;
		psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		psoDesc.DepthStencilState.StencilEnable = false;
		psoDesc.SampleMask = UINT_MAX;
		psoDesc.NumRenderTargets = 1;
		psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
		psoDesc.SampleDesc.Count = 1;
		CHK(mDev->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(mPso.ReleaseAndGetAddressOf())));
		vs->Release();
		ps->Release();

		WaveFrontReader<uint16_t> mesh;
		CHK(mesh.Load(L"teapot_tex2.obj"));

		mIndexCount = static_cast<UINT>(mesh.indices.size());
		mVBIndexOffset = static_cast<UINT>(sizeof(mesh.vertices[0]) * mesh.vertices.size());
		UINT IBSize = static_cast<UINT>(sizeof(mesh.indices[0]) * mIndexCount);

		void* vbData = mesh.vertices.data();
		void* ibData = mesh.indices.data();
		CHK(mDev->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(mVBIndexOffset + IBSize),
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(mVB.ReleaseAndGetAddressOf())));
		mVB->SetName(L"VertexBuffer");
		char* vbUploadPtr = nullptr;
		CHK(mVB->Map(0, nullptr, reinterpret_cast<void**>(&vbUploadPtr)));
		memcpy_s(vbUploadPtr, mVBIndexOffset, vbData, mVBIndexOffset);
		memcpy_s(vbUploadPtr + mVBIndexOffset, IBSize, ibData, IBSize);
		mVB->Unmap(0, nullptr);

		mVBView.BufferLocation = mVB->GetGPUVirtualAddress();
		mVBView.StrideInBytes = sizeof(mesh.vertices[0]);
		mVBView.SizeInBytes = mVBIndexOffset;
		mIBView.BufferLocation = mVB->GetGPUVirtualAddress() + mVBIndexOffset;
		mIBView.Format = DXGI_FORMAT_R16_UINT;
		mIBView.SizeInBytes = IBSize;

		auto resourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(
			DXGI_FORMAT_R32_TYPELESS, mBufferWidth, mBufferHeight, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
			D3D12_TEXTURE_LAYOUT_UNKNOWN, 0);
		D3D12_CLEAR_VALUE dsvClearValue;
		dsvClearValue.Format = DXGI_FORMAT_D32_FLOAT;
		dsvClearValue.DepthStencil.Depth = 1.0f;
		dsvClearValue.DepthStencil.Stencil = 0;
		CHK(mDev->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), // No need to read/write by CPU
			D3D12_HEAP_FLAG_NONE,
			&resourceDesc,
			D3D12_RESOURCE_STATE_DEPTH_WRITE,
			&dsvClearValue,
			IID_PPV_ARGS(mDB.ReleaseAndGetAddressOf())));
		mDB->SetName(L"DepthTexture");

		D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
		dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
		dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
		dsvDesc.Texture2D.MipSlice = 0;
		dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
		mDev->CreateDepthStencilView(mDB.Get(), &dsvDesc, mDescHeapDsv->GetCPUDescriptorHandleForHeapStart());

		CHK(mDev->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT),
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(mCB.ReleaseAndGetAddressOf())));
		mCB->SetName(L"ConstantBuffer");
		D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
		cbvDesc.BufferLocation = mCB->GetGPUVirtualAddress();
		cbvDesc.SizeInBytes = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT; // must be a multiple of 256
		mDev->CreateConstantBufferView(
			&cbvDesc,
			mDescHeapCbvSrvUav->GetCPUDescriptorHandleForHeapStart());
		CHK(mCB->Map(0, nullptr, reinterpret_cast<void**>(&mCBUploadPtr)));

		{
			// Read DDS file
			vector<char> texData(4 * 256 * 256);
			ifstream ifs("cat.dds", ios::binary);
			if (!ifs)
				throw runtime_error("Texture not found.");
			ifs.seekg(128, ios::beg); // Skip DDS header
			ifs.read(texData.data(), texData.size());
			resourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(
				DXGI_FORMAT_B8G8R8A8_UNORM, 256, 256, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_NONE,
				D3D12_TEXTURE_LAYOUT_UNKNOWN, 0);
			CHK(mDev->CreateCommittedResource(
				&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
				D3D12_HEAP_FLAG_NONE,
				&resourceDesc,
				D3D12_RESOURCE_STATE_GENERIC_READ,
				nullptr,
				IID_PPV_ARGS(mTex.ReleaseAndGetAddressOf())));
			mTex->SetName(L"Texure");
			D3D12_BOX box = {};
			box.right = 256;
			box.bottom = 256;
			box.back = 1;
			CHK(mTex->WriteToSubresource(0, &box, texData.data(), 4 * 256, 4 * 256 * 256));
		}

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Texture2D.MipLevels = 1;
		srvDesc.Texture2D.MostDetailedMip = 0; // No MIP
		srvDesc.Texture2D.PlaneSlice = 0;
		srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
		auto cbvSrcUavDescHandle = mDescHeapCbvSrvUav->GetCPUDescriptorHandleForHeapStart();
		auto cbvSrvUavDescStep =  mDev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		cbvSrcUavDescHandle.ptr += cbvSrvUavDescStep;
		mDev->CreateShaderResourceView(mTex.Get(), &srvDesc, cbvSrcUavDescHandle);

		D3D12_SAMPLER_DESC samplerDesc;
		samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplerDesc.MinLOD = -FLT_MAX;
		samplerDesc.MaxLOD = FLT_MAX;
		samplerDesc.MipLODBias = 0;
		samplerDesc.MaxAnisotropy = 0;
		samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		mDev->CreateSampler(&samplerDesc, mDescHeapSampler->GetCPUDescriptorHandleForHeapStart());
	}
	~D3D()
	{
		mCB->Unmap(0, nullptr);
		CloseHandle(mFenceEveneHandle);
	}
	ID3D12Device* GetDevice() const
	{
		return mDev;
	}
	void Draw()
	{
		mFrameCount++;

		// Upload constant buffer
		{
			static float rot = 0.0f;
			rot += 1.0f;
			if (rot >= 360.0f) rot = 0.0f;

			XMMATRIX worldMat, viewMat, projMat;
			worldMat = XMMatrixRotationY(XMConvertToRadians(rot));
			viewMat = XMMatrixLookAtLH({ 0, 1, -2.5f }, { 0, 0.5f, 0 }, { 0, 1, 0 });
			projMat = XMMatrixPerspectiveFovLH(45, (float)mBufferWidth / mBufferHeight, 0.01f, 50.0f);
			auto mvpMat = XMMatrixTranspose(worldMat * viewMat * projMat);

			auto worldTransMat = XMMatrixTranspose(worldMat);

			// mCBUploadPtr is Write-Combine memory
			memcpy_s(mCBUploadPtr, 64, &mvpMat, 64);
			memcpy_s(reinterpret_cast<char*>(mCBUploadPtr) + 64, 64, &worldTransMat, 64);
		}

		// Get current RTV descriptor
		auto descHandleRtvStep = mDev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		D3D12_CPU_DESCRIPTOR_HANDLE descHandleRtv = mDescHeapRtv->GetCPUDescriptorHandleForHeapStart();
		descHandleRtv.ptr += ((mFrameCount - 1) % BUFFER_COUNT) * descHandleRtvStep;
		// Get current swap chain
		ID3D12Resource* d3dBuffer = mD3DBuffer[(mFrameCount - 1) % BUFFER_COUNT].Get();
		// Get DSV
		auto descHandleDsv = mDescHeapDsv->GetCPUDescriptorHandleForHeapStart();

		// Barrier Present -> RenderTarget
		setResourceBarrier(mCmdList.Get(), d3dBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

		// Viewport & Scissor
		D3D12_VIEWPORT viewport = {};
		viewport.Width = (float)mBufferWidth;
		viewport.Height = (float)mBufferHeight;
		viewport.MinDepth = 0.0f;
		viewport.MaxDepth = 1.0f;
		mCmdList->RSSetViewports(1, &viewport);
		D3D12_RECT scissor = {};
		scissor.right = (LONG)mBufferWidth;
		scissor.bottom = (LONG)mBufferHeight;
		mCmdList->RSSetScissorRects(1, &scissor);

		// Clear DepthTexture
		mCmdList->ClearDepthStencilView(descHandleDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

		// Clear
		{
			float clearColor[4] = { 0.1f, 0.2f, 0.3f, 1.0f };
			mCmdList->ClearRenderTargetView(descHandleRtv, clearColor, 0, nullptr);
		}

		mCmdList->OMSetRenderTargets(1, &descHandleRtv, true, &descHandleDsv);

		// Draw
		mCmdList->SetGraphicsRootSignature(mRootSignature.Get());
		ID3D12DescriptorHeap* descHeaps[] = { mDescHeapCbvSrvUav.Get(), mDescHeapSampler.Get() };
		mCmdList->SetDescriptorHeaps(ARRAYSIZE(descHeaps), descHeaps);
		{
			mCmdList->SetGraphicsRootDescriptorTable(0, mDescHeapCbvSrvUav->GetGPUDescriptorHandleForHeapStart());
			mCmdList->SetGraphicsRootDescriptorTable(1, mDescHeapSampler->GetGPUDescriptorHandleForHeapStart());
			mCmdList->SetPipelineState(mPso.Get());
			mCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			mCmdList->IASetVertexBuffers(0, 1, &mVBView);
			mCmdList->IASetIndexBuffer(&mIBView);
			mCmdList->DrawIndexedInstanced(mIndexCount, 1, 0, 0, 0);
		}

		// Barrier RenderTarget -> Present
		setResourceBarrier(mCmdList.Get(), d3dBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

		// Exec
		CHK(mCmdList->Close());
		ID3D12CommandList* const cmdList = mCmdList.Get();
		mCmdQueue->ExecuteCommandLists(1, &cmdList);

		// Present
		CHK(mSwapChain->Present(1, 0));

		// Set queue flushed event
		CHK(mFence->SetEventOnCompletion(mFrameCount, mFenceEveneHandle));

		// Wait for queue flushed
		// This code would occur CPU stall!
		CHK(mCmdQueue->Signal(mFence.Get(), mFrameCount));
		DWORD wait = WaitForSingleObject(mFenceEveneHandle, 10000);
		if (wait != WAIT_OBJECT_0)
			throw runtime_error("Failed WaitForSingleObject().");

		CHK(mCmdAlloc->Reset());
		CHK(mCmdList->Reset(mCmdAlloc.Get(), nullptr));
	}

private:
	void setResourceBarrier(ID3D12GraphicsCommandList* commandList,
		ID3D12Resource* res,
		D3D12_RESOURCE_STATES before,
		D3D12_RESOURCE_STATES after)
	{
		D3D12_RESOURCE_BARRIER desc = {};
		desc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		desc.Transition.pResource = res;
		desc.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		desc.Transition.StateBefore = before;
		desc.Transition.StateAfter = after;
		desc.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		commandList->ResourceBarrier(1, &desc);
	}
};

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	PAINTSTRUCT ps;
	HDC hdc;

	switch (message) {
	case WM_KEYDOWN:
		if (wParam == VK_ESCAPE) {
			PostMessage(hWnd, WM_DESTROY, 0, 0);
			return 0;
		}
		break;

	case WM_PAINT:
		hdc = BeginPaint(hWnd, &ps);
		EndPaint(hWnd, &ps);
		break;

	case WM_DESTROY:
		PostQuitMessage(0);
		break;

	default:
		return DefWindowProc(hWnd, message, wParam, lParam);
	}
	return 0;
}

static HWND setupWindow(int width, int height)
{
	WNDCLASSEX wcex;
	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.style = CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc = WndProc;
	wcex.cbClsExtra = 0;
	wcex.cbWndExtra = 0;
	wcex.hInstance = (HMODULE)GetModuleHandle(0);
	wcex.hIcon = nullptr;
	wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wcex.lpszMenuName = nullptr;
	wcex.lpszClassName = _T("WindowClass");
	wcex.hIconSm = nullptr;
	if (!RegisterClassEx(&wcex)) {
		throw runtime_error("RegisterClassEx()");
	}

	RECT rect = { 0, 0, width, height };
	AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
	const int windowWidth = (rect.right - rect.left);
	const int windowHeight = (rect.bottom - rect.top);

	HWND hWnd = CreateWindow(_T("WindowClass"), _T("Window"),
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0, windowWidth, windowHeight,
		nullptr, nullptr, nullptr, nullptr);
	if (!hWnd) {
		throw runtime_error("CreateWindow()");
	}

	return hWnd;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
	MSG msg;
	ZeroMemory(&msg, sizeof msg);

	ID3D12Device* dev = nullptr;

#ifdef NDEBUG
	try
#endif
	{
		g_mainWindowHandle = setupWindow(WINDOW_WIDTH, WINDOW_HEIGHT);
		ShowWindow(g_mainWindowHandle, SW_SHOW);
		UpdateWindow(g_mainWindowHandle);

		D3D d3d(WINDOW_WIDTH, WINDOW_HEIGHT, g_mainWindowHandle);
		dev = d3d.GetDevice();

		while (msg.message != WM_QUIT) {
			BOOL r = PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE);
			if (r == 0) {
				d3d.Draw();
			}
			else {
				DispatchMessage(&msg);
			}
		}
	}
#ifdef NDEBUG
	catch (std::exception &e) {
		MessageBoxA(g_mainWindowHandle, e.what(), "Exception occuured.", MB_ICONSTOP);
	}
#endif

	if (dev)
		dev->Release();

	return static_cast<int>(msg.wParam);
}
