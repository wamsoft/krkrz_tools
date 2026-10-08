// 形式ごとの読み書き (Image.cpp から振り分ける)
#pragma once
#include "krt/image/Image.h"

namespace krt::image::detail {

bool loadBmp(const uint8_t* data, size_t size, Image& out, LoadInfo& info, std::string& error);
bool saveBmp(std::vector<uint8_t>& out, const Image& img, const SaveOptions& opt, std::string& error);

bool loadPng(const uint8_t* data, size_t size, Image& out, LoadInfo& info, std::string& error);
bool savePng(std::vector<uint8_t>& out, const Image& img, const SaveOptions& opt, std::string& error);

bool loadJpeg(const uint8_t* data, size_t size, Image& out, LoadInfo& info, std::string& error);
bool saveJpeg(std::vector<uint8_t>& out, const Image& img, const SaveOptions& opt, std::string& error);

bool loadPsd(const uint8_t* data, size_t size, Image& out, LoadInfo& info, std::string& error);
bool loadClip(const uint8_t* data, size_t size, Image& out, LoadInfo& info, std::string& error);

} // namespace krt::image::detail
