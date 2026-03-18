#pragma once

#include <vector>
#include <string>
#include <cstring>

namespace microStore {

template<typename T>
struct Codec
{
	static std::vector<uint8_t> encode(const T& v)
	{
		const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&v);
		return std::vector<uint8_t>(ptr, ptr + sizeof(T));
	}
	static bool decode(const std::vector<uint8_t>& data, T& out)
	{
		if(data.size() != sizeof(T)) return false;
		memcpy(&out, data.data(), sizeof(T));
		return true;
	}
};

template<>
struct Codec<char*>
{
	static std::vector<uint8_t> encode(const char* s)
	{
		return std::vector<uint8_t>(s, s + strlen(s));
	}
	static bool decode(const std::vector<uint8_t>& data, char* s, size_t len)
	{
		memcpy(s, data.data(), std::min(data.size(), len-1));
		s[std::min(data.size(), len-1)] = 0;
		return true;
	}
/*
	static bool decode(const std::vector<uint8_t>& data, char* s)
	{
		memcpy(s, data.data(), data.size());
		s[data.size()] = 0;
		return true;
	}
*/
};

template<>
struct Codec<std::string>
{
	static std::vector<uint8_t> encode(const std::string& s)
	{
		return std::vector<uint8_t>(s.begin(),s.end());
	}
	static bool decode(const std::vector<uint8_t>& data,std::string& out)
	{
		out.assign((const char*)data.data(), data.size());
		return true;
	}
};

template<>
struct Codec<std::vector<uint8_t>>
{
	static std::vector<uint8_t> encode(const std::vector<uint8_t>& vec)
	{
		return std::vector<uint8_t>(vec.begin(), vec.end());
	}
	static bool decode(const std::vector<uint8_t>& data, std::vector<uint8_t>& out)
	{
		out.assign(data.begin(), data.end());
		return true;
	}
};

}
