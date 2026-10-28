// SPDX-License-Identifier: BSD-3-Clause
// Settings files (settings_file.h): a small JSON reader for their shape and the light history's replay.
#include "settings_file.h"

#include "insiliscope/version.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <utility>

namespace settings {

namespace {

// ---- JSON (RFC 8259; numbers as doubles, strings with their escapes, \u as UTF-8) ----
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

// A value of a file (a number, a name, true/false) as the option's number; false for one the engine cannot take.
bool OptionNumber(const std::string& key, const Json& v, double& out)
{
   if (v.type == Json::Number) { out = v.num; return true; }
   if (v.type == Json::Bool) { out = v.b ? 1 : 0; return true; }
   if (v.type == Json::String) return sim::ScopeOptionValue(key, v.str.c_str(), out);
   return false;
}

bool IsPart(const std::string& p)
{
   for (const std::string& n : PartNames())
      if (n == p)
         return true;
   return false;
}

} // namespace

const std::vector<std::string>& PartNames()
{
   static const std::vector<std::string> v = { "instrument", "settings", "sample", "measurement", "view" };
   return v;
}

void ParseArg(const std::string& arg, std::string& path, std::vector<std::string>& parts)
{
   path = arg;
   parts.clear();
   const size_t colon = arg.rfind(':');
   if (colon == std::string::npos || colon + 1 >= arg.size())
      return;
   std::vector<std::string> list;
   std::stringstream ss(arg.substr(colon + 1));
   std::string p;
   while (std::getline(ss, p, ','))
   {
      if (!IsPart(p))
         return;   // not a part list: the colon is the path's (C:\...)
      list.push_back(p);
   }
   path = arg.substr(0, colon);
   parts = list;
}

bool ReadText(const std::string& text, const std::vector<std::string>& partsIn, Loaded& out, std::string& err)
{
   Json root;
   Parser ps{ text };
   if (!ps.Value(root, 0))
   {
      err = "not JSON: " + ps.err;
      return false;
   }
   const Json* fmt = root.Get("format");
   if (root.type != Json::Object || !fmt || fmt->type != Json::String || fmt->str != "insiliscope-settings")
   {
      err = "not an inSiliScope settings file (format insiliscope-settings)";
      return false;
   }
   const Json* ver = root.Get("version");
   if (ver && ver->type == Json::Number && ver->num > 1)
      out.warnings.push_back("a newer file format (version " + std::to_string(static_cast<int>(ver->num)) + "): what this release knows of it is read");
   if (const Json* made = root.Get("made"))
      if (const Json* rel = made->Get("release"))
         if (rel->type == Json::String && rel->str != ISC_VERSION)
            out.warnings.push_back("made with inSiliScope " + rel->str + ", this is " ISC_VERSION ": the same settings, the image may differ");
   const Json* parts = root.Get("parts");
   if (!parts || parts->type != Json::Object)
   {
      err = "the file has no parts";
      return false;
   }
   std::vector<std::string> want = partsIn;
   if (want.empty())
      for (const auto& kv : parts->obj)
         if (kv.first != "view" && IsPart(kv.first))
            want.push_back(kv.first);
   for (const std::string& name : want)
   {
      const Json* part = parts->Get(name.c_str());
      if (!part || part->type != Json::Object)
      {
         err = "the file has no " + name + " part";
         return false;
      }
      if (name == "view")
         continue;   // the viewer's own (not engine options)
      const Json* values = part->Get("values");
      if (values && values->type == Json::Object)
         for (const auto& kv : values->obj)
         {
            double v = 0;
            if (kv.second.type == Json::Null)
               continue;
            if (!OptionNumber(kv.first, kv.second, v) || !sim::ScopeSpecSet(out.spec, kv.first, v))
               out.warnings.push_back(name + "." + kv.first + ": not an option of this engine (left out)");
            else if (sim::ScopeOptionPart(kv.first) && name != sim::ScopeOptionPart(kv.first))
               out.warnings.push_back(name + "." + kv.first + ": belongs to the " + sim::ScopeOptionPart(kv.first) + " part here");
         }
      if (name != "measurement")
         continue;
      out.haveMeasurement = true;
      out.history.clear();
      const Json* hist = part->Get("history");
      const Json* recs = hist ? hist->Get("records") : nullptr;
      if (!recs || recs->type != Json::Array)
         continue;
      sim::ScopeSpec cur;   // the records hold the changes from the one before (null: a key the spec no longer has)
      for (const Json& r : recs->arr)
      {
         Record rec;
         if (const Json* n = r.Get("n")) rec.n = std::max(1L, static_cast<long>(n->num));
         if (const Json* lit = r.Get("lit")) rec.lit = lit->type != Json::Bool || lit->b;
         if (const Json* set = r.Get("set"))
            for (const auto& kv : set->obj)
            {
               double v = 0;
               if (kv.second.type == Json::Null) { cur.erase(kv.first); continue; }
               if (!OptionNumber(kv.first, kv.second, v) || !sim::ScopeSpecSet(cur, kv.first, v))
               {
                  err = "history: '" + kv.first + "' is not an option of this engine";
                  return false;
               }
            }
         rec.spec = cur;
         out.history.push_back(std::move(rec));
      }
   }
   return true;
}

bool Read(const std::string& path, const std::vector<std::string>& parts, Loaded& out, std::string& err)
{
   std::ifstream f(path, std::ios::binary);
   if (!f)
   {
      err = "cannot read " + path;
      return false;
   }
   std::stringstream text;
   text << f.rdbuf();
   return ReadText(text.str(), parts, out, err);
}

bool Replay(const Loaded& s, sim::LiveScope& live, std::string& err)
{
   live.Reset();
   for (const Record& r : s.history)
   {
      sim::ScopeSpec spec = r.spec;
      for (const auto& kv : s.spec)
      {
         const char* part = sim::ScopeOptionPart(kv.first);
         if (part && !std::strcmp(part, "sample"))
            spec[kv.first] = kv.second;
      }
      if (!live.Advance(spec, r.n, r.lit, err))
         return false;
   }
   return true;
}

} // namespace settings
