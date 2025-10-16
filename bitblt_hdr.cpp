#include "bitblt_hdr.hpp"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <dxgi1_6.h>
#include <wingdi.h>

#include <format>
#include <numbers>
#include <vector>

#include "monitor.hpp"
#include "utils/com_ptr.hpp"

bitblt_hdr::bitblt_hdr(trampoline<decltype(BitBlt)> bitblt) : bitblt_(bitblt)
{
	init_desktop_dup();
}

bitblt_hdr::~bitblt_hdr()
{
	monitors_.clear();

	render_const_buffer_ = nullptr;
	virtual_desktop_tex_ = nullptr;
	render_cs_ = nullptr;
	ctx_ = nullptr;
	device_ = nullptr;
}

bool bitblt_hdr::init_desktop_dup()
{
	if (device_ && ctx_)
	{
		return true;
	}

	D3D_FEATURE_LEVEL feature_level;

	HRESULT hr = D3D11CreateDevice(nullptr,
				 D3D_DRIVER_TYPE_HARDWARE,
				 nullptr,
#ifdef _DEBUG
				 D3D11_CREATE_DEVICE_DEBUG,
#else
				 0,
#endif
				 nullptr,
				 0,
				 D3D11_SDK_VERSION,
				 device_,
				 &feature_level,
				 ctx_);

	if (FAILED(hr))
	{
		printf("init_desktop_dup failed at line %d, hr = 0x%x\n", __LINE__, hr);
		return false;
	}

	if (device_->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0)
	{
		printf("init_desktop_dup failed at line %d, feature level < 11.0\n", __LINE__);

		device_ = nullptr;
		ctx_ = nullptr;

		return false;
	}

	return true;
}

void bitblt_hdr::enum_monitors()
{
	if (monitors_.size())
	{
		monitors_.clear();
	}

	com_ptr<IDXGIDevice> dxgi_device = device_.as<IDXGIDevice>();

	if (!dxgi_device)
	{
		throw std::runtime_error{"enum_monitors failed to get as IDXGIDevice"};
	}

	com_ptr<IDXGIAdapter> adapter;
	HRESULT hr = dxgi_device->GetAdapter(adapter);

	if (FAILED(hr))
	{
		auto msg = std::format("enum_monitors failed to GetAdapter: {:x}", hr);
		throw std::runtime_error{msg};
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
			throw std::runtime_error{msg};
		}

		com_ptr<IDXGIOutput6> output6 = output.as<IDXGIOutput6>();
		if (!output6)
		{
			throw std::runtime_error{"enum_monitors failed to get as IDXGIOutput6"};
		}

		DXGI_OUTPUT_DESC1 desc;
		hr = output6->GetDesc1(&desc);

		if (FAILED(hr))
		{
			printf("enum_monitors failed to GetDesc1: %x", hr);
			continue;
		}

		if (desc.AttachedToDesktop)
		{
			monitors_.push_back(std::make_unique<monitor>(output6, device_));
			continue;
		}
	}
}

bool bitblt_hdr::create_shader_from_source_file(LPCWSTR file_name)
{
	if (!device_)
	{
		return false;
	}

	if (render_cs_)
	{
		render_cs_ = nullptr;
	}

	com_ptr<ID3DBlob> shader;
	com_ptr<ID3DBlob> error;

	HRESULT hr = D3DCompileFromFile(file_name,
					nullptr,
					D3D_COMPILE_STANDARD_FILE_INCLUDE,
					"main",
					"cs_5_0",
					D3DCOMPILE_ENABLE_STRICTNESS,
					0,
					shader,
					error);

	if (error)
	{
		printf("create_shader_from_source_file: %s\n",
		 reinterpret_cast<const char*>(error->GetBufferPointer()));
	}

	if (FAILED(hr))
	{
		printf("create_shader_from_source_file failed at line %d, hr = 0x%x\n", __LINE__, hr);
		return false;
	}

	hr = device_->CreateComputeShader(
			shader->GetBufferPointer(), shader->GetBufferSize(), nullptr, render_cs_);

	if (FAILED(hr))
	{
		printf("create_shader_from_source_file failed at line %d, hr = 0x%x\n", __LINE__, hr);
		return false;
	}
	return true;
}

bool bitblt_hdr::create_shader_from_resource(HINSTANCE instance, WORD res_id)
{
	if (!device_)
	{
		return false;
	}

	if (render_cs_)
	{
		render_cs_ = nullptr;
	}

	auto* const res = FindResource(instance, MAKEINTRESOURCE(res_id), RT_RCDATA);
	if (!res)
	{
		printf("shader resource not found\n");
		return {};
	}

	auto* const handle = LoadResource(instance, res);
	if (!handle)
	{
		printf("shader failed to load resource\n");
		return {};
	}

	const auto* bytecode = LockResource(handle);
	const auto size = SizeofResource(instance, res);

	FreeResource(handle);

	HRESULT hr = device_->CreateComputeShader(bytecode, size, nullptr, render_cs_);

	if (FAILED(hr))
	{
		printf("create_shader_from_resource failed at line %d, hr = 0x%x\n", __LINE__, hr);
		return false;
	}
	return true;
}

bool bitblt_hdr::render(com_ptr<ID3D11Texture2D> input, com_ptr<ID3D11Texture2D> target)
{
	D3D11_TEXTURE2D_DESC desc;
	input->GetDesc(&desc);

	render_cb_data_.is_hdr = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;

	com_ptr<ID3D11ShaderResourceView> src_srv;
	D3D11_SHADER_RESOURCE_VIEW_DESC src_desc = {};
	src_desc.Format = desc.Format;
	src_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	src_desc.Texture2D.MipLevels = 1;
	HRESULT hr = device_->CreateShaderResourceView(input, &src_desc, src_srv);

	if (FAILED(hr))
	{
		return false;
	}

	com_ptr<ID3D11UnorderedAccessView> dest_uav;
	D3D11_UNORDERED_ACCESS_VIEW_DESC dest_desc = {};
	dest_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	dest_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
	dest_desc.Texture2D.MipSlice = 0;
	hr = device_->CreateUnorderedAccessView(target, &dest_desc, dest_uav);

	if (FAILED(hr))
	{
		return false;
	}

	if (!render_const_buffer_)
	{
		D3D11_BUFFER_DESC cb_desc;
		cb_desc.ByteWidth = sizeof(render_constant_buffer_t);
		cb_desc.Usage = D3D11_USAGE_DYNAMIC;
		cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		cb_desc.MiscFlags = 0;
		cb_desc.StructureByteStride = 0;

		hr = device_->CreateBuffer(&cb_desc, nullptr, render_const_buffer_);

		if (FAILED(hr))
		{
			return false;
		}

		ctx_->CSSetConstantBuffers(0, 1, render_const_buffer_);
	}

	D3D11_MAPPED_SUBRESOURCE mapped_cb;
	ctx_->Map(render_const_buffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_cb);
	memcpy(mapped_cb.pData, &render_cb_data_, sizeof(render_constant_buffer_t));
	ctx_->Unmap(render_const_buffer_, 0);

	ctx_->CSSetShader(render_cs_, nullptr, 0);
	ctx_->CSSetShaderResources(0, 1, src_srv);
	ctx_->CSSetUnorderedAccessViews(0, 1, dest_uav, nullptr);
	ctx_->Dispatch((desc.Width + 15) / 16, (desc.Height + 15) / 16, 1);

	ctx_->CSSetShader(nullptr, nullptr, 0);

	src_srv = nullptr;
	ctx_->CSSetShaderResources(0, 1, src_srv);

	dest_uav = nullptr;
	ctx_->CSSetUnorderedAccessViews(0, 1, dest_uav, nullptr);

	return true;
}

void bitblt_hdr::capture_frame(
		std::vector<uint8_t>& buffer, int width, int height, int origin_x, int origin_y)
{
	HRESULT hr = S_OK;

	if (width != width_ || height != height_)
	{
		if (virtual_desktop_tex_)
		{
			virtual_desktop_tex_ = nullptr;
		}

		enum_monitors();

		width_ = width;
		height_ = height;
	}

	if (!virtual_desktop_tex_)
	{
		D3D11_TEXTURE2D_DESC desc;
		desc.Width = width_;
		desc.Height = height_;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		desc.MiscFlags = 0;
		desc.CPUAccessFlags = 0;

		hr = device_->CreateTexture2D(&desc, nullptr, virtual_desktop_tex_);
		if (FAILED(hr))
		{
			auto msg = std::format("failed to create virtual desktop texture: {:x}", hr);
			throw std::runtime_error{msg};
		}
	}

	for (const auto& monitor : monitors_)
	{
		monitor->update_output_desc();
		const auto [x, y] = monitor->virtual_position();
		const auto rotation = monitor->rotation();
		const auto rad = rotation * (std::numbers::pi_v<float> / 180.f);

		const auto sin_r = std::sinf(rad);
		const auto cos_r = std::cosf(rad);

		/*
		 *      cos(θ) -sin(θ) Tx
		 * Mt = sin(θ)  cos(θ) Ty
		 *      0       0      1
		 */
		render_cb_data_.transform_matrix[0][0] = cos_r;
		render_cb_data_.transform_matrix[0][1] = -sin_r;
		render_cb_data_.transform_matrix[0][2] = static_cast<float>(x - origin_x);

		render_cb_data_.transform_matrix[1][0] = sin_r;
		render_cb_data_.transform_matrix[1][1] = cos_r;
		render_cb_data_.transform_matrix[1][2] = static_cast<float>(y - origin_y);

		render_cb_data_.transform_matrix[2][0] = 0;
		render_cb_data_.transform_matrix[2][1] = 0;
		render_cb_data_.transform_matrix[2][2] = 1;

		printf("transform matrix: \n%.6f %.6f %.6f\n%.6f %.6f %.6f\n%.6f %.6f %.6f\n",
		 render_cb_data_.transform_matrix[0][0],
		 render_cb_data_.transform_matrix[0][1],
		 render_cb_data_.transform_matrix[0][2],
		 render_cb_data_.transform_matrix[1][0],
		 render_cb_data_.transform_matrix[1][1],
		 render_cb_data_.transform_matrix[1][2],
		 render_cb_data_.transform_matrix[2][0],
		 render_cb_data_.transform_matrix[2][1],
		 render_cb_data_.transform_matrix[2][2]);

		render_cb_data_.white_level = monitor->sdr_white_level();

		auto screenshot = monitor->take_screenshot();
		if (!render(screenshot, virtual_desktop_tex_)) [[unlikely]]
		{
			auto name = monitor->name();
			printf("failed to render monitor %s to virtual desktop texture\n", name.data());
		}
	}

	D3D11_TEXTURE2D_DESC staging_desc;
	virtual_desktop_tex_->GetDesc(&staging_desc);
	staging_desc.Usage = D3D11_USAGE_STAGING;
	staging_desc.BindFlags = 0;
	staging_desc.MiscFlags = 0;
	staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

	com_ptr<ID3D11Texture2D> staging_tex;
	hr = device_->CreateTexture2D(&staging_desc, nullptr, staging_tex);
	if (FAILED(hr))
	{
		auto msg = std::format("failed to create staging texture: {:x}", hr);
		throw std::runtime_error{msg};
	}

	ctx_->CopyResource(staging_tex, virtual_desktop_tex_);

	D3D11_MAPPED_SUBRESOURCE mapped;
	ctx_->Map(staging_tex, 0, D3D11_MAP_READ, 0, &mapped);

	buffer.resize(staging_desc.Width * staging_desc.Height * 4);

	for (size_t i = 0; i < staging_desc.Height; i++)
	{
		const auto* src = reinterpret_cast<uint8_t*>(mapped.pData) + mapped.RowPitch * i;
		auto* dest = buffer.data() + (staging_desc.Width * 4 * i);

		std::memcpy(dest, src, staging_desc.Width * 4);
	}

	ctx_->Unmap(staging_tex, 0);
}

bool bitblt_hdr::is_ready() const {
	return device_ && ctx_ && render_cs_;
}

bool bitblt_hdr::bitblt(
		HDC hdc, int x, int y, int cx, int cy, HDC hdcSrc, int x1, int y1, DWORD rop)
{
	if (!is_ready())
	{
		return bitblt_(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);
	}

	auto src_window = WindowFromDC(hdcSrc);
	auto desktop_window = GetDesktopWindow();

	if (src_window != desktop_window)
	{
		return bitblt_(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);
	}

	std::vector<uint8_t> buffer;

	try
	{
		capture_frame(buffer, cx, cy, x1, y1);
	}
	catch (std::runtime_error e)
	{
		printf("failed to capture_frame, error: \n%s\n", e.what());
		return bitblt_(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);
	}

	HBITMAP map = CreateBitmap(cx, cy, 1, 32, buffer.data());
	HDC src = CreateCompatibleDC(hdc);
	SelectObject(src, map);

	auto result = bitblt_(hdc, x, y, cx, cy, src, 0, 0, rop & ~CAPTUREBLT);

	DeleteDC(src);
	DeleteObject(map);

	return result;
}
