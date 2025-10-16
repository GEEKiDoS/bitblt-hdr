#pragma once

#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <tuple>

#include "monitor.hpp"
#include "utils/com_ptr.hpp"
#include "utils/trampoline.hpp"

using vec2_t = std::tuple<int, int>;

class bitblt_hdr
{
 public:
  explicit bitblt_hdr(trampoline<decltype(BitBlt)> bitblt);
  ~bitblt_hdr();

  bool create_shader_from_source_file(LPCWSTR file_name);
  bool create_shader_from_resource(HINSTANCE instance, WORD res_id);
  bool is_ready() const;
  bool bitblt(HDC hdc, int x, int y, int cx, int cy, HDC hdcSrc, int x1, int y1, DWORD rop);

 private:
  bool init_desktop_dup();
  void enum_monitors();
  bool render(com_ptr<ID3D11Texture2D> input, com_ptr<ID3D11Texture2D> target);
  void capture_frame(
      std::vector<uint8_t>& buffer, int width, int height, int origin_x, int origin_y);

  com_ptr<ID3D11Device> device_;
  com_ptr<ID3D11DeviceContext> ctx_;
  com_ptr<ID3D11ComputeShader> render_cs_;
  com_ptr<ID3D11Texture2D> virtual_desktop_tex_;
  com_ptr<ID3D11Buffer> render_const_buffer_;

  int width_ = 0;
  int height_ = 0;

  struct render_constant_buffer_t
  {
    float white_level = 200.0f;
    uint32_t is_hdr = 0;

    float __gap[2];

    float transform_matrix[3][4];
  } render_cb_data_;

  std::vector<std::unique_ptr<monitor>> monitors_;
  trampoline<decltype(BitBlt)> bitblt_;
};
