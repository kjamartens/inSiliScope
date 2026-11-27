// SPDX-License-Identifier: BSD-3-Clause
// A small JSON reader (RFC 8259; numbers as doubles, strings with their escapes, \u as UTF-8): the settings files'
// (settings_file.cpp) and the remote server's messages (remote_server.cpp).
#pragma once

#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace jsonlite {

struct Json
{
   enum Type { Null, Bool, Number, String, Array, Object } type = Null;
   bool b = false;
   double num = 0;
   std::string str;
   std::vector<Json> arr;
   std::vector<std::pair<std::string, Json>> obj;   // in file order
   const Json* Get(const char* key) const
   {
      for (const auto& kv : obj)
         if (kv.first == key)
            return &kv.second;
      return nullptr;
   }
};

struct Parser
{
   const std::string& s;
   size_t i = 0;
   std::string err;
   void Ws()
   {
      while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
         i++;
   }
   bool Fail(const char* what)
   {
      if (err.empty())
         err = std::string(what) + " at byte " + std::to_string(i);
      return false;
   }
   bool Str(std::string& out)
   {
      if (i >= s.size() || s[i] != '"')
         return Fail("a string expected");
      i++;
      out.clear();
      while (i < s.size() && s[i] != '"')
      {
         char c = s[i++];
         if (c != '\\')
         {
            out += c;
            continue;
         }
         if (i >= s.size())
            return Fail("a bad escape");
         c = s[i++];
         switch (c)
         {
         case 'n': out += '\n'; break;
         case 't': out += '\t'; break;
         case 'r': out += '\r'; break;
         case 'b': out += '\b'; break;
         case 'f': out += '\f'; break;
         case 'u':
         {
            if (i + 4 > s.size())
               return Fail("a bad \\u escape");
            const unsigned cp = static_cast<unsigned>(std::strtoul(s.substr(i, 4).c_str(), nullptr, 16));
            i += 4;
            if (cp < 0x80) out += static_cast<char>(cp);
            else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
            else { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
            break;
         }
         default: out += c;   // \" \\ \/
         }
      }
      if (i >= s.size())
         return Fail("an unterminated string");
      i++;
      return true;
   }
   bool Value(Json& v, int depth)
   {
      if (depth > 64)
         return Fail("too deep");
      Ws();
      if (i >= s.size())
         return Fail("a value expected");
      const char c = s[i];
      if (c == '{')
      {
         v.type = Json::Object;
         i++;
         Ws();
         if (i < s.size() && s[i] == '}') { i++; return true; }
         for (;;)
         {
            Ws();
            std::string k;
            if (!Str(k)) return false;
            Ws();
            if (i >= s.size() || s[i] != ':') return Fail("':' expected");
            i++;
            v.obj.emplace_back(std::move(k), Json());
            if (!Value(v.obj.back().second, depth + 1)) return false;
            Ws();
            if (i < s.size() && s[i] == ',') { i++; continue; }
            if (i < s.size() && s[i] == '}') { i++; return true; }
            return Fail("',' or '}' expected");
         }
      }
      if (c == '[')
      {
         v.type = Json::Array;
         i++;
         Ws();
         if (i < s.size() && s[i] == ']') { i++; return true; }
         for (;;)
         {
            v.arr.emplace_back();
            if (!Value(v.arr.back(), depth + 1)) return false;
            Ws();
            if (i < s.size() && s[i] == ',') { i++; continue; }
            if (i < s.size() && s[i] == ']') { i++; return true; }
            return Fail("',' or ']' expected");
         }
      }
      if (c == '"')
      {
         v.type = Json::String;
         return Str(v.str);
      }
      auto word = [&](const char* w) { const size_t n = std::strlen(w); if (s.compare(i, n, w) != 0) return false; i += n; return true; };
      if (word("true")) { v.type = Json::Bool; v.b = true; return true; }
      if (word("false")) { v.type = Json::Bool; v.b = false; return true; }
      if (word("null")) { v.type = Json::Null; return true; }
      char* end = nullptr;
      v.num = std::strtod(s.c_str() + i, &end);
      if (end == s.c_str() + i)
         return Fail("a value expected");
      v.type = Json::Number;
      i = static_cast<size_t>(end - s.c_str());
      return true;
   }
};

} // namespace jsonlite
