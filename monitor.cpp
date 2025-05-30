#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <format>
#include <vector>

#include "monitor.hpp"

// https://chromium.googlesource.com/chromium/src/+/c71f15ab1ace78c7efeeeda9f8552b4af9db2877/ui/display/win/screen_win.cc#112
bool get_path_info(HMONITOR monitor, DISPLAYCONFIG_PATH_INFO* path_info)
{
	LONG result;
	uint32_t num_path_array_elements = 0;
	uint32_t num_mode_info_array_elements = 0;
	std::vector<DISPLAYCONFIG_PATH_INFO> path_infos;
	std::vector<DISPLAYCONFIG_MODE_INFO> mode_infos;

	// Get the monitor name.
	MONITORINFOEXW view_info;
	view_info.cbSize = sizeof(view_info);

	if (!GetMonitorInfoW(monitor, &view_info))
		return false;

	// Get all path infos.
	do
	{
		if (GetDisplayConfigBufferSizes(
			QDC_ONLY_ACTIVE_PATHS, &num_path_array_elements,
			&num_mode_info_array_elements) != ERROR_SUCCESS)
		{
			return false;
		}
		path_infos.resize(num_path_array_elements);
		mode_infos.resize(num_mode_info_array_elements);
		result = QueryDisplayConfig(
			QDC_ONLY_ACTIVE_PATHS, &num_path_array_elements, path_infos.data(),
			&num_mode_info_array_elements, mode_infos.data(), nullptr);
	} while (result == ERROR_INSUFFICIENT_BUFFER);

	// Iterate of the path infos and see if we find one with a matching name.
	if (result == ERROR_SUCCESS)
	{
		for (uint32_t p = 0; p < num_path_array_elements; p++)
		{
			DISPLAYCONFIG_SOURCE_DEVICE_NAME device_name;
			device_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
			device_name.header.size = sizeof(device_name);
			device_name.header.adapterId = path_infos[p].sourceInfo.adapterId;
			device_name.header.id = path_infos[p].sourceInfo.id;
			if (DisplayConfigGetDeviceInfo(&device_name.header) == ERROR_SUCCESS)
			{
				if (wcscmp(view_info.szDevice, device_name.viewGdiDeviceName) == 0)
				{
					*path_info = path_infos[p];
					return true;
				}
			}
		}
	}

	return false;
}

monitor::monitor(com_ptr<IDXGIOutput6> output, com_ptr<ID3D11Device> device) :
	output_(output), device_(device)
{
	memset(&desc_, 0, sizeof(DXGI_OUTPUT_DESC1));
}

monitor::~monitor()
{
	last_tex_ = nullptr;
	dup_ = nullptr;
	output_ = nullptr;
	device_ = nullptr;
}

std::string monitor::name()
{
	if (name_.size())
		return name_;

	auto size = WideCharToMultiByte(932, 0, desc_.DeviceName, -1, nullptr, 0, nullptr, nullptr);
	size--;
	name_.resize(size);

	WideCharToMultiByte(932, 0, desc_.DeviceName, -1, name_.data(), size, nullptr, nullptr);
	return name_;
}

bool monitor::hdr_on() const
{
	return desc_.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
}

vec2_t monitor::virtual_position() const
{
	const auto& coords = desc_.DesktopCoordinates;

	return { coords.left, coords.top };
}

float monitor::rotation() const
{
	switch (desc_.Rotation)
	{
	case DXGI_MODE_ROTATION_ROTATE90:
		return 90.f;
	case DXGI_MODE_ROTATION_ROTATE180:
		return 180.f;
	case DXGI_MODE_ROTATION_ROTATE270:
		return 270.f;
	default:
		return 0.f;
	}
}

vec2_t monitor::resolution() const
{
	const auto& coords = desc_.DesktopCoordinates;
	auto w = coords.right - coords.left;
	auto h = coords.bottom - coords.top;

	return { w, h };
}

float monitor::sdr_white_level() const
{
	const float default_white_level = 200.0f;

	DISPLAYCONFIG_PATH_INFO path_info = {};
	if (!get_path_info(desc_.Monitor, &path_info))
		return default_white_level; // default

	DISPLAYCONFIG_SDR_WHITE_LEVEL white_level = {};
	white_level.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
	white_level.header.size = sizeof(white_level);
	white_level.header.adapterId = path_info.targetInfo.adapterId;
	white_level.header.id = path_info.targetInfo.id;
	if (DisplayConfigGetDeviceInfo(&white_level.header) != ERROR_SUCCESS)
		return default_white_level;

	return white_level.SDRWhiteLevel * 80.0f / 1000.0f;
}

com_ptr<ID3D11Texture2D> monitor::take_screenshot()
{
	if (!dup_) recreate_output_duplication();

	if (last_tex_)
	{
		last_tex_ = nullptr;
	}

	DXGI_OUTDUPL_FRAME_INFO frame_info{ 0 };
	com_ptr<IDXGIResource> resource;

	HRESULT hr = S_OK;
	while (!frame_info.LastPresentTime.QuadPart)
	{
		hr = dup_->AcquireNextFrame(0, &frame_info, resource);

		if (hr == DXGI_ERROR_INVALID_CALL) [[likely]]
		{
			hr = dup_->ReleaseFrame();

			if (hr == DXGI_ERROR_ACCESS_LOST) [[unlikely]]
			{
				recreate_output_duplication();
				continue;
			}

			if (FAILED(hr)) [[unlikely]]
			{
				auto msg = std::format("failed to release frame on monitor {}: {:x}", name(), hr);
				throw std::runtime_error{ msg };
			}

			continue;
		}

		if (hr == DXGI_ERROR_ACCESS_LOST) [[unlikely]]
		{
			recreate_output_duplication();
			continue;
		}

		if (hr == DXGI_ERROR_WAIT_TIMEOUT)
		{
			Sleep(20);
			continue;
		}

		if (FAILED(hr)) [[unlikely]]
		{
			auto msg = std::format("failed to acquire next frame on monitor {}: {:x}", name(), hr);
			throw std::runtime_error{ msg };
		}
	}

	com_ptr<ID3D11Texture2D> tex = resource.as<ID3D11Texture2D>();

	if (!tex)
	{
		auto msg = std::format("failed to get texture from resource on monitor {}", name());
		throw std::runtime_error{ msg };
	}

	last_tex_ = tex;
	return tex;
}

void monitor::recreate_output_duplication()
{
	if (dup_)
	{
		dup_ = nullptr;
	}

	const DXGI_FORMAT formats[] = 
	{
		DXGI_FORMAT_R8G8B8A8_UNORM,
		DXGI_FORMAT_R16G16B16A16_FLOAT,
	};

	auto hr = output_->DuplicateOutput1(device_, 0, 2, formats, dup_);

	if (FAILED(hr))
	{
		auto msg = std::format("recreate_output_duplication DuplicateOutput1 failed on monitor {}: {:X}", name(), static_cast<unsigned long>(hr));
		throw std::runtime_error{ msg };
	}

	update_output_desc();
}

void monitor::update_output_desc()
{
	auto hr = output_->GetDesc1(&desc_);

	if (FAILED(hr))
	{
		auto msg = std::format("update_output_desc GetDesc1 failed on monitor {}: {:X}", name(), static_cast<unsigned long>(hr));
		throw std::runtime_error{ msg };
	}
}
