#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <wingdi.h>
#include <dxgi.h>
#include <dxgi1_6.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <vector>
#include <format>
#include <numbers>

#include <MinHook.h>

#include "resource.h"

#include "monitor.hpp"

#include "utils/com_ptr.hpp"
#include "utils/trampoline.hpp"

namespace
{
	com_ptr<ID3D11Device> device;
	com_ptr<ID3D11DeviceContext> ctx;
	com_ptr<ID3D11ComputeShader> render_cs;
	com_ptr<ID3D11Texture2D> virtual_desktop_tex;
	com_ptr<ID3D11Buffer> render_const_buffer;

	int w = 0, h = 0;

	struct render_constant_buffer_t
	{
		float white_level = 200.0f;
		uint32_t is_hdr = 0;

		float __gap[2];

		float transform_matrix[3][4];
	} render_cb_data;

	HINSTANCE self_instance;

	std::vector<std::unique_ptr<monitor>> monitors;

	bool init_desktop_dup()
	{
		if (device && ctx)
			return true;

		D3D_FEATURE_LEVEL feature_level;

		HRESULT hr = D3D11CreateDevice(
			nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
#ifdef _DEBUG
			D3D11_CREATE_DEVICE_DEBUG,
#else
			0,
#endif
			nullptr, 0, D3D11_SDK_VERSION,
			device, &feature_level, ctx
		);

		if (FAILED(hr))
		{
			printf("init_desktop_dup failed at line %d, hr = 0x%x\n", __LINE__, hr);
			return false;
		}

		if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0)
		{
			printf("init_desktop_dup failed at line %d, feature level < 11.0\n", __LINE__);

			device = nullptr;
			ctx = nullptr;

			return false;
		}

		return true;
	}

	void enum_monitors()
	{
		if (monitors.size())
			monitors.clear();

		com_ptr<IDXGIDevice> dxgi_device = device.as<IDXGIDevice>();
		
		if (!dxgi_device)
			throw std::runtime_error{ "enum_monitors failed to get as IDXGIDevice" };

		com_ptr<IDXGIAdapter> adapter;
		HRESULT hr = dxgi_device->GetAdapter(adapter);

		if (FAILED(hr))
		{
			auto msg = std::format("enum_monitors failed to GetAdapter: {:x}", hr);
			throw std::runtime_error{ msg };
		}

		auto outputIndex = 0u;
		while (true)
		{
			com_ptr<IDXGIOutput> output;
			hr = adapter->EnumOutputs(outputIndex++, output);

			if (hr == DXGI_ERROR_NOT_FOUND)
			{
				break;
			}

			if (FAILED(hr))
			{
				auto msg = std::format("enum_monitors failed to EnumOutputs: {:x}", hr);
				throw std::runtime_error{ msg };
			}

			com_ptr<IDXGIOutput6> output6 = output.as<IDXGIOutput6>();
			if (!output6)
				throw std::runtime_error{ "enum_monitors failed to get as IDXGIOutput6" };

			DXGI_OUTPUT_DESC1 desc;
			hr = output6->GetDesc1(&desc);

			if (FAILED(hr))
			{
				printf("enum_monitors failed to GetDesc1: %x", hr);
				continue;
			}

			if (desc.AttachedToDesktop)
			{
				monitors.push_back(std::make_unique<monitor>(output6, device));
				continue;
			}
		}
	}

	bool compile_shader()
	{
		if (render_cs)
		{
#if _DEBUG
			render_cs = nullptr;
#else
			return true;
#endif
		}

#if _DEBUG
		// compile tonemapping compute shader
		com_ptr<ID3DBlob> shader;
		com_ptr<ID3DBlob> error;

		HRESULT hr = D3DCompileFromFile(
			L"tonemapper.hlsl",
			nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
			"main", "cs_5_0",
			D3DCOMPILE_ENABLE_STRICTNESS, 0,
			shader, error
		);

		if (error)
		{
			printf("compile_shader: %s\n", reinterpret_cast<const char*>(error->GetBufferPointer()));
		}

		if (FAILED(hr))
		{
			printf("compile_shader failed at line %d, hr = 0x%x\n", __LINE__, hr);
			return false;
		}

		hr = device->CreateComputeShader(
			shader->GetBufferPointer(), shader->GetBufferSize(),
			nullptr, render_cs
		);

		if (FAILED(hr))
		{
			printf("compile_shader failed at line %d, hr = 0x%x\n", __LINE__, hr);
			return false;
		}
#else
		auto* const res = FindResourceA(self_instance, MAKEINTRESOURCE(TONEMAPPER_SHADER), RT_RCDATA);
		if (!res)
		{
			printf("compile_shader resource not found\n");
			return false;
		}

		auto* const handle = LoadResource(self_instance, res);
		if (!handle)
		{
			printf("compile_shader failed to load resource\n");
			return false;
		}

		const auto* bytecode = LockResource(handle);
		const auto size = SizeofResource(self_instance, res);

		HRESULT hr = device->CreateComputeShader(
			bytecode, size,
			nullptr, render_cs
		);

		FreeResource(handle);

		if (FAILED(hr))
		{
			printf("compile_shader failed at line %d, hr = 0x%x\n", __LINE__, hr);
			return false;
		}
#endif

		return true;
	}

	bool render(com_ptr<ID3D11Texture2D> input, com_ptr<ID3D11Texture2D> target)
	{
		if (!compile_shader())
			return false;

		D3D11_TEXTURE2D_DESC desc;
		input->GetDesc(&desc);

		render_cb_data.is_hdr = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;

		com_ptr<ID3D11ShaderResourceView> src_srv;
		D3D11_SHADER_RESOURCE_VIEW_DESC src_desc = {};
		src_desc.Format = desc.Format;
		src_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		src_desc.Texture2D.MipLevels = 1;
		HRESULT hr = device->CreateShaderResourceView(input, &src_desc, src_srv);

		if (FAILED(hr))
		{
			return false;
		}

		com_ptr<ID3D11UnorderedAccessView> dest_uav;
		D3D11_UNORDERED_ACCESS_VIEW_DESC dest_desc = {};
		dest_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		dest_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		dest_desc.Texture2D.MipSlice = 0;
		hr = device->CreateUnorderedAccessView(target, &dest_desc, dest_uav);

		if (FAILED(hr))
		{
			return false;
		}

		if (!render_const_buffer)
		{
			D3D11_BUFFER_DESC cb_desc;
			cb_desc.ByteWidth = sizeof(render_constant_buffer_t);
			cb_desc.Usage = D3D11_USAGE_DYNAMIC;
			cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			cb_desc.MiscFlags = 0;
			cb_desc.StructureByteStride = 0;

			hr = device->CreateBuffer(&cb_desc, nullptr, render_const_buffer);

			if (FAILED(hr))
				return false;

			ctx->CSSetConstantBuffers(0, 1, render_const_buffer);
		}

		D3D11_MAPPED_SUBRESOURCE mapped_cb;
		ctx->Map(render_const_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_cb);
		memcpy(mapped_cb.pData, &render_cb_data, sizeof(render_constant_buffer_t));
		ctx->Unmap(render_const_buffer, 0);

		ctx->CSSetShader(render_cs, nullptr, 0);
		ctx->CSSetShaderResources(0, 1, src_srv);
		ctx->CSSetUnorderedAccessViews(0, 1, dest_uav, nullptr);
		ctx->Dispatch((desc.Width + 15) / 16, (desc.Height + 15) / 16, 1);

		ctx->CSSetShader(nullptr, nullptr, 0);

		src_srv = nullptr;
		ctx->CSSetShaderResources(0, 1, src_srv);

		dest_uav = nullptr;
		ctx->CSSetUnorderedAccessViews(0, 1, dest_uav, nullptr);

		return true;
	}

	void capture_frame(std::vector<uint8_t>& buffer, int width, int height, int origin_x, int origin_y)
	{
		HRESULT hr = S_OK;

		if (width != w || height != h)
		{
			if (virtual_desktop_tex)
			{
				virtual_desktop_tex = nullptr;
			}

			enum_monitors();

			w = width;
			h = height;
		}

		if (!virtual_desktop_tex)
		{
			D3D11_TEXTURE2D_DESC desc;
			desc.Width = w;
			desc.Height = h;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.SampleDesc.Quality = 0;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
			desc.MiscFlags = 0;
			desc.CPUAccessFlags = 0;

			hr = device->CreateTexture2D(&desc, nullptr, virtual_desktop_tex);
			if (FAILED(hr))
			{
				auto msg = std::format("failed to create virtual desktop texture: {:x}", hr);
				throw std::runtime_error{ msg };
			}
		}

		for (const auto& monitor : monitors)
		{
			monitor->update_output_desc();
			const auto [x, y] = monitor->virtual_position();
			const auto rotation = monitor->rotation();
			const auto rad = rotation * (std::numbers::pi_v<float> / 180.f);

			const auto sin_r = std::sinf(rad);
			const auto cos_r = std::cosf(rad);

			/*
			*      cos(¦È) -sin(¦È) Tx
			* Mt = sin(¦È)  cos(¦È) Ty
			*      0       0      1
			*/
			render_cb_data.transform_matrix[0][0] = cos_r;
			render_cb_data.transform_matrix[0][1] = -sin_r;
			render_cb_data.transform_matrix[0][2] = static_cast<float>(x - origin_x);

			render_cb_data.transform_matrix[1][0] = sin_r;
			render_cb_data.transform_matrix[1][1] = cos_r;
			render_cb_data.transform_matrix[1][2] = static_cast<float>(y - origin_y);

			render_cb_data.transform_matrix[2][0] = 0;
			render_cb_data.transform_matrix[2][1] = 0;
			render_cb_data.transform_matrix[2][2] = 1;

			printf("transform matrix: \n%.6f %.6f %.6f\n%.6f %.6f %.6f\n%.6f %.6f %.6f\n",
				   render_cb_data.transform_matrix[0][0], render_cb_data.transform_matrix[0][1], render_cb_data.transform_matrix[0][2],
				   render_cb_data.transform_matrix[1][0], render_cb_data.transform_matrix[1][1], render_cb_data.transform_matrix[1][2],
				   render_cb_data.transform_matrix[2][0], render_cb_data.transform_matrix[2][1], render_cb_data.transform_matrix[2][2]
			);

			render_cb_data.white_level = monitor->sdr_white_level();

			auto screenshot = monitor->take_screenshot();
			if (!render(screenshot, virtual_desktop_tex)) [[unlikely]]
			{
				auto name = monitor->name();
				printf("failed to render monitor %s to virtual desktop texture\n", name.data());
			}
		}

		D3D11_TEXTURE2D_DESC staging_desc;
		virtual_desktop_tex->GetDesc(&staging_desc);
		staging_desc.Usage = D3D11_USAGE_STAGING;
		staging_desc.BindFlags = 0;
		staging_desc.MiscFlags = 0;
		staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

		com_ptr<ID3D11Texture2D> staging_tex;
		hr = device->CreateTexture2D(&staging_desc, nullptr, staging_tex);
		if (FAILED(hr))
		{
			auto msg = std::format("failed to create staging texture: {:x}", hr);
			throw std::runtime_error{ msg };
		}

		ctx->CopyResource(staging_tex, virtual_desktop_tex);

		D3D11_MAPPED_SUBRESOURCE mapped;
		ctx->Map(staging_tex, 0, D3D11_MAP_READ, 0, &mapped);

		buffer.resize(staging_desc.Width * staging_desc.Height * 4);

		for (size_t i = 0; i < staging_desc.Height; i++)
		{
			const auto* src = reinterpret_cast<uint8_t*>(mapped.pData) + mapped.RowPitch * i;
			auto* dest = buffer.data() + (staging_desc.Width * 4 * i);

			std::memcpy(dest, src, staging_desc.Width * 4);
		}

		ctx->Unmap(staging_tex, 0);
	}

	trampoline<decltype(BitBlt)> bitblt;
	BOOL WINAPI bitblt_hook(HDC hdc, int x, int y, int cx, int cy, HDC hdcSrc, int x1, int y1, DWORD rop)
	{
		printf("bitblt called\n");

		static bool inited = init_desktop_dup();

		if (!inited)
			return bitblt(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

		auto src_window = WindowFromDC(hdcSrc);
		auto desktop_window = GetDesktopWindow();

		if (src_window != desktop_window)
			return bitblt(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

		std::vector<uint8_t> buffer;

		try
		{
			capture_frame(buffer, cx, cy, x1, y1);
		}
		catch (std::runtime_error e)
		{
			printf("failed to capture_frame, error: \n%s\n", e.what());
			return bitblt(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);
		}

		HBITMAP map = CreateBitmap(cx, cy, 1, 32, buffer.data());
		HDC src = CreateCompatibleDC(hdc);
		SelectObject(src, map);

		auto result = bitblt(hdc, x, y, cx, cy, src, 0, 0, rop & ~CAPTUREBLT);

		DeleteDC(src);
		DeleteObject(map);

		return result;
	}

	void free_desktop_dup()
	{
		monitors.clear();

		render_const_buffer = nullptr;
		virtual_desktop_tex = nullptr;
		render_cs = nullptr;
		ctx = nullptr;
		device = nullptr;
	}

	trampoline<void WINAPI(UINT)> exit_process;
	void exit_process_hook(UINT code)
	{
		free_desktop_dup();
		exit_process(code);
	}

#if _DEBUG
	void create_console()
	{
		AllocConsole();

		FILE* f;
		auto _ = freopen_s(&f, "CONOUT$", "w+t", stdout);
		_ = freopen_s(&f, "CONOUT$", "w", stderr);
		_ = freopen_s(&f, "CONIN$", "r", stdin);
	}
#endif

	class hook_autoinit
	{
	public:
		hook_autoinit()
		{
#if _DEBUG
			create_console();
#endif
			LoadLibraryA("gdi32.dll");
			MH_Initialize();
			MH_CreateHookApi(L"gdi32.dll", "BitBlt", bitblt_hook, &bitblt);
			MH_CreateHookApi(L"kernel32.dll", "ExitProcess", exit_process_hook, &exit_process);
			MH_EnableHook(MH_ALL_HOOKS);
		}
	} hook;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD fdwReason, LPVOID)
{
	if (fdwReason == DLL_PROCESS_ATTACH)
	{
		self_instance = instance;
	}

	return TRUE;
}
